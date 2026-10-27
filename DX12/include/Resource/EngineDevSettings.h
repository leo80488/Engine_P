#pragma once
// engine.json: the DEVELOPER half of the configuration, the third of three documents:
//   game.json      project manifest (scene registry): sealed into game.ipak, read pak-first,
//                  never written at runtime.
//   settings.json  player options (EngineSettings.h): loose and writable, never sealed,
//                  rewritten by every Save.
//   engine.json    developer / perf tuning a player cannot evaluate; game.json's lifecycle.
// A file of its own, not a section of game.json: game.json has no version field
// (SceneManager::LoadManifest reads none) and this document needs one, and a
// developer-toggle Save must never read-modify-write the boot manifest, whose failure is
// a project that boots nowhere.
// Versioning: a pak built by an older engine ships its engine.json into a newer Game.exe,
// which cannot rewrite it, so version skew on read is the normal case. Bump
// kEngineDevConfigVersion only when an existing key changes MEANING; a new key needs no
// bump (every read is j.value(key, default)).
// Read: AssetFS (pak-first), then Sys::ExeDir(). The ExeDir fallback is required: a
// --no-pack build started from a shortcut has a CWD that is not the exe directory, and
// without it every developer value silently reverts to its C++ default.
// Write: editor host only (Sys::IsEditorHost(), never #ifdef WITH_EDITOR: EngineCore is
// compiled once for both exes), to the writable root (the project directory).
// Adding a field: one row in include/Settings/Rows/Developer.inl - its "section.key",
// member, range / clamp, policy and, for a Renderer knob, its apply / capture hooks. Load,
// the clamps, Save, ApplyTo and CaptureFrom all walk the rows; building the table checks
// each row's hooks against its phase. A string with a fixed set of names also needs a name
// rule in ParseEngineDevSettings, which logs its own warning.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Renderer;

namespace Resource
{
    struct EngineDevSettings
    {
        // ---- Geometry / culling path ----
        // Output-identical toggles; only speed differs. gpuCulling and indirectDraw are
        // AND-ed at the consumer, so they live in the same document.
        bool  gpuCulling    = true;
        bool  indirectDraw  = true;
        bool  gpuCompaction = true;
        bool  hiZOcclusion  = true;
        bool  animCulling   = true;
        // Chain-physics distance LOD (hair / cloth). On here although
        // ChainPhysicsSystem::lodEnabled defaults false in C++: this key is what turns it on
        // in a shipped game (the Lua ChainLOD binding is the only other switch).
        bool  chainPhysicsLod = true;
        // Meshlet path. meshShading: draw the eligible library meshes through the
        // amplification + mesh shaders (output-identical at tau 0); a GPU without mesh
        // shaders, or a library without MLET, quietly keeps the VS. meshletLodError: the LOD
        // threshold tau in pixels of projected error (1 px; 0 = always the finest level).
        // meshletShadowLodError: the CSM's tau in shadow-map texels. -meshlets / -meshletlod
        // pin the first two for a run; -meshletsoff turns the path off.
        bool  meshShading           = true;
        float meshletLodError       = 1.0f;
        float meshletShadowLodError = 1.0f;

        // ---- Perf experiments ----
        // The depth prepass wins only on scenes with real overdraw: A/B it. Per-object shadow
        // is a shadow PATH, not a quality tier; the atlas is allocated either way.
        bool  depthPrepass    = false;
        bool  perObjectShadow = true;

