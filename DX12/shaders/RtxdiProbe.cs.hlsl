// RtxdiProbe.cs.hlsl - the RTXDI integration probe: RTXDI's ReSTIR DI and GI
// entry points compiled through the engine's bridge (RtxdiBridge.hlsli) with
// the library's resources reached bindlessly. Built by RtxdiProbePass under
// -rtxdi_probe (and by the CI shader gate when shaders/Rtxdi exists): a
// pipeline that compiles and creates is the evidence the bridge covers what
// the library calls. Never dispatched for a picture; the DI / GI cores
// (RtxdiDI / RtxdiGI) follow its resource pattern.
#include "RtxdiBridgeConstants.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/DI/ReSTIRDIParameters.h"
#include "Rtxdi/GI/ReSTIRGIParameters.h"
#include "Rtxdi/LightSampling/RISBufferSegmentParameters.h"

cbuffer Params : register(b0)
{
    RAB_BridgeConstants g_RabBridge;
    RTXDI_RuntimeParameters g_Runtime;
    RTXDI_LightBufferParameters g_LightBuffer;
    RTXDI_RISBufferSegmentParameters g_LocalRIS;
    RTXDI_RISBufferSegmentParameters g_EnvironmentRIS;
    RTXDI_ReservoirBufferParameters g_Reservoirs;
    RTXDI_DIInitialSamplingParameters g_DIInitial;
    RTXDI_DITemporalResamplingParameters g_DITemporal;
    RTXDI_DISpatialResamplingParameters g_DISpatial;
    RTXDI_GITemporalResamplingParameters g_GITemporal;
    RTXDI_GISpatialResamplingParameters g_GISpatial;
    uint g_DIReservoirUav, g_GIReservoirUav, g_RisUav, g_NeighborOffsetsSrv;
    uint g_FrameIndex, g_ProbePad0, g_ProbePad1, g_ProbePad2;
};

// The library's resources, reached bindlessly: its macros expand to calls
// returning a descriptor-heap resource (no register bindings, so a core can
// share the RTDI shell's root signature). The packed reservoir types come from
// the parameter headers above, and every macro must exist before the first
// library header that tests it.
RWBuffer<uint2> RtxdiRisBuffer() { return ResourceDescriptorHeap[g_RisUav]; }
Buffer<float2> RtxdiNeighborOffsets() { return ResourceDescriptorHeap[g_NeighborOffsetsSrv]; }
RWStructuredBuffer<RTXDI_PackedDIReservoir> RtxdiDIReservoirs() { return ResourceDescriptorHeap[g_DIReservoirUav]; }
RWStructuredBuffer<RTXDI_PackedGIReservoir> RtxdiGIReservoirs() { return ResourceDescriptorHeap[g_GIReservoirUav]; }
#define RTXDI_RIS_BUFFER RtxdiRisBuffer()
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
#define RTXDI_LIGHT_RESERVOIR_BUFFER RtxdiDIReservoirs()
#define RTXDI_GI_RESERVOIR_BUFFER RtxdiGIReservoirs()

#include "RtxdiBridge.hlsli"
#include "Rtxdi/DI/Reservoir.hlsli"
#include "Rtxdi/GI/Reservoir.hlsli"
#include "Rtxdi/LightSampling/PresamplingFunctions.hlsli"
#include "Rtxdi/DI/InitialSampling.hlsli"
#include "Rtxdi/DI/TemporalResampling.hlsli"
#include "Rtxdi/DI/SpatialResampling.hlsli"
#include "Rtxdi/DI/ReservoirStorage.hlsli"
#include "Rtxdi/GI/TemporalResampling.hlsli"
#include "Rtxdi/GI/SpatialResampling.hlsli"

[numthreads(8, 8, 1)]
void ProbeDI(uint3 id : SV_DispatchThreadID)
{
    const int2 pixel = int2(id.xy);
    RAB_Surface surface = RAB_GetGBufferSurface(pixel, false);
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_FrameIndex, 1);
    RTXDI_RandomSamplerState coherentRng = RTXDI_InitRandomSampler(id.xy / RTXDI_TILE_SIZE_IN_PIXELS, g_FrameIndex, 2);
    RAB_LightSample lightSample;
    RTXDI_DIReservoir reservoir = RTXDI_SampleLightsForSurface(rng, coherentRng, surface, g_DIInitial, g_LightBuffer,
#if RTXDI_ENABLE_PRESAMPLING
        g_LocalRIS, g_EnvironmentRIS,
#if RTXDI_REGIR_MODE != RTXDI_REGIR_DISABLED
        (ReGIR_Parameters)0,
#endif
#endif
        lightSample);
    int2 temporalPixel;
    reservoir = RTXDI_DITemporalResampling(id.xy, surface, reservoir, rng, g_Runtime, g_Reservoirs,
        RAB_ScreenSpaceMotion(pixel, surface), 0, g_DITemporal, temporalPixel, lightSample);
    reservoir = RTXDI_DISpatialResampling(id.xy, surface, reservoir, rng, g_Runtime, g_Reservoirs, 1, g_DISpatial, lightSample);
    RTXDI_StoreDIReservoir(reservoir, g_Reservoirs, id.xy, 0);
    RAB_StoreSurface(pixel, surface);
}

[numthreads(8, 8, 1)]
void ProbeGI(uint3 id : SV_DispatchThreadID)
{
    const int2 pixel = int2(id.xy);
    RAB_Surface surface = RAB_GetGBufferSurface(pixel, false);
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_FrameIndex, 3);
    RTXDI_GIReservoir reservoir = RTXDI_EmptyGIReservoir();
    reservoir = RTXDI_GITemporalResampling(id.xy, surface, RAB_ScreenSpaceMotion(pixel, surface), 0, reservoir, rng,
        g_Runtime, g_Reservoirs, g_GITemporal);
    reservoir = RTXDI_GISpatialResampling(id.xy, surface, 1, reservoir, rng, g_Runtime, g_Reservoirs, g_GISpatial);
    RTXDI_StoreGIReservoir(reservoir, g_Reservoirs, id.xy, 0);
}
