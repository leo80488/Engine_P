#pragma once

// StylizePass - NPR stylization (compute, HDR). One dispatch hosts independently
// toggled features (pixelate, Kuwahara, posterize, halftone, dither, crosshatch and a
// depth / normal source outline). An HDR-space producer like CASPass: reads the
// running HDR colour (t0) and writes an output the Stack swaps into ctx.hdrSrv.
// Driven by the profile's stylize groups via StylizeEffect.

#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's target)
#include "RenderGraph/PassLifecycle.h"
#include "RenderGraph/PassTexture.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class IGraphicsDevice;

class StylizePass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // RG::PrepareStage: at PostPool, learn that the shared post target exists (before
    // the working set, or RebuildTextures strands a private full-res target); at
    // PostEffects, build the working set.
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.stage == RG::PrepareStage::PostPool) { if (c.postPool) SetSharedOutput(c.postPool); }
        else                                       PrepareResources(c.postW, c.postH);
    }
    // It records any view: the target is the bound view's (BindView).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    void BindView(ViewState& view) override;

    void SetInputSrv(uint64_t srv)               { m_inputSrv = srv; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void PrepareResources(uint32_t w, uint32_t h);
    void SetEnabled(bool v) { m_enabled = v; }   // "any feature on"
    bool IsEnabled() const   { return m_enabled; }

    // Feature parameters (pushed by the adapter from the resolved profile).
    bool     pixelateOn = false;  uint32_t pixelSize = 8;
    bool     kuwaharaOn = false;  uint32_t kuwaharaRadius = 3;
    bool     posterizeOn = false; uint32_t posterizeLevels = 6;
    bool     halftoneOn = false;  float    halftoneCell = 6.0f, halftoneAngle = 45.0f;
    bool     ditherOn = false;    uint32_t ditherLevels = 4;
    bool     crosshatchOn = false; float   crosshatchDensity = 0.12f, crosshatchThickness = 0.4f;
    struct alignas(16) SourceOutlineCB {
        uint32_t enabled = 0;
        float depthWidth = .6f, normalWidth = .51f, depthExponent = 5000.f;
        float normalExponent = 50.f, distancePower = 3.f, depthMax = 500.f, highlightU = -1.f;
        float highlightV = -1.f, nearZ = .1f, farZ = 1000.f;
        uint32_t materialCount = 0;
        DirectX::XMFLOAT3 outlineColor = {0,0,0}; float pad0 = 0;
        DirectX::XMFLOAT3 highlightColor = {1.6927f,3.8358f,5.f}; float pad1 = 0;
    } sourceOutline;
    static_assert(sizeof(SourceOutlineCB) == 80);
    void SetGeometryInputs(uint64_t depth, uint64_t normal, uint64_t materials) {
        m_depthSrv = depth; m_normalSrv = normal; m_materialSrv = materials;
    }

    void Execute(RHI::CommandList cl);

    uint64_t GetOutputSrvHandle() const;

private:
    void RebuildTextures();
    bool CreatePipelines(IGraphicsDevice& gfx);

    struct alignas(16) StylizeCB
    {
        uint32_t width;
        uint32_t height;
        uint32_t kuwaharaOn;
        uint32_t posterizeOn;

        uint32_t halftoneOn;
        uint32_t ditherOn;
        uint32_t crosshatchOn;
        uint32_t kuwaharaRadius;

        float    posterizeLevels;
        float    halftoneCell;
        float    halftoneAngle;
        float    ditherLevels;

        float    crosshatchDensity;
        float    crosshatchThickness;
        uint32_t pixelateOn;
        uint32_t pixelSize;
        SourceOutlineCB sourceOutline;
    };
    static_assert(sizeof(StylizeCB) == 144);

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
                                                         .debugName = "Stylize.Output" } };
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

    // ---- Shared post-chain output (ping-pong pool) ----
    // The post chain is linear: each pass reads the previous result and fully overwrites
    // its own output, so any chain needs only two full-res buffers (five private RGBA16F
    // targets: 79 MB at 1080p, ~315 MB at 4K; the pool: 32 MB).
    // A pool rather than placed-resource aliasing: every target is fully written, so
    // plain reuse needs no aliasing barriers and has no undefined-contents rule. The
    // state is the device's book's (RG::TransitionTo), one book per texture: tracking it
    // per pass over shared memory breeds double transitions.
public:
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
    // True only for a frame in which Execute dispatched. Every early return in Execute
    // (disabled, missing PSO, no input SRV, zero viewport, invalid target) leaves the
    // output holding someone else's pixels (the previous frame's or, on a shared pool
    // slot, another effect's): GetOutputSrvHandle() must not publish them. The adapters'
    // `if (const uint64_t out = ...)` idiom then leaves ctx.hdrSrv untouched and the
    // chain passes through.
    bool m_wroteThisFrame = false;

    const RHI::Texture& Out() const { return m_t->sharedOutput ? *m_t->sharedOutput : m_t->output.Tex(); }

    uint64_t m_inputSrv = 0;
    uint64_t m_depthSrv = 0, m_normalSrv = 0, m_materialSrv = 0;
    bool     m_enabled  = false;
};
