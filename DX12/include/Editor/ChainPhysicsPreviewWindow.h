#pragma once

// ChainPhysicsPreviewWindow - the "Chain Physics Preview" editor panel, the one
// place chain physics is edited: a dockable panel with its own viewport and a
// parameter column (side by side or stacked, see Layout).
//
// STAGE. The previewed character (the "subject": an .ipfb prefab, or a clone of a
// selected scene character) is spawned into the MAIN World as a hidden stage at
// y = kStageOriginY, and the picture is an ordinary WorldCull preview view through
// the panel's own camera entity. The stage lives in the main World so animation,
// skinning, materials and face shading serve it for free. A spawn over budget is
// refused; the budget counts the scene's own load.
//
// CLOCK. The panel owns the clip clock (Controller: RestPose / Clip /
// FollowSceneEntity): every frame it writes externalTime / paused / primaryTime /
// prevNotifyTime on the stage's AnimationComponent - Edit mode's animDt = 0 gates
// only the advance, never the sample. The stage's skeletons go to
// Renderer::AddAnimPinOwner, so anim culling stays on and the stage still
// animates. One StageClock yields N fixed steps per editor frame and the same N
// advances the clip (m_clipT), the root-motion driver and the chain solver's
// PREVIEW LANE (SetChainPhysicsPreviewLane(set, stepDt, N): N passes in one
// Update; the clip is sampled once per frame and pass k's target is interpolated
// k/N from the previous frame's input). The main lane excludes the stage, so
// nothing is integrated twice and the scene's own chains stay as they were. A
// frame with N = 0, and Pause, HOLD the last simulated pose
// (ChainPhysicsSystem::HoldPass) instead of flickering to the raw clip. A clip
// change / step / jump / scrub resets the stage's simulation (ResetPreviewEntity);
// reverse playback holds it and disables Simulate.
//
// ONE WRITER. m_staged is the authority: Parameters draws
// EditorLayer::DrawChainParamTree on it, and ProcessPendingActions syncs it into
// the stage's live ChainPhysicsComponent field by field (CopyAuthoredInto, never
// `*live = m_staged`) only when it changed (AuthoredEquals). A structural change
// (ChainPhysicsSystem::NeedsReinit, or the tree's structural hook) is followed by
// Invalidate, a "chains re-initialised" toast and the re-init marker. Panel undo
// is a 64-deep ring + redo, Ctrl+Z / Ctrl+Y only while the panel is focused, never
// the global EditHistory; a structural undo replays Invalidate through the same
// sync. DrawComponentWithUndo edits on the stage never reach the global undo
// stack, the scene-dirty flag or the scene selection (PreviewStageTag gate). Every
// teardown captures the live parameters into m_staged and the next spawn of the
// same subject session re-applies them (Play -> Stop keeps the tuning). The
// rest-pose tool state is per tree (ChainPoseState): the stage's lives here, never
// in EditorLayer's main-viewport state. The overlay (ChainPreviewOverlay) is drawn
// into the panel's draw list from a snapshot PPA takes, with an edit and a sim
// show mask.
//
// SCENE. A raw .ipfb spawns at its authored scale while scenes place the character
// scaled, so the stage defaults to the scale of the scene's first instance of the
// same prefab (PrefabSourcePath); the Scene section's Scale control rewrites it and
// resets the chains. The solver simulates in SKELETON space, so neither the scale
// nor the driver's motion reaches the chains unless the rig authors world-motion
// inertia (ChainPhysicsComponent::worldInertiaLocation / Rotation): then starts /
// stops / turns kick the chains and a hop beyond teleportDistance / teleportAngle
// is ignored. Floor: a visual plane plus an optional capsule proxy the solver can
// collide with (it only has capsules). Scene gravity and the floor proxy are
// preview-lane settings pushed every frame (never authored; the solver maps them
// into skeleton space); wind is authored and goes to m_staged. The root-motion
// driver (translate / rotate / teleport, the stress programs) is THE writer of the
// stage root's LocalTransform. Turntable / pan / Focus move the camera only.
//
// Apply / Revert / presets: ChainPhysicsPreviewApply.cpp. The Scene Apply target,
// the dirty guard, the world-load stash, the solver-state stats (FillDebugSnapshot
// -> the overlay's sim bits, the stats box, the "[CHAINPREVIEW] stats:" line),
// "Edit Chains..." and the Play keep slot: ChainPhysicsPreviewSession.cpp. The A/B
// ghost, solo and poke: ChainPhysicsPreviewCompare.cpp. The in-picture rest-pose
// gizmo: ChainPhysicsPreviewPose.cpp. The -chainpreview_* flag parsing:
// ChainPhysicsPreviewHarness.cpp. In Play, Parameters becomes "LIVE: <entity>"
// (EditorLayer's ChainLiveEditFn: transient, no global undo) with "Keep these
// values", written into m_staged after Stop as one panel-undo step. There is no
// continuous auto-apply (two writers + double undo).
//
// The render size comes from both axes of the view box (16 px steps, area cap
// 1280 x 720, debounced on (W, H)); the picture is displayed fit-scaled and
// centred, never stretched, and the overlay, orbit, pan and poke all use that
// displayed rect. Under -chainpreview_size the display scale stays 1 at the
// top-left, so the pinned packs and the overlay golden do not move.
//
// HOW THE STAGE STAYS OUT OF THE SCENE:
//   .iscene save           SceneTransientTag on EVERY stage entity
//   Outliner               PreviewStageTag pool test (EditorLayer_Outliner)
//   viewport picking       EditorLayer::SetPickResult drops tagged entities
//   main viewport          viewMask loses ViewBit::MainCamera, KEEPS
//                          ViewBit::SecondaryView; renderInMainPass stays TRUE
//                          (filterList tests it BEFORE the view bit)
//   CSM / shadows          castShadow = false
//   Jolt / AI / Lua / ...  STRIP list right after LoadPrefab (KEEPS
//                          CapsuleColliderComponent; REMOVES Locomotion, the
//                          other animation writers and VFX)
//   structural undo        the spawn is never RecordSpawn'd
//   Save as Prefab / Copy  tag guard at both SavePrefab call sites
//   Play snapshot          TearDownForPlay runs before EnterPlay's CloneState;
//                          the stage is rebuilt after Stop
//   BVH leaf               accepted (a skinned subject has none)
//   DDGI / skinned TLAS    accepted residual
//   orphan GPU buffers     every stage entity is marked
//                          Renderer::MarkSkinnedBuffersUnsnapshotted before the
//                          stage is destroyed
//
// SIDE-EFFECT DISCIPLINE:
//   * Draw() runs in the editor UI half and touches NO World and NO renderer
//     state. It edits the panel's own orbit / size / subject request and reads
//     GetPreviewSrv once per frame (never cached).
//   * ProcessPendingActions() (PPA) is the ONLY place the panel spawns / destroys
//     entities, acquires / releases the view, requests pictures and moves the
//     WorldCull lane. It runs in EditorLayer::ProcessPendingActions, the game
//     half's serial window.
//   * A World::Epoch() change (a clear, AdoptState, another World) recycles
//     entity ids, so every stage handle is DROPPED - never destroyed, the ids may
//     name someone else's entities now - and the stage is respawned from its
//     subject or cleared with it, never re-found by name.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"                      // Entity
#include "Editor/ChainParamTree.h"        // ChainParamHooks / ChainParamTreeFn
#include "Editor/ChainPreviewOverlay.h"   // the overlay snapshot + masks
#include "Editor/ChainPresetLibrary.h"    // presets (m_userPresets)
#include "Editor/PreviewStage.h"          // the shared stage mechanics + origin slots
#include "Editor/TimelineCanvas.h"        // the transport's scrub ruler
#include "Editor/Undo/EditAction.h"       // Undo::IEditAction (Apply -> scene undo)
#include "Physics/ChainPhysicsSystem.h"   // ChainPhysicsComponent (m_staged)
#include "Resource/SystemHandles.h"       // Resource::AnimHandle (clip read-out)

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class Renderer;
class World;
class IGraphicsDevice;
namespace Resource    { class AssetManager; class AnimationClipSystem; }
namespace DX12Physics { class PhysicsSystem; }

namespace Editor
{
    namespace ChainPreview
    {
        // The stage's clock is FIXED-STEP by default. Chain physics clamps its dt to 1/30
        // twice (Simulate and PrepareSubsteps), so feeding one large dt to "catch up"
        // silently drops the excess: a slow frame runs at most kMaxCatchupSteps steps and
        // the rest of the time is thrown away.
        constexpr float         kFixedDt         = 1.f / 60.f;
        constexpr std::uint32_t kMaxCatchupSteps = 4;
    }

    // THE stage's one clock. Every editor frame Advance yields N fixed steps
    // (0..kMaxCatchupSteps; the excess is DROPPED, never banked) and the SAME N
    // advances the clip clock (m_clipT) AND the chain solver's preview lane
    // (SetChainPhysicsPreviewLane(set, stepDt, N)), so chain time and clip time cannot
    // drift apart at any editor rate. timeScale changes HOW MANY fixed steps a frame
    // runs, never the size of one, so a tuned spring stays the same spring. fixedDt 0
    // = "follow editor": one step of the real delta x timeScale, clamped to 1/30 (the
    // solver's own clamp, applied to the clip too; a clamped frame counts as dropped).
    struct StageClock
    {
        float         accumulator  = 0.f;
        float         fixedDt      = ChainPreview::kFixedDt;  // 0 = follow editor
        float         timeScale    = 1.f;
        bool          paused       = false;
        bool          stepOnce     = false;
        std::uint64_t droppedSteps = 0;     // catch-up overflow, for the tooltip
        // ---- harness pacing (set only by the -chainpreview_* setters) ----
        // pinSteps: the EXACT step count of successive frames, cycled
        // (-chainpreview_steps; a harness subject defaults to {1}, one fixed step per
        // editor frame) - realDt and timeScale are ignored. feedDt: synthetic REAL frame
        // times, cycled, fed through the real accumulator path in place of io.DeltaTime
        // (-chainpreview_realdt). Pause and Step win over both.
        std::vector<std::uint32_t> pinSteps;
        std::vector<float>         feedDt;
        std::size_t   pinCursor = 0, feedCursor = 0;
        // ---- totals for the "[CHAINPREVIEW] clock:" line (per stage session) ----
        std::uint64_t frames = 0, totalSteps = 0, zeroStepFrames = 0;
        std::uint64_t pausedFrames = 0, stepsWhilePaused = 0;
        std::uint32_t lastSteps = 0, maxStepsPerFrame = 0;
        double        stageTime = 0.0;      // the sum of every step's dt
        // Steps this frame (not yet counted - see Commit); *outStepDt receives the
        // size of each.
        std::uint32_t Advance(float realDt, float* outStepDt);
        // Counts the n steps of size stepDt the frame actually ran (the harness A/B
        // window may truncate Advance's n on its last frame).
        void Commit(std::uint32_t n, float stepDt);
        // A new timeline (Reset, an A/B window): nothing banked, the pacing patterns
        // restart at their first entry. Totals and settings are kept.
        void Rewind() { accumulator = 0.f; pinCursor = feedCursor = 0; stepOnce = false; }
        void ResetTotals()
        {
            frames = totalSteps = zeroStepFrames = pausedFrames = stepsWhilePaused = 0;
            lastSteps = maxStepsPerFrame = 0;
            stageTime = 0.0;
            droppedSteps = 0;
        }
    };

    // What ProcessPendingActions needs from EditorLayer, one struct rather than a
    // long argument list.
    struct ChainPreviewContext
    {
        Renderer*                      renderer    = nullptr;
        World*                         world       = nullptr;
        Resource::AssetManager*        assetMgr    = nullptr;
        Resource::AnimationClipSystem* animClipSys = nullptr;
        DX12Physics::PhysicsSystem*    physics     = nullptr;
        IGraphicsDevice*               gfx         = nullptr;
        // EditorLayer::GetPlayState() == Stopped. The stage exists ONLY in Edit mode.
        bool                           editing     = true;
        // The editor frame's real (unscaled) delta, fed to the StageClock.
        float                          realDt      = 0.f;
        // The overlay's bone source - EditorLayer::BuildBoneWorldMatrices (FK over the
        // post-chain LocalPose), so the overlay, the picker and the gizmo agree about
        // where a bone is. Empty = no overlay snapshot.
        ChainOverlay::BoneFn           buildBones;
        // Is ANY ImGui item active (a slider mid-drag)? A panel-undo step is
        // committed only once the edit is released.
        bool                           uiEditActive = false;
        // Apply -> Scene entity / All instances records ONE global undo entry (an
        // EditMacro of ValueEditActions). EditorLayer supplies the gate (Edit mode only -
        // Stop's AdoptState would revert the write anyway) and the push
        // (SyncedEditContext + PushEdit, which also marks the scene dirty); undoCount
        // feeds the apply read-out.
        bool                           undoEnabled = true;
        std::function<void(std::unique_ptr<Undo::IEditAction>)> pushEdit;
        std::function<std::size_t()>   undoCount;
        // The editor's current scene path - part of an in-place subject's stash
        // identity.
        std::string                    scenePath;
    };

    class ChainPhysicsPreviewWindow
    {
    public:
        struct AbPacing { std::vector<std::uint32_t> steps; std::vector<float> realDt; };

        // Apply's three destinations are NOT equivalent:
        //   Scene     - the chain carrier of one scene character: field-wise write +
        //               Invalidate + ONE global undo entry. A prefab-REF instance gets
        //               the physics material only (CopyMaterialInto: the scene would save
        //               a written rest pose per instance, a CPR line) and is saved as a
        //               reference, so the write does not persist for it (the UI says so).
        //   Prefab    - the subject's .ipfb (LoadPrefabDoc -> SetNodeComponentBlob ->
        //               SavePrefabDoc: .bak + header sizes patched). Persists.
        //   Instances - every scene instance of the subject's prefab, as Scene: SESSION
        //               ONLY - not saved; the .ipfb is what makes it stick.
        // Default: a prefab-sourced subject -> Prefab; an in-place one -> Scene.
        enum class ApplyDest : std::uint8_t { Scene, Prefab, Instances };

        // ---- layout ----
        // The panel's body is VIEW | splitter | PARAMETERS side by side, or stacked (view
        // above, parameters below). Auto stacks when the body is taller than wide
        // (aspect < 1.0; un-stacks above 1.25 - hysteresis, so a near-square window does
        // not flip every frame) or too narrow for both minimums side by side. The two
        // split ratios and the mode persist in imgui.ini ([ChainPhysicsPreview][Layout];
        // inert under -unattended, where io.IniFilename is null). The parameter column
        // scrolls horizontally as a safety net; its rows flow (EditorUI::Flow), read-outs
        // wrap, and the trees keep a label column (TreeLabelColumn).
        enum class Layout : std::uint8_t { Auto, Side, Stacked };

