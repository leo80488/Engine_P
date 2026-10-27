// Settings/Rows/Passes.inl - the pass settings rows: the render passes' tuning
// parameters, once.
//
// One row per member of Resource::PassSettings (Settings/PassSettings.h keeps each
// field's rationale). Path "pass.<pass>.<field>"; the document is not saved anywhere.
//
// Macro contract, as in Developer.inl: SETTING(Kind, "path", member, .field = value,
// ...), the tail a designated initializer of Settings::RowSpecT<PassSettings> in its
// declaration order. No row has an apply or capture hook: the passes read their values
// through Settings::Pass::<member>() (Phase::None).
//
// .legacyFlag names the pass's own command-line flag. Given at boot, it is read exactly
// as the pass read it (a Bool by presence, an Int / Float by GetInt / GetFloat - a
// malformed value is the default, no clamp - a String by Get) and recorded in the
// CommandLine layer under the flag's name, so every pack and script that passes the flag
// keeps working. -set:pass.<...>=<v> goes through the row's rules (Coerce: range, legal
// names).

#ifndef SETTING
#error "Passes.inl needs SETTING(Kind, path, member, ...) defined by the includer"
#endif

// ---- SpotShadowPass ----
SETTING(Bool, "pass.spotShadow.noMixedCache", spotShadowNoMixedCache,
        .title = "No Mixed Static Cache",
        .tip = "Skips the optional static backing depth of the mixed spot-shadow\n"
               "cache (up to 384 MiB): every spot shadow renders in full. Read when\n"
               "the atlas is allocated: a change lands with the next world.",
        .legacyFlag = "-spot_shadow_no_mixed_cache")

// ---- FSR2Pass, DLSSPass (the upscalers) ----
SETTING(Bool, "pass.upscaler.engineExposure", upscalerEngineExposure,
        .title = "Upscalers Use the Engine's Exposure",
        .tip = "FSR 2 and DLSS are handed the exposure the tone map applies (a 1x1\n"
               "texture) instead of metering their own. Off: the SDK's auto exposure.\n"
               "Read when the upscaler's context is made (a resize, a mode change) and\n"
               "re-made when it changes.")

// ---- DLSSPass (ray reconstruction) ----
SETTING(Float, "pass.dlss.rrExposureScale", dlssRrExposureScale,
        .title = "RR Exposure Scale",
        .tip = "Multiplies the exposure DLSS ray reconstruction is told.",
        .lo = 0.f, .hi = 16.f,
        .legacyFlag = "-dlss_rr_exposure_scale")
SETTING(Float, "pass.dlss.rrPreExposure", dlssRrPreExposure,
        .title = "RR Pre-Exposure",
        .tip = "Multiplies the pre-exposure DLSS ray reconstruction is told, on\n"
               "top of the radiance encoding's scale.",
        .lo = 0.f, .hi = 16.f,
        .legacyFlag = "-dlss_rr_pre_exposure")
SETTING(String, "pass.dlss.rrNormalTag", dlssRrNormalTag,
        .title = "RR Normal Guide Tag",
        .tip = "How the packed normal + roughness guide is tagged for ray\n"
               "reconstruction: normalroughness (the default) or normals.",
        .labels = "normalroughness|normals",
        .legacyFlag = "-dlss_rr_normal_tag")
SETTING(Float, "pass.dlss.rrExposure", dlssRrExposure,
        .title = "RR Explicit Exposure",
        .tip = "An explicit exposure handed to ray reconstruction, only when set\n"
               "(otherwise its auto-exposure). Applies when RR's resources are\n"
               "next made (a resize, RR coming on).",
        .lo = 1e-6f, .hi = 1e6f,
        .legacyFlag = "-dlss_rr_exposure")
SETTING(Float, "pass.dlss.rrRadianceScale", dlssRrRadianceScale,
        .title = "RR Radiance Encoding Scale",
        .tip = "Scales the radiance ray reconstruction is given (and unscales its\n"
               "output); 1 = none. Applies when RR's resources are next made.",
        .lo = 1e-6f, .hi = 1e6f,
        .legacyFlag = "-dlss_rr_radiance_scale")

