// RtxdiPT.cs.hlsl - the RTXDI ReSTIR PT core: PathTracePass's rtxdi mode.
//
// RTXDI's ReSTIR PT (initial path sampling, hybrid-shift temporal and spatial
// reuse, the boiling filter) over the engine's own path tracer
// (RtxdiBridgePT.hlsli, RAB_PT): the GBuffer is the primary vertex, paths
// leave it at bounce depth 2, and the RTXDI reservoir carries every path of
// length >= 2 (emissive and sky hits seen from the primary, next-event light
// at the secondary vertices, deeper bounces up to maxBounceDepth). The
// primary's direct light stays outside the reservoir, as in RTXDI's own
// sample: its local lights come from ReSTIR DI (RestirDIResult, where the DI
// shell has an estimate) or one next-event sample, its sun from the engine
// path tracer's cone estimate (MIS'd against the BSDF hits of the disk the
// reservoir records). Primary emission is not added: the GBuffer seeds it into
// the HDR target, as for the engine path tracer.
//
//   RtxdiPTClear     the [PT_RTXDI] counters
//   RtxdiPTInitial   GenerateInitialSamples; stores the primary surfaces for the
//                    next frame's temporal pass (the bridge's ping-pong)
//   RtxdiPTTemporal  RTXDI_PTTemporalResampling (in place over the initial)
//   RtxdiPTBoiling   RTXDI's PT boiling filter (16x16 groups, whole grid)
//   RtxdiPTSpatial   RTXDI_PTSpatialResampling
//   RtxdiPTShade     primary direct + targetFunction x W into PathTraceResult,
//                    progressive when the scene is static; under DLSS-RR also
//                    the four RR guides
//
// The SHaRC radiance cache (RAB_SHARC; RtxdiBridgeSharc.hlsli), in the order
// the core runs them before the kernels above, which it then compiles with
// SHARC_QUERY (paths end in the cache):
//   RtxdiPTSharcClear    zero the cache (a new world, a new capacity)
//   RtxdiPTSharcUpdate   sparse camera-rooted paths deposit into it (SHARC_UPDATE)
//   RtxdiPTSharcResolve  this frame's deposits into each entry's history
//
// RAB_MEDIA (the core's media builds, while the frame has a medium): every segment the bridge
// weighs crosses the fog (RtxdiBridge.hlsli's RAB_SegmentTransmittance) - a light segment by its
// whole transmittance, a continuation by g_PT.mediaFlags' rule. No segment scatters: the camera
// rays' fog is the raster fog's, and the light the fog scatters into a secondary segment is lost.
//
// HLSL 2021 + 16-bit types (ShaderDiskCache::CompileFlags{true, true}); only
// on a device with native 16-bit shader ops (PathTracePass checks).
//
// PT_REFERENCE_GGX before any include, as PathTrace.cs.hlsl: the path
// tracer's GGX (brdf.hlsli) without the raster NDF floor, which clips a
// roughness-0.045 lobe to ~1/1900 of its peak - without it every near-mirror
// reflection is dark (the all-mirror box -33%, tools/verify_rtxdi_pt.py).
#define PT_REFERENCE_GGX 1
#include "RtxdiBridgeConstants.hlsli"
#include "RtxdiBridgePTConstants.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"

// RTXDI's PT headers read their parameters as g_Const.restirPT (FullSample's
// name; RtxdiProbePT.cs.hlsl).
struct RtxdiPTConstants
{
    RTXDI_PTParameters restirPT;
};

cbuffer Params : register(b0)
{
    RAB_BridgeConstants g_RabBridge;
    RTXDI_RuntimeParameters g_Runtime;
    RtxdiPTConstants g_Const;
    RAB_PTConstants g_PT;
};

// The library's resources, bindless (RtxdiProbe.cs.hlsl explains the pattern).
RWStructuredBuffer<RTXDI_PackedPTReservoir> RtxdiPTReservoirs() { return ResourceDescriptorHeap[g_PT.reservoirUav]; }
Buffer<float2> RtxdiNeighborOffsets() { return ResourceDescriptorHeap[g_PT.neighborOffsetsSrv]; }
#define RTXDI_PT_RESERVOIR_BUFFER RtxdiPTReservoirs()
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
#define RTXDI_ENABLE_BOILING_FILTER 1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 16

#define RAB_PT 1
#include "RtxdiBridgePT.hlsli"
#include "Rtxdi/PT/PathTracerContext.hlsli"
#if RAB_SHARC
#include "RtxdiBridgeSharc.hlsli"
#endif
#define RTXDI_BRIDGE_PT_TRACE
#include "RtxdiBridgePT.hlsli"
#include "Rtxdi/PT/InitialSampling.hlsli"
#include "Rtxdi/PT/TemporalResampling.hlsli"
#include "Rtxdi/PT/SpatialResampling.hlsli"
#include "Rtxdi/PT/BoilingFilter.hlsli"

// RTXDI's duplication map (-rtxdi_pt_duplication_map=1, and under the DLSS-RR
// recipe), and the recipe's smoothed stagnancy maps (both UAVs: the last
// frame's is only read, never transitioned).
RWTexture2D<uint> RtxdiPTSampleIds() { return ResourceDescriptorHeap[g_PT.sampleIdUav]; }
RWTexture2D<float2> RtxdiPTDuplicationMap() { return ResourceDescriptorHeap[g_PT.dupMapUav]; }
RWTexture2D<float> RtxdiPTStagnancy() { return ResourceDescriptorHeap[g_PT.stagnancyUav]; }
RWTexture2D<float> RtxdiPTPrevStagnancy() { return ResourceDescriptorHeap[g_PT.prevStagnancyUav]; }
#define RTXDI_PT_SAMPLE_ID_TEXTURE RtxdiPTSampleIds()
#define RTXDI_PT_DUPLICATION_MAP RtxdiPTDuplicationMap()
#define RTXDI_PT_SMOOTHED_DUPLICATION_MAP RtxdiPTStagnancy()
#define RTXDI_PT_PREV_SMOOTHED_DUPLICATION_MAP RtxdiPTPrevStagnancy()
#define RTXDI_PT_ENABLE_DUPLICATION_MAP_COUNT 1
#include "Rtxdi/PT/DuplicationMap.hlsli"
#include "Rtxdi/PT/Decorrelation.hlsli"

