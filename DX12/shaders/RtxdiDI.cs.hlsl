// RtxdiDI.cs.hlsl - the RTXDI ReSTIR DI core.
// TODO(ReSTIR_Dual_Backend_Plan S8): optional until 2026-11-15, CLI-only until 2026-12-13,
// then this core is deleted.
//
// One TU with the RTDI shell: it includes RTDI.cs.hlsl, so the shell's
// constants (b0: the compaction list, counters, NRD targets, flags), its
// receivers (eligibility, the lifted geometric origin), the engine integrand
// and target (EvaluateLight + LightTarget), its visibility rays
// (TraceVisibility, counted in the Visibility slot) and its NRD packing are
// exactly the engine core's. The vendored RTXDI-Library (never modified) does
// the sampling and the resampling through the bridge in its DI mode
// (RAB_DI_ENGINE): RTXDI's surfaces are the shell's receivers on the trace
// grid (FullPixel), its light points use the engine's seed convention, so a
// sample converts between the two reservoir formats exactly (the hybrid
// compositions below).
//
// Every per-pixel entry runs over the shell's compacted list (ExecuteIndirect
// with Prepare's arguments), 64 lanes per group, like the engine core's.
//   RtxdiPresampleLocal  - RTXDI's power-RIS tiles from the engine's PDF
//                          texture (LightSamplingPass), 128 x 1024
//   RtxdiPresampleReGIR  - ReGIR's onion cells from those tiles
//   RtxdiInitial         - RTXDI_SampleLightsForSurface (+ initial visibility)
//   RtxdiTemporal        - RTXDI_DITemporalResampling
//   RtxdiBoiling         - RTXDI's boiling filter on the temporal output: it
//                          averages a 16x16 thread group, so it runs over the
//                          whole trace grid, not the compacted list
//   RtxdiSpatial         - RTXDI_DISpatialResampling
//   RtxdiShade           - the selected sample through the engine integrand and
//                          one visibility ray, NRD-packed into DiffIn / SpecIn
//   RtxdiToEngine        - (hybrid: RTXDI candidates x engine reuse) the RTXDI
//                          initial reservoir into the engine core's Initial slot
//   EngineToRtxdi        - (engine candidates x RTXDI reuse) the other way
#include "RTDI.cs.hlsl"

#define RTXDI_REGIR_MODE RTXDI_REGIR_ONION
#include "RtxdiBridgeConstants.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/LightSampling/RISBufferSegmentParameters.h"
#include "Rtxdi/ReGIR/ReGIRParameters.h"

// The RTXDI core's own constants, b1 of the shell's root signature (mirror:
// RTDI::RtxdiConstants, RTDIRtxdiCore.h).
cbuffer RtxdiParams : register(b1)
{
    RAB_BridgeConstants g_RabBridge;
    RTXDI_RuntimeParameters g_Runtime;
    RTXDI_LightBufferParameters g_LightBuffer;
    RTXDI_RISBufferSegmentParameters g_LocalRIS;
    RTXDI_RISBufferSegmentParameters g_EnvironmentRIS;
    RTXDI_ReservoirBufferParameters g_Reservoirs;
    RTXDI_DIBufferIndices g_DIBuffers;
    RTXDI_DIInitialSamplingParameters g_Initial;
    RTXDI_DITemporalResamplingParameters g_Temporal;
    RTXDI_BoilingFilterParameters g_Boiling;
    RTXDI_DISpatialResamplingParameters g_Spatial;
    RTXDI_ShadingParameters g_Shading;
    ReGIR_Parameters g_ReGIR;
    uint g_ReservoirUav, g_RisUav, g_NeighborOffsetsSrv, g_RtxdiFlags;
    uint g_RegirLightSlots, g_LocalTiles, g_RtxdiPad0, g_RtxdiPad1;
};
// g_RtxdiFlags
static const uint kRtxdiFlagTemporal = 1u << 0;
static const uint kRtxdiFlagSpatial = 1u << 1;
static const uint kRtxdiFlagInitialVisibility = 1u << 2;
static const uint kRtxdiFlagBoilingFilter = 1u << 3;

// The library's resources, bindless (RtxdiProbe.cs.hlsl explains the pattern).
RWBuffer<uint2> RtxdiRisBuffer() { return ResourceDescriptorHeap[g_RisUav]; }
Buffer<float2> RtxdiNeighborOffsets() { return ResourceDescriptorHeap[g_NeighborOffsetsSrv]; }
RWStructuredBuffer<RTXDI_PackedDIReservoir> RtxdiDIReservoirs() { return ResourceDescriptorHeap[g_ReservoirUav]; }
#define RTXDI_RIS_BUFFER RtxdiRisBuffer()
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
#define RTXDI_LIGHT_RESERVOIR_BUFFER RtxdiDIReservoirs()

