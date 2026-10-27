// meshlet_shared.hlsli - the meshlet contract shared by the amplification / mesh shaders and
// C++: one source for the limits, the GPU record layouts and the LOD selection rule. The C++
// unit tests (tests/unit/meshlet_lod_test.cpp, meshlet_cull_test.cpp) include it too and
// static_assert that it agrees with include/Resource/MeshletData.h. Compiled as C++ when
// __cplusplus is defined - only the plain-scalar part below is visible there.
// KEEP THIS FILE PURE ASCII: the offline SDK dxc reads it in the system code page (docs:
// build-and-dxc-gotchas).

#ifndef MESHLET_SHARED_HLSLI
#define MESHLET_SHARED_HLSLI

#ifdef __cplusplus
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace MeshletShared
{
typedef uint32_t uint;
struct float3 { float x, y, z; };
// Layout shims for the GPU records below: plain 4-byte members, so the C++
// offsets equal the HLSL cbuffer offsets as long as no member straddles a
// 16-byte row (each record is laid out so none does; static_asserts check it).
struct float4   { float x, y, z, w; };
struct uint4    { uint x, y, z, w; };
struct float4x4 { float m[4][4]; };
inline float3 MeshletSub3(float3 a, float3 b) { return float3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float  MeshletLen3(float3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline float  MeshletMax1(float a, float b) { return a > b ? a : b; }
#define MESHLET_FN inline
#else
#define MESHLET_FN
float3 MeshletSub3(float3 a, float3 b) { return a - b; }
float  MeshletLen3(float3 v) { return length(v); }
float  MeshletMax1(float a, float b) { return max(a, b); }
#endif

// ==== Limits ====
// Must equal Resource::kMeshletMaxVerts / kMeshletMaxTris / kMeshletASGroupSize /
// kMeshletMSGroupSize (include/Resource/MeshletData.h) - a unit test pairs them.
static const uint kMeshletMaxVerts    = 64;
static const uint kMeshletMaxTris     = 124;
static const uint kMeshletASGroupSize = 32;    // clusters per amplification group
static const uint kMeshletMSGroupSize = 128;   // mesh shader threads

// MeshletDesc is 80 bytes (5 x uint4) in the GPU part of the MLET chunk.
static const uint kMeshletDescBytes = 80;
// MeshletDesc::counts flags (bits 24..31).
static const uint kMeshletFlagNoCone = 1;
// A parent error at or above this is "the root": nothing coarser replaces it.
static const float kMeshletRootErrorMin = 1.0e37f;

// MeshletMeshInfo (the per-mesh-slot sidecar): 16 bytes.
static const uint kMeshletInfoBytes = 16;
static const uint kMeshletNoData    = 0xFFFFFFFF;   // blobBufferIdx of a slot without meshlets

// Byte offsets inside one MeshletDesc (include/Resource/MeshletData.h).
static const uint kMeshletDescConeOffset   = 16;   // coneAxisCutoff (int8 snorm x4)
static const uint kMeshletDescRefsOffset   = 20;   // vertexRefOffset (DWORD index)
static const uint kMeshletDescTrisOffset   = 24;   // triangleOffset  (DWORD index)
static const uint kMeshletDescCountsOffset = 28;   // vertexCount | triCount<<8 | level<<16 | flags<<24
static const uint kMeshletDescLodSelfOffset   = 32;  // lodSelfCenter.xyz, lodSelfRadius
static const uint kMeshletDescLodParentOffset = 48;  // lodParentCenter.xyz, lodParentRadius
static const uint kMeshletDescLodErrorOffset  = 64;  // lodSelfError, lodParentError

// MeshletCullCB::cullMask bits. -meshletcull sets the first three; the Renderer adds Stats
// and Lod.
static const uint kMeshletCullFrustum = 1;
static const uint kMeshletCullCone    = 2;
static const uint kMeshletCullHiZ     = 4;
static const uint kMeshletCullStats   = 8;
static const uint kMeshletCullLod     = 16;

// Per-draw flags: the 4th root constant of a meshlet draw (GBuffer.vs's vestigial
// prevPosInfo slot), written by GBufferPass::RecordChunk.
static const uint kMeshletDrawNoCone  = 1;            // DOUBLE_SIDED: the rasterizer culls nothing
static const uint kMeshletDrawMeshlet = 0x80000000;   // drawn through the mesh pipeline (the debug view)

// MeshletCullCB::debugMode (the Meshlets view mode).
static const uint kMeshletDebugOff     = 0;
static const uint kMeshletDebugCluster = 1;   // a hash colour per cluster
static const uint kMeshletDebugLevel   = 2;   // a colour per LOD level

// The statistics buffer (MeshletCullCB::statsUavIdx): u32 counters summed over
// every amplification group of the frame, one InterlockedAdd per group each.
static const uint kMeshletStatTested      = 0;   // clusters looked at
static const uint kMeshletStatLodRejected = 1;   // not the selected level
static const uint kMeshletStatCullFrustum = 2;
static const uint kMeshletStatCullCone    = 3;
static const uint kMeshletStatCullHiZ     = 4;
static const uint kMeshletStatVisible     = 5;   // clusters handed to the mesh shader
static const uint kMeshletStatVisibleTris = 6;   // their triangles
static const uint kMeshletStatReserved    = 7;
static const uint kMeshletStatLevel0      = 8;   // visible clusters per LOD level, 8 levels (the last: 7+)
static const uint kMeshletStatLevels      = 8;
static const uint kMeshletStatsCount      = 16;

// ==== MeshletCullCB: register b4, 256 bytes ====
// One definition for both languages: the Renderer fills it through this struct (C++ shims
// above), the amplification shader reads it as ConstantBuffer<MeshletCullCB>. The main view
// has one per frame (FrameCB); each CSM cascade gets its own.
// Matrix convention: hiZViewProj is uploaded transposed, like every engine matrix, so the
// default column_major HLSL read sees the row-vector matrix and mul(float4(p, 1), hiZViewProj)
// projects. A missing transpose makes Hi-Z falsely cull; the rotated instance in the parity
// fixture is its canary.
struct MeshletCullCB
{
    float4   frustumPlanes[6];   //   0  inward normals, normalised: dot(n, p) + d >= 0 is inside
    float4x4 hiZViewProj;        //  96  the VP the Hi-Z pyramid was captured with (transposed)
    float3   cameraPosWS;        // 160
    uint     cullMask;           // 172  kMeshletCull* bits
    uint     hiZValid;           // 176
    uint     hiZWidth;           // 180
    uint     hiZHeight;          // 184
    uint     hiZMipCount;        // 188
    uint     hiZSrvIdx;          // 192
    uint     sidecarIdx;         // 196  g_Buffers index of this frame's sidecar (MeshletMeshInfo[])
    uint     statsUavIdx;        // 200
    uint     debugMode;          // 204
    float    lodScale;           // 208  perspective: renderHeight / (2 tan(fovY/2)); ortho: texels / unit
    float    lodThreshold;       // 212  tau in px / texels; <= 0 = level 0 only
    uint     lodOrtho;           // 216
    float    zNear;              // 220
    uint4    pad0;               // 224
    uint4    pad1;               // 240
};
#ifdef __cplusplus
static_assert(sizeof(MeshletCullCB) == 256, "MeshletCullCB is one 256-byte CB");
static_assert(offsetof(MeshletCullCB, hiZViewProj) == 96, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, cameraPosWS) == 160, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, cullMask) == 172, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, hiZSrvIdx) == 192, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, sidecarIdx) == 196, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, lodScale) == 208, "MeshletCullCB layout");
static_assert(offsetof(MeshletCullCB, zNear) == 220, "MeshletCullCB layout");
#endif

