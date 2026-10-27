#include "System/CommandLine.h"
#include "Resource/EngineSettings.h"
#include "Settings/SettingsRows.h"      // the player rows: load / save walk them
#include "Settings/SettingsStore.h"     // the boot load records its Player layer
#include "Settings/SettingsRowJson.h"   // a row's JSON read / write / sanitize
#include "Settings/SetFlags.h"         // -set:<path>=<value> (the player half)
#include "Settings/Scalability.h"      // the quality levels: the boot writes their layer
#include "Graphics/IGraphicsDevice.h"  // the adapter: a first boot's levels

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "Platform/Window.h"
#include "System/Log.h"
#include "Diagnostics/OwnerAccounting.h"   // Diag::DrainCause
#include "System/PathUtil.h"
#include "System/EventBus.h"            // mixer volumes travel as events
#include "Audio/AudioEvents.h"          // BusVolumeChangedEvent + BusType
#include "Input/InputSystem.h"          // CaptureFrom pulls the live bindings
#include "Harness/ExitCodes.h"          // -set: refuses the run like -gipath

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

namespace
{
    // Renderer::AAMode <-> readable token: Settings::kAAModeCodec (the row's
    // codec, SettingsRows.cpp). Unknown tokens fall back to TAA. The peeks
    // below read antialiasing.mode the same way the load does.
    const char* AAModeToString(int m)                { return Settings::kAAModeCodec.toToken(m); }
    int         AAModeFromString(const std::string& s) { return Settings::kAAModeCodec.fromToken(s); }

    // Writable-root resolution: a path with a directory component is used verbatim (tools
    // and tests that want an explicit location); a bare name goes to the build's writable
    // root (the project directory in the editor, the per-user root in Game.exe). A bare name
    // must never resolve against the working directory: that is not the exe directory when
    // the game starts from a shortcut, the debugger or elevated (system32), and under
    // C:\Program Files a write is UAC-denied or VirtualStore'd. The rule lives here, not at
    // the call sites, so any bare name lands where App reads.
    std::string ResolveSettingsPath(const std::string& path)
    {
        if (path.find('/') != std::string::npos || path.find('\\') != std::string::npos)
            return path;
        return Sys::WritableDataDir() + "/" + path;
    }

    // Live mixer mirror. AudioEngine owns the bus volumes but exposes no getter, so the
    // audio rows' capture reads this mirror: a Save writes the volumes actually playing,
    // not the struct defaults. Subscribed once to the BusVolumeChangedEvent AudioSystem
    // consumes, it shadows every change, whoever published it (settings, Lua, an options
    // menu). Subscribed lazily, so a headless / tool run that never touches settings
    // installs no listener; EventBus::Clear() at shutdown drops it after the last capture.
    constexpr int kBusCount = static_cast<int>(Audio::BusType::Count);
    // A new BusType would leave the extra slots ZERO-initialised (= silence) and
    // unserialised. Fail the build instead, and give the bus its field and row.
    static_assert(kBusCount == 6, "Audio::BusType changed — add the bus's field "
                                  "to EngineSettings.h and its audio.* row to "
                                  "EngineSettingsSchema.inl");
    float g_busVolume[kBusCount] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    bool  g_busMirrorInstalled = false;

    void EnsureBusMirror()
    {
        if (g_busMirrorInstalled) return;
        g_busMirrorInstalled = true;
        (void)EventBus::Get().Subscribe<Audio::BusVolumeChangedEvent>(
            [](const Audio::BusVolumeChangedEvent& e)
            {
                const int i = static_cast<int>(e.bus);
                if (i >= 0 && i < kBusCount) g_busVolume[i] = e.volume;
            });
    }

    float BusMirror(Audio::BusType bus)
    {
        const int i = static_cast<int>(bus);
        return (i >= 0 && i < kBusCount) ? g_busVolume[i] : 1.0f;
    }

    // The key's value, or @p fallback when it is absent or mistyped (a
    // mistyped one warns; Settings::TryValue). The input block's reads.
    template <class T>
    T ValueOr(const nlohmann::json& obj, const char* key, const T& fallback)
    {
        T v = fallback;
        Settings::TryValue(obj, key, v, "EngineSettings");
        return v;
    }

    // ---- The player rows (Settings/SettingsRows.h) ----
    // Save and load walk the rows of EngineSettingsSchema.inl. Each row's JSON value, read
    // and load sanitize live in the row (Settings/SettingsRowJson.h); the two cross-field
    // rules are SanitizePairs. tests/unit/test_settings_roundtrip.py holds the file format
    // byte for byte.
    using Settings::RowJson;
    using Settings::ValueJson;
    using Settings::SanitizeRow;
    constexpr const char* kWho = "EngineSettings";   // the warnings' prefix

    // The two rules that span rows.
    void SanitizePairs(Resource::EngineSettings& out)
    {
        // 0 is the meaningful "leave the window alone" value, so only a NEGATIVE
        // or absurdly small size is corrected - and both axes are cleared
        // together, because half a size is not a size. No upper bound:
        // Window::ServicePendingSize clamps to the monitor work area at apply
        // time, the only place that knows what the monitor is.
        if (out.windowWidth < 0 || out.windowHeight < 0 ||
            (out.windowWidth > 0 && out.windowWidth  < 320) ||
            (out.windowHeight > 0 && out.windowHeight < 240) ||
            (out.windowWidth > 0) != (out.windowHeight > 0))
        {
            out.windowWidth = out.windowHeight = 0;
        }
        // Negative headroom is degenerate: the tone curve would roll off BELOW
        // the white point. The clamped ranges overlap (paper white tops out at
        // 480, peak starts at 400), so this is reachable from a legal-looking
        // file. Raise PEAK rather than lowering paper white - paper white is the
        // player's comfort choice, peak is a claim about the panel.
        if (out.hdrPeakNits < out.hdrPaperWhiteNits)
            out.hdrPeakNits = out.hdrPaperWhiteNits;
    }
}

