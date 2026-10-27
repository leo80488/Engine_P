#pragma once

// LightingPass - fullscreen deferred lighting: three stencil-gated PSO variants (PBR=1, NPR=2,
// Unlit=3) plus a camera-primary variant. Reads the GBuffer SRVs + depth, writes the HDR scene
// colour and the reflection-free reflection source.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's inputs)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"

#include <memory>

namespace RG { class RenderContext; }

class LightingPass : public RG::RenderPass
{
public:
    // The base name every instance's GetName() starts with, kept in one place: Renderer's
    // SetExternalWait("LightingPass", ...) is keyed on this exact spelling.
    static constexpr const char* kBaseName = "LightingPass";

    LightingPass(RG::RGTextureHandle albedo,
                 RG::RGTextureHandle normal,
                 RG::RGTextureHandle surface,
                 RG::RGTextureHandle depth,
                 RG::RGTextureHandle emissive,
                 RG::RGTextureHandle reflectionSource);
    // Frees nothing: a destructor runs at an arbitrary point with no device reference, where
    // freeing descriptors could be a use-after-free. Shutdown releases everything (its contract is
    // a quiesced boundary).
    ~LightingPass() override;

    // Instance name. RenderGraph keys the external-wait map, the PIX marker and the GPU timestamp
    // region on GetName(), and GetGPUTimestampResult is a first-match-wins name scan, so two
    // instances sharing a name would hand every consumer the union of both. Without a suffix it is
    // kBaseName.
    const char* GetName() const override { return m_name.c_str(); }
    // Call before the pass is handed to a RenderGraph (see GBufferPass.h).
    void SetNameSuffix(const char* suffix);

