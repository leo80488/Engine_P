#pragma once

// MeshDescriptorHeap - owns the two GPU-side PVF (programmable vertex fetch) resources:
//  1. The bindless buffer table: kMaxBuffers contiguous ByteAddressBuffer SRVs in the CBV/SRV/UAV
//     heap, bound as the g_Buffers[] descriptor table (root param kBindlessSlot). RegisterBuffer
//     returns the stable bindless index a StreamDescriptor::bufferIndex holds.
//  2. The MeshDescriptor StructuredBuffer: persistently mapped UPLOAD buffers of RHI::MeshDescriptor,
//     bound through Upload/CurrentMeshDescBuffer() and IGraphicsDevice::SetRootBufferSRV. RegisterMesh
//     writes a descriptor and returns its stable slot index.
// Slot lifetime: world-scoped (RegisterBuffer / RegisterMesh, cleared by OnWorldClear) or
// engine-lifetime (RegisterPersistentBuffer / RegisterPersistentMesh, kept across every world reload,
// so the index can be cached at Init).
// A DX12-specific helper: the public surface is RHI types, but the implementation writes
// descriptor-heap SRVs through D3D12 directly (RHI does not abstract bindless tables), so this header
// includes d3d12.h and every TU that includes it sees the DX12 API.
// Call Init() once at startup.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/FrameConstants.h"
#include <windows.h>
#include <wrl.h>
#include "d3d12.h"

#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/RootSignatureLayout.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

class GraphicsDX12;
class IGraphicsDevice;

class MeshDescriptorHeap
{
public:
    // Bindless g_Buffers[] slots - must match RHI::Slots::MaxBindlessBuffers (kMaxBindlessBuffers) and
    // the explicit [16384] arrays in the shaders (pvf_fetch.hlsli, DDGIRayTrace.cs.hlsl's g_DDGIBuffers,
    // the RT passes).
    // Sized for character-heavy scenes: ~30 NPCs x 25 submeshes x 2 buffers = ~1500 skinned slots + a few
    // hundred static + 256 persistent = ~2000 typical, ~14k headroom; far below the shader-visible heap.
    static constexpr uint32_t kMaxBuffers = 16384;
    // This capacity IS the g_Buffers[] descriptor-range size baked into every root signature that
    // declares the bindless buffer table (the shared one and the private RT / DDGI ones). Drift is a
    // range overrun, not a compile error, so it is pinned to the shared layout - and it stays static
    // constexpr: a second heap serving three meshes still allocates all 16384 contiguous descriptors.
    // Only the mesh-slot cap is per instance (kDefaultMaxMeshes -> m_maxMeshes).
    static_assert(kMaxBuffers == RHI::Slots::MaxBindlessBuffers,
                  "MeshDescriptorHeap::kMaxBuffers must match "
                  "RHI::Slots::MaxBindlessBuffers (RootSignatureLayout.h)");
    // MeshDescriptor slots - one per unique (mesh, submesh-range) pair. Bistro-class scenes carry ~22k
    // submeshes in one merged .imsh; aggregates of several such scenes pass 100k.
    // Every slot is paid UP FRONT, and none of it is VRAM: the descriptor ring and the AABB ring are
    // UPLOAD buffers created at the full cap and permanently mapped, and the CPU masters match, so an
    // EMPTY scene pays the whole bill in host RAM / BAR:
    //   descriptor ring : kFrameCount x cap x sizeof(RHI::MeshDescriptor)
    //   master mirror   : cap x sizeof(RHI::MeshDescriptor)   (CPU only)
    //   AABB ring/master: (kFrameCount + 1) x cap x sizeof(MeshAABB)
    // At the 131072 default (sizeof(RHI::MeshDescriptor) == 160, pinned in
    // shaders/shared/mesh_descriptor.hlsli, UV2/UV3 included; MeshAABB 24 B) that is
    // 3 x 20 + 20 + 4 x 3 = ~92 MiB, plus dirty lists. For the real number multiply by sizeof(): the
    // static_asserts compile, this comment does not.
    // Running out is neither silent nor fatal: NotePoolPressure warns at 75 % / 90 % naming this cap,
    // overflow returns an invalid slot and logs, and the editor shows it as a diagnostic.
    // This is only the DEFAULT of the per-instance cap (m_maxMeshes, set by Init). A SECOND heap must also
    // honour entity-cached slot indices, a per-frame BeginFrame and FlushAndWait before OnWorldClear.
    static constexpr uint32_t kDefaultMaxMeshes = 131072;
    // MUST match GraphicsDX12::kFrameCount. Triple-buffering the descriptor ring keeps the CPU's
    // per-frame skinned-mesh patches (position.bufferIndex / byteOffset) from racing the GPU reads of a
    // frame still in flight; without it late passes such as OutlinePass read the next frame's bindless
    // index into a not-yet-written ring slice (an inverted hull lagging behind a moving character).
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    // Slot range [0..kPermanent*Slots) is reserved for ENGINE-LIFETIME registrations (procedural
    // primitives, the skinned-vertex ring, ...). OnWorldClear() does NOT reset it, so callers can cache
    // the returned index at Init. Sized at several times the current use (~42 buffers / 6 meshes: five
    // primitives, the billboard, the skinned vertex ring) so a new engine-lifetime registration needs no
    // constant bump; 256 of the 16384 bindless slots leave 16128 for the world.
    static constexpr uint32_t kPermanentBufferSlots = 256;
    static constexpr uint32_t kPermanentMeshSlots   = 64;

