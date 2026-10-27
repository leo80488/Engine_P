// RtxdiBridgeConstants.hlsli - the engine views the RTXDI Application Bridge
// reads. An RTXDI core TU embeds one of these as `g_RabBridge` in its cbuffer
// before including RtxdiBridge.hlsli, so the struct lives in its own header.
// Mirror: RTXDI::BridgeConstants (C++).
#ifndef RTXDI_BRIDGE_CONSTANTS_HLSLI
#define RTXDI_BRIDGE_CONSTANTS_HLSLI

struct RAB_BridgeConstants
{
    float4x4 invViewProj;        // current, row-vector convention (mul(p, M))
    float4x4 worldToView;
    float4x4 prevWorldToView;    // the previous frame's view (motion depth)
    float3 camera; uint width;   // render resolution
    float3 prevCamera; uint height;
    uint depthIdx, normalIdx, albedoIdx, surfaceIdx;          // GBuffer SRVs (bindless)
    uint velocityIdx, materialsIdx, lightsIdx, lightCount;
    // Lights: the previous list, the key map (0 = identity), the local-light PDF
    // texture and its shape (LightSamplingPass).
    uint prevLightsIdx, prevLightCount, lightMapIdx, lightPdfIdx;
    uint lightPdfSize, lightPdfMips, compactLightsUav, compactLightsSrv;
    // The bridge's own surface ping-pong: this frame's (UAV), the previous
    // frame's (SRV), 32 bytes per pixel of the surface grid below.
    uint surfaceOutUav, prevSurfaceSrv, surfaceGridWidth, surfaceGridHeight;
    float rayTMax; uint alphaTestEnabled, alphaInstanceCount, bridgeFlags;
};

#endif
