#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/ThreadContext.h"
#include <cstring>

// The CPU side of this class (m_master, m_slotDirty[], the free lists, m_meshCount) is game-half
// state; the render half only touches the GPU buffers and the stable m_bufferRange. Under
// -animoverlap render work can be in flight while these run, so each game-half entry point checks.
// A macro, not a helper: ENGINE_CHECK_ONCE's latch belongs to the function that expands it, so a
// shared helper would report only whichever entry point fired first.
#define MESHHEAP_GAME_HALF_ONLY(what)                                          \
    ENGINE_CHECK_ONCE(!Sys::ThreadCtx::PoolReadForbidden(),                    \
        "[SPLITCHECK][MESHHEAP] " what " ran on a render-context thread. The "  \
        "mesh descriptor heap's CPU side (master table, per-slot dirty flags, " \
        "free lists) belongs to the game half; the render half may only read "  \
        "the GPU buffers through the render slot.")

MeshDescriptorHeap::~MeshDescriptorHeap()
{
    if (!m_gfx) return;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_meshDescMapped[i])
            m_gfx->UnmapBuffer(m_meshDescBuffers[i]);
        if (m_meshAABBMapped[i])
            m_gfx->UnmapBuffer(m_meshAABBBuffers[i]);
        if (m_meshletInfoMapped[i])
            m_gfx->UnmapBuffer(m_meshletInfoBuffers[i]);
    }
}

uint32_t MeshDescriptorHeap::CurrentMeshletInfoBindless() const
{
    const uint32_t slot = m_gfx ? m_gfx->GetFrameIndex() % kFrameCount : m_currentFrameSlot;
    return m_meshletInfoBindless[slot];
}

const RHI::GPUBuffer& MeshDescriptorHeap::CurrentMeshAABBBuffer() const
{
    const uint32_t slot = m_gfx ? m_gfx->GetFrameIndex() % kFrameCount : m_currentFrameSlot;
    return m_meshAABBBuffers[slot];
}

const RHI::GPUBuffer& MeshDescriptorHeap::CurrentMeshDescBuffer() const
{
    // The RENDER slot, deliberately not m_currentFrameSlot (see the header). Without a device
    // (pre-Init) the two cannot differ.
    const uint32_t slot = m_gfx ? (m_gfx->GetFrameIndex() % kFrameCount)
                                : m_currentFrameSlot;
    return m_meshDescBuffers[slot];
}

void MeshDescriptorHeap::Init(IGraphicsDevice& gfx, uint32_t maxMeshes)
{
    // The slot cap is per instance. Clamp it: m_meshCount starts at kPermanentMeshSlots, so a cap
    // below the persistent reservation would start the world allocator past its own end and fail
    // every RegisterMesh.
    if (!ENGINE_CHECK(maxMeshes >= kPermanentMeshSlots,
                      "MeshDescriptorHeap::Init: maxMeshes %u is below the "
                      "persistent reservation %u; clamping",
                      maxMeshes, kPermanentMeshSlots))
        maxMeshes = kPermanentMeshSlots;
    m_maxMeshes = maxMeshes;

    auto& dx12  = static_cast<GraphicsDX12&>(gfx);
    m_gfx       = &dx12;
    m_device    = dx12.GetDevice();
    m_allocator = &dx12.GetCbvSrvUavAllocator();
    m_descSize  = m_allocator->GetDescriptorSize();

    // Allocate kMaxBuffers contiguous SRV slots for the bindless g_Buffers[] table.
    m_bufferRange = m_allocator->Allocate(kMaxBuffers);

    // On failure registration hands out no slots (it checks the range / the mapped buffers), as
    // after the failures below.
    if (!ENGINE_CHECK(m_bufferRange.IsValid(),
            "MeshDescriptorHeap: failed to allocate the bindless SRV range (%u descriptors) - "
            "descriptor heap too small", kMaxBuffers))
        return;

    // One UPLOAD-heap StructuredBuffer<MeshDescriptor> per frame in flight, so the per-frame
    // UpdateMeshThisFrame patches never clobber a slot the GPU still reads for an earlier frame.
    RHI::GPUBufferDesc desc;
    desc.size       = static_cast<uint64_t>(m_maxMeshes) * sizeof(RHI::MeshDescriptor);
    desc.stride     = sizeof(RHI::MeshDescriptor);
    desc.usage      = RHI::Usage::UPLOAD;
    desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    desc.debugName = "MeshDescriptorHeap.Descriptors";
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (!dx12.CreateBuffer(desc, m_meshDescBuffers[i]))
        {
            LOG_ERROR("MeshDescriptorHeap: failed to create MeshDescriptor buffer[%u]", i);
            return;
        }
        m_meshDescMapped[i] = dx12.MapBuffer(m_meshDescBuffers[i]);
        if (!m_meshDescMapped[i])
        {
            LOG_ERROR("MeshDescriptorHeap: failed to map MeshDescriptor buffer[%u]", i);
            return;
        }
    }

    // CPU-side master mirror, sized up front so RegisterMesh writes stay O(1).
    m_master.resize(m_maxMeshes);
    m_dedupMeshDirty = Sys::CommandLine::Get().Has("-meshdirtydedup");
    if (m_dedupMeshDirty) m_slotDirtyMask.assign(m_maxMeshes, 0);
    m_currentFrameSlot = 0;

    // Every ring slot starts unsynced: its first BeginFrame does a full master->slot copy (covering
    // at least the persistent prefix), then the incremental dirty-list path takes over.
    for (uint32_t i = 0; i < kFrameCount; ++i)
        m_slotNeedsFullResync[i] = true;

    // Parallel AABB buffer (same slot count as MeshDescriptor).
    RHI::GPUBufferDesc aabbDesc;
    aabbDesc.size       = static_cast<uint64_t>(m_maxMeshes) * sizeof(MeshAABB);
    aabbDesc.stride     = sizeof(MeshAABB);
    aabbDesc.usage      = RHI::Usage::UPLOAD;
    aabbDesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    aabbDesc.debugName = "MeshDescriptorHeap.AABB";
    m_aabbMaster.assign(m_maxMeshes, kNeverCulledAABB);
    m_aabbDirtyMask.assign(m_maxMeshes, 0);
