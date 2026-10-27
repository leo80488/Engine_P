#pragma once

// SceneBVH - binary bounding volume hierarchy for CPU frustum culling.
//
// Two leaf types:
//   - Static mesh:  AABB from LocalAabb + GlobalTransform (stable, good for SAH)
//   - Skinned mesh: AABB from bone positions (changes every frame, leaf refit only)
//
// Usage:
//   Frame 0:     Build(entities, aabbs, types, count) - full SAH rebuild
//   Frame 1..N:  RefitLeaf(entity, newAabb)           - update individual leaf AABBs
//                RefitInternal()                      - bottom-up refit of node bounds
//   Periodic:    Build(...)                           - full rebuild when topology changes
//
// FrustumCull() returns visible entity IDs via BoundingFrustum intersection.

#include "ECS/ECS.h"
#include "Graphics/RenderTypes.h"
#include <DirectXMath.h>
#include <DirectXCollision.h>
#include <vector>
#include <unordered_map>

class SceneBVH
{
public:
    // Leaf type: determines the per-frame update strategy.
    //   Static  - AABB changes only when the entity moves (transform dirty).
    //   Skinned - AABB changes every frame (bone animation).
    enum class LeafType : uint8_t { Static = 0, Skinned = 1 };

    struct AABB
    {
        DirectX::XMFLOAT3 min = {  1e30f,  1e30f,  1e30f };
        DirectX::XMFLOAT3 max = { -1e30f, -1e30f, -1e30f };

        void Expand(const AABB& other)
        {
            min.x = (std::min)(min.x, other.min.x);
            min.y = (std::min)(min.y, other.min.y);
            min.z = (std::min)(min.z, other.min.z);
            max.x = (std::max)(max.x, other.max.x);
            max.y = (std::max)(max.y, other.max.y);
            max.z = (std::max)(max.z, other.max.z);
        }

        void Reset()
        {
            min = {  1e30f,  1e30f,  1e30f };
            max = { -1e30f, -1e30f, -1e30f };
        }

        float SurfaceArea() const
        {
            float dx = max.x - min.x;
            float dy = max.y - min.y;
            float dz = max.z - min.z;
            return 2.0f * (dx*dy + dy*dz + dz*dx);
        }

        DirectX::XMFLOAT3 Center() const
        {
            return { (min.x+max.x)*0.5f, (min.y+max.y)*0.5f, (min.z+max.z)*0.5f };
        }
    };

    // Public so the parallel cull helper in SceneBVH.cpp can reference it; a private type
    // would force that helper into the class or into a friend declaration.
    struct Node
    {
        AABB     bounds;
        uint32_t left    = 0;
        uint32_t right   = 0;
        uint32_t first   = 0;     // first leaf index (leaf only)
        uint32_t count   = 0;     // leaf count (0 = internal)
        uint32_t parent  = ~0u;   // parent node index (for bottom-up refit)
    };

    // Full SAH rebuild from scratch; call when entities are added / removed. @p types is
    // optional: null -> every leaf is Static.
    void Build(const Entity* entities, const AABB* aabbs, const LeafType* types, uint32_t count);

    // Update a single leaf's AABB by entity ID (for skinned mesh refit).
    void RefitLeaf(Entity entity, const AABB& newAabb);

    // A leaf OUTSIDE the tree: a skinned part a runtime spawn just added. RefitLeaf /
    // TryGetLeafAabb / GetLeafType treat it like any leaf, every query tests it linearly
    // after the tree, and the next Build() takes it into the tree. It saves a full SAH
    // rebuild for the few parts a spawn adds (parts refit every frame anyway). The caller
    // keeps the count small (GetLooseLeafCount) and rebuilds past its limit.
    void AddLooseLeaf(Entity entity, const AABB& aabb, LeafType type);
    uint32_t GetLooseLeafCount() const
    { return static_cast<uint32_t>(m_leafEntities.size()) - m_treeLeafCount; }

    // Bottom-up refit all internal node bounds after leaf changes.
    void RefitInternal();

    // Query leaf type by entity ID.
    LeafType GetLeafType(Entity entity) const;

    // This frame's world AABB for one entity, or false when the entity has no leaf (the
    // same two-array-load lookup RefitLeaf uses).
    // A secondary camera view uses it to re-sort the frame's TRANSPARENT draw packets
    // back-to-front against its own eye on the render half, where World is off limits: the
    // BVH is the only render-safe world position per entity, refit every frame for exactly
    // the geometry that gets drawn.
    bool TryGetLeafAabb(Entity entity, AABB& out) const
    {
        if (entity >= m_entityLeafSparse.size()) return false;
        const uint32_t leafIdx = m_entityLeafSparse[entity];
        if (leafIdx == kInvalidLeafIdx || leafIdx >= m_leafAABBs.size()) return false;
        out = m_leafAABBs[leafIdx];
        return true;
    }

    // Traverse and return visible entity IDs.
    void FrustumCull(const DirectX::BoundingFrustum& frustum, std::vector<Entity>& outVisible) const;
    void FrustumCull(const FrustumPlanes& frustum, std::vector<Entity>& outVisible) const;

    // AABB range query: returns every leaf whose bounds intersect @p box. The
    // reflection-probe capture uses it to limit per-probe draws to geometry inside the
    // probe's influence bounds (outer falloff AABB). Serial traversal only: probe bake is
    // amortized to one per frame, so a parallel dispatch would not pay back.
    void QueryAABB(const AABB& box, std::vector<Entity>& outVisible) const;

    uint32_t GetNodeCount()  const { return static_cast<uint32_t>(m_nodes.size()); }
    uint32_t GetLeafCount()  const { return static_cast<uint32_t>(m_leafEntities.size()); }
    bool     NeedsRebuild()  const { return m_needsRebuild; }
    void     MarkRebuild()         { m_needsRebuild = true; }

private:
    static constexpr uint32_t           kInvalidLeafIdx = ~0u;

    void AppendLooseHits(const DirectX::BoundingFrustum& frustum, std::vector<Entity>& out) const;
    void AppendLooseHits(const FrustumPlanes& frustum, std::vector<Entity>& out) const;

    uint32_t BuildRecursive(uint32_t begin, uint32_t end, uint32_t parentIdx);

    std::vector<Node>     m_nodes;
    std::vector<Entity>   m_leafEntities;
    std::vector<AABB>     m_leafAABBs;
    std::vector<LeafType> m_leafTypes;
    bool                  m_needsRebuild = true;
    // Leaves [0, m_treeLeafCount) are the tree's (Build); the rest are loose
    // (AddLooseLeaf), tested linearly by every query.
    uint32_t              m_treeLeafCount = 0;

    // Entity -> leaf index map for fast RefitLeaf lookup: a sparse array indexed directly
    // by Entity ID, two array loads per lookup instead of unordered_map's hash + bucket
    // walk. kInvalidLeafIdx means "entity not in BVH". Costs ~4 bytes per Entity ID, trivial
    // on Bistro-scale scenes.
    std::vector<uint32_t>               m_entityLeafSparse;
};
