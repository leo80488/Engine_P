#pragma once

// MeshManager - owns every mesh-side GPU resource the Renderer holds:
// - MeshDescriptorHeap (bindless buffer table + MeshDescriptor StructuredBuffer)
// - the built-in procedural primitives (PrimitiveMeshType)
// - the billboard quad shared by all billboard entities
// - per-library bindless VB/IB slots + the (library, meshId) -> descriptor-slot cache
// OnWorldClear() bumps a generation counter so every cached slot on a MeshLibRef becomes lazily
// invalid without walking the ECS.

#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/GraphicsStruct.h"
#include "Resource/ProceduralMesh.h"   // MeshData, PrimitiveMeshType
#include "ECS/HierarchyComponents.h"   // MeshLibRef

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;
namespace Resource { class MeshLibrary; }

class MeshManager
{
public:
    // Per-primitive GPU resources. Public because Renderer draws directly from
    // the primitive pool (m_meshMgr.GetPrimitive(type).meshDescSlot, etc.).
    struct GPUMesh
    {
        RHI::GPUBuffer posBuffer;
        RHI::GPUBuffer normalBuffer;
        RHI::GPUBuffer tangentBuffer;
        RHI::GPUBuffer uvBuffer;
        RHI::GPUBuffer colorBuffer;
        RHI::GPUBuffer indexBuffer;
        uint32_t meshDescSlot = RHI::kInvalidBufferIndex;
        uint32_t indexCount   = 0;
        uint32_t vertexCount  = 0; // uploaded vertex count (written into the MeshDescriptor)
        // Bindless slots of the de-interleaved streams (filled by UploadMesh). DDGISceneAS forwards them
        // to the RT trace shaders so ray hits on procedural primitives fetch real normals/UVs.
        uint32_t posBindlessIdx    = RHI::kInvalidBufferIndex;
        uint32_t normalBindlessIdx = RHI::kInvalidBufferIndex;
        uint32_t uvBindlessIdx     = RHI::kInvalidBufferIndex;
        uint32_t ibBindlessIdx     = RHI::kInvalidBufferIndex;
    };

    enum class MeshLibOwner : uint8_t { Scene = 0, Preview = 1 };

    // The descriptor-heap slots DetachMeshLib lifts out of one library, as indices; the caller holds
    // them until ReleaseMeshLibHandback frees them.
    struct MeshLibHandback
    {
        std::vector<uint32_t> meshDescSlots;
        uint32_t              vbSlot = RHI::kInvalidBufferIndex;
        uint32_t              ibSlot = RHI::kInvalidBufferIndex;
        // The library's meshlet buffer rides the same two steps as its VB/IB: detached now, freed with
        // them.
        uint32_t              meshletSlot = RHI::kInvalidBufferIndex;
        bool Empty() const
        {
            return meshDescSlots.empty() &&
                   vbSlot == RHI::kInvalidBufferIndex &&
                   ibSlot == RHI::kInvalidBufferIndex &&
                   meshletSlot == RHI::kInvalidBufferIndex;
        }
    };

    // Initialise the descriptor heap. Must be called once at startup.
    void Init(IGraphicsDevice& gfx);

    // Pre-generate + upload the built-in primitives. Called during Renderer::Compile().
    void InitPrimitives();

    // Upload the shared billboard quad (used by every BillboardComponent). Must
    // run after Init().
    void InitBillboardQuad();

    // Upload a ProceduralMesh::MeshData and register its buffers + MeshDescriptor; on success fills
    // @p out (slots, counts). @p persistent picks the slot range:
    //   true  -> MeshDescriptorHeap::RegisterPersistent*: engine lifetime, survives world reloads
    //            (Init-time uploads: primitives, billboard, anything cached forever).
    //   false -> world-scoped, invalidated by the next OnWorldClear (gameplay-time geometry that dies
    //            with the world).
    bool UploadMesh(const ProceduralMesh::MeshData& data, GPUMesh& out, bool persistent = false);

