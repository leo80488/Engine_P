// PhysicsSystem queries: rays (closest and all hits), the broad-phase sphere overlap, the exact box / capsule
// overlaps and the box / capsule / sphere sweeps. They read the Jolt world through the pImpl and never change it.
// Every query takes a QueryFilter (layers, the trigger switch, one body or one entity to skip); the LayerMask
// overloads are the same query with only the mask set. A hit carries the entity and the generation it had when its
// body was built, so a result for a body whose entity died and whose id was recycled is recognisable.

#include "PhysicsInternal.h"
#include "Physics/PhysicsSystem.h"

#include "ECS/ECS.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <DirectXMath.h>

#include <algorithm>

namespace DX12Physics
{
namespace
{
    // The body gate every query shares: one body, every body of one entity (its rigid body and its
    // character's query body), and the trigger switch (a body built as a sensor).
    class QueryBodyFilter final : public JPH::BodyFilter
    {
    public:
        explicit QueryBodyFilter(const QueryFilter& filter)
            : m_filter(filter), m_ignore(filter.ignoreBodyId) {}
        bool ShouldCollide(const JPH::BodyID& id) const override
        {
            return m_ignore.IsInvalid() || id != m_ignore;
        }
        bool ShouldCollideLocked(const JPH::Body& body) const override
        {
            return Accepts(m_filter, body);
        }
        static bool Accepts(const QueryFilter& filter, const JPH::Body& body)
        {
            if (filter.triggers == TriggerMode::Exclude && body.IsSensor()) return false;
            if (filter.triggers == TriggerMode::Only && !body.IsSensor()) return false;
            return filter.ignoreEntity == NullEntity ||
                   BodyUserEntity(body.GetUserData()) != filter.ignoreEntity;
        }
    private:
        const QueryFilter& m_filter;
        JPH::BodyID        m_ignore;
    };

    // direction * length as a Jolt vector; false for a zero direction.
    bool ScaledDirection(const DirectX::XMFLOAT3& direction, float length, JPH::Vec3& out)
    {
        using namespace DirectX;
        XMVECTOR dirV = XMLoadFloat3(&direction);
        if (XMVectorGetX(XMVector3LengthSq(dirV)) < 1e-12f) return false;
        dirV = XMVector3Normalize(dirV);
        out = JPH::Vec3(XMVectorGetX(dirV) * length, XMVectorGetY(dirV) * length, XMVectorGetZ(dirV) * length);
        return true;
    }

    JPH::Quat ToQuat(const DirectX::XMFLOAT4& rotation)
    {
        return JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w).Normalized();
    }

    void StampEntity(const JPH::Body& body, Entity& entity, std::uint32_t& generation)
    {
        const JPH::uint64 user = body.GetUserData();
        entity     = BodyUserEntity(user);   // NullEntity for a body no entity owns (editor-only)
        generation = BodyUserGeneration(user);
    }

    // A box with the edges rounded by Jolt's default convex radius (what a box body uses), never more than its
    // smallest half extent.
    JPH::Ref<JPH::Shape> MakeBox(const DirectX::XMFLOAT3& halfExtents)
    {
        const float minHalf = (std::min)({ halfExtents.x, halfExtents.y, halfExtents.z });
        if (!(minHalf > 0.f)) return nullptr;
        return JPH::Ref<JPH::Shape>(new JPH::BoxShape(JPH::Vec3(halfExtents.x, halfExtents.y, halfExtents.z),
                                                      (std::min)(JPH::cDefaultConvexRadius, minHalf)));
    }

    // Jolt asserts on a capsule with halfHeight <= 0 instead of degrading to a sphere, so the shape is picked here.
    JPH::Ref<JPH::Shape> MakeCapsule(float radius, float halfHeight)
    {
        if (!(radius > 0.f)) return nullptr;
        if (halfHeight > 1e-4f)
            return JPH::Ref<JPH::Shape>(new JPH::CapsuleShape(halfHeight, radius));
        return JPH::Ref<JPH::Shape>(new JPH::SphereShape(radius));
    }