namespace Resource
{
    // The mirror above, made reachable (the contract is at the declaration).
    float LiveBusVolume(Audio::BusType bus)
    {
        EnsureBusMirror();
        return BusMirror(bus);
    }

    // Published, not called: AudioEngine is not reachable from here. EventBus::Publish
    // QUEUES, which makes the boot safe: App::BootRenderer calls ApplyTo before InitAudio,
    // and the events wait in the channel until AudioTickSystem's first DispatchOne, when
    // AudioSystem has subscribed. Clamped to [0, 2] so a hand-edited file cannot blow out
    // the mastering voice.
    void PublishBusVolume(Audio::BusType bus, float volume)
    {
        EnsureBusMirror();
        Audio::BusVolumeChangedEvent ev;
        ev.bus    = bus;
        ev.volume = std::clamp(volume, 0.0f, 2.0f);
        EventBus::Get().Publish(ev);
        // Seed the mirror now instead of waiting for the dispatch, so a
        // CaptureFrom before the first frame still round-trips.
        const int i = static_cast<int>(bus);
        if (i >= 0 && i < kBusCount) g_busVolume[i] = ev.volume;
    }


// ==== Apply / Capture ====

// Every player row's apply hook, phase by phase (Settings::ApplyPlayerRows;
// the hooks and the reasons for each live in EngineSettingsSchema.inl).
void EngineSettings::ApplyTo(Renderer& r, IGraphicsDevice& gfx, Plat::Window* window) const
{
    gfx.DrainCpuFrame(static_cast<uint8_t>(Diag::DrainCause::Settings));
    Settings::ApplyCtx c{ r, gfx, window };
    Settings::ApplyPlayerRows(*this, c);
}

void EngineSettings::CaptureFrom(Renderer& r, IGraphicsDevice& gfx, const Plat::Window* window)
{
    // Input first, and unconditionally: without the live bindings captured here, a Save
    // that captures into a fresh struct would wipe the player's rebinds.
    CaptureInputFrom(Input::Get().Bindings());

    Settings::CaptureCtx c{ r, gfx, window };
    Settings::CapturePlayerRows(*this, c);
}

// ==== Save / Load (JSON, nlohmann) ====

const std::string& SettingsFilePath()
{
    // WritableDataDir, not UserDataDir: the project directory in the editor (settings.json
    // stays in the repo, versioned and diffable), the per-user root in Game.exe; main.cpp
    // makes the choice. Cached: the root is final before Logger::Initialize.
    // -settings=<path> wins: an automated run gets an explicit profile, so its display
    // configuration (and the composition format a screenshot depends on) is deliberate;
    // SaveEngineSettings never rewrites it, so a run cannot change the next one's start.
    static const std::string s_path = []
    {
        const std::string override = Sys::CommandLine::Get().Get("-settings");
        if (!override.empty()) return override;
        return Sys::WritableDataDir() + "/settings.json";
    }();
    return s_path;
}

std::string SerializeEngineSettings(const EngineSettings& s, std::span<const uint8_t> rows)
{
    nlohmann::json j;
    j["_comment"] = "Player graphics/quality options. Safe to hand-edit; the engine "
                    "re-writes it when you change settings in-game. Deleting it resets "
                    "everything to defaults. This is NOT the project manifest (game.json).";
    j["version"] = kSettingsVersion;
    // Every player row at its "section.key" (EngineSettingsSchema.inl, RowJson) - with
    // @p rows, the chosen ones. The objects' keys are sorted (nlohmann::json is a
    // std::map), so the order the rows are written in never reaches the file.
    const auto all = Settings::PlayerRows();
    for (size_t i = 0; i < all.size(); ++i)
        if (rows.empty() || (i < rows.size() && rows[i]))
            j[all[i].section][all[i].key] = RowJson(all[i], s);
    // The input block is an array, not a row (see the end of the .inl).
    {
        nlohmann::json ji;
        ji["lookRateDegPerSec"] = s.input.lookRateDegPerSec;
        ji["mouseLookScale"]    = s.input.mouseLookScale;
        ji["stickDeadzone"]     = s.input.stickDeadzone;
        ji["stickCurveExp"]     = s.input.stickCurveExp;
        ji["triggerThreshold"]  = s.input.triggerThreshold;
        ji["padInvertLookY"]    = s.input.padInvertLookY;
        ji["mouseInvertLookY"]  = s.input.mouseInvertLookY;
        ji["gamepadEnabled"]    = s.input.gamepadEnabled;

        nlohmann::json jb = nlohmann::json::array();
        for (const auto& o : s.inputOverrides)
        {
            jb.push_back({
                { "action", o.action },
                { "device", o.device },
                { "slot",   o.slot   },
                { "path",   o.path   },
            });
        }
        ji["bindings"] = std::move(jb);
        j["input"] = std::move(ji);
    }

    return j.dump(2);
}

bool EditEngineSettingsFile(const std::function<void(EngineSettings&, std::vector<uint8_t>& rows)>& edit)
{
    // READ-MODIFY-WRITE, always against SettingsFilePath(): a writer that captured into a
    // fresh struct would write the struct defaults for every field it does not own, and a
    // bare "settings.json" would ignore -settings=.
    EngineSettings s;
    SettingsLoadInfo info;
    LoadEngineSettings(SettingsFilePath(), s, &info);   // absent file => defaults
    std::vector<uint8_t> rows = PlayerRowsToRecord(s, info);
    edit(s, rows);
    return SaveEngineSettings(s, SettingsFilePath(), rows);
}

bool SavePlayerChoices(const EngineSettings& values, std::span<const uint8_t> chosen)
{
    return EditEngineSettingsFile([&](EngineSettings& onDisk, std::vector<uint8_t>& rows)
    {
        const auto all = Settings::PlayerRows();
        for (size_t i = 0; i < all.size(); ++i)
        {
            rows[i] = (i < chosen.size() && chosen[i]) ? 1 : 0;
            if (rows[i]) Settings::PutValue(all[i], Settings::ValueOf(all[i], values), onDisk);
        }
    });
}

bool SaveCapturedEngineSettings(Renderer& r, IGraphicsDevice& gfx, const Plat::Window* window)
{
    // The live values over the store's: a row the player changed outside the store (the
    // window size, a fullscreen toggle) is recorded in the Player layer before the save.
    // Starting from the store's values, a field CaptureFrom does not own (ui.language; the
    // windowed size while fullscreen) compares equal and keeps its value; the input block
    // is the live bindings.
    EngineSettings live;
    ProjectStoreValues(live);
    live.CaptureFrom(r, gfx, window);
    Settings::SettingsStore& st = Settings::SettingsStore::Get();
    for (const auto& row : Settings::PlayerRows())
    {
        const Settings::Value v = Settings::ValueOf(row, live);
        if (!Settings::SameValue(row, v, st.OwnerValue(row)))
            st.Write(row, Settings::Layer::Player, v, "Save (captured)");
    }
    return EditEngineSettingsFile([&](EngineSettings& onDisk, std::vector<uint8_t>& rows)
    {
        onDisk.input          = live.input;
        onDisk.inputOverrides = live.inputOverrides;
        const auto all = Settings::PlayerRows();
        for (size_t i = 0; i < all.size(); ++i)
        {
            const auto& player = st.At(all[i], Settings::Layer::Player);
            rows[i] = player ? 1 : 0;
            if (player) Settings::PutValue(all[i], *player, onDisk);
        }
    });
}

std::vector<uint8_t> PlayerRowsToRecord(const EngineSettings& s, const SettingsLoadInfo& info)
{
    const auto rows = Settings::PlayerRows();
    const EngineSettings defaults{};
    std::vector<uint8_t> mask(rows.size(), 0);
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const bool fromFile  = i < info.rowFromFile.size() && info.rowFromFile[i];
        const bool off       = !Settings::SameValue(rows[i], Settings::ValueOf(rows[i], s),
                                                    Settings::ValueOf(rows[i], defaults));
        // A version-3 document holds the player's choices; an older one held
        // every row, so only a value off its default can have been chosen.
        mask[i] = ((info.version >= 3 && fromFile) || off) ? 1 : 0;
    }
    return mask;
}

