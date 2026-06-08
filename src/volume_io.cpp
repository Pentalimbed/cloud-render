#include "volume_io.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

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
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }
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
    volume.handle = nanovdb::tools::createNanoGrid<openvdb::FloatGrid, float>(*grid);
    if (!volume.handle.grid<float>()) {
        throw std::runtime_error("OpenVDB to NanoVDB conversion did not produce a float grid");
    }
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
