#pragma once

// AudioSystem - turns ECS / event activity into AudioEngine calls. Subscribes to PlaySoundEvent /
// StopSoundEvent / SetAudioParamEvent / BusVolumeChangedEvent on the global EventBus, and sweeps
// AudioSourceComponent each frame (clip acquisition, stop / play / pause requests, playOnEnable,
// finished voices back to Stopped). It owns every clip reference it acquires.
// Design: docs/design/audio_system_architecture.md.

#include "ECS/ECS.h"
#include "Audio/AudioTypes.h"
#include "Resource/SystemHandles.h"
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Audio
{

class AudioEngine;
class AudioClipSystem;

class AudioSystem
{
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // Bind* before Update or any event fires. The clip system resolves path -> AudioHandle ->
    // AudioClipResource; without it nothing plays.
    void BindEngine    (AudioEngine&     engine);
    void BindClipSystem(AudioClipSystem& clipSystem);
    void BindWorld     (World&           world);
    void Unbind();

    // Per-frame tick: releases finished fire-and-forget clip references, then runs every
    // AudioSourceComponent of @p world (pool iteration) through acquire / stop / start / pause.
    void Update(World& world, float dt);

    // Releases every clip reference this system holds (each live source's clipHandle + the
    // fire-and-forget list), stops their voices and resets source runtime state. Call on a world
    // teardown, notably the Play -> Edit edge: World::AdoptState swaps the pools without firing destroy
    // listeners, so the play-world sources' clip refs would leak. Must run before AdoptState, while the
    // play-world pool is live.
    void OnWorldClear();

private:
    struct PendingRelease
    {
        VoiceHandle           voice;
        Resource::AudioHandle handle;
    };

    void OnPlaySound (const struct PlaySoundEvent&);
    void OnStopSound (const struct StopSoundEvent&);
    void OnSetParam  (const struct SetAudioParamEvent&);
    void OnBusVolume (const struct BusVolumeChangedEvent&);
    void OnEntityDestroyed(Entity e);

    // Starts a voice for @p src from its clip handle + params (consuming playStartOffsetSec) and sets
    // activeVoice / state; the caller ensures the clip is ready. False if PlayClip failed (null
    // resource, no slot). Used by the playOnEnable and playRequested paths.
    bool StartSourceVoice(struct AudioSourceComponent& src);

    // Gives @p src the reference acquired on @p handle for @p path; the source keeps exactly one
    // reference on its clip. AcquireClip is path-deduped, so replaying the held clip returns the same
    // handle and the extra reference is released here. A different clip replaces the old one as
    // Update's swap does: the voice reading the old PCM stops before that clip is released.
    // boundClipPath moves with clipPath, or Update would swap straight back.
    void AdoptClip(struct AudioSourceComponent& src, Resource::AudioHandle handle,
                   const std::string& path);

    AudioEngine*     m_engine     = nullptr;
    AudioClipSystem* m_clipSystem = nullptr;
    World*           m_world      = nullptr;

    // Fire-and-forget plays have no component to hold the clip reference, and releasing it at once
    // would free PCM XAudio2 still reads: (voice, handle) pairs wait here until the first Update that
    // sees IsVoicePlaying == false.
    std::vector<PendingRelease> m_pendingReleases;

    // One closure per subscription, each calling EventBus::Unsubscribe<E>; Unbind() runs them.
    std::vector<std::function<void()>> m_unsubscribers;
    uint32_t                            m_destroyListenerId = 0;
};

} // namespace Audio
