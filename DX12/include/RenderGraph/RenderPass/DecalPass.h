#pragma once

// DecalPass - compute-only clustered decals, between GBufferPass and LightingPass in
// the render graph. Two dispatches per frame:
//   1. DecalClusterCull: assign decal OBBs to clusters, reusing ClusterPass's cluster
//      AABB buffer (no second AABB build).
//   2. DecalApply: per-pixel blend of decal textures into GBuffer0/1/2 through UAVs.
// External inputs: the cluster AABB SRV (ClusterPass), the scene depth SRV and the
// three GBuffer textures as UAVs. The graph transitions them from Setup's
// declarations: GBuffer RT -> UAV (DecalPass) -> SRV (LightingPass); depth ->
// DEPTH_READ_SRV (valid for compute).

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's grid)
#include "ECS/Components.h"

#include <DirectXMath.h>
#include <vector>
#include <cstdint>

class IGraphicsDevice;

class DecalPass : public RG::RenderPass
{
public:
    // kMaxDecals: decals per frame (the upload ring's size). kMaxPerCluster MUST match
    // MAX_DECALS_PER_CLUSTER in decal_common.hlsli; raise the two together if the debug
    // heatmap shows clusters saturating.
    static constexpr uint32_t kMaxDecals       = 512;
    static constexpr uint32_t kMaxPerCluster   = 16;

    // CPU-side decal record: the Renderer builds one per DecalComponent each frame
    // (skipping decals with no valid texture). The 9 texture indices map 1:1 to
    // DecalMaterialAsset::TextureSlot (asset->texBindless[], -1 = unused). The scalars
    // pack into two float4s, one 16-byte read each in the apply CS (decal_common.hlsli
    // GPUDecal).
    struct ResolvedDecal
    {
        DirectX::XMFLOAT4X4 worldToDecal;   // inverse of entity world transform
        DirectX::XMFLOAT3   boundsCenter;
        float               boundsRadius;
        DirectX::XMFLOAT3   decalForwardWS;

        // Texture bindless indices (-1 = unused).
        int32_t texBaseColor    = -1;
        int32_t texNormal       = -1;
        int32_t texOpacity      = -1;
        int32_t texRoughness    = -1;
        int32_t texSpecular     = -1;
        int32_t texAO           = -1;
        int32_t texBump         = -1;
        int32_t texCavity       = -1;
        int32_t texDisplacement = -1;

        uint32_t flags = 0;
        float    sortLayer = 0.0f;
        float    angleFadeStart = 0.3f;

        DirectX::XMFLOAT4 baseColorTint = { 1, 1, 1, 1 }; // rgb tint, a = overall opacity factor

        // scalars0: x=opacity, y=roughness, z=specular, w=ao
        DirectX::XMFLOAT4 scalars0 = { 1.f, 0.5f, 0.5f, 1.f };
        // scalars1: x=normalStrength, y=bumpStrength, z=cavityStrength, w=displacementScale
        DirectX::XMFLOAT4 scalars1 = { 1.f, 1.f, 1.f, 0.f };
    };

    DecalPass(RG::RGTextureHandle albedo,
              RG::RGTextureHandle normal,
              RG::RGTextureHandle surface,
              RG::RGTextureHandle depth);

    const char* GetName() const override { return "DecalPass"; }
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-outs.
    bool IsActive() const override;
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // Optional: dispatch the cluster cull on an external command list (the graphics CL
    // that hosts ClusterPass, so the cull overlaps GBufferPass rasterisation without a
    // compute queue or cross-queue fences); Execute() then skips its own cull and only
    // runs the apply CS. Call AFTER SetDecals() + SetCamera() for the frame. The external
    // CL must be submitted before the CL that hosts Execute() on the same queue:
    // same-queue ordering gives the cull outputs read-after-write visibility.
    void DispatchCull(RHI::CommandList cl);

    // Upload the per-frame decal list. Call before Execute() each frame.
    void SetDecals(const std::vector<ResolvedDecal>& decals);

    // Hook ClusterPass's cluster AABB SRV to reuse its per-frame AABB build. Not wired:
    // the pass does nothing (no cluster AABB -> no cull). The main view's, once; a secondary view's
    // grid is its ClusterPass grid's (SetBoundClusterAABBSRV, its segment).
    void SetClusterAABBSRV(uint64_t srv) { m_mainGrid.clusterAABBSRV = srv; }
    void SetBoundClusterAABBSRV(uint64_t srv) { m_g->clusterAABBSRV = srv; }

    // It records any view: the decal list is the scene's, the cluster grid, its decal lists and the
    // camera the bound view's (a secondary view's made when it is first bound).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // Debug heatmap toggle: replaces the albedo output with a per-cluster decal-count
    // heatmap (green -> yellow -> red); normal/surface stay real, so lighting reads the
    // real GBuffer. Driven from the editor's Debug menu.
    void SetDebugHeatmap(bool enabled) { m_debugHeatmap = enabled; }
    bool GetDebugHeatmap() const { return m_debugHeatmap; }

