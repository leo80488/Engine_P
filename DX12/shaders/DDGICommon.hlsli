// DDGICommon.hlsli - shared types and helpers for the DDGI pipeline.
// DDGIVolumeGPU and DDGIProbeData mirror C++ DDGI::VolumeGPUDesc and DDGI::ProbeData
// (DDGIVolumeManager.h) byte for byte: change both sides together.

#ifndef DDGI_COMMON_HLSLI
#define DDGI_COMMON_HLSLI

// ==== Layout constants (mirrored as constexpr in DDGIVolumeManager.h) ====
#define DDGI_IRRADIANCE_PROBE_SIZE   6
#define DDGI_IRRADIANCE_PROBE_STRIDE (DDGI_IRRADIANCE_PROBE_SIZE + 2)  // +1 border each side
#define DDGI_DEPTH_PROBE_SIZE        16
#define DDGI_DEPTH_PROBE_STRIDE      (DDGI_DEPTH_PROBE_SIZE + 2)
#define DDGI_MAX_VOLUMES             8

// DDGI_MAX_VOLUMES must equal DDGI::kMaxVolumes and RHI::Slots::Gfx::DDGIMaxVolumes. C++
// static_asserts those two against each other; DDGIPass passes
// -D DDGI_MAX_VOLUMES_EXPECTED=<kMaxVolumes> to every DDGI compute shader so a drift here
// is a build error instead of volumes past the HLSL count silently going invisible.
// Keep the #ifdef: the other includers (Lighting.ps, the probe debug view), ShaderLab and
// offline dxc compile without the define.
#ifdef DDGI_MAX_VOLUMES_EXPECTED
    #if DDGI_MAX_VOLUMES != DDGI_MAX_VOLUMES_EXPECTED
        #error "DDGI_MAX_VOLUMES here disagrees with DDGI::kMaxVolumes in DDGIVolumeManager.h — update BOTH (plus RHI::Slots::Gfx::DDGIMaxVolumes)."
    #endif
#endif

// Ray allocation bucketing (as in WickedEngine): the ray-count buffer stores per-probe
// bucket counts; rays = buckets * DDGI_RAY_BUCKET_COUNT.
#define DDGI_RAY_BUCKET_COUNT 4

// ==== Trace dispatch geometry ====
// Shared by DDGIFinalizeIndirect (writes the indirect args) and DDGIRayTrace (decodes
// the flat ray index DTid.x + DTid.y * DDGI_MAX_GROUPS_X * DDGI_TRACE_GROUP_SIZE). Do not
// let them drift: a mismatch silently re-traces row 0's rays for every later row.
// The Y dimension exists because probes x raysPerProbe can exceed 65535 groups (32x32x32
// probes at 256 rays is 131072 groups of 64), and ExecuteIndirect does not validate its
// args. Do not clamp X instead: the dropped rays' probes keep a non-zero ray count, so
// DDGIRelight would integrate never-written ray data, which also bypasses the trace's
// firefly cap.
// Group size 64 fills a whole wave per group on wave64 GPUs.
#define DDGI_TRACE_GROUP_SIZE 64
#define DDGI_MAX_GROUPS_X     65535

// ==== Active-probe compaction ====
// Relight.Irr / Relight.Dep / Border run one thread group per probe. With
// probeUpdateDivisor = 4 a full Dispatch(probeCount) would be ~75% no-op groups, so
// DDGIRayAllocation also compacts the probes it gave rays to into a dense list,
// DDGIFinalizeIndirect turns its length into a second D3D12_DISPATCH_ARGUMENTS, and
// the three kernels run via ExecuteIndirect with probeIdx = <list>[SV_GroupID.x].
// The list lives inside the ray-count buffer (already bound at u2 in every kernel that
// needs it), so it costs no new resource, descriptor or root parameter:
//   [0 .. P-1]      per-probe bucket count
//   [P]             active probe count
//   [P+1 .. 2P]     compacted probe indices
// C++ (CreateAtlasesForVolume) sizes the buffer 2*P+1 uints. The DDGI_ActiveCountSlot /
// DDGI_ActiveListSlot accessors follow DDGIVolumeGPU, which they take.

