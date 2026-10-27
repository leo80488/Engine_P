#pragma once

// MeshSpawner: creates procedural-mesh entities.
// Knows only the ECS (World, Entity, Transform, MeshHandle) and PrimitiveMeshType; no
// Renderer or GPU dependency.

#include "ECS/ECS.h"
#include <DirectXMath.h>

class World;

class MeshSpawner
{
public:
    // Creates an entity at @p position with a default Transform, a MeshHandle to the pre-uploaded
    // GPU primitive for @p meshType (0=Cube, 1=Sphere, 2=Cone, 3=Plane, 4=Torus; PrimitiveMeshType)
    // and the default material. The editor passes a point in front of the camera so new meshes
    // appear in view. Returns the entity, or NullEntity if meshType is out of range.
    static Entity Spawn(int meshType, World& world,
                        const DirectX::XMFLOAT3& position = { 0.f, 0.f, 0.f });

    // Creates a ready-to-use water surface: a Plane primitive @p size metres square with a
    // MaterialComponent of shading model ShadingModel::Water (a translucent Surface material, as in
    // Unreal) and the shipped water shader.
    //
    // The material is built in code and carries no MaterialSourcePath, so it persists through the
    // `MaterialInline:` scene serializer. Deliberate: a .imat must only be produced by
    // Resource::SaveMaterial, so a hand-authored default .imat cannot ship, and an inline material
    // means editing this water does not change an asset other lakes share.
    static Entity SpawnWaterPlane(World& world,
                                  const DirectX::XMFLOAT3& position = { 0.f, 0.f, 0.f },
                                  float size = 50.0f);
};
