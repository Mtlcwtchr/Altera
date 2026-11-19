// The game's placeholder actors (src/game_view/sim_game.cpp): people, beasts
// and the village's buildings as flat-shaded low-poly shapes, one instanced
// draw per mesh. Positions are eye-relative metres, as everything in the lab.
// Lit by the world's sun and sky (the prefix of terrain_core::LightUniforms,
// same arithmetic as lighting.hlsli shadeSurface/applyFog, without the shadow
// map and the clouds' shadows).

cbuffer View : register(b0, space1) {
    row_major float4x4 viewProjection;
};

cbuffer Light : register(b0, space3) {
    float3 lightSun;
    float lightSunAbove;
    float3 lightSunColour;
    float lightTime;
    float3 lightZenith;
    float lightFogDensity;
    float3 lightHorizon;
    float lightFogFalloff;
    float3 lightGround;
    float lightEyeZ;
};

struct VsIn {
    float3 position : TEXCOORD0;      // model space, metres, z up
    float3 normal : TEXCOORD1;
    float shade : TEXCOORD2;          // 0: body colour, 1: second colour (head, roof)
    float4 placeYaw : TEXCOORD3;      // instance: eye-relative origin, yaw
    float4 scale : TEXCOORD4;         // instance: scale x, y, z; highlight
    float4 colour : TEXCOORD5;        // instance: body rgb, -
    float4 colour2 : TEXCOORD6;       // instance: second rgb, -
};

struct VsOut {
    float4 position : SV_Position;
    float3 rel : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 albedo : TEXCOORD2;
    float highlight : TEXCOORD3;
};

VsOut actorsVs(VsIn v) {
    const float c = cos(v.placeYaw.w), s = sin(v.placeYaw.w);
    const float3 p = v.position * v.scale.xyz;
    const float3 rel = v.placeYaw.xyz + float3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
    float3 n = v.normal / max(v.scale.xyz, 1e-3);
    n = normalize(float3(n.x * c - n.y * s, n.x * s + n.y * c, n.z));
    VsOut o;
    o.position = mul(viewProjection, float4(rel, 1));
    o.rel = rel;
    o.normal = n;
    o.albedo = lerp(v.colour.rgb, v.colour2.rgb, v.shade);
    o.highlight = v.scale.w;
    return o;
}

float3 skyColour(float3 dir) {
    const float up = saturate(dir.z);
    return lerp(lightHorizon, lightZenith, pow(up, 0.55));
}

float3 applyFog(float3 colour, float3 rel) {
    const float distance = length(rel);
    const float k = lightFogFalloff;
    const float atEye = lightFogDensity * exp(-k * max(lightEyeZ, 0.0));
    const float rise = rel.z * k;
    const float through = abs(rise) > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0;
    const float amount = 1.0 - exp(-atEye * distance * through);
    const float3 dir = rel / max(distance, 1e-3);
    return lerp(colour, skyColour(float3(dir.xy, max(dir.z, 0.0) * 0.3)), saturate(amount));
}

float4 actorsPs(VsOut i) : SV_Target0 {
    const float3 n = normalize(i.normal);
    const float facing = dot(n, lightSun);
    float3 light = lightSunColour * saturate(facing);
    light += lerp(lightGround, lightZenith, n.z * 0.5 + 0.5) * 0.75;
    light += lightSunColour * float3(1.0, 0.86, 0.70) * 0.12;
    float3 c = i.albedo * light;
    // The selected / controlled one: a warm rim so it reads at any distance.
    const float rim = pow(1.0 - saturate(dot(n, normalize(-i.rel))), 2.0);
    c += float3(1.0, 0.80, 0.35) * rim * i.highlight * 0.9;
    return float4(applyFog(c, i.rel), 1);
}
