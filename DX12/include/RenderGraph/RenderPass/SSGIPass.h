#pragma once

// SSGIPass - screen-space diffuse GI, denoised by NVIDIA NRD REBLUR_DIFFUSE.
// A graphics-queue compute pass after DecalPass and before the RT block:
//   Trace+Pack - multi-ray gather, or (reservoir mode) one fresh candidate plus
//                temporal reservoir reuse at 0.25 full-res rpp.
//   Spatial    - optional reservoir reconnection against nearby samples; writes
//                coverage plus the REBLUR radiance / hit-distance input.
//   REBLUR     - NRD REBLUR_DIFFUSE spatial + temporal denoise.
//   Resolve    - denoised near-field irradiance + bilaterally filtered coverage
//                -> one RGBA16F texture.
// Lighting.ps folds the result through LightCB.ssgiResultIdx as a HEMISPHERE
// PARTITION, not a confidence blend:
//     iblDiffuse = ssgi.rgb * ddgiDiffuseScale + farDiffuse * (1 - ssgi.a)
// so the traced tier and the DDGI / sky far field own disjoint solid angles and
// nothing is double-counted; ssgi.a == 0 is exactly the frame without SSGI.
// Do not copy the specular reflection fold's confidence lerp: REBLUR never
// filters the alpha channel, so a per-ray binary confidence there becomes
// salt-and-pepper noise over the whole hemisphere.
//
// Ordering:
//  1. After DecalPass: decals UAV-blend the GBuffer normals the hemispheres and
//     ray origins come from.
//  2. Before RTReflectionPass: while SSGI is live it builds the same-frame
//     grass-free Hi-Z, which the reflection screen tier borrows instead of
//     building its own.
//  3. Before LightingPass: it produces one of its inputs.
// The graph has no compute queue (every pass list is graphics, run serially), so
// this pass's cost is on the critical path: a budget constraint, not something
// to design around with async.
// Register map: shaders/SSGICommon.hlsli (the single source of truth).

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/SSR/SSRDepthHierarchyPass.h"   // OWNED Hi-Z build
#include "Graphics/NrdDenoiser.h"                  // IsReady() in GetInertReason

#include <DirectXMath.h>
#include <memory>

class SSGIPass : public RG::RenderPass
{
public:
    struct ServiceRequest { bool reservoir = false, wanted = false, hiZOnly = false; };

    struct FrameData
    {
        DirectX::XMFLOAT4X4 viewProj{};              // current, jittered
        DirectX::XMFLOAT4X4 prevViewProj{};          // previous, jittered
        DirectX::XMFLOAT4X4 viewMatrix{};            // NRD worldToView (non-jittered)
        DirectX::XMFLOAT4X4 projMatrixNoJitter{};    // NRD viewToClip (non-jittered)
        DirectX::XMFLOAT3   cameraPos{};
        float    jitterX = 0.0f, jitterY = 0.0f;
        float    nearZ = 0.1f;
        float    farZ  = 1000.f;
        uint32_t frameIndex = 0;

        // Previous-frame scene-colour pyramid (SSRSubsystem). 0 => no radiance source
        // => every ray is unresolved, the correct degradation: coverage 0 hands the
        // whole hemisphere to the far field.
        uint64_t prevHdrSrv     = 0;
        float    prevHdrMaxMip  = 0.0f;

        // DDGI - only the calibration path reads these, but they are bound every
        // frame so a debugMode flip needs no re-plumbing.
        uint64_t ddgiVolumeBufSrv   = 0;
        uint64_t ddgiIrradianceTable = 0;   // t0 space4, kMaxVolumes wide
        uint64_t ddgiDepthTable      = 0;   // t0 space5
        uint64_t ddgiProbeDataTable  = 0;   // t0 space6
        uint32_t ddgiVolumeCount     = 0;
        uint32_t ddgiEnabled         = 0;
    };

    // ssrTraceDepth is the GRASS-FREE depth RTReflectionPass also uses. Grass
    // is excluded on purpose: it is dense alpha-tested geometry that would
    // occlude nearly every ground-level diffuse ray at its own blade depth.
    SSGIPass(RG::RGTextureHandle normal,
             RG::RGTextureHandle depth,
             RG::RGTextureHandle velocity,
             RG::RGTextureHandle ssrTraceDepth);

