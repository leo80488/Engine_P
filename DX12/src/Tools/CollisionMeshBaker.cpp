#include "Tools/CollisionMeshBaker.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"   // MeshSourcePath, MeshLibRef
#include "ECS/PhysicsComponents.h"     // ColliderComponent (debug wireframe walk)
#include "ECS/StaticMeshComponents.h"  // a MeshAsset collider with no model = the entity's StaticMesh
#include "ECS/AnimationComponents.h"   // SkeletonRef - a skinned part (Skip skinned)
#include "Resource/ModelCollision.h"   // an asset collider's version (ModelCollisionAt)
#include "Physics/PhysicsSystem.h"     // EnsureCollisionBlob - shared blob cache
#include "Resource/AssetFS.h"          // AssetFS::Get().ReadFile
#include "Resource/AssetHeader.h"      // MAGIC_*, accessor helpers
#include "Resource/MeshLibIdentity.h"  // IDNT - the output library's Mesh/Section keys
#include "RenderGraph/RenderPass/DebugWirePass.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/TaskSystem.h"

#include "meshoptimizer/meshoptimizer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>   // debug-wireframe decoded-slice cache

#ifdef _DEBUG
#pragma comment(lib, "meshoptimizer_debug.lib")
#else
#pragma comment(lib, "meshoptimizer_release.lib")
#endif

