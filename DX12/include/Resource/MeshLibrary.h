#pragma once

#include "Resource/SystemHandles.h"
#include "Resource/MeshletData.h"   // MeshletEntryRange / MeshletLevel (the resident MLET table)
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;

namespace Resource
{
    struct MeshIdentity;   // Resource/MeshLibIdentity.h

    // MeshLibrary - runtime container for .meshlib files. One .meshlib holds N entries sharing one
    // GPU VB and one GPU IB; each entry has its own (vertexStart/Count, indexStart/Count, AABB).
    //
    // An ENTRY is one material SECTION (importers split a source object per material), and meshId
    // is its POSITION in the entry table - not a stable identity: a re-import that adds, removes
    // or reorders source meshes renumbers it. The stable names are the file's identity layer
    // (GetIdentity, Resource/MeshLibIdentity.h): keyed Sections, one per entry, grouped into
    // keyed, named Meshes.
    //
    // Lifetime: Load(path) uploads the shared VB+IB and caches the entry table; the last Release
    // frees the GPU buffers and bumps the slot generation, so every held handle goes stale.
    //
    // Thread safety: every call that reads or changes a slot takes m_mutex. PrepareFromFile (any
    // thread) and CreateCommitBuffers (a loader worker inside the device's loader upload scope)
    // touch no slot. An Entry pointer stays valid until the library's last Release: do not Release
    // a library while another thread is drawing it.
    class MeshLibrary
    {
    public:
        // One entry (material section) of the library, indexed by meshId. Mirrors AssetHeader's
        // MeshLibraryEntry with DirectXMath types.
        struct Entry
        {
            uint32_t          vertexStart        = 0;
            uint32_t          vertexCount        = 0;
            uint32_t          indexStart         = 0;
            uint32_t          indexCount         = 0;
            DirectX::XMFLOAT3 aabbMin            = { 0, 0, 0 };
            DirectX::XMFLOAT3 aabbMax            = { 0, 0, 0 };
            uint32_t          defaultMaterialIdx = 0xFFFFFFFFu;
            uint32_t          flags              = 0;
        };

        struct TotalsView
        {
            uint32_t vertexCount = 0;
            uint32_t indexCount  = 0;
        };

        using IdentityProvider = std::function<std::shared_ptr<const MeshIdentity>(
            const std::string& path, const uint8_t* blob, size_t size)>;

        // ---- Meshlets ----
        enum class MeshletState : uint8_t
        {
            None,           // the file carries no MLET
            Resident,       // validated and uploaded
            Dropped,        // the MLET did not validate against the geometry (warned at Load)
            UploadFailed,   // valid, but the GPU buffer could not be created
        };

        struct MeshletLibraryInfo
        {
            MeshletState state        = MeshletState::None;
            uint32_t     clusters     = 0;   // every entry, every level
            uint32_t     levelRecords = 0;
            uint32_t     maxLevels    = 0;   // the most levels one entry has
            uint32_t     gpuBytes     = 0;   // the resident buffer's size
        };

        struct MeshletEntryInfo
        {
            uint32_t firstCluster = 0;
            uint32_t clusterCount = 0;
            uint32_t levelCount   = 0;
            uint32_t level0Clusters = 0;
            uint32_t shadedLevelCount   = 0;
            uint32_t shadedClusterCount = 0;
        };

        struct MeshletLevelInfo
        {
            uint32_t clusters = 0, triangles = 0;
            float    selfCenter[3] = {}, selfRadius = 0, selfError = 0;
            float    parentCenter[3] = {}, parentRadius = 0, parentError = 0;
        };

