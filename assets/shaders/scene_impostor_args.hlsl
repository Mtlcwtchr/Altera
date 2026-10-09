// The impostor pool's draw arguments (scene_impostor_pool.hlsl): set before
// the pool counts into them, held to the capacity after. Its own file so the
// arguments are this stage's only write, at u0: a stage's storage slots are
// counted from what it uses, and beside an unused instance buffer they would
// sit one slot past where the dispatch binds them.
RWByteAddressBuffer arguments : register(u0, space1);

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

[numthreads(1, 1, 1)]
void ClearCS(uint3 id : SV_DispatchThreadID)
{
    arguments.Store(0, asuint(card.x));
    arguments.Store(4, 0u);
    arguments.Store(8, asuint(card.y));
    arguments.Store(12, asuint(card.z));
    arguments.Store(16, 0u);
}

// After the pool: the count the draw reads never past what was written.
[numthreads(1, 1, 1)]
void ClampCS(uint3 id : SV_DispatchThreadID)
{
    arguments.Store(4, min(arguments.Load(4), asuint(view.y)));
}

