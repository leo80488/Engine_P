#include "Resource/LuaSettingsBindings.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound

#include "Resource/EngineSettings.h"
#include "Resource/EngineDevSettings.h"
#include "Settings/SettingsRows.h"      // the player rows this bridge walks
#include "Settings/SettingsStore.h"     // Settings.where; apply records its writes
#include "Settings/LightingTier.h"      // Settings.setLightingTier
#include "Settings/Scalability.h"       // the quality levels: apply records a level's rule
#include "Diagnostics/OwnerAccounting.h"   // Diag::DrainCause: a runtime write drains as ApplyTo does
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/RenderPass/PTRtxdiCore.h"
#include "Graphics/GraphicsDX12.h"      // FG caps, the display refresh and the desktop size live on the concrete backend
#include "Graphics/StreamlineBackend.h" // DLSS / DLSS-G / Reflex availability + reason strings
#include "Graphics/HDROutput.h"
#include "Platform/Window.h"
#include "System/Log.h"
#include "System/Check.h"              // ENGINE_CHECK: the schema / enum parity, DevWrite's member lookup

#include <sol/sol.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace Resource
{
namespace
{
    // ---- Schema ----
    // The player rows (Settings/SettingsRows.h, expanded from EngineSettingsSchema.inl):
    // the table settings.json's load and save walk, so a path the menu can set is
    // exactly a key the file holds. Pointer-to-member rows: a renamed field is a build
    // error.
    using Settings::Kind;
    using Settings::kSettingRestart;
    using SettingDesc = Settings::Row;

    // engine.json's rows (Settings/Rows/Developer.inl): Settings.where / setRuntime /
    // clearRuntime and the pipe take them too; the menu (get / set / apply / options /
    // list) is the player rows' alone.
    using DevDesc = Settings::DevRow;

    // The pass rows (Settings/Rows/Passes.inl): the passes' tuning parameters;
    // Settings.where / setRuntime / clearRuntime and the pipe take them as they take
    // engine.json's.
    using PassDesc = Settings::PassRow;

    std::span<const SettingDesc> Schema() { return Settings::PlayerRows(); }

    const SettingDesc* Find(const std::string& path) { return Settings::FindPlayerRow(path); }

    const DevDesc* FindDev(const std::string& path) { return Settings::FindDevRow(path); }

    const PassDesc* FindPass(const std::string& path) { return Settings::FindPassRow(path); }

    template <class S>
    const char* DomainOf()
    {
        if constexpr (std::is_same_v<S, EngineSettings>)    return "player";
        if constexpr (std::is_same_v<S, EngineDevSettings>) return "developer";
        return "pass";
    }

    // ---- Equality ----
    // EngineSettings has no operator==, and one would be another list to keep in step.
    // Comparing THROUGH the schema makes dirty-tracking cover exactly the fields a menu
    // can edit.
    bool SchemaEqual(const EngineSettings& a, const EngineSettings& b)
    {
        for (const auto& d : Schema())
        {
            switch (d.kind)
            {
                case Kind::Bool:   if (a.*d.mb != b.*d.mb) return false; break;
                case Kind::Int:
                case Kind::Enum:
                case Kind::Choice: if (a.*d.mi != b.*d.mi) return false; break;
                // Exact compare is right here: both sides are values this bridge
                // wrote, so there is no accumulated arithmetic error to absorb,
                // and a tolerance would make a small deliberate slider nudge
                // read as "not dirty".
                case Kind::Float:  if (a.*d.mf != b.*d.mf) return false; break;
                case Kind::String: if (a.*d.ms != b.*d.ms) return false; break;
            }
        }
        return true;
    }

    // ---- The bridge ----
    struct Bridge
    {
        struct Rebase
        {
            EngineSettings                  values;
            std::optional<SettingsLoadInfo> file;
            EngineSettings                  fileValues;   // the load itself (reload): what the Player layer records
        };

        Renderer*        renderer = nullptr;
        IGraphicsDevice* gfx      = nullptr;
        Plat::Window*    window   = nullptr;

        EngineSettings pending;   // what the menu is editing
        EngineSettings applied;   // last state pushed to the engine

        // A Settings.reset or Settings.reload not applied yet: its rows as they will read
        // once the Player layer is replaced - emptied (reset) or the file's keys (reload,
        // @c file) - the baseline the apply records later edits against.
        std::optional<Rebase> rebase;

        void SeedFromEngine()
        {
            // THE STORE FIRST, then the engine overwrites what it actually owns.
            //
            // CaptureFrom alone is silently not enough: it reports only fields with an
            // engine-side reader, and some have none - ui.language has no C++ consumer (the
            // script layer owns it), ui.defaultFont is apply-at-boot, and the quality levels are
            // the store's alone. Seeded from a default struct, Settings.get("ui.language")
            // would return "en" whatever the player chose, so a language picker opens on the
            // wrong entry and Loc caches it for the session.
            //
            // The store holds what the boot load recorded - the file's choices, the command
            // line's -set:, a first boot's levels, the Scalability layer - so its values come
            // first; then CaptureFrom supplies the fields the ENGINE is authoritative for
            // (which may legitimately differ: a demoted AA mode, a latched HDR change, a window
            // the player has since resized).
            ProjectStoreValues(applied);
            applied.CaptureFrom(*renderer, *gfx, window);
            pending = applied;
        }

        // The Player layer an apply leaves (and a save writes): the store's -
        // after a reset, empty; after a reload, the file's choices - with every
        // row the menu changed since the last apply (since the reset / reload)
        // recorded the player's way (Settings::Scalability::ApplyChoices: a
        // changed quality level lets go of its group's values, and a group
        // row's value its level gives already is no override).
        // @p choicesOut: the rows the menu changed, with their values.
        std::vector<std::optional<Settings::Value>> PlayerAfterApply(
            std::vector<std::optional<Settings::Value>>* choicesOut = nullptr) const
        {
            const auto rows = Schema();
            std::vector<std::optional<Settings::Value>> player(rows.size());
            if (!rebase)
                player = Settings::Scalability::PlayerLayer();
            else if (rebase->file)
            {
                const std::vector<uint8_t> file = PlayerRowsToRecord(rebase->fileValues, *rebase->file);
                for (size_t i = 0; i < rows.size() && i < file.size(); ++i)
                    if (file[i]) player[i] = Settings::ValueOf(rows[i], rebase->fileValues);
            }
            const EngineSettings& base = rebase ? rebase->values : applied;
            std::vector<std::optional<Settings::Value>> choices(rows.size());
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const Settings::Value v = Settings::ValueOf(rows[i], pending);
                if (!Settings::SameValue(rows[i], v, Settings::ValueOf(rows[i], base))) choices[i] = v;
            }
            Settings::Scalability::ApplyChoices(player, choices);
            if (choicesOut) *choicesOut = std::move(choices);
            return player;
        }

        // Every row is pushed to its owner; the store records only what CHANGED since the
        // last apply (after a reset / reload: since its baseline, the Player layer replaced
        // first), so the Player layer holds the player's own choices and a layer under it
        // (or a -set: / Runtime value over it) keeps its place. The Player layer is written
        // as PlayerAfterApply leaves it, the quality levels' rule included: a changed level
        // lets go of its group's overrides and its new level rewrites the group's
        // Scalability layer (the store's observer). A Capped or Layered row then takes the
        // store's owner value, since a layer over the player's can hold it. A LastWrite
        // row's latest write is the menu's own, so its value stands - the window size,
        // which the store only learns at a Save, among them.
        void Apply(const char* writer)
        {
            Settings::SettingsStore& st = Settings::SettingsStore::Get();
            const auto rows = st.Rows();
            std::vector<std::optional<Settings::Value>> choices;
            const auto player = PlayerAfterApply(&choices);
            Settings::Scalability::RecordPlayerLayer(player, choices, writer);
            rebase.reset();
            for (const auto& r : rows)
                if (r.spec.policy != Settings::Policy::LastWrite)
                    Settings::PutValue(r, st.OwnerValue(r), pending);
            pending.ApplyTo(*renderer, *gfx, window);
            applied = pending;
        }
    };

    // The bridge the pipe's `setting` verb reaches. RegisterLuaSettingsBindings
    // sets it; a re-registration (a new Lua state) replaces it.
    std::weak_ptr<Bridge> g_bridge;

    // A Lua value as a settings request (Settings/SettingsStore.h: Coerce).
    Settings::Request ToRequest(const sol::object& v)
    {
        Settings::Request q;
        if (v.is<bool>())             { q.type = Settings::Request::Type::Bool;   q.b      = v.as<bool>(); }
        else if (v.is<double>())      { q.type = Settings::Request::Type::Number; q.number = v.as<double>(); }
        else if (v.is<std::string>()) { q.type = Settings::Request::Type::Text;   q.text   = v.as<std::string>(); }
        return q;
    }

    // A store value as the Lua value Settings.get returns for the row.
    template <class S>
    sol::object ToLua(sol::state& lua, const Settings::RowT<S>& d, const Settings::Value& v)
    {
        switch (d.kind)
        {
            case Kind::Bool:   return sol::make_object(lua, v.b);
            case Kind::Int:
            case Kind::Enum:
            case Kind::Choice: return sol::make_object(lua, v.i);
            case Kind::Float:  return sol::make_object(lua, v.f);
            case Kind::String: return sol::make_object(lua, v.s);
        }
        return sol::lua_nil;
    }

    // The same, as JSON (the pipe's `setting` verb).
    template <class S>
    nlohmann::json ToJson(const Settings::RowT<S>& d, const Settings::Value& v)
    {
        switch (d.kind)
        {
            case Kind::Bool:   return v.b;
            case Kind::Int:
            case Kind::Enum:
            case Kind::Choice: return v.i;
            case Kind::Float:  return v.f;
            case Kind::String: return v.s;
        }
        return nullptr;
    }

    // ---- Runtime writes ----
    // A Runtime-layer write of one row (value == nullptr clears its Runtime layer),
    // then the row's new EFFECTIVE value pushed to its owner alone
    // (Settings::ApplyPlayerRow; the rest of the phase stays as it is live) - for a
    // Capped row its ALLOWANCE (OwnerValue: the owner applies the scene's intent
    // itself). The bridge's pending and applied copies follow, so Settings.get reads it
    // and a later Settings.apply re-asserts it instead of reverting it. Never saved: the
    // Runtime layer is not persisted, and with pending and applied moved together it is
    // no menu change for Settings.save to pick up. A quality level's write moves its
    // group's rows too (their Scalability layer): each row whose owner value moved is
    // followed and pushed the same way. Returns whether the row has an owner to push to
    // (a Boot / None row - a level among them - is recorded only).
    bool RuntimeWrite(Bridge& br, const SettingDesc& d, const Settings::Value* value, const char* writer)
    {
        Settings::SettingsStore& st = Settings::SettingsStore::Get();
        const auto rows = Schema();
        std::vector<Settings::Value> before;
        before.reserve(rows.size());
        for (const auto& r : rows) before.push_back(st.OwnerValue(r));
        if (value) st.Write(d, Settings::Layer::Runtime, *value, writer);
        else       st.Clear(d, Settings::Layer::Runtime, writer);
        std::vector<uint8_t> moved(rows.size(), 0);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const Settings::Value now = st.OwnerValue(rows[i]);
            if (&rows[i] != &d && Settings::SameValue(rows[i], before[i], now)) continue;
            moved[i] = 1;
            Settings::PutValue(rows[i], now, br.pending);
            Settings::PutValue(rows[i], now, br.applied);
        }
        br.gfx->DrainCpuFrame(static_cast<uint8_t>(Diag::DrainCause::Settings));
        Settings::ApplyCtx c{ *br.renderer, *br.gfx, br.window };
        bool pushed = false;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (&rows[i] == &d)  pushed = Settings::ApplyPlayerRow(d, br.applied, c);
            else if (moved[i])   Settings::ApplyPlayerRow(rows[i], br.applied, c);
        }
        return pushed;
    }

    // The same for one of engine.json's rows, then that row's new effective value
    // pushed to the Renderer alone (Settings::ApplyDevRow: each developer hook reads
    // only its own field). No drain (the values land at the next frame's service point)
    // and no copy to follow (engine.json has no menu; its Save captures from the
    // Renderer). Returns whether the row has an apply hook (rayReconstructionLoad,
    // gameFlow and debug are read at boot: recorded only).
    bool DevRuntimeWrite(Bridge& br, const DevDesc& d, const Settings::Value* value, const char* writer)
    {
        Settings::DevSettingsStore& st = Settings::DevSettingsStore::Get();
        if (value) st.Write(d, Settings::Layer::Runtime, *value, writer);
        else       st.Clear(d, Settings::Layer::Runtime, writer);
        EngineDevSettings s;
        Settings::PutValue(d, st.Effective(d), s);
        Settings::DevApplyCtx c{ *br.renderer };
        return Settings::ApplyDevRow(d, s, c);
    }

    // One of the pass rows: the store IS what the passes read - the write
    // publishes the row's new effective value to Settings::Pass::<member>() -
    // so there is no owner to push to and nothing to drain; a pass sees it at
    // its next read. Always applied.
    bool PassRuntimeWrite(const PassDesc& d, const Settings::Value* value, const char* writer)
    {
        Settings::WritePassSetting(d, Settings::Layer::Runtime, value, writer);
        return true;
    }

    bool WriteRuntime(Bridge& br, const SettingDesc& d, const Settings::Value* v, const char* w) { return RuntimeWrite(br, d, v, w); }
    bool WriteRuntime(Bridge& br, const DevDesc& d, const Settings::Value* v, const char* w)     { return DevRuntimeWrite(br, d, v, w); }
    bool WriteRuntime(Bridge&, const PassDesc& d, const Settings::Value* v, const char* w)       { return PassRuntimeWrite(d, v, w); }

    // One of engine.json's rows by its member - the developer setters below
    // name their rows this way, so a renamed field is a build error.
    bool MemberIs(const DevDesc& r, bool EngineDevSettings::* m)        { return r.mb == m; }
    bool MemberIs(const DevDesc& r, int EngineDevSettings::* m)         { return r.mi == m; }
    bool MemberIs(const DevDesc& r, float EngineDevSettings::* m)       { return r.mf == m; }
    bool MemberIs(const DevDesc& r, std::string EngineDevSettings::* m) { return r.ms == m; }

    template <class M>
    void DevWrite(Bridge& br, M EngineDevSettings::* member, const Settings::Value& v, const char* writer)
    {
        for (const DevDesc& r : Settings::DevRows())
            if (MemberIs(r, member)) { DevRuntimeWrite(br, r, &v, writer); return; }
        ENGINE_CHECK(false, "%s: no engine.json row holds this member", writer);
    }

    // A Lua value checked against the row (Settings::Coerce), or a warning.
    template <class S>
    bool CoerceOrWarn(const Settings::RowT<S>& d, const sol::object& v, Settings::Value& out, const char* fn)
    {
        const Settings::Refusal why = Settings::Coerce(d, ToRequest(v), out);
        if (why == Settings::Refusal::None) return true;
        LOG_WARNING("%s('%s'): refused (%s)", fn, d.path, Settings::RefusalName(why));
        return false;
    }

    // The pipe accepts a path in any case, as -set: does.
    template <class S>
    const Settings::RowT<S>* FindAnyCase(std::span<const Settings::RowT<S>> rows, const std::string& path)
    {
        auto low = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        const std::string want = low(path);
        for (const Settings::RowT<S>& d : rows)
            if (low(d.path) == want) return &d;
        return nullptr;
    }

    // One row as the pipe reports it: its document (player / developer / pass),
    // the store's effective value (also as text - an Enum's label), the layer it
    // came from, the policy, restart.
    template <class S>
    nlohmann::json RowJson(const Settings::RowT<S>& d)
    {
        const Settings::BasicStore<S>& st = Settings::BasicStore<S>::Get();
        const Settings::Value& v = st.Effective(d);
        nlohmann::json o;
        o["path"]    = d.path;
        o["domain"]  = DomainOf<S>();
        o["value"]   = ToJson(d, v);
        o["text"]    = Settings::ValueText(d, v);
        o["source"]  = Settings::LayerName(st.Source(d));
        o["policy"]  = Settings::PolicyName(d.spec.policy);
        o["restart"] = (d.spec.flags & kSettingRestart) != 0;
        return o;
    }

    // RowJson plus every layer that holds a value, the revisions and the last
    // writes - Settings.where's answer.
    template <class S>
    nlohmann::json WhereJson(const Settings::RowT<S>& d)
    {
        const Settings::BasicStore<S>& st = Settings::BasicStore<S>::Get();
        nlohmann::json o = RowJson(d);
        o["revision"]       = st.RowRevision(d);
        o["globalRevision"] = st.Revision();
        nlohmann::json layers = nlohmann::json::object();
        for (size_t l = 0; l < Settings::kLayerCount; ++l)
            if (const auto& v = st.At(d, static_cast<Settings::Layer>(l)))
                layers[Settings::LayerName(static_cast<Settings::Layer>(l))] = ToJson(d, *v);
        o["layers"] = std::move(layers);
        nlohmann::json history = nlohmann::json::array();
        for (const Settings::WriteRecord& w : st.History(d))
        {
            nlohmann::json e;
            e["layer"]  = Settings::LayerName(w.layer);
            e["writer"] = w.writer;
            e["frame"]  = w.frame;
            if (w.cleared) e["cleared"] = true;
            else           e["value"]   = ToJson(d, w.value);
            history.push_back(std::move(e));
        }
        o["history"] = std::move(history);
        return o;
    }

    // The same as a Lua table (Settings.where).
    template <class S>
    sol::table WhereTable(sol::state& lua, const Settings::RowT<S>& d)
    {
        const Settings::BasicStore<S>& st = Settings::BasicStore<S>::Get();
        sol::table t = lua.create_table();
        t["path"]           = std::string(d.path);
        t["domain"]         = std::string(DomainOf<S>());
        t["policy"]         = std::string(Settings::PolicyName(d.spec.policy));
        t["value"]          = ToLua(lua, d, st.Effective(d));
        t["source"]         = std::string(Settings::LayerName(st.Source(d)));
        t["revision"]       = st.RowRevision(d);
        t["globalRevision"] = st.Revision();
        sol::table layers = lua.create_table();
        for (size_t l = 0; l < Settings::kLayerCount; ++l)
            if (const auto& v = st.At(d, static_cast<Settings::Layer>(l)))
                layers[Settings::LayerName(static_cast<Settings::Layer>(l))] = ToLua(lua, d, *v);
        t["layers"] = layers;
        sol::table history = lua.create_table();
        int n = 1;
        for (const Settings::WriteRecord& w : st.History(d))
        {
            sol::table e = lua.create_table();
            e["layer"]  = std::string(Settings::LayerName(w.layer));
            e["writer"] = w.writer;
            e["frame"]  = w.frame;
            if (w.cleared) e["cleared"] = true;
            else           e["value"]   = ToLua(lua, d, w.value);
            history[n++] = e;
        }
        t["history"] = history;
        return t;
    }

    // caps() - everything a menu must ask before OFFERING an option.
    sol::table BuildCaps(sol::state& lua, Bridge& br)
    {
        sol::table t = lua.create_table();

        // DXR, from the backend-agnostic capability struct (the flag Renderer uses to
        // decide whether DDGIPass is created). The Renderer's RT setters run a switch only
        // where inline RT exists, so a menu that does not check it ships controls that lie.
        t["raytracing"] = br.gfx->GetCapabilities().raytracing;
        t["inlineRaytracing"] = br.gfx->GetCapabilities().inlineRaytracing;
        t["renderPathRequested"] = static_cast<int>(br.renderer->GetRenderPathRequested());
        t["renderPathEffective"] = static_cast<int>(br.renderer->GetRenderPath());
        t["ptSamples"] = br.renderer->GetPathTraceSamples();
        t["ptResets"] = br.renderer->GetPathTraceResets();
        t["ptAreaModeRequested"] = br.renderer->GetPTAreaMode();
        t["ptAreaMotionRequested"] = br.renderer->GetPTAreaMotion();
        t["ptAreaHistoryRequested"] = br.renderer->GetPTAreaHistory();
        t["meshShader"] = br.gfx->GetCapabilities().meshShader;
        // The adapter (what a first boot's quality pick reads).
        {
            const RHI::AdapterInfo a = br.gfx->GetAdapterInfo();
            t["vramMB"]   = static_cast<double>(a.dedicatedVideoMemoryMB);
            t["vendorId"] = a.vendorId;
            t["vendor"]   = std::string(a.vendorId == 0x10DE ? "NVIDIA" : a.vendorId == 0x1002 ? "AMD"
                                      : a.vendorId == 0x8086 ? "Intel" : a.vendorId == 0x1414 ? "Microsoft" : "other");
            t["adapter"]  = a.description;
            // Every hardware adapter's name, the high-performance one first (display.adapter's options).
            sol::table names = lua.create_table();
            int n = 0;
            for (const RHI::AdapterInfo& each : br.gfx->ListAdapters()) names[++n] = each.description;
            t["adapters"] = names;
        }

        // HDR: advisory, NOT a gate (see the HDR rows in EngineSettingsSchema.inl).
        const RHI::DisplayHDRInfo hdr = br.gfx->MainSurface().GetDisplayHDRInfo();
        t["hdrCapable"]      = hdr.hdrCapable;
        t["hdrPeakNits"]     = hdr.maxLuminanceNits;
        t["hdrFullFrameNits"]= hdr.maxFullFrameNits;
        t["hdrBlackNits"]    = hdr.minLuminanceNits;
        // The paper white the HDR encode uses: the player's override, or Windows' SDR white level.
        t["hdrPaperWhiteNits"] = br.gfx->MainSurface().GetPaperWhiteNits();

        // The lighting tiers and whether this machine offers each (a tier above T0 needs inline RT);
        // `reason` is what a menu shows beside a greyed one.
        {
            sol::table tiers = lua.create_table();
            int n = 0;
            for (FramePlan::Tier tier : Settings::kLightingTiers)
            {
                const char* reason = Settings::LightingTierUnavailableReason(tier, br.gfx->GetCapabilities().inlineRaytracing);
                sol::table row = lua.create_table();
                row["key"]       = std::string(Settings::LightingTierKey(tier));
                row["summary"]   = std::string(Settings::LightingTierSummary(tier));
                row["needsRt"]   = Settings::LightingTierNeedsRT(tier);
                row["available"] = reason == nullptr;
                row["reason"]    = std::string(reason ? reason : "");
                tiers[++n] = row;
            }
            t["lightingTiers"] = tiers;
        }

        // Streamline features, each with a human-readable reason string so a greyed-out
        // control can explain itself: Streamline refusals are otherwise SILENT (slInit and
        // the options calls both succeed).
        auto& sl = SL::StreamlineBackend::Get();
        t["streamline"]     = sl.IsInitialized();
        t["dlss"]           = sl.IsFeatureSupported(SL::Feature::DLSS);
        t["dlssRR"]         = sl.IsFeatureSupported(SL::Feature::DLSS_RR);
        t["dlssRRActive"]   = br.renderer->IsRayReconstructionResolved();
        const char* rrReason=br.renderer->GetRayReconstructionInertReason();
        t["dlssRRReason"]   = rrReason?rrReason:"ready";
        t["dlssReason"]     = sl.GetFeatureStatusText(SL::Feature::DLSS);
        t["frameGen"]       = sl.IsFeatureSupported(SL::Feature::DLSS_G);
        t["frameGenReason"] = sl.GetFeatureStatusText(SL::Feature::DLSS_G);
        t["reflex"]         = sl.IsFeatureSupported(SL::Feature::Reflex);
        t["reflexReason"]   = sl.GetFeatureStatusText(SL::Feature::Reflex);

        // Frame-Generation limits, and the live request-vs-reality split.
        //
        // numFramesToGenerateMax is a HARD SDK limit: a multiplier past it fails
        // slDLSSGSetOptions, which turns FG OFF rather than degrading, so a menu truncates
        // its options to frameGenMax and offers "Dynamic" only when dynamicMFG.
        //
        // Read through GraphicsDX12's cached accessors, never slDLSSGGetState: that query
        // RESETS the SDK's presents-since-last-query counter, and a per-frame UI calling it
        // drains the window the backend measures over, so FG reads as dead.
        int   fgMax = 1, fgActive = 0;
        bool  fgDynamic = false;
        float fgRatio = 0.f;
        const char* fgRefusal = "";
        if (auto* dx12 = dynamic_cast<GraphicsDX12*>(br.gfx))
        {
            const SL::FrameGenState& c = dx12->Surface().GetFrameGenCaps();
            fgMax     = static_cast<int>(c.numFramesToGenerateMax);
            fgDynamic = c.dynamicMFGSupported;
            fgActive  = dx12->Surface().GetFrameGenActive();
            fgRatio   = dx12->Surface().GetFrameGenPresentRatio();
            fgRefusal = dx12->Surface().GetFrameGenRefusal();
            t["displayRefreshHz"] = dx12->Surface().GetDisplayRefreshHz();
        }
        t["frameGenMax"]     = fgMax;        // 1 == 2x-capable only
        t["dynamicMFG"]      = fgDynamic;
        t["frameGenActive"]  = fgActive;     // EFFECTIVE, vs display.frameGen = the request
        t["frameGenRatio"]   = fgRatio;
        t["frameGenRefusal"] = fgRefusal;

        // Effective vs requested for the two other settings the engine may
        // silently demote. A menu showing only the request looks broken on
        // hardware that demoted; showing both is the diagnosis.
        t["aaModeEffective"]  = static_cast<int>(br.renderer->GetAAMode());
        t["aaModeRequested"]  = static_cast<int>(br.renderer->GetAAModeRequested());
        t["hdrModeEffective"] = static_cast<int>(br.gfx->MainSurface().GetHDROutputMode());
        t["renderWidth"]      = br.renderer->GetRenderWidth();
        t["renderHeight"]     = br.renderer->GetRenderHeight();
        t["displayWidth"]     = br.renderer->GetDisplayWidth();
        t["displayHeight"]    = br.renderer->GetDisplayHeight();

        // Resolution. The engine has no display-MODE enumeration (nothing calls
        // GetDisplayModeList), so a menu offers a preset list filtered against the desktop
        // size; anything larger is clamped to the monitor work area by
        // Window::ServicePendingSize anyway. Only meaningful while windowed (fullscreen is
        // borderless and matches the output): disable the row on `fullscreen`.
        t["canPickResolution"] = true;
        t["fullscreen"]        = br.window ? br.window->IsFullscreen() : false;
        if (br.window)
        {
            // Requested, so a row built while a boot-time resize is still latched
            // shows the size the player is about to get, not the one being replaced.
            t["windowWidth"]  = br.window->RequestedClientWidth();
            t["windowHeight"] = br.window->RequestedClientHeight();
        }
        if (auto* dx12 = dynamic_cast<GraphicsDX12*>(br.gfx))
        {
            t["desktopWidth"]  = dx12->Surface().GetDesktopWidth();
            t["desktopHeight"] = dx12->Surface().GetDesktopHeight();
            // The display snapshot: the exact refresh (59.94, not 60), Windows' SDR white level for this
            // monitor, and the snapshot's generation (bumps on every change the engine saw).
            const Plat::DisplayMonitorPtr display = dx12->Surface().Display();
            t["displayRefreshExact"]  = display->refresh.Hz();
            t["displaySdrWhiteNits"]  = display->sdrWhiteNits;
            t["displayGeneration"]    = static_cast<double>(display->generation);
            t["displayLooks"]         = dx12->Surface().DisplayLooks();
        }
        // Fullscreen is BORDERLESS and exclusive is unavailable by construction
        // (ALLOW_TEARING is incompatible with exclusive and cannot be toggled
        // through ResizeBuffers).
        t["exclusiveFullscreen"] = false;

        // ReSTIR backends: per stage the request, what runs and why, as the shells last
        // reported it. A menu offering RTXDI must show the fallback, and a pack must assert
        // the EFFECTIVE backend, never the request.
        sol::table restir = lua.create_table();
        for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Stage::Count); ++i)
        {
            const auto stage = static_cast<ReSTIR::Stage>(i);
            const ReSTIR::BackendStatus st = br.renderer->GetRestirBackendStatus(stage);
            sol::table row = lua.create_table();
            row["requested"]     = std::string(st.requestedName);
            row["effective"]     = std::string(st.effectiveName);
            row["reason"]        = std::string(st.reason);
            row["setting"]       = std::string(ReSTIR::BackendName(br.renderer->GetRestirBackend(stage)));
            row["pipelineMs"]    = st.pipelineMs;
            row["residentBytes"] = static_cast<double>(st.residentBytes);
            row["ignored"]       = st.ignored;
            row["legal"]         = ReSTIR::LegalBackends(stage);
            std::string available;
            for (uint8_t b = 0; b < static_cast<uint8_t>(ReSTIR::Backend::Count); ++b)
                if (br.renderer->IsRestirBackendAvailable(stage, static_cast<ReSTIR::Backend>(b)))
                { if (!available.empty()) available += '|'; available += ReSTIR::BackendName(static_cast<ReSTIR::Backend>(b)); }
            row["available"]     = available;
            // The date until which the requested core stays selectable here while it is being
            // retired, "" when it is not retiring.
            const char* retiring = ReSTIR::RetirementDate(stage, br.renderer->GetRestirBackend(stage));
            row["retiring"]      = std::string(retiring ? retiring : "");
            std::string key = ReSTIR::StageName(stage);
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            // GI's SHaRC radiance cache: the request, whether it runs and why not (the shell's
            // report).
            if (stage == ReSTIR::Stage::GI)
            {
                const Renderer::GISharcStatus sc = br.renderer->GetRestirGISharcStatus();
                sol::table sharc = lua.create_table();
                sharc["requested"] = sc.requested;
                sharc["setting"]   = br.renderer->GetRestirGISharc();
                sharc["effective"] = sc.effective;
                sharc["reason"]    = std::string(sc.reason ? sc.reason : "");
                sharc["mode"]      = std::string(sc.mode == 1u ? "multi" : "parity");
                sharc["bytes"]     = static_cast<double>(sc.bytes);
                sharc["clears"]    = sc.clears;
                row["sharc"] = sharc;
            }
            // The denoiser: the setting and what the shell runs after the NRD flags and ray
            // reconstruction.
            if (stage == ReSTIR::Stage::DI || stage == ReSTIR::Stage::GI)
            {
                const bool di = stage == ReSTIR::Stage::DI;
                sol::table denoiser = lua.create_table();
                denoiser["setting"]   = std::string(ReSTIR::DenoiserName(di ? br.renderer->GetRestirDIDenoiser()
                                                                            : br.renderer->GetRestirGIDenoiser()));
                denoiser["effective"] = std::string(di ? br.renderer->GetRestirDIEffectiveDenoiser()
                                                       : br.renderer->GetRestirGIEffectiveDenoiser());
                row["denoiser"] = denoiser;
            }
            // x1's direct light on the RTXDI PT core: the setting and what the core runs after
            // the CLI pins and auto's rule.
            if (stage == ReSTIR::Stage::PT)
            {
                sol::table direct = lua.create_table();
                direct["setting"]   = std::string(PTRtxdi::DirectLightName(br.renderer->GetPTDirectLight()));
                direct["effective"] = std::string(PTRtxdi::DirectLightName(PTRtxdi::EffectiveDirectLight()));
                row["directLight"] = direct;
            }
            restir[key] = row;
        }
        sol::table compare = lua.create_table();
        compare["requested"] = std::string(ReSTIR::CompareName(br.renderer->GetRestirCompare()));
        compare["effective"] = std::string(ReSTIR::CompareName(br.renderer->GetRestirCompareEffective()));
        compare["reason"]    = std::string(br.renderer->GetRestirCompareReason());
        restir["compare"] = compare;
        t["restir"] = restir;
        return t;
    }

    // "di" / "gi" / "pt", case-insensitive.
    bool ParseStage(const std::string& token, ReSTIR::Stage& out)
    {
        for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Stage::Count); ++i)
        {
            const char* name = ReSTIR::StageName(static_cast<ReSTIR::Stage>(i));
            if (token.size() == 2 && std::toupper(static_cast<unsigned char>(token[0])) == name[0]
                && std::toupper(static_cast<unsigned char>(token[1])) == name[1])
            { out = static_cast<ReSTIR::Stage>(i); return true; }
        }
        return false;
    }
}   // namespace