    void SetPathTraceResult(RG::RGTextureHandle result) { m_ptResult = result; }
    void SetCameraPrimary(bool enabled) { m_in->cameraPrimary = enabled; }
    void SetRestirDIResult(RG::RGTextureHandle result) { m_diResult = result; }
    void SetRestirGIResult(RG::RGTextureHandle result) { m_giResult = result; }
    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    // Releases everything this instance owns: its own static ShaderLibrary (a GBufferPass /
    // TransparentPass preview instance borrows one instead), the fallback textures and buffers and
    // the null reflection-probe SRV block.
    void Shutdown(IGraphicsDevice& gfx)        override;
    // PassSet events (the main graph never Destroys, so this is its only Shutdown). It records any view:
    // the inputs that are a view's (the AO, the traced reflection and sun shadow, the path tracer's
    // camera primary) are the bound view's.
    uint32_t HandledEvents() const override { return kShutdown | kView | kViewSafe; }
    // The main view keeps its inputs below; a secondary view keeps its own in its ViewState, none
    // set until something feeds it.
    void BindView(ViewState& view) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx)  override;

    // ---- Per-frame setters (called by the Renderer before each Execute) ----
    // Pass 0 to disable a contribution.

    // IBL cubemaps (irradiance + radiance) and BRDF LUT.
    void SetIBL(uint64_t irradianceHandle, uint64_t radianceHandle,
                uint32_t radianceMips, float iblStrength)
    {
        m_iblIrradianceHandle = irradianceHandle;
        m_iblRadianceHandle   = radianceHandle;
        m_iblRadianceMips     = radianceMips;
        m_iblStrength         = iblStrength;
    }
    void SetBRDFLUT(uint64_t lutHandle) { m_brdfLutHandle = lutHandle; }

    // Shadows: CSM cascade table + spot-shadow atlas/VPs.
    void SetShadowMap(uint64_t shadowHandle) { m_shadowSrvHandle = shadowHandle; }
    void SetSpotShadowAtlas(uint64_t atlasHandle, uint64_t vpsHandle)
    {
        m_spotShadowAtlasHandle = atlasHandle;
        m_spotShadowVPHandle    = vpsHandle;
    }
    // Point-light cube shadow atlas (TextureCubeArray<float>); 0 -> the 1x1 12-slice cube
    // fallback. Read bindless: see GetPointShadowAtlasSrv().
    void SetPointShadowAtlas(uint64_t cubeAtlasHandle)
    {
        m_pointShadowAtlasHandle = cubeAtlasHandle;
    }
    // Per-object (player) high-res shadow map (Texture2D<float>). 0 -> the 1x1 depth-0.0
    // fallback; the shader samples it only when LightCB.perObjShadowParams.x != 0.
    void SetPerObjectShadowMap(uint64_t mapHandle)
    {
        m_perObjShadowHandle = mapHandle;
    }
    // Hair (bangs -> face) shadow map (Texture2D<float>). Same contract; the shader samples it only
    // on NPR face pixels when LightCB.hairShadowParams.x != 0.
    void SetHairShadowMap(uint64_t mapHandle)
    {
        m_hairShadowHandle = mapHandle;
    }

    // Clustered lighting (all 3 must be non-zero to take effect).
    void SetClusterSRVs(uint64_t lights, uint64_t indexList, uint64_t grid)
    {
        m_clusterLightsSRV    = lights;
        m_clusterIndexListSRV = indexList;
        m_clusterGridSRV      = grid;
    }

    // Per-frame MaterialBuffer SRV.
    void SetMaterialBuffer(uint64_t handle) { m_materialBufHandle = handle; }

    // XeGTAO SSAO SRV (t18); 0 -> the 1x1 white fallback.
    void SetSSAOHandle(uint64_t handle) { m_in->ssao = handle; }
    // The AO source chosen for this frame (RTAO or XeGTAO output, 0 = none);
    // Renderer's resolve() names the same texture for the forward BRDF.
    uint64_t GetSSAOHandle() const { return m_in->ssao; }

    // Sky SH (t19, 9 x float4). When valid, the shader uses SH for diffuse IBL.
    void SetSkySH(uint64_t handle, bool valid)
    {
        m_skySHHandle = handle;
        m_skySHValid  = valid;
    }

    // Aerial Perspective 3D LUT (t20). Shader gates on aerialMaxDistKm > 0.
    void SetAerialPerspective(uint64_t handle, float maxDistKm)
    {
        m_apHandle    = handle;
        m_apMaxDistKm = maxDistKm;
    }
    // A secondary view's own aerial LUT, in its segment (0 = the main view's, SetAerialPerspective).
    void SetViewAerialPerspective(uint64_t handle) { m_in->aerial = handle; }

    // Reflection-probe cubes, the probe buffer and DDGI have no setter: the shaders reach them
    // through handles the Renderer takes from their owners each frame (see Execute). The SSR result
    // is not read by this pass at all (WaterPass owns it). Do not mirror those handles here: a
    // cached per-frame ring handle goes a frame stale.

    // ClusterPass probe grid + flat probe index list, read through LightCB handles
    // (GetReflProbeGridSrv / GetReflProbeIndexSrv), not bound. The Renderer wires them once, right
    // after ClusterPass::Init (DEFAULT-heap buffers, never recreated); the handle is re-derived every
    // frame.
    void SetReflectionProbeCluster(uint64_t gridHandle, uint64_t indexHandle)
    {
        m_reflectionProbeGridHandle  = gridHandle;
        m_reflectionProbeIndexHandle = indexHandle;
    }

    // RT reflection result, same frame (RTReflectionPass runs between DecalPass and this pass).
    // 0 -> the 1x1 black fallback (rtConf == 0, inert).
    void SetRTReflection(uint64_t handle) { m_in->rtReflection = handle; }

    // RT sun-shadow mask (1 = lit), same frame (RTShadowPass runs between DecalPass and this pass).
    // 0 -> the shared 1x1 lit fallback (shadow.hlsli's min() fold becomes a no-op; also gated by
    // LightCB.rtShadowEnabled).
    void SetRTShadowMask(uint64_t handle) { m_in->rtShadowMask = handle; }

    // ---- Resolved SRVs for the bindless reads ----
    // The Renderer calls these once per frame, right before graph.Execute, and writes them into
    // LightCB as typed bindless handles (IGraphicsDevice::BindlessOf) that HLSL opens through
    // bindless.hlsli (the graphics root signature is at the 64-DWORD limit; see
    // RootSignatureLayout.h). Each returns its Init fallback on frames without a live resource, so
    // the shader reads a defined value. Not valid before Init has created the fallbacks.
    uint64_t GetRTReflectionSrv() const
    {
        return m_in->rtReflection ? m_in->rtReflection : m_rtReflectionFallback;
    }
    uint64_t GetRTShadowMaskSrv() const
    {
        return m_in->rtShadowMask ? m_in->rtShadowMask : m_rtShadowMaskFallback;
    }
    // The fallback is a real 1x1 TextureCubeArray with 12 slices, so even a stale cube index samples
    // in bounds. HLSL must declare it TextureCubeArray<float>: the SRV is TEXTURECUBEARRAY /
    // R32_FLOAT.
    uint64_t GetPointShadowAtlasSrv() const
    {
        return m_pointShadowAtlasHandle ? m_pointShadowAtlasHandle
                                        : m_pointShadowAtlasFallback;
    }
    // Texture2D<float>, R32_FLOAT; the fallback is a single 1x1 depth-0.0 texel.
    uint64_t GetPerObjectShadowSrv() const
    {
        return m_perObjShadowHandle ? m_perObjShadowHandle : m_perObjShadowFallback;
    }
    // Shares the per-object fallback: both are Texture2D<float> depth maps read through the same
    // reversed-Z comparison sampler, where 0.0 == fully lit.
    uint64_t GetHairShadowSrv() const
    {
        return m_hairShadowHandle ? m_hairShadowHandle : m_perObjShadowFallback;
    }

    // ---- The per-cluster reflection-probe lists ----
    // StructuredBuffer<ProbeGridEntry> (stride 8) and StructuredBuffer<uint> (stride 4), read by
    // Lighting.ps only inside `reflectionProbeCount > 0`. When absent these return type-correct null
    // structured-buffer SRVs built in Init: opening `StructuredBuffer<T> b = ResourceDescriptorHeap[i]`
    // on a descriptor of another type is undefined behaviour that Resource Binding Tier 3 never
    // reports.
    uint64_t GetReflProbeGridSrv() const
    {
        return m_reflectionProbeGridHandle ? m_reflectionProbeGridHandle
                                           : m_reflProbeGridNullSrv;
    }
    uint64_t GetReflProbeIndexSrv() const
    {
        return m_reflectionProbeIndexHandle ? m_reflectionProbeIndexHandle
                                            : m_reflProbeIndexNullSrv;
    }

    // ---- The reflection-probe buffer's absent descriptor ----
    // StructuredBuffer<GpuShared::ReflectionProbe>, stride 192. Unlike the accessors above this
    // bakes in no live handle: the live SRV is a 3-slot UPLOAD ring owned by ReflectionProbeManager
    // and selected by gfx.GetFrameIndex(), so the only correct source is m_probeMgr.GetBufferSrv(gfx)
    // read in the same frame. The Renderer falls back to this type-correct null SRV only when the
    // manager has no buffer at all (its Init failed).
    uint64_t GetReflProbeBufferNullSrv() const { return m_reflProbeBufferNullSrv; }

