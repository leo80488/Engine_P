#pragma once

// CullingPass - GPU culling of indirect draw commands: InstanceCull.cs.hlsl with ONE THREAD
// PER IndirectDrawCommand; the modes share one PSO (mode flag in the CB).
// Execute (in place, on frames that do not compact, with or without retained instances): each
// thread walks its command's instance range (GPUInstanceData + per-mesh local AABB), tests the
// exact frustum + optional Hi-Z occlusion, and zeroes the command's instanceCount IN PLACE in
// the DEFAULT-heap arg buffer when every instance fails. Group argOffset / cmdCount stay valid
// (degenerate-draw convention, as BuildRenderScene's wantsMain filter): no count buffer, no
// compaction. The Renderer copies the CPU-written upload args into the DEFAULT buffer first.
// ExecuteCompact (requires retained instances): the pristine args are a read-only SRV (a GPU
// write would sticky-cull retained args across camera cuts); instances are culled one by one
// (frustum + Hi-Z + the wantsMain / viewMask predicates, via GPUInstanceData.viewMask /
// instFlags), their stable slots compacted into the upper half of the per-view working remap,
// and the surviving commands group-compacted into the working arg buffer with per-group
// counts bumped by atomics. GBufferPass passes the count buffer + per-group offset to
// ExecuteIndirect. ExecuteCompactShadow does the same per shadow view.
// Owned by Renderer (not a RenderGraph pass); the Renderer owns every barrier around the
// dispatches.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include <DirectXMath.h>

class IGraphicsDevice;

class CullingPass : public RG::PassLifecycle
{
public:
    void Init(IGraphicsDevice& gfx);
    // Hot reload (standalone: Renderer::ReloadShaders calls it).
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events from Renderer::PassSet

    // Per-frame data; set before Execute.
    void SetFrustumPlanes(const float frustumPlanes[6][4]); // 6 planes, each {nx,ny,nz,d}
    // Stored PRE-TRANSPOSED for the row-vector mul(v, viewProj) in InstanceCull.cs.hlsl: an
    // HLSL cbuffer float4x4 is column-major, so like every CB matrix (PerViewCB, LightCB, ...)
    // the CPU transposes it. Do not upload it raw: the Hi-Z test then projects with the
    // transpose, and borderline objects oscillate through the culled -> absent-from-pyramid ->
    // visible feedback loop.
    void SetViewProj(const DirectX::XMFLOAT4X4& vp)
    {
        DirectX::XMStoreFloat4x4(&m_viewProj,
            DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(&vp)));
    }

    // Dispatches the per-command culling shader.
    //   instanceBuffer - SRV, GPUInstanceData[] (this frame's slot, or the single DEFAULT-heap
    //                    buffer with retained instances)
    //   meshAABBBuffer - SRV, MeshAABB[] (per-mesh local bounds)
    //   argBuffer      - UAV, IndirectDrawCommand[] in the DEFAULT heap; must already hold this
    //                    frame's commands and be in UAV state
    //   commandCount   - commands to test (1:1 with m_drawPackets)
    //   hiZSrv         - GPU SRV of the Hi-Z pyramid, 0 = skip occlusion
    //   hiZW/hiZH/hiZMips - Hi-Z mip 0 size + mip count
    //   remapSrv       - this frame's instance remap (StructuredBuffer<uint>, draw-local ->
    //                    stable slot), read from the heap; 0 = instanceBuffer is in draw-local
    //                    order
    void Execute(RHI::CommandList cl,
                 const RHI::GPUBuffer& instanceBuffer,
                 const RHI::GPUBuffer& meshAABBBuffer,
                 const RHI::GPUBuffer& argBuffer,
                 uint32_t commandCount,
                 uint64_t hiZSrv,
                 uint32_t hiZW, uint32_t hiZH, uint32_t hiZMips,
                 uint64_t remapSrv = 0);

    // Per-instance cull + compaction (see the file comment). Every handle is REQUIRED (the
    // caller gates on their validity):
    //   workingArgBuffer - UAV u0, write-only per-view arg buffer
    //   pristineArgsSrv  - SRV t3, this frame's CPU-written args (read-only)
    //   cmdMetaSrv       - SRV t4, per-command groupBase<<16|groupIdx
    //                      (0xFFFFFFFF = not in any indirect group)
    //   pristineRemapSrv - draw-local -> stable slot, read from the heap
    //   workingRemap     - UAV u1, 2x capacity u32 (the CS writes the upper half)
    //   groupCounts      - UAV u2, per-group survivor-command counts (the caller zeroes them
    //                      before this dispatch)
    //   compactRemapBase - element offset of the working remap's upper half (== kMaxInstances)
    void ExecuteCompact(RHI::CommandList cl,
                        const RHI::GPUBuffer& instanceBuffer,
                        const RHI::GPUBuffer& meshAABBBuffer,
                        const RHI::GPUBuffer& workingArgBuffer,
                        uint32_t commandCount,
                        uint64_t hiZSrv,
                        uint32_t hiZW, uint32_t hiZH, uint32_t hiZMips,
                        uint64_t pristineRemapSrv,
                        uint64_t pristineArgsSrv,
                        uint64_t cmdMetaSrv,
                        const RHI::GPUBuffer& workingRemap,
                        const RHI::GPUBuffer& groupCounts,
                        uint32_t compactRemapBase,
                        // Optional post-cull triangle / instance / command counters (u3).
                        // Pass an INVALID buffer to skip them: the CB flag then leaves u3
                        // unbound and unwritten. Diagnostics only, nothing rendering reads
                        // them; never added to the shadow variant (up to 15 dispatches a
                        // frame).
                        const RHI::GPUBuffer* visStats = nullptr);

    // Shadow-view compaction (compactMode 2, see the shader's CompactShadowCommand banner). One
    // call per shadow view; set that view's frustum with SetFrustumPlanes first. Unlike
    // ExecuteCompact: cmdMetaSrv routes the SHADOW buckets, no Hi-Z, no main-pass predicates,
    // workingRemap is per view (base 0, no pristine lower half), and groupCounts is sliced per
    // view at element countBase.
    //   requiredViewBits     - this view's ViewBit (per-cascade author intent)
    //   excludePerObjCasters - skip kInstanceFlagPerObjShadow instances (the per-object shadow
    //                          map renders them this frame)
    //   dynamicOnly          - a dynamic-only spot cache update
    //   meshletDispatch      - emit meshlet twins as DISPATCH_MESH (CSM)
    void ExecuteCompactShadow(RHI::CommandList cl,
                              const RHI::GPUBuffer& instanceBuffer,
                              const RHI::GPUBuffer& meshAABBBuffer,
                              const RHI::GPUBuffer& workingArgBuffer,
                              uint32_t commandCount,
                              uint64_t pristineRemapSrv,
                              uint64_t pristineArgsSrv,
                              uint64_t cmdMetaSrv,
                              const RHI::GPUBuffer& workingRemap,
                              const RHI::GPUBuffer& groupCounts,
                              uint32_t countBase,
                              uint32_t requiredViewBits,
                              bool excludePerObjCasters, bool dynamicOnly = false,
                              bool meshletDispatch = false);

