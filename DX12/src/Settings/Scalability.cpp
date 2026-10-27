#include "Settings/Scalability.h"

#include "Resource/AssetFS.h"
#include "Graphics/IGraphicsDevice.h"   // RHI::AdapterInfo: a first boot's levels
#include "System/Check.h"
#include "System/CommandLine.h"   // -scalability_table (harness)
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <vector>

namespace Settings::Scalability
{
namespace
{
    constexpr const char* kTablePath = "engine/scalability.json";
    constexpr const char* kGroupNames[kGroupCount] = {
        "shadows", "gi", "reflections", "post", "textures", "effects", "viewDistance", "aa",
    };
    // What the lighting tier derives from (Settings/LightingTier.h): a level never sets
    // them. features.ssao is the tier's too, but not one it derives from - the post group
    // holds it.
    constexpr const char* kTierRows[] = {
        "features.renderPath", "features.rtReflections", "features.rtShadows", "features.rtao", "features.ddgi",
    };

    struct Table
    {
        bool        tried = false;
        bool        ok    = false;
        std::string source = kTablePath;
        std::string refusal;
        std::array<std::vector<Entry>, kGroupCount> groups;
        std::vector<size_t> groupOfRow;          // per player row: its group, or kGroupCount
        std::array<int, kGroupCount> written{};  // the level each group's layer holds
        bool        primed = false;              // WriteLayer ran; the observer keeps it from then on
    };
    Table& T()
    {
        static Table s_table;
        return s_table;
    }

