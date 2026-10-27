#pragma once
// RtxdiBridge.h - the C++ side of the engine's RTXDI Application Bridge: the
// constants every RTXDI core TU embeds as g_RabBridge. Mirror of
// shaders/RtxdiBridgeConstants.hlsli, member for member; RtxdiProbe checks the
// struct's offset and size in each probe TU against the shader's reflection.
#include <DirectXMath.h>
#include <cstdint>

namespace RTXDI
{
struct BridgeConstants
{
    DirectX::XMFLOAT4X4 invViewProj;       // current, row-vector convention (mul(p, M))
    DirectX::XMFLOAT4X4 worldToView;
    DirectX::XMFLOAT4X4 prevWorldToView;   // the previous frame's view (motion depth)
    DirectX::XMFLOAT3 camera; uint32_t width;       // render resolution
    DirectX::XMFLOAT3 prevCamera; uint32_t height;
    uint32_t depthIdx = 0, normalIdx = 0, albedoIdx = 0, surfaceIdx = 0;   // GBuffer SRVs (bindless)
    uint32_t velocityIdx = 0, materialsIdx = 0, lightsIdx = 0, lightCount = 0;
    // The previous light list, the key map (0 = identity), the local-light PDF
    // texture and its shape (LightSamplingPass).
    uint32_t prevLightsIdx = 0, prevLightCount = 0, lightMapIdx = 0, lightPdfIdx = 0;
    uint32_t lightPdfSize = 0, lightPdfMips = 0, compactLightsUav = 0, compactLightsSrv = 0;
    // The bridge's surface ping-pong: this frame's (UAV), the previous
    // frame's (SRV), 32 bytes per pixel of this grid.
    uint32_t surfaceOutUav = 0, prevSurfaceSrv = 0, surfaceGridWidth = 0, surfaceGridHeight = 0;
    float rayTMax = 0.f; uint32_t alphaTestEnabled = 0, alphaInstanceCount = 0, bridgeFlags = 0;
};
static_assert(sizeof(BridgeConstants) == 320, "mirror of RAB_BridgeConstants (RtxdiBridgeConstants.hlsli)");
} // namespace RTXDI
