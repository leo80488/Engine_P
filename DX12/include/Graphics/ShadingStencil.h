#pragma once

// ShadingStencil - the GBuffer stencil code that selects a deferred lighting variant, and
// RefFor, which derives it from a ShadingModel.
// GBufferPass stamps one code per pixel; LightingPass runs three PSOs, each testing stencil EQUAL
// against one code, so the code is a COMPILE-TIME specialisation selector (Lighting.ps.hlsl has
// some twenty `#if NPR_PASS` regions; the Unlit variant early-outs). It plays Unreal's
// ShadingModelID role and is NOT redundant with the per-pixel `customShadingModel` branch: one
// runtime branch instead would diverge per pixel across the whole screen. Every writer and
// tester uses these constants: a private copy that drifts compiles, runs, draws nothing and
// logs nothing.
// The values are a GPU contract - do not renumber them. LightingPass tests EQUAL under
// stencilReadMask = kCodeMask, so a pixel whose code matches no lighting draw is BLACK (it keeps
// only the GBuffer emissive seed). Three writers have no MaterialComponent behind them
// (OutlineGeometryPass stamps Unlit on rim pixels; TerrainPass and GrassPass stamp PBR), so
// following the material code does not find them.
// The byte is shared: the shading codes keep 0x00..0x3F (kCodeMask), ParticlePasses and
// TrailPasses stamp kTaaExclude (0x40) and OutlinePass kOutlineRim (0x80), each with a masked
// write. TAA and the upscaler reactive mask read both flag bits through the stencil-plane SRV
// (the masks reach the shaders in their CBs); the debug target `stencil` shows the byte.
// RenderGraph/StencilBits.h declares who writes and who tests each field, and the render graph
// holds every pipeline to exactly its field. Keep the codes small and dense.

#include "Graphics/ShadingModel.h"
#include "Graphics/ShadingModelTable.h"

#include <cstdint>

namespace ShadingStencil
{
    // Must match the values GBufferPass writes and LightingPass tests.
    inline constexpr uint8_t kPBR   = 1;
    inline constexpr uint8_t kNPR   = 2;
    inline constexpr uint8_t kUnlit = 3;
    // The table's `route` column IS this code (shaders/shading_model_table.hlsli).
    static_assert(kPBR == SMT_ROUTE_PBR && kNPR == SMT_ROUTE_NPR && kUnlit == SMT_ROUTE_UNLIT,
                  "shading_model_table.hlsli's SMT_ROUTE_* must equal the stencil codes");

    // The two flag bits above the codes.
    inline constexpr uint8_t kCodeMask   = 0x3F;   // the shading codes' bits
    inline constexpr uint8_t kTaaExclude = 0x40;   // a particle / trail kept out of TAA's history (excludeFromTAA)
    inline constexpr uint8_t kOutlineRim = 0x80;   // the outline rim (OutlinePass, while it tags for TAA)
    static_assert(((kPBR | kNPR | kUnlit) & ~kCodeMask) == 0 && (kTaaExclude & kCodeMask) == 0 &&
                  (kOutlineRim & kCodeMask) == 0 && (kTaaExclude & kOutlineRim) == 0,
                  "the shading codes, kTaaExclude and kOutlineRim share the stencil byte without overlapping");

    // The material's lighting route: the table's `route` column for its shading model. Every model
    // but Unlit and Toon writes the standard GBuffer and is lit by the PBR variant; the per-model
    // BRDFs (Subsurface, Anisotropic, TwoSidedFoliage, ClearCoat) are runtime branches inside it,
    // not separate PSOs. Unlit early-outs to baseColor, its emissive arriving through the additive
    // blend onto the GBuffer-seeded HdrSceneColor; Toon selects the NPR permutation, the code
    // HairShadowMarkPass gates its bangs->face mark on. Water never reaches the deferred lighting
    // pass (WaterSurfacePass forward-shades it and stamps a GBuffer subset), so its PBR code is
    // never read - the honest default rather than a fourth, dead value. A value outside the enum
    // reads PBR (ShadingModels::kUnknown).
    constexpr uint8_t RefFor(ShadingModel m)
    {
        return ShadingModels::Of(m).route;
    }
}