// [PT_RTXDI] counters (PTRtxdiCore.cpp reads them back under -pt_raycounts):
// 0 primary surfaces, 1 valid initial reservoirs, 2 temporal history picked,
// 3 spatial neighbour picked, 4 boiled, 5 nonfinite pixels, 6 shaded with a
// valid reservoir, 7 primaries lit by RestirDIResult; SHaRC (RAB_SHARC):
// 8 initial paths the cache ended, 9 update vertices deposited, 10 initial
// paths the cache would have ended but held no entry for. The DLSS-RR recipe:
// 11 pixels shaded with the preserved initial sample instead of the reused
// one, 12 final reservoirs flagged as fireflies.
static const uint kPTCounterCount = 13u;
void PTCount(uint slot, bool value)
{
    if (!(g_PT.ptFlags & kRabPTCounters)) return;
    const uint n = WaveActiveCountBits(value);
    if (WaveIsFirstLane() && n != 0)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[g_PT.counterUav];
        InterlockedAdd(counters[slot], n);
    }
}

uint2 PTSize() { return uint2(g_RabBridge.width, g_RabBridge.height); }

[numthreads(8, 1, 1)]
void RtxdiPTClear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= kPTCounterCount) return;
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[g_PT.counterUav];
    counters[tid.x] = 0;
}

[numthreads(8, 8, 1)]
void RtxdiPTInitial(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(px), false);
    RAB_StoreSurface(int2(px), surface);
    RTXDI_PTReservoir r = RTXDI_EmptyPTReservoir();
    if (surface.valid && !(g_PT.ptFlags & kRabPTDirectOnly))
    {
        RTXDI_PathTracerRandomContext ptRand = RTXDI_InitializePathTracerRandomContext(px, g_PT.frameIndex, 1, 2);
        RAB_PathTracerUserData ptud = (RAB_PathTracerUserData)0;
        RTXDI_PTInitialSamplingRuntimeParameters isr = RTXDI_EmptyPTInitialSamplingRuntimeParameters();
        isr.cameraPos = g_RabBridge.camera;
        isr.prevCameraPos = g_RabBridge.prevCamera;
        isr.prevPrevCameraPos = g_PT.prevPrevCamera;
        r = GenerateInitialSamples(g_Const.restirPT.initialSampling, isr, g_Const.restirPT.reconnection, ptRand, surface, ptud);
    }
    PTCount(0, surface.valid);
    PTCount(1, RTXDI_IsValidPTReservoir(r) && r.weightSum > 0);
    RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.initialPathTracerOutputBufferIndex);
    // The unresampled initial sample, kept for the final shading's decorrelation
    // (the DLSS-RR recipe; RTXDI's slot 2; the temporal pass overwrites slot 0).
    if (g_PT.ptFlags & kRabPTDecorrelation)
        RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.initialPathTracerPreservedBufferIndex);
}

// -rtxdi_pt_dual_mv=1: Zeng et al. 2021's dual motion vector, built from the
// current motion field. A pixel whose reprojected history is another surface
// (a disocclusion) takes the motion of its nearest occluder - the closest
// surface clearly in front of it within 32 px - assuming the background kept
// its place relative to the occluder; RTXDI then validates and searches
// around that history as for any motion vector. A static scene's histories
// all match: the base motion, bit for bit.
bool PTHistoryMatches(int2 prevPos, RAB_Surface s, float expectedDepth)
{
    if (any(prevPos < 0) || prevPos.x >= int(g_RabBridge.width) || prevPos.y >= int(g_RabBridge.height)) return false;
    const RAB_Surface p = RAB_GetGBufferSurface(prevPos, true);
    const RTXDI_PTTemporalResamplingParameters t = g_Const.restirPT.temporalResampling;
    return p.valid && RTXDI_IsValidNeighbor(s.normal, p.normal, expectedDepth, p.viewDepth, t.normalThreshold, t.depthThreshold) &&
           RAB_AreMaterialsSimilar(s.material, p.material);
}
float3 PTDualMotion(uint2 px, RAB_Surface s, float3 mv)
{
    const float expected = s.viewDepth + mv.z;
    if (PTHistoryMatches(int2(round(float2(px) + mv.xy)), s, expected)) return mv;
    // Reverse-Z: device depth ~ 1 / view depth, so "10% closer" is 1.1x deeper.
    static const int2 kDirs[8] = { int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1), int2(-1, 0), int2(-1, -1), int2(0, -1), int2(1, -1) };
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_RabBridge.depthIdx];
    float best = depthTex.Load(int3(px, 0)) * 1.1;
    int2 occluder = int2(-1, -1);
    [loop] for (uint ring = 0; ring < 4; ++ring)
    {
        [unroll] for (uint d = 0; d < 8; ++d)
        {
            const int2 q = int2(px) + kDirs[d] * int(4u << ring);
            if (any(q < 0) || q.x >= int(g_RabBridge.width) || q.y >= int(g_RabBridge.height)) continue;
            const float depth = depthTex.Load(int3(q, 0));
            if (depth > best) { best = depth; occluder = q; }
        }
    }
    if (occluder.x < 0) return mv;
    const RAB_Surface o = RAB_GetGBufferSurface(occluder, false);
    if (!o.valid) return mv;
    const float3 dual = float3(RAB_ScreenSpaceMotion(occluder, o).xy, mv.z);
    return PTHistoryMatches(int2(round(float2(px) + dual.xy)), s, expected) ? dual : mv;
}

// In place over the initial reservoir (RTXDI's temporal+spatial indices: the
// initial and the temporal output share a slot); a cell without a surface
// keeps the empty reservoir the initial pass stored.
[numthreads(8, 8, 1)]
void RtxdiPTTemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(px), false);
    if (!surface.valid) return;
    RTXDI_PTTemporalResamplingRuntimeParameters trr = RTXDI_EmptyPTTemporalResamplingRuntimeParameters();
    trr.pixelPosition = px;
    trr.reservoirPosition = px;
    trr.motionVector = RAB_ScreenSpaceMotion(int2(px), surface);
    if (g_PT.ptFlags & kRabPTDualMV) trr.motionVector = PTDualMotion(px, surface, trr.motionVector);
    trr.cameraPos = g_RabBridge.camera;
    trr.prevCameraPos = g_RabBridge.prevCamera;
    trr.prevPrevCameraPos = g_PT.prevPrevCamera;
    bool selectedPrevious = false;
    RAB_PathTracerUserData ptud = (RAB_PathTracerUserData)0;
    const RTXDI_PTReservoir r = RTXDI_PTTemporalResampling(g_Const.restirPT.temporalResampling, trr,
        g_Const.restirPT.hybridShift, g_Const.restirPT.reconnection, g_Runtime, g_Const.restirPT.reservoirBuffer,
        RTXDI_InitRandomSampler(px, g_PT.frameIndex, 3), g_Const.restirPT.bufferIndices, selectedPrevious, ptud);
    PTCount(2, selectedPrevious);
    RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.temporalResamplingOutputBufferIndex);
}