        // ---- Load in two halves ----
        // Move-only: the views point into the owned blob.
        struct Prepared
        {
            std::string          path;
            uint64_t             pathHash = 0;
            bool                 ok = false;
            std::vector<uint8_t> blob;
            const uint8_t*       vertexData = nullptr;
            const uint32_t*      indexData  = nullptr;
            size_t               vertexBytes = 0, indexBytes = 0;
            uint32_t             meshCount = 0, vertexCount = 0, indexCount = 0, vertexStride = 0, flags = 0;
            bool                 hasTangent = false;
            std::shared_ptr<const MeshIdentity> identity;
            MeshletChunkView      mlet;
            MeshletState          mletState = MeshletState::None;
            std::vector<uint32_t> shadedLevels;
            std::vector<Entry>    entries;
            // CommitStep's progress: the buffers made so far (owned by the
            // Prepared until the slot takes them; AbandonCommit frees them).
            int                   commitStage = 0;
            RHI::GPUBuffer        vb, ib;
            RHI::GPUBuffer        meshletBuf;   // made on a loader worker (CreateCommitBuffers)

            Prepared() = default;
            Prepared(Prepared&&) = default;
            Prepared& operator=(Prepared&&) = default;
            Prepared(const Prepared&) = delete;
            Prepared& operator=(const Prepared&) = delete;
        };

        // ---- Lifetime, load, release and queries ----
        MeshLibrary()  = default;
        ~MeshLibrary() = default;

        MeshLibrary(const MeshLibrary&)            = delete;
        MeshLibrary& operator=(const MeshLibrary&) = delete;

        // Load a .meshlib and upload its shared VB/IB. Returns an opaque library handle
        // (ResourceType::MeshLibrary), or an invalid handle when the file is missing or malformed
        // (each failure kind logged once per path).
        // Path-deduplicated (MeshLibCanonicalPath spelling): an already-loaded file returns its slot
        // with the refcount bumped and no re-upload, so a same-world reload neither allocates a fresh
        // VB+IB nor misses the DDGI BLAS cache.
        Handle Load(const std::string& path, IGraphicsDevice& gfx);

        // Add one ownership reference to an already-loaded library without
        // doing path lookup or GPU upload. Used by Editor Play snapshots: the
        // live Play world may load several other scenes (and balance the
        // original world's Load refs), while the detached editor snapshot
        // still stores this handle. Returns false for an invalid/stale handle.
        // Every successful Retain must eventually be paired with Release.
        bool Retain(Handle libHandle);

        // Decrement refcount. When refcount reaches 0, free GPU buffers and
        // bump the slot generation so stale handles fail IsValid(). A no-op for
        // the null handle or a stale one. Each Load call must be paired
        // with exactly one Release call to balance refcount.
        void Release(Handle libHandle, IGraphicsDevice& gfx);

        // Release, AND the answer to "was MINE the reference that destroyed it?", decided and acted
        // on under ONE lock.
        // A caller that caches per-library GPU state derived from this one (MeshManager's
        // mesh-descriptor slots and the library-wide bindless VB/IB slots) must free that state ONLY
        // when its own Release destroys the library: the live World and the asset preview routinely
        // hold the SAME path (Load is path-deduplicated), and handing the shared descriptors back
        // would stop the scene drawing geometry it still owns. Do not ask RefCount()==1 and then call
        // Release: a Load or Retain in the gap makes "I am the last" false, in the direction that
        // frees descriptors a surviving holder still draws through.
        // @p outDestroyed is true exactly when THIS call freed the GPU buffers and bumped the
        // generation. Returns the refcount that REMAINS (0 when it destroyed): how many OTHER holders
        // are left. Invalid or stale handle: outDestroyed = false, returns 0.
        // Ordering for the derived-cache caller: hand the derived caches back AFTER this call.
        // IGraphicsDevice::DestroyBuffer only QUEUES the VB/IB into the deferred-release bucket (it
        // moves the ComPtr there), so on return the ID3D12Resource* is still alive and its address
        // cannot be recycled - what the detach half (MeshDescriptorHeap::DetachBufferSlot) needs.
        // Renderer::ReleasePreviewMeshLib shows the ordering.
        uint32_t ReleaseIfLast(Handle libHandle, IGraphicsDevice& gfx,
                               bool& outDestroyed);

