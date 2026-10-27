#pragma once

// SkeletonEditorWindow - the Skeleton Editor: a model's skeleton, shown the way
// UE's Skeleton Editor shows one.
//
// Subject: a model (.iscn) whose K line names a skeleton, or an .iskel (the
// same-stem .iscn beside it, when its K line names this .iskel, gives the
// picture; without one, the tree and the details only). The skeleton is read off
// the file (SceneInstanceLoader::LoadSkeletonFile with its blend streams), never
// from the runtime registry, and inspected by Resource::InspectSkeleton - the
// report `-assettool=skeleton_report` prints, so that CLI is this panel's oracle.
//
// Shows: the Skeleton Tree (hierarchy, search box, filter - either flattens it,
// as in UE - and category badges); the picture (the whole model through a
// Synthetic Model preview view at the bind pose, under a CPU overlay of bones as
// lines or octahedra in a draw scope like UE's EBoneDrawMode, joints, names,
// local axes, category colours; a click near a joint selects its bone); Details
// (the bone's rest local TRS, bind position, length, weights, grant (MMD append
// rotation), IK and humanoid roles, issues) and Check (every issue).
//
// Staged edits share one undo ring (Ctrl+Z / Ctrl+Y while focused; one gizmo
// drag = one step) and one Save:
//   * sockets (the model's SO records): listed under their bones (right-click a
//     bone to add or paste; drag onto a bone to reparent, Ctrl keeps the world
//     place), edited in Details or with the gizmo (W / E / R; rotation in
//     RollPitchYaw degrees, the Inspector's order). Save makes every placed
//     instance re-derive them (SkeletonEditorContext::socketsChanged). An .iskel
//     opened without its model has no file to hold sockets: none, read-only.
//   * virtual bones (VB records, written with the sockets by
//     SceneInstanceLoader::ReplaceModelSkeletonRecords): a row under `from`,
//     drawn at `to`'s bind place (dashed line, hollow diamond); a rename carries
//     its sockets, a delete leaves them on `to`. Staging obeys
//     Resource::ApplyVirtualBones (the panel calls it). A placed instance takes a
//     saved change when it spawns again: the panel counts the stale ones
//     (SkeletonEditorContext::staleVirtualBones) and offers a scene reload.
//   * the humanoid profile (the Retarget tab: the Animation Retarget window's role
//     rows, Editor/HumanoidMapping): Save writes the .iskel's humanoid section,
//     the only other file this panel writes, and the World's skeletons made from
//     that .iskel take it at once (SkeletonEditorContext::humanoidChanged). The
//     bone mask preview colours the bones a mask layer would drive
//     (AnimationSystem::BuildBoneMask).
//
// The preview-layer rules (AssetPreviewWindow.h) hold: the orbit is the panel's
// own state, the SRV is re-resolved every frame, the render size is quantised
// and debounced, the view is released at the frame boundary once the panel stops
// drawing, and everything that reads files or touches the Renderer's game-half
// state runs in ProcessPendingActions.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/HierarchyComponents.h"    // PreviewStageTag (the stage's owner)
#include "Editor/BoneWeightHeat.h"
#include "Editor/PreviewStage.h"        // the Animation host's stage
#include "Resource/HumanoidBone.h"      // the humanoid profile, staged
#include "Resource/ModelSockets.h"
#include "Resource/ModelVirtualBones.h"
#include "Resource/SkeletonInspect.h"

#include <DirectXMath.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Renderer;
class IGraphicsDevice;
struct ImDrawList;
struct SkeletonAsset;
namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;
}

namespace Editor
{
    // What the panel needs from EditorLayer, which it cannot reach itself.
    struct SkeletonEditorContext
    {
        Renderer*        renderer    = nullptr;
        IGraphicsDevice* gfx         = nullptr;
        // The Animation host spawns its stage into the main World (only in the game
        // half's serial window - ProcessPendingActions).
        World*                         world       = nullptr;
        Resource::AssetManager*        assetMgr    = nullptr;
        Resource::AnimationClipSystem* animClipSys = nullptr;
        // The live pose's bones via EditorLayer::BuildBoneWorldMatrices (the one FK the
        // main overlay, the Chain Physics Preview and the pickers use): bone -> model
        // matrices and the entity's world matrix.
        std::function<bool(Renderer&, World&, Entity, std::vector<DirectX::XMFLOAT4X4>&, DirectX::XMMATRIX&)>
                         buildBones;
        // The editor frame's real delta (Edit mode's animDt is 0 by design); the stage
        // clip's playhead runs on it.
        float            realDt      = 0.f;
        std::uint64_t    editorFrame = 0;     // EditorLayer::m_editorFrame (the harness clock)
        // World::Epoch(): a reload invalidates the mesh-descriptor slots the
        // composition resolved to, so the panel asks for it again.
        std::uint32_t    worldEpoch  = 0;
        bool             hasWorld    = false;
        bool             playing     = false; // Play mode: saving a model is refused
        // Show a file in the Content Browser.
        std::function<void(const std::string&)> reveal;
        // The model's socket records changed on disk: every placed instance re-derives
        // them (SceneInstanceLoader::RefreshModelSockets). Returns the instances
        // refreshed.
        std::function<std::uint32_t(const std::string&)> socketsChanged;
        // Attach the main scene's selected entity to @p socket of the placed instance
        // of @p model nearest it (one undo step); "" = done, else why not.
        std::function<std::string(const std::string& model, const std::string& socket)> attachSelection;
        // The placed instances of @p model whose skeleton lacks the model file's
        // virtual bones (SceneInstanceLoader::CountStaleVirtualBones).
        std::function<std::uint32_t(const std::string& model)> staleVirtualBones;
        // Reload the main scene so they spawn again (its unsaved-changes prompt
        // applies). False: there is no scene file to reload.
        std::function<bool()> reloadScene;
        // The .iskel's humanoid profile was saved: the registry skeletons that model
        // roots in the World got from it take @p hp. Returns the roots updated.
        // @p fileSkel is the skeleton the profile indexes; a registry skeleton whose
        // real bones differ (loaded before a re-import changed the rig) is skipped.
        std::function<std::uint32_t(const std::string& iskel, const HumanoidProfile& hp, const SkeletonAsset& fileSkel)>
            humanoidChanged;
        // Open the Animation Retarget window aimed at a placed instance of @p model
        // (selected); "" = done, else why not.
        std::function<std::string(const std::string& model)> openRetarget;
    };

    class SkeletonEditorWindow
    {
    public:
        // ---- Bone draw scope (UE's EBoneDrawMode) ----
        enum class BoneScope : std::uint8_t { None, Selected, SelectedAndParents, SelectedAndChildren, Family, All };

