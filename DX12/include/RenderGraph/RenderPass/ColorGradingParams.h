#pragma once

// ColorGradingParams - CPU-side parameters of the procedural 3D LUT generation.
// Must match the cbuffer layout in GenerateLUT.cs.hlsl (16-byte rows); operator== compares
// the bytes, padding included, so the pads stay zero-initialised.

#include <DirectXMath.h>
#include <cstring>

struct alignas(16) ColorGradingParams
{
    // ---- Exposure and contrast ----
    float exposure   = 0.0f;   // EV offset: stops of light, so x 2^(ev/2.2) on the gamma-encoded value
    float contrast   = 1.0f;   // 1 = neutral, >1 = more contrast
    float brightness = 0.0f;   // additive offset
    uint32_t debugFlags = 0;   // harness reverse arm: bit 0 = the old tint sign and exposure maths

    // ---- Colour balance (lift / gamma / gain) ----
    DirectX::XMFLOAT3 lift  = { 0.f, 0.f, 0.f };   // shadows offset
    float _pad1 = 0.f;
    DirectX::XMFLOAT3 gamma = { 1.f, 1.f, 1.f };   // midtones power
    float _pad2 = 0.f;
    DirectX::XMFLOAT3 gain  = { 1.f, 1.f, 1.f };   // highlights multiply
    float _pad3 = 0.f;

    // ---- HSL ----
    float hueShift   = 0.0f;   // degrees, -180..180
    float saturation = 1.0f;   // 1 = neutral
    float vibrance   = 0.0f;   // boosts only low-saturation regions
    float _pad4      = 0.0f;

    // ---- White balance ----
    float temperature = 0.0f;  // -1..+1 (cool..warm)
    float tint        = 0.0f;  // -1..+1 (green..magenta): +1 lowers green
    float _pad5[2]    = {};

    // ---- Vignette and grain ----
    float vignetteStrength = 0.0f;
    float filmGrain        = 0.0f;
    float _pad6[2]         = {};

    bool operator==(const ColorGradingParams& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
    bool operator!=(const ColorGradingParams& o) const { return !(*this == o); }
};

static_assert(sizeof(ColorGradingParams) % 16 == 0, "ColorGradingParams must be 16-byte aligned");
