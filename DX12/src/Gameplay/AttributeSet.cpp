// AttributeSet.cpp: the attribute container, its profile defaults from a data table, its text form and the
// writes that announce a change (Gameplay/AttributeSet.h).

#include "Gameplay/AttributeSet.h"

#include "Gameplay/DataTable.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace
{
    constexpr size_t kMaxNameLength = 48;

    bool NameLess(const AttributeSetComponent::Entry& entry, std::string_view name)
    {
        return std::string_view(entry.name) < name;
    }

    // The profile table's column for @p name, or null.
    const Gameplay::Data::Column* ProfileColumn(const AttributeSetComponent& set, std::string_view name,
                                                std::shared_ptr<const Gameplay::Data::Table>& keep)
    {
        std::string path, row;
        if (!Gameplay::Attr::SplitProfile(set.profile, path, row)) return nullptr;
        keep = Gameplay::Data::Tables::Get().Find(path);
        return keep ? keep->schema.Find(std::string(name)) : nullptr;
    }

    float AsFloat(const Gameplay::Data::Json& value)
    {
        if (value.is_boolean()) return value.get<bool>() ? 1.f : 0.f;
        return value.is_number() ? value.get<float>() : 0.f;
    }

    // Announces what moved between @p before and the entity's set now.
    void AnnounceMoves(World& world, Entity entity, const std::vector<AttributeSetComponent::Entry>& before)
    {
        const AttributeSetComponent* set = world.GetComponent<AttributeSetComponent>(entity);
        if (!set) return;
        for (const AttributeSetComponent::Entry& now : set->entries)
        {
            float previous = 0.f;
            for (const AttributeSetComponent::Entry& old : before)
                if (old.name == now.name) { previous = old.current; break; }
            if (previous == now.current) continue;
            EventBus::Get().Publish(AttributeChangedEvent{ entity, now.name, previous, now.current, now.base });
        }
    }
}

// ==== The container ====
const AttributeSetComponent::Entry* AttributeSetComponent::Find(std::string_view name) const
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), name, NameLess);
    return it != entries.end() && it->name == name ? &*it : nullptr;
}

AttributeSetComponent::Entry* AttributeSetComponent::Find(std::string_view name)
{
    return const_cast<Entry*>(static_cast<const AttributeSetComponent*>(this)->Find(name));
}

AttributeSetComponent::Entry& AttributeSetComponent::Obtain(std::string_view name)
{
    auto it = std::lower_bound(entries.begin(), entries.end(), name, NameLess);
    if (it == entries.end() || it->name != name)
        it = entries.insert(it, Entry{ std::string(name), 0.f, 0.f });
    return *it;
}

bool AttributeSetComponent::Remove(std::string_view name)
{
    const auto it = std::lower_bound(entries.begin(), entries.end(), name, NameLess);
    if (it == entries.end() || it->name != name) return false;
    entries.erase(it);
    return true;
}

float AttributeSetComponent::Get(std::string_view name, float fallback) const
{
    const Entry* entry = Find(name);
    return entry ? entry->current : fallback;
}

namespace Gameplay::Attr
{
// ==== Names and profiles ====
bool ValidName(std::string_view name, std::string* error)
{
    const auto fail = [&](const char* why)
    {
        if (error) *error = std::string("bad_attribute: '") + std::string(name) + "' " + why;
        return false;
    };
    if (name.empty()) return fail("is empty");
    if (name.size() > kMaxNameLength) return fail("is longer than 48 characters");
    for (size_t i = 0; i < name.size(); ++i)
    {
        const char c = name[i];
        const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!(letter || (i > 0 && c >= '0' && c <= '9'))) return fail("must be letters, digits and '_' and not start with a digit");
    }
    return true;
}

std::string ResistName(std::string_view damageType)
{
    return "Resist_" + std::string(damageType);
}

bool SplitProfile(std::string_view ref, std::string& path, std::string& row)
{
    const size_t hash = ref.rfind('#');
    if (hash == std::string_view::npos || hash == 0 || hash + 1 >= ref.size()) return false;
    path.assign(ref.substr(0, hash));
    row.assign(ref.substr(hash + 1));
    return true;
}

