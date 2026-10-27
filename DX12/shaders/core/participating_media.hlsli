// core/participating_media.hlsli - the engine's fog as a participating medium, for the path
// tracers and the ray-traced froxel lighting: extinction, optical depth, free-flight
// sampling, transmittance, the Henyey-Greenstein phase function and equi-angular sampling.
// The rules every core header follows are at the top of core/gbuffer_codec.hlsli.
//
// Three kinds of source, summed:
//   * the global medium (VolumetricFogComponent, or a scene's legacy fog):
//     sigma_t(y) = density x exp(-falloff x max(y - baseHeight, 0)), FroxelCommon.hlsli's
//     GlobalFogExtinction;
//   * the height fog (HeightFogComponent): sigma_t(y) = density x exp(-falloff x (y - height)),
//     height_fog.hlsli's model;
//   * bounded local volumes (LocalFogVolumeComponent), LocalFogVolumes.hlsli's LocalFogWeight.
// Units are metres, Y up. sigma_t is grey; sigma_s = sigma_t x albedo per channel. Emission is
// the froxel fog's convention: a source term Le x sigma_t, so a thick medium of albedo w glows
// at Le / (1 - w).
//
// Phase convention: both directions point the way the path travels (camera -> scene). A path
// arriving along d that leaves along, or samples a light at, l scatters through
// cosTheta = dot(d, l); g > 0 favours going straight on. FroxelCommon.hlsli's
// PhaseHG(dot(V, toLight), g) is the same convention.
//
// The local volumes live in a buffer this file cannot read: it declares
//   MediumLocal MediumLoadLocal(uint index)
// and an includer that calls anything reading the local volumes defines it after including
// this file; the set carries the count (MediumSet.localCount).
// The includer also provides PI (core/math.hlsli) and OrthonormalBasis (core/sampling.hlsli).

#ifndef CORE_PARTICIPATING_MEDIA_HLSLI
#define CORE_PARTICIPATING_MEDIA_HLSLI

#ifndef CORE_MATH_HLSLI
#error "core/participating_media.hlsli needs core/math.hlsli - include it first"
#endif
#ifndef CORE_SAMPLING_HLSLI
#error "core/participating_media.hlsli needs core/sampling.hlsli - include it first"
#endif

// ==== The medium set ====
// Mirror: Graphics/MediumSet.h. Six float4 each, so the set and the local volumes after it
// share one StructuredBuffer<float4>.
// The global medium is infinite unless it has a reach: a sphere about the camera past which it
// is gone (pass.pathTrace.mediaMaxDistance). Its centre is the set's camera, which the loader
// copies into reachCenter (not one of the global medium's rows).
struct MediumGlobal
{
    float3 albedo;     float density;      // sigma_t at and below baseHeight (1/m); 0 = none
    float3 emission;   float falloff;      // emission premultiplied by its intensity; 1/m
    float  baseHeight;                     // world Y (m)
    float  anisotropy;                     // HG g
    float  reach;                          // m from reachCenter; 0 = everywhere
    float  _pad;
    float3 reachCenter;                    // world (m): MediumSet.camera
};

struct MediumHeight
{
    float3 albedo;     float density;      // sigma_t at y == height (1/m); 0 = none
    float  falloff;                        // 1/m
    float  height;                         // world Y (m)
    float  anisotropy;                     // HG g
    float  _pad;
};

struct MediumSet
{
    MediumGlobal global;
    MediumHeight height;
    uint   localCount;
    float3 camera;                         // the global medium's reach is measured from here
};

// One local volume, field for field LocalFogVolumes.hlsli's LocalFogVolumeGPU (96 B).
struct MediumLocal
{
    float4 row0;                           // world -> unit-volume space:
    float4 row1;                           //   local.i = dot(float4(p, 1), row_i)
    float4 row2;
    float3 albedo;     float density;      // sigma_t at the core (1/m)
    float3 emission;   float edgeFade;     // emission premultiplied by its intensity
    uint   shape;      float heightFalloff;  float halfHeight;  float _pad;
};

// The index-th local volume of the set being evaluated: defined by the includer.
MediumLocal MediumLoadLocal(uint index);

static const uint kMediumLocalBox    = 0u;
static const uint kMediumLocalSphere = 1u;