        enum SectionBit : std::uint32_t
        {
            kSecPreview = 1u, kSecSimulation = 2u, kSecScene = 4u, kSecParams = 8u,
            kSecAb = 16u, kSecShow = 32u, kSecTree = 64u, kSecAll = 127u
        };

        enum class AbPress : std::uint8_t { Swap, Copy, Promote, Off };

        enum class Controller : std::uint8_t
        {
            RestPose,           // the reference pose, no clip
            Clip,               // a picked .ianim on the panel's own clock
            // The stage plays the clip a scene entity plays, at that entity's time, every
            // frame. (1) The source is NOT pinned (a culled source is mirrored at its last
            // known state); (2) it keeps its own main-lane chains, so the two differ - that
            // is the point; (3) a destroyed source / a world clear falls back to Clip and
            // logs it.
            FollowSceneEntity,
        };
        // The root-motion driver's deterministic stress programs (synthesized, not
        // .ianim assets).
        enum class DriveProgram : std::uint8_t { None, YawSweep, TeleportHop };

        // Stage origin: far outside any sane scene, and still a float with ~0.5 mm of
        // precision - origin slot 0 of Editor/PreviewStage.h (the Skeleton Editor's stage
        // takes slot 1, 512 m below).
        static constexpr float kStageOriginY = StageOriginY(kStageSlotChain);

        // ---- display mode: what the picture shows ----
        // OverlayOnly, ChainOnly and CollidersOnly hide every STAGE mesh (the character +
        // the floor plane; the sky stays) from the preview picture by clearing
        // ViewBit::SecondaryView on the stage's VisibilityComponents in PPA - the stage
        // keeps animating, skinning and simulating, and the main viewport's bits are never
        // touched. MeshOnly draws no overlay primitive (ActiveOverlayMask() == 0, the
        // pose tool's pickable-bone dots too); ChainOnly and CollidersOnly draw only their
        // named overlay. The per-bit masks stay as the user set them. Both leaves the
        // spawn policy's bits as they are.
        enum class DisplayMode : std::uint8_t
        { Both = 0, OverlayOnly = 1, MeshOnly = 2, ChainOnly = 3, CollidersOnly = 4 };

        // ---- instance, open flag, Draw ----
        static ChainPhysicsPreviewWindow& Get();

        // The panel's open flag (ImGui::Begin's close box writes it; the menu goes
        // through RequestOpen). Owned here rather than by EditorLayer so EditorLayer.h
        // does not grow a member. Not persisted to editor_prefs.ini.
        bool* OpenFlag()       { return &m_open; }
        bool  IsOpen() const   { return m_open; }

        // UI half: no World / renderer mutation here (see the file header).
        // drawParamTree is EditorLayer::DrawChainParamTree, drawn on m_staged (panel
        // state), never on the live component. `playing` (the editor is not Stopped)
        // turns Parameters into the "LIVE" section drawn by drawLive - the World-touching
        // body is EditorLayer's; the panel only calls it.
        void Draw(Renderer* renderer, World* world, Entity editorSelection,
                  const ChainParamTreeFn& drawParamTree = {},
                  const ChainLiveEditFn& drawLive = {}, bool playing = false);

        // ---- staged parameters ----
        // m_staged when `e` is the stage's chain carrier and a staged copy exists, else
        // nullptr. UI half: the caller edits PANEL state; the live component is written
        // only by ProcessPendingActions' sync.
        ChainPhysicsComponent* EditStagedFor(Entity e);
        // The hooks every drawer of m_staged uses: entity = the stage carrier, invalidate
        // = raise the panel's structural flag (the sync Invalidates), markStructural =
        // true, pose = the panel's own rest-pose tool state, mainOverlayTarget = false.
        ChainParamHooks StagedHooks();
        // EditorLayer's shortcut handler (top of OnUIRender): true when the panel was
        // focused last frame - Ctrl+Z / Ctrl+Y then step the PANEL's history and the
        // global EditHistory is not touched.
        bool OwnsUndoShortcut() const;
        void RequestUndo() { ++m_pendingUndo; }
        void RequestRedo() { ++m_pendingRedo; }

        // GAME-half serial window. Every side effect of the panel lands here.
        void ProcessPendingActions(const ChainPreviewContext& ctx);

        // PlayModeSession's beforeSnapshot hook (Edit -> Play), BEFORE CloneState: tears the
        // stage and the camera down so the Play snapshot never contains them, releases the
        // view, and restores the process-global state the panel overrode. The subject is
        // kept: the next Edit-mode ProcessPendingActions after Stop rebuilds the stage.
        // Idempotent.
        void TearDownForPlay(Renderer* renderer, World* world);

        // EditorLayer::ReleasePreviewViews at process shutdown: emits the [PANELVIS]
        // chainpreview line under -panelvisibleverify, restores the globals, releases the
        // view. Does NOT destroy the stage entities: the World is about to be destroyed
        // wholesale and a destroy listener firing into a half-shut renderer buys nothing.
        // `world` (still whole then) is only read, for -chainpreview_dump. Idempotent.
        void ReleaseViewsAtShutdown(Renderer* renderer, World* world = nullptr);

        // ---- the Inspector's "Edit Chains..." + Play LIVE ----
        // The Inspector's "Edit Chains..." on `carrier` (the entity whose Chain Physics
        // row was clicked). Opens + focuses the panel. Edit mode: the next
        // ProcessPendingActions routes it through the guarded Use Selected - or only
        // focuses when that character already IS the subject (its root is the scene
        // target, or its prefab is the subject's) - and the clicked carrier is preferred
        // by Apply's carrier choice. Play (`playing`): it becomes the LIVE section's
        // target. Panel state only (the caller makes the handle).
        void RequestEditEntity(EntityHandle carrier, bool playing);
        // The LIVE section's inputs (EditorLayer's functor resolves the target in this
        // order: the Play-time Inspector request, the scene target, the selection, the
        // first scene instance of the subject prefab).
        EntityHandle LiveRequest() const        { return m_liveRequestH; }
        EntityHandle SceneTargetHandle() const  { return m_sceneTargetH; }
        EntityHandle SceneCarrierHandle() const { return m_sceneCarrierH; }
        const std::string& SubjectPath() const  { return m_subjectPath; }
        bool SubjectInPlace() const             { return m_subjectInPlace; }
        // "Keep these values": the LIVE component's value, taken by EditorLayer's
        // functor. materialOnly = the root is a prefab-REF instance (its rest pose is
        // saved per instance - the Apply rule). After Stop it is written into m_staged as
        // ONE panel-undo step, the kept character becoming the subject first when it is
        // not.
        void KeepLiveValues(const ChainPhysicsComponent& value, bool materialOnly,
                            EntityHandle root, EntityHandle carrier,
                            const std::string& name, const std::string& prefabPath);
        bool HasPlayKeep() const { return m_playKeep.valid; }
        void ForgetPlayKeep();
        // The LIVE section's read-out of the keep slot.
        const std::string& PlayKeepLabel() const { return m_playKeep.name; }
        // The instance ROOT of a scene entity (PrefabInstanceMember, else the Parent
        // chain) and the chain carrier under it - the Apply rules, shared with
        // EditorLayer's LIVE target resolution. SceneCarrierOf prefers the carrier an
        // Inspector "Edit Chains..." named.
        static Entity SceneRootOf(World& world, Entity e);
        Entity SceneCarrierOf(World& world, Entity root) const;
        // The carrier under a scene `root` that CORRESPONDS to the stage's carrier: same
        // name path below the root, else the same ordinal among the root's chain carriers
        // (DFS), else the first. Before any spawn: CarrierOf's rule. The staged values
        // come from the stage carrier, so this is the only carrier an Apply -> Scene /
        // All instances may write.
        Entity CounterpartCarrier(World& world, Entity root) const;
        // Chain carriers (Skeleton + ChainPhysics) under `root`, Children DFS order
        // (stageOk: include PreviewStageTag entities).
        static void ChainCarriersUnder(World& world, Entity root, bool stageOk,
                                       std::vector<Entity>& out);
        // "child/grandchild/carrier" below `root` (empty when e == root or not under
        // it).
        static std::string CarrierNamePath(World& world, Entity root, Entity e);
        // The stage's chain carrier (NullEntity when no stage).
        Entity StageChainEntity() const { return m_stageChain; }
        // ProcessPendingActions ticks since boot - the "editor frame" clock every
        // -chainpreview_*_at flag counts in (EditorLayer's stage-edit probe reads it too).
        std::uint64_t EditorFrame() const { return m_tick; }

        // ---- harness (-chainpreview_*, parsed in ChainPhysicsPreviewHarness.cpp) ----
        // -chainpreview_subject=<path>: open the panel on this subject.
        bool HarnessSetSubject(const std::string& path);
        // -chainpreview_size=<w>x<h>: pin the render size (bypasses the measurement AND
        // the debounce; under -hidden the content region is always <= 0). The picture is
        // then drawn 1:1 at the view's top-left. -chainpreview_size=auto is
        // HarnessSetSizeAuto.
        bool HarnessSetSize(std::uint32_t w, std::uint32_t h);
        // -chainpreview_frames=<n>: keep the stage for n editor frames after it
        // spawned, then close the panel through the ordinary close path.
        bool HarnessSetFrames(std::uint32_t n);
        // -chainpreview_open_at=<f>: open the panel at editor frame f instead of at boot.
        // Call BEFORE HarnessSetSubject. The isolation pack needs it: its "pre" census
        // must read a scene that has finished loading.
        bool HarnessSetOpenAt(std::uint64_t editorFrame);
        // -chainpreview_close_at=<f>: close the panel at editor frame f
        // (ProcessPendingActions ticks since boot).
        bool HarnessSetCloseAt(std::uint64_t editorFrame);
        // -chainpreview_strip_off: skip the STRIP list (the isolation pack's reverse
        // arm - Jolt / AI / shadow component counts must then move).
        void HarnessSetStripOff()   { m_harnessStripOff = true; }
        // -chainpreview_release_off (reverse arm): park the stage's skinned buffers on
        // the orphan list the way every other editor delete does.
        void HarnessSetReleaseOff() { m_harnessReleaseOff = true; }
        // -chainpreview_play_teardown_off (reverse arm): TearDownForPlay does nothing,
        // so the Play snapshot CONTAINS the stage.
        void HarnessSetPlayTeardownOff() { m_harnessPlayTeardownOff = true; }
        // -chainpreview_clip=<name|path.ianim>: put the controller in Clip mode on this
        // clip once the stage exists. A path is used as is; a name resolves to an alias
        // the subject authored, else a scanned .ianim stem (exact, then substring; the
        // shortest wins and the choice is logged). Unresolvable = exit 12.
        bool HarnessSetClip(const std::string& nameOrPath);
        // -chainpreview_time=<sec>: the playhead's position on the frame the clip is
        // assigned (and where a Reset / an A/B window puts it back). Under the harness the
        // stage clock then advances EXACTLY one fixed step per editor frame (unless
        // -chainpreview_steps / -chainpreview_realdt), so a picture's clip time is a
        // function of its frame number, not of load or wall time.
        bool HarnessSetTime(double seconds);
        // -chainpreview_pin_off (reverse arm): never hand the stage to
        // Renderer::AddAnimPinOwner - the stage is then anim-culled and drawn in its bind
        // pose.
        void HarnessSetPinOff() { m_harnessPinOff = true; }
        // -chainpreview_dt=<sec>: the StageClock's fixed step, i.e. the dt dropdown
        // (default 1/60). 0 is legal and means "the lane never runs" (chain_preview_sim's
        // reverse arm); the clip clock keeps its fixed step. Above 1/30 is refused (the
        // solver clamps dt to 1/30 twice, so the excess would vanish silently).
        bool HarnessSetDt(float seconds);
        // -chainpreview_dt=follow: the dt dropdown's "follow editor" (fixedDt 0) -
        // chain_preview_fixeddt's reverse arm.
        void HarnessSetDtFollow() { m_clock.fixedDt = 0.f; }
        // -chainpreview_timescale=<f>: the stage clock's time scale, 0.1..2.0
        // (default 1).
        bool HarnessSetTimeScale(float scale);
        // -chainpreview_steps=<n>[,<n>...]: the EXACT step count of successive editor
        // frames, cycled (each 0..kMaxCatchupSteps); StageClock::Advance ignores
        // io.DeltaTime and timeScale. Replaces the harness default {1} and any
        // -chainpreview_realdt.
        bool HarnessSetSteps(const std::vector<std::uint32_t>& steps);
        // -chainpreview_realdt=<sec>[,<sec>...]: synthetic REAL editor frame times,
        // cycled, fed through StageClock's real accumulator path (catch-up cap and drop
        // included) in place of io.DeltaTime - the harness way to vary editor frame
        // pacing.
        bool HarnessSetRealDt(const std::vector<float>& realDt);
        // -chainpreview_pause_at=<editor frame>: press Pause (the stage clock stops;
        // the chains HOLD their simulated pose).
        bool HarnessSetPauseAt(std::uint64_t editorFrame);
        // -chainpreview_step_at=<editor frame>: press Step once (one fixed step of
        // clip and chains, also while paused).
        bool HarnessSetStepAt(std::uint64_t editorFrame);

