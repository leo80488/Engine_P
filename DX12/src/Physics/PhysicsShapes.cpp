// Shape building for PhysicsSystem (see PhysicsShapes.h).

#include "PhysicsShapes.h"

#include "ECS/PhysicsComponents.h"
#include "ECS/StaticMeshComponents.h"   // a MeshAsset collider with no model uses the entity's StaticMesh
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"
#include "Resource/ModelCollision.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <algorithm>
#include <cstring>
#include <filesystem>

namespace DX12Physics
{
// ==== Mesh-collider decode ====
// Reads the positions + indices of a .imsh / .meshlib file from its bytes: the runtime MeshLibrary
// keeps GPU-only buffers, so physics goes back to the source. Mirrors CollisionMeshBaker's
// primitives, kept inline so Physics does not depend on Tools.
bool DecodeImshPositions(const std::vector<uint8_t>& blob,
                                std::vector<DirectX::XMFLOAT3>& outPositions,
                                std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESH))
        return false;
    const auto* meta = Resource::GetMetadata<Resource::MeshMetadata>(blob.data());
    const uint32_t nv = meta->vertexCount;
    const uint32_t ni = meta->indexCount;
    const uint32_t vStride = meta->vertexStride;
    if (nv == 0 || ni == 0 || vStride < sizeof(float) * 3) return false;

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

bool DecodeMeshLibSlice(const std::vector<uint8_t>& blob, uint32_t meshId,
                               std::vector<DirectX::XMFLOAT3>& outPositions,
                               std::vector<uint32_t>&          outIndices)
{
    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_MESHLIB))
        return false;
    const auto* meta = Resource::GetMetadata<Resource::MeshLibraryMetadata>(blob.data());
    if (meshId >= meta->meshCount) return false;

    const uint8_t* payload = Resource::GetPayload(blob.data());
    const auto*    entries = reinterpret_cast<const Resource::MeshLibraryEntry*>(payload);
    const auto&    entry   = entries[meshId];
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

// Modification stamp of a collision source, memoised per path. Folded into the shape-cache key so
// the cache is content-addressed: re-cooking a level rewrites its collision .meshlib in place with
// a different entry order, and a path-only key would hand the next scene the previous bake's
// shapes. The stamp is what lets the shapes survive a Play -> Stop world clear (~39 ms of
// PrewarmMeshShapes per Play press otherwise).
//
// A mounted pak is immutable, so pak-served paths stamp to 0 and are never stat'ed. Memoised
// because the per-frame prewarm scan would stat once per collider (7000+ per frame on an imported
// level); the memo is dropped with the caches, so each invalidation re-stats each unique .meshlib
// once.
// MAIN THREAD ONLY: mutates the memo. Never call it from a ParallelFor body.
std::int64_t SourceStamp(StampCache& stamps, const std::string& path)
{
    if (path.empty()) return 0;
    if (auto it = stamps.find(path); it != stamps.end()) return it->second;

    std::int64_t stamp = 0;   // 0 = pak-served (immutable) or missing on disk
    if (!Resource::AssetFS::Get().HasInPak(path))
    {
        std::error_code ec;
        const std::string onDisk = Resource::AssetFS::Get().ResolveOnDisk(path);   // the file the load reads
        const auto t = std::filesystem::last_write_time(Sys::U8Path(onDisk.empty() ? path : onDisk), ec);
        if (!ec) stamp = static_cast<std::int64_t>(t.time_since_epoch().count());
    }
    stamps.emplace(path, stamp);
    return stamp;
}

// Cache key for a built shape. ConvexHull and Mesh can be requested for the SAME
// entry, so the kind is part of the key too.
std::string MeshShapeKey(StampCache& stamps, const std::string& path,
                                std::uint32_t meshId, bool hull)
{
    return (hull ? "hull|" : "") + path + '@'
         + std::to_string(SourceStamp(stamps, path)) + '#' + std::to_string(meshId);
}