        // ---- Overlays (Show) ----
        static constexpr std::uint32_t kShowJoints     = 1u << 0;   // a dot per drawn joint
        static constexpr std::uint32_t kShowOctahedra  = 1u << 1;   // bones as octahedra (else lines)
        static constexpr std::uint32_t kShowNames      = 1u << 2;   // the selected bone's name
        static constexpr std::uint32_t kShowAllNames   = 1u << 3;   // every drawn bone's (declutter grid)
        static constexpr std::uint32_t kShowAxes       = 1u << 4;   // the selected bone's local axes
        static constexpr std::uint32_t kShowAllAxes    = 1u << 5;   // every drawn bone's
        static constexpr std::uint32_t kShowCategories = 1u << 6;   // colour bones by category
        static constexpr std::uint32_t kShowGrid       = 1u << 7;   // a ground grid under the model
        static constexpr std::uint32_t kShowSockets    = 1u << 8;   // the model's sockets
        static constexpr std::uint32_t kShowVirtual    = 1u << 9;   // the model's virtual bones
        static constexpr std::uint32_t kShowGhost      = 1u << 10;  // the stage host: the reference pose, faint
        // A marker on each humanoid role's joint: required roles a filled circle,
        // optional ones a dashed ring (Unity's Avatar). Always drawn while the Retarget
        // tab is shown; this bit draws them under the other tabs too.
        static constexpr std::uint32_t kShowHumanoid   = 1u << 11;
        static constexpr std::uint32_t kShowAll        = (1u << 12) - 1u;

        // ---- Tree filter ----
        enum class TreeFilter : std::uint8_t { All, Weighted, Deform, IK, Twist, Chain, Humanoid, Issues };

        // ---- Right-hand tabs ----
        enum class Tab : std::uint8_t { Details, Check, Animations, Retarget };

        // ---- Sockets ----
        enum class GizmoOp : std::uint8_t { Off, Translate, Rotate, Scale };

        struct SaveReport
        {
            std::string   model;
            bool          written   = false;   // the file changed
            std::uint32_t records   = 0;       // SO records now in the file
            std::uint32_t refreshed = 0;       // placed instances that re-derived them
            std::uint32_t virtualBones = 0;    // VB records now in the file
            std::uint32_t stale     = 0;       // placed instances without them yet (a reload gives them)
            bool          humanoidWritten = false;   // the .iskel's humanoid section was written
            std::uint32_t humanoidPushed  = 0;       // model roots in the World that took it
        };

        // ---- Hosts ----
        enum class Host : std::uint8_t { Reference, Stage };

        // ---- Preview attachments ----
        struct PreviewAttachment
        {
            std::string socket, asset;
            bool operator==(const PreviewAttachment& o) const { return socket == o.socket && asset == o.asset; }
        };

        // ---- Weights and animations ----
        // The compatible-animation list: each bone clip of each .ianim checked
        // against this skeleton by playback's binding (Resource::FitClip - what
        // -assettool=compat says). verdict: fits / rest_pose / no_bone_clip / load_failed.
        struct AnimRow
        {
            std::string   anim, clip, verdict;
            std::uint32_t channels = 0, boundBones = 0, bones = 0, frames = 0;
            float         duration = 0.f;
        };

        // ---- Scripted surface (Lua Editor.SkeletonEditor*, the harness) ----
        struct StateView
        {
            bool          open = false, loaded = false, preview = false;
            std::string   path, model, iskel, error, humanoidSource;
            std::uint32_t viewId = ~0u, renderW = 0, renderH = 0, outputW = 0, outputH = 0;
            std::uint32_t pendingW = 0, pendingH = 0;   // the size after the resize debounce (== render: none waits)
            std::uint32_t lease = 0;              // the view's tenancy (capture {view, lease})
            std::uint64_t renders = 0;
            bool          meshVisible = true;     // Show > Mesh
            int           selected = -1;
            std::string   selectedName;
            std::string   scope, show, filter, search;
            std::uint32_t treeRows = 0;           // rows the tree lists (the filter / search applied)
            std::uint32_t drawnBones = 0, drawnJoints = 0, overlayLines = 0, labels = 0;   // the last overlay
            std::uint32_t drawnSockets = 0;
            Resource::SkeletonStats stats;
            std::uint32_t errors = 0, warnings = 0, infos = 0;
            // Sockets
            std::string   selectedSocket, socketLock, gizmo;
            std::uint32_t sockets = 0;            // staged skeleton sockets
            bool          socketsDirty = false;
            std::size_t   undoDepth = 0, redoDepth = 0;
            std::uint32_t gizmoGestures = 0;
            // Weights and animations
            bool          weightsOn = false, weightsLive = false;
            int           weightsBone = -1;
            std::size_t   weightParts = 0, weightSkipped = 0;
            std::uint64_t weightsRevision = 0;
            std::size_t   animRows = 0, animPending = 0;
            // Virtual bones
            std::string   selectedVirtualBone;
            std::uint32_t virtualBones = 0, virtualBonesApplied = 0, drawnVirtualBones = 0;
            bool          virtualBonesDirty = false, dirty = false;
            std::uint32_t staleInstances = 0;
            int           pickTargetFrom = -1;    // "pick the target in the picture" from this bone
            // Stage host
            std::string   host;                   // reference / stage (the chosen one)
            bool          stageShown = false;     // the stage's view is what is drawn
            bool          stageUp = false, stagePinned = false, stageSharingLane = false;
            Entity        stageRoot = NullEntity, stageSkeleton = NullEntity;
            std::uint32_t stageEntities = 0, stageSpawns = 0, stageViewId = ~0u, pinOwners = 0;
            float         stageOriginY = 0.f;
            std::uint64_t stagePosedFrames = 0, stageRestFrames = 0;   // pose-ring slot / none (anim-culled)
            std::string   stageStatus;
            // The stage's clip, and the last overlay's ghost bones
            std::string   clipPath, clipSource, clipName;
            bool          clipBound = false, clipPlaying = false, clipLooping = true;
            double        clipTime = 0.0;
            float         clipDuration = 0.f, clipFps = 0.f, clipSpeed = 1.f;
            std::uint32_t clipFrames = 0, clipChannels = 0, clipBoundBones = 0, drawnGhost = 0;
            std::uint32_t posedBones = 0;         // temporary posing's bones
            // The staged humanoid profile, the tab shown, the bone mask preview, the last
            // overlay's role markers / bones drawn in the mask.
            std::string   tab;
            bool          humanoidDirty = false, humanoidUserEdited = false;
            std::uint32_t humanoidConvention = 0, humanoidMissing = 0;   // required roles unmapped
            std::string   maskMode, maskRoot;
            bool          maskResolved = true;
            std::uint32_t maskBones = 0, drawnHumanoid = 0, drawnMasked = 0;
        };

