#ifndef CLOUD_RENDER_ENABLE_DEBUG_VIZ
#define CLOUD_RENDER_ENABLE_DEBUG_VIZ 0
#endif

cbuffer RenderConstants : register(b0)
{
    float3 cameraPosition;
    uint frameIndex;

    float3 cameraForward;
    uint rendererMode;

    float3 cameraRight;
    uint imageWidth;

    float3 cameraUp;
    uint imageHeight;

    float3 lightDirection;
    float densityMultiplier;

    float3 lightColor;
    float exposure;

    float3 absorption;
    float stepJitter;

    float3 scattering;
    float densityMajorant;

    float3 volumeWorldMin;
    float raymarchPrimaryMinStep;

    float3 volumeWorldMax;
    uint raymarchShadowSteps;

    uint pathMaxBounces;
    float fovYRadians;
    float temporalBlend;
    float raymarchPrimaryStepScale;

    float timeSeconds;
    uint resetHistory;
    uint pathHistoryMode;
    uint phaseFunctionMode;
    
    float cloudPhaseGhg;
    float cloudPhaseGd;
    float cloudPhaseAlpha;
    float cloudPhaseWeight;

#if CLOUD_RENDER_ENABLE_DEBUG_VIZ
    uint debugViewMode;
    float debugSampleCountScale;
    float debugMaxDistanceToZero;
    float debugPad1;
#endif

    float phasePad0;
    float phasePad1;
    float phasePad2;
}

StructuredBuffer<uint> gNanoVdb : register(t0);
Texture2D<float4> gCurrentColor : register(t1);
Texture2D<float4> gHistoryColor : register(t2);
Texture2D<float4> gDenoisedColor : register(t3);
StructuredBuffer<uint> gDistanceNanoVdb : register(t4);

RWTexture2D<float4> gRenderOut : register(u0);
RWTexture2D<float4> gDenoisedOut : register(u1);
RWTexture2D<float4> gHistoryOut : register(u2);
RWTexture2D<float4> gDisplayOut : register(u3);
