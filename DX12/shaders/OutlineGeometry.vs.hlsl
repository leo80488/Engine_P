// Inverted-hull outline as first-class geometry: drawn in the geometry phase into
// the same 6-MRT GBuffer, so the rim writes depth and velocity like any other
// surface and needs no special casing downstream. OutlinePass skips its own
// composite-style hull (InvertedHull.vs/ps: colour only, after lighting) while this
// pass draws (hullSuppressed); that hull gives TAA no velocity to reproject the rim
// and no depth change for its disocclusion detector, so a moving character leaves
// outline-shaped trails.
//
// Distance fade shrinks the extrusion width, not alpha: a GBuffer has no alpha
// blending, and distant characters get a thinner line instead of a translucent one
// (black at 50% alpha reads as muddy grey). Sub-pixel widths resolve smoothly
// because TAA averages the partial coverage across its jitter sequence.
//
// A thin shell is what makes writing depth safe: ~20 passes read this depth buffer
// (SSR, XeGTAO, fog, particles, ...), and at the authored 1-2 px widths the
// silhouette error is sub-pixel to them.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    // Same slot, same meaning as GBuffer.vs: base element index of this draw's
    // previous-frame skinned positions (0xFFFFFFFF = static). The outline WIDTH
    // rides GPUInstanceData.dissolveEdgeColor.w instead, because root param 0
    // only has four dwords and velocity needs this one.
    uint prevPosInfo;
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // current, JITTERED (matches GBuffer raster)
    float4x4 prevViewProj;         // previous frame, UNJITTERED (velocity)
    float4x4 curViewProjNoJitter;  // current frame, UNJITTERED (velocity)
    float4   gTimeParams;
    float4   gCameraPosWS;
    uint     gDissolveNoiseIdx;
    uint     gInstanceRemapIdx;    // 228: stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;
};

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;        // global default; per-draw value rides the instance
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    // The resolution outlinePixels is measured in: the display size, not the raster
    // size. Under FSR2/DLSS they differ, and that is the point (see ExtrudeClip).
    uint     outlinePxSpaceW;
    uint     outlinePxSpaceH;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

// Pushes a clip-space position outward along its world normal by a constant number
// of screen pixels (the rim keeps its width at any distance). Apply it to the
// jittered raster position and to both unjittered velocity positions with the same
// magnitude, or the motion vector describes the surface under the rim instead of
// the rim.
// vpW / vpH are the pixel space `pixels` is measured in: the display extents, not
// the render-target extents. The NDC offset 2 x pixels / vpW rasterizes at render
// width Wr as pixels x Wr / vpW render pixels, which the upscaler scales back to
// exactly `pixels` display pixels. Do not pass the render extents: the upscaler
// would multiply the width by the upscale ratio (a 2 px outline 4 px thick at DLSS
// Performance). At native / DLAA the two extents are equal.
void ExtrudeClip(inout float4 clipPos, float3 wNrm, float4x4 vp,
                 float pixels, uint vpW, uint vpH)
{
    const float4 clipNrm   = mul(float4(wNrm, 0.0f), vp);
    const float2 nrmNDC    = clipNrm.xy / max(abs(clipPos.w), 1e-5f);
    const float2 scaledNrm = nrmNDC * float2((float)vpW, (float)vpH);
    const float  screenLen = length(scaledNrm);

    [flatten]
    if (screenLen > 1e-4f)
    {
        const float2 ndcOffset = (scaledNrm / screenLen)
                               / float2((float)vpW, (float)vpH) * (2.0f * pixels);
        clipPos.xy += ndcOffset * clipPos.w;
    }
}