    // Shared implementation of the sweeps: moves @p shape (at @p rotation) from @p from along
    // `direction * maxDistance` and returns the closest hit.
    ShapeCastHit DoShapeCast(JPH::PhysicsSystem& physics,
                             JPH::Ref<JPH::Shape>  shape,
                             const DirectX::XMFLOAT3& from,
                             const JPH::Quat&         rotation,
                             const DirectX::XMFLOAT3& direction,
                             float                    maxDistance,
                             const QueryFilter&       filter)
    {
        ShapeCastHit out;
        if (!shape || maxDistance <= 0.f) return out;

        JPH::Vec3 sweep;
        if (!ScaledDirection(direction, maxDistance, sweep)) return out;
        const JPH::Vec3 origin(from.x, from.y, from.z);

        // sFromWorldTransform expects the centre-of-mass transform; a primitive's COM is its local
        // origin, so rotation + translation works.
        const JPH::RMat44 startXform = JPH::RMat44::sRotationTranslation(rotation, origin);
        JPH::RShapeCast shapeCast = JPH::RShapeCast::sFromWorldTransform(
            shape, JPH::Vec3::sReplicate(1.f), startXform, sweep);

        JPH::ShapeCastSettings settings;
        // Back faces are ignored for triangles and convex shapes: a triangle hit from behind, or a sweep
        // that starts inside a convex body and moves out of it, is not reported.
        settings.mBackFaceModeTriangles  = JPH::EBackFaceMode::IgnoreBackFaces;
        settings.mBackFaceModeConvex     = JPH::EBackFaceMode::IgnoreBackFaces;
        settings.mUseShrunkenShapeAndConvexRadius = false;
        settings.mReturnDeepestPoint     = false;

        LayerMaskBPFilter bpFilter(filter.layerMask);   // skip the trees the mask cannot want
        LayerMaskFilter   layerFilter(filter.layerMask);
        QueryBodyFilter   bodyFilter(filter);

        JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
        physics.GetNarrowPhaseQuery().CastShape(
            shapeCast, settings, origin /*base offset*/, collector,
            bpFilter, layerFilter, bodyFilter);

        if (!collector.HadHit()) return out;
        const auto& hit = collector.mHit;

        out.hit      = true;
        out.fraction = hit.mFraction;
        out.distance = hit.mFraction * maxDistance;
        out.bodyId   = hit.mBodyID2.GetIndexAndSequenceNumber();

        // Jolt returns contact points relative to the CastShape base offset: add it back for world
        // coordinates (the ShapeCastHit / Raycast contract). The penetration axis points from body 1 into
        // body 2; negate and normalize it for the surface normal.
        const JPH::Vec3 cp = hit.mContactPointOn2 + origin;
        out.point  = { cp.GetX(), cp.GetY(), cp.GetZ() };
        JPH::Vec3 n = hit.mPenetrationAxis;
        if (n.LengthSq() > 1e-12f)
            n = -n.Normalized();
        else
            n = JPH::Vec3(0, 1, 0);
        out.normal = { n.GetX(), n.GetY(), n.GetZ() };

        JPH::BodyLockRead lock(physics.GetBodyLockInterface(), hit.mBodyID2);
        if (lock.Succeeded())
            StampEntity(lock.GetBody(), out.entity, out.generation);
        return out;
    }

    void FillRayHit(JPH::PhysicsSystem& physics, const JPH::RRayCast& ray, const JPH::RayCastResult& result,
                    float maxDistance, RayHit& out)
    {
        out.hit      = true;
        out.distance = result.mFraction * maxDistance;
        out.bodyId   = result.mBodyID.GetIndexAndSequenceNumber();
        const JPH::Vec3 hitPos = ray.GetPointOnRay(result.mFraction);
        out.point  = { hitPos.GetX(), hitPos.GetY(), hitPos.GetZ() };

        // Recover the normal and the owning entity under a brief body lock:
        // Body::GetWorldSpaceSurfaceNormal needs a live Body, and the read lock keeps it alive for the
        // scope (readers do not block each other).
        JPH::BodyLockRead lock(physics.GetBodyLockInterface(), result.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& body = lock.GetBody();
            const JPH::Vec3 n = body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hitPos);
            out.normal = { n.GetX(), n.GetY(), n.GetZ() };
            StampEntity(body, out.entity, out.generation);
        }
    }
}

// ==== Rays ====

