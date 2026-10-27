#pragma once

// GuidComponent / PersistentTag / AttachmentRef: entity identity and stable reference
// primitives (docs/design/entity_persistence_architecture.md).
//
//   GuidComponent  - a 128-bit stable identity, serialized in the scene. Required on any entity
//                    something else references across save/load.
//   PersistentTag  - the entity is also visited by the save-game writer, not just the scene
//                    writer.
//   AttachmentRef  - a stable cross-entity reference: the target's Guid on disk, resolved to a
//                    runtime Entity through GuidRegistry, with a cached Entity so steady-state
//                    access is one branch and one load, not a hash probe.
//
// Bone / socket attachment is FollowSocketComponent's job (FollowComponents.h), not
// AttachmentRef's.

#include "ECS/ECS.h"
#include "ECS/Guid.h"

#include <cstdint>

// GuidComponent: opt-in stable identity. Add it when something references this entity by
// anything other than its volatile Entity id.
struct GuidComponent
{
    ECS::Guid guid = ECS::kInvalidGuid;
};

// PersistentTag: the entity takes part in the save game as well as the scene save. Without it
// the entity is scene-scoped and recreated fresh when the scene loads.
struct PersistentTag : ComponentBase {};

// RuntimeSpawnedTag: marks a PersistentTag entity created at runtime (Engine.MakePersistent
// after Engine.SpawnPrefab). The save-game writer stores it in the `spawned` section, with its
// PrefabSourcePath as the recreate recipe, instead of the scene-entity patch section; the load
// path re-instantiates it when its Guid is not live. Deliberately not scene-serialized: a scene
// round trip must never fabricate runtime provenance.
struct RuntimeSpawnedTag : ComponentBase {};

// AttachmentRef: a save-safe pointer to another entity. Only the target's Guid (ownerGuid) is
// serialized; the runtime Entity / generation pair is cached so repeated access skips the
// registry. Resolve hits `cachedOwner` after the first lookup, validates its generation, and
// falls back to the registry only when the cache is stale (the entity died and its slot was
// reused, or the world reloaded).
struct AttachmentRef
{
    // ---- Serialized ----
    ECS::Guid ownerGuid = ECS::kInvalidGuid;

    // ---- Runtime cache (not serialized) ----
    // `cachedOwner` + `cachedGeneration` form an EntityHandle-style validity
    // check. A stale cache (entity destroyed, slot recycled, or world swap)
    // triggers a re-resolve through GuidRegistry.
    mutable Entity   cachedOwner      = NullEntity;
    mutable uint32_t cachedGeneration = 0u;

    bool HasGuid() const noexcept { return ownerGuid.IsValid(); }

    // Resolve the target entity for this frame. Returns NullEntity if the Guid has no registered
    // owner (target destroyed, never loaded, or never stamped with a GuidComponent). Out of line in
    // GuidRegistry.cpp to keep GuidRegistry.h out of every component-using TU.
    Entity Resolve(class World& world) const;

    // Bind to `target` by pulling the Guid off its GuidComponent. Without one the ref is cleared:
    // stamp the target first or use BindAndStamp. The editor's "pick target" entry point.
    void Bind(class World& world, Entity target);

    // Bind + auto-stamp: gives `target` a GuidComponent with a fresh Guid if it has none, then stores
    // its Guid. Returns the Guid now in `ownerGuid`. For inspector / drag-drop code where the user
    // picks any entity, ref-able or not.
    ECS::Guid BindAndStamp(class World& world, Entity target);

    // Drop the reference entirely.
    void Clear() noexcept
    {
        ownerGuid        = ECS::kInvalidGuid;
        cachedOwner      = NullEntity;
        cachedGeneration = 0u;
    }
};
