#include "bindings.slang"

#define PNANOVDB_HLSL
#include "PNanoVDB.h"

static const float Pi = 3.14159265358979323846f;
static const float Inv4Pi = 0.07957747154594767f;

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
    return l2 > 1.0e-12f ? v * rsqrt(l2) : float3(0.0f, 1.0f, 0.0f);
}

float max3(float3 v)
{
    return max(max(v.x, v.y), v.z);
}

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

float sampleDensityWorld(float3 worldPosition, pnanovdb_grid_handle_t grid, inout pnanovdb_readaccessor_t accessor, inout uint rng)
{
    float3 indexPosition = pnanovdb_grid_world_to_indexf(gNanoVdb, grid, worldPosition);
    return sampleDensityIndex(indexPosition, accessor, rng) * densityMultiplier;
}

float3 estimateLightTransmittance(float3 worldPosition, pnanovdb_grid_handle_t grid, pnanovdb_root_handle_t root, inout uint rng)
{
    float3 lightDir = safeNormalize(lightDirection);
    float t0;
    float t1;
    if (!clipVolume(grid, root, worldPosition + lightDir * 0.02f, lightDir, t0, t1)) {
        return float3(1.0f, 1.0f, 1.0f);
    }

    t0 = max(t0, 0.0f);
    uint steps = max(1u, shadowSteps);
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

float phaseHenyeyGreenstein(float cosTheta)
{
    float g = clamp(anisotropy, -0.95f, 0.95f);
    float denom = max(1.0f + g * g - 2.0f * g * cosTheta, 1.0e-4f);
    return Inv4Pi * (1.0f - g * g) / (denom * sqrt(denom));
}

void makeBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    float3 helper = abs(n.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = safeNormalize(cross(helper, n));
    bitangent = cross(n, tangent);
}

float3 sampleHenyeyGreensteinDirection(float3 incomingDirection, inout uint rng)
{
    float g = clamp(anisotropy, -0.95f, 0.95f);
    float u = rand01(rng);
    float cosTheta;
    if (abs(g) < 1.0e-3f) {
        cosTheta = 1.0f - 2.0f * u;
    } else {
        float sq = (1.0f - g * g) / max(1.0f - g + 2.0f * g * u, 1.0e-4f);
        cosTheta = clamp((1.0f + g * g - sq * sq) / (2.0f * g), -1.0f, 1.0f);
    }

    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float phi = 2.0f * Pi * rand01(rng);
    float3 t;
    float3 b;
    makeBasis(safeNormalize(incomingDirection), t, b);
    return safeNormalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + safeNormalize(incomingDirection) * cosTheta);
}

float3 renderRaymarchSample(float3 rayOrigin, float3 rayDirection, pnanovdb_grid_handle_t grid, pnanovdb_root_handle_t root, inout uint rng)
{
    float t0;
    float t1;
    if (!clipVolume(grid, root, rayOrigin, rayDirection, t0, t1)) {
        return float3(0.0f, 0.0f, 0.0f);
    }

    t0 = max(t0, 0.0f);
    uint steps = max(1u, raymarchSteps);
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
            float3 lightTr = estimateLightTransmittance(position, grid, root, rng);
            float phase = phaseHenyeyGreenstein(dot(safeNormalize(lightDirection), -rayDirection));
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
    for (uint step = 0u; step < max(1u, raymarchSteps) && t < t1; ++step) {
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

float3 renderPathTraceSample(float3 rayOrigin, float3 rayDirection, pnanovdb_grid_handle_t grid, pnanovdb_root_handle_t root, inout uint rng)
{
    float3 radiance = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    [loop]
    for (uint bounce = 0u; bounce < max(1u, pathDepth); ++bounce) {
        float3 position;
        if (!sampleVolPathEvent(rayOrigin, rayDirection, grid, root, rng, throughput, position)) {
            break;
        }

        // Directional-light next-event estimate. With a single delta directional
        // light, the light-sampling MIS branch from volpathmis degenerates to
        // weight 1; phase-sampled paths cannot hit this light by solid angle.
        float3 lightTr = estimateLightTransmittance(position, grid, root, rng);
        float phase = phaseHenyeyGreenstein(dot(safeNormalize(lightDirection), -rayDirection));
        radiance += throughput * lightColor * lightTr * phase;

        rayDirection = sampleHenyeyGreensteinDirection(rayDirection, rng);
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

    uint samples = max(1u, pathSamples);
    float3 color = float3(0.0f, 0.0f, 0.0f);

    [loop]
    for (uint i = 0u; i < samples; ++i) {
        float3 rayDirection = makeCameraRay(pixel, seed);
        if (rendererMode == 0u) {
            color += renderRaymarchSample(cameraPosition, rayDirection, grid, root, seed);
        } else {
            color += renderPathTraceSample(cameraPosition, rayDirection, grid, root, seed);
        }
    }

    color /= float(samples);
    gRenderOut[pixel] = float4(max(color, float3(0.0f, 0.0f, 0.0f)), 1.0f);
}
