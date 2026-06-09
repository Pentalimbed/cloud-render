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

DenseBounds makeDenseBounds(const openvdb::CoordBBox& activeBBox)
{
    DenseBounds bounds;
    bounds.min = activeBBox.min();
    bounds.max = activeBBox.max();
    bounds.min.offsetBy(-1);
    bounds.max.offsetBy(1);

    const int64_t sizeX = static_cast<int64_t>(bounds.max.x()) - static_cast<int64_t>(bounds.min.x()) + 1;
    const int64_t sizeY = static_cast<int64_t>(bounds.max.y()) - static_cast<int64_t>(bounds.min.y()) + 1;
    const int64_t sizeZ = static_cast<int64_t>(bounds.max.z()) - static_cast<int64_t>(bounds.min.z()) + 1;
    if (sizeX <= 0 || sizeY <= 0 || sizeZ <= 0) {
        throw std::runtime_error("Invalid active VDB bounding box");
    }

    constexpr uint64_t kMaxDistanceTransformSamples = 256ull * 1024ull * 1024ull;
    const uint64_t voxelCount = static_cast<uint64_t>(sizeX) * static_cast<uint64_t>(sizeY) * static_cast<uint64_t>(sizeZ);
    if (voxelCount > kMaxDistanceTransformSamples) {
        throw std::runtime_error("VDB active bounds are too large for dense distance transform");
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

float axisWorldScale(const openvdb::FloatGrid& grid, int axis)
{
    const openvdb::Vec3d origin = grid.transform().indexToWorld(openvdb::Vec3d(0.0, 0.0, 0.0));
    openvdb::Vec3d unit(0.0, 0.0, 0.0);
    unit[axis] = 1.0;
    const openvdb::Vec3d mapped = grid.transform().indexToWorld(unit);
    const openvdb::Vec3d delta = mapped - origin;
    const double lengthSquared = delta.dot(delta);
    return static_cast<float>(std::sqrt(std::max(lengthSquared, 1.0e-12)));
}

void distanceTransformLine(
    const std::vector<float>& input,
    std::vector<float>& output,
    std::vector<int>& parabolaSites,
    std::vector<double>& boundaries,
    float spacing)
{
    const int count = static_cast<int>(input.size());
    if (count == 0) {
        return;
    }

    const double spacingSquared = static_cast<double>(spacing) * static_cast<double>(spacing);
    int envelopeSize = -1;
    for (int q = 0; q < count; ++q) {
        if (!std::isfinite(input[q])) {
            continue;
        }

        double boundary = -std::numeric_limits<double>::infinity();
        while (envelopeSize >= 0) {
            const int p = parabolaSites[envelopeSize];
            const double qTerm = static_cast<double>(input[q]) + spacingSquared * static_cast<double>(q) * static_cast<double>(q);
            const double pTerm = static_cast<double>(input[p]) + spacingSquared * static_cast<double>(p) * static_cast<double>(p);
            boundary = (qTerm - pTerm) / (2.0 * spacingSquared * static_cast<double>(q - p));
            if (boundary > boundaries[envelopeSize]) {
                break;
            }
            --envelopeSize;
        }

        ++envelopeSize;
        parabolaSites[envelopeSize] = q;
        boundaries[envelopeSize] = envelopeSize == 0 ? -std::numeric_limits<double>::infinity() : boundary;
        boundaries[envelopeSize + 1] = std::numeric_limits<double>::infinity();
    }

    if (envelopeSize < 0) {
        std::fill(output.begin(), output.end(), std::numeric_limits<float>::infinity());
        return;
    }

    int envelopeIndex = 0;
    for (int q = 0; q < count; ++q) {
        while (boundaries[envelopeIndex + 1] < static_cast<double>(q)) {
            ++envelopeIndex;
        }
        const int p = parabolaSites[envelopeIndex];
        const double delta = static_cast<double>(q - p);
        output[q] = static_cast<float>(spacingSquared * delta * delta + static_cast<double>(input[p]));
    }
}

void transformAxis(
    const std::vector<float>& input,
    std::vector<float>& output,
    const std::array<uint32_t, 3>& size,
    int axis,
    float spacing)
{
    const uint32_t lineLength = size[axis];
    std::vector<float> line(lineLength);
    std::vector<float> transformed(lineLength);
    std::vector<int> parabolaSites(lineLength);
    std::vector<double> boundaries(static_cast<size_t>(lineLength) + 1u);

    const auto sourceIndex = [axis, &size](uint32_t lineCoord, uint32_t outerA, uint32_t outerB) {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;
        if (axis == 0) {
            x = lineCoord;
            y = outerA;
            z = outerB;
        } else if (axis == 1) {
            x = outerA;
            y = lineCoord;
            z = outerB;
        } else {
            x = outerA;
            y = outerB;
            z = lineCoord;
        }
        return denseIndex(x, y, z, size);
    };

    const uint32_t outerASize = axis == 0 ? size[1] : size[0];
    const uint32_t outerBSize = axis == 2 ? size[1] : size[2];
    for (uint32_t outerB = 0; outerB < outerBSize; ++outerB) {
        for (uint32_t outerA = 0; outerA < outerASize; ++outerA) {
            for (uint32_t lineCoord = 0; lineCoord < lineLength; ++lineCoord) {
                line[lineCoord] = input[sourceIndex(lineCoord, outerA, outerB)];
            }

            distanceTransformLine(line, transformed, parabolaSites, boundaries, spacing);

            for (uint32_t lineCoord = 0; lineCoord < lineLength; ++lineCoord) {
                output[sourceIndex(lineCoord, outerA, outerB)] = transformed[lineCoord];
            }
        }
    }
}

openvdb::FloatGrid::Ptr createSignedDistanceGrid(const openvdb::FloatGrid& densityGrid, const openvdb::CoordBBox& activeBBox, float& maxDistanceToZero)
{
    const DenseBounds bounds = makeDenseBounds(activeBBox);
    const float infinity = std::numeric_limits<float>::infinity();

    std::vector<float> distanceSquared(bounds.voxelCount, infinity);
    std::vector<float> scratch(bounds.voxelCount, infinity);

    openvdb::FloatGrid::ConstAccessor densityAccessor = densityGrid.getConstAccessor();
    for (uint32_t z = 0; z < bounds.size[2]; ++z) {
        for (uint32_t y = 0; y < bounds.size[1]; ++y) {
            for (uint32_t x = 0; x < bounds.size[0]; ++x) {
                const openvdb::Coord coord(
                    bounds.min.x() + static_cast<int>(x),
                    bounds.min.y() + static_cast<int>(y),
                    bounds.min.z() + static_cast<int>(z));
                if (densityAccessor.getValue(coord) <= 0.0f) {
                    distanceSquared[denseIndex(x, y, z, bounds.size)] = 0.0f;
                }
            }
        }
    }

    transformAxis(distanceSquared, scratch, bounds.size, 0, axisWorldScale(densityGrid, 0));
    transformAxis(scratch, distanceSquared, bounds.size, 1, axisWorldScale(densityGrid, 1));
    transformAxis(distanceSquared, scratch, bounds.size, 2, axisWorldScale(densityGrid, 2));

    openvdb::FloatGrid::Ptr signedDistanceGrid = openvdb::FloatGrid::create(0.0f);
    signedDistanceGrid->setTransform(densityGrid.transform().copy());
    signedDistanceGrid->setGridClass(openvdb::GRID_LEVEL_SET);
    const std::string sourceName = densityGrid.getName();
    signedDistanceGrid->setName(sourceName.empty() ? "distance_to_zero" : sourceName + "_distance_to_zero");

    maxDistanceToZero = 0.0f;
    openvdb::FloatGrid::Accessor sdfAccessor = signedDistanceGrid->getAccessor();
    for (openvdb::FloatGrid::ValueOnCIter iter = densityGrid.cbeginValueOn(); iter; ++iter) {
        const openvdb::Coord coord = iter.getCoord();
        const uint32_t x = static_cast<uint32_t>(coord.x() - bounds.min.x());
        const uint32_t y = static_cast<uint32_t>(coord.y() - bounds.min.y());
        const uint32_t z = static_cast<uint32_t>(coord.z() - bounds.min.z());
        const float distance = std::sqrt(scratch[denseIndex(x, y, z, bounds.size)]);
        if (!std::isfinite(distance)) {
            continue;
        }

        maxDistanceToZero = std::max(maxDistanceToZero, distance);
        sdfAccessor.setValueOn(coord, iter.getValue() > 0.0f ? -distance : 0.0f);
    }

    return signedDistanceGrid;
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

    openvdb::FloatGrid::Ptr signedDistanceGrid = createSignedDistanceGrid(grid, activeBBox, volume.maxDistanceToZero);
    volume.signedDistanceHandle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*signedDistanceGrid);
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
