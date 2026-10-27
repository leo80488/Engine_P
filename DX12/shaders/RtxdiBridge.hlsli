// RtxdiBridge.hlsli - the engine's RTXDI Application Bridge (RAB), the part
// shared by the DI, GI and PT cores.
// TODO(ReSTIR_Dual_Backend_Plan S8): after 2026-12-13 delete the RAB_DI_ENGINE / RAB_GI_ENGINE
// paths with the RTXDI DI / GI cores.
//
// Written for this engine: the vendored RTXDI-Library (shaders/Rtxdi/, unpacked
// from the RTXDI-Library zip in external/) is never modified, and the RTXDI
// sample's own bridge is not used. RTXDI calls RAB_* to read the engine's
// world; this file answers with the engine's own data and conventions:
//   * surfaces from the GBuffer (the same decode RTDI.cs.hlsl's LoadReceiver
//     uses) and, for the previous frame, from the bridge's own surface
//     ping-pong (32 B per pixel; the engine keeps no previous GBuffer);
//   * the engine's Cook-Torrance BRDF and BSDF sampling (brdf.hlsli,
//     bsdf_sample.hlsli), so an RTXDI core targets exactly the integrand the
//     engine core does;
//   * visibility: inline RayQuery against the scene TLAS with the engine's
//     alpha test, from the receiver lifted along its geometric normal;
//   * lights: GPULight records, current and previous, the stable-key index
//     map, emitter size (a point on the emitter's disk);
//   * motion and depth for temporal reuse.
// Core-specific hooks sit under RAB_DI_ENGINE / RAB_GI_ENGINE / RAB_PT below;
// RtxdiBridgePT.hlsli holds the path tracer's.
//
// The including TU provides, before this header:
//   cbuffer ... { RAB_BridgeConstants g_RabBridge; ... }   (RtxdiBridgeConstants.hlsli)
// and gets the engine's ray-tracing resources declared here in the RTDI shell
// root-signature layout (t0-t2 root SRVs, t0 space1 buffers, t0 space2
// textures, s1), unless it defines RAB_NO_ENGINE_RESOURCES.
//
// RAB_DI_ENGINE (the DI core TU, RtxdiDI.cs.hlsl, which includes RTDI.cs.hlsl
// first): RTXDI's surfaces are the RTDI shell's receivers on its trace grid,
// and the target, the integrand and the visibility rays are the engine DI
// core's own (EvaluateLight / LightTarget / TraceVisibility) - the two cores
// differ only in how they sample and reuse.
//
// RAB_GI_ENGINE (the GI core TU, RtxdiGI.cs.hlsl, which includes RTGI.cs.hlsl
// first): RTXDI's surfaces are the RTGI shell's receivers on its trace grid as
// Trace stored them (GISurface, this frame's and the previous frame's), the
// GI target is the engine core's (GITarget, with its visibility from the
// receiver unless RAB_GI_VISIBLE_TARGET is false), visibility is the engine
// GI's VisibleFrom. The TU provides RAB_GIOwnSample(cell, previousFrame,
// out position) before this header: the sample of the reservoir stored at a
// surface's cell, and whether it is known visible from that surface.
//
// RAB_PT (the PT core TU, RtxdiPT.cs.hlsl, which declares g_PT -
// RtxdiBridgePTConstants.hlsli - before this header): the engine path
// tracer's conventions - an unlit primary is no surface, visibility is
// PathTrace.cs.hlsl's shadow ray, the sun is a light (index
// kRabSunLightIndex, a cone around g_PT's direction), and light samples follow
// RTXDI's delta-light convention (solidAnglePdf 1, the attenuated intensity as
// the radiance) so a shift's Jacobian to a light is exactly 1.
#ifndef RTXDI_BRIDGE_HLSLI
#define RTXDI_BRIDGE_HLSLI

#include "RtxdiBridgeConstants.hlsli"
#include "cluster_common.hlsli"
#include "light_eval.hlsli"
#include "brdf.hlsli"
#include "bsdf_sample.hlsli"
#include "material.hlsli"
#include "core/packing.hlsli"   // OctEncode / OctDecode
#include "core/depth.hlsli"   // LinearizeReverseZ, WorldPosFromDepth
#include "core/gbuffer_codec.hlsli"   // GBufferDecodeMatIdx
#include "rt_instance_data.hlsli"
#include "rt_alpha_record.hlsli"
#include "Rtxdi/RtxdiParameters.h"
#include "Rtxdi/Utils/RandomSamplerState.hlsli"

#if !defined(RAB_NO_ENGINE_RESOURCES) && !defined(RAB_DI_ENGINE) && !defined(RAB_GI_ENGINE)
RaytracingAccelerationStructure g_TLAS : register(t0);
StructuredBuffer<RTInstanceData> g_Instances : register(t1);
StructuredBuffer<RTAlphaRecord> g_AlphaRecords : register(t2);
ByteAddressBuffer g_Buffers[16384] : register(t0, space1);
Texture2D g_AllTextures[16384] : register(t0, space2);
SamplerState gSampWrap : register(s1);
typedef RTInstanceData HRInstanceData;
#define HR_MaterialUV RTInstanceUV
// rt_hit_fetch.hlsli reads the alpha-record gate as cbuffer globals; here they
// live in the bridge constants.
#define alphaTestEnabled (g_RabBridge.alphaTestEnabled)
#define alphaInstanceCount (g_RabBridge.alphaInstanceCount)
#include "rt_hit_fetch.hlsli"
#ifdef RAB_PT
#include "rt_hit_surface.hlsli"   // the path tracer's hit-point material fetch (RTLoadHitSurface)
#include "core/bsdf_dielectric.hlsli"
#include "rt_glass.hlsli"         // a Transmissive instance as a path vertex, as in the reference path tracer
#include "rt_diffuse_transmission.hlsli"   // foliage and thin skin, as in the reference path tracer
#endif
#undef alphaTestEnabled
#undef alphaInstanceCount
#endif
#include "core/roughness_floor.hlsli"   // RT_ROUGHNESS_FLOOR, for the shells that do not include rt_hit_fetch.hlsli

// ==== Materials ====
struct RAB_Material
{
    float3 albedo;      // base colour (the engine's metal workflow: F0 below)
    float3 F0;
    float roughness;    // perceptual, >= RT_ROUGHNESS_FLOOR (0.045; the path tracer 0.01)
    float metal;
    float reflectance;  // the GBuffer's dielectric reflectance (F0 = 0.16 r^2)
    uint id;            // material table row
    // RAB_PT: a Transmissive instance's surface (rt_glass.hlsli; id is its InstanceID). Only a
    // continuation hit is one: the GBuffer never holds glass.
    bool glass;
    // RAB_PT: the light a TwoSidedFoliage / Subsurface surface lets through (rt_diffuse_transmission.hlsli)
    // and the probability a draw takes that lobe; thinQ 0 on every other surface.
    float3 thinTint;
    float thinQ;
};
RAB_Material RAB_EmptyMaterial() { return (RAB_Material)0; }