        bool IsValid(Handle libHandle) const;

        // Outstanding ownership references (Load cache hits + Retain), 0 for an
        // invalid or stale handle. REPORTING ONLY - a holder deciding whether
        // to hand derived GPU state back must use ReleaseIfLast, which answers
        // the same question without a window between the answer and the act.
        uint32_t RefCount(Handle libHandle) const;

        // Entry count of this library; 0 for an invalid or stale handle.
        uint32_t GetMeshCount(Handle libHandle) const;

        // Per-entry metadata. nullptr for an invalid/stale handle or an out-of-range meshId. The
        // pointer is stable until the library's last Release.
        const Entry* GetEntry(Handle libHandle, uint32_t meshId) const;

        // Raw-byte library-wide pools: the Renderer registers one bindless SRV per library (not per
        // entry), since every entry shares the same VB/IB.
        const RHI::GPUBuffer* GetVertexBuffer(Handle libHandle) const;
        const RHI::GPUBuffer* GetIndexBuffer (Handle libHandle) const;

        // Library-wide vertex stride in bytes (32 base, plus the optional streams the MESHLIB_FLAG_*
        // bits add; see ComputeMeshLibVertexLayout), so the Renderer can fill
        // MeshDescriptor.byteStride without knowing the layout.
        uint32_t GetVertexStride(Handle libHandle) const;

        // Whether the interleaved VB carries a per-vertex tangent (float4 at byte offset 32). Drives
        // MeshManager's choice of binding the tangent stream; without one, GBuffer.vs synthesises the
        // basis from the normal alone.
        bool GetHasTangent(Handle libHandle) const;

        // Raw MeshLibraryMetadata::flags (MESHLIB_FLAG_*). MeshManager feeds this to
        // ComputeMeshLibVertexLayout() to derive the optional streams' byte offsets without
        // duplicating the layout rules.
        uint32_t GetMeshLibFlags(Handle libHandle) const;

        // Release every loaded library. Call before IGraphicsDevice::Shutdown.
        void Shutdown(IGraphicsDevice& gfx);

        // Every currently-alive library handle (the editor's library list); a copy, safe to use
        // after the lock is released.
        std::vector<Handle> GetLoadedHandles() const;

        // Library-wide totals (used by the editor's MeshLibrary panel).
        // Returns (vertexCount, indexCount) summed across every entry, or
        // zero if the library handle is invalid.
        TotalsView GetTotals(Handle libHandle) const;

        // The path given to the Load that created the slot (not canonicalised); empty for an
        // invalid / stale handle.
        std::string GetSourcePath(Handle libHandle) const;

        // The library's identity: named Meshes with stable keys, and one keyed Section per entry.
        // The file's own (the IDNT chunk of the .meshlib tail) when it carries one. Otherwise - a
        // tail-less file, or one whose tail is unreadable (warned at Load; the geometry still loads) -
        // the one the installed IdentityProvider synthesized at Load, flagged IDNT_FLAG_SYNTHESIZED:
        // key for key what the backfill tool would write. nullptr only for such a file loaded with no
        // provider installed (tools, headless probes).
        // Immutable once loaded, so a caller may keep the pointer past Release.
        std::shared_ptr<const MeshIdentity> GetIdentity(Handle libHandle) const;

        // Who names a library that carries no identity of its own. The Resource layer cannot: a
        // tail-less library's meshes are grouped by its model's .iscn. App installs
        // SceneInstanceLoader::SynthesizeMeshLibIdentity at startup. PrepareFromFile calls it for each
        // such file it reads, with the file's bytes (so the file is never read twice), on the
        // preparing thread - a preload's TaskSystem worker too, possibly for a library already loaded;
        // Load's dedup fast path reads nothing and does not call it. Install it before the first
        // Load; it is read without the lock.
        void SetIdentityProvider(IdentityProvider provider) { m_identityProvider = std::move(provider); }