#define RAB_DI_ENGINE 1
// Which bias corrections the DI resampling functions compile. Define it here:
// the library's default sits in DI/SpatialResampling.hlsli only, and
// DI/TemporalResampling.hlsli (included first) tests it undefined, i.e. 0 =
// off, so temporal reuse would fall back to the biased 1/M weights whatever
// -rtxdi_di_bias says (~-3.5% energy from the temporal pass alone).
#define RTXDI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_RAY_TRACED
#include "RtxdiBridge.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#define RTXDI_ENABLE_BOILING_FILTER 1
#define RTXDI_BOILING_FILTER_GROUP_SIZE 16
#include "Rtxdi/DI/BoilingFilter.hlsli"

// The RTXDI random streams: per trace pixel, per frame, one per entry.
RTXDI_RandomSamplerState RtxdiRng(uint2 tp, uint pass) { return RTXDI_InitRandomSampler(tp, frame, pass); }

// The light sample a stored reservoir stands for, at this surface.
RAB_LightSample RtxdiSampleOf(RTXDI_DIReservoir r, RAB_Surface surface)
{
    if (!RTXDI_IsValidDIReservoir(r)) return RAB_EmptyLightSample();
    return RAB_SamplePolymorphicLight(RAB_LoadLightInfo(RTXDI_GetDIReservoirLightIndex(r), false), surface,
                                      RTXDI_GetDIReservoirSampleUV(r));
}

[numthreads(256, 1, 1)]
void RtxdiPresampleLocal(uint3 tid : SV_DispatchThreadID)
{
    const uint tile = tid.x / g_LocalRIS.tileSize, sampleInTile = tid.x % g_LocalRIS.tileSize;
    if (tile >= g_LocalTiles) return;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(sampleInTile, tile), frame, 5);
    Texture2D pdf = ResourceDescriptorHeap[g_RabBridge.lightPdfIdx];   // RTXDI_TEX2D: Texture2D (float4)
    RTXDI_PresampleLocalLights(rng, pdf, uint2(g_RabBridge.lightPdfSize, g_RabBridge.lightPdfSize), tile, sampleInTile,
                               g_LightBuffer.localLightBufferRegion, g_LocalRIS);
}

[numthreads(256, 1, 1)]
void RtxdiPresampleReGIR(uint3 tid : SV_DispatchThreadID)
{
    const uint slot = tid.x;
    if (slot >= g_RegirLightSlots) return;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(uint2(slot & 0xfffu, slot >> 12), frame, 6);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(uint2(slot / g_ReGIR.commonParams.lightsPerCell, 0), frame, 7);
    RTXDI_PresampleLocalLightsForReGIR(rng, coherentRng, slot, g_LightBuffer.localLightBufferRegion, g_LocalRIS, g_ReGIR);
}

[numthreads(64, 1, 1)]
void RtxdiInitial(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
    // Next frame's previous surface (the engine keeps no previous GBuffer).
    RAB_StoreSurface(int2(tp), surface);
    RTXDI_RandomSamplerState rng = RtxdiRng(tp, 1);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(tp / RTXDI_TILE_SIZE_IN_PIXELS, frame, 1);
    RAB_LightSample lightSample;
    g_RabOccluded = false;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(rng, coherentRng, surface, g_Initial, g_LightBuffer,
        g_LocalRIS, g_EnvironmentRIS, g_ReGIR, lightSample);
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[counterUav];
        const uint streamed = WaveActiveSum(g_Initial.numLocalLightSamples);
        if (WaveIsFirstLane()) InterlockedAdd(counters[4], streamed);   // Candidates
    }
    // RTXDI_SampleLightsForSurface traced the initial visibility itself
    // (g_Initial.enableInitialVisibility) and discarded an occluded selection:
    // the reservoir lost its light (M kept). Its one ray is the only one in
    // this pass, so a refused ray is exactly a discarded selection (the
    // returned sample is undefined when nothing was selected).
    if (g_Initial.enableInitialVisibility && !RTXDI_IsValidDIReservoir(reservoir) && g_RabOccluded)
        Count(7);   // Occluded
    RTXDI_StoreDIReservoir(reservoir, g_Reservoirs, tp, g_DIBuffers.initialSamplingOutputBufferIndex);
}