// A collision's source: the global medium, the height fog, or local volume (source - 2).
static const uint kMediumSourceGlobal = 0u;
static const uint kMediumSourceHeight = 1u;
static const uint kMediumSourceLocal0 = 2u;
static const uint kMediumSourceNone   = 0xFFFFFFFFu;

// A segment end at or past kMediumFar runs to infinity (a ray to the sky). An optical depth
// that diverges is reported as kMediumTauInfinite, which exp() turns into an exact 0.
static const float kMediumFar         = 1e20;
static const float kMediumTauInfinite = 1e30;

// Cap on the steps of one delta- or ratio-tracking walk: a local volume's majorant is its own
// core density, so a walk takes about density x length steps.
static const uint kMediumMaxTrackingSteps = 256u;

// Midpoint samples per local volume in the deterministic transmittance.
static const uint kMediumMarchSteps = 8u;

// ==== Random numbers ====
// The trackers draw a variable count of numbers, so they run on a stream of their own: seed it
// with one draw from the caller's stream and the caller's dimension count stays fixed.
uint MediumRngNext(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (w >> 22u) ^ w;
}

float MediumRandom(inout uint state)
{
    return float(MediumRngNext(state) >> 8u) * (1.0 / 16777216.0);
}

uint MediumRngSeed(uint seed)
{
    uint state = seed ^ 0x9E3779B9u;
    MediumRngNext(state);
    return state;
}

// ==== Extinction at a point ====
// By height alone, inside the global medium's reach: MediumGlobalSigmaTAt tests the reach.
float MediumGlobalSigmaT(MediumGlobal m, float y)
{
    return m.density * exp(-m.falloff * max(y - m.baseHeight, 0.0));
}

float MediumGlobalSigmaTAt(MediumGlobal m, float3 p)
{
    const float3 r = p - m.reachCenter;
    if (m.reach > 0.0 && dot(r, r) > m.reach * m.reach)
        return 0.0;
    return MediumGlobalSigmaT(m, p.y);
}

// The part of o + t d, t in [t0, t1], inside the global medium's reach. False when none of it
// is; always true without a reach.
bool MediumGlobalReach(MediumGlobal m, float3 o, float3 d, inout float t0, inout float t1)
{
    if (!(m.reach > 0.0))
        return true;
    const float3 oc   = o - m.reachCenter;
    const float  qa   = dot(d, d);
    const float  qb   = dot(oc, d);
    const float  disc = qb * qb - qa * (dot(oc, oc) - m.reach * m.reach);
    if (!(qa > 0.0) || !(disc > 0.0))
        return false;
    const float root = sqrt(disc);
    t0 = max(t0, (-qb - root) / qa);
    t1 = min(t1, (-qb + root) / qa);
    return t1 > t0;
}

// The exponent is clamped as height_fog.hlsli clamps it: far below the fog height it would
// overflow.
float MediumHeightSigmaT(MediumHeight m, float y)
{
    return m.density * exp(clamp(-m.falloff * (y - m.height), -80.0, 80.0));
}

// A local volume's density weight in [0, 1]: LocalFogVolumes.hlsli's LocalFogWeight.
float MediumLocalWeight(MediumLocal v, float3 p)
{
    const float4 wp = float4(p, 1.0);
    const float3 l  = float3(dot(wp, v.row0), dot(wp, v.row1), dot(wp, v.row2));
    const float  invFade = 1.0 / max(v.edgeFade, 1e-4);

    float w;
    if (v.shape == kMediumLocalSphere)
    {
        const float r = length(l);
        if (r >= 1.0)
            return 0.0;
        w = saturate((1.0 - r) * invFade);
    }
    else
    {
        const float3 a = abs(l);
        if (any(a >= 1.0))
            return 0.0;
        const float3 f = saturate((1.0 - a) * invFade);
        w = f.x * f.y * f.z;
    }

    if (v.heightFalloff > 0.0)
        w *= exp(-v.heightFalloff * (l.y + 1.0) * v.halfHeight);
    return w;
}

float MediumLocalSigmaT(MediumLocal v, float3 p)
{
    return v.density * MediumLocalWeight(v, p);
}

// What every source contributes at p: sigma_t, sigma_s per channel and the emission source
// term (Le x sigma_t).
struct MediumPoint
{
    float  sigmaT;
    float3 sigmaS;
    float3 emission;
};