    bool SameName(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    size_t RowIndex(const Row& r)
    {
        const auto rows = PlayerRows();
        if (rows.empty() || &r < rows.data() || &r >= rows.data() + rows.size()) return rows.size();
        return static_cast<size_t>(&r - rows.data());
    }

    // A table value as a request of the row's kind (a boolean, a number, or
    // text: an Enum's label or settings.json token), held to the row's rules -
    // as it is: a number Coerce would clamp into the range is refused here.
    bool ToValue(const Row& r, const nlohmann::json& j, Value& out, std::string& why)
    {
        Request q;
        if (j.is_boolean())     { q.type = Request::Type::Bool;   q.b = j.get<bool>(); }
        else if (j.is_number()) { q.type = Request::Type::Number; q.number = j.get<double>(); }
        else if (j.is_string()) q = RequestFromText(r, j.get<std::string>());
        else { why = std::string("a ") + j.type_name() + ", not a value"; return false; }
        const Settings::Refusal refused = Coerce(r, q, out);
        if (refused != Settings::Refusal::None) { why = RefusalName(refused); return false; }
        if ((r.kind == Kind::Int || r.kind == Kind::Float) && q.type == Request::Type::Number
            && (q.number < r.spec.lo || q.number > r.spec.hi))
        {
            why = "outside the row's range";
            return false;
        }
        return true;
    }

    // Group @p g's rows at its level into the Scalability layer - a row whose
    // layer holds that value already is left alone.
    size_t WriteGroup(size_t g)
    {
        Table& t = T();
        SettingsStore& st = SettingsStore::Get();
        const int level = Level(g);
        const std::string writer = std::string("scalability:") + kGroupNames[g] + "=" + LevelName(level);
        size_t n = 0;
        for (const Entry& e : t.groups[g])
        {
            const Value& v = e.values[level];
            const auto& held = st.At(*e.row, Layer::Scalability);
            if (held && SameValue(*e.row, *held, v)) continue;
            st.Write(*e.row, Layer::Scalability, v, writer);
            ++n;
        }
        t.written[g] = level;
        return n;
    }

    bool Refuse(Table& t, std::string why)
    {
        t.ok      = false;
        t.refusal = std::move(why);
        for (auto& g : t.groups) g.clear();
        std::fill(t.groupOfRow.begin(), t.groupOfRow.end(), kGroupCount);
        LOG_ERROR("Scalability: %s: %s - the quality levels set no rows this run",
                  t.source.c_str(), t.refusal.c_str());
        return false;
    }
}

const char* LevelName(int level)
{
    return (level >= 0 && level < kLevelCount) ? kLevelCodec.toToken(level) : "?";
}

bool ParseLevel(std::string_view text, int& out)
{
    for (int l = 0; l < kLevelCount; ++l)
        if (SameName(text, kLevelCodec.toToken(l))) { out = l; return true; }
    return false;
}

const char* GroupName(size_t group)
{
    return group < kGroupCount ? kGroupNames[group] : "?";
}

bool ParseGroup(std::string_view text, size_t& out)
{
    for (size_t g = 0; g < kGroupCount; ++g)
        if (SameName(text, kGroupNames[g])) { out = g; return true; }
    return false;
}

const Row* LevelRow(size_t group)
{
    static const std::array<const Row*, kGroupCount> s_rows = []
    {
        std::array<const Row*, kGroupCount> a{};
        for (size_t g = 0; g < kGroupCount; ++g)
        {
            a[g] = FindPlayerRow(std::string("scalability.") + kGroupNames[g]);
            ENGINE_CHECK(a[g] && a[g]->spec.codec == &kLevelCodec,
                         "Scalability: no level row scalability.%s (EngineSettingsSchema.inl)", kGroupNames[g]);
        }
        return a;
    }();
    return group < kGroupCount ? s_rows[group] : nullptr;
}

size_t GroupOfLevel(const Row& r)
{
    for (size_t g = 0; g < kGroupCount; ++g)
        if (LevelRow(g) == &r) return g;
    return kGroupCount;
}

bool Load()
{
    Table& t = T();
    if (t.tried) return t.ok;
    t.tried = true;
    const auto rows = PlayerRows();
    t.groupOfRow.assign(rows.size(), kGroupCount);
    t.written.fill(kHigh);
    SettingsStore::Get().SetObserver(&OnLevelChanged);

    std::string text;
    bool read = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    // A table of the run's own, off disk (tests/unit/test_scalability.py: the
    // refusals).
    if (const std::string own = Sys::CommandLine::Get().Get("-scalability_table"); !own.empty())
    {
        t.source = own;
        read = Sys::ReadFileText(own, text);
    }
    else
#endif
        read = Resource::AssetFS::Get().ReadFileText(kTablePath, text);
    if (!read)
        return Refuse(t, "not found");
    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/true, /*ignore_comments*/true);
    }
    catch (const std::exception& e)
    {
        return Refuse(t, std::string("does not parse (") + e.what() + ")");
    }
    const auto groups = j.find("groups");
    if (groups == j.end() || !groups->is_object())
        return Refuse(t, "no \"groups\" object");
    const Resource::EngineSettings defaults{};
    for (auto it = groups->begin(); it != groups->end(); ++it)
    {
        size_t g = kGroupCount;
        if (!ParseGroup(it.key(), g))
            return Refuse(t, "no group '" + it.key() + "'");
        if (!it->is_object())
            return Refuse(t, "group '" + it.key() + "' is not an object of rows");
        for (auto jr = it->begin(); jr != it->end(); ++jr)
        {
            const std::string& path = jr.key();
            const Row* r = FindPlayerRow(path);
            if (!r)
                return Refuse(t, "no player row '" + path + "'");
            if (GroupOfLevel(*r) < kGroupCount)
                return Refuse(t, "'" + path + "' is a quality level itself");
            for (const char* tier : kTierRows)
                if (path == tier)
                    return Refuse(t, "'" + path + "' is the lighting tier's (a level never sets it)");
            if (r->spec.policy == Policy::LastWrite)
                return Refuse(t, "'" + path + "' is LastWrite: the level's write would beat the player's value");
            const size_t i = RowIndex(*r);
            if (t.groupOfRow[i] != kGroupCount)
                return Refuse(t, "'" + path + "' is in two groups");
            if (!jr->is_array() || jr->size() != static_cast<size_t>(kLevelCount))
                return Refuse(t, "'" + path + "' needs five values, Low .. Cinematic");
            Entry e;
            e.row = r;
            for (int l = 0; l < kLevelCount; ++l)
            {
                std::string why;
                if (!ToValue(*r, (*jr)[static_cast<size_t>(l)], e.values[l], why))
                    return Refuse(t, "'" + path + "' at " + LevelName(l) + ": " + why);
            }
            // High is the row's default: a fresh install runs the defaults.
            const Value def = ValueOf(*r, defaults);
            if (!SameValue(*r, e.values[kHigh], def))
                return Refuse(t, "'" + path + "': High is " + ValueText(*r, e.values[kHigh]) +
                                 ", not the row's default " + ValueText(*r, def));
            t.groups[g].push_back(e);
            t.groupOfRow[i] = g;
        }
    }
    t.ok = true;
    return true;
}

bool Loaded() { return T().ok; }
const std::string& Source()  { return T().source; }
const std::string& RefusalReason() { return T().refusal; }

std::span<const Entry> Rows(size_t g)
{
    if (g >= kGroupCount) return {};
    const auto& v = T().groups[g];
    return { v.data(), v.size() };
}

size_t RowCount()
{
    size_t n = 0;
    for (const auto& g : T().groups) n += g.size();
    return n;
}

size_t GroupOf(const Row& r)
{
    const Table& t = T();
    const size_t i = RowIndex(r);
    return i < t.groupOfRow.size() ? t.groupOfRow[i] : kGroupCount;
}