        // -chainpreview_ab=<seconds>[:steps=<list>|:realdt=<list>]...: back-to-back
        // windows of <seconds> of STAGE time, each opened by a full Reset (simulation +
        // playhead + sway phase + clock) once the clip is bound. Window a runs the run's
        // own pacing; every ":<pacing>" adds one more window (b, c, ... at most 3 more)
        // run with that pacing (none given = one window b with a's pacing). The particles
        // and the pose are captured at the end of each window and every later window is
        // compared with a ("[CHAINPREVIEW] ab compare b:" / "... c:").
        bool HarnessSetAb(double seconds, const std::vector<AbPacing>& extraWindows);
        // -chainpreview_hold_off (reverse arm): a step-less frame pushes dt 0 (the raw
        // clip pose) instead of HOLDING.
        void HarnessSetHoldOff() { m_harnessHoldOff = true; }
        // -chainpreview_sim=<0|1>: start with the simulation on (1, the default) or
        // held (0: the stage keeps the Edit-mode rest pose).
        void HarnessSetSim(bool on) { m_simulate = on; }
        // -chainpreview_reset_at=<editor frame>: reset the stage's chain simulation
        // once, through the same path a clip change takes.
        bool HarnessSetResetAt(std::uint64_t editorFrame);
        // -chainpreview_dump=<file.json>: write the stage's particle positions + pose
        // hashes when the panel closes (a relative path lands in the -artifacts
        // directory).
        bool HarnessSetDump(const std::string& path);
        // -chainpreview_param=<key>=<value>[,<key>=<value>...]: an edit of the stage's
        // chain parameters on the first frame a stage exists - the edit a tree slider
        // makes: into m_staged as one panel-undo step, synced to the live component in
        // the same serial window. <key> is a REFLECT field name of ChainPhysicsComponent
        // (damping, iterations, windDir as x:y:z, ...), restPoseWeight / restPoseEnabled,
        // or g<idx>_<ChainGroupDef member> for one group (g0_ovrDamping, g2_collide,
        // ...). Syntax and the key are checked here (false = ENGINE_CHECK + ignored); a
        // group index past the stage's groups is refused when applied. Structural group
        // keys (enabled / excludeSubtree / ringGroup / ringIndex / pairGroupId)
        // re-initialise the chain. Logs "[CHAINPREVIEW] param apply: key= value= live=
        // rebuild=".
        bool HarnessAddParam(const std::string& key, const std::string& value);
        // -chainpreview_param=<key>=<value>@<editor frame>: the same edit at that
        // editor frame (0 = the first frame with a stage).
        bool HarnessAddParamAt(const std::string& key, const std::string& value,
                               std::uint64_t editorFrame);
        // -chainpreview_undo_at / -chainpreview_redo_at=<f>[,<f>...]: press the
        // panel's Ctrl+Z / Ctrl+Y at those editor frames.
        bool HarnessSetUndoAt(const std::vector<std::uint64_t>& frames);
        bool HarnessSetRedoAt(const std::vector<std::uint64_t>& frames);
        // -chainpreview_fullreset_at=<f>[,<f>...]: press the Simulation section's Reset
        // (chains + playhead + sway phase + clock) at those editor frames. Two resets a
        // fixed number of steps apart replay bit-identical windows, so a parameter
        // changed at the second one is the ONLY difference between two captures taken at
        // the same offset into each window (chain_preview_param).
        bool HarnessSetFullResetAt(const std::vector<std::uint64_t>& frames);
        // -chainpreview_overlay=<hex>[@<f>][,<hex>@<f>...]: the overlay show mask (both
        // the edit and the sim mask) from editor frame f on (no @ = from the start).
        bool HarnessSetOverlay(const std::vector<std::pair<std::uint32_t, std::uint64_t>>& schedule);
        // -chainpreview_overlay_shot=<file.png>@<f>[,...]: write the overlay LAYER (the
        // panel's overlay primitives, software-rasterized from a private ImDrawList at
        // the preview output size) at editor frame f. A relative path lands in the
        // -artifacts directory.
        bool HarnessAddOverlayShot(const std::string& path, std::uint64_t editorFrame);
        // Reverse arm: the overlay's view looks AWAY from the subject - every bone
        // projects behind the camera (offscreenBones = bones).
        void HarnessSetOverlayBadProj() { m_harnessOverlayBadProj = true; }
        // Reverse arm: the one writer copies a STRUCTURAL change into the live component
        // but does NOT Invalidate - the topology keeps running on the old groups (what
        // "hot-applying" a structural edit would do).
        void HarnessSetReinitOff() { m_harnessReinitOff = true; }
        // -chainpreview_carry_off (reverse arm): do not re-apply the captured stage
        // parameters (m_staged) when the same subject respawns - edits then vanish at
        // Play.
        void HarnessSetCarryOff() { m_harnessCarryOff = true; }
        // -chainpreview_lod_keep (reverse arm): do NOT force chain LOD off while the
        // panel is open - the stage, ~4 km from the main camera, then runs LOD tier 2
        // (1 iteration, no collision).
        void HarnessSetLodKeep() { m_harnessLodKeep = true; }

        // ---- harness: scene settings, forces, driver ----
        // -chainpreview_scale=<f>|auto: the stage root's (uniform) scale. auto (<= 0) =
        // the default rule: the scene's first instance of the same prefab, else the
        // prefab's authored root scale.
        bool HarnessSetScale(float scale);
        // -chainpreview_floor=<0|1|collide>[:<gap>]: mode 0 off, 1 the visual floor
        // plane, 2 (collide) plane + the solver's capsule proxy.
        bool HarnessSetFloor(int mode, float gap);
        // -chainpreview_gravity=<x,y,z>[:<scale>][@<f>] (repeatable): the scene
        // gravity's "down" vector x scale from editor frame f (0 = the first frame with a
        // stage). (0,-1,0) x 1 = the authored gravity.
        bool HarnessAddGravity(const float down[3], float scale, std::uint64_t at);
        // -chainpreview_nogravity[=@<f>]: the No Gravity diagnostic.
        void HarnessSetNoGravityAt(std::uint64_t at);
        // -chainpreview_wind=<x,y,z>:<strength>[:<freq>[:<turb>]][@<f>] (repeatable):
        // the staged wind (and every group that overrides wind). freq / turb < 0 = keep.
        bool HarnessAddWind(const float dir[3], float strength, float freq,
                            float turb, std::uint64_t at);
        // -chainpreview_drive=<move>,<rotDegPerSec>[@<f>] (repeatable): the
        // root-motion driver's speeds.
        bool HarnessAddDrive(float move, float rotDegPerSec, std::uint64_t at);
        // -chainpreview_drive_off (reverse arm): the driver writes nothing - no
        // speed, no teleport, no stress program reaches the stage root.
        void HarnessSetDriveOff() { m_harnessDriveOff = true; }
        // -chainpreview_teleport_at=<f>[:<dist>[:<angleDeg>[:reset]]][,...]: one-shot
        // teleports of the stage root (dist <= 0 = the auto distance; ":reset" = this one
        // resets the chains).
        bool HarnessAddTeleportAt(std::uint64_t at, float dist, float angleDeg,
                                  bool reset = false);
        // -chainpreview_scale_at=<f>:<scale>[,...]: press the Scale control.
        bool HarnessAddScaleAt(std::uint64_t at, float scale);
        // -chainpreview_teleport_reset=<0|1>: the "Reset chains on teleport"
        // checkbox (default 0: the chains integrate across the hop).
        void HarnessSetTeleportReset(bool on) { m_teleportReset = on; }
        // -chainpreview_stress=<yawsweep|teleport|idle|walkrun>: pick one of the four
        // stress motions the way the picker's top rows do.
        bool HarnessSetStress(const std::string& which);
        // -chainpreview_shot=<file.png>@<f>[,...]: capture the panel's OWN view output
        // at editor frame f (any number of moments in one run; -preview_screenshot has
        // two slots).
        bool HarnessAddShot(const std::string& path, std::uint64_t at);
        // -chainpreview_follow=<entity name>: FollowSceneEntity on the named scene
        // entity. -chainpreview_follow_kill_at=<f> (harness only) destroys that source at
        // editor frame f - the fallback's trigger.
        bool HarnessSetFollow(const std::string& entityName);
        bool HarnessSetFollowKillAt(std::uint64_t at);

        // ---- Apply / Revert / presets (ChainPhysicsPreviewApply.cpp) ----
        static const char* ApplyDestName(ApplyDest d);
        // -chainpreview_apply=<scene|prefab|instances>[:<arg>][@<f>][,...]: press Apply
        // at editor frame f (0 = the first frame with a stage, after the harness params).
        // arg: prefab = the .ipfb to write (a relative path lands in -artifacts; default
        // the subject), scene = a scene entity NAME (default: Use Selected's source / the
        // selection), instances = the prefab path whose instances to write (default the
        // subject). A rig mismatch is REFUSED (the UI asks; the harness never confirms).
        bool HarnessAddApply(ApplyDest dest, const std::string& arg, std::uint64_t at);
        // -chainpreview_apply_seed=<src.ipfb>: copy src over every prefab Apply
        // target before the first write - the pack's "copy of a fixture".
        bool HarnessSetApplySeed(const std::string& src);
        // -chainpreview_preset=<name|file.json>[:g<idx>][@<f>][,...]: load a preset into
        // m_staged at editor frame f (one panel-undo step); g<idx> = that group only (a
        // Cloth group = its piece).
        bool HarnessAddPreset(const std::string& nameOrPath, int group, std::uint64_t at);
        // -chainpreview_preset_clamp_off (reverse arm): presets load UNclamped.
        void HarnessSetPresetClampOff() { m_harnessPresetClampOff = true; }
        // -chainpreview_revert_at=<f>[,...]: press Revert.
        bool HarnessSetRevertAt(const std::vector<std::uint64_t>& frames);
        // -chainpreview_sync_wholesale (reverse arm): the one writer copies m_staged into
        // the live component WHOLE (`*live = m_staged`, BoneRestPose's runtime cache
        // included) every frame.
        void HarnessSetSyncWholesale() { m_harnessSyncWholesale = true; }
        // m_staged differs from the subject's load-time value (the dirty dot).
        bool IsStagedDirty() const;
        // Unapplied work that a subject change or a close would lose: the live stage's
        // dirty staged value, or, with NO stage (Play suspends it), the carry's captured
        // edits, or a "Keep these values" slot not yet staged.
        bool HasUnappliedWork() const;

        // ---- harness: prefab-ref Apply, the scene target, the dirty guard ----
        // -chainpreview_apply_restpose (reverse arm): Apply writes the WHOLE authored
        // block (rest pose included) onto prefab-ref instances, whose rest pose the scene
        // then saves per instance (CPR).
        void HarnessSetApplyRestPose() { m_harnessApplyRestPose = true; }
        // -chainpreview_scene_save=<file.iscene>@<f>: Resource::SaveScene of the World
        // into -artifacts at editor frame f, then the CPR lines of the written file are
        // counted ("[CHAINPREVIEW] scene save:").
        bool HarnessAddSceneSave(const std::string& path, std::uint64_t at);
        // -chainpreview_scene_target_drop (reverse arm): an epoch change drops the
        // Use Selected target.
        void HarnessSetSceneTargetDrop() { m_harnessSceneTargetDrop = true; }
        // -chainpreview_request=<kind>[:<arg>]@<f>[,...]: press a subject / panel
        // control at editor frame f THROUGH the UI's own path (the dirty confirmation
        // included). kinds: load:<path>, reload, close (the Window menu),
        // useselected:<name>[#k], select:<name>[#k] (the EDITOR's selection), worldload
        // (reload the current scene), open.
        bool HarnessAddRequest(const std::string& kind, const std::string& arg, std::uint64_t at);
        // -chainpreview_confirm=<keep|discard|apply>[,...]: the answers the harness
        // gives, in order, to each dirty confirmation that opens.
        bool HarnessSetConfirm(const std::vector<int>& answers);
        // -chainpreview_confirm_off (reverse arm): no confirmation (a subject change
        // discards silently) and no world-load stash.
        void HarnessSetConfirmOff() { m_harnessConfirmOff = true; }
        // -chainpreview_stash_pathkey (reverse arm): every stash keyed by the subject
        // PATH - all in-place characters share the clone's path, so one character's stash
        // is offered to (and re-applied onto) another.
        void HarnessSetStashPathKey() { m_harnessStashPathKey = true; }
        // -chainpreview_reinit_bare (reverse arm): a structural A or B sync
        // re-initialises with a BARE Invalidate on that entity alone (InitEntity then
        // bakes from the swung LocalPose, and the other side is not restarted).
        void HarnessSetReinitBare() { m_harnessReinitBare = true; }
        // -chainpreview_reapply=<0|1>: answer the "Re-apply staged params" offer (1)
        // or Forget it (0) once it shows.
        void HarnessSetReapply(int answer) { m_harnessReapply = answer; }
        // Harness: an editor-selection request (select:) that EditorLayer applies.
        Entity TakeHarnessSelect() { const Entity e = m_harnessSelectOut; m_harnessSelectOut = NullEntity; return e; }
        // Harness "worldload": EditorLayer reloads its current scene.
        bool TakeWorldReloadRequest() { const bool r = m_harnessWorldReload; m_harnessWorldReload = false; return r; }
        // The Window menu's toggle. Opening opens; closing with unapplied staged
        // edits asks first (the title-bar X's confirmation).
        void RequestOpen(bool open);
        // Probe read-outs (the panel's staged history counters).
        std::uint64_t PanelUndoOps() const { return m_undoOps; }
        std::uint64_t PanelCommits() const { return m_commits; }
        // -chainpreview_panel_tree_at's body: the panel's OWN tree call (the one
        // DrawStagedParams makes: drawParamTree on m_staged with StagedHooks) inside a
        // probe window; `focused` records the panel's own undo-scope focus frame exactly
        // as Draw does. False when there is no staged copy / skeleton.
        bool HarnessDrawStagedTree(Renderer* renderer, const ChainParamTreeFn& drawParamTree,
                                   bool focused);
        // -chainpreview_tree_live (reverse arm): the panel-tree probe edits the stage's
        // LIVE component with a direct Invalidate instead of the panel's tree on
        // m_staged.
        static bool TreeLiveArm();
        // -chainpreview_edit_naive (reverse arm): "Edit Chains..." mapped straight onto
        // Use Selected - no "already the subject" short-circuit (a re-clone + respawn, or
        // a confirmation, for a no-op) and no clicked-carrier preference.
        static bool EditNaiveArm();
        // -chainpreview_play_keep_off (reverse arm): "Keep these values" is taken
        // but never written into m_staged after Stop.
        void HarnessSetPlayKeepOff() { m_harnessPlayKeepOff = true; }
        // The -chainpreview_param grammar, for EditorLayer's harness probes (the Play
        // LIVE probe edits a scene component with it).
        static bool HarnessParamValid(const std::string& key, const std::string& value)
        { return ParamKeyValid(key, value); }
        static bool HarnessApplyParam(ChainPhysicsComponent& c, const std::string& key,
                                      const std::string& value)
        { return ApplyParamKey(c, key, value); }

        // ---- layout ----
        // EditorLayer init, after the ImGui context exists and BEFORE the first
        // NewFrame (imgui.ini is read there): the settings handler.
        static void RegisterSettingsHandler();
        // -chainpreview_window=<w>x<h>[@<editor frame>][,...]: force the panel's window
        // size (and a fixed position) from that editor frame on, and log
        // "[CHAINPREVIEW] layout:" a settle period later. While any step is given the
        // render-size debounce is bypassed (the commit frame must not depend on wall
        // time).
        bool HarnessAddWindowStep(std::uint32_t w, std::uint32_t h, std::uint64_t at);
        // The same step with an explicit position, <w>x<h>+<x>+<y>: the window's top-left
        // at (x, y) px from the main viewport's work area (default +24+36). The gizmo
        // layering pack floats the panel over the main viewport's gizmo.
        bool HarnessAddWindowStepAt(std::uint32_t w, std::uint32_t h, float x, float y,
                                    std::uint64_t at);
        // -chainpreview_size=auto: no pin - the layout sizes the render (only
        // meaningful in a VISIBLE run; under -hidden the region is <= 0).
        void HarnessSetSizeAuto() { m_harnessSizeAuto = true; m_harnessW = m_harnessH = 0; }
        bool HarnessSizeAuto() const { return m_harnessSizeAuto; }
        // -chainpreview_layout_fixed (the layout pack's reverse arm): a FIXED 300 px
        // parameter column, the picture sized from the width alone at 16:9 and drawn
        // 1:1, the tree at its default item widths.
        void HarnessSetLayoutFixed() { m_harnessLayoutFixed = true; }