        // ---- Lifetime ----
        static SkeletonEditorWindow& Get();

        // ---- Bone draw scope (UE's EBoneDrawMode) ----
        // "none", "selected", "parents", "children", "family", "all".
        static const char* BoneScopeName(BoneScope s);
        static bool        ParseBoneScope(const std::string& s, BoneScope& out);

        // ---- Overlays (Show) ----
        // "joints+octahedra+names+allnames+axes+allaxes+categories+grid+sockets+
        // virtualbones+ghost+humanoid", "all", "none" or a hex mask.
        static bool        ParseShow(const std::string& text, std::uint32_t& out);
        static std::string ShowNames(std::uint32_t bits);

        // ---- Tree filter ----
        // "all", "weighted", "deform", "ik", "twist", "chain", "humanoid", "issues".
        static const char* TreeFilterName(TreeFilter f);
        static bool        ParseTreeFilter(const std::string& s, TreeFilter& out);

        // ---- Right-hand tabs ----
        static const char* TabName(Tab t);                                  // details / check / animations / retarget
        static bool        ParseTab(const std::string& s, Tab& out);
        void RequestTab(Tab t) { m_requestTab = static_cast<int>(t); }   // selected by the next Draw
        Tab  CurrentTab() const { return static_cast<Tab>(m_rightTab); }

        // ---- Lifetime ----
        void Draw(const SkeletonEditorContext& ctx);
        void ProcessPendingActions(const SkeletonEditorContext& ctx);   // GAME half, serial window
        void ReleaseViewsAtShutdown(Renderer* renderer);
        bool  IsOpen() const { return m_open; }
        bool* OpenFlag()     { return &m_open; }

        // ---- Subject ----
        // From a UI handler (a render-side context): queued, applied in
        // ProcessPendingActions.
        void RequestOpen(const std::string& path);
        // GAME half (Lua, harness): now. @p path = a model .iscn (with a K line)
        // or an .iskel. @p code: bad_value / unsupported / load_failed / dirty
        // (unsaved socket / virtual bone / humanoid edits and not @p discard).
        bool OpenNow(const SkeletonEditorContext& ctx, const std::string& path, std::string& code, std::string& why,
                     bool discard = false);
        void Close() { m_open = false; }

        // ---- Scripted surface (Lua Editor.SkeletonEditor*, the harness) ----
        // -1 = none. @p reveal: open the tree to it and scroll there (a pick in the
        // picture, a script); a click in the tree itself does not.
        bool Select(int bone, bool reveal = true);
        bool SelectByName(const std::string& name); // exact, case-sensitive (as clips bind)
        void SetScope(BoneScope s) { m_scope = s; }
        void SetShow(std::uint32_t bits) { m_show = bits & kShowAll; }
        void SetFilter(TreeFilter f) { m_filter = f; m_treeDirty = true; }
        void SetSearch(const std::string& text);
        void SetOrbit(float yaw, float pitch, float distScale);
        void SetPan(float x, float y, float z) { m_pan[0] = x; m_pan[1] = y; m_pan[2] = z; }
        // Show > Mesh (the Reference Pose host; the stage always shows its model): off =
        // every part masked out, the bones over the bare backdrop.
        void SetMeshVisible(bool on) { m_meshVisible = on; }
        // The bone whose joint projects nearest to (@p x, @p y) - the picture's
        // OUTPUT pixels - within 12 px, among the joints the scope draws (all of
        // them when the scope draws none, or @p anyJoint); -1 = none. What a click does.
        int  PickAt(Renderer* renderer, float x, float y, bool anyJoint = false) const;
        // Where @p bone's joint lands in output pixels (false: not composed yet,
        // behind the camera).
        bool ProjectJoint(Renderer* renderer, int bone, float& x, float& y) const;

        // ---- Sockets ----
        static const char* GizmoOpName(GizmoOp op);                       // off / translate / rotate / scale
        static bool        ParseGizmoOp(const std::string& s, GizmoOp& out);

        // The staged records (every SO record of the model, mesh sockets too - they
        // pass through a Save untouched) and the saved ones.
        const std::vector<Resource::ModelSocket>& Sockets() const { return m_sockets; }
        bool SocketsDirty() const { return m_loaded && m_sockets != m_savedSockets; }
        // Why the sockets cannot be edited ("" = they can): no model file holds them.
        std::string SocketLock() const;
        // A skeleton socket by name: its index in Sockets(), -1 = none.
        int  FindSocket(const std::string& name) const;
        int  SelectedSocket() const { return m_selSocket.empty() ? -1 : FindSocket(m_selSocket); }
        bool SelectSocket(const std::string& name);                        // "" = none
        // Staged edits - each one undo step; false + @p why = refused, nothing changed.
        // Add: @p name "" = "<bone>_socket" (made unique); @p outName = the name given.
        bool AddSocket(const std::string& bone, const std::string& name, std::string& outName, std::string& why);
        // Replace the socket named @p name with @p value (a rename when value.name
        // differs; the bone must exist; the name must be legal and unique).
        bool SetSocket(const std::string& name, const Resource::ModelSocket& value, std::string& why);
        bool RemoveSocket(const std::string& name, std::string& why);
        // Onto @p bone: @p keepWorld = keep its place in the bind pose (its offset
        // re-expressed under the new bone), else keep the offset (it moves).
        bool ReparentSocket(const std::string& name, const std::string& bone, bool keepWorld, std::string& why);
        // Several fields at once, all or nothing: onto @p bone first (when given;
        // @p keepWorld as in ReparentSocket), then @p edit on the result (name,
        // offset). One undo step; a refusal leaves the staged lists and the history as
        // they were (Lua SkeletonEditorSocketSet: a bad field changes nothing).
        bool EditSocket(const std::string& name, const std::string* bone, bool keepWorld,
                        const std::function<void(Resource::ModelSocket&)>& edit, std::string& why);
        // Why the humanoid mapping cannot be edited here ("" = it can): a packed or
        // junction .iskel.
        const std::string& HumanoidLock() const { return m_iskelLock; }
        // A scripted gizmo drag (one gesture, one undo step), in the picture's model
        // space: Translate by @p v; Rotate @p deg degrees about the socket's LOCAL
        // axis @p v; Scale by the factors @p v (positive).
        bool GizmoDrag(const std::string& name, GizmoOp op, const DirectX::XMFLOAT3& v, float deg, std::string& why);
        GizmoOp CurrentGizmoOp() const { return m_gizmoOp; }
        void    SetGizmoOp(GizmoOp op);
        std::uint32_t GizmoGestures() const { return m_gizmoGestures; }
        // SO records as text (the clipboard's format) - @p names "" = every skeleton socket.
        std::string CopySockets(const std::vector<std::string>& names) const;
        // The SO records in @p text, onto their own bones (@p toBone "") or all onto
        // @p toBone; a name in use gets a new one. One undo step.
        bool PasteSockets(const std::string& text, const std::string& toBone, std::vector<std::string>& added,
                          std::string& why);
        bool Undo();
        bool Redo();
        void RequestUndo() { ++m_pendingUndo; }
        void RequestRedo() { ++m_pendingRedo; }
        void RequestSave() { m_saveQueued = true; }   // the toolbar's Save, run in PPA
        // Ctrl+C / Ctrl+V while the panel is focused (EditorLayer routes them here
        // instead of the entity clipboard): the selected socket / onto the selected bone.
        void RequestCopy()  { m_copyQueued = true; }
        void RequestPaste() { m_pasteQueued = true; }
        bool OwnsUndoShortcut() const { return m_focused; }
        std::size_t UndoDepth() const { return m_undo.size(); }
        std::size_t RedoDepth() const { return m_redo.size(); }
        // Back to the saved records - sockets, virtual bones and the humanoid profile
        // (one undo step).
        bool Revert();

