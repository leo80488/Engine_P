// RtxdiBridgePT.hlsli - the RTXDI Application Bridge, ReSTIR PT hooks.
//
// HLSL 2021 + 16-bit types (the PT headers use templates and uint16_t): a TU
// including this compiles with ShaderDiskCache::CompileFlags{true, true} and
// runs only where the device reports native 16-bit shader ops.
//
// Include order (the library's PT headers call these hooks): RtxdiBridge.hlsli,
// then this file's PART 1 (types and simple hooks) via
// RTXDI_BRIDGE_PT_TYPES, the library's PathTracerContext.hlsli, then PART 2
// (RAB_PathTrace) via RTXDI_BRIDGE_PT_TRACE, then the resampling headers.
//
// RAB_PT (the RtxdiPT core, RtxdiPT.cs.hlsl): the TU also provides g_PT
// (RAB_PTConstants, RtxdiBridgePTConstants.hlsli) before RtxdiBridge.hlsli,
// and the path tracer below is the engine's own (PathTrace.cs.hlsl's
// RTTraceSample): the same BSDF and its sampling, the same hit-surface fetch,
// alpha test and ray offsets, the same sun (a cone, MIS'd against BSDF hits
// of its disk) and local lights (one per vertex, by power) - written as
// calls into RTXDI's path tracer context. Without RAB_PT (the probe TU,
// RtxdiProbePT.cs.hlsl) PART 2 is the type-checking skeleton.
#ifndef RTXDI_BRIDGE_PT_TYPES_HLSLI
#define RTXDI_BRIDGE_PT_TYPES_HLSLI

#include "RtxdiBridge.hlsli"
#include "Rtxdi/PT/ReSTIRPTParameters.h"
#include "Rtxdi/PT/Reservoir.hlsli"

// The fake hit distance RTXDI gives an environment / distant-light vertex
// (InitialSamplingPathTracerContext); far beyond any scene the engine draws,
// near enough that the fake position keeps fp32 precision.
#ifndef RAB_DISTANT_LIGHT_DISTANCE
#define RAB_DISTANT_LIGHT_DISTANCE 1.0e5f
#endif

// One traced segment's result, as the path tracer context records it.
struct RAB_RayPayload
{
    float committedHitT;     // 0 = miss
    uint instanceID;
    uint primitiveIndex;
    float2 barycentrics;
};
float RAB_RayPayloadGetCommittedHitT(RAB_RayPayload p) { return p.committedHitT; }

// What the engine carries through one RTXDI path trace call.
struct RAB_PathTracerUserData
{
    uint pathType;           // RTXDI_PTPathTraceInvocationType_*
};
void RAB_PathTracerUserDataSetPathType(inout RAB_PathTracerUserData ptud, uint pathType) { ptud.pathType = pathType; }

// A reconnection's target at a receiver, in RGB (the library multiplies its
// float3 path throughput by it): the engine BSDF towards the sample (cosine
// included, RTEvalBSDF) times the radiance the sample carries. The library
// asks this only for a vertex a BSDF ray found - a surface, an emitter, the
// sky; a light sample's reconnection is RAB_GetReflectedBsdfRadianceForSurface's -
// so the segment is a continuation's.
float3 RAB_GetPTSampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface)
{
    if (!surface.valid) return 0;
    const float3 l = normalize(samplePosition - surface.worldPos);
#ifdef RAB_PT
    return max(RAB_PTBsdfCos(surface, l) * sampleRadiance *
               RAB_SegmentTransmittance(surface, samplePosition, kRabSegmentContinuation), 0);
#else
    return max(RTEvalBSDF(surface.normal, surface.viewDir, l, surface.material.albedo,
        surface.material.roughness, surface.material.metal, surface.material.reflectance) * sampleRadiance, 0);
#endif
}
// A light sample's reflected radiance with no glass on the way: RAB_RecordVertexNee's candidates
// (one transmittance ray for the chosen one, not one per candidate).
float3 RAB_ReflectedBsdfRadiance(float3 lightPosition, float3 lightRadiance, RAB_Surface surface)
{
    if (!surface.valid) return 0;
    const float3 l = normalize(lightPosition - surface.worldPos);
#ifdef RAB_PT
    return max(RAB_PTBsdfCos(surface, l) * lightRadiance, 0);
#else
    return max(RTEvalBSDF(surface.normal, surface.viewDir, l, surface.material.albedo, surface.material.roughness,
                          surface.material.metal, surface.material.reflectance) * lightRadiance, 0);
#endif
}
float3 RAB_GetReflectedBsdfRadianceForSurface(float3 lightPosition, float3 lightRadiance, RAB_Surface surface)
{
    float3 value = RAB_ReflectedBsdfRadiance(lightPosition, lightRadiance, surface);
#ifdef RAB_PT
    if (any(value > 0)) value *= RAB_SegmentTransmittance(surface, lightPosition, kRabSegmentLight);
#endif
    return value;
}