// Every thread reaches the group barrier inside the filter: a cell outside the
// grid weighs 0 and is not written.
[numthreads(RTXDI_BOILING_FILTER_GROUP_SIZE, RTXDI_BOILING_FILTER_GROUP_SIZE, 1)]
void RtxdiPTBoiling(uint3 tid : SV_DispatchThreadID, uint3 local : SV_GroupThreadID)
{
    const uint2 px = tid.xy;
    const bool inside = all(px < PTSize());
    RTXDI_PTReservoir r = RTXDI_EmptyPTReservoir();
    if (inside)
        r = RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.temporalResamplingOutputBufferIndex);
    const bool before = RTXDI_IsValidPTReservoir(r);
    RTXDI_PTBoilingFilter(local.xy, g_Const.restirPT.boilingFilter.boilingFilterStrength, r);
    if (!inside) return;
    if (before && !RTXDI_IsValidPTReservoir(r))
    {
        PTCount(4, true);
        RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.temporalResamplingOutputBufferIndex);
    }
}

[numthreads(8, 8, 1)]
void RtxdiPTSpatial(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(px), false);
    RTXDI_PTReservoir r = RTXDI_EmptyPTReservoir();
    if (surface.valid)
    {
        RTXDI_PTSpatialResamplingRuntimeParameters srr = RTXDI_EmptyPTSpatialResamplingRuntimeParameters();
        srr.pixelPosition = px;
        srr.reservoirPosition = px;
        srr.cameraPos = g_RabBridge.camera;
        srr.prevCameraPos = g_RabBridge.prevCamera;
        srr.prevPrevCameraPos = g_PT.prevPrevCamera;
        srr.viewportSize = PTSize();
        bool resampled = false;
        RAB_PathTracerUserData ptud = (RAB_PathTracerUserData)0;
        r = RTXDI_PTSpatialResampling(srr, g_Const.restirPT.spatialResampling, g_Const.restirPT.hybridShift,
            g_Const.restirPT.reconnection, g_Const.restirPT.reservoirBuffer, g_Const.restirPT.bufferIndices, g_Runtime,
            RTXDI_InitRandomSampler(px, g_PT.frameIndex, 4), resampled, ptud);
        PTCount(3, resampled);
    }
    RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.spatialResamplingOutputBufferIndex);
}

// The DLSS-RR guides of a GBuffer primary. PathTrace.cs.hlsl's formulas (its
// flags & 8192) over the same reads - RAB_LoadCurrentSurface is its GBuffer
// decode - so both cores hand RR the same diffuse albedo, specular albedo and
// normal + roughness. The specular hit distance is energy-weighted as the
// engine path tracer's: the primary's direct specular at its light (the sun at
// 10000, the engine's miss distance), the reservoir's path at its secondary
// vertex. ReSTIR DI's specular (kRabPTDiResult) carries its light's distance
// when the DI shell runs raw; otherwise a power-sampled light of the guide's
// own stands in (PTPrimaryDirect). The reservoir knows the secondary vertex
// when it is the reconnection vertex (rcVertexLength 2: a surface, the sky's
// fake vertex, or x1's next-event light); otherwise a ray along the mirror
// direction stands in, weighted by the split-sum specular share. No random
// number is drawn from the image's stream for a guide: the image is the same
// with RR's guides as without.
static const float kPTRRMissDistance = 10000;   // PathTrace.cs.hlsl's continuation TMax
#include "core/color.hlsli"   // Luminance
float PTRRLuminance(float3 v) { return Luminance(max(v, 0)); }
// PathTrace.cs.hlsl's RRSpecular / RTEvaluate: the specular share of the BSDF
// towards l, per channel (0 under -pt_diffuse).
float3 PTSpecularShare(RAB_Surface s, float3 l)
{
    if (g_PT.ptFlags & kRabPTDiffuse) return 0;
    float3 spec, kd;
    EvalCookTorrance(s.normal, s.viewDir, l, s.material.F0, s.material.roughness, s.material.metal, s.material.albedo, spec, kd);
    return saturate(spec * saturate(dot(s.normal, l)) / max(RAB_PTBsdfCos(s, l), 1e-30));
}
float3 PTSpecularAlbedo(RAB_Surface s)
{
    if (g_PT.ptFlags & kRabPTDiffuse) return 0;
    const float2 ab = EnvBRDFApprox(s.material.roughness, saturate(dot(s.normal, s.viewDir)));
    return max(s.material.F0 * ab.x + ab.y, 0);
}
// The first hit along the mirror direction, as the path tracer's rays find it
// (back faces culled, alpha-tested candidates; mask 0x08 like
// RAB_TraceContinuation - forward-translucent glass is not a path vertex).
float PTMirrorHitDistance(RAB_Surface s)
{
    const float3 l = reflect(-s.viewDir, s.normal);
    if (dot(l, s.normal) <= 0) return 0;
    RayDesc ray; ray.Origin = s.worldPos + s.normal * 0.001; ray.Direction = l; ray.TMin = 0.001; ray.TMax = kPTRRMissDistance;
    RayQuery<RAY_FLAG_CULL_BACK_FACING_TRIANGLES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x08, ray);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            (q.CandidateTriangleFrontFace() || (g_Instances[q.CandidateInstanceID()].rtFlags & RT_INST_FLAG_DOUBLE_SIDED)) &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), true))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : ray.TMax;
}
// The reservoir path's share of the hit distance: (specular energy, x distance).
float2 PTRRIndirectMoment(RAB_Surface s, RTXDI_PTReservoir r, float3 indirect)
{
    float distance;
    float3 share;
    if (r.rcVertexLength == 2 && r.pathLength >= 2)
    {
        float3 x2 = r.translatedWorldPosition;
        if (isinf(r.radiance.x))
        {
            // x1's next-event light (unified): the light data rides in the radiance.
            const RTXDI_SampledLightData data = RTXDI_GetSampledLightData(r);
            x2 = RAB_LightSamplePosition(RAB_SamplePolymorphicLight(
                RAB_LoadLightInfo(RTXDI_SampledLightData_GetLightIndex(data), false), s, RTXDI_SampledLightData_GetUVDataFloat2(data)));
        }
        const float3 d = x2 - s.worldPos;
        const float len = length(d);
        if (!(len > 0)) return 0;
        distance = min(len, kPTRRMissDistance);
        share = PTSpecularShare(s, d / len);
    }
    else
    {
        const float spec = PTRRLuminance(PTSpecularAlbedo(s));
        const float diffuse = PTRRLuminance(s.material.albedo * (1 - s.material.metal));
        if (!(spec > 0)) return 0;
        distance = PTMirrorHitDistance(s);
        share = (spec / (spec + diffuse)).xxx;
    }
    const float e = PTRRLuminance(indirect * share);
    return float2(e, e * distance);
}
void PTStoreRRGuides(uint2 px, RAB_Surface s, float hitDistance)
{
    RWTexture2D<float4> diffuseAlbedo = ResourceDescriptorHeap[g_PT.rrAlbedoUav];
    RWTexture2D<float4> specularAlbedo = ResourceDescriptorHeap[g_PT.rrSpecularUav];
    RWTexture2D<float4> normalRoughness = ResourceDescriptorHeap[g_PT.rrNormalUav];
    RWTexture2D<float> distance = ResourceDescriptorHeap[g_PT.rrDistanceUav];
    if (!s.valid)
    {
        // The sky and unlit primaries: PathTrace.cs.hlsl's cleared guides.
        diffuseAlbedo[px] = 0; specularAlbedo[px] = 0; normalRoughness[px] = float4(0, 0, 1, 1); distance[px] = 0;
        return;
    }
    // Engine split-sum convention, linear reflectance; no NRD demodulation;
    // reflectances in [0,1], as PathTrace.cs.hlsl: a base colour above 1 makes
    // DLSS-RR's model D write non-finite texels at 3x upscaling.
    diffuseAlbedo[px] = float4(saturate(s.material.albedo * (1 - s.material.metal)), 1);
    specularAlbedo[px] = float4(saturate(PTSpecularAlbedo(s)), 1);
    normalRoughness[px] = float4(s.normal, s.material.roughness);
    distance[px] = hitDistance;
    // -dlss_rr_corrupt_guide: one guide as per-pixel noise - the model must
    // answer with a different picture if it reads that guide.
    const uint corrupt = (g_PT.ptFlags >> kRabPTRRCorruptShift) & 7u;
    if (corrupt != 0)
    {
        uint h = (px.x * 73856093u) ^ (px.y * 19349663u) ^ (g_PT.frameIndex * 83492791u);
        h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
        const float4 n = float4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, h >> 24) / 255.0;
        if (corrupt == 1) diffuseAlbedo[px] = float4(n.rgb, 1);
        else if (corrupt == 2) specularAlbedo[px] = float4(n.rgb, 1);
        else if (corrupt == 3) normalRoughness[px] = float4(normalize(n.xyz * 2 - 1 + 1e-4), n.w);
        else if (corrupt == 4) distance[px] = n.x * 100;
    }
}

