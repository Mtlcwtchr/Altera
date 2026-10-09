#ifndef GROUND_FRAME_HLSLI
#define GROUND_FRAME_HLSLI

#include "world_frame.hlsli"

// A point of the ground as the material reads it (world_frame.hlsli): an exact
// frame origin and the metres from it. `local.z` is the height itself - heights
// are small enough for a float to hold to a fraction of a millimetre.
struct GroundFrame {
    float2 anchor;
    float3 local;
};

GroundFrame groundFrame(float2 worldXY, float2 frameXY, float height)
{
    GroundFrame f;
    f.anchor = frameAnchorFrom(worldXY, frameXY);
    f.local = float3(frameXY, height);
    return f;
}

// A world position taken as it is (frame origin nought): what callers without
// an interpolated offset - probes, tests - always had.
GroundFrame groundFrameOfWorld(float3 world)
{
    GroundFrame f;
    f.anchor = float2(0.0, 0.0);
    f.local = world;
    return f;
}

// The world position again, for smooth fields and metre-scale noise only.
float3 groundFrameWorld(GroundFrame f)
{
    return float3(f.anchor + f.local.xy, f.local.z);
}

#endif
