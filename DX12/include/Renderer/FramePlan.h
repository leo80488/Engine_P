#pragma once

// Renderer/FramePlan.h - the frame's feature decisions as one pure function.
//
// PlanFrame decides which features the render half runs this frame - the TLAS,
// the path tracer, ReSTIR DI / GI, RT and screen-space reflections and the
// glass's front layer, RT shadows, RTAO / XeGTAO, SSGI, DDGI, ray
// reconstruction and its raster guides, the upscaler and the post-chain size -
// from three inputs, each read once:
//   Intent  what the game half decided: render path, AA mode, view mode and the
//           feature switches as the renderer applied them. Captured once per
//           frame on the game half right after RenderPhaseBarrier
//           (Renderer::CaptureFrameIntent) into the frame's FrameView. Game half
//           N+1 (Sim, Lua, the editor UI) may change the sources while render
//           half N runs; render half N reads this copy, so a change lands on the
//           next frame on both halves.
//   State   what exists and is ready: pass readiness and hardware tiers,
//           outputs, inert reasons, upscaler contexts and DLSS health, the DDGI
//           volume count, the sun, the camera and sizes, the ReSTIR budget
//           ladder's last stage, and the scene's IndirectLightingSettings (which
//           the scene build re-applies after the intent is captured). Read once
//           at the top of the render half (Renderer::CaptureFramePlanState): the
//           game half's ServiceEnableGatedResources / UploadFrameData / scene
//           build and the render half itself change these, and the next game
//           half's do not run before this render half ends (RenderPhaseBarrier).
//           The one exception, a Lua settings apply in the next Sim writing the
//           DDGI switch (SetDDGIAuthored), is read once here instead of at every
//           site.
//   Facts   what only the render half learns, in stages: A (nothing yet: the
//           DDGI and TLAS demand, decided before the acceleration structure is
//           built), B (after it: the TLAS, instance and bindless tables, promote
//           list, SSR pyramid, depth and normal, front-layer draws), C (the
//           resolve: the path tracer's RR inputs, the raster guides'
//           allocation). A fact can only turn a decision OFF.
//
// RenderAsync evaluates PlanFrame once per stage and its decision sites read
// Renderer::m_plan; [FRAMEPLAN] prints it whenever it changes. The game half uses
// the same function and intent: the scene-AS gather reads stage A
// (Renderer::PlanGameStageA); the render resolution, upscaler contexts and jitter
// read Renderer::GamePlan; the resource service and mip bias read the resolve at
// the frame's own sizes (Renderer::PlanGameResolve).
//
// Rules:
//   * The path tracer OWNS the frame (ptOwns) when the path traces, the view is
//     Lit and the core is usable. While it does, RT and screen-space
//     reflections, RTAO, XeGTAO and SSGI do not run. A path-tracing path whose
//     core is not usable FALLS BACK to the raster features as switched
//     ([FRAMEPLAN] fallback); a frame without a TLAS VETOES the path tracer and
//     what it replaces stays off ([FRAMEPLAN] veto).
//   * rr is the ray-reconstruction SELECTION (the render resolution follows
//     it); rrRuns adds DLSS's runtime health and drives the ReSTIR shells' NRD,
//     the path tracer's RR mode and the colour snapshot. A failed RR evaluate
//     keeps RR's resolution (which DLSS SR rejects) until
//     DLSSPass::ServiceRetry re-arms it.
//   * The TLAS is wanted exactly when some decision could trace this frame.
//   * While the path tracer owns the frame, the raster systems it does not
//     replace run only for their readers (ptReaders):
//       CSM          toon pixels (faces always), water, translucents, lit
//                    particles, fog, the face overlay, the editor's world
//                    previews (they borrow it) and a reflection-probe bake;
//       DDGI         toon pixels (their specular - rough band, sky visibility
//                    along R, probe normalization - and their ambient where the
//                    path tracer does not light them), translucents, water, the
//                    face overlay and the probe-debug spheres;
//       SSR chain    water, toon specular, the translucents' screen march;
//       RT sun mask  water, translucents, particles and toon pixels the path
//                    tracer does not light.
//     Under camera-primary path tracing toon pixels read none of them; the fog reads the
//     CSM only while the raster fog runs (fogOwner Raster). DDGI
//     resumes from its last atlas when a reader appears; the translucents'
//     screen march needs last frame's pyramid, so it starts one frame after the
//     chain. A veto frame reads whatever the skipped systems last left (the CSM
//     is never undefined: ShadowPass::PrepareUnwritten).
//
// The lighting tier (Tier) is derived from the same inputs and only describes
// the frame ([FRAMEPLAN] lighting=); choosing a tier writes the switches, it is
// never an input. Show flags are not in the plan: the render graph gates a pass
// by the view's ShowFlags (RenderPass::RequiredShowFlags).