        // Game half: the SO and VB records in one write (with a model), then the
        // humanoid profile into the .iskel when it has unsaved edits or
        // @p forceHumanoid (an .iskel opened alone saves only that). Refused in Play
        // mode, when a name is illegal / repeated, or when this subject has nothing to
        // save.
        bool SaveNow(const SkeletonEditorContext& ctx, std::string& why, SaveReport* rep = nullptr,
                     bool forceHumanoid = false);
        const std::string& DocumentPath() const { return m_path; }   // as opened

        // ---- Virtual bones ----
        // The model's VB records, staged with the sockets. A virtual bone joins two real
        // bones: its parent is `from`, and every frame it is where `to` is
        // (Resource/ModelVirtualBones.h); at the bind pose, at `to`'s place. The rules
        // are Resource::ApplyVirtualBones': what is staged is what a spawn applies.
        const std::vector<Resource::ModelVirtualBone>& VirtualBones() const { return m_vbs; }
        bool VirtualBonesDirty() const { return m_loaded && m_vbs != m_savedVbs; }
        bool Dirty() const { return SocketsDirty() || VirtualBonesDirty() || HumanoidDirty(); }   // anything unsaved
        int  FindVirtualBone(const std::string& name) const;                // -1 = none
        int  SelectedVirtualBone() const { return m_selVb.empty() ? -1 : FindVirtualBone(m_selVb); }
        bool SelectVirtualBone(const std::string& name);                     // "" = none
        // Why the staged virtual bone @p index is not applied - "" = it is (an orphan,
        // a bone the skeleton lacks, a pair taken twice: ApplyVirtualBones' verdict).
        std::string VirtualBoneProblem(int index) const;
        // Its `from` / `to` bone index (-1: not in the skeleton).
        int  VirtualBoneFrom(int index) const;
        int  VirtualBoneTo(int index) const;
        // Staged edits - each one undo step; false + @p why = refused, nothing changed.
        // Add: @p name "" = UE's "VB <from>_<to>" (made unique); @p outName = the name given.
        bool AddVirtualBone(const std::string& from, const std::string& to, const std::string& name,
                            std::string& outName, std::string& why);
        // The sockets on it follow the new name (the same step).
        bool RenameVirtualBone(const std::string& name, const std::string& newName, std::string& why);
        // The sockets on it move onto its `to` bone with their offset - where they
        // were in every frame (the same step).
        bool RemoveVirtualBone(const std::string& name, std::string& why);
        // Placed instances of the model without the saved virtual bones (checked
        // every 30 editor frames and after a Save).
        std::uint32_t StaleInstances() const { return m_staleInstances; }

        // ---- Hosts ----
        // Reference = the Synthetic Model view at the bind pose (the default; Play too).
        // Stage = the model spawned as a hidden PreviewStage in the main World (origin
        // slot 1, Editor/PreviewStage.h), seen by a WorldCull view: the live pose with
        // the whole pipeline on it (animation, grants, virtual bones, chains in their
        // rest pose - the chain solver's preview lane is the Chain Physics Preview's),
        // under the scene's lighting. One view at a time: switching releases the other
        // host's. The stage exists only while the panel shows it, in Edit mode: Play
        // tears it down before its snapshot and the panel shows the reference pose until
        // Stop brings the stage back. On the stage, the overlay, the picking and the
        // socket gizmo (ApplyLiveMatrix) work on the live pose.
        static const char* HostName(Host h);                              // reference / stage
        static bool        ParseHost(const std::string& s, Host& out);
        void SetHost(Host h) { m_host = h; }
        Host CurrentHost() const { return m_host; }
        // The stage is up and its view drawn: the panel shows the live pose.
        bool StageShown() const;
        // Called by App before the Play snapshot, beside the chain panel's
        // TearDownForPlay.
        void TearDownForPlay(Renderer* renderer, World* world);

        // ---- The stage's clip ----
        // What the Animation host plays on its stage: an .ianim (+ one of its clips by
        // name, "" = its first) or "" = the rest pose. Queued: applied by the next
        // ProcessPendingActions (the clip system's game-half calls). The transport
        // (play, time, speed, loop) acts on the player at once; the playhead runs on the
        // real frame delta and is written onto the stage's AnimationComponent every
        // frame.
        void RequestClip(const std::string& path, const std::string& clip = {});
        StageClipPlayer&       ClipPlayer() { return m_clipPlayer; }
        const StageClipPlayer& ClipPlayer() const { return m_clipPlayer; }

        // ---- Preview attachments ----
        // UE's Add Preview Asset: a model (.iscn) or a prefab (.ipfb) shown on a socket
        // of the Animation host's stage - spawned onto the stage (adopted: torn down with
        // it, never in the scene, never saved into an asset) and following the socket
        // (FollowSocket on the stage root, by socket name: a socket edit moves it at
        // once). The list is the user's, per model:
        // WritableDataDir()/cache/skeleton_editor/<model key>.json.
        const std::vector<PreviewAttachment>& Attachments() const { return m_attachments; }
        // false + @p why: no model, not an .iscn / .ipfb, no such file, no such socket, already there.
        bool AddAttachment(const std::string& socket, const std::string& asset, std::string& why);
        // @p asset "" = every attachment of @p socket. False: none matched.
        bool RemoveAttachment(const std::string& socket, const std::string& asset);
        // Move an attachment onto another socket of the list (one whose socket is
        // gone - removed, or renamed by an undo / a revert / the file - is listed under
        // the selected socket with "Move here"). False + @p why: no such attachment /
        // socket, or @p toSocket has it already.
        bool MoveAttachment(const std::string& socket, const std::string& asset, const std::string& toSocket,
                            std::string& why);
        // The one on the stage now: its root (NullEntity: not spawned), why not.
        Entity      AttachmentRoot(std::size_t index) const;
        std::string AttachmentError(std::size_t index) const;
        // Spawned, but its socket is not on the stage (gone, or on a virtual bone not
        // saved yet): parked at the stage's origin, far below the scene - never at the
        // world origin, where the ray-traced passes would see it - until the socket
        // comes back.
        bool        AttachmentParked(std::size_t index) const;

