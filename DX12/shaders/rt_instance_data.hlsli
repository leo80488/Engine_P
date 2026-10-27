#ifndef RT_INSTANCE_DATA_HLSLI
#define RT_INSTANCE_DATA_HLSLI
// RTInstanceData: per-TLAS-instance data shared by every ray-tracing consumer, indexed
// by InstanceID(), 128 bytes. This is the only HLSL declaration of the layout; shaders
// that use another name (DDGIInstanceData, HRInstanceData) typedef it. The instance
// buffers are root SRVs, so the stride comes from this struct: a field reorder here or in
// the C++ mirror shears every instance after [0]. C++ mirror: struct DDGIInstanceData in
// src/Renderer/DDGISceneAS.cpp (static_assert on sizeof and on the block-boundary
// offsetof). Consumers declare their own binding, e.g.
//     StructuredBuffer<RTInstanceData> g_Instances : register(t1, space0);
//
// ==== rtFlags bits (mirror: kRTInstFlag* in src/Renderer/DDGISceneAS.cpp) ====
// Spare bits of an existing word, so none of them grows the 128-byte layout.
#define RT_INST_FLAG_SKINNED          1u // deforming BLAS, rebuilt/refit each frame
// Custom-shader material whose albedo resolved to neither a known space3 alias
// (_BaseMap / _MainTex / ...) nor an authored engine slot: every material field in this
// record is an engine guess, the real look comes from a pixel shader a ray cannot run.
// Consumers should substitute an environment estimate rather than shade the wrong surface.
#define RT_INST_FLAG_PROXY_UNTRUSTED  2u
// albedo / surface come from a material surface-cache atlas page. uvTilingOffset holds
// the page rect (the material's own tiling is baked in) and the page's alpha holds opacity.
#define RT_INST_FLAG_SURFACE_CACHE    4u
// Backface hits are legitimate (two-sided material). Lives here, not only in
// RTAlphaRecord, because the alpha table is gated on resource availability: a frame
// without it must not turn foliage and cloth single-sided.
#define RT_INST_FLAG_DOUBLE_SIDED     8u
// Material shaderType is NPR (SHADERTYPE_NPR_RAMP / SHADERTYPE_NPR_COLOR, the stencil-2
// deferred toon path in Lighting.ps). Reflection hit shading swaps the Cook-Torrance sun
// term for a ramp approximation with the toon model's minimum brightness: on screen the
// ramp replaces NdotL and its U=0 end is an authored non-black floor, so an NdotL-gated
// sun term would render the reflected character's shadow side far darker than the
// primary view.
#define RT_INST_FLAG_NPR              16u
// Surface-cached instance whose albedo stays the material's own full-res texture
// (space3 alias), sampled at the raw mesh UV; the 128^2 page serves surface / emissive
// mask only. baseColor is the authored tint (not neutralised to white) for these.
#define RT_INST_FLAG_ALBEDO_RAW       32u
// Material draws an inverted-hull outline on screen (MaterialComponent outline flag).
// The hull never enters the TLAS; reflection consumers ink the silhouette analytically
// (grazing-incidence darkening).
#define RT_INST_FLAG_OUTLINE          64u
// Interleaved MeshLibrary has an authored float4 tangent at byte offset 32.
#define RT_INST_FLAG_INTERLEAVED_TANGENT 128u
// Listed in the ReSTIR GI emitter table (emits at a GI hit, reachable by GI
// rays, not blended); RTGI's emitter-sampling MIS split keys on it.
#define RT_INST_FLAG_GI_EMITTER       256u
// BlendMode::Transmissive: a dielectric the path tracer crosses (PathTrace.cs.hlsl reads its
// transmission fields from the path material table); its back faces are never culled, so a
// ray inside a solid leaves through them.
#define RT_INST_FLAG_TRANSMISSIVE     512u
// Bits 16..31 carry the hit material's persistent MaterialGPUData slot + 1 (0 = unknown
// this frame). Consumers read the real toon knobs through the bindless material table
// (HybridReflConstants.materialBufIdx):
//   uint slotPlus1 = rtFlags >> 16;