// ---- PathTracePass ----
SETTING(Int, "pass.pathTrace.frontSpp", ptFrontSpp,
        .title = "Front Layer Mirror Samples",
        .tip = "Mirror samples per pixel of the glass layer's traced reflection.\n"
               "Applies when the front layer is next allocated.",
        .lo = 1, .hi = 8,
        .legacyFlag = "-pt_front_spp")
SETTING(Int, "pass.pathTrace.frontNee", ptFrontNee,
        .title = "Front Layer Light Candidates",
        .tip = "Local-light candidates at the glass reflection's vertices (1 = one\n"
               "light by power). Applies when the front layer is next allocated.",
        .lo = 1, .hi = 32,
        .legacyFlag = "-pt_front_nee")
SETTING(Float, "pass.pathTrace.sunRadius", ptSunRadius,
        .title = "Sun Angular Radius",
        .tip = "The sun's angular radius (radians) for the path tracer's sun\n"
               "sampling. Applies at the next shader reload of the pass.",
        .lo = 0.001f, .hi = 1.f,
        .legacyFlag = "-pt_sun_radius")
SETTING(Float, "pass.pathTrace.areaReconnectWeight", ptAreaReconnectWeight,
        .title = "Area Reconnection Weight",
        .tip = "The legacy Area reconnection experiment's MIS weight\n"
               "(-pt_area_reconnect). Applies at the next shader reload of the pass.",
        .lo = 0.f, .hi = 1.f,
        .legacyFlag = "-pt_area_reconnect_weight")
SETTING(Float, "pass.pathTrace.areaSpatialConfidence", ptAreaSpatialConfidence,
        .title = "Area Spatial Confidence",
        .tip = "Area ReSTIR's spatial pass confidence.",
        .lo = 0.f, .hi = 4.f)
SETTING(Bool, "pass.pathTrace.media", ptMedia,
        .title = "Participating Media",
        .tip = "The reference path tracer integrates the scene's fog (Volumetric Fog,\n"
               "Height Fog, Local Fog Volumes) as a participating medium and draws\n"
               "the camera's fog itself. Off: the raster fog passes draw it.")
SETTING(Bool, "pass.pathTrace.mediaSingleScatter", ptMediaSingleScatter,
        .title = "Media: Single Scattering",
        .tip = "A path ends at its first scattering in the fog, after that vertex's light\n"
               "samples: the fog's single scattering alone (the analytic tests).")
SETTING(Bool, "pass.pathTrace.mediaEquiAngular", ptMediaEquiAngular,
        .title = "Media: Equi-Angular Sampling",
        .tip = "The camera segment's scattering distance is also drawn toward a light\n"
               "(Kulla and Fajardo 2012). Off: free flight and the local volumes only.")
SETTING(Float, "pass.pathTrace.mediaMaxDistance", ptMediaMaxDistance,
        .title = "Media: Fog Reach",
        .tip = "How far from the camera the volumetric fog's global medium reaches in the\n"
               "path tracers and the ray-traced froxels, in metres. 0: it has no end (the\n"
               "default); past the reach every ray leaves the fog.",
        .lo = 0.0f, .hi = 100000.0f)

// ---- PTRtxdiCore (RTXDI's ReSTIR PT) ----
SETTING(String, "pass.ptRtxdi.reuse", ptRtxdiReuse,
        .title = "Reuse",
        .tip = "Which reuse the core runs: none, temporal, spatial or temporal_spatial.",
        .labels = "none|temporal|spatial|temporal_spatial",
        .legacyFlag = "-rtxdi_pt_reuse")
SETTING(Int, "pass.ptRtxdi.bounces", ptRtxdiBounces,
        .title = "Bounces",
        .tip = "Path length (the reservoir stores 8-bit lengths). Unset: 5 on the real-time path, 3 on\nthe reference.",
        .lo = 2.0f, .hi = 64.0f,
        .flags = kSettingLegacyValueOnly,
        .legacyFlag = "-rtxdi_pt_bounces")
