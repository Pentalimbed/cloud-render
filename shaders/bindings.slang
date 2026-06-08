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
    float anisotropy;

    float3 absorption;
    float stepJitter;

    float3 scattering;
    float exposure;

    float3 volumeWorldMin;
    uint raymarchSteps;

    float3 volumeWorldMax;
    uint shadowSteps;

    uint pathSamples;
    uint pathDepth;
    float fovYRadians;
    float temporalBlend;

    float densityMajorant;
    float timeSeconds;
    uint resetHistory;
    uint pathHistoryMode;
}

StructuredBuffer<uint> gNanoVdb : register(t0);
Texture2D<float4> gCurrentColor : register(t1);
Texture2D<float4> gHistoryColor : register(t2);
Texture2D<float4> gDenoisedColor : register(t3);

RWTexture2D<float4> gRenderOut : register(u0);
RWTexture2D<float4> gDenoisedOut : register(u1);
RWTexture2D<float4> gHistoryOut : register(u2);
RWTexture2D<float4> gDisplayOut : register(u3);