        // -chainpreview_sections=<name>[+<name>...]@<editor frame>[,...]: from that frame
        // the panel's sections are forced to EXACTLY that set - preview, simulation,
        // scene, params, ab, show, tree (every header and group node of the parameter
        // tree; the exclude bone trees stay closed), all - the rest closed. The layout
        // pack's "expanded" steps.
        bool HarnessAddSections(std::uint32_t mask, std::uint64_t at);
        // -chainpreview_params_scroll=<0..1>@<editor frame>[,...]: scroll the parameter
        // column to that fraction of its range from that frame (so a -ui_screenshot can
        // show the lower part of an expanded tree).
        bool HarnessAddParamsScroll(float frac, std::uint64_t at);
        // -chainpreview_split=<ratio|reset>@<f>[,...]: at editor frame f the splitter is
        // set as a DRAG to ratio x the body would set it - through the same clamp (view
        // >= 240 / 200 px, params >= 300 / 180 px) - on whichever axis the current layout
        // splits; `reset` is the double-click (the default ratio). ratio in [0, 1].
        bool HarnessAddSplit(float ratio, std::uint64_t at);

        // ---- solver-state stats ----
        // -chainpreview_stats=<0|1>[@<f>][,...]: the stats overlay from editor frame f
        // (no @ = from the start). Interactive default ON; a harness subject defaults it
        // OFF (the box carries live timings, which would make the overlay layer's
        // pictures non-deterministic).
        bool HarnessSetStats(const std::vector<std::pair<bool, std::uint64_t>>& schedule);

        // ---- A/B ghost, per-group solo, poke (ChainPhysicsPreviewCompare.cpp) ----
        // TODO(ChainPhysics_Preview_Viewport_Plan Q8): the FK <-> sim blend.
        // -chainpreview_abparam=<key>=<value>[@<f>][,...] (repeatable, the
        // -chainpreview_param keys): turn A/B on (at the earliest entry's editor frame;
        // 0 = the first frame with a stage) and set that field of B's staged copy.
        // (-chainpreview_ab is the replay-window flag.)
        bool HarnessAddAbParam(const std::string& key, const std::string& value,
                               std::uint64_t editorFrame);
        // -chainpreview_ab_same (the A/B pack's reverse arm): B is forced equal to A
        // at every edit / button - the two solves must then be identical.
        void HarnessSetAbSame() { m_harnessAbSame = true; }

        // -chainpreview_ab_press=<swap|copy|promote|off>@<f>[,...]: press an A/B
        // button at editor frame f.
        bool HarnessAddAbPress(AbPress what, std::uint64_t editorFrame);
        // -chainpreview_solo=<group name|#idx|none>[@<f>][,...]: solo one group (the rest
        // FROZEN AT REST, ChainPhysicsSystem::SetSoloGroup) from editor frame f; 'none'
        // clears. The name is percent-decoded.
        bool HarnessAddSolo(const std::string& group, std::uint64_t editorFrame);
        // -chainpreview_solo_off (reverse arm): the solo is logged but never handed
        // to the solver.
        void HarnessSetSoloOff() { m_harnessSoloOff = true; }
        // -chainpreview_poke=<f>,<particle index|bone name>,<strength>[,<blend s>]
        // (repeatable): at editor frame f, poke the particle through the UI's own path -
        // its projection in the preview becomes the "mouse", the panel's ray picks a
        // particle, and an impulse of <strength> SKELETON units / s along the preview
        // camera's screen-right goes to ChainPhysicsSystem::ApplyExternalImpulse (blend
        // default 0.1 s).
        bool HarnessAddPoke(std::uint64_t editorFrame, const std::string& target,
                            float strength, float blendSeconds);
        // -chainpreview_poke_probe=<after>,<settle>: the probe's two captures, in
        // editor frames after the poke (default 3, 180).
        bool HarnessSetPokeProbe(std::uint32_t afterFrames, std::uint32_t settleFrames);

        // ---- in-picture rest-pose gizmo (ChainPhysicsPreviewPose.cpp) ----
        // The stage tree's "Pose bones in preview" arms it (m_stagePose): pickable chain
        // bones are drawn over the picture, a click picks one, and an ImGuizmo ROTATE ring
        // on it edits the STAGED authored rest pose (never the live component). One drag
        // = one panel-undo step + one structural re-init (a gesture flag holds the commit
        // and the sync; ImGuizmo never sets an ImGui ActiveId) and the simulation is
        // auto-paused for the drag.
        // -chainpreview_pose_gizmo=<bone|#idx>:<x|y|z|view>:<deg>:<frames>@<f>[,...]: at
        // editor frame f select that stage bone (pose mode on) and drive a <frames>-frame
        // gesture turning it <deg> degrees about a WORLD axis ('view' = the preview
        // camera's forward) through the SAME delta path the ring uses (ApplyPoseDelta ->
        // ChainRestPose::ApplyWorldDelta), then log '[CHAINPREVIEW] pose gizmo <n>:'. The
        // bone is percent-decoded.
        bool HarnessAddPoseGesture(const std::string& bone, int axis, float deg,
                                   std::uint32_t frames, std::uint64_t editorFrame);
        // -chainpreview_pose_local_order (reverse arm): the delta applied in the
        // bone's LOCAL order (ChainRestPose::DeltaOrder::BoneLocal).
        void HarnessSetPoseLocalOrder() { m_harnessPoseLocalOrder = true; }
        // -chainpreview_pose_gesture_off (reverse arm): no gesture flag - every drag frame
        // commits a panel-undo step and re-initialises the chains, and nothing is
        // auto-paused (the naive ImGuizmo-in-a-panel shape).
        void HarnessSetPoseGestureOff() { m_harnessPoseGestureOff = true; }
        // Harness: FindNamed ("<name>[#k]", a parentless scene entity) for
        // EditorLayer's -chainpreview_main_pose probe.
        Entity HarnessFindNamed(World& world, const std::string& nameIdx) const
        { return FindNamed(world, nameIdx); }

        // ---- display mode: what the picture shows ----
        static const char* DisplayName(DisplayMode m);
        // -chainpreview_display=<both|overlay|mesh|chain|colliders>[@<editor frame>][,...]
        bool HarnessAddDisplay(DisplayMode m, std::uint64_t at);
        // -chainpreview_display_off (reverse arm): the mode is recorded and reported but
        // never applied - no stage mesh is hidden and the overlay keeps its masks in every
        // mode.
        void HarnessSetDisplayOff() { m_harnessDisplayOff = true; }

    private:
        struct Census
        {
            std::uint32_t joltBodies = 0, joltComps = 0, aiAgents = 0, aiComps = 0;
            std::uint32_t shadowSet = 0, shadowRoots = 0, locomotion = 0, scripts = 0;
            std::uint32_t skinnedOwned = 0, skinnedOrphans = 0, drawPackets = 0;
            std::uint32_t sharedSources = 0;
            std::uint32_t bvhLeaves = 0, skeletons = 0, entities = 0, cameras = 0;
            double        vramMB = 0.0;
            bool          valid = false;
        };

        static constexpr float kMinViewW  = 240.f, kMinParamW = 300.f;
        static constexpr float kMinViewH  = 200.f, kMinParamH = 180.f;
        static constexpr float kSplitBar  = 6.f;
        static constexpr float kDefSplitSide = 0.62f, kDefSplitStack = 0.55f;

        struct LayoutReport
        {
            bool   valid = false, stacked = false, fixed = false;
            float  panelW = 0.f, panelH = 0.f, split = 0.f;
            float  viewX = 0.f, viewY = 0.f, viewW = 0.f, viewH = 0.f;
            float  imgW = 0.f, imgH = 0.f, scale = 1.f;
            float  paramsW = 0.f, paramsH = 0.f;
            float  paramsOverflow = 0.f, viewOverflow = 0.f;
            bool   paramsHScroll = false, transportVisible = false, transportDrawn = false;
            float  toolbarOverflow = 0.f;
            // The scrub ruler's fit: the clip's pixel length over the lanes' width
            // (1 = exactly fitted).
            bool   rulerDrawn = false;
            float  rulerFill  = 0.f, rulerLanesW = 0.f;
            float  secOverflow[6] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };   // params, per section
            std::uint32_t renderW = 0, renderH = 0;
            int    drawFrame = 0;
        };

        struct WindowStep
        {
            std::uint32_t w = 0, h = 0; std::uint64_t at = 0; bool reported = false;
            float x = 24.f, y = 36.f;   // top-left, px from the main viewport's WorkPos
        };

        struct SplitStep { float ratio = -1.f; std::uint64_t at = 0; bool done = false; };   // -1 = reset

        static constexpr std::uint64_t kLayoutSettle = 40;   // editor frames after a step

        struct UndoEntry { ChainPhysicsComponent value; };

        static constexpr std::size_t kUndoDepth = 64;

        struct OverlayShot { std::string path; std::uint64_t at = 0; bool done = false; };

        enum class PushMode : std::uint8_t { None, Step, Hold, Off };
        struct PushRecord { PushMode mode = PushMode::None; double clipT = 0.0; std::uint64_t resetSerial = 0; };

        enum class AbPhase : std::uint8_t { Off, Wait, Run, Capture, Done };
        struct AbWindow
        {
            std::uint64_t steps = 0, frames = 0, zeroFrames = 0, dropped0 = 0, dropped = 0;
            std::uint32_t maxSteps = 0;
            double        stageTime = 0.0, clipT = 0.0;
            std::vector<DirectX::XMFLOAT3> particles;
            std::uint32_t particleHash = 0, poseHash = 0;
            float         maxOffset = 0.f;
        };

        struct AuthoredClip { std::string name, path, source; };

        // ---- harness: scene events ----
        struct SceneEvent
        {
            enum Kind : std::uint8_t { Gravity, NoGravity, Wind, Drive, Teleport, FollowKill, Scale } kind = Gravity;
            std::uint64_t at = 0;
            float v[6] = {};
            bool done = false;
        };

        struct Shot { std::string path; std::uint64_t at = 0; bool done = false; };

        struct ApplyRequest { ApplyDest dest = ApplyDest::Prefab; std::string arg; bool confirmed = false; bool valid = false; };

        struct HarnessApply { ApplyDest dest = ApplyDest::Prefab; std::string arg; std::uint64_t at = 0; bool done = false; };

        struct HarnessPreset { std::string name; int group = -1; std::uint64_t at = 0; bool done = false; };

        static constexpr std::uint64_t kSyncSettleFrames = 30;

        struct SceneSave { std::string path; std::uint64_t at = 0; bool done = false; };

        enum class GuardKind : std::uint8_t { None, Load, Drop, UseSelected, Reload, Close };

        // fromKeep = the keep slot's own subject switch (the slot is the work it serves,
        // so it does not count as work it would lose).
        struct GuardedRequest { GuardKind kind = GuardKind::None; std::string path; Entity selected = NullEntity;
                                bool fromKeep = false; };

        struct StagedStash { ChainPhysicsComponent value; std::string why; std::string label; };

        struct HarnessRequest { std::string kind, arg; std::uint64_t at = 0; bool done = false; };

        struct CarrierPref { std::string path; int ordinal = -1; };

        struct PlayKeep
        {
            bool                  valid = false;
            ChainPhysicsComponent value;
            bool                  materialOnly = false;
            EntityHandle          root = NullEntityHandle, carrier = NullEntityHandle;
            std::string           name, prefabPath;
            bool                  switchRequested = false;   // the guarded Use Selected went out
            std::uint64_t         takenAt = 0;
        };

        struct StageStats
        {
            std::uint32_t chains = 0, strands = 0, particles = 0, capsules = 0, links = 0;
            std::uint32_t substeps = 0, steps = 0, tier0 = 0, tier1 = 0, tier2 = 0;
            std::uint32_t interpenetrating = 0, interPairs = 0, limitOnCone = 0, limitBeyond = 0;
            std::uint32_t skippedCollideGroups = 0, islands = 0, springs = 0, iterations = 0;
            std::uint32_t lodTier = 0, valid = 0;
            // With A/B on, the lane solves TWO entities: substeps / *Ms stay LANE totals
            // (labelled "A+B"); tier0-2 and iterations are A's own (snapshot lodTier, A's job
            // iterations - LOD is forced off on the stage).
            std::uint32_t abOn = 0, laneEntities = 0;
            float serialMs = 0.f, parallelMs = 0.f, cpuMs = 0.f;
        };

        struct AbBudget
        {
            std::uint32_t instances = 0, skinJobs = 0, poseBones = 0, verts = 0;
            std::uint32_t entities = 0, previewChainEntities = 0, particles = 0;
            float         solveMs = 0.f;
            bool          valid = false;
        };

        struct AbDivergence
        {
            std::uint32_t diverged = 0, compared = 0;
            float         maxDelta = 0.f, meanDelta = 0.f;
            bool          valid = false;
        };

        static constexpr float     kAbDivergeEps = 1e-4f;       // skeleton units
        struct HarnessAbParam { std::string key, value; std::uint64_t at = 0; bool applied = false; };

        struct HarnessAbPress { AbPress what = AbPress::Swap; std::uint64_t at = 0; bool done = false; };

        struct HarnessSolo { std::string group; std::uint64_t at = 0; bool done = false; };

        struct PendingPoke
        {
            bool          valid = false;
            float         u = 0.f, v = 0.f;           // press, normalised over the output
            float         du = 0.f, dv = 0.f;         // drag, normalised
            bool          harness = false;
            std::uint32_t target = ~0u;               // harness: the particle it aimed at
            float         strength = 0.f, blend = 0.f;
        };

        struct HarnessPoke { std::uint64_t at = 0; std::string target; float strength = 0.f; float blend = 0.1f; bool done = false; bool preDone = false; };

        struct PokeProbe
        {
            bool          active = false, afterDone = false, settleDone = false;
            std::uint64_t at = 0;
            std::uint32_t particle = 0, n = 0;
            std::vector<DirectX::XMFLOAT3> before, pre;
            float         preDrift = -1.f;
        };

        // ---- in-picture rest-pose gizmo ----
        struct PoseSnap
        {
            bool                           valid = false;
            std::uint32_t                  skelAsset = 0xFFFFFFFFu;
            std::uint32_t                  bone = ~0u;          // m_stagePose.poseBone it was taken for
            std::string                    boneName;
            DirectX::XMFLOAT4X4            boneWorld{}, parentWorld{};
            DirectX::XMFLOAT4              seedRot{ 0.f, 0.f, 0.f, 1.f };
            std::vector<std::uint32_t>     pickBone;
            std::vector<DirectX::XMFLOAT3> pickPos;
        };

