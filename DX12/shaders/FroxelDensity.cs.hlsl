// FroxelDensity.cs.hlsl - fills the 3D density texture with per-froxel medium
// parameters (the first froxel pass):
//   .rgb = scattering coefficient (per channel, for chromatic fog)
//   .a   = extinction coefficient (scattering + absorption)
// Global medium: exponential height fog (VolumetricFogComponent, or the pass's own
// settings). Local medium: every LocalFogVolumeComponent that survived the CPU cull
// ADDS its density on top.

#include "FroxelCommon.hlsli"

cbuffer FroxelCB : register(b0, space2)
{
    FroxelParams P;
};

#define LOCAL_FOG_VOLUMES_REGISTER register(t5, space2)
#include "LocalFogVolumes.hlsli"
#if FROXEL_RT
#include "core/sampling.hlsli"
#include "core/participating_media.hlsli"   // MediumHeightSigmaT
#endif

RWTexture3D<float4> DstDensity : register(u0, space2);

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= P.froxelW || DTid.y >= P.froxelH || DTid.z >= P.froxelD)
        return;

    // XY-only sub-voxel jitter: the height-fog boundary would otherwise snap to voxel
    // centres and alias visibly. Z is NOT jittered: far voxels are metres deep, and a
    // per-frame Z shift shows as depth flicker temporal reprojection cannot hide.
    float3 jitter  = GetFroxelJitter(P.frameIndex);
    jitter.z = 0.0;
    float3 worldPos = FroxelToWorldJittered(DTid, P, jitter);

    // Exponential height fog: density = base x exp(-(y - start) x falloff); constant
    // base below `start`. Keep the albedo multiply last: with albedo 1 (the pass-owned
    // settings) scatterRGB then equals the untinted scatter bit for bit.
    float  density = GlobalFogExtinction(P, worldPos);
#if FROXEL_RT
    // Past the global medium's reach (Graphics/MediumSet.h; 0: none) there is none, as the path
    // tracers see it.
    if (P.mediumIdx != 0u)
    {
        StructuredBuffer<float4> reachWords = ResourceDescriptorHeap[P.mediumIdx];
        const float reach = reachWords[2].z;
        const float3 r = worldPos - P.cameraPos;
        if (reach > 0.0 && dot(r, r) > reach * reach)
            density = 0.0;
    }
#endif
    float  scatter = density * P.fogScattering;
    float  absorp  = density * P.fogAbsorption;
    float  ext     = scatter + absorp;
    float3 scatterRGB = scatter * P.fogAlbedo;

    // Local volumes: sigma_t = density x weight, sigma_s = sigma_t x albedo.
    [loop] for (uint i = 0; i < P.localVolumeCount; ++i)
    {
        const LocalFogVolumeGPU v = LocalFogVolumes[i];
        const float w = LocalFogWeight(v, worldPos);
        if (w <= 0.0) continue;
        const float d = v.density * w;
        scatterRGB += d * v.albedo;
        ext        += d;
    }

#if FROXEL_RT
    // The height fog inside the froxel range is part of the lit medium (its analytic composite
    // starts at the range's end): Graphics/MediumSet.h's height source, sigma_s = sigma_t x albedo.
    if (P.mediumIdx != 0u)
    {
        StructuredBuffer<float4> words = ResourceDescriptorHeap[P.mediumIdx];
        MediumHeight h;
        const float4 r3 = words[3], r4 = words[4];
        h.albedo = r3.xyz; h.density = r3.w;
        h.falloff = r4.x; h.height = r4.y; h.anisotropy = r4.z; h._pad = r4.w;
        const float d = MediumHeightSigmaT(h, worldPos.y);
        scatterRGB += d * h.albedo;
        ext        += d;
    }
#endif

    DstDensity[DTid] = float4(scatterRGB, ext);
}
