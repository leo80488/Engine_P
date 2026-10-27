#pragma once

// VolumetricFogPass - froxel volumetric fog (sun, height fog, local volumes).
// Per enabled frame:
//   1. FroxelDensity     compute  -> 3D RGBA16F (.rgb scattering, .a extinction)
//   2. FroxelLightInject compute  -> 3D RGBA16F (.rgb sigma_s x L_in, .a extinction); the RT tier
//                                   runs FroxelIndirectRT (its coarse indirect grid) and then
//                                   FroxelLightInjectRT instead
//   3. FroxelTemporal    compute  -> 3D RGBA16F (same layout, reprojected / blended)
//   4. FroxelScatter     compute  -> 3D RGBA16F (.rgb integral of T x L, .a transmittance)
//   5. VolumetricApply   graphics -> composites the scatter volume onto scene HDR in a
//                                   fullscreen triangle, after lighting, before bloom / tone map.
// Temporal runs BEFORE Scatter on purpose: only the un-integrated (sigma_s x L_in,
// sigma_t) volume is a local voxel property that survives reprojection
// (FroxelTemporal.cs.hlsl has the rationale).
//
// The fog parameters have two sources:
//   * SCENE - VolumetricFogComponent (global medium) + LocalFogVolumeComponent
//     (bounded pockets), gathered on the game half by the Renderer and pushed every
//     frame via SetSceneFog / SetLocalFogVolumes. A scene with a
//     VolumetricFogComponent owns the global medium outright.
//   * PASS-OWNED - SetEnabled / SetDensity / ... (logged as "legacy fog"), written
//     by the scene's .ippc / Project Settings; used only while the scene has no
//     VolumetricFogComponent.
// Local volumes inject in both modes and keep the pipeline running on their own, so
// a scene can have "no global fog, one smoky room". The player's allowance
// (SetAllowed) caps all of it.

#include "Graphics/FrameConstants.h"
#include "Graphics/BindlessKind.h"   // BINDLESS_NULL_CBV
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/GpuLight.h"
#include "Graphics/FrameCB.h"
#include "Graphics/SharcCache.h"   // the indirect rays' view of the path tracer's cache

#include <cstddef>
#include <vector>
#include <DirectXMath.h>

class TransientAliasArena;
namespace Media { struct Global; struct FroxelVolume; }

// HLSL mirrors: FroxelConstants is FroxelParams (FroxelCommon.hlsli), VolApplyConstants
// is VolApplyCB (VolumetricApply.ps.hlsl) - keep the layouts in sync. They live here so
// the per-frame CBs can be FrameCB<> members of the pass.
struct alignas(16) FroxelConstants
{
    float    viewProj[16];
    float    invViewProj[16];
    float    prevViewProj[16];

    float    cameraPos[3];   float nearPlane;
    float    farPlane;       float froxelNear;
    float    froxelFar;      float temporalAlpha;

    uint32_t frameIndex;
    uint32_t froxelW;
    uint32_t froxelH;
    uint32_t froxelD;

    float    fogDensity;
    float    fogScattering;
    float    fogAbsorption;
    float    anisotropy;

    float    heightFogStart;
    float    heightFogFalloff;
    float    ambientContribution;
    float    _pad0;

    float    sunDir[3];      float sunStrength;
    float    sunColor[3];    float _pad1;
    float    ambientColor[3];float _pad2;

    // CSM cascades.
    float    shadowMatrix0[16];
    float    shadowMatrix1[16];
    float    shadowMatrix2[16];
    float    cascadeSplits[4];
    float    shadowParams[4]; // x=texelSize, y=volumetric light count (as float), z=bias, w=strength
    float    cameraForward[3];float _pad3;

    // Voxel occupancy grid - written by SceneVoxelPass, read by FroxelLightInject for
    // spot / point light occlusion: a world-space AABB around the camera, snapped to the
    // voxel size.
    float    voxelGridMin[3];    float _padVG0;
    float    voxelGridExtent[3]; uint32_t voxelGridDim; // extent = gridMax - gridMin

    // ---- The frame's LightCB and medium set, opened from the heap (mirror: FroxelCommon.hlsli) ----
    // A typed CBV handle to the frame's LightCB (null = none). VolumetricRaymarch reads
    // the cloud shadow there - the map's index and its basis - so the shafts and the lit
    // surfaces use one copy. rmMediumIdx: the medium set (Graphics/MediumSet.h) the raymarch's
    // sun crosses on its way to each step (pass.volumetricFog.raymarchSunTransmittance; 0 = none).
    uint32_t lightCbIdx;         uint32_t rmMediumIdx;   uint32_t _padCS0[2];

    // ---- Scene fog extensions (mirror: FroxelCommon.hlsli, appended) ----
    // The albedo tints sigma_s (1,1,1 for pass-owned fog); the emissive is premultiplied
    // by its intensity and scaled by sigma_t in the shader. The local-volume counts index
    // the t5 StructuredBuffer; emissive volumes are sorted first so LightInject walks
    // only the first localEmissiveCount.
    float    fogAlbedo[3];       uint32_t localVolumeCount;
    float    fogEmissive[3];     uint32_t localEmissiveCount;

    // ---- Last frame's camera (mirror: FroxelCommon.hlsli, appended) ----
    // Temporal reprojects by the distance to it in every build; rtFlags is the FROXEL_RT builds'.
    float    prevCameraPos[3];   uint32_t rtFlags;

