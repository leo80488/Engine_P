#pragma once

// PreviewRenderer - the preview layer: up to Preview::kMaxPreviews live preview views, each
// owning a device HDR slot and a ToneMapPass, drawn by one of three RenderGraphs. The LDR SRV
// goes to ImGui on a side command list chained after the main graph.
//   m_live   material inspector and asset preview, 128/256/384/512, Synthetic draw source
//            (private per-part instance and material tables);
//   m_thumb  asset-browser tiles, pinned to 128^2, same draw source;
//   m_world  camera preview / split viewports / in-game PiP, WorldCull draw source (a second
//            BVH frustum cull over the scene's draw lists), one size shared by every live
//            WorldCull view.
// Each graph holds up to five passes, all second instances of the shipping classes:
// PreviewStudioShadowPass, GBufferPass, SkyboxPass, LightingPass and (when a main instance
// exists to borrow from) TransparentPass.
//
// Rules this class keeps:
//   1. No ShaderLibrary created here resolves a dynamic customPSID: GBufferPass#2 and
//      TransparentPass#2 borrow the main instances' libraries; the owned ones (Lighting,
//      Skybox, ToneMap) hold fixed shaders only.
//   2. No temporal history and no TransientAliasArena: no pass here has a history member,
//      and the graphs' physical textures are committed, not arena-placed.
//   3. Targets are created only in PrepareFrame (device-frame thread, serial, before the
//      graph's Execute). The HDR SLOT is released only in ServiceReleases, which App calls
//      where it calls EditorLayer::ProcessPendingActions - the one place that can prove no
//      in-flight frame still names the descriptor ImGui captured. A resize inside
//      PrepareFrame re-creates through CreateHdrTarget's frame-stamped deferred release,
//      which is legal anywhere.
//   4. Idle cost is zero at both ends: nothing is allocated before the first AcquireView,
//      and a set whose views have all been gone for Preview::kIdleTeardownFrames is given
//      back and rebuilt lazily on the next request.
//
// Threading:
//   AcquireView / ReleaseView / SetRequest / RequestWorldCullLaneSize write the latch. They
//   run on the editor UI half (MaterialPreviewWidget::Draw and the other panels inside
//   EditorLayer::OnUIRender, a Render ThreadCtx, legal only because the render half is parked
//   meanwhile) or on the game half (Renderer::ApplyPendingGamePiP, which applies the intent
//   Renderer::SetGamePiP queues, and Renderer::OnWorldClear). Never call them from the render
//   half of a threaded Game.exe: each carries ENGINE_CHECK_ONCE(PreviewLatchCallerOk()).
//   ApplyFeedback / ExtractRequests: the game half (BuildRenderScene).
//   PrepareFrame: the device-frame thread inside Renderer::Render, before m_graph.Execute.
//   ProcessQueue: the render half, Renderer::Render Phase 4.3.
//   Nothing here reads the World, so none of it needs a ThreadCtx exemption.

#include "Graphics/FrameCB.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/RenderTypes.h"        // DrawPacket
#include "RenderGraph/RenderGraph.h"
#include "Renderer/Preview/PreviewTypes.h"
#include "Renderer/Renderer.h"           // RendererDetail::PerViewCB / LightCB + the by-key material API

#include <array>
#include <atomic>
#include <chrono>          // steady_clock: the session clock behind rebuildsPerHr
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class IGraphicsDevice;
class ToneMapPass;
class GBufferPass;
class LightingPass;
class TransparentPass;
class SkyboxPass;
class PreviewStudioShadowPass;

class PreviewRenderer
{
public:
    struct SceneLighting
    {
        uint64_t irradiance = 0;
        uint64_t radiance   = 0;
        uint32_t radianceMips = 0;
        uint64_t skyboxCube = 0;
        uint64_t skySH      = 0;
        bool     skyUseSH   = false;
        float    sunDir[3]   = { 0.f, -1.f, 0.f };
        float    sunColor[3] = { 0.f,  0.f, 0.f };
        float    iblStrength = 1.f;
        float    ambientColor[3] = { 0.f, 0.f, 0.f };
        float    ambientIntensity = 0.f;
    };

    PreviewRenderer(IGraphicsDevice& gfx, Renderer& owner);
    ~PreviewRenderer();

    PreviewRenderer(const PreviewRenderer&)            = delete;
    PreviewRenderer& operator=(const PreviewRenderer&) = delete;

    // ---- Latch writers: editor UI half or game half ----
    // Claims a view id; returns Preview::kInvalidView when the pool is full. @p worldCull latches
    // the slot as a WorldCull view. Allocates nothing on the device: the next PrepareFrame builds
    // the target, so the panel shows its first pixels on its second frame. Arms needsRender, so a
    // re-acquired view redraws even when its ViewDesc is byte-identical to the stale latch.
    uint32_t AcquireView(bool worldCull = false);
    // Which lane @p viewId belongs to: the source latched at acquire. The one answer every lane
    // question uses (release, live count, size ownership, idle teardown, scheduler, record
    // route); SetRequest refuses a ViewDesc whose source disagrees. An out-of-range or unacquired
    // id reads Synthetic.
    // Every live WorldCull view shares the one m_world GraphSet and so one size: the first
    // request to latch fixes it, a request at another size is refused (WorldCullSizeAllowed), and
    // only RequestWorldCullLaneSize moves it. ProcessQueue's round-robin keeps the budget of one
    // lane-1 record per frame.
    Preview::Source SlotSource(uint32_t viewId) const
    {
        return (viewId < Preview::kMaxPreviews) ? m_slotSource[viewId]
                                                : Preview::Source::Synthetic;
    }
    bool IsWorldCullSlot(uint32_t viewId) const
    { return SlotSource(viewId) == Preview::Source::WorldCull; }

    // Moves the whole WorldCull lane to @p w x @p h atomically - the only way a fixed size
    // changes. @p views / @p count are the caller's WorldCull view ids; the move is allowed only
    // when they are all the live WorldCull views, because the one-size rule exists to prevent
    // another target left at the old size, whose every record would then bail silently on
    // `set.w != t->w`. Every listed latch and extract snapshot moves in the same call, so exactly
    // one graph recompile follows. A single-view caller (the Camera Preview panel, the in-game
    // PiP) takes the same path.
    // Returns true when the lane is at @p w x @p h afterwards (including the no-op cases: nothing
    // fixed yet, or already that size). Returns false, and logs one line naming the view that
    // holds the lane, when another panel's view is live at another size.
    bool RequestWorldCullLaneSize(uint32_t w, uint32_t h,
                                  const uint32_t* views, uint32_t count);

    // The size the WorldCull lane is fixed at, 0x0 when nothing has fixed it yet. For the panel's
    // "why is this cell empty" text.
    void GetWorldCullSize(uint32_t& outW, uint32_t& outH) const
    { outW = m_worldSizeW; outH = m_worldSizeH; }
    // Whether @p viewId's last SetRequest was refused for its size. A refused view is suspended:
    // it keeps its last picture (the SRV stays valid, so the panel can show it greyed at its own
    // size), mints no generation and takes no lane turn until a request at the lane's size
    // arrives. It still holds its view id and, if it ever rendered, an HDR slot.
    bool IsViewSizeRefused(uint32_t viewId) const;

    // The view the round-robin serves first (the hovered / focused split cell); kInvalidView
    // clears it. Writes no main-viewport input state, it only re-orders the scheduler.
    // It expires: valid for the preview frame it was set on plus one, then ProcessQueue drops it,
    // because a panel can stop drawing without reaching a Close button and the next tenant of
    // that id would inherit a double share. Re-assert it every frame (the panel does).
    // ReleaseView and NoteWorldClear clear it at once: a released id must not be preferred for
    // even one more frame.
    void SetPriorityView(uint32_t viewId)
    {
        m_priorityView.store(viewId, std::memory_order_relaxed);
        m_priorityFrame.store(m_previewFramePub.load(std::memory_order_relaxed),
                              std::memory_order_relaxed);
    }

    // A world boundary: re-arms the one-shot refusal diagnostics (size, lane move, pool
    // exhaustion, zero extent) so the first refusal after a reload is logged, and clears the
    // scheduler priority (view ids are recycled). The views themselves belong to the panels and
    // to Renderer::OnWorldClear, which releases the PiP's.
    // Do not clear the per-view size-refused flags or the lane size here: the flags are the
    // suspended state, not a diagnostic, and clearing them alone resumes every suspended view for
    // one stray frame at a size its panel is not showing. The next SetRequest, which every live
    // panel makes every frame, re-derives them; the lane size goes with the last ReleaseView.
    void NoteWorldClear()
    {
        // Stamp the render counter, so the shutdown line can tell "rendered after the world went
        // away" from "froze on its last pre-reload picture"; ViewsRenderedTotal alone cannot.
        ++m_worldClearCount;
        m_rendersAtLastClear = m_viewsRenderedTotal.load(std::memory_order_relaxed);
        m_worldSizeRefusedLogged = false;
        m_moveRefusedCount       = 0;
        m_slotsExhaustedLogged   = false;
        for (bool& z : m_zeroSizeLogged) z = false;
        m_priorityView.store(Preview::kInvalidView, std::memory_order_relaxed);
    }

    // Queues a view for teardown. ServiceReleases frees the HDR slot, never this call: it is
    // reachable from an ImGui handler, and freeing a descriptor there is the use-after-free the
    // ProcessPendingActions slot exists to prevent. Bumps the generation at once, so any result
    // still in flight is dropped.
    void ReleaseView(uint32_t viewId);

    // Latches a request; called every frame the panel is visible. A new generation is minted only
    // on a real edge: here when the ViewDesc changed or needsRender is armed, in ExtractRequests
    // when the material hash changed (and on a WorldCull view's tick), so an untouched panel costs
    // one comparison and no GPU work.
    // @p mat is copied by value into the latch on every call (see Preview::Request). The runtime
    // key, parameter hash, custom shader ids and texture readiness are filled in later on the game
    // half by ExtractRequests: they need the Renderer's material maps or the shader registry, which
    // this context must not touch.
    void SetRequest(uint32_t viewId, const Preview::ViewDesc& v,
                    const MaterialComponent& mat);

    // ---- WorldCull camera (game half) ----
    // Game half: the camera resolver writes @p viewId's camera here. @p valid false makes the
    // record bail instead of drawing a default RenderCamera at the world origin.
    // Renderer::ExtractPreviewCameras is the only caller.
    void SetViewCamera(uint32_t viewId, const RenderCamera& cam, bool valid);
    // True when @p viewId is live, latched as a WorldCull view and not suspended - what
    // Renderer::ExtractPreviewCameras asks before resolving a camera and contributing a secondary
    // frustum. A suspended view keeps req.valid and a valid camera, so the suspension test
    // belongs here: otherwise it would contribute a frustum and an auto-exposure readback every
    // frame for a view that records nothing.
    bool IsWorldCullView(uint32_t viewId) const;
    // The requested picture size of @p viewId, for the aspect ratio of the frustum the game half
    // builds for it. False when the view is not live or has no valid latch.
    bool GetViewRequestSize(uint32_t viewId, uint32_t& outW, uint32_t& outH) const;

    // ---- Scene lights for WorldCull views ----
    // Game half only: publishes the punctual lights culled for @p viewId this frame. @p lights
    // points at @p n packed records (clamped to Preview::kMaxPreviewLights) and is copied, so the
    // caller may reuse its buffer at once. @p scene / @p culled / @p dropped are the log line's
    // bookkeeping: punctual lights in the scene, rejected by the frustum, dropped by the cap.
    // @p shadowed counts the packed records carrying a real shadow slice (`shadowed=` on the log
    // line) - a tally, so the packs' `shadowed=0` assertion can fail. @p nonFinite counts source
    // lights refused for a NaN / infinite position, radius or score; non-zero is a content bug
    // upstream, not a preview bug.
    // Stamps m_previewFrame + 1, not m_previewFrame - see m_viewLightStamp.
    void SetViewSceneLights(uint32_t viewId, const Preview::GPULight* lights,
                            uint32_t n, uint32_t scene, uint32_t culled,
                            uint32_t dropped, uint32_t shadowed,
                            uint32_t nonFinite);
    // How many live, non-suspended WorldCull views there are. The game half's zero-idle-cost
    // guard: with none, ExtractPreviewSceneLights returns before touching m_frameLights, so a
    // game with no PiP and an editor with only a material inspector pay one compare.
    uint32_t LiveWorldCullViewCount() const;
    // How many clustered lights the render half last bound for @p viewId (kInvalidView or an
    // out-of-range id: whichever world view recorded last) - what Lua's
    // Preview.WorldClusterLights() reports and packs assert per window. Written where the bind
    // happens, so it cannot say "on" for a frame that refused the set.
    uint32_t LastWorldClusterLights(uint32_t viewId) const;
    // The process-wide off-arm for WorldCull scene lights, and their only circuit breaker. False
    // gives every WorldCull view the scene's sun and IBL only, no point or spot lights. A runtime
    // toggle, not a boot flag, because the cost pack's p50Delta is an in-process A/B and a boot
    // flag cannot alternate windows.
    // Call it only inside the parked extract window: the member is a plain bool the render half
    // reads, so any other caller races WriteWorldClusterLights. Renderer::SetPreviewWorldSceneLights
    // only records an intent; Renderer::ApplyPendingGamePiP (early in BuildRenderScene, after the
    // park) applies it, and AcquirePreviewView / QueuePreviewThumbnail push it into a layer they
    // have just built, before any render half reads it. Nothing else may call it.
    void SetWorldSceneLightsEnabled(bool on) { m_worldSceneLightsEnabled = on; }
    bool WorldSceneLightsEnabled() const { return m_worldSceneLightsEnabled; }

