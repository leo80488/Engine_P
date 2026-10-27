#include "Graphics/MeshManager.h"
#include "Graphics/IGraphicsDevice.h"
#include "Resource/MeshLibrary.h"
#include "Resource/AssetHeader.h"   // ComputeMeshLibVertexLayout / MESHLIB_FLAG_*
#include "System/Check.h"
#include "System/Log.h"

void MeshManager::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_descHeap.Init(gfx);
}

bool MeshManager::UploadMesh(const ProceduralMesh::MeshData& data, GPUMesh& out, bool persistent)
{
    if (!m_gfx) return false;

    auto makeRaw = [&](const void* rawData, uint64_t size) -> RHI::GPUBuffer
    {
        RHI::GPUBuffer buf;
        RHI::GPUBufferDesc d;
        d.size       = size;
        d.usage      = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        m_gfx->CreateBuffer(d, buf, rawData);
        return buf;
    };

    const uint64_t posSz  = data.positions.size() * sizeof(data.positions[0]);
    const uint64_t norSz  = data.normals.size()   * sizeof(data.normals[0]);
    const uint64_t idxSz  = data.indices.size()   * sizeof(data.indices[0]);
    const bool hasTangents = !data.tangents.empty();
    const bool hasUVs      = !data.uvs.empty();
    const bool hasColors   = !data.colors.empty();
    const uint64_t tanSz  = hasTangents ? data.tangents.size() * sizeof(data.tangents[0]) : 0;
    const uint64_t uvSz   = hasUVs      ? data.uvs.size()      * sizeof(data.uvs[0])      : 0;
    const uint64_t colSz  = hasColors   ? data.colors.size()   * sizeof(data.colors[0])   : 0;

    out.posBuffer    = makeRaw(data.positions.data(), posSz);
    out.normalBuffer = makeRaw(data.normals.data(),   norSz);
    out.indexBuffer  = makeRaw(data.indices.data(),   idxSz);
    if (hasTangents) out.tangentBuffer = makeRaw(data.tangents.data(), tanSz);
    if (hasUVs)      out.uvBuffer      = makeRaw(data.uvs.data(),      uvSz);
    if (hasColors)   out.colorBuffer   = makeRaw(data.colors.data(),   colSz);
    out.indexCount   = static_cast<uint32_t>(data.indices.size());
    out.vertexCount  = static_cast<uint32_t>(data.positions.size());

    // Pick the register variant by lifetime. The persistent path lands in the low reserved slot range,
    // so the slot indices cached on @p out survive every world reload; the world path is dedup'd and
    // cleared with the rest of the per-world allocations.
    auto regBuf = [&](const RHI::GPUBuffer& b) {
        return persistent ? m_descHeap.RegisterPersistentBuffer(b)
                          : m_descHeap.RegisterBuffer(b);
    };

    const uint32_t posIdx = regBuf(out.posBuffer);
    const uint32_t norIdx = regBuf(out.normalBuffer);
    const uint32_t idxIdx = regBuf(out.indexBuffer);
    const uint32_t tanIdx = hasTangents ? regBuf(out.tangentBuffer)
                                        : RHI::kInvalidBufferIndex;
    const uint32_t uvIdx  = hasUVs      ? regBuf(out.uvBuffer)
                                        : RHI::kInvalidBufferIndex;
    const uint32_t colIdx = hasColors   ? regBuf(out.colorBuffer)
                                        : RHI::kInvalidBufferIndex;

    if (posIdx == RHI::kInvalidBufferIndex ||
        norIdx == RHI::kInvalidBufferIndex ||
        idxIdx == RHI::kInvalidBufferIndex ||
        (hasColors && colIdx == RHI::kInvalidBufferIndex))
    {
        LOG_ERROR("MeshManager::UploadMesh: bindless table full (persistent=%d)", persistent ? 1 : 0);
        return false;
    }

    // Retain the stream slots on the GPUMesh: DDGISceneAS forwards them to the RT trace shaders
    // (de-interleaved attribute fetch at ray hits).
    out.posBindlessIdx    = posIdx;
    out.normalBindlessIdx = norIdx;
    out.uvBindlessIdx     = uvIdx;
    out.ibBindlessIdx     = idxIdx;

    RHI::MeshDescriptor md{};
    md.position.bufferIndex = posIdx;
    md.position.byteOffset  = 0;
    md.position.byteStride  = sizeof(float) * 3;
    md.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.normal.bufferIndex = norIdx;
    md.normal.byteOffset  = 0;
    md.normal.byteStride  = sizeof(float) * 3;
    md.normal.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    if (hasTangents && tanIdx != RHI::kInvalidBufferIndex)
    {
        md.tangent.bufferIndex = tanIdx;
        md.tangent.byteOffset  = 0;
        md.tangent.byteStride  = sizeof(float) * 4;
        md.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }

    if (hasUVs && uvIdx != RHI::kInvalidBufferIndex)
    {
        md.uv0.bufferIndex = uvIdx;
        md.uv0.byteOffset  = 0;
        md.uv0.byteStride  = sizeof(float) * 2;
        md.uv0.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }

    // Procedural per-vertex color -> dedicated color stream (Float3; the shader defaults alpha to 1).
    // uv1 stays absent: primitives have no second UV set.
    if (hasColors && colIdx != RHI::kInvalidBufferIndex)
    {
        md.color.bufferIndex = colIdx;
        md.color.byteOffset  = 0;
        md.color.byteStride  = sizeof(float) * 3;
        md.color.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
    }

    md.indexBufferIndex = idxIdx;
    md.indexByteOffset  = 0;
    md.indexFormat      = 0;
    md.vertexCount      = out.vertexCount;

    out.meshDescSlot = persistent ? m_descHeap.RegisterPersistentMesh(md)
                                  : m_descHeap.RegisterMesh(md);
    return out.meshDescSlot != RHI::kInvalidBufferIndex;
}

