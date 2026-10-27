#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/ClusterPass.h"   // kClusterCount
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>
#include <algorithm>

using namespace DirectX;

// GPU-side decal layout: must match GPUDecal in decal_common.hlsli.
// 192 bytes, 3 cache lines. 9 bindless indices + flags/sortLayer pack in the middle
// section; the float4 packs line up on 16-byte borders.
#pragma pack(push, 4)
struct GPUDecalUpload
{
    float    worldToDecal[16];   // 64  offset 0
    float    boundsCenter[3];    // 12  offset 64
    float    boundsRadius;       // 4   offset 76
    int32_t  texBaseColor;       // 4   offset 80
    int32_t  texNormal;          // 4   offset 84
    int32_t  texOpacity;         // 4   offset 88
    int32_t  texRoughness;       // 4   offset 92
    int32_t  texSpecular;        // 4   offset 96
    int32_t  texAO;              // 4   offset 100
    int32_t  texBump;            // 4   offset 104
    int32_t  texCavity;          // 4   offset 108
    int32_t  texDisplacement;    // 4   offset 112
    uint32_t flags;              // 4   offset 116
    float    sortLayer;          // 4   offset 120
    float    _pad0;              // 4   offset 124: aligns the next float4
    float    baseColorTint[4];   // 16  offset 128
    float    scalars0[4];        // 16  offset 144 (opacity, roughness, specular, ao)
    float    scalars1[4];        // 16  offset 160 (normalStrength, bumpStrength, cavityStrength, displacementScale)
    float    decalForwardWS[3];  // 12  offset 176
    float    angleFadeStart;     // 4   offset 188
};
#pragma pack(pop)
static_assert(sizeof(GPUDecalUpload) == 192, "GPUDecalUpload must be 192 bytes");

// Compute root-signature slots: aliases of the shared compute root signature
// (Graphics/RootSignatureLayout.h; GraphicsDX12::CreateComputeRootSignature builds it).
// Cull CS (space2): b0 its CB, t0 cluster AABBs (ClusterPass's), t1 decal buffer,
//   u0 decal index list, u1 decal grid.
// Apply CS (space2): b0 its own CB, t1 decal buffer, t2 scene depth, t3 decal grid
//   (SRV), t4 decal index list (SRV), u2/u3/u4 GBuffer0/1/2; t0 space3 bindless
//   textures.
static constexpr auto kCBSlot         = RHI::Slots::Cmp::CB;      // b0  space2
static constexpr auto kSRV0_ClusterAABB = RHI::Slots::Cmp::SRV_t0; // t0 space2
static constexpr auto kSRV1_Decals    = RHI::Slots::Cmp::SRV_t1;  // t1  space2
static constexpr auto kSRV2_SceneDepth = RHI::Slots::Cmp::SRV_t2; // t2  space2
static constexpr auto kUAV0_IndexList = RHI::Slots::Cmp::UAV_u0;  // u0  space2
static constexpr auto kUAV1_Grid      = RHI::Slots::Cmp::UAV_u1;  // u1  space2
static constexpr auto kSRV3_Grid      = RHI::Slots::Cmp::SRV_t3;  // t3  space2 (apply-side grid-as-SRV)
static constexpr auto kSRV4_IndexList = RHI::Slots::Cmp::SRV_t4;  // t4  space2 (apply-side index-list-as-SRV)
static constexpr auto kUAV2_Albedo    = RHI::Slots::Cmp::UAV_u2;  // u2  space2
static constexpr auto kUAV3_Normal    = RHI::Slots::Cmp::UAV_u3;  // u3  space2
static constexpr auto kUAV4_Surface   = RHI::Slots::Cmp::UAV_u4;  // u4  space2
static constexpr auto kBindless       = RHI::Slots::Cmp::BindlessTex; // t0 space3 (bindless texture array)

DecalPass::DecalPass(RG::RGTextureHandle albedo,
                     RG::RGTextureHandle normal,
                     RG::RGTextureHandle surface,
                     RG::RGTextureHandle depth)
    : m_albedoH(albedo), m_normalH(normal), m_surfaceH(surface), m_depthH(depth)
{
}