    // Register one mesh inside a MeshLibrary. The library's shared VB/IB register once into the
    // bindless table; each unique (lib, meshId) gets its own MeshDescriptor slot, cached for O(1)
    // lookups. The cache is shared by the preview layer and the live world, so a .meshlib the level
    // already holds is previewed without a second set of descriptors. @p owner only selects which
    // sharing counter moves (ReportMeshLibDescOwnership); it never changes what is registered.
    uint32_t RegisterMeshLibMesh(Resource::MeshLibrary& lib, const MeshLibRef& ref,
                                 MeshLibOwner owner = MeshLibOwner::Scene);

    // Logs one line at shutdown, `[MESH] meshlib desc cache: ...`; silent when no meshlib mesh was
    // ever registered. Fields:
    //   live          entries in the cache now (a world boundary clears it)
    //   previewFirst / sceneFirst  entries MINTED by each owner this session (inserts is their sum)
    //   dedupHits     cache hits on an already registered (lib, meshId)
    //   crossHits     hits by the owner that did NOT mint the entry: each is a slot sharing saved
    //   sameHits      hits by the minting owner
    //   refMemoHits   answered by MeshLibRef's own cached slot before the cache (never shared)
    // A pack that previews a library the scene also draws gates `sceneFirst >= 1 && crossHits >= 1`;
    // crossHits == 0 there is the regression.
    void ReportMeshLibDescOwnership() const;

    // Drops the meshlib caches and resets the heap's world-scoped slots. The generation bump makes
    // every MeshLibRef::cachedGeneration stale, so cached slots are ignored without walking the ECS.
    void OnWorldClear();

    // ---- Handing ONE mesh library back, in two steps ----
    // Releasing the library alone is not enough: m_meshLibDescCache and m_meshLibBindless are keyed on
    // the library's SLOT INDEX, which MeshLibrary recycles, so the next Load on that index would draw
    // the old geometry. One step is not enough either: MeshDescriptorHeap::FreeMesh writes a degenerate
    // (0-vertex) descriptor IMMEDIATELY (only the index is held kFrameCount frames), so a command list
    // already recorded this frame that names the slot would silently draw nothing.
    // So DetachMeshLib runs NOW, while the library is alive, and only lifts the state out (caches drop
    // their entries, the generation bumps, the slot indices go to the caller). The caller may Release
    // the library at once (so the next Load re-reads the file) and calls ReleaseMeshLibHandback with
    // the slots once every frame that could read them has retired.

    // Step 1, immediate. Drops every cache entry keyed on @p libHandle's slot index and reports the
    // descriptor-heap slots behind them in @p out.
    // Nothing in the heap is freed, but the bindless slots' resource REGISTRATION is dropped now
    // (MeshDescriptorHeap::DetachBufferSlot): ProcessAllDeferredReleases() (WaitForPreviousFrame,
    // Resize, the destructor) can free the VB/IB regardless of stamp while the hand-back still waits,
    // and a new resource on the recycled address would be pointer-dedup'd onto the dying slot.
    // Detaching touches no descriptor, only the CPU-side pointer -> slot map.
    // May run after the caller's Release: GraphicsDX12::DestroyBuffer only moves the VB/IB into the
    // deferred bucket, so their addresses cannot be recycled yet. That lets
    // Renderer::ReleasePreviewMeshLib decide "was mine the last reference?" and release in one step,
    // then detach. Needs no MeshLibrary: the slots are held as indices. The generation bump only
    // invalidates per-MeshLibRef cached slots; re-taking one is a lookup in the untouched cache.
    void DetachMeshLib(Resource::Handle libHandle, MeshLibHandback& out);

    // Step 2, deferred. Frees the slots DetachMeshLib collected in @p hb. Do not call it before every
    // frame that could have recorded them has retired (RHI::kFramesInFlight frames, the bound
    // GraphicsDX12::DrainReleasesUpTo uses). Works by slot index (MeshDescriptorHeap::FreeBufferSlot):
    // the library and its buffers are gone by then.
    void ReleaseMeshLibHandback(const MeshLibHandback& hb);

