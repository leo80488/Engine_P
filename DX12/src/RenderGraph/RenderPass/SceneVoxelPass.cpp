#include "RenderGraph/RenderPass/SceneVoxelPass.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <cstring>

// COMPUTE root signature slots, aliases of RHI::Slots::Cmp (Graphics/RootSignatureLayout.h).
// kMeshDescSlot / kBindlessSlot are the COMPUTE indices (10/11); the same-named
// GRAPHICS constants in other passes are 9/14 - never map them by name, only by
// the root signature this pass binds.
//   [0]  ROOT_CBV   b0 space2 - VoxelizeCB / ClearCB
//   [4]  DESC_TABLE u0 space2 - RWTexture3D<uint> occupancy
//   [9]  DESC_TABLE t0 space0 - StructuredBuffer<GPUInstanceData>
//   [10] DESC_TABLE t1 space0 - StructuredBuffer<MeshDescriptor>
//   [11] DESC_TABLE t0 space1 - bindless g_Buffers[] (ByteAddressBuffer array)
static constexpr auto kCBSlot         = RHI::Slots::Cmp::CB;
static constexpr auto kUAVSlot        = RHI::Slots::Cmp::UAV_u0;
static constexpr auto kInstanceSlot   = RHI::Slots::Cmp::InstanceBuf;
static constexpr auto kMeshDescSlot   = RHI::Slots::Cmp::MeshDesc;
static constexpr auto kBindlessSlot   = RHI::Slots::Cmp::BindlessBuffers;

void SceneVoxelPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::SceneVoxelize_CS,  RHI::ShaderStage::CS,
                         "SceneVoxelize.cs.hlsl",  "CSMain");
    m_shaderLib.Register(ShaderID::SceneVoxelClear_CS, RHI::ShaderStage::CS,
                         "SceneVoxelClear.cs.hlsl", "CSMain");

    CreatePipelines(gfx);
    CreateOccupancy(gfx, m_mainGrid);
    m_occupancyImport = m_mainGrid.occupancyTex;

    // ---- Per-dispatch CB (ring of kMaxDraws+1 slots) ----
    m_cb.Create(gfx, "SceneVoxel.CB");

    LOG_SUCCESS("SceneVoxelPass: Tier-2 triangle voxelization initialised (%u^3 R8_UINT)",
                kGridDim);
}

// ---- Occupancy texture (128^3 R8_UINT, 2 MB) ----
void SceneVoxelPass::CreateOccupancy(IGraphicsDevice& gfx, Grid& g)
{
    {
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_3D;
        td.width      = kGridDim;
        td.height     = kGridDim;
        td.depth      = kGridDim;
        td.mipLevels = 1;
        td.format     = RHI::Format::R8_UINT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
        td.debugName = "SceneVoxel.Occupancy";
        if (!gfx.CreateTexture(td, g.occupancyTex))
            LOG_ERROR("SceneVoxelPass: occupancy texture create failed");
        g.occupancyState = RHI::ResourceState::UNORDERED_ACCESS;
    }
}

void SceneVoxelPass::ViewGrid::Release(IGraphicsDevice& gfx)
{
    if (grid.occupancyTex.IsValid()) gfx.DestroyTexture(grid.occupancyTex);
    grid = Grid{};
}

void SceneVoxelPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
        m_g = &m_mainGrid;
    else
    {
        Grid& g = view.Data<ViewGrid>(this).grid;
        if (!g.occupancyTex.IsValid() && m_gfx) CreateOccupancy(*m_gfx, g);
        m_g = &g;
    }
    m_occupancyImport = m_g->occupancyTex;
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void SceneVoxelPass::CreatePipelines(IGraphicsDevice& gfx)
{
    auto makeCs = [&](ShaderID id, RHI::PipelineState& out, const char* tag) {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("SceneVoxelPass: %s missing", tag); return; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, out))
            LOG_ERROR("SceneVoxelPass: %s PSO create failed", tag);
    };
    makeCs(ShaderID::SceneVoxelize_CS,  m_voxelizePSO, "voxelize");
    makeCs(ShaderID::SceneVoxelClear_CS, m_clearPSO,   "clear");
}

void SceneVoxelPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

uint64_t SceneVoxelPass::GetOccupancySrvHandle() const
{
    if (!m_gfx || !m_g->occupancyTex.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_g->occupancyTex);
}

// Keep in lockstep with Execute's early-outs. Renderer fills the bound view's draws (PushDraw)
// before the graph runs, so its emptiness is already final.
bool SceneVoxelPass::IsActive() const
{
    if (!m_voxelizePSO.IsValid() || !m_clearPSO.IsValid()) return false;
    if (!m_g->occupancyTex.IsValid() || !m_cb.IsValid())      return false;
    return !m_g->draws.empty();
}