        // ---- Ray-tracing internals ----
        // The player-facing RT controls (on/off, ray length) are in settings.json. Here is the
        // tuning a player cannot evaluate: maxRoughness (a quality / cost knob with no natural
        // units), the NRD switch (a denoiser implementation), blend occluders (the walk-back
        // valve for TLAS cost), the budget watchdog (a diagnostic that only fires while the
        // editor GPU profiler is open).
        float rtReflectionsMaxRoughness = 0.55f;
        bool  rtReflectionsUseNRD       = true;
        bool  rtShadowBlendOccluders    = true;
        bool  rtBudgetGuard             = false;
        float rtBudgetMs                = 4.0f;
        int   ptMaxBounces              = 8;
        bool  ptReferenceArm            = true;
        // There is no ptRealtime key: real-time path tracing is the render path "Path Trace
        // (Real-time)" (settings.json features.renderPath = PathTrace_Realtime). A file with
        // raytracing.ptRealtime = true gets one warning at load and the key is ignored.
        bool  ptCameraPrimary           = false;
        int   ptAreaMode                = 0; // Off / temporal / temporal + spatial
        bool  ptAreaMotion              = false;
        int   ptAreaHistory             = 20;
        int   ptAreaSpatialK            = 1;    // Area v2 spatial neighbours (1..4); each costs 2 full replays
        float ptAreaSpatialRadius       = 6.f;  // Area v2 spatial disk radius, pixels
        // Area v2 primary-hit reconnection shift (opt-in; see ReSTIR_PT_Area_V2_Validation.md).
        bool  ptAreaReconnect           = false;
        bool  ptFilmJitter              = false;
        bool  ptPrimaryRayCone          = false;
        float ptLensRadius              = 0.f;
        float ptFocusDistance           = 5.f;
        bool  ptDenoise                 = true;
        int   ptDenoiseFadeSpp          = 64;
        bool  ptFreezeTime              = false;
        float restirBudgetMs            = 0.f; // 0 disables the reversible ReSTIR guard
        // When Streamline loads the DLSS-RR plugin: 0 auto = every interactive run, and an
        // unattended run whose settings ask for DLSS-RR; 1 always; 2 never (-dlss_rr still
        // forces it). Read at boot by PeekEngineDevRayReconstructionLoad, before this struct
        // is loaded.
        int   rayReconstructionLoad     = 0;
        float diRayTMax = 150.f;
        int restirDiSpatialPasses = 1;
        int restirDiSpatialRadiusPx = 16;
        int restirDiK = 3;
        int restirDiMCap = 20;
        // Age caps: 63 = never discard a history because its sample persisted
        // (RTXDI DI has no age rejection). GI keeps a long cap because its
        // selected radiance is re-shaded every frame.
        int restirDiAgeCap = 63;
        int restirGiAgeCap = 30;
        // Temporal + spatial resampling; OFF leaves plain per-frame RIS / one ray.
        bool restirDiReuse = true;
        bool restirGiReuse = true;
        int restirGiSpatialRadiusPx = 8; // trace (half-res) pixels
        float restirGiMaxDistance       = 150.f;
        int   restirGiTier              = 1; // 1 = shadowed hit lights (0 leaks light through walls)
        // The knobs that move ReSTIR cost and quality. -di_candidates, -di_fullres,
        // -gi_no_spatial, -gi_initial_candidates, -gi_emitter_candidates, -gi_fullres and
        // -gi_hit_exact_lights pin them for A/B runs.
        int   restirDiCandidates        = 8;     // initial RIS candidates per pixel
        bool  restirDiFullRes           = false; // trace DI at render resolution
        bool  restirDiSpecAntiFirefly   = true;  // DI specular anti-firefly (RELAX): steady, dims tiny metal highlights
        bool  restirGiSpatial           = true;  // GI spatial stage (with reuse)
        int   restirGiInitialCandidates = 2;     // cosine rays per trace pixel
        int   restirGiEmitterCandidates = 8;     // emitter points per trace pixel
        bool  restirGiFullRes           = false; // trace GI at render resolution (~3.5x cost)
        int   restirGiHitExactLights    = 8;     // exact local lights at a GI hit
        // SHaRC radiance cache at the GI hits: "parity" caches the Tier 1 hit shading (only
        // the cost changes), "multi" caches multi-bounce light. -gi_sharc / -gi_sharc_mode
        // pin them.
        bool        restirGiSharc       = false;
        std::string restirGiSharcMode   = "parity";
        // The DI / GI denoiser: auto | relax | reblur | none. Under DLSS Ray Reconstruction
        // it is the denoiser BEFORE RR (auto: DI none - RR denoises the full-resolution
        // signal; GI relax). -di_no_nrd / -di_nrd_reblur / -gi_no_nrd / -gi_nrd_reblur pin
        // them for one run.
        std::string restirDiDenoiser    = "auto";
        std::string restirGiDenoiser    = "auto";
        // Where the RTXDI PT core takes x1's direct light (PTRtxdi::DirectLight): auto | nee |
        // restir_di | unified. auto = ReSTIR DI, run raw for the path tracer's denoiser.
        // -rtxdi_pt_di / -rtxdi_pt_unified pin it for one run.
        std::string ptDirectLight       = "auto";
        // ReSTIR backends: the core each stage resamples with. DI engine|rtxdi|hybrid, GI
        // engine|rtxdi, PT auto|area|rtxdi (Reference is never a backend); compare off|di|gi
        // runs the other DI / GI core as a shadow pass. An unknown name loads as the default
        // (logged); -di_backend= etc. override for one run.
        // TODO(ReSTIR_Dual_Backend_Plan S8): selectable here to 2026-11-15, CLI-only to 2026-12-13,
        // then deleted: the RTXDI DI / GI cores (DI rtxdi | hybrid, GI rtxdi, compare di | gi).
        std::string restirBackendDI = "engine";
        std::string restirBackendGI = "engine";
        std::string restirBackendPT = "auto";   // auto: rtxdi for real-time PT, area for the reference / thin lens
        std::string restirCompare   = "off";

