#pragma once

// DebugCategory - bitmask of editor debug-visual categories.
//
// Every editor debug visual (collision/AABB/frustum/capsule wireframes, navmesh,
// probe gizmos, light billboard icons, ...) belongs to a category so the Debug
// menu can toggle them independently and DebugDrawSystem can drop submissions
// for disabled categories at the source. EDITOR-ONLY: never serialized and never
// a gameplay component (see docs/design/editor_debug_draw_system.md).

#include <cstdint>

enum class DebugCategory : uint32_t
{
    None             = 0,
    AABB             = 1u << 0,  // per-entity world-space bounding boxes
    Frustum          = 1u << 1,  // camera frustum
    Capsules         = 1u << 2,  // skeletal capsule colliders
    Collision        = 1u << 3,  // physics colliders (box / sphere / capsule / mesh)
    ReflectionProbes = 1u << 4,  // reflection-probe influence volumes
    DDGIVolumes      = 1u << 5,  // DDGI volume bounds + probe crosses
    NavMesh          = 1u << 6,  // recast/detour navmesh polygons
    PostProcessVolumes = 1u << 9, // post-process volume bounds + blend-distance shell
    Decals           = 1u << 10, // decal projection volumes (OBB + facing arrow)
    // ---- Billboard-icon categories (standalone gizmos, not under the master switch) ----
    // Add a new icon kind here and in DebugDrawSystem's kIconKinds[].
    Lights           = 1u << 7,  // light billboard icons
    Cameras          = 1u << 8,  // camera position icons
    VFX              = 1u << 11, // VFX emitter icons (particle / trail / beam)
    // ---- Wireframe category (under the master switch) ----
    // Point sphere + spot capped cone + directional arrow bundle: all light
    // gizmos (the name is kept for scene / settings compatibility).
    LightRanges      = 1u << 12,
    // Recast's INPUT triangle soup, coloured by walkability (green/red). The
    // NavMesh category above shows the OUTPUT; when the two disagree this is
    // the one that tells you why.
    NavSoup          = 1u << 13,
    // Local fog volume bounds (box / ellipsoid) + the edge-fade inner shell.
    FogVolumes       = 1u << 14,
    // Every Jolt body in the world (wireframe, by sleep state) and the character-controller capsules:
    // PhysicsSystem::DrawDebug. Not in Retail, whose Jolt has no debug renderer.
    Physics          = 1u << 15,
    All              = 0xFFFFFFFFu,
};

inline constexpr uint32_t operator|(DebugCategory a, DebugCategory b)
{
    return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}
inline constexpr uint32_t operator|(uint32_t a, DebugCategory b)
{
    return a | static_cast<uint32_t>(b);
}

// True if category `c` is set in `mask`.
inline bool DebugCatEnabled(uint32_t mask, DebugCategory c)
{
    return (mask & static_cast<uint32_t>(c)) != 0u;
}

// Set or clear category `c` in `mask`.
inline void DebugCatSet(uint32_t& mask, DebugCategory c, bool on)
{
    if (on) mask |=  static_cast<uint32_t>(c);
    else    mask &= ~static_cast<uint32_t>(c);
}
