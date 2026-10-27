#include "Scene/MeshSpawner.h"
#include "ECS/ECS.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Resource/ProceduralMesh.h"
#include "Resource/MaterialSerializer.h"
#include "System/Log.h"

// Forward slashes and the current location: Spawn writes this into the world as
// MaterialSourcePath, so the .iscene serialises it exactly as spelled here.
static const char* kDefaultMatPath = "engine/textures/Default_mat.imat";

Entity MeshSpawner::Spawn(int meshType, World& world,
                          const DirectX::XMFLOAT3& position)
{
    if (meshType < 0 || meshType >= static_cast<int>(PrimitiveMeshType::Count))
        return NullEntity;

    static const char* kNames[] = { "Cube", "Sphere", "Cone", "Plane", "Torus" };

    Entity e = world.CreateEntity();
    world.SetName(e, kNames[meshType]);
    LocalTransform spawnLT{};
    spawnLT.translation = position;
    world.AddComponent<LocalTransform>(e, spawnLT);
    world.AddComponent<GlobalTransform>(e, GlobalTransform{});
    world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
    world.AddComponent<RenderLayer>(e, RenderLayer{});
    world.AddComponent<Children>(e, Children{});

    MeshHandle mh;
    mh.gpuMeshID = static_cast<uint32_t>(meshType);
    world.AddComponent<MeshHandle>(e, mh);

    // Unit bounding box used for mouse picking (half-extents fit every primitive;
    // the Plane is flat and the Torus is sized to stay inside the unit box).
    world.AddComponent<BoundingVolume>(e, BoundingVolume{});

    MaterialComponent mat{};
    if (Resource::LoadMaterial(kDefaultMatPath, mat))
        mat.SetDirty();
    world.AddComponent<MaterialComponent>(e, std::move(mat));
    world.AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ kDefaultMatPath });

    LOG_INFO("MeshSpawner: spawned %s (entity=%u)", kNames[meshType], e);
    return e;
}

Entity MeshSpawner::SpawnWaterPlane(World& world,
                                    const DirectX::XMFLOAT3& position,
                                    float size)
{
    Entity e = world.CreateEntity();
    world.SetName(e, "Water");

    LocalTransform lt{};
    lt.translation = position;
    // The Plane primitive is a unit quad in XZ: scale X/Z to the footprint. Y stays 1: water has
    // no thickness, and a non-unit Y would skew the tangent frame the flow-normal map is applied in.
    lt.scale = { size, 1.0f, size };
    world.AddComponent<LocalTransform>(e, lt);
    world.AddComponent<GlobalTransform>(e, GlobalTransform{});
    world.AddComponent<VisibilityComponent>(e, VisibilityComponent{});
    world.AddComponent<RenderLayer>(e, RenderLayer{});
    world.AddComponent<Children>(e, Children{});

    MeshHandle mh;
    mh.gpuMeshID = static_cast<uint32_t>(PrimitiveMeshType::Plane);
    world.AddComponent<MeshHandle>(e, mh);
    world.AddComponent<BoundingVolume>(e, BoundingVolume{});

    MaterialComponent mat{};
    // Unreal's Single Layer Water: an ordinary translucent Surface material whose shading model is
    // Water, not a domain (IsWaterShadingModel()).
    mat.customShadingModel = ShadingModel::Water;
    mat.userBlendMode      = BlendMode::Alpha;
    mat.useCustomShader = true;
    mat.customShaderPath = "asset/Shaders/water_surface.hlsl";
    // Double-sided so the surface is still shaded with the camera below it. The hook must not flip
    // the normal itself: custom_surface.hlsli's SV_IsFrontFace handling does, and both cancel out.
    mat.flags |= MaterialComponent::DOUBLE_SIDED;
    // Water never casts a shadow: it is an alpha-blended forward sheet with no
    // shadow-pass PSO. ApplyBlendMode clears this anyway; setting it here keeps
    // the authored state honest in the Inspector.
    mat.flags &= ~static_cast<uint32_t>(MaterialComponent::CAST_SHADOW);
    // Deep-water base colour; the shader derives its shallow tint from it.
    mat.baseColor = { 0.012f, 0.07f, 0.14f, 1.0f };
    mat.SetDirty();
    world.AddComponent<MaterialComponent>(e, std::move(mat));
    // Deliberately no MaterialSourcePath (see the declaration).

    LOG_INFO("MeshSpawner: spawned Water plane %.1fm (entity=%u)", size, e);
    return e;
}