        // ---- Temporary posing ----
        // UE's bone manipulation: a stage bone turned by a rotation in its own frame
        // (degrees, the Inspector's RollPitchYaw order) on top of whatever poses it -
        // PreviewPoseOverrideComponent on the stage's skeleton, applied after the
        // chains. Kept across clip changes; never written anywhere. @p deg all zero
        // removes that bone's pose.
        void SetBonePose(int bone, const DirectX::XMFLOAT3& deg);
        void ResetPose();
        const std::vector<std::pair<int, DirectX::XMFLOAT3>>& PoseEdits() const { return m_poseEdits; }

        // ---- Humanoid mapping (the Retarget tab) ----
        // The skeleton's humanoid profile (retargeting, an AnimLayer's Upper / Lower
        // Body mask), staged here with the sockets and virtual bones (one undo ring):
        // the .iskel's when it maps a role, else the runtime's lazy build. Edited as the
        // Animation Retarget window edits one (Editor/HumanoidMapping.h); Save writes
        // the .iskel's humanoid section, and the World's skeletons made from this .iskel
        // take it at once. Real bones only.
        const HumanoidProfile& Humanoid() const;
        bool HumanoidDirty() const;                    // edited and not saved
        // Each one undo step; false + @p why = refused, nothing changed. @p bone -1 = unmapped.
        bool SetHumanoidRole(HumanoidBone role, int bone, std::string& why);
        bool SetHipsTranslation(int bone, std::string& why);
        bool AutoDetectHumanoid();                     // userEdited = 0, the builder's map
        bool ClearHumanoid();                          // nothing mapped, userEdited = 1
        // Select the bone @p role is mapped to (false: unmapped).
        bool SelectRole(HumanoidBone role);

        // ---- Bone mask preview ----
        // The bones an AnimLayer with this mask drives - AnimationSystem::BuildBoneMask
        // on the staged skeleton (its humanoid profile included): coloured in the
        // picture, an M badge in the tree. @p mode: -1 = off, else BoneMaskMode (0 full,
        // 1 upper, 2 lower, 3 custom - @p root the bone's name).
        static const char* MaskModeName(int mode);               // off / full / upper / lower / custom
        static bool        ParseMaskMode(const std::string& s, int& out);
        void SetMask(int mode, const std::string& root);
        int                MaskMode() const { return m_maskMode; }
        const std::string& MaskRoot() const { return m_maskRoot; }
        bool               MaskResolved() const { return m_maskResolved; }   // false: the full-body fallback
        std::uint32_t      MaskBones() const;
        bool               InMask(int bone) const;
        // The mask's bones: the real ones, then the staged virtual bones (the runtime's
        // indices - Skeleton.BoneMask's). 0 = no mask.
        std::uint32_t      MaskSize() const { return static_cast<std::uint32_t>(m_maskW.size()); }
        // The Inspector's AnimLayer "Preview": open @p model (unless it is the subject)
        // and show that layer's mask. From a UI handler: queued.
        void RequestMaskPreview(const std::string& model, int mode, const std::string& root);

        // ---- Weights and animations ----
        // The weight heatmap: while on, the skinned parts are coloured by the selected
        // bone's weights (Editor/BoneWeightHeat).
        void SetWeights(bool on) { m_weightsOn = on; }
        bool WeightsOn() const { return m_weightsOn; }

        // Now (the GAME half): every .ianim under @p folder (two levels down; "" =
        // the model's folder) and @p files, the list replaced. The panel's own scan
        // does the same a few files per frame.
        bool ScanAnimations(const std::string& folder, const std::vector<std::string>& files, std::string& why);
        const std::vector<AnimRow>& AnimationRows() const { return m_animRows; }

        // The staged socket's place in the bind pose, the model's space (false: its
        // bone is not in the skeleton).
        bool SocketModelMatrix(int index, DirectX::XMFLOAT4X4& out) const;
        // The socket whose diamond projects nearest to (@p x, @p y) - output pixels -
        // within 12 px; -1 = none.
        int  PickSocketAt(Renderer* renderer, float x, float y) const;
        bool ProjectSocket(Renderer* renderer, int index, float& x, float& y) const;

        // Everything a script reads (Editor.SkeletonEditor()).
        StateView State(Renderer* renderer) const;
        const Resource::SkeletonReport& Report() const { return m_report; }
        // The loaded skeleton (null when none) - read-only.
        const SkeletonAsset* Skeleton() const { return m_skel.get(); }

        // An offline PNG of the overlay layer (software raster, never a capture of
        // ImGui) at the view's output size. Returns lit pixels, -1 on failure.
        long long WriteOverlayPng(Renderer* renderer, const std::string& path, std::string& why);

        // ---- Harness (-skeleton_editor*) ----
        // Reads every -skeleton_editor* flag; false + @p why = a precondition
        // failure (exit 12). Called once from App at startup.
        bool HarnessConfigure(std::string& why);
        // True once a harness step could not happen - EditorLayer makes it exit 12.
        bool HarnessFailed() const { return m_harnessFailed; }

    private:
        struct WeightStreams;

        struct Staged
        {
            std::vector<Resource::ModelSocket>      sockets;
            std::vector<Resource::ModelVirtualBone> vbs;
            HumanoidProfile                         humanoid;

            // All three, always: a snapshot missing one would restore a default on undo
            // (e.g. an empty humanoid mapping).
            Staged(std::vector<Resource::ModelSocket> s, std::vector<Resource::ModelVirtualBone> v,
                   const HumanoidProfile& h)
                : sockets(std::move(s)), vbs(std::move(v)), humanoid(h) {}
        };

        struct LiveFrame
        {
            bool                             valid = false;
            std::vector<float>               joints, axes;   // xyz / 3 x xyz per bone
            std::vector<DirectX::XMFLOAT4X4> boneWorld;
            DirectX::XMFLOAT4X4              rootWorld{};
            DirectX::XMFLOAT4X4              view{}, proj{};
            std::uint32_t                    w = 0, h = 0;   // the view's size the projection is for
            // The selected bone's animated local pose (AnimationSystem's pose cache,
            // copied in the serial window; Details reads only this).
            int                              selBone = -1;
            bool                             selValid = false;
            DirectX::XMFLOAT3                selPos{}, selScl{};
            DirectX::XMFLOAT4                selRot{};
        };

