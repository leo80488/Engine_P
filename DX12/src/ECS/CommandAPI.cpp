#include "ECS/CommandAPI.h"

#include "ECS/AnimationComponents.h"   // SocketComponent
#include "ECS/FollowComponents.h"
#include "ECS/EquipmentEvents.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <cstring>
#include <limits>

namespace Command
{

bool EquipToSocket(World& world, Entity character, Entity item, const char* socketName)
{
    if (!world.IsAlive(character) || !world.IsAlive(item) || !socketName || !*socketName)
    {
        LOG_WARNING("Command::EquipToSocket: invalid args (char=%u item=%u name=%s)",
                    character, item, socketName ? socketName : "<null>");
        return false;
    }

    const SocketComponent* sc = world.GetComponent<SocketComponent>(character);
    if (!sc)
    {
        LOG_WARNING("Command::EquipToSocket: character %u has no SocketComponent", character);
        return false;
    }

    uint32_t foundIndex = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < sc->count; ++i)
    {
        if (std::strcmp(sc->sockets[i].name, socketName) == 0)
        {
            foundIndex = i;
            break;
        }
    }
    if (foundIndex == std::numeric_limits<uint32_t>::max())
    {
        LOG_WARNING("Command::EquipToSocket: character %u has no socket named '%s'",
                    character, socketName);
        return false;
    }

    // An item already equipped publishes Unequipped for its old slot first, so
    // subscribers get paired events without tracking prior locations.
    if (FollowSocketComponent* prior = world.GetComponent<FollowSocketComponent>(item))
    {
        const Entity priorChar = prior->target.Resolve(world);
        EventBus::Get().Publish(WeaponUnequippedEvent{
            priorChar != NullEntity ? world.MakeHandle(priorChar) : NullEntityHandle,
            world.MakeHandle(item) });
    }

    // BindAndStamp (not Bind): the character may never have needed a Guid before,
    // and a plain Bind would silently clear the ref on an unstamped target.
    FollowSocketComponent fsc;
    fsc.target.BindAndStamp(world, character);
    fsc.socketNameHash  = SocketComponent::HashName(socketName);
    fsc.socketIndex     = foundIndex;
    fsc.cachedSocketGen = sc->generation;
    world.AddComponent(item, fsc);

    EventBus::Get().Publish(WeaponEquippedEvent{
        world.MakeHandle(character), world.MakeHandle(item), foundIndex });

    return true;
}

bool UnequipItem(World& world, Entity item)
{
    if (!world.IsAlive(item)) return false;

    FollowSocketComponent* fsc = world.GetComponent<FollowSocketComponent>(item);
    if (!fsc) return false;

    // Resolve the character before removal: the component's memory goes away
    // in RemoveComponent.
    const Entity character = fsc->target.Resolve(world);
    const EntityHandle characterHandle =
        (character != NullEntity) ? world.MakeHandle(character) : NullEntityHandle;

    world.RemoveComponent<FollowSocketComponent>(item);

    EventBus::Get().Publish(WeaponUnequippedEvent{
        characterHandle, world.MakeHandle(item) });

    return true;
}

bool AttachToEntity(World& world, Entity follower, Entity target)
{
    if (!world.IsAlive(follower) || !world.IsAlive(target))
    {
        LOG_WARNING("Command::AttachToEntity: invalid args (follower=%u target=%u)",
                    follower, target);
        return false;
    }

    FollowEntityComponent fec;
    fec.target.BindAndStamp(world, target);
    world.AddComponent(follower, fec);
    return true;
}

bool DetachFromEntity(World& world, Entity follower)
{
    if (!world.IsAlive(follower)) return false;
    if (!world.HasComponent<FollowEntityComponent>(follower)) return false;
    world.RemoveComponent<FollowEntityComponent>(follower);
    return true;
}

} // namespace Command
