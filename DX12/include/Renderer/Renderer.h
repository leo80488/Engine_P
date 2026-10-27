#pragma once
#include "Graphics/ResolvedSceneLight.h"
#include "Renderer/ReSTIRBudget.h"
#include "System/FrameTaskContext.h"
#include "System/SharedSnapshot.h"
#include "System/LeasedBuffer.h"

// Renderer - ECS -> DrawPacket producer and RenderGraph owner.
// PVF: per-attribute ByteAddressBuffer SRVs.
//
// This header names no DX12 type itself but does not isolate callers from the
// backend: it includes ECS/Components.h, MaterialCBVRing.h, MaterialSRVRing.h
// and DDGISceneAS.h and holds the latter three by value, so every TU that
// includes Renderer.h also sees <d3d12.h>. Isolating it needs those members
// behind a pimpl.

#include <atomic>
#include <cmath>
#include "Graphics/FrameConstants.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/TransientAliasArena.h"
#include "Graphics/RootSignatureLayout.h"  // RHI::Slots::BindlessIdx - LightCB folded-index offsets
#include "Graphics/FrameCB.h"
#include "Graphics/FogApply.h"                 // FogApply::Constants
#include "Graphics/ShadowSystem.h"             // ShadowSystem::FrameInput (CascadeInput)
#include "Graphics/MediumSet.h"                // Media::SetBuffer
#include "../../shaders/meshlet_shared.hlsli"  // MeshletCullCB (one definition, C++ + HLSL)
#include "../../shaders/shared/terrain_params.hlsli"  // TerrainParams (one definition, C++ + HLSL)
#include "Graphics/MeshManager.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RenderWorker.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTexture.h"                    // the post pool
#include "RenderGraph/RenderPass/ReSTIRBackend.h"      // backend selection vocabulary (by value)
namespace PTRtxdi { enum class DirectLight : uint8_t; }   // defined in RenderGraph/RenderPass/PTRtxdiCore.h
#include "RenderGraph/RenderPass/VolumetricFogPass.h"  // VolLight nested type used by-value
#include "RenderGraph/RenderPass/SkyboxPass.h"         // SkyState by value
#include "Graphics/SceneBVH.h"
#include "ECS/ECS.h"
#include "Resource/SystemHandles.h"

#include "ECS/HierarchyComponents.h"
#include "ECS/Components.h"
#include "ECS/ReflectionProbeComponent.h"   // ProbeBakeRequest::influenceShape
#include "Scene/SceneLoader.h"
#include "ECS/AnimationComponents.h"
#include "ECS/AnimationSystem.h"
#include "ECS/IKSystem.h"
#include "ECS/SocketSystem.h"
#include "ECS/FollowSystem.h"
#include "ECS/DecalSystem.h"
#include "ECS/DecalSpawner.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Resource/SkeletonAsset.h"
#include "Graphics/SkinningBuffers.h"
#include "Graphics/MaterialCBVRing.h"
#include "Graphics/MaterialSRVRing.h"
#include "Graphics/MaterialRuntime.h"
#include "RenderGraph/ViewState.h"  // the main view's cross-frame state
#include "Graphics/GPUInstanceData.h"
#include "Renderer/Preview/PreviewTypes.h"   // Preview::ViewDesc crosses the API
#include "Renderer/DebugTargets.h"           // the debug target view's API types
#include "Renderer/PassCapture.h"            // the pass capture's API types
#include "Renderer/ViewCull.h"               // CullView's request / stats
#include "Graphics/UpscaleMode.h"
// For SL::DLSSMode only - this header pulls in no Streamline SDK headers and
// compiles the same with or without the SDK present.
#include "Graphics/StreamlineBackend.h"
#include "Renderer/ShadowFrustumCompute.h"
#include "Renderer/FramePlan.h"
#include "Settings/SettingsSnapshot.h"      // the per-frame settings snapshot
#include "Renderer/ReflectionProbeManager.h"
#include "Renderer/RenderLifecycle.h"
#include "Renderer/PassSet.h"
#include "Renderer/FrameView.h"
#include "Renderer/RenderFeature.h"
#include "Renderer/SkinnedMeshSubsystem.h"
// SkinningPass / ClusterPass / DecalPass / ReflectionProbeCapturePass /
// SpotShadowPass are forward-declared below; full headers in Renderer.cpp.
#include "Resource/MeshLibrary.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/DecalMaterialLibrary.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Renderer/DDGISceneAS.h"
#include "Renderer/DXRParityTest.h"
#include "Graphics/MaterialSurfaceCache.h"
#include "ECS/DDGIComponents.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

#include "Resource/ProceduralMesh.h"  // PrimitiveMeshType
#include "Scene/MeshAssetRegistry.h"  // MeshAssetRegistry::Model (BuildPreviewModelView); light: no engine headers

namespace PostProcess { class PostChain; }
namespace ShaderReflect { struct Reflection; }

class World;
namespace Resource { class AssetManager; }   // -preview_asset_verify's spawn
class PickingPass;
class LightingPass;
class SkyboxPass;
class TransparentPass;
class ShadowPass;
class OutlinePass;
class OutlineGeometryPass;
class NPRStabilizePass;
class AutoExposurePass;
class LumaReducePass;
class BloomPass;
class LensFlarePass;
class ToneMapPass;
class TAAPass;
class FSR2Pass;
class DLSSPass;
class RRColorSnapshot;
class UpscalerReactiveMaskPass;
class FXAAPass;
class XeGTAOPass;
class CASPass;
class UnderwaterPass;
class DepthOfFieldPass;
class StylizePass;
class DebugTargetViewPass;
class TextureViewService;
class UIViewService;
class SkinningPass;
class ClusterPass;
class DecalPass;
class RTReflectionPass;
class PathTracePass;
class RTDIPass;
class LightSamplingPass;
class RtxdiProbe;
class RTGIPass;
class ReflectionProbeCapturePass;
class SpotShadowPass;
class PointShadowPass;
class PerObjectShadowPass;
class CloudPass;
class CullingPass;
class GBufferPass;
class SurfaceCacheBakePass;
class TerrainPass;
class GrassPass;
class WaterPass;
class WaterSurfacePass;
class DebugWirePass;
class HiZPass;
class SSRSubsystem;
class SceneVoxelPass;
class SkyIBLPass;
class DDGIPass;
class DDGIProbeDebugPass;
class UIPass;
class WorldUIBillboardPass;
class DebugIconPass;
class PreviewRenderer;

// CPU-side cbuffer mirrors for Renderer's per-frame UPLOAD-heap CBs. Public so
// FrameCB<T> can be instantiated in Renderer.h. Layouts must match the HLSL
// (see the top of Renderer.cpp).
namespace RendererDetail
{
    // viewProj is jittered for SV_POSITION; curViewProjNoJitter feeds velocity.
    // The time and camera rows feed custom vertex-shader hooks
    // (shaders/custom_vertex.hlsli) and follow the three matrices, so a consumer
    // that declares only the matrices keeps its offsets. The struct stays under
    // 256 bytes (one CBV). Mirror: custom_vertex.hlsli PerViewCB.
    struct alignas(16) PerViewCB
    {
        float viewProj[16];            // current, jittered (rasterization)
        float prevViewProj[16];        // previous frame, unjittered (velocity)
        float curViewProjNoJitter[16]; // current, unjittered (velocity numerator)
        float timeParams[4];           // x=time(sec) y=deltaTime z=sin(time) w=cos(time)
        float cameraPosWS[4];          // xyz = camera world position, w = unused
        // Offset 224 - engine-global dissolve noise bindless index (~0u = none).
        // The shadow passes' 256 B cascade CB slots carry it at the same offset, so
        // Shadow.ps reads it the same way (material_bindings.hlsli).
        uint32_t dissolveNoiseIdx;
        // Offset 228 - shader-visible heap index of this frame's InstanceRemap
        // buffer (stable-slot remap), read via ResourceDescriptorHeap (SM 6.6; the
        // root sig sets HEAP_DIRECTLY_INDEXED and the DXR requirement implies
        // Resource Binding Tier 3). 0 = identity mapping, so the retained path off
        // needs no shader permutation. Every b1 layout carries it: the shadow
        // cascade / caster / face CBs and the probe-capture CB at this offset,
        // FringeMarkCB at byte 128 of its own layout - see gpu_instance.hlsli.
        uint32_t instanceRemapIdx;
        // Offset 232 - the frame counter, phase of BlendMode::Dithered's temporal
        // screen-door (shaders/dither.hlsli). Main view only: the shadow b1 layouts
        // keep it as padding (they dither statically).
        uint32_t frameIndex;
        uint32_t _dissolvePad;
    };
    // Byte 228 is a cross-layout ABI contract: every b1 variant (this struct,
    // the shadow cascade / caster / face CB slots, ProbeCaptureCB) stores the
    // remap heap index there and every VS reads it there. A drift feeds the
    // bindless remap fetch a garbage heap index.
    static_assert(offsetof(PerViewCB, dissolveNoiseIdx) == 224 &&
                  offsetof(PerViewCB, instanceRemapIdx) == 228 &&
                  offsetof(PerViewCB, frameIndex)       == 232,
                  "PerViewCB dissolve/remap/frame fields must sit at bytes 224/228/232");

    // Mirror of light_cb.hlsli (member list: light_cb_fields.hlsli) - do not
    // reorder fields without changing both.
    struct alignas(16) LightCB
    {
        float    lightDir[3];    float pad0;
        float    lightColor[3];  float pad1;
        float    cameraPos[3];   float pad3;
        float    invViewProj[16];
        uint32_t iblRadianceMips;
        float    iblStrength;
        uint32_t iblUseSH;
        float    aerialMaxDistKm;
        float    shadowMatrix[4][16];
        float    cascadeSplits[4];
        float    cascadeTexelWorldSize[4];
        float    shadowBias;
        float    shadowStrength;
        float    shadowMapTexelSize;
        float    shadowBlendRange;
        uint32_t shadowFrameIndex;
        float    shadowNormalOffset;
        float    rtShadowEnabled;     // RT sun-shadow mask gate - mirror light_cb.hlsli
        // ssgiResultIdx is a shader-visible heap index, not a handle: Lighting.ps
        // opens ResourceDescriptorHeap[ssgiResultIdx]. 0 = no SSGI this frame; it is
        // also the kill switch (Lighting.ps then composes as without SSGI). Mirror:
        // shaders/light_cb.hlsli.
        // Write both SSGI fields unconditionally every frame: LightCB is a
        // triple-buffered ring, and a write guarded by `if (m_ssgiPass)` leaves an
        // index from kFrameCount frames ago, which under Resource Binding Tier 3
        // names a live but wrong descriptor with no D3D12 validation message.
        uint32_t ssgiResultIdx;       // heap index of SSGIPass's RGBA16F output (0 = off)
        float    cameraForward[3];
        float    ssgiIntensity;       // 0..1 kill switch / debug fade - not a brightness knob
        float    viewMatrix[16];
        float    clusterNearZ;
        float    clusterFarZ;
        uint32_t clusterLightCount;
        float    nprMinBrightness;
        uint32_t reflectionProbeCount;
        float    nprMaxBrightness;    // scene-wide absolute NPR HDR ceiling (0 = none)
        float    reflectionPad[2];
        uint32_t ddgiVolumeCount;
        uint32_t ddgiEnabled;
        float    ddgiDiffuseScale;
        float    _padSkyDiffuse;      // reserved padding (keeps the layout)
        float    ddgiAONearFieldStrength;
        uint32_t viewMode;            // global view-mode switch - mirrors light_cb.hlsli viewMode
        uint32_t exposureBufIdx;      // heap index of the AutoExposure buffer (0 = unavailable)
        float    ambientIntensity;    // WorldEnvironment flat-ambient scale (None/FlatColor)

        // ---- Per-object (player) high-res directional shadow ----
        // Mirror: light_cb.hlsli.
        float    perObjShadowMatrix[16];   // light VP (row-major, transposed) fitted to player AABB
        float    perObjShadowParams[4];    // x=enabled(0/1) y=texelWorld z=receiverBias w=normalOffset
        float    perObjShadowTexelSize;    // 1.0 / kPerObjShadowSize (PCF tap step)
        float    ambientColor[3];          // WorldEnvironment flat ambient (None/FlatColor)

        // ---- Hair (bangs -> face) directional shadow ----
        // Small map fitted to the HairShadowCasterComponent subtree (the bangs);
        // sampled only by NPR face pixels in Lighting.ps (SampleHairShadow,
        // shadow.hlsli, HAIR_SHADOW). Mirror: light_cb.hlsli - keep the field order
        // in lockstep.
        float    hairShadowMatrix[16];     // light VP (row-major, transposed) fitted to bangs AABB
        float    hairShadowParams[4];      // x=enabled(0/1) y=texelWorld z=receiverBias w=normalOffset
        float    hairShadowTexelSize;      // 1.0 / kHairShadowMapSize (PCF tap step)
        float    hairShadowStrength;       // artistic strength on the face (HairShadowCasterComponent)

        // ---- Folded-to-bindless descriptor heap indices ----
        // Heap indices instead of root descriptor tables: the graphics root
        // signature is near the D3D12 limit of 64 DWORDs, and an index read through
        // ResourceDescriptorHeap[idx] costs none (see RootSignatureLayout.h's ceiling
        // note).
        // Written every frame from the resource's current GPU handle, never cached
        // (a resize recreates textures and handle ids are recycled). An absent
        // resource = the index of LightingPass's real 1x1 fallback texture.
        uint32_t rtShadowMaskIdx;   // RTShadowPass mask
        uint32_t rtReflectionIdx;   // RTReflectionPass output

        // ---- Shadow-family heap indices ----
        // Same rules as above: resolved every frame from the live GPU handle, never
        // cached; absent = the heap index of LightingPass's real 1x1 fallback, not a
        // sentinel. Each is read inside an existing gate (light.shadowSliceIdx /
        // perObjShadowParams.x / hairShadowParams.x).
        uint32_t pointShadowIdx;    // PointShadowPass TextureCubeArray<float>
        uint32_t perObjShadowIdx;   // PerObjectShadowPass Texture2D<float>
        uint32_t hairShadowIdx;     // hair (bangs -> face) Texture2D<float>

        // ---- DDGI ----
        // The engine-wide StructuredBuffer<DDGIVolumeGPU>, the entry to every DDGI
        // resource: each element carries its volume's base heap indices
        // (VolumeGPUDesc::irradianceBaseIdx / depthBaseIdx / probeDataBaseIdx), so the
        // shader opens this buffer, reads a volume, then opens that volume's three
        // blocks. The buffer is an UPLOAD ring (one slot per frame in flight): the
        // index is resolved every frame from DDGIVolumeManager::GetVolumeBufferSrv(gfx)
        // in the resolve() block - a cached index would pin one ring slot. Read only
        // inside `ddgiEnabled != 0 && ddgiVolumeCount > 0`; with DDGI off Renderer
        // writes 0 (heap index 0 is a real descriptor, never read).
        uint32_t ddgiVolumeBufIdx;  // StructuredBuffer<DDGIVolumeGPU>

        // ---- Per-cluster reflection-probe list ----
        // ClusterPass DEFAULT-heap buffers whose SRVs ClusterPass::Init creates once;
        // still re-resolved every frame in the resolve() block, because no heap index
        // is ever cached. Read by Lighting.ps inside `reflectionProbeCount > 0`; not
        // indexed by a loop variable, so no NonUniformResourceIndex(). Absent =
        // LightingPass's typed null structured-buffer SRVs (stride 8 / 4), not the
        // cube-array filler: a cube-array descriptor opened as a StructuredBuffer is
        // UB that Tier 3 never reports.
        uint32_t reflProbeGridIdx;  // StructuredBuffer<ProbeGridEntry>, stride 8
        uint32_t reflProbeIndexIdx; // StructuredBuffer<uint>, stride 4

        // ---- SSR result ----
        // SSRUpsamplePass's colour output (rgb = resolved reflection, a =
        // confidence), Texture2D<float4>. Only Water.ps reads it, inside its
        // `!underwater && prevClip.w > 1e-3` gate, to dampen the sky term by
        // (1 - conf). A CB value, never a loop variable: no NonUniformResourceIndex().
        // The texture is recreated at runtime: SSRSubsystem::OnResize rebuilds
        // SSRUpsamplePass::m_colorTex on a render-resolution change and frees the old
        // descriptor. Renderer::BeginFrame calls OnResize and pushes the new handle
        // into WaterPass before the resolve() block runs, so the index names the
        // current texture. Do not cache it: a cached index samples a freed descriptor
        // the frame after every resize. Absent (SSR off / no result yet) = WaterPass's
        // 1x1 black Texture2D<float4>, alpha 0 -> no dampening.
        uint32_t ssrResultIdx;      // Texture2D<float4>

        // ---- Reflection-probe buffer ----
        // ReflectionProbeManager's StructuredBuffer<GpuShared::ReflectionProbe>, stride 192.
        // Read by Lighting.ps (inside `reflectionProbeCount > 0`) and by
        // custom_surface.hlsli's MS_IS_FORWARD IBL block. A CB value, never a loop
        // variable: no NonUniformResourceIndex(). reflection_probe.hlsli is also
        // compiled under RTReflectionPass's own root signature (HybridReflIntersect
        // HW/SW.cs, probe buffer bound at t25); only the graphics path, which leaves
        // REFL_PROBE_BUFFER_REGISTER undefined, reads this field.
        // GetBufferSrv(gfx) picks a slot of a 3-slot UPLOAD ring by
        // gfx.GetFrameIndex(), so the index changes every frame. Write it
        // unconditionally, outside the `if (m_lightingPass)` block: LightCB is itself
        // a ring, and a skipped write names the probe ring slot from kFrameSlots
        // frames ago. Absent (buffer creation failed) = LightingPass's typed null
        // StructuredBuffer SRV (stride 192), not the t23 cube array.
        uint32_t reflProbeBufIdx;   // StructuredBuffer<GpuShared::ReflectionProbe>, stride 192

        // ---- Water domain ----
        // HLSL mirror: gWaterGlobals / gWaterRTSize / gWaterBodies in light_cb.hlsli,
        // read by water_common.hlsli (every WATER_SURFACE permutation).
        //
        // waterGlobals[0] = refraction source bindless index (0 = unbound)
        // waterGlobals[1] = scene depth snapshot bindless index (0 = unbound)
        // waterGlobals[2] = valid entries in waterBodies (0..kLightCB_MaxWaterBodies)
        // waterGlobals[3] = reserved
        //
        // 0 means unbound, not 0xFFFFFFFF: bindless index 0 is a real descriptor, so
        // the helpers test `== 0` and bail instead of sampling slot 0's unrelated
        // texture.
        uint32_t waterGlobals[4];
        float    waterRTSize[4];    // .xy = render-res width/height, .zw reserved
        // 2 rows per body: [2i+0] origin.xyz/size, [2i+1] window origin.xy /
        // window size / asfloat(ripple field bindless index).
        float    waterBodies[RHI::Slots::BindlessIdx::kLightCB_MaxWaterBodies * 2][4];

        // ---- Volumetric cloud shadow ----
        // HLSL mirror: gCloudShadowRight / gCloudShadowUp / gCloudShadowIdx in
        // light_cb.hlsli, read through shaders/cloud_shadow.hlsli. The two basis rows
        // are pre-divided by the projection extent with the centre folded into .w,
        // so a consumer's UV is two dot products. Both are perpendicular to the sun
        // direction, which makes the projection altitude-invariant.
        float    cloudShadowRight[4];   // xyz = right / (2*extent), w = 0.5 - dot(centre, xyz)
        float    cloudShadowUp[4];      // xyz = up    / (2*extent), w = 0.5 - dot(centre, xyz)
        // 0 = unbound (clouds off, bake did not run, or strength 0): every consumer
        // returns full sun. Index 0 is a real descriptor, so test for it explicitly
        // (as waterGlobals).
        uint32_t cloudShadowIdx;
        // PT-lit toon receivers (HLSL gNprPtAmbientIdx): PathTracePass's denoised
        // NprAmbient output, 0 = none.
        uint32_t nprPtAmbientIdx;
        // Face frame records (HLSL gFaceFrameIdx): this frame's m_matFrameRecord SRV,
        // StructuredBuffer<float4> by material slot. 0 = unavailable; read only where
        // materialFlags has the record bit.
        uint32_t faceFrameIdx;
        // HLSL gTransmissionApplied: the transmission apply ran this frame (a render
        // feature's ITransmissionComposite).
        uint32_t transmissionApplied;

        // ---- Forward translucent traced-reflection fold ----
        // HLSL mirror: gFwdReflNormalIdx / gFwdReflDepthIdx / gFwdReflStrength in
        // light_cb.hlsli, read only by custom_surface.hlsli's translucent main
        // (MS_FetchTracedReflection). Both indices are re-resolved every frame from
        // the graph's physical GBuffer1 / SSRTraceDepth textures (a resize recreates
        // them) in the resolve() block. 0 = unbound (heap slot 0 is a real
        // descriptor).
        uint32_t fwdReflNormalIdx;  // GBuffer1 normal, Texture2D<float4>
        uint32_t fwdReflDepthIdx;   // SSRTraceDepth snapshot, Texture2D<float>
        float    fwdReflStrength;   // features.transparentReflections ? 1 : 0 (negative = debug paint)
        uint32_t fwdReflPrevHdrIdx; // prev-frame SceneColorPyramid, Texture2D<float4>; 0 = none

        // ---- Forward translucent screen trace ----
        // HLSL mirror: gFwdReflPrevHdrMaxMip / NearZ / FarZ / RoughMax / RayMax, read
        // only by MS_ScreenTraceReflection. All rewritten every frame in the
        // resolve() block (camera planes and the two RTR tunables are live values).
        float    fwdReflPrevHdrMaxMip;
        float    fwdReflNearZ;
        float    fwdReflFarZ;
        float    fwdReflRoughMax;
        float    fwdReflRayMax;
        float    fwdReflPrevJitterUV[2];   // last frame's jitter / render res (UV units)
        // RTReflectionPass(TranslucentFront) output - the trace from the glass
        // (MS_FetchFrontLayerReflection). Texture2D<float4>; 0 = none.
        uint32_t fwdReflFrontRtrIdx;

        // ---- Forward translucent front layer ----
        // TranslucentFront_Depth, Texture2D<float> (reverse-Z, sky 0): the fold's
        // front-layer identity test. Re-derived every frame from the graph's physical
        // texture, 0 = unbound.
        uint32_t fwdReflFrontDepthIdx;
        // The AO texture LightingPass samples at t18 this frame (RTAO / XeGTAO),
        // for the forward BRDF's proximity-gated AO fold. 0 = none.
        uint32_t fwdAoIdx;
        uint32_t restirDiIdx;
        uint32_t ptResultIdx;

        // ---- CSM cascade-selection camera ----
        // HLSL mirror: gCsmCamPos / gCsmCamFwd. shadow.hlsli selects a cascade from
        // dot(worldPos - csmCamPos, csmCamFwd): the camera the cascades were fitted
        // to, not always the shading camera. The main view writes them equal to
        // cameraPos / cameraForward; a secondary view overrides cameraPos /
        // cameraForward for its own specular V and leaves these at the borrowed main
        // camera's.
        float    csmCamPos[3];
        uint32_t renderPathMode;
        float    csmCamFwd[3];
        uint32_t restirGiIdx;
    };

    // The HLSL mirror is shaders/light_cb.hlsli and nothing checks that the two
    // agree at build time, so pin the C++ side against RootSignatureLayout.h,
    // which declares both the root-parameter layout and the folded-index offsets.
    // A drift feeds Lighting.ps a garbage heap index, which under
    // HEAP_DIRECTLY_INDEXED reads whatever descriptor sits there instead of
    // failing.
    static_assert(offsetof(LightCB, restirDiIdx) == RHI::Slots::BindlessIdx::kLightCB_RestirDiIdx);
    static_assert(offsetof(LightCB, ptResultIdx) == RHI::Slots::BindlessIdx::kLightCB_PtResultIdx);
    static_assert(offsetof(LightCB, renderPathMode) == RHI::Slots::BindlessIdx::kLightCB_RenderPathMode);
    static_assert(offsetof(LightCB, restirGiIdx) == RHI::Slots::BindlessIdx::kLightCB_RestirGiIdx);
    static_assert(offsetof(LightCB, nprPtAmbientIdx) == RHI::Slots::BindlessIdx::kLightCB_NprPtAmbientIdx);
    static_assert(offsetof(LightCB, faceFrameIdx) == RHI::Slots::BindlessIdx::kLightCB_FaceFrameIdx);
    static_assert(sizeof(LightCB) == RHI::Slots::BindlessIdx::kLightCB_Size,
                  "LightCB size changed — update light_cb.hlsli AND "
                  "RHI::Slots::BindlessIdx::kLightCB_Size together");
    static_assert(offsetof(LightCB, rtShadowMaskIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_RTShadowMaskIdx &&
                  offsetof(LightCB, rtReflectionIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_RTReflectionIdx,
                  "LightCB bindless index fields moved — shadow.hlsli / "
                  "Lighting.ps.hlsl read them at these byte offsets");
    static_assert(offsetof(LightCB, pointShadowIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_PointShadowIdx &&
                  offsetof(LightCB, perObjShadowIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_PerObjShadowIdx &&
                  offsetof(LightCB, hairShadowIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_HairShadowIdx,
                  "LightCB fold-batch-2 index fields moved — shadow.hlsli / "
                  "Lighting.ps.hlsl read them at these byte offsets");
    static_assert(offsetof(LightCB, ddgiVolumeBufIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_DDGIVolumeBufIdx,
                  "LightCB fold-batch-3 index field moved — Lighting.ps.hlsl "
                  "opens the DDGI volume buffer at this byte offset");
    static_assert(offsetof(LightCB, reflProbeGridIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_ReflProbeGridIdx &&
                  offsetof(LightCB, reflProbeIndexIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_ReflProbeIndexIdx,
                  "LightCB fold-batch-4 index fields moved — Lighting.ps.hlsl "
                  "opens the per-cluster probe grid / index list at these "
                  "byte offsets");
    static_assert(offsetof(LightCB, ssrResultIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_SSRResultIdx,
                  "LightCB fold-batch-5 index field moved — Water.ps.hlsl opens "
                  "the SSR result texture at this byte offset");
    static_assert(offsetof(LightCB, reflProbeBufIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_ReflProbeBufIdx,
                  "LightCB fold-batch-6 index field moved — Lighting.ps.hlsl and "
                  "custom_surface.hlsli open the reflection-probe buffer at this "
                  "byte offset");
    static_assert(offsetof(LightCB, waterGlobals)
                      == RHI::Slots::BindlessIdx::kLightCB_WaterGlobals &&
                  offsetof(LightCB, waterRTSize)
                      == RHI::Slots::BindlessIdx::kLightCB_WaterRTSize &&
                  offsetof(LightCB, waterBodies)
                      == RHI::Slots::BindlessIdx::kLightCB_WaterBodies,
                  "LightCB water rows moved — water_common.hlsli reads the scene "
                  "depth/colour bindless indices and the ripple body table at "
                  "these byte offsets; a drift feeds it garbage heap indices");
    static_assert(offsetof(LightCB, fwdReflNormalIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflNormalIdx &&
                  offsetof(LightCB, fwdReflDepthIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflDepthIdx &&
                  offsetof(LightCB, fwdReflStrength)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflStrength &&
                  offsetof(LightCB, fwdReflPrevHdrIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflPrevHdrIdx &&
                  offsetof(LightCB, fwdReflPrevHdrMaxMip)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflPrevHdrMaxMip &&
                  offsetof(LightCB, fwdReflNearZ)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflNearZ &&
                  offsetof(LightCB, fwdReflFarZ)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflFarZ &&
                  offsetof(LightCB, fwdReflRoughMax)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflRoughMax &&
                  offsetof(LightCB, fwdReflRayMax)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflRayMax &&
                  offsetof(LightCB, fwdReflPrevJitterUV)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflPrevJitterUV &&
                  offsetof(LightCB, fwdReflFrontRtrIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflFrontRtrIdx &&
                  offsetof(LightCB, fwdReflFrontDepthIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdReflFrontDepthIdx &&
                  offsetof(LightCB, fwdAoIdx)
                      == RHI::Slots::BindlessIdx::kLightCB_FwdAoIdx,
                  "LightCB forward-reflection rows moved — custom_surface.hlsli "
                  "opens the GBuffer normal / depth snapshot / colour pyramid "
                  "at these byte offsets");
    static_assert(offsetof(LightCB, csmCamPos)
                      == RHI::Slots::BindlessIdx::kLightCB_CsmCamPos &&
                  offsetof(LightCB, csmCamFwd)
                      == RHI::Slots::BindlessIdx::kLightCB_CsmCamFwd,
                  "LightCB CSM cascade-selection camera moved - shadow.hlsli "
                  "reads gCsmCamPos / gCsmCamFwd at these byte offsets");

    // The terrain constant buffer (b2) the five Terrain stages read, defined once
    // for C++ and HLSL in shaders/shared/terrain_params.hlsli. Geometry only -
    // per-layer material data is the variable-count
    // StructuredBuffer<TerrainLayerGPU> below. 176 bytes; only the colour AS reads
    // frustumPlanes.

    // One element of StructuredBuffer<TerrainLayerGPU> g_TerrainLayers, read only
    // by Terrain.ps.hlsl; one per terrain layer (the PS loops [0, layerCount)).
    // No SIMD-lane packing: extend by adding a field, never by widening an int4.
    // Layout / stride must match the HLSL struct (48 bytes).
    struct TerrainLayerGPU
    {
        int32_t albedoIdx;
        int32_t normalIdx;
        int32_t armIdx;
        float   tilingScale;
        float   minHeight;
        float   maxHeight;
        float   fadeHeight;
        float   minSlopeDeg;
        float   maxSlopeDeg;
        float   fadeSlopeDeg;
        int32_t dispIdx;              // displacement map for height-correlated blend (-1 = none)
        float   _pad1;
    };
}

// Per-candidate material predicates, cached at gather time on
// Renderer::DrawCandidate::matBits. Namespace scope so the gather helpers can be
// free functions - DrawCandidate itself is private to Renderer.
enum MatBit : uint16_t
{
    MB_HAS_MATERIAL    = 1u << 0,
    MB_ALPHA_TEST      = 1u << 1,
    MB_TRANSPARENT     = 1u << 2,   // IsTransparentBlendMode(userBlendMode)
    MB_WATER           = 1u << 3,
    MB_CUSTOM_SHADER   = 1u << 4,
    MB_FRAME_PARAMS    = 1u << 5,   // HasPerFrameParams(): keyed by identity, b8 + frame record per frame
    MB_IDENTITY_UV     = 1u << 6,   // texMulAdd == (1,1,0,0)
    MB_AUTHORED_RT     = 1u << 7,   // standard slots are the authored RT surface
    // IsCastingShadow() - the material layer of cast-shadow, which
    // TrackSpotShadowCasters reads on the render-owner lane, where `mc` may not
    // be dereferenced. The ninth bit is why matBits is 16 wide.
    MB_CAST_SHADOW     = 1u << 8,
};

// Per-Renderer pool capacities, a constructor parameter so a second
// Renderer-shaped consumer can size its own pools. Every caller uses the
// defaults.
//
// Sizes these drive:
//   maxInstances x sizeof(GPUInstanceData)        UPLOAD staging ring + DEFAULT
//   maxInstances x sizeof(IndirectDrawCommand)    indirect args (several)
//   maxMaterials x sizeof(Resource::MaterialGPUData)  material table x (3 + 1)
struct RendererPoolCaps
{
    uint32_t maxInstances = 32768;
    uint32_t maxMaterials = 4096;
};

class Renderer : public IMaterialRuntimeLookup
{
public:
    struct DestroyDeferStats
    {
        uint64_t deferred = 0, applied = 0, immediate = 0, skippedAlive = 0;
        size_t   peak = 0;
        uint64_t callsDeferred = 0, callsApplied = 0, callsImmediate = 0;
        size_t   callsPeak = 0;
    };

    struct DeferredEntityDestroy
    {
        Entity               e = NullEntity;
        bool                 hasSkin = false;
        MeshSkinnedComponent skin;                  // captured at destroy time
        uint32_t             beamSlot = 0xFFFFFFFFu; // captured at destroy time
    };

    struct DeferredSyncCall
    {
        size_t                beforeDestroy = 0;
        std::function<void()> fn;
    };

    struct MeshletPathReport
    {
        const char* path = "vs";   // "vs" | "ms_cpu" | "ms_indirect"
        std::string reason;        // empty when a meshlet path drew it
    };

    struct MaterialCostRow
    {
        enum : std::uint8_t { GBuffer = 0, Forward, Water };   // the pass that draws it
        std::uint32_t slot = 0;              // the material slot = GBuffer1.a's matIdx
        std::uint8_t  pass = GBuffer;        // GBufferPass / TransparentPass / WaterSurfacePass
        std::uint32_t psId = 0;              // a ShaderID, or a dynamic id (a custom pixel shader)
        std::uint32_t permBits = 0;          // after ShaderLibrary::KeyBits: what its blob is keyed by
        bool          compiled = false;
        std::uint32_t instructions = 0;      // 0 when it does not compile
        std::uint32_t packets = 0, instances = 0;
        std::string   file, entry, blob;     // details: the source, its entry point, the shader cache blob
        Entity        entity = NullEntity;   // details: one entity drawn with it
    };

    struct MeshletLodReport
    {
        struct Level { uint32_t clusters = 0, triangles = 0; float selfError = 0, parentError = 0, projected = 0; };

        bool     valid = false;
        float    tau = 0.0f;
        int32_t  selected = -1;           // -1 = none (no meshlets / LOD off answers 0)

        std::vector<Level> levels;
    };

    struct LoadedMeshLibInfo
    {
        Resource::Handle handle;
        std::string      path;
        uint32_t         refs = 0, entries = 0, meshes = 0;
        uint32_t         vertices = 0, indices = 0, stride = 0;
        uint64_t         vramBytes = 0;          // VB + IB (u32 indices)
        uint32_t         worldRefs = 0;
        bool             preview = false, editorPinned = false, pendingRelease = false;
        uint32_t         users = 0;
        const char*      identity = "none";      // "file" | "synthesized" | "none"
        // The library's resident meshlets.
        const char*      meshlets = "none";      // MeshLibrary::MeshletStateName
        uint32_t         meshletClusters = 0, meshletLevels = 0, meshletMaxLevels = 0;
        uint64_t         meshletBytes = 0;       // the resident meshlet buffer (VRAM)
    };

    // ---- AA mode ----
    enum class AAMode : uint32_t
    {
        None     = 0,
        FXAA     = 1,
        TAA      = 2,
        FXAA_TAA = 3,   // TAA first, FXAA on TAA's resolved output
        FSR      = 4,   // AMD FidelityFX FSR2 - its own temporal AA + optional
                        // upscale; replaces the engine TAA/FXAA. Render-scale
                        // ratio is picked separately via SetUpscaleMode.
        DLSS     = 5,   // NVIDIA DLSS Super Resolution. Same deal as FSR: own
                        // temporal AA, replaces engine TAA/FXAA, render scale
                        // from SetUpscaleMode. Selectable only when the
                        // Streamline runtime reports the feature supported -
                        // see StreamlineBackend. At Native scale this is DLAA.

        DLSS_RR  = 6,   // Opt-in native GBuffer-primary PT reconstruction.

        Count           // keep last - kMaxAAMode is derived from it
    };
    // Anything past this is not a valid AAMode. Every bound check on a
    // deserialised or UI-sourced mode index must use this, never a literal.
    // Derived from Count, not pinned to the last named mode, so appending a mode
    // moves the bound (EngineSettings' ApplyTo and load-validator clamps, the
    // editor's combo static_assert); a frozen bound silently reverts a new mode
    // to TAA on load. Mirrors ViewMode::Count.
    static constexpr uint32_t kMaxAAMode = static_cast<uint32_t>(AAMode::Count) - 1;

    // Two path-tracing paths, separate in options and processing: PathTrace is
    // the reference - the engine path tracer accumulating progressively (Area
    // reuse, camera rays, a thin lens, the uncapped bounce count; no ray
    // reconstruction: it converges instead) - and PathTraceRealtime is RTXDI's
    // ReSTIR PT with the SHaRC cache, a fresh estimate every frame for DLSS Ray
    // Reconstruction. PathTrace keeps index 4 (settings.json "PathTrace", packs'
    // Settings.set("features.renderPath", 4)).
    enum class RenderPath : uint32_t { Raster, ReSTIR_DI, ReSTIR_GI, ReSTIR_DI_GI, PathTrace, PathTraceRealtime, Count };
    static constexpr RenderPath kMaxRenderPath = RenderPath::PathTraceRealtime;

    enum class DevPin : uint32_t { PTMaxBounces, PTReferenceArm, PTCameraPrimary, PTFilmJitter, PTLensRadius, PTAreaMotion,
                                   RestirDiReuse, RestirDiSpatialPasses, RestirDiSpatialRadius, RestirDiAgeCap,
                                   RestirGiSpatialRadius, RestirGiSharc };

    struct GISharcStatus
    {
        bool requested = false, effective = false;
        const char* reason = "off";   // a static string
        uint32_t mode = 0;
        uint64_t bytes = 0;
        uint32_t clears = 0;
    };

    // ---- Global view mode (Unity / Unreal-style view-mode switch) ----
    // Applies to all objects (opaque / terrain via the deferred LightingPass
    // branch; transparent via its forward shader). Wireframe also forces
    // FILL_MODE_WIREFRAME on the geometry passes and suppresses the background
    // passes (skybox / clouds / fog) and TAA for a clean dark background.
    // Mirror of shaders/view_mode_common.hlsli: the values are uploaded raw in
    // LightCB.viewMode, so they must match the VIEW_MODE_* defines there.
    enum class ViewMode : uint32_t
    {
        Lit       = 0,
        Unlit     = 1,
        Wireframe = 2,

        // ---- Lighting-taxonomy debug views ----
        // Each isolates one bucket of AggregateLighting (shaders/lighting_types.hlsli):
        // sun-coloured energy surviving in a fully shadowed NPR pixel is invisible in
        // a Lit view and obvious in DirectDiffuse alone. They reuse values of the
        // existing viewMode field on purpose: LightCB's tail 16 B row is full, so a
        // new field would cost a fresh row plus three synchronised bumps.
        DirectDiffuse      = 3,
        DirectSpecular     = 4,
        DirectTransmission = 5,
        IndirectDiffuse    = 6,
        IndirectSpecular   = 7,
        DirectOnly         = 8,
        IndirectOnly       = 9,
        // The two visibility quantities, side by side: PhysicalVisibility is what an
        // occlusion query returned, FormShadow is authored surface shading. Do not
        // conflate them.
        PhysicalVisibility = 10,
        FormShadow         = 11,

        // ---- GBuffer channel visualisation ----
        // Raw GBuffer contents, no BRDF - the first look at a pixel of the wrong
        // colour. Same viewMode reuse as above. No Velocity view: LightingPass does
        // not bind the velocity target.
        BaseColor        = 12,
        WorldNormal      = 13,
        Roughness        = 14,
        Metallic         = 15,
        AmbientOcclusion = 16,
        Depth            = 17,

        // ---- Meshlet views ----
        // Every opaque packet draws with MeshletDebug.ps: a mesh-pipeline draw shows
        // a hash colour per cluster (Meshlets) or a colour per LOD level
        // (MeshletLod), a GBuffer.vs draw flat grey. LightCB.viewMode receives
        // BaseColor for both - Lighting.ps shows the albedo raw and needs no case of
        // its own (Render_BindFrameResources / the LightCB write).
        Meshlets         = 18,
        MeshletLod       = 19,

        // ---- The sky's share of the indirect light ----
        // What the sky-IBL fallback is multiplied by (R diffuse, G specular, B the
        // GI-owned share of the diffuse hemisphere) and what it delivers.
        // Lighting-debug views like DirectDiffuse..FormShadow, appended because the
        // values are append-only.
        SkyVisibility    = 20,
        IblOnly          = 21,

        Count
    };

    // ---- Lifecycle ----
    // @p caps: the pool sizes; every caller passes the defaults.
    explicit Renderer(IGraphicsDevice& gfx, const RendererPoolCaps& caps = {});
    ~Renderer();

    // Build GPU resources, init passes, register them with the RenderGraph.
    void Compile();
    // Invalidate per-entity caches; call before World::Clear().
    // @p idsPreserved: true only from the Play -> Stop edge, where the same world
    // is restored from a snapshot and entity ids keep their meaning. Only
    // DDGISceneAS uses it (skinned BLAS retention, DDGISceneAS::OnWorldClear);
    // everything else clears unconditionally.
    // @p worldSurvives: the bake-preview-restore flavour - descriptor caches are
    // flushed but the World keeps rendering, so the skinned-buffer ownership
    // sweep is skipped (it would queue live buffers for destruction).
    void OnWorldClear(bool idsPreserved = false, bool worldSurvives = false);

    // ---- Deferred render-side destroys ----
    // With the render-owner lane on, an entity destroyed while a lane frame is in
    // flight does not drain the lane: the render-side cleanup (instance slot,
    // skinned GPU slots / buffers, particle / billboard texture caches,
    // afterimage spawns, beam slot) is recorded, with the components it needs
    // captured at destroy time, and ApplyStructuralSyncPoint applies it at the
    // structural sync point, where App has just drained the lane. The game-half
    // cleanup (the binding release queue, chain physics) stays immediate. The
    // retained-scene untrack and the DDGI record are deferred with the render
    // side (ApplyEntityDestroySharedState), because the lane reads
    // m_sceneStore.tracked (IsStaticLaneInstance in PlanEmit / EmitBatches) and
    // drains the DDGI record list in BuildOrRefit. The World quarantines the ids
    // until App releases them after the apply. -destroyimmediate is the escape
    // (drain the lane, destroy now).
    void SetDeferEntityDestroys(bool on) { m_deferEntityDestroys = on; }
    bool DeferEntityDestroys() const { return m_deferEntityDestroys; }
    // true while the lane has a frame in flight (FramePipeline::RenderOwnerFrameInFlight).
    void SetRenderFrameInFlightQuery(std::function<bool()> q) { m_renderFrameInFlight = std::move(q); }

    // ---- The structural mailbox ----
    // `fn` writes render-side state from the game half (a Lua request, the PiP
    // rect). While a lane frame is in flight and the mailbox is on, it is queued
    // and runs at the sync point, in request order with the deferred destroys;
    // otherwise `drain` runs first and `fn` runs now. Main thread only; returns
    // true when `fn` ran now. Not for an API whose caller reads the effect back
    // in the same call: Settings.apply keeps its drain, since restir_switch_20 /
    // renderpath_switch read the effective path right after.
    // -mailboximmediate is the escape.
    bool RunOrDeferToSyncPoint(std::function<void()> fn, const std::function<void()>& drain);
    void SetDeferSyncPointCalls(bool on) { m_deferSyncCalls = on; }
    bool DeferSyncPointCalls() const { return m_deferSyncCalls; }
    // The sync point: deferred destroys + mailbox calls, FIFO. worldBoundary
    // (OnWorldClear) applies every destroy record regardless of liveness.
    void ApplyStructuralSyncPoint(bool worldBoundary = false);
    // Captures the frame's intent (Renderer/FramePlan.h) - the render path after
    // the budget ladder, the AA and view modes, the feature switches as applied -
    // into the frame's FrameView, once per frame. App calls it on the game half
    // right after RenderPhaseBarrier, while the render half is not running;
    // anything the game half does later this frame (the editor UI, the scene
    // build) lands in the next frame's plan, on both halves. Also plans the game
    // half's stage A (GamePlan).
    void CaptureFrameIntent();
    // The game half's plan for this frame: stage A, from the intent
    // CaptureFrameIntent just took and the state as of then. The render
    // resolution (here and in App_Render.cpp), the upscaler contexts, the jitter
    // (UploadFrameData) and the resource service read rr / fsr / dlssWanted from
    // it, so both halves decide them from one function and one intent.
    const FramePlan::Decisions& GamePlan() const { return m_gamePlan; }
    // The plan of the last frame the render half finished, every stage of it
    // (stage A alone is GamePlan): CaptureFrameIntent copies m_plan here while
    // the render half is not running. Main thread (Lua Render.plan()).
    const FramePlan::Decisions& LastRenderedPlan() const { return m_planRendered; }
    // The same frame's volumetric fog predicate (FramePlan::State::fog, read at
    // the render half's top: VolumetricFogPass::WantsFog). Lua Render.plan().fog.
    bool LastRenderedFog() const { return m_planRenderedFog; }

    const DestroyDeferStats& GetDestroyDeferStats() const { return m_destroyDeferStats; }
    // Editor Play snapshot resource lifetime. Capture pins every MeshLibrary
    // referenced by the pristine editor world so runtime scene transitions
    // cannot invalidate the detached ECS snapshot's handles. After Stop has
    // called OnWorldClear and AdoptState, Restore transfers those pins back to
    // normal world ownership and rebuilds world-scoped mesh descriptors.
    void CaptureEditorWorldResources(const World& world);
    void RestoreEditorWorldResources(World& world);
    // Editor undo - GPU-slot lifetime across a delete that can be undone:
    //   ParkEntitiesForUndo:    before the delete, keep skinned-mesh descriptor
    //                           slots alive so a resurrect reuses them.
    //   OnEntitiesRestored:     after a resurrect, un-park skinned meshes and
    //                           re-resolve static MeshLibRef descriptor caches.
    //   FreeUndoParkedEntities: the undo action was dropped without resurrecting
    //                           - release the parked slots for good.
    void ParkEntitiesForUndo(const std::vector<Entity>& ents);
    void OnEntitiesRestored(const std::vector<Entity>& ents);
    void FreeUndoParkedEntities(const std::vector<Entity>& ents);
    // Invalidate temporal history on a hard cut, e.g. an Edit <-> Play switch,
    // which jumps the viewport between the editor fly camera and the game camera
    // stack (docs/design/editor_runtime_mode_architecture.md). Raises the main
    // view's HardCut for the frame being built (RaiseViewBreak): TAA, the
    // upscalers, SSR, ReSTIR and the path tracer reset so the first frame after
    // the cut blends no stale history. GTAO, volumetric fog, SSGI and the RT
    // denoisers follow the camera's cut (historyValid, Camera::HardCutTo), so
    // the caller pairs this with a hard cut on the active channel.
    // @p quiet suppresses the log line only: true from callers that can fire
    // every frame (a Sequencer scrub), false for one-off cuts, where the log
    // line proves the reset happened.
    void InvalidateTemporalHistory(bool quiet = false);
    // Raises a reason the main view's history breaks (HistoryReset bits,
    // RenderGraph/ViewState.h) from the game side, any thread: it travels with
    // the frame being built (FrameView::viewBreaks at the hand-off) and the
    // render half applies it at that frame's start. @p quiet: no log line.
    void RaiseViewBreak(uint32_t reasons, bool quiet = false);
    // The main view's owner: whose camera the main pipeline renders (in the editor, the editor camera's view
    // or the game's). A change raises HardCut before the new owner's first frame, so no view reads history
    // another one left: TAA, DLSS / FSR2, the exposure (it snaps), ReSTIR, SSR, the fog. @p owner 0 = none,
    // and the first owner adopts the view without a cut; @p name is a string literal (the log and
    // Render.views() keep the pointer). Game half; Game.exe never calls it.
    void        SetMainViewOwner(uint32_t owner, const char* name);
    const char* MainViewOwnerName() const { return m_mainViewOwnerName; }
    // Whether the main view shows the editor's overlays (the grid, debug wires and icons, the selection
    // outline): the editor turns them off while the main view is the Game View. Game half; Game.exe never
    // calls it (shown).
    void        SetMainViewEditorOverlays(bool shown) { m_mainEditorOverlays = shown; }
    // ECS -> DrawPackets + per-frame CB upload (rebuilds the projection on resize).
    void BeginFrame(World& world, FrameIndex frame, float dt, uint32_t vpW, uint32_t vpH,
                    Sys::FrameTaskContext* cpuFrame = nullptr);
    // Required after BeginFrame with a CPU context, before render packet release.
    // Owner UI/debug extraction may run in between; no new frame/slot/reset may.
    void FinishFramePreparation(Sys::FrameTaskContext& cpuFrame);

    void ApplyEntityDestroyRenderSide(Entity e, const MeshSkinnedComponent* skin, uint32_t beamSlot);
    // The destroy's writes to state the lane READS: the retained-scene untrack
    // and the DDGI skinned-BLAS record. Only with the lane quiescent - the
    // immediate path (after its drain) or the sync point.
    void ApplyEntityDestroySharedState(Entity e);
    void DrainPendingBindingReleases();

    // ---- Game-side view, computed before Renderer::BeginFrame ----
    // The UI paint needs this frame's un-jittered view-projection so world-space
    // anchors land on their geometry. BuildGameView computes the two matrices
    // the paint reads up front, right after SetCamera, from the camera and the
    // viewport size App already knows, so the paint does not depend on
    // UploadFrameData having written m_view. Only the jitter-free pair: the
    // jittered matrices depend on the render resolution, decided inside
    // BeginFrame.
    void BuildGameView(uint32_t vpW, uint32_t vpH);
    const RenderView& GetGameView() const { return m_gameView; }

    // Build the world-space UI vertex lane on the game side. Call it after the
    // UI tick (WorldSpaceUISystem::Tick).
    void ExtractWorldUI(World& world);

    // ---- Reflection-probe bake: extract / record / apply ----
    // The one bidirectional render-side World read, so it has a return path as
    // well as a lane. Both halves run on the game side in
    // BuildScene_SelectProbes (the top of BuildRenderScene): the apply first,
    // then the extract. Do not swap them: the select re-enqueues any probe not
    // yet marked baked, so applying after it would bake last frame's probe a
    // second time.
    void ExtractProbeBakeRequest(World& world);
    void ApplyProbeBakeFeedback(World& world);

    // ---- Material preview: the same extract / record / apply triple ----
    // Mirrors the probe lane above, plus an in-flight flag, a generation on the
    // return path, and no scene filtering on the render half. This is the
    // resident live view lifecycle, not a one-shot thumbnail: a resident view
    // has no dirty flag for the apply to clear, only a lastRenderedGeneration
    // that a matching result advances. Neither takes a World.
    void ApplyPreviewFeedback();     // GAME, top of BuildRenderScene
    void ExtractPreviewRequests();   // GAME, end of BuildRenderScene
    // RENDER, Render() phase 4.3. Records onto its own command list, which
    // depends on colorLastCL but is not spliced into that chain - see
    // TakePreviewCommandListThisFrame.
    void ProcessPreviewQueue(RHI::CommandList colorLastCL);
    // The preview command list Render() recorded this frame, or an invalid
    // handle when it recorded none. Consumed once, by FramePipeline::RenderHalfAsync,
    // which gives it its own AddCommandListDependency(primaryCL, previewCL) edge
    // next to the one for lastPassCL.
    // Do not make it the new colorLastCL: colorLastCL feeds Hi-Z, SSR, the DDGI
    // debug list and preComputeCL, so splicing the preview in would put all of
    // them behind it on the GPU and make the main viewport wait on the material
    // preview panel.
    RHI::CommandList TakePreviewCommandListThisFrame();

    // ---- Frame capture (RenderGraph/FrameCapture.h) ----
    // The render half's two steps around the device frame. ArmFrameCapture,
    // before BeginFrame: when a reader asked RG::FrameCaptures(), arm the
    // device's barrier journal for this frame and return true.
    // PublishFrameCapture, after EndFrame of that frame: join the device's frame
    // snapshot with the main graph's, publish the result, and log [RG_COVERAGE].
    // A frame nobody asked about costs one relaxed load.
    bool ArmFrameCapture();
    void PublishFrameCapture();
    // Owner-only CPU visibility prepass; must run after world activation/interpolation.
    void PrepareAnimationVisibility(World& world, FrameIndex frame);
    // -brssplit (default off): publish / join the static half of the next scene
    // build. Publish immediately before TickPhase::SecondaryPhysics (an empty
    // phase - chain physics runs in Animation), join immediately after it,
    // before TickPhase::PreRender. Both owner-only and no-ops with the flag off;
    // Join is idempotent. The m_brsEarly* member block holds the contract.
    void PrepareStaticSceneEarly(World& world);
    void JoinStaticSceneEarly();
    // The CPU animation chain - character state -> AnimationSystem sample -> IK
    // -> ChainPhysics -> LocalToWorld -> Socket -> Follow -> bone AABB merge ->
    // SkinMatrix -> BuildSkinJobs - which writes the PoseRingBuffer / VertexRing
    // the SkinningPass consumes in Render(). Outside BeginFrame so the scheduler
    // can run it in the Animation phase; it must run before BeginFrame each
    // frame so SkinningOutputComponent offsets match the slot Render() reads.
    // No-op without the Skinning subsystem.
    void TickAnimationChain(World& world, FrameIndex frame, float dt);

    // -animoverlapverify: the skin-job double buffer's own invariant, surfaced
    // so FramePipeline's oracle (VerifyAnimOverlap) can pin it without reaching
    // into the subsystem. See SkinnedMeshSubsystem::SkinJobHalvesDistinct.
    bool SkinJobHalvesDistinct() const { return m_skin.SkinJobHalvesDistinct(); }
    // Execute the deferred rendering graph; each pass allocates its own CL.
    RHI::CommandList Render();
    void SetRecordingReadyCallback(std::function<void()> callback) { m_graph.SetRecordingReadyCallback(std::move(callback)); }
    // @p viewSlot 0: the frame - the scene's work and the main view. 1..: a secondary view
    // (Renderer_Views.cpp), the same per-view work again through its camera into its own picture,
    // after the main view's call (RenderViewsAsync).
    Sys::OwnerTask RenderAsync(Sys::OwnerTaskQueue& owner,
        std::optional<Sys::ExecutionDomain::Token> token, RHI::CommandList& result, uint32_t viewSlot = 0);
    // The render half after RenderAsync: every acquired secondary view whose camera is set this
    // frame, one after another, each ordered behind @p after (the main view's last list); @p last
    // is the last view's last list (invalid when none rendered) - FramePipeline orders the primary
    // list (ImGui samples the pictures) behind it.
    Sys::OwnerTask RenderViewsAsync(Sys::OwnerTaskQueue& owner,
        std::optional<Sys::ExecutionDomain::Token> token, RHI::CommandList after, RHI::CommandList& last);
    // Hot-reload shaders on every pass. Caller must guarantee GPU is idle.
    void ReloadShaders();

    // Read the shadow-atlas demand off the World on the game side, immediately
    // before ServiceEnableGatedResources, so that call (which may hard-sync)
    // touches no World. It carries no ThreadCtx::Exempt only because it runs
    // before the Render scope opens: moving the call inside that scope needs one.
    void ExtractShadowAtlasDemand(World& world);
    // The prepare hooks' order table (PassSet, one place: the end of Compile)
    // and the context every stage starts from.
    void RegisterPrepareOrder();
    // The service hooks' order table (one place: the end of Compile).
    void RegisterServiceOrder();
    RG::PrepareContext PrepareContextNow() const;
    // The enable-gated pass working sets (RTAO / RTShadow + NRD pools, fog
    // froxels), for the current display viewport at the parked frame boundary,
    // before BeginFrame - enable / disable paths may hard-sync. App::RenderPhase
    // (the RenderSystem lambda) owns the call.
    void ServiceEnableGatedResources(uint32_t viewportW, uint32_t viewportH);
    // Copies the shells' backend status for GetRestirBackendStatus and flags
    // an effective-core switch for the budget ladder (render half).
    void PublishRestirStatus();

    // ---- System injection ----
    // Also creates the MeshAssetRegistry over it (StaticMeshComponent resolution).
    void SetMeshLibrary(Resource::MeshLibrary* meshLib);
    void SetTextureSystem(Resource::TextureSystem* ts)       { m_texSys  = ts; }
    void SetResourceManager(Resource::ResourceManager* rm)   { m_resMgr  = rm; }
    Resource::MeshLibrary* GetMeshLibrary() const            { return m_meshLib; }
    // (model, MeshKey) -> library + sections (Scene/MeshAssetRegistry.h); null
    // until SetMeshLibrary. Its libraries are this renderer's WORLD libraries.
    MeshAssetRegistry*     GetMeshAssets() const             { return m_meshAssets.get(); }
    MeshManager&           GetMeshManager()                  { return m_meshMgr; }
    const MeshManager&     GetMeshManager() const            { return m_meshMgr; }

    // Authored MaterialComponent stores texture paths only. These queries
    // expose the shared renderer-owned GPU view to editor and RT systems.
    MaterialRuntimeTextureView GetMaterialTextureRuntime(
        Entity entity, uint32_t slot) const override;
    MaterialRuntimeTextureView GetMaterialCustomTextureRuntime(
        Entity entity, std::string_view bindingName) const override;
    int GetMaterialRuntimeCustomPSID(Entity entity) const;
    int GetMaterialRuntimeCustomVSID(Entity entity) const;
    // Same two ids, keyed by the material rather than an entity.
    // ResolveCustomPSID / VSID ignore their Entity argument and key
    // m_runtimeMaterialShaders by a hash of (useCustomShader, customShaderPath,
    // customVertexShaderPath), so the id belongs to the material. A caller with
    // no entity (the asset-mode material inspector, the material preview) asks
    // here instead of borrowing a live entity that uses the same shader.
    // Read-only: find(), never operator[] (which would insert), and it never
    // registers; registration is RegisterMaterialShader on the game half.
    // Same three states as the accessors above: -1 = no material has resolved
    // this shader key in this process (nothing registered, no reflection), 0 =
    // resolved to no shader for this stage (a pixel-only material's VS), > 0 =
    // the dynamic shader id.
    int LookupMaterialCustomPSID(const MaterialComponent& material) const;
    int LookupMaterialCustomVSID(const MaterialComponent& material) const;
    // A runtime spawn's census (Resource/PrefabSpawn.h): of @p materials, how many
    // distinct custom-shader keys no material has resolved in this process yet
    // (each one's first gather pays the dynamic registration, the .ishdr load,
    // the DXC reflection and the //@ annotation parse), and how many distinct
    // texture paths are not GPU-resident yet. Read-only like the lookups above
    // (find, never operator[]; no Acquire, no registration). Game half only.
    void CountColdMaterialInputs(const std::vector<const MaterialComponent*>& materials,
                                 uint32_t& coldShaders, uint32_t& coldTextures) const;
    // A runtime spawn's new entities. Their custom shaders are resolved (the
    // dynamic registration and the reflection sync) at the top of the next
    // BuildRenderScene, before SyncMaterials binds them, so the bind takes the
    // synced layout and the next frame sees no material dirt. Not at the spawn
    // itself: a Lua spawn runs in the render-owner overlap window, where the
    // lane's record workers may be compiling against the registry a registration
    // appends to. Main thread; OnWorldClear drops the queue.
    void QueueSpawnMaterialResolve(const std::vector<Entity>& entities);
    void SetMaterialTextureRuntimeOverride(
        Entity entity, uint32_t slot, const MaterialRuntimeTextureView* view);
    // Render.explain (Renderer/RenderExplain.h): the runtime-material and
    // draw-list half of why an entity looks the way it does - what each slot
    // resolved to, the custom shaders' compile state over all variants, the draw
    // lists of the last built frame holding its instances. Read-only. The
    // frame's render work must be drained: it reads what the scene build writes.
    void ExplainEntity(Entity entity, struct EntityRenderReport& out) const;

    // Render.explain(id).meshlet's path half: which geometry path the entity's
    // last emitted packet took and, when it is not a meshlet one, why. Same
    // drained-render-work rule as ExplainEntity.
    void ExplainMeshletPath(Entity entity, MeshletPathReport& out) const;

    // Render.materialCosts and the materialCost debug target: per material slot
    // drawn by the main view's opaque, water and forward-translucent packets, the
    // pixel shader that draws it (GBuffer.ps / WaterSurface.ps / Transparent.ps in
    // the packet's permutation, or the material's custom one) and its reflected
    // instruction count (ShaderLibrary::Report). A slot two shaders draw (two
    // permutations of one material) is two rows; sorted by slot. @p details adds
    // the files and one entity (main thread, render work drained, as
    // ExplainEntity); the render half builds the frame's table without them.
    void CollectMaterialCosts(std::vector<MaterialCostRow>& out, bool details) const;
    // The visible pixels per material slot that the last materialCost frame
    // counted, and that frame's number (~0: none read back yet).
    std::uint64_t DebugMaterialCoverage(std::vector<std::uint32_t>& pixels) const;
    // The meshlet path's engine settings (engine.json "culling"). A command-line
    // flag (-meshlets / -meshletlod) pins its value for the run - a pack must not
    // depend on engine.json - and these setters then leave it alone.
    void  SetMeshShadingEnabled(bool v)       { if (!m_meshletsPinned) m_meshletsRequested = v; }
    bool  IsMeshShadingEnabled() const        { return m_meshletsRequested; }
    void  SetMeshletLodError(float px)        { if (!m_meshletLodPinned) m_meshletLodTau = px < 0.0f ? 0.0f : px; }
    float GetMeshletLodError() const          { return m_meshletLodTau; }
    void  SetMeshletShadowLodError(float t)   { if (!m_meshletShadowLodPinned) m_meshletShadowLodTau = t < 0.0f ? 0.0f : t; }
    float GetMeshletShadowLodError() const    { return m_meshletShadowLodTau; }
    // The player's multiplier on both taus (settings.json geometry.lodScale): the
    // emit hands the cull tau x scale. Not pinned by -meshletlod (a pin is of
    // tau; the scale defaults to 1).
    void  SetLodScale(float s)
    {
        if (!(s == s)) s = 1.0f;
        m_meshletLodScale = s < 0.25f ? 0.25f : (s > 4.0f ? 4.0f : s);
    }
    float GetLodScale() const                 { return m_meshletLodScale; }
    // XeGTAO's slices x steps (settings.json features.aoQuality): 0 Low,
    // 1 Medium, 2 Scene (the scene's counts), 3 Ultra - handed to XeGTAOPass
    // every frame.
    void SetAOQuality(int q) { m_aoQuality = (q >= 0 && q <= 3) ? q : 2; }
    int  GetAOQuality() const { return m_aoQuality; }

    // Render.explain(id).meshlet.lod: the level the LOD rule picks for the
    // entity's instance now, from the library's per-level records.
    void ExplainMeshletLod(Entity entity, MeshletLodReport& out) const;
    // The Lua `Asset` table (Renderer/AssetQuery.h): the entities whose runtime
    // material samples @p path in some slot - the renderer's resolved view
    // (defaults filled, per-entity overrides applied), which is what a picture
    // shows. Same drain rule as ExplainEntity.
    void TextureConsumers(const std::string& path, std::vector<Entity>& out) const;
    // The same answer for EVERY texture in one walk: TextureUserKey(path) ->
    // how many entities sample it. An audit of N textures (Loaded Textures,
    // Asset.textures{users=true}) must not walk the world N times, and a scene
    // of thousands of entities must not fold a string per slot per entity: a
    // material resource's paths are hashed once however many entities share
    // it. Same drain rule.
    void TextureConsumerCounts(std::unordered_map<uint64_t, uint32_t>& out) const;
    // The key TextureConsumerCounts files a path under: FNV-1a over the path
    // with separators and letter case folded (never 0 for a non-empty path in
    // practice; 0 means "no texture").
    static uint64_t TextureUserKey(const std::string& path);

    // Track a MeshLibrary handle loaded for the current world. OnWorldClear
    // releases every tracked handle so per-reload VB / IB allocations do not
    // accumulate (MeshLibrary::Load does not dedupe by path: the same .meshlib
    // loaded N times gives N GPU buffer pairs). Called by SceneInstanceLoader
    // after each successful MeshLibrary::Load.
    void TrackWorldMeshLibrary(Resource::Handle h)
    {
        if (h.IsValid()) m_worldMeshLibs.push_back(h);
    }

    // Every loaded .meshlib and who holds it (the Loaded Geometry window, Lua
    // Asset.meshLibraries()). `worldRefs` counts TrackWorldMeshLibrary calls for
    // the current world, `preview` the asset preview lane's reference,
    // `editorPinned` Play's snapshot pin, `pendingRelease` the previous world's
    // one-world-lag release; `users` the entities of @p world whose MeshLibRef
    // names it (0 with no world). Sorted by path. Game half (or the serial
    // editor).
    std::vector<LoadedMeshLibInfo> DescribeLoadedMeshLibraries(World* world) const;

    // ---- Camera / view / draw list ----
    // Game half (App resolves the VCam stack there).
    void SetCamera(const RenderCamera& cam) { FrameViewWrite().camera = cam; }
    // ---- Secondary views (Renderer_Views.cpp) ----
    // Game half, like SetCamera: view @p slot (1 .. RHI::kMaxViews - 1) looks through @p cam at
    // @p width x @p height from this frame on, showing @p show (RG::ShowFlag bits), until
    // ClearSecondaryView.
    void SetSecondaryView(uint32_t slot, const RenderCamera& cam, uint32_t width, uint32_t height, uint32_t show);
    void ClearSecondaryView(uint32_t slot);
    // A secondary view: the main pipeline renders the scene again through its camera into its own
    // picture, every frame SetSecondaryView names it. Main thread. AcquireView answers its slot (1 .. kMaxViews - 1;
    // 0 = none free, or no room in the video memory budget for a @p width x @p height view: ViewBudget); its
    // resources are made at the next frame boundary (ServiceViews), given back at the one after ReleaseView.
    uint32_t AcquireView(const char* name, uint32_t width = 0, uint32_t height = 0);
    void     ReleaseView(uint32_t slot);
    // The camera view @p slot follows from now on, resolved on the game half every frame before the
    // scene's cull (SetSecondaryView): @p camera's GlobalTransform and lens or, with @p followMain, the
    // main camera; at @p width x @p height, showing @p show (RG::ShowFlag bits). Main thread.
    void     SetViewCamera(uint32_t slot, Entity camera, bool followMain, uint32_t width, uint32_t height,
                           uint32_t show);
    // A secondary view's id where Render.views() and a capture {view, lease} name it: this plus its
    // slot (the preview views' ids are below it).
    static constexpr uint32_t kSecondaryViewIdBase = 32;
    // The frame boundary (App's ProcessPendingActions), the render half parked: makes the acquired
    // views' resources and gives back the released ones'.
    void     ServiceViews();
    // What a secondary view is now: live = acquired; lease = its acquisition (bumped on each);
    // renders = pictures so far; width / height = its last picture's.
    struct ViewInfo
    {
        bool        live    = false;
        uint32_t    lease   = 0;
        uint64_t    renders = 0;
        uint32_t    width   = 0;
        uint32_t    height  = 0;
        const char* name    = "";
        uint64_t    vramBytes        = 0;   // what its setup and segments allocated (GpuLedger::ViewBytes)
        uint64_t    graphBytes       = 0;   // its own committed graph textures (part of vramBytes)
        uint64_t    graphPlacedBytes = 0;   // its graph textures in the views' alias heaps (shared)
    };
    ViewInfo GetViewInfo(uint32_t slot) const;
    // The views' alias heaps (RenderGraph::ViewAliasHeapBytes): the bytes every view's placed graph textures
    // share.
    uint64_t GetViewAliasHeapBytes() const { return m_graph.ViewAliasHeapBytes(); }
    // A secondary view as this frame draws it (the game half, once BeginFrame resolved its camera):
    // its camera's un-jittered view-projection and view matrices (as its segment builds them), its
    // size and what it shows (RG::ShowFlag bits). False when view @p slot renders nothing this frame.
    struct ViewFrameCamera
    {
        DirectX::XMFLOAT4X4 viewProjNoJitter{};
        DirectX::XMFLOAT4X4 view{};
        uint32_t            width  = 0;
        uint32_t            height = 0;
        uint32_t            show   = 0;
    };
    bool GetViewFrameCamera(uint32_t slot, ViewFrameCamera& out) const;
    // A secondary view's picture (its composition buffer, tone mapped): an ImGui image or a capture's
    // source; 0 / null before its first render. Any thread.
    uint64_t            GetViewOutputSrv(uint32_t slot) const;
    const RHI::Texture* GetViewOutputTexture(uint32_t slot) const;
    // No callers. Kept as API, but see m_clearColor: a caller on the game half
    // would be a cross-seam write into a value the render half reads without
    // synchronisation.
    void SetClearColor(const float clearColor[4]) { std::memcpy(m_clearColor, clearColor, sizeof(m_clearColor)); }
    // Game half: the editor overlay and App read it above the seam.
    const RenderView& GetView() const       { return FrameViewWrite().view; }
    DrawList GetDrawList(DrawFilter f) const;

    // ---- Picking ----
    // Store a pending pick request; PickingPass executes the GPU copy in
    // Render(). @p forHarness marks it the harness's (the pipe's `pick` verb):
    // the editor's resolve sites leave such a pick alone (IsHarnessPick) and
    // HarnessCaptureSystem resolves it - otherwise the editor would take the
    // answer for a click and change its selection. @p viewSlot: the view it is
    // a pixel of (0 = the main view, in display pixels; a secondary view's slot,
    // in its pixels): that view's segment draws it. False for a view that is not
    // live or has not rendered yet.
    bool RequestPick(float pixelX, float pixelY, bool forHarness = false, uint32_t viewSlot = 0);
    // After IGraphicsDevice::FlushAndWait(), read the pick result.
    bool ResolvePick(Entity& outEntity);
    // The one pick slot: asked or awaiting its readback / the readback is there.
    bool IsPickBusy() const;
    bool IsPickReadable() const;
    bool IsHarnessPick() const { return m_pickForHarness; }
    // Give up a harness pick still in the slot, so neither the editor (which
    // skips harness picks) nor the next harness pick waits on it for ever.
    void CancelHarnessPick();

    // The object-ID image (the pipe's `pick {"all": true}`): the picking pass
    // draws the whole view on the next frame and copies it back - its own
    // buffer, not the pick's slot. ResolvePickImage (FlushAndWait) hands back
    // one (instanceSlot + 1) per pixel of the internal render size (0 = no
    // instance there), PickSlotEntity names a slot's entity - both against this
    // frame's slot table, so resolve in the frame the image became readable
    // (the harness does it at PostRender). @p viewSlot as RequestPick's.
    bool   RequestPickImage(uint32_t viewSlot = 0);
    bool   IsPickImageBusy() const;        // asked or awaiting its read
    bool   IsPickImageReadable() const;
    bool   ResolvePickImage(std::vector<std::uint32_t>& outSlots, std::uint32_t& outW, std::uint32_t& outH);
    Entity PickSlotEntity(std::uint32_t slotPlusOne) const;
    void   CancelPickImage();

    // Editor-only highlight: force an OutlinePass Custom-filter draw for this
    // entity even if its MaterialComponent::OUTLINE flag is off. NullEntity
    // disables the highlight. App pushes EditorLayer's selection here before
    // each BeginFrame. Does NOT mutate MaterialComponent.
    void   SetPickingOutlineEntity(Entity e) { m_pickingOutlineEntity = e; }
    Entity GetPickingOutlineEntity() const   { return m_pickingOutlineEntity; }
    // Whether the selection outline draws around @p e: the outlined entity
    // itself, or - when that is a StaticMesh owner, which draws nothing of its
    // own - one of its derived sections (refreshed at each BuildRenderScene).
    bool   IsPickingOutlined(Entity e) const
    {
        if (m_pickingOutlineEntity == NullEntity) return false;
        if (e == m_pickingOutlineEntity) return true;
        for (Entity s : m_pickingOutlineSections)
            if (s == e) return true;
        return false;
    }

    // ---- Pass accessors - geometry / shadows / lighting ----
    OutlinePass*             GetOutlinePass()        { return m_outlinePass; }
    OutlineGeometryPass*     GetOutlineGeometryPass(){ return m_outlineGeometryPass; }
    DebugWirePass*           GetDebugWirePass()      { return m_debugWirePass.get(); }
    SkyIBLPass*              GetSkyIBLPass()         { return m_skyIBLPass; }
    VolumetricFogPass*       GetVolumetricFogPass()  { return m_volFogPass; }
    CloudPass*               GetCloudPass()          { return m_cloudPass; }
    class EditorGridPass*    GetEditorGridPass()     { return m_editorGridPass; }
    DecalPass*               GetDecalPass()          { return m_decalPass; }

    // ---- AA mode ----
    // Effective mode - what the renderer is actually doing. Everything in the
    // frame path uses this.
    AAMode GetAAMode() const;

    // Requested mode - what the user last asked for, even if the hardware cannot
    // honour it. Persistence must save this: saving the effective mode demotes a
    // settings.json carried to a machine without DLSS (to FSR at the same ratio,
    // or TAA at Native) and loses the preference. Same requested-vs-effective
    // split as HDROutput's m_hdrModeRequested.
    AAMode GetAAModeRequested() const { return m_aaModeRequested; }

    void   SetAAMode(AAMode m);

    // ---- Render path, path tracing and ReSTIR settings ----
    // Either path-tracing path: the PT pass composites the frame.
    static constexpr bool IsPathTrace(RenderPath p) { return p == RenderPath::PathTrace || p == RenderPath::PathTraceRealtime; }
    static const char* RenderPathName(RenderPath path);
    static bool ParseRenderPath(const std::string& token, RenderPath& path);
    void SetRenderPath(RenderPath path);
    RenderPath GetRenderPath() const {
        const auto path=m_renderPath.load(std::memory_order_relaxed);
        return m_restirBudgetStage.load(std::memory_order_relaxed)==4 && path>=RenderPath::ReSTIR_DI && path<=RenderPath::ReSTIR_DI_GI ? RenderPath::Raster : path;
    }
    RenderPath GetRenderPathRequested() const { return m_renderPathRequested.load(std::memory_order_relaxed); }
    uint32_t GetPathTraceSamples() const;
    uint32_t GetPathTraceResets() const;
    void SetPTMaxBounces(uint32_t b) { m_ptMaxBounces.store(std::clamp(b,1u,64u)); }
    uint32_t GetPTMaxBounces() const { return m_ptMaxBounces.load(); }
    void SetPTReferenceArm(bool enabled) { m_ptReferenceArm.store(enabled); }
    bool GetPTReferenceArm() const { return m_ptReferenceArm.load(); }
    // The real-time PT preset (PathTracePass::SetRealtimePreset) is the
    // PathTraceRealtime render path: true while it is the requested path
    // (-pt_realtime=0|1 pins the PT pass's preset for one run).
    bool GetPTRealtime() const { return GetRenderPathRequested() == RenderPath::PathTraceRealtime; }
    // Where the RTXDI PT core takes x1's direct light (PTRtxdi::DirectLight):
    // auto | nee | restir_di | unified. engine.json raytracing.ptDirectLight; a
    // change remakes the core at the next frame.
    void SetPTDirectLight(PTRtxdi::DirectLight d) { m_ptDirectLight.store(static_cast<uint8_t>(d), std::memory_order_relaxed); }
    PTRtxdi::DirectLight GetPTDirectLight() const { return static_cast<PTRtxdi::DirectLight>(m_ptDirectLight.load(std::memory_order_relaxed)); }
    void SetPTCameraPrimary(bool enabled) { m_ptCameraPrimary.store(enabled); }
    bool GetPTCameraPrimary() const { return m_ptCameraPrimary.load(); }
    void SetPTAreaMode(uint32_t mode) { m_ptAreaMode.store(std::min(mode,2u)); }
    uint32_t GetPTAreaMode() const { return m_ptAreaMode.load(); }
    void SetPTAreaMotion(bool enabled) { m_ptAreaMotion.store(enabled); }
    bool GetPTAreaMotion() const { return m_ptAreaMotion.load(); }
    void SetPTAreaHistory(uint32_t cap) { m_ptAreaHistory.store(std::clamp(cap,1u,64u)); }
    uint32_t GetPTAreaHistory() const { return m_ptAreaHistory.load(); }
    void SetPTAreaSpatialK(uint32_t k) { m_ptAreaSpatialK.store(std::clamp(k,1u,4u)); }
    uint32_t GetPTAreaSpatialK() const { return m_ptAreaSpatialK.load(); }
    void SetPTAreaSpatialRadius(float px) { m_ptAreaSpatialRadius.store(std::isfinite(px)?std::clamp(px,1.f,32.f):6.f); }
    float GetPTAreaSpatialRadius() const { return m_ptAreaSpatialRadius.load(); }
    void SetPTAreaReconnect(bool enabled) { m_ptAreaReconnect.store(enabled); }
    bool GetPTAreaReconnect() const { return m_ptAreaReconnect.load(); }
    void SetPTFilmJitter(bool enabled) { m_ptFilmJitter.store(enabled); }
    bool GetPTFilmJitter() const { return m_ptFilmJitter.load(); }
    void SetPTPrimaryRayCone(bool enabled) { m_ptPrimaryRayCone.store(enabled); }
    bool GetPTPrimaryRayCone() const { return m_ptPrimaryRayCone.load(); }
    void SetPTLensRadius(float value) { m_ptLensRadius.store(std::isfinite(value)?std::clamp(value,0.f,1.f):0.f); }
    float GetPTLensRadius() const { return m_ptLensRadius.load(); }
    void SetPTFocusDistance(float value) { m_ptFocusDistance.store(std::isfinite(value)?std::clamp(value,.01f,10000.f):5.f); }
    float GetPTFocusDistance() const { return m_ptFocusDistance.load(); }
    // The reference path tracer's preview denoiser (PathTracePass::RecordDenoise): on while the
    // image holds fewer than fadeSpp samples, faded to the plain running mean from fadeSpp / 2.
    void SetPTDenoise(bool enabled) { m_ptDenoise.store(enabled); }
    bool GetPTDenoise() const { return m_ptDenoise.load(); }
    void SetPTDenoiseFadeSpp(uint32_t spp) { m_ptDenoiseFadeSpp.store(std::min(spp, 4096u)); }
    uint32_t GetPTDenoiseFadeSpp() const { return m_ptDenoiseFadeSpp.load(); }

    // ---- The path tracer's time freeze ----
    // The setting (raytracing.ptFreezeTime); it holds while the render path is Path Trace
    // (Reference): the simulation and the world clocks stop so the progressive image converges.
    void SetPTFreezeTime(bool enabled) { m_ptFreezeTime.store(enabled); }
    bool GetPTFreezeTime() const { return m_ptFreezeTime.load(); }
    // Once a frame on the main thread, before the simulation: the frame's world dt - @p dt, 0
    // while the freeze holds, or once the seconds of a pending RequestWorldStep. BeginFrame hands
    // it to the world clocks (FrameView::worldDeltaTime); a host that never calls it runs unfrozen.
    float BeginWorldClock(float dt);
    bool IsWorldClockFrozen() const { return m_worldClockFrozen.load(std::memory_order_relaxed); }
    // Advance a frozen world by @p seconds on the next frame (Lua Render.ptStep): the simulation
    // and the world clocks take one step, the path tracer restarts. False while not frozen or for
    // a step outside (0, 1] seconds.
    bool RequestWorldStep(float seconds);
    uint32_t GetWorldStepsDone() const { return m_worldStepsDone.load(std::memory_order_relaxed); }
    bool IsWorldStepPending() const { return m_worldStepSeconds.load(std::memory_order_relaxed) > 0.f; }

    // The engine.json rows whose value a command-line pin set (the -pt_* pins are
    // those rows' command-line spellings). Some rules hold for a pin only, not
    // for the same value from engine.json or the editor: the PT auto backend (an
    // explicit bounce count or -pt_reference sends the real-time path to the
    // engine path tracer), the camera modes held off the reference path,
    // RRCameraRaysForced. Set by the rows' apply hooks
    // (Settings::DevApplyCtx::fromCommandLine).
    // ReSTIR DI: -di_visibility_confidence turns reuse on unless the reuse row is
    // a command-line "off"; command-line two passes default the radius to 8
    // unless the radius is from the command line too; the RTXDI core reports a
    // command-line age cap and two-pass request it ignores. ReSTIR GI: a
    // command-line spatial radius is not doubled at full resolution; a
    // command-line SHaRC "off" names itself as the reason.
    void SetDevPinned(DevPin p, bool pinned)
    {
        const uint32_t bit = 1u << static_cast<uint32_t>(p);
        if (pinned) m_devPinned.fetch_or(bit, std::memory_order_relaxed);
        else        m_devPinned.fetch_and(~bit, std::memory_order_relaxed);
    }
    bool IsDevPinned(DevPin p) const
    {
        return ((m_devPinned.load(std::memory_order_relaxed) >> static_cast<uint32_t>(p)) & 1u) != 0;
    }
    void SetDIRayTMax(float value) { m_diRayTMax.store(std::clamp(value==value ? value : 150.f,1.f,1000.f)); }
    float GetDIRayTMax() const { return m_diRayTMax.load(); }
    void SetRestirDISpatialPasses(uint32_t value) { m_restirDiSpatialPasses.store(std::clamp(value,0u,2u)); }
    uint32_t GetRestirDISpatialPasses() const { return m_restirDiSpatialPasses.load(); }
    void SetRestirDISpatialRadius(uint32_t value) { m_restirDiSpatialRadiusPx.store(std::clamp(value,0u,64u)); }
    uint32_t GetRestirDISpatialRadius() const { return m_restirDiSpatialRadiusPx.load(); }
    void SetRestirDIK(uint32_t value) { m_restirDiK.store(std::clamp(value,1u,8u)); }
    uint32_t GetRestirDIK() const { return m_restirDiK.load(); }
    void SetRestirDIMCap(uint32_t value) { m_restirDiMCap.store(std::clamp(value,1u,63u)); }
    uint32_t GetRestirDIMCap() const { return m_restirDiMCap.load(); }
    void SetRestirDIAgeCap(uint32_t value) { m_restirDiAgeCap.store(std::clamp(value,1u,63u)); }
    uint32_t GetRestirDIAgeCap() const { return m_restirDiAgeCap.load(); }
    void SetRestirGIAgeCap(uint32_t value) { m_restirGiAgeCap.store(std::clamp(value,1u,63u)); }
    uint32_t GetRestirGIAgeCap() const { return m_restirGiAgeCap.load(); }
    // Temporal+spatial resampling for the ReSTIR paths (engine.json). The
    // passes rebuild their reservoir buffers at the service point on change.
    void SetRestirDIReuse(bool v) { m_restirDiReuse.store(v); }
    bool GetRestirDIReuse() const { return m_restirDiReuse.load(); }
    void SetRestirGIReuse(bool v) { m_restirGiReuse.store(v); }
    bool GetRestirGIReuse() const { return m_restirGiReuse.load(); }
    void SetRestirGISpatialRadius(uint32_t value) { m_restirGiSpatialRadiusPx.store(std::min(value,32u)); }
    uint32_t GetRestirGISpatialRadius() const { return m_restirGiSpatialRadiusPx.load(); }
    void SetRestirGIMaxDistance(float d) { m_restirGIMaxDistance.store(std::clamp(d==d ? d : 150.f,1.f,1000.f)); }
    float GetRestirBudgetMs() const { return m_restirBudgetMs.load(); }
    void SetRestirBudgetMs(float value) { m_restirBudgetMs.store(value>=0 && value<=1000 ? value : 0.f); }
    uint32_t GetRestirBudgetStage() const { return m_restirBudgetStage.load(); }
    float GetRestirGIMaxDistance() const { return m_restirGIMaxDistance.load(); }
    void SetRestirGITier(uint32_t t) { m_restirGITier.store(std::min(t,1u)); }
    uint32_t GetRestirGITier() const { return m_restirGITier.load(); }
    // Cost knobs; the -di_* / -gi_* flags of the same meaning pin them inside the
    // passes.
    void SetRestirDICandidates(uint32_t v) { m_restirDiCandidates.store(std::clamp(v,1u,32u)); }
    uint32_t GetRestirDICandidates() const { return m_restirDiCandidates.load(); }
    void SetRestirDIFullRes(bool v) { m_restirDiFullRes.store(v); }
    bool GetRestirDIFullRes() const { return m_restirDiFullRes.load(); }
    // RTDIPass::Settings::specularAntiFirefly: steady DI highlights vs the energy
    // of small bright ones.
    void SetRestirDISpecAntiFirefly(bool v) { m_restirDiSpecAntiFirefly.store(v); }
    bool GetRestirDISpecAntiFirefly() const { return m_restirDiSpecAntiFirefly.load(); }
    void SetRestirGISpatial(bool v) { m_restirGiSpatial.store(v); }
    bool GetRestirGISpatial() const { return m_restirGiSpatial.load(); }
    void SetRestirGIInitialCandidates(uint32_t v) { m_restirGiInitialCandidates.store(std::clamp(v,1u,16u)); }
    uint32_t GetRestirGIInitialCandidates() const { return m_restirGiInitialCandidates.load(); }
    void SetRestirGIEmitterCandidates(uint32_t v) { m_restirGiEmitterCandidates.store(std::clamp(v,1u,64u)); }
    uint32_t GetRestirGIEmitterCandidates() const { return m_restirGiEmitterCandidates.load(); }
    void SetRestirGIFullRes(bool v) { m_restirGiFullRes.store(v); }
    bool GetRestirGIFullRes() const { return m_restirGiFullRes.load(); }
    void SetRestirGIHitExactLights(uint32_t v) { m_restirGiHitExactLights.store(std::clamp(v,1u,64u)); }
    uint32_t GetRestirGIHitExactLights() const { return m_restirGiHitExactLights.load(); }
    // SHaRC on ReSTIR GI (engine.json restirGiSharc / restirGiSharcMode;
    // -gi_sharc / -gi_sharc_mode pin them in the pass). Mode 0 parity (the cache
    // holds the Tier 1 shading), 1 multi.
    void SetRestirGISharc(bool v) { m_restirGiSharc.store(v); }
    bool GetRestirGISharc() const { return m_restirGiSharc.load(); }
    void SetRestirGISharcMode(uint32_t m) { m_restirGiSharcMode.store(std::min(m,1u)); }
    uint32_t GetRestirGISharcMode() const { return m_restirGiSharcMode.load(); }
    // The DI / GI denoiser (engine.json restirDiDenoiser / restirGiDenoiser).
    // Under DLSS Ray Reconstruction it is the denoiser before RR; the -di_* /
    // -gi_* NRD flags pin it in the pass.
    void SetRestirDIDenoiser(ReSTIR::Denoiser d) { m_restirDiDenoiser.store(static_cast<uint8_t>(d < ReSTIR::Denoiser::Count ? d : ReSTIR::Denoiser::Auto)); }
    ReSTIR::Denoiser GetRestirDIDenoiser() const { return static_cast<ReSTIR::Denoiser>(m_restirDiDenoiser.load()); }
    void SetRestirGIDenoiser(ReSTIR::Denoiser d) { m_restirGiDenoiser.store(static_cast<uint8_t>(d < ReSTIR::Denoiser::Count ? d : ReSTIR::Denoiser::Auto)); }
    ReSTIR::Denoiser GetRestirGIDenoiser() const { return static_cast<ReSTIR::Denoiser>(m_restirGiDenoiser.load()); }
    // What the shells run after the flags and ray reconstruction ("relax" /
    // "reblur" / "none"; a static string), for the editor.
    const char* GetRestirDIEffectiveDenoiser() const;
    const char* GetRestirGIEffectiveDenoiser() const;

    // What the GI shell last reported about its cache (the service point).
    GISharcStatus GetRestirGISharcStatus() const;

    // ReSTIR backends: the requested core per stage (engine.json, the CLI, Lua,
    // the editor). The shells apply it at the service point and report what
    // really runs; an illegal pair is ignored.
    void SetRestirBackend(ReSTIR::Stage stage, ReSTIR::Backend backend);
    ReSTIR::Backend GetRestirBackend(ReSTIR::Stage stage) const;
    // The status each shell last published (ServiceEnableGatedResources).
    ReSTIR::BackendStatus GetRestirBackendStatus(ReSTIR::Stage stage) const;
    // Whether this build and device can run @p backend for @p stage: the default
    // cores always; an RTXDI-based one when ReSTIR::RtxdiInertReason has no
    // objection (the SDK and the core in this build, native 16-bit ops for PT). A
    // request for an unavailable one is served by the default core, with that
    // reason (UnavailableRestirBackendReason).
    bool IsRestirBackendAvailable(ReSTIR::Stage stage, ReSTIR::Backend backend) const
    { return ReSTIR::IsLegal(stage, backend) && !UnavailableRestirBackendReason(stage, backend); }
    const char* UnavailableRestirBackendReason(ReSTIR::Stage stage, ReSTIR::Backend backend) const
    {
        if (!ReSTIR::IsLegal(stage, backend)) return "not a backend of this stage";
        // The engine-owned core, and PT's auto (it resolves to what can run).
        if (backend == ReSTIR::DefaultBackend(stage) || backend == ReSTIR::Backend::Auto) return nullptr;
        return ReSTIR::RtxdiInertReason(stage, m_gfx.GetCapabilities().native16BitShaderOps);
    }
    void SetRestirCompare(ReSTIR::Compare compare)
    { m_restirCompare.store(static_cast<uint8_t>(compare < ReSTIR::Compare::Count ? compare : ReSTIR::Compare::Off)); }
    ReSTIR::Compare GetRestirCompare() const { return static_cast<ReSTIR::Compare>(m_restirCompare.load()); }
    // Compare mode runs the other core in a second shell instance: DI and GI,
    // each needing its RTXDI core.
    ReSTIR::Compare GetRestirCompareEffective() const
    {
        const auto c = GetRestirCompare();
        if (c == ReSTIR::Compare::DI && IsRestirBackendAvailable(ReSTIR::Stage::DI, ReSTIR::Backend::Rtxdi)) return c;
        if (c == ReSTIR::Compare::GI && IsRestirBackendAvailable(ReSTIR::Stage::GI, ReSTIR::Backend::Rtxdi)) return c;
        return ReSTIR::Compare::Off;
    }
    const char* GetRestirCompareReason() const
    {
        const auto c = GetRestirCompare();
        if (c == ReSTIR::Compare::Off) return "selected";
        const auto stage = c == ReSTIR::Compare::DI ? ReSTIR::Stage::DI : ReSTIR::Stage::GI;
        const char* why = UnavailableRestirBackendReason(stage, ReSTIR::Backend::Rtxdi);
        return why ? why : "selected";
    }

    // ---- Upscaler render scale (FSR / DLSS) ----
    // An upscaler is turned on by selecting it as the AA mode (AAMode::FSR,
    // DLSS or DLSS_RR; see IsUpscalerActive()). This enum picks its
    // render-scale ratio:
    //   Native -> 1:1 (native AA / DLAA-style, no upscaling)
    //   Quality / Balanced / Performance / UltraPerformance -> upscale.
    // With no upscaling AA mode active the effective ratio is Native, so the HDR
    // target stays at display res.
    UpscaleMode GetUpscaleMode() const { return m_upscaleMode; }
    // Also re-decides a demoted DLSS request's stand-in (FSR at this ratio, TAA
    // at Native - SetAAMode).
    void        SetUpscaleMode(UpscaleMode m);
    // The ratio that actually takes effect: the selected scale only when an
    // upscaling AA mode is active, else Native (1.0x -> render == display).
    UpscaleMode GetEffectiveUpscaleMode() const
    {
        return IsUpscalerActive() ? m_upscaleMode : UpscaleMode::Native;
    }
    float       GetUpscaleRatio() const { return ::GetUpscaleRatio(GetEffectiveUpscaleMode()); }

    // Display = output res (user-visible viewport / swap chain).
    uint32_t GetDisplayWidth()  const { return FrameViewWrite().vpWidth;  }
    uint32_t GetDisplayHeight() const { return FrameViewWrite().vpHeight; }
    // Render = the actual internal HDR/GBuffer size selected by App. It may
    // combine the upscaler's quality ratio with display.renderScale; UI never
    // uses it.
    uint32_t GetRenderWidth()   const { return FrameViewWrite().renderWidth;  }
    uint32_t GetRenderHeight()  const { return FrameViewWrite().renderHeight; }

    // Apply the active upscaler's quality ratio. App may apply
    // display.renderScale after this before sizing the HDR target.
    // DLSS does not use the ratio table: it validates the render extent against
    // its own per-mode table and rejects a mismatch, so slDLSSGetOptimalSettings
    // is the authority. The table is the fallback when that query fails (no SDK /
    // unsupported GPU), which keeps this function total.
    void ComputeRenderResolution(uint32_t displayW, uint32_t displayH,
                                 uint32_t& outRenderW, uint32_t& outRenderH) const;

    // True iff FSR2Pass should run this frame - FSR is the selected AA mode and
    // the path is not path tracing. FSR does its own temporal AA (and upscales
    // when the render scale is not Native), so it replaces the engine TAA / FXAA
    // rather than composing.
    bool IsFSRActive() const { return m_aaMode == AAMode::FSR && !IsPathTrace(GetRenderPath()); }
    // The post chain follows the actual resolve output, including context
    // failures and the raw RR diagnostic path, at preparation AND execution.
    bool IsFSRResolved() const;
    bool IsDLSSResolved() const;

    // True iff DLSSPass should run this frame. Mode selection alone - whether the
    // feature actually came up is IsDLSSResolved().
    bool IsDLSSActive() const { return (m_aaMode == AAMode::DLSS && !IsPathTrace(GetRenderPath())) || IsRayReconstructionActive() || IsRRFallbackToDLSS(); }
    // RR is an option of DLSS (the editor's "Ray Reconstruction" box under AA
    // DLSS): a DLSS_RR request that cannot run off the path tracer (the Raster
    // path has no ray-traced signal; a non-Lit or orthographic view) keeps DLSS
    // Super Resolution running at the same render size instead of dropping to
    // TAA at native. TAA stands in only where DLSS SR itself is unavailable; the
    // path tracer falls back to its accumulation.
    bool IsRRFallbackToDLSS() const;
    // The editor's camera-ray modes - PT Area ReSTIR, PT Camera Rays, PT Lens
    // Radius - belong to the reference path tracer: live only while Path Trace
    // (Reference) is the requested path and held off on every other path (their
    // settings are kept and come back with it). Real-time PT starts at the
    // GBuffer primary - RTXDI's paths and DLSS Ray Reconstruction both need it -
    // and the reference never runs RR (it converges). Keyed on the requested
    // path, so ReSTIR <-> real-time switches do not rebuild Area. The
    // -pt_camera_primary / -pt_area_* / -pt_lens_radius command-line flags still
    // force them on any path (and RR goes inert).
    bool PTCameraModesHeld() const;
    // This frame's RR inputs exist: on the path-trace path, the path tracer is
    // in RR mode with its guides (PathTracePass::RayReconstructionReady) unless
    // DLSS reads the raster guides; the ReSTIR paths' raster guides are written
    // in the same frame. False for the frame a switch turns RR on.
    bool RRInputsReady() const;
    bool IsRayReconstructionActive() const;
    const char* GetRayReconstructionInertReason(bool includeRuntimeFailure=true) const;
    // The RR guide render feature (Renderer/Features/RRGuideFeature.cpp) writes
    // the raster guides - for ray reconstruction on a raster path, or with
    // -rr_raster_guides beside the path tracer's GBuffer primary (=1 a shadow for
    // the equality gate, =2 DLSS reads them instead of the path tracer's).
    // Whether DLSS reads them:
    bool DLSSReadsRasterRRGuides() const;
    bool IsRayReconstructionResolved() const;
    // The DLSS-RR model (Streamline 2.14.1 / DLSS 310.9.1 add F, the SDK's
    // "latest and default"). -dlss_rr_preset=d|e|f overrides the setting
    // (-dlss_rr_preset_e means E). D unless something asks: the RR packs were
    // judged with D.
    void SetDLSSRRModel(SL::DLSSRRModel m) { m_dlssRRModel=m; }
    SL::DLSSRRModel GetDLSSRRModel() const;
    SL::DLSSRRModel GetDLSSRRModelSetting() const { return m_dlssRRModel; }   // without the CLI override (settings save)

    // True iff some external upscaler owns the resolve this frame. A test that
    // really means "an upscaler replaces TAA / FXAA" or "render res != display
    // res" must use this, not IsFSRActive(), or DLSS silently misses the
    // exemptions FSR gets.
    bool IsUpscalerActive() const { return IsFSRActive() || IsDLSSActive(); }

    // Map the shared render-scale enum onto DLSS's own quality modes. Native
    // means DLAA (1:1, anti-aliasing only) rather than "no upscaler", because
    // at Native the user has still explicitly asked for DLSS.
    SL::DLSSMode GetDLSSQualityMode() const
    {
        switch (m_upscaleMode)
        {
            case UpscaleMode::Quality:          return SL::DLSSMode::MaxQuality;
            case UpscaleMode::Balanced:         return SL::DLSSMode::Balanced;
            case UpscaleMode::Performance:      return SL::DLSSMode::MaxPerformance;
            case UpscaleMode::UltraPerformance: return SL::DLSSMode::UltraPerformance;
            case UpscaleMode::Native:
            default:                            return SL::DLSSMode::DLAA;
        }
    }

    // Which DL model DLSS runs. Forwarded to DLSSPass (defined in the .cpp, so
    // this header need not see DLSSPass). Owned by Renderer, not only the pass,
    // so settings.json and the editor have one owner, and because the SDK's
    // per-mode defaults would otherwise make the Render Scale combo change the
    // model as well as the resolution. See DLSSPass::preset.
    void           SetDLSSPreset(SL::DLSSPreset p);
    SL::DLSSPreset GetDLSSPreset() const;

    // ---- Global view mode (Unity / Unreal-style view-mode switch) ----
    // Is this one of the lighting-bucket isolations? Drives the same environment
    // suppression + AA disable that Wireframe uses, so the isolated bucket reads
    // against a clean background instead of the sky.
    static constexpr bool IsLightingDebugView(ViewMode m)
    {
        return (m >= ViewMode::DirectDiffuse && m <= ViewMode::FormShadow)
            || m == ViewMode::SkyVisibility || m == ViewMode::IblOnly;
    }
    // GBuffer channel views share the same suppression treatment: raw data
    // ramps are unreadable with a sky painted over them or TAA blending them.
    static constexpr bool IsGBufferDebugView(ViewMode m)
    {
        return (m >= ViewMode::BaseColor && m <= ViewMode::Depth) || IsMeshletDebugView(m);
    }
    static constexpr bool IsMeshletDebugView(ViewMode m)
    {
        return m == ViewMode::Meshlets || m == ViewMode::MeshletLod;
    }
    ViewMode GetViewMode() const { return m_viewMode.load(std::memory_order_relaxed); }
    void     SetViewMode(ViewMode m);

    // ---- Debug target view (Renderer/DebugTargets.h) ----
    // The main view shows one render target instead of the lit image - the
    // viewport toolbar's Buffer menu, Lua Render.target, -debugtarget.
    // Independent of the ViewMode above (which re-shades through LightingPass):
    // this paints the target's texels over the final output.
    // ListDebugTargets: the registry, each entry with whether the last frame had
    // it. @p refresh recomputes that now and needs the render lane parked
    // (FramePipeline::DrainFrameWork / IGraphicsDevice::DrainCpuFrame first - Lua,
    // the harness); without it the answer is the snapshot the render half
    // refreshes on request, a frame or two old - what a per-frame UI reads.
    // Thread-safe either way.
    std::vector<DebugTargets::Info> ListDebugTargets(bool refresh = false) const;
    // Show @p v from the next rendered frame on; an empty name = the lit
    // image. False with @p error, and the view unchanged, for a name the
    // registry does not hold, a target the view cannot paint (an array or an
    // integer texture: capture it as data instead) or options that do not fit
    // it (a channel it lacks, min == max, a non-positive exposure / scale).
    // The stored view carries the registry's own name. Any thread.
    bool SetDebugTargetView(const DebugTargets::View& v, std::string& error);
    DebugTargets::View  GetDebugTargetView() const;
    DebugTargets::Shown GetDebugTargetShown() const;
    // Build the view's pipeline NOW rather than at the next frame boundary,
    // so the very next frame paints: Lua's Render.target, after its drain.
    // Main thread, render lane parked.
    void EnsureDebugTargetPipeline();

    // ---- Pass capture (Renderer/PassCapture.h) ----
    // A main-graph texture's content right after one pass, copied in the graph's
    // own command-list chain - the Render Graph panel's preview, the pipe's
    // capture {"attachment", "afterPass"}, -rgcapture. Request and Release latch
    // (any thread); the next frame boundary applies them. GetPassCaptureStatus:
    // the last result (any thread). GetPassCaptureCopy: the copy and its state,
    // for a readback after IGraphicsDevice::DrainCpuFrame - null when there is
    // none.
    void                RequestPassCapture(const PassCapture::Desc& desc);
    void                ReleasePassCapture();
    PassCapture::Status GetPassCaptureStatus() const;
    const RHI::Texture* GetPassCaptureCopy(RHI::ResourceState& state) const;

    // ---- Pass accessors - post-process ----
    TAAPass*          GetTAAPass()          { return m_taaPass.get(); }
    FSR2Pass*         GetFSR2Pass()         { return m_fsr2Pass.get(); }
    DLSSPass*         GetDLSSPass()         { return m_dlssPass.get(); }
    FXAAPass*         GetFXAAPass()         { return m_fxaaPass.get(); }
    ToneMapPass*      GetToneMapPass()      { return m_toneMapPass.get(); }

    // ---- Material preview ----
    // A preview view is an offscreen render into its own device HDR slot, tone
    // mapped by a second ToneMapPass instance and handed to the editor as an
    // ImGui image.
    // Nothing is allocated until the first AcquirePreviewView: a session that
    // never opens the panel pays one null test per frame in Render() and one in
    // BuildRenderScene, and no VRAM or descriptors.

    // Kill switch, both halves, default enabled: the environment variable
    // DX12_PREVIEW=0 (read once, the ThreadContext.h idiom) or editor_prefs.ini's
    // preview.enabled=0 turns it off. Disabled means the PreviewRenderer is never
    // constructed, so this is checked at the construction site, not in a
    // per-frame early-out (which would still pay the VRAM).
    // One truth on purpose: Debug.Stats().previewEnabled, the Game twins' fixture
    // check and the editor widget all read this, so "off" and "on but idle"
    // cannot disagree. Reporting only the env half would let a pref-disabled
    // session pass the twins' `enabledSeen` assertion while the feature is off.
    bool     IsPreviewEnabled() const;
    // The editor half of the kill switch. Pushed once from
    // MaterialPreviewWidget::ProcessPendingActions after EditorLayer has loaded
    // editor_prefs.ini - not from LoadEditorPrefs, which runs in OnAttach, long
    // before App calls SetRenderer. Game.exe never calls it and keeps the
    // default, so the Game twins see previewEnabled true.
    void     SetPreviewPrefEnabled(bool v);

    // The acquire / request / release calls below are the editor UI half, not the
    // game half. MaterialPreviewWidget::Draw calls them from
    // EditorLayer::OnUIRender, which FramePipeline builds on main under a Render
    // thread context (Sys::ThreadCtx::Scope uiContext in BuildEarlyEditorUI or
    // RenderHalf), and the editor ImGui block is ThreadCtx-Exempt. Safe because
    // the render-owner lane is idle while the UI is built: BuildEarlyEditorUI runs
    // after the Render-phase drain, RenderHalf after its entry drain, both before
    // the post. A game-side PiP must not call them from the render half - route
    // its requests through the game half (SetGamePiP). PreviewRenderer checks
    // every latch writer with ENGINE_CHECK_ONCE(PreviewLatchCallerOk()): any
    // editor-host caller, or a context that is not Render.

    // Claims a view id, or Preview::kInvalidView when disabled or the pool is
    // full. Constructs the PreviewRenderer on first use. @p worldCull declares up
    // front that this view is a WorldCull (secondary scene camera) one.
    // Up to Preview::kMaxPreviews WorldCull views may live at once (the split
    // viewports), under two rules, because the lane owns one m_world GraphSet and
    // a RenderGraph has one size. One size: the first live view's accepted
    // request fixes it, and RequestPreview refuses a mismatch with one log line
    // and suspends that view. One lane: the scheduler records at most one
    // WorldCull view per frame and round-robins the rest.
    uint32_t AcquirePreviewView(bool worldCull = false);
    // There is deliberately no PreviewWorldCullSizeAllowed() here: a panel that
    // pre-checks the size and tears its view down on a "no" makes a sole
    // panel's own resize take the two-panel refusal path and leaves the per-view
    // "size refused" state unreachable. A request is judged once, inside
    // RequestPreview (PreviewRenderer::WorldCullSizeAllowed, private, called only
    // by SetRequest). A panel that wants to change the size calls
    // RequestPreviewWorldCullLaneSize below; one that wants to show why it could
    // not reads IsPreviewViewSizeRefused + GetPreviewWorldCullSize.
    // Move the WorldCull lane to a new size. @p views / @p count are the caller's
    // WorldCull view ids; the move succeeds only when they are all of the live
    // ones (PreviewRenderer::RequestWorldCullLaneSize says why that is the safe
    // predicate). False, with one log line naming the holder, when another
    // panel's view is live at another size; the caller's own request then stays
    // at the lane's size or is refused.
    bool     RequestPreviewWorldCullLaneSize(uint32_t w, uint32_t h,
                                             const uint32_t* views,
                                             uint32_t count);
    // The size the WorldCull lane is fixed at (0x0 = not fixed).
    void     GetPreviewWorldCullSize(uint32_t& outW, uint32_t& outH) const;
    // Was this view's last request refused for its size? A refused view is
    // suspended: it keeps its last picture and its SRV, and takes no lane turn
    // until a request at the lane's size arrives.
    bool     IsPreviewViewSizeRefused(uint32_t viewId) const;
    // The pixel size of the texture behind GetPreviewSrv - what the view actually
    // rendered at, which differs from the size the panel asks for while a
    // request is refused or a recompile is pending. False until the view has
    // recorded a picture.
    bool     GetPreviewOutputSize(uint32_t viewId, uint32_t& outW,
                                  uint32_t& outH) const;
    // The view the scheduler should serve first - the hovered split cell.
    // Preview::kInvalidView clears it. Reorders only: it writes no main-viewport
    // input state.
    void     SetPreviewPriorityView(uint32_t viewId);
    // Queues teardown; the HDR slot dies in the next serial window
    // (ServicePreviewReleases), never in the ImGui handler that asked for it. The
    // view id is recycled with its generation bumped, so any result still in
    // flight is dropped.
    void     ReleasePreviewView(uint32_t viewId);
    // Called every frame the panel is visible. Sets the single request latch; a
    // new generation is minted only on a real edge - a changed ViewDesc or a
    // freshly acquired view. There is no refresh-when-idle term: idle is the
    // steady state (see PreviewRenderer::SetRequest).
    // @p mat is copied by value into the latch: the record half runs under
    // ThreadCtx::Render and may not reach back into the editor's buffer, and a
    // hand-written field snapshot that misses a schema field is a silent wrong
    // picture (see WriteMaterialSlot).
    void     RequestPreview(uint32_t viewId, const Preview::ViewDesc& v,
                            const MaterialComponent& mat);
    // LDR SRV of the last preview render recorded; 0 until one exists.
    // Re-resolve every frame, never cache. Usable the same frame: the render half
    // arms it at record time and previewCL is a proven predecessor of primaryCL,
    // so the recording frame's ImGui pass may already sample it
    // (PreviewRenderer::GetSrv).
    uint64_t GetPreviewSrv(uint32_t viewId) const;
    // For -preview_screenshot. Null until a preview has recorded a render.
    const RHI::Texture* GetPreviewOutputTexture(uint32_t viewId) const;
    // The view whose pixels are newest, so a harness capture does not have to
    // know which id the editor happens to hold.
    uint32_t GetLastRenderedPreviewView() const;
    // Monotonic count of preview renders recorded since boot; 0 with no preview
    // layer. The -preview_edit_at harness action latches it across a material
    // edit to prove the picture was rebuilt within one frame
    // (PreviewRenderer::ViewsRenderedTotal says why FrameStats' per-frame counter
    // cannot).
    uint64_t GetPreviewViewsRenderedTotal() const;
    // The same for one view slot (PreviewRenderer::ViewRenderCount): compare two
    // readings - has this view recorded a picture since I pointed it.
    uint64_t GetPreviewViewRenderCount(uint32_t viewId) const;
    // The view's lifetime generation (PreviewRenderer::ViewLease): +1 each time
    // its slot is acquired, so (viewId, lease) names one tenancy.
    uint32_t GetPreviewViewLease(uint32_t viewId) const;
    // A live view's latched picture size; false when the slot is not live
    // (PreviewRenderer::GetViewRequestSize). For Render.views().
    bool     GetPreviewViewRequestSize(uint32_t viewId, uint32_t& outW, uint32_t& outH) const;
    // Does the slot render the world through a camera (a WorldCull view: the
    // PiP, a camera preview) rather than a synthetic material scene?
    bool     IsPreviewWorldCullSlot(uint32_t viewId) const;
    // The camera entity a WorldCull view looks through (SetPreviewCameraEntity).
    Entity   GetPreviewCameraEntity(uint32_t viewId) const
    {
        return (viewId < Preview::kMaxPreviews) ? m_previewCamEntity[viewId] : NullEntity;
    }
    // GBuffer stencil code of the last preview draw (1 PBR / 2 NPR / 3 Unlit); 0
    // with no preview layer or before the first render.
    // PreviewRenderer::LastRenderedStencil says why a per-fixture pack needs it.
    uint32_t GetLastRenderedPreviewStencil() const;
    // 1 when that draw took the forward route (TransparentPass#2); see
    // PreviewRenderer::LastRenderedForward.
    uint32_t GetLastRenderedPreviewForward() const;
    // Serial window on the game half. App calls this where it calls
    // EditorLayer::ProcessPendingActions, the only point at which a descriptor
    // an earlier ImGui::Image captured is provably unreferenced by any in-flight
    // frame (contract: EditorLayer.h).
    void     ServicePreviewReleases();

    // The shutdown half of ServicePreviewReleases: it services the view destroys
    // a release queued and runs neither the thumbnail lane's reference drop nor
    // the idle sweep (whose grace is counted in calls, so an extra one at
    // shutdown could mint a teardown a pack pins exactly). Called from
    // EditorLayer::ReleasePreviewViews once the four panels have handed their
    // views back, and from ~Renderer for the in-game PiP. No-op with no preview
    // layer.
    void     DrainPreviewViewReleases();

    // Shorten the preview layer's idle-teardown grace, in frames. Harness only;
    // 0 restores Preview::kIdleTeardownFrames. Safe before the layer exists:
    // latched here, applied at construction.
    void     SetPreviewIdleTeardownFrames(uint32_t frames);

    // ---- Deferred destroys and the structural mailbox ----
    // Entities destroyed since the last scene build whose material runtime
    // binding must be released. Deferred, not released in the listener:
    // ResolveMaterialRuntime hands out raw pointers into
    // m_entityMaterialRuntimeBindings and m_runtimeMaterialResources, and the
    // render half calls it (from WriteMaterialSlot and from DDGISceneAS's
    // material proxy), so erasing under those pointers is a use-after-free.
    // Drained at the top of BuildRenderScene, before BuildScene_SyncMaterials
    // creates any binding this frame. That order makes it safe against
    // immediate entity-id recycling: a destroy and a respawn in the same
    // gameplay phase reuse the id, but the new entity has no binding until
    // SyncMaterials runs, so the deferred erase can only remove the old one.
    std::vector<Entity> m_pendingBindingRelease;

    // Render-side destroy records, applied at the sync point.
    std::vector<DeferredEntityDestroy> m_deferredDestroys;

    // Mailbox calls; beforeDestroy = m_deferredDestroys.size() when queued.
    std::vector<DeferredSyncCall> m_deferredCalls;
    std::function<bool()> m_renderFrameInFlight;
    bool                  m_deferEntityDestroys = false;
    bool                  m_deferSyncCalls = false;
    DestroyDeferStats     m_destroyDeferStats;
#ifdef WITH_HARNESS

    // ---- Material preview ----
    // Lower the preview mesh-library LRU cap (-preview_meshlib_cap). Harness
    // only, no shipped caller: no corpus workload reaches the 24-entry ceiling,
    // so this lets a pack drive the eviction path. 0 is ignored; values above
    // the shipped cap are accepted (a pack may prove the ceiling does not fire).
    void     SetPreviewMeshLibCap(std::size_t cap);

    // Hold the resolved studio IBL pair back from the preview layer for
    // @p frames calls of SyncPreviewEnvironment, so a pack can force the race
    // where the HDRI lands after the first render (an unlit picture). Harness
    // only; 0 is the shipped behaviour.
    void     SetPreviewIblDelayFrames(uint32_t frames);

    // Move the studio key light for this run (-preview_key_light=<x,y,z>, the
    // travel direction). Harness only. The shipped Preview::kKeyLight puts every
    // self-shadowed point at N.L <= 0, so the studio shadow map is a null test at
    // the panel's default framing; this lets a pack light the subject from where
    // the shadow lands. Latched, so it may be called before any panel exists.
    // Returns false for a zero-length direction, which the caller reports.
    bool     SetPreviewKeyLightDir(float x, float y, float z);

    // Turn the studio shadow pass's alpha-test pixel stage off or on from now on
    // (-preview_shadow_alpha). Harness only. Latched, so it may be called before
    // any panel exists, and applied at once when the preview layer is up - so a
    // fixture schedule can flip it between two windows and capture the same
    // fixture with and without the clipped caster.
    void     SetPreviewStudioShadowAlphaTest(bool on);
#endif
    // Compile the preview RenderGraph, once, in the same serial window as
    // ServicePreviewReleases - never from inside Renderer::Render.
    // RenderGraph::Compile destroys and recreates every physical texture and runs
    // each pass's Init() / OnCompile() serially, all of which allocate
    // descriptors; doing that on or beside a graph worker races them. The panel
    // therefore shows its first pixels one frame after it opens. No-op with no
    // preview layer or no view requested.
    void     ServicePreviewCompile();

    // Resolve a preview mesh override. Game half only: MeshLibrary::Load and
    // MeshManager::RegisterMeshLibMesh allocate descriptor-heap slots and upload
    // buffers.
    // @p meshlibPath is a `.meshlib` and @p meshId an index inside it. A `.imsh`
    // is refused: there is no single-.imsh render pool, so nothing can turn one
    // into a mesh-descriptor slot (see where MeshSourcePath is deserialised in
    // ComponentSerializers.cpp).
    // Returns false and leaves the outputs untouched when the library is missing,
    // the id is out of range, or the mesh is not resident yet; the preview then
    // falls back to its primitive rather than drawing nothing.
    // The library handle is cached per path and released only by OnWorldClear:
    // MeshLibrary::Load is path-deduplicated and refcounted, so calling it every
    // frame from the extract would climb the refcount and pin the library past
    // every scene change.
    bool     ResolvePreviewMesh(const std::string& meshlibPath, uint32_t meshId,
                                uint32_t& outSlot, uint32_t& outIndexCount,
                                float& outFitScale, float outFitOffset[3]);

    // Resolve a whole asset - a `.meshlib` or a `.ipfb` - into a flat list of draw
    // parts. Game half only, for the same reasons as ResolvePreviewMesh: it loads
    // mesh libraries, registers mesh-descriptor slots, parses materials and
    // resolves custom-shader ids.
    // The composition happens here, not on the record half: a prefab's node tree,
    // the `.iscn` it re-spawns and that scene's node transforms are file data,
    // but turning them into world matrices needs the asset database, and the
    // record half runs under ThreadCtx::Render. The parts come back already
    // fitted into the studio camera's unit box, so the record half only
    // transposes them.
    // @p outDropped receives how many parts were discarded at
    // Preview::kMaxPreviewParts; non-zero means the caller renders a deliberately
    // partial picture and must say so.
    // Returns false when nothing could be resolved - a path that does not parse,
    // or a library not streamed in yet. Do not fall back to the primitive sphere
    // on false: a sphere where a prefab was asked for reads as "the prefab looks
    // like this".
    bool     ResolvePreviewAsset(const std::string& assetPath,
                                 Preview::AssetKind kind,
                                 std::vector<Preview::AssetPart>& outParts,
                                 uint32_t& outDropped,
                                 Resource::PrefabSkipCounts* outSkipped = nullptr,
                                 // ViewDesc::partMaterials: placement i draws (*matOverrides)[i] when that
                                 // is non-empty.
                                 const std::vector<std::string>* matOverrides = nullptr);

    // Drop the memoised .ipfb / .iscn composition for one path, or all of them
    // with an empty string. The editor calls it when an asset is re-imported;
    // nothing else needs to, because the composition is a pure function of files
    // the editor does not change behind its own back.
    void     InvalidatePreviewAsset(const std::string& assetPath);

#ifdef WITH_HARNESS
    // Self-check (-preview_asset_verify): spawn what this asset composes to
    // through the real SceneInstanceLoader::Load, walk
    // Resource::CollectPrefabSubtreeDFS (the function PrefabSerializer::LoadPrefab
    // keys its per-part overrides through), and compare the (DFS index -> mesh)
    // mapping and each part's world matrix against the composition. Logs
    // `[PREVIEW] asset order verify: parts=N mismatches=M matrixMismatches=K` and
    // returns true when both counts are zero.
    // Accepts a `.ipfb` (every SceneRef node is spawned and checked against its
    // own DFS) or a bare `.iscn`. It creates and destroys entities, so it is
    // game-half, editor-host, harness-only, and it tears its spawn down with
    // Hierarchy::DestroySubtree.
    bool     VerifyPreviewAssetOrder(const std::string& assetPath, World& world,
                                     Resource::AssetManager& assetMgr);
#endif

    // The CSM's default near-cascade reach in metres (SetShadowDistance).
    static constexpr float kDefaultShadowDistance = 400.0f;

    // ---- Camera preview / in-game PiP (WorldCull views) ----
    struct PreviewFrameSnapshot
    {
        const RHI::GPUBuffer*          instanceBuffer  = nullptr;
        const RHI::GPUBuffer*          materialBuffer  = nullptr;
        const RendererDetail::LightCB* mainLightCB     = nullptr;
        uint32_t pristineRemapIdx = 0;
        uint32_t frameIndex       = 0;
        uint32_t dissolveNoiseIdx = ~0u;
        float    globalTimeSec    = 0.f;
        // The scene's exposure, for the preview's own per-frame-slot buffer and its
        // ToneMapPass. 0 = no reading: the preview uses its neutral 1.0 buffer for
        // both its LightCB exposureBufIdx and its tone map.
        // Two sources, and `sceneExposureExact` says which. Auto exposure off:
        // resolvedPost.autoExposure.manualExposure, the CPU value the main ToneMap
        // uses (no lag; the preview packs' pinned profile). Auto exposure on:
        // AutoExposurePass's readback ring, kFramesInFlight-1 frames behind (the
        // newest slot the GPU has provably retired). PreviewFrameState's body says
        // why the scene's GPU buffer cannot be bound directly.
        float    sceneExposure      = 0.f;
        bool     sceneExposureExact = false;
    };

    struct PreviewMainShadow
    {
        bool     valid = false;
        uint64_t atlasSrv = 0;
        float    matrix[4][16] = {};
        float    splits[4] = {};
        float    texelWorld[4] = {};
        float    bias = 0.f, strength = 0.f, texelSize = 0.f;
        float    blendRange = 0.f, normalOffset = 0.f;
        uint32_t frameIndex = 0;
    };

    // ---- Final viewport output ----
    struct AttachmentSource
    {
        const RHI::Texture* texture   = nullptr;
        RHI::ResourceState  state     = RHI::ResourceState::UNDEFINED;
        std::uint32_t       plane     = 0;
        std::uint32_t       arraySlice = 0;   // a texture array's slice (the CSM cascades)
        bool                deviceHdr = false;
        DebugTargets::Info  info;
    };

    struct AttachmentCamera
    {
        float         nearZ = 0.f, farZ = 0.f, fovY = 0.f;
        bool          orthographic = false;
        float         position[3] = { 0.f, 0.f, 0.f };
        float         forward[3]  = { 0.f, 0.f, 1.f };
        std::uint32_t renderWidth = 0, renderHeight = 0;
    };

    // ---- Reflection probes ----
    struct ProbeBakeRecord
    {
        Entity   entity = NullEntity;
        uint32_t slice  = 0;
        uint64_t frame  = 0;   // the frame whose render half recorded it
        // Per face (0:+X .. 5:-Z): the packets its CullView kept, and of them the
        // probe's own copies of what the main view dropped (ProbeView).
        uint32_t kept[6]   = {};
        uint32_t copies[6] = {};
    };
    static constexpr uint32_t kProbeBakeLog = 16;

    // ---- A prefab preload's renderer half (Renderer_Preload.cpp) ----
    struct PreloadMaterialWarm
    {
        DrawPacket     packet;
        uint32_t       customPSID = 0, customVSID = 0;
        PermutationKey reflectPerm;            // MaterialDrawPermutation
        bool           outline      = false;   // IsOutlineEnabled on an opaque route
        bool           surfaceCache = false;   // SurfaceAtlasEligible's test
    };

    // ---- Frame statistics (editor Profiler panel) ----
    // Cheap counters BuildRenderScene already computes, so "why is this scene
    // slow" starts in the Profiler, not in a capture tool. Filled once per
    // BuildRenderScene; reads are free.
    struct FrameStats
    {
        uint32_t drawPackets      = 0;  // scene draw packets emitted this frame
        uint32_t staticInstSlots  = 0;  // retained static instance slots in use
        uint32_t dynamicInstances = 0;  // dynamic (per-frame) instances staged
        uint32_t lights           = 0;  // resolved lights this frame

        // ---- Triangle census (CPU, exact) ----
        // triangles = vertexOrIndexCount / 3 x instanceCount. Every scene producer
        // emits an index count of a triangle list: with programmable vertex fetch,
        // DrawInstanced is handed the index count and the command signature is
        // D3D12_INDIRECT_ARGUMENT_TYPE_DRAW. The index format (u16 / u32) does not
        // matter to the /3.
        // uint64 because one packet can carry up to kMaxDrawInstances (1M)
        // instances; a u32 accumulator wraps on a dense scene.
        // Not included, since no CPU number exists for them: terrain and grass
        // (AS / MS DispatchMesh, culled in the amplification shader), particles,
        // and UI. On a landscape these totals under-report by an unbounded factor,
        // which is why the HUD labels them "Mesh tris".
        uint64_t triOpaque      = 0;   // DrawFilter::Opaque
        uint64_t triTransparent = 0;   // DrawFilter::Transparent
        uint64_t triWater       = 0;   // DrawFilter::Water
        // DrawFilter::Custom - inverted-hull outline shells. Each is a second
        // candidate carrying the same indexCount as the mesh it wraps, so folding
        // them into triOpaque would read ~2x on every outlined NPR character.
        uint64_t triOutline     = 0;
        // DrawFilter::Shadow. This means re-filtered to the shadow list, not never
        // on screen: FaceOverlayPass and HairShadowMarkPass both consume
        // GetDrawList(Shadow) into the main view.
        uint64_t triShadowOnly  = 0;
        // filter != Shadow && renderInMainPass && (viewMask & MainCamera) - the
        // same predicate the indirect-args writer uses.
        uint64_t triMainView    = 0;
        uint32_t instancesEmitted = 0;

        // Pre-cull scene total, summed before BuildScene_ClassifyCandidates.
        // valid == false when the retained path is off: the gathers then cull inline
        // and no pre-cull set exists. Render as absent, never as 0.
        uint64_t triScene      = 0;
        bool     triSceneValid = false;

        // True when the emit-once path was armed. Static packets then keep their
        // natural filter regardless of the camera frustum, so triMainView is what
        // the CPU submitted, not what is visible, and a Culled% derived from it
        // reads ~0 like a broken culler. triTransparent and triOutline stay
        // camera-dependent even then.
        bool     cpuCullBypassed = false;

        // ---- Post-cull GPU counters (~kFrameSlots frames stale) ----
        // Read back from the compaction CS. Narrower than it looks and not
        // reconcilable with triMainView: CompactCommand only sees commands the meta
        // buffer placed in an indirect group and applies INSTANCE_FLAG_MAIN_PASS +
        // VIEWBIT_MAIN_CAMERA, so transparent, Custom and shadow-only packets are
        // absent. Label it accordingly.
        uint64_t gpuVisibleTriangles = 0;
        uint32_t gpuVisibleInstances = 0;
        uint32_t gpuVisibleCommands  = 0;
        bool     gpuVisStatsValid    = false;
        // ---- Meshlet path ----
        // meshletPath: 0 = GBuffer.vs drew every eligible packet, 1 = the CPU
        // per-draw mesh path, 2 = the indirect one. The counters are the
        // amplification shader's own, read back ~kFramesInFlight frames late;
        // meshletStatsValid false = absent (path off, or not primed yet).
        uint32_t meshletPath         = 0;
        uint32_t meshletTested       = 0;
        uint32_t meshletLodRejected  = 0;
        uint32_t meshletCullFrustum  = 0;
        uint32_t meshletCullCone     = 0;
        uint32_t meshletCullHiZ      = 0;
        uint32_t meshletVisible      = 0;
        // The frame the counters above were recorded in, so a pack can pin a
        // counter to one frame (e.g. cullHiZ == 0 on a camera cut).
        uint32_t meshletStatsFrame   = 0;
        uint64_t meshletTris         = 0;
        bool     meshletStatsValid   = false;
        // Post-cull surviving indirect commands, read back from the GPU and so a few
        // frames stale. Not an instance count - see the note on m_drawCountReadback.
        // valid == false means absent (GPU compaction inactive, or the ring not
        // primed yet): render as null, not 0.
        uint32_t visibleDrawCmds      = 0;
        bool     visibleDrawCmdsValid = false;
        // World-space UI vertices built this frame, so the world_ui pack can assert
        // on the six-pool vertex build, which otherwise produces no observable
        // number: a build that silently produced nothing looks like a scene with no
        // world UI in it.
        uint32_t worldUIVerts         = 0;
        // Material-snapshot instrumentation. matBatches is the distinct-binding-key
        // count SortAndEmit emitted; mismatches is how many times the double-compute
        // oracle disagreed - a silently differing field needs a number, not a pack
        // that happens to stay green.
        uint32_t matBatches           = 0;
        uint32_t matBitsMismatches    = 0;
        uint32_t matSnapMismatches    = 0;

        // Material-preview views rendered / draws submitted for them. One frame late
        // on purpose: they are produced on the render half and folded in by
        // ApplyPreviewFeedback on the game half of the next frame, because
        // HarnessCaptureSystem reads FrameStats from TickPhase::PostRender, which
        // runs concurrently with the render half (see the mailbox members). The two
        // Game packs assert previewViewsRendered == 0 on every frame with no panel
        // open (zero idle cost).
        uint32_t previewViewsRendered = 0;
        uint32_t previewViewDraws     = 0;

        // ---- Which frame the emit-half census describes ----
        // The emit half fills a private copy of this struct (EmitContext::stats) and
        // publishes it, and -brslane runs that half on the render-owner lane, so the
        // copy main reads can be one frame behind main's own frame. The frame is
        // published so "stale by one" and "torn" read differently.
        //   statsFrame    the frame whose emit half produced these counters;
        //                 == the reader's own frame when main runs the emit half.
        //   statsValid    false until the first emit half has published (under
        //                 -brslane that includes frame 0's row). Read it before
        //                 asserting on the counters: 0 and "not produced yet" are
        //                 different answers (as for triSceneValid /
        //                 visibleDrawCmdsValid).
        //   statsFromLane the publishing half ran on a Render-context thread; read
        //                 off ThreadCtx at the publish, not off the -brslane flag (a
        //                 flag parsed but never acted on would look the same).
        uint64_t statsFrame    = 0;
        bool     statsValid    = false;
        bool     statsFromLane = false;
    };

    // ---- The emit lease, checked ----
    enum EmitLeaseBit : uint32_t {
        kEmitLeaseUploadSlot = 1u << 0,
        kEmitLeaseEmitIdx    = 1u << 1,
        kEmitLeaseBatchPlan  = 1u << 2,
        kEmitLeaseSortIdx    = 1u << 3,
    };

    // ---- -surfacepagesearly, asked from outside the renderer ----
    // The hoist's own default, false: alone it costs ~0.07 ms on main's
    // [BUILDZONE] sortEmit; its value is that it makes -brslane possible.
    static constexpr bool kSurfacePagesEarlyDefault = false;

    // ---- -brslane: who runs the emit half ----
    // The first term of the host-aware default (SetBrsLaneHostDefault). True
    // together with FramePipeline::kAnimOverlapDefault (the lane refuses without
    // the -animoverlap barrier): it halves main's [BUILDZONE] sortEmit (~1.3 ->
    // ~0.65 ms) and saves ~0.8-1.0 ms per frame on the CPU-bound AIBench scenes.
    static constexpr bool kBrsLaneDefault = true;

private:
    // ---- Stable instance slots + one DEFAULT instance buffer (retained) ----
    // Dirty staging ranges - staged this SortAndEmit, drained by Render()'s copy
    // CL the same frame (non-empty at the next SortAndEmit = a frame was built
    // but never rendered -> content desync -> full reset, self-healing).
    struct InstanceCopyRange { uint32_t frameSlot, stagingFirst, dstSlot, count; };

    // ---- Shadow-family retained args + per-view GPU cull ----
    static constexpr uint32_t kShadowCullViews = 3; // CSM cascades 0..2 (3 = terrain-only)
    static constexpr uint32_t kSpotCullViewBase  = kShadowCullViews;
    static constexpr uint32_t kSpotCullViews     = 48; // per-frame updates, not resident lights
    static constexpr uint32_t kPointCullViewBase = kSpotCullViewBase + kSpotCullViews;
    static constexpr uint32_t kPointCullViews    = 4;  // == PointShadowPass::kMaxCasters
    static constexpr uint32_t kShadowCullViewsTotal =
        kPointCullViewBase + kPointCullViews;

    // ---- The frame's emitted command layout, double-buffered ----
    struct SceneEmit
    {
        std::vector<IndirectGroup> indirectGroups;
        uint32_t indirectDrawCount = 0;
        // The retained bucket layout (ShadowDrawGroup order): the four buckets and
        // their meshlet twins (+MeshletOffset). base = first command slot in the
        // per-cascade working args, cap = count (== ExecuteIndirect maxCommands; the
        // GPU survivor count is always <= cap).
        uint32_t shadowGroupBase[ShadowDrawGroup::RetainedCount] = {};
        uint32_t shadowGroupCap [ShadowDrawGroup::RetainedCount] = {};
        uint32_t            spotCullCount = 0;
        DirectX::XMFLOAT4X4 spotCullVP[kSpotCullViews]{};       // un-transposed VP
        uint32_t            pointCullCount = 0;
        DirectX::XMFLOAT4   pointCullSphere[kPointCullViews]{}; // xyz center, w radius
        bool gpuCompactionFrameActive = false;   // per-frame latch
        bool shadowArgsFrameActive    = false;   // per-frame latch
        bool perObjCsmExcludeFrame    = false;   // mirror of ShadowPass::SetExcludePerObjectCasters
        // The meshlet gate's path half, latched beside the compaction latch
        // (PrepareLanePlan / SortAndEmit) from MeshletPathPreconditions, so the emit
        // and the render half answer one question asked once.
        bool        meshletFrameActive = false;
        const char* meshletReason      = "off";   // static string; why not, when inactive
        uint32_t    meshletCullMask    = 0;       // MeshletCullCB.cullMask's stage bits
        float       meshletLodTau      = 0.0f;    // MeshletCullCB.lodThreshold; <= 0 = level 0
        // The CSM meshlet path this frame: the main latch, the retained shadow args
        // (the conversion is the shadow cull CS's) and ShadowPass's mesh PSO.
        // Decides the meshlet twin buckets (ShadowRetainedGroup).
        bool        shadowMeshletFrameActive = false;
        float       meshletShadowLodTau      = 0.0f;   // the CSM banks' lodThreshold, texels
    };

    // Dirty spans of persistent slots to blit staging -> DEFAULT. src slot ==
    // dst slot (staging is slot-indexed), so one index suffices. Appended by
    // StageMaterialSlot, drained by Render(). Not cleared on a
    // build-without-render frame: staging bytes survive (a slot is only
    // rewritten after kMatSlotReuseLagEpochs) and append order is time order, so
    // a later range for the same slot always wins.
    struct MaterialCopyRange { uint32_t frameSlot, slot, count; };
    struct MaterialUpload
    {
        std::vector<MaterialCopyRange> ranges;
        // True: the next Render() blits the whole staging table into the DEFAULT
        // material buffer instead of the ranges. Set at creation (the default-PBR
        // pre-fill seed - never-written slots must not read DEFAULT-heap garbage)
        // and by StageMaterialSlot when the ranges fragment.
        bool                           fullInit = true;
    };

    // Per-draw-local-instance world bounds, flattened out of the candidates by
    // SortAndEmit and read by the scene-voxel cull in the render half.
    // Indexed by draw-local instance index (dp.instanceOffset + inst), not by
    // instance slot. Per-slot would be wrong three ways: the dynamic-overflow
    // fallback aliases several instances onto slot 0, the duplicate-emit branch
    // skips the record write, and with retained instances on a per-slot array
    // keeps stale entries for every slot not emitted this frame.
    // It must not ride the instance remap (m_instanceRemapBuf) either, which is
    // filled only when retained instances are active: a lane pushed there would
    // vanish without them and degrade the cull to never-cull.
    struct InstAabb { DirectX::XMFLOAT3 min, max; };

    // ---- Shared runtime material resources ----
    struct MatTexEntry {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
        MaterialRuntimeTextureView view;
    };
    struct RuntimeMaterialResource {
        uint32_t refCount = 0; // number of entities bound to this runtime layout
        uint64_t shaderKey = 0;
        std::array<MatTexEntry, MaterialComponent::TEXTURESLOT_COUNT> engineTextures;
        // MaterialComponent::NameMap - a transparent hasher, so CustomTex() can probe
        // with a string_view instead of allocating a std::string per texture query
        // per candidate (HashMatParams hits this every rebuild).
        MaterialComponent::NameMap<MatTexEntry> customTextures;
        // Event-driven promote: once every valid texture handle in this resource is
        // Ready, its runtime views are frozen (bindless indices are stable per handle
        // until Release), and PromoteRuntimeMaterialTextures skips this resource
        // every frame instead of re-fetching all SRVs. A texture only goes NotLoaded
        // -> Loading -> Ready within a resource's life (a world reload erases and
        // recreates the resource), so the latch is never wrongly stuck true. A
        // never-ready (failed) texture keeps it false, so that resource alone keeps
        // polling.
        bool allTexturesResolved = false;
    };
    // resourceKey is a 64-bit hash of the normalized shader paths, engine texture
    // paths and sorted custom binding-name / path pairs (BuildMaterialRuntimeKey).
    // Dynamic scalar params (HeadForward, dissolve, etc.) deliberately do not
    // participate, so per-entity animation can share immutable texture /
    // shader-layout ownership. A hash, not a string, because it is recomputed for
    // every material every frame - see the collision note in
    // BuildMaterialRuntimeKey.
    struct RuntimeMaterialEntityBinding {
        uint64_t resourceKey = 0;
        std::array<MaterialRuntimeTextureView, MaterialComponent::TEXTURESLOT_COUNT>
            engineOverrides{};
        std::array<uint8_t, MaterialComponent::TEXTURESLOT_COUNT> overrideMask{};
    };

public:
    // Pre-resolved runtime material texture views for one entity. Resolve the
    // entity's shared runtime resource + binding once (two map lookups) via
    // ResolveMaterialRuntime, then read every engine / custom slot from it instead
    // of GetMaterialTextureRuntime's per-slot lookups (~12 hashmap lookups per
    // candidate on the per-draw hot path). Thread-safe to build and read (const):
    // the runtime maps are mutated only in BuildScene_SyncMaterials, which
    // finishes before the parallel gather resolves these. Non-owning; the
    // pointers are valid only within the current frame build.
    struct ResolvedMaterialRuntime {
        const RuntimeMaterialEntityBinding* binding  = nullptr;
        const RuntimeMaterialResource*      resource = nullptr;
        MaterialRuntimeTextureView EngineTex(uint32_t slot) const;
        MaterialRuntimeTextureView CustomTex(std::string_view name) const;
    };
    struct MaterialCpuWrite {
        void* target = nullptr;
        uint32_t bytes = 0;
        bool baseRecord = false;
        const MaterialComponent* material = nullptr;
        ResolvedMaterialRuntime runtime{};
        const ShaderReflect::Reflection* ps = nullptr;
        const ShaderReflect::Reflection* vs = nullptr;
        uint32_t snapshotIndex = ~0u; // owned retained material source, never an ECS pointer
        bool frameRecord = false;     // base record: WriteMaterialSlot's frameRecord
    };

    // ---- The one custom-shader binding packer ----
    // Where PackCustomMaterialBindings writes: the b8 constant slice bound at
    // root 35 and the eight-descriptor space3 table bound at root 36. Two modes,
    // chosen by `slot` alone:
    //   * slot != kNoSlot - the main path. Destinations are chosen inside the
    //     packer from the persistent per-slot stores (m_customMatCbvStore /
    //     m_customMatSrvStore) with the per-frame rings as the fallback, and the
    //     five per-slot bookkeeping arrays are maintained. The six fields below
    //     are ignored.
    //   * slot == kNoSlot - a caller-owned destination (the preview). The packer
    //     touches nothing slot-keyed: not the stores, not the rings, not
    //     m_matCustomCbvVA / m_matCustomTexHandle / m_matSlotCustomPacked /
    //     m_matSlotCustomOversize / m_matSlotSrvBlock. Those are game-half
    //     members whose block allocation is keyed on a persistent material slot,
    //     which a preview does not have; a second writer would also race
    //     SortAndEmit under threaded Game.exe.
    // The caller-owned CBV slice must be at least `cbvCapacity` bytes and stay
    // alive while the draw is in flight (a per-frame ring slot, not one shared
    // buffer), and the descriptor block must hold
    // GraphicsDX12::kCustomMatTextureSlots contiguous descriptors in the
    // shader-visible heap. A reflected b8 layout larger than cbvCapacity is
    // reported and not written: outCbvVA stays 0 and the caller must not draw.
    struct CustomPackDest
    {
        static constexpr uint32_t kNoSlot = ~0u;
        uint32_t                    slot             = kNoSlot;
        bool                        persistentCustom = false;
        // Caller-owned destinations (slot == kNoSlot only).
        void*                       cbvCpu      = nullptr;
        uint64_t                    cbvGpu      = 0;
        uint32_t                    cbvCapacity = 0;
        uint64_t                    texCpu      = 0;   // CPU descriptor handle
        uint64_t                    texGpu      = 0;
        uint32_t                    texInc      = 0;
        // Results, both modes. outCbvVA is 0 when nothing was packed (no b8 cbuffer
        // declared, or no destination); outTexHandle is 0 when the space3 table was
        // not filled, which on a custom draw means do not draw (an unbound root 36
        // is a DEVICE_HUNG, not a black texture).
        uint64_t                    outCbvVA       = 0;
        uint64_t                    outTexHandle   = 0;
        uint32_t                    outCbBytes     = 0;
        uint32_t                    outTexResolved = 0;
    };

    // ---- Retained-scene dirty feed ----
    struct SceneDiffView
    {
        bool valid = false;                       // false -> the consumer must full-rebuild
        uint32_t frameStamp = 0;                  // diff generation - consumers detect gaps
        const std::vector<Entity>* dirtyXform = nullptr;
        const std::vector<Entity>* dirtyOther = nullptr;
        const std::vector<Entity>* added      = nullptr;
        const std::vector<Entity>* removed    = nullptr;
        const std::vector<Entity>* matDirty   = nullptr;
        // True while any material runtime resource still has a texture that has
        // not finished loading. A consumer that retains resolved texture indices must
        // not arm its patch path on such a frame: an unresolved slot resolves to
        // bindlessIndex -1, and a -1 latched into a retained blob is replayed
        // forever (the entity is clean by the time the load lands, so nothing
        // re-opens the gate). The raster candidate cache reads m_matTexUnsettled
        // directly; DDGISceneAS is on the far side of the Renderer interface and
        // reads it here, so RT / DDGI do not sample -1 (flat baseColor, alpha-clip
        // treated as solid) for a material whose texture resolves a frame or more
        // after it went dirty.
        bool matTexUnsettled = false;
        // Renderer::ClassifySceneAdds: the adds are all skinned and the dirtyOther /
        // matDirty entries are all theirs; and the entities bound to a resource
        // whose textures are still arriving (null when none are).
        bool addsSkinnedOnly = false;
        bool addsOwnDirt     = false;
        const std::vector<Entity>* texUnsettledEntities = nullptr;
    };

    // ---- Material preview ----
    // Bumped whenever the composition memo or the preview mesh-library map is
    // dropped (InvalidatePreviewAsset, OnWorldClear). The extract folds it into
    // its dirty stamp, so an invalidation re-resolves the parts on the next frame
    // and nothing else does. Monotonic; a wrap costs one extra resolve.
    uint32_t PreviewAssetMemoEpoch() const { return m_previewAssetMemoEpoch; }

    // The retry term for one asset path, folded into that asset's extract stamp
    // beside the global memo epoch. Zero for an asset that is not failing.
    // Not the memo epoch: that is in every target's stamp, so one missing asset
    // would de-idle every other asset target every frame. This term moves only
    // for the failed asset, once every kPreviewAssetRetryFrames frames.
    // Derived from the frame clock, not bumped by the failure: a counter only a
    // failing resolve can move deadlocks against the idle path only a moved stamp
    // can leave (the extract matches its stored stamp on an empty part vector,
    // where `allBound` is vacuously true, and never asks again). The clock makes
    // the term advance by itself while the entry exists; the entry is erased on
    // the first success (ClearPreviewAssetLoadRetry), which stops it.
    uint32_t PreviewAssetRetryTick(const std::string& assetPath) const
    {
        auto it = m_previewAssetRetry.find(PreviewAssetKey(assetPath));
        if (it == m_previewAssetRetry.end()) return 0u;
        // +1 so a failing asset can never derive the same term as a healthy one
        // (bucket 0 is reachable in the first 30 frames of a session).
        return 1u + static_cast<uint32_t>(m_previewAssetFrame /
                                          kPreviewAssetRetryFrames);
    }

    // The one spelling every per-asset preview map is keyed on (the composition,
    // the unfitted flag, the retry clock): Resource::MeshLibCanonicalPath, the
    // same canonical form Resource::MeshLibrary::HashPath uses - lexically normal
    // first (a prefab's `../` reference), then Sys::RelativizeAssetPath, case
    // kept. A raw caller string would make a panel opened from a browser tile
    // (working-directory-relative, native separators) and an
    // InvalidatePreviewAsset from the watcher (project-relative, forward slashes)
    // name two entries for one file.
    static std::string PreviewAssetKey(const std::string& assetPath);

    // True when this asset's bounds were degenerate - zero extent, or not finite
    // (a corrupt library AABB) - so the studio auto-fit could not derive a scale
    // and the parts were drawn at their authored size. Set by both fit sites: the
    // multi-part ResolvePreviewAsset (keyed on the asset path) and the
    // single-mesh ResolvePreviewMesh (keyed on the `.meshlib` path, that path's
    // asset identity). The panel says so; nothing else should branch on it.
    bool     IsPreviewAssetUnfitted(const std::string& assetPath) const
    {
        // Both fit sites write the set with PreviewAssetKey, so the query must use
        // it too: the panel asks with the spelling the user typed / the browser
        // produced, not the one the resolve had.
        return m_previewUnfittedAssets.count(PreviewAssetKey(assetPath)) != 0;
    }

    // The studio fit this asset's parts were drawn with (composed space -> the
    // rig's unit box is (p - centre) * scale), so a panel can project its own CPU
    // overlays onto the picture. False until the asset has been composed and
    // fitted (a degenerate box is drawn unfitted and has none). Game half.
    bool     GetPreviewAssetFit(const std::string& assetPath, float outCentre[3],
                                float& outScale) const
    {
        auto it = m_previewAssetFits.find(PreviewAssetKey(assetPath));
        if (it == m_previewAssetFits.end()) return false;
        outCentre[0] = it->second.centre[0];
        outCentre[1] = it->second.centre[1];
        outCentre[2] = it->second.centre[2];
        outScale     = it->second.scale;
        return true;
    }

    // The files an asset preview depends on - the asset itself, every `.iscn` a
    // prefab expands, every `.meshlib` a part comes from - and the distinct
    // material paths its parts name. The asset browser stamps all of them into a
    // cached tile's dependency set, so a re-imported model or an edited material
    // invalidates the tile.
    // Composes on demand through ResolvePreviewAsset's memo, so asking costs a
    // map lookup after the first time. Game half only. Returns false when the
    // asset does not parse.
    bool     GetPreviewAssetDeps(const std::string& assetPath,
                                 Preview::AssetKind kind,
                                 std::vector<std::string>& outFiles,
                                 std::vector<std::string>& outMaterials);

    // ---- Camera preview / in-game PiP (WorldCull views) ----
    // Game half. Resolve every live WorldCull preview view's camera out of the
    // World into its request. The editor window and the Lua / harness PiP only
    // name an entity; this turns it into a RenderCamera with the same two
    // branches App uses for the main camera: the VirtualCameraComponent
    // quaternion path (keeps roll) first, then GlobalTransform's basis
    // (position = world.r[3], forward = r[2], up = r[1]) with the lens from
    // CameraComponent.
    // Called from BuildRenderScene before ExtractPreviewRequests: the extract
    // mints the generation that hands the request over, and a request handed
    // over with last frame's camera lags a frame. No-op with no preview layer.
    void     ExtractPreviewCameras(World& world);

    // Game half only. Cull the scene's punctual lights against each live
    // WorldCull view's exact frustum, cap the survivors by importance, and push
    // the result into the preview layer for the record half to upload.
    // Runs at the end of BuildRenderScene, from ExtractPreviewRequests, the first
    // point where m_frameLights is final (BuildScene_GatherPrimitivesAndLights
    // fills it, BuildScene_UploadLights assigns the shadow slices). It reads
    // m_frameLights read-only and writes a fixed-size POD array the preview layer
    // owns, so nothing crosses the seam that the render half could re-resolve
    // (the same shape as SetSceneLighting).
    // No idle cost: it returns before touching m_frameLights when no live
    // WorldCull view exists, so a game with no PiP and an editor with only a
    // material inspector pay one compare per frame.
    void     ExtractPreviewSceneLights();

    // Point preview view @p viewId at a scene camera entity. NullEntity turns the
    // view back into "nothing to resolve", which makes its requests bail rather
    // than draw the origin. The editor's CameraPreviewWindow calls this from its
    // combo; SetGamePiP calls it for the in-game view.
    void     SetPreviewCameraEntity(uint32_t viewId, Entity cameraEntity);

    // Game half. Turn the in-game picture-in-picture on for @p cameraEntity at
    // @p w x @p h, rendering at most one frame in @p tickInterval. Game.exe
    // compiles the preview layer in and registers nothing until somebody calls
    // this, which keeps preview_view_game's per-frame `previewViews == 0`
    // assertion green for a shipped game. ClearGamePiP() is the circuit breaker:
    // it takes the feature back out with no recompile.
    // This only queues: it writes a game-owned pending struct, and
    // ApplyPendingGamePiP (top of BuildRenderScene, inside the extract window,
    // after FramePipeline::DrainFrameWork) is the single point that acquires or
    // releases a view. The -preview_pip harness block sits at the tail of
    // App::Run's frame loop, where the previous frame's render half may still be
    // inside ProcessQueue reading the Target fields an acquire rewrites.
    // Returns false when the request is unusable (preview layer disabled via
    // DX12_PREVIEW=0, null entity, zero size); an acquire failure is logged by
    // the apply. Calling it again with another entity or size re-points the
    // existing view instead of acquiring a second one.
    bool     SetGamePiP(Entity cameraEntity, uint32_t w, uint32_t h,
                        uint32_t tickInterval = 1);
    // Queues the PiP off (see SetGamePiP for why it only queues). The apply
    // releases the preview view - its HDR slot and its target - and forgets the
    // camera entity, so GetGamePiPComposite answers false and nothing is
    // composited.
    // It does not tear down the WorldCull GraphSet: PreviewRenderer's idle sweep
    // does (RenderGraph::Destroy) once no WorldCull view has been live for
    // Preview::kIdleTeardownFrames, so the physical textures and pass instances
    // go on a grace and re-arming the PiP inside that window costs nothing.
    void     ClearGamePiP();
    // Game half, from BuildRenderScene's extract window. Applies whatever
    // SetGamePiP / ClearGamePiP queued. One apply point, after the park.
    void     ApplyPendingGamePiP();
    // Where the PiP composites, in back-buffer pixels. A zero (or negative) width
    // or height means auto: bottom-right with a 16 px margin, resolved at
    // composite time. Lua: Preview.SetPiPRect.
    void     SetGamePiPRect(int32_t x, int32_t y, int32_t w, int32_t h);
    // The rect the PiP composites into, in back-buffer pixels, plus the SRV to
    // draw. False when there is no PiP or it has not rendered yet. Render half
    // (FramePipeline::RenderHalfAsync, after the previewCL dependency edge); the
    // SRV is re-resolved here every frame, never cached.
    bool     GetGamePiPComposite(uint64_t& outSrv, int32_t& outX, int32_t& outY,
                                 int32_t& outW, int32_t& outH) const;
    Entity   GetGamePiPEntity() const { return m_pipEntity; }
    // The PiP's view slot (Preview::kInvalidView while off or not yet acquired -
    // SetGamePiP only queues) and the size / tick it was set to.
    uint32_t GetGamePiPView() const { return m_pipView; }
    void     GetGamePiPSettings(uint32_t& outW, uint32_t& outH, uint32_t& outTick) const
    { outW = m_pipW; outH = m_pipH; outTick = m_pipTick; }
    // Draw accounting of a WorldCull record: how many packets it drew and how
    // many the frame's draw lists offered; 0/0 with no preview layer. The pair
    // proves the secondary view rendered the scene: a view that culls everything
    // and one that culls nothing are both bugs, and neither shows in a mean-luma
    // band.
    // Per view: @p viewId defaults to Preview::kInvalidView, meaning whichever
    // view recorded last - with a split panel, whichever cell the round-robin
    // served last.
    uint32_t GetPreviewWorldCullDraws(
        uint32_t viewId = Preview::kInvalidView) const;
    uint32_t GetPreviewWorldCullCandidates(
        uint32_t viewId = Preview::kInvalidView) const;
    // How many of those draws came from the secondary-view lists - geometry the
    // main camera cannot see. 0 with a camera pointed elsewhere means the
    // secondary classify lane is broken, and that failure renders a plausible
    // picture.
    uint32_t GetPreviewWorldCullSecondary(
        uint32_t viewId = Preview::kInvalidView) const;
    // How many scene punctual lights the record half last bound for @p viewId -
    // what LightCB.clusterLightCount said. Same per-view convention as above
    // (kInvalidView = whichever world view recorded last).
    // Read at the bind, not at the cull: a run where the game half culled ten
    // lights and the render half refused them on the frame stamp reports 0, so a
    // pack asserting > 0 in every on window catches that off-by-one.
    uint32_t GetPreviewWorldClusterLights(
        uint32_t viewId = Preview::kInvalidView) const;
    // The process-wide off-arm for WorldCull scene lights. Game half. False draws
    // every WorldCull view with the scene's sun and IBL only (no point or spot
    // lights); Preview::Feature::kNoWorldLights is the per-view version and the
    // shipped circuit breaker. This one exists because the corpus's cost gate
    // (expect.frames.p50Delta) is an in-process A/B and a boot flag cannot
    // alternate windows. No-op with no preview layer.
    void     SetPreviewWorldSceneLights(bool on);
    bool     PreviewWorldSceneLightsEnabled() const;
    // The preview frame counter, incremented once per ProcessQueue entry (once
    // per frame the preview layer was serviced). The clock
    // ViewDesc::tickIntervalFrames is measured on - not Engine.Frame(): a run
    // with no preview panel never advances it. 0 with no preview layer.
    uint64_t GetPreviewFrame() const;

    // What an asset preview view last resolved to: how many draw parts, how many
    // distinct materials among them, and how many parts were dropped at
    // Preview::kMaxPreviewParts. The panel reports all three: "12 packets" and
    // "12 materials" are different claims, and a partial picture must say so on
    // screen and in the log. All zeros for a non-asset view and with no preview
    // layer.
    void     GetPreviewAssetStats(
        uint32_t viewId, uint32_t& outParts, uint32_t& outMaterials,
        uint32_t& outDropped,
        Resource::PrefabSkipCounts* outSkipped = nullptr) const;

    // Render half. The WorldCull view's second frustum cull: this camera's
    // SceneView (exact frustum, its ViewBit) through CullView - this frame's
    // Opaque / SecondaryView / Transparent / SecondaryViewBlend lists down to
    // packets with a surviving instance, appended to @p out with the natural
    // filters and the Transparent tail re-sorted back-to-front against this
    // camera.
    // Costs the game half nothing: it reads the BVH BuildRenderScene built (with
    // GPU culling off the tree is not built and every packet is kept: CullView's
    // tested-vs-visible rule). @p out must outlive the Execute that consumes it
    // (the DrawList contract).
    // Packets whose viewMask lacks @p viewBit are dropped; survivors get viewMask
    // ~0u, because GBufferPass tests ViewBit::MainCamera unconditionally and the
    // copy is preview-local.
    void     CullWorldForPreview(const RenderCamera& cam, uint32_t viewW,
                                 uint32_t viewH, uint32_t viewBit,
                                 std::vector<DrawPacket>& out) const;
    // Render half. What @p view draws of this frame's packets
    // (Renderer/ViewCull.h): the scene BVH asked with view.cull.volume, the lists
    // @p req names filtered by the tested-vs-visible rule, the batch rule and the
    // view's ViewBit, appended to @p out in the request's list order. One call at
    // a time (member scratch).
    void     CullView(const RG::SceneView& view, const ViewCullRequest& req,
                      std::vector<DrawPacket>& out, ViewCullStats* stats = nullptr) const;

    // Render half. Everything a WorldCull preview needs from this frame's
    // main-path state, in one snapshot: the scene-level buffers it binds instead
    // of its synthetic private ones (root 8 instances, root 10 / 28 materials);
    // PerViewCB byte 228's pristine remap heap index - never the working half,
    // whose lower range is a GPU copy this command list does not wait on (the
    // shadow family's rule); the scene clock its custom vertex shaders sway on;
    // and the main LightCB the secondary view starts from.
    // Any field can be null / zero on an early frame; the record then declines to
    // draw rather than substituting.
    PreviewFrameSnapshot PreviewFrameState() const;

    // Render half. The main view's CSM, for a secondary view to borrow. The
    // cascades are fitted to the main camera, so a secondary camera looking
    // elsewhere gets correct shadows only where the two frusta overlap - a known
    // limitation; re-fitting would need a second ShadowPass and four more slices.
    PreviewMainShadow PreviewMainShadowSnapshot() const;
    // Render half. The main SkyboxPass's per-frame sun / moon / starfield state,
    // for the preview's own SkyboxPass instance to adopt. A copy of the CPU
    // inputs, never a read of the main pass's SkyCB ring: that ring is another
    // graph's rotating per-frame state and must not be cached across the seam
    // (the same rule as PreviewMainShadowSnapshot). `valid` is false with no main
    // pass, which leaves the preview sky a plain radiance cube.
    SkyboxPass::SkyState PreviewSkyState() const;

    // ---- The asset browser's thumbnail lane ----
    // Thin forwards to PreviewRenderer's one-shot lane. All of it runs in the
    // game half's serial window (EditorLayer::ProcessPendingActions), before
    // BeginFrame and so before ExtractPreviewRequests hands anything over.
    // Constructing the layer is gated on the same kill switch as
    // AcquirePreviewView, so DX12_PREVIEW=0 turns tiles off too.
    bool     QueuePreviewThumbnail(const Preview::ThumbRequest& r);
    void     CancelPreviewThumbnail(uint64_t assetKey);
    bool     HasPreviewThumbnailInFlight(uint64_t assetKey) const;
    // The lane refused itself for this session (an HDR output mode, whose
    // tone-map output is FP16 and cannot be copied into an R8G8B8A8 tile). The
    // browser must stop creating tiles while this is true: treating the refusal
    // as "skip to the next entry" walks the whole folder every frame, creating a
    // texture and re-parsing a material per entry.
    bool     ArePreviewThumbnailsDisabled() const;
    std::size_t PreviewThumbnailQueueDepth() const;
    void     TakePreviewThumbnailResults(std::vector<Preview::ThumbResult>& out);
    // The oldest finished readback, for the editor's .itex encoder. False when
    // nothing is pending; the bytes stay valid until the next call. Never a
    // FlushAndWait: the ring is kFrameCount deep and the slot handed back is one
    // the GPU has provably retired.
    bool     TakePreviewThumbnailPixels(uint64_t& outAssetKey,
                                        uint32_t& outGeneration,
                                        const uint8_t*& outRgba,
                                        uint32_t& outRowPitch,
                                        uint32_t& outWidth, uint32_t& outHeight);
    uint64_t GetPreviewThumbnailsRenderedTotal() const;

    AutoExposurePass* GetAutoExposurePass() { return m_autoExposurePass.get(); }
    BloomPass*        GetBloomPass()        { return m_bloomPass.get(); }
    LensFlarePass*    GetLensFlarePass()    { return m_lensFlarePass.get(); }
    XeGTAOPass*       GetXeGTAOPass()       { return m_xegtaoPass; }
    CASPass*          GetCASPass()          { return m_casPass.get(); }
    // One-shot: capture the Tonemap output, init shard physics from impactUV
    // [0..1], composite shards until the duration elapses. Re-call to restart.
    // The glass shatter is a render feature (IGlassShatter).
    void TriggerGlassShatter(float impactU, float impactV);

    // ---- Pass accessors - UI ----
    UIPass*               GetUIPass()      { return m_uiPass.get(); }
    WorldUIBillboardPass* GetWorldUIPass() { return m_worldUIPass.get(); }
    // Editor-only debug billboard icons (light/camera gizmos). DebugDrawSystem
    // fills it each frame via AddIcon(); empty (no draw) in Game builds.
    DebugIconPass*        GetDebugIconPass() { return m_debugIconPass.get(); }
    // The Texture Viewer's pictures (Renderer/TextureViewService.h): panel-owned
    // targets painted from one texture on the restore list. Never null after
    // Init.
    TextureViewService*   GetTextureViews() { return m_textureViews.get(); }
    // Off-screen Canvas UI pictures - the UI Editor's designer view
    // (Renderer/UIViewService.h): painted in the UI paint window, drawn on the
    // restore list. Never null after Init.
    UIViewService*        GetUIViews() { return m_uiViews.get(); }
    // The editor's Show > Screen UI off: the Canvas UI is still laid out (hit
    // tests, Engine.GetUIComputedRect, a designer's "match main") but not
    // painted into the main view. The editor sets it every frame (never in
    // Play); false = painted normally.
    void SetHideCanvasUI(bool hide) { m_hideCanvasUI = hide; }
    bool HideCanvasUI() const       { return m_hideCanvasUI; }
    // Lazy-load (and cache) a debug-icon texture by asset path and return its
    // bindless table index (== handleId), or 0xFFFFFFFF if missing / not ready.
    // Generic so any debug icon kind works by just passing its texture path.
    uint32_t              GetDebugIconBindless(const char* path);

    // Register a glyph-baked debug icon: create an engine texture (once) from
    // `rgba` (w x h, R8G8B8A8, one pixel per texel, tightly packed) and cache it
    // by `codepoint`; returns its bindless index (== handleId). Idempotent - a
    // second call for the same codepoint returns the cached index. The editor
    // fills `rgba` by copying the glyph sub-rect out of the ImGui font atlas.
    uint32_t              RegisterGlyphIcon(uint32_t codepoint, const void* rgba,
                                            uint32_t w, uint32_t h);
    // Bindless index of a previously-registered glyph icon, or 0xFFFFFFFF.
    uint32_t              GetGlyphIconBindless(uint32_t codepoint) const;

    // ---- Final viewport output ----
    // SRV GPU handle of the final tone-mapped LDR output (the editor viewport
    // texture).
    uint64_t GetFinalOutputSrvHandle() const;
    // Capture tonemap output to PNG. Synchronous (flushes GPU). False on error.
    bool CaptureViewportToPNG(const char* path);

    // The pipe's capture {"attachment"} (Harness/AttachmentDecode.h): a main-view
    // render target as the last frame left it, by any name the debug target
    // registry holds (ListDebugTargets: the GBuffer's albedo, normal, surface,
    // depth, velocity, extra; hdr; ssao, rtao, rtshadow; a render-graph
    // texture's name) - its texture, the state its tracker holds for it, the
    // plane to read (0; a depth's plane 0 is the depth), or deviceHdr for the
    // device's own HDR target (IGraphicsDevice::ReadHdrSceneRaw), and the
    // registry entry (canonical name, semantic, meaning). False with @p why for
    // an unknown name or a target the last frame did not have. Ask after
    // IGraphicsDevice::DrainCpuFrame: the trackers are the render lane's until
    // then.
    bool GetAttachmentSource(const std::string& name, AttachmentSource& out, std::string& why) const;
    // capture {"attachment", "afterPass"}: the main-graph texture @p name stands
    // for (a registry name's graph texture - "stencil" is GBuffer_Depth's plane
    // 1, "hdr" the HDR import - or a graph texture's own name) and the entry the
    // decoder reads (made from its format when the registry has none). False
    // with @p why. Serial windows.
    bool ResolvePassCaptureTarget(const std::string& name, std::string& graphName, std::uint32_t& plane,
                                  DebugTargets::Info& info, std::string& why) const;
    // @p name is a debug target made on request (shadingModel) that the last
    // frame did not make: this call asks for it, and the next frames make it.
    // The capture service then waits a frame instead of failing.
    bool DebugTargetNeedsProduce(const std::string& name);

    // The camera that frame was drawn with: a depth value means nothing
    // without its near / far (reverse-Z: 1 at near, 0 at far - the sky).
    AttachmentCamera GetAttachmentCamera() const;
    // The exposure the last frame's tonemap multiplied by - read back from the
    // exposure buffer itself, so it is exact in both modes (auto exposure
    // writes it; with auto exposure off the pass uploads the manual value
    // there). Synchronous (a frame-boundary readback): for a capture's
    // metadata, never per frame. False with @p error when there is no pass or
    // the readback failed.
    bool ReadFrameExposure(float& exposure, bool& autoExposure, std::string& error);

    // ---- Post-process stack ----
    // The stack is a pure consumer of PostProcess::Runtime::resolved (filled by
    // PostProcessResolveSystem each frame). Volumes / profiles / overrides live
    // in the ECS + ProfileSystem + Runtime, not here.
    PostProcess::PostChain*          GetPostChain()                { return m_postChain.get(); }
    const PostProcess::PostChain*    GetPostChain() const          { return m_postChain.get(); }

    // ---- Reflection probes ----
    uint64_t GetReflectionProbeArraySrv()  const { return m_probeMgr.GetArraySrv(); }
    uint32_t GetActiveProbeCount()         const { return m_probeMgr.GetActiveProbeCount(); }
    // Editor-driven bake API. BakeAllProbes walks m_lastWorld -> call after
    // BeginFrame.
    void BakeProbe(uint32_t cubeSlice);
    void BakeAllProbes();

    // Export every non-realtime, baked reflection probe's cubemap to a .itex
    // beside @p worldPath (in a "<worldStem>_probes" folder) and stamp its path
    // (relative to the working directory when possible; a probe that already
    // has a path keeps it) onto ReflectionProbeComponent::bakedCubemapPath. Call
    // right before Resource::SaveScene so the component serializer persists the
    // paths; after the next LoadScene, BuildScene_SelectProbes restores a cubemap
    // whose bakeDataVersion is current from disk instead of enqueueing a full
    // re-bake. Realtime probes are skipped (their path is cleared - they re-bake
    // on an interval).
    void ExportBakedProbeCubemaps(World& world, const std::string& worldPath);

    // The reflection probes the render half baked - what the harness's
    // -probedump reads back (the probe array's six faces of each slice, mip 0,
    // at a frame boundary; the array keeps a slice until it is baked again).
    // ProbeBakeCount() is how many bakes were recorded so far (any thread);
    // ProbeBake(n) the n-th (0-based), kept for the last kProbeBakeLog bakes -
    // read it only once the frame is drained (IGraphicsDevice::DrainCpuFrame): a
    // pipelined render half can record the next bake before the harness looks.
    uint64_t            ProbeBakeCount() const { return m_probeBakeCount.load(std::memory_order_acquire); }
    ProbeBakeRecord     ProbeBake(uint64_t n) const { return m_probeBakeLog[n % kProbeBakeLog]; }
    const RHI::Texture& GetProbeArrayTexture() const { return m_probeMgr.GetArrayTexture(); }

    // ---- DDGI ----
    bool                                     IsDDGIReady() const { return m_ddgiReady; }
    DDGI::DDGIVolumeManager&                 GetDDGIManager()       { return m_ddgiMgr; }
    const DDGI::DDGIVolumeManager&           GetDDGIManager() const { return m_ddgiMgr; }
    // What the indirect lighting runs with this frame: the scene's
    // IndirectLightingSettings (or that component's defaults) with
    // ddgiEnabled / ssgiEnabled capped by the player (BuildScene_UpdateDDGI).
    // Read-only: the scene build rewrites it every frame - change the scene's
    // component (Entity.set "IndirectLighting") or the player's rows instead.
    const IndirectLightingSettingsComponent& GetIndirectLightingSettings() const { return m_ddgiSettings; }

    // The player's DDGI / SSGI allowances - features.ddgi / features.ssgi, what
    // those rows capture and settings.json saves. A scene's
    // IndirectLightingSettings says whether its world uses DDGI / SSGI (its
    // intent), these say whether this machine allows them, and what runs is
    // both: BuildScene_UpdateDDGI applies it every frame and the settings store
    // records the same rule (Settings::Policy::Capped). So the player's "off"
    // holds in every scene, and a scene's "off" never reaches the player's file.
    // -ddgi_off still forces DDGI off on top.
    void SetDDGIAuthored(bool v);
    bool GetDDGIAuthored() const  { return m_ddgiAuthored; }
    void SetSSGIAuthored(bool v);
    bool GetSSGIAuthored() const  { return m_ssgiAuthored; }
    // The scene's intent the last scene build read: its IndirectLightingSettings,
    // or that component's defaults (DDGI yes, SSGI no) when it has none.
    bool GetSceneDDGIIntent() const { return m_sceneDdgiIntent; }
    bool GetSceneSSGIIntent() const { return m_sceneSsgiIntent; }
    DDGIProbeDebugPass*                      GetDDGIProbeDebugPass() { return m_ddgiProbeDebugPass.get(); }

    // ---- Decal subsystem ----
    Resource::DecalMaterialLibrary& GetDecalMaterialLibrary() { return m_decalMaterialLibrary; }
    DecalSpawner&                   GetDecalSpawner()         { return m_decalSpawner; }

    // ---- VFX subsystems ----
    class TracerSystem*     GetTracerSystem()     { return m_tracerSystem.get(); }
    class BeamSystem*       GetBeamSystem()       { return m_beamSystem.get(); }
    class AfterimageSystem* GetAfterimageSystem() { return m_afterimageSystem.get(); }
    // Interactive water ripples - App registers Engine.RippleAt against this.
    class WaterRippleSystem* GetWaterRippleSystem() { return m_waterRippleSystem.get(); }

    // The main view's render graph, read-only, for introspection in a serial
    // window (the -dump_catalog harness dump: RG::RenderGraph::DescribePasses).
    const RG::RenderGraph& GetMainGraph() const { return m_graph; }

    // ---- Skeletal animation ----
    SkeletonRegistry& GetSkeletonRegistry() { return m_skin.GetSkeletonRegistry(); }
    ClipLibrary&      GetClipLibrary()      { return m_skin.GetClipLibrary();      }
    MorphClipLibrary& GetMorphClipLibrary() { return m_skin.GetMorphClipLibrary(); }
    // Morph.GetPoolWeights: see SkinnedMeshSubsystem::ReadPoolWeights.
    bool ReadMorphPoolWeights(Entity skeleton, std::vector<std::pair<uint16_t, float>>& out)
    { return m_skin.ReadPoolWeights(skeleton, out); }
    AnimGraphSystem* GetAnimGraphSystem() { return m_skin.GetAnimGraphSystem(); }
    AnimationSystem*  GetAnimationSystem()  { return m_skin.GetAnimationSystem();  }
    IKRigSystem*      GetIKRigSystem() { return m_skin.GetIKRigSystem(); }
    IKSystem*         GetIKSystem()         { return m_skin.GetIKSystem();         }
    HeadTrackSystem*  GetHeadTrackSystem()  { return m_skin.GetHeadTrackSystem();  }
    // A module's chain stages (Anim/AnimChainHook.h); not owned.
    void AddAnimChainHook(IAnimChainHook* hook) { m_skin.AddChainHook(hook); }
    ChainPhysicsSystem* GetChainPhysicsSystem() { return m_skin.GetChainPhysicsSystem(); }
    // The chain solver's preview lane (ChainPhysicsSystem::SetPreviewLane). The
    // set is caller-owned and read inside TickAnimationChain's chain Update, so
    // the caller may only rewrite it outside that call - the Chain Physics
    // Preview panel does so in the editor's ProcessPendingActions window.
    // nullptr hands the lane back. `steps` fixed steps this frame (0 with dt > 0 =
    // hold the last simulated pose).
    void SetChainPhysicsPreviewLane(const std::unordered_set<Entity>* ents, float dt,
                                    std::uint32_t steps = 1)
    { if (ChainPhysicsSystem* c = m_skin.GetChainPhysicsSystem()) c->SetPreviewLane(ents, dt, steps); }
    // See SkinnedMeshSubsystem::MarkBuffersUnsnapshotted. The Chain Physics
    // Preview panel marks every stage entity right before it destroys the stage,
    // so its skinned buffers are released instead of being parked on the
    // editor's orphan list for the rest of the session.
    void     MarkSkinnedBuffersUnsnapshotted(Entity e) { m_skin.MarkBuffersUnsnapshotted(e); }
    // Harness census (the chain_preview_isolation pack): numbers that must read
    // the same before a panel opens and after it closes.
    uint32_t GetSkinnedOwnedBufferEntityCount() const { return m_skin.OwnedGpuBufferEntityCount(); }
    uint32_t GetSkinnedOrphanedBufferCount() const    { return m_skin.OrphanedGpuBufferCount(); }
    uint64_t GetSkinnedUnsnapshottedReleased() const  { return m_skin.UnsnapshottedBuffersReleased(); }
    uint32_t GetSkinnedSharedSourceCount() const      { return m_skin.SharedSourceCount(); }
    // A skinned part keyed @p sourceKey would borrow a live shared source
    // (SceneLoader::SkinnedMeshPending::sharedOnly). Game half.
    bool HasSharedSkinnedSource(uint64_t sourceKey) const { return m_skin.HasSharedSource(sourceKey); }
    // A preload's skinned part: its shared source built with no instance
    // (SkinnedMeshSubsystem::PublishSharedSource). Game half.
    bool PublishSharedSkinnedSource(const SceneLoader::SkinnedMeshPending& pending,
                                    const SkinnedMeshSubsystem::SharedSourceDerived* derived = nullptr)
    { return m_skin.PublishSharedSource(m_gfx, pending, derived); }
    // The same, built over several calls, a few buffers each
    // (SkinnedMeshSubsystem::BeginSourceBuild / StepSourceBuild). Game half.
    std::shared_ptr<SkinnedMeshSubsystem::SourceBuild> BeginSharedSkinnedSourceBuild(
        const SceneLoader::SkinnedMeshPending& pending, const SkinnedMeshSubsystem::SharedSourceDerived* derived)
    { return m_skin.BeginSourceBuild(m_gfx, pending, derived); }
    bool StepSharedSkinnedSourceBuild(SkinnedMeshSubsystem::SourceBuild& build) { return m_skin.StepSourceBuild(build); }
    // The same, with every stage on a loader worker
    // (SkinnedMeshSubsystem::BuildSourceOnLoader, any thread), then the cache
    // insert (PublishSharedSkinnedSourceBuild, game half).
    static bool BuildSharedSkinnedSourceOnLoader(SkinnedMeshSubsystem::SourceBuild& build)
    { return SkinnedMeshSubsystem::BuildSourceOnLoader(build); }
    bool PublishSharedSkinnedSourceBuild(SkinnedMeshSubsystem::SourceBuild& build) { return m_skin.PublishSourceBuild(build); }

    // ---- A prefab preload's renderer half (Renderer_Preload.cpp) ----
    // The preload itself is Resource/PrefabSpawnQueue.h. Bumped at the top of
    // every OnWorldClear, a world-survives clear too: a preload published before
    // it holds GPU halves that clear released (its handle answers `stale`).
    // Main thread.
    uint64_t GetWorldClearCount() const { return m_worldClearCount; }

    // One material, prepared on main (game half): its custom shaders registered
    // exactly as ResolveCustomPSID / ResolveCustomVSID register them (the same
    // map entry, the same nine-library fan-out) but without the reflection - the
    // first resolve's shader miss, which WarmPreloadMaterial takes on a worker -
    // and the draw packet the gather and emit would make of it (ApplyBlendMode's
    // routing), plus what else its first frames would build: the material
    // outline's PSO, an atlas page of the material surface cache.
    PreloadMaterialWarm PreparePreloadMaterial(const MaterialComponent& m, bool skinned);
    // Any thread (a preload's TaskSystem worker; the shader registries and PSO
    // caches are thread-safe): the GBuffer library's reflection union of each id
    // (what the spawn's resolve would load on main), then every pass's PSO for
    // the packet (their WarmPipelines).
    void WarmPreloadMaterial(const PreloadMaterialWarm& w);
    // Main thread, render lane idle (App's AssetTick): the material surface
    // cache's atlas and the bake pass's scratch MRTs + compute PSOs, which the
    // first atlas-eligible material's frames would otherwise create on main and
    // in the render half. A preload's listed big step. false = the feature is
    // off (or a creation failed).
    bool PrewarmSurfaceCache();
    // ANY thread (a preload's worker): the shaders PrewarmSurfaceCache's compute
    // PSOs are built from, loaded first.
    void WarmPreloadSurfaceCache();
    uint32_t GetPerObjectShadowSetSize() const
    { return static_cast<uint32_t>(m_playerShadowSet.size()); }
    uint32_t GetSceneBVHLeafCount() const { return m_sceneBVH.GetLeafCount(); }
    // Chain physics follows the editor transport even though skeletal rendering
    // itself remains active in Edit/Pause. Reset also invalidates LocalPose scratch
    // so a new Play cannot inherit rotations written by the previous run.
    void SetChainPhysicsSimulationEnabled(bool enabled);
    // The scene plays but its time stands still (time scale 0, the path tracer's time freeze): the
    // chains hold their simulated pose instead of the rest pose (ChainPhysicsSystem::SetHold).
    void SetChainPhysicsHold(bool hold);
    void ResetChainPhysicsSimulation();
    CharacterStateSystem* GetCharacterStateSystem() { return m_skin.GetCharacterStateSystem(); }
    void              SetPhysicsSystem(DX12Physics::PhysicsSystem* p) { m_skin.SetPhysicsSystem(p); }
    void              SetAnimationClipSystem(Resource::AnimationClipSystem* cs) { m_skin.SetAnimationClipSystem(cs); }
    PoseRingBuffer&   GetPoseRingBuffer()   { return m_skin.GetPoseRingBuffer();   }
    // Register a skinned entity. Adds MeshSkinnedComponent + SkinningOutputComponent.
    void RegisterSkinnedMesh(World& world, Entity e,
                             const DirectX::XMFLOAT3* positions,
                             const DirectX::XMFLOAT3* normals,
                             const BlendVertex*       blendData,
                             uint32_t                 vertexCount,
                             uint32_t                 baseMeshDescIdx)
    {
        m_skin.RegisterSkinnedMesh(m_gfx, world, e, positions, normals,
                                   blendData, vertexCount, baseMeshDescIdx);
    }
    // All-in-one registration from SceneLoader::SkinnedMeshPending.
    bool RegisterSkinnedMeshFull(World& world, const SceneLoader::SkinnedMeshPending& pending)
    {
        return m_skin.RegisterSkinnedMeshFull(m_gfx, m_meshMgr, world, pending);
    }

    // ---- Custom shader queries ----
    // The interface a material's custom shader is synced and packed from
    // (id <= 0 -> nullptr): its source declarations, else - with the material -
    // permutation 0 merged with the variant it draws with
    // (Renderer/MaterialPermutation.h), without it permutation 0.
    const ShaderReflect::Reflection* GetCustomShaderReflection(int customShaderID,
                                                               const MaterialComponent* mc = nullptr) const;
    // What the variants of a custom shader compiled so far by the material
    // libraries (GBuffer's and Transparent's) read: the Inspector's "not read"
    // mark beside a declared parameter. known = false until one of them
    // compiled a variant.
    ShaderLibrary::DynamicReads GetCustomShaderReads(int customShaderID) const;

    // The ShaderLibrary that compiles material shaders, exposed so an offline
    // pass can reflect a shader no live entity uses: the material-instance repair
    // walks .imatinst files on disk and needs each one's declared //@ parameter
    // defaults, which exist only in a Reflection. Null before the GBuffer pass is
    // built. Do not cache the pointer across a renderer teardown.
    // Read-only for external callers: use FindDynamic / GetDynamicReflection /
    // GetReloadEpoch on it. Get a material shader's id from
    // RegisterMaterialShader below (one registry for every library, so the id is
    // the same whichever library is asked).
    ShaderLibrary* GetMaterialShaderLibrary() const;
    // DDGISceneAS's PT material cache: the inputs of a material record that
    // MaterialComponent dirt does not cover - a texture finishing its load and a
    // custom material shader's hot reload (its reflection).
    uint64_t GetTextureContentRevision() const;
    uint32_t GetMaterialShaderReloadEpoch() const;
    // The device's shader pool: slots used / its cap. A ratchet - every compile a
    // hot reload loads retires the slot it replaces (the Material Editor's live
    // graph compile reports what an edit costs).
    std::size_t ShaderPoolUsed() const;
    std::size_t ShaderPoolCapacity() const;

    // A material custom shader's dynamic id, or
    // ShaderLibrary::kInvalidDynShaderID for an empty path: one registration in
    // the process's DynamicShaderRegistry, which every pass library resolves
    // through - no per-library registration, no order to keep. Also the door for
    // editor-side code that resolves a shader no live entity uses.
    // Game / extract half only: its callers insert into m_runtimeMaterialShaders,
    // which the render half reads; checked inside.
    uint32_t RegisterMaterialShader(const char* path, RHI::ShaderStage stage);

    std::string                      GetCustomShaderError(int customShaderID) const;

    // ---- Runtime toggles ----
    bool IsGPUCullingEnabled() const   { return m_gpuCullingEnabled; }
    void SetGPUCullingEnabled(bool v)  { m_gpuCullingEnabled = v; }

    // ---- Depth prepass ----
    // Pre-populates depth so the 6-MRT GBuffer early-Z rejects occluded
    // fragments. Default off: it costs an extra geometry pass, so whether it wins
    // depends on the scene's overdraw - A/B it, do not assume.
    // GetDepthPrepassDrawCount() reports how many draws it covered last frame
    // (draws that can discard - alpha-test, custom PS, dissolve - are excluded).
    void     SetDepthPrepassEnabled(bool v);
    bool     IsDepthPrepassEnabled() const;
    uint32_t GetDepthPrepassDrawCount() const;

    bool IsIndirectDrawEnabled() const { return m_useIndirectDraw; }
    void SetIndirectDrawEnabled(bool v){ m_useIndirectDraw = v; }
    // GPU compaction (per-instance cull + count buffers). Safe to flip at
    // runtime: the latch re-evaluates at the next SortAndEmit, and instance
    // records carry viewMask / instFlags in both states.
    bool IsGPUCompactionEnabled() const { return m_gpuCompaction; }
    void SetGPUCompactionEnabled(bool v){ m_gpuCompaction = v; m_gpuCompactionRequested = v; }

    // What was asked for, before the boot-time capability demotion. Renderer
    // clears m_gpuCompaction when the instance-remap buffers fail to create, so
    // IsGPUCompactionEnabled() reports the demoted value; engine.json is sealed
    // into game.ipak, so capturing the demotion would ship one broken dev
    // machine's "compaction off" to every player with no runtime path to correct
    // it. The config captures this instead.
    bool GetGPUCompactionRequested() const { return m_gpuCompactionRequested; }
    // Async-compute kickoff: closes the submit batch after the DDGI demote CL so
    // the compute queue stops waiting on the whole graphics prologue. A runtime
    // A/B toggle - see m_asyncComputeEarlyKickoff.
    void SetAsyncComputeEarlyKickoff(bool v){ m_asyncComputeEarlyKickoff = v; }
    bool GetAsyncComputeEarlyKickoff() const { return m_asyncComputeEarlyKickoff; }
    // Editor bisect toggles for the GPU-cull chain (angle-dependent
    // false-cull triage: Hi-Z vs per-instance frustum vs whole chain).
    bool IsHiZOcclusionEnabled() const  { return m_hiZEnabled; }
    void SetHiZOcclusionEnabled(bool v) { m_hiZEnabled = v; }
    bool IsRetainInstancesEnabled() const { return m_retainInstances; }
    // Shadow-family retained args: static packets emit once (no camera-dependent
    // DrawFilter::Shadow conversion) and CSM consumes the shared pristine args
    // through per-cascade GPU cull views. Safe to flip at runtime: the latch
    // re-evaluates each BuildRenderScene and every stage (classify, meta fill,
    // dispatch, ShadowPass) reads the same latch.
    bool IsRetainedShadowArgsEnabled() const { return m_retainedShadowArgs; }
    void SetRetainedShadowArgsEnabled(bool v){ m_retainedShadowArgs = v; }
    // On: the Spot / Point shadow passes honour castShadow and the shadow
    // viewMask like CSM. Off: they ignore both.
    bool IsSpotPointHonorShadowFlags() const { return m_spotPointHonorShadowFlags; }
    void SetSpotPointHonorShadowFlags(bool v){ m_spotPointHonorShadowFlags = v; }
    bool IsAnimCullingEnabled() const  { return m_skin.IsAnimCullingEnabled(); }
    void SetAnimCullingEnabled(bool v) { m_skin.SetAnimCullingEnabled(v); }
    // Skeletons in an owner's pinned set are classified kFrustumPass by
    // PrepareAnimationVisibility whatever the main camera / CSM see, so a panel's
    // hidden preview stage (km below the scene, visible only to a secondary
    // view) keeps animating with anim culling on. Each owner (the Chain Physics
    // Preview, the Skeleton Editor) hands in its own set; classify reads their
    // union. The sets are owned by the callers and read on TaskSystem workers
    // (classify fans out to ParallelFor when skelN >= 16): neither a set nor the
    // owner list may change while PrepareAnimationVisibility runs. The only
    // writers are the panels' ProcessPendingActions (the game half's serial
    // window), which never overlaps it. No owner (the default) = one size
    // compare per skeleton. Adding again replaces the owner's set; the order is
    // registration order. -anim_pin_single: only the first owner's set is read.
    void AddAnimPinOwner(const void* owner, const std::unordered_set<Entity>* s);
    void RemoveAnimPinOwner(const void* owner);
    bool HasAnimPinOwner(const void* owner) const;
    std::size_t AnimPinOwnerCount() const { return m_animPinOwners.size(); }
    // Effective value - what renders this frame: the player's allowance AND the
    // scene's intent (Settings::Policy::Capped).
    bool IsSSAOEnabled() const         { return m_ssaoEnabled; }

    // Authored value - the player's settings.json allowance (features.ssao),
    // which only the player path may write. settings.json persists this, never
    // the effective value above, so a scene's post-process config cannot leak
    // into the player's options file. Same requested-vs-effective shape as
    // m_aaModeRequested.
    void SetSSAOAuthored(bool v);
    bool GetSSAOAuthored() const       { return m_ssaoAuthored; }
    // The scene's AO intent: its .ippc's switch (PostProcessConfig::ApplyTo; the
    // World Settings AO checkbox), or that file's default - on - for a scene
    // without one. @p writer names it in the settings store's Scene layer
    // (Settings.where("features.ssao")). Main thread.
    void SetSSAOSceneIntent(bool on, std::string_view writer);
    bool GetSSAOSceneIntent() const    { return m_ssaoSceneIntent; }

    // Volumetric fog, the same split: the player's allowance
    // (features.volumetricFog -> VolumetricFogPass::SetAllowed) and the scene's
    // intent - its Volumetric Fog component, its local fog volumes, else its
    // .ippc's switch (SetFogSceneConfigIntent -> VolumetricFogPass::SetEnabled).
    // Defined in the .cpp because the pass type is only forward-declared here.
    void SetVolumetricFogAuthored(bool v);
    bool GetVolumetricFogAuthored() const { return m_volFogAuthored; }
    // The .ippc's fog switch (or its default - off - for a scene without one),
    // named @p writer in the settings store when it decides the scene's intent.
    void SetFogSceneConfigIntent(bool on, std::string_view writer);
    // Whether the scene wants fog at all, as the game half last resolved it
    // (the component, the local volumes, else the .ippc) - editor readout.
    bool GetSceneFogIntent() const { return m_sceneFogIntent.load(std::memory_order_relaxed); }
    // Scene-owned fog state as the game half last resolved it (editor readout).
    bool     IsVolumetricFogSceneOwned() const
    { return (m_volFogDemandBits.load(std::memory_order_relaxed) & kFogDemandOwned) != 0; }
    uint32_t GetLocalFogVolumeTotal() const { return m_localFogTotal.load(std::memory_order_relaxed); }
    uint32_t GetLocalFogVolumeKept()  const { return m_localFogKept.load(std::memory_order_relaxed); }
    bool IsSSREnabled()  const         { return m_ssrEnabled; }
    void SetSSREnabled(bool v)         { m_ssrEnabled = v; }
    // The RT switches as the player asked for them (what a Save stores); the setters run one only with inline RT.
    struct RTSwitchRequests { bool reflections = false, ao = false, shadows = false; };
    RTSwitchRequests GetRTSwitchRequests() const { return m_rtRequested; }
    // Ray-traced reflections (RTReflectionPass), default off (TDR budget). The scene TLAS is built even
    // in DDGI-less scenes when RTR is on; active DDGI volumes enrich hit shading with probe light.
    bool IsRTReflectionsEnabled() const { return m_rtReflectionsEnabled.load(std::memory_order_relaxed); }
    void SetRTReflectionsEnabled(bool v){ m_rtRequested.reflections = v; m_rtReflectionsEnabled.store(v && m_gfx.GetCapabilities().inlineRaytracing, std::memory_order_relaxed); }
    // Forward translucent surfaces fold the same-frame traced reflection of the
    // opaque surface behind them (coplanar + close gate in custom_surface.hlsli
    // MS_FetchTracedReflection). A pure LightCB weight (fwdReflStrength) that
    // toggles no pass, so it is live the frame it changes. Default on: it is
    // bit-identical to off wherever RTR / SSR traced nothing.
    bool IsTransparentReflectionsEnabled() const { return m_transparentReflectionsEnabled.load(std::memory_order_relaxed); }
    void SetTransparentReflectionsEnabled(bool v){ m_transparentReflectionsEnabled.store(v, std::memory_order_relaxed); }
    // Max GBuffer roughness that still traces a ray (GGX-glossy band; pixels
    // above it fall to the probe/sky tiers). Pushed to the pass per frame.
    float GetRTReflectionsMaxRoughness() const { return m_rtrMaxRoughness; }
    void  SetRTReflectionsMaxRoughness(float r)
    {
        // NaN-guard before the clamp: NaN passes both comparisons and would reach
        // the trace CB (`roughness > NaN` is false for every pixel -> full-screen
        // ray allocation), then poison settings.json via CaptureFrom (nlohmann
        // writes NaN as null).
        if (!(r == r)) r = 0.55f;
        m_rtrMaxRoughness = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r);
    }
    // Max world-unit distance an RT reflection ray travels. Beyond it the ray
    // misses and the sky cube fills in, which makes distant geometry "disappear
    // into the sky" in a mirror. Also scales REBLUR's hit-distance normalization
    // (A = rayTMax * kHitDistAScale), so it is a denoiser knob as well as a
    // range / cost knob. Pushed to the pass per frame.
    // Upper clamp 400 = ShadowSystem::kShadowFarCap: past that, reflections would
    // show geometry the CSM does not shadow. Raise it only while watching
    // RTReflectionPass GPU ms - a longer ray turns cheap misses into full
    // material-proxy hit shading (see the cost note on RTReflectionPass::rayTMax).
    float GetRTReflectionsRayTMax() const { return m_rtrRayTMax; }
    void  SetRTReflectionsRayTMax(float v)
    {
        // NaN-guard before the clamp - same reasoning as MaxRoughness above.
        if (!(v == v)) v = 150.0f;
        m_rtrRayTMax = (v < 20.0f) ? 20.0f : (v > 400.0f ? 400.0f : v);
    }
    // Alpha-blend (translucent) objects as partial RT sun-shadow occluders:
    // glass darkens its shadow by its opacity instead of casting a solid
    // silhouette. Off reverts the TLAS instance flags, so every RT consumer
    // returns to the opaque-only traversal cost - the valve if the extra
    // non-opaque traversal is too expensive on glass / smoke-heavy content.
    // Applied to DDGISceneAS each frame.
    bool IsRTShadowBlendOccludersEnabled() const { return m_rtShadowBlendOccluders; }
    void SetRTShadowBlendOccluders(bool v)       { m_rtShadowBlendOccluders = v; }
    // NRD (REBLUR_SPECULAR) as the RTR denoiser; OFF = the built-in temporal
    // + spatial chain. Pushed to the pass per frame; the pass auto-falls-back
    // to the classic chain if the NRD instance failed to initialize.
    bool IsRTReflectionsNRDEnabled() const { return m_rtrUseNRD; }
    void SetRTReflectionsNRDEnabled(bool v){ m_rtrUseNRD = v; }
    // Ray-traced AO - a sub-mode of the AO toggle (effective only while SSAO is enabled): swaps
    // XeGTAO's main pass for an inline-RayQuery hemisphere trace against the scene TLAS; the XeGTAO
    // denoise / temporal chain and the Lighting wiring are unchanged. Keeps the TLAS alive even when RTR is off.
    bool IsRTAOEnabled() const { return m_rtaoEnabled; }
    void SetRTAOEnabled(bool v){ m_rtRequested.ao = v; m_rtaoEnabled = v && m_gfx.GetCapabilities().inlineRaytracing; }
    // Ray-traced sun shadows (RTShadowPass) - a full-screen visibility mask
    // min()'d into the CSM result by Lighting.ps (LightCB rtShadowMaskIdx). Needs
    // DXR and a directional light; keeps the TLAS alive even when RTR is off.
    bool IsRTShadowsEnabled() const { return m_rtShadowsEnabled.load(std::memory_order_relaxed); }
    void SetRTShadowsEnabled(bool v){ m_rtRequested.shadows = v; m_rtShadowsEnabled.store(v && m_gfx.GetCapabilities().inlineRaytracing, std::memory_order_relaxed); }
    // RT budget watchdog: when on, a rolling EMA of the RTReflectionPass /
    // RTShadowPass GPU regions hard-disables the feature after ~3 s sustained
    // over kRTBudgetMs. Default off: it only fires while the editor GPU profiler
    // is open, and on a heavy scene it silently kills the feature mid-evaluation
    // with only a log line to explain it. The always-on safety nets (ray TMax,
    // pixel-list capacity, roughness gate) do not depend on it. Turn it on when
    // validating a perf budget on target hardware.
    bool IsRTBudgetGuardEnabled() const { return m_rtBudgetGuardEnabled; }
    void SetRTBudgetGuardEnabled(bool v){ m_rtBudgetGuardEnabled = v;
                                          m_rtrOverBudgetFrames = 0;
                                          m_rtShadowOverBudgetFrames = 0; }
    // Budget ceiling (ms) shared by both watchdogs. Only meaningful while
    // IsRTBudgetGuardEnabled().
    float GetRTBudgetMs() const { return m_rtBudgetMs; }
    void  SetRTBudgetMs(float v){ m_rtBudgetMs = (v > 0.0f && v == v) ? v : 4.0f; }

    // ---- Material surface cache ----
    // Bakes a custom-shader material's own GBuffer output into an atlas page so
    // ray hits sample the real look instead of an engine guess at its textures
    // (docs/design/MaterialSurfaceCache.md).
    // Off by default, and off is bit-identical to not having it: no atlas
    // allocation, no bake pass activity, no proxy change, no TLAS instance flag.
    // Turn it on only for content whose reflections need it (NPR characters): a
    // bake re-issues a real mesh-section draw.
    bool IsMaterialSurfaceCacheEnabled() const { return m_surfaceCache.IsEnabled(); }
    void SetMaterialSurfaceCacheEnabled(bool v) { m_surfaceCache.SetEnabled(v); }
    uint32_t GetMaterialSurfaceCachePagesPerFrame() const
    { return m_surfaceCache.GetPagesPerFrame(); }
    void SetMaterialSurfaceCachePagesPerFrame(uint32_t n)
    { m_surfaceCache.SetPagesPerFrame(n); }
    // Always non-null, even when the feature is off: DDGISceneAS reads the
    // publish epoch on the off -> on and on -> off edges to invalidate its
    // retained gather. LookupEntity returns "no page" when off.
    const MaterialSurfaceCache* GetMaterialSurfaceCache() const { return &m_surfaceCache; }
    // Editor-only: hide all light icon billboards (entities with LightData +
    // BillboardComponent). Skips DrawCandidate emission for those entities so
    // the billboard quad never reaches the GBuffer / Transparent pass.
    bool AreLightBillboardsVisible() const { return m_lightBillboardsVisible; }
    void SetLightBillboardsVisible(bool v) { m_lightBillboardsVisible = v; }
    // Per-object (player) high-res directional shadow - global master toggle.
    // The per-frame pass enable is also gated on whether a player caster exists.
    bool IsPerObjectShadowEnabled() const { return m_perObjectShadowEnabled; }
    void SetPerObjectShadowEnabled(bool v){ m_perObjectShadowEnabled = v; }

    // Dev measurement only - never ship this on. Forces AnimVisibilityCull's
    // shadow branch to cull, so the cost of shadow-only skeletons can be
    // measured (holdout on vs off) rather than estimated. Rendering is wrong
    // while set: a skeleton with no pose falls back to the bind-pose mesh slot,
    // so its shadow becomes a T-pose. AnimProfile::shadowHoldout records that a
    // measurement was taken under it.
    bool IsAnimShadowHoldout() const  { return m_animShadowHoldout; }
    void SetAnimShadowHoldout(bool v) { m_animShadowHoldout = v; }
    // CSM per-cascade shadow resolution (EngineSettings
    // "shadows.csmResolution"). Stored as the desired value; the ShadowPass
    // atlas is sized from it at Compile(), so a change takes effect at the next
    // Compile() / restart. Set before Compile() at boot (App applies
    // EngineSettings there).
    void     SetShadowResolution(uint32_t s) { m_shadowResolution = s ? s : 4096u; }
    uint32_t GetShadowResolution() const     { return m_shadowResolution; }

    // The next three setters are EngineSettings "shadows.*" (the shadows group's
    // level). The CSM's near-cascade reach in metres (ShadowSystem::FrameInput,
    // every frame; [50, 1000]). The default equals ShadowSystem::kShadowFarCap
    // (checked where the fit reads it, Renderer_FrameData.cpp).
    void  SetShadowDistance(float m)
    {
        if (!(m == m)) m = kDefaultShadowDistance;
        m_shadowDistance = m < 50.0f ? 50.0f : (m > 1000.0f ? 1000.0f : m);
    }
    float GetShadowDistance() const { return m_shadowDistance; }
    // The highest spot-shadow tier the per-frame tier pick may hand a light:
    // 512 = base only, 1024 = up to medium, 2048 = up to high (all three; the
    // default). The atlases stay as they are - only the pick is capped.
    void     SetSpotShadowMaxResolution(uint32_t px) { m_spotShadowMaxTier = px >= 2048u ? 2u : (px >= 1024u ? 1u : 0u); }
    uint32_t GetSpotShadowMaxResolution() const      { return 512u << m_spotShadowMaxTier; }
    // Each point-shadow cube face: the desired value, like the CSM's - the
    // PointShadowPass atlas is sized from it at Compile(), so a change takes
    // effect at the next launch. Set before Compile() at boot.
    void     SetPointShadowResolution(uint32_t px)
    {
        m_pointShadowResolution = (px == 256u || px == 512u || px == 2048u) ? px : 1024u;
    }
    uint32_t GetPointShadowResolution() const { return m_pointShadowResolution; }

    // Global material-texture anisotropy (EngineSettings "textures.anisotropy").
    // Stored as the desired value and consumed by sampler-owning passes during
    // Compile(); changing it after Compile therefore takes effect on restart.
    // 1 means anisotropy off (the passes create a trilinear sampler instead).
    void SetTextureAnisotropy(uint32_t x)
    {
        m_textureAnisotropy = (x < 1u) ? 1u : ((x > 16u) ? 16u : x);
    }
    uint32_t GetTextureAnisotropy() const { return m_textureAnisotropy; }

    // ---- Pool watermark telemetry ----
    // Read-only view of the renderer's fixed-capacity pools for the editor
    // panel. Every value is written by BuildRenderScene / SortAndEmit / Render on
    // the main thread and read from OnUIRender on the same thread - plain loads,
    // no synchronisation. Do not call these from a RenderGraph Phase B worker.
    // Naming mirrors the members: *HighWater = peak since start (or since the
    // last ResetPoolWatermarks), *LastFrame = the value of the most recently
    // built frame (a frame built but never rendered still updates it).
    // "Capacity" getters return this renderer's own pool caps, so they are
    // non-static: a second Renderer reports its own numbers.

    // Instance emit budget (the SortAndEmit truncation).
    uint32_t GetInstanceEmitHighWater() const      { return m_instanceEmitHighWater; }
    uint32_t GetInstanceDropHighWater() const      { return m_instanceDropHighWater; }
    uint32_t GetInstanceDropLastFrame() const      { return m_instanceDropLastFrame; }
    uint32_t GetInstanceOverflowFrameCount() const { return m_instanceOverflowFrames; }
    uint32_t GetInstanceCapacity() const           { return m_maxInstances; }

    // ---- Frame statistics (editor Profiler panel) ----
    // The last published emit-half census, folded onto the fields main owns -
    // never the struct the emit half is writing into (see PublishEmitStats /
    // ConsumeEmitStats and the member block beside m_frameStats).
    const FrameStats& GetFrameStats() const { return m_frameStats; }

    // ---- Pool watermark telemetry ----
    // Retained instance slot allocator (static lane) + its dynamic fallback.
    uint32_t GetInstanceSlotHighWater() const      { return m_instSlotHighWater; }
    uint32_t GetInstanceSlotCurrent() const        { return m_instSlotNext; }
    uint32_t GetStaticLaneDeniedLastFrame() const  { return m_instStaticLaneDeniedLastFrame; }
    uint32_t GetInstanceStagingUsedLastFrame() const { return m_instanceStagingUsed; }
    uint32_t GetInstanceUploadRecordsPeak() const  { return m_instUploadRecordsPeak; }
    uint32_t GetStaticInstanceLaneCapacity() const { return m_staticInstanceCap; }
    static constexpr uint32_t GetDynamicInstanceReserve()     { return kDynamicInstanceReserve; }

    // Material slot table + the per-material custom CBV/SRV rings.
    uint32_t GetMaterialSlotHighWater() const      { return m_materialBindingHighWater; }
    uint32_t GetMaterialSlotCurrent() const        { return m_persistentMatSlotNext; }
    uint32_t GetMaterialUploadRecordsPeak() const  { return m_materialCopyRecordsPeak; }
    uint32_t GetCustomMaterialTableHighWater() const { return m_customMaterialTableHighWater; }
    uint32_t GetCustomMaterialCbvHighWater() const   { return m_customMaterialCbvHighWater; }
    uint32_t GetRuntimeMaterialEntityHighWater() const   { return m_runtimeMaterialEntityHighWater; }
    uint32_t GetRuntimeMaterialResourceHighWater() const { return m_runtimeMaterialResourceHighWater; }
    uint32_t GetMaterialSlotCapacity() const       { return m_maxMaterials; }

    // Spot / point shadow slice assignment. Capacity is 0 until the first
    // BuildScene_UploadLights runs (the caps live on the passes, which this
    // header only forward-declares).
    uint32_t GetSpotShadowCasterHighWater() const   { return m_spotShadowCasterHighWater; }
    uint32_t GetSpotShadowCastersDropped() const    { return m_spotShadowCastersDropped; }
    uint32_t GetSpotShadowCasterCapacity() const    { return m_spotShadowCasterCap; }
    uint32_t GetPointShadowCasterHighWater() const  { return m_pointShadowCasterHighWater; }
    uint32_t GetPointShadowCastersDropped() const   { return m_pointShadowCastersDropped; }
    uint32_t GetPointShadowCasterCapacity() const   { return m_pointShadowCasterCap; }

    // Clears every high-water above (not the live current / last-frame values,
    // which the next build overwrites). Nothing resets these on world load, so
    // without this a past heavy scene pins the readings forever.
    void ResetPoolWatermarks();

    // ---- The frame's view state, double-buffered ----
    // The render half's read, after the publish (like FrameViewRead).
    const Settings::Snapshot& SettingsRead() const      { return m_settingsSnap[m_sceneRead]; }
    // The buffer the last hand-off filled - main thread (the harness's frames.jsonl).
    const Settings::Snapshot& SettingsPublished() const { return m_settingsSnap[m_settingsSnapLast]; }

    FrameView&       FrameViewWrite()       { return m_frameViewBuf[m_sceneWrite]; }
    const FrameView& FrameViewWrite() const { return m_frameViewBuf[m_sceneWrite]; }
    // In a secondary view's segment (RHI::ViewScope) the render half reads that view's frame:
    // the main one with its camera, sizes and view (m_viewFrames, made at the segment's start).
    const FrameView& FrameViewRead()  const
    {
        const uint32_t slot = RHI::CurrentRecordingView().slot;
        return slot != 0 && slot < RHI::kMaxViews ? m_viewFrames[slot] : m_frameViewBuf[m_sceneRead];
    }
    // The frame in flight's view, for the emit path only. Identical to
    // FrameViewWrite() until AdvanceSceneWrite runs, and deliberately a separate
    // accessor: after the hand-off FrameViewWrite() is the buffer SetCamera(F+1)
    // and BuildGameView(F+1) write into, and BuildGameView overwrites vpWidth /
    // vpHeight and restores them - a torn read the emit path must not see.
    FrameView&       FrameViewEmit()        { return m_frameViewBuf[m_emitIdx]; }
    const FrameView& FrameViewEmit()  const { return m_frameViewBuf[m_emitIdx]; }

    // ---- What the FramePacket carries for the emit lane ----
    // Read on main at hand-off, echoed back by whoever finishes the frame.
    // Getters rather than packet-only state because a main-thread emit consumes
    // them on the thread that produced them.
    uint32_t GetEmitIdx()        const { return m_emitIdx; }
    uint32_t GetFrameUploadSlot() const { return m_frameUploadSlot; }
    uint32_t GetBatchPlanGen()   const { return m_batchPlanGen; }
    uint32_t GetSortIdxGen()     const { return m_sortIdxGen; }

    // ---- The emit lease, checked ----
    // Returns a bit mask of the lease fields the caller's packet disagrees with;
    // 0 = the packet and the emit path describe the same frame. A predicate, not
    // a check: the ENGINE_CHECK belongs at the consume point
    // (FramePipeline::VerifyEmitLease), and the harness self-test hands it a
    // deliberately corrupted packet and checks that it says no.
    uint32_t EmitLeaseMismatch(uint32_t uploadSlot, uint32_t emitIdx,
                               uint32_t batchPlanGen, uint32_t sortGen) const;

    // ---- -surfacepagesearly, asked from outside the renderer ----
    // The predicate (-surfacepagesearly; -surfacepagesearlyoff is the escape) is
    // defined beside ResolveBatchPlan in Renderer_EmitPlan.cpp and latched on
    // first call. Public because FramePipeline's [FRAME_EXECUTOR] summary names
    // the run's shape, and a second copy of the predicate there could disagree
    // with the one that chooses the path.
    static bool SurfacePagesEarlyActive();
    // The hoist's effective default: kSurfacePagesEarlyDefault OR the lane's
    // implication. "default=" compares against this, not the constant alone;
    // otherwise every default Game run would print default=0 while running the
    // shipped shape, and cpu_profile.py groups captures on that field.
    static bool SurfacePagesEarlyDefaultActive();

    // ---- -brslane: who runs the emit half ----
    // Wanted is the command line's answer; enabled is what survived the refusal
    // ladder in FramePipeline::Configure (editor host with -editoruilate / no
    // lane / -brssplit / no -animoverlap / -surfacepagesearlyoff /
    // -framepreparelegacy / the two byte-oracles that dereference main-owned
    // state). The renderer is told rather than asking: those terms are
    // FramePipeline's to evaluate, and a second copy here could disagree.
    // -brslane implies -surfacepagesearly (see SurfacePagesEarlyActive): the
    // lane-side nest must not touch the instance-slot allocator's material-slot
    // twin or the surface cache, and the hoist moves both onto main.
    // The default is host-aware: kBrsLaneDefault && graph mode && the lane
    // started && the -animoverlap barrier moved, and on the editor host only
    // while it builds its UI early (not -editoruilate). -renderowner's default,
    // by contrast, is every graph-mode host. FramePipeline::Configure owns every
    // term but the constant and calls SetBrsLaneHostDefault once, before anything
    // here asks; a bare constant would make every boot without the lane (serial,
    // -renderowneroff, -animoverlapoff, an -editoruilate editor) log a [BRSLANE]
    // refusal for a lane it never asked for. An explicit -brslane still gets the
    // refusal line and its reason (the brslane_refused* packs pin it).
    static void SetBrsLaneHostDefault(bool laneIsDefaultHere);
    static bool BrsLaneDefaultActive();
    static bool BrsLaneWanted();
    // The two byte-oracles -brslane refuses under, asked rather than re-derived:
    // both are environment variables read once in this library, and a second
    // copy of either in FramePipeline would be one edit from disagreeing with the
    // code that actually arms them.
    static bool MatSnapOracleArmed();    // DX12_VERIFY_MATSNAP
    static bool MatBufChecksumArmed();   // DX12_MATBUF_CHECKSUM
    void SetBrsLaneEnabled(bool on) { m_brsLaneEnabled = on; }
    // -brsplanlane: the emit half's plan (BuildScene_PlanEmit - the sort, the
    // preamble, ResolveBatchPlan) moves to the lane with the nest. Needs the lane
    // emit; FramePipeline's ladder refuses it otherwise. See PrepareLanePlan for
    // what main still does itself.
    void SetBrsPlanLane(bool on) { m_brsPlanLane = on; }
    bool BrsPlanLane() const     { return m_brsPlanLane; }
    // Does the lane owe this frame's emit half? Read by FramePipeline when it
    // builds the FramePacket, so the lane's PrepareFrameOnLane is gated on what
    // this frame decided rather than on the run-wide flag: a frame whose batch
    // plan was refused has already emitted on main and the lane must go straight
    // to RenderAsync. False on every non--brslane frame.
    bool LaneEmitOwed() const { return m_laneEmitOwed; }
    // Frames that took that fallback, for the [SURFACEPAGES] read-back.
    uint64_t EmitLaneFallbackFrames() const { return m_emitLaneFallbackFrames; }
    // Is the emit-half byte oracle armed (SetBrsLaneVerify)? The
    // [BRSLANE_PREPARE] line is gated on this, not on
    // Sys::CommandLine::Has("-brslaneverify"), so the gate and the instrument
    // cannot disagree: SetBrsLaneVerify is WITH_HARNESS-only, so on a Retail
    // build the flag parses but nothing arms.
    bool BrsLaneVerifyArmed() const { return m_brsLaneVerifyEvery > 0; }
    // -brslaneverify=N. Set from FramePipeline::Configure beside the ladder, and
    // deliberately not gated on the ladder's answer: the oracle has to print in
    // the arm the comparison is against.
    void SetBrsLaneVerify(int every, int selfTestFrame = -1)
    { m_brsLaneVerifyEvery = every; m_brsLaneSelfTestFrame = selfTestFrame; }
    // What the digest cost on the frame just finished, then zero. Reported next
    // to prepareMs so the gate's number is prepareMs - oracleMs and the reader
    // does not need to know which frames were sampled.
    double TakeBrsLaneOracleMs()
    { const double ms = m_brsLaneOracleMs; m_brsLaneOracleMs = 0.0; return ms; }
    // The whole lane-side half of the frame, called by
    // FramePipeline::RenderOwnerLoop before RenderHalfAsync, on the lane's own
    // thread:
    //   FrameTaskContext(queue)  - the context must be created here; its
    //                              CheckOwner throws for any other thread
    //   BuildScene_EmitBatches   - the nest + tail over the handed-off EmitContext
    //   FinishFramePreparation   - seal + PublishSceneRead + skin read half
    // `queue` is the lane's own OwnerTaskQueue, and this runs before its RunUntil
    // starts, so nothing here is a recursive pump.
    void PrepareFrameOnLane(Sys::OwnerTaskQueue& queue, uint64_t frame,
                            uint32_t uploadSlot, uint32_t emitIdx);

    // ---- Stable instance slots + one DEFAULT instance buffer (retained) ----
    std::vector<InstanceCopyRange>& InstCopyWrite() { return m_instCopyBuf[m_emitIdx]; }
    std::vector<InstanceCopyRange>& InstCopyRead()  { return m_instCopyBuf[m_sceneRead];  }

    // ---- The frame's emitted command layout, double-buffered ----
    SceneEmit&       SceneEmitWrite()       { return m_sceneEmitBuf[m_emitIdx]; }
    const SceneEmit& SceneEmitWrite() const { return m_sceneEmitBuf[m_emitIdx]; }
    const SceneEmit& SceneEmitRead()  const { return m_sceneEmitBuf[m_sceneRead];  }

    MaterialUpload& MatUploadWrite() { return m_matUploadBuf[m_emitIdx]; }
    MaterialUpload& MatUploadRead()  { return m_matUploadBuf[m_sceneRead];  }

    // ---- Shared runtime material resources ----
    static void RunMaterialCpuWrite(const MaterialCpuWrite& write);

    ResolvedMaterialRuntime ResolveMaterialRuntime(Entity e) const;

    // ---- The one material packer ----
    // Packs one MaterialComponent into one slot of a MaterialGPUData table: the
    // schema fields, the reconciled alphaRef, the sRGB -> linear colour cache,
    // the identity-UV default, the MATERIAL_GPU_FLAGS bitmask, the translucent
    // Unlit -> Standard narrowing, and the reflection-ordered b8 params / space3
    // texture mirror. A member so a preview with no World and no Entity can
    // produce a byte-correct record. Do not hand-pack one: `MaterialGPUData{}` is
    // not a valid record (a zero textureHandleId is a valid bindless index,
    // uvTiling (0,0) collapses every UV to one texel), and the stride keeps
    // changing (quote the sizeof).
    // `buf` may be any caller-owned table - the frame's material staging buffer,
    // or a private preview one. `mc == nullptr` writes the neutral default-PBR
    // record. `slot` is indexed, never bounds-checked: the caller owns the size.
    // Two resolve points, one path: `rt` is a parameter on purpose - this
    // function never resolves it. A material's GPU slot is keyed by the
    // candidate's matHash, which HashMatParams computes from its own
    // ResolvedMaterialRuntime, while the record written into that slot comes from
    // the `rt` handed in here. Both must be the same resolution of the same
    // material: the main path passes ResolveMaterialRuntime(first.entity) at both
    // points, and a preview / by-key caller must feed one runtime to both.
    // Different runtimes give a GPU record permanently inconsistent with its
    // key: a silently wrong texture set that a retained slot never rewrites.
    // `frameRecord`: the slot has a frame record this frame - sets
    // kMatGpuFlagFrameRecord. Only the main emit passes true, for a
    // PER_FRAME_PARAMS material; the preview and the path tracer's table write no
    // records and keep the default.
    static void WriteMaterialSlot(Resource::MaterialGPUData* buf, uint32_t slot,
                           const MaterialComponent* mc,
                           const ResolvedMaterialRuntime& rt,
                           const ShaderReflect::Reflection* customRefl = nullptr,
                           bool frameRecord = false);

    // ---- The one custom-shader binding packer ----
    // Pack a custom-shader material's b8 parameters and space3 textures. The
    // custom-texture lookup takes `rt` rather than calling
    // GetMaterialCustomTextureRuntime(entity, name), which is what lets an
    // entity-free preview use it. `rt` must be the same resolution the caller
    // hashed the material with (as for WriteMaterialSlot).
    void PackCustomMaterialBindings(CustomPackDest& dest,
                                    const MaterialComponent* mat,
                                    const ResolvedMaterialRuntime& rt,
                                    const ShaderReflect::Reflection* customRefl,
                                    uint32_t customVSID,
                                    bool isCustomShader,
                                    std::vector<MaterialCpuWrite>* deferredWrites = nullptr);

    // ---- Entity -> persistent material slot ----
    // Frame-stamped entity -> persistent material slot, recorded by
    // BuildScene_SortAndEmit for every instance it emits. Lets consumers that
    // shade outside the packet path (DDGISceneAS's RT material proxy -> NPR hit
    // shading) reach the same MaterialGPUData record the raster used this frame.
    // Returns m_maxMaterials when the entity emitted nothing this epoch (culled
    // static, hidden, or the slot map overflowed).
    uint32_t GetEntityMaterialSlot(Entity e) const
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id >= m_entityMatSlot.size() ||
            m_entityMatSlotEpoch[id] != m_materialFrameEpoch)
            return m_maxMaterials;
        return m_entityMatSlot[id];
    }

    // The write half of GetEntityMaterialSlot: stamps entity -> persistent
    // material slot for this material epoch. One producer per frame, chosen by
    // EmitContext::planStampedMatSlots:
    //   planActive (the -surfacepagesearly / -brslane hoist): ResolveBatchPlan's
    //     pass B, on main, right after AcquirePersistentMatSlot resolved the
    //     row, so the map is complete before DDGISceneAS::Gather reads it,
    //     whichever thread runs the emit nest afterwards. Under -brslane the nest
    //     runs on the render-owner lane and has not run when Gather does;
    //     stamping there would make the epoch gate above miss for every entity,
    //     and every Toon / Unlit RT / DDGI material proxy would silently lose its
    //     toon knobs and exposure protection (pack ddgi_proxy_slot_lane).
    //   otherwise: the emit nest's emitInstance (no hoist, or the per-frame plan
    //     refusal) is the only writer.
    // The two agree by construction: under planActive the nest reads thisMatIdx /
    // slotValid from the same BatchPlanRow this stamp is taken from. That the
    // plan covers exactly the batches the nest emits is asserted by the plan's
    // tdrSkip prediction ([SURFACEPAGES_ORACLE] part 3) and the nest's row
    // cross-walk (row.begin / row.instBegin), and reported as `stamped=` vs
    // `nestInst=` on the [SURFACEPAGES] line.
    // -matslotstampoff (WITH_HARNESS) forces the nest back into the producer role
    // with the hoist on: the escape hatch, and the positive control that
    // reproduces the defect.
    void StampEntityMaterialSlot(Entity e, uint32_t matIdx, bool slotValid)
    {
        const uint32_t id = static_cast<uint32_t>(e);
        if (id >= m_entityMatSlot.size())
        {
            m_entityMatSlot.resize(id + 1, m_maxMaterials);
            m_entityMatSlotEpoch.resize(id + 1, 0);
        }
        m_entityMatSlot[id]      = slotValid ? matIdx : m_maxMaterials;
        m_entityMatSlotEpoch[id] = m_materialFrameEpoch;
    }

    // ---- By-key material runtime ----
    // Every other way to reach a material's texture handles starts from an Entity
    // and looks it up in m_entityMaterialRuntimeBindings, so a MaterialComponent
    // that belongs to no entity (the asset-mode inspector's edit buffer, the
    // preview, a thumbnail) would resolve every slot to bindlessIndex == -1.
    // m_runtimeMaterialResources is content-addressed (BuildMaterialRuntimeKey
    // over the normalised shader paths, engine texture paths and sorted custom
    // bindings); this is its entrance - an acquire that refcounts a resource
    // without binding it to an entity, and a read that starts from the key.
    // Six rules, all of them correctness:
    //  1. Acquire / Release are game half only. The render half holds raw
    //     pointers into these two maps (WriteMaterialSlot, DDGISceneAS's
    //     material proxy), and the maps may change only inside the
    //     BuildScene_SyncMaterials window. Both entry points check ThreadCtx.
    //  2. Never cache the returned resource across frames: the last world entity
    //     using the material can change material at any frame boundary, dropping
    //     the refcount to zero and releasing the TextureSystem handles the
    //     preview samples. Re-resolve by key every frame (one hash lookup).
    //  3. OnWorldClear wipes the whole table without looking at refcounts and
    //     defers every texture for release, and the preview outlives Load, New,
    //     ActivateScene and editor Play -> Stop. So the preview must re-acquire
    //     when a by-key lookup misses (the self-heal in
    //     Renderer::ExtractPreviewRequests).
    //  4. Both resolve points use one runtime: the slot key comes from
    //     HashMaterialParams(mat, rt) and the record from
    //     WriteMaterialSlot(..., rt); two different runtimes make the GPU record
    //     silently inconsistent with its key.
    //  5. No invented entity ids: nothing here touches m_matBoundBits or
    //     m_entityMatSlot, which size arrays from a raw id.
    //  6. A key collision shows the wrong textures; it cannot corrupt memory.

    // The content hash that names a material's runtime resource. Pure function
    // of the component - safe from any thread.
    uint64_t BuildPreviewMaterialRuntimeKey(const MaterialComponent& mc) const;

    // Create-or-refcount the runtime resource for @p mc and return its key.
    // Acquires a TextureSystem handle for every engine slot and every custom
    // texture, as SyncMaterialTextures does for an entity, and shares the
    // resource with any world entity using the same material, so previewing a
    // material already on screen costs no extra upload. Game half only (rule 1).
    // Returns 0 when the renderer has no texture system yet.
    uint64_t AcquirePreviewMaterialRuntime(const MaterialComponent& mc);

    // Drop one reference taken by AcquirePreviewMaterialRuntime. Game half only.
    // Releasing a key that is not held is a no-op.
    void     ReleasePreviewMaterialRuntime(uint64_t key);

    // Resolve a key to the pair WriteMaterialSlot wants. `binding` is left null
    // on purpose: it is the per-entity override layer, and a preview has no
    // entity. EngineTex(slot) falls through to the shared resource when binding
    // is null and CustomTex(name) never reads binding, so both the engine slots
    // and the custom shader's space3 textures resolve from a key alone.
    // Read-only, but not safe on either half: the game half mutates the map, and
    // rehashing invalidates every pointer this hands out. A Render-context read is
    // legal only where no game-half writer can overlap it, as in the editor's UI
    // build (on main, the lane idle; see the threading block in
    // PreviewRenderer.h); a game-side PiP must snapshot what it needs on the game
    // half instead. Returns an empty runtime when the key is unknown - what the
    // OnWorldClear self-heal detects.
    ResolvedMaterialRuntime ResolvePreviewMaterialRuntime(uint64_t key) const;

    // True when @p key names a live runtime resource. The miss is the
    // OnWorldClear signal (rule 3), but only half of it - see
    // MaterialRuntimeEpoch(). Same render-half restriction as
    // ResolvePreviewMaterialRuntime.
    bool     HasPreviewMaterialRuntime(uint64_t key) const;

    // Bumped by OnWorldClear, which wipes m_runtimeMaterialResources without
    // looking at refcounts and defers every texture in it for release.
    // The missing-key test alone is not a sufficient self-heal: OnWorldClear runs
    // before the next BuildScene_SyncMaterials, which recreates entries for the
    // new world, so a holder that checks `Has(key)` after that sync sees true for
    // a key the new world also uses and keeps a reference the clear already
    // dropped. Its next Release then erases an entry whose refCount is 1 but
    // whose real holders are live scene entities, handing every TextureSystem
    // handle back underneath them (SyncMaterialTextures will not rebuild them:
    // it early-outs on an unchanged key) - a permanently white object and an
    // unbalanced release, with nothing in the log. So a holder stores this value
    // beside its key and, on a mismatch, forgets the key without releasing it,
    // then re-acquires.
    uint32_t MaterialRuntimeEpoch() const { return m_matRuntimeEpoch; }

    // True when every texture this runtime references has finished streaming.
    // False does not block the draw - the preview renders with the engine's
    // white / flat-normal fallbacks and re-renders when this flips - but the UI
    // says so, and the harness waits for it before asserting on pixels.
    bool     IsPreviewMaterialRuntimeReady(uint64_t key) const;

    // The two texture reads keyed by material instead of by entity, for the
    // preview and the asset-mode inspector's texture-slot thumbnails.
    MaterialRuntimeTextureView GetPreviewEngineTextureRuntime(
        uint64_t key, uint32_t slot) const;
    MaterialRuntimeTextureView GetPreviewCustomTextureRuntime(
        uint64_t key, std::string_view bindingName) const;

    // The material-parameter hash BuildScene keys GPU material slots with: the
    // one hash over exactly the GPU-observable field set, derived from the
    // MATERIAL_GPU_FLAGS X-macro so a new flag cannot be forgotten. This, not
    // SetDirty (consumed and cleared every frame) and not IsAssetDirty (a level,
    // not an edge), is the right "would the picture differ" key.
    uint64_t HashMaterialParams(const MaterialComponent& mc,
                                const ResolvedMaterialRuntime& rt) const;

    // Custom shader ids for a material with no entity. Game half only - this
    // registers, and registration must reach all nine libraries through the one
    // fan-out, on one thread. Returns 0 for a material with no custom shader for
    // that stage.
    uint32_t ResolvePreviewCustomPSID(const MaterialComponent& mc);
    uint32_t ResolvePreviewCustomVSID(const MaterialComponent& mc);
    // Does a masked custom material's shadow run its own hook
    // (DrawPacket::shadowHookPS, RenderGraph/RenderPass/ShadowCasterShaders.h)?
    // False under -shadow_hook_off, the reverse arm: Shadow.ps. One answer for
    // the game's packets (BuildScene_EmitBatches) and the preview's
    // (PreviewRenderer::BuildOnePacket).
    bool ShadowHookEnabled() const { return m_shadowHookEnabled; }
    // Reflection for a dynamic shader id - pixel or vertex, they share one id
    // space and one registry - from the GBuffer library the preview borrows. A
    // caller that has both should mirror the main packer's fallback
    // `packRefl = reflPS ? reflPS : reflVS`: DXC strips each stage's reflection
    // to the members that stage uses, so a VS-only material's b8 layout exists
    // only in the vertex reflection.
    // Read-only, and it reads the library's dynamic reflection vector, which the
    // game half's RegisterDynamic reallocates - so a Render-context caller must
    // not overlap it, as in the editor's UI build (on main, the lane idle).
    // With @p mc: the reflection that material is synced and packed from
    // (MaterialShaderReflection); without, permutation 0.
    const ShaderReflect::Reflection* LookupCustomShaderReflection(uint32_t shaderID,
                                                                  const MaterialComponent* mc = nullptr) const;

    // ---- Shared environment handles the preview borrows ----
    // The engine-wide 1x1 black TextureCube. Renderer::SyncSkyboxIBL substitutes
    // it whenever a real IBL cube handle is 0, so deferred and forward shaders
    // never sample an unbound t6 / t7 - but that substitution lives here, not in
    // LightingPass, so any path that drives a LightingPass instance directly must
    // apply it itself.
    uint64_t GetFallbackSkyCubeSrv() const { return m_fallbackSkyCubeSrv; }
    // engine/ibl/BRDF_LUT.itex, or 0 until it is Ready. t8 has no fallback
    // anywhere and the indirect block in Lighting.ps does not gate on the LUT, so
    // a consumer must keep the indirect path off while this is 0.
    uint64_t GetBRDFLUTSrv() const { return m_brdfLutSrv; }

    // The shared half of the mesh-override resolve: load the library (cached
    // under one normalised spelling), register the mesh-descriptor slot, and
    // hand back the entry's raw AABB. ResolvePreviewMesh wraps it with the
    // single-mesh fit; ResolvePreviewAsset merges the raw boxes in composed space
    // and fits once, which is why this is separate.
    // A failed load is not cached: the same `.meshlib` reached while an importer
    // is mid-rewrite comes back invalid, and memoising that would turn a
    // transient miss into a permanent no-preview latch. The failure is logged
    // once per path and sets the retry flag the composer reads. Game half only.
    bool     ResolvePreviewMeshEntry(const std::string& meshlibPath, uint32_t meshId,
                                     uint32_t& outSlot, uint32_t& outIndexCount,
                                     float outAabbMin[3], float outAabbMax[3]);
    // The memoised composition of one asset, parsed on first ask. An unparseable
    // asset yields an empty composition that is cached, so a bad path is read
    // once, not once per frame.
    // Null means ask again: a `.meshlib` (or a MeshAsset's library) that could not
    // be opened, the one routinely transient failure. Nothing is memoised on that
    // path; NotePreviewAssetLoadRetry arms this asset's own retry term, so its
    // extract stamp moves every kPreviewAssetRetryFrames frames (the memo epoch is
    // not bumped: that would de-idle every other asset target). Game half only.
    // @p outLibraryOpened, when given, is set true on exactly the call that
    // opened the asset's own `.meshlib` and found it good - the inverse of the
    // null branch that arms the retry, and the only thing that lets a caller
    // disarm it. It stays false for a prefab (whose compose reads no library, so
    // a failing part library is visible only to a resolve) and on a memo hit
    // (nothing was opened).
    const Preview::AssetComposition* ComposePreviewAsset(const std::string& assetPath,
                                                         Preview::AssetKind kind,
                                                         bool* outLibraryOpened = nullptr);

    // MeshAssetRegistry::BuildModel over preview library references
    // (m_previewMeshLibs): the model's Meshes, sections and default materials
    // exactly as a StaticMesh of it resolves them, without making any library a
    // world library. What a MeshAsset composition and the Mesh Editor read.
    // False = a library would not open (transient: ask again later); @p view is
    // filled either way. Game half.
    bool BuildPreviewModelView(const std::string& model, MeshAssetRegistry::Model& view);

    // ---- The frame's emitted command layout, double-buffered ----
    std::vector<InstAabb>&       InstAabbWrite()       { return m_instAabbBuf[m_emitIdx]; }
    const std::vector<InstAabb>& InstAabbRead()  const { return m_instAabbBuf[m_sceneRead];  }

    // ---- Retained-scene dirty feed ----
    // Read-only view of this frame's dirty feed for retained consumers
    // (DDGISceneAS). Valid between BuildRenderScene and the next frame.
    SceneDiffView GetSceneDiffView() const
    {
        SceneDiffView v;
        v.valid      = m_retainedSceneDiff;
        v.frameStamp = m_sceneStore.frameStamp;
        v.dirtyXform = &m_sceneDirtyXform;
        v.dirtyOther = &m_sceneDirtyOther;
        v.added      = &m_sceneAdded;
        v.removed    = &m_sceneRemoved;
        v.matDirty   = &m_sceneMatDirty;
        v.matTexUnsettled = (m_matTexUnsettled != 0);
        v.addsSkinnedOnly = m_addsSkinnedOnly;
        v.addsOwnDirt     = m_addsOtherOwn && m_addsMatOwn;
        v.texUnsettledEntities = m_matTexUnsettled != 0 ? &m_matTexUnsettledEntities : nullptr;
        return v;
    }

private:
    // Number of phase buckets Renderer::Render times itself into.
    static constexpr int kRenderZoneCount = 12;
    static constexpr int kBuildZoneCount  = 14;   // [BUILDZONE], BuildRenderScene (see the slot map in Renderer_Scene.cpp)
    static constexpr int kSortEmitZoneCount = 10; // [EMITZONE] (slot map in Renderer_EmitPlan.cpp)

    // ---- FrameStats: one writer per field, and a publish across the seam ----
    struct EmitStatsSlot { FrameStats stats; float emitMs = 0.0f;
                           float planMs = -1.0f; };   // >= 0: the plan ran on the lane

    struct VolFogSceneState
    {
        VolumetricFogPass::SceneFog                      fog;
        std::vector<VolumetricFogPass::LocalFogVolumeGPU> locals;   // culled, emissive first
        uint32_t                                         emissiveCount = 0;
        // The participating medium's local volumes (UploadMediumSet): the nearest
        // Media::kMaxLocals at any distance - a path reaches past the froxel range.
        std::vector<VolumetricFogPass::LocalFogVolumeGPU> mediumLocals;
    };

    // ---- Per-frame light build state (Phase 0 -> Phase 4) ----
    using ResolvedLight = ResolvedSceneLight;

    static constexpr uint8_t kFogDemandOwned   = 1u << 0;
    static constexpr uint8_t kFogDemandEnabled = 1u << 1;
    static constexpr uint8_t kFogDemandGodRays = 1u << 2;
    static constexpr uint8_t kFogDemandLocal   = 1u << 3;

    // ---- Per-frame upload buffers (UPLOAD heap, persistently mapped) ----
    // Compile-time upper bound on m_maxInstances, checked in the constructor,
    // for the two uses that cannot be runtime-sized:
    //   1. m_instanceSlotToEntity is a fixed inline array, read by the picking
    //      resolve and written per emitted instance in SortAndEmit; inline keeps
    //      that path free of an indirection.
    //   2. The cmd-group meta packs two command indices into bits 16..30 and
    //      0..15 (bit 31 marks a meshlet group), a static_assert on this bound.
    // 32768 == RendererPoolCaps{}.maxInstances and the packing's ceiling
    // (1 << 15): raising it means repacking the meta first.
    static constexpr uint32_t kMaxInstancesUpperBound = 32768;
    // Upper bound on m_maxMaterials. Not a compile-time context anywhere, but
    // two GPU contracts cap it: GBuffer normal.a carries matIdx as an exact
    // 16-bit UNORM integer, and DDGISceneAS packs (slot + 1) into rtFlags bits
    // 16..31. Both are 16-bit, so slot + 1 must stay under 65536.
    static constexpr uint32_t kMaxMaterialsUpperBound = 65534;
    static constexpr uint32_t kFrameSlots   = RHI::kFramesInFlight;     // matches GraphicsDX12::kFrameCount

    // ---- Meshlet path ----
    struct MeshletShadowBanks { MeshletShared::MeshletCullCB cascade[3]; };

    struct MeshletLodView
    {
        float    tau = 0.0f;
        float    camera[3] = { 0, 0, 0 };
        float    lodScale = 1.0f;
        uint32_t ortho = 0;
        float    zNear = 0.1f;
    };

    // ---- Stable instance slots + one DEFAULT instance buffer (retained) ----
    // Static slots [0, m_staticInstanceCap); per-frame dynamic segment
    // [m_staticInstanceCap, m_maxInstances). Static-alloc overflow degrades the
    // entity to the dynamic segment for the frame (never fails the draw).
    static constexpr uint32_t kDynamicInstanceReserve = 8192;

    static constexpr uint32_t kInstSlotReuseLagFrames = 4; // > frames-in-flight (3)

    // ---- Shadow-family retained args + per-view GPU cull ----
    struct SpotCasterBox { DirectX::XMFLOAT3 min{}, max{}; bool unknown = false; };
    struct SpotCasterState {
        SpotCasterBox previous{}, current{};
        SpotCasterBox previousStatic{}, currentStatic{};
        uint64_t seen = 0, dirty = 0;
        bool active = false, animated = false, previousAnimated = false;
        bool hasStatic = false, previousHasStatic = false;
    };

    // ---- Terrain layer table, material-slot churn window ----
    static constexpr uint32_t kMaxTerrainLayers = 8;

    static constexpr uint32_t kMatSlotWindowFrames = 120;

    // ---- Post-cull survivor readback (agent harness) ----
    static constexpr uint32_t kDrawCountReadbackCap = 4096;   // u32s, 16 KB

    // ---- Post-cull visible-geometry counters (InstanceCull u3) ----
    static constexpr uint32_t  kVisStatsElems = 4;

    // ---- Shared runtime material resources ----
    struct RuntimeMaterialShaderState {
        int customPSID = -1;
        int customVSID = -1;
    };

    struct alignas(64) FrameMaterialOutputBlock { uint8_t bytes[64]; };

    // ---- The one custom-shader binding packer ----
    // -matpack_dump (harness): one [MATPACK] log line per distinct
    // custom-material pack - the b8 cbuffer member by member (value and where it
    // came from: the bag, a frame value or nothing), the space3 table slot by
    // slot, the g_Materials mirror, and the bag after each Sync - so two builds'
    // material bindings compare as sets of lines. Set once, in the constructor;
    // false in Retail. Static because two of its readers are static members
    // (WriteMaterialSlot, RunMaterialCpuWrite); a data member, not a
    // function-local static, so the per-batch reads carry no init guard.
    static inline bool s_matPackDump = false;

    // ---- Retained (persistent) material GPU slots ----
    struct MaterialBindingKey {
        uint64_t paramsHash    = 0;
        uint64_t texBaseColor  = 0;
        uint64_t texSurfaceMap = 0;
        uint64_t texNormalMap  = 0;
        uint32_t customPSID    = 0;
        uint32_t customVSID    = 0;
        bool     hasMaterial   = false;
        uint64_t identity      = 0;
        bool operator==(const MaterialBindingKey&) const = default;
    };
    struct MaterialBindingKeyHash {
        size_t operator()(const MaterialBindingKey& key) const noexcept;
    };
    struct PersistentMatSlot {
        uint32_t slot          = 0;
        uint8_t  frameMask     = 0;  // bit f set once frameSlot f's buffer holds this content
        uint64_t lastSeenEpoch = 0;  // == m_materialFrameEpoch on the batch's first sight this frame
    };

    // ---- The hoisted batch plan (-surfacepagesearly) ----
    struct BatchPlanRow
    {
        uint32_t begin       = 0;   // first sorted ordinal of the batch
        uint32_t end         = 0;   // one past the last (== the nest's `j`)
        // The instance ordinal the plan predicts the nest holds when it reaches this
        // batch (the nest's `instIdx` at batch start). A second counter, advanced by
        // the plan's own walk, so comparing it with the nest's is a cross-walk check
        // rather than a restatement of `begin`: the two agree only while both loops
        // truncate at the same m_maxInstances boundary and neither has an early-out
        // that advances one without the other.
        uint32_t instBegin   = 0;
        uint32_t matIdx      = 0;   // AcquirePersistentMatSlot result
        uint32_t page        = 0xFFFFFFFFu;  // diagnostics/oracle only
        uint64_t contentHash = 0;            // oracle only
        uint8_t  firstSight     = 0;
        uint8_t  needsBaseWrite = 0;
        uint8_t  slotValid      = 1;
        uint8_t  hasDissolve    = 0;
        uint8_t  tdrSkip        = 0;   // the packet's bad-count guard fires
        uint8_t  pageGate       = 0;   // the surface-cache gate passed
    };

    // ---- The sort -> nest hand-off buffers ----
    struct CandidateEmitPlan {
        uint32_t staticSlot = ~0u;
        bool staticEligible = false, hasDissolve = false, startsBatch = false;
    };

    // ---- The PlanEmit -> EmitBatches contract ----
    struct EmitContext
    {
        // ---- Latched decisions: PlanEmit is the only writer ----
        bool earlyPlan            = false;
        bool planActive           = false;   // earlyPlan && the plan was not refused
        bool verifySurfacePages   = false;
        bool parallelPlan         = false;
        bool verifyPlan           = false;
        bool overlapPlan          = false;
        bool prefixPlan           = false;
        bool verifyPrefixPlan     = false;
        bool widePrefix           = false;
        bool deferMaterial        = false;
        bool verifyMaterialWrites = false;
        bool framePreparation     = false;
        bool earlyMaterial        = false;
        bool overlapMaterialTail  = false;
        bool parallelEmit         = false;
        // ResolveBatchPlan stamped m_entityMatSlot for this frame, so the emit nest
        // must not - one producer per frame, see StampEntityMaterialSlot.
        // == planActive && !-matslotstampoff.
        bool planStampedMatSlots  = false;
        // -brslane asked for this frame's emit half and the batch plan was refused
        // for it, so PlanEmit took it back: m_emitDeferred was cleared, the nest runs
        // on main, and both allocators plus the surface-cache publish epoch stay
        // main's. Recorded rather than re-derived because by the time EmitBatches
        // runs m_emitDeferred is false for two different reasons (no flag, or this),
        // and only this one is worth a log line.
        bool laneFallback         = false;
        bool reuseEmitMemory      = true;
        bool retainInst           = false;
        bool persistentCustom     = false;

        // ---- The frame's lease ----
        // All four derive from one upload slot, latched at the head of BeginFrame,
        // so they cannot be half a frame apart.
        uint32_t                   frameSlot   = 0;
        GPUInstanceData*           instances   = nullptr;
        Resource::MaterialGPUData* matBuf      = nullptr;
        uint32_t*                  remapMapped = nullptr;

        // ---- The sorted array and the parallel candidate plan ----
        const uint32_t*    sortedCandidates = nullptr;   // == m_sortIdx.data()
        CandidateEmitPlan* emitPlan         = nullptr;   // == m_planScratch.data()
        uint32_t           planCount        = 0;

        // ---- EnsurePlanPrefix's whole state ----
        // The one lambda driven from both halves (pass A of ResolveBatchPlan, then
        // the nest) off one cursor, so the cursor lives here and the consumer is a
        // member function rather than two copies of a lambda.
        std::array<Sys::TaskHandle, 16> planHandles{};
        std::array<uint32_t, 17>        planBounds{};
        uint32_t activePlanLanes = 0;
        uint32_t readyPlanEnd    = 0;
        uint32_t nextPlanLane    = 0;

        // ---- Arming stamps ----
        // A context field that a consumer reads before the producer writes it is
        // this struct's silent failure mode: the field holds the previous frame's
        // value, nothing crashes and nothing warns (pass A of ResolveBatchPlan runs
        // well before the seam, so an unarmed prefix waiter returns at once and pass
        // A reads emitPlan rows the plan lanes are still writing; only the [MATBUF]
        // checksum shows it). So each group of fields carries the frame it was
        // written for, and its consumer compares: prefixArmedEpoch covers the
        // plan-prefix group (published early, before pass A); armedEpoch covers the
        // whole context and is the last statement of the seam block, so EmitBatches
        // can tell "PlanEmit ran for this frame" from "for the last one". ~0ull =
        // never armed, a value m_materialFrameEpoch never takes.
        uint64_t prefixArmedEpoch = ~0ull;
        uint64_t armedEpoch       = ~0ull;

        // ---- The three scopes ----
        // Destructor == Join, so EmitBatches' seam guard resets them in this order
        // (not declaration order): planScope, materialTailScope, earlyMaterialScope,
        // then the [EMITZONE] report.
        std::optional<Sys::CpuTaskScope> planScope;
        std::optional<Sys::CpuTaskScope> earlyMaterialScope;
        std::optional<Sys::CpuTaskScope> materialTailScope;

        // ---- Accumulators ----
        // PlanEmit initialises, the nest writes, the tail reads. Bound by reference
        // inside EmitBatches, so this stays the authoritative copy.
        uint32_t dynCursor                   = 0;
        bool     dynOverflowWarned           = false;
        bool     materialSlotOverflowWarned  = false;
        uint32_t materialBatchCount          = 0;
        uint32_t secondaryPackets            = 0;
        uint32_t argsWritten                 = 0;
        uint32_t secondaryArgsSkipped        = 0;
        uint32_t firstSkippedArg             = 0xFFFFFFFFu;
        uint32_t snapDrivenWrites            = 0;
        uint32_t snapVerifiedWrites          = 0;
        uint32_t reusedMaterialBindings      = 0;
        uint32_t planVerifiedBatches         = 0;
        uint32_t planCachedSlots             = 0;
        // Instances ResolveBatchPlan stamped a material slot for. The nest's
        // own nestInstances counts the instances it emitted, in a different
        // loop in a different function; both land on the [SURFACEPAGES] line.
        uint32_t matSlotStamped              = 0;

        // ---- The emit half's private FrameStats ----
        // An accumulator like the fields above: PlanEmit seeds and zeroes, the nest
        // adds into it, the tail assigns into it, and PublishEmitStats hands it to
        // main. It is why nothing off-main touches m_frameStats (see the member
        // block beside m_frameStats for what it carries). Not bound by reference into
        // a worker: every writer is on the emit half's own thread.
        FrameStats stats;

        // ---- ResolveBatchPlan's own output ----
        // Consumed by the nest's side-effect-free assertion and by the [SURFACEPAGES]
        // lines.
        uint32_t acquiresBeforeNest = 0;
        uint32_t mapsBeforeNest     = 0;
        uint32_t planGatedBatches   = 0;
        uint32_t planMappedEntities = 0;
        uint32_t planEpochBefore    = 0;
        uint32_t planEpochAfter     = 0;
        double   planMs             = 0.0;

        // ---- The [EMITZONE] frame bracket ----
        // EmitZoneMark advances seT and FinishSortEmitZones reads all three.
        std::chrono::steady_clock::time_point seT{};
        std::chrono::steady_clock::time_point seStart{};
        bool seAlternating = false;
        bool sePlanned     = false;
    };

    // ---- Skybox / IBL textures ----
    struct SkyboxTexEntry {
        std::string             path;
        Resource::TextureHandle handle           = Resource::kInvalidTextureHandle;
        uint64_t                lastLoggedHandle = 0;
    };

    // ---- Lazy-loaded misc textures ----
    struct GlyphIconTex { RHI::Texture tex; uint32_t texIdx = 0xFFFFFFFFu; };

    // ---- Reflection-probe bake: extract / record / apply ----
    struct ProbeBakeRequest
    {
        bool              valid     = false;
        Entity            entity    = NullEntity;
        uint32_t          cubeSlice = ~0u;
        DirectX::XMFLOAT3 probePos{};      // the entity: the influence volume's centre
        DirectX::XMFLOAT3 capturePos{};    // + captureOffsetLS: where the cube is rendered from
        DirectX::XMFLOAT3 influenceHalfExtents{};
        EInfluenceShape   influenceShape = EInfluenceShape::Box;
        float             influenceOuterRadius = 0.0f;
        // Every reflection probe that carries a primitive mesh (MeshHandle - the
        // editor's viz ball), excluded from the faces' CullView: a probe never
        // captures a probe. A probe authored onto a real MeshLibRef prop keeps that
        // prop.
        std::vector<Entity> probeMeshes;
    };

    // ---- Material preview layer ----
    struct PreviewMeshLibEntry
    {
        Resource::Handle handle;
        uint64_t         lastUse = 0;   // m_previewMeshLibClock at the last hit
    };

    struct PreviewAssetRetry
    {
        uint64_t firstFailFrame = 0;   // m_previewAssetFrame at the first failure
        uint64_t lastFailFrame  = 0;   // ... and at the most recent one
        // Distinct frames on which this asset failed, not calls: one full resolve
        // of a missing `.meshlib` reaches NotePreviewAssetLoadRetry twice
        // (ComposePreviewAsset's own branch, then ResolvePreviewAsset's "no
        // composition" exit), so a call count would read double the real retry
        // rate.
        uint32_t attempts       = 0;
    };

    // ~0.5 s at 60 Hz: long enough that a permanently missing asset costs
    // nothing, short enough that an importer finishing a rewrite shows up
    // without the user touching anything. Not a tuning knob - any value giving a
    // few hundred ms behaves the same.
    static constexpr uint64_t kPreviewAssetRetryFrames = 30;

    // The ceiling on that map. Without it every `.meshlib` an asset-browser tile
    // composed would stay pinned for the life of the process - one VB + one IB
    // per library, gigabytes for a project with a few hundred. The trim releases
    // the least recently used entries back to MeshLibrary and hands their
    // mesh-descriptor / bindless slots back through MeshManager::DetachMeshLib,
    // which keeps a recycled library slot from inheriting the evicted one's
    // descriptors.
    // 24 is above the worst case any one asset needs (a prefab names one library
    // per SceneRef - at most 2 in this tree - though Preview::kMaxPreviewParts
    // parts could name up to 64) and far above the two the preview_churn Tokyo
    // scan composes, so the trim is a ceiling, not a policy that runs in normal
    // use.
    static constexpr std::size_t kPreviewMeshLibCap = 24;

    struct PendingMeshLibHandback
    {
        MeshManager::MeshLibHandback slots;
        // The device's "newest possible referencer" stamp. The free waits for
        // IGraphicsDevice::GetRetiredFrame() to reach it.
        uint64_t                     stampFrame  = 0;
        // m_previewAssetFrame when it was queued, so the report can say what the
        // deferral actually was rather than restating the predicate.
        uint64_t                     queuedFrame = 0;
    };

    struct PreviewAssetFit { float centre[3] = { 0.f, 0.f, 0.f }; float scale = 1.f; };

    // ---- The in-game picture-in-picture ----
    struct PipPending
    {
        enum class Action : uint8_t { None, Set, Clear };
        Action   action = Action::None;
        Entity   entity = NullEntity;
        uint32_t w = 0, h = 0, tick = 1;
    };

    // ---- Material snapshots ----
    // A value copy of the MaterialComponent, taken while `mc` is provably valid.
    // Copying the whole component rather than extracting fields is what makes it
    // provable: WriteMaterialSlot reads ~75 members, many through
    // MaterialSchema::FieldPtr (offsetof pointer arithmetic over the component's
    // layout), and a hand-written field-by-field snapshot would fail silently for
    // any field it missed. Running the same function against a copy is
    // byte-identical by construction. MaterialComponent is fully copyable: its
    // only non-trivial members are five std::strings and two unordered_maps.
    struct MatSnapshot
    {
        Sys::SharedSnapshot<MaterialComponent> mat;
        // HashMatScalars of the content last copied whole into `mat`. A volatile
        // (identity-keyed) snapshot refreshes only its customParams values while
        // this still matches the source (RefreshVolatileSnapshot).
        uint64_t scalarHash = 0;
        // The source's customParams, in the source's iteration order, mapped to the
        // key / value nodes inside `mat`. Built after each whole copy: an
        // unordered_map's copy does not keep the source's iteration order, so the
        // fast path walks the source and follows this map. Node pointers stay valid
        // until the next whole copy, which rebuilds it.
        std::vector<std::pair<const std::string*, std::array<float, 4>*>> paramMap;
        bool paramMapValid = false;
        // No ResolvedMaterialRuntime here, on purpose: it is two raw pointers into
        // renderer-owned maps that BuildScene_SyncMaterials rehashes and a world
        // clear empties, so caching it across frames would dangle them (the
        // entity-churn, scene-reload and skinned packs crash). It is resolved at use,
        // from the batch's entity.
    };

    struct MatSnapSlot { uint64_t key = 0; uint32_t idx = 0xFFFFFFFFu; };

    static constexpr size_t kMatSnapMemo = 512;   // power of two
    struct MatSnapMemo
    {
        const MaterialComponent* mc   = nullptr;
        uint64_t                 hash = 0;
        uint32_t                 idx  = 0xFFFFFFFFu;
    };

    using MatSnapshotCopy = std::pair<uint32_t, const MaterialComponent*>;

    // ---- Skeletal animation ----
    struct AnimPinOwner
    {
        const void*                       owner = nullptr;
        const std::unordered_set<Entity>* set   = nullptr;
    };

    // ---- Dissolve (per-entity noise dissolve) ----
    struct DissolveDrawParams
    {
        DirectX::XMFLOAT4 params;    // x=amount y=edgeWidth z=noiseScale w=edgeIntensity
        DirectX::XMFLOAT4 edgeColor; // rgb edge glow, w unused
    };

    // ---- Debug target view (Renderer_DebugTargets.cpp) ----
    struct DebugTargetSlot
    {
        std::uint8_t        source = 0;   // Renderer_DebugTargets.cpp: Source
        std::uint8_t        slice  = 0;   // the array slice it reads (Source::Csm)
        RG::RGTextureHandle handle{};     // its graph texture (Source::Graph)
    };

    // ---- Pass capture (Renderer_PassCapture.cpp) ----
    struct PassCaptureState
    {
        PassCapture::Desc   requested;                 // latched (mutex)
        bool                hasRequest     = false;    // (mutex)
        bool                releasePending = false;    // (mutex)
        PassCapture::Status status;                    // published (mutex)
        PassCapture::Desc   applied;                   // the serial window's from here on
        bool                live  = false;             // applied and not finished (a one-shot: until a result)
        bool                armed = false;             // the graph is armed for this frame
        std::uint32_t       tries = 0;                 // a one-shot's frames without a result
        std::uint32_t       idle  = 0;                 // frame boundaries with nothing asked
        RG::RGTextureHandle texture{};
        RHI::Texture        copy;
        RHI::ResourceState  copyState    = RHI::ResourceState::UNDEFINED;
        RHI::Texture        picture;
        RHI::ResourceState  pictureState = RHI::ResourceState::SHADER_RESOURCE;
        std::uint64_t       seq = 0;
        std::uint64_t       loggedFrame = 0;           // [RG_CAPTURE]: the frame last logged
        std::string         loggedKey;                 // [RG_CAPTURE]: the last result logged
    };

    // ---- Persistent custom-material CBV / SRV store ----
    static constexpr uint64_t kMatSlotReuseLagEpochs = 4; // > frames-in-flight (3)

    // ---- Terrain texture caches ----
    struct TerrainTexSlot {
        std::string             path;
        Resource::TextureHandle handle = Resource::kInvalidTextureHandle;
    };
    struct TerrainLayerSlots {
        TerrainTexSlot albedo;
        TerrainTexSlot normal;
        TerrainTexSlot arm;       // R=AO, G=Roughness, B=Metalness
        TerrainTexSlot disp;      // single-channel height for height-correlated blend
    };
    struct TerrainTexCache {
        TerrainTexSlot                   heightmap;
        TerrainTexSlot                   splatmap;
        std::vector<TerrainLayerSlots>   layers;   // sized to match TerrainComponent::layers
    };

    // ---- Water texture cache (multi-body: two flow-normal slots per body) ----
    struct WaterTexSlots { TerrainTexSlot a; TerrainTexSlot b; };

    // ---- Draw candidates ----
    // DrawCandidate - per-draw staging record built during BuildRenderScene's
    // gather phase, then sorted and emitted into DrawPackets. Must stay trivially
    // copyable: the parallel MeshLibRef gather merges shards via raw
    // std::memcpy.
    struct DrawCandidate
    {
        Entity     entity;
        uint32_t   meshDescSlot;
        uint32_t   indexCount;
        DirectX::XMFLOAT4X4 worldMatrix;
        // The material's content key (HashMatKeys' second output): what matHash
        // hashes minus every resource id, with the textures by path. The
        // non-depth-sorted sort orders by it (then the mesh) before the id fields, so
        // the draw order - and which of two coplanar layers wins GREATER_EQUAL -
        // does not move when a texture is created somewhere else. 0 with no
        // material. Not a batch key: matHash and the texture handles still are. 32
        // bits because it lives in the padding before `mc` (offset 76 - the row
        // stays 248 B).
        uint32_t   orderKey = 0;
        // Gather side only. A raw pointer into ComponentPool storage, and Erase is
        // swap-with-back, so it can dangle or name another entity's material. Valid
        // from the gather that set it until the material snapshot pass, and not read
        // after that: BuildScene_SortAndEmit reads matBits and matSnap instead. Kept
        // because PatchCandidateCache's null-ness flip test detects a material
        // appearing or disappearing on a cached candidate.
        const MaterialComponent* mc;
        // Per-candidate material predicates, resolved once at gather time. `mc` can
        // dangle (see above), and it is read two ways: ~8400 times a frame for these
        // booleans, and once per distinct material binding key (~100-300 a frame)
        // for the GPU record. So the booleans are bits here, and the record comes
        // from a keyed snapshot table indexed from the candidate (matSnap) - a
        // per-candidate snapshot would be the wrong shape by an order of magnitude
        // (26:1 dedup on AIBench).
        uint16_t   matBits = 0;   // see MatBit, at namespace scope
        // 1 when aabbMin / aabbMax below were copied from a WorldAabb. The classify
        // on the render-owner lane reads the box from here instead of the pool, and
        // there "no WorldAabb" (no cull) and "a WorldAabb that happens to be
        // inverted" (frustum-tested) are different answers; the inverted default
        // cannot tell them apart, this can. Sits in the padding before matSnap - no
        // size change.
        uint8_t    hasAabb = 0;
        // Index into Renderer::m_matSnapshots, or ~0u. The GPU record is produced
        // from a value copy of the MaterialComponent taken at gather time, when the
        // pointer is known good, not from the pointer at emit time.
        uint32_t   matSnap = 0xFFFFFFFFu;
        uint64_t       texBaseColor  = 0;
        uint64_t       texSurfaceMap = 0;
        uint64_t       texNormalMap  = 0;
        uint64_t       matHash       = 0;
        DrawFilter     filter        = DrawFilter::Opaque;
        PermutationKey perm          = {};
        // World-space depth-sort centroid; MeshLibRef overrides it with the
        // WorldAabb centre to fix off-pivot meshes.
        DirectX::XMFLOAT3 worldCenter = { 0.0f, 0.0f, 0.0f };
        // World-space bounds, carried so the render half can cull the scene-voxel
        // dispatches without reaching into the World for a WorldAabb.
        // Inverted default (min.x > max.x) = no bounds, do not cull: one encoding
        // for no entity, no WorldAabb component and a degenerate stored box, so the
        // lane needs no validity flag and stays a flat POD array.
        // Only MeshLibRef-derived entities have a WorldAabb (the two
        // AddComponent<WorldAabb> sites are SceneInstanceLoader, on an entity that
        // just got a MeshLibRef, and the editor's clone path, which copies an
        // existing one), so the primitive, billboard and beam gathers leave this at
        // the default.
        DirectX::XMFLOAT3 aabbMin = {  1.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 aabbMax = { -1.0f, 0.0f, 0.0f };
        float          depth              = 0.0f;  // view-space Z (transparent painter's-algorithm sort)
        float          outlinePixels      = 2.0f;  // per-material outline width (Custom packets only)
        bool           screenSpaceOutline = true;  // enable screen-space edge detection
        bool           isPickingOutline   = false; // editor selection (x-ray inverted hull)
        uint8_t        stencilRef         = 1;     // 1=PBR, 2=NPR
        // 1 when det3(worldMatrix) < 0 - a mirrored instance (UE-style scale
        // (-1,1,1), or any odd number of negative scale axes). Such a matrix reverses
        // triangle winding, so the raster front-face convention must flip with it
        // (rs.frontCounterClockwise) or the draw renders inside-out. A batch-key
        // field: one DrawPacket gets one PSO, so mirrored and unmirrored instances of
        // the same mesh + material must never merge. Mirrors the RT path's
        // per-instance handling (DDGISceneAS: det3 < 0 ->
        // TRIANGLE_FRONT_COUNTERCLOCKWISE).
        uint8_t        mirrored           = 0;
        uint8_t        shadowCullMode     = 0;     // ShadowCullMode raw; 0=Default back-cull
        uint8_t        castShadow         = 1;     // 0 = material opted out of ShadowPass
        uint32_t       prevPosElementBase = 0xFFFFFFFFu; // TAA: skinned prev pos (0xFFFFFFFF = static)
        // 1 when this draw's vertices are GPU-skinned. Drives
        // kInstanceFlagNoGeomCull: the mesh slot behind a skinned draw carries the
        // bind-pose bounds (if any), which do not contain the animated pose - a
        // raised arm or a swinging skirt sits outside. Do not infer it from
        // prevPosElementBase: that means "has a valid previous-frame deformation" (a
        // TAA concern) and is 0xFFFFFFFF for a skinned mesh on its first frame,
        // while paused, or whenever the prev-pose ring allocation was skipped.
        uint8_t        skinned            = 0;
        // The meshlet gate's geometric half, read from the mesh slot's sidecar at
        // gather: ceil(clusters over all levels / 32) when the slot has a resident
        // MLET and the draw is neither skinned nor a custom VS, else 0. Sits in the
        // padding after `skinned`. Copied to DrawPacket::meshletBlocks; a function of
        // meshDescSlot, so it never splits a batch that meshDescSlot did not already
        // split.
        uint16_t       meshletBlocks      = 0;
        uint32_t       customPSID         = 0;     // 0 = default GBuffer PS; non-zero = dynamic id
        uint32_t       customVSID         = 0;     // 0 = default GBuffer VS; non-zero = dynamic id
        // Author-intent visibility, copied verbatim into DrawPacket. viewMask
        // defaults to All so emitters that do not read VisibilityComponent (lights,
        // billboards, beams) are not accidentally hidden.
        uint32_t       viewMask           = ViewBit::All;
        bool           renderInMainPass   = true;
        // 1 when this draw belongs to the player character (root or a descendant):
        // PerObjectShadowPass renders only these into the high-res player map.
        uint8_t        perObjectShadowCaster = 0;
        // 1 when this draw belongs to a HairShadowCasterComponent subtree (the
        // bangs) - the Hair variant of PerObjectShadowPass renders only these.
        uint8_t        hairShadowCaster      = 0;
        // 1 when this draw belongs to a FaceOverlayComponent subtree (brows /
        // lashes) - FaceOverlayPass re-draws only these through the hair.
        uint8_t        faceOverlay           = 0;
        // Low 16 bits = alpha UNORM; high 16 bits = maxHairDistance mapped
        // from [0.005, 0.5] metres. Consumed only by FaceOverlayPass.
        uint32_t       faceOverlayParams     = 0;
        // Per-instance dissolve (copied verbatim into GPUInstanceData; all-zero =
        // inactive). Rides the instance record, not the batch key - dissolving and
        // intact instances of the same material still batch together.
        DirectX::XMFLOAT4 dissolveParams     = { 0.0f, 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 dissolveEdgeColor  = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // ---- The secondary-view classify lane ----
    static constexpr uint32_t kSeenByPiP   = 1u;
    static constexpr uint32_t kSeenByProbe = 2u;

    struct InstStageStats
    {
        uint32_t candidates     = 0;   // instances walked
        uint32_t staticExamined = 0;   // took the retained static-lane branch
        uint32_t staticStaged   = 0;   // ...and actually wrote a record
        uint32_t staticSkipped  = 0;   // provably unchanged: shadow never touched
        uint32_t staticFresh    = 0;   // of those, first-ever emit for the slot
        uint32_t dynamicStaged  = 0;   // dynamic lane - always writes
        uint32_t legacyStaged   = 0;   // retained instances off
    };

    struct alignas(64) GatherLane { std::vector<DrawCandidate> v; };

    // ---- Retained-scene dirty feed ----
    struct SceneItemKey
    {
        DirectX::XMFLOAT4X4 world{};          // GlobalTransform.matrix (post-propagate)
        uint64_t meshKey        = 0;          // meshlib: (libIdx<<32)|meshId; primitive: type|kPrimBit
        uint32_t viewMask       = 0;
        uint8_t  visFlags       = 0;          // VisibilityComponent::flags (default when absent)
        uint8_t  inheritedHidden= 0;
        uint8_t  renderLayer    = 0;
        uint8_t  lane           = 0;          // 0 = static, 1 = always-dynamic
        // Structural-hide tag presence - DDGI's structurallyHidden() and the raster
        // gather both branch on these, so a tag add / remove must dirty the key or
        // retained consumers would keep stale membership.
        // bit0 DisabledTag, bit1 EditorOnlyTag, bit2 HiddenInGameTag.
        uint8_t  tagBits        = 0;
        uint8_t  _pad[3]        = {};         // keep sizeof a multiple of 8, zeroed for memcmp
    };
    struct SceneItemStore
    {
        std::vector<SceneItemKey> keys;       // dense by entity id
        std::vector<uint32_t>     seenStamp;  // dense by entity id; == frame stamp when present
        std::vector<uint8_t>      tracked;    // dense by entity id
        uint32_t frameStamp    = 0;
        uint32_t trackedCount  = 0;
        bool     fullInvalidate = true;       // world clear / desc-generation bump
    };

    // ---- Retained draw-candidate cache ----
    struct ClassifyInputs
    {
        DirectX::BoundingFrustum mainFrustum;
        DirectX::XMFLOAT3 cameraPos{ 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 camForward{ 0.0f, 0.0f, 1.0f };
        size_t cullBegin             = 0;
        bool   gpuCulling            = false;
        bool   shadowArgsFrameActive = false;
    };

    // ---- The dynamic rows' patch as jobs ----
    struct DynPatchPools
    {
        const ComponentPool<GlobalTransform>*         gt       = nullptr;
        const ComponentPool<WorldAabb>*               aabb     = nullptr;
        const ComponentPool<MeshSkinnedComponent>*    skinned  = nullptr;
        const ComponentPool<SkinningOutputComponent>* skinOut  = nullptr;
        const ComponentPool<MaterialComponent>*       material = nullptr;
    };
    struct DynPatchLaneResult { uint32_t fail = 0, rehash = 0; };

    // ---- BuildRenderScene static / dynamic split (-brssplit, default off) ----
    struct BrsLaneResult { bool valid = true; uint32_t ptrWrites = 0, bitWrites = 0; };
    static constexpr uint32_t kBrsEarlyLanes = 4;   // same decomposition as the patch lane

    // ---- Retained BVH leaves + retained tag sets ----
    struct TagBoundaryAnc { uint8_t player = 0, hair = 0, face = 0; uint32_t faceParams = 0; };

    // ---- -brslane: the emit half runs on the render-owner lane ----
    enum EmitHalfBit : uint32_t {
        kEmitHalfThread = 1u << 0,
        kEmitHalfIndex  = 1u << 1,
        kEmitHalfEpoch  = 1u << 2,
        kEmitHalfPlan   = 1u << 3,
    };

    // ---- Post-gather phases ----
    struct LightPrepInputs
    {
        FrameView view{};
        uint32_t  uploadSlot = 0;
        uint64_t  frameEpoch = 0;
    };

    struct ProbeSelectEntry { Entity e; uint32_t slot; };

    // ---- The frame's view state, double-buffered ----
    void PublishSettingsSnapshot(uint32_t emitIdx);

    // ---- Core ----
    // What exists and is ready, as of now, for the frame @p view describes.
    // @p renderHalf picks the LightCB slot the sun is read from (Current: the
    // frame being rendered; Upload: the frame the game half is building).
    FramePlan::State ReadPlanState(const FrameView& view, bool renderHalf);
    // The render half's read, once, at its top.
    void CaptureFramePlanState();
    // Evaluates the plan with what this render half knows at @p stage.
    void PlanStage(FramePlan::Stage stage);
    // The game half's stage A - DDGI and the TLAS demand - for the scene-AS
    // gather, from the same intent the render half's build reads.
    FramePlan::Decisions PlanGameStageA();

    FramePlan::Decisions PlanGameResolve(uint32_t viewportW, uint32_t viewportH);
    static FramePlan::Path PlanPath(RenderPath p) { return static_cast<FramePlan::Path>(static_cast<uint32_t>(p)); }
    static FramePlan::AA   PlanAA(AAMode m)       { return static_cast<FramePlan::AA>(static_cast<uint32_t>(m)); }

    // ---- FrameStats: one writer per field, and a publish across the seam ----
    // Copies the emit-owned fields of `src` onto `dst`, and nothing else; the
    // main-owned fields listed over m_frameStats are left alone.
    static void FoldEmitStats(FrameStats& dst, const FrameStats& src);
    // Emit half (main, or the lane under -brslane): hand the private copy over.
    // Called from RunEmitHalf at the end of the emit half - not from
    // FinishFramePreparation, which on a main-thread emit runs after App's debug
    // HUD has read GetFrameStats() and would add a one-frame lag.
    void PublishEmitStats(float emitMs);
    // Main: fold the last published copy onto m_frameStats. One call site, at
    // the end of BuildScene_SortAndEmit in both arms, so m_frameStats changes at
    // the same program point whichever half produced it.
    void ConsumeEmitStats();

    // ---- Core ----
    // The two halves' views of the draw list. Named rather than indexed at the
    // call site so a reader cannot pick the wrong buffer by writing 0 or 1.
    std::vector<DrawPacket>&       DrawPacketsWrite()       { return m_drawPacketsBuf[m_emitIdx]; }
    const std::vector<DrawPacket>& DrawPacketsWrite() const  { return m_drawPacketsBuf[m_emitIdx]; }
    const std::vector<DrawPacket>& DrawPacketsRead()  const  { return m_drawPacketsBuf[m_sceneRead];  }

    std::vector<uint32_t>&         InstanceRemapWrite()      { return m_instanceRemapBuf[m_emitIdx]; }
    const std::vector<uint32_t>&   InstanceRemapRead() const { return m_instanceRemapBuf[m_sceneRead];  }
    std::vector<VolumetricFogPass::VolLight>&       VolLightsWrite()      { return m_volLightsBuf[m_emitIdx]; }
    const std::vector<VolumetricFogPass::VolLight>& VolLightsRead() const { return m_volLightsBuf[m_sceneRead];  }

    // Scene-authored fog (VolumetricFogComponent + LocalFogVolumeComponents),
    // resolved by SyncSkyboxIBL on the game half and pushed into
    // VolumetricFogPass by Render_BindFrameResources on the render half. Same
    // hand-off as the volumetric lights: the local list is a vector refilled
    // every frame, so it must not be read while the other half rebuilds it.
    VolFogSceneState&       VolFogSceneWrite()      { return m_volFogSceneBuf[m_emitIdx]; }
    const VolFogSceneState& VolFogSceneRead() const { return m_volFogSceneBuf[m_sceneRead]; }

    // The scene-payload publish is split along the index / payload seam,
    // because the two halves belong to different threads once PREPARE runs on
    // the lane.
    // AdvanceSceneWrite is the index half and runs on main at hand-off: it names
    // the buffer the frame in flight owns and moves main onto the other one, so
    // SetCamera(F+1) can start writing at once. The seed copy comes with it
    // (state, not payload); that is correct on main only because UploadFrameData
    // stays on main too - the FrameView is final for F when this runs.
    uint32_t AdvanceSceneWrite()
    {
        // The mesh-AABB frame oracle runs here, not in PublishSceneRead: under
        // -brslane PublishSceneRead runs on the render-owner lane, while this half
        // always runs on main at the hand-off. ValidateMeshAABBFrame memcmps the
        // published mapping against m_aabbMaster, which the Animation phase of F+1
        // rewrites (the bone-AABB merge) inside the window the -animoverlap barrier
        // opens; on the lane it would compare a snapshot against a master main is
        // editing and report its own race. Nothing in the emit half writes a mesh
        // AABB (BuildScene_EmitBatches makes no m_meshMgr call), so checking at the
        // hand-off compares the same two things, on the thread that owns them.
        m_meshMgr.GetDescriptorHeap().ValidateMeshAABBFrame();
        const uint32_t emitIdx = m_sceneWrite;
        // The settings snapshot rides the index half: the store is main-only,
        // and this half always runs on main (the payload half may not).
        PublishSettingsSnapshot(emitIdx);
        // The view's break reasons raised while this frame was built go with it
        // (the render half applies them at its start).
        m_frameViewBuf[emitIdx].viewBreaks |= m_pendingViewBreaks.exchange(0, std::memory_order_acq_rel);
        m_frameViewBuf[emitIdx].viewBreaksLoud |= m_pendingViewBreaksLoud.exchange(0, std::memory_order_acq_rel);
        m_sceneWrite ^= 1u;
        // State, not payload: seed the next frame's write buffer with what was just
        // published, so a game-half reader that runs before this frame's write
        // still sees F-1, not F-2. See FrameView.
        m_frameViewBuf[m_sceneWrite] = m_frameViewBuf[emitIdx];
        // ...except this frame's payload: its view breaks and whether its DLSS
        // constants carried a reset belong to it alone.
        m_frameViewBuf[m_sceneWrite].viewBreaks     = 0;
        m_frameViewBuf[m_sceneWrite].viewBreaksLoud = 0;
        m_frameViewBuf[m_sceneWrite].dlssResetSent  = false;
        return emitIdx;
    }

    // The payload half, and the last thing PREPARE does - on whichever thread
    // finished it. m_sceneRead has exactly one writer, here.
    void PublishSceneRead(uint32_t emitIdx)
    {
        // One store. This half may run on the lane, so the mesh-AABB oracle lives in
        // AdvanceSceneWrite.
        m_sceneRead = emitIdx;
    }

    // Called once at the end of the game half. Swaps which buffers the render
    // half sees; the game half then builds into the other set.
    void PublishRenderScene() { PublishSceneRead(AdvanceSceneWrite()); }

    // ---- Runtime toggles ----
    // The pyramid's validity and the camera it was captured with are the main
    // view's cross-frame state (m_mainView.hiz, RenderGraph/ViewState.h). The
    // cull projects AABBs with that viewProj, not the current frame's: the
    // pyramid is one frame old, and projecting with the current VP mis-taps by
    // the camera's per-frame motion, false-culling large parts of the scene
    // while the camera flies or cuts.
    // HiZUsableThisFrame: whether last frame's pyramid may be tested against this
    // frame (render half) - generated, camera history continuous, and the camera
    // moved less than 1 m / ~1.8 deg since the capture. The one gate for
    // InstanceCull and the meshlet amplification shader.
    bool HiZUsableThisFrame() const;

    // ---- Meshlet path ----
    void     ReadMeshletFlags();
    // The meshlet gate's path half: setting && capability && pre-warmed PSO &&
    // not killed. `reason` = a static snake_case string when false.
    bool     MeshletPathPreconditions(const char*& reason) const;
    // The meshlet gate's geometric half, from the mesh slot's sidecar (game
    // half, gather).
    uint16_t MeshletBlocksForSlot(uint32_t meshDescSlot, bool skinned, uint32_t customVSID) const;
    // Emit half: whether the packet's indirect command belongs to a meshlet
    // group - the cull CS converts it to DISPATCH_MESH. The pristine fill, the
    // group key, the meta bit and -meshletverify all ask this.
    bool     MeshletIndirectPacket(const DrawPacket& dp) const;
    // Emit half: the packet's bucket in the retained shadow layout -
    // ClassifyShadowDrawGroup's, + ShadowDrawGroup::MeshletOffset when the CSM
    // meshlet path draws it this frame; -1 = casts into no bucket.
    int      ShadowRetainedGroup(const DrawPacket& dp) const;
    // Render half: cascade @p cascade's MeshletCullCB bank - the planes its cull
    // dispatch used (plane 5 neutralised), the sidecar, the CSM stats slice.
    void     WriteMeshletShadowBank(uint32_t cascade, const float (&planes)[6][4],
                                    const DirectX::XMFLOAT4X4& cascadeVP);
    // -meshletverify: re-derive every group's meshlet decision and check what
    // the GPU reads (meta bit 31, pristine words 3 / 7, the limits); one
    // [MESHLET_VERIFY] line per frame (emit half, after the meta fill).
    void     VerifyMeshletGroups(uint32_t frameSlot);
    void     WriteMeshletCullCB();     // render half, Render_BindFrameResources

    // The main view's LOD inputs, as the CB carries them (render half).
    MeshletLodView GetMeshletLodView() const;
    void     LogMeshletFrame();        // render half, after the graph recorded

    // ---- Shared runtime material resources ----
    void QueueFrameMaterialWrites(const std::vector<MaterialCpuWrite>& writes, bool verify);

    // ---- Shared environment handles the preview borrows ----
    // The composition of one Mesh of a model ("<model>#<keyhex>",
    // Preview::AssetKind::MeshAsset): its sections at the identity with the
    // materials the model gives them, from the view MeshAssetRegistry::BuildModel
    // builds over preview library references (the StaticMesh registry's own view
    // would make them world libraries). False = a library would not open: not
    // memoised, retried, like a `.meshlib` that would not open.
    bool ComposeMeshItemAsset(const std::string& ref, Preview::AssetComposition& comp);
    // The preview's reference to @p meshlibPath (m_previewMeshLibs), taken the
    // way the `.meshlib` composition takes it; invalid when it would not open.
    Resource::Handle PreviewLibraryHandle(const std::string& meshlibPath);
    // The library entry @p pl draws: its section key's entry in the preview's
    // library when it carries one (and the library has it), else its meshId
    // hint.
    uint32_t PlacementEntry(const Preview::AssetPlacement& pl);

    // ---- RT reflection bindless slots ----
    void PrepareReflectionBindings();

    // ---- Skybox / IBL textures ----
    uint64_t EnsureUniformSkyCube(const DirectX::XMFLOAT3& radiance);

    // ---- Material preview layer ----
    // Resolve the studio IBL handles and push them at the preview layer. Game
    // half - TextureSystem::GetTexture is main-thread only.
    void SyncPreviewEnvironment();

    // Hand one cached library back. False when the path was not cached. The
    // three things it must get right are documented at the definition
    // (PreviewAssetSource.cpp): the shared-library test, which is inside the
    // release (MeshLibrary::ReleaseIfLast) rather than asked before it; the
    // deferred slot hand-back; and their order - release first, detach second,
    // which the definition proves safe against GraphicsDX12::DestroyBuffer's
    // deferral. Game half only, and it checks ([SPLITCHECK][RENDERCTX]).
    // When the live World holds the same library this is a bare refcount
    // decrement: correct for the descriptors the scene draws, but it also leaves
    // registered every mesh-descriptor slot the preview minted for an entry the
    // world does not draw (a prefab part naming mesh id 7 of a library the level
    // only uses entry 0 of). Those slots are per (lib, entry) in MeshManager's
    // cache, bounded by the library's entry count, and freed when OnWorldClear
    // resets the table - a deliberate trade against a per-entry refcount
    // spanning two owners to reclaim a few 64-byte descriptors.
    bool     ReleasePreviewMeshLib(const std::string& normalisedPath);

    // Drained at the top of BuildRenderScene, beside ApplyPendingGamePiP: before
    // any of the game half's slot registration for the frame (the scene's own
    // MeshLibrary::Load, the skinned / VFX RegisterBuffer calls, and
    // ExtractPreviewRequests at the end). In the editor ReleasePreviewMeshLib is
    // also reached from EditorLayer::OnUIRender, which FramePipeline builds under
    // a Render thread context. Game half only, and it checks
    // ([SPLITCHECK][RENDERCTX]).
    void     DrainPreviewMeshLibHandbacks();
    // Enter (or stay in) the failed state for @p assetPath, so its extract stamp
    // term advances every kPreviewAssetRetryFrames frames.
    void     NotePreviewAssetLoadRetry(const std::string& assetPath);
    // Leave it. Called from every outcome that is not a retryable load
    // failure, on both routes that can arm one: the multi-part resolve
    // (ResolvePreviewAsset) and the thumbnail sidecar's dependency query
    // (GetPreviewAssetDeps), whose compose arms the retry just as readily. Says
    // once how long the asset was failing - the only place the retry cadence is
    // observable.
    void     ClearPreviewAssetLoadRetry(const std::string& assetPath);
    // Enforce kPreviewMeshLibCap. @p keep is never evicted (it is the library
    // the caller is resolving right now), and neither is anything touched
    // during the current compose / resolve - lastUse is stamped on every hit
    // and compared against m_previewMeshLibKeepMark.
    void     TrimPreviewMeshLibs(const std::string& keep);

    // ---- Material snapshots ----
    uint32_t AcquireMatSnapshot(const MaterialComponent* mc, uint64_t key,
                               std::vector<MatSnapshotCopy>* deferredCopies);
    // The per-frame refresh of an identity-keyed (volatile) snapshot. The only
    // volatile writer is FaceShading, which rewrites three customParams float4s a
    // frame; a whole-component copy (3 string-keyed unordered_maps + ~25
    // strings) costs ~2.7 ms-core for 400 faces. When allowParamsOnly (not in
    // this frame's matDirty), the scalar hash still matches and no lease shares
    // the snapshot, only the customParams values are copied (same keys, same
    // order); otherwise the whole thing is copied. Called from the refresh jobs:
    // one job per destination slot.
    void RefreshVolatileSnapshot(uint32_t idx, const MaterialComponent& src, bool allowParamsOnly);

    // Return the storage of snapshots nothing referenced recently to the free
    // list. The point is the storage, not the memory: assigning into a retired
    // MaterialComponent reuses its ~25 string allocations, which saves ~1 ms a
    // frame on AIBench (materials with volatile parameters re-hash every frame
    // and never hit).
    void     RetireMatSnapshots();
    // Clear the whole table once its storage passes a cap. Unreferenced
    // snapshots are not collected here: RetireMatSnapshots does that every
    // build and keeps their storage (the definition says why there is no age
    // test).
    void     GCMatSnapshots();

    // ---- AA mode ----
    // Re-applies the TAA / FXAA enable from m_aaMode, forced off in Wireframe
    // view (thin lines ghost badly under temporal AA). Called by both SetAAMode
    // and SetViewMode so the AA state stays consistent across the two toggles.
    void UpdateAAEnabled();

    void NoteAAFallback();

    // ---- Shared HDR post-process ping-pong pool ----
    struct PostPool;   // with the secondary views, below
    void EnsurePostProcessPool(PostPool& pool, uint32_t w, uint32_t h);

    // ---- Debug target view (Renderer_DebugTargets.cpp) ----
    std::uint32_t DebugTargetProduceMask();                                  // the service point
    void          RunDebugProducers(RHI::CommandList cl, std::uint32_t mask);   // the render half
    // This frame's material cost table into DebugTargetViewPass's buffer, on the
    // frames a producer that reads it runs (the render half).
    void          RecordDebugCostTable(std::uint32_t frameSlot);

    void   BuildDebugTargetRegistry();                                // Compile
    void   RefreshDebugTargetAvailability() const;                    // render lane parked / render half
    size_t FindDebugTargetLocked(const std::string& name) const;      // SIZE_MAX = none
    bool   ResolveDebugTargetSlot(const DebugTargetSlot& slot, AttachmentSource& out, std::string& why) const;
    void   ExecuteDebugTargetView(RHI::CommandList cl);               // Phase 7, the restore list

    // ---- Pass capture (Renderer_PassCapture.cpp) ----
    void ServicePassCapture();                         // ServiceEnableGatedResources
    bool PassCapturePictureWanted() const;             // the debug target pass's Request (its pipeline)
    void ExecutePassCapture(RHI::CommandList cl);      // the restore list, after the graph
    void ShutdownPassCapture();
    void PublishPassCapture(PassCapture::Status st);   // the render half / the serial window
    // Renderer_DebugTargets.cpp: the copy painted into the picture with the
    // debug target shader (Slot::Preview); Auto resolved from the registry
    // entry of @p name (the stencil row for a depth copy's plane 1), else the
    // format. False with @p why.
    bool PaintPassCapture(RHI::CommandList cl, const std::string& name, const RHI::Texture& copy,
                          RHI::ResourceState copyState, int plane, const DebugTargets::View& asked,
                          const RHI::Texture& picture, RHI::ResourceState pictureState,
                          DebugTargets::View& shown, std::string& why);

    // ---- Internal helpers ----
    void EnsureWorkers();
    // Render() sub-stages. The rest of Render() (worker kicks, fences,
    // cross-queue CL wiring) stays inline because those phases share
    // command-list lifetimes that cannot cross a return.
    void Render_BindFrameResources(uint32_t frameSlot); // bind CBs/SRVs to passes; allocates no CLs
    // The view's half of the bindings, for the main view (above) or a secondary one (@p primary false:
    // @p drawList is its own, @p show the flags it shows).
    void BindViewInputs(uint32_t frameSlot, const std::vector<DrawPacket>& drawList, const char* name,
                        ViewState& state, bool primary, uint32_t show);
    // ---- A secondary view's segment of the render half (Renderer_Views.cpp) ----
    // Begins view @p slot's segment of its RenderAsync call: its frame, its HDR target (pushed), the
    // recording-view scope, the graph's view set, the passes' view, its plan, its camera-derived frame
    // data and its draw list. False = it cannot render this frame (nothing to undo).
    bool BeginViewSegment(uint32_t slot);
    // The view's breaks and bindings (Render_BindFrameResources' counterpart).
    // The particle / trail / tracer render passes' camera, from FrameViewRead(): the main view's (the compute
    // prepass) or a secondary view's (its segment).
    void Render_BindVfxFrameData();
    void Render_BindViewResources(uint32_t slot, uint32_t frameSlot);
    // Ends it: everything BeginViewSegment set goes back to the main view's.
    void EndViewSegment(uint32_t slot);
    // The view's plan: the main view's with what only the main view does switched off (the upscalers,
    // the path tracer and ReSTIR, the traced and screen-space reflections, shadows and AO, SSGI and
    // XeGTAO, the CSM render - it reads the main view's cascades) and its post size.
    FramePlan::Decisions SecondaryViewPlan(const FramePlan::Decisions& main, uint32_t width, uint32_t height) const;
    // A ready view's resources back to the device (deferred): ServiceViews and shutdown.
    void FreeViewResources(uint32_t slot);
    // The recording view's ViewState: the main view's outside a secondary view's segment.
    ViewState& RecordingViewState();
    // The recording view's SSR chain (null: none).
    SSRSubsystem* RecordingSSR();
    // Secondary view @p viewSlot is live and has rendered: a pick or an image can be asked of it.
    bool IsViewPickable(uint32_t viewSlot) const;
    void Render_ComputePrepass();                      // skinning/particle/trail/tracer/beam compute CLs
    // Plan this frame (Renderer/FramePlan.h) from what the decision sites read
    // and compare it with what they decided. Render half.
    void FinishFramePlan();
    // A flag that forces the path tracer's camera rays, which ray reconstruction
    // cannot denoise: GetRayReconstructionInertReason and the frame plan's post
    // inputs read it (Renderer_RenderPath.cpp). The pins folded into engine.json
    // rows count when they are pins (DevPin).
    bool RRCameraRaysForced() const;
    // The material passes' s0 sampler LOD bias for these render and display
    // widths (Graphics/MaterialSampler.h); BeginFrame, on a change.
    void ApplyUpscaleMipBias(uint32_t renderW, uint32_t displayW);
    // This frame's FogApply constants (Graphics/FogApply.h) - which fog
    // composites will run, their parameters, the froxel volume and the opaque
    // depth snapshot. The render half, after the view-mode gates and before the
    // graph records.
    void WriteFogApplyCB(const RendererDetail::LightCB& lb, uint32_t opaqueDepthIdx);
    // The global medium, the medium contributors' (the height fog) and the gather's local
    // volumes into this frame's slot of m_mediumSet; sets m_mediumIdx. Render half, after
    // VolumetricFogPass::SetSceneFog.
    void UploadMediumSet();
    void SubscribeToWorld(World* world);
    void OnEntityDestroyed(Entity e);
    void InitMeshManager();
    void InitSkinningSystems();
    void CreateConstantBuffers();
    void UploadFrameData(FrameIndex frame, uint32_t vpW, uint32_t vpH);
    // What BuildViewFrame builds one view's camera-derived frame data into: the main view's in
    // UploadFrameData (game half, the upload slot), a secondary view's at the start of its segment
    // (render half, the render slot).
    struct ViewFrameTarget
    {
        FrameView&                  fv;     // its camera and sizes in; its RenderView out
        ViewState&                  view;   // its jitter
        const FramePlan::Decisions& plan;
        RendererDetail::PerViewCB*  perView  = nullptr;
        RendererDetail::LightCB*    light    = nullptr;
        ShadowSystem*               cascades = nullptr;   // its CSM fit; null = none
        uint32_t                    slot     = 0;         // the ring slot of its per-frame buffers
        uint32_t                    instanceRemapIdx = 0;
        // The main view: Streamline's constants, the upscalers' jitter, the scene's shadow passes and
        // the outline's camera are its alone.
        bool                        primary  = false;
    };
    void BuildViewFrame(const ViewFrameTarget& t);
    // @p fv's camera as the light clusters' (ClusterPass: the main view's grid, or secondary @p view's)
    // and as LightCB's cluster params (@p lb may be null).
    void SetClusterCamera(const FrameView& fv, ViewState* view, RendererDetail::LightCB* lb);
    // @p fv's camera as DecalPass's (the main view's, or secondary @p viewState's).
    void SetDecalCamera(const FrameView& fv, ViewState* viewState);
    // @p fv's CSM fit input (@p view: its view matrix), the main view's and a secondary view's alike.
    ShadowSystem::FrameInput CascadeInput(const FrameView& fv, const DirectX::XMMATRIX& view,
                                          const float lightDir[3]) const;
    // The secondary views' cascade volumes for the next frame's caster retention (game half).
    void FitViewShadowRetention(const float lightDir[3]);
    // Caster retention: inside the main view's cascades, or a secondary view's. With no secondary
    // view these are m_shadowFrustum's IsValid / IntersectsAny.
    bool ShadowRetentionValid() const { return m_shadowFrustum.IsValid() || m_viewShadowRetention; }
    bool ShadowRetains(const DirectX::BoundingBox& bb) const;
    // This frame's camera basis / view + unjittered proj from m_camera. Safe
    // during BuildRenderScene: m_view is one frame stale during the scene build
    // (UploadFrameData refreshes it after BuildRenderScene), which would make the
    // cluster / decal camera CBs lag the GBuffer by a frame (decals visibly jump
    // while the camera moves). Scene-build code must use these, not m_view.
    void BuildCameraBasis(DirectX::XMVECTOR& outPos,
                          DirectX::XMVECTOR& outForward,
                          DirectX::XMVECTOR& outUp) const;
    void BuildCameraViewProj(DirectX::XMMATRIX& outView,
                             DirectX::XMMATRIX& outProjNoJitter) const;
    // The same two from a FrameView snapshot (the light prep on the lane uses
    // the view main captured when the prep would have run there).
    static void BuildCameraBasisFor(const FrameView& fv, DirectX::XMVECTOR& outPos,
                                    DirectX::XMVECTOR& outForward,
                                    DirectX::XMVECTOR& outUp);
    static void BuildCameraViewProjFor(const FrameView& fv, DirectX::XMMATRIX& outView,
                                       DirectX::XMMATRIX& outProjNoJitter);
    void SyncMaterialTextures(Entity e, const MaterialComponent& mc);
    // Refresh MaterialRuntimeTextureView for every shared runtime resource once
    // per frame (not once per binding entity) - promotes async-ready textures.
    void PromoteRuntimeMaterialTextures();
    void ReleaseRuntimeMaterialResource(uint64_t key, bool deferRelease);
    void ReleaseEntityMaterialRuntimeBinding(Entity e);
    RuntimeMaterialResource* FindRuntimeMaterialResource(Entity e);
    const RuntimeMaterialResource* FindRuntimeMaterialResource(Entity e) const;

    // Return the persistent GPU material slot for a binding key and whether this
    // is its first sight this frame (drives write-once-per-frameSlot).
    // outNeedsBaseWrite is true when frameSlot's buffer does not yet hold the
    // content. slotValid is false only on overflow (> m_maxMaterials live keys).
    uint32_t AcquirePersistentMatSlot(const MaterialBindingKey& key, uint32_t frameSlot,
                                      bool& outFirstSight, bool& outNeedsBaseWrite,
                                      bool& outSlotValid);
    // End-of-frame GC: evict every persistent key not seen this frame, parking
    // its slot in m_freeMatSlots. Bounds the map to live keys so
    // animated-material key churn cannot fill it to m_maxMaterials (overflow ->
    // slot 0). Safe because per-frameSlot buffering means an absent key's slot
    // is referenced by no in-flight frame. Returns the live key count (for the
    // high-water log).
    uint32_t SweepPersistentMatSlots();
    // Record one just-written staging slot as a dirty span for the DEFAULT-heap
    // material buffer, coalescing with the open range when both the frame slot
    // and the slot index are contiguous. No-op when m_materialBufferGPU is
    // invalid (creation failed -> everything binds the UPLOAD staging).
    void     StageMaterialSlot(uint32_t frameSlot, uint32_t slot);
    // Bind target for every per-pixel material consumer: the DEFAULT buffer when
    // it exists, else this frame's UPLOAD staging. Gated on
    // m_retainMaterialSlots: per-frame compaction reassigns slot indices every
    // frame, so a shared DEFAULT buffer would rewrite slots that in-flight frames
    // still draw from (the flicker the per-frame UPLOAD ring prevents); without
    // retained slots each frame binds its own frameSlot's staging buffer.
    bool MaterialGpuBufferActive() const
    {
        return m_retainMaterialSlots && m_materialBufferGPU.IsValid();
    }
    const RHI::GPUBuffer& MaterialSRVBuffer(uint32_t frameSlot) const
    {
        return MaterialGpuBufferActive() ? m_materialBufferGPU
                                         : m_materialBuffer[frameSlot];
    }

    // ---- Stable instance slots (see the member-block comment) ----
    // (IsStaticLaneInstance is declared below DrawCandidate - a nested type.)
    // Return the entity's stable slot, allocating on first sight (free list ->
    // high-water). ~0u on static-segment overflow - the caller degrades the
    // candidate to the per-frame dynamic segment (never fails the draw).
    uint32_t AcquireInstanceSlot(Entity e);
    // Park the entity's slot for deferred reuse (idempotent). Fired by the
    // destroy listener and by the dirty feed's removed list.
    void     FreeInstanceSlot(Entity e);
    // Full invalidate: drop every slot / shadow / range. Safe mid-frame - all
    // consumers are graphics-queue, and in-flight frames' draws execute before
    // this frame's re-upload copies (same-queue ordering).
    void     ResetInstanceSlots();
    // Append one record to the staging ring (m_instanceBuffer[frameSlot]) and
    // extend / open a coalesced copy range. Updates the CPU shadow.
    void     StageInstanceRecord(uint32_t frameSlot, uint32_t slot,
                                 const GPUInstanceData& rec, bool shadowAlreadyUpdated = false);
    // drawLocal -> entity for CPU consumers that hold packet-relative indices
    // (SceneVoxelPass filter, probe-bake filter). Retained mode resolves through
    // this frame's remap; otherwise the identity lookup.
    Entity   EntityForDrawLocalInstance(uint32_t drawLocal) const;
    // drawLocal -> instance-buffer slot (identity when retained mode is off).
    uint32_t SlotForDrawLocalInstance(uint32_t drawLocal) const;
    // True when retained-instance mode is ACTIVE this frame (flag + resources).
    bool     RetainedInstancesActive() const
    {
        return m_retainInstances && m_instanceBufferGPU.IsValid() &&
               m_instanceRemapHeapIdx[0] != 0;
    }
    // GPU-compaction frame preconditions - the one definition behind both the
    // SortAndEmit SceneEmit::gpuCompactionFrameActive latch and the earlier
    // SceneEmit::shadowArgsFrameActive latch in BuildRenderScene: every input is
    // frame-stable, so the two evaluations cannot disagree within a frame.
    bool     GpuCompactionPreconditions(uint32_t frameSlot) const;
    // True when anything still reads m_bvhVisibleMask / m_bvhTestedMask this
    // frame. Two consumers: the in-gather cull inside GatherMeshLibRefs (dead
    // whenever deferCull == retained == true) and the `visible` test in
    // ClassifyCandidates, whose only use is a drop branch that cannot fire while
    // the shadow-args latch holds (the packet set then does not depend on the
    // camera). When both are dead the whole BVH FrustumCull (~0.66 ms a frame) is
    // dead output.
    // Evaluate only after `retained` and the shadow-args latch are latched for
    // the frame (BuildRenderScene hoists both above the cull). Fail-safe: the
    // masks are always cleared, never left stale, so a wrongly-true gate costs
    // one frustum cull and a wrongly-false one makes every candidate visible - it
    // over-draws but never drops geometry. Do not leave stale masks instead: that
    // would pop geometry.
    bool     BvhCullMasksNeeded() const
    {
        // Write side: the only callers are BuildScene_CullBVH and BuildRenderScene,
        // both game half, and both ask about the latch this frame is setting - not
        // the one the render half is consuming.
        return !(m_retainedCandidates && m_retainedSceneDiff) ||
               !SceneEmitWrite().shadowArgsFrameActive;
    }
    // Record the per-cascade shadow cull dispatches (count zeroing, barriers,
    // InstanceCull.cs mode 2) at the head of the shadow pass's CL. Main thread
    // only, before the shadow worker kick - CullingPass's CB pool cursor is not
    // thread-safe against the later main-view dispatch. true = the retained args
    // are consumable this frame (including zero shadow commands: nothing
    // recorded, every cap 0, the pass draws no groups). false = hard failure -
    // the caller must not arm ShadowPass's retained state.
    bool     RecordShadowFamilyCull(RHI::CommandList cl, uint32_t frameSlot);

    void SyncSkyboxIBL(World& world);
    // Game half: VolumetricFogComponent + LocalFogVolumeComponents ->
    // VolFogSceneWrite() + m_volFogDemandBits. Called from SyncSkyboxIBL.
    void GatherVolumetricFog(World& world);

    // Per-channel atmosphere transmittance for the sun, or (1,1,1) when the
    // feature is off / the scene has no procedural atmosphere. `lightDirTravels`
    // is LightCB.lightDir (sun -> ground); the towards-sun negation happens
    // inside. `altitudeKm` is where the light arrives: 0 = the ground (LightCB),
    // the cloud layer for the clouds - (0,0,0) once the planet blocks the sun
    // from there. See SunTransmittance.h for why this exists.
    DirectX::XMFLOAT3 ResolveSunTransmittance(
        World& world, const float lightDirTravels[3], float altitudeKm = 0.0f) const;

    // Write the sun into LightCB: `colorTOA` is the authored (top-of-atmosphere)
    // colour, which is cached in m_sunColorTOA for the sky/cloud passes, while
    // LightCB.lightColor receives the transmittance-filtered ground-level value
    // that shading consumes. Both LightCB sun writers go through this.
    void SetLightCBSun(World& world, const DirectX::XMFLOAT3& colorTOA);

    uint32_t RegisterMeshLibMesh(const MeshLibRef& ref);

    // Unified VFX: drain PendingMeshVFXSpawns - load each requested mesh path
    // into the MeshLibrary and patch the pre-created VFX entity with a
    // MeshLibRef (+ material). Called from BeginFrame before BuildRenderScene.
    void DrainMeshVFXSpawns(World& world);

    // The persistent-slot key of a batch whose first candidate is `c` - the ONE
    // construction the emit nest and ResolveBatchPlan share, so the two can never
    // key a PER_FRAME_PARAMS material differently (MaterialBindingKey::identity).
    static MaterialBindingKey BindingKeyOf(const DrawCandidate& c);

    // `laneForm`: the material cast-shadow layer comes from matBits
    // (MB_CAST_SHADOW) instead of *mc - the render-owner lane's form. The two
    // flags are what main captured where the tracking ran on main.
    void TrackSpotShadowCasters(const std::vector<DrawCandidate>& candidates,
                                bool retainedSceneDiff, bool matTexUnsettled,
                                bool laneForm);

    // Folds a candidate's material binding fields into the one 64-bit key
    // m_matSnapTable is probed with. Declared here because it names DrawCandidate.
    static uint64_t MatSnapKey(const DrawCandidate& c);

    // ---- The secondary-view classify lane ----
    // BuildScene_ClassifyCandidates rewrites every candidate outside the main
    // frustum to DrawFilter::Shadow (casters) or drops it (everything else),
    // before the render half's second BVH cull sees a draw list, so without this
    // lane a PiP camera pointed away from the player could show only what is in
    // both frusta.
    // A duplicate, not a reprieve: the original candidate keeps its fate exactly
    // (Shadow caster, or dropped), so the CSM, the GPU cull, the indirect groups
    // and the main view are unchanged; a copy with a secondary-only DrawFilter is
    // appended for the preview. A reprieve is wrong twice over: an off-frustum
    // caster needs both a Shadow packet (for the cascades) and a shaded packet
    // (for the PiP), and DrawFilter::Shadow packets are lossy - the conversion
    // resets perm to {}, stencilRef to 1, shadowCullMode to 0 and depth to 0, so
    // the preview's GBuffer would draw skinned meshes through an unskinned
    // permutation and NPR characters through the PBR stencil. That is why
    // CullWorldForPreview reads these lists and not Shadow.
    // No cost with no live secondary view: m_secondaryCullActive is false, which
    // is one bool test at each of the three classify call sites and one at the
    // top of the append.

    // Which secondary volumes see this world AABB: kSeenByPiP (any live
    // secondary view's frustum), kSeenByProbe (the faces of the probe baking
    // this frame). 0 immediately when the lane is inactive.
    uint32_t SecondaryFrustaSee(const DirectX::XMFLOAT3& mn,
                                const DirectX::XMFLOAT3& mx) const;
    // Queue a secondary-view duplicate of @p c (already carrying its natural
    // filter and every derived field) when a live secondary view can see
    // @p aabb. Called from the classify sites that are about to drop the
    // candidate or convert it to DrawFilter::Shadow. No-op when the lane is
    // inactive, when the candidate has no bounds, or for filters no secondary
    // view draws (Custom outline shells, Water).
    void     NoteSecondaryViewCandidate(const DrawCandidate& c,
                                        const DirectX::XMFLOAT3& mn,
                                        const DirectX::XMFLOAT3& mx);
    // Map a natural filter to its secondary-view twin in place. false = this
    // filter has no secondary form and the caller must not emit a copy.
    static bool RemapToSecondaryFilter(DrawCandidate& c);
    // Does the probe baking this frame take a DrawFilter::ProbeView copy of @p c
    // (natural filter)? Opaque only - the bake draws no blended surface - and
    // only an entity whose viewMask has ViewBit::ReflectionProbe (the faces'
    // CullView would drop it).
    static bool WantsProbeCopy(const DrawCandidate& c);

    // ---- BuildRenderScene: shared helpers and per-frame state ----
    // Static-lane test shared by the candidate snapshot and the retained emit:
    // tracked in the scene store, lane 0 (no skin / dissolve component), no
    // skinned prev-pos ring, no live dissolve params.
    bool     IsStaticLaneInstance(const DrawCandidate& c) const;

    // Material -> filter / permutation / stencil / depth classification, shared
    // by the gather helpers; reads m_camera + the passed camera forward
    // (view-space depth).
    void     ApplyBlendMode(DrawCandidate& c, const DirectX::XMFLOAT3& camForward);
    // Lazy-resolve customShaderPath -> dynamic PS id (RegisterMaterialShader +
    // reflection sync). Returns 0 when the material has no custom shader.
    uint32_t ResolveCustomPSID(Entity entity, const MaterialComponent& m);
    // Lazy-resolve customVertexShaderPath -> dynamic VS id
    // (RegisterMaterialShader + reflection sync, mirroring ResolveCustomPSID).
    // Returns 0 when the material has no custom vertex shader.
    uint32_t ResolveCustomVSID(Entity entity, const MaterialComponent& m);
    // The vertex source ResolveCustomVSID registers for @p m ("" = pixel-only:
    // the engine's generic VS) - shared with PreparePreloadMaterial.
    std::string CustomVertexShaderPath(const MaterialComponent& m);

    void ResolveSpawnMaterials(World& world);

    // BuildRenderScene is a thin orchestrator over the BuildScene_* helpers
    // below. Order + the few cross-helper locals (the candidate list, the BVH
    // masks, the camera forward) are documented at the call site.
    void BuildRenderScene(World& world);

    // ---- Gather / cull stages (run in sequence inside BuildRenderScene) ----
    // Resolve the player entity (PlayerComponent) + its descendant subtree into
    // m_playerShadowSet and merge their world AABBs (m_playerShadowAabbMin/Max).
    // Runs before the gather so candidates can be tagged perObjectShadowCaster;
    // the set is then read-only during the parallel MeshLibRef gather.
    void BuildScene_ComputePlayerShadowSet(World& world);
    // Same, for the bangs subtree (HairShadowCasterComponent roots) -> the small
    // hair (bangs -> face) shadow map. Fills m_hairShadowSet + m_hairShadowAabb*.
    void BuildScene_ComputeHairShadowSet(World& world);

    // ---- Retained-scene dirty feed ----
    // SceneItemStore snapshots a cheap per-entity "render key" (post-propagate
    // GlobalTransform + mesh identity + visibility fields) for every renderable
    // entity (MeshLibRef + MeshHandle pools) and diffs it once per frame at
    // BuildRenderScene's read point (BuildScene_DirtyDiff). The diff is the
    // authority; events (entity destroy, undo restore, material sync, world
    // clear) are accelerators feeding the same store. Membership add / remove is
    // detected by a per-frame seen-stamp sweep - no ECS pool hooks. Consumers
    // (the retained DDGI gather, the candidate cache, stable instance slots) read
    // m_sceneDirty / m_sceneRemoved.
    void BuildScene_DirtyDiff(World& world);  // fills the lists below

    // ---- Retained draw-candidate cache ----
    // Static-lane DrawCandidates (primitive + MeshLibRef gathers) persist across
    // frames: a patch frame restores them from m_candCache, patches
    // SceneDiffView.dirtyXform transforms in place, and re-gathers only the
    // dynamic lane (skinned / dissolving / billboard / beam / afterimage).
    // Anything membership- or appearance-affecting (add / remove / dirtyOther /
    // matDirty, unresolved meshes, pending material textures, player / hair /
    // face-overlay set churn, active dissolve, picking-outline change) falls back
    // to the full gather that frame. Candidates are cached pre-classification:
    // the camera-dependent frustum cull + shadow-fallback split runs per frame
    // over the merged vector in BuildScene_ClassifyCandidates.
    // ClassifyInputs: everything the classify reads that is not a candidate
    // field, captured where BuildRenderScene would run the classify on main. The
    // lane classifies later, after UploadFrameData has rewritten FrameView's view
    // (the classify reads the view before that rewrite - last frame's frustum)
    // and after PrepareLanePlan clamped shadowArgsFrameActive, so it must not
    // re-read either off the renderer. m_shadowFrustum and the BVH masks are read
    // in place: their next writers are BeginFrame(F+1) and BuildRenderScene(F+1),
    // both after the drain that joins the lane.
    ClassifyInputs CaptureClassifyInputs(size_t cullBegin,
                                         const DirectX::XMFLOAT3& camForward) const;
    // `world` null = the lane form: the box comes from the candidate's own
    // aabbMin / aabbMax / hasAabb and the material predicates from matBits alone,
    // so nothing here reads World memory. Non-null = the main form (live
    // WorldAabb lookups, MatBitCheck against *mc).
    void     BuildScene_ClassifyCandidates(const ClassifyInputs& in, World* world,
                            std::vector<DrawCandidate>& candidates);

    void           RunLaneClassify(std::vector<DrawCandidate>& candidates);
    void           BuildLaneClassifyReference(World& world,
                                              const std::vector<DrawCandidate>& candidates,
                                              const std::vector<DrawCandidate>& procedural);
    void     SnapshotCandidateCache(const std::vector<DrawCandidate>& candidates,
                                    size_t primEnd, uint64_t setSig);
    // Refresh cached MaterialComponent pointers (pools may reallocate without a
    // tracked membership event) + apply dirtyXform. false = drift detected -> the
    // caller falls back to the full gather.
    bool     PatchCandidateCache(World& world);
    // Order-independent membership signature over the per-frame BFS tag sets
    // (player / hair shadow, face overlay) - their inputs (hierarchy, tag
    // components) are not part of SceneItemKey, so set churn must gate the patch
    // path.
    uint64_t ComputeCandidateSetSig() const;
    // The same signature over only the members the static cache holds
    // (m_candEntityToIdx) - what a cached row's tag bits depend on. A frame that
    // only adds dynamic entities may change the full signature (a spawned
    // character's hair / face-overlay tags) without touching this half.
    uint64_t ComputeCandidateStaticSetSig() const;

    bool TexUnsettledTouches(const std::vector<uint32_t>& entityToIdx) const;

    // ---- The static rows' copy goes to the lane too (-laneassemble) ----
    std::vector<DrawCandidate>& AssembleLaneCandidates();   // returns m_candLaneScratch
    void                       AssembleCachedRowsInto(std::vector<DrawCandidate>& out,
                                                      const std::vector<DrawCandidate>& dynamicRows,
                                                      uint32_t dynPrimCount) const;
    static bool                SurfaceAtlasEligible(const DrawCandidate& c);
    // Field-by-field comparison of two candidate lists (the lane oracles).
    // Returns the number of differing fields; `firstField` / `firstRow` name the
    // first. A different row count counts as one difference ("rowCount").
    static uint32_t            DiffCandidateLists(const std::vector<DrawCandidate>& a,
                                                  const std::vector<DrawCandidate>& b,
                                                  const char*& firstField, size_t& firstRow);

    // ---- Retained dynamic rows ----
    // The MeshLibRef rows the cache above leaves out (skinned / dissolving
    // entities, which a patch frame would re-gather with
    // BuildScene_GatherMeshLibRefs(dynamicOnly)) are kept too, exactly as that
    // gather produced them. A patch frame produces each row from its copy plus
    // this frame's World state, on worker lanes: the transform, the WorldAabb
    // (box + depth centroid), the skinning output's prev-pos base, the
    // MaterialComponent pointer, the view depth ApplyBlendMode computes, and -
    // for a volatile material only (FaceShading rewrites it every frame) - the
    // textures + matHash. Every other field is a function of state the patch
    // gate (usePatch) already pins, as for the static rows: mesh identity /
    // visibility / tags (SceneItemKey), material content (matDirty, SetDirty),
    // textures (m_matTexUnsettled), the tag sets (candSetSig), dissolve, the
    // picking outline.
    // What the patch can see changing and the gate cannot - skinning switched on
    // or off, a skinned mesh re-registered, a material appearing, vanishing or
    // changing its predicate bits, a missing transform - makes it return false:
    // the dynamicOnly gather runs that frame and its rows re-arm this cache.
    // -dynretainedoff = that gather on every patch frame (the A/B arm);
    // -dynretainedverify = run it anyway and compare every field of every row
    // ([DYN_RETAINED_ORACLE]).
    static bool DynRetainedEnabled();
    bool     PatchDynCandCache(World& world, std::vector<DrawCandidate>& candidates,
                               const DirectX::XMFLOAT3& camForward);
    void     SnapshotDynCandCache(const std::vector<DrawCandidate>& candidates, size_t from);
    void     VerifyDynCandCache(World& world, const std::vector<DrawCandidate>& candidates,
                                size_t from, const DirectX::XMFLOAT3& camForward);
    // The gather's custom-shader steady state, read-only: false whenever
    // ResolveCustomPSID / ResolveCustomVSID would still insert, register, sync
    // reflection or clear customParamsLegacyFormat for this material. Shared by
    // the gather's parallel pre-pass and the dynamic-row snapshot, which arms
    // only when no row has such work pending - so no patch frame skips a side
    // effect the gather would have performed.
    bool     PeekCustomShaderIDs(const MaterialComponent& m, uint32_t& ps, uint32_t& vs) const;

    // ---- The dynamic rows' patch as jobs ----
    // Rows [lo, hi): copied from `src` first when it is non-null, then patched.
    void     PatchDynRows(const DynPatchPools& pools, DrawCandidate* rows,
                          const DrawCandidate* src, size_t lo, size_t hi,
                          const DirectX::XMFLOAT3& camPos,
                          const DirectX::XMFLOAT3& camForward,
                          DynPatchLaneResult& r) const;
    bool     KickDynPatchAsync(World& world, const DirectX::XMFLOAT3& camForward);
    // Joins the jobs. `use`: append the rows to `candidates` and count the frame
    // (true returned), or discard them (false returned, nothing counted).
    bool     JoinDynPatchAsync(std::vector<DrawCandidate>& candidates, bool use);

    // ---- BuildRenderScene static / dynamic split (-brssplit, default off) ----
    // The retained candidate cache is the static half. Every row in it survived a
    // frame with no membership- or appearance-affecting event, and its only
    // per-frame maintenance is (a) re-resolving its MaterialComponent pointer +
    // matBits (pools reallocate without a tracked event) and (b) the mirror sign
    // of its world matrix. Both are camera-independent and touch only
    // renderer-owned storage, so they can run as worker jobs published from the
    // Animation phase, before the ChainPhysics OwnerJoin where the owner
    // otherwise blocks while the pool is busy, and joined at the top of
    // BuildRenderScene. The dynamic half and every camera-dependent stage
    // (classify, depth sort, emit) stay where they are, so the DrawPacket stream
    // is byte-identical by construction - one sort over the same array, one emit
    // nest.
    // Do not split the sort: TaskParallelSort leaves equivalent records in
    // unspecified order (like std::sort), and lessRecord's final tiebreak,
    // meshDescSlot, is not unique (two instances of one mesh with one material
    // compare equal), so merging two sorted streams cannot reproduce the single
    // sort's tie order - and the instance buffer bytes depend on it.
    // Guard (see PrepareStaticSceneEarly): the MaterialComponent pool's storage
    // pointer + dense pointer + size are captured at publish and re-checked at
    // join; any Add or Erase moves at least one of them. The residual hole is an
    // equal number of adds and erases on untracked entities inside the window;
    // on a tracked scene entity either lands in the scene diff and the patch gate
    // (added / removed / matDirty all empty) rejects the patch frame, discarding
    // the early result. PrepareStaticSceneEarly / JoinStaticSceneEarly are
    // public, beside PrepareAnimationVisibility - App drives both.
    // RefreshCandCacheRange: one lane of the static refresh - re-resolve mc /
    // matBits and (optionally) the mirror bit over [begin, end) of m_candCache.
    // Shared by the early worker jobs and PatchCandidateCache's in-place
    // fallback, so the two paths cannot drift.
    void     RefreshCandCacheRange(const ComponentPool<MaterialComponent>* pMaterial,
                                   size_t begin, size_t end, bool writeUnchanged,
                                   bool doMirror, bool& valid,
                                   uint32_t& ptrWrites, uint32_t& bitWrites);
    // sign(det3(worldMatrix)) -> mirrored. One definition, so the early job, the
    // patch fixup and the serial mirror loop cannot produce different bits.
    static uint8_t MirrorBitFor(const DirectX::XMFLOAT4X4& m);
    static bool BrsSplitEnabled();
    // Re-test the publish-time guards. true = the early lanes' output is still
    // the answer the serial path would have produced.
    bool     ConsumeStaticSceneEarly(const ComponentPool<MaterialComponent>* pMaterial);

    // ---- Dynamic-only adds ----
    // ClassifySceneAdds (right after BuildScene_DirtyDiff, and again for
    // matDirty after SyncMaterials): this frame's diff added entities and removed
    // none, every added entity joins the dynamic lane (SceneItemKey::lane 1:
    // skinned or dissolving - gathered every frame anyway; skinnedOnly: all
    // skinned, the DDGI static gather's own exclusion), and the frame's
    // dirtyOther / matDirty entries are all the adds' own (an add is dirtyOther,
    // and matDirty once SyncMaterials binds it). Such a frame keeps the static
    // caches: the BVH takes the new leaves beside its tree, the candidate cache
    // patches and re-gathers only its dynamic rows, DDGI patches its static blob.
    // -gather_noaddpath: never (prefab_spawn_gather's reverse arm).
    void ClassifySceneAdds(World& world);
    void ClassifySceneAddsMaterials();

    // ---- Retained BVH leaves + retained tag sets ----
    // O(dirtyXform x depth) invariant check: each moved entity's true
    // membership (a tagged ancestor-or-self exists, recomputed from the live
    // hierarchy with memoization; face overlay resolves the nearest ancestor's
    // params) compared against last frame's retained sets. The full ancestor
    // chain is required - a parent-only check against the retained set misses
    // subtrees reparented through a non-renderable (untracked) pivot node. A
    // violation = a subtree crossed a set boundary -> rebuild.
    // Residual gap: a reparent whose recomputed world matrix is bit-identical
    // produces no diff event; membership self-heals on the next membership event
    // (accepted: the inverse-parent float round-trip makes bit-identity a
    // measure-zero case).
    bool     BuildScene_TagSetBoundaryStable(World& world) const;
    // The reference implementation (per-call unordered_map memo), kept for
    // -tagboundaryverify / -tagboundaryhash.
    bool     BuildScene_TagSetBoundaryStableRef(World& world) const;

    // ---- Gather / cull stages (run in sequence inside BuildRenderScene) ----
    // Like BuildScene_ComputeHairShadowSet, for the brows / lashes subtree
    // (FaceOverlayComponent roots) -> the through-hair overlay. Fills
    // m_faceOverlayParams (per-entity params).
    void BuildScene_ComputeFaceOverlaySet(World& world);
    // Same, for DissolveComponent subtrees -> per-instance dissolve params.
    // Fills m_dissolveParams (entities with amount <= 0 are skipped entirely).
    void BuildScene_ComputeDissolveSet(World& world);
    void BuildScene_SyncMaterials(World& world);   // material tex sync + NPR ramp + deferred release
    void BuildScene_CullBVH(World& world,
                            std::vector<uint8_t>& bvhVisibleMask,
                            std::vector<uint8_t>& bvhTestedMask); // Phase 0.5 BVH + frustum cull
    void BuildScene_DebugWireframes(World& world); // debug overlay (independent of the gather)
    // dynamicOnly (patch frames): lights always gather; mesh entities already
    // restored from m_candCache are skipped wholesale.
    void BuildScene_GatherPrimitivesAndLights(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward,
                            bool dynamicOnly); // primitives and lights share one entity loop
    // deferCull (retained mode): skip the in-gather frustum cull +
    // shadow-fallback split - BuildScene_ClassifyCandidates applies it
    // post-gather, so candidates stay a cacheable pure function of mesh /
    // material / visibility.
    void BuildScene_GatherMeshLibRefs(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const std::vector<uint8_t>& bvhTestedMask,
                            const std::vector<uint8_t>& bvhVisibleMask,
                            const DirectX::XMFLOAT3& camForward,
                            bool deferCull, bool dynamicOnly);
    void BuildScene_GatherBillboards(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_GatherBeams(World& world,
                            std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    void BuildScene_GatherAfterimages(std::vector<DrawCandidate>& candidates,
                            const DirectX::XMFLOAT3& camForward);
    // ---- Sort and emit: the PlanEmit / EmitBatches halves ----
    // A thin wrapper over the two halves below: it owns the "BS.SortAndEmit"
    // Tracy zone and nothing else.
    void BuildScene_SortAndEmit(const std::vector<DrawCandidate>& candidates);
    // Main's share of a lane plan (the latches UploadFrameData and the packet
    // read before the hand-off, the atlas).
    void PrepareLanePlan(const std::vector<DrawCandidate>& candidates);
    // PlanEmit = sort + preamble + ResolveBatchPlan, ending at the
    // EmitZoneMark(8) hand-off point with m_emitCtx fully written; EmitBatches =
    // the emit nest + the post-nest tail, a pure consumer of it.
    void BuildScene_PlanEmit   (const std::vector<DrawCandidate>& candidates);
    void BuildScene_EmitBatches(const std::vector<DrawCandidate>& candidates);
    // EmitBatches + its zone, its brsEmitMs bracket and its oracle. One body, so
    // the main-thread path and the lane are instrumented identically.
    void RunEmitHalf(const std::vector<DrawCandidate>& candidates);
    // The four things that cross the PlanEmit / EmitBatches seam as code rather
    // than data (this and the three below). SameEmitBatch captures nothing and
    // touches no member; a static member only because DrawCandidate is nested in
    // Renderer.
    static bool SameEmitBatch(const DrawCandidate& first, const DrawCandidate& cur);
    void EmitZoneMark(int slot);        // advances the [EMITZONE] bracket
    void FinishSortEmitZones();         // reports the [EMITZONE] frame
    void EnsurePlanPrefix(uint32_t candidate);   // the plan prefix, driven from both halves
    // -brslaneverify's payload: a digest of everything EmitBatches produced,
    // computed where it finished (the definition says why the output is digested
    // rather than the input replayed).
    // @p leasedEmitIdx: the scene-payload index this frame leased, from a source
    // other than the publish being checked (the packet on the lane arm, the
    // pre-publish write index on the main arm) - see EmitHalfInvariantMask.
    void ReportEmitHalfDigest(uint64_t frame, bool onLane, uint32_t leasedEmitIdx);

    // Its four invariants as a bit mask, no logging - so -brslaneselftest can
    // feed it the wrong answer and observe a quiet "no". Same predicate/check
    // split as EmitLeaseMismatch.
    uint32_t EmitHalfInvariantMask(bool onLane, uint32_t leasedEmitIdx) const;

    // ---- Post-gather phases ----
    void BuildScene_SyncTerrain(World& world);     // Phase 0b
    void BuildScene_SyncGrass  (World& world);     // Phase 0b.1 - after SyncTerrain
    void BuildScene_SyncWater  (World& world);     // Phase 0b.2 - after SyncTerrain

    // What the light prep reads that main may move on from: the FrameView as it
    // stood at the prep's position in BuildRenderScene (last frame's view - the
    // spot score reads view.cameraPosition - and this frame's camera, with the
    // viewport size UploadFrameData has not yet rewritten), the frame's upload
    // slot and the frame epoch (log cadence).
    LightPrepInputs CaptureLightPrepInputs() const;
    // `clusterMetaDone`: main already ran ClusterPass::SetLightsMeta for this
    // frame's list (the lane form), so only the records are uploaded here.
    void BuildScene_PrepareLightData(const LightPrepInputs& in, bool clusterMetaDone);
    void BuildScene_UploadLights(World& world, bool dataPrepared = false); // Phase 4 resources

    // -lightprepdigest=N: every Nth frame, one hash of everything the tracking
    // and the prep produced ([LIGHTPREP_DIGEST]), from whichever thread ran
    // them - the two arms' logs are compared line for line.
    void LogLightPrepDigest(const LightPrepInputs& in, const char* where);
    // The probe upload is split in two. The select half - the bake feedback, the
    // stable slots, the disk restore, the order, the bake queue and the bake
    // request - runs at the top of BuildRenderScene beside ExtractPreviewCameras,
    // because the classify needs the probe baking this frame (its faces are a
    // secondary volume: m_probeCullActive). The upload half packs the GPU
    // records at Phase 5. Both read only the probe components and their
    // transforms, which nothing between the two writes.
    void BuildScene_SelectProbes(World& world);    // top of BuildRenderScene
    void BuildScene_UploadProbes(World& world);    // Phase 5

    void BuildScene_UpdateDDGI  (World& world);    // Phase 6
    // The scene's DDGI / SSGI intent into the settings store's Scene layer, when
    // it changed (main thread - the scene build).
    void RecordSceneIndirectIntent(bool sceneHasComponent);
    // The same for the scene's fog intent (GatherVolumetricFog).
    void RecordSceneFogIntent(bool intent, std::string_view writer);
    // Bake at most one queued probe per frame (cost: 6 forward + 42 prefilter).
    void ProcessProbeBakeQueue(RHI::CommandList colorLastCL);

    // ---- Core ----
    IGraphicsDevice& m_gfx;
    MeshManager      m_meshMgr;
    // Const after construction and not a seam lane: its only reader is
    // m_graph.Execute on the render half, and its only writer, SetClearColor
    // above, has no callers - nothing to publish. Do not confuse it with
    // FrameView::hdrClearColor, the HDR clear SyncSkyboxIBL sets every frame (a
    // real game-to-render value).
    float            m_clearColor[4]{ 0.0f, 0.0f, 0.0f, 1.0f };

    // Authored sun colour BEFORE atmosphere transmittance (top-of-atmosphere).
    // LightCB.lightColor holds the filtered ground-level sun that shading uses;
    // the sky-view and cloud passes integrate transmittance along their own
    // paths and must be handed this untinted value instead, or the extinction
    // gets applied twice and the sky goes muddy. Written by SetLightCBSun.
    DirectX::XMFLOAT3 m_sunColorTOA{ 0.0f, 0.0f, 0.0f };

    // ---- The frame's view state, double-buffered ----
    // State, not a per-frame payload: several game-half readers consume the
    // previous frame's value on purpose (BuildScene_ClassifyCandidates and the
    // BVH cull read m_view before UploadFrameData rewrites it; see
    // BuildRenderScene). A plain flip would hand them frame F-2 instead of F-1,
    // because the write buffer at F was last written at F-2, so the publish
    // seeds the new write buffer with what it just published (one ~600-byte copy
    // per frame). FrameView itself: Renderer/FrameView.h, so a render feature
    // reads it without this header.
    FrameView m_frameViewBuf[2];
    // A secondary view's frame for its segment (FrameViewRead), by slot ([0] unused): the read
    // frame with the view's camera, sizes and RenderView.
    FrameView m_viewFrames[RHI::kMaxViews];
    // A post-chain ping-pong pool: two RGBA16F targets at the post size (see m_ppPool).
    struct PostPool
    {
        RG::PassTexture tex[2] = {
            RG::PassTexture(RG::PassTextureDesc{ .format = RHI::Format::R16G16B16A16_FLOAT,
                                                 .size   = RG::SizePolicy::PostAA }, "PostProcessPool0"),
            RG::PassTexture(RG::PassTextureDesc{ .format = RHI::Format::R16G16B16A16_FLOAT,
                                                 .size   = RG::SizePolicy::PostAA }, "PostProcessPool1") };
        uint32_t w = 0, h = 0;
        bool Ready() const { return tex[0].IsValid() && tex[1].IsValid(); }
    };
    // The secondary views by slot ([0] unused; Renderer_Views.cpp). live / pendingRelease are the
    // main thread's; the rest is made and given back at the frame boundary (ServiceViews) and used by
    // the view's RenderAsync call.
    struct SecondaryView
    {
        std::unique_ptr<ViewState> state;
        std::string                name;
        uint32_t                   lease          = 0;
        std::atomic<uint64_t>      renders{ 0 };
        uint32_t                   width          = 0;   // its HDR target's size (0: none yet)
        uint32_t                   height         = 0;
        bool                       live           = false;
        bool                       pendingRelease = false;
        bool                       ready          = false;   // its resources exist
        uint32_t                   breaks         = 0;       // HistoryReset bits for its next segment
        // What SetViewCamera asked for (main thread, read on the game half).
        Entity                     cameraEntity   = NullEntity;
        bool                       followCamera   = false;
        bool                       followMain     = false;
        uint32_t                   reqWidth       = 0;
        uint32_t                   reqHeight      = 0;
        uint32_t                   reqShow        = 0;
        uint32_t                   show           = 0;       // this frame's RG::ShowFlag bits
        std::vector<DrawPacket>    drawList;                 // this frame's (CullView)
        // Its CSM fit (render half, its segment): the cascades it draws into the shared array.
        std::unique_ptr<ShadowSystem> shadows;
        // Its SSR chain (render-size textures, scene-colour pyramid, frame index; its temporal history is
        // the view's), resized and recorded in its segment; whether that pyramid was built last frame.
        std::unique_ptr<SSRSubsystem> ssr;
        bool                          pyramidBuiltLastFrame = false;
        bool                          giActivePrevFrame     = false;   // its ReSTIR GI ran last frame
        // Its post-chain ping-pong pool, at its size (made in its segment).
        PostPool                      ppPool;
    };
    SecondaryView m_secondary[RHI::kMaxViews];
    // Released views' SSR chains and the frame boundaries left until nothing in flight reads their alias
    // heaps (ServiceViews counts down).
    std::vector<std::pair<std::unique_ptr<SSRSubsystem>, uint32_t>> m_retiredViewSSR;
    // The segment in flight (BeginViewSegment / EndViewSegment, render half): its recording-view
    // scope, the main view's plan it replaced, whether it pushed its HDR target.
    std::optional<RHI::ViewScope> m_viewScope;
    FramePlan::Decisions          m_mainPlanDuringView;
    bool                          m_viewHdrPushed = false;
    // RenderViewsAsync: the list a secondary view's post is ordered behind (the main view's, then the
    // previous view's last); the main view's DDGI promote list this frame, which a secondary view's
    // lighting waits on as the main graph's does.
    RHI::CommandList              m_viewAfter{};
    RHI::CommandList              m_framePromoteCL{};
    uint32_t                      m_viewAttributed = 0;   // the attribution a view segment replaced
    // The main view's acceleration structure this frame (built on its call): what a secondary view's traced
    // passes trace.
    struct FrameRayTracing
    {
        uint64_t tlasVA        = 0;
        uint64_t instanceVA    = 0;
        uint64_t alphaVA       = 0;
        uint32_t alphaCount    = 0;
        uint64_t bindlessTable = 0;
        uint64_t lightsSrv     = 0;
    };
    FrameRayTracing               m_frameRt{};

    // The settings snapshot (Settings/SettingsSnapshot.h), double-buffered on
    // the same index as the FrameView: the hand-off fills the frame in flight's
    // buffer from the settings store, on main, refilling only a buffer the store
    // has moved on from. SettingsRead has no caller; the harness reads
    // SettingsPublished (frames.jsonl).
    Settings::Snapshot m_settingsSnap[2];
    uint32_t           m_settingsSnapLast = 0;   // the buffer the last hand-off filled

    // ---- Core ----
    // See BuildGameView. Written before BeginFrame; m_view is written inside it.
    RenderView       m_gameView;
    // Extracted by ExtractShadowAtlasDemand, consumed by
    // ServiceEnableGatedResources. Two bools rather than a pool scan, so the
    // service half needs no World.
    bool             m_wantSpotAtlas  = false;
    bool             m_wantPointAtlas = false;
    RG::RenderGraph  m_graph;
    // Who is shut down, hot-reloaded, reset on a hard cut or a resize, and told
    // about a world boundary: each pass and system registers where Compile
    // creates it (Renderer/RenderLifecycle.h).
    RenderLifecycle  m_lifecycle;
    PassSet          m_passes{m_lifecycle, m_gfx};   // registers passes' lifecycle entries
    // The render features (Renderer/RenderFeature.h): created at the top of
    // Compile, called at the insertion points. Declared after m_passes, so they
    // are destroyed first - nothing calls a lifecycle hook during destruction.
    // FrameTextures are the graph handles they see (Compile), FrameResources what
    // they exchange with the Renderer each frame.
    RenderFeatureRegistry m_features;
    FrameTextures         m_frameTextures;
    FrameResources        m_frameResources;
    // The frame plan (Renderer/FramePlan.h), render half only: the state read at
    // its top, the facts learned so far, the plan its decision sites read (the
    // latest stage) and the plan [FRAMEPLAN] last printed.
    FramePlan::State     m_planState;
    FramePlan::Facts     m_planFacts;
    FramePlan::Decisions m_plan;
    FramePlan::Decisions m_planPrinted;

    // The game half's plan: GamePlan(), and the resolve at this frame's viewport
    // (App's) and render size (the device's) - fsrResolved / dlssResolved / the
    // post size - for ServiceEnableGatedResources.
    FramePlan::Decisions m_gamePlan;
    FramePlan::Decisions m_planRendered;   // LastRenderedPlan: main's copy of the finished m_plan
    bool                 m_planRenderedFog = false;   // LastRenderedFog: m_planState.fog, copied with it

    // Frames planned, plan changes printed, and the frames a path-tracing path
    // fell back to the raster features or had its path tracer vetoed.
    uint64_t             m_planFrames = 0, m_planChanges = 0, m_planFallbacks = 0, m_planVetoes = 0;
    uint32_t             m_planEventLogs = 0;   // fallback / veto lines printed (capped)
    // The live WorldCull preview views, counted on the game half after
    // ExtractRequests (they borrow the main CSM - a path-traced frame's reader).
    uint32_t             m_planPreviewWorldViews = 0;
    World*           m_lastWorld    = nullptr;
    // Extracted once per frame by SyncSkyboxIBL (game side) and read by the
    // lens-flare horizon gate, which runs on the post-process render worker - a
    // bool, so that worker needs no World read.
    bool             m_todMoonActive = false;
    // The scene payload is double-buffered. BuildRenderScene clears the draw list
    // and SortAndEmit appends to it on the game half, while
    // Render_BindFrameResources hands the graph a borrowed raw pointer to it
    // (m_graph.SetDrawList) that every geometry pass iterates during Execute on
    // the render half. With one buffer, the next frame's push_back reallocating
    // would free the storage the passes are walking - a use-after-free. DrawPacket
    // is POD (RenderTypes.h) and the two buffers alternate, each keeping its
    // capacity, so steady state is allocation-free.
    // One index pair for the whole scene payload, not one per container: every
    // buffer below describes the same frame and is published at the same
    // instant, so separate indices could only drift apart and pair a draw packet
    // with another frame's AABB or remap entry.
    uint32_t                m_sceneWrite = 0;   // game half builds here
    uint32_t                m_sceneRead  = 0;   // render half reads here
    // The index the emit path resolves the scene payload through. Latched from
    // m_sceneWrite at the head of BeginFrame and equal to it until
    // AdvanceSceneWrite (which runs last), so with the lane off the two are
    // interchangeable. With PREPARE on the render-owner lane, main advances
    // m_sceneWrite at hand-off and immediately writes FrameViewWrite() for F+1
    // (SetCamera, BuildGameView) while the lane finishes frame F's payload;
    // without this index the lane's frame would land in the F-2 buffer. Each
    // index has one writer: main owns m_sceneWrite, the lane owns m_sceneRead,
    // and m_emitIdx names the buffer the frame in flight belongs to.
    uint32_t                m_emitIdx    = 0;
    std::vector<DrawPacket> m_drawPacketsBuf[2];

    // ---- FrameStats: one writer per field, and a publish across the seam ----
    // m_frameStats is main's copy and only main writes it. Two kinds of field:
    //   main-owned (written in place by main):
    //     triScene / triSceneValid   - BuildRenderScene, before the emit half
    //     worldUIVerts               - ExtractWorldUI, after BeginFrame returns
    //     previewViewsRendered/Draws - ApplyPreviewFeedback, on the game half
    //   emit-owned (the triangle census, the packet / instance / light counters,
    //     the GPU-visibility readback and the three material counters): the emit
    //     half fills m_emitCtx.stats and PublishEmitStats hands it over.
    //     FoldEmitStats is the one place that list exists.
    // Why: the census is zero-then-accumulate, and -brslane runs the emit nest on
    // the render-owner lane while main's readers (debug HUD, Lua Debug.Stats, the
    // frames.jsonl row) are already in the next frame; writing m_frameStats from
    // there would be a data race on non-atomic uint64s that reads torn totals.
    // So nothing off-main writes m_frameStats: the emit half owns a private copy
    // for its whole length and hands it over at one point.
    FrameStats              m_frameStats;   // see GetFrameStats()

    // The hand-over: two slots plus a release-stored sequence, because at most
    // one publish can be outstanding - the consume runs at the end of
    // BuildScene_SortAndEmit (main, both arms) and the next publish cannot happen
    // until the lane has been handed frame F+1, which BeginFrame(F+1)'s
    // publication guard already orders after it. The ENGINE_CHECK in
    // PublishEmitStats enforces this.
    // emitMs travels with the stats: brsEmitMs is the second diagnostic the emit
    // half writes that main reads (Diag::FrameTiming is a main-read singleton),
    // so it takes the same route instead of a second off-main store.
    EmitStatsSlot           m_emitStatsSlot[2];
    std::atomic<uint64_t>   m_emitStatsSeq{0};      // publishes, release-stored
    // Main's cursor. Atomic only so PublishEmitStats (on the lane under
    // -brslane) can read it for its one-outstanding-publish check.
    std::atomic<uint64_t>   m_emitStatsConsumed{0};

    // ---- Per-frame light build state (Phase 0 -> Phase 4) ----
    std::vector<ResolvedLight>               m_frameLights;
    uint32_t                                 m_lightGatherSeq = 0; // LightData::gatherSeq source

    // Entity-id-indexed mirror of "has a m_entityMaterialRuntimeBindings
    // entry": SyncMaterials' steady state does one byte load per material per
    // frame instead of an unordered_map probe (thousands of hash + chain walks in
    // a city scene). The map stays authoritative (the debug self-heal still
    // consults it).
    std::vector<uint8_t>                     m_matBoundBits;
    // Cleared and refilled by BuildScene_GatherPrimitivesAndLights /
    // BuildScene_UploadLights on the game half, and handed to VolumetricFogPass
    // by Render_BindFrameResources on the render half. Double-buffered for the
    // same use-after-free as the draw packets: the reserve at refill time
    // reallocates under the reader.
    std::vector<VolumetricFogPass::VolLight> m_volLightsBuf[2];
    VolFogSceneState                         m_volFogSceneBuf[2];

    // Game -> main: the scene half of the froxel resource gate
    // (VolumetricFogPass::FogDemand, packed as kFogDemand* bits). Written by
    // SyncSkyboxIBL, read by ServiceEnableGatedResources at the top of the next
    // game half - the same one-frame lag as every enable-gated pool.
    std::atomic<uint8_t>  m_volFogDemandBits { 0 };
    // Editor readout: local fog volumes enabled in the scene / kept after the
    // view-distance cull + cap (last game half).
    std::atomic<uint32_t> m_localFogTotal    { 0 };
    std::atomic<uint32_t> m_localFogKept     { 0 };
    bool  m_sunVolumetric      = false;  // any directional+VolumetricLightComponent
    float m_sunVolumetricScale = 1.0f;
    float m_sunMediumScale     = 1.0f;   // the sun's scattering in the path tracers' medium (ResolvedSceneLight)
    ShadowFrustumCompute m_shadowFrustum; // CSM frustum OBB + plane caches
    // The secondary views' cascade volumes, by slot, for caster retention: fitted on the game half
    // (UploadFrameData, FitViewShadowRetention) into m_viewShadowFit, and taken into
    // m_viewShadowFrustum where m_shadowFrustum is computed - a frame ahead of their use, as the main
    // view's are, and past the drain of the lane that reads them.
    ShadowFrustumCompute m_viewShadowFit[RHI::kMaxViews];
    bool                 m_viewShadowFitAny = false;
    ShadowFrustumCompute m_viewShadowFrustum[RHI::kMaxViews];
    bool                 m_viewShadowRetention = false;   // any of them valid

    // ---- Runtime toggles ----
    bool m_ssaoEnabled        = true;   // m_ssaoAuthored && m_ssaoSceneIntent
    // Authored (player) mirrors - see SetSSAOAuthored. Seeded to the same
    // defaults, so a build with no settings.json captures the values it would
    // have applied.
    bool m_ssaoAuthored       = true;   // matches EngineSettings::ssao's default
    bool m_volFogAuthored     = true;   // matches EngineSettings::volumetricFog's default
    // The scene's .ippc intents (SetSSAOSceneIntent / SetFogSceneConfigIntent):
    // PostProcessConfig's defaults until a scene load says otherwise.
    bool m_ssaoSceneIntent    = true;
    bool m_fogSceneConfig     = false;
    std::string m_fogConfigWriter = "no .ippc (its defaults)";
    // The scene's whole fog intent (RecordSceneFogIntent, game half) and what the
    // settings store's Scene layer last got for it (0 off, 1 on, 0xFF nothing yet).
    std::atomic<bool> m_sceneFogIntent { false };
    uint8_t     m_fogIntentRecorded = 0xFF;
    std::string m_fogIntentWriterRecorded;
    bool m_ddgiAuthored       = true;   // matches EngineSettings::ddgi's default
    bool m_ssgiAuthored       = true;   // matches EngineSettings::ssgi's default
    // The scene's IndirectLightingSettings intent (SetDDGIAuthored): the
    // component's defaults until the first scene build reads a world.
    bool m_sceneDdgiIntent    = true;
    bool m_sceneSsgiIntent    = false;
    // What the settings store's Scene layer last got (RecordSceneIndirectIntent):
    // bit 0 DDGI, bit 1 SSGI, bit 2 "the scene has the component"; 0xFF = nothing yet.
    uint8_t m_sceneIntentRecorded = 0xFF;
    bool m_ssrEnabled         = true;
    // Render -> game, the only cross-half lane here that decides work rather
    // than a displayed number. The budget watchdog in Renderer::Render clears
    // these after ~300 sustained over-budget frames, on the render half; the next
    // frame's intent (CaptureFrameIntent) reads them, and the plan's TLAS demand
    // decides from it whether the game half gathers the TLAS at all. Atomic
    // because two threads touch one bool; relaxed because the flags are
    // independent one-way latches with no other state ordered against them, so
    // seeing a flip a frame late costs one frame of unused gather.
    std::atomic<bool> m_rtReflectionsEnabled { false };  // RTR default off (TDR budget)
    std::atomic<bool> m_transparentReflectionsEnabled { true };  // LightCB.fwdReflStrength (features.transparentReflections)
    // Whether the post-graph SSR chain rebuilt the scene-colour pyramid last
    // frame - exactly when its content is a valid prev-frame radiance source for
    // this frame's translucent screen march. Written post-graph, read in the
    // next frame's LightCB resolve() block (both on the render half). Never name
    // a stale-but-existing pyramid.
    bool m_sceneColorPyramidBuiltLastFrame = false;
    float m_rtrMaxRoughness      = 0.55f;  // RTR trace gate (GGX band)
    float m_rtrRayTMax           = 150.0f; // RTR HW ray length + miss/sky hitDist sentinel
    bool  m_rtShadowBlendOccluders = true; // glass casts a PARTIAL RT sun shadow
    bool  m_rtrUseNRD            = true;   // NRD (REBLUR_SPECULAR) denoiser by default
    bool  m_rtaoEnabled          = false;  // RTAO source swap for XeGTAO (needs DXR + SSAO on)
    RTSwitchRequests m_rtRequested;        // the three RT switches as asked for (main thread: the settings apply)
    std::atomic<bool> m_rtShadowsEnabled { false };     // RT sun-shadow mask (needs DXR + a sun); see the lane note above
    bool m_useIndirectDraw    = true;
    bool m_gpuCullingEnabled  = true;
    // Hi-Z mip chain, consumed by CullingPass's per-command occlusion test.
    // Generated at the end of the frame from the depth buffer for the next
    // frame's cull (~13 small dispatches). Runs only while gpuCulling is also
    // enabled (generation is gated on both flags).
    bool m_hiZEnabled         = true;

    // Default OFF: light icons are an editor debug gizmo driven by the editor's
    // DebugDrawSystem (Lights debug category), which sets this true each frame.
    // Game builds never touch it, so light icons are stripped from shipping.
    bool m_lightBillboardsVisible = false;
    // Desired CSM per-cascade resolution; consumed by ShadowPass at Compile().
    uint32_t m_shadowResolution = 4096;
    // shadows.distance (read every frame by the cascade fit), the spot-shadow
    // tier cap (the per-frame tier pick), the desired point-shadow face size
    // (PointShadowPass at Compile()), features.aoQuality (pushed to XeGTAOPass
    // every frame) and geometry.lodScale (times both meshlet LOD taus at emit).
    float    m_shadowDistance        = kDefaultShadowDistance;
    uint32_t m_spotShadowMaxTier     = 2;
    uint32_t m_pointShadowResolution = 1024;
    int      m_aoQuality             = 2;
    float    m_meshletLodScale       = 1.0f;
    // Desired material-texture anisotropy; consumed by sampler-owning passes at
    // Compile(). 1 = trilinear, 2..16 = anisotropic.
    uint32_t m_textureAnisotropy = 8;

    // ---- Viewport / window tracking ----
    // Display (output) res = viewport / swap chain (FrameView::vpWidth /
    // vpHeight): ToneMap output, UI and the post after an upscaler are sized to
    // it. Render res = internal scene res (FrameView::renderWidth / renderHeight,
    // upscaler ratio and optional display.renderScale applied): HDR scene RTs,
    // GBuffer, TAA, SSR, XeGTAO. UploadFrameData reads it from the device's HDR
    // target. The m_last* members are the sizes last seen.
    uint32_t m_lastWindowW = 0;
    uint32_t m_lastWindowH = 0;
    uint32_t m_lastRenderW = 0;
    uint32_t m_lastRenderH = 0;
    // The render / display widths the material samplers' LOD bias was last
    // built for (ApplyUpscaleMipBias).
    uint32_t m_mipBiasRenderW  = 0;
    uint32_t m_mipBiasDisplayW = 0;
    UpscaleMode m_upscaleMode = UpscaleMode::Native;

    // ---- Per-entity prev-frame world matrix (TAA velocity) ----
    // The GBuffer VS computes the prev-frame clip position for the velocity
    // buffer; the skinned path reads prev positions in mesh-local space
    // (Skin.cs.hlsl outputs bone-deformed, un-world-transformed positions), so
    // projecting them through prevViewProj alone misses the entity's prev-frame
    // world transform. Tracked CPU-side and fed via GPUInstanceData.prevWorld.
    // Dense arrays indexed by entity id (valid byte = id seen before); an unseen
    // id falls back to the current world (zero velocity).
    std::vector<DirectX::XMFLOAT4X4> m_prevWorldDense;
    std::vector<uint8_t>             m_prevWorldValid;

    // ---- Per-frame upload buffers (UPLOAD heap, persistently mapped) ----
    // kFrameSlots deep, so pipelined frame pacing (the CPU runs ahead via
    // GraphicsDX12::BeginFrame) does not race CPU writes against in-flight GPU
    // reads. CBs use FrameCB<T>; SRV / staging upload buffers use manual
    // per-frame arrays (FrameCB hard-codes the CONSTANT_BUFFER bind flag).
    // Pool capacities: runtime values, seeded from RendererPoolCaps by the
    // constructor. The instance buffer is m_maxInstances x
    // sizeof(GPUInstanceData) (quote the static_assert in GPUInstanceData.h, not
    // a size). The in-class initialisers read RendererPoolCaps{} so the default
    // lives in one place; the constructor overwrites them either way.
    uint32_t m_maxInstances = RendererPoolCaps{}.maxInstances;
    uint32_t m_maxMaterials = RendererPoolCaps{}.maxMaterials;

    FrameCB<RendererDetail::PerViewCB>       m_perObjectCB;
    FrameCB<RendererDetail::LightCB>         m_lightCB;
    FrameCB<GpuShared::TerrainParams> m_terrainCB;
    // The fog the opaque scene got, for everything drawn after the fog
    // composites (Graphics/FogApply.h). Written on the render half
    // (WriteFogApplyCB), registered with the graph as "FogApply" (b4).
    FrameCB<FogApply::Constants>             m_fogApplyCB;
    // The soft fade particles / trails / billboards get against the opaque
    // depth, in metres. -softfade_range=<m> (harness) overrides it; 0 turns it
    // off.
    float                                    m_softFadeRange = 0.3f;
    // [FWDFOG] is printed when what the forward fog applies changes.
    uint32_t                                 m_fogApplyLogKey = ~0u;
    // The fog as one participating medium for the path tracers (Graphics/MediumSet.h),
    // uploaded on the render half (UploadMediumSet): this frame's heap index (0 = no
    // medium), the local volumes' scratch, and what [MEDIUM] last printed.
    Media::SetBuffer                         m_mediumSet;
    uint32_t                                 m_mediumIdx = 0;
    std::vector<Media::Local>                m_mediumLocals;
    uint32_t                                 m_mediumLogKey = ~0u;

    // ---- Meshlet path ----
    // MeshletCullCB (b4): written on the render half (Current()), registered
    // with the graph as "MeshletCull", bound by GBufferPass with the mesh PSO.
    FrameCB<MeshletShared::MeshletCullCB>    m_meshletCullCB;

    // One MeshletCullCB per CSM cascade (ShadowPass binds b4 at cascade * 256),
    // written by RecordShadowFamilyCull.
    FrameCB<MeshletShadowBanks>              m_meshletShadowCB;
    bool     m_meshletsRequested = false;  // engine.json culling.meshShading; -meshlets=0|1 pins it
    bool     m_meshletsPinned    = false;  // -meshlets given: engine.json / the editor do not override it
    bool     m_meshletsKilled    = false;  // -meshletsoff: the whole path, no mesh PSO is built
    int64_t  m_meshletsAtFrame   = -1;     // -meshlets_at=<frame>:<0|1>
    bool     m_meshletsAtValue   = false;
    // The amplification shader's culling stages (kMeshletCull* bits; the stats
    // bit is added when the counters exist). Default all three: frustum, cone
    // and Hi-Z.
    uint32_t m_meshletCullMask   = MeshletShared::kMeshletCullFrustum | MeshletShared::kMeshletCullCone |
                                   MeshletShared::kMeshletCullHiZ;
    int64_t  m_meshletCullAtFrame = -1;    // -meshletcull_at=<frame>:<mask>
    uint32_t m_meshletCullAtValue = 0;
    // LOD threshold tau: pixels of projected error; <= 0 = level 0.
    // engine.json culling.meshletLodError; -meshletlod=<tau> pins it,
    // -meshletlod_at=<frame>:<tau> switches it later.
    float    m_meshletLodTau      = 1.0f;
    bool     m_meshletLodPinned   = false;
    int64_t  m_meshletLodAtFrame  = -1;
    float    m_meshletLodAtValue  = 0.0f;
    // The CSM's tau in texels; engine.json culling.meshletShadowLodError.
    // -meshletshadowlod=<tau> pins it, -meshletshadowlod_at=<frame>:<tau>
    // switches it later.
    float    m_meshletShadowLodTau = 1.0f;
    bool     m_meshletShadowLodPinned  = false;
    int64_t  m_meshletShadowLodAtFrame = -1;
    float    m_meshletShadowLodAtValue = 0.0f;
    uint32_t m_meshletLogEvery   = 0;      // -meshletlog_every=N
    bool     m_meshletVerify     = false;  // -meshletverify
    // The last [MESHLET] frame line's path + reason, to log every change.
    std::string m_meshletLastLogKey;

    RHI::GPUBuffer m_instanceBuffer       [kFrameSlots];
    void*          m_instanceBufferMapped [kFrameSlots] = {};

    // ---- Stable instance slots + one DEFAULT instance buffer (retained) ----
    // Retained mode, instead of compacting and rewriting the UPLOAD instance
    // buffers every frame, has: a stable per-entity slot allocator (the
    // PersistentMatSlot template: free list + high-water + destroy-listener free
    // + world-clear reset + frames-in-flight deferred reuse), one DEFAULT-heap
    // buffer holding the records at those slots, and a per-frame dirty staging
    // pass - only changed records are written (into m_instanceBuffer[frameSlot],
    // used as the staging ring; no extra upload memory) and copied by coalesced
    // CopyBufferRegion ranges at the top of Render(). Draws keep draw-local
    // instanceOffsets; a per-frame InstanceRemap buffer (u32 per draw-local
    // index) maps them to stable slots in the VS (identity when the PerViewCB
    // heap index is 0 - see gpu_instance.hlsli). prevWorld follows a 2-frame
    // rule: the move frame writes prev = old world, the settle frame re-writes
    // prev = world so TAA velocity returns to zero when motion stops.
    // Dynamic-lane candidates (skinned / billboard / beam / afterimage /
    // dissolving / untracked) get per-frame slots in a reserved trailing
    // segment, rewritten wholesale. m_instanceSlotToEntity is persistent (slot
    // reuse deferred >= frames in flight, so the picking readback maps the right
    // entity).
    // Queue safety: every consumer of the instance buffer (GBuffer / shadow /
    // outline / picking draws, CullingPass, SceneVoxelPass, probe bake) runs on
    // the graphics queue, and the dirty-copy CL is created before all of them
    // (pool order == submit order for dependency-free CLs), so same-queue
    // ordering covers the CopyBufferRegion hazard. DDGI / RTR async compute reads
    // DDGISceneAS's own instance buffer, never this one. Do not let a
    // compute-queue consumer read this buffer mid-copy: extend the 3-fence DDGI
    // sync (AddCommandListDependency on the copy CL) first. Flag off = the
    // non-retained path, kept permanently (like m_retainMaterialSlots).
    bool           m_retainInstances     = true;
    bool           m_instanceRetainReset = true;   // world clear / flag flip / desync
    bool           m_instanceRetainWasOn = false;  // detects runtime flag flips
    RHI::GPUBuffer m_instanceBufferGPU;            // DEFAULT: m_maxInstances x sizeof(GPUInstanceData)
    RHI::GPUBuffer m_instanceRemapBuffer [kFrameSlots]; // UPLOAD: m_maxInstances x u32
    void*          m_instanceRemapMapped [kFrameSlots] = {};
    uint32_t       m_instanceRemapHeapIdx[kFrameSlots] = {}; // 0 = unavailable
    // Draw-local index -> instance slot, paired index-for-index with the draw
    // packets, so it is published with them: the scene-voxel cull
    // (Render_BindFrameResources) and ProcessProbeBakeQueue index it with a
    // draw-local index taken from a DrawPacket.
    std::vector<uint32_t> m_instanceRemapBuf[2];      // this frame's drawLocal -> slot
    // Slot allocator state (dense by entity id / by slot).
    std::vector<uint32_t>        m_instSlotOfEntity;   // entity id -> slot (~0u none)
    std::vector<GPUInstanceData> m_instSlotShadow;     // CPU copy of DEFAULT content
    std::vector<uint32_t>        m_instSlotLastEmit;   // frame stamp of last static emit
    std::vector<uint32_t>        m_freeInstSlots;
    // A freed slot re-enters m_freeInstSlots only kInstSlotReuseLagFrames later:
    // the picking readback maps a texel rendered up to frames-in-flight ago, and
    // m_instanceSlotToEntity[slot] keeps the OLD entity until actual reuse.
    std::vector<std::pair<uint32_t, uint32_t>> m_instSlotsPendingReuse; // {slot, retireStamp}
    uint32_t       m_instSlotNext   = 0;           // static high-water (< m_staticInstanceCap)

    uint32_t       m_instFrameStamp = 0;           // ++ per retained SortAndEmit
    // Last m_instFrameStamp at which an entity appeared in any scene-diff dirty
    // list. Indexed by entity id; 0 = never. Lets the retained emit prove an
    // instance unchanged without reading m_instSlotShadow, which is the whole
    // cost: a random probe into a multi-MB array, per candidate, every frame.
    std::vector<uint32_t> m_entityDirtyStamp;
    // DX12_VERIFY_STAGESKIP=1 disables the skip and audits its decision against
    // the full path. Read once at startup.
    bool           m_verifyStageSkip = false;

    // Ownership-transfer queue, not a read-only handoff: the render half both
    // iterates and clears it, so it must clear only the buffer it read. The
    // alternation keeps the self-heal's meaning: at frame F the write buffer was
    // the read buffer at F-1, so non-empty when about to be filled still means a
    // frame was built and never rendered, the DEFAULT buffer missed those
    // copies, and the CPU shadows are wrong.
    std::vector<InstanceCopyRange> m_instCopyBuf[2];

    uint32_t       m_instanceStagingUsed = 0;      // records appended this frame
    uint32_t       m_instUploadRecordsPeak = 0;    // telemetry high-water
    uint32_t       m_instSlotHighWater     = 0;    // telemetry high-water

    // ---- Instance-budget truncation telemetry ----
    // BuildScene_SortAndEmit stops emitting when instIdx hits m_maxInstances.
    // These four make the truncation visible: otherwise the trailing candidates
    // produce no DrawPacket, log line or counter, and an over-budget scene looks
    // like a failed mesh load or wrong culling. The eviction order is the raw
    // sort order (see the report site in Renderer_EmitBatches.cpp); making it
    // depth-aware would be a behaviour change.
    uint32_t       m_instanceEmitHighWater   = 0;  // peak instIdx reached, ever
    uint32_t       m_instanceDropHighWater   = 0;  // worst single-frame drop
    uint32_t       m_instanceDropLastFrame   = 0;  // drops in the last built frame
    uint32_t       m_instanceOverflowFrames  = 0;  // frames that truncated at all

    // ---- The indirect arg array's own high-water ----
    // `args` is how many arg slots a frame wrote; `perPacket` is one per
    // DrawPacket, which is also what the buffer must be sized for if the
    // secondary-view skip is reverted. perPacket - args == the secondary-view
    // duplicates that burn no degenerate command.
    // `saved` is its own high-water, measured per frame (totalPackets -
    // argsWritten), with that frame's args / perPacket / secondarySkipped
    // latched beside it, so the four numbers on the line come from one frame and
    // satisfy perPacket - args == saved. Do not derive it as the difference of
    // two maxima: they come from different frames. The sizing maxima stay as
    // `argsPeak` / `perPacketPeak` - what the buffer must hold.
    uint32_t       m_indirectArgHighWater       = 0;   // argsPeak (sizing)
    uint32_t       m_indirectArgPacketHighWater = 0;   // perPacketPeak (sizing)
    // The latched frame: the one with the largest saving seen so far (ties
    // broken by the larger packet count, so a run that never saves anything -
    // boot_baseline, which has no secondary view at all - still reports a real
    // frame's args/perPacket beside saved=0 instead of two zeroes).
    uint32_t       m_indirectArgSavedHighWater  = 0;
    uint32_t       m_indirectArgSavedArgs       = 0;
    uint32_t       m_indirectArgSavedPackets    = 0;
    uint32_t       m_indirectArgSecondarySkipped = 0;
    // Sticky; a safety valve rather than a statistic. The skip is sound only
    // because the secondary filters sort last (DrawFilter is the sort key's high
    // field and SecondaryView / SecondaryViewBlend are its two largest values).
    // If the emit loop sees a non-secondary packet after a skipped one, it
    // back-fills the skipped slots with degenerate commands and sets this; from
    // then on every packet gets an arg entry for the life of the process.
    bool           m_indirectArgTailBroken      = false;
    // AcquireInstanceSlot returning ~0u: the entity falls back to the per-frame
    // dynamic segment, which is correct but re-stages that record every frame -
    // the retained-instance win is gone for it. Counted per frame because it is
    // a performance cliff, not a visual bug.
    uint32_t       m_instStaticLaneDeniedLastFrame = 0;

    // Derived from m_maxInstances in the constructor: m_maxInstances -
    // kDynamicInstanceReserve (24576 at the default).
    uint32_t m_staticInstanceCap =
        RendererPoolCaps{}.maxInstances - kDynamicInstanceReserve;

    // ---- GPU per-instance cull + compaction + per-group counts ----
    // When active, InstanceCull.cs runs in compaction mode: the CPU-written
    // upload args are read-only pristine input (SRV; the GPU never mutates them,
    // so a camera cut cannot sticky-cull, and no per-frame upload -> DEFAULT arg
    // copy is needed); per-instance survivors' stable slots are compacted into
    // the upper half of m_instanceRemapGPU (element base m_maxInstances -
    // deterministic per command, no allocator atomics); surviving commands land
    // group-compacted in m_indirectArgBuffer (the per-view working arg buffer)
    // with per-group counts bumped atomically in m_drawCountBuffer, and
    // GBufferPass passes count buffer + group offset to ExecuteIndirect. The
    // remap's lower half is a CPU-copied pristine mapping that full-count
    // consumers on the main per-view CB (picking, transparent, outline Custom
    // packets) read unchanged. The wantsMain / viewMask arg zero-out happens in
    // the CS (GPUInstanceData.viewMask / instFlags), so the pristine args are
    // view-independent (the shadow family reuses them). Every other b1 consumer
    // (shadow cascade / caster / face CBs, probe capture, fringe / hair mark)
    // keeps the pristine UPLOAD remap heap index: their draws use full instance
    // ranges. Requires retained instances: the slot indirection is what lets a
    // draw skip individual instances.
    // SceneEmit::gpuCompactionFrameActive is latched in SortAndEmit (pristine
    // args carry full counts only then) and consumed by UploadFrameData (remap
    // index choice) and Render (copy / dispatch / binding) the same frame - the
    // three stages must never disagree. Flag off = upload -> DEFAULT copy +
    // in-place zero-out CS + CPU wantsMain zero; m_useIndirectDraw off is the
    // second, fully-CPU fallback.
    bool           m_gpuCompaction            = true;
    // Survives the boot-time demotion below - see GetGPUCompactionRequested.
    bool           m_gpuCompactionRequested   = true;
    RHI::GPUBuffer m_instanceRemapGPU;            // DEFAULT: 2 x m_maxInstances x u32
    uint32_t       m_instanceRemapGPUHeapIdx = 0; // 0 = unavailable
    RHI::GPUBuffer m_cmdGroupMeta      [kFrameSlots]; // UPLOAD: m_maxInstances x u32
    void*          m_cmdGroupMetaMapped[kFrameSlots] = {};

    // ---- Shadow-family retained args + per-view GPU cull ----
    // When the frame latch (SceneEmit::shadowArgsFrameActive, armed in
    // BuildRenderScene before the classify pass, so every consumer this frame
    // reads the same value) is set:
    //   * BuildScene_ClassifyCandidates emits static packets once - the
    //     off-screen drop + DrawFilter::Shadow conversion is skipped, so the
    //     packet set does not depend on the main camera (camera orbit = zero
    //     packet churn). Off-screen main-view visibility is GPU-side (the latch
    //     requires the compaction preconditions).
    //   * SortAndEmit classifies every command into the CSM ShadowDrawGroup
    //     buckets (batch-static fields only - ClassifyShadowDrawGroup in
    //     RenderTypes.h, shared with ShadowPass's ring path) and fills
    //     m_shadowCmdMeta with groupBase<<16|bucket routing.
    //   * Render() records per-cascade InstanceCull.cs mode-2 dispatches at the
    //     head of the shadow pass's own command list (the shadow CL is pooled
    //     before the main cull CL; same-CL ordering makes the args final before
    //     ShadowPass's ExecuteIndirect). Each cascade view gets its own working
    //     arg buffer + working remap (no pristine lower half - base 0) + a slice
    //     of one shared count grid; frustum = the cascade's own planes,
    //     per-instance viewMask refines to the cascade bit, Hi-Z off, per-object
    //     casters excluded via kInstanceFlagPerObjShadow + the dispatch CB (the
    //     exclude decision lands in UploadFrameData, after the meta fill).
    //   * ShadowPass draws the 4 buckets from the per-cascade working args with
    //     GPU survivor counts, binding cascade CB bank two (byte 228 = the
    //     cascade's working-remap heap index).
    // Flag off (or a precondition missing): the latch stays false, classify
    // converts, ShadowPass builds its UPLOAD ring, and these buffers sit
    // untouched.
    // Spot and point shadows use the same per-view cull. The sliced working
    // buffers carry three families; the bases are all that keep them from
    // aliasing each other's slices:
    //   [0,                  kShadowCullViews)  CSM cascades (1 view / cascade)
    //   [kSpotCullViewBase,  +kSpotCullViews)   spot slices  (1 view / slice)
    //   [kPointCullViewBase, +kPointCullViews)  point cubes  (1 view / cube,
    //                                           shared by its 6 faces)
    // A point light is culled per cube against the axis-aligned box around its
    // sphere, not per face: the six faces share one bounding sphere, so one
    // dispatch strips the same ~90-98 % of the scene six per-face dispatches
    // would, for 1/6 of the dispatches and of the working-buffer VRAM
    // (CompactShadowCommand is plane-only, so the sphere is the 6 planes of its
    // bounding box). The per-face 90 deg frustum would cut a further ~6x off an
    // already tiny list - not worth 20 MB more and 20 more dispatches.
    uint32_t m_spotShadowRefreshCursor = 0;
    uint64_t m_spotShadowSceneRevision = 1;

    std::vector<SpotCasterState> m_spotCasterState;
    std::vector<SpotCasterBox> m_spotCasterChanges;
    std::vector<SpotCasterBox> m_spotStaticCasterChanges;
    uint64_t m_spotCasterStamp = 0;
    bool m_spotForceRefresh = false;
    bool m_spotStaticForceRefresh = false;
    uint64_t m_spotLightRevision[256]{};
    uint64_t m_spotLightStaticRevision[256]{};
    uint64_t m_spotLightLastUpdate[256]{};

    uint32_t m_spotShadowReadyHighWater = 0;

    bool           m_retainedShadowArgs       = true;
    bool           m_spotPointHonorShadowFlags = true; // see IsSpotPointHonorShadowFlags
    RHI::GPUBuffer m_shadowWorkingArgs [kShadowCullViewsTotal]; // DEFAULT: m_maxInstances x sizeof(IndirectDrawCommand) each
    RHI::GPUBuffer m_shadowWorkingRemap[kShadowCullViewsTotal]; // DEFAULT: m_maxInstances x u32 each
    uint32_t       m_shadowRemapHeapIdx[kShadowCullViewsTotal] = {}; // 0 = unavailable
    RHI::GPUBuffer m_shadowGroupCounts;      // DEFAULT: (view x bucket) u32 grid
    RHI::GPUBuffer m_shadowCountZeroUpload;  // UPLOAD: create-time-memset zero source
    RHI::GPUBuffer m_shadowCmdMeta      [kFrameSlots]; // UPLOAD: m_maxInstances x u32
    void*          m_shadowCmdMetaMapped[kFrameSlots] = {};

    // ---- The frame's emitted command layout, double-buffered ----
    // CPU metadata describing GPU rings that are kFrameSlots deep and indexed by
    // GetUploadSlot on the game half / GetFrameIndex on the render half
    // (m_indirectArgUpload, m_cmdGroupMeta, m_shadowCmdMeta,
    // m_instanceRemapBuffer, PerViewCB). Do not single-buffer it: one frame of
    // lag would drive frame F's ringed buffers with frame F+1's element counts
    // and group offsets, and each ring is filled only up to its own frame's
    // count, so reading past it reads what an older frame left, not zeros. The
    // two latches matter most: the three stages that read them must never
    // disagree within a frame, and they run on different threads.
    // spotCull* / pointCull*: the single source of truth for which light owns
    // slice / cube i, written by BuildScene_UploadLights in the same loop, at the
    // same index, that calls SpotShadowPass::SetShadowMatrix /
    // PointShadowPass::SetFaceMatrix and feeds SetActiveCasterCount, so the cull
    // never pairs a slice with another light's volume than the pass renders
    // into it. Never derive them from a second walk over m_frameLights.
    SceneEmit m_sceneEmitBuf[2];

    // ---- Dissolve noise, spot-shadow VPs, terrain layers, material buffers ----
    // Engine-global dissolve noise (256x256 R8 tileable FBM, CPU-baked once at
    // init). Bindless slot == handleId, published to shaders via
    // PerViewCB.dissolveNoiseIdx + the shadow cascade CBs (offset 224).
    // Engine-lifetime: created in CreateConstantBuffers, destroyed in ~Renderer;
    // OnWorldClear never touches the bindless texture table.
    RHI::Texture   m_dissolveNoiseTex;
    RHI::GPUBuffer m_spotShadowVPBuffer   [kFrameSlots];
    void*          m_spotShadowVPMapped   [kFrameSlots] = {};
    uint64_t       m_spotShadowVPSrv     [kFrameSlots] = {};

    // Terrain per-layer material table (StructuredBuffer<TerrainLayerGPU>).
    // Triple-buffered UPLOAD heap, sized to kMaxTerrainLayers; BuildScene_SyncTerrain
    // writes layers.size() entries each frame and the terrain PS loops over
    // layerCount. Bound to the terrain pass's free per-draw SRV table (t4 space0).
    RHI::GPUBuffer m_terrainLayerBuffer   [kFrameSlots];
    void*          m_terrainLayerMapped   [kFrameSlots] = {};
    uint64_t       m_terrainLayerSrv      [kFrameSlots] = {};

    // Material buffer - CPU staging side. kFrameSlots UPLOAD buffers,
    // persistently mapped, indexed by the same persistent slot the GPU buffer
    // uses (never a per-frame compacted cursor). With retained slots
    // (m_persistentMatSlots) a key's sizeof(MaterialGPUData) bytes (quote the
    // static_assert in SystemHandles.h, not a number) are written into a given
    // frameSlot's staging buffer at most once for the key's whole life, so these
    // are near-idle in steady state. Still per frame slot because each remains a
    // complete material table (ReflectionProbeCapturePass reads staging
    // directly; see m_materialBufferGPU).
    RHI::GPUBuffer m_materialBuffer       [kFrameSlots];
    void*          m_materialBufferMapped [kFrameSlots] = {};

    // Per-material-slot frame records: one float4 per slot, (HeadForward.xz,
    // HeadRight.xz) of a PER_FRAME_PARAMS face, written by the emit on the slot's
    // first sight each frame. The deferred toon reads it at the pixel's material
    // index through LightCB.faceFrameIdx when the record's materialFlags carry
    // kMatGpuFlagFrameRecord, so a slot without one is never read and needs no
    // write. UPLOAD and kFrameSlots deep like the instance remap: a few faces a
    // frame, read only by face pixels - no DEFAULT copy.
    RHI::GPUBuffer m_matFrameRecord        [kFrameSlots];
    void*          m_matFrameRecordMapped  [kFrameSlots] = {};

    // [MATCHURN] window: persistent material keys minted, base records written
    // and frame records written over the last kMatSlotWindowFrames emits (a face
    // must not mint a key per frame).
    uint32_t       m_matSlotWindowFrames  = 0;
    uint32_t       m_matSlotWindowNewKeys = 0;
    uint32_t       m_matSlotWindowBaseWrites = 0;
    uint32_t       m_matSlotWindowFrameRecords = 0;

    // Material buffer - GPU side. One DEFAULT-heap StructuredBuffer that every
    // per-pixel consumer reads (GBuffer / Transparent / FaceOverlay via the
    // graph's "MaterialBuffer" binding, all four shadow passes via shadowCtx,
    // Lighting via SetMaterialBuffer). Not the UPLOAD buffer: on a discrete GPU
    // that is write-combined system memory, so material_surface.hlsli's
    // per-pixel g_Materials[matIdx].materialFlags fetch and Lighting.ps.hlsl's
    // SSS / NPR MaterialGPUData copies would cross PCIe for every pixel.
    // Kept coherent by dirty ranges only: WriteMaterialSlot's needsBaseWrite gate
    // is an exact "this slot changed" signal, so a scene whose materials are not
    // edited produces zero copies per frame. The copies are recorded in Render()
    // into a command list opened before every consumer CL (the same placement
    // and COPY_DEST -> SRV barrier as m_instanceBufferGPU).
    // ProcessProbeBakeQueue (Renderer_DDGI.cpp) feeds ReflectionProbeCapturePass
    // ctx.materialBufSrv from m_materialBuffer[frameSlot], so
    // AcquirePersistentMatSlot tracks all three frame bits and the staging
    // buffers stay complete tables. Re-pointing that one line would let frameMask
    // become a single "in GPU buffer" bit (2 fewer redundant writes + copies per
    // new key).
    RHI::GPUBuffer m_materialBufferGPU;

    // Same ownership-transfer shape as the instance ranges, plus a flag that
    // travels with them: a full-init bool shared across the halves (set on the
    // game half, cleared on the render half) would let the render half's full
    // blit clear a range set the game half is still building - an incomplete
    // blit followed by deleting the ranges that would have completed it.
    MaterialUpload m_matUploadBuf[2];

    uint32_t       m_materialCopyRecordsPeak = 0;
    uint32_t       m_materialBindingHighWater = 0;
    uint32_t       m_customMaterialTableHighWater = 0;
    uint32_t       m_customMaterialCbvHighWater = 0;

    // ---- Shadow-caster slot telemetry ----
    // BuildScene_UploadLights hands a shadow slice to the first N qualifying
    // spot / point lights in m_frameLights order and then stops; lights past N
    // stay lit but cast nothing, with no log line. These counters answer "why
    // doesn't this lamp cast a shadow?". The caps live on SpotShadowPass /
    // PointShadowPass (forward-declared here), so the capacity is mirrored into
    // a member where it is read instead of #included.
    uint32_t       m_spotShadowCasterCap        = 0;
    uint32_t       m_spotShadowCasterHighWater  = 0;
    uint32_t       m_spotShadowCastersDropped   = 0;  // last frame
    uint32_t       m_spotShadowCastersDeniedWorst  = 0; // warn-throttle latch
    uint32_t       m_pointShadowCasterCap       = 0;
    uint32_t       m_pointShadowCasterHighWater = 0;
    uint32_t       m_pointShadowCastersDropped  = 0;  // last frame
    uint32_t       m_pointShadowCastersDeniedWorst = 0; // warn-throttle latch

    // ---- ExecuteIndirect ----
    // m_indirectArgBuffer: without compaction, upload-copied then zeroed in
    // place by the cull CS; with compaction, the working arg buffer (CS-written,
    // group-compacted).
    RHI::GPUBuffer             m_indirectArgBuffer;  // DEFAULT: IndirectDrawCommand[]
    // Pristine args (compaction). Carries SHADER_RESOURCE so the compaction CS
    // reads it directly (UPLOAD heap = GENERIC_READ; no DEFAULT re-copy).
    RHI::GPUBuffer             m_indirectArgUpload [kFrameSlots];  // UPLOAD: CPU staging
    void*                      m_indirectArgMapped [kFrameSlots] = {};
    // Per-group survivor-command counts, m_maxInstances entries (groups cannot
    // outnumber commands). Zeroed per frame by copying from the pre-zeroed upload
    // twin, bumped by the compaction CS, consumed by ExecuteIndirect at group
    // offset x 4.
    RHI::GPUBuffer             m_drawCountBuffer;    // DEFAULT: u32 per group
    RHI::GPUBuffer             m_drawCountUpload   [kFrameSlots]; // pre-zeroed source
    void*                      m_drawCountMapped   [kFrameSlots] = {};

    // ---- Post-cull survivor readback (agent harness) ----
    // GPU culling runs entirely on the GPU, so without this readback
    // drawPackets reports what the CPU submitted, not what survived Hi-Z, and a
    // false-cull bug is invisible from outside the process (black window, black
    // screenshot, no ERROR line, exit 0).
    // Sized to a cap rather than m_maxInstances: only m_indirectGroups.size()
    // entries are copied, and 128 KB x 3 of readback heap for a diagnostic would
    // be waste.
    RHI::GPUBuffer             m_drawCountReadback [kFrameSlots];
    // How many u32s were actually copied into that slot. Only the first
    // groupCount entries of m_drawCountBuffer are zeroed each frame; the
    // tail is process-lifetime garbage, so summing a fixed length reads
    // noise.
    uint32_t                   m_drawCountReadbackGroups[kFrameSlots] = {};
    // 0 = this slot holds nothing to resolve. Cleared after every resolve so
    // a frame that records no copy cannot re-serve stale data from the last
    // time the slot came round.
    uint64_t                   m_drawCountReadbackFrame [kFrameSlots] = {};
    // Whether the survivor readback runs at all, latched once from
    // Sys::HasArtifactDir(): its only consumer is frames.jsonl, which exists only
    // for a harness run. A normally launched Game.exe must not pay 48 KB of
    // readback heap, a CopyBufferRegion and two cullCL barriers every frame, plus
    // a Map / sum / Unmap, for a diagnostic nobody asked for.
    bool                       m_visibleDrawCmdsEnabled = false;
    // Game -> render. Incremented on the game half (Renderer::BeginFrame) and
    // read on the render half (Renderer::Render) to stamp which frame a readback
    // slot belongs to. Atomic because two threads touch one scalar (as with the
    // RT watchdog flags above); relaxed - it orders nothing but itself, and the
    // slot it stamps is picked by the render half's own frame index.
    std::atomic<uint64_t>      m_frameCounterForReadback { 0 };

    // ---- Post-cull visible-geometry counters (InstanceCull u3) ----
    // [0] triangles [1] instances [2] commands [3] unused. Mirror:
    // shaders/InstanceCull.cs.hlsl g_VisStats.
    // Not gated on Sys::HasArtifactDir like the survivor ring above: the
    // consumer is the debug HUD, which a player can turn on in a shipped build.
    // 64 B of GPU heap, one 16-byte CopyBufferRegion and one Map / Unmap per
    // frame.
    // Its own frame counter, not m_frameCounterForReadback: that one is
    // incremented inside `if (m_visibleDrawCmdsEnabled)`, so in a normally
    // launched Game.exe it stays 0 and every slot would read as empty.
    RHI::GPUBuffer             m_visStatsBuffer;                    // DEFAULT/UAV
    RHI::GPUBuffer             m_visStatsZeroUpload;                // permanent zeros
    RHI::GPUBuffer             m_visStatsReadback [kFrameSlots];
    uint64_t                   m_visStatsReadbackFrame[kFrameSlots] = {};
    std::atomic<uint64_t>      m_visStatsFrameCounter { 0 };   // same lane as m_frameCounterForReadback above
    bool                       m_visStatsEnabled  = false;  // buffers created OK
    uint64_t                   m_visTriangles     = 0;
    uint32_t                   m_visInstances     = 0;
    uint32_t                   m_visCommands      = 0;
    bool                       m_visStatsValid    = false;  // ~kFrameSlots stale
    uint32_t                   m_visibleDrawCmds      = 0;
    bool                       m_visibleDrawCmdsValid = false;

    // ---- Virtual texture handles (RG-managed) ----
    RG::RGTextureHandle m_albedoHandle;
    RG::RGTextureHandle m_normalHandle;
    RG::RGTextureHandle m_surfaceHandle;
    RG::RGTextureHandle m_depthHandle;
    RG::RGTextureHandle m_velocityHandle;
    RG::RGTextureHandle m_emissiveHandle;
    // Lighting MRT holding opaque scene colour without the traced-reflection
    // contribution. SSR snapshots this to break the HDR -> SSR -> NRD -> traced
    // reflection -> HDR cross-frame feedback loop.
    RG::RGTextureHandle m_reflectionSourceHandle;
    RG::RGTextureHandle m_ptResultHandle;
    RG::RGTextureHandle m_diResultHandle;
    RG::RGTextureHandle m_diShadowResultHandle;   // compare mode's RTDI.Shadow (never composited)
    RG::RGTextureHandle m_giResultHandle;
    RG::RGTextureHandle m_giShadowResultHandle;   // compare mode's RTGI.Shadow (never composited)
    // Pre-grass depth snapshot: the SSR trace marches its Hi-Z pyramid against
    // this so thin grass blades do not occlude water reflections.
    RG::RGTextureHandle m_ssrTraceDepthHandle;
    // Translucent front-layer targets (TranslucentFrontLayerPass writes,
    // RTReflectionPass(TranslucentFront) + the forward fold read).
    RG::RGTextureHandle m_translucentNormalHandle;
    RG::RGTextureHandle m_translucentSurfaceHandle;
    RG::RGTextureHandle m_translucentVelocityHandle;
    RG::RGTextureHandle m_translucentDepthHandle;
    // The HDR scene colour, imported: the device's slot 0 target, bound with this
    // frame's clear value before every Execute. Invalid under -rgimportoff.
    RG::RGTextureHandle m_hdrHandle;
    // The SSR scene pyramid and result, imported: written after the graph, read
    // inside it as last frame's (History, Owner), bound before every Execute -
    // the subsystem is made after the graph's first Compile. Invalid under
    // -rgimportoff.
    RG::RGTextureHandle m_ssrPyramidHandle, m_ssrResultHandle;

    // ---- System refs (injected; not owned) ----
    Resource::MeshLibrary*     m_meshLib = nullptr;
    std::unique_ptr<MeshAssetRegistry> m_meshAssets;   // owned; built over m_meshLib
    Resource::TextureSystem*   m_texSys  = nullptr;
    Resource::ResourceManager* m_resMgr  = nullptr;

    // ---- Shared runtime material resources ----
    Sys::FrameTaskContext* m_cpuFrame = nullptr; // borrowed only during BeginFrame
    Sys::FrameTaskContext* m_pendingPublication = nullptr; // identity only until FinishFramePreparation

    std::optional<FrameIndex> m_animationVisibilityFrame;
    const World* m_animationVisibilityWorld = nullptr;
    uint32_t m_animationVisibilityEpoch = 0;
    Sys::LeasedBuffer<FrameMaterialOutputBlock> m_frameMaterialOutput;

    // ---- By-key material runtime ----
    // ShadowHookEnabled(): read once, in the constructor - the emit nest asks
    // per batch, and a function-local static there would carry MSVC's
    // thread-safe-init guard (Renderer_EmitBatches.cpp's s_verifyMatSnap note).
    bool m_shadowHookEnabled = true;

    // ---- Shared runtime material resources ----
    std::unordered_map<uint64_t, RuntimeMaterialResource> m_runtimeMaterialResources;
    std::unordered_map<Entity, RuntimeMaterialEntityBinding> m_entityMaterialRuntimeBindings;
    std::unordered_map<uint64_t, RuntimeMaterialShaderState> m_runtimeMaterialShaders;
    uint32_t m_runtimeMaterialResourceHighWater = 0;
    uint32_t m_runtimeMaterialEntityHighWater = 0;
    // See MaterialRuntimeEpoch(). Bumped inside OnWorldClear, at the
    // m_runtimeMaterialResources.clear() that makes every outstanding by-key
    // reference a phantom.
    uint32_t m_matRuntimeEpoch = 0;

    // ---- Retained (persistent) material GPU slots ----
    // The material buffer is kFrameSlots UPLOAD buffers
    // (m_materialBuffer[frameSlot]). A per-frame compacted slot index would mean
    // slot 5 = material A this frame and B the next, forcing every visible
    // material's record to be rewritten every frame. Instead a material's GPU
    // appearance is fully captured by MaterialBindingKey (matHash folds in every
    // render-affecting field, texture bindless indices included), so a key's
    // slot content is immutable: any appearance change (an edit, or a texture
    // finishing its async load) changes matHash -> a different key -> a
    // different slot -> a fresh write. Each key gets a persistent slot and its
    // record is written into a given frameSlot buffer once (frameMask's bits);
    // once all frame buffers hold it, WriteMaterialSlot is skipped. Self-healing:
    // the hash is recomputed every frame and the key is the identity, so a
    // missed SetDirty cannot show stale data.
    // Custom-shader materials still re-pack their b8 CBV + space3 SRV table every
    // frame (those live in per-frame ring allocators); only the base
    // MaterialGPUData write is retained.
    // `identity`: 0 for a content-keyed material; for a PER_FRAME_PARAMS one, the
    // MaterialComponent's address (the identity MatSnapKey uses). Its
    // frameParams stay out of matHash, so its content key is stable, but its
    // slot carries per-frame values (the b8 repack and the frame record), so two
    // faces with the same authored content still get two slots - one per face
    // for its life, not one per face per frame. Build keys with BindingKeyOf.
    std::unordered_map<MaterialBindingKey, PersistentMatSlot, MaterialBindingKeyHash>
        m_persistentMatSlots;
    // Slot 0 is reserved as the neutral no-material record and never handed to
    // a key: terrain and WaterComponent water encode matIdx 0 in GBuffer
    // normal.a to mean "no per-material data", and Lighting / NPRStabilize /
    // AutoExposure all dereference that slot, so a live material there would
    // leak its NPR ceiling / shoulder / exposureCompensation onto every terrain
    // and water pixel. The staging buffers are pre-filled with default PBR
    // content at creation, so the reserved slot needs only a one-time
    // StageMaterialSlot per frame buffer (m_matSlot0Staged).
    uint32_t m_persistentMatSlotNext    = 1;      // high-water of distinct slots handed out
    uint8_t  m_matSlot0Staged           = 0;      // bit f: frameSlot f's DEFAULT-heap copy holds slot 0
    bool     m_persistentMatSlotsReset  = true;   // rebuild the slot map at the next SortAndEmit
    bool     m_retainMaterialSlots      = true;   // false => clear every frame (no retained slots)
    // Slots reclaimed from keys evicted by the end-of-frame GC
    // (SweepPersistentMatSlots). Without reclamation the map fills to
    // m_maxMaterials and overflows to slot 0, because any per-frame-animated
    // material (matHash folds baseColor / emissive / customParams) mints a new
    // MaterialBindingKey every frame. The GC evicts every key not seen this
    // frame and parks its slot here; AcquirePersistentMatSlot prefers this list
    // over bumping the high-water.
    std::vector<uint32_t> m_freeMatSlots;

    // ---- The hoisted batch plan (-surfacepagesearly) ----
    // One row per emitted batch, produced by the ResolveBatchPlan stage that
    // runs on main between the sort and the emit nest, consumed by the nest.
    // The emit nest holds the only two stateful allocators in
    // BuildScene_SortAndEmit: AcquirePersistentMatSlot (which stamps
    // lastSeenEpoch on first sight, so it cannot run twice for one key in a
    // frame) and MaterialSurfaceCache::AcquirePage / MapEntity (whose publish
    // epoch DDGISceneAS::Gather reads later in the same Renderer::BeginFrame).
    // Walking the sorted candidate array once ahead of the nest and making
    // exactly those calls in that order leaves the nest side-effect free with
    // respect to both, which is what lets it run on another thread. Both move
    // together: the page key is (meshDescriptorIndex, materialIndex) and
    // materialIndex is the persistent material slot.
    // Bit-identity: every other field is a pure function of the sorted array
    // (batch boundary, the m_maxInstances truncation, the dissolve tally, the
    // TDR-guard prediction, the page gate), and the two allocators are
    // deterministic state machines whose only in-frame writer is themselves, so
    // the same input order yields the same slots, page indices, LRU victims and
    // publish epoch.
    std::vector<BatchPlanRow> m_batchPlan;
    // +1 per ResolveBatchPlan. The FramePacket carries this value (read through
    // GetBatchPlanGen at hand-off); m_batchPlanGenConsumed below is what the emit
    // nest actually read, and FramePipeline compares the two when it consumes the
    // packet. See EmitLeaseMismatch.
    uint32_t m_batchPlanGen = 0;
    // Latched by the nest at its entry from the plan it is about to consume.
    // Comparing it with the packet's copy checks that the batch-plan generation
    // the lane read is the packet's; the two are produced at different points by
    // different code, so the check can fail.
    uint32_t m_batchPlanGenConsumed = 0;
    // Same shape for the sort permutation: +1 wherever s_sortIdx is (re)built,
    // latched by the nest, carried by the packet. An empty or stale permutation
    // is the other silent way the nest emits a wrong frame when it runs on
    // another thread than the sort (an `indices.size() == candidates.size()`
    // check cannot catch it).
    uint32_t m_sortIdxGen         = 0;
    uint32_t m_sortIdxGenConsumed = 0;

    // ---- The sort -> nest hand-off buffers ----
    // Members, not thread_local locals of BuildScene_SortAndEmit: with the plan
    // stage on main and the nest on the render-owner lane, thread_locals would
    // resolve to two different objects and the lane's would be empty - a silent
    // empty frame, no crash, no debug-layer message. The size check at the
    // nest's entry guards it.
    // The other emit scratch (materialWriteScratch, earlyScratch, emitScratch,
    // sortScratch) stays thread_local on purpose: each is produced and consumed
    // inside one half, so it is self-consistent wherever that half runs.
    std::vector<uint32_t>          m_sortIdx;      // the sort's output permutation
    std::vector<CandidateEmitPlan> m_planScratch;  // the parallel plan lanes' output

    // ---- The PlanEmit -> EmitBatches contract ----
    // Everything that crosses from BuildScene_PlanEmit (the sort, the preamble,
    // ResolveBatchPlan) to BuildScene_EmitBatches is named here, including three
    // Sys::CpuTaskScope objects (non-copyable, destructor == Join). Do not
    // capture the caller's frame by reference across the seam: a by-reference
    // capture that outlives its frame is a dangling read once the halves run on
    // different threads, and CpuTaskScope's own guard does not catch
    // "constructed on main, joined on the lane" - it only refuses worker
    // threads, and a render-owner lane is an owner.
    // PlanEmit writes every field it owns - most in one block at the seam, the
    // plan-prefix group earlier, because ResolveBatchPlan's pass A consumes it on
    // PlanEmit's side (read the arming stamps at the bottom before adding a
    // field). EmitBatches reads them and builds its own lambdas over this struct
    // alone; materialTailScope is the one member EmitBatches emplaces. A member
    // rather than a parameter for the three std::optional<CpuTaskScope> (as
    // m_brsEarlyScope is in flight across phases) and because this object is
    // what is handed to the lane.
    // Not here on purpose: anything produced and consumed inside one half. The
    // emit scratch (materialWriteScratch / earlyScratch / emitScratch /
    // sortScratch) stays thread_local inside EmitBatches; the InstanceJob /
    // StagedRecord / EmitLane types and the allocateInstance / emitInstance /
    // publishMaterialChunk / prepareMaterialOracle lambdas live in EmitBatches;
    // the nest's `i` / `instIdx` walk counters are its own locals (a second
    // instIdx here would make the cross-walk check a tautology).
    EmitContext m_emitCtx;

    // The upload slot this frame's emit writes through, latched once at the head
    // of BeginFrame. Every mapped pointer the emit uses derives from it, so
    // re-reading m_gfx.GetUploadSlot() after main has acquired F+1's slot would
    // move six mapped pointers at once. Latching it also lets the FramePacket
    // carry the slot.
    uint32_t m_frameUploadSlot = 0;

    // ---- -brslane: the emit half runs on the render-owner lane ----
    // m_brsLaneEnabled is the run-wide answer FramePipeline's refusal ladder
    // reached. m_emitDeferred is this frame's: latched at the head of BeginFrame
    // beside the lease, so BuildScene_PlanEmit and BuildRenderScene read one
    // value that cannot change under them, and so `framePreparation` can be true
    // on a frame where main holds no FrameTaskContext (the lane makes one).
    // m_laneEmitOwed is the hand-off latch: set by main when BeginFrame returns
    // without an emit half, cleared by the lane's FinishFramePreparation.
    bool m_brsLaneEnabled = false;
    // -brsplanlane: m_brsPlanLane is the run's switch; m_planOnLaneThisFrame is
    // this frame's promise, set by main in BuildScene_SortAndEmit when it hands
    // the plan over and cleared by the lane once PrepareFrameOnLane ran it.
    // m_emitStatsSeed is PlanEmit's ctx.stats seed, taken by main (m_frameStats
    // is main's from the hand-off on); m_lanePlanMs rides the stats publish.
    bool       m_brsPlanLane         = false;
    bool       m_planOnLaneThisFrame = false;
    FrameStats m_emitStatsSeed{};
    float      m_lanePlanMs          = 0.0f;
    // (m_classifyOnLaneThisFrame lives beside ClassifyInputs, which it needs
    // complete.)
    bool m_emitDeferred   = false;
    bool m_laneEmitOwed   = false;
    // How many frames took the per-frame fallback (a refused batch plan under
    // -brslane: BuildScene_PlanEmit clears m_emitDeferred and the emit half runs
    // on main). Monotonic; it rate-limits the log line and [SURFACEPAGES]
    // reports it, so "the fallback never fired" and "it fired unprinted" read
    // differently.
    uint64_t m_emitLaneFallbackFrames = 0;
    // The skin payload's emit index, latched by main's AdvanceSkinJobsWrite at
    // the hand-off and consumed by the lane's PublishSkinJobsRead. Same seam,
    // same ordering, one writer each side.
    uint32_t m_laneSkinIdx = 0;
    // The emit index the packet carried into PrepareFrameOnLane, so the
    // emit-half oracle's `index` bit compares the published payload against a
    // value FramePipeline snapshotted on main, not against the argument of the
    // PublishSceneRead just before it (which could not fail). Written only on the
    // lane arm; the main arm passes its pre-publish m_sceneWrite instead.
    uint32_t m_laneLeaseEmitIdx = 0;
    // -brslaneverify=N: the emit-half oracle. Armed independently of -brslane
    // (like -animoverlapverify): a baseline arm has to report the same digest,
    // or "the bytes match" asserts something about a path no run in the
    // comparison took. Logged every N frames from whichever thread ran
    // EmitBatches.
    int      m_brsLaneVerifyEvery = 0;
    uint64_t m_brsLaneVerifyFrames = 0;
    // -brslaneselftest=N: the frame the invariant predicate is handed a wrong
    // answer on. -1 = never, and frame numbers start at 0, so "off" and "frame
    // 0" are distinguishable.
    int      m_brsLaneSelfTestFrame = -1;
    // The oracle's own cost, in ms, for the frame it last ran on.
    // FramePipeline::RenderOwnerLoop reports it beside prepareMs, not inside it:
    // [BRSLANE_PREPARE] is the gate's instrument, and a diagnostic must not
    // charge itself to what it measures. Consumed and cleared by
    // TakeBrsLaneOracleMs, so a frame the oracle skipped reports 0.
    double   m_brsLaneOracleMs = 0.0;
    // Staging for the write-combined mapped reads the digest makes. A member so
    // a steady run allocates nothing; see BrsLaneFoldMapped for why the copy
    // exists at all.
    std::vector<uint8_t> m_brsLaneStage;

    // Monotonic, +1 per BuildRenderScene. Stamps the retained-slot first-sight /
    // last-seen test.
    uint64_t m_materialFrameEpoch = 0;
    // Entity-id-indexed material-slot record for GetEntityMaterialSlot (epoch-
    // stamped so no per-frame clear). Written in the SortAndEmit per-instance
    // loop; a slot is only valid while its epoch matches m_materialFrameEpoch.
    std::vector<uint32_t> m_entityMatSlot;
    std::vector<uint64_t> m_entityMatSlotEpoch;
    // Bindless g_Buffers[] slot of m_materialBufferGPU for RT reflection NPR hit
    // shading. Owner-only cache, published via FrameView; reset on world clear
    // (the mesh descriptor heap invalidates world-scoped slots) and whenever the
    // GPU material buffer is recreated.
    uint32_t m_materialBufGpuBindless = RHI::kInvalidBufferIndex;
    // Same lifetime, for the AutoExposure 1-float buffer - reflected NPR hits
    // replicate the NPRStabilize exposed-space shoulder / ceiling through it.
    uint32_t m_exposureBufBindless = RHI::kInvalidBufferIndex;

    // Texture-handle releases queued by OnWorldClear, held until the next
    // BeginFrame's SyncMaterialTextures pass has acquired same-path handles.
    // Releasing inline would drop TextureSystem refcounts to 0, FreeSlot would
    // wipe the path-hash mapping, and the next Acquire would re-upload the
    // texture from CPU bytes even when reloading the same world. Deferring by one
    // frame lets the new world's Acquire bump the refcount 1 -> 2 before the
    // release takes it back to 1, so the slot survives with no GPU re-upload.
    std::vector<Resource::TextureHandle> m_pendingMatTexReleaseAfterNextSync;

    // MeshLibrary handles loaded for the currently bound world. Two-stage
    // release: each OnWorldClear releases the previous clear's pending set and
    // parks the current one, so every library lives at least one world reload
    // before it is freed. Inline release (even with full multi-queue sync)
    // caused a delayed TDR; the one-world lag avoids it.
    std::vector<Resource::Handle> m_worldMeshLibs;
    std::vector<Resource::Handle> m_meshLibsPendingRelease;
    // One retained reference per distinct MeshLibrary in the detached Editor
    // world snapshot. Ownership transfers to m_worldMeshLibs on Stop.
    std::vector<Resource::Handle> m_editorPinnedMeshLibs;

    // ---- Skybox / IBL textures ----
    SkyboxTexEntry m_skyboxTexCache[3]; // [0]=irradiance, [1]=radiance, [2]=skybox visual
    std::unordered_map<std::string, SkyboxTexEntry> m_skyMaterialTextures;
    MaterialComponent m_skyMaterialConfig;
    std::string m_skyMaterialPath;
    uint32_t m_skyMaterialPoll = 0;
    SkyboxTexEntry m_brdfLutEntry;      // pre-integrated BRDF LUT (2D)
    uint64_t       m_brdfLutSrv             = 0; // cached per frame for SSRComposite
    uint64_t       m_skyRadianceSrvForDDGI  = 0; // DDGI miss shader; 0 if no skybox
    // Same irradiance cube LightingPass diffuse-samples (real, or the black
    // fallback; set by SyncSkyboxIBL) - RTR analytic hits mirror the exact
    // skyDiff branch.
    uint64_t       m_skyIrradianceSrvForRTR = 0;

    // Shared 1x1 black TextureCube - a valid fallback binding for sky-cube
    // consumers (DDGI miss, water reflection) when the scene has no sky
    // (WorldEnvironment None / FlatColor) or the sky cube is not ready, so those
    // passes sample black instead of a stale / unbound descriptor. Created once
    // in Compile().
    RHI::Texture   m_fallbackSkyCube;
    uint64_t       m_fallbackSkyCubeSrv     = 0;

    // FlatColor's uniform sky light: a 1x1 cube of radiance ambientColor x
    // ambientIntensity, handed to every sky consumer in place of the sky cubes
    // (deferred, forward, water, particles, the face overlay, DDGI's and RT
    // reflections' misses, the path tracer) with iblStrength 1, so GI occludes
    // and bounces it like a sky. Recreated (the previous one released through
    // the deferred queue) only when the radiance changes. None mode keeps the
    // ambient as a non-physical fill.
    RHI::Texture      m_uniformSkyCube;
    uint64_t          m_uniformSkyCubeSrv  = 0;
    DirectX::XMFLOAT3 m_uniformSkyRadiance { -1.0f, -1.0f, -1.0f };

    // The last "[SKYLIGHT] unoccluded" state logged (BuildScene_UpdateDDGI).
    bool              m_skyLightUnoccluded = false;

    // ---- Lazy-loaded misc textures ----
    // Generic debug-icon texture cache, keyed by asset path. One Acquire per
    // unique path; GetDebugIconBindless() returns the bindless index. A new debug
    // icon kind is just a new path - no new member needed.
    std::unordered_map<std::string, Resource::TextureHandle> m_debugIconTex;

    // Glyph-baked debug icons: an engine texture rasterized once from a font
    // glyph (Font Awesome), keyed by codepoint. The editor extracts the RGBA
    // sub-rect from the ImGui atlas and calls RegisterGlyphIcon; the gizmo pass
    // then samples it by its bindless handleId (== GetGlyphIconBindless).
    // Lets the viewport light/camera gizmos reuse the UI icon font instead of
    // shipping separate .itex assets.
    std::unordered_map<uint32_t, GlyphIconTex> m_glyphIconTex;

    Resource::TextureHandle m_moonHandle      = Resource::kInvalidTextureHandle;
    uint64_t                m_moonSRV         = 0;

    // ---- Picking ----
    // m_instanceSlotToEntity is deliberately sticky and not a seam lane:
    // AcquireInstanceSlot updates it only at (re)assignment, so a freed slot
    // keeps its old entity until reuse and a picking readback up to
    // frames-in-flight stale still resolves the entity that was rendered.
    // Double-buffering it would need a 128 KB seed copy every frame. Its one
    // render-half reader is the probe-bake visibility filter
    // (EntityForDrawLocalInstance): if the game half of F+1 reassigns a slot
    // while the render half of F resolves it, nothing tears (Entity is 32-bit)
    // and one frame of one probe bake filters against the slot's newer owner -
    // not worth a per-frame copy. The other reader, ResolvePick, is game half and
    // stale-tolerant by design.
    // Sized at the compile-time upper bound, not m_maxInstances: an inline array
    // on the picking / SortAndEmit path, and the constructor guarantees
    // m_maxInstances <= kMaxInstancesUpperBound, so every index that reaches it
    // is in range. Entries at [m_maxInstances, bound) are unused.
    Entity       m_instanceSlotToEntity[kMaxInstancesUpperBound]{}; // slot index -> ECS Entity

    // Game -> render: everything the bake needs about the probe, resolved out
    // of the World once so the record half needs none of it.
    ProbeBakeRequest m_probeBakeReq;
    // Render -> game: which probe actually got recorded. NullEntity = nothing
    // to apply. Deliberately just the entity - the four fields written are
    // constants of "a bake happened", so there is nothing else to carry back.
    Entity           m_probeBakeDone = NullEntity;
    // The bakes the render half recorded, for the harness's -probedump
    // (ProbeBake). The count is published last (release), so a reader that sees
    // it change sees that bake's record.
    ProbeBakeRecord       m_probeBakeLog[kProbeBakeLog];
    std::atomic<uint64_t> m_probeBakeCount{ 0 };

    // ---- Material preview layer ----
    // Lazy: constructed by the first AcquirePreviewView, and only when the kill
    // switch allows it, so a run that never opens the panel allocates nothing.
    // Held by unique_ptr to a forward-declared type: PreviewRenderer.h pulls in
    // ToneMapPass.h, and Renderer.h is included by ~200 TUs.
    // Destruction order matters: preview passes borrow the main GBufferPass's
    // ShaderLibrary, and PSOCache keeps that as a raw pointer. Declared after
    // m_graph, and members are destroyed in reverse declaration order, so the
    // preview layer is torn down before the graph that owns what it borrows. Do
    // not move this declaration above m_graph.
    std::unique_ptr<PreviewRenderer> m_preview;

    // Render -> game mailbox for the two preview FrameStats counters.
    // ProcessPreviewQueue produces them on the render half, and
    // HarnessCaptureSystem reads FrameStats from TickPhase::PostRender, which
    // runs after the seam's kick, concurrently with the render half - writing
    // FrameStats directly would race. So the render half publishes atomics and
    // ApplyPreviewFeedback (game half, after the park) folds them in: the
    // numbers describe the last completed render half, the same one-frame offset
    // gpuAgeFrames carries.
    std::atomic<uint32_t> m_previewViewsRenderedMbx{ 0 };
    std::atomic<uint32_t> m_previewViewDrawsMbx{ 0 };

    // The editor half of the kill switch (SetPreviewPrefEnabled). Atomic because
    // it is written from the game half's ProcessPendingActions and read by the
    // editor widget from OnUIRender on the render-side context.
    std::atomic<bool> m_previewPrefEnabled{ true };

    // The preview's studio IBL pair (PreviewStudioRig.h). Engine assets,
    // acquired once on the game half and held for the life of the Renderer:
    // OnWorldClear does not touch them (it only defers the runtime-material
    // textures), so the preview's environment survives every Load / New /
    // Play -> Stop.
    Resource::TextureHandle m_previewIrrHandle = Resource::kInvalidTextureHandle;
    Resource::TextureHandle m_previewRadHandle = Resource::kInvalidTextureHandle;
    bool                    m_previewIblRequested = false;
    // One-shot latch for the "radiance mips N" line - see the emit site for why
    // that number is a contract and not a diagnostic.
    bool                    m_previewIblLogged    = false;
    // -preview_ibl_delay. 0 = shipped behaviour. Members rather than #ifdef'd
    // fields so the layout is identical in every configuration; only the setter
    // is harness-gated.
    uint32_t                m_previewIblDelayFrames = 0;
    uint32_t                m_previewIblHeldFrames  = 0;
    // -preview_key_light. Same shape and reason: the fields exist in every
    // configuration, only the setter is harness-gated. Latched here because the
    // flag is parsed long before the first AcquirePreviewView builds the layer;
    // AcquirePreviewView pushes it through.
    bool                    m_previewKeyLightSet    = false;
    float                   m_previewKeyLightDir[3] = { 0.f, 0.f, 0.f };
    // -preview_shadow_alpha. Same shape again; true is the shipped value, so a
    // layer built with nothing latched behaves normally.
    bool                    m_previewShadowAlphaTest = true;

    // ---- The world scene-light off-arm ----
    // Two fields. The value the game half wants is latched here (a boot flag and
    // a Lua call can both land before the first SetGamePiP builds the preview
    // layer, and a value pushed at a layer that does not exist yet would
    // silently do nothing); the dirty flag lets ApplyPendingGamePiP be the only
    // writer of the render half's copy. PreviewRenderer::m_worldSceneLightsEnabled
    // is a plain bool read by WriteWorldClusterLights on the render half, while
    // SetPreviewWorldSceneLights is called from Lua (game half, parked) and from
    // the harness block at the tail of App's frame loop, where the previous
    // frame's render half can still be inside RenderHalf. So, as for SetGamePiP:
    // record an intent, apply it inside the parked extract window.
    bool                    m_previewWorldSceneLights = true;
    bool                    m_previewWorldSceneLightsDirty = false;

    // ---- Material preview layer ----
    // What SyncSkyboxIBL published this frame, captured for
    // Feature::kSceneLighting. Written at the end of SyncSkyboxIBL (game half)
    // and read by SyncPreviewEnvironment later in the same half, so plain values
    // with no synchronisation, like the studio handles above. Not the pass's own
    // state: LightingPass exposes no IBL getter, and one would let the record
    // half re-read a handle the game half is replacing.
    uint64_t m_sceneIblIrradiance = 0;
    uint64_t m_sceneIblRadiance   = 0;
    uint32_t m_sceneIblRadianceMips = 0;
    uint64_t m_sceneSkyboxCube    = 0;
    uint64_t m_sceneSkySH         = 0;
    bool     m_sceneSkyUseSH      = false;
    float    m_sceneIblStrength   = 1.0f;

    // Preview mesh-override libraries, keyed on the .meshlib path in one
    // spelling (PreviewLibKey = Resource::MeshLibCanonicalPath, applied at the
    // single entry point ResolvePreviewMeshEntry), so a browser tile's spelling
    // and a prefab part's spelling are one cache key, one upload, and one entry
    // for InvalidatePreviewAsset to find.
    // MeshLibrary::Load is refcounted and path-deduplicated, so this map is what
    // keeps the extract from bumping that refcount every frame. Released in
    // OnWorldClear with the rest of the per-world mesh state, in
    // InvalidatePreviewAsset, and by the LRU trim below.
    std::unordered_map<std::string, PreviewMeshLibEntry> m_previewMeshLibs;
    uint64_t m_previewMeshLibClock = 0;
    // The clock reading at the start of the compose / resolve in progress.
    // TrimPreviewMeshLibs refuses to evict anything stamped after it, so an asset
    // that needs more distinct libraries than the cap keeps all of them for its
    // own resolve instead of evicting the slots it is registering; it then stays
    // over the cap and says so once.
    uint64_t m_previewMeshLibKeepMark = 0;
    bool     m_previewMeshLibCapLogged = false;
    // Said once when a preview invalidation finds the library still held by
    // somebody else (the live world), so the hand-back is a bare Release.
    bool     m_previewMeshLibSharedLogged = false;
    // Paths whose load failed, so the "FAILED" line is emitted once per path
    // even though the failure itself is not memoised.
    std::unordered_set<std::string> m_previewMeshLibFailLogged;
    // Set by ResolvePreviewMeshEntry when a library could not be opened during
    // the resolve in progress; read (and cleared) by ComposePreviewAsset /
    // ResolvePreviewAsset, which turn it into a retry for that asset.
    bool m_previewMeshLibLoadFailed = false;

    // The retry clock, per asset (keyed on PreviewAssetKey). Presence is the
    // failed state: there is no counter to advance or re-arm - while an entry
    // exists, PreviewAssetRetryTick derives its value from m_previewAssetFrame,
    // so the stamp term moves once every kPreviewAssetRetryFrames frames whether
    // or not anybody calls back in.
    // The fields are diagnostic, not part of the mechanism: they let the
    // recovery say how long it took and how many attempts it cost.
    std::unordered_map<std::string, PreviewAssetRetry> m_previewAssetRetry;

    // Monotonic, advanced once per ExtractPreviewRequests (once per game
    // frame). Only the two preview clocks above read it.
    uint64_t m_previewAssetFrame = 0;

    // The cap in force. kPreviewMeshLibCap is the shipped default and the only
    // value a normal run uses; the harness can lower it so a folder scan crosses
    // the ceiling (the corpus otherwise never does - the Tokyo scan peaks at 2
    // libraries), which exercises the whole eviction path: the release, the
    // mesh-descriptor and bindless VB / IB detach, the memo drop, the epoch bump
    // and the deferred hand-back that covers the one-frame race.
    std::size_t m_previewMeshLibCap = kPreviewMeshLibCap;
    // Evictions since boot, reported on the trim line as `sessionEvicted=`. One
    // trim call can evict as few as one library (the watermark refuses anything
    // the current resolve stamped), so the per-call count cannot tell "the
    // ceiling was crossed once" from "repeatedly"; the running total can, and
    // asset_preview_lru floors it.
    std::uint64_t m_previewMeshLibEvicted = 0;

    // The descriptor-heap slots of libraries this renderer has detached,
    // waiting for the frames that could still read them to retire.
    // A queue, not a direct call: MeshDescriptorHeap::FreeMesh defers the slot
    // index by kFramesInFlight frames but zeroes the descriptor immediately, so
    // freeing at eviction would make every command list already recorded this
    // frame that names the slot draw nothing - a silently missing mesh with no
    // validation error. So the detach happens now, with its heap-side
    // registration erase (the resource is alive only at that instant); the free
    // happens when the device says the referencing frames have retired, stamped
    // with IGraphicsDevice::GetReleaseStampFrame / GetRetiredFrame - the pair
    // GraphicsDX12 keys its own deferred releases on, since a slot ring of our
    // own cannot answer this once the halves are a frame apart.
    std::vector<PendingMeshLibHandback> m_previewMeshLibHandbacks;
    // Said once when the first hand-back is actually freed. The deferral is
    // invisible in every other artifact - same packets, same picture, same
    // validation - so without this line nothing can assert it.
    bool m_previewMeshLibHandbackLogged = false;

    // The memoised composition of one `.meshlib` / `.ipfb`, keyed on its path. A
    // pure function of the files (see Preview::AssetPlacement), so safe to hold
    // across frames, and it must be held: the alternative re-parses a prefab plus
    // its .iscn every frame the panel is open. An entry is inserted even when
    // the parse failed (an empty vector), so a bad path is not re-read and
    // re-logged every frame. Dropped by InvalidatePreviewAsset and OnWorldClear.
    // A `.meshlib` that would not open is not memoised, and that covers the empty
    // container too: MeshLibrary::Load refuses meshCount == 0, so an importer
    // that publishes a header before its entries takes the retry path. A prefab
    // that composes zero placements is a settled answer about the file and stays
    // memoised.
    std::unordered_map<std::string, Preview::AssetComposition>
        m_previewAssetPlacements;

    // Assets whose merged bounds had zero extent, so the studio auto-fit could
    // not derive a scale and they are drawn at their authored size. The panel
    // reads this to say so - a silently unfitted subject in a uniform grid reads
    // as a renderer bug.
    std::unordered_set<std::string> m_previewUnfittedAssets;

    // Mesh Editor: the fit each composed asset got (centre of the merged bounds,
    // uniform scale), keyed like the set above; absent = not composed yet, or
    // not fittable. Written by ResolvePreviewAsset, read by GetPreviewAssetFit.
    std::unordered_map<std::string, PreviewAssetFit> m_previewAssetFits;
    uint32_t m_previewAssetMemoEpoch = 0;
    // Once-per-session latches for the three "the picture is partial, and here
    // is why" lines the asset composition can emit. Not ENGINE_CHECKs: each is a
    // legitimate state of a real asset, and the right response is a partial
    // picture plus a line, not a failed run.
    bool m_previewPartResolveFailLogged = false;
    bool m_previewFitNonFiniteLogged    = false;
    bool m_previewFitZeroExtentLogged   = false;
    // One bit per PrimitiveMeshType already reported, so
    // `[PREVIEW] asset primitive: type=` is emitted at most once per distinct
    // primitive per session (five lines at most). A bitmask, not a bool: a
    // prefab that places a Cube and a Sphere must name both. Not one line per
    // resolve: the resolve re-runs on every composition invalidation, and the
    // packs read the last matching line.
    uint32_t m_previewPrimitiveTypeLogged = 0;
    // The single-mesh path's equivalent of the latches above. Its own latch:
    // the two paths are reached by different panels, and a shared one would let
    // whichever ran first silence the other for the session.
    bool m_previewMeshFitDegenerateLogged = false;

    // The command list ProcessPreviewQueue recorded this frame. Invalid when it
    // recorded none. Taken (and reset) by FramePipeline::RenderHalfAsync, which
    // owns the primaryCL edge - see TakePreviewCommandListThisFrame.
    RHI::CommandList m_previewCLThisFrame{};

    // ---- The in-game picture-in-picture ----
    // Plain members, as for the probe-bake lane: FramePipeline::DrainFrameWork
    // joins render half F before game frame F+1 touches any of this. The render
    // half's one reader is GetGamePiPComposite, and by then the game half is
    // parked.
    // Nothing is allocated until SetGamePiP is called: m_pipView stays
    // kInvalidView, no view is acquired, no graph is compiled, and Game.exe pays
    // one per-frame null test.
    Entity   m_pipEntity  = NullEntity;
    uint32_t m_pipView    = Preview::kInvalidView;
    uint32_t m_pipW       = 0, m_pipH = 0;
    uint32_t m_pipTick    = 1;

    // What SetGamePiP / ClearGamePiP queued, drained by ApplyPendingGamePiP at
    // the top of BuildRenderScene. Game-owned and single-slot on purpose: the
    // last request this frame wins, which is what a script toggling a camera
    // means.
    PipPending m_pipPending{};
    // Where it composites, in back-buffer pixels. 0 = "auto", which is the
    // bottom-right corner with a margin, resolved at composite time because the
    // back-buffer size is a render-half fact.
    int32_t  m_pipRect[4] = { 0, 0, 0, 0 };
    // The live camera views (PiP and the editor's CameraPreviewWindow alike),
    // so ExtractPreviewCameras knows which entity each one is pointed at. Index
    // is the preview view id. NullEntity = not a camera view.
    Entity   m_previewCamEntity[Preview::kMaxPreviews] = {};
    // Log-once-per-view latch for "this camera view could not contribute a
    // secondary frustum". Per view and re-armed as soon as it does contribute:
    // the normal case (a view acquired this frame, sized on the next) must not be
    // reported as a fault, but a view that never resolves must not stay silent.
    // See ExtractPreviewCameras.
    bool     m_previewNoFrustumLogged[Preview::kMaxPreviews] = {};

    // ---- The exact frustum of each live WorldCull view ----
    // Built in ExtractPreviewCameras right after the inflated one the classify
    // lane gets, and consumed by ExtractPreviewSceneLights at the end of the same
    // BuildRenderScene. Built there, not at the point of use: everything that can
    // refuse a frustum (no request size, no lens, a degenerate or NaN lens) has
    // run by then, and re-deriving the projection later would skip those checks
    // (MakeSecondaryViewProj is the one construction).
    // Exact (inflate=false), not the classify lane's inflated form: the classify
    // pass only decides whether to keep a duplicate of a candidate, but only
    // pixels inside the exact frustum are rasterised, so a light outside it would
    // spend cap budget on nothing.
    // Indexed by view id - unlike m_secondaryFrusta, which is compacted by
    // push_back.
    FrustumPlanes m_previewLightPlanes[Preview::kMaxPreviews] = {};
    bool     m_previewLightPlanesValid[Preview::kMaxPreviews] = {};
    // The eye the planes were built from, cached beside them for the distance
    // half of the importance score - stored here, where the camera was
    // validated, rather than read back out of the preview latch later.
    DirectX::XMFLOAT3 m_previewLightEye[Preview::kMaxPreviews] = {};
    // The importance scratch for the over-cap case: (score, index into
    // m_frameLights) for the survivors of the frustum test. A member so the
    // allocation is reused (the scene builder's adaptive-reserve pattern). Never
    // handed across the seam - the seam carries the fixed-size packed array.
    std::vector<std::pair<float, uint32_t>> m_previewLightScore;
    // 0 = use Preview::kIdleTeardownFrames. Set by the harness flag
    // -preview_idle_frames before any panel exists, so it is latched here and
    // pushed into the layer at its lazy construction.
    uint32_t m_previewIdleFramesOverride = 0;
    // The count the last "[PREVIEW] secondary frusta=" line reported.
    // 0xFFFFFFFF rather than 0 so the first extract always says something: "0"
    // is a real answer (every camera view suspended) and a pack must be able to
    // see it arrive.
    uint32_t m_previewSecondaryFrustaLogged = 0xFFFFFFFFu;
    // One-shot: the BVH's own answer for the first secondary cull. See the
    // emit site - it is what separates "the frustum was built wrong" from "the
    // draw-local remap was read from the wrong buffer".
    mutable bool m_previewCullLogged = false;
    // The secondary-view water count: how many Water packets the frame holds
    // while a preview view is culling. Documentation, not a gate: the exclusion
    // holds by construction (filterList reads only the four non-water lists and
    // RemapToSecondaryFilter answers false for DrawFilter::Water), so a count of
    // packets the preview never looked at cannot catch a regression. It says
    // whether this scene had water to exclude, which makes the panels' tooltip a
    // statement about the run. Logged on change; the sentinel keeps "0 water
    // packets" distinct from "never said".
    mutable uint32_t m_previewWaterPacketsLogged = 0xFFFFFFFFu;

    // ---- CullView's scratch, hoisted out of the frame ----
    // Members for the capacity (no per-cull allocation on the render half's
    // budget); every one is clear()ed, never shrunk. Render half only, one call
    // at a time, and mutable because the cull is const (it reads published frame
    // state).
    // m_viewCullStamp is an entity-indexed stamp array tested against
    // m_viewCullStampVal instead of an unordered_set: a "was this entity
    // visible" query is one indexed load, and the per-call reset is an
    // increment.
    mutable std::vector<Entity>     m_viewCullVisible;
    mutable std::vector<uint32_t>   m_viewCullStamp;
    mutable uint32_t                m_viewCullStampVal = 0;
    mutable std::vector<float>      m_viewCullDepth;
    mutable std::vector<uint32_t>   m_viewCullOrder;
    mutable std::vector<DrawPacket> m_viewCullSorted;
    // [VIEW_CULL]: the view names that have logged their first cull.
    mutable std::vector<std::string> m_viewCullLoggedNames;
    // ViewCullRequest::exclude as a stamp array, like m_viewCullStamp.
    mutable std::vector<uint32_t>   m_viewCullExclStamp;
    mutable uint32_t                m_viewCullExclStampVal = 0;
    mutable bool                    m_viewCullMixedWarned  = false;
    // The frame BuildScene_CullBVH last built or refit the scene BVH for. A tree
    // that is not this frame's (GPU culling off: never built, or left stale when
    // switched off) answers nothing - CullView treats every entity as never
    // asked.
    FrameIndex                      m_bvhStampFrame = 0;
    bool                            m_bvhStampValid = false;

    // ---- Material snapshots ----
    // Persistent across frames as a performance requirement: a per-frame table
    // cost ~1.5 ms on AIBench (~2050 whole-component deep copies per frame, each
    // with five std::strings and two unordered_maps). Keyed by the same
    // MaterialBindingKey the emit side uses for its persistent material slots;
    // the key includes the parameter hash, so a material whose contents change
    // gets a different key and a fresh snapshot - the table cannot serve stale
    // bytes.
    std::vector<MatSnapshot>              m_matSnapshots;
    std::vector<uint64_t>                 m_matSnapEpoch;   // parallel to m_matSnapshots

    // Open-addressed, not unordered_map: probed once per candidate (~8,400 a
    // frame on AIBench, not grouped by material), and an unordered_map keyed on
    // the 40-byte MaterialBindingKey costs ~1.1 ms of the frame. Linear probing
    // on one 64-bit key is a single cache line in the common case. The key folds
    // the fields MaterialBindingKey carries into one word - the same trust the
    // engine places in matHash (MaterialBindingKey::paramsHash).
    // key: 0 = empty, ~0ull = tombstone (probing walks past it, insertion may
    // take it). Tombstones exist because entries are retired and their storage
    // reused - see RetireMatSnapshots.
    std::vector<MatSnapSlot>              m_matSnapTable;   // power-of-two
    std::vector<uint64_t>                 m_matSnapKeyOf;   // parallel to m_matSnapshots

    // Per-frame direct-mapped memo in front of the snapshot table: ~8,500
    // candidates a frame resolve ~600 distinct materials and arrive in gather
    // order, not material order, so a single-entry memo catches under 2% of
    // them.
    MatSnapMemo m_matSnapMemo[kMatSnapMemo] = {};
    std::vector<uint32_t>                 m_matSnapFree;    // reusable storage
    size_t                                m_matSnapTombs = 0;   // tombstones in the table

    std::atomic<uint32_t> m_matSnapFastRefreshes{0}, m_matSnapFullRefreshes{0};
    std::atomic<uint64_t> m_matSnapParamVerified{0}, m_matSnapParamMismatches{0};
    std::atomic<uint64_t> m_matSnapWhyScalar{0}, m_matSnapWhyLeased{0}, m_matSnapWhyKeys{0};

    // ---- The frame's emitted command layout, double-buffered ----
    // Same pairing as the remap table above: one entry per opaque draw packet.
    std::vector<InstAabb> m_instAabbBuf[2];

    // ---- Picking ----
    PickingPass* m_pickingPass = nullptr; // owned by m_graph
    bool         m_pickForHarness = false; // the pick in the slot is the harness's (RequestPick)
    // Editor selection highlight. Treated as NullEntity in game builds.
    Entity       m_pickingOutlineEntity = NullEntity;
    std::vector<Entity> m_pickingOutlineSections;   // a StaticMesh owner's sections (IsPickingOutlined)

    // ---- Material surface cache (RT proxy for custom-shader materials) ----
    // Owned here, borrowed by SurfaceCacheBakePass (producer) and read by
    // DDGISceneAS::fillMaterialProxy (consumer). Both run inside one Render(),
    // main thread / one graph worker - see the threading note on the class.
    MaterialSurfaceCache          m_surfaceCache;
    SurfaceCacheBakePass*         m_surfaceCacheBakePass = nullptr; // owned by m_graph

    // ---- Render passes - geometry & shadows ----
    GBufferPass*                  m_gbufferPass    = nullptr; // owned by m_graph
    TerrainPass*                  m_terrainPass    = nullptr; // owned by m_graph
    GrassPass*                    m_grassPass      = nullptr; // owned by m_graph
    WaterPass*                    m_waterPass      = nullptr; // owned by m_graph
    // Mesh-driven water (ShadingModel::Water -> DrawFilter::Water). Coexists
    // with m_waterPass, the WaterComponent path.
    WaterSurfacePass*             m_waterSurfacePass = nullptr; // owned by m_graph
    SkyboxPass*                   m_skyboxPass     = nullptr; // owned by m_graph
    TransparentPass*              m_transparentPass = nullptr; // owned by m_graph
    LightingPass*                 m_lightingPass   = nullptr; // owned by m_graph
    OutlinePass*                  m_outlinePass    = nullptr; // owned by m_graph (pre-TAA composite)
    // Geometry-phase outline (writes depth + velocity into the GBuffer).
    // Mutually exclusive with m_outlinePass's hull sub-pass.
    OutlineGeometryPass*          m_outlineGeometryPass = nullptr; // owned by m_graph
    SkyIBLPass*                   m_skyIBLPass     = nullptr; // owned by m_graph
    VolumetricFogPass*            m_volFogPass     = nullptr; // owned by m_graph
    CloudPass*                    m_cloudPass      = nullptr; // owned by m_graph
    class EditorGridPass*         m_editorGridPass = nullptr; // owned by m_graph
    SceneVoxelPass*               m_sceneVoxelPass = nullptr; // owned by m_graph
    SpotShadowPass*               m_spotShadowPass = nullptr; // owned by m_graph
    PointShadowPass*              m_pointShadowPass = nullptr; // owned by m_graph
    PerObjectShadowPass*          m_perObjShadowPass = nullptr; // owned by m_graph
    PerObjectShadowPass*          m_hairShadowPass   = nullptr; // owned by m_graph (Variant::Hair)
    class HairShadowMarkPass*     m_hairMarkPass     = nullptr; // owned by m_graph (screen-space fringe mark)
    class FaceOverlayPass*        m_faceOverlayPass  = nullptr; // owned by m_graph (brows-through-hair)
    DecalPass*                    m_decalPass      = nullptr; // owned by m_graph
    PathTracePass* m_pathTracePass = nullptr;
    RTDIPass* m_rtDiPass = nullptr;
    // Compare mode: the same shell running the other DI core in the same frame;
    // active only while -restir_compare=di / restirCompare = di.
    RTDIPass* m_rtDiShadowPass = nullptr;
    bool m_restirCompareDiActive = false;
    // Compare mode's second GI instance: the other core, the same frame; active
    // only while -restir_compare=gi / restirCompare = gi.
    RTGIPass* m_rtGiShadowPass = nullptr;
    bool m_restirCompareGiActive = false;
    LightSamplingPass* m_lightSamplingPass = nullptr; // Lights v2 PDF texture + RIS buffer
    RTGIPass* m_rtGiPass = nullptr;
    RTReflectionPass*             m_rtReflectionPass = nullptr; // owned by m_graph
    // The second RTReflectionPass instance (Variant::TranslucentFront) and the
    // prepass that stamps its inputs - translucent front-layer reflections. Both
    // gated per frame in Render(); m_frontLayerActiveThisFrame is what the
    // resolve() block reads to name (or zero) the two LightCB indices.
    RTReflectionPass*                m_rtReflectionFrontPass = nullptr; // owned by m_graph
    class TranslucentFrontLayerPass* m_frontLayerPass        = nullptr; // owned by m_graph
    bool                             m_frontLayerActiveThisFrame = false;
    // The path-trace path's front tier: the same stamp, traced by
    // PathTracePass (FrontReflection) instead of the RT reflection instance.
    bool                             m_ptFrontActiveThisFrame = false;
    // PT-lit toon receivers (PathTracePass::SetNprLighting): NPR draws in the
    // frame's packets, and whether the path tracer lit them this frame (the
    // resolve() block names the result in LightCB.nprPtAmbientIdx).
    uint32_t                         m_nprDrawCount = 0;
    bool                             m_ptNprActiveThisFrame = false;
    // The main view's ShowFlags the last [RG_VIEW] line printed (~0u: none
    // yet). Render half.
    uint32_t                         m_loggedViewShow = ~0u;
    class RTShadowPass*           m_rtShadowPass     = nullptr; // owned by m_graph
    class RTAOPass*               m_rtaoPass         = nullptr; // owned by m_graph
    class SSGIPass*               m_ssgiPass         = nullptr; // owned by m_graph
    // RTR budget guard: EMA of the "RTReflectionPass" GPU region; sustained
    // over-budget -> hard disable. Only fed while the GPU profiler is enabled
    // (editor Profiler window).
    float    m_rtrSmoothedMs      = 0.0f;
    uint32_t m_rtrOverBudgetFrames = 0;
    // Same guard for RTShadowPass.
    float    m_rtShadowSmoothedMs      = 0.0f;
    uint32_t m_rtShadowOverBudgetFrames = 0;
    // Master switch for both watchdogs above + their shared ms ceiling. Default
    // off - see SetRTBudgetGuardEnabled().
    bool     m_rtBudgetGuardEnabled = false;
    float    m_rtBudgetMs           = 4.0f;
    std::unique_ptr<ShadowSystem> m_shadowSystem;             // CSM math + stabilization
    std::unique_ptr<ShadowPass>   m_shadowPass;               // standalone (not in graph)
    std::unique_ptr<ClusterPass>  m_clusterPass;
    std::unique_ptr<CullingPass>  m_cullingPass;              // GPU culling, pre-graph
    std::unique_ptr<HiZPass>      m_hiZPass;                  // post-GBuffer depth

    // ---- Per-object (player) high-res directional shadow ----
    // BuildRenderScene fills the merged player AABB + the per-frame caster set;
    // UploadFrameData builds the reverse-Z ortho VP (fitted to that AABB along
    // the sun) into LightCB.perObjShadowMatrix and hands the raw VP to the pass;
    // Render() binds the resulting depth map (or a fallback) to LightingPass.
    bool                          m_perObjectShadowEnabled = true; // editor toggle
    bool                          m_animShadowHoldout      = false; // dev measurement only

    // AddAnimPinOwner's owners and their caller-owned sets (never mutated during
    // PrepareAnimationVisibility - see AddAnimPinOwner).
    std::vector<AnimPinOwner>     m_animPinOwners;
    bool                          m_hasPlayerShadowCaster  = false;
    DirectX::XMFLOAT3             m_playerShadowAabbMin    = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3             m_playerShadowAabbMax    = { 0.f, 0.f, 0.f };
    // Entities (player root + descendants) tagged as per-object shadow casters.
    // Built once per frame before the parallel gather; read-only during it.
    std::unordered_set<Entity>    m_playerShadowSet;

    // ---- Hair (bangs -> face) directional shadow ----
    // Mirrors the player per-object shadow above, but for the small bangs-only
    // map (the Hair variant of PerObjectShadowPass) that only NPR face pixels
    // sample. Roots = HairShadowCasterComponent entities (+ subtrees).
    bool                          m_hairShadowEnabled   = true;
    bool                          m_hasHairShadowCaster = false;
    DirectX::XMFLOAT3             m_hairShadowAabbMin   = { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3             m_hairShadowAabbMax   = { 0.f, 0.f, 0.f };
    float                         m_hairShadowStrength  = 1.0f; // from the first tagged component
    float                         m_hairShadowFitScale  = 1.0f; // from the first tagged component
    float                         m_hairShadowWorldScale = 1.0f; // uniform-equivalent scale of the first tagged root
    // Source select + screen-mark tuning (HairShadowCasterComponent):
    // mode 0 = screen mark (DanbaidongRP fringe proxy), 1 = shadow map, 2 = both.
    int                           m_hairShadowMode      = 0;
    float                         m_hairMarkOffsetX     = 1.0f;
    float                         m_hairMarkOffsetY     = 1.0f;
    float                         m_hairMarkDepthPush   = 0.5f;
    std::unordered_set<Entity>    m_hairShadowSet;

    // ---- Face overlay (brows / lashes through hair) ----
    // FaceOverlayComponent subtrees; FaceOverlayPass re-draws these packets over
    // the lit scene where hair covers them. Packed settings are carried per
    // entity so independently tagged meshes stay independently editable.
    bool                          m_hasFaceOverlay      = false;
    std::unordered_map<Entity, uint32_t> m_faceOverlayParams;

    // ---- Dissolve (per-entity noise dissolve) ----
    // DissolveComponent subtrees resolved per frame (subtree inheritance - a tag
    // on a character root dissolves every child mesh part; nested tags
    // override). Values are the packed GPUInstanceData dissolve fields.
    bool                          m_hasDissolve         = false;
    std::unordered_map<Entity, DissolveDrawParams> m_dissolveParams;

    // ---- Render passes - post-process compute ----
    std::unique_ptr<TAAPass>          m_taaPass;
    std::unique_ptr<NPRStabilizePass> m_nprStabilizePass;
    std::unique_ptr<FSR2Pass>         m_fsr2Pass;  // runs iff IsFSRActive()
    std::unique_ptr<DLSSPass>         m_dlssPass;  // runs iff IsDLSSActive()
    // The reactive mask FSR 2 / DLSS SR read (one R8 texture at the render size,
    // allocated at the frame boundary only while one of them runs).
    std::unique_ptr<UpscalerReactiveMaskPass> m_reactiveMaskPass;
    // DLSS-RR's colour before transparency (an HDR copy by a graph pass),
    // allocated at the frame boundary only while RR runs.
    std::unique_ptr<RRColorSnapshot>  m_rrColorSnapshot;
    // The render half's Streamline frame token (opaque sl::FrameToken*), latched
    // at the top of Render() from StreamlineBackend::RenderFrameToken().
    // Render-half only: UploadFrameData for frame F runs while the render half
    // is still on F-1, so the game half (SetConstants) takes its own local from
    // GameFrameToken() and this is not carried across the seam. Null when
    // Streamline is absent (the normal dormant state).
    void*                             m_slFrameToken = nullptr;
    std::unique_ptr<FXAAPass>         m_fxaaPass;

    AAMode                            m_aaMode = AAMode::TAA;  // the default AA mode
    SL::DLSSRRModel                   m_dlssRRModel = SL::DLSSRRModel::D;   // GetDLSSRRModel
    // What the user asked for, before any hardware-capability demotion. Only
    // persistence reads it - see GetAAModeRequested.
    AAMode                            m_aaModeRequested = AAMode::TAA;
    // The (requested, effective) pair NoteAAFallback last logged - a demotion
    // is named once per change, after the frame's applies.
    AAMode                            m_aaNotedRequested = AAMode::TAA;
    AAMode                            m_aaNotedEffective = AAMode::TAA;

    std::atomic<float> m_restirBudgetMs{0.f};
    bool m_restirBudgetWant=false;
    ReSTIRBudget m_restirBudget;
    std::atomic<uint32_t> m_restirBudgetStage{0};
    RenderPath m_restirBudgetPath=RenderPath::Raster;
    uint64_t m_restirBudgetFrame=0;
    std::atomic<RenderPath> m_renderPath{RenderPath::Raster};
    std::atomic<RenderPath> m_renderPathRequested{RenderPath::Raster};
    std::atomic<uint32_t> m_ptMaxBounces{8};
    std::atomic<uint32_t> m_ptAreaMode{0},m_ptAreaHistory{20},m_ptAreaSpatialK{1}; // Area spatial K 1 by default
    std::atomic<float> m_ptAreaSpatialRadius{6.f};
    std::atomic<bool> m_ptAreaReconnect{false};
    std::atomic<bool> m_ptAreaMotion{false};
    std::atomic<bool> m_ptReferenceArm{true};
    std::atomic<bool> m_ptCameraPrimary{false},m_ptFilmJitter{false},m_ptPrimaryRayCone{false};
    std::atomic<uint32_t> m_devPinned{0};   // DevPin bits
    std::atomic<float> m_ptLensRadius{0.f},m_ptFocusDistance{5.f};
    std::atomic<bool> m_ptDenoise{true};
    std::atomic<uint32_t> m_ptDenoiseFadeSpp{64};
    // The time freeze (BeginWorldClock): the setting, whether it held this frame, a pending step's
    // seconds, the steps taken, and the frame's world dt for BeginFrame (valid until it reads it).
    std::atomic<bool> m_ptFreezeTime{false};
    std::atomic<bool> m_worldClockFrozen{false};
    std::atomic<float> m_worldStepSeconds{0.f};
    std::atomic<uint32_t> m_worldStepsDone{0};
    float m_frameWorldDt = 0.f;
    bool m_frameWorldDtSet = false, m_frameWorldStepped = false;
    std::atomic<float> m_diRayTMax{150.f};
    std::atomic<uint32_t> m_restirDiSpatialPasses{1};
    std::atomic<uint32_t> m_restirDiSpatialRadiusPx{16};
    std::atomic<uint32_t> m_restirDiK{3};
    std::atomic<uint32_t> m_restirDiMCap{20};
    std::atomic<uint32_t> m_restirDiAgeCap{63};
    std::atomic<uint32_t> m_restirGiAgeCap{30};
    std::atomic<bool>     m_restirDiReuse{true};
    std::atomic<bool>     m_restirGiReuse{true};
    std::atomic<uint32_t> m_restirGiSpatialRadiusPx{8};
    std::atomic<float> m_restirGIMaxDistance{150.f};
    std::atomic<uint32_t> m_restirGITier{1}; // shadowed hit lights by default
    std::atomic<uint32_t> m_restirDiCandidates{8};
    std::atomic<bool>     m_restirDiFullRes{false};
    std::atomic<bool>     m_restirDiSpecAntiFirefly{true};
    std::atomic<bool>     m_restirGiSpatial{true};
    std::atomic<uint32_t> m_restirGiInitialCandidates{2};
    std::atomic<uint32_t> m_restirGiEmitterCandidates{8};
    std::atomic<bool>     m_restirGiFullRes{false};
    std::atomic<uint32_t> m_restirGiHitExactLights{8};
    std::atomic<bool>     m_restirGiSharc{false};      // off by default
    std::atomic<uint32_t> m_restirGiSharcMode{0};      // 0 parity, 1 multi
    std::atomic<uint8_t>  m_restirDiDenoiser{static_cast<uint8_t>(ReSTIR::Denoiser::Auto)};
    bool                  m_giActivePrevFrame = false;   // RTR reads last frame's GI
    std::atomic<uint8_t>  m_ptDirectLight{0};            // PTRtxdi::DirectLight::Auto
    uint32_t              m_staticGeometryEpochSeen = 0; // DDGISceneAS::StaticGeometryEpoch
    std::atomic<uint8_t>  m_restirGiDenoiser{static_cast<uint8_t>(ReSTIR::Denoiser::Auto)};
    const char*           m_restirDiDenoiserEffective = "relax";   // under m_restirStatusMutex
    const char*           m_restirGiDenoiserEffective = "relax";   // (static strings)
    GISharcStatus         m_giSharcStatus;             // under m_restirStatusMutex
    // ReSTIR::Backend per stage (DI engine, GI engine, PT area) and Compare.
    std::atomic<uint8_t>  m_restirBackendDI{static_cast<uint8_t>(ReSTIR::Backend::Engine)};
    std::atomic<uint8_t>  m_restirBackendGI{static_cast<uint8_t>(ReSTIR::Backend::Engine)};
    std::atomic<uint8_t>  m_restirBackendPT{static_cast<uint8_t>(ReSTIR::Backend::Auto)};   // Auto: chosen by use
    std::atomic<uint8_t>  m_restirCompare{static_cast<uint8_t>(ReSTIR::Compare::Off)};
    mutable std::mutex    m_restirStatusMutex;
    ReSTIR::BackendStatus m_restirStatus[static_cast<size_t>(ReSTIR::Stage::Count)];
    bool                  m_restirStatusPublished = false;
    uint8_t               m_restirCompareLogged = static_cast<uint8_t>(ReSTIR::Compare::Off);
    // Set by the game half when a stage's effective core changed; the render
    // half resets the budget ladder like a path switch.
    std::atomic<bool>     m_restirBackendSwitched{false};
    // Set by SetRenderPath and the budget ladder; the render half turns it into
    // a PathChange of the frame it starts (Render_BindFrameResources).
    std::atomic<bool> m_renderPathChanged{false};
    // The view's break reasons raised since the last hand-off (RaiseViewBreak,
    // any thread) and the loud ones; AdvanceSceneWrite moves them into the frame
    // it hands off.
    std::atomic<uint32_t> m_pendingViewBreaks{0};
    std::atomic<uint32_t> m_pendingViewBreaksLoud{0};
    // The main view's owner (SetMainViewOwner); game half only.
    uint32_t    m_mainViewOwner     = 0;
    const char* m_mainViewOwnerName = "";
    bool        m_mainEditorOverlays = true;   // SetMainViewEditorOverlays; game half
    // Atomic rather than a lane: an editor-set mode, not per-frame state.
    // SetViewMode writes it on the game half; RTAOWantsTlas / RTShadowWantsTlas /
    // RTScreenWantsTlas read it from both halves (the game half to decide whether
    // to gather the TLAS, the render half whether to trace), and
    // Render_BindFrameResources reads it directly. Relaxed: nothing is ordered
    // against it, and seeing a mode change a frame late is what a toggle does
    // anyway.
    std::atomic<ViewMode> m_viewMode{ ViewMode::Lit };
    std::unique_ptr<AutoExposurePass> m_autoExposurePass;
    std::unique_ptr<LumaReducePass> m_lumaReducePass;
    std::unique_ptr<BloomPass>        m_bloomPass;
    std::unique_ptr<LensFlarePass>    m_lensFlarePass;
    std::unique_ptr<ToneMapPass>      m_toneMapPass;
    // XeGTAO is a graph pass (RTAO's slot: after Decal, before Lighting), not a
    // post-process pass - the graph owns it; this is a borrowed pointer.
    XeGTAOPass*                       m_xegtaoPass = nullptr;

    // ---- Shared HDR post-process ping-pong pool ----
    // The post chain is linear and every producer fully overwrites its target,
    // so only two full-res buffers are ever live: alternating slots instead of
    // five private RGBA16F targets (79 MB at 1080p, ~315 MB at 4K) cost 32 MB.
    // Reused rather than memory-aliased on purpose: full overwrite means nothing
    // needs preserving, so there are no aliasing barriers and no
    // undefined-contents rule to get wrong. RG::PassTextures at the post size; a
    // slot's state is the device's book's - every pooled producer moves it with
    // RG::TransitionTo, so no state array travels with the pointers.
    // The main view's; a secondary view has its own at its size (SecondaryView::ppPool).
    PostPool           m_ppPool;

    std::unique_ptr<CASPass>          m_casPass;
    std::unique_ptr<UnderwaterPass>   m_underwaterPass;
    std::unique_ptr<DepthOfFieldPass> m_dofPass;
    std::unique_ptr<StylizePass>      m_stylizePass;
    // The main view's cross-frame state (RenderGraph/ViewState.h): its TAA
    // jitter (.jitter), the Hi-Z capture (.hiz) and the temporal passes'
    // histories (bound as the passes register in Compile). Slot 0.
    ViewState                         m_mainView{ "main" };

    // ---- Debug target view (Renderer_DebugTargets.cpp) ----
    // m_debugTargetMutex guards the registry (built by Compile, its availability
    // refreshed on request), the asked-for view and what the last frame showed:
    // the view is set from any thread, the render half paints it.
    // m_debugTargetWanted mirrors "a view is set" for the lock-free per-frame
    // checks.
    std::unique_ptr<DebugTargetViewPass> m_debugTargetPass;
    // Texture Viewer pictures (TextureViewService.h): applied in
    // ServiceEnableGatedResources, drawn on restoreCL before the debug target view.
    std::unique_ptr<TextureViewService>  m_textureViews;
    // UI Editor designer pictures (UIViewService.h): targets made in
    // ServiceEnableGatedResources, drawn on restoreCL beside the texture views.
    std::unique_ptr<UIViewService>       m_uiViews;
    bool                                 m_hideCanvasUI = false;   // Show > Screen UI off (editor)

    mutable std::mutex                      m_debugTargetMutex;
    mutable std::vector<DebugTargets::Info> m_debugTargetInfos;
    std::vector<DebugTargetSlot>            m_debugTargetSlots;   // parallel to m_debugTargetInfos
    DebugTargetRegistry                     m_featureDebugTargets;   // the render features' own rows (Compile)
    DebugTargets::View                      m_debugTargetView;
    DebugTargets::Shown                     m_debugTargetShown;
    std::atomic<bool>                       m_debugTargetWanted{ false };
    mutable std::atomic<bool>               m_debugTargetRefreshWanted{ false };
    bool                                    m_debugTargetPainting = false;   // render half only
    // The producers a capture asked for (the next service point arms them), and
    // the frame the render half last ran the view in.
    std::atomic<std::uint32_t>              m_debugProduceRequested{ 0 };
    std::atomic<std::uint64_t>              m_debugTargetFrame{ ~0ull };

    std::vector<std::uint32_t> m_debugCostTable;                              // its scratch
    // The visible pixels per material slot of the last materialCost frame read
    // back (frame ~0 = none yet) - Render.materialCosts()'s pixels.
    mutable std::mutex         m_debugCoverageMutex;
    std::vector<std::uint32_t> m_debugCoverage;
    std::uint64_t              m_debugCoverageFrame = ~0ull;

    // ---- Pass capture (Renderer_PassCapture.cpp) ----
    PassCaptureState   m_passCapture;
    // m_passCaptureMutex guards the latched request and the published status;
    // the rest is the serial window's (the render half reads it, parked
    // meanwhile).
    mutable std::mutex m_passCaptureMutex;

    // ---- Render passes - SSR ----
    // Single owning subsystem; individual passes accessed via GetSSRxxx().
    std::unique_ptr<SSRSubsystem>          m_ssrSubsystem;

    // ---- Transient aliasing ----
    // One shared placement heap for the four frame-transient texture groups (RTR
    // scratch / XeGTAO working set / VolFog froxel trio / Bloom chain) - each
    // group packs from offset 0, so the heap costs max(group) instead of
    // sum(groups). Disjointness proof and joining rules: TransientAliasArena.h.
    TransientAliasArena m_transientAliasArena;

    // ---- Render passes - debug & UI ----
    std::unique_ptr<DebugWirePass>         m_debugWirePass;
    std::unique_ptr<DebugIconPass>         m_debugIconPass;
    std::unique_ptr<UIPass>                m_uiPass;
    std::unique_ptr<WorldUIBillboardPass>  m_worldUIPass;

    // ---- Reflection probes ----
    ReflectionProbeManager m_probeMgr;

    // ---- DDGI ----
    DDGI::DDGIVolumeManager             m_ddgiMgr;
    std::unique_ptr<DDGIPass>           m_ddgiPass;
    // RTXDI integration probe (-rtxdi_probe): the RTXDI DI / GI / PT pipelines
    // through the bridge, never dispatched.
    std::unique_ptr<RtxdiProbe>         m_rtxdiProbe;
    std::unique_ptr<DDGIProbeDebugPass> m_ddgiProbeDebugPass;
    DDGISceneAS                         m_ddgiSceneAS;
    // -dxr_parity: the DXR pipeline traced against inline RayQuery on
    // m_ddgiSceneAS's TLAS (Renderer_RenderAsync, after the build).
    DXRParityTest                       m_dxrParity;

    // ---- Async-compute kickoff ----
    // This frame's skinning CL, published by Render_ComputePrepass so the
    // compute-queue scene-AS build can declare an explicit dependency on it.
    // Invalid when there were no skin jobs. Reset at the top of every Render().
    RHI::CommandList                    m_frameSkinCL{};
    // Kill switch for the submit barrier that lets the compute queue start
    // before the graphics prologue retires. Off = compute waits on the whole
    // opening batch, so this can be A/B'd at runtime without a rebuild. The
    // correctness edges (asCL <- skinCL) are not gated - they are always
    // declared.
    bool                                m_asyncComputeEarlyKickoff = true;
    bool                                m_ddgiReady = false;
    IndirectLightingSettingsComponent   m_ddgiSettings;

    // ---- Skeletal animation + GPU skinning ----
    SkinnedMeshSubsystem m_skin;

    // ---- Custom-material rings ----
    MaterialCBVRing       m_customMatCbvRing;
    std::vector<uint64_t> m_matCustomCbvVA;     // per-MaterialBuffer-slot GPU VA (stable with the persistent store)
    MaterialSRVRing       m_customMatSrvRing;
    std::vector<uint64_t> m_matCustomTexHandle;

    // ---- Persistent custom-material CBV / SRV store ----
    // Per-material-slot regions written once at key assignment instead of
    // per-frame ring allocations: MaterialBindingKey folds every param and every
    // resolved bindless index (engine + custom textures - see HashMatParams), so
    // a key's custom CBV bytes and SRV table are immutable for the key's life;
    // any appearance change (edit, async texture promote) mints a new key -> new
    // slot -> fresh pack. That makes m_matCustomCbvVA / m_matCustomTexHandle
    // stable per-slot values (no per-frame zero-fill / repack), which retainable
    // IndirectGroup / DrawPacket keys need.
    // Requires m_retainMaterialSlots (per-frame slot compaction would rewrite
    // live store content inside the frames-in-flight window). The rings above
    // stay selectable: flag off, oversize b8 cbuffers
    // (> MaterialCBVStore::kSlotStride) and SRV-store exhaustion all fall back to
    // per-first-sight ring packing.
    bool                  m_persistentCustomMatStore = true;
    MaterialCBVStore      m_customMatCbvStore;
    MaterialSRVStore      m_customMatSrvStore;
    std::vector<uint8_t>  m_matSlotCustomPacked;   // per slot: persistent content valid
    std::vector<uint8_t>  m_matSlotCustomOversize; // per slot: ring-pinned for the key's life (b8 > slot stride, or SRV-store exhaustion)
    std::vector<uint32_t> m_matSlotSrvBlock;       // per slot: SRV store block (~0u none); attached forever

    // A slot whose key retired at epoch E re-enters m_freeMatSlots only at
    // E + kMatSlotReuseLagEpochs: the store rewrites slot content at
    // reassignment, and in-flight frames (3 deep) must never observe it.
    std::vector<std::pair<uint32_t, uint64_t>> m_matSlotsPendingReuse; // {slot, retireEpoch}

    // ---- Decals ----
    Resource::DecalMaterialLibrary m_decalMaterialLibrary;
    DecalSpawner                   m_decalSpawner;
    DecalLifetimeSystem            m_decalLifetimeSystem;

    // ---- Post-process orchestration ----
    std::unique_ptr<PostProcess::PostChain>          m_postChain;

    // ---- Interactive water ripples (GPU height-field sim) ----
    std::unique_ptr<class WaterRippleSystem>  m_waterRippleSystem;

    // ---- VFX - particles / trails / tracers / beams ----
    std::unique_ptr<class ParticleSystem>     m_particleSystem;
    std::unique_ptr<class ParticleSimPass>    m_particleSimPass;
    class ParticleRenderPass*                 m_particleRenderPass = nullptr; // m_graph
    std::unique_ptr<class TrailSystem>        m_trailSystem;
    std::unique_ptr<class TrailUpdatePass>    m_trailUpdatePass;
    class TrailRenderPass*                    m_trailRenderPass = nullptr;    // m_graph
    std::unique_ptr<class TracerSystem>       m_tracerSystem;
    // Tracer noise textures referenced by path from .ivfx (`noiseTex=`): path ->
    // acquired handle, so repeated spawns do not re-acquire and the texture stays
    // alive while tracers sample it. Released in OnWorldClear.
    std::unordered_map<std::string, Resource::TextureHandle> m_tracerNoiseTexCache;
    std::unique_ptr<class TracerSimPass>      m_tracerSimPass;
    class TracerRenderPass*                   m_tracerRenderPass = nullptr;   // m_graph
    class BillboardFXPass*                    m_billboardFXPass = nullptr;    // m_graph
    std::unique_ptr<class BeamSystem>         m_beamSystem;
    std::unique_ptr<class BeamSimPass>        m_beamSimPass;
    std::unique_ptr<class AfterimageSystem>      m_afterimageSystem;
    std::unique_ptr<class AfterimageCapturePass> m_afterimageCapturePass;

    // ---- Terrain texture caches ----
    std::unordered_map<Entity, TerrainTexCache> m_terrainTexCache;

    // ---- Water texture cache (multi-body: two flow-normal slots per body) ----
    // Keyed on the owning entity so each water body manages its own flow-map
    // acquire / release on a path change. Reuses TerrainTexSlot (path + handle).
    // Synced by BuildScene_SyncWater; released via the deferred material-texture
    // queue on OnWorldClear (load, then release).
    std::unordered_map<Entity, WaterTexSlots> m_waterTexCache;

    // ---- Scene BVH (rebuilt per frame for frustum culling) ----
    SceneBVH m_sceneBVH;
    // Order-independent hash of the BVH leaf entity-ID set from the last
    // BuildScene_CullBVH. A change forces a full rebuild (not just a refit),
    // catching wholesale entity-set swaps that keep the leaf count unchanged:
    // world reload between same-sized scenes, and the Play -> Stop snapshot
    // AdoptState restore - neither of which routes through OnWorldClear.
    uint64_t m_cullBvhSetSig = 0;

    // BuildScene_CullBVH per-frame scratch - members so the hot path does no
    // steady-state allocation. The two masks are filled by CullBVH and read by
    // GatherMeshLibRefs.
    std::vector<uint8_t>            m_bvhVisibleMask;
    std::vector<uint8_t>            m_bvhTestedMask;
    std::vector<Entity>             m_bvhScratchEntities;
    std::vector<SceneBVH::AABB>     m_bvhScratchAABBs;
    std::vector<SceneBVH::LeafType> m_bvhScratchTypes;
    std::vector<Entity>             m_bvhScratchVisible;
    // Last frame's gathered leaf AABBs - when bit-identical to this frame's (and
    // the leaf set is unchanged) the refit is skipped.
    std::vector<SceneBVH::AABB>     m_bvhPrevAABBs;

    // ---- Render workers (dedicated threads + binary_semaphore) ----
    RenderWorker m_renderWorkers[3];   // graph slots: [0]=shadow [1]=skin+afterimage [2]=compute
    bool         m_workersStarted = false;

    // ---- World destroy-listener (per-entity cleanup hook) ----
    World*                              m_subscribedWorld         = nullptr;
    World::EntityDestroyListenerHandle  m_entityDestroyListenerId = 0;

    // ---- The secondary-view classify lane ----
    // The live secondary cameras' frusta, inflated exactly like the main one
    // (near*0.9 / far*1.1 / slopes*1.05 - BuildCameraViewProj) so an object on
    // the edge does not pop into the PiP a frame later than into the main view.
    // Rebuilt by ExtractPreviewCameras at the top of BuildRenderScene, because
    // the classify pass must know the secondary frusta before it drops
    // candidates. Game half only.
    // Six planes, not a BoundingFrustum, because of orthographic cameras:
    // DirectXCollision's BoundingFrustum is slope-based and cannot express a
    // parallel volume (the main view works around it with a conservative
    // containing cone in UploadFrameData's ortho branch). ExtractFrustumPlanes
    // handles both projections exactly and is the same form the render half's
    // own cull uses (CullWorldForPreview): both halves share one construction,
    // MakeSecondaryViewProj (Renderer_PreviewViews.cpp), called with
    // inflate=true here and inflate=false there. The plane test is also the
    // cheaper one.
    std::vector<FrustumPlanes>            m_secondaryFrusta;
    // Any secondary volume this frame: a PiP frustum above, or the probe box
    // below. The auto-exposure readback stays the PiP's alone
    // (ExtractPreviewCameras arms it before the probe joins).
    bool                                  m_secondaryCullActive = false;
    // The reflection probe baking this frame as a secondary volume. Its six
    // faces (90 deg frusta from the capture point to
    // ReflectionProbeCapturePass::kFarZ) together cover exactly the cube of that
    // half-size around the capture point, so the classify asks the cube and each
    // face's CullView asks its own frustum. Set by BuildScene_SelectProbes at the
    // top of BuildRenderScene; game half only.
    bool                                  m_probeCullActive = false;
    DirectX::XMFLOAT3                     m_probeCullMin{}, m_probeCullMax{};
    // The duplicates, appended to `candidates` immediately after classify
    // returns and before the procedural gathers (so triScene, which brackets
    // that range, is unaffected). A member for the capacity, like every other
    // per-frame scratch vector in this file.
    std::vector<DrawCandidate>            m_secondaryCands;

    // ---- BuildRenderScene: shared helpers and per-frame state ----
    // Instance-staging census, filled by BuildScene_SortAndEmit, reported under
    // -phaselog_every: of the ~8.5k candidates walked every frame, how many
    // produce a GPU write? The static lane stages only on a memcmp mismatch, so
    // in a settled scene that should be a small fraction - the gap between
    // `examined` and `staged` is the wasted motion in this loop.
    InstStageStats m_instStageStats{};

    // Per-frame draw-candidate scratch. A member so its ~1.85 MB capacity
    // survives the frame instead of being allocated and freed every time - see
    // BuildRenderScene. Declared after DrawCandidate because it needs the type
    // complete. Never read outside that one call.
    std::vector<DrawCandidate> m_candScratch;
    // The procedural rows (billboards, beams, afterimages), gathered before the
    // material snapshots and appended after the classify. Capacity persists like
    // m_candScratch's.
    std::vector<DrawCandidate> m_proceduralScratch;

    // Per-worker gather shards for BuildScene_GatherMeshLibRefs. Retained
    // members: a function-local vector<vector<DrawCandidate>> costs 1 +
    // numWorkers malloc / free pairs plus ~3 MB of reserve traffic per frame, and
    // its inner vectors' 24-byte headers sit contiguously, so every push_back
    // stores to a line two other workers are writing. alignas(64) gives each lane
    // its own line; retaining them keeps the capacity, so the reserve is a no-op
    // after frame 1. These are the shards feeding the merge; m_candScratch is its
    // destination.
    std::vector<GatherLane> m_gatherLanes;

    // QueueSpawnMaterialResolve's entities, resolved at the top of
    // BuildRenderScene (the serial window that allows registration) - then the
    // queue is empty.
    std::vector<Entity> m_spawnMaterialResolve;

    // ---- A prefab preload's renderer half (Renderer_Preload.cpp) ----
    // OnWorldClear's count (GetWorldClearCount). Main thread.
    uint64_t m_worldClearCount = 0;

    // ---- Retained-scene dirty feed ----
    SceneItemStore       m_sceneStore;
    std::vector<Entity>  m_sceneDirty;        // key changed (or add) this frame
    // Subsets of m_sceneDirty: only the world matrix changed (high-frequency -
    // editor drag / mover scripts; retained consumers patch in place) vs any
    // other key field (visibility / tags / mesh identity - membership may change,
    // consumers do a full re-gather that frame).
    std::vector<Entity>  m_sceneDirtyXform;
    std::vector<Entity>  m_sceneDirtyOther;
    std::vector<Entity>  m_sceneAdded;        // subset of m_sceneDirty: newly tracked
    std::vector<Entity>  m_sceneRemoved;      // untracked this frame (destroy / component removed)
    std::vector<Entity>  m_sceneMatDirty;     // material re-synced this frame (event accelerator)
    std::vector<Entity>  m_sceneRemovedEvents;// destroy-listener accelerator, drained by DirtyDiff
    bool                 m_retainedSceneDiff = true;   // retained scene diff on / off

    // Telemetry accumulators (rolling max between throttled logs).
    uint32_t m_sceneDiffPeakDirty = 0, m_sceneDiffPeakAdd = 0,
             m_sceneDiffPeakRemove = 0, m_sceneDiffPeakMat = 0;

    // ---- Retained draw-candidate cache ----
    // The classify moves with the plan. Set by main in BuildRenderScene (only
    // when the plan goes to the lane too: the classify's output is PlanEmit's
    // input) and cleared by the lane once PrepareFrameOnLane ran it. Main hands
    // over the unclassified rows in m_candScratch plus the procedural rows in
    // m_proceduralScratch; the lane classifies, appends the secondary-view
    // duplicates, then the procedural rows - main's order. -laneclassifyoff keeps
    // it on main.
    bool           m_classifyOnLaneThisFrame = false;
    ClassifyInputs m_laneClassifyIn{};
    // -laneclassifyverify: main classifies a copy with the live World reads
    // (the reference), the lane compares its result row by row.
    std::vector<DrawCandidate> m_laneClassifyRef;
    bool           m_laneClassifyRefValid  = false;
    uint32_t       m_laneClassifyAabbStale = 0;   // rows whose box differs from the pool
    uint32_t       m_laneClassifyBitsStale = 0;   // rows whose matBits differ from *mc
    uint64_t       m_laneClassifyFrames = 0, m_laneClassifyMismatchFrames = 0;

    uint64_t m_candCacheStaticSig  = 0;       // ComputeCandidateStaticSetSig at snapshot time
    std::vector<DrawCandidate> m_candCache; // static-lane, pre-classification, gather order
    std::vector<uint32_t> m_candEntityToIdx;  // dense entity id -> first cache index (~0u = absent)
    uint32_t m_candCachePrimCount  = 0;       // cache[0..primCount) came from the primitive gather
    uint32_t m_candCacheLastStamp  = 0;       // stamp-continuity gate (mirrors DDGISceneAS)
    uint64_t m_candCacheSetSig     = 0;       // ComputeCandidateSetSig at snapshot time
    Entity   m_candCachePickEntity = NullEntity; // m_pickingOutlineEntity at snapshot time
    uint32_t m_candUnresolved      = 0;       // meshes still loading - skipped by the last gather
    uint32_t m_matTexUnsettled     = 0;       // material resources with pending textures this frame
    // Which resources those are, and the entities bound to them (filled by
    // SyncMaterials only on a frame that has any). A retained cache whose own
    // rows bind none of them need not re-gather: what is still arriving is
    // somebody else's texture.
    std::unordered_set<uint64_t> m_matTexUnsettledKeys;
    std::vector<Entity>          m_matTexUnsettledEntities;

    bool     m_candCacheValid      = false;   // re-armed by whichever gather path completes
    bool     m_retainedCandidates  = true;    // false = the per-frame gather path

    // ---- Static rows not re-walked on a patch frame ----
    // How a patch frame avoids three per-row passes over the ~7,000 cached rows:
    //  * the MaterialComponent pointer / matBits re-resolve (Patch.Cache) - needed
    //    because pool growth or compaction moves material storage without a
    //    tracked event - runs only when the pool's StructureVersion() moved since
    //    the cache last re-resolved (or the pool is another object);
    //  * the mirror bit is kept in the cache (written back after the frame that
    //    built it, recomputed on every transform patch);
    //  * the cache keeps each row's matSnap, and the slots it names are stamped
    //    every frame (a list of the distinct slots, not the rows) so
    //    RetireMatSnapshots never takes them; a volatile material still
    //    re-acquires every frame (its snapshot is refreshed there). A
    //    snapshot-table clear (GCMatSnapshots) drops the retention for that
    //    frame. triScene's static share is cached too.
    // -candretainoff is the reverse arm, -candretainverify the oracle.
    const void*           m_candCacheMatPool        = nullptr;  // pool the pointers came from
    uint64_t              m_candCacheMatPoolVersion = 0;        // its StructureVersion() then
    std::vector<uint32_t> m_candCacheSrc;        // cache row -> candidate index when snapshotted
    bool                  m_candCacheWriteBack = false;  // snapshotted this frame: write back after MatSnap
    bool                  m_candCacheRetained  = false;  // cache rows carry valid matSnap + mirrored
    // Distinct snapshot slots the retained rows name, with the key each held at
    // write-back: stamped every patch frame, and a slot whose key moved voids
    // the retention for that frame.
    std::vector<std::pair<uint32_t, uint64_t>> m_candCacheSnapKeys;
    std::vector<uint32_t> m_candCacheVolatile;   // cache rows that still acquire every frame (sorted)
    bool                  m_candCacheHasSnaps  = false;  // some cache row carries a retained matSnap
    uint64_t              m_candCacheTris      = 0;      // triScene share of the cached rows
    uint64_t              m_matSnapClearGen    = 0;      // bumped by every GCMatSnapshots clear
    uint64_t              m_candCacheSnapGen   = 0;      // m_matSnapClearGen at write-back
    uint64_t              m_candRetainFrames = 0, m_candRetainFallbacks = 0, m_candMatRefreshSkips = 0;
    bool                  m_candRetainOffThisFrame = false;  // -candretainoff / -candretainab this frame

    // ---- The static rows' copy goes to the lane too (-laneassemble) ----
    // With the retention usable and the tracking + classify on the lane, main
    // does not copy the cached rows into m_candScratch: main's list is
    // [dynamic prims | dynamic meshlib], and the lane assembles the patch-frame
    // layout [cached prims | dynamic prims | cached meshlib | dynamic meshlib]
    // into m_candLaneScratch and swaps it in. A volatile cached row is acquired
    // in the cache (on main), so the copy carries this frame's matSnap.
    // m_candCache is written only by main inside BuildRenderScene and by a
    // drained world clear, so the lane reads it between the hand-off and the
    // barrier. Opt-in (a net loss on the GPU-bound Game - see BuildRenderScene);
    // -laneassembleverify has main assemble the same list at the hand-off and the
    // lane compare.
    bool                       m_laneAssembleThisFrame  = false;
    uint32_t                   m_laneDynPrimCount       = 0;     // main's list: [0, this) = dynamic prims
    std::vector<DrawCandidate> m_candLaneScratch;
    // Some cached row passes the surface-cache page gate (PrepareLanePlan's
    // atlas prediction, which does not see the cached rows in main's list).
    // Kept current wherever the rows' predicates can change.
    bool                       m_candCacheAtlasEligible = false;
    std::vector<DrawCandidate> m_laneAssembleRef;
    bool                       m_laneAssembleRefValid   = false;
    uint64_t                   m_laneAssembleFrames = 0, m_laneAssembleBadFrames = 0;

    // ---- Retained dynamic rows ----
    std::vector<DrawCandidate> m_dynCandCache;   // gather order, pre-classification
    bool     m_dynCandCacheValid = false;         // armed by a dynamic-row snapshot
    // What the rows' mesh slots and custom-shader ids were resolved against:
    // MeshManager's MeshLibRef slot generation (a bump makes the gather
    // re-take every non-skinned slot) and the GBuffer shader library's reload
    // epoch (a bump makes it re-sync reflection). Either moving = fall back.
    uint32_t m_dynCandCacheMeshGen = 0, m_dynCandCacheShaderEpoch = 0;
    uint64_t m_dynPatchFrames = 0, m_dynPatchFallbacks = 0, m_dynPatchRows = 0,
             m_dynPatchRehash = 0;
    uint32_t m_dynPatchLastFail = 0;              // why the last patch fell back (0 = none)

    // ---- The dynamic rows' patch as jobs ----
    // Every input of the patch gate is final right after SyncMaterials, and what
    // main does between there and the patch (CullBVH, the debug wireframes, the
    // snapshot GC / retire, the static cache patch + copy) writes none of what
    // the patch reads (World pools, the runtime material map, m_dynCandCache). So
    // the patch is kicked there, as worker jobs that copy their range of
    // m_dynCandCache into m_dynAsyncRows and patch it in place, and joined at the
    // head of the patch path; main then appends the rows. Anything that voids
    // the result in between - GCMatSnapshots clearing the dynamic cache's
    // validity, the mesh generation / shader epoch moving, the static patch
    // failing so the frame full-gathers - discards it and the synchronous path
    // runs. The join is also forced before anything writes m_dynCandCache (the
    // gather's snapshot) and on unwinding. -dynpatchasyncoff keeps it
    // synchronous.
    std::vector<DrawCandidate>             m_dynAsyncRows;   // keeps its size across frames
    std::array<DynPatchLaneResult, 32>     m_dynAsyncResults{};
    std::optional<Sys::CpuTaskScope>       m_dynAsyncScope;
    uint32_t m_dynAsyncLanes = 0;
    uint32_t m_dynAsyncMeshGen = 0, m_dynAsyncShaderEpoch = 0;
    bool     m_dynAsyncKicked = false;
    uint64_t m_dynAsyncFrames = 0, m_dynAsyncDiscards = 0;

    // ---- BuildRenderScene static / dynamic split (-brssplit, default off) ----
    std::optional<Sys::CpuTaskScope> m_brsEarlyScope;   // in flight across phases
    std::array<BrsLaneResult, kBrsEarlyLanes> m_brsEarlyLaneResults{};
    const void* m_brsEarlyMatData  = nullptr; // pool guard: component storage
    const void* m_brsEarlyMatDense = nullptr; // pool guard: dense entity array
    size_t   m_brsEarlyMatSize     = 0;
    size_t   m_brsEarlyCacheSize   = 0;
    uint32_t m_brsEarlyLanes       = 0;
    bool     m_brsEarlyPending     = false;   // submitted, not yet joined
    bool     m_brsEarlyResultLive  = false;   // joined, lanes agreed, not yet consumed
    bool     m_brsEarlyReady       = false;   // consumed by this frame's patch arm
    uint64_t m_brsEarlyAttempts = 0, m_brsEarlyUsed = 0, m_brsEarlyRejected = 0;

    // ---- Retained BVH leaves + retained tag sets ----
    // Shared membership gate, computed once per frame in BuildRenderScene right
    // after BuildScene_DirtyDiff (the stamp-continuity check consumes the stamp,
    // so per-consumer re-checks would always see a gap). true = the diff is
    // continuous and carried no membership-affecting event (added / removed /
    // dirtyOther all empty) and no dirtyXform entity crossed a retained tag-set
    // boundary - reparenting fires dirtyXform (the world matrix recomputes), not
    // dirtyOther, so subtree moves need the explicit boundary walk below.
    bool     m_sceneMembershipStable  = false;
    uint32_t m_retainedSceneLastStamp = 0;

    // ---- Dynamic-only adds ----
    std::vector<Entity> m_addsSorted;          // m_sceneAdded, sorted (membership tests)
    bool     m_addsDynamicOnly = false;
    bool     m_addsSkinnedOnly = false;
    bool     m_addsOtherOwn    = false;        // every dirtyOther entry is an add
    bool     m_addsMatOwn      = false;        // every matDirty entry is an add
    bool     m_bvhAddPatchFrame = false;       // CullBVH may patch: continuous + dynamic-only adds

    // ---- Retained BVH leaves + retained tag sets ----
    // BuildScene_TagSetBoundaryStable's ancestor memo, flat and indexed by
    // entity id, with an epoch stamp instead of a per-call hash map (that map's
    // ~4K inserts per frame on AIBench200 were the whole cost of the BRS.TagSets
    // zone). Mutable: the gate is const and single-threaded.
    mutable std::vector<uint32_t>       m_tagBoundaryStamp;
    mutable std::vector<TagBoundaryAnc> m_tagBoundaryAnc;
    mutable std::vector<Entity>         m_tagBoundaryChain;
    mutable uint32_t                    m_tagBoundaryEpoch = 0;
    mutable uint64_t                    m_tagBoundaryChecks = 0, m_tagBoundaryMismatches = 0;

    // (a) BVH: the leaf arrays (m_bvhScratch*) persist across frames; a patch
    // frame refits only SceneDiffView.dirtyXform leaves (WorldAabb pool read)
    // plus the skinned leaves (animation moves WorldAabb without transform
    // dirt). Any membership event / stamp gap / unresolved row falls back to the
    // full gather + rebuild / refit path that frame. FrustumCull stays per frame
    // (camera-dependent).
    // m_bvhPrevAABBs stays: the flag-off fallback still needs its memcmp
    // refit-skip and every full-gather frame refreshes it; on patch frames the
    // dirty feed subsumes it (per-leaf compare inside the patch), so the two
    // never disagree - a full frame always re-baselines both.
    bool     m_retainedBvh      = true;   // false = per-frame gather
    bool     m_bvhRetainedValid = false;  // armed by a completed full gather
    uint32_t m_bvhSkippedRows   = 0;      // MeshLibRef rows not turned into leaves
                                          // (missing WorldAabb / untracked in the
                                          // scene store) - their later resolve makes no
                                          // diff event, so > 0 pins the full path
    uint32_t m_bvhWorldAabbCount = 0;     // WorldAabb pool size at the last full
                                          // gather - component add/remove is invisible
                                          // to the diff, so a count change disarms
                                          // the patch path
    std::vector<uint32_t> m_bvhEntityToLeaf;  // dense entity id -> leaf idx (~0u absent)
    std::vector<uint32_t> m_bvhDynamicLeaves; // LeafType::Skinned leaves, re-read per patch frame

    // (b) Tag sets: membership (player / hair shadow, face overlay) persists;
    // rebuilt when the per-frame seed signature changes (the tag pools are not
    // part of SceneItemKey - a tag add / remove produces no diff event, so the
    // seeds must be re-scanned every frame; they are O(#tagged), a handful) or on
    // any membership event. The merged AABBs still recompute every frame over the
    // retained membership (they track skinned animation), and the hair artistic
    // params stay per-frame raw reads (inspector scrub). Dissolve is exempt by
    // contract: a small live set, and the Edit-mode inspector scrub previews the
    // raw amount (see BuildScene_ComputeDissolveSet).
    bool     m_retainedTagSets      = true;  // false = per-frame BFS
    bool     m_tagSetsRetainedValid = false; // armed after the sets ran over a valid diff
    uint64_t m_playerSetSeedSig   = 0;
    uint64_t m_hairSetSeedSig     = 0;
    uint64_t m_faceOverlaySeedSig = 0;

    // ---- Post-gather phases ----
    // The spot-caster tracking and the light prep go to the lane with the
    // classify (the tracking must see the unclassified rows, and the prep
    // consumes what the tracking produced that frame). Main keeps
    // ClusterPass::SetLightsMeta (DDGI reads its count and world grid on main).
    // Set by main in BuildRenderScene, cleared by the lane's PrepareFrameOnLane.
    // -lanelightoff keeps both on main; so do -earlylightprep / -lightprepab.
    bool            m_lightPrepOnLaneThisFrame = false;
    LightPrepInputs m_laneLightIn{};
    bool            m_laneSpotRetainedDiff = false;
    bool            m_laneSpotMatTexUnsettled = false;

    std::vector<ProbeSelectEntry> m_probeSelected;  // the GPU order: priority DESC, volume ASC, entity ASC
    bool m_probeSelectOverflowed = false;           // more probes than kMaxReflectionProbes / free slots
    bool m_probeSelectRan        = false;           // false: the upload publishes no probe (no buffer, none in the World)
};