#include <cstddef>
#include <cstdint>

namespace FramePlan
{
    // Renderer::RenderPath and Renderer::AAMode by value (Renderer_RenderAsync.cpp
    // static_asserts the mirror), so this header needs no Renderer.h.
    enum class Path : uint8_t { Raster, ReSTIR_DI, ReSTIR_GI, ReSTIR_DI_GI, PathTrace, PathTraceRealtime };
    enum class AA : uint8_t { None, FXAA, TAA, FXAA_TAA, FSR, DLSS, DLSS_RR };

    // The lighting tier, DERIVED from the path and the switches - it describes
    // the frame, it does not decide it:
    //   T0  raster lights and shadow maps, the sky (SSGI near field if the scene
    //       has it), SSR + probes, XeGTAO
    //   T1  T0 + DDGI for the diffuse GI
    //   T2  hybrid RT on top: RT reflections, RT sun shadows, RTAO (with DDGI)
    //   T3  ReSTIR DI + GI (DDGI fills what GI's coverage leaves)
    //   T4  the real-time path tracer (RTXDI ReSTIR PT + SHaRC + RR)
    //   Ref the reference path tracer (accumulating)
    // Partial (tierPartial, printed "*" or ":DI" / ":GI"): T2 without all of RT
    // reflections, RT shadows, RTAO and DDGI; T3 with DI or GI alone (the ReSTIR
    // DI / GI development paths).
    enum class Tier : uint8_t { T0, T1, T2, T3, T4, Ref };

    // Who integrates the fog along the camera rays (Decisions::fogOwner): the raster
    // fog passes, the ray-traced froxel lighting, or the reference path tracer, which
    // integrates the scene's fog as a participating medium (Graphics/MediumSet.h).
    enum class FogOwner : uint8_t { Raster, RtFroxel, PathTracer };

    // The game half's decisions for the frame (see the header comment).
    struct Intent
    {
        bool captured = false;                 // false: the frame never passed CaptureFrameIntent
        // The selected path (m_renderPath: the request, Raster on a device
        // without inline RT). The ReSTIR budget ladder's demotion is State: the
        // render half updates it at its top (EffectivePath applies it).
        Path path = Path::Raster;
        AA   aa   = AA::None;                  // m_aaMode
        bool lit = false, lightingDebug = false, wireframe = false;   // m_viewMode
        bool rtReflections = false, ssr = false, transparentReflections = false;
        bool rtShadows = false, rtao = false, ssao = false;
    };

