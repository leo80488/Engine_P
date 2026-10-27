// material_bindings.hlsli - the engine root-signature bindings shared by every material
// shader stage (vertex + pixel). Declared in one place so a unified single-file material
// (custom_material.hlsli) compiles for both stages without double-declaring registers, and
// the vertex, pixel and unified templates agree on the layout.
// Every binding is a slot the GBuffer/Transparent/Shadow root signatures already expose.
// DXC dead-strips the bindings a stage does not read (MeshDescriptors in the PS), so each
// stage pays only for what it reads.

#ifndef MATERIAL_BINDINGS_HLSLI
#define MATERIAL_BINDINGS_HLSLI

#include "material.hlsli"       // MaterialGPUData + MAT_* / SHADING_MODEL_*
#include "pvf_fetch.hlsli"      // MeshDescriptor + FETCH_* (programmable vertex fetch)
#include "gpu_instance.hlsli"   // GPUInstanceData (world + prevWorld)

// [0] ROOT_CONSTANTS b0 space0 - 4 values; the PS only reads materialIndex, the VS also
//     reads meshDescIdx / instanceOffset. Declaring all four matches the root-constant count
//     the engine sets; unread members are stripped per stage.
cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
    uint prevPosInfo;     // prev-pos base element (0xFFFFFFFF = none); velocity reads md.prevPosition, not this
};

// [1] ROOT_CBV b1 space0 - PerViewCB (VISIBILITY_ALL, so the PS reads cameraPos).
//     gTimeParams/gCameraPosWS sit at a fixed byte offset (192/208) in both the GBuffer and
//     the shadow layout, so a vertex hook reads identical time/camera in the GBuffer pass
//     and in a depth-only shadow pass.
#if CUSTOM_VS_SHADOW
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 shadowViewProj;       // 0   per-cascade / per-caster / per-face VP
    float4   _cvShadowPad[8];      // 64..191 (CSM Gribb-Hartmann planes; unused here)
    float4   gTimeParams;          // 192 x=time y=deltaTime z=sin w=cos
    float4   gCameraPosWS;         // 208 xyz = camera world position
    uint     gDissolveNoiseIdx;    // 224 engine-global dissolve noise bindless index (~0u = none)
    uint     gInstanceRemapIdx;    // 228 instance remap-buffer heap index (0 = identity)
    uint2    _gDissolvePad;        // 232..239; no gFrameIndex on purpose:
                                   // shadow passes dither STATICALLY
                                   // (dither.hlsli StaticDither), the CSM is
                                   // texel-snapped and never temporally filtered
};
#else
cbuffer PerViewCB : register(b1, space0)
{
    float4x4 viewProj;             // 0   jittered - SV_POSITION only
    float4x4 prevViewProj;         // 64  previous frame, unjittered (velocity)
    float4x4 curViewProjNoJitter;  // 128 current frame, unjittered (velocity)
    float4   gTimeParams;          // 192 x=time y=deltaTime z=sin w=cos
    float4   gCameraPosWS;         // 208 xyz = camera world position
    uint     gDissolveNoiseIdx;    // 224 engine-global dissolve noise bindless index (~0u = none)
    uint     gInstanceRemapIdx;    // 228 instance remap-buffer heap index (0 = identity)
    uint     gFrameIndex;          // 232 presented-frame counter - temporal dither phase
    uint     _gDissolvePad;        // 236
};
#endif

// Instance/mesh SRVs (t0/t1 space0). InstanceBuffer is VISIBILITY_ALL: the PS reads
// per-instance dissolve params via the VS-forwarded instanceSlot interpolant.
// MeshDescriptors stays VS-only (stripped in the PS).
StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

// Pixel-side material data + textures.
StructuredBuffer<MaterialGPUData> g_Materials    : register(t2, space0);
Texture2D<float4>                 g_BaseColor     : register(t3, space0); // per-draw fallback
Texture2D<float4>                 g_SurfaceMap    : register(t4, space0);
Texture2D<float4>                 g_NormalMap     : register(t5, space0);
Texture2D                         g_AllTextures[] : register(t0, space2); // bindless
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearWrap SmpDynamic()   // material sampler: anisotropy + upscale mip bias in the main view's material passes; LinearWrap where shadow / surface-cache / probe passes draw the same materials

#endif // MATERIAL_BINDINGS_HLSLI
