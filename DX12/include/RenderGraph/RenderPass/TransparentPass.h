#pragma once

// TransparentPass - forward PBR pass for alpha-blended geometry.
//
// Renders after GBufferPass + LightingPass + SkyboxPass onto the same HDR target,
// depth-testing against the GBuffer depth (no depth write).
// Draw list: DrawFilter::Transparent packets, sorted back-to-front (painter's order)
// by the Renderer.
//
// Blend modes come from the PermutationKey (BuildPSODesc):
//   ADDITIVE_BLEND      -> ONE/ONE
//   PREMULTIPLIED_BLEND -> ONE/INV_SRC_ALPHA
//   MULTIPLY_BLEND      -> DEST_COLOR/INV_SRC_ALPHA
//   otherwise           -> SRC_ALPHA/INV_SRC_ALPHA
//
// LightCB at b2 (the VS owns b1 for PerViewCB); Transparent.ps.hlsl has the full
// register table.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSampler.h"
#include "Graphics/FogApply.h"
#include "Graphics/RenderTypes.h"

#include <string>

class TransparentPass : public RG::RenderPass
{
public:
    // The base name every instance's GetName() starts with, kept in one place.
    static constexpr const char* kBaseName = "TransparentPass";

    // @p depth: the GBuffer depth, bound as the read-only DSV.
    // @p gbufNormal / @p traceDepth: GBuffer1 normal + the SSRTraceDepth snapshot. Not
    // bound here - the forward shader opens them bindless from LightCB.fwdReflNormalIdx /
    // fwdReflDepthIdx (MS_FetchTracedReflection); declared ReadSRV so the graph
    // guarantees their state when the draws record.
    // @p frontDepth: the TranslucentFront_Depth the front-layer prepass writes, for the
    // shader's "am I the stamped front layer" test (MS_FetchFrontLayerReflection,
    // LightCB.fwdReflFrontDepthIdx); declared ReadSRV for the same reason. The second
    // RTReflectionPass's output is not declared: it is pass-owned and self-barriered,
    // like the opaque instance's t44 texture.
    TransparentPass(RG::RGTextureHandle depth,
                    RG::RGTextureHandle gbufNormal,
                    RG::RGTextureHandle traceDepth,
                    RG::RGTextureHandle frontDepth);

