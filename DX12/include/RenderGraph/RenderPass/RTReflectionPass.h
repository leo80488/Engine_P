#pragma once

// RTReflectionPass: HYBRID REFLECTIONS in the AMD FidelityFX-HSR architecture
// (register map contract: shaders/HybridReflCommon.hlsli).
//
// A GRAPHICS-queue compute pass between DecalPass and LightingPass running the
// whole chain on the graph's command list:
//
//   Hi-Z build (owned SSRDepthHierarchyPass over the grass-free SSRTraceDepth
//              snapshot - FFX depth hierarchy, .r = max = nearest, reverse-Z)
//   ClearCounters -> ClassifyTiles (8x8 tiles: per-pixel ray routing, FFX
//              hit-counter hysteresis FULL_SW / HALF_SW / FULL_HW, except the
//              deterministic mirror band is HW-only when RT is live; SW + HW
//              ray lists, no-ray pixels zeroed)
//   PrepareArgsSW -> ExecuteIndirect IntersectSW (FFX SSSR Hi-Z traversal,
//              inverted depth; ValidateHit; confident hits fetch the previous
//              frame's scene-color pyramid at hitUV - mv; failed rays APPEND to
//              the HW list)
//   PrepareArgsHW -> ExecuteIndirect IntersectHW (deterministic ray re-derive,
//              inline RayQuery vs the DDGI scene TLAS; screen reuse, else the
//              material-proxy hit shading: sun + shadow ray, local lights, DDGI,
//              reflection probes, sky IBL; miss = sky cube)
//   NRDPack -> NRD REBLUR_SPECULAR -> NRDResolve -> the denoised output
//
// Every view runs it with a working set of its own (WorkingSet); the main view's
// scratch is aliased, and ray reconstruction is the main view's.
//
// NRD is the ONLY denoiser: without it the pass reports !IsUnifiedAvailable()
// and Renderer falls back to the SSR FullFold chain.
//
// The output (rgb = reflected radiance, a = rtConf) is read by Lighting.ps
// through LightCB.rtReflectionIdx and folded into specRadiance BEFORE
// Fresnel / envBRDF.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/SSR/SSRDepthHierarchyPass.h"   // owned Hi-Z build (SW tier)

#include <DirectXMath.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class NrdDenoiser;
class TransientAliasArena;

class RTReflectionPass : public RG::RenderPass
{
public:
    // TWO graph instances of this class exist (as PerObjectShadowPass: one class,
    // two registrations, the name switched on a ctor-set member):
    //   Opaque           - the GBuffer trace LightingPass folds. Owns a Hi-Z
    //                      pyramid for the SW tier, runs NRD.
    //   TranslucentFront - traced FROM the nearest translucent surface: its four
    //                      GBuffer-shaped inputs are the TranslucentFront_* targets
    //                      TranslucentFrontLayerPass stamps (~95% sky, so Classify
    //                      emits rays only on glass). RT-only: the SW tier's
    //                      HR_ValidateHit and the HW tier's screen reuse both
    //                      validate hit pixels against gDepth/gNormal, which would
    //                      be the translucent buffers, so both are off
    //                      (enableScreenTrace 0, prevHdrSrv 0) and every hit is
    //                      proxy-shaded. Denoiser BYPASSED (glass sits in the
    //                      mirror band; the raw trace is the output), no Hi-Z,
    //                      reflection rays trace mask 0x08 so the pane cannot hit
    //                      itself. Output folded by custom_surface.hlsli
    //                      MS_FetchFrontLayerReflection via
    //                      LightCB.fwdReflFrontRtrIdx.
    // The name is load-bearing THREE ways: RenderGraph::SetExternalWait keys on
    // it, the PIX/RenderDoc marker carries it, and GetGPUTimestampResult is
    // first-match-wins, so a shared name would hand the RT budget watchdog a
    // union of both instances.
    enum class Variant : uint8_t { Opaque, TranslucentFront };
    static constexpr const char* kOpaqueName = "RTReflectionPass";
    static constexpr const char* kFrontName  = "RTReflectionFrontLayerPass";

