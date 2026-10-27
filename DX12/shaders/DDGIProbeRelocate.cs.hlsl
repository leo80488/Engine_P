// DDGIProbeRelocate.cs.hlsl - moves probes out of nearby surfaces and classifies probes
// buried in solid geometry as INACTIVE. Also exports the always-on sky visibility (and
// its first moment) from the same ray scan.
// Three forces on ProbeData.offset, all from this frame's rays:
//   (A) Frontface push (as in WickedEngine's ddgi_updateCS): each ray with
//       0 < depth < keepDistance pushes by -rayDir * (keepDistance - depth), averaged
//       over the close hits.
//   (B) Backface classification + escape: a backface hit means the probe is on the
//       solid side along that ray. Smoothed backface fraction > 25% -> INACTIVE,
//       < 10% -> ACTIVE, dead band in between. Escape pushes along the closest
//       backface ray, through the surface (RTXGI convention).
//   (C) Return-to-grid pull (RTXGI ProbeRelocation's third branch): while not
//       escaping, pull back toward the authored grid point, bounded by the clearance
//       toward it, so a probe displaced by an obstacle returns once it leaves.
// The summed push is low-passed (pushSmooth), integrated at 0.02 per frame, frozen
// under a dead-band and clamped to +-0.45 cell so the trilinear blend stays valid.
// Read before touching the push math: the ray set is re-rotated every frame, so every
// ray-scan total is a random variable even for a static probe, and the offset is the
// next frame's trace origin. The escape threshold and cap, the push EMA and the
// dead-band keep that noise from random-walking the probe; do not add an unfiltered
// term to the push.
// Hit-distance encoding (DDGIRayTrace.cs):
//   depth > 0       - frontface hit at this distance
//   depth == -1.0   - miss (sky)
//   depth < -1.5    - backface hit, real t = -(depth + 2.0)
// Bindings:
//   b0 space0 - DDGIVolumeGPU CB
//   u0 space0 - RWStructuredBuffer<DDGIProbeData> (offset, state, sky statistics)
//   t2 space0 - StructuredBuffer<float4> ray data (depth in .w)
//   u2 space0 - RWStructuredBuffer<uint> per-probe ray count

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
RWStructuredBuffer<DDGIProbeData>  g_ProbeData : register(u0, space0);
StructuredBuffer<float4>           g_RayData  : register(t2, space0);
RWStructuredBuffer<uint>           g_RayCount : register(u2, space0);

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeIdx   = DTid.x;
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    if (probeIdx >= probeCount) return;

    // Flag bits (DDGIVolumeComponent toggles, packed by DDGIVolumeManager):
    //   bit 1 - enableRelocation
    //   bit 2 - enableClassification
    const bool doRelocate = (g_Vol.flags & 2u) != 0u;
    const bool doClassify = (g_Vol.flags & 4u) != 0u;
    // Adaptive ray prefix; direction reconstruction uses Halton(2,3) which
    // is index-stable independent of count (matches trace + relight). Ray
    // data buffer stride is the volume's MAX raysPerProbe.
    const uint adaptiveRays = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    const uint rayBase      = probeIdx * g_Vol.raysPerProbe;
    if (adaptiveRays == 0u)
    {
        // No rays this frame: keep the sky history, but still reset the grid state when
        // relocation and classification are both off, so stale state cannot persist.
        if (!doRelocate && !doClassify)
        {
            DDGIProbeData pd0 = g_ProbeData[probeIdx];
            pd0.state      = DDGI_PROBE_STATE_ACTIVE;
            pd0.offset     = float3(0, 0, 0);
            pd0.pushSmooth = float3(0, 0, 0);
            g_ProbeData[probeIdx] = pd0;
        }
        return;
    }

    // Probes keep at least this distance from any surface: a quarter of the smallest
    // cell dimension, so the threshold scales with the volume's resolution.
    const float minCell      = min(g_Vol.probeSpacing.x,
                                   min(g_Vol.probeSpacing.y, g_Vol.probeSpacing.z));
    const float keepDistance = minCell * 0.25;

    // Read the probe first: the return-to-grid clearance (C) is measured around the pull
    // direction, -offset.
    DDGIProbeData pd = g_ProbeData[probeIdx];
    const float  offLen  = length(pd.offset);
    const float3 pullDir = (offLen > 0.0) ? (-pd.offset / offLen) : float3(0, 0, 0);

    // Per-probe ray-scan totals.
    float3 offsetAccum     = float3(0, 0, 0);  // (A) close-frontface push
    uint   closeHits       = 0;
    uint   backfaceHits    = 0;
    uint   skyMisses       = 0;
    float3 skyDirSum       = float3(0, 0, 0);  // sum of the missing rays' directions
    float  closestBackT    = 1e9;              // (B) closest backface hit
    float3 closestBackDir  = float3(0, 1, 0);
    // (C) clearance TOWARD the grid point: the smallest axial projection of a
    // frontface hit inside a ~26 deg cone (cos > 0.9) around the pull
    // direction. Narrow on purpose: a wall the probe sits 0.7 keepDistance
    // BESIDE projects to >= 1.45 keepDistance and so does not suppress a pull
    // along that wall; a surface between the probe and its grid point
    // projects to ~its distance and does. Frames whose random rotation puts
    // no ray in the cone read as clear; the push EMA absorbs that exactly as
    // it absorbs (A)'s per-frame variance.
    float  clearTowardGrid = 1e9;

    [loop] for (uint r = 0; r < adaptiveRays; ++r)
    {
        const float depth = g_RayData[rayBase + r].w;

        // Reconstruct the world-space ray direction. Must mirror the trace +
        // relight reconstruction exactly (same adaptive sample index + random
        // rotation) or the offset push goes the wrong way.
        const uint sampleIdx = DDGI_AdaptiveSampleIdx(r, adaptiveRays, g_Vol);
        const float3 dirLocal = DDGI_HaltonSphere(sampleIdx);
        const float3 rayDir   = DDGI_ApplyRandomRotation(dirLocal, g_Vol);

        // DDGIRayTrace's exact miss sentinel: this direction reached the environment. Counted
        // whatever the relocation / classification toggles say: skyVisibility and its first
        // moment (the missing rays' mean direction) are an always-on, nearly free export of
        // this ray scan.
        if (depth == -1.0)
        {
            skyMisses++;
            skyDirSum += rayDir;
        }

        // Backface hit: the probe is on the solid side along this direction. Not part of the
        // close-hit push (t may exceed keepDistance, but the geometry still wraps the probe).
        // Track the closest one: the escape (B) moves the probe along that ray, through the
        // surface, to its open front side.
        if (depth < -1.5)
        {
            backfaceHits++;
            const float bt = -depth - 2.0;
            if (bt < closestBackT)
            {
                closestBackT   = bt;
                closestBackDir = rayDir;
            }
            continue;
        }

        // Return-to-grid clearance (C); see clearTowardGrid above.
        if (depth > 0.0)
        {
            const float c = dot(rayDir, pullDir);
            if (c > 0.9) clearTowardGrid = min(clearTowardGrid, depth * c);
        }

        // Miss or far frontface: no relocation pressure either way.
        if (depth <= 0.0 || depth >= keepDistance) continue;

        // Close frontface hit: push away from it, in proportion to how close.
        offsetAccum -= rayDir * (keepDistance - depth);
        closeHits++;
    }

    // Sky visibility history. Overwrite frames seed immediately; steady-state
    // uses a conservative EMA so the low-frequency fallback does not shimmer
    // with adaptive ray-count changes.
    const float skyNow = float(skyMisses) / float(adaptiveRays);
    const bool overwrite = (g_Vol.flags & 8u) != 0u;
    pd.skyVisibility = overwrite ? skyNow
                                 : lerp(saturate(pd.skyVisibility), skyNow, 0.05);
    // The first moment, with the same history. The ray set is a randomly
    // rotated Halton sphere, so over the uniform directions the mean of
    // (miss ? dir : 0) is the Monte-Carlo estimate of (1/4pi) * integral of
    // V(w) w dw: with skyVisibility (the zeroth moment) the L1 projection that
    // DDGI_SkyVisibility evaluates as V(d) ~ v0 + 3 k dot(v1, d).
    const float3 skyDirNow = skyDirSum / float(adaptiveRays);
    pd.skyVisDir = overwrite ? skyDirNow : lerp(pd.skyVisDir, skyDirNow, 0.05);

    if (!doRelocate && !doClassify)
    {
        // Relocation and classification both off: keep the always-on sky statistics and
        // restore the pristine grid state.
        pd.state      = DDGI_PROBE_STATE_ACTIVE;
        pd.offset     = float3(0, 0, 0);
        pd.pushSmooth = float3(0, 0, 0);
        g_ProbeData[probeIdx] = pd;
        return;
    }

    // ---- Classification (B) ----
    // Verdicts use a temporally smoothed backface fraction. The ray set is re-rotated every
    // frame, so the raw fraction of a facade-straddling probe swings far beyond binomial
    // noise, across the whole 10%..25% dead band. A state toggle re-weights all 8
    // surrounding trilinear cells at once and bypasses the irradiance hysteresis: a
    // region-scale dark <-> bright oscillator in high-contrast (night) scenes. The EMA
    // gives the binary state the damping everything else has.
    const uint  prevState = DDGI_ProbeState(pd.state);
    bool        hasBf;
    const float bfPrev = DDGI_ProbeBfSmooth(pd.state, hasBf);
    const float bfNow  = float(backfaceHits) / float(adaptiveRays);
    // Seed directly on burn-in/overwrite frames and on the first measurement
    // (biased-255 encoding: 0 = never measured); otherwise EMA. INACTIVE
    // probes only get rays every ~16th frame (PrepareRayCount retest), so
    // they blend faster per update to keep a comparable wall-clock response.
    float bfSmooth;
    if (overwrite || !hasBf)
        bfSmooth = bfNow;
    else
        bfSmooth = lerp(bfPrev, bfNow,
                        (prevState == DDGI_PROBE_STATE_INACTIVE) ? 0.34 : 0.15);

    // FRESH is a one-frame handshake, set on INACTIVE -> ACTIVE below. PrepareRayCount
    // sees it next frame, grants a full budget and marks the probe's variance texels for a
    // one-shot relight overwrite (seed the history from a fresh trace instead of popping
    // in the stale, often blackened mean that would crawl for ~100 frames); samplers skip
    // FRESH probes for that frame. Cleared here on the next run (this kernel only runs
    // with rays, which FRESH guarantees), so the bit lives exactly one relocate interval.
    bool fresh = false;

    uint newState = prevState;
    if (doClassify)
    {
        if (bfSmooth > 0.25)
            newState = DDGI_PROBE_STATE_INACTIVE;
        else if (bfSmooth < 0.10)
            newState = DDGI_PROBE_STATE_ACTIVE;
        // else: dead band, keep the current state.
        fresh = (prevState == DDGI_PROBE_STATE_INACTIVE) &&
                (newState  == DDGI_PROBE_STATE_ACTIVE);
    }
    else
    {
        // Classification is off: clear any stale INACTIVE, or samplers skip those probes
        // forever with no writer left to reactivate them.
        newState = DDGI_PROBE_STATE_ACTIVE;
    }
    pd.state = DDGI_PackProbeState(newState, fresh, bfSmooth);

    // ---- Relocation (A, B escape, C) ----
    if (doRelocate)
    {
        // ---- This frame's instantaneous push ----
        float3 push = float3(0, 0, 0);

        // (A) Average the per-ray frontface push so its magnitude stays in [0, keepDistance]
        // however many rays hit; an unscaled sum saturates the +-0.45-cell clamp on probes
        // with many close hits.
        if (closeHits > 0u)
            push += offsetAccum / float(closeHits);

        // (B) Escape push along the closest backface ray, through the surface, to
        // keepDistance past its open front side (RTXGI convention): a backface hit means the
        // open side lies beyond the hit point, so crossing it is the shortest way out. Do not
        // push opposite the mean backface direction: it cancels to ~0 inside a floor slab (up
        // and down rays both hit backfaces) and points the wrong way at single-sided walls.
        // Bounds on this term:
        //  * A threshold, not a linear ramp from 0: closestBackDir is whichever stochastic ray
        //    hit nearest, re-randomised every frame, so escape pressure on probes merely
        //    grazing a wall is the dominant jitter source in dense scenes.
        //  * The ramp runs over the smoothed fraction and starts at the INACTIVE threshold
        //    (0.25). Do not overlap the classification dead band [0.10, 0.25]: a probe held
        //    ACTIVE there is pushed through the wall until it is classified out, then returns
        //    only via the retest at < 10% - a slow limit cycle. Burn-in seeds
        //    bfSmooth = bfNow, so a newly spawned buried probe still escapes from frame 1.
        //  * Magnitude capped to one cell diagonal: closestBackT is bounded only by the
        //    trace's TMax (50 units), and the offset is clamped to +-0.45 cell anyway, so
        //    anything larger is noise injected into the EMA.
        if (backfaceHits > 0u)
        {
            const float escape  = smoothstep(0.25, 0.45, bfSmooth);
            const float maxStep = length(g_Vol.probeSpacing);   // one cell diagonal
            const float dist    = min(closestBackT + keepDistance, maxStep);
            push += closestBackDir * dist * escape;
        }

        // (C) Return-to-grid pull (RTXGI ProbeRelocation's third branch). (A) and (B) only
        // push away from geometry, so without it a probe displaced by an obstacle (a door, a
        // moved prop) keeps its offset after the obstacle leaves: the push decays and the
        // dead-band freezes it. The authored grid point is the better sample point
        // (undistorted trilinear cell), so pull back whenever the way is clear.
        // Gates and bound:
        //  * bfSmooth <= 0.25 (smoothed, not the raw count): exactly where the escape ramp is
        //    zero, so escape and return are mutually exclusive (RTXGI's else-if). It also
        //    covers dead-band probes and probes beside single-sided cards that see a backface
        //    ray or two every frame; a raw backfaceHits == 0 gate opens on ~2% of their frames
        //    and the filtered pull never clears the dead-band.
        //  * No closeHits == 0 gate, and the bound is clearTowardGrid (directional, see the ray
        //    loop). A probe pushed toward a floor or wall parks ~0.7-1.0 keepDistance from it
        //    and sees it with ~10% of its rays every frame, so a no-close-hit gate or a
        //    nearest-hit-in-any-direction bound would never let it move. With the directional
        //    bound a surface behind the pull does not constrain it, and a surface between the
        //    probe and its grid point gives clearTowardGrid < keepDistance -> margin < 0 -> no
        //    pull, so (C) never carries the probe into (A)'s shell; near d ~ keepDistance both
        //    forces are small, opposite and filtered, and settle instead of cycling.
        // Rate: the shared 0.02 integrator, a ~50-frame time constant (~2 s from a full-cell
        // offset).
        if (bfSmooth <= 0.25 && offLen > 0.0)
        {
            const float margin = min(offLen, clearTowardGrid - keepDistance);
            if (margin > 0.0)
                push += pullDir * margin;
        }

        // ---- Temporal low-pass of the push signal ----
        // Every input to push is stochastic (see the file header) and the offset feeds the
        // next frame's trace origin, so integrating raw push is a self-sustaining random walk:
        // some ray finds a nearby wall almost every frame, and the probe never stops.
        // Filtering push before integrating cancels its zero-mean part over ~1/kPushBlend
        // frames while a persistent displacement survives at full strength. 0.05 ~ a 20-frame
        // window: a newly buried probe still escapes in a fraction of a second, and per-frame
        // ray noise is rejected.
        const float kPushBlend = 0.05;
        float3 pushSmooth = pd.pushSmooth;
        if (any(isnan(pushSmooth)) || any(isinf(pushSmooth)))
            pushSmooth = float3(0, 0, 0);
        // Seed instead of blend on the first run that proves an escape is over: a full-budget
        // retest of an INACTIVE (or escaping) probe that saw zero backface rays is decisive (a
        // truly buried probe does that with probability ~0.75^32). Its stored pushSmooth is a
        // stale escape vector up to one cell diagonal long that the 0.05 EMA would keep
        // integrating for ~30 runs: an outward hop, often to the +-0.45-cell clamp, right
        // after the obstacle leaves. Gated on bfPrev so an ACTIVE probe's filtering is never
        // touched.
        const bool escapeGone = (backfaceHits == 0u) && (bfPrev > 0.25);
        pushSmooth = (overwrite || escapeGone) ? push : lerp(pushSmooth, push, kPushBlend);
        pd.pushSmooth = pushSmooth;

        // ---- Dead-band: a settled probe must stop ----
        // Below it the probe is where it wants to be and the remaining signal is filter
        // residue; without a hard zero the EMA tail keeps it creeping (0.02 x a 1 mm residual
        // is still motion every frame, and the probe debug view shows it). 15% of
        // keepDistance is ~8x the residual standard deviation the kPushBlend filter leaves on
        // a settled probe: real motion crosses it, noise does not. Relative to keepDistance so
        // it scales like the push.
        const float kDeadBand = keepDistance * 0.15;
        const float pushLen   = length(pushSmooth);
        if (pushLen < kDeadBand)
        {
            // Frozen: pd.offset stays bit-for-bit identical, so the trace origin is stable and
            // the debug spheres stop moving.
            // Except at the end of a return-to-grid (C): once the offset itself is under the
            // dead-band and the way to the grid is clear, snap it to exactly zero; only the
            // authored grid restores the undistorted cell, and the residual would otherwise be
            // parked forever. The clearance test keeps the snap out of (A)'s shell, and the
            // filtered push must also point home (cos > 0.5): when a wall froze the probe,
            // pushSmooth is (A)'s push away from the grid, ~8 sigma from flipping on one lucky ray
            // set, so thin geometry the rays occasionally miss cannot make the snap and (A) trade
            // the probe back and forth.
            if (bfSmooth <= 0.25 && offLen > 0.0 && offLen < kDeadBand
                && clearTowardGrid - keepDistance > kDeadBand
                && dot(pushSmooth, pullDir) > 0.5 * pushLen)
            {
                pd.offset     = float3(0, 0, 0);
                pd.pushSmooth = float3(0, 0, 0);
            }
            g_ProbeData[probeIdx] = pd;
            return;
        }

        // ---- Integrate the filtered push ----
        // Incremental target: offset + push, not push alone. The rays were traced from the
        // current offset, so push is relative to where the probe already is; blending the
        // absolute offset toward it has its fixed point at offset == push(offset) (a probe
        // rising off a floor stalls at keepDistance/2, keeping close hits and
        // micro-oscillating). With offset + push the motion is monotone until the push falls
        // under the dead-band.
        // The per-frame step is capped so one outlier frame (a probe just buried by moving
        // geometry) cannot teleport it.
        float3 step = pushSmooth * 0.02;
        const float kMaxStep = minCell * 0.05;
        const float stepLen  = length(step);
        if (stepLen > kMaxStep) step *= (kMaxStep / stepLen);

        float3 newOffset = pd.offset + step;

        // On a pure return frame (no close frontface, not escaping) the only force is (C),
        // pointing at the grid point; if this step would carry the offset through it, land
        // exactly on the grid. The EMA + integrator is a second-order loop with ~1.7%
        // overshoot scaling with |offset|, and on the long axis of an anisotropic cell
        // (spacing up to 20x the smallest) the step cap saturates while pushSmooth keeps its
        // lagged magnitude: up to ~0.2 keepDistance past the grid, briefly inside whatever is
        // on the far side.
        if (closeHits == 0u && bfSmooth <= 0.25 && dot(newOffset, pd.offset) < 0.0)
        {
            newOffset     = float3(0, 0, 0);
            pd.pushSmooth = float3(0, 0, 0);
        }

        // Clamp to the authored cell: further would put the probe inside a neighbouring cell
        // and break the trilinear blend at sample time.
        const float3 limit = g_Vol.probeSpacing * 0.45;
        newOffset = clamp(newOffset, -limit, limit);

        pd.offset = newOffset;
    }
    else
    {
        // Relocation disabled -> return the probe to its authored grid position, also while
        // classification stays on; nothing else would ever clear the offset.
        pd.offset     = float3(0, 0, 0);
        pd.pushSmooth = float3(0, 0, 0);
    }

    g_ProbeData[probeIdx] = pd;
}