        struct HarnessPoseGesture
        {
            std::string   bone, name;       // the spec; the resolved bone's name
            int           axis = 1;             // 0 x, 1 y, 2 z, 3 view
            float         deg = 0.f;
            std::uint32_t frames = 1;
            std::uint64_t at = 0;
            int           phase = 0;            // 0 wait, 1 select, 2 drag, 3 report, 4 done
            std::uint32_t boneIdx = ~0u, k = 0, refused = 0;
            std::uint64_t waitFrom = 0, reportAt = 0;
            std::uint64_t commits0 = 0, rebuilds0 = 0, submits0 = 0;
            std::size_t   entries0 = 0;
            bool          created = false, pausedBefore = false, simulating = false;
            DirectX::XMFLOAT4   q0{ 0.f, 0.f, 0.f, 1.f };
            DirectX::XMFLOAT4X4 parent{};
            DirectX::XMFLOAT3   axisW{ 0.f, 1.f, 0.f };
        };

        // ---- harness ----
        // `at` = the editor frame (0 = the first frame with a stage).
        struct HarnessParam { std::string key, value; std::uint64_t at = 0; bool applied = false; };

        // ---- instance ----
        ChainPhysicsPreviewWindow() = default;

        // ---- the one teardown every close path takes ----
        void TearDown(Renderer* renderer, World* world, const char* reason,
                      bool destroyEntities);
        void DestroyStage(Renderer* renderer, World& world);
        // Copy the live stage component's AUTHORED fields into m_staged (the
        // Play-boundary saved value) while the stage is alive.
        void CaptureStaged(World& world, const char* why);
        // -chainpreview_param entries due this frame, into m_staged.
        void ApplyHarnessParams(const ChainPreviewContext& ctx);
        // Drop stage entities whose HANDLE died outside the panel (Lua console, an
        // Internal-toggle delete) from every set; tear the stage down - destroying only
        // still-valid handles - if the root died.
        void ValidateStageHandles(const ChainPreviewContext& ctx);
        void RestoreGlobals(Renderer* renderer, const char* reason);
        bool SpawnStage(const ChainPreviewContext& ctx);
        void EnsureCamera(World& world);
        void UpdateCamera(World& world, Renderer& renderer);
        void NegotiateLaneSize(Renderer& renderer);
        Census TakeCensus(const ChainPreviewContext& ctx) const;
        void   LogCensus(const char* phase, const Census& c) const;

        // ---- animation transport ----
        void DrawControllerUi();
        void DrawTransport();
        void ScanClips();
        void SelectClip(const ChainPreviewContext& ctx, const std::string& path,
                        const std::string& sourceClip, const char* why);
        bool ResolveHarnessClip(const ChainPreviewContext& ctx);
        void UpdateAnimation(const ChainPreviewContext& ctx);
        void RefreshClipInfo(const ChainPreviewContext& ctx);
        void ReleaseClipHandle(Resource::AnimationClipSystem* acs);
        void RemovePreviewAlias(const ChainPreviewContext& ctx);
        void LogAnimSummary(const char* when) const;
        // A clip change, a scrub (while m_resetOnScrub), a step and a jump reset the
        // chain simulation. Called from the transport inside ProcessPendingActions; it
        // only LATCHES the reset - UpdateSimulation applies it later in the same serial
        // window.
        void OnPlayheadDiscontinuity(const char* why);

        // ---- the preview lane ----
        // Reset requests, then the per-frame SetChainPhysicsPreviewLane push.
        // ProcessPendingActions only, right after UpdateAnimation.
        void UpdateSimulation(const ChainPreviewContext& ctx);
        // ResetPreviewEntity on every stage chain carrier.
        void ResetSimulation(Renderer& renderer, World& world, const char* why);
        void WriteDump(Renderer& renderer, World& world, const char* when);

        // ---- the one clock + the simulation transport ----
        // Transport requests (Draw's buttons, the harness hooks), the A/B window
        // boundaries and the hold check. PPA only, before the clock.
        void TickSimTransport(const ChainPreviewContext& ctx);
        // StageClock::Advance -> m_frameSteps / m_frameStepDt, the one N that
        // UpdateAnimation (clip) and UpdateSimulation (lane) both consume.
        void TickStageClock(const ChainPreviewContext& ctx);
        // Reset Sim: the stage's chains + the playhead + the preview sway phase + the
        // clock's banked time, in one go.
        void FullReset(const ChainPreviewContext& ctx, const char* why);
        void DrawSimulationUi();
        void LogTransport(const char* what) const;
        void LogClock(const char* when) const;
        // Hash of the preview chain carriers' LocalPose (post-chain, i.e. what
        // the last frame left) - the harness hold check.
        std::uint32_t HashStagePose(Renderer& renderer, World& world) const;
        // harness A/B windows
        void StartAbWindow(const ChainPreviewContext& ctx, std::size_t which);
        void CaptureAbWindow(const ChainPreviewContext& ctx, std::size_t which);
        void LogAbCompare(std::size_t which) const;

        // ---- the one writer + panel undo + the overlay ----
        // Commit a finished edit to the undo ring, apply Ctrl+Z / Ctrl+Y (UI or
        // harness), then sync m_staged -> live. PPA only, after the params and before the
        // clock (a structural Invalidate is re-initialised by THIS frame's chain Update).
        void TickStagedParams(const ChainPreviewContext& ctx);
        // The ONE writer of the stage's live ChainPhysicsComponent: field-wise
        // CopyAuthoredInto when m_staged differs from what was synced last, Invalidate on
        // a structural difference.
        void SyncStagedToLive(const ChainPreviewContext& ctx, const char* why);
        // A new staged baseline (spawn, auto-detect): the sync baseline and the undo
        // base become m_staged; `clearHistory` empties the ring.
        void ResetStagedTracking(bool clearHistory);
        void LogStagedSummary(World* world, const char* when) const;
        void DrawStagedParams(Renderer* renderer, const ChainParamTreeFn& drawParamTree);
        // The tree call itself (DrawStagedParams' tail; the panel-tree probe calls it
        // too). False when it could not draw.
        bool DrawStagedTree(Renderer* renderer, const ChainParamTreeFn& drawParamTree);
        // Apply, prominent at the top of Parameters.
        void DrawParamsApplyRow();
        // The Play-mode Parameters body.
        void DrawLiveParams(const ChainLiveEditFn& drawLive);
        // The Inspector's edit request (PPA, Edit mode).
        void TickEditRequest(const ChainPreviewContext& ctx);
        // The keep slot after Stop (PPA, before TickStagedParams).
        void TickPlayKeep(const ChainPreviewContext& ctx);
        std::string ApplyButtonLabel() const;
        void DrawShowUi();
        void DrawOverlay(Renderer* renderer, const ImVec2& origin, const ImVec2& size);
        void TickOverlay(const ChainPreviewContext& ctx);
        void LogOverlay(const char* when) const;
        // The mask the overlay DRAWS with: the user's edit / sim mask; 0 in Mesh only;
        // roots + chain + links in Chain only; capsules in Colliders only. The user's
        // masks are untouched, so switching back restores them.
        std::uint32_t ActiveOverlayMask() const
        {
            if (EffectiveDisplay() == DisplayMode::MeshOnly) return 0u;
            if (EffectiveDisplay() == DisplayMode::ChainOnly)
                return ChainOverlay::kRoots | ChainOverlay::kChain | ChainOverlay::kLinks;
            if (EffectiveDisplay() == DisplayMode::CollidersOnly) return ChainOverlay::kCapsules;
            return m_simulate ? m_showSim : m_showEdit;
        }

        // ---- display mode: what the picture shows ----
        DisplayMode EffectiveDisplay() const
        {
            return m_harnessDisplayOff ? DisplayMode::Both : m_display;
        }
        // UI half: the segmented control / the picture's corner toggle / V.
        void RequestDisplay(DisplayMode m, const char* via);
        void DrawDisplayOptions();
        // PPA (after TickScene - the floor exists): the harness schedule, the stage's
        // secondary-view bit (idempotent, O(stage)), the settle line.
        void TickDisplay(const ChainPreviewContext& ctx);
        // Stage entities whose VisibilityComponent lacks the secondary-view bit.
        std::uint32_t CountStageHidden(const World& world, std::uint32_t* ents) const;
        // "[CHAINPREVIEW] <head> mode= ..." - head is "display <n>:" (the settled line of
        // switch n), "display respawn <k>:" (the settled line of the k-th respawn after
        // the first spawn) or "display:" (every teardown).
        void LogDisplay(Renderer* renderer, const World* world, const char* when,
                        const char* head) const;

        // ---- preview scene, driver, follow ----
        // Harness Reset windows (-chainpreview_fullreset_at) as an in-process A/B: the
        // state each window leaves, compared with the first's. PPA, FIRST - before
        // anything this frame can reset the chains.
        void TickResetWindows(const ChainPreviewContext& ctx);
        // Harness scene events due this frame (gravity / wind / drive / teleport /
        // follow kill). PPA, before TickStagedParams (a wind event writes m_staged and is
        // synced in the same window).
        void TickSceneHarness(const ChainPreviewContext& ctx);
        // Draw's scene requests (scale, floor, focus, teleport button, stress pick,
        // follow pick) and the floor entity. PPA, after the params.
        void TickScene(const ChainPreviewContext& ctx);
        // THE writer of the stage root's LocalTransform: scale, the driver's speeds, the
        // stress program, the teleports - advanced by the SAME StageClock steps as the
        // clip and the chains. PPA, after the clock.
        void TickDriver(const ChainPreviewContext& ctx);
        // FollowSceneEntity: the source's clip + time into the clip clock.
        void TickFollow(const ChainPreviewContext& ctx);
        void FollowFallback(const char* why);
        // -chainpreview_shot captures. PPA, top (the previous frame's picture).
        void TickShots(const ChainPreviewContext& ctx);
        void EnsureFloor(const ChainPreviewContext& ctx);
        void DestroyFloor(World& world);
        // Scene gravity + the floor proxy capsule, pushed with the lane.
        void PushPreviewScene(ChainPhysicsSystem& cps);
        // The spawn's default scale (scene instance of the same prefab, else the prefab's
        // authored root scale), unless the harness / Use Selected / the user chose one.
        void ResolveStageScale(World& world, Entity root);
        std::uint32_t ApplyWindToStaged(const float dir[3], float strength, float freq,
                                        float turb, bool alsoGroups);
        void Teleport(float dist, float angleDeg, const char* why);
        void FocusChains(const ChainPreviewContext& ctx);
        std::string ResolveStressClip(bool idle);
        void DrawSceneUi();
        void LogScene(const char* when) const;
        void LogDrive(const char* when) const;
        float SubjectHeight() const { return m_subjectHeight > 0.f ? m_subjectHeight : 2.f * m_radius; }
        float FloorSize() const;
        float DriveSpan() const { return m_driveSpan > 0.f ? m_driveSpan : 2.f * SubjectHeight(); }

        // ---- Apply / Revert / presets (ChainPhysicsPreviewApply.cpp) ----
        // Presets + Revert requests into m_staged (Draw's buttons, the harness schedule);
        // a preset is clamped field by field. PPA, BEFORE TickStagedParams (each becomes
        // one panel-undo step and is synced in the same serial window).
        void TickPresets(const ChainPreviewContext& ctx);
        // Apply requests. PPA, AFTER TickStagedParams (the live component already
        // equals m_staged).
        void TickApply(const ChainPreviewContext& ctx);
        // Pre-checks: a rig (bone-name hash) mismatch is refused unless `confirmed` (from
        // the UI it opens the confirmation); capsule bone indices past the target's
        // skeleton are warned.
        void RunApply(const ChainPreviewContext& ctx, ApplyDest dest, const std::string& arg,
                      bool confirmed, const char* why);
        // The serializer's "  ChainPhysics: ...\n" line for m_staged, through a scratch
        // World (never the live component - a probe may have written it).
        // *textRoundTrip: serialize(deserialize(blob)) == blob.
        bool SerializeStaged(std::string& blob, bool* textRoundTrip) const;
        // The UI's cached Apply targets (Draw never reads the World).
        void RefreshApplyTargets(const ChainPreviewContext& ctx);
        Entity ResolveSceneTarget(World& world, const std::string& name, std::string* why) const;
        // FNV of the stage skeleton's bone names (0 = unknown).
        std::uint32_t StageRigHash(Renderer& renderer, std::uint32_t* boneCount) const;
        std::uint32_t PrefabRigHash(const std::string& iskelPath, std::uint32_t* boneCount);
        void DoRevert(const char* why);
        // Sync probe: rest-pose entries of the stage's LIVE component whose name cache is
        // stale at the end of PPA - each is a name re-resolve in this frame's
        // ApplyRestPose. Counted from settle.
        void TickSyncProbe(const ChainPreviewContext& ctx);
        void LogSyncProbe(const char* when) const;
        void DrawApplyUi();
        void DrawPresetUi();
        void DrawM5Modals();

        // ---- layout ----
        void DrawToolbar(Entity editorSelection, bool playing);
        void DrawBody(Renderer* renderer, const ChainParamTreeFn& drawParamTree,
                      const ChainLiveEditFn& drawLive, bool playing);
        // The picture + transport, sized from the view child.
        void DrawViewArea(Renderer* renderer);
        void DrawParamsBody(Renderer* renderer, const ChainParamTreeFn& drawParamTree,
                            const ChainLiveEditFn& drawLive, bool playing);
        void DrawLayoutSelector();
        // The label column the staged / A/B trees keep free (0 under the
        // -chainpreview_layout_fixed arm = the default item widths).
        float TreeLabelColumn() const;
        // PPA: the "[CHAINPREVIEW] layout:" self-reports of -chainpreview_window.
        void TickLayoutHarness();
        void LogLayout(const char* when) const;

        // The -chainpreview_sections mask in force (~0u = none: the user's).
        std::uint32_t HarnessSectionMask() const;

        // ---- prefab-ref Apply, the scene target, the dirty guard ----
        void TickSceneSave(const ChainPreviewContext& ctx);

        void RecordSceneTarget(World& world, Entity root);
        void KeepSceneTargetAcrossEpoch(World& world);

        static const char* GuardKindName(GuardKind k);

        // Every subject-changing request (and the menu close) goes through ONE dirty
        // confirmation.
        void RequestGuarded(const GuardedRequest& r);         // UI half
        void CommitGuarded(const GuardedRequest& r);          // UI or PPA: posts the request
        void TickGuard(const ChainPreviewContext& ctx);       // PPA: answers

