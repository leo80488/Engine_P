#include "Graphics/ParticleSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderLibrary.h"       // material-driven bucket b8 reflection
#include "Graphics/ShaderReflection.h"
#include "ECS/ECS.h"
#include "ECS/ParticleComponent.h"
#include "ECS/Components.h"               // MaterialComponent: material-driven look
#include "ECS/HierarchyComponents.h"
#include "ECS/MaterialReflectionSync.h"   // Sync + IsReservedCBuffer
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "System/Log.h"
#include "System/Check.h"   // ENGINE_CHECK_ONCE: lazy pool creation is retried per frame

#include <cstring>
#include <cmath>
#include <algorithm>

namespace {
// Refill key of an ambient volume: extents quantized to 0.25 m (so inspector drag
// jitter does not thrash refills) plus count and density. A mismatch forces a
// uniform refill: re-folding an old distribution into a resized box is NOT uniform
// (shrinking makes density bands, growing leaves an empty shell).
uint64_t AmbientRefillKey(const DirectX::XMFLOAT3& halfExt, int count, float density)
{
    auto q = [](float v) { return static_cast<int32_t>(std::lround(v / 0.25f)); };
    uint64_t h = 1469598103934665603ull;                 // FNV-1a 64
    auto mix = [&](uint32_t x) { h ^= x; h *= 1099511628211ull; };
    mix(static_cast<uint32_t>(q(halfExt.x)));
    mix(static_cast<uint32_t>(q(halfExt.y)));
    mix(static_cast<uint32_t>(q(halfExt.z)));
    mix(static_cast<uint32_t>(count));
    uint32_t db; std::memcpy(&db, &density, sizeof(db)); mix(db);
    return h;
}
} // namespace

void ParticleSystem::OnEntityDestroyed(IGraphicsDevice& gfx, Entity e)
{
    m_ambientSlotByEntity.erase(e);   // free the ambient volume slot (if any)
    if (auto cit = m_customTexCache.find(e); cit != m_customTexCache.end())
    {
        if (m_texSys)
            for (auto& [_n, entry] : cit->second)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, gfx);
        m_customTexCache.erase(cit);
    }
    m_customTexIndices.erase(e);
    auto it = m_emitterTexCache.find(e);
    if (it == m_emitterTexCache.end()) return;
    if (m_texSys && it->second.handle != Resource::kInvalidTextureHandle)
        m_texSys->Release(it->second.handle, gfx);
    m_emitterTexCache.erase(it);
}

void ParticleSystem::OnWorldClear(IGraphicsDevice& gfx, bool worldSurvives)
{
    // The pools are released with the world that owned them, except in a bake-preview
    // clear (worldSurvives); see the declaration.
    if (!worldSurvives)
    {
        ReleaseMainPool();
        ReleaseBoidPools();
    }

    // World::Clear() fires no per-entity destroy listeners, so OnEntityDestroyed is
    // bypassed: clear the per-entity maps here, or dead entities keep the ambient volume
    // slots and recycled entity IDs inherit stale ones.
    m_ambientSlotByEntity.clear();
    m_ambientActiveMask = 0;
    for (uint32_t v = 0; v < kMaxAmbientVolumes; ++v)
    {
        m_ambientActiveLastFrame[v] = false;
        m_ambientActiveThisFrame[v] = false;
        m_ambientVolumes[v] = AmbientVolumeCPU{};
    }

    // Release the cached emitter textures. TextureSystem::Release defers the GPU destroy
    // to its next Tick, so this is safe mid-frame.
    if (m_texSys)
    {
        for (auto& [_e, entry] : m_emitterTexCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, gfx);
        for (auto& [_e, slots] : m_customTexCache)
            for (auto& [_n, entry] : slots)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, gfx);
    }
    m_emitterTexCache.clear();
    m_customTexCache.clear();
    m_customTexIndices.clear();
}

uint32_t ParticleSystem::ComputeAmbientCount(const ParticleAmbientComponent& ac,
                                             const DirectX::XMFLOAT3& halfExt) const
{
    uint32_t want;
    if (ac.ambientDensity > 0.0f)
    {
        const double vol = 8.0 * static_cast<double>(halfExt.x) * halfExt.y * halfExt.z;
        const double c   = std::ceil(static_cast<double>(ac.ambientDensity) * vol);
        want = static_cast<uint32_t>(std::min(c, static_cast<double>(kParticlesPerBucket)));
    }
    else
    {
        want = static_cast<uint32_t>(std::max(ac.ambientCount, 1));
    }
    return std::clamp(want, 1u, kParticlesPerBucket);
}

void ParticleSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // Own b8 param ring for material-driven buckets: the renderer's material ring
    // rotates AFTER CollectEmitters. <= kMaxParticleBuckets allocations of <= 256 B per
    // frame, so 32 KB is generous headroom (as in TrailSystem).
    m_paramRing.Init(gfx, 32 * 1024);

    // The main pool and the boid working set are not created here: CollectEmitters
    // allocates them on first use (EnsurePoolResources). Everything below is kilobytes
    // and stays eager.

    // Emitter upload buffers, one per frame in flight: emit dispatches bind a root CBV
    // at emitterIdx * kEmitterSlotStride (D3D12 needs 256B-aligned CBV offsets); the
    // 272 B ParticleEmitterGPU fills the start of each slot, the rest is zero padding.
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxEmittersPerFrame) * kEmitterSlotStride;
        d.usage      = RHI::Usage::UPLOAD;
        d.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_emitterBuffer[i]))
                m_emitterMapped[i] = gfx.MapBuffer(m_emitterBuffer[i]);
            if (!m_emitterMapped[i])
                LOG_ERROR("ParticleSystem: emitter buffer map failed (slot %u)", i);
        }
    }

    m_emitterSpawnCounts.reserve(kMaxEmittersPerFrame);

    // Per-frame system CB (UPLOAD heap, root CBV).
    if (!m_systemCB.Create(gfx, "ParticleSystem.CB"))
        LOG_ERROR("ParticleSystem: system CB create failed");

    // Per-frame Boids params CB (UPLOAD heap, root CBV).
    if (!m_boidCB.Create(gfx, "ParticleSystem.BoidCB"))
        LOG_ERROR("ParticleSystem: boid CB create failed");

    LOG_SUCCESS("ParticleSystem: initialised (pool capacity %u slots × %zu B = %zu MiB, "
                "allocated on first emitter)",
                kMaxGlobalParticles, sizeof(ParticleGPU),
                (static_cast<size_t>(kMaxGlobalParticles) * sizeof(ParticleGPU)) / (1024 * 1024));
}