private:
    enum class Variant : uint8_t { PBR, NPR, Unlit, CameraPrimary };

    PSODesc BuildPSODesc(Variant variant) const;

    // ---- Core ----
    // m_name: see GetName() / SetNameSuffix.
    std::string         m_name = kBaseName;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    // No sampler members: Lighting.ps names fixed sampler-heap slots.

    // The inputs that belong to a view (BindView).
    struct ViewInputs
    {
        uint64_t ssao          = 0;       // t18 space0
        uint64_t rtReflection  = 0;
        uint64_t rtShadowMask  = 0;
        bool     cameraPrimary = false;
        uint64_t aerial        = 0;       // t20: a secondary view's LUT; 0 = m_apHandle
    };
    struct ViewInputsData : ViewData
    {
        ViewInputs in;
        void Release(IGraphicsDevice&) override {}
    };

    // ---- Graph inputs (the GBuffer ones are constructor parameters) ----
    RG::RGTextureHandle m_ptResult;
    RG::RGTextureHandle m_diResult;
    RG::RGTextureHandle m_giResult;
    RG::RGTextureHandle m_albedo;
    RG::RGTextureHandle m_normal;
    RG::RGTextureHandle m_surface;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_emissive;
    // Opaque scene colour with emissive + direct/indirect lighting but without the same-frame
    // traced-reflection contribution. The late SSR subsystem snapshots this instead of the final HDR
    // so screen radiance cannot feed the reflection result back into itself.
    RG::RGTextureHandle m_reflectionSource;

    // ---- Per-frame SRV handles (set by the Renderer) ----
    // IBL
    uint64_t m_iblIrradianceHandle = 0;
    uint64_t m_iblRadianceHandle   = 0;
    uint64_t m_brdfLutHandle       = 0;
    uint32_t m_iblRadianceMips     = 7;
    float    m_iblStrength         = 1.0f;
    // Shadows
    uint64_t m_shadowSrvHandle        = 0;
    // 1x1 fully-lit CSM stand-in, bound whenever the real shadow array is absent so the shader
    // never samples an unbound t9 (created in Init).
    RHI::Texture        m_shadowFallbackTex;
    uint64_t            m_shadowFallbackHandle = 0;
    uint64_t m_spotShadowAtlasHandle  = 0;
    uint64_t m_spotShadowVPHandle     = 0;
    uint64_t m_pointShadowAtlasHandle = 0;   // TextureCubeArray<float> (bindless)
    uint64_t m_perObjShadowHandle     = 0;   // Texture2D<float> (player, bindless)
    uint64_t m_hairShadowHandle       = 0;   // Texture2D<float> (bangs -> face, bindless)
    // Clustered lighting: Lighting.ps.hlsl declares gLights / gLightIndexList / gLightGrid at
    // t13..t15.
    uint64_t m_clusterLightsSRV    = 0;            // t13 space0
    uint64_t m_clusterIndexListSRV = 0;            // t14 space0
    uint64_t m_clusterGridSRV      = 0;            // t15 space0
    // Material
    uint64_t m_materialBufHandle = 0;              // t12 space0 (per-material NPR params)
    // SSAO + sky
    uint64_t m_skySHHandle   = 0;                  // t19 space0
    bool     m_skySHValid    = false;
    // Aerial perspective
    uint64_t m_apHandle     = 0;                   // t20 space0
    float    m_apMaxDistKm  = 32.0f;
    // Reflection-probe cluster lists (the probe buffer is a per-frame ring the Renderer reads off
    // ReflectionProbeManager).
    uint64_t m_reflectionProbeGridHandle   = 0;
    uint64_t m_reflectionProbeIndexHandle  = 0;
    // RT reflection (same frame, RTReflectionPass temporal output) and the RT sun-shadow mask
    // A view's inputs: the main view's here, a secondary view's in its ViewState; m_in is the bound
    // view's (BindView).
    ViewInputs  m_mainIn;
    ViewInputs* m_in = &m_mainIn;

    // ---- Init-time fallbacks (what a slot or LightCB handle names when its input is absent) ----
    // SSAO 1x1 white (reads as 1.0 = no occlusion).
    RHI::Texture m_ssaoFallbackTex;
    uint64_t     m_ssaoFallbackHandle = 0;
    // Sky SH 9 x float4 zeros (the shader gates reads on iblUseSH).
    RHI::GPUBuffer m_skySHFallback;
    uint64_t       m_skySHFallbackHandle = 0;
    // No aerial-perspective fallback: SkyIBLPass creates the real 3D LUT and the shader gates its
    // read on aerialMaxDistKm > 0, so an unbound t20 is the defined state on frames without a LUT.
    // Do not add a fallback member without its creator.
    //
    // Spot-shadow atlas 1x1 array (reversed-Z: texel 0.0 = fully lit) + identity-VP buffer.
    RHI::Texture   m_spotShadowAtlasFallbackTex;
    uint64_t       m_spotShadowAtlasFallback = 0;
    RHI::GPUBuffer m_spotShadowVPFallbackBuf;
    uint64_t       m_spotShadowVPFallback = 0;
    // Point-shadow cube atlas 1x1 fallback (TextureCubeArray<float>).
    RHI::Texture   m_pointShadowAtlasFallbackTex;
    uint64_t       m_pointShadowAtlasFallback = 0;
    // Per-object (player) shadow 1x1 fallback (Texture2D<float>; reversed-Z: texel 0.0 = fully
    // lit).
    RHI::Texture   m_perObjShadowFallbackTex;
    uint64_t       m_perObjShadowFallback = 0;
    // RT reflection 1x1 black (rtConf == 0 -> the 3-tier composite collapses to the no-RTR math).
    RHI::Texture m_rtReflectionFallbackTex;
    uint64_t     m_rtReflectionFallback = 0;
    // Dedicated 1x1 WHITE fallback for the RT visibility mask (the per-object fallback's 0.0 texel
    // means "lit" only under the comparison sampler, wrong here).
    RHI::Texture m_rtShadowMaskFallbackTex;
    uint64_t     m_rtShadowMaskFallback = 0;
    // Type-correct null StructuredBuffer SRVs for the probe grid / index lists (strides 8 and 4):
    // adjacent slots in the shader-visible heap's static region, written once in Init by
    // IGraphicsDevice::CreateNullStructuredSrvBlock. The block itself is kept (m_probeNullBlock) so a
    // shorter-lived instance (a preview GraphSet's) can hand its descriptors back.
    uint64_t     m_reflProbeGridNullSrv  = 0;
    uint64_t     m_reflProbeIndexNullSrv = 0;
    // Third slot of the same block: a null StructuredBuffer SRV of stride
    // sizeof(GpuShared::ReflectionProbe) (192) for the probe buffer. Same lifetime.
    uint64_t     m_reflProbeBufferNullSrv = 0;

    // The three-descriptor static block the handles above point into (the device keeps the
    // allocation). Shutdown and a re-Init hand it back through IGraphicsDevice::ReleaseDescriptorBlock,
    // never Free() it in place: DescriptorAllocation::Free is immediate (the slots are reusable on the
    // next AllocateStatic) and RenderGraph::Destroy checks no quiescence, so only the device's
    // retired-frame counter can say no command list still names them.
    RHI::DescriptorBlock m_probeNullBlock;

    // IGraphicsDevice::GetReleaseStampFrame() at the last Execute; 0 if this instance never
    // recorded. Shutdown compares it with GetRetiredFrame() to check the quiesced-boundary contract
    // instead of trusting the caller. Written on the render half, read on the game half's serial
    // window with the render half parked (the hand-over every teardown field uses).
    uint64_t m_lastRecordStamp = 0;

    // Never set: no current reader.
    uint64_t m_defaultWhiteGpuHandle = 0;
};