// ==== LOD selection ====
// The projected error of a group, in pixels (perspective: lodScale = renderHeight /
// (2 tan(fovY/2))) or texels (orthographic, lodOrtho != 0: lodScale = texels per world unit).
// The sphere and the error are already in world space (the caller scaled them by the
// instance's largest axis scale). The distance is to the sphere's nearest point, clamped to
// zNear - the conservative side: a nearer group looks bigger, so a coarser level is picked
// later, never sooner.
// A root (errorWS >= kMeshletRootErrorMin) projects to +infinity whatever the distance, so
// "parent > tau" always holds for it and every cluster has some level selected, even when
// tau is huge.
MESHLET_FN float MeshletProjectedError(float3 centerWS, float radiusWS, float errorWS,
                                       float3 cameraPosWS, float lodScale, uint lodOrtho, float zNear)
{
    if (errorWS >= kMeshletRootErrorMin) return 3.402823466e+38f;
    if (lodOrtho != 0) return errorWS * lodScale;
    float d = MeshletMax1(MeshletLen3(MeshletSub3(centerWS, cameraPosWS)) - radiusWS, zNear);
    return errorWS * lodScale / d;
}

// Draw a cluster when its parent group is too coarse for this view and its own group is fine
// enough. tau <= 0 is "LOD off": level 0 only.
// No holes, no double draw: every cluster of one group stores the same sphere and errors, so
// all take the same branch; errors are monotonic up the chain, so for one view exactly one
// level of a discrete-LOD entry (one group per level) passes both tests.
MESHLET_FN bool MeshletLodSelected(uint level,
                                   float3 selfCenterWS, float selfRadiusWS, float selfErrorWS,
                                   float3 parentCenterWS, float parentRadiusWS, float parentErrorWS,
                                   float3 cameraPosWS, float lodScale, float lodThreshold, uint lodOrtho,
                                   float zNear)
{
    if (lodThreshold <= 0.0f) return level == 0;
    return MeshletProjectedError(parentCenterWS, parentRadiusWS, parentErrorWS, cameraPosWS, lodScale,
                                 lodOrtho, zNear) > lodThreshold &&
           MeshletProjectedError(selfCenterWS, selfRadiusWS, selfErrorWS, cameraPosWS, lodScale,
                                 lodOrtho, zNear) <= lodThreshold;
}