    // What asset view @p viewId last resolved to, so the asset panel can report "12 parts,
    // 10 materials, 0 dropped" and a partial picture (dropped > 0) says so on screen as well as
    // in the log. @p outSkipped, when given, receives the node kinds the prefab walk declined -
    // the other reason an asset picture is partial. All zeros for a view that is not an asset view.
    void GetAssetStats(uint32_t viewId, uint32_t& outParts,
                       uint32_t& outMaterials, uint32_t& outDropped,
                       Resource::PrefabSkipCounts* outSkipped = nullptr) const;
    // How many packets @p viewId's last WorldCull record drew, and how many the frame's draw lists
    // held before the cull; both 0 until one has run. Packs gate on "N > 0 and N <= M": culling
    // everything and culling nothing are both bugs the picture cannot tell apart.
    // Per view; Preview::kInvalidView (the default) means whichever view recorded last, which a
    // single-view caller such as preview_pip_cull reads as its own.
    uint32_t LastWorldCullDraws(uint32_t viewId = Preview::kInvalidView) const
    { return m_worldCullDraws[WorldCullSlot(viewId)].load(std::memory_order_relaxed); }
    uint32_t LastWorldCullCandidates(uint32_t viewId = Preview::kInvalidView) const
    { return m_worldCullCandidates[WorldCullSlot(viewId)].load(std::memory_order_relaxed); }
    // How many kept packets came from the secondary-view lists, i.e. geometry the main camera
    // cannot see at all. Zero means the secondary-view cull regressed: the picture still looks
    // plausible (the intersection of both frusta plus sky), so this is the only number that tells
    // a working second view from a cropped copy of the first.
    uint32_t LastWorldCullSecondary(uint32_t viewId = Preview::kInvalidView) const
    { return m_worldCullSecondary[WorldCullSlot(viewId)].load(std::memory_order_relaxed); }
    // Renderer::CullWorldForPreview's callback - the only place that sees all four draw lists:
    // @p candidates is what the lists offered the cull, @p secondary the survivors from the
    // secondary lists. Read back by the "[PREVIEW] world cull" line, so the pack's
    // "N > 0 and N <= M" has an M.
    // Attributed to m_worldCullRecordingView, which BuildWorldDrawList publishes immediately before
    // it calls CullWorldForPreview; both run on the one recording thread, with no other view
    // interleaved.
    void NoteWorldCullCounts(uint32_t candidates, uint32_t secondary,
                             uint32_t leaflessPrim, uint32_t leaflessOther,
                             uint32_t leaflessPrimKept, uint32_t leaflessOtherKept)
    {
        const uint32_t v = WorldCullSlot(m_worldCullRecordingView);
        m_worldCullCandidates[v].store(candidates, std::memory_order_relaxed);
        m_worldCullSecondary[v].store(secondary, std::memory_order_relaxed);
        // How much of the kept set the conservative "no BVH leaf -> keep" branch carries, split into
        // what a plane test could convert (prim) and what it never will (other). Definitions in
        // Renderer::CullWorldForPreview.
        m_worldCullLeaflessPrim[v].store(leaflessPrim, std::memory_order_relaxed);
        m_worldCullLeaflessOther[v].store(leaflessOther, std::memory_order_relaxed);
        m_worldCullLeaflessPrimKept[v].store(leaflessPrimKept, std::memory_order_relaxed);
        m_worldCullLeaflessOtherKept[v].store(leaflessOtherKept, std::memory_order_relaxed);
    }

    // ---- Game half ----
    // Consumes the render half's result. Runs at the top of BuildRenderScene, before anything
    // that could re-arm the lane (apply before the enqueue sweep, as the probe-bake lane does).
    void ApplyFeedback();

    // Hands the latch to the render half. Runs at the very end of BuildRenderScene, like
    // ExtractProbeBakeRequest.
    void ExtractRequests();

    // Destroys the targets ReleaseView queued and runs the idle sweep that gives whole GraphSets
    // back. Called from the EditorLayer::ProcessPendingActions slot - see rule 3 in the file header.
    void ServiceReleases();

    // Shutdown only: services just the view destroys ReleaseView queued - not the thumbnail
    // lane's reference drop and not the idle sweep. ServiceReleases would be wrong there twice:
    // the idle grace is counted in calls, so one extra call can tip a set idle for grace-1 into a
    // teardown that lands in the session summary a pack pins exactly; and the destructor drops
    // (never releases) the thumbnail lane's references a moment later.
    void DrainPendingViewDestroys();

    // Harness only: shortens the idle-teardown grace (no UI, no preference; the shipped value and
    // its reasoning are Preview::kIdleTeardownFrames). Clamped up to
    // Preview::kMinIdleTeardownFrames, below which the sweep could hand back descriptors a command
    // list may still name. 0 restores the default.
    void SetIdleTeardownFrames(uint32_t frames);

    // Harness only: points the studio key light somewhere else. The shipped Preview::kKeyLight
    // sits ~30 deg off the panel's default camera axis, so every self-shadowed point is at
    // N.L <= 0 and the studio shadow cannot be seen. Moving the constant would re-bless the
    // preview_fixtures bands, two parity floors and a machine-pinned golden, so a pack that wants
    // to see the shadow overrides it for its own run.
    // @p dir is the direction the light travels, as StudioLight::dir is; the consumers normalise
    // it. A zero-length vector is refused (the caller reports it) and the shipped constant stays.
    bool SetKeyLightDirOverride(const float dir[3]);

    // Harness only (-preview_shadow_alpha): turns the studio shadow pass's pixel stage on or off
    // for the following frames, so one run can capture a fixture with and without the alpha clip
    // (see PreviewStudioShadowPass::SetAlphaTestEnabled). Pushed to every GraphSet's shadow pass
    // on the render half beside SetViewCB, so a change applies whole on the next preview render.
    // Logs only when the value changes.
    void SetStudioShadowAlphaTest(bool on);

    // ---- Thumbnail lane ----
    // A second, independent one-shot lane (see Preview::ThumbRequest for why it bypasses the live
    // latch). All of it runs in the game half's serial window (EditorLayer::ProcessPendingActions),
    // before renderer.BeginFrame and so before ExtractRequests, so an enqueue lands in the same
    // frame's extract.
    // Queues one thumbnail. False when the queue holds Preview::kMaxThumbnailQueue, the tile
    // texture is invalid, or @p r.assetKey is already queued or in flight - a duplicate would
    // render twice and let the second result clear a dirty flag the first request predates.
    bool QueueThumbnail(const Preview::ThumbRequest& r);
    // Drops every queued request for @p assetKey and voids the in-flight one. Call it before the
    // tile texture is destroyed (eviction, rescan orphan): a queued request holds the RHI::Texture
    // handle by value, and a recycled pool slot names somebody else's texture.
    // Callers rely on the in-flight void: a request at a destroyed tile can never publish (its
    // copy is skipped), so the lane would resend it up to its cap. No result is published for the
    // voided request; the editor's dirty flag stays set and the next sweep asks again.
    void CancelThumbnail(uint64_t assetKey);
    // True while @p assetKey is queued or in flight.
    bool HasThumbnailInFlight(uint64_t assetKey) const;
    // True once the lane switched itself off for the session - today only for an HDR output mode,
    // whose FP16 tone-map output cannot be copied into an R8G8B8A8 tile. QueueThumbnail then
    // refuses everything, so the editor must stop creating tiles and parsing materials for it: an
    // enqueue loop that treats a refusal as "try the next one" walks the whole folder every frame.
    bool ThumbnailsDisabled() const { return m_thumbDisabled; }
    std::size_t ThumbnailQueueDepth() const { return m_thumbQueue.size(); }
    // Game half: moves the results the render half published (via ApplyFeedback) into @p out.
    void TakeThumbnailResults(std::vector<Preview::ThumbResult>& out);
    // Pops the oldest completed readback, staged by PrepareFrame from the kFramesInFlight-deep
    // ring; false when none is pending. The pixels are RGBA8 with @p outRowPitch bytes per row
    // (256-aligned, so not width*4 in general) and stay valid until the next call; a second call
    // returns the next entry, never this one again.
    bool TakeThumbnailPixels(uint64_t& outAssetKey, uint32_t& outGeneration,
                             const uint8_t*& outRgba, uint32_t& outRowPitch,
                             uint32_t& outWidth, uint32_t& outHeight);
    // Monotonic count of thumbnails recorded. Kept apart from ViewsRenderedTotal: preview_edit
    // latches that counter at an edit and asserts the next increment is the live re-render, which
    // an unrelated asset-browser tile could satisfy or corrupt.
    uint64_t ThumbnailsRenderedTotal() const
    { return m_thumbsRenderedTotal.load(std::memory_order_relaxed); }

    // Builds, and re-sizes, the graphs the live views need: m_live at the size of the Synthetic
    // view owed a picture, m_thumb while there is thumbnail work, m_world while a WorldCull view
    // is live. Never from Renderer::Render: RenderGraph::Compile recreates every physical texture
    // and runs each pass's Init() / OnCompile() serially, all of which touch the descriptor-heap
    // allocator and must not run on or beside a graph worker. App calls this in ServiceReleases'
    // serial window: game half, after ProcessPendingActions, before the device frame opens.
    // @p mainGBuffer is the pass whose ShaderLibrary is borrowed (rule 1); nothing is built
    // without it. Returns whether m_live is usable (IsCompiled).
    // @p mainTransparent is a second, separate borrow because a custom PS's SV_Target count comes
    // from its permutation bits: a translucent material's shader is a 1-RTV forward binary, and
    // binding it into the 6-RTV GBuffer PSO fails. Null is legal and leaves TransparentPass#2 out
    // of the graph (forward-domain materials then preview through the opaque route).
    bool ServiceCompile(GBufferPass* mainGBuffer, TransparentPass* mainTransparent);
    bool IsCompiled() const { return m_live.compiled; }

    // Hot reload. Called by Renderer::ReloadShaders after the main graph's, because GBufferPass#2
    // borrows the main ShaderLibrary and the owner must clear and re-warm it first. The borrower
    // clears only its own PSOCache (GBufferPass gates the library ClearCaches on
    // OwnsShaderLibrary), so the reload epoch is bumped once. Also reloads every view's
    // ToneMapPass, which lives outside the graphs.
    void ReloadShaders(IGraphicsDevice& gfx);

    // ---- Device-frame thread, serial, before the graph Execute ----
    // Creates / resizes the target for each request in flight and pushes the main-thread-only
    // ToneMapPass setters (EnsureTexture / SetOutputMode), so the size-only safety net inside
    // ToneMapPass::Execute never fires.
    void PrepareFrame();

    // ---- Render half, Renderer::Render Phase 4.3 ----
    // Records this frame's scheduled pictures, one command list each, chained after
    // @p colorLastCL, and returns the last one; an invalid CommandList when nothing was recorded.
    // Deliberately not spliced into the caller's colorLastCL chain: that would put Hi-Z, SSR, the
    // DDGI debug list and preComputeCL behind the preview, a real GPU stall. Instead
    // FramePipeline::RenderHalfAsync adds AddCommandListDependency(primaryCL, previewCL) next to
    // the one for lastPassCL, so ImGui samples finished pixels; the chain still runs after the
    // graph through its dependency on @p colorLastCL, which the borrowed sky IBL needs.
    RHI::CommandList ProcessQueue(RHI::CommandList colorLastCL);