// ==== Surfaces ====
struct RAB_Surface
{
    float3 worldPos;
    float viewDepth;    // linear view depth (positive)
    float3 normal;      // shading normal
    float3 geoNormal;   // geometric: depth-reconstructed (DI core), Trace's (GI core), else the shading normal
    float3 viewDir;     // surface -> camera
    RAB_Material material;
    bool valid;
    // The full-resolution pixel of a current-frame surface (hasPixel): the DI
    // core reconstructs the geometric normal there on demand (hasNg once it did).
    uint2 pixel;
    bool hasPixel;
    bool hasNg;
#ifdef RAB_GI_ENGINE
    // The sample of the reservoir stored at this surface's cell, and whether
    // it is known visible from here (Trace hit it, this frame's resampling
    // found it visible, or the history validation re-traced it): the GI
    // target's visibility ray is skipped for it.
    float3 ownSample;
    bool ownVisible;
#endif
};
RAB_Surface RAB_EmptySurface() { return (RAB_Surface)0; }
bool RAB_IsSurfaceValid(RAB_Surface s) { return s.valid; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface s) { return s.worldPos; }
float3 RAB_GetSurfaceNormal(RAB_Surface s) { return s.normal; }
float RAB_GetSurfaceLinearDepth(RAB_Surface s) { return s.viewDepth; }
float3 RAB_GetSurfaceViewDir(RAB_Surface s) { return s.viewDir; }
// Glass reads as smooth: its events are deltas to the reconnection rules (RAB_SampleBrdfRay), and
// a fixed-threshold reconnection must not pick it either.
float RAB_GetSurfaceRoughness(RAB_Surface s) { return s.material.glass ? 0.0 : s.material.roughness; }
void RAB_SetSurfaceNormal(inout RAB_Surface s, float3 n) { s.normal = n; }
void RAB_SetSurfaceWorldPos(inout RAB_Surface s, float3 p) { s.worldPos = p; }
RAB_Material RAB_GetMaterial(RAB_Surface s) { return s.material; }
// The engine core's history test: the same material row, roughness close.
bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    return a.id == b.id && abs(a.roughness - b.roughness) <= 0.5 * max(a.roughness, b.roughness);
}

#ifdef RAB_PT
// The PT core's BSDF: the engine path tracer's (RTEvalBSDF / RTBsdfPdf), or
// its Lambertian arm under -pt_diffuse (PathTrace.cs.hlsl's RTEvaluate /
// RTPdf with flags & 1).
// The scene's glass can be read this frame: the TLAS holds a Transmissive instance (kRabPTGlass)
// and the path material table its fields. Glass is then a path vertex, and light that crosses it
// is weighed by RAB_GlassTransmittance instead of stopped.
bool RAB_PTGlassScene() { return (g_PT.ptFlags & kRabPTGlass) != 0 && g_PT.pathMaterialsIdx != 0; }
// A glass surface's vertex (rt_glass.hlsli): its viewDir points back along the ray that found it.
RTGlass RAB_PTGlass(RAB_Surface s)
{
    return RTGlassAtHit(g_PT.pathMaterialsIdx, s.material.id, s.material.albedo, s.material.roughness,
                        s.material.metal, s.geoNormal, -s.viewDir);
}
// The lobe behind a surface (inactive when thinQ is 0), and setting it from a material - where a
// surface is made: the GBuffer's (current and previous) and a continuation hit's.
RTThin RAB_PTThin(RAB_Surface s)
{
    RTThin t = (RTThin)0;
    t.active = s.material.thinQ > 0;
    t.tint = s.material.thinTint;
    t.q = s.material.thinQ;
    return t;
}
void RAB_PTSetThin(inout RAB_Surface s, MaterialGPUData m)
{
    RTThin t = (RTThin)0;
    if (!(g_PT.ptFlags & kRabPTDiffuse)) t = RTThinAt(m, s.material.albedo, s.material.metal);
    s.material.thinTint = t.tint;
    s.material.thinQ = t.active ? t.q : 0;
}
// A glass surface weighs a light or a reconnection as the reference path tracer's light sample does:
// its reflection side (RTGlassLightEval); its density is every event's. A leaf weighs one behind it.
float3 RAB_PTBsdfCos(RAB_Surface s, float3 l)
{
    if (g_PT.ptFlags & kRabPTDiffuse) return s.material.albedo * (saturate(dot(s.normal, l)) / PI);
    if (s.material.thinQ > 0)
        return RTThinEval(RAB_PTThin(s), s.normal, s.viewDir, l, s.material.albedo, s.material.roughness,
                          s.material.metal, s.material.reflectance);
    if (s.material.glass)
        return RTGlassLightEval(RAB_PTGlass(s), s.normal, s.viewDir, l, s.material.albedo, s.material.roughness,
                                s.material.metal, s.material.reflectance);
    return RTEvalBSDF(s.normal, s.viewDir, l, s.material.albedo, s.material.roughness, s.material.metal, s.material.reflectance);
}
float RAB_PTScatterPdf(RAB_Surface s, float3 l)
{
    if (g_PT.ptFlags & kRabPTDiffuse) return saturate(dot(s.normal, l)) / PI;
    if (s.material.thinQ > 0) return RTThinPdf(RAB_PTThin(s), s.normal, s.viewDir, l, s.material.roughness);
    if (s.material.glass) return RTGlassMixturePdf(RAB_PTGlass(s), s.normal, s.viewDir, l, s.material.roughness);
    return RTBsdfPdf(s.normal, s.viewDir, l, s.material.roughness);
}
#endif

float3 RAB_ViewToWorldPosition(float2 uv, float depth)
{
    return WorldPosFromDepth(uv, depth, g_RabBridge.invViewProj);
}