#if defined(WITH_HARNESS) && WITH_HARNESS
    m_verifyAABBFrames = Sys::CommandLine::Get().Has("-meshaabbverify");
    m_verifyMeshDirty = Sys::CommandLine::Get().Has("-meshdirtyverify");
#endif
    for (uint32_t frame = 0; frame < kFrameCount; ++frame)
    {
        if (!dx12.CreateBuffer(aabbDesc, m_meshAABBBuffers[frame]))
        {
            LOG_ERROR("MeshDescriptorHeap: failed to create MeshAABB buffer[%u]", frame);
            return;
        }
        m_meshAABBMapped[frame] = dx12.MapBuffer(m_meshAABBBuffers[frame]);
        if (!m_meshAABBMapped[frame])
        {
            LOG_ERROR("MeshDescriptorHeap: failed to map MeshAABB buffer[%u]", frame);
            return;
        }
        // Every slot starts with a huge AABB, so a mesh whose registration never provides real bounds is
        // never culled: the GPU command cull reads this buffer for every draw. RegisterMesh re-applies it
        // per slot, which keeps the guarantee once slots are recycled.
        std::memcpy(m_meshAABBMapped[frame], m_aabbMaster.data(), aabbDesc.size);
        m_aabbFullResync[frame] = true;
    }

    // Meshlet sidecar: the AABB ring's twin, every row "no meshlets" until RegisterMeshLibMesh says
    // otherwise. Each frame buffer is a PERSISTENT g_Buffers[] slot the amplification shader reaches
    // by index (MeshletCullCB.sidecarIdx); it lives as long as this heap, through every world clear.
    {
        RHI::GPUBufferDesc infoDesc;
        infoDesc.size       = static_cast<uint64_t>(m_maxMeshes) * sizeof(MeshletInfo);
        infoDesc.stride     = 0;
        infoDesc.usage      = RHI::Usage::UPLOAD;
        infoDesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        // RAW: the device creates the buffer's own SRV from these flags, and a
        // stride-0 buffer without it asks for an UNKNOWN-format view - an
        // invalid call that removes the device.
        infoDesc.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        infoDesc.debugName = "MeshDescriptorHeap.MeshletInfo";
        m_meshletInfoMaster.assign(m_maxMeshes, kNoMeshlets);
        m_meshletInfoDirtyMask.assign(m_maxMeshes, 0);
        for (uint32_t frame = 0; frame < kFrameCount; ++frame)
        {
            m_meshletInfoBindless[frame] = RHI::kInvalidBufferIndex;
            if (!dx12.CreateBuffer(infoDesc, m_meshletInfoBuffers[frame]))
            {
                LOG_ERROR("MeshDescriptorHeap: failed to create MeshletInfo buffer[%u]", frame);
                continue;
            }
            m_meshletInfoMapped[frame] = dx12.MapBuffer(m_meshletInfoBuffers[frame]);
            if (!m_meshletInfoMapped[frame])
            {
                LOG_ERROR("MeshDescriptorHeap: failed to map MeshletInfo buffer[%u]", frame);
                continue;
            }
            std::memcpy(m_meshletInfoMapped[frame], m_meshletInfoMaster.data(), infoDesc.size);
            m_meshletInfoFullResync[frame] = true;
            m_meshletInfoBindless[frame] = RegisterPersistentBuffer(m_meshletInfoBuffers[frame]);
        }
    }

    LOG_SUCCESS("MeshDescriptorHeap: initialised (%u buffer slots, %u mesh slots × %u frames)",
                kMaxBuffers, m_maxMeshes, kFrameCount);
}

void MeshDescriptorHeap::BeginFrame(uint32_t frameIndex)
{
    m_currentFrameSlot = frameIndex % kFrameCount;
    const uint32_t slot = m_currentFrameSlot;

    // Reclaim slots freed kFrameCount frames ago: this frame slot was last
    // active kFrameCount frames back, so every command list that referenced
    // them has retired. Drain before any Register* this frame so the recycled
    // slots are immediately reusable.
    for (uint32_t s : m_meshFreePending[slot])   m_meshFreeList.push_back(s);
    m_meshFreePending[slot].clear();
    for (uint32_t s : m_bufferFreePending[slot]) m_bufferFreeList.push_back(s);
    m_bufferFreePending[slot].clear();

    // Meshlet sidecar before the early return below (and independent of the AABB block), or a frame
    // without a mapped descriptor ring would leave this slot's sidecar stale.
    PublishMeshletInfo(slot);

    if (!m_meshDescMapped[slot]) return;

    {
        ZoneScopedN("MeshDescriptor.SnapshotRefresh");
        auto* dst = static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[slot]);

        // Refresh the new frame's slot from the CPU master so stable descriptors (RegisterMesh +
        // UpdateMesh) propagate and TRANSIENT patches written to this slot kFrameCount frames ago are
        // wiped (callers must re-issue).
        // Full copy on the slot's first visit after Init/OnWorldClear, and when the dirty list is as large
        // as a full copy. m_meshCount starts at kPermanentMeshSlots, so the copy always covers the
        // persistent prefix (how RegisterPersistentMesh indices stay valid across reloads).
        if (m_slotNeedsFullResync[slot] || m_slotDirty[slot].size() >= m_meshCount)
        {
            std::memcpy(dst, m_master.data(),
                        static_cast<size_t>(m_meshCount) * sizeof(RHI::MeshDescriptor));
            m_slotNeedsFullResync[slot] = false;
        }
        else
        {
            // Incremental: recopy only the diverged indices (stable writes this
            // slot missed while inactive + transient patches it took last cycle).
            for (uint32_t idx : m_slotDirty[slot])
                if (idx < m_meshCount)          // guard: OnWorldClear may shrink count
                    dst[idx] = m_master[idx];
        }
        if (m_dedupMeshDirty)
            for (uint32_t idx : m_slotDirty[slot])
                m_slotDirtyMask[idx] &= uint8_t(~(1u << slot));
        m_slotDirty[slot].clear();
        if (m_verifyMeshDirty)
        {
            // Compare every live row, independently of the dirty list.
            const bool match = std::memcmp(dst, m_master.data(),
                size_t(m_meshCount) * sizeof(RHI::MeshDescriptor)) == 0;
            ENGINE_CHECK(match, "MeshDescriptor dirty refresh differs from full master copy");
            // Mark one unchanged persistent row twice; GPU/master stay untouched.
            if (m_dedupMeshDirty)
            {
                const auto count = m_slotDirty[slot].size();
                MarkSlotDirty(slot, 0);
                MarkSlotDirty(slot, 0);
                ENGINE_CHECK(m_slotDirty[slot].size() == count + 1,
                    "MeshDescriptor duplicate dirty row was enqueued twice");
            }
            LOG_INFO("[MESH_DIRTY_ORACLE] match=%u duplicates=%llu",
                match ? 1u : 0u, static_cast<unsigned long long>(m_meshDirtyDuplicates));
        }
    }


    // This slot's previous GPU use has retired under the same lease that
    // protects mesh descriptors. Only copy changed rows, once per slot.
    {
        ZoneScopedN("MeshAABB.SnapshotRefresh");
        auto* bounds = static_cast<MeshAABB*>(m_meshAABBMapped[slot]);
        if (!bounds) return;
        if (m_aabbFullResync[slot])
            std::memcpy(bounds, m_aabbMaster.data(), size_t(m_meshCount) * sizeof(MeshAABB));
        else
            for (uint32_t idx : m_aabbDirty[slot])
                if (idx < m_meshCount) bounds[idx] = m_aabbMaster[idx];
        for (uint32_t idx : m_aabbDirty[slot])
            m_aabbDirtyMask[idx] &= uint8_t(~(1u << slot));
        m_aabbDirty[slot].clear();
        m_aabbFullResync[slot] = false;
    }
}

