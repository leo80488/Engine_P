#pragma once

// UnderwaterPass - underwater screen distortion + water tint (compute, HDR).
//
// Like CASPass: an HDR-space producer that reads an input SRV and writes a
// same-size R16G16B16A16_FLOAT output, which PostProcess::PostChain swaps in as the
// running ctx.hdrSrv (so tonemap sees the distorted frame). Driven by the
// `underwater` group of ResolvedPostProcessSettings via UnderwaterEffect.
// `time` accumulates internally (AddTime each frame), so the caller threads no clock.

#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's target)
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/PassTexture.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>

class IGraphicsDevice;

class UnderwaterPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Prepare stages: PostPool - learn that the shared post target exists (before the
    // working set, or RebuildTextures strands a private full-res target);
    // PostEffects - the working set.
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
    void SetViewportSize(uint32_t w, uint32_t h);
    void PrepareResources(uint32_t w, uint32_t h);
    void AddTime(float dt)                        { m_time += dt; }

    void SetEnabled(bool v)   { m_enabled = v; }
    bool IsEnabled() const     { return m_enabled; }

    void Execute(RHI::CommandList cl);

    // SRV of the distorted HDR result; 0 unless this frame's Execute dispatched (see
    // m_wroteThisFrame).
    uint64_t GetOutputSrvHandle() const;

    // ---- Shared post-chain output (ping-pong pool) ----
    // The post chain is linear: each pass reads the previous result and fully
    // overwrites its own output, so any chain needs only two full-res buffers (five
    // private RGBA16F targets: 79 MB at 1080p; a 2-buffer pool: 32 MB).
    // A pool rather than placed-resource aliasing: nothing needs preserving, so plain
    // reuse needs no aliasing barriers and no undefined-contents rule. The state is the
    // device's book's (RG::TransitionTo), one book per texture: do not track it per pass
    // while the memory is shared, that is how double transitions happen.
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
    struct alignas(16) UnderwaterCB
    {
        uint32_t width;
        uint32_t height;
        float    time;
        float    strength;

        float    scale;
        float    speed;
        float    tintAmount;
        float    _pad0;

        float    tint[3];
        float    _pad1;
    };

    void RebuildTextures();
    bool CreatePipelines(IGraphicsDevice& gfx);

    const RHI::Texture& Out() const { return m_t->sharedOutput ? *m_t->sharedOutput : m_t->output.Tex(); }

public:
    // Look parameters (pushed by the adapter from the resolved profile).
    float             strength   = 0.012f;  // UV-space wobble amplitude
    float             scale      = 28.0f;   // wave spatial frequency
    float             speed      = 1.5f;    // wave temporal speed
    float             tintAmount = 0.6f;    // 0..1 blend toward tint
    DirectX::XMFLOAT3 tint       = { 0.45f, 0.75f, 0.85f }; // water colour

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
                                                         .debugName = "Underwater.Output" } };
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

    // True only for a frame in which Execute dispatched. Every early return in Execute
    // (disabled, missing PSO, no input SRV, zero viewport, invalid target) leaves the
    // output holding someone else's pixels (the previous frame's, or another effect's in
    // a pool slot). GetOutputSrvHandle() must not publish that; the adapters'
    // `if (const uint64_t out = ...)` idiom then leaves ctx.hdrSrv untouched and the
    // chain passes through.
    bool m_wroteThisFrame = false;

    uint64_t m_inputSrv = 0;
    float    m_time = 0.0f;
    bool     m_enabled  = false;   // off until a volume/profile enables it
};