namespace Tools::CollisionMesh {

namespace {

// 32-byte vertex written to the output .meshlib. Normal / uv stay zero: collision and LOD
// callers do not need them, and GBuffer.vs's synthesised basis still renders the shape
// for preview.
struct PackedVertex32
{
    float px, py, pz;
    float nx, ny, nz;
    float u,  v;
};
static_assert(sizeof(PackedVertex32) == 32, "PackedVertex32 layout drift");

std::string ToLowerExt(const std::filesystem::path& p)
{
    std::string ext = Sys::PathToUtf8(p.extension());
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool DecodeImsh(const std::vector<uint8_t>& blob,
                std::vector<DirectX::XMFLOAT3>& outPositions,
                std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
        return false;

    const auto* meta = Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
    const uint32_t nv      = meta->vertexCount;
    const uint32_t ni      = meta->indexCount;
    const uint32_t vStride = meta->vertexStride;
    if (nv == 0 || ni == 0 || vStride < sizeof(float) * 3)
        return false;

    const uint8_t* payload = Resource::GetPayload(blob.data());
    const uint8_t* vbBase  = payload;
    const uint8_t* ibBase  = vbBase + size_t(nv) * vStride;

    outPositions.resize(nv);
    for (uint32_t i = 0; i < nv; ++i)
        std::memcpy(&outPositions[i], vbBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));

    outIndices.resize(ni);
    std::memcpy(outIndices.data(), ibBase, size_t(ni) * sizeof(uint32_t));
    return true;
}

bool DecodeMeshLibEntry(const std::vector<uint8_t>& blob, uint32_t meshId,
                        std::vector<DirectX::XMFLOAT3>& outPositions,
                        std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
        return false;

    const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
    if (meshId >= meta->meshCount) return false;

    const uint8_t* payload   = Resource::GetPayload(blob.data());
    const auto*    entries   = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
    const auto&    entry     = entries[meshId];
    if (entry.vertexCount == 0 || entry.indexCount == 0) return false;

    const uint32_t vStride   = meta->vertexStride;
    const uint8_t* vbBase    = payload + size_t(meta->meshCount) * sizeof(Resource::MeshLibraryEntry);
    const uint8_t* ibBase    = vbBase + size_t(meta->vertexCount) * vStride;
    const uint8_t* vertsBase = vbBase + size_t(entry.vertexStart) * vStride;
    const auto*    idxData   = reinterpret_cast<const uint32_t*>(ibBase) + entry.indexStart;

    outPositions.resize(entry.vertexCount);
    for (uint32_t i = 0; i < entry.vertexCount; ++i)
        std::memcpy(&outPositions[i], vertsBase + size_t(i) * vStride, sizeof(DirectX::XMFLOAT3));

    outIndices.resize(entry.indexCount);
    for (uint32_t i = 0; i < entry.indexCount; ++i)
        outIndices[i] = idxData[i] - entry.vertexStart;
    return true;
}

bool SimplifyPositionsIndices(std::vector<DirectX::XMFLOAT3>& positions,
                              std::vector<uint32_t>&          indices,
                              const BakeOptions&              opts,
                              float&                          outError,
                              DirectX::XMFLOAT3&              outAabbMin,
                              DirectX::XMFLOAT3&              outAabbMax)
{
    outError = 0.f;
    if (positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
        return false;

    const size_t stride = sizeof(DirectX::XMFLOAT3);

    std::vector<uint32_t> remap(positions.size());
    const size_t welded = meshopt_generateVertexRemap(
        remap.data(),
        indices.data(), indices.size(),
        positions.data(), positions.size(), stride);

    std::vector<DirectX::XMFLOAT3> weldedVerts(welded);
    meshopt_remapVertexBuffer(weldedVerts.data(), positions.data(),
                              positions.size(), stride, remap.data());
    std::vector<uint32_t> weldedIdx(indices.size());
    meshopt_remapIndexBuffer(weldedIdx.data(), indices.data(),
                             indices.size(), remap.data());

    const size_t origTris   = weldedIdx.size() / 3;
    size_t targetTris       = static_cast<size_t>(std::max<uint32_t>(
        opts.minTriangles,
        static_cast<uint32_t>(origTris * opts.targetTriangleRatio)));
    if (targetTris > origTris) targetTris = origTris;
    const size_t targetIndexCount = targetTris * 3;

    std::vector<uint32_t> simplified(weldedIdx.size());
    const size_t outIndexCount = meshopt_simplify(
        simplified.data(),
        weldedIdx.data(), weldedIdx.size(),
        reinterpret_cast<const float*>(weldedVerts.data()),
        weldedVerts.size(), stride,
        targetIndexCount,
        opts.targetError,
        /*options*/ 0,
        &outError);
    simplified.resize(outIndexCount);
    if (simplified.empty()) return false;

    std::vector<uint32_t> compactRemap(weldedVerts.size());
    const size_t finalVertCount = meshopt_optimizeVertexFetchRemap(
        compactRemap.data(),
        simplified.data(), simplified.size(),
        weldedVerts.size());

    std::vector<DirectX::XMFLOAT3> finalVerts(finalVertCount);
    meshopt_remapVertexBuffer(finalVerts.data(), weldedVerts.data(),
                              weldedVerts.size(), stride, compactRemap.data());
    std::vector<uint32_t> finalIdx(simplified.size());
    meshopt_remapIndexBuffer(finalIdx.data(), simplified.data(),
                             simplified.size(), compactRemap.data());

    outAabbMin = finalVerts[0];
    outAabbMax = finalVerts[0];
    for (const auto& p : finalVerts)
    {
        outAabbMin.x = std::min(outAabbMin.x, p.x);
        outAabbMin.y = std::min(outAabbMin.y, p.y);
        outAabbMin.z = std::min(outAabbMin.z, p.z);
        outAabbMax.x = std::max(outAabbMax.x, p.x);
        outAabbMax.y = std::max(outAabbMax.y, p.y);
        outAabbMax.z = std::max(outAabbMax.z, p.z);
    }

    positions = std::move(finalVerts);
    indices   = std::move(finalIdx);
    return true;
}

} // namespace

// ==== BakeJob ====

BakeJob::~BakeJob()
{
    WaitForWorkers();
}

void BakeJob::WaitForWorkers()
{
    // Spin until all chunks have finished; usually the queue has already drained and this
    // falls through.
    while (m_inFlightChunks.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

void BakeJob::Reset()
{
    WaitForWorkers();
    m_opts = {};
    m_queue.clear();
    m_perItem.clear();
    m_processedCount.store(0, std::memory_order_relaxed);
    m_inFlightChunks.store(0, std::memory_order_relaxed);
    m_active      = false;
    m_completed   = false;
    m_writeFailed = false;
    m_mergedVB.clear();
    m_mergedIB.clear();
    m_entries.clear();
    m_results.clear();
    m_origTriTotal  = 0;
    m_bakedTriTotal = 0;
}

void BakeJob::Begin(World& world, const BakeOptions& opts)
{
    Reset();
    m_opts   = opts;
    m_active = true;

    std::set<std::pair<std::string, uint32_t>> seen;
    uint32_t skinned = 0;
    world.ForEach<MeshLibRef>([&](Entity e, MeshLibRef& ref)
    {
        const MeshSourcePath* sp = world.GetComponent<MeshSourcePath>(e);
        if (!sp || sp->path.empty()) return;
        // A skinned part moves with its skeleton: a static trimesh of its bind pose would be a
        // wall where the character stood.
        if (opts.skipSkinned && world.HasComponent<SkeletonRef>(e)) { ++skinned; return; }
        if (!seen.insert({ sp->path, ref.meshId }).second) return;
        m_queue.push_back({ sp->path, ref.meshId });
    });
    if (skinned) LOG_INFO("CollisionMeshBaker: skipped %u skinned part(s) (Skip skinned)", skinned);

    const uint32_t N = static_cast<uint32_t>(m_queue.size());
    LOG_INFO("CollisionMeshBaker: queued %u unique source meshes", N);

    if (N == 0)
    {
        m_completed = true;
        m_active    = false;
        return;
    }

    m_perItem.assign(N, PerItemOutput{});

    // Split [0, N) across as many chunks as the pool has workers. Each chunk
    // runs DecodeAndSimplify serially over its slice and bumps the atomics
    // when done. Main thread polls in Tick().
    const uint32_t workers   = std::max(1u, TaskSystem::Get().GetWorkerCount());
    const uint32_t chunks    = std::min(N, workers);
    const uint32_t chunkSize = (N + chunks - 1) / chunks;

    m_inFlightChunks.store(chunks, std::memory_order_release);

    for (uint32_t t = 0; t < chunks; ++t)
    {
        const uint32_t lo = t * chunkSize;
        const uint32_t hi = std::min(N, lo + chunkSize);
        if (lo >= hi) { m_inFlightChunks.fetch_sub(1, std::memory_order_acq_rel); continue; }

        TaskSystem::Get().Push([this, lo, hi]()
        {
            for (uint32_t i = lo; i < hi; ++i)
            {
                DecodeAndSimplify(m_queue[i], m_perItem[i]);
                m_processedCount.fetch_add(1, std::memory_order_relaxed);
            }
            m_inFlightChunks.fetch_sub(1, std::memory_order_acq_rel);
        }, TaskSystem::TaskPriority::High);
    }

    LOG_INFO("CollisionMeshBaker: dispatched %u parallel chunks (%u workers)", chunks, workers);
}

bool BakeJob::Tick()
{
    if (!m_active || m_completed)
        return false;

    // Still work in flight? Just paint the progress bar this frame.
    if (m_inFlightChunks.load(std::memory_order_acquire) != 0)
        return true;

    // All workers done: serial merge phase on the main thread.
    m_results.reserve(m_queue.size());
    for (uint32_t i = 0; i < m_queue.size(); ++i)
    {
        BakeItemResult r{};
        AppendOne(m_queue[i], m_perItem[i], r);
        m_results.push_back(std::move(r));
    }
    Finalize();
    m_active    = false;
    m_completed = true;
    return false;
}

// Runs on a worker thread. Must not touch m_merged* / m_entries / m_results.
void BakeJob::DecodeAndSimplify(const Item& item, PerItemOutput& out)
{
    namespace fs = std::filesystem;

    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(item.path, blob) || blob.empty())
    {
        out.success = false; out.reason = "cannot read source file";
        return;
    }

    const std::string ext = ToLowerExt(Sys::U8Path(item.path));
    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;
    bool decoded = false;
    if (ext == ".meshlib")
    {
        decoded = DecodeMeshLibEntry(blob, item.meshId, positions, indices);
        out.sourceSectionKey = Resource::SectionKeyOfEntry(blob.data(), blob.size(), item.meshId);
    }
    else if (ext == ".imsh") decoded = DecodeImsh(blob, positions, indices);
    else                     { out.reason = "unsupported source extension"; return; }

    if (!decoded)
    {
        out.success = false; out.reason = "decode failed";
        return;
    }

    out.origVertCount = static_cast<uint32_t>(positions.size());
    out.origTriCount  = static_cast<uint32_t>(indices.size() / 3);

    if (!SimplifyPositionsIndices(positions, indices, m_opts,
                                   out.actualError, out.aabbMin, out.aabbMax))
    {
        out.success = false; out.reason = "simplification failed";
        return;
    }

    out.positions = std::move(positions);
    out.indices   = std::move(indices);
    out.success   = true;
}

// Runs on the main thread after all workers finish.
void BakeJob::AppendOne(const Item& item, const PerItemOutput& slot, BakeItemResult& r)
{
    r.sourcePath    = item.path;
    r.sourceMeshId  = item.meshId;
    r.sourceSectionKey = slot.sourceSectionKey;
    r.origVertCount = slot.origVertCount;
    r.origTriCount  = slot.origTriCount;

    if (!slot.success)
    {
        r.success = false;
        r.reason  = slot.reason.empty() ? "unknown failure" : slot.reason;
        return;
    }

    const uint32_t vertexStart = static_cast<uint32_t>(
        m_mergedVB.size() / sizeof(PackedVertex32));
    const uint32_t indexStart  = static_cast<uint32_t>(m_mergedIB.size());

    const size_t oldVBSize = m_mergedVB.size();
    m_mergedVB.resize(oldVBSize + slot.positions.size() * sizeof(PackedVertex32));
    auto* vbDst = reinterpret_cast<PackedVertex32*>(m_mergedVB.data() + oldVBSize);
    for (size_t i = 0; i < slot.positions.size(); ++i)
    {
        vbDst[i] = {};
        vbDst[i].px = slot.positions[i].x;
        vbDst[i].py = slot.positions[i].y;
        vbDst[i].pz = slot.positions[i].z;
        // normal/uv left zero.
    }

    m_mergedIB.reserve(m_mergedIB.size() + slot.indices.size());
    for (uint32_t idx : slot.indices)
        m_mergedIB.push_back(idx + vertexStart);

    Resource::MeshLibraryEntry entry{};
    entry.vertexStart       = vertexStart;
    entry.vertexCount       = static_cast<uint32_t>(slot.positions.size());
    entry.indexStart        = indexStart;
    entry.indexCount        = static_cast<uint32_t>(slot.indices.size());
    entry.aabbMin[0] = slot.aabbMin.x; entry.aabbMin[1] = slot.aabbMin.y; entry.aabbMin[2] = slot.aabbMin.z;
    entry.aabbMax[0] = slot.aabbMax.x; entry.aabbMax[1] = slot.aabbMax.y; entry.aabbMax[2] = slot.aabbMax.z;
    entry.defaultMaterialIdx = 0xFFFFFFFFu;
    entry.flags             = 0;
    m_entries.push_back(entry);

    r.bakedMeshId    = static_cast<uint32_t>(m_entries.size() - 1);
    r.bakedVertCount = entry.vertexCount;
    r.bakedTriCount  = entry.indexCount / 3;
    r.actualError    = slot.actualError;
    r.success        = true;

    m_origTriTotal  += r.origTriCount;
    m_bakedTriTotal += r.bakedTriCount;
}

bool BakeJob::Finalize()
{
    namespace fs = std::filesystem;

    if (m_entries.empty())
    {
        LOG_WARNING("CollisionMeshBaker: no meshes survived the bake — nothing written");
        return false;
    }

    Sys::CreateParentDirs(m_opts.outputPath);

    const uint32_t entryBytes  = static_cast<uint32_t>(
        m_entries.size() * sizeof(Resource::MeshLibraryEntry));
    const uint32_t vbBytes     = static_cast<uint32_t>(m_mergedVB.size());
    const uint32_t ibBytes     = static_cast<uint32_t>(m_mergedIB.size() * sizeof(uint32_t));
    const uint32_t vertexCount = vbBytes / static_cast<uint32_t>(sizeof(PackedVertex32));
    const uint32_t indexCount  = static_cast<uint32_t>(m_mergedIB.size());

    Resource::MeshLibraryMetadata meta{};
    meta.meshCount    = static_cast<uint32_t>(m_entries.size());
    meta.vertexCount  = vertexCount;
    meta.indexCount   = indexCount;
    meta.vertexStride = static_cast<uint16_t>(sizeof(PackedVertex32));
    meta.indexStride  = 4;
    meta.flags        = 0;
    meta.reserved     = 0;

    Resource::AssetHeader hdr{};
    hdr.magic        = Resource::MAGIC_MESHLIB;
    hdr.version      = Resource::CurrentVersion(Resource::MAGIC_MESHLIB);
    hdr.resourceType = static_cast<uint16_t>(Resource::ResourceType::MeshLibrary);
    hdr.metadataSize = sizeof(Resource::MeshLibraryMetadata);
    hdr.dataSize     = entryBytes + vbBytes + ibBytes;
    hdr.flags        = 0;
    hdr.reserved     = 0;

    const size_t totalBytes = sizeof(Resource::AssetHeader)
                            + sizeof(Resource::MeshLibraryMetadata)
                            + entryBytes + vbBytes + ibBytes;
    std::vector<uint8_t> blob(totalBytes);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr,  sizeof(hdr));   dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta));  dst += sizeof(meta);
    std::memcpy(dst, m_entries.data(), entryBytes); dst += entryBytes;
    if (vbBytes > 0)  { std::memcpy(dst, m_mergedVB.data(), vbBytes); dst += vbBytes; }
    if (ibBytes > 0)  { std::memcpy(dst, m_mergedIB.data(), ibBytes); }

