// rt_glass.hlsli - a BlendMode::Transmissive instance as a path vertex: the reference path
// tracer (PathTrace.cs.hlsl) and the ReSTIR PT bridge (RtxdiBridgePT.hlsli) share it.
//
// An instance with RT_INST_FLAG_TRANSMISSIVE (mask bit 0x10) is a dielectric a continuation ray
// crosses; the camera primary and the GBuffer never hold one (TransmissionApply and
// TransparentPass composite it over the traced image). Its BSDF mixes a clear interface
// (core/bsdf_dielectric.hlsli: exact-Fresnel reflection and refraction, a thin wall when its
// thickness is 0), weighted by the share s = transmission x (1 - metallic), with the opaque
// BSDF, whose F0 the material's IOR sets as in the raster surface layer. A refraction is tinted
// by baseColor once through the object (a thin wall at its one crossing, a solid by sqrt at each
// face), and a solid absorbs along the path inside it (Beer). A thin wall's coverage
// (baseColor.a) is a third event: the uncovered share passes straight on.
//
// The sun and the local lights reach a vertex through glass only by its next-event estimate,
// whose shadow ray takes each crossing's transmittance (RTGlassCrossing). So a glass vertex
// estimates only the light on its own side (RTGlassEval's reflectionOnly), and a path that
// refracted adds no sun disk itself - except straight through a smooth thin wall or its
// uncovered share, which is the shadow ray's own line: the vertex before the wall weighs the two
// (RTGlassStep.passThrough). No caustics.
//
// The includer provides pathMaterials' MaterialGPUData (material.hlsli), g_Instances
// (rt_instance_data.hlsli), HR_LoadHitTriangleAttributes (rt_hit_fetch.hlsli),
// core/bsdf_dielectric.hlsli and bsdf_sample.hlsli.

#ifndef RT_GLASS_HLSLI
#define RT_GLASS_HLSLI

struct RTGlass
{
    bool           active;   // the vertex is glass
    float          share;    // the clear interface's
    float          cover;    // a thin wall's coverage; 1 for a solid
    float3         tint;     // one crossing's
    float3         sigma;    // a solid's absorption per world unit
    bool           inside;   // wo is inside a solid
    DielectricLobe lobe;
};

bool RTGlassInstance(uint id) { return (g_Instances[id].rtFlags & RT_INST_FLAG_TRANSMISSIVE) != 0u; }

bool RTGlassSolid(uint materialsIdx, uint id)
{
    StructuredBuffer<MaterialGPUData> m = ResourceDescriptorHeap[materialsIdx];
    return m[id].thickness > 0;
}

// The glass vertex at a hit on instance id (materialsIdx: the path material table, indexed by
// InstanceID): its transmission fields with the hit's albedo, roughness and metallic; ng the
// geometric normal (outward on a solid: its hits load with frontFace), l the arriving direction.
RTGlass RTGlassAtHit(uint materialsIdx, uint id, float3 albedo, float rough, float metal, float3 ng, float3 l)
{
    StructuredBuffer<MaterialGPUData> m = ResourceDescriptorHeap[materialsIdx];
    const bool thin = m[id].thickness <= 0;
    RTGlass g = (RTGlass)0;
    g.active             = true;
    g.lobe.alpha         = rough * rough;
    g.lobe.eta           = max(m[id].ior, 1.0);
    g.lobe.transmission  = 1;
    g.lobe.thinWalled    = thin;
    g.inside             = !thin && dot(ng, l) > 0;
    g.share              = g.inside ? 1.0 : saturate(m[id].transmission) * (1 - saturate(metal));
    g.cover              = thin ? saturate(m[id].baseColor.a) : 1.0;
    g.tint               = DielectricCrossingTint(albedo, thin);
    g.sigma              = DielectricAttenuationCoefficient(m[id].attenuationColor, m[id].attenuationDistance);
    return g;
}

// The clear interface: f x |cos| toward l, a light sample's reflection side only, and the density
// its sampler draws l with.
float3 RTGlassEval(RTGlass g, float3 n, float3 v, float3 l, bool reflectionOnly)
{
    float3 t, b; OrthonormalBasis(n, t, b);
    const float3 wo = float3(dot(v, t), dot(v, b), dot(v, n)), wi = float3(dot(l, t), dot(l, b), dot(l, n));
    const bool transmitted = wo.z * wi.z < 0;
    if (transmitted && reflectionOnly) return 0;
    return EvalDielectric(g.lobe, wo, wi) * abs(wi.z) * (transmitted ? g.tint : 1.0);
}
float RTGlassPdf(RTGlass g, float3 n, float3 v, float3 l)
{
    float3 t, b; OrthonormalBasis(n, t, b);
    return PdfDielectric(g.lobe, float3(dot(v, t), dot(v, b), dot(v, n)), float3(dot(l, t), dot(l, b), dot(l, n)));
}

// The whole vertex toward l, as a light sample weighs it (the reflection side: its coverage, its
// clear share, the opaque rest), and the density of every event that leaves along l.
float3 RTGlassLightEval(RTGlass g, float3 n, float3 v, float3 l, float3 albedo, float rough, float metal,
                        float reflectance)
{
    const float3 opaque = g.share < 1 ? RTEvalBSDF(n, v, l, albedo, rough, metal, reflectance) : 0;
    return g.cover * lerp(opaque, RTGlassEval(g, n, v, l, true), g.share);
}
float RTGlassMixturePdf(RTGlass g, float3 n, float3 v, float3 l, float rough)
{
    const float opaque = g.share < 1 ? RTBsdfPdf(n, v, l, rough) : 0;
    return g.cover * lerp(opaque, RTGlassPdf(g, n, v, l), g.share);
}