// Probe state values. A zero-initialised ProbeData buffer means "every probe is
// active", so no init pass is needed; classification writes INACTIVE (1).
#define DDGI_PROBE_STATE_ACTIVE   0
#define DDGI_PROBE_STATE_INACTIVE 1

// ---- Packed probe-state word ----
// Classification state shares DDGIProbeData::state:
//   bits 0..1  - state enum (ACTIVE / INACTIVE, values above)
//   bit  2     - FRESH: reactivated on the last relocate run. PrepareRayCount grants a
//                full ray budget and marks the probe's variance texels for a one-shot
//                relight overwrite, seeding the history from the first fresh trace
//                instead of a stale (often black) mean that would crawl in over ~100
//                frames. Samplers skip FRESH probes for that one frame; Relocate clears
//                the bit on its next run.
//   bits 8..15 - temporally smoothed backface fraction, biased encoding:
//                0 = never measured (zero-initialised) -> seed from the next
//                measurement; 1..255 = round(bf * 254) + 1. Classification reads this
//                smoothed value: the ray set is re-rotated every frame, so the raw
//                fraction of a facade-straddling probe swings across the 25% / 10% band
//                and would toggle whole probes INACTIVE <-> ACTIVE.
// Read it only through these helpers: a raw `state == INACTIVE` test is wrong.
#define DDGI_PROBE_STATE_MASK      0x3u
#define DDGI_PROBE_STATE_FRESH_BIT 0x4u

uint DDGI_ProbeState(uint packed)      { return packed & DDGI_PROBE_STATE_MASK; }
bool DDGI_ProbeIsFresh(uint packed)    { return (packed & DDGI_PROBE_STATE_FRESH_BIT) != 0u; }
// Skip test for samplers: contribute only settled ACTIVE probes.
bool DDGI_ProbeSkipSampling(uint packed)
{
    return (packed & (DDGI_PROBE_STATE_MASK | DDGI_PROBE_STATE_FRESH_BIT))
           != DDGI_PROBE_STATE_ACTIVE;
}
// Smoothed backface fraction; hasBf = false means "never measured".
float DDGI_ProbeBfSmooth(uint packed, out bool hasBf)
{
    const uint enc = (packed >> 8) & 0xFFu;
    hasBf = (enc != 0u);
    return hasBf ? float(enc - 1u) / 254.0 : 0.0;
}
uint DDGI_PackProbeState(uint stateEnum, bool fresh, float bfSmooth)
{
    const uint enc = uint(saturate(bfSmooth) * 254.0 + 0.5) + 1u;
    return (stateEnum & DDGI_PROBE_STATE_MASK)
         | (fresh ? DDGI_PROBE_STATE_FRESH_BIT : 0u)
         | (enc << 8);
}

// ==== Volume GPU descriptor (mirrors C++ DDGI::VolumeGPUDesc, 224 B) ====
struct DDGIVolumeGPU
{
    // See DDGIVolumeComponent::perceptualBlendWeight: 1 = reference gamma-2 inter-probe
    // blend, 0 = linear weighted mean (no contrast darkening).
    float3 origin;        float perceptualBlendWeight;
    // See DDGIVolumeComponent::changeResponseBoost; 0 = plain EMA.
    float3 extent;        float changeResponseBoost;
    float3 probeSpacing;  float _padC;
    uint   probeCountsX, probeCountsY, probeCountsZ;
    uint   raysPerProbe;
    float  hysteresis;
    float  normalBias;
    float  viewBias;
    float  boundaryFadeRatio;
    uint   flags;            // bit 0 enabled, 1 relocation, 2 classification, 3 overwrite atlas, 5 adaptive ray budget

