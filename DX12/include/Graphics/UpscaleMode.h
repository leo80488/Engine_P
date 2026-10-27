#pragma once

// UpscaleMode - render-resolution scaling factor used by the upscaler stage.
//
// The HDR target is sized at render res; the render -> display boundary is FSR 2 (FSR2Pass) or
// DLSS (DLSSPass) when one of them is resolved, and the material samplers then take the matching
// negative mip bias (MaterialSampler.h). With TAA at a ratio > 1 the tonemap's sampler does a plain
// bilinear upscale.
//
// Ratios match AMD FSR 2 preset conventions:
//   Quality          1.5  (e.g. 1920x1080 -> render at 1280x720)
//   Balanced         1.7
//   Performance      2.0
//   UltraPerformance 3.0

#include <cstdint>

enum class UpscaleMode : uint32_t
{
    Native           = 0,  // 1.0 - no upscaling
    Quality          = 1,  // 1.5
    Balanced         = 2,  // 1.7
    Performance      = 3,  // 2.0
    UltraPerformance = 4,  // 3.0
};

inline float GetUpscaleRatio(UpscaleMode mode)
{
    switch (mode)
    {
        case UpscaleMode::Quality:          return 1.5f;
        case UpscaleMode::Balanced:         return 1.7f;
        case UpscaleMode::Performance:      return 2.0f;
        case UpscaleMode::UltraPerformance: return 3.0f;
        case UpscaleMode::Native:
        default:                            return 1.0f;
    }
}

inline const char* UpscaleModeName(UpscaleMode mode)
{
    switch (mode)
    {
        case UpscaleMode::Quality:          return "Quality (1.5x)";
        case UpscaleMode::Balanced:         return "Balanced (1.7x)";
        case UpscaleMode::Performance:      return "Performance (2.0x)";
        case UpscaleMode::UltraPerformance: return "Ultra Performance (3.0x)";
        case UpscaleMode::Native:
        default:                            return "Native (1.0x)";
    }
}

// Computes the internal render resolution for a display resolution: display / ratio rounded to the
// nearest pixel, clamped to >= 1 in each dimension so a small dimension never becomes 0. Native
// returns the display size.
inline void ComputeRenderSize(uint32_t displayW, uint32_t displayH,
                              UpscaleMode mode,
                              uint32_t& outRenderW, uint32_t& outRenderH)
{
    const float ratio = GetUpscaleRatio(mode);
    if (ratio <= 1.0f)
    {
        outRenderW = displayW;
        outRenderH = displayH;
        return;
    }
    const float fw = static_cast<float>(displayW) / ratio;
    const float fh = static_cast<float>(displayH) / ratio;
    outRenderW = (fw < 1.0f) ? 1u : static_cast<uint32_t>(fw + 0.5f);
    outRenderH = (fh < 1.0f) ? 1u : static_cast<uint32_t>(fh + 0.5f);
}
