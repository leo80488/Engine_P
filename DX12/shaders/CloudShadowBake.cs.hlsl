// Bakes the cloud deck's transmittance along the sun direction into a low-res map the whole
// renderer samples, so a cloud in front of the sun darkens the world.
// Projection: an orthographic grid along the sun, centred on the camera. csRight / csUp are
// orthonormal and perpendicular to sunDir, so a receiver's projected coordinates are constant
// along its sun ray: one map serves every altitude below the layer, with no depth term.
// Consumers project with two dot products (cloud_shadow.hlsli); only this shader turns the
// basis back into world positions.
// Each texel walks the shell along -sunDir from above the layer, accumulating Beer's-law
// optical depth with the same SampleDensity as the visible raymarch. Output: transmittance
// in [0, 1], 1 = full sun.
// Deliberately coarse (a soft large-scale term, not a contact shadow): no detail erosion,
// whose 15-30 m features cannot survive a ~100 m texel and would only alias in the wind.

#define CLOUD_CB_REGISTER register(b0, space2)
#include "CloudCommon.hlsli"
#include "CloudDensity.hlsli"

RWTexture2D<float> ShadowOut : register(u0, space2);

// Steps along the sun ray through the shell (2.5 km thick by default). 24 is plenty for a
// low-frequency term: ~512^2 x 24 samples, well under the visible raymarch's cost.
static const int kShadowSteps = 24;

[numthreads(8, 8, 1)]
void main(uint2 dt : SV_DispatchThreadID)
{
    const uint dim = (uint)cloudShadowDim;
    if (dt.x >= dim || dt.y >= dim) return;

    // Texel centre -> [-1, 1] -> world point on the plane through cloudShadowCenter.
    float2 g = ((float2(dt) + 0.5) / cloudShadowDim) * 2.0 - 1.0;
    float3 planeP = cloudShadowCenter
                  + csRight * (g.x * cloudShadowExtent)
                  + csUp    * (g.y * cloudShadowExtent);

    // Back up along the sun ray to a point safely above the layer, then march down through the
    // shell; starting on the plane would miss everything when the plane is inside or above the deck.
    float tTop, tBot;
    float3 originAbove = planeP + sunDir * (topAltitude * 4.0 + 1.0);
    if (!ShellInterval(originAbove, -sunDir, tTop, tBot))
    {
        ShadowOut[dt] = 1.0;     // the sun ray misses the deck
        return;
    }

    const float span = tBot - tTop;
    if (span <= 0.0)
    {
        ShadowOut[dt] = 1.0;
        return;
    }

    // No detail erosion (see the file header).
    gDetailFade = 0.0;

    const float stepLen = span / float(kShadowSteps);
    float od = 0.0;
    [loop] for (int i = 0; i < kShadowSteps; ++i)
    {
        float3 p = originAbove - sunDir * (tTop + (float(i) + 0.5) * stepLen);
        float  h = HeightInLayer(p);
        if (h <= 0.0 || h >= 1.0) continue;
        float dummy;
        od += SampleDensity(p, h, true, dummy) * stepLen;
    }

    // cloudShadowStrength scales the optical depth, not the transmittance: at 0 the map is
    // exactly 1 everywhere (a free no-op for consumers), and in between it stays physically
    // shaped instead of washing toward flat grey.
    ShadowOut[dt] = saturate(exp(-od * extinction * cloudShadowStrength));
}
