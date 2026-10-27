// RtxdiGI.cs.hlsl - the RTXDI ReSTIR GI core.
// TODO(ReSTIR_Dual_Backend_Plan S8): optional until 2026-11-15, CLI-only until 2026-12-13,
// then this core is deleted.
//
// One TU with the RTGI shell: it includes RTGI.cs.hlsl, so the shell's
// constants (b0: the reservoir / receiver buffers, counters, flags), its
// receiver records (GISurface), the GI target and integrand (GITarget,
// GIIntegrand), its visibility rays (VisibleFrom), the history re-trace and
// re-shade (ValidateHistory) and the coverage history are exactly the engine
// core's. The vendored RTXDI-Library (never modified) does the temporal and
// spatial resampling through the bridge in its GI mode (RAB_GI_ENGINE):
// RTXDI's surfaces are the shell's receivers on the trace grid.
//
//   RtxdiGIPrepare  - the history validation pre-pass (RTXDI loads the
//                     previous reservoir inside RTXDI_GITemporalResampling, so
//                     the engine core's re-trace / re-shade cannot run on the
//                     tap it picks: it runs before, on the history buffer in
//                     place, from the receiver each history belongs to), the
//                     coverage history (the engine core's Temporal does that
//                     part) and Trace's 40-byte reservoir into RTXDI's initial
//                     slot
//   RtxdiGITemporal - RTXDI_GITemporalResampling
//   RtxdiGIBoiling  - RTXDI's GI boiling filter (opt-in, -gi_boiling_filter)
//   RtxdiGISpatial  - RTXDI_GISpatialResampling
// The stage that ends the chain also writes the final reservoir in the shell's
// 40-byte format (RtxdiGIToEngine when the boiling filter ends it), which the
// engine's Shade (RTGI.cs.hlsl) shades; with the unshadowed target, through
// one final visibility ray.
#include "RTGI.cs.hlsl"

#include "RtxdiBridgeConstants.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"

// The RTXDI core's own constants, b1 of the shell's root signature (mirror:
// RtxdiGIConstants, RTGIRtxdiCore.cpp; BuildPipelines checks the layout).
cbuffer RtxdiParams : register(b1)
{
    RAB_BridgeConstants g_RabBridge;
    RTXDI_RuntimeParameters g_Runtime;
    RTXDI_ReservoirBufferParameters g_Reservoirs;
    RTXDI_GIBufferIndices g_GIBuffers;
    RTXDI_GITemporalResamplingParameters g_Temporal;
    RTXDI_BoilingFilterParameters g_Boiling;
    RTXDI_GISpatialResamplingParameters g_Spatial;
    uint g_ReservoirUav, g_NeighborOffsetsSrv, g_RtxdiGIFlags, g_RtxdiGIPad;
};
// g_RtxdiGIFlags
static const uint kRtxdiGIFlagTemporal = 1u << 0;
static const uint kRtxdiGIFlagSpatial = 1u << 1;
// RTXDI's own setup: an unshadowed target, visibility only in the bias
// correction (ray-traced mode) and in the final shading (ToEngine).
static const uint kRtxdiGIFlagUnshadowed = 1u << 2;
static const uint kRtxdiGIFlagBoiling = 1u << 3;
static const uint kRtxdiGIFlagValidate = 1u << 4;
// The temporal pass ends the chain (no spatial reuse, no boiling filter): it
// writes the final reservoir.
static const uint kRtxdiGIFlagFinalTemporal = 1u << 5;

// The library's resources, bindless (RtxdiProbe.cs.hlsl explains the pattern).
RWStructuredBuffer<RTXDI_PackedGIReservoir> RtxdiGIReservoirs() { return ResourceDescriptorHeap[g_ReservoirUav]; }
Buffer<float2> RtxdiNeighborOffsets() { return ResourceDescriptorHeap[g_NeighborOffsetsSrv]; }
#define RTXDI_GI_RESERVOIR_BUFFER RtxdiGIReservoirs()
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
// Every bias-correction mode compiles in; the constants pick one.
#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED
#include "Rtxdi/GI/Reservoir.hlsli"

