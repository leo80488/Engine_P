#pragma once

// settings.json: the player-facing graphics / quality / audio / input options (the
// engine's GameUserSettings). Kept apart from game.json:
//   game.json      dev-authored, sealed into game.ipak: a shipped build cannot retarget scenes.
//   settings.json  player-authored, writable, never sealed. In Sys::WritableDataDir(): the
//                  project directory in the editor (versioned beside its content), the
//                  per-user root (%LOCALAPPDATA%\<product>) in Game.exe - never beside the
//                  exe, because C:\Program Files is not writable and two Windows accounts
//                  must not share one file. main.cpp makes the choice once.
// An absent file or key keeps the C++ default, so a fresh install boots and an older file
// keeps working.
// Scope: Renderer-authoritative knobs (frame pacing, AA, shadows, render features) are
// applied directly. The post-process toggles (CAS, DoF, lens flare, auto-exposure, color
// grading) go through the ENGINE-DEFAULT PostProcessProfile, the base layer of the volume
// resolve (PostProcessResolve.h): settings.json is the global default look, and a scene's
// .ppprofile and any PostProcessVolume still blend on top. Their defaults MUST match
// MakeEngineDefaultProfile() so an absent "postProcess" section is a no-op. Bloom
// (intensity only), NPR stylize and underwater stay profile-owned.
// Use: the boot load is LoadBootEngineSettings (App::BootRenderer); the apply-at-boot values
// (csmResolution, textureAnisotropy, pointShadowResolution) reach the Renderer before
// Compile(), ApplyTo pushes the rest once the passes exist. Every write goes through
// EditEngineSettingsFile.
// Adding a field: the member below plus ONE row in EngineSettingsSchema.inl - its
// settings.json key, load sanitize, apply / capture hooks and Lua Settings.* path all come
// from that row (Settings/SettingsRows.h). The input block is the exception: an array,
// hand-written in Save / Load / CaptureInputFrom.

#include "Input/InputTypes.h"
#include "Audio/AudioTypes.h"   // BusType, for LiveBusVolume

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Renderer;
class IGraphicsDevice;
namespace Plat { class Window; }

namespace Resource
{
    // One rebind delta. `path` uses the canonical control names produced by
    // FormatBindingPath ("kbm/space", "pad/a") so the file stays readable and
    // hand-editable.
    struct InputBindingOverride
    {
        std::string  action;       // "Jump" - must match ToString(GameAction)
        std::string  device;       // "kbm" | "pad"
        std::uint8_t slot = 0;     // kbm: 0 or 1 (two keys per action); pad: ignored
        std::string  path;         // "space", "w", "pad/a", or a bare control name
    };

    // Applies @p overrides on top of the base table @p b. Unknown action names and control
    // paths are logged and SKIPPED, never applied as "unbound", so a corrupt file cannot
    // lock the player out of moving.
    void ApplyBindingOverrides(InputBindings& b,
                               const std::vector<InputBindingOverride>& overrides);

    struct EngineSettings
    {
        // ---- Display / present ----
        int   frameRateCap = 0;      // 0 = unlimited (SurfaceDX12::targetFrameRate)
        bool  vsync        = true;   // SurfaceDX12::vsyncEnabled
        bool  gsync        = false;  // SurfaceDX12::gsyncEnabled (VRR)
        // NVIDIA Reflex: 0 off, 1 low latency, 2 low latency + boost.
        // When on, Reflex takes over frame pacing and frameRateCap above is
        // enforced by it rather than by the engine's own limiter.
        int   reflexMode   = 0;
        // DLSS Frame Generation: frames GENERATED per rendered frame.
        // 0 = off, 1 = 2x, 2 = 3x, 3 = 4x. Requires Reflex (forced on when
        // this is non-zero), a game build, and an SDR or HDR10 back buffer.
        int   frameGenFrames = 0;
        bool  fullscreen   = false;  // Window::SetFullscreen (applied at boot)
        // Internal render scale [0.25 .. 2.0]: the scene renders at scale x display
        // resolution and the final composite resamples to the swap chain. 1.0 = native,
        // < 1 faster, > 1 supersampling.
        float renderScale  = 1.0f;
        // Windowed CLIENT size. 0 = leave the window alone (a fresh install, or a file
        // without the key), so this field never moves a window nobody asked to move;
        // CaptureFrom writes the live size, so the first Save pins it. The WINDOW size, not a
        // display mode: fullscreen is borderless (the swap chain matches the output) and
        // there is no mode enumeration, so ApplyTo routes it through Window::RequestSize,
        // which drops the request while fullscreen and clamps to the monitor work area.
        int   windowWidth  = 0;
        int   windowHeight = 0;