RayHit PhysicsSystem::CastRayClosest(const DirectX::XMFLOAT3& from,
                                     const DirectX::XMFLOAT3& direction,
                                     float                    maxDistance,
                                     const QueryFilter&       filter) const
{
    RayHit out;
    if (!m_impl || maxDistance <= 0.f) return out;

    // Jolt's RRayCast direction carries both heading and length (the hit fraction is in [0, 1] of the
    // ray vector), so the direction is normalized and scaled by maxDistance: the caller need not
    // normalize it.
    JPH::Vec3 dir;
    if (!ScaledDirection(direction, maxDistance, dir)) return out;

    // The default LAYER_MASK_DEFAULT excludes the query-only layers (HurtBox / AttackBox / Sensing),
    // so gameplay rays do not hit invisible trigger volumes; LAYER_MASK_ALL includes them.
    JPH::RRayCast       ray(JPH::Vec3(from.x, from.y, from.z), dir);
    JPH::RayCastResult  hit;
    LayerMaskBPFilter   bpFilter(filter.layerMask);
    LayerMaskFilter     layerFilter(filter.layerMask);
    QueryBodyFilter     bodyFilter(filter);
    if (!m_impl->physics->GetNarrowPhaseQuery().CastRay(ray, hit, bpFilter, layerFilter, bodyFilter))
        return out;

    FillRayHit(*m_impl->physics, ray, hit, maxDistance, out);
    return out;
}

RayHit PhysicsSystem::CastRayClosest(const DirectX::XMFLOAT3& from,
                                     const DirectX::XMFLOAT3& direction,
                                     float                    maxDistance,
                                     LayerMask                layerMask) const
{
    QueryFilter filter;
    filter.layerMask = layerMask;
    return CastRayClosest(from, direction, maxDistance, filter);
}

std::size_t PhysicsSystem::CastRayAll(const DirectX::XMFLOAT3& from,
                                      const DirectX::XMFLOAT3& direction,
                                      float                    maxDistance,
                                      std::vector<RayHit>&     out,
                                      const QueryFilter&       filter,
                                      std::size_t              maxResults) const
{
    if (!m_impl || maxDistance <= 0.f || maxResults == 0) return 0;
    JPH::Vec3 dir;
    if (!ScaledDirection(direction, maxDistance, dir)) return 0;

    JPH::RRayCast ray(JPH::Vec3(from.x, from.y, from.z), dir);
    JPH::RayCastSettings settings;
    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    LayerMaskBPFilter bpFilter(filter.layerMask);
    LayerMaskFilter   layerFilter(filter.layerMask);
    QueryBodyFilter   bodyFilter(filter);
    m_impl->physics->GetNarrowPhaseQuery().CastRay(ray, settings, collector, bpFilter, layerFilter, bodyFilter);
    collector.Sort();   // nearest first

    // One hit per body: a trimesh or a compound reports every sub-shape the ray crosses, the nearest is
    // the body's hit.
    const std::size_t before = out.size();
    for (const JPH::RayCastResult& r : collector.mHits)
    {
        if (out.size() - before >= maxResults) break;
        const std::uint32_t id = r.mBodyID.GetIndexAndSequenceNumber();
        bool seen = false;
        for (std::size_t i = before; i < out.size() && !seen; ++i)
            seen = out[i].bodyId == id;
        if (seen) continue;
        RayHit hit;
        FillRayHit(*m_impl->physics, ray, r, maxDistance, hit);
        out.push_back(hit);
    }
    return out.size() - before;
}

// ==== Broad-phase overlap ====

std::size_t PhysicsSystem::OverlapSphere(const DirectX::XMFLOAT3&  center,
                                         float                     radius,
                                         std::vector<EntityHandle>& out,
                                         const QueryFilter&        filter,
                                         std::size_t               maxResults) const
{
    if (!m_impl || radius <= 0.f || maxResults == 0) return 0;

    // AllHitCollisionCollector, not a closest-hit one: this query wants the whole set. The collector
    // has no cap, so the cap applies when copying out - a 500-agent scene with a 30 m perception
    // radius can collect hundreds of bodies.
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    LayerMaskFilter layerFilter(filter.layerMask);
    LayerMaskBPFilter bpFilter(filter.layerMask);
    m_impl->physics->GetBroadPhaseQuery().CollideSphere(
        JPH::Vec3(center.x, center.y, center.z), radius, collector,
        bpFilter, layerFilter);

    const std::size_t before = out.size();
    // One lock interface for the loop. Locking each body is Jolt's pattern for reading user data, and
    // read locks do not block other readers.
    const JPH::BodyLockInterface& lockIface = m_impl->physics->GetBodyLockInterface();
    for (const JPH::BodyID& id : collector.mHits)
    {
        if (out.size() - before >= maxResults) break;
        if (filter.ignoreBodyId == id.GetIndexAndSequenceNumber()) continue;
        JPH::BodyLockRead lock(lockIface, id);
        if (!lock.Succeeded()) continue;
        if (!QueryBodyFilter::Accepts(filter, lock.GetBody())) continue;
        EntityHandle h;
        StampEntity(lock.GetBody(), h.entity, h.generation);
        if (h.entity == NullEntity) continue;   // editor-owned body, no entity stamped
        out.push_back(h);
    }
    return out.size() - before;
}

