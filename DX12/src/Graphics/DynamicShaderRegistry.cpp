// DynamicShaderRegistry.cpp - see the header.

#include "Graphics/DynamicShaderRegistry.h"

#include "System/Log.h"

#include <cstring>
#include <mutex>

DynamicShaderRegistry& DynamicShaderRegistry::Get()
{
    static DynamicShaderRegistry s_registry;
    return s_registry;
}

std::string DynamicShaderRegistry::MakeKey(const char* hlslFile, const char* entry,
                                           RHI::ShaderStage stage)
{
    std::string key;
    key.reserve(std::strlen(hlslFile) + 16);
    key.append(hlslFile);
    key.push_back('|');
    key.append(entry);
    key.push_back('|');
    key.push_back(static_cast<char>('0' + static_cast<int>(stage)));
    return key;
}

uint32_t DynamicShaderRegistry::HashId(const std::string& key)
{
    uint32_t h = 2166136261u;
    for (const char c : key)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return kIdBase | (h & kIdMask);
}

uint32_t DynamicShaderRegistry::Register(const char* hlslFile, RHI::ShaderStage stage,
                                         const char* entry)
{
    if (!hlslFile || !*hlslFile) return kInvalidId;
    const char* entryName = (entry && *entry) ? entry : "main";
    std::string key = MakeKey(hlslFile, entryName, stage);

    {
        std::shared_lock<std::shared_mutex> rd(m_mutex);
        const auto it = m_keyToId.find(key);
        if (it != m_keyToId.end()) return it->second;
    }

    std::unique_lock<std::shared_mutex> wr(m_mutex);
    const auto it = m_keyToId.find(key);
    if (it != m_keyToId.end()) return it->second;   // another thread got here first

    uint32_t id = HashId(key);
    const uint32_t hashed = id;
    while (m_idToEntry.count(id) != 0)
        id = kIdBase | ((id + 1u) & kIdMask);
    if (id != hashed)
        LOG_WARNING("DynamicShaderRegistry: '%s' collides with '%s' at id %u - it takes id %u "
                    "instead (the one id that depends on registration order)",
                    key.c_str(), m_idToEntry.at(hashed)->hlslFile.c_str(), hashed, id);

    auto e = std::make_unique<Entry>();
    e->hlslFile   = hlslFile;
    e->entry      = entryName;
    e->stage      = stage;
    e->registered = true;
    e->id         = id;
    const char* slash = std::strrchr(hlslFile, '/');
    const char* back  = std::strrchr(hlslFile, '\\');
    const char* base  = (back && (!slash || back > slash)) ? back : slash;
    e->name = base ? base + 1 : hlslFile;

    m_idToEntry.emplace(id, e.get());
    m_keyToId.emplace(std::move(key), id);
    m_entries.push_back(std::move(e));

    LOG_INFO("DynamicShaderRegistry: registered '%s' (entry=%s stage=%d) -> id=%u",
             hlslFile, entryName, static_cast<int>(stage), id);
    return id;
}

uint32_t DynamicShaderRegistry::Find(const char* hlslFile, RHI::ShaderStage stage,
                                     const char* entry) const
{
    if (!hlslFile || !*hlslFile) return kInvalidId;
    const char* entryName = (entry && *entry) ? entry : "main";
    const std::string key = MakeKey(hlslFile, entryName, stage);
    std::shared_lock<std::shared_mutex> rd(m_mutex);
    const auto it = m_keyToId.find(key);
    return it != m_keyToId.end() ? it->second : kInvalidId;
}

const DynamicShaderRegistry::Entry* DynamicShaderRegistry::Lookup(uint32_t id) const
{
    std::shared_lock<std::shared_mutex> rd(m_mutex);
    const auto it = m_idToEntry.find(id);
    return it != m_idToEntry.end() ? it->second : nullptr;
}

size_t DynamicShaderRegistry::Size() const
{
    std::shared_lock<std::shared_mutex> rd(m_mutex);
    return m_entries.size();
}

void DynamicShaderRegistry::ForgetKeyMasks() const
{
    std::shared_lock<std::shared_mutex> rd(m_mutex);
    for (const auto& e : m_entries) e->keyMask.store(0, std::memory_order_release);
}

std::string DynamicShaderRegistry::Label(uint32_t id) const
{
    if (!IsDynamicId(id)) return std::to_string(id);
    if (const Entry* e = Lookup(id)) return e->name;
    return "#" + std::to_string(id);
}
