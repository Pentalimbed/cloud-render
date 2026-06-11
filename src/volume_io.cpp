#include "volume_io.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/NanoToOpenVDB.h>
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/FastSweeping.h>
#include <openvdb/tools/Interpolation.h>

namespace cloud_render {
namespace {

std::string lowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

struct Bounds {
    Vec3 min;
    Vec3 max;
};

struct VolumePlacement {
    Vec3 nativeWorldMin;
    Vec3 nativeWorldMax;
    Vec3 worldMin;
    Vec3 worldMax;
    Matrix4 nativeToRender = identityMatrix4();
    int nativeUpAxis = 2;
};

struct DenseBounds {
    openvdb::Coord min;
    openvdb::Coord max;
    std::array<uint32_t, 3> size = {};
    size_t voxelCount = 0;
};

struct SignedDistanceGrids {
    openvdb::FloatGrid::Ptr profileGrid;
    openvdb::FloatGrid::Ptr fullGrid;
    float maxDistanceToZero = 0.0f;
};

template <typename GridT>
Vec3 toVec3(const GridT& v)
{
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

void includePoint(Bounds& bounds, Vec3 point)
{
    bounds.min.x = std::min(bounds.min.x, point.x);
    bounds.min.y = std::min(bounds.min.y, point.y);
    bounds.min.z = std::min(bounds.min.z, point.z);
    bounds.max.x = std::max(bounds.max.x, point.x);
    bounds.max.y = std::max(bounds.max.y, point.y);
    bounds.max.z = std::max(bounds.max.z, point.z);
}

openvdb::math::Mat4d toOpenVdbMatrix(const Matrix4& matrix)
{
    openvdb::math::Mat4d result = openvdb::math::Mat4d::zero();
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            result[row][column] = matrix.m[row][column];
        }
    }
    return result;
}

Bounds uniformScaleTranslateActiveWorldBounds(
    const openvdb::math::UniformScaleTranslateMap& map,
    const openvdb::CoordBBox& activeBBox)
{
    const openvdb::Coord minCoord = activeBBox.min();
    const openvdb::Coord maxCoord = activeBBox.max();
    const openvdb::Vec3d indexMin(
        static_cast<double>(minCoord.x()) - 0.5,
        static_cast<double>(minCoord.y()) - 0.5,
        static_cast<double>(minCoord.z()) - 0.5);
    const openvdb::Vec3d indexMax(
        static_cast<double>(maxCoord.x()) + 0.5,
        static_cast<double>(maxCoord.y()) + 0.5,
        static_cast<double>(maxCoord.z()) + 0.5);

    const Vec3 first = toVec3(map.applyMap(indexMin));
    Bounds bounds{first, first};
    const double xs[] = {indexMin.x(), indexMax.x()};
    const double ys[] = {indexMin.y(), indexMax.y()};
    const double zs[] = {indexMin.z(), indexMax.z()};

    for (double x : xs) {
        for (double y : ys) {
            for (double z : zs) {
                includePoint(bounds, toVec3(map.applyMap(openvdb::Vec3d(x, y, z))));
            }
        }
    }

    return bounds;
}

Bounds activeWorldBounds(const openvdb::FloatGrid& grid, const openvdb::CoordBBox& activeBBox)
{
    if (const auto map = grid.transform().constMap<openvdb::math::UniformScaleTranslateMap>()) {
        return uniformScaleTranslateActiveWorldBounds(*map, activeBBox);
    }

    openvdb::Coord minCoord = activeBBox.min();
    openvdb::Coord maxCoord = activeBBox.max();
    maxCoord.offsetBy(1);

    const Vec3 first = toVec3(grid.transform().indexToWorld(minCoord));
    Bounds bounds{first, first};
    const int xs[] = {minCoord.x(), maxCoord.x()};
    const int ys[] = {minCoord.y(), maxCoord.y()};
    const int zs[] = {minCoord.z(), maxCoord.z()};

    for (int x : xs) {
        for (int y : ys) {
            for (int z : zs) {
                includePoint(bounds, toVec3(grid.transform().indexToWorld(openvdb::Coord(x, y, z))));
            }
        }
    }

    return bounds;
}

float maxActiveDensity(const openvdb::FloatGrid& grid)
{
    bool hasValue = false;
    float maxDensity = 0.0f;
    for (openvdb::FloatGrid::ValueOnCIter iter = grid.cbeginValueOn(); iter; ++iter) {
        const float density = iter.getValue();
        if (!hasValue || density > maxDensity) {
            maxDensity = density;
            hasValue = true;
        }
    }
    return hasValue ? maxDensity : 0.0f;
}

DenseBounds makeDenseBounds(const openvdb::CoordBBox& activeBBox, int padding = 0)
{
    DenseBounds bounds;
    bounds.min = activeBBox.min();
    bounds.max = activeBBox.max();
    bounds.min.offsetBy(-padding);
    bounds.max.offsetBy(padding);

    const int64_t sizeX = static_cast<int64_t>(bounds.max.x()) - static_cast<int64_t>(bounds.min.x()) + 1;
    const int64_t sizeY = static_cast<int64_t>(bounds.max.y()) - static_cast<int64_t>(bounds.min.y()) + 1;
    const int64_t sizeZ = static_cast<int64_t>(bounds.max.z()) - static_cast<int64_t>(bounds.min.z()) + 1;
    if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0) {
        throw std::runtime_error("Invalid active VDB bounding box");
    }