#ifdef RAB_DI_ENGINE
// The DI core: a cell of the shell's trace grid is the receiver at its full
// pixel (FullPixel: half resolution with the 2x2 phase, or full), with the
// shell's eligibility - an ineligible receiver is no surface.
RAB_Surface RAB_SurfaceFromReceiver(Receiver r)
{
    RAB_Surface s = RAB_EmptySurface();
    if (!r.eligible) return s;
    s.worldPos = r.P;
    s.viewDepth = r.viewZ;
    s.normal = r.N;
    s.geoNormal = r.hasNg ? r.Ng : r.N;
    s.hasNg = r.hasNg;
    s.viewDir = r.V;
    s.material.albedo = r.albedo;
    s.material.F0 = r.F0;
    s.material.roughness = r.rough;
    s.material.metal = r.metal;
    s.material.id = r.material;
    s.pixel = r.pixel;
    s.hasPixel = r.hasPixel;
    s.valid = true;
    return s;
}
Receiver RAB_SurfaceToReceiver(RAB_Surface s)
{
    Receiver r = (Receiver)0;
    r.P = s.worldPos; r.N = s.normal; r.V = s.viewDir;
    r.albedo = s.material.albedo; r.F0 = s.material.F0;
    r.rough = s.material.roughness; r.metal = s.material.metal;
    r.viewZ = s.viewDepth; r.material = s.material.id; r.eligible = s.valid;
    r.pixel = s.pixel; r.hasPixel = s.hasPixel;
    r.Ng = s.geoNormal; r.hasNg = s.hasNg;
    return r;
}
RAB_Surface RAB_LoadCurrentSurface(int2 cell)
{
    if (any(cell < 0) || cell.x >= int(traceW) || cell.y >= int(traceH)) return RAB_EmptySurface();
    const uint2 px = FullPixel(uint2(cell));
    RAB_Surface s = RAB_SurfaceFromReceiver(LoadReceiver(px));
    if (s.valid)
    {
        // The receiver carries F0; the previous-surface record stores the
        // reflectance it came from.
        Texture2D<float4> surfaceTex = ResourceDescriptorHeap[surfaceIdx];
        s.material.reflectance = surfaceTex.Load(int3(px, 0)).a;
    }
    return s;
}
#elif defined(RAB_GI_ENGINE)
// The GI core: a cell of the shell's trace grid is the GISurface record of its
// receiver - the point lifted along the geometric normal (every connection's
// origin), the shading and geometric normals, the material row, the albedo;
// count 0 = no eligible receiver - this frame's (surfaceSrv, after the core's
// coverage update) or the previous frame's (previousSurfaceSrv): the engine
// core's receiver ping-pong, so both cores resample for the same receivers.
RAB_Surface RAB_SurfaceFromGI(GISurface g, float4x4 toView, float3 eye)
{
    RAB_Surface s = RAB_EmptySurface();
    if (GISurfaceCount(g) == 0) return s;
    s.worldPos = g.P;
    s.viewDepth = abs(mul(float4(g.P, 1), toView).z);
    s.normal = GIUnpackNormal(g.normal);
    s.geoNormal = GIUnpackNormal(g.geoNormal);
    s.viewDir = normalize(eye - g.P);
    s.material.albedo = GIUnpackAlbedo(g.albedo);
    s.material.roughness = 1;   // the GI lobe is diffuse
    s.material.id = g.material;
    s.valid = true;
    return s;
}
Receiver RAB_SurfaceToGIReceiver(RAB_Surface s)
{
    Receiver r = (Receiver)0;
    r.P = s.worldPos; r.N = s.normal; r.Ng = s.geoNormal; r.albedo = s.material.albedo;
    r.viewZ = s.viewDepth; r.material = s.material.id; r.eligible = s.valid;
    return r;
}
bool RAB_InTraceGrid(int2 cell) { return all(cell >= 0) && cell.x < int(traceW) && cell.y < int(traceH); }
RAB_Surface RAB_LoadCurrentSurface(int2 cell)
{
    if (!RAB_InTraceGrid(cell)) return RAB_EmptySurface();
    StructuredBuffer<GISurface> surfaces = ResourceDescriptorHeap[surfaceSrv];
    RAB_Surface s = RAB_SurfaceFromGI(surfaces[cell.y * traceW + cell.x], worldToView, camera);
    if (s.valid) s.ownVisible = RAB_GIOwnSample(cell, false, s.ownSample);
    return s;
}
// The previous frame's receivers (RTXDI's temporal taps): the depth through
// the previous view, as RAB_ScreenSpaceMotion predicts it.
RAB_Surface RAB_LoadPreviousSurface(int2 cell)
{
    if (!RAB_InTraceGrid(cell) || previousSurfaceSrv == 0) return RAB_EmptySurface();
    StructuredBuffer<GISurface> surfaces = ResourceDescriptorHeap[previousSurfaceSrv];
    RAB_Surface s = RAB_SurfaceFromGI(surfaces[cell.y * traceW + cell.x], g_RabBridge.prevWorldToView, g_RabBridge.prevCamera);
    if (s.valid) s.ownVisible = RAB_GIOwnSample(cell, true, s.ownSample);
    return s;
}
#else
// The current frame's surface at a full-resolution pixel: RTDI.cs.hlsl's
// LoadReceiver decode (reverse-Z, sky = depth 0; the normal's alpha is the
// material row; surface.r/g/a = roughness / metal / reflectance).
RAB_Surface RAB_LoadCurrentSurface(int2 pixel)
{
    RAB_Surface s = RAB_EmptySurface();
    if (any(pixel < 0) || pixel.x >= int(g_RabBridge.width) || pixel.y >= int(g_RabBridge.height)) return s;
    Texture2D<float> depths = ResourceDescriptorHeap[g_RabBridge.depthIdx];
    const float depth = depths.Load(int3(pixel, 0));
    if (depth <= 0) return s;
    Texture2D<float4> normals = ResourceDescriptorHeap[g_RabBridge.normalIdx];
    Texture2D<float4> albedos = ResourceDescriptorHeap[g_RabBridge.albedoIdx];
    Texture2D<float4> surfaces = ResourceDescriptorHeap[g_RabBridge.surfaceIdx];
    const float4 normal = normals.Load(int3(pixel, 0));
    const float4 surface = surfaces.Load(int3(pixel, 0));
    const float2 uv = (float2(pixel) + 0.5) / float2(g_RabBridge.width, g_RabBridge.height);
    s.worldPos = RAB_ViewToWorldPosition(uv, depth);
    s.normal = normalize(normal.xyz * 2 - 1);
    s.geoNormal = s.normal;
    s.viewDir = normalize(g_RabBridge.camera - s.worldPos);
    s.viewDepth = abs(mul(float4(s.worldPos, 1), g_RabBridge.worldToView).z);
    s.material.id = GBufferDecodeMatIdx(normal.a);
    s.material.albedo = albedos.Load(int3(pixel, 0)).rgb;
    s.material.roughness = max(surface.r, RT_ROUGHNESS_FLOOR);
    s.material.metal = surface.g;
    s.material.reflectance = surface.a;
    s.material.F0 = lerp((0.16 * surface.a * surface.a).xxx, s.material.albedo, s.material.metal);
    s.pixel = uint2(pixel);
    s.hasPixel = true;
#ifdef RAB_PT
    // An unlit primary does not scatter: no surface for RTXDI (the PT core's
    // shade pass writes its base colour, as PathTrace.cs.hlsl does).
    if (g_RabBridge.materialsIdx != 0)
    {
        StructuredBuffer<MaterialGPUData> materials = ResourceDescriptorHeap[g_RabBridge.materialsIdx];
        if (materials[s.material.id].customShadingModel == SHADING_MODEL_UNLIT) return RAB_EmptySurface();
        RAB_PTSetThin(s, materials[s.material.id]);
    }
#endif
    s.valid = true;
    return s;
}
#endif

// The bridge's surface ping-pong: 32 bytes per pixel of the surface grid -
// position (3 floats), octahedral normal, material row, roughness | metal
// (2 x 16-bit unorm), albedo rgb + reflectance (4 x 8-bit unorm). The engine
// keeps no previous GBuffer, so a core stores its receivers here every frame
// and reads the previous frame's back (RAB_GetGBufferSurface(.., true)).
uint RAB_PackUnorm16x2(float a, float b)
{
    return uint(round(saturate(a) * 65535)) | (uint(round(saturate(b) * 65535)) << 16);
}
uint RAB_PackUnorm8x4(float4 v)
{
    const uint4 q = uint4(round(saturate(v) * 255));
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}
float4 RAB_UnpackUnorm8x4(uint p)
{
    return float4(p & 255u, (p >> 8) & 255u, (p >> 16) & 255u, p >> 24) / 255.0;
}
void RAB_StoreSurface(int2 cell, RAB_Surface s)
{
    if (g_RabBridge.surfaceOutUav == 0 || any(cell < 0) ||
        cell.x >= int(g_RabBridge.surfaceGridWidth) || cell.y >= int(g_RabBridge.surfaceGridHeight)) return;
    RWStructuredBuffer<uint4> surfaces = ResourceDescriptorHeap[g_RabBridge.surfaceOutUav];
    const uint i = (uint(cell.y) * g_RabBridge.surfaceGridWidth + uint(cell.x)) * 2;
    const uint2 oct = uint2(round(saturate(OctEncode(s.normal) * 0.5 + 0.5) * 65535));
    surfaces[i] = uint4(asuint(s.worldPos), s.valid ? (oct.x | (oct.y << 16)) : 0xffffffffu);
    surfaces[i + 1] = uint4(s.material.id, RAB_PackUnorm16x2(s.material.roughness, s.material.metal),
                            RAB_PackUnorm8x4(float4(s.material.albedo, s.material.reflectance)), asuint(s.viewDepth));
}
#ifndef RAB_GI_ENGINE   // the GI core reads the shell's receiver history (above)
RAB_Surface RAB_LoadPreviousSurface(int2 cell)
{
    RAB_Surface s = RAB_EmptySurface();
    if (g_RabBridge.prevSurfaceSrv == 0 || any(cell < 0) ||
        cell.x >= int(g_RabBridge.surfaceGridWidth) || cell.y >= int(g_RabBridge.surfaceGridHeight)) return s;
    StructuredBuffer<uint4> surfaces = ResourceDescriptorHeap[g_RabBridge.prevSurfaceSrv];
    const uint i = (uint(cell.y) * g_RabBridge.surfaceGridWidth + uint(cell.x)) * 2;
    const uint4 a = surfaces[i], b = surfaces[i + 1];
    if (a.w == 0xffffffffu) return s;
    s.worldPos = asfloat(a.xyz);
    s.normal = OctDecode(float2(a.w & 0xffffu, a.w >> 16) / 65535.0 * 2 - 1);
    s.geoNormal = s.normal;
    s.viewDir = normalize(g_RabBridge.prevCamera - s.worldPos);
    s.viewDepth = asfloat(b.w);
    s.material.id = b.x;
    s.material.roughness = max((b.y & 0xffffu) / 65535.0, RT_ROUGHNESS_FLOOR);
    s.material.metal = (b.y >> 16) / 65535.0;
    const float4 albedo = RAB_UnpackUnorm8x4(b.z);
    s.material.albedo = albedo.rgb;
    s.material.reflectance = albedo.a;
    s.material.F0 = lerp((0.16 * albedo.a * albedo.a).xxx, albedo.rgb, s.material.metal);
#ifdef RAB_PT
    if (g_RabBridge.materialsIdx != 0)
    {
        StructuredBuffer<MaterialGPUData> materials = ResourceDescriptorHeap[g_RabBridge.materialsIdx];
        RAB_PTSetThin(s, materials[s.material.id]);
    }
#endif
    s.valid = true;
    return s;
}
#endif
// RTXDI addresses surfaces by pixel; the cores call it on their own grid
// (full resolution, or half with the DI shell's 2x2 phase, or the GI shell's
// trace grid).
RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame)
{
    if (previousFrame) return RAB_LoadPreviousSurface(pixelPosition);
    return RAB_LoadCurrentSurface(pixelPosition);
}
int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame)
{
#if defined(RAB_DI_ENGINE) || defined(RAB_GI_ENGINE)
    return clamp(pixelPosition, int2(0, 0), int2(traceW, traceH) - 1);
#else
    return clamp(pixelPosition, int2(0, 0), int2(g_RabBridge.width, g_RabBridge.height) - 1);
#endif
}

