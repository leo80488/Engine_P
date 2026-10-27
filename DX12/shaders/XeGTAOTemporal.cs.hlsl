// AO temporal accumulation over DENOISED input. Order (XeGTAOPass::Execute):
//   Main (raw 1-spp AO) -> spatial denoise (edge-aware 3x3) -> this pass -> LightingPass
// Do not run it before the denoise: on raw AO sigma is so wide that the variance clip
// accepts nearly every stale history value, and crevice / corner ghosts persist for 10+
// frames. On denoised input sigma is a tight real spread, so the clip detects
// disocclusion and alpha can sit at ~0.95.
//
// Compute root signature (space2):
//   b0 - TemporalCB
//   t0 - denoised AO this frame    (R8_UNORM, spatial-denoise output)
//   t1 - packed XeGTAO edge mask   (R8_UNORM)
//   t2 - prev accumulated AO       (R8_UNORM, AO history ping-pong)
//   t3 - prev linear depth         (R32_FLOAT, depth history ping-pong)
//   t4 - NDC velocity              (R16G16_FLOAT)
//   t5 - current linear depth      (mip 0 of the XeGTAO pyramid, R32_FLOAT)
//   u0 - this frame's accumulated AO (R8_UNORM, FINAL output)
//   u1 - this frame's linear depth (R32_FLOAT, next frame's t3)
//
// Reprojection: current UV minus the GBuffer NDC velocity -> previous UV. History
// rejection, layered:
//   1. screen-edge fade (off-screen reprojection);
//   2. clamp-delta disocclusion (variance clip, AO-space mismatch);
//   3. edge-mask amplified rejection (edges are disocclusion-prone);
//   4. plane distance: current vs reprojected previous linear Z, which catches a
//      dynamic reveal whose two surfaces have similar AO.

cbuffer TemporalCB : register(b0, space2)
{
    uint  viewportWidth;
    uint  viewportHeight;
    float historyAlpha;       // 0 on first frame; steady-state ~0.95
    float rejectionDiff;      // disocclusion threshold; ~0.05 on denoised input
    // float4: keeps the CB size in sync with the C++ mirror's float _pad[4] (HLSL pads
    // cbuffer array elements to 16 B).
    float4 _pad;
};

Texture2D<float>   gAOInput      : register(t0, space2);  // denoised AO
Texture2D<float>   gEdges        : register(t1, space2);  // packed 4-edge mask
Texture2D<float>   gHistory      : register(t2, space2);  // AO history
Texture2D<float>   gPrevDepth    : register(t3, space2);  // prev linear depth
Texture2D<float2>  gVelocity     : register(t4, space2);
Texture2D<float>   gCurrDepth    : register(t5, space2);  // current linear depth (mip0)
RWTexture2D<float> gOutput       : register(u0, space2);  // AO write
RWTexture2D<float> gPrevDepthOut : register(u1, space2);  // depth copy for next frame
SamplerState       gLinear       : register(s0, space2);

