#include "bindings.slang"

float3 tonemapAces(float3 x)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[numthreads(8, 8, 1)]
void tonemap(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= imageWidth || dispatchThreadId.y >= imageHeight) {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    float3 hdr = gDenoisedColor.Load(int3(pixel, 0)).rgb * exposure;
    float3 srgb = pow(tonemapAces(hdr), float3(1.0f / 2.2f, 1.0f / 2.2f, 1.0f / 2.2f));
    gDisplayOut[pixel] = float4(srgb, 1.0f);
}