// ==== Motion and depth ====
// RTXDI's screenSpaceMotion: (previous - current) pixel position in xy, the
// previous linear depth minus the current one in z. The engine's velocity is
// an NDC delta (the NRD path scales it by (-1/2, 1/2) into UV); z comes from
// the previous view matrix applied to today's position (exact for static
// geometry, as RTXDI's samples compute it).
#if defined(RAB_DI_ENGINE) || defined(RAB_GI_ENGINE)
// The DI and GI cores: the engine core's own reprojection of a trace cell
// (RTPreviousTraceCoordinate with the shell's jitter delta, and DI's phase
// delta), as the offset RTXDI adds before rounding.
float3 RAB_ScreenSpaceMotion(int2 cell, RAB_Surface s)
{
    Texture2D<float2> velocityTex = ResourceDescriptorHeap[velocityIdx];
    const float2 mv = velocityTex.Load(int3(FullPixel(uint2(cell)), 0)) * float2(-.5, .5);
    const float stride = (flags & 4u) ? 1 : 2;
    const float2 previous = float2(RTPreviousTraceCoordinate(cell.x, mv.x, width, stride, historyJitterDelta.x),
                                   RTPreviousTraceCoordinate(cell.y, mv.y, height, stride, historyJitterDelta.y));
    const float prevDepth = abs(mul(float4(s.worldPos, 1), g_RabBridge.prevWorldToView).z);
    return float3(previous - float2(cell), prevDepth - s.viewDepth);
}
#else
float3 RAB_ScreenSpaceMotion(int2 pixel, RAB_Surface s)
{
    Texture2D<float2> velocity = ResourceDescriptorHeap[g_RabBridge.velocityIdx];
    const float2 v = velocity.Load(int3(pixel, 0)) * float2(-0.5, 0.5);
    const float prevDepth = abs(mul(float4(s.worldPos, 1), g_RabBridge.prevWorldToView).z);
    return float3(v * float2(g_RabBridge.width, g_RabBridge.height), prevDepth - s.viewDepth);
}
#endif