MediumPoint MediumAt(MediumSet s, float3 p)
{
    MediumPoint r;
    const float g = MediumGlobalSigmaTAt(s.global, p);
    const float h = MediumHeightSigmaT(s.height, p.y);
    r.sigmaT   = g + h;
    r.sigmaS   = g * s.global.albedo + h * s.height.albedo;
    r.emission = g * s.global.emission;
    for (uint i = 0u; i < s.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        const float l = MediumLocalSigmaT(v, p);
        r.sigmaT   += l;
        r.sigmaS   += l * v.albedo;
        r.emission += l * v.emission;
    }
    return r;
}

// One source's albedo, emission and phase asymmetry. The local volumes scatter with the global
// medium's g.
void MediumSourceProps(MediumSet s, uint source, out float3 albedo, out float3 emission, out float g)
{
    g = s.global.anisotropy;
    if (source == kMediumSourceGlobal)
    {
        albedo   = s.global.albedo;
        emission = s.global.emission;
    }
    else if (source == kMediumSourceHeight)
    {
        albedo   = s.height.albedo;
        emission = float3(0.0, 0.0, 0.0);
        g        = s.height.anisotropy;
    }
    else
    {
        const MediumLocal v = MediumLoadLocal(source - kMediumSourceLocal0);
        albedo   = v.albedo;
        emission = v.emission;
    }
}

// ==== Optical depth ====
float MediumPieceLength(float t0, float t1)
{
    return (t1 >= kMediumFar) ? kMediumFar : t1 - t0;
}

// sigma(s) = a exp(-b s) integrated over s in [0, len]. len >= kMediumFar is the limit to
// infinity, which diverges unless the density falls along the ray (b > 0).
float MediumExpDepth(float a, float b, float len)
{
    if (a <= 0.0 || len <= 0.0)
        return 0.0;
    if (len >= kMediumFar)
        return (b > 1e-9) ? a / b : kMediumTauInfinite;
    const float u = b * len;
    if (abs(u) < 1e-3)
        return a * len * (1.0 - 0.5 * u + u * u / 6.0);
    return min(a * (1.0 - exp(clamp(-u, -80.0, 80.0))) / b, kMediumTauInfinite);
}

// The global medium along o + t d, t in [t0, t1], as at most two pieces in ray order, each
// float4(start, length, a, b) for sigma = a exp(-b s): the part below baseHeight has b = 0.
// Only the part inside its reach: the optical depth and the free flight both come from here.
uint MediumGlobalPieces(MediumGlobal m, float3 o, float3 d, float t0, float t1, out float4 pieces[2])
{
    pieces[0] = float4(0.0, 0.0, 0.0, 0.0);
    pieces[1] = float4(0.0, 0.0, 0.0, 0.0);
    if (m.density <= 0.0 || !(t1 > t0) || !MediumGlobalReach(m, o, d, t0, t1))
        return 0u;
    if (abs(d.y) < 1e-9)
    {
        pieces[0] = float4(t0, MediumPieceLength(t0, t1), MediumGlobalSigmaT(m, o.y), 0.0);
        return 1u;
    }
    const float kdy = m.falloff * d.y;
    const float tb  = (m.baseHeight - o.y) / d.y;
    const bool  climbs = d.y > 0.0;
    uint n = 0u;
    // A climbing ray starts below baseHeight, a descending one above it.
    const float firstEnd = min(t1, max(tb, t0));
    if (firstEnd > t0)
    {
        const float a = climbs ? m.density : MediumGlobalSigmaT(m, o.y + d.y * t0);
        const float b = climbs ? 0.0 : kdy;
        pieces[n] = float4(t0, MediumPieceLength(t0, firstEnd), a, b);
        ++n;
    }
    const float secondStart = max(t0, tb);
    if (t1 > secondStart)
    {
        const float a = climbs ? MediumGlobalSigmaT(m, o.y + d.y * secondStart) : m.density;
        const float b = climbs ? kdy : 0.0;
        pieces[n] = float4(secondStart, MediumPieceLength(secondStart, t1), a, b);
        ++n;
    }
    return n;
}

float MediumGlobalDepth(MediumGlobal m, float3 o, float3 d, float t0, float t1)
{
    float4 pieces[2];
    const uint n = MediumGlobalPieces(m, o, d, t0, t1, pieces);
    float tau = 0.0;
    for (uint i = 0u; i < n; ++i)
        tau += MediumExpDepth(pieces[i].z, pieces[i].w, pieces[i].y);
    return min(tau, kMediumTauInfinite);
}

