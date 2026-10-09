// Camera motion for the static terrain and depth-writing vegetation. Pixel
// motion points from current to previous, excludes jitter, and uses the input
// resolution. The infinite sky is reprojected as a direction (depth = zero).
Texture2D<float> depthTexture : register(t0, space0);
SamplerState depthSampler : register(s0, space0);
RWTexture2D<float2> motionTexture : register(u0, space1);
cbuffer Motion : register(b0, space2) {
    row_major float4x4 clipToPrevious;
    float2 resolution;
    float2 jitterOffset;
};
[numthreads(8, 8, 1)]
void motionCs(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(resolution))) return;
    float2 uv = (float2(id.xy) + 0.5) / resolution;
    float4 clip = float4(uv.x * 2 - 1, 1 - uv.y * 2, depthTexture.SampleLevel(depthSampler, uv, 0), 1);
    float4 previous = mul(clipToPrevious, clip);
    float2 motion = 0;
    if (previous.w > 1e-6) {
        float2 previousUv = previous.xy / previous.w * float2(0.5, -0.5) + 0.5;
        motion = (previousUv - uv) * resolution - jitterOffset;
    }
    motionTexture[id.xy] = motion;
}
