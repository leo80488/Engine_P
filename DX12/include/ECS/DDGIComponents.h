#pragma once

// ECS data for Dynamic Diffuse Global Illumination:
//   DDGIVolumeComponent               authored volume parameters; serialized, editor-edited.
//   DDGIVolumeRuntimeComponent        DDGIVolumeManager's per-volume state; not serialized.
//   DDGISceneTagComponent             DDGI TLAS membership marker (not read yet).
//   IndirectLightingSettingsComponent per-world GI settings (one entity per world).

#include "Resource/SystemHandles.h" // ResourceHandle uint64_t typedef
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

// ==== DDGIVolumeComponent - authored parameters ====
// One component per DDGI volume: an arbitrarily placed AABB filled with a regular probe grid.
// Volumes may overlap; shading picks by priority, then probe density, and blends out at the
// boundary fade.

struct DDGIVolumeComponent
{
    DirectX::XMFLOAT3 origin       = { 0.0f, 0.0f, 0.0f };  // world-space center
    DirectX::XMFLOAT3 extent       = { 5.0f, 3.0f, 5.0f };  // half-size of the AABB

    // Probe grid resolution per axis, practical range [4, 32] each. The atlas size scales with
    // X x Y x Z.
    uint32_t          probeCountsX = 16;
    uint32_t          probeCountsY = 8;
    uint32_t          probeCountsZ = 16;

    // Rays per probe per frame. Common values: 64 (perf), 128 (default), 256 (quality).
    uint32_t          raysPerProbe = 128;

    // Temporal amortization: update only 1/N of the probes each frame, interleaved by
    // (x + 2y + 3z) % N so every 2x2x2 block covers all N phases and no region lags as a group.
    // Trace + relight cost scales ~1/N (it is dominated by rays per frame). 1 = every probe every
    // frame; practical range [1, 8]. The knob for large grids: 32x16x32 at 128 rays is 2.1 M rays
    // per frame, 524 K at N = 4.
    // DDGIRelight multiplies the EMA blend speed by N (see updateCompensation), so the wall-clock
    // response is kept; the trade is temporal resolution: a change lands up to N frames late and
    // each estimate has N x fewer samples per unit time, so low `hysteresis` plus high N looks
    // noisier. With hysteresis >= 0.92, N = 2..4 is visually free.
    uint32_t          probeUpdateDivisor = 1;

    // How the relight blend compensates for probeUpdateDivisor (N):
    //     blendSpeed = max(1 - hysteresis, 0.005) * N^updateCompensation
    // (depth path: retain = hysteresis^(N^updateCompensation)).
    //   1 - keep the wall-clock RESPONSE TIME. Each update still integrates the same ray budget
    //       while the step is N x larger, so steady-state noise variance grows ~N x: at N = 4 a
    //       hysteresis of 0.99 filters like 0.96 at N = 1.
    //   0 - keep the NOISE FLOOR of N = 1; the response time stretches N x instead. Right for
    //       mostly static lighting (a night scene lit by fixed lamps / emissives).
    //   Values in between interpolate the trade.
    float             updateCompensation = 1.0f;

    // Per-texel adaptive change response. 0 = off (the plain EMA). When > 0, a texel whose new
    // sample diverges strongly from its history blends up to (1 + boost) x faster, so a real
    // lighting change lands quickly without lowering hysteresis globally. Feed-forward and
    // per-texel, so it cannot form the ray-count feedback loop of enableAdaptiveRayBudget. Opt-in:
    // with ray jitter on, Monte-Carlo noise can sit near the trigger threshold, and a permanently
    // engaged boost is just a lower hysteresis (flicker). Tune by eye; try 2-4.
    float             changeResponseBoost = 0.0f;

