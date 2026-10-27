#pragma once

// FaceOverlayComponent - opt-in tag: this entity AND its descendant subtree are
// redrawn by FaceOverlayPass after opaque lighting, alpha-blended over the hair
// that covers them (eyebrows / eyelashes visible through hair, the DanbaidongRP
// CharacterEyelashOutter technique). Add it to the EYEBROW / EYELASH entities of an
// NPR character.
// Requirements:
//   * The HAIR material's GBuffer PS must write extra.b = 1 (the hair mask;
//     asset/Shaders/Hair.ps.hlsl does, custom hair shaders must too), else the overlay
//     finds no hair pixels to draw on.
//   * The tagged meshes keep their normal GBuffer draw; this only ADDS the
//     see-through redraw where hair covers them.
// alpha / maxHairDistance are per tagged subtree, so several characters (or
// independently tagged meshes) in one scene can use different settings.

#include "ECS/ECS.h"

struct FaceOverlayComponent : ComponentBase
{
    // Overlay opacity where the lash is fully covered by near hair.
    float alpha = 0.35f;

    // Lash -> covering-hair gap (metres) past which the overlay fades out entirely.
    // Also keeps the far side of the head from showing through (head width >> this).
    // Reference: 0.05 (DanbaidongRP's `* 20`).
    float maxHairDistance = 0.05f;
};