    // ---- Observation ----
    // LDR SRV of @p viewId's last recorded render, 0 until then. Re-resolve it on every call;
    // never cache it.
    // One frame of latency: `hasOutput` is published by the render half at record time, not by
    // ApplyFeedback, and Render() records before OnUIRender in the same frame with previewCL a
    // predecessor of primaryCL, so that frame's ImGui pass can already sample the picture.
    // Arming it from ApplyFeedback would delay first pixels to frame N+2.
    uint64_t GetSrv(uint32_t viewId) const;
    // The size @p viewId's target physically is - between a resize request and the recompile
    // that services it, not the requested size. False when the view has never rendered.
    bool GetOutputSize(uint32_t viewId, uint32_t& outW, uint32_t& outH) const;
    const RHI::Texture* GetOutputTexture(uint32_t viewId) const;
    // The view whose pixels are newest, for -preview_screenshot. Atomic: written by the render
    // half, read by HarnessCaptureSystem from TickPhase::PostRender, which runs before the park.
    uint32_t LastRenderedView() const
    { return m_lastRenderedView.load(std::memory_order_relaxed); }
    // The stencil ref the last preview draw packet carried - ShadingStencil's kPBR / kNPR /
    // kUnlit, i.e. which compile-time Lighting.ps variant the GBuffer stencil selects for those
    // pixels; 0 before any render. The fixture driver reads it at the end of each fixture's
    // window and compares it with ShadingStencil::RefFor of that fixture's shading model (the
    // "first render" line reports the stencil only once per size).
    // That proves only that the shading model reached the draw packet with the expected ref -
    // the install path material -> asset buffer -> widget -> ViewDesc -> packet, where a dropped
    // fixture would silently re-render the previous one under the new name. It does not prove
    // the permutation: both sides are the same function of the shading model, so a wrong
    // OMSetStencilRef, a broken stencil test or a Lighting.ps that ignores the ref would pass it.
    // The permutation evidence is pixel-level: preview_fixtures' per-fixture luma bands, and the
    // two A/B parity packs that render the PBR and NPR variants through both pipelines.
    // Same atomic contract as LastRenderedView.
    uint32_t LastRenderedStencil() const
    { return m_lastRenderedStencil.load(std::memory_order_relaxed); }

    // 1 when the last recorded picture's subject went to TransparentPass#2
    // (DrawFilter::Transparent, the forward route), 0 when GBufferPass#2 drew it or nothing was
    // drawn. Read off the same packets[0] as LastRenderedStencil, so a fixture pack can bind the
    // route to the fixture it captured: the "draw route" line logs only on a change and names no
    // fixture. preview_fwd_parity compares each fixture with its forward twin, and a twin that
    // quietly drew deferred would match its partner by construction.
    uint32_t LastRenderedForward() const
    { return m_lastRenderedForward.load(std::memory_order_relaxed); }

    // Per-frame counters, written by ProcessQueue on the render half (reset at its top) and read
    // by the game half through Renderer's mailbox - see Renderer::FrameStats.
    uint32_t ViewsRenderedThisFrame() const { return m_viewsRendered; }
    uint32_t ViewDrawsThisFrame()     const { return m_viewDraws; }

    // Monotonic total of preview renders, never reset. The -preview_edit_at harness action
    // latches it at the edit and compares it on the following frames, which turns "drag a
    // slider -> the picture updates within one frame" into an assertion; the per-frame counters
    // are one frame wide and frames.jsonl's fieldEquals compares every frame.
    // Atomic because ProcessQueue writes it on the render half while the editor's
    // ProcessPendingActions reads it on the game half.
    uint64_t ViewsRenderedTotal() const
    { return m_viewsRenderedTotal.load(std::memory_order_relaxed); }

    // The same count for one view slot, monotonic across the slot's tenants: compare two
    // readings, never an absolute value. What a named-camera capture waits on ("this view has
    // recorded a picture since I pointed it"). 0 for an out-of-range id.
    uint64_t ViewRenderCount(uint32_t viewId) const
    {
        return (viewId < Preview::kMaxPreviews)
            ? m_viewRenderCount[viewId].load(std::memory_order_relaxed) : 0u;
    }

    // The slot's lifetime generation: how many times it has been acquired. (viewId, lease) names
    // one tenancy - a panel closed and reopened, or the id handed to another panel, is a new
    // lease - so a caller holding an old pair is told stale_view instead of being handed the new
    // tenant's picture. Kept apart from the content generation SetRequest mints on every change,
    // which would make a handle go stale on every parameter tweak. 0 = never acquired.
    uint32_t ViewLease(uint32_t viewId) const
    {
        return (viewId < Preview::kMaxPreviews)
            ? m_targets[viewId].lease.load(std::memory_order_relaxed) : 0u;
    }

    // The preview frame counter. ProcessQueue increments it once per call, so it counts frames on
    // which the preview layer was serviced, not engine frames: a run with no preview panel never
    // advances it, and it advances at most once per engine frame. ViewDesc::tickIntervalFrames
    // is measured on this clock, so a panel's "every N frames" means N preview frames.
    // Read through m_previewFramePub, so the game half (Lua's Preview.PreviewFrame(), a panel's
    // stats text) may call it; m_previewFrame itself belongs to the render half and must not be
    // read elsewhere.
    uint64_t PreviewFrame() const
    { return m_previewFramePub.load(std::memory_order_relaxed); }

    // ---- Scene inputs pushed in from the game half ----
    // Resolved once per frame by Renderer::ExtractPreviewRequests and read by the record half.
    // Plain values, not handles the record half could re-resolve: TextureSystem::GetTexture is
    // main-thread only.
    // Game half only: pushes this frame's studio environment. A zero cube handle means not ready
    // yet; the record half then binds Renderer's shared 1x1 black cube, a defined black rather
    // than an unbound t6/t7 (the substitution lives in Renderer, not in the pass). A zero
    // @p brdfLut keeps the indirect path off entirely: t8 has no fallback anywhere.
    void SetEnvironment(uint64_t irradianceCube, uint64_t radianceCube,
                        uint32_t radianceMips, uint64_t brdfLut);

    // True once both studio cubes have resolved at least once. Read by the thumbnail lane, which,
    // unlike the live panel, bakes to disk and must not write a tile lit by the 1x1 black
    // fallback.
    bool StudioIblEverReady() const { return m_iblEverReady; }

    // The live scene's lighting, for Feature::kSceneLighting, pushed every frame by the same
    // game-half call as the studio triple, from what SyncSkyboxIBL published this frame. All
    // zeroes = the scene has no sky, so the toggle falls back to the studio rig rather than to
    // black (a preview that goes dark on a None / FlatColor scene would read as a bug).
    // skyboxCube is what SkyboxPass#2 draws for Background::Skybox with scene lighting on; 0 falls
    // back to the studio radiance cube. sunDir is the travel direction (LightCB.lightDir's
    // convention), already flipped by the caller.
    void SetSceneLighting(const SceneLighting& s) { m_sceneLighting = s; }
    // The studio radiance cube, for the Background::Skybox backdrop.
    uint64_t StudioRadianceCube() const { return m_iblRadiance; }

private:
    struct Target
    {
        bool     live           = false;   // view id handed out
        bool     pendingCreate  = false;   // PrepareFrame owes it a target
        bool     pendingDestroy = false;   // ServiceReleases owes it a teardown
        uint32_t hdrSlot        = 0;       // 1..kMaxHdrTargets-1; 0 = none
        uint32_t w = 0, h = 0;             // size the target currently IS
        uint32_t reqW = 0, reqH = 0;       // size the request wants
        std::unique_ptr<ToneMapPass> toneMap;
        bool     toneMapInited  = false;
        // Monotonic content generation. Bumped when a new picture is wanted (SetRequest, or
        // ExtractOneRequest for a material or tick edge) and by ReleaseView (everything in flight is
        // void).
        uint32_t generation           = 0;
        uint32_t lastRenderedGeneration = 0;
        // The other half of the dirty-edge regulator: a generation is minted on a ViewDesc change or
        // on this flag. There is deliberately no "the lane is idle, refresh anyway" term - that is the
        // steady state and would re-render an untouched panel every other frame. Set on acquire /
        // re-acquire / becoming visible again; cleared the moment a generation is minted for it.
        bool     needsRender          = true;
        // Published by the RENDER half at RECORD time (see GetSrv). Atomic
        // because the editor's ImGui pass reads it later in the same frame.
        std::atomic<bool> hasOutput{ false };
        // The slot's lifetime generation: +1 on every AcquireView, never reset (see ViewLease). Not
        // `generation`, the content revision above.
        std::atomic<uint32_t> lease{ 0 };
        // ---- Idle clock ----
        // Consecutive ServiceReleases frames this slot has been neither live nor pending anything. At
        // m_idleTeardownFrames the ToneMapPass and its output texture go back - what DestroyTarget
        // deliberately keeps, because on the frame a panel closes ImGui's queued draw data still
        // names that texture.
        uint32_t idleFrames = 0;

        // ---- Material ownership (game half only) ----
        // The runtime-material key this view holds one reference to, taken and released in
        // ExtractRequests / ServiceReleases; never touched from the record half or from ImGui.
        // Zero also means no reference held: OnWorldClear wipes the table regardless of refcount on
        // every world load and every editor Play -> Stop, so the self-heal zeroes it when a by-key
        // lookup misses.
        uint64_t heldMatKey = 0;
        // Renderer::MaterialRuntimeEpoch() when heldMatKey was taken. Closes the hole the miss-based
        // heal leaves: when the new world uses the same key, BuildScene_SyncMaterials re-creates it
        // before ExtractRequests runs, Has(key) is true, and the reference is gone anyway. Releasing it
        // then would drop a refCount held by live scene entities and return their TextureSystem
        // handles (SyncMaterialTextures early-outs on an unchanged key), turning the object
        // permanently white. An epoch mismatch means: forget the key without releasing it, then
        // re-acquire.
        uint32_t heldMatEpoch = 0;
        // Edge detector for "the textures finished streaming". A flip re-arms
        // needsRender, which is what turns the white-fallback first draw into
        // the real textured one without polling.
        bool     lastTexturesReady = false;

        // ---- The same ownership, per asset part ----
        // An asset view holds one runtime-material reference per distinct material its parts name,
        // under every rule above: acquire before release on a change, forget without releasing on an
        // epoch bump, re-acquire on a missing key. A vector rather than folded into heldMatKey because
        // the release must be exact: 15 parts sharing 10 materials take and hand back 10 references.
        // heldMatKey is 0 while this is non-empty, so the two schemes never both own a key.
        std::vector<uint64_t> heldPartKeys;
        // The asset path those keys were taken for. A change is what triggers
        // the release-and-re-acquire, exactly as heldMatKey != wantKey does for
        // the single-material path.
        std::string           heldAssetPath;

        // ---- Asset extract dirty edge ----
        // Stamp of everything the full asset resolve depends on: the path, the kind, the
        // runtime-material epoch and the composition-memo epoch. While it is unchanged the extract
        // only refreshes the per-part texture readiness and returns, so an open, untouched asset
        // panel does not re-resolve its mesh slots, reflections and material hashes every frame.
        uint64_t assetExtractStamp = 0;

        // ---- The latch, one per view ----
        // Each view has its own latch, so a request for view B cannot disturb view A's in-flight
        // hand-over; ProcessQueue's scheduler and its per-frame cap decide who renders. The
        // generation is monotonic for the life of the pool slot (never reset on acquire), inFlight is
        // set by the extract and cleared by the record (on success and on every early-out), and a
        // result whose generation does not match the current one is dropped.
        Preview::Request req{};
        Preview::Result  result{};

        // ---- Round-robin regulator ----
        // Preview frame of this view's last record (or bail), stamped by ProcessQueue, whose scheduler
        // serves each lane's oldest candidate first. A WorldCull view is dirty every frame, so its
        // tick gate against ViewDesc::tickIntervalFrames (in ExtractOneRequest) is the only thing
        // bounding it; a Synthetic view is dirty-edge gated, not ticked.
        uint64_t lastTickFrame = 0;
        // The view's camera entity is Renderer::m_previewCamEntity, not a field here:
        // Renderer::ExtractPreviewCameras needs a World to resolve it, and this class never sees one.
    };

