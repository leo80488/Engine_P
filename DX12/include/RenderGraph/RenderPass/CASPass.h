#pragma once

// CASPass - AMD FidelityFX Contrast Adaptive Sharpening (compute).
// Runs between TAA (HDR resolve) and AutoExposure / Bloom / ToneMap, so the sharpening happens in
// HDR and flows through the rest of the post chain; when disabled or unavailable the Renderer
// passes the HDR through untouched. Reads the HDR input as an SRV and writes a same-size
// R16G16B16A16_FLOAT output (a shared pool slot or the private target) as a UAV.

#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's target)
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/PassTexture.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;

class CASPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // RG::PrepareStage: PostPool - learn that the shared post target exists (before the working set,
    // or RebuildTextures strands a private full-res target); PostEffects - the working set.
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.stage == RG::PrepareStage::PostPool) { if (c.postPool) SetSharedOutput(c.postPool); }
        else                                       PrepareResources(c.postW, c.postH);
    }
    // It records any view: the target is the bound view's (BindView).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // Per-frame inputs.
    void SetInputSrv(uint64_t srv)            { m_inputSrv = srv; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void PrepareResources(uint32_t w, uint32_t h);

    // Record the compute dispatch onto cl.
    void Execute(RHI::CommandList cl);

    // Public SRV handle of the sharpened HDR result (valid after Execute).
    uint64_t GetOutputSrvHandle() const;

    // Enable / disable: the Renderer checks this before inserting CAS into the post chain.
    // Disabling frees no GPU resources (cheap toggle).
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // ---- Shared post-chain output (ping-pong pool) ----
    // The post chain is linear: each pass reads the previous result and fully overwrites its own
    // output, so any chain needs only two full-res buffers. Five private RGBA16F targets cost 79 MB
    // at 1080p (~315 MB at 4K); a 2-buffer pool costs 32 MB.
    // A pool rather than placed-resource aliasing: every pass fully writes its target, so plain reuse
    // needs no aliasing barriers and has no undefined-contents rule to get wrong. The state is the
    // device's book's (RG::TransitionTo), one book per texture: tracking it per pass while sharing the
    // memory is how double-transition bugs happen.
    // A null <-> non-null transition changes which allocation Out() resolves
    // to, so the private target has to be released (or rebuilt). Ping-ponging
    // between two non-null pool slots is NOT a rebuild. The Renderer sets this
    // on the main thread immediately before PrepareResources, so the dirty flag
    // is always consumed there and never reaches the compute worker's lazy path.
    void SetSharedOutput(const RHI::Texture* tex)
    {
        if ((m_t->sharedOutput == nullptr) != (tex == nullptr)) m_t->texDirty = true;
        m_t->sharedOutput = tex;
    }

private:
    struct alignas(16) CASCB
    {
        uint32_t width;
        uint32_t height;
        float    sharpness;
        float    _pad;
    };

    void RebuildTextures();
    bool CreatePipelines(IGraphicsDevice& gfx);

    const RHI::Texture& Out() const { return m_t->sharedOutput ? *m_t->sharedOutput : m_t->output.Tex(); }

public:
    // Sharpness [0..1], the AMD CAS convention. Even 0 sharpens mildly (peak = -1/8; the weight
    // curve runs from -1/8 to -1/5), so use SetEnabled(false) for a true passthrough.
    //   0.3-0.5 -> TAA-softness cleanup (quality preset)
    //   0.6-0.8 -> visible crispening (sharpen preset)
    //   0.9-1.0 -> aggressive; can ring on specular edges in HDR
    float sharpness = 0.6f;

private:
    IGraphicsDevice*    m_gfx = nullptr;
    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_pso;

    // One view's target: the private output (a PassTexture at the post size, made only when no pool is
    // bound), the pool slot when one is, and the size latch. m_t is the bound view's (BindView); a
    // secondary view's is prepared in its segment, from its own pool.
    struct Target
    {
        RG::PassTexture     output{ RG::PassTextureDesc{ .format    = RHI::Format::R16G16B16A16_FLOAT,
                                                         .size      = RG::SizePolicy::PostAA,
                                                         .debugName = "CAS.Output" } };
        const RHI::Texture* sharedOutput = nullptr;
        uint32_t            vpW = 0, vpH = 0;
        bool                texDirty = true;
    };
    struct ViewTarget : ViewData
    {
        Target t;
        void Release(IGraphicsDevice& gfx) override { t.output.Release(gfx); }
    };
    Target  m_mainTarget;
    Target* m_t = &m_mainTarget;

    // True only for a frame in which Execute dispatched. Every early return in Execute (disabled,
    // missing PSO, no input SRV, zero viewport, invalid target) leaves the output holding someone
    // else's pixels: the previous frame's or, in a shared pool slot, another effect's.
    // GetOutputSrvHandle() must not publish that; the adapters' if (const uint64_t out = ...) idiom
    // then leaves ctx.hdrSrv untouched and the chain passes through.
    bool m_wroteThisFrame = false;

    uint64_t m_inputSrv = 0;
    bool     m_enabled  = true;
};