bool ApplyProfile(AttributeSetComponent& set, std::string_view ref, std::string& error)
{
    std::string path, rowId;
    if (!SplitProfile(ref, path, rowId))
    {
        error = "bad_profile: '" + std::string(ref) + "' is not \"path/table.dtable#row\"";
        return false;
    }
    std::string loadError;
    Data::Tables::Get().Load(path, loadError);
    const std::shared_ptr<const Data::Table> table = Data::Tables::Get().Find(path);
    if (!table)
    {
        error = "bad_profile: table '" + path + "' is not loaded: " + loadError;
        return false;
    }
    const auto row = table->rows.find(rowId);
    if (row == table->rows.end())
    {
        error = "bad_profile: table '" + path + "' has no row '" + rowId + "'";
        return false;
    }

    std::vector<AttributeSetComponent::Entry> fresh;
    for (auto it = row->second.begin(); it != row->second.end(); ++it)
    {
        if (!it.value().is_number() && !it.value().is_boolean()) continue;
        const float value = AsFloat(it.value());
        fresh.push_back({ it.key(), value, value });
    }
    std::sort(fresh.begin(), fresh.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    // A profile that names MaxHealth and not Health starts the entity at full health.
    const auto maxHealth = std::find_if(fresh.begin(), fresh.end(), [](const auto& e) { return e.name == kMaxHealth; });
    const bool hasHealth = std::any_of(fresh.begin(), fresh.end(), [](const auto& e) { return e.name == kHealth; });
    if (maxHealth != fresh.end() && !hasHealth)
    {
        const float full = maxHealth->current;
        fresh.insert(std::lower_bound(fresh.begin(), fresh.end(), std::string_view(kHealth), NameLess),
                     AttributeSetComponent::Entry{ kHealth, full, full });
    }
    set.entries = std::move(fresh);
    set.profile.assign(ref);
    set.recentHits.clear();
    return true;
}

// ==== Clamping ====
float Clamp(const AttributeSetComponent& set, std::string_view name, float value)
{
    if (name == kHealth)
    {
        const AttributeSetComponent::Entry* max = set.Find(kMaxHealth);
        return max ? std::clamp(value, 0.f, std::max(0.f, max->current)) : std::max(0.f, value);
    }
    std::shared_ptr<const Data::Table> keep;
    if (const Data::Column* column = ProfileColumn(set, name, keep))
        return static_cast<float>(std::clamp<double>(value, column->min, column->max));
    return value;
}

// ==== Text form ====
std::string ToText(const AttributeSetComponent& set)
{
    std::string out;
    char buf[96];
    for (const AttributeSetComponent::Entry& e : set.entries)
    {
        if (!out.empty()) out += ',';
        std::snprintf(buf, sizeof(buf), ":%.7g:%.7g", e.base, e.current);
        out += e.name;
        out += buf;
    }
    return out;
}

bool FromText(AttributeSetComponent& set, std::string_view text, std::string* error)
{
    set.entries.clear();
    size_t at = 0;
    while (at < text.size())
    {
        size_t end = text.find(',', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view item = text.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;

        const size_t c1 = item.find(':');
        const size_t c2 = c1 == std::string_view::npos ? c1 : item.find(':', c1 + 1);
        const auto fail = [&](const char* why)
        {
            if (error) *error = std::string("entry '") + std::string(item) + "' " + why;
            return false;
        };
        if (c2 == std::string_view::npos) return fail("is not name:base:current");
        const std::string_view name = item.substr(0, c1);
        std::string nameError;
        if (!ValidName(name, &nameError)) { if (error) *error = nameError; return false; }

        float values[2] = { 0.f, 0.f };
        const std::string_view parts[2] = { item.substr(c1 + 1, c2 - c1 - 1), item.substr(c2 + 1) };
        for (int i = 0; i < 2; ++i)
        {
            const auto [ptr, ec] = std::from_chars(parts[i].data(), parts[i].data() + parts[i].size(), values[i]);
            if (ec != std::errc() || ptr != parts[i].data() + parts[i].size() || !std::isfinite(values[i]))
                return fail("has a value that is not a finite number");
        }
        AttributeSetComponent::Entry& entry = set.Obtain(name);
        entry.base    = values[0];
        entry.current = values[1];
    }
    return true;
}

// ==== Writes ====
WriteResult Write(World& world, Entity entity, std::string_view name, float value, Target target, bool announceDeath)
{
    WriteResult result;
    if (!ValidName(name, &result.error)) return result;
    if (!std::isfinite(value)) { result.error = "not_finite: the value must be a finite number"; return result; }
    if (!world.IsAlive(entity)) { result.error = "stale_handle: the entity is not alive"; return result; }

    AttributeSetComponent* set = world.GetComponent<AttributeSetComponent>(entity);
    if (!set)
    {
        world.AddComponent<AttributeSetComponent>(entity, AttributeSetComponent{});
        set = world.GetComponent<AttributeSetComponent>(entity);
    }

    const float clamped = Clamp(*set, name, value);
    AttributeSetComponent::Entry& entry = set->Obtain(name);
    result.previous = entry.current;
    entry.current = clamped;
    if (target == Target::BaseAndCurrent) entry.base = clamped;
    result.current = entry.current;
    result.ok = true;

    const float base = entry.base;
    if (result.previous != result.current)
        EventBus::Get().Publish(AttributeChangedEvent{ entity, std::string(name), result.previous, result.current, base });

    if (name == kHealth && result.previous > 0.f && result.current <= 0.f)
    {
        result.died = true;
        if (announceDeath) EventBus::Get().Publish(DiedEvent{ entity });
    }

    // A lower ceiling pulls Health down with it.
    if (name == kMaxHealth)
        if (const AttributeSetComponent::Entry* health = set->Find(kHealth); health && health->current > result.current)
            Write(world, entity, kHealth, result.current, Target::BaseAndCurrent, announceDeath);
    return result;
}

bool Init(World& world, Entity entity, std::string_view ref, std::string& error)
{
    if (!world.IsAlive(entity)) { error = "stale_handle: the entity is not alive"; return false; }
    // A new set is a new life: the hit groups the old one remembered do not carry over.
    AttributeSetComponent fresh;
    if (!ApplyProfile(fresh, ref, error)) return false;

    std::vector<AttributeSetComponent::Entry> before;
    if (const AttributeSetComponent* old = world.GetComponent<AttributeSetComponent>(entity)) before = old->entries;
    world.AddComponent<AttributeSetComponent>(entity, std::move(fresh));
    AnnounceMoves(world, entity, before);
    return true;
}

bool InitValues(World& world, Entity entity, const std::vector<std::pair<std::string, float>>& values, std::string& error)
{
    if (!world.IsAlive(entity)) { error = "stale_handle: the entity is not alive"; return false; }
    AttributeSetComponent fresh;
    for (const auto& [name, value] : values)
    {
        if (!ValidName(name, &error)) return false;
        if (!std::isfinite(value)) { error = "not_finite: '" + name + "' must be a finite number"; return false; }
        AttributeSetComponent::Entry& entry = fresh.Obtain(name);
        entry.base = entry.current = value;
    }
    // Health defaults to the ceiling, as a profile does.
    if (const AttributeSetComponent::Entry* max = fresh.Find(kMaxHealth); max && !fresh.Find(kHealth))
    {
        AttributeSetComponent::Entry& health = fresh.Obtain(kHealth);
        health.base = health.current = max->current;
    }
    for (AttributeSetComponent::Entry& e : fresh.entries)
        e.base = e.current = Clamp(fresh, e.name, e.current);

    std::vector<AttributeSetComponent::Entry> before;
    if (const AttributeSetComponent* old = world.GetComponent<AttributeSetComponent>(entity)) before = old->entries;
    world.AddComponent<AttributeSetComponent>(entity, std::move(fresh));
    AnnounceMoves(world, entity, before);
    return true;
}
}
