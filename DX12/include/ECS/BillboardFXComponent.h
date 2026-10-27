#pragma once

// BillboardFXComponent - an animated, camera-facing sprite-sheet quad (atlas
// flipbook): one self-contained VFX entity for explosions, impacts, smoke puffs,
// muzzle flashes, glows.
// Position comes from GlobalTransform; orientation is rebuilt each frame from the
// camera, so the entity's own rotation is ignored (like BillboardComponent).
// BillboardFXPass draws it into the HDR scene colour BEFORE tonemap with a
// read-only depth test, so additive / emissive sprites glow through Bloom;
// depthTest = false makes it an always-on-top overlay.

#include <cstdint>
#include <string>
#include <DirectXMath.h>

// Camera-facing mode.
enum class BillboardFXFace : uint8_t
{
    Spherical   = 0,  // squarely faces the camera (full billboard)
    Cylindrical = 1,  // yaws to the camera but stays world-up (smoke columns)
};

// Blend mode (drives the PSO blend state).
enum class BillboardFXBlend : uint8_t
{
    Alpha    = 0,  // SRC_ALPHA / INV_SRC_ALPHA: smoke, soft sprites, decals
    Additive = 1,  // SRC_ALPHA / ONE: fire, energy, glow (default)
};

// Flipbook playback behaviour.
enum class BillboardFXPlayback : uint8_t
{
    Loop     = 0,  // wrap back to frame 0
    Once     = 1,  // play through, then hold the last frame
    PingPong = 2,  // 0,1,..,N-1,N-2,..,0,..
};

struct BillboardFXComponent
{
    // ---- Sprite sheet (texture atlas) ----
    std::string texturePath;            // atlas image (.itex / .dds / ...)
    int   columns    = 1;               // atlas grid columns
    int   rows       = 1;               // atlas grid rows
    int   frameCount = 0;               // animated frames (0 => columns*rows)
    float fps        = 24.0f;           // playback speed (frames / second)

    // ---- Appearance ----
    float             size     = 1.0f;            // world-space quad size (height = width)
    DirectX::XMFLOAT4 tint     = { 1, 1, 1, 1 };  // RGB tint, A = base opacity
    float             emissive = 1.0f;            // HDR colour multiplier (bloom glow)
    float             opacity  = 1.0f;            // master fade [0..1]

    // ---- Behaviour ----
    BillboardFXFace     face     = BillboardFXFace::Spherical;
    BillboardFXBlend    blend    = BillboardFXBlend::Additive;
    BillboardFXPlayback playback = BillboardFXPlayback::Loop;
    bool depthTest = true;              // false = always-on-top overlay
    bool playing   = true;              // pause/resume the flipbook

    // ---- Runtime state (not serialized, rebuilt every frame) ----
    float    elapsed            = 0.0f; // accumulated playback time
    int      frame              = 0;    // current atlas frame index
    int32_t  textureBindlessIdx = -1;   // resolved bindless slot (-1 = none/pending)
    uint64_t textureGpuHandle   = 0;    // resolved SRV gpu handle (editor badge)
};