    // Per-mesh local AABB buffer (parallel to MeshDescriptor buffer).
    // Each entry is 6 floats: {minX, minY, minZ, maxX, maxY, maxZ} = 24 bytes.
    struct MeshAABB { float minX, minY, minZ, maxX, maxY, maxZ; };
    static_assert(sizeof(MeshAABB) == 24);

    // "Never culled" sentinel. The GPU command cull reads this buffer for every draw, so a mesh whose
    // registration path publishes no real bounds must land on a box that always passes - never on what
    // the slot's previous occupant left behind.
    static constexpr MeshAABB kNeverCulledAABB =
        { -1e18f, -1e18f, -1e18f, 1e18f, 1e18f, 1e18f };

    // ---- Meshlet sidecar ----
    struct MeshletInfo
    {
        uint32_t blobBufferIdx;   // g_Buffers[] index of the library's meshlet buffer; 0xFFFFFFFF = none
        uint32_t firstCluster;    // the entry's first MeshletDesc (element index in that buffer)
        uint32_t clusterCount;    // every level - what the shadow views draw from
        // The leading clusters the SHADED main view may draw (SceneMeshlet.as):
        // its levels stop before the first one that fails the LOD shading check
        // (Resource/MeshletData.h); == clusterCount when every level passes.
        uint32_t shadedClusterCount;
    };
    static_assert(sizeof(MeshletInfo) == 16);
    static constexpr MeshletInfo kNoMeshlets = { 0xFFFFFFFFu, 0u, 0u, 0u };

    // ---- Pool watermarks ----
    struct PoolStats
    {
        // World-scoped allocators. The base is NOT zero: the world allocator starts at kPermanent*Slots, so a
        // freshly cleared world reports current == 256 / 64. Subtract *Reserved for "how much this world
        // uses", or an empty scene looks like a few hundred leaked slots.
        uint32_t bufferCurrent, bufferHighWater, bufferCapacity, bufferReserved;
        uint32_t meshCurrent,   meshHighWater,   meshCapacity,   meshReserved;

        // Engine-lifetime allocators: monotonic, never reset, not even by OnWorldClear (the contract
        // RegisterPersistent* offers).
        uint32_t persistentBufferCurrent, persistentBufferCapacity;
        uint32_t persistentMeshCurrent,   persistentMeshCapacity;

        // Slots handed back by FreeMesh/FreeBuffer that are reusable NOW
        // (the kFrameCount-deferred pending buckets are not counted). A big
        // free list next to a high-water pinned at the cap means recycling is
        // not keeping up with registration, not that the cap is wrong.
        uint32_t bufferFreeList, meshFreeList;

