#ifndef CLOUD_RENDER_PHASE_SLANG
#define CLOUD_RENDER_PHASE_SLANG

static const float PhasePi = 3.14159265358979323846f;
static const float PhaseInv4Pi = 0.07957747154594767f;
static const uint PhaseModeHenyeyGreenstein = 0u;
static const uint PhaseModeDraine = 1u;
static const uint PhaseModeJendersieCloud = 2u;

struct CloudPhaseParameters {
    float gHg;
    float gD;
    float alpha;
    float wD;
};

float phaseClampG(float g)
{
    return clamp(g, -0.999f, 0.999f);
}

float phaseSqr(float x)
{
    return x * x;
}

float phaseSafeDenom(float value)
{
    return max(value, 1.0e-6f);
}

float3 phaseSafeNormalize(float3 v)
{
    float l2 = dot(v, v);
    return l2 > 1.0e-12f ? v * rsqrt(l2) : float3(0.0f, 0.0f, 1.0f);
}

float evalHenyeyGreenstein(float u, float g)
{
    float phaseG = phaseClampG(g);
    float g2 = phaseG * phaseG;
    float denom = phaseSafeDenom(1.0f + g2 - 2.0f * phaseG * u);
    return PhaseInv4Pi * (1.0f - g2) / (denom * sqrt(denom));
}

float sampleHenyeyGreensteinCos(float xi, float g)
{
    float phaseG = phaseClampG(g);
    if (abs(phaseG) < 1.0e-3f) {
        return 2.0f * xi - 1.0f;
    }

    float sq = (1.0f - phaseG * phaseG) / max(1.0f - phaseG + 2.0f * phaseG * xi, 1.0e-6f);
    return clamp((1.0f + phaseG * phaseG - sq * sq) / (2.0f * phaseG), -1.0f, 1.0f);
}

float evalDraine(float u, float g, float alpha)
{
    float phaseG = phaseClampG(g);
    float a = max(alpha, 0.0f);
    float g2 = phaseG * phaseG;
    float denom = phaseSafeDenom(1.0f + g2 - 2.0f * phaseG * u);
    float normalization = max(1.0f + a * (1.0f + 2.0f * g2) / 3.0f, 1.0e-6f);
    return ((1.0f - g2) * (1.0f + a * u * u)) / (4.0f * PhasePi * normalization * denom * sqrt(denom));
}

float sampleDraineZeroGCos(float xi, float alpha)
{
    float a = max(alpha, 0.0f);
    if (a < 1.0e-5f) {
        return 2.0f * xi - 1.0f;
    }

    float target = (2.0f * xi - 1.0f) * (1.0f + a / 3.0f);
    float u = 2.0f * xi - 1.0f;

    for (int i = 0; i < 6; ++i) {
        float u2 = u * u;
        float f = u + (a * u * u2) / 3.0f - target;
        float df = max(1.0f + a * u2, 1.0e-5f);
        u = clamp(u - f / df, -1.0f, 1.0f);
    }

    return u;
}

float sampleDraineCos(float xi, float g, float alpha)
{
    float phaseG = phaseClampG(g);
    float a = max(alpha, 0.0f);
    if (a < 1.0e-5f) {
        return sampleHenyeyGreensteinCos(xi, phaseG);
    }
    if (abs(phaseG) < 1.0e-3f) {
        return sampleDraineZeroGCos(xi, a);
    }

    // Exact inverse-CDF form from the Jendersie/d'Eon Draine supplemental code.
    const float twoCubeRoot = 1.2599210498948732f;
    float g2 = phaseG * phaseG;
    float g3 = phaseG * g2;
    float g4 = g2 * g2;
    float g6 = g2 * g4;
    float pgp1_2 = (1.0f + g2) * (1.0f + g2);
    float T1 = (-1.0f + g2) * (4.0f * g2 + a * pgp1_2);
    float T1a = -a + a * g4;
    float T1a3 = T1a * T1a * T1a;
    float T2 = -1296.0f * (-1.0f + g2) * (a - a * g2) * T1a * (4.0f * g2 + a * pgp1_2);
    float T3 = 3.0f * g2 * (1.0f + phaseG * (-1.0f + 2.0f * xi))
        + a * (2.0f + g2 + g3 * (1.0f + 2.0f * g2) * (-1.0f + 2.0f * xi));
    float T4a = 432.0f * T1a3 + T2 + 432.0f * (a - a * g2) * T3 * T3;
    float T4b = -144.0f * a * g2 + 288.0f * a * g4 - 144.0f * a * g6;
    float T4b3 = T4b * T4b * T4b;
    float T4 = T4a + sqrt(max(-4.0f * T4b3 + T4a * T4a, 0.0f));
    float T4p3 = pow(max(T4, 1.0e-20f), 1.0f / 3.0f);
    float denom = max(a - a * g2, 1.0e-8f);
    float T6 = (2.0f * T1a
        + (48.0f * twoCubeRoot * (-(a * g2) + 2.0f * a * g4 - a * g6)) / T4p3
        + T4p3 / (3.0f * twoCubeRoot)) / denom;
    float T5 = 6.0f * (1.0f + g2) + T6;
    float sqrtT5 = sqrt(max(T5, 1.0e-8f));
    float rootDenom = a * (-1.0f + g2) * sqrtT5;
    rootDenom = rootDenom < -1.0e-8f ? rootDenom : -1.0e-8f;
    float secondRootArg = 6.0f * (1.0f + g2)
        - (8.0f * T3) / rootDenom
        - T6;
    float rootTerm = -0.5f * sqrtT5 + 0.5f * sqrt(max(secondRootArg, 0.0f));
    return clamp((1.0f + g2 - rootTerm * rootTerm) / (2.0f * phaseG), -1.0f, 1.0f);
}