// ==== Pool pressure telemetry ====
// The exhaustion reports below only speak once a pool is already dry. These at most two lines per
// pool land while there is still headroom; the thresholds are percentages of the cap, so they keep
// meaning the same after a cap is raised.
void MeshDescriptorHeap::NotePoolPressure(const char* poolName,
                                          const char* capConstant,
                                          uint32_t used, uint32_t cap,
                                          uint8_t& warnedFlags)
{
    if (cap == 0) return;
    const uint32_t pct =
        static_cast<uint32_t>((static_cast<uint64_t>(used) * 100ull) / cap);

    // 90% sets BOTH bits: a pool that jumps straight past 90 (bulk world load)
    // must not emit the 75% line afterwards on its way back up.
    if (pct >= 90 && !(warnedFlags & 0x2))
        warnedFlags |= 0x3;
    else if (pct >= 75 && !(warnedFlags & 0x1))
        warnedFlags |= 0x1;
    else
        return;

    LOG_WARNING("MeshDescriptorHeap: %s at %u%% of capacity (%u/%u). "
                "Exhaustion makes meshes silently stop rendering — "
                "raise MeshDescriptorHeap::%s.",
                poolName, pct, used, cap, capConstant);
}

MeshDescriptorHeap::PoolStats MeshDescriptorHeap::GetPoolStats() const
{
    PoolStats s{};
    s.bufferCurrent   = m_bufferCount;
    s.bufferHighWater = m_bufferHighWater;
    s.bufferCapacity  = kMaxBuffers;
    s.bufferReserved  = kPermanentBufferSlots;
    s.meshCurrent     = m_meshCount;
    s.meshHighWater   = m_meshHighWater;
    s.meshCapacity    = m_maxMeshes;
    s.meshReserved    = kPermanentMeshSlots;

    s.persistentBufferCurrent  = m_persistentBufferCount;
    s.persistentBufferCapacity = kPermanentBufferSlots;
    s.persistentMeshCurrent    = m_persistentMeshCount;
    s.persistentMeshCapacity   = kPermanentMeshSlots;

    s.bufferFreeList = static_cast<uint32_t>(m_bufferFreeList.size());
    s.meshFreeList   = static_cast<uint32_t>(m_meshFreeList.size());

    s.bufferOverflows           = m_bufferOverflowCount;
    s.meshOverflows             = m_meshOverflowCount;
    s.persistentBufferOverflows = m_persistentBufferOverflowCount;
    s.persistentMeshOverflows   = m_persistentMeshOverflowCount;
    return s;
}

void MeshDescriptorHeap::ResetPoolWatermarks()
{
    m_bufferHighWater = m_bufferCount;
    m_meshHighWater   = m_meshCount;

    m_bufferOverflowCount           = 0;
    m_meshOverflowCount             = 0;
    m_persistentBufferOverflowCount = 0;
    m_persistentMeshOverflowCount   = 0;

    // Re-arm the latches too: a reset taken while a pool sits above 75% would otherwise never warn
    // again.
    m_bufferPressureWarned           = 0;
    m_meshPressureWarned             = 0;
    m_persistentBufferPressureWarned = 0;
    m_persistentMeshPressureWarned   = 0;
}

void MeshDescriptorHeap::MarkSlotDirty(uint32_t frameSlot, uint32_t idx)
{
    if (!m_dedupMeshDirty)
    {
        m_slotDirty[frameSlot].push_back(idx);
        return;
    }
    if (m_slotNeedsFullResync[frameSlot]) return;
    const uint8_t bit = uint8_t(1u << frameSlot);
    if (m_slotDirtyMask[idx] & bit)
    {
        if (m_verifyMeshDirty) ++m_meshDirtyDuplicates;
        return;
    }
    m_slotDirty[frameSlot].push_back(idx);
    m_slotDirtyMask[idx] |= bit;
}

void MeshDescriptorHeap::MarkStableDirty(uint32_t idx)
{
    // A stable write patched m_master + the active slot directly, so only the OTHER ring slots are
    // stale at idx and must recopy it next BeginFrame. Slots pending a full resync are skipped: their
    // full copy picks idx up anyway, and this keeps dirty lists small during a world load.
    for (uint32_t s = 0; s < kFrameCount; ++s)
        if (s != m_currentFrameSlot && !m_slotNeedsFullResync[s])
            MarkSlotDirty(s, idx);
}