        // The entry that carries @p sectionKey, or -1 (stale handle, no
        // identity in the file, or no such key).
        int FindEntryBySectionKey(Handle libHandle, uint64_t sectionKey) const;

        // ---- Meshlets ----
        static const char* MeshletStateName(MeshletState s);

        MeshletLibraryInfo GetMeshletInfo(Handle libHandle) const;
        // The resident meshlet buffer (raw SRV), or nullptr. The MLET chunk's GPU part is uploaded
        // as is: one raw DEFAULT-heap buffer per library, created at commit beside the VB/IB (or by
        // CreateCommitBuffers) and destroyed with them. A library without a valid MLET has no buffer
        // and draws through the vertex shader; the chunk is never built here.
        const RHI::GPUBuffer* GetMeshletBuffer(Handle libHandle) const;

        // One entry's clusters (all levels, level 0 first) and its levels.
        // False when the library has no resident meshlets or the entry none
        // (a skinned section, or one with no triangles).
        // The shaded main view draws only the leading `shadedLevelCount` levels
        // (`shadedClusterCount` clusters): its chain stops before the first
        // level that fails the LOD shading check (Resource/MeshletData.h,
        // MeshletDrawableLevels, run at Load). Shadow views draw every level.
        bool GetMeshletEntry(Handle libHandle, uint32_t meshId, MeshletEntryInfo& out) const;

        // One entry's LOD levels AS THE SHADED MAIN VIEW SEES THEM, finest first: each level's
        // clusters and triangles and the LOD values its clusters share (discrete LOD: every cluster
        // of a level carries the same spheres and errors, so the level's first cluster speaks for
        // all); a chain the shading check ended early has its last level as the root
        // (parentError = kMeshletRootError). False in the same cases as GetMeshletEntry.
        bool GetMeshletLevels(Handle libHandle, uint32_t meshId, std::vector<MeshletLevelInfo>& out) const;
        // Every resident meshlet buffer: how many libraries and how many bytes
        // (the `[MESHLET] resident` log line).
        void GetMeshletResidency(uint32_t& libraries, uint64_t& bytes) const;

        // ---- Load in two halves ----
        // PrepareFromFile reads, validates and parses a .meshlib (the file, its entry table, identity
        // and meshlet chunk) with no GPU work, on any thread (a preload's worker). Commit creates the
        // VB / IB / meshlet buffers and the slot on the thread that may create buffers with initial
        // data (main); it is path-deduplicated like Load (a library another caller loaded meanwhile
        // wins: refcount bumped, nothing created). Load is the dedup fast path, then both. A failed
        // Prepared commits to an invalid handle (the failure was logged once, as Load logs it).
        bool   PrepareFromFile(const std::string& path, Prepared& out);
        Handle Commit(Prepared& prep, IGraphicsDevice& gfx);
        // Commit in three calls (a preload's budgeted steps): the VB, the IB,
        // then the meshlet buffer and the slot. true = more to do; false = done
        // (@p out the library, or invalid on a failure). Each call is its own
        // upload. A library loaded meanwhile wins, as in Commit.
        bool   CommitStep(Prepared& prep, IGraphicsDevice& gfx, Handle& out);
        // A staged commit given up midway: its buffers go.
        void   AbandonCommit(Prepared& prep, IGraphicsDevice& gfx);
        // The VB, the IB and the meshlet buffer all made at once on a TaskSystem worker inside the
        // device's loader upload scope (IGraphicsDevice::BeginLoaderUploads), each placed in a heap
        // of its size, so the next CommitStep is the slot alone (Commit creates nothing a Prepared
        // brings). Touches no library state. false: the VB or the IB failed; Commit makes any missing
        // buffer again on main and reports.
        static bool CreateCommitBuffers(Prepared& prep, IGraphicsDevice& gfx);
        // The library loaded from @p path, with one more reference (Load's fast
        // path, without a read); invalid when none is.
        Handle AcquireLoaded(const std::string& path);

