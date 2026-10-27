#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/MeshLibrary.h"
#include "Resource/MeshLibIdentity.h"   // MeshLibCanonicalPath, the ONE canonical spelling
#include "Resource/MeshletData.h"   // the MLET chunk, validated at Load
#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "Resource/LoadCounters.h"
#include "Diagnostics/Profiler.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace Resource
{
    // ==== Slot pool helpers (caller must hold m_mutex) ====

    uint32_t MeshLibrary::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void MeshLibrary::FreeSlot(uint32_t index)
    {
        Slot& s = m_slots[index];
        if (s.pathHash != 0)
            m_pathHashToSlot.erase(s.pathHash);
        s.generation = (s.generation % Handle::MAX_GEN) + 1;  // never wraps to 0
        s.alive      = false;
        s.refCount   = 0;
        s.pathHash   = 0;
        s.sourcePath.clear();
        s.entries.clear();
        s.identity.reset();
        s.meshletBuf = RHI::GPUBuffer{};   // destroyed by the caller (ReleaseIfLast) before this
        s.meshletEntries.clear();
        s.meshletShadedLevels.clear();
        s.meshletLevels.clear();
        s.meshletLevelInfo.clear();
        s.meshletState    = MeshletState::None;
        s.meshletClusters = 0;
        m_freeList.push_back(index);
    }

    // CANONICALISED BEFORE HASHING, and NEVER before opening.
    // The dedup bucket answers "is this the same FILE", and on Windows several spellings of one
    // file reach here: std::filesystem's preferred separator ('.../Rossi\Rossi.meshlib') against
    // the preview's PreviewLibKey ('.../Rossi/Rossi.meshlib'), or a prefab .iscn's '../'
    // ('.../SchoolNight/Prefabs\../SchoolNight.meshlib'). A byte-exact hash makes those separate
    // libraries: the VB+IB uploaded twice and two refcounts that cannot see each other.
    // The canonical form is MeshLibCanonicalPath (lexically normal, THEN Sys::RelativizeAssetPath)
    // and nothing else - the function the preview keys its maps on and the identity layer names a
    // library by:
    //   - lexically normal first: RelativizeAssetPath alone keeps a '..';
    //   - NO case folding: the .ipak index is case-SENSITIVE (AssetFS's Normalize folds
    //     separators, a leading './' and dot segments, and stops there), so 'Rossi.meshlib' and
    //     'rossi.meshlib' are two packaged entries a case-folded hash would alias into one.
    // The string itself is not touched: slot.sourcePath keeps what the first successful caller
    // passed, the spelling AssetFS was handed.
    uint64_t MeshLibrary::HashPath(const std::string& path)
    {
        const std::string canonical = MeshLibCanonicalPath(path);
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : canonical)
        {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    bool MeshLibrary::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        const Slot& s = m_slots[idx];
        return s.alive && s.generation == h.Generation();
    }

    const MeshLibrary::Slot* MeshLibrary::GetSlot(Handle h) const
    {
        if (!IsValidHandle(h)) return nullptr;
        return &m_slots[h.Index()];
    }

    // ==== Load: parse the .meshlib blob, upload VB/IB, fill the entry table ====
    // On-disk layout (AssetHeader.h, MeshLibraryMetadata):
    //   [AssetHeader 24 B]
    //   [MeshLibraryMetadata 24 B]
    //   [MeshLibraryEntry x meshCount] - 48 B each
    //   [vertices: vertexCount x vertexStride]
    //   [indices:  indexCount x 4]
    //   [tail, when MESHLIB_FLAG_HAS_TAIL: chunk directory + IDNT identity / MLET meshlets]
    Handle MeshLibrary::Load(const std::string& path, IGraphicsDevice& gfx)
    {
        // Path-dedup fast path: an already-loaded file -> refcount bumped and the existing handle
        // returned, so a same-world reload re-uploads no VB/IB.
        if (const Handle loaded = AcquireLoaded(path); loaded.IsValid())
            return loaded;

        // Past the dedup fast path: this call reads the file (a runtime spawn's
        // census counts it as a cold library, Resource/LoadCounters.h).
        PROF_SCOPE("MeshLib.Load");
        ++LoadCounters::ThisThread().meshlibLoads;
        Prepared prep;
        if (!PrepareFromFile(path, prep)) return Handle{};
        return Commit(prep, gfx);
    }

    Handle MeshLibrary::AcquireLoaded(const std::string& path)
    {
        const uint64_t pathHash = HashPath(path);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pathHashToSlot.find(pathHash);
        if (it != m_pathHashToSlot.end() && it->second < m_slots.size())
        {
            Slot& s = m_slots[it->second];
            if (s.alive)
            {
                ++s.refCount;
                // The dedup fast path is a SUCCESS path, so it clears the path's failure latch too: most
                // loads of a previously failing path come through here (the world spawn loads it, the preview
                // finds it), and a latch left armed would silence the file's next failure.
                m_loadFailLogged.erase(pathHash);
                return Handle::Make(it->second,
                                    ResourceType::MeshLibrary,
                                    static_cast<uint16_t>(s.generation));
            }
        }
        return Handle{};
    }

    // The read-and-parse half of Load: no GPU, no slot - any thread.
    bool MeshLibrary::PrepareFromFile(const std::string& path, Prepared& out)
    {
        out = Prepared{};
        out.path = path;
        const uint64_t pathHash = HashPath(path);
        out.pathHash = pathHash;

        // ---- One ERROR per path PER FAILURE KIND, not per attempt ----
        // Every failure return below asks this first with its own branch id (one bit of
        // m_loadFailLogged[pathHash]; see the header): a path's first failure of each kind logs at
        // full volume, a retry that fails the same way says nothing. A missing file that later turns
        // up truncated or with a bad magic therefore still reports the corruption, while a
        // permanently missing file stays at one line. The bitmask keeps "this path is clean again" a
        // single erase on every success path.
        auto firstFailure = [&](uint32_t branch) -> bool
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint32_t&      mask = m_loadFailLogged[pathHash];
            const uint32_t bit  = 1u << branch;
            if (mask & bit) return false;
            mask |= bit;
            return true;
        };

        PROF_SCOPE("MeshLib.Prepare");
        std::vector<uint8_t>& blob = out.blob;
        if (!::Resource::AssetFS::Get().ReadFile(path, blob))
        {
            if (firstFailure(0)) LOG_ERROR("MeshLibrary::Load: cannot open '%s'", path.c_str());
            return false;
        }
        const std::streamoff size = static_cast<std::streamoff>(blob.size());
        if (size <= static_cast<std::streamoff>(sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)))
        {
            if (firstFailure(1)) LOG_ERROR("MeshLibrary::Load: '%s' too small (%lld B)", path.c_str(),
                      static_cast<long long>(size));
            return false;
        }
        if (!ValidateHeader(blob.data(), blob.size(), MAGIC_MESHLIB))
        {
            if (firstFailure(2)) LOG_ERROR("MeshLibrary::Load: bad magic/version in '%s'", path.c_str());
            return false;
        }

        const MeshLibraryMetadata* meta = GetMetadata<MeshLibraryMetadata>(blob.data());
        const uint8_t*             payload = GetPayload(blob.data());

        const uint32_t meshCount   = meta->meshCount;
        const uint32_t vertexCount = meta->vertexCount;
        const uint32_t indexCount  = meta->indexCount;
        const uint32_t vStride     = meta->vertexStride;
        const uint32_t iStride     = meta->indexStride;

        if (meshCount == 0 || vertexCount == 0 || indexCount == 0)
        {
            if (firstFailure(3)) LOG_ERROR("MeshLibrary::Load: '%s' empty (meshes=%u verts=%u idx=%u)",
                      path.c_str(), meshCount, vertexCount, indexCount);
            return false;
        }
        // Vertex layout is fully described by meta->flags through ComputeMeshLibVertexLayout()
        // (pos+nrm+uv0 base 32 B, then the optional tangent / uv1 / color / uv2 / uv3 streams in that
        // order). The on-disk stride MUST equal the flag-derived stride; a mismatch means a corrupted
        // or mixed-version asset, and the load is refused.
        const bool flagHasTangent = (meta->flags & MESHLIB_FLAG_HAS_TANGENT) != 0u;
        const uint32_t expectedStride =
            ComputeMeshLibVertexLayout(meta->flags).stride;
        if (vStride != expectedStride)
        {
            if (firstFailure(4)) LOG_ERROR("MeshLibrary::Load: '%s' inconsistent layout - vertexStride=%u "
                      "but flags=0x%X imply stride=%u",
                      path.c_str(), vStride, meta->flags, expectedStride);
            return false;
        }
        if (iStride != 4)
        {
            if (firstFailure(5)) LOG_ERROR("MeshLibrary::Load: '%s' indexStride=%u (only 4 supported)",
                      path.c_str(), iStride);
            return false;
        }

        // Bounds-check the payload: the entry table, vertex blob and index blob must fit inside the
        // loaded bytes, or the parse below would read past the blob.
        const size_t entryBytes  = size_t(meshCount) * sizeof(MeshLibraryEntry);
        const size_t vertexBytes = size_t(vertexCount) * vStride;
        const size_t indexBytes  = size_t(indexCount)  * iStride;
        const uint8_t* blobEnd   = blob.data() + blob.size();
        if (payload + entryBytes + vertexBytes + indexBytes > blobEnd)
        {
            if (firstFailure(6)) LOG_ERROR("MeshLibrary::Load: '%s' truncated (need %zu B, have %zu B past payload)",
                      path.c_str(),
                      entryBytes + vertexBytes + indexBytes,
                      static_cast<size_t>(blobEnd - payload));
            return false;
        }

        const MeshLibraryEntry* entryTable =
            reinterpret_cast<const MeshLibraryEntry*>(payload);
        const uint8_t*          vertexData = payload + entryBytes;
        const uint32_t*         indexData  =
            reinterpret_cast<const uint32_t*>(vertexData + vertexBytes);

        // ---- Identity layer ----
        // A file without a tail is normal and says nothing: it gets the identity the provider
        // synthesizes (the one the backfill would write). A tail that does not parse costs the file's
        // own keys, never the geometry; the synthesized identity stands in. Not latched like the
        // failures above: a successful load is cached, so the warning cannot repeat per frame.
        std::shared_ptr<const MeshIdentity> identity;
        {
            auto        parsed = std::make_shared<MeshIdentity>();
            std::string why;
            switch (ParseMeshLibIdentity(blob.data(), blob.size(), *parsed, &why))
            {
            case MeshIdentityParse::Ok:
                identity = std::move(parsed);
                break;
            case MeshIdentityParse::Malformed:
                LOG_WARNING("MeshLibrary::Load: '%s' has an unreadable identity tail (%s) - "
                            "geometry loaded, its own mesh/section keys are lost; the legacy "
                            "identity stands in", path.c_str(), why.c_str());
                break;
            case MeshIdentityParse::NoTail:
            case MeshIdentityParse::NoIdentity:
                break;
            }
            if (!identity && m_identityProvider)
                identity = m_identityProvider(path, blob.data(), blob.size());
        }

        // ---- Meshlets ----
        // Built offline and stored in the MLET chunk; never built here. A chunk that does not
        // validate against THIS geometry costs the meshlets (the library draws through the vertex
        // shader), never the file. Like the identity warning, it cannot repeat per frame: a
        // successful load is cached.
        MeshletChunkView mlet;
        MeshletState     mletState = MeshletState::None;
        {
            std::string             why;
            const MeshletChunkState ms = ReadMeshletChunk(blob.data(), blob.size(), mlet, &why);
            if (ms == MeshletChunkState::Malformed)
            {
                mletState = MeshletState::Dropped;
                LOG_WARNING("[MESHLET] load path='%s' dropped: %s - drawn through the vertex shader; "
                            "rebuild it: python tools/asset_tool.py meshlets --apply <file>",
                            path.c_str(), why.c_str());
            }
            else if (ms == MeshletChunkState::Ok)
            {
                mletState = MeshletState::Resident;
                LOG_INFO("[MESHLET] load path='%s' clusters=%u levels=%u vertexRefs=%u triangles=%u gpuBytes=%u",
                         path.c_str(), mlet.header.clusterCount, mlet.header.levelCount,
                         mlet.header.vertexRefCount, mlet.header.triangleCount, mlet.header.gpuByteSize);
            }
        }

        // The LOD shading check is CPU work over the vertices: done here too.
        std::vector<uint32_t> shadedLevels;
        if (mletState == MeshletState::Resident)
            shadedLevels = MeshletDrawableLevels(mlet, vertexData, vStride);

        // ---- Build runtime entry table ----
        std::vector<Entry> entries(meshCount);
        for (uint32_t i = 0; i < meshCount; ++i)
        {
            const MeshLibraryEntry& src = entryTable[i];
            Entry& dst = entries[i];
            dst.vertexStart        = src.vertexStart;
            dst.vertexCount        = src.vertexCount;
            dst.indexStart         = src.indexStart;
            dst.indexCount         = src.indexCount;
            dst.aabbMin            = { src.aabbMin[0], src.aabbMin[1], src.aabbMin[2] };
            dst.aabbMax            = { src.aabbMax[0], src.aabbMax[1], src.aabbMax[2] };
            dst.defaultMaterialIdx = src.defaultMaterialIdx;
            dst.flags              = src.flags;
        }

        out.vertexData   = vertexData;
        out.indexData    = indexData;
        out.vertexBytes  = vertexBytes;
        out.indexBytes   = indexBytes;
        out.meshCount    = meshCount;
        out.vertexCount  = vertexCount;
        out.indexCount   = indexCount;
        out.vertexStride = vStride;
        out.flags        = meta->flags;
        out.hasTangent   = flagHasTangent;
        out.identity     = std::move(identity);
        out.mlet         = mlet;
        out.mletState    = mletState;
        out.shadedLevels = std::move(shadedLevels);
        out.entries      = std::move(entries);
        out.ok           = true;
        return true;
    }

    bool MeshLibrary::CommitStep(Prepared& prep, IGraphicsDevice& gfx, Handle& out)
    {
        out = Handle{};
        if (!prep.ok) return false;
        // Loaded meanwhile: that one wins, ours go.
        if (const Handle loaded = AcquireLoaded(prep.path); loaded.IsValid())
        {
            AbandonCommit(prep, gfx);
            out = loaded;
            return false;
        }
        auto aligned = [](uint64_t b) { return (b + 0xFFFFull) & ~0xFFFFull; };
        auto oneBuffer = [&](RHI::GPUBuffer& dst, uint64_t size, uint32_t stride, RHI::BindFlag bind,
                             const void* data) -> bool
        {
            RHI::GPUBufferDesc d;
            d.size       = size;
            d.stride     = stride;
            d.usage      = RHI::Usage::DEFAULT;
            d.bindFlags = bind;
            d.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
            gfx.BeginBufferHeapBatch(aligned(size));
            gfx.BeginBufferUploadBatch();
            const bool ok = gfx.CreateBuffer(d, dst, data);
            gfx.EndBufferUploadBatch();
            gfx.EndBufferHeapBatch();
            return ok;
        };
        if (prep.commitStage == 0)
        {
            PROF_SCOPE("MeshLib.CommitVB");
            prep.commitStage = 1;
            if (!oneBuffer(prep.vb, prep.vertexBytes, 0, RHI::BindFlag::SHADER_RESOURCE, prep.vertexData))
                prep.vb = RHI::GPUBuffer{};   // Commit retries it and reports the failure
            return true;
        }
        if (prep.commitStage == 1)
        {
            PROF_SCOPE("MeshLib.CommitIB");
            prep.commitStage = 2;
            if (!oneBuffer(prep.ib, prep.indexBytes, sizeof(uint32_t),
                           RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE, prep.indexData))
                prep.ib = RHI::GPUBuffer{};
            return true;
        }
        // The meshlets and the slot: Commit, with the two buffers made.
        prep.commitStage = 3;
        const bool meshlets = prep.mletState == MeshletState::Resident;
        gfx.BeginBufferHeapBatch(meshlets ? aligned(prep.mlet.header.gpuByteSize) : 0u);
        gfx.BeginBufferUploadBatch();
        out = Commit(prep, gfx);
        gfx.EndBufferUploadBatch();
        gfx.EndBufferHeapBatch();
        return false;
    }

    void MeshLibrary::AbandonCommit(Prepared& prep, IGraphicsDevice& gfx)
    {
        if (prep.vb.IsValid()) gfx.DestroyBuffer(prep.vb);
        if (prep.ib.IsValid()) gfx.DestroyBuffer(prep.ib);
        if (prep.meshletBuf.IsValid()) gfx.DestroyBuffer(prep.meshletBuf);
        prep.vb         = RHI::GPUBuffer{};
        prep.ib         = RHI::GPUBuffer{};
        prep.meshletBuf = RHI::GPUBuffer{};
    }

    bool MeshLibrary::CreateCommitBuffers(Prepared& prep, IGraphicsDevice& gfx)
    {
        if (!prep.ok) return false;
        PROF_SCOPE("MeshLib.CreateOnLoader");
        auto aligned = [](uint64_t b) { return (b + 0xFFFFull) & ~0xFFFFull; };
        auto make = [&](RHI::GPUBuffer& dst, uint64_t size, uint32_t stride, RHI::BindFlag bind,
                        const void* data) -> bool
        {
            if (dst.IsValid()) return true;
            RHI::GPUBufferDesc d;
            d.size       = size;
            d.stride     = stride;
            d.usage      = RHI::Usage::DEFAULT;
            d.bindFlags = bind;
            d.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
            gfx.BeginBufferHeapBatch(aligned(size));
            const bool ok = gfx.CreateBuffer(d, dst, data);
            gfx.EndBufferHeapBatch();
            if (!ok) dst = RHI::GPUBuffer{};
            return ok;
        };
        bool ok = make(prep.vb, prep.vertexBytes, 0, RHI::BindFlag::SHADER_RESOURCE, prep.vertexData);
        ok = make(prep.ib, prep.indexBytes, sizeof(uint32_t),
                  RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE, prep.indexData) && ok;
        if (prep.mletState == MeshletState::Resident)
            (void)make(prep.meshletBuf, prep.mlet.header.gpuByteSize, 0, RHI::BindFlag::SHADER_RESOURCE, prep.mlet.gpu);
        prep.commitStage = 2;   // CommitStep's next is the slot (Commit)
        return ok;
    }

    // The GPU half of Load: buffers with initial data and the slot (main).
    Handle MeshLibrary::Commit(Prepared& prep, IGraphicsDevice& gfx)
    {
        if (!prep.ok) return Handle{};
        PROF_SCOPE("MeshLib.Commit");
        const std::string&    path        = prep.path;
        const uint64_t        pathHash    = prep.pathHash;
        const size_t          vertexBytes = prep.vertexBytes;
        const size_t          indexBytes  = prep.indexBytes;
        const uint8_t*        vertexData  = prep.vertexData;
        const uint32_t*       indexData   = prep.indexData;
        const uint32_t        meshCount   = prep.meshCount;
        const uint32_t        vertexCount = prep.vertexCount;
        const uint32_t        indexCount  = prep.indexCount;
        const MeshletChunkView& mlet      = prep.mlet;
        MeshletState          mletState   = prep.mletState;
        auto firstFailure = [&](uint32_t branch) -> bool
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            uint32_t&      mask = m_loadFailLogged[pathHash];
            const uint32_t bit  = 1u << branch;
            if (mask & bit) return false;
            mask |= bit;
            return true;
        };

        // A library another caller committed meanwhile wins, as in the race recheck below - checked
        // first so no buffer is created (a staged commit's buffers are abandoned).
        if (const Handle loaded = AcquireLoaded(path); loaded.IsValid())
        {
            AbandonCommit(prep, gfx);
            return loaded;
        }

        // ---- Upload GPU buffers ----
        RHI::GPUBufferDesc vbDesc;
        vbDesc.size       = vertexBytes;
        vbDesc.stride     = 0;
        vbDesc.usage      = RHI::Usage::DEFAULT;
        vbDesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        vbDesc.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;

        RHI::GPUBufferDesc ibDesc;
        ibDesc.size       = indexBytes;
        ibDesc.stride     = sizeof(uint32_t);
        ibDesc.usage      = RHI::Usage::DEFAULT;
        ibDesc.bindFlags = RHI::BindFlag::INDEX_BUFFER | RHI::BindFlag::SHADER_RESOURCE;
        ibDesc.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;

        // A staged commit (CommitStep) brings them made already - the meshlet buffer too when a
        // loader worker made them (CreateCommitBuffers).
        RHI::GPUBuffer vb = prep.vb, ib = prep.ib;
        RHI::GPUBuffer meshletBuf = prep.meshletBuf;
        prep.vb         = RHI::GPUBuffer{};
        prep.ib         = RHI::GPUBuffer{};
        prep.meshletBuf = RHI::GPUBuffer{};
        if (!vb.IsValid() && !gfx.CreateBuffer(vbDesc, vb, vertexData))
        {
            if (firstFailure(7)) LOG_ERROR("MeshLibrary::Load: VB upload failed (%s, %zu B)",
                      path.c_str(), vertexBytes);
            if (ib.IsValid()) gfx.DestroyBuffer(ib);
            if (meshletBuf.IsValid()) gfx.DestroyBuffer(meshletBuf);
            return Handle{};
        }
        if (!ib.IsValid() && !gfx.CreateBuffer(ibDesc, ib, indexData))
        {
            if (firstFailure(8)) LOG_ERROR("MeshLibrary::Load: IB upload failed (%s, %zu B)",
                      path.c_str(), indexBytes);
            gfx.DestroyBuffer(vb);
            if (meshletBuf.IsValid()) gfx.DestroyBuffer(meshletBuf);
            return Handle{};
        }

        // ---- The LOD shading check (Resource/MeshletData.h) ----
        // A coarse level can shade its triangles with other faces' normals - black triangles that
        // show only from far enough away for that level to be picked. The SHADED main view stops each
        // entry's chain before the first such level (SceneMeshlet.as reads the cutoff from the
        // sidecar, MeshManager); the shadow views keep every level: a depth-only caster needs its
        // geometry within the error bound, not its normals. PrepareFromFile computed the levels; this
        // only reports the capped entries.
        std::vector<uint32_t>& shadedLevels = prep.shadedLevels;
        if (mletState == MeshletState::Resident)
        {
            uint32_t cappedEntries = 0, shadowOnlyLevels = 0;
            for (uint32_t e = 0; e < mlet.header.entryCount; ++e)
            {
                const uint32_t levels = mlet.Entry(e).levelCount;
                if (shadedLevels[e] >= levels) continue;
                ++cappedEntries;
                shadowOnlyLevels += levels - shadedLevels[e];
            }
            if (cappedEntries)
                LOG_INFO("[MESHLET] load path='%s' shadedLodCapped entries=%u shadowOnlyLevels=%u - those levels "
                         "shade with other faces' normals: the shaded view stops before them, shadows keep them",
                         path.c_str(), cappedEntries, shadowOnlyLevels);
        }

        // ---- The meshlets' GPU part ----
        // Created after the IB, in the same upload batch as VB/IB (CreateBuffer defers the copy),
        // unless a staged commit brought it. A failure costs the meshlets, not the library.
        std::vector<MeshletEntryRange> meshletEntries;
        std::vector<MeshletLevel>      meshletLevels;
        std::vector<MeshletLevelInfo>  meshletLevelInfo;
        if (mletState == MeshletState::Resident)
        {
            RHI::GPUBufferDesc mbDesc;
            mbDesc.size       = mlet.header.gpuByteSize;
            mbDesc.stride     = 0;
            mbDesc.usage      = RHI::Usage::DEFAULT;
            mbDesc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
            mbDesc.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
            if (meshletBuf.IsValid() || gfx.CreateBuffer(mbDesc, meshletBuf, mlet.gpu))
            {
                meshletEntries.resize(mlet.header.entryCount);
                for (uint32_t e = 0; e < mlet.header.entryCount; ++e) meshletEntries[e] = mlet.Entry(e);
                meshletLevels.resize(mlet.header.levelCount);
                meshletLevelInfo.resize(mlet.header.levelCount);
                for (uint32_t l = 0; l < mlet.header.levelCount; ++l)
                {
                    meshletLevels[l] = mlet.Level(l);
                    MeshletLevelInfo& li = meshletLevelInfo[l];
                    li.clusters = meshletLevels[l].clusterCount;
                    for (uint32_t c = 0; c < meshletLevels[l].clusterCount; ++c)
                        li.triangles += MeshletTriangleCount(mlet.Cluster(meshletLevels[l].firstCluster + c));
                    if (meshletLevels[l].clusterCount)
                    {
                        const MeshletDesc d = mlet.Cluster(meshletLevels[l].firstCluster);
                        for (int a = 0; a < 3; ++a) { li.selfCenter[a] = d.lodSelfCenter[a]; li.parentCenter[a] = d.lodParentCenter[a]; }
                        li.selfRadius   = d.lodSelfRadius;
                        li.parentRadius = d.lodParentRadius;
                        li.selfError    = d.lodSelfError;
                        li.parentError  = d.lodParentError;
                    }
                }
            }
            else
            {
                mletState = MeshletState::UploadFailed;
                LOG_WARNING("[MESHLET] load path='%s': the meshlet buffer (%u B) could not be created - "
                            "drawn through the vertex shader", path.c_str(), mlet.header.gpuByteSize);
            }
        }

        // ---- Commit slot ----
        std::lock_guard<std::mutex> lock(m_mutex);

        // Race recheck: another thread may have committed the same path while this one worked
        // outside the lock. If so, drop our buffers and return the winner's handle (refcount++).
        if (auto it = m_pathHashToSlot.find(pathHash);
            it != m_pathHashToSlot.end() && it->second < m_slots.size())
        {
            Slot& winner = m_slots[it->second];
            if (winner.alive)
            {
                ++winner.refCount;
                gfx.DestroyBuffer(vb);
                gfx.DestroyBuffer(ib);
                if (meshletBuf.IsValid()) gfx.DestroyBuffer(meshletBuf);
                // A success, so the path's failure latch goes, as on the other success paths: reached when a
                // path that HAD been failing is loaded by two threads at once - the usual shape of "the
                // importer finished writing it".
                m_loadFailLogged.erase(pathHash);
                return Handle::Make(it->second,
                                    ResourceType::MeshLibrary,
                                    static_cast<uint16_t>(winner.generation));
            }
        }

        const uint32_t idx = AllocSlot();
        Slot& slot = m_slots[idx];
        if (slot.generation == 0) slot.generation = 1;
        slot.entries          = std::move(prep.entries);
        slot.vb               = std::move(vb);
        slot.ib               = std::move(ib);
        slot.sourcePath       = path;
        slot.vertexCountTotal = vertexCount;
        slot.indexCountTotal  = indexCount;
        slot.vertexStride     = prep.vertexStride;
        slot.hasTangent       = prep.hasTangent;
        slot.flags            = prep.flags;
        slot.alive            = true;
        slot.refCount         = 1;
        slot.pathHash         = pathHash;
        slot.identity         = std::move(prep.identity);
        slot.meshletBuf       = std::move(meshletBuf);
        slot.meshletEntries   = std::move(meshletEntries);
        slot.meshletShadedLevels = slot.meshletEntries.empty() ? std::vector<uint32_t>{} : std::move(shadedLevels);
        slot.meshletLevels    = std::move(meshletLevels);
        slot.meshletLevelInfo = std::move(meshletLevelInfo);
        slot.meshletState     = mletState;
        slot.meshletClusters  = slot.meshletBuf.IsValid() ? mlet.header.clusterCount : 0u;
        m_pathHashToSlot[pathHash] = idx;
        // The path works now, so the next failure on it is news again.
        m_loadFailLogged.erase(pathHash);
        if (slot.meshletBuf.IsValid()) LogMeshletResidencyLocked("load", path);

        LOG_SUCCESS("MeshLibrary: loaded '%s' (%u meshes, %u verts, %u idx, %.1f MB)",
                    path.c_str(), meshCount, vertexCount, indexCount,
                    double(vertexBytes + indexBytes) / (1024.0 * 1024.0));

        return Handle::Make(idx, ResourceType::MeshLibrary, slot.generation);
    }

    // ==== Retain / Release ====

    bool MeshLibrary::Retain(Handle libHandle)
    {
        if (!libHandle.IsValid() ||
            libHandle.Type() != ResourceType::MeshLibrary)
            return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(libHandle)) return false;

        ++m_slots[libHandle.Index()].refCount;
        return true;
    }

    void MeshLibrary::Release(Handle libHandle, IGraphicsDevice& gfx)
    {
        bool destroyed = false;
        (void)ReleaseIfLast(libHandle, gfx, destroyed);
    }

    // ONE LOCK FOR THE DECISION AND THE ACT (see ReleaseIfLast in the header).
    uint32_t MeshLibrary::ReleaseIfLast(Handle libHandle, IGraphicsDevice& gfx,
                                        bool& outDestroyed)
    {
        outDestroyed = false;
        if (!libHandle.IsValid()) return 0u;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(libHandle)) return 0u;

        Slot& slot = m_slots[libHandle.Index()];
        if (slot.refCount == 0) return 0u;         // defensive: imbalanced Release
        if (--slot.refCount > 0)
            return slot.refCount;                  // still referenced elsewhere

        // DestroyBuffer only QUEUES these into the device's deferred bucket, so the resources outlive
        // this call and the caller may still detach the bindless registrations that name them (see
        // ReleaseIfLast in the header), the meshlet buffer's included (MeshManager::DetachMeshLib
        // hands its slot back).
        if (slot.vb.IsValid()) gfx.DestroyBuffer(slot.vb);
        if (slot.ib.IsValid()) gfx.DestroyBuffer(slot.ib);
        const bool hadMeshlets = slot.meshletBuf.IsValid();
        const std::string path = hadMeshlets ? slot.sourcePath : std::string();
        if (hadMeshlets) gfx.DestroyBuffer(slot.meshletBuf);
        FreeSlot(libHandle.Index());
        if (hadMeshlets) LogMeshletResidencyLocked("release", path);
        outDestroyed = true;
        return 0u;
    }

    // ==== Query accessors ====

    bool MeshLibrary::IsValid(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return IsValidHandle(libHandle);
    }

    uint32_t MeshLibrary::RefCount(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->refCount : 0u;
    }

    uint32_t MeshLibrary::GetMeshCount(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? static_cast<uint32_t>(s->entries.size()) : 0u;
    }

    const MeshLibrary::Entry* MeshLibrary::GetEntry(Handle libHandle, uint32_t meshId) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s || meshId >= s->entries.size()) return nullptr;
        return &s->entries[meshId];
    }

    const RHI::GPUBuffer* MeshLibrary::GetVertexBuffer(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return (s && s->vb.IsValid()) ? &s->vb : nullptr;
    }

    const RHI::GPUBuffer* MeshLibrary::GetIndexBuffer(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return (s && s->ib.IsValid()) ? &s->ib : nullptr;
    }

    uint32_t MeshLibrary::GetVertexStride(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->vertexStride : 0u;
    }

    bool MeshLibrary::GetHasTangent(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->hasTangent : false;
    }

    uint32_t MeshLibrary::GetMeshLibFlags(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->flags : 0u;
    }

    // ==== Editor enumeration helpers ====

    std::vector<Handle> MeshLibrary::GetLoadedHandles() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Handle> out;
        out.reserve(m_slots.size());
        for (uint32_t i = 0; i < m_slots.size(); ++i)
        {
            const Slot& s = m_slots[i];
            if (!s.alive) continue;
            out.push_back(Handle::Make(i, ResourceType::MeshLibrary, s.generation));
        }
        return out;
    }

    MeshLibrary::TotalsView MeshLibrary::GetTotals(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s) return {};
        return { s->vertexCountTotal, s->indexCountTotal };
    }

    std::string MeshLibrary::GetSourcePath(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->sourcePath : std::string{};
    }

    std::shared_ptr<const MeshIdentity> MeshLibrary::GetIdentity(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return s ? s->identity : nullptr;
    }

    int MeshLibrary::FindEntryBySectionKey(Handle libHandle, uint64_t sectionKey) const
    {
        const std::shared_ptr<const MeshIdentity> id = GetIdentity(libHandle);
        return id ? id->FindSection(sectionKey) : -1;
    }

    // ==== Meshlets ====

    const char* MeshLibrary::MeshletStateName(MeshletState s)
    {
        switch (s)
        {
        case MeshletState::Resident:     return "resident";
        case MeshletState::Dropped:      return "dropped";
        case MeshletState::UploadFailed: return "upload_failed";
        default:                         return "none";
        }
    }

    MeshLibrary::MeshletLibraryInfo MeshLibrary::GetMeshletInfo(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s) return {};
        MeshletLibraryInfo i;
        i.state        = s->meshletState;
        i.clusters     = s->meshletClusters;
        i.levelRecords = static_cast<uint32_t>(s->meshletLevels.size());
        i.gpuBytes     = s->meshletBuf.IsValid() ? static_cast<uint32_t>(s->meshletBuf.desc.size) : 0u;
        for (const MeshletEntryRange& r : s->meshletEntries) i.maxLevels = std::max(i.maxLevels, r.levelCount);
        return i;
    }

    const RHI::GPUBuffer* MeshLibrary::GetMeshletBuffer(Handle libHandle) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        return (s && s->meshletBuf.IsValid()) ? &s->meshletBuf : nullptr;
    }

    bool MeshLibrary::GetMeshletLevels(Handle libHandle, uint32_t meshId, std::vector<MeshletLevelInfo>& out) const
    {
        out.clear();
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s || !s->meshletBuf.IsValid() || meshId >= s->meshletEntries.size()) return false;
        const MeshletEntryRange& r = s->meshletEntries[meshId];
        if (r.levelCount == 0 || r.firstLevel + r.levelCount > s->meshletLevelInfo.size()) return false;
        const uint32_t shaded = meshId < s->meshletShadedLevels.size()
            ? std::min(std::max(s->meshletShadedLevels[meshId], 1u), r.levelCount) : r.levelCount;
        out.assign(s->meshletLevelInfo.begin() + r.firstLevel, s->meshletLevelInfo.begin() + r.firstLevel + shaded);
        // The shaded view's chain ends here: SceneMeshlet.as treats its last
        // level as the root.
        if (shaded < r.levelCount) out.back().parentError = kMeshletRootError;
        return true;
    }

    bool MeshLibrary::GetMeshletEntry(Handle libHandle, uint32_t meshId, MeshletEntryInfo& out) const
    {
        out = MeshletEntryInfo{};
        std::lock_guard<std::mutex> lock(m_mutex);
        const Slot* s = GetSlot(libHandle);
        if (!s || !s->meshletBuf.IsValid() || meshId >= s->meshletEntries.size()) return false;
        const MeshletEntryRange& r = s->meshletEntries[meshId];
        if (r.levelCount == 0 || r.clusterCount == 0) return false;
        out.firstCluster   = r.firstCluster;
        out.clusterCount   = r.clusterCount;
        out.levelCount     = r.levelCount;
        out.level0Clusters = r.firstLevel < s->meshletLevels.size() ? s->meshletLevels[r.firstLevel].clusterCount
                                                                    : r.clusterCount;
        // The shaded view: the levels before the first that fails the shading
        // check, and the clusters they span (levels are contiguous, finest first).
        out.shadedLevelCount   = r.levelCount;
        out.shadedClusterCount = r.clusterCount;
        if (meshId < s->meshletShadedLevels.size())
        {
            const uint32_t shaded = std::min(std::max(s->meshletShadedLevels[meshId], 1u), r.levelCount);
            const uint32_t last   = r.firstLevel + shaded - 1;
            if (shaded < r.levelCount && last < s->meshletLevels.size())
            {
                out.shadedLevelCount   = shaded;
                out.shadedClusterCount = s->meshletLevels[last].firstCluster + s->meshletLevels[last].clusterCount -
                                         r.firstCluster;
            }
        }
        return true;
    }

    void MeshLibrary::GetMeshletResidency(uint32_t& libraries, uint64_t& bytes) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        libraries = 0;
        bytes     = 0;
        for (const Slot& s : m_slots)
            if (s.alive && s.meshletBuf.IsValid())
            {
                ++libraries;
                bytes += s.meshletBuf.desc.size;
            }
    }

    // The residency pack's evidence (meshlet_residency): a reload of the same
    // world must leave these numbers where they were, a world switch must bring
    // them down. One line per library whose meshlets came or went - never per
    // frame, since Load caches and ReleaseIfLast runs once per library.
    void MeshLibrary::LogMeshletResidencyLocked(const char* event, const std::string& path) const
    {
        uint32_t libraries = 0;
        uint64_t bytes     = 0;
        for (const Slot& s : m_slots)
            if (s.alive && s.meshletBuf.IsValid())
            {
                ++libraries;
                bytes += s.meshletBuf.desc.size;
            }
        LOG_INFO("[MESHLET] resident event=%s path='%s' libraries=%u bytes=%llu", event, path.c_str(), libraries,
                 static_cast<unsigned long long>(bytes));
    }

    void MeshLibrary::Shutdown(IGraphicsDevice& gfx)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& slot : m_slots)
        {
            if (!slot.alive) continue;
            if (slot.vb.IsValid()) gfx.DestroyBuffer(slot.vb);
            if (slot.ib.IsValid()) gfx.DestroyBuffer(slot.ib);
            if (slot.meshletBuf.IsValid()) gfx.DestroyBuffer(slot.meshletBuf);
            slot.alive = false;
        }
        m_slots.clear();
        m_freeList.clear();
        m_pathHashToSlot.clear();
        m_loadFailLogged.clear();
    }

} // namespace Resource
