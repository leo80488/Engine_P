#pragma once

// Command API: the script -> engine translation layer. Scripts do not touch
// these components directly; each function validates, mutates the components
// and publishes the matching event, so the side effects (logging, event
// dispatch) live in one place.
// The Lua counterparts are the Engine.* table bound in
// ScriptSystem::RegisterBindings (Engine.EquipToSocket, Engine.UnequipItem, ...).
// Every function takes an explicit World& (testable, no hidden global state);
// pass the World the frame pipeline uses.

#include "ECS/ECS.h"

namespace Command
{
    // Equip `item` to the socket named `socketName` on `character`.
    // On success the item gains FollowSocketComponent { target = character,
    // socketIndex } and WeaponEquippedEvent is published on the global EventBus; an
    // item already equipped elsewhere first publishes WeaponUnequippedEvent for its
    // previous slot, so subscribers see paired transitions.
    // Returns false (and logs a warning) when either entity is dead, socketName is
    // null or empty, or the character has no SocketComponent or no socket of that
    // name.
    bool EquipToSocket(World& world, Entity character, Entity item, const char* socketName);

    // Remove item's FollowSocketComponent and publish WeaponUnequippedEvent.
    // Returns false if the item had no FollowSocketComponent to begin with.
    bool UnequipItem(World& world, Entity item);

    // Generic entity follow: adds FollowEntityComponent so Lua need not know its
    // internals. Returns false when either entity is dead.
    bool AttachToEntity(World& world, Entity follower, Entity target);

    // Remove follower's FollowEntityComponent; false when it had none.
    bool DetachFromEntity(World& world, Entity follower);
}
