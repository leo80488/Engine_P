#pragma once

// ComponentSerializers: per-type serialize / deserialize of ECS components as key=value text.
// Each serializable type registers a tag (e.g. "LightData"), has(), serialize() (one line of
// key=value pairs) and deserialize() (a KVMap -> creates or updates the component). Components are
// INLINE (data stored in the scene / prefab: LightData, Billboard, Camera ...), REF (a file path
// the loader resolves: MaterialRef, MeshRef ...) or DERIVED (never serialized, rebuilt at runtime:
// GlobalTransform, Parent ...).

#include "ECS/ECS.h"
#include <string>
#include <sstream>
#include <unordered_map>
#include <functional>
#include <typeindex>
#include <vector>

class World;

namespace Resource
{
    class AssetManager;
}

namespace Reflect
{
    struct TypeDescriptor;
}

// Key-value pairs parsed from one serialized component block.
using KVMap = std::unordered_map<std::string, std::string>;

// What a component's registration declares beyond its three functions.
enum class ComponentFlag : uint32_t
{
    None        = 0,
    // Entity.set may change it. A component with a descriptor (RegisterReflectedSerializer) is
    // written field by field through Reflect::WriteProperty, atomically; one with a hand-written
    // serializer is rewritten through its deserializer, ComponentWrite carrying its runtime state.
    LuaWritable = 1u << 0,
};

constexpr ComponentFlag operator|(ComponentFlag a, ComponentFlag b)
{
    return static_cast<ComponentFlag>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool HasFlag(ComponentFlag set, ComponentFlag flag)
{
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(flag)) != 0;
}

// Per-component-type serialization entry.
struct ComponentSerializer
{
    std::string tag;  // e.g. "LightData", "Billboard", "Camera"

    // Returns true if the entity has this component.
    std::function<bool(World&, Entity)> has;

    // Write this component of entity @p e to the stream as one line:
    // "  <tag>: key=val key=val ...\n".
    std::function<void(World&, Entity, std::ostringstream&)> serialize;

    // Read the key-value pairs and create / update the component on entity @p e.
    std::function<void(World&, Entity, const KVMap&, Resource::AssetManager*)> deserialize;

    ComponentFlag flags = ComponentFlag::None;

    // A component described by a reflection descriptor (ReflectedSerializer<T>): the descriptor and a
    // way to reach the live component, so a writer can address its fields. Null otherwise.
    const Reflect::TypeDescriptor* descriptor = nullptr;
    std::function<void*(World&, Entity)> fetch;
};

// Registry of all serializable component types.
class ComponentSerializerRegistry
{
public:
    void Register(std::type_index typeIdx, ComponentSerializer entry)
    {
        m_byType[typeIdx] = std::move(entry);
        m_byTag[m_byType[typeIdx].tag] = &m_byType[typeIdx];
    }

    const std::unordered_map<std::type_index, ComponentSerializer>& All() const { return m_byType; }

    const ComponentSerializer* FindByTag(const std::string& tag) const
    {
        auto it = m_byTag.find(tag);
        return (it != m_byTag.end()) ? it->second : nullptr;
    }

    // Adds @p flags to the serializer tagged @p tag. False when no serializer has that tag.
    bool AddFlags(const std::string& tag, ComponentFlag flags)
    {
        auto it = m_byTag.find(tag);
        if (it == m_byTag.end()) return false;
        it->second->flags = it->second->flags | flags;
        return true;
    }

private:
    std::unordered_map<std::type_index, ComponentSerializer> m_byType;
    std::unordered_map<std::string, ComponentSerializer*>    m_byTag;
};

// Register all engine component serializers.
void RegisterAllComponentSerializers(ComponentSerializerRegistry& reg);

// The gameplay components' serializers (Gameplay/GameplayTagSerializer.cpp).
void RegisterGameplayComponentSerializers(ComponentSerializerRegistry& reg);
// The Attributes line (Gameplay/AttributeSerializer.cpp); RegisterGameplayComponentSerializers adds it.
void RegisterAttributeSerializer(ComponentSerializerRegistry& reg);

// Flags the hand-written serializers' tags carry (ComponentFlags.cpp); a reflected component states
// its own where it registers.
void ApplyHandWrittenComponentFlags(ComponentSerializerRegistry& reg);

// Drift between this registry and the reflection descriptors, two independent lists of "all
// components": one line per described type that no serializer writes and no row of kNotSerialized
// (ComponentRegistryCheck.cpp) excuses, and per excuse that no longer applies. A component with an
// inspector but no serializer silently loses its data in .iscene / .prefab save, SaveGame's
// PersistentTag snapshot and the editor's Undo / Redo ComponentBlob, which all go through this registry.
// Needs the descriptors registered (GetComponentRegistry does that first).
std::vector<std::string> FindRegistryDrift(const ComponentSerializerRegistry& reg);

// Reports every FindRegistryDrift line through ENGINE_CHECK (fires in Release too). GetComponentRegistry
// calls it once the registry is complete.
void ValidateComponentRegistries(const ComponentSerializerRegistry& reg);

// Get the global singleton registry (lazy-initialized on first call).
ComponentSerializerRegistry& GetComponentRegistry();

// One component line of a scene, prefab or save onto @p e: its serializer reads @p kv. A @p tag
// no serializer reads is logged once per tag (naming the module that owns it, when one is
// declared) and kept verbatim on @p e (ECS/UnresolvedComponents.h), so the next save writes it
// back; a component whose module the project leaves off still loads, and is logged once as
// inert. The loaders' own records (SceneRef, PrefabFollowSocket, ...) are skipped. Main thread.
void ApplyComponentLine(const ComponentSerializerRegistry& reg, World& world, Entity e, const std::string& tag,
                        const KVMap& kv, Resource::AssetManager* assets);
