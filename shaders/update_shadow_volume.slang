#include "bindings.slang"

#define PNANOVDB_HLSL
#include "PNanoVDB.h"

pnanovdb_grid_handle_t firstDensityGrid()
{
    pnanovdb_grid_handle_t grid;
    grid.address = pnanovdb_address_null();
    return grid;
}

pnanovdb_grid_handle_t firstDistanceGrid()
{
    pnanovdb_grid_handle_t grid;
    grid.address = pnanovdb_address_null();
    return grid;
}

pnanovdb_root_handle_t gridRoot(pnanovdb_grid_handle_t grid)
{
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(gNanoVdb, grid);
    return pnanovdb_tree_get_root(gNanoVdb, tree);
}

pnanovdb_root_handle_t distanceGridRoot(pnanovdb_grid_handle_t grid)
{
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(gDistanceNanoVdb, grid);
    return pnanovdb_tree_get_root(gDistanceNanoVdb, tree);
}

float readDensityAtCoord(int3 ijk, inout pnanovdb_readaccessor_t accessor)
{
    pnanovdb_coord_t coord = ijk;
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, gNanoVdb, accessor, coord);
    return max(0.0f, pnanovdb_read_float(gNanoVdb, address));
}

float readSignedDistanceAtCoord(int3 ijk, inout pnanovdb_readaccessor_t accessor)
{
    pnanovdb_coord_t coord = ijk;
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, gDistanceNanoVdb, accessor, coord);
    return pnanovdb_read_float(gDistanceNanoVdb, address);
}

bool clipWorldBounds(float3 rayOrigin, float3 rayDirection, out float tmin, out float tmax)
{
    float3 safeDir = rayDirection;
    safeDir.x = abs(safeDir.x) < 1.0e-8f ? (safeDir.x < 0.0f ? -1.0e-8f : 1.0e-8f) : safeDir.x;
    safeDir.y = abs(safeDir.y) < 1.0e-8f ? (safeDir.y < 0.0f ? -1.0e-8f : 1.0e-8f) : safeDir.y;
    safeDir.z = abs(safeDir.z) < 1.0e-8f ? (safeDir.z < 0.0f ? -1.0e-8f : 1.0e-8f) : safeDir.z;

    float3 invDir = 1.0f / safeDir;
    float3 t0 = (volumeWorldMin - rayOrigin) * invDir;
    float3 t1 = (volumeWorldMax - rayOrigin) * invDir;
    float3 tNear = min(t0, t1);
    float3 tFar = max(t0, t1);
    tmin = max(max(tNear.x, tNear.y), tNear.z);
    tmax = min(min(tFar.x, tFar.y), tFar.z);
    return tmax >= max(tmin, 0.0f);
}

float sampleProfileDensityWorld(
    float3                        worldPosition,
    pnanovdb_grid_handle_t        grid,
    inout pnanovdb_readaccessor_t densityAccessor,
    pnanovdb_grid_handle_t        distanceGrid,
    inout pnanovdb_readaccessor_t distanceAccessor)
{
    float3 indexPosition = pnanovdb_grid_world_to_indexf(gNanoVdb, grid, worldPosition);
    int3 sampleIndex = int3(floor(indexPosition + 0.5f));

    float baseDensity = readDensityAtCoord(sampleIndex, densityAccessor);
    if (baseDensity <= 0.0f) {
        return 0.0f;
    }

    float signedDistance = -readSignedDistanceAtCoord(sampleIndex, distanceAccessor);
    float dimensionalProfile = saturate(max(signedDistance, 0.0f) / max(maxDistanceToZero, 1.0e-4f));
    return baseDensity * dimensionalProfile * densityMultiplier;
}

[numthreads(4, 4, 4)]
void updateShadowVolume(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint width;
    uint height;
    uint depth;
    gShadowSummedDensityOut.GetDimensions(width, height, depth);

    if (dispatchThreadId.x >= width || dispatchThreadId.y >= height || dispatchThreadId.z >= depth) {
        return;
    }

    uint flatIndex = dispatchThreadId.x + dispatchThreadId.y * width + dispatchThreadId.z * width * height;
    uint updateFrames = max(shadowUpdateFrames, 1u);
    if ((flatIndex % updateFrames) != (shadowUpdateFrame % updateFrames)) {
        return;
    }

    float3 uvw = (float3(dispatchThreadId) + 0.5f) / float3(width, height, depth);
    float3 worldPosition = lerp(volumeWorldMin, volumeWorldMax, uvw);

    float t0;
    float t1;
    if (!clipWorldBounds(worldPosition, lightDirection, t0, t1)) {
        gShadowSummedDensityOut[dispatchThreadId] = 0.0f;
        return;
    }

    float t = max(t0, 0.0f);
    float tEnd = max(t1, 0.0f);
    float stepLength = max(shadowVolumeStepLength, 1.0f);
    float summedDensity = 0.0f;

    pnanovdb_grid_handle_t grid = firstDensityGrid();
    pnanovdb_root_handle_t root = gridRoot(grid);
    pnanovdb_grid_handle_t distanceGrid = firstDistanceGrid();
    pnanovdb_root_handle_t distanceRoot = distanceGridRoot(distanceGrid);

    pnanovdb_readaccessor_t densityAccessor;
    pnanovdb_readaccessor_init(densityAccessor, root);
    pnanovdb_readaccessor_t distanceAccessor;
    pnanovdb_readaccessor_init(distanceAccessor, distanceRoot);

    [loop]
    for (uint i = 0u; i < 512u && t < tEnd; ++i) {
        float segment = min(stepLength, tEnd - t);
        float3 samplePosition = worldPosition + lightDirection * (t + segment * 0.5f);
        summedDensity += sampleProfileDensityWorld(samplePosition, grid, densityAccessor, distanceGrid, distanceAccessor) * segment;
        t += segment;
    }

    gShadowSummedDensityOut[dispatchThreadId] = summedDensity;
}
