#pragma once

// XeGTAOPass - screen-space Ground Truth Ambient Occlusion (compute).
//
// A graphics-queue compute pass in RTAOPass's slot of the render graph: after DecalPass (so it
// reads the post-decal GBuffer normals) and before LightingPass, which samples the result at
// gSSAO (t18) in the same frame - the previous frame's AO would lag on moving silhouettes.
//
// Four dispatches per frame:
//   1. Prefilter - 5-mip linear depth pyramid
//   2. Main      - horizon-based AO from depth + normals -> raw AO (R8_UNORM)
//   3. Denoise   - edge-aware bilateral blur -> denoised AO (R8_UNORM)
//   4. Temporal  - AO-only ping-pong accumulation (the pass output)

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTexture.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>

class TransientAliasArena;

class IGraphicsDevice;

class XeGTAOPass : public RG::RenderPass
{
public:
    XeGTAOPass(RG::RGTextureHandle normal,
               RG::RGTextureHandle depth,
               RG::RGTextureHandle velocity);

    const char* GetName() const override { return "XeGTAOPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init(IGraphicsDevice& gfx) override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // AO-source decision (the frame plan's xegtao), pushed by Renderer before m_graph.Execute: false
    // when SSAO is off, the path tracer replaces the raster frame, the view is orthographic (the
    // view-space math is invalid) or RTAO owns gSSAO this frame (a second AO chain would waste four
    // full-res dispatches). The graph skips the pass in its serial phase.
    void SetActive(bool a) { m_w->active = a; }
    bool IsActive() const override { return m_w->active; }

    // Per-frame setters (called by Renderer before m_graph.Execute).
    void SetDepthSrvHandle(uint64_t h)    { m_w->depthSrvHandle    = h; }
    void SetNormalSrvHandle(uint64_t h)   { m_w->normalSrvHandle   = h; }
    void SetVelocitySrvHandle(uint64_t h) { m_w->velocitySrvHandle = h; }
    void SetViewportSize(uint32_t w, uint32_t h);
    void PrepareResources(uint32_t w, uint32_t h);

    // Shared transient-aliasing heap (Renderer-owned, set once at wiring). The four frame
    // transients (linearDepth pyramid, aoRaw, edges, aoFinal) are placed in it only when
    // RebuildTextures runs on the main thread (PrepareResources); Execute's dirty fallback runs on
    // a Phase-B worker and creates committed textures. The history ping-pongs stay committed.
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena = arena; }

    // Releases the full-res working set (~36 MiB at 1080p) while SSAO is off. Not released just
    // because RTAO is on: XeGTAO is RTAO's same-frame fallback (empty TLAS, promote fence missing),
    // which the frame plan can pick on any frame. The next PrepareResources rebuilds (texDirty).
    // No-op when already released.
    void ReleaseResources();
    // Frame-early prepare (RG::PrepareStage), on the main thread before any worker kicks: builds the
    // textures whenever SSAO is on at all - releasing them under an enabled but stalled RTAO would
    // hand Lighting a destroyed SRV - and releases them when SSAO is off.
    void OnPrepare(const RG::PrepareContext& c) override
    {
        if (c.ssao) PrepareResources(c.renderW, c.renderH);
        else        ReleaseResources();
        // Flip the history after a frame that wrote, before anything binds Cur (GetUpcomingAOSrv) or
        // the graph reads it.
        if (m_hist) m_hist->tex.BeginFrame();
        SyncImports();
    }
    // Imports the AO history (ImportDeclarer::ImportHistory, graph-tracked): RG::PassTex::kXeGTAO is
    // Cur, the slot this frame writes, which Lighting and the forward passes read; kXeGTAOPrev is
    // Prev, which the temporal dispatch reads.
    void DeclareImports(RG::ImportDeclarer& d) override;
    // Binds the view's AO history (RenderGraph/ViewState.h), created empty here, and its working set;
    // RebuildTextures makes their textures. It records any view: the setters write the bound view's
    // (the render half), the prepare sizes it at the view's render size.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;
    // Projection + view matrices (row-major XMFLOAT4X4 from XMMatrixPerspectiveFovLH / LookToLH;
    // reversed-Z and standard-Z both work, the depth unpack detects handedness from the sign of the
    // Z coefficients). viewMatrix turns GBuffer world normals into view space; frameCounter picks
    // the noise index.
    void SetProjectionMatrix(const DirectX::XMFLOAT4X4& projMatrix,
                             const DirectX::XMFLOAT4X4& viewMatrix,
                             uint32_t frameCounter);

