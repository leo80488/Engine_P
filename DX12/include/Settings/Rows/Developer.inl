// Settings/Rows/Developer.inl - the developer settings rows (engine.json), one per key,
// each a member of Resource::EngineDevSettings (EngineDevSettings.h keeps each field's
// rationale). engine.json's load / save / sanitize and EngineDevSettings::ApplyTo /
// CaptureFrom walk these rows. The five name rules (the denoisers, ptDirectLight, the
// backends, compare: parsed to the canonical spelling, warning on anything else) and the
// warning for the retired raytracing.ptRealtime key stay hand-written in
// EngineDevSettings.cpp, because each logs its own warning.
//
// The path is the file's "section.key", not always the member's name
// (raytracing.reflectionsMaxRoughness is rtReflectionsMaxRoughness).
//
// Macro contract, as in EngineSettingsSchema.inl: SETTING(Kind, "path", member,
// .field = value, ...), the tail a designated initializer of
// Settings::RowSpecT<EngineDevSettings> (fields in RowSpecT's order). The hooks reach
// only the Renderer: DevApplyCtx / DevCaptureCtx.
//
// Labels on a String row are its legal names: -set:, the pipe and Settings.setRuntime
// take only those (Coerce), while the file's load keeps its name rules (any case, an
// index, a warning). DevRows.cpp checks them against the C++ tables the hooks parse with.
//
// Order is the apply order: culling, perf, ray tracing, the surface cache. Phase:
// Renderer for every row ApplyTo applies and CaptureFrom reads back; None for the keys
// their owner reads from the file itself (raytracing.rayReconstructionLoad - the boot
// peek; gameFlow and debug - App; upload.streamRingMiB - the device;
// skinning.vertexBudget - the skinned vertex ring); Applied for culling.chainPhysicsLod
// (applied, deliberately not captured - see the row).
//
// Policy: LastWrite (engine.json, the editor's Developer page, command-line flags and
// the Lua developer setters each call the setter), except the meshlet path's two knobs
// (meshShading, meshletLodError): Layered, as their -meshlets / -meshletlod pins outrank
// every other writer.

#ifndef SETTING
#error "Developer.inl needs SETTING(Kind, path, member, ...) defined by the includer"
#endif

// ---- Geometry / culling path ----
SETTING(Bool, "culling.gpuCulling", gpuCulling, .title = "GPU Frustum Culling",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetGPUCullingEnabled(s.gpuCulling); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.gpuCulling = c.r.IsGPUCullingEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "culling.indirectDraw", indirectDraw, .title = "Indirect Draw",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetIndirectDrawEnabled(s.indirectDraw); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.indirectDraw = c.r.IsIndirectDrawEnabled(); },
        .policy = Policy::LastWrite)
// Captured as requested: a boot-time buffer-creation failure demotes the live value,
// and this file is sealed into the pak, so one broken dev machine would ship
// "compaction off" to everyone.
SETTING(Bool, "culling.gpuCompaction", gpuCompaction, .title = "GPU Compaction (M6)",
        .tip = "Per-instance GPU cull + count buffers. OFF falls back to\n"
               "per-command zero-out culling (M5 path).",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetGPUCompactionEnabled(s.gpuCompaction); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.gpuCompaction = c.r.GetGPUCompactionRequested(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "culling.hiZOcclusion", hiZOcclusion, .title = "Hi-Z Occlusion",
        .tip = "Previous-frame depth-pyramid occlusion test inside the\n"
               "GPU cull. OFF keeps frustum-only culling.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetHiZOcclusionEnabled(s.hiZOcclusion); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.hiZOcclusion = c.r.IsHiZOcclusionEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "culling.animCulling", animCulling, .title = "Animation Culling",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetAnimCullingEnabled(s.animCulling); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.animCulling = c.r.IsAnimCullingEnabled(); },
        .policy = Policy::LastWrite)
// Applied, not captured, on purpose: ChainPhysicsSystem::lodEnabled is also forced off
// by the Chain Physics Preview panel while it is open and written by Lua's
// ChainLOD.SetEnabled, runtime values a Save must not seal into content. No editor
// widget sets it, so the read-modify-write Save keeps the file's value.
SETTING(Bool, "culling.chainPhysicsLod", chainPhysicsLod,
        .phase = Phase::Applied,
        .apply   = [](const DS& s, DevApplyCtx& c) { if (auto* cps = c.r.GetChainPhysicsSystem()) cps->lodEnabled = s.chainPhysicsLod; },
        .policy = Policy::LastWrite)
SETTING(Bool, "culling.meshShading", meshShading, .title = "Mesh Shading",
        .tip = "Draw library meshes that carry meshlets (an MLET chunk) through\n"
               "the amplification + mesh shaders: per-cluster frustum / cone culling\n"
               "and LOD. Output-identical with LOD 0 (Meshlet_MeshShader_Plan.md).\n"
               "Needs a GPU with mesh shaders; skinned / custom-VS draws stay on the VS.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetMeshShadingEnabled(s.meshShading); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.meshShading = c.r.IsMeshShadingEnabled(); })
// tau in pixels of projected error (0 = always the finest level); the CSM's in
// shadow-map texels. The load does not clamp them; the range is a UI's.
SETTING(Float, "culling.meshletLodError", meshletLodError, .title = "Meshlet LOD error (px)",
        .tip = "Coarser levels are drawn while their projected error stays under\n"
               "this many pixels. 0 = always the finest level (level 0).",
        .lo = 0.f, .hi = 64.f,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetMeshletLodError(s.meshletLodError); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.meshletLodError = c.r.GetMeshletLodError(); })
SETTING(Float, "culling.meshletShadowLodError", meshletShadowLodError, .title = "Meshlet shadow LOD error (texels)",
        .tip = "The cascaded shadow maps' threshold, in shadow-map texels.",
        .lo = 0.f, .hi = 64.f,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetMeshletShadowLodError(s.meshletShadowLodError); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.meshletShadowLodError = c.r.GetMeshletShadowLodError(); },
        .policy = Policy::LastWrite)

