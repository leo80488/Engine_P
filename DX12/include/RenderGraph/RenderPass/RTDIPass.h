#pragma once
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RTDICore.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ReSTIRCounters.h"
#include <array>
#include <memory>
#include <utility>
#include <vector>
class TransientAliasArena;

// ReSTIR DI, the SHELL: half-resolution punctual-light RIS with one visibility ray per
// selected sample, NRD and the full-resolution bilateral remodulation. The shell owns
// Classify / Prepare / NRD / Resolve, the result texture, the counters' shell slots and every
// name a pack or the Lighting fold reads; the resampling in between is a core (RTDI::Core)
// chosen by ReSTIR::Backend: the engine core (default) or the RTXDI core.
// TODO(ReSTIR_Dual_Backend_Plan S8): RTXDI DI core optional to 2026-11-15, CLI-only to 2026-12-13, then deleted
// together with the compare mode's shadow instance (@p shadow).
class RTDIPass final : public RG::RenderPass
{
public:
    struct Settings
    {
        float distance=150.f;
        uint32_t spatialPasses=1,spatialRadius=16,spatialK=3,maxM=20,maxAge=63;
        uint32_t candidates=8;   // initial RIS candidates per pixel
        bool reuse=true;
        bool fullResolution=false;
        // RELAX's firefly suppression on the DI specular denoiser: steady lamp highlights under a
        // jittered AA, at the price of small bright ones (metal reads ~16% low against the path
        // tracer in SchoolNight with it). -di_no_nrd_antifirefly forces it off.
        bool specularAntiFirefly=true;
        // The denoiser: Auto = RELAX, and no NRD under ray reconstruction (full-resolution trace);
        // RELAX / REBLUR keep NRD before RR at the configured resolution; None = the raw signal.
        // -di_no_nrd / -di_nrd_reblur pin it from the command line; -di_history_counts (a REBLUR-only
        // diagnostic) pins REBLUR.
        ReSTIR::Denoiser denoiser=ReSTIR::Denoiser::Auto;
        // Which values come from the command line (Renderer::DevPin); only the pin-specific rules
        // read them.
        bool reusePinned=false,spatialPassesPinned=false,spatialRadiusPinned=false,ageCapPinned=false;
    };

    struct ServiceRequest
    {
        Settings        settings;
        ReSTIR::Backend backend = ReSTIR::Backend::Engine;
        bool            raw = false;      // SetRayReconstruction: RR or the path tracer takes the raw estimate
        bool            wanted = false;   // the effective path runs DI (the shadow: compare DI is active)
        bool            retain = false;   // the ladder's path does: keep the working set (budget ladder)
    };

    using SceneData = PathTracePass::FrameData;

