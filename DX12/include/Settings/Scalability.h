#pragma once

// Settings/Scalability.h - the quality levels.
//
// A group is a set of player rows a level sets together: shadows, gi, reflections, post,
// textures, effects, viewDistance, aa. Each group's level is a player row of its own
// (scalability.<group>: Low, Medium, High, Ultra, Cinematic; EngineSettingsSchema.inl),
// saved in settings.json like any other. The table (engine/scalability.json, read
// through AssetFS) names each group's rows and their value at every level.
//
// The Scalability layer: whenever a level's effective value changes, whoever changed it
// (settings.json, -scalability / -set:, Settings.apply, an editor control, a Runtime
// write), the store calls OnLevelChanged and the group's rows at the new level are
// rewritten into the store's Scalability layer (writer "scalability:<group>=<level>").
// A group row is Layered or Capped, so the player's own value (the Player layer) sits
// over its level and a -set: over both; with neither, the level decides. High is every
// row's default, so a fresh install runs exactly the defaults.
//
// Choosing a level is the player's choice of the whole group: a player write of a level
// row lets go of the group's Player values (RecordPlayerChoice), and a player value
// equal to what the level gives is not kept as an override. A load (settings.json, a
// reload) records what the file holds instead; the command line never touches the
// Player layer.
//
// The table's rules, checked at load: every row is a player row, in one group only, not
// one the lighting tier derives (features.renderPath, rtReflections, rtShadows, rtao,
// ddgi), not LastWrite; exactly five values, each one the row accepts as it is (Coerce,
// no clamping); the High value is the row's default. One bad entry refuses the whole
// table (an ERROR): every group then has no rows and the levels change nothing.
//
// Main thread, like the store.

#include "Settings/SettingsStore.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Resource { struct EngineSettings; struct SettingsLoadInfo; }
namespace RHI { struct AdapterInfo; }

namespace Settings::Scalability
{
    inline constexpr int kLevelCount = 5;   // Low, Medium, High, Ultra, Cinematic
    inline constexpr int kHigh       = 2;   // every group's default: the rows' defaults

    // "Low" .. "Cinematic" (the level rows' labels); "?" out of range.
    const char* LevelName(int level);
    // Any case. False for anything else.
    bool        ParseLevel(std::string_view text, int& out);

    // The groups, in their level rows' order.
    inline constexpr size_t kGroupCount = 8;
    // "shadows", "gi", "reflections", "post", "textures", "effects",
    // "viewDistance", "aa" (the level row is scalability.<name>).
    const char* GroupName(size_t group);
    // Any case. False for anything else.
    bool        ParseGroup(std::string_view text, size_t& out);
    // scalability.<group>; nullptr for a group out of range.
    const Row*  LevelRow(size_t group);
    // The group whose level @p r is; kGroupCount when it is not a level row.
    size_t      GroupOfLevel(const Row& r);

    // One row a group sets, with its value at each level.
    struct Entry
    {
        const Row*                     row = nullptr;
        std::array<Value, kLevelCount> values;
    };

    // engine/scalability.json, read and checked once (later calls answer the
    // first one's result): true when the table was accepted. The boot load
    // calls it first. Installs the store's observer. A harness build reads
    // -scalability_table=<path> off disk instead (the refusals' unit test).
    bool Load();
    bool Loaded();
    // Where the table came from and, when it was refused, why (the boot line).
    const std::string& Source();
    const std::string& RefusalReason();
    // The rows group @p g sets (empty without a table, or for an empty group).
    std::span<const Entry> Rows(size_t g);
    size_t RowCount();
    // The group @p r belongs to; kGroupCount when none.
    size_t GroupOf(const Row& r);
    // The value @p r's level gives it at @p level; nullptr when @p r is in no
    // group (or the level is out of range).
    const Value* LevelValue(const Row& r, int level);

    // A group's level: its level row's effective value.
    int  Level(size_t g);
    // Whether some row of the group runs a value its level does not give it
    // (an override, a -set:, a Runtime value). An empty group never is.
    bool Custom(size_t g);
    // Every group with rows at one level: that level; otherwise -1 (custom).
    int  Overall();

    // The first boot. When the boot load read no settings file of the player's own (none
    // at the path, or the shipped template beside the exe), every group's level is picked
    // from the adapter's video memory: >= 8 GB High (the defaults: nothing is written),
    // >= 4 GB Medium, less Low. Written in the Player layer (writer "first boot") as a
    // player's choice of Overall would be, with the template's own values over it, and saved
    // only with the player's first save: until then every launch picks again. The lighting
    // tier is left alone. An unattended run skips it unless -scalability_firstboot=<MB>
    // (harness) fakes the video memory.
    int  FirstBootLevel(uint64_t videoMemoryMB);
    void ApplyFirstBoot(const RHI::AdapterInfo& adapter, const Resource::SettingsLoadInfo& info);
    // The video memory a first boot judges, MB: the adapter's dedicated memory, or the
    // -scalability_firstboot=<MB> a harness run fakes. What the skinned vertex ring's
    // adapter-sized budget reads too (SkinnedVertexRing::Init), so a pack can give it the Low
    // tier's.
    uint64_t FirstBootVideoMemoryMB(const RHI::AdapterInfo& adapter);

    // The boot (Resource::LoadBootEngineSettings, after settings.json, the
    // command line and a first boot): every group's rows at its level into the
    // Scalability layer; from then on the store's observer keeps it there.
    void WriteLayer();
    // The store's observer: a level row's effective value changed.
    void OnLevelChanged(const Row& r);

    // THE PLAYER'S RULE. @p choices (indexed like PlayerRows(); nullopt: not
    // chosen) recorded into @p player, a Player layer's contents: the quality
    // levels first - a chosen level lets go of its group's values - then the
    // other rows, a group row's value its level gives already (the level the
    // group will run then) letting go of the row's value instead of being
    // kept. Settings.apply and Settings.save (what an apply leaves) and the
    // editor's controls all go through it.
    void ApplyChoices(std::vector<std::optional<Value>>& player, std::span<const std::optional<Value>> choices);
    // The store's Player layer, row by row.
    std::vector<std::optional<Value>> PlayerLayer();
    // @p player into the store's Player layer, by @p writer: a write or a clear
    // for each row whose value differs - and a write of each row in @p chosen
    // that keeps a value even when the layer holds it already (a LastWrite
    // row's latest write is what runs).
    void RecordPlayerLayer(std::span<const std::optional<Value>> player,
                           std::span<const std::optional<Value>> chosen, std::string_view writer);
    // One row chosen (an editor control): ApplyChoices, then RecordPlayerLayer.
    void RecordPlayerChoice(const Row& r, const Value& v, std::string_view writer);

    // Every player row of @p out set to the owner value it will have once the
    // Player layer holds exactly @p player (indexed like PlayerRows(); nullopt:
    // no value) - the levels that leaves rewriting the Scalability layer. What
    // a Settings.reset (all nullopt) or a Settings.reload (the file's choices)
    // will apply.
    void ProjectIfPlayer(Resource::EngineSettings& out, std::span<const std::optional<Value>> player);

    // [SCALABILITY] table=<source> rows=<n> <group>=<level>[*] ... overall=<level|Custom>
    // (* = custom), or table=refused (<why>). Once, at boot.
    void LogBoot();
}