// The primary's direct light: the sun (the engine's cone estimate, MIS'd with
// the reservoir's BSDF hits of its disk) and the local lights - ReSTIR DI's
// result where its shell has one, else one next-event sample by power. Under
// RR it also adds the specular hit distance's (energy, moment) to rrSpec.
float3 PTPrimaryDirect(uint2 px, RAB_Surface s, inout RTXDI_RandomSamplerState rng, inout float2 rrSpec)
{
    float3 value = 0;
    const bool rr = (g_PT.ptFlags & kRabPTRRGuides) != 0;
    if (RAB_SunEnabled())
    {
        const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const RAB_LightSample ls = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(kRabSunLightIndex, false), s, uv);
        const float3 sun = RAB_NeeEstimate(s, kRabSunLightIndex, ls, 1.0);
        value += sun;
        if (rr && any(sun > 0))
        {
            const float e = PTRRLuminance(sun * PTSpecularShare(s, normalize(RAB_LightSamplePosition(ls) - s.worldPos)));
            rrSpec += float2(e, e * kPTRRMissDistance);
        }
    }
    bool fromDI = false;
    float4 diSpecular = 0;
    if (g_PT.ptFlags & kRabPTDiResult)
    {
        Texture2DArray<float4> di = ResourceDescriptorHeap[g_PT.diResultIdx];
        const float4 diffuse = di.Load(int4(px, 0, 0));
        if (diffuse.a != 0)
        {
            diSpecular = di.Load(int4(px, 1, 0));
            value += diffuse.rgb + diSpecular.rgb;
            fromDI = true;
        }
    }
    PTCount(7, fromDI);
    // On the path-trace path the DI shell runs raw (the renderer's diRaw) and
    // packs the distance to the light its reservoir chose beside the specular
    // (RTDI.cs.hlsl kDIFlags2RRDistance) - the light this pixel's DI specular
    // really comes from, as the raster RR path hands the model. Prefer it to the
    // random light by power below: on a spot-lit floor that distance jumps between
    // unrelated lamps every frame.
    if (fromDI && rr && diSpecular.a > 0)
    {
        const float e = PTRRLuminance(diSpecular.rgb);
        if (e > 0) rrSpec += float2(e, e * min(diSpecular.a, kPTRRMissDistance));
    }
    else if (fromDI && rr)
    {
        // No packed distance (a DI shell outside its raw mode): one light by
        // power from a stream of the guide's own (the image's stream is
        // untouched), unshadowed, stands in for the next-event light.
        RTXDI_RandomSamplerState guide = RTXDI_InitRandomSampler(px, g_PT.frameIndex, 6);
        uint index; float pdf;
        if (RAB_SelectLocalLight(RTXDI_GetNextRandom(guide), index, pdf))
        {
            const float2 uv = float2(RTXDI_GetNextRandom(guide), RTXDI_GetNextRandom(guide));
            const RAB_LightSample ls = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(index, false), s, uv);
            if (ls.solidAnglePdf > 0)
            {
                const float3 d = RAB_LightSamplePosition(ls) - s.worldPos;
                const float3 f = RAB_GetReflectedBsdfRadianceForSurface(RAB_LightSamplePosition(ls), RAB_LightSampleRadiance(ls), s) /
                                 (ls.solidAnglePdf * pdf);
                const float e = PTRRLuminance(f * PTSpecularShare(s, normalize(d)));
                rrSpec += float2(e, e * length(d));
            }
        }
    }
    if (!fromDI)
    {
        uint index; float pdf;
        if (RAB_SelectLocalLight(RTXDI_GetNextRandom(rng), index, pdf))
        {
            const float2 uv = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
            const RAB_LightSample ls = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(index, false), s, uv);
            const float3 local = RAB_NeeEstimate(s, index, ls, pdf);
            value += local;
            if (rr && any(local > 0))
            {
                const float3 d = RAB_LightSamplePosition(ls) - s.worldPos;
                const float e = PTRRLuminance(local * PTSpecularShare(s, normalize(d)));
                rrSpec += float2(e, e * length(d));
            }
        }
    }
    return value;
}