        // Times a Register* path had to return kInvalidBufferIndex. ANY
        // non-zero value means geometry silently did not render.
        uint32_t bufferOverflows, meshOverflows;
        uint32_t persistentBufferOverflows, persistentMeshOverflows;
    };

    // ---- Lifetime, registration and the descriptor ring ----
    MeshDescriptorHeap() = default;
    ~MeshDescriptorHeap();

    // Call once before any Register* call. Takes IGraphicsDevice& and downcasts to the DX12 backend.
    // @p maxMeshes is THIS heap's mesh-descriptor slot cap; a value below kPermanentMeshSlots (where the
    // world allocator starts) is rejected and clamped back up.
    void Init(IGraphicsDevice& gfx, uint32_t maxMeshes = kDefaultMaxMeshes);

    // Per-frame: pick the GPU slot the frame just released, then refresh it
    // from the CPU-side master so any stable Register/UpdateMesh writes done
    // during previous frames are present.  Must be called once at the start
    // of the frame, BEFORE any per-frame UpdateMeshThisFrame() patches.
    void BeginFrame(uint32_t frameIndex);

    // Write a MeshDescriptor to the persistently-mapped GPU buffer.
    // Returns the stable mesh slot index (== DrawPacket::meshDescriptorIndex).
    // World-scoped: slot is invalidated by OnWorldClear().
    uint32_t RegisterMesh(const RHI::MeshDescriptor& desc);

    // PERSISTENT (engine-lifetime) registration. Allocated from the reserved
    // low slot range [0..kPermanent*Slots); the returned index stays valid
    // across every world reload so Init-time callers can stash it once.
    //
    // No resource-dedup is performed: callers are expected to invoke each
    // persistent register exactly once per app lifetime (Init code, not
    // hot paths). Each call consumes one slot until the cap is hit.
    uint32_t RegisterPersistentBuffer(const RHI::GPUBuffer& buf);
    uint32_t RegisterPersistentMesh  (const RHI::MeshDescriptor& desc);

    // STABLE update: persists across frames (master + current GPU slot).
    // Use for descriptors whose value is expected to survive multiple frames
    // (e.g. afterimage snapshot patches pointing at a non-ring pool buffer).
    void UpdateMesh(uint32_t slot, const RHI::MeshDescriptor& desc);

    // TRANSIENT update: written ONLY to the current frame's GPU slot, NOT to
    // the master.  Use for per-frame patches whose value depends on the
    // current frame slot (e.g. SkinnedMeshSubsystem's per-frame patch where
    // `position.bufferIndex` references the per-frame skinned-vertex ring).
    // Next BeginFrame() will reset that slot from master, so the caller MUST
    // re-issue this patch every frame the override is needed.
    void UpdateMeshThisFrame(uint32_t slot, const RHI::MeshDescriptor& desc);

    // Read back the current value of a mesh descriptor slot (for skinned-mesh patching).
    // Returns a zeroed descriptor if slot is out of range.
    RHI::MeshDescriptor GetMesh(uint32_t slot) const;

    // Registers a GPUBuffer as a ByteAddressBuffer SRV in the bindless table. World-scoped: OnWorldClear()
    // invalidates the slot (use RegisterPersistentBuffer for engine-lifetime buffers). Returns the
    // bindless buffer index, or kInvalidBufferIndex on failure.
    uint32_t RegisterBuffer(const RHI::GPUBuffer& buf);
    // Read-only cache validation. Caller must own the registration phase;
    // does not allocate/repair a missing slot and is not a concurrent lookup.
    uint32_t FindRegisteredBuffer(const RHI::GPUBuffer& buf) const;

    // GPU handle of the start of the bindless SRV table (for kBindlessSlot), as a raw uint64_t like the
    // other RHI GPU descriptor handles (e.g. IGraphicsDevice::GetTextureSRVGpuHandle).
    uint64_t GetBufferTableGpuHandle() const;