        // ---- Anti-aliasing ----
        // aaMode mirrors Renderer::AAMode: 0 None, 1 FXAA, 2 TAA, 3 FXAA+TAA, 4 FSR, 5 DLSS,
        // 6 DLSS_RR; serialized as its token ("TAA", "FXAA_TAA", ...), never the index.
        int   renderPath   = 0;      // Renderer::RenderPath, requested preference
        int   aaMode       = 2;      // TAA
        // Upscaler render-scale ratio, mirrors UpscaleMode: 0 Native (1:1 AA), 1 Quality,
        // 2 Balanced, 3 Performance, 4 UltraPerformance. Only bites with an upscaler (FSR, DLSS).
        int   upscaleMode  = 0;      // Native
        // Which DL model DLSS runs, mirrors SL::DLSSPreset: 0 driver default, 1 J, 2 K, 3 L,
        // 4 M. Pinned, not the driver default: the SDK's per-mode defaults (K for DLAA /
        // Quality / Balanced, M / L for Performance / UltraPerformance) would make the
        // render-scale setting change the MODEL as well as the resolution, and the models
        // differ visibly on thin dark features. Only bites when aaMode == DLSS.
        int   dlssPreset   = 4;      // M
        // Which model DLSS Ray Reconstruction runs: "D", "E" or "F". D by default: the model
        // every RR pack was judged with. Only bites when aaMode == DLSS_RR; -dlss_rr_preset
        // overrides it.
        std::string dlssRRModel = "D";

        // ---- Texture filtering (APPLY-AT-BOOT) ----
        // Global material-texture anisotropy for opaque, transparent, terrain-layer and water
        // textures: 1 = trilinear, 2/4/8/16 = that max anisotropy. Samplers are created in
        // Renderer::Compile(), so a live edit takes effect at the next launch.
        int   textureAnisotropy = 8;

        // ---- HDR display output ----
        // Mirrors HDROutputMode: 0 SDR, 1 HDR10, 2 scRGB; an int so this header needs no
        // Graphics include. Serialized as a STRING, never the index, so a mode inserted later
        // cannot re-point existing files. SDR is the default on purpose: HDR on a panel that
        // does not report HDR is a washed-out image the player cannot diagnose in game, so a
        // fresh install boots into the mode correct on every display (and the cheapest: the
        // RGBA8 composition buffer).
        int   hdrMode           = 0;      // mirrors HDROutputMode; 0 = SDR
        // The nits "SDR white" maps to: everything the SDR path drew as 1.0 is emitted at
        // this brightness (the composition space stores pow(linearNits / paperWhite, 1/2.2)).
        // 0 = follow the SDR content brightness Windows has set for the monitor (the display
        // snapshot's sdrWhiteNits); a value is the player's override, [80, 480]: below ~100 the
        // UI looks murky beside SDR apps, above ~400 it tires the eye in a dark room.
        float hdrPaperWhiteNits = 0.0f;
        // The brightest value the tone curve rolls off toward; should track the PANEL's
        // capability (GraphicsDX12 caches DXGI_OUTPUT_DESC1::MaxLuminance and the editor seeds
        // this from it). More peak than the display has buys no brightness: it hands the top
        // of the curve to the panel's tone mapper to clip. Clamped to [400, 4000] and never
        // below paper white. Ignored while hdrMode == SDR.
        float hdrPeakNits       = 1000.0f;

        // ---- Shadows (APPLY-AT-BOOT) ----
        // CSM per-cascade resolution (1024 / 2048 / 4096). ShadowPass::Init() creates the
        // shadow Texture2DArray once, so this only takes effect when applied BEFORE
        // Renderer::Compile(): a change needs a restart (the UI says so).
        int   csmResolution = 4096;
        // Quality knobs the shadows group's level sets (Settings/Scalability.h); the defaults
        // are the High level.
        //  shadowDistance          how far the CSM's three near cascades reach (m; the far
        //                          cascade still goes to 2000)
        //  spotShadowMaxResolution the highest tier a spot light's shadow gets
        //                          (512 / 1024 / 2048: the nearest lights' tiers)
        //  pointShadowResolution   each cube face (APPLY-AT-BOOT, like CSM)
        float shadowDistance          = 400.0f;
        int   spotShadowMaxResolution = 2048;
        int   pointShadowResolution   = 1024;