// ---- Perf experiments ----
SETTING(Bool, "perf.depthPrepass", depthPrepass, .title = "Depth Prepass",
        .tip = "Fills depth first so the 6-MRT GBuffer early-Z rejects occluded\n"
               "fragments instead of shading them (~28 B/pixel per overdrawn\n"
               "fragment). Costs one extra geometry pass, so it is a NET WIN\n"
               "ONLY IF this scene actually has overdraw - A/B it against the\n"
               "frame time, do not leave it on by assumption.\n"
               "\n"
               "GBuffer depth state is unchanged (GREATER_EQUAL, depth-write\n"
               "on), so draws the prepass skips still render normally - there\n"
               "is no 'objects vanish' failure mode. Draws that can discard\n"
               "(alpha-test, custom PS, dissolve) are excluded.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetDepthPrepassEnabled(s.depthPrepass); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.depthPrepass = c.r.IsDepthPrepassEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "perf.perObjectShadow", perObjectShadow, .title = "Per-Object (Player) Shadow",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPerObjectShadowEnabled(s.perObjectShadow); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.perObjectShadow = c.r.IsPerObjectShadowEnabled(); },
        .policy = Policy::LastWrite)

// ---- Ray-tracing internals ----
SETTING(Float, "raytracing.reflectionsMaxRoughness", rtReflectionsMaxRoughness, .title = "RTR Max Roughness",
        .tip = "Surfaces rougher than this fall back to probe/sky reflections.\n"
               "Higher = more surfaces traced = more rays + more denoiser load.\n"
               "0.15 ~= mirror-only (old behaviour), 0.55 = default glossy band.",
        .lo = 0.f, .hi = 1.f, .san = San::Clamp, .fallback = 0.55,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRTReflectionsMaxRoughness(s.rtReflectionsMaxRoughness); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.rtReflectionsMaxRoughness = c.r.GetRTReflectionsMaxRoughness(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "raytracing.reflectionsUseNRD", rtReflectionsUseNRD, .title = "RTR Denoiser: NVIDIA NRD (REBLUR)",
        .tip = "ON: full-rate trace + NVIDIA NRD REBLUR_SPECULAR denoiser\n"
               "     (higher quality, more rays, needs the NRD runtime).\n"
               "OFF: sparse trace + built-in temporal/spatial denoiser.\n"
               "Falls back to the built-in chain automatically if NRD\n"
               "failed to initialize (see log).",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRTReflectionsNRDEnabled(s.rtReflectionsUseNRD); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.rtReflectionsUseNRD = c.r.IsRTReflectionsNRDEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "raytracing.shadowBlendOccluders", rtShadowBlendOccluders, .title = "Translucent Occluders",
        .tip = "ON (default): alpha-blend objects (glass, water planes, smoke\n"
               "cards) cast a PARTIAL shadow - a 30%-opaque window darkens its\n"
               "shadow by 30% instead of casting a solid silhouette. Additive\n"
               "and Multiply blend modes never cast (they add light, not absorb).\n"
               "\n"
               "OFF: those objects stay fully opaque occluders, exactly as before.\n"
               "This is the walk-back valve - ON makes their instances non-opaque\n"
               "in the TLAS, so a shadow ray no longer terminates on the glass and\n"
               "keeps traversing behind it. Turn OFF if RTShadowPass GPU ms spikes\n"
               "on glass/smoke-heavy content.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRTShadowBlendOccluders(s.rtShadowBlendOccluders); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.rtShadowBlendOccluders = c.r.IsRTShadowBlendOccludersEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "raytracing.budgetGuard", rtBudgetGuard, .title = "RT Budget Watchdog (auto-disable)",
        .tip = "OFF (default): RT Reflections / RT Sun Shadows stay on no matter\n"
               "how expensive they get - you own the perf budget.\n"
               "ON: a rolling GPU-time average over the ceiling below for ~3 s\n"
               "hard-disables that feature (logged as a warning).\n"
               "Only ever fires while the Profiler window is open, which is why\n"
               "it is opt-in. The always-on caps (ray TMax, pixel-list capacity,\n"
               "RTR max roughness) are unaffected either way.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRTBudgetGuardEnabled(s.rtBudgetGuard); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.rtBudgetGuard = c.r.IsRTBudgetGuardEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Float, "raytracing.budgetMs", rtBudgetMs, .title = "RT Budget (ms)",
        .tip = "Shared GPU-time ceiling for the RTReflectionPass and\n"
               "RTShadowPass watchdogs. 4 ms is the historical default.",
        .lo = 0.5f, .hi = 32.f, .san = San::Clamp, .fallback = 4.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRTBudgetMs(s.rtBudgetMs); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.rtBudgetMs = c.r.GetRTBudgetMs(); },
        .policy = Policy::LastWrite)