    // The MeshDescriptor StructuredBuffer, keyed by frame half; rebind every frame either way.
    // UploadMeshDescBuffer is the GAME slot (m_currentFrameSlot: BeginFrame runs with GetUploadSlot()
    // from Renderer::TickAnimationChain); CurrentMeshDescBuffer is the RENDER slot (GetFrameIndex()). A
    // render-half reader must use Current: the upload slot is the one the game thread may be memcpy'ing
    // into, and the debug layer cannot see that tearing. The two are equal while the loop is serial and
    // differ by one slot once the render half runs a frame behind. Same shape as FrameCB<T>::Upload() /
    // ::Current().
    const RHI::GPUBuffer& UploadMeshDescBuffer() const
    { return m_meshDescBuffers[m_currentFrameSlot]; }
    // Out of line: IGraphicsDevice is only forward-declared here.
    const RHI::GPUBuffer& CurrentMeshDescBuffer() const;

    // ---- Per-mesh AABBs ----
    void SetMeshAABB(uint32_t slot, const MeshAABB& aabb);
    // Restore a slot to kNeverCulledAABB. Called by RegisterMesh so a recycled
    // slot never inherits the previous mesh's bounds.
    void ResetMeshAABB(uint32_t slot);
    // Render-slot reader, like CurrentMeshDescBuffer; never the mutable game slot.
    const RHI::GPUBuffer& CurrentMeshAABBBuffer() const;
    void ValidateMeshAABBFrame(); // opt-in publication oracle
    void ValidateMeshAABBIdleWrite(); // harness: unused-row probe while owner is parked

    // ---- Meshlet sidecar ----
    // 16 bytes per mesh slot, indexed by meshDescIdx: where that slot's clusters are. The amplification
    // shader reads it through the bindless g_Buffers[] index CurrentMeshletInfoBindless()
    // (MeshletCullCB.sidecarIdx).
    // Same machinery as the per-slot AABB: kFrameCount persistently mapped UPLOAD buffers + a CPU master
    // + per-ring-slot dirty lists, published in BeginFrame, written on the game half only, read by the
    // render half at GetFrameIndex(). RegisterMesh AND FreeMesh reset a slot to kNoMeshlets: a recycled
    // slot must never name the previous mesh's clusters (their buffer may already be freed:
    // DEVICE_REMOVED, not a wrong picture). RegisterMeshLibMesh is the one writer of real values.
    void        SetMeshletInfo(uint32_t slot, const MeshletInfo& info);
    void        ResetMeshletInfo(uint32_t slot);
    MeshletInfo GetMeshletInfo(uint32_t slot) const;      // the CPU master (explain, tests)
    // The render slot's sidecar as a g_Buffers[] index (persistent registration).
    uint32_t    CurrentMeshletInfoBindless() const;

    // ---- Slot counts and capacity ----
    uint32_t GetBufferCount() const { return m_bufferCount; }
    uint32_t GetMeshCount()   const { return m_meshCount;   }
    // This heap's mesh-slot cap: kDefaultMaxMeshes until Init says otherwise; PoolStats::meshCapacity
    // reports the same value.
    uint32_t GetMeshCapacity() const { return m_maxMeshes;  }

    // ---- Pool watermarks ----
    // On exhaustion the Register* paths return kInvalidBufferIndex and the mesh simply stops rendering,
    // with nothing on screen tying that to a slot budget, and the .cpp's exhaustion reports fire only once
    // a pool is already dry. These counters measure the approach.
    // The high-water marks SURVIVE OnWorldClear: the useful number is the session's worst peak (a
    // per-reload leak shows as a high-water that keeps climbing across loads of the same scene); the
    // *Current fields give the loaded world.
    PoolStats GetPoolStats() const;

    // Rewinds the high-water marks to the current counts and clears the overflow counters and
    // pressure-warning latches (the editor's "reset watermarks" button), so the numbers read "since the
    // reset" - for measuring one scene load rather than a whole editor session.
    void ResetPoolWatermarks();

    // ---- World clear ----
    // Resets the WORLD-SCOPED slot allocators and the dedup map to the persistent base. Required on every
    // world reload: otherwise each reload re-registers at higher indices and a Bistro-scale scene
    // exhausts the bindless table within ~3 reloads.
    // The persistent range [0..kPermanent*Slots) is UNTOUCHED: RegisterPersistent* slots keep their index
    // and SRV across the reload with no re-registration.
    // The caller MUST FlushAndWait first: world slots (SRVs at indices >= kPermanent*Slots) may still be
    // bound by an in-flight command list. The descriptor range stays allocated; only the world-scoped
    // logical contents are reset.
    void OnWorldClear();