void MeshDescriptorHeap::WriteRawBufferSrv(uint32_t idx, ID3D12Resource* resource, UINT64 sizeBytes)
{
    // Compute per-slot handles by offsetting from the allocation base.
    D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = m_bufferRange.GetCpuHandle();
    cpuSlot.ptr += static_cast<SIZE_T>(idx) * m_descSize;

    D3D12_CPU_DESCRIPTOR_HANDLE gpuCpuSlot = m_bufferRange.GetGpuCpuHandle();
    gpuCpuSlot.ptr += static_cast<SIZE_T>(idx) * m_descSize;

    // Create ByteAddressBuffer SRV at the CPU staging slot.
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = DXGI_FORMAT_R32_TYPELESS;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Buffer.NumElements      = static_cast<UINT>(sizeBytes / 4);
    srvd.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    m_allocator->WriteSrv(resource, &srvd, cpuSlot);

    // Copy to the shader-visible slot through the allocator, which records what the slot holds.
    m_allocator->CopyDescriptors(1, gpuCpuSlot, cpuSlot);
}

uint32_t MeshDescriptorHeap::RegisterRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterRawBuffer");
    if (!resource || !m_device || !m_bufferRange.IsValid())
        return RHI::kInvalidBufferIndex;

    // Dedup: meshes sharing one VB/IB (e.g. a merged per-scene .imsh whose submeshes all reference one
    // buffer pair) must resolve to the SAME bindless slot, or the kMaxBuffers pool runs dry after a
    // few thousand RegisterSceneMesh calls and later meshes silently fail to render. GetPoolStats(),
    // the OnWorldClear peak line and NotePoolPressure report how close the pool is.
    auto it = m_resourceToSlot.find(resource);
    if (it != m_resourceToSlot.end())
        return it->second;

    uint32_t idx;
    if (!m_bufferFreeList.empty())
    {
        idx = m_bufferFreeList.back();   // recycled world slot (an entity was destroyed)
        m_bufferFreeList.pop_back();
    }
    else
    {
        if (m_bufferCount >= kMaxBuffers)
        {
            ++m_bufferOverflowCount;
            // The free list is empty here, so report what is still DEFERRED: slots FreeBuffer reclaimed that
            // BeginFrame has not recycled yet. A large value means the caller registers faster than the
            // kFrameCount-deep recycle hands slots back, not that the table is really full.
            uint32_t pendingFree = 0;
            for (uint32_t f = 0; f < kFrameCount; ++f)
                pendingFree += static_cast<uint32_t>(m_bufferFreePending[f].size());

            // Latched: once the table is full every later RegisterBuffer lands here, and an unlatched line
            // would bury the first (only interesting) one. The magnitude lives in m_bufferOverflowCount /
            // GetPoolStats().
            ENGINE_CHECK_ONCE(false,
                "MeshDescriptorHeap: bindless buffer table full (%u/%u slots; "
                "%u freed slots still deferred). This buffer and every later "
                "one gets kInvalidBufferIndex and its mesh will NOT render. "
                "Raise MeshDescriptorHeap::kMaxBuffers (and RHI::Slots::"
                "MaxBindlessBuffers, which the static_assert pins to it).",
                m_bufferCount, kMaxBuffers, pendingFree);
            return RHI::kInvalidBufferIndex;
        }
        idx = m_bufferCount++;
        if (m_bufferCount > m_bufferHighWater) m_bufferHighWater = m_bufferCount;
        NotePoolPressure("bindless buffer table", "kMaxBuffers",
                         m_bufferCount, kMaxBuffers, m_bufferPressureWarned);
    }
    m_resourceToSlot.emplace(resource, idx);
    m_bufferSlotResource[idx] = resource;   // reverse map for FreeBuffer's dedup erase
    WriteRawBufferSrv(idx, resource, sizeBytes);
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterPersistentRawBuffer");
    if (!resource || !m_device || !m_bufferRange.IsValid())
        return RHI::kInvalidBufferIndex;

    if (m_persistentBufferCount >= kPermanentBufferSlots)
    {
        // Cold path (Init-time callers only): one LOG_ERROR per failure is the right volume, so this is
        // not latched.
        ++m_persistentBufferOverflowCount;
        LOG_ERROR("MeshDescriptorHeap: persistent bindless buffer range exhausted "
                  "(%u/%u). Bump kPermanentBufferSlots.",
                  m_persistentBufferCount, kPermanentBufferSlots);
        return RHI::kInvalidBufferIndex;
    }

    const uint32_t idx = m_persistentBufferCount++;
    NotePoolPressure("persistent bindless buffer range", "kPermanentBufferSlots",
                     m_persistentBufferCount, kPermanentBufferSlots,
                     m_persistentBufferPressureWarned);
    // Not inserted into m_resourceToSlot: persistent slots bypass world-scoped dedup so OnWorldClear
    // can wipe that map without losing them. Persistent callers are Init code and register each
    // resource exactly once.
    WriteRawBufferSrv(idx, resource, sizeBytes);
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentBuffer(const RHI::GPUBuffer& buf)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterPersistentBuffer");
    if (!buf.IsValid()) return RHI::kInvalidBufferIndex;
    auto* dx12 = static_cast<GraphicsDX12*>(m_gfx);
    if (!dx12) return RHI::kInvalidBufferIndex;
    ID3D12Resource* resource = dx12->GetBufferResource(buf);
    if (!resource) return RHI::kInvalidBufferIndex;
    return RegisterPersistentRawBuffer(resource, buf.desc.size);
}