// What a shadow ray along `direction` keeps across glass instance id at a candidate: the uncovered
// share, plus the clear share through the interface - a thin wall's slab, or one face of a solid
// and half its volume, a straight line through a solid standing for its two crossings - tinted as
// one crossing. The cosine is the candidate's shading normal's.
float3 RTGlassCrossing(uint materialsIdx, uint id, uint primitive, float2 bary, float3x3 worldToObject,
                       float3 direction)
{
    StructuredBuffer<MaterialGPUData> m = ResourceDescriptorHeap[materialsIdx];
    float cosTheta = 1;
    float3 p0, p1, p2, n0, n1, n2; float2 uv0, uv1, uv2;
    if (HR_LoadHitTriangleAttributes(id, primitive, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2))
    {
        // Normals take the inverse transpose, as RTLoadHitSurface's.
        const float3 ns = mul(n0 * (1 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y, worldToObject);
        if (dot(ns, ns) > 1e-12) cosTheta = abs(dot(normalize(ns), direction));
    }
    const float3 base  = m[id].baseColor.rgb;
    const float  share = saturate(m[id].transmission) * (1 - saturate(m[id].metalnessMax));
    const float  F     = FresnelDielectric(cosTheta, max(m[id].ior, 1.0));
    if (m[id].thickness <= 0)
    {
        const float cover = saturate(m[id].baseColor.a);
        return (1 - cover) + cover * share * ThinSlabTransmittance(F) * base;
    }
    return share * (1 - F) * DielectricCrossingTint(base, false) * DielectricTransmittance(
        DielectricAttenuationCoefficient(m[id].attenuationColor, m[id].attenuationDistance), .5 * m[id].thickness);
}

// One draw at a glass vertex from three uniforms, as an opaque vertex takes (a replayed path must
// find the same directions): u.z picks the event - the uncovered share straight on, the clear
// interface (u.z rescaled is its reflect / refract choice, u.xy its microfacet), or the opaque
// BSDF (u.z rescaled is its cosine / GGX choice, u.xy its lobe sample) - weighed as the one-sample
// mixture of the two. pdf: the density of l over every event (0 on a straight line or a smooth
// lobe); sunMode: how a miss after it adds the sun disk (0 weighed against this vertex's light
// sample, 1 whole: a smooth reflection, which no light sample reaches, 2 not: refracted light,
// which the shadow rays carry); passThrough: the shadow ray's own line, which keeps the vertex
// before; delta and transmitted for the ReSTIR bridge's reconnection rules.
struct RTGlassStep
{
    float3 l, weight;
    float  pdf;
    uint   sunMode;
    bool   passThrough, transmitted, delta;
};
bool RTGlassSample(RTGlass g, float3 n, float3 v, float3 albedo, float rough, float metal, float reflectance,
                   float3 u, out RTGlassStep step)
{
    step = (RTGlassStep)0;
    if (u.z >= g.cover)
    {
        step.l = -v; step.weight = 1; step.passThrough = true; step.delta = true; step.transmitted = true;
        return true;
    }
    float3 t, b; OrthonormalBasis(n, t, b);
    const float3 wo = float3(dot(v, t), dot(v, b), dot(v, n));
    const float  uEvent = u.z / g.cover;
    if (uEvent < g.share)
    {
        DielectricSample s;
        if (!SampleDielectric(g.lobe, wo, uEvent / g.share, u.xy, s)) return false;
        step.l = t * s.wi.x + b * s.wi.y + n * s.wi.z;
        step.transmitted = s.transmitted;
        if (s.delta)
        {
            // The opaque share has no density on a delta: the clear event's own weight.
            step.weight      = s.weight * (s.transmitted ? g.tint : 1.0);
            step.passThrough = s.transmitted && g.lobe.thinWalled;
            step.sunMode     = s.transmitted ? 2u : 1u;
            step.delta       = true;
            return all(isfinite(step.weight));
        }
    }
    else
    {
        const float uLobe = (uEvent - g.share) / max(1 - g.share, 1e-6);
        step.l = uLobe < 0.5 ? RTBasis(n, RTSampleCosine(u.xy)) : reflect(-v, RTBasis(n, SampleGGXVNDF(wo, rough * rough, u.x, u.y)));
    }
    const float3 wi = float3(dot(step.l, t), dot(step.l, b), dot(step.l, n));
    step.transmitted = wo.z * wi.z < 0;
    const bool  opaque = g.share < 1 && !step.transmitted;
    const float3 f = lerp(opaque ? RTEvalBSDF(n, v, step.l, albedo, rough, metal, reflectance) : 0,
                          RTGlassEval(g, n, v, step.l, false), g.share);
    const float  p = lerp(opaque ? RTBsdfPdf(n, v, step.l, rough) : 0, RTGlassPdf(g, n, v, step.l), g.share);
    if (!(p > 0)) return false;
    step.weight  = f / p;
    step.pdf     = g.cover * p;
    step.sunMode = step.transmitted ? 2u : 0u;
    return all(isfinite(step.weight));
}

#endif // RT_GLASS_HLSLI
