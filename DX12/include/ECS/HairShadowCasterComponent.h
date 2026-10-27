#pragma once

// HairShadowCasterComponent: opt-in tag. This entity and its descendant subtree are rendered into
// the small hair shadow map (the Hair variant of PerObjectShadowPass), a directional shadow fitted
// to the tagged bangs/fringe meshes. Only NPR face pixels sample it, in Lighting.ps (extra.a marker
// from Face.ps), where it is min()'d into the SDF face terminator: the anime bangs shadow on the
// face.
//
// Add it (editor -> Add Component -> "Hair Shadow Caster") to the bangs entity, or the parent of
// the bangs meshes, of an NPR character. The face receiver needs no tag: any face pixel (SDF marker
// set) receives it. Body and hair keep their normal CSM / per-object shadow casting; this map is an
// additional projection, never composed into the scene-wide shadow factor.
//
// Limits (like the player per-object map):
//   * One global map: tag one character's bangs at a time. Several tagged characters union their
//     AABBs into one fit; metres of separation collapse the texel density and the mm-scale bangs
//     shadow vanishes.
//   * The fit tracks the entity transform, not bone-space head motion. An animation that moves the
//     head far from the bind-pose bounds (crouch, lie down) can leave the fitted footprint and the
//     shadow soft-fades out; raise fitScale then.

#include "ECS/ECS.h"

struct HairShadowCasterComponent : ComponentBase
{
    // How the bangs shadow is produced (Lighting.ps min()s whichever ran):
    //   0 = Screen Mark (default), a DanbaidongRP CharacterFringe port: the bangs are re-drawn with a
    //       view-space offset along the light's screen direction and stamp a flag (extra.g) on the
    //       face pixels (HairShadowMarkPass). Crisp, contact-perfect, no shadow map.
    //   1 = Shadow Map: the fitted bangs-only depth map (Hair variant of PerObjectShadowPass, t43).
    //       Projected, works at any light angle, softer edge.
    //   2 = Both: min() of the two.
    int mode = 0;

    // Artistic strength of the bangs shadow on the face: 0 = invisible,
    // 1 = full ramp-shadow depth (uploaded to LightCB.hairShadowStrength).
    float strength = 1.0f;

    // ---- Shadow Map tuning (mode 1/2) ----
    // Scales the fitted ortho footprint around the bangs AABB. Raise it if the shadow clips at the
    // frustum border (soft-fades out) on wide faces or at a grazing sun; lower it for maximum texel
    // density.
    float fitScale = 1.0f;

    // ---- Screen Mark tuning (mode 0/2, DanbaidongRP reference defaults) ----
    // Every metre distance below is multiplied by the tagged root's world scale, so the mark keeps its
    // proportion when the character is resized through its hierarchy.
    // The base constants are tiny (mm-scale, tuned for ~1.6 m characters): near 0 the mark hides
    // behind the fringe's own silhouette. Typical visible range 2-6; X acts only when the light has a
    // sideways view-space component, Y fades as the camera rises above the head.
    // Horizontal shift scale: view-space metres = -0.0045 x lightDirVS.x x X.
    float screenOffsetX = 1.0f;
    // Downward shift scale: view-space metres = -0.0075 x Y x camAboveFactor.
    float screenOffsetY = 1.0f;
    // Depth push scale: metres along the camera ray = 0.01 x this. Larger needs a bigger
    // fringe-to-face gap before the mark lands (kills marks on the fringe's own roots); smaller =
    // tighter contact.
    float depthPush = 0.5f;
};