        // ---- Render features (Renderer-authoritative toggles) ----
        // AO, volumetric fog, DDGI and SSGI are ALLOWANCES (Settings::Policy::Capped): what
        // runs is the row AND the scene's intent - its .ippc (AO, fog), Volumetric Fog
        // component and local fog volumes, IndirectLightingSettings (DDGI, SSGI).
        // true = allowed.
        bool  ssao            = true;  // Renderer::SetSSAOAuthored (XeGTAO)
        bool  ssr             = true;  // Renderer::SetSSREnabled
        bool  rtReflections   = false; // Renderer::SetRTReflectionsEnabled (needs DXR; default OFF - TDR budget)
        // Max world-unit distance an RT reflection ray travels; beyond it the
        // ray misses and the sky cube fills in. Also scales REBLUR's
        // hit-distance normalization (A = rayTMax * 0.075), so it re-tunes the
        // denoiser as well as the range. Clamped to [20, 400] by the setter.
        float rtReflectionsRayTMax = 150.0f;     // Renderer::SetRTReflectionsRayTMax
        // Forward translucent surfaces fold the traced reflection of the
        // opaque surface behind them (coplanar + close gate). Works with the
        // unified pass in any mode (SSR-only, RTR-only, hybrid); a no-op when
        // neither traces. Default ON.
        bool  transparentReflections = true;     // Renderer::SetTransparentReflectionsEnabled
        // Ray-traced AO, a sub-mode of ssao: swaps XeGTAO's main pass for a TLAS hemisphere
        // trace; the denoise / temporal chain is unchanged. Needs DXR.
        bool  rtao            = false; // Renderer::SetRTAOEnabled
        // Ray-traced sun shadows: a full-screen mask min()'d with CSM. Needs DXR.
        bool  rtShadows       = false; // Renderer::SetRTShadowsEnabled
        bool  volumetricFog   = true;  // Renderer::SetVolumetricFogAuthored (VolumetricFogPass::SetAllowed)
        // DDGI / SSGI allowances: what runs is this AND the scene's IndirectLightingSettings
        // intent (Settings::Policy::Capped, Renderer::BuildScene_UpdateDDGI). true = allowed.
        bool  ddgi            = true;
        bool  ssgi            = true;
        // XeGTAO's slices x steps (the post group's level): 0 Low 1x2, 1 Medium 2x2,
        // 2 Scene - the counts the scene's render features config gives (3x3 without one),
        // 3 Ultra 9x3.
        int   aoQuality       = 2;
        // The meshlet LOD threshold's multiplier (the viewDistance group's level):
        // culling.meshletLodError (engine.json) and its shadow twin times this - above 1
        // coarser, below finer. The VS path has no LOD.
        float lodScale        = 1.0f;

        // ---- Post-process toggles (the engine-default profile, the global base) ----
        // Defaults MUST equal MakeEngineDefaultProfile()'s (CAS / ColorGrading / LensFlare on,
        // DoF / AutoExposure off), so applying an absent section changes nothing. Scenes and
        // volumes still blend on top.
        bool  ppCAS           = true;  // Contrast Adaptive Sharpening
        bool  ppDoF           = false; // Depth of Field
        bool  ppLensFlare     = true;  // procedural lens flare
        // histogram auto-exposure (off = manual). Off here, in the engine default profile and in the shipped
        // template (settings.default.json): the scene's post-process volume or .ppprofile turns it on, so it
        // is the scene author's parameter, not a player preference (the player menu has no switch for it).
        bool  ppAutoExposure  = false;
        bool  ppColorGrading  = true;  // 3D-LUT color grading

        // ---- Audio (mixer bus volumes) ----
        // One linear gain per Audio::BusType (1.0 = unity); field order and names follow that
        // enum (Audio/AudioTypes.h) - keep them in step. Applied by PUBLISHING one
        // BusVolumeChangedEvent per bus, the entry point Lua's Audio.setBusVolume uses:
        // AudioEngine is not reachable from here and stays the single owner of the mixer.
        // CaptureFrom reads the mirror LiveBusVolume keeps.
        float audioMaster  = 1.0f;
        float audioMusic   = 1.0f;
        float audioSFX     = 1.0f;
        float audioVoice   = 1.0f;
        float audioAmbient = 1.0f;
        float audioUI      = 1.0f;