void RegisterLuaSettingsBindings(sol::state& lua,
                                 Renderer& renderer,
                                 IGraphicsDevice& gfx,
                                 Plat::Window* window)
{
    // One-time schema sanity: the AA label list must cover every AAMode, or a
    // menu built from options() renders a blank entry the moment another panel
    // selects the missing mode. Checked at runtime rather than static_assert
    // because the labels live in the .inl as text.
    if (const SettingDesc* aa = Find("antialiasing.mode"))
    {
        ENGINE_CHECK(aa->labels.size() == static_cast<size_t>(Renderer::kMaxAAMode) + 1,
                     "EngineSettingsSchema.inl: antialiasing.mode has %zu labels but "
                     "Renderer::kMaxAAMode implies %d — add the missing label(s) or a "
                     "menu built from Settings.options() will show a blank entry",
                     aa->labels.size(), static_cast<int>(Renderer::kMaxAAMode) + 1);
    }

    const auto* rp = Find("features.renderPath");
    ENGINE_CHECK(rp && rp->labels.size() == static_cast<size_t>(Renderer::RenderPath::Count),
                 "EngineSettingsSchema.inl: features.renderPath must have exactly %u labels",
                 static_cast<unsigned>(Renderer::RenderPath::Count));
    auto br = std::make_shared<Bridge>();
    br->renderer = &renderer;
    br->gfx      = &gfx;
    br->window   = window;
    br->SeedFromEngine();
    g_bridge = br;   // the pipe's `setting` verb (ServeSettingVerb)

    sol::table s = lua.create_named_table("Settings");

    // ---- Read ----
    LuaDoc::Fn(s, "Settings", "get", [br, &lua](const std::string& path) -> sol::object
    {
        const SettingDesc* d = Find(path);
        if (!d)
        {
            LOG_WARNING("Settings.get: unknown path '%s'", path.c_str());
            return sol::lua_nil;
        }
        return ToLua(lua, *d, Settings::ValueOf(*d, br->pending));
    },
        { .doc = "A player setting by its path (\"display.vsync\"), as requested so far; nil for an unknown path.", .params = { { .name = "path", .type = "SettingKey" } } });

    // ---- Write (pending only: nothing reaches the engine until apply) ----
    LuaDoc::Fn(s, "Settings", "set", [br](const std::string& path, sol::object v) -> bool
    {
        const SettingDesc* d = Find(path);
        if (!d)
        {
            LOG_WARNING("Settings.set: unknown path '%s'", path.c_str());
            return false;
        }
        // The row's rules (type, NaN, clamp, index or label, the offered
        // values) are Settings::Coerce's, shared with every other writer.
        const Settings::Request q = ToRequest(v);
        Settings::Value val;
        switch (Settings::Coerce(*d, q, val))
        {
            case Settings::Refusal::None:
                Settings::PutValue(*d, val, br->pending);
                return true;
            case Settings::Refusal::WrongType:
                return false;
            case Settings::Refusal::NaN:
                LOG_WARNING("Settings.set('%s'): refused NaN", path.c_str());
                return false;
            case Settings::Refusal::OutOfRange:
                LOG_WARNING("Settings.set('%s'): value out of range (0..%zu)",
                            path.c_str(), d->labels.size() ? d->labels.size() - 1 : 0);
                return false;
            case Settings::Refusal::NotOffered:
                if (d->kind == Kind::String)
                    LOG_WARNING("Settings.set('%s'): '%s' is not one of the offered values",
                                path.c_str(), q.text.c_str());
                else
                    LOG_WARNING("Settings.set('%s'): %d is not one of the offered values",
                                path.c_str(), static_cast<int>(std::lround(q.number)));
                return false;
        }
        return false;
    },
        { .doc = "Request a player setting by its path; nothing reaches the engine until Settings.apply(). false for an unknown path or a value of the wrong kind.", .params = { { .name = "path", .type = "SettingKey" }, { .name = "value" } }, .effect = LuaDoc::Effect::Settings });

    // ---- Introspection ----
    s.set_function("options", [br, &lua](const std::string& path) -> sol::object
    {
        const SettingDesc* d = Find(path);
        if (!d) return sol::lua_nil;
        if (d->kind == Kind::Enum || (d->kind == Kind::String && !d->labels.empty()))
        {
            sol::table t = lua.create_table();
            for (size_t i = 0; i < d->labels.size(); ++i) t[i + 1] = d->labels[i];
            return t;
        }
        if (d->kind == Kind::Choice)
        {
            sol::table t = lua.create_table();
            for (size_t i = 0; i < d->values.size(); ++i) t[i + 1] = d->values[i];
            return t;
        }
        return sol::lua_nil;
    });

    s.set_function("range", [br](const std::string& path)
                   -> std::tuple<sol::optional<double>, sol::optional<double>>
    {
        const SettingDesc* d = Find(path);
        if (!d || (d->kind != Kind::Float && d->kind != Kind::Int))
            return { sol::nullopt, sol::nullopt };
        return { double(d->spec.lo), double(d->spec.hi) };
    });

    s.set_function("meta", [br, &lua](const std::string& path) -> sol::object
    {
        const SettingDesc* d = Find(path);
        if (!d) return sol::lua_nil;
        const char* kind = "bool";
        switch (d->kind)
        {
            case Kind::Bool:   kind = "bool";   break;
            case Kind::Int:    kind = "int";    break;
            case Kind::Float:  kind = "float";  break;
            case Kind::Enum:   kind = "enum";   break;
            case Kind::Choice: kind = "choice"; break;
            case Kind::String: kind = "string"; break;
        }
        const std::string full = d->path;
        const size_t dot = full.find('.');
        const std::string name = dot == std::string::npos ? full : full.substr(dot + 1);
        sol::table t = lua.create_table_with(
            "path",    full,
            "kind",    kind,
            "restart", (d->spec.flags & kSettingRestart) != 0,
            "group",   dot == std::string::npos ? std::string() : full.substr(0, dot),
            "name",    name,
            // A UI's label (the key when the row has no title) and tooltip.
            "title",   d->spec.title ? std::string(d->spec.title) : name);
        if (d->spec.tip) t["tip"] = std::string(d->spec.tip);
        return sol::make_object(lua, t);
    });

    // Settings.list([groupPrefix]): declaration order, the .inl's authoring order, so a
    // menu can lay a group out without a table of its own.
    s.set_function("list", [&lua](sol::optional<std::string> prefix) -> sol::table
    {
        sol::table t = lua.create_table();
        int n = 1;
        for (const auto& d : Schema())
        {
            if (prefix && *prefix != "")
            {
                const std::string p = *prefix;
                if (std::string(d.path).compare(0, p.size(), p) != 0) continue;
            }
            t[n++] = d.path;
        }
        return t;
    });

    // Settings.where(path): who set the row - its value in each layer that holds one,
    // the effective value its policy picks and that layer, and the last writes
    // (Settings/SettingsStore.h). The store records the writes it sees: an editor widget
    // or a scene writing its owner directly is not here, so the running value can
    // differ from `value`. A player setting, one of engine.json's or a pass row; for a
    // pass row `value` IS what the passes run.
    LuaDoc::Fn(s, "Settings", "where", [&lua](const std::string& path) -> sol::object
    {
        if (const SettingDesc* d = Find(path)) return WhereTable(lua, *d);
        if (const DevDesc* d = FindDev(path))  return WhereTable(lua, *d);
        if (const PassDesc* d = FindPass(path)) return WhereTable(lua, *d);
        LOG_WARNING("Settings.where: unknown path '%s'", path.c_str());
        return sol::lua_nil;
    },
        { .doc = "Who set a setting - a player one, an engine.json one or a pass one (domain): its value per layer, the effective value and its layer, and the last writes. nil for an unknown path.", .params = { { .name = "path", .type = "SettingKey|DevSettingKey|PassSettingKey" } } });

    // ---- Runtime layer ----
    // Settings.setRuntime / clearRuntime: a developer's live change, applied at once,
    // never saved (RuntimeWrite). The pipe's `setting set` / `setting clear` are the
    // same thing.
    LuaDoc::Fn(s, "Settings", "setRuntime", [br](const std::string& path, sol::object v) -> bool
    {
        Settings::Value val;
        if (const SettingDesc* d = Find(path))
        {
            if (!CoerceOrWarn(*d, v, val, "Settings.setRuntime")) return false;
            RuntimeWrite(*br, *d, &val, "Settings.setRuntime");
            return true;
        }
        if (const DevDesc* d = FindDev(path))
        {
            if (!CoerceOrWarn(*d, v, val, "Settings.setRuntime")) return false;
            DevRuntimeWrite(*br, *d, &val, "Settings.setRuntime");
            return true;
        }
        if (const PassDesc* d = FindPass(path))
        {
            if (!CoerceOrWarn(*d, v, val, "Settings.setRuntime")) return false;
            PassRuntimeWrite(*d, &val, "Settings.setRuntime");
            return true;
        }
        LOG_WARNING("Settings.setRuntime: unknown path '%s'", path.c_str());
        return false;
    },
        { .doc = "Set a setting NOW, in the Runtime layer: applied to the engine at once (only that setting), never saved, and on top of the files and the command line until Settings.clearRuntime. A player setting: Settings.get reads it. An engine.json one: only its row reaches the Renderer. A pass one: the pass reads it at its next read. false for an unknown path or a value the setting refuses.", .params = { { .name = "path", .type = "SettingKey|DevSettingKey|PassSettingKey" }, { .name = "value" } }, .effect = LuaDoc::Effect::Settings });

    LuaDoc::Fn(s, "Settings", "clearRuntime", [br](const std::string& path) -> bool
    {
        if (const SettingDesc* d = Find(path)) { RuntimeWrite(*br, *d, nullptr, "Settings.clearRuntime"); return true; }
        if (const DevDesc* d = FindDev(path))  { DevRuntimeWrite(*br, *d, nullptr, "Settings.clearRuntime"); return true; }
        if (const PassDesc* d = FindPass(path)) { PassRuntimeWrite(*d, nullptr, "Settings.clearRuntime"); return true; }
        LOG_WARNING("Settings.clearRuntime: unknown path '%s'", path.c_str());
        return false;
    },
        { .doc = "Drop a Settings.setRuntime value: the setting goes back to what the other layers say (Settings.where) and that is applied. false for an unknown path.", .params = { { .name = "path", .type = "SettingKey|DevSettingKey|PassSettingKey" } }, .effect = LuaDoc::Effect::Settings });

    // ---- Lifecycle ----
    s.set_function("dirty", [br]() -> bool
    {
        return br->rebase.has_value() || !SchemaEqual(br->pending, br->applied);
    });

    LuaDoc::Fn(s, "Settings", "apply", [br]() -> bool
    {
        br->Apply("Settings.apply");
        LOG_INFO("Settings.apply(): player settings pushed to the engine");
        return true;
    },
        { .doc = "Push the requested player settings to the engine.", .effect = LuaDoc::Effect::Settings });

    // Revert = re-apply the last applied state. It cannot undo the input half (ApplyTo
    // never touches Input: use Input.ResetBindings / Input.SetTuning), and anything
    // already latched to a frame boundary still lands, with the reverted value. Nothing
    // changed, so the store records nothing (the in-game menu reverts when it opens).
    s.set_function("revert", [br]() -> bool
    {
        br->pending = br->applied;
        br->rebase.reset();
        br->pending.ApplyTo(*br->renderer, *br->gfx, br->window);
        LOG_INFO("Settings.revert(): restored the last applied settings");
        return true;
    });

    // Reset: the settings without the player's own values, into `pending` only - NOT
    // applied, so "Reset" then "Cancel" is a no-op as a player expects. apply() commits
    // it and clears the Player layer. Each row reads what the other layers give it: the
    // default unless a -set: or a Runtime value holds it; for a group's rows, the level
    // the reset leaves (High unless the command line holds one).
    s.set_function("reset", [br]() -> bool
    {
        // CaptureInputFrom keeps the live rebinds: resetting graphics must not
        // silently wipe the player's controls, and Input.ResetBindings is the
        // separate, explicit button for that.
        EngineSettings def{};
        def.CaptureInputFrom(br->pending.input);
        def.input = br->pending.input;
        const std::vector<std::optional<Settings::Value>> none(Schema().size());
        Settings::Scalability::ProjectIfPlayer(def, none);
        br->pending = def;
        br->rebase  = Bridge::Rebase{ def, std::nullopt, {} };
        return true;
    });

    // Read-modify-write (SavePlayerChoices -> EditEngineSettingsFile), never
    // CaptureFrom + Save: CaptureFrom does not read every field, and the `input` block
    // belongs to Input.SaveBindings - loading first and overwriting only the schema rows
    // means an "apply graphics" button cannot rewrite a rebind.
    // What is saved is the player's own choices (the store's Player layer) plus what the
    // menu changed and has not applied; a value another layer holds (a -set:, a Runtime
    // write, a default) is not the player's and stays out of the file. An unapplied
    // reset / reload saves what its apply will leave in the Player layer
    // (PlayerAfterApply, the quality levels' rule included).
    s.set_function("save", [br]() -> bool
    {
        const auto rows = Schema();
        const auto player = br->PlayerAfterApply();
        EngineSettings values;
        std::vector<uint8_t> chosen(rows.size(), 0);
        for (size_t i = 0; i < rows.size(); ++i)
            if (player[i]) { chosen[i] = 1; Settings::PutValue(rows[i], *player[i], values); }
        const bool ok = SavePlayerChoices(values, chosen);
        if (!ok)
            LOG_ERROR("Settings.save(): could not write '%s'", SettingsFilePath().c_str());
        else
            LOG_INFO("Settings.save(): wrote '%s'", SettingsFilePath().c_str());
        return ok;
    });

    // settings.json -> pending (does not apply). Lets a menu offer "discard my
    // unsaved changes and go back to what is on disk", which is a different
    // thing from revert() ("go back to what is running").
    s.set_function("reload", [br]() -> bool
    {
        EngineSettings onDisk{};
        SettingsLoadInfo info;
        const bool found = LoadEngineSettings(SettingsFilePath(), onDisk, &info);
        // Keep the live input block: the file's input half is owned by Input.*
        // and re-applying it from here would fight that owner.
        onDisk.input          = br->pending.input;
        onDisk.inputOverrides = br->pending.inputOverrides;
        // Every row as it will read once the apply has replaced the Player layer with the
        // file's choices (PlayerRowsToRecord): a row the file does not hold reads what the
        // other layers give it - for a group's rows, the file's level.
        EngineSettings values = onDisk;
        const auto schema = Schema();
        const std::vector<uint8_t> chosen = PlayerRowsToRecord(onDisk, info);
        std::vector<std::optional<Settings::Value>> player(schema.size());
        for (size_t i = 0; i < schema.size() && i < chosen.size(); ++i)
            if (chosen[i]) player[i] = Settings::ValueOf(schema[i], onDisk);
        Settings::Scalability::ProjectIfPlayer(values, player);
        br->pending = values;
        br->rebase  = Bridge::Rebase{ values, info, onDisk };
        return found;
    });

    LuaDoc::Fn(s, "Settings", "caps", [br, &lua]() -> sol::table { return BuildCaps(lua, *br); },
        { .doc = "What this machine and build can do and what is in effect: render path, ray tracing, upscalers, ReSTIR backends and their reasons." });

    // ---- The lighting tier (Settings/LightingTier.h) ----
    // A preset of player rows: requested like Settings.set, one row at a time into
    // `pending`, and applied by Settings.apply(). The tier the frame runs is derived
    // from the rows (lightingTier).
    LuaDoc::Fn(s, "Settings", "setLightingTier", [br](const std::string& name) -> bool
    {
        FramePlan::Tier tier;
        if (!Settings::ParseLightingTier(name, tier))
        {
            LOG_WARNING("Settings.setLightingTier: unknown tier '%s' (T0, T1, T2, T3, T4, Ref)", name.c_str());
            return false;
        }
        for (const Settings::TierWrite& w : Settings::LightingTierWrites(tier))
        {
            const SettingDesc* d = Find(w.path);
            if (!d) continue;
            Settings::Value v;
            if (d->kind == Kind::Bool) v.b = w.value != 0;
            else                       v.i = w.value;
            Settings::PutValue(*d, v, br->pending);
        }
        return true;
    },
        { .doc = "Request a lighting tier - T0 (raster), T1 (+DDGI), T2 (hybrid RT), T3 (ReSTIR), T4 (real-time path "
                 "tracer), Ref (reference path tracer): the render path and the RT / AO / DDGI switches it names go "
                 "into the requested settings like Settings.set, and Settings.apply() applies them. false for an "
                 "unknown name.",
          .params = { { .name = "tier" } }, .effect = LuaDoc::Effect::Settings });

    // ---- The quality levels (Settings/Scalability.h) ----
    // A level is a player row (scalability.<group>); choosing one here writes the level
    // and the group's rows at its values into `pending`, as setLightingTier writes its
    // rows. Settings.apply() then lets go of the group's overrides and records the level
    // (ApplyChoices).
    LuaDoc::Fn(s, "Settings", "scalability", [br, &lua]() -> sol::table
    {
        namespace Sc = Settings::Scalability;
        sol::table t      = lua.create_table();
        sol::table custom = lua.create_table();
        sol::table groups = lua.create_table();
        const bool anyRows = Sc::RowCount() > 0;
        int common = -2;   // -2: no group yet, -1: two levels differ
        for (size_t g = 0; g < Sc::kGroupCount; ++g)
        {
            const Settings::Row* lr = Sc::LevelRow(g);
            const int level = lr ? Settings::ValueOf(*lr, br->pending).i : Sc::kHigh;
            bool off = false;
            for (const Sc::Entry& e : Sc::Rows(g))
                if (level >= 0 && level < Sc::kLevelCount
                    && !Settings::SameValue(*e.row, Settings::ValueOf(*e.row, br->pending), e.values[level]))
                    off = true;
            t[Sc::GroupName(g)]      = std::string(Sc::LevelName(level));
            custom[Sc::GroupName(g)] = off;
            groups[g + 1]            = std::string(Sc::GroupName(g));
            if (!anyRows || !Sc::Rows(g).empty())
                common = (common == -2 || common == level) ? level : -1;
        }
        t["overall"] = std::string(common >= 0 ? Sc::LevelName(common) : "Custom");
        t["custom"]  = custom;
        t["groups"]  = groups;
        return t;
    },
        { .doc = "The quality levels as requested so far: each group's level by name (shadows, gi, reflections, post, "
                 "textures, effects, viewDistance, aa = \"Low\" .. \"Cinematic\"), overall (the level every group with "
                 "rows shares, else \"Custom\"), custom (per group: a row of it requested at a value its level does not "
                 "give - an override, or a -set: holding it) and groups (the names in order)." });

    LuaDoc::Fn(s, "Settings", "setScalability", [br](const std::string& group, sol::object level) -> bool
    {
        namespace Sc = Settings::Scalability;
        int l = -1;
        if (level.is<std::string>())
        {
            if (!Sc::ParseLevel(level.as<std::string>(), l)) l = -1;
        }
        else if (level.is<double>())
        {
            const double d = level.as<double>();
            l = (d >= 0.0 && d < Sc::kLevelCount && d == std::floor(d)) ? static_cast<int>(d) : -1;
        }
        if (l < 0)
        {
            LOG_WARNING("Settings.setScalability: unknown level (Low, Medium, High, Ultra, Cinematic or 0..4)");
            return false;
        }
        std::vector<size_t> groups;
        std::string low = group;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low == "overall")
            for (size_t g = 0; g < Sc::kGroupCount; ++g) groups.push_back(g);
        else if (size_t g = 0; Sc::ParseGroup(group, g))
            groups.push_back(g);
        else
        {
            LOG_WARNING("Settings.setScalability: unknown group '%s' (overall, shadows, gi, reflections, post, "
                        "textures, effects, viewDistance, aa)", group.c_str());
            return false;
        }
        for (const size_t g : groups)
        {
            if (const Settings::Row* lr = Sc::LevelRow(g))
                Settings::PutValue(*lr, Settings::Value{ .i = l }, br->pending);
            for (const Sc::Entry& e : Sc::Rows(g))
                Settings::PutValue(*e.row, e.values[l], br->pending);
        }
        return true;
    },
        { .doc = "Request a quality level for one group or every one (\"overall\"): the level and the group's rows at "
                 "its values go into the requested settings like Settings.set, and Settings.apply() applies them - "
                 "the group's earlier overrides let go. Any case; a level by name or 0..4. false for an unknown "
                 "group or level.",
          .params = { { .name = "group" }, { .name = "level" } }, .effect = LuaDoc::Effect::Settings });

    LuaDoc::Fn(s, "Settings", "lightingTier", [br]() -> std::string
    {
        return FramePlan::TierName(br->renderer->GamePlan());
    },
        { .doc = "The lighting tier this frame's plan derives from the applied switches: T0..T4 or Ref; partial "
                 "combinations read T2* (not all of RT reflections, RT shadows, RTAO and DDGI) or T3:DI / T3:GI "
                 "(one ReSTIR stage). Planned at the start of the frame's render phase, so a Settings.apply() shows "
                 "here on the next frame." });
    // engine.json (editor hosts only): what runs, read-modify-write; a file that cannot
    // be read is refused. The save records every row in the developer store's Project
    // layer.
    s.set_function("saveDeveloper", [br]() -> bool
    {
        return EditEngineDevSettingsFile([&](EngineDevSettings& dev) { dev.CaptureFrom(*br->renderer); },
                                         "Settings.saveDeveloper", /*requireFile*/true);
    });
    // engine.json onto the Renderer again, recorded (ReloadEngineDevSettings);
    // false, and nothing applied, when the file cannot be read.
    s.set_function("reloadDeveloper", [br]() -> bool
    {
        return ReloadEngineDevSettings(*br->renderer, "Settings.reloadDeveloper", /*requireFile*/true);
    });
    // Developer PT controls, deliberately separate from the player settings:
    // Runtime-layer writes of engine.json's rows (DevWrite; the row's own hook calls the
    // setter). Persistence is the editor's engine.json Save / saveDeveloper.
    // setRestirBackend (developer, like setPTArea): applied at the next frame's service
    // point; caps().restir says what runs. False for an unknown stage or a name the
    // stage does not take.
    s.set_function("setRestirBackend", [br](const std::string& stage, const std::string& name) -> bool
    {
        ReSTIR::Stage st;
        ReSTIR::Backend backend;
        if(!ParseStage(stage,st) || !ReSTIR::ParseBackend(st,name,backend)) return false;
        Settings::Value v;
        v.s = ReSTIR::BackendName(backend);
        DevWrite(*br, st == ReSTIR::Stage::DI ? &EngineDevSettings::restirBackendDI
                    : st == ReSTIR::Stage::GI ? &EngineDevSettings::restirBackendGI
                                              : &EngineDevSettings::restirBackendPT, v, "Settings.setRestirBackend");
        return true;
    });
    s.set_function("setRestirCompare", [br](const std::string& name) -> bool
    {
        ReSTIR::Compare compare;
        if(!ReSTIR::ParseCompare(name,compare)) return false;
        Settings::Value v;
        v.s = ReSTIR::CompareName(compare);
        DevWrite(*br, &EngineDevSettings::restirCompare, v, "Settings.setRestirCompare");
        return true;
    });
    // true selects Path Trace (Real-time) - RTXDI's ReSTIR PT with the SHaRC cache -
    // false Path Trace (Reference), the engine path tracer: what
    // Settings.set("features.renderPath", 5 / 4) + apply() do, but applied at once (the
    // pending and applied snapshots follow, so a later apply() keeps it).
    // caps().restir.pt says which core runs.
    s.set_function("setPTRealtime", [br](bool on) -> bool
    {
        const auto path = on ? Renderer::RenderPath::PathTraceRealtime : Renderer::RenderPath::PathTrace;
        br->pending.renderPath = br->applied.renderPath = static_cast<int>(path);
        br->renderer->SetRenderPath(path);
        // A developer setter: it writes the Runtime layer.
        if (const SettingDesc* d = Find("features.renderPath"))
        {
            Settings::Value v;
            v.i = static_cast<int>(path);
            Settings::SettingsStore::Get().Write(*d, Settings::Layer::Runtime, v, "Settings.setPTRealtime");
        }
        return true;
    });
    // SHaRC on ReSTIR GI (engine.json raytracing.restirGiSharc / restirGiSharcMode): on /
    // off and optionally the content mode "parity" | "multi"; applied at the next
    // frame's service point, where caps().restir.gi.sharc says what runs. False for an
    // unknown mode.
    s.set_function("setRestirGISharc", [br](bool on, sol::optional<std::string> mode) -> bool
    {
        if (mode && *mode != "parity" && *mode != "multi") return false;
        Settings::Value v;
        v.b = on;
        DevWrite(*br, &EngineDevSettings::restirGiSharc, v, "Settings.setRestirGISharc");
        if (mode)
        {
            Settings::Value m;
            m.s = *mode;
            DevWrite(*br, &EngineDevSettings::restirGiSharcMode, m, "Settings.setRestirGISharc");
        }
        return true;
    });
    // The DI / GI denoiser (engine.json raytracing.restirDiDenoiser / restirGiDenoiser):
    // "auto" | "relax" | "reblur" | "none", applied at the next frame's service point;
    // under ray reconstruction it is the denoiser before RR.
    // caps().restir.<di|gi>.denoiser says what runs. False for an unknown stage (DI and
    // GI only) or name.
    s.set_function("setRestirDenoiser", [br](const std::string& stage, const std::string& name) -> bool
    {
        ReSTIR::Stage st;
        ReSTIR::Denoiser denoiser;
        if(!ParseStage(stage,st) || st==ReSTIR::Stage::PT || !ReSTIR::ParseDenoiser(name,denoiser)) return false;
        Settings::Value v;
        v.s = ReSTIR::DenoiserName(denoiser);
        DevWrite(*br, st == ReSTIR::Stage::DI ? &EngineDevSettings::restirDiDenoiser : &EngineDevSettings::restirGiDenoiser,
                 v, "Settings.setRestirDenoiser");
        return true;
    });
    // Where the RTXDI PT core takes x1's direct light (engine.json
    // raytracing.ptDirectLight): "auto" | "nee" | "restir_di" | "unified"; the core is
    // remade at the next frame. caps().restir.pt.directLight says what runs. False for
    // an unknown name.
    s.set_function("setPTDirectLight", [br](const std::string& name) -> bool
    {
        PTRtxdi::DirectLight d;
        if(!PTRtxdi::ParseDirectLight(name.c_str(),d)) return false;
        Settings::Value v;
        v.s = PTRtxdi::DirectLightName(d);
        DevWrite(*br, &EngineDevSettings::ptDirectLight, v, "Settings.setPTDirectLight");
        return true;
    });
    s.set_function("setPTArea", [br](int mode,bool motion,int historyCap) -> bool
    {
        if(mode<0 || mode>2 || historyCap<1 || historyCap>64) return false;
        Settings::Value m, mo, h;
        m.i  = mode;
        mo.b = motion;
        h.i  = historyCap;
        DevWrite(*br, &EngineDevSettings::ptAreaMode, m, "Settings.setPTArea");
        DevWrite(*br, &EngineDevSettings::ptAreaMotion, mo, "Settings.setPTArea");
        DevWrite(*br, &EngineDevSettings::ptAreaHistory, h, "Settings.setPTArea");
        return true;
    });

    s.set_function("path", []() -> std::string { return SettingsFilePath(); });
}