    struct FrameData
    {
        uint64_t tlasVA     = 0;                    // DDGISceneAS TLAS (0 = HW tier off this frame)
        uint64_t instanceVA = 0;                    // DDGIInstanceData upload buffer
        // RTAlphaRecord upload buffer (40 B/instance, same InstanceID() index
        // space) - read per traversal CANDIDATE by the alpha-cutout test.
        uint64_t                  alphaRecordVA = 0;
        uint32_t                  alphaInstanceCount = 0;
        uint64_t bindlessBufferTable = 0;           // t0 space1 (VB/IB)
        uint64_t lightsSrv           = 0;           // cluster GPULight buffer
        // RestirGIResult as the previous frame left it, or 0: the opaque instance
        // only, while ReSTIR GI ran last frame.
        uint64_t restirGiPrevSrv     = 0;
        uint64_t ssrPrevSrv          = 0;           // unused: always 0 (the unified trace owns every eligible pixel)
        // SW (screen-space) tier: Hi-Z march + prev-frame pyramid radiance.
        uint32_t enableScreenTrace   = 0;
        uint64_t skySHSrv            = 0;           // SkyIBL 9 x float4 SH (t22; 0 = none)
        uint64_t prevHdrSrv          = 0;           // last frame's scene-color pyramid (t20; 0 = none)
        uint32_t prevHdrMips         = 0;           // its mip count (SampleLevel clamp)
        uint64_t ddgiVolumeBufSrv    = 0;           // StructuredBuffer<DDGIVolumeGPU>
        uint64_t ddgiIrradianceTable = 0;           // kMaxVolumes-slot SRV tables (slot order)
        uint64_t ddgiDepthTable      = 0;
        uint64_t ddgiProbeDataTable  = 0;
        uint32_t ddgiVolumeCount     = 0;
        uint32_t lightCount          = 0;
        uint32_t worldLightGridIndex = ~0u;
        // Sky radiance cube (miss shading + analytic-hit indirect specular);
        // Renderer binds the shared black fallback in None/FlatColor modes.
        uint64_t skyCubeSrv          = 0;
        uint32_t skyCubeIsReal       = 0;
        uint32_t skyCubeMips         = 1;
        // Reflection probes for analytic-hit indirect specular (Lighting.ps
        // parity). Count forced 0 when either SRV is missing.
        uint64_t probeArraySrv       = 0;   // TextureCubeArray (persistent)
        uint64_t probeBufferSrv      = 0;   // StructuredBuffer (per-frame ring)
        uint32_t probeCount          = 0;
        // The SAME irradiance cube Lighting.ps diffuse-samples (gIrradiance
        // t6) - hit-ambient brightness parity in static-skybox mode.
        uint64_t skyIrrSrv           = 0;
        float    ddgiAONearFieldStrength = 0.4f;
        DirectX::XMFLOAT3 flatAmbient    {0.f, 0.f, 0.f};   // ambientColor x intensity
        // NRD CommonSettings camera state (NON-jittered matrices + jitter).
        DirectX::XMFLOAT4X4 viewMatrix        {};
        DirectX::XMFLOAT4X4 projMatrixNoJitter{};
        float    jitterX = 0.0f;
        float    jitterY = 0.0f;
        DirectX::XMFLOAT4X4 viewProj     {};        // current, jittered
        DirectX::XMFLOAT4X4 prevViewProj {};        // previous, jittered
        DirectX::XMFLOAT3   cameraPos    {};
        DirectX::XMFLOAT3   sunDirection {0.f, -1.f, 0.f};  // from-light (LightCB.lightDir)
        DirectX::XMFLOAT3   sunColor     {};                 // radiance x intensity
        float    nearZ = 0.1f;
        float    farZ  = 1000.f;
        uint32_t frameIndex = 0;
        // Hit-shading brightness parity - sourced from the LightCB CPU mirror.
        float    ddgiDiffuseScale   = 1.0f;   // physical default; Renderer copies LightCB's value every frame
        float    iblStrength        = 1.0f;
        uint32_t iblUseSH           = 0;
    };

    struct ServiceRequest { bool hwTier = false, rr = false; };

    // REBLUR hitDistanceParameters.A = rayTMax * this. 0.075 x 40 == 3.0, the A the
    // REBLUR tuning was made with at a 40 m ray, so that tuning is reproduced exactly
    // there and the roughness-dependent saturation profile keeps the same SHAPE as
    // the range grows.
    static constexpr float kHitDistAScale = 0.075f;

