#pragma once

// Shape building for PhysicsSystem: the collision sources (.imsh / .meshlib / a model's BO / PR records)
// decoded into Jolt shapes, cached by content (path + modification stamp + entry), and MakeShape, which
// turns a ColliderComponent into the shape of its body. Internal to src/Physics.

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include "Physics/PhysicsSystem.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct ColliderComponent;

namespace DX12Physics
{
    // The caches the builders take by reference: PhysicsSystem::Impl is a private nested type.
    using MeshShapeCache = std::unordered_map<std::string, JPH::ShapeRefC>;
    using BlobCache      = std::unordered_map<std::string, std::vector<uint8_t>>;
    using StampCache     = std::unordered_map<std::string, std::int64_t>;

    inline JPH::Vec3 ToJolt(const DirectX::XMFLOAT3& v) { return JPH::Vec3(v.x, v.y, v.z); }
    inline JPH::Quat ToJoltQuat(const DirectX::XMFLOAT4& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }

    inline DirectX::XMFLOAT3 FromJolt(JPH::Vec3Arg v) { return { v.GetX(), v.GetY(), v.GetZ() }; }
    inline DirectX::XMFLOAT4 FromJolt(JPH::QuatArg q) { return { q.GetX(), q.GetY(), q.GetZ(), q.GetW() }; }

    // Positions + indices of a .imsh / .meshlib file from its bytes (the runtime MeshLibrary keeps GPU-only
    // buffers, so physics goes back to the source).
    bool DecodeImshPositions(const std::vector<uint8_t>& blob, std::vector<DirectX::XMFLOAT3>& outPositions,
                             std::vector<uint32_t>& outIndices);
    bool DecodeMeshLibSlice(const std::vector<uint8_t>& blob, uint32_t meshId,
                            std::vector<DirectX::XMFLOAT3>& outPositions, std::vector<uint32_t>& outIndices);

    // Modification stamp of a collision source, memoised per path (main thread only).
    std::int64_t SourceStamp(StampCache& stamps, const std::string& path);

    // Cache keys of a built shape: a .imsh / .meshlib entry, and a model's asset collision.
    std::string MeshShapeKey(StampCache& stamps, const std::string& path, std::uint32_t meshId, bool hull);
    std::string AssetShapeKey(StampCache& stamps, const std::string& model, std::uint64_t meshKey);

    // Builds (or looks up) the JPH::MeshShape of a .imsh / .meshlib collider; nullptr on failure.
    JPH::ShapeRefC GetOrBuildMeshShape(MeshShapeCache& cache, BlobCache& blobs, StampCache& stamps,
                                       const ColliderComponent& c);

    // The body prims of (model, meshKey) that read a library, as shape requests.
    void AssetLibraryPrims(const std::string& model, std::uint64_t meshKey,
                           std::vector<PhysicsSystem::MeshShapeRequest>& out);

    // A trimesh among the body prims: the body must be static.
    bool AssetNeedsStatic(const std::string& model, std::uint64_t meshKey);

    // The shape of a collider's body, scaled by @p scale; a failed mesh build falls back to a 0.5 m box.
    JPH::ShapeRefC MakeShape(MeshShapeCache& cache, BlobCache& blobs, StampCache& stamps,
                             const ColliderComponent& c, const DirectX::XMFLOAT3& scale);
}