    // ---- The ray-traced lighting (mirror: FroxelCommon.hlsli's FROXEL_RT block, appended) ----
    // Read only by the FROXEL_RT builds; heap indices, 0 = none.
    uint32_t mediumIdx, lightsIdx, lightCount, lightGridIdx;
    uint32_t lightIndexIdx, lightMapIdx, lightMediumScaleIdx, pathMaterialsIdx;
    uint32_t densityIdx, reservoirIdx, prevReservoirIdx, candidates;
    uint32_t alphaTestEnabled, alphaInstanceCount; float rtTemporalAlpha, sunMediumScale;
    float    clusterNear, clusterFar; uint32_t prevLightCount, volLightsIdx;
    // The indirect light (FroxelIndirectRT.cs.hlsl): its source, its grids, the sky, the SHaRC cache,
    // the grid the injection reads; the ray-traced shafts' output and the scene depth.
    uint32_t indirectMode, indirectIdx, prevIndirectIdx, skyIdx;
    float    environmentScale, indirectAlpha; uint32_t sharcCapacity, sharcHashUav;
    uint32_t sharcAccumulationUav, sharcResolvedUav; float sharcSceneScale, sharcRadianceScale;
    uint32_t indirectReadIdx, shaftsIdx, sceneDepthIdx, _padRt;
};
static_assert(offsetof(FroxelConstants, prevCameraPos) == 640, "FroxelCommon.hlsli's last-frame camera row is at 640");
static_assert(offsetof(FroxelConstants, indirectMode) == 736, "FroxelCommon.hlsli's indirect rows follow volLightsIdx");

struct alignas(16) VolApplyConstants
{
    float    nearZ;
    float    farZ;
    float    froxelNear;
    float    froxelFar;
    uint32_t froxelDepth;
    uint32_t _pad0;
    uint32_t _pad1;
    uint32_t _pad2;
};

class VolumetricFogPass : public RG::RenderPass
{
public:
    // The scene's VolumetricFogComponent, resolved by the Renderer (SetSceneFog).
    struct SceneFog
    {
        bool  owned          = false;
        bool  enabled        = false;
        bool  godRays        = true;
        bool  temporal       = true;
        float density        = 0.0f;
        float anisotropy     = 0.4f;
        float baseHeight     = 0.0f;
        float heightFalloff  = 0.05f;
        float ambientIntensity = 0.4f;
        float startDistance  = 0.5f;
        float viewDistance   = 80.0f;
        float temporalBlend  = 0.12f;
        DirectX::XMFLOAT3 albedo       { 1.0f, 1.0f, 1.0f };
        DirectX::XMFLOAT3 ambientColor { 0.4f, 0.55f, 0.85f };
        DirectX::XMFLOAT3 emissive     { 0.0f, 0.0f, 0.0f };  // premultiplied by intensity
    };

    // One LocalFogVolumeComponent, resolved to world -> unit-volume space.
    // Mirror: LocalFogVolumes.hlsli (96 B, StructuredBuffer at t5 space2).
    struct LocalFogVolumeGPU
    {
        float    worldToLocal[12];  // 3 rows: local = (dot(p1,r0), dot(p1,r1), dot(p1,r2))
        float    albedo[3];   float density;
        float    emissive[3]; float edgeFade;     // emissive premultiplied by intensity
        uint32_t shape;       float heightFalloff; float halfHeight; float _pad;
    };
    static_assert(sizeof(LocalFogVolumeGPU) == 96, "LocalFogVolumeGPU must match LocalFogVolumes.hlsli");
    static constexpr uint32_t kMaxLocalFogVolumes = 32;

    using VolLight = GpuShared::GPULight;

    // The SCENE half of EnsureFroxelResources' decision, gathered on the game half
    // (EnsureFroxelResources says why it is not the render-half SceneFog).
    struct FogDemand
    {
        bool sceneOwned   = false;   // a VolumetricFogComponent exists
        bool sceneEnabled = false;
        bool sceneGodRays = false;
        bool anyLocal     = false;   // at least one local volume survived culling
        bool rt           = false;   // the ray-traced lighting may run (its row is not off)
    };

    // ---- The ray-traced lighting (pass.volumetricFog.rt*; FramePlan::Decisions::fogRt) ----
    // Off: the raster lighting. Hybrid (the hybrid tiers): the raymarched shafts keep the sun and
    // the shadow-casting spots, the froxels light every other light. Full: the sun and every light
    // in the froxels, no shafts. ForwardOnly: the path tracer integrates the camera's fog; the
    // froxels are lit for the surfaces drawn after it (fog_apply.hlsli) and composite nothing.
    enum class RtTier : uint8_t { Off, Hybrid, Full, ForwardOnly };
    // What the ray-traced injection reads, from the renderer after the plan's stage B. Heap
    // indices are 0 when absent.
    struct RtInputs
    {
        uint64_t tlasVA = 0, instanceVA = 0, alphaVA = 0;   // root SRVs (ReSTIR's shell layout)
        uint64_t buffers = 0;                               // the bindless buffer table
        uint32_t alphaTestEnabled = 0, alphaInstanceCount = 0;
        uint32_t pathMaterialsIdx = 0;
        bool     glass = false;                             // the TLAS holds a Transmissive instance
        uint32_t mediumIdx = 0;                             // Graphics/MediumSet.h's set, on every frame
        float    sunMediumScale = 1.f;                      // the sun's fog scale (VolumetricLightComponent)
        // ClusterPass: the lights, the cluster grid and index list, the previous -> current key
        // map and the previous count, the lights' fog scales; the cluster grid's planes.
        uint32_t lightsIdx = 0, lightCount = 0, lightGridIdx = 0, lightIndexIdx = 0;
        uint32_t lightMapIdx = 0, prevLightCount = 0, lightMediumScaleIdx = 0;
        float    clusterNear = 0.1f, clusterFar = 1000.f;
        // The indirect light's sources: DDGI runs this frame (its volumes in the frame's LightCB),
        // the sky cube the rays miss into and its scale (the path tracer's), the path tracer's SHaRC
        // cache when it resolves one earlier in this frame.
        bool     ddgi = false;
        uint32_t skyIdx = 0;
        float    environmentScale = 1.f;
        SharcCache::Lookup sharc;
    };

