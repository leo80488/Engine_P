#pragma once

// TransmissionPasses - the raster half of BlendMode::Transmissive (TransmissionFeature).
//
//   TransmissionPrepass   draws every Transmissive surface into three graph targets whose
//                         blends are commutative, so the layers need no sort: the summed
//                         screen offset to its refracted background and the summed spread
//                         of its rough lobe (ADD), the product of the layers' tints
//                         (MULTIPLY) and the nearest layer's depth (MAX). The surface's own
//                         hook runs (custom_surface.hlsli's transmission main, the
//                         TRANSMISSION_PREPASS bit), occluded by the read-only GBuffer depth.
//   TransmissionApplyPass copies the HDR scene as the opaque pass, the sky and the fog left
//                         it, builds that copy's mip chain and writes the refracted,
//                         blurred, tinted background under every covered pixel
//                         (TransmissionApply.ps.hlsl).
// TransparentPass then draws the surfaces' own light over it (the TRANSMISSIVE_BLEND
// permutation), sorted with the other translucent draws.
//   TransmissionMotionPass gives every covered pixel the motion of the background it shows
//                         (TransmissionMotion.cs.hlsl), after the sky's velocity fill and
//                         before the temporal resolves. Not a graph pass: TransmissionFeature
//                         records it at PreResolve, like VelocityFillPass.

#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/PSOCache.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/SSR/SceneColorPyramidPass.h"
#include "Graphics/ShaderLibrary.h"

class TransmissionPrepass : public RG::RenderPass
{
public:
    // The three targets the apply pass reads (formats: kParamsFormat, kTintFormat,
    // kLayerDepthFormat) and the GBuffer depth, tested read-only.
    static constexpr RHI::Format kParamsFormat     = RHI::Format::R16G16B16A16_FLOAT;
    static constexpr RHI::Format kTintFormat       = RHI::Format::R16G16B16A16_FLOAT;
    static constexpr RHI::Format kLayerDepthFormat = RHI::Format::R32_FLOAT;

    TransmissionPrepass(RG::RGTextureHandle depth, RG::RGTextureHandle params,
                        RG::RGTextureHandle tint, RG::RGTextureHandle layerDepth);

    const char* GetName() const override { return "TransmissionPrepass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return !RG::PassActivityGatesEnabled() || m_active; }
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Transparent); }

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Main thread, before the graph records (TransmissionFeature::Prepare).
    void SetActive(bool active) { m_active = active; m_ran = false; }
    // The graph recorded it this frame, so its targets hold this frame's layers: false when it was
    // skipped (inactive, or a show flag hid the translucent draws).
    bool Ran() const { return m_ran; }

    static bool IsTransmissivePacket(const DrawPacket& dp)
    {
        return dp.filter == DrawFilter::Transparent && dp.renderInMainPass &&
               dp.permutation.Has(PermutationKey::TRANSMISSIVE_BLEND);
    }

private:
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customPSID, uint32_t customVSID, bool mirrored) const;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    RG::RGTextureHandle m_depth, m_params, m_tint, m_layerDepth;
    int          m_linearSamplerIdx = -1;   // the material sampler at its native bias
    bool         m_active = false;
    bool         m_ran = false;
    RHI::Texture m_defaultWhite;
    RHI::Texture m_defaultFlatNormal;
    uint64_t     m_defaultWhiteGpuHandle      = 0;
    uint64_t     m_defaultFlatNormalGpuHandle = 0;
};

class TransmissionApplyPass : public RG::RenderPass
{
public:
    // What the apply needs from the frame's camera (TransmissionFeature::Prepare).
    struct View
    {
        float nearZ = 0.1f;
        float farZ  = 1000.0f;
        float fovY  = 1.0f;
    };

    TransmissionApplyPass(RG::RenderGraph& graph, RG::RGTextureHandle depth, RG::RGTextureHandle params,
                          RG::RGTextureHandle tint, RG::RGTextureHandle layerDepth);