        // ---- Material surface cache ----
        bool  materialSurfaceCache              = true;
        int   materialSurfaceCachePagesPerFrame = 8;

        // ---- Game flow ----
        // The GameFlow state the title-bar X / Alt+F4 is handed to, so a shipped build can ask
        // "really quit?". Empty (the default), or a state the project's GameFlow does not
        // define, closes immediately. App::InstallWindowHooks reads it once, at boot, for its
        // first close handler (Window::AddCloseHandler, order 0). A hook-less row (phase
        // None): ApplyTo and CaptureFrom leave it alone, so a Save that did not start from the
        // file would wipe it - EditEngineDevSettingsFile, the one caller of
        // SaveEngineDevSettings, loads the file before its edit for that reason.
        std::string gameFlowQuitConfirmState;

        // ---- Diagnostics ----
        // Boot level of the in-viewport stat-unit HUD: 0 Off / 1 Fps / 2 Unit / 3 Full. F9
        // cycles it at runtime and the runtime level is never written back (`stat unit` is
        // the same); a game host cannot write this file anyway. Not in EngineSettingsSchema.inl:
        // that is the player-facing options surface (Lua Settings.list()), and a debug overlay
        // does not belong in a shipped options menu. A hook-less row.
        int debugHudLevel = 0;

        // The streaming upload ring per upload slot, MiB. Read by the device at its first
        // QueueTextureUpload through PeekEngineDevStreamRingMiB - a hook-less row.
        int streamRingMiB = 8;

        // The skinned vertex ring's budget, vertices per frame (0: sized by the adapter). Read by
        // SkinnedVertexRing::Init at boot through PeekEngineDevSkinVertexBudget - a hook-less row.
        int skinVertexBudget = 0;

        // ---- Physics ----
        // Gravity in m/s^2 along Y (negative is down) and the seconds a body must rest before Jolt puts
        // it to sleep. Read by App through PeekEngineDevPhysics and handed to
        // PhysicsSystem::ApplyWorldSettings after Init - hook-less rows.
        // CharacterControllerComponent::gravity (-25) and the chain solver's keep their own.
        float physicsGravity         = -9.81f;
        float physicsTimeBeforeSleep = 0.5f;

        // Pushes these onto the engine: each row's apply hook, in row order. Safe before or
        // after Renderer::Compile(): none is an apply-at-boot value (those - csmResolution,
        // pointShadowResolution, textureAnisotropy - are player settings in settings.json).
        void ApplyTo(Renderer& r) const;

        // Pull the engine's live values back: each row's capture hook. Reads
        // REQUESTED values wherever a capability demotion exists, so a machine
        // that failed to create a buffer does not commit that failure into a
        // sealed file. A row without a capture keeps its value.
        void CaptureFrom(const Renderer& r);
    };

    // See the versioning note at the top of this file.
    inline constexpr int kEngineDevConfigVersion = 1;

