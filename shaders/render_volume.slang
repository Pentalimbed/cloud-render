#include "bindings.slang"

#define PNANOVDB_HLSL
#include "PNanoVDB.h"

uint hashUint(uint x)
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

float rand01(inout uint state)
{
    state = hashUint(state);
    return float(state & 0x00ffffffu) * (1.0f / 16777216.0f);
}

float3 safeNormalize(float3 v)
{
    float l2 = dot(v, v);
    return l2 > 1.0e-12f ? v * rsqrt(l2) : float3(0.0f, 0.0f, 1.0f);
}

float max3(float3 v)
{
    return max(max(v.x, v.y), v.z);
}

#include "phase.slang"

#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
static uint gVolumeSampleCount;
#endif

float3 transmittanceFromOpticalDepth(float3 opticalDepth)
{
    return exp(-min(opticalDepth, float3(80.0f, 80.0f, 80.0f)));
}

pnanovdb_grid_handle_t firstGrid()
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

bool clipIndexBounds(float3 origin, float3 direction, float3 bmin, float3 bmax, out float tmin, out float tmax)
{
    float3 fallback = sign(direction) * float3(1.0e-8f, 1.0e-8f, 1.0e-8f) + float3(1.0e-8f, 1.0e-8f, 1.0e-8f);
    float3 safeDir = select(abs(direction) < float3(1.0e-8f, 1.0e-8f, 1.0e-8f), fallback, direction);
    float3 invDir = 1.0f / safeDir;
    float3 t0 = (bmin - origin) * invDir;
    float3 t1 = (bmax - origin) * invDir;
    float3 tNear = min(t0, t1);
    float3 tFar = max(t0, t1);
    tmin = max(max(tNear.x, tNear.y), tNear.z);
    tmax = min(min(tFar.x, tFar.y), tFar.z);
    return tmax >= max(tmin, 0.0f);
}

bool clipVolume(pnanovdb_grid_handle_t grid, pnanovdb_root_handle_t root, float3 rayOrigin, float3 rayDirection, out float tmin, out float tmax)
{
    pnanovdb_coord_t bboxMinCoord = pnanovdb_root_get_bbox_min(gNanoVdb, root);
    pnanovdb_coord_t bboxMaxCoord = pnanovdb_root_get_bbox_max(gNanoVdb, root);
    float3 bmin = float3(bboxMinCoord);
    float3 bmax = float3(bboxMaxCoord + int3(1, 1, 1));
    float3 indexOrigin = pnanovdb_grid_world_to_indexf(gNanoVdb, grid, rayOrigin);
    float3 indexDirection = pnanovdb_grid_world_to_index_dirf(gNanoVdb, grid, rayDirection);
    return clipIndexBounds(indexOrigin, indexDirection, bmin, bmax, tmin, tmax);
}

float readDensityAtCoord(int3 ijk, inout pnanovdb_readaccessor_t accessor)
{
    pnanovdb_coord_t coord = ijk;
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(PNANOVDB_GRID_TYPE_FLOAT, gNanoVdb, accessor, coord);
    return max(0.0f, pnanovdb_read_float(gNanoVdb, address));
}

float sampleDensityIndex(float3 indexPosition, inout pnanovdb_readaccessor_t accessor, inout uint rng)
{
    float3 basef = floor(indexPosition);
    float3 frac = saturate(indexPosition - basef);
    int3 base = int3(basef);

    int3 corner = int3(
        rand01(rng) < frac.x ? 1 : 0,
        rand01(rng) < frac.y ? 1 : 0,
        rand01(rng) < frac.z ? 1 : 0);

    return readDensityAtCoord(base + corner, accessor);
}

float sampleDensityWorld(
    float3 worldPosition,
    pnanovdb_grid_handle_t grid,
    inout pnanovdb_readaccessor_t accessor,
    inout uint rng)
{
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    gVolumeSampleCount += 1u;
#endif
    float3 indexPosition = pnanovdb_grid_world_to_indexf(gNanoVdb, grid, worldPosition);
    return sampleDensityIndex(indexPosition, accessor, rng) * densityMultiplier;
}

