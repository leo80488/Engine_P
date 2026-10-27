#include "Reflection/ReflectRegistry.h"

#include <unordered_map>

namespace Reflect
{
    namespace
    {
        std::unordered_map<std::type_index, RuntimeEntry>& Map()
        {
            static std::unordered_map<std::type_index, RuntimeEntry> s_entries;
            return s_entries;
        }

        // Tags named before their descriptor was registered.
        std::unordered_map<std::type_index, const char*>& PendingTags()
        {
            static std::unordered_map<std::type_index, const char*> s_tags;
            return s_tags;
        }
    }

    void RegisterRuntime(std::type_index type, const TypeDescriptor* desc, const char* label)
    {
        RuntimeEntry& entry = Map()[type];
        entry.desc  = desc;
        entry.label = label ? label : "";
        if (!entry.tag)
        {
            const auto it = PendingTags().find(type);
            if (it != PendingTags().end()) entry.tag = it->second;
        }
    }

    void SetRuntimeTag(std::type_index type, const char* tag)
    {
        PendingTags()[type] = tag;
        const auto it = Map().find(type);
        if (it != Map().end()) it->second.tag = tag;
    }

    const RuntimeEntry* FindRuntimeByTag(std::string_view tag)
    {
        for (const auto& [type, entry] : Map())
            if (entry.tag && tag == entry.tag) return &entry;
        return nullptr;
    }

    const RuntimeEntry* FindRuntime(std::type_index type)
    {
        const auto it = Map().find(type);
        return it == Map().end() ? nullptr : &it->second;
    }

    std::size_t RuntimeCount()
    {
        return Map().size();
    }

    std::vector<RuntimeEntry> AllRuntime()
    {
        std::vector<RuntimeEntry> out;
        out.reserve(Map().size());
        for (const auto& [type, entry] : Map()) out.push_back(entry);
        return out;
    }
}
