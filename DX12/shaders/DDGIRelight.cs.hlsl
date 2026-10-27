// DDGIRelight.cs.hlsl - per-probe atlas update from this frame's ray data.
//
// Two define paths share this kernel:
//   DDGI_RELIGHT_TARGET_IRRADIANCE - write the irradiance atlas tile (6x6 + border).
//   DDGI_RELIGHT_TARGET_DEPTH       - write the depth atlas tile (16x16 + border).
//
// Irradiance (DDGICommon.hlsli explains the octahedral storage): each thread
// (lid.x, lid.y) of the 6x6 grid integrates the rays for its octahedral direction
// with a cosine weight, blends the result into that texel's own fp32 history
// (DDGIVarianceData, the convergent accumulator below) and writes the mean
// straight to the probe's atlas tile at tile + (1,1) + texel, the depth path's
// addressing. Sampling is a bilinear tap along the octahedral mapping of N
// (DDGI_SampleProbeIrradiance in DDGISampling.hlsli).
//
// Depth: per-texel cos^64-weighted integral of distance and distance^2, EMA blend
// with hysteresis, stored as (mean, variance).
//
// Ray staging: a ray's world direction (DDGI_AdaptiveSampleIdx ->
// DDGI_HaltonSphere -> DDGI_ApplyRandomRotation) depends only on (r,
// adaptiveRays, g_Vol), all uniform across the group (one probe per group), so
// the group stages the ray data and directions into groupshared once and every
// texel thread reads them. DDGI_HaltonSphere is expensive (DDGI_RadicalInverseBase3
// is an unrolled 20-iteration integer div/mod chain, plus a sincos); derived per
// thread it would be the largest term of the DDGI GPU cost.
//
// Depth path: the cos^64 lobe only counts a ray with dot(rayDir, texelDir) >=
// 1e-3^(1/64) ~ 0.8977 (a ~26 deg cone, ~5% of the sphere), so the raw dot is
// tested before the pow() and the transcendental is skipped for ~95% of the
// (texel, ray) pairs.

#include "DDGICommon.hlsli"

#ifndef DDGI_RELIGHT_TARGET_IRRADIANCE
#define DDGI_RELIGHT_TARGET_IRRADIANCE 1
#endif
#ifndef DDGI_RELIGHT_TARGET_DEPTH
#define DDGI_RELIGHT_TARGET_DEPTH 0
#endif

ConstantBuffer<DDGIVolumeGPU>      g_Vol      : register(b0, space0);
// g_RayData must live at t2, not t0: the DDGIPass root signature puts the TLAS
// at t0 (root SRV, slot 1) and the ray data SRV in the slot-4 descriptor table at
// t2. Reading t0 here would reinterpret the TLAS's driver data as floats and write
// ~1e30 garbage into probe 0 (magenta / saturated probes).
StructuredBuffer<float4>           g_RayData  : register(t2, space0);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
// R16G16B16A16_FLOAT. .rgb = linear irradiance (E/pi); .a is written 1.0 so a
// debug view of the raw atlas is opaque. DDGICommon.hlsli explains RGBA16F and
// the linear encoding.
RWTexture2D<float4>                  g_Atlas    : register(u0, space0);
RWStructuredBuffer<DDGIVarianceData> g_Variance : register(u1, space0);
static const uint kProbeSize   = DDGI_IRRADIANCE_PROBE_SIZE;
static const uint kProbeStride = DDGI_IRRADIANCE_PROBE_STRIDE;
#else
RWTexture2D<float2>                g_Atlas    : register(u0, space0);
static const uint kProbeSize   = DDGI_DEPTH_PROBE_SIZE;
static const uint kProbeStride = DDGI_DEPTH_PROBE_STRIDE;
#endif

// Adaptive per-probe ray count, written by DDGIPrepareRayCount.cs each frame.
RWStructuredBuffer<uint>           g_RayCount : register(u2, space0);

// Groupshared ray staging (file header). Sized for one full chunk; larger ray
// counts run in successive chunks, so any authored raysPerProbe is correct.
#define DDGI_RELIGHT_GROUP_THREADS 256
#define DDGI_RELIGHT_STAGE_RAYS    256
groupshared float4 s_rayData[DDGI_RELIGHT_STAGE_RAYS];
groupshared float3 s_rayDir [DDGI_RELIGHT_STAGE_RAYS];