SETTING(Int, "pass.ptRtxdi.rcLength", ptRtxdiRcLength,
        .title = "Reconnection Length",
        .lo = 2.0f, .hi = 32.0f)
SETTING(Int, "pass.ptRtxdi.initialSamples", ptRtxdiInitialSamples,
        .title = "Initial Samples",
        .lo = 1.0f, .hi = 8.0f)
SETTING(Int, "pass.ptRtxdi.spatialSamples", ptRtxdiSpatialSamples,
        .title = "Spatial Samples",
        .tip = "Spatial neighbours (the pairwise pass takes up to 4).",
        .lo = 1.0f, .hi = 8.0f)
SETTING(Float, "pass.ptRtxdi.radius", ptRtxdiRadius,
        .title = "Spatial Radius (px)",
        .lo = 1.0f, .hi = 128.0f)
SETTING(Int, "pass.ptRtxdi.history", ptRtxdiHistory,
        .title = "History",
        .tip = "Temporal history length. Set, it holds under the DLSS-RR recipe (which otherwise takes 20).",
        .lo = 1.0f, .hi = 64.0f)
SETTING(Int, "pass.ptRtxdi.boiling", ptRtxdiBoiling,
        .title = "Boiling Filter",
        .tip = "RTXDI's boiling filter: 0 off (the energy contract), 1 always, -1 while DLSS-RR takes the guides.",
        .lo = -1.0f, .hi = 1.0f)
SETTING(Float, "pass.ptRtxdi.boilingStrength", ptRtxdiBoilingStrength,
        .title = "Boiling Strength",
        .lo = 0.001f, .hi = 1.0f)
SETTING(String, "pass.ptRtxdi.reconnection", ptRtxdiReconnection,
        .title = "Reconnection Rule",
        .labels = "footprint|fixed")
SETTING(Int, "pass.ptRtxdi.temporalVisibility", ptRtxdiTemporalVisibility,
        .title = "Temporal Visibility",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Int, "pass.ptRtxdi.permutation", ptRtxdiPermutation,
        .title = "Permutation Sampling",
        .tip = "0 off, 1 always, -1 while DLSS-RR takes the guides.",
        .lo = -1.0f, .hi = 1.0f)
SETTING(Int, "pass.ptRtxdi.rrRecipe", ptRtxdiRrRecipe,
        .title = "DLSS-RR Recipe",
        .tip = "RTXDI's DLSS-RR recipe: -1 while DLSS-RR takes the guides, 0 / 1 pinned.",
        .lo = -1.0f, .hi = 1.0f,
        .legacyFlag = "-rtxdi_pt_rr_recipe")
SETTING(String, "pass.ptRtxdi.decorrelation", ptRtxdiDecorrelation,
        .title = "Decorrelation",
        .labels = "uniform|stagnancy")
SETTING(Float, "pass.ptRtxdi.decorrelationFactor", ptRtxdiDecorrelationFactor,
        .title = "Decorrelation Factor",
        .tip = "Unset: 0.85 (uniform), 0.4 (stagnancy).",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Int, "pass.ptRtxdi.firefly", ptRtxdiFirefly,
        .title = "Firefly Replacement",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Float, "pass.ptRtxdi.fireflyStrength", ptRtxdiFireflyStrength,
        .title = "Firefly Strength",
        .lo = 0.001f, .hi = 1.0f)
SETTING(String, "pass.ptRtxdi.fresh", ptRtxdiFresh,
        .title = "Fresh Reservoir Rule",
        .labels = "replace|clamp")
SETTING(Int, "pass.ptRtxdi.duplicationMap", ptRtxdiDuplicationMap,
        .title = "Duplication Map",
        .tip = "Set, it holds under the DLSS-RR recipe.",
        .lo = 0.0f, .hi = 1.0f,
        .legacyFlag = "-rtxdi_pt_duplication_map")