    constexpr uint64_t kMaxDistanceTransformSamples = 256ull * 1024ull * 1024ull;
    const uint64_t voxelCount = static_cast<uint64_t>(sizeX) * static_cast<uint64_t>(sizeY) * static_cast<uint64_t>(sizeZ);
    if (voxelCount > kMaxDistanceTransformSamples) {
        throw std::runtime_error("VDB active bounds are too large for OpenVDB SDF generation");
    }

    bounds.size = {
        static_cast<uint32_t>(sizeX),
        static_cast<uint32_t>(sizeY),
        static_cast<uint32_t>(sizeZ),
    };
    bounds.voxelCount = static_cast<size_t>(voxelCount);
    return bounds;
}

size_t denseIndex(uint32_t x, uint32_t y, uint32_t z, const std::array<uint32_t, 3>& size)
{
    return (static_cast<size_t>(z) * size[1] + y) * size[0] + x;
}

uint32_t clampedCoarseSize(uint32_t fineSize)
{
    constexpr uint32_t kCoarseDistanceDownsample = 4;
    constexpr uint32_t kMaxCoarseDistanceResolution = 128;
    const uint32_t downsampled = (fineSize + kCoarseDistanceDownsample - 1u) / kCoarseDistanceDownsample;
    return std::clamp(downsampled, 1u, kMaxCoarseDistanceResolution);
}

std::array<uint32_t, 3> makeCoarseDistanceSize(const openvdb::CoordBBox& activeBBox)
{
    const DenseBounds bounds = makeDenseBounds(activeBBox);
    return {
        clampedCoarseSize(bounds.size[0]),
        clampedCoarseSize(bounds.size[1]),
        clampedCoarseSize(bounds.size[2]),
    };
}

uint32_t coarseIndexForWorld(float value, float minValue, float extent, uint32_t size)
{
    if (size <= 1u || extent <= 1.0e-6f) {
        return 0u;
    }
    const float normalized = std::clamp((value - minValue) / extent, 0.0f, 0.99999994f);
    return std::min(static_cast<uint32_t>(normalized * static_cast<float>(size)), size - 1u);
}

openvdb::FloatGrid::Ptr createBinaryFogGrid(const openvdb::FloatGrid& densityGrid, const openvdb::CoordBBox& activeBBox, bool& hasPositiveDensity)
{
    const DenseBounds bounds = makeDenseBounds(activeBBox, 1);
    openvdb::FloatGrid::Ptr fogGrid = openvdb::FloatGrid::create(0.0f);
    fogGrid->setTransform(densityGrid.transform().copy());
    fogGrid->setGridClass(openvdb::GRID_FOG_VOLUME);
    fogGrid->setName("positive_density_mask");

    openvdb::FloatGrid::ConstAccessor densityAccessor = densityGrid.getConstAccessor();
    openvdb::FloatGrid::Accessor fogAccessor = fogGrid->getAccessor();
    hasPositiveDensity = false;

    for (uint32_t z = 0; z < bounds.size[2]; ++z) {
        for (uint32_t y = 0; y < bounds.size[1]; ++y) {
            for (uint32_t x = 0; x < bounds.size[0]; ++x) {
                const openvdb::Coord coord(
                    bounds.min.x() + static_cast<int>(x),
                    bounds.min.y() + static_cast<int>(y),
                    bounds.min.z() + static_cast<int>(z));
                const float occupied = densityAccessor.getValue(coord) > 0.0f ? 1.0f : 0.0f;
                hasPositiveDensity = hasPositiveDensity || occupied > 0.0f;
                fogAccessor.setValueOn(coord, occupied);
            }
        }
    }

    return fogGrid;
}

