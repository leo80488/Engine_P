#pragma once

// SceneVoxelPass - triangle-precision world-space occupancy grid for the
// volumetric fog LightInject shader. Each frame a compute clear zeroes the 3D
// R8_UINT texture, then one dispatch per opaque draw (the list Renderer sets)
// runs a thread per triangle: it fetches the vertices via PVF, transforms them
// to world and marks every voxel the triangle intersects (Akenine-Moller
// triangle-box SAT).
//
// Occupied cells follow real geometry: openings stay empty, thin occluders mark
// only where their triangles lie, concave interiors stay unmarked. LightInject
// raymarches voxel -> light and treats any marked cell as full occlusion.
// Conservative rasterization could replace the SAT loop for ~2-3x speed with an
// identical result.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTextureNames.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's grid)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>
#include <cstdint>
#include <vector>

class SceneVoxelPass : public RG::RenderPass
{
public:
    static constexpr uint32_t kGridDim  = 128;
    // Max mesh dispatches per frame. Each takes one 256-byte slot of the
    // per-dispatch CB; one CBV covers at most 64 KB = 256 slots and slot 0 is the
    // clear's. More opaque meshes need a size cull (skip tiny ones) or a second CB
    // ring.
    static constexpr uint32_t kMaxDraws = 255;

    // ---- Per-frame draw list (the bound view's) ----
    struct DrawEntry
    {
        uint32_t meshDescIdx;
        uint32_t instanceOffset;
        uint32_t triangleCount;
    };

    const char* GetName() const override { return "SceneVoxelPass"; }
    // The occupancy volume is this pass's import (RG::PassTex::kSceneVoxelOccupancy;
    // the desc cannot say 3D, so the snapshot shows one 128^2 slice), created in
    // Init. The graph transitions it: to UNORDERED_ACCESS in this pass's Phase A, to
    // SHADER_RESOURCE_COMPUTE in VolumetricFogPass's, whose light injection (a
    // compute dispatch) reads it scoped at that state. The UAV barrier between the
    // clear and the voxelize dispatches stays in Execute. The import's source is a copy of
    // the bound view's grid (BindView), which the graph takes at each Execute's start.
    void DeclareImports(RG::ImportDeclarer& d) override
    {
        m_occupancyH = d.Import(RG::PassTex::kSceneVoxelOccupancy,
                                { RHI::Format::R8_UINT, kGridDim, kGridDim, false, true },
                                { &m_occupancyImport, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
    }
    void Setup (RG::RenderGraphBuilder& b) override { b.WriteUAV(m_occupancyH); }
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's early-outs (missing PSO / texture / CB, or no draws).
    bool IsActive() const override;
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // It records any view: each voxelizes around its own camera from its own list into its own
    // grid (a secondary view's made when it is first bound); the setters below write the bound
    // view's (the render half, BindViewInputs).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // World-space AABB of the occupancy grid. Renderer snaps it to voxel-sized
    // units centered on the camera each frame.
    void SetGridBounds(const DirectX::XMFLOAT3& mn, const DirectX::XMFLOAT3& mx)
    {
        m_g->gridMin = mn;
        m_g->gridMax = mx;
    }

    // Resource handles the voxelize shader needs. Renderer sets these once
    // per frame after BuildRenderScene so pointers are stable.
    void SetSceneBindings(uint64_t instanceBufferSrv,
                          uint64_t meshDescriptorsSrv,
                          uint64_t bindlessBufferTable)
    {
        m_instanceBufferSrv    = instanceBufferSrv;
        m_meshDescriptorsSrv   = meshDescriptorsSrv;
        m_bindlessBufferTable  = bindlessBufferTable;
    }

    // ResetDraws() once per frame, then PushDraw() for every opaque mesh whose
    // bounds overlap the grid; Execute() consumes the list. PushDraw drops empty
    // meshes and anything past kMaxDraws.
    void ResetDraws() { m_g->draws.clear(); }
    void PushDraw(uint32_t meshDescIdx, uint32_t instanceOffset, uint32_t triangleCount)
    {
        if (triangleCount == 0) return;
        if (m_g->draws.size() >= kMaxDraws) return;
        m_g->draws.push_back({ meshDescIdx, instanceOffset, triangleCount });
    }
    uint32_t GetDrawCount() const { return static_cast<uint32_t>(m_g->draws.size()); }

    // ---- Consumed by VolumetricFogPass ----
    uint64_t                  GetOccupancySrvHandle() const;
    const DirectX::XMFLOAT3&  GetGridMin() const { return m_g->gridMin; }
    const DirectX::XMFLOAT3&  GetGridMax() const { return m_g->gridMax; }
    static constexpr uint32_t GetGridDim()       { return kGridDim; }

private:
    // Per-dispatch CB of the voxelize shader. Each gets its own kCBSlotStride slot
    // in one UPLOAD buffer, so the CBV root offset advances between dispatches
    // without aliasing.
    struct alignas(16) VoxelizeCB
    {
        float    gridMin[3];     float _pad0;
        float    gridExtent[3];  uint32_t gridDim;
        uint32_t meshDescIdx;
        uint32_t instanceOffset;
        uint32_t numTriangles;
        uint32_t _pad1;
    };

    // CB of the clear shader: slot 0 of the same buffer.
    struct alignas(16) ClearCB
    {
        uint32_t gridDim;
        uint32_t _pad[3];
    };

    static constexpr uint32_t kCBSlotStride = RHI::kConstantBufferAlignment; // D3D12 CBV alignment (256 bytes)
    static_assert(sizeof(VoxelizeCB) <= kCBSlotStride, "VoxelizeCB overflows CB slot");
    static_assert(sizeof(ClearCB)    <= kCBSlotStride, "ClearCB overflows CB slot");

    struct alignas(256) VoxelCBPool { uint8_t bytes[kCBSlotStride * (kMaxDraws + 1)]; };

    // One view's occupancy grid (Texture3D<uint> R8_UINT), its bounds and this frame's draws.
    struct Grid
    {
        RHI::Texture       occupancyTex;
        // The pass's own state tracking, used only without the import (-rgimportoff).
        RHI::ResourceState occupancyState = RHI::ResourceState::UNORDERED_ACCESS;
        DirectX::XMFLOAT3  gridMin { -32.f, -32.f, -32.f };
        DirectX::XMFLOAT3  gridMax {  32.f,  32.f,  32.f };
        std::vector<DrawEntry> draws;
    };
    struct ViewGrid : ViewData
    {
        Grid grid;
        void Release(IGraphicsDevice& gfx) override;
    };

    void CreatePipelines(IGraphicsDevice& gfx);
    void CreateOccupancy(IGraphicsDevice& gfx, Grid& g);

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_voxelizePSO;
    RHI::PipelineState m_clearPSO;

    // The main view's grid; m_g is the bound view's (BindView), m_occupancyImport a copy of its
    // texture for the graph's import.
    Grid                m_mainGrid;
    Grid*               m_g = &m_mainGrid;
    RHI::Texture        m_occupancyImport;
    RG::RGTextureHandle m_occupancyH;   // its import

    // Per-frame CB ring: slot 0 = clear, slots 1..kMaxDraws = voxelize dispatches.
    FrameCB<VoxelCBPool> m_cb;

    // Resource bindings: SRVs / bindless table Renderer stamps in each frame.
    uint64_t m_instanceBufferSrv   = 0;
    uint64_t m_meshDescriptorsSrv  = 0;
    uint64_t m_bindlessBufferTable = 0;
};
