// UV-space rasterisation VS for the material surface cache.
// Same programmable-vertex-fetch chain as GBuffer.vs.hlsl, except that SV_Position
// comes from uv0 instead of the view-projection, so each mesh section rasterises
// into its own UV space. Everything downstream (the material's custom pixel shader,
// the SurfaceData contract, the 6-MRT encode) is the GBuffer path, so the cached
// look comes from the material's own shader, not a guess from texture names.
//
// It must emit every PSIn element, including `nointerpolation uint instanceSlot :
// INSTSLOT`: MS_MakeParams reads InstanceBuffer[instanceSlot].world (object axes /
// origin) and MS_ApplyDissolve reads .dissolveParams, so a VS one element short is
// a signature mismatch, not a missing feature.
//
// Lossy by design: worldPos is synthesised as gCameraPosWS - wn, so MS_MakeParams'
// viewDirWS = normalize(gCameraPosWS - worldPos) is exactly the normal and the
// material is evaluated head-on. That is the neutral choice for an albedo cache (as
// in Lumen's surface cache): toon fresnel falls to its inside tint, matcap to its
// centre texel, rim / sheen to ~0, since those are shading responses the ray
// shader's lighting would otherwise count twice.
// The price: a hook that reads p.worldPos directly (world triplanar, distance fade,
// world noise) sees a point one unit in front of the camera. viewDirWS derives from
// worldPos, so both cannot be right; viewDirWS wins because the NPR character
// shaders consume it and almost none read world position.

#include "pvf_fetch.hlsli"
#include "core/sampling.hlsli"   // TangentFrameRefY
#include "gpu_instance.hlsli"

// ==== Root parameter bindings (identical to GBuffer.vs.hlsl) ====

// [0] ROOT_CONSTANTS  b0 space0
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint prevPosInfo;     // unused here (velocity is discarded to scratch)
};

// [8] ROOT_SRV  t0 space0
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
// [9] ROOT_SRV  t1 space0
StructuredBuffer<MeshDescriptor> MeshDescriptors : register(t1, space0);

// [1] ROOT_CBV  b1 space0 - the same PerViewCB the GBuffer pass binds. viewProj is
// deliberately unread (the bake is in UV space); gCameraPosWS (byte 208) and
// gInstanceRemapIdx (byte 228) are live, and the rows before them are declared so
// those offsets are exact.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             //   0 UNUSED - the bake rasterises in UV space
    float4x4 prevViewProj;         //  64 UNUSED
    float4x4 curViewProjNoJitter;  // 128 UNUSED
    float4   gTimeParams;          // 192
    float4   gCameraPosWS;         // 208 xyz = camera world position
    uint     gDissolveNoiseIdx;    // 224
    uint     gInstanceRemapIdx;    // 228 stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;         // 232..239
};

// ==== Output: must match GBuffer.vs.hlsl / MaterialSurfaceVaryings ====

struct PSIn
{
    float4 sv        : SV_POSITION;
    float3 worldPos  : POSITIONWS;
    float2 uv        : TEXCOORD0;
    float3 wn        : NORMAL;
    float3 wt        : TANGENT;
    float3 wbt       : BINORMAL;
    float3 col       : COLOR;
    float2 uv1       : TEXCOORD3;
    float2 uv2       : TEXCOORD4;
    float2 uv3       : TEXCOORD5;
    float4 curClip   : TEXCOORD1;
    float4 prevClip  : TEXCOORD2;
    nointerpolation uint instanceSlot : INSTSLOT;
};

PSIn main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    uint vid = FetchIndex(md, rawID);

    // No FETCH_POS: a UV-space bake never needs the object-space position.
    float3 localNrm = FETCH_NORMAL(md, vid);
    float4 localTan = FETCH_TANGENT4(md, vid);
    float2 uv0      = FETCH_UV0(md, vid);
    float2 uv1      = FETCH_UV1(md, vid);
    float2 uv2      = FETCH_UV2(md, vid);
    float2 uv3      = FETCH_UV3(md, vid);
    float4 color    = FETCH_COLOR(md, vid);

    // The bake forces instanceCount = 1, so this resolves the batch's first instance.
    // Intended: everything the pixel side reads off the instance record (object axes /
    // origin / scale) is uniform over a page, because the page key is the batch key.
    uint instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world = InstanceBuffer[instSlot].world;

    // World-space TBN built exactly as in GBuffer.vs.hlsl: the material hook
    // normal-maps against it, so any difference makes the baked shading response
    // differ from the raster one.
    float3x3 w3 = (float3x3)world;
    float3   wn = normalize(mul(localNrm, w3));
    float3   wt, wbt;
    [branch]
    if (md.tangent.bufferIndex == INVALID_BUFFER)
    {
        // No authored tangent stream: build the basis from N alone (TangentFrameRefY).
        TangentFrameRefY(wn, wt, wbt);
    }
    else
    {
        wt  = normalize(mul(localTan.xyz, w3));
        wt  = normalize(wt - dot(wt, wn) * wn);
        wbt = cross(wn, wt) * localTan.w * InstanceHandednessSign(world);
    }

    PSIn o;
    // UV -> NDC, Y flipped (UV origin is top-left, NDC +Y is up). z = 0 is inside the
    // clip range for any depth convention, and the bake PSO has no depth test or depth
    // target, so reversed-Z does not matter here.
    o.sv = float4(uv0.x * 2.0 - 1.0, 1.0 - uv0.y * 2.0, 0.0, 1.0);

    // This exact expression makes the pixel side's viewDirWS equal wn (head-on
    // evaluation, see the file header).
    o.worldPos = gCameraPosWS.xyz - wn;

    o.uv           = uv0;
    o.wn           = wn;
    o.wt           = wt;
    o.wbt          = wbt;
    o.col          = color.rgb;
    o.uv1          = uv1;
    o.uv2          = uv2;
    o.uv3          = uv3;
    o.instanceSlot = instSlot;

    // Velocity goes to a scratch RT that is never resolved. Equal clip positions make
    // MS_Velocity return exactly (0,0), keeping the scratch target free of NaNs.
    o.curClip  = o.sv;
    o.prevClip = o.sv;
    return o;
}
