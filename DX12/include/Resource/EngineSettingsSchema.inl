// EngineSettingsSchema.inl - the PLAYER settings rows (settings.json), once.
//
// Everything that reads or writes a player setting walks the table expanded from
// these rows (Settings/SettingsRows.h): settings.json load / save / sanitize,
// EngineSettings::ApplyTo / CaptureFrom, and the Lua Settings.* bridge
// (LuaSettingsBindings.cpp: get / set / meta / options / range / list / dirty, with
// the in-game options menu on top). Adding a player setting = ONE row here plus its
// EngineSettings member.
//
// The paths are the GROUP NAMES IN settings.json ("features.ssr", not
// "graphics.ssr"): a menu script path, an on-disk key, a log line and a catalog
// entry are the same word. Do not rename one without the others.
//
// MACRO CONTRACT: the includer defines SETTING(Kind, "path", member, .field = value, ...).
// Kind is Bool / Int / Float / Enum / Choice / String; member is the
// Resource::EngineSettings field (type-checked against Kind); the tail is a C++20
// designated initializer of Settings::RowSpec in its declaration order: title, tip,
// lo, hi, labels, values, codec, san, fallback, fallbackStr, flags, phase, apply,
// capture, policy (each documented in Settings/SettingsRows.h).
//
// RANGES (lo / hi) are what a UI offers; SANITIZE (san + fallback) is what
// LoadEngineSettings does to a loaded value. They differ on purpose:
// features.rtReflectionsRayTMax loads unclamped and its setter clamps;
// display.windowWidth / Height follow a both-or-neither pair rule
// (EngineSettings.cpp) instead of a per-row clamp.
//
// APPLY / CAPTURE push a row's value to its owner and read it back, phase by phase
// (Settings::Phase), the rows of a phase in the order written here. A row that is
// applied must also be captured: every editor Save captures into a FRESH
// EngineSettings, so a row without a capture writes its compiled-in default over
// the player's file. Captures read what the player ASKED for, not what runs: the
// requested AA mode, render path and window size, the authored feature gates.
//
// POLICY: most rows are LastWrite - each writer (settings.json, the menu, the
// editor, a flag) calls the setter and the latest write runs; the settings store
// still records each layer. Rows a SCENE also states a value for are Capped: the
// scene's value is its intent, the row's the player's allowance, and both must say
// yes. Rows a QUALITY LEVEL sets (engine/scalability.json, Settings/Scalability.h)
// are Layered or Capped: the level's value sits in the Scalability layer, the
// player's own over it, a -set: over both - a LastWrite row would let the level's
// boot write beat the player's file.

#ifndef SETTING
#error "EngineSettingsSchema.inl needs SETTING(Kind, path, member, ...) defined by the includer"
#endif

// ---- Display / present ----
// The Present rows fill one RHI::PresentSettings, handed once to the grouped setter
// (the path the editor Display menu uses). SetPresentSettings re-queries the live
// panel refresh, so the G-Sync ceiling EffectiveFrameCap() applies stays right
// after gsync changes. Capture reads the same struct back.
//
// frameRateCap is a CHOICE of real FPS values (0 = unlimited), not a range: a tier
// list is a better control than a free number field. A settings.json holding
// another number still loads and applies; Settings.set only refuses to write one,
// so a hand-edited 90 survives until the player touches the control.
SETTING(Choice, "display.frameRateCap", frameRateCap, .title = "Frame Rate Cap",
        .lo = 0, .labels = "Unlimited|30 FPS|60 FPS|120 FPS|144 FPS|165 FPS|180 FPS", .values = "0|30|60|120|144|165|180", .san = San::AtLeast, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.frameRateCap = s.frameRateCap > 0 ? s.frameRateCap : 0; },
        .capture = [](ES& s, CaptureCtx& c) { s.frameRateCap = c.ps.frameRateCap; },
        .policy = Policy::LastWrite)
SETTING(Bool, "display.vsync", vsync, .title = "VSync",
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.vsync = s.vsync; },
        .capture = [](ES& s, CaptureCtx& c) { s.vsync = c.ps.vsync; },
        .policy = Policy::LastWrite)
SETTING(Bool, "display.gsync", gsync, .title = "G-Sync / VRR",
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.gsync = s.gsync; },
        .capture = [](ES& s, CaptureCtx& c) { s.gsync = c.ps.gsync; },
        .policy = Policy::LastWrite)
// Fullscreen is BORDERLESS by decision: the swap chain is created with
// ALLOW_TEARING, DXGI rejects that flag in exclusive fullscreen (every Present
// returns 0x887A0001) and ResizeBuffers cannot add or remove it. A menu labels
// this "Fullscreen" and never offers an exclusive / borderless choice.
//
// A REQUEST, never applied inline: a mode change rebuilds the swap chain, and
// ApplyTo can run from the Graphics Settings button handler between
// BeginImGuiFrame and EndImGuiFrame with primaryCL recording against the current
// back buffer - applying inline destroys that buffer mid-frame and the next ImGui
// draw faults in the driver. Window services the request at the top of the main
// loop; with no window (a headless capture path) it is skipped.
SETTING(Bool, "display.fullscreen", fullscreen, .title = "Fullscreen",
        .phase = Phase::Window,
        .apply   = [](const ES& s, ApplyCtx& c) { if (c.window) c.window->RequestFullscreen(s.fullscreen); },
        .capture = [](ES& s, CaptureCtx& c) { if (c.window) s.fullscreen = c.window->IsFullscreen(); },
        .policy = Policy::LastWrite)