#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
float3 sampleCountDebugColor()
{
    float scale = max(debugSampleCountScale, 1.0f);
    float t = saturate(log2(float(gVolumeSampleCount) + 1.0f) / log2(scale + 1.0f));
    float3 c0 = float3(0.02f, 0.03f, 0.08f);
    float3 c1 = float3(0.00f, 0.45f, 1.00f);
    float3 c2 = float3(0.10f, 0.95f, 0.45f);
    float3 c3 = float3(1.00f, 0.85f, 0.10f);
    float3 c4 = float3(1.00f, 0.08f, 0.02f);

    if (t < 0.25f) {
        return lerp(c0, c1, t * 4.0f);
    }
    if (t < 0.50f) {
        return lerp(c1, c2, (t - 0.25f) * 4.0f);
    }
    if (t < 0.75f) {
        return lerp(c2, c3, (t - 0.50f) * 4.0f);
    }
    return lerp(c3, c4, (t - 0.75f) * 4.0f);
}
#endif

float3 estimateLightTransmittance(
    float3 worldPosition,
    pnanovdb_grid_handle_t grid,
    pnanovdb_root_handle_t root,
    uint stepBudget,
    inout uint rng)
{
    float3 lightDir = safeNormalize(lightDirection);
    float t0;
    float t1;
    if (!clipVolume(grid, root, worldPosition + lightDir * 0.02f, lightDir, t0, t1)) {
        return float3(1.0f, 1.0f, 1.0f);
    }

    t0 = max(t0, 0.0f);
    uint steps = max(1u, stepBudget);
    float dt = max((t1 - t0) / float(steps), 0.001f);
    float t = t0 + dt * rand01(rng);
    float3 optical = float3(0.0f, 0.0f, 0.0f);

    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(accessor, root);

    [loop]
    for (uint i = 0u; i < steps && t < t1; ++i) {
        float density = sampleDensityWorld(worldPosition + lightDir * t, grid, accessor, rng);
        optical += density * (absorption + scattering) * dt;
        t += dt;
    }

    return transmittanceFromOpticalDepth(optical);
}

float3 renderRaymarchSample(
    float3 rayOrigin,
    float3 rayDirection,
    pnanovdb_grid_handle_t grid,
    pnanovdb_root_handle_t root,
    inout uint rng)
{
    float t0;
    float t1;
    if (!clipVolume(grid, root, rayOrigin, rayDirection, t0, t1)) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    t0 = max(t0, 0.0f);
    uint steps = max(1u, raymarchPrimarySteps);
    float dt = max((t1 - t0) / float(steps), 0.001f);
    float jitter = lerp(0.5f, rand01(rng), saturate(stepJitter));
    float t = t0 + dt * jitter;

    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(accessor, root);

    [loop]
    for (uint i = 0u; i < steps && t < t1; ++i) {
        float3 position = rayOrigin + rayDirection * t;
        float density = sampleDensityWorld(position, grid, accessor, rng);
        if (density > 0.0f) {
            float3 sigmaT = max(absorption + scattering, float3(0.0f, 0.0f, 0.0f));
            float3 lightTr = estimateLightTransmittance(position, grid, root, raymarchShadowSteps, rng);
            float phase = evalPhase(dot(safeNormalize(lightDirection), rayDirection));
            radiance += throughput * lightColor * lightTr * scattering * density * phase * dt;
            throughput *= transmittanceFromOpticalDepth(sigmaT * density * dt);
            if (max3(throughput) < 0.003f) {
                break;
            }
        }
        t += dt;
    }

    return radiance;
}

