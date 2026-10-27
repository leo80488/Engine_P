#pragma once

// PointShadowPass - omnidirectional (cubemap) shadow maps for point lights with
// LightData::castsShadow. The mirror of SpotShadowPass, but each caster owns a full cube
// (6 faces) instead of one slice.
//
// Storage: one D32_FLOAT TextureCubeArray; cube c is array slices [c*6 .. c*6+5] in D3D face
// order (+X,-X,+Y,-Y,+Z,-Z). ResourceMiscFlag::TEXTURECUBE with arraySize == 6*N makes the RHI
// create the TextureCubeArray<float> SRV (GraphicsDX12::CreateTexture); the per-slice DSVs let
// SetDepthStencilSlice / ClearDepthStencilSlice address face f of cube c as slice c*6 + f.
//
// Each frame:
//   1. The Renderer gives each shadow-casting point light a cube index and six face
//      view-projections (90 deg fov, reversed-Z perspective with near = radius /
//      far = kNearPlane, the SpotShadowPass convention, so Shadow.vs/ps and the comparison
//      sampler work unchanged).
//   2. This pass clears every active face, then draws the casters depth-only once per face.
//   3. Lighting.ps (SamplePointShadow) samples the cube by (worldPos - lightPos) and compares
//      against a reversed-Z depth reconstructed from the fragment's distance to the light.
//      shadowSliceIdx == 0xFFFFFFFF skips the sampling (no shadow).
//
// Shaders: Shadow_VS / Shadow_PS (the depth-only pair CSM and SpotShadow use).
// Root signature, as SpotShadowPass:
//   b1 space0 - ShadowPerViewCB (per-face shadowViewProj)
//   t0 space0 - InstanceBuffer
//   t1 space0 - MeshDescriptors
//   t0 space1 - bindless g_Buffers[]

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "Graphics/IndirectDrawCommand.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class PointShadowPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kMaxCasters   = 4;       // cubes (each = 6 faces)
    static constexpr uint32_t kFacesPerCube = 6;
    static constexpr uint32_t kShadowMapSize = 1024;
    static constexpr uint32_t kInvalidSliceIdx = 0xFFFFFFFFu;
    // Reversed-Z near plane shared with Lighting.ps SamplePointShadow. The
    // far plane is the light's radius (per-caster). Keep both sides in sync.
    static constexpr float    kNearPlane = 0.05f;

    // ---- Per-cube GPU-culled indirect args ----
    static constexpr uint32_t kShadowGroupCount = 8;
    struct RetainedShadowArgs
    {
        bool                  active = false;
        const RHI::GPUBuffer* workingArgs [kMaxCasters] = {};
        const RHI::GPUBuffer* countBuffer = nullptr;   // (view x bucket) u32 grid
        uint32_t              remapHeapIdx[kMaxCasters] = {};
        uint32_t              countBase   [kMaxCasters] = {}; // element base into countBuffer
        uint32_t              groupBase[kShadowGroupCount] = {}; // command slots
        uint32_t              groupCap [kShadowGroupCount] = {};
    };

    struct ServiceRequest { bool wanted = false; };

    PointShadowPass();
    ~PointShadowPass();

    const char* GetName() const override { return "PointShadowPass"; }
    // Each cube face's size (256 / 512 / 1024 / 2048). Set BEFORE Init and DeclareImports:
    // Renderer::Compile passes the player's shadows.pointResolution; the shader reads it back off
    // the atlas.
    void     SetFaceSize(uint32_t px) { m_faceSize = px ? px : kShadowMapSize; }
    // Default face size (settings.json shadows.pointResolution); the size in use is FaceSize(),
    // set before Init.
    uint32_t FaceSize() const         { return m_faceSize; }
    // The cube atlas is this pass's import (RG::PassTex::kShadowPointCube): allocated by
    // EnsureAtlas, transitioned by the graph; Lighting and the forward passes declare reading it.
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup (RG::RenderGraphBuilder& b) override { b.WriteDepthStencil(m_atlasH); }
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs, including the one-shot
    // DEPTHSTENCIL -> SRV transition the first caster-less frame still owes
    // LightingPass (m_firstExecution).
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_atlas.IsValid() && (m_activeCount != 0 || m_firstExecution));
    }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame BEFORE graph Execute.
    // activeCount = number of point lights that opted-in this frame (<= kMaxCasters).
    void SetActiveCasterCount(uint32_t n) { m_activeCount = (n > kMaxCasters) ? kMaxCasters : n; }
    void SetFaceMatrix(uint32_t cube, uint32_t face, const DirectX::XMFLOAT4X4& viewProj)
    {
        if (cube < kMaxCasters && face < kFacesPerCube)
            m_pendingVP[cube * kFacesPerCube + face] = viewProj;
    }
    // Scene time + camera for custom-VS (WPO) casters, the dissolve noise index and the
    // instance-remap heap index; written into each face CB at byte offsets 192/208/224/228
    // (matches custom_vertex.hlsli CUSTOM_VS_SHADOW + Shadow.ps ShadowPerViewCB).
    void SetCustomVsTime(float t, float dt, const DirectX::XMFLOAT3& camPos,
                         uint32_t dissolveNoiseIdx = ~0u,
                         uint32_t instanceRemapIdx = 0)
    { m_cvTime = t; m_cvDeltaTime = dt; m_cvCameraPos = camPos;
      m_dissolveNoiseIdx = dissolveNoiseIdx;
      m_instanceRemapIdx = instanceRemapIdx; }

    // true (default): honor MaterialComponent CAST_SHADOW and the VisibilityComponent shadow
    // viewMask, as CSM does. false ignores both (a compat flag in the Renderer).
    void SetHonorShadowFlags(bool b) { m_honorShadowFlags = b; }

    // The Renderer dispatches InstanceCull.cs mode 2 once per CUBE (not per face) against the
    // light's bounding sphere, as the 6 axis-aligned planes of its bounding box, and hands the
    // result here. All six faces of cube c replay c's culled list: per-face 90 deg frusta would
    // only shave an already-tiny list (Renderer.h kPointCullViewBase has the arithmetic).
    // Per-cube arrays take the cube index (SetFaceMatrix's `cube`): face f uses c = f / kFacesPerCube.
    // kShadowGroupCount == ShadowDrawGroup::RetainedCount: the four buckets and their meshlet twins.
    // These views keep DRAW commands, so a twin draws exactly like its base bucket.
    void SetRetainedShadowArgs(const RetainedShadowArgs& ra) { m_ra = ra; }

    // Consumed by Lighting.ps as a TextureCubeArray<float>. Returns 0 while the atlas is not
    // resident (see EnsureAtlas); the consumer needs a fallback
    // (LightingPass::m_pointShadowAtlasFallbackTex).
    uint64_t                  GetCubeAtlasSrvHandle() const;
    uint32_t                  GetAtlasSize() const { return m_faceSize; }
    static constexpr uint32_t GetMaxCasters() { return kMaxCasters; }

    // ---- Lazily allocated cube atlas ----
    // At 1024^2 x 24 faces of D32 the atlas is 96 MiB, so a world without a shadow-casting point
    // light allocates none. Mirrors SpotShadowPass::EnsureAtlas: allocated at the frame boundary
    // (OnService, from Renderer::ServiceEnableGatedResources) on demand from a DIRECT LightData pool
    // scan - never from this pass's caster count, which a residency-gated block produces and which
    // would deadlock at zero. Released per world from OnWorldClear.
    void EnsureAtlas(bool wanted);
    void ReleaseAtlas();
    uint32_t HandledEvents() const override { return kWorldClear; }   // Renderer::PassSet events this pass handles
    // A new world gives the atlas back - not the editor's Play / Stop, which restores the same
    // world two frames later, nor a restore that destroyed nothing (the release rule is explained
    // where the Renderer registers the shadow passes).
    void OnWorldClear(const RG::WorldClearArgs& a) override { if (!a.worldSurvives && !a.idsPreserved) ReleaseAtlas(); }

    // The service point: allocate when a caster wants the atlas - never a release (that is per
    // world, in OnWorldClear).
    ServiceRequest& Request() { return m_request; }
    void OnService(RG::ServiceContext&) override { EnsureAtlas(m_request.wanted); }
    bool IsAtlasResident() const { return m_atlas.IsValid(); }

    // Builds now, on the calling thread (a preload's TaskSystem worker), the PSO(s) Execute would
    // build for @p dp, a material's synthetic packet: its group PSO (alpha-test bucket, or the
    // opaque one with Shadow_PS unless ShadowDrawNeedsPS says it can go; cull is NONE for every
    // bucket) or its per-draw custom-VS PSO. Mirrors the record path's choice; the PSO cache is
    // thread-safe (single flight). Nothing is bound or drawn.
    void WarmPipelines(const DrawPacket& dp);