    // One deferred-shading graph, instantiated three times (m_live, m_thumb, m_world). Everything
    // the instances differ in is here; the packer, the constant buffers, the draw list and the
    // custom-shader net are shared, because a thumbnail must be the picture the live panel would
    // show, only smaller.
    // Separate graphs rather than a second target on one: a RenderGraph has one size
    // (SetFixedDimensions) and a change recreates every physical texture, so alternating the
    // live panel (128-512) and 128 thumbnails on one graph would recompile seven textures each
    // time. The thumbnail set costs 128^2 x 56 B/px = 0.9 MB and is built lazily.
    struct GraphSet
    {
        RG::RenderGraph  graph;
        bool             compiled = false;
        GBufferPass*     gbuffer  = nullptr;   // owned by `graph`
        LightingPass*    lighting = nullptr;   // owned by `graph`
        // ---- Optional passes, owned by `graph` ----
        // Present when the owner whose library they borrow existed at Compile time (transparent) or
        // always (shadow / skybox). Whether they run is decided per frame by the graph's view
        // (ApplyPreviewView), not by their existence: RenderGraph::Compile runs before the activity
        // gate, so a pass that exists has paid its VRAM whatever the view shows.
        PreviewStudioShadowPass* shadow      = nullptr;
        SkyboxPass*              skybox      = nullptr;
        TransparentPass*         transparent = nullptr;
        uint32_t passCount = 0;   // the passes added (the roster line's passes=)
        RG::RGTextureHandle hAlbedo{}, hNormal{}, hSurface{}, hDepth{},
                            hVelocity{}, hEmissive{}, hReflectionSource{};
        // The HDR scene colour, imported: the slot's target, bound with the view's clear value before
        // every Execute. Invalid under -rgimportoff.
        RG::RGTextureHandle hHdr{};
        // The size the graph's physical textures currently ARE. A change forces
        // MarkDirty + Compile, which is why the widget quantises panel sizes to
        // four values and debounces them, and why the thumbnail set is pinned.
        uint32_t         w = 0, h = 0;
        // Which draw source this set is built for. Synthetic sets bind the preview's private instance
        // and material tables; the WorldCull set binds the scene's, so its GBufferPass gets the
        // scene's pristine remap index at PerViewCB byte 228. A field rather than a pointer comparison
        // because the record half branches on it.
        Preview::Source  source = Preview::Source::Synthetic;
        // The constant-buffer lane this set writes through. A FrameCB holds one slot per frame (and
        // FrameCB.h is not widened), so two pictures in one frame use two independent rings: m_live
        // and m_thumb share lane 0, m_world has lane 1. The per-frame cap of 2 is the lane count, and
        // RecordRequest's ENGINE_CHECK counts renders per lane. Borrowed pointers into
        // PreviewRenderer's own members; never null after the constructor.
        FrameCB<RendererDetail::PerViewCB>* perViewCB = nullptr;
        FrameCB<RendererDetail::LightCB>*   lightCB   = nullptr;
        // Which lane: 0 = m_live and m_thumb (sharing m_perViewCB, m_lightCB, m_shadowViewCB,
        // m_customCbv, the space3 block and the cluster buffers), 1 = m_world. The render counter is
        // indexed by this on PreviewRenderer, not kept per set: a per-set counter would read 0 for
        // m_thumb on a frame m_live had already written lane 0.
        uint32_t         lane = 0;
        // ---- Idle clock ----
        // Consecutive ServiceReleases frames during which nothing this set serves was live; reset the
        // moment one is. See ServiceIdleTeardown and Preview::kIdleTeardownFrames for why teardown is
        // timed rather than immediate.
        uint32_t         idleFrames = 0;
        // How many times this set was built this session. `compiled` goes false only in
        // DestroyGraphSet, so every build past the first is a rebuild after an idle teardown - the
        // 43-231 ms hitch the session-totals line reports a per-hour rate for.
        uint32_t         builds = 0;
        // Name suffix for both passes; must be unique per instance: GetGPUTimestampResult is a
        // first-match linear name scan, so two instances sharing a name would hand the profiler the
        // union of both.
        const char*      suffix = "Preview";
    };

    // m_laneRenders is indexed by GraphSet::lane and reset at the top of ProcessQueue. Two lanes,
    // three sets: m_live and m_thumb share lane 0 because they share every constant-buffer ring.
    // With kMaxPreviewsPerFrame == 2 an inspector view and a thumbnail in the same frame is
    // reachable, so this per-lane count is the guard that has to hold.
    static constexpr uint32_t kLaneCount = 2;
    static constexpr uint32_t kLaneLive  = 0;   // m_live + m_thumb
    static constexpr uint32_t kLaneWorld = 1;   // m_world
    static constexpr uint32_t kStudioGridView = 0;

    static constexpr uint32_t kMaxPreviewLights = Preview::kMaxPreviewLights;

    struct PendingPixels
    {
        uint64_t assetKey   = 0;
        uint32_t generation = 0;
        std::vector<uint8_t> bytes;
    };

    struct WaterDowngradeKey { uint64_t matRuntimeKey; uint32_t blend; };

    enum TeardownKind { kTearLive = 0, kTearThumb = 1, kTearWorld = 2, kTearKinds = 3 };

    // The size rule, and the one place a request is judged against it. All live WorldCull views
    // share m_world and so its size: the first accepted request fixes it, later ones must match;
    // moving a fixed size is RequestWorldCullLaneSize's job, for a caller owning the whole lane.
    // Returns whether @p w x @p h is acceptable, logging a refusal once per tenancy here (this and
    // the lane move are the only places that see every caller).
    // SetRequest is the only caller: a panel that pre-checked before acquiring would send every
    // resize of a sole panel down the refusal path. Not const: the log-once latch lives here.
    bool WorldCullSizeAllowed(uint32_t w, uint32_t h);
    // True when a lane move to @p w x @p h has not yet been reported as refused this tenancy, and
    // records it, so the next caller asking for that size stays silent. See m_moveRefusedSizes.
    bool NoteMoveRefusal(uint32_t w, uint32_t h);

    Target*       Get(uint32_t viewId);
    const Target* Get(uint32_t viewId) const;
    void          EnsureNeutralInputs();
    // One pooled preview view. Closing a panel keeps the ToneMapPass and its output texture until
    // the slot has been idle for Preview::kIdleTeardownFrames: an earlier ImGui::Image captured a
    // descriptor into that texture's SRV, and ImGui's draw data outlives the close by a frame, so
    // freeing it in DestroyTarget would be a use-after-free. DestroyTarget frees only the HDR
    // slot; ServiceIdleTeardown releases the ToneMapPass (2 shader-pool slots, 2 PSO slots, a
    // 32^3 LUT, an RTV and the output texture) once the grace proves the close over. Reopening
    // inside the grace costs one EnsureTexture - the tab-switch case.
    void          DestroyTarget(Target& t);
    // The idle sweep. Runs from ServiceReleases - the game half's serial window, outside the
    // device frame, with the render half parked - the only context allowed to destroy a graph
    // (the rule that keeps ServiceCompile out of Renderer::Render: never inside any graph's
    // Phase B).
    void          ServiceIdleTeardown();
    // Gives @p set back in full: every pass's Shutdown, every physical texture and the
    // declarations themselves. @p kind is the word the log line and the pack use ("live" /
    // "thumb" / "world"). A no-op on a set that was never compiled.
    void          DestroyGraphSet(GraphSet& set, const char* kind);
    // True when nothing @p set serves is live. Per set, because the three serve different things:
    // m_live the Synthetic views, m_world the WorldCull ones, m_thumb the one-shot queue.
    bool          LaneIdle(const GraphSet& set) const;
    // The body of ServiceCompile, per graph. Game half serial window only.
    bool          CompileGraphSet(GraphSet& set, GBufferPass* mainGBuffer,
                                  TransparentPass* mainTransparent,
                                  uint32_t w, uint32_t h);
    // Sets @p set's graph view (RenderGraph/SceneView.h): the studio shadow and backdrop @p req
    // asks for, and the forward pass when @p wantForward (a WorldCull set: no studio shadow, and
    // the forward pass whenever it has one). Render half, immediately before Execute; written in
    // full every frame.
    void          ApplyPreviewView(GraphSet& set, const Preview::Request& req,
                                   bool wantForward);
    // Creates lane @p lane's kFramesInFlight-deep clustered-light triple (gLights /
    // gLightIndexList / gLightGrid). Device-frame serial window only: called from PrepareTarget
    // beside EnsureSceneBuffers, never from the record half, because nine CreateBuffer calls and
    // nine descriptors are not a graph-worker operation (rule 3). See the clustered-light triple
    // members for why the grid is uniform. Lane kLaneWorld is created only when a WorldCull target
    // is prepared (see m_clusterReady).
    bool          EnsureClusterBuffers(uint32_t lane);
    // Writes this frame's studio fill/back point lights into lane @p lane, slot @p frameSlot, and
    // returns how many landed; 0 leaves LightCB.clusterLightCount at 0, which switches the whole
    // clustered block off. Lane 0 (the studio rig) only; see WriteWorldClusterLights.
    uint32_t      WriteClusterLights(uint32_t lane, uint32_t frameSlot,
                                     const Preview::Request& req);
    // Writes the scene light set the game half culled for @p req's view into lane kLaneWorld,
    // slot @p frameSlot, and returns how many landed. Render half. 0 gives the sun-and-IBL-only
    // picture, which every refusal here degrades to.
    uint32_t      WriteWorldClusterLights(uint32_t frameSlot,
                                          const Preview::Request& req);
    // Creates the 8x8x6 procedural checker TextureCube for Background::Checker, once, with
    // initialData - legal only here, because it runs in the game half's serial ServiceCompile
    // window outside BeginFrame..EndFrame, so CreateTexture's FlushUploadAndWait is not a
    // mid-frame hard sync. Called only when Background::Checker is the latched backdrop, so a
    // session that never selects it pays nothing.
    void          EnsureCheckerCube();
    // Builds the key light's reversed-Z ortho VP, writes it into this frame's m_shadowViewCB slot
    // and caches the transposed copy LightCB.shadowMatrix[0] needs. Render half. False when the
    // CB ring is not up yet; the shadow pass is then denied for the frame rather than drawn with
    // a stale matrix.
    bool          WriteStudioShadowView(const Preview::Request& req);
    // Any compiled set's GBufferPass, used only for the 1x1 default-white the space3 floor is
    // stamped with (identical in every instance). m_world is included because a session with
    // only a camera panel compiles m_world alone, and after an idle teardown the other two are
    // empty. Null when no set is compiled, which every caller tests for.
    GBufferPass*  AnyGBuffer() const
    {
        if (m_live.gbuffer)  return m_live.gbuffer;
        if (m_thumb.gbuffer) return m_thumb.gbuffer;
        return m_world.gbuffer;
    }
    // Finds and claims a free device HDR slot (1..kMaxHdrTargets-1; slot 0 is the main
    // viewport's for the life of the process). 0 = none free.
    uint32_t      ClaimHdrSlot();
    // Creates / resizes @p t's device HDR slot and ToneMapPass for @p w x @p h. Device-frame
    // thread, serial, before the graph Execute (rule 3).
    void          PrepareTarget(Target& t, uint32_t viewId, uint32_t w, uint32_t h);
    // Records one picture: the shared body of the live and thumbnail paths. Returns an invalid
    // list when nothing was recorded; @p inFlight is then cleared and @p req.generation must not
    // be published.
    // @p tile is the thumbnail destination (null on the live path); when set, the tone-map output
    // is copied into it and into this frame's readback ring slot, keyed by @p tileAssetKey,
    // before the list is returned.
    RHI::CommandList RecordRequest(GraphSet& set, Target& t,
                                   Preview::Request& req, bool& inFlight,
                                   RHI::CommandList colorLastCL,
                                   const RHI::Texture* tile,
                                   uint64_t tileAssetKey);

    // ---- Extract (game half) and thumbnail internals ----
    // The per-view body of ExtractRequests, game half. The WorldCull branch at its head is a
    // different state machine from the material one below it.
    void          ExtractOneRequest(uint32_t viewId, Target& t);
    // Game half only: resolves an asset view's parts, takes one refcounted runtime-material
    // reference per distinct material, and folds the whole into the dirty key.
    void          ExtractAssetParts(Target& t, uint32_t matEpoch);

    // ---- The two ownership schemes are exclusive ----
    // A Target holds runtime-material references either as the single-material `heldMatKey` or
    // as the asset path's `heldPartKeys` - never both. Both lanes (the live and the thumbnail
    // extract) switch between them, and these are the one place a switch hands the outgoing
    // scheme's references back (a browser folder holding materials and a `.meshlib` switches on
    // every tile); CheckSchemeExclusive trips if a third path appears.
    // Both respect @p matEpoch: past an OnWorldClear bump the keys are forgotten, not released -
    // releasing would decrement somebody else's.
    void          DropMaterialScheme(Target& t, uint32_t matEpoch);
    void          DropAssetScheme(Target& t, uint32_t matEpoch);
    void          CheckSchemeExclusive(const Target& t, const char* where) const;
    // Counts one live-lane asset extract as a full resolve (@p full) or a cheap readiness
    // refresh, and reports the window when it fills.
    void          NoteAssetExtract(bool full);

