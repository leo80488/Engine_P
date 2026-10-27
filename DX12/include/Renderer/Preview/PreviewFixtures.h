#pragma once

// PreviewFixtures - MaterialComponents synthesised in code, one per shading model / blend
// mode the material preview claims to cover (Standard, Metal, Masked, Dithered, Toon, Aniso,
// SSS, ClearCoat, TwoSided, Unlit, custom VS, a masked hook PS, plus shadow, glass, water and
// forward-twin rows; the table in the .cpp is the roster).
//
// Why in code and not as .imat assets: most rows have no material on disk that selects them
// (the tree's .imat files are Standard or Toon), and .imat is a binary container only
// Resource::SaveMaterial may write, so test assets would be binaries nobody can review in a
// diff or need an untested generator. A builder is reviewable, costs no files, cannot rot
// against the serializer, and gives the A/B parity harness the same material on both sides.
//
// What every fixture shares, and why:
//   * The engine default textures (MaterialComponent's constructor seeds them): no asset, no
//     picture change on a re-import, and no mip bias / anisotropy / streaming in the parity
//     floor. Exception: `masked` binds an OPACITYMAP, an engine-shipped uncompressed ramp
//     (see Masked() in the .cpp), because with every clip input at 1.0 a synthesised Masked
//     material keeps or discards every texel and looks identical to `standard`. It is not
//     in any parity pack.
//   * DISABLE_RECEIVE_SHADOW. The main pipeline always runs CSM for a directional light
//     (LightData::castsShadow only gates spot/point) while the preview's studio shadow is one
//     ortho map, so a receiving fixture would differ between the two by construction; the
//     flag makes the pixel always lit, the same value on both sides.
//   * CAST_SHADOW cleared, for the same reason from the other end: a caster in the parity
//     scene would shadow itself through the cascades.
//     The *shadow and maskedhook fixtures change both flags on purpose, and customvs /
//     customvs_off set CAST_SHADOW; the .cpp says why for each.
//   * No custom shader, except `customvs` / `customvs_off` (the custom-vertex A/B pair: the
//     WPO claim needs the same subject rendered with the branch on and off in one run) and
//     `maskedhook` (a test hook PS). The custom-PS path is covered by preview_material_custom
//     against a shipping material. A fixture only states a path:
//     Renderer::ResolvePreviewCustomPSID / ResolvePreviewCustomVSID register it on the GAME
//     half, through the one fan-out, as for any .imat - the preview registers nothing.
//
// Editor/harness-facing but in EngineCore on purpose: MaterialComponent, ShadingModel and
// PreviewStudioRig live there, and the parity fixture builds its World from here, on the same
// side of the WITH_EDITOR line as Game.exe.

#include "ECS/Components.h"   // MaterialComponent, BlendMode

#include <cstddef>
#include <string_view>

namespace Preview
{
    // Fills @p out with the fixture named @p name (lower-case, see the table in the .cpp).
    // Returns false and leaves @p out untouched for an unknown name: callers report it rather
    // than substitute a default, since a typo that silently renders `standard` would make a pack
    // pass having tested the wrong row.
    bool MakeFixtureMaterial(std::string_view name, MaterialComponent& out);

    // The fixture roster, for diagnostics and the "unknown name" message. FixtureNameAt
    // returns nullptr for @p i >= FixtureCount().
    std::size_t FixtureCount();
    const char* FixtureNameAt(std::size_t i);

    // The stencil ShadingStencil::RefFor derives for the named fixture, or 0 for an unknown
    // name. Logged by the harness so a pack can assert the permutation each fixture selected
    // without re-deriving the mapping.
    unsigned FixtureStencil(std::string_view name);
}
