#pragma once

// BloomPass - 13-tap Sledgehammer bloom: 5 downsample steps, then upsamples back to bloom[0].
// The first downsample uses a Karis average to suppress fireflies; upsamples use a 3x3 tent filter
// blended additively. Reads the HDR scene (SetHdrSrvHandle) and leaves the composited bloom in
// bloom[0] (half resolution).

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's chain)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"

class TransientAliasArena;

class BloomPass : public RG::RenderPass
{
public:
    static constexpr int kSteps = 5;

    struct ServiceRequest { uint32_t w = 0, h = 0; };

    const char* GetName() const override { return "BloomPass"; }
    void Setup  (RG::RenderGraphBuilder&) override {}
    void Init   (IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // PostEffects (RG::PrepareStage): the working set at the post chain's size.
    void OnPrepare(const RG::PrepareContext& c) override { PrepareResources(c.postW, c.postH); }

    // Also a service point: a group-D resident of the shared alias heap is prepared before any frame
    // commands record, at the plan's post size.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override { PrepareResources(m_request.w, m_request.h); }
    // Events this pass handles (Renderer::PassSet); it records any view (its chain is the view's).
    uint32_t HandledEvents() const override { return kReloadShaders | kView | kViewSafe; }
    // The main view keeps the chain below; a secondary view keeps its own in its ViewState. Every
    // call after this one is that view's.
    void BindView(ViewState& view) override;

    // Called by Renderer before Execute() each frame.
    void SetHdrSrvHandle(uint64_t handle)      { m_hdrSrvHandle = handle; }
    void SetViewportSize(uint32_t w, uint32_t h) { m_vpW = w; m_vpH = h; }
    void PrepareResources(uint32_t w, uint32_t h);

    // Shared transient-aliasing heap (Renderer-owned, set once at wiring). The chain is
    // frame-transient (fully rewritten by the downsample; last read = ToneMap t1 on the same
    // computeCL), but it is the one cross-queue arena resident: its alias barriers record into
    // computeCL, and correctness rests on the existing fences (computeCL waits preComputeCL;
    // restoreCL waits computeCL).
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena = arena; }

    // After Execute(), this is the final composited bloom SRV (bloom[0]).
    // Pass this to ToneMapPass::SetBloomSrvHandle().
    uint64_t GetBloomSrvHandle() const;

private:
    static_assert(kSteps <= 8, "Chain::placedMask is 8 bits");

    // One view's mip chain: tex[0] = viewport/2 ... tex[4] = viewport/32, and the size it was built
    // for. Only the main view's chain is ever placed in the alias arena.
    struct Chain
    {
        RHI::Texture       tex[kSteps];
        RHI::ResourceState state[kSteps]{};
        uint32_t           w[kSteps]{};
        uint32_t           h[kSteps]{};
        uint32_t           placedEpoch = 0;
        uint8_t            placedMask  = 0;   // bit i = tex[i] is placed
        uint32_t           lastW = 0;
        uint32_t           lastH = 0;
    };
    struct ViewChain : ViewData
    {
        Chain chain;
        void Release(IGraphicsDevice& gfx) override;
    };

    bool CreatePipelines(IGraphicsDevice& gfx);

    // allowPlaced: true only from the main-thread PrepareResources path. Execute's dirty fallback runs
    // on the compute render worker and must create committed textures: placed creation there is a
    // known descriptor race.
    void RebuildTextures(bool allowPlaced = false);
    void DestroyTextures();

    ServiceRequest m_request;   // OnService's
    // The main view's chain here, a secondary view's in its ViewState; m_chain is the bound view's.
    Chain  m_mainChain;
    Chain* m_chain = &m_mainChain;

    // ---- Transient aliasing (contract: TransientAliasArena.h) ----
    TransientAliasArena* m_aliasArena = nullptr;

    RHI::PipelineState m_downsamplePSO;
    RHI::PipelineState m_upsamplePSO;
    ShaderLibrary       m_shaderLib;

    uint64_t m_hdrSrvHandle = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;

    IGraphicsDevice* m_gfx = nullptr;
};