    struct ServiceRequest { FogDemand demand; };

    VolumetricFogPass(RG::RGTextureHandle depth);
    ~VolumetricFogPass();

    const char* GetName() const override { return "VolumetricFogPass"; }
    // The froxel scattering volume is this pass's import (RG::PassTex::kVolFogScattering;
    // one 160x90 slice of the 3D volume in the snapshot): placed in the transient arena by
    // EnsureFroxelResources and transitioned by this pass (Owner). The forward passes read
    // it after this pass - an arena texture read across passes that only the pass order
    // keeps safe.
    // TODO(RenderGraph_AutoTopology_NodeView_Plan M6): declare that read so the DAG sees the volume's lifetime.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's first two early-outs (the third, no bound view, is a wiring
    // error Create3DTextures reports). Everything below them (the alias barriers, the
    // teleport test, the CB upload) belongs to a frame that actually dispatches.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (WantsFog() && Shown()
                && m_v->densityTex.IsValid() && m_densityPSO.IsValid());
    }
    // A view that hides the volumetric fog (the main view's wireframe, debug view modes,
    // an ortho camera, the path tracer's camera primary) skips it and its forward fog
    // (RenderGraph/SceneView.h).
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::VolumetricFog); }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }
    // Clearing the two above leaves the six compute PSOs (members, built once)
    // on the old code; this rebuilds them too.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // It records any view: the scene's medium, lights and local volumes are the frame's, the froxel
    // volumes, the raymarch target and the camera and cascade inputs the bound view's. The view's
    // fog histories (RenderGraph/ViewState.h) are created here, empty - EnsureFroxelResources makes
    // the main view's textures, BindView a secondary view's.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // ---- Scene-authored fog (pushed by the Renderer on the render half) ----
    // The resolved VolumetricFogComponent. `owned` = the component exists; it then
    // replaces every pass-owned value below.
    void SetSceneFog(const SceneFog& f);
    const SceneFog& GetSceneFog() const { return m_scene; }

    // `volumes` must already be culled/sorted (emissive first); anything past
    // kMaxLocalFogVolumes is dropped. Copies into this frame's upload slot.
    void SetLocalFogVolumes(const std::vector<LocalFogVolumeGPU>& volumes,
                            uint32_t emissiveCount);
    uint32_t GetLocalFogVolumeCount() const { return m_localCount; }

    // THE fog predicate: Execute, IsActive and the Renderer's voxel gating all ask it, so
    // "runs" never means two things. The scene's intent (its component, else the
    // pass-owned flag, or any local volume), capped by the player's allowance (SetAllowed).
    bool WantsFog() const
    {
        return m_allowed && ((m_scene.owned ? m_scene.enabled : m_enabled) || m_localCount > 0);
    }
    bool WantsGodRays() const
    {
        return WantsFog() && (m_scene.owned ? m_scene.godRays : m_rmEnabled);
    }
    // The froxel scattering volume this frame's apply samples, for the surfaces drawn
    // AFTER it (Renderer::WriteFogApplyCB -> fog_apply.hlsli). False when the apply adds
    // no fog: no fog, hidden by the view mode, or a volume / pipeline the dispatches
    // decline without (a declined scatter leaves the arena's bytes in the volume). @p srv
    // is re-derived every frame: the volume lives in the transient alias arena and may
    // move. Reading it after the apply extends group C's window to the last VFX pass;
    // TransientAliasArena.h says why that is safe.
    bool ForwardScattering(uint64_t& srv, float& froxelNear, float& froxelFar,
                           uint32_t& depth) const;

    // The global medium as a participating medium for the path tracers (Graphics/MediumSet.h):
    // the scene's component or the pass-owned values, under the player's allowance, without the
    // froxel range. False when there is none; the asymmetry, which the local volumes scatter
    // with, is written whenever the fog is allowed. Render half, after SetSceneFog.
    bool WriteMediumGlobal(Media::Global& out) const;

    // The ray-traced lighting's mode (pass.volumetricFog.rtLighting: 0 off, 1 auto, 2 on), read
    // every frame.
    static uint8_t RtMode();
    // Where the ray-traced lighting takes its indirect light (pass.volumetricFog.rtIndirect: 0 off,
    // 1 auto, 2 ddgi, 3 rays), read every frame.
    static uint8_t RtIndirectMode();
    // The device traces rays and the ray-traced pipelines have not failed to build.
    bool RtUsable() const;
    // This frame's ray-traced tier and its inputs: the render half, after the plan's stage B and
    // before the graph records. A tier the pass cannot run this frame (no pipelines, no reservoirs,
    // no TLAS) lights the froxels the raster way; ForwardOnly still composites nothing.
    void SetRt(RtTier tier, const RtInputs& in);
    // This frame lights the froxels by ray tracing: the fog runs, a tier, its pipelines, its
    // reservoirs, a TLAS. Render half, after SetRt and the frame's RenderGraph::SetView.
    bool RtRuns() const;
    // The bound view's last frame was ray-traced: the scene-voxel occlusion the raster injection
    // reads is not needed (BindViewInputs gates SceneVoxelPass on it, before this frame's plan).
    bool RtRanLastFrame() const { return m_hist && m_hist->rtLast; }
    // @p view's lighting volume as its last Execute left it (Temporal's history: sigma_s x the light
    // scattered toward that view's camera, and sigma_t), for the path tracer's continuation
    // segments. Render half, before this frame's Execute; srv 0 when there is none (no temporal pass,
    // no fog in that view).
    Media::FroxelVolume LastLitVolume(const ViewState& view) const;

    // Froxel grid range actually used this frame (both CBs must agree).
    float EffectiveFroxelNear() const { return m_scene.owned ? m_scene.startDistance : m_froxelNear; }
    float EffectiveFroxelFar()  const { return m_scene.owned ? m_scene.viewDistance  : m_froxelFar;  }
    bool  EffectiveTemporal()   const { return m_scene.owned ? m_scene.temporal      : m_temporalEnabled; }

    // ---- Pass-owned configuration (the scene's .ippc / Project Settings) ----
    // Ignored while the scene owns its fog (SceneFog::owned). IsEnabled() is the
    // pass-owned flag - the .ippc's switch, which CaptureFrom must round-trip - not "fog
    // runs" (that is WantsFog()). Only the scene writes it
    // (Renderer::SetFogSceneConfigIntent); the player's features.volumetricFog is the
    // ALLOWANCE (SetAllowed), over every kind of fog.
    void  SetEnabled(bool on)      { m_enabled = on; }
    bool  IsEnabled() const        { return m_enabled; }
    void  SetAllowed(bool on)      { m_allowed = on; }
    bool  IsAllowed() const        { return m_allowed; }

    void  SetDensity(float d)      { m_fogDensity = d; }
    float GetDensity() const       { return m_fogDensity; }

    void  SetScattering(float s)   { m_fogScattering = s; }
    float GetScattering() const    { return m_fogScattering; }

    void  SetAbsorption(float a)   { m_fogAbsorption = a; }
    float GetAbsorption() const    { return m_fogAbsorption; }

    void  SetAnisotropy(float g)   { m_anisotropy = g; }
    float GetAnisotropy() const    { return m_anisotropy; }

    void  SetHeight(float start, float falloff) { m_heightStart = start; m_heightFalloff = falloff; }
    float GetHeightStart() const   { return m_heightStart; }
    float GetHeightFalloff() const { return m_heightFalloff; }

    void  SetRange(float nearKm, float farKm) { m_froxelNear = nearKm; m_froxelFar = farKm; }
    float GetRangeNear() const     { return m_froxelNear; }
    float GetRangeFar()  const     { return m_froxelFar;  }

    void  SetAmbient(const DirectX::XMFLOAT3& col, float contrib)
    { m_ambientColor = col; m_ambientContribution = contrib; }
    float GetAmbientContribution() const { return m_ambientContribution; }
    const DirectX::XMFLOAT3& GetAmbientColor() const { return m_ambientColor; }

    void  SetTemporalEnabled(bool on) { m_temporalEnabled = on; }
    bool  IsTemporalEnabled() const   { return m_temporalEnabled; }
    void  SetTemporalAlpha(float a)   { m_temporalAlpha = a; }
    float GetTemporalAlpha() const    { return m_temporalAlpha; }

    // Per-pixel raymarch tier (sun + shadow-casting spots), alongside the froxel grid
    // (lights without shadows). On by default; the editor can toggle it for debugging.
    void  SetRaymarchEnabled(bool on) { m_rmEnabled = on; }
    bool  IsRaymarchEnabled() const   { return m_rmEnabled; }

    // Per-frame state pushed by the Renderer. @p prevViewProj is ignored: the history
    // snapshots its own at the end of Execute (History::prevViewProj).
    void SetFrameState(const DirectX::XMFLOAT4X4& viewProj,
                       const DirectX::XMFLOAT4X4& invViewProj,
                       const DirectX::XMFLOAT4X4& prevViewProj,
                       const DirectX::XMFLOAT3&   cameraPos,
                       float nearPlane, float farPlane,
                       uint32_t frameIndex);

    // @p dir is LightCB's lightDir convention: the direction the light travels.
    void SetSun(const DirectX::XMFLOAT3& dir,
                const DirectX::XMFLOAT3& color,
                float strength)
    { m_sunDir = dir; m_sunColor = color; m_sunStrength = strength; }

    // ---- Debug accessors (read by EditorLayer) ----
    const DirectX::XMFLOAT3& GetSunDirDebug()   const { return m_sunDir; }
    const DirectX::XMFLOAT3& GetSunColorDebug() const { return m_sunColor; }
    float GetSunStrengthDebug()                 const { return m_sunStrength; }
    uint32_t GetVolumetricLightCount()          const { return m_spotLightCount; }

    // @p shadowSrvHandle is the 3-cascade CSM Texture2DArray; the matrices and splits come
    // from LightCB.
    void SetShadowState(uint64_t shadowSrvHandle,
                        const DirectX::XMFLOAT4X4 shadowMatrix[3],
                        const DirectX::XMFLOAT3& cascadeSplits,
                        float texelSize, float bias, float strength);

    // Volumetric-only lights, uploaded each frame: the Renderer pushes the lights that
    // have a VolumetricLightComponent (and matching LightData). A light without one only
    // lights surfaces and is invisible in the fog. The record is the shared GPULight
    // (shaders/shared/gpu_light.hlsli); power / sourceRadius stay 0 here.
    void SetVolumetricLights(const std::vector<VolLight>& lights);

    // World-space unit camera forward: the light injection picks the CSM cascade by view
    // Z with it. This one, SetFrameState, SetShadowState and SetLightCB write the bound view's
    // inputs (the render half: BindViewInputs, RenderAsync).
    void SetCameraForward(const DirectX::XMFLOAT3& fwd) { m_v->cameraForward = fwd; }

    void SetSceneTargets(uint64_t depthSrvHandle,
                         RG::RGTextureHandle hdrColor)
    { m_depthSrv = depthSrvHandle; m_hdrColor = hdrColor; }

    // Wire the spot-light shadow atlas and its per-slice VP buffer (SpotShadowPass /
    // Renderer). A 0 handle binds its 1x1 stand-in instead (Init); when either is 0 every
    // clustered light falls back to ScreenSpaceShadow + VoxelOcclusion.
    void SetSpotShadowAtlas(uint64_t atlasHandle, uint64_t vpsHandle)
    {
        m_spotShadowAtlasSrv = atlasHandle;
        m_spotShadowVPSrv    = vpsHandle;
    }

    // The frame's LightCB CBV (a typed CBV handle, null = none), pushed by the Renderer
    // each frame from the render slot's buffer. The raymarched shafts read the cloud
    // shadow there - the map's heap index (0 = no map: clouds off, bake skipped, strength
    // 0) and its basis rows - the same values the lit surfaces read, so the two always
    // agree on where the shadows are.
    void SetLightCB(uint32_t heapIdx) { m_v->lightCbIdx = heapIdx; }

    // Wire the scene-voxel occupancy grid from SceneVoxelPass: its bounds go into
    // FroxelConstants so the light injection can raymarch the occupancy for spot / point
    // light occlusion. @p occupancySrvGpuHandle 0 disables voxel occlusion (every sample
    // counts as visible). The bound view's, like the camera.
    void SetVoxelOcclusion(uint64_t occupancySrvGpuHandle,
                           const DirectX::XMFLOAT3& gridMin,
                           const DirectX::XMFLOAT3& gridMax,
                           uint32_t gridDim)
    {
        m_v->voxelOccupancySrv = occupancySrvGpuHandle;
        m_v->voxelGridMin      = gridMin;
        m_v->voxelGridMax      = gridMax;
        m_v->voxelGridDim      = gridDim;
    }

    // Enable-gated froxel volumes (5 x 160x90x128 RGBA16F = 70.3 MiB). Called by the
    // Renderer every frame on the MAIN thread (texture and descriptor creation must not
    // race the graph's parallel recorders): creates them on the first frame fog is
    // wanted, releases them (deferred) when it is switched off; steady state is one
    // validity compare.
    //
    // @p demand is the SCENE half of the decision, gathered on the game half (the
    // render-half SceneFog may be a frame behind and belongs to the other thread). The
    // pass-owned half is m_enabled / m_rmEnabled, which only the main thread writes.
    void EnsureFroxelResources(const FogDemand& demand);

    // The service point: the froxels for the scene's demand.
    ServiceRequest& Request() { return m_request; }
    // The graph's import then follows a recreated scattering volume.
    void OnService(RG::ServiceContext&) override
    {
        EnsureFroxelResources(m_request.demand);
        m_scatteringImport = m_v->scatteringTex;
    }

    // Shared transient-aliasing heap (Renderer-owned, set once at wiring).
    // The frame-transient trio (density/lighting/scattering) is placed into
    // it by Create3DTextures; the temporal history ping-pong stays committed.
    void SetAliasArena(TransientAliasArena* arena) { m_aliasArena = arena; }