// RTXDI's DLSS-RR decorrelation (RTXDI_PTApplyDecorrelation, the same rules
// and random stream, inlined so the shade knows which estimate it shades).
// With probability p - the factor (Uniform, the default), or 4 x factor x
// stagnancy^exponent from the smoothed map (Stagnancy), 1 for a flagged
// firefly - the pixel shades the preserved, unresampled initial sample
// instead of the reused one: RR gets independent noise where the reuse would
// hand it the same sample for frames. With the bias reduction a fresh (age 0)
// reservoir caps the replacement at multiplyBound x its own weight. Only a p
// that does not look at the sample is unbiased: a reservoir is fresh exactly
// when this frame's initial sample won, a bright one.
// Returns false (and `initial` = r) when the recipe is off or keeps the sample.
bool PTDecorrelate(uint2 px, RTXDI_PTReservoir r, out RTXDI_PTReservoir initial)
{
    initial = r;
    const RTXDI_PTDecorrelationParameters d = g_Const.restirPT.decorrelation;
    const RTXDI_PTBufferIndices b = g_Const.restirPT.bufferIndices;
    if (!(g_PT.ptFlags & kRabPTDecorrelation) || d.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_NONE ||
        !(d.decorrelationFactor > 0) || b.temporalResamplingInputBufferIndex == b.temporalResamplingOutputBufferIndex)
        return false;
    float p = 0;
    if (d.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_UNIFORM)
        p = saturate(d.decorrelationFactor);
    else if (d.decorrelationMode == RTXDI_PT_DECORRELATION_MODE_STAGNANCY)
        p = saturate(4 * saturate(d.decorrelationFactor) *
                     pow(saturate(RTXDI_PT_SMOOTHED_DUPLICATION_MAP[px]), max(0.0, d.decorrelationStagnancyExponent)));
    if (d.fireflyReplacementFilterEnable != 0 && RTXDI_GetFireflyDetected(r)) p = 1;
    if (!(p > 0)) return false;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(px, g_PT.frameIndex, RTXDI_PT_FINAL_SHADING_RANDOM_SEED);
    if (!(RTXDI_GetNextRandom(rng) < p)) return false;
    initial = RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, px, b.initialPathTracerPreservedBufferIndex);
    if (d.fireflyReplacementBiasReduction != 0 && saturate(RTXDI_PT_DUPLICATION_MAP[px].y) == 0)
        initial.weightSum = min(d.fireflyReplacementMultiplyBound * r.weightSum, initial.weightSum);
    return true;
}

// The final reservoir's duplication inputs - its sample ID and stagnancy
// (age / 40) - for the count below and the stagnancy smoothing, both of which
// run before Shade (kRabPTDupMap). A sky, unlit or direct-only pixel has none.
[numthreads(8, 8, 1)]
void RtxdiPTDupInputs(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_RabBridge.depthIdx];
    RTXDI_PTReservoir r = RTXDI_EmptyPTReservoir();
    if (depthTex.Load(int3(px, 0)) > 0 && !(g_PT.ptFlags & kRabPTDirectOnly) && RAB_GetGBufferSurface(int2(px), false).valid)
        r = RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.finalShadingInputBufferIndex);
    RTXDI_PTStoreDuplicationInputs(px, r);
}

// The stagnancy's EMA over its reprojected 5x5 (RTXDI's
// RTXDI_PTComputeSmoothedDuplicationMap) - the motion is the temporal pass's.
[numthreads(8, 8, 1)]
void RtxdiPTDupSmooth(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    const RAB_Surface s = RAB_GetGBufferSurface(int2(px), false);
    int2 prevPixel = int2(px);
    bool prevValid = false;
    if (s.valid && g_PT.prevStagnancyUav != 0)
    {
        prevPixel = int2(round(float2(px) + RAB_ScreenSpaceMotion(int2(px), s).xy));
        prevValid = all(prevPixel >= 0) && all(prevPixel < int2(PTSize()));
    }
    RTXDI_PTComputeSmoothedDuplicationMap(px, PTSize(), g_Const.restirPT.decorrelation.decorrelationEmaFactor, prevPixel, prevValid);
}

// The final reservoir's firefly flag - RTXDI's boiling test at
// fireflyReplacementFilterStrength, a weight above (10 / s - 9) x its 16x16
// group's mean - which makes PTDecorrelate replace it. Every thread reaches
// the group barriers; a cell outside the grid weighs 0 and is not written.
[numthreads(RTXDI_BOILING_FILTER_GROUP_SIZE, RTXDI_BOILING_FILTER_GROUP_SIZE, 1)]
void RtxdiPTFirefly(uint3 tid : SV_DispatchThreadID, uint3 local : SV_GroupThreadID)
{
    const uint2 px = tid.xy;
    const bool inside = all(px < PTSize());
    RTXDI_PTReservoir r = RTXDI_EmptyPTReservoir();
    if (inside)
        r = RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.finalShadingInputBufferIndex);
    const bool wasFlagged = RTXDI_GetFireflyDetected(r);
    RTXDI_PTDetectDecorrelationFireflies(local.xy, r, g_Const.restirPT.decorrelation, g_Const.restirPT.bufferIndices);
    if (!inside) return;
    const bool flagged = RTXDI_GetFireflyDetected(r);
    PTCount(12, flagged);
    // Only a changed flag is written back: ~2% of the reservoirs are flagged,
    // and a full store would be most of the pass.
    if (flagged != wasFlagged)
        RTXDI_StorePTReservoir(r, g_Const.restirPT.reservoirBuffer, px, g_Const.restirPT.bufferIndices.finalShadingInputBufferIndex);
}

