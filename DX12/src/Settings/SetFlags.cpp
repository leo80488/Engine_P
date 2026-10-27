#include "Settings/SetFlags.h"

#include "Settings/LightingTier.h"
#include "Settings/Scalability.h"
#include "System/CommandLine.h"

#include <cctype>

namespace Settings
{
const char* LightingTierUnavailableReason(FramePlan::Tier t, bool inlineRaytracing)
{
    bool needsRt = LightingTierNeedsRT(t);
#if WITH_HARNESS
    // -tier_t1_needs_no_rt: T1 is offered without inline RT again (the reverse arm).
    if (t == FramePlan::Tier::T1 && Sys::CommandLine::Get().Has("-tier_t1_needs_no_rt"))
        needsRt = false;
#endif
    return needsRt && !inlineRaytracing ? kLightingTierNeedsRTReason : nullptr;
}

namespace
{
    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    template <class S>
    const RowT<S>* FindAnyCase(std::span<const RowT<S>> rows, const std::string& path)
    {
        const std::string want = Lower(path);
        for (const RowT<S>& r : rows)
            if (Lower(r.path) == want) return &r;
        return nullptr;
    }

    // The value after '=' for a row both documents share the rules of.
    template <class S>
    bool Take(const RowT<S>& row, const std::string& arg, size_t eq, std::vector<SetFlag<S>>& out,
              std::vector<std::string>* errors)
    {
        if (eq == std::string::npos)
        {
            if (errors) errors->push_back("'" + arg + "' needs a value: -set:" + std::string(row.path) + "=<value>");
            return false;
        }
        SetFlag<S> f;
        f.row  = &row;
        f.flag = std::string("-set:") + row.path;
        const Refusal why = Coerce(row, RequestFromText(row, arg.substr(eq + 1)), f.value);
        if (why != Refusal::None)
        {
            if (errors) errors->push_back("'" + arg + "': " + std::string(row.path) + " refuses the value ("
                                          + RefusalName(why) + ")");
            return false;
        }
        out.push_back(std::move(f));
        return true;
    }
}

SetFlags ParseSetFlags(std::vector<std::string>* errors)
{
    SetFlags out;
    const auto& cl = Sys::CommandLine::Get();
    // -scalability=<level> / -scalability:<group>=<level> (Settings/Scalability.h): quality
    // levels for this run - every group's, then the named groups' in argv order. First, so
    // the lighting tier and an explicit -set: of a level, or of a row a level sets, win.
    auto level = [&](size_t g, const std::string& name, const std::string& flag, const std::string& arg)
    {
        int l = 0;
        const Row* row = Scalability::LevelRow(g);
        if (!Scalability::ParseLevel(name, l))
        {
            if (errors) errors->push_back("'" + arg + "': no such level (Low, Medium, High, Ultra, Cinematic)");
            return;
        }
        if (!row) return;
        SetFlag<Resource::EngineSettings> f;
        f.row     = row;
        f.flag    = flag;
        f.value.i = l;
        out.player.push_back(std::move(f));
    };
    if (cl.Has("-scalability"))
    {
        const std::string name = cl.Get("-scalability");
        int l = 0;
        if (!Scalability::ParseLevel(name, l))
            level(0, name, "-scalability", "-scalability=" + name);   // the one error
        else
            for (size_t g = 0; g < Scalability::kGroupCount; ++g)
                level(g, name, "-scalability", "-scalability=" + name);
    }
    for (const std::string& a : cl.Args())
    {
        const size_t dashes = a.rfind("--", 0) == 0 ? 2 : (a.rfind("-", 0) == 0 ? 1 : 0);
        if (!dashes || Lower(a.substr(dashes, 12)) != "scalability:")
            continue;
        const size_t eq   = a.find('=');
        const size_t from = dashes + 12;
        const std::string group = a.substr(from, eq == std::string::npos ? std::string::npos : eq - from);
        (void)cl.Has(("-scalability:" + group).c_str());
        size_t g = 0;
        if (!Scalability::ParseGroup(group, g))
        {
            if (errors) errors->push_back("'" + a + "': no such group (shadows, gi, reflections, post, textures, "
                                          "effects, viewDistance, aa)");
            continue;
        }
        if (eq == std::string::npos)
        {
            if (errors) errors->push_back("'" + a + "' needs a level: -scalability:" + group + "=<level>");
            continue;
        }
        level(g, a.substr(eq + 1), std::string("-scalability:") + Scalability::GroupName(g), a);
    }
    // -lighting_tier=<T0..T4|Ref>: the tier's preset (the rows Settings/LightingTier.h
    // names, the table the Project Settings combo and Settings.setLightingTier write) as
    // player overrides for this run. First, so an explicit -set: of one of its rows wins.
    if (cl.Has("-lighting_tier"))
    {
        const std::string name = cl.Get("-lighting_tier");
        FramePlan::Tier tier;
        if (!ParseLightingTier(name, tier))
        {
            if (errors) errors->push_back("'-lighting_tier=" + name + "': no such tier (T0, T1, T2, T3, T4, Ref)");
        }
        else
        {
            for (const TierWrite& w : LightingTierWrites(tier))
            {
                const Row* row = FindAnyCase(PlayerRows(), w.path);
                if (!row) continue;
                SetFlag<Resource::EngineSettings> f;
                f.row  = row;
                f.flag = "-lighting_tier";
                if (row->kind == Kind::Bool) f.value.b = w.value != 0;
                else                         f.value.i = w.value;
                out.player.push_back(std::move(f));
            }
        }
    }
    for (const std::string& a : cl.Args())
    {
        const size_t dashes = a.rfind("--", 0) == 0 ? 2 : (a.rfind("-", 0) == 0 ? 1 : 0);
        if (!dashes || Lower(a.substr(dashes, 4)) != "set:")
            continue;
        const size_t eq   = a.find('=');
        const size_t from = dashes + 4;
        const std::string path = a.substr(from, eq == std::string::npos ? std::string::npos : eq - from);
        (void)cl.Has(("-set:" + path).c_str());
        // A path is one document's (DevRows.cpp and PassRows.cpp check that no
        // path is in two).
        if (const Row* row = FindAnyCase(PlayerRows(), path))
            Take(*row, a, eq, out.player, errors);
        else if (const DevRow* dev = FindAnyCase(DevRows(), path))
            Take(*dev, a, eq, out.developer, errors);
        else if (const PassRow* pass = FindAnyCase(PassRows(), path))
            Take(*pass, a, eq, out.pass, errors);
        else if (errors)
            errors->push_back("'" + a + "': there is no setting '" + path +
                              "' (python tools/gen_catalog.py --setting all names them)");
    }
    return out;
}
}
