// RtxdiProbePT.cs.hlsl - the RTXDI integration probe for ReSTIR PT: RTXDI's
// PT initial sampling, temporal and spatial resampling compiled through the
// engine's bridge. HLSL 2021 with 16-bit types (the PT headers use templates
// and uint16_t), so it is compiled with ShaderDiskCache::CompileFlags{true,
// true} and only on a device that reports native 16-bit shader ops. Built by
// RtxdiProbePass under -rtxdi_probe; never dispatched for a picture (the
// RtxdiPT core is the dispatched one).
#include "RtxdiBridgeConstants.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/PT/ReSTIRPTParameters.h"

// RTXDI's PT SpatialResampling.hlsli reads the PT parameters from a global
// named g_Const.restirPT (under RTXDI_SPATIAL_NEIGHBOR_SELECTION_BUFFER, in
// the vendored version), so the PT parameters live under that name.
struct RtxdiPTConstants
{
    RTXDI_PTParameters restirPT;
};

cbuffer Params : register(b0)
{
    RAB_BridgeConstants g_RabBridge;
    RTXDI_RuntimeParameters g_Runtime;
    RtxdiPTConstants g_Const;
    uint g_PTReservoirUav, g_NeighborOffsetsSrv, g_NeighborSelectionUav, g_SampleIdUav;
    uint g_DuplicationUav, g_SmoothedDuplicationUav, g_PrevSmoothedDuplicationSrv, g_FrameIndex;
};

// The library's PT resources, reached bindlessly (RtxdiProbe.cs.hlsl explains
// the pattern).
RWStructuredBuffer<RTXDI_PackedPTReservoir> RtxdiPTReservoirs() { return ResourceDescriptorHeap[g_PTReservoirUav]; }
Buffer<float2> RtxdiNeighborOffsets() { return ResourceDescriptorHeap[g_NeighborOffsetsSrv]; }
RWByteAddressBuffer RtxdiNeighborSelection() { return ResourceDescriptorHeap[g_NeighborSelectionUav]; }
RWTexture2D<uint> RtxdiSampleIds() { return ResourceDescriptorHeap[g_SampleIdUav]; }
RWTexture2D<float2> RtxdiDuplicationMap() { return ResourceDescriptorHeap[g_DuplicationUav]; }
RWTexture2D<float> RtxdiSmoothedDuplicationMap() { return ResourceDescriptorHeap[g_SmoothedDuplicationUav]; }
Texture2D<float> RtxdiPrevSmoothedDuplicationMap() { return ResourceDescriptorHeap[g_PrevSmoothedDuplicationSrv]; }
#define RTXDI_PT_RESERVOIR_BUFFER RtxdiPTReservoirs()
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER RtxdiNeighborOffsets()
#define RTXDI_SPATIAL_NEIGHBOR_SELECTION_BUFFER RtxdiNeighborSelection()
#define RTXDI_PT_SAMPLE_ID_TEXTURE RtxdiSampleIds()
#define RTXDI_PT_DUPLICATION_MAP RtxdiDuplicationMap()
#define RTXDI_PT_SMOOTHED_DUPLICATION_MAP RtxdiSmoothedDuplicationMap()
#define RTXDI_PT_PREV_SMOOTHED_DUPLICATION_MAP RtxdiPrevSmoothedDuplicationMap()

// Bridge, then the PT context, then the path trace hook (the library's PT
// headers call it from non-template code, so it must precede them).
#include "RtxdiBridgePT.hlsli"
#include "Rtxdi/PT/PathTracerContext.hlsli"
#define RTXDI_BRIDGE_PT_TRACE
#include "RtxdiBridgePT.hlsli"
#include "Rtxdi/PT/InitialSampling.hlsli"
#include "Rtxdi/PT/TemporalResampling.hlsli"
#include "Rtxdi/PT/SpatialResampling.hlsli"
#include "Rtxdi/PT/SpatialNeighborSelection.hlsli"
#include "Rtxdi/PT/DuplicationMap.hlsli"
#include "Rtxdi/PT/Decorrelation.hlsli"

[numthreads(8, 8, 1)]
void ProbePT(uint3 id : SV_DispatchThreadID)
{
    const RAB_Surface surface = RAB_GetGBufferSurface(int2(id.xy), false);
    RTXDI_PathTracerRandomContext ptRand = RTXDI_InitializePathTracerRandomContext(id.xy, g_FrameIndex, 1, 2);
    RAB_PathTracerUserData ptud = (RAB_PathTracerUserData)0;

    RTXDI_PTInitialSamplingRuntimeParameters isr = RTXDI_EmptyPTInitialSamplingRuntimeParameters();
    isr.cameraPos = g_RabBridge.camera;
    isr.prevCameraPos = g_RabBridge.prevCamera;
    isr.prevPrevCameraPos = g_RabBridge.prevCamera;
    RTXDI_PTReservoir reservoir = GenerateInitialSamples(g_Const.restirPT.initialSampling, isr, g_Const.restirPT.reconnection, ptRand, surface, ptud);
    RTXDI_StorePTReservoir(reservoir, g_Const.restirPT.reservoirBuffer, id.xy, g_Const.restirPT.bufferIndices.initialPathTracerOutputBufferIndex);

    RTXDI_PTTemporalResamplingRuntimeParameters trr = RTXDI_EmptyPTTemporalResamplingRuntimeParameters();
    trr.pixelPosition = id.xy;
    trr.reservoirPosition = id.xy;
    trr.motionVector = RAB_ScreenSpaceMotion(int2(id.xy), surface);
    trr.cameraPos = g_RabBridge.camera;
    trr.prevCameraPos = g_RabBridge.prevCamera;
    trr.prevPrevCameraPos = g_RabBridge.prevCamera;
    bool selectedPrev = false;
    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(id.xy, g_FrameIndex, 3);
    reservoir = RTXDI_PTTemporalResampling(g_Const.restirPT.temporalResampling, trr, g_Const.restirPT.hybridShift, g_Const.restirPT.reconnection, g_Runtime,
        g_Const.restirPT.reservoirBuffer, rng, g_Const.restirPT.bufferIndices, selectedPrev, ptud);

    RTXDI_PTSpatialResamplingRuntimeParameters srr = RTXDI_EmptyPTSpatialResamplingRuntimeParameters();
    srr.pixelPosition = id.xy;
    srr.reservoirPosition = id.xy;
    srr.cameraPos = g_RabBridge.camera;
    srr.prevCameraPos = g_RabBridge.prevCamera;
    srr.prevPrevCameraPos = g_RabBridge.prevCamera;
    srr.viewportSize = uint2(g_RabBridge.width, g_RabBridge.height);
    bool resampled = false;
    reservoir = RTXDI_PTSpatialResampling(srr, g_Const.restirPT.spatialResampling, g_Const.restirPT.hybridShift, g_Const.restirPT.reconnection,
        g_Const.restirPT.reservoirBuffer, g_Const.restirPT.bufferIndices, g_Runtime, rng, resampled, ptud);
    RTXDI_StorePTReservoir(reservoir, g_Const.restirPT.reservoirBuffer, id.xy, g_Const.restirPT.bufferIndices.spatialResamplingOutputBufferIndex);
}