uint32_t MeshDescriptorHeap::RegisterMesh(const RHI::MeshDescriptor& desc)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterMesh");
    if (!m_meshDescMapped[m_currentFrameSlot])
        return RHI::kInvalidBufferIndex;

    uint32_t idx;
    if (!m_meshFreeList.empty())
    {
        idx = m_meshFreeList.back();   // recycled world slot (an entity was destroyed)
        m_meshFreeList.pop_back();
    }
    else
    {
        if (m_meshCount >= m_maxMeshes)
        {
            ++m_meshOverflowCount;
            uint32_t pendingFree = 0;
            for (uint32_t f = 0; f < kFrameCount; ++f)
                pendingFree += static_cast<uint32_t>(m_meshFreePending[f].size());

            // Latched for the same reason as the buffer table: a full pool
            // rejects every remaining mesh of the load, one line each.
            ENGINE_CHECK_ONCE(false,
                "MeshDescriptorHeap: mesh descriptor buffer full (%u/%u slots; "
                "%u freed slots still deferred). This mesh and every later one "
                "gets kInvalidBufferIndex and will NOT render. Raise "
                "MeshDescriptorHeap::kDefaultMaxMeshes (or the cap this heap "
                "was Init'd with).",
                m_meshCount, m_maxMeshes, pendingFree);
            return RHI::kInvalidBufferIndex;
        }
        idx = m_meshCount++;
        if (m_meshCount > m_meshHighWater) m_meshHighWater = m_meshCount;
        NotePoolPressure("mesh descriptor buffer", "kDefaultMaxMeshes",
                         m_meshCount, m_maxMeshes, m_meshPressureWarned);
    }
    // Mesh slots are recycled (m_meshFreeList on entity destroy, OnWorldClear rewinding m_meshCount)
    // and neither path resets the AABB buffer. A mesh that never publishes bounds (skinned) relies on
    // the "never culled" default; without this reset it inherits the previous occupant's box and the
    // GPU command cull drops it while in view. Callers with real bounds overwrite it right after.
    ResetMeshAABB(idx);
    // Same for the meshlet sidecar: a recycled slot must not name the previous mesh's clusters.
    ResetMeshletInfo(idx);

    // Master gets the canonical value. The active GPU slot is patched in place only while the game
    // side owns it (as in FreeMesh): this slot may be recycled, and an unguarded write would make an
    // in-flight frame render the NEW mesh in the old one's place. Deferring costs nothing: a
    // registration during frame F's gameplay is replayed into F's slot by the Animation phase's
    // rotate, before F renders.
    m_master[idx] = desc;
    if (m_gfx && m_gfx->IsUploadSlotWritable()
              && m_currentFrameSlot == m_gfx->GetUploadSlot())
    {
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[idx] = desc;
        MarkStableDirty(idx);
    }
    else
    {
        static bool s_reportedReg = false;
        if (!s_reportedReg)
        {
            s_reportedReg = true;
            LOG_INFO("[SPLITAUDIT][HEAPOWNER] RegisterMesh outside the game-owned "
                     "window (ring slot %u, game slot %u) - deferred",
                     m_currentFrameSlot, m_gfx ? m_gfx->GetUploadSlot() : 0u);
        }
        // Current slot included - MarkStableDirty skips it by design.
        MarkStableDirty(idx);
        if (!m_slotNeedsFullResync[m_currentFrameSlot])
            MarkSlotDirty(m_currentFrameSlot, idx);
    }
    return idx;
}

uint32_t MeshDescriptorHeap::RegisterPersistentMesh(const RHI::MeshDescriptor& desc)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterPersistentMesh");
    if (m_persistentMeshCount >= kPermanentMeshSlots)
    {
        // Cold path (Init-time callers only): not latched, as in RegisterPersistentRawBuffer.
        ++m_persistentMeshOverflowCount;
        LOG_ERROR("MeshDescriptorHeap: persistent mesh slot range exhausted "
                  "(%u/%u). Bump kPermanentMeshSlots.",
                  m_persistentMeshCount, kPermanentMeshSlots);
        return RHI::kInvalidBufferIndex;
    }
    if (!m_meshDescMapped[m_currentFrameSlot])
        return RHI::kInvalidBufferIndex;

    const uint32_t idx = m_persistentMeshCount++;
    NotePoolPressure("persistent mesh slot range", "kPermanentMeshSlots",
                     m_persistentMeshCount, kPermanentMeshSlots,
                     m_persistentMeshPressureWarned);
    // Persistent slot lives in the low reserved range; the world allocator
    // starts at kPermanentMeshSlots so there's no overlap. Write to master
    // (so all future BeginFrame() refreshes propagate it) plus the active GPU
    // slot (so the current frame can already render it).
    m_master[idx] = desc;
    static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[idx] = desc;
    MarkStableDirty(idx);
    ResetMeshletInfo(idx);   // persistent meshes (primitives, billboard) have no meshlets
    return idx;
}

void MeshDescriptorHeap::UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc)
{
    if (slot >= m_meshCount) return;
    m_master[slot] = desc;
    if (m_meshDescMapped[m_currentFrameSlot])
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[slot] = desc;
    MarkStableDirty(slot);  // other ring slots must catch up to the new value
}

void MeshDescriptorHeap::UpdateMeshThisFrame(uint32_t slot, const RHI::MeshDescriptor& desc)
{
    if (slot >= m_meshCount) return;
    // Writes ONLY the active GPU slot, not m_master: the patch is wiped at the next BeginFrame, so
    // the caller must re-issue it every frame.
    if (m_meshDescMapped[m_currentFrameSlot])
    {
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[slot] = desc;
        // This slot now diverges from master at `slot`; its next BeginFrame
        // must recopy master[slot] to wipe this transient patch.
        MarkSlotDirty(m_currentFrameSlot, slot);
    }
}

uint32_t MeshDescriptorHeap::RegisterBuffer(const RHI::GPUBuffer& buf)
{
    MESHHEAP_GAME_HALF_ONLY("RegisterBuffer");
    if (!buf.IsValid()) return RHI::kInvalidBufferIndex;
    auto* dx12 = static_cast<GraphicsDX12*>(m_gfx);
    if (!dx12) return RHI::kInvalidBufferIndex;
    ID3D12Resource* resource = dx12->GetBufferResource(buf);
    if (!resource) return RHI::kInvalidBufferIndex;
    return RegisterRawBuffer(resource, buf.desc.size);
}

uint32_t MeshDescriptorHeap::FindRegisteredBuffer(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid() || !m_gfx) return RHI::kInvalidBufferIndex;
    auto* resource = static_cast<GraphicsDX12*>(m_gfx)->GetBufferResource(buf);
    if (!resource) return RHI::kInvalidBufferIndex;
    const auto found = m_resourceToSlot.find(resource);
    return found == m_resourceToSlot.end() ? RHI::kInvalidBufferIndex : found->second;
}

RHI::MeshDescriptor MeshDescriptorHeap::GetMesh(uint32_t slot) const
{
    if (slot >= m_meshCount) return {};
    // CPU master is the source of truth for "stable" reads.  Any TRANSIENT
    // patches from UpdateMeshThisFrame are intentionally NOT visible here
    // (they're only in the active GPU slot's mapped buffer).
    return m_master[slot];
}

void MeshDescriptorHeap::SetMeshAABB(uint32_t slot, const MeshAABB& aabb)
{
    WriteMeshAABB(slot, aabb, false);
}

