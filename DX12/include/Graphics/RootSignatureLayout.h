#pragma once

// RootSignatureLayout - the single source of truth for the root-parameter indices of
// the two SHARED root signatures the backend builds (GraphicsDX12::
// CreateDefaultRootSignature / CreateComputeRootSignature). The builders, every render
// pass and the HLSL registers must agree: a drifted slot is not a compile error but a
// wrong-resource bind with no validation message.
// Everything here is a compile-time constant; the header names no DX12 type and pulls
// in no D3D header, so portable code may include it.
//
// Two kinds of slot flow through IGraphicsDevice, each its own type, and every binding
// API takes the matching one, slot first (.index is the number; only the backend's
// D3D12 calls want it):
//   ABSOLUTE root-parameter index (GfxParam in Gfx::, CmpParam in Cmp::):
//       SetRootBufferSRV, BindDescriptorTableGpuHandle, SetComputeRootCBV,
//       SetComputeDescriptorTable, SetGraphicsRootConstant, cl.BindDescriptorTableHandle.
//       BindCustomMaterialCBV / BindCustomMaterialTextureTable take no slot: they bind
//       Gfx::CustomMatCBV / Gfx::CustomMatTex.
//   GROUP-RELATIVE index (GfxCB / GfxSRV in GfxRel::):
//       BindConstantBuffer, BindConstantBufferAtOffset, BindResource. The backend adds
//       the group base (CB -> +Gfx::CBVBase, SRV -> +Gfx::SRVBase) and range-checks
//       against the group count; an out-of-range index binds nothing.
//
// There are no sampler slots: every shader on these signatures takes its samplers from
// the sampler heap (shaders/sampler_heap.hlsli: the fixed slots of
// shaders/shared/sampler_slots.hlsli, or the one sampler a pass picks at run time,
// Gfx::DynamicSampler).
//
// Not covered: the private root signatures DDGIPass / RTReflectionPass / RTShadowPass /
// RTAOPass build for themselves. Those numbers stay local to their own layouts.

#include <cstdint>

namespace RHI::Slots
{
    // ---- A slot is a TYPE, not an integer ----
    // An absolute index passed where a group-relative one is expected (or the reverse), a
    // graphics slot passed to a compute call, or a bare number, is a compile error. The
    // number is .index. A pass's private root signature declares its own constants of
    // these types (e.g. `constexpr RHI::Slots::CmpParam kCB{0};`), local to that pass.
    // Counts and bases (CBVBase, SRVCount, ParamCount ...) are plain integers, not slots.
    struct GfxParam                   // a graphics root parameter, absolute
    {
        uint32_t index;
        constexpr bool operator==(const GfxParam&) const = default;
    };
    struct GfxCB                      // the root CBV group b1..b7, group-relative
    {
        uint32_t index;
        constexpr bool operator==(const GfxCB&) const = default;
    };
    struct GfxSRV                     // the SRV table group t2..t5, group-relative
    {
        uint32_t index;
        constexpr bool operator==(const GfxSRV&) const = default;
    };
    struct CmpParam                   // a compute root parameter: the shared
    {                                 // signature's or a pass's private one
        uint32_t index;
        constexpr bool operator==(const CmpParam&) const = default;
    };