std::size_t PhysicsSystem::OverlapSphere(const DirectX::XMFLOAT3& center,
                                        float                    radius,
                                        std::vector<Entity>&     out,
                                        LayerMask                layerMask,
                                        std::size_t              maxResults) const
{
    std::vector<EntityHandle> handles;
    QueryFilter filter;
    filter.layerMask = layerMask;
    const std::size_t n = OverlapSphere(center, radius, handles, filter, maxResults);
    for (const EntityHandle& h : handles) out.push_back(h.entity);
    return n;
}

// ==== Exact overlap ====

std::size_t CollectMeleeOverlaps(JPH::PhysicsSystem& physics,
    JPH::ShapeRefC shape, const JPH::RMat44& com, std::vector<OverlapHit>& out,
    const QueryFilter& filter, std::size_t maxResults)
{
    JPH::CollideShapeSettings settings;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    LayerMaskBPFilter bpFilter(filter.layerMask);      // skip whole trees the mask cannot want
    LayerMaskFilter   layerFilter(filter.layerMask);   // exact per-body gate
    QueryBodyFilter   bodyFilter(filter);
    physics.GetNarrowPhaseQuery().CollideShape(
        shape, JPH::Vec3::sReplicate(1.0f), com, settings,
        JPH::RVec3::sZero(), collector, bpFilter, layerFilter, bodyFilter);

    const std::size_t before = out.size();
    const JPH::BodyLockInterface& lockIface = physics.GetBodyLockInterface();

    // One body can report many sub-shape contacts (a compound collider, a mesh). Damage wants one row
    // per entity, so keep the deepest contact - what a designer would call "the hit".
    for (const JPH::CollideShapeResult& hit : collector.mHits)
    {
        JPH::BodyLockRead lock(lockIface, hit.mBodyID2);
        if (!lock.Succeeded()) continue;
        Entity e; std::uint32_t generation;
        StampEntity(lock.GetBody(), e, generation);
        if (e == NullEntity) continue;      // editor-owned body, no entity stamped

        const JPH::Vec3 n = -hit.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero());

        OverlapHit* existing = nullptr;
        for (std::size_t i = before; i < out.size(); ++i)
            if (out[i].entity == e && out[i].generation == generation) { existing = &out[i]; break; }

        if (existing)
        {
            if (hit.mPenetrationDepth > existing->depth)
            {
                existing->depth  = hit.mPenetrationDepth;
                existing->point  = { hit.mContactPointOn2.GetX(),
                                     hit.mContactPointOn2.GetY(),
                                     hit.mContactPointOn2.GetZ() };
                existing->normal = { n.GetX(), n.GetY(), n.GetZ() };
            }
            continue;
        }

        if (out.size() - before >= maxResults) continue;   // cap NEW entities only
        OverlapHit oh;
        oh.entity     = e;
        oh.generation = generation;
        oh.depth  = hit.mPenetrationDepth;
        oh.point  = { hit.mContactPointOn2.GetX(),
                      hit.mContactPointOn2.GetY(),
                      hit.mContactPointOn2.GetZ() };
        oh.normal = { n.GetX(), n.GetY(), n.GetZ() };
        oh.bodyId = hit.mBodyID2.GetIndexAndSequenceNumber();
        out.push_back(oh);
    }
    return out.size() - before;
}

std::size_t CollectMeleeOverlaps(JPH::PhysicsSystem& physics,
    JPH::ShapeRefC shape, const JPH::RMat44& com, std::vector<OverlapHit>& out,
    LayerMask layerMask, std::size_t maxResults)
{
    QueryFilter filter;
    filter.layerMask = layerMask;
    return CollectMeleeOverlaps(physics, shape, com, out, filter, maxResults);
}