float MediumHeightDepth(MediumHeight m, float3 o, float3 d, float t0, float t1)
{
    if (m.density <= 0.0 || !(t1 > t0))
        return 0.0;
    return MediumExpDepth(MediumHeightSigmaT(m, o.y + d.y * t0), m.falloff * d.y, MediumPieceLength(t0, t1));
}

// The global medium and the height fog, both in closed form.
float MediumAnalyticDepth(MediumSet s, float3 o, float3 d, float t0, float t1)
{
    return min(MediumGlobalDepth(s.global, o, d, t0, t1) + MediumHeightDepth(s.height, o, d, t0, t1),
               kMediumTauInfinite);
}

// ==== Local volumes along a ray ====
// The part of o + t d, t in [t0, t1], inside a local volume: [ta, tb]. False when it misses.
// The unit-volume space is an affine image of world space, so t means the same in both.
bool MediumLocalInterval(MediumLocal v, float3 o, float3 d, float t0, float t1, out float ta, out float tb)
{
    const float4 o4 = float4(o, 1.0);
    const float3 lo = float3(dot(o4, v.row0), dot(o4, v.row1), dot(o4, v.row2));
    const float3 ld = float3(dot(d, v.row0.xyz), dot(d, v.row1.xyz), dot(d, v.row2.xyz));
    ta = t0;
    tb = t1;
    if (v.shape == kMediumLocalSphere)
    {
        const float qa   = dot(ld, ld);
        const float qb   = dot(lo, ld);
        const float disc = qb * qb - qa * (dot(lo, lo) - 1.0);
        if (!(qa > 0.0) || !(disc > 0.0))
            return false;
        const float root = sqrt(disc);
        ta = max(ta, (-qb - root) / qa);
        tb = min(tb, (-qb + root) / qa);
    }
    else
    {
        [unroll] for (int axis = 0; axis < 3; ++axis)
        {
            if (abs(ld[axis]) < 1e-12)
            {
                if (abs(lo[axis]) >= 1.0)
                    return false;
                continue;
            }
            const float enter = (-1.0 - lo[axis]) / ld[axis];
            const float leave = ( 1.0 - lo[axis]) / ld[axis];
            ta = max(ta, min(enter, leave));
            tb = min(tb, max(enter, leave));
        }
    }
    return tb > ta;
}

// Delta tracking inside one local volume over its interval [ta, tb], the volume's core density
// the majorant: the first real collision, or kMediumFar.
float MediumLocalFreeFlight(MediumLocal v, float3 o, float3 d, float ta, float tb, inout uint rng)
{
    if (!(v.density > 0.0))
        return kMediumFar;
    float t = ta;
    for (uint i = 0u; i < kMediumMaxTrackingSteps; ++i)
    {
        t -= log(1.0 - MediumRandom(rng)) / v.density;
        if (t >= tb)
            return kMediumFar;
        if (MediumRandom(rng) < MediumLocalWeight(v, o + t * d))
            return t;
    }
    return kMediumFar;
}

// Ratio tracking over the same interval: an unbiased estimate of the volume's transmittance.
float MediumLocalRatioTracking(MediumLocal v, float3 o, float3 d, float ta, float tb, inout uint rng)
{
    if (!(v.density > 0.0) || !(tb > ta))
        return 1.0;
    float T = 1.0;
    float t = ta;
    for (uint i = 0u; i < kMediumMaxTrackingSteps; ++i)
    {
        t -= log(1.0 - MediumRandom(rng)) / v.density;
        if (t >= tb)
            break;
        T *= 1.0 - MediumLocalWeight(v, o + t * d);
        if (T <= 0.0)
            break;
    }
    return T;
}

// The volume's optical depth by the midpoint rule: biased, but the same for every caller that
// asks about the same segment.
float MediumLocalDepthMarch(MediumLocal v, float3 o, float3 d, float ta, float tb)
{
    if (!(v.density > 0.0) || !(tb > ta))
        return 0.0;
    const float dt = (tb - ta) / float(kMediumMarchSteps);
    float sum = 0.0;
    for (uint i = 0u; i < kMediumMarchSteps; ++i)
        sum += MediumLocalWeight(v, o + (ta + (float(i) + 0.5) * dt) * d);
    return v.density * sum * dt;
}