    // traceDepth = grass-free depth snapshot (SSRTraceDepth) - the Hi-Z march
    // source; invalid handle -> the pyramid is built from the main depth.
    // (Unused by the TranslucentFront variant, which builds no pyramid.)
    RTReflectionPass(RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle depth,
                     RG::RGTextureHandle velocity,
                     RG::RGTextureHandle traceDepth,
                     Variant variant = Variant::Opaque);

    const char* GetName() const override
    { return m_variant == Variant::TranslucentFront ? kFrontName : kOpaqueName; }
    Variant GetVariant() const { return m_variant; }
    // The denoised output is this instance's import: RG::PassTex::kRTReflection,
    // or kRTReflectionFront for the translucent front. Created in OnCompile; the
    // graph transitions it (Graph tracking). Lighting and the forward passes
    // declare reading it.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void OnCompile(IGraphicsDevice& gfx) override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // ---- Per-frame inputs, set by Renderer BEFORE m_graph.Execute (the bound view's) ----
    void SetFrameData(const FrameData& fd) { m_ws->fd = fd; }
    // The opaque instance's read of RestirGIResult: declared so the graph puts
    // last frame's GI in a compute-readable state before this pass (RTGI rewrites
    // it later in the frame); an on-demand texture with nothing behind it gets no
    // barrier, and the shader reads it only through a non-zero
    // FrameData::restirGiPrevSrv.
    void SetRestirGIResult(RG::RGTextureHandle h) { m_giResultH = h; }

    // Optional same-frame pyramid built by the earlier SSGIPass. Passing 0
    // restores the owned-build path. Renderer sets this before graph Execute;
    // pass ordering guarantees the borrowed texture is in SRV state by use.
    void SetExternalDepthHierarchy(uint64_t srv, uint32_t mipCount)
    {
        // A borrowed pyramid could resurrect the SW tier on an instance that
        // has no SW ray list - refuse it there (the tier is off by design).
        if (!m_swTierPossible) { m_ws->externalHiZSrv = 0; m_ws->externalHiZMips = 0u; return; }
        m_ws->externalHiZSrv  = srv;
        m_ws->externalHiZMips = srv ? mipCount : 0u;
    }

