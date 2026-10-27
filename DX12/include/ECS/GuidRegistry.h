#pragma once

// GuidRegistry: process-global Guid -> Entity lookup used by AttachmentRef
// (docs/design/entity_persistence_architecture.md).
//
// Lifecycle:
//   - App init: RegisterWithWorld(world) installs an EntityDestroyListener, so destroyed
//     entities unregister their Guid.
//   - After a scene load: RebuildFromWorld(world) re-populates the map in one pass over the
//     GuidComponent pool.
//   - With World::Clear: Clear() drops the whole map.
//
// Not thread-safe: every mutation runs on the main thread (entity create/destroy, scene load),
// like the rest of the ECS.

#include "ECS/ECS.h"
#include "ECS/Guid.h"

#include <unordered_map>

class World;

namespace ECS
{

class GuidRegistry
{
public:
    // Process-wide singleton: App has one World at a time. A second live World would need a
    // World-owned registry.
    static GuidRegistry& Get();

    // Install the destroy listener so entities with a GuidComponent unregister when DestroyEntity
    // runs. Idempotent; keeps the listener handle for DetachFromWorld.
    void RegisterWithWorld(World& world);

    // Drop the destroy listener. Call before destroying the World it was
    // attached to.
    void DetachFromWorld(World& world);

    // Manual register / unregister: normally only the GuidComponent serializer and RebuildFromWorld
    // call these.
    void Register(Guid guid, Entity e);
    void Unregister(Guid guid);
    void UnregisterEntity(Entity e);  // for the destroy hook

    // Returns NullEntity when guid isn't registered.
    Entity Find(Guid guid) const noexcept;

    // Returns kInvalidGuid when entity isn't registered.
    Guid   ReverseFind(Entity e) const noexcept;

    // Wipe the entire map. The scene loader calls it right after World::Clear.
    void Clear() noexcept;

    // Walk every GuidComponent in the world and (re)populate the map.
    // Drops the previous contents first. Use after a scene load.
    void RebuildFromWorld(World& world);

    size_t Size() const noexcept { return m_guidToEntity.size(); }

private:
    GuidRegistry()  = default;
    ~GuidRegistry() = default;
    GuidRegistry(const GuidRegistry&)            = delete;
    GuidRegistry& operator=(const GuidRegistry&) = delete;

    std::unordered_map<Guid, Entity> m_guidToEntity;
    std::unordered_map<Entity, Guid> m_entityToGuid;  // reverse map for unregister-by-entity

    // Destroy-listener handle for the world we hooked into. 0 = not hooked.
    World*   m_hookedWorld   = nullptr;
    uint32_t m_listenerHandle = 0u;
};

// Adds a GuidComponent (with a fresh Guid) if absent, registers it and returns the entity's
// Guid; kInvalidGuid if `e` is not alive. Used by AttachmentRef::BindAndStamp.
Guid EnsureGuidOn(World& world, Entity e);

} // namespace ECS