    void          ExtractThumbnails();     // GAME half, from ExtractRequests
    void          ApplyThumbFeedback();    // GAME half, from ApplyFeedback
    void          EnsureThumbReadback();
    // Stages the fence-covered readback slot into m_thumbPending. Called at the top of
    // PrepareFrame, on the device-frame thread with this frame's index current and before
    // Phase 4.3 records into that slot. It reads slot `frameIndex` (frame F-3's picture), the
    // only slot BeginFrame's WaitSlotRetired has fenced, so the copy is retired without a fence
    // of our own and without a FlushAndWait.
    void          ServiceThumbReadback();
    // The one size: the graph's physical textures, the device HDR slot and the ToneMapPass output
    // must match, and ServiceCompile (game half), PrepareFrame (device-frame thread) and
    // ProcessQueue (render half) all take it from here - the extract snapshot Target::reqW/H,
    // falling back to the latch. False when nothing has asked for a size yet.
    bool          ResolveRequestedSize(const Target& t,
                                       uint32_t& w, uint32_t& h) const;

    // ---- Draw helpers (Ensure*: device-frame serial window; the rest: render half) ----
    bool          EnsureSceneBuffers();                       // private instance + material tables
    void          EnsureCustomShaderNet();                    // b8 CBV ring + white space3 ring
    // Re-stamps part @p part's eight space3 descriptors in slot @p frameSlot with the preview
    // GBufferPass's 1x1 default-white. Runs before the packer on every custom draw: the slot last
    // held another material's textures, and one the packer does not write would keep them. This
    // floor keeps all eight naming a live descriptor.
    void          PrefillSpace3White(uint32_t frameSlot, uint32_t part);
    // Byte offset of part @p part's eight-descriptor space3 block in frame slot @p frameSlot, from
    // the base of the allocation. Per (frame, part), so two custom materials in one asset cannot
    // overwrite each other's slots.
    uint64_t      Space3BlockOffset(uint32_t frameSlot, uint32_t part) const;
    // Byte offset of part @p part's 256 B b8 slice inside a frame's CBV buffer.
    uint64_t      CustomCbvSliceOffset(uint32_t part) const;
    void          WritePerViewCB(GraphSet& set, const Preview::Request& req);
    void          WriteLightCB  (GraphSet& set, const Preview::Request& req,
                                 const LightingPass* lighting,
                                 uint32_t clusterLightCount);
    // Builds one packet, for the single-material path and for every asset part. @p part indexes
    // the private instance table and the custom rings; @p matSlot the private material table
    // (part + 1: slot 0 is the engine-reserved neutral record). False = do not draw this packet;
    // the reason has already been reported.
    bool          BuildOnePacket(const MaterialComponent& mc,
                                 uint64_t matRuntimeKey,
                                 uint32_t meshDescSlot, uint32_t indexCount,
                                 uint32_t customPSID, uint32_t customVSID,
                                 uint32_t part, uint32_t matSlot,
                                 uint32_t frameSlot,
                                 const ShaderReflect::Reflection* reflPS,
                                 bool wantForward,
                                 DrawPacket& dp);
    // Whether this material draws through TransparentPass#2: needs both a forward blend mode and
    // a forward instance in @p set. @p matRuntimeKey names the material the answer is about; it
    // keys the water-downgrade log latch, so each water material reports once.
    bool          WantsForward(const MaterialComponent& mc, uint64_t matRuntimeKey,
                               const GraphSet& set) const;
    bool          BuildDrawList (const Preview::Request& req, uint32_t frameSlot,
                                 const GraphSet& set,
                                 const ShaderReflect::Reflection* reflPS,
                                 bool wantForward);

    // ---- WorldCull record helpers (render half) ----
    // Fills m_worldPackets from Renderer::CullWorldForPreview. False when the camera is not
    // resolved or nothing survived the cull: an empty draw list is a legal picture (an empty sky)
    // but also what a broken viewMask looks like, so it is reported rather than drawn.
    bool          BuildWorldDrawList(const Preview::Request& req);
    // The WorldCull PerViewCB: the request's camera, reversed-Z like the main path, and byte
    // 228 = the scene's pristine instance-remap heap index.
    void          WriteWorldPerViewCB(GraphSet& set, const Preview::Request& req);
    // The WorldCull LightCB: the main frame's sun, IBL, sky SH and CSM, with every view-dependent
    // field re-derived from the request's camera and every screen-space subsystem off (the
    // definition lists what a secondary view does not get). @p clusterLightCount is what
    // WriteWorldClusterLights just wrote into this lane's upload slot - a parameter, as in
    // WriteLightCB, so the count and the CB field have one writer and cannot disagree.
    void          WriteWorldLightCB(GraphSet& set, const Preview::Request& req,
                                    const LightingPass* lighting,
                                    uint32_t clusterLightCount);
    // The whole render-half setup for a WorldCull view: draw list, constants, graph bindings and
    // the LightingPass / TransparentPass / SkyboxPass inputs, as one call from RecordRequest
    // rather than branches through the synthetic body. False = do not draw (no camera, nothing
    // survived the cull, scene buffers not up).
    bool          RecordWorldCullSetup(GraphSet& set, const Preview::Request& req);

    // ---- The grid is per view ----
    // How many grid rings lane @p lane owns: one per view on the WorldCull lane, one in total on
    // the studio lane.
    static constexpr uint32_t GridViews(uint32_t lane)
    { return lane == kLaneWorld ? Preview::kMaxPreviews : 1u; }

    // ---- WorldCull draw accounting ----
    // @p viewId -> a slot in the per-view arrays. Preview::kInvalidView (the default argument)
    // and any out-of-range id answer "whichever recorded last".
    uint32_t WorldCullSlot(uint32_t viewId) const
    {
        if (viewId < Preview::kMaxPreviews) return viewId;
        const uint32_t last = m_worldCullLastView.load(std::memory_order_relaxed);
        return (last < Preview::kMaxPreviews) ? last : 0u;
    }

    // ---- Harness key-light override ----
    // The studio key light's travel direction for this run: the harness override when one was
    // accepted, Preview::kKeyLight.dir otherwise. Out of line so this header does not pull
    // PreviewStudioRig.h into every translation unit that sees a PreviewRenderer.
    const float* KeyLightDir() const;

    // ---- Core state ----
    IGraphicsDevice& m_gfx;
    // The Renderer that owns this layer; never used for World state. What the record half
    // reaches through it, in full:
    //   - WriteMaterialSlot             - the one material packer (pure; takes the resolved
    //                                     runtime as an argument);
    //   - ResolvePreviewMaterialRuntime - reads m_runtimeMaterialResources;
    //   - LookupCustomShaderReflection  - reads the GBuffer ShaderLibrary's dynamic reflection
    //                                     vector, which RegisterDynamic reallocates;
    //   - GetMeshManager                - the persistent primitive table and the shared
    //                                     mesh-descriptor heap;
    //   - GetFallbackSkyCubeSrv         - a plain handle;
    //   - PackCustomMaterialBindings    - the only mutator: writes the preview-owned b8 slice and
    //                                     space3 ring, and reads the dynamic reflection vector
    //                                     again for the VS stage.
    // The first three and the last read state the game half mutates. They are safe only because
    // FramePipeline::DrainFrameWork serialises game-half mutation against render-half reads for
    // the whole editor frame. Loosening that park needs the resolved runtime and the reflection
    // snapshotted into Preview::Request on the game half, not a longer list here.
    Renderer&        m_owner;

    std::array<Target, Preview::kMaxPreviews> m_targets{};

    // Device HDR slots in use. Bit i set == slot i taken. Bit 0 is the main
    // viewport and is never handed out.
    uint32_t m_hdrSlotsUsed = 1u;
    // ...and the union of every slot ever claimed; it only grows. Packs use it to show two views
    // were alive at once: the live mask reads 1 at shutdown once the panels close (the leak
    // gate), and the per-frame previewViews counter legitimately reads 0, 1 or 2 within one run.
    uint32_t m_hdrSlotsPeak = 1u;

    // ProcessQueue's scheduler orders each lane's candidates by Target::lastTickFrame (ascending,
    // id as tie-break, m_priorityView first): fair under per-view tickIntervalFrames,
    // deterministic from frame 0, and stateless.
    // m_previewFrame is the record half's frame counter for tickIntervalFrames, advanced once per
    // ProcessQueue rather than read from the device, so "every 3rd frame" means every 3rd frame
    // the preview ran - the same in the editor and in a paused game.
    uint64_t m_previewFrame = 0;

    // Neutral tone-map inputs. ToneMap.cs.hlsl reads bloom (t1), the exposure buffer (t2) and
    // lens flare (t3) unconditionally, and SetComputeDescriptorTable ignores a 0 handle, which
    // would leave those slots holding whatever the list bound before (nothing, on a fresh list).
    // The main path's bloom / exposure are produced later in the frame than Phase 4.3, so they
    // cannot be borrowed.
    // Neither is created with initialData: EnsureNeutralInputs runs inside BeginFrame..EndFrame,
    // where a DEFAULT + initialData upload ends in FlushUploadAndWait, a mid-frame hard sync. So:
    //   - the black texture is a RENDER_TARGET created empty, cleared once on previewCL
    //     (m_neutralBlackCleared), then left in SHADER_RESOURCE;
    //   - the exposure buffer is an UPLOAD structured buffer, persistently mapped and written
    //     from the CPU (BeamSystem / PoseRingBuffer bind UPLOAD buffers as SRVs the same way).
    RHI::Texture   m_neutralBlack;      // 1x1 RGBA8 zero - bloom + lens flare

    // ---- Scene exposure for a WorldCull view ----
    // One buffer per frame slot, unlike the neutral one: the contents change every frame, and a
    // single UPLOAD buffer would be rewritten by frame N+1's CPU while frame N's tone map still
    // reads it. Written in RecordWorldCullSetup from Renderer::PreviewFrameState().sceneExposure
    // and used for both LightCB::exposureBufIdx (which custom_surface.hlsli's forward Unlit
    // branch divides by) and ToneMapPass#3's exposure input - the two must agree. Falls back to
    // m_neutralExposure until the scene has a CPU-visible reading.
    RHI::GPUBuffer m_worldExposure[RHI::kFramesInFlight];
    void*          m_worldExposureMapped[RHI::kFramesInFlight] = {};
    uint64_t       m_worldExposureSrv[RHI::kFramesInFlight] = {};
    bool           m_worldExposureReady = false;
    // The exposure SRV the WorldCull record's LightCB named this frame; the tone-map tail hands
    // ToneMapPass#3 the same one, or the forward divide is not undone. Written by
    // WriteWorldLightCB and read later in the same RecordRequest call (render half, one thread).
    uint64_t       m_worldToneMapExposureSrv = 0;

    RHI::GPUBuffer m_neutralExposure;   // 1 float == 1.0, UPLOAD + mapped
    uint64_t       m_neutralBlackSrv    = 0;
    uint64_t       m_neutralExposureSrv = 0;
    void*          m_neutralExposureMapped = nullptr;
    // Set ONLY when both SRVs came back non-zero. A latch that ignored a failed
    // creation would leave ProcessQueue binding handle 0 for bloom/exposure and
    // dispatching the tone map against whatever the root slot last held.
    bool           m_neutralReady       = false;
    bool           m_neutralBlackCleared = false;

    // ---- The three preview graphs ----
    // Each holds up to five passes, all second instances of the shipping classes, not
    // reimplementations. GBufferPass#2 and TransparentPass#2 borrow the main ShaderLibraries, so
    // a material's custom shaders compile and reload once for both views; LightingPass#2 and
    // SkyboxPass#2 own static libraries that draw no material shader.
    // Destruction order matters: PSOCache stores a bare ShaderLibrary*, so these graphs must die
    // before the main graph that owns the borrowed libraries. Renderer destroys m_preview before
    // m_graph, and the idle teardown runs while the main graph is alive and destroys only these
    // three.
    GraphSet         m_live;    // the inspector and asset preview, 128/256/384/512
    GraphSet         m_thumb;   // the asset browser's tiles, pinned 128^2
    // The WorldCull set, a third graph for the reason the thumbnails have their own: a camera
    // preview (480x270 PiP, or the docked window's size) is not one of the inspector's four
    // quantised squares, and sharing m_live would rebuild seven physical textures on every
    // alternation. Compiled lazily, so Game.exe with no PiP and an editor with no camera window
    // pay nothing.
    GraphSet         m_world;