[numthreads(8, 8, 1)]
void RtxdiPTShade(uint3 tid : SV_DispatchThreadID)
{
    const uint2 px = tid.xy;
    if (any(px >= PTSize())) return;
    RWTexture2D<float4> result = ResourceDescriptorHeap[g_PT.resultUav];
    RWTexture2D<float4> accumulation = ResourceDescriptorHeap[g_PT.accumUav];
    Texture2D<float> depthTex = ResourceDescriptorHeap[g_RabBridge.depthIdx];
    const bool rr = (g_PT.ptFlags & kRabPTRRGuides) != 0;
    if (depthTex.Load(int3(px, 0)) <= 0)
    {
        // The sky: nothing, alpha 0 (PathTrace.cs.hlsl's direct store).
        result[px] = 0;
        accumulation[px] = 0;
        if (rr) PTStoreRRGuides(px, RAB_EmptySurface(), 0);
        return;
    }
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(px), false);
    if (!surface.valid)
    {
        // An unlit primary (RAB_PT's surface loader refuses it): its base
        // colour once, no accumulation (the engine path tracer's direct store).
        Texture2D<float4> albedoTex = ResourceDescriptorHeap[g_RabBridge.albedoIdx];
        const float3 albedo = albedoTex.Load(int3(px, 0)).rgb;
        result[px] = float4(albedo, 1);
        accumulation[px] = float4(albedo, 1);
        if (rr) PTStoreRRGuides(px, RAB_EmptySurface(), 0);
        return;
    }
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(px, g_PT.frameIndex, 5);
    float2 rrSpec = 0;   // the specular hit distance's (energy, energy x distance)
    // Unified (-rtxdi_pt_unified=1): the reservoir already holds x1's direct light.
    float3 value = (g_PT.ptFlags & kRabPTUnified) ? 0 : PTPrimaryDirect(px, surface, rng, rrSpec);
    bool reservoir = false;
    if (!(g_PT.ptFlags & kRabPTDirectOnly))
    {
        const RTXDI_PTReservoir r = RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, px,
                                                          g_Const.restirPT.bufferIndices.finalShadingInputBufferIndex);
        reservoir = RTXDI_IsValidPTReservoir(r) && r.weightSum > 0;
        RTXDI_PTReservoir initial = r;
        const bool decorrelated = reservoir && PTDecorrelate(px, r, initial);
        PTCount(11, decorrelated);
        float3 indirect = 0;
        if (decorrelated)
        {
            if (RTXDI_IsValidPTReservoir(initial) && initial.weightSum > 0) indirect = initial.targetFunction * initial.weightSum;
        }
        else if (g_PT.ptFlags & kRabPTRgb)
        {
            // -rtxdi_pt_rgb=1: the pairwise pass's RGB estimate over every candidate.
            RWTexture2D<float4> estimate = ResourceDescriptorHeap[g_PT.rgbUav];
            indirect = estimate[px].rgb;
        }
        else if (reservoir) indirect = r.targetFunction * r.weightSum;
        value += indirect;
        // initial == r unless decorrelated (PTDecorrelate's first line; ?: takes no struct).
        if (rr && reservoir && any(indirect > 0)) rrSpec += PTRRIndirectMoment(surface, initial, indirect);
    }
    PTCount(6, reservoir);
    const bool finite = all(isfinite(value));
    PTCount(5, !finite);
    if (!finite) value = 0;
    float3 mean = value;
    if ((g_PT.ptFlags & kRabPTAccumulate) && g_PT.spp != 0)
        mean = accumulation[px].rgb + (value - accumulation[px].rgb) / (float(g_PT.spp) + 1);
    accumulation[px] = float4(mean, 1);
    result[px] = float4(mean, 1);
    if (rr) PTStoreRRGuides(px, surface, rrSpec.x > 1e-20 ? rrSpec.y / rrSpec.x : 0);
}

// How many of each pixel's 17x17 neighbours share its final sample
// (RTXDI's 16x16 groups over a 32x32 tile in LDS); the next frame's temporal
// pass reduces the history cap where samples are duplicated.
[numthreads(RTXDI_PT_DUPMAP_GROUP_SIZE, RTXDI_PT_DUPMAP_GROUP_SIZE, 1)]
void RtxdiPTDupMapCount(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    RTXDI_PTComputeDuplicationMap(gid.xy, gtid.xy, PTSize());
}

// ==== Pairwise spatial pass ====
// The engine's pairwise spatial pass (-rtxdi_pt_spatial=pairwise) built from
// RTXDI's public shift (RTXDI_ComputeHybridShift). RTXDI's own spatial pass
// shifts each neighbour's sample here and this pixel's winner back into every
// neighbour (2K shifts per pixel); with reciprocal pairs and pairwise MIS
// (Bitterli / Lin's defensive pairwise, the engine's restir_pairwise.hlsli)
// each sample is shifted once per pair (K per pixel), and pixel and partner
// read each other's shift.
//   RtxdiPTPairShift     every pixel shifts its own sample into each partner's
//                        domain (scratch); -rtxdi_pt_compaction=1 appends the
//                        shifts that need a random replay to a list instead
//   RtxdiPTPairArgs      that list's dispatch arguments
//   RtxdiPTPairReplay    the listed shifts (ExecuteIndirect: replays only)
//   RtxdiPTPairResample  the canonical sample and each partner's, the next
//                        frame's reservoir, and under -rtxdi_pt_rgb=1 the RGB
//                        estimate sum_i m_i f(T y_i) |J_i| W_i for the shade
// Pairings: partner_k(p) = ((p + t_k) xor m_k) - t_k, an involution with fresh
// t, m every frame; the partner lies within 31 px on each axis.
#include "restir_pairwise.hlsli"

int2 PTPartner(int2 p, uint k)
{
    const uint4 q = g_PT.pairing[k];
    const int2 t = int2(q.xy);
    return int2(uint2(p + t) ^ q.zw) - t;
}
bool PTInView(int2 p) { return all(p >= 0) && p.x < int(g_RabBridge.width) && p.y < int(g_RabBridge.height); }
// A pair is used only if each surface accepts the other (RTXDI's neighbour
// test is relative to its first surface): both sides agree, as MIS requires.
bool PTPairValid(RAB_Surface a, RAB_Surface b)
{
    const RTXDI_PTSpatialResamplingParameters sp = g_Const.restirPT.spatialResampling;
    return a.valid && b.valid && IsValidNeighborSurface(sp, a, b) && IsValidNeighborSurface(sp, b, a);
}
// Scratch, 2 x uint4 per (pixel, pairing): this pixel's sample shifted into
// the partner's domain - target rgb, |J| - then the shifted partial Jacobian
// and the status (0 no pair, 1 shifted, 2 a pair but no sample to shift).
uint PTScratchIndex(int2 p, uint k) { return ((uint(p.y) * g_RabBridge.width + uint(p.x)) * g_PT.pairCount + k) * 2; }
void PTStoreShift(int2 p, uint k, float3 target, float jacobian, float partialJacobian, uint status)
{
    RWStructuredBuffer<uint4> scratch = ResourceDescriptorHeap[g_PT.pairScratchUav];
    const uint i = PTScratchIndex(p, k);
    scratch[i] = uint4(asuint(target), asuint(jacobian));
    scratch[i + 1] = uint4(asuint(partialJacobian), status, 0, 0);
}
RTXDI_PTReservoir PTLoadSpatialInput(int2 p)
{
    return RTXDI_LoadPTReservoir(g_Const.restirPT.reservoirBuffer, uint2(p), g_Const.restirPT.bufferIndices.spatialResamplingInputBufferIndex);
}
// RTXDI's spatial shift of a sample into the partner's domain: replay up to
// the reconnection vertex (or none), reconnect, and for a reconnection the BSDF
// at the vertex before it and the visibility (SpatialResampling.hlsli's
// ResampleNeighbors, one neighbour).
void PTShiftInto(int2 p, uint k, RAB_Surface partner, RTXDI_PTReservoir r)
{
    RTXDI_PTHybridShiftRuntimeParameters hs = (RTXDI_PTHybridShiftRuntimeParameters)0;
    hs.cameraPos = g_RabBridge.camera;
    hs.prevCameraPos = g_RabBridge.prevCamera;
    hs.prevPrevCameraPos = g_PT.prevPrevCamera;
    RAB_Surface surface = partner;
    const bool evaluate = ShouldEvaluateSpatialReconnection(r);
    float3 target; float jacobian;
    RAB_PathTracerUserData ptud = (RAB_PathTracerUserData)0;
    RAB_PathTracerUserDataSetPathType(ptud, RTXDI_PTPathTraceInvocationType_Spatial);
    RTXDI_ComputeHybridShift(surface, r, g_Const.restirPT.hybridShift, hs, g_Const.restirPT.reconnection, target, jacobian, ptud);
    if (evaluate) EvaluateReconnection(g_Const.restirPT.hybridShift, target, surface, r);
    if (!all(isfinite(target)) || !isfinite(jacobian)) { target = 0; jacobian = 0; }
    PTStoreShift(p, k, target, jacobian, r.partialJacobian, 1u);
}

