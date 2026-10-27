#include "Gameplay/GameplayTags.h"

#include <algorithm>
#include <charconv>
#include <mutex>

namespace Gameplay
{
namespace
{
    constexpr size_t kMaxNameLength = 63;
    constexpr size_t kMaxDepth      = 8;

    // 32-bit FNV-1a; 0 means "no tag", so a name that hashes to it takes the next id.
    uint32_t HashName(std::string_view name)
    {
        uint32_t h = 2166136261u;
        for (unsigned char c : name)
        {
            h ^= c;
            h *= 16777619u;
        }
        return h ? h : 1u;
    }

    std::string_view ParentName(std::string_view name)
    {
        const size_t dot = name.rfind('.');
        return dot == std::string_view::npos ? std::string_view{} : name.substr(0, dot);
    }

    std::string_view Trim(std::string_view s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        return s;
    }
}

// ==== TagTable ====

TagTable& TagTable::Get()
{
    static TagTable s_instance;
    return s_instance;
}

bool TagTable::ValidName(std::string_view name, std::string* error)
{
    auto fail = [&](const char* why)
    {
        if (error) *error = "tag '" + std::string(name) + "': " + why;
        return false;
    };
    if (name.empty()) return fail("is empty");
    if (name.size() > kMaxNameLength) return fail("is longer than 63 characters");
    size_t depth = 1, segment = 0;
    for (char c : name)
    {
        if (c == '.')
        {
            if (segment == 0) return fail("has an empty segment");
            ++depth;
            segment = 0;
            continue;
        }
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return fail("may hold only letters, digits, '_' and '.' between segments");
        ++segment;
    }
    if (segment == 0) return fail("ends with a '.'");
    if (depth > kMaxDepth) return fail("has more than 8 segments");
    return true;
}

Tag TagTable::InternLocked(std::string_view name, std::string* error)
{
    const uint32_t id = HashName(name);
    auto it = m_nodes.find(id);
    if (it != m_nodes.end())
    {
        if (it->second.name == name) return Tag{ id };
        if (error) *error = "tag '" + std::string(name) + "' has the same hash as '" + it->second.name + "'";
        return Tag{};
    }
    Node node;
    node.name = std::string(name);
    const std::string_view parentName = ParentName(name);
    if (!parentName.empty())
    {
        node.parent = InternLocked(parentName, error);
        if (!node.parent.IsValid()) return Tag{};
    }
    m_nodes.emplace(id, std::move(node));
    return Tag{ id };
}

Tag TagTable::Intern(std::string_view name, std::string* error)
{
    if (!ValidName(name, error)) return Tag{};
    {
        std::shared_lock read(m_lock);
        auto it = m_nodes.find(HashName(name));
        if (it != m_nodes.end() && it->second.name == name) return Tag{ it->first };
    }
    std::unique_lock write(m_lock);
    return InternLocked(name, error);
}

Tag TagTable::Find(std::string_view name) const
{
    std::shared_lock read(m_lock);
    auto it = m_nodes.find(HashName(name));
    return it != m_nodes.end() && it->second.name == name ? Tag{ it->first } : Tag{};
}

std::string TagTable::Name(Tag tag) const
{
    std::shared_lock read(m_lock);
    auto it = m_nodes.find(tag.id);
    return it == m_nodes.end() ? std::string{} : it->second.name;
}

Tag TagTable::Parent(Tag tag) const
{
    std::shared_lock read(m_lock);
    auto it = m_nodes.find(tag.id);
    return it == m_nodes.end() ? Tag{} : it->second.parent;
}

bool TagTable::IsSelfOrBelow(Tag tag, Tag ancestor) const
{
    if (!ancestor.IsValid()) return false;
    std::shared_lock read(m_lock);
    for (Tag cursor = tag; cursor.IsValid();)
    {
        if (cursor == ancestor) return true;
        auto it = m_nodes.find(cursor.id);
        if (it == m_nodes.end()) return false;
        cursor = it->second.parent;
    }
    return false;
}

size_t TagTable::Size() const
{
    std::shared_lock read(m_lock);
    return m_nodes.size();
}

bool ParseTagList(std::string_view text, std::vector<Tag>& out, std::string* error)
{
    while (!text.empty())
    {
        const size_t comma = text.find(',');
        const std::string_view item = Trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (item.empty()) continue;
        const Tag tag = TagTable::Get().Intern(item, error);
        if (!tag.IsValid()) return false;
        out.push_back(tag);
    }
    return true;
}

// ==== TagCounts ====

TagCounts::Entry* TagCounts::Find(uint32_t id)
{
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), id,
                               [](const Entry& e, uint32_t key) { return e.id < key; });
    return it != m_entries.end() && it->id == id ? &*it : nullptr;
}