    // ---- Descriptor-block base heap indices ----
    // Each block is one static DDGI_MAX_VOLUMES-wide descriptor allocation that never moves;
    // a consumer opens element base + volumeSlot:
    //   Texture2D<float4> irr = ResourceDescriptorHeap[
    //       NonUniformResourceIndex(vol.irradianceBaseIdx + vi)];
    // NonUniformResourceIndex is required: `vi` is a per-pixel loop variable. The declared
    // type must match the SRV exactly; Resource Binding Tier 3 gives no validation message
    // on a mismatch, only UB:
    //   irradianceBaseIdx -> Texture2D<float4>               (R16G16B16A16_FLOAT)
    //   depthBaseIdx      -> Texture2D<float2>               (R16G16_FLOAT)
    //   probeDataBaseIdx  -> StructuredBuffer<DDGIProbeData> (stride 48)
    // Neither this header nor DDGISampling.hlsli dereferences them: Lighting.ps and the other
    // shading shaders open them, and DDGISampling.hlsli takes the resources as parameters so
    // the DDGI compute shaders share it with their own bindings. A root signature without
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED fails a heap read at PSO creation, not at compile.
    // The C++ offsets are static_asserted against RHI::Slots::BindlessIdx::kDDGIVolumeDesc_*.
    uint   irradianceBaseIdx;   // irradiance atlas block base
    uint   depthBaseIdx;     // depth atlas block base
    uint   probeDataBaseIdx; // probe-data block base

    float4 randomRotation0;  // float3x3 rows packed in xyz, w unused
    float4 randomRotation1;
    float4 randomRotation2;

    float3 diffuseTint;
    float  diffuseScale;

    // Trace-time lighting:
    //   lightCount - valid entries in the cluster GPULight buffer bound at t7; 0 -> no
    //                direct light from it (sky miss and multi-bounce still contribute).
    //   frameIndex - per-volume frame counter (multi-bounce branch + RNG seed).
    uint   lightCount;
    uint   frameIndex;
    // Temporal amortization: only probes whose interleave phase matches this frame are
    // traced / relit. 1 = every probe every frame.
    uint   probeUpdateDivisor;
    // Shading-side arbitration inputs, packed:
    //   bits  0..15 : priority
    //   bits 16..31 : cascadeLevel
    // Mirror: DDGI::VolumeGPUDesc::schedulingPacked.
    uint   schedulingPacked;

    // Directional sun - the single source of truth, mirrored from LightCB.lightDir /
    // lightColor (written by the light gather and the TOD day/night override). The trace
    // CS evaluates it per ray and skips directional entries in the cluster g_Lights pick,
    // like the deferred Lighting.ps, so DDGI's directional GI tracks direct lighting
    // (Time-of-Day included) whether or not the sun has a SunLightTag.
    float3 sunDirection; float _padSun0;   // from-light convention (== LightCB.lightDir)
    float3 sunColor;     float _padSun1;   // rgb radiance, already x intensity

    // Amortization blend-compensation exponent (see
    // DDGIVolumeComponent::updateCompensation): blendSpeed is multiplied by
    // probeUpdateDivisor^updateCompensation. 1 keeps wall-clock response (~N x noise
    // variance), 0 keeps the N = 1 noise floor (~N x slower response).
    float updateCompensation; uint worldLightGridIndex;
    // The sky's radiance scale on a probe ray's miss: LightCB iblStrength, as the path
    // tracer and the raster sky fallback apply it. Mirror: DDGI::VolumeGPUDesc::skyScale.
    float skyScale; float _padUC;
};

struct DDGIProbeData
{
    float3 offset;
    uint   state;
    float  skyVisibility;
    // Temporally low-passed relocation push (world units). Written only by
    // DDGIProbeRelocate.cs; see the jitter notes in that file's header.
    float3 pushSmooth;
    // The sky visibility's first moment: the mean over this probe's rays of
    // (miss ? rayDir : 0), EMA'd by DDGIProbeRelocate.cs like skyVisibility (its zeroth
    // moment). Together they are the L1 SH projection of "this direction reaches the
    // sky", which DDGI_SkyVisibility reconstructs per direction. Struct is 48 B, as the
    // C++ mirror DDGI::ProbeData.
    float3 skyVisDir;
    float  _padSkyDir;
};

