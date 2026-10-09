// The placed objects as impostors, decided on the card every frame
// (SceneModelsPass, the impostor pool).
//
// Every published object lives in one buffer on the card from the moment the
// placement publishes it until it is let go: nothing about it is uploaded per
// frame. Each frame one thread an object culls it to the view, sizes it on
// the screen and, if it is an impostor, writes the card instance the scene
// model shader draws (SceneModelsPass::Instance) and counts it into the one
// indirect draw. The objects near enough to be meshes (within their model's
// radius, which the processor works out and also uses to pick its own) are
// the processor's: it draws them, impostor or mesh, with their dissolves.
//
// What decides here is what decides on the processor for the same object
// (collect): the frustum, the pixel size (objectLod's coverage), the
// placement's fade in, the hand-over to the far trees, the pair of baked views
// (impostorPair) and the hemisphere for a model that has one.

StructuredBuffer<float4> sources : register(t0, space0);   // three a object
StructuredBuffer<float4> models : register(t1, space0);    // two a model
RWStructuredBuffer<float4> instances : register(u0, space1); // four an instance
RWByteAddressBuffer arguments : register(u1, space1);

cbuffer Pool : register(b0, space2)
{
    float4 rowX;
    float4 rowY;
    float4 rowW;
    float4 eye;        // xyz the eye, w the focal length in pixels (0: orthographic)
    float4 counts;     // x pixels a metre (orthographic), y objects (bits), z hand-over, w its band
    float4 view;       // x the placement's fade in, y capacity (bits), z the screen's right (orthographic), w -
    float4 facing;     // xyz the direction to the eye (orthographic)
    float4 card;       // x index count, y first index, z vertex offset (bits)
};

static const float kTwoPi = 6.283185307179586;

float smoothValue(float a, float b, float x)
{
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0 - 2.0 * t);
}

bool visible(float3 centre, float radius)
{
    const float4 planes[5] = {rowW + rowX, rowW - rowX, rowW + rowY, rowW - rowY, rowW};
    [unroll] for (int p = 0; p < 5; ++p)
        if (dot(planes[p].xyz, centre) + planes[p].w + radius * length(planes[p].xyz) < 0.0) return false;
    return true;
}

[numthreads(64, 1, 1)]
void PoolCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= asuint(counts.y)) return;
    const float4 s0 = sources[id.x * 3];       // position, scale
    const float4 s1 = sources[id.x * 3 + 1];   // yaw, phase, tint, surface
    const float4 s2 = sources[id.x * 3 + 2];   // model (bits)
    const uint model = asuint(s2.x);
    const float4 m0 = models[model * 2];       // width, height, extent, impostor
    const float4 m1 = models[model * 2 + 1];   // vegetation, depth impostor, hemisphere, processor radius
    const float3 position = s0.xyz;
    const float scale = s0.w;
    const bool perspective = eye.w > 0.0;

    // The processor's own: near enough that it may be a mesh.
    const float flat = length(position.xy - eye.xy);
    if (flat < m1.w) return;
    if (perspective && flat >= counts.z) return;

    const float3 centre = position + float3(0.0, 0.0, m0.y * 0.5 * scale);
    if (!visible(centre, length(float2(m0.x * 0.5, m0.y * 0.5)) * scale)) return;
    const float depth = dot(rowW.xyz, centre) + rowW.w;
    const float pixelsPerMetre = perspective ? eye.w / max(depth, 1e-3) : counts.x;
    const float pixels = m0.z * scale * pixelsPerMetre;
    if (!(pixels > 0.0)) return;
    // objectLod's coverage, the placement's fade in and the far trees' band.
    float coverage = smoothValue(0.8, 2.0, pixels) * view.x;
    if (perspective && counts.w > 0.0) coverage *= 1.0 - smoothValue(counts.z - counts.w, counts.z, flat);
    if (coverage <= 0.0) return;

    const float3 direction = perspective ? normalize(eye.xyz - centre) : facing.xyz;
    const float right = perspective ? atan2(direction.y, direction.x) - kTwoPi * 0.25 : view.z;
    const float turns = (right - s1.x) / kTwoPi;
    const float exact = (turns - floor(turns)) * 8.0;
    const float first = fmod(floor(exact), 8.0);
    const float next = fmod(first + 1.0, 8.0);
    float layer = m0.w + first, layerNext = m0.w + next, blend = exact - floor(exact);
    float mode = m1.y > 0.5 ? 2.0 : 1.0;
    // The hemisphere, seen from anywhere but far below the object's middle.
    if (m1.z >= 0.0 && direction.z >= -0.45) {
        mode = 3.0;
        layer = layerNext = m1.z;
        blend = 0.0;
    }

    uint slot;
    arguments.InterlockedAdd(4, 1u, slot);
    if (slot >= asuint(view.y)) return;
    instances[slot * 4] = float4(position, scale);
    instances[slot * 4 + 1] = float4(s1.x, s1.y, s1.z, m1.x > 0.5 ? 1.0 : -saturate(s1.w));
    instances[slot * 4 + 2] = float4(m0.x, m0.y, layer, 0.0);
    instances[slot * 4 + 3] = float4(mode, coverage, layerNext, blend);
}