        // ---- UI ----
        // Default UI font (FontLibrary entry 0), for text elements without a fontPath.
        // Applied at boot (App::InitUIFont).
        std::string uiDefaultFont = "engine/font/FGMiraiRen.ttf";
        // UI language: a BCP-47-ish code matching a table under asset/scripts/loc/ ("en",
        // "zh-Hant", "ja"). The engine has no localization subsystem, so ApplyTo does nothing
        // with it; the script layer (asset/scripts/ui/Loc.lua) reads it. A CODE, not an index,
        // so inserting a language cannot re-point saved files (aaMode and hdrMode serialize as
        // tokens for the same reason).
        std::string uiLanguage = "en";

        // ---- Display adapter ----
        // The graphics card to run on, by the name DXGI gives it; "" = the high-performance one.
        // Read when the device is created (PeekSettingsAdapter), so it takes effect at the next launch.
        std::string displayAdapter;

        // ---- Accessibility ----
        float cameraShake = 1.0f;   // every camera shake's strength: 0 none, 1 as authored
        int   colorVision = 0;      // the present shader's correction: 0 off, 1 protanopia, 2 deuteranopia, 3 tritanopia
        float colorVisionStrength = 1.0f;   // how much of the deficiency it assumes: 0 none, 1 the full dichromat

        // ---- Quality levels (Settings/Scalability.h) ----
        // One per group of quality rows: 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Cinematic, saved
        // by name ("scalability": { "shadows": "Low" }). A level decides its group's rows
        // through the settings store's Scalability layer, under the player's own values; High
        // is those rows' defaults, so a fresh install runs the defaults.
        // engine/scalability.json says which rows each group holds.
        int   levelShadows      = 2;
        int   levelGI           = 2;
        int   levelReflections  = 2;
        int   levelPost         = 2;
        int   levelTextures     = 2;
        int   levelEffects      = 2;
        int   levelViewDistance = 2;
        int   levelAA           = 2;

        // ---- Face tracking ----
        // Whether face tracking may open a camera at all: a privacy switch, OFF until the
        // player turns it on (the in-game Controls tab, the editor's Face Tracking window).
        // While off, FaceTracking.OpenCamera and ListCameras are refused; replays, network
        // apps and video files are not cameras and are unaffected.
        bool faceTrackingCamera = false;

        // ---- Input ----
        // Tuning scalars (deadzone, curves, look rate, invert), applied to the Input singleton
        // at boot. Sanitize() clamps every field on load, so a hand-corrupted file cannot make
        // the game unplayable.
        InputBindings input {};

        // Rebind DELTAS, not the whole table: a partially-written file degrades per binding,
        // and a GameAction added later still gets its compiled-in default instead of arriving
        // unbound.
        std::vector<InputBindingOverride> inputOverrides;

        // Pushes the live-mutable values onto the engine. @p window may be null (the
        // fullscreen apply is then skipped). csmResolution goes to the renderer's pending
        // value and only takes effect at the next Compile().
        void ApplyTo(Renderer& r, IGraphicsDevice& gfx, Plat::Window* window) const;

        // Pulls the engine's live values back into this struct (the editor Save, the
        // options-menu round trip), the live input bindings included (CaptureInputFrom).
        // @p window may be null.
        void CaptureFrom(Renderer& r, IGraphicsDevice& gfx, const Plat::Window* window);

        // Copies the tuning scalars and records the bindings that DIFFER from
        // InputBindings::Defaults() into inputOverrides. CaptureFrom calls it, so no save path
        // drops the player's rebinds.
        void CaptureInputFrom(const InputBindings& live);
    };

    // Current linear gain of a mixer bus, as last published on the EventBus. AudioEngine
    // exposes no getter (it is a private App member), so this shadows the
    // BusVolumeChangedEvent stream AudioSystem consumes: CaptureFrom reads it, and an options
    // menu initialises its sliders from it. Accurate only while EVERY write goes through
    // BusVolumeChangedEvent (ApplyTo and Lua's Audio.setBusVolume do): do not call
    // AudioEngine::SetBusVolume directly, it would desync this.
    float LiveBusVolume(Audio::BusType bus);