// Which reservoir array holds, at a surface's cell, that surface's own
// sample (set by each entry; ~0 = none): the bridge's GI target skips the
// visibility ray for it.
static uint g_GIOwnCurrent = 0xffffffffu, g_GIOwnPrevious = 0xffffffffu;
// How far in front of its receiver a genuine zero's point sits (Prepare).
static const float kGIZeroDistance = 1000.0;
bool RAB_GIOwnSample(int2 cell, bool previousFrame, out float3 position)
{
    position = float3(0, 0, 0);
    const uint array = previousFrame ? g_GIOwnPrevious : g_GIOwnCurrent;
    if (array == 0xffffffffu) return false;
    const RTXDI_GIReservoir r = RTXDI_LoadGIReservoir(g_Reservoirs, uint2(cell), array);
    position = r.position;
    // A weight means its target was > 0 at its own receiver: visible there.
    return r.M > 0 && r.weightSum > 0;
}

#define RAB_GI_ENGINE 1
#define RAB_GI_VISIBLE_TARGET ((g_RtxdiGIFlags & kRtxdiGIFlagUnshadowed) == 0u)
#include "RtxdiBridge.hlsli"
#include "Rtxdi/GI/TemporalResampling.hlsli"
#include "Rtxdi/GI/SpatialResampling.hlsli"
#define RTXDI_ENABLE_BOILING_FILTER 1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 16
#include "Rtxdi/GI/BoilingFilter.hlsli"

// The RTXDI random streams: per trace cell, per frame (with the engine's
// -seed mixed in, so two seeds give independent runs; -seed=1 keeps the
// frame index), one per entry.
RTXDI_RandomSamplerState RtxdiGIRng(uint2 tp, uint pass)
{
    return RTXDI_InitRandomSampler(tp, frame + (seed - 1u) * 0x9E3779B9u, pass);
}

// The final reservoir in the shell's 40-byte format, which the engine's Shade
// (RTGI.cs.hlsl) shades: the stage that produces it writes it (Spatial, or
// Temporal without spatial reuse, or ToEngine after a boiling filter that ends
// the chain). With the unshadowed target the selection is shaded through one
// final visibility ray (RTXDI's final visibility); the engine target holds it.
void RtxdiGIWriteFinal(uint2 tp, RTXDI_GIReservoir r, RAB_Surface surface)
{
    GIReservoir e = (GIReservoir)0;
    e.metadata = min(r.M, 63u) | (min(r.age, 63u) << 6);
    if (surface.valid && RTXDI_IsValidGIReservoir(r) && r.weightSum > 0)
    {
        e.P = r.position;
        e.normal = GIPackNormal(r.normal);
        e.radiance = r.radiance;
        e.W = r.weightSum;
        e.target = GITarget(surface.worldPos, surface.normal, e, surface.material.albedo);
        if (!(e.target > 0) || (!(RAB_GI_VISIBLE_TARGET) && !RAB_TraceVisibility(surface, e.P))) e.W = 0;
    }
    if (!isfinite(e.W)) { e.W = 0; Count(5); }
    RWStructuredBuffer<GIReservoir> output = ResourceDescriptorHeap[initialUav];
    output[tp.y * traceW + tp.x] = e;
}

#if RTGI_SHARC
// SHaRC: the shell's cached bit (GIReservoir.metadata, kGIMetaCached) has no
// slot in RTXDI's reservoir - the library keeps no misc data of its own
// (RTXDI_StoreGIReservoir packs 0) - but RTXDI copies a sample's position
// verbatim through every resampling (RTXDI_CombineGIReservoirs), so the bit
// rides as the lowest mantissa bit of position.x (one ulp: ~1e-6 m at 10 m
// from the origin). Prepare writes it, the validation reads it and writes its
// outcome back: the RTXDI core re-shades a sample the way it was shaded,
// exactly as the engine core does.
float3 GISetCachedBit(float3 p, bool cached)
{
    p.x = asfloat((asuint(p.x) & ~1u) | (cached ? 1u : 0u));
    return p;
}
bool GICachedBit(float3 p) { return (asuint(p.x) & 1u) != 0u; }
#endif