// Runs in CollectEmitters (Renderer::BeginFrame: main thread, before any graph
// recording), the same serial slot the fog / RT passes use for their enable-gated
// working sets. Do not move it into a pass Execute: creation mints descriptors, and
// passes record on TaskSystem workers.
// DestroyBuffer defers the resource and its descriptors for kFrameCount frames, so an
// in-flight command list that still references the pool finishes safely;
// ParticleSimPass / ParticleRenderPass also skip an invalid pool, so the release
// frame draws nothing instead of binding a destroyed SRV/UAV.
void ParticleSystem::EnsurePoolResources(bool wantPool, bool wantBoids)
{
    if (!m_gfx) return;
    IGraphicsDevice& gfx = *m_gfx;

    // Boids cannot run without the pool they snapshot from.
    wantPool = wantPool || wantBoids;

    // ---- Main pool ----
    if (wantPool)
    {
        if (!m_particlePool.IsValid())
        {
            RHI::GPUBufferDesc d{};
            d.size       = static_cast<uint64_t>(kMaxGlobalParticles) * sizeof(ParticleGPU);
            d.usage      = RHI::Usage::DEFAULT;
            d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
            d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
            d.stride     = sizeof(ParticleGPU);
            // ONCE: retried every frame an emitter exists, so a persistent failure would log
            // every frame. On failure the handle stays invalid: CollectEmitters bails and both
            // particle passes skip, so nothing binds a dead resource.
            if (ENGINE_CHECK_ONCE(gfx.CreateBuffer(d, m_particlePool),
                                  "ParticleSystem: particle pool creation failed "
                                  "(%zu MiB) — particles disabled",
                                  static_cast<size_t>(d.size) / (1024 * 1024)))
            {
                m_poolSrvHandle = gfx.GetBufferSRVGpuHandle(m_particlePool);
                m_poolUavHandle = gfx.GetBufferUAVGpuHandle(m_particlePool);
                // A fresh DEFAULT buffer comes back ZEROED: every ring cursor and the update
                // dispatch bound restart from empty.
                for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
                {
                    m_bucketCursor[b]        = 0;
                    m_buckets[b].drawCount   = 0;
                }
                m_simHighWater = 0;
                LOG_INFO("ParticleSystem: particle pool allocated (%zu MiB, emitter present)",
                         static_cast<size_t>(d.size) / (1024 * 1024));
            }
        }
    }
    // Not released when the census goes empty: the pool is per-world sticky and
    // released in OnWorldClear (see Lazy pool residency in the header). An idle-timer
    // release would pop live effects and re-create 128 MiB synchronously on the frame
    // the next effect appears.

    // ---- Boid snapshot + spatial grid ----
    if (wantBoids)
    {
        if (!m_boidSnapshot.IsValid())
        {
            bool ok = true;
            // Stable per-frame {pos,vel,valid} copy of the pool so the steering
            // pass reads neighbours without racing its own writes.
            {
                RHI::GPUBufferDesc d{};
                d.size       = static_cast<uint64_t>(kMaxGlobalParticles) * kBoidSnapStride;
                d.usage      = RHI::Usage::DEFAULT;
                d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
                d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
                d.stride     = kBoidSnapStride;
                if (!ENGINE_CHECK_ONCE(gfx.CreateBuffer(d, m_boidSnapshot),
                                       "ParticleSystem: boid snapshot buffer (%zu MiB) failed",
                                       static_cast<size_t>(d.size) / (1024 * 1024)))
                {
                    ok = false;
                }
                else
                {
                    m_boidSnapshotSrv = gfx.GetBufferSRVGpuHandle(m_boidSnapshot);
                    m_boidSnapshotUav = gfx.GetBufferUAVGpuHandle(m_boidSnapshot);
                }
            }
            // cellCount: one atomic counter per cell. cellItems: kBoidMaxPerCell
            // agent indices per cell. Both UAV-only (written by the grid passes,
            // read by steer).
            if (ok && !m_boidCellCount.IsValid())
            {
                RHI::GPUBufferDesc d{};
                d.size       = static_cast<uint64_t>(kBoidGridCells) * sizeof(uint32_t);
                d.usage      = RHI::Usage::DEFAULT;
                d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
                d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
                d.stride     = sizeof(uint32_t);
                if (!ENGINE_CHECK_ONCE(gfx.CreateBuffer(d, m_boidCellCount),
                                       "ParticleSystem: boid cell-count buffer failed"))
                {
                    ok = false;
                }
                else m_boidCellCountUav = gfx.GetBufferUAVGpuHandle(m_boidCellCount);
            }
            if (ok && !m_boidCellItems.IsValid())
            {
                RHI::GPUBufferDesc d{};
                d.size       = static_cast<uint64_t>(kBoidGridCells) * kBoidMaxPerCell * sizeof(uint32_t);
                d.usage      = RHI::Usage::DEFAULT;
                d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
                d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
                d.stride     = sizeof(uint32_t);
                if (!ENGINE_CHECK_ONCE(gfx.CreateBuffer(d, m_boidCellItems),
                                       "ParticleSystem: boid cell-items buffer failed"))
                {
                    ok = false;
                }
                else m_boidCellItemsUav = gfx.GetBufferUAVGpuHandle(m_boidCellItems);
            }
            // Partial failure would leave the sim pass binding half a working set: drop the
            // lot so IsValid() gates it off entirely.
            if (!ok) ReleaseBoidPools();
            else
                LOG_INFO("ParticleSystem: boid working set allocated (%zu MiB, flock present)",
                         (static_cast<size_t>(kMaxGlobalParticles) * kBoidSnapStride
                          + static_cast<size_t>(kBoidGridCells) * (kBoidMaxPerCell + 1) * sizeof(uint32_t))
                         / (1024 * 1024));
        }
    }
    // Per-world sticky like the main pool (flocks are long-lived, so an idle release
    // would thrash even sooner).
}

void ParticleSystem::ReleaseMainPool()
{
    if (!m_gfx || !m_particlePool.IsValid()) return;
    m_gfx->DestroyBuffer(m_particlePool);
    m_particlePool  = {};
    m_poolSrvHandle = 0;
    m_poolUavHandle = 0;
    // The next allocation returns a ZEROED buffer, so the update dispatch bound and the
    // ring cursors restart from empty.
    for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
    {
        m_bucketCursor[b]      = 0;
        m_buckets[b].drawCount = 0;
        m_buckets[b].window    = 0;
        m_buckets[b].used      = false;
    }
    m_simHighWater   = 0;
    LOG_INFO("ParticleSystem: particle pool released (%zu MiB reclaimed, no emitters)",
             (static_cast<size_t>(kMaxGlobalParticles) * sizeof(ParticleGPU)) / (1024 * 1024));
}

void ParticleSystem::ReleaseBoidPools()
{
    if (!m_gfx) return;
    const bool had = m_boidSnapshot.IsValid();
    if (m_boidSnapshot.IsValid()) m_gfx->DestroyBuffer(m_boidSnapshot);
    if (m_boidCellCount.IsValid()) m_gfx->DestroyBuffer(m_boidCellCount);
    if (m_boidCellItems.IsValid()) m_gfx->DestroyBuffer(m_boidCellItems);
    m_boidSnapshot = {}; m_boidCellCount = {}; m_boidCellItems = {};
    m_boidSnapshotSrv = m_boidSnapshotUav = 0;
    m_boidCellCountUav = m_boidCellItemsUav = 0;
    m_boidSimHighWater = 0;
    m_boidSimLowWater  = kMaxGlobalParticles;   // sentinel: "no flock seen yet"
    if (had)
        LOG_INFO("ParticleSystem: boid working set released (%zu MiB reclaimed, no flock)",
                 (static_cast<size_t>(kMaxGlobalParticles) * kBoidSnapStride
                  + static_cast<size_t>(kBoidGridCells) * (kBoidMaxPerCell + 1) * sizeof(uint32_t))
                 / (1024 * 1024));
}

void ParticleSystem::Shutdown(IGraphicsDevice& gfx)
{
    m_systemCB.Destroy(gfx);
    m_boidCB.Destroy(gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_emitterMapped[i]) { gfx.UnmapBuffer(m_emitterBuffer[i]); m_emitterMapped[i] = nullptr; }
        if (m_emitterBuffer[i].IsValid()) gfx.DestroyBuffer(m_emitterBuffer[i]);
    }
    if (m_boidSnapshot.IsValid()) gfx.DestroyBuffer(m_boidSnapshot);
    if (m_boidCellCount.IsValid()) gfx.DestroyBuffer(m_boidCellCount);
    if (m_boidCellItems.IsValid()) gfx.DestroyBuffer(m_boidCellItems);
    if (m_particlePool.IsValid()) gfx.DestroyBuffer(m_particlePool);
    m_gfx = nullptr;
}

void ParticleSystem::UpdateSystemCB(float dt, uint32_t frameIndex)
{
    if (!m_gfx) return;
    auto* p = m_systemCB.Upload(*m_gfx);
    if (!p) return;
    ParticleSystemCB cb{};
    cb.deltaTime         = dt;
    cb.particleCount     = kMaxGlobalParticles;
    cb.frameIndex        = frameIndex;
    cb.timeWrapped       = 0.0f;   // reserved; no engine time is plumbed here
    cb.globalGravity     = m_globalGravity;
    cb.ambientActiveMask = m_ambientActiveMask;
    cb.cameraPos         = m_cameraPos;
    cb._padC             = 0.0f;
    for (uint32_t v = 0; v < kMaxAmbientVolumes; ++v)
    {
        const AmbientVolumeCPU& av = m_ambientVolumes[v];
        cb.ambientVolA[v] = { av.halfExt.x, av.halfExt.y, av.halfExt.z, av.edgeFade };
        cb.ambientVolB[v] = { av.center.x,  av.center.y,  av.center.z,  av.count };
        cb.ambientVolC[v] = { av.wind.x,    av.wind.y,    av.wind.z,    av.nearFade0 };
        // Expected bucket as a float VALUE (e.g. 2.0f), NOT a bit-cast: small ints
        // bit-cast to float are denormals a float CB load can flush to zero, so asuint()
        // would read 0 and the slice check would kill every ambient particle. The update
        // CS rounds it back.
        cb.ambientVolD[v] = { av.nearFade1, static_cast<float>(av.bucketId), av.intensity,
                              (av.apTextureIdx < 0) ? -1.0f : static_cast<float>(av.apTextureIdx) };
        // Live appearance rows (re-applied per-particle by ParticleUpdate.cs).
        cb.ambientVolE[v] = av.apStartColor;
        cb.ambientVolF[v] = av.apEndColor;
        cb.ambientVolG[v] = { av.apStartScale.x, av.apStartScale.y, av.apStartScale.z, av.apStartSize };
        // Pack visualMode (0-3) + orientation (0-3) into one exact float VALUE, never a
        // bit-cast (the same denormal hazard as the expected bucket).
        const float apPacked = static_cast<float>((av.apVisualMode & 3u) + (av.apOrientation & 3u) * 4u);
        cb.ambientVolH[v] = { av.apIntensity, av.apEmissiveFade, av.apStretch, apPacked };
    }
    *p = cb;
}