void MeshDescriptorHeap::WriteMeshAABB(uint32_t slot, const MeshAABB& aabb, bool force)
{
    if (slot >= m_aabbMaster.size()) return;
    if (!force && std::memcmp(&m_aabbMaster[slot], &aabb, sizeof(aabb)) == 0) return;
    MeshAABB before[kFrameCount];
    if (m_verifyAABBFrames)
        for (uint32_t f = 0; f < kFrameCount; ++f)
            if (m_meshAABBMapped[f]) before[f] = static_cast<const MeshAABB*>(m_meshAABBMapped[f])[slot];
    m_aabbMaster[slot] = aabb;
    const bool writable = m_gfx && m_gfx->IsUploadSlotWritable() &&
        m_currentFrameSlot == m_gfx->GetUploadSlot() && m_meshAABBMapped[m_currentFrameSlot];
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        if (writable && f == m_currentFrameSlot)
            static_cast<MeshAABB*>(m_meshAABBMapped[f])[slot] = aabb;
        else if (!m_aabbFullResync[f] && !(m_aabbDirtyMask[slot] & (1u << f)))
        {
            m_aabbDirty[f].push_back(slot);
            m_aabbDirtyMask[slot] |= uint8_t(1u << f);
        }
        if (m_verifyAABBFrames && m_meshAABBMapped[f])
        {
            const auto& expected = writable && f == m_currentFrameSlot ? aabb : before[f];
            ENGINE_CHECK(std::memcmp(&static_cast<const MeshAABB*>(m_meshAABBMapped[f])[slot],
                &expected, sizeof(aabb)) == 0, "MeshAABB snapshot write escaped acquired slot");
        }
    }
    if (m_verifyAABBFrames) writable ? ++m_aabbDirectWrites : ++m_aabbDeferredWrites;
}

void MeshDescriptorHeap::ResetMeshAABB(uint32_t slot)
{
    // A reused row may lie beyond the prefix copied after world clear. Its
    // master can already be sentinel while a GPU slot still holds older bounds.
    WriteMeshAABB(slot, kNeverCulledAABB, true);
}

// ==== Meshlet sidecar ====
// WriteMeshAABB's rules, for the same reasons: the master takes every write; the active ring slot
// is patched in place only while the game half owns it; every other slot catches up in its own
// BeginFrame (PublishMeshletInfo).
void MeshDescriptorHeap::SetMeshletInfo(uint32_t slot, const MeshletInfo& info)
{
    MESHHEAP_GAME_HALF_ONLY("SetMeshletInfo");
    WriteMeshletInfo(slot, info, false);
}

void MeshDescriptorHeap::ResetMeshletInfo(uint32_t slot)
{
    // Forced like ResetMeshAABB: a reused row beyond the prefix a world clear
    // resynced can hold an older value in a GPU slot while its master is clean.
    WriteMeshletInfo(slot, kNoMeshlets, true);
}

MeshDescriptorHeap::MeshletInfo MeshDescriptorHeap::GetMeshletInfo(uint32_t slot) const
{
    return slot < m_meshletInfoMaster.size() ? m_meshletInfoMaster[slot] : kNoMeshlets;
}

void MeshDescriptorHeap::WriteMeshletInfo(uint32_t slot, const MeshletInfo& info, bool force)
{
    if (slot >= m_meshletInfoMaster.size()) return;
    if (!force && std::memcmp(&m_meshletInfoMaster[slot], &info, sizeof(info)) == 0) return;
    m_meshletInfoMaster[slot] = info;
    const bool writable = m_gfx && m_gfx->IsUploadSlotWritable() &&
        m_currentFrameSlot == m_gfx->GetUploadSlot() && m_meshletInfoMapped[m_currentFrameSlot];
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        if (writable && f == m_currentFrameSlot)
            static_cast<MeshletInfo*>(m_meshletInfoMapped[f])[slot] = info;
        else if (!m_meshletInfoFullResync[f] && !(m_meshletInfoDirtyMask[slot] & (1u << f)))
        {
            m_meshletInfoDirty[f].push_back(slot);
            m_meshletInfoDirtyMask[slot] |= uint8_t(1u << f);
        }
    }
}

void MeshDescriptorHeap::PublishMeshletInfo(uint32_t frameSlot)
{
    ZoneScopedN("MeshletInfo.SnapshotRefresh");
    auto* rows = static_cast<MeshletInfo*>(m_meshletInfoMapped[frameSlot]);
    if (!rows) return;
    if (m_meshletInfoFullResync[frameSlot])
        std::memcpy(rows, m_meshletInfoMaster.data(), size_t(m_meshCount) * sizeof(MeshletInfo));
    else
        for (uint32_t idx : m_meshletInfoDirty[frameSlot])
            if (idx < m_meshletInfoMaster.size()) rows[idx] = m_meshletInfoMaster[idx];
    for (uint32_t idx : m_meshletInfoDirty[frameSlot])
        if (idx < m_meshletInfoDirtyMask.size()) m_meshletInfoDirtyMask[idx] &= uint8_t(~(1u << frameSlot));
    m_meshletInfoDirty[frameSlot].clear();
    m_meshletInfoFullResync[frameSlot] = false;
}