// The internal render resolution lever (the window size is display.windowWidth /
// Height; there is no display-mode enumeration - no GetDisplayModeList /
// EnumDisplayModes). renderScale multiplies the internal render target; the UI
// always composites at display resolution.
SETTING(Float, "display.renderScale", renderScale, .title = "Render Scale",
        .tip = "The 3D render resolution without an upscaler: <1 trades resolution\n"
               "for frames, >1 supersamples. The UI stays at display resolution.",
        .lo = 0.25f, .hi = 2.0f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.renderScale = std::clamp(s.renderScale, 0.25f, 2.0f); },
        .capture = [](ES& s, CaptureCtx& c) { s.renderScale = c.ps.renderScale; },
        .policy = Policy::LastWrite)
// WINDOW client size, not a display mode: fullscreen is borderless and matches
// the output, so there is nothing to pick while fullscreen. 0/0 = leave the window
// alone (what a fresh install resolves to).
//
// Two plain INTs, not a "resolution" enum: the schema describes storage. A menu
// builds its own preset list (filtered against caps().desktopWidth/Height - the
// engine has no display-mode enumeration) and writes both fields together. The
// upper bound is only a sanity ceiling; Window::ServicePendingSize clamps to the
// monitor work area at apply time.
//
// Applying is a REQUEST, like fullscreen (SetWindowPos dispatches WM_SIZE
// synchronously, which rebuilds every back buffer). The two rows only collect the
// size; the Window phase requests it ONCE at its end, after the fullscreen
// request, and Window drops the resize when the result is fullscreen. Capture
// skips both while fullscreen (saving the output size would pin the next windowed
// launch to monitor size) and reads the REQUESTED size: a settings.json resize is
// still latched while the Settings bridge seeds itself at boot, and capturing the
// live size would let the next ApplyTo push the pre-resize value back.
SETTING(Int,    "display.windowWidth",  windowWidth,  .lo = 0, .hi = 16384,
        .phase = Phase::Window,
        .apply   = [](const ES& s, ApplyCtx& c) { c.windowW = s.windowWidth; },
        .capture = [](ES& s, CaptureCtx& c) { if (c.window && !c.window->IsFullscreen()) s.windowWidth = c.window->RequestedClientWidth(); },
        .policy = Policy::LastWrite)
SETTING(Int,    "display.windowHeight", windowHeight, .lo = 0, .hi = 16384,
        .phase = Phase::Window,
        .apply   = [](const ES& s, ApplyCtx& c) { c.windowH = s.windowHeight; },
        .capture = [](ES& s, CaptureCtx& c) { if (c.window && !c.window->IsFullscreen()) s.windowHeight = c.window->RequestedClientHeight(); },
        .policy = Policy::LastWrite)
SETTING(Enum, "display.reflex", reflexMode, .title = "Reflex",
        .labels = "Off|On|On + Boost", .san = San::Range, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.reflexMode = s.reflexMode; },
        .capture = [](ES& s, CaptureCtx& c) { s.reflexMode = c.ps.reflexMode; },
        .policy = Policy::LastWrite)
// frameGen: 0 off, 1..3 = 2x..4x, 4 = dynamic (driver picks). A menu MUST clamp
// its options to caps().frameGenMax and offer "Dynamic" only when
// caps().dynamicMFG: numFramesToGenerateMax is a hard SDK limit, and exceeding it
// fails slDLSSGSetOptions, which turns Frame Generation OFF instead of degrading
// to a lower multiplier.
SETTING(Enum, "display.frameGen", frameGenFrames, .title = "Frame Generation",
        .labels = "Off|2x|3x|4x|Dynamic", .san = San::Range, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.frameGenFrames = s.frameGenFrames; },
        .capture = [](ES& s, CaptureCtx& c) { s.frameGenFrames = c.ps.frameGenFrames; },
        .policy = Policy::LastWrite)

// ---- HDR output ----
// Not gated on caps().hdrCapable by design: that query is wrong on some drivers
// and in remote sessions, and the backend falls back to SDR itself, so
// hard-disabling would lock out displays that work. Warn, do not disable, and show
// caps().hdrCapable next to the control.
//
// HDR rides on the present struct and must not get a call of its own (a mode
// change rebuilds the swap chain, as with fullscreen). Writing it mid-frame is
// safe only because SetPresentSettings LATCHES a mode change to the next frame
// boundary (GraphicsDX12::RequestHDROutputMode). An out-of-range mode degrades to
// SDR. The device is authoritative for all three rows - it clamps the nits and may
// still hold a latched mode - so capture reads them back.
SETTING(Enum, "display.hdrMode", hdrMode, .title = "Output Mode",
        .tip = "Which signal the swap chain hands the display. The scene is composed\n"
               "identically in all three - only the present shader's tail differs.\n"
               "\n"
               "SDR:   8-bit, the classic gamma-encoded output. Unchanged from before\n"
               "       this feature existed; nothing can regress here.\n"
               "HDR10: 10-bit Rec.2020 with the ST.2084 (PQ) transfer - ABSOLUTE\n"
               "       luminance, so the numbers below are literal nits the panel is\n"
               "       asked to produce. What TVs and most HDR monitors expect, and\n"
               "       what exclusive fullscreen passes through untouched.\n"
               "scRGB: FP16 Rec.709 with a LINEAR transfer where 1.0 == 80 nits, and\n"
               "       values may exceed 1.0 (or go negative for out-of-gamut colour).\n"
               "       The Windows compositor converts it for the panel, so it behaves\n"
               "       best in windowed mode. Costs 2x the back-buffer bandwidth.",
        .labels = "SDR|HDR10|scRGB", .codec = &kHDRModeCodec, .san = San::Range, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.hdrMode = static_cast<HDROutputMode>((s.hdrMode >= 0 && s.hdrMode <= 2) ? s.hdrMode : 0); },
        .capture = [](ES& s, CaptureCtx& c) { s.hdrMode = static_cast<int>(c.ps.hdrMode); },
        .policy = Policy::LastWrite)
