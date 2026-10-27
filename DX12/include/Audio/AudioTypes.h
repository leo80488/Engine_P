#pragma once

// Audio shared types: small POD structs used by every layer (engine wrapper, ECS components,
// events, systems).

#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

namespace Audio
{

// Submix bus categories: one IXAudio2SubmixVoice per bus feeding the mastering voice (Master is the
// mastering voice itself); each bus has its own volume.
enum class BusType : uint32_t
{
    Master = 0,
    Music,
    SFX,
    Voice,
    Ambient,
    UI,
    Count
};

// Lifecycle of a source as seen by the ECS layer. The audio worker thread flags a finished voice
// through an atomic on its voice record; AudioSystem syncs that into the component once per frame.
enum class PlayState : uint8_t
{
    Stopped = 0,
    Playing,
    Paused,
    Stopping,   // fade-out in progress; not set yet (StopVoice does not fade)
};

// 3D distance attenuation curve, carried by sources, events and voices.
// TODO(audio_system_architecture 9): not applied yet - Apply3D uses X3DAudio's default distance
// curve for every value.
enum class AttenuationCurve : uint8_t
{
    Linear = 0,
    Logarithmic,
    InverseSquare,
};

// VoiceHandle - opaque id into AudioEngine's voice table: 20-bit slot index + 12-bit generation, so
// a slot reused by a later PlayClip does not alias the old handle.
struct VoiceHandle
{
    static constexpr uint32_t kIndexBits      = 20;
    static constexpr uint32_t kGenerationBits = 12;
    static constexpr uint32_t kIndexMask      = (1u << kIndexBits) - 1u;
    static constexpr uint32_t kGenerationMask = (1u << kGenerationBits) - 1u;

    uint32_t value = 0;   // 0 = invalid

    static VoiceHandle Make(uint32_t index, uint32_t generation)
    {
        VoiceHandle h;
        h.value = (index & kIndexMask)
                | ((generation & kGenerationMask) << kIndexBits);
        return h;
    }

    uint32_t Index()      const { return value & kIndexMask; }
    uint32_t Generation() const { return (value >> kIndexBits) & kGenerationMask; }
    bool     IsValid()    const { return value != 0; }
};

// Playback parameters. Most fields mirror AudioSourceComponent; a play without a source component
// (UI clicks) fills them from its event.
struct PlayParams
{
    BusType          bus           = BusType::SFX;
    float            volume        = 1.0f;
    float            pitch         = 1.0f;     // 1.0 = original pitch
    bool             loop          = false;
    bool             is3D          = false;

    // 3D attenuation envelope (read only when is3D).
    // TODO(audio_system_architecture 3.3): position is not consumed yet - AudioEngine::PlayClip ignores
    // it, so only AudioSource voices are spatialized (by Audio3DSystem).
    DirectX::XMFLOAT3 position     = { 0.f, 0.f, 0.f };
    float            minDistance   = 1.0f;
    float            maxDistance   = 50.0f;
    AttenuationCurve curve         = AttenuationCurve::Linear;

    // Start playback this many seconds into the clip, so a cinematic scrubbed into a line of dialogue
    // plays from there. A straight XAUDIO2_BUFFER PlayBegin, valid because clips are fully resident
    // PCM; a compressed / streaming strategy must honour it or say plainly that it cannot.
    float            startOffsetSec = 0.0f;
};

// Parameters that can change on a live voice (the bus is fixed at PlayClip).
struct VoiceParams
{
    float volume = 1.0f;
    float pitch  = 1.0f;
};

} // namespace Audio