    // Set a mixer bus: publish a BusVolumeChangedEvent (volume clamped to
    // [0, 2]) and seed the mirror LiveBusVolume reads, so a capture before the
    // event is dispatched still reads it back. The audio.* rows' apply.
    void PublishBusVolume(Audio::BusType bus, float volume);

    // Document schema version, written by Save and READ by Load. Bump it when an existing
    // key changes MEANING (renamed, re-scaled, an enum value inserted mid-list) and add the
    // step to MigrateSettings(). Adding a key needs no bump: an absent key reads as the C++
    // default.
    //   2: features.volumetricFog means "fog allowed" (default true); a v1 value ("fog in
    //      scenes that do not configure fog", default false) becomes true.
    //   3: a key in the file MEANS the player chose it - a save writes only the settings
    //      store's Player layer, so a row the player never set follows the layers under it.
    //      A v1 / v2 file wrote every row: its keys equal to the row's default read as not
    //      chosen (PlayerRowsToRecord).
    inline constexpr int kSettingsVersion = 3;

    // Absolute path of the player's settings.json in the writable root. Use it instead of
    // a bare "settings.json": a relative path resolves against the working directory, which
    // is not the exe directory when the game starts from a shortcut, a debugger or elevated.
    const std::string& SettingsFilePath();

    // Writes @p s to @p path as pretty-printed JSON; false only on an I/O failure. A BARE
    // filename resolves against the per-user writable root. With @p rows (indexed like
    // Settings::PlayerRows()) only the rows whose entry is non-zero are written (a
    // settings.json holds the player's choices); empty writes every row (a resolved-state
    // dump, -provenance_settings).
    bool SaveEngineSettings(const EngineSettings& s, const std::string& path,
                            std::span<const uint8_t> rows = {});
    // Same persisted field contract as Save, without filesystem side effects.
    std::string SerializeEngineSettings(const EngineSettings& s, std::span<const uint8_t> rows = {});

    // THE way to change settings.json: load SettingsFilePath(), let @p edit change the
    // fields its caller owns and which rows the file holds (rows, filled with the loaded
    // file's PlayerRowsToRecord), save it back. Read-modify-write, so every field the
    // caller does not own keeps its saved value. The editor's Saves
    // (SaveCapturedEngineSettings), Settings.save (SavePlayerChoices) and
    // Input.SaveBindings (the input block) all go through it. Returns SaveEngineSettings'
    // result (true, writing nothing, under -settings=).
    bool EditEngineSettingsFile(const std::function<void(EngineSettings&, std::vector<uint8_t>& rows)>& edit);

    // The editor's "save what is live" path: every row whose live value (CaptureFrom)
    // differs from what the settings store gives its owner is recorded in the Player layer
    // first - the window size the player dragged, a change made outside the store - then
    // the Player layer is saved, with the live input block. A value a -set: or Runtime
    // write holds is the store's own and is not saved; a field CaptureFrom does not own
    // keeps the store's value.
    bool SaveCapturedEngineSettings(Renderer& r, IGraphicsDevice& gfx, const Plat::Window* window);

    // settings.json holding the player's choices: the rows @p chosen marks (indexed like
    // Settings::PlayerRows()), at @p values, over what the file has, the input block kept.
    // Settings.save hands it the Player layer its apply leaves.
    bool SavePlayerChoices(const EngineSettings& values, std::span<const uint8_t> chosen);

    // What a load found, for a caller that needs more than found / not found.
    struct SettingsLoadInfo
    {
        bool        found = false;
        std::string loadedAs;              // the file the bytes came from
        // Per player row (Settings::PlayerRows() order): 1 when the file held
        // the row's key with a usable value, 0 when the value is the one `out`
        // already had or the row's absent-key default.
        std::vector<uint8_t> rowFromFile;
        // The document's own "version" (1 when it has none), before migration;
        // 0 without a file.
        int version = 0;
        // The bytes are the shipped template beside the exe, not the player's own file (a
        // first boot).
        bool fromShipped = false;
    };