        static std::string StashKey(const std::string& subject);   // normalised subject path
        // The stash key of THIS session. A prefab subject = StashKey(path); an IN-PLACE
        // subject = the scene target's identity (scene path + name#ordinal + Guid) -
        // every in-place clone is written to the same cache file, so the path names no
        // character.
        std::string SessionStashKey() const;
        std::string SessionStashLabel() const;

        // A world load stashes the unapplied staged edits under SessionStashKey(); the
        // next spawn of that subject offers them back. PPA, BEFORE TickStagedParams:
        // takes the answer (a re-apply is one panel-undo step into m_staged).
        void TickReapply(const ChainPreviewContext& ctx);

        void TickHarnessRequests(const ChainPreviewContext& ctx);
        Entity FindNamed(World& world, const std::string& nameIdx) const;

        // ---- Inspector "Edit Chains..." + Play LIVE ----
        bool                       HasUnappliedStagedWork() const;   // HasUnappliedWork minus the keep slot

        // ---- solver-state snapshot + stats overlay ----
        void TickStats(const ChainPreviewContext& ctx);
        void LogStats(const char* when) const;

        // ---- A/B ghost, per-group solo, poke (ChainPhysicsPreviewCompare.cpp) ----
        // A/B GHOST: B is ONE bare entity carrying only a ChainPhysicsComponent (+ the
        // stage tags) - no skeleton, mesh, animation, pin, skin job, pose ring or vertex
        // ring - in the PREVIEW LANE's set, fed A's animated input pose every frame by
        // ChainPhysicsSystem::SetPreviewPoseFeed, so it costs one chain solve. It is
        // drawn only through the overlay (the ghost: FillDebugSnapshot(B) as desaturated
        // links + particles, mapped with A's skeleton -> world transform) plus the A/B
        // parameter diff. B has its OWN staged component (m_abStaged); the single-writer
        // rule applies to it too - SyncAbToLive is the only writer of B's live one.
        void TickAb(const ChainPreviewContext& ctx);          // PPA, after TickStagedParams
        void TickAbReadout(const ChainPreviewContext& ctx);   // PPA, after TickStats
        void CreateAbStage(const ChainPreviewContext& ctx);
        void DestroyAbStage(Renderer* renderer, World* world, const char* why);
        void SyncAbToLive(const ChainPreviewContext& ctx, const char* why);
        void RebuildAbDiff();
        void LogAb(const char* when) const;
        void DrawAbUi(Renderer* renderer, const ChainParamTreeFn& drawParamTree);

        AbBudget SampleAbBudget(const ChainPreviewContext& ctx) const;
        // The -chainpreview_param grammar (REFLECT field / g<idx>_<member> /
        // restPoseWeight / restPoseEnabled), for B's copy too; the field tables live in
        // ChainPhysicsPreviewWindow.cpp.
        static bool ParamKeyValid(const std::string& key, const std::string& value);
        static bool ApplyParamKey(ChainPhysicsComponent& c, const std::string& key,
                                  const std::string& value);

        // PER-GROUP SOLO (PhAT "Selected Simulation"): the rest FROZEN AT REST.
        void TickSolo(const ChainPreviewContext& ctx);        // PPA, before the clock
        void ApplySolo(const ChainPreviewContext& ctx, int groupIdx, const char* why);
        int  ResolveSoloGroup(const std::string& name) const; // -1 none, -2 unresolvable
        void LogSolo(const char* when) const;
        void DrawSoloUi();

        // POKE (Ctrl + right-drag in the viewport, PhAT's shape): Draw only records the
        // gesture (m_pendingPoke); TickPoke casts the panel's OWN camera-basis ray from the
        // camera it authored (not Ray::FromViewport: no matrix inverse, no reverse-Z
        // unprojection), maps it into the stage's SKELETON space, picks the nearest
        // particle of the solver snapshot (CPU, no GPU readback) and hands the impulse to
        // ChainPhysicsSystem::ApplyExternalImpulse.
        void TickPoke(const ChainPreviewContext& ctx);        // PPA, after TickStats