openvdb::FloatGrid::Ptr createZeroSignedDistanceGrid(const openvdb::FloatGrid& densityGrid)
{
    openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(0.0f);
    grid->setTransform(densityGrid.transform().copy());
    grid->setGridClass(openvdb::GRID_LEVEL_SET);
    const std::string sourceName = densityGrid.getName();
    grid->setName(sourceName.empty() ? "distance_to_zero" : sourceName + "_distance_to_zero");
    return grid;
}

SignedDistanceGrids createSignedDistanceGrids(const openvdb::FloatGrid& densityGrid, const openvdb::CoordBBox& activeBBox)
{
    bool hasPositiveDensity = false;
    openvdb::FloatGrid::Ptr binaryFogGrid = createBinaryFogGrid(densityGrid, activeBBox, hasPositiveDensity);
    openvdb::FloatGrid::Ptr fullSignedDistanceGrid;
    if (hasPositiveDensity) {
        fullSignedDistanceGrid = openvdb::tools::fogToSdf(*binaryFogGrid, 0.5f, 1);
        if (!fullSignedDistanceGrid) {
            throw std::runtime_error("OpenVDB fogToSdf did not produce a signed-distance grid");
        }
        fullSignedDistanceGrid->setTransform(densityGrid.transform().copy());
        fullSignedDistanceGrid->setGridClass(openvdb::GRID_LEVEL_SET);
    } else {
        fullSignedDistanceGrid = createZeroSignedDistanceGrid(densityGrid);
    }

    openvdb::FloatGrid::Ptr profileGrid = createZeroSignedDistanceGrid(densityGrid);
    openvdb::FloatGrid::ConstAccessor sdfAccessor = fullSignedDistanceGrid->getConstAccessor();
    openvdb::FloatGrid::Accessor profileAccessor = profileGrid->getAccessor();

    float maxDistanceToZero = 0.0f;
    for (openvdb::FloatGrid::ValueOnCIter iter = densityGrid.cbeginValueOn(); iter; ++iter) {
        const openvdb::Coord coord = iter.getCoord();
        float signedDistance = 0.0f;
        if (iter.getValue() > 0.0f) {
            signedDistance = sdfAccessor.getValue(coord);
            if (!std::isfinite(signedDistance)) {
                signedDistance = 0.0f;
            }
            signedDistance = std::min(signedDistance, 0.0f);
            maxDistanceToZero = std::max(maxDistanceToZero, -signedDistance);
        }

        profileAccessor.setValueOn(coord, signedDistance);
    }

    SignedDistanceGrids result;
    result.profileGrid = std::move(profileGrid);
    result.fullGrid = std::move(fullSignedDistanceGrid);
    result.maxDistanceToZero = maxDistanceToZero;
    return result;
}

openvdb::Vec3d lerpVec3(Vec3 minValue, Vec3 maxValue, double x, double y, double z)
{
    return {
        static_cast<double>(minValue.x) + (static_cast<double>(maxValue.x) - static_cast<double>(minValue.x)) * x,
        static_cast<double>(minValue.y) + (static_cast<double>(maxValue.y) - static_cast<double>(minValue.y)) * y,
        static_cast<double>(minValue.z) + (static_cast<double>(maxValue.z) - static_cast<double>(minValue.z)) * z,
    };
}

