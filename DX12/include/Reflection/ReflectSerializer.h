#pragma once

// ReflectSerializer - a component's "  Tag: k=v k=v" line written and read from its reflection
// descriptor (Reflection/Reflect.h): every saved data field (not Runtime) in declaration order, by
// its key (a split key writes one scalar per component), legacy keys accepted on read. A component
// registers it instead of a hand-written serializer (RegisterReflectedSerializer); the existing
// hand-written ones are replaced type by type only where tests/unit/reflect_serializer_test.py
// proves the generic line byte-identical on the scene corpus.

#include "Reflection/Property.h"
#include "Reflection/Reflect.h"
#include "Resource/ComponentSerializers.h"   // KVMap, ComponentSerializer(Registry)

#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Reflect
{
    // How numbers are spelled: "%.4f" (most serializers), or std::ostream's default (six significant
    // digits; the Canvas UI components).
    enum class NumberStyle : uint8_t { Fixed4, Stream };

    // The line, newline included.
    std::string SerializeLine(const char* tag, const TypeDescriptor& desc, const void* obj, NumberStyle numbers);

    // Reads the keys @p kv holds into @p obj (a key's legacy names too); an absent key keeps the
    // object's value, so start from the type's default. False with @p error naming a malformed value.
    bool DeserializeKV(const TypeDescriptor& desc, void* obj, const KVMap& kv, std::string& error);

    // Writes the (key, text) pairs into @p obj through WriteProperty with @p origin: the keys and the
    // texts as the line carries them (a split key's part, a percent-encoded string). All or nothing -
    // the pairs go onto a copy, which replaces @p obj only when every one was accepted, so a refusal
    // leaves the object as it was. False with the reason in @p error: an unknown key, a text that is
    // not a value of the field, a value WriteProperty refuses (past its hard range, a read-only field).
    bool WriteKeys(const TypeDescriptor& desc, void* obj, const std::vector<std::pair<std::string, std::string>>& pairs,
                   WriteOrigin origin, std::string& error);
}

// A serializer entry for T made from its descriptor: has / serialize / deserialize (deserialize starts
// from T{} and replaces the component, as a hand-written deserializer does), and the descriptor with a
// way to reach the live component, which is what Entity.set writes through.
template <class T>
ComponentSerializer ReflectedSerializer(const char* tag, Reflect::NumberStyle numbers = Reflect::NumberStyle::Fixed4,
                                        ComponentFlag flags = ComponentFlag::None)
{
    ComponentSerializer cs;
    cs.tag = tag;
    cs.flags = flags;
    cs.descriptor = &Reflect::Describe<T>();
    cs.fetch = [](World& w, Entity e) -> void* { return w.GetComponent<T>(e); };
    cs.has = [](World& w, Entity e) { return w.GetComponent<T>(e) != nullptr; };
    cs.serialize = [tag, numbers](World& w, Entity e, std::ostringstream& ss) {
        if (const T* c = w.GetComponent<T>(e)) ss << Reflect::SerializeLine(tag, Reflect::Describe<T>(), c, numbers);
    };
    cs.deserialize = [](World& w, Entity e, const KVMap& kv, Resource::AssetManager*) {
        T c{};
        std::string error;
        Reflect::DeserializeKV(Reflect::Describe<T>(), &c, kv, error);
        w.AddComponent<T>(e, std::move(c));
    };
    return cs;
}

// Registers T's reflected serializer under @p tag. This one line is a new component's whole cost: it
// saves and loads (scene, prefab, save, undo), Entity.set writes it when @p flags says LuaWritable, and
// the registry check (FindRegistryDrift) finds its descriptor serialized.
template <class T>
void RegisterReflectedSerializer(ComponentSerializerRegistry& reg, const char* tag,
                                 Reflect::NumberStyle numbers = Reflect::NumberStyle::Fixed4,
                                 ComponentFlag flags = ComponentFlag::None)
{
    reg.Register(std::type_index(typeid(T)), ReflectedSerializer<T>(tag, numbers, flags));
}