[numthreads(16, 16, 1)]
void main(uint3 tid : SV_GroupID, uint3 lid : SV_GroupThreadID)
{
    // Dispatched by ExecuteIndirect over the compacted active-probe list: the group
    // index is a slot in that list, not the probe index, so every group has real work.
    //
    // The load is group-uniform (tid.x is SV_GroupID.x), which keeps BOTH early-outs
    // below uniform across the group. Do not make a return divergent: the groupshared
    // ray staging uses GroupMemoryBarrierWithGroupSync, and a divergent return would
    // strand the other threads on a barrier no one else reaches.
    const uint probeIdx = g_RayCount[DDGI_ActiveListSlot(g_Vol, tid.x)];
    if (probeIdx >= DDGI_ProbeCountOf(g_Vol))
        return;

    const uint2 inProbeTexel = lid.xy;
    const bool  validTexel   = (inProbeTexel.x < kProbeSize && inProbeTexel.y < kProbeSize);
    const uint  linIdx       = lid.y * 16u + lid.x;   // 0..255, group-linear

    // Adaptive ray count: this probe traced only `adaptiveRays` rays this frame.
    // The directions come from DDGI_AdaptiveSampleIdx(r, adaptiveRays), the same inputs
    // the trace CS uses, so adaptiveRays must be read exactly as there. Ray data is
    // laid out at the volume's MAX raysPerProbe stride (the buffer size).
    const uint adaptiveRays = g_RayCount[probeIdx] * DDGI_RAY_BUCKET_COUNT;
    const uint rayBase      = probeIdx * g_Vol.raysPerProbe;

    // Temporal amortization: PrepareRayCount zeroes g_RayCount for probes outside
    // this frame's interleave phase. Bail BEFORE writing anything: with zero rays every
    // texel would resolve sumWeight == 0 -> newSample == 0 and blend black into the
    // running mean, so amortized probes would fade out instead of holding their value.
    // This early-out is group-uniform (adaptiveRays derives from the group's
    // probeIdx), so it cannot strand a thread on the barriers below.
    if (adaptiveRays == 0u) return;

    // ---- Per-texel ray integration ----
    float3 texelDir = float3(0, 0, 1);
    if (validTexel)
        texelDir = DDGI_DirectionFromTexel(inProbeTexel, kProbeSize);

#if DDGI_RELIGHT_TARGET_IRRADIANCE
    float3 sumColor = 0;
#else
    float  sumDist  = 0;
    float  sumDist2 = 0;
#endif
    float  sumWeight = 0;

    // Chunked over the staged-ray capacity. With the documented ray counts
    // (64 / 128 / 256) this is a single chunk and the outer loop costs nothing.
    for (uint chunkBase = 0u; chunkBase < adaptiveRays; chunkBase += DDGI_RELIGHT_STAGE_RAYS)
    {
        const uint chunkCount = min((uint)DDGI_RELIGHT_STAGE_RAYS, adaptiveRays - chunkBase);

        // Cooperative stage: each ray's data + world direction computed ONCE
        // per group instead of once per thread. Barrier before the fill guards
        // the previous chunk's readers.
        GroupMemoryBarrierWithGroupSync();
        [loop] for (uint li = linIdx; li < chunkCount; li += DDGI_RELIGHT_GROUP_THREADS)
        {
            const uint r = chunkBase + li;
            s_rayData[li] = g_RayData[rayBase + r];
            // Direction reconstruction matches the trace CS exactly: same adaptive sample
            // index -> same Halton point -> same rotated rayDir.
            const uint sampleIdx = DDGI_AdaptiveSampleIdx(r, adaptiveRays, g_Vol);
            s_rayDir[li] = DDGI_ApplyRandomRotation(DDGI_HaltonSphere(sampleIdx), g_Vol);
        }
        GroupMemoryBarrierWithGroupSync();

        // Guarded rather than `if (!validTexel) continue;` so every thread reaches both
        // barriers through identical control flow (the trip count is uniform:
        // adaptiveRays and chunkCount are group-uniform).
        if (validTexel)
        [loop] for (uint c = 0u; c < chunkCount; ++c)
        {
            const float4 sample = s_rayData[c];
            const float3 rayDir = s_rayDir[c];

            float w = saturate(dot(rayDir, texelDir));
#if DDGI_RELIGHT_TARGET_DEPTH
            // Same ray set as rejecting pow(w, 64) < 1e-3: 1e-3^(1/64) = 0.897687. Testing
            // the raw dot first skips pow() for the ~95% of the sphere outside the cos^64
            // lobe.
            if (w < 0.897687) continue;
            // pow(w, 64) by repeated squaring: 6 multiplies instead of a log2/exp2
            // transcendental pair.
            w *= w; w *= w; w *= w; w *= w; w *= w; w *= w;
#else
            if (w < 1e-3) continue;
#endif

#if DDGI_RELIGHT_TARGET_IRRADIANCE
            // Backface rays carry no irradiance information: the trace writes radiance 0 as
            // a "this direction is buried" sentinel, not a measurement. Blending those zeros
            // in would pull the mean down: a probe ON a floor plane (a whole probe layer in
            // grid-aligned scenes) sees ~half its rays as backface and would converge to ~half
            // brightness. RTXGI excludes them for the same reason; the depth path still
            // records them as near occluders for Chebyshev.
            if (sample.w < -1.5) continue;
            sumWeight += w;
            sumColor += sample.rgb * w;
#else
            // Hit-distance encoding (set by DDGIRayTrace.cs):
            //   sample.w > 0       : frontface hit, this distance
            //   sample.w == -1.0   : miss
            //   sample.w < -1.5    : backface hit, real t = -(sample.w + 2.0)
            //
            // Misses are excluded from the depth integral. Recording a miss as a far distance
            // is an indoor/outdoor leak: a texel with close wall hits (d ~ 0.5) and far misses
            // averages to a mean past the shading point's distToProbe, so Chebyshev's
            // `distToProbe > mean` gate never fires. All-miss texels end with sumWeight ~ 0
            // (the resolve keeps the old texel or writes 0), and the sampler's `mean > 1e-3`
            // guard then skips Chebyshev: no occlusion info, no gating, right for open
            // directions.
            //
            // Backface hits record the wall distance: the probe is inside solid material in
            // that direction, so a shading point beyond the wall must be gated at that depth.
            //
            // Recorded distances are clamped to 1.5x the cell diagonal, above the largest
            // shading-point -> probe distance inside one trilinear cell (1.0x the diagonal), so
            // open directions always satisfy distToProbe < mean. Two reasons (both from the
            // RTXGI reference):
            //   1. The sampler never consults occlusion beyond one cell; raw far hits (TMax
            //      50) drag the mean toward "open" and dilute close occluders in the integral.
            //   2. Cell-scale distances keep d^2 small, where the R16G16_FLOAT atlas keeps
            //      real precision.
            const float kMaxRecordDist = length(g_Vol.probeSpacing) * 1.5;
            if (sample.w >= 0.0)
            {
                float d = min(sample.w, kMaxRecordDist);
                sumWeight += w;
                sumDist  += d * w;
                sumDist2 += d * d * w;
            }
            else if (sample.w < -1.5)
            {
                float d = min(-sample.w - 2.0, kMaxRecordDist);
                sumWeight += w;
                sumDist  += d * w;
                sumDist2 += d * d * w;
            }
            // else: miss - skip
#endif
        }
    }   // ---- end chunk loop ----

    // ---- Resolve: one write per valid texel, after all chunks ----
    if (validTexel)
    {
        const bool overwrite = (g_Vol.flags & 8u) != 0;

#if DDGI_RELIGHT_TARGET_IRRADIANCE
        float3 newSample = (sumWeight > 1e-4) ? (sumColor / sumWeight) : float3(0, 0, 0);
        const uint vidx = probeIdx * kProbeSize * kProbeSize
                        + inProbeTexel.y * kProbeSize + inProbeTexel.x;
        DDGIVarianceData vd = g_Variance[vidx];

        // ---- Convergent temporal accumulator ----
        // A fixed-rate EMA at blend b keeps a permanent noise floor of sigma x sqrt(b/2):
        // it filters the per-frame ray-rotation Monte-Carlo noise but never averages it
        // out, so a static scene shimmers. Turning the rotation jitter off is no fix: a
        // static ray set never samples between its fixed directions, and small bright
        // sources (emissive signs, sky slivers) go missing. The jitter keeps the energy
        // right; the estimator has to converge.
        //
        // Scheme: count-based progressive averaging with a CUSUM drift integrator:
        //   vd.shortMean - fast EMA of the (winsorized) sample, ~7-update window
        //   vd.variance  - same-window EW variance of the sample
        //   vd.vbbr      - a packed uint (asuint/asfloat, no float math): bits 0..15 N
        //                  (updates accumulated, clamped 1024), bits 16..23 the signed
        //                  drift bias quantised over [-3,3], bit 24 the initialised
        //                  sentinel (a zero-initialised buffer seeds cleanly).
        // While the scene is static the mean blends at 1/N, the arithmetic average of the
        // last N updates: noise decays as sigma/sqrt(N) instead of plateauing, and the
        // average converges to the jitter's full-sphere coverage.
        //
        // Change response is a continuous pressure, not a gate: a thresholded gate has a
        // deadband (a sustained change below it crawls at 1/1024 for tens of seconds) and
        // turns a continuous ramp (time-of-day sun, scripted fades) into a whole-volume
        // sawtooth. The CUSUM integrates the normalised divergence
        // z = (shortMean - mean)/D slowly into `bias`: zero-mean noise integrates to ~0, a
        // persistent shift of any size to its true z. Two continuous lanes read it:
        //   press = smoothstep(1.0, 2.0, |bias|)   - full authored response; also
        //           restarts the averaging (pulls N down);
        //   drift = smoothstep(0.4, 1.2, |bias|)^2 - raises the blend floor without
        //           crushing N; the square keeps noise tails cheap, while a persistent
        //           10-15% change still resolves near the authored rate.
        // So there is no deadband cliff; a ramp settles at a stable proportional-tracking
        // lag (speed x lag ~ drift rate; open-loop gain < 1, no limit cycle); at full
        // press the accumulator IS the authored EMA; and a single x50 firefly moves the
        // mean only ~6% for a few updates.
        //
        // Why the detector cannot become a flicker:
        //   1. It integrates shortMean, never the raw sample, normalised by shortMean's
        //      own standard error (sqrt(0.15/1.85) ~ 0.285 x dev). Autocorrelation through
        //      shortMean widens static |bias| tails to ~0.4-0.9, hence the press knee at
        //      1.0 (~4 sigma) and the squared drift lane (false-pressure duty <= 0.6%).
        //   2. Only the detector input is winsorized; the mean sees the raw sample, so a
        //      rare bright direction keeps exactly its time-average share of energy. On
        //      spike-lottery texels (rare huge emissive hits) bias saturates and the texel
        //      runs at press ~ 1, i.e. the authored EMA: no win, no regression.
        //   3. A false positive is cheap: pressure blends toward the current samples,
        //      which for a false alarm ARE the mean; only N drops for a while.
        //   4. It is feed-forward and per texel: it never changes how many rays are
        //      traced, so no feedback loop with the adaptive ray count can form.
        //
        // updateCompensation affects only the fast path: the convergent path counts
        // updates, so its noise floor is independent of probeUpdateDivisor - static scenes
        // get the ucomp=0 noise floor, lighting changes the ucomp=1 wall-clock response.
        const float kUpdateN   = pow(float(max(g_Vol.probeUpdateDivisor, 1u)),
                                     saturate(g_Vol.updateCompensation));
        const float blendSpeed = saturate(max(saturate(1.0 - g_Vol.hysteresis), 0.005)
                                          * kUpdateN);
        if (any(isnan(vd.mean)) || any(isinf(vd.mean))) vd.mean = float3(0, 0, 0);
        if (isnan(vd.inconsistency) || isinf(vd.inconsistency)) vd.inconsistency = 0.0;
        if (any(isnan(vd.shortMean)) || any(isinf(vd.shortMean))) vd.shortMean = vd.mean;
        if (any(isnan(vd.variance)) || any(isinf(vd.variance))) vd.variance = float3(0, 0, 0);
        // Reactivation seed: a negative inconsistency is the sentinel DDGIPrepareRayCount
        // writes on the FRESH handshake - the probe just returned from INACTIVE and its
        // held mean is stale (typically blackened by buried-view retests). The texel takes
        // a one-shot overwrite, so the first fresh trace replaces the history instead of
        // crawling toward it at blendSpeed for ~100 frames. Unreachable otherwise:
        // inconsistency is an EMA of saturate()d values, NaN-scrubbed above.
        const bool seedTexel      = vd.inconsistency < 0.0;
        const bool overwriteFrame = ((g_Vol.flags & 8u) != 0) || seedTexel;

        // Zero-coverage hold: backface rays are excluded, so a texel whose lobe caught no
        // usable ray this update carries no information and newSample would be a
        // fabricated black (a flicker source near walls at low ray counts). Skip the whole
        // update: no state advance, no atlas write (the atlas keeps its texel, and
        // BorderUpdate re-mirrors it). Overwrite frames still write: the atlas is undefined
        // then and must be initialised (a buried texel writes 0). Per-texel divergence is
        // safe here: no barriers below this point.
        if (overwriteFrame || sumWeight > 1e-4)
        {
        // Unpack the convergence state (vbbr layout above). max(, 1) guards a garbage
        // pattern with bit 24 set that decodes N = 0 -> speed 1/0 -> a NaN mean; no real
        // writer produces one (the buffer is zero-initialised and only this kernel writes
        // vbbr), and the guard costs one instruction.
        const uint  cw     = asuint(vd.vbbr);
        const bool  inited = (cw & 0x1000000u) != 0u;
        float N    = max(float(cw & 0xFFFFu), 1.0);
        float bias = (float((cw >> 16) & 0xFFu) - 128.0) * (3.0 / 127.0);

        // Inconsistency: how far this frame's sample diverged from the running mean -
        // DDGIPrepareRayCount's ray-budget signal (the probe's maximum drives its budget tier:
        // > 0.35 goes to full, < 0.10 after audits steps down). Computed BEFORE the mean
        // update on purpose.
        const float meanMag  = length(vd.mean);
        const float deltaMag = length(newSample - vd.mean);
        const float instInc  = saturate(deltaMag / max(meanMag, 0.01));
        vd.inconsistency = overwriteFrame ? instInc
                                          : lerp(vd.inconsistency, instInc, blendSpeed);

        // ---- Fast rate = authored hysteresis (+ changeResponseBoost) ----
        // changeResponseBoost: when a change is being responded to, respond this much
        // faster than the authored hysteresis. It scales only the fast path; the
        // convergent path ignores it.
        float fastRate = blendSpeed;
        {
            const float boost = g_Vol.changeResponseBoost;
            if (boost > 0.0)
            {
                // Per-channel MAX rather than luminance: a jump confined to
                // one channel (a coloured light switching on) barely moves a
                // luminance measure.
                const float3 dAbs = abs(newSample - vd.mean);
                const float  dMax = max(dAbs.r, max(dAbs.g, dAbs.b));
                const float  ref  = max(vd.mean.r, max(vd.mean.g, vd.mean.b));
                const float  rel  = dMax / max(ref, 1e-3);
                const float  t    = smoothstep(0.25, 1.00, rel);
                fastRate = saturate(lerp(blendSpeed, blendSpeed * (1.0 + boost), t));
            }
        }

        if (overwriteFrame || !inited)
        {
            // Seed the detector from this sample. The variance seeds at (0.3 x sample +
            // 0.05)^2, not 0, so ordinary MC jitter right after the seed does not read as an
            // infinite z (a change) while the EW variance learns the real sigma (~7 updates).
            vd.shortMean = newSample;
            const float3 seedDev = 0.3 * newSample + 0.05;
            vd.variance  = seedDev * seedDev;
            bias = 0.0;
            if (overwriteFrame)
            {
                // True overwrite (burn-in window / FRESH reactivation): the history is garbage or
                // stale - replace it. N restarts at 1, so the next updates blend at 1/2, 1/3, ...:
                // burn-in flows straight into progressive averaging.
                vd.mean = newSample;
                N = 1.0;
            }
            else
            {
                // Live volume, state never initialised (shader hot reload onto a running world):
                // the mean is valid converged history - keep it, adopt it at the authored rate and
                // let N grow from there. No pop.
                N = clamp(1.0 / fastRate, 1.0, 1024.0);
                vd.mean = lerp(vd.mean, newSample, 1.0 / N);
            }
        }
        else
        {
            // ---- Drift detector ----
            // Winsorize the DETECTOR input only (accumulator note above): clip upward spikes
            // against the short window's own spread so a lottery hit cannot pass for a
            // lighting change. A real brightening grows shortMean geometrically through the
            // cap (~x3.7 per update), so even a huge jump reaches its target in log time.
            float3 dev  = sqrt(vd.variance);
            const float3 yDet = min(newSample, vd.shortMean * 4.0 + dev * 8.0 + 0.1);
            const float kShort = 0.15;   // ~7-update window
            const float3 d1 = yDet - vd.shortMean;
            vd.shortMean = lerp(vd.shortMean, yDet, kShort);
            const float3 d2 = yDet - vd.shortMean;   // = (1-kShort) x d1, so d1 x d2 >= 0
            // Variance growth is clamped to x4 per update. Unclamped, a step change's own
            // transient (d1^2 ~ delta^2) inflates the detector's denominator in lockstep with
            // its numerator and masks the change for ~5 extra updates. x4 still learns a
            // genuinely noisier regime (a fire lighting up) in 2-3 updates; decay is uncapped.
            const float3 varTarget = max(lerp(vd.variance, d1 * d2, kShort), 0.0);
            vd.variance = min(varTarget, vd.variance * 4.0 + 1e-6);
            dev = sqrt(vd.variance);

            // z: divergence of the short average from the long mean, in units
            // of the short average's own standard error (0.285 =
            // sqrt(kShort/(2-kShort))), plus a 5%-of-mean relative term and an
            // absolute epsilon (div-by-zero guard that also lets pitch-black
            // texels see a light turning on). SIGNED, from the dominant
            // channel, so the integrator can tell a persistent shift from
            // noise; clamped so one winsorized outlier cannot swamp it.
            const float3 sd = vd.shortMean - vd.mean;
            const float3 ad = abs(sd);
            const float  sdom = (ad.r > ad.g) ? ((ad.r > ad.b) ? sd.r : sd.b)
                                              : ((ad.g > ad.b) ? sd.g : sd.b);
            const float devMax = max(dev.r, max(dev.g, dev.b));
            const float mMax   = max(vd.mean.r, max(vd.mean.g, vd.mean.b));
            const float D      = devMax * 0.285 + mMax * 0.05 + 1e-4;
            const float z      = clamp(sdom / D, -6.0, 6.0);

            // CUSUM integrator. Gain 0.02, not faster: z is autocorrelated through
            // shortMean's ~7-update memory, so the static bias distribution is ~2.7x wider
            // than the i.i.d. estimate; a 0.1 gain lets the noise tails hold ~8% pressure duty
            // and caps N near 150, forfeiting the convergence. 0.02 puts the tails ~4 sigma
            // under the press knee (duty <= 0.6%, N reaches 1024) at the cost of ~tens of
            // updates of detection latency for moderate changes, which the drift lane covers.
            // Time, not a threshold on z, separates a small real change from noise: the two
            // overlap instantaneously.
            bias = clamp(lerp(bias, z, 0.02), -3.0, 3.0);

            // Two continuous response lanes (smoothsteps, never a hard threshold: a step
            // function would flip the blend rate frame to frame at the boundary, a new
            // oscillator). The knees are explained in the accumulator note above.
            const float press = smoothstep(1.0, 2.0, abs(bias));
            const float driftT = smoothstep(0.4, 1.2, abs(bias));
            const float drift  = driftT * driftT;

            // Converged rate 1/N; pressure stops N's growth, pulls it toward the authored
            // regime and floors the speed proportionally. At full pressure N == 1/fastRate
            // makes the max() terms equal: exactly the authored EMA. The drift lane raises the
            // floor for sub-press persistent divergence without touching N, so a continuous
            // ramp settles at speed x lag ~ drift rate instead of sawtoothing. Accepted
            // trade-off: a change resolved by the drift lane alone blends at ~the authored
            // rate while N stays high, leaving that rate's noise (a few % of mean) in the
            // texel until 1/N averaging washes it out.
            const float Nfast = clamp(1.0 / fastRate, 1.0, 1024.0);
            N = min(N + 1.0 - press, 1024.0);
            N = lerp(N, min(N, Nfast), press);

            const float speed = max(max(1.0 / N, press * fastRate),
                                    drift * fastRate);
            vd.mean = lerp(vd.mean, newSample, speed);
        }

        // Pack the convergence state back (sentinel set). N rounds to nearest; bias uses
        // STOCHASTIC rounding: at gain 0.02 a deterministic round freezes the integrator
        // whenever the step 0.02 x |z - bias| is under half the 8-bit LSB (3/127), i.e. for
        // every persistent divergence below ~0.6 x D. A hash dither in [0,1) before the
        // floor makes the quantiser unbiased in expectation; its wander (~0.01-0.03 bias
        // units) is far under the 0.4 drift knee. (h >> 8) x 2^-24 is exact and < 1;
        // float(h) x 2^-32 would round the top 2^25 hash values up to exactly 1.0.
        uint h = (vidx * 0x9E3779B9u) ^ (g_Vol.frameIndex * 0x85EBCA77u);
        h = h * 747796405u + 2891336453u;
        h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
        h = (h >> 22u) ^ h;
        const float dither = float(h >> 8) * 5.9604644775e-8;   // [0,1) exact
        const uint Nq = (uint)(clamp(N, 1.0, 1024.0) + 0.5);
        const uint bq = (uint)clamp(bias * (127.0 / 3.0) + 128.0 + dither,
                                    0.0, 255.0);
        vd.vbbr = asfloat((Nq & 0xFFFFu) | (bq << 16) | 0x1000000u);

        g_Variance[vidx] = vd;

        // ---- Write this texel straight into the octahedral atlas ----
        // Same addressing as the depth path: tile origin + the (1,1) border offset + the
        // interior texel. DDGIBorderUpdate then mirrors the edge ring so the bilinear tap
        // at sample time is seam-free.
        //
        // Do not add a second EMA here: vd.mean is the temporal filter, and blending the
        // atlas on top would double-smooth (~3%/frame x ~3.5%/frame ~ 0.1% effective
        // change: probes look frozen).
        //
        // Sanitise at the WRITE site (DDGICommon.hlsli): one test per updated texel
        // instead of one per probe tap per shaded pixel, and every stored texel is finite
        // and non-negative.
        {
            const uint2 tileI = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol),
                                                    g_Vol, kProbeStride);
            g_Atlas[tileI + uint2(1, 1) + inProbeTexel] =
                float4(DDGI_SanitizeIrradiance(vd.mean), 1.0);
        }
        }   // ---- end zero-coverage hold ----