CoarseSignedDistanceVolume createCoarseSignedDistanceVolume(
    const openvdb::FloatGrid& signedDistanceGrid,
    const openvdb::CoordBBox& activeBBox,
    Vec3 worldMin,
    Vec3 worldMax)
{
    CoarseSignedDistanceVolume result;
    result.size = makeCoarseDistanceSize(activeBBox);
    result.values.assign(
        static_cast<size_t>(result.size[0]) * static_cast<size_t>(result.size[1]) * static_cast<size_t>(result.size[2]),
        std::numeric_limits<float>::infinity());

    const Vec3 extent = worldMax - worldMin;
    openvdb::FloatGrid::ConstAccessor sdfAccessor = signedDistanceGrid.getConstAccessor();
    const DenseBounds sourceBounds = makeDenseBounds(activeBBox);
    for (uint32_t z = 0; z < sourceBounds.size[2]; ++z) {
        for (uint32_t y = 0; y < sourceBounds.size[1]; ++y) {
            for (uint32_t x = 0; x < sourceBounds.size[0]; ++x) {
                const openvdb::Coord coord(
                    sourceBounds.min.x() + static_cast<int>(x),
                    sourceBounds.min.y() + static_cast<int>(y),
                    sourceBounds.min.z() + static_cast<int>(z));
                const float signedDistance = sdfAccessor.getValue(coord);
                if (!std::isfinite(signedDistance)) {
                    continue;
                }

                const openvdb::Vec3d world = signedDistanceGrid.transform().indexToWorld(coord);
                const uint32_t cx = coarseIndexForWorld(static_cast<float>(world.x()), worldMin.x, extent.x, result.size[0]);
                const uint32_t cy = coarseIndexForWorld(static_cast<float>(world.y()), worldMin.y, extent.y, result.size[1]);
                const uint32_t cz = coarseIndexForWorld(static_cast<float>(world.z()), worldMin.z, extent.z, result.size[2]);
                float& cell = result.values[denseIndex(cx, cy, cz, result.size)];
                cell = std::min(cell, signedDistance);
            }
        }
    }

    openvdb::tools::GridSampler<openvdb::FloatGrid, openvdb::tools::BoxSampler> sampler(signedDistanceGrid);
    for (uint32_t z = 0; z < result.size[2]; ++z) {
        for (uint32_t y = 0; y < result.size[1]; ++y) {
            for (uint32_t x = 0; x < result.size[0]; ++x) {
                float& cell = result.values[denseIndex(x, y, z, result.size)];
                if (std::isfinite(cell)) {
                    continue;
                }

                const openvdb::Vec3d world = lerpVec3(
                    worldMin,
                    worldMax,
                    (static_cast<double>(x) + 0.5) / static_cast<double>(result.size[0]),
                    (static_cast<double>(y) + 0.5) / static_cast<double>(result.size[1]),
                    (static_cast<double>(z) + 0.5) / static_cast<double>(result.size[2]));
                cell = sampler.wsSample(world);
                if (!std::isfinite(cell)) {
                    cell = 0.0f;
                }
            }
        }
    }

    return result;
}

int shortestAxis(Vec3 extent)
{
    int axis = 0;
    if (extent.y < component(extent, axis)) {
        axis = 1;
    }
    if (extent.z < component(extent, axis)) {
        axis = 2;
    }
    return axis;
}

VolumePlacement makeVolumePlacement(Bounds nativePlacementBounds)
{
    const Vec3 nativeWorldMin = nativePlacementBounds.min;
    const Vec3 nativeWorldMax = nativePlacementBounds.max;
    const Vec3 nativeExtent = nativeWorldMax - nativeWorldMin;
    const int upAxis = shortestAxis(nativeExtent);
    const int renderXAxis = (upAxis + 2) % 3;
    const int renderYAxis = (upAxis + 1) % 3;

    Vec3 nativeOrigin = (nativeWorldMin + nativeWorldMax) * 0.5f;
    setComponent(nativeOrigin, upAxis, component(nativeWorldMin, upAxis));

    const Vec3 renderExtent = {
        component(nativeExtent, renderXAxis),
        component(nativeExtent, renderYAxis),
        component(nativeExtent, upAxis),
    };

    Matrix4 nativeToRender = zeroMatrix4();
    nativeToRender.m[renderXAxis][0] = 1.0f;
    nativeToRender.m[renderYAxis][1] = 1.0f;
    nativeToRender.m[upAxis][2] = 1.0f;
    nativeToRender.m[3][0] = -component(nativeOrigin, renderXAxis);
    nativeToRender.m[3][1] = -component(nativeOrigin, renderYAxis);
    nativeToRender.m[3][2] = -component(nativeOrigin, upAxis);
    nativeToRender.m[3][3] = 1.0f;

    VolumePlacement placement;
    placement.nativeWorldMin = nativeWorldMin;
    placement.nativeWorldMax = nativeWorldMax;
    placement.worldMin = {-renderExtent.x * 0.5f, -renderExtent.y * 0.5f, 0.0f};
    placement.worldMax = {renderExtent.x * 0.5f, renderExtent.y * 0.5f, renderExtent.z};
    placement.nativeToRender = nativeToRender;
    placement.nativeUpAxis = upAxis;
    return placement;
}

