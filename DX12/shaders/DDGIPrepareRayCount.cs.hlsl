// DDGIPrepareRayCount.cs.hlsl - per-probe ray budget for this frame.
// Writes g_RayCount[probeIdx] = bucket count in [minBuckets..maxBuckets] (rays =
// buckets * DDGI_RAY_BUCKET_COUNT); zero skips the probe in every later DDGI pass. By
// default a probe that updates this frame gets the full budget; the opt-in adaptive
// budget (flags bit 5) also reads last frame's variance buffer.
// Also resets the counters DDGIRayAllocation accumulates into: g_RayAlloc[0] (total
// rays) and the active-probe count.

#include "DDGICommon.hlsli"

ConstantBuffer<DDGIVolumeGPU>          g_Vol      : register(b0, space0);
RWStructuredBuffer<DDGIVarianceData>   g_Variance : register(u1, space0);
RWStructuredBuffer<uint>               g_RayCount : register(u2, space0);
RWStructuredBuffer<uint>               g_RayAlloc : register(u3, space0);
// Read-only view of the classification written by DDGIProbeRelocate: the t6 slot
// (root param 10) the trace CS uses for its multi-bounce probe-data read. The buffer
// is in kSHReadState all frame except the UAV window around the relocate dispatch,
// which runs after this pass.
StructuredBuffer<DDGIProbeData>        g_ProbeDataRO : register(t6, space0);

// Retest period for INACTIVE probes, in frames. Their budget is zero, which also stops
// DDGIProbeRelocate (it early-outs on zero rays), so without a periodic full-budget
// frame a probe freed by an opened door or a deleted wall would stay dead for the
// session. 16 retests ~1/16 of the inactive set per frame, each probe at a hashed
// phase.
#define DDGI_INACTIVE_RETEST_PERIOD 16u

