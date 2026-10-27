#ifndef REFLECTION_PROBE_HLSLI
#define REFLECTION_PROBE_HLSLI

// Reflection probe struct plus the math used wherever probes are evaluated
// (deferred Lighting, forward custom_surface, hybrid RTR intersect).
//
// The 192 B struct lives in shared/reflection_probe_gpu.hlsli, one definition for C++
// (Reflection::GPUReflectionProbe) and every shader. Cubemaps are sampled bindlessly:
// each probe carries an absolute shader-visible heap index (cubemapSrvIndex) for
// ResourceDescriptorHeap[] (SM 6.6, needs HEAP_DIRECTLY_INDEXED); no TextureCubeArray
// is bound.
//
// This header declares no probe buffer. Each consumer opens it from the heap inside
// its own gate, from an index its constants carry:
//     StructuredBuffer<ReflectionProbe> b = ResourceDescriptorHeap[idx];
//   * shared graphics signature (Lighting.ps, custom_surface's MS_IS_FORWARD block,
//     asset custom .ps): LightCB.reflProbeBufIdx (offset 812, RootSignatureLayout.h
//     BindlessIdx);
//   * RTReflectionPass's signature (HybridReflIntersectHW/SW.cs):
//     HybridReflConstants.probeBufferIdx;
//   * ClusterCullProbes.cs on the shared compute signature: ClusterCB.probeBufferIdx.
// Every signature that runs shaders/ sets HEAP_DIRECTLY_INDEXED; without it a heap read
// compiles and then fails at PSO creation.
//
// Everything here (the struct, ComputeProbeWeight, SampleProbe) takes the probe by
// value and never touches the buffer.

#include "shared/reflection_probe_gpu.hlsli"   // struct ReflectionProbe

#define REFLECTION_PROBE_FLAG_BAKED     1u
#define REFLECTION_PROBE_FLAG_NORMALIZE 2u   // probe_normalization.hlsli applies (unless -probe_norm_off)

#define PROXY_INFINITE 0u
#define PROXY_SPHERE   1u
#define PROXY_OBB      2u
#define INFL_BOX       0u
#define INFL_SPHERE    1u

// Single roughness <-> mip mapping; it must match the bake prefilter's per-mip roughness
// schedule (linear, as the bake is), so change the two together.
// TODO(reflection_probe_proxy_influence_impl_plan M4): switch both to
// mip = sqrt(roughness) * (mipCount - 1), i.e. roughness = (mip / (mipCount - 1))^2.
float ProbeRoughnessToMip(float roughness, float mipCount)
{
    return saturate(roughness) * max(mipCount - 1.0, 0.0);
}

// worldPos -> volume-local via a rigid 3x4 (rows r0/r1/r2, translation in .w).
float3 ProbeWorldToLocal(float4 r0, float4 r1, float4 r2, float3 worldPos)
{
    float3x4 m = float3x4(r0, r1, r2);
    return mul(m, float4(worldPos, 1.0));
}
// Rotate a direction by the rigid transform's rotation (length preserved).
float3 ProbeRotateDir(float4 r0, float4 r1, float4 r2, float3 dir)
{
    float3x3 rot = float3x3(r0.xyz, r1.xyz, r2.xyz);
    return mul(rot, dir);
}