const RHI::GPUBuffer& ParticleSystem::GetEmitterBuffer() const
{
    const uint32_t s = m_gfx ? m_gfx->GetFrameIndex() : 0;
    return m_emitterBuffer[s < kFrameCount ? s : 0];
}

const RHI::GPUBuffer& ParticleSystem::GetSystemCB() const
{
    // FrameCB::CurrentBuffer takes a non-const device ref; m_gfx supplies one from this
    // const accessor (buffer selection is logically const).
    static const RHI::GPUBuffer s_empty{};
    if (!m_gfx) return s_empty;
    return m_systemCB.CurrentBuffer(*m_gfx);
}

const RHI::GPUBuffer& ParticleSystem::GetBoidParamsCB() const
{
    static const RHI::GPUBuffer s_empty{};
    if (!m_gfx) return s_empty;
    return m_boidCB.CurrentBuffer(*m_gfx);
}

uint32_t ParticleSystem::FindOrCreateBucket(const std::string& vs,
                                            const std::string& ps,
                                            uint32_t blend, uint32_t taaExclude,
                                            uint32_t lit, uint32_t flocking,
                                            uint32_t recvShadow)
{
    // Reuse an ORDINARY bucket claimed this frame with the same tuple (ambient buckets
    // are keyed by volume slot, not tuple).
    for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
    {
        ParticleBucket& bk = m_buckets[b];
        if (!bk.used || bk.ambient != 0) continue;
        // receiveShadow is part of the reuse key: it selects the PARTICLE_SHADOW PSO
        // permutation, and a shared bucket would let the first emitter decide for both.
        if (bk.blendMode == blend && bk.taaExclude == taaExclude && bk.lit == lit
            && bk.receiveShadow == recvShadow
            && bk.flocking == flocking
            && bk.customVertexPath == vs && bk.customSurfacePath == ps)
        {
            return b;
        }
    }
    // Allocate the lowest free bucket NOT reserved by an active ambient volume
    // this frame. When no ambient volume is active the reserved mask is 0, so
    // ordinary emitters still get the full [0, kMaxParticleBuckets) range.
    for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
    {
        if (m_ambientReservedBucketMask & (1u << b)) continue;
        if (m_buckets[b].used) continue;
        ParticleBucket& bk = m_buckets[b];
        // Dynamic-shader ids survive re-claims with the SAME paths (the steady state:
        // buckets are re-discovered every frame with identical paths). Only a path change
        // re-resolves, so this is the only place particles pay RegisterDynamic (string key
        // + global mutex).
        if (bk.customVertexPath != vs || bk.customSurfacePath != ps)
        {
            bk.dynVsId = bk.dynPsId = ShaderLibrary::kInvalidDynShaderID;
            bk.reflVS  = bk.reflPS  = nullptr;
            bk.reflEpoch = 0;
        }
        if (m_shaderLib)
        {
            if (bk.dynVsId == ShaderLibrary::kInvalidDynShaderID && !vs.empty())
                bk.dynVsId = m_shaderLib->RegisterDynamic(vs.c_str(), RHI::ShaderStage::VS, "main");
            if (bk.dynPsId == ShaderLibrary::kInvalidDynShaderID && !ps.empty())
                bk.dynPsId = m_shaderLib->RegisterDynamic(ps.c_str(), RHI::ShaderStage::PS, "main");
        }
        bk.customVertexPath  = vs;
        bk.customSurfacePath = ps;
        bk.blendMode  = blend;
        bk.taaExclude = taaExclude;
        bk.lit        = lit;
        bk.receiveShadow = recvShadow;
        bk.flocking   = flocking;
        bk.ambient    = 0;
        bk.bucketBase = b * kParticlesPerBucket;
        bk.liveCount  = 0;
        bk.used       = true;
        if (b + 1 > m_bucketCount) m_bucketCount = b + 1;
        return b;
    }
    // No free non-reserved bucket: fold into bucket 0, which is never ambient-reserved
    // (ambient buckets start at kAmbientBucketBase).
    LOG_WARNING("ParticleSystem: no free particle bucket (>%u tuples; ambient-reserved mask 0x%X); "
                "folding extra into bucket 0", kMaxParticleBuckets, m_ambientReservedBucketMask);
    m_buckets[0].used = true;
    if (m_bucketCount < 1) m_bucketCount = 1;
    return 0;
}