private:
    static constexpr uint32_t kFroxelW = 160;
    static constexpr uint32_t kFroxelH = 90;
    // Z slices: each slice's discrete value shows as horizontal banding on spot-cone
    // walls; 128 halves the step of 64 at ~2x the scatter + temporal work.
    static constexpr uint32_t kFroxelD = 128;
    // The ray-traced indirect light's grid: one cell per 4 x 4 x 2 froxels (FroxelCommon.hlsli's
    // kFroxelIndirectCell).
    static constexpr uint32_t kIndirectW = (kFroxelW + 3) / 4;
    static constexpr uint32_t kIndirectH = (kFroxelH + 3) / 4;
    static constexpr uint32_t kIndirectD = (kFroxelD + 1) / 2;

    struct History final : ViewHistory
    {
        // Temporal reprojection (the froxel ping-pong).
        RHI::Texture        tex[2];
        RHI::ResourceState  state[2] = {
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE };
        uint32_t            readIdx  = 0;
        uint32_t            writeIdx = 1;
        bool                valid    = false;

        // The per-pixel raymarch tier's half-res ping-pong.
        RHI::Texture        rmTex[2];
        RHI::ResourceState  rmState[2] = {
            RHI::ResourceState::UNORDERED_ACCESS,
            RHI::ResourceState::UNORDERED_ACCESS };
        uint32_t            rmReadIdx  = 0;
        uint32_t            rmWriteIdx = 1;
        bool                rmValid    = false;

        // Last frame's viewProj (the reprojection) and camera position (the teleport guard:
        // a scene switch or a debug-cam jump bypasses the blend for that frame), snapshot at
        // the end of Execute. An identity prevViewProj would make every reprojected UV miss,
        // so the first SetFrameState (firstFrameDone) seeds both with that frame's values.
        DirectX::XMFLOAT4X4 prevViewProj{};
        DirectX::XMFLOAT3   prevCameraPos { 0, 0, 0 };
        bool                firstFrameDone = false;
        // tex[readIdx] holds the lighting volume the last Execute wrote, lit for prevViewProj
        // (LastLitVolume).
        bool                lit = false;

        // The ray-traced lighting's (EnsureRtVolumes, while its tier may run): the light reservoirs
        // (8 B a froxel; both stay UAVs), this frame's [rtReservoirSlot] and the last, reused when the
        // last frame was ray-traced (rtLast); the indirect grid's ping-pong (RGBA16F, froxel / (4, 4, 2),
        // created and kept as UAVs; rtIndirectRan: the last frame wrote it) and the rays' grid smoothed
        // for the injection (the Filter entry).
        RHI::GPUBuffer      rtReservoir[2];
        RHI::ResourceState  rtReservoirState[2] = { RHI::ResourceState::UNDEFINED, RHI::ResourceState::UNDEFINED };
        uint32_t            rtReservoirSlot = 0;
        RHI::Texture        rtIndirect[2];
        RHI::Texture        rtIndirectSmoothed;
        uint32_t            rtIndirectSlot = 0;
        bool                rtLast        = false;
        bool                rtIndirectRan = false;

        History() { DirectX::XMStoreFloat4x4(&prevViewProj, DirectX::XMMatrixIdentity()); }
        const char* Name() const override { return "VolumetricFog"; }
        // The content cuts - a hard cut, the camera's, a world clear: alpha 1 for that frame
        // (Execute reads the view's FrameBreaks) - and a new render size (the raymarch half
        // notices it). The 5 m teleport guard and fog switched off (the volumes released)
        // are the pass's own policy.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t) override {}
        void Release(IGraphicsDevice& gfx) override;
        void ReleaseRt(IGraphicsDevice& gfx);
    };

    // One view's frame-transient froxel trio, its raymarch target and this frame's camera, cascade
    // and LightCB inputs. The main view's trio is placed in the alias arena, a secondary view's is
    // committed.
    struct Volumes
    {
        // 3D textures (RHI). SRV / UAV descriptors live in the backend pool and
        // are queried per-use via IGraphicsDevice::GetTextureSRV/UAVGpuHandle.
        RHI::Texture        densityTex;
        RHI::Texture        lightingTex;
        RHI::Texture        scatteringTex;
        RHI::ResourceState  densityState    = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        RHI::ResourceState  lightingState   = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        RHI::ResourceState  scatteringState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        // The per-pixel raymarch tier's half-res target (its ping-pong is the History's).
        RHI::Texture        rmCurrentTex;
        RHI::ResourceState  rmCurrentState = RHI::ResourceState::UNORDERED_ACCESS;
        uint32_t            rmHalfW     = 0;
        uint32_t            rmHalfH     = 0;
        uint32_t            rmLastFullW = 0;
        uint32_t            rmLastFullH = 0;

        // Per-frame CB values (filled by Renderer via SetFrameState / SetCameraForward).
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT4X4 invViewProj{};
        DirectX::XMFLOAT3   cameraPos { 0, 0, 0 };
        float               nearPlane  = 0.1f;
        float               farPlane   = 1000.0f;
        uint32_t            frameIndex = 0;
        DirectX::XMFLOAT3   cameraForward { 0, 0, 1 };

        // Its cascades (SetShadowState) and the frame's LightCB CBV in the heap (SetLightCB; the
        // cloud shadow lives there).
        uint64_t            shadowSrv = 0;
        DirectX::XMFLOAT4X4 shadowMatrix[3]{};
        DirectX::XMFLOAT3   cascadeSplits{ 10, 50, 200 };
        float               shadowTexelSize = 1.0f / 2048.0f;
        float               shadowBias      = 0.0005f;
        float               shadowStrength  = 1.0f;
        uint32_t            lightCbIdx      = BINDLESS_NULL_CBV;   // a typed CBV handle

        // Its voxel occupancy grid (SceneVoxelPass -> LightInject). A 0 SRV / zero extent makes
        // the shader treat everything as visible.
        uint64_t            voxelOccupancySrv = 0;
        DirectX::XMFLOAT3   voxelGridMin { 0, 0, 0 };
        DirectX::XMFLOAT3   voxelGridMax { 0, 0, 0 };
        uint32_t            voxelGridDim = 0;

        Volumes()
        {
            DirectX::XMStoreFloat4x4(&viewProj,    DirectX::XMMatrixIdentity());
            DirectX::XMStoreFloat4x4(&invViewProj, DirectX::XMMatrixIdentity());
            for (DirectX::XMFLOAT4X4& m : shadowMatrix) DirectX::XMStoreFloat4x4(&m, DirectX::XMMatrixIdentity());
        }
    };
    struct ViewVolumes : ViewData
    {
        Volumes vol;
        void Release(IGraphicsDevice& gfx) override;
    };

    static constexpr uint32_t kMaxVolLights = 16;
    static constexpr uint32_t kFrameCount   = RHI::kFramesInFlight;

    void Create3DTextures(IGraphicsDevice& gfx);
    // The bound view's trio and froxel history (deferred); fog switched off.
    void ReleaseFroxelVolumes();
    // A secondary view's volumes after the main view's (EnsureFroxelResources decides them at the
    // frame boundary): its trio and history while the main view's live, its raymarch target at its
    // render size in its segment.
    void MatchMainVolumes(const ViewState& view);
    void CreateComputePipelines(IGraphicsDevice& gfx);
    // The frame-transient trio only (density / lighting / scattering), placed in the
    // shared alias arena when there is one; separate so an arena epoch change can rebuild
    // them without touching the history.
    void CreateTransient3DTextures(IGraphicsDevice& gfx);
    static RHI::TextureDesc FroxelDesc();

    // Density and Temporal take their raster or FROXEL_RT build (@p pso).
    void DispatchDensity(RHI::CommandList cl, const RHI::PipelineState& pso);
    void DispatchLightInject(RHI::CommandList cl);
    // @p srcSrv is the (sigma_s x L_in, sigma_t) volume to integrate: the lighting
    // texture, or the temporal history when the temporal pass ran.
    void DispatchScatter(RHI::CommandList cl, uint64_t srcSrv);
    // Returns true when the dispatch actually ran (PSO + history textures
    // valid); the caller needs that to know whether the history slot holds
    // this frame's filtered lighting volume.
    bool DispatchTemporal(RHI::CommandList cl, const RHI::PipelineState& pso);
    void DrawApply(RHI::CommandList cl);

    // ---- The ray-traced lighting ----
    // The FROXEL_RT builds and the injection's layout and pipeline, once (a failure is final
    // until a shader reload).
    bool BuildRtPipelines(IGraphicsDevice& gfx);
    void ReleaseRtPipelines(IGraphicsDevice& gfx);
    // The bound view's reservoirs and indirect grids: made while @p rtOn, released otherwise.
    void EnsureRtVolumes(bool rtOn);
    // This frame's indirect source (FroxelCommon.hlsli's kFroxelIndirect*) and, for the rays, whether
    // they query the SHaRC cache.
    uint32_t ResolveIndirect(bool& sharc) const;
    void DispatchIndirectRT(RHI::CommandList cl, const RHI::PipelineState& pso, bool smooth);
    void DispatchLightInjectRT(RHI::CommandList cl);
    // The sun's shafts per pixel (FroxelLightInjectRT.cs.hlsl's Shafts) into the raymarch's
    // half-resolution texture, which its temporal pass and composite then take.
    void DispatchShaftsRT(RHI::CommandList cl);

    // Per-pixel raymarch tier.
    void DestroyRaymarchTextures();
    void RebuildRaymarchTextures(uint32_t fullW, uint32_t fullH);
    void DispatchRaymarch(RHI::CommandList cl);
    void DispatchRaymarchTemporal(RHI::CommandList cl);
    void DrawRaymarchApply(RHI::CommandList cl);

    uint64_t WriteApplyCB();

    void                   LogSceneFogStateIfChanged();

    ServiceRequest m_request;   // read by OnService

    IGraphicsDevice*        m_gfx = nullptr;
    ShaderLibrary           m_shaderLib;
    PSOCache                m_psoCache;

    // Compute PSOs.
    RHI::PipelineState      m_densityPSO;
    RHI::PipelineState      m_lightInjectPSO;
    RHI::PipelineState      m_scatterPSO;
    RHI::PipelineState      m_temporalPSO;

    // The froxel volumes, the raymarch target and the camera inputs: the main view's, and m_v the
    // bound view's (BindView). The graph import's source is a copy of the bound view's scattering
    // volume: the graph takes it at each Execute's start.
    Volumes             m_mainVol;
    Volumes*            m_v = &m_mainVol;
    RHI::Texture        m_scatteringImport;
    RG::RGTextureHandle m_scatteringH;   // its graph import (DeclareImports)

    // ---- Transient aliasing (contract: TransientAliasArena.h) ----
    TransientAliasArena* m_aliasArena  = nullptr;
    uint32_t             m_placedEpoch = 0;
    uint8_t              m_placedMask  = 0;  // bit: 0=density 1=lighting 2=scattering

    // ---- The view's histories (RenderGraph/ViewState.h) ----
    // One per view, created empty by BindView. The froxel ping-pong is made by
    // Create3DTextures (after the arena trio, from EnsureFroxelResources at the frame
    // boundary; a secondary view's by BindView), the raymarch one by RebuildRaymarchTextures
    // (after RM.Current).
    History* m_hist = nullptr;   // the bound view's (BindView)
    // The bound view: a content cut in its FrameBreaks forces alpha 1, ANDed with the
    // history's valid and the teleport guard, so one cut clears every temporal effect in
    // lockstep (as XeGTAOPass::m_view).
    const ViewState*                       m_view = nullptr;
    bool                                   m_temporalEnabled = true;
    // 0.12 weights the history heavily (~8-frame decay) yet follows real scene changes
    // in ~3 frames; 0.05 keeps the history too long and smears under camera motion.
    float                                  m_temporalAlpha   = 0.12f;

    // ---- Constant buffers and per-frame inputs ----
    // FroxelParams (FroxelConstants), one slot per frame in flight.
    FrameCB<FroxelConstants>  m_paramCB;
    // Apply-pass CB (graphics root sig, b3 space0): just enough for the PS to recover
    // view Z from depth and slice the froxel grid. This frame's block from the frame's
    // constants, written before both apply draws (WriteApplyCB) and bound by each;
    // 0 = none.
    uint64_t m_applyVA = 0;

    // Scene-side state (per-frame).
    RG::RGTextureHandle m_depth;                  // read as an SRV in Setup, so the graph emits its barrier
    RG::RGTextureHandle m_hdrColor{};
    uint64_t            m_depthSrv = 0;
    bool                m_enabled  = false;
    bool                m_allowed  = true;          // the player's features.volumetricFog

    // Sun / shadow.
    DirectX::XMFLOAT3      m_sunDir       { 0, -1, 0 };
    DirectX::XMFLOAT3      m_sunColor     { 1, 1, 1 };
    float                  m_sunStrength  = 5.0f;
    // 1x1 fully-lit CSM stand-in, bound whenever the real shadow array is absent so the
    // shaders never sample an unbound t1 space2 (created in Init).
    RHI::Texture        m_shadowFallbackTex;
    uint64_t            m_shadowFallbackHandle = 0;

    // Spot-light shadow atlas SRVs (t6 / t7 space2), set by the Renderer each frame.
    // 0 = none: the stand-ins below are bound and the lights use ScreenSpaceShadow +
    // VoxelOcclusion.
    uint64_t                m_spotShadowAtlasSrv = 0;
    uint64_t                m_spotShadowVPSrv    = 0;
    // 1x1 stand-ins for the pair above, bound whenever the real atlas is absent
    // (SpotShadowPass allocates it lazily, so 0 is the normal state in a scene without a
    // shadow-casting spot). Built like LightingPass::Init's, in Init, which says why
    // arraySize and the texel value are both load-bearing.
    RHI::Texture            m_spotShadowAtlasFallbackTex;
    uint64_t                m_spotShadowAtlasFallback = 0;
    RHI::GPUBuffer          m_spotShadowVPFallbackBuf;
    uint64_t                m_spotShadowVPFallback    = 0;

    // Volumetric-only lights: a separate UPLOAD buffer (at most kMaxVolLights), one per
    // frame in flight so the CPU memcpy for frame N+1 cannot race frame N's GPU read.
    RHI::GPUBuffer         m_volLightsBuf[kFrameCount];
    void*                  m_volLightsBufMapped[kFrameCount] = {};
    uint64_t               m_lightsSrv[kFrameCount]          = {}; // SRV of m_volLightsBuf[i]
    uint32_t               m_spotLightCount     = 0;

    // Local fog volumes: the same per-frame UPLOAD ring as the lights. Bound at t5 space2
    // by Density and LightInject on EVERY dispatch (a zero count still needs a live
    // descriptor behind the declared slot).
    RHI::GPUBuffer         m_localFogBuf[kFrameCount];
    void*                  m_localFogBufMapped[kFrameCount] = {};
    uint64_t               m_localFogSrv[kFrameCount]       = {};
    uint32_t               m_localCount         = 0;
    uint32_t               m_localEmissiveCount = 0;
    // Last logged fog state (owned / enabled flags, the allowance, the local and emissive
    // counts): the state line is printed on CHANGE only, so a harness log can prove which
    // path ran.
    uint32_t               m_loggedFogState     = 0xFFFFFFFFu;

    // Scene-authored global medium (see SetSceneFog).
    SceneFog               m_scene;

    // ---- Per-pixel raymarch tier ----
    // Half-resolution 2D textures (the view's Volumes and History), rebuilt when the render size changes.
    RHI::PipelineState  m_rmPSO;
    RHI::PipelineState  m_rmTemporalPSO;
    bool                m_rmEnabled         = true;
    uint32_t            m_rmLogKey          = 0;   // the last [VOLFOG_RM] line's state (0: none yet)

    // Pass-owned fog parameters (used while the scene has no VolumetricFogComponent).
    float                  m_fogDensity         = 0.02f;
    float                  m_fogScattering      = 0.6f;
    float                  m_fogAbsorption      = 0.02f;
    float                  m_anisotropy         = 0.4f;
    float                  m_heightStart        = 0.0f;
    float                  m_heightFalloff      = 0.05f;
    float                  m_froxelNear         = 0.5f;
    float                  m_froxelFar          = 80.0f;
    DirectX::XMFLOAT3      m_ambientColor       { 0.4f, 0.55f, 0.85f };
    float                  m_ambientContribution = 0.4f;

    // ---- The ray-traced lighting ----
    // The FROXEL_RT builds of Density / Temporal (the compute signature) and the injection on
    // ReSTIR's shell layout, built the first frame the tier runs.
    RHI::PipelineLayout    m_rtLayout;
    RHI::PipelineState     m_rtDensityPSO, m_rtInjectPSO, m_rtTemporalPSO;
    bool                   m_rtFailed = false;
    RtTier                 m_rtTier = RtTier::Off;
    RtInputs               m_rtIn;
    // The frame boundary's decision (FogDemand::rt): the views' histories hold the reservoirs and
    // the grids while it stands (EnsureRtVolumes; MatchMainVolumes for a secondary view).
    bool                   m_rtVolumes = false;
    uint32_t               m_rtLogKey  = 0;   // the last [VOLFOG_RT] line's state (0: off)

    // ---- The ray-traced indirect light (FroxelIndirectRT.cs.hlsl) ----
    // Its pipelines on the shell layout (the SHaRC query's is a 16-bit build, made where the device
    // and the tree have the cache) and the Filter entry that smooths the rays' grid; the grids are
    // the view's (History).
    RHI::PipelineState     m_rtIndirectPSO, m_rtIndirectSharcPSO;
    RHI::PipelineState     m_rtIndirectFilterPSO;
    // The sun's shafts traced per pixel (pass.volumetricFog.rtShafts), on the shell layout; invalid:
    // the froxels keep the sun.
    RHI::PipelineState     m_rtShaftsPSO;
};
