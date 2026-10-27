#pragma once
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RTGICore.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ReSTIRCounters.h"
#include "Graphics/SharcCache.h"
#include <array>
#include <memory>
#include <utility>
#include <vector>
class TransientAliasArena;

// ReSTIR GI, the SHELL: finite-range diffuse transport, separate from the
// screen-colour SSGI path. The shell owns Trace (candidates + hit shading -> the
// neutral GIReservoir), NRD, Resolve, coverage and the names packs read; the
// reuse is a core (RTGI::Core).
class RTGIPass final : public RG::RenderPass
{
public:
    struct Settings
    {
        float maxDistance=150.f;
        uint32_t tier=1,maxAge=30,spatialRadius=8;
        bool reuse=true;            // temporal (+ spatial) resampling
        bool spatial=true;          // the spatial stage, while reuse is on
        bool fullResolution=false;  // trace at render resolution, not half
        uint32_t initialCandidates=2,emitterCandidates=8,hitExactLights=8;
        bool sharc=false;           // the SHaRC radiance cache at the GI hits
        uint32_t sharcMode=0;       // 0 parity (the Tier 1 result), 1 multi-bounce
        // The denoiser: Auto = RELAX, kept under ray reconstruction; None = the raw
        // signal. -gi_no_nrd / -gi_nrd_reblur also set it; -gi_rr_raw keeps the raw
        // signal under RR.
        ReSTIR::Denoiser denoiser=ReSTIR::Denoiser::Auto;
        // Which values are command-line ones (Renderer::DevPin).
        bool spatialRadiusPinned=false,sharcPinned=false;
    };

    struct ServiceRequest
    {
        Settings        settings;
        ReSTIR::Backend backend = ReSTIR::Backend::Engine;
        bool            raw = false;      // SetRayReconstruction (RR denoises the GI)
        bool            wanted = false;   // the effective path runs GI (the shadow: compare GI is active)
        bool            retain = false;   // the ladder's path does
    };

    struct SharcStatus
    {
        bool requested=false, effective=false;
        const char* reason="off";   // a static string
        uint32_t mode=0;            // 0 parity, 1 multi
        uint64_t bytes=0;
        uint32_t clears=0;
    };

    struct FrameData : PathTracePass::FrameData
    {
        uint64_t volumeSrv=0, irradianceTable=0, depthTable=0, probeTable=0;
        uint32_t volumeCount=0,worldGrid=~0u;
        // GI emitter table (DDGISceneAS): list SRV, area/CDF UAV, row count.
        uint64_t emitterSrv=0, emitterTableUav=0;
        uint32_t emitterCount=0;
    };

    // @p shadow: the compare mode's second instance (RTGI.Shadow): the same shell
    // running the OTHER core in the same frame, writing a private result nobody
    // composites, with its own Trace, NRD, counters, log lines ([RTGI_SHADOW],
    // stage=GI.Shadow) and GPU regions (RTGI.Shadow.*).
    RTGIPass(RG::RenderGraph&, RG::RGTextureHandle albedo, RG::RGTextureHandle normal,
        RG::RGTextureHandle surface, RG::RGTextureHandle depth,
        RG::RGTextureHandle velocity, RG::RGTextureHandle result, bool shadow=false);
    ~RTGIPass() override;
    const char* GetName() const override { return m_shadow ? "RTGI.Shadow" : "RTGI"; }
    bool IsShadow() const { return m_shadow; }
    // The core that runs (the effective backend; the compare mode's shadow
    // runs the other one).
    ReSTIR::Backend GetCoreBackend() const { return m_coreBackend; }
    void Setup(RG::RenderGraphBuilder&) override;
    void Init(IGraphicsDevice&) override;
    void OnCompile(IGraphicsDevice&) override;
    void Shutdown(IGraphicsDevice&) override;
    void ReloadShaders(IGraphicsDevice&) override;
    RHI::CommandList Execute(RHI::CommandList) override;
    bool IsActive() const override { return m_ws->active; }
    // At the frame boundary only; wanted=false RELEASES the core's working
    // set unless @p retain; the shell's set stays resident (see RTDIPass).
    void EnsureResources(bool wanted,bool retain=false);
    // A secondary view's working set at its render size while @p wanted (the main view runs ReSTIR GI without
    // SHaRC), released otherwise - its segment, before the graph records. Its core is the main view's core's
    // backend and settings and records with its pipelines; its NRD instance is retired, not drained.
    void EnsureViewResources(uint32_t w,uint32_t h,bool wanted);

    // engine.json / the editor; the -gi_* command-line pins also set these rows.
    void SetSettings(const Settings& s);

