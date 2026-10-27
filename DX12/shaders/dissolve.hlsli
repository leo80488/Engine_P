// dissolve.hlsli - per-instance noise dissolve, shared by the material surface template
// (custom_surface.hlsli: GBuffer + forward transparent) and the depth-only shadow PS
// (Shadow.ps.hlsl).
//
// Pure functions: the caller supplies the noise texture/sampler and the per-instance params, so
// this header has no binding dependencies and can be included from any stage.
//
// Data flow:
//   GPUInstanceData.dissolveParams    x=amount (0 intact -> 1 gone)
//                                     y=edgeWidth  z=noiseScale  w=edgeIntensity
//   GPUInstanceData.dissolveEdgeColor rgb edge glow color
//   PerViewCB.gDissolveNoiseIdx / shadow cascade CB offset 224 - bindless index of the
//   engine-global 256x256 R8 tileable FBM noise (~0u = missing).
//
// The caller branches on dissolveParams.x > 0 before calling, so instances with amount == 0 (the
// common case) never touch the noise texture.

#ifndef DISSOLVE_HLSLI
#define DISSOLVE_HLSLI

// clip()s fully dissolved pixels and returns the edge-glow factor in [0,1] (1 = at the dissolve
// front, 0 = outside the glow band). amount sweeps the threshold past the noise max, so amount=1
// clips all pixels (including n==1) with the glow band fully consumed by the end of the sweep.
float DissolveClipEdge(Texture2D noiseTex, SamplerState samp,
                       float4 dissolveParams, float2 uv)
{
    const float amount    = dissolveParams.x;
    const float edgeWidth = max(dissolveParams.y, 1e-4);
    const float n = noiseTex.Sample(samp, uv * dissolveParams.z).r;
    const float d = n - amount * (1.0 + edgeWidth + 1e-3);
    clip(d);
    return 1.0 - saturate(d / edgeWidth);
}

// Sampler-free variant for secondary passes (outline hull / outline object-ID / hair fringe
// mark) that do not bind s0: point fetch with manual wrap. They need only the clip decision, so
// filtering quality does not matter and a per-pass sampler is not worth binding.
void DissolveClipLoad(Texture2D noiseTex, float4 dissolveParams, float2 uv)
{
    uint w, h;
    noiseTex.GetDimensions(w, h);
    w = max(w, 1u); h = max(h, 1u);
    const float2 tuv = frac(uv * dissolveParams.z);
    const uint2  tc  = min(uint2(tuv * float2(w, h)), uint2(w - 1u, h - 1u));
    const float  n   = noiseTex.Load(int3(tc, 0)).r;
    clip(n - dissolveParams.x * (1.0 + max(dissolveParams.y, 1e-4) + 1e-3));
}

#endif // DISSOLVE_HLSLI