// Influence weight in [0,1]: per-face smoothstep box fade or sphere inner/outer.
// 0 outside the volume or when the probe is not baked.
float ComputeProbeWeight(float3 worldPos, ReflectionProbe probe)
{
    if ((probe.flags & REFLECTION_PROBE_FLAG_BAKED) == 0u) return 0.0;

    float3 posLS = ProbeWorldToLocal(probe.worldToInflR0, probe.worldToInflR1,
                                     probe.worldToInflR2, worldPos);
    float w;
    if (probe.inflShape == INFL_SPHERE)
    {
        float d = length(posLS);
        w = saturate((probe.inflExtents.x - d) /
                     max(probe.inflExtents.x - probe.inflExtents.y, 1e-4));
    }
    else
    {
        float3 e    = probe.inflExtents;
        float3 dPos = e - posLS;   // to +X/+Y/+Z faces
        float3 dNeg = e + posLS;   // to -X/-Y/-Z faces
        if (any(min(dPos, dNeg) < 0.0)) return 0.0;   // outside box
        float3 wP = saturate(dPos / max(probe.fadePos, 1e-4));
        float3 wN = saturate(dNeg / max(probe.fadeNeg, 1e-4));
        w = min(min(min(wP.x, wP.y), wP.z), min(min(wN.x, wN.y), wN.z));
    }
    return w * w * (3.0 - 2.0 * w);   // smoothstep: kills the fade-edge crease
}

// Parallax-correct R against the proxy volume and sample `cube`, the probe's cubemap
// array. Returns float4(radiance, geometry-coverage alpha). The cube is passed in, so a
// signature that binds it can call this too.
float4 SampleProbeCube(ReflectionProbe probe, TextureCubeArray<float4> cube,
                       float3 worldPos, float3 R, float roughness, SamplerState samp)
{
    float3 sampleDir = R;

    [branch]
    if (probe.proxyShape == PROXY_OBB)
    {
        float3 posLS = ProbeWorldToLocal(probe.worldToProxyR0, probe.worldToProxyR1,
                                         probe.worldToProxyR2, worldPos);
        float3 dirLS = ProbeRotateDir(probe.worldToProxyR0, probe.worldToProxyR1,
                                      probe.worldToProxyR2, R);
        float3 invD = 1.0 / dirLS;                    // 0 components -> +-inf; min/max drop them
        float3 t1   = ( probe.proxyExtents - posLS) * invD;
        float3 t2   = (-probe.proxyExtents - posLS) * invD;
        float3 tf   = max(t1, t2);
        float  t    = min(min(tf.x, tf.y), tf.z);     // rigid -> t == world distance
        float3 hitWS = worldPos + R * t;
        sampleDir = hitWS - probe.capturePosWS;
        float dCH = length(sampleDir);
        roughness = saturate(roughness * (t / max(dCH, 1e-4)));  // Lagarde correction
    }
    else if (probe.proxyShape == PROXY_SPHERE)
    {
        float3 posLS = ProbeWorldToLocal(probe.worldToProxyR0, probe.worldToProxyR1,
                                         probe.worldToProxyR2, worldPos);
        float3 dirLS = ProbeRotateDir(probe.worldToProxyR0, probe.worldToProxyR1,
                                      probe.worldToProxyR2, R);
        float r = probe.proxyExtents.x;
        float b = dot(posLS, dirLS);
        float c = dot(posLS, posLS) - r * r;
        float t = -b + sqrt(max(b * b - c, 0.0));     // exit (inside -> c<0 -> real root)
        float3 hitWS = worldPos + R * t;
        sampleDir = hitWS - probe.capturePosWS;
        float dCH = length(sampleDir);
        roughness = saturate(roughness * (t / max(dCH, 1e-4)));
    }
    // PROXY_INFINITE: sampleDir = R, roughness unchanged.

    float mip = ProbeRoughnessToMip(roughness, probe.mipCount);
    return cube.SampleLevel(samp, float4(sampleDir, 0.0), mip);
}

// Samples the probe's cubemap from the bindless heap (the graphics signature and
// RTReflectionPass's both set HEAP_DIRECTLY_INDEXED).
float4 SampleProbe(ReflectionProbe probe, float3 worldPos, float3 R, float roughness,
                   SamplerState samp)
{
    TextureCubeArray<float4> cube =
        ResourceDescriptorHeap[NonUniformResourceIndex(probe.cubemapSrvIndex)];
    return SampleProbeCube(probe, cube, worldPos, R, roughness, samp);
}

#endif // REFLECTION_PROBE_HLSLI