// MUST match OutlineGeometry.ps.hlsl PSIn.
struct VSOut
{
    float4 sv       : SV_POSITION;
    float3 wn       : TEXCOORD0;   // world normal: RT1 needs a real normal
    float2 uv       : TEXCOORD1;
    float4 curClip  : TEXCOORD2;   // UNJITTERED current clip pos (velocity)
    float4 prevClip : TEXCOORD3;   // UNJITTERED previous clip pos (velocity)
    nointerpolation uint instanceSlot : INSTSLOT;
};

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md  = MeshDescriptors[meshDescIdx];
    uint           vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float3   localNrm = FETCH_NORMAL(md, vid);
    float2   uv0      = FETCH_UV0(md, vid);
    uint     instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);

    GPUInstanceData inst = InstanceBuffer[instSlot];
    float4x4 world = inst.world;

    float4 wPos = mul(float4(localPos, 1.0f), world);
    // GPU-TDR safety net (mirrors GBuffer.vs): collapse a non-finite / absurd vertex
    // to the instance origin so a corrupt pose can't rasterize a screen-spanning
    // triangle and hang the GPU.
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);

    float3 wNrm    = normalize(mul(float4(localNrm, 0.0f), world).xyz);
    float4 clipPos = mul(wPos, viewProj);

    // clipPos.w = view-space depth (linear distance from camera).
    const float dist     = clipPos.w;
    const float distFade = 1.0 - saturate((dist - outlineFadeStart)
                                          / max(outlineFadeEnd - outlineFadeStart, 0.01));

    VSOut o;
    o.uv           = uv0;
    o.wn           = wNrm;
    o.instanceSlot = instSlot;

    // Beyond fadeEnd the width is zero anyway; collapse behind the near plane so
    // the GPU clips the triangle instead of rasterizing a degenerate sliver.
    if (distFade <= 0.0)
    {
        o.sv       = float4(0, 0, -1, 0);
        o.curClip  = float4(0, 0, -1, 0);
        o.prevClip = float4(0, 0, -1, 0);
        return o;
    }

    // ---- Extrusion, identical for the current and previous clip positions ----
    // The per-draw width rides the instance record (see the PushConstants note);
    // falling back to the CB global keeps a zero/unwritten record from erasing the rim.
    float pixels = inst.dissolveEdgeColor.w;
    if (!(pixels > 0.0)) pixels = outlinePixels;
    pixels *= distFade;                     // width fade, not alpha

    // Velocity must be measured between the two UNJITTERED projections, or a
    // static camera reads as full-pixel motion every frame.
    float4 curClipNJ = mul(wPos, curViewProjNoJitter);

    float4x4 prevWorld = inst.prevWorld;
    float4   prevWPos;
    if (md.prevPosition.bufferIndex != 0xFFFFFFFFu)
    {
        // Skinned: the previous-frame skinned position in mesh-local space, from last
        // frame's buffer: md.prevPosition names the previous vertex-ring slot, which still
        // holds last frame's skinned output (patched beside md.position every frame).
        // Gated on bufferIndex, not on the prevPosInfo root constant: OutlinePass (picking
        // flag / outline width) and FaceOverlayPass alias that DWORD when they run this VS,
        // so it is neither a reliable base nor a reliable sentinel.
        float3 prevLocalPos = FetchAsFloat3(md.prevPosition.bufferIndex,
                                            md.prevPosition.byteOffset + vid * 12u,
                                            md.prevPosition.format);
        prevWPos = mul(float4(prevLocalPos, 1.0f), prevWorld);
    }
    else
    {
        prevWPos = mul(float4(localPos, 1.0f), prevWorld);
    }
    float4 prevClipNJ = mul(prevWPos, prevViewProj);

    // The rim is offset from the silhouette, so its motion vector must describe the
    // rim's movement, not the surface underneath: extrude all three clip positions by
    // the same screen-space amount along their own normals. The previous-frame normal
    // uses prevWorld so a rotating object's rim tracks.
    float3 prevWNrm = normalize(mul(float4(localNrm, 0.0f), prevWorld).xyz);

    ExtrudeClip(clipPos,    wNrm,     viewProj,            pixels, outlinePxSpaceW, outlinePxSpaceH);
    ExtrudeClip(curClipNJ,  wNrm,     curViewProjNoJitter, pixels, outlinePxSpaceW, outlinePxSpaceH);
    ExtrudeClip(prevClipNJ, prevWNrm, prevViewProj,        pixels, outlinePxSpaceW, outlinePxSpaceH);

    o.sv       = clipPos;
    o.curClip  = curClipNJ;
    o.prevClip = prevClipNJ;
    return o;
}