    // The rows a load of @p s (@p info) counts as the player's choices - what goes into the
    // settings store's Player layer, and what a re-save keeps: every key of a version-3
    // file; of an older one, which wrote every row, the keys whose value is not the row's
    // default. Either way, a value the load's sanitize moved off its default too.
    std::vector<uint8_t> PlayerRowsToRecord(const EngineSettings& s, const SettingsLoadInfo& info);

    // Parses @p path. A missing file leaves @p out at its defaults and returns false
    // (absence means "use defaults", not an error); missing or corrupt keys are tolerated.
    // A BARE filename resolves against the per-user writable root, as in Save.
    bool LoadEngineSettings(const std::string& path, EngineSettings& out,
                            SettingsLoadInfo* info = nullptr);

    // The boot load (App::BootRenderer): LoadEngineSettings(SettingsFilePath()) into
    // @p out, then every value the file supplied into the settings store's Player layer
    // (Settings/SettingsStore.h), written by the file's path; then the command line's -set:
    // overrides (ApplyCommandLineSettings with record: the CommandLine layer); then the
    // quality levels' rows into the Scalability layer (Settings/Scalability.h); and @p out
    // becomes the store's values (ProjectStoreValues), which is what the owners are handed.
    // Logs [SCALABILITY]. With @p gfx, a first boot (no settings file of the player's own
    // was read) first picks the quality levels from the adapter's video memory
    // (Settings::Scalability::ApplyFirstBoot).
    bool LoadBootEngineSettings(EngineSettings& out, const IGraphicsDevice* gfx = nullptr);

    // The Player layer from a load of settings.json: every row the file supplied
    // (@p info), plus any value the load's sanitize moved off its default, written by
    // @p writer. With @p replace the Player layer is emptied first - a reload, where the
    // player's earlier values go (the boot writes into an empty store).
    void RecordLoadedPlayerRows(const EngineSettings& s, const SettingsLoadInfo& info,
                                std::string_view writer, bool replace);

    // Every player row of @p s set to the settings store's owner value
    // (SettingsStore::OwnerValue: the layered value; a Capped row's allowance) - what the
    // owners must be handed after the store changed under them, a layer emptied or replaced.
    void ProjectStoreValues(EngineSettings& s);

    // -set:<path>=<value>: a command-line override of one player row, repeatable, a later
    // one for the same row winning. The path matches any case; the value is read by the
    // row's kind (a Bool takes true/false/1/0/on/off/yes/no, an Enum its index, label or
    // settings.json token) and held to the rules Settings.set applies. Each valid one is put
    // into @p s, in argv order. With @p record (the boot load) each is also written into the
    // settings store's CommandLine layer, and a malformed one - an unknown path, no value, a
    // value its row refuses - refuses the run (exit 12, like -gipath); without (the Settings
    // bridge's seed) only the valid ones are put. The boot peeks below honour them too.
    // Returns how many were applied.
    int ApplyCommandLineSettings(EngineSettings& s, bool record);

    // Seed the input layer (Input::Get().SetBindings) from loaded settings: the
    // tuning scalars, then the rebind deltas on top of InputBindings::Defaults().
    // Unknown entries are skipped (ApplyBindingOverrides), so a corrupt file still
    // boots playable.
    void ApplyInputBindings(const EngineSettings& settings);

    // Will the settings this process is about to load ask for anything that needs
    // Streamline - DLSS / DLSS_RR anti-aliasing, Reflex, Frame Generation? Reads the SAME
    // bytes LoadEngineSettings(SettingsFilePath()) will (path, shipped-default fallback,
    // migration) but only those fields, and logs nothing: it runs before Streamline
    // initialises. An unreadable or unparsable file answers true (keep Streamline). @p why
    // gets a one-line reason. The harness uses it to skip Streamline where a run cannot use it.
    bool PeekSettingsWantStreamline(std::string& why);

    // The client size settings.json asks for (display.windowWidth / Height), read the
    // same way, before anything else exists: App creates its window at it, so the first
    // frame already has the right window and swap chain. False - keep the caller's
    // default - when the file sets no size, or under -hidden (see the .cpp).
    bool PeekSettingsWindowSize(int& width, int& height);
    // display.adapter as settings.json (and -set:) hold it, before the load: "" = the high-performance
    // adapter. Under -hidden always "" (a harness run's GPU must not depend on the machine's settings).
    std::string PeekSettingsAdapter();
    bool PeekSettingsWantRayReconstruction(std::string& why);
}