    // Instance name: kBaseName plus the optional suffix. RenderGraph keys the
    // external-wait map, the PIX/RenderDoc marker and the GPU timestamp region on
    // GetName(), and GetGPUTimestampResult is a first-match-wins name scan, so two
    // instances must not share a name.
    const char* GetName() const override { return m_name.c_str(); }
    // Call before the pass is handed to a RenderGraph (see GBufferPass.h).
    void SetNameSuffix(const char* suffix);

    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    // Same borrow gate as GBufferPass: a preview instance borrows the main
    // TransparentPass's library and must never destroy it.
    void Shutdown(IGraphicsDevice& gfx)            override;
    // Renderer::PassSet events (the main graph never Destroys its passes, so this is their only
    // Shutdown); kViewSafe: it draws the view's list with the view's constants.
    uint32_t HandledEvents() const override { return kShutdown | kViewSafe; }
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // Activity gate. Execute / PrepareRecording start from
    // GetDrawList(DrawFilter::Transparent), which IsActive cannot reach (no
    // RenderContext in Phase A), so the Renderer pushes the count, as for
    // WaterSurfacePass and TranslucentFrontLayerPass.
    // Do not default the count to 0: PreviewRenderer instantiates this class again (the
    // m_live / m_world GraphSets) and nothing there pushes a count, so 0 would hide the
    // glass in the material inspector and the split viewports. -1 = "no count pushed"
    // -> active.
    bool IsActive() const override
    { return !RG::PassActivityGatesEnabled() || m_transparentDrawCount != 0; }
    void SetTransparentDrawCount(int32_t n) { m_transparentDrawCount = n; }
    // A preview view whose material domain has no forward pass hides it
    // (RenderGraph/SceneView.h); the main view always shows it.
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Transparent); }

    // Borrowers return null. The base ReloadShaders ClearCaches() the library this
    // names; a second clear on a borrowed library double-bumps the reload epoch
    // MaterialReflectionSync stamps against. The PSO cache is per-instance and always
    // cleared.
    ShaderLibrary*         GetReloadableShaderLibrary() override
    { return OwnsShaderLibrary() ? m_lib : nullptr; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // ---- ShaderLibrary injection ----
    // The mirror of GBufferPass's: a custom PS's SV_Target count comes from its
    // permutation bits, not from the pass, so a translucent material's shader is a
    // 1-RTV forward binary. Borrowing GBufferPass's library would resolve the id (any
    // library does) and then bind a 1-RTV PS into a 6-RTV PSO (see ShaderLibrary.h). A
    // translucent preview must borrow this library.
    // Call before Init(): Init wires m_psoCache to the library, and only the owner
    // registers the two static shaders. Obligations, as in GBufferPass: order (the
    // borrower Inits after the owner), hot reload (the accessor above), lifetime
    // (PSOCache holds a bare ShaderLibrary*, so the borrower must not outlive the owner;
    // the Renderer destroys m_preview before m_graph), signature (this is the forward
    // domain's library; opaque GBuffer-shaped previews keep borrowing GBufferPass's).
    void SetSharedShaderLibrary(ShaderLibrary& lib) { m_lib = &lib; }
    bool OwnsShaderLibrary() const { return m_lib == &m_ownedLib; }
    // The library this instance's PSOCache compiles with: the borrowed one for a
    // borrowing instance.
    ShaderLibrary& GetShaderLibrary() { return *m_lib; }

    // Clustered-light triple (t13/t14/t15) for MS_ForwardLighting's clustered point /
    // spot loop. All three or none, matching LightCB.clusterLightCount: the shader opens
    // the tables whenever that count is non-zero (the same contract as LightingPass and
    // WaterSurfacePass), so a caller that passes zeros here must also write a zero count.
    void SetClusterSRVs(uint64_t lights, uint64_t indexList, uint64_t grid)
    {
        m_clusterLightsSrv = lights;
        m_clusterIndexSrv  = indexList;
        m_clusterGridSrv   = grid;
    }
    // The spot-shadow per-slice view buffer (t22, Gfx::SpotShadowVP), the current
    // frame's as LightingPass receives it, for the forward loop's spot shadows
    // (punctual_shadow.hlsli). 0 binds this pass's 1-entry "no atlas" fallback; the
    // shader reads it only for a light that owns a slice.
    void SetSpotShadowVP(uint64_t vps) { m_spotShadowVPHandle = vps; }

    // IBL handles forwarded from Renderer::SyncSkyboxIBL each frame.
    void SetIBL(uint64_t irradiance, uint64_t radiance,
                uint32_t radianceMips, float strength)
    {
        m_iblIrradianceHandle = irradiance;
        m_iblRadianceHandle   = radiance;
        (void)radianceMips; (void)strength; // carried by LightCB
    }
    void SetBRDFLUT(uint64_t lut) { m_brdfLutHandle = lut; }

    // Sky SH StructuredBuffer SRV (t19 space0, 9 x float4), the buffer the deferred
    // LightingPass reads. With LightCB.iblUseSH=1 the forward shader evaluates SH for
    // diffuse IBL instead of sampling the irradiance cubemap.
    void SetSkySH(uint64_t handle) { m_skySHHandle = handle; }

    // CSM cascade array SRV (t9 space0, root param Gfx::ShadowSRV), the same array
    // LightingPass and WaterSurfacePass receive. The forward BRDF (custom_surface.hlsli
    // MS_ForwardLighting) calls ComputeShadowFactor, which samples t9 on every draw; the
    // comparison sampler is a fixed sampler-heap slot. Passing 0 is legal: Execute()
    // then binds a 1x1 fully-lit fallback, because ShadowPass::GetShadowArrayGpuHandle()
    // returns 0 until its array exists and an unbound table the shader reads is UB that
    // Tier 3 validation does not report.
    void SetShadowMap(uint64_t srv) { m_shadowSrvHandle = srv; }

    // No reflection-probe setter: the forward shader reaches probe cubes and the probe
    // buffer through descriptor-heap indices (see RecordRange).

    // Global view mode: FILL_MODE_WIREFRAME when true (part of the PSO key), set per
    // frame by the Renderer from ViewMode::Wireframe. Transparent.ps handles the
    // Unlit / Wireframe colour itself.
    void SetWireframe(bool w) { m_wireframeMode = w; }
    // The DEBUG_SHADE permutation on this frame's PSOs (see GBufferPass::SetDebugShade).
    void SetDebugShade(bool on) { m_debugShade = on; }
    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }
    // The material sampler's LOD bias follows the upscale ratio
    // (Graphics/MaterialSampler.h). Renderer::BeginFrame calls it with the graph's
    // resize recompile, never while a frame records.
    void SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW);

    // Builds now, on the calling thread (a preload's TaskSystem worker), the PSOs
    // Execute would build for @p dp (a material's synthetic packet), mirroring the
    // record path's choice; the PSO cache is thread-safe (single flight). Nothing is
    // bound or drawn.
    void WarmPipelines(const DrawPacket& dp);

