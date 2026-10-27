#pragma once

// DepthOfFieldPass - focus-distance depth of field (compute, HDR).
//
// Like CASPass/UnderwaterPass an HDR-space producer: reads the running HDR colour
// (t0) + the hardware scene depth (t1, reverse-Z) and writes a blurred
// R16G16B16A16_FLOAT output that PostProcess::PostChain swaps in as ctx.hdrSrv.
// Driven by the `dof` group of ResolvedPostProcessSettings via DepthOfFieldEffect.

#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's target)
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/PassTexture.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;

class DepthOfFieldPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Prepare stages: PostPool - learn that the shared post target exists (before the
    // working set, or RebuildTextures strands a private full-res target); PostEffects -
    // the working set.
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.stage == RG::PrepareStage::PostPool) { if (c.postPool) SetSharedOutput(c.postPool); }
        else                                       PrepareResources(c.postW, c.postH);
    }
    // It records any view: the target is the bound view's (BindView).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // Per-frame inputs.
    void SetInputSrv(uint64_t srv)               { m_inputSrv = srv; }
    void SetDepthSrv(uint64_t srv)               { m_depthSrv = srv; }
    void SetCameraPlanes(float nearZ, float farZ){ m_near = nearZ; m_far = farZ; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void PrepareResources(uint32_t w, uint32_t h);

    void SetEnabled(bool v) { m_enabled = v; }
    bool IsEnabled() const   { return m_enabled; }

    void Execute(RHI::CommandList cl);

    // SRV of the blurred HDR result (valid after Execute).
    uint64_t GetOutputSrvHandle() const;

    // ---- Shared post-chain output (ping-pong pool) ----
    // The post chain is linear: each pass reads the previous result and FULLY overwrites
    // its own output, so any chain needs only TWO full-res buffers (five private RGBA16F
    // targets: 79 MB at 1080p, ~315 MB at 4K; the pool: 32 MB).
    // A pool rather than placed-resource aliasing on purpose: there is nothing to
    // preserve, so plain reuse needs no aliasing barriers and has no undefined-contents
    // rule to get wrong. The STATE is the device's book's (RG::TransitionTo): one book
    // per texture - tracking it per pass while sharing the memory breeds
    // double-transition bugs.
    // A null <-> non-null change switches the allocation Out() resolves to, so the
    // private target has to be released (or rebuilt); ping-ponging between two non-null
    // pool slots is NOT a rebuild. Set at the PostPool prepare stage, before
    // PostEffects' PrepareResources, so the dirty flag is always consumed there and never
    // reaches the compute worker's lazy path.
    void SetSharedOutput(const RHI::Texture* tex)
    {
        if ((m_t->sharedOutput == nullptr) != (tex == nullptr)) m_t->texDirty = true;
        m_t->sharedOutput = tex;
    }

private:
    struct alignas(16) DoFCB
    {
        uint32_t width;
        uint32_t height;
        float    nearZ;
        float    farZ;

        float    focusDistance;
        float    focusRange;
        float    transitionRange;
        float    maxRadius;
    };

    void RebuildTextures();
    bool CreatePipelines(IGraphicsDevice& gfx);

    const RHI::Texture& Out() const { return m_t->sharedOutput ? *m_t->sharedOutput : m_t->output.Tex(); }

public:
    // Look parameters (pushed by the adapter from the resolved profile).
    float focusDistance   = 8.0f;   // world units kept sharp
    float focusRange      = 2.0f;   // half-width of the in-focus band
    float transitionRange = 10.0f;  // ramp distance past the band
    float maxRadius       = 8.0f;   // max blur radius in pixels (aperture proxy)

private:
    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    // One view's target: the private output (a PassTexture at the post size, made only when no pool is
    // bound), the pool slot when one is, and the size latch. m_t is the bound view's (BindView); a
    // secondary view's is prepared in its segment, from its own pool.
    struct Target
    {
        RG::PassTexture     output{ RG::PassTextureDesc{ .format    = RHI::Format::R16G16B16A16_FLOAT,
                                                         .size      = RG::SizePolicy::PostAA,
                                                         .debugName = "DepthOfField.Output" } };
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

    // True only for a frame in which Execute actually dispatched. Every early return in
    // Execute (disabled, missing PSO, no input SRV, zero viewport, invalid target) leaves
    // the output holding SOMEBODY ELSE'S pixels: the previous frame's or, in a shared
    // pool slot, another effect's. GetOutputSrvHandle() must not publish that; the
    // adapters' `if (const uint64_t out = ...)` idiom then leaves ctx.hdrSrv untouched
    // and the chain passes through.
    bool m_wroteThisFrame = false;

    uint64_t m_inputSrv = 0;
    uint64_t m_depthSrv = 0;
    float    m_near = 0.1f;
    float    m_far  = 1000.0f;
    bool     m_enabled  = false;
};