    const char* GetName() const override { return "SSGIPass"; }
    // The folded result and the Hi-Z this pass builds are its imports
    // (RG::PassTex::kSSGIResult / kSSGIHiZ). The graph transitions the result; the
    // Hi-Z stays the pass's (Owner): written and read inside the pass and left
    // readable for RTReflection, an end state the graph's write declaration would
    // not know. Lighting, the forward passes and the opaque RTR declare reading them.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;

    // Enable-gated working set - same contract as RTAOPass::EnsureResources.
    // Called every frame on the MAIN thread with the user's INTENT (the settings
    // flag), never with active: active also requires the output SRV to exist,
    // so gating creation on it would deadlock. @p hiZOnlyWanted requests only the
    // retained depth hierarchy (for GI), without the SSGI / NRD textures.
    void EnsureResources(bool wanted, bool hiZOnlyWanted=false);

    // A secondary view's working set at its render size, after the main view's request (its segment,
    // before the graph records). Its NRD pools are retired, not drained.
    void EnsureViewResources(uint32_t w, uint32_t h);

    // The service point: the reservoir switch, then the working set.
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    void SetFrameData(const FrameData& fd) { m_ws->fd = fd; }

    // The view's histories (RenderGraph/ViewState.h): created empty by BindView;
    // OnCompile makes their textures. It records any view: a secondary view's working set (its Hi-Z
    // builder made when it is first bound) is sized in its segment (EnsureViewResources).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    void SetActive(bool active)
    {
        if (active && !m_ws->active) RestartHistory();
        m_ws->active = active;
    }
    void SetHiZOnly(bool value) { if(value!=m_ws->hiZOnly) RestartHistory(); m_ws->hiZOnly=value; }
    bool IsActive() const override { return m_ws->active; }
    bool IsReady()  const { return m_ready; }

    // Why this pass would do nothing this frame, or nullptr if it is fully
    // armed. Every failure mode here looks exactly like the feature switched off
    // (coverage 0 renders like SSGI disabled), so neither the image nor a boolean
    // answers "is it running?". Renderer logs this on change.
    const char* GetInertReason() const
    {
        if (!m_ready)                    return "Init failed (shader/root-sig)";
        if (!m_gfx)                      return "no device";
        if (!m_ws->resourcesWanted)          return "resources not requested (ssgiEnabled false?)";
        if (!m_ws->result.IsValid())         return "result texture not created";
        if (!m_ws->diffIn.IsValid())         return "NRD input textures not created";
        if (!m_hist)                     return "no view bound (PassSet::BindView)";
        if (!m_hist->depthHistory.IsValid()) return "depth history texture not created";
        if (m_reservoirEnabled &&
            (!m_ws->reservoirTemporal.IsValid() ||
             !m_hist->reservoirHistory[0].IsValid() ||
             !m_hist->reservoirHistory[1].IsValid()))
            return "reservoir scratch textures not created";
        if (!m_cb.IsValid())             return "constant buffer NOT CREATED (b0 unbound; shaders read screenW=0)";
        if (!m_ws->nrd)                      return "NRD instance missing (REBLUR_DIFFUSE resize FAILED)";
        if (!m_ws->nrd->IsReady())           return "NRD not ready";
        if (!m_ws->hiZ)                      return "Hi-Z builder missing";
        if (m_ws->w == 0 || m_ws->h == 0)        return "zero render size";
        return nullptr;
    }
    // Diagnostic: non-zero only when the last Execute reached the end of all
    // active stages. A dispatch-completion estimate, not a GPU hit counter.
    uint32_t GetLastActivePixelEstimate() const { return m_ws->lastDispatchedPixels; }
    // Why Execute bailed on the LAST frame it was called, or nullptr if it ran
    // to completion. Distinct from GetInertReason(), which is a pre-flight
    // check Renderer makes; this is what actually happened inside the graph.
    const char* GetExecBailReason() const { return m_ws->execBailReason; }