// The neighbour-selection G-buffer (the heuristic spatial mode is not built,
// RTXDI_SPATIAL_NEIGHBOR_SELECTION_BUFFER undefined) and the duplication map.
bool RAB_GetNeighborSelectionSurface(int2 pixel, out float3 normal, out float3 position, out float distance)
{
    const RAB_Surface s = RAB_GetGBufferSurface(pixel, false);
    normal = s.normal; position = s.worldPos; distance = s.viewDepth;
    return s.valid;
}
// How many of the 17x17 pixels around previousPixel shared its sample last
// frame (RTXDI_PTComputeDuplicationMap stores count / 255 in .x).
uint RAB_GetDuplicationMapCount(int2 previousPixel)
{
#ifdef RAB_PT
    if (!(g_PT.ptFlags & kRabPTDupMap) || g_PT.dupMapUav == 0) return 0;
    RWTexture2D<float2> map = ResourceDescriptorHeap[g_PT.dupMapUav];
    const int2 p = clamp(previousPixel, int2(0, 0), int2(g_RabBridge.width, g_RabBridge.height) - 1);
    return uint(map[p].x * 255.0 + 0.5);
#else
    return 0;
#endif
}

// The MIS weight of a next-event light sample against a BSDF ray finding the
// same light (the engine path tracer's power heuristic, RTMis). Engine local
// lights are not geometry - no BSDF ray reaches one - so only the sun has a
// partner; the light sample carries the BSDF pdf at the receiver it was drawn
// for (RAB_SamplePolymorphicLight), so the weight is right after a shift too
// whatever reconnection mode left in scatterPdf.
float RAB_PowerHeuristic(float a, float b) { return a * a / max(a * a + b * b, 1e-20); }
float RAB_GetMISWeightForNEE(uint lightIndex, RAB_LightSample lightSample, float3 lightDirection,
                             float invPartialJacobian, float scatterPdf)
{
#ifdef RAB_PT
    if (!lightSample.sun) return 1;
    return RAB_PowerHeuristic(lightSample.solidAnglePdf, lightSample.bsdfPdf);
#else
    return 1;
#endif
}
void RAB_LastBounceDenoiserCallback(float3 lightPosition, RAB_Surface surface, inout RAB_PathTracerUserData ptud) {}
void RAB_ReconnectionDenoiserCallback(RTXDI_PTReservoir sample, RAB_Surface surface, inout RAB_PathTracerUserData ptud) {}

#endif // RTXDI_BRIDGE_PT_TYPES_HLSLI

