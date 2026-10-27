// FaceOverlay.vs.hlsl - brows/lashes-through-hair overlay VS.
//
// Port of DanbaidongRP's CharacterEyelashOutter: the eyebrow/eyelash meshes are
// drawn a SECOND time AFTER opaque lighting, alpha-blended over the HDR scene with
// NO depth test (the PSO disables it), so they show through the fringe. The PS
// limits the redraw to pixels whose visible surface is HAIR and fades it by the
// lash -> hair depth gap, so it never draws over the environment.
//
// Uses the same PVF fetch + jittered PerViewCB as GBuffer.vs, so the overlay lands
// pixel-exact on this frame's GBuffer / depth. Skinned lashes work for free
// (FETCH_POS reads the skinned vertex stream).

#include "pvf_fetch.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint packedOverlayParams; // consumed by the PS; the fourth root DWORD
};

#include "gpu_instance.hlsli"
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

// Same "PerView" buffer GBuffer.vs binds at b1 (BindCBByName(0, "PerView")).
// Tail rows declared so gInstanceRemapIdx lands at its fixed byte offset 228.
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // JITTERED - used for SV_POSITION only
    float4x4 prevViewProj;         // unused here
    float4x4 curViewProjNoJitter;  // unused here
    float4   gTimeParams;          // 192 (unused here)
    float4   gCameraPosWS;         // 208 (unused here)
    uint     gDissolveNoiseIdx;    // 224 (unused here)
    uint     gInstanceRemapIdx;    // 228 stable-slot remap heap index (0 = identity)
    uint2    _gPerViewPad;         // 232..239
};

struct VSOut
{
    float4 sv       : SV_POSITION;
    float3 worldPos : POSITIONWS;
    float3 wn       : NORMAL;
    float2 uv       : TEXCOORD0;
};

VSOut main(uint rawID : SV_VertexID, uint instID : SV_InstanceID)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];
    uint vid = FetchIndex(md, rawID);

    float3   localPos = FETCH_POS(md, vid);
    float3   localNrm = FETCH_NORMAL(md, vid);
    float2   uv0      = FETCH_UV0(md, vid);
    uint     instSlot = ResolveInstanceSlot(gInstanceRemapIdx, instanceOffset + instID);
    float4x4 world    = InstanceBuffer[instSlot].world;
    float4   wPos     = mul(float4(localPos, 1.0f), world);

    // GPU-TDR safety net (the reason is at GBufferVertex in gbuffer_vertex.hlsli).
    if (!(dot(wPos.xyz, wPos.xyz) < 1e10f))
        wPos = float4(world[3][0], world[3][1], world[3][2], 1.0f);

    VSOut o;
    o.sv       = mul(wPos, viewProj);
    o.worldPos = wPos.xyz;
    o.wn       = normalize(mul(localNrm, (float3x3)world));
    o.uv       = uv0;
    return o;
}