    // Records the four dispatches onto @p cl, a graphics-queue list: Lighting.ps reads the result
    // later on the same queue.
    RHI::CommandList Execute(RHI::CommandList cl) override;

    // SRV of the AO the last Execute wrote (the history's newest slot). Debug / readback only:
    // LightingPass must not bind it - before this frame's Execute it is last frame's AO.
    uint64_t GetAOSrvHandle() const;

    // SRV of the history slot this frame's Execute writes (Cur), bindable before the dispatch is
    // recorded: the history flip in OnPrepare fixes the slot for the whole frame. This is what
    // LightingPass::SetSSAOHandle takes. 0 when this frame's AO will not be written there: no view
    // bound, the textures not made yet (texDirty) or a missing PSO.
    uint64_t GetUpcomingAOSrv() const;

    // The AO this frame wrote and its device-tracked state, for the debug target view and capture
    // {"attachment": "ssao"}. It is the history slot, not aoFinal, which lives in the alias heap
    // and may hold another resource's bytes by the end of the frame. Null when the pass did not run
    // this frame.
    const RHI::Texture* GetDebugOutput(RHI::ResourceState& state) const;

    // The player's AO quality (settings.json features.aoQuality; Renderer pushes it every frame):
    // 0 Low 1x2, 1 Medium 2x2, 2 Scene - sliceCount x stepsPerSlice above, written by the scene's
    // render features config and World Settings - 3 Ultra 9x3.
    void SetQualityLevel(int q) { m_qualityLevel = q; }
    // Slice and step counts the main dispatch uses this frame.
    void EffectiveCounts(uint32_t& slices, uint32_t& steps) const
    {
        switch (m_qualityLevel)
        {
        case 0:  slices = 1; steps = 2; return;
        case 1:  slices = 2; steps = 2; return;
        case 3:  slices = 9; steps = 3; return;
        default: slices = sliceCount; steps = stepsPerSlice; return;
        }
    }

private:
    static constexpr uint32_t kCBTotalSize = 256 * 8;
    struct alignas(256) GTAOCBPool { uint8_t bytes[kCBTotalSize]; };

    static constexpr uint32_t kDepthMipLevels = 5;

    struct History final : ViewHistory
    {
        // The temporal ping-pong (RG::HistoryTexture): Cur is the slot this frame writes, Prev the slot
        // the last Execute wrote (the history it reads); both planes flip together in OnPrepare after a
        // frame that wrote. Their states are tracked by the device.
        //   plane 0: the AO-only accumulation, the pass output - Cur feeds Lighting this frame and is
        //            next frame's history.
        //   plane 1: the linear depth written with it (R32F, mip-0 size), at the same slot. The
        //            temporal pass rejects history whose depth at the reprojected UV disagrees with the
        //            current depth (a disocclusion: a hand reveals background, a silhouette slides
        //            across): alpha drops toward 0 and the ghost dies in one frame instead of 10-20.
        RG::HistoryTexture tex{
            RG::PassTextureDesc{ .format = RHI::Format::R8_UNORM,  .debugName = "XeGTAO.AOHistory" },
            RG::PassTextureDesc{ .format = RHI::Format::R32_FLOAT, .debugName = "XeGTAO.PrevLinearDepth" } };
        bool valid = false;   // false until an Execute wrote Cur

        const char* Name() const override { return "XeGTAO"; }
        // Reset on the content's cuts (a hard cut, the camera's, a world clear: Execute reads the view's
        // FrameBreaks and uses no history that frame) and on a resize (RebuildTextures notices it).
        // SSAO switched off releases it.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t) override {}
        void Release(IGraphicsDevice& gfx) override;
    };

    // One view's working set: the render-size transients, the size latch, its camera and GBuffer
    // SRVs, and whether it runs this frame. The main view's transients are placed in the alias
    // arena; a secondary view's in the views' shared scratch heap (WorkArena).
    struct Work
    {
        // 5-mip linear depth pyramid, written by the prefilter's single dispatch (mips 0..4 bound as
        // u0..u4 space2). The main pass samples it with SampleLevel and a log2-based mip choice (the
        // reference's distant-sample MIP optimization).
        RHI::Texture       linearDepth;
        RHI::ResourceState linearDepthState = RHI::ResourceState::UNORDERED_ACCESS;

