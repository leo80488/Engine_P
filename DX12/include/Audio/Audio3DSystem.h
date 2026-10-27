#pragma once

// Audio3DSystem - feeds X3DAudio this frame's listener pose and each playing 3D voice's emitter
// pose, then has AudioEngine apply the resulting DSP (output matrix + doppler) to the source voice.
// Must run after TransformSystem::Propagate so GlobalTransform is current for the listener and the
// emitters. Spec: docs/design/audio_system_architecture.md.

#include "ECS/ECS.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <unordered_map>

namespace Audio
{

class AudioEngine;

class Audio3DSystem
{
public:
    void BindEngine(AudioEngine& engine) { m_engine = &engine; }
    void Update(World& world, float dt);

private:
    struct EmitterCache
    {
        DirectX::XMFLOAT3 position { 0.f, 0.f, 0.f };
        bool              valid    = false;
    };

    AudioEngine* m_engine = nullptr;

    // Previous emitter positions, for velocity -> doppler. Keyed by Entity, not pool slot: the dense
    // AudioSourceComponent order changes as components are added and removed.
    std::unordered_map<Entity, EmitterCache> m_emitterCache;

    DirectX::XMFLOAT3 m_listenerPrevPos { 0.f, 0.f, 0.f };
    bool              m_listenerCached  = false;
};

} // namespace Audio