    // What exists and is ready (see the header comment). A pass that does not
    // exist decides false, as its site would (it is skipped).
    struct State
    {
        bool budgetRaster = false;             // the ReSTIR budget ladder's last stage: a ReSTIR path renders as Raster
        bool ortho = false;
        // LightCB has a sun this frame (the game half's light upload wrote it
        // before the render half starts; lightColor is zeroed without one).
        bool sun = false;
        // DDGI / SSGI as applied: the scene's IndirectLightingSettings intent AND
        // the player's features.ddgi / ssgi. The game half's scene build applies them
        // every frame (BuildScene_UpdateDDGI), after the intent is captured, so they
        // are read here with the volumes.
        bool ddgiEnabled = false, ssgiEnabled = false;
        bool ddgiReady = false;                // m_ddgiReady && the DDGI pass
        uint32_t ddgiVolumes = 0;              // the volume manager's active volumes
        // The reflection pass (its site runs only when it exists).
        bool refl = false, reflReady = false, reflHwTier = false, reflUnified = false;
        // The glass's front layer: its reflection instance and its prepass.
        bool front = false, transparentPass = false, frontReady = false;
        bool rtShadow = false, rtShadowReady = false, rtShadowOutput = false;
        bool rtao = false, rtaoReady = false, rtaoOutput = false;
        // The path tracer and the ReSTIR shells: inert = an inert reason; output = it exists.
        bool ptPass = false, ptInert = false, ptOutput = false, ptCameraPrimary = false, ptRestirDI = false;
        bool diPass = false, diInert = false, diOutput = false;
        bool giPass = false, giInert = false, giOutput = false;
        bool ssgiPass = false, ssgiUsable = false;   // usable: no inert reason, the output exists
        bool xegtaoPass = false;
        // The resolve (its block runs only when a post pass exists).
        bool post = false;
        bool slRR = false, slDLSS = false;     // Streamline reports the feature supported
        bool rrForcedCameraRays = false;       // a -pt_camera_primary / -pt_area_* / -pt_lens_radius flag
        bool fsrContext = false, dlssContext = false;
        bool dlssHealthy = false;              // no failed RR evaluate waiting for DLSSPass::ServiceRetry
        uint32_t renderW = 0, renderH = 0, viewportW = 0, viewportH = 0;
        bool rrGuidePass = false;              // the RR guide render feature is registered
        // What this frame draws that reads a raster lighting system while the path
        // tracer owns the frame: Render_BindFrameResources' draw counts and the passes'
        // activity, read after it on the render half (the game half sees the last
        // frame's, which only its post size and tier read, neither of which uses them).
        bool translucentDraws = false;   // TransparentPass: CSM, DDGI, the RT sun mask, the SSR pyramid
        bool waterDraws = false;         // CSM, DDGI, the RT sun mask, ssrResultIdx
        bool toonDraws = false;          // CSM (faces external-only); DDGI and the SSR fold (specular)
        bool ptToonLighting = false;     // the path tracer lights the toon pixels (NprLightingReady, not camera rays)
        bool particles = false;          // lit particles: CSM, the RT sun mask (the pool resident: conservative)
        bool fog = false;                // volumetric fog: CSM (froxel inject, god rays)
        bool faceOverlay = false;        // external-only CSM, DDGI's ambient
        bool previewWorldViews = false;  // a live WorldCull preview borrows the CSM (PreviewMainShadowSnapshot)
        bool probeBake = false;          // a reflection-probe capture records this frame: CSM
        bool ddgiDebug = false;          // DDGIProbeDebugPass draws the probes this frame: DDGI
        // The path tracer integrates the fog as a participating medium: the frame has one
        // (Renderer::UploadMediumSet, earlier on the render half) and pass.pathTrace.media
        // is on. Render half only; the game half's plans read false.
        bool ptMedia = false;
        // The froxel fog's ray-traced lighting (pass.volumetricFog.rtLighting: 0 off, 1 auto,
        // 2 on) and whether the fog pass can trace (VolumetricFogPass::RtUsable).
        uint8_t fogRtMode = 0;
        bool fogRtUsable = false;
        // Harness only (-frameplan_legacy_pt, the reverse arms of
        // frame_plan_fallback / frame_plan_veto): instead of ptOwns, the path-tracing
        // PATH excludes RT / screen reflections and RTAO, and the path tracer RUNNING
        // excludes XeGTAO and SSGI.
        bool legacyPtRule = false;
        // Harness only (-frameplan_no_pt_readers, the reverse arm of
        // frame_plan_pt_readers): the raster systems run on the path-tracing path
        // whether anything reads them or not.
        bool ignorePtReaders = false;
    };

    // What only this render half learns (see the header comment).
    enum class Stage : uint8_t { A, B, C };
    struct Facts
    {
        Stage stage = Stage::A;
        // B: after the acceleration structure.
        bool tlas = false, tlasTables = false, promote = false;   // tables: instance VA + bindless table
        bool pyramid = false;                  // the SSR subsystem's scene-colour pyramid exists
        bool depth = false, normal = false;    // the GBuffer depth / normal SRVs
        bool frontDraws = false;               // the front layer has a translucent draw to stamp
        // C: the resolve. The caller reads -dlss_rr_raw only while rr is selected
        // and -rr_raster_guides only while rr is selected on a path-tracing path, so
        // run.json's unreadFlags lists them on a run that cannot use them.
        bool rrRaw = false;                    // -dlss_rr_raw
        int  rrRasterGuides = 0;               // -rr_raster_guides: >0 the PT path makes them, >=2 DLSS reads them
        bool ptRRReady = false;                // PathTracePass::RayReconstructionReady
        bool rrGuidesPrepared = false;         // RRGuidePass::Prepare (called only when rrGuidesWanted)
    };

