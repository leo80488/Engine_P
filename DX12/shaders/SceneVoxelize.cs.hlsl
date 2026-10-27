// SceneVoxelize.cs.hlsl - triangle-precision voxelization for the volumetric fog occlusion grid.
// One thread per triangle: fetch its vertices through programmable vertex fetching, transform them
// by the instance's world matrix, then run the Akenine-Moller triangle-box SAT test on every voxel
// of the triangle's bounding box; each voxel the triangle really intersects is marked 255.
// Unlike an AABB-dilated grid this keeps openings / doorways, thin railings, angled or curved faces
// and concave meshes.
// Root signature (compute):
//   b0 space2 - VoxelizeCB
//   u0 space2 - RWTexture3D<uint> occupancy
//   t0 space0 - StructuredBuffer<GPUInstanceData> (root SRV, slot 9)
//   t1 space0 - StructuredBuffer<MeshDescriptor>  (root SRV, slot 10)
//   t0 space1 - ByteAddressBuffer g_Buffers[]     (bindless, slot 11)

#include "gpu_instance.hlsli"
#include "pvf_fetch.hlsli"

cbuffer VoxelizeCB : register(b0, space2)
{
    float3 gridMin;      float _pad0;
    float3 gridExtent;   uint  gridDim;
    uint   meshDescIdx;      // per-dispatch: which MeshDescriptor to fetch
    uint   instanceOffset;   // per-dispatch: which instance entry to read world from
    uint   numTriangles;     // per-dispatch: how many triangles this dispatch covers
    uint   _pad1;
};

StructuredBuffer<GPUInstanceData> InstanceBuffer  : register(t0, space0);
StructuredBuffer<MeshDescriptor>  MeshDescriptors : register(t1, space0);

RWTexture3D<uint> gOccupancy : register(u0, space2);

// ==== Akenine-Moller triangle-box overlap test (the 2001 paper) ====
// The AXISTEST macros read TriangleBoxOverlap's box-relative v0/v1/v2 and boxHalf.

