#pragma once

// AudioClipSystem - path-deduplicating cache of .aclip resources, the same shape as
// AnimationClipSystem.
//   AcquireClip(path)   -> AudioHandle (ref-counted; the same path gives the same handle)
//   ReleaseClip(handle) -> drops a reference; unloads at zero
//   IsReady(handle)     -> true once ResourceManager finished (a failed load is Ready too, and
//                          GetResource then returns null)
//   GetResource(handle) -> const AudioClipResource*, borrowed: do not store it
//   Tick()              -> promotes ResourceManager-ready clips to Ready
// AudioSystem acquires a clip the first time it sees its path, keeps the handle on the
// AudioSourceComponent and checks IsReady before AudioEngine::PlayClip.

#include "Resource/ResourceManager.h"
#include "Resource/SystemHandles.h"   // AudioHandle alias
#include <mutex>
#include <string>
#include <unordered_map>

namespace Audio
{
    class AudioClipResource;

    class AudioClipSystem
    {
    public:
        // ---- Diagnostics ----
        struct Counts { uint32_t clips = 0; uint32_t refs = 0; };

        // ---- Lifetime ----
        AudioClipSystem()  = default;
        ~AudioClipSystem() = default;

        AudioClipSystem(const AudioClipSystem&)            = delete;
        AudioClipSystem& operator=(const AudioClipSystem&) = delete;

        void Init(Resource::ResourceManager& rm);
        void Shutdown();

        // ---- Async load + dedup ----
        Resource::AudioHandle AcquireClip(const std::string& path);
        void                  ReleaseClip(Resource::AudioHandle handle);

        bool                       IsReady    (Resource::AudioHandle handle) const;
        const AudioClipResource*   GetResource(Resource::AudioHandle handle) const;

        // ---- Per-frame pump ----
        // Promotes ResourceManager-ready entries to Ready. Idempotent and cheap.
        void Tick();

        // ---- Diagnostics ----
        // Clips held (loaded or loading) and the references on them: one per AudioSource holding a handle,
        // one per fire-and-forget voice still playing (Debug.Stats().audioClips / audioClipRefs). A
        // reference nobody releases shows as a count that never comes back down.
        Counts GetCounts() const;

    private:
        enum class ClipState { Loading, Ready };

        struct ClipEntry
        {
            Resource::AudioHandle handle;
            Resource::Handle      rmHandle;
            ClipState             state    = ClipState::Loading;
            uint32_t              refCount = 0;
        };

        static std::string NormalizePath(const std::string& path);

        Resource::ResourceManager*                 m_rm = nullptr;
        mutable std::mutex                         m_mutex;
        uint32_t                                   m_nextSlot = 1;
        std::unordered_map<std::string, ClipEntry> m_byPath;
        std::unordered_map<uint32_t, std::string>  m_pathBySlot;
    };
}