    // ---- Per-lane record counters ----
    // The studio lane's single grid ring (GridViews(kLaneLive) == 1). Named rather than a bare 0
    // where it is indexed, so the world lane's per-view form and this one read alike.
    uint32_t         m_laneRenders[kLaneCount] = {};

    // ---- Per-view constants and the synthetic draw source ----
    // Per-view constants, lane 0 (m_live and m_thumb): private rings, one slot per frame in
    // flight, like ToneMapPass's own FrameCB. Renderer's single per-frame PerViewCB / LightCB slot
    // is off limits: a second writer there is read by every main pass and shows up as
    // intermittent flicker with no debug-layer message.
    // Lane 0 renders at most one picture per frame because ProcessQueue refuses the thumbnail
    // whenever m_laneRenders[kLaneLive] is non-zero, and the record-side ENGINE_CHECK counts per
    // lane, so a scheduler change that breaks the rule trips instead of flickering. The second
    // concurrent picture uses lane 1's rings (below); GraphSet::perViewCB points each set at its
    // lane.
    FrameCB<RendererDetail::PerViewCB> m_perViewCB;
    FrameCB<RendererDetail::LightCB>   m_lightCB;
    // The FogApply constants every preview graph binds (RG slot FogApply, b4): zero and never
    // written - no fog, no soft fade. The preview graphs run no fog composites, but
    // TransparentPass#2's forward main reads the block unconditionally. One ring serves every
    // lane because nothing writes it.
    FrameCB<FogApply::Constants>       m_fogApplyNoneCB;
    // Lane 1, the WorldCull set: two more 256 B x kFramesInFlight rings, created when the
    // WorldCull graph is first compiled.
    FrameCB<RendererDetail::PerViewCB> m_worldPerViewCB;
    FrameCB<RendererDetail::LightCB>   m_worldLightCB;
    // A second PerViewCB whose viewProj is the key light's, for PreviewStudioShadowPass. A
    // separate ring, not a second slot in m_perViewCB, because both are live in the same frame -
    // the shadow pass binds this one and GBufferPass#2 the camera one - and a FrameCB slot holds
    // one write per frame.
    FrameCB<RendererDetail::PerViewCB> m_shadowViewCB;
    // The light VP the shadow map was rendered with, TRANSPOSED for HLSL, as
    // LightCB.shadowMatrix[0] wants it. Written by the same helper that fills
    // m_shadowViewCB so the map and the matrix that samples it can never
    // disagree - the failure that produces is a shadow offset from its caster
    // with nothing in the log.
    float    m_studioShadowMatrix[16] = {};
    float    m_studioShadowTexelWorld = 0.f;
    bool     m_studioShadowValid      = false;

    // The synthetic draw source: one instance and one material record per part (material slot 0
    // stays the engine-reserved neutral record), deliberately outside the retained material
    // system, so SweepPersistentMatSlots' per-frame GC (which evicts every key not drawn this
    // frame) and the 4096-material budget do not apply.
    // UPLOAD + persistently mapped, kFramesInFlight deep: the CPU writes slot N while the GPU may
    // still read slot N-1.
    RHI::GPUBuffer m_instanceBuf[RHI::kFramesInFlight];
    void*          m_instanceMapped[RHI::kFramesInFlight] = {};
    RHI::GPUBuffer m_materialBuf[RHI::kFramesInFlight];
    void*          m_materialMapped[RHI::kFramesInFlight] = {};
    bool           m_sceneBuffersReady = false;

    // ---- Custom-shader bindings (root 35 b8 + root 36 t0..t7 space3) ----
    // A custom material's PS is compiled against b8 and up to eight space3 textures, and the root
    // signature needs both bound whenever the shader can reach them: an unbound or dangling
    // space3 table on a custom draw is a DEVICE_HUNG with no page fault and no debug-layer
    // message, not a black texture.
    // Renderer::PackCustomMaterialBindings (the packer BuildScene_SortAndEmit uses) fills both in
    // its caller-owned destination mode: it writes this ring slot and this eight-descriptor block
    // and never touches m_customMatCbvStore / m_customMatSrvStore, so a custom material previews
    // with its real parameter values and space3 textures. The preview still owns the floor:
    // PrefillSpace3White stamps all eight slots with default-white before the packer runs, so a
    // declared slot with no texture, or one left from the previous material, samples white.
    // Both are rings for the instance tables' reason: the CPU writes this frame's slot while the
    // GPU may still read the previous one.
    // The descriptor block is kCustomMatTextureSlots(8) x kFramesInFlight(3) x
    // kMaxPreviewParts(64) = 1536 shader-visible descriptors in one AllocateStatic, held by
    // m_customTexTable (an RHI::DescriptorBlock) and handed back by ~PreviewRenderer. The handles
    // are raw uint64_t so this header does not pull in <d3d12.h>.
    RHI::GPUBuffer m_customCbv[RHI::kFramesInFlight];
    void*          m_customCbvMapped[RHI::kFramesInFlight] = {};
    uint64_t       m_customCbvVA[RHI::kFramesInFlight]     = {};
    uint64_t       m_customTexTableGpu = 0;     // base of the 1536-descriptor block
    uint64_t       m_customTexTableCpu = 0;     // same block, shader-visible CPU side
    // Owns the block behind the two handles above.
    RHI::DescriptorBlock m_customTexTable;
    uint32_t       m_customTexTableInc = 0;     // descriptor increment
    // Last packer result reported (bytes, textures), so a pack can gate on it. Re-logged only when
    // the numbers change (streaming makes the resolved count climb): a handful of lines per
    // session.
    uint32_t       m_customPackLoggedBytes = 0xFFFFFFFFu;
    uint32_t       m_customPackLoggedTex   = 0xFFFFFFFFu;
    // Stops the space3 block allocation retry. The block needs the preview GBufferPass, which
    // exists only after ServiceCompile, so the allocation is retried rather than latched. Set
    // once the pass exists and an attempt still failed (e.g. the default-white SRV is
    // unresolved); that path hands the block back, so the retry cannot leak eight static
    // descriptors per frame.
    bool           m_customTexTableFailed = false;
    // An idle teardown leaves m_customTexTable alone. Most of its descriptors then name a
    // destroyed GBufferPass's default-white, which is harmless: root 36 binds only one (frame,
    // part) eight-slot block per draw, and all eight are rewritten on the frame the draw is
    // recorded (PrefillSpace3White from AnyGBuffer(), then the packer); a draw that cannot get
    // both is refused. Do not bulk re-stamp the whole block (or add a "stale" flag to drive one):
    // it would rewrite slots frames N-1 / N-2 are still sampling.
    // m_packets must outlive Execute: RenderGraph::SetDrawList stores a borrowed pointer and the
    // passes iterate it on their recording threads.
    std::vector<DrawPacket> m_packets;

    // ---- Clustered-light triple, per lane ----
    // What Lighting.ps's `if (clusterLightCount > 0)` block reads: gLights (t13),
    // gLightIndexList (t14) and gLightGrid (t15), built here instead of a ClusterPass#2.
    //   The grid is uniform: every cluster names every light in the set, at offset 0. That is
    //   conservative, not approximate - AccumClusteredLight opens with
    //   `if (dist > light.radius) return;`, so a light listed where it does not reach adds
    //   exactly zero and the result equals a perfect cull; only the cost differs.
    //   preview_parity_twosided checks it against the main ClusterPass froxel cull (maxDelta 1).
    //   What bounds the cost is the set: the game half frustum-culls and importance-caps the
    //   scene lights to Preview::kMaxPreviewLights (Renderer::ExtractPreviewSceneLights). If that
    //   cap is raised, re-run the cost ladder on it, not this grid.
    //   ClusterPass#2 would cost two dispatches, three PSOs, a ShaderLibrary and ~8.3 MB of
    //   DEFAULT-heap buffers to cull an already-culled set; this is ~243 KB per frame slot per
    //   lane, with no dispatch and no shader.
    // Per lane for correctness: lane 0 and lane 1 both render in one frame (kMaxPreviewsPerFrame
    // is 2: one Synthetic plus one WorldCull), and a shared ring would let both dispatches read
    // whichever wrote last, with no crash and no debug-layer message. m_laneRenders refuses a
    // second record per lane; this keeps the two lanes from sharing bytes.
    // All are UPLOAD + persistently mapped rings (the CPU writes this slot while the GPU may read
    // the previous one); binding an UPLOAD buffer as an SRV is the shipped shape (BeamSystem,
    // PoseRingBuffer). One cap for both lanes - see Preview::kMaxPreviewLights.
    RHI::GPUBuffer m_clusterLightBuf[kLaneCount][RHI::kFramesInFlight];
    void*          m_clusterLightMapped[kLaneCount][RHI::kFramesInFlight] = {};
    RHI::GPUBuffer m_clusterIndexBuf[kLaneCount][RHI::kFramesInFlight];
    void*          m_clusterIndexMapped[kLaneCount][RHI::kFramesInFlight] = {};

    // ---- The grid is per view ----
    // The light and index buffers are rewritten on every record, so one per (lane, frame slot)
    // suffices. The grid (243 KiB, a pure function of the light count) is memoised, and the memo
    // and the buffer need the same dimensions: one grid per (lane, slot) is rewritten on every
    // render once several views with different counts share the lane, and keying a shared buffer
    // on (view, count) would let a view bind another view's grid - a light silently missing. So
    // the ring is [lane][view][slot] and each view owns 243 KiB per slot.
    // Only the world lane pays: GridViews(lane) is kMaxPreviews for kLaneWorld and 1 for
    // kLaneLive, so the extra 9 x 243 KiB = 2.2 MiB is allocated only once a WorldCull view
    // exists.
    RHI::GPUBuffer m_clusterGridBuf[kLaneCount][Preview::kMaxPreviews][RHI::kFramesInFlight];
    void*          m_clusterGridMapped[kLaneCount][Preview::kMaxPreviews][RHI::kFramesInFlight] = {};

    // Per lane; lane 1 is created lazily. PrepareTarget's serial window runs for the first preview
    // of any kind and never frees, so an unconditional lane 1 would charge every inspector-only
    // session ~732 KiB for the life of the process. PrepareTarget asks for the lane it prepares.
    bool           m_clusterReady[kLaneCount]  = {};
    // Hard latch on a failed creation: the ring can fail part way, and a retry would overwrite an
    // already-created slot's handle and leak it every frame (as EnsureThumbReadback documents).
    // Per lane, so the warning names the lane that died.
    bool           m_clusterFailed[kLaneCount] = {};
    // The light count each grid slot was last written for, [lane][view][slot] like the buffer. A
    // changed count rewrites the grid; an unchanged one skips 243 KB of memcpy.
    uint32_t       m_clusterGridCount[kLaneCount][Preview::kMaxPreviews][RHI::kFramesInFlight] = {};
    // One line when the rig first lands, so a pack can gate on the lights having
    // reached the GPU rather than only on the picture.
    bool           m_clusterLoggedOnce = false;

