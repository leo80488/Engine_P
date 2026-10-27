// rt_diffuse_transmission.hlsli - ShadingModel::TwoSidedFoliage and Subsurface in the path tracers
// (PathTrace.cs.hlsl, RtxdiBridgePT.hlsli): light through a leaf or a thin skin, a Lambertian lobe
// on the side away from the viewer.
//
// The tint is the raster models': subsurfaceColor x subsurfaceStrength (Lighting.ps.hlsl's foliage
// and subsurface tints); a Subsurface surface also keeps 1 - subsurfaceOpacity of it, an opaque skin
// letting nothing through. It is limited so the diffuse reflection and it never exceed 1 per
// channel. The lobe joins the surface's BSDF as a third one, drawn with probability
// q = lum(tint) / (lum(tint) + lum(diffuse albedo)) and cosine-weighted about -n; a light behind the
// surface is a light sample's, as one in front is.
//
// The includer provides MaterialGPUData (material.hlsli), the shading model constants and
// bsdf_sample.hlsli.

#ifndef RT_DIFFUSE_TRANSMISSION_HLSLI
#define RT_DIFFUSE_TRANSMISSION_HLSLI

#include "core/color.hlsli"   // Luminance

struct RTThin
{
    bool   active;
    float3 tint;   // the transmitted albedo
    float  q;      // the probability a draw takes the lobe
};

// The lobe of a surface with material m (its shading model and subsurface fields) and the hit's
// albedo and metallic; inactive for every other model and for a tint of 0.
RTThin RTThinAt(MaterialGPUData m, float3 albedo, float metal)
{
    RTThin t = (RTThin)0;
    const bool foliage = m.customShadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE;
    const bool skin    = m.customShadingModel == SHADING_MODEL_SUBSURFACE;
    if (!foliage && !skin) return t;
    float3 tint = max(m.subsurfaceColor, 0.0) * max(m.subsurfaceStrength, 0.0);
    if (skin) tint *= 1.0 - saturate(m.subsurfaceOpacity);
    const float3 diffuse = albedo * (1.0 - saturate(metal));
    tint = min(tint, max(1.0 - diffuse, 0.0));
    if (!any(tint > 0)) return t;
    const float lt = Luminance(tint), ld = Luminance(diffuse);
    t.active = true;
    t.tint   = tint;
    t.q      = clamp(lt / max(lt + ld, 1e-6), 0.05, 0.95);
    return t;
}

// The whole surface toward l: the opaque BSDF in front, the lobe behind (cosine included).
float3 RTThinEval(RTThin t, float3 n, float3 v, float3 l, float3 albedo, float rough, float metal, float reflectance)
{
    const float c = dot(n, l);
    if (c < 0) return t.tint * (-c / PI);
    return RTEvalBSDF(n, v, l, albedo, rough, metal, reflectance);
}
float RTThinPdf(RTThin t, float3 n, float3 v, float3 l, float rough)
{
    const float c = dot(n, l);
    if (c < 0) return t.q * (-c / PI);
    return (1 - t.q) * RTBsdfPdf(n, v, l, rough);
}

// One draw from three uniforms, as an opaque vertex takes: u.z picks the lobe (rescaled, it is the
// opaque sampler's cosine / GGX choice), u.xy its direction. weight: f x |cos| / pdf; pdf: the
// mixture's density of l.
bool RTThinSample(RTThin t, float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
                  float3 u, out float3 l, out float3 weight, out float pdf)
{
    weight = 0; pdf = 0;
    if (u.z < t.q)
    {
        l = RTBasis(-n, RTSampleCosine(u.xy));
        pdf = t.q * (-dot(n, l) / PI);
        if (!(pdf > 0)) return false;
        weight = t.tint / t.q;
        return all(isfinite(weight));
    }
    const float uLobe = (u.z - t.q) / (1 - t.q);
    if (uLobe < 0.5)
        l = RTBasis(n, RTSampleCosine(u.xy));
    else
    {
        float3 tb, bb; OrthonormalBasis(n, tb, bb);
        const float3 vt = float3(dot(v, tb), dot(v, bb), dot(v, n));
        l = reflect(-v, RTBasis(n, SampleGGXVNDF(vt, rough * rough, u.x, u.y)));
    }
    if (dot(n, l) <= 0 || dot(n, v) <= 0) return false;
    pdf = (1 - t.q) * RTBsdfPdf(n, v, l, rough);
    if (!(pdf > 0)) return false;
    weight = RTEvalBSDF(n, v, l, albedo, rough, metal, reflectance) / pdf;
    return all(isfinite(weight));
}

#endif // RT_DIFFUSE_TRANSMISSION_HLSLI
