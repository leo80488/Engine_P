// Shadow.ps.hlsl - depth-only PS for shadow map rendering.
// The ALPHA_TEST permutation clips with the GBuffer's coverage rule (alpha_coverage.hlsli) on the
// default surface's opacity, so a masked material's shadow is the silhouette the camera sees; a
// masked CUSTOM material runs its own hook instead (custom_surface.hlsli's SHADOW_HOOK main,
// RenderGraph/RenderPass/ShadowCasterShaders.h). Both variants run the per-instance dissolve clip,
// so a dissolving mesh stops casting in lockstep with its GBuffer holes (CSM / spot / point /
// per-object player + hair maps all share this PS).

#include "material.hlsli"
#include "dither.hlsli"
#include "gpu_instance.hlsli"
#include "dissolve.hlsli"

cbuffer PushConstants : register(b0, space0)
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIndex;
};

// Per-instance dissolve params (root SRV t0 space0, VISIBILITY_ALL; every shadow pass binds it at
// the shared instance-buffer slot).
StructuredBuffer<GPUInstanceData> InstanceBuffer : register(t0, space0);

// Shadow cascade CB (256-byte slot: VP + planes + time/camera). Offset 224 carries the global
// dissolve noise bindless index; mirrors ShadowPerViewCB in material_bindings.hlsli.
cbuffer ShadowPerViewCB : register(b1, space0)
{
    float4x4 _sdShadowViewProj;    // 0
    float4   _sdPad[8];            // 64..191
    float4   _sdTimeParams;        // 192
    float4   _sdCameraPosWS;       // 208
    uint     gDissolveNoiseIdx;    // 224 (~0u = none)
    uint3    _sdDissolvePad;       // 228..239
};

// Bindless textures + sampler: the dissolve clip needs them in EVERY group, the ALPHA_TEST branch
// too, so the shadow passes bind them for all indirect groups (not only GRP_ALPHATEST).
Texture2D    g_AllTextures[] : register(t0, space2);
#include "sampler_heap.hlsli"   // samplers from the sampler heap
#define g_LinearWrap SmpFixed(SMP_LINEAR_WRAP)

#if ALPHA_TEST
StructuredBuffer<MaterialGPUData> g_Materials : register(t2, space0);
#include "alpha_coverage.hlsli"   // after g_AllTextures / g_LinearWrap (its contract)
#endif

// MUST match Shadow.vs.hlsl VSOut / custom_vertex.hlsli CUSTOM_VS_SHADOW CV_Out.
struct PSIn
{
    float4 sv : SV_POSITION;
    float2 uv : TEXCOORD0;
    nointerpolation uint instanceSlot : INSTSLOT;
};

void main(PSIn i)
{
    // Per-instance dissolve: a uniform (per-instance) branch, free when off.
    const float4 dparams = InstanceBuffer[i.instanceSlot].dissolveParams;
    [branch]
    if (dparams.x > 0.0 && gDissolveNoiseIdx != 0xFFFFFFFFu)
        DissolveClipEdge(g_AllTextures[gDissolveNoiseIdx], g_LinearWrap,
                         dparams, i.uv);

#if ALPHA_TEST
    MaterialGPUData mat = g_Materials[materialIndex];
    // Material UV transform: must match custom_surface.hlsli's MS_MakeParams, or a tiled cutout
    // casts a different silhouette than it renders. The dissolve clip above keeps the RAW uv on
    // purpose: its noise has its own scale and is not a material-texture lookup.
    const float2 muv = MaterialUV(mat, i.uv);
    // THE GBUFFER'S COVERAGE, not a shadow-only one. Opacity is the default surface's - baseColor.a x
    // BASECOLORMAP.a (material_surface.hlsli; an unbound slot reads as the GBuffer's white fallback,
    // alpha 1) - times the Masked-mode mask, and the clip is the same Golus edge (alpha_coverage.hlsli).
    // A material whose baseColor.a is below its alphaRef casts nothing, as the GBuffer draws nothing.
    float a = mat.baseColor.a;
    const int texBaseColor = mat.textureHandleIds[0];   // BASECOLORMAP
    if (texBaseColor >= 0)
        a *= g_AllTextures[texBaseColor].Sample(g_LinearWrap, muv).a;
    a = AC_ApplyOpacityMask(a, mat.textureHandleIds[MAT_TEX_OPACITYMAP], muv);
    [branch]
    if ((mat.materialFlags & MAT_FLAG_DITHERED_COVERAGE) != 0u)
    {
        // BlendMode::Dithered: a STATIC screen door in shadow-map space, so the PCF-averaged density
        // equals the coverage without flicker. Shadow maps are not temporally filtered; the GBuffer's
        // per-frame phase (TemporalDither) would crawl here.
        clip(a - StaticDither(uint2(i.sv.xy)));
    }
    else
    {
        AC_ClipGolus(a, mat.alphaRef);
    }
#endif
}