bool LoadBootEngineSettings(EngineSettings& out, const IGraphicsDevice* gfx)
{
    // The quality levels' table first: the store's observer is in place before a level is
    // recorded.
    Settings::Scalability::Load();
    SettingsLoadInfo info;
#if WITH_HARNESS
    // -autoexposure_default_on: a file that leaves postProcess.autoExposure out starts with auto exposure on
    // again (the reverse arm of the off default).
    if (Sys::CommandLine::Get().Has("-autoexposure_default_on"))
        out.ppAutoExposure = true;
#endif
    const bool found = LoadEngineSettings(SettingsFilePath(), out, &info);
    RecordLoadedPlayerRows(out, info, found ? info.loadedAs : SettingsFilePath(), /*replace*/false);
    ApplyCommandLineSettings(out, /*record*/true);
    // A first boot's levels from the adapter - before the Scalability layer is written, so
    // the restart rows they set (the CSM, the point cubes, anisotropy) reach Compile.
    if (gfx)
        Settings::Scalability::ApplyFirstBoot(gfx->GetAdapterInfo(), info);
    // Every group's rows at its level into the Scalability layer, then the owners are
    // handed the store's values: a row neither the file nor the command line holds follows
    // its level (High: its default).
    Settings::Scalability::WriteLayer();
    ProjectStoreValues(out);
    Settings::Scalability::LogBoot();
    return found;
}

void RecordLoadedPlayerRows(const EngineSettings& s, const SettingsLoadInfo& info,
                            std::string_view writer, bool replace)
{
    // The Player layer gets the player's choices (PlayerRowsToRecord) plus any value the
    // load's sanitize moved off its default (the window pair rule clears both sizes), so
    // the store's effective values equal @p s row for row while nothing else holds them -
    // and @p s is what ApplyTo pushes. A key not recorded stays out: `where` must not
    // credit settings.json with a compiled-in value.
    Settings::SettingsStore& st = Settings::SettingsStore::Get();
    const auto rows = Settings::PlayerRows();
    if (replace)
        for (const auto& r : rows) st.Clear(r, Settings::Layer::Player, writer);
    st.WriteRows(s, Settings::Layer::Player, writer, PlayerRowsToRecord(s, info));
}

