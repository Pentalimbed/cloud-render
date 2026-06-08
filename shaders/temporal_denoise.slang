#include "bindings.slang"

[numthreads(8, 8, 1)]
void temporalDenoise(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= imageWidth || dispatchThreadId.y >= imageHeight) {
        return;
    }

    uint2 pixel = dispatchThreadId.xy;
    float4 current = gCurrentColor.Load(int3(pixel, 0));
    float4 history = gHistoryColor.Load(int3(pixel, 0));

    float4 denoised;
    if (rendererMode == 1u && pathHistoryMode == 1u) {
        float sampleCount = resetHistory != 0u ? 0.0f : max(history.a, 0.0f);
        float nextCount = min(sampleCount + 1.0f, 65504.0f);
        float3 accumulated = (history.rgb * sampleCount + current.rgb) / max(nextCount, 1.0f);
        denoised = float4(accumulated, nextCount);
    } else {
        float blend = resetHistory != 0u ? 0.0f : saturate(temporalBlend);
        denoised = lerp(float4(current.rgb, 1.0f), float4(history.rgb, 1.0f), blend);
    }

    gDenoisedOut[pixel] = denoised;
    gHistoryOut[pixel] = denoised;
}