void DecalPass::Setup(RG::RenderGraphBuilder& b)
{
    // GBuffer textures: declared as UAV writes (RG transitions to UNORDERED_ACCESS).
    b.WriteUAV(m_albedoH);
    b.WriteUAV(m_normalH);
    b.WriteUAV(m_surfaceH);
    // Scene depth: SRV read (the graph transitions it to DEPTH_READ_SRV, which includes
    // NON_PIXEL_SHADER_RESOURCE: valid for compute).
    b.ReadSRV(m_depthH);
}

void DecalPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DecalClusterCull_CS, RHI::ShaderStage::CS,
                         "DecalClusterCull.cs.hlsl", "CSCullDecals");
    m_shaderLib.Register(ShaderID::DecalApply_CS, RHI::ShaderStage::CS,
                         "DecalApply.cs.hlsl", "CSApplyDecals");

    CreatePipelines(gfx);

    // Decal buffer (UPLOAD), triple-buffered ring.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxDecals) * sizeof(GPUDecalUpload);
        bd.stride     = sizeof(GPUDecalUpload);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_decalBuffer[i]))
            {
                m_decalMapped[i] = gfx.MapBuffer(m_decalBuffer[i]);
                m_decalsSRV[i]   = gfx.GetBufferSRVGpuHandle(m_decalBuffer[i]);
            }
        }
    }

    CreateGrid(gfx, m_mainGrid);

    // Constant buffers (UPLOAD), triple-buffered.
    if (!m_cullCB.Create(gfx, "DecalPass.CullCB"))
        LOG_ERROR("DecalPass: cull CB create failed");
    if (!m_applyCB.Create(gfx, "DecalPass.ApplyCB"))
        LOG_ERROR("DecalPass: apply CB create failed");

    // Cache the bindless texture table GPU handle (persistent: allocated once at device
    // init); bound each frame at the compute BindlessTex slot.
    m_bindlessTexHandle = gfx.GetBindlessTextureTableGpuHandle();

    LOG_SUCCESS("DecalPass: initialized (%u max decals, %u clusters, %u per-cluster cap)",
                kMaxDecals, ClusterPass::kClusterCount, kMaxPerCluster);
}

void DecalPass::CreateGrid(IGraphicsDevice& gfx, Grid& g)
{
    // Decal index list (DEFAULT), sized to cluster count x per-cluster cap.
    const uint32_t kClusterCount = ClusterPass::kClusterCount;
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * kMaxPerCluster * sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, g.indexBuf))
        {
            g.indexSRV = gfx.GetBufferSRVGpuHandle(g.indexBuf);
            g.indexUAV = gfx.GetBufferUAVGpuHandle(g.indexBuf);
        }
    }

    // Decal grid (DEFAULT)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kClusterCount) * 8;  // DecalGridEntry = 2 x uint
        bd.stride     = 8;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (gfx.CreateBuffer(bd, g.gridBuf))
        {
            g.gridSRV = gfx.GetBufferSRVGpuHandle(g.gridBuf);
            g.gridUAV = gfx.GetBufferUAVGpuHandle(g.gridBuf);
        }
    }
}

void DecalPass::ViewGrid::Release(IGraphicsDevice& gfx)
{
    if (grid.indexBuf.IsValid()) gfx.DestroyBuffer(grid.indexBuf);
    if (grid.gridBuf.IsValid())  gfx.DestroyBuffer(grid.gridBuf);
    grid = Grid{};
}

void DecalPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_g = &m_mainGrid;
        return;
    }
    Grid& g = view.Data<ViewGrid>(this).grid;
    if (!g.indexBuf.IsValid() && m_gfx) CreateGrid(*m_gfx, g);
    m_g = &g;
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void DecalPass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::DecalClusterCull_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_cullPSO))
            LOG_ERROR("DecalPass: cull PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::DecalApply_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_applyPSO))
            LOG_ERROR("DecalPass: apply PSO creation failed");
    }
}

void DecalPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void DecalPass::SetDecals(const std::vector<ResolvedDecal>& decals)
{
    m_decalCount = std::min<uint32_t>(static_cast<uint32_t>(decals.size()), kMaxDecals);
    if (!m_gfx || m_decalCount == 0) return;
    const uint32_t frameSlot = m_gfx->GetUploadSlot();
    if (frameSlot >= kFrameCount || !m_decalMapped[frameSlot]) return;

    auto* dst = static_cast<GPUDecalUpload*>(m_decalMapped[frameSlot]);
    for (uint32_t i = 0; i < m_decalCount; ++i)
    {
        const ResolvedDecal& src = decals[i];
        std::memcpy(dst[i].worldToDecal, &src.worldToDecal, 64);
        dst[i].boundsCenter[0]   = src.boundsCenter.x;
        dst[i].boundsCenter[1]   = src.boundsCenter.y;
        dst[i].boundsCenter[2]   = src.boundsCenter.z;
        dst[i].boundsRadius      = src.boundsRadius;
        dst[i].texBaseColor      = src.texBaseColor;
        dst[i].texNormal         = src.texNormal;
        dst[i].texOpacity        = src.texOpacity;
        dst[i].texRoughness      = src.texRoughness;
        dst[i].texSpecular       = src.texSpecular;
        dst[i].texAO             = src.texAO;
        dst[i].texBump           = src.texBump;
        dst[i].texCavity         = src.texCavity;
        dst[i].texDisplacement   = src.texDisplacement;
        dst[i].flags             = src.flags;
        dst[i].sortLayer         = src.sortLayer;
        dst[i]._pad0             = 0.f;
        dst[i].baseColorTint[0]  = src.baseColorTint.x;
        dst[i].baseColorTint[1]  = src.baseColorTint.y;
        dst[i].baseColorTint[2]  = src.baseColorTint.z;
        dst[i].baseColorTint[3]  = src.baseColorTint.w;
        dst[i].scalars0[0]       = src.scalars0.x;
        dst[i].scalars0[1]       = src.scalars0.y;
        dst[i].scalars0[2]       = src.scalars0.z;
        dst[i].scalars0[3]       = src.scalars0.w;
        dst[i].scalars1[0]       = src.scalars1.x;
        dst[i].scalars1[1]       = src.scalars1.y;
        dst[i].scalars1[2]       = src.scalars1.z;
        dst[i].scalars1[3]       = src.scalars1.w;
        dst[i].decalForwardWS[0] = src.decalForwardWS.x;
        dst[i].decalForwardWS[1] = src.decalForwardWS.y;
        dst[i].decalForwardWS[2] = src.decalForwardWS.z;
        dst[i].angleFadeStart    = src.angleFadeStart;
    }
}

void DecalPass::SetCamera(const DirectX::XMFLOAT4X4& invProj,
                          const DirectX::XMFLOAT4X4& invViewProj,
                          const DirectX::XMFLOAT4X4& viewMatrix,
                          const DirectX::XMFLOAT3&   cameraPosWS,
                          float nearZ, float farZ,
                          uint32_t screenW, uint32_t screenH)
{
    SetCameraOf(m_mainGrid, invProj, invViewProj, viewMatrix, cameraPosWS, nearZ, farZ, screenW, screenH);
}

void DecalPass::SetViewCamera(ViewState& view,
                              const DirectX::XMFLOAT4X4& invProj,
                              const DirectX::XMFLOAT4X4& invViewProj,
                              const DirectX::XMFLOAT4X4& viewMatrix,
                              const DirectX::XMFLOAT3&   cameraPosWS,
                              float nearZ, float farZ,
                              uint32_t screenW, uint32_t screenH)
{
    if (view.Slot() == 0) return;
    SetCameraOf(view.Data<ViewGrid>(this).grid, invProj, invViewProj, viewMatrix, cameraPosWS, nearZ, farZ,
                screenW, screenH);
}