    MeshDescriptorHeap&       GetDescriptorHeap()        { return m_descHeap; }
    const MeshDescriptorHeap& GetDescriptorHeap() const  { return m_descHeap; }

    const GPUMesh& GetPrimitive(int idx) const           { return m_primitives[idx]; }

    uint32_t       GetBillboardMeshDescSlot() const      { return m_billboardMeshDescSlot; }
    const GPUMesh& GetBillboardQuad()         const      { return m_billboardQuad; }

    uint32_t       GetMeshLibDescGeneration() const      { return m_meshLibDescGeneration; }

    // A library's bindless VB/IB slots (populated by RegisterMeshLibMesh). Returns false, out args at
    // kInvalidBufferIndex, when the library is not registered yet. The DDGI scene AS uses it to feed
    // closest-hit's vertex-normal lookup.
    bool GetLibBindlessSlots(uint32_t libIdx, uint32_t& outVbIdx, uint32_t& outIbIdx) const
    {
        outVbIdx = RHI::kInvalidBufferIndex;
        outIbIdx = RHI::kInvalidBufferIndex;
        auto it = m_meshLibBindless.find(libIdx);
        if (it == m_meshLibBindless.end()) return false;
        outVbIdx = it->second.vbIdx;
        outIbIdx = it->second.ibIdx;
        return outVbIdx != RHI::kInvalidBufferIndex && outIbIdx != RHI::kInvalidBufferIndex;
    }
    // The library's meshlet buffer as a g_Buffers[] index (kInvalidBufferIndex:
    // not registered yet, or the library has no meshlets).
    uint32_t GetLibMeshletSlot(uint32_t libIdx) const
    {
        auto it = m_meshLibBindless.find(libIdx);
        return it == m_meshLibBindless.end() ? RHI::kInvalidBufferIndex : it->second.meshletIdx;
    }

private:
    struct MeshLibBindless
    {
        uint32_t vbIdx = RHI::kInvalidBufferIndex;
        uint32_t ibIdx = RHI::kInvalidBufferIndex;
        // The library's meshlet buffer, when it has one; registered with VB/IB, handed back with them.
        uint32_t meshletIdx = RHI::kInvalidBufferIndex;
    };

    IGraphicsDevice*    m_gfx = nullptr;
    MeshDescriptorHeap  m_descHeap;

    std::array<GPUMesh, static_cast<int>(PrimitiveMeshType::Count)> m_primitives;

    // (libSlotIndex << 32 | meshId) -> MeshDescriptor slot.
    std::unordered_map<uint64_t, uint32_t> m_meshLibDescCache;
    uint32_t m_meshLibDescGeneration = 1;

    // ---- Sharing metric (ReportMeshLibDescOwnership) ----
    // Which owner FIRST minted each live cache key, so a hit by the other owner counts as a slot
    // sharing saved. The map is cleared with m_meshLibDescCache (DetachMeshLib / OnWorldClear) because
    // it describes those entries; the counters below are per session.
    std::unordered_map<uint64_t, uint8_t> m_meshLibDescFirstOwner;
    uint32_t m_mlInsertsPreview = 0;   // entries the PREVIEW minted
    uint32_t m_mlInsertsScene   = 0;   // entries the SCENE minted
    uint32_t m_mlHitsSame       = 0;   // cache hit by the owner that minted it
    uint32_t m_mlHitsCross      = 0;   // cache hit by the OTHER owner == a saving
    uint32_t m_mlRefMemoHits    = 0;   // answered by MeshLibRef's own cached slot

    // Per-library bindless VB/IB slots (shared across every mesh in the lib).
    std::unordered_map<uint32_t, MeshLibBindless> m_meshLibBindless;

    uint32_t m_billboardMeshDescSlot = RHI::kInvalidBufferIndex;
    GPUMesh  m_billboardQuad;
};