// The path tracer's rows below take the old -pt_* flags as their old spellings
// (Settings/LegacyFlags.h). Rules that hold for a command-line value only (the auto
// backend, the camera modes held off the reference path, RRCameraRaysForced) see it
// through DevApplyCtx::fromCommandLine (Renderer::DevPin).
SETTING(Int, "raytracing.ptMaxBounces", ptMaxBounces, .title = "PT Max Bounces",
        .lo = 1, .hi = 64, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTMaxBounces(static_cast<uint32_t>(std::clamp(s.ptMaxBounces, 1, 64)));
                                                       c.r.SetDevPinned(Renderer::DevPin::PTMaxBounces, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptMaxBounces = static_cast<int>(c.r.GetPTMaxBounces()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_bounces")
SETTING(Bool, "raytracing.ptReferenceArm", ptReferenceArm, .title = "PT Uncapped Reference",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTReferenceArm(s.ptReferenceArm);
                                                       c.r.SetDevPinned(Renderer::DevPin::PTReferenceArm, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptReferenceArm = c.r.GetPTReferenceArm(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_reference=true")
SETTING(Bool, "raytracing.ptCameraPrimary", ptCameraPrimary, .title = "PT Camera Rays (experimental)",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTCameraPrimary(s.ptCameraPrimary);
                                                       c.r.SetDevPinned(Renderer::DevPin::PTCameraPrimary, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptCameraPrimary = c.r.GetPTCameraPrimary(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_camera_primary=true")
// Off / temporal / temporal + spatial.
SETTING(Int, "raytracing.ptAreaMode", ptAreaMode, .title = "PT Area ReSTIR",
        .tip = "Area ReSTIR: film/lens random replay and primary-hit reconnection, MIS-combined.\n"
               "History survives camera motion and animation; hard cuts discard it.",
        .lo = 0, .hi = 2, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaMode(static_cast<uint32_t>(std::clamp(s.ptAreaMode, 0, 2))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaMode = static_cast<int>(c.r.GetPTAreaMode()); },
        .policy = Policy::LastWrite)
SETTING(Bool, "raytracing.ptAreaMotion", ptAreaMotion,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaMotion(s.ptAreaMotion);
                                                       c.r.SetDevPinned(Renderer::DevPin::PTAreaMotion, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaMotion = c.r.GetPTAreaMotion(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_area_motion=true")
SETTING(Int, "raytracing.ptAreaHistory", ptAreaHistory, .title = "Area History Limit",
        .tip = "1 disables temporal reuse. Higher values retain more candidate weight.",
        .lo = 1, .hi = 64, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaHistory(static_cast<uint32_t>(std::clamp(s.ptAreaHistory, 1, 64))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaHistory = static_cast<int>(c.r.GetPTAreaHistory()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_area_history")
SETTING(Int, "raytracing.ptAreaSpatialK", ptAreaSpatialK, .title = "Area Spatial Neighbours",
        .lo = 1, .hi = 4, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaSpatialK(static_cast<uint32_t>(std::clamp(s.ptAreaSpatialK, 1, 4))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaSpatialK = static_cast<int>(c.r.GetPTAreaSpatialK()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_area_spatial_k")
SETTING(Float, "raytracing.ptAreaSpatialRadius", ptAreaSpatialRadius, .title = "Area Spatial Radius (px)",
        .lo = 1.f, .hi = 32.f, .san = San::ClampFinite, .fallback = 6.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaSpatialRadius(s.ptAreaSpatialRadius); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaSpatialRadius = c.r.GetPTAreaSpatialRadius(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_area_spatial_radius")
SETTING(Bool, "raytracing.ptAreaReconnect", ptAreaReconnect, .title = "Area Reconnection Shift",
        .tip = "Experimental: also reuse defocused samples by reconnecting at their primary hit (lens radius > 0).\n"
               "Unbiased, but it measured noisier than replay alone during camera motion.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTAreaReconnect(s.ptAreaReconnect); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptAreaReconnect = c.r.GetPTAreaReconnect(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_area_no_reconnect=false|-pt_area_v2_reconnect=true")
SETTING(Bool, "raytracing.ptFilmJitter", ptFilmJitter, .title = "PT Film Jitter",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTFilmJitter(s.ptFilmJitter);
                                                       c.r.SetDevPinned(Renderer::DevPin::PTFilmJitter, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptFilmJitter = c.r.GetPTFilmJitter(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_film_jitter=true")
SETTING(Bool, "raytracing.ptPrimaryRayCone", ptPrimaryRayCone, .title = "PT Primary Texture LOD",
        .tip = "Filters textures at the camera's first hit. Secondary-bounce cone propagation is not yet supported.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTPrimaryRayCone(s.ptPrimaryRayCone); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptPrimaryRayCone = c.r.GetPTPrimaryRayCone(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_primary_ray_cone=true")
SETTING(Float, "raytracing.ptLensRadius", ptLensRadius, .title = "PT Lens Radius",
        .tip = "World-space aperture radius. A positive radius enables camera rays and optical depth of field.",
        .lo = 0.f, .hi = 1.f, .san = San::ClampFinite, .fallback = 0.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTLensRadius(s.ptLensRadius);
                                                       c.r.SetDevPinned(Renderer::DevPin::PTLensRadius, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptLensRadius = c.r.GetPTLensRadius(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_lens_radius")
SETTING(Float, "raytracing.ptFocusDistance", ptFocusDistance, .title = "PT Focus Distance",
        .lo = 0.01f, .hi = 10000.f, .san = San::ClampFinite, .fallback = 5.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTFocusDistance(s.ptFocusDistance); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptFocusDistance = c.r.GetPTFocusDistance(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-pt_focus_distance")
// The reference path tracer's preview denoiser: frames that restart the image (a moving scene or
// camera) and the first samples after them are filtered, faded to the plain running mean by
// ptDenoiseFadeSpp samples.
SETTING(Bool, "raytracing.ptDenoise", ptDenoise, .title = "PT Preview Denoiser",
        .tip = "Filters the young image (fewer samples than the fade) so motion is not raw noise.\n"
               "From the fade sample count on, the image is the plain running mean.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTDenoise(s.ptDenoise); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptDenoise = c.r.GetPTDenoise(); },
        .policy = Policy::LastWrite)
SETTING(Int, "raytracing.ptDenoiseFadeSpp", ptDenoiseFadeSpp, .title = "PT Denoiser Fade (spp)",
        .tip = "The sample count where the denoised image has faded into the running mean\n"
               "(the fade starts at half of it). 0 filters only frames that restart.",
        .lo = 0, .hi = 4096, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTDenoiseFadeSpp(static_cast<uint32_t>(std::clamp(s.ptDenoiseFadeSpp, 0, 4096))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptDenoiseFadeSpp = static_cast<int>(c.r.GetPTDenoiseFadeSpp()); },
        .policy = Policy::LastWrite)
// The path tracer's time freeze, on the reference path only: the simulation and every world
// clock stop (animation, chains, scripts, physics, time of day, clouds, water, particles,
// material time) so the image converges; the camera and the editor keep running.
SETTING(Bool, "raytracing.ptFreezeTime", ptFreezeTime, .title = "PT Freeze Time",
        .tip = "Stops the world's clocks while Path Trace (Reference) renders, so the image converges.\n"
               "The camera still moves. Lua Render.ptStep(seconds) advances one step.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetPTFreezeTime(s.ptFreezeTime); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptFreezeTime = c.r.GetPTFreezeTime(); },
        .policy = Policy::LastWrite)
// 0 disables the reversible ReSTIR guard.
SETTING(Float, "raytracing.restirBudgetMs", restirBudgetMs, .title = "ReSTIR Budget (ms; 0 disables)",
        .tip = "Above this GPU cost the ReSTIR paths step down: 1 = one spatial\n"
               "neighbour, 2 = no spatial, 4 = Raster (ReSTIRBudget.h).",
        .lo = 0.f, .hi = 1000.f, .san = San::Clamp, .fallback = 0.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirBudgetMs(s.restirBudgetMs); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirBudgetMs = c.r.GetRestirBudgetMs(); },
        .policy = Policy::LastWrite)
// 0 auto / 1 always / 2 never. Read by the boot peek
// (PeekEngineDevRayReconstructionLoad) before Streamline initialises, so
// neither ApplyTo nor CaptureFrom has anything to do with it.
SETTING(Int, "raytracing.rayReconstructionLoad", rayReconstructionLoad, .lo = 0, .hi = 2, .san = San::Clamp,
        .policy = Policy::LastWrite)
// ReSTIR DI's rows take the old -di_* flags as their old spellings
// (Settings/LegacyFlags.h). Rules that hold for a command-line value only
// (-di_visibility_confidence's reuse unless reuse is pinned off, two pinned passes'
// radius 8, the RTXDI core's ignored report) see it through
// DevApplyCtx::fromCommandLine (Renderer::DevPin).
SETTING(Float, "raytracing.diRayTMax", diRayTMax, .title = "DI Ray Distance",
        .lo = 1.f, .hi = 1000.f, .san = San::Clamp, .fallback = 150.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetDIRayTMax(s.diRayTMax); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.diRayTMax = c.r.GetDIRayTMax(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_ray_tmax")
SETTING(Int, "raytracing.restirDiSpatialPasses", restirDiSpatialPasses, .title = "DI Spatial Passes",
        .tip = "Spatial reuse passes after the temporal merge (0 = none). Needs temporal reuse.",
        .lo = 0, .hi = 2, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDISpatialPasses(static_cast<uint32_t>(s.restirDiSpatialPasses));
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirDiSpatialPasses, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiSpatialPasses = static_cast<int>(c.r.GetRestirDISpatialPasses()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_spatial_passes")
SETTING(Int, "raytracing.restirDiSpatialRadiusPx", restirDiSpatialRadiusPx, .title = "DI Spatial Radius",
        .lo = 0, .hi = 64, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDISpatialRadius(static_cast<uint32_t>(s.restirDiSpatialRadiusPx));
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirDiSpatialRadius, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiSpatialRadiusPx = static_cast<int>(c.r.GetRestirDISpatialRadius()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_spatial_radius")
SETTING(Int, "raytracing.restirDiK", restirDiK, .title = "DI Spatial Neighbours (K)",
        .lo = 1, .hi = 8, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDIK(static_cast<uint32_t>(s.restirDiK)); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiK = static_cast<int>(c.r.GetRestirDIK()); },
        .policy = Policy::LastWrite)
SETTING(Int, "raytracing.restirDiMCap", restirDiMCap, .title = "DI M Cap",
        .lo = 1, .hi = 63, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDIMCap(static_cast<uint32_t>(s.restirDiMCap)); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiMCap = static_cast<int>(c.r.GetRestirDIMCap()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_m_cap")
// 63 = never discard a history because its sample persisted (RTXDI DI has no
// age rejection); GI keeps a long cap because its radiance is re-shaded.
SETTING(Int, "raytracing.restirDiAgeCap", restirDiAgeCap, .title = "DI Age Cap",
        .tip = "63 = never drop a history for persisting (RTXDI behaviour).",
        .lo = 1, .hi = 63, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDIAgeCap(static_cast<uint32_t>(s.restirDiAgeCap));
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirDiAgeCap, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiAgeCap = static_cast<int>(c.r.GetRestirDIAgeCap()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_age_cap")
// ReSTIR GI's rows take the old -gi_* flags as their old spellings. A command-line
// spatial radius is taken as given at full resolution (not doubled), and a command-line
// SHaRC "off" names itself as the reason: DevPin.
SETTING(Int, "raytracing.restirGiAgeCap", restirGiAgeCap, .title = "GI Age Cap",
        .lo = 1, .hi = 63, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIAgeCap(static_cast<uint32_t>(s.restirGiAgeCap)); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiAgeCap = static_cast<int>(c.r.GetRestirGIAgeCap()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_age_cap")
SETTING(Bool, "raytracing.restirDiReuse", restirDiReuse, .title = "DI Temporal Reuse",
        .tip = "Off = per-frame RIS only: no temporal or spatial reuse (the A/B control).",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDIReuse(s.restirDiReuse);
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirDiReuse, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiReuse = c.r.GetRestirDIReuse(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_no_reuse=false|-di_reuse=true")
SETTING(Bool, "raytracing.restirGiReuse", restirGiReuse, .title = "GI Temporal Reuse",
        .tip = "Off = per-frame candidates only: no temporal or spatial reuse (the A/B control).",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIReuse(s.restirGiReuse); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiReuse = c.r.GetRestirGIReuse(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_no_reuse=false|-gi_reuse=true")
// In trace (half-res) pixels.
SETTING(Int, "raytracing.restirGiSpatialRadiusPx", restirGiSpatialRadiusPx, .title = "GI Spatial Radius",
        .tip = "Half-resolution trace pixels; doubled automatically at full resolution.",
        .lo = 0, .hi = 32, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGISpatialRadius(static_cast<uint32_t>(s.restirGiSpatialRadiusPx));
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirGiSpatialRadius, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiSpatialRadiusPx = static_cast<int>(c.r.GetRestirGISpatialRadius()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_spatial_radius")
SETTING(Float, "raytracing.restirGiMaxDistance", restirGiMaxDistance, .title = "GI Max Distance (m)",
        .lo = 1.f, .hi = 1000.f, .san = San::Clamp, .fallback = 150.0,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIMaxDistance(s.restirGiMaxDistance); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiMaxDistance = c.r.GetRestirGIMaxDistance(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_ray_tmax")
// 1 = shadowed hit lights (0 leaks light through walls).
SETTING(Int, "raytracing.restirGiTier", restirGiTier, .title = "GI Hit Shading",
        .tip = "Tier 0: coarse diffuse - hit lights are unshadowed, so light leaks\n"
               "through walls. Tier 1: local shadows. The radiance cache holds Tier 1\n"
               "shading, so it is Tier 1 while the cache is requested.",
        .lo = 0, .hi = 1, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGITier(static_cast<uint32_t>(std::clamp(s.restirGiTier, 0, 1))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiTier = static_cast<int>(c.r.GetRestirGITier()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_tier")
// Cost / quality knobs (the -di_* / -gi_* flags are their old spellings).
SETTING(Int, "raytracing.restirDiCandidates", restirDiCandidates, .title = "DI Candidates",
        .tip = "Initial light candidates per trace pixel (RIS). Cost grows linearly.",
        .lo = 1, .hi = 32, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDICandidates(static_cast<uint32_t>(std::clamp(s.restirDiCandidates, 1, 32))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiCandidates = static_cast<int>(c.r.GetRestirDICandidates()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_candidates")
SETTING(Bool, "raytracing.restirDiFullRes", restirDiFullRes, .title = "DI Full Resolution",
        .tip = "Trace every render pixel instead of a 2x2 phase at half resolution:\n"
               "sharper shadow edges, about 4x the trace cost. Rebuilds the working\n"
               "set and drops the history once. Under DLSS Ray Reconstruction DI\n"
               "always traces at full resolution: RR cannot remove the half-resolution\n"
               "upsample's correlated noise (it boils).",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDIFullRes(s.restirDiFullRes); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiFullRes = c.r.GetRestirDIFullRes(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_fullres=true")
SETTING(Bool, "raytracing.restirDiSpecAntiFirefly", restirDiSpecAntiFirefly, .title = "DI Specular Anti-Firefly",
        .tip = "NRD's firefly suppression on the DI specular denoiser. On: steady lamp\n"
               "highlights under TAA / DLSS (sub-pixel highlights blink without it). Off:\n"
               "full highlight energy - on SchoolNight it dims small metal highlights by\n"
               "~16% against the path tracer. No effect without NRD (DI Denoiser None,\n"
               "or Auto under Ray Reconstruction). engine.json restirDiSpecAntiFirefly;\n"
               "-di_no_nrd_antifirefly.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirDISpecAntiFirefly(s.restirDiSpecAntiFirefly); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiSpecAntiFirefly = c.r.GetRestirDISpecAntiFirefly(); },
        .policy = Policy::LastWrite)
SETTING(Bool, "raytracing.restirGiSpatial", restirGiSpatial, .title = "GI Spatial Reuse",
        .tip = "Neighbour reuse after the temporal merge. Needs temporal reuse.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGISpatial(s.restirGiSpatial); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiSpatial = c.r.GetRestirGISpatial(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_no_spatial=false")
SETTING(Int, "raytracing.restirGiInitialCandidates", restirGiInitialCandidates, .title = "GI Cosine Rays",
        .tip = "Cosine-distributed initial rays per trace pixel. Each costs a full trace + hit shading.",
        .lo = 1, .hi = 16, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIInitialCandidates(static_cast<uint32_t>(std::clamp(s.restirGiInitialCandidates, 1, 16))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiInitialCandidates = static_cast<int>(c.r.GetRestirGIInitialCandidates()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_initial_candidates")
SETTING(Int, "raytracing.restirGiEmitterCandidates", restirGiEmitterCandidates, .title = "GI Emitter Candidates",
        .tip = "Emissive-surface points in the same RIS stream as the cosine rays.",
        .lo = 1, .hi = 64, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIEmitterCandidates(static_cast<uint32_t>(std::clamp(s.restirGiEmitterCandidates, 1, 64))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiEmitterCandidates = static_cast<int>(c.r.GetRestirGIEmitterCandidates()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_emitter_candidates")
SETTING(Bool, "raytracing.restirGiFullRes", restirGiFullRes, .title = "GI Full Resolution",
        .tip = "Trace at render resolution instead of half. Measured about 3.5x the\n"
               "GI cost (SchoolNight, ReSTIR_Dual_Backend_Plan.md sec.9). Rebuilds\n"
               "the working set and drops the history once.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIFullRes(s.restirGiFullRes); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiFullRes = c.r.GetRestirGIFullRes(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_halfres=false|-gi_fullres=true")
SETTING(Int, "raytracing.restirGiHitExactLights", restirGiHitExactLights, .title = "GI Exact Lights At Hit",
        .tip = "Local lights summed exactly (each with a shadow ray) at a GI hit; more are resampled.",
        .lo = 1, .hi = 64, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGIHitExactLights(static_cast<uint32_t>(std::clamp(s.restirGiHitExactLights, 1, 64))); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiHitExactLights = static_cast<int>(c.r.GetRestirGIHitExactLights()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_hit_exact_lights")
// Captured as requested, not as whether this device runs it.
SETTING(Bool, "raytracing.restirGiSharc", restirGiSharc, .title = "GI Radiance Cache (SHaRC)",
        .tip = "A world-space hash grid stands for a GI hit's lights, shadow rays and\n"
               "DDGI lookup (one probe instead); its emission and sun stay exact.\n"
               "Parity mode caches today's Tier 1 shading, so only the cost changes.\n"
               "Forces GI Hit Shading to Tier 1. Needs 64-bit heap atomics, native\n"
               "16-bit ops and the unpacked SHaRC headers. engine.json restirGiSharc;\n"
               "-gi_sharc / -gi_sharc_mode are its old spellings.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGISharc(s.restirGiSharc);
                                                     c.r.SetDevPinned(Renderer::DevPin::RestirGiSharc, c.fromCommandLine); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiSharc = c.r.GetRestirGISharc(); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_sharc")
// "parity" | "multi" (the name rule in EngineDevSettings.cpp makes anything
// else parity, said once).
SETTING(String, "raytracing.restirGiSharcMode", restirGiSharcMode, .title = "GI Cache Mode",
        .tip = "Parity: the cache holds the hit's local lights + DDGI (the analytic\n"
               "shading's expectation). Multi-bounce: several bounces, no DDGI - a\n"
               "change of the picture. A new mode clears the cache.",
        .labels = "parity|multi",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetRestirGISharcMode(s.restirGiSharcMode == "multi" ? 1u : 0u); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiSharcMode = c.r.GetRestirGISharcMode() == 1u ? "multi" : "parity"; },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_sharc_mode")
// auto | relax | reblur | none: the denoiser before RR under ray reconstruction.
SETTING(String, "raytracing.restirDiDenoiser", restirDiDenoiser, .title = "DI Denoiser",
        .tip = "NRD before the composite (and before DLSS Ray Reconstruction when it is on).\n"
               "Auto: RELAX; under RR no NRD - RR denoises the full-resolution trace (sec.9.23).\n"
               "RELAX: steady hard shadows, NRD's denoiser for RTXDI signals.\n"
               "REBLUR: sizes its blur from the light distance (softer shadows).\n"
               "None: the raw signal (A/B; without RR it is noisy).\n"
               "A change rebuilds the working set and drops the history once.\n"
               "engine.json restirDiDenoiser; -di_no_nrd / -di_nrd_reblur are its old spellings.",
        .labels = "auto|relax|reblur|none",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Denoiser d;
                       c.r.SetRestirDIDenoiser(ReSTIR::ParseDenoiser(s.restirDiDenoiser, d) ? d : ReSTIR::Denoiser::Auto); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirDiDenoiser = ReSTIR::DenoiserName(c.r.GetRestirDIDenoiser()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-di_no_nrd=none|-di_nrd_reblur=reblur")
SETTING(String, "raytracing.restirGiDenoiser", restirGiDenoiser, .title = "GI Denoiser",
        .tip = "NRD before the composite (and before DLSS Ray Reconstruction when it is on).\n"
               "Auto: RELAX, kept under RR - the half-resolution upsample's correlated noise\n"
               "boils under RR otherwise (sec.9.32).\n"
               "RELAX: keeps contact shadows and corner darkening.\n"
               "REBLUR: sizes its blur from the hit distance (blurs long connections).\n"
               "None: the raw signal (A/B).\n"
               "A change rebuilds the working set and drops the history once.\n"
               "engine.json restirGiDenoiser; -gi_no_nrd / -gi_nrd_reblur are its old spellings\n"
               "(-gi_rr_raw keeps the raw signal under RR).",
        .labels = "auto|relax|reblur|none",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Denoiser d;
                       c.r.SetRestirGIDenoiser(ReSTIR::ParseDenoiser(s.restirGiDenoiser, d) ? d : ReSTIR::Denoiser::Auto); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirGiDenoiser = ReSTIR::DenoiserName(c.r.GetRestirGIDenoiser()); },
        .policy = Policy::LastWrite,
        .legacyFlag = "-gi_no_nrd=none|-gi_nrd_reblur=reblur")
// auto | nee | restir_di | unified: where the RTXDI PT core takes x1's direct light.
SETTING(String, "raytracing.ptDirectLight", ptDirectLight, .title = "PT Direct Light",
        .tip = "Where the path tracer takes the first hit's direct light (lamps and the sun):\n"
               "ReSTIR DI - the DI pass's raw, full-resolution estimate (temporal + spatial reuse\n"
               "over every lamp): the steadiest input for Ray Reconstruction in many-light scenes.\n"
               "Path NEE - the core's own sample by power each frame: cheaper (no DI pass), noisier.\n"
               "Unified reservoir - the first hit's light inside the path's reservoir (ReSTIR PT\n"
               "Enhanced sec.6.1). Changing it rebuilds the path tracer. engine.json\n"
               "raytracing.ptDirectLight; -rtxdi_pt_di / -rtxdi_pt_unified are its old spellings.",
        .labels = "auto|nee|restir_di|unified",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { PTRtxdi::DirectLight d;
                       c.r.SetPTDirectLight(PTRtxdi::ParseDirectLight(s.ptDirectLight.c_str(), d) ? d : PTRtxdi::DirectLight::Auto); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.ptDirectLight = PTRtxdi::DirectLightName(c.r.GetPTDirectLight()); },
        .policy = Policy::LastWrite,
        // The old flags, in priority order: -rtxdi_pt_unified=1, then -rtxdi_pt_di=0|1 (a bare
        // -rtxdi_pt_di is 1). Both are marked read.
        .legacyRead = [](Value& out, std::string& writer, std::string*) {
            const Sys::CommandLine& cl = Sys::CommandLine::Get();
            const bool unified = cl.Peek("-rtxdi_pt_unified"), di = cl.Peek("-rtxdi_pt_di");
            if (unified) cl.MarkRead("-rtxdi_pt_unified");
            if (di) cl.MarkRead("-rtxdi_pt_di");
            if (unified && cl.PeekInt("-rtxdi_pt_unified", 0) != 0)
            {
                out.s = PTRtxdi::DirectLightName(PTRtxdi::DirectLight::Unified);
                writer = "-rtxdi_pt_unified";
                return LegacyResult::Taken;
            }
            if (!di) return LegacyResult::Absent;
            out.s = PTRtxdi::DirectLightName(cl.PeekInt("-rtxdi_pt_di", 1) != 0 ? PTRtxdi::DirectLight::RestirDI
                                                                                : PTRtxdi::DirectLight::Nee);
            writer = "-rtxdi_pt_di";
            return LegacyResult::Taken;
        })
// The backends each stage resamples with, captured as REQUESTED: a build without
// the RTXDI core must not persist its engine fallback over the user's choice.
SETTING(String, "raytracing.restirBackendDI", restirBackendDI, .title = "DI Backend",
        .tip = "The resampling core this stage runs. The shell around it (NRD,\n"
               "the result texture, the counters) is the same for every core.",
        .labels = "engine|rtxdi|hybrid",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Backend b;
                       if (ReSTIR::ParseBackend(ReSTIR::Stage::DI, s.restirBackendDI, b)) c.r.SetRestirBackend(ReSTIR::Stage::DI, b); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirBackendDI = ReSTIR::BackendName(c.r.GetRestirBackend(ReSTIR::Stage::DI)); },
        .policy = Policy::LastWrite)
SETTING(String, "raytracing.restirBackendGI", restirBackendGI, .title = "GI Backend",
        .tip = "The resampling core this stage runs. The shell around it (NRD,\n"
               "the result texture, the counters) is the same for every core.",
        .labels = "engine|rtxdi",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Backend b;
                       if (ReSTIR::ParseBackend(ReSTIR::Stage::GI, s.restirBackendGI, b)) c.r.SetRestirBackend(ReSTIR::Stage::GI, b); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirBackendGI = ReSTIR::BackendName(c.r.GetRestirBackend(ReSTIR::Stage::GI)); },
        .policy = Policy::LastWrite)
SETTING(String, "raytracing.restirBackendPT", restirBackendPT, .title = "PT Backend",
        .tip = "The resampling core this stage runs. The shell around it (NRD,\n"
               "the result texture, the counters) is the same for every core.",
        .labels = "auto|area|rtxdi",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Backend b;
                       if (ReSTIR::ParseBackend(ReSTIR::Stage::PT, s.restirBackendPT, b)) c.r.SetRestirBackend(ReSTIR::Stage::PT, b); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirBackendPT = ReSTIR::BackendName(c.r.GetRestirBackend(ReSTIR::Stage::PT)); },
        .policy = Policy::LastWrite)
SETTING(String, "raytracing.restirCompare", restirCompare, .title = "ReSTIR Compare",
        .tip = "The other core of a stage as a shadow pass in the same frame\n"
               "(ReSTIR_Dual_Backend_Plan.md sec.3.2); it needs that stage's second core.",
        .labels = "off|di|gi",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { ReSTIR::Compare cmp;
                       if (ReSTIR::ParseCompare(s.restirCompare, cmp)) c.r.SetRestirCompare(cmp); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.restirCompare = ReSTIR::CompareName(c.r.GetRestirCompare()); },
        .policy = Policy::LastWrite)

// ---- Material surface cache ----
SETTING(Bool, "materialSurfaceCache.enabled", materialSurfaceCache, .title = "Material Surface Cache (RT proxy)",
        .tip = "OFF (default): a CUSTOM-SHADER material's ray-traced look is an\n"
               "engine GUESS - it name-matches _BaseMap / _MainTex / ... and falls\n"
               "back to the importer-seeded default white, so NPR characters\n"
               "reflect as over-bright mannequins.\n"
               "\n"
               "ON: the material's OWN GBuffer pixel shader is rasterised into its\n"
               "UV space once and stored in a 2048^2 atlas page, which the ray\n"
               "shaders then sample. No name guessing, no NPR re-implementation.\n"
               "Costs one extra mesh-section draw per baked page (2 pages/frame)\n"
               "and ~20 MB of VRAM, allocated only once a page is actually needed.\n"
               "\n"
               "Bakes the material, NOT the lighting: view-dependent terms (toon\n"
               "fresnel, matcap, rim) are evaluated head-on, which is the correct\n"
               "neutral choice for an albedo cache.",
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetMaterialSurfaceCacheEnabled(s.materialSurfaceCache); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.materialSurfaceCache = c.r.IsMaterialSurfaceCacheEnabled(); },
        .policy = Policy::LastWrite)
SETTING(Int, "materialSurfaceCache.pagesPerFrame", materialSurfaceCachePagesPerFrame, .title = "Bakes / frame",
        .lo = 1, .hi = 8, .san = San::Clamp,
        .phase = Phase::Renderer,
        .apply   = [](const DS& s, DevApplyCtx& c) { c.r.SetMaterialSurfaceCachePagesPerFrame(static_cast<uint32_t>(s.materialSurfaceCachePagesPerFrame)); },
        .capture = [](DS& s, DevCaptureCtx& c) { s.materialSurfaceCachePagesPerFrame = static_cast<int>(c.r.GetMaterialSurfaceCachePagesPerFrame()); },
        .policy = Policy::LastWrite)

// ---- Read from the file by their owners (App) ----
// The GameFlow state the title-bar X / Alt+F4 is handed to (empty = close at once);
// App::InstallWindowHooks reads it once, at boot, for its first close handler
// (Window::AddCloseHandler).
SETTING(String, "gameFlow.quitConfirmState", gameFlowQuitConfirmState, .policy = Policy::LastWrite)
// The stat-unit HUD's boot level, 0 Off / 1 Fps / 2 Unit / 3 Full (F9 cycles it,
// transiently).
SETTING(Int, "debug.hudLevel", debugHudLevel, .lo = 0, .hi = 3, .san = San::Clamp, .policy = Policy::LastWrite)

// ---- Read by its owner at first use (the device) ----
// The streaming texture upload's ring per upload slot, in MiB (three slots). The device
// creates the rings at the first QueueTextureUpload and reads this then, through
// PeekEngineDevStreamRingMiB: like rayReconstructionLoad, ApplyTo / CaptureFrom never
// touch it. -streamupload_ring_mib=N wins.
SETTING(Int, "upload.streamRingMiB", streamRingMiB, .lo = 1, .hi = 512, .san = San::Clamp,
        .policy = Policy::LastWrite)

// ---- Read by its owner at boot (the skinned vertex ring) ----
// The skinned vertex ring's budget, in vertices per frame. SkinnedVertexRing::Init reads it
// once, through PeekEngineDevSkinVertexBudget, before it allocates: three frame slots of 40 B
// a vertex stay resident on every scene (960 MiB at 8388608). 0 sizes it by the adapter:
// 1048576 (120 MiB) under 4 GB of video memory, the first boot's Low tier, else 8388608.
// The ring takes 1048576 .. 33554432. -skinvertexbudget=N wins.
SETTING(Int, "skinning.vertexBudget", skinVertexBudget, .lo = 0, .hi = 33554432, .san = San::Clamp,
        .policy = Policy::LastWrite)

// ---- Read by its owner at boot (the physics world) ----
// PhysicsSystem::Init reads both once, through PeekEngineDevPhysics, before the first step: Jolt's
// world gravity along Y (negative is down) and the seconds a body must rest before it sleeps. The
// character controller and the chain solver have their own gravity (CharacterControllerComponent::gravity,
// ChainPhysicsSystem's), which these do not move.
SETTING(Float, "physics.gravity", physicsGravity, .title = "Gravity (m/s^2)", .lo = -200.f, .hi = 200.f,
        .san = San::Clamp, .fallback = -9.81, .policy = Policy::LastWrite)
SETTING(Float, "physics.timeBeforeSleep", physicsTimeBeforeSleep, .title = "Time before sleep (s)",
        .lo = 0.f, .hi = 60.f, .san = San::Clamp, .fallback = 0.5, .policy = Policy::LastWrite)