    // ---- Per-entity slot recycling ----
    // Reclaims a WORLD-scoped slot (RegisterMesh / RegisterBuffer) when its owning entity is destroyed;
    // without it the monotonic allocators leak a slot per destroyed entity until the next OnWorldClear
    // (~80 delete + reload cycles of a skinned character exhaust kMaxBuffers). A freed slot is held
    // kFrameCount frames before reuse, so no in-flight command list still references it (as
    // GraphicsDX12's deferred resource release). No-op for persistent / out-of-range slots and for a
    // buffer that was never world-registered.
    void FreeMesh(uint32_t slot);
    void FreeBuffer(const RHI::GPUBuffer& buf);

    // FreeBuffer, split in two. FreeBuffer resolves the slot FROM the ID3D12Resource*, so it only works
    // while the buffer is alive, which ties freeing the slot to releasing its owner. A caller handing a
    // library back NOW (so the next Load re-reads the file) must not disturb descriptors this frame's
    // command lists read - and the registration cannot wait for the free either.
    // The hazard is ProcessAllDeferredReleases (WaitForPreviousFrame, Resize, the destructor): it drains
    // every device release bucket regardless of stamp, while the slot hand-back queue keeps its own
    // predicate until a later frame. In that window the resource is gone with m_resourceToSlot and
    // m_bufferSlotResource still naming it, a new resource can land on the recycled ADDRESS, and
    // RegisterRawBuffer's pointer dedup returns the OLD slot without rewriting the SRV: the new buffer
    // draws through a dead descriptor, and the deferred free later recycles the slot from under a live
    // library. (In normal frames the two clocks agree: the device frees the resource at m_renderFrame -
    // kFrameCount, the hand-back at GetRetiredFrame(), the same value.)
    // So DetachBufferSlot runs while the resource is PROVABLY ALIVE and drops the registration (no dedup
    // hit can name a dead pointer), and FreeBufferSlot - which never consults the pointer maps - hands
    // the index back kFrameCount frames later. Both are no-ops for a persistent / out-of-range slot;
    // FreeBufferSlot is also a no-op for a slot that was not detached first, so a double hand-back is
    // impossible.
    void DetachBufferSlot(uint32_t slot);
    void FreeBufferSlot  (uint32_t slot);

private:
    static_assert(kFrameCount <= 8, "Pending-slot masks store one byte per mesh");

    // Internal D3D12-typed register paths, private so the public surface stays RHI-typed. RegisterBuffer
    // / RegisterPersistentBuffer resolve the ID3D12Resource* through GraphicsDX12 and forward here.
    uint32_t RegisterRawBuffer          (ID3D12Resource* resource, UINT64 sizeBytes);
    uint32_t RegisterPersistentRawBuffer(ID3D12Resource* resource, UINT64 sizeBytes);

    // Internal: write the ByteAddressBuffer SRV for `resource` at the given
    // absolute bindless slot. Shared by world + persistent register paths.
    void WriteRawBufferSrv(uint32_t slot, ID3D12Resource* resource, UINT64 sizeBytes);

    // Log ONE warning as a pool crosses 75%, and one more at 90%, of its
    // capacity. `warnedFlags` is the caller's latch (bit0 = 75%, bit1 = 90%)
    // so a world load that registers tens of thousands of meshes contributes
    // at most two lines instead of one per Register* call.
    static void NotePoolPressure(const char* poolName, const char* capConstant,
                                 uint32_t used, uint32_t cap, uint8_t& warnedFlags);

    void MarkSlotDirty(uint32_t frameSlot, uint32_t idx);

    // Record that master[idx] changed via a stable write: every ring slot
    // EXCEPT the just-written active one must recopy it on its next BeginFrame.
    void MarkStableDirty(uint32_t idx);

    void WriteMeshAABB(uint32_t slot, const MeshAABB& aabb, bool force);

    void WriteMeshletInfo(uint32_t slot, const MeshletInfo& info, bool force);
    void PublishMeshletInfo(uint32_t frameSlot);