[numthreads(64, 1, 1)]
void RtxdiTemporal(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
    RTXDI_DIReservoir current = RTXDI_LoadDIReservoir(g_Reservoirs, tp, g_DIBuffers.initialSamplingOutputBufferIndex);
    RAB_LightSample lightSample = RtxdiSampleOf(current, surface);
    RTXDI_DIReservoir result = current;
    if (historyReset == 0)
    {
        RTXDI_RandomSamplerState rng = RtxdiRng(tp, 2);
        int2 temporalPixel;
        result = RTXDI_DITemporalResampling(tp, surface, current, rng, g_Runtime, g_Reservoirs,
            RAB_ScreenSpaceMotion(int2(tp), surface), g_DIBuffers.temporalResamplingInputBufferIndex, g_Temporal,
            temporalPixel, lightSample);
        if (result.M > current.M) Count(5);   // ReusedTemporal: a history merged
    }
    RTXDI_StoreDIReservoir(result, g_Reservoirs, tp, g_DIBuffers.temporalResamplingOutputBufferIndex);
}

// Every thread reaches the group barrier inside RTXDI_BoilingFilter: a cell
// outside the grid or with no eligible receiver weighs 0 and is not written.
[numthreads(RTXDI_BOILING_FILTER_GROUP_SIZE, RTXDI_BOILING_FILTER_GROUP_SIZE, 1)]
void RtxdiBoiling(uint3 tid : SV_DispatchThreadID, uint3 local : SV_GroupThreadID)
{
    const uint2 tp = tid.xy;
    const bool inside = all(tp < uint2(traceW, traceH));
    const bool valid = inside && RAB_LoadCurrentSurface(int2(tp)).valid;
    RTXDI_DIReservoir reservoir = RTXDI_EmptyDIReservoir();
    if (valid) reservoir = RTXDI_LoadDIReservoir(g_Reservoirs, tp, g_DIBuffers.temporalResamplingOutputBufferIndex);
    const bool before = RTXDI_IsValidDIReservoir(reservoir);
    RTXDI_BoilingFilter(local.xy, g_Boiling.boilingFilterStrength, reservoir);
    if (!valid) return;
    if (before && !RTXDI_IsValidDIReservoir(reservoir))
    {
        Count(26);   // Boiled
        RTXDI_StoreDIReservoir(reservoir, g_Reservoirs, tp, g_DIBuffers.temporalResamplingOutputBufferIndex);
    }
}

[numthreads(64, 1, 1)]
void RtxdiSpatial(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    Count(14);   // SpatialStage0
    const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
    RTXDI_DIReservoir center = RTXDI_LoadDIReservoir(g_Reservoirs, tp, g_DIBuffers.spatialResamplingInputBufferIndex);
    RAB_LightSample lightSample = RtxdiSampleOf(center, surface);
    RTXDI_RandomSamplerState rng = RtxdiRng(tp, 3);
    const float before = center.M;
    RTXDI_DIReservoir result = RTXDI_DISpatialResampling(tp, surface, center, rng, g_Runtime, g_Reservoirs,
        g_DIBuffers.spatialResamplingInputBufferIndex, g_Spatial, lightSample);
    if (result.M > before) Count(6);   // ReusedSpatial
    RTXDI_StoreDIReservoir(result, g_Reservoirs, tp, g_DIBuffers.spatialResamplingOutputBufferIndex);
}

[numthreads(64, 1, 1)]
void RtxdiShade(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    RTXDI_DIReservoir sample = RTXDI_LoadDIReservoir(g_Reservoirs, tp, g_DIBuffers.shadingInputBufferIndex);
    if (!RTXDI_IsValidDIReservoir(sample)) return;
    const uint lightIndex = RTXDI_GetDIReservoirLightIndex(sample);
    if (lightIndex >= lightCount) return;
    Count(2);   // Selected
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[counterUav];
        const uint represented = WaveActiveSum(uint(sample.M));
        if (WaveIsFirstLane()) InterlockedAdd(counters[9], represented);   // MSum
    }
    // The engine core's Shade, on RTXDI's selection: the integrand at the
    // light's centre, one visibility ray to the sample's point, NRD packing.
    Receiver r = LoadTracingReceiver(FullPixel(tp));
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[lightsIdx];
    float3 d, s, l; float distance;
    EvaluateLight(r, lights[lightIndex], d, s, l, distance);
    const float visibility = SampleVisibility(r, lights[lightIndex], sample.uvData, l, distance, 1);
    if (!(visibility > 0)) Count(7);   // Occluded
    const float weight = RTXDI_GetDIReservoirInvPdf(sample) * visibility;
    float3 df, sf;
    DIMaterialFactors(r.N, r.V, r.albedo, r.F0, r.rough, df, sf);
    d = d * weight / df; s = s * weight / sf;
    if (!all(isfinite(d)) || !all(isfinite(s))) { Count(3); d = s = 0; }
    RWTexture2D<float4> diffuse = ResourceDescriptorHeap[diffUav];
    RWTexture2D<float4> specular = ResourceDescriptorHeap[specUav];
    diffuse[tp] = DIPackSignal(d, distance, r.viewZ, r.rough);
    specular[tp] = DIPackSignal(s, distance, r.viewZ, r.rough);
}