void ProjectStoreValues(EngineSettings& s)
{
    const Settings::SettingsStore& st = Settings::SettingsStore::Get();
    for (const auto& r : Settings::PlayerRows())
        Settings::PutValue(r, st.OwnerValue(r), s);
}

int ApplyCommandLineSettings(EngineSettings& s, bool record)
{
    std::vector<std::string> errors;
    // The whole command line's -set: is parsed here, so the boot load (record)
    // reports every bad one - engine.json's paths too; engine.json's boot load
    // applies those (LoadBootEngineDevSettings).
    const auto flags = Settings::ParseSetFlags(record ? &errors : nullptr).player;
    for (const auto& f : flags)
    {
        Settings::PutValue(*f.row, f.value, s);
        if (record)
            Settings::SettingsStore::Get().Write(*f.row, Settings::Layer::CommandLine, f.value, f.flag);
    }
    if (record && !errors.empty())
    {
        // A setting the run asked for and cannot have: a precondition, like
        // an unknown -gipath. Carrying on would test something else.
        for (const std::string& e : errors)
            LOG_ERROR("-set: %s", e.c_str());
        LOG_ERROR("-set: %zu override(s) cannot be applied. Refusing to run.", errors.size());
        PostQuitMessage(Harness::PreconditionFail);
    }
    return static_cast<int>(flags.size());
}