    // Identity: every baked entry is its own hidden Mesh, keyed by WHAT it was
    // baked from - (source library, source section key) - so a re-bake that
    // walks the world in another order keys the same proxy the same way.
    {
        Resource::MeshIdentitySpec spec;
        spec.modelSource = "bake|" + Resource::MeshLibCanonicalPath(m_opts.outputPath);
        spec.sections.resize(m_entries.size());
        for (const BakeItemResult& r : m_results)
        {
            if (!r.success || r.bakedMeshId >= m_entries.size()) continue;
            const std::string src = Resource::MeshLibCanonicalPath(r.sourcePath);
            Resource::MeshIdentitySpec::Mesh m;
            m.name      = Sys::PathToUtf8(Sys::U8Path(r.sourcePath).stem()) + " #" + std::to_string(r.sourceMeshId);
            m.sourceKey = "bake:" + src + "#" + (r.sourceSectionKey ? Resource::MeshKeyToHex(r.sourceSectionKey)
                                                                   : std::to_string(r.sourceMeshId));
            m.flags     = Resource::MESH_IDENT_HIDDEN;
            m.sections  = { r.bakedMeshId };
            spec.sections[r.bakedMeshId].slotName  = "collision";
            spec.sections[r.bakedMeshId].sourceKey = m.sourceKey + "/collision";
            spec.meshes.push_back(std::move(m));
        }
        Resource::MeshIdentity id;
        std::string            why;
        if (!Resource::BuildMeshIdentity(spec, blob.data(), blob.size(), id, &why) ||
            !Resource::WriteMeshLibIdentity(blob, id, &why))
            LOG_WARNING("CollisionMeshBaker: identity layer not written (%s)", why.c_str());
    }