        // AO textures: raw (main pass output) and denoised (denoise output, the temporal input).
        RHI::Texture       aoRaw;
        RHI::Texture       aoFinal;
        RHI::Texture       edges;               // packed 4-edge mask (R8_UNORM)
        RHI::ResourceState aoRawState   = RHI::ResourceState::UNORDERED_ACCESS;
        RHI::ResourceState aoFinalState = RHI::ResourceState::UNORDERED_ACCESS;
        RHI::ResourceState edgesState   = RHI::ResourceState::UNORDERED_ACCESS;

        // Per-frame inputs (set by Renderer).
        uint64_t depthSrvHandle    = 0;
        uint64_t normalSrvHandle   = 0;
        uint64_t velocitySrvHandle = 0;
        uint32_t vpW = 0, vpH = 0;
        bool     texDirty = true;
        bool     active   = false;

        // Cached projection / view matrices and frame counter for the CB fill.
        DirectX::XMFLOAT4X4 projMatrix{};
        DirectX::XMFLOAT4X4 viewMatrix{};
        uint32_t            frameCounter = 0;

        // Placement of the transients (contract: TransientAliasArena.h).
        uint32_t            placedEpoch = 0;
        uint8_t             placedMask  = 0;   // bit: 0=linearDepth 1=aoRaw 2=edges 3=aoFinal
    };
    struct ViewWork : ViewData
    {
        Work work;
        void Release(IGraphicsDevice& gfx) override;
    };

    // GTAOConstants: the verbatim layout of Intel's XeGTAO.h; must match the cbuffer in
    // XeGTAO.hlsli exactly. Do not rearrange fields.
    struct alignas(16) GTAOConstants
    {
        int32_t  ViewportSize[2];
        float    ViewportPixelSize[2];

        float    DepthUnpackConsts[2];
        float    CameraTanHalfFOV[2];

        float    NDCToViewMul[2];
        float    NDCToViewAdd[2];

        float    NDCToViewMul_x_PixelSize[2];
        float    EffectRadius;
        float    EffectFalloffRange;

        float    RadiusMultiplier;
        float    Padding0;
        float    FinalValuePower;
        float    DenoiseBlurBeta;

        float    SampleDistributionPower;
        float    ThinOccluderCompensation;
        float    DepthMIPSamplingOffset;
        int32_t  NoiseIndex;

        // Engine extension: the reference passes slice / step counts as HLSL template parameters;
        // here they are CB fields.
        uint32_t SliceCount;
        uint32_t StepsPerSlice;
        float    _pad1[2];

        // Engine extension: world -> view matrix for the GBuffer normal transform.
        float    ViewMatrix[16];
    };
    static_assert(sizeof(GTAOConstants) % 16 == 0, "GTAOConstants must be 16-byte aligned");

    struct alignas(16) DenoiseCB
    {
        uint32_t viewportWidth;
        uint32_t viewportHeight;
        float    depthLinearizeMul;
        float    depthLinearizeAdd;
        float    denoiseBlurBeta;
        uint32_t finalApply;          // 0 = intermediate pass; 1 = final pass
        float    _pad[2];
    };

    struct alignas(16) TemporalCB
    {
        uint32_t viewportWidth;
        uint32_t viewportHeight;
        float    historyAlpha;        // 0 = first frame / accumulation disabled
        float    rejectionDiff;       // |curr-hist| threshold for smooth rejection
        float    _pad[4];
    };

    // allowPlaced: true only from the main-thread PrepareResources path. Placed creation while a
    // Phase-B worker records races on descriptors, so Execute's dirty fallback passes false
    // (committed).
    void RebuildTextures(bool allowPlaced = false);
    // Transient subset only (linearDepth / aoRaw / edges / aoFinal): the arena-epoch refresh
    // re-places these and the committed AO history survives the heap re-layout.
    void RebuildTransients(bool allowPlaced = false);
    // Copies the bound view's history slots into the import sources.
    void SyncImports();
    void CreatePipelines(IGraphicsDevice& gfx);

public:
    // Tuning (public for the editor UI). Defaults are Intel's reference GTAOSettings at
    // QualityLevel=2 (HIGH), the XE_GTAO_DEFAULT_* constants of XeGTAO.h. Slices x steps per
    // preset: LOW 1x2, MEDIUM 2x2, HIGH 3x3, ULTRA 9x3.
    float    effectRadius              = 0.5f;
    float    radiusMultiplier          = 1.457f;   // XE_GTAO_DEFAULT_RADIUS_MULTIPLIER
    float    effectFalloffRange        = 0.615f;   // XE_GTAO_DEFAULT_FALLOFF_RANGE
    float    sampleDistributionPower   = 2.0f;     // XE_GTAO_DEFAULT_SAMPLE_DISTRIBUTION_POWER
    float    thinOccluderCompensation  = 0.0f;     // XE_GTAO_DEFAULT_THIN_OCCLUDER_COMPENSATION
    float    finalValuePower           = 2.2f;     // XE_GTAO_DEFAULT_FINAL_VALUE_POWER
    float    depthMIPSamplingOffset    = 3.30f;    // XE_GTAO_DEFAULT_DEPTH_MIP_SAMPLING_OFFSET
    float    denoiseBlurBeta           = 1.2f;     // reference default, when denoising
    uint32_t sliceCount                = 3;        // HIGH preset
    uint32_t stepsPerSlice             = 3;

