// CollisionFit.cpp - see Physics/CollisionFit.h.

#include "Physics/CollisionFit.h"

#include <Jolt/Jolt.h>

JPH_SUPPRESS_WARNINGS

#include <Jolt/Geometry/ConvexHullBuilder.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Physics::CollisionFit
{
namespace
{
    using DirectX::XMFLOAT3;

    struct V3
    {
        float x = 0.f, y = 0.f, z = 0.f;
        V3() = default;
        V3(float a, float b, float c) : x(a), y(b), z(c) {}
        V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        V3 operator*(float s) const { return { x * s, y * s, z * s }; }
        float Dot(const V3& o) const { return x * o.x + y * o.y + z * o.z; }
        V3 Cross(const V3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
    };

    bool Bounds(const Points& pts, float lo[3], float hi[3])
    {
        if (pts.empty()) return false;
        for (int k = 0; k < 3; ++k) { lo[k] = FLT_MAX; hi[k] = -FLT_MAX; }
        for (const XMFLOAT3& p : pts)
        {
            const float v[3] = { p.x, p.y, p.z };
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = (std::min)(lo[k], v[k]);
                hi[k] = (std::max)(hi[k], v[k]);
            }
        }
        return true;
    }

    float Dist(const XMFLOAT3& a, const float c[3])
    {
        const float d[3] = { a.x - c[0], a.y - c[1], a.z - c[2] };
        return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }

    // The k-DOP's slab normals (unit), opposite pairs adjacent: [2i] and [2i+1].
    std::vector<V3> KDopDirections(KDop kind)
    {
        std::vector<V3> d = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        const float e = 1.f / std::sqrt(2.f), c = 1.f / std::sqrt(3.f);
        // The 4 edges along an axis are bevelled by the diagonals of the other two.
        auto edges = [&](int axis)
        {
            const int a = (axis + 1) % 3, b = (axis + 2) % 3;
            for (const float sa : { 1.f, -1.f })
                for (const float sb : { 1.f, -1.f })
                {
                    float v[3] = { 0.f, 0.f, 0.f };
                    v[a] = sa * e;
                    v[b] = sb * e;
                    // Its opposite comes right after it (-sa, -sb): emit the pair once.
                    if (sa < 0.f) continue;
                    d.push_back({ v[0], v[1], v[2] });
                    d.push_back({ -v[0], -v[1], -v[2] });
                }
        };
        switch (kind)
        {
        case KDop::X10: edges(0); break;
        case KDop::Y10: edges(1); break;
        case KDop::Z10: edges(2); break;
        case KDop::K18: edges(0); edges(1); edges(2); break;
        case KDop::K26:
            edges(0); edges(1); edges(2);
            for (const float sy : { 1.f, -1.f })
                for (const float sz : { 1.f, -1.f })
                {
                    d.push_back({ c, sy * c, sz * c });
                    d.push_back({ -c, -sy * c, -sz * c });
                }
            break;
        }
        return d;
    }

    void EnsureJoltAllocator()
    {
        // The engine's PhysicsSystem registers it at startup; a tool path that
        // never started physics still gets a hull.
        if (JPH::Allocate == nullptr) JPH::RegisterDefaultAllocator();
    }
}

bool FitBox(const Points& pts, Resource::CollisionPrim& out)
{
    float lo[3], hi[3];
    if (!Bounds(pts, lo, hi)) return false;
    out             = Resource::CollisionPrim{};
    out.type        = Resource::CollisionPrim::Type::Box;
    out.center      = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
    out.halfExtents = { (std::max)(0.5f * (hi[0] - lo[0]), kMinHalfThickness),
                        (std::max)(0.5f * (hi[1] - lo[1]), kMinHalfThickness),
                        (std::max)(0.5f * (hi[2] - lo[2]), kMinHalfThickness) };
    return true;
}

bool FitSphere(const Points& pts, Resource::CollisionPrim& out)
{
    float lo[3], hi[3];
    if (!Bounds(pts, lo, hi)) return false;
    const float boxC[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
    double mean[3] = { 0.0, 0.0, 0.0 };
    for (const XMFLOAT3& p : pts) { mean[0] += p.x; mean[1] += p.y; mean[2] += p.z; }
    const float meanC[3] = { static_cast<float>(mean[0] / pts.size()), static_cast<float>(mean[1] / pts.size()),
                             static_cast<float>(mean[2] / pts.size()) };
    float r1 = 0.f, r2 = 0.f;
    for (const XMFLOAT3& p : pts)
    {
        r1 = (std::max)(r1, Dist(p, boxC));
        r2 = (std::max)(r2, Dist(p, meanC));
    }
    const float* c = (r2 < r1) ? meanC : boxC;
    out        = Resource::CollisionPrim{};
    out.type   = Resource::CollisionPrim::Type::Sphere;
    out.center = { c[0], c[1], c[2] };
    out.radius = (std::max)((std::min)(r1, r2), kMinHalfThickness);
    return true;
}

bool FitCapsule(const Points& pts, Resource::CollisionPrim& out)
{
    float lo[3], hi[3];
    if (!Bounds(pts, lo, hi)) return false;
    int axis = 1;   // Y (the capsule's own axis) unless another is longer
    if (hi[0] - lo[0] > hi[axis] - lo[axis]) axis = 0;
    if (hi[2] - lo[2] > hi[axis] - lo[axis]) axis = 2;
    const float c[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
    const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
    auto comp = [](const XMFLOAT3& p, int k) { return k == 0 ? p.x : (k == 1 ? p.y : p.z); };
    float r = 0.f;
    for (const XMFLOAT3& p : pts)
    {
        const float a = comp(p, o1) - c[o1], b = comp(p, o2) - c[o2];
        r = (std::max)(r, std::sqrt(a * a + b * b));
    }
    r = (std::max)(r, kMinHalfThickness);
    // A point at axial distance t and radial distance d is inside when
    // t <= hh + sqrt(r^2 - d^2).
    float hh = 0.f;
    for (const XMFLOAT3& p : pts)
    {
        const float a = comp(p, o1) - c[o1], b = comp(p, o2) - c[o2];
        const float t = std::fabs(comp(p, axis) - c[axis]);
        hh = (std::max)(hh, t - std::sqrt((std::max)(0.f, r * r - (a * a + b * b))));
    }
    out            = Resource::CollisionPrim{};
    out.type       = Resource::CollisionPrim::Type::Capsule;
    out.center     = { c[0], c[1], c[2] };
    out.radius     = r;
    out.halfHeight = (std::max)(hh, kMinHalfThickness);
    // The capsule runs along its local Y: turn Y onto the axis.
    const float s = std::sqrt(0.5f);
    if (axis == 0)      out.rotation = { 0.f, 0.f, -s, s };   // -90 deg about Z: Y -> X
    else if (axis == 2) out.rotation = { s, 0.f, 0.f, s };    // +90 deg about X: Y -> Z
    return true;
}

const char* KDopName(KDop k)
{
    switch (k)
    {
    case KDop::X10: return "10x";
    case KDop::Y10: return "10y";
    case KDop::Z10: return "10z";
    case KDop::K18: return "18";
    case KDop::K26: return "26";
    }
    return "?";
}

bool ParseKDop(const std::string& s, KDop& out)
{
    for (KDop k : { KDop::X10, KDop::Y10, KDop::Z10, KDop::K18, KDop::K26 })
        if (s == KDopName(k)) { out = k; return true; }
    return false;
}

bool FitKDop(const Points& pts, KDop kind, Points& outCorners, std::string* outError)
{
    outCorners.clear();
    float lo[3], hi[3];
    if (!Bounds(pts, lo, hi))
    {
        if (outError) *outError = "no points";
        return false;
    }
    const float extent = (std::max)({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], kMinHalfThickness });
    const std::vector<V3> dirs = KDopDirections(kind);
    std::vector<float> dist(dirs.size(), -FLT_MAX);
    for (const XMFLOAT3& p : pts)
        for (size_t i = 0; i < dirs.size(); ++i)
            dist[i] = (std::max)(dist[i], dirs[i].Dot({ p.x, p.y, p.z }));
    // Every slab at least 2 * kMinHalfThickness thick (a flat input stays a solid).
    for (size_t i = 0; i + 1 < dirs.size(); i += 2)
    {
        const float width = dist[i] + dist[i + 1];
        if (width < 2.f * kMinHalfThickness)
        {
            const float grow = 0.5f * (2.f * kMinHalfThickness - width);
            dist[i] += grow;
            dist[i + 1] += grow;
        }
    }
    // The corners: every three planes' meeting point that is inside all of them.
    const float eps = 1e-5f * extent + 1e-7f;
    for (size_t i = 0; i < dirs.size(); ++i)
        for (size_t j = i + 1; j < dirs.size(); ++j)
            for (size_t k = j + 1; k < dirs.size(); ++k)
            {
                const V3  jk  = dirs[j].Cross(dirs[k]);
                const float det = dirs[i].Dot(jk);
                if (std::fabs(det) < 1e-4f) continue;
                const V3 x = (jk * dist[i] + dirs[k].Cross(dirs[i]) * dist[j] + dirs[i].Cross(dirs[j]) * dist[k]) *
                             (1.f / det);
                bool inside = true;
                for (size_t l = 0; l < dirs.size() && inside; ++l)
                    inside = dirs[l].Dot(x) <= dist[l] + eps;
                if (!inside) continue;
                const bool dup = std::any_of(outCorners.begin(), outCorners.end(), [&](const XMFLOAT3& q)
                {
                    const V3 d = V3(q.x, q.y, q.z) - x;
                    return d.Dot(d) <= eps * eps * 100.f;
                });
                if (!dup) outCorners.push_back({ x.x, x.y, x.z });
            }
    if (outCorners.size() < 4)
    {
        if (outError) *outError = "the k-DOP has no volume";
        return false;
    }
    return true;
}

bool ConvexHull(const Points& pts, Points& outHull, int maxVertices, std::string* outError,
                std::vector<std::uint32_t>* outEdges)
{
    outHull.clear();
    if (outEdges) outEdges->clear();
    auto fail = [&](const std::string& why)
    {
        if (outError) *outError = why;
        return false;
    };
    float lo[3], hi[3];
    if (!Bounds(pts, lo, hi)) return fail("no points");
    EnsureJoltAllocator();
    maxVertices = (std::clamp)(maxVertices, 4, 256);
    const float extent    = (std::max)({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] });
    const float tolerance = (std::min)(1e-3f, 0.01f * (std::max)(extent, kMinHalfThickness));

    Points input = pts;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        JPH::ConvexHullBuilder::Positions positions;
        positions.reserve(input.size());
        for (const XMFLOAT3& p : input) positions.emplace_back(p.x, p.y, p.z);
        JPH::ConvexHullBuilder builder(positions);
        const char* err = nullptr;
        const auto  result = builder.Initialize(maxVertices, tolerance, err);
        if (result != JPH::ConvexHullBuilder::EResult::Success &&
            result != JPH::ConvexHullBuilder::EResult::MaxVerticesReached)
            return fail(std::string("Jolt's hull builder: ") + (err ? err : "failed"));

        std::vector<int> used;
        int faces = 0;
        const JPH::ConvexHullBuilder::Face* first = nullptr;
        for (const JPH::ConvexHullBuilder::Face* f : builder.GetFaces())
        {
            if (f->mRemoved || !f->mFirstEdge) continue;
            ++faces;
            if (!first) first = f;
            const JPH::ConvexHullBuilder::Edge* e = f->mFirstEdge;
            do
            {
                used.push_back(e->mStartIdx);
                e = e->mNextEdge;
            } while (e && e != f->mFirstEdge);
        }
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());

        // Coplanar input: the builder returns one polygon, back to back. Give it
        // thickness along its normal and build again.
        if (faces == 2 && attempt == 0 && first)
        {
            const JPH::Vec3 n = first->mNormal.NormalizedOr(JPH::Vec3::sAxisY()) * kMinHalfThickness;
            input.clear();
            for (int i : used)
            {
                const JPH::Vec3 p = positions[static_cast<size_t>(i)];
                input.push_back({ p.GetX() + n.GetX(), p.GetY() + n.GetY(), p.GetZ() + n.GetZ() });
                input.push_back({ p.GetX() - n.GetX(), p.GetY() - n.GetY(), p.GetZ() - n.GetZ() });
            }
            continue;
        }
        if (used.size() < 4) return fail("the points span no volume");
        outHull.reserve(used.size());
        for (int i : used)
        {
            const JPH::Vec3 p = positions[static_cast<size_t>(i)];
            outHull.push_back({ p.GetX(), p.GetY(), p.GetZ() });
        }
        if (outEdges)
        {
            // Each face's loop, as pairs of hull vertices; a shared edge once.
            auto at = [&](int idx)
            {
                return static_cast<std::uint32_t>(std::lower_bound(used.begin(), used.end(), idx) - used.begin());
            };
            std::vector<std::uint64_t> seen;
            for (const JPH::ConvexHullBuilder::Face* f : builder.GetFaces())
            {
                if (f->mRemoved || !f->mFirstEdge) continue;
                const JPH::ConvexHullBuilder::Edge* e = f->mFirstEdge;
                do
                {
                    if (!e->mNextEdge) break;
                    std::uint32_t a = at(e->mStartIdx), b = at(e->mNextEdge->mStartIdx);
                    if (a > b) std::swap(a, b);
                    const std::uint64_t k = (std::uint64_t(a) << 32) | b;
                    if (a != b && std::find(seen.begin(), seen.end(), k) == seen.end())
                    {
                        seen.push_back(k);
                        outEdges->push_back(a);
                        outEdges->push_back(b);
                    }
                    e = e->mNextEdge;
                } while (e != f->mFirstEdge);
            }
        }
        return true;
    }
    return fail("the points span no volume");
}
}
