#pragma once

// FXAAPass - NVIDIA FXAA 3.11 quality preset, single-pass HDR-aware compute.
//
// Reads one HDR scene SRV (raw HDR, or TAA's resolved output for FXAA+TAA) and writes
// one R16G16B16A16_FLOAT output. No history, no jitter: purely spatial.
// Per frame: SetInputSrvHandle() with the source SRV; the PostAA prepare stage sizes
// the output; Execute() dispatches; downstream (Bloom / AutoExposure / ToneMap) reads
// GetResolvedSrvHandle().

#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's output)
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTexture.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class FXAAPass : public RG::RenderPass
{
public:
    // The base name every instance's GetName() starts with, named (like
    // RTReflectionPass::kOpaqueName) so the string lives in exactly one place.
    static constexpr const char* kBaseName = "FXAAPass";

    // Instance name. RenderGraph keys the external-wait map, the PIX marker and the GPU
    // timestamp region on GetName(), and GetGPUTimestampResult is a FIRST-MATCH-WINS
    // linear name scan, so two instances sharing a name hand every consumer the union
    // of both. With no suffix it is kBaseName.
    const char* GetName() const override { return m_name.c_str(); }
    // Call BEFORE the pass is handed to a RenderGraph (see GBufferPass.h).
    void SetNameSuffix(const char* suffix);

    void Setup(RG::RenderGraphBuilder&) override {}
    void Init(IGraphicsDevice& gfx)     override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // PostAA prepare stage: the output (~15.8 MiB at 1080p) while the AA config can run
    // FXAA, released otherwise (TAA's rule).
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.fxaa) PrepareResources(c.renderW, c.renderH);
        else        ReleaseResources();
    }
    // It records any view: its output is the bound view's (BindView).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // ---- Per-frame inputs ----
    void SetInputSrvHandle(uint64_t h)           { m_inputSrvHandle = h; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_t->vpW = w; m_t->vpH = h; }
    void PrepareResources(uint32_t w, uint32_t h);
    // Drop the output buffer while an AA mode that cannot run FXAA is selected
    // (~15.8 MiB at 1080p). As TAAPass::ReleaseResources: the Renderer gates the
    // prepare/release pair on the CONFIG, never on IsEnabled().
    void ReleaseResources();

    // ---- Output ----
    uint64_t GetResolvedSrvHandle() const;

    // ---- Enable / runtime tunables ----
    bool IsEnabled() const  { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

private:
    struct alignas(16) FXAACB
    {
        uint32_t width;
        uint32_t height;
        float    qualitySubpix;
        float    qualityEdgeThreshold;
        float    qualityEdgeThresholdMin;
        float    _pad0;
        float    _pad1;
        float    _pad2;
    };

    void RebuildBuffers();
    bool CreatePipelines(IGraphicsDevice& gfx);

public:
    // ---- Enable / runtime tunables ----
    // FXAA 3.11 quality knobs, exposed so the editor can tune them without recompiling.
    // qualitySubpix          : 0 disables sub-pixel AA; 0.75 default; 1.0 max softening
    // qualityEdgeThreshold   : minimum local contrast to apply AA. 0.063 high quality,
    //                          0.166 default, 0.333 low quality (skip more pixels)
    // qualityEdgeThresholdMin: absolute floor. 0.0312 high, 0.0833 default
    float qualitySubpix          = 0.75f;
    float qualityEdgeThreshold   = 0.166f;
    float qualityEdgeThresholdMin= 0.0833f;

private:
    // See GetName() / SetNameSuffix.
    std::string         m_name = kBaseName;
    IGraphicsDevice*    m_gfx = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    // One view's output and size stamps. The output: a PassTexture whose state is the device's book's
    // (RG::TransitionTo). FXAA never takes a post-pool slot (FXAAEffect does not ask for one:
    // IEffect::UsesSharedOutput). m_t is the bound view's (BindView).
    struct Target
    {
        RG::PassTexture output{ RG::PassTextureDesc{ .format    = RHI::Format::R16G16B16A16_FLOAT,
                                                     .debugName = "FXAA.Output" } };
        uint32_t        vpW = 0, vpH = 0;
        uint32_t        lastVpW = 0, lastVpH = 0;
    };
    struct ViewTarget : ViewData
    {
        Target t;
        void Release(IGraphicsDevice& gfx) override { t.output.Release(gfx); }
    };
    Target  m_mainTarget;
    Target* m_t = &m_mainTarget;

    uint64_t m_inputSrvHandle = 0;

    bool     m_enabled = false;
};