    // @p shadow: the compare mode's second instance (RTDI.Shadow): the same shell running the
    // OTHER core in the same frame, writing a private result nobody composites, with its own NRD,
    // counters, log lines ([RTDI_SHADOW], stage=DI.Shadow) and GPU regions (RTDI.Shadow.*).
    RTDIPass(RG::RenderGraph& graph, RG::RGTextureHandle albedo, RG::RGTextureHandle normal,
             RG::RGTextureHandle surface, RG::RGTextureHandle extra, RG::RGTextureHandle depth,
             RG::RGTextureHandle velocity, RG::RGTextureHandle result, bool shadow=false);
    ~RTDIPass() override;
    const char* GetName() const override { return m_shadow ? "RTDI.Shadow" : "RTDI"; }
    bool IsShadow() const { return m_shadow; }
    // The composition the running core implements (the effective one).
    const ReSTIR::DIComposition& GetCoreComposition() const { return m_coreComposition; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;
    void Shutdown(IGraphicsDevice& gfx) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    bool IsActive() const override { return m_ws->active; }
    // At the frame boundary only (OnService, from ServiceEnableGatedResources): it may wait for
    // the GPU. wanted=false releases the core's working set (its reservoirs) unless @p retain: the
    // request still wants the pass and only the budget ladder's Raster stage stopped it, so its
    // recovery probe does not reallocate. The shell's set (NRD pools, the transient textures,
    // counters, the on-demand result) stays resident, as it does across a backend switch.
    void EnsureResources(bool wanted,bool retain=false);
    // A secondary view's working set at its render size while @p wanted (the main view runs ReSTIR DI),
    // released otherwise - its segment, before the graph records. Its core is the main view's core's
    // composition and settings and records with its pipelines; its NRD instance is retired, not drained.
    void EnsureViewResources(uint32_t w,uint32_t h,bool wanted);

    // engine.json / the editor; -di_* command-line values pin the rows.
    void SetSettings(const Settings& s);

    ServiceRequest& Request() { return m_request; }
    // The service point's request: the Renderer fills it from the settings and the frame's plan,
    // OnService applies it. The compare shadow takes the settings and wanted only: its backend is
    // the core the main pass does not run (the board), and it never takes RR's raw mode.
    void OnService(RG::ServiceContext& c) override;
    // Ray reconstruction denoises this pass's output. Under RR the Auto denoiser runs no NRD (the raw
    // packing Lighting reads under -di_no_nrd), and without NRD the trace runs at full resolution -
    // the half-resolution Resolve's upsample is spatially correlated noise RR cannot remove
    // (-di_rr_halfres keeps the configured resolution). Call before SetSettings at the service
    // point; a change rebuilds the working set there (the NRD pools go, or come back) and resets
    // the history.
    void SetRayReconstruction(bool on);
    bool RayReconstructionRaw() const { return m_rrRaw; }
    // The requested core. Applied at the next EnsureResources; a core this build or device cannot
    // provide falls back to the engine core with a reason ([RESTIR_BACKEND], GetBackendStatus()).
    void SetBackend(ReSTIR::Backend requested);
    const ReSTIR::BackendStatus& GetBackendStatus() const { return m_status; }
    const char* GetInertReason() const;
    uint64_t GetUpcomingOutputSrv() const;

    void SetFrameData(const SceneData& fd, bool active, uint32_t worldGrid,
                      uint32_t frame, float jitterX, float jitterY);
    void ResetHistory() { if (m_hist) m_hist->reset=true; }
    // ReSTIR history names surfaces, lights and instances of one view of one world: a cut, a
    // resize and a world boundary drop it (flags only - a world clear runs mid-frame; the
    // reservoirs and denoiser history go at the next Execute); the view reports them
    // (History::Invalidate). Shutdown runs after the GPU drain. The composited pass records any view (a
    // working set per view); the compare shadow the main view only.
    uint32_t HandledEvents() const override { return kShutdown | kView | (m_shadow ? 0u : kViewSafe); }
    // The view's reset flags (RenderGraph/ViewState.h): bound at registration, before Init. The
    // reservoirs stay in the core.
    void BindView(ViewState& view) override;
    // A history reset is pending (consumed by the next Execute).
    bool HistoryResetPending() const { return !m_hist || m_hist->reset; }
    // Lights v2: drop NRD's history only (the reservoirs follow their lights).
    void ResetDenoiserHistory() { if (m_hist) m_hist->nrdReset=true; }
    // The denoiser that runs after the setting, the flags and RR: "relax" / "reblur" / "none".
    // The service point's state.
    const char* EffectiveDenoiserName() const { return (m_flags&1u)?"none":(m_relax?"relax":"reblur"); }
    // The 2x2 trace phase rotates (-di_rotate_phase) only while a temporal integrator (TAA, FSR,
    // DLSS: a jittered camera) averages the four phases; without one it measured worse
    // (tools/verify_restir_di_quality.py, AA None FLIP +43%).
    void SetTemporalAA(bool enabled) { m_temporalAA=enabled; }
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena=arena; }
    void SetBudgetStage(uint32_t stage) { m_budgetStage=stage; if(m_main.core) m_main.core->ApplyBudgetStage(stage); }
    // LightSamplingPass's PDF texture this frame (0 = not built): an RTXDI candidates half
    // samples its local lights from it.
    void SetLightTables(uint32_t pdfIdx, uint32_t pdfSize, uint32_t pdfMips)
    { m_lightPdfIdx=pdfIdx; m_lightPdfSize=pdfSize; m_lightPdfMips=pdfMips; }
    // Whether the running core draws RTXDI candidates, i.e. needs those tables.
    bool WantsLightTables() const { return m_coreComposition.initial==ReSTIR::Backend::Rtxdi; }

private:
    static constexpr uint32_t CounterCount=ReSTIR::Counters::DI::Count; // mirror: Clear in RTDI.cs.hlsl
    using Constants=RTDI::Constants;
    using StageConstants=RTDI::StageConstants;
    using ConstantBlock=RTDI::ConstantBlock;

