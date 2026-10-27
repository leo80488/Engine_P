// debug_shade.hlsli - what a material pixel shader reports to the debug targets that count
// fragments. Compiled only into the DEBUG_SHADE permutation, which GBufferPass, TransparentPass,
// WaterSurfacePass and ParticleRenderPass put on their PSOs for the frames that show or capture
// such a target (DebugTargetViewPass). Every other permutation compiles the macros below to
// nothing, so its DXIL is unaffected.
//
// The counters are R32_UINT textures at fixed descriptor heap slots (shared/bindless_kind.hlsli
// BINDLESS_DEBUG_*), written with atomics - order does not matter, so draws need no barrier
// between them - and copied out and zeroed by their reader (DebugTargetProduce.cs). A UAV write
// turns the early depth test off for the debug PSO: a fragment the depth test then rejects has
// still run the shader, and is counted.

#ifndef DEBUG_SHADE_HLSLI
#define DEBUG_SHADE_HLSLI

#if defined(DEBUG_SHADE) && DEBUG_SHADE

#include "bindless.hlsli"   // BindlessRWTex2D, BINDLESS_DEBUG_*

// One more fragment the pixel shader ran for at @p svPos (SV_Position): the
// debug target overdraw. Called first thing in main, so a fragment that clip()
// or discard drops later still counts - it was shaded up to there. Helper lanes
// write nothing.
void DebugShade_Invocation(float4 svPos)
{
    RWTexture2D<uint> count = BindlessRWTex2D<uint>(BINDLESS_DEBUG_OVERDRAW);
    InterlockedAdd(count[uint2(svPos.xy)], 1u);
}
#define DEBUG_SHADE_INVOCATION(svPos) DebugShade_Invocation(svPos)

// The static size of the fragment's pixel shader (the debug target shadingCost): the material
// cost table's entry for @p matIdx - the instruction count of the shader the pass draws that
// material with in the normal frame's permutation (Renderer::CollectMaterialCosts) - added to
// the pixel's sum. Called with DEBUG_SHADE_INVOCATION, so it sums what overdraw counts.
void DebugShade_Cost(float4 svPos, uint matIdx)
{
    const uint c = BindlessSBuf<uint>(BINDLESS_DEBUG_COST_TABLE)[matIdx];
    if (c != 0u)
    {
        RWTexture2D<uint> sum = BindlessRWTex2D<uint>(BINDLESS_DEBUG_COST);
        InterlockedAdd(sum[uint2(svPos.xy)], c);
    }
}
#define DEBUG_SHADE_COST(svPos, matIdx) DebugShade_Cost(svPos, matIdx)

// The texel density of the fragment's base colour texture (the debug target mipDensity):
// @p lod is log2 texels per pixel - the isotropic level of detail max(|ddx(uv * size)|,
// |ddy(uv * size)|), not the sampler's (anisotropy and bias belong to the sampler) - and
// @p textured is false for a surface without one. Packed under the fragment's depth so that
// InterlockedMax keeps the front-most fragment's whatever the draw order (reverse-Z: larger
// depth is nearer; the low 8 bits of the depth hold the density, 1/16 of a level from -7.94 to
// +7.94, 0 = untextured). Call it after the shader's clip()s: only a surviving fragment is a
// candidate.
void DebugShade_Density(float4 svPos, float lod, bool textured)
{
    const uint code = textured ? (uint)clamp(round((lod + 8.0) * 16.0), 1.0, 255.0) : 0u;
    RWTexture2D<uint> packed = BindlessRWTex2D<uint>(BINDLESS_DEBUG_DENSITY);
    InterlockedMax(packed[uint2(svPos.xy)], (asuint(svPos.z) & ~0xFFu) | code);
}
// log2 texels per pixel of a @p w x @p h texture at @p uv (isotropic).
float DebugShade_Lod(float2 uv, uint w, uint h)
{
    const float2 size = float2(w, h);
    const float2 dx = ddx(uv) * size;
    const float2 dy = ddy(uv) * size;
    return 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-20));
}

#else

#define DEBUG_SHADE_INVOCATION(svPos)
#define DEBUG_SHADE_COST(svPos, matIdx)

#endif

#endif // DEBUG_SHADE_HLSLI