// ==== Visibility ====
// One shadow ray, as RTDI's TraceVisibility traces it: from the receiver
// lifted along its geometric normal by a view-scaled offset, the alpha-tested
// shadow instances (mask 0x02), first hit ends the search.
#ifdef RAB_DI_ENGINE
// The DI core: the engine core's TraceVisibility (lifted origin, -di_debug's
// no-visibility mode, the Visibility counter slot). g_RabOccluded (per thread)
// records a refused ray: the initial pass reads it to count RTXDI's discarded
// selections, which the reservoir alone cannot tell from "nothing selected".
static bool g_RabOccluded = false;
bool RAB_TraceVisibility(RAB_Surface s, float3 target)
{
    const float3 toTarget = target - s.worldPos;
    const float distance = length(toTarget);
    if (!(distance > 1e-4)) return true;
    const bool visible = TraceVisibility(RAB_SurfaceToReceiver(s), toTarget / distance, distance, 1) > 0;
    g_RabOccluded = g_RabOccluded || !visible;
    return visible;
}
#elif defined(RAB_GI_ENGINE)
// The GI core: the engine GI's VisibleFrom - from the lifted receiver, the
// geometric horizon, the range, the GI instance mask, -gi_no_validation's
// no-ray mode, counted in the SpatialVisibility slot.
bool RAB_TraceVisibility(RAB_Surface s, float3 target)
{
    return VisibleFrom(RAB_SurfaceToGIReceiver(s), target);
}
#elif defined(RAB_PT)
// The PT core: PathTrace.cs.hlsl's RTVisibility - from the vertex pushed 1 mm
// along its shading normal, to 2 mm short of the target, at most 10 km (the
// sun's far point), the alpha-tested shadow instances. Below the shading
// horizon every engine BSDF is 0: no ray.
bool RAB_TraceVisibility(RAB_Surface s, float3 target)
{
    const float3 toTarget = target - s.worldPos;
    const float distance = length(toTarget);
    if (!(distance > 1e-4)) return true;
    const float3 l = toTarget / distance;
    // Below the shading horizon only a leaf (thinQ) lets light through, from its back.
    if (dot(s.normal, l) <= 0 && !(s.material.thinQ > 0)) return false;
    RayDesc ray;
    ray.Origin = s.worldPos + (dot(s.normal, l) >= 0 ? s.normal : -s.normal) * 0.001;
    ray.Direction = l;
    ray.TMin = 0.001;
    ray.TMax = max(min(distance - 0.002, 10000.0), ray.TMin + 1e-4);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    // Glass passes: what it lets through is the light's (RAB_GlassTransmittance), not visibility.
    const bool glassScene = RAB_PTGlassScene();
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            !(glassScene && RTGlassInstance(q.CandidateInstanceID())) &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus() == COMMITTED_NOTHING;
}
// What reaches s from target through the glass on the way (RTGlassCrossing's product; 1 without
// glass in the scene). Opaque occluders are visibility's: this ray commits nothing. Every segment
// the bridge evaluates - a light sample, a reconnection - takes it, so the initial path, a shift
// and a resampled neighbour weigh light through glass alike.
float3 RAB_GlassTransmittance(RAB_Surface s, float3 target)
{
    if (!RAB_PTGlassScene()) return 1;
    const float3 toTarget = target - s.worldPos;
    const float distance = length(toTarget);
    if (!(distance > 1e-4)) return 1;
    RayDesc ray;
    ray.Direction = toTarget / distance;
    ray.Origin = s.worldPos + (dot(s.geoNormal, ray.Direction) >= 0 ? s.geoNormal : -s.geoNormal) * 0.001;
    ray.TMin = 0.001;
    ray.TMax = max(min(distance - 0.002, 10000.0), ray.TMin + 1e-4);
    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    float3 t = 1;
    while (q.Proceed())
    {
        if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE || !RTGlassInstance(q.CandidateInstanceID())) continue;
        t *= RTGlassCrossing(g_PT.pathMaterialsIdx, q.CandidateInstanceID(), q.CandidatePrimitiveIndex(),
                             q.CandidateTriangleBarycentrics(), (float3x3)q.CandidateWorldToObject3x4(), ray.Direction);
    }
    return t;
}
// The fog on a segment (the kernels PTRtxdiCore builds with RAB_MEDIA while the frame has a medium).
// A light segment - a light sample, a shift's re-evaluation of one - keeps the whole transmittance;
// a continuation segment - a BSDF ray, a reconnection to the vertex one found - keeps what
// g_PT.mediaFlags says, as no medium vertex returns the light the fog scatters into it; the radiance
// cache's paths (kRabSegmentCache) keep it too, but absorption under the froxel rule (the cache holds a
// surface's light, not the lit volume's).
// Deterministic (the local volumes marched): a shift and the path it replays weigh the same fog.
static const uint kRabSegmentLight = 0u, kRabSegmentContinuation = 1u, kRabSegmentCache = 2u;
#if RAB_MEDIA
#include "core/participating_media.hlsli"
MediumLocal MediumLoadLocal(uint index)
{
    StructuredBuffer<float4> words = ResourceDescriptorHeap[g_PT.mediumIdx];
    const uint base = 6u * (index + 1u);
    const float4 r3 = words[base + 3], r4 = words[base + 4], r5 = words[base + 5];
    MediumLocal v;
    v.row0 = words[base]; v.row1 = words[base + 1]; v.row2 = words[base + 2];
    v.albedo = r3.xyz; v.density = r3.w;
    v.emission = r4.xyz; v.edgeFade = r4.w;
    v.shape = asuint(r5.x); v.heightFalloff = r5.y; v.halfHeight = r5.z; v._pad = r5.w;
    return v;
}
MediumSet RAB_MediumSet()
{
    StructuredBuffer<float4> words = ResourceDescriptorHeap[g_PT.mediumIdx];
    const float4 r0 = words[0], r1 = words[1], r2 = words[2], r3 = words[3], r4 = words[4], r5 = words[5];
    MediumSet s;
    s.global.albedo = r0.xyz; s.global.density = r0.w;
    s.global.emission = r1.xyz; s.global.falloff = r1.w;
    s.global.baseHeight = r2.x; s.global.anisotropy = r2.y; s.global.reach = r2.z; s.global._pad = r2.w;
    s.height.albedo = r3.xyz; s.height.density = r3.w;
    s.height.falloff = r4.x; s.height.height = r4.y; s.height.anisotropy = r4.z; s.height._pad = r4.w;
    s.localCount = asuint(r5.x); s.camera = r5.yzw; s.global.reachCenter = r5.yzw;
    return s;
}
// kRabMediaFroxel: the froxel fog's last lit volume (g_PT.froxel*: what VolumetricFogPass lit last
// frame for the view it was lit for). A continuation segment's part inside it keeps its scattering
// as well, and takes the light the volume holds there - sigma_s x the in-scattered light (with the
// phase toward that view's camera: biased where the fog is not isotropic) and the medium's
// emission; the rest of the segment keeps the absorption rule. kRabFroxelSteps deterministic steps
// over the part within froxelFar of the volume's camera, so a shift and the path it replays weigh
// the same segment alike.
static const uint kRabFroxelSteps = 16u;
struct RAB_FroxelPart
{
    float3 inScatter;   // the light the volume scatters toward o along the segment, through the
                        // rule's transmittance from o
    float3 tauS;        // the scattering depth of the part inside the volume
    float  tExit;       // where the segment leaves the sphere of radius froxelFar (0: never in it)
};
// The volume's UVW at p (the slice of its distance to the volume's camera), false outside it.
bool RAB_FroxelUVW(float3 p, out float3 uvw)
{
    const float4 clip = mul(float4(p, 1.0), g_PT.froxelViewProj);
    const float r = length(p - g_PT.froxelCamera);
    uvw = float3(clip.x / clip.w * 0.5 + 0.5, -clip.y / clip.w * 0.5 + 0.5,
                 log(max(r, 1e-4) / g_PT.froxelNear) / log(g_PT.froxelFar / g_PT.froxelNear));
    return clip.w > 0.0 && all(uvw >= 0.0) && all(uvw <= 1.0);
}
RAB_FroxelPart RAB_FroxelSegment(MediumSet m, float3 o, float3 d, float tMax, bool inScatter)
{
    RAB_FroxelPart r = (RAB_FroxelPart)0;
    if (g_PT.froxelHistoryIdx == 0u) return r;
    const float3 oc = o - g_PT.froxelCamera;
    const float b = dot(oc, d);
    const float disc = b * b - (dot(oc, oc) - g_PT.froxelFar * g_PT.froxelFar);
    if (!(disc > 0.0)) return r;
    const float root = sqrt(disc);
    const float ta = max(-b - root, 0.0), tb = min(-b + root, tMax);
    if (!(tb > ta)) return r;
    r.tExit = tb;
    Texture3D<float4> volume = ResourceDescriptorHeap[g_PT.froxelHistoryIdx];
    uint3 dims;
    volume.GetDimensions(dims.x, dims.y, dims.z);
    // Inside half a texel of the edges the wrap sampler filters like a clamped one.
    const float3 lo = 0.5 / float3(dims), hi = 1.0 - lo;
    const float dt = (tb - ta) / float(kRabFroxelSteps);
    float3 T = inScatter ? MediumAbsorptionTransmittance(m, o, d, 0.0, ta) : float3(1, 1, 1);
    [loop] for (uint i = 0u; i < kRabFroxelSteps; ++i)
    {
        const float3 p = o + d * (ta + (float(i) + 0.5) * dt);
        float3 uvw;
        const bool inside = RAB_FroxelUVW(p, uvw);
        const MediumPoint mp = MediumAt(m, p);
        if (inside) r.tauS += mp.sigmaS * dt;
        if (!inScatter) continue;
        // The rule's extinction over the step: all of it inside the volume, absorption outside.
        const float3 sigma = inside ? mp.sigmaT.xxx : max(mp.sigmaT - mp.sigmaS, 0.0);
        const float3 Ti = exp(-sigma * dt);
        if (inside)
        {
            const float3 S = volume.SampleLevel(gSampWrap, clamp(uvw, lo, hi), 0).rgb;
            r.inScatter += T * S * (1.0 - Ti) / max(sigma, 1e-6);
        }
        T *= Ti;
    }
    return r;
}
// The fog over o + t d, t in [0, tMax] (kMediumFar: to the sky), by @p kind's rule.
float3 RAB_FogTransmittance(float3 o, float3 d, float tMax, uint kind)
{
    if (g_PT.mediumIdx == 0) return 1;
    const uint rule = kind == kRabSegmentLight ? kRabMediaFull
                    : kind == kRabSegmentCache && g_PT.mediaFlags == kRabMediaFroxel ? kRabMediaAbsorption
                    : g_PT.mediaFlags;
    if (rule == kRabMediaNone) return 1;
    const MediumSet m = RAB_MediumSet();
    if (rule == kRabMediaAbsorption) return MediumAbsorptionTransmittance(m, o, d, 0, tMax);
    if (rule == kRabMediaFroxel)
    {
        // To the sky only as far as the volume's sphere: the sky sample takes the rest (RAB_PathTrace).
        const RAB_FroxelPart f = RAB_FroxelSegment(m, o, d, tMax, false);
        return MediumAbsorptionTransmittance(m, o, d, 0, tMax >= kMediumFar ? f.tExit : tMax) * exp(-f.tauS);
    }
    return MediumTransmittanceDeterministic(m, o, d, 0, tMax);
}
// A continuation segment's fog (RAB_PathTrace), and under the froxel rule the light the lit volume
// scatters into it toward o (@p inScatter, through the rule's transmittance from o) and, to the sky,
// the fog past the volume's sphere, which the sky's light takes (@p skyRest).
float3 RAB_FogContinuation(float3 o, float3 d, float tMax, out float3 inScatter, out float3 skyRest)
{
    inScatter = 0;
    skyRest = 1;
    if (g_PT.mediumIdx == 0 || g_PT.mediaFlags != kRabMediaFroxel)
        return RAB_FogTransmittance(o, d, tMax, kRabSegmentContinuation);
    const MediumSet m = RAB_MediumSet();
    const RAB_FroxelPart f = RAB_FroxelSegment(m, o, d, tMax, true);
    inScatter = f.inScatter;
    if (tMax >= kMediumFar)
    {
        skyRest = MediumAbsorptionTransmittance(m, o, d, f.tExit, kMediumFar);
        return MediumAbsorptionTransmittance(m, o, d, 0, f.tExit) * exp(-f.tauS);
    }
    return MediumAbsorptionTransmittance(m, o, d, 0, tMax) * exp(-f.tauS);
}
// The fog from s to target. A target at the sun's distance (RAB_TraceVisibility's 10 km and beyond)
// is the sky: the fog runs to infinity, as on the reference path tracer's shadow rays.
float3 RAB_FogToward(RAB_Surface s, float3 target, uint kind)
{
    const float3 toTarget = target - s.worldPos;
    const float distance = length(toTarget);
    if (!(distance > 1e-4)) return 1;
    return RAB_FogTransmittance(s.worldPos, toTarget / distance, distance >= 10000.0 ? kMediumFar : distance, kind);
}
// What reaches s from target on a segment of @p kind: the glass on the way and the fog.
float3 RAB_SegmentTransmittance(RAB_Surface s, float3 target, uint kind)
{
    float3 t = RAB_GlassTransmittance(s, target);
    if (any(t > 0)) t *= RAB_FogToward(s, target, kind);
    return t;
}
#else
// Without the fog, the glass alone. An alias, not a function: the extra call level reorders the plain
// kernels' DXIL (tools/dxil_ab.py).
#define RAB_SegmentTransmittance(s, target, kind) RAB_GlassTransmittance(s, target)
#endif
#else
bool RAB_TraceVisibility(RAB_Surface s, float3 target)
{
    const float3 toTarget = target - s.worldPos;
    const float distance = length(toTarget);
    if (!(distance > 1e-4)) return true;
    const float3 l = toTarget / distance;
    if (dot(s.geoNormal, l) <= 0) return false;
    const float lift = 1e-3 + 1e-4 * s.viewDepth;
    RayDesc ray;
    ray.Origin = s.worldPos + s.geoNormal * lift;
    ray.Direction = l;
    ray.TMin = 1e-4 + 1e-5 * s.viewDepth;
    ray.TMax = min(g_RabBridge.rayTMax, max(distance - lift - 0.002, ray.TMin + 1e-4));
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0x02, ray);
    while (q.Proceed())
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            HR_AlphaAccept(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), false))
            q.CommitNonOpaqueTriangleHit();
    return q.CommittedStatus() == COMMITTED_NOTHING;
}
#endif