// Builds (or looks up) the JPH::MeshShape of a .imsh / .meshlib collider; nullptr on failure.
JPH::ShapeRefC GetOrBuildMeshShape(MeshShapeCache& cache,
                                          BlobCache&      blobs,
                                          StampCache&     stamps,
                                          const ColliderComponent& c)
{
    if (c.meshLibPath.empty()) return nullptr;

    const std::string key = MeshShapeKey(stamps, c.meshLibPath, c.meshLibMeshId, false);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    // Reuse the file blob across every entity that points at this .meshlib: the first one pays the
    // read, the others decode the cached bytes.
    //
    // Every failure below is negative-cached (an empty ShapeRefC under the key), as in the hull and
    // asset builders: the per-frame prewarm scan counts a cached key as done, so an unreadable source
    // is not re-read and re-logged every frame. MakeShape still stands a 0.5 m box in for it; the
    // reason is logged once.
    auto blobIt = blobs.find(c.meshLibPath);
    if (blobIt == blobs.end())
    {
        std::vector<uint8_t> fresh;
        if (!Resource::AssetFS::Get().ReadFile(c.meshLibPath, fresh) || fresh.empty())
        {
            LOG_ERROR("PhysicsSystem: cannot read mesh collider source '%s'",
                      c.meshLibPath.c_str());
            cache.emplace(key, JPH::ShapeRefC());
            return nullptr;
        }
        blobIt = blobs.emplace(c.meshLibPath, std::move(fresh)).first;
    }
    const std::vector<uint8_t>& blob = blobIt->second;

    // File-extension dispatch, the renderer's convention: .meshlib = multi-mesh library, .imsh =
    // single mesh.
    std::string ext;
    {
        std::filesystem::path p = Sys::U8Path(c.meshLibPath);
        ext = Sys::PathToUtf8(p.extension());
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;
    bool decoded = false;
    if      (ext == ".meshlib") decoded = DecodeMeshLibSlice(blob, c.meshLibMeshId, positions, indices);
    else if (ext == ".imsh")    decoded = DecodeImshPositions(blob, positions, indices);
    if (!decoded || positions.empty() || indices.size() < 3 || indices.size() % 3 != 0)
    {
        LOG_ERROR("PhysicsSystem: failed to decode mesh collider source '%s' (mesh=%u)",
                  c.meshLibPath.c_str(), c.meshLibMeshId);
        cache.emplace(key, JPH::ShapeRefC());   // negative-cache; see above
        return nullptr;
    }

    // Jolt wants Float3 + IndexedTriangle arrays; material index 0 = the default physics material
    // (no per-triangle materials).
    JPH::VertexList         joltVerts;
    JPH::IndexedTriangleList joltTris;
    joltVerts.reserve(positions.size());
    for (const auto& p : positions)
        joltVerts.emplace_back(p.x, p.y, p.z);
    joltTris.reserve(indices.size() / 3);
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        // Skip degenerate triangles: Jolt asserts on them during the shape build.
        if (indices[i] == indices[i+1] || indices[i+1] == indices[i+2] || indices[i] == indices[i+2])
            continue;
        joltTris.emplace_back(indices[i], indices[i+1], indices[i+2], 0u);
    }
    if (joltTris.empty())
    {
        LOG_ERROR("PhysicsSystem: mesh collider '%s' has no valid triangles after dedup",
                  c.meshLibPath.c_str());
        cache.emplace(key, JPH::ShapeRefC());   // negative-cache; see above
        return nullptr;
    }

    JPH::MeshShapeSettings settings(std::move(joltVerts), std::move(joltTris));
    auto result = settings.Create();
    if (result.HasError())
    {
        LOG_ERROR("PhysicsSystem: Jolt MeshShape build failed for '%s': %s",
                  c.meshLibPath.c_str(), result.GetError().c_str());
        cache.emplace(key, JPH::ShapeRefC());   // negative-cache; see above
        return nullptr;
    }

    JPH::ShapeRefC shape = result.Get();
    cache.emplace(key, shape);
    LOG_INFO("PhysicsSystem: built MeshShape '%s' (mesh=%u, %zu verts, %zu tris)",
             c.meshLibPath.c_str(), c.meshLibMeshId,
             positions.size(), indices.size() / 3);
    return shape;
}