bool SaveEngineSettings(const EngineSettings& s, const std::string& path, std::span<const uint8_t> rows)
{
    // -settings=<path> makes the profile READ-ONLY. Guarded here, not at the call sites,
    // so a new writer cannot forget it: a run that rewrote the profile it was handed would
    // make the next run start from different settings, in exactly the runs meant to be
    // comparable.
    if (Sys::CommandLine::Get().Has("-settings"))
    {
        LOG_INFO("SaveEngineSettings: ignored — running with an explicit -settings= profile, which is read-only.");
        return true;
    }
    const std::string text = SerializeEngineSettings(s, rows);
    // Bare names land in the per-user writable root (ResolveSettingsPath).
    const std::string full = ResolveSettingsPath(path);
    Sys::CreateParentDirs(full);
    std::ofstream f = Sys::OpenOfstream(full, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("EngineSettings: cannot write '%s'", full.c_str());
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    LOG_SUCCESS("EngineSettings: saved '%s' (%zu bytes)", full.c_str(), text.size());
    return true;
}

// In-place upgrade of a document written by an OLDER build, before any field is read:
// each step may assume the document is exactly at version `from` and must leave it
// valid at the next version. Adding a key needs no step (an absent key reads as the C++
// default); a step is required when an existing key CHANGES MEANING - renamed,
// re-scaled, or (silently) an enum value inserted mid-list, which re-points every
// stored index. A step's shape: if (from < N) { rewrite j from N-1 to N; from = N; }
static void MigrateSettings(int from, nlohmann::json& j)
{
    // v1 -> v2: the player's fog switch is an ALLOWANCE. In v1, volumetricFog turned fog
    //   on in scenes that configure none (default false) while a scene's own fog rendered
    //   whatever it said; in v2 false forbids every scene's fog, so a v1 value (almost
    //   always the default) would silently remove authored fog: it becomes true.
    //   features.ssao / ddgi became allowances too, but a v1 value means the same to the
    //   player, so it stays.
    if (from < 2)
    {
        const auto f = j.find("features");
        if (f != j.end() && f->is_object() && f->contains("volumetricFog"))
            (*f)["volumetricFog"] = true;
        from = 2;
    }
    // v2 -> v3: no key changes; what a key MEANS does. A v3 file holds only the rows the
    // player chose, a v2 one held every row, so its keys equal to the row's default are not
    // taken as chosen (PlayerRowsToRecord reads SettingsLoadInfo::version).
    if (from < 3)
        from = 3;
    (void)from;
}

// The bytes LoadEngineSettings reads for an already-resolved path: that file,
// or - only when it is the DEFAULT location and missing - the shipped template
// next to the exe (the fallback is explained in LoadEngineSettings).
// The boot peeks (PeekDocument) share it, so a peek can never read a different
// file than the load that follows it. `loadedAs` names where the bytes came from.
static bool ReadSettingsBytes(const std::string& full, std::string& src,
                              std::string& loadedAs, bool& fromShipped)
{
    loadedAs    = full;
    fromShipped = false;
    if (Sys::ReadFileText(full, src))
        return true;
    if (full.rfind(Sys::WritableDataDir(), 0) == 0)
    {
        // The template a package ships beside the exe: <stem>.default<ext>
        // (settings.default.json, authored apart from the editor's working settings.json), then
        // the file under its own name (the layout of packages staged by older tools).
        const std::string base  = full.substr(full.find_last_of("/\\") + 1);
        const size_t      dot   = base.rfind('.');
        const std::string templ = dot == std::string::npos
                                ? base + ".default"
                                : base.substr(0, dot) + ".default" + base.substr(dot);
        for (const std::string& name : { templ, base })
        {
            const std::string shipped = Sys::ExeDir() + "/" + name;
            if (shipped != full && Sys::ReadFileText(shipped, src))
            {
                loadedAs    = shipped;
                fromShipped = true;
                return true;
            }
        }
    }
    return false;
}

bool LoadEngineSettings(const std::string& path, EngineSettings& out, SettingsLoadInfo* info)
{
    // Straight disk read, not AssetFS: settings.json is never sealed into game.ipak (the
    // player edits it) and lives in the writable root, which no pak contains.
    const std::string full = ResolveSettingsPath(path);

    std::string src;
    // Where the bytes came from: `full` is where a SAVE goes, which differs once the
    // shipped-defaults fallback fires.
    std::string loadedAs;
    bool fromShipped = false;
    const bool found = ReadSettingsBytes(full, src, loadedAs, fromShipped);
    const auto rows = Settings::PlayerRows();
    if (info)
    {
        info->found       = found;
        info->loadedAs    = loadedAs;
        info->fromShipped = fromShipped;
        info->rowFromFile.assign(rows.size(), 0);
    }

    // ---- Shipped-default fallback ----
    // The writable root is EMPTY on a fresh install: package_game.py stages the project's
    // settings.default.json NEXT TO THE EXE, and %LOCALAPPDATA%\<Product> does not exist
    // until something writes there. Without the fallback every fresh install would boot on
    // C++ defaults (frameRateCap 0 = unlimited, whatever the project authored). The
    // writable copy is read first so the player's own changes win; writes go only to the
    // writable root (the install directory may be under C:\Program Files). The gate is
    // "resolved to the DEFAULT location", not "a bare name" (App passes the absolute
    // SettingsFilePath()); an explicit path (tools, tests) gets no fallback.
    // ReadSettingsBytes does the read, shared with the boot peeks.
    if (fromShipped)
        LOG_INFO("EngineSettings: no per-user '%s' yet — loading the shipped defaults "
                 "from '%s' (the first Save writes the per-user copy)",
                 full.c_str(), loadedAs.c_str());

    if (!found)
    {
        LOG_INFO("EngineSettings: no '%s' on disk — using engine defaults", full.c_str());
        return false;
    }

    try
    {
        // ignore_comments=true so a hand-editing player can drop // notes in.
        nlohmann::json j = nlohmann::json::parse(src, nullptr, /*allow_exceptions*/true,
                                                 /*ignore_comments*/true);

        // ---- Schema version ----
        // Read first, and route an older document through MigrateSettings before any field is
        // read: otherwise a key whose meaning changed is silently misread. Absent => 1: a
        // file without it is v1-shaped.
        const int fileVersion = ValueOr(j, "version", 1);
        if (info) info->version = fileVersion;
        if (fileVersion > kSettingsVersion)
        {
            // Load anyway: keys this build does not know are skipped, and the known ones keep
            // their meaning within a forward-compatible bump. Re-saving drops the unknown keys;
            // say so.
            LOG_WARNING("EngineSettings: '%s' was written by a newer build "
                        "(version %d > %d) — loading what this build understands; "
                        "unknown fields are ignored and lost if you re-save",
                        full.c_str(), fileVersion, kSettingsVersion);
        }
        else if (fileVersion < kSettingsVersion)
        {
            LOG_INFO("EngineSettings: migrating '%s' from version %d to %d",
                     full.c_str(), fileVersion, kSettingsVersion);
            MigrateSettings(fileVersion, j);
        }

        // ---- The player rows (EngineSettingsSchema.inl) ----
        // A row whose section is absent keeps its current value; a present section reads the
        // row's key, and an absent or mistyped key keeps the current value too (ReadRow,
        // ValueOr).
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto sec = j.find(rows[i].section);
            if (sec != j.end() && sec->is_object())
            {
                const bool got = Settings::ReadRow(rows[i], *sec, out, kWho);
                if (info) info->rowFromFile[i] = got ? 1 : 0;
            }
        }
        // An absent "input" section keeps the compiled-in defaults.
        if (j.contains("input") && j["input"].is_object())
        {
            const auto& ji = j["input"];
            out.input.lookRateDegPerSec = ValueOr(ji, "lookRateDegPerSec", out.input.lookRateDegPerSec);
            out.input.mouseLookScale    = ValueOr(ji, "mouseLookScale",    out.input.mouseLookScale);
            out.input.stickDeadzone     = ValueOr(ji, "stickDeadzone",     out.input.stickDeadzone);
            out.input.stickCurveExp     = ValueOr(ji, "stickCurveExp",     out.input.stickCurveExp);
            out.input.triggerThreshold  = ValueOr(ji, "triggerThreshold",  out.input.triggerThreshold);
            out.input.padInvertLookY    = ValueOr(ji, "padInvertLookY",    out.input.padInvertLookY);
            out.input.mouseInvertLookY  = ValueOr(ji, "mouseInvertLookY",  out.input.mouseInvertLookY);
            out.input.gamepadEnabled    = ValueOr(ji, "gamepadEnabled",    out.input.gamepadEnabled);

            if (ji.contains("bindings") && ji["bindings"].is_array())
            {
                for (const auto& e : ji["bindings"])
                {
                    if (!e.is_object()) continue;
                    InputBindingOverride o;
                    o.action = ValueOr(e, "action", std::string());
                    o.device = ValueOr(e, "device", std::string("kbm"));
                    o.slot   = static_cast<std::uint8_t>(ValueOr(e, "slot", 0));
                    o.path   = ValueOr(e, "path", std::string());
                    if (!o.action.empty() && !o.path.empty())
                        out.inputOverrides.push_back(std::move(o));
                }
            }
        }
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("EngineSettings: failed to parse '%s': %s — using defaults",
                  full.c_str(), ex.what());
        return false;
    }

    // Clamp to sane ranges so a fat-fingered file can't wedge the engine: every
    // row's own sanitize (its `san` in EngineSettingsSchema.inl), then the two
    // rules that span rows (the window pair, peak >= paper white).
    for (const Settings::Row& r : rows)
        SanitizeRow(r, out);
    SanitizePairs(out);
    // Never trust the file: clamp every input scalar (and reject NaN). This is
    // what stops "stickDeadzone": 9.9 from making the character unmovable.
    out.input.Sanitize();

    LOG_SUCCESS("EngineSettings: loaded '%s'", loadedAs.c_str());
    return true;
}

