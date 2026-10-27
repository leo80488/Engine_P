#pragma once

// The sampler a pass that reads MATERIAL textures binds at s0: anisotropic wrap (trilinear at 1x)
// with a TAA-sharpening LOD bias that follows the upscale ratio. One description, so GBuffer,
// Transparent, the translucent front layer, water and terrain cannot drift apart, and one bias rule,
// so an upscaled frame samples the mips its DISPLAY resolution needs.

#include "Graphics/GraphicsStruct.h"

#include <cmath>
#include <cstdint>

namespace MaterialSampler
{
    // A pass's bias at 1:1 - the sharper mip TAA (or the upscaler) integrates
    // across frames; going below about -1.5 aliases faster than it can.
    inline constexpr float kNativeBias        = -1.0f;
    inline constexpr float kTerrainNativeBias = -0.5f;   // terrain's milder one

    // An upscaler rebuilds display-resolution detail from render-resolution
    // samples, so the sampler must pick the mip the display would:
    // log2(render / display) below the native bias - the FSR and DLSS
    // integration guides' rule, log2(render / display) - 1 with the native -1.
    // At or above 1:1 the native bias stands.
    inline float Bias(float nativeBias, uint32_t renderWidth, uint32_t displayWidth)
    {
        if (renderWidth == 0 || displayWidth == 0 || renderWidth >= displayWidth) return nativeBias;
        return nativeBias + std::log2(static_cast<float>(renderWidth) / static_cast<float>(displayWidth));
    }

    inline RHI::SamplerDesc Desc(uint32_t anisotropy, float mipBias)
    {
        RHI::SamplerDesc sd;
        sd.filter         = anisotropy > 1 ? RHI::Filter::ANISOTROPIC : RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU      = RHI::TextureAddressMode::WRAP;
        sd.addressV      = RHI::TextureAddressMode::WRAP;
        sd.addressW      = RHI::TextureAddressMode::WRAP;
        sd.maxAnisotropy = anisotropy;
        sd.mipLodBias   = mipBias;
        return sd;
    }

    // Rebuilds a pass's s0 sampler (@p index) for @p newBias, only when it differs from the @p bias it
    // was built with. The device's sampler pool is append-only and dedupes by description, so returning
    // to a ratio reuses that ratio's sampler and 1:1 gives back the boot sampler. Renderer::BeginFrame
    // drives it with the graph's resize recompile, never while a frame records. A failed creation keeps
    // the old sampler.
    template <class Device>
    bool Rebuild(Device& gfx, uint32_t anisotropy, float newBias, int& index, float& bias)
    {
        if (index >= 0 && newBias == bias) return true;
        int fresh = -1;
        if (!gfx.CreateSampler(Desc(anisotropy, newBias), fresh)) return false;
        index = fresh;
        bias  = newBias;
        return true;
    }
}
