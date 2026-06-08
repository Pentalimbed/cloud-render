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
    float blend = resetHistory != 0u ? 0.0f : saturate(temporalBlend);
    float4 denoised = lerp(current, history, blend);
    denoised.a = 1.0f;
    gDenoisedOut[pixel] = denoised;
    gHistoryOut[pixel] = denoised;
}
