#pragma once
// Simple collision fitted to a point cloud: what the Mesh Editor's Collision > Add offers
// (box, sphere, capsule, k-DOP, convex hull - UE's Simplified Collision). Every result is in the
// points' own space: a Mesh's vertex space, where a PR record's c= / q= and a hull's points live.
// A fit never hands Jolt a shape it refuses: a box or k-DOP of a flat input keeps
// kMinHalfThickness (the asset collision builder rejects a zero half extent), a capsule's cylinder
// never collapses to zero (Jolt asserts on it), and a hull of coplanar points is thickened the same way.

#include "Resource/ModelCollision.h"   // CollisionPrim

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Physics::CollisionFit
{
    using Points = std::vector<DirectX::XMFLOAT3>;

    constexpr float kMinHalfThickness = 0.005f;   // metres; a flat input's half thickness
    constexpr int   kDefaultHullVertices = 64;     // a hull's vertex cap (Jolt allows 256)

    // The points' axis-aligned box. False on no points.
    bool FitBox(const Points& pts, Resource::CollisionPrim& out);
    // The smaller of two enclosing spheres: about the box centre and about the
    // points' mean (what UE's sphere fit does). False on no points.
    bool FitSphere(const Points& pts, Resource::CollisionPrim& out);
    // The tightest enclosing capsule along the box's longest axis, through the box
    // centre (radius from the points' distance to the axis, then the cylinder
    // just long enough for the caps to hold every point). False on no points.
    bool FitCapsule(const Points& pts, Resource::CollisionPrim& out);

    // k-DOP: the intersection of k/2 slabs, each pushed out to the points.
    // 10X / 10Y / 10Z = the box with the 4 edges along that axis bevelled;
    // 18 = every edge bevelled; 26 = every edge and every corner.
    enum class KDop { X10, Y10, Z10, K18, K26 };
    const char* KDopName(KDop k);                          // "10x", "10y", "10z", "18", "26"
    bool        ParseKDop(const std::string& s, KDop& out);
    // Its corners (a hull's points). False when the points span nothing.
    bool FitKDop(const Points& pts, KDop kind, Points& outCorners, std::string* outError = nullptr);

    // The convex hull of @p pts through Jolt's builder, as its vertices (what a
    // hull prim stores; Jolt builds the same hull from them at runtime). At most
    // @p maxVertices (4..256): past that the hull approximates the points.
    // @p outEdges (optional): the hull's edges, index pairs into @p outHull (a
    // polygon face's own edges - the wireframe an overlay draws).
    bool ConvexHull(const Points& pts, Points& outHull, int maxVertices = kDefaultHullVertices,
                    std::string* outError = nullptr, std::vector<std::uint32_t>* outEdges = nullptr);
}