void applyVolumePlacement(openvdb::FloatGrid& grid, Volume& volume, const openvdb::CoordBBox& activeBBox)
{
    if (!grid.transform().isLinear()) {
        throw std::runtime_error("Selected VDB grid must have a linear transform");
    }

    const Bounds nativeWorldBounds = activeWorldBounds(grid, activeBBox);
    const VolumePlacement placement = makeVolumePlacement(nativeWorldBounds);
    const auto oldIndexToNative = grid.transform().baseMap()->getAffineMap()->getMat4();
    grid.setTransform(openvdb::math::Transform::createLinearTransform(oldIndexToNative * toOpenVdbMatrix(placement.nativeToRender)));

    volume.nativeWorldMin = placement.nativeWorldMin;
    volume.nativeWorldMax = placement.nativeWorldMax;
    volume.worldMin = placement.worldMin;
    volume.worldMax = placement.worldMax;
    volume.maxDensity = maxActiveDensity(grid);
    volume.nativeUpAxis = placement.nativeUpAxis;
}

void createNanoVolumeHandles(openvdb::FloatGrid& grid, Volume& volume, const openvdb::CoordBBox& activeBBox)
{
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }

    SignedDistanceGrids signedDistance = createSignedDistanceGrids(grid, activeBBox);
    volume.maxDistanceToZero = signedDistance.maxDistanceToZero;
    volume.coarseSignedDistance = createCoarseSignedDistanceVolume(*signedDistance.fullGrid, activeBBox, volume.worldMin, volume.worldMax);
    volume.signedDistanceHandle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*signedDistance.profileGrid);
    if (!volume.signedDistanceHandle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float signed-distance grid");
    }
}

Volume loadNvdb(const std::filesystem::path& path)
{
    auto nanoHandle = nanovdb::io::readGrid(path.string());
    const auto* nanoGrid = nanoHandle.grid<float>();
    if (!nanoGrid) {
        throw std::runtime_error(".nvdb file does not contain a float NanoVDB grid: " + path.string());
    }

    openvdb::GridBase::Ptr base = nanovdb::tools::nanoToOpenVDB(nanoHandle);
    if (!base || !base->isType<openvdb::FloatGrid>()) {
        throw std::runtime_error("NanoVDB to OpenVDB conversion did not produce a float grid: " + path.string());
    }

    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
    if (grid->getGridClass() == openvdb::GRID_UNKNOWN) {
        grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    }

    openvdb::CoordBBox activeBBox;
    if (!grid->tree().evalActiveVoxelBoundingBox(activeBBox)) {
        throw std::runtime_error("Selected NanoVDB grid has no active voxels: " + std::string(nanoGrid->gridName()));
    }

    Volume volume;
    applyVolumePlacement(*grid, volume, activeBBox);
    createNanoVolumeHandles(*grid, volume, activeBBox);
    volume.gridName = nanoGrid->gridName();
    return volume;
}

Volume loadVdb(const std::filesystem::path& path)
{
    openvdb::io::File file(path.string());
    file.open();

    openvdb::GridBase::Ptr selectedBase;
    std::string selectedName;
    for (openvdb::io::File::NameIterator iter = file.beginName(); iter != file.endName(); ++iter) {
        openvdb::GridBase::Ptr base = file.readGrid(iter.gridName());
        if (base && base->isType<openvdb::FloatGrid>()) {
            selectedBase = std::move(base);
            selectedName = iter.gridName();
            break;
        }
    }
    file.close();

    if (!selectedBase) {
        throw std::runtime_error("No float grid found in " + path.string());
    }

    auto grid = openvdb::gridPtrCast<openvdb::FloatGrid>(selectedBase);
    if (grid->getGridClass() == openvdb::GRID_UNKNOWN) {
        grid->setGridClass(openvdb::GRID_FOG_VOLUME);
    }

    openvdb::CoordBBox activeBBox;
    if (!grid->tree().evalActiveVoxelBoundingBox(activeBBox)) {
        throw std::runtime_error("Selected VDB grid has no active voxels: " + selectedName);
    }

    Volume volume;
    applyVolumePlacement(*grid, volume, activeBBox);
    createNanoVolumeHandles(*grid, volume, activeBBox);
    volume.gridName = selectedName;
    return volume;
}

} // namespace

Volume loadVolume(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        throw std::runtime_error("Volume file does not exist: " + path.string());
    }

    const std::string ext = lowerExtension(path);
    if (ext == ".nvdb") {
        return loadNvdb(path);
    }
    if (ext == ".vdb") {
        return loadVdb(path);
    }
    throw std::runtime_error("Expected .vdb or .nvdb file: " + path.string());
}

} // namespace cloud_render