    // ---- Scene light set, pushed in from the game half ----
    // Filled by Renderer::ExtractPreviewSceneLights (game half, end of BuildRenderScene, where
    // m_frameLights is final) and read by WriteWorldClusterLights (render half, for the WorldCull
    // view ProcessQueue picked this frame).
    // Fixed-size arrays, never a std::vector: one reallocating under the render half is the
    // failure this seam exists to prevent, and the arrays cost 4 views x 16 x 64 B = 4 KiB.
    Preview::GPULight m_viewLights[Preview::kMaxPreviews][kMaxPreviewLights] = {};
    uint32_t m_viewLightCount  [Preview::kMaxPreviews] = {};   // what was kept
    uint32_t m_viewLightScene  [Preview::kMaxPreviews] = {};   // punctual lights the scene had
    uint32_t m_viewLightCulled [Preview::kMaxPreviews] = {};   // dropped by the frustum test
    uint32_t m_viewLightDropped[Preview::kMaxPreviews] = {};   // dropped by the cap
    // `shadowed=` is counted off the packed records (see Renderer::ExtractPreviewSceneLights),
    // not printed as a literal, so the packs pinning `shadowed=0` go red the day shadow slices
    // reach WorldCull views.
    // TODO(MultiViewport_MaterialPreview_Plan NF1-D): punctual-light shadows for WorldCull views
    // (pass L.shadowSliceIdx through; borrow SpotShadowAtlas / SpotShadowVPs / pointShadowIdx).
    uint32_t m_viewLightShadowed [Preview::kMaxPreviews] = {};
    // Source lights refused for a non-finite position, radius or importance score - see the
    // strict-weak-ordering note at the nth_element.
    uint32_t m_viewLightNonFinite[Preview::kMaxPreviews] = {};
    // The push stamp is m_previewFrame + 1 on the game side: ProcessQueue increments
    // m_previewFrame later in the same frame, so at push time it still names F-1. Do not stamp
    // the bare value: the render half would refuse the set on every frame and WorldCull views
    // would silently get no scene lights (the tick regulator in ExtractOneRequest handles the
    // same off-by-one).
    uint64_t m_viewLightStamp[Preview::kMaxPreviews] = {};
    // What the render half last bound per view, for the Lua accessor and the log line. Atomic
    // (relaxed): written on the render half and read directly by Lua's Preview.WorldClusterLights
    // on the game half and by the editor panels - the contract every counter here that is read
    // directly, rather than through Renderer's mailbox, carries.
    std::atomic<uint32_t> m_viewLightBound[Preview::kMaxPreviews] = {};
    // Log-once-on-change bookkeeping for the "[PREVIEW] world clustered lights:"
    // line (one line per transition, never per frame).
    uint32_t m_viewLightLoggedUsed    [Preview::kMaxPreviews] = {};
    uint32_t m_viewLightLoggedDropped [Preview::kMaxPreviews] = {};
    // `shadowed` and `nonfinite` are part of the change test too, so a change in either alone
    // still prints a line - the one transition the field exists to catch.
    uint32_t m_viewLightLoggedShadowed[Preview::kMaxPreviews] = {};
    uint32_t m_viewLightLoggedNonFin  [Preview::kMaxPreviews] = {};
    bool     m_viewLightLogged        [Preview::kMaxPreviews] = {};
    // Refusal counters. A real stamp mismatch (a camera that went invalid between push and
    // record) is legitimate and silent; a persistent one is the off-by-one bug, and the shutdown
    // line shows it: a session whose every world render refused a non-empty set drew no scene
    // lights.
    uint32_t m_worldLightRenders     = 0;   // world records that reached the writer
    uint32_t m_worldLightStaleRefusals = 0; // ...refused on the stamp
    uint32_t m_worldLightMinUsed     = 0xFFFFFFFFu;
    uint32_t m_worldLightMaxUsed     = 0;
    // How often the 243 KiB uniform grid was rewritten. Each view owns its grid ring, so a settled
    // session writes each slot once: the ceiling is (live world views) x kFramesInFlight - 3 for
    // one view, 12 for four split cells. A number that keeps climbing means the light count
    // oscillates (the cap flickering), CPU with no picture change; the shutdown line reports it
    // against `renders`.
    uint32_t m_worldLightGridRewrites = 0;
    // Session scope of the shadowed / nonfinite tallies: `shadowed` is a max (one record binding
    // a real shadow slice is the event worth catching), `nonfinite` a total (each one is a light
    // thrown away).
    uint32_t m_worldLightShadowedMax  = 0;
    uint32_t m_worldLightNonFiniteTot = 0;
    // What the most recent world record bound, for the viewless form of
    // Preview.WorldClusterLights(). Zeroed at the top of WriteWorldClusterLights. Last rather than
    // a max over the per-view array, which would report an OFF window as on for every view but the
    // one that just refused. Same atomic contract as m_viewLightBound above.
    std::atomic<uint32_t> m_worldLightLastBound{ 0 };
    // The runtime off-arm (see SetWorldSceneLightsEnabled): the process-wide toggle the cost pack
    // flips between its ON and OFF windows. Written on the game half inside the parked extract
    // window, read on the render half; a plain bool across a park-first seam, like
    // m_sceneLighting.
    bool     m_worldSceneLightsEnabled = true;

    // ---- Checker backdrop cube ----
    // 8x8 per face, RGBA8, created once with initialData from the serial ServiceCompile window.
    // SkyboxPass#2 samples it as it would a sky cube (no new pass, no new shader), and its
    // trilinear sampler smooths it into a soft studio backdrop rather than aliasing.
    RHI::Texture m_checkerCube;
    uint64_t     m_checkerCubeSrv = 0;
    bool         m_checkerTried   = false;

    uint64_t m_iblIrradiance = 0;
    uint64_t m_iblRadiance   = 0;
    uint32_t m_iblRadianceMips = 1;
    uint64_t m_brdfLut       = 0;

    // ---- Studio-IBL readiness edge ----
    // False until both studio cubes first resolve; the flip is a dirty edge, like
    // Target::lastTexturesReady. The studio HDRI streams in asynchronously and an unchanged
    // ViewDesc mints no new generation, so without this edge a panel that drew its first picture
    // while SetEnvironment still pushed 0/0 would keep that unlit picture for the session - a
    // golden that is wrong one run in ten.
    bool     m_iblEverReady = false;
    SceneLighting m_sceneLighting{};

    // ---- Thumbnail lane state ----
    // FIFO, drained one per frame and only in frames the live view did not use (the probe-bake
    // budget policy): a 300-tile burst takes ~5 s and never hitches, which the churn pack's
    // cpuMs p99 assertion gates.
    std::deque<Preview::ThumbRequest> m_thumbQueue;
    // The one in flight. `valid`/`inFlight`/`generation` carry the same meaning
    // they do in the live latch; what differs is who sets them and when (see
    // Preview::ThumbRequest).
    Preview::Request m_thumbReq{};
    uint64_t         m_thumbAssetKey = 0;    // identity of m_thumbReq
    // The editor's tile texture for m_thumbReq: a handle borrowed for the frames between extract
    // and record, cleared with the latch.
    RHI::Texture     m_thumbTile;
    Target           m_thumbTarget{};
    Preview::ThumbResult             m_thumbResult{};
    std::vector<Preview::ThumbResult> m_thumbDone;   // drained by the editor
    std::atomic<uint64_t> m_thumbsRenderedTotal{ 0 };
    bool             m_thumbFirstLogged = false;
    // Set on success by RecordRequest's tile block; ProcessQueue publishes a
    // result only when it is true, so a render whose copy was skipped cannot
    // mark a zero-filled tile ready.
    bool             m_thumbTileCopied  = false;
    // Hard latch: the tone-map output is FP16 in every HDR output mode and CopyTextureRegion
    // between formats is undefined, so the lane switches itself off rather than corrupting tiles.
    // One LOG_WARNING says so - not an ENGINE_CHECK: an HDR mode is a legitimate configuration,
    // and a CHECK FAILED line is an engineError every pack counts. ThumbnailsDisabled() lets the
    // editor stop creating tiles for it.
    bool             m_thumbDisabled    = false;
    // How many times the CURRENT request has been re-sent after a record
    // early-out. Bounded so a permanent refusal cannot spin the FIFO head.
    uint32_t         m_thumbResends     = 0;

    // kFramesInFlight-deep readback ring, never a hard sync: the record half copies the tile into
    // slot `frameIndex`, and PrepareFrame stages the same slot at the top of the frame, before
    // this frame's record overwrites it - frame F-3's picture, which BeginFrame's
    // WaitSlotRetired(frameIndex) has retired. Do not read (frameIndex+1): frame F-2's slot is not
    // fenced (see ServiceThumbReadback). 128^2 RGBA8 is 64 KiB per slot with a 256-aligned row
    // pitch of 512, 192 KiB for the ring.
    RHI::GPUBuffer   m_thumbReadback[RHI::kFramesInFlight];
    void*            m_thumbReadbackMapped[RHI::kFramesInFlight] = {};
    bool             m_thumbReadbackPrimed[RHI::kFramesInFlight] = {};
    uint64_t         m_thumbReadbackKey[RHI::kFramesInFlight]    = {};
    uint32_t         m_thumbReadbackGen[RHI::kFramesInFlight]    = {};
    uint32_t         m_thumbRowPitch   = 0;
    uint32_t         m_thumbRows       = 0;
    uint64_t         m_thumbReadbackBytes = 0;
    bool             m_thumbReadbackReady = false;
    // Hard latch on a failed creation: the ring can fail part way (slot 0 created and mapped,
    // slot 1 refused), and a retry would overwrite slot 0's handle - one 64 KiB READBACK leak per
    // frame. See EnsureThumbReadback.
    bool             m_thumbReadbackFailed = false;

    // Bounded: the lane produces at most one per frame and the editor drains
    // every frame, so this holds 0 or 1 in the steady state. The cap exists so
    // an editor that stops draining (a modal, a long import) cannot grow it.
    std::deque<PendingPixels> m_thumbPending;
    PendingPixels             m_thumbHanded;   // keeps TakeThumbnailPixels' bytes alive

    // RENDER half -> harness. See LastRenderedView().
    std::atomic<uint32_t> m_lastRenderedView{ Preview::kInvalidView };
    // See LastRenderedStencil(). 0 = nothing drawn yet (no stencil code is 0).
    std::atomic<uint32_t> m_lastRenderedStencil{ 0 };
    // See LastRenderedForward(). 1 = TransparentPass#2 drew the subject.
    std::atomic<uint32_t> m_lastRenderedForward{ 0 };
    uint32_t m_viewsRendered    = 0;
    uint32_t m_viewDraws        = 0;
    // See ViewsRenderedTotal(). Monotonic, never reset by ProcessQueue.
    std::atomic<uint64_t> m_viewsRenderedTotal{ 0 };
    // Per slot twin of m_viewsRenderedTotal - see ViewRenderCount().
    std::atomic<uint64_t> m_viewRenderCount[Preview::kMaxPreviews]{};
    // The WorldCull draw accounting behind LastWorldCullDraws / ...Candidates / ...Secondary, per
    // view: up to Preview::kMaxPreviews WorldCull views are live at once (all at the lane's size),
    // each culling its own frustum. Indexed by the recording view; m_worldCullLastView answers
    // "whichever recorded last" for the default-argument accessors.
    std::atomic<uint32_t> m_worldCullDraws[Preview::kMaxPreviews]{};
    std::atomic<uint32_t> m_worldCullCandidates[Preview::kMaxPreviews]{};
    std::atomic<uint32_t> m_worldCullSecondary[Preview::kMaxPreviews]{};
    // Leafless counters: the same per-view shape, written by the same callback; read only by the
    // "[PREVIEW] world cull:" line.
    std::atomic<uint32_t> m_worldCullLeaflessPrim[Preview::kMaxPreviews]{};
    std::atomic<uint32_t> m_worldCullLeaflessOther[Preview::kMaxPreviews]{};
    std::atomic<uint32_t> m_worldCullLeaflessPrimKept[Preview::kMaxPreviews]{};
    std::atomic<uint32_t> m_worldCullLeaflessOtherKept[Preview::kMaxPreviews]{};
    // The view BuildWorldDrawList is culling for. Render half only and plain (not atomic): it is
    // written and read on the one recording thread, between the store and CullWorldForPreview's
    // NoteWorldCullCounts callback.
    uint32_t              m_worldCullRecordingView = 0;
    // Which view recorded last; what the accessors' default argument resolves to.
    std::atomic<uint32_t> m_worldCullLastView{ 0 };

    // Per-view latch for the "[PREVIEW] world cull:" line, re-emitted when that view's count
    // moves by more than a packet, so a pack gates on a real number without a per-frame flood.
    // Per view: with several camera views culling different counts, one shared latch would
    // re-emit on every hand-over.
    uint32_t m_worldCullLoggedDraws[Preview::kMaxPreviews] = {};
    // The two ...Kept counters are part of the re-emit predicate: the runner's logLineFields
    // reads the last matching line, so on a scene whose draw count settles (BaseScene: 1) the
    // line would otherwise stop at frame 1, and a pack bounding leaflessPrimKept would read a
    // value taken before the scene settled - a false pass.
    uint32_t m_worldLoggedLeaflessPrimKept[Preview::kMaxPreviews]  = {};
    uint32_t m_worldLoggedLeaflessOtherKept[Preview::kMaxPreviews] = {};
    // Per-view one-shot diagnostics that tell "the secondary view drew nothing" from "it was
    // never asked to draw" - the same absence in the log, different causes. Reset by
    // AcquireView / ReleaseView: they describe one tenancy, and a re-acquired view that inherited
    // them would render its first picture with no evidence in the log. Per view, so view 0's
    // first attempt cannot silence the others.
    bool     m_worldCullAttempted[Preview::kMaxPreviews]   = {};
    bool     m_worldCullEmptyLogged[Preview::kMaxPreviews] = {};
    bool     m_worldFirstLogged[Preview::kMaxPreviews]     = {};

