// TransmissionMotion.cs.hlsl - motion vectors for what the Transmissive surfaces show
// (TransmissionMotionPass): the background each covered pixel refracts, not the opaque pixel
// straight behind it.
//
// The velocity buffer carries the opaque surface behind each pixel. A solid or rough layer shows
// the background at its offset instead, and the offset itself changes when the layer turns, moves
// or is seen from elsewhere, so a temporal resolve that reprojects with the buffer's motion smears
// what the glass shows. With P the pixel, o(P) its offset now (TransmissionBackUV, as the apply
// resolves it), Q = P + o(P) the background it shows and v(Q) that background's own motion: the
// background was at Q - v(Q) last frame and showed through the pixel P' with P' + o'(P') = Q - v(Q),
// o' the last frame's offsets. The motion is
//     P - P' = v(Q) - o(P) + o'(P'),
// solved by two fixed-point steps from P' = P - v(Q). A thin layer has no offset, and its pixel
// keeps the buffer's own value. Velocity is MS_Velocity's: unjittered NDC, current minus previous;
// offsets are differences of one frame's projections, so the jitter cancels in them.
//
// CSMotion writes this frame's offsets (0 where no layer is) and every covered pixel's motion;
// CSWrite copies the motions into the velocity buffer. Two dispatches: CSMotion reads the buffer at
// Q while other pixels' motions are made.
//
// Compute root signature (space2):
//   b0 - TransmissionMotionCB
//   t0 - Transmission_Params (offset.xy in UV, spread, layers)
//   t1 - Transmission_LayerDepth (the nearest layer's reverse-Z depth)
//   t2 - the scene depth (reverse-Z, the sky 0)
//   t3 - the last frame's offsets (NDC), read when historyValid
//   t4 - velocity (R16G16_FLOAT, GBuffer3), CSMotion
//   t5 - the motions, CSWrite
//   u0 - velocity, CSWrite
//   u1 - this frame's offsets (NDC), CSMotion
//   u2 - the motions (NDC), CSMotion

#include "transmission_back.hlsli"

cbuffer TransmissionMotionCB : register(b0, space2)
{
    uint width;
    uint height;
    uint historyValid;
    uint _pad;
};

Texture2D<float4>   gParams     : register(t0, space2);
Texture2D<float>    gLayerDepth : register(t1, space2);
Texture2D<float>    gSceneDepth : register(t2, space2);
Texture2D<float2>   gPrevOffset : register(t3, space2);
Texture2D<float2>   gVelocityIn : register(t4, space2);
Texture2D<float2>   gMotionIn   : register(t5, space2);
RWTexture2D<float2> gVelocity   : register(u0, space2);
RWTexture2D<float2> gOffset     : register(u1, space2);
RWTexture2D<float2> gMotion     : register(u2, space2);

[numthreads(8, 8, 1)]
void CSMotion(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;
    const int2   px     = int2(dtid.xy);
    const float4 params = gParams.Load(int3(px, 0));
    if (params.w <= 0.0)
    {
        gOffset[px] = float2(0.0, 0.0);
        return;
    }

    const float2 size = float2(width, height);
    const float2 uv   = (float2(px) + 0.5) / size;
    float backZ;
    const float2 uvBack = TransmissionBackUV(uv, params.xy, gLayerDepth.Load(int3(px, 0)), gSceneDepth, size, backZ);
    // UV to NDC: x doubles, y doubles and flips.
    const float2 offset = (uvBack - uv) * float2(2.0, -2.0);
    gOffset[px] = offset;

    const int2   pxBack = clamp(int2(uvBack * size), int2(0, 0), int2(size) - 1);
    const float2 vBack  = gVelocityIn.Load(int3(pxBack, 0));
    float2 motion = vBack;
    if (historyValid != 0)
    {
        [unroll] for (int k = 0; k < 2; ++k)
        {
            const int2 pxPrev = int2(floor((uv - motion * float2(0.5, -0.5)) * size));
            // Off screen last frame: nothing says the offset was different.
            const float2 prevOffset = all(pxPrev >= 0) && all(pxPrev < int2(width, height))
                                    ? gPrevOffset.Load(int3(pxPrev, 0)) : offset;
            motion = vBack - offset + prevOffset;
        }
    }
    gMotion[px] = motion;
}

[numthreads(8, 8, 1)]
void CSWrite(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= width || dtid.y >= height)
        return;
    const int2 px = int2(dtid.xy);
    if (gParams.Load(int3(px, 0)).w > 0.0)
        gVelocity[px] = gMotionIn.Load(int3(px, 0));
}