// The nit ranges are the backend's (Graphics/HDROutput.h). A NaN paper white
// poisons the whole composition space (stored = pow(nits / paperWhite, 1/2.2)) - a
// black screen the player cannot fix - so Clamp tests for NaN first. 0 is "follow
// Windows" (the surface resolves it), so the range starts there; a value between 0
// and the backend's minimum is raised to it by the surface.
SETTING(Float, "display.hdrPaperWhiteNits", hdrPaperWhiteNits, .title = "Paper White (nits)",
        .tip = "The nit level that plain SDR white maps to - i.e. how bright the UI,\n"
               "the editor chrome and any 1.0-valued pixel come out on the panel.\n"
               "It is the divisor in the composition encode, so raising it does not\n"
               "make the image brighter: it makes everything else DIMMER relative to\n"
               "white while freeing more of the panel's range for real highlights.\n"
               "\n"
               "0 follows the SDR content brightness set in Windows' display settings\n"
               "for this monitor (80 to 480 nits). A value overrides it; 200 is Windows'\n"
               "own HDR desktop default. Below ~100 the UI looks murky next to SDR apps;\n"
               "above ~400 it is fatiguing in a dark room.",
        .lo = 0.f, .hi = kHDRPaperWhiteMax, .san = San::Clamp, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.paperWhiteNits = s.hdrPaperWhiteNits; },
        .capture = [](ES& s, CaptureCtx& c) { s.hdrPaperWhiteNits = c.ps.paperWhiteNits; },
        .policy = Policy::LastWrite)
SETTING(Float, "display.hdrPeakNits", hdrPeakNits, .title = "Peak Brightness (nits)",
        .tip = "The brightest value the tone curve rolls off toward. Set it to what\n"
               "the panel can REALLY do - the capability line above reports the OS's\n"
               "own figure. Claiming more peak than the display has does not buy\n"
               "headroom; it just hands the top of the curve to the panel's internal\n"
               "tone mapping, which clips harder than rolling off ourselves does.",
        .lo = kHDRPeakNitsMin, .hi = kHDRPeakNitsMax, .san = San::Clamp, .fallback = kHDRPeakNitsDefault,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.peakNits = s.hdrPeakNits; },
        .capture = [](ES& s, CaptureCtx& c) { s.hdrPeakNits = c.ps.peakNits; },
        .policy = Policy::LastWrite)

// ---- Anti-aliasing / upscaling ----
// aaMode's label count is checked against Renderer::kMaxAAMode at registration;
// out of range applies TAA. SetAAMode demotes DLSS on hardware that cannot run it
// (to FSR at antialiasing.fsrRenderScale's ratio, TAA at Native), so a
// settings.json from an RTX machine lands safely. Capture reads the REQUESTED
// mode, or the first launch on such a machine would rewrite the player's
// preference on disk to the demoted one.
SETTING(Enum, "antialiasing.mode", aaMode, .title = "Anti-Aliasing Mode",
        .labels = "None|FXAA|TAA|FXAA + TAA|FSR|DLSS|DLSS-RR", .codec = &kAAModeCodec,
        .san = San::Range, .fallback = 2,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetAAMode(static_cast<Renderer::AAMode>((s.aaMode >= 0 && s.aaMode <= static_cast<int>(Renderer::kMaxAAMode)) ? s.aaMode : 2)); },
        .capture = [](ES& s, CaptureCtx& c) { s.aaMode = static_cast<int>(c.r.GetAAModeRequested()); },
        .policy = Policy::Layered)
// Applies only while mode is FSR / DLSS / DLSS-RR. The two render-scale controls
// COMPOSE under FSR, so a menu shows exactly one: this enum while an upscaler is
// active, display.renderScale otherwise.
SETTING(Enum, "antialiasing.fsrRenderScale", upscaleMode, .title = "Render Scale",
        .tip = "The upscaler's preset (FSR / DLSS / DLSS-RR): Native is 1:1\n"
               "anti-aliasing (FSR native AA / DLAA), the rest render smaller and\n"
               "let the upscaler resolve to the display resolution.",
        .labels = "Native|Quality|Balanced|Performance|Ultra Performance", .san = San::Range, .fallback = 0,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetUpscaleMode(static_cast<UpscaleMode>((s.upscaleMode >= 0 && s.upscaleMode <= 4) ? s.upscaleMode : 0)); },
        .capture = [](ES& s, CaptureCtx& c) { s.upscaleMode = static_cast<int>(c.r.GetUpscaleMode()); },
        .policy = Policy::Layered)
// 0 driver default, 1 J, 2 K, 3 L, 4 M. Out of range lands on M, not on the driver
// default: the driver's default silently varies per mode (and per OTA), which is
// what this field exists to pin.
SETTING(Enum, "antialiasing.dlssPreset", dlssPreset, .title = "DLSS Model",
        .tip = "Which DL model DLSS runs. The SDK's per-mode defaults make the quality\n"
               "preset change the MODEL as well as the resolution; pinning one makes a\n"
               "quality change a resolution change and nothing else.",
        .labels = "Driver default|J|K|L|M", .san = San::Range, .fallback = 4,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetDLSSPreset(static_cast<SL::DLSSPreset>((s.dlssPreset >= 0 && s.dlssPreset <= 4) ? s.dlssPreset : 4)); },
        .capture = [](ES& s, CaptureCtx& c) { s.dlssPreset = static_cast<int>(c.r.GetDLSSPreset()); },
        .policy = Policy::LastWrite)
// Which model DLSS Ray Reconstruction runs; applies only while mode is DLSS-RR,
// and -dlss_rr_preset overrides it. A STRING row, not an enum: the file stores the
// model letter.
SETTING(String, "antialiasing.dlssRRModel", dlssRRModel, .title = "RR Model",
        .tip = "The DLSS Ray Reconstruction model. F is Streamline 2.14.1's latest and\n"
               "default; -dlss_rr_preset=d|e|f on the command line overrides it.",
        .labels = "D|E|F", .san = San::OneOf, .fallbackStr = "D",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetDLSSRRModel(s.dlssRRModel == "F" ? SL::DLSSRRModel::F : s.dlssRRModel == "E" ? SL::DLSSRRModel::E : SL::DLSSRRModel::D); },
        .capture = [](ES& s, CaptureCtx& c) { s.dlssRRModel = SL::DLSSRRModelName(c.r.GetDLSSRRModelSetting()); },
        .policy = Policy::LastWrite)