    // EMA hysteresis of the per-texel relight blend; blendSpeed = 1 - hysteresis per frame.
    //   0.92  -> 8 %/frame,   ~0.5 s to 90 % at 60 fps (default, responsive)
    //   0.965 -> 3.5 %/frame, ~1.1 s to 90 % at 60 fps (smooth, slow)
    //   0.85  -> 15 %/frame, noisier but tracks fast motion
    float             hysteresis   = 0.92f;

    // Per-frame ray-set rotation jitter as a fraction of the automatic amplitude (~ the Halton
    // gap radius, sqrt(pi / raysPerProbe)). 1 = full jitter (best gap filling, least bias);
    // 0 = static ray set (structured banding at low ray counts). The jitter is the only per-frame
    // variable in the probe trace, so it dominates temporal flicker on hard directional shadows;
    // at high ray counts ~0.3-0.5 steadies sun lighting for a little extra bias.
    float             rotationJitterScale = 1.0f;

    // Sampling biases in ABSOLUTE world units: they push the sample position off the surface so
    // the Chebyshev occlusion test does not read the surface itself. On a large flat surface the
    // distance to the probe and the probe's filtered depth are nearly equal, so the test sits on
    // a noisy knife edge and shows soft blobby occlusion patches.
    // The defaults suit sub-metre probe spacing; at 3.5 m spacing 0.25 is 1/14 of a cell and the
    // guard stops working (which is why "more probes" seems to fix it). Prefer biasSpacingScale.
    // Prefer normalBias over viewBias: viewBias follows the camera direction, so a large value
    // makes indirect lighting swim as the camera moves.
    float             normalBias   = 0.25f;
    float             viewBias     = 0.10f;

    // Extra bias as a fraction of the smallest probe spacing, added to both absolute biases:
    //     effective = <absolute> + biasSpacingScale * min(probeSpacing)
    // It tracks the cell size, so changing probe counts or extent needs no re-tune. Folded in on
    // the CPU (DDGIVolumeManager::Tick): no GPU field. 0 = off (absolute biases only). ~0.25 is a
    // good start (~0.88 m at 3.5 m spacing); too large leaks light through thin walls.
    float             biasSpacingScale = 0.0f;
    // Volume-edge fade ratio (0..0.5): the outer fraction of the half-extent per axis
    // (DDGI_VolumeFadeWeight's Chebyshev distance) over which the volume's weight ramps to 0 at
    // its faces. What the volume does not cover goes to the SKY fallback, so a wide band trades
    // probe bounce for unoccluded sky on every surface inside it - including a street near the
    // bottom face of a tall volume.
    float             boundaryFadeRatio = 0.12f;

    // Share of the PERCEPTUAL (sqrt-space) inter-probe blend, in [0, 1]: 1 = reference DDGI,
    // 0 = plain linear weighted mean.
    // The sampler combines the 8 surrounding probes as E = (sum(w * sqrt(E_i)) / sum(w))^2, which
    // is always <= the linear mean; the deficit grows with probe contrast (~10% at a 4x spread,
    // ~21% at 10x, ~40% at 100x), so a hard sun / shadow boundary gets a smooth DARK LOBE that
    // denser probes shrink but never remove. The perceptual blend hides probe-cell seams in dark
    // regions, so this is a real trade: lower it if the lobes bother you more than the seams
    // (0 making the lobes vanish confirms the mechanism). This engine is more exposed than the
    // reference: irradiance is stored as L1 SH, not an octahedral map, so more spatial detail
    // comes from the inter-probe interpolation the power mean darkens.
    float             perceptualBlendWeight = 1.0f;

    // Probe relocation / classification passes (g_Vol.flags bits 1 / 2).
    bool              enableRelocation     = true;
    bool              enableClassification = true;
    // Adaptive per-probe ray budget (discrete tiers + a periodic full-budget audit). Keep it off
    // unless a profile says DDGI's ray count is on the critical path.
    // Hazard: variance-driven ray counts form a closed loop - fewer rays give a biased estimate,
    // bias reads as inconsistency, inconsistency buys rays - that oscillates over tens of frames,
    // a low frequency the relight EMA passes, so the whole scene slowly "breathes". The loop is
    // broken by auditing at full budget every 16 updates and deciding downgrades only on the
    // update after an audit; this has not been validated over long runs, so if the scene
    // breathes, suspect this first. probeUpdateDivisor 2-4 gives the same saving more safely.
    bool              enableAdaptiveRayBudget = false;