std::size_t PhysicsSystem::OverlapCapsuleAll(const DirectX::XMFLOAT3& center,
                                             const DirectX::XMFLOAT4& rotation,
                                             float                    radius,
                                             float                    halfHeight,
                                             std::vector<OverlapHit>& out,
                                             const QueryFilter&       filter,
                                             std::size_t              maxResults) const
{
    if (!m_impl || radius <= 0.f || maxResults == 0) return 0;

    JPH::Ref<JPH::Shape> shape = MakeCapsule(radius, halfHeight > 0.f ? halfHeight : 0.f);
    if (!shape) return 0;

    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(
        ToQuat(rotation), JPH::RVec3(center.x, center.y, center.z));

    return CollectMeleeOverlaps(*m_impl->physics, shape, com, out, filter, maxResults);
}

std::size_t PhysicsSystem::OverlapCapsuleAll(const DirectX::XMFLOAT3& center,
                                             const DirectX::XMFLOAT4& rotation,
                                             float                    radius,
                                             float                    halfHeight,
                                             std::vector<OverlapHit>& out,
                                             LayerMask                layerMask,
                                             std::size_t              maxResults) const
{
    QueryFilter filter;
    filter.layerMask = layerMask;
    return OverlapCapsuleAll(center, rotation, radius, halfHeight, out, filter, maxResults);
}

std::size_t PhysicsSystem::OverlapBoxAll(const DirectX::XMFLOAT3& center,
                                         const DirectX::XMFLOAT4& rotation,
                                         const DirectX::XMFLOAT3& halfExtents,
                                         std::vector<OverlapHit>& out,
                                         const QueryFilter&       filter,
                                         std::size_t              maxResults) const
{
    if (!m_impl || maxResults == 0) return 0;
    JPH::Ref<JPH::Shape> shape = MakeBox(halfExtents);
    if (!shape) return 0;

    const JPH::RMat44 com = JPH::RMat44::sRotationTranslation(
        ToQuat(rotation), JPH::RVec3(center.x, center.y, center.z));
    return CollectMeleeOverlaps(*m_impl->physics, shape, com, out, filter, maxResults);
}

// ==== Sweeps ====

ShapeCastHit PhysicsSystem::CastBoxClosest(const DirectX::XMFLOAT3& from,
                                           const DirectX::XMFLOAT4& rotation,
                                           const DirectX::XMFLOAT3& halfExtents,
                                           const DirectX::XMFLOAT3& direction,
                                           float                    maxDistance,
                                           const QueryFilter&       filter) const
{
    if (!m_impl) return {};
    return DoShapeCast(*m_impl->physics, MakeBox(halfExtents), from, ToQuat(rotation), direction, maxDistance,
                       filter);
}

ShapeCastHit PhysicsSystem::CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                               const DirectX::XMFLOAT4& rotation,
                                               const DirectX::XMFLOAT3& direction,
                                               float                    radius,
                                               float                    halfHeight,
                                               float                    maxDistance,
                                               const QueryFilter&       filter) const
{
    if (!m_impl) return {};
    return DoShapeCast(*m_impl->physics, MakeCapsule(radius, halfHeight), from, ToQuat(rotation), direction,
                       maxDistance, filter);
}

ShapeCastHit PhysicsSystem::CastCapsuleClosest(const DirectX::XMFLOAT3& from,
                                               const DirectX::XMFLOAT3& direction,
                                               float                    radius,
                                               float                    halfHeight,
                                               float                    maxDistance,
                                               std::uint32_t            ignoreBodyId,
                                               LayerMask                layerMask) const
{
    QueryFilter filter;
    filter.layerMask    = layerMask;
    filter.ignoreBodyId = ignoreBodyId;
    return CastCapsuleClosest(from, { 0.f, 0.f, 0.f, 1.f }, direction, radius, halfHeight, maxDistance, filter);
}

ShapeCastHit PhysicsSystem::CastSphereClosest(const DirectX::XMFLOAT3& from,
                                              const DirectX::XMFLOAT3& direction,
                                              float                    radius,
                                              float                    maxDistance,
                                              std::uint32_t            ignoreBodyId,
                                              LayerMask                layerMask) const
{
    if (!m_impl || radius <= 0.f) return {};
    QueryFilter filter;
    filter.layerMask    = layerMask;
    filter.ignoreBodyId = ignoreBodyId;
    JPH::Ref<JPH::Shape> shape(new JPH::SphereShape(radius));
    return DoShapeCast(*m_impl->physics, shape, from, JPH::Quat::sIdentity(), direction, maxDistance, filter);
}

} // namespace DX12Physics
