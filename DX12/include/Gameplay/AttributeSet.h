#pragma once

// Attributes: the named numbers an entity carries (Health, MaxHealth, Resist_Fire, ...), each with a
// base and a current value. Data and order are C++, the formulas that fill them are Lua's.
//   * `base` is the permanent value, `current` what gameplay reads (the base plus whatever temporary
//     modifiers add; nothing modifies it yet, so the two move together). A write through Attr::Write
//     clamps: Health into [0, MaxHealth], any other attribute into its profile column's [min, max].
//   * The defaults of a kind of entity come from a data table (Gameplay/DataTable.h): each column is an
//     attribute, each row a profile ("player", "boss"), and the component remembers the "path#row" it was
//     filled from. A profile row that names MaxHealth but not Health starts the entity at full health.
//   * A name is an identifier (letters, digits, '_'; at most 48), the same rule the table's columns obey.
//     A resistance to a damage type is the attribute "Resist_<Type>": a fraction of that type's damage
//     taken off (0.5 = half, 1 = none, negative = vulnerable).
//   * Every change of a current value is announced as AttributeChangedEvent, and Health reaching zero
//     once as DiedEvent, at EventDelivery's drain points (Gameplay/EventDelivery.h). Lua reads them as
//     the "AttributeChanged" and "Died" events.
// Main thread.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// AttributeSetComponent - one entity's attributes (the scene / prefab / save line is "Attributes:").
struct AttributeSetComponent
{
    struct Entry
    {
        std::string name;
        float       base    = 0.f;
        float       current = 0.f;
    };

    // A hit the damage pipeline applied, remembered so the same swing's next overlap does not apply
    // again (runtime only: a rewrite of the component starts it empty).
    struct RecentHit
    {
        Entity   instigator = NullEntity;
        uint32_t generation = 0;
        uint32_t hitGroup   = 0;
    };

    std::string           profile;     // "path/table.dtable#row" the entries were filled from; empty = none
    std::vector<Entry>    entries;     // sorted by name
    std::vector<RecentHit> recentHits; // oldest first, at most kMaxRecentHits

    static constexpr size_t kMaxRecentHits = 8;

    const Entry* Find(std::string_view name) const;
    Entry*       Find(std::string_view name);
    // The entry for @p name, added at zero when absent.
    Entry&       Obtain(std::string_view name);
    bool         Remove(std::string_view name);

    // The current value, or @p fallback when the attribute is absent.
    float Get(std::string_view name, float fallback = 0.f) const;
};

// The AttributeChanged event: an attribute's current value moved.
struct AttributeChangedEvent
{
    Entity      entity = NullEntity;
    std::string name;
    float       previous = 0.f;
    float       current  = 0.f;
    float       base     = 0.f;
};

// The Died event: Health went from above zero to zero, once per life. The damage pipeline fills the
// cause; a script's own write of Health leaves it empty.
struct DiedEvent
{
    Entity      target     = NullEntity;
    Entity      instigator = NullEntity;
    Entity      source     = NullEntity;
    std::string type;
    float       overkill   = 0.f;
    uint32_t    hitGroup   = 0;
};

namespace Gameplay::Attr
{
    inline constexpr const char* kHealth    = "Health";
    inline constexpr const char* kMaxHealth = "MaxHealth";

    // True when @p name is a legal attribute name; otherwise false, with the reason in @p error.
    bool ValidName(std::string_view name, std::string* error = nullptr);
    // "Resist_<type>".
    std::string ResistName(std::string_view damageType);

    // Splits "path#row". False when either half is empty.
    bool SplitProfile(std::string_view ref, std::string& path, std::string& row);
    // Replaces @p set's entries with the profile row's values (loading the table once); false, with
    // the reason in @p error, when the table is refused or has no such row. @p set is untouched then.
    bool ApplyProfile(AttributeSetComponent& set, std::string_view ref, std::string& error);

    // @p value clamped to what @p name may hold in @p set.
    float Clamp(const AttributeSetComponent& set, std::string_view name, float value);

    // "Health:140:120,MaxHealth:140:140" - name:base:current, the scene / save text form.
    std::string ToText(const AttributeSetComponent& set);
    // Replaces the entries from ToText's form; false, with the first problem in @p error, on a
    // malformed entry (the entries before it are kept).
    bool FromText(AttributeSetComponent& set, std::string_view text, std::string* error = nullptr);

    enum class Target : uint8_t
    {
        BaseAndCurrent,   // the permanent value, and the current one with it
        Current,          // the current value only
    };

    struct WriteResult
    {
        bool        ok = false;
        std::string error;       // why not: "bad_attribute: ..." / "not_finite"
        float       previous = 0.f;   // current before
        float       current  = 0.f;   // current after (clamped)
        bool        died     = false; // Health crossed to zero with this write
    };

    // Sets @p name on @p entity (adding the component and the attribute when absent). Announces the
    // change, and Health reaching zero as DiedEvent unless @p announceDeath is false (the damage pipeline
    // announces it with the cause). Lowering MaxHealth below Health pulls Health down with it.
    WriteResult Write(World& world, Entity entity, std::string_view name, float value,
                      Target target = Target::BaseAndCurrent, bool announceDeath = true);

    // Fills @p entity's set from the profile row @p ref, announcing every current value that moved.
    // False, with the reason in @p error, when the profile cannot be read (the set is untouched).
    bool Init(World& world, Entity entity, std::string_view ref, std::string& error);

    // Replaces @p entity's attributes with @p values (no profile); the same announcements as Init.
    // False, with the reason in @p error, on an illegal name or value.
    bool InitValues(World& world, Entity entity, const std::vector<std::pair<std::string, float>>& values,
                    std::string& error);
}
