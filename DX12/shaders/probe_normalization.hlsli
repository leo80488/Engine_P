#ifndef PROBE_NORMALIZATION_HLSLI
#define PROBE_NORMALIZATION_HLSLI
#include "core/color.hlsli"   // Luminance
#include "bindless.hlsli"   // typed bindless handles

// probe_normalization.hlsli - reflection-probe brightness normalization.
// A probe is one baked capture (Lambert + the sky SH at bake time + the main camera's CSM)
// from its capture point. At a shading point that sees much less of that light - a floor
// under an awning, a room - it still reflects the outdoors. As with UE's reflection
// captures, the fix is to scale the capture by how bright the indirect light is here
// relative to the capture point, in one lighting model both points share: Lighting.ps's far
// field without AO (DDGI irradiance by its coverage plus the sky's diffuse fallback occluded
// by DDGI's sky visibility), along the shading normal at both points. The scale only darkens:
// a probe captured in a dark corner is not brightened where it is used.
// The includer provides: light_cb.hlsli (ddgiEnabled, ddgiVolumeCount, ddgiVolumeBufIdx,
// ddgiDiffuseScale, iblStrength, iblUseSH), sky_sh.hlsli (EvalSH2), DDGISampling.hlsli,
// `TextureCube gIrradiance` and `SamplerState gIBLSampler`. ResourceDescriptorHeap: graphics
// only (the shared graphics root signature is heap-indexed).
// TODO(IndirectLighting_Environment_Review_Plan IE5): relight probes with the live lighting;
// this scale is the interim.
// TODO(RenderPipeline_Architecture_Review_Plan FT3): the same live probe relight.

// The far-field diffuse irradiance (E/pi, no AO) at a point P along N.
float3 ProbeNorm_FarFieldIrradiance(float3 P, float3 N)
{
    float3 ddgi    = float3(0, 0, 0);
    float  cov     = 0.0;
    float4 moments = float4(1, 0, 0, 0);
    float  skyCov  = 0.0;
    if (ddgiEnabled != 0 && ddgiVolumeCount > 0)
    {
        StructuredBuffer<DDGIVolumeGPU> vols = BindlessSBuf<DDGIVolumeGPU>(ddgiVolumeBufIdx);
        const DDGIVolumePick pick = DDGI_PickVolumes(P, vols);
        float4 best  = float4(0, 0, 0, 0), second  = float4(0, 0, 0, 0);
        float4 bestM = float4(1, 0, 0, 0), secondM = float4(1, 0, 0, 0);
        [unroll] for (uint vi = 0; vi < DDGI_MAX_VOLUMES; ++vi)
        {
            if (!DDGI_IsPicked(pick, vi)) continue;
            const DDGIVolumeGPU vol = vols[vi];
            Texture2D<float4> irr = ResourceDescriptorHeap[NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
            Texture2D<float2> dep = ResourceDescriptorHeap[NonUniformResourceIndex(vol.depthBaseIdx + vi)];
            StructuredBuffer<DDGIProbeData> pd =
                ResourceDescriptorHeap[NonUniformResourceIndex(vol.probeDataBaseIdx + vi)];
            float4 m;
            // No view bias: the capture point is not a surface seen by a camera.
            const float4 s = DDGI_SampleVolumeSkyMoments(P, N, float3(0, 0, 0), vol, irr, dep, pd,
                                                         gIBLSampler, false, m);
            if (vi == (uint)pick.bestSlot) { best = s; bestM = m; }
            else                           { second = s; secondM = m; }
        }
        const float4 r = DDGI_CombineWinners(best, second, pick);
        ddgi    = r.rgb * ddgiDiffuseScale;
        cov     = r.a;
        moments = DDGI_CombineSkyMoments(bestM, best.a, secondM, second.a, pick, skyCov);
    }
    const float3 sky = (iblUseSH != 0) ? EvalSH2(N) : gIrradiance.SampleLevel(gIBLSampler, N, 0).rgb;
    const float  vis = lerp(1.0, DDGI_SkyVisibility(moments, N, 2.0 / 3.0), skyCov);
    return ddgi * cov + sky * saturate(1.0 - cov) * vis * iblStrength;
}

// The probe's scale at a shading point whose far-field irradiance (same model,
// same N) is `localIrradiance`: their luminance ratio, never above 1. Callers
// skip it (scale 1) when DDGI is off - the model is then the unoccluded sky at
// both points and the ratio is 1 anyway.
float ProbeNorm_Scale(float3 localIrradiance, float3 capturePos, float3 N)
{
    const float  lumCap = Luminance(ProbeNorm_FarFieldIrradiance(capturePos, N));
    const float  lumLoc = Luminance(localIrradiance);
    return lumCap > 1e-6 ? saturate(lumLoc / lumCap) : 1.0;
}

#endif // PROBE_NORMALIZATION_HLSLI