    IGraphicsDevice*         m_gfx        = nullptr;
    ID3D12Device*            m_device     = nullptr;
    DescriptorHeapAllocator* m_allocator  = nullptr;
    UINT                     m_descSize   = 0;

    // Per-instance mesh-slot cap, set once by Init and read by every sizing, bounds and census path
    // below. kMaxBuffers deliberately has no twin (see its static_assert).
    uint32_t                 m_maxMeshes  = kDefaultMaxMeshes;

    // Contiguous SRV block for g_Buffers[]. Layout:
    //   [0 .. kPermanentBufferSlots)              persistent (Init-time)
    //   [kPermanentBufferSlots .. m_bufferCount)  world-scoped (this reload)
    //   [m_bufferCount .. kMaxBuffers)            unused
    DescriptorAllocation     m_bufferRange;
    uint32_t                 m_persistentBufferCount = 0;                    // grows on RegisterPersistent*; never reset
    uint32_t                 m_bufferCount           = kPermanentBufferSlots; // world allocator; OnWorldClear rewinds it
    // Resource -> slot dedup for WORLD-scoped registrations only. The bindless table is a finite pool, so
    // N mesh entities sharing one VB/IB (a merged-per-scene .imsh shares one pair across a whole Bistro)
    // must resolve to the SAME slot; 22k submeshes x 2 registrations would otherwise overflow it.
    // Persistent registrations bypass this map (each resource is registered at most once).
    std::unordered_map<ID3D12Resource*, uint32_t> m_resourceToSlot;

    // Persistently mapped UPLOAD buffers of the MeshDescriptor StructuredBuffer, a kFrameCount-deep ring;
    // BeginFrame() rotates the bound buffer, so per-frame skinning patches never race a GPU read.
    RHI::GPUBuffer   m_meshDescBuffers[kFrameCount];
    void*            m_meshDescMapped [kFrameCount]{};
    uint32_t         m_persistentMeshCount = 0;                  // grows on RegisterPersistentMesh; never reset
    uint32_t         m_meshCount           = kPermanentMeshSlots; // world allocator; OnWorldClear resets to base
    uint32_t         m_currentFrameSlot    = 0;

    // ---- Per-entity slot recycling (deferred kFrameCount) ----
    // Free*() pushes a reclaimed world-scoped slot into m_*FreePending[cur]; BeginFrame drains the bucket
    // for the frame slot cycling back (kFrameCount frames later) into m_*FreeList; Register* pop the free
    // list before bumping the high-water counter. m_bufferSlotResource lets FreeBuffer drop the dedup
    // entry by slot. All cleared by OnWorldClear (slots rewind to base).
    std::vector<uint32_t> m_meshFreeList;
    std::vector<uint32_t> m_bufferFreeList;
    std::vector<uint32_t> m_meshFreePending[kFrameCount];
    std::vector<uint32_t> m_bufferFreePending[kFrameCount];
    std::unordered_map<uint32_t, ID3D12Resource*> m_bufferSlotResource;
    // Slots whose REGISTRATION was dropped (DetachBufferSlot) but whose INDEX is not handed back yet - the
    // only thing FreeBufferSlot consults, since by then the slot's ID3D12Resource* may be destroyed and
    // its address reused, so the pointer maps know nothing about it. Membership also makes a second
    // FreeBufferSlot on one index a no-op. Empty in normal use (a few entries while a preview hand-back
    // is in flight).
    std::unordered_set<uint32_t> m_bufferSlotDetached;

    // CPU-side master copy: the source of truth for "stable" descriptors
    // (RegisterMesh + stable UpdateMesh).  BeginFrame() refreshes the new
    // frame's GPU slot from this so stable writes propagate and the slot's
    // previous tenant's transient patches are wiped.
    std::vector<RHI::MeshDescriptor> m_master;

