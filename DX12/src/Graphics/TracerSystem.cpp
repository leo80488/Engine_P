#include "Graphics/TracerSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

void TracerSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Main tracer pool (DEFAULT heap, SRV + UAV) ----
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxTracers) * sizeof(TracerGPU);
        d.usage      = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TracerGPU);
        if (!gfx.CreateBuffer(d, m_pool))
        {
            LOG_ERROR("TracerSystem: pool buffer creation failed");
            return;
        }
        m_poolSrv = gfx.GetBufferSRVGpuHandle(m_pool);
        m_poolUav = gfx.GetBufferUAVGpuHandle(m_pool);
    }

    // ---- Spawn upload buffers (UPLOAD heap, structured SRV), one per frame in flight ----
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxSpawnsPerFrame) * sizeof(TracerSpawnGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TracerSpawnGPU);
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_spawnBuffer[i]))
            {
                m_spawnMapped[i] = gfx.MapBuffer(m_spawnBuffer[i]);
                m_spawnSrv[i]    = gfx.GetBufferSRVGpuHandle(m_spawnBuffer[i]);
            }
            if (!m_spawnMapped[i])
                LOG_ERROR("TracerSystem: spawn buffer[%u] map failed", i);
        }
    }

    // ---- System CB (UPLOAD heap, root CBV), one per frame in flight ----
    if (!m_systemCB.Create(gfx, "TracerSystem.SystemCB"))
        LOG_ERROR("TracerSystem: system CB create failed");

    m_pendingSpawns.reserve(kMaxSpawnsPerFrame);

    LOG_SUCCESS("TracerSystem: initialised (%u slots * %zu B = %zu KB)",
                kMaxTracers, sizeof(TracerGPU),
                (kMaxTracers * sizeof(TracerGPU)) / 1024);
}

void TracerSystem::Shutdown(IGraphicsDevice& gfx)
{
    m_systemCB.Destroy(gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_spawnMapped[i])  { gfx.UnmapBuffer(m_spawnBuffer[i]); m_spawnMapped[i] = nullptr; }
        if (m_spawnBuffer[i].IsValid()) gfx.DestroyBuffer(m_spawnBuffer[i]);
        m_spawnSrv[i] = 0;
    }
    if (m_pool.IsValid())       gfx.DestroyBuffer(m_pool);
    m_gfx = nullptr;
}

uint64_t TracerSystem::GetSpawnSRV(IGraphicsDevice& gfx) const
{
    return m_spawnSrv[gfx.GetFrameIndex()];
}

void TracerSystem::Spawn(const DirectX::XMFLOAT3& start,
                         const DirectX::XMFLOAT3& end,
                         const DirectX::XMFLOAT4& color,
                         float                    width,
                         float                    lifetime,
                         uint32_t                 noiseTexBindless)
{
    if (m_pendingSpawns.size() >= kMaxSpawnsPerFrame) return; // drop

    // Keep the liveness estimate covering the longest-lived tracer in flight (see
    // HasLiveTracers). Folded after the drop check: a dropped spawn never reaches the pool,
    // so it must not extend the estimate.
    m_liveTimer = std::max(m_liveTimer, lifetime);

    TracerSpawnGPU s{};
    s.startPos            = start;
    s.endPos              = end;
    s.color               = color;
    s.width               = width;
    s.lifetime            = lifetime;
    s.maxLifetime         = lifetime;
    s.spawnTime           = m_lastGlobalTime;
    s.noiseTexBindlessIdx = noiseTexBindless;
    m_pendingSpawns.push_back(s);
}

void TracerSystem::BeginFrame(float dt, uint32_t /*frameIndex*/, float globalTimeSeconds)
{
    m_lastGlobalTime = globalTimeSeconds;
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetUploadSlot();

    // Mirror the GPU lifetime countdown: TracerUpdate.cs.hlsl decrements every live tracer by
    // this same deltaTime, so HasLiveTracers goes false only once the longest-lived slot has
    // provably hit zero. Decayed after the !m_gfx return: with no device the sim does not run
    // either, and decaying anyway would run the estimate ahead of the pool.
    m_liveTimer = std::max(0.0f, m_liveTimer - dt);

    // Clamp spawn count to upload-buffer capacity.
    const uint32_t spawnCount = std::min(static_cast<uint32_t>(m_pendingSpawns.size()),
                                          kMaxSpawnsPerFrame);

    // Re-stamp spawnTime with this frame's time: Spawn ran before this BeginFrame and stamped
    // the previous frame's time.
    if (spawnCount > 0 && frameSlot < kFrameCount && m_spawnMapped[frameSlot])
    {
        for (uint32_t i = 0; i < spawnCount; ++i)
            m_pendingSpawns[i].spawnTime = globalTimeSeconds;
        std::memcpy(m_spawnMapped[frameSlot], m_pendingSpawns.data(),
                    spawnCount * sizeof(TracerSpawnGPU));
    }

    if (auto* slot = m_systemCB.Upload(*m_gfx))
    {
        TracerSystemParams cb{};
        cb.deltaTime    = dt;
        cb.poolCapacity = kMaxTracers;
        cb.spawnCount   = spawnCount;
        cb.time         = globalTimeSeconds;
        cb.writeCursor  = m_writeCursor;
        *slot = cb;
    }

    m_spawnCountThisFrame = spawnCount;

    // Advance ring cursor for next frame.
    m_writeCursor = (m_writeCursor + spawnCount) % kMaxTracers;

    m_pendingSpawns.clear();
}