#define AXISTEST_X01(a, b, fa, fb)                             \
{                                                              \
    p0 = a * v0.y - b * v0.z;                                  \
    p2 = a * v2.y - b * v2.z;                                  \
    if (p0 < p2) { mn = p0; mx = p2; } else { mn = p2; mx = p0; } \
    rad = fa * boxHalf.y + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_X2(a, b, fa, fb)                              \
{                                                              \
    p0 = a * v0.y - b * v0.z;                                  \
    p1 = a * v1.y - b * v1.z;                                  \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.y + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Y02(a, b, fa, fb)                             \
{                                                              \
    p0 = -a * v0.x + b * v0.z;                                 \
    p2 = -a * v2.x + b * v2.z;                                 \
    if (p0 < p2) { mn = p0; mx = p2; } else { mn = p2; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Y1(a, b, fa, fb)                              \
{                                                              \
    p0 = -a * v0.x + b * v0.z;                                 \
    p1 = -a * v1.x + b * v1.z;                                 \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.z;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Z12(a, b, fa, fb)                             \
{                                                              \
    p1 = a * v1.x - b * v1.y;                                  \
    p2 = a * v2.x - b * v2.y;                                  \
    if (p2 < p1) { mn = p2; mx = p1; } else { mn = p1; mx = p2; } \
    rad = fa * boxHalf.x + fb * boxHalf.y;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

#define AXISTEST_Z0(a, b, fa, fb)                              \
{                                                              \
    p0 = a * v0.x - b * v0.y;                                  \
    p1 = a * v1.x - b * v1.y;                                  \
    if (p0 < p1) { mn = p0; mx = p1; } else { mn = p1; mx = p0; } \
    rad = fa * boxHalf.x + fb * boxHalf.y;                     \
    if (mn > rad || mx < -rad) return false;                   \
}

// True when the world-space triangle tv0/tv1/tv2 intersects the voxel centred at boxCenter with
// half extent boxHalf, edge or face contact included.
bool TriangleBoxOverlap(float3 tv0, float3 tv1, float3 tv2,
                        float3 boxCenter, float3 boxHalf)
{
    // Move box to origin.
    float3 v0 = tv0 - boxCenter;
    float3 v1 = tv1 - boxCenter;
    float3 v2 = tv2 - boxCenter;

    // Triangle edges.
    float3 e0 = v1 - v0;
    float3 e1 = v2 - v1;
    float3 e2 = v0 - v2;

    float p0, p1, p2, mn, mx, rad;
    float3 fe;

    // 9 axis tests (edges x box principal axes).
    fe = abs(e0);
    AXISTEST_X01(e0.z, e0.y, fe.z, fe.y);
    AXISTEST_Y02(e0.z, e0.x, fe.z, fe.x);
    AXISTEST_Z12(e0.y, e0.x, fe.y, fe.x);

    fe = abs(e1);
    AXISTEST_X01(e1.z, e1.y, fe.z, fe.y);
    AXISTEST_Y02(e1.z, e1.x, fe.z, fe.x);
    AXISTEST_Z0 (e1.y, e1.x, fe.y, fe.x);

    fe = abs(e2);
    AXISTEST_X2 (e2.z, e2.y, fe.z, fe.y);
    AXISTEST_Y1 (e2.z, e2.x, fe.z, fe.x);
    AXISTEST_Z12(e2.y, e2.x, fe.y, fe.x);

    // AABB overlap on the three box axes.
    float3 vmin = min(v0, min(v1, v2));
    float3 vmax = max(v0, max(v1, v2));
    if (any(vmin > boxHalf))  return false;
    if (any(vmax < -boxHalf)) return false;

    // Triangle plane vs box test.
    float3 normal = cross(e0, e1);
    float3 vMinP, vMaxP;
    [unroll] for (int q = 0; q < 3; ++q)
    {
        if (normal[q] > 0) { vMinP[q] = -boxHalf[q] - v0[q]; vMaxP[q] =  boxHalf[q] - v0[q]; }
        else               { vMinP[q] =  boxHalf[q] - v0[q]; vMaxP[q] = -boxHalf[q] - v0[q]; }
    }
    if (dot(normal, vMinP) > 0) return false;
    if (dot(normal, vMaxP) < 0) return false;

    return true;
}

#undef AXISTEST_X01
#undef AXISTEST_X2
#undef AXISTEST_Y02
#undef AXISTEST_Y1
#undef AXISTEST_Z12
#undef AXISTEST_Z0

// Fetch one triangle's three world-space vertex positions, through the mesh's index buffer when it
// has one.
void FetchTriangleWorld(uint triIdx, out float3 w0, out float3 w1, out float3 w2)
{
    MeshDescriptor md = MeshDescriptors[meshDescIdx];

    uint i0 = FetchIndex(md, triIdx * 3u + 0u);
    uint i1 = FetchIndex(md, triIdx * 3u + 1u);
    uint i2 = FetchIndex(md, triIdx * 3u + 2u);

    float3 p0 = FETCH_POS(md, i0);
    float3 p1 = FETCH_POS(md, i1);
    float3 p2 = FETCH_POS(md, i2);

    // The world matrix is laid out for row-vector mul(pos, world), as in GBuffer / Shadow.
    float4x4 world = InstanceBuffer[instanceOffset].world;
    w0 = mul(float4(p0, 1.0), world).xyz;
    w1 = mul(float4(p1, 1.0), world).xyz;
    w2 = mul(float4(p2, 1.0), world).xyz;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint triIdx = dtid.x;
    if (triIdx >= numTriangles) return;

    float3 w0, w1, w2;
    FetchTriangleWorld(triIdx, w0, w1, w2);

    // The triangle's world-space AABB -> voxel index range.
    float3 triMin = min(w0, min(w1, w2));
    float3 triMax = max(w0, max(w1, w2));

    // Early-out if the triangle is entirely outside the grid.
    float3 gMax = gridMin + gridExtent;
    if (any(triMax < gridMin) || any(triMin > gMax)) return;

    float3 invExt  = 1.0 / gridExtent;
    float  voxSize = gridExtent.x / float(gridDim);
    float3 voxHalf = float3(voxSize, voxSize, voxSize) * 0.5;

    int3 vMin = clamp(int3(floor((triMin - gridMin) * invExt * float(gridDim))),
                      0, int(gridDim) - 1);
    int3 vMax = clamp(int3(ceil ((triMax - gridMin) * invExt * float(gridDim))),
                      0, int(gridDim) - 1);

    // Safety cap: a triangle spanning the whole grid would iterate 128^3 ~ 2M voxels in one thread (a
    // hang). A span over 32 voxels per axis is pathological; the clamped remainder is not voxelized.
    const int kMaxSpan = 32;
    vMax = min(vMax, vMin + kMaxSpan);

    for (int z = vMin.z; z <= vMax.z; ++z)
    for (int y = vMin.y; y <= vMax.y; ++y)
    for (int x = vMin.x; x <= vMax.x; ++x)
    {
        float3 vCenter = gridMin + (float3(x, y, z) + 0.5) * voxSize;
        if (TriangleBoxOverlap(w0, w1, w2, vCenter, voxHalf))
        {
            // Writes of the same value from several threads are race-safe for R8_UINT: no InterlockedOr.
            gOccupancy[int3(x, y, z)] = 255;
        }
    }
}