    enum TextureIndex { DiffIn, SpecIn, Normal, ViewZ, MV, DiffOut, SpecOut, Confidence, TextureCount };

    static constexpr float kDefaultSpecularAA=3.f;   // pass.rtdi.specAA's default; 0 = off

    struct History final : ViewHistory
    {
        explicit History(const char* name) : m_name(name) {}
        const char* Name() const override { return m_name; }
        // Every reason: a hard cut, the camera's cut, a resize, a world clear and a render-path
        // change. The camera's cut alone is the Renderer's cut block: it resets only a pass this
        // frame's plan runs (stage B, mid render half). A backend, settings, denoiser or light-set
        // change and switching on reset it too (the pass's policy).
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        void Invalidate(uint32_t reasons) override
        {
            if (reasons & (HistoryReset::HardCut | HistoryReset::Resize | HistoryReset::WorldClear |
                           HistoryReset::PathChange))
                reset = true;
        }
        void Release(IGraphicsDevice&) override {}

        bool reset    = true;    // the next Execute drops the reservoirs and NRD's history
        bool nrdReset = false;   // ... NRD's alone (a remapped light set)
    private:
        const char* m_name;
    };

    // One view's working set: the shell's transient textures, the ray list, counter and indirect args,
    // its NRD instance and its core (the reservoirs and receivers), the sizes, this frame's inputs and
    // its run state. The main view's textures sit in the alias arena and its core owns the pipelines;
    // a secondary view's are committed and its core records with them.
    struct WorkingSet
    {
        std::array<RHI::Texture,TextureCount> tex;
        std::array<RHI::ResourceState,TextureCount> states{};
        uint32_t placedMask=0,placedEpoch=0;
        RHI::GPUBuffer list,counter,args;
        RHI::ResourceState listState=RHI::ResourceState::UNDEFINED;
        RHI::ResourceState counterState=RHI::ResourceState::UNDEFINED;
        RHI::ResourceState argsState=RHI::ResourceState::UNDEFINED;
        std::unique_ptr<NrdDenoiser> nrd;
        std::unique_ptr<RTDI::Core> core;
        SceneData fd;
        uint32_t w=0,h=0,tw=0,th=0,frame=0,grid=~0u,phase=0;
        uint32_t flags=0;          // a secondary view's flag word (the main view's is m_flags)
        uint64_t generation=0;     // the m_generation its core was made at (a secondary view's)
        float jitterX=0,jitterY=0;
        float rayTMax=0;           // the distance its core was last given (a secondary view's)
        bool relax=true;           // the denoiser it was made with (a secondary view's)
        bool phaseRotating=false,active=false,resourcesReady=false;
    };
    // A secondary view's working set, given back when the view goes (its NRD instance retired).
    struct ViewWork : ViewData
    {
        WorkingSet ws;
        RTDIPass*  owner=nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };

    void ApplyBackend();
    // The setting-driven bits of the flag word (full resolution), after Init
    // rebuilt it from the command line.
    void ApplySettingFlags();
    void LogBackend() const;

    // @p ws given back: the main view's drains the GPU before its NRD pools go, a secondary view's
    // retires them.
    void ReleaseWorkingSet(IGraphicsDevice& gfx,WorkingSet& ws);
    // The bound view's working set at @p w x @p h.
    void BuildWorkingSet(IGraphicsDevice& gfx,uint32_t w,uint32_t h);
    // The bound view's flag word.
    uint32_t BoundFlags() const { return m_ws==&m_main ? m_flags : m_ws->flags; }
    // The flag word without ray reconstruction's raw mode: a secondary view's (one Streamline viewport is
    // the main view's; a view keeps NRD at the configured resolution).
    uint32_t ViewFlags() const;
    // A core of @p composition for @p shellFlags (the engine core when the composition has no RTXDI half).
    static std::unique_ptr<RTDI::Core> MakeCore(uint32_t shellFlags,const ReSTIR::DIComposition& composition);

    // Built on the first EnsureResources(true), not in Init: ~0.25 s of every boot, raster ones
    // included. A failure is not retried until the next Init (a shader hot reload).
    bool BuildPipelines();

    uint32_t m_budgetStage=0;
    bool m_shadow=false;
    const RTDI::RegionNames* m_regions=&RTDI::kMainRegions;
    TransientAliasArena* m_aliasArena=nullptr;   // the main view's textures