#if defined(RTXDI_BRIDGE_PT_TRACE) && !defined(RTXDI_BRIDGE_PT_TRACE_HLSLI)
#define RTXDI_BRIDGE_PT_TRACE_HLSLI
#ifdef RAB_PT
// ==== The path tracer ====
// A BSDF direction from three uniforms (the engine's RTSampleBSDF with its
// numbers given: u.xy the lobe sample, u.z the 50/50 cosine / GGX VNDF lobe
// choice). Always exactly three: a shift replays the same stream and must
// find the same directions on its own surfaces.
// The last glass draw (RAB_SampleBrdfRay at a glass surface): RAB_PathTrace's sun and ray-origin
// bookkeeping reads it.
static RTGlassStep g_RabGlassStep = (RTGlassStep)0;
RTXDI_BrdfRaySample RAB_SampleBrdfRay(RAB_Surface s, float3 u, out bool ok)
{
    RTXDI_BrdfRaySample brs = RTXDI_EmptyBrdfRaySample();
    ok = false;
    const float3 n = s.normal, v = s.viewDir;
    float3 l;
    if (s.material.glass && !(g_PT.ptFlags & kRabPTDiffuse))
    {
        // The reference path tracer's glass vertex from the same three numbers. Every event is a
        // delta to the library (outPdf 1, brdfTimesNoL its weight): no reconnection is made at glass
        // or across it, so a path through glass is reused by replay. Inside a solid, the segment that
        // arrived here (viewDepth long: a continuation hit's distance) was absorbed on the way.
        const RTGlass g = RAB_PTGlass(s);
        if (!RTGlassSample(g, n, v, s.material.albedo, s.material.roughness, s.material.metal, s.material.reflectance,
                           u, g_RabGlassStep)) return brs;
        brs.outDirection = g_RabGlassStep.l;
        brs.properties.SetDelta();
        if (g_RabGlassStep.transmitted) brs.properties.SetTransmission();
        brs.outPdf = 1;
        brs.brdfTimesNoL = g_RabGlassStep.weight * (g.inside ? DielectricTransmittance(g.sigma, s.viewDepth) : 1.0);
        ok = all(isfinite(brs.brdfTimesNoL));
        return brs;
    }
    if (g_PT.ptFlags & kRabPTDiffuse)
    {
        // -pt_diffuse: cosine sampling, BSDF x cos / pdf = albedo (u.z unused,
        // still drawn: the stream stays three numbers per bounce).
        l = RTBasis(n, float3(sqrt(u.x) * cos(2 * PI * u.y), sqrt(u.x) * sin(2 * PI * u.y), sqrt(1 - u.x)));
        brs.properties.SetDiffuse();
        if (dot(n, l) <= 0) return brs;
        brs.outDirection = l;
        brs.outPdf = saturate(dot(n, l)) / PI;
        brs.brdfTimesNoL = s.material.albedo * brs.outPdf;
        ok = brs.outPdf > 0 && all(isfinite(brs.brdfTimesNoL));
        return brs;
    }
    if (s.material.thinQ > 0)
    {
        // A leaf or a thin skin: the reference path tracer's draw from the same three numbers - u.z
        // picks the lobe behind (a diffuse transmission) or, rescaled, the opaque ones.
        float3 weight;
        float pdf;
        if (!RTThinSample(RAB_PTThin(s), n, v, s.material.albedo, s.material.roughness, s.material.metal,
                          s.material.reflectance, u, l, weight, pdf)) return brs;
        if (dot(n, l) < 0) { brs.properties.SetDiffuse(); brs.properties.SetTransmission(); }
        else if ((u.z - s.material.thinQ) / (1 - s.material.thinQ) < 0.5) brs.properties.SetDiffuse();
        else brs.properties.SetSpecular();
        brs.outDirection = l;
        brs.outPdf = pdf;
        brs.brdfTimesNoL = weight * pdf;
        ok = pdf > 0 && all(isfinite(brs.brdfTimesNoL));
        return brs;
    }
    if (u.z < 0.5)
    {
        l = RTBasis(n, RTSampleCosine(u.xy));
        brs.properties.SetDiffuse();
    }
    else
    {
        float3 t, b; OrthonormalBasis(n, t, b);
        const float3 vt = float3(dot(v, t), dot(v, b), dot(v, n));
        const float3 ht = SampleGGXVNDF(vt, s.material.roughness * s.material.roughness, u.x, u.y);
        l = reflect(-v, t * ht.x + b * ht.y + n * ht.z);
        brs.properties.SetSpecular();
    }
    if (dot(n, l) <= 0 || dot(n, v) <= 0) return brs;
    const float pdf = RTBsdfPdf(n, v, l, s.material.roughness);
    if (!(pdf > 0)) return brs;
    brs.outDirection = l;
    brs.outPdf = pdf;
    brs.brdfTimesNoL = RTEvalBSDF(n, v, l, s.material.albedo, s.material.roughness, s.material.metal, s.material.reflectance);
    ok = all(isfinite(brs.brdfTimesNoL));
    return brs;
}

// What a continuation ray found (PathTrace.cs.hlsl's loop): nothing (the
// environment), a surface it cannot load (the path ends), an untrusted proxy
// (an environment estimate ends the path), an unlit surface (it emits its base
// colour and does not scatter), or a surface that scatters.
static const uint kRabTraceMiss = 0u, kRabTraceUnloadable = 1u, kRabTraceUntrusted = 2u,
                  kRabTraceUnlit = 3u, kRabTraceSurface = 4u;