#ifdef __cplusplus
}   // namespace MeshletShared
#else
// ==== HLSL only: the amplification -> mesh payload and the record loads ====
// The AS keeps up to kMeshletASGroupSize clusters of one instance and hands their absolute
// MeshletDesc indices (sidecar.firstCluster + local) to the MS in cluster order (the
// compaction is an ordered prefix sum, so the raster order is deterministic). 136 bytes.
struct MeshletPayload
{
    uint instance;                          // draw-local instance (the VS's SV_InstanceID)
    uint blobIdx;                           // g_Buffers index of the library's meshlet buffer
    uint clusterIds[kMeshletASGroupSize];   // MeshletDesc indices in the GPU part
};

// The GPU part of the MLET chunk, as uploaded: MeshletDesc[clusterCount], then
// the vertex refs and the packed triangles at DWORD offsets the desc names.
uint MeshletDescByte(uint cluster) { return cluster * kMeshletDescBytes; }
uint MeshletDwordByte(uint dwordIndex) { return dwordIndex * 4; }

// The Meshlets view mode's colours: a stable hash per cluster, a palette per level.
float3 MeshletDebugColor(uint mode, uint cluster, uint level)
{
    if (mode == kMeshletDebugLevel)
    {
        static const float3 kLevel[8] = {
            float3(0.15, 0.75, 0.25), float3(0.20, 0.45, 0.95), float3(0.95, 0.80, 0.15), float3(0.95, 0.45, 0.10),
            float3(0.85, 0.15, 0.20), float3(0.70, 0.25, 0.85), float3(0.25, 0.85, 0.85), float3(0.95, 0.95, 0.95) };
        return kLevel[min(level, 7u)];
    }
    uint h = cluster * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return float3(float(h & 0xFF), float((h >> 8) & 0xFF), float((h >> 16) & 0xFF)) * (0.75 / 255.0) + 0.25;
}
#endif

#endif // MESHLET_SHARED_HLSLI