    RG::RenderGraph& m_graph;
    RG::RGTextureHandle m_albedo,m_normal,m_surface,m_extra,m_depth,m_velocity,m_result;
    IGraphicsDevice* m_gfx=nullptr;
    // The shell layout (ReSTIR::CreateShellLayout), which every DI core's
    // pipelines are built on too.
    RHI::PipelineLayout m_layout;
    // The shell's entries: Clear, Classify, Prepare, Resolve.
    std::array<RHI::PipelineState,4> m_pso;

    bool m_pipelinesFailed=false;
    FrameCB<ConstantBlock> m_cb;
    // ---- The working sets: the main view's here, a secondary view's in its ViewWork ----
    WorkingSet  m_main;
    WorkingSet* m_ws=&m_main;   // the bound view's (BindView)
    // NRD tears its pools down at once: an instance a view drops waits out the frames in flight that may
    // still read it (OnService counts the frame boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>,uint32_t>> m_retiredNrd;
    // Bumped whenever the main view's core is replaced or given other settings: a secondary view's core made
    // at another generation is made again.
    uint64_t m_generation=1;
    // The main view's counters, read back (the [RTDI] lines).
    RHI::GPUBuffer m_readback[RHI::kFramesInFlight];
    const uint32_t* m_mapped[RHI::kFramesInFlight]{};
    bool m_primed[RHI::kFramesInFlight]{};
    uint32_t m_readbackFrame[RHI::kFramesInFlight]{};
    bool m_readbackReset[RHI::kFramesInFlight]{};
    ReSTIR::Backend m_requested=ReSTIR::Backend::Engine;
    // -di_backend_initial= / -di_backend_reuse=: one half of the composition,
    // over the named request (Count = not given).
    ReSTIR::Backend m_initialHalf=ReSTIR::Backend::Count,m_reuseHalf=ReSTIR::Backend::Count;
    ReSTIR::BackendStatus m_status{};
    // The composition the main view's core runs (the effective one) and the settings it was
    // last given, re-applied to a core that replaces it (ApplyBackend) and given a view's core.
    ReSTIR::DIComposition m_coreComposition{};
    RTDI::CoreSettings m_coreSettings{};
    bool m_haveCoreSettings=false;
    ServiceRequest m_request;   // set by the Renderer, read by OnService
    uint32_t m_lightPdfIdx=0,m_lightPdfSize=0,m_lightPdfMips=0;
    bool m_fullResSetting=false,m_fullResForced=false;
    bool m_rrHalfRes=false;   // -di_rr_halfres: RR keeps the configured resolution (A/B)
    bool m_rrRaw=false,m_noNrdForced=false;   // RR's raw mode (SetRayReconstruction); NRD forced off by -di_debug=1
    bool m_backendDirty=true,m_logBackend=false;
    bool m_relax=true;      // ApplySettingFlags: the denoiser setting unless pinned
    bool m_reblurPinned=false;   // -di_history_counts
    ReSTIR::Denoiser m_denoiserSetting=ReSTIR::Denoiser::Auto;   // Settings::denoiser
    bool m_nrdSplit=true;   // RELAX_DIFFUSE + RELAX_SPECULAR (specular-only anti-firefly)
    bool m_specAntiFireflySetting=true;   // Settings::specularAntiFirefly
    uint32_t m_flags=0;   // the main view's flag word
    float m_rayTMax=150,m_mirrorRoughness=0;
    float m_nrdHitDistScale=1; // Diagnostic: deliberately mismatch NRD and producer A.
    // 2x2 trace phase (half resolution only; fixed unless -di_rotate_phase). m_flags2 carries the
    // Resolve fallback, ray-origin and confidence switches (bits 0-2) and the DI specular AA scale
    // (bits 8-15, x4); Execute adds bit 3 (RR's light distance).
    bool m_rotatePhase=false,m_temporalAA=false;
    uint32_t m_flags2=0;

    float m_specularAA=kDefaultSpecularAA;
    bool m_wanted=false;

    // The view's ReSTIR DI history state (RenderGraph/ViewState.h): the reservoirs and receivers
    // live in the view's core (its working set's; the main view's also owns the backend's pipelines),
    // the denoiser's history in NRD; the shell's side is whether the next Execute drops them. One
    // per view and per instance (the shadow pass has its own).
    History* m_hist = nullptr;   // the bound view's (BindView)
    // Lights v2: translate reservoir light indices through the key map (-di_no_light_translation
    // instead resets on a light-count change only).
    bool m_translateLights=true;
};