    // The service point's request (RTDIPass's rule: the compare shadow takes the
    // settings and wanted, and runs the core the main pass does not).
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override;
    // SHaRC. A new world: the cache is cleared at the next Execute
    // (Renderer::OnWorldClear: world loads, the editor's Stop). Never on a resize, a
    // camera cut or ResetHistory - the cache is world-space.
    // The compare shadow pass borrows the main pass's cache: only the main pass
    // clears it. Both drop their reservoir history (RTDIPass's rule) - after this,
    // when the view's WorldClear reaches History::Invalidate.
    void OnWorldClear(const RG::WorldClearArgs&) override
    {
        if (!m_shadow) m_sharc.RequestClear("world");
    }
    // The TLAS holds every static occluder again (DDGISceneAS::StaticGeometryEpoch):
    // GI hits shaded while a slab's BLAS was still building took light through it,
    // and the cache would keep that light for hundreds of frames. Only an allocated
    // cache has anything to forget.
    void OnStaticGeometryComplete() { if (m_sharc.IsAllocated()) m_sharc.RequestClear("geometry"); }
    // The compare mode's shadow pass queries the main pass's cache (one cache,
    // updated once a frame by the main pass).
    void SetSharcSource(const RTGIPass* main) { m_sharcSource=main; }

    SharcStatus GetSharcStatus() const;
    // Ray reconstruction denoises this pass's output; NRD (RELAX by default) still
    // denoises at trace resolution before it unless -gi_rr_raw or the denoiser None
    // (ApplySettingFlags), and the trace keeps its configured resolution. Call before
    // SetSettings at the service point.
    void SetRayReconstruction(bool on);
    bool RayReconstructionRaw() const { return m_rrRaw; }
    // The denoiser that runs after the setting, the flags and RR:
    // "relax" / "reblur" / "none". The service point's state.
    const char* EffectiveDenoiserName() const { return (m_flags&1u)?"none":((m_flags&512u)?"relax":"reblur"); }
    // The diffuse hit distance (each pixel's connection distance, R16F,
    // render-sized) Resolve writes while ray reconstruction denoises this pass;
    // left in SHADER_RESOURCE_COMPUTE for DLSS. Null otherwise.
    const RHI::Texture* GetRRDiffuseDistance() const { return m_rrDistance.IsValid()?&m_rrDistance:nullptr; }
    void SetBackend(ReSTIR::Backend requested);
    const ReSTIR::BackendStatus& GetBackendStatus() const { return m_status; }
    // Never clears the SHaRC cache (world-space): a resize, a camera cut and
    // InvalidateTemporalHistory all come here (the view's reasons).
    // -gi_sharc_clear_on_history_reset is the lifecycle pack's reverse arm
    // (restir_gi_sharc_lifecycle).
    void ResetHistory() { if (m_hist) m_hist->reset=m_hist->resetReservoir=true; if(m_sharcClearOnReset) m_sharc.RequestClear("history"); }
    // The world clear is the main pass's SHaRC (world state); the shadow pass
    // has none of its own. Both histories hear the cut, the resize and the
    // world clear from the view (History::Invalidate): kView. Renderer::PassSet
    // delivers the events. The composited pass records any view (a working set per
    // view; SHaRC stays the main view's); the compare shadow the main view only.
    uint32_t HandledEvents() const override
    { return kShutdown | (m_shadow ? 0u : kWorldClear | kViewSafe) | kView; }
    // The view's reset flags and SHaRC camera: bound at registration, before Init.
    // The reservoirs stay in the core, SHaRC (world state) on the main pass.
    void BindView(ViewState& view) override;
    // A history reset is pending (consumed by the next Execute).
    bool HistoryResetPending() const { return !m_hist || m_hist->reset || m_hist->resetReservoir; }
    const char* GetInertReason() const;
    uint64_t GetUpcomingOutputSrv() const;

    void SetFrameData(const FrameData&, bool active, uint32_t frame, float jitterX, float jitterY);
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena=arena; }
    void SetBudgetStage(uint32_t stage) { m_budgetStage=stage; }