SETTING(Int, "pass.ptRtxdi.boost", ptRtxdiBoost,
        .title = "Disocclusion Boost",
        .tip = "Spatial neighbours for a short history. Unset: the spatial sample count.",
        .lo = 1.0f, .hi = 32.0f,
        .flags = kSettingLegacyValueOnly,
        .legacyFlag = "-rtxdi_pt_boost")
SETTING(Int, "pass.ptRtxdi.dualMv", ptRtxdiDualMv,
        .title = "Dual Motion Vectors",
        .lo = 0.0f, .hi = 1.0f,
        .legacyFlag = "-rtxdi_pt_dual_mv")
SETTING(String, "pass.ptRtxdi.spatial", ptRtxdiSpatial,
        .title = "Spatial Pass",
        .tip = "The engine's pairwise pass or RTXDI's (rtxdi).",
        .labels = "pairwise|rtxdi",
        .legacyFlag = "-rtxdi_pt_spatial")
SETTING(Int, "pass.ptRtxdi.compaction", ptRtxdiCompaction,
        .title = "Pairwise Compaction",
        .tip = "Read with the pairwise spatial pass only.",
        .lo = 0.0f, .hi = 1.0f,
        .legacyFlag = "-rtxdi_pt_compaction")
SETTING(Int, "pass.ptRtxdi.rgb", ptRtxdiRgb,
        .title = "Pairwise RGB Estimate",
        .tip = "Read with the pairwise spatial pass only.",
        .lo = 0.0f, .hi = 1.0f,
        .legacyFlag = "-rtxdi_pt_rgb")
SETTING(Int, "pass.ptRtxdi.neeCandidates", ptRtxdiNeeCandidates,
        .title = "NEE Candidates",
        .tip = "Local-light candidates at the path's vertices.",
        .lo = 1.0f, .hi = 32.0f,
        .legacyFlag = "-rtxdi_pt_nee_candidates")
SETTING(Int, "pass.ptRtxdi.neeCandidatesX1", ptRtxdiNeeCandidatesX1,
        .title = "NEE Candidates at x1",
        .lo = 1.0f, .hi = 32.0f,
        .legacyFlag = "-rtxdi_pt_nee_candidates_x1")
SETTING(Int, "pass.ptRtxdi.sharc", ptRtxdiSharc,
        .title = "SHaRC Cache",
        .tip = "The radiance cache the paths end in. Unset: on for the real-time path, off for the\nreference.",
        .lo = 0.0f, .hi = 1.0f,
        .flags = kSettingLegacyValueOnly,
        .legacyFlag = "-rtxdi_pt_sharc")
SETTING(Float, "pass.ptRtxdi.sharcMinRoughness", ptRtxdiSharcMinRoughness,
        .title = "SHaRC Min Roughness",
        .tip = "A vertex ends its path in the cache only this rough (and not metal).",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Int, "pass.ptRtxdi.sharcGateLast", ptRtxdiSharcGateLast,
        .title = "SHaRC Gate Last",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Bool, "pass.ptRtxdi.media", ptRtxdiMedia,
        .title = "Participating Media",
        .tip = "Every path segment crosses the scene's fog (Volumetric Fog, Height Fog,\n"
               "Local Fog Volumes): a light's by the whole transmittance, a continuation's\n"
               "by Media Continuation. The camera's fog stays the raster fog's.")
SETTING(String, "pass.ptRtxdi.mediaContinuation", ptRtxdiMediaContinuation,
        .title = "Media Continuation",
        .tip = "What a continuation segment keeps through the fog, which has no medium vertex\n"
               "to return the light scattered into it: absorption (only what absorption\n"
               "removes is lost), none (the fog takes nothing), full (the whole transmittance),\n"
               "froxel (inside the froxel fog's last lit volume the whole transmittance and the\n"
               "light the volume scattered into the segment; absorption outside it).",
        .labels = "absorption|none|full|froxel")

