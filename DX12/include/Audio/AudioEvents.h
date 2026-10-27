#pragma once

// Audio events, published through the EventBus singleton (design:
// docs/design/audio_system_architecture.md). AudioSystem subscribes to all four and drives
// AudioEngine; UI / Lua / gameplay code sends these events and never touches AudioEngine directly.

#include "Audio/AudioTypes.h"
#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <string>

namespace Audio
{

// Plays a clip. With an entity that has an AudioSourceComponent, the component takes the clip and the
// voice (a still-loading clip plays once ready, through playOnEnable). Any other play (a null
// `entity` for UI clicks and one-shots, or an entity without a source) is fire-and-forget: AudioSystem
// holds the clip reference until the voice ends, and drops the play if the clip is still loading.
struct PlaySoundEvent
{
    EntityHandle      entity     = NullEntityHandle;  // optional
    std::string       clipPath;                       // clip asset path (resolved by AudioClipSystem)
    BusType           bus        = BusType::SFX;
    float             volume     = 1.0f;
    float             pitch      = 1.0f;
    bool              loop       = false;
    bool              is3D       = false;
    // Position of a fire-and-forget 3D sound (null `entity`); not consumed yet (see
    // PlayParams::position).
    DirectX::XMFLOAT3 position   = { 0.f, 0.f, 0.f };
    float             minDistance = 1.0f;
    float             maxDistance = 50.0f;
    AttenuationCurve  curve      = AttenuationCurve::Linear;
};

// Stops a playing source: a raw VoiceHandle (fire-and-forget plays; checked first) or an entity's
// AudioSourceComponent.
struct StopSoundEvent
{
    EntityHandle entity      = NullEntityHandle;     // optional
    VoiceHandle  voice       = {};                   // optional; takes precedence over entity
    float        fadeOutSec  = 0.0f;                 // not applied yet: the voice stops at once
};

// Sets volume or pitch on a live voice (`voice`, else the entity's active voice).
struct SetAudioParamEvent
{
    enum Param : uint8_t { Volume, Pitch };

    EntityHandle entity = NullEntityHandle;
    VoiceHandle  voice  = {};
    Param        param  = Volume;
    float        value  = 1.0f;
};

// Mixer / settings -> bus volume. AudioSystem applies it to AudioEngine; EngineSettings mirrors it.
struct BusVolumeChangedEvent
{
    BusType bus    = BusType::Master;
    float   volume = 1.0f;
};

} // namespace Audio
