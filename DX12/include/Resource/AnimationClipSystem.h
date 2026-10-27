#pragma once

// AnimationClipSystem - path-deduplicated, ref-counted cache of .ianim animation
// resources (the TextureSystem pattern): AcquireClip returns the same AnimHandle for
// the same path, ReleaseClip unloads at refcount zero, Tick marks a clip Ready once the
// ResourceManager has finished its load (a failed load is Ready with a null resource),
// GetResource lends a pointer (do not store it). BindToSkeleton binds a ready resource's
// clips to a skeleton and registers them in a ClipLibrary (kInvalidClipIndex while not
// ready or when there are no bone clips).

#include "Resource/SystemHandles.h"
#include "Resource/ResourceManager.h"
#include "Resource/AnimationResource.h"
#include "Resource/SkeletonAsset.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class World;
class Renderer;
class AnimGraphSystem;

namespace Resource
{
    class AnimationClipSystem
    {
    public:
        AnimationClipSystem()  = default;
        ~AnimationClipSystem() = default;

        AnimationClipSystem(const AnimationClipSystem&)            = delete;
        AnimationClipSystem& operator=(const AnimationClipSystem&) = delete;

        void Init(ResourceManager& rm);
        void SetGraphSystem(AnimGraphSystem* graph){m_graph=graph;}
        AnimGraphSystem* GraphSystem()const{return m_graph;}

        // ---- Async load + dedup ----
        // Starts an async load and returns a handle at once; the same path returns the same
        // handle (refCount++). Poll IsReady(), then GetResource().
        AnimHandle AcquireClip(const std::string& path);

        // Decrement ref count. Resource unloaded when it reaches zero.
        void ReleaseClip(AnimHandle handle);

        // True once Tick has seen the load finish, including a failed one: GetResource()
        // then returns nullptr.
        bool IsReady(AnimHandle handle) const;

        // Borrowed pointer, valid only for the current call stack; nullptr until ready and
        // after a failed load.
        const AnimationResource* GetResource(AnimHandle handle) const;

        // Mutable borrowed pointer, editor only: the animation-timeline editor authors
        // notify tracks on the loaded resource and re-serializes it to .ianim. Valid only
        // for the current call stack; do not store. nullptr until ready and after a failed load.
        AnimationResource* GetResourceMutable(AnimHandle handle);

        // ---- Per-frame pump ----
        // Marks a clip Ready once the ResourceManager has finished its load. A failed load is
        // marked Ready too (stops polling) and its resource stays null, so every bind sees no
        // data. Call once per frame after BeginFrame.
        void Tick();

        // Once per frame after Tick(): binds the deferred clips that became ready -
        // PendingAnimBind (sets AnimationComponent.primaryClip and MorphComponent.primaryMorphClip,
        // then removes the component), locomotion samples, anim layers and transitions - then
        // resolves the authored AnimationSets and the anim graph's pending binds.
        void ResolvePendingBinds(::World& world, ::Renderer& renderer);

        // ---- Authored AnimationSet / runtime Animator ----
        bool QueueAnimatorPlay(::World& world, uint32_t entity,
                               const std::string& clipName,
                               float blendDuration = 0.2f,
                               bool restart = false);
        void StopAnimator(::World& world, uint32_t entity);
        void UnassignAnimator(::World& world, uint32_t entity, bool clearSet);
        std::string GetCurrentAnimatorClip(::World& world, uint32_t entity) const;
        std::string GetTargetAnimatorClip(::World& world, uint32_t entity) const;
        float GetAnimatorDuration(::World& world, uint32_t entity) const;
        bool GetBoundAnimatorClip(::World&, uint32_t entity, const std::string& alias,
                                  uint32_t& clip, bool& failed) const;

        bool SetAnimatorClip(::World& world, uint32_t entity,
                             const std::string& alias,
                             const std::string& path,
                             bool looping = true, float speed = 1.f,
                             const std::string& sourceClip = {},
                             const std::string& sourceMorphClip = {});
        bool RemoveAnimatorClip(::World& world, uint32_t entity,
                                const std::string& alias);
        bool HasAnimatorClip(::World& world, uint32_t entity,
                             const std::string& alias) const;
        std::vector<std::string> GetAnimatorClipAliases(
            ::World& world, uint32_t entity) const;