// ---- Texture filtering ----
// Sampler descriptors are built once during Renderer::Compile(). 1x deliberately
// selects trilinear filtering instead of D3D12_FILTER_ANISOTROPIC with a max of 1.
SETTING(Choice, "textures.anisotropy", textureAnisotropy, .title = "Anisotropic Filtering",
        .tip = "Controls grazing-angle texture filtering for opaque and transparent\n"
               "materials, terrain layers, and water normals. 1x uses trilinear filtering.\n"
               "Material samplers are created at startup.",
        .labels = "1x (Trilinear)|2x|4x|8x|16x", .values = "1|2|4|8|16", .san = San::OneOf, .fallback = 8, .flags = kSettingRestart,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetTextureAnisotropy(static_cast<uint32_t>(s.textureAnisotropy)); },
        .capture = [](ES& s, CaptureCtx& c) { s.textureAnisotropy = static_cast<int>(c.r.GetTextureAnisotropy()); },
        .policy = Policy::Layered)

// ---- Shadows ----
// The CSM Texture2DArray is sized in ShadowPass::Init, and Renderer::Compile() runs
// once (App boot): ApplyTo accepting the value is NOT applying it, and the engine
// has no restart mechanism - a menu must show the restart marker.
SETTING(Choice, "shadows.csmResolution", csmResolution, .title = "CSM Resolution",
        .tip = "The shadow atlas is created at startup.",
        .lo = 1, .values = "1024|2048|4096", .san = San::AtLeast, .fallback = 4096, .flags = kSettingRestart,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetShadowResolution(static_cast<uint32_t>(s.csmResolution > 0 ? s.csmResolution : 4096)); },
        .capture = [](ES& s, CaptureCtx& c) { s.csmResolution = static_cast<int>(c.r.GetShadowResolution()); },
        .policy = Policy::Layered)
// The shadows quality group's other knobs (Settings/Scalability.h). Distance and
// the spot tier apply the next frame; the point cube face size, like the CSM's, at
// the next launch - PointShadowPass sizes its atlas at Init and the shader reads
// the size back.
SETTING(Float, "shadows.distance", shadowDistance, .title = "Shadow Distance (m)",
        .tip = "How far the sun's three near shadow cascades reach. Past it the far\n"
               "cascade (to 2000 m) still shadows terrain, at its coarse resolution.\n"
               "Shorter spends the near cascades' texels closer to the camera.",
        .lo = 50.f, .hi = 1000.f, .san = San::Clamp, .fallback = 400.0,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetShadowDistance(s.shadowDistance); },
        .capture = [](ES& s, CaptureCtx& c) { s.shadowDistance = c.r.GetShadowDistance(); },
        .policy = Policy::Layered)
SETTING(Choice, "shadows.spotMaxResolution", spotShadowMaxResolution, .title = "Spot Shadow Resolution",
        .tip = "The highest resolution a spot light's shadow map gets. The nearest\n"
               "lights take the 2048 tier (4 of them), then 1024 (16), the rest 512;\n"
               "a lower cap keeps them on the smaller tiers. The atlases stay allocated.",
        .labels = "512|1024|2048", .values = "512|1024|2048", .san = San::OneOf, .fallback = 2048,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetSpotShadowMaxResolution(static_cast<uint32_t>(s.spotShadowMaxResolution)); },
        .capture = [](ES& s, CaptureCtx& c) { s.spotShadowMaxResolution = static_cast<int>(c.r.GetSpotShadowMaxResolution()); },
        .policy = Policy::Layered)
SETTING(Choice, "shadows.pointResolution", pointShadowResolution, .title = "Point Shadow Resolution",
        .tip = "Each face of a point light's shadow cube. The cube atlas (4 lights x 6\n"
               "faces) is created at startup: 1024 is 96 MiB, 2048 is 384 MiB.",
        .labels = "256|512|1024|2048", .values = "256|512|1024|2048", .san = San::OneOf, .fallback = 1024,
        .flags = kSettingRestart,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetPointShadowResolution(static_cast<uint32_t>(s.pointShadowResolution)); },
        .capture = [](ES& s, CaptureCtx& c) { s.pointShadowResolution = static_cast<int>(c.r.GetPointShadowResolution()); },
        .policy = Policy::Layered)

// Path Trace (Reference) and Path Trace (Real-time) are two paths; index 4 stays
// the reference. Phase::Path runs BEFORE Phase::Renderer: SetAAMode ->
// UpdateAAEnabled reads the render path, and with the path applied second TAA
// would stay on across a switch to the path tracer. Capture reads the requested
// path.
SETTING(Enum, "features.renderPath", renderPath, .title = "Render Path",
        .tip = "Path Trace (Reference): the engine path tracer, accumulating while the\n"
               "camera is still - the ground truth (Area reuse, camera rays, thin lens,\n"
               "uncapped bounces); no Ray Reconstruction.\n"
               "Path Trace (Real-time): RTXDI's ReSTIR PT with the SHaRC radiance cache,\n"
               "a fresh estimate every frame - pair it with AA DLSS + Ray Reconstruction.",
        .labels = "Raster|ReSTIR DI|ReSTIR GI|ReSTIR DI+GI|Path Trace (Reference)|Path Trace (Real-time)",
        .codec = &kRenderPathCodec, .san = San::Range, .fallback = 0, .flags = kSettingNeedsDXR,
        .phase = Phase::Path,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetRenderPath(static_cast<Renderer::RenderPath>(s.renderPath)); },
        .capture = [](ES& s, CaptureCtx& c) { s.renderPath = static_cast<int>(c.r.GetRenderPathRequested()); },
        .policy = Policy::LastWrite)