    // ---- Transient aliasing and the view's history ----
    // Shared transient-aliasing heap (Renderer-owned). Set once at wiring;
    // the main view's 7 frame-transient scratch textures (rawColor/hitDist/
    // specIn/specOut/normalRough/viewZ/mv) are placed into it in OnCompile when
    // available. History textures (denoised/depthHist/hitCounter) and a
    // secondary view's scratch stay committed.
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena = arena; }
    // Main-thread, PRE-GRAPH only: if another resident grew the arena since
    // our OnCompile (epoch mismatch), rebuild the transients onto the current
    // heap so the groups actually share bytes again. Cheap no-op otherwise.
    void EnsureTransientsCurrent(IGraphicsDevice& gfx);
    // FrameEarly (RG::PrepareStage): the transient-alias upkeep. RTR rebuilds
    // only in OnCompile, so when ANOTHER arena resident grew the shared heap
    // (Bloom flipping to display res under FSR2) this moves its placed scratch
    // onto the current heap. A no-op every other frame.
    void OnPrepare(const RG::PrepareContext& c) override { EnsureTransientsCurrent(*c.gfx); }
    // The view's history (RenderGraph/ViewState.h): created here, empty -
    // OnCompile makes its textures (at the main graph's first Compile already,
    // which is why the view is bound at registration). It records any view: a
    // secondary view's working set is sized in its segment (EnsureViewResources).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    // A secondary view's working set at its render size while @p wanted (the main view traces
    // reflections or runs their screen tier), released otherwise - its segment, before the graph
    // records. Its NRD instance is retired, not drained.
    void EnsureViewResources(uint32_t w, uint32_t h, bool wanted);

    // ---- Activation, tiers, denoising and output ----
    // Renderer gates the pass per frame. OFF -> ON drops history (stale).
    void SetActive(bool active)
    {
        if (active && !m_ws->active) ResetHistory();
        m_ws->active = active;
    }
    // RenderGraph queries this in its serial Phase A and SKIPS the pass
    // entirely when false - it must stay a side-effect-free read of pre-graph
    // state (the bound view's flag, pushed by Renderer before m_graph.Execute).
    bool IsActive() const override { return m_ws->active; }
    // The screen-space tier and everything shared are built at Init.
    bool IsReady()  const { return m_ready; }

    // The HARDWARE tier (HybridReflIntersectHW - a 70 KB inline-RayQuery
    // pipeline, ~0.85 s to create) is built the first time RT reflections are
    // wanted, not at Init, so a raster boot does not pay for a tier only RT
    // reflections dispatch. Called at the frame boundary
    // (Renderer::ServiceEnableGatedResources); a failure is not retried until
    // the next shader reload. HasHWTier gates the RT tier.
    bool EnsureHWTier();
    bool HasHWTier() const { return m_intersectHWPSO.IsValid(); }

    // NRD REBLUR_SPECULAR is the only denoiser. Turning it off (or an NRD init
    // failure) makes the unified pass unavailable - Renderer then has Lighting
    // fold the SSR FullFold output instead.
    void SetUseNRD(bool v)
    {
        if (v != m_useNRD) ResetHistory();
        m_useNRD = v;
    }
    bool IsUsingNRD() const { return m_useNRD; }

    // Ray reconstruction denoises the reflections - REBLUR is skipped (NRDPack
    // still runs: it keeps the screen tier's depth history) and the resolve passes
    // the raw trace through, as the translucent front-layer instance always does;
    // REBLUR's pools are released while it lasts. The world hit distance of the
    // trace is copied to a committed texture the post chain reads (RRGuidePass's
    // specular hit distance). Main thread, at the service point.
    void SetRayReconstruction(bool on);

    // The service point: RTReflectionTier builds the hardware tier when wanted
    // and lets go of the NRD instances views retired once their frames drained
    // (both instances); WorkingSets takes RR's raw mode (the main instance; a
    // change allocates or releases, after both tiers).
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext& c) override;
    bool RayReconstructionRaw() const { return m_rrRaw; }
    // The kept hit distance (R16F, |t|: rayTMax on a miss, a negative sign
    // marks a moving reflector), when the main view's last Execute wrote it.
    const RHI::Texture* GetRRHitDistance(RHI::ResourceState& state) const
    {
        if (!RrRawBound() || !m_rrHitDistWritten || !m_rrHitDist.IsValid()) return nullptr;
        state = m_rrHitDistState;
        return &m_rrHitDist;
    }

    // Drop the temporal history next Execute. Callers use this when they change
    // something that alters what the accumulated signal MEANS rather than just
    // its value - e.g. rayTMax, which rescales the hit-distance normalization
    // REBLUR reprojects with.
    void ResetHistory() { if (m_hist) m_hist->reset = true; }

    // True when the pass can actually produce this frame's reflection signal
    // (ready + NRD alive). Renderer consults this for unifiedReflActive and the
    // SSR FullFold fallback decision.
    bool IsUnifiedAvailable() const;

    // SRV of the bound view's denoised output - single non-ping-pong texture
    // written every active frame; stable handle. Renderer plumbs it into
    // LightingPass::SetRTReflection.
    uint64_t GetUpcomingOutputSrv() const;