    // ---- Incremental BeginFrame refresh ----
    // Re-copying the whole [0..m_meshCount) master prefix into the rotated GPU slot every frame would be
    // multi-MB on 22k-100k-submesh worlds, almost all of it unchanged since the slot was last active
    // kFrameCount frames ago. Each ring slot instead tracks the indices where it diverges from m_master
    // and recopies only those. A slot diverges in exactly two ways, both fixed by recopying from master:
    //   (a) a STABLE write (RegisterMesh / RegisterPersistentMesh / UpdateMesh) changed m_master while
    //       the slot was inactive;
    //   (b) a TRANSIENT UpdateMeshThisFrame patched the slot directly last cycle and must be wiped.
    // The list is a CONSERVATIVE superset: a duplicate or already-matching index only costs an
    // idempotent recopy.
    std::vector<uint32_t> m_slotDirty[kFrameCount];
    // -meshdirtydedup (opt-in): one bit per (mesh, ring slot) drops duplicate dirty-list entries; off by
    // default, as the benchmark corpus has no natural duplicates.
    std::vector<uint8_t> m_slotDirtyMask;

    bool m_verifyMeshDirty = false;
    bool m_dedupMeshDirty = false;
    uint64_t m_meshDirtyDuplicates = 0;
    // Forces a full master->slot memcpy on this slot's next BeginFrame. Set at Init and OnWorldClear (the
    // slot was never synced to the current world), and as the overflow fallback (see BeginFrame), so a
    // slot never copies more than one full-prefix memcpy.
    bool m_slotNeedsFullResync[kFrameCount]{};

    // Owner-written bounds master and fenced GPU snapshots. Inactive mappings
    // are never patched; each catches up only when its upload slot is acquired.
    RHI::GPUBuffer m_meshAABBBuffers[kFrameCount];
    void* m_meshAABBMapped[kFrameCount]{};
    std::vector<MeshAABB> m_aabbMaster;
    std::vector<uint32_t> m_aabbDirty[kFrameCount];
    std::vector<uint8_t> m_aabbDirtyMask;

    bool m_aabbFullResync[kFrameCount]{};
    bool m_verifyAABBFrames = false;
    bool m_aabbIdleWriteVerified = false;
    bool m_aabbResetVerified = false;
    uint64_t m_aabbDirectWrites = 0, m_aabbDeferredWrites = 0;

    // Meshlet sidecar (SetMeshletInfo): the AABB ring's twin. Each frame buffer
    // is also registered once as a PERSISTENT g_Buffers[] slot (it lives as
    // long as the heap, across world clears).
    RHI::GPUBuffer           m_meshletInfoBuffers[kFrameCount];
    void*                    m_meshletInfoMapped[kFrameCount]{};
    uint32_t                 m_meshletInfoBindless[kFrameCount]{};   // set by Init (kInvalidBufferIndex before)
    std::vector<MeshletInfo> m_meshletInfoMaster;
    std::vector<uint32_t>    m_meshletInfoDirty[kFrameCount];
    std::vector<uint8_t>     m_meshletInfoDirtyMask;
    bool                     m_meshletInfoFullResync[kFrameCount]{};

    // ---- Pool watermarks (see PoolStats) ----
    // Seeded at the allocator bases, NOT at 0, so the peak uses the same coordinates as m_bufferCount /
    // m_meshCount. Untouched by OnWorldClear on purpose: a high-water that climbs across repeated loads of
    // the SAME scene is the signature of a per-reload slot leak, and rewinding the peak with the
    // allocator would erase it.
    uint32_t m_bufferHighWater = kPermanentBufferSlots;
    uint32_t m_meshHighWater   = kPermanentMeshSlots;

    // Non-zero == geometry silently failed to render. Counts every rejected
    // Register* call, not just the first (the LOG_ERROR fires once per call
    // site per run; the magnitude lives here).
    uint32_t m_bufferOverflowCount           = 0;
    uint32_t m_meshOverflowCount             = 0;
    uint32_t m_persistentBufferOverflowCount = 0;
    uint32_t m_persistentMeshOverflowCount   = 0;

    // 75% / 90% pressure-warning latches (bit0 / bit1) - see NotePoolPressure.
    uint8_t  m_bufferPressureWarned           = 0;
    uint8_t  m_meshPressureWarned             = 0;
    uint8_t  m_persistentBufferPressureWarned = 0;
    uint8_t  m_persistentMeshPressureWarned   = 0;
};