    // Priority: a higher value wins where volumes overlap (probe density breaks ties) and is
    // promoted one camera-relevance tier. cascadeLevel (0 = nearest) is packed for the GPU but no
    // shader reads it.
    // TODO(DDGI_Integration_Plan Phase 5): multi-cascade volumes and cascade selection in shading.
    int               priority      = 0;
    uint32_t          cascadeLevel  = 0;

    // Draws this volume's probes in the DDGI debug view; no shading effect.
    bool              debugDraw     = false;

    // Per-volume diffuse tint and scale (an art / debug knob; 1 leaves DDGI physical).
    DirectX::XMFLOAT3 diffuseTint   = { 1.0f, 1.0f, 1.0f };
    float             diffuseScale  = 1.0f;
};

// ==== DDGIVolumeRuntimeComponent - DDGIVolumeManager state ====
// Renderer::BuildScene_UpdateDDGI adds one to every new volume; DDGIVolumeManager allocates
// its slot, reallocates on a probe-layout change and frees slots no volume claims.

using DDGIResourceHandle = uint64_t;
inline constexpr DDGIResourceHandle kInvalidDDGIResource = 0xFFFFFFFFFFFFFFFFull;

struct DDGIVolumeRuntimeComponent
{
    // Camera relevance of a volume, graded by Renderer::BuildScene_UpdateDDGI from the camera's
    // distance to the AABB surface; a less relevant tier never gets a smaller divisor.
    enum class RelevanceTier : uint32_t
    {
        Inside  = 0,  // camera in (or just outside) the AABB: authored rate
        Near    = 1,
        Far     = 2,
        Dormant = 3,  // skipped entirely except for a periodic heartbeat
        Count
    };

    // Index into DDGIVolumeManager::m_volumes. ~0u = not yet allocated.
    uint32_t           volumeSlot = 0xFFFFFFFFu;

    // Probe layout the slot was allocated with (written by DDGIVolumeManager::AllocateOrUpdate);
    // a change in probe counts or raysPerProbe reallocates, an unchanged layout keeps the slot.
    uint32_t           probeCountX = 0;
    uint32_t           probeCountY = 0;
    uint32_t           probeCountZ = 0;
    uint32_t           raysPerProbe = 0;

    // Per-volume frame counter: seeds the ray rotation and the interleave phase. Advances only
    // on a frame the volume's dispatches ran.
    uint32_t           frameCounter = 0;

    // Frames left of atlas overwrite mode: while > 0 the relight CS skips the hysteresis lerp
    // and writes the fresh sample, so a new volume converges in 1-2 frames whatever memory the
    // atlas was created with. Decremented by DDGIVolumeManager::Tick only after a frame the
    // volume executed.
    uint32_t           firstFramesRemaining = 3;

    // Per-frame random ray rotation (3x3, row-major), from GenerateRandomRotation in
    // DDGIVolumeManager::Tick.
    float              randomRotation[9] = { 1,0,0, 0,1,0, 0,0,1 };

    // Toroidal scroll offset; nothing scrolls volumes, so it stays zero (volumes are static in
    // world space).
    // TODO(DDGI_Integration_Plan Phase 5): camera-centric scrolling with toroidal addressing.
    int32_t            scrollOffsetX = 0;
    int32_t            scrollOffsetY = 0;
    int32_t            scrollOffsetZ = 0;

