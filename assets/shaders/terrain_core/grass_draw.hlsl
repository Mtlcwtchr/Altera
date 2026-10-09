// Grass tufts from grass_spawn.hlsl: two crossed cards each, made from the
// vertex index - no mesh. The sprite is white and shaded; the colour is the
// ground's, lit as the ground is, so a tuft's foot is the ground under it and
// its tip a little lighter.

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;   // eye-relative
};

struct TuftIn {
    uint vertex : SV_VertexID;
    float4 positionYaw : TEXCOORD0;
    float4 shape : TEXCOORD1;            // width, height, sprite, packed colour
};

struct VsOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float sprite : TEXCOORD1;
    nointerpolation float3 colour : TEXCOORD2;
    float3 view : TEXCOORD3;
};

VsOut grassVs(TuftIn input) {
    // Two quads, six vertices each: (0,0) (1,0) (1,1) (0,0) (1,1) (0,1).
    const uint corner = input.vertex % 6u;
    const uint card = input.vertex / 6u;
    const float2 c = float2(corner == 1 || corner == 2 || corner == 4 ? 1.0 : 0.0,
                            corner == 2 || corner == 4 || corner == 5 ? 1.0 : 0.0);
    const float yaw = input.positionYaw.w + float(card) * 1.5707963;
    const float2 across = float2(cos(yaw), sin(yaw)) * (c.x - 0.5) * input.shape.x;
    // Seen from above a card is an edge - a dark scratch on the ground. The
    // steeper the eye looks down at a tuft, the lower it lies, until from
    // overhead it is gone and the ground under it, the same colour, is all
    // there is.
    const float down = saturate(-normalize(input.positionYaw.xyz).z);
    const float height = input.shape.y * (1.0 - smoothstep(0.45, 0.8, down));
    VsOut o;
    o.view = input.positionYaw.xyz + float3(across, c.y * height);
    o.position = mul(viewProjection, float4(o.view, 1));
    o.uv = float2(c.x, 1.0 - c.y);
    o.sprite = input.shape.z;
    const uint packed = asuint(input.shape.w);
    o.colour = float3(packed & 255u, (packed >> 8) & 255u, (packed >> 16) & 255u) / 255.0 * 2.0;
    return o;
}

Texture2DArray<float4> sprites : register(t0, space2);
SamplerState spriteSampler : register(s0, space2);

cbuffer Look : register(b0, space3) {
    float3 sunDirection;
    float hazeDistance;
    float3 skyColour;
    uint debugLevels;
    uint lookClipSize;
    uint lookLevels;
    uint lookFlags;
    float lookPad;
};

float4 grassPs(VsOut input) : SV_Target0 {
    const float4 s = sprites.Sample(spriteSampler, float3(input.uv, input.sprite));
    clip(s.a - 0.45);
    // Foot darker, tip lighter - but on average exactly the ground's own
    // brightness (the 1.12), and flat with distance, so where the tufts end
    // the ground goes on in the same colour and there is no edge to find.
    const float up = 1.0 - input.uv.y;
    const float shade = lerp(0.72, 1.12, up) * lerp(0.85, 1.05, s.r) * 1.12;
    const float3 colour = input.colour * lerp(shade, 1.0, saturate(length(input.view) / 70.0));
    const float haze = 1.0 - exp(-length(input.view) / hazeDistance);
    return float4(lerp(colour, skyColour, haze), 1);
}