    // ==== Graphics ("default" / PVF) root signature: 33 parameters, 0..32 ====
    // Built by GraphicsDX12::CreateDefaultRootSignature().
    //
    // The 64-DWORD ceiling - read before adding a root parameter. This signature costs:
    //     1 x 32BIT_CONSTANTS (4 values)  =  4 DWORD
    //     8 x root CBV (b1..b7 + b8)      = 16 DWORD  (2 each)
    //     2 x root SRV (t0, t1)           =  4 DWORD  (2 each)
    //    21 x descriptor table            = 21 DWORD  (1 each)
    //     1 x 32BIT_CONSTANTS (1 value)   =  1 DWORD  (DynamicSampler)
    //                                       ---------
    //                                        46 DWORD  (Gfx::DwordCost)
    // Going past 64 does not fail to compile: D3D12SerializeRootSignature fails at startup,
    // CreateDefaultRootSignature throws and the engine does not boot. The headroom is not
    // free capacity: a feature that wants a root parameter should fold something first.
    //
    // To buy headroom, FOLD a single-SRV descriptor table into a bindless handle carried in
    // an already-bound constant buffer and opened in HLSL through bindless.hlsli
    // (ResourceDescriptorHeap[]: SM 6.6 + Resource Binding Tier 3 +
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED, all in place). That costs 0 DWORD. Namespace
    // BindlessIdx lists the folded fields and their rules.

    // Which descriptor heaps the two shared signatures let a shader index directly
    // (SM 6.6). The builders in GraphicsDX12.cpp set their flags from these, and
    // ShaderBindingValidation checks every shared-signature shader against them, so a
    // shader that indexes a heap the signatures do not open is named at load, before any
    // PSO is built from it. A *_HEAP_DIRECTLY_INDEXED signature needs its heaps set
    // (SetDescriptorHeaps) before SetGraphicsRootSignature / SetComputeRootSignature; the
    // engine binds the CBV/SRV/UAV and sampler heaps together on every list.
    inline constexpr bool kSharedResourceHeapIndexed = true;
    inline constexpr bool kSharedSamplerHeapIndexed  = true;

    namespace Gfx
    {
        // [0] 4x 32-bit root constants at b0 space0:
        //     [0] meshDescIdx [1] instanceOffset [2] materialIndex
        //     [3] outlinePixelsBits (asfloat)
        inline constexpr GfxParam RootConstants   { 0 };
        inline constexpr uint32_t RootConstantCount = 4;   // Num32BitValues

        // [1..7] inline root CBVs at b1..b7 space0. Address via GfxRel::CB*.
        inline constexpr uint32_t CBVBase        =  1;
        inline constexpr uint32_t CBVCount       =  7;

        inline constexpr GfxParam InstanceBuf    { 8 };   // ROOT_SRV  t0 space0
        inline constexpr GfxParam MeshDesc       { 9 };   // ROOT_SRV  t1 space0

        // [10..13] generic per-draw SRV tables t2..t5 space0.
        // Two ways in: BindResource(i) takes the RELATIVE index (GfxRel::SRV_t*), while
        // cl.BindDescriptorTableHandle / SetRootBufferSRV take the ABSOLUTE index below.
        // Every pass binds something different here (terrain heightmap, splatmap, object-ID,
        // water normals, UI vertices, ...), so these are named by register, not by purpose.
        inline constexpr uint32_t SRVBase        = 10;
        inline constexpr uint32_t SRVCount       =  4;
        inline constexpr GfxParam SRV_t2         { 10 };
        inline constexpr GfxParam SRV_t3         { 11 };
        inline constexpr GfxParam SRV_t4         { 12 };
        inline constexpr GfxParam SRV_t5         { 13 };

        inline constexpr GfxParam BindlessBuffers { 14 };  // t0 space1, 16384 SRVs

        // [15..17] IBL: irradiance cube (t6), radiance cube (t7), BRDF LUT (t8).
        // Also reused by passes that need any t6/t7/t8 SRV and never bind IBL (SkyboxPass env
        // map, CloudPass, VideoPass Y plane, WaterPass ripple texture). Use the IBL* name when
        // the resource really is IBL, the SRV_t* alias otherwise; the value is the same.
        inline constexpr uint32_t IBLSRVBase     = 15;
        inline constexpr uint32_t IBLSRVCount    =  3;
        inline constexpr GfxParam IBLIrradiance  { 15 };   // t6 space0
        inline constexpr GfxParam IBLRadiance    { 16 };   // t7 space0
        inline constexpr GfxParam BRDFLUT        { 17 };   // t8 space0
        inline constexpr GfxParam SRV_t6         { 15 };   // register-shaped alias
        inline constexpr GfxParam SRV_t7         { 16 };
        inline constexpr GfxParam SRV_t8         { 17 };

        inline constexpr GfxParam ShadowSRV      { 18 };   // t9..t11 space0
        inline constexpr uint32_t ShadowSRVCount =  3;

        // [19..21] clustered lighting: lights (t13), index list (t14), grid (t15).
        // The gap is deliberate: t12 belongs to MaterialBuffer (slot 23).
        inline constexpr uint32_t ClusterSRVBase  = 19;
        inline constexpr uint32_t ClusterSRVCount =  3;
        inline constexpr GfxParam ClusterLights    { 19 }; // t13 space0
        inline constexpr GfxParam ClusterIndexList { 20 }; // t14 space0
        inline constexpr GfxParam ClusterGrid      { 21 }; // t15 space0

        inline constexpr GfxParam BindlessTex    { 22 };   // t0  space2, g_AllTextures[]
        inline constexpr GfxParam MaterialBuf    { 23 };   // t12 space0
        inline constexpr GfxParam EmissiveSRV    { 24 };   // t17 space0
        inline constexpr GfxParam SSAOSRV        { 25 };   // t18 space0
        inline constexpr GfxParam SkySH          { 26 };   // t19 space0
        inline constexpr GfxParam AerialPersp    { 27 };   // t20 space0, Texture3D
        inline constexpr GfxParam SpotShadowAtlas { 28 };  // t21 space0
        inline constexpr GfxParam SpotShadowVP   { 29 };   // t22 space0
        inline constexpr GfxParam CustomMatCBV   { 30 };   // ROOT_CBV b8 space0
        inline constexpr GfxParam CustomMatTex   { 31 };   // t0..t7 space3 (8 SRVs)
        // The one sampler a pass picks at run time: the heap index of the material sampler /
        // Terrain's / Water's / a UI draw's, as a 1-DWORD root constant (sampler_heap.hlsli's
        // g_DynamicSamplerIdx), written by IGraphicsDevice::SetDynamicSampler. Every other
        // sampler is a fixed sampler-heap slot the shader names as a constant
        // (shaders/shared/sampler_slots.hlsli).
        inline constexpr GfxParam DynamicSampler { 32 };   // 32BIT_CONSTANTS b0 space9, 1 value

        // ---- Registers t16 and t23..t45 are RESERVED: do not reuse them ----
        // They named root parameters that are gone. Most became a heap index carried in an
        // already-bound buffer (namespace BindlessIdx):
        //   t45 RT shadow sun mask   -> LightCB.rtShadowMaskIdx
        //   t44 RT reflection output -> LightCB.rtReflectionIdx
        //   t43 hair (bangs) shadow  -> LightCB.hairShadowIdx
        //   t42 per-object shadow    -> LightCB.perObjShadowIdx
        //   t41 point cube atlas     -> LightCB.pointShadowIdx
        //   t37..t40 DDGI probe data -> VolumeGPUDesc.probeDataBaseIdx
        //   t33..t36 DDGI depth      -> VolumeGPUDesc.depthBaseIdx
        //   t29..t32 DDGI probe SH   -> VolumeGPUDesc.irradianceBaseIdx
        //   t28 DDGI volume buffer   -> LightCB.ddgiVolumeBufIdx
        //   t27 SSR result           -> LightCB.ssrResultIdx
        //   t26 probe index list     -> LightCB.reflProbeIndexIdx
        //   t25 probe grid           -> LightCB.reflProbeGridIdx
        //   t24 probe buffer         -> LightCB.reflProbeBufIdx
        //   t23 probe cube array     -> nothing: probe cubes are sampled bindless per cube
        //                               (GpuShared::ReflectionProbe::cubemapSrvIndex). The cube array
        //                               itself stays: ReflectionProbeManager carves those
        //                               per-cube SRVs out of it, so deleting it silently
        //                               removes probe reflections.
        //   t16 global NPR ramp      -> nothing: each Toon material samples its own ramp
        //                               (customTextureIds[3] through g_AllTextures[])
        // The sampler tables s0..s3 space0 are gone too (the sampler heap replaced them).
        // There are no constants for any of them on purpose: a leftover reference must fail to
        // compile, not bind a renumbered neighbour. Docs, shader headers and PIX captures still
        // name these resources by the old registers, so the next graphics-signature SRV starts
        // at t46. ShaderBindingValidation has no range for t16, t23..t45 or s0..s3, so a
        // shader that declares one fails at startup instead of binding nothing.
        // Exceptions on PRIVATE signatures, which that table cannot see: SSGIReservoirSpatial.cs's
        // t16 (the SSGI pass's), and HybridReflIntersectHW/SW.cs's t23/t24 sky cubes and t27
        // Hi-Z (RTReflectionPass's).
        // A signature without HEAP_DIRECTLY_INDEXED fails a heap read at PSO creation, never at
        // dxc compile time; the offline check greps the DXIL for `createHandleFromHeap`.

        inline constexpr uint32_t ParamCount     = 33;
        // The DWORD cost of those parameters (the sum in the graphics root signature banner
        // above; the D3D12 ceiling is 64). CreateDefaultRootSignature adds up what it actually
        // built and ENGINE_CHECKs it against this, and the skills cite it through gen_catalog's
        // facts (rootSig.graphics.dwords).
        inline constexpr uint32_t DwordCost      = 46;

        // Width of the per-volume DDGI descriptor blocks. Mirrors DDGI::kMaxVolumes and
        // DDGI_MAX_VOLUMES in the HLSL. ABI: it is the stride the shader walks with
        // `base + volumeSlot`, so DDGIVolumeManager static_asserts against it.
        inline constexpr uint32_t DDGIMaxVolumes = 8;
    }

    // ==== GfxRel: group-relative indices for the graphics root signature ====
    // What BindConstantBuffer / BindResource take: an offset within the group, not a
    // root-parameter index.
    namespace GfxRel
    {
        // BindConstantBuffer(i) -> root Gfx::CBVBase + i -> register b(1+i).
        inline constexpr GfxCB  CB_b1{ 0 };
        inline constexpr GfxCB  CB_b2{ 1 };
        inline constexpr GfxCB  CB_b3{ 2 };
        inline constexpr GfxCB  CB_b4{ 3 };
        inline constexpr GfxCB  CB_b5{ 4 };
        inline constexpr GfxCB  CB_b6{ 5 };
        inline constexpr GfxCB  CB_b7{ 6 };

        // BindResource(i) -> root Gfx::SRVBase + i -> register t(2+i).
        inline constexpr GfxSRV SRV_t2{ 0 };
        inline constexpr GfxSRV SRV_t3{ 1 };
        inline constexpr GfxSRV SRV_t4{ 2 };
        inline constexpr GfxSRV SRV_t5{ 3 };
    }

    // ==== BindlessIdx: bindless fields that replace a root parameter ====
    // A single-SRV descriptor table costs 1 root DWORD; a folded one is a uint field in an
    // already-bound constant buffer, at zero root cost.
    //
    // Three mirrors must agree: the C++ struct (RendererDetail::LightCB, Renderer.h), the
    // HLSL cbuffer (shaders/light_cb.hlsli) and these byte offsets. Renderer.h pins the
    // C++ struct to these with static_assert(offsetof(...) == ...); the HLSL mirror is
    // checked against the C++ struct, every member, when each shader that declares it
    // loads (LayoutSpecs.cpp, [LAYOUT]). DDGI::VolumeGPUDesc (C++ DDGIVolumeManager.h /
    // HLSL DDGIVolumeGPU in DDGICommon.hlsli) is a second carrier with the same discipline:
    // the kDDGIVolumeDesc_* constants at the end of this namespace.
    //
    // Rules for anything added here:
    //   * The value is a BYTE OFFSET into the CB named in the constant.
    //   * A LightCB index field holds a typed bindless handle (gen << 20 | heap index), never
    //     a bare heap index: write it with IGraphicsDevice::BindlessOf<K> (which checks the
    //     heap's record of the slot) or NullOf<K>, and open it in HLSL only through
    //     bindless.hlsli (BindlessTex2D<T>(h), BindlessSBuf<T>(h) ...), never
    //     ResourceDescriptorHeap[h]. The DDGI block bases (kDDGIVolumeDesc_*) are plain heap
    //     indices.
    //   * The handle is recomputed EVERY FRAME from the resource's current GPU handle
    //     (BindlessOf goes through DescriptorHeapIndexFromGpuHandle). Do not cache one at
    //     Init: resize recreates textures and the texture free list recycles handle ids.
    //   * "Resource absent" is a real fallback resource (a 1x1 texture or a type-correct null
    //     SRV) or the kind's null (NullOf<K>, or BindlessOf<K> of a 0 GPU handle), so the
    //     shader path needs no new branch. Where an entry says 0 = none, the shader gates
    //     with BindlessNone(h), which is true for 0 and for a fixed null only: the null of a
    //     structured stride other than 4 is not fixed, so a gated structured field hands over
    //     BINDLESS_NULL_STRUCTURED.
    namespace BindlessIdx
    {
        // ---- LightCB (b1 deferred / b2 forward; RendererDetail::LightCB) ----
        // uint rtShadowMaskIdx - RTShadowPass sun-visibility mask. Read by shadow.hlsli under
        // RT_SHADOW_MASK, and only when LightCB.rtShadowEnabled != 0: the fallback is 1x1, so
        // a full-screen Load() against it is out of bounds everywhere but (0,0). The gate, not
        // the index, keeps that safe.
        inline constexpr uint32_t kLightCB_RTShadowMaskIdx = 776;
        // uint rtReflectionIdx - RTReflectionPass denoised output. Read unconditionally by
        // Lighting.ps.hlsl; the fallback is 1x1 BLACK, so rtConf == 0 and the 3-tier specular
        // composite collapses to its form without RT reflections.
        inline constexpr uint32_t kLightCB_RTReflectionIdx = 780;

        // ---- Shadow family (16B row 784..799) ----
        // Each is opened inside an existing gate and falls back to a real 1x1 texture whose
        // handle is handed over, so no sentinel is needed.
        //
        // uint pointShadowIdx - PointShadowPass cube atlas, TextureCubeArray<float>. It cannot
        // live in g_AllTextures[] (Texture2D only), so a heap-index fold is the only legal one.
        // Sampled with SampleCmpLevelZero against gShadowSampler, the fixed sampler-heap slot
        // SMP_CMP_LINEAR_BORDER_WHITE_GE. Gate: a clustered light with
        // type == 1 && shadowSliceIdx != 0xFFFFFFFF. Fallback: a 1x1 12-slice cube array, so
        // even a stale slice index samples in bounds.
        inline constexpr uint32_t kLightCB_PointShadowIdx  = 784;
        // uint perObjShadowIdx - PerObjectShadowPass player map, Texture2D<float>.
        // Gate: perObjShadowParams.x != 0.
        inline constexpr uint32_t kLightCB_PerObjShadowIdx = 788;
        // uint hairShadowIdx - hair (bangs -> face) shadow map, Texture2D<float>. Gate:
        // hairShadowParams.x != 0, NPR face pixels only. Shares the 1x1 depth-0.0 fallback
        // with perObjShadowIdx: under a reversed-Z GREATER_EQUAL comparison sampler 0.0 means
        // "no occluder".
        inline constexpr uint32_t kLightCB_HairShadowIdx   = 792;

        // ---- DDGI ----
        // uint ddgiVolumeBufIdx - the engine-wide StructuredBuffer<DDGIVolumeGPU>, the entry
        // point to DDGI: the per-volume block base indices live inside the descriptors it
        // holds, so the shader opens this one first. The buffer is an UPLOAD ring (one slot per
        // frame in flight), so the handle is re-resolved every frame from
        // DDGIVolumeManager::GetVolumeBufferSrv(gfx), the current slot's SRV.
        // Gate: `ddgiEnabled != 0 && ddgiVolumeCount > 0`. Without DDGI (no DXR) the handle is
        // the null of the buffer's stride, never read.
        inline constexpr uint32_t kLightCB_DDGIVolumeBufIdx = 796;

        // ---- Per-cluster reflection-probe list (16B row 800..815) ----
        // uint reflProbeGridIdx - ClusterPass per-cluster probe grid:
        // StructuredBuffer<ProbeGridEntry> { uint offset; uint count; }, stride 8,
        // kClusterCount elements. A DEFAULT buffer created once in ClusterPass::Init, so the
        // index is lifetime-stable; it is still re-resolved every frame like every entry here.
        inline constexpr uint32_t kLightCB_ReflProbeGridIdx  = 800;
        // uint reflProbeIndexIdx - the flat probe index list the grid points into:
        // StructuredBuffer<uint>, stride 4, kMaxProbeIndices elements.
        //
        // Gate for both: `reflectionProbeCount > 0` in Lighting.ps. Neither index is a loop
        // variable, so NonUniformResourceIndex() is not required.
        //
        // Absent = a type-correct null structured-buffer SRV (stride 8 / stride 4) built once
        // in LightingPass::Init (IGraphicsDevice::CreateNullStructuredSrvBlock). Do not hand a
        // descriptor of another type (e.g. a TextureCubeArray) to a StructuredBuffer open: it
        // is UB and the Tier 3 debug layer stays silent.
        inline constexpr uint32_t kLightCB_ReflProbeIndexIdx = 804;

        // ---- SSR result ----
        // uint ssrResultIdx - SSRUpsamplePass color output (rgb = resolved reflection,
        // a = confidence), Texture2D<float4>, sampled with SMP_LINEAR_CLAMP and Load()ed.
        // Read only by Water.ps.hlsl, inside its `!underwater && prevClip.w > 1e-3` block;
        // Lighting.ps gets the traced signal through rtReflectionIdx.
        //
        // Resize hazard: this resource is recreated at runtime. SSRSubsystem::OnResize
        // rebuilds SSRUpsamplePass::m_colorTex on a render-resolution change and frees the old
        // descriptor. Renderer calls OnResize at the top of BeginFrame and resolves this index
        // in the same frame's resolve() block, both before graph.Execute. Caching the index
        // would sample a released descriptor for a frame.
        //
        // Absent (m_ssrEnabled == false, or no result yet) = WaterPass's own 1x1 BLACK
        // Texture2D<float4>: alpha 0 makes the (1 - conf) sky dampening a no-op.
        inline constexpr uint32_t kLightCB_SSRResultIdx      = 808;

        // ---- Reflection-probe buffer ----
        // uint reflProbeBufIdx - ReflectionProbeManager's
        // StructuredBuffer<GpuShared::ReflectionProbe>, stride 192, mirroring
        // `struct ReflectionProbe` in reflection_probe.hlsli.
        //
        // Two graphics consumers, each opening it inside a gate it already had:
        //   Lighting.ps.hlsl     - `reflectionProbeCount > 0`, the block that also opens
        //                          reflProbeGridIdx / reflProbeIndexIdx
        //   custom_surface.hlsli - the MS_IS_FORWARD IBL block
        //                          `iblRadianceMips > 0 || reflectionProbeCount > 0`
        // A CB value, never a loop variable: no NonUniformResourceIndex(). Compute readers
        // (ClusterCullProbes.cs, HybridReflIntersectHW/SW.cs) open the same buffer by the
        // index their own constants carry (ClusterCB / HybridReflConstants .probeBufferIdx).
        //
        // Upload ring: ReflectionProbeManager::Init creates all kFramesInFlight SRVs and
        // GetBufferSrv(gfx) picks by gfx.GetFrameIndex(), so the value changes every frame.
        // Renderer writes it unconditionally (not under `if (m_lightingPass)`): LightCB is
        // itself a ring, so a skipped write leaves what this ring slot held kFrameSlots frames
        // ago - the wrong probe ring slot, silently.
        //
        // Absent = a type-correct null StructuredBuffer SRV of stride 192, built in
        // LightingPass::Init next to the probe-list pair.
        inline constexpr uint32_t kLightCB_ReflProbeBufIdx   = 812;

        // ---- exposureBufIdx (no constant) ----
        // uint LightCB::exposureBufIdx - AutoExposurePass's 1-element StructuredBuffer<float>.
        // It has no constant here and no offset static_assert on purpose: it took a former
        // padding slot in place, and its one consumer, custom_surface.hlsli's forward Unlit
        // branch, reads it by name out of the cbuffer, so there is no cross-language offset
        // contract to pin. Listed so the next fold does not spend a fresh row on it.
        //
        // Without the pass it is the stride-4 null (BINDLESS_NULL_STRUCTURED): the consumer
        // gates on BindlessNone(exposureBufIdx) and falls back to exposure 1.0 (an identity
        // counter-scale).

        // ---- Water domain ----
        // gWaterGlobals @816 (uint4), gWaterRTSize @832 (float4),
        // gWaterBodies  @848 (float4[16] = 256B) -> 1104.
        inline constexpr uint32_t kLightCB_WaterGlobals      = 816;
        inline constexpr uint32_t kLightCB_WaterRTSize       = 832;
        inline constexpr uint32_t kLightCB_WaterBodies       = 848;
        // Mirrors WaterRippleSystem::kMaxBodies and WATER_MAX_RIPPLE_BODIES in light_cb.hlsli;
        // 2 float4 rows per body.
        inline constexpr uint32_t kLightCB_MaxWaterBodies    = 8;

        // ---- Volumetric cloud shadow (1104 -> 1152) ----
        inline constexpr uint32_t kLightCB_CloudShadowRight  = 1104;
        inline constexpr uint32_t kLightCB_CloudShadowUp     = 1120;
        inline constexpr uint32_t kLightCB_CloudShadowIdx    = 1136;
        // PT-lit toon receivers' traced ambient + sun visibility (in the cloud row's pad).
        inline constexpr uint32_t kLightCB_NprPtAmbientIdx   = 1140;
        // Face frame records (also in the cloud row's pad): the per-material-slot float4
        // (HeadForward.xz, HeadRight.xz) the deferred toon reads for a PER_FRAME_PARAMS face.
        // 0 = unavailable.
        inline constexpr uint32_t kLightCB_FaceFrameIdx      = 1144;

        // ---- Forward translucent traced-reflection fold (1152 -> 1168) ----
        // GBuffer1 normal + SSRTraceDepth snapshot heap indices and the feature weight
        // (features.transparentReflections), read only by custom_surface.hlsli's translucent
        // main (MS_FetchTracedReflection). Both textures are graph physical textures recreated
        // on resize, so they are re-derived every frame in Renderer's resolve() block.
        // 0 = unbound.
        //   fwdReflNormalIdx -> Texture2D<float4>   (GBuffer1, N*0.5+0.5)
        //   fwdReflDepthIdx  -> Texture2D<float>    (reverse-Z NDC depth)
        inline constexpr uint32_t kLightCB_FwdReflNormalIdx  = 1152;
        inline constexpr uint32_t kLightCB_FwdReflDepthIdx   = 1156;
        inline constexpr uint32_t kLightCB_FwdReflStrength   = 1160;
        // prev-frame SceneColorPyramid -> Texture2D<float4>, mipped; 0 = none
        inline constexpr uint32_t kLightCB_FwdReflPrevHdrIdx = 1164;
        // ---- Forward translucent screen trace (1168 -> 1200) ----
        // Pyramid top mip, camera near/far (reverse-Z linearisation), the roughness band and
        // the world-space ray range of the translucent surface's own screen-space march
        // (MS_ScreenTraceReflection).
        inline constexpr uint32_t kLightCB_FwdReflPrevHdrMaxMip = 1168;
        inline constexpr uint32_t kLightCB_FwdReflNearZ         = 1172;
        inline constexpr uint32_t kLightCB_FwdReflFarZ          = 1176;
        inline constexpr uint32_t kLightCB_FwdReflRoughMax      = 1180;
        inline constexpr uint32_t kLightCB_FwdReflRayMax        = 1184;
        inline constexpr uint32_t kLightCB_FwdReflPrevJitterUV  = 1188;   // float2
        // The FRONT-LAYER trace (RTReflectionPass Variant::TranslucentFront output,
        // Texture2D<float4>, rgb = radiance, a = conf; 0 = no instance this frame), in the
        // row's last slot.
        inline constexpr uint32_t kLightCB_FwdReflFrontRtrIdx   = 1196;
        // ---- Forward translucent front layer (1200 -> 1216) ----
        // TranslucentFront_Depth (Texture2D<float>, reverse-Z NDC, sky = 0): the fold's
        // "am I the stamped front layer" test. 0 = unbound.
        inline constexpr uint32_t kLightCB_FwdReflFrontDepthIdx = 1200;
        // The screen AO texture LightingPass samples at t18 (Texture2D<float>),
        // folded by the forward BRDF with an opaque-proximity gate. 0 = none.
        inline constexpr uint32_t kLightCB_FwdAoIdx             = 1204;

        // ---- CSM cascade-selection camera (1216 -> 1248) ----
        // shadow.hlsli's ComputeCascadeShadowTerm picks a cascade from
        // `dot(worldPos - csmCamPos, csmCamFwd)`: the camera the cascades were fitted to, which
        // is not always the shading camera. A secondary view (PiP / camera preview) overrides
        // cameraPos / cameraForward for its own specular V while borrowing the main camera's
        // cascade matrices and splits, and leaves these at the main camera's values. The main
        // path writes them equal to cameraPos / cameraForward, at the same two sites.
        // float3 + one 4-byte field per row (HLSL cannot straddle a row).
        inline constexpr uint32_t kLightCB_CsmCamPos            = 1216;
        inline constexpr uint32_t kLightCB_CsmCamFwd            = 1232;

        // ReSTIR DI and PT result heap indices (the front-layer row's last two slots), the
        // render-path mode and the ReSTIR GI index (the CSM rows' fourth slots).
        // kLightCB_Size is the total LightCB size, asserted on the C++ mirror: bump it (and
        // the assert) whenever a fold appends a fresh 16B row.
        inline constexpr uint32_t kLightCB_RestirDiIdx     = 1208;
        inline constexpr uint32_t kLightCB_PtResultIdx     = 1212;
        inline constexpr uint32_t kLightCB_RenderPathMode  = 1228;
        inline constexpr uint32_t kLightCB_RestirGiIdx     = 1244;
        inline constexpr uint32_t kLightCB_Size            = 1248;

        // ---- DDGI::VolumeGPUDesc-carried indices ----
        // Byte offsets into DDGI::VolumeGPUDesc / HLSL `struct DDGIVolumeGPU`, NOT into LightCB.
        //
        // Each holds the heap index of the FIRST descriptor of a kMaxVolumes-wide block that
        // DescriptorHeapAllocator::AllocateStatic allocates once and never moves (volume
        // create / destroy rewrites descriptors in place: DDGIVolumeManager::
        // WriteSlotDescriptors / WriteSlotNullDescriptors). The shader reads
        // `base + volumeSlot`; volumeSlot is a per-pixel loop variable, so those reads need
        // NonUniformResourceIndex(). DDGIVolumeManager::Tick still rewrites them every frame
        // from DescriptorHeapIndexFromGpuHandle(<block base GPU handle>): three divisions keep
        // the never-cache-a-heap-index rule engine-wide.
        //
        // DECLARED TYPES (Tier 3 gives NO validation message on a mismatch):
        //   irradianceBaseIdx -> Texture2D<float4>               (R16G16B16A16_FLOAT)
        //   depthBaseIdx      -> Texture2D<float2>               (R16G16_FLOAT)
        //   probeDataBaseIdx  -> StructuredBuffer<DDGIProbeData> (stride 48)
        //
        // No compute shader reads this block: DDGISampling.hlsli takes all three resources as
        // function parameters, so only the pixel shader opens them.
        inline constexpr uint32_t kDDGIVolumeDesc_IrradianceBaseIdx   = 84;
        inline constexpr uint32_t kDDGIVolumeDesc_DepthBaseIdx     = 88;
        inline constexpr uint32_t kDDGIVolumeDesc_ProbeDataBaseIdx = 92;
    }

    // ==== Compute root signature: 18 parameters, 0..17 ====
    // Built by GraphicsDX12::CreateComputeRootSignature().
    //
    // Several names collide with Gfx:: names at DIFFERENT values (InstanceBuf 8 vs 9,
    // MeshDesc 9 vs 10, BindlessBuffers 14 vs 11). Never migrate a pass file by matching
    // constant names; match by the root signature that is bound.
    //
    // The index order is non-monotonic in register space (6 -> t5, 7 -> t3, 8 -> t4) and
    // ~20 shaders bind by these indices: do not "tidy" it.
    // The generic SRV/UAV slots are named by register, not by purpose: many passes bind
    // different resources to the same slot. Only slots with fixed contents carry a purpose
    // name.
    namespace Cmp
    {
        inline constexpr CmpParam CB              { 0 };  // ROOT_CBV b0 space2
        inline constexpr CmpParam SRV_t0          { 1 };  // t0 space2
        inline constexpr CmpParam SRV_t1          { 2 };  // t1 space2
        inline constexpr CmpParam SRV_t2          { 3 };  // t2 space2
        inline constexpr CmpParam UAV_u0          { 4 };  // u0 space2
        inline constexpr CmpParam UAV_u1          { 5 };  // u1 space2
        inline constexpr CmpParam SRV_t5          { 6 };  // t5 space2 (morph weights origin)
        inline constexpr CmpParam SRV_t3          { 7 };  // t3 space2 (scene depth origin)
        inline constexpr CmpParam SRV_t4          { 8 };  // t4 space2 (scene occupancy origin)
        inline constexpr CmpParam InstanceBuf     { 9 };  // t0 space0, fixed
        inline constexpr CmpParam MeshDesc        { 10 };  // t1 space0, fixed
        inline constexpr CmpParam BindlessBuffers { 11 };  // t0 space1, 16384 SRVs, fixed
        inline constexpr CmpParam SRV_t6          { 12 };  // t6 space2 (spot shadow atlas origin)
        inline constexpr CmpParam SRV_t7          { 13 };  // t7 space2 (spot shadow VP origin)
        inline constexpr CmpParam UAV_u2          { 14 };  // u2 space2
        inline constexpr CmpParam UAV_u3          { 15 };  // u3 space2
        inline constexpr CmpParam UAV_u4          { 16 };  // u4 space2
        inline constexpr CmpParam BindlessTex     { 17 };  // t0 space3, 16384 SRVs, fixed

        // Purpose aliases for the fixed-content generic slots. Same values.
        inline constexpr CmpParam SpotShadowAtlas = SRV_t6;
        inline constexpr CmpParam SpotShadowVP    = SRV_t7;

        inline constexpr uint32_t ParamCount      = 18;

        // Static samplers baked into the compute root signature (space2):
        //   s0 linear clamp, s1 comparison PCF (reversed-Z GREATER_EQUAL),
        //   s2 linear wrap (tileable noise / weather).
        inline constexpr uint32_t StaticSamplerCount = 3;
    }

    // ==== Shared descriptor-array capacities ====
    // ABI: they must match the HLSL array declarations and MeshDescriptorHeap's capacity.
    // g_Buffers[] (t0 space1): must match kMaxBuffers in MeshDescriptorHeap.h, g_Buffers[]
    // in pvf_fetch.hlsli and g_DDGIBuffers[] in DDGIRayTrace.cs.hlsl.
    inline constexpr uint32_t MaxBindlessBuffers  = 16384;
    // g_AllTextures[]: t0 space2 (graphics) / t0 space3 (compute).
    inline constexpr uint32_t MaxBindlessTextures = 16384;

} // namespace RHI::Slots