    // SRV of the resolved SSGI texture - a stable handle, folded into LightCB by
    // Renderer. 0 while the working set does not exist.
    uint64_t GetUpcomingOutputSrv() const;

    // Same-frame grass-free Hi-Z built by this pass. RTReflectionPass runs later
    // in the graph and may borrow it instead of building its own, while SSGI stays
    // independent of the reflection settings.
    uint64_t GetDepthHierarchySrv() const
    {
        return (m_ready && m_ws->hiZ) ? m_ws->hiZ->GetSrvHandle() : 0;
    }
    uint32_t GetDepthHierarchyMipCount() const
    {
        return (m_ready && m_ws->hiZ) ? m_ws->hiZ->GetMipCount() : 0;
    }

    // ---- Tunables (ECS IndirectLightingSettingsComponent -> Renderer) ----
    void SetReservoirEnabled(bool enabled)
    {
        if (enabled != m_reservoirEnabled && m_hist)
        {
            m_hist->reset = true;
            m_hist->reservoirValid = false;
        }
        m_reservoirEnabled = enabled;
    }
    bool IsReservoirEnabled() const { return m_reservoirEnabled; }

private:
    struct alignas(16) SSGIConstants
    {
        float    invViewProj[16];
        float    viewProj[16];
        float    cameraPos[3];      float    maxDistance;
        float    _pad0[3];          float    thickness;
        uint32_t screenW;           uint32_t screenH;
        float    invScreenW;        float    invScreenH;
        float    nearZ;             float    farZ;
        uint32_t frameIndex;        uint32_t rayCount;
        uint32_t hizMipCount;       uint32_t maxTraversalIters;
        uint32_t hasPrevHdr;        float    prevHdrMaxMip;
        uint32_t ddgiVolumeCount;   uint32_t ddgiEnabled;
        float    normalBias;        float    hitDistParamA;
        uint32_t debugMode;         uint32_t covHistoryValid;
        float    historyJitterDelta[2];
        uint32_t traceW;            uint32_t traceH;
        float    invTraceW;         float    invTraceH;
        float    prevViewProj[16];
        float    prevInvViewProj[16];
        uint32_t reservoirEnabled;
        uint32_t reservoirHistoryValid;
        uint32_t reservoirSpatialCandidates;
        uint32_t reservoirMaxM;
        uint32_t reservoirMaxAge;
        uint32_t reservoirValidationSteps;
        float    reservoirNormalThreshold;
        float    reservoirDepthThreshold;
    };
    static_assert(sizeof(SSGIConstants) == 416,
                  "must mirror cbuffer SSGIConstants in shaders/SSGICommon.hlsli");

    struct ReservoirSet
    {
        RHI::Texture direction; // RGBA16F: oct direction, selected hitT/confidence
        RHI::Texture radiance;  // RGBA16F: selected RGB, cumulative W
        RHI::Texture aux;       // RG16F: all-path mean hit distance, reserved
        RHI::Texture meta;      // R32_UINT: receiver normal + M + selected age
        RHI::ResourceState directionState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::ResourceState radianceState  = RHI::ResourceState::SHADER_RESOURCE;
        RHI::ResourceState auxState       = RHI::ResourceState::SHADER_RESOURCE;
        RHI::ResourceState metaState      = RHI::ResourceState::SHADER_RESOURCE;

        bool IsValid() const
        {
            return direction.IsValid() && radiance.IsValid() &&
                   aux.IsValid() && meta.IsValid();
        }
    };

    struct History final : ViewHistory
    {
        // Temporally accumulated coverage, full res, ping-ponged. Coverage is the
        // ONLY part of the signal REBLUR does not see, so without its own history
        // the blend weight between SSGI and the far field flickers at the ray-count
        // quantum while the radiance it weights sits still.
        RHI::Texture       covHistory[2];
        RHI::ResourceState covHistoryState[2] = { RHI::ResourceState::SHADER_RESOURCE,
                                                  RHI::ResourceState::SHADER_RESOURCE };
        uint32_t           covWrite = 0;