#else
        if (sumWeight > 1e-4)
        {
            // ---- Storage: (mean, VARIANCE), not (mean, E[d^2]) ----
            // The sampler cubes variance/(variance + chebDist^2) into the probe weight. From
            // raw moments in R16G16_FLOAT, variance = E[d^2] - E[d]^2 cancels catastrophically
            // (at mean ~ 5, d^2 ~ 25 has an fp16 ULP of 0.0156, so variances below ~0.02 are
            // pure quantization noise) and probes near the Chebyshev knife-edge pop in and out
            // of their cell's blend. The EMA stays in the raw-moment basis, but the
            // subtraction runs here in fp32 and the variance itself is stored, with fp16
            // relative precision (~0.1%).
            const float newMean  = sumDist  / sumWeight;
            const float newMean2 = sumDist2 / sumWeight;
            uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
            uint2 px   = tile + uint2(1, 1) + inProbeTexel;
            // Argument order: hysteresis weights the PREVIOUS value here, so the amortization
            // compensation is hysteresis^N (retain less per update when updates are N x
            // rarer), not blendSpeed x N. Same updateCompensation exponent trade as the
            // irradiance fast path: N^1 preserves the response, N^0 the noise floor.
            const float hyst = pow(saturate(g_Vol.hysteresis),
                                   pow(float(max(g_Vol.probeUpdateDivisor, 1u)),
                                       saturate(g_Vol.updateCompensation)));
            float2 outv;
            if (overwrite)
            {
                outv = float2(newMean, max(newMean2 - newMean * newMean, 0.0));
            }
            else
            {
                float2 prev = g_Atlas[px];   // (mean, variance)
                const float prevMean  = prev.x;
                const float prevMean2 = prev.y + prev.x * prev.x;
                const float m  = lerp(newMean,  prevMean,  hyst);
                const float m2 = lerp(newMean2, prevMean2, hyst);
                outv = float2(m, max(m2 - m * m, 0.0));
            }
            g_Atlas[px] = outv;
        }
        else if (overwrite)
        {
            uint2 tile = DDGI_ProbeAtlasTile(DDGI_ProbeCoord(probeIdx, g_Vol), g_Vol, kProbeStride);
            uint2 px   = tile + uint2(1, 1) + inProbeTexel;
            g_Atlas[px] = float2(0, 0);
        }
#endif
    }

}