// ==== The compositions (a DI core is {candidates} x {reuse}) ====
// The light point converts exactly: the engine's seed is RTXDI's packed uv
// (ApplyLightPoint reads the seed's halves; RAB_SamplePolymorphicLight scales
// RTXDI's uv onto the same 1/65536 grid). The contribution weight W (the
// estimate of 1/pdf) carries over; the target is the receiving side's
// (the same integrand and scalarization in both cores).
[numthreads(64, 1, 1)]
void RtxdiToEngine(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    const RTXDI_DIReservoir source = RTXDI_LoadDIReservoir(g_Reservoirs, tp, g_DIBuffers.initialSamplingOutputBufferIndex);
    Receiver r = LoadReceiver(FullPixel(tp));
    DIReservoir e; DIReset(e);
    // One proposal was drawn whatever came of it: M is 1 for an empty or
    // discarded result too, exactly as the engine core's Initial writes it
    // (an M of 0 would hand the history the canonical sample's MIS share).
    e.M = 1;
    const float W = RTXDI_GetDIReservoirInvPdf(source);
    if (RTXDI_IsValidDIReservoir(source))
    {
        e.lightIndex = RTXDI_GetDIReservoirLightIndex(source);
        e.pathSeed = source.uvData;
        if (W > 0 && e.lightIndex < lightCount)
        {
            StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[lightsIdx];
            float3 d, s, l; float distance;
            EvaluateLight(r, lights[e.lightIndex], d, s, l, distance);
            e.target = LightTarget(d, s);
            e.valid = e.target > 0;
            e.weightSum = e.valid ? W * e.target : 0;
        }
        // RTXDI's initial visibility discards an occluded selection (W 0):
        // the engine's Initial records that as tested and not visible.
        if (g_RtxdiFlags & kRtxdiFlagInitialVisibility) { e.visibilityTested = true; e.visible = e.valid; }
    }
    RWStructuredBuffer<DIPacked> initial = ResourceDescriptorHeap[initialUav];
    initial[pixel] = DIPack(e, r.N);
}

[numthreads(64, 1, 1)]
void EngineToRtxdi(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    uint pixel; uint2 tp;
    if (!CompactPixel(CompactLane(tid, gid), pixel, tp)) return;
    // Read through the UAV: the engine core leaves its Initial slot in UAV state.
    RWStructuredBuffer<DIPacked> initial = ResourceDescriptorHeap[initialUav];
    float3 ignored;
    const DIReservoir e = DIUnpack(initial[pixel], ignored);
    RTXDI_DIReservoir r = RTXDI_EmptyDIReservoir();
    const float W = DIWeight(e);
    if (e.valid && W > 0 && e.lightIndex < lightCount)
    {
        const RAB_Surface surface = RAB_LoadCurrentSurface(int2(tp));
        r.lightData = e.lightIndex | RTXDI_DIReservoir_LightValidBit;
        r.uvData = e.pathSeed;
        r.targetPdf = RAB_GetLightSampleTargetPdfForSurface(RtxdiSampleOf(r, surface), surface);
        r.weightSum = r.targetPdf > 0 ? W : 0;
        if (!(r.targetPdf > 0)) r = RTXDI_EmptyDIReservoir();
    }
    // One proposal was drawn whatever came of it (RTXDI's own initial sampling
    // leaves M 1 on an empty or discarded result too): an M of 0 would hand
    // the history the canonical sample's MIS share.
    r.M = 1;
    // The receiver's surface for next frame's temporal (RtxdiInitial stores
    // it when RTXDI samples the candidates).
    RAB_StoreSurface(int2(tp), RAB_LoadCurrentSurface(int2(tp)));
    RTXDI_StoreDIReservoir(r, g_Reservoirs, tp, g_DIBuffers.initialSamplingOutputBufferIndex);
}