[numthreads(8, 8, 1)]
void RtxdiPTPairShift(uint3 tid : SV_DispatchThreadID)
{
    const int2 p = int2(tid.xy);
    if (!PTInView(p)) return;
    const RAB_Surface s = RAB_GetGBufferSurface(p, false);
    const RTXDI_PTReservoir r = PTLoadSpatialInput(p);
    const bool sample = RTXDI_IsValidPTReservoir(r) && r.weightSum > 0;
    [loop] for (uint k = 0; k < g_PT.pairCount; ++k)
    {
        const int2 q = PTPartner(p, k);
        if (!PTInView(q)) { PTStoreShift(p, k, 0, 0, 0, 0u); continue; }
        const RAB_Surface partner = RAB_GetGBufferSurface(q, false);
        if (!PTPairValid(s, partner)) { PTStoreShift(p, k, 0, 0, 0, 0u); continue; }
        if (!sample) { PTStoreShift(p, k, 0, 0, 0, 2u); continue; }
        if ((g_PT.ptFlags & kRabPTCompaction) && NeedToRunRandomReplayPathTracer(r))
        {
            // -rtxdi_pt_compaction=1: a random replay waits for its own compacted pass.
            RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_PT.pairCounterUav];
            RWStructuredBuffer<uint> list = ResourceDescriptorHeap[g_PT.pairListUav];
            uint slot;
            InterlockedAdd(counter[0], 1u, slot);
            list[slot] = uint(p.x) | (uint(p.y) << 14) | (k << 28);
            continue;
        }
        PTShiftInto(p, k, partner, r);
    }
}

[numthreads(1, 1, 1)]
void RtxdiPTPairArgs(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_PT.pairCounterUav];
    RWStructuredBuffer<uint> args = ResourceDescriptorHeap[g_PT.pairArgsUav];
    const uint n = counter[0];
    args[0] = (n + 63) / 64; args[1] = 1; args[2] = 1;
    counter[1] = n;
    counter[0] = 0;
}

[numthreads(64, 1, 1)]
void RtxdiPTPairReplay(uint3 tid : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> counter = ResourceDescriptorHeap[g_PT.pairCounterUav];
    if (tid.x >= counter[1]) return;
    RWStructuredBuffer<uint> list = ResourceDescriptorHeap[g_PT.pairListUav];
    const uint e = list[tid.x];
    const int2 p = int2(e & 0x3fffu, (e >> 14) & 0x3fffu);
    const uint k = e >> 28;
    PTShiftInto(p, k, RAB_GetGBufferSurface(PTPartner(p, k), false), PTLoadSpatialInput(p));
}

[numthreads(8, 8, 1)]
void RtxdiPTPairResample(uint3 tid : SV_DispatchThreadID)
{
    const int2 p = int2(tid.xy);
    if (!PTInView(p)) return;
    // No surface load: a pixel without one has an empty canonical reservoir
    // and no pair (PairShift stored status 0 for every pairing), which yields
    // the empty reservoir and a zero estimate below.
    RTXDI_PTReservoir result = RTXDI_EmptyPTReservoir();
    float3 rgb = 0;
    bool resampled = false;
    {
        RWStructuredBuffer<uint4> scratch = ResourceDescriptorHeap[g_PT.pairScratchUav];
        const RTXDI_PTReservoir c = PTLoadSpatialInput(p);
        const bool canonical = RTXDI_IsValidPTReservoir(c);
        const float Mc = canonical ? c.M : 0;
        const float pcXc = canonical ? RTXDI_Luminance(c.targetFunction) : 0;
        const uint K = g_PT.pairCount;
        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(p), g_PT.frameIndex, 4);
        float weightSum = 0, mc = 0, M = Mc;
        float3 selectedTarget = 0;
        [loop] for (uint k = 0; k < K; ++k)
        {
            const int2 q = PTPartner(p, k);
            const uint i = PTScratchIndex(p, k);
            const uint4 own0 = scratch[i], own1 = scratch[i + 1];
            if (!PTInView(q) || own1.y == 0u) { mc += 1.0 / K; continue; }   // no partner: the canonical keeps the share
            const RTXDI_PTReservoir n = PTLoadSpatialInput(q);
            const bool neighbour = RTXDI_IsValidPTReservoir(n);
            const float Mi = neighbour ? n.M : 0;
            M += Mi;
            // The canonical's share of this pair: could the partner have produced x_c (through T_{p->q})?
            const float piTxc = own1.y == 1u ? RTXDI_Luminance(asfloat(own0.xyz)) : 0;
            mc += RTPairCanonical(Mc, pcXc, Mi, piTxc, asfloat(own0.w), K, true);
            const uint j = PTScratchIndex(q, k);
            const uint4 their0 = scratch[j], their1 = scratch[j + 1];
            if (!neighbour || their1.y != 1u || !(n.weightSum > 0)) continue;
            // The partner's sample shifted here (T_{q->p}, computed by the partner).
            const float3 f = asfloat(their0.xyz);
            const float J = asfloat(their0.w);
            const float pcTyi = RTXDI_Luminance(f);
            const float mi = RTPairNeighbour(Mi, RTXDI_Luminance(n.targetFunction), Mc, pcTyi, J, K, true);
            const float w = mi * pcTyi * J * n.weightSum;
            if (!(w > 0) || !isfinite(w)) continue;
            rgb += mi * f * J * n.weightSum;
            weightSum += w;
            if (RTXDI_GetNextRandom(rng) * weightSum < w)
            {
                result = n;
                result.partialJacobian = asfloat(their1.x);
                selectedTarget = f;
                resampled = true;
            }
        }
        if (canonical && c.weightSum > 0)
        {
            const float w = mc * pcXc * c.weightSum;
            if (w > 0 && isfinite(w))
            {
                rgb += mc * c.targetFunction * c.weightSum;
                weightSum += w;
                if (RTXDI_GetNextRandom(rng) * weightSum < w) { result = c; selectedTarget = c.targetFunction; resampled = false; }
            }
        }
        const float pHat = RTXDI_Luminance(selectedTarget);
        result.weightSum = pHat > 0 ? weightSum / pHat : 0;
        result.targetFunction = selectedTarget;
        result.M = min(M, float(RTXDI_PTReservoir::maxM));
        result.auxFlag = 0;   // RTXDI's combine never carries it either
    }
    PTCount(3, resampled);
    RTXDI_StorePTReservoir(result, g_Const.restirPT.reservoirBuffer, uint2(p), g_Const.restirPT.bufferIndices.spatialResamplingOutputBufferIndex);
    if (g_PT.ptFlags & kRabPTRgb)
    {
        RWTexture2D<float4> estimate = ResourceDescriptorHeap[g_PT.rgbUav];
        estimate[p] = float4(all(isfinite(rgb)) ? rgb : 0, 1);
    }
}