        // Previous full-res depth validates radiance reprojection at the HIT, not
        // merely at the shading pixel. A single texture is sufficient: trace reads
        // last frame first, then resolve overwrites every pixel for next frame.
        RHI::Texture       depthHistory;
        RHI::ResourceState depthHistoryState = RHI::ResourceState::SHADER_RESOURCE;
        bool               depthValid = false;
        float              jitterX = 0, jitterY = 0;   // the jitter the history was traced with

        ReservoirSet reservoirHistory[2];
        uint32_t     reservoirWrite = 0;
        bool         reservoirValid = false;

        bool reset = true;   // the next Execute discards every history (and NRD's)

        const char* Name() const override { return "SSGI"; }
        // The content's cuts (a hard cut, the camera's, a world clear - the view
        // reports them at the render frame's start) and a new size (OnCompile notices
        // it). Switching the pass on, the Hi-Z-only toggle, the reservoir switch, the
        // debug mode and a shader reload restart it too - the pass's policy.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) Restart(); }
        void Release(IGraphicsDevice& gfx) override;
        // Every history restarts (RestartHistory; the camera's cut).
        void Restart()
        {
            reset = true;
            depthValid = false;
            reservoirValid = false;
        }
    };

    // One view's working set: its Hi-Z builder, the half / full-res textures, the reservoir scratch, its
    // NRD instance, the size latch, this frame's camera data and its run state.
    struct WorkingSet
    {
        // Owned, not borrowed. RTReflectionPass::GetDepthHierarchySrv() is valid as
        // soon as EnsureTexture has run, but that pass builds the hierarchy only while
        // its screen tier is live (SSR toggle + a prev-frame pyramid + a built Hi-Z);
        // in any other reflection configuration a borrowed one is never written - no
        // error, coverage 0, an image identical to SSGI off.
        std::unique_ptr<SSRDepthHierarchyPass> hiZ;
        RHI::ResourceState hiZState = RHI::ResourceState::UNORDERED_ACCESS;
        bool               resourcesWanted = false;

        // Near radiance lives in diffIn; only the scalar confidence coverage
        // crosses from trace to resolve (a second RGBA16F copy would cost 8 B per
        // trace pixel of write + read for no consumer).
        RHI::Texture       raw;            // R16F - raw confidence coverage
        RHI::ResourceState rawState       = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       normalRough;    // R10G10B10A2 - IN_NORMAL_ROUGHNESS
        RHI::ResourceState normalRoughState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       viewZ;          // R32F - IN_VIEWZ
        RHI::ResourceState viewZState     = RHI::ResourceState::SHADER_RESOURCE;
        // IN_MV: REBLUR_DIFFUSE's temporal-stabilization pass declares IN_MV as an
        // OUTPUT, so this texture must carry UNORDERED_ACCESS. Do not copy RTAOPass's
        // SRV-only MV texture (REBLUR_DIFFUSE_OCCLUSION has no such pass): it fails at
        // NRD bind time.
        RHI::Texture       mv;             // RG16F - IN_MV (UAV-capable)
        RHI::ResourceState mvState        = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       diffIn;         // RGBA16F - IN_DIFF_RADIANCE_HITDIST
        RHI::ResourceState diffInState    = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       diffOut;        // RGBA16F - OUT_DIFF_RADIANCE_HITDIST
        RHI::ResourceState diffOutState   = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       result;         // RGBA16F - the folded texture
        RHI::ResourceState resultState    = RHI::ResourceState::SHADER_RESOURCE;

        ReservoirSet reservoirTemporal;

        std::unique_ptr<NrdDenoiser> nrd;   // REBLUR_DIFFUSE

        uint32_t w = 0, h = 0;          // FULL render size (result + resolve)
        // HALF size - the trace, the NRD guide quartet and REBLUR all run here: 4
        // rays/px at half res is 1 ray per full-res pixel of work, and each full-res
        // output pixel is reconstructed from a 3x3 half-res neighbourhood, so it draws
        // on far more rays than it pays for.
        uint32_t tw = 0, th = 0;

        FrameData fd{};
        bool active    = false;
        bool hiZOnly   = false;
        bool hiZWanted = false;
        uint32_t    lastDispatchedPixels = 0;  // cleared on Execute entry, set at its end
        const char* execBailReason       = "Execute never called by the graph";
    };
    struct ViewWork : ViewData
    {
        WorkingSet ws;
        SSGIPass*  owner = nullptr;   // where its NRD instance is retired
        void Release(IGraphicsDevice& gfx) override;
    };