// ==== Active-probe compaction accessors ====
// Layout and rationale: the Active-probe compaction section above.
uint DDGI_ProbeCountOf(DDGIVolumeGPU vol)
{
    return vol.probeCountsX * vol.probeCountsY * vol.probeCountsZ;
}
// Slot holding the number of probes actually scheduled this frame.
uint DDGI_ActiveCountSlot(DDGIVolumeGPU vol)
{
    return DDGI_ProbeCountOf(vol);
}
// Slot holding the i-th compacted probe index.
uint DDGI_ActiveListSlot(DDGIVolumeGPU vol, uint i)
{
    return DDGI_ProbeCountOf(vol) + 1u + i;
}

// ==== Probe coordinate <-> linear index <-> atlas position ====
uint DDGI_ProbeIndex(int3 coord, DDGIVolumeGPU vol)
{
    return (uint)coord.x
         + (uint)coord.y * vol.probeCountsX
         + (uint)coord.z * vol.probeCountsX * vol.probeCountsY;
}

int3 DDGI_ProbeCoord(uint idx, DDGIVolumeGPU vol)
{
    int z = (int)(idx / (vol.probeCountsX * vol.probeCountsY));
    int rem = (int)(idx % (vol.probeCountsX * vol.probeCountsY));
    int y = rem / (int)vol.probeCountsX;
    int x = rem % (int)vol.probeCountsX;
    return int3(x, y, z);
}

// World-space position of a probe (without relocation offset).
float3 DDGI_ProbeWorldPos(int3 coord, DDGIVolumeGPU vol)
{
    // Grid runs from origin - extent .. origin + extent. With probeCountsN
    // probes along an axis there are (probeCountsN - 1) cells; first probe
    // sits at the negative boundary.
    float3 t = float3(coord) / max(float3(vol.probeCountsX - 1,
                                          vol.probeCountsY - 1,
                                          vol.probeCountsZ - 1), 1.0);
    return vol.origin - vol.extent + t * (vol.extent * 2.0);
}

// Atlas tile origin (in texels) for the (probe, atlas-stride) tuple.
// Atlas layout: probeCountX x (probeCountY * probeCountZ) tiles.
uint2 DDGI_ProbeAtlasTile(int3 coord, DDGIVolumeGPU vol, uint stride)
{
    uint x = (uint)coord.x * stride;
    uint y = ((uint)coord.y + (uint)coord.z * vol.probeCountsY) * stride;
    return uint2(x, y);
}

// ==== Temporal amortization: does this probe update this frame? ====
// Splits the grid into probeUpdateDivisor spatially interleaved phases and updates one
// phase per frame. Trace, relight and relocate all gate on the same predicate (through
// the ray count, which PrepareRayCount zeroes for skipped probes), so a skipped probe
// costs nothing and keeps last frame's irradiance / depth / offset.
// Pattern (x + 2y + 3z) % N: in any 2x2x2 block the eight values are
// {0,1,2,3,3,4,5,6}, which mod 2, 3 and 4 cover every phase, so no coherent region is
// stale as a group and the trilinear sampler averages the lag out. A plain
// probeIdx % N would stripe whole rows: a visible scanline crawl when lighting changes.
bool DDGI_ProbeUpdatesThisFrame(int3 coord, DDGIVolumeGPU vol)
{
    const uint n = max(vol.probeUpdateDivisor, 1u);
    if (n <= 1u) return true;
    // Overwrite (freshly (re)allocated volume) and burn-in must touch every probe, or
    // N-1 of N probes keep garbage until their phase comes round.
    if ((vol.flags & 8u) != 0u || vol.frameIndex < 8u) return true;
    const uint phase = (uint)(coord.x + 2 * coord.y + 3 * coord.z) % n;
    return phase == (vol.frameIndex % n);
}

