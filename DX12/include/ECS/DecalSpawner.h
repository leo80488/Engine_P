#pragma once

// DecalSpawner - gameplay helper that creates dynamic decals, in one of two modes:
// - Pool mode: InitPool(world, N) pre-allocates N entities that Spawn* reuses round-robin,
//   recycling the oldest when full. Slots carry destroyEntityOnExpire = false, so at expiry
//   DecalLifetimeSystem removes only the DecalComponent and the entity stays for reuse.
//   For high-frequency hit effects.
// - Fresh mode (no pool): every Spawn* creates an entity with destroyEntityOnExpire = true.
//   For low-frequency spawns where entity churn does not matter.
// Example (pool mode):
//   renderer.GetDecalSpawner().InitPool(world, 256);  // scene init
//   auto mat = renderer.GetDecalMaterialLibrary().Find("blood_a");
//   renderer.GetDecalSpawner().SpawnAtSurface(world, mat, hit.pos, hit.normal,
//                                             {0.25f, 0.25f}, 0.1f, /*lifetime*/ 10.0f);

#include "ECS/ECS.h"
#include <DirectXMath.h>
#include <memory>
#include <cstdint>

namespace Resource { class DecalMaterialAsset; }

class DecalSpawner
{
public:
    DecalSpawner() = default;

    // Allocates @p capacity entities up-front, each with LocalTransform + GlobalTransform.
    // A second call drops the old slot list without destroying its entities: call World::Clear()
    // first when reinitialising for a different world.
    void InitPool(World& world, uint32_t capacity);

    // Called by Renderer::OnWorldClear: drops the slot list (World::Clear() already destroyed
    // the entities).
    void OnWorldClear();

    // Spawns with an explicit TRS. Returns the entity owning the new DecalComponent, or
    // NullEntity when @p material is null. Writes LocalTransform and GlobalTransform, so the decal
    // is visible the same frame; TransformSystem derives the same GlobalTransform next frame, since
    // the entity has no parent.
    Entity Spawn(World&                                          world,
                 std::shared_ptr<Resource::DecalMaterialAsset>   material,
                 const DirectX::XMFLOAT3&                        position,
                 const DirectX::XMFLOAT4&                        rotationQuat,
                 const DirectX::XMFLOAT3&                        scale,
                 float                                            lifetime,
                 float                                            fadeOutDuration = 0.5f);

    // Spawns a decal stuck onto a surface: local +Z maps onto -@p surfaceNormal, the direction
    // the DecalApply CS's angleFade term expects.
    //   @p size  - decal width / height in world units.
    //   @p depth - thickness along the projection axis (clip box Z).
    //   @p rollZ - rotation (radians) around the projection axis, for variation between instances.
    Entity SpawnAtSurface(World&                                         world,
                          std::shared_ptr<Resource::DecalMaterialAsset>  material,
                          const DirectX::XMFLOAT3&                       hitPos,
                          const DirectX::XMFLOAT3&                       surfaceNormal,
                          const DirectX::XMFLOAT2&                       size,
                          float                                           depth,
                          float                                           lifetime,
                          float                                           fadeOutDuration = 0.5f,
                          float                                           rollZ           = 0.0f);

    // Stats for profiler / debug UI.
    uint32_t GetPoolCapacity() const { return static_cast<uint32_t>(m_slots.size()); }
    uint32_t GetActiveCount(const World& world) const;

private:
    // Returns an entity for a fresh DecalComponent: the next pool slot (round-robin), or in fresh
    // mode a new entity with Local / Global transforms.
    Entity AcquireSlot(World& world);

    std::vector<Entity> m_slots;   // empty => pool disabled, fresh-mode spawn
    uint32_t            m_cursor = 0;
};
