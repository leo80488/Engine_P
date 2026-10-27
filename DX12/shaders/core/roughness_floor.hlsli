// Perceptual-roughness floors. The rules every core header follows are at the top of
// core/gbuffer_codec.hlsli.
//
// 0.045 is the raster lighting floor (Lighting.ps): it keeps a point light's GGX
// highlight from aliasing to one pixel, and the DI / GI / reflection consumers of the
// ray-traced surface headers keep it. A path tracer (PT_REFERENCE_GGX: the unclipped
// NDF and the |N x H|^2 pdf, exact for a narrow lobe) samples the lobe instead; at 0.045
// (alpha 0.002) a mirror's reflection spreads over several pixels at street distances,
// visibly blurred next to the RT reflections' exact mirror direction. Its floor is 0.01.
//
// RT_ROUGHNESS_FLOOR is the floor of a ray-traced surface in the including shader: the
// path tracer's under PT_REFERENCE_GGX, else the raster one (a shader may define it
// first). rt_hit_fetch.hlsli and RtxdiBridge.hlsli read it; RRGuides.cs.hlsl picks by
// path (beside the path tracer its guides must equal the path tracer's, on a ReSTIR path
// they follow the raster lighting).

#ifndef CORE_ROUGHNESS_FLOOR_HLSLI
#define CORE_ROUGHNESS_FLOOR_HLSLI

#define RASTER_ROUGHNESS_FLOOR 0.045
#define PT_ROUGHNESS_FLOOR 0.01

#if !defined(RT_ROUGHNESS_FLOOR) && defined(PT_REFERENCE_GGX)
#define RT_ROUGHNESS_FLOOR PT_ROUGHNESS_FLOOR
#elif !defined(RT_ROUGHNESS_FLOOR)
#define RT_ROUGHNESS_FLOOR RASTER_ROUGHNESS_FLOOR
#endif

#endif