// Build (or look up) a JPH::ConvexHullShape from the same .meshlib entry a Mesh collider would
// use, from its vertices only; Jolt computes the hull. The Unreal UCX_ / KConvexElem equivalent:
// cheap queries, and unlike MeshShape legal on dynamic and kinematic bodies.
//
// Shares meshShapeCache with the trimesh path; the key's "hull|" prefix keeps a hull and a trimesh
// of the same entry apart.
static JPH::ShapeRefC GetOrBuildConvexShape(MeshShapeCache& cache,
                                            BlobCache&      blobs,
                                            StampCache&     stamps,
                                            const ColliderComponent& c)
{
    if (c.meshLibPath.empty()) return nullptr;

    const std::string key = MeshShapeKey(stamps, c.meshLibPath, c.meshLibMeshId, true);
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    auto blobIt = blobs.find(c.meshLibPath);
    if (blobIt == blobs.end())
    {
        std::vector<uint8_t> fresh;
        if (!Resource::AssetFS::Get().ReadFile(c.meshLibPath, fresh) || fresh.empty())
        {
            LOG_ERROR("PhysicsSystem: cannot read convex collider source '%s'",
                      c.meshLibPath.c_str());
            // Negative-cache: a whole imported level shares one collision library, so one bad path would
            // otherwise retry the pak lookup + file open for every body (thousands) with a LOG_ERROR each, and
            // the logger flushes on error batches - a multi-second stall.
            cache.emplace(key, JPH::ShapeRefC());
            return nullptr;
        }
        blobIt = blobs.emplace(c.meshLibPath, std::move(fresh)).first;
    }
    const std::vector<uint8_t>& blob = blobIt->second;

    std::string ext;
    {
        std::filesystem::path p = Sys::U8Path(c.meshLibPath);
        ext = Sys::PathToUtf8(p.extension());
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    std::vector<DirectX::XMFLOAT3> positions;
    std::vector<uint32_t>          indices;   // decoded but unused: a hull needs points only
    bool decoded = false;
    if      (ext == ".meshlib") decoded = DecodeMeshLibSlice(blob, c.meshLibMeshId, positions, indices);
    else if (ext == ".imsh")    decoded = DecodeImshPositions(blob, positions, indices);
    // A hull needs 4 non-coplanar points; Jolt's builder reports the rest.
    if (!decoded || positions.size() < 4)
    {
        LOG_ERROR("PhysicsSystem: failed to decode convex collider source '%s' (mesh=%u, %zu verts)",
                  c.meshLibPath.c_str(), c.meshLibMeshId, positions.size());
        cache.emplace(key, JPH::ShapeRefC());   // negative-cache; see above
        return nullptr;
    }

    JPH::Array<JPH::Vec3> points;
    points.reserve(positions.size());
    for (const auto& p : positions)
        points.emplace_back(p.x, p.y, p.z);

    JPH::ConvexHullShapeSettings settings(points);
    auto result = settings.Create();
    if (result.HasError())
    {
        LOG_ERROR("PhysicsSystem: Jolt ConvexHullShape build failed for '%s' (mesh=%u): %s",
                  c.meshLibPath.c_str(), c.meshLibMeshId, result.GetError().c_str());
        cache.emplace(key, JPH::ShapeRefC());   // negative-cache; see above
        return nullptr;
    }

    JPH::ShapeRefC shape = result.Get();
    cache.emplace(key, shape);
    LOG_INFO("PhysicsSystem: built ConvexHullShape '%s' (mesh=%u, %zu input points)",
             c.meshLibPath.c_str(), c.meshLibMeshId, positions.size());
    return shape;
}

// ==== Asset collision ====
// A Mesh's asset collision: the body prims of its model's BO / PR records
// (Resource/ModelCollision.h) as one shape - the prim itself when there is only one (wrapped only
// when it is off the origin), else a StaticCompoundShape. Hull / mesh prims come from the same
// caches as the Mesh / ConvexHull colliders (entry by SectionKey, MeshLibKeyTableAt). The key
// carries the model's stamp; a failure is cached too and logged once, and the caller builds no
// body for it (never the 0.5 m stand-in box).
std::string AssetShapeKey(StampCache& stamps, const std::string& model, std::uint64_t meshKey)
{
    return "asset|" + model + '@' + std::to_string(SourceStamp(stamps, model)) + '#' +
           Resource::MeshKeyToHex(meshKey);
}

// The body prims of (model, meshKey) that read a library: (resolved path, entry, hull).
void AssetLibraryPrims(const std::string& model, std::uint64_t meshKey,
                              std::vector<PhysicsSystem::MeshShapeRequest>& out)
{
    const auto mc = Resource::ModelCollisionAt(model);
    const Resource::MeshBodySetup* setup = mc->Find(meshKey);
    if (!setup || !setup->HasBody()) return;
    for (const Resource::CollisionPrim& p : setup->prims)
    {
        if (!p.InBody() || (p.type != Resource::CollisionPrim::Type::Hull &&
                            p.type != Resource::CollisionPrim::Type::Mesh))
            continue;
        const std::string lib   = Resource::ResolveModelRelative(model, p.lib);
        const auto        table = Resource::MeshLibKeyTableAt(lib);
        const int         entry = table ? table->Find(p.sectionKey) : -1;
        if (entry >= 0)
            out.push_back({ lib, static_cast<std::uint32_t>(entry),
                            p.type == Resource::CollisionPrim::Type::Hull });
    }
}

// A trimesh among the body prims: the body must be static (Jolt's MeshShape).
bool AssetNeedsStatic(const std::string& model, std::uint64_t meshKey)
{
    const auto mc = Resource::ModelCollisionAt(model);
    const Resource::MeshBodySetup* setup = mc->Find(meshKey);
    if (!setup) return false;
    return std::any_of(setup->prims.begin(), setup->prims.end(), [](const Resource::CollisionPrim& p)
                       { return p.InBody() && p.type == Resource::CollisionPrim::Type::Mesh; });
}

static JPH::ShapeRefC GetOrBuildAssetShape(MeshShapeCache& cache, BlobCache& blobs, StampCache& stamps,
                                           const std::string& model, std::uint64_t meshKey)
{
    using namespace JPH;
    if (model.empty()) return nullptr;
    const std::string key = AssetShapeKey(stamps, model, meshKey);
    if (auto it = cache.find(key); it != cache.end()) return it->second;   // may be the failure entry
    auto fail = [&](const std::string& why) -> ShapeRefC
    {
        LOG_ERROR("PhysicsSystem: the asset collision of mesh %s in '%s' does not build: %s - no body",
                  Resource::MeshKeyToHex(meshKey).c_str(), model.c_str(), why.c_str());
        cache.emplace(key, ShapeRefC());
        return nullptr;
    };

    const auto mc = Resource::ModelCollisionAt(model);
    const Resource::MeshBodySetup* setup = mc->Find(meshKey);
    if (!setup) return fail("the model has no BO / PR records for it");
    if (!setup->HasBody()) return fail("no body prim (complexity=none, or only its own sections' triangles)");

    struct Sub { ShapeRefC shape; Vec3 pos; Quat rot; };
    std::vector<Sub> subs;
    for (const Resource::CollisionPrim& p : setup->prims)
    {
        if (!p.InBody()) continue;
        const std::string label = p.name.empty() ? std::to_string(subs.size()) : p.name;
        ShapeRefC sub;
        switch (p.type)
        {
        case Resource::CollisionPrim::Type::Box:
            if (!(p.halfExtents.x > 0.f && p.halfExtents.y > 0.f && p.halfExtents.z > 0.f))
                return fail("box '" + label + "' has a non-positive half extent");
            sub = new BoxShape(Vec3(p.halfExtents.x, p.halfExtents.y, p.halfExtents.z));
            break;
        case Resource::CollisionPrim::Type::Sphere:
            if (!(p.radius > 0.f)) return fail("sphere '" + label + "' has a non-positive radius");
            sub = new SphereShape(p.radius);
            break;
        case Resource::CollisionPrim::Type::Capsule:
            if (!(p.radius > 0.f) || !(p.halfHeight > 0.f))
                return fail("capsule '" + label + "' has a non-positive size");
            sub = new CapsuleShape(p.halfHeight, p.radius);
            break;
        case Resource::CollisionPrim::Type::Hull:
        case Resource::CollisionPrim::Type::Mesh:
        {
            const std::string lib   = Resource::ResolveModelRelative(model, p.lib);
            const auto        table = Resource::MeshLibKeyTableAt(lib);
            const int         entry = table ? table->Find(p.sectionKey) : -1;
            if (entry < 0)
                return fail("prim '" + label + "': section " + Resource::MeshKeyToHex(p.sectionKey) + " is not in '" +
                            lib + "'");
            ColliderComponent src;
            src.meshLibPath   = lib;
            src.meshLibMeshId = static_cast<std::uint32_t>(entry);
            sub = (p.type == Resource::CollisionPrim::Type::Hull) ? GetOrBuildConvexShape(cache, blobs, stamps, src)
                                                                  : GetOrBuildMeshShape(cache, blobs, stamps, src);
            if (!sub) return fail("prim '" + label + "' (" + lib + "#" + std::to_string(entry) + ") did not build");
            break;
        }
        }
        Quat rot(p.rotation.x, p.rotation.y, p.rotation.z, p.rotation.w);
        rot = rot.IsNormalized() ? rot : (rot.LengthSq() > 1e-12f ? rot.Normalized() : Quat::sIdentity());
        subs.push_back({ sub, Vec3(p.center.x, p.center.y, p.center.z), rot });
    }

    ShapeRefC out;
    if (subs.size() == 1)
    {
        const Sub& s = subs.front();
        const bool origin = s.pos.IsNearZero() && s.rot.IsClose(Quat::sIdentity());
        out = origin ? s.shape : ShapeRefC(new RotatedTranslatedShape(s.pos, s.rot, s.shape));
    }
    else
    {
        StaticCompoundShapeSettings compound;
        for (const Sub& s : subs) compound.AddShape(s.pos, s.rot, s.shape);
        auto result = compound.Create();
        if (result.HasError()) return fail(std::string("StaticCompoundShape: ") + result.GetError().c_str());
        out = result.Get();
    }
    cache.emplace(key, out);
    LOG_INFO("PhysicsSystem: built the asset collision of mesh %s in '%s' (%zu prim(s))",
             Resource::MeshKeyToHex(meshKey).c_str(), model.c_str(), subs.size());
    return out;
}

JPH::ShapeRefC MakeShape(MeshShapeCache& cache,
                                BlobCache&      blobs,
                                StampCache&     stamps,
                                const ColliderComponent& c,
                                const DirectX::XMFLOAT3& scale)
{
    using namespace JPH;
    ShapeRefC inner;
    switch (c.shape)
    {
        case ColliderComponent::Shape::Box:
            inner = new BoxShape(ToJolt(c.halfExtents));
            break;
        case ColliderComponent::Shape::Sphere:
            inner = new SphereShape(c.radius);
            break;
        case ColliderComponent::Shape::Capsule:
            inner = new CapsuleShape(c.halfHeight, c.radius);
            break;
        case ColliderComponent::Shape::Mesh:
        {
            ShapeRefC mesh = GetOrBuildMeshShape(cache, blobs, stamps, c);
            inner = mesh ? mesh : ShapeRefC(new BoxShape(Vec3::sReplicate(0.5f)));
            break;
        }
        case ColliderComponent::Shape::ConvexHull:
        {
            ShapeRefC hull = GetOrBuildConvexShape(cache, blobs, stamps, c);
            inner = hull ? hull : ShapeRefC(new BoxShape(Vec3::sReplicate(0.5f)));
            break;
        }
        case ColliderComponent::Shape::MeshAsset:
            // Nothing stands in for an asset collision that did not build: the caller makes no body (the
            // failure was logged once).
            inner = GetOrBuildAssetShape(cache, blobs, stamps, c.assetModel, c.assetMeshKey);
            if (!inner) return nullptr;
            break;
    }
    if (!inner) inner = new BoxShape(Vec3::sReplicate(0.5f));   // an unknown shape value

    // Apply the entity's world scale to every collider shape (primitives and meshes) so the collider
    // tracks the visual: an unscaled 0.5 box on a 20x1x20 floor would be a 0.5 m box that bodies fall
    // through. ScaledShape asserts on unit scale in checked Jolt builds, so wrap only when scaled.
    // Non-uniform scale on a Capsule is degenerate in Jolt: give capsule colliders uniform scale.
    const bool unitScale =
        std::fabs(scale.x - 1.f) < 1e-4f &&
        std::fabs(scale.y - 1.f) < 1e-4f &&
        std::fabs(scale.z - 1.f) < 1e-4f;
    if (!unitScale)
        inner = ShapeRefC(new ScaledShape(inner, Vec3(scale.x, scale.y, scale.z)));

    // Apply the per-shape local offset + rotation. Jolt primitives are centred on the shape origin, so
    // without it a character capsule (mesh pivot at the feet) sinks half into the floor and a capsule
    // meant to lie on its side stands upright. Wrapping last composes correctly with the ScaledShape.
    //
    // The offset is authored in the entity's local space (the units of halfExtents / radius /
    // halfHeight), and the RotatedTranslatedShape sits outside the ScaledShape, so the offset carries
    // the entity scale per axis. Unscaled, a prefab authored at scale 1 and instanced at 0.1 would push
    // its capsule 10x too far, and a door leaf mirrored with sx = -1 would put its box outside the
    // doorway. Per-axis scaling is exactly ScaledShape-outside semantics for uniform and axis-aligned
    // scales (the only ones Jolt accepts in that order), and keeps the Inspector's "Snap to Bottom"
    // (offset.y = halfHeight + radius) true at every scale.
    const Vec3 scaledOffset(c.offset.x * scale.x,
                            c.offset.y * scale.y,
                            c.offset.z * scale.z);
    const bool zeroOffset =
        std::fabs(scaledOffset.GetX()) < 1e-6f &&
        std::fabs(scaledOffset.GetY()) < 1e-6f &&
        std::fabs(scaledOffset.GetZ()) < 1e-6f;
    const bool zeroRotation =
        std::fabs(c.rotationEulerDeg.x) < 1e-6f &&
        std::fabs(c.rotationEulerDeg.y) < 1e-6f &&
        std::fabs(c.rotationEulerDeg.z) < 1e-6f;
    if (zeroOffset && zeroRotation) return inner;

    Quat rot = Quat::sIdentity();
    if (!zeroRotation)
    {
        using namespace DirectX;
        const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(c.rotationEulerDeg.x),
            XMConvertToRadians(c.rotationEulerDeg.y),
            XMConvertToRadians(c.rotationEulerDeg.z));
        XMFLOAT4 qf; XMStoreFloat4(&qf, q);
        rot = Quat(qf.x, qf.y, qf.z, qf.w);
    }

    return new RotatedTranslatedShape(scaledOffset, rot, inner);
}
}