namespace
{
    const char* kSettingUsage = "setting list [<prefix>] | get <path> | where <path> | set <path> <value> | clear <path>";

    // `setting get | where | set | clear` on one row of any document (player /
    // developer / pass); a refusal sets @p error / @p message instead.
    template <class S>
    nlohmann::json ServeRow(Bridge& br, const std::string& sub, const Settings::RowT<S>& d,
                            const std::string& value, std::string& error, std::string& message)
    {
        if (sub == "set")
        {
            if (value.empty())
            {
                error = "no_value";
                message = kSettingUsage;
                return {};
            }
            Settings::Value v;
            const Settings::Refusal why = Settings::Coerce(d, Settings::RequestFromText(d, value), v);
            if (why != Settings::Refusal::None)
            {
                error = "refused";
                message = std::string(d.path) + " refuses '" + value + "' (" + Settings::RefusalName(why) + ")";
                return {};
            }
            const bool applied = WriteRuntime(br, d, &v, "setting set (pipe)");
            nlohmann::json o = RowJson(d);
            o["applied"] = applied;
            return o;
        }
        if (sub == "clear")
        {
            const bool applied = WriteRuntime(br, d, nullptr, "setting clear (pipe)");
            nlohmann::json o = RowJson(d);
            o["applied"] = applied;
            return o;
        }
        return sub == "where" ? WhereJson(d) : RowJson(d);
    }
}