// ---- RTDIPass (ReSTIR DI) ----
SETTING(Int, "pass.rtdi.spatialRadius2", rtdiSpatialRadius2,
        .title = "Second Spatial Radius (px)",
        .tip = "The engine core's second spatial pass. Unset: the first pass's radius.",
        .lo = 0.0f, .hi = 64.0f,
        .flags = kSettingLegacyValueOnly,
        .legacyFlag = "-di_spatial_radius2")
SETTING(Float, "pass.rtdi.nrdHitDistScale", rtdiNrdHitDistScale,
        .title = "NRD Hit-Distance Scale",
        .tip = "A diagnostic: scales the hit distance NRD is told (against the trace's).\n"
               "Applies at the next shader reload of the pass.",
        .lo = 0.25f, .hi = 4.0f,
        .legacyFlag = "-di_nrd_hitdist_scale")
SETTING(Float, "pass.rtdi.rawMirrorRoughness", rtdiRawMirrorRoughness,
        .title = "Raw Mirror Roughness",
        .tip = "Applies at the next shader reload of the pass.",
        .lo = 0.0f, .hi = 1.0f,
        .legacyFlag = "-di_raw_mirror_roughness")
SETTING(Float, "pass.rtdi.specAA", rtdiSpecAA,
        .title = "Specular AA Scale",
        .tip = "Widens the receiver's GGX lobe by the normal variance across its 2x2\n"
               "block (0 = off). Applies at the next shader reload of the pass.",
        .lo = 0.0f, .hi = 63.75f)
SETTING(String, "pass.rtdi.rtxdiBias", rtdiRtxdiBias,
        .title = "RTXDI Bias Correction",
        .tip = "The RTXDI core's temporal and spatial bias correction.",
        .labels = "off|basic|pairwise|raytraced",
        .legacyFlag = "-rtxdi_di_bias")
SETTING(Int, "pass.rtdi.rtxdiBoiling", rtdiRtxdiBoiling,
        .title = "RTXDI Boiling Filter",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Int, "pass.rtdi.rtxdiInitialVisibility", rtdiRtxdiInitialVisibility,
        .title = "RTXDI Initial Visibility",
        .lo = 0.0f, .hi = 1.0f)
SETTING(String, "pass.rtdi.rtxdiLocal", rtdiRtxdiLocal,
        .title = "RTXDI Local Light Sampling",
        .labels = "regir|power|uniform")
SETTING(Int, "pass.rtdi.rtxdiPermutation", rtdiRtxdiPermutation,
        .title = "RTXDI Permutation Sampling",
        .lo = 0.0f, .hi = 1.0f)

// ---- RTGIPass (ReSTIR GI) ----
SETTING(Int, "pass.rtgi.mCap", rtgiMCap,
        .title = "M Cap",
        .tip = "The reservoirs' history cap. Applies when the reuse core is next made.",
        .lo = 1.0f, .hi = 63.0f)
SETTING(Int, "pass.rtgi.spatialSamples", rtgiSpatialSamples,
        .title = "Spatial Neighbours",
        .tip = "The engine core takes up to 16, the RTXDI core up to 32.",
        .lo = 1.0f, .hi = 32.0f,
        .legacyFlag = "-gi_spatial_samples")
SETTING(Int, "pass.rtgi.boostSamples", rtgiBoostSamples,
        .title = "Disocclusion Boost",
        .tip = "The engine core's spatial neighbours for a young reservoir.",
        .lo = 1.0f, .hi = 32.0f,
        .legacyFlag = "-gi_boost_samples")
SETTING(Float, "pass.rtgi.freshBlend", rtgiFreshBlend,
        .title = "Fresh Sample Blend",
        .lo = 0.0f, .hi = 1.0f)
SETTING(String, "pass.rtgi.rtxdiTarget", rtgiRtxdiTarget,
        .title = "RTXDI Target",
        .tip = "visible: the engine core's target (visibility from the receiver).\n"
               "unshadowed: RTXDI's own setup.",
        .labels = "visible|unshadowed",
        .legacyFlag = "-rtxdi_gi_target")
