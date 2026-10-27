#pragma once

// PreviewStage - an editor panel's HIDDEN PREVIEW STAGE in the main World: a
// character spawned from a prefab far below the scene, seen only by the panel's
// WorldCull preview view and kept out of the scene paths (save, undo, the editor's
// entry points, the main view, shadows, probes). It stays in the TLAS: with
// renderInMainPass true, RT reflection and RTAO rays can still hit it. The
// mechanics every stage owner shares, so each fix lands once:
//
//   LoadStageSubject      LoadPrefab (.ipfb) or SceneInstanceLoader::Load (a
//                         model .iscn) - never RecordSpawn - and the EXACT set of
//                         entities it created: a DIFF of the alive set, not the
//                         root's Children (SavePrefab writes FollowSocket /
//                         FollowEntity followers as TOP-LEVEL nodes).
//   StripStage            the authored components a preview must not run
//                         (Jolt, character controller, AI, Lua, audio, the
//                         player tag, per-object / hair shadow opt-ins, the
//                         animation writers beside the panel's clock, VFX).
//                         KEPT: CapsuleColliderComponent (the chain solver's).
//   TagStage              SceneTransientTag (not saved) + PreviewStageTag{owner}
//                         (the editor's entry points test the tag pool, whoever
//                         the owner).
//   ApplyStageViewPolicy  off the main camera, the shadow cascades and the
//                         reflection probes; castShadow off; renderInMainPass
//                         stays TRUE (CullWorldForPreview tests it before the
//                         view bit).
//   MeasureStageBudget    the four per-frame caps against the LIVE headroom -
//                         refuse and name the cap, never truncate.
//   DestroyStageEntities  DestroySubtree of each stage entity whose parent is not
//                         in the stage, through generation-stamped handles (a
//                         recycled id is somebody else's), the skinned buffers
//                         released first.
//   CreateStageCamera     a VirtualCameraComponent entity - NO CameraComponent
//                         (App::RefreshMainCamera would adopt it in a scene with
//                         no ActiveCameraTag).
//
// Origin slots: stage slot k sits at y = -4096 - 512 k (x = z = 0) - the Chain
// Physics Preview uses slot 0, the Skeleton Editor slot 1 - so two stages never
// overlap, occlude or pick each other.
//
// Everything here runs in the game half's serial window (a panel's
// ProcessPendingActions): it creates and destroys entities.
//
// Editor-only TU: compiled into EditorCore.

#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"   // Resource::AnimHandle (StageClipPlayer)

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

class Renderer;
struct SkeletonAsset;
namespace Resource
{
    class AssetManager;
    class AnimationClipSystem;
}

namespace Editor
{
    // ==== origin slots ====
    constexpr float kStageOriginY0    = -4096.f;   // slot 0 (the Chain Physics Preview's)
    constexpr float kStageSlotStride  = 512.f;
    constexpr int   kStageSlotChain   = 0;
    constexpr int   kStageSlotSkeleton = 1;
    constexpr float StageOriginY(int slot) { return kStageOriginY0 - kStageSlotStride * static_cast<float>(slot); }

    // The entities of World @p w carrying PreviewStageTag of @p owner.
    std::uint32_t StageTagCount(World& w, std::uint8_t owner);

    // ==== spawn ====
    struct StageSpawn
    {
        Entity                     root = NullEntity;
        std::vector<Entity>        ents;   // every entity the load created (the root included)
        std::unordered_set<Entity> set;    // the same, for lookups
    };
    // Load @p path into @p world - an .ipfb through LoadPrefab, a model .iscn through
    // SceneInstanceLoader::Load (the renderer's mesh library; what the viewport drop
    // places) - and take the entities it created. NullEntity root = it failed (the
    // loader logged why).
    StageSpawn LoadStageSubject(const std::string& path, World& world, Resource::AssetManager& assetMgr,
                                Renderer* renderer, Resource::AnimationClipSystem* animClipSys);

