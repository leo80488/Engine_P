#pragma once

// TracerSystem - GPU pool of cylindrical-billboard tracer beams (thin lasers, gun tracers,
// far energy shots): thousands of cheap short-lived events. Thick PBR beams use BeamSystem.
//
// Spawn() queues a record on a CPU vector (Renderer::BeginFrame feeds it from the VFX
// PendingTracerSpawns requests; the editor Debug menu spawns too). BeginFrame copies the queue
// into this frame's upload buffer; TracerSimPass runs the emit and update compute shaders and
// TracerRenderPass draws the whole pool as instanced quads.
//
// Pool: one StructuredBuffer<Tracer> on the DEFAULT heap (SRV + UAV), written as a ring, so a
// new spawn overwrites the oldest slot once the cursor wraps (see kMaxTracers).

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <vector>
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;

// GPU-visible tracer record, 64 bytes; mirrors Tracer in shaders/Tracer.hlsli.
struct alignas(16) TracerGPU
{
    DirectX::XMFLOAT3 startPos;            // 12
    float             lifetime;            // 4   - seconds remaining; <= 0 = dead
    DirectX::XMFLOAT3 endPos;              // 12
    float             maxLifetime;         // 4
    DirectX::XMFLOAT4 color;               // 16  - linear HDR; .a = intensity
    float             width;               // 4   - beam radius in metres
    float             spawnTime;           // 4   - global time when spawned
    uint32_t          noiseTexBindlessIdx; // 4   - 0xFFFFFFFFu = procedural
    uint32_t          _pad0;               // 4
};
static_assert(sizeof(TracerGPU) == 64, "TracerGPU layout drift");

// CPU pushes one of these per spawn. Same shape as TracerGPU so the emit CS
// is a straight memcpy with `dst = (writeCursor + threadId) % capacity`.
using TracerSpawnGPU = TracerGPU;

// 32 bytes. Mirrors shaders/Tracer.hlsli TracerSystemParams.
struct alignas(16) TracerSystemParams
{
    float    deltaTime;     // 4
    uint32_t poolCapacity;  // 4
    uint32_t spawnCount;    // 4
    float    time;          // 4 - global seconds (for noise scroll)
    uint32_t writeCursor;   // 4 - cursor at start of this frame
    uint32_t _pad0;         // 4
    uint32_t _pad1;         // 4
    uint32_t _pad2;         // 4
};
static_assert(sizeof(TracerSystemParams) == 32, "TracerSystemParams layout drift");

class TracerSystem
{
public:
    // 4096 tracers x 64 B = 256 KB. A slot is overwritten 4096 spawns later, so spawn rate x
    // lifetime must stay under 4096 (2000 spawns/s at 2 s, 8000/s at 0.5 s) or old tracers pop.
    static constexpr uint32_t kMaxTracers          = 4096;
    static constexpr uint32_t kMaxSpawnsPerFrame   = 1024;

    void Init(IGraphicsDevice& gfx);
    void Shutdown(IGraphicsDevice& gfx);

    // Queues one tracer for the next BeginFrame (run from Renderer::BeginFrame). Not
    // thread-safe; requests beyond kMaxSpawnsPerFrame in one frame are dropped.
    //   @p start / @p end : world-space endpoints (typically muzzle -> impact)
    //   @p color          : linear HDR; .a is an intensity multiplier (> 1 ok)
    //   @p width          : world-space radius in metres (0.02 - 0.2 typical)
    //   @p lifetime       : seconds
    //   @p noiseTexBindless : optional bindless texture index for the noise scroll;
    //                         0xFFFFFFFFu = cheap procedural noise
    void Spawn(const DirectX::XMFLOAT3& start,
               const DirectX::XMFLOAT3& end,
               const DirectX::XMFLOAT4& color,
               float                    width,
               float                    lifetime,
               uint32_t                 noiseTexBindless = 0xFFFFFFFFu);

    // Drain the pending CPU queue into the GPU upload buffer; advance the
    // ring cursor; refresh the system CB. Call from Renderer::BeginFrame.
    void BeginFrame(float dt, uint32_t frameIndex, float globalTimeSeconds);

    // Number of spawns emitted this frame (after BeginFrame). Used by
    // TracerSimPass to size the emit dispatch.
    uint32_t GetSpawnCount() const { return m_spawnCountThisFrame; }

    // Conservative CPU-side liveness estimate. Lifetimes tick down on the GPU
    // (TracerUpdate.cs.hlsl), so the CPU cannot know exactly when the pool empties;
    // m_liveTimer mirrors that countdown with the same dt, seeded by the longest-lived spawn,
    // so it can only over-report. False means every slot is provably dead and
    // TracerRenderPass can skip the frame.
    bool HasLiveTracers() const { return m_liveTimer > 0.0f || GetSpawnCount() > 0; }

    // Descriptor accessors for passes.
    const RHI::GPUBuffer& GetPool()        const { return m_pool; }
    uint64_t              GetPoolSRV()     const { return m_poolSrv; }
    uint64_t              GetPoolUAV()     const { return m_poolUav; }
    uint64_t              GetSpawnSRV    (IGraphicsDevice& gfx) const;
    const RHI::GPUBuffer& GetSystemCB    (IGraphicsDevice& gfx) const { return m_systemCB.CurrentBuffer(gfx); }

    static constexpr uint32_t GetPoolCapacity() { return kMaxTracers; }

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    IGraphicsDevice* m_gfx = nullptr;

    // Main pool (DEFAULT heap, SRV + UAV).
    RHI::GPUBuffer m_pool;
    uint64_t       m_poolSrv = 0;
    uint64_t       m_poolUav = 0;

    // Per-frame spawn upload buffers (UPLOAD heap, SRV), one per frame in flight.
    RHI::GPUBuffer m_spawnBuffer[kFrameCount];
    uint64_t       m_spawnSrv[kFrameCount]    = {};
    void*          m_spawnMapped[kFrameCount] = {};

    // Per-frame system CB (UPLOAD heap, root CBV).
    FrameCB<TracerSystemParams> m_systemCB;

    // CPU spawn queue, flushed at BeginFrame.
    std::vector<TracerSpawnGPU> m_pendingSpawns;

    // GPU pool ring cursor; advances by the spawn count each frame.
    uint32_t m_writeCursor = 0;

    // Spawn count actually written this frame (after clamping).
    uint32_t m_spawnCountThisFrame = 0;

    // Global time from the last BeginFrame; Spawn stamps it on new records and BeginFrame
    // re-stamps them with the current time before the upload.
    float m_lastGlobalTime = 0.0f;

    // Longest tracer lifetime seen at spawn, decayed by BeginFrame with the same dt the update
    // CS uses. Conservative: never under-reports.
    float m_liveTimer = 0.0f;
};