// ==== Lights ====
// RAB_LightInfo is the engine's GPULight (64 bytes): type 0 directional (never
// a local light), 1 point, 2 spot; sourceRadius > 0 makes a point / spot a
// sphere emitter for visibility.
struct RAB_LightInfo
{
    GPULight light;
    bool valid;
    bool sun;           // RAB_PT: the engine's sun (kRabSunLightIndex), not a list entry
};
#ifdef RAB_PT
// The sun as an RTXDI light: an index no light list reaches (ClusterPass keeps
// at most 4096), the cone and colour from g_PT.
static const uint kRabSunLightIndex = 0x7ffffffeu;
#ifndef RAB_DISTANT_LIGHT_DISTANCE
#define RAB_DISTANT_LIGHT_DISTANCE 1.0e5f   // RtxdiBridgePT.hlsli: the fake distance of a distant vertex
#endif
bool RAB_SunEnabled() { return !(g_PT.ptFlags & kRabPTNoSunNee) && any(g_PT.sunColor > 0); }
#endif
RAB_LightInfo RAB_EmptyLightInfo() { return (RAB_LightInfo)0; }
float RAB_LightSourceRadius(RAB_LightInfo info) { return asfloat(info.light.sourceRadiusBits); }
float RAB_LightPower(RAB_LightInfo info)
{
    if (!info.valid || info.light.type == 0u) return 0;
    const float p = asfloat(info.light.powerBits);
    return isfinite(p) && p > 0 ? p : 0;
}
RAB_LightInfo RAB_LoadLightInfo(uint index, bool previousFrame)
{
    RAB_LightInfo info = RAB_EmptyLightInfo();
#ifdef RAB_PT
    if (index == kRabSunLightIndex)
    {
        // Today's sun for both frames: the engine keeps no previous one.
        info.light.direction = g_PT.sunDirection;
        info.light.color = g_PT.sunColor;
        info.light.intensity = 1;
        info.sun = true;
        info.valid = RAB_SunEnabled();
        return info;
    }
#endif
    const uint count = previousFrame ? g_RabBridge.prevLightCount : g_RabBridge.lightCount;
    const uint idx = previousFrame ? g_RabBridge.prevLightsIdx : g_RabBridge.lightsIdx;
    if (index >= count || idx == 0) return info;
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[idx];
    info.light = lights[index];
    info.valid = true;
    return info;
}
// ClusterPass's light map: [0,4096) previous -> current, [4096,8192)
// current -> previous, ~0 where a light is gone / new; no map (lightMapIdx 0)
// = the list kept its keys and order.
static const uint kRabLightMapCurrentOffset = 4096u;   // mirror: ClusterPass::kMaxLights
int RAB_TranslateLightIndex(uint lightIndex, bool currentToPrevious)
{
#ifdef RAB_PT
    if (lightIndex == kRabSunLightIndex) return int(lightIndex);
#endif
    if (g_RabBridge.lightMapIdx == 0) return int(lightIndex);
    const uint count = currentToPrevious ? g_RabBridge.lightCount : g_RabBridge.prevLightCount;
    if (lightIndex >= count) return -1;
    StructuredBuffer<uint> map = ResourceDescriptorHeap[g_RabBridge.lightMapIdx];
    const uint mapped = map[(currentToPrevious ? kRabLightMapCurrentOffset : 0u) + lightIndex];
    return mapped == 0xffffffffu ? -1 : int(mapped);
}
// RTXDI's presampling keeps a copy of each RIS entry's light next to it
// (cache-coherent reads): 64 bytes (the GPULight) per entry.
bool RAB_StoreCompactLightInfo(uint linearIndex, RAB_LightInfo info)
{
    if (g_RabBridge.compactLightsUav == 0) return false;
    RWStructuredBuffer<GPULight> lights = ResourceDescriptorHeap[g_RabBridge.compactLightsUav];
    lights[linearIndex] = info.light;
    return true;
}
RAB_LightInfo RAB_LoadCompactLightInfo(uint linearIndex)
{
    RAB_LightInfo info = RAB_EmptyLightInfo();
    if (g_RabBridge.compactLightsSrv == 0) return info;
    StructuredBuffer<GPULight> lights = ResourceDescriptorHeap[g_RabBridge.compactLightsSrv];
    info.light = lights[linearIndex];
    info.valid = true;
    return info;
}
// The proposal's source pdf: the light's share of the local-light PDF
// texture (LightSamplingPass: light i at its Z-curve texel, the top mip the total).
uint RAB_Part1By1(uint x)
{
    x &= 0x0000ffffu;
    x = (x ^ (x << 8)) & 0x00ff00ffu; x = (x ^ (x << 4)) & 0x0f0f0f0fu;
    x = (x ^ (x << 2)) & 0x33333333u; x = (x ^ (x << 1)) & 0x55555555u;
    return x;
}
uint RAB_Compact1By1(uint x)
{
    x &= 0x55555555u;
    x = (x ^ (x >> 1)) & 0x33333333u; x = (x ^ (x >> 2)) & 0x0f0f0f0fu;
    x = (x ^ (x >> 4)) & 0x00ff00ffu; x = (x ^ (x >> 8)) & 0x0000ffffu;
    return x;
}
float RAB_EvaluateLocalLightSourcePdf(uint lightIndex)
{
    if (g_RabBridge.lightPdfIdx == 0) return 0;
    Texture2D<float> pdf = ResourceDescriptorHeap[g_RabBridge.lightPdfIdx];
    const float total = pdf.Load(int3(0, 0, g_RabBridge.lightPdfMips - 1));
    const uint2 texel = uint2(RAB_Compact1By1(lightIndex), RAB_Compact1By1(lightIndex >> 1));
    return total > 0 ? pdf.Load(int3(texel, 0)) / total : 0;
}