void MeshDescriptorHeap::ValidateMeshAABBFrame()
{
    if (!m_verifyAABBFrames) return;
    ZoneScopedN("MeshAABB.SnapshotOracle");
    // The reset probe is game-half only. GetUploadSlot() is the game half's slot accessor: compared
    // against m_currentFrameSlot on a render-context thread it reads a value -animoverlap has already
    // moved to F+1 (what the [SPLITAUDIT][GAMESLOT] audit reports), and under -brslane this function
    // runs on the render-owner lane because PublishSceneRead does. The probe writes a sentinel into the
    // acquired slot, which belongs to the half that acquired it, so it is skipped there. The snapshot
    // assertion below keys off m_currentFrameSlot and runs on whichever thread finished the frame.
    if (!m_aabbResetVerified && m_meshCount < m_maxMeshes && m_gfx &&
        Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render &&
        m_gfx->IsUploadSlotWritable() && m_currentFrameSlot == m_gfx->GetUploadSlot())
    {
        // Reproduce master-equal / GPU-stale on an UNREGISTERED row in the
        // acquired slot. No in-flight mapping is touched by this probe.
        const uint32_t idx = m_maxMeshes - 1;
        auto* rows = static_cast<MeshAABB*>(m_meshAABBMapped[m_currentFrameSlot]);
        if (rows)
        {
            const auto originalMaster = m_aabbMaster[idx];
            const auto originalGPU = rows[idx];
            m_aabbMaster[idx] = kNeverCulledAABB;
            rows[idx] = MeshAABB{-1.f, -2.f, -3.f, 4.f, 5.f, 6.f};
            ResetMeshAABB(idx);
            const bool resetOK = std::memcmp(&rows[idx], &kNeverCulledAABB, sizeof(MeshAABB)) == 0;
            m_aabbMaster[idx] = originalMaster;
            rows[idx] = originalGPU;
            ENGINE_CHECK(resetOK, "MeshAABB reset skipped master-equal GPU-stale row");
            m_aabbResetVerified = true;
            LOG_INFO("[MESH_AABB_RESET_ORACLE] match=%u", resetOK ? 1u : 0u);
        }
    }
    const auto* mapped = m_meshAABBMapped[m_currentFrameSlot];
    const bool match = mapped && std::memcmp(mapped, m_aabbMaster.data(),
        size_t(m_meshCount) * sizeof(MeshAABB)) == 0;
    ENGINE_CHECK(match, "MeshAABB published slot differs from latest owner bounds");
    for (uint32_t f = 0; f < kFrameCount; ++f)
        for (uint32_t other = 0; other < f; ++other)
            ENGINE_CHECK(m_meshAABBMapped[f] != m_meshAABBMapped[other], "MeshAABB frame mappings alias");
    LOG_INFO("[MESH_AABB_SNAPSHOT] slot=%u meshes=%u direct=%llu deferred=%llu match=%u",
        m_currentFrameSlot, m_meshCount, (unsigned long long)m_aabbDirectWrites,
        (unsigned long long)m_aabbDeferredWrites, match ? 1u : 0u);
}

void MeshDescriptorHeap::ValidateMeshAABBIdleWrite()
{
    // Called only at App's parked owner boundary. Exercise deferred writes
    // without modifying any registered mesh: the last unused row is restored
    // before returning and can only enter a later frame through normal resync.
    if (!m_verifyAABBFrames || m_aabbIdleWriteVerified || !m_gfx ||
        m_gfx->IsUploadSlotWritable() || m_meshCount >= m_maxMeshes || m_aabbMaster.empty()) return;
    const uint32_t idx = m_maxMeshes - 1;
    MeshAABB before[kFrameCount];
    for (uint32_t f = 0; f < kFrameCount; ++f)
    {
        if (!m_meshAABBMapped[f]) return;
        before[f] = static_cast<const MeshAABB*>(m_meshAABBMapped[f])[idx];
    }
    const auto original = m_aabbMaster[idx];
    const MeshAABB probe{-17.f, -13.f, -11.f, 19.f, 23.f, 29.f};
    SetMeshAABB(idx, probe);
    bool match = std::memcmp(&m_aabbMaster[idx], &probe, sizeof(probe)) == 0;
    for (uint32_t f = 0; f < kFrameCount; ++f)
        match &= std::memcmp(&static_cast<const MeshAABB*>(m_meshAABBMapped[f])[idx],
                             &before[f], sizeof(probe)) == 0;
    SetMeshAABB(idx, original);
    ENGINE_CHECK(match, "MeshAABB idle write changed a GPU snapshot");
    m_aabbIdleWriteVerified = true;
    LOG_INFO("[MESH_AABB_IDLE_ORACLE] match=%u", match ? 1u : 0u);
}

uint64_t MeshDescriptorHeap::GetBufferTableGpuHandle() const
{
    return m_bufferRange.GetGpuHandle().ptr;
}

void MeshDescriptorHeap::OnWorldClear()
{
    // World-scoped allocators rewind to the persistent base; persistent counters and SRVs
    // (slot < kPermanent*Slots) are untouched, so RegisterPersistent* indices stay valid across the
    // reload. m_resourceToSlot is world-scoped only (RegisterPersistent* bypasses it): clear it fully.
    // m_master keeps its memory; rows past kPermanentMeshSlots hold the old world's bytes, but nothing
    // references them (the skinned / meshlib descriptor caches and per-entity meshDescriptorIdx were
    // cleared upstream) and new RegisterMesh calls overwrite them.
    // Log the peak the departing world reached: the only place these numbers surface without an
    // editor panel. Do not rewind the high-water marks: a peak climbing across reloads of one scene
    // is the signature of a per-reload slot leak.
    LOG_INFO("MeshDescriptorHeap: world clear at %u/%u buffer slots, "
             "%u/%u mesh slots (session peak %u / %u; overflows %u / %u)",
             m_bufferCount, kMaxBuffers, m_meshCount, m_maxMeshes,
             m_bufferHighWater, m_meshHighWater,
             m_bufferOverflowCount, m_meshOverflowCount);

    m_meshCount   = kPermanentMeshSlots;
    m_bufferCount = kPermanentBufferSlots;
    m_resourceToSlot.clear();

    // World-scoped recycle state names the world we're tearing down; drop it so
    // a freed slot index can't leak into the next world's allocators.
    m_meshFreeList.clear();
    m_bufferFreeList.clear();
    m_bufferSlotResource.clear();
    // Detached-but-not-freed indices name the world being torn down. Renderer::OnWorldClear drops its
    // pending hand-backs rather than draining them, so without this the set grows by one entry per
    // world load.
    m_bufferSlotDetached.clear();
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        m_meshFreePending[i].clear();
        m_bufferFreePending[i].clear();
    }

    // Force every ring slot to fully resync to the new world on its next BeginFrame, and drop pending
    // dirty indices: they refer to the old world's layout and m_meshCount just shrank.
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        m_slotNeedsFullResync[i] = true;
        m_slotDirty[i].clear();
        m_aabbFullResync[i] = true;
        m_aabbDirty[i].clear();
        m_meshletInfoFullResync[i] = true;
        m_meshletInfoDirty[i].clear();
    }
    std::fill(m_slotDirtyMask.begin(), m_slotDirtyMask.end(), uint8_t{0});
    std::fill(m_aabbDirtyMask.begin(), m_aabbDirtyMask.end(), uint8_t{0});
    std::fill(m_meshletInfoDirtyMask.begin(), m_meshletInfoDirtyMask.end(), uint8_t{0});
}