        struct AttachmentLive
        {
            PreviewAttachment         what;
            Entity                    root = NullEntity;
            std::vector<EntityHandle> handles;
            std::string               error;
            bool                      parked = false;   // its socket is not on the stage
        };

        struct OverlayStats
        {
            std::uint32_t bones = 0, joints = 0, lines = 0, labels = 0, sockets = 0, virtualBones = 0, ghost = 0;
            std::uint32_t humanoid = 0, masked = 0;   // role markers, drawn bones in the mask
        };

        // ---- Harness schedule (editor frames) ----
        struct Step
        {
            std::uint64_t at = 0;
            int           kind = 0;   // a StepKind (SkeletonEditorWindow.cpp)
            std::string   text;
            std::uint32_t value = 0;
            bool          done = false;
        };

        // ---- Lifetime ----
        SkeletonEditorWindow();
        ~SkeletonEditorWindow();

        // ---- Subject ----
        bool LoadSubject(const SkeletonEditorContext& ctx, const std::string& path, std::string& code,
                         std::string& why);
        // Rebuild m_report (and what derives from it) from the staged skeleton.
        void ReInspect();

        // ---- Selection / tree ----
        // The filter and @p searchLower (ASCII-lowered) both pass @p bone.
        bool Matches(int bone, const std::string& searchLower) const;
        void RebuildRows();
        void DrawTree();
        void DrawTreeNode(int bone);
        void DrawBadges(int bone) const;
        void DrawDetails(const SkeletonEditorContext& ctx);
        void DrawCheck();
        void DrawStatsLine() const;

        // ---- View state ----
        void ResetView();
        void UpdateView(const SkeletonEditorContext& ctx);
        void DrawToolbar(const SkeletonEditorContext& ctx);
        void DrawViewport(const SkeletonEditorContext& ctx);

        // ---- Sockets ----
        int  BoneIndex(const std::string& name) const;   // a REAL bone; -1 = none
        void PushUndo();
        // The staged socket list becomes @p next (validated; one undo step unless
        // @p pushUndo is false - the caller already pushed one); @p what is logged.
        bool StageSockets(std::vector<Resource::ModelSocket> next, const char* what, std::string& why,
                          bool pushUndo = true);
        // Both lists at once (an edit of a virtual bone moves its sockets too).
        bool StageRecords(std::vector<Resource::ModelSocket> sockets, std::vector<Resource::ModelVirtualBone> vbs,
                          const char* what, std::string& why, bool pushUndo = true);
        std::string UniqueSocketName(const std::string& base) const;
        std::vector<int> SocketsOfBone(int bone) const;
        void DrawSocketRow(int index);
        void DrawSocketDetails(const SkeletonEditorContext& ctx);
        void DrawRenamePopup();
        void DrawAskPopup(const SkeletonEditorContext& ctx);
        void DrawSocketGizmo(Renderer* renderer, float x, float y, float w, float h);
        void ReleaseGizmo();
        // The gizmo's display matrix (the socket in the picture's unit box) and back.
        bool SocketDisplayMatrix(int index, const float centre[3], float scale, DirectX::XMFLOAT4X4& out) const;
        bool ApplyDisplayMatrix(int index, const DirectX::XMFLOAT4X4& display, const float centre[3], float scale,
                                GizmoOp op);
        // The stage host's counterpart: the socket's new world matrix on the live pose.
        bool ApplyLiveMatrix(int index, const DirectX::XMFLOAT4X4& world, GizmoOp op);

        // ---- Weights and animations ----
        std::vector<std::string> ListAnimations(const std::string& folder) const;
        void CheckAnimation(const std::string& path);   // appends its rows
        void DrawAnimations();

        // ---- Virtual bones ----
        // Resource::ApplyVirtualBones' verdict on each record of @p vbs, applied in
        // order to a copy of the loaded skeleton (what a spawn does): "" = applied.
        std::vector<std::string> VirtualBoneVerdicts(const std::vector<Resource::ModelVirtualBone>& vbs) const;
        void RefreshVirtualBones();
        // The REAL bone whose bind place @p boneName has: a bone itself; an applied
        // staged virtual bone, its `to`. -1 = neither. What a socket's bone resolves to.
        int  BindBoneOf(const std::string& boneName) const;
        // A post-IK grant (MMD append) target: AnimationSystem copies its rotation
        // after IK, after the virtual bones were posed, so a virtual bone following it
        // misses that rotation.
        bool PostIKGrant(int bone) const;
        std::string UniqueVirtualBoneName(const std::string& base) const;
        void DrawVirtualBoneRow(int index);
        void DrawVirtualBoneDetails(const SkeletonEditorContext& ctx);
        // UE's "Add Virtual Bone > Pick Target Bone": a searchable list of the real
        // bones; a click adds the virtual bone from @p from and selects it.
        void DrawVirtualBoneTargets(int from);

        // ---- Stage sockets, attachments, posing and clip ----
        // Parked / back on the socket as the stage root's sockets say, every tick.
        void ParkAttachments(const SkeletonEditorContext& ctx);
        // A socket renamed between @p before and @p after (one name gone, one new - a
        // rename, its undo or redo): its attachments go with it.
        void CarryAttachments(const std::vector<Resource::ModelSocket>& before,
                              const std::vector<Resource::ModelSocket>& after);

        void TickPose(const SkeletonEditorContext& ctx);
        std::string AttachmentsFile() const;
        void LoadAttachments();
        void SaveAttachments() const;
        void TickAttachments(const SkeletonEditorContext& ctx);
        void DrawAttachments(const std::string& socket);   // the socket Details' section

        void DrawTransport();