    // Strip the authored components a preview must not run; returns how many were
    // removed. Logs "<logPrefix> strip: removed=... keptCapsuleCollider=N" (the packs'
    // field names). @p skip (a reverse arm): nothing is removed, a WARNING says so.
    std::uint32_t StripStage(World& world, const std::unordered_set<Entity>& stage, const char* logPrefix,
                             bool skip, const char* skipFlag);

    // SceneTransientTag (added when missing) + PreviewStageTag{owner} on every entity.
    void TagStage(World& world, const std::vector<Entity>& ents, std::uint8_t owner);

    // Every stage entity gets a VisibilityComponent (a missing one reads "visible
    // everywhere"), then: MainCamera / ShadowAny / ReflectionProbe bits off,
    // SecondaryView on, castShadow off, renderInMainPass on.
    void ApplyStageViewPolicy(World& world, const std::vector<Entity>& ents, const std::unordered_set<Entity>& set);

    // ==== budgets ====
    struct StageBudget
    {
        // The stage's.
        std::uint32_t instances = 0, skinJobs = 0, bones = 0, verts = 0, sources = 0, skeletons = 0;
        // The rest of the World's (skin jobs / bones / verts: its WORST CASE - every
        // skinned part and skeleton animated in one frame; instances: the last
        // frame's staged total).
        std::uint32_t sceneInstances = 0, sceneSkinJobs = 0, sceneBones = 0, sceneVerts = 0;
        // The per-frame caps.
        std::uint32_t capInstances = 0, capSkinJobs = 0, capBones = 0, capVerts = 0;
    };
    // Fills @p out; returns the name of the first cap the scene plus the stage would
    // exceed ("instances (Renderer::m_maxInstances)", ...), nullptr = it fits.
    const char* MeasureStageBudget(World& world, Renderer& renderer, const std::unordered_set<Entity>& stage,
                                   StageBudget& out);

    // ==== teardown ====
    // Destroys the stage through @p handles (only still-valid ones are ours) and
    // returns the entities destroyed. @p markBuffers: MarkSkinnedBuffersUnsnapshotted
    // on each first, so their skinned buffers are released rather than parked on the
    // editor's orphan list - safe because a stage is never in a Play snapshot nor
    // undo-parked.
    std::uint32_t DestroyStageEntities(World& world, Renderer* renderer, const std::vector<EntityHandle>& handles,
                                       bool markBuffers);

    // A stage camera entity named @p name: LocalTransform, GlobalTransform, a
    // VirtualCameraComponent (the studio rig's fov, kStageCameraNear / Far) and the tags.
    constexpr float kStageCameraNear = 0.05f;
    constexpr float kStageCameraFar  = 200.f;
    Entity CreateStageCamera(World& world, const char* name, std::uint8_t owner);

    // ==== one owner's stage ====
    // The Skeleton Editor's Animation host owns one: the whole lifecycle on top of the
    // functions above - spawn at the owner's origin slot, the anim pin (this object is
    // one of the Renderer's pin owners), the WorldCull view and its camera entity, the
    // epoch drop, the teardown. The Chain Physics Preview keeps its own state and calls
    // the functions above from its lifecycle (its A/B, floor, driver and clock
    // interleave with them).
    class PreviewStageHost
    {
    public:
        struct Desc
        {
            std::uint8_t owner      = 0;
            int          slot       = 0;
            const char*  logPrefix  = "";   // e.g. "[SKELEDITOR]"
            const char*  rootPrefix = "";   // the root's name prefix, e.g. "[SkeletonEditor] "
            const char*  cameraName = "";   // e.g. "__SkeletonEditorCam"
        };
        explicit PreviewStageHost(const Desc& d) : m_desc(d) {}