    // ---- Camera-relevance scheduling ----
    // Written every frame by the policy in Renderer::BuildScene_UpdateDDGI, read by
    // DDGIVolumeManager::Tick (which uploads effectiveDivisor and mirrors executeThisFrame into
    // its slot). Not serialized. It turns "N volumes cost N x as much" into "N volumes cost more
    // latency": distant volumes update less often instead of every volume running its full ray
    // budget every frame.
    RelevanceTier      relevanceTier = RelevanceTier::Inside;

    // Divisor in effect (the authored divisor raised to the tier's floor, hysteresis-capped),
    // before Tick's [1, 8] clamp. The policy switches it only on an interleave phase boundary (at
    // once while waking) and then restarts frameCounter. Never written back to
    // DDGIVolumeComponent, which is authored, serialized data.
    uint32_t           effectiveDivisor = 1;

    // False on a frame this volume is skipped (Dormant, except on its heartbeat). The dispatch
    // loop iterates slots, so it reads the manager's mirror of this.
    bool               executeThisFrame = true;

    // Frames since this volume last executed. Drives the Dormant heartbeat.
    uint32_t           framesSinceExecute = 0;

    // True when the hysteresis cap lowered the divisor the authored value and tier asked for.
    // TODO: show it in the inspector, so "raising the divisor does not help" has a visible answer.
    bool               divisorCappedByHysteresis = false;

    // Frames of forced full rate (divisor 1) after the volume moves up a relevance tier.
    // Do not use the overwrite path (firstFramesRemaining / flag bit 3) for this: it writes raw
    // single-frame estimates into the atlas, which reads as a one-frame flash; full rate lets the
    // normal EMA catch up.
    uint32_t           wakeWarmupRemaining = 0;
};

// ==== DDGISceneTagComponent - TLAS membership marker ====
// Meant to force an entity into (or out of) the DDGI TLAS. The TLAS build (DDGISceneAS) does
// not read it: it takes every non-skinned static mesh.
// TODO(DDGI_Integration_Plan 3.1): honour the tag to force-include / force-exclude entities.

struct DDGISceneTagComponent
{
    // Placeholder; the tag's presence is the state.
    uint8_t reserved = 0;
};

// ==== IndirectLightingSettingsComponent - per-world GI settings ====
// One per world, on a settings entity. The scene build (BuildScene_UpdateDDGI) copies it into
// the Renderer every frame, so change it on the entity (inspector, Lua Entity.set
// "IndirectLighting"). A world without one runs with these defaults.

struct IndirectLightingSettingsComponent
{
    // Whether this scene uses DDGI (its intent). What runs is this AND the player's
    // features.ddgi; when off, Lighting skips DDGI sampling and falls back to sky IBL diffuse.
    bool   ddgiEnabled               = true;

    // Global scale on the DDGI diffuse contribution. 1 is PHYSICAL: the atlas stores E(N)/pi and
    // Lighting.ps applies kD x albedo x sample x scale with no /pi, matching the direct path's
    // kD x albedo/pi x E. Any other value is an artistic multiplier on GI only; a scene that
    // serialized ddgiScale = 3.1416 runs GI pi x hotter than direct until re-tuned.
    float  ddgiDiffuseScale          = 1.0f;

    // AO attenuation on the DDGI diffuse path. Probes already encode mid-scale visibility
    // (probe-spacing distances ~0.5-2 m), so full screen-space AO on top darkens those scales
    // twice. Lerps the AO term toward 1 for the DDGI contribution only: 0 = no AO on DDGI, 1 = full
    // AO (double occlusion); ~0.4 keeps near-field contact shadow. The sky IBL fallback always
    // uses full AO.
    float  ddgiAONearFieldStrength   = 0.4f;