        // ---- Humanoid mapping and bone mask ----
        // A humanoid edit: @p edit changes the staged skeleton's profile (it may draw the
        // role rows - one frame's UI); a change is one undo step, and the report and the
        // mask follow. False: nothing changed. @p what is logged.
        bool EditHumanoid(const char* what, const std::function<void(SkeletonAsset&)>& edit);
        // Undo / redo / revert put @p hp back (the report and the mask follow a change).
        void RestoreHumanoid(const HumanoidProfile& hp);
        void RefreshMask();
        void DrawRetarget(const SkeletonEditorContext& ctx);
        void DrawMaskPreview();
        void DrawHumanoidRoleMenu(int bone);   // the tree's context menu: this bone's roles
        // ---- The Animation host's stage ----
        // The stage mode's condition as Draw sees it (the model can have a stage now).
        bool StageMode(bool playing) const;
        void TickStage(const SkeletonEditorContext& ctx);
        void TearDownStage(const SkeletonEditorContext& ctx, const char* reason);
        void BuildLiveFrame(const SkeletonEditorContext& ctx);
        // The view drawn now (the stage's, else the reference host's) - ~0u = none.
        std::uint32_t CurrentView() const;
        // The frame of the host drawn now, projected into a (w x h) picture: the joints /
        // axes (the reference host's bind pose in MODEL space, the stage's live pose in
        // WORLD space), @p toClip their space -> clip, @p toFrame model space -> their
        // space (identity / the stage root's world). False: nothing composed / framed yet.
        bool FrameFor(Renderer* renderer, float w, float h, DirectX::XMFLOAT4X4& toClip,
                      DirectX::XMFLOAT4X4& toFrame, const float*& joints, const float*& axes) const;
        // The socket's place in the host's frame space.
        bool SocketFrameMatrix(int index, DirectX::XMFLOAT4X4& out) const;

        // ---- Overlays (Show) ----
        // The drawn set for the current scope / selection.
        void DrawnBones(std::vector<std::uint8_t>& out) const;

        // Projected CPU overlay into @p dl over the picture rect (x, y, w, h).
        OverlayStats DrawOverlay(Renderer* renderer, ImDrawList* dl, float x, float y, float w, float h) const;

        // ---- Harness schedule (editor frames) ----
        void TickHarness(const SkeletonEditorContext& ctx);

        // ---- Subject ----
        std::string                    m_path;     // as opened
        std::string                    m_model;    // the .iscn the picture composes ("" = no picture)
        std::string                    m_iskel;
        std::unique_ptr<SkeletonAsset> m_skel;     // MAX_BONES-sized: heap
        Resource::SkeletonReport       m_report;
        std::string                    m_humanoidSource;   // file | built | none
        bool                           m_loaded = false;
        std::string                    m_error;
        // Bind pose, the model's space: joint positions (xyz) and local axes
        // (3 x xyz, unit) per bone; children lists.
        std::vector<float>             m_joints, m_axes;
        std::vector<std::vector<int>>  m_children;
        float                          m_modelSize = 1.f;
        float                          m_boundsMin[3] = { 0.f, 0.f, 0.f }, m_boundsMax[3] = { 0.f, 0.f, 0.f };
        std::vector<std::string>       m_names;         // UTF-8, at most 64 bytes
        std::vector<std::string>       m_namesLower;    // for the search
        std::vector<std::vector<int>>  m_boneIssues;    // [bone] -> indices into m_report.issues

        // The .iskel's blend streams, kept so a humanoid edit re-inspects the skeleton
        // exactly as a load does (the report is always InspectSkeleton of what is
        // staged).
        std::unique_ptr<WeightStreams> m_streams;
        HumanoidProfile                m_savedHumanoid;  // as opened / last saved
        // Why the model's records / the .iskel's humanoid section cannot be written
        // here ("" = they can): not a loose file (packed), or another checkout's (a
        // junction) - the Mesh Editor's rule. Decided at open.
        std::string                    m_modelLock, m_iskelLock;
        // The .iskel's size + write time as opened / last saved by this panel: a
        // humanoid Save refuses a file that changed since (a re-import, the Retarget
        // window), since its bone indices were read from the old one. A clean panel
        // reopens it.
        std::string                    m_iskelStamp;
        bool                           m_iskelChangedSaid = false;

        // ---- Selection / tree ----
        int                 m_selected = -1;
        bool                m_revealSelected = false;   // open its ancestors + scroll once
        TreeFilter          m_filter = TreeFilter::All;
        char                m_search[128] = {};
        std::string         m_searchLower;
        bool                m_treeDirty = true;
        std::vector<int>    m_rows;                     // flat rows while searching / filtering
        bool                m_showIndex = false;

        // ---- View state ----
        bool          m_open = false;
        BoneScope     m_scope = BoneScope::All;
        std::uint32_t m_show = kShowJoints | kShowOctahedra | kShowNames | kShowAxes | kShowGrid | kShowSockets |
                               kShowVirtual | kShowGhost;
        float         m_yaw = 0.f, m_pitch = 0.f, m_dist = 1.f, m_pan[3] = { 0.f, 0.f, 0.f };
        int           m_background = 1;   // Preview::Background (Checker)
        bool          m_shadow = true;
        bool          m_meshVisible = true;
        int           m_rightTab = 0;     // Tab: 0 details, 1 check, 2 animations, 3 retarget
        int           m_requestTab = -1;  // a Tab to select on the next Draw

        // ---- Sockets ----
        std::vector<Resource::ModelSocket>             m_sockets, m_savedSockets;   // staged / as in the file

        // One undo ring for sockets, virtual bones (a virtual bone's edit moves its
        // sockets in the same step) and the humanoid profile.
        std::deque<Staged> m_undo, m_redo;
        int           m_pendingUndo = 0, m_pendingRedo = 0;
        bool          m_focused = false;
        std::string   m_selSocket;                               // by NAME: an edit that shifts indices keeps it
        std::vector<std::pair<std::string, int>> m_boneByName;   // sorted by name: the name -> index lookup
        std::vector<std::vector<int>> m_socketsByBone;           // the tree's rows, rebuilt each draw
        std::vector<int>              m_socketsNoBone;           // skeleton sockets whose bone is missing
        // Details: one widget gesture = one undo step; the Rotation field's degrees
        // are kept while it edits the same socket (no Euler jitter near gimbal lock).
        bool          m_detailsGesture = false;
        int           m_eulerFor = -1;
        DirectX::XMFLOAT4 m_eulerQ = { 0.f, 0.f, 0.f, 1.f };
        float         m_euler[3] = { 0.f, 0.f, 0.f };
        char          m_nameEdit[64] = {};
        int           m_nameEditFor = -1;
        std::string   m_nameError;
        char          m_boneSearch[64] = {};
        // The gizmo (the Mesh Editor's collision-gizmo recipe).
        GizmoOp       m_gizmoOp = GizmoOp::Translate;
        bool          m_gizmoUsing = false, m_gizmoOver = false, m_gizmoPushed = false, m_orbitActive = false;
        int           m_gizmoSocket = -1;
        DirectX::XMFLOAT4X4 m_gizmoMatrix = {};
        std::uint32_t m_gizmoGestures = 0;
        // Unsaved edits (sockets, virtual bones or the humanoid profile): a UI open /
        // close asks first (1 = close, 2 = open m_queuedPath).
        int           m_askFor = 0;
        bool          m_askOpenPopup = false;
        bool          m_saveQueued = false;
        bool          m_closeAfterSave = false;
        bool          m_openAfterSave = false;
        bool          m_openDiscard = false;   // the queued open goes ahead over unsaved edits
        bool          m_copyQueued = false, m_pasteQueued = false;
        std::string   m_lastSave;   // the last Save's result line (shown in the toolbar)
        // Rename popup (F2 / the context menu): a socket, or (m_renameVb) a virtual bone.
        int           m_renameFor = -1;
        bool          m_renameVb = false;
        char          m_renameBuf[64] = {};
        bool          m_renameOpen = false;