        // Spawn @p subject (.ipfb or .iscn, LoadStageSubject) with its root at the origin
        // slot: strip, tag, view policy, the budget (refused and named), the pinned set
        // (every stage skeleton). The PRIMARY skeleton is the first stage
        // SkeletonComponent in the root's depth-first order. false + @p why: nothing left.
        bool Spawn(World& world, Renderer& renderer, Resource::AssetManager& assetMgr,
                   Resource::AnimationClipSystem* animClipSys, const std::string& subject, std::string& why);
        // Destroy the stage and the camera (through their handles), release the view,
        // unpin. @p world null (shutdown): the ids are only dropped - the World's own
        // teardown takes the entities.
        void TearDown(Renderer* renderer, World* world, const char* reason);
        // The World was cleared / adopted (World::Epoch): the ids name nothing now - or
        // somebody else's entities. Dropped, NOTHING destroyed; the view is kept.
        void ForgetEntities();
        // Handles destroyed from outside: out of every set now. False = the root or the
        // primary skeleton went - the caller tears the stage down.
        bool Validate(World& world);
        // Extra entities onto the stage (a preview attachment, LoadStageSubject's
        // result): stripped, tagged, the view policy, their skeletons pinned, torn down
        // with the stage. Returns their handles (for Drop).
        std::vector<EntityHandle> Adopt(World& world, StageSpawn&& extra);
        // Destroys the adopted entities in @p handles ahead of the stage (an attachment
        // removed) and takes them out of every list.
        void Drop(World& world, Renderer* renderer, const std::vector<EntityHandle>& handles);

        // The anim pin: while registered, this object is one of the Renderer's owners.
        void Pin(Renderer& renderer);
        void Unpin(Renderer* renderer);

        // The WorldCull view (acquired on demand; retried every 60th call, ~1 s, when
        // none is free).
        bool EnsureView(Renderer& renderer);
        void ReleaseView(Renderer* renderer, const char* reason);
        // The lane moves only when this is its only WorldCull view; otherwise the lane's
        // size is adopted (SharingLane) - never another panel's view suspended.
        void NegotiateSize(Renderer& renderer, std::uint32_t w, std::uint32_t h);
        // EnsureCamera creates the camera entity (when its handle is not valid);
        // PlaceCamera puts it at @p eye looking along @p forward (world up); Request asks
        // for the view - every frame (a view that stops requesting is torn down).
        void EnsureCamera(World& world);
        void PlaceCamera(World& world, const float eye[3], const float forward[3]);
        void Request(Renderer& renderer);

        bool          Up() const { return m_root != NullEntity; }
        Entity        Root() const { return m_root; }
        Entity        Skeleton() const { return m_skel; }
        std::uint32_t View() const { return m_view; }
        Entity        Camera() const { return m_cam; }
        float         OriginY() const { return StageOriginY(m_desc.slot); }
        std::uint32_t Spawns() const { return m_spawns; }
        std::uint32_t Stripped() const { return m_stripped; }
        bool          Pinned() const { return m_pinned; }
        bool          SharingLane() const { return m_sharingLane; }
        std::uint32_t RenderW() const { return m_renderW; }
        std::uint32_t RenderH() const { return m_renderH; }
        const std::vector<Entity>&        Entities() const { return m_ents; }
        const std::unordered_set<Entity>& Set() const { return m_set; }

    private:
        Desc                       m_desc;
        Entity                     m_root = NullEntity, m_skel = NullEntity, m_cam = NullEntity;
        EntityHandle               m_rootH = NullEntityHandle, m_skelH = NullEntityHandle, m_camH = NullEntityHandle;
        std::vector<Entity>        m_ents;
        std::vector<EntityHandle>  m_handles;
        std::unordered_set<Entity> m_set, m_pin;
        bool                       m_pinned = false;
        std::uint32_t              m_view = ~0u, m_viewTries = 0;
        std::uint32_t              m_askedW = 0, m_askedH = 0, m_renderW = 0, m_renderH = 0;
        bool                       m_sharingLane = false;
        std::uint32_t              m_spawns = 0, m_stripped = 0;
    };