// ---- Render features ----
// The three RT toggles run only on a device with inline ray tracing: the Renderer's
// setters keep the player's request (Renderer::GetRTSwitchRequests) and run the
// switch where the device has inline RT, so the frame is clamped and a Save is not.
// Capture stores the request, or a settings file taken to a machine without inline RT
// would lose its choice. Gate the controls in the UI on caps().inlineRaytracing, or
// they lie about having an effect.
//
// ssao, volumetricFog, ddgi and ssgi go through the AUTHORED setters and are
// CAPPED: the row is the player's allowance, the scene states its intent (the
// .ippc's AO and fog switches via PostProcessConfig::ApplyTo - without an .ippc,
// AO yes and fog no; its Volumetric Fog component and local fog volumes; its
// IndirectLightingSettings), and what runs is both. Capture reads the authored
// mirror, so a Save persists the PLAYER's choice, never the scene's.
SETTING(Bool, "features.ssao", ssao, .title = "Ambient Occlusion (SSAO / XeGTAO)",
        .tip = "Allows screen-space AO where a scene uses it (its .ippc); turning\n"
               "this off turns it off in every scene.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetSSAOAuthored(s.ssao); },
        .capture = [](ES& s, CaptureCtx& c) { s.ssao = c.r.GetSSAOAuthored(); },
        .policy = Policy::Capped)
// The post quality group's: how many directions (slices) and steps per slice
// XeGTAO marches - its cost. Scene keeps the scene's counts (its render features
// config, World Settings > XeGTAO; 3 x 3 without one).
SETTING(Enum, "features.aoQuality", aoQuality, .title = "Ambient Occlusion Quality",
        .tip = "XeGTAO's slices x steps per pixel: Low 1x2, Medium 2x2, Scene - what the\n"
               "scene's render features set (3x3 by default) - Ultra 9x3.",
        .labels = "Low|Medium|Scene|Ultra", .san = San::Range, .fallback = 2,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetAOQuality(s.aoQuality); },
        .capture = [](ES& s, CaptureCtx& c) { s.aoQuality = c.r.GetAOQuality(); },
        .policy = Policy::Layered)
SETTING(Bool, "features.ssr", ssr, .title = "Screen-Space Reflections (SSR)",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetSSREnabled(s.ssr); },
        .capture = [](ES& s, CaptureCtx& c) { s.ssr = c.r.IsSSREnabled(); },
        .policy = Policy::Layered)
SETTING(Bool, "features.rtReflections", rtReflections, .title = "Ray-Traced Reflections (RTR)",
        .tip = "Inline-RayQuery reflections over the scene TLAS (built even in\n"
               "DDGI-less scenes; active DDGI volumes enrich hit lighting).\n"
               "Requires DXR hardware. Rays are GGX importance-sampled by material\n"
               "roughness; SSR keeps priority where it is confident, RTR fills its\n"
               "off-screen / grazing-angle blind spots, probes/sky take the rest.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetRTReflectionsEnabled(s.rtReflections); },
        .capture = [](ES& s, CaptureCtx& c) { s.rtReflections = CaptureRTSwitch(c.r.GetRTSwitchRequests().reflections, c.r.IsRTReflectionsEnabled()); },
        .policy = Policy::LastWrite)
SETTING(Float, "features.rtReflectionsRayTMax", rtReflectionsRayTMax, .title = "RTR Ray Length",
        .tip = "Max world-unit distance a reflection ray travels. Past it the ray\n"
               "misses and the sky cube fills in - this is why distant objects\n"
               "'disappear into the sky' in a mirror when it is set too low.\n"
               "Higher = more hits = more shading work: every off-screen hit runs\n"
               "full material-proxy shading with a secondary shadow ray and up to\n"
               "32 local lights. Watch RTReflectionPass GPU ms when raising it.\n"
               "Also rescales REBLUR's hit-distance normalization, so changing it\n"
               "re-tunes the denoiser and drops the temporal history for a frame.\n"
               "400 matches the CSM far cap.",
        .lo = 20.f, .hi = 400.f,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetRTReflectionsRayTMax(s.rtReflectionsRayTMax); },
        .capture = [](ES& s, CaptureCtx& c) { s.rtReflectionsRayTMax = c.r.GetRTReflectionsRayTMax(); },
        .policy = Policy::Layered)
// Forward translucent surfaces reuse the traced reflection of the opaque surface
// behind them (custom_surface.hlsli MS_FetchTracedReflection). A pure LightCB
// weight: no pass toggles, live the frame it changes, bit-identical to off where
// RTR / SSR traced nothing - so it needs no caps gate.
SETTING(Bool,   "features.transparentReflections", transparentReflections,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetTransparentReflectionsEnabled(s.transparentReflections); },
        .capture = [](ES& s, CaptureCtx& c) { s.transparentReflections = c.r.IsTransparentReflectionsEnabled(); },
        .policy = Policy::Layered)
SETTING(Bool, "features.rtao", rtao, .title = "Ray-Traced AO (RTAO)",
        .tip = "Swaps XeGTAO's screen-space AO source for inline-RayQuery\n"
               "hemisphere rays over the scene TLAS (skinned characters\n"
               "included) - true off-screen occlusion, no screen-edge halo.\n"
               "The XeGTAO denoise/temporal chain runs unchanged on top.\n"
               "Requires DXR hardware; falls back to XeGTAO when no TLAS\n"
               "exists this frame.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetRTAOEnabled(s.rtao); },
        .capture = [](ES& s, CaptureCtx& c) { s.rtao = CaptureRTSwitch(c.r.GetRTSwitchRequests().ao, c.r.IsRTAOEnabled()); },
        .policy = Policy::LastWrite)