// A light sample: a point on the light (its centre, or for an emitter of
// radius R a point on its disk facing the receiver) and what the engine's
// shading needs. The engine shades local lights as points with a windowed
// attenuation (light_eval.hlsli), so the sample's radiance is evaluated
// against the centre, exactly like the engine DI core's target; the point
// only aims visibility. Delta sources: no solid-angle pdf.
struct RAB_LightSample
{
    float3 position;     // the visibility target
    float3 center;       // the light's position (shading)
    float3 normal;
    float3 radiance;     // colour * intensity, before attenuation and BRDF (RAB_PT: after attenuation)
    float solidAnglePdf;
    uint lightType;
    GPULight light;
    bool sun;            // RAB_PT: the sun's cone sample
    float bsdfPdf;       // RAB_PT: the BSDF pdf towards it at the surface it was drawn for (NEE MIS)
};
RAB_LightSample RAB_EmptyLightSample() { return (RAB_LightSample)0; }
bool RAB_IsAnalyticLightSample(RAB_LightSample s) { return true; }   // point / spot: delta for the BRDF
float RAB_LightSampleSolidAnglePdf(RAB_LightSample s) { return s.solidAnglePdf; }
#ifdef RAB_PT
// The PT core shades a local light at its centre (the engine path tracer's
// integrand); its sample's disk point only aims the visibility ray.
float3 RAB_LightSamplePosition(RAB_LightSample s) { return s.center; }
#else
float3 RAB_LightSamplePosition(RAB_LightSample s) { return s.position; }
#endif
float3 RAB_LightSampleRadiance(RAB_LightSample s) { return s.radiance; }
void RAB_GetLightDirDistance(RAB_Surface surface, RAB_LightSample s, out float3 outLightDir, out float outLightDistance)
{
    const float3 d = s.position - surface.worldPos;
    outLightDistance = length(d);
    outLightDir = d / max(outLightDistance, 1e-6);
}
RAB_LightSample RAB_SamplePolymorphicLight(RAB_LightInfo info, RAB_Surface surface, float2 uv)
{
    RAB_LightSample s = RAB_EmptyLightSample();
#ifdef RAB_PT
    if (info.valid && info.sun)
    {
        // The engine path tracer's sun (RTSun): a direction uniform in the
        // cone's solid angle, radiance such that the whole disk gives
        // sunColor at normal incidence; the cone pdf is the same for every
        // receiver, so a shift to the sun keeps its direction (Jacobian 1).
        const float c = lerp(g_PT.sunCosRadius, 1.0, uv.x), phi = 2 * PI * uv.y, st = sqrt(saturate(1 - c * c));
        const float3 l = RTBasis(normalize(-g_PT.sunDirection), float3(st * cos(phi), st * sin(phi), c));
        s.light = info.light;
        s.sun = true;
        s.position = s.center = surface.worldPos + l * RAB_DISTANT_LIGHT_DISTANCE;
        s.normal = -l;
        s.radiance = g_PT.sunColor / (PI * (1 - g_PT.sunCosRadius * g_PT.sunCosRadius));
        s.solidAnglePdf = 1.0 / (2 * PI * (1 - g_PT.sunCosRadius));
        s.bsdfPdf = surface.valid ? RAB_PTScatterPdf(surface, l) : 0;
        return s;
    }
#endif
    if (!info.valid || info.light.type == 0u) return s;
    s.light = info.light;
    s.lightType = info.light.type;
    s.center = info.light.position;
    s.position = info.light.position;
    s.radiance = info.light.color * info.light.intensity;
    const float R = RAB_LightSourceRadius(info);
    const float3 toLight = info.light.position - surface.worldPos;
    const float distance = length(toLight);
    s.normal = distance > 0 ? -toLight / distance : float3(0, 1, 0);
    if (R > 0 && distance > R)
    {
        // The emitter's disk facing the receiver, uniform in area - the engine
        // DI core's point (RTDI.cs.hlsl ApplyLightPoint): the same basis, and
        // RTXDI's 16-bit uv (k / 65535) put on the engine seed's 1/65536 grid,
        // so a sample's uvData is its engine seed and converts exactly.
        const float3 w = toLight / distance;
        const float3 t = normalize(abs(w.y) < .999 ? cross(w, float3(0, 1, 0)) : cross(w, float3(1, 0, 0)));
        const float3 b = cross(w, t);
        const float2 xi = uv * (65535.0 / 65536.0);
        const float r = R * sqrt(xi.x), phi = 6.28318530718 * xi.y;
        s.position = info.light.position + r * (cos(phi) * t + sin(phi) * b);
    }
#ifdef RAB_PT
    // RTXDI's delta-light convention (its sample's point light): solidAnglePdf
    // 1, the radiance the limit radiance / pdf - here what the engine shades at
    // this receiver, colour x intensity x the windowed attenuation towards the
    // centre - so the reconnection Jacobian pdf_src / pdf_dst is exactly 1.
    s.solidAnglePdf = 1;
    s.radiance = 0;
    s.bsdfPdf = 0;
    if (distance > 0.002 && distance < info.light.radius)
    {
        const float3 l = toLight / distance;
        s.radiance = info.light.color * info.light.intensity * EvalLocalLightAttenuation(info.light, l, distance);
        if (surface.valid) s.bsdfPdf = RAB_PTScatterPdf(surface, l);
    }
#endif
    return s;
}
// The engine DI core's integrand (RTDI.cs.hlsl EvaluateLight + DITarget):
// Cook-Torrance x colour x intensity x windowed attenuation x cosine, summed
// over RGB.
#ifdef RAB_DI_ENGINE
// The DI core: the engine core's integrand and target, whatever its flags
// (the albedo-free diffuse, the luminance target).
float3 RAB_ShadeLightSample(RAB_LightSample s, RAB_Surface surface)
{
    if (s.lightType == 0u || !surface.valid) return 0;
    float3 d, sp, l; float distance;
    EvaluateLight(RAB_SurfaceToReceiver(surface), s.light, d, sp, l, distance);
    return d + sp;
}
float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample s, RAB_Surface surface)
{
    if (s.lightType == 0u || !surface.valid) return 0;
    float3 d, sp, l; float distance;
    EvaluateLight(RAB_SurfaceToReceiver(surface), s.light, d, sp, l, distance);
    return LightTarget(d, sp);
}
#else
float3 RAB_ShadeLightSample(RAB_LightSample s, RAB_Surface surface)
{
    if (s.lightType == 0u || !surface.valid) return 0;
    const float3 toLight = s.center - surface.worldPos;
    const float distance = length(toLight);
    if (distance <= 0.002 || distance >= s.light.radius) return 0;
    const float3 l = toLight / distance;
    const float ndl = saturate(dot(surface.normal, l));
    if (ndl <= 0) return 0;
    float3 spec, kd;
    EvalCookTorrance(surface.normal, surface.viewDir, l, surface.material.F0, surface.material.roughness,
                     surface.material.metal, surface.material.albedo, spec, kd);
    const float3 energy = s.radiance * EvalLocalLightAttenuation(s.light, l, distance) * ndl;
    return (kd * surface.material.albedo / PI + spec) * energy;
}
float RAB_GetLightSampleTargetPdfForSurface(RAB_LightSample s, RAB_Surface surface)
{
    const float3 v = max(RAB_ShadeLightSample(s, surface), 0);
    return v.r + v.g + v.b;
}
#endif
// ReGIR's per-cell proposal. It must be conservative: a light it gives 0 for a
// cell is never a candidate for any surface that samples that cell, which is
// energy lost, not noise. So it takes the light seen from the nearest point of
// the region that can sample the cell (distance window and inverse square) and
// widens a spot's cone by the angle that region subtends from the light. Do not
// narrow either: the cone of the cell's centre alone drops every spot covering
// only part of a cell (~-31% DI energy against the path tracer), and a surface
// looks its cell up through a jittered position (RTXDI_CalculateReGIRCellIndex:
// up to half the jitter scale per axis), so a cell serves surfaces outside its
// own bounding sphere (~-2.5%).
#if defined(RAB_DI_ENGINE) && RTXDI_REGIR_MODE == RTXDI_REGIR_ONION
// How far outside a cell's sphere a surface that samples it can be: the onion
// jitter scale (RTXDI_ReGIR_GetJitterScale) a little further out than the
// centre, times the half-diagonal of the jitter cube.
float RAB_ReGIRJitterReach(float3 volumeCenter, float volumeRadius)
{
    const float3 onionCenter = float3(g_ReGIR.commonParams.centerX, g_ReGIR.commonParams.centerY, g_ReGIR.commonParams.centerZ);
    const float cell = max(g_ReGIR.commonParams.cellSize, 1e-4);
    const float d = (length(volumeCenter - onionCenter) + 2 * volumeRadius) / cell;
    const float scale = max(1.0, max(pow(d, 1.0 / 3.0) * g_ReGIR.onionParams.cubicRootFactor, d * g_ReGIR.onionParams.linearFactor));
    return 0.8660254 * scale * g_ReGIR.commonParams.samplingJitter * cell;
}
#else
float RAB_ReGIRJitterReach(float3 volumeCenter, float volumeRadius) { return 0; }
#endif
float RAB_GetLightTargetPdfForVolume(RAB_LightInfo info, float3 volumeCenter, float volumeRadius)
{
    if (!info.valid || info.light.type == 0u) return 0;
    volumeRadius += RAB_ReGIRJitterReach(volumeCenter, volumeRadius);
    const float3 toLight = info.light.position - volumeCenter;
    const float centerDistance = length(toLight);
    const float distance = max(centerDistance - volumeRadius, 0.01);
    if (distance >= info.light.radius) return 0;
    const float3 c = info.light.color * info.light.intensity;
    float angular = 1;
    if (info.light.type == 2u && info.light.spotAngle > 0 && centerDistance > volumeRadius)
    {
        // The smallest angle between the spot axis and a direction from the
        // light into the cell's bounding sphere.
        const float axis = acos(clamp(dot(-toLight / centerDistance, normalize(info.light.direction)), -1.0, 1.0));
        const float spread = asin(saturate(volumeRadius / centerDistance));
        const float theta = max(axis - spread, 0.0);
        angular = smoothstep(cos(info.light.spotAngle), cos(info.light.spotAngle * 0.75), cos(theta));
    }
    return (c.r + c.g + c.b) * SurfaceDistanceAttenuation(distance, info.light.radius) * angular;
}
bool RAB_GetConservativeVisibility(RAB_Surface surface, RAB_LightSample s)
{
    return RAB_TraceVisibility(surface, s.position);
}
bool RAB_GetConservativeVisibility(RAB_Surface surface, float3 samplePosition)
{
    return RAB_TraceVisibility(surface, samplePosition);
}
// RTXDI's "previous BVH" visibility: the engine keeps no previous TLAS, so
// the previous surface is tested against today's (RTXDI's documented biased
// option).
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, RAB_LightSample s)
{
    return RAB_TraceVisibility(previousSurface, s.position);
}
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface previousSurface, float3 samplePosition)
{
    return RAB_TraceVisibility(previousSurface, samplePosition);
}