private:
    // Must byte-match the CullingCB block in InstanceCull.cs.hlsl.
    struct alignas(16) CullingCB
    {
        float               frustumPlanes[6][4]; // 96 B
        DirectX::XMFLOAT4X4 viewProj;            // 64 B
        uint32_t            commandCount;
        uint32_t            hiZValid;
        uint32_t            hiZWidth;
        uint32_t            hiZHeight;
        uint32_t            hiZMipCount;
        // Shadow-view fields (zero in modes 0 / 1).
        uint32_t            shadowViewFlags;     // bit0 = exclude per-object casters; bit1 = dynamic-only spot cache update;
                                                 // bit2 = meshlet twins as DISPATCH_MESH (CSM)
        uint32_t            requiredViewBits;    // this view's ViewBit
        uint32_t            countBase;           // u2 element base for this view
        uint32_t            remapEnabled;        // 1 = resolve through the remap (remapIdx)
        uint32_t            compactMode;         // 0 = in place; 1 = main compaction; 2 = shadow view
        uint32_t            compactRemapBase;    // working-remap upper-half base (mode 1)
        // Diagnostics: 1 = accumulate visible-triangle counters into u3.
        uint32_t            visStatsEnabled;
        // Heap index of the instance remap the shader reads when remapEnabled != 0.
        uint32_t            remapIdx;
        uint32_t            _padRemap[3];
    }; // 224 B
    static_assert(sizeof(CullingCB) % 16 == 0);

    bool CreatePipelines(IGraphicsDevice& gfx);

    // One CB per dispatch from the frame's constants: a later dispatch's CPU write must not
    // clobber an earlier dispatch's pending data, and the shadow views add up to 15 dispatches
    // to the main view's. Copies m_cbData into a fresh block and returns its GPU address for the
    // root CBV; 0 when the frame's constants ran out.
    uint64_t CommitCB(IGraphicsDevice& gfx);

    IGraphicsDevice*    m_gfx = nullptr;
    RHI::PipelineState  m_pso;
    ShaderLibrary       m_shaderLib;

    CullingCB              m_cbData{};

    DirectX::XMFLOAT4X4 m_viewProj{};
};