bool sampleVolPathEvent(
    float3 rayOrigin,
    float3 rayDirection,
    pnanovdb_grid_handle_t grid,
    pnanovdb_root_handle_t root,
    inout uint rng,
    inout float3 throughput,
    out float3 position)
{
    float t0;
    float t1;
    position = rayOrigin;
    if (!clipVolume(grid, root, rayOrigin, rayDirection, t0, t1)) {
        return false;
    }

    t0 = max(t0, 0.0f);
    float3 sigmaT = max(absorption + scattering, float3(0.0f, 0.0f, 0.0f));
    float majorant = max(densityMajorant * max3(sigmaT), 1.0e-4f);
    float t = t0;

    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(accessor, root);

    // Volpathmis-style null-collision tracking: sample free flights from a scalar
    // majorant, classify sampled points as null or real medium events, and carry
    // the corresponding throughput ratio forward.
    [loop]
    for (uint step = 0u; step < 1024u && t < t1; ++step) {
        t += -log(max(rand01(rng), 1.0e-6f)) / majorant;
        if (t >= t1) {
            return false;
        }

        position = rayOrigin + rayDirection * t;
        float density = sampleDensityWorld(position, grid, accessor, rng);
        float3 localSigmaT = density * sigmaT;
        float realProb = saturate(max3(localSigmaT) / majorant);

        if (realProb > 0.0f && rand01(rng) < realProb) {
            float3 scatterWeight = density * scattering / (majorant * max(realProb, 1.0e-4f));
            throughput *= scatterWeight;
            return true;
        }

        float nullProb = max(1.0f - realProb, 1.0e-4f);
        float3 nullWeight = max((majorant - localSigmaT) / (majorant * nullProb), float3(0.0f, 0.0f, 0.0f));
        throughput *= nullWeight;
        if (max3(throughput) < 1.0e-4f) {
            return false;
        }
    }

    return false;
}

float3 renderPathTraceSample(
    float3 rayOrigin,
    float3 rayDirection,
    pnanovdb_grid_handle_t grid,
    pnanovdb_root_handle_t root,
    inout uint rng)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (uint bounce = 0u; bounce < max(1u, pathMaxBounces); ++bounce) {
        float3 position;
        if (!sampleVolPathEvent(rayOrigin, rayDirection, grid, root, rng, throughput, position)) {
            break;
        }

        // Directional-light next-event estimate. With a single delta directional
        // light, the light-sampling MIS branch from volpathmis degenerates to
        // weight 1; phase-sampled paths cannot hit this light by solid angle.
        float3 lightTr = estimateLightTransmittance(position, grid, root, 48u, rng);
        float phase = evalPhase(dot(safeNormalize(lightDirection), rayDirection));
        radiance += throughput * lightColor * lightTr * phase;

        rayDirection = samplePhaseDirection(rayDirection, rng);
        rayOrigin = position + rayDirection * 0.02f;

        if (bounce >= 2u) {
            float continueProb = clamp(max3(throughput), 0.05f, 0.95f);
            if (rand01(rng) > continueProb) {
                break;
            }
            throughput /= continueProb;
        }
    }

    return radiance;
}

float3 makeCameraRay(uint2 pixel, inout uint rng)
{
    float2 jitter = float2(rand01(rng), rand01(rng));
    float2 uv = (float2(pixel) + jitter) / float2(imageWidth, imageHeight);
    float2 ndc = uv * 2.0f - 1.0f;
    ndc.y = -ndc.y;

    float aspect = float(imageWidth) / max(float(imageHeight), 1.0f);
    float tanHalfFov = tan(0.5f * fovYRadians);
    return safeNormalize(cameraForward + cameraRight * (ndc.x * aspect * tanHalfFov) + cameraUp * (ndc.y * tanHalfFov));
}

[numthreads(8, 8, 1)]
void renderVolume(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= imageWidth || dispatchThreadId.y >= imageHeight) {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    uint seed = hashUint(pixel.x + pixel.y * imageWidth + frameIndex * 9781u + 0x9e3779b9u);
    pnanovdb_grid_handle_t grid = firstGrid();
    pnanovdb_root_handle_t root = gridRoot(grid);

    float3 rayDirection = makeCameraRay(pixel, seed);
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    gVolumeSampleCount = 0u;
#endif
    float3 color;
    if (rendererMode == 0u) {
        color = renderRaymarchSample(cameraPosition, rayDirection, grid, root, seed);
    } else {
        color = renderPathTraceSample(cameraPosition, rayDirection, grid, root, seed);
    }
#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    if (debugViewMode == 1u) {
        color = sampleCountDebugColor();
    }

    gRenderOut[pixel] = float4(max(color, float3(0.0f, 0.0f, 0.0f)), min(float(gVolumeSampleCount), 65504.0f));
#else
    gRenderOut[pixel] = float4(max(color, float3(0.0f, 0.0f, 0.0f)), 1.0f);
#endif
}