SETTING(Bool, "features.rtShadows", rtShadows, .title = "Ray-Traced Sun Shadows",
        .tip = "Full-screen inline-RayQuery shadow rays toward the sun, jittered\n"
               "inside its angular cone for soft penumbras, min()'d with the CSM\n"
               "result (darkest wins - never double-darkens). Skinned characters\n"
               "cast pixel-accurate shadows. Requires DXR hardware + a directional\n"
               "light.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetRTShadowsEnabled(s.rtShadows); },
        .capture = [](ES& s, CaptureCtx& c) { s.rtShadows = CaptureRTSwitch(c.r.GetRTSwitchRequests().shadows, c.r.IsRTShadowsEnabled()); },
        .policy = Policy::LastWrite)
SETTING(Bool, "features.volumetricFog", volumetricFog, .title = "Volumetric Fog",
        .tip = "Allows the fog a scene has - its Volumetric Fog component, its local\n"
               "fog volumes, its .ippc; turning this off turns fog off in every scene.\n"
               "A scene without any of them has no fog either way.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetVolumetricFogAuthored(s.volumetricFog); },
        .capture = [](ES& s, CaptureCtx& c) { s.volumetricFog = c.r.GetVolumetricFogAuthored(); },
        .policy = Policy::Capped)
// DDGI and SSGI are CAPPED: the scene's IndirectLightingSettings says whether its
// world uses them (intent), these rows whether this machine allows them (cap), and
// what runs is both - BuildScene_UpdateDDGI applies the rule the store records
// (Policy::Capped). A scene without the component does not object, so the
// player's value decides there. -ddgi_off still forces DDGI off on top.
SETTING(Bool,   "features.ddgi",          ddgi, .title = "DDGI (probe GI)",
        .tip = "Allows dynamic diffuse GI. A scene can still leave it off (its Indirect\n"
               "Lighting Settings); turning this off turns it off in every scene.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetDDGIAuthored(s.ddgi); },
        .capture = [](ES& s, CaptureCtx& c) { s.ddgi = c.r.GetDDGIAuthored(); },
        .policy = Policy::Capped)
SETTING(Bool,   "features.ssgi",          ssgi, .title = "SSGI (screen-space GI)",
        .tip = "Allows screen-space GI where a scene asks for it (its Indirect\n"
               "Lighting Settings); turning this off turns it off in every scene.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetSSGIAuthored(s.ssgi); },
        .capture = [](ES& s, CaptureCtx& c) { s.ssgi = c.r.GetSSGIAuthored(); },
        .policy = Policy::Capped)

// ---- Geometry detail (the viewDistance quality group) ----
// The meshlet LOD threshold's multiplier: a cluster may coarsen while its
// projected error stays under tau x lodScale pixels (engine.json's
// culling.meshletLodError / meshletShadowLodError are the developer's tau). The
// test is homogeneous in tau, so the scale is exact; tau 0 (LOD off) stays off.
// The vertex-shader path has no LOD.
SETTING(Float, "geometry.lodScale", lodScale, .title = "Geometry Detail (LOD Scale)",
        .tip = "Multiplies the meshlet LOD error threshold: above 1 meshes coarsen\n"
               "sooner (cheaper), below 1 they keep detail longer. Mesh-shader path only.",
        .lo = 0.25f, .hi = 4.0f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx& c) { c.r.SetLodScale(s.lodScale); },
        .capture = [](ES& s, CaptureCtx& c) { s.lodScale = c.r.GetLodScale(); },
        .policy = Policy::Layered)

// ---- Post-process (engine-default profile = the global base look) ----
// These seed the base layer of the volume resolve; a scene's .ppprofile and any
// PostProcessVolume still blend ON TOP, so they are a default, not an override
// (the same caveat as features.ddgi; say so in the UI). .Set() writes the value
// AND marks the property overriding. Each also writes the AUTHORED mirror, which
// capture reads: a scene load can replace the whole engine-default profile
// (PostProcessConfig::ApplyTo), and a Save must persist the player's choice.
SETTING(Bool, "postProcess.cas", ppCAS, .title = "Contrast Adaptive Sharpening (CAS)",
        .phase = Phase::PostProcess,
        .apply   = [](const ES& s, ApplyCtx&) { auto& p = PostProcess::ProfileSystem::Get();
                       p.EngineDefault().cas.enabled.Set(s.ppCAS); p.Authored().cas = s.ppCAS; },
        .capture = [](ES& s, CaptureCtx&) { s.ppCAS = PostProcess::ProfileSystem::Get().Authored().cas; },
        .policy = Policy::LastWrite)
SETTING(Bool, "postProcess.dof", ppDoF, .title = "Depth of Field",
        .phase = Phase::PostProcess,
        .apply   = [](const ES& s, ApplyCtx&) { auto& p = PostProcess::ProfileSystem::Get();
                       p.EngineDefault().dof.enabled.Set(s.ppDoF); p.Authored().dof = s.ppDoF; },
        .capture = [](ES& s, CaptureCtx&) { s.ppDoF = PostProcess::ProfileSystem::Get().Authored().dof; },
        .policy = Policy::LastWrite)
SETTING(Bool, "postProcess.lensFlare", ppLensFlare, .title = "Lens Flare",
        .phase = Phase::PostProcess,
        .apply   = [](const ES& s, ApplyCtx&) { auto& p = PostProcess::ProfileSystem::Get();
                       p.EngineDefault().lensFlare.enabled.Set(s.ppLensFlare); p.Authored().lensFlare = s.ppLensFlare; },
        .capture = [](ES& s, CaptureCtx&) { s.ppLensFlare = PostProcess::ProfileSystem::Get().Authored().lensFlare; },
        .policy = Policy::Layered)