    // ---- Which lane a view is: one source of truth ----
    // The slot's source, latched at acquire. Every lane question - ReleaseView, the live count,
    // the ownership predicate in RequestWorldCullLaneSize, LaneIdle, the scheduler, the record
    // route, ServiceCompile - goes through SlotSource() / IsWorldCullSlot(). req.view.source is
    // what the panel declares; SetRequest refuses a request where the two disagree, so the
    // answer cannot split (a split answer can destroy a GraphSet still in use).
    // Synthetic by default, as an unacquired slot must read: the world lane counts membership,
    // and a wrong default would make an empty lane look occupied and defer its teardown for ever.
    Preview::Source m_slotSource[Preview::kMaxPreviews] = {};
    // One latch per view for that refusal, so one miswired cell cannot silence the next. Cleared
    // on acquire and on release, like every other per-tenancy one-shot here.
    bool     m_sourceRefusedLogged[Preview::kMaxPreviews] = {};
    uint32_t m_worldCullLive        = 0;
    // The one size every live WorldCull view shares: fixed by the first accepted request, cleared
    // when the last WorldCull view is released (a fresh tenancy may pick another size, costing
    // one recompile). 0 = not fixed yet.
    uint32_t m_worldSizeW           = 0;
    uint32_t m_worldSizeH           = 0;
    uint32_t m_worldSizeOwner       = Preview::kInvalidView;
    // The size refusal is logged once per tenancy, not per process, so a later configuration that
    // meets the rule (after a panel closed, a world reload, a lane move) still gets its line.
    // Cleared wherever the lane's size stops being the refused one: the last WorldCull release, a
    // lane move, and NoteWorldClear.
    bool     m_worldSizeRefusedLogged = false;
    // The lane-move refusal latch, separate from m_worldSizeRefusedLogged because a panel refused
    // the move then has every per-cell request refused too, and one shared latch would swallow
    // the second line (the one the _size packs assert). A set: one line per distinct refused
    // size. A bool would hide a second panel's refusal, and a last-value latch would log twice a
    // frame while two panels re-ask at alternating sizes. Bounded by kMaxPreviews (a tenancy has
    // at most that many sizes refused at once); when full a further size is silent, and the
    // panel's own UI still reports it. Packed (w << 16) | h; cleared wherever the lane's size
    // stops being the refused one.
    uint32_t m_moveRefusedSizes[Preview::kMaxPreviews] = {};
    uint32_t m_moveRefusedCount = 0;
    // Per-view "your size was refused", so the cell can say why instead of
    // showing an empty box for the life of the session.
    bool     m_viewSizeRefused[Preview::kMaxPreviews] = {};
    // The zero-extent refusal has been logged for this view. Per view, so one broken cell cannot
    // silence the next; re-armed by ReleaseView and NoteWorldClear.
    bool     m_zeroSizeLogged[Preview::kMaxPreviews] = {};
    // The pool-exhaustion refusal has been logged this tenancy. Re-armed by ReleaseView: "all
    // slots in use" is worth one line each time the configuration reaches it and no more while it
    // stays there (a panel asks every frame).
    bool     m_slotsExhaustedLogged = false;
    // Consecutive failed acquires during which every slot was parked in pendingDestroy. The leak
    // signature is the streak, not the state - see AcquireView.
    uint32_t m_allParkedStreak      = 0;
    // The round-robin's preference (see SetPriorityView), written by the editor panel and read
    // by ProcessQueue; atomic because the UI half and the record half are one thread only on the
    // serial-pinned editor host. m_priorityFrame is the published preview frame it was set on;
    // ProcessQueue honours it for that frame plus one, then clears it. m_previewFramePub
    // republishes ProcessQueue's counter for readers off the record half; never read
    // m_previewFrame from the UI.
    std::atomic<uint32_t> m_priorityView{ Preview::kInvalidView };
    std::atomic<uint64_t> m_priorityFrame{ 0 };
    std::atomic<uint64_t> m_previewFramePub{ 0 };

    // ---- Rebuild cadence, per wall-clock hour ----
    // The idle teardown makes a GraphSet rebuild (43-231 ms) a once-per-reopen cost for any panel
    // closed longer than the grace (5 s). These feed `rebuilds` / `rebuildMs` / `rebuildsPerHr` /
    // `rebuildMsPerHr` on the session-totals line, to show how often that trade fires. A rebuild
    // is any build of a set built before, which (`compiled` goes false only in DestroyGraphSet)
    // is exactly a build after a teardown.
    // Read the per-hour pair off an ordinary editor session, not a pack: a pack forces its own
    // cadence (preview_teardown cuts the grace to 20), so its per-hour figure describes the pack.
    // A pack can gate `rebuilds` itself.
    uint32_t m_graphRebuilds   = 0;
    double   m_graphRebuildMs  = 0.0;
    std::chrono::steady_clock::time_point m_sessionT0 =
        std::chrono::steady_clock::now();

    // ---- Shutdown accounting for the "[PREVIEW] split:" line ----
    // Per-view WorldCull render counts, the peak of concurrently live WorldCull views, the
    // preview frames that passed with at least one live, and each view's last draws / secondary.
    // `renders` and `frames` make the round-robin a number: N views at tick 1 should each land
    // near frames / N. All of it is per session, the per-view half included: nothing on this line
    // resets while the process lives, so numerators and denominators share one clock and
    // `renders` equals the sum of v0..v3.
    uint32_t m_worldViewRenders[Preview::kMaxPreviews]   = {};
    // The size the per-view "world cull:" line last reported. A resize re-emits it: that line is
    // the only per-view evidence a pack has that a new size reached the raster rather than being
    // refused.
    uint32_t m_worldViewLoggedW[Preview::kMaxPreviews] = {};
    uint32_t m_worldViewLoggedH[Preview::kMaxPreviews] = {};
    uint32_t m_worldViewLastDraws[Preview::kMaxPreviews] = {};
    uint32_t m_worldViewLastSec[Preview::kMaxPreviews]   = {};
    uint32_t m_worldCullPeakViews   = 0;
    uint32_t m_worldCullRenders     = 0;
    uint64_t m_worldCullFrames      = 0;
    uint32_t m_worldCullMaxTick     = 0;
    // STICKY, unlike m_worldSizeW/H, which are released with the last view. The
    // shutdown line runs after every panel has closed, so a released size would
    // print 0x0 for every run that tidied up after itself.
    uint32_t m_loggedWorldW         = 0;
    uint32_t m_loggedWorldH         = 0;
    // How many times the WorldCull GraphSet has been compiled this session.
    // A resize is one legitimate recompile; a size that stopped being quantised
    // is one per frame, and that is the failure the packs gate on.
    uint32_t m_worldCompileCount    = 0;
    // The packets a WorldCull record hands the graph. A member because the DrawList contract
    // requires it to outlive Execute, and separate from m_packets so a Synthetic and a WorldCull
    // record in the same frame cannot overwrite each other's list.
    std::vector<DrawPacket> m_worldPackets;
    bool     m_firstRenderLogged = false;
    // The size the "first render" line last reported. A resize re-emits it, so
    // a pack can gate on the recompile having produced a PICTURE at the new
    // size rather than merely having completed.
    uint32_t m_loggedW = 0, m_loggedH = 0;
    bool     m_selfHealLogged    = false;
    bool     m_epochHealLogged   = false;
    // One-shot studio-shadow diagnostic: see its emit site in RecordRequest.
    bool     m_shadowLogged      = false;
    // The line naming what the WorldCull sky adopted from the main pass, emitted once per
    // discrete shape (valid, moon drawable, stars cube bound) - not once per session, and not
    // keyed on the intensities, which move every frame in a day/night cycle. A settled scene emits
    // one line, one that reaches night a second, and logLineFields (last match) reads the shape
    // the captured picture was drawn with. -1 = nothing reported yet.
    int      m_worldSkyLoggedShape = -1;
    // The exposure the WorldCull lane bound (an informational line), with a separate latch for
    // the neutral fallback (a warning a pack counts): starting before the first reading is
    // normal, failing to create the buffer is not.
    bool     m_worldExposureLogged        = false;
    // The last exposure the WorldCull lane actually BOUND and reported. A
    // value rather than a latch: the line is re-emitted on every change, so
    // an exposure ramp leaves one line per step and the LAST one names the
    // exposure the captured picture was drawn at. -1 = nothing bound yet.
    float    m_worldExposureLastLogged   = -1.f;
    bool     m_worldExposureNeutralWarned = false;
    // Route reporting: see the emit site. Two flags, not one, because `false` is
    // a meaningful route and a single bool could not tell it from "never said".
    bool     m_forwardLogged     = false;
    bool     m_forwardEverLogged = false;

    // The water downgrade, reported from WantsForward, the one branch that makes it (mutable
    // because that function is const; a line raised anywhere else could disagree with the
    // decision). Latched per (matRuntimeKey, blend): each water material logs once, a blend edit
    // re-logs once, and a material re-rendered every frame in three views still logs once. A
    // linearly scanned vector: bounded by the lane's material count, kept for the process like
    // m_forwardEverLogged, and off the allocator in the common no-water case.
    mutable std::vector<WaterDowngradeKey> m_waterDowngradeLogged;
    // The last (packets, materials) pair the asset path reported. Sentinels rather than 0: "0
    // packets" is a real result (a prefab whose library has not streamed in yet), and a
    // 0-initialised pair would swallow its first report.
    uint32_t m_assetLoggedPackets   = 0xFFFFFFFFu;
    uint32_t m_assetLoggedMaterials = 0xFFFFFFFFu;
    bool     m_assetOverCapLogged   = false;
    // Assets whose composition yields no drawable part, reported once per asset: the answer is
    // permanent and the tile is dropped rather than retried.
    std::unordered_set<std::string> m_assetNoPartsLogged;
    // How often the live asset extract did the full resolve versus the cheap readiness refresh,
    // reported once per window, so the asset extract's idle cost is a measured number.
    uint32_t m_assetExtractFull    = 0;
    uint32_t m_assetExtractSkipped = 0;
    uint32_t m_assetExtractWindow  = 0;

    // ---- Idle teardown ----
    // The grace, in ServiceReleases frames: Preview::kIdleTeardownFrames unless a harness flag
    // shortened it (SetIdleTeardownFrames).
    uint32_t m_idleTeardownFrames = Preview::kIdleTeardownFrames;

    // ---- Harness key-light override ----
    // The members are unconditional and only the setter is harness-only, so the read sites are
    // one branch of ordinary code rather than two spellings under an #ifdef.
    // Three code paths read the key light. WriteStudioShadowView (where the depth map is cast
    // from) and WriteLightCB (LightCB.lightDir, where it is shaded from) both go through
    // KeyLightDir(), so they can never disagree. The third, EditorLayer's -preview_parity world
    // builder, aims its "ParityKeyLight" from Preview::kKeyLight.dir directly, on purpose: the
    // parity packs compare against the main pipeline pixel for pixel, so both sides need the
    // shipped rig. Do not combine -preview_key_light with -preview_parity: only the preview side
    // would move, and no code gate stops it.
    bool  m_keyLightOverridden = false;
    float m_keyLightOverride[3] = { 0.f, 0.f, 0.f };
    // The -preview_shadow_alpha lever, pushed to every set's shadow pass each frame. Shipped value
    // true: alpha-tested casters clip in the studio shadow.
    bool  m_studioShadowAlphaTest = true;

    // Session totals for the shutdown summary. `cycles` counts SWEEPS that
    // destroyed at least one thing, so three open/close cycles read as 3 even
    // though each one may give back two sets.
    uint32_t m_teardownCycles     = 0;
    uint32_t m_setsDestroyed      = 0;
    uint32_t m_toneMapsDestroyed  = 0;
    // How many world boundaries this layer saw, and what ViewsRenderedTotal read at the last one.
    // See NoteWorldClear.
    uint32_t m_worldClearCount    = 0;
    uint64_t m_rendersAtLastClear = 0;

    uint32_t m_teardownCount[kTearKinds] = {};
    // Descriptor high-waters (cbv, rtv, dsv, sampler) at the first and the last teardown of each
    // kind, indexed by TeardownKind. `used` is a monotonic cursor FreeStatic never decrements, so
    // it stays flat exactly when the free list is reused; comparing cycle 1 with cycle N of the
    // same kind is the "cycle 3 high-water == cycle 1" gate. Per kind, not per session: a run's
    // first teardown is the thumbnail set, landing while the asset browser still creates tiles,
    // so a session-wide delta would measure unrelated lazy allocation.
    uint32_t m_teardownFirst[kTearKinds][4] = {};
    uint32_t m_teardownLast [kTearKinds][4] = {};
};