// ==== BRDF ====
// The engine's BSDF sampling (bsdf_sample.hlsli: 50/50 cosine / GGX VNDF).
bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface s, inout RTXDI_RandomSamplerState rng, out float3 dir)
{
    dir = float3(0, 0, 1);
    uint state = RTXDI_GetNextRandom(rng) * 4294967295.0 + 1;
    float3 weight;
    return RTSampleBSDF(s.normal, s.viewDir, s.material.albedo, s.material.roughness, s.material.metal,
                        s.material.reflectance, state, dir, weight);
}
float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface s, float3 dir)
{
#ifdef RAB_PT
    return RAB_PTScatterPdf(s, dir);
#else
    return RTBsdfPdf(s.normal, s.viewDir, dir, s.material.roughness);
#endif
}

// ==== Environment ====
// The engine DI samples no environment map (the sky reaches the frame through
// IBL); an RTXDI core runs with numEnvironmentSamples = 0.
float2 RAB_GetEnvironmentMapRandXYFromDir(float3 worldDir) { return float2(0, 0); }
float RAB_EvaluateEnvironmentMapSamplingPdf(float3 L) { return 0; }
// Engine lights are not geometry: a BRDF ray never hits one.
bool RAB_TraceRayForLocalLight(float3 origin, float3 direction, float tMin, float tMax,
                               out uint outLightIndex, out float2 outRandXY)
{
    outLightIndex = RTXDI_InvalidLightIndex;
    outRandXY = float2(0, 0);
    return false;
}

// ==== GI ====
#ifdef RAB_GI_ENGINE
// The GI core: the engine core's target - GITarget, the luminance of the
// albedo-weighted diffuse integrand - and, unless RAB_GI_VISIBLE_TARGET is
// false (RTXDI's own setup: an unshadowed target, visibility only in the bias
// correction and the final shading), its visibility from this receiver. A
// sample hidden from a receiver is a zero of that receiver's integrand, so
// the bias correction's weights then see exactly the receivers that could
// have produced it (what the engine core's reuse does with its rays). A
// surface's own sample is known visible: no ray.
float RAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface)
{
    if (!surface.valid) return 0;
    GIReservoir sample = (GIReservoir)0;
    sample.P = samplePosition;
    sample.radiance = sampleRadiance;
    const float target = GITarget(surface.worldPos, surface.normal, sample, surface.material.albedo);
    if (!(target > 0) || !(RAB_GI_VISIBLE_TARGET)) return target;
    if (surface.ownVisible && all(samplePosition == surface.ownSample)) return target;
    return RAB_TraceVisibility(surface, samplePosition) ? target : 0;
}
// The engine core's Jacobian leash (counted in its JacobianLeash slot) and its
// opt-in clamp to [1/3, 3] (-gi_jacobian_clamp, RTXDI's suggestion).
bool RAB_ValidateGISampleWithJacobian(inout float jacobian)
{
    if (!(jacobian > 0.1) || !(jacobian < 10)) { Count(19); return false; }
    jacobian = GIClampJacobian(jacobian);
    return true;
}
#else
// A GI sample's target at a receiver: the engine GI's diffuse lobe (the
// reservoir carries the secondary radiance), summed over RGB.
float RAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface)
{
    if (!surface.valid) return 0;
    const float3 l = normalize(samplePosition - surface.worldPos);
    const float3 v = max(sampleRadiance * surface.material.albedo / PI * saturate(dot(surface.normal, l)), 0);
    return v.r + v.g + v.b;
}
// RTXDI's default Jacobian rejection window.
bool RAB_ValidateGISampleWithJacobian(inout float jacobian)
{
    if (!(jacobian > 0.1) || !(jacobian < 10)) return false;
    return true;
}
#endif

#endif // RTXDI_BRIDGE_HLSLI