    std::ofstream f = Sys::OpenOfstream(m_opts.outputPath, std::ios::binary | std::ios::trunc);
    if (!f)
    {
        LOG_ERROR("CollisionMeshBaker: cannot open '%s' for writing",
                  m_opts.outputPath.c_str());
        m_writeFailed = true;
        return false;
    }
    f.write(reinterpret_cast<const char*>(blob.data()),
            static_cast<std::streamsize>(blob.size()));
    if (!f.good())
    {
        LOG_ERROR("CollisionMeshBaker: write failed for '%s'", m_opts.outputPath.c_str());
        m_writeFailed = true;
        return false;
    }

    LOG_INFO("CollisionMeshBaker: wrote '%s' (%u meshes, %u verts, %u indices, "
             "%llu -> %llu tris, %zu bytes)",
             m_opts.outputPath.c_str(),
             meta.meshCount, vertexCount, indexCount,
             static_cast<unsigned long long>(m_origTriTotal),
             static_cast<unsigned long long>(m_bakedTriTotal),
             blob.size());
    return true;
}

// ==== EmitDebugWireframe: per-frame collision wireframe overlay ====
// File bytes come from PhysicsSystem's blob cache (the one GetOrBuildMeshShape uses for
// body creation), so toggling the wireframe on adds no I/O and no second cache. Decoded
// slices and shape triangles are cached below.
namespace { WireStats s_wireStats; }
const WireStats& LastWireStats() { return s_wireStats; }

