#ifndef FROXEL_RT_MEDIUM_HLSLI
#define FROXEL_RT_MEDIUM_HLSLI
// froxel_rt_medium.hlsli - the medium set (Graphics/MediumSet.h: six float4 for the set, six per
// local volume) for the froxel fog's kernels, read from the heap index FROXEL_MEDIUM_IDX: the
// FROXEL_RT kernels' P.mediumIdx unless the includer defines another first (the raymarch's
// P.rmMediumIdx). The includer declares FroxelCB's P and includes core/participating_media.hlsli
// first.
#ifndef FROXEL_MEDIUM_IDX
#define FROXEL_MEDIUM_IDX P.mediumIdx
#endif

MediumLocal MediumLoadLocal(uint index)
{
    StructuredBuffer<float4> words = ResourceDescriptorHeap[FROXEL_MEDIUM_IDX];
    const uint base = 6u * (index + 1u);
    const float4 r3 = words[base + 3], r4 = words[base + 4], r5 = words[base + 5];
    MediumLocal v;
    v.row0 = words[base]; v.row1 = words[base + 1]; v.row2 = words[base + 2];
    v.albedo = r3.xyz; v.density = r3.w;
    v.emission = r4.xyz; v.edgeFade = r4.w;
    v.shape = asuint(r5.x); v.heightFalloff = r5.y; v.halfHeight = r5.z; v._pad = r5.w;
    return v;
}
MediumSet FroxelMediumSet()
{
    MediumSet s = (MediumSet)0;
    if (FROXEL_MEDIUM_IDX == 0u) return s;
    StructuredBuffer<float4> words = ResourceDescriptorHeap[FROXEL_MEDIUM_IDX];
    const float4 r0 = words[0], r1 = words[1], r2 = words[2], r3 = words[3], r4 = words[4], r5 = words[5];
    s.global.albedo = r0.xyz; s.global.density = r0.w;
    s.global.emission = r1.xyz; s.global.falloff = r1.w;
    s.global.baseHeight = r2.x; s.global.anisotropy = r2.y; s.global.reach = r2.z; s.global._pad = r2.w;
    s.height.albedo = r3.xyz; s.height.density = r3.w;
    s.height.falloff = r4.x; s.height.height = r4.y; s.height.anisotropy = r4.z; s.height._pad = r4.w;
    s.localCount = asuint(r5.x); s.camera = r5.yzw; s.global.reachCenter = r5.yzw;
    return s;
}
#endif