        // ---- Weights and animations ----
        BoneWeightHeat           m_weights;
        bool                     m_weightsOn = false;
        std::vector<AnimRow>     m_animRows;
        std::vector<std::string> m_animQueue;     // files the panel's scan still has to read
        std::size_t              m_animTotal = 0;
        std::string              m_animScanned;   // what the list is of (a folder, "files")
        char                     m_animFolder[260] = {};
        char                     m_animFile[260]   = {};
        bool                     m_animOnlyFits = false;
        bool                     m_animAutoScanned = false;

        // ---- Virtual bones ----
        std::vector<Resource::ModelVirtualBone> m_vbs, m_savedVbs;   // staged / as in the file
        // Per staged virtual bone, refreshed with the list (RefreshVirtualBones): why it
        // is not applied ("" = it is), its bones (-1: not in the skeleton).
        std::vector<std::string>      m_vbProblems;
        std::vector<int>              m_vbFrom, m_vbTo;
        std::string                   m_selVb;             // by NAME, as m_selSocket
        std::vector<std::vector<int>> m_vbsByBone;         // the tree's rows under each `from`, rebuilt each draw
        std::vector<int>              m_vbsNoBone;         // their `from` is not in the skeleton
        std::vector<std::vector<int>> m_socketsByVb;       // sockets whose bone is an applied virtual bone
        int           m_pickTargetFrom = -1;               // the next joint clicked in the picture is the target
        char          m_vbSearch[64] = {};
        char          m_vbNameEdit[64] = {};
        int           m_vbNameEditFor = -1;
        std::string   m_vbNameError;
        std::uint32_t m_staleInstances = 0;

        // ---- The Animation host's stage ----
        Host             m_host = Host::Reference;
        PreviewStageHost m_stage{ { PreviewStageTag::kSkeletonEditor, kStageSlotSkeleton, "[SKELEDITOR]",
                                    "[SkeletonEditor] ", "__SkeletonEditorCam" } };
        std::string      m_stageStatus;          // why there is no stage ("" = nothing to say)
        std::string      m_stageRefusedFor;      // the model a spawn was refused for: not retried until it changes
        bool             m_stageDrawn = false;   // Draw showed the Stage host (read in the next PPA)
        bool             m_stageRespawn = false; // a Save changed the virtual bones: spawn the model again
        std::uint32_t    m_stageEpoch = 0;
        World*           m_stageWorld = nullptr;
        std::uint64_t    m_stagePosed = 0, m_stageRest = 0, m_stageTicks = 0;

        // The live frame (PPA; the overlay, the picking and the state read only it): the
        // real bones' joints and local axes and bone -> world matrices in WORLD space, the
        // stage root's world matrix (the grid), and the camera the panel gave the stage.
        LiveFrame        m_live;

        // ---- Stage sockets, attachments, posing and clip ----
        std::vector<Resource::ModelSocket> m_stageSocketsDerived;   // the list the stage's sockets were derived from
        // Preview attachments: the list, and what is on the current stage.
        std::vector<PreviewAttachment> m_attachments;

        std::vector<AttachmentLive> m_attachLive;
        bool             m_attachDirty = false;   // the list or the stage changed: spawned again
        char             m_attachEdit[260] = {};

        // "Attach Selected Entity Here": Draw queues it, PPA attaches (a component
        // and a scene undo step: never from Draw).
        bool             m_attachSelQueued = false;
        std::string      m_attachSelSocket;
        // Temporary posing: bone -> degrees; written onto the stage when changed.
        std::vector<std::pair<int, DirectX::XMFLOAT3>> m_poseEdits;
        bool             m_poseDirty = false;

        StageClipPlayer  m_clipPlayer{ "__ske_", "[SKELEDITOR]" };
        bool             m_clipRequested = false;
        std::string      m_clipReqPath, m_clipReqName;

        // ---- Humanoid mapping and bone mask ----
        char             m_roleFilter[64] = {};      // the role rows' bone-picker search
        std::string      m_retargetMsg;              // the last "Open Animation Retarget" refusal
        bool             m_retargetQueued = false;   // the button: opened by the next PPA (the World)
        bool             m_forceHumanoidSave = false;
        int              m_maskMode = -1;            // -1 off, else BoneMaskMode
        std::string      m_maskRoot;
        std::vector<float> m_maskW;                  // BuildBoneMask's weights (1 = in)
        bool             m_maskResolved = true;
        // RequestMaskPreview: applied once the queued open has loaded that model.
        bool             m_maskQueued = false;
        int              m_maskQueuedMode = -1;
        std::string      m_maskQueuedRoot, m_maskQueuedModel;

        // ---- Overlays (Show) ----
        mutable OverlayStats m_lastOverlay;

        // ---- Preview view (AssetPreviewWindow's lifecycle) ----
        std::uint32_t m_viewId = ~0u;
        bool          m_visibleThisFrame = false;
        bool          m_releaseQueued    = false;
        // The size asked for; 0x0 = the picture's box not measured yet this showing, and
        // nothing is asked for until DrawViewport measured it (UpdateView).
        std::uint32_t m_renderW = 0, m_renderH = 0;
        std::uint32_t m_pendW = 0, m_pendH = 0;     // the measured size a resize waits to switch to
        double        m_pendAt = 0.0;
        std::uint32_t m_harnessW = 0, m_harnessH = 0;
        std::uint32_t m_worldEpoch = 0;
        bool          m_worldEpochSeen = false;
        bool          m_recomposeQueued = false;

        // Queued from the UI (render-side context) for the game half.
        bool          m_openQueued = false;
        std::string   m_queuedPath;
        bool          m_focusQueued = false;   // RequestOpen: the next Draw brings the window to the front

        // ---- Harness schedule (editor frames) ----
        std::vector<Step> m_steps;
        std::string   m_harnessOpen;
        bool          m_harnessOpened = false;
        bool          m_harnessFailed = false;
        bool          m_harnessOverlayOff = false;   // -skeleton_editor_overlay_off (reverse arm)
        std::uint64_t m_rendersAtDesc = 0;
        std::uint64_t m_lastRenders = 0, m_lastRendersFrame = 0;
        std::string   m_lastDescKey;
    };
}