const Value* LevelValue(const Row& r, int level)
{
    const size_t g = GroupOf(r);
    if (g >= kGroupCount || level < 0 || level >= kLevelCount) return nullptr;
    for (const Entry& e : T().groups[g])
        if (e.row == &r) return &e.values[level];
    return nullptr;
}

int Level(size_t g)
{
    const Row* lr = LevelRow(g);
    if (!lr) return kHigh;
    const int l = SettingsStore::Get().Effective(*lr).i;
    return (l >= 0 && l < kLevelCount) ? l : kHigh;
}

bool Custom(size_t g)
{
    const SettingsStore& st = SettingsStore::Get();
    const int level = Level(g);
    for (const Entry& e : Rows(g))
        if (!SameValue(*e.row, st.OwnerValue(*e.row), e.values[level])) return true;
    return false;
}

int Overall()
{
    // The groups that set rows; without a table, the level rows themselves.
    int level = -1;
    const bool anyRows = RowCount() > 0;
    for (size_t g = 0; g < kGroupCount; ++g)
    {
        if (anyRows && Rows(g).empty()) continue;
        const int l = Level(g);
        if (level >= 0 && l != level) return -1;
        level = l;
    }
    return level;
}

int FirstBootLevel(uint64_t videoMemoryMB)
{
    return videoMemoryMB >= 8192 ? kHigh : (videoMemoryMB >= 4096 ? 1 : 0);
}

uint64_t FirstBootVideoMemoryMB(const RHI::AdapterInfo& adapter)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    const auto& cl = Sys::CommandLine::Get();
    // A first boot in an unattended run, with this much video memory (MB):
    // the pack's way in (scalability_firstboot, test_scalability).
    if (cl.Has("-scalability_firstboot"))
        return static_cast<uint64_t>(std::max(0, cl.GetInt("-scalability_firstboot", 0)));
#endif
    return adapter.dedicatedVideoMemoryMB;
}

void ApplyFirstBoot(const RHI::AdapterInfo& adapter, const Resource::SettingsLoadInfo& info)
{
    const auto& cl = Sys::CommandLine::Get();
    const uint64_t vram   = FirstBootVideoMemoryMB(adapter);
    bool           forced = false;
#if defined(WITH_HARNESS) && WITH_HARNESS
    forced = cl.Has("-scalability_firstboot");   // FirstBootVideoMemoryMB took its value
#endif
    // The player's own file was read: theirs to keep.
    if (info.found && !info.fromShipped)
    {
        if (forced)
            LOG_INFO("[SCALABILITY] first boot: no - '%s' was read", info.loadedAs.c_str());
        return;
    }
    if (cl.Has("-unattended") && !forced)
        return;
    const int level = FirstBootLevel(vram);
    if (level == kHigh)
    {
        LOG_INFO("[SCALABILITY] first boot: vram=%llu MB (%s) -> High, the defaults - nothing written",
                 static_cast<unsigned long long>(vram), adapter.description.c_str());
        return;
    }
    SettingsStore& st = SettingsStore::Get();
    for (size_t g = 0; g < kGroupCount; ++g)
        if (const Row* lr = LevelRow(g))
            st.Write(*lr, Layer::Player, Value{ .i = level }, "first boot");
    LOG_INFO("[SCALABILITY] first boot: vram=%llu MB (%s) -> %s - every level, the Player layer "
             "(saved with the player's first save)",
             static_cast<unsigned long long>(vram), adapter.description.c_str(), LevelName(level));
}

void WriteLayer()
{
    Table& t = T();
    for (size_t g = 0; g < kGroupCount; ++g)
        WriteGroup(g);
    t.primed = true;
}

void OnLevelChanged(const Row& r)
{
    Table& t = T();
    const size_t g = GroupOfLevel(r);
    if (!t.primed || g >= kGroupCount) return;   // the boot writes every group once (WriteLayer)
    const int was = t.written[g];
    const size_t n = WriteGroup(g);
    const auto history = SettingsStore::Get().History(r);
    LOG_INFO("[SCALABILITY] %s: %s -> %s (%s), %zu row(s) rewritten", kGroupNames[g], LevelName(was),
             LevelName(t.written[g]), history.empty() ? "?" : history.back().writer.c_str(), n);
}

