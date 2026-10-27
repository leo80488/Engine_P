// Vertex shader of the inverted-hull outline pass. Extrudes vertices along
// world-space normals in clip space by a constant pixel width. Distance fade: alpha
// ramps to zero between fadeStart and fadeEnd; beyond fadeEnd the vertex collapses
// behind the near plane and the GPU clips the triangle.

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint outlinePixelsBits;  // asfloat() -> per-draw outline thickness in pixels
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// Pre-TAA composite: the outline rasterizes alongside the main pass at the same
// jittered NDC, so TAA reprojects and history-blends both together and the outline
// never desyncs from the mesh in motion. The tail rows are declared so
// gInstanceRemapIdx lands at its fixed byte offset 228.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // current, jittered (matches GBuffer raster)
    float4x4 prevViewProj;         // unused here
    float4x4 curViewProjNoJitter;  // unused here
    float4   gTimeParams;          // 192 (unused here)
    float4   gCameraPosWS;         // 208 (unused here)
    uint     gDissolveNoiseIdx;    // 224 (unused here)
    uint     gInstanceRemapIdx;    // 228: stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;         // 232..239
};

cbuffer OutlineCB : register(b2, space0)
{
    float    outlinePixels;
    float    depthThreshold;
    float    normalThreshold;
    float    outlineStrength;
    float3   outlineColor;
    float    outlineFadeStart;
    // Display extents, not the render-target extents (see the extrusion below).
    uint     outlinePxSpaceW;
    uint     outlinePxSpaceH;
    float    outlineFadeEnd;
    float    nearZ;
    float    farZ;
};

// Must match InvertedHull.ps.hlsl PSIn: uv + instanceSlot feed the per-instance
// dissolve clip so a dissolving mesh loses its rim in sync.
struct VSOut
{
    float4 sv       : SV_POSITION;
    float  distFade : TEXCOORD0;
    float2 uv       : TEXCOORD1;
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
    float4x4 world    = InstanceBuffer[instSlot].world;

    float4 wPos = mul(float4(localPos, 1.0f), world);
    // GPU-TDR safety net (see GBuffer.vs.hlsl): collapse a non-finite / absurd
    // vertex to the instance origin so the outline hull can't rasterize a
    // screen-spanning triangle and hang the GPU. (The distFade early-out below
    // catches inf/huge but NOT NaN, so guard here.)
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))  // NaN / inf / |pos| > 1e5; fast-math-proof
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);
    float3 wNrm = normalize(mul(float4(localNrm, 0.0f), world).xyz);
    float4 clipPos = mul(wPos, viewProj);

    // clipPos.w = view-space depth (linear distance from camera).
    float dist     = clipPos.w;
    float distFade = 1.0 - saturate((dist - outlineFadeStart) / max(outlineFadeEnd - outlineFadeStart, 0.01));

    VSOut o;
    o.uv           = uv0;
    o.instanceSlot = instSlot;   // remapped; the PS dissolve clip reads it

    // Beyond fadeEnd: collapse behind the near plane -> the GPU clips the triangle.
    if (distFade <= 0.0)
    {
        o.sv       = float4(0, 0, -1, 0);
        o.distFade = 0;
        return o;
    }

    // Extrude along the normal in clip space. The extents are the display size, so
    // `pixels` means display pixels: the NDC offset 2 x pixels / W rasterizes at render
    // width Wr as pixels x Wr / W render pixels, i.e. pre-divided by the upscale ratio
    // so FSR2/DLSS restore it to `pixels`. Do not feed the render extents here: the rim
    // would thicken with upsampling.
    float4 clipNrm   = mul(float4(wNrm, 0.0f), viewProj);
    float2 nrmNDC    = clipNrm.xy / max(abs(clipPos.w), 1e-5f);
    float2 scaledNrm = nrmNDC * float2((float)outlinePxSpaceW, (float)outlinePxSpaceH);
    float  screenLen = length(scaledNrm);

    [flatten]
    if (screenLen > 1e-4f)
    {
        // Constant screen-space width regardless of distance;
        // distFade only affects alpha (pixel shader), not extrusion.
        float pixels = asfloat(outlinePixelsBits);
        float2 ndcOffset = (scaledNrm / screenLen)
                           / float2((float)outlinePxSpaceW, (float)outlinePxSpaceH)
                           * (2.0f * pixels);
        clipPos.xy += ndcOffset * clipPos.w;
    }

    o.sv       = clipPos;
    o.distFade = distFade;
    return o;
}
