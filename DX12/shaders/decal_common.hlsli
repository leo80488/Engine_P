// decal_common.hlsli - shared definitions of the clustered decal system.
//
// Unreal-style 9-channel schema: every channel is one bindless texture slot + one scalar
// multiplier. Texture index -1 means "slot unused"; the scalar then acts as the raw value (for
// roughness/specular/ao) or the blend is skipped (for normal/bump/opacity/cavity/displacement).
//
// Used by DecalClusterCull.cs.hlsl and DecalApply.cs.hlsl. The layout must mirror GPUDecalUpload
// in DecalPass.cpp (192 bytes, static_assert on the C++ side).

#ifndef DECAL_COMMON_HLSLI
#define DECAL_COMMON_HLSLI

#include "cluster_common.hlsli"   // grid dims + ClusterAABB + GetClusterCoord + ClusterIndex

// Per-cluster decal budget - must match DecalPass::kMaxPerCluster.
#define MAX_DECALS_PER_CLUSTER 16

// flags bit layout - mirrors DecalMaterialAsset::FLAGS (bits 0-4) plus renderer-set source bits
// (bit 8+).
#define DECAL_WRITE_BASECOLOR  (1u << 0)
#define DECAL_WRITE_NORMAL     (1u << 1)
#define DECAL_WRITE_ROUGHNESS  (1u << 2)
#define DECAL_WRITE_SPECULAR   (1u << 3)
#define DECAL_WRITE_AO         (1u << 4)
// texRoughness/texAO point at one packed ARM texture (the unified-material decal path:
// SURFACEMAP, R=AO G=roughness B=metalness): sample roughness from .g and AO from .r. Without this
// bit each is a per-channel grayscale texture read from .r (9-slot decal assets).
#define DECAL_SRC_ARM          (1u << 8)

// 192 bytes, 3 cache lines. Scalars grouped into two float4 packs so the
// shader reads them with a single 16-byte fetch each.
struct GPUDecal
{
    float4x4 worldToDecal;       // 64  offset 0 - world -> decal local [-0.5..0.5]

    float3   boundsCenter;       // 12  offset 64
    float    boundsRadius;       // 4   offset 76

    // Texture bindless indices (-1 = slot unused). Keep in declared order
    // so the apply CS can access by name without lookup tables.
    int      texBaseColor;       // 4   offset 80
    int      texNormal;          // 4   offset 84
    int      texOpacity;         // 4   offset 88
    int      texRoughness;       // 4   offset 92
    int      texSpecular;        // 4   offset 96
    int      texAO;              // 4   offset 100
    int      texBump;            // 4   offset 104
    int      texCavity;          // 4   offset 108
    int      texDisplacement;    // 4   offset 112
    uint     flags;              // 4   offset 116 - DECAL_WRITE_* bitmask
    float    sortLayer;          // 4   offset 120
    float    _pad0;              // 4   offset 124 - align the next float4

    float4   baseColorTint;      // 16  offset 128 - rgb tint, a opacity
    // scalars0: x=opacity, y=roughness, z=specular, w=ao
    float4   scalars0;           // 16  offset 144
    // scalars1: x=normalStrength, y=bumpStrength, z=cavityStrength, w=displacementScale
    float4   scalars1;           // 16  offset 160

    float3   decalForwardWS;     // 12  offset 176 - decal's world-space +Z (normalised on the CPU)
    float    angleFadeStart;     // 4   offset 188
    // total: 192 bytes
};

struct DecalGridEntry
{
    uint offset;
    uint count;
};

// Sphere vs AABB broad phase; both arguments are in view space.
bool DecalSphereVsAABB(float3 centerVS, float radius, ClusterAABB aabb)
{
    float3 closest = clamp(centerVS, aabb.minPt, aabb.maxPt);
    float3 diff    = closest - centerVS;
    return dot(diff, diff) <= radius * radius;
}

// Decal tangent frame, world space.
//
// worldToDecal is inverse(world) in the engine's row-vector convention
// (localPos = mul(float4(worldPos, 1), worldToDecal)), so the columns of its upper 3x3 are the
// decal's world-space axes divided by their scale; normalising each recovers the unit axis.
// Column 2 reproduces the CPU-side decalForwardWS by construction. (Non-uniform scale under a
// rotated parent shears the frame; the rest of the decal path assumes it does not.)
//
// Rows are (T, BT, N), matching the mesh path's tangentToWorld:
//   T  = +decal X - the direction decalUV.u grows (decalUV = localPos.xy + 0.5)
//   BT = +decal Y - the direction decalUV.v grows
//   N  = -decal Z - decals project along +Z, so a receiving surface faces -Z. A flat (0,0,1)
//                   decal normal then maps back onto that surface normal: the blend is a no-op.
// world = mul(tangentSpace, frame);  tangent = mul(frame, worldSpace).
float3x3 DecalTangentFrame(float4x4 worldToDecal)
{
    float3 axisX = normalize(worldToDecal._m00_m10_m20);
    float3 axisY = normalize(worldToDecal._m01_m11_m21);
    float3 axisZ = normalize(worldToDecal._m02_m12_m22);
    return float3x3(axisX, axisY, -axisZ);
}

// Reoriented Normal Mapping blend (Barre-Brisebois / Hill).
// Both arguments must be in the same tangent basis with +Z up - the `base + float3(0, 0, 1)` line
// encodes that. Operands from different spaces (e.g. a world-space base with a tangent-space
// detail) give garbage; use DecalTangentFrame to bring them into one basis.
float3 ReorientedNormalBlend(float3 base, float3 detail)
{
    float3 t = base  + float3(0, 0, 1);
    float3 u = detail * float3(-1, -1, 1);
    return normalize(t * dot(t, u) - u * t.z);
}

#endif // DECAL_COMMON_HLSLI