#if RAB_SHARC
// SHaRC: every buffer SHaRC reads starts at 0 (Integration.md) - on the first
// frame, a new world (PathTracePass::OnWorldClear) and a new capacity.
[numthreads(256, 1, 1)]
void RtxdiPTSharcClear(uint3 tid : SV_DispatchThreadID)
{
    SHARC_ENGINE_CLEAR(tid.x, g_PT.sharcHashUav, g_PT.sharcAccumulationUav, g_PT.sharcResolvedUav, g_PT.sharcCapacity);
}

// SHaRC: this frame's deposits into each entry's history; an entry no path
// refreshed for sharcStaleFrames frames is evicted.
[numthreads(256, 1, 1)]
void RtxdiPTSharcResolve(uint3 tid : SV_DispatchThreadID)
{
    SHARC_ENGINE_RESOLVE(tid.x, RAB_SharcParameters(), g_PT.sharcPrevCamera, g_PT.sharcAccumulationFrames,
                         g_PT.sharcStaleFrames, g_PT.sharcFrame);
}

#if SHARC_UPDATE
// SHaRC: one camera-rooted path per sharcUpdateTile^2 pixels, its pixel
// moving every frame, through the bridge's path tracer (RAB_TraceContinuation,
// RAB_SampleBrdfRay). Each vertex deposits its next-event light - the sun and
// one local light by power, as the path tracer's vertices take it - and its
// emission rides along (SHARC_SEPARATE_EMISSIVE: carried back, never stored);
// SHaRC propagates both to the vertices before. A miss deposits the sky (the
// sun's disk MIS'd against its next-event estimate). The path ends where SHaRC
// resamples the cache (past its propagation depth) or after
// sharcUpdateBounces bounces. Its own random stream (7): no other kernel's.
[numthreads(8, 8, 1)]
void RtxdiPTSharcUpdate(uint3 tid : SV_DispatchThreadID)
{
    const uint tile = max(g_PT.sharcUpdateTile, 1u);
    const uint2 tiles = (PTSize() + tile - 1) / tile;
    if (any(tid.xy >= tiles)) return;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(tid.xy, g_PT.sharcFrame, 7);
    const uint pick = min(uint(RTXDI_GetNextRandom(rng) * float(tile * tile)), tile * tile - 1);
    const uint2 px = tid.xy * tile + uint2(pick % tile, pick / tile);
    if (any(px >= PTSize())) return;
    const float2 uv = (float2(px) + float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng))) / float2(PTSize());
    RayDesc ray;
    ray.Origin = g_RabBridge.camera;
    ray.Direction = normalize(RAB_ViewToWorldPosition(uv, 0.5) - g_RabBridge.camera);
    ray.TMin = 0;
    ray.TMax = 10000;
    const SharcParameters params = RAB_SharcParameters();
    SharcState state;
    SharcInit(state);
    float bsdfPdf = 0;
    [loop] for (uint bounce = 0; bounce <= g_PT.sharcUpdateBounces; ++bounce)
    {
        RAB_RayPayload payload;
        RAB_Surface hit;
        float3 emission;
        // Glass is no vertex of the cache's paths: they pass straight through it (untinted, a
        // limitation of the cache alone; the light at each vertex is still weighed through glass).
        const uint outcome = RAB_TraceContinuation(ray, payload, hit, emission, false);
#if RAB_MEDIA
        // The fog between this hit and the vertex before, by the continuation rule of the paths that
        // read the cache (RAB_PathTrace; absorption under the froxel rule, kRabSegmentCache); the camera
        // ray reaches no vertex's light.
        if (bounce != 0 && outcome != kRabTraceUnloadable)
            SharcSetThroughput(state, RAB_FogTransmittance(ray.Origin, ray.Direction,
                                                           outcome == kRabTraceMiss ? kMediumFar : payload.committedHitT,
                                                           kRabSegmentCache));
#endif
        if (outcome == kRabTraceMiss || outcome == kRabTraceUntrusted)
        {
            // The camera ray's sky is no vertex's light.
            if (bounce != 0)
                SharcUpdateMiss(params, state, outcome == kRabTraceMiss ? RAB_EnvironmentRadiance(ray.Direction, bsdfPdf)
                                                                        : RAB_SkyRadiance(ray.Direction));
            break;
        }
        if (outcome == kRabTraceUnloadable) break;
        SharcHitData h;
        h.positionWorld = hit.worldPos;
        h.normalWorld = hit.geoNormal;
        h.emissive = all(isfinite(emission)) ? emission : 0;
        PTCountSharcSlot(9, true);
        if (outcome == kRabTraceUnlit)
        {
            // It emits its base colour and does not scatter.
            SharcUpdateHit(params, state, h, 0, RTXDI_GetNextRandom(rng));
            break;
        }
        float3 direct = 0;
        if (RAB_SunEnabled())
        {
            const float2 su = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
            const RAB_LightSample ls = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(kRabSunLightIndex, false), hit, su);
            direct += RAB_NeeEstimate(hit, kRabSunLightIndex, ls, 1.0);
        }
        uint index; float pdf;
        if (RAB_SelectLocalLight(RTXDI_GetNextRandom(rng), index, pdf))
        {
            const float2 lu = float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
            const RAB_LightSample ls = RAB_SamplePolymorphicLight(RAB_LoadLightInfo(index, false), hit, lu);
            direct += RAB_NeeEstimate(hit, index, ls, pdf);
        }
        if (!all(isfinite(direct))) direct = 0;
        if (!SharcUpdateHit(params, state, h, direct, RTXDI_GetNextRandom(rng))) break;
        if (bounce == g_PT.sharcUpdateBounces) break;
        bool ok;
        const float3 u = float3(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
        const RTXDI_BrdfRaySample brs = RAB_SampleBrdfRay(hit, u, ok);
        if (!ok) break;
        SharcSetThroughput(state, brs.brdfTimesNoL / brs.outPdf);
        bsdfPdf = brs.outPdf;
        ray.Origin = hit.worldPos + (dot(brs.outDirection, hit.normal) >= 0 ? hit.normal : -hit.normal) * 0.001;
        ray.Direction = brs.outDirection;
        ray.TMin = 0.001;
        ray.TMax = 10000;
    }
}
#endif
#endif