// traceGlass: glass is a vertex of this path (RAB_PathTrace); without it the ray passes through.
uint RAB_TraceContinuation(RayDesc ray, out RAB_RayPayload payload, out RAB_Surface hit, out float3 emission,
                           bool traceGlass)
{
    payload = (RAB_RayPayload)0;
    hit = RAB_EmptySurface();
    emission = 0;
    // As PathTrace.cs.hlsl: opaque hits in fixed function, back faces culled
    // (double-sided instances: TRIANGLE_CULL_DISABLE); mask 0x08, the GBuffer's
    // surfaces, and 0x10, Transmissive glass, a vertex of its own from either
    // side (rt_glass.hlsli) - the other forward-translucent surfaces are not
    // path vertices, as in the engine core.
    const bool glassVertices = traceGlass && !(g_PT.ptFlags & kRabPTDiffuse) && RAB_PTGlassScene();
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, glassVertices ? 0x18 : 0x08, ray);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            (q.CandidateTriangleFrontFace() ||
             (g_Instances[q.CandidateInstanceID()].rtFlags & (RT_INST_FLAG_DOUBLE_SIDED | RT_INST_FLAG_TRANSMISSIVE))) &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), true))
            q.CommitNonOpaqueTriangleHit();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return kRabTraceMiss;
    const uint id = q.CommittedInstanceID();
    payload.committedHitT = q.CommittedRayT();
    payload.instanceID = id;
    payload.primitiveIndex = q.CommittedPrimitiveIndex();
    payload.barycentrics = q.CommittedTriangleBarycentrics();
    float normalStrength = 1;
    if (g_PT.pathMaterialsIdx != 0)
    {
        StructuredBuffer<MaterialGPUData> pathMaterials = ResourceDescriptorHeap[g_PT.pathMaterialsIdx];
        normalStrength = pathMaterials[id].normalStrength;
    }
    // A solid's surface keeps its outward normal from either side (RAB_PTGlass tells them apart).
    const bool glass = glassVertices && RTGlassInstance(id);
    const bool solid = glass && RTGlassSolid(g_PT.pathMaterialsIdx, id);
    RTHitSurface s;
    if (!RTLoadHitSurface(id, payload.primitiveIndex, payload.barycentrics, q.CommittedObjectToWorld3x4(),
                          q.CommittedWorldToObject3x4(), ray.Origin + ray.Direction * payload.committedHitT,
                          solid || q.CommittedTriangleFrontFace(), 0, s, normalStrength))
        return kRabTraceUnloadable;
    if (!s.trusted) return kRabTraceUntrusted;
    hit.worldPos = s.P;
    hit.normal = s.Nshade;
    hit.geoNormal = s.Ngeo;
    hit.viewDir = -ray.Direction;
    hit.viewDepth = payload.committedHitT;
    hit.material.albedo = s.albedo;
    hit.material.roughness = s.rough;
    hit.material.metal = s.metal;
    hit.material.reflectance = s.reflectance;
    hit.material.F0 = lerp((0.16 * s.reflectance * s.reflectance).xxx, s.albedo, s.metal);
    hit.material.id = id;
    hit.material.glass = glass;
    if (!glass && g_PT.pathMaterialsIdx != 0)
    {
        StructuredBuffer<MaterialGPUData> pathMaterials = ResourceDescriptorHeap[g_PT.pathMaterialsIdx];
        RAB_PTSetThin(hit, pathMaterials[id]);
    }
    hit.valid = true;
    emission = s.emissive;
    if (s.unlit) { emission += s.albedo; return kRabTraceUnlit; }
    return kRabTraceSurface;
}

// The sky along a direction, without the sun's disk: the constant
// environment (-pt_environment) or the sky cube x its IBL scale.
float3 RAB_SkyRadiance(float3 direction)
{
    if (g_PT.ptFlags & kRabPTConstantEnvironment) return g_PT.constantEnvironment;
    if (g_PT.skyIdx == 0) return 0;
    TextureCube<float4> sky = ResourceDescriptorHeap[g_PT.skyIdx];
    return sky.SampleLevel(gSampWrap, direction, 0).rgb * g_PT.environmentScale;
}
// A BSDF ray that left the scene: the sky, plus the sun's disk weighted
// against the sun's next-event estimate (the engine path tracer's miss).
// sunMode (rt_glass.hlsli's RTGlassStep): 0 weighed against the light sample of
// the vertex the ray left, 1 whole (a smooth glass reflection), 2 not at all
// (light refracted through glass, which the shadow rays carry).
float3 RAB_EnvironmentRadiance(float3 direction, float bsdfPdf, uint sunMode = 0u)
{
    float3 value = RAB_SkyRadiance(direction);
    if (sunMode != 2u && any(g_PT.sunColor > 0) && dot(direction, normalize(-g_PT.sunDirection)) >= g_PT.sunCosRadius)
    {
        const float sunPdf = 1.0 / (2 * PI * (1 - g_PT.sunCosRadius));
        const float mis = ((g_PT.ptFlags & kRabPTNoSunNee) || sunMode == 1u) ? 1.0 : RAB_PowerHeuristic(bsdfPdf, sunPdf);
        value += g_PT.sunColor * mis / (PI * (1 - g_PT.sunCosRadius * g_PT.sunCosRadius));
    }
    return value;
}

// One local light by power: the alias table (one uniform picks the row and
// its coin), or uniform over the list without one.
struct RAB_PTLightAlias { uint light, aliasLight; float threshold, pdfLight, pdfAlias; uint pad0, pad1, pad2; };
bool RAB_SelectLocalLight(float u, out uint index, out float pdf)
{
    index = 0; pdf = 0;
    if (g_PT.lightAliasIdx != 0 && g_PT.activeLocalLightCount != 0)
    {
        StructuredBuffer<RAB_PTLightAlias> table = ResourceDescriptorHeap[g_PT.lightAliasIdx];
        const float x = u * g_PT.activeLocalLightCount;
        const uint row = min(uint(x), g_PT.activeLocalLightCount - 1);
        const RAB_PTLightAlias entry = table[row];
        const bool primary = (x - float(row)) < entry.threshold;
        index = primary ? entry.light : entry.aliasLight;
        pdf = primary ? entry.pdfLight : entry.pdfAlias;
        return pdf > 0;
    }
    if (g_RabBridge.lightCount == 0) return false;
    index = min(uint(u * g_RabBridge.lightCount), g_RabBridge.lightCount - 1);
    pdf = 1.0 / g_RabBridge.lightCount;
    return true;
}