// The settings document a boot peek reads: the bytes LoadEngineSettings will
// read (same file, fallback and migration), then every valid -set: written
// into it in the file's own form - so a peek and the load after it agree on
// what the run asked for (a -set:antialiasing.mode=DLSS that the Streamline
// gate did not see would skip Streamline and demote DLSS to TAA). False when
// the file does not parse; @p why then says so.
static bool PeekDocument(nlohmann::json& j, std::string& loadedAs, std::string& why)
{
    const std::string full = ResolveSettingsPath(SettingsFilePath());
    std::string src;
    bool fromShipped = false;
    j = nlohmann::json::object();
    if (ReadSettingsBytes(full, src, loadedAs, fromShipped))
    {
        try
        {
            j = nlohmann::json::parse(src, nullptr, /*allow_exceptions*/true, /*ignore_comments*/true);
            const int fileVersion = j.value("version", 1);
            if (fileVersion < kSettingsVersion)
                MigrateSettings(fileVersion, j);
        }
        catch (const std::exception& e)
        {
            why = "'" + loadedAs + "' did not parse (" + e.what() + ")";
            return false;
        }
    }
    else
        loadedAs = "no settings file, engine defaults";
    const auto flags = Settings::ParseSetFlags(nullptr).player;
    if (!flags.empty())
    {
        if (!j.is_object()) j = nlohmann::json::object();
        for (const auto& f : flags)
        {
            nlohmann::json& sec = j[f.row->section];
            if (!sec.is_object()) sec = nlohmann::json::object();
            sec[f.row->key] = ValueJson(*f.row, f.value);
        }
        loadedAs += " with " + std::to_string(flags.size()) + " -set override(s)";
    }
    return true;
}

bool PeekSettingsWantRayReconstruction(std::string& why)
{
    // Only antialiasing.mode, read the way PeekSettingsWantStreamline does.
    nlohmann::json j;
    std::string loadedAs;
    if (!PeekDocument(j, loadedAs, why))
        return false;
    int aaMode = EngineSettings{}.aaMode;
    try
    {
        if (j.contains("antialiasing") && j["antialiasing"].is_object())
            aaMode = AAModeFromString(j["antialiasing"].value("mode", std::string(AAModeToString(aaMode))));
    }
    catch (const std::exception& e) { why = e.what(); return false; }
    why = loadedAs + ": antialiasing.mode=" + AAModeToString(aaMode);
    return aaMode == static_cast<int>(Renderer::AAMode::DLSS_RR);
}

bool PeekSettingsWantStreamline(std::string& why)
{
    // Same document as LoadEngineSettings (PeekDocument) and the same j.value defaults -
    // only the fields that need Streamline, and no log line: this runs before Streamline
    // initialises, long before the real load.
    nlohmann::json j;
    std::string loadedAs;
    if (!PeekDocument(j, loadedAs, why))
        return true;   // unknown: keep Streamline
    const EngineSettings defaults;
    int aaMode   = defaults.aaMode;
    int reflex   = defaults.reflexMode;
    int frameGen = defaults.frameGenFrames;
    int aaLevel  = defaults.levelAA;
    try
    {
        if (j.contains("display") && j["display"].is_object())
        {
            reflex   = j["display"].value("reflex",   reflex);
            frameGen = j["display"].value("frameGen", frameGen);
        }
        if (j.contains("antialiasing") && j["antialiasing"].is_object())
            aaMode = AAModeFromString(
                j["antialiasing"].value("mode", std::string(AAModeToString(aaMode))));
        // The aa group's level can ask for DLSS through the Scalability layer: any level but
        // High (the defaults, TAA) keeps Streamline without reading the table (a level the
        // player overrode to TAA keeps it needlessly, which costs only the boot).
        if (j.contains("scalability") && j["scalability"].is_object())
            aaLevel = Settings::kLevelCodec.fromToken(
                j["scalability"].value("aa", std::string(Settings::kLevelCodec.toToken(aaLevel))));
    }
    catch (const std::exception& e)
    {
        why = "'" + loadedAs + "' did not parse (" + e.what() + ")";
        return true;   // unknown: keep Streamline
    }

    const bool want = aaMode == static_cast<int>(Renderer::AAMode::DLSS) ||
                      aaMode == static_cast<int>(Renderer::AAMode::DLSS_RR) ||
                      reflex != 0 || frameGen != 0 || aaLevel != Settings::Scalability::kHigh;
    why = loadedAs + ": antialiasing.mode=" + AAModeToString(aaMode) +
          " display.reflex=" + std::to_string(reflex) +
          " display.frameGen=" + std::to_string(frameGen) +
          " scalability.aa=" + Settings::kLevelCodec.toToken(aaLevel);
    return want;
}