        // Editor introspection: the clip names of an acquired path. Empty while the path is
        // not acquired, still loading, or after a failed load.
        std::vector<std::string> GetBoneClipNames(const std::string& path) const;
        std::vector<std::string> GetMorphClipNames(const std::string& path) const;

        // Releases every ECS-owned/in-flight animation handle before
        // World::Clear destroys the components that carry them.
        void OnWorldClear(::World& world);

        // ---- Skeleton binding ----
        // Binds every clip in the resource to the skeleton and registers them in clipLib.
        // Returns the ClipLibrary index of the first clip (the rest follow contiguously), or
        // kInvalidClipIndex if the handle is not ready, its load failed or it has no bone clips.
        // Cached per skeleton: a repeat call returns the same index.
        uint32_t BindToSkeleton(AnimHandle           handle,
                                 const SkeletonAsset& skeleton,
                                 ClipLibrary&         clipLib);

        // Converts the resource's morph clips to MorphClipAssets and registers them in
        // morphLib. Returns the MorphClipLibrary index of the first morph clip, or
        // kInvalidClipIndex if the handle is not ready, its load failed or it has no morph
        // clips. A repeat call returns the cached index.
        uint32_t BindMorphClip(AnimHandle        handle,
                                MorphClipLibrary& morphLib);

        // ---- Lifecycle ----
        void Shutdown();

    private:
        // Ready = the ResourceManager finished the load, successfully or not.
        enum class ClipState { Loading, Ready };

        struct ClipEntry
        {
            AnimHandle   handle;
            Handle       rmHandle;          // ResourceManager handle
            ClipState    state    = ClipState::Loading;
            uint32_t     refCount = 0;

            // Set after BindToSkeleton succeeds (first call only).
            uint32_t     firstClipLibIdx  = kInvalidClipIndex;
            // Set after BindMorphClip succeeds (first call only).
            uint32_t     firstMorphLibIdx = kInvalidClipIndex;

            // Per-skeleton cache (bone order differs between rigs): key = FNV-32 of the
            // skeleton's bone count and bone names, value = its first ClipLibrary index.
            std::unordered_map<uint32_t, uint32_t> perSkelCache;
        };

        struct BoundAnimatorClip
        {
            std::string path;
            std::string sourceClip;
            std::string sourceMorphClip;
            AnimHandle  handle{};
            uint32_t    clipIndex  = kInvalidClipIndex;
            uint32_t    morphIndex = kInvalidClipIndex;
            float       duration   = 0.f;
            bool        failed     = false;
            // Authored looping / speed last synced to the live playback. ResolveAnimationSets
            // pushes an authored edit to a playing clip only when these differ from the
            // AnimationClipReference: re-asserting them every frame would revert runtime
            // overrides (Animator.SetSpeed, script writes) one frame after they land.
            bool        authoredLooping = true;
            float       authoredSpeed   = 1.f;
        };

        struct AnimatorEntityBindings
        {
            uint32_t entity        = 0;
            uint32_t generation    = 0;
            bool     defaultQueued = false;
            std::string defaultClip;
            std::unordered_map<std::string, BoundAnimatorClip> clips;
        };

        static std::string NormalizePath(const std::string& path);
        void ResolveAnimationSets(::World& world, ::Renderer& renderer);
        void ReleaseAnimatorBindings(uint64_t key);
        static uint64_t AnimatorBindingKey(uint32_t entity, uint32_t generation)
        {
            return (static_cast<uint64_t>(generation) << 32u) | entity;
        }

        ResourceManager* m_rm = nullptr;
        AnimGraphSystem* m_graph = nullptr;

        mutable std::mutex m_mutex;

        uint32_t                                        m_nextSlot = 1;
        std::unordered_map<std::string, ClipEntry>      m_byPath;

        // Resource ownership stays outside ECS values so Edit/Play World
        // cloning never duplicates a ref-counted AnimHandle.
        std::unordered_map<uint64_t, AnimatorEntityBindings> m_animatorBindings;
        std::unordered_map<uint32_t, std::string>       m_pathBySlot; // slot -> normalized path
    };
}