// A next-event estimate towards one light sample: its reflected radiance over
// its pdfs, times the visibility and the MIS weight (0 when it contributes
// nothing - no ray is traced then, as the engine path tracer does).
float3 RAB_NeeEstimate(RAB_Surface s, uint lightIndex, RAB_LightSample ls, float selectionPdf)
{
    if (!(ls.solidAnglePdf > 0) || !(selectionPdf > 0)) return 0;
    const float3 f = RAB_GetReflectedBsdfRadianceForSurface(RAB_LightSamplePosition(ls), RAB_LightSampleRadiance(ls), s);
    if (!any(f > 0)) return 0;
    if (!RAB_GetConservativeVisibility(s, ls)) return 0;
    return f / (ls.solidAnglePdf * selectionPdf) *
        RAB_GetMISWeightForNEE(lightIndex, ls, normalize(RAB_LightSamplePosition(ls) - s.worldPos), ls.solidAnglePdf, ls.bsdfPdf);
}
// A light's sample as RTXDI stores it: index + 16-bit uv, and the sample the
// QUANTIZED uv gives (what a shift will rebuild from the reservoir).
RAB_LightSample RAB_DrawLightSample(RAB_Surface s, uint lightIndex, float2 uv, out RTXDI_SampledLightData data)
{
    data = RTXDI_SampledLightData_CreateInvalidData();
    RTXDI_SampledLightData_SetLightData(data, lightIndex);
    RTXDI_SampledLightData_SetUVData(data, uv);
    return RAB_SamplePolymorphicLight(RAB_LoadLightInfo(lightIndex, false), s,
                                      RTXDI_SampledLightData_GetUVDataFloat2(data));
}

// The engine path tracer's two next-event estimates at a vertex: the sun, and
// one local light by power (initial stream only). With candidates > 1 the
// local light is resampled (power RIS, RTXDI Medium's NEE): every candidate's
// unshadowed contribution is its target, the chosen one alone is traced, and
// W = sum(w) / (N p_hat) is its contribution weight. The context excludes the
// "nee pdf" from the reservoir's target, so 1 / W goes there: the target stays
// the BSDF x radiance / solid-angle pdf a shift re-evaluates at a new receiver.
template<typename PTContextType>
void RAB_RecordVertexNee(inout RTXDI_PathTracerContext<PTContextType> ctx, RAB_Surface s, inout RTXDI_RandomSamplerState rng,
                         uint candidates)
{
    RTXDI_SampledLightData data;
    if (RAB_SunEnabled())
    {
        const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const RAB_LightSample ls = RAB_DrawLightSample(s, kRabSunLightIndex, uv, data);
        ctx.RecordNeeLightSample(data, RAB_NeeEstimate(s, kRabSunLightIndex, ls, 1.0), 1.0, ls.bsdfPdf, ls, rng);
    }
    if (candidates <= 1)
    {
        uint index; float pdf;
        if (RAB_SelectLocalLight(RTXDI_GetNextRandom(rng), index, pdf))
        {
            const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
            const RAB_LightSample ls = RAB_DrawLightSample(s, index, uv, data);
            ctx.RecordNeeLightSample(data, RAB_NeeEstimate(s, index, ls, pdf), pdf, ls.bsdfPdf, ls, rng);
        }
        return;
    }
    float weightSum = 0, chosenTarget = 0;
    float3 chosenValue = 0;
    uint chosenIndex = 0;
    RAB_LightSample chosen = RAB_EmptyLightSample();
    RTXDI_SampledLightData chosenData = RTXDI_SampledLightData_CreateInvalidData();
    [loop] for (uint i = 0; i < candidates; ++i)
    {
        uint index; float pdf;
        if (!RAB_SelectLocalLight(RTXDI_GetNextRandom(rng), index, pdf)) continue;
        const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const RAB_LightSample ls = RAB_DrawLightSample(s, index, uv, data);
        if (!(ls.solidAnglePdf > 0)) continue;
        float3 value = RAB_ReflectedBsdfRadiance(RAB_LightSamplePosition(ls), RAB_LightSampleRadiance(ls), s) /
                       ls.solidAnglePdf;
#if RAB_MEDIA
        // The fog costs no ray: unlike the glass it is in every candidate's target.
        if (any(value > 0)) value *= RAB_FogToward(s, RAB_LightSamplePosition(ls), kRabSegmentLight);
#endif
        const float target = RTXDI_Luminance(value);
        const float w = target / pdf;
        if (!(w > 0) || !isfinite(w)) continue;
        weightSum += w;
        if (RTXDI_GetNextRandom(rng) * weightSum < w)
        {
            chosen = ls; chosenData = data; chosenIndex = index; chosenValue = value; chosenTarget = target;
        }
    }
    if (!(chosenTarget > 0)) return;
    const float W = weightSum / (float(candidates) * chosenTarget);
    if (!RAB_GetConservativeVisibility(s, chosen)) return;
    // The glass on the way, for the chosen one alone: the candidates' target leaves it out, and the
    // contribution (and what a shift re-evaluates) keeps it.
    chosenValue *= RAB_GlassTransmittance(s, RAB_LightSamplePosition(chosen));
    const float mis = RAB_GetMISWeightForNEE(chosenIndex, chosen, normalize(RAB_LightSamplePosition(chosen) - s.worldPos),
                                             chosen.solidAnglePdf, chosen.bsdfPdf);
    ctx.RecordNeeLightSample(chosenData, chosenValue * W * mis, 1.0 / W, chosen.bsdfPdf, chosen, rng);
}