// Unpacks the 8-bit edge byte (XeGTAO.cs.hlsl XeGTAO_PackEdges) into 4 cardinal
// flatness values: 1.0 = flat, 0.0 = strong depth discontinuity, quantised to
// {0, 1/3, 2/3, 1}. A copy of XeGTAODenoise's XeGTAO_UnpackEdges, so this file needs
// no include of it.
float4 UnpackEdges(float packedVal)
{
    uint pv = uint(packedVal * 255.5);
    float4 e;
    e.x = float((pv >> 6) & 0x03u) / 3.0;
    e.y = float((pv >> 4) & 0x03u) / 3.0;
    e.z = float((pv >> 2) & 0x03u) / 3.0;
    e.w = float((pv >> 0) & 0x03u) / 3.0;
    return saturate(e);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= viewportWidth || dtid.y >= viewportHeight) return;

    const int2   px      = int2(dtid.xy);
    const float2 vpSize  = float2(viewportWidth, viewportHeight);
    const int2   vpMax   = int2(viewportWidth - 1, viewportHeight - 1);
    const float  currAO  = gAOInput.Load(int3(px, 0));
    const float  currZ   = gCurrDepth.Load(int3(px, 0));

    // Stash the current depth for next frame unconditionally, so the first-frame / resize
    // path below still leaves a valid prev-depth source.
    gPrevDepthOut[px] = currZ;

    // First frame / resize: historyAlpha is 0, no history work.
    if (historyAlpha <= 0.0)
    {
        gOutput[px] = currAO;
        return;
    }

    // Reproject via the GBuffer NDC velocity (currNDC - prevNDC): UV delta = flip Y (NDC +y
    // up -> UV +y down) and scale by 0.5 (NDC [-1,1] -> UV [0,1]).
    const float2 currUV  = (float2(px) + 0.5) / vpSize;
    const float2 vel     = gVelocity.Load(int3(px, 0)).xy;
    const float2 uvDelta = float2(vel.x, -vel.y) * 0.5;
    const float2 prevUV  = currUV - uvDelta;

    // Soft screen-edge fade: no "has history / no history" snap-flicker when the
    // reprojection crosses a viewport boundary.
    const float2 edgeDist   = min(prevUV, 1.0 - prevUV);
    const float  edgeDistPx = min(edgeDist.x * vpSize.x,
                                  edgeDist.y * vpSize.y);
    const float  edgeFade   = saturate(edgeDistPx * (1.0 / 20.0));

    // ---- 5-tap cross neighbourhood stats ----
    // On denoised input the 3x3 corners often straddle depth edges (alcove corners meet at
    // diagonals) and pull mean / sigma off; a cross (centre + 4 cardinals) estimates the
    // central surface's spread better. gamma = 1.0 is tight enough on denoised sigma that any
    // clip is a real disocclusion signal.
    const float n0 = currAO;
    const float n1 = gAOInput.Load(int3(clamp(px + int2(-1,  0), int2(0,0), vpMax), 0));
    const float n2 = gAOInput.Load(int3(clamp(px + int2( 1,  0), int2(0,0), vpMax), 0));
    const float n3 = gAOInput.Load(int3(clamp(px + int2( 0, -1), int2(0,0), vpMax), 0));
    const float n4 = gAOInput.Load(int3(clamp(px + int2( 0,  1), int2(0,0), vpMax), 0));

    const float aoMean = (n0 + n1 + n2 + n3 + n4) * (1.0 / 5.0);
    const float aoVar  = max(0.0,
        ((n0 - aoMean) * (n0 - aoMean) +
         (n1 - aoMean) * (n1 - aoMean) +
         (n2 - aoMean) * (n2 - aoMean) +
         (n3 - aoMean) * (n3 - aoMean) +
         (n4 - aoMean) * (n4 - aoMean)) * (1.0 / 5.0));

    // Sigma floor: the denoised signal is smooth enough that sigma can reach ~0 on uniform
    // surfaces. Without the floor the clip window collapses to one value and the
    // accumulation's own frame-to-frame drift reads as disocclusion.
    const float kSigmaFloor = 0.02;
    const float aoStdev     = max(sqrt(aoVar), kSigmaFloor);
    const float kGamma      = 1.0;
    const float aoMin       = saturate(aoMean - kGamma * aoStdev);
    const float aoMax       = saturate(aoMean + kGamma * aoStdev);

    // ---- 4-tap history fetch with cross-surface rejection ----
    // Bilinear history at a depth edge (alcove wall / floor) blends two surfaces into a value
    // that belongs to neither and can still pass the tight clip window. When the 4 taps'
    // range exceeds what a coherent surface's stdev predicts, take the single tap closest to
    // aoMean (most likely the current pixel's surface); otherwise interpolate bilinearly.
    const float2 prevUVexact = prevUV * vpSize - 0.5;
    const int2   p00         = int2(floor(prevUVexact));
    const float2 fPart       = prevUVexact - float2(p00);

    const int2 q00 = clamp(p00 + int2(0, 0), int2(0, 0), vpMax);
    const int2 q10 = clamp(p00 + int2(1, 0), int2(0, 0), vpMax);
    const int2 q01 = clamp(p00 + int2(0, 1), int2(0, 0), vpMax);
    const int2 q11 = clamp(p00 + int2(1, 1), int2(0, 0), vpMax);

    const float h00 = gHistory.Load(int3(q00, 0));
    const float h10 = gHistory.Load(int3(q10, 0));
    const float h01 = gHistory.Load(int3(q01, 0));
    const float h11 = gHistory.Load(int3(q11, 0));

    const float hRange = max(max(h00, h10), max(h01, h11))
                       - min(min(h00, h10), min(h01, h11));
    const float crossSurfaceThresh = max(0.08, aoStdev * 3.0);

    float historyAO;
    if (hRange > crossSurfaceThresh)
    {
        float best     = h00;
        float bestDist = abs(h00 - aoMean);
        float d;
        d = abs(h10 - aoMean); if (d < bestDist) { bestDist = d; best = h10; }
        d = abs(h01 - aoMean); if (d < bestDist) { bestDist = d; best = h01; }
        d = abs(h11 - aoMean); if (d < bestDist) { bestDist = d; best = h11; }
        historyAO = best;
    }
    else
    {
        historyAO = lerp(lerp(h00, h10, fPart.x),
                         lerp(h01, h11, fPart.x),
                         fPart.y);
    }

    // ---- Variance clip + clamp-delta rejection ----
    const float historyClamped = clamp(historyAO, aoMin, aoMax);
    const float clampDelta     = abs(historyAO - historyClamped);
    const float rejection      = smoothstep(rejectionDiff * 0.5,
                                            rejectionDiff, clampDelta);

    // ---- Edge-gated rejection boost ----
    // Edge pixels are spatial-denoise dead zones (XeGTAODenoise's weights collapse across
    // strong edges), so they arrive with raw per-frame AO and only temporal integration at
    // alpha ~0.95 cleans them. Do not lower alpha on edges: that starves the integrator and
    // the raw noise shows. Every pixel keeps the full alpha baseline; on edges the existing
    // rejection signal is amplified instead, so only a real disocclusion
    // (clampDelta > rejectionDiff) pulls alpha down there. Static edges keep rejection 0.
    const float4 edges    = UnpackEdges(gEdges.Load(int3(px, 0)));
    const float  flatness = dot(edges, 0.25.xxxx);
    const float  edgeBoost = lerp(1.5, 1.0, flatness);     // 1x..1.5x multiplier
    const float  rejectionE = saturate(rejection * edgeBoost);

    // ---- Plane-distance disocclusion rejection ----
    // AO-space rejection misses a reveal whose surfaces have similar AO (a hand or a door
    // sliding across leaves exactly that ghost); depth does not. Relative Z error
    // (normalised by currZ, so the test is scale-invariant) through exp(-zDiff x 50): ~0.6 at
    // 1%, ~0 beyond ~8% (where distinct surfaces start). 50 is the classic TAA depth
    // rejection constant: higher over-rejects sub-pixel reprojection noise, lower lets
    // ghosts through.
    const float prevZ        = gPrevDepth.SampleLevel(gLinear, saturate(prevUV), 0);
    const float zDiff        = abs(currZ - prevZ) / max(currZ, 1e-4);
    const float depthAccept  = exp(-zDiff * 50.0);

    const float alpha = historyAlpha * edgeFade * (1.0 - rejectionE) * depthAccept;

    gOutput[px] = lerp(currAO, historyClamped, alpha);
}