struct RTInstanceData
{
    float4 baseColor;      //   0  rgb albedo (linear, see DDGISceneAS); a =
                           //      material opacity, read by the candidate
                           //      alpha test for GBuffer parity - in use.
    float4 emissive;       //  16  rgb premultiplied by strength;
                           //      a = customShadingModel (1 = Unlit -> flat
                           //      albedo + emissive at hits, Lighting.ps parity)
    uint   vbBindlessIdx;  //  32
    uint   ibBindlessIdx;  //  36
    uint   vertexStart;    //  40
    uint   indexStart;     //  44
    int    emissiveTexIdx; //  48  bindless tex pool index, -1 = none
    // Bytes per vertex in the interleaved VB: the same per-library stride the BLAS build
    // used. MeshLibrary strides vary (32, 48 with a float4 tangent, PMX / scene imports write
    // their own); do not hard-code 32: other meshes would read garbage normals / UVs and
    // randomise the front / backface classification. pos@0 nrm@12 uv@24 hold in every layout.
    uint   vertexStride;   //  52
    // De-interleaved stream mode (procedural primitives, skinned): when normalBufIdx !=
    // kInvalidBufferIndex, vbBindlessIdx is the position stream (float3, stride 12) and
    // normals / uv0 come from these separate streams. Invalid -> interleaved walk.
    uint   normalBufIdx;   //  56  float3 stream, stride 12
    uint   uvBufIdx;       //  60  float2 stream, stride 8 (may be invalid)

    // ---- Material proxy for analytic hit shading (material_surface.hlsli) ----
    //   albedo    = baseColor.rgb * albedoTex.rgb
    //   roughness = lerp(roughnessMin, roughnessMax, surfaceTex.g)
    //   metalness = lerp(metalnessMin, metalnessMax, surfaceTex.b)
    //   ao        = surfaceTex.r          (no texture -> white defaults)
    //   F0        = lerp(0.16*reflectance^2, albedo, metalness)
    // DDGI's own shading deliberately uses only baseColor and emissiveTexIdx.
    int    albedoTexIdx;   //  64  -1 = none
    int    surfaceTexIdx;  //  68  -1 = none (R=AO, G=roughness, B=metalness)
    float  roughnessMin;   //  72
    float  roughnessMax;   //  76
    float  metalnessMin;   //  80
    float  metalnessMax;   //  84
    float  reflectance;    //  88
    uint   indexBytes;     //  92  2 = R16_UINT (primitives), else 4 = R32_UINT

    // ---- De-interleaved stream base elements ----
    // Skinned meshes read pos / nrm from the shared SkinnedVertexRing arena, so their streams
    // start at a per-entity element base (vertexStart is the position base in that mode).
    // Primitives keep all bases 0, so the uniform (base + index) math is a no-op for them.
    // Interleaved fetches ignore all three.
    uint   nrmStart;       //  96  base element into the normal stream (float3)
    uint   uvStart;        // 100  base element into the uv stream (float2)
    uint   rtFlags;        // 104  RT_INST_FLAG_* bits; bits 16..31 = material slot + 1
    // NORMALMAP bindless index, -1 = none: analytic hit shading applies the normal map, so
    // off-screen reflections show it too. rt_hit_surface takes the authored tangent when
    // RT_INST_FLAG_INTERLEAVED_TANGENT is set; otherwise (and always in HybridReflIntersectHW)
    // the tangent is derived per triangle from the fetched positions and UVs.
    int    normalTexIdx;   // 108

    // ---- Material UV transform ----
    // uv' = uv * xy + zw, mirror of MaterialComponent::texMulAdd (the raster path's
    // MaterialGPUData::uvTilingOffset). Sample every material texture through
    // RTInstanceUV(), or a tiled material renders one way and reflects / shadows another.
    float4 uvTilingOffset; // 112
};                         // 128

// RT-side twin of MaterialUV() in material.hlsli; keep the two in lockstep.
float2 RTInstanceUV(RTInstanceData inst, float2 uv)
{
    return uv * inst.uvTilingOffset.xy + inst.uvTilingOffset.zw;
}

#endif // RT_INSTANCE_DATA_HLSLI