void ParticleSystem::CollectEmitters(World& world, float dt, uint32_t frameIndex,
                                     const DirectX::XMFLOAT3& cameraPos)
{
    m_emitterCountThisFrame = 0;
    m_emitterSpawnCounts.clear();
    m_globalGravity = { 0.0f, 0.0f, 0.0f };
    m_pendingMeshResolves.clear();
    m_flockActive = false;
    m_cameraPos = cameraPos;
    m_ambientActiveMask = 0;
    m_ambientReservedBucketMask = 0;
    for (uint32_t v = 0; v < kMaxAmbientVolumes; ++v) m_ambientActiveThisFrame[v] = false;

    // Reset the per-bucket DISCOVERY state each frame (FindOrCreateBucket re-discovers
    // buckets in emitter order) but NOT the write cursors: m_bucketCursor[] persists
    // across frames so successive frames spread new particles across the slice instead
    // of overwriting the same slots (which clumps them at the spawn point).
    for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
    {
        m_buckets[b].used        = false;
        m_buckets[b].liveCount   = 0;
        m_buckets[b].window      = 0;   // re-decided each frame (first spawning emitter wins)
        m_buckets[b].paramCbvVA  = 0;   // re-packed each frame (material-driven buckets)
        m_buckets[b].paramReady  = true;
        m_buckets[b].paramPacked = false;
    }
    m_bucketCount = 0;
    if (!m_gfx) return;

    // ---- Lazy pool residency ----
    // An O(1) presence test on the three emitter pools decides whether the pool / boid
    // snapshot must exist this frame. Gated on the COMPONENT existing, not on
    // `enabled`: a disabled emitter still drains its spawned particles (see the
    // draining path below), and a flickering enabled flag must not thrash a 128 MiB
    // allocation. Runs here (main thread, before any pass records) because buffer
    // creation mints descriptors.
    {
        auto* pEmitPool   = world.GetPool<ParticleEmitterComponent>();
        auto* pFlockPool  = world.GetPool<ParticleFlockComponent>();
        auto* pAmbPool    = world.GetPool<ParticleAmbientComponent>();
        const bool wantBoids = pFlockPool && pFlockPool->Size() > 0;
        const bool wantPool  = wantBoids
                            || (pEmitPool && pEmitPool->Size() > 0)
                            || (pAmbPool  && pAmbPool->Size()  > 0);
        EnsurePoolResources(wantPool, wantBoids);
        // No pool this frame (none wanted, or creation failed): nothing can be emitted or
        // simulated. Bail before the emitter loops so no upload slot or bucket claim
        // references a missing pool slice.
        if (!m_particlePool.IsValid()) return;
    }

    // Write the GAME half's slot: Renderer::BeginFrame runs before the device frame
    // opens, and the render half reads this slot back through GetFrameIndex() in
    // GetEmitterBuffer(). Do not use GetFrameIndex() here: it still names the previous
    // frame's slot, whose GPU work may be queued, so the emit dispatch would read stale
    // or rewritten records and sprites vanish or come back.
    const uint32_t frameSlot = m_gfx->GetUploadSlot();
    if (frameSlot >= kFrameCount || !m_emitterMapped[frameSlot]) return;
    void* emitterMapped = m_emitterMapped[frameSlot];
    m_paramRing.BeginFrame(frameSlot);

    auto* pGlobal = world.GetPool<GlobalTransform>();

    // Default appearance for a behaviour entity without a ParticleRenderComponent.
    static const ParticleRenderComponent kDefaultRender;

    // ---- Shared helpers (used by every behaviour loop) ----
    auto worldPosOf = [&](Entity e) -> DirectX::XMFLOAT3 {
        DirectX::XMFLOAT3 p{ 0.0f, 0.0f, 0.0f };
        if (pGlobal)
            if (const GlobalTransform* gt = pGlobal->Get(e))
            { p.x = gt->matrix._41; p.y = gt->matrix._42; p.z = gt->matrix._43; }
        return p;
    };

    // Effective emitter texture: the material's Base Color slot wins only when the
    // author DELIBERATELY set it (the constructor seeds every slot with the engine
    // default, so a plain non-empty check would swap the sprite for
    // Default_BaseColor.itex); else rc.texturePath.
    static const std::string kDefaultBaseColor =
        MaterialComponent{}.textures[MaterialComponent::BASECOLORMAP].name;
    auto emitterTexPathOf = [&](const ParticleRenderComponent& rc,
                                const MaterialComponent* mc) -> const std::string& {
        if (mc)
        {
            const std::string& bc = mc->textures[MaterialComponent::BASECOLORMAP].name;
            if (!bc.empty() && bc != kDefaultBaseColor) return bc;
        }
        return rc.texturePath;
    };

    // Resolves the effective texture path to a bindless index on the render component
    // (writes rc->textureBindlessIdx / textureGpuHandle); handles are cached per entity.
    auto resolveTexture = [&](Entity e, ParticleRenderComponent* rc, const std::string& path) {
        if (!rc || !m_texSys || !m_resMgr) return;
        auto [cacheIt, _ins] = m_emitterTexCache.try_emplace(e);
        auto& entry = cacheIt->second;
        if (path != entry.path)
        {
            if (entry.handle != Resource::kInvalidTextureHandle && m_gfx)
                m_texSys->Release(entry.handle, *m_gfx);
            entry.path   = path;
            entry.handle = path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(path, *m_resMgr, *m_gfx);
            rc->textureBindlessIdx = -1;
            rc->textureGpuHandle   = 0;
        }
        if (rc->textureBindlessIdx < 0
            && entry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(entry.handle))
            if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
            {
                rc->textureBindlessIdx = static_cast<int32_t>(tex->handleId);
                if (m_gfx) rc->textureGpuHandle = m_gfx->GetTextureSRVGpuHandle(*tex);
            }
    };

    // Mesh-like split: ParticleRenderComponent = SHAPE-side appearance (size, colours,
    // orientation, stretch), MaterialComponent = LOOK (blend, texture, custom shader +
    // params). The material drives whenever it is on the emitter entity; rc's own look
    // fields are the fallback.
    // Domain gate: a material on a MESH-bearing entity belongs to the mesh draw (a
    // dissolve / NPR surface PS in a particle bucket cannot link against the procedural
    // particle VS, so the PSO fails and the emitter silently vanishes); such an entity
    // uses rc's look.
    auto* pMeshRef = world.GetPool<MeshLibRef>();
    auto lookMaterialOf = [&](Entity e) -> MaterialComponent* {
        MaterialComponent* mc = world.GetComponent<MaterialComponent>(e);
        if (!mc) return nullptr;
        if (pMeshRef && pMeshRef->Get(e)) return nullptr;
        // Decal-domain materials author a projected decal, never an emitter look.
        if (mc->IsDecalDomain()) return nullptr;
        // Water-shading-model materials author a water SURFACE (WaterSurfacePass) and
        // compile a 4-MRT main: in a pool-VS particle bucket the PSO would fail and the
        // emitter silently vanish.
        if (mc->IsWaterShadingModel()) return nullptr;
        return mc;
    };

    // Bucket-key inputs from appearance. lit forces TAA on (lit detail wants temporal
    // stabilization). With a material: blend = its blend mode UNLESS Opaque (the
    // material default, meaning "unset" for pool VFX: keep rc.blendMode, so adding an
    // empty Material never changes an emitter's look). The material's CUSTOM SHADER
    // also picks PS / VS and lit from its shading model; without one, rc's shader / lit
    // fields apply.
    auto bucketKey = [](const ParticleRenderComponent& rc, const MaterialComponent* mc,
                        uint32_t& blend, uint32_t& taa, uint32_t& lit,
                        uint32_t& recvShadow,
                        const std::string*& vs, const std::string*& ps) {
        static const std::string s_empty;
        blend = static_cast<uint32_t>(rc.blendMode);
        if (mc)
        {
            switch (mc->GetBlendMode())
            {
                case BlendMode::Alpha:         blend = 0u; break;
                case BlendMode::Additive:      blend = 1u; break;
                case BlendMode::Premultiplied: blend = 2u; break;
                case BlendMode::Multiply:      blend = 3u; break;
                default: break;                              // Opaque = unset -> rc
            }
        }
        const bool mcFx = mc && mc->useCustomShader && !mc->customShaderPath.empty();
        if (mcFx)
        {
            lit = (mc->customShadingModel != ShadingModel::Unlit) ? 1u : 0u;
            // Even on a custom-shader emitter the shadow opt-in comes from the
            // EMITTER, not the material. The material's Receive Shadow flag
            // defaults to true and governs meshes; honouring it here would turn
            // shadows on for every existing custom lit particle at once.
            recvShadow = (lit && rc.receiveShadow) ? 1u : 0u;
            taa = (lit ? false : rc.excludeFromTAA) ? 1u : 0u;
            vs  = &mc->customVertexShaderPath;   // "" = engine default VS
            ps  = &mc->customShaderPath;
            return;
        }
        lit   = rc.lit ? 1u : 0u;
        // `lit &&` so an emitter that ticked receiveShadow without lit cannot mint
        // a pointless extra PSO whose shader ignores the define.
        recvShadow = (rc.lit && rc.receiveShadow) ? 1u : 0u;
        taa   = (rc.lit ? false : rc.excludeFromTAA) ? 1u : 0u;
        vs = rc.useCustomShader ? &rc.customVertexPath  : &s_empty;
        ps = rc.useCustomShader ? &rc.customSurfacePath : &s_empty;
    };

    // Resolve every "Tex_*" slot of a driving material to a bindless index (entity+name
    // keyed Acquire/Release cache, as in TrailSystem).
    auto resolveCustomTextures = [&](Entity e, const MaterialComponent& mc) {
        if (!m_texSys || !m_resMgr || !m_gfx || mc.customTextures.empty()) return;
        auto& slots   = m_customTexCache[e];
        auto& indices = m_customTexIndices[e];
        for (const auto& [name, texMap] : mc.customTextures)
        {
            const std::string& path = texMap.name;
            auto& entry = slots[name];
            if (path != entry.path)
            {
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, *m_gfx);
                entry.path   = path;
                entry.handle = path.empty()
                    ? Resource::kInvalidTextureHandle
                    : m_texSys->Acquire(path, *m_resMgr, *m_gfx);
                indices[name] = -1;
            }
            auto idxIt = indices.find(name);
            const bool unresolved = (idxIt == indices.end() || idxIt->second < 0);
            if (unresolved
                && entry.handle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(entry.handle))
                if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                    indices[name] = static_cast<int32_t>(tex->handleId);
        }
    };

    // Packs one bucket's b8 CBV from the FIRST claiming emitter's material
    // (first-emitter-wins, like window / gravity). Reflection comes from the render
    // pass's ShaderLibrary so IDs and layout agree with the PSO.
    //   no driving custom shader       -> nothing to do
    //   shader not compiled/reflected  -> paramReady = false (the pass skips the bucket:
    //                                     drawing with an unbound b8 the shader reads would fault)
    //   no b8 declared                 -> ready, nothing to bind
    //   ring full                      -> paramReady = false
    auto packBucketParams = [&](uint32_t bucketId, Entity e, MaterialComponent* mc) {
        // Only a material whose CUSTOM SHADER drives the bucket has b8 params; a
        // blend / texture-only material leaves the default shaders (and rc custom shaders)
        // untouched.
        if (!mc || !mc->useCustomShader || mc->customShaderPath.empty()) return;
        ParticleBucket& bk = m_buckets[bucketId];
        if (bk.paramPacked) return;         // first-emitter-wins per frame
        bk.paramPacked = true;
        if (!m_shaderLib) { bk.paramReady = false; return; }

        // Ids were resolved at bucket claim (FindOrCreateBucket); reflection
        // pointers cache per bucket until the library's reload epoch bumps.
        // Steady state here: one epoch compare + two pointer checks, no lock.
        const uint32_t epoch = m_shaderLib->GetReloadEpoch();
        if (bk.reflEpoch != epoch)
        {
            bk.reflPS = bk.reflVS = nullptr;
            bk.reflEpoch = epoch;
        }
        if (!bk.reflPS && bk.dynPsId != ShaderLibrary::kInvalidDynShaderID)
            bk.reflPS = m_shaderLib->GetDynamicReflection(bk.dynPsId);
        if (!bk.reflVS && bk.dynVsId != ShaderLibrary::kInvalidDynShaderID)
            bk.reflVS = m_shaderLib->GetDynamicReflection(bk.dynVsId);
        const ShaderReflect::Reflection* reflPS = bk.reflPS;
        const ShaderReflect::Reflection* reflVS = bk.reflVS;
        if (!reflPS && !reflVS) { bk.paramReady = false; return; }   // still compiling

        // Same annotation / Tex_ machinery as mesh materials and trails, epoch/path-stamped
        // so the string-map walk runs once, not per frame (MaterialReflectionSync::StageSynced).
        if (reflPS && !MaterialReflectionSync::StageSynced(*mc, epoch, /*vs*/ false))
        {
            MaterialReflectionSync::Sync(*mc, *reflPS);
            MaterialReflectionSync::MarkStageSynced(*mc, epoch, /*vs*/ false);
        }
        if (reflVS && !MaterialReflectionSync::StageSynced(*mc, epoch, /*vs*/ true))
        {
            MaterialReflectionSync::Sync(*mc, *reflVS);
            MaterialReflectionSync::MarkStageSynced(*mc, epoch, /*vs*/ true);
        }
        // Both declared stages have synced: clear customParamsLegacyFormat here, since a
        // mesh-less emitter entity never runs Renderer::ResolveCustomVSID (the mesh path's
        // clear site) and a set flag re-runs the sentinel migration every frame.
        if (reflPS && (mc->customVertexShaderPath.empty() || reflVS))
            mc->customParamsLegacyFormat = false;
        resolveCustomTextures(e, *mc);

        const ShaderReflect::Reflection* refls[2] = { reflPS, reflVS };
        uint32_t cbSize = 0;
        for (const auto* r : refls)
        {
            if (!r) continue;
            for (const auto& cb : r->cbuffers)
                if (cb.bindPoint == 8u && cb.space == 0u &&
                    !MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str()) &&
                    cb.sizeBytes > cbSize)
                    cbSize = cb.sizeBytes;
        }
        if (cbSize == 0) return;            // no b8 declared: draw, no bind

        auto slice = m_paramRing.Allocate(cbSize);
        if (!slice.cpu) { bk.paramReady = false; return; }
        std::memset(slice.cpu, 0, cbSize);
        const auto* texIndices = [&]() -> const std::unordered_map<std::string, int32_t>* {
            auto it = m_customTexIndices.find(e);
            return it != m_customTexIndices.end() ? &it->second : nullptr;
        }();
        for (const auto* r : refls)
        {
            if (!r) continue;
            for (const auto& cb : r->cbuffers)
            {
                if (cb.bindPoint != 8u || cb.space != 0u) continue;
                if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                for (const auto& v : cb.vars)
                {
                    // "Tex_*" slots: bindless index as a float VALUE, -1 = unbound.
                    if (v.name.rfind("Tex_", 0) == 0)
                    {
                        float idx = -1.0f;
                        if (texIndices)
                            if (auto ti = texIndices->find(v.name); ti != texIndices->end())
                                idx = static_cast<float>(ti->second);
                        if (v.offset + sizeof(float) <= cbSize)
                            std::memcpy(static_cast<uint8_t*>(slice.cpu) + v.offset,
                                        &idx, sizeof(float));
                        continue;
                    }
                    const float* src = nullptr;
                    if (auto it = mc->customParams.find(v.name); it != mc->customParams.end())
                        src = it->second.data();
                    else if (auto mit = mc->customParamMeta.find(v.name);
                             mit != mc->customParamMeta.end())
                        src = mit->second.defaultValue.data();
                    if (!src) continue;
                    const uint32_t copy = (v.size < 16u) ? v.size : 16u;
                    if (v.offset + copy <= cbSize)
                        std::memcpy(static_cast<uint8_t*>(slice.cpu) + v.offset, src, copy);
                }
            }
        }
        bk.paramCbvVA = slice.gpu;
    };

    // Pack shared appearance into a GPU emitter record (colour/size/scale/
    // intensity/fade/stretch/visualMode/texture). Motion + shape are per-behaviour.
    auto packAppearance = [](const ParticleRenderComponent& rc, ParticleEmitterGPU* slot) {
        slot->startColor         = rc.startColor;
        slot->endColor           = rc.endColor;
        slot->startSize          = rc.startSize;
        slot->startScale         = rc.startScale;
        slot->intensity          = rc.intensity;
        slot->emissiveFade       = rc.emissiveFade;
        slot->velocityStretch    = rc.velocityStretch;
        slot->visualMode         = static_cast<uint32_t>(rc.visualMode);
        slot->textureBindlessIdx = (rc.textureBindlessIdx < 0)
                                   ? 0xFFFFFFFFu : static_cast<uint32_t>(rc.textureBindlessIdx);
    };

    // Allocate + zero the next per-emitter upload slot.
    auto writeSlot = [&]() -> ParticleEmitterGPU* {
        uint8_t* bytes = static_cast<uint8_t*>(emitterMapped)
                       + m_emitterCountThisFrame * kEmitterSlotStride;
        std::memset(bytes, 0, kEmitterSlotStride);
        return reinterpret_cast<ParticleEmitterGPU*>(bytes);
    };

    // ---- Ambient pre-pass ----
    // Reconcile the persistent volume slots and build the reserved-bucket mask BEFORE
    // loops A/B, so ordinary and flock emitters (whatever their order) avoid the fixed
    // slices of ambient volumes (slot v -> bucket kAmbientBucketBase + v). A slot is held
    // while the component exists and freed on component removal, destroy or world clear.
    if (auto* pAmb = world.GetPool<ParticleAmbientComponent>())
    {
        for (auto it = m_ambientSlotByEntity.begin(); it != m_ambientSlotByEntity.end(); )
        {
            if (!world.GetComponent<ParticleAmbientComponent>(it->first))
                it = m_ambientSlotByEntity.erase(it);
            else ++it;
        }
        auto& aents = pAmb->Entities();
        auto& adata = pAmb->Data();
        const size_t an = adata.size();
        for (size_t i = 0; i < an; ++i)
        {
            const Entity e = aents[i];
            const ParticleAmbientComponent& ac = adata[i];
            uint32_t vslot;
            auto it = m_ambientSlotByEntity.find(e);
            if (it != m_ambientSlotByEntity.end())
                vslot = it->second;
            else
            {
                uint32_t used = 0;
                for (const auto& kv : m_ambientSlotByEntity) used |= (1u << kv.second);
                int free = -1;
                for (uint32_t s = 0; s < kMaxAmbientVolumes; ++s)
                    if (!(used & (1u << s))) { free = static_cast<int>(s); break; }
                if (free < 0)
                {
                    LOG_WARNING("ParticleSystem: >%u ambient volumes; entity %u ignored this session",
                                kMaxAmbientVolumes, static_cast<uint32_t>(e));
                    continue;
                }
                vslot = static_cast<uint32_t>(free);
                m_ambientSlotByEntity[e] = vslot;
            }
            if (ac.enabled)
                m_ambientReservedBucketMask |= (1u << (kAmbientBucketBase + vslot));
        }
    }

    // ==== Loop A: STANDARD emitters (spawn from a shape) ====
    if (auto* pEmit = world.GetPool<ParticleEmitterComponent>())
    {
        auto& ents = pEmit->Entities();
        auto& data = pEmit->Data();
        const size_t n = data.size();
        bool gravitySampled = false;   // first spawning standard emitter wins
        for (size_t i = 0; i < n && m_emitterCountThisFrame < kMaxEmittersPerFrame; ++i)
        {
            const Entity e = ents[i];
            ParticleEmitterComponent& ec = data[i];

            // A just-disabled emitter keeps CLAIMING ITS BUCKET until the particles it spawned
            // have lived out their lifetime: buckets are re-derived every frame and an
            // unclaimed slice is neither simulated nor drawn, so bailing out when `enabled`
            // goes false would make every live spark vanish on the frame an animation-gated
            // weapon effect switches off. Draining costs one bucket claim per frame and spawns
            // nothing.
            const bool draining = !ec.enabled;
            if (draining)
            {
                // Re-seed the Trail shape's swept chord on re-enable: prevTipWorld still holds the
                // pose from before the emitter was switched off, and the first frame back would
                // smear one chord across the gap.
                ec.hasPrevTip       = false;
                ec.spawnAccumulator = 0.0f;
                if (ec.drainTimer <= 0.0f) continue;
                ec.drainTimer -= dt;
            }
            else
            {
                // Worst-case survival time of anything spawned this frame.
                ec.drainTimer = ec.startLifetime + ec.lifeVariance;
            }

            ParticleRenderComponent* rcp = world.GetComponent<ParticleRenderComponent>(e);
            ParticleRenderComponent  rcLocal = kDefaultRender;
            ParticleRenderComponent& rc = rcp ? *rcp : rcLocal;
            MaterialComponent* mcLook = lookMaterialOf(e);
            resolveTexture(e, &rc, emitterTexPathOf(rc, mcLook));

            uint32_t blend, taa, lit, recvShadow; const std::string *vs, *ps;
            bucketKey(rc, mcLook, blend, taa, lit, recvShadow, vs, ps);
            const uint32_t bucketId   = FindOrCreateBucket(*vs, *ps, blend, taa, lit, 0u, recvShadow);
            const uint32_t bucketBase = bucketId * kParticlesPerBucket;
            packBucketParams(bucketId, e, mcLook);

            // Bucket held, nothing emitted. Skipping the emitter slot entirely
            // is the same path the swing seed-frame / teleport guard below
            // already take, so the sim and draw ranges stay correct.
            if (draining) continue;

            // ---- Trail shape: sample the swept blade-tip chord ----
            // worldPosOf() is translation-ONLY, which would miss a blade that spins about a
            // near-stationary socket, so sample the authored tip through the full world matrix.
            // prevTipWorld is last frame's tip, so (prev -> cur) is exactly one frame of swing.
            const bool swingMode = (ec.shape == ParticleShape::Trail);
            DirectX::XMFLOAT3 swingTip{ 0.0f, 0.0f, 0.0f };
            DirectX::XMFLOAT3 swingPrev{ 0.0f, 0.0f, 0.0f };
            DirectX::XMFLOAT3 swingVel{ 0.0f, 0.0f, 0.0f };
            float swingDist = 0.0f, swingSpeed = 0.0f;
            if (swingMode)
            {
                const GlobalTransform* gt = pGlobal ? pGlobal->Get(e) : nullptr;
                if (!gt) continue;                       // no transform -> no arc
                DirectX::XMStoreFloat3(&swingTip, DirectX::XMVector3TransformCoord(
                    DirectX::XMLoadFloat3(&ec.bladeTipLocal),
                    DirectX::XMLoadFloat4x4(&gt->matrix)));

                if (!ec.hasPrevTip)
                {
                    // Seed frame (spawn / world load / re-enable): emit nothing,
                    // else the first chord smears from wherever prevTip happened
                    // to be (typically the world origin).
                    ec.prevTipWorld = swingTip;
                    ec.hasPrevTip = true;
                    ec.spawnAccumulator = 0.0f;
                    continue;
                }
                swingPrev = ec.prevTipWorld;
                const float dx = swingTip.x - swingPrev.x;
                const float dy = swingTip.y - swingPrev.y;
                const float dz = swingTip.z - swingPrev.z;
                swingDist  = std::sqrt(dx * dx + dy * dy + dz * dz);
                const float invDt = (dt > 1e-6f) ? (1.0f / dt) : 0.0f;
                swingSpeed = swingDist * invDt;

                // A teleport (socket retarget, world reload, possession switch, long hitch) is not
                // a swing: it would smear one chord across the map and dump a whole ring window of
                // sparks. Guard on BOTH speed and chord length: a speed test alone lets a big jump
                // through on a long-dt frame (40 m across a 0.5 s stall is only 80 m/s), and
                // spawnCount below is driven by DISTANCE.
                constexpr float kMaxSwingSpeed = 100.0f;   // m/s
                constexpr float kMaxSwingChord = 5.0f;     // m travelled in ONE frame
                if (swingSpeed > kMaxSwingSpeed || swingDist > kMaxSwingChord)
                {
                    ec.prevTipWorld = swingTip;
                    ec.spawnAccumulator = 0.0f;
                    continue;
                }
                // Flush near-zero components: tiny magnitudes reaching a float
                // CB slot as denormals are flush-to-zero territory on some
                // drivers, and a jittering sign there reads as spark noise.
                auto flushSmall = [](float v) { return (std::fabs(v) < 1e-6f) ? 0.0f : v; };
                swingVel = { flushSmall(dx * invDt), flushSmall(dy * invDt), flushSmall(dz * invDt) };

                // Advance UNCONDITIONALLY from here on, so the chord is always
                // exactly one frame of travel no matter which gate skips below.
                ec.prevTipWorld = swingTip;
            }

            // One-shot burst + continuous spawn.
            uint32_t burstNow = 0;
            if (ec.burstTrigger && ec.burstCount > 0) burstNow = static_cast<uint32_t>(ec.burstCount);
            ec.burstTrigger = false;
            uint32_t spawnCount = 0;
            if (swingMode)
            {
                // Distance-driven so fast and slow swings lay down the same
                // spatial density along the arc (a per-second rate would bunch
                // sparks up on a slow swing and thin them on a fast one).
                // Under minSwingSpeed the blade is not swinging: emit nothing,
                // and DROP the accumulator so a long hold cannot dump a backlog
                // the instant the swing starts.
                if (swingSpeed >= ec.minSwingSpeed && ec.sparksPerMeter > 0.0f)
                {
                    ec.spawnAccumulator += ec.sparksPerMeter * swingDist;
                    spawnCount = static_cast<uint32_t>(ec.spawnAccumulator);
                    ec.spawnAccumulator -= static_cast<float>(spawnCount);
                }
                else ec.spawnAccumulator = 0.0f;
            }
            else if (ec.spawnRate > 0.0f)
            {
                ec.spawnAccumulator += ec.spawnRate * dt;
                spawnCount = static_cast<uint32_t>(ec.spawnAccumulator);
                ec.spawnAccumulator -= static_cast<float>(spawnCount);
            }
            spawnCount += burstNow;
            if (spawnCount == 0) continue;

            const uint32_t desiredWin = static_cast<uint32_t>(
                std::min(std::max(ec.maxParticles, 1), static_cast<int>(kParticlesPerBucket)));
            if (m_buckets[bucketId].window == 0) m_buckets[bucketId].window = desiredWin;
            m_buckets[bucketId].drawCount = m_buckets[bucketId].window;
            const uint32_t win = m_buckets[bucketId].window;
            if (spawnCount > kParticlesPerBucket) spawnCount = kParticlesPerBucket;
            if (spawnCount > win) spawnCount = win;

            ParticleEmitterGPU* slot = writeSlot();
            // Trail emits from the CURRENT blade tip (the chord's END); every
            // other shape from the entity origin.
            slot->position      = swingMode ? swingTip : worldPosOf(e);
            slot->startLifetime = ec.startLifetime;
            slot->velocityMin   = ec.velocityMin;
            slot->velocityMax   = ec.velocityMax;
            slot->gravity       = ec.gravity;
            slot->drag          = ec.drag;
            slot->sizeVariance  = ec.sizeVariance;
            slot->lifeVariance  = ec.lifeVariance;
            slot->spawnCount    = spawnCount;
            slot->writeCursor   = bucketBase + m_bucketCursor[bucketId];
            slot->randomSeed    = frameIndex * 2654435761u + static_cast<uint32_t>(e);
            slot->shapeType     = static_cast<uint32_t>(ec.shape);
            slot->orientation   = static_cast<uint32_t>(rc.orientation);   // no flock bit
            slot->maxLive       = win;
            packAppearance(rc, slot);

            slot->shapeParam0 = { 0, 0, 0, 0 };
            slot->shapeParam1 = { 0, 0, 0, 0 };
            switch (ec.shape)
            {
            case ParticleShape::Sphere:
                slot->shapeParam0 = { ec.sphereRadius, ec.sphereSpawnOnShell ? 1.0f : 0.0f, 0.0f, 0.0f };
                break;
            case ParticleShape::Cone:
                slot->shapeParam0 = { 0.0f, ec.coneHalfAngle, 0.0f, 0.0f };
                slot->shapeParam1 = { ec.coneDirection.x, ec.coneDirection.y, ec.coneDirection.z, ec.coneLength };
                break;
            case ParticleShape::Box:
                slot->shapeParam0 = { ec.boxHalfExtents.x, ec.boxHalfExtents.y, ec.boxHalfExtents.z, 0.0f };
                break;
            case ParticleShape::Circle:
                slot->shapeParam0 = { ec.circleRadius, 0.0f, 0.0f, 0.0f };
                slot->shapeParam1 = { ec.circleNormal.x, ec.circleNormal.y, ec.circleNormal.z, 0.0f };
                break;
            case ParticleShape::Mesh:
                m_pendingMeshResolves.push_back({ m_emitterCountThisFrame, ec.meshSourceEntity });
                break;
            case ParticleShape::Trail:
                // p0.xyz = chord START (previous tip); p1.xyz = tip velocity
                // handed to every spark, p1.w = how much of it to inherit.
                // The chord END is slot->position (current tip), set above.
                slot->shapeParam0 = { swingPrev.x, swingPrev.y, swingPrev.z, 0.0f };
                slot->shapeParam1 = { swingVel.x, swingVel.y, swingVel.z,
                                      std::clamp(ec.inheritVelocity, 0.0f, 4.0f) };
                break;
            default: break; // Point: no params
            }
            DirectX::XMStoreFloat4x4(&slot->meshWorldMatrix, DirectX::XMMatrixIdentity());
            slot->meshDescSlot   = 0xFFFFFFFFu;
            slot->meshIndexCount = 0;

            m_bucketCursor[bucketId] = (m_bucketCursor[bucketId] + spawnCount) % win;
            m_buckets[bucketId].liveCount += spawnCount;
            m_emitterSpawnCounts.push_back(spawnCount);
            if (!gravitySampled) { m_globalGravity = ec.gravity; gravitySampled = true; }
            ++m_emitterCountThisFrame;
        }
    }

    // ==== Loop B: FLOCK (Boids) ====
    if (auto* pFlock = world.GetPool<ParticleFlockComponent>())
    {
        auto& ents = pFlock->Entities();
        auto& data = pFlock->Data();
        const size_t n = data.size();
        for (size_t i = 0; i < n && m_emitterCountThisFrame < kMaxEmittersPerFrame; ++i)
        {
            const Entity e = ents[i];
            ParticleFlockComponent& fc = data[i];
            if (!fc.enabled) continue;

            ParticleRenderComponent* rcp = world.GetComponent<ParticleRenderComponent>(e);
            ParticleRenderComponent  rcLocal = kDefaultRender;
            ParticleRenderComponent& rc = rcp ? *rcp : rcLocal;
            MaterialComponent* mcLook = lookMaterialOf(e);
            resolveTexture(e, &rc, emitterTexPathOf(rc, mcLook));

            uint32_t blend, taa, lit, recvShadow; const std::string *vs, *ps;
            bucketKey(rc, mcLook, blend, taa, lit, recvShadow, vs, ps);
            const uint32_t bucketId   = FindOrCreateBucket(*vs, *ps, blend, taa, lit, 1u, recvShadow);
            const uint32_t bucketBase = bucketId * kParticlesPerBucket;
            packBucketParams(bucketId, e, mcLook);
            const DirectX::XMFLOAT3 home = worldPosOf(e);

            // Widen the boid dispatch range for THIS flock (every enabled flock, not just the
            // first-wins params donor: a second flock in a higher bucket must still be
            // snapshotted and steered). Top is raised, base lowered; both are monotonic, so
            // [base, top) stays a superset of every slot any flock ever occupied (the steer
            // shrink-kill relies on it).
            {
                const uint32_t fcClamped = static_cast<uint32_t>(
                    std::min(std::max(fc.flockCount, 0), static_cast<int>(kParticlesPerBucket)));
                const uint32_t flockEnd = bucketBase + fcClamped;
                if (flockEnd > m_boidSimHighWater) m_boidSimHighWater = flockEnd;
                if (bucketBase < m_boidSimLowWater) m_boidSimLowWater = bucketBase;
            }

            // Sample steering + appearance into the global BoidParams CB (first
            // flock wins). Every frame so inspector edits are live.
            if (!m_flockActive)
            {
                m_flockActive = true;
                const float cellSize = std::max(fc.perceptionRadius, 0.01f);
                const float gridHalf = 0.5f * static_cast<float>(kBoidGridDim) * cellSize;
                m_boidParams.deltaTime        = dt;
                m_boidParams.particleCount    = kMaxGlobalParticles;
                m_boidParams.sliceLen         = kParticlesPerBucket;
                m_boidParams.perceptionRadius = fc.perceptionRadius;
                m_boidParams.separationRadius = fc.separationRadius;
                m_boidParams.separationWeight = fc.separationWeight;
                m_boidParams.alignmentWeight  = fc.alignmentWeight;
                m_boidParams.cohesionWeight   = fc.cohesionWeight;
                m_boidParams.maxSpeed         = fc.maxSpeed;
                m_boidParams.maxForce         = fc.maxForce;
                m_boidParams.boundsWeight     = fc.boundsWeight;
                m_boidParams.boundsRadius     = fc.boundsRadius;
                m_boidParams.boundsCenter     = home;
                m_boidParams.startColor       = rc.startColor;
                m_boidParams.startScale       = rc.startScale;
                m_boidParams.startSize        = rc.startSize;
                m_boidParams.intensity        = rc.intensity;
                m_boidParams.visualMode       = static_cast<uint32_t>(rc.visualMode);
                m_boidParams.orientation      = static_cast<uint32_t>(rc.orientation);
                m_boidParams.textureBindlessIdx = (rc.textureBindlessIdx < 0)
                                                ? 0xFFFFFFFFu : static_cast<uint32_t>(rc.textureBindlessIdx);
                m_boidParams.cellSize   = cellSize;
                m_boidParams.gridOrigin = { home.x - gridHalf, home.y - gridHalf, home.z - gridHalf };
                m_boidParams.flockCount = static_cast<uint32_t>(
                    std::min(std::max(fc.flockCount, 0), static_cast<int>(kParticlesPerBucket)));
            }

            // A flock owns its whole slice; drawn every frame (drawCount persists).
            if (m_buckets[bucketId].window == 0) m_buckets[bucketId].window = kParticlesPerBucket;
            m_buckets[bucketId].drawCount = m_buckets[bucketId].window;
            const uint32_t win = m_buckets[bucketId].window;

            // (Re)spawn once, and whenever flockCount changes (structural).
            uint32_t flockBurst = 0;
            if (!fc.flockSpawned || fc.flockCount != fc.flockSpawnedCount)
            {
                flockBurst = static_cast<uint32_t>(fc.flockCount < 0 ? 0 : fc.flockCount);
                if (flockBurst > kParticlesPerBucket) flockBurst = kParticlesPerBucket;
                fc.flockSpawned      = true;
                fc.flockSpawnedCount = fc.flockCount;
                m_bucketCursor[bucketId] = 0;
            }
            if (flockBurst == 0) continue;   // steady flock: bucket used + drawn, no upload slot

            uint32_t spawnCount = (flockBurst > win) ? win : flockBurst;

            ParticleEmitterGPU* slot = writeSlot();
            slot->position      = home;
            slot->startLifetime = 1.0f;                         // positive; steer re-arms every frame
            const float vspread = 0.3f * std::max(fc.maxSpeed, 0.1f);
            slot->velocityMin   = { -vspread, -vspread, -vspread };
            slot->velocityMax   = {  vspread,  vspread,  vspread };
            slot->gravity       = { 0.0f, 0.0f, 0.0f };
            slot->drag          = 0.0f;
            slot->spawnCount    = spawnCount;
            slot->writeCursor   = bucketBase + m_bucketCursor[bucketId];
            slot->randomSeed    = frameIndex * 2654435761u + static_cast<uint32_t>(e);
            slot->shapeType     = static_cast<uint32_t>(ParticleShape::Point);   // spawn at home; steer spreads
            slot->orientation   = static_cast<uint32_t>(rc.orientation) | 0x100u; // FLOCK bit
            slot->maxLive       = win;
            packAppearance(rc, slot);
            slot->shapeParam0 = { 0, 0, 0, 0 };
            slot->shapeParam1 = { 0, 0, 0, 0 };
            DirectX::XMStoreFloat4x4(&slot->meshWorldMatrix, DirectX::XMMatrixIdentity());
            slot->meshDescSlot   = 0xFFFFFFFFu;
            slot->meshIndexCount = 0;

            m_bucketCursor[bucketId] = (m_bucketCursor[bucketId] + spawnCount) % win;
            m_buckets[bucketId].liveCount += spawnCount;
            m_emitterSpawnCounts.push_back(spawnCount);
            ++m_emitterCountThisFrame;
        }
    }

    // ==== Loop C: AMBIENT (camera-follow toroidal wrap volume) ====
    if (auto* pAmb = world.GetPool<ParticleAmbientComponent>())
    {
        auto& ents = pAmb->Entities();
        auto& data = pAmb->Data();
        const size_t n = data.size();
        for (size_t i = 0; i < n && m_emitterCountThisFrame < kMaxEmittersPerFrame; ++i)
        {
            const Entity e = ents[i];
            ParticleAmbientComponent& ac = data[i];
            if (!ac.enabled) continue;
            auto slotIt = m_ambientSlotByEntity.find(e);
            if (slotIt == m_ambientSlotByEntity.end()) continue;   // no volume slot (pool full)
            const uint32_t vslot = slotIt->second;

            ParticleRenderComponent* rcp = world.GetComponent<ParticleRenderComponent>(e);
            ParticleRenderComponent  rcLocal = kDefaultRender;
            ParticleRenderComponent& rc = rcp ? *rcp : rcLocal;
            MaterialComponent* mcLook = lookMaterialOf(e);
            resolveTexture(e, &rc, emitterTexPathOf(rc, mcLook));

            uint32_t blend, taa, lit, recvShadow; const std::string *vs, *ps;
            bucketKey(rc, mcLook, blend, taa, lit, recvShadow, vs, ps);

            // Fixed slice per volume slot, stable across frames; claimed in place.
            const uint32_t bucketId   = kAmbientBucketBase + vslot;
            const uint32_t bucketBase = bucketId * kParticlesPerBucket;
            {
                ParticleBucket& bk = m_buckets[bucketId];
                bk.customVertexPath  = *vs;
                bk.customSurfacePath = *ps;
                bk.blendMode  = blend;
                bk.taaExclude = taa;
                bk.lit        = lit;
                bk.flocking   = 0u;
                bk.ambient    = 1u + vslot;
                bk.bucketBase = bucketBase;
                bk.used       = true;
                if (bucketId + 1 > m_bucketCount) m_bucketCount = bucketId + 1;
            }
            packBucketParams(bucketId, e, mcLook);

            const DirectX::XMFLOAT3 H = { std::max(ac.ambientExtents.x, 0.01f),
                                          std::max(ac.ambientExtents.y, 0.01f),
                                          std::max(ac.ambientExtents.z, 0.01f) };
            const DirectX::XMFLOAT3 center = { m_cameraPos.x + ac.ambientOffset.x,
                                               m_cameraPos.y + ac.ambientOffset.y,
                                               m_cameraPos.z + ac.ambientOffset.z };
            const uint32_t pop = ComputeAmbientCount(ac, H);

            AmbientVolumeCPU& av = m_ambientVolumes[vslot];
            av.halfExt   = H;
            av.edgeFade  = std::max(ac.ambientEdgeFade, 0.0f);
            av.center    = center;
            av.count     = static_cast<float>(pop);
            av.wind      = ac.ambientWind;
            av.nearFade0 = std::max(ac.ambientNearFade0, 0.0f);
            av.nearFade1 = std::max(ac.ambientNearFade1, ac.ambientNearFade0 + 1e-3f);
            av.bucketId  = bucketId;
            av.intensity = std::clamp(ac.ambientIntensity, 0.0f, 1.0f);
            // Mirror the sibling render appearance so ParticleUpdate.cs re-applies it live to
            // every ambient particle; refreshed every active frame, including steady no-emit
            // frames (the `continue` below comes after this).
            av.apStartColor   = rc.startColor;
            av.apEndColor     = rc.endColor;
            av.apStartScale   = rc.startScale;
            av.apStartSize    = rc.startSize;
            av.apIntensity    = rc.intensity;
            av.apEmissiveFade = rc.emissiveFade;
            av.apStretch      = rc.velocityStretch;
            av.apVisualMode   = static_cast<uint32_t>(rc.visualMode);
            av.apOrientation  = static_cast<uint32_t>(rc.orientation);
            av.apTextureIdx   = rc.textureBindlessIdx;
            m_ambientActiveMask |= (1u << vslot);
            m_ambientActiveThisFrame[vslot] = true;

            m_buckets[bucketId].window    = pop;
            m_buckets[bucketId].drawCount = pop;

            const uint64_t key       = AmbientRefillKey(H, ac.ambientCount, ac.ambientDensity);
            const bool     wasActive = m_ambientActiveLastFrame[vslot];
            uint32_t ambientBurst = 0;
            if (!ac.ambientSpawned || ac.ambientKey != key || !wasActive)
            {
                ambientBurst      = pop;
                ac.ambientSpawned = true;
                ac.ambientKey     = key;
                m_bucketCursor[bucketId] = 0;   // fill from the slice start
            }
            if (ambientBurst == 0) continue;   // steady: bucket used + drawn, no upload slot

            const uint32_t win = pop;
            uint32_t spawnCount = (ambientBurst > win) ? win : ambientBurst;

            ParticleEmitterGPU* slot = writeSlot();
            slot->position      = center;
            slot->startLifetime = ac.startLifetime;
            slot->velocityMin   = ac.velocityMin;
            slot->velocityMax   = ac.velocityMax;
            slot->gravity       = { 0.0f, 0.0f, 0.0f };   // ambient ignores gravity
            slot->drag          = ac.drag;
            slot->spawnCount    = spawnCount;
            slot->writeCursor   = bucketBase + m_bucketCursor[bucketId];
            slot->randomSeed    = frameIndex * 2654435761u + static_cast<uint32_t>(e);
            slot->shapeType     = static_cast<uint32_t>(ParticleShape::Box);   // uniform box fill
            slot->shapeParam0   = { H.x, H.y, H.z, 0.0f };
            slot->shapeParam1   = { 0, 0, 0, 0 };
            slot->orientation   = static_cast<uint32_t>(rc.orientation);       // no flock bit
            slot->maxLive       = win;
            slot->ambientFlags  = 1u | (vslot << 1);   // bit0 = ambient, bits1-2 = slot
            packAppearance(rc, slot);
            DirectX::XMStoreFloat4x4(&slot->meshWorldMatrix, DirectX::XMMatrixIdentity());
            slot->meshDescSlot   = 0xFFFFFFFFu;
            slot->meshIndexCount = 0;

            m_bucketCursor[bucketId] = (m_bucketCursor[bucketId] + spawnCount) % win;
            m_buckets[bucketId].liveCount += spawnCount;
            m_emitterSpawnCounts.push_back(spawnCount);
            ++m_emitterCountThisFrame;
        }
    }

    // Latch this frame's ambient activity for next frame's inactive -> active check (a
    // re-enabled volume refills).
    for (uint32_t v = 0; v < kMaxAmbientVolumes; ++v)
        m_ambientActiveLastFrame[v] = m_ambientActiveThisFrame[v];

    // Raise the update-dispatch bound past every bucket's active window. Read
    // from ALL buckets (not just the ones used this frame) so a bucket whose
    // emitter went quiet keeps aging its lingering particles.
    for (uint32_t b = 0; b < kMaxParticleBuckets; ++b)
    {
        const uint32_t end = m_buckets[b].bucketBase + m_buckets[b].drawCount;
        if (end > m_simHighWater) m_simHighWater = end;
    }
    if (m_simHighWater > kMaxGlobalParticles) m_simHighWater = kMaxGlobalParticles;
    if (m_boidSimHighWater > kMaxGlobalParticles) m_boidSimHighWater = kMaxGlobalParticles;

    // Push the sampled flock params to the GPU (consumed by the snapshot + steer
    // passes). deltaTime is refreshed here in case it changed since sampling.
    if (m_flockActive && m_gfx)
    {
        m_boidParams.deltaTime = dt;
        // dispatchBase must cover EVERY flock, not just the first-wins params
        // donor, so it is taken from the low-water here rather than at sample time.
        m_boidParams.dispatchBase = (m_boidSimLowWater < m_boidSimHighWater)
                                  ? m_boidSimLowWater : 0u;
        if (auto* bp = m_boidCB.Upload(*m_gfx)) *bp = m_boidParams;   // game half: Upload, not Current
    }
}

void ParticleSystem::PatchMeshEmitter(uint32_t slotIndex,
                                       uint32_t meshDescSlot,
                                       uint32_t meshIndexCount,
                                       const DirectX::XMFLOAT4X4& worldMatrix)
{
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetUploadSlot();   // the slot CollectEmitters wrote
    if (frameSlot >= kFrameCount || !m_emitterMapped[frameSlot]) return;
    if (slotIndex >= m_emitterCountThisFrame) return;

    ParticleEmitterGPU* slot = reinterpret_cast<ParticleEmitterGPU*>(
        static_cast<uint8_t*>(m_emitterMapped[frameSlot])
        + slotIndex * kEmitterSlotStride);

    slot->meshDescSlot    = meshDescSlot;
    slot->meshIndexCount  = meshIndexCount;
    slot->meshWorldMatrix = worldMatrix;
}
