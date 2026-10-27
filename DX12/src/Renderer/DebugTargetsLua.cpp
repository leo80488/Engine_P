// DebugTargetsLua.cpp - Render.targets() / Render.target(...): the debug target view from Lua
// (Renderer/DebugTargets.h; Lua_API_Reference.md, Render). The SDK's eng.targets() /
// eng.target().
#include "Renderer/DebugTargets.h"
#include "Renderer/Renderer.h"

#include <sol/sol.hpp>

#include <cctype>
#include <cmath>
#include <string>
#include <thread>

namespace
{
    std::vector<std::string> Split(const std::string& list)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (!list.empty())
        {
            const size_t comma = list.find(',', start);
            out.push_back(list.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }

    sol::table ChannelTable(sol::state_view& lua, const std::string& list)
    {
        sol::table t = lua.create_table();
        int n = 0;
        for (const std::string& c : Split(list)) t[++n] = c;
        return t;
    }

    // What Render.target() answers: the view asked for, and what the last
    // painted frame showed.
    sol::object ViewTable(sol::state_view& lua, const Renderer& r)
    {
        const DebugTargets::View v = r.GetDebugTargetView();
        if (v.name.empty()) return sol::make_object(lua, sol::lua_nil);
        sol::table t = lua.create_table();
        t["name"]     = v.name;
        t["mode"]     = DebugTargets::ModeName(v.mode);
        t["channel"]  = v.channel;
        t["min"]      = v.rangeMin;
        t["max"]      = v.rangeMax;
        t["exposure"] = v.exposure;
        t["scale"]    = v.scale;
        t["mask"]     = v.mask;
        const DebugTargets::Shown s = r.GetDebugTargetShown();
        if (s.active)
        {
            sol::table sh = lua.create_table();
            sh["name"]      = s.view.name;
            sh["mode"]      = DebugTargets::ModeName(s.view.mode);
            sh["channel"]   = s.view.channel;
            // The mapping painted: the asker's, or the target's own.
            sh["min"]       = s.view.rangeMin;
            sh["max"]       = s.view.rangeMax;
            sh["scale"]     = s.view.scale;
            sh["mask"]      = s.view.mask;
            sh["available"] = s.available;
            sh["frame"]     = s.frame;
            if (!s.why.empty()) sh["why"] = s.why;
            t["shown"] = sh;
        }
        return t;
    }
}

void DebugTargets::RegisterLua(lua_State* L, Renderer& renderer, std::function<void()> drain)
{
    if (!L) return;
    sol::state_view lua(L);
    sol::table render = lua["Render"].get_or_create<sol::table>();
    const std::thread::id mainThread = std::this_thread::get_id();
    Renderer* r = &renderer;
    auto mainOnly = [mainThread](const char* fn)
    {
        if (std::this_thread::get_id() != mainThread)
            throw sol::error(std::string("Render.") + fn + ": wrong_thread: call it from the main thread (a "
                             "control-channel run, a scenario driver)");
    };

    // Render.targets() -> { {name, label, group, semantic, meaning, channels,
    //   graphName, mode, channel, min, max, scale, mask, available, why,
    //   viewable, viewWhy, width, height, format, shown}, ... } - the registry
    //   in the Buffer menu's order, as the last frame left it (it drains
    //   first). mode / channel / min / max / scale / mask are the target's
    //   defaults; shown marks the one the view shows.
    render.set_function("targets", [r, drain, mainOnly](sol::this_state ts) -> sol::table
    {
        mainOnly("targets");
        if (drain) drain();
        sol::state_view lua(ts);
        const std::string current = r->GetDebugTargetView().name;
        sol::table out = lua.create_table();
        int n = 0;
        for (const DebugTargets::Info& i : r->ListDebugTargets(/*refresh=*/true))
        {
            sol::table t = lua.create_table();
            t["name"]      = i.name;
            t["label"]     = i.label;
            t["group"]     = i.group;
            t["semantic"]  = i.semantic;
            t["meaning"]   = i.meaning;
            t["channels"]  = ChannelTable(lua, i.channels);
            if (!i.graphName.empty()) t["graphName"] = i.graphName;
            t["mode"]      = DebugTargets::ModeName(i.defaultMode);
            t["channel"]   = i.defaultChannel;
            t["min"]       = i.defaultMin;
            t["max"]       = i.defaultMax;
            t["scale"]     = i.defaultScale;
            t["mask"]      = i.defaultMask;
            t["available"] = i.available;
            if (!i.why.empty()) t["why"] = i.why;
            t["viewable"]  = i.viewable;
            if (!i.viewWhy.empty()) t["viewWhy"] = i.viewWhy;
            t["width"]     = i.width;
            t["height"]    = i.height;
            if (!i.format.empty()) t["format"] = i.format;
            t["shown"]     = (i.name == current);
            out[++n] = t;
        }
        return out;
    });

    // Render.target()                -> the view (nil = the lit image), with
    //                                   `shown`: what the last frame painted
    // Render.target(false | "")      -> back to the lit image; returns nil
    // Render.target(name [, opts])   -> show it from the next frame on; returns
    //   the view. opts = { mode = "auto" | "raw" | "channel" | "hdr" | "depth"
    //   | "velocity" | "signed" | "heat" | "categorical" | "bits" | "ev", channel =
    //   0..3 or a channel's name, min, max, exposure, scale, mask }; min / max /
    //   scale / mask not given are the target's own. Errors: unknown_target,
    //   not_viewable, bad_option.
    render.set_function("target", [r, drain, mainOnly](sol::this_state ts, sol::variadic_args va) -> sol::object
    {
        mainOnly("target");
        sol::state_view lua(ts);
        const sol::object first = va.size() > 0 ? va.get<sol::object>(0) : sol::make_object(lua, sol::lua_nil);
        const sol::object second = va.size() > 1 ? va.get<sol::object>(1) : sol::make_object(lua, sol::lua_nil);
        if (!first.valid() || first.get_type() == sol::type::lua_nil)
            return ViewTable(lua, *r);

        std::string error;
        if ((first.get_type() == sol::type::boolean && !first.as<bool>()) ||
            (first.get_type() == sol::type::string && first.as<std::string>().empty()))
        {
            r->SetDebugTargetView(DebugTargets::View{}, error);
            return sol::make_object(lua, sol::lua_nil);
        }
        if (first.get_type() != sol::type::string)
            throw sol::error("Render.target: bad_option: the first argument is a target's name, false (the lit "
                             "image) or nothing (what is shown)");

        // The pipeline is built below, after the drain: the next frame paints.
        if (drain) drain();
        const std::string name = first.as<std::string>();
        // Renderer::FindDebugTargetLocked's rule: the name, then a graph name a
        // curated entry covers, case-insensitively.
        auto same = [](const std::string& a, const std::string& b)
        {
            if (a.size() != b.size()) return false;
            for (size_t k = 0; k < a.size(); ++k)
                if (std::tolower(static_cast<unsigned char>(a[k])) != std::tolower(static_cast<unsigned char>(b[k])))
                    return false;
            return true;
        };
        const std::vector<DebugTargets::Info> all = r->ListDebugTargets(/*refresh=*/true);
        const DebugTargets::Info* hit = nullptr;
        for (const DebugTargets::Info& i : all)
            if (same(i.name, name)) { hit = &i; break; }
        if (!hit)
            for (const DebugTargets::Info& i : all)
                if (!i.graphName.empty() && same(i.graphName, name)) { hit = &i; break; }
        const bool found = hit != nullptr;
        const DebugTargets::Info info = found ? *hit : DebugTargets::Info{};
        if (!found)
            throw sol::error("Render.target: unknown_target: no target '" + name + "' - Render.targets() lists them");
        if (!info.viewable)
            throw sol::error("Render.target: not_viewable: '" + info.name + "' cannot be painted: " + info.viewWhy);

        DebugTargets::View v;
        v.name = info.name;
        if (second.valid() && second.get_type() != sol::type::lua_nil)
        {
            if (second.get_type() != sol::type::table)
                throw sol::error("Render.target: bad_option: the options are a table");
            const sol::table opts = second.as<sol::table>();
            for (const auto& kv : opts)
            {
                if (kv.first.get_type() != sol::type::string)
                    throw sol::error("Render.target: bad_option: option keys are names");
                const std::string key = kv.first.as<std::string>();
                const sol::object& val = kv.second;
                auto number = [&](float& into)
                {
                    if (val.get_type() != sol::type::number)
                        throw sol::error("Render.target: bad_option: " + key + " is a number");
                    into = static_cast<float>(val.as<double>());
                };
                if (key == "mode")
                {
                    if (val.get_type() != sol::type::string || !DebugTargets::ParseMode(val.as<std::string>(), v.mode))
                        throw sol::error("Render.target: bad_option: mode is auto, raw, channel, hdr, depth, "
                                         "velocity, signed, heat, categorical, bits or ev");
                }
                else if (key == "channel")
                {
                    if (val.get_type() == sol::type::number)
                    {
                        const double c = val.as<double>();
                        if (c != std::floor(c)) throw sol::error("Render.target: bad_option: channel is an index 0..3");
                        v.channel = static_cast<int>(c);
                    }
                    else if (val.get_type() == sol::type::string)
                    {
                        const std::vector<std::string> names = Split(info.channels);
                        const std::string want = val.as<std::string>();
                        v.channel = -2;
                        for (size_t k = 0; k < names.size(); ++k)
                            if (names[k] == want) v.channel = static_cast<int>(k);
                        if (v.channel == -2)
                            throw sol::error("Render.target: bad_option: '" + info.name + "' has no channel '" +
                                             want + "' (its channels: " + info.channels + ")");
                    }
                    else
                        throw sol::error("Render.target: bad_option: channel is an index or a channel's name");
                }
                else if (key == "min")      { number(v.rangeMin); v.minGiven = true; }
                else if (key == "max")      { number(v.rangeMax); v.maxGiven = true; }
                else if (key == "exposure") number(v.exposure);
                else if (key == "scale")    { number(v.scale); v.scaleGiven = true; }
                else if (key == "mask")
                {
                    if (val.get_type() != sol::type::number)
                        throw sol::error("Render.target: bad_option: mask is an integer 0..0xFFFFFFFF");
                    const double m = val.as<double>();
                    if (m != std::floor(m) || m < 0.0 || m > 4294967295.0)
                        throw sol::error("Render.target: bad_option: mask is an integer 0..0xFFFFFFFF");
                    v.mask = static_cast<std::uint32_t>(m);
                    v.maskGiven = true;
                }
                else
                    throw sol::error("Render.target: bad_option: unknown option '" + key +
                                     "' (mode, channel, min, max, exposure, scale, mask)");
            }
        }
        if (!r->SetDebugTargetView(v, error))
            throw sol::error("Render.target: bad_option: " + error);
        r->EnsureDebugTargetPipeline();
        return ViewTable(lua, *r);
    });
}
