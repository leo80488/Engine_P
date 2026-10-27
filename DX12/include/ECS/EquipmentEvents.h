#pragma once

// Events published by the equipment Command API (Command::EquipToSocket /
// Command::UnequipItem). Separate from FollowEvents.h so gameplay subscribers do not
// pull in engine-internal follow bookkeeping. Payloads carry EntityHandle, not raw
// Entity, so a subscriber can drop a stale notification in a later frame (e.g. an
// unequip queued and the item destroyed before dispatch).

#include "ECS/ECS.h"

#include <cstdint>

// WeaponEquippedEvent - Command::EquipToSocket succeeded. socketIndex indexes the
// character's SocketComponent::sockets at publish time; it may be stale by the time
// a subscriber resolves it (e.g. a socket removed mid-frame).
struct WeaponEquippedEvent
{
    EntityHandle character;
    EntityHandle item;
    uint32_t     socketIndex;
};

// WeaponUnequippedEvent - Command::UnequipItem succeeded, or EquipToSocket
// auto-detached a previously equipped item before attaching it elsewhere.
// character may already be stale if the unequip was triggered by its death.
struct WeaponUnequippedEvent
{
    EntityHandle character;
    EntityHandle item;
};
