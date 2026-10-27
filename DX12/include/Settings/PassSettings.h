#pragma once

// Settings/PassSettings.h - the pass settings document: the render passes' tuning
// parameters as registry rows, the third document beside settings.json's player rows
// and engine.json's developer rows.
//
// One member per row of include/Settings/Rows/Passes.inl, path "pass.<pass>.<field>".
// Not saved: no file holds these. Their layers are Default (the member's initializer),
// CommandLine (-set:pass.<...>=<v>, and the row's old flag - .legacyFlag - read at boot
// exactly as the pass read it) and Runtime (Lua Settings.setRuntime, the pipe's
// `setting set`).
//
// Reading: a pass asks Settings::Pass::<member>() (one getter per row, made from
// Passes.inl), never the struct. The getter is safe on any thread (the effective values
// live in atomics the main thread publishes after a write), and its first call marks
// the row's old flag read, so run.json's "unreadFlags" still means nothing in this run
// asked for it. <member>Given(): a layer above Default holds the value - for a
// "Has(x) ? GetInt(x, n) : other" or a computed fallback.

#include <string>

namespace Resource
{
    struct PassSettings
    {
        // ---- pass.spotShadow (SpotShadowPass) ----
        // Skips the optional static backing depth of the mixed spot-shadow cache (up to
        // 384 MiB): every spot shadow renders in full.
        bool        spotShadowNoMixedCache = false;

        // ---- pass.upscaler (FSR2Pass, DLSSPass) ----
        // The upscalers take the exposure the tone map applies (AutoExposurePass's 1x1 texture) instead of
        // the SDK's own metering. The upscaler's context is made with it and re-made when it changes.
        bool        upscalerEngineExposure = true;

        // ---- pass.dlss (DLSSPass, ray reconstruction) ----
        // Multiplies the exposure DLSS-RR is told (SetDLSSDOptions).
        float       dlssRrExposureScale    = 1.f;
        // Multiplies the pre-exposure DLSS-RR is told, on top of the radiance
        // encoding's scale.
        float       dlssRrPreExposure      = 1.f;
        // How the packed normal + roughness guide is tagged: "normalroughness"
        // (sl NormalRoughness, the default) or "normals" (sl Normals) -
        // tools/verify_rr_truth.py asks the model which of the two it reads.
        std::string dlssRrNormalTag        = "normalroughness";
        // An explicit exposure for RR, a tagged 1x1 texture - only when given
        // (Given(); the SDK's auto-exposure otherwise): exposureScale alone is
        // ignored by the current RR auto-exposure path. Read when RR's
        // resources are made (a resize or RR coming on).
        float       dlssRrExposure         = 1.f;
        // The reversible HDR encoding's scale for RR's input (and 1/scale on
        // its output); 1 = none. Read when RR's resources are made.
        float       dlssRrRadianceScale    = 1.f;

        // ---- pass.pathTrace (PathTracePass) ----
        // The front layer's (glass) traced reflection: mirror samples per pixel and local-light
        // candidates at its vertices, resampled by their unshadowed contribution with one
        // shadow ray (1 = one light by power). Read when the front layer is allocated.
        int         ptFrontSpp             = 2;
        int         ptFrontNee             = 8;
        // The sun's angular radius for the path tracer's sun sampling. Read
        // when the pass is initialised (a shader reload).
        float       ptSunRadius            = 0.0047f;
        // Area ReSTIR: the experimental reconnection shift's MIS weight (-pt_area_reconnect),
        // read when the pass is initialised; the spatial pass's confidence, read every frame.
        float       ptAreaReconnectWeight  = 1.f;
        float       ptAreaSpatialConfidence = 1.f;
        // The reference path tracer integrates the scene's fog as a participating medium
        // (Graphics/MediumSet.h) and owns the camera rays' fog (FramePlan::FogOwner).
        bool        ptMedia                = true;
        // The medium's A/B arms: single scattering (a medium vertex ends its path after its
        // next-event estimates), and equi-angular sampling toward a light on the camera segment.
        bool        ptMediaSingleScatter   = false;
        bool        ptMediaEquiAngular     = true;
        // How far from the camera the global medium reaches in every ray-traced path (m; 0: it
        // has no end, the default): Graphics/MediumSet.h's reach.
        float       ptMediaMaxDistance     = 0.f;

        // ---- pass.ptRtxdi (PTRtxdiCore: RTXDI's ReSTIR PT) ----
        // Read when the core is made (a backend or preset change remakes it); the rows' tips
        // name the rules. The ValueOnly rows' defaults are computed when unset (Given()).

        std::string ptRtxdiReuse                = "temporal_spatial";
        int         ptRtxdiBounces              = 5;
        int         ptRtxdiRcLength             = 5;
        int         ptRtxdiInitialSamples       = 1;
        int         ptRtxdiSpatialSamples       = 1;
        float       ptRtxdiRadius               = 32.f;
        int         ptRtxdiHistory              = 8;
        int         ptRtxdiBoiling              = 0;
        float       ptRtxdiBoilingStrength      = 0.2f;
        std::string ptRtxdiReconnection         = "footprint";
        int         ptRtxdiTemporalVisibility   = 1;
        int         ptRtxdiPermutation          = 0;
        int         ptRtxdiRrRecipe             = -1;
        std::string ptRtxdiDecorrelation        = "uniform";
        float       ptRtxdiDecorrelationFactor  = 0.85f;
        int         ptRtxdiFirefly              = 0;
        float       ptRtxdiFireflyStrength      = 0.7f;
        std::string ptRtxdiFresh                = "replace";
        int         ptRtxdiDuplicationMap       = 0;
        int         ptRtxdiBoost                = 1;
        int         ptRtxdiDualMv               = 1;
        std::string ptRtxdiSpatial              = "pairwise";
        int         ptRtxdiCompaction           = 1;
        int         ptRtxdiRgb                  = 1;
        int         ptRtxdiNeeCandidates        = 4;
        int         ptRtxdiNeeCandidatesX1      = 8;
        int         ptRtxdiSharc                = 1;
        float       ptRtxdiSharcMinRoughness    = 0.5f;
        int         ptRtxdiSharcGateLast        = 0;
        // The segments through the fog, read every frame: whether they cross it, and what a
        // continuation keeps (absorption, none or full; a light segment keeps the transmittance).
        bool        ptRtxdiMedia                = true;
        std::string ptRtxdiMediaContinuation    = "absorption";