void EmitDebugWireframe(World& world,
                        const DirectX::XMFLOAT3& cameraPos,
                        float maxDistance,
                        DebugWirePass& dbg,
                        DX12Physics::PhysicsSystem& physics,
                        uint32_t color,
                        Entity selected)
{
    // maxDistance <= 0 -> unlimited (every collider drawn).
    const bool  cullByDistance = maxDistance > 0.f;
    const float maxD2          = maxDistance * maxDistance;

    WireStats& stats = s_wireStats;
    stats = WireStats{};

    // What the body IS decides the colour (see the header): selected (the
    // entity, or the Static Mesh owning a derived section), a trigger, an
    // explicit non-world layer, a moving body, else the static world.
    auto colourFor = [&](Entity e, const ColliderComponent& c) -> uint32_t
    {
        if (selected != NullEntity)
        {
            if (e == selected) return kWireSelected;
            if (const MeshSectionOf* ms = world.GetComponent<MeshSectionOf>(e); ms && ms->owner == selected)
                return kWireSelected;
        }
        if (c.isTrigger) return kWireTrigger;
        switch (c.layer)
        {
        case ColliderComponent::CollisionLayer::HurtBox:   return kWireHurtBox;
        case ColliderComponent::CollisionLayer::AttackBox: return kWireAttack;
        case ColliderComponent::CollisionLayer::Auto:
        case ColliderComponent::CollisionLayer::StaticEnv: break;
        default:                                           return kWireLayer;
        }
        const RigidBodyComponent* rb = world.GetComponent<RigidBodyComponent>(e);
        if (rb && rb->motion != RigidBodyComponent::Motion::Static && c.shape != ColliderComponent::Shape::Mesh)
            return kWireMoving;
        return color;
    };
    // A trigger's edges are dashed (a sensor, not a wall).
    constexpr float kDash = 0.15f;
    auto line = [&dbg](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t col, bool dashed)
    {
        if (!dashed) { dbg.AddLine(a, b, col); return; }
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const int   n   = (std::max)(1, (std::min)(64, static_cast<int>(len / kDash)));
        for (int k = 0; k < n; k += 2)
        {
            const float t0 = float(k) / n, t1 = float(k + 1) / n;
            dbg.AddLine({ a.x + dx * t0, a.y + dy * t0, a.z + dz * t0 },
                        { a.x + dx * t1, a.y + dy * t1, a.z + dz * t1 }, col);
        }
    };

    // Per-call scratch reused across entities: memory peak O(largest mesh), no reallocation in
    // the loop.
    std::vector<DirectX::XMFLOAT3> worldScratch;

    // Decoded local-space slices, cached by "path#meshId": the decode is a pure function of
    // (blob, meshId) and costs real CPU per frame. The same file yields the same geometry, so
    // the cache stays valid across world reloads. Bounded by the unique collider meshes seen.
    struct CachedSlice
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<uint32_t>          indices;
    };
    static std::unordered_map<std::string, CachedSlice> s_sliceCache;

    // A hull's / an asset collision's triangles in the shape's own space (see
    // the branch below); an asset entry remembers the ModelCollision it came from.
    struct ShapeTriangles
    {
        std::vector<DirectX::XMFLOAT3>                  positions;
        std::vector<uint32_t>                           indices;
        std::shared_ptr<const Resource::ModelCollision> version;
    };
    static std::unordered_map<std::string, ShapeTriangles> s_shapeCache;
    // This frame's ModelCollision per model (ModelCollisionAt checks the file).
    std::unordered_map<std::string, std::shared_ptr<const Resource::ModelCollision>> modelVersion;

    uint32_t& seen = stats.seen;
    uint32_t& drewMesh = stats.meshes;
    uint32_t& drewPrim = stats.prims;
    uint32_t& skipped = stats.skipped;

    world.ForEach<ColliderComponent>([&](Entity e, ColliderComponent& c)
    {
        ++seen;
        const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
        if (!gt) { ++skipped; return; }
        if (cullByDistance)
        {
            const float dx = gt->matrix._41 - cameraPos.x;
            const float dy = gt->matrix._42 - cameraPos.y;
            const float dz = gt->matrix._43 - cameraPos.z;
            if (dx*dx + dy*dy + dz*dz > maxD2) { ++skipped; return; }
        }
        const uint32_t col    = colourFor(e, c);
        const bool     dashed = c.isTrigger && col != kWireSelected;
        auto drew = [&](uint32_t& counter)
        {
            ++counter;
            if (dashed) ++stats.triggers;
            if (col == kWireSelected) stats.selectedDrawn = true;
        };

        using namespace DirectX;
        const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
        // Mirror PhysicsSystem::MakeShape exactly. The Jolt shape is
        //   RotatedTranslatedShape(S_entity*offset, rot, ScaledShape(inner, S_entity))
        // on a body posed at the entity's scale-stripped world pos+rot, i.e.
        //   world = T_entity( R_entity( S_entity*offset + R_shape * (S_entity * p) ) )
        // so local shape coords take the entity world scale before the shape's own rotation, and
        // the offset (authored in entity-local units, like halfExtents / radius) is scaled per
        // axis too, because the RotatedTranslated wrap sits outside the ScaledShape. Decompose as
        // ReadEntityWorldPose does, so the wireframe cannot drift from the body.
        XMVECTOR vScale, vQuat, vTrans;
        if (!XMMatrixDecompose(&vScale, &vQuat, &vTrans, M)) { ++skipped; return; }
        XMFLOAT3 scl;
        XMStoreFloat3(&scl, vScale);

        const XMVECTOR shapeQ = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(c.rotationEulerDeg.x),
            XMConvertToRadians(c.rotationEulerDeg.y),
            XMConvertToRadians(c.rotationEulerDeg.z));
        const XMMATRIX shapeM =
              XMMatrixScalingFromVector(vScale)
            * XMMatrixRotationQuaternion(shapeQ)
            * XMMatrixTranslation(c.offset.x * scl.x, c.offset.y * scl.y, c.offset.z * scl.z)
            * XMMatrixRotationQuaternion(vQuat)
            * XMMatrixTranslationFromVector(vTrans);
        XMFLOAT4X4 shapeFM;
        XMStoreFloat4x4(&shapeFM, shapeM);
        const XMFLOAT3 shapeOrigin{ shapeFM._41, shapeFM._42, shapeFM._43 };
        // Radius scale for Sphere/Capsule: under non-uniform scale ScaledShape stretches them into
        // non-primitives (unsupported authoring: MakeShape asks for uniform scale); the max
        // component is the conservative single-radius approximation.
        const float maxScale =
            (std::max)({ std::fabs(scl.x), std::fabs(scl.y), std::fabs(scl.z) });

        // ---- Primitive colliders ----
        // Box / Sphere / Capsule through DebugWirePass's primitive helpers, placed by shapeM.
        if (c.shape == ColliderComponent::Shape::Box)
        {
            // Oriented box through shapeM: the actual physics box, with entity scale on the
            // half-extents and entity + shape rotation. A trigger's 12 edges are drawn one by one so
            // they can be dashed.
            if (!dashed) dbg.AddOBB(shapeFM, c.halfExtents, col);
            else
            {
                XMFLOAT3 corner[8];
                for (int k = 0; k < 8; ++k)
                    XMStoreFloat3(&corner[k], XMVector3Transform(
                        XMVectorSet((k & 1) ? c.halfExtents.x : -c.halfExtents.x,
                                    (k & 2) ? c.halfExtents.y : -c.halfExtents.y,
                                    (k & 4) ? c.halfExtents.z : -c.halfExtents.z, 1.f), shapeM));
                static constexpr int kEdge[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},
                                                      {4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
                for (const auto& ed : kEdge) line(corner[ed[0]], corner[ed[1]], col, true);
            }
            drew(drewPrim);
            return;
        }
        if (c.shape == ColliderComponent::Shape::Sphere)
        {
            // Radius carries the entity world scale, matching the ScaledShape
            // the body was built with (max component under non-uniform scale).
            // (A trigger sphere is coloured, not dashed: the helper draws rings.)
            dbg.AddSphere(shapeOrigin, c.radius * maxScale, col);
            drew(drewPrim);
            return;
        }
        if (c.shape == ColliderComponent::Shape::Capsule)
        {
            // Capsule axis along local +Y; endpoints go through shapeM, so they pick up entity scale
            // and both rotations. Radius scales by the XZ max: Jolt capsules are valid only under
            // uniform scale, where this is exact.
            XMVECTOR a = XMVectorSet(0, -c.halfHeight, 0, 1);
            XMVECTOR b = XMVectorSet(0,  c.halfHeight, 0, 1);
            a = XMVector3Transform(a, shapeM);
            b = XMVector3Transform(b, shapeM);
            XMFLOAT3 fa, fb;
            XMStoreFloat3(&fa, a);
            XMStoreFloat3(&fb, b);
            const float rScale = (std::max)(std::fabs(scl.x), std::fabs(scl.z));
            dbg.AddCapsule(fa, fb, c.radius * rScale, col);
            drew(drewPrim);
            return;
        }

        // ---- Convex hulls and asset collision: the shape's own triangles ----
        // A ConvexHull's stored indices are the synthetic fan the importer writes to keep the
        // entry decodable, not the hull, and a MeshAsset collider (a Mesh's BO / PR records, one
        // prim or a compound) has no slice at all. Both draw the triangles Jolt collides against:
        // PhysicsSystem::AppendColliderTriangles over the collider with offset / rotation zeroed
        // and an identity matrix, cached per shape, then placed by shapeM. A MeshAsset entry keeps
        // the ModelCollision it was built from; an edited model hands out a new one and the entry
        // is rebuilt.
        if (c.shape == ColliderComponent::Shape::ConvexHull || c.shape == ColliderComponent::Shape::MeshAsset)
        {
            ColliderComponent local = c;
            local.offset           = { 0.f, 0.f, 0.f };
            local.rotationEulerDeg = { 0.f, 0.f, 0.f };
            std::string key;
            std::shared_ptr<const Resource::ModelCollision> version;
            if (c.shape == ColliderComponent::Shape::MeshAsset)
            {
                if (local.assetModel.empty())   // = the entity's own StaticMesh, as PhysicsSystem reads it
                    if (const StaticMeshComponent* sm = world.GetComponent<StaticMeshComponent>(e))
                        local.assetModel = sm->model;
                if (local.assetModel.empty()) { ++skipped; return; }
                auto vIt = modelVersion.find(local.assetModel);   // one file check per model per frame
                if (vIt == modelVersion.end())
                    vIt = modelVersion.emplace(local.assetModel, Resource::ModelCollisionAt(local.assetModel)).first;
                version = vIt->second;
                key = "asset|" + local.assetModel + '#' + Resource::MeshKeyToHex(local.assetMeshKey);
            }
            else
            {
                if (c.meshLibPath.empty()) { ++skipped; return; }
                key = "hull|" + c.meshLibPath + '#' + std::to_string(c.meshLibMeshId);
            }
            auto it = s_shapeCache.find(key);
            if (it == s_shapeCache.end() || it->second.version != version)
            {
                ShapeTriangles st;
                st.version = version;
                physics.AppendColliderTriangles(local, XMMatrixIdentity(), st.positions, st.indices);
                it = s_shapeCache.insert_or_assign(key, std::move(st)).first;
            }
            const ShapeTriangles& tri = it->second;
            if (tri.indices.size() < 3) { ++skipped; return; }   // does not build: nothing to show
            worldScratch.resize(tri.positions.size());
            for (size_t i = 0; i < tri.positions.size(); ++i)
                XMStoreFloat3(&worldScratch[i], XMVector3Transform(
                    XMVectorSet(tri.positions[i].x, tri.positions[i].y, tri.positions[i].z, 1.f), shapeM));
            for (size_t i = 0; i + 2 < tri.indices.size(); i += 3)
            {
                const uint32_t i0 = tri.indices[i], i1 = tri.indices[i + 1], i2 = tri.indices[i + 2];
                if (i0 >= worldScratch.size() || i1 >= worldScratch.size() || i2 >= worldScratch.size()) continue;
                line(worldScratch[i0], worldScratch[i1], col, dashed);
                line(worldScratch[i1], worldScratch[i2], col, dashed);
                line(worldScratch[i2], worldScratch[i0], col, dashed);
                stats.triangleLines += 3;
            }
            if (c.shape == ColliderComponent::Shape::MeshAsset) ++stats.assets;
            drew(drewMesh);
            return;
        }
        if (c.shape != ColliderComponent::Shape::Mesh) { ++skipped; return; }

        // ---- Mesh trimesh wireframe ----
        // The collider's .meshlib slice: its triangles ARE the collision.
        if (c.meshLibPath.empty()) { ++skipped; return; }

        // Shared blob cache: populated on first request, reused by body creation, so later toggles
        // cost no I/O.
        const std::vector<uint8_t>* blob = physics.EnsureCollisionBlob(c.meshLibPath);
        if (!blob) { ++skipped; return; }

        // Decode the slice once (keyed by path#meshId); later frames reuse the cached local-space
        // geometry.
        const std::string key = c.meshLibPath + '#' + std::to_string(c.meshLibMeshId);
        auto cacheIt = s_sliceCache.find(key);
        if (cacheIt == s_sliceCache.end())
        {
            CachedSlice cs;
            const std::string ext = ToLowerExt(Sys::U8Path(c.meshLibPath));
            bool ok = false;
            if      (ext == ".meshlib") ok = DecodeMeshLibEntry(*blob, c.meshLibMeshId,
                                                                  cs.positions, cs.indices);
            else if (ext == ".imsh")    ok = DecodeImsh(*blob, cs.positions, cs.indices);
            if (!ok || cs.positions.empty() || cs.indices.size() < 3) { ++skipped; return; }
            cacheIt = s_sliceCache.emplace(key, std::move(cs)).first;
        }
        const CachedSlice& slice = cacheIt->second;

        // shapeM (scale -> shape rot -> offset -> entity rot/pos, MakeShape's wrap order) from
        // above, so entity scale and c.offset apply the same way to every shape kind.
        worldScratch.resize(slice.positions.size());
        for (size_t i = 0; i < slice.positions.size(); ++i)
        {
            XMVECTOR v = XMVectorSet(slice.positions[i].x,
                                      slice.positions[i].y,
                                      slice.positions[i].z, 1.f);
            v = XMVector3Transform(v, shapeM);
            XMStoreFloat3(&worldScratch[i], v);
        }

        for (size_t i = 0; i + 2 < slice.indices.size(); i += 3)
        {
            const uint32_t i0 = slice.indices[i+0];
            const uint32_t i1 = slice.indices[i+1];
            const uint32_t i2 = slice.indices[i+2];
            if (i0 >= worldScratch.size() || i1 >= worldScratch.size() || i2 >= worldScratch.size())
                continue;
            line(worldScratch[i0], worldScratch[i1], col, dashed);
            line(worldScratch[i1], worldScratch[i2], col, dashed);
            line(worldScratch[i2], worldScratch[i0], col, dashed);
            stats.triangleLines += 3;
        }
        drew(drewMesh);
    });

    // Logged only when the collider count changes, so missing entities are explainable
    // without spamming the log.
    static uint32_t s_lastSeen = 0;
    if (seen != s_lastSeen)
    {
        LOG_INFO("CollisionWireframe: scanned %u entities — drew %u triangle shape(s) (%u asset) + %u primitive "
                 "(Box/Sphere/Capsule), %u trigger(s) dashed, skipped %u",
                 seen, drewMesh, stats.assets, drewPrim, stats.triggers, skipped);
        s_lastSeen = seen;
    }
}

} // namespace Tools::CollisionMesh