    // One field per decision, in the order RenderAsync makes them.
    struct Decisions
    {
        // A: the path the frame renders (EffectivePath; printed as path=, not
        // one of the compared fields) and the lighting tier the path and the
        // switches describe (Tier).
        Path path = Path::Raster;
        Tier tier = Tier::T0;
        bool tierPartial = false;
        // A: the path tracer owns the frame (the rules in the header comment).
        bool ptOwns = false;
        // A: the raster systems the path tracer does not replace - the CSM and the
        // SSR chain (pyramid + FullFold) - and, while it owns the frame, which readers
        // kept them (and DDGI and the RT sun mask) on: ReaderBit, 0 off the
        // path-tracing path.
        bool csm = false, ssrChain = false;
        uint32_t ptReaders = 0;
        bool ddgi = false, tlas = false;
        // A: who integrates the camera rays' fog (FogOwner): the path tracer while it owns a
        // reference frame and integrates the medium (State::ptMedia), else the ray-traced froxels
        // where pass.volumetricFog.rtLighting wants them, else the raster passes.
        FogOwner fogOwner = FogOwner::Raster;
        // A too: they read the intent and the state only, so the game half's
        // resolution, upscaler contexts and jitter read them from the same plan
        // (Renderer::GamePlan). rr = IsRayReconstructionActive (the selection),
        // rrRuns = rr with DLSS healthy, fsr = IsFSRActive, dlssWanted =
        // IsDLSSActive (DLSS SR or RR selected and applicable to the path).
        bool rr = false, rrRuns = false, fsr = false, dlssWanted = false;
        bool rtr = false, ssrTier = false, front = false;   // B
        bool rtShadow = false, rtao = false;
        bool pt = false, di = false, gi = false;
        bool ssgi = false, xegtao = false;
        // B: the froxel fog is lit by ray tracing (it needs the TLAS): for the camera rays
        // (fogOwner RtFroxel) or, while the path tracer owns the fog, for the surfaces drawn
        // after it alone. Without the TLAS an RtFroxel frame takes the raster lighting.
        bool fogRt = false;
        bool dlss = false;                     // C
        bool fsrResolved = false, dlssResolved = false;
        // C: FSR or DLSS Super Resolution was selected and applies, but its context
        // did not come up this frame - TAA resolves it (at the render size) instead of
        // no AA at all. Not for ray reconstruction, which has its own fallback
        // (rrRuns).
        bool taaStandIn = false;
        uint32_t postW = 0, postH = 0;
        bool rrGuidesWanted = false, rrGuides = false;
    };

    // Decisions::ptReaders: the readers present while the path tracer owns the
    // frame (State's reader fields, the toon pixels split by whether the path
    // tracer lights them; neither toon bit under camera-primary rays).
    enum ReaderBit : uint32_t
    {
        kReaderTranslucent = 1u << 0,
        kReaderWater       = 1u << 1,
        kReaderToon        = 1u << 2,   // any toon pixels (faces among them)
        kReaderToonUnlit   = 1u << 3,   // toon pixels the path tracer does not light
        kReaderParticles   = 1u << 4,
        kReaderFog         = 1u << 5,
        kReaderFaceOverlay = 1u << 6,
        kReaderPreview     = 1u << 7,   // the editor's world previews (the borrowed CSM)
        kReaderProbeBake   = 1u << 8,   // a reflection-probe capture (the CSM)
        kReaderDdgiDebug   = 1u << 9,   // the DDGI probe-debug spheres
    };

    // The path the frame renders (Renderer::GetRenderPath): the selected one,
    // or Raster for a ReSTIR path the budget ladder has demoted.
    Path EffectivePath(const Intent& intent, const State& state);

    // The decisions this stage can make; the fields of later stages stay false.
    Decisions PlanFrame(const Intent& intent, const State& state, const Facts& facts);

    // Bit i set: field i (in Format's order) differs between @p a and @p b.
    uint64_t DiffMask(const Decisions& a, const Decisions& b);

    // Every field as "name=value", for the [FRAMEPLAN] lines.
    void Format(const Decisions& d, char* out, size_t outSize);
    // The same fields one by one, in that order (Lua Render.plan()); bools are 0 / 1.
    void ForEachDecision(const Decisions& d, void (*fn)(void* ctx, const char* name, uint32_t value), void* ctx);

    const char* PathName(Path p);
    // "T0".."T4", "Ref", with the partial mark: "T2*", "T3:DI", "T3:GI".
    const char* TierName(const Decisions& d);
}
