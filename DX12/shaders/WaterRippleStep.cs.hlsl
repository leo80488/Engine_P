// One explicit 2D wave-equation step for an interactive water body's height field,
// with per-frame impulse injection.
// The field is R16G16_FLOAT: .r = height h(t), .g = h(t-dt). It is a high-detail SIM
// WINDOW that follows the interaction (WaterRippleSystem), not the whole body, so a stone
// in a huge ocean makes a local ripple. When the window moves, gShift re-anchors the
// previous field in world space so ripples stay put while the window slides.
// Shared compute root signature (the table ParticleSimPass uses; WaterRippleSystem binds it):
//   b0 space2 - RippleStepCB (params + impulse point list)
//   u0 space2 - gSrc (read:  h(t), h(t-dt)) in the PREVIOUS window's frame
//   u1 space2 - gDst (write: h(t+dt), h(t)) in this window's frame

RWTexture2D<float2> gSrc : register(u0, space2);
RWTexture2D<float2> gDst : register(u1, space2);

#define MAX_RIPPLE_POINTS 32

cbuffer RippleStepCB : register(b0, space2)
{
    uint   gResolution;   // field is gResolution x gResolution
    float  gC2;           // wave coefficient (CFL-stable: clamped <= 0.30 on the CPU)
    float  gDamping;      // 0..1 energy loss per step (0 = ripples never die)
    uint   gPointCount;   // active impulse points this frame

    uint   gClear;        // 1 -> zero the field (slot (re)claim: kills GPU garbage)
    int    gShiftX;       // texel shift to re-anchor gSrc after a window move
    int    gShiftZ;
    float  _pad0;

    // xy = uv [0,1] in the WINDOW, z = radius (uv units), w = signed height impulse
    float4 gPoints[MAX_RIPPLE_POINTS];
};

// Read the previous field at this-frame texel q, re-anchored by the window shift.
// Texels that scrolled in from outside the previous window read as calm (0).
float2 SrcAt(int2 q, int res)
{
    int2 sp = q + int2(gShiftX, gShiftZ);
    if (sp.x < 0 || sp.y < 0 || sp.x >= res || sp.y >= res) return float2(0.0, 0.0);
    return gSrc[sp];
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const int res = int(max(gResolution, 1u));
    if (int(id.x) >= res || int(id.y) >= res) return;
    const int2 p = int2(id.xy);

    if (gClear != 0) { gDst[p] = float2(0.0, 0.0); return; }

    const int last = res - 1;
    const float2 cC = SrcAt(p, res);
    const float  hC    = cC.r;
    const float  hPrev = cC.g;
    const float  hL = SrcAt(int2(max(p.x - 1, 0),    p.y), res).r;
    const float  hR = SrcAt(int2(min(p.x + 1, last), p.y), res).r;
    const float  hD = SrcAt(int2(p.x, max(p.y - 1, 0)), res).r;
    const float  hU = SrcAt(int2(p.x, min(p.y + 1, last)), res).r;

    // Explicit finite-difference wave step (Laplacian in texel units):
    //   h_{n+1} = 2 h_n - h_{n-1} + c^2 lap(h_n), then global damping.
    const float lap   = (hL + hR + hD + hU) - 4.0 * hC;
    float       hNext = (2.0 * hC - hPrev + gC2 * lap) * (1.0 - gDamping);

    // Impulse injection: a splat per active source (splash / wake / Lua Engine.RippleAt).
    // Few points; the per-texel loop is race-free and cheap.
    const float2 uv = (float2(id.xy) + 0.5) / float(res);
    [loop]
    for (uint i = 0; i < gPointCount; ++i)
    {
        const float2 c    = gPoints[i].xy;
        const float  rad  = max(gPoints[i].z, 1e-4);
        const float  amp  = gPoints[i].w;
        const float  d2   = dot(uv - c, uv - c);
        const float  d    = sqrt(d2);
        // Concentric ring profile (a few rings under a Gaussian envelope) rather
        // than one filled bump, so a splash reads as expanding "stone in a pond"
        // rings instead of a single blob heaving up and down.
        const float  env  = exp(-d2 / (rad * rad));
        const float  ring = cos(d / rad * 8.0);
        hNext += amp * ring * env;
    }

    // Absorbing boundary: fade to 0 within a thin border so waves die at the
    // window edge instead of reflecting back and filling it.
    const float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    hNext *= smoothstep(0.0, 0.06, edge);

    hNext = clamp(hNext, -16.0, 16.0);
    gDst[p] = float2(hNext, hC);
}