SETTING(Bool, "postProcess.autoExposure", ppAutoExposure, .title = "Auto-Exposure",
        .phase = Phase::PostProcess,
        .apply   = [](const ES& s, ApplyCtx&) { auto& p = PostProcess::ProfileSystem::Get();
                       p.EngineDefault().autoExposure.enabled.Set(s.ppAutoExposure); p.Authored().autoExposure = s.ppAutoExposure; },
        .capture = [](ES& s, CaptureCtx&) { s.ppAutoExposure = PostProcess::ProfileSystem::Get().Authored().autoExposure; },
        .policy = Policy::LastWrite)
SETTING(Bool, "postProcess.colorGrading", ppColorGrading, .title = "Color Grading",
        .phase = Phase::PostProcess,
        .apply   = [](const ES& s, ApplyCtx&) { auto& p = PostProcess::ProfileSystem::Get();
                       p.EngineDefault().colorGrading.enabled.Set(s.ppColorGrading); p.Authored().colorGrading = s.ppColorGrading; },
        .capture = [](ES& s, CaptureCtx&) { s.ppColorGrading = PostProcess::ProfileSystem::Get().Authored().colorGrading; },
        .policy = Policy::LastWrite)

// ---- Audio mixer ----
// LINEAR amplitude gain, 1.0 = unity. The engine has no dB helper or perceptual
// curve, so a raw linear slider does almost nothing over its top half and
// collapses at the bottom: curve it in the UI (gain = t*t is enough) and keep the
// STORED value linear for the [0,2] clamp and the NaN sanitize on load. Applied as
// a BusVolumeChangedEvent and captured from its mirror: AudioEngine has no getter
// (see Resource::PublishBusVolume / LiveBusVolume).
SETTING(Float,  "audio.master",  audioMaster,  .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::Master, s.audioMaster); },
        .capture = [](ES& s, CaptureCtx&) { s.audioMaster = Resource::LiveBusVolume(Audio::BusType::Master); },
        .policy = Policy::LastWrite)
SETTING(Float,  "audio.music",   audioMusic,   .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::Music, s.audioMusic); },
        .capture = [](ES& s, CaptureCtx&) { s.audioMusic = Resource::LiveBusVolume(Audio::BusType::Music); },
        .policy = Policy::LastWrite)
SETTING(Float,  "audio.sfx",     audioSFX,     .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::SFX, s.audioSFX); },
        .capture = [](ES& s, CaptureCtx&) { s.audioSFX = Resource::LiveBusVolume(Audio::BusType::SFX); },
        .policy = Policy::LastWrite)
SETTING(Float,  "audio.voice",   audioVoice,   .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::Voice, s.audioVoice); },
        .capture = [](ES& s, CaptureCtx&) { s.audioVoice = Resource::LiveBusVolume(Audio::BusType::Voice); },
        .policy = Policy::LastWrite)
SETTING(Float,  "audio.ambient", audioAmbient, .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::Ambient, s.audioAmbient); },
        .capture = [](ES& s, CaptureCtx&) { s.audioAmbient = Resource::LiveBusVolume(Audio::BusType::Ambient); },
        .policy = Policy::LastWrite)
SETTING(Float,  "audio.ui",      audioUI,      .lo = 0.f, .hi = 2.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Audio,
        .apply   = [](const ES& s, ApplyCtx&) { Resource::PublishBusVolume(Audio::BusType::UI, s.audioUI); },
        .capture = [](ES& s, CaptureCtx&) { s.audioUI = Resource::LiveBusVolume(Audio::BusType::UI); },
        .policy = Policy::LastWrite)

// ---- UI ----
// Apply-at-boot, like shadows.csmResolution: App::InitUIFont hands it to
// FontLibrary::Init before the main loop, and the atlas pages Init allocates are
// never destroyed before Shutdown (their SRV / bindless slots must stay stable
// under live draws), so there is no runtime re-Init - Phase::Boot, captured only.
// Capture reads the LIVE default font, so a fresh-EngineSettings capture + Save
// keeps the project's font. Guarded on IsReady(): before FontLibrary::Init (or
// after a failed one) Default() is invalid and the loaded value must survive.
SETTING(String, "ui.defaultFont", uiDefaultFont, .flags = kSettingRestart,
        .phase = Phase::Boot,
        .capture = [](ES& s, CaptureCtx&) { if (UI::FontLibrary::Get().IsReady()) s.uiDefaultFont = UI::FontLibrary::Get().Default().TTFPath(); },
        .policy = Policy::LastWrite)
// A language CODE matching a table under asset/scripts/loc/. Not an enum: the
// available set is whichever files exist (content, not a compiled list), and a
// code, unlike an index, does not shift when a language is added. Applied by the
// script layer, not ApplyTo (no engine code reads it), so it is NOT marked
// restart: a menu changes it and re-texts itself immediately
// (asset/scripts/ui/Loc.lua). Phase::None: no engine-side owner, so a capture
// keeps whatever the struct holds - for the Settings bridge, the copy the menu
// wrote.
SETTING(String, "ui.language",    uiLanguage, .policy = Policy::LastWrite)

// ---- Graphics card ----
// Chosen where the device is created (GraphicsDX12::LoadPipeline through PeekSettingsAdapter):
// a restart row with no live owner, so neither applied nor captured - a capture keeps the copy
// the menu wrote. A name no adapter has falls back to the high-performance one, with a warning.
SETTING(String, "display.adapter", displayAdapter, .title = "Graphics Card",
        .tip = "The graphics card the game runs on, by its name; empty = the high-performance one.\n"
               "Takes effect at the next launch.",
        .flags = kSettingRestart, .policy = Policy::LastWrite)

// ---- Accessibility ----
// Camera shake: a scale on every channel's shake (Camera::SetShakeScale), read each frame by the
// camera stack. Colour vision: the present shader's daltonization (Present.ps.hlsl), the last
// stage of the picture, so the HUD and the UI are corrected too.
SETTING(Float, "accessibility.cameraShake", cameraShake, .title = "Camera Shake",
        .tip = "How strongly the camera shakes: 0 = not at all, 1 = as designed.",
        .lo = 0.f, .hi = 1.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx&) { Camera::SetShakeScale(s.cameraShake); },
        .capture = [](ES& s, CaptureCtx&) { s.cameraShake = Camera::ShakeScale(); },
        .policy  = Policy::LastWrite)