// ==== Transmittance ====
// Over [t0, t1]: the analytic sources exactly, the local volumes by ratio tracking. Unbiased;
// the reference path tracer's.
float MediumTransmittance(MediumSet s, float3 o, float3 d, float t0, float t1, inout uint rng)
{
    float T = exp(-min(MediumAnalyticDepth(s, o, d, t0, t1), 80.0));
    for (uint i = 0u; i < s.localCount; ++i)
    {
        if (!(T > 0.0))
            break;
        const MediumLocal v = MediumLoadLocal(i);
        float ta, tb;
        if (MediumLocalInterval(v, o, d, t0, t1, ta, tb))
            T *= MediumLocalRatioTracking(v, o, d, ta, tb, rng);
    }
    return T;
}

// The optical depth over [t0, t1] with the local volumes marched: deterministic, so a ReSTIR
// shift and the path it replays agree, and a froxel's light target is smooth.
float MediumDepthDeterministic(MediumSet s, float3 o, float3 d, float t0, float t1)
{
    float tau = MediumAnalyticDepth(s, o, d, t0, t1);
    for (uint i = 0u; i < s.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        float ta, tb;
        if (MediumLocalInterval(v, o, d, t0, t1, ta, tb))
            tau += MediumLocalDepthMarch(v, o, d, ta, tb);
    }
    return min(tau, kMediumTauInfinite);
}

float MediumTransmittanceDeterministic(MediumSet s, float3 o, float3 d, float t0, float t1)
{
    return exp(-min(MediumDepthDeterministic(s, o, d, t0, t1), 80.0));
}

// What only absorption removes, sigma_a = sigma_t x (1 - albedo) per channel, deterministic:
// the light a path keeps when the energy scattered out of it is assumed to come back.
float3 MediumAbsorptionTransmittance(MediumSet s, float3 o, float3 d, float t0, float t1)
{
    float3 tau = MediumGlobalDepth(s.global, o, d, t0, t1) * (1.0 - s.global.albedo)
               + MediumHeightDepth(s.height, o, d, t0, t1) * (1.0 - s.height.albedo);
    for (uint i = 0u; i < s.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        float ta, tb;
        if (MediumLocalInterval(v, o, d, t0, t1, ta, tb))
            tau += MediumLocalDepthMarch(v, o, d, ta, tb) * (1.0 - v.albedo);
    }
    return exp(-min(max(tau, 0.0), 80.0));
}

// ==== Free-flight sampling ====
// The distance s into a piece (sigma = a exp(-b s), length len) where the optical depth reaches
// xi. False when the piece holds less than xi; xi then loses the piece's depth.
bool MediumInvertPiece(float a, float b, float len, inout float xi, out float s)
{
    s = 0.0;
    if (!(a > 0.0))
        return false;
    const float tau = MediumExpDepth(a, b, len);
    if (xi >= tau)
    {
        xi -= tau;
        return false;
    }
    const float x = xi * b / a;
    s = (abs(x) < 1e-4) ? (xi / a) * (1.0 + 0.5 * x) : -log(max(1.0 - x, 1e-30)) / b;
    s = min(s, len);
    return true;
}

// Where along o + t d, t in [t0, t1], the global medium's optical depth from t0 reaches xi
// (xi = -ln(1 - u) is a free flight); kMediumFar when the segment holds less.
float MediumGlobalFreeFlight(MediumGlobal m, float3 o, float3 d, float t0, float t1, float xi)
{
    float4 pieces[2];
    const uint n = MediumGlobalPieces(m, o, d, t0, t1, pieces);
    for (uint i = 0u; i < n; ++i)
    {
        float s;
        if (MediumInvertPiece(pieces[i].z, pieces[i].w, pieces[i].y, xi, s))
            return pieces[i].x + s;
    }
    return kMediumFar;
}

float MediumHeightFreeFlight(MediumHeight m, float3 o, float3 d, float t0, float t1, float xi)
{
    if (m.density <= 0.0 || !(t1 > t0))
        return kMediumFar;
    float s;
    if (MediumInvertPiece(MediumHeightSigmaT(m, o.y + d.y * t0), m.falloff * d.y, MediumPieceLength(t0, t1), xi, s))
        return t0 + s;
    return kMediumFar;
}

// The first collision on o + t d, t in [t0, t1], with the source it belongs to: decomposition
// tracking (Kutz et al. 2017) - each source draws its own free flight (the analytic ones in
// closed form, the local volumes by delta tracking) and the nearest wins, which picks each
// source with probability sigma_source / sigma_t at the collision.
struct MediumCollision
{
    float t;        // kMediumFar: no collision on the segment
    uint  source;   // kMediumSource*
};

