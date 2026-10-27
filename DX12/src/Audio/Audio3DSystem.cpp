#include "Audio/Audio3DSystem.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioComponents.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "ECS/Components.h"            // CameraComponent

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

namespace Audio
{

namespace
{

// World-space translation: row 3 of the row-major (row-vector) world matrix.
DirectX::XMFLOAT3 WorldPositionFromMatrix(const DirectX::XMFLOAT4X4& m)
{
    return { m._41, m._42, m._43 };
}

} // anonymous

void Audio3DSystem::Update(World& world, float dt)
{
    if (!m_engine) return;

    // ---- 1. Listener pose ----
    // The first active AudioListenerComponent with a GlobalTransform; otherwise any camera, so 3D audio
    // works in scenes that tag no listener. Both read the pose from GlobalTransform.
    DirectX::XMFLOAT3 listenerPos { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 listenerFwd { 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 listenerUp  { 0.f, 1.f, 0.f };
    bool              haveListener = false;

    // Forward = +Z row, up = +Y row (row-vector / row-major convention).
    auto poseFromMatrix = [&](const DirectX::XMFLOAT4X4& m)
    {
        listenerPos = WorldPositionFromMatrix(m);
        listenerFwd = { m._31, m._32, m._33 };
        listenerUp  = { m._21, m._22, m._23 };
        haveListener = true;
    };

    world.ForEach<AudioListenerComponent>(
        [&](Entity e, AudioListenerComponent& lc)
        {
            if (haveListener || !lc.active) return;
            if (auto* gt = world.GetComponent<GlobalTransform>(e))
                poseFromMatrix(gt->matrix);
        });

    if (!haveListener)
    {
        // Fallback: any camera in the world acts as the listener.
        world.ForEach<CameraComponent>(
            [&](Entity e, CameraComponent&)
            {
                if (haveListener) return;
                if (auto* gt = world.GetComponent<GlobalTransform>(e))
                    poseFromMatrix(gt->matrix);
            });
    }

    if (!haveListener) return;

    X3DAUDIO_LISTENER listener{};
    listener.Position    = { listenerPos.x, listenerPos.y, listenerPos.z };
    listener.OrientFront = { listenerFwd.x, listenerFwd.y, listenerFwd.z };
    listener.OrientTop   = { listenerUp.x,  listenerUp.y,  listenerUp.z  };

    if (m_listenerCached && dt > 0.f)
    {
        listener.Velocity = {
            (listenerPos.x - m_listenerPrevPos.x) / dt,
            (listenerPos.y - m_listenerPrevPos.y) / dt,
            (listenerPos.z - m_listenerPrevPos.z) / dt };
    }
    m_listenerPrevPos = listenerPos;
    m_listenerCached  = true;

    m_engine->SetListener(listener);

    // ---- 2. DSP for every playing 3D source ----
    auto* pool = world.GetPool<AudioSourceComponent>();
    if (!pool) return;
    auto& ents = pool->Entities();
    auto& data = pool->Data();
    const size_t n = data.size();

    for (size_t i = 0; i < n; ++i)
    {
        const Entity e = ents[i];
        const AudioSourceComponent& src = data[i];
        if (!src.is3D) continue;
        if (src.state != PlayState::Playing) continue;
        if (!src.activeVoice.IsValid())      continue;

        DirectX::XMFLOAT3 pos { 0.f, 0.f, 0.f };
        if (auto* gt = world.GetComponent<GlobalTransform>(e))
            pos = WorldPositionFromMatrix(gt->matrix);

        DirectX::XMFLOAT3 vel { 0.f, 0.f, 0.f };
        EmitterCache& cache = m_emitterCache[e];
        if (cache.valid && dt > 0.f)
        {
            vel = { (pos.x - cache.position.x) / dt,
                    (pos.y - cache.position.y) / dt,
                    (pos.z - cache.position.z) / dt };
        }
        cache.position = pos;
        cache.valid    = true;

        X3DAUDIO_EMITTER emitter{};
        emitter.Position             = { pos.x, pos.y, pos.z };
        emitter.Velocity             = { vel.x, vel.y, vel.z };
        emitter.OrientFront          = { 0.f, 0.f, 1.f };
        emitter.OrientTop            = { 0.f, 1.f, 0.f };
        emitter.ChannelCount         = 1;       // overridden inside Apply3D for stereo clips
        emitter.InnerRadius          = src.minDistance;
        emitter.InnerRadiusAngle     = X3DAUDIO_PI / 4.f;
        emitter.CurveDistanceScaler  = src.maxDistance > 0.f ? src.maxDistance : 1.f;
        emitter.DopplerScaler        = 1.f;

        m_engine->Apply3D(src.activeVoice, emitter);
    }

    // Drop cache entries of entities without an AudioSourceComponent, so the map does not grow without
    // bound as sources come and go (spawned projectile sounds).
    if (m_emitterCache.size() > n + 32)
    {
        std::erase_if(m_emitterCache, [&](const auto& kv){
            return !world.HasComponent<AudioSourceComponent>(kv.first);
        });
    }
}

} // namespace Audio