void MeshManager::InitPrimitives()
{
    constexpr int kCount = static_cast<int>(PrimitiveMeshType::Count);
    const ProceduralMesh::MeshData meshes[kCount] =
    {
        ProceduralMesh::Cube(),
        ProceduralMesh::Sphere(),
        ProceduralMesh::Cone(),
        ProceduralMesh::Plane(),
        ProceduralMesh::Torus(),
    };
    static const char* kPrimNames[] = { "Cube", "Sphere", "Cone", "Plane", "Torus" };

    for (int i = 0; i < kCount; ++i)
    {
        // persistent=true: primitive slots stay valid across every world reload
        // (entities created by MeshSpawner reference them by cached slot index).
        if (UploadMesh(meshes[i], m_primitives[i], /*persistent=*/true))
            LOG_SUCCESS("MeshManager: uploaded %s (meshDescSlot=%u, %u indices)",
                        kPrimNames[i], m_primitives[i].meshDescSlot,
                        m_primitives[i].indexCount);
        else
            LOG_ERROR("MeshManager: failed to upload %s", kPrimNames[i]);
    }
}

void MeshManager::InitBillboardQuad()
{
    ProceduralMesh::MeshData quad;
    quad.positions = {
        { -0.5f,  0.5f, 0.f }, {  0.5f,  0.5f, 0.f },
        { -0.5f, -0.5f, 0.f }, {  0.5f, -0.5f, 0.f }
    };
    quad.normals = {
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f },
        { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }
    };
    quad.uvs = {
        { 0.f, 0.f }, { 1.f, 0.f },
        { 0.f, 1.f }, { 1.f, 1.f }
    };
    quad.tangents = {
        { 1.f, 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f, 1.f },
        { 1.f, 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f, 1.f }
    };
    quad.colors = {
        { 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f },
        { 1.f, 1.f, 1.f }, { 1.f, 1.f, 1.f }
    };
    quad.indices = { 0, 1, 2, 2, 1, 3 };

    // persistent=true: billboard slot is referenced by every BillboardComponent
    // across every world; must survive reloads.
    if (UploadMesh(quad, m_billboardQuad, /*persistent=*/true))
    {
        m_billboardMeshDescSlot = m_billboardQuad.meshDescSlot;
        LOG_SUCCESS("MeshManager: billboard quad mesh registered (slot %u)",
                    m_billboardMeshDescSlot);
    }
    else
        LOG_ERROR("MeshManager: failed to create billboard quad mesh");
}