MediumCollision MediumSampleCollision(MediumSet s, float3 o, float3 d, float t0, float t1, inout uint rng)
{
    MediumCollision c;
    c.t      = kMediumFar;
    c.source = kMediumSourceNone;
    const float tg = MediumGlobalFreeFlight(s.global, o, d, t0, t1, -log(1.0 - MediumRandom(rng)));
    if (tg < c.t)
    {
        c.t      = tg;
        c.source = kMediumSourceGlobal;
    }
    const float th = MediumHeightFreeFlight(s.height, o, d, t0, min(t1, c.t), -log(1.0 - MediumRandom(rng)));
    if (th < c.t)
    {
        c.t      = th;
        c.source = kMediumSourceHeight;
    }
    for (uint i = 0u; i < s.localCount; ++i)
    {
        const MediumLocal v = MediumLoadLocal(i);
        float ta, tb;
        // Only a collision nearer than the current one matters.
        if (!MediumLocalInterval(v, o, d, t0, min(t1, c.t), ta, tb))
            continue;
        const float tl = MediumLocalFreeFlight(v, o, d, ta, tb, rng);
        if (tl < c.t)
        {
            c.t      = tl;
            c.source = kMediumSourceLocal0 + i;
        }
    }
    return c;
}

// ==== Henyey-Greenstein ====
// Normalised over the sphere for |g| < 1. FroxelCommon.hlsli's PhaseHG clamps its denominator
// at 1e-3, which shows only above g ~ 0.97.
float MediumPhaseHG(float cosTheta, float g)
{
    const float g2  = g * g;
    const float den = max(1.0 + g2 - 2.0 * g * cosTheta, 1e-8);
    return (1.0 - g2) / (4.0 * PI * den * sqrt(den));
}

// A direction about d (the way the path travels) with density MediumPhaseHG(dot(d, l), g).
float3 MediumSampleHG(float3 d, float g, float2 u)
{
    float cosTheta;
    if (abs(g) < 1e-3)
    {
        cosTheta = 1.0 - 2.0 * u.x;
    }
    else
    {
        const float sq = (1.0 - g * g) / (1.0 - g + 2.0 * g * u.x);
        cosTheta = (1.0 + g * g - sq * sq) / (2.0 * g);
    }
    cosTheta = clamp(cosTheta, -1.0, 1.0);
    const float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    const float phi      = 2.0 * PI * u.y;
    float3 t, b;
    OrthonormalBasis(d, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + d * cosTheta);
}

// ==== Equi-angular sampling (Kulla and Fajardo 2012) ====
// A distance on o + t d, t in [t0, t1] (both finite, d a unit vector), with density
// proportional to the inverse square distance to the point c:
//   pdf(t) = h / ((thetaB - thetaA) (h^2 + (t - delta)^2)).
void MediumEquiAngularFrame(float3 o, float3 d, float t0, float t1, float3 c,
                            out float delta, out float h, out float thetaA, out float thetaB)
{
    delta  = dot(c - o, d);
    h      = max(length(o + delta * d - c), 1e-4);
    thetaA = atan2(t0 - delta, h);
    thetaB = atan2(t1 - delta, h);
}

float MediumSampleEquiAngular(float3 o, float3 d, float t0, float t1, float3 c, float u, out float pdf)
{
    float delta, h, thetaA, thetaB;
    MediumEquiAngularFrame(o, d, t0, t1, c, delta, h, thetaA, thetaB);
    const float theta = lerp(thetaA, thetaB, u);
    const float t     = clamp(delta + h * tan(theta), t0, t1);
    const float x     = t - delta;
    pdf = h / (max(thetaB - thetaA, 1e-12) * (h * h + x * x));
    return t;
}

float MediumPdfEquiAngular(float3 o, float3 d, float t0, float t1, float3 c, float t)
{
    if (t < t0 || t > t1)
        return 0.0;
    float delta, h, thetaA, thetaB;
    MediumEquiAngularFrame(o, d, t0, t1, c, delta, h, thetaA, thetaB);
    const float x = t - delta;
    return h / (max(thetaB - thetaA, 1e-12) * (h * h + x * x));
}

#endif // CORE_PARTICIPATING_MEDIA_HLSLI
