// particle_common.hlsli - the interpolators shared by the particle VS template
// (particle_vertex.hlsli) and PS template (particle_surface.hlsli).
// The particle analogue of the mesh material varyings, deliberately not
// MaterialSurfaceVaryings: a camera-facing billboard has no real TBN / uv1 / velocity clip
// space, and it carries particle-domain data (visual mode, age) the mesh template never sees.
// Custom particle shaders hook this struct, never the mesh custom_surface.hlsli.

#ifndef PARTICLE_COMMON_HLSLI
#define PARTICLE_COMMON_HLSLI

#include "Particle.hlsli"

// VS -> PS interpolators, produced by particle_vertex.hlsli's main and consumed by
// particle_surface.hlsli's main. Keep both in sync with this layout.
struct ParticleVaryings
{
    float4 pos     : SV_POSITION;
    float2 uv      : TEXCOORD0;
    float4 color   : COLOR0;
    nointerpolation uint texIdx  : TEXCOORD1;   // bindless texture index (0xFFFFFFFFu = none)
    nointerpolation uint visMode : TEXCOORD2;   // ParticleVisualMode enum
    float  lifeT     : TEXCOORD3;               // 0..1 age for PS visual-mode ramps
    float  intensity : TEXCOORD4;               // emissive brightness multiplier (HDR)
    float3 worldPos  : TEXCOORD5;               // world position (lit path: view dir / IBL)
    float3 normalWS  : TEXCOORD6;               // billboard shading normal (lit path),
                                                // toward the viewer on camera-facing quads
};

// Per-draw root constants (b0 space0, the shared PVF root sig's param [0]), set per bucket
// by ParticleRenderPass. Declared once here because both the VS and the PS read it.
//   g_bucketBase  (VS) the first pool slot of this draw's bucket slice: SV_InstanceID is
//                 per-draw 0-based and excludes StartInstanceLocation, so the VS rebases
//                 the pool index.
//   g_bucketBlend (PS) the bucket's ParticleBlendMode (0 Alpha, 1 Additive, 2 Premultiplied,
//                 3 Multiply): the PS applies the soft fade and the fog per blend state,
//                 which is PSO state it cannot otherwise see.
cbuffer ParticleBucketCB : register(b0, space0)
{
    uint  g_bucketBase;
    uint  g_bucketBlend;
    uint2 _bucketPad;
};

#endif // PARTICLE_COMMON_HLSLI
