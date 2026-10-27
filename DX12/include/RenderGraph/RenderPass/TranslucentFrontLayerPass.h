#pragma once

// TranslucentFrontLayerPass - the translucent front-layer prepass.
//
// RTReflectionPass traces from the opaque GBuffer, so forward glass could only borrow
// the trace of what is behind it, or march the screen. This pass (UE Lumen's "front
// layer translucency") draws the forward translucent packets once more, colour-less
// and depth-writing, into a private GBuffer-shaped target set, so a second
// RTReflectionPass instance (Variant::TranslucentFront) traces reflections from the
// nearest translucent surface and the colour pass folds that trace at full strength
// (custom_surface.hlsli MS_FetchFrontLayerReflection, via LightCB).
//
// Output: 3 render targets + depth, Renderer-created graph textures in the
// GBuffer1/2/3 formats RTReflectionPass's root signature expects:
//   RT0 TranslucentFront_Normal    R16G16B16A16_UNORM  N*0.5+0.5, .a = exact matIdx
//   RT1 TranslucentFront_Surface   R8G8B8A8_UNORM      (rough, metal, ao, reflectance)
//   RT2 TranslucentFront_Velocity  R16G16_FLOAT        raw NDC delta
//   DSV TranslucentFront_Depth     D32_FLOAT_S8X24     cleared to sky (0) every frame;
//                                                      GREATER_EQUAL + write keeps the
//                                                      nearest translucent layer
// Opaque occlusion is a manual PS test against the SSRTraceDepth snapshot (bindless,
// LightCB.fwdReflDepthIdx): there is no read-only DSV, and seeding this depth from
// the opaque one would hand the second RTR instance every opaque pixel. Most of the
// target stays at depth 0, which keeps the second instance cheap (HybridReflClassify
// emits no ray at depth 0).
//
// Packets: IsFrontLayerPacket - DrawFilter::Transparent, main pass, Alpha,
// Premultiplied or Transmissive blend (Additive / Multiply never carry a reflection), not
// Unlit, not Billboard (VFX quads). The same predicate drives the Renderer's draw count
// (IsActive) and the draw loop.
//
// Frame slot: right after the opaque RTReflectionPass, before Lighting, so the
// translucent instance sits next to the opaque one in the TransientAliasArena
// timeline (group E aliases group A's bytes) and inherits the DDGI promote wait.
//
// Custom material shaders reach this pass through the same dynamic ids as every
// other pass; the TRANSLUCENT_PREPASS permutation bit selects the 3-MRT main in
// custom_surface.hlsli.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/MaterialSampler.h"
#include "Graphics/RenderTypes.h"

#include <vector>

class TranslucentFrontLayerPass : public RG::RenderPass
{
public:
    TranslucentFrontLayerPass(RG::RenderGraph& graph,
                              RG::RGTextureHandle normal,
                              RG::RGTextureHandle surface,
                              RG::RGTextureHandle velocity,
                              RG::RGTextureHandle frontDepth,
                              RG::RGTextureHandle opaqueDepthSnapshot);

    const char* GetName() const override { return "TranslucentFrontLayerPass"; }
    void Setup(RG::RenderGraphBuilder& b)   override;
    void Init (IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Pre-graph state only (the Renderer pushes both on the main thread before the
    // graph executes, for the view it records); the graph skips the pass when false.
    bool IsActive() const override { return m_active && m_drawCount > 0; }
    // It records any view: its draws are the view's, its gate and count pushed for it.
    uint32_t HandledEvents() const override { return kViewSafe; }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }
    ShaderLibrary*         GetShaderLibrary()                    { return &m_shaderLib; }

    // The one front-layer candidate predicate, used by the Renderer's count and by the
    // draw loop.
    static bool IsFrontLayerPacket(const DrawPacket& dp)
    {
        if (dp.filter != DrawFilter::Transparent || !dp.renderInMainPass) return false;
        const PermutationKey& k = dp.permutation;
        if (k.Has(PermutationKey::ADDITIVE_BLEND) || k.Has(PermutationKey::MULTIPLY_BLEND))
            return false;
        if (k.Has(PermutationKey::UNLIT) || k.Has(PermutationKey::BILLBOARD))
            return false;
        return true;
    }

    // Pushed by the Renderer on the MAIN thread, before the graph runs.
    void SetDrawCount(uint32_t n) { m_drawCount = n; }
    bool HasDraws() const         { return m_drawCount > 0; }
    // The consumer gate: the second RTR instance is live this frame for the view the
    // graph records (RT tier + features.transparentReflections). No consumer -> no stamp.
    void SetActive(bool a)        { m_active = a; }
    // The main view's gate, which a preload's worker reads (WarmPipelines).
    void SetEnabled(bool e)       { m_enabled.store(e, std::memory_order_relaxed); }
    // OnDemand, or Always under the harness's eager arm (MemoryPolicy::EagerTransients).
    static RG::TextureResidency Residency();
    // Backs the four targets (on-demand graph textures) the first time a frame draws a front layer, and
    // keeps them. Serial window before the graph records; false when one could not be made.
    bool EnsureTargets(IGraphicsDevice& gfx);

    // Builds now, on the calling thread (a preload's TaskSystem worker), the PSOs
    // Execute would build for @p dp (a material's synthetic packet), mirroring the
    // record path's choice; the PSO cache is thread-safe (single flight). Nothing is
    // bound or drawn. No-op unless the pass is enabled (the path-traced /
    // RT-reflection paths).
    void WarmPipelines(const DrawPacket& dp);
    void SetTextureAnisotropy(uint32_t x) { m_textureAnisotropy = x; }
    // The material sampler's LOD bias follows the upscale ratio
    // (Graphics/MaterialSampler.h). Renderer::BeginFrame calls it with the graph's
    // resize recompile, never while a frame records.
    void SetUpscaleMipBias(IGraphicsDevice& gfx, uint32_t renderW, uint32_t displayW);

private:
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID,
                         uint32_t customVSID, bool mirrored) const;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RG::RenderGraph*    m_graph;
    RG::RGTextureHandle m_normal, m_surface, m_velocity, m_frontDepth;
    // Declared ReadSRV so the graph keeps it DEPTH_READ_SRV across this pass;
    // the PS opens it bindless from LightCB.fwdReflDepthIdx.
    RG::RGTextureHandle m_opaqueDepthSnapshot;

    int      m_linearSamplerIdx = -1;   // the material sampler (SetDynamicSampler): aniso wrap
    float    m_mipBias = MaterialSampler::kNativeBias;   // ...and its LOD bias
    uint32_t m_textureAnisotropy = 8;
    uint32_t m_drawCount = 0;
    bool     m_active    = false;   // the recorded view's gate (SetActive)
    // Atomic: set by the render half, read by a preload's worker (WarmPipelines).
    std::atomic<bool> m_enabled{ false };

    RHI::Texture m_defaultWhite;
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultWhiteGpuHandle      = 0;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;
};
