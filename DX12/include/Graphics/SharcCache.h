#pragma once
// SharcCache - the SHaRC radiance cache's GPU resources, configuration and device gate, shared
// by its consumers: the RTXDI PT core (PTRtxdiCore) and the ReSTIR GI shell (RTGIPass). Render
// paths are exclusive, so at most one consumer holds a cache at a time; the GI compare mode's
// shadow pass borrows the main pass's.
// The kernels are NOT here: each consumer compiles its own clear / update / resolve entries in
// its own TU, reading the buffer indices from its own constants and depositing what it caches
// (PT: camera-rooted paths through the RTXDI bridge; GI: one segment from each sparse G-buffer
// receiver). shaders/SharcEngine.hlsli holds what they share: SHaRC's configuration (one cache
// semantics, never forked) and the clear / resolve bodies.
// No RTXDI dependency: a build without the RTXDI SDK still has the GI cache.
#include "Graphics/GraphicsStruct.h"
#include <cstdint>

class IGraphicsDevice;

class SharcCache
{
public:
    struct Config
    {
        uint32_t capacityLog2 = 22;       // 2^capacityLog2 entries of 40 B
        float sceneScale = 50.f;          // voxel = 2^level / sceneScale
        float radianceScale = 1e4f;       // fixed-point deposits: uint(radiance x scale)
        uint32_t updateTile = 5;          // one update path per tile x tile pixels
        uint32_t updateBounces = 3;
        uint32_t accumulationFrames = 20;
        uint32_t staleFrames = 60;
    };

    struct Indices { uint32_t hashUav = 0, accumulationUav = 0, resolvedUav = 0, capacity = 0; };

    // What a reader that does not own the cache needs to query it (the froxel fog's indirect rays):
    // the buffers and the configuration its keys and fixed-point radiance depend on.
    // indices.capacity 0: no cache.
    struct Lookup
    {
        Indices indices;
        float sceneScale = 0.f, radianceScale = 0.f;
    };

    // The shared -sharc_* flags (-sharc_capacity_log2, -sharc_scene_scale, -sharc_radiance_scale,
    // -sharc_update_tile, -sharc_update_bounces, -sharc_accumulation_frames, -sharc_stale_frames)
    // over the consumer's @p defaults, each clamped to its valid range.
    static Config FromCommandLine(const Config& defaults);
    // Why this device / tree cannot run the cache, or nullptr: the update's
    // hash insert needs 64-bit atomics on descriptor-heap resources; a source
    // tree needs SHaRC's unpacked headers (a package carries the compiled
    // kernels instead); @p needNative16: the consumer's kernels are not 16-bit
    // already (SHaRC stores fp16 radiance, SharcTypes.h).
    static const char* InertReason(const IGraphicsDevice& gfx, bool needNative16);
    static uint64_t BytesFor(uint32_t capacityLog2) { return 40ull << capacityLog2; }

    // Committed buffers, never aliased (the cache IS cross-frame data). A new
    // cache starts with a clear pending ("allocate").
    bool Allocate(IGraphicsDevice& gfx, uint32_t capacityLog2);
    void Release(IGraphicsDevice& gfx);
    bool IsAllocated() const { return m_hash.IsValid(); }
    uint32_t CapacityLog2() const { return m_capacityLog2; }
    uint32_t Capacity() const { return IsAllocated() ? 1u << m_capacityLog2 : 0u; }
    uint64_t ResidentBytes() const { return IsAllocated() ? BytesFor(m_capacityLog2) : 0ull; }

    // Forget the cache at the consumer's next record (a new world, a new
    // capacity, a new content mode). Never on a resize, a camera cut or a
    // history reset: the cache is world-space.
    void RequestClear(const char* reason) { m_clearReason = reason ? reason : "request"; }
    bool ClearPending() const { return m_clearReason != nullptr; }
    // The pending clear's reason, consumed (and counted), or nullptr.
    const char* TakeClear();
    uint32_t Clears() const { return m_clears; }

    // The three buffers into UNORDERED_ACCESS for this command list.
    void Transition(IGraphicsDevice& gfx, RHI::CommandList cl);

    // Bindless UAV indices (zeros without a cache).
    Indices GetIndices(const IGraphicsDevice& gfx) const;

private:
    RHI::GPUBuffer m_hash, m_accumulation, m_resolved;
    RHI::ResourceState m_hashState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_accumulationState = RHI::ResourceState::UNDEFINED;
    RHI::ResourceState m_resolvedState = RHI::ResourceState::UNDEFINED;
    uint32_t m_capacityLog2 = 0, m_clears = 0;
    const char* m_clearReason = "allocate";
};