// The history validation, as the engine core's ValidateHistory does it, on
// one history in place, from the receiver it was resampled for (the previous
// GISurface at its cell):
//   valid     it is still the closest hit: re-shaded (M 1 on a large
//             luminance change);
//   occluded  something now sits in front of it: a genuine zero, so its
//             weight goes and its M stays (its population keeps its share of
//             the MIS weights);
//   vanished  nothing there any more: the reservoir is emptied (M 0), and
//             RTXDI's tap search moves on.
void RtxdiGIValidate(uint2 tp)
{
    const uint array = g_GIBuffers.temporalResamplingInputBufferIndex;
    RTXDI_GIReservoir history = RTXDI_LoadGIReservoir(g_Reservoirs, tp, array);
    if (!RTXDI_IsValidGIReservoir(history) || !(history.weightSum > 0)) return;
    StructuredBuffer<GISurface> surfaces = ResourceDescriptorHeap[previousSurfaceSrv];
    const RAB_Surface previous = RAB_SurfaceFromGI(surfaces[tp.y * traceW + tp.x], g_RabBridge.prevWorldToView, g_RabBridge.prevCamera);
    if (!previous.valid) return;
    GIReservoir sample = (GIReservoir)0;
    sample.P = history.position;
    sample.normal = GIPackNormal(history.normal);
    sample.radiance = history.radiance;
    sample.W = history.weightSum;
    sample.target = 1;     // it has a sample (its weight is > 0)
    sample.metadata = 2u;  // a sentinel M: ValidateHistory writes 1 on a luminance reset
#if RTGI_SHARC
    if (GICachedBit(history.position)) sample.metadata |= kGIMetaCached;
#endif
    uint rng = HashLowbias32((tp.y * traceW + tp.x) ^ HashLowbias32(frame) ^ 0x2c1b3c6du) | 1u;
    const uint state = ValidateHistory(RAB_SurfaceToGIReceiver(previous), sample, rng);
    if (state == kGIHistoryVanished) { history = RTXDI_EmptyGIReservoir(); Count(9); }
    else if (state == kGIHistoryOccluded) history.weightSum = 0;
    else
    {
        history.radiance = sample.radiance;
        if (GIM(sample) == 1u) history.M = 1;
#if RTGI_SHARC
        history.position = GISetCachedBit(history.position, (sample.metadata & kGIMetaCached) != 0u);
#endif
    }
    RTXDI_StoreGIReservoir(history, g_Reservoirs, tp, array);
}