std::string PeekSettingsAdapter()
{
    if (Sys::CommandLine::Get().Has("-hidden"))
        return {};
    nlohmann::json j;
    std::string loadedAs, why;
    if (!PeekDocument(j, loadedAs, why))
        return {};
    try
    {
        if (j.contains("display") && j["display"].is_object())
            return j["display"].value("adapter", std::string());
    }
    catch (const std::exception&)
    {
    }
    return {};
}

bool PeekSettingsWindowSize(int& width, int& height)
{
    // Under -hidden a harness run keeps the fixed creation size: a -hidden swap chain is
    // the compiled-in size, and Game.exe's settings file is PER USER, so following it would
    // make every hidden pack's render resolution depend on the machine it ran on.
    if (Sys::CommandLine::Get().Has("-hidden"))
        return false;

    // Same document as LoadEngineSettings (PeekDocument: the file and any
    // -set:) - two fields, no log line: this runs before the window exists,
    // long before the load.
    nlohmann::json j;
    std::string loadedAs, why;
    if (!PeekDocument(j, loadedAs, why))
        return false;   // the real load reports it
    try
    {
        if (!j.contains("display") || !j["display"].is_object())
            return false;
        const int w = j["display"].value("windowWidth",  0);
        const int h = j["display"].value("windowHeight", 0);
        // 0 = "leave the window alone" (ApplyTo's rule); below the 320x240
        // floor is a value the load would reject.
        if (w < 320 || h < 240)
            return false;
        width  = w;
        height = h;
        return true;
    }
    catch (const std::exception&)
    {
        return false;   // the real load reports it
    }
}

// Live binding table -> persisted form. Tuning scalars are copied wholesale; bindings
// are stored as DELTAS against Defaults(), so an action added later arrives with its
// compiled-in default instead of unbound, and a hand-edited file stays short.
void EngineSettings::CaptureInputFrom(const InputBindings& live)
{
    input = live;
    inputOverrides.clear();

    const InputBindings def = InputBindings::Defaults();
    char buf[64];

    for (int i = 0; i < kGameActionCount; ++i)
    {
        const GameAction a = static_cast<GameAction>(i);
        const ActionBinding& lb = live.action[i];
        const ActionBinding& db = def.action[i];

        for (int slot = 0; slot < 2; ++slot)
        {
            if (lb.vk[slot] == db.vk[slot]) continue;
            InputBindingOverride o;
            o.action = ToString(a);
            o.device = "kbm";
            o.slot   = static_cast<std::uint8_t>(slot);
            // An unbound slot round-trips as an explicit "none" so clearing a
            // default binding survives a reload.
            o.path   = FormatBindingPath(false, lb.vk[slot], buf, sizeof(buf))
                     ? buf : "none";
            inputOverrides.push_back(std::move(o));
        }

        if (lb.pad != db.pad)
        {
            // ActionBinding::pad is an OR (any of these bits fires it), but the persisted form
            // names ONE control and ParsePadButton returns one bit, so a multi-bit mask cannot
            // round-trip. Defaults() and the rebind capture produce single-bit masks; only a
            // hand-edited file or Lua reaches this - warn rather than drop bits silently.
            const bool multiBit = (lb.pad & (lb.pad - 1)) != 0;
            if (multiBit)
            {
                LOG_WARNING("EngineSettings: pad binding for '%s' has multiple bits (0x%04X);"
                            " only the primary control is saved",
                            ToString(a), static_cast<unsigned>(lb.pad));
            }

            InputBindingOverride o;
            o.action = ToString(a);
            o.device = "pad";
            o.slot   = 0;
            o.path   = FormatBindingPath(true, lb.pad, buf, sizeof(buf))
                     ? buf : "none";
            inputOverrides.push_back(std::move(o));
        }
    }

    // ---- Movement / look composite ----
    // The loop above walks only InputBindings::action[]; the WASD composite and the stick
    // selection live in `move` / `look`, and without this block the editor's Movement table
    // would be lost on the next launch. Same delta-against-defaults shape as an action, with
    // the field named in the `action` slot; the spellings match the editor's rebind panel
    // (kCompositeFieldNames), so the two cannot disagree about a row's name.
    {
        struct AxisRef  { const char* prefix; const Axis2DBinding* live; const Axis2DBinding* def; };
        struct FieldRef { const char* name;   std::uint16_t Axis2DBinding::* member; };

        const AxisRef axes[] = {
            { "move", &live.move, &def.move },
            { "look", &live.look, &def.look },
        };
        static constexpr FieldRef kFields[] = {
            { "vkPosX", &Axis2DBinding::vkPosX }, { "vkNegX", &Axis2DBinding::vkNegX },
            { "vkPosY", &Axis2DBinding::vkPosY }, { "vkNegY", &Axis2DBinding::vkNegY },
        };

        for (const AxisRef& ax : axes)
        {
            for (const FieldRef& f : kFields)
            {
                const std::uint16_t lv = ax.live->*f.member;
                if (lv == ax.def->*f.member) continue;
                InputBindingOverride o;
                o.action = std::string(ax.prefix) + "." + f.name;
                o.device = "kbm";
                o.slot   = 0;
                o.path   = FormatBindingPath(false, lv, buf, sizeof(buf)) ? buf : "none";
                inputOverrides.push_back(std::move(o));
            }
            if (ax.live->stick != ax.def->stick)
            {
                InputBindingOverride o;
                o.action = std::string(ax.prefix) + ".stick";
                o.device = "pad";
                o.slot   = 0;
                // A stick is not a control path, so it gets its own two tokens
                // rather than being squeezed through ParseBindingPath.
                o.path   = (ax.live->stick == 1) ? "right" : "left";
                inputOverrides.push_back(std::move(o));
            }
        }
    }
}