// ==== Octahedral mapping (Cigolle 2014) ====
// core/packing.hlsli's OctEncode / OctDecode: [-1,1]^2 <-> unit vector.
#include "core/packing.hlsli"

// Map a probe-local octahedral texel coordinate (in [0..probeSize-1]) to a unit
// direction. Borders excluded: the caller handles them.
float3 DDGI_DirectionFromTexel(uint2 texel, uint probeSize)
{
    // Map from integer texel center to [-1,1] octahedron.
    float2 uv = (float2(texel) + 0.5) / float(probeSize);
    return OctDecode(uv * 2.0 - 1.0);
}

// Relative solid angle of one octahedral texel. The parameterisation is not
// area-preserving: radially projecting the octahedron point p (|x|+|y|+|z| = 1) onto
// the sphere has Jacobian 1/|p|^3, so centre and corner texels differ by ~2x. Weight
// texels by this when projecting them onto a basis, or the projection is biased.
// Returns an unnormalised weight: the caller divides by its own sum and multiplies by
// 4*PI. Uses the same (texel + 0.5)/size * 2 - 1 convention as
// DDGI_DirectionFromTexel; change both together. No shader calls it at present.
float DDGI_OctTexelSolidAngleWeight(uint2 texel, uint probeSize)
{
    const float2 e = ((float2(texel) + 0.5) / float(probeSize)) * 2.0 - 1.0;
    // Point on the octahedron |x| + |y| + |z| = 1 for this square coordinate.
    float3 p = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    // Lower hemisphere fold, as in the standard oct decode.
    if (p.z < 0.0)
        p.xy = (1.0 - abs(float2(p.y, p.x))) * sign(p.xy);
    const float l = length(p);
    return 1.0 / max(l * l * l, 1e-6);
}

// ==== Volume -> grid-space mapping (DDGISampling.hlsli, at shading time) ====
float3 DDGI_GridSpace(float3 worldPos, DDGIVolumeGPU vol)
{
    float3 minCorner = vol.origin - vol.extent;
    return (worldPos - minCorner) / vol.probeSpacing;
}

// Volume membership / fade weight in [0..1]: 1 inside, falling off over
// boundaryFadeRatio near the AABB boundary.
float DDGI_VolumeFadeWeight(float3 worldPos, DDGIVolumeGPU vol)
{
    float3 d = abs(worldPos - vol.origin) / max(vol.extent, 1e-3);
    float maxd = max(d.x, max(d.y, d.z));
    if (maxd >= 1.0) return 0.0;
    float fadeStart = 1.0 - vol.boundaryFadeRatio;
    return saturate(1.0 - max(maxd - fadeStart, 0.0) / max(vol.boundaryFadeRatio, 1e-4));
}

// Apply the volume's per-frame random rotation to a unit vector (ray generation), so
// ray orientations spread uniformly over frames.
float3 DDGI_ApplyRandomRotation(float3 dir, DDGIVolumeGPU vol)
{
    return float3(
        dot(vol.randomRotation0.xyz, dir),
        dot(vol.randomRotation1.xyz, dir),
        dot(vol.randomRotation2.xyz, dir));
}

// ==== Halton(2, 3) on the unit sphere: ray directions ====
// Adaptive dispatch traces only a contiguous block of indices, so the sequence must be
// uniform on any prefix. Halton is by construction (each sample fills the largest
// gap); a latitude-sorted sequence such as spherical Fibonacci makes every prefix a
// polar cap and biases the probe to one hemisphere. The direction at index i is also
// independent of how many rays a probe traces this frame.
#include "core/math.hlsli"   // PI