const TagCounts::Entry* TagCounts::Find(uint32_t id) const
{
    return const_cast<TagCounts*>(this)->Find(id);
}

TagCounts::Entry& TagCounts::Obtain(uint32_t id)
{
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), id,
                               [](const Entry& e, uint32_t key) { return e.id < key; });
    if (it == m_entries.end() || it->id != id)
    {
        Entry fresh;
        fresh.id = id;
        it = m_entries.insert(it, fresh);
    }
    return *it;
}

// Moves the tag's own count and every ancestor's "below" count by @p delta; an entry both of whose
// counts reach zero is dropped.
void TagCounts::Adjust(Tag tag, int delta)
{
    const TagTable& table = TagTable::Get();
    Obtain(tag.id).own += delta;
    for (Tag up = table.Parent(tag); up.IsValid(); up = table.Parent(up))
        Obtain(up.id).below += delta;

    for (Tag at = tag; at.IsValid(); at = table.Parent(at))
    {
        Entry* e = Find(at.id);
        if (e && e->own == 0 && e->below == 0)
            m_entries.erase(m_entries.begin() + (e - m_entries.data()));
    }
}

int TagCounts::Add(Tag tag, int n)
{
    if (!tag.IsValid() || n <= 0) return Count(tag);
    Adjust(tag, n);
    return Count(tag);
}

int TagCounts::Remove(Tag tag, int n)
{
    if (!tag.IsValid() || n <= 0) return Count(tag);
    const int held = Count(tag);
    if (held > 0) Adjust(tag, -std::min(n, held));
    return Count(tag);
}

void TagCounts::Clear(Tag tag)
{
    const int held = Count(tag);
    if (held > 0) Adjust(tag, -held);
}

int TagCounts::Count(Tag tag) const
{
    const Entry* e = Find(tag.id);
    return e ? e->own : 0;
}

bool TagCounts::Matches(Tag tag) const
{
    const Entry* e = Find(tag.id);
    return e && (e->own > 0 || e->below > 0);
}

bool TagCounts::MatchesAll(const std::vector<Tag>& tags) const
{
    for (Tag tag : tags)
        if (!Matches(tag)) return false;
    return true;
}

bool TagCounts::MatchesAny(const std::vector<Tag>& tags) const
{
    for (Tag tag : tags)
        if (Matches(tag)) return true;
    return false;
}

std::string TagCounts::ToText() const
{
    std::vector<std::pair<std::string, int>> rows;
    ForEach([&](Tag tag, int count) { rows.emplace_back(TagTable::Get().Name(tag), count); });
    std::sort(rows.begin(), rows.end());
    std::string out;
    for (const auto& [name, count] : rows)
    {
        if (!out.empty()) out += ',';
        out += name + ":" + std::to_string(count);
    }
    return out;
}

bool TagCounts::FromText(std::string_view text, std::string* error)
{
    ClearAll();
    while (!text.empty())
    {
        const size_t comma = text.find(',');
        const std::string_view item = Trim(text.substr(0, comma));
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        if (item.empty()) continue;
        const size_t colon = item.rfind(':');
        const std::string_view name = Trim(colon == std::string_view::npos ? item : item.substr(0, colon));
        int count = 1;
        if (colon != std::string_view::npos)
        {
            const std::string_view digits = Trim(item.substr(colon + 1));
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), count);
            if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || count < 1)
            {
                if (error) *error = "tag entry '" + std::string(item) + "': the count after ':' is not a positive integer";
                return false;
            }
        }
        const Tag tag = TagTable::Get().Intern(name, error);
        if (!tag.IsValid()) return false;
        Add(tag, count);
    }
    return true;
}

// ==== The engine's own tags ====

namespace Std
{
    Tag IFrame()
    {
        static const Tag s_tag = TagTable::Get().Intern("State.IFrame");
        return s_tag;
    }

    Tag Invincible()
    {
        static const Tag s_tag = TagTable::Get().Intern("State.Invincible");
        return s_tag;
    }
}
}