[numthreads(64, 1, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint probeCount = g_Vol.probeCountsX * g_Vol.probeCountsY * g_Vol.probeCountsZ;
    const uint probeIdx = DTid.x;

    // The first thread resets the counters DDGIRayAllocation accumulates into: the total
    // ray count and the compacted active-probe list length. Safe because a UAV barrier
    // separates this kernel from DDGIRayAllocation, the only writer of either.
    if (probeIdx == 0)
    {
        g_RayAlloc[0] = 0;
        g_RayCount[DDGI_ActiveCountSlot(g_Vol)] = 0u;
    }

    if (probeIdx >= probeCount) return;

    const uint maxBuckets = max(g_Vol.raysPerProbe / DDGI_RAY_BUCKET_COUNT, 1u);
    // Default: minBuckets == maxBuckets, a constant full budget. A fast variance-driven
    // budget forms a ray count <-> inconsistency loop (few rays -> biased sample -> high
    // inconsistency -> more rays -> unbiased -> few rays) with a period of tens of frames:
    // a low-frequency oscillation the relight EMA passes, seen as whole-scene breathing.
    // A constant budget removes the loop and keeps a moderate hysteresis stable, at ~2x
    // the trace work of an adaptive average.
    // ---- Adaptive ray budget, opt-in (flags bit 5) ----
    // Off by default; the default path is bit-identical to the constant budget. Its
    // failure mode is a low-frequency whole-scene oscillation a short test will not
    // reveal: turn it on with a profiler open. It keeps the budget slow, below the relight
    // EMA's bandwidth:
    //   * Discrete tiers (full / half / quarter), never a continuous ramp, so the budget
    //     cannot chase noise.
    //   * Downgrade one tier at a time, after 30 consecutive stable post-audit updates.
    //     Upgrade is immediate: wrong-bright costs rays, wrong-dark costs image.
    //   * A full-budget audit every 16 updates, and downgrades are judged only on the
    //     update after one. This breaks the loop: inconsistency describes the estimate
    //     from the previous update's ray count, so judging a reduced-budget probe by its
    //     own variance is the circular signal. Post-audit judging makes the control
    //     input independent of the control output.
    const bool adaptiveBudget = (g_Vol.flags & 32u) != 0u;
    const uint minBuckets = adaptiveBudget ? max(maxBuckets / 4u, 1u) : maxBuckets;

    // Overrides, ahead of every other gate:
    //  * slot inactive (flags bit 0 clear): zero rays (Tick zeroes the desc too; defensive).
    //  * burn-in (frameIndex < 8) or overwrite (flags bit 3): full budget for every probe.
    if ((g_Vol.flags & 1u) == 0u) { g_RayCount[probeIdx] = 0u; return; }
    if (g_Vol.frameIndex < 8u || (g_Vol.flags & 8u) != 0u)
    {
        g_RayCount[probeIdx] = maxBuckets;
        return;
    }

    // FRESH handshake (see DDGICommon.hlsli): relocate reactivated this probe last frame.
    // Grant a full budget regardless of the interleave phase and mark every irradiance
    // texel for a one-shot relight overwrite, so its first sampled frame carries a fresh
    // estimate instead of the stale, blackened mean it held while INACTIVE. The sentinel
    // is a negative inconsistency, unreachable on the normal path (an EMA of saturated
    // values, NaN-scrubbed on read); relight consumes and overwrites it this same frame.
    // Then the classification gate: an INACTIVE probe (buried in geometry) is skipped by
    // DDGISampling, so it gets no rays outside its retests. The gate requires
    // classification enabled (flags bit 2): with it off, relocate rewrites every probe to
    // ACTIVE, which it can only do on a frame with rays, so ignoring the flag would freeze
    // INACTIVE probes for good. Both run after burn-in / overwrite: a warming-up volume has
    // no classification yet, and the zero state is ACTIVE.
    const uint packedState = g_ProbeDataRO[probeIdx].state;
    if (DDGI_ProbeIsFresh(packedState))
    {
        const uint kSeedTexels = DDGI_IRRADIANCE_PROBE_SIZE * DDGI_IRRADIANCE_PROBE_SIZE;
        const uint vSeedBase   = probeIdx * kSeedTexels;
        [loop] for (uint t = 0; t < kSeedTexels; ++t)
            g_Variance[vSeedBase + t].inconsistency = -1.0;
        g_RayCount[probeIdx] = maxBuckets;
        return;
    }

    if ((g_Vol.flags & 4u) != 0u &&
        DDGI_ProbeState(packedState) == DDGI_PROBE_STATE_INACTIVE)
    {
        // Per-probe phase so retests spread over the period instead of spiking on one frame.
        // Knuth multiplicative hash (probeIdx % N would retest whole rows together); >> 16
        // rather than >> 28 keeps the modulo well distributed for periods above 16.
        const uint phase = ((probeIdx * 2654435761u) >> 16) % DDGI_INACTIVE_RETEST_PERIOD;
        if ((g_Vol.frameIndex % DDGI_INACTIVE_RETEST_PERIOD) != phase)
        {
            g_RayCount[probeIdx] = 0u;
            return;
        }
        // Retest frame: full budget, bypassing the update divisor below. The divisor would
        // multiply the retest period by N, and relocate's verdict is a ratio over the rays it
        // got (backfaceHits / adaptiveRays), so a full set is the least biased way to ask "is
        // this probe still buried?".
        g_RayCount[probeIdx] = maxBuckets;
        return;
    }

    // Temporal amortization. Probes outside this frame's interleave phase get zero rays,
    // the single gate the whole pipeline keys off:
    //   RayAllocation  - claims no slice, so the trace never sees the probe
    //   Trace          - total rays (and the indirect dispatch size) shrink by the divisor
    //   Relocate       - early-outs on adaptiveRays == 0
    //   Relight        - must early-out too, or sumWeight == 0 blends a black sample into
    //                    the running mean (see DDGIRelight.cs)
    // After the burn-in / overwrite branches, so a warming-up or resized volume still
    // converges at full rate.
    if (!DDGI_ProbeUpdatesThisFrame(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol))
    {
        g_RayCount[probeIdx] = 0u;
        return;
    }

    // With adaptivity off (minBuckets == maxBuckets) the variance scan below cannot change
    // the result, and on a 32x16x32 grid it reads 16384 probes x 36 texels x 48 B = 28 MB
    // per frame: bail before the read.
    if (minBuckets >= maxBuckets) { g_RayCount[probeIdx] = maxBuckets; return; }

    const uint kProbeSize = DDGI_IRRADIANCE_PROBE_SIZE;
    const uint vBase      = probeIdx * kProbeSize * kProbeSize;
    float maxInconsistency = 0.0;
    [loop] for (uint texel = 0; texel < kProbeSize * kProbeSize; ++texel)
    {
        DDGIVarianceData vd = g_Variance[vBase + texel];
        // NaN-safe: treat a corrupt texel as fully inconsistent so the probe
        // gets a full budget that resets it.
        float inc = (isnan(vd.inconsistency) || isinf(vd.inconsistency))
                        ? 1.0 : vd.inconsistency;
        maxInconsistency = max(maxInconsistency, inc);
    }

    // ---- Persistent per-probe budget state ----
    // Packed into _pad of this probe's first variance texel: that buffer is already bound
    // and per-probe indexable, so no new resource, descriptor or root parameter.
    //   bits 0..1   tier          0 = quarter, 1 = half, 2 = full
    //   bits 2..9   stableCount   consecutive quiet post-audit evaluations
    //   bits 10..14 sinceAudit    updates since the last full-budget audit
    //   bit  15     prevWasAudit  the previous update ran at full budget
    //   bit  16     always set, so a written state never reads as uninitialised (0)
    const uint kStateSlot = vBase;   // texel 0 of this probe
    uint st = asuint(g_Variance[kStateSlot]._pad);
    // A never-initialised slot reads zero; tier 0 would start every probe at the minimum
    // budget, the one start that cannot recover cleanly. Seed to full.
    uint tier         = st & 3u;
    uint stableCount  = (st >> 2) & 0xFFu;
    uint sinceAudit   = (st >> 10) & 0x1Fu;
    bool prevWasAudit = ((st >> 15) & 1u) != 0u;
    if (st == 0u) { tier = 2u; stableCount = 0u; sinceAudit = 0u; prevWasAudit = false; }

    // Upgrade is immediate and unconditional, without waiting for an audit: a probe whose
    // lighting just changed must not spend 16 updates at a reduced budget.
    if (maxInconsistency > 0.35)
    {
        tier = 2u;
        stableCount = 0u;
    }
    else if (prevWasAudit)
    {
        // ONLY here is the input unbiased: this inconsistency was produced by a
        // full-budget trace. Everything else holds the tier.
        if (maxInconsistency < 0.10)
        {
            stableCount = min(stableCount + 1u, 0xFFu);
            if (stableCount >= 30u && tier > 0u) { tier -= 1u; stableCount = 0u; }
        }
        else
        {
            stableCount = 0u;
        }
    }

    // Schedule the next audit. sinceAudit counts updates, not frames, so neither
    // probeUpdateDivisor nor the camera-relevance tiering (both change how often a probe
    // updates) stretches the audit period.
    const bool auditNow = (sinceAudit >= 16u);
    sinceAudit = auditNow ? 0u : (sinceAudit + 1u);

    uint buckets;
    if (auditNow || tier >= 2u)      buckets = maxBuckets;
    else if (tier == 1u)             buckets = max(maxBuckets / 2u, 1u);
    else                             buckets = minBuckets;

    st = (tier & 3u)
       | ((stableCount & 0xFFu) << 2)
       | ((sinceAudit & 0x1Fu) << 10)
       | ((auditNow ? 1u : 0u) << 15);
    // Guard the sentinel: st == 0 means "uninitialised" above, and tier 0 with
    // every counter at zero would collide with it. Bit 16 is otherwise unused.
    g_Variance[kStateSlot]._pad = asfloat(st | 0x10000u);

    g_RayCount[probeIdx] = clamp(buckets, minBuckets, maxBuckets);
}
