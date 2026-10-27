// core/bsdf_dielectric.hlsli - one rough dielectric interface (glass, water, a lacquer):
// GGX reflection and refraction under the exact Fresnel term, the sampler that draws
// both lobes and its pdf, the thin-walled variant, and the IOR and absorption helpers.
// The rules every core header follows are at the top of core/gbuffer_codec.hlsli.
//
// Frame: every direction is in the local shading frame (+Z the shading normal) and points
// away from the surface: wo toward the viewer, wi toward the light. The exterior is the
// side +Z points into, so a direction with z < 0 is inside, and eta is the interior IOR
// over the exterior's (1.5 for glass in air).
//
// Transport is radiance (camera paths): a refraction scales radiance by 1/etap^2, etap the
// IOR ratio across the boundary along wi (Walter et al. 2007 eq. 21, PBRT-v4
// DielectricBxDF), so a path that enters and leaves a solid keeps its throughput. A
// thin-walled surface is a slab whose second face is crossed at once: no bend, no etap^2.
//
// The includer provides PI (core/math.hlsli) and SampleGGXVNDF (core/sampling.hlsli).

#ifndef CORE_BSDF_DIELECTRIC_HLSLI
#define CORE_BSDF_DIELECTRIC_HLSLI

#ifndef CORE_MATH_HLSLI
#error "core/bsdf_dielectric.hlsli needs core/math.hlsli - include it first"
#endif
#ifndef CORE_SAMPLING_HLSLI
#error "core/bsdf_dielectric.hlsli needs core/sampling.hlsli - include it first"
#endif

// Below this GGX alpha the interface is a mirror and a clear pane: both lobes are deltas,
// so EvalDielectric and PdfDielectric return 0 and only the sampler reaches them.
static const float kDielectricSmoothAlpha = 1e-3;