// ==== Firefly cap relative to the scene's light level ====
// The DDGIRayTrace caps (ray radiance, multi-bounce feed-in, sky miss) clamp values in
// radiance / (E/pi) units, so a fixed constant bites on ordinary surfaces once the sun
// is bright. Reference: a white, fully lit Lambertian surface under the sun returns
// L = a*E/pi with a = 1, N.L = 1 -> sunColor/pi. The steady state for albedo a in a
// fully enclosing environment is a/(1-a) x that reference (the fixed point of the
// bounce loop): a = 0.5 -> 1.0x, a = 0.7 -> 2.3x, a = 0.8 -> 4.0x. kRel = 4 therefore
// clears every albedo up to ~0.8 fully enclosed while still cutting the light-pick and
// emissive-mip spikes the caps exist for. Example, sun intensity 25: reference 7.96,
// ray cap max(5.0, 31.8) = 31.8; a fixed 5.0 would lose 37% at mid-grey albedo.
// absoluteFloor is a floor, never a ceiling: this only relaxes a cap, so a scene with a
// dim or absent sun (sunColor ~ 0) keeps the absolute cap.
// sunColor is a parameter because Lighting.ps includes this header (via DDGISampling)
// without declaring the volume CB, so g_Vol does not exist there.
float3 DDGI_FireflyCap(float absoluteFloor, float3 sunColor)
{
    const float kRel = 4.0;
    const float sunLevel = max(sunColor.r, max(sunColor.g, sunColor.b));
    const float cap = max(absoluteFloor, kRel * sunLevel * (1.0 / PI));
    return float3(cap, cap, cap);
}

float DDGI_RadicalInverseBase2(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10; // / 2^32
}

float DDGI_RadicalInverseBase3(uint i)
{
    float r = 0.0;
    float f = 1.0 / 3.0;
    // 20 base-3 digits cover i up to 3^20 ~ 3.5 billion, far past any probe ray count;
    // the bounded loop lets HLSL unroll it.
    [unroll(20)]
    for (uint k = 0u; k < 20u; ++k)
    {
        if (i == 0u) break;
        r += f * float(i % 3u);
        i /= 3u;
        f *= 1.0 / 3.0;
    }
    return r;
}

float3 DDGI_HaltonSphere(uint i)
{
    // i+1 skips the exact pole (cosTh = 1 makes phi unobservable) without changing the
    // distribution.
    const uint  k     = i + 1u;
    const float u1    = DDGI_RadicalInverseBase2(k);
    const float u2    = DDGI_RadicalInverseBase3(k);
    const float cosTh = 1.0 - 2.0 * u1;                  // uniform [-1, 1]
    const float sinTh = sqrt(saturate(1.0 - cosTh * cosTh));
    const float phi   = 2.0 * PI * u2;
    return float3(cos(phi) * sinTh, sin(phi) * sinTh, cosTh);
}

// Per-frame adaptive sample index. Each frame samples a contiguous block of
// adaptiveRays Halton indices and advances by adaptiveRays, so over
// raysPerProbe / adaptiveRays frames every index is sampled exactly once.
// Do not advance by 1 to "smooth" the jump: the block jump has a short period (e.g. 4
// frames), high frequency the relight EMA filters (gain ~0.07); a slow sweep (~64
// frames) is low frequency the EMA passes (gain ~0.7) and the whole scene breathes.
// Do not sample with a stride: stride 4 selects odd k only, whose LSB becomes the MSB
// after the bit reverse, so u1 >= 0.5 and cosTh <= 0 - lower hemisphere only. A
// contiguous range of size 2^k still tiles [0,1] uniformly in base 2.
uint DDGI_AdaptiveSampleIdx(uint rayIdx, uint adaptiveRays, DDGIVolumeGPU vol)
{
    const uint adaptive   = max(adaptiveRays, 1u);
    const uint baseOffset = (vol.frameIndex * adaptive) % vol.raysPerProbe;
    return (rayIdx + baseOffset) % vol.raysPerProbe;
}