std::string ServeSettingVerb(const std::string& payload)
{
    nlohmann::json out;
    out["verb"] = "setting";
    auto fail = [&out](const char* error, const std::string& message) {
        out["ok"]      = false;
        out["error"]   = error;
        out["message"] = message;
        return out.dump();
    };
    const std::shared_ptr<Bridge> br = g_bridge.lock();
    if (!br)
        return fail("no_settings_bridge", "the Lua Settings bridge is not registered (yet)");

    // <sub> [<path> [<value ...>]]; the value is the rest of the line.
    std::istringstream in(payload);
    std::string sub, path, value;
    in >> sub >> path;
    std::getline(in, value);
    value.erase(0, value.find_first_not_of(" \t"));
    if (sub.empty()) sub = "list";
    out["sub"] = sub;

    if (sub == "list")
    {
        // Every document: the player rows, engine.json's, the pass rows.
        nlohmann::json rows = nlohmann::json::array();
        for (const SettingDesc& d : Schema())
            if (path.empty() || std::string(d.path).rfind(path, 0) == 0)
                rows.push_back(RowJson(d));
        for (const DevDesc& d : Settings::DevRows())
            if (path.empty() || std::string(d.path).rfind(path, 0) == 0)
                rows.push_back(RowJson(d));
        for (const PassDesc& d : Settings::PassRows())
            if (path.empty() || std::string(d.path).rfind(path, 0) == 0)
                rows.push_back(RowJson(d));
        out["ok"]   = true;
        out["rows"] = std::move(rows);
        return out.dump();
    }
    if (sub != "get" && sub != "where" && sub != "set" && sub != "clear")
        return fail("unknown_subcommand", kSettingUsage);

    std::string error, message;
    nlohmann::json o;
    if (const SettingDesc* d = FindAnyCase(Schema(), path))
        o = ServeRow(*br, sub, *d, value, error, message);
    else if (const DevDesc* d = FindAnyCase(Settings::DevRows(), path))
        o = ServeRow(*br, sub, *d, value, error, message);
    else if (const PassDesc* d = FindAnyCase(Settings::PassRows(), path))
        o = ServeRow(*br, sub, *d, value, error, message);
    else
        return fail("unknown_path", "there is no setting '" + path + "' (setting list names them)");
    if (!error.empty())
        return fail(error.c_str(), message);
    o["verb"] = "setting"; o["sub"] = sub; o["ok"] = true;
    return o.dump();
}

}   // namespace Resource