        // ---- in-picture rest-pose gizmo ----
        // PPA snapshots what Draw needs (Draw reads no World): the picked bone's and its
        // parent's WORLD matrices (FK through ctx.buildBones - the overlay's source), the
        // seed rotation a new entry starts from (the bone's clean base -
        // EnsureBoneRestPoseEntry's rule), and the pickable bones' positions
        // (ChainRestPose::CollectChainBones, m_stagePose's focus group).
        void TickPoseSnapshot(const ChainPreviewContext& ctx);   // PPA, after TickOverlay
        void TickPoseGizmo(const ChainPreviewContext& ctx);      // PPA, before TickStagedParams
        void DrawPoseGizmo(const ImVec2& origin, const ImVec2& size);   // Draw, before ##cpv_orbit
        void PickPoseBone(const ImVec2& local, const ImVec2& size);
        bool PoseViewProj(const ImVec2& size, DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const;
        // The ONE edit path of both the ring and the harness: find / seed the picked
        // bone's entry in m_staged.restPose and turn it by a WORLD delta
        // (ChainRestPose::ApplyWorldDelta). Panel state only.
        bool ApplyPoseDelta(const DirectX::XMFLOAT4X4& deltaWorld);
        // A gesture is open and holds the panel commit + the sync.
        bool PoseGestureHeld() const
        { return (m_poseGestureUi || m_poseGestureHarness) && !m_harnessPoseGestureOff; }
        // End a UI ring drag from our side AND release ImGuizmo's global "using"
        // latch.
        void ReleasePoseGesture();

    public:
        // Scene component UI supplied by EditorLayer, with scene undo semantics.
        std::function<void()> drawSceneColliders;

    private:
        std::uint32_t m_displaySpawnSeen = 0;                  // m_spawnCount TickDisplay last saw
        std::uint32_t m_displayRespawns = 0;
        std::uint64_t m_displayRespawnSettleAt = 0;
        DisplayMode   m_display = DisplayMode::Both;           // UI state (persisted)
        DisplayMode   m_displayLogged = DisplayMode::Both;     // the last mode PPA saw
        bool          m_displayLoggedValid = false;
        bool          m_harnessDisplayOff = false;
        std::vector<std::pair<DisplayMode, std::uint64_t>> m_harnessDisplay;
        std::size_t   m_harnessDisplayNext = 0;
        std::uint32_t m_displaySwitches = 0;                   // PPA-observed mode changes
        std::uint64_t m_displaySettleAt = 0;                   // editor frame of the next settled line
        const char*   m_displayVia = "init";                   // who asked for the last change
        std::uint32_t m_displayFirstDraws = 0, m_displayFirstEdit = 0, m_displayFirstSim = 0;
        bool          m_displayFirstValid = false;
        std::uint32_t m_poseDotsDrawn = 0;                     // Draw's last frame (the pose tool)
        mutable std::uint32_t m_displayLastDraws = 0;          // for the close line

        // ---- layout ----
        Layout        m_layout      = Layout::Auto;
        float         m_splitSide   = kDefSplitSide;   // view width / body width
        float         m_splitStack  = kDefSplitStack;  // view height / body height
        bool          m_stacked     = false;           // Auto's current answer (hysteresis)
        float         m_belowH      = 0.f;             // measured block under the picture (last frame)
        bool          m_sizeCapped  = false;           // the render hit the area cap (display may upscale)
        std::uint32_t m_measuredH   = 0;

        // What the last Draw laid out (UI half writes, PPA logs it).
        LayoutReport  m_layoutRep;

        std::vector<WindowStep> m_harnessWindows;
        bool          m_harnessSizeAuto    = false;
        bool          m_harnessLayoutFixed = false;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> m_harnessSections;   // (mask, at)
        std::vector<std::pair<float, std::uint64_t>>         m_harnessParamsScroll; // (frac, at)

        std::vector<SplitStep>                               m_harnessSplits;
        std::uint32_t                                        m_harnessSplitsApplied = 0;

        // ---- open state + the editor frame ----
        bool          m_open = false;
        bool          m_visibleThisFrame = false;
        std::uint64_t m_tick = 0;             // ProcessPendingActions ticks

        // ---- subject ----
        std::string   m_subjectPath;          // what the stage is spawned from
        bool          m_subjectFromHarness = false;
        bool          m_subjectInPlace = false;   // cloned from a scene entity
        std::string   m_pendingSubject;       // set by Draw, applied by PPA
        bool          m_pendingSubjectInPlace = false;
        bool          m_pendingUseSelected = false;
        Entity        m_pendingSelected = NullEntity;
        bool          m_pendingReload = false;
        // Use Selected on a scene instance: the instance root's scale, applied to the
        // stage root (the stage's world size; the chains themselves are invariant to
        // it).
        float         m_pendingScale[3] = { 1.f, 1.f, 1.f };
        bool          m_pendingScaleValid = false;
        float         m_stageScale[3] = { 1.f, 1.f, 1.f };
        bool          m_stageScaleValid = false;
        bool          m_pendingAutoDetect = false;
        char          m_subjectEdit[512] = {};
        std::string   m_status;               // last refusal / info, shown by Draw
        bool          m_spawnFailed = false;  // latched until the subject changes
        // A Use Selected request ALWAYS respawns (the in-place clone reuses one file
        // name, and a same-path prefab instance may carry a different scale), and like
        // every explicit subject request it starts a NEW subject session (the staged
        // carry is dropped).
        bool          m_pendingForceRespawn = false;

        // ---- stage ----
        Entity                     m_stageRoot  = NullEntity;
        Entity                     m_stageChain = NullEntity;
        Entity                     m_camEnt     = NullEntity;
        // Generation-stamped handles of the same three. Entity has no generation of its
        // own, so a raw id destroyed OUTSIDE the panel and recycled would otherwise be
        // simulated, excluded and finally destroyed as if it were still the stage. Every
        // destroy goes through a handle.
        EntityHandle               m_stageRootH  = NullEntityHandle;
        EntityHandle               m_stageChainH = NullEntityHandle;
        EntityHandle               m_camEntH     = NullEntityHandle;
        std::vector<EntityHandle>  m_stageHandles;   // parallel to m_stageEnts
        std::vector<Entity>        m_stageEnts;
        std::unordered_set<Entity> m_stageSet;
        // The stage's SkeletonComponent entities, handed to Renderer::AddAnimPinOwner.
        // Rewritten ONLY in ProcessPendingActions (spawn / destroy / epoch drop):
        // classify reads it on workers.
        std::unordered_set<Entity> m_stagePin;
        bool                       m_pinLogged  = false;
        // The PREVIEW LANE's set = every stage entity (so a second chain carrier - an
        // accessory, a component auto-attached after the spawn - can never fall back to
        // the main lane). Same rules as m_stagePin: the chain Update reads it, so it is
        // rewritten ONLY in ProcessPendingActions, and the pointer goes back on every
        // close path.
        std::unordered_set<Entity> m_previewSet;
        Entity                     m_animEnt    = NullEntity;   // Animator owner
        bool                       m_noChain    = false;
        // m_staged: the stage's authored chain parameters, the AUTHORITY (the panel's
        // tree edits it; PPA syncs it into the live component). Taken at spawn (the
        // loaded value) and CAPTURED AGAIN at every teardown while the stage is alive
        // (CopyAuthoredInto from the live component, which equals it after the sync
        // unless a harness probe wrote the live one). A respawn in the SAME subject
        // session (Play -> Stop, a close + reopen, a collapsed tab, a world reload of a
        // harness subject) re-applies it field by field + Invalidate, so tuning work is
        // never lost at a Play boundary. A NEW subject request drops it.
        ChainPhysicsComponent      m_staged;
        bool                       m_stagedValid = false;
        bool                       m_stagedCarry = false;   // captured, pending re-apply
        bool                       m_stagedDirty = false;   // differs from the loaded value
        std::string                m_stagedSubject;         // the subject it belongs to
        std::string                m_loadedBlob;            // serializer blob at spawn (dirty test)
        std::uint32_t              m_carryApplied = 0;      // respawns that re-applied it

        // ---- the one writer + panel undo ----
        // m_stagedSynced: what the live component was last made equal to. The sync
        // compares against THIS, not against the live component, so a write from anywhere
        // else (EditorLayer's stage-edit probe) is not undone behind the user's back.
        // m_stagedStructural: raised by the tree's structural hook; the sync ORs it with
        // NeedsReinit.
        ChainPhysicsComponent      m_stagedSynced;
        bool                       m_stagedStructural = false;
        std::uint32_t              m_stageSkelAsset = ~0u;  // for Draw's tree (registry index)

        std::deque<UndoEntry>      m_undo;                  // oldest first, at most kUndoDepth
        std::vector<UndoEntry>     m_redo;
        ChainPhysicsComponent      m_undoBase;              // the value the next commit records as "before"
        bool                       m_undoBaseValid = false;
        std::string                m_undoSubject;           // the ring belongs to this subject session
        bool                       m_undoSessionReset = false;
        int                        m_pendingUndo = 0, m_pendingRedo = 0;
        std::uint64_t              m_undoOps = 0, m_redoOps = 0, m_commits = 0;
        std::uint64_t              m_syncs = 0, m_rebuilds = 0;
        const char*                m_lastRebuildWhy = "";
        double                     m_toastUntil = 0.0;      // ImGui time the "re-initialised" toast ends
        int                        m_uiFocusFrame = -100;
        // The STAGE tree's rest-pose tool state (owner = the stage carrier; reset
        // with every spawn).
        ChainPoseState             m_stagePose;

        std::vector<std::uint64_t> m_harnessUndoAt, m_harnessRedoAt, m_harnessFullResetAt;
        std::size_t                m_harnessUndoNext = 0, m_harnessRedoNext = 0;
        std::size_t                m_harnessFullResetNext = 0;

        // ---- overlay ----
        std::uint32_t              m_showEdit = ChainOverlay::kDefaultEdit;
        std::uint32_t              m_showSim  = ChainOverlay::kDefaultSim;
        bool                       m_depthFade = true;
        int                        m_overlayFocusGroup = -1;   // -1 = auto (first limited group)
        ChainOverlay::Snapshot     m_overlaySnap;              // taken in PPA, drawn in Draw
        ChainOverlay::Camera       m_overlayCam;               // what UpdateCamera authored
        ChainOverlay::Stats        m_overlayStats;             // Draw's last frame
        std::uint32_t              m_overlayOutW = 0, m_overlayOutH = 0;
        std::uint64_t              m_overlayDrawn = 0;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> m_harnessOverlay;
        std::size_t                m_harnessOverlayNext = 0;
        bool                       m_harnessOverlayBadProj = false;
        bool                       m_harnessReinitOff = false;

        std::vector<OverlayShot>   m_harnessOverlayShots;
        std::uint32_t m_budgetInstances = 0, m_budgetSkinJobs = 0;
        std::uint32_t m_budgetBones = 0, m_budgetVerts = 0, m_stripped = 0;
        // Distinct shared skinned-source keys the stage's parts use - the residual a
        // close leaves behind in a scene that did not already hold this character (see
        // the isolation resources line).
        std::uint32_t m_stageSources = 0;
        std::uint32_t m_spawnCount = 0;
        std::uint64_t m_spawnTick  = 0;
        std::uint32_t m_worldEpoch = 0;
        bool          m_worldEpochSeen = false;
        const World*  m_lastWorld = nullptr;
        bool          m_suspendedForPlay = false;

        // ---- globals the panel overrides, restored on EVERY close path ----
        // Chain LOD (forced off), the anim pin pointer and the preview lane pointer.
        // Anim culling itself is never overridden: the pin set keeps the stage animated.
        bool          m_globalsOverridden = false;
        bool          m_savedLodEnabled   = false;
        bool          m_pinPushed         = false;
        bool          m_lanePushed        = false;   // SetChainPhysicsPreviewLane(&m_previewSet)

        // ---- simulation ----
        bool          m_simulate          = true;    // the Simulate checkbox
        bool          m_resetOnScrub      = true;    // the "Reset sim on scrub" checkbox
        const char*   m_pendingSimReset   = nullptr; // latched by OnPlayheadDiscontinuity
        std::uint64_t m_simResets         = 0;
        float         m_lastSimDt         = 0.f;     // what was pushed last frame

        // ---- the one clock + the simulation transport ----
        std::uint32_t m_frameSteps        = 0;       // THIS editor frame's N (clip AND lane)
        float         m_frameStepDt       = ChainPreview::kFixedDt;
        std::uint32_t m_lastLaneSteps     = 0;       // what was pushed last frame
        // Draw's transport buttons, applied by TickSimTransport (PPA).
        bool          m_pendingPauseToggle = false;
        bool          m_pendingSimStep     = false;
        bool          m_pendingFullReset   = false;
        int           m_pendingSimulate    = -1;     // -1 none, 0 off, 1 on
        float         m_pendingTimeScale   = -1.f;   // < 0 none
        float         m_pendingFixedDt     = -1.f;   // < 0 none, 0 follow editor
        // A lane that INTEGRATED and then stopped (Simulate off, reverse
        // playback) holds particles from another pose - going live again
        // resets rather than whipping the chains across the gap.
        bool          m_laneHasState       = false;
        bool          m_simStale           = false;
        std::uint64_t m_resetSerial        = 0;      // ResetSimulation calls (hold check)
        // Cost sentinel: the SCENE lane's chainCpuMs when the stage spawned (in Edit mode
        // the main lane never RunPasses, so it must not move).
        float         m_sceneCpuMsAtOpen   = 0.f;

        // The hold check (harness only): a frame that HELD must reproduce the
        // previous frame's post-chain pose bit for bit when nothing moved the
        // playhead or reset the chains in between.
        PushRecord    m_lastPush, m_prevPush;
        std::uint32_t m_prevPoseHash       = 0;
        bool          m_prevPoseValid      = false;
        std::uint64_t m_holdChecks = 0, m_holdMismatches = 0;
        // harness hooks
        std::uint64_t m_harnessPauseAt = 0, m_harnessStepAt = 0;
        bool          m_harnessPauseDone = false, m_harnessStepDone = false;
        bool          m_harnessLaneOff   = false;   // -chainpreview_dt=0
        bool          m_harnessHoldOff   = false;   // -chainpreview_hold_off
        bool          m_harnessPacingSet = false;   // -chainpreview_steps / _realdt given

        // harness A/B windows (-chainpreview_ab): Run / Capture refer to
        // window m_abIndex (0 = a).
        AbPhase       m_abPhase    = AbPhase::Off;
        std::size_t   m_abIndex    = 0;             // the window Run / Capture refer to
        double        m_abSeconds  = 0.0;
        std::uint64_t m_abTarget   = 0;             // steps (fixed mode), 0 = time-based
        std::uint32_t m_abWait     = 0;
        // m_abPacing[0] = window a's (the run's own, saved when it starts);
        // [1..] the -chainpreview_ab segments (empty = a's pacing).
        std::vector<AbPacing> m_abPacing;
        bool          m_abSavedA   = false;
        std::vector<AbWindow> m_abWin;
        // Draw never reads the solver: PPA copies the lane's counters here.
        ChainPhysicsSystem::PreviewLaneStats m_uiLaneStats{};
        float         m_uiSwayTime        = 0.f;

        // ---- controller, clip and transport ----
        Controller    m_controller        = Controller::RestPose;
        Controller    m_pendingController = Controller::RestPose;
        bool          m_pendingControllerSet = false;
        std::vector<std::string> m_clipList;          // scanned .ianim, sorted
        bool          m_clipScanDone      = false;
        // The clip scan is LAZY: requested by Draw when the Clip picker is first shown
        // (or by Rescan), or run for a harness clip NAME. A RestPose session and a clip
        // given as a path never walk the roots.
        bool          m_pendingRescan     = false;
        char          m_clipFilter[128]   = {};

        // the subject's own authored aliases, cached by PPA for Draw (which
        // never reads the World)
        std::vector<AuthoredClip> m_authoredClips;
        std::string   m_pendingClipPath, m_pendingClipSource;
        bool          m_pendingClipSet    = false;
        std::string   m_clipPath;            // what the controller plays
        std::string   m_clipSource;          // sub-clip in a multi-clip file
        std::string   m_clipAlias;           // "__cpv_<stem>" registered on the stage
        bool          m_clipApplyPending  = false;   // (re)register on the stage
        std::uint64_t m_clipSelectTick    = 0;
        Resource::AnimHandle m_clipHandle{};   // the panel's own ref (read-out)
        Resource::AnimationClipSystem* m_clipHandleOwner = nullptr;
        bool          m_clipInfoValid     = false;
        float         m_clipDuration      = 0.f, m_clipFps = 30.f;
        std::uint32_t m_clipFrames        = 0;
        std::uint32_t m_clipMatched       = 0, m_clipChannels = 0, m_clipBones = 0;
        std::vector<std::string> m_boneClipNames, m_morphClipNames;
        bool          m_clipBoundLogged   = false;
        bool          m_clipDeadlineFired = false;
        // the transport: arithmetic on m_clipT only
        double        m_clipT             = 0.0;
        bool          m_playing           = true;
        bool          m_looping           = true;
        float         m_speed             = 1.f;   // negative = reverse
        StageClock    m_clock;
        int           m_pendingStep       = 0;
        int           m_pendingJump       = 0;     // -1 first, +1 last
        double        m_pendingScrubT     = -1.0;
        bool          m_pendingPlayToggle = false;
        bool          m_scrubbing         = false;
        TimelineCanvas m_timeline;
        bool          m_timelineFramed    = false;
        // The ruler re-fits whenever its width moves (panel / splitter / stacked mode)
        // until the user zooms or pans it (wheel / middle drag); right-click or the next
        // clip re-fits and clears that. Under -chainpreview_layout_fixed it fits once per
        // clip.
        float         m_timelineFramedW   = 0.f;    // lanesW of the last auto-fit
        bool          m_timelineUserZoom  = false;   // the user zoomed / panned the ruler
        // the pose read-out: what LocalToWorldSystem did with the stage
        bool          m_clipBound         = false;  // our alias is the current clip
        bool          m_lastTpose         = false;  // poseByteOffset == ~0u
        std::uint64_t m_animFrames = 0, m_tposeFrames = 0, m_discontinuities = 0;

        // ---- scale (the stage's world size; the chains ignore it) ----
        // m_stageScale / m_stageScaleValid (above) carry a Use Selected scale into the
        // spawn; m_scaleSource says where the EFFECTIVE one came from.
        const char*   m_scaleSource       = "authored";
        Entity        m_scaleInstance     = NullEntity;   // the scene instance it came from
        float         m_pendingStageScale = -1.f;         // Draw's Scale control (< 0 none)
        bool          m_userScale         = false;        // the user / harness chose it
        float         m_harnessScale      = -1.f;         // -chainpreview_scale (< 0 auto)
        float         m_rootBaseRot[4]    = { 0.f, 0.f, 0.f, 1.f };   // the prefab root's authored rotation
        std::uint64_t m_focusHoldUntil    = 0;            // reframe after a scale change
        float         m_subjectHeight     = 0.f;          // the bind-pose joint box height (framing)

        // ---- floor + gap (a visual plane; the solver collides with capsules only) ----
        int           m_floorMode         = -1;           // -1 = default (on; off under the harness)
        float         m_floorGap          = 0.f;
        bool          m_floorCollide      = false;
        Entity        m_floorEnt          = NullEntity;
        EntityHandle  m_floorH            = NullEntityHandle;
        bool          m_floorRefused      = false;
        std::string   m_floorRefusedWhy;
        std::uint32_t m_floorSpawns       = 0;
        float         m_floorProxyA[3] = {}, m_floorProxyB[3] = {}, m_floorProxyR = 0.f;
        bool          m_floorProxyValid   = false;

        // ---- scene gravity (a preview-lane override, never authored) ----
        float         m_gravDown[3]       = { 0.f, -1.f, 0.f };
        float         m_gravScale         = 1.f;
        bool          m_noGravity         = false;

        // ---- wind UI (writes m_staged) ----
        bool          m_windAlsoGroups    = true;
        std::uint32_t m_windGroupsSet     = 0;            // last apply: groups whose ovr wind it wrote
        std::uint64_t m_windApplies       = 0;

        // ---- turntable (camera yaw, on the stage clock) + pan + focus ----
        bool          m_turntable         = false;
        float         m_turntableSpeed    = 30.f;         // deg/s
        float         m_turntableYaw      = 0.f;          // rad
        float         m_pan[3]            = { 0.f, 0.f, 0.f };   // world-space camera pan
        bool          m_pendingFocusChains = false;
        bool          m_pendingFrameAll   = false;
        float         m_focusDriveBase[3] = { 0.f, 0.f, 0.f };   // drive offset when framed
        std::uint64_t m_focusChainsCount  = 0;

        // ---- the root-motion driver ----
        float         m_driveMove         = 0.f;          // world units / s, ping-pong along Z
        float         m_driveSpan         = 0.f;          // 0 = auto (2 x subject height)
        float         m_driveRot          = 0.f;          // deg / s about world +Y
        DriveProgram  m_driveProgram      = DriveProgram::None;
        double        m_driveProgT        = 0.0;          // program time (stage clock)
        double        m_driveRotTotal     = 0.0;          // accumulated rotation (deg, signed)
        float         m_driveYawDeg       = 0.f;          // the root's yaw now
        float         m_driveOffset[3]    = { 0.f, 0.f, 0.f };
        float         m_driveDir          = 1.f;          // ping-pong direction
        float         m_teleportDist      = 0.f;          // 0 = auto (0.5 x subject height)
        float         m_teleportAngle     = 0.f;          // deg
        bool          m_teleportReset     = false;        // reset the chains on a teleport
        bool          m_pendingTeleport   = false;
        std::uint64_t m_teleports         = 0;
        std::uint64_t m_driveSteps        = 0;
        int           m_pendingStress     = -1;           // 0 idle 1 walkrun 2 yawsweep 3 teleport
        std::string   m_stressIdle, m_stressWalkRun;      // resolved clip paths (Draw's labels)
        bool          m_teleportResetOnce = false;        // a harness ":reset" teleport
        // Harness Reset windows as an in-process A/B (TickResetWindows).
        std::uint32_t m_resetWindows      = 0;
        AbWindow      m_resetWindowRef;

        // ---- FollowSceneEntity ----
        EntityHandle  m_followSrcH        = NullEntityHandle;
        Entity        m_followSrc         = NullEntity;
        std::string   m_followName;
        bool          m_pendingFollowSel  = false;
        Entity        m_uiSelection       = NullEntity;   // Draw's editor selection
        Entity        m_pendingFollowEnt  = NullEntity;
        bool          m_followCulled      = false;
        std::string   m_followStatus;
        std::uint64_t m_followFrames = 0, m_followCulledFrames = 0, m_followClipChanges = 0;
        std::uint64_t m_followFallbacks   = 0;
        double        m_followLastT       = -1.0;

        // ---- harness: scene events ----
        std::vector<SceneEvent> m_harnessScene;
        bool          m_harnessDriveOff   = false;
        std::string   m_harnessStress;
        bool          m_harnessStressDone = false;
        std::string   m_harnessFollow;
        bool          m_harnessFollowDone = false;

        std::vector<Shot> m_harnessShots;

        // ---- Apply / Revert / dirty ----
        // m_loadedValue: what loading the subject yields NOW - the Revert target and the
        // dirty baseline. Taken at spawn from the component as LOADED (before a carry
        // re-applies captured edits), and moved forward by an Apply that PERSISTS to the
        // subject's own source (its .ipfb, or the in-place original).
        ChainPhysicsComponent      m_loadedValue;
        bool                       m_loadedValid = false;
        bool                       m_loadedMoved = false;     // a persisting Apply moved it this session
        ApplyDest                  m_applyDest = ApplyDest::Prefab;
        bool                       m_applyDestUser = false;   // the user picked it (else the default)
        // Use Selected's source root (the in-place ORIGINAL, or the selected prefab
        // instance): the default Scene target. Kept across an epoch change
        // (KeepSceneTargetAcrossEpoch).
        EntityHandle               m_sceneTargetH = NullEntityHandle;
        EntityHandle               m_pendingSceneTargetH = NullEntityHandle;

        ApplyRequest               m_pendingApply;            // Draw -> PPA
        ApplyRequest               m_confirmApply;            // awaiting the rig-mismatch confirm
        bool                       m_confirmApplyOpen = false;
        std::string                m_confirmApplyText;
        bool                       m_pendingRevert = false;
        std::string                m_applyStatus;
        bool                       m_applyStatusError = false;
        std::uint64_t              m_applies = 0, m_applyRefusals = 0, m_reverts = 0;
        // The UI's view of the targets (refreshed by PPA; Draw reads only these).
        std::string                m_uiSceneTargetName;
        bool                       m_uiSceneTargetValid = false;
        bool                       m_uiSceneTargetPrefabRef = false;
        std::uint32_t              m_uiInstanceCount = 0;
        bool                       m_uiDirty = false;
        // Close with unapplied edits: Draw asks, PPA discards (the close path reverts
        // m_staged and clears the panel-undo ring and the stash).
        bool                       m_pendingDiscard = false;
        bool                       m_closeAfterApply = false;
        std::unordered_map<std::string, std::pair<std::uint32_t, std::uint32_t>> m_rigCache;   // iskel -> (hash, bones)

        std::vector<HarnessApply>  m_harnessApply;
        std::string                m_harnessApplySeed;
        bool                       m_harnessSeedDone = false;
        std::vector<std::uint64_t> m_harnessRevertAt;
        std::size_t                m_harnessRevertNext = 0;

        // ---- presets ----
        std::vector<std::string>   m_presetCovered;           // the active preset's keys
        std::string                m_presetActive;            // its name ("" none)
        int                        m_presetGroup = -1;        // -1 = every group of its type
        std::vector<ChainPresets::Preset> m_userPresets;
        bool                       m_userPresetsScanned = false;
        std::string                m_pendingPreset;           // Draw -> PPA (name or path)
        bool                       m_pendingPresetSet = false;
        std::string                m_presetPick;              // the combo's selection (Draw)
        bool                       m_pendingPresetRescan = false;
        char                       m_presetSaveName[64] = {};
        int                        m_presetSaveTarget = 0;    // 0 chain 1 cloth
        bool                       m_pendingPresetSave = false;
        std::string                m_pendingPresetDelete;
        std::uint64_t              m_presetLoads = 0;

        std::vector<HarnessPreset> m_harnessPresets;
        bool                       m_harnessPresetClampOff = false;

        // ---- sync probe (rest-pose name re-resolves) ----
        bool                       m_harnessSyncWholesale = false;
        std::uint64_t              m_boneNameResolves = 0;    // since settle
        std::uint64_t              m_syncProbeFrames = 0;
        std::uint32_t              m_syncProbeEntries = 0;
        std::uint32_t              m_syncProbeSpawn = 0;      // the spawn the counters belong to

        // ---- prefab-ref Apply, the scene target, the dirty guard ----
        // -chainpreview_apply_restpose (reverse arm): Apply onto a prefab-REF instance
        // writes the whole authored block instead of the physics material only.
        bool                       m_harnessApplyRestPose = false;

        std::vector<SceneSave>     m_harnessSceneSaves;

        // The Use Selected target survives AdoptState (ids + generations are restored;
        // otherwise it is re-resolved through its Guid) and, once set, is NEVER replaced
        // by the live selection - lost = refused.
        bool                       m_sceneTargetSet  = false;   // this session has a Use Selected target
        bool                       m_sceneTargetLost = false;
        std::string                m_sceneTargetName;
        std::uint64_t              m_sceneTargetGuidHi = 0, m_sceneTargetGuidLo = 0;
        bool                       m_sceneTargetGuidValid = false;
        std::uint32_t              m_sceneTargetKeeps = 0, m_sceneTargetReresolves = 0;
        bool                       m_harnessSceneTargetDrop = false;
        bool                       m_uiSceneTargetLost = false;

        GuardedRequest             m_guarded;                 // awaiting the answer
        bool                       m_confirmGuardOpen = false; // Draw opens the modal
        int                        m_pendingGuardAnswer = -1;  // 0 keep 1 discard 2 apply
        GuardedRequest             m_continueAfterApply;       // "Apply and continue"
        std::uint32_t              m_confirms = 0;
        std::vector<int>           m_harnessConfirm;
        std::size_t                m_harnessConfirmNext = 0;
        bool                       m_harnessConfirmOff = false;

        std::string                m_sceneTargetScene;         // scene path at Use Selected
        int                        m_sceneTargetOrdinal = 0;   // k of FindNamed's "<name>#k"
        std::string                m_scenePathNow;             // ctx.scenePath, latched per PPA
        std::string                m_reapplyLabel;             // the offered stash's source
        std::unordered_map<std::string, StagedStash> m_stagedStash;   // SessionStashKey() -> value
        bool                       m_reapplyOffer = false;
        std::string                m_reapplyKey;
        int                        m_pendingReapply = -1;      // 1 re-apply, 0 forget
        int                        m_harnessReapply = -1;
        std::uint32_t              m_stashes = 0, m_reapplies = 0;
        std::uint32_t              m_reapplyOffers = 0;        // offers made (the pack's gate)
        bool                       m_harnessStashPathKey = false;
        bool                       m_harnessReinitBare = false;

        // -chainpreview_request entries
        std::vector<HarnessRequest> m_harnessRequests;
        Entity                     m_harnessSelectOut = NullEntity;
        bool                       m_harnessWorldReload = false;
        std::string                m_harnessUseSelected;       // -chainpreview_subject=selected:<name>
        bool                       m_harnessUseSelectedDone = false;

        // ---- Inspector "Edit Chains..." + Play LIVE ----
        // The Inspector's "Edit Chains..." (Edit mode) waits here for PPA.
        EntityHandle               m_pendingEditH = NullEntityHandle;
        bool                       m_pendingEdit = false;
        bool                       m_pendingFocus = false;       // Draw: SetNextWindowFocus
        std::uint32_t              m_editRequests = 0, m_editSameTarget = 0, m_editGuardAsks = 0;
        // The carrier the Inspector named (Apply prefers it over the first
        // carrier in the root's DFS). Set with the Use Selected it caused;
        // dropped by every other subject change.
        EntityHandle               m_sceneCarrierH = NullEntityHandle;
        EntityHandle               m_pendingSceneCarrierH = NullEntityHandle;

        // The clicked carrier travels INTO the stage: SpawnStage picks the stage carrier
        // with the same name path below the root (else the same DFS ordinal), so the
        // staged values are read from the carrier Apply will write. Committed with the
        // subject (Use Selected / Edit Chains), cleared by Load / drop.
        CarrierPref                m_carrierPref, m_pendingCarrierPref;
        // What the last spawn picked (kept across DestroyStage: a hidden panel's
        // "same target" test and Apply's counterpart rule still need it).
        std::string                m_stageCarrierPath;
        int                        m_stageCarrierOrdinal = -1;
        std::uint32_t              m_stageCarrierCount = 0;
        std::uint32_t              m_applyCarrierMismatch = 0;
        // The Play-time Inspector request (the LIVE target), cleared at Stop.
        EntityHandle               m_liveRequestH = NullEntityHandle;
        bool                       m_wasPlaying = false;

        // "Keep these values".
        PlayKeep                   m_playKeep;
        std::uint32_t              m_playKeeps = 0, m_playKeepApplied = 0, m_playKeepDropped = 0;
        bool                       m_harnessPlayKeepOff = false;

        // ---- solver-state snapshot + stats overlay ----
        bool                       m_showStats = true;
        std::vector<std::pair<bool, std::uint64_t>> m_harnessStats;
        std::size_t                m_harnessStatsNext = 0;
        bool                       m_harnessStatsSet = false;
        ChainPhysicsSystem::DebugSnapshot m_dbgSnap;          // the stage carrier's (PPA)
        bool                       m_dbgValid = false;
        ChainOverlay::StatsBox     m_statsBox;                // Draw reads only this

        StageStats                 m_stageStats;
        std::uint32_t              m_statsMaxTier2 = 0, m_statsMaxInterpen = 0;

        // ---- A/B ghost, per-group solo, poke (ChainPhysicsPreviewCompare.cpp) ----
        bool                       m_abOn = false;
        int                        m_pendingAbToggle = -1;      // Draw: 0 off, 1 on
        std::vector<AbPress>       m_pendingAbPress;            // Draw's buttons (+ the harness)
        ChainPhysicsComponent      m_abStaged;                  // B's authority
        ChainPhysicsComponent      m_abSynced;                  // B's live value as last synced
        bool                       m_abStagedValid = false;
        bool                       m_abStructural = false;      // B's tree structural hook
        std::string                m_abSubject;                 // the subject B belongs to
        Entity                     m_abEnt  = NullEntity;
        EntityHandle               m_abEntH = NullEntityHandle;
        std::vector<std::string>   m_abDiff;                    // "key: A -> B" (Draw reads)
        ChainPhysicsComponent      m_abDiffA, m_abDiffB;        // what m_abDiff describes
        bool                       m_abDiffValid = false;
        ChainPhysicsSystem::DebugSnapshot m_abDbg;              // B's solver snapshot (PPA)
        bool                       m_abDbgValid = false;

        AbDivergence               m_abDiv;
        std::uint32_t              m_abMaxDiverged = 0;
        std::uint64_t              m_abFrames = 0;
        // Frames compared while B's parameters EQUALLED A's, and of those, the
        // frames where any bone diverged - the pose feed's exactness claim:
        // equal parameters must give bit-identical chains (the second stays 0).
        std::uint64_t              m_abSameFrames = 0, m_abSameDivergedFrames = 0;
        std::uint32_t              m_abCreates = 0, m_abSwaps = 0, m_abCopies = 0, m_abPromotes = 0;
        std::uint32_t              m_abSyncs = 0, m_abRebuilds = 0;
        AbBudget                   m_abBudgetOff, m_abBudgetOn;
        std::uint64_t              m_abBudgetOnAt = 0;
        bool                       m_abBudgetLogged = false;

        std::vector<HarnessAbParam> m_harnessAbParams;
        bool                       m_harnessAbSame = false;

        std::vector<HarnessAbPress> m_harnessAbPresses;

        int                        m_soloGroup = -1;            // what the solver was handed
        int                        m_pendingSolo = -2;          // Draw: -2 none, -1 clear, >= 0 group
        Entity                     m_soloAppliedTo = NullEntity;   // A's carrier it was handed for
        std::string                m_soloSubject;
        std::uint32_t              m_soloSets = 0;

        std::vector<HarnessSolo>   m_harnessSolo;
        bool                       m_harnessSoloOff = false;

        PendingPoke                m_pendingPoke;
        bool                       m_pokeDragging = false;
        float                      m_pokeStart[2] = { 0.f, 0.f };
        float                      m_pokeBlend = 0.1f;          // s (the TTL)
        float                      m_pokeGain  = 8.f;           // drag distance -> impulse (1/s)
        std::uint32_t              m_pokes = 0, m_pokeRefusals = 0;
        std::string                m_pokeStatus;

        std::vector<HarnessPoke>   m_harnessPokes;
        std::uint32_t              m_pokeProbeAfter = 3, m_pokeProbeSettle = 180;

        PokeProbe                  m_pokeProbe;

        // ---- in-picture rest-pose gizmo ----
        std::uint64_t              m_poseGestureReleases = 0;
        PoseSnap                   m_poseSnap;
        bool                       m_poseGestureUi = false;       // Draw: ImGuizmo::IsUsing in our ID scope
        bool                       m_poseGestureHarness = false;  // PPA: a -chainpreview_pose_gizmo gesture
        bool                       m_poseGestureWas = false;      // PPA's last view of either (the edges)
        bool                       m_poseAutoPaused = false;      // the clock was paused BY the gesture
        bool                       m_poseLastAutoPaused = false;  // ...for the gesture that opened last
        bool                       m_poseGizmoOver = false;       // Draw: ImGuizmo::IsOver, this frame
        bool                       m_orbitActive = false;         // Draw: ##cpv_orbit held last frame
        DirectX::XMFLOAT4X4        m_poseGizmoMatrix{};           // the fed matrix, frozen while held
        std::uint32_t              m_poseGizmoBone = ~0u;
        std::uint64_t              m_poseGestures = 0, m_poseEdits = 0, m_poseCreated = 0;
        std::uint64_t              m_posePicks = 0, m_poseGizmoSubmits = 0, m_poseRefusals = 0;
        std::string                m_poseStatus;

        std::vector<HarnessPoseGesture> m_harnessPoseGestures;
        std::size_t                m_harnessPoseNext = 0;
        std::uint32_t              m_harnessPoseReports = 0;
        bool                       m_harnessPoseLocalOrder = false;
        bool                       m_harnessPoseGestureOff = false;

        // ---- view ----
        std::uint32_t m_viewId = ~0u;
        std::uint32_t m_sizeW = 640, m_sizeH = 360;       // committed (debounced)
        std::uint32_t m_pendW = 640, m_pendH = 360;
        std::uint32_t m_measuredW = 0;
        double        m_pendAt = 0.0;
        std::uint32_t m_renderW = 0, m_renderH = 0;        // what we request
        std::uint32_t m_laneAskedW = 0, m_laneAskedH = 0;
        bool          m_sharingLane = false;
        bool          m_hovered = false;
        bool          m_viewUnavailable = false;
        bool          m_requesting = false;   // first RequestPreview has gone out

        // ---- camera (the panel's own orbit; never the viewport's input) ----
        float         m_yaw = 0.f, m_pitch = 0.f, m_dist = 1.f;
        float         m_focus[3] = { 0.f, kStageOriginY + 0.9f, 0.f };
        float         m_radius = 0.9f;
        float         m_frameWide = 0.f;   // the framed horizontal extent
        float         m_camRadius = 0.f;   // the radius the camera used (aspect-aware)
        bool          m_focusValid = false;
        std::uint32_t m_focusTries = 0;

        // ---- harness ----
        std::string   m_harnessSubject;
        std::uint32_t m_harnessW = 0, m_harnessH = 0;
        std::uint32_t m_harnessFrames = 0;
        std::uint64_t m_harnessCloseAt = 0;
        std::uint64_t m_harnessOpenAt = 0;
        bool          m_harnessOpened = false;
        bool          m_harnessClosed = false;
        bool          m_harnessStripOff = false;
        bool          m_harnessReleaseOff = false;
        bool          m_harnessPlayTeardownOff = false;
        std::string   m_harnessClip;
        bool          m_harnessClipResolved = false;
        double        m_harnessTime       = 0.0;
        bool          m_harnessTimeSet    = false;
        bool          m_harnessPinOff     = false;
        std::uint64_t m_harnessResetAt    = 0;
        bool          m_harnessResetDone  = false;
        std::string   m_harnessDump;
        bool          m_dumpWritten       = false;

        std::vector<HarnessParam> m_harnessParams;      // -chainpreview_param
        bool          m_harnessCarryOff   = false;
        bool          m_harnessLodKeep    = false;
        std::uint32_t m_harnessTries = 0;     // ticks the subject has not resolved
        bool          m_harnessDeadlineFired = false;
        Census        m_censusPre, m_censusOpen, m_censusPost;
        std::uint64_t m_censusOpenAt = 0, m_censusPostAt = 0;
        bool          m_isolationLogged = false;

        // ---- panel visibility (the [PANELVIS] self-reporter) ----
        // CameraPreviewWindow's four counters: counted UNCONDITIONALLY; only the
        // -panelvisibleverify report and its ENGINE_CHECK are gated. A harness-requested
        // panel that stops drawing produces no picture and no stage, and every symptom a
        // pack can see is downstream of that - these make it readable.
        std::uint64_t m_drewFrames         = 0;  // PPA saw a draw last frame
        std::uint64_t m_hiddenFrames       = 0;  // ...did not, while asked for
        std::uint64_t m_beginRefusedFrames = 0;  // ImGui::Begin() returned false
        std::uint32_t m_autoReleases       = 0;  // releases by the not-visible path
    };
}