SETTING(String, "pass.rtgi.rtxdiBias", rtgiRtxdiBias,
        .title = "RTXDI Bias Correction",
        .tip = "Empty: basic with the visible target, raytraced with the unshadowed one.",
        .labels = "off|basic|raytraced",
        .legacyFlag = "-rtxdi_gi_bias")
SETTING(Int, "pass.rtgi.rtxdiFallback", rtgiRtxdiFallback,
        .title = "RTXDI Fallback Tap",
        .tip = "RTXDI's fallback spatial tap (locally biased, usually darker).",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Int, "pass.rtgi.rtxdiPermutation", rtgiRtxdiPermutation,
        .title = "RTXDI Permutation Sampling",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Bool, "pass.rtgi.boilingFilter", rtgiBoilingFilter,
        .title = "Boiling Filter",
        .tip = "RTXDI's GI boiling filter (opt-in). Applies at the next shader reload of the pass.",
        .legacyFlag = "-gi_boiling_filter")
SETTING(Bool, "pass.rtgi.jacobianClamp", rtgiJacobianClamp,
        .title = "Jacobian Clamp",
        .tip = "RTXDI's [1/3, 3] reconnection Jacobian clamp (opt-in). Applies at the next\n"
               "shader reload of the pass.")
SETTING(Int, "pass.rtgi.hitLightSamples", rtgiHitLightSamples,
        .title = "Hit Light Samples",
        .tip = "Resampled local lights at a GI hit beyond the exact ones, each with its shadow ray.",
        .lo = 1.0f, .hi = 16.0f)
SETTING(Int, "pass.rtgi.localEmitters", rtgiLocalEmitters,
        .title = "Per-Receiver Emitter Limit",
        .tip = "A per-receiver emitter pmf up to this many emitters (the global power CDF above).",
        .lo = 0.0f, .hi = 4096.0f)
SETTING(Float, "pass.rtgi.sharcMaxVoxel", rtgiSharcMaxVoxel,
        .title = "SHaRC Max Voxel (m)",
        .lo = 0.01f, .hi = 1000.0f)
SETTING(Int, "pass.rtgi.sharcMinSamples", rtgiSharcMinSamples,
        .title = "SHaRC Min Samples",
        .lo = 0.0f, .hi = 64.0f)
SETTING(Bool, "pass.rtgi.sharcResampling", rtgiSharcResampling,
        .title = "SHaRC Resampling",
        .tip = "Multi-bounce: SHaRC's cache resampling in the update.",
        .legacyFlag = "-gi_sharc_resampling")
SETTING(Float, "pass.rtgi.sharcCachedReset", rtgiSharcCachedReset,
        .title = "SHaRC Cached Reset",
        .tip = "A cached sample's relative luminance change above this resets its history.",
        .lo = 0.01f, .hi = 100.0f)
SETTING(Float, "pass.rtgi.nrdPrepass", rtgiNrdPrepass,
        .title = "RELAX Pre-Pass Blur Radius",
        .lo = 0.0f, .hi = 60.0f)
SETTING(Bool, "pass.rtgi.nrdAntiFirefly", rtgiNrdAntiFirefly,
        .title = "RELAX Anti-Firefly")
SETTING(Int, "pass.rtgi.nrdVarianceHistory", rtgiNrdVarianceHistory,
        .title = "RELAX Variance History",
        .lo = 0.0f, .hi = 16.0f)
SETTING(Float, "pass.rtgi.nrdAntilagReset", rtgiNrdAntilagReset,
        .title = "RELAX Anti-Lag Reset",
        .lo = 0.0f, .hi = 1.0f)
SETTING(Float, "pass.rtgi.nrdAntilagSigma", rtgiNrdAntilagSigma,
        .title = "RELAX Anti-Lag Spatial Sigma",
        .lo = 0.5f, .hi = 20.0f)
SETTING(Int, "pass.rtgi.nrdAtrous", rtgiNrdAtrous,
        .title = "RELAX A-Trous Iterations",
        .lo = 2.0f, .hi = 8.0f)
SETTING(Float, "pass.rtgi.nrdHistorySeconds", rtgiNrdHistorySeconds,
        .title = "RELAX History (s)",
        .lo = 0.05f, .hi = 2.0f,
        .legacyFlag = "-gi_nrd_history_seconds")
