#pragma once

// HDROutput - display output transform selection (SDR / HDR10 / scRGB).
//
// ==== The composition space ====
// In every mode the tonemap output holds EXTENDED-GAMMA values, not linear nits:
//     stored = pow(linearNits / paperWhiteNits, 1/2.2)
//   stored == 1.0 is paper white ("SDR white"), < 1.0 the ordinary SDR range, > 1.0 HDR
//   headroom up to pow(peakNits / paperWhiteNits, 1/2.2).
// Why: the raster overlays (DebugWire / WorldUIBillboard / DebugIcon / UI) and the editor's ImGui
// alpha-blend INTO this buffer with 8-bit sRGB-authored colours, and UIPass builds PSOs for custom
// text shaders in game assets that the engine cannot rescale. In this space sRGB UI colours land at
// paper white with no shader edits, blending stays in gamma space exactly as in SDR, and the SDR
// encode is the plain pow(1/2.2) tail, so HDR support cannot change the SDR picture.
// Only the tail of ToneMap.cs.hlsl (produce) and the present pixel shader (consume) know the display
// encoding; everything between them is format-agnostic. HDR modes need an FP16 tonemap output (see
// HDRCompositionFormat).
//
// ==== The two HDR signal standards ====
// HDR10 - R10G10B10A2_UNORM swap chain, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: Rec.2020
//         primaries, ST.2084 (PQ) transfer, absolute nits (1.0 == 10000 nits). What TVs and most
//         HDR monitors expect; exclusive fullscreen passes it to the display untouched. 10 bits
//         suffice only because PQ is perceptually uniform.
// scRGB - R16G16B16A16_FLOAT swap chain, DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: Rec.709
//         primaries, linear, 1.0 == 80 nits; values may exceed 1.0 or go negative (colours outside
//         Rec.709). The compositor converts it for the panel, so it suits windowed mode. Costs 2x
//         back-buffer bandwidth.
// Both are driven from the same extended-gamma buffer; only the present pixel shader's tail differs
// (SurfaceDX12::EnsurePresentPSO).

#include "Graphics/GraphicsStruct.h"   // RHI::Format (descriptor-only layer, no DX12 objects)

#include <cstdint>
#include <string_view>

enum class HDROutputMode : uint32_t
{
    SDR   = 0,  // R8G8B8A8_UNORM back buffer, sRGB-ish gamma. The default.
    HDR10 = 1,  // R10G10B10A2_UNORM back buffer, Rec.2020 + ST.2084 PQ.
    scRGB = 2,  // R16G16B16A16_FLOAT back buffer, Rec.709 + linear, 1.0 = 80 nits.
};

// Settings.json token of a mode. Strings, not enum indices, so inserting a mode into the enum
// cannot silently re-point existing settings files at a different mode.
inline const char* HDROutputModeToString(HDROutputMode m)
{
    switch (m)
    {
        case HDROutputMode::HDR10: return "HDR10";
        case HDROutputMode::scRGB: return "scRGB";
        case HDROutputMode::SDR:
        default:                   return "SDR";
    }
}

inline HDROutputMode HDROutputModeFromString(const char* s)
{
    if (!s) return HDROutputMode::SDR;
    // Unknown token -> SDR: a corrupt or future settings file must degrade to the mode every display
    // can show, never to an HDR mode the panel cannot show.
    if (std::string_view(s) == "HDR10") return HDROutputMode::HDR10;
    if (std::string_view(s) == "scRGB") return HDROutputMode::scRGB;
    return HDROutputMode::SDR;
}

inline const char* HDROutputModeName(HDROutputMode m)
{
    switch (m)
    {
        case HDROutputMode::HDR10: return "HDR10 (PQ / Rec.2020, 10-bit)";
        case HDROutputMode::scRGB: return "scRGB (linear FP16, Rec.709)";
        case HDROutputMode::SDR:
        default:                   return "SDR (8-bit sRGB)";
    }
}

// Nit defaults and authoring ranges.
// paperWhite: the nit level "SDR white" maps to. 200 is the Windows HDR desktop slider's default
// and what most HDR titles ship; below ~100 the UI looks murky next to SDR apps, above ~400 it is
// fatiguing in a dark room.
// peak: the brightest value the tone curve rolls off toward. Track the panel's real peak
// (ISurface::GetDisplayHDRInfo().maxLuminanceNits): claiming more than the display has lets the
// panel's own tone mapping clip the top of the curve, which is worse than rolling off ourselves.
inline constexpr float kHDRPaperWhiteDefault = 200.0f;
inline constexpr float kHDRPaperWhiteMin     =  80.0f;   // scRGB 1.0
inline constexpr float kHDRPaperWhiteMax     = 480.0f;

inline constexpr float kHDRPeakNitsDefault   = 1000.0f;
inline constexpr float kHDRPeakNitsMin       =  400.0f;
inline constexpr float kHDRPeakNitsMax       = 4000.0f;

// scRGB's defining constant: 1.0 == 80 nits.
inline constexpr float kSCRGBNitsPerUnit     = 80.0f;

// ST.2084 encodes absolute luminance normalized to this ceiling.
inline constexpr float kPQMaxNits            = 10000.0f;

// Gamma the composition space is encoded with: the SDR tonemap tail's pow(1/2.2).
inline constexpr float kCompositionGamma     = 2.2f;

inline bool IsHDR(HDROutputMode m) { return m != HDROutputMode::SDR; }

// Single authority for the composition-buffer (tonemap output) format.
// Every PSO that renders into the tonemap output takes RTVFormats[0] from here, never from a
// literal: the RTV inherits the texture's format, so a stale hardcoded RGBA8 is a debug-layer error
// and undefined output with no compile error. Users: ToneMapPass (creates it), UIPass,
// WorldUIBillboardPass, DebugIconPass, DebugWirePass, GlassShatterPass's frozen-frame copy and the
// editor's ImGui composition buffer.
// SDR: RGBA8 (half FP16's memory and bandwidth). HDR: FP16, the only format that both stores the
// headroom above 1.0 and is guaranteed typed UAV load + store, which GlassShatterComposite.cs.hlsl
// does in place on this texture.
inline RHI::Format HDRCompositionFormat(HDROutputMode m)
{
    return IsHDR(m) ? RHI::Format::R16G16B16A16_FLOAT
                    : RHI::Format::R8G8B8A8_UNORM;
}
