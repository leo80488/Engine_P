#pragma once

// VFXSpecReflection.h - Reflect descriptors for the .ivfx per-emitter param
// structs that are NOT ECS components and so have no block in
// ComponentReflection.h. Included by ONE TU (VFXEditorWindow.cpp) so the
// explicit Descriptor<> specializations never collide across translation units.
//
// The Particle / Trail / Beam LOOK is drawn through the component descriptors
// (Reflect::DrawObject(&spec.particle) etc.); only the plain-POD Lane-B specs and
// the beam wrapper fields live here.

#include "Resource/VFXPrefab.h"
#include "Reflection/Reflect.h"

// ==== BeamVFXSpec ====
// The embedded BeamComponent is drawn via its own descriptor; only the wrapper's
// shader / blend fields are reflected here.
REFLECT_BEGIN(Resource::BeamVFXSpec)
    REFLECT_STRING (shaderPath, "Custom PS (shaders/...)")
    REFLECT_BOOL   (additive,   "Additive (glow)")
    REFLECT_INFO   ("Off = opaque inner core. Control points are edited above.")
REFLECT_END()

REFLECT_BEGIN(Resource::DecalVFXSpec)
    REFLECT_STRING (materialName, "Material (library name)")
    REFLECT_INFO   ("A DecalMaterialLibrary NAME, not a path. Register it in the Decal Materials editor.")
    REFLECT_FLOAT  (sizeX,           "Size X (m)",        0.01f, 20.f)
    REFLECT_FLOAT  (sizeY,           "Size Y (m)",        0.01f, 20.f)
    REFLECT_FLOAT  (depth,           "Projection Depth",  0.01f, 10.f)
    REFLECT_FLOAT_FMT(lifetime,      "Lifetime (s, <0=static)", -1.f, 120.f, "%.2f", 0.1f)
    REFLECT_FLOAT_FMT(fadeOutDuration,"Fade Out (s)",     0.f, 30.f, "%.2f", 0.05f)
    REFLECT_FLOAT_FMT(rollZ,         "Roll (rad)",       -6.2832f, 6.2832f, "%.3f", 0.01f)
    REFLECT_COLOR4 (tintOverride,    "Tint Override (a>0 replaces)")
REFLECT_END()

REFLECT_BEGIN(Resource::TracerVFXSpec)
    REFLECT_COLOR4 (color,     "Color (HDR, a=intensity)")
    REFLECT_FLOAT  (width,     "Width (m)",   0.001f, 5.f)
    REFLECT_FLOAT_FMT(lifetime,"Lifetime (s)",0.01f, 10.f, "%.2f", 0.02f)
    REFLECT_FLOAT  (length,    "Length (m)",  0.01f, 100.f)
    REFLECT_FLOAT3 (direction, "Local Direction", -1.f, 1.f)
    REFLECT_STRING_DROP(noiseTexPath, "Noise Texture (.itex)", "ITEX_PATH")
REFLECT_END()

REFLECT_BEGIN(Resource::AfterimageVFXSpec)
    REFLECT_INFO   ("Applies to the SPAWNER skinned character itself (snapshots its meshes).")
    REFLECT_FLOAT_FMT(lifetime, "Lifetime (s)", 0.02f, 5.f, "%.2f", 0.02f)
    REFLECT_COLOR4 (color,     "Tint (HDR rgb)")
REFLECT_END()

REFLECT_BEGIN(Resource::MeshVFXSpec)
    REFLECT_STRING_DROP(meshPath,     "Mesh (.meshlib)", "IMESHLIB_PATH")
    REFLECT_STRING_DROP(materialPath, "Material (.imat)", "IMAT_PATH")
    REFLECT_FLOAT_FMT(spinDegPerSec, "Spin (deg/s) [not yet wired]", -720.f, 720.f, "%.1f", 1.f)
    REFLECT_FLOAT3 (spinAxis,     "Spin Axis", -1.f, 1.f)
    REFLECT_BOOL   (castShadow,   "Cast Shadow")
REFLECT_END()