private:
    static constexpr uint32_t kFaceCount     = kMaxCasters * kFacesPerCube;
    static constexpr uint32_t kFaceCBStride  = RHI::kConstantBufferAlignment; // D3D12 CBV alignment
    struct FaceCBPool
    {
        // Two banks (as ShadowPass::CascadeCBPool): bank one [0, kFaceCount) carries the pristine
        // instance-remap heap index at byte 228 and serves the ring path and the per-draw custom-VS
        // loop; bank two [kFaceCount, 2*kFaceCount) is the same bytes with the owning cube's WORKING
        // remap index, bound only by retained group draws.
        uint8_t slots[2 * kFaceCount * kFaceCBStride];
    };

    static constexpr uint32_t kMaxIndirectCommands = 32768;
    static constexpr uint32_t kFrameCount          = RHI::kFramesInFlight;

    // depthOnly drops the PS (PSODesc::kNoPixelShader): legal only when no packet in the group can
    // discard (ShadowDrawNeedsPS). hookPSID != 0: a masked custom material's own PS in its
    // SHADOW_HOOK variant, fed by its custom VS or ShadowHook_VS (ShadowCasterShaders.h).
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customVSID = 0,
                         bool depthOnly = false, uint32_t hookPSID = 0) const;
    void    UploadFaceCBs();

    ServiceRequest m_request;   // set by Renderer::ServiceEnableGatedResources, read by OnService

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    // Custom-VS (WPO) caster time + camera (written into each face CB @192).
    float             m_cvTime      = 0.0f;
    float             m_cvDeltaTime = 0.0f;
    DirectX::XMFLOAT3 m_cvCameraPos = { 0.0f, 0.0f, 0.0f };
    // Dissolve noise bindless index (face CB offset 224; ~0u = none).
    uint32_t          m_dissolveNoiseIdx = ~0u;
    // Instance-remap heap index (face CB offset 228; 0 = identity).
    uint32_t          m_instanceRemapIdx = 0;

    RHI::Texture m_atlas;  // Texture2DArray<D32_FLOAT> with TEXTURECUBE -> TextureCubeArray SRV (see EnsureAtlas)
    RG::RGTextureHandle m_atlasH;   // its graph import
    // Create-failure latch, as in SpotShadowPass: ENGINE_CHECK_ONCE throttles the log, this stops
    // a 96 MiB committed allocation attempt every frame. Cleared by ReleaseAtlas so a new world
    // retries once.
    bool m_allocFailed = false;
    uint32_t m_faceSize = kShadowMapSize;   // SetFaceSize, before Init

    // One 256-byte CBV slot per face (kMaxCasters * 6 faces) in each bank; one pool per frame in
    // flight.
    FrameCB<FaceCBPool> m_faceCBs;

    DirectX::XMFLOAT4X4 m_pendingVP[kFaceCount]{};
    uint32_t            m_activeCount = 0;

    // Indirect draw args for the batched opaque draws (same layout as SpotShadowPass /
    // ShadowPass): built once per frame, replayed for every face. One UPLOAD buffer per frame in
    // flight.
    RHI::GPUBuffer m_indirectArgBuffer[kFrameCount];
    void*          m_indirectArgMapped[kFrameCount] = {};

    IGraphicsDevice* m_gfxPtr = nullptr;

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps's g_LinearWrap).
    int  m_alphaSamplerIdx = -1;

    // Transition from DEPTHSTENCIL on the first frame, DEPTH_READ_SRV after.
    bool m_firstExecution = true;

    // Honor castShadow + the shadow viewMask (see SetHonorShadowFlags).
    bool m_honorShadowFlags = true;

    // Per-frame retained-args state (see RetainedShadowArgs).
    RetainedShadowArgs m_ra;
};