SETTING(Enum, "accessibility.colorVision", colorVision, .title = "Color Vision",
        .tip = "Shifts the colours a colour-vision deficiency loses into ones it tells apart.",
        .labels = "Off|Protanopia|Deuteranopia|Tritanopia", .san = San::Range, .fallback = 0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.colorVision = s.colorVision; },
        .capture = [](ES& s, CaptureCtx& c) { s.colorVision = c.ps.colorVision; },
        .policy  = Policy::LastWrite)
SETTING(Float, "accessibility.colorVisionStrength", colorVisionStrength, .title = "Color Vision Strength",
        .tip = "How much of the deficiency the correction assumes: 1 = the full dichromat, lower\n"
               "values suit a milder deficiency. Nothing changes while Color Vision is Off.",
        .lo = 0.f, .hi = 1.f, .san = San::Clamp, .fallback = 1.0,
        .phase = Phase::Present,
        .apply   = [](const ES& s, ApplyCtx& c) { c.ps.colorVisionSeverity = s.colorVisionStrength; },
        .capture = [](ES& s, CaptureCtx& c) { s.colorVisionStrength = c.ps.colorVisionSeverity; },
        .policy  = Policy::LastWrite)

// ---- Quality levels (Settings/Scalability.h) ----
// One level per group of quality rows. engine/scalability.json names each group's
// rows and their value at every level; those values form the settings store's
// Scalability layer, so a row the player sets on its own (the Player layer) sits
// over its level until a level is chosen again. High is every row's default: a
// fresh install changes nothing. Not applied or captured (the rows a level sets
// are); saved by name.
SETTING(Enum, "scalability.shadows", levelShadows, .title = "Shadow Quality",
        .tip = "The shadow rows' preset (engine/scalability.json): the sun's shadow\n"
               "resolution and distance, the spot shadows' highest tier, the point\n"
               "shadows' cube size (the two resolutions apply at the next launch). A row\n"
               "set on its own keeps its value until a level is chosen again.",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.gi", levelGI, .title = "Global Illumination Quality",
        .tip = "The screen-space GI allowance's preset (engine/scalability.json). DDGI and\n"
               "the ray-traced paths are the lighting tier's, not a level's.",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.reflections", levelReflections, .title = "Reflection Quality",
        .tip = "SSR, the reflections forward translucency reuses, and the RT reflection\n"
               "ray length (engine/scalability.json).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.post", levelPost, .title = "Post-Processing Quality",
        .tip = "Ambient occlusion (on / off and its quality) and lens flare\n"
               "(engine/scalability.json).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.textures", levelTextures, .title = "Texture Quality",
        .tip = "Anisotropic filtering (engine/scalability.json; applies at the next launch).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.effects", levelEffects, .title = "Effects Quality",
        .tip = "The volumetric fog allowance (engine/scalability.json).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.viewDistance", levelViewDistance, .title = "View Distance Quality",
        .tip = "The meshlet LOD scale - how soon meshes coarsen with distance\n"
               "(engine/scalability.json).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)
SETTING(Enum, "scalability.aa", levelAA, .title = "Anti-Aliasing Quality",
        .tip = "The anti-aliasing mode and the upscaler's ratio (engine/scalability.json):\n"
               "Low / Medium upscale with DLSS, High is TAA, Ultra / Cinematic DLAA. A\n"
               "machine without DLSS runs FSR at the same ratio (TAA at Native).",
        .labels = "Low|Medium|High|Ultra|Cinematic", .codec = &kLevelCodec, .san = San::Range, .fallback = 2,
        .policy = Policy::Layered)

// ---- Face tracking ----
// The camera permission. Applied to the FaceTracking module's service through its bridge
// (Settings/SettingBridge.h), which refuses to list or open a camera while it is off;
// captured from it. Without the module the value is only kept. Phase::Renderer only orders
// it among the live rows - the service needs no renderer.
SETTING(Bool, "faceTracking.camera", faceTrackingCamera, .title = "Face Tracking: Use the Camera",
        .tip = "Face tracking may use a camera only while this is on. The picture stays on this computer and is never saved or sent anywhere: only the face's movements drive the character.",
        .phase = Phase::Renderer,
        .apply   = [](const ES& s, ApplyCtx&) {
            if (const auto* b = Settings::FindBoolBridge("faceTracking.camera")) b->apply(s.faceTrackingCamera); },
        .capture = [](ES& s, CaptureCtx&) {
            if (const auto* b = Settings::FindBoolBridge("faceTracking.camera")) s.faceTrackingCamera = b->capture(); },
        .policy  = Policy::LastWrite)

// ==== Not in this table, on purpose ====
//  * EngineSettings::input / inputOverrides: the input half of settings.json has
//    its own script surface (Input.GetBinding / SetBinding / GetTuning / SetTuning
//    / ResetBindings / ResetTuning / SaveBindings); mirroring it would give two
//    writers and two save paths. Input.SaveBindings rewrites ONLY the input block,
//    so an "apply controls" button cannot rewrite graphics settings, and
//    Settings.save() touches everything EXCEPT it. Its load / save stays
//    hand-written in EngineSettings.cpp (an array, not a row).
//  * EngineDevSettings (engine.json): developer configuration, sealed into
//    game.ipak, never written by a game host; its rows are Settings/Rows/Developer.inl
//    (the RTR max roughness, RTR NRD denoiser, translucent occluders and material
//    surface cache knobs among them).
//  * FSR sharpening + sharpness: live editor-panel knobs with no settings field at
//    all, so a control here would forget on restart.
