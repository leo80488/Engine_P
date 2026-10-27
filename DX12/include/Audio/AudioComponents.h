#pragma once

// ECS components for audio sources and listeners (spec: docs/design/audio_system_architecture.md).
// Pure data: no XAudio2 / Windows headers leak through here. AudioSystem owns the runtime fields
// (activeVoice, state); treat them as opaque.

#include "Audio/AudioTypes.h"
#include "Resource/SystemHandles.h"   // AudioHandle alias
#include "ECS/ECS.h"                  // ComponentCloner<T> (snapshot/restore hook)
#include <string>

namespace Audio
{

// An entity that emits sound. is3D + a transform -> spatial; otherwise a 2D sample (no
// attenuation, panning or doppler).
// clipPath is the serialized identity (an asset-relative path such as
// "asset/footsteps/grass_01.wav"). The first frame after it is set, AudioSystem acquires the clip
// through AudioClipSystem::AcquireClip and caches the handle in clipHandle; later plays use the handle.
struct AudioSourceComponent
{
    std::string           clipPath;                     // source-of-truth identity
    Resource::AudioHandle clipHandle = Resource::kInvalidAudioHandle;  // runtime cache
    BusType          bus            = BusType::SFX;
    float            volume         = 1.0f;
    float            pitch          = 1.0f;
    bool             loop           = false;
    bool             is3D           = true;
    bool             playOnEnable   = false;            // auto-play once when the simulation begins (config)

    // 3D attenuation envelope (consulted when is3D == true).
    float            minDistance    = 1.0f;
    float            maxDistance    = 50.0f;
    AttenuationCurve curve          = AttenuationCurve::Linear;

    // ---- Runtime state, written by AudioSystem; do not edit ----
    VoiceHandle      activeVoice    = {};
    PlayState        state          = PlayState::Stopped;
    // The clipPath that clipHandle was acquired for. When clipPath stops matching it (an inspector
    // edit, a serializer rewrite), AudioSystem stops the voice on the old clip, releases that clip and
    // acquires the new one; otherwise a handle is never re-resolved from the path.
    std::string      boundClipPath;

    // ---- Runtime transport requests (inspector / Lua / MotionDirector) ----
    // playRequested / stopRequested are edge triggers that AudioSystem clears each frame. paused is a
    // level reconciled against the live voice: pause is an XAudio2 Stop without flush, so a resume
    // continues from the same position. They let the inspector buttons, Lua and a MotionDirector audio
    // track drive a source without touching the engine.
    bool             playRequested  = false;   // (re)start playback from playStartOffsetSec
    // Seconds into the clip where the next playRequested starts; consumed (reset to 0) with the play
    // edge. A Sequencer audio section scrubbed to its middle sets it, so dialogue resumes at the
    // playhead instead of restarting.
    float            playStartOffsetSec = 0.f;
    bool             stopRequested  = false;   // stop + release the active voice
    bool             paused         = false;   // desired pause state of the voice
    bool             autoPlayFired  = false;   // playOnEnable one-shot guard (keeps playOnEnable pure config)
};

// The entity whose transform is the listener pose (typically the active camera). Mark only one
// active; with several, Audio3DSystem takes the first it meets (and falls back to any camera when
// none is active).
struct AudioListenerComponent
{
    float masterVolume = 1.0f;   // applied on top of BusType::Master volume
    bool  active       = true;
};

} // namespace Audio

// Edit <-> Play snapshot/restore hook. The per-play runtime state (live voice handle, transport
// edge flags) must not reach the editor-world snapshot, or a queued Play / Stop or a live voice
// would be restored on Stop and fire again on the next Play. Copy the authored fields and reset the
// runtime state, which Play rebuilds.
template <>
struct ComponentCloner<Audio::AudioSourceComponent>
{
    static Audio::AudioSourceComponent Clone(const Audio::AudioSourceComponent& src)
    {
        Audio::AudioSourceComponent dst = src;   // authored fields + clipHandle (ref shared)
        dst.activeVoice   = {};
        dst.state         = Audio::PlayState::Stopped;
        dst.playRequested = false;
        dst.playStartOffsetSec = 0.f;
        dst.stopRequested = false;
        dst.paused        = false;
        dst.autoPlayFired = false;
        return dst;
    }
};