private:
    struct alignas(16) HybridReflConstants
    {
        float    invViewProj[16];
        float    prevViewProj[16];
        float    viewProj[16];
        float    cameraPos[3];       float    rayTMax;
        float    sunDirection[3];    float    roughnessThreshold;
        float    sunColor[3];        float    mirrorRoughness;
        float    flatAmbient[3];     float    iblStrength;
        uint32_t screenW;            uint32_t screenH;
        float    invScreenW;         float    invScreenH;
        float    nearZ;              float    farZ;
        float    normalBias;         float    ssrConfidenceThreshold;
        float    depthThicknessBase; float    thicknessRoughScale;
        float    thicknessLenFactor; uint32_t maxTraversalIters;
        float    hybridSpawnRate;    float    hybridMissWeight;
        uint32_t tileCountX;         uint32_t tileCountY;
        uint32_t frameIndex;         uint32_t resetHistory;
        uint32_t unifiedFlags;       uint32_t hizMipCount;
        uint32_t ddgiVolumeCount;    uint32_t lightCount;
        float    ddgiDiffuseScale;   float    _padSkyDiffuse;   // padding
        uint32_t hasSkySH;           uint32_t hasPrevHdr;
        float    prevHdrMaxMip;      uint32_t hasSkyCube;
        uint32_t skyCubeMips;        float    ddgiAONearFieldStrength;
        uint32_t probeCount;         uint32_t missEnvFallback;
        // Alpha-cutout occluders: one 16-byte row (384 -> 400). alphaTestEnabled == 0
        // means the RTAlphaRecord table is not live this frame: the shader must not
        // touch g_AlphaRecords at all (the root SRV is bound unconditionally and may
        // be VA 0) and every candidate is accepted (every occluder solid).
        uint32_t alphaTestEnabled;   uint32_t alphaInstanceCount;
        // hitDistParamA mirrors HybridReflCommon.hlsli `float hitDistParamA;`.
        // materialBufIdx: bindless g_Buffers[] slot of the persistent MaterialGPUData
        // table (kInvalidBufferIndex = not bound) - NPR hit shading reads the REAL
        // per-material toon knobs through it.
        float    hitDistParamA;      uint32_t materialBufIdx;
        // NPRStabilize primary-view parity for reflected NPR hits (400 -> 416):
        // the bindless slot of the 1-float AutoExposure buffer, and the
        // scene-wide NPR exposed-space soft ceiling (0 = none). See
        // HybridReflCommon.hlsli for the full rationale.
        uint32_t exposureBufIdx;     float    sceneNprMaxBrightness;
        // rayMask: the reflection ray's InstanceInclusionMask - 0x08 (the GBuffer's
        // surfaces) on both instances.
        uint32_t rayMask;            uint32_t worldLightGridIndex;
        // ReSTIR GI as the previous frame left it (416 -> 432): the environment
        // stand-in's sky visibility and rough band.
        uint32_t restirGiPrevIdx;    uint32_t probeBufferIdx;   uint32_t _giPad[2];
    };
    static_assert(sizeof(HybridReflConstants) == 432,
                  "must mirror HybridReflConstants in HybridReflCommon.hlsli");

    struct History final : ViewHistory
    {
        explicit History(const char* name) : m_name(name) {}
        const char* Name() const override { return m_name; }
        // The content's cuts - a hard cut, the camera's, a world clear (the view
        // tells it) - and a new size (OnCompile notices it). rayTMax, the NRD / RR
        // switches, switching on, a declined frame and a shader reload are the
        // pass's own policy.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) reset = true; }
        void Release(IGraphicsDevice& gfx) override;

        // Prev-frame NDC depth ping-pong: NRDPack writes this frame (u8), the
        // intersect passes read last frame (t21) for prev-radiance validation.
        // Not on the denoiser-bypass (front-layer) instance: the working set's nrdViewZ stands in.
        RHI::Texture       depthHist[2];
        RHI::ResourceState depthHistState[2] = { RHI::ResourceState::SHADER_RESOURCE,
                                                 RHI::ResourceState::SHADER_RESOURCE };

        // FFX hit-counter (R32_UINT, one texel per 8x8 tile) ping-pong: classify
        // reads history (t31) + seeds this frame (u6); IntersectSW accumulates.
        RHI::Texture       hitCounter[2];
        RHI::ResourceState hitCounterState[2] = { RHI::ResourceState::SHADER_RESOURCE,
                                                  RHI::ResourceState::SHADER_RESOURCE };

        uint32_t writeIdx = 0;   // depthHist + hitCounter parity
        bool     reset    = true;
    private:
        const char* m_name;
    };

    // One view's working set: the screen tier's Hi-Z pyramid (and its builder), the
    // ray lists and counters, the 7 frame-transient scratch textures, the denoised
    // output, its NRD instance, the size latch, this frame's inputs and its run
    // state. The main view's scratch sits in the alias arena; a secondary view's is
    // committed.
    struct WorkingSet
    {
        // SW tier: owned Hi-Z fallback / borrowed SSGI Hi-Z. Contract: the pyramid
        // lives in UNORDERED_ACCESS outside the trace read; hiZTexState tracks the
        // UAV <-> SRV flips per frame. The builder binds the SHARED compute root
        // sig -> it must run BEFORE ours is set each frame.
        std::unique_ptr<SSRDepthHierarchyPass> hiZ;
        RHI::ResourceState hiZTexState     = RHI::ResourceState::UNORDERED_ACCESS;
        uint64_t           externalHiZSrv  = 0;
        uint32_t           externalHiZMips = 0;

        RHI::GPUBuffer swRayList;      // uint x W*H (packed pixel per ray)
        RHI::GPUBuffer hwRayList;      // uint x W*H
        RHI::GPUBuffer rayCounter;     // 8 uints (HR_CTR_* slots)
        RHI::GPUBuffer indirectArgs;   // 8 uints: SW args @0, HW args @byte 16

        RHI::Texture       rawColor;   // RGBA16F (radiance, relia)
        RHI::ResourceState rawColorState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       hitDist;    // R16F world hit t
        RHI::ResourceState hitDistState = RHI::ResourceState::SHADER_RESOURCE;
        // NRD (REBLUR_SPECULAR) input/output textures.
        RHI::Texture       nrdNormalRough;   // R10G10B10A2
        RHI::ResourceState nrdNormalRoughState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       nrdViewZ;         // R32F
        RHI::ResourceState nrdViewZState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       nrdMV;            // RG16F
        RHI::ResourceState nrdMVState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       nrdSpecIn;        // RGBA16F
        RHI::ResourceState nrdSpecInState = RHI::ResourceState::SHADER_RESOURCE;
        RHI::Texture       nrdSpecOut;       // RGBA16F
        RHI::ResourceState nrdSpecOutState = RHI::ResourceState::SHADER_RESOURCE;
        uint32_t placedEpoch = 0;   // arena epoch the transients target
        uint8_t  placedMask  = 0;   // bit i = transient i is placed (aliasing barriers only for those)

        // NRD-resolve output - the texture Lighting.ps reads (LightCB.rtReflectionIdx;
        // rgb = denoised radiance, a = rtConf). Single, non-ping-pong.
        RHI::Texture       denoised;
        RHI::ResourceState denoisedState = RHI::ResourceState::SHADER_RESOURCE;

        std::unique_ptr<NrdDenoiser> nrd;   // REBLUR_SPECULAR, made with the working set
        uint32_t  w = 0, h = 0;
        uint32_t  tileX = 0, tileY = 0;
        FrameData fd{};
        bool      active = false;
    };
    // A secondary view's working set, given back when the view goes (its NRD instance retired).
    struct ViewWork : ViewData
    {
        WorkingSet        ws;
        RTReflectionPass* owner = nullptr;
        void Release(IGraphicsDevice& gfx) override;
    };

    bool BuildRootSignature(IGraphicsDevice& gfx);
    bool CreatePipelines(IGraphicsDevice& gfx);   // every pipeline but the hardware tier's
    bool BuildCS(IGraphicsDevice& gfx, const std::string& path, const std::string& entry,
                 RHI::PipelineState& outPSO);

    // The bound view's working set at @p w x @p h. @p mayStall: the frame
    // boundary's (the main view's), which resizes its NRD in place; a view's
    // segment retires its old instance instead.
    void BuildWorkingSet(IGraphicsDevice& gfx, uint32_t w, uint32_t h, bool mayStall);
    void DestroyResolutionResources(IGraphicsDevice& gfx);
    // Every buffer and texture of @p ws but its Hi-Z pyramid (deferred releases);
    // its NRD instance stays the caller's.
    static void DestroyWorkingSet(IGraphicsDevice& gfx, WorkingSet& ws);
    // The 7 frame-transient scratch textures of @p ws - placed into @p arena when
    // there is one (the main view's), committed otherwise. Split out so the
    // working set and EnsureTransientsCurrent share one creation path.
    void CreateTransientTextures(IGraphicsDevice& gfx, WorkingSet& ws, TransientAliasArena* arena);
    static void DestroyTransientTextures(IGraphicsDevice& gfx, WorkingSet& ws);
    void RetireNrd();
    // The import's source: the bound view's denoised output (the graph reads it at each Execute).
    void SyncImports() { m_denoisedImport = m_ws->denoised; }
    // Ray reconstruction denoises the main view's reflections only (one Streamline
    // viewport): a secondary view keeps REBLUR.
    bool RrRawBound() const { return m_rrRaw && m_ws == &m_main; }
    // Every working-set buffer/texture of the bound view is valid. Folded into
    // IsUnifiedAvailable() so a PARTIAL creation failure (VRAM pressure on
    // resize) sends Renderer to the FullFold fallback instead of dispatching
    // through unset descriptor tables (DEVICE_REMOVED), and into the
    // working set's early-out so a failed resource is retried on the next build.
    bool AllResourcesValid() const;
    // The alias arena the bound view's transients go in: the main one, or a secondary view's shared scratch heap
    // (null: committed).
    TransientAliasArena* WorkArena() const;

    // ---- RR hit-distance texture ----
    void EnsureRRHitDistance(IGraphicsDevice& gfx);