void DecalPass::SetCameraOf(Grid& g, const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& invViewProj,
                            const DirectX::XMFLOAT4X4& viewMatrix, const DirectX::XMFLOAT3& cameraPosWS,
                            float nearZ, float farZ, uint32_t screenW, uint32_t screenH)
{
    DecalCullCB&  cull  = g.cullData;
    DecalApplyCB& apply = g.applyData;
    std::memcpy(cull.invProj,    &invProj,    64);
    std::memcpy(cull.viewMatrix, &viewMatrix, 64);
    cull.nearZ      = nearZ;
    cull.farZ       = farZ;
    cull.screenW    = screenW;
    cull.screenH    = screenH;
    cull.decalCount = m_decalCount;

    std::memcpy(apply.invViewProj, &invViewProj, 64);
    std::memcpy(apply.invProj,     &invProj,     64);
    apply.cameraPosWS[0] = cameraPosWS.x;
    apply.cameraPosWS[1] = cameraPosWS.y;
    apply.cameraPosWS[2] = cameraPosWS.z;
    apply.nearZ      = nearZ;
    apply.farZ       = farZ;
    apply.screenW    = screenW;
    apply.screenH    = screenH;
    apply.decalCount = m_decalCount;
}

// Keep in lockstep with Execute's early-outs. Decal count, heatmap flag and the
// cluster-AABB SRV are all pushed by the Renderer before the graph runs.
bool DecalPass::IsActive() const
{
    if (!m_gfx || !m_cullPSO.IsValid() || !m_applyPSO.IsValid() || m_g->clusterAABBSRV == 0)
        return false;
    return m_decalCount != 0 || m_debugHeatmap;
}

RHI::CommandList DecalPass::Execute(RHI::CommandList cl)
{
    Grid& g = *m_g;
    if (!m_gfx || !m_cullPSO.IsValid() || !m_applyPSO.IsValid() || g.clusterAABBSRV == 0)
        return cl;

    // Skip the pass when there is nothing to do and the debug heatmap is off: a scene
    // without DecalComponents dispatches nothing. The heatmap still runs apply, so empty
    // clusters draw green.
    if (m_decalCount == 0 && !m_debugHeatmap)
        return cl;

    auto& gfx  = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();

    // Refresh CB data written to the per-frame UPLOAD slots.
    g.cullData.decalCount  = m_decalCount;
    g.applyData.decalCount = m_decalCount;
    g.applyData.debugMode  = m_debugHeatmap ? 1u : 0u;
    if (auto* slot = m_cullCB.Current(gfx))  *slot = g.cullData;
    if (auto* slot = m_applyCB.Current(gfx)) *slot = g.applyData;

    const uint64_t decalsSRV = m_decalsSRV[frameSlot];

    // ---- Pass 1: cluster cull ----
    // Skipped when there are no decals (the heatmap still runs apply) or DispatchCull()
    // already ran on an external CL this frame (the grid's externalCullDone: the Renderer runs the
    // cull on the ClusterPass CL so it overlaps GBufferPass on the GPU).
    const uint32_t kClusterCount = ClusterPass::kClusterCount;
    if (m_decalCount > 0 && !g.externalCullDone)
    {
        gfx.BindComputePipelineState(m_cullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, m_cullCB.CurrentBuffer(gfx), 0, cl);
        gfx.SetComputeDescriptorTable(kSRV0_ClusterAABB, g.clusterAABBSRV, cl);
        gfx.SetComputeDescriptorTable(kSRV1_Decals,      decalsSRV,        cl);
        gfx.SetComputeDescriptorTable(kUAV0_IndexList,   g.indexUAV,       cl);
        gfx.SetComputeDescriptorTable(kUAV1_Grid,        g.gridUAV,        cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.indexBuf), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.gridBuf),  cl);
        g.gridWritten = true;
    }
    // Clear the "done elsewhere" flag: next frame the Renderer can opt in again by
    // calling DispatchCull() before Execute.
    g.externalCullDone = false;

    // ---- Pass 2: apply to GBuffer ----
    gfx.BindComputePipelineState(m_applyPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_applyCB.CurrentBuffer(gfx), 0, cl);

    gfx.SetComputeDescriptorTable(kSRV1_Decals,     decalsSRV,       cl);

    // Depth SRV: the current physical texture's SRV handle.
    const auto& ctx = cl.GetContext();
    const RHI::Texture* depthTex = ctx.GetTexture({ m_depthH.id });
    if (!depthTex) return cl;
    uint64_t depthSRV = gfx.GetTextureSRVGpuHandle(*depthTex);
    gfx.SetComputeDescriptorTable(kSRV2_SceneDepth, depthSRV, cl);

    gfx.SetComputeDescriptorTable(kSRV3_Grid,       g.gridSRV,  cl);
    gfx.SetComputeDescriptorTable(kSRV4_IndexList,  g.indexSRV, cl);

    // GBuffer UAVs.
    const RHI::Texture* albedoTex  = ctx.GetTexture({ m_albedoH.id  });
    const RHI::Texture* normalTex  = ctx.GetTexture({ m_normalH.id  });
    const RHI::Texture* surfaceTex = ctx.GetTexture({ m_surfaceH.id });
    if (!albedoTex || !normalTex || !surfaceTex) return cl;
    gfx.SetComputeDescriptorTable(kUAV2_Albedo,  gfx.GetTextureUAVGpuHandle(*albedoTex),  cl);
    gfx.SetComputeDescriptorTable(kUAV3_Normal,  gfx.GetTextureUAVGpuHandle(*normalTex),  cl);
    gfx.SetComputeDescriptorTable(kUAV4_Surface, gfx.GetTextureUAVGpuHandle(*surfaceTex), cl);

    // Bindless textures: the same heap region graphics uses, bound on compute.
    if (m_bindlessTexHandle != 0)
        gfx.SetComputeDescriptorTable(kBindless, m_bindlessTexHandle, cl);

    const uint32_t groupsX = (g.applyData.screenW + 7) / 8;
    const uint32_t groupsY = (g.applyData.screenH + 7) / 8;
    gfx.DispatchCompute(groupsX, groupsY, 1, cl);

    // UAV barriers on the three GBuffer textures so LightingPass sees the writes. The
    // graph emits the UAV->SRV transition itself; these MEMORY barriers only serialise
    // pending UAV writes within this command list.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(albedoTex),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(normalTex),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(surfaceTex), cl);

    return cl;
}

