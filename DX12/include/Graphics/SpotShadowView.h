#pragma once
#include <DirectXMath.h>
#include <cstdint>

// StructuredBuffer ABI shared with spot_shadow_view.hlsli; vp is transposed on upload. A logical
// light keeps its index when promoted between physical atlas tiers.
struct SpotShadowViewGPU
{
    DirectX::XMFLOAT4X4 vp{};
    uint32_t atlasIndex = ~0u;
    uint32_t slice = 0;
    uint32_t resolution = 512;
    uint32_t padding = 0;
};
static_assert(sizeof(SpotShadowViewGPU) == 80);