public:
    // ---- Tunables ----
    // HW ray leash AND the miss/sky hitDist sentinel in BOTH tiers. 150 so distant
    // geometry does not read as sky: 75% of the default camera far plane
    // (RenderCamera::farZ = 200).
    //
    // MUST be kept in step with the REBLUR normalization: RTReflectionPass
    // derives hitDistParamA = rayTMax * kHitDistAScale and feeds the SAME
    // number to the pack shader and to NrdDenoiser. Changing one alone is
    // silent and global.
    //
    // COST WARNING - this ray is NOT the cheap kind. Unlike the sun-shadow ray
    // (ACCEPT_FIRST_HIT | SKIP_CLOSEST_HIT, no shading), IntersectHW traces
    // RAY_FLAG_NONE - a full closest-hit search - and every ray converted from
    // a miss (one cube sample) into a hit then runs HR_ShadeHit: a secondary
    // 100 wu shadow ray, up to 32 clustered lights, DDGI + probes, bindless
    // attribute fetches. Off-screen far hits also cannot take the cheap
    // prev-frame screen-reuse path. Measure RTReflectionPass GPU ms before
    // raising it - the RT budget watchdog is default-OFF and Windows TDR fires
    // at 2 s on a single dispatch. Live knob: Renderer::SetRTReflectionsRayTMax /
    // EngineSettings "features.rtReflectionsRayTMax" / the "RTR Ray Length"
    // editor slider.
    float    rayTMax            = 150.0f;

    // Trace band gate: pixels rougher than this fall to the probe/sky fill.
    float    roughnessThreshold = 0.55f;
    // At/below -> deterministic mirror reflect(); hybrid classification keeps
    // this band HW-only and NRDResolve preserves its full-resolution current-
    // frame trace instead of spatially filtering it.
    float    mirrorRoughness    = 0.1f;
    float    normalBias         = 0.05f;
    // SW ValidateHit confidence >= this = confident screen hit; below (in
    // hybrid mode) the ray is demoted to the HW list (AMD ssr_confidence).
    float    ssrConfidenceThreshold = 0.9f;
    // ValidateHit dynamic thickness: base + alpha x roughScale + rayLen x lenFactor.
    float    depthThicknessBase  = 0.2f;
    float    thicknessRoughScale = 10.0f;
    float    thicknessLenFactor  = 0.01f;
    uint32_t maxTraversalIters   = 128;
    // FFX hit-counter hysteresis knobs.
    float    hybridSpawnRate  = 0.02f;
    float    hybridMissWeight = 0.5f;
    // Bindless g_Buffers[] slot of the persistent MaterialGPUData table
    // (Renderer::m_materialBufferGPU), set by Renderer each frame; NPR hits in
    // IntersectHW read the REAL per-material toon knobs (ramp colors/texture,
    // nprDirectLightScale, light coupling, min/max brightness) through it.
    // kInvalidBufferIndex = not bound -> NPR hits fall back to default knobs.
    uint32_t materialBufBindless = RHI::kInvalidBufferIndex;
    // Bindless g_Buffers[] slot of the AutoExposure 1-float buffer and the
    // scene-wide NPR ceiling (LightCB.nprMaxBrightness source) - reflected NPR
    // hits replicate NPRStabilize's exposed-space shoulder/ceiling with them.
    uint32_t exposureBufBindless = RHI::kInvalidBufferIndex;
    float    sceneNprMaxBrightness = 0.0f;
    // SSR-only outcomes hand off to the probe -> sky environment IN RADIANCE
    // space (AMD never-black-miss contract): relia stays ~1, so the fold alpha -
    // which REBLUR never filters - does not dither per pixel without TAA, probes
    // fall back where the screen cannot resolve, and mirror sky misses keep full
    // brightness instead of the x iblStrength dim fill. false = conservative
    // zero / conf-0 baseline for A/B.
    bool     missEnvFallback = true;