uint32_t MeshManager::RegisterMeshLibMesh(Resource::MeshLibrary& lib,
                                          const MeshLibRef& ref,
                                          MeshLibOwner owner)
{
    if (!ref.IsValid()) return RHI::kInvalidBufferIndex;

    if (ref.cachedGeneration == m_meshLibDescGeneration &&
        ref.cachedDescSlot   != RHI::kInvalidBufferIndex)
    {
        // Counted apart from the cache-map hits below: this per-REF memo says nothing about sharing (two
        // owners never hold the same MeshLibRef), so folding it into dedupHits would inflate the saving.
        ++m_mlRefMemoHits;
        return ref.cachedDescSlot;
    }

    const uint32_t libIdx = ref.libHandle.Index();
    const uint64_t cacheKey = (uint64_t(libIdx) << 32) | uint64_t(ref.meshId);

    // Only a success enters the cache (find, then insert on success). A transient failure below
    // (library not parsed, VB/IB not uploaded, bindless table full) must not latch an INVALID slot in
    // the map, or every later call returns kInvalidBufferIndex from the cache and the mesh never draws
    // again for the life of the World.
    auto descIt = m_meshLibDescCache.find(cacheKey);
    if (descIt != m_meshLibDescCache.end())
    {
        // Unreachable while only successes are inserted; the tripwire for an exit that reserves the key
        // early: a cached INVALID is a mesh that can never come back.
        ENGINE_CHECK_ONCE(descIt->second != RHI::kInvalidBufferIndex,
                          "MeshManager: poisoned meshlib desc cache entry served "
                          "(lib %u mesh %u) - this mesh can never render again",
                          libIdx, ref.meshId);
        // A hit by the OTHER owner is the saving: this branch minted no MeshDescriptor slot, and crossHits
        // counts the hits where the asker is not the owner that minted it (the preview riding on a slot the
        // world registered, or the reverse). Same-owner hits are ordinary cache behaviour, counted apart.
        {
            const auto oit = m_meshLibDescFirstOwner.find(cacheKey);
            const uint8_t first = (oit == m_meshLibDescFirstOwner.end())
                ? static_cast<uint8_t>(owner) : oit->second;
            if (first == static_cast<uint8_t>(owner)) ++m_mlHitsSame;
            else                                      ++m_mlHitsCross;
        }
        ref.cachedDescSlot   = descIt->second;
        ref.cachedGeneration = m_meshLibDescGeneration;
        return descIt->second;
    }

    const auto* entry = lib.GetEntry(ref.libHandle, ref.meshId);
    if (!entry) return RHI::kInvalidBufferIndex;

    const RHI::GPUBuffer* vb = lib.GetVertexBuffer(ref.libHandle);
    const RHI::GPUBuffer* ib = lib.GetIndexBuffer (ref.libHandle);
    if (!vb || !ib || !vb->IsValid() || !ib->IsValid())
    {
        LOG_ERROR("MeshManager::RegisterMeshLibMesh: library %u has invalid VB/IB", libIdx);
        return RHI::kInvalidBufferIndex;
    }

    const uint32_t vStride  = lib.GetVertexStride(ref.libHandle);
    const uint32_t libFlags = lib.GetMeshLibFlags(ref.libHandle);
    const Resource::MeshLibVertexLayout L =
        Resource::ComputeMeshLibVertexLayout(libFlags);

    MeshLibBindless& slots = m_meshLibBindless[libIdx];
    if (slots.vbIdx == RHI::kInvalidBufferIndex)
    {
        slots.vbIdx = m_descHeap.RegisterBuffer(*vb);
        slots.ibIdx = m_descHeap.RegisterBuffer(*ib);
        if (slots.vbIdx == RHI::kInvalidBufferIndex || slots.ibIdx == RHI::kInvalidBufferIndex)
        {
            LOG_ERROR("MeshManager::RegisterMeshLibMesh: bindless buffer table full");
            return RHI::kInvalidBufferIndex;
        }
        // The library's meshlet buffer, beside its VB/IB and under the same world-scoped registration
        // (DetachMeshLib / OnWorldClear drop it with them). A full table costs the meshlets only.
        if (const RHI::GPUBuffer* mb = lib.GetMeshletBuffer(ref.libHandle))
            slots.meshletIdx = m_descHeap.RegisterBuffer(*mb);
    }

    RHI::MeshDescriptor md{};
    md.position.bufferIndex = slots.vbIdx;
    md.position.byteOffset  = L.posOffset;
    md.position.byteStride  = vStride;
    md.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.normal.bufferIndex   = slots.vbIdx;
    md.normal.byteOffset    = L.normalOffset;
    md.normal.byteStride    = vStride;
    md.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);

    md.uv0.bufferIndex      = slots.vbIdx;
    md.uv0.byteOffset       = L.uv0Offset;
    md.uv0.byteStride       = vStride;
    md.uv0.format           = static_cast<uint32_t>(RHI::VertexFormat::Float2);

    // Optional interleaved streams, present only when the matching MESHLIB_FLAG_* bit set the offset.
    // Absent streams keep their default INVALID_BUFFER so the shader helpers fall back (tangent ->
    // synthesised TBN, uv1 -> (0,0), color -> white).
    if (L.tangentOffset != 0xFFFFFFFFu)
    {
        md.tangent.bufferIndex = slots.vbIdx;
        md.tangent.byteOffset  = L.tangentOffset;
        md.tangent.byteStride  = vStride;
        md.tangent.format      = static_cast<uint32_t>(RHI::VertexFormat::Float4);
    }
    if (L.uv1Offset != 0xFFFFFFFFu)
    {
        md.uv1.bufferIndex = slots.vbIdx;
        md.uv1.byteOffset  = L.uv1Offset;
        md.uv1.byteStride  = vStride;
        md.uv1.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (L.uv2Offset != 0xFFFFFFFFu)
    {
        md.uv2.bufferIndex = slots.vbIdx;
        md.uv2.byteOffset  = L.uv2Offset;
        md.uv2.byteStride  = vStride;
        md.uv2.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (L.uv3Offset != 0xFFFFFFFFu)
    {
        md.uv3.bufferIndex = slots.vbIdx;
        md.uv3.byteOffset  = L.uv3Offset;
        md.uv3.byteStride  = vStride;
        md.uv3.format      = static_cast<uint32_t>(RHI::VertexFormat::Float2);
    }
    if (L.colorOffset != 0xFFFFFFFFu)
    {
        md.color.bufferIndex = slots.vbIdx;
        md.color.byteOffset  = L.colorOffset;
        md.color.byteStride  = vStride;
        md.color.format      = static_cast<uint32_t>(RHI::VertexFormat::R8G8B8A8);
    }

    md.indexBufferIndex     = slots.ibIdx;
    md.indexByteOffset      = entry->indexStart * 4u;
    md.indexFormat          = 1;
    md.vertexCount          = entry->vertexCount;

    const uint32_t slot = m_descHeap.RegisterMesh(md);
    if (slot != RHI::kInvalidBufferIndex)
    {
        m_meshLibDescCache.emplace(cacheKey, slot);
        // First touch, recorded beside the entry it describes.
        m_meshLibDescFirstOwner.emplace(cacheKey, static_cast<uint8_t>(owner));
        if (owner == MeshLibOwner::Preview) ++m_mlInsertsPreview;
        else                                ++m_mlInsertsScene;
        ref.cachedDescSlot   = slot;
        ref.cachedGeneration = m_meshLibDescGeneration;

        // Publish the mesh's local bounds for the GPU command cull. Only when
        // non-degenerate: a meshlib authored without bounds keeps the heap's
        // huge default AABB (= never culled) instead of collapsing every
        // instance to a point at its origin and getting frustum-culled while
        // its real geometry is on screen.
        if (entry->aabbMax.x > entry->aabbMin.x ||
            entry->aabbMax.y > entry->aabbMin.y ||
            entry->aabbMax.z > entry->aabbMin.z)
        {
            m_descHeap.SetMeshAABB(slot,
                { entry->aabbMin.x, entry->aabbMin.y, entry->aabbMin.z,
                  entry->aabbMax.x, entry->aabbMax.y, entry->aabbMax.z });
        }

        // Where this entry's clusters are: the ONE writer of real sidecar values (RegisterMesh above reset
        // the slot).
        Resource::MeshLibrary::MeshletEntryInfo mi;
        if (slots.meshletIdx != RHI::kInvalidBufferIndex && lib.GetMeshletEntry(ref.libHandle, ref.meshId, mi))
            m_descHeap.SetMeshletInfo(slot, { slots.meshletIdx, mi.firstCluster, mi.clusterCount,
                                              mi.shadedClusterCount });
    }
    // No else: a failed RegisterMesh simply never enters the cache, so the next
    // call retries from scratch once the heap has room again.
    return slot;
}

// Step 1 of 2 (contract in the header): lifts this library's state out; frees nothing.
void MeshManager::DetachMeshLib(Resource::Handle libHandle, MeshLibHandback& out)
{
    out = MeshLibHandback{};
    if (!libHandle.IsValid()) return;
    const uint32_t libIdx = libHandle.Index();

    // 1. The per-entry mesh descriptors.
    for (auto it = m_meshLibDescCache.begin(); it != m_meshLibDescCache.end(); )
    {
        if (static_cast<uint32_t>(it->first >> 32) != libIdx) { ++it; continue; }
        if (it->second != RHI::kInvalidBufferIndex)
            out.meshDescSlots.push_back(it->second);
        m_meshLibDescFirstOwner.erase(it->first);   // its sharing-metric entry
        it = m_meshLibDescCache.erase(it);
    }

    // 2. The library-wide bindless VB/IB slots, taken from the map as indices: the only form that
    //    outlives the library (MeshDescriptorHeap::FreeBuffer resolves the slot from a resource that
    //    is gone by the deferred half). Either may be kInvalidBufferIndex (RegisterBuffer failed on a
    //    full table); the handback then carries nothing for it.
    //    The heap's registration goes now, the index later: DetachBufferSlot touches no descriptor
    //    (MeshDescriptorHeap::DetachBufferSlot explains the ProcessAllDeferredReleases window).
    auto bit = m_meshLibBindless.find(libIdx);
    if (bit != m_meshLibBindless.end())
    {
        out.vbSlot      = bit->second.vbIdx;
        out.ibSlot      = bit->second.ibIdx;
        out.meshletSlot = bit->second.meshletIdx;
        m_meshLibBindless.erase(bit);
        if (out.vbSlot != RHI::kInvalidBufferIndex) m_descHeap.DetachBufferSlot(out.vbSlot);
        if (out.ibSlot != RHI::kInvalidBufferIndex) m_descHeap.DetachBufferSlot(out.ibSlot);
        if (out.meshletSlot != RHI::kInvalidBufferIndex) m_descHeap.DetachBufferSlot(out.meshletSlot);
    }

    // 3. Every MeshLibRef's cached slot, invalidated wholesale. Cheap: re-taking one is a lookup in
    //    the cache above, which still holds every other library's entries. Done here, not in the
    //    deferred half: it makes callers stop naming the slots, so the sooner the shorter the window.
    ++m_meshLibDescGeneration;
}

// Step 2 of 2, kFramesInFlight frames later (see the header).
void MeshManager::ReleaseMeshLibHandback(const MeshLibHandback& hb)
{
    for (uint32_t slot : hb.meshDescSlots) m_descHeap.FreeMesh(slot);
    if (hb.vbSlot != RHI::kInvalidBufferIndex) m_descHeap.FreeBufferSlot(hb.vbSlot);
    if (hb.ibSlot != RHI::kInvalidBufferIndex) m_descHeap.FreeBufferSlot(hb.ibSlot);
    if (hb.meshletSlot != RHI::kInvalidBufferIndex) m_descHeap.FreeBufferSlot(hb.meshletSlot);
}

void MeshManager::ReportMeshLibDescOwnership() const
{
    if (m_mlInsertsPreview == 0 && m_mlInsertsScene == 0 &&
        m_mlHitsSame == 0 && m_mlHitsCross == 0)
        return;
    LOG_INFO("[MESH] meshlib desc cache: live=%zu inserts=%u previewFirst=%u "
             "sceneFirst=%u dedupHits=%u crossHits=%u sameHits=%u "
             "refMemoHits=%u",
             m_meshLibDescCache.size(),
             m_mlInsertsPreview + m_mlInsertsScene,
             m_mlInsertsPreview, m_mlInsertsScene,
             m_mlHitsSame + m_mlHitsCross, m_mlHitsCross, m_mlHitsSame,
             m_mlRefMemoHits);
}

void MeshManager::OnWorldClear()
{
    m_meshLibDescCache.clear();
    m_meshLibDescFirstOwner.clear();   // it describes those entries
    m_meshLibBindless.clear();
    ++m_meshLibDescGeneration;
    // Reset only the heap's world-scoped slot allocators: the persistent range [0..kPermanent*Slots)
    // keeps its SRVs, so the primitives + billboard quad (UploadMesh(..., persistent=true)) and the
    // skinned vertex ring (RegisterPersistentBuffer) keep valid slot indices across the reload.
    // The caller must FlushAndWait first so the GPU is not still reading world slots
    // (Renderer::OnWorldClear runs mid-frame, but the editor "Load World..." path flushes first).
    m_descHeap.OnWorldClear();
}