    // Canonical document name. Resolved through AssetFS on read (pak-first) and
    // against the writable root on write.
    inline constexpr const char* kEngineDevConfigName = "engine.json";

    // What a load found: the developer settings store's Project layer gets only the rows
    // the file supplied.
    struct EngineDevLoadInfo
    {
        bool                 found  = false;   // the bytes were read
        bool                 parsed = false;   // ...and parsed: the load's answer
        // The -engine_settings path, "engine.json" (pak / project) or the exe-side copy.
        std::string          loadedAs;
        std::vector<uint8_t> rowFromFile;      // per Settings::DevRows() row: the file held it
    };

    // Reads engine.json. Returns false when it is absent or unreadable, leaving @p out at
    // its C++ defaults (an engine without engine.json boots on them). Never fatal.
    bool LoadEngineDevSettings(EngineDevSettings& out, EngineDevLoadInfo* info = nullptr);
    // The boot load (App): LoadEngineDevSettings, the developer settings store's Project
    // layer (the rows the file supplied), then -set:'s engine.json paths into @p out (the
    // CommandLine layer; settings.json's boot load already refused the run over a bad
    // one). Returns the load's answer.
    bool LoadBootEngineDevSettings(EngineDevSettings& out);
    // The same parse of one explicit file (the -enginejson_roundtrip harness). False when
    // it cannot be read or does not parse.
    bool LoadEngineDevSettingsFile(const std::string& path, EngineDevSettings& out);
    // The text SaveEngineDevSettings writes, without writing it.
    std::string SerializeEngineDevSettings(const EngineDevSettings& s);
    // raytracing.rayReconstructionLoad (0 auto, 1 always, 2 never) from the file
    // LoadEngineDevSettings reads, or its -set:, without logging: it runs before the
    // device exists, long before the real load.
    int PeekEngineDevRayReconstructionLoad();
    // upload.streamRingMiB the same way (1..512, default 8, or its -set:), for the
    // device's lazily created stream rings.
    int PeekEngineDevStreamRingMiB();
    // skinning.vertexBudget the same way (0..33554432, default 0, or its -set:), for
    // SkinnedMeshSubsystem's ring budget.
    int PeekEngineDevSkinVertexBudget();
    // physics.gravity and physics.timeBeforeSleep the same way (-200..200 and 0..60, default -9.81 and
    // 0.5, or their -set:), for App to hand to PhysicsSystem::ApplyWorldSettings. A Peek* reader pulls
    // the developer row table and the renderer behind it, so the owners that read one live on the
    // renderer / App side, never in a core system a unit test links on its own.
    struct PhysicsDevSettings { float gravity = -9.81f; float timeBeforeSleep = 0.5f; };
    PhysicsDevSettings PeekEngineDevPhysics();

    // Writes engine.json to the writable root. Editor hosts only: a game host returns
    // false without touching the disk, because this document is sealed content a shipped
    // build never rewrites.
    bool SaveEngineDevSettings(const EngineDevSettings& s);

    // Read-modify-write of engine.json, the one writer behind the editor's "Save to
    // engine.json" and Lua's Settings.saveDeveloper: the file as loaded (unreadable or
    // unparsable = the C++ defaults, unless @p requireFile, which refuses then), then
    // @p edit, then SaveEngineDevSettings (editor hosts only). A save that lands records
    // every row in the developer settings store's Project layer as @p writer.
    bool EditEngineDevSettingsFile(const std::function<void(EngineDevSettings&)>& edit, const char* writer,
                                   bool requireFile = false);

    // Reads engine.json and applies it again (the editor's "Reload engine.json", Lua's
    // Settings.reloadDeveloper), recording what that did: every row with an apply hook now
    // runs the file's value or its default, so it holds the file's value in the Project
    // layer (when the file has the key) and its other layers are cleared - a Runtime or
    // command-line value stops running. Rows without an apply hook were read by their
    // owners at boot and keep their record. @p requireFile: returns false and applies
    // nothing when the file cannot be read or parsed; otherwise such a file reapplies the
    // C++ defaults. Returns the load's answer.
    bool ReloadEngineDevSettings(Renderer& r, const char* writer, bool requireFile);
}