// ==== IOR and F0 ====
float DielectricIorToF0(float ior)
{
    const float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

// The IOR whose normal-incidence reflectance is f0, for f0 below 0.99.
float DielectricF0ToIor(float f0)
{
    const float s = sqrt(clamp(f0, 0.0, 0.99));
    return (1.0 + s) / (1.0 - s);
}

// ==== Fresnel ====
// Unpolarised reflectance of a dielectric boundary. cosThetaI is the cosine to the normal
// on the side the direction arrives from; a negative one is a direction inside, which
// swaps the media. 1 under total internal reflection.
float FresnelDielectric(float cosThetaI, float eta)
{
    float c = clamp(cosThetaI, -1.0, 1.0);
    float e = eta;
    if (c < 0.0)
    {
        e = 1.0 / e;
        c = -c;
    }
    const float sin2T = (1.0 - c * c) / (e * e);
    if (sin2T >= 1.0)
        return 1.0;
    const float cosT  = sqrt(max(1.0 - sin2T, 0.0));
    const float rParl = (e * c - cosT) / (e * c + cosT);
    const float rPerp = (c - e * cosT) / (c + e * cosT);
    return 0.5 * (rParl * rParl + rPerp * rPerp);
}

// A thin slab whose two faces each reflect F, every internal bounce summed and nothing
// absorbed: R = 2F / (1 + F), T = (1 - F) / (1 + F). A pane of glass reflects ~8%.
float ThinSlabReflectance(float F)
{
    return 2.0 * F / (1.0 + F);
}

float ThinSlabTransmittance(float F)
{
    return (1.0 - F) / (1.0 + F);
}

// ==== Refraction ====
// Snell refraction of w (pointing away from the surface) through the boundary with normal
// n (pointing outside). False under total internal reflection. etap is the IOR ratio
// across the boundary along the refracted direction: eta entering, 1/eta leaving.
bool DielectricRefract(float3 w, float3 n, float eta, out float3 wt, out float etap)
{
    float  cosI   = dot(n, w);
    float  e      = eta;
    float3 facing = n;
    if (cosI < 0.0)
    {
        e      = 1.0 / e;
        cosI   = -cosI;
        facing = -n;
    }
    wt   = float3(0.0, 0.0, 0.0);
    etap = e;
    const float sin2T = max(0.0, 1.0 - cosI * cosI) / (e * e);
    if (sin2T >= 1.0)
        return false;
    const float cosT = sqrt(max(1.0 - sin2T, 0.0));
    wt = -w / e + (cosI / e - cosT) * facing;
    return true;
}

// ==== GGX in the local frame ====
// The normal distribution, on |wm.xy|^2 rather than 1 - cos^2 so a narrow lobe keeps its
// precision. wm is a unit vector; 0 below the surface.
float DielectricGgxD(float3 wm, float alpha)
{
    if (wm.z <= 0.0)
        return 0.0;
    const float a2 = alpha * alpha;
    const float d  = dot(wm.xy, wm.xy) + a2 * wm.z * wm.z;
    return a2 / max(PI * d * d, 1e-30);
}

// Smith's Lambda (Heitz 2014). 0 exactly at the horizon, where every caller has already
// returned on a zero cosine.
float DielectricGgxLambda(float3 w, float alpha)
{
    const float cos2 = w.z * w.z;
    if (cos2 <= 0.0)
        return 0.0;
    const float tan2 = dot(w.xy, w.xy) / cos2;
    return 0.5 * (sqrt(1.0 + alpha * alpha * tan2) - 1.0);
}

float DielectricGgxG1(float3 w, float alpha)
{
    return 1.0 / (1.0 + DielectricGgxLambda(w, alpha));
}

// Height-correlated masking-shadowing; the same form serves reflection and refraction.
float DielectricGgxG2(float3 wo, float3 wi, float alpha)
{
    return 1.0 / (1.0 + DielectricGgxLambda(wo, alpha) + DielectricGgxLambda(wi, alpha));
}

// The density of the microfacet normals w sees, for w on either side (wm above).
float DielectricGgxVisiblePdf(float3 w, float3 wm, float alpha)
{
    const float cosW = abs(w.z);
    if (cosW <= 0.0)
        return 0.0;
    return DielectricGgxG1(w, alpha) / cosW * DielectricGgxD(wm, alpha) * abs(dot(w, wm));
}

// A visible normal for w on either side: Heitz's sampler wants w above, and the visible
// distribution of -w is the same set of normals.
float3 DielectricGgxSampleVisible(float3 w, float alpha, float2 u)
{
    const float side = (w.z < 0.0) ? -1.0 : 1.0;
    return SampleGGXVNDF(w * side, alpha, u.x, u.y);
}

// ==== The interface ====
struct DielectricLobe
{
    float alpha;          // GGX alpha (perceptual roughness squared)
    float eta;            // interior IOR over exterior IOR
    float transmission;   // [0,1] share of the refracted light that stays specular; the
                          // rest belongs to the caller's base (a diffuse lobe)
    bool  thinWalled;     // a slab: wi leaves through the far face, unbent
};

struct DielectricSample
{
    float3 wi;            // the sampled direction, local frame
    float  weight;        // f * |wi.z| / pdf, before a transmission's tint
    float  pdf;           // solid-angle pdf; 0 for a delta lobe
    float  etap;          // IOR ratio across the boundary along wi; 1 for a reflection
    bool   transmitted;   // wi is on the far side
    bool   delta;         // a smooth interface: no light sample can reach this lobe
};

bool DielectricIsSmooth(DielectricLobe lobe)
{
    return lobe.alpha < kDielectricSmoothAlpha;
}

// The reflected and transmitted shares at a microfacet whose normal makes cosine
// cosThetaM with wo, the caller's transmission weight applied. The sampler picks between
// the lobes in the same proportion. A thin slab is symmetric, so it sees the cosine
// from either side as an entry.
void DielectricShares(DielectricLobe lobe, float cosThetaM, out float R, out float T)
{
    if (lobe.thinWalled)
    {
        const float F = FresnelDielectric(abs(cosThetaM), lobe.eta);
        R = ThinSlabReflectance(F);
        T = ThinSlabTransmittance(F) * lobe.transmission;
    }
    else
    {
        const float F = FresnelDielectric(cosThetaM, lobe.eta);
        R = F;
        T = (1.0 - F) * lobe.transmission;
    }
}

// The microfacet normal that connects wo and wi, facing +Z, and the direction the lobe is
// evaluated with (wi, or for a thin slab's transmission wi mirrored back through the
// slab). False for a pair no microfacet connects.
bool DielectricHalfVector(DielectricLobe lobe, float3 wo, float3 wi, out float3 wm,
                          out float3 wl, out float etap)
{
    const bool reflected = wo.z * wi.z > 0.0;
    etap = 1.0;
    wl   = wi;
    if (lobe.thinWalled)
    {
        if (!reflected)
            wl = float3(wi.x, wi.y, -wi.z);
        wm = wo + wl;
    }
    else
    {
        if (!reflected)
            etap = (wo.z > 0.0) ? lobe.eta : 1.0 / lobe.eta;
        wm = wi * etap + wo;
    }
    const float len2 = dot(wm, wm);
    if (len2 <= 0.0)
        return false;
    wm *= rsqrt(len2);
    if (wm.z < 0.0)
        wm = -wm;
    // Backfacing microfacets: either direction on the far side of its microfacet.
    return dot(wm, wl) * wl.z > 0.0 && dot(wm, wo) * wo.z > 0.0;
}

// f(wo, wi) without the cosine, radiance transport; 0 for a smooth interface (its lobes
// are deltas). A transmission's tint is the caller's.
float EvalDielectric(DielectricLobe lobe, float3 wo, float3 wi)
{
    if (DielectricIsSmooth(lobe) || wo.z == 0.0 || wi.z == 0.0)
        return 0.0;
    float3 wm, wl;
    float  etap;
    if (!DielectricHalfVector(lobe, wo, wi, wm, wl, etap))
        return 0.0;
    float R, T;
    DielectricShares(lobe, dot(wo, wm), R, T);
    const float D = DielectricGgxD(wm, lobe.alpha);
    const float G = DielectricGgxG2(wo, wl, lobe.alpha);
    const bool  reflected = wo.z * wi.z > 0.0;
    if (reflected || lobe.thinWalled)
        return D * G * (reflected ? R : T) / abs(4.0 * wo.z * wl.z);
    const float denom = dot(wi, wm) + dot(wo, wm) / etap;
    const float ft = D * G * T * abs(dot(wi, wm) * dot(wo, wm) / (wi.z * wo.z * denom * denom));
    return ft / (etap * etap);
}

// The solid-angle density SampleDielectric draws wi with; 0 for a smooth interface.
float PdfDielectric(DielectricLobe lobe, float3 wo, float3 wi)
{
    if (DielectricIsSmooth(lobe) || wo.z == 0.0 || wi.z == 0.0)
        return 0.0;
    float3 wm, wl;
    float  etap;
    if (!DielectricHalfVector(lobe, wo, wi, wm, wl, etap))
        return 0.0;
    float R, T;
    DielectricShares(lobe, dot(wo, wm), R, T);
    if (R + T <= 0.0)
        return 0.0;
    const bool  reflected = wo.z * wi.z > 0.0;
    const float visible   = DielectricGgxVisiblePdf(wo, wm, lobe.alpha);
    if (reflected || lobe.thinWalled)
        return visible / (4.0 * abs(dot(wo, wm))) * (reflected ? R : T) / (R + T);
    const float denom = dot(wi, wm) + dot(wo, wm) / etap;
    return visible * abs(dot(wi, wm)) / (denom * denom) * T / (R + T);
}

// Draws wi for wo: uc picks the lobe, u the microfacet. False when the draw leaves no
// direction (a reflection under the horizon, a refraction into total internal reflection).
bool SampleDielectric(DielectricLobe lobe, float3 wo, float uc, float2 u, out DielectricSample s)
{
    s = (DielectricSample)0;
    s.etap = 1.0;
    if (wo.z == 0.0)
        return false;
    float R, T;
    if (DielectricIsSmooth(lobe))
    {
        DielectricShares(lobe, wo.z, R, T);
        if (R + T <= 0.0)
            return false;
        // f = share / |cos| on one direction, picked with probability share / (R + T).
        s.delta  = true;
        s.weight = R + T;
        if (uc < R / (R + T))
        {
            s.wi = float3(-wo.x, -wo.y, wo.z);
            return true;
        }
        s.transmitted = true;
        if (lobe.thinWalled)
        {
            s.wi = -wo;
            return true;
        }
        float3 wt;
        float  etap;
        if (!DielectricRefract(wo, float3(0.0, 0.0, 1.0), lobe.eta, wt, etap))
            return false;
        s.wi      = wt;
        s.etap    = etap;
        s.weight /= etap * etap;
        return true;
    }
    const float3 wm = DielectricGgxSampleVisible(wo, lobe.alpha, u);
    DielectricShares(lobe, dot(wo, wm), R, T);
    if (R + T <= 0.0)
        return false;
    const float3 wr = reflect(-wo, wm);
    if (uc < R / (R + T))
    {
        if (wr.z * wo.z <= 0.0)
            return false;
        s.wi = wr;
    }
    else
    {
        s.transmitted = true;
        if (lobe.thinWalled)
        {
            if (wr.z * wo.z <= 0.0)
                return false;
            s.wi = float3(wr.x, wr.y, -wr.z);
        }
        else
        {
            float3 wt;
            float  etap;
            if (!DielectricRefract(wo, wm, lobe.eta, wt, etap) || wt.z * wo.z >= 0.0)
                return false;
            s.wi   = wt;
            s.etap = etap;
        }
    }
    // Eval and pdf rebuild the half vector from (wo, wi), so a sample's weight is exactly
    // what a light sample in the same direction would be weighed with.
    s.pdf = PdfDielectric(lobe, wo, s.wi);
    if (!(s.pdf > 0.0))
        return false;
    s.weight = EvalDielectric(lobe, wo, s.wi) * abs(s.wi.z) / s.pdf;
    return isfinite(s.weight);
}

// ==== Tint and absorption ====
// The transmission tint one crossing applies. baseColor tints the light through the whole
// object once: a thin slab is crossed in one event, a solid at two faces, sqrt each.
float3 DielectricCrossingTint(float3 baseColor, bool thinWalled)
{
    return thinWalled ? baseColor : sqrt(max(baseColor, 0.0));
}

// Beer-Lambert: light that travelled attenuationDistance inside keeps attenuationColor.
// A distance of 0 means no absorption.
float3 DielectricAttenuationCoefficient(float3 attenuationColor, float attenuationDistance)
{
    if (attenuationDistance <= 0.0)
        return float3(0.0, 0.0, 0.0);
    return -log(clamp(attenuationColor, 1e-4, 1.0)) / attenuationDistance;
}

float3 DielectricTransmittance(float3 sigmaA, float pathLength)
{
    return exp(-sigmaA * max(pathLength, 0.0));
}

#endif // CORE_BSDF_DIELECTRIC_HLSLI