SETTING(Int, "pass.rtgi.nrdMinHistory", rtgiNrdMinHistory,
        .title = "RELAX Min History Frames",
        .lo = 1.0f, .hi = 60.0f)

// ---- TransmissionPrepass / TransmissionApplyPass ----
SETTING(Bool, "pass.transmission.enabled", transmissionEnabled,
        .title = "Transmission",
        .tip = "Refracts and tints the background behind Transmissive materials.\n"
               "Off: they blend over it by coverage, as in a preview.")
SETTING(Float, "pass.transmission.blurScale", transmissionBlurScale,
        .title = "Rough Transmission Blur",
        .tip = "Scales how much a rough Transmissive surface blurs what is behind it.",
        .lo = 0.0f, .hi = 4.0f)
SETTING(Bool, "pass.transmission.motionVectors", transmissionMotionVectors,
        .title = "Refraction Motion Vectors",
        .tip = "Gives the pixels behind a refracting surface the motion of what they show,\n"
               "so TAA and the upscalers do not smear it while it moves.")

// ---- VolumetricFogPass (the ray-traced froxel lighting) ----
SETTING(String, "pass.volumetricFog.rtLighting", volumetricFogRtLighting,
        .title = "Ray-Traced Fog Lighting",
        .tip = "Lights the froxel fog with ray-traced visibility, every light of its cluster\n"
               "(resampled) and the sun's transmittance through the medium. auto: on the\n"
               "hybrid and path-traced paths, where the frame has an acceleration structure;\n"
               "on: wherever the device traces rays (it builds one); off: the raster lighting.",
        .labels = "off|auto|on")
SETTING(Int, "pass.volumetricFog.rtCandidates", volumetricFogRtCandidates,
        .title = "Fog Light Candidates",
        .tip = "How many of a froxel's cluster lights its light selection weighs each frame;\n"
               "a cluster with no more takes every one.",
        .lo = 1.0f, .hi = 32.0f)
SETTING(Float, "pass.volumetricFog.rtTemporalAlpha", volumetricFogRtTemporalAlpha,
        .title = "Fog Temporal Weight",
        .tip = "The weight of each new frame in the ray-traced fog's history (an exponential\n"
               "average, clamped to the froxel's neighbourhood).",
        .lo = 0.01f, .hi = 1.0f)
SETTING(String, "pass.volumetricFog.rtIndirect", volumetricFogRtIndirect,
        .title = "Ray-Traced Fog Indirect Light",
        .tip = "Where the ray-traced fog takes the light that does not come straight from a light.\n"
               "auto: DDGI's probes where DDGI runs, else four rays per fog cell a frame into the path\n"
               "tracer's radiance cache (SHaRC), else the fog's constant ambient. ddgi / rays: that\n"
               "source alone (the rays without the cache see the sky and emissive surfaces only).\n"
               "off: the constant ambient.",
        .labels = "off|auto|ddgi|rays")
SETTING(Bool, "pass.volumetricFog.rtShafts", volumetricFogRtShafts,
        .title = "Ray-Traced Fog Shafts",
        .tip = "Where the fog is ray-traced, scatters the sun along each view ray per pixel (half\n"
               "resolution, two distances a frame with a visibility ray each, accumulated over frames)\n"
               "instead of in the froxels: shafts as sharp as their shadows, for more GPU time.")

// ---- VolumetricFogPass (the raymarched shafts) ----
SETTING(Bool, "pass.volumetricFog.raymarchSunTransmittance", volumetricFogRaymarchSunTransmittance,
        .title = "God Rays Through the Fog",
        .tip = "Dims the raymarched god rays' sun by the fog between each point and the sun (the global\n"
               "and height fog in closed form, the nearest local volumes marched), as the ray-traced fog\n"
               "and the path tracer do. Off: the sun reaches every point of the fog undimmed, brighter\n"
               "than the medium lets it be, most under a low sun.")
