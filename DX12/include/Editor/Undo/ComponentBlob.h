#pragma once

// ComponentBlob - serialize / deserialize one ECS component to / from a text
// blob through the ComponentSerializerRegistry: the same round-trip
// EditorLayer's CloneEntityRecursive uses for duplicate, so undo actions can
// snapshot and restore a component's whole state without knowing its concrete
// type. The blob is the serializer's "  Tag: k=v k=v\n" line.

#include "Resource/ComponentSerializers.h"   // ComponentSerializerRegistry, KVMap

#include <sstream>
#include <string>
#include <typeindex>

namespace Editor::Undo
{
inline const ComponentSerializer* FindSerializer(ComponentSerializerRegistry& reg,
                                                 std::type_index type)
{
    const auto& all = reg.All();
    const auto  it  = all.find(type);
    return it != all.end() ? &it->second : nullptr;
}

// True when the type has a serializer that can both serialize and deserialize,
// i.e. a whole-component blob swap is possible for it.
inline bool HasSerializer(ComponentSerializerRegistry& reg, std::type_index type)
{
    const ComponentSerializer* s = FindSerializer(reg, type);
    return s && s->serialize && s->deserialize;
}

// Serialize the component to its "  Tag: k=v ...\n" line. Returns "" if the
// entity lacks the component or the type has no serializer.
inline std::string SerializeComponentBlob(ComponentSerializerRegistry& reg,
                                          World& w, Entity e, std::type_index type)
{
    const ComponentSerializer* s = FindSerializer(reg, type);
    if (!s || !s->serialize) return {};
    if (s->has && !s->has(w, e)) return {};
    std::ostringstream ss;
    s->serialize(w, e, ss);
    return ss.str();
}

// Parse a blob produced by SerializeComponentBlob and apply it to the entity
// (creates / updates the component). The blob may hold MULTIPLE lines: some
// serializers (e.g. ScriptComponent) emit one "  Tag: ...\n" line per element
// and rely on deserialize being called ONCE PER LINE, each appending a slot
// (as SceneSerializer dispatches). Do not parse the whole blob as one
// key-value map: a multi-line component would collapse to a single slot.
// Per line: find ':', tokenize the remainder on whitespace, split on '='.
// Returns true if at least one line was dispatched.
inline bool DeserializeComponentBlob(ComponentSerializerRegistry& reg,
                                     World& w, Entity e, std::type_index type,
                                     const std::string& blob,
                                     Resource::AssetManager* assets)
{
    const ComponentSerializer* s = FindSerializer(reg, type);
    if (!s || !s->deserialize || blob.empty()) return false;

    std::istringstream lines(blob);
    std::string        line;
    bool               any = false;
    while (std::getline(lines, line))
    {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::size_t start = colon + 1;
        if (start < line.size() && line[start] == ' ') ++start;

        KVMap kv;
        std::istringstream tok(line.substr(start));
        std::string t;
        while (tok >> t)
        {
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            kv[t.substr(0, eq)] = t.substr(eq + 1);
        }
        s->deserialize(w, e, kv, assets);
        any = true;
    }
    return any;
}

} // namespace Editor::Undo