// The path tracer. Bounce depth 2 leaves the primary surface; the loop runs
// while the depth is <= the context's maximum, so the last segment's hit is
// still recorded (its emission, or a shift's reconnection test) - next-event
// estimates beyond the maximum path length are refused by the context.
// Random numbers: the replay stream drives ONLY the path's construction (the
// three BSDF numbers per bounce); next-event, roulette and every Record*
// selection use the initial stream, so a shift (which records nothing and
// draws no roulette) stays aligned with the path it replays.
template<typename PTContextType>
void RAB_PathTrace(inout RTXDI_PathTracerContext<PTContextType> ctx,
                   inout RTXDI_PathTracerRandomContext ptRandContext,
                   inout RAB_PathTracerUserData ptud)
{
    // -rtxdi_pt_unified=1: x1's direct light joins the reservoir as paths of
    // length 2, so the final shading needs no separate DI. The context records
    // next-event light at bounceDepth + 1 and its walk starts at depth 2 (the
    // vertex after x1), so x1's estimates run at depth 1 (the context has no
    // setter; its state is the bridge's to set, the library is untouched). Such a
    // sample's reconnection vertex is the light at path length 2: a shift
    // reconnects the new primary straight to it, no replay.
    if ((g_PT.ptFlags & kRabPTUnified) && ctx.ShouldSampleNee())
    {
        ctx.ctx.ptState.bounceDepth = (uint16_t)1;
        RAB_RecordVertexNee(ctx, ctx.GetIntersectionSurface(), ptRandContext.initialRandomSamplerState, g_PT.neeCandidatesX1);
        ctx.ctx.ptState.bounceDepth = (uint16_t)2;
    }
    // The sun disk's weight on a miss (RAB_EnvironmentRadiance): the density of the last direction a
    // light sample could also reach, and how glass on the way changed that. A straight crossing of a
    // thin wall keeps the vertex before it.
    float misPdf = 0;
    uint sunMode = 0u;
    [loop] while (!ctx.IsPathTerminated() && ctx.GetBounceDepth() <= ctx.GetMaxPathBounce())
    {
        ctx.BeginPathState();
        const RAB_Surface surface = ctx.GetIntersectionSurface();
        const float3 u = float3(RTXDI_GetNextRandom(ptRandContext.replayRandomSamplerState),
                                RTXDI_GetNextRandom(ptRandContext.replayRandomSamplerState),
                                RTXDI_GetNextRandom(ptRandContext.replayRandomSamplerState));
        bool ok;
        const RTXDI_BrdfRaySample brs = RAB_SampleBrdfRay(surface, u, ok);
        if (!ok) break;
        ctx.SetBrdfRaySample(brs);
        if (!ctx.ValidContinuationRayBrdfOverPdf()) break;
        float survival = 1;
        if (ctx.ShouldRunRussianRoulette())
        {
            // The engine's survival (max throughput channel, clamped), with
            // this bounce's BSDF weight included.
            const float3 t = ctx.GetPathThroughput() * ctx.GetContinuationRayBrdfOverPdf();
            survival = clamp(max(t.x, max(t.y, t.z)), 0.05, 0.95);
            if (RTXDI_GetNextRandom(ptRandContext.initialRandomSamplerState) >= survival) break;
            ctx.RecordRussianRouletteProbability(survival);
        }
        ctx.MultiplyPathThroughput(ctx.GetContinuationRayBrdfOverPdf() / survival);

        const bool glassVertex = surface.material.glass && !(g_PT.ptFlags & kRabPTDiffuse);
        if (!glassVertex) { misPdf = brs.outPdf; sunMode = 0u; }
        else if (!g_RabGlassStep.passThrough) { misPdf = g_RabGlassStep.pdf; sunMode = g_RabGlassStep.sunMode; }

        RayDesc ray;
        // A glass vertex leaves on the side its ray goes to; light through a leaf by its back.
        ray.Origin = surface.worldPos + (glassVertex ? (dot(brs.outDirection, surface.geoNormal) >= 0 ? surface.geoNormal
                                                                                                   : -surface.geoNormal)
                                                     : (dot(brs.outDirection, surface.normal) >= 0 ? surface.normal
                                                                                                  : -surface.normal)) * 0.001;
        ray.Direction = brs.outDirection;
        ray.TMin = 0.001;
        ray.TMax = 10000;
        ctx.SetContinuationRay(ray);
        if (!ctx.AnalyzePathReconnectibilityBeforeTrace()) break;

        RAB_RayPayload payload;
        RAB_Surface hit;
        float3 emission;
        const uint outcome = RAB_TraceContinuation(ray, payload, hit, emission, true);
        ctx.SetTraceResult(payload);
        // The froxel rule's light from the fog along this segment, as light its end sends back along
        // it, and a miss's fog past the lit volume, which the sky's light takes (0 and 1 otherwise).
        float3 inScatter = 0, skyRest = 1;
#if RAB_MEDIA
        // The fog on this continuation segment, before anything at its end is recorded: the prefix
        // takes it, and so does the suffix a reconnection keeps when the segment leaves the
        // reconnection vertex or a later one (the context's rule for the BSDF weights there). A
        // segment that ran inside a glass solid crossed no fog.
        if (outcome != kRabTraceUnloadable && !(hit.material.glass && RAB_PTGlass(hit).inside))
        {
            const float3 fog = RAB_FogContinuation(ray.Origin, ray.Direction,
                                                   outcome == kRabTraceMiss ? kMediumFar : payload.committedHitT,
                                                   inScatter, skyRest);
            ctx.MultiplyPathThroughput(fog);
            if (ctx.ctx.ptState.bounceDepth > ctx.ctx.ptState.rcVertexLength) ctx.ctx.ptState.rcPathThroughput *= fog;
            // The throughput holds the segment's transmittance now: a reconnection to the segment's end
            // weighs this light by its own segment's.
            inScatter /= max(fog, 1e-4);
        }
#endif
        // A hybrid shift (the context that samples no next-event light) replays
        // a path up to the vertex before its reconnection vertex and ends there
        // (RecordPathIntersection terminates it). Only a path with no reconnection
        // vertex replays past that point to its light - the case upstream RTXDI
        // PR #42 changes inside the library; the vendored library lets it through
        // unguarded. The bridge refuses it: the replay records no light, its
        // shifted contribution is 0 in the forward and the reverse
        // (bias-correction) shift alike, so such a path stays with the pixel that
        // traced it - unbiased by construction, at the price of no reuse along
        // purely specular chains. On the all-mirror box (every reuse such a replay,
        // tools/verify_rtxdi_pt.py) the refusal is +0.03% and the library's replay
        // (-rtxdi_pt_replay_no_rc=1) +0.13%, both inside the 2% contract.
        const bool refuseLight = !ctx.ShouldSampleNee() && !(g_PT.ptFlags & kRabPTReplayNoRc);
        if (outcome == kRabTraceMiss || outcome == kRabTraceUntrusted)
        {
            ctx.RecordPathRadianceMiss(ptRandContext.initialRandomSamplerState);
            if (!refuseLight)
                ctx.RecordEnvironmentMapLightSample((outcome == kRabTraceMiss ? RAB_EnvironmentRadiance(ray.Direction, misPdf, sunMode)
                                                                              : RAB_SkyRadiance(ray.Direction)) * skyRest + inScatter,
                                                    surface, ptRandContext.initialRandomSamplerState);
            break;
        }
        if (outcome == kRabTraceUnloadable) break;
        ctx.RecordPathIntersection(hit);
        // Glass is never a reconnection vertex: a shift could not evaluate its deltas from another
        // direction.
        if (hit.material.glass) ctx.ctx.SetIsLastVertexFar(false);
        if (ctx.IsPathTerminated()) break;
        if (refuseLight && ctx.GetBounceDepth() >= ctx.GetMaxPathBounce()) break;
#if SHARC_QUERY
        // SHaRC: a secondary vertex the path's footprint covers ends the path in
        // the radiance cache - its cached light and its own emission, recorded as
        // the light at this vertex (RtxdiBridgeSharc.hlsli). Before the emission
        // record: the cached value already holds it. At the last vertex the path
        // may take (initial sampling's cap) any entry ends it: past the cap the
        // path has nothing, and the cache's footprint error is smaller than that
        // truncation. A shift's replay never reaches it (it ends before its
        // reconnection vertex, or is refused at the end of a path without one), so
        // initial sampling and reuse still end paths by one rule.
        float3 cached;
        const bool last = ctx.ShouldSampleNee() && ctx.GetBounceDepth() >= ctx.GetMaxPathBounce() &&
                          !(g_PT.sharcFlags & kRabSharcFootprintOnly);
        const bool eligible = outcome == kRabTraceSurface &&
                              (last ? (!(g_PT.sharcFlags & kRabSharcGateLast) || RAB_SharcDiffuseLike(hit))
                                    : (RAB_SharcDiffuseLike(hit) && RAB_SharcTerminates(surface, brs, payload.committedHitT, hit.worldPos)));
        const bool found = eligible && RAB_SharcQuery(hit, emission, cached);
        if (ctx.ShouldSampleNee()) PTCountSharcSlot(10, eligible && !found);
        if (found)
        {
            if (ctx.ShouldSampleNee()) PTCountSharc(true);   // initial sampling's paths
            if (ctx.ShouldSampleEmissiveSurfaces() && any(cached + inScatter > 0))
                ctx.RecordEmissiveLightSample(cached + inScatter, surface, ptRandContext.initialRandomSamplerState);
            break;
        }
#endif
        if (ctx.ShouldSampleEmissiveSurfaces() && any(emission + inScatter > 0))
            ctx.RecordEmissiveLightSample(emission + inScatter, surface, ptRandContext.initialRandomSamplerState);
        if (outcome == kRabTraceUnlit) break;
        // Inside a solid no light sample reaches a vertex (lights are outside, and light through glass
        // arrives by the shadow rays of the vertices outside).
        const bool insideGlass = hit.material.glass && RAB_PTGlass(hit).inside;
        if (ctx.ShouldSampleNee() && !insideGlass)
            RAB_RecordVertexNee(ctx, hit, ptRandContext.initialRandomSamplerState, g_PT.neeCandidates);
        ctx.IncreaseBounceDepth();
    }
}
#else
// The probe skeleton: walks the context's whole interface in the order
// PathTracerContext.hlsli documents, every continuation ray a miss (no TLAS
// query, no hit-point material fetch), so each path ends at its first
// vertex. Walking every method instantiates all of them for both context
// types (initial sampling, hybrid shift): a type check of the library
// against this bridge.
template<typename PTContextType>
void RAB_PathTrace(inout RTXDI_PathTracerContext<PTContextType> ctx,
                   inout RTXDI_PathTracerRandomContext ptRandContext,
                   inout RAB_PathTracerUserData ptud)
{
    RTXDI_RandomSamplerState rng = ptRandContext.initialRandomSamplerState;
    [loop] while (!ctx.IsPathTerminated() && ctx.GetBounceDepth() < ctx.GetMaxPathBounce())
    {
        const RAB_Surface surface = ctx.GetIntersectionSurface();
        RTXDI_BrdfRaySample brs = RTXDI_EmptyBrdfRaySample();
        ctx.SetBrdfRaySample(brs);
        if (!ctx.ValidContinuationRayBrdfOverPdf()) break;
        if (ctx.ShouldRunRussianRoulette()) ctx.RecordRussianRouletteProbability(1.0);
        ctx.MultiplyPathThroughput(ctx.GetContinuationRayBrdfOverPdf());

        RayDesc ray;
        ray.Origin = RAB_GetSurfaceWorldPos(surface);
        ray.Direction = RAB_GetSurfaceNormal(surface);
        ray.TMin = 0.0;
        ray.TMax = g_RabBridge.rayTMax;
        ctx.SetContinuationRay(ray);
        if (!ctx.AnalyzePathReconnectibilityBeforeTrace()) break;

        RAB_RayPayload payload = (RAB_RayPayload)0;
        ctx.SetTraceResult(payload);
        if (RAB_RayPayloadGetCommittedHitT(payload) <= 0.0)
        {
            ctx.RecordPathRadianceMiss(rng);
            ctx.RecordEnvironmentMapLightSample(float3(0, 0, 0), surface, rng);
            break;
        }
        ctx.RecordPathIntersection(RAB_EmptySurface());
        if (ctx.ShouldSampleEmissiveSurfaces())
            ctx.RecordEmissiveLightSample(float3(0, 0, 0), surface, rng);
        if (ctx.ShouldSampleNee())
            ctx.RecordNeeLightSample(RTXDI_SampledLightData_CreateInvalidData(), float3(0, 0, 0), 1.0, 1.0,
                                     RAB_EmptyLightSample(), rng);
        ctx.IncreaseBounceDepth();
    }
    ptRandContext.initialRandomSamplerState = rng;
}
#endif
#endif