    // ==== a stage's clip transport ====
    // For an owner that plays a clip on its stage (the Chain Physics Preview's shape):
    // the clip goes onto the stage's Animator as an ALIAS in TWO steps -
    // SetAnimatorClip (the alias table) then QueueAnimatorPlay (it resolves names only;
    // blend 0: Edit mode's animDt = 0 could never start a cross-fade) - and the owner
    // owns the clock: every frame externalTime, paused = false, primaryTime and
    // prevNotifyTime = -1 (a scrub replays no notify) on the AnimationComponent.
    // AnimationSystem samples primaryTime unconditionally; only its ADVANCE is gated.
    // The playhead is arithmetic on t. (The chain panel keeps its own: its clock is
    // also the chain lane's.)
    class StageClipPlayer
    {
    public:
        StageClipPlayer(const char* aliasPrefix, const char* logPrefix)
            : m_aliasPrefix(aliasPrefix), m_logPrefix(logPrefix) {}
        // The clip: an .ianim (and one of its clips by name - "" = its first); "" = the
        // rest pose (no clip on the stage). Takes the player's own reference (the read-out
        // does not wait for the alias's binding). The playhead is left as it is (the
        // caller sets it: a picker starts at 0, playing).
        void Select(Resource::AnimationClipSystem* acs, const std::string& path, const std::string& sourceClip = {});
        // A new stage (a spawn): the alias goes onto its Animator again.
        void OnNewStage() { m_applyPending = !m_path.empty(); m_alias.clear(); m_bound = false; }
        // The reference given back (the panel is done; the selection is kept).
        void Release(Resource::AnimationClipSystem* acs);

        // Transport - the playhead wraps (looping) or clamps (and stops) in Update.
        void SetPlaying(bool p) { m_playing = p; }
        void SetTime(double t) { m_t = t; }
        void SetSpeed(float s) { m_speed = s; }
        void SetLooping(bool l) { m_looping = l; }
        void StepFrames(int n);   // pauses; n frames at the clip's rate

        // Every frame the stage is up (the game half's serial window): the alias
        // (re)registered on @p animEnt, the playhead advanced by @p dt real seconds when
        // playing, the clock written; the read-outs, against @p skel (the stage's skeleton
        // - the bound-bone count, Resource::FitClip's).
        void Update(World& world, Resource::AnimationClipSystem* acs, Entity animEnt, const SkeletonAsset* skel,
                    double dt);

        const std::string& Path() const { return m_path; }
        const std::string& Source() const { return m_source; }
        const std::string& ClipName() const { return m_clipName; }   // the clip that plays (resolved)
        bool          Bound() const { return m_bound; }
        bool          Playing() const { return m_playing; }
        bool          Looping() const { return m_looping; }
        float         Speed() const { return m_speed; }
        double        Time() const { return m_t; }
        float         Duration() const { return m_duration; }
        float         Fps() const { return m_fps; }
        std::uint32_t Frames() const { return m_frames; }
        std::uint32_t Channels() const { return m_channels; }
        std::uint32_t BoundBones() const { return m_boundBones; }
        bool          InfoValid() const { return m_infoValid; }

    private:
        void RefreshInfo(Resource::AnimationClipSystem* acs, const SkeletonAsset* skel);

        std::string    m_aliasPrefix, m_logPrefix;
        std::string    m_path, m_source, m_clipName, m_alias;
        Resource::AnimHandle m_handle{};     // the player's own reference (IsValid: held)
        bool           m_applyPending = false, m_bound = false, m_infoValid = false;
        bool           m_playing = true, m_looping = true;
        float          m_speed = 1.f, m_duration = 0.f, m_fps = 30.f;
        double         m_t = 0.0;
        std::uint32_t  m_frames = 0, m_channels = 0, m_boundBones = 0;
        std::uint32_t  m_infoSkelBones = 0;   // the skeleton the bound count is for
    };
}