        // ---- pass.rtdi (RTDIPass and its cores: ReSTIR DI) ----
        // The second spatial pass's radius: the first one's when unset (Given()). NRD's hit-distance
        // scale (logged when set), the raw path's mirror roughness and the specular AA scale: read when the pass is initialised. The RTXDI
        // core's knobs: read when it is made.
        int         rtdiSpatialRadius2          = 0;
        float       rtdiNrdHitDistScale         = 1.f;
        float       rtdiRawMirrorRoughness      = 0.f;
        float       rtdiSpecAA                  = 3.f;   // RTDIPass::kDefaultSpecularAA
        std::string rtdiRtxdiBias               = "raytraced";
        int         rtdiRtxdiBoiling            = 1;
        int         rtdiRtxdiInitialVisibility  = 1;
        std::string rtdiRtxdiLocal              = "regir";
        int         rtdiRtxdiPermutation        = 1;

        // ---- pass.rtgi (RTGIPass and its cores: ReSTIR GI) ----
        // The cores' reuse knobs, read when a core is made: the M cap, the spatial neighbours
        // (the engine core clamps them to 16, the RTXDI core to 32), the disocclusion boost
        // (the engine core's), the fresh sample's blend; the RTXDI core's target, bias (empty =
        // the target's: basic with the visible target, raytraced with the unshadowed one;
        // Given()), fallback tap and permutation. The shell's, read when the pass is
        // initialised: the opt-in boiling filter and Jacobian clamp, the hit-light samples, the
        // per-receiver emitter limit, SHaRC's gate, resampling and cached reset. NRD's pool
        // precision (read at the working set's allocation) and RELAX's settings (every frame).
        int         rtgiMCap                    = 8;
        int         rtgiSpatialSamples          = 3;
        int         rtgiBoostSamples            = 8;
        float       rtgiFreshBlend              = 0.f;
        std::string rtgiRtxdiTarget             = "visible";
        std::string rtgiRtxdiBias               = "";
        int         rtgiRtxdiFallback           = 0;
        int         rtgiRtxdiPermutation        = 0;
        bool        rtgiBoilingFilter           = false;
        bool        rtgiJacobianClamp           = false;
        int         rtgiHitLightSamples         = 1;
        int         rtgiLocalEmitters           = 256;
        float       rtgiSharcMaxVoxel           = 1.f;
        int         rtgiSharcMinSamples         = 2;
        bool        rtgiSharcResampling         = false;
        float       rtgiSharcCachedReset        = .6f;
        float       rtgiNrdPrepass              = 0.f;
        bool        rtgiNrdAntiFirefly          = false;
        int         rtgiNrdVarianceHistory      = 3;
        float       rtgiNrdAntilagReset         = .5f;
        float       rtgiNrdAntilagSigma         = 4.5f;
        int         rtgiNrdAtrous               = 5;
        float       rtgiNrdHistorySeconds       = .5f;
        int         rtgiNrdMinHistory           = 16;

        // ---- pass.transmission (TransmissionPrepass / TransmissionApplyPass) ----
        // Off: the Transmissive surfaces blend over the plain background by coverage, as in
        // a preview.
        bool        transmissionEnabled         = true;
        // Scales the blur a rough Transmissive surface gives what is behind it.
        float       transmissionBlurScale       = 1.f;
        // The pixels behind a refracting surface move with what they show (TransmissionMotionPass).
        // Off: they keep the opaque surface's motion, and TAA and the upscalers smear the
        // refracted image while it moves.
        bool        transmissionMotionVectors   = true;

        // ---- pass.volumetricFog (VolumetricFogPass's ray-traced lighting) ----
        // Where the froxels are ray-traced (off, auto, on; FramePlan::Decisions::fogRt: auto on the
        // frames that already trace rays), read every frame; how many cluster lights a froxel weighs
        // per frame; the history's weight; the indirect light's source (off, auto, ddgi, rays;
        // VolumetricFogPass::RtIndirectMode); the sun's shafts traced per pixel instead of in the froxels.
        std::string volumetricFogRtLighting     = "auto";
        int         volumetricFogRtCandidates   = 8;
        float       volumetricFogRtTemporalAlpha = 0.08f;
        std::string volumetricFogRtIndirect     = "auto";
        bool        volumetricFogRtShafts       = false;
        // The raymarched shafts' sun dimmed by the medium on its way to each step, as the ray-traced
        // froxels and the path tracer dim it; off: it reaches every step whole, brighter than the
        // medium lets it be.
        bool        volumetricFogRaymarchSunTransmittance = true;
    };
}

namespace Settings::Pass
{
    // Two getters per row of Passes.inl: the row's effective value, and whether
    // a layer above Default holds it. Safe on any thread; the first call of
    // either marks the row's .legacyFlag read.
#define SETTING(K, P, M, ...) decltype(::Resource::PassSettings::M) M(); bool M##Given();
#include "Settings/Rows/Passes.inl"
#undef SETTING
}
