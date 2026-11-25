// The game's interface over the finished frame: the ui::Canvas picture
// (BGRA, straight alpha) on one full-screen triangle, alpha-blended.

Texture2D<float4> picture : register(t0, space2);
SamplerState pictureSampler : register(s0, space2);

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut overlayVs(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 overlayPs(VsOut input) : SV_Target0 {
    return picture.SampleLevel(pictureSampler, input.uv, 0);
}