    // Temporal accumulation runs after the spatial denoise (Main -> Denoise -> Temporal). On the
    // denoised, low-sigma input the shader's variance clip is a real disocclusion test, so alpha can
    // stay high without crevice ghosts; against raw 1-spp AO the wide sigma accepts stale history.
    float    temporalHistoryAlpha      = 0.95f;    // 0 disables accumulation
    // Disocclusion threshold on clampDelta = |history - variance-clipped history|. With denoised
    // input the clip window is tight and clampDelta ~0 on converged surfaces; 0.05 rejects history
    // clipped by more than 5% of the AO range, which kills trailing ghosts in one frame without
    // hurting steady-state accumulation.
    float    temporalRejectionDiff     = 0.05f;

private:
    // Graph handles, declared as SRV reads in Setup so the graph transitions the GBuffer targets
    // before this pass. The dispatches bind the raw SRV handles Renderer captured on the main thread
    // (SetDepthSrvHandle and friends), like RTAOPass.
    RG::RGTextureHandle m_normalH, m_depthH, m_velocityH;
    // The AO history's imports (invalid under -rgimportoff, when the pass moves the plane itself).
    // The graph takes their textures at each Execute's start from copies of the bound view's two
    // slots (SyncImports).
    RG::HistoryHandles  m_aoH;
    RHI::Texture        m_aoCurImport;
    RHI::Texture        m_aoPrevImport;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_mainPSO;
    RHI::PipelineState m_temporalPSO;   // AO-only temporal accumulation
    RHI::PipelineState m_denoisePSO;
    RHI::PipelineState m_prefilterPSO;  // 16x16 groupshared 5-mip prefilter

    // Persistent constant buffer: 8 x 256-byte slots, one per dispatch type (kCBOffset* in the .cpp).
    FrameCB<GTAOCBPool> m_cb;

    // The main view's working set; m_w is the bound view's (BindView).
    Work  m_mainWork;
    Work* m_w = &m_mainWork;

    // 64x64 R16_UINT Hilbert-curve lookup: HilbertIndex(x, y) on the 6-level curve
    // (XE_HILBERT_LEVEL = 6, width 64, area 4096). SpatioTemporalNoise applies R2 to it for a
    // decorrelated blue-noise-like 2D value per pixel and frame. Immutable, lives with the pass.
    RHI::Texture       m_hilbertLUT;
    uint64_t           m_hilbertLUTSrv = 0;

    // 1x1 zero-velocity fallback for the velocity slot (t4 space2, root 8) when no GBuffer velocity
    // SRV exists. XeGTAOTemporal.cs reads gVelocity unconditionally, so the slot must never be
    // unbound (GPU-Based Validation: "uninitialized root argument accessed" every frame). Zero
    // velocity = no reprojection, the intended no-history case.
    RHI::Texture       m_zeroVelocityTex;
    uint64_t           m_zeroVelocitySrv = 0;

    // The view's AO history (RenderGraph/ViewState.h): one per view, created empty by BindView;
    // RebuildTextures makes its textures after the transients (the frame's prepare, or Execute's
    // dirty fallback). The view's size latch (Work) rebuilds its transients and history together.
    History* m_hist = nullptr;   // the bound view's (BindView)
    // The bound view: its FrameBreaks carry the camera-stack hard cut (LiveCamera.historyValid ==
    // false, the view's CameraCut). On such a frame the temporal CB gets historyAlpha 0 even with a
    // valid history: resize, first frame and cuts all go through that one gate.
    const ViewState* m_view = nullptr;

    // ---- Transient aliasing (contract: TransientAliasArena.h) ----
    TransientAliasArena* m_aliasArena  = nullptr;
    // The arena the bound view's transients go in: the main one, or a secondary view's scratch heap (null:
    // committed).
    TransientAliasArena* WorkArena() const;

    int m_qualityLevel = 2;   // SetQualityLevel: 2 = the scene's counts
};
