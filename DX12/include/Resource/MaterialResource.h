#pragma once

#include "Resource/Resource.h"
#include <string>
#include <unordered_map>

namespace Resource
{
    // CPU-side result of loading an .imat through ResourceManager (MaterialLoader):
    // the key=value pairs of its text payload, looked up by key. Resource::LoadMaterial
    // reads .imat files itself and does not use this.
    class MaterialResource : public Resource
    {
    public:
        MaterialResource() = default;

        void Set(const std::string& key, const std::string& value) { m_entries[key] = value; }

        // Returns nullptr if the key is absent.
        const std::string* Get(const std::string& key) const
        {
            auto it = m_entries.find(key);
            return it != m_entries.end() ? &it->second : nullptr;
        }

        const std::unordered_map<std::string, std::string>& GetEntries() const { return m_entries; }

    private:
        std::unordered_map<std::string, std::string> m_entries;
    };
}