// Records the cluster cull on an external CL (the Renderer's cluster CL, Phase 2a.5).
// Execute then sees the bound grid's externalCullDone, skips its own cull and runs only apply; the
// graph's CL follows this one in the Renderer's CL chain, so the index list / grid
// writes are visible to the apply CS.
void DecalPass::DispatchCull(RHI::CommandList cl)
{
    // Reset for this frame's grid: DispatchCull runs every frame (Phase 2a.5), before
    // Execute's own cull could set it.
    Grid& g = *m_g;
    g.gridWritten = false;
    if (!m_gfx || !m_cullPSO.IsValid() || g.clusterAABBSRV == 0 || m_decalCount == 0)
        return;

    auto& gfx = *m_gfx;

    // Refresh the cull-side CB: the camera was set in SetCamera(), decalCount is live.
    g.cullData.decalCount = m_decalCount;
    if (auto* slot = m_cullCB.Current(gfx)) *slot = g.cullData;

    gfx.BindComputePipelineState(m_cullPSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cullCB.CurrentBuffer(gfx), 0, cl);
    gfx.SetComputeDescriptorTable(kSRV0_ClusterAABB, g.clusterAABBSRV, cl);
    gfx.SetComputeDescriptorTable(kSRV1_Decals,      m_decalsSRV[gfx.GetFrameIndex()], cl);
    gfx.SetComputeDescriptorTable(kUAV0_IndexList,   g.indexUAV,  cl);
    gfx.SetComputeDescriptorTable(kUAV1_Grid,        g.gridUAV,   cl);

    gfx.DispatchCompute((ClusterPass::kClusterCount + 63) / 64, 1, 1, cl);

    // UAV barriers so the end of this CL flushes writes to memory before
    // the next CL that reads the buffers as SRV. Same-queue CL ordering +
    // these barriers give RAW safety without needing a typed transition.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.indexBuf), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.gridBuf),  cl);

    g.externalCullDone = true;
    g.gridWritten = true;
}