CloudPhaseParameters currentCloudPhaseParameters()
{
    CloudPhaseParameters params;
    params.gHg = phaseClampG(cloudPhaseGhg);
    params.gD = phaseClampG(cloudPhaseGd);
    params.alpha = max(cloudPhaseAlpha, 0.0f);
    params.wD = saturate(cloudPhaseWeight);
    return params;
}

float evalCloudPhase(float u)
{
    CloudPhaseParameters params = currentCloudPhaseParameters();
    return (1.0f - params.wD) * evalHenyeyGreenstein(u, params.gHg)
        + params.wD * evalDraine(u, params.gD, params.alpha);
}

float evalPhase(float u)
{
    if (phaseFunctionMode == PhaseModeDraine) {
        return evalDraine(u, anisotropy, draineAlpha);
    }
    if (phaseFunctionMode == PhaseModeJendersieCloud) {
        return evalCloudPhase(u);
    }
    return evalHenyeyGreenstein(u, anisotropy);
}

void phaseMakeBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    float3 helper = abs(n.z) < 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = phaseSafeNormalize(cross(helper, n));
    bitangent = cross(n, tangent);
}

float3 sampleDirectionFromPhaseCos(float3 incomingDirection, float cosTheta, float xi)
{
    float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    float phi = 2.0f * PhasePi * xi;
    float3 t;
    float3 b;
    float3 n = phaseSafeNormalize(incomingDirection);
    phaseMakeBasis(n, t, b);
    return phaseSafeNormalize(t * (cos(phi) * sinTheta) + b * (sin(phi) * sinTheta) + n * cosTheta);
}

float3 sampleCloudPhaseDirection(float3 incomingDirection, inout uint rng)
{
    CloudPhaseParameters params = currentCloudPhaseParameters();
    float xi = rand01(rng);
    float cosTheta;

    if (xi < params.wD && params.wD > 1.0e-5f) {
        cosTheta = sampleDraineCos(xi / params.wD, params.gD, params.alpha);
    } else {
        float hgWeight = max(1.0f - params.wD, 1.0e-5f);
        cosTheta = sampleHenyeyGreensteinCos(saturate((xi - params.wD) / hgWeight), params.gHg);
    }

    return sampleDirectionFromPhaseCos(incomingDirection, cosTheta, rand01(rng));
}

float3 samplePhaseDirection(float3 incomingDirection, inout uint rng)
{
    float cosTheta;
    if (phaseFunctionMode == PhaseModeDraine) {
        cosTheta = sampleDraineCos(rand01(rng), anisotropy, draineAlpha);
        return sampleDirectionFromPhaseCos(incomingDirection, cosTheta, rand01(rng));
    }
    if (phaseFunctionMode == PhaseModeJendersieCloud) {
        return sampleCloudPhaseDirection(incomingDirection, rng);
    }

    cosTheta = sampleHenyeyGreensteinCos(rand01(rng), anisotropy);
    return sampleDirectionFromPhaseCos(incomingDirection, cosTheta, rand01(rng));
}

#endif