    // ---- Screen-space GI (SSGIPass) ----
    // The near-field half of the hybrid diffuse GI; DDGI is the far field. They own disjoint solid
    // angles and Lighting.ps combines them as a hemisphere partition, so ssgiEnabled = false gives
    // exactly the DDGI-only result. Off by default (it changes every lit surface). Scene intent:
    // what runs is this AND the player's features.ssgi.
    bool   ssgiEnabled       = false;
    // 0..1 kill switch / A-B fade, not a brightness knob: SSGI shares ddgiDiffuseScale with the
    // far field, because two independent scales turn every coverage gradient into a visible band.
    float  ssgiIntensity     = 1.0f;
    // Screen-march range in metres. Short on purpose: this tier supplies what the probe grid
    // (~3.5 m spacing in the reference scenes) cannot resolve; longer marches cost iterations for
    // pixels the far field already describes.
    float  ssgiMaxDistance   = 3.0f;
    // Rays per traced pixel on the non-reservoir path. The trace runs at half resolution, so
    // this is rayCount/4 rays per full-res pixel: the default 2 is 0.5 rpp (Lumen ships 0.25 rpp,
    // HDRP's ceiling is 1 rpp; 4 rays at full res cost ~4.4 ms). The pass clamps it to >= 2; the
    // reservoir path traces one ray.
    uint32_t ssgiRayCount    = 2;
    // ReSTIR-GI-style half-res sample reuse: one fresh ray per traced pixel (0.25 full-res rpp),
    // with temporal history and neighbouring reservoirs reconnected / validated before REBLUR.
    // Opt-in; off keeps the ssgiRayCount path.
    bool     ssgiReservoirEnabled = false;
    uint32_t ssgiReservoirSpatialCandidates = 6; // 0..8
    uint32_t ssgiReservoirMaxM = 16;              // 1..63
    uint32_t ssgiReservoirMaxAge = 4;             // selected-sample lifetime
    uint32_t ssgiReservoirValidationSteps = 6;    // clamped 4..6
    // Depth-buffer thickness floor (world units) for the hit-uncertainty gate.
    // Raise it if thin geometry produces missing bounce; lower it if rays
    // accept surfaces they actually passed behind.
    float  ssgiThickness     = 0.15f;
    // Bring-up / verification only.
    //   0 = normal
    //   1 = CALIBRATION: skip the march and return the far-field DDGI irradiance at coverage 1;
    //       Lighting.ps computes rgb + far*(1-1) = rgb, so THE FRAME MUST NOT CHANGE (checks units,
    //       partition algebra and the binding chain at once).
    //   2 = COVERAGE: resolved fraction as greyscale, at coverage 1.
    //   3 = NEAR TERM: SSGI's bounce alone, amplified, at coverage 1.
    // Run 2 before trusting 1: mode 1 is a null test, and a pass that never ran also shows "no
    // change". Modes 2 / 3 show only this texture: if the screen does not change dramatically,
    // the pass is not running or ssgiResultIdx is 0.
    uint32_t ssgiDebugMode   = 0;

    // Not read by any shading code; kept only because scene files serialize them.
    bool   reflectionProbePriorityOverDDGI = true;
    bool   useDDGIForRoughSpecularFallback = false;

    // Scale on the sky-SH ambient baked into reflection-probe cubemaps (ProbeCapture.ps). The bake
    // also multiplies by the live iblStrength, so probes capture at the main pass's indirect
    // level. Lower it to darken over-bright interior captures without touching the runtime sky
    // IBL. 1 = full captured ambient.
    float  reflectionProbeBakeAmbient = 1.0f;

    // Scene-wide absolute NPR ceiling (LightCB.nprMaxBrightness): an exposed-space max-RGB soft
    // shoulder in NPRStabilize (knee at half, asymptote at the value; see
    // MaterialComponent::nprMaxBrightness). 0 = no scene ceiling. It is min()ed with the
    // per-material value, so a per-material value above it cannot lift the cap: it stops one
    // blown-out NPR pixel from dragging bloom / the AutoExposure histogram. Reflected NPR hits get
    // the same ceiling via HybridReflConstants.sceneNprMaxBrightness.
    float  nprMaxBrightness           = 0.0f;
};