private:
    ServiceRequest m_request;   // OnService's

    bool m_hwFailed = false;   // EnsureHWTier failed: not retried until ReloadShaders

    RG::RGTextureHandle m_normalH, m_surfaceH, m_depthH, m_velocityH, m_traceDepthH;
    RG::RGTextureHandle m_giResultH;   // RestirGIResult (opaque instance only)
    IGraphicsDevice*    m_gfx = nullptr;

    // ---- Per-instance identity (see Variant) ----
    Variant     m_variant        = Variant::Opaque;
    const char* m_dbgPrefix      = "RTR.";   // debug names + StageTimer regions
    bool        m_ownsHiZ        = true;     // build the SW-tier pyramid at all
    // The SW (screen) tier can run on this instance at all. False for
    // TranslucentFront: no owned Hi-Z AND SetExternalDepthHierarchy is
    // refused, so the SW ray list and the depth-history ping-pong (written only by
    // NRDPack, read only by the screen paths) are never created - ~24 MiB at
    // 1080p that nothing on that instance could write or read. Their root
    // slots are bound to type-exact live stand-ins instead.
    bool        m_swTierPossible = true;
    bool        m_bypassDenoiser = false;    // skip NRDPack/REBLUR, resolve rawColor
    bool        m_rrRaw = false;             // RR: skip REBLUR, resolve rawColor
    bool        m_rrHitDistWritten = false;
    RHI::Texture       m_rrHitDist;          // RR: the kept world hit t (R16F)
    RHI::ResourceState m_rrHitDistState = RHI::ResourceState::SHADER_RESOURCE;

    uint32_t    m_rayMask        = 0x01u;    // reflection-ray InstanceInclusionMask (the ctor sets 0x08)
    const char* m_stageNames[7]  = {};       // ClearCounters..Resolve, prefixed

    // ---- Transient aliasing (see TransientAliasArena.h for the contract): the main view's scratch ----
    TransientAliasArena* m_aliasArena  = nullptr;

    RHI::PipelineLayout m_layout;   // BuildRootSignature
    RHI::PipelineState  m_classifyPSO;
    RHI::PipelineState  m_clearCountersPSO;
    RHI::PipelineState  m_prepareArgsSWPSO;
    RHI::PipelineState  m_prepareArgsHWPSO;
    RHI::PipelineState  m_intersectSWPSO;
    RHI::PipelineState  m_intersectHWPSO;
    RHI::PipelineState  m_nrdPackPSO;
    RHI::PipelineState  m_nrdResolvePSO;

    FrameCB<HybridReflConstants> m_cb;

    // ---- The working sets: the main view's here, a secondary view's in its ViewWork ----
    WorkingSet  m_main;
    WorkingSet* m_ws = &m_main;   // the bound view's (BindView)
    // NRD tears its pool down at once: an instance a view drops waits out the
    // frames in flight that may still read it (OnService counts the frame
    // boundaries down).
    std::vector<std::pair<std::unique_ptr<NrdDenoiser>, uint32_t>> m_retiredNrd;

    // Opt-in measurement of the main view's DISPATCHED primary SW/HW rays, not
    // secondary NEE rays inside hit shading. Read the current slot before reusing
    // its copy.
    RHI::GPUBuffer m_rayReadback[RHI::kFramesInFlight];
    const uint32_t* m_rayReadbackMapped[RHI::kFramesInFlight]{};
    bool m_rayReadbackPrimed[RHI::kFramesInFlight]{};
    uint64_t m_rayReadbackFrame[RHI::kFramesInFlight]{};
    uint64_t m_rayReadbackSequence = 0;

    // The view's reflection history (RenderGraph/ViewState.h): one per view AND
    // per instance (the opaque and the front-layer instance each own one -
    // History<H>(owner) keys on the pass), created empty by BindView; the working
    // set makes its textures (after the scratch and the denoised output). REBLUR
    // keeps its own history in NRD; the size latch stays in the working set.
    History* m_hist = nullptr;   // the bound view's (BindView)

    RHI::Texture        m_denoisedImport;   // the import's source: the bound view's denoised output (SyncImports)
    RG::RGTextureHandle m_denoisedH;        // its import (DeclareImports)

    uint64_t m_bindlessTexTable = 0;               // t0 space2, cached at Init

    bool m_ready        = false;
    bool m_useNRD       = true;
};