    const char* GetName() const override { return "TransmissionApplyPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return !RG::PassActivityGatesEnabled() || m_active; }
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Transparent); }
    uint32_t HandledEvents() const override { return kShutdown; }
    void Shutdown(IGraphicsDevice& gfx) override;
    // The graph's reload walk; the mip-chain builder keeps a library of its own.
    void ReloadShaders(IGraphicsDevice& gfx) override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Main thread, before the graph records (TransmissionFeature::Prepare). The scene snapshot
    // and its mip chain, sized to the HDR target: made on the first frame that draws a
    // Transmissive surface, so a scene without one never holds them. False when they cannot
    // be made this frame.
    bool EnsureTargets(IGraphicsDevice& gfx);
    void SetActive(bool active, const View& view) { m_active = active; m_view = view; }

private:
    struct Constants
    {
        uint32_t paramsIdx, tintIdx, layerDepthIdx, sceneDepthIdx;
        uint32_t sourceIdx;
        float    sourceMaxMip, nearZ, farZ;
        float    renderSize[2];
        float    pixelAngle, blurScale;
    };

    RG::RenderGraph&    m_graph;
    RG::RGTextureHandle m_depth, m_params, m_tint, m_layerDepth;

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // The HDR scene before translucency (its copy, then the copy's mip chain).
    RHI::Texture          m_snapshot;
    RHI::ResourceState    m_snapshotState = RHI::ResourceState::COPY_DST;
    uint32_t              m_snapshotW = 0, m_snapshotH = 0;
    SceneColorPyramidPass m_pyramid;

    bool m_active = false;
    View m_view;
};

class TransmissionMotionPass : public RG::PassLifecycle
{
public:
    // One frame's inputs: the prepass's targets (compute-readable), the scene depth's SRV and the
    // velocity buffer in @p velocityState. Every texture is the render resolution.
    struct Inputs
    {
        const RHI::Texture* params     = nullptr;
        const RHI::Texture* layerDepth = nullptr;
        const RHI::Texture* velocity   = nullptr;
        RHI::ResourceState  velocityState = RHI::ResourceState::UNORDERED_ACCESS;
        uint64_t            depthSrv   = 0;
        uint32_t            width      = 0;
        uint32_t            height     = 0;
    };

    void Init(IGraphicsDevice& gfx);
    uint32_t HandledEvents() const override { return kShutdown | kReloadShaders | kWorldClear; }
    void Shutdown(IGraphicsDevice& gfx) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    void OnWorldClear(const RG::WorldClearArgs&) override { m_historyValid = false; }

    // Records both dispatches into @p cl and leaves the velocity buffer in UNORDERED_ACCESS. False
    // when they did not run (the buffer and its state are then untouched).
    bool Execute(RHI::CommandList cl, const Inputs& in);
    // A frame without the pass: the next one has no last offsets to compare with.
    void ResetHistory() { m_historyValid = false; }

private:
    struct alignas(16) Constants
    {
        uint32_t width, height, historyValid, pad;
    };
    // The offsets of this frame and the last (they swap each frame), then the motions.
    static constexpr int kTargets = 3;
    static constexpr int kMotion  = 2;

    bool CreatePipelines(IGraphicsDevice& gfx);
    bool EnsureTargets(IGraphicsDevice& gfx, uint32_t w, uint32_t h);
    void Transition(IGraphicsDevice& gfx, RHI::CommandList cl, int target, RHI::ResourceState to);

    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_motionPso;
    RHI::PipelineState m_writePso;

    // R16G16_FLOAT at the render resolution, made on the first frame the pass runs.
    RHI::Texture       m_targets[kTargets];
    RHI::ResourceState m_states[kTargets] = {};
    uint32_t           m_width = 0, m_height = 0;
    int                m_current = 0;          // the offsets written this frame: 0 or 1
    bool               m_historyValid = false;
};
