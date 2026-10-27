// meshlet_cull.hlsli - the per-cluster culling tests of the meshlet amplification shader,
// shared with C++: tests/unit/meshlet_cull_test.cpp includes this file and checks every test
// against the D3D rasterizer (random affine instances: rotation, non-uniform scale,
// mirroring). Compiled as C++ when __cplusplus is defined.
// Instance transform: the engine's row-vector convention, as GBufferVertex uses it -
// p_world = p.x * r0 + p.y * r1 + p.z * r2 + t, where r0..r2 / t are rows 0..2 / 3 of
// GPUInstanceData::world (InstanceBuffer[slot].world[i].xyz).
// KEEP THIS FILE PURE ASCII (docs: build-and-dxc-gotchas).

#ifndef MESHLET_CULL_HLSLI
#define MESHLET_CULL_HLSLI

#include "meshlet_shared.hlsli"

#ifdef __cplusplus
namespace MeshletShared
{
inline float3 MeshletMake3(float x, float y, float z) { return float3{ x, y, z }; }
inline float  MeshletSqrt(float v) { return std::sqrt(v); }
// A signed byte of a packed u32, as a float in [-1, 1] (int8 SNORM, /127).
inline float MeshletS8(uint packed, uint byteIndex)
{
    return float(int8_t(uint8_t((packed >> (8u * byteIndex)) & 0xFFu))) / 127.0f;
}
#define MESHLET_PLANES_PARAM const float4 planes[6]
#else
float3 MeshletMake3(float x, float y, float z) { return float3(x, y, z); }
float  MeshletSqrt(float v) { return sqrt(v); }
float MeshletS8(uint packed, uint byteIndex)
{
    // Move the byte to the top, then sign-extend it down with an arithmetic shift.
    return float(int(packed << (24u - 8u * byteIndex)) >> 24) / 127.0f;
}
#define MESHLET_PLANES_PARAM float4 planes[6]
#endif

MESHLET_FN float MeshletDot3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
MESHLET_FN float3 MeshletCross3(float3 a, float3 b)
{
    return MeshletMake3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

// ==== Object space -> world space ====
MESHLET_FN float3 MeshletXformPoint(float3 p, float3 r0, float3 r1, float3 r2, float3 t)
{
    return MeshletMake3(p.x * r0.x + p.y * r1.x + p.z * r2.x + t.x,
                        p.x * r0.y + p.y * r1.y + p.z * r2.y + t.y,
                        p.x * r0.z + p.y * r1.z + p.z * r2.z + t.z);
}

// The largest axis scale: an object-space sphere maps into the world sphere of radius
// r * MeshletMaxScale (exact for uniform scale, conservative otherwise).
MESHLET_FN float MeshletMaxScale(float3 r0, float3 r1, float3 r2)
{
    return MeshletSqrt(MeshletMax1(MeshletDot3(r0, r0), MeshletMax1(MeshletDot3(r1, r1), MeshletDot3(r2, r2))));
}

// ==== World space -> object space (the camera, for the cone) ====
// p_o = (p_w - t) * R^-1. The columns of R^-1 are (r1 x r2, r2 x r0, r0 x r1) / det.
// Returns false (and leaves camOS untouched) for a singular transform: the cone is then not
// tested.
MESHLET_FN bool MeshletWorldToObject(float3 pw, float3 r0, float3 r1, float3 r2, float3 t,
#ifdef __cplusplus
                                     float3& camOS)
#else
                                     inout float3 camOS)
#endif
{
    const float3 c0 = MeshletCross3(r1, r2);
    const float3 c1 = MeshletCross3(r2, r0);
    const float3 c2 = MeshletCross3(r0, r1);
    const float det = MeshletDot3(r0, c0);
    if (!(det * det > 1e-24f)) return false;   // NaN-safe: a NaN det is "singular" too
    const float3 d = MeshletSub3(pw, t);
    const float inv = 1.0f / det;
    camOS = MeshletMake3(MeshletDot3(d, c0) * inv, MeshletDot3(d, c1) * inv, MeshletDot3(d, c2) * inv);
    return true;
}

// ==== Frustum ====
// Planes as the engine extracts them (Renderer's view.frustum / MeshletCullCB): normalised,
// inward - dot(n, p) + d >= 0 is inside. A sphere wholly behind one plane is outside the
// frustum, and so is every triangle inside it.
MESHLET_FN bool MeshletSphereOutsideFrustum(float3 centerWS, float radiusWS, MESHLET_PLANES_PARAM)
{
    for (uint i = 0; i < 6; ++i)
    {
        const float s = planes[i].x * centerWS.x + planes[i].y * centerWS.y + planes[i].z * centerWS.z + planes[i].w;
        if (s < -radiusWS) return true;
    }
    return false;
}

// ==== Normal cone (backface) ====
// meshoptimizer's sphere form (meshoptimizer.h, meshopt_computeMeshletBounds):
// dot(center - camera, axis) >= cutoff * length(center - camera) + radius means every
// triangle of the cluster faces away from the camera. Tested in object space with the camera
// brought there: an affine map keeps "the camera is behind this triangle's plane", so the
// answer is exact for rotation and non-uniform scale. A mirrored instance needs no reversal
// here: its PSO flips the front-face convention (rs.front_counter_clockwise,
// GBufferPass::BuildPSODesc), so the rasterizer culls exactly the object-space back faces.
// coneAxisCutoff: MeshletDesc's int8 SNORM axis.xyz | cutoff << 24 (conservative).
MESHLET_FN bool MeshletConeBackfacing(float3 centerOS, float radiusOS, uint coneAxisCutoff, float3 camOS)
{
    const float3 axis = MeshletMake3(MeshletS8(coneAxisCutoff, 0u), MeshletS8(coneAxisCutoff, 1u),
                                     MeshletS8(coneAxisCutoff, 2u));
    const float cutoff = MeshletS8(coneAxisCutoff, 3u);
    const float3 v = MeshletSub3(centerOS, camOS);
    return MeshletDot3(v, axis) >= cutoff * MeshletLen3(v) + radiusOS;
}

#ifdef __cplusplus
}   // namespace MeshletShared
#endif

#endif // MESHLET_CULL_HLSLI