    private:
        struct Slot
        {
            std::vector<Entry> entries;
            RHI::GPUBuffer     vb;
            RHI::GPUBuffer     ib;
            std::string        sourcePath;
            uint32_t           vertexCountTotal = 0;
            uint32_t           indexCountTotal  = 0;
            uint32_t           vertexStride     = 32;
            uint32_t           generation       = 0;   // 0 = never issued
            bool               alive            = false;
            // Set when the metadata flags carry MESHLIB_FLAG_HAS_TANGENT: MeshManager then registers the
            // tangent stream in the MeshDescriptor (offset 32, format Float4); without it GBuffer.vs
            // synthesises the basis from the normal.
            bool               hasTangent       = false;
            // Raw MeshLibraryMetadata::flags (MESHLIB_FLAG_*). Drives the optional streams' offsets via
            // ComputeMeshLibVertexLayout.
            uint32_t           flags            = 0;
            // Path-dedup refcount: bumped by a Load cache hit and by Retain, decremented by Release; the
            // slot is freed when it reaches 0.
            uint32_t           refCount         = 0;
            // HashPath(sourcePath), kept on the slot for O(1) erase from m_pathHashToSlot in FreeSlot
            // (no re-hashing).
            uint64_t           pathHash         = 0;
            // The identity (GetIdentity): the parsed IDNT chunk, else the provider's synthesized one;
            // null without either.
            std::shared_ptr<const MeshIdentity> identity;
            // Meshlets (GetMeshletInfo): the MLET's GPU part and its CPU tables.
            RHI::GPUBuffer                  meshletBuf;
            std::vector<MeshletEntryRange>  meshletEntries;   // [entry]; empty = no meshlets
            std::vector<uint32_t>           meshletShadedLevels;   // [entry]: MeshletDrawableLevels
            std::vector<MeshletLevel>       meshletLevels;
            std::vector<MeshletLevelInfo>   meshletLevelInfo;  // [level record], parallel to meshletLevels
            MeshletState                    meshletState = MeshletState::None;
            uint32_t                        meshletClusters = 0;
        };

        // The `[MESHLET] resident` line after a library's meshlets came or went.
        // Caller holds m_mutex.
        void LogMeshletResidencyLocked(const char* event, const std::string& path) const;

        // Slot pool helpers; caller must hold m_mutex.
        uint32_t AllocSlot();
        void     FreeSlot(uint32_t index);
        bool     IsValidHandle(Handle h) const;

        // Resolve handle -> slot pointer; nullptr for an invalid handle. Caller must hold m_mutex.
        const Slot* GetSlot(Handle h) const;

        // FNV-1a 64 of MeshLibCanonicalPath(path): the dedup key (see the definition).
        static uint64_t HashPath(const std::string& path);

        IdentityProvider   m_identityProvider;
        mutable std::mutex m_mutex;
        std::vector<Slot>   m_slots;
        std::vector<uint32_t> m_freeList;
        std::unordered_map<uint64_t, uint32_t> m_pathHashToSlot; // path hash -> slot index
        // Per path hash: a bitmask of the Load failure branches already reported, so each kind of
        // failure logs its ERROR ONCE PER PATH instead of once per attempt.
        // Load is legitimately RETRIED: the asset preview does not memoise a failed handle, because
        // the failure is routinely transient (an importer rewriting the file, an asset still
        // copying). Without the latch that retry turns one missing .meshlib into an unbounded ERROR
        // stream, which every pack's -maxerrors budget reads as a failed run. One bit per branch, not
        // one bool per path, so a path that was MISSING and later turns up CORRUPT still reports the
        // second failure. Cleared for a path on every success path (the dedup fast path, the race
        // recheck, the commit) and wholesale by Shutdown.
        std::unordered_map<uint64_t, uint32_t> m_loadFailLogged;
    };
}