// Before RTXDI's temporal pass, per cell: the history validation (RTXDI loads
// the previous reservoir inside RTXDI_GITemporalResampling, so the engine
// core's re-trace / re-shade cannot run on the tap it picks - it runs here, on
// the history buffer in place), the coverage history, and Trace's reservoir
// into RTXDI's initial slot.
[numthreads(8, 8, 1)]
void RtxdiGIPrepare(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tp = tid.xy;
    if (any(tp >= uint2(traceW, traceH))) return;
    if (g_RtxdiGIFlags & kRtxdiGIFlagValidate) RtxdiGIValidate(tp);
    const uint index = tp.y * traceW + tp.x;
    RWStructuredBuffer<GIReservoir> initial = ResourceDescriptorHeap[initialUav];
    RWStructuredBuffer<GISurface> surfaces = ResourceDescriptorHeap[surfaceUav];
    const GIReservoir e = initial[index];
    GISurface surface = surfaces[index];
    const bool eligible = GISurfaceCount(surface) > 0;
    Receiver r = (Receiver)0;
    r.P = surface.P; r.N = GIUnpackNormal(surface.normal); r.Ng = GIUnpackNormal(surface.geoNormal);
    r.albedo = GIUnpackAlbedo(surface.albedo); r.material = surface.material; r.eligible = eligible;
    r.viewZ = abs(GIViewDepth(surface.P));
    if (eligible && !historyReset)
    {
        // The coverage history, as the engine core's Temporal keeps it: the
        // reprojected tap first, then a few nearby ones, the first compatible
        // receiver's coverage averaged in (it estimates geometry, independently
        // of which sample the reservoirs select).
        uint rng = HashLowbias32(index ^ HashLowbias32(frame) ^ 0x6a09e667u) | 1u;
        Texture2D<float2> velocity = ResourceDescriptorHeap[velocityIdx];
        const float2 motion = velocity.Load(int3(FullPixel(tp), 0)) * float2(.5, -.5);
        const float stride = (flags & 4u) ? 1 : 2;
        const int2 reprojected = int2(round(float2(
            RTPreviousTraceCoordinate(tp.x, -motion.x, width, stride, historyJitterDelta.x),
            RTPreviousTraceCoordinate(tp.y, -motion.y, height, stride, historyJitterDelta.y))));
        StructuredBuffer<GISurface> oldSurfaces = ResourceDescriptorHeap[previousSurfaceSrv];
        bool found = false; GISurface oldSurface = (GISurface)0;
        [loop] for (uint attempt = 0; attempt < 5 && !found; ++attempt)
        {
            int2 q = reprojected;
            if (attempt > 0) q += int2(round((float2(RTRandom(rng), RTRandom(rng)) * 2 - 1) * (stride > 1 ? 1.5 : 2.5)));
            if (any(q < 0) || any(q >= int2(traceW, traceH))) continue;
            oldSurface = oldSurfaces[q.y * traceW + q.x];
            found = Compatible(r, oldSurface);
        }
        if (found)
        {
            const uint n = min(GISurfaceCount(oldSurface), 31u);
            GISetCoverageCount(surface, (GISurfaceCoverage(surface) + GISurfaceCoverage(oldSurface) * n) / (n + 1), n + 1);
        }
        else Count(18);
    }
    surfaces[index] = surface;
    // Trace's reservoir: W is its RIS contribution weight (the estimate of
    // 1/pdf), which RTXDI_MakeGIReservoir takes as pdf = 1/W, M 1. A genuine
    // zero (no connection: the rays missed or left the range) is still one
    // proposal, M 1 and W 0 - but RTXDI shifts a reservoir's point to every
    // receiver that reuses it, and an all-zero point gives a NaN Jacobian,
    // which RTXDI's validation rejects: the zeros' population would be
    // dropped and sparse GI biased bright (see the engine core's Temporal).
    // So a zero gets a point far in front of its receiver, facing it (a
    // Jacobian of ~1 to every receiver near it), with no radiance and no
    // weight.
    RTXDI_GIReservoir fresh = RTXDI_EmptyGIReservoir();
    if (eligible)
    {
        if (e.W > 0 && isfinite(e.W)) fresh = RTXDI_MakeGIReservoir(e.P, GIUnpackNormal(e.normal), e.radiance, 1.0 / e.W);
        else fresh = RTXDI_MakeGIReservoir(r.P + r.N * kGIZeroDistance, -r.N, float3(0, 0, 0), 0);
#if RTGI_SHARC
        if (e.W > 0 && isfinite(e.W)) fresh.position = GISetCachedBit(fresh.position, (e.metadata & kGIMetaCached) != 0u);
#endif
    }
    RTXDI_StoreGIReservoir(fresh, g_Reservoirs, tp, g_GIBuffers.secondarySurfaceReSTIRDIOutputBufferIndex);
}