    // REBLUR hitDistanceParameters.A, derived from maxDistance so a ray that
    // reaches its cap normalises to exactly 1. Mirrored into
    // NrdDenoiser::Params::diffHitDistScaleA: the two must be the same number (a
    // mismatch is silent and global).
    float HitDistParamA() const { return maxDistance; }

    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreatePipelines(IGraphicsDevice& gfx);
    // The bound view's working set at @p w x @p h (OnCompile's body). @p mayStall: the main view's, at
    // the frame boundary, may drain the GPU before reusing its NRD pool; a secondary view's, in its
    // segment, retires the pool and makes a fresh instance instead.
    void BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall);
    void RetireNrd();
    void SyncImports();
    void DestroyReservoirResources(IGraphicsDevice& gfx);
    void DestroyResolutionResources(IGraphicsDevice& gfx);

    // Every history restarts: switching on, the Hi-Z-only toggle (the camera's
    // cut: History::Invalidate).
    void RestartHistory()
    {
        if (m_hist) m_hist->Restart();
    }

public:
    // ---- Tunables (ECS IndirectLightingSettingsComponent -> Renderer) ----
    // Screen-march world range. Short on purpose: this tier supplies the near field
    // the probe grid cannot resolve.
    float    maxDistance = 3.0f;
    // Depth-buffer thickness floor (world units) for the uncertainty gate.
    float    thickness   = 0.15f;
    float    normalBias  = 0.05f;
    // Cosine rays per pixel per frame in non-reservoir mode. Reservoir mode always
    // emits one fresh ray per half-res pixel (= 0.25 full-res rpp).
    uint32_t rayCount    = 4;
    uint32_t maxTraversalIters = 64;
    // 0 = normal, 1 = calibration (bypass the march, emit the far field at
    // coverage 1 - the frame must be unchanged), 2..4 = binding / coverage /
    // near-field debug views.
    uint32_t debugMode   = 0;

    uint32_t reservoirSpatialCandidates = 6; // 0..8; the centre temporal sample is separate
    uint32_t reservoirMaxM = 16;             // 1..63, packed into six bits
    uint32_t reservoirMaxAge = 4;            // conservative screen-space lifetime
    uint32_t reservoirValidationSteps = 6;   // clamped to 4..6 in shader
    float reservoirNormalThreshold = 0.90f;
    float reservoirDepthThreshold = 0.01f;

private:
    ServiceRequest m_request;   // OnService's

    RG::RGTextureHandle m_normalH, m_depthH, m_velocityH, m_ssrTraceDepthH;
    // The imports' sources: copies of the bound view's result and Hi-Z (SyncImports), which the graph
    // takes at each Execute's start.
    RHI::Texture        m_resultImport;
    RHI::Texture        m_hiZOut;
    RG::RGTextureHandle m_resultH, m_hiZH;     // the imports
    IGraphicsDevice*    m_gfx = nullptr;

    RHI::PipelineLayout m_layout;
    RHI::PipelineState  m_tracePSO;
    RHI::PipelineState  m_reservoirSpatialPSO;
    RHI::PipelineState  m_resolvePSO;

    FrameCB<SSGIConstants> m_cb;

    // The main view's working set; m_ws is the bound view's (BindView).
    WorkingSet  m_mainWs;
    WorkingSet* m_ws = &m_mainWs;
    // A secondary view's NRD instances replaced or released mid-frame, and the frame boundaries left
    // until nothing in flight reads their pools (OnService counts down): NRD tears a pool down at once.
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>, uint32_t>> m_retiredNrd;

    // The view's SSGI history (RenderGraph/ViewState.h): one per view, created
    // empty by BindView; OnCompile makes its textures, interleaved with the working
    // set. REBLUR's own temporal state stays inside NRD.
    History* m_hist = nullptr;   // the bound view's (BindView)

    bool m_reservoirEnabled = false; // opt-in; the multi-ray mode is the safe fallback
    uint32_t m_lastDebugMode = ~0u;
    bool m_ready = false;
};
