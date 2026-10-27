#pragma once

// Opt-in interactive ripples for a water-domain surface (a plane mesh + a material with
// Domain = Water). Look, placement and size come from the mesh, transform and material; this holds
// only what the renderer cannot derive: the sim budget (wave speed, decay, window size). Put it on
// the entity that carries the water mesh + material; without it there are no interactive ripples
// (flow-normal motion is the material's job).
// The footprint comes from the entity's GlobalTransform: surface level = translation.y, body
// extent = its XZ scale.
// Limitation (deliberate): the sim is an axis-aligned square with one scalar water level - the
// containment test, the texel-snapped window and the physics surface-crossing test all assume it -
// so keep this entity axis-aligned and horizontal. Rendering may rotate, tilt and non-uniformly
// scale the water freely.

#include <cstdint>

struct WaterInteractionComponent
{
    bool  enabled = true;

    // Wave speed. GPU-side CFL clamp keeps this <= 0.30 whatever is authored.
    float propagation = 0.15f;
    // Per-step energy loss. Low = rings persist and spread.
    float damping = 0.015f;
    // Impulse height per unit of source strength.
    float amplitude = 1.0f;
    // Direct dial: height gradient -> normal tilt, read by the material's WaterRippleSlope() call.
    // Do not divide it by body size: ripples would vanish on large bodies and explode on small ones.
    float normalStrength = 3.0f;

    // Sim window size in metres: ripples are simulated only in a square of this size that follows the
    // interaction, not across the whole body, so a stone in a 4 km ocean makes a local ring. Keep it
    // close to the area actually interacted with; smaller = finer detail for the same texture budget.
    float areaSize = 120.0f;

    // Renderer-internal, published by WaterRippleSystem each frame. rippleBindlessIndex is
    // RHI::Texture::handleId (a bindless heap index, not a GPU descriptor handle) that
    // water_common.hlsli opens with ResourceDescriptorHeap[]; 0 = no field this frame.
    mutable uint32_t rippleBindlessIndex = 0;
    mutable float    winOriginX = 0.0f;
    mutable float    winOriginZ = 0.0f;
    mutable float    winSize    = 1.0f;
};
