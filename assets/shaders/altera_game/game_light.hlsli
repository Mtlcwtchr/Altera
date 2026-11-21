// The world's light for the game's models (people, beasts, buildings): the
// prefix of terrain_core::LightUniforms and the same arithmetic as
// lighting.hlsli shadeSurface/applyFog, without the shadow map and the
// clouds' shadows. Shared by skinned.hlsl and statics.hlsl.

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

float3 gameSky(float3 dir) {
    const float up = saturate(dir.z);
    return lerp(lightHorizon, lightZenith, pow(up, 0.55));
}

float3 gameFog(float3 colour, float3 rel) {
    const float distance = length(rel);
    const float k = lightFogFalloff;
    const float atEye = lightFogDensity * exp(-k * max(lightEyeZ, 0.0));
    const float rise = rel.z * k;
    const float through = abs(rise) > 1e-4 ? (1.0 - exp(-rise)) / rise : 1.0;
    const float amount = 1.0 - exp(-atEye * distance * through);
    const float3 dir = rel / max(distance, 1e-3);
    return lerp(colour, gameSky(float3(dir.xy, max(dir.z, 0.0) * 0.3)), saturate(amount));
}

// Albedo under sun, sky and bounce; a warm rim for the selected one.
float3 gameShade(float3 albedo, float3 n, float3 rel, float highlight) {
    const float facing = dot(n, lightSun);
    // Wrapped a little: low-poly faces turned from the sun should not go black.
    float3 light = lightSunColour * saturate(facing * 0.8 + 0.2) * 0.95;
    light += lerp(lightGround, lightZenith, n.z * 0.5 + 0.5) * 0.75;
    light += lightSunColour * float3(1.0, 0.86, 0.70) * 0.10;
    // AL-45: by night a cold moon from above, so a house or a person still
    // stands out against the dark ground instead of sinking into it.
    const float night = 1.0 - lightSunAbove;
    light += float3(0.20, 0.23, 0.32) * night * saturate(n.z * 0.45 + 0.65);
    float3 c = albedo * light;
    const float rim = pow(1.0 - saturate(dot(n, normalize(-rel))), 2.0);
    c += float3(1.0, 0.80, 0.35) * rim * highlight * 0.9;
    return gameFog(c, rel);
}
