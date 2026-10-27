#ifndef CLOUD_COMMON_HLSLI
#define CLOUD_COMMON_HLSLI

// Shared CB layout and spherical-shell math for the volumetric cloud raymarch (CS) and the
// depth-aware composite (PS). Both must see the identical shell intersection, or the
// composite's occlusion guard drifts from what the march did.
// The includer picks the CB binding first:
//   #define CLOUD_CB_REGISTER register(b0, space2)   // raymarch CS
//   #define CLOUD_CB_REGISTER register(b3, space0)   // composite PS
// The layout must match CloudConstants in CloudPass.h bit for bit; the row comments count
// 16-byte registers - do not reorder.

cbuffer CloudCB : CLOUD_CB_REGISTER
{
    // invViewProj is uploaded transposed (engine convention), so keep the default column-major
    // qualifier and use row-vector mul.
    float4x4 invViewProj;

    float3 cameraPos;       float nearZ;             // row 4
    float  farZ;            float bottomAltitude;
    float  topAltitude;     float coverage;          // row 5
    float  density;         float baseNoiseScale;
    float  detailNoiseScale; float detailStrength;   // row 6
    float  weatherScale;    float cloudTypeBias;
    float  anvilBias;       float extinction;        // row 7

    float3 sunDir;          float ambientStrength;   // row 8
    float3 sunColor;        float phaseFwdG;         // row 9
    float3 ambientTint;     float phaseBackG;        // row 10
    float3 cloudColor;      float phaseBlend;        // row 11
    float3 windOffset;      float silverIntensity;   // row 12

    float  silverSpread;    float frameIndex;
    float  maxSteps;        float maxTraceDist;      // row 13

    float  halfResW;        float halfResH;
    float  fullResW;        float fullResH;          // row 14

    // Temporal reconstruction (CloudTemporal.cs.hlsl). prevViewProj is the previous frame's
    // un-jittered view-projection, transposed like invViewProj: a world point projected through it
    // lands on the pixel grid the history was written on.
    float4x4 prevViewProj;                           // rows 15..18

    // resetHistory != 0 -> ignore the history (frame 0, resize, camera teleport).
    // temporalBlend = steady-state history weight.
    float  resetHistory;    float temporalBlend;
    // ambientLightLum: luminance of the light on the sky dome at the layer (CloudPass::SetSun's
    // skyColor); scales the skylight term, 0 at night.
    float  temporalOn;      float ambientLightLum;   // row 19

    // Cloud-shadow basis (CloudShadowBake.cs.hlsl). csRight / csUp are orthonormal and both
    // perpendicular to sunDir, which makes the projection altitude-invariant.
    // cloudShadowExtent is the half-size of the covered square in metres.
    float3 csRight;              float cloudShadowExtent;   // row 20
    float3 csUp;                 float cloudShadowDim;      // row 21
    float3 cloudShadowCenter;    float cloudShadowStrength; // row 22
};

static const float kPlanetRadius  = 6360000.0;            // m (UE default 6360 km)
static const float3 kPlanetCenter = float3(0.0, -kPlanetRadius, 0.0);

// Sentinel scene distance for sky pixels (reversed-Z depth == 0). The raymarch writes it to
// the march-distance texture and the composite rebuilds it for full-res sky pixels, so the
// depth-aware weights treat sky vs sky as a perfect match.
static const float kCloudSkyDist = 1.0e8;

// Ray vs sphere centred at kPlanetCenter; false if there are no real roots. The constant term
// is (h-r)(h+r): at planet scale dot(oc,oc) - r*r loses ~7 digits to cancellation.
bool RaySphere(float3 ro, float3 rd, float radius, out float t0, out float t1)
{
    float3 oc = ro - kPlanetCenter;
    float h  = length(oc);
    float b  = dot(oc, rd);
    float c  = (h - radius) * (h + radius);
    float disc = b * b - c;
    t0 = 0.0; t1 = 0.0;
    if (disc < 0.0) return false;
    float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

// March interval through the cloud shell [R_bottom, R_top] for a camera below, inside or
// above the layer. False if the ray misses.
bool ShellInterval(float3 ro, float3 rd, out float tEntry, out float tExit)
{
    const float rB = kPlanetRadius + bottomAltitude;
    const float rT = kPlanetRadius + topAltitude;
    const float hCam = length(ro - kPlanetCenter);

    float b0, b1, u0, u1;
    bool hitB = RaySphere(ro, rd, rB, b0, b1);
    bool hitT = RaySphere(ro, rd, rT, u0, u1);

    tEntry = 0.0; tExit = 0.0;
    if (hCam < rB)
    {
        // Below the layer (the usual case): enter at the bottom sphere's exit, leave at the top
        // sphere's exit. Downward rays get a planet-scale tEntry that the horizon fade rejects.
        tEntry = b1;
        tExit  = u1;
    }
    else if (hCam < rT)
    {
        // Inside the layer.
        tEntry = 0.0;
        tExit  = (hitB && b0 > 0.0) ? b0 : u1;
    }
    else
    {
        // Above the layer.
        if (!hitT || u0 < 0.0) return false;
        tEntry = u0;
        tExit  = (hitB && b0 > 0.0) ? b0 : u1;
    }
    return tExit > tEntry;
}

float HeightInLayer(float3 wp)
{
    float r = length(wp - kPlanetCenter);
    return (r - (kPlanetRadius + bottomAltitude))
         / max(topAltitude - bottomAltitude, 1.0);
}

#endif // CLOUD_COMMON_HLSLI