// Rebind deltas -> binding table; an unknown entry is logged and skipped, never
// unbound (see the declaration).
void ApplyBindingOverrides(InputBindings& b,
                           const std::vector<InputBindingOverride>& overrides)
{
    for (const auto& o : overrides)
    {
        // ---- Composite rows first ----
        // "move.vkPosY" / "look.stick" etc. are not GameActions: recognise them BEFORE
        // ParseGameAction, which would reject them as unknown and drop a legitimate rebind.
        if (o.action.rfind("move.", 0) == 0 || o.action.rfind("look.", 0) == 0)
        {
            Axis2DBinding& axis = (o.action.rfind("move.", 0) == 0) ? b.move : b.look;
            const std::string field = o.action.substr(5);

            if (field == "stick")
            {
                if      (o.path == "left")  axis.stick = 0;
                else if (o.path == "right") axis.stick = 1;
                else
                    LOG_WARNING("EngineSettings: unknown stick '%s' for '%s' — keeping "
                                "the existing selection", o.path.c_str(), o.action.c_str());
                continue;
            }

            std::uint16_t Axis2DBinding::* member = nullptr;
            if      (field == "vkPosX") member = &Axis2DBinding::vkPosX;
            else if (field == "vkNegX") member = &Axis2DBinding::vkNegX;
            else if (field == "vkPosY") member = &Axis2DBinding::vkPosY;
            else if (field == "vkNegY") member = &Axis2DBinding::vkNegY;
            if (!member)
            {
                LOG_WARNING("EngineSettings: unknown composite field '%s' — ignored",
                            o.action.c_str());
                continue;
            }

            if (o.path == "none") { axis.*member = 0; continue; }

            bool          cIsPad = false;
            std::uint16_t cVk = 0, cPad = 0;
            if (!ParseBindingPath(o.path.c_str(), cIsPad, cVk, cPad))
                cVk = ParseVirtualKey(o.path.c_str());
            // A composite direction is a KEY. A pad control here would be a
            // corrupt or hand-edited row; skipping keeps the same never-unbind
            // policy the action path uses.
            if (cIsPad || cVk == 0)
            {
                LOG_WARNING("EngineSettings: '%s' is not a keyboard key for '%s' — "
                            "keeping the existing binding",
                            o.path.c_str(), o.action.c_str());
                continue;
            }
            axis.*member = cVk;
            continue;
        }

        GameAction a{};
        if (!ParseGameAction(o.action.c_str(), a))
        {
            LOG_WARNING("EngineSettings: unknown input action '%s' — binding ignored",
                        o.action.c_str());
            continue;
        }

        const bool wantPad = (o.device == "pad");

        // "none" is the round-trip form of a DELIBERATELY cleared binding, and
        // is the one case where unbinding is intended rather than a parse
        // failure. Handled before ParseBindingPath so it can never be mistaken
        // for a corrupt entry.
        if (o.path == "none")
        {
            if (wantPad) b.action[static_cast<int>(a)].pad = PadBtn::None;
            else         b.action[static_cast<int>(a)].vk[(o.slot > 1) ? 1 : o.slot] = 0;
            continue;
        }

        // Accept both a bare control name ("space", "a") and a fully-qualified
        // path ("kbm/space", "pad/a"); the qualified form wins if present.
        bool          isPad = wantPad;
        std::uint16_t vk = 0, padMask = 0;
        if (!ParseBindingPath(o.path.c_str(), isPad, vk, padMask))
        {
            if (wantPad) padMask = ParsePadButton(o.path.c_str());
            else         vk      = ParseVirtualKey(o.path.c_str());
            isPad = wantPad;
        }

        if (isPad)
        {
            if (padMask == PadBtn::None)
            {
                LOG_WARNING("EngineSettings: unknown pad control '%s' for action '%s'"
                            " — keeping the existing binding",
                            o.path.c_str(), o.action.c_str());
                continue;
            }
            b.action[static_cast<int>(a)].pad = padMask;
        }
        else
        {
            if (vk == 0)
            {
                LOG_WARNING("EngineSettings: unknown key '%s' for action '%s'"
                            " — keeping the existing binding",
                            o.path.c_str(), o.action.c_str());
                continue;
            }
            b.action[static_cast<int>(a)].vk[(o.slot > 1) ? 1 : o.slot] = vk;
        }
    }
}

// Tuning scalars first, then the rebind deltas on top of the compiled-in defaults
// (see the declaration).
void ApplyInputBindings(const EngineSettings& settings)
{
    InputBindings b = settings.input;
    // The overrides list only carries what differs from the defaults, so
    // start from Defaults() and keep the loaded tuning scalars.
    InputBindings base = InputBindings::Defaults();
    base.lookRateDegPerSec = b.lookRateDegPerSec;
    base.mouseLookScale    = b.mouseLookScale;
    base.stickDeadzone     = b.stickDeadzone;
    base.stickCurveExp     = b.stickCurveExp;
    base.triggerThreshold  = b.triggerThreshold;
    base.padInvertLookY    = b.padInvertLookY;
    base.mouseInvertLookY  = b.mouseInvertLookY;
    base.gamepadEnabled    = b.gamepadEnabled;
    ::Resource::ApplyBindingOverrides(base, settings.inputOverrides);   // `Resource` alone names the class here
    Input::Get().SetBindings(base);
}

} // namespace Resource