private:
    using Constants=RTGI::Constants;

    enum TextureIndex { DiffIn,Normal,ViewZ,MV,DiffOut,Coverage,TextureCount };

    static constexpr uint32_t kCounterCount=ReSTIR::Counters::GI::Count; // mirror: Clear in RTGI.cs.hlsl

    enum SharcPso { SharcClear,SharcUpdate,SharcResolve,SharcPsoCount };

    struct History final : ViewHistory
    {
        History(const char* name, RTGIPass* pass) : m_name(name), m_pass(pass) {}
        const char* Name() const override { return m_name; }
        // Every reason: a hard cut, the camera's cut, a resize, a world clear and a
        // render-path change. The camera's cut alone is the Renderer's cut block: it
        // resets only a pass this frame's plan runs (stage B, mid render half). A
        // backend, settings, denoiser or light-set change and switching on reset it
        // too (the pass's policy).
        uint32_t ResetsOn() const override
        { return HistoryReset::Cuts | HistoryReset::Resize | HistoryReset::PathChange; }
        // The pass's ResetHistory for this view: the flags here and, under
        // -gi_sharc_clear_on_history_reset, the pass's SHaRC.
        void Invalidate(uint32_t reasons) override
        {
            if (reasons & (HistoryReset::HardCut | HistoryReset::Resize | HistoryReset::WorldClear |
                           HistoryReset::PathChange))
            {
                reset = resetReservoir = true;
                if (m_pass->m_sharcClearOnReset) m_pass->m_sharc.RequestClear("history");
            }
        }
        void Release(IGraphicsDevice&) override {}

        bool  reset          = true;   // the next Execute drops NRD's history
        bool  resetReservoir = true;   // ... and the reservoirs
        float sharcPrevCamera[3] = {}; // the camera SHaRC resolved from last frame
        bool  sharcHasPrev   = false;
    private:
        const char* m_name;
        RTGIPass*   m_pass;
    };

    // One view's working set: the shell's transient textures and counters, its NRD instance and its core
    // (the reservoirs and receivers), the sizes, this frame's inputs and its run state. The main view's
    // textures sit in the alias arena and its core owns the pipelines; a secondary view's are committed
    // and its core records with them.
    struct WorkingSet
    {
        std::array<RHI::Texture,TextureCount> tex;
        std::array<RHI::ResourceState,TextureCount> states{};
        uint32_t placedMask=0,placedEpoch=0;
        RHI::GPUBuffer counter;
        RHI::ResourceState counterState=RHI::ResourceState::UNDEFINED;
        std::unique_ptr<NrdDenoiser> nrd;
        std::unique_ptr<RTGI::Core> core;
        FrameData fd{};
        uint32_t w=0,h=0,tw=0,th=0,frame=0;
        uint32_t flags=0;          // a secondary view's flag word (the main view's is m_flags)
        uint32_t tier=0;           // the tier its history was traced with (a secondary view's)
        uint64_t generation=0;     // the m_generation its core was made at (a secondary view's)
        float jitterX=0,jitterY=0;
        float rayTMax=0;           // the distance its history was traced with (a secondary view's)
        bool active=false,resourcesReady=false;
    };
    // A secondary view's working set, given back when the view goes (its NRD instance retired).
    struct ViewWork : ViewData
    {
        WorkingSet ws;
        RTGIPass*  owner=nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };

    void ApplyBackend();
    // The setting-driven bits of the flag word (full resolution, the denoiser, no
    // spatial), after Init rebuilt it from the command line.
    void ApplySettingFlags();
    // The bound view's flag word.
    uint32_t BoundFlags() const { return m_ws==&m_main ? m_flags : m_ws->flags; }
    // The flag word without ray reconstruction's raw mode: a secondary view's (one Streamline
    // viewport is the main view's).
    uint32_t ViewFlags() const;
    // A core of @p backend (the engine core when the RTXDI core is not in this build).
    static std::unique_ptr<RTGI::Core> MakeCore(ReSTIR::Backend backend);

    void LogBackend() const;

    // @p ws given back: the main view's drains the GPU before its NRD pools go, a secondary view's
    // retires them.
    void ReleaseWorkingSet(IGraphicsDevice& gfx,WorkingSet& ws);
    // The bound view's working set at @p w x @p h.
    void BuildWorkingSet(IGraphicsDevice& gfx,uint32_t w,uint32_t h);

    // Built on the first EnsureResources(true), not in Init: these inline RayQuery
    // pipelines would cost ~0.6 s of every boot, raster ones included. A failure is
    // not retried until the next Init (a shader hot reload).
    bool BuildPipelines();

    // ---- SHaRC ----
    // The cache's kernels (RTGISharc.cs.hlsl: Clear, Update, Resolve) and
    // Trace's RTGI_SHARC build; the core's validation build (BuildSharcPipelines).
    bool BuildSharcPipelines();
    void ReleaseSharcPipelines();
    // At the service point (EnsureResources): whether the cache runs this frame
    // and why not; the main pass allocates it, the shadow pass borrows it.
    void ApplySharc();
    bool SharcRuns() const;
    const SharcCache* SharcSourceCache() const { return m_sharcSource?&m_sharcSource->m_sharc:&m_sharc; }

    bool m_shadow=false;
    const RTGI::RegionNames* m_regions=&RTGI::kMainRegions;
    ReSTIR::Backend m_coreBackend=ReSTIR::Backend::Engine;
    // The settings the core was last given, re-applied to a core that
    // replaces it (ApplyBackend).
    RTGI::CoreSettings m_coreSettings{};
    bool m_haveCoreSettings=false;
    ServiceRequest m_request;   // OnService's

    bool m_fullResSetting=false,m_spatialSetting=true;
    bool m_rrRaw=false;                      // SetRayReconstruction: RR denoises the output
    bool m_rrRawGiCli=false;                 // -gi_rr_raw
    ReSTIR::Denoiser m_denoiserSetting=ReSTIR::Denoiser::Auto;   // Settings::denoiser
    RHI::Texture m_rrDistance;               // the DLSS-RR diffuse hit distance
    RHI::ResourceState m_rrDistanceState=RHI::ResourceState::UNORDERED_ACCESS;

    uint32_t m_budgetStage=0;
    TransientAliasArena* m_aliasArena=nullptr;   // the main view's textures

    RG::RenderGraph& m_graph;
    RG::RGTextureHandle m_albedo,m_normal,m_surface,m_depth,m_velocity,m_result;
    IGraphicsDevice* m_gfx=nullptr;
    // The shell layout (ReSTIR::CreateShellLayout), which every GI core's
    // pipelines and the SHaRC builds are built on too.
    RHI::PipelineLayout m_layout;
    // The shell's entries: Clear, Trace, Resolve, EmitterArea, EmitterCdf.
    std::array<RHI::PipelineState,5> m_pso;

    bool m_pipelinesFailed=false;
    FrameCB<Constants> m_cb;
    // ---- The working sets: the main view's here, a secondary view's in its ViewWork ----
    WorkingSet  m_main;
    WorkingSet* m_ws=&m_main;   // the bound view's (BindView)
    // NRD tears its pools down at once: an instance a view drops waits out the frames in flight that may
    // still read it (OnService counts the frame boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>,uint32_t>> m_retiredNrd;
    // Bumped whenever the main view's core is replaced: a secondary view's core made at another
    // generation is made again.
    uint64_t m_generation=1;

    // The main view's counters, read back (the [RTGI] lines).
    RHI::GPUBuffer m_readback[RHI::kFramesInFlight];
    const uint32_t* m_mapped[RHI::kFramesInFlight]{};
    bool m_primed[RHI::kFramesInFlight]{};
    uint32_t m_readbackFrame[RHI::kFramesInFlight]{};
    bool m_readbackReset[RHI::kFramesInFlight]{};
    ReSTIR::Backend m_requested=ReSTIR::Backend::Engine;
    ReSTIR::BackendStatus m_status{};
    bool m_backendDirty=true,m_logBackend=false;

    SharcCache m_sharc;
    SharcCache::Config m_sharcConfig{};
    const RTGIPass* m_sharcSource=nullptr;

    std::array<RHI::PipelineState,SharcPsoCount> m_sharcPso;
    RHI::PipelineState m_traceSharc;
    std::vector<DxcCompiler::Define> m_sharcDefines;
    bool m_sharcWanted=false,m_sharcActive=false,m_sharcPipelinesFailed=false,m_sharcLogPending=true;
    bool m_sharcForceMiss=false,m_sharcBound=false,m_sharcResampling=false,m_tierForced=false;
    bool m_sharcClearOnReset=false;   // diagnostic: -gi_sharc_clear_on_history_reset
    uint32_t m_sharcMode=0,m_sharcMinSamples=2,m_sharcLoggedMode=~0u;
    bool m_sharcPinned=false;   // Settings::sharcPinned: a command-line request
    float m_sharcMaxVoxel=1.f;
    float m_sharcCachedReset=.6f;   // pass.rtgi.sharcCachedReset (the RTGI_SHARC_CACHED_RESET define)
    const char* m_sharcWhy="off";
    const char* m_sharcInert=nullptr;   // SharcCache::InertReason, asked once
    bool m_sharcInertChecked=false;
    uint32_t m_flags=0,m_seed=1,m_tier=1,m_emitterCandidates=8,m_initialCandidates=2,m_hitExactLights=8,m_hitLightSamples=1,
        m_localEmitterLimit=256;
    float m_rayTMax=150,m_constantRadiance=1;
    bool m_wanted=false;

    // The view's ReSTIR GI history state (RenderGraph/ViewState.h): the reservoirs
    // and receivers live in the view's core (its working set's, as for DI), the
    // denoiser's history in NRD, the SHaRC radiance cache - world state, not the
    // view's - on the main pass. The shell's side: whether the next Execute drops them, and the camera
    // SHaRC last resolved from. One per view and per instance (the shadow pass has
    // its own).
    History* m_hist = nullptr;   // the bound view's (BindView)
};