// ==== Per-texel temporal state for DDGIRelight's convergent accumulator ====
//   mean          fp32 irradiance history - the temporal filter; the atlas is
//                 rewritten from it every update.
//   shortMean     fast (~7-update) EMA of the winsorized sample - change-detector
//                 input.
//   variance      same-window EW variance of the sample - scales the change
//                 detector's threshold.
//   inconsistency EMA of saturate(|sample - mean| / |mean|); the adaptive ray budget in
//                 DDGIPrepareRayCount takes its max over a probe's texels (> 0.35 ->
//                 full budget, < 0.10 -> may step down). Negative = FRESH-reactivation
//                 sentinel (one-shot overwrite, written by PrepareRayCount).
//   vbbr          not a float - a uint via asuint/asfloat: bits 0..15 update count N,
//                 bits 16..23 signed CUSUM drift bias quantised over [-3,3]
//                 (128 = zero), bit 24 initialised sentinel. Never do float math on it.
//   _pad          texel 0 of each probe only: DDGIPrepareRayCount's packed
//                 adaptive-budget tier state; Relight round-trips it untouched.
// One entry per (probe, irradiance-tile texel), 48 B. Keep the layout: the buffer
// survives a shader hot-reload. Must be zero-initialised: zero means "uninitialised"
// for both packed words and seeds every path cleanly.
struct DDGIVarianceData
{
    float3 mean;          float vbbr;
    float3 shortMean;     float inconsistency;
    float3 variance;      float _pad;
};

// ==== Per-probe irradiance storage: octahedral atlas ====
// DDGI_IRRADIANCE_PROBE_SIZE^2 (36) octahedral texels per probe plus a 1-texel mirrored
// border: 36 independent directional samples. Do not replace it with L1 SH: 4
// coefficients per channel lose directionality and reconstruct negative values that
// need a clamp.
// Format R16G16B16A16_FLOAT, LINEAR irradiance (not sqrt):
//   * RGBA16F because DDGIBorderUpdate does a typed UAV load (g_Atlas[dst] =
//     g_Atlas[src]); R11G11B10_FLOAT would need TypedUAVLoadAdditionalFormats, while
//     RGBA16F typed UAV load and store is guaranteed on FL11.0. Revisit only if VRAM
//     binds (R11G11B10 halves it).
//   * Linear because DDGISampling keeps both a linear and a sqrt-space accumulator so
//     vol.perceptualBlendWeight can dial between them; sqrt storage would make the
//     linear one a lie and force the power-mean darkening that knob exists to escape.
// The atlas is pure output: the history lives in DDGIVarianceData.mean (fp32) and the
// atlas is rewritten from it each update, so its quantisation never accumulates.

// Magnitude convention: atlas texels hold E(D)/pi, the cos-weighted radiance average
// DDGIRelight computes per texel direction D. A bilinear tap at N returns ~E(N)/pi,
// the same convention as sky_sh.hlsli's EvalSH2 and a prefiltered irradiance cube, so
// Lighting.ps does albedo * iblDiffuse with no extra /pi and the ddgiDiffuseScale
// default 1.0 is physical (the direct path is a kD*albedo/pi Lambertian). The
// multi-bounce consumer in DDGIRayTrace converts a sample to irradiance with x PI.
// NaN/Inf gate, applied at the write site: max(x, 0.0) does not strip NaN, and one
// corrupt texel re-entering the multi-bounce loop poisons probes every frame.
// DDGIRelight calls this before every atlas write, which costs one test per updated
// texel instead of one per probe tap per shaded pixel. Finite, non-negative texels
// bilinearly interpolate to finite, non-negative values, so readers need no guard and
// no negative-lobe clamp.
float3 DDGI_SanitizeIrradiance(float3 v)
{
    if (any(isnan(v)) || any(isinf(v))) return float3(0, 0, 0);
    return max(v, 0.0);
}

#endif // DDGI_COMMON_HLSLI
