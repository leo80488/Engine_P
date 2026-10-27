#pragma once

// BillboardComponent - marks an entity as a camera-facing billboard.
// GlobalTransform supplies the position only: Renderer::BuildScene_GatherBillboards
// builds a camera-facing world matrix from the camera right / up axes and sets the
// BILLBOARD permutation bit. Ignored on light entities (their icons are editor-only).
// Routing:
//   Opaque / AlphaClip     -> GBufferPass (deferred, lit)
//   Transparent / Additive -> TransparentPass (forward, blended)

#include <cstdint>

enum class BillboardMode : uint8_t
{
    Opaque      = 0,  // GBuffer pass, deferred lit
    AlphaClip   = 1,  // GBuffer pass with alpha test (clip), deferred lit
    Transparent = 2,  // Forward, SRC_ALPHA / INV_SRC_ALPHA
    Additive    = 3,  // Forward, SRC_ALPHA / ONE (particles, glow)
};

struct BillboardComponent
{
    BillboardMode mode       = BillboardMode::Transparent;
    float         worldSize  = 1.0f;    // world-space width/height of the quad
    bool          fixedSize  = false;   // meant as constant screen-space size; reflected, but the renderer does not read it (the quad uses worldSize)
    float         fixedPixels = 48.0f;  // screen-space size for fixedSize (likewise unread)
};