void ApplyChoices(std::vector<std::optional<Value>>& player, std::span<const std::optional<Value>> choices)
{
    using Sub = SettingsStore::LayerSub;
    const SettingsStore& st = SettingsStore::Get();
    const auto rows = PlayerRows();
    player.resize(rows.size());
    const size_t n = std::min(rows.size(), choices.size());
    // Choosing a level is choosing the group: its rows follow the level again,
    // the overrides the player made under the old one gone.
    for (size_t i = 0; i < n; ++i)
    {
        const size_t lg = GroupOfLevel(rows[i]);
        if (!choices[i] || lg >= kGroupCount) continue;
        player[i] = choices[i];
        for (const Entry& e : Rows(lg))
            player[RowIndex(*e.row)].reset();
    }
    for (size_t i = 0; i < n; ++i)
    {
        if (!choices[i] || GroupOfLevel(rows[i]) < kGroupCount) continue;
        player[i] = choices[i];
        const size_t g = GroupOf(rows[i]);
        const Row*   lr = LevelRow(g);
        if (!lr) continue;
        // The level the group will run: its level row's layers, the Player one
        // as chosen. A value that level gives the row already is no override.
        const size_t li = RowIndex(*lr);
        const Sub sub[] = { { Layer::Player, player[li] ? &*player[li] : nullptr } };
        if (const Value* lv = LevelValue(rows[i], st.OwnerValueWith(*lr, sub).i); lv && SameValue(rows[i], *lv, *choices[i]))
            player[i].reset();
    }
}

std::vector<std::optional<Value>> PlayerLayer()
{
    const SettingsStore& st = SettingsStore::Get();
    std::vector<std::optional<Value>> out;
    for (const Row& r : PlayerRows())
        out.push_back(st.At(r, Layer::Player));
    return out;
}

void RecordPlayerLayer(std::span<const std::optional<Value>> player,
                       std::span<const std::optional<Value>> chosen, std::string_view writer)
{
    SettingsStore& st = SettingsStore::Get();
    const auto rows = PlayerRows();
    for (size_t i = 0; i < rows.size() && i < player.size(); ++i)
    {
        const auto& held   = st.At(rows[i], Layer::Player);
        const bool  picked = i < chosen.size() && chosen[i].has_value();
        if (player[i] && (picked || !(held && SameValue(rows[i], *held, *player[i]))))
            st.Write(rows[i], Layer::Player, *player[i], writer);
        else if (!player[i] && held)
            st.Clear(rows[i], Layer::Player, writer);
    }
}

void RecordPlayerChoice(const Row& r, const Value& v, std::string_view writer)
{
    std::vector<std::optional<Value>> player = PlayerLayer();
    std::vector<std::optional<Value>> choice(player.size());
    const size_t i = RowIndex(r);
    if (i >= choice.size()) return;
    choice[i] = v;
    ApplyChoices(player, choice);
    RecordPlayerLayer(player, choice, writer);
}

void ProjectIfPlayer(Resource::EngineSettings& out, std::span<const std::optional<Value>> player)
{
    using Sub = SettingsStore::LayerSub;
    const SettingsStore& st = SettingsStore::Get();
    const auto rows = PlayerRows();
    auto playerAt = [&](size_t i) -> const Value* { return i < player.size() && player[i] ? &*player[i] : nullptr; };
    // The levels first: they decide what the Scalability layer will hold.
    std::array<int, kGroupCount> level{};
    for (size_t g = 0; g < kGroupCount; ++g)
    {
        const Row* lr = LevelRow(g);
        level[g] = kHigh;
        if (!lr) continue;
        const Sub sub[] = { { Layer::Player, playerAt(RowIndex(*lr)) } };
        const int l = st.OwnerValueWith(*lr, sub).i;
        if (l >= 0 && l < kLevelCount) level[g] = l;
    }
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const size_t g = GroupOf(rows[i]);
        if (g < kGroupCount)
        {
            const Sub sub[] = { { Layer::Player, playerAt(i) },
                                { Layer::Scalability, LevelValue(rows[i], level[g]) } };
            PutValue(rows[i], st.OwnerValueWith(rows[i], sub), out);
        }
        else
        {
            const Sub sub[] = { { Layer::Player, playerAt(i) } };
            PutValue(rows[i], st.OwnerValueWith(rows[i], sub), out);
        }
    }
}

void LogBoot()
{
    const Table& t = T();
    if (!t.ok)
    {
        LOG_INFO("[SCALABILITY] table=refused (%s: %s) rows=0", t.source.c_str(), t.refusal.c_str());
        return;
    }
    std::string line = "[SCALABILITY] table=" + t.source + " rows=" + std::to_string(RowCount());
    bool anyCustom = false;
    for (size_t g = 0; g < kGroupCount; ++g)
    {
        const bool custom = Custom(g);
        anyCustom |= custom;
        line += std::string(" ") + kGroupNames[g] + "=" + LevelName(Level(g)) + (custom ? "*" : "");
    }
    const int overall = Overall();
    line += std::string(" overall=") + (overall < 0 ? "Custom" : LevelName(overall)) + (overall >= 0 && anyCustom ? "*" : "");
    LOG_INFO("%s", line.c_str());
}
}