private:
    static constexpr uint32_t kMaxRecordChunks = 4;
    struct RecordChunkState {
        RHI::CommandList cl;
        uint32_t begin = 0, end = 0, gpuRegion = ~0u;
    };

    uint32_t PrepareRecording(RHI::CommandList cl) override;
    RHI::CommandList ExecutePrepared(RHI::CommandList cl) override;
    void ExecuteAdditionalRecording(uint32_t task) override;
    void FinishRecording() override;
    RHI::CommandList RecordRange(RHI::CommandList cl, DrawList draws,
                                const RHI::PipelineState* const* psos);

    // Builds the PSO descriptor for a permutation (blend mode + cull). customPSID > 0
    // replaces Transparent_PS with a ShaderLibrary::RegisterDynamic shader, as in
    // GBufferPass, so a custom-shader material with userBlendMode=Additive draws here.
    // mirrored: negative-determinant world matrix (see GBufferPass::BuildPSODesc).
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID = 0, uint32_t customVSID = 0,
                         bool mirrored = false) const;

    RecordChunkState m_recordChunks[kMaxRecordChunks]{};
    uint32_t m_recordChunkCount = 1;
    DrawList m_recordDraws{};
    std::vector<const RHI::PipelineState*> m_recordPSOs;

    // DrawFilter::Transparent packet count, pushed by the Renderer before the graph
    // executes. -1 = never pushed (preview instances) -> active.
    int32_t m_transparentDrawCount = -1;

    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_gbufNormal;   // declared ReadSRV; read bindless via LightCB
    RG::RGTextureHandle m_traceDepth;   // declared ReadSRV; read bindless via LightCB
    RG::RGTextureHandle m_frontDepth;   // declared ReadSRV; read bindless via LightCB

    // OWNED vs ACTIVE. m_lib is what PSOCache is Init'd against and what
    // GetShaderLibrary() hands out; it points at m_ownedLib unless
    // SetSharedShaderLibrary redirected it. Same shape as GBufferPass.
    ShaderLibrary  m_ownedLib;
    ShaderLibrary* m_lib = &m_ownedLib;
    PSOCache       m_psoCache;
    std::string    m_name = kBaseName;

    // Clustered-light SRVs (SetClusterSRVs); bound only when all three are set.
    uint64_t m_clusterLightsSrv = 0;
    uint64_t m_clusterIndexSrv  = 0;
    uint64_t m_clusterGridSrv   = 0;

    int   m_linearSamplerIdx = -1;   // the material sampler (SetDynamicSampler)
    float m_mipBias = MaterialSampler::kNativeBias;   // ...and its LOD bias
    uint32_t m_textureAnisotropy = 8;

    // Default fallback textures (as in GBufferPass)
    RHI::Texture m_defaultWhite;
    uint64_t     m_defaultWhiteGpuHandle = 0;
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;

    // IBL handles, set each frame by the Renderer.
    uint64_t m_iblIrradianceHandle = 0;
    uint64_t m_iblRadianceHandle   = 0;
    uint64_t m_brdfLutHandle       = 0;

    // Sky SH (t19 space0), set by Renderer::SyncSkyboxIBL each frame. 0 is fine: the
    // shader gates on LightCB.iblUseSH, which the Renderer writes 0 when SkyIBLPass has
    // not run.
    uint64_t m_skySHHandle = 0;

    // CSM array from ShadowPass, plus a type-correct 1x1 stand-in for frames before it
    // exists: R32_FLOAT, arraySize > 1, texel 0.0 (fully lit under reverse-Z), so the SRV
    // matches the shader's Texture2DArray<float> (built like LightingPass's spot-atlas
    // fallback). A type-mismatched filler would be silent UB under Resource Binding
    // Tier 3.
    uint64_t            m_shadowSrvHandle      = 0;
    RHI::Texture        m_shadowFallbackTex;
    uint64_t            m_shadowFallbackHandle = 0;
    // The spot-shadow VP buffer (t22) and its 1-entry fallback.
    uint64_t            m_spotShadowVPHandle   = 0;
    RHI::GPUBuffer      m_spotShadowVPFallbackBuf;
    uint64_t            m_spotShadowVPFallback = 0;

    // Global view-mode wireframe toggle (see SetWireframe). Atomic: set by the render
    // half, read by BuildPSODesc on a recording worker and on a preload's worker
    // (WarmPipelines).
    std::atomic<bool> m_wireframeMode{ false };
    std::atomic<bool> m_debugShade{ false };   // SetDebugShade
};