RHI::CommandList SceneVoxelPass::Execute(RHI::CommandList cl)
{
    if (!m_voxelizePSO.IsValid() || !m_clearPSO.IsValid()) return cl;
    if (!m_g->occupancyTex.IsValid() || !m_cb.IsValid())      return cl;

    // Nothing to voxelize (e.g. volumetric fog off, so Renderer skipped
    // SetSceneBindings / PushDraw): skip the clear too. Nothing samples the grid
    // then - VolumetricFogPass gets a zero occupancy SRV handle and LightInject takes
    // its "no grid" branch whatever the texture holds.
    if (m_g->draws.empty()) return cl;

    IGraphicsDevice& gfx = *m_gfx;

    auto* cbMapped = m_cb.Current(gfx);
    if (!cbMapped) return cl;
    uint8_t* cbBaseRaw = cbMapped->bytes;
    const RHI::GPUBuffer& cbBuf = m_cb.CurrentBuffer(gfx);

    // Make the 3D texture UAV-writable (it arrives in SHADER_RESOURCE_COMPUTE from
    // the previous frame's LightInject read). An imported grid is the graph's to
    // transition (this pass's Phase A, VolumetricFogPass's scoped read); without the
    // import (-rgimportoff) the pass does it.
    const bool graphTracked = m_occupancyH.IsValid();
    if (!graphTracked && m_g->occupancyState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_g->occupancyTex, m_g->occupancyState,
                        RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_g->occupancyState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    const uint64_t occupancyUav = gfx.GetTextureUAVGpuHandle(m_g->occupancyTex);

    // ---- Phase 1: clear the grid ----
    {
        ClearCB cc{};
        cc.gridDim = kGridDim;
        std::memcpy(cbBaseRaw, &cc, sizeof(cc)); // slot 0 = clear

        gfx.BindComputePipelineState(m_clearPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, cbBuf, 0, cl);
        gfx.SetComputeDescriptorTable(kUAVSlot, occupancyUav, cl);

        const uint32_t groups = (kGridDim + 7) / 8;
        gfx.DispatchCompute(groups, groups, groups, cl);
    }

    // A UAV barrier between the clear and the voxelize dispatches: the clear's 0
    // writes must land before the voxelize's 255 writes, or the clear races and
    // wipes real marks.
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_g->occupancyTex), cl);

    // ---- Phase 2: per-mesh triangle voxelize dispatches ----
    if (!m_g->draws.empty() && m_instanceBufferSrv && m_meshDescriptorsSrv &&
        m_bindlessBufferTable)
    {
        gfx.BindComputePipelineState(m_voxelizePSO, cl);
        // UAV + scene SRVs / bindless table stay bound for all dispatches.
        gfx.SetComputeDescriptorTable(kUAVSlot,       occupancyUav,         cl);
        gfx.SetComputeDescriptorTable(kInstanceSlot,  m_instanceBufferSrv,  cl);
        gfx.SetComputeDescriptorTable(kMeshDescSlot,  m_meshDescriptorsSrv, cl);
        gfx.SetComputeDescriptorTable(kBindlessSlot,  m_bindlessBufferTable, cl);

        const DirectX::XMFLOAT3 extent {
            m_g->gridMax.x - m_g->gridMin.x,
            m_g->gridMax.y - m_g->gridMin.y,
            m_g->gridMax.z - m_g->gridMin.z
        };

        uint8_t* cbBase = cbBaseRaw;
        for (size_t i = 0; i < m_g->draws.size(); ++i)
        {
            const DrawEntry& d = m_g->draws[i];

            // Slot 0 is the clear CB; per-draw slots are 1..kMaxDraws.
            const uint32_t slotIdx    = static_cast<uint32_t>(i + 1);
            const uint32_t byteOffset = slotIdx * kCBSlotStride;

            VoxelizeCB c{};
            c.gridMin[0] = m_g->gridMin.x; c.gridMin[1] = m_g->gridMin.y; c.gridMin[2] = m_g->gridMin.z;
            c.gridExtent[0] = extent.x; c.gridExtent[1] = extent.y; c.gridExtent[2] = extent.z;
            c.gridDim        = kGridDim;
            c.meshDescIdx    = d.meshDescIdx;
            c.instanceOffset = d.instanceOffset;
            c.numTriangles   = d.triangleCount;
            std::memcpy(cbBase + byteOffset, &c, sizeof(c));

            gfx.SetComputeRootCBV(kCBSlot, cbBuf, byteOffset, cl);

            const uint32_t groups = (d.triangleCount + 63) / 64;
            gfx.DispatchCompute(groups, 1, 1, cl);
        }
    }

    // Hand off to LightInject (with the import: VolumetricFogPass's Phase A).
    if (!graphTracked)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(&m_g->occupancyTex,
                        RHI::ResourceState::UNORDERED_ACCESS,
                        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_g->occupancyState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }

    return cl;
}