[numthreads(8, 8, 1)]
void RtxdiGITemporal(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tp = tid.xy;
    if (any(tp >= uint2(traceW, traceH))) return;
    const uint source = g_GIBuffers.secondarySurfaceReSTIRDIOutputBufferIndex;
    g_GIOwnCurrent = source;                                          // Trace's own hit
    g_GIOwnPrevious = g_GIBuffers.temporalResamplingInputBufferIndex;  // validated histories
    const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
    const RTXDI_GIReservoir input = RTXDI_LoadGIReservoir(g_Reservoirs, tp, source);
    RTXDI_GIReservoir result = input;
    if (surface.valid && !historyReset && (g_RtxdiGIFlags & kRtxdiGIFlagTemporal))
    {
        RTXDI_RandomSamplerState rng = RtxdiGIRng(tp, 1);
        result = RTXDI_GITemporalResampling(tp, surface, RAB_ScreenSpaceMotion(int2(tp), surface),
            g_GIBuffers.temporalResamplingInputBufferIndex, input, rng, g_Runtime, g_Reservoirs, g_Temporal);
        if (result.M > input.M) Count(6);   // ReusedTemporal: a history merged
    }
    if (!isfinite(result.weightSum)) { result.weightSum = 0; Count(5); }
    RTXDI_StoreGIReservoir(result, g_Reservoirs, tp, g_GIBuffers.temporalResamplingOutputBufferIndex);
    if (g_RtxdiGIFlags & kRtxdiGIFlagFinalTemporal) RtxdiGIWriteFinal(tp, result, surface);
}

// Every thread reaches the group barrier inside RTXDI_GIBoilingFilter: a cell
// outside the grid weighs 0 and is not written.
[numthreads(RTXDI_BOILING_FILTER_GROUP_SIZE, RTXDI_BOILING_FILTER_GROUP_SIZE, 1)]
void RtxdiGIBoiling(uint3 tid : SV_DispatchThreadID, uint3 local : SV_GroupThreadID)
{
    const uint2 tp = tid.xy;
    const bool inside = all(tp < uint2(traceW, traceH));
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    if (inside) reservoir = RTXDI_LoadGIReservoir(g_Reservoirs, tp, g_GIBuffers.temporalResamplingOutputBufferIndex);
    const bool before = reservoir.weightSum > 0;
    RTXDI_GIBoilingFilter(local.xy, g_Boiling.boilingFilterStrength, reservoir);
    if (inside && before && !RTXDI_IsValidGIReservoir(reservoir))
    {
        Count(13);   // Boiled
        RTXDI_StoreGIReservoir(reservoir, g_Reservoirs, tp, g_GIBuffers.temporalResamplingOutputBufferIndex);
    }
}

[numthreads(8, 8, 1)]
void RtxdiGISpatial(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tp = tid.xy;
    if (any(tp >= uint2(traceW, traceH))) return;
    const uint source = g_GIBuffers.spatialResamplingInputBufferIndex;
    g_GIOwnCurrent = source;   // each receiver's temporal result: visible from it when weighted
    const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
    const RTXDI_GIReservoir input = RTXDI_LoadGIReservoir(g_Reservoirs, tp, source);
    RTXDI_GIReservoir result = input;
    if (surface.valid)
    {
        RTXDI_RandomSamplerState rng = RtxdiGIRng(tp, 2);
        result = RTXDI_GISpatialResampling(tp, surface, source, input, rng, g_Runtime, g_Reservoirs, g_Spatial);
        if (result.M > input.M) Count(7);   // ReusedSpatial: a neighbour merged
    }
    if (!isfinite(result.weightSum)) { result.weightSum = 0; Count(5); }
    RTXDI_StoreGIReservoir(result, g_Reservoirs, tp, g_GIBuffers.spatialResamplingOutputBufferIndex);
    RtxdiGIWriteFinal(tp, result, surface);
}

// Temporal alone with the boiling filter after it: the final reservoir is the
// filtered one.
[numthreads(8, 8, 1)]
void RtxdiGIToEngine(uint3 tid : SV_DispatchThreadID)
{
    const uint2 tp = tid.xy;
    if (any(tp >= uint2(traceW, traceH))) return;
    g_GIOwnCurrent = 0xffffffffu;
    RtxdiGIWriteFinal(tp, RTXDI_LoadGIReservoir(g_Reservoirs, tp, g_GIBuffers.finalShadingInputBufferIndex),
                      RAB_LoadCurrentSurface(int2(tp)));
}