void MeshDescriptorHeap::FreeMesh(uint32_t slot)
{
    MESHHEAP_GAME_HALF_ONLY("FreeMesh");
    // Reported before the range filter, so the audit can tell "FreeMesh is never reached" from
    // "reached, but every slot was filtered out".
    static bool s_reportedEntry = false;
    if (!s_reportedEntry)
    {
        s_reportedEntry = true;
        LOG_INFO("[SPLITAUDIT][HEAPOWNER] FreeMesh reached (slot %u, permanent<%u, "
                 "count %u)", slot, kPermanentMeshSlots, m_meshCount);
    }

    // Persistent slots [0..kPermanentMeshSlots) live forever; only world-scoped slots recycle.
    // Out of range (incl. ~0u "not registered") -> ignore.
    if (slot < kPermanentMeshSlots || slot >= m_meshCount) return;

    // Overwrite with a degenerate (0-vertex) descriptor so any lingering stale
    // reference (a cull-BVH leaf or cached draw candidate still naming this slot
    // before the next gather) draws nothing rather than the freed mesh.
    RHI::MeshDescriptor degenerate{};
    m_master[slot] = degenerate;
    // No meshlets either: the library behind them may be gone by the time this slot is drawn
    // through.
    ResetMeshletInfo(slot);

    // The direct write into the ACTIVE ring slot is legal only while the game side owns it. FreeMesh
    // runs from World::DestroyEntity (via SkinnedMeshSubsystem::OnEntityDestroyed) in any gameplay
    // phase, including before the Animation phase rotates this ring; m_currentFrameSlot then still
    // names the slot the GPU is reading, and a degenerate write makes that in-flight frame drop the
    // mesh (a visible pop the debug layer cannot see). The game side owns the ring exactly when the
    // slot it rotated to is the upload slot it claimed for the frame it simulates.
    const bool gameOwnsSlot = m_gfx
                           && m_gfx->IsUploadSlotWritable()
                           && m_currentFrameSlot == m_gfx->GetUploadSlot();

    if (gameOwnsSlot && m_meshDescMapped[m_currentFrameSlot])
    {
        // Reported once, like the deferred branch below: together they show whether this guard is
        // exercised at all.
        static bool s_reportedFreeDirect = false;
        if (!s_reportedFreeDirect)
        {
            s_reportedFreeDirect = true;
            LOG_INFO("[SPLITAUDIT][HEAPOWNER] FreeMesh inside the game-owned "
                     "window (ring slot %u) - direct write", m_currentFrameSlot);
        }
        static_cast<RHI::MeshDescriptor*>(m_meshDescMapped[m_currentFrameSlot])[slot] = degenerate;
        MarkStableDirty(slot);
    }
    else
    {
        // Outside the window every slot, the current one INCLUDED, must be caught by the deferred path.
        // MarkStableDirty skips m_currentFrameSlot (it assumes a direct write), so pushing it explicitly
        // is load-bearing. Reported once: a deferred branch that never runs looks identical in every test.
        static bool s_reportedFree = false;
        if (!s_reportedFree)
        {
            s_reportedFree = true;
            LOG_INFO("[SPLITAUDIT][HEAPOWNER] FreeMesh outside the game-owned "
                     "window (ring slot %u, game slot %u) - deferred to the dirty "
                     "list instead of writing a slot the GPU is reading",
                     m_currentFrameSlot, m_gfx ? m_gfx->GetUploadSlot() : 0u);
        }
        MarkStableDirty(slot);
        if (!m_slotNeedsFullResync[m_currentFrameSlot])
            MarkSlotDirty(m_currentFrameSlot, slot);
    }

    m_meshFreePending[m_currentFrameSlot].push_back(slot);
}

void MeshDescriptorHeap::FreeBuffer(const RHI::GPUBuffer& buf)
{
    MESHHEAP_GAME_HALF_ONLY("FreeBuffer");
    if (!buf.IsValid()) return;
    auto* dx12 = static_cast<GraphicsDX12*>(m_gfx);
    if (!dx12) return;
    ID3D12Resource* resource = dx12->GetBufferResource(buf);
    if (!resource) return;

    auto it = m_resourceToSlot.find(resource);
    if (it == m_resourceToSlot.end()) return;     // not a world-scoped registered buffer
    const uint32_t slot = it->second;
    if (slot < kPermanentBufferSlots) return;      // persistent - never recycle

    m_resourceToSlot.erase(it);
    m_bufferSlotResource.erase(slot);
    // The SRV at `slot` now dangles at the about-to-be-freed resource, but
    // nothing reads it (its entity is gone); the kFrameCount-deferred reuse
    // rewrites the descriptor via WriteRawBufferSrv before the slot is live.
    m_bufferFreePending[m_currentFrameSlot].push_back(slot);
}

// Step 1 of 2 (contract at the declaration): drop the REGISTRATION for slot while its resource is
// still alive. Nothing is freed and no descriptor is touched.
void MeshDescriptorHeap::DetachBufferSlot(uint32_t slot)
{
    if (slot < kPermanentBufferSlots || slot >= m_bufferCount) return;

    auto sit = m_bufferSlotResource.find(slot);
    if (sit == m_bufferSlotResource.end()) return;   // never registered / already detached

    // The resource is alive by this call's contract, so m_resourceToSlot[R] still names this slot.
    // The both-ways test is a sanity check, not a staleness guard: two stale entries agree with each
    // other.
    auto rit = m_resourceToSlot.find(sit->second);
    if (rit != m_resourceToSlot.end() && rit->second == slot)
        m_resourceToSlot.erase(rit);
    m_bufferSlotResource.erase(sit);

    m_bufferSlotDetached.insert(slot);
}

// Step 2 of 2, kFrameCount frames later: hand the INDEX back without reading the pointer maps (the
// resource is destroyed and its address may belong to another one).
void MeshDescriptorHeap::FreeBufferSlot(uint32_t slot)
{
    MESHHEAP_GAME_HALF_ONLY("FreeBufferSlot");
    if (slot < kPermanentBufferSlots || slot >= m_bufferCount) return;
    // Only a DETACHED slot can be freed by index: this stops a double hand-back from pushing one index
    // into the free list twice (two callers would get the same bindless slot).
    if (m_bufferSlotDetached.erase(slot) == 0) return;

    m_bufferFreePending[m_currentFrameSlot].push_back(slot);
}