    // The per-cluster decal grid's SRV ({offset, count}, stride 8) when this frame's cull
    // wrote it, else 0 (render half; read by the lightComplexity debug target) - with no
    // decal nothing writes it.
    uint64_t GetDecalGridSRVHandle() const { return m_g->gridWritten ? m_g->gridSRV : 0; }

    // Camera info (same layout as ClusterPass: shared CB design). Call each frame before
    // Execute(). cameraPosWS feeds the displacement-parallax path in the apply CS.
    // The main view's (game half); SetViewCamera is a secondary view's (its segment).
    void SetCamera(const DirectX::XMFLOAT4X4& invProj,
                   const DirectX::XMFLOAT4X4& invViewProj,
                   const DirectX::XMFLOAT4X4& viewMatrix,
                   const DirectX::XMFLOAT3&   cameraPosWS,
                   float nearZ, float farZ,
                   uint32_t screenW, uint32_t screenH);
    void SetViewCamera(ViewState& view,
                       const DirectX::XMFLOAT4X4& invProj,
                       const DirectX::XMFLOAT4X4& invViewProj,
                       const DirectX::XMFLOAT4X4& viewMatrix,
                       const DirectX::XMFLOAT3&   cameraPosWS,
                       float nearZ, float farZ,
                       uint32_t screenW, uint32_t screenH);

private:
    // CB for DecalClusterCull: same binary layout as DecalCB in the cull shader.
    struct alignas(16) DecalCullCB
    {
        float    invProj[16];      // 64
        float    nearZ;            // 4
        float    farZ;             // 4
        uint32_t screenW;          // 4
        uint32_t screenH;          // 4
        uint32_t decalCount;       // 4
        uint32_t _pad0[3];         // 12
        float    viewMatrix[16];   // 64
    }; // 160

    // CB for DecalApply: mirrors DecalApplyCB in DecalApply.cs.hlsl.
    struct alignas(16) DecalApplyCB
    {
        float    invViewProj[16];  // 64
        float    invProj[16];      // 64
        float    cameraPosWS[3];   // 12: parallax needs the view direction
        float    nearZ;            // 4
        float    farZ;             // 4
        uint32_t screenW;          // 4
        uint32_t screenH;          // 4
        uint32_t decalCount;       // 4
        uint32_t debugMode;        // 4: 0 = off, 1 = cluster heatmap
        uint32_t _pad1[2];         // 8
    }; // 176

    // ---- GPU buffers ----
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    void CreatePipelines(IGraphicsDevice& gfx);

    // One view's per-cluster decal lists, the cluster AABBs they are culled against, its CB contents
    // (the camera) and this frame's cull flags.
    struct Grid
    {
        RHI::GPUBuffer indexBuf;   // RWStructuredBuffer<uint>, DEFAULT
        RHI::GPUBuffer gridBuf;    // RWStructuredBuffer<DecalGridEntry>, DEFAULT
        uint64_t indexSRV = 0, indexUAV = 0;
        uint64_t gridSRV  = 0, gridUAV  = 0;
        uint64_t clusterAABBSRV = 0;
        DecalCullCB  cullData{};
        DecalApplyCB applyData{};
        // Set by DispatchCull; consumed + cleared by Execute. Lets Renderer opt
        // into cross-CL overlap without forcing every caller to.
        bool externalCullDone = false;
        bool gridWritten      = false;   // this frame's cull ran (GetDecalGridSRVHandle)
    };
    struct ViewGrid : ViewData
    {
        Grid grid;
        void Release(IGraphicsDevice& gfx) override;
    };
    void CreateGrid(IGraphicsDevice& gfx, Grid& g);
    void SetCameraOf(Grid& g, const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj,
                     const DirectX::XMFLOAT4X4& viewMatrix, const DirectX::XMFLOAT3& cameraPosWS,
                     float nearZ, float farZ, uint32_t screenW, uint32_t screenH);

    RG::RGTextureHandle m_albedoH;
    RG::RGTextureHandle m_normalH;
    RG::RGTextureHandle m_surfaceH;
    RG::RGTextureHandle m_depthH;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;

    RHI::PipelineState m_cullPSO;
    RHI::PipelineState m_applyPSO;

    // Decal upload: a manual ring, one slot per frame in flight; structured UPLOAD heap,
    // SRV-bound.
    RHI::GPUBuffer m_decalBuffer[kFrameCount];     // StructuredBuffer<GPUDecal>, UPLOAD, mapped
    FrameCB<DecalCullCB>  m_cullCB;                // UPLOAD CB, triple-buffered
    FrameCB<DecalApplyCB> m_applyCB;               // UPLOAD CB, triple-buffered

    void*    m_decalMapped[kFrameCount]  = {};
    uint64_t m_decalsSRV[kFrameCount]    = {};

    // The main view's grid; m_g is the bound view's (BindView).
    Grid  m_mainGrid;
    Grid* m_g = &m_mainGrid;

    // Bindless texture table (cached from the device), bound for the apply CS at the
    // compute BindlessTex slot (t0 space3 -> g_AllTextures[]).
    uint64_t m_bindlessTexHandle = 0;

    uint32_t     m_decalCount = 0;
    bool         m_debugHeatmap = false;
};
