#include "RenderGraph/RenderPass/CullingPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Check.h"
#include "System/Log.h"
#include <cstring>

// Aliases of the shared compute root signature (Graphics/RootSignatureLayout.h): b0
// CullingCB, t0 GPUInstanceData[], t1 MeshAABB[], t2 Hi-Z pyramid (bound only when valid),
// u0 IndirectDrawCommand[]. Culling and skinning share it but never run on the same command
// list at once, so the slot reuse is safe.
static constexpr auto kCBSlot       = RHI::Slots::Cmp::CB;      // b0 space2
static constexpr auto kInstanceSRV  = RHI::Slots::Cmp::SRV_t0;  // t0 space2
static constexpr auto kMeshAABBSRV  = RHI::Slots::Cmp::SRV_t1;  // t1 space2
static constexpr auto kHiZSRV       = RHI::Slots::Cmp::SRV_t2;  // t2 space2
static constexpr auto kCommandsUAV  = RHI::Slots::Cmp::UAV_u0;  // u0 space2
// Compaction slots: the frozen shared root signature gives them to skinning / morph (t3 /
// t4, u1) and the XeGTAO prefilter (u2), none of which shares a recording with culling.
// The instance remap is read from the heap (remapIdx), not bound.
static constexpr auto kPristineSRV     = RHI::Slots::Cmp::SRV_t3;  // t3 space2
static constexpr auto kCmdMetaSRV      = RHI::Slots::Cmp::SRV_t4;  // t4 space2
static constexpr auto kWorkingRemapUAV = RHI::Slots::Cmp::UAV_u1;  // u1 space2
static constexpr auto kGroupCountsUAV  = RHI::Slots::Cmp::UAV_u2;  // u2 space2
// Diagnostics counters (ExecuteCompact only). u3 is already in the shared compute root
// signature and on the startup binding validation's allow-list.
static constexpr auto kVisStatsUAV     = RHI::Slots::Cmp::UAV_u3;  // u3 space2

void CullingPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::InstanceCull_CS, RHI::ShaderStage::CS,
                         "InstanceCull.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("CullingPass: initialized (per-command frustum + Hi-Z cull)");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool CullingPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::InstanceCull_CS);
    if (!cs) { LOG_ERROR("CullingPass: InstanceCull_CS not found"); return false; }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_pso))
    { LOG_ERROR("CullingPass: PSO creation failed"); return false; }
    return true;
}

void CullingPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void CullingPass::SetFrustumPlanes(const float frustumPlanes[6][4])
{
    std::memcpy(m_cbData.frustumPlanes, frustumPlanes, sizeof(m_cbData.frustumPlanes));
}

// Every dispatch owns its block of the frame's constants - up to 15 shadow views (3 CSM
// cascades + 8 spot slices + 4 point cubes) and the main view a frame - so no view can cull
// against another's frustum. 0 when the constants ran out (reported by the allocator; the
// caller skips the dispatch).
uint64_t CullingPass::CommitCB(IGraphicsDevice& gfx)
{
    const RHI::Constants<CullingCB> c = AllocConstants<CullingCB>(gfx);
    if (!c) return 0;
    *c.cpu = m_cbData;
    return c.gpu;
}

void CullingPass::Execute(RHI::CommandList cl,
                          const RHI::GPUBuffer& instanceBuffer,
                          const RHI::GPUBuffer& meshAABBBuffer,
                          const RHI::GPUBuffer& argBuffer,
                          uint32_t commandCount,
                          uint64_t hiZSrv,
                          uint32_t hiZW, uint32_t hiZH, uint32_t hiZMips,
                          uint64_t remapSrv)
{
    if (!m_pso.IsValid() || commandCount == 0) return;

    IGraphicsDevice& gfx = *m_gfx;

    m_cbData.viewProj         = m_viewProj;
    m_cbData.commandCount     = commandCount;
    m_cbData.hiZValid         = (hiZSrv != 0 && hiZMips > 0) ? 1u : 0u;
    m_cbData.hiZWidth         = hiZW;
    m_cbData.hiZHeight        = hiZH;
    m_cbData.hiZMipCount      = hiZMips;
    // Zero the shadow-view fields: m_cbData persists across dispatches and a shadow-view
    // dispatch may have been recorded earlier this frame.
    m_cbData.shadowViewFlags  = 0;
    m_cbData.requiredViewBits = 0;
    m_cbData.countBase        = 0;
    m_cbData.remapEnabled     = (remapSrv != 0) ? 1u : 0u;
    // The remap is opened from the heap, recomputed from the current handle every dispatch: a
    // typed handle (StructuredBuffer<uint>, stride 4); no remap -> the structured null, and a
    // refused one stops the cull.
    m_cbData.remapIdx         = gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
                                    remapSrv, "InstanceCull remap", 4u).Bits();
    if (m_cbData.remapEnabled && m_cbData.remapIdx == BINDLESS_NULL_STRUCTURED)
    {
        ENGINE_CHECK_ONCE(false, "InstanceCull: the remap SRV has no heap index - culling skipped");
        return;
    }
    m_cbData.compactMode      = 0;   // in-place zero-out (the frame does not compact)
    m_cbData.compactRemapBase = 0;
    // m_cbData persists across dispatches and this path does not bind u3: a 1 inherited from
    // ExecuteCompact would make the shader write an UNBOUND UAV, silently under Resource Binding
    // Tier 3.
    m_cbData.visStatsEnabled  = 0;
    const uint64_t cbVA       = CommitCB(gfx);
    if (!cbVA) return;

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, cbVA, cl);
    gfx.SetComputeDescriptorTable(kInstanceSRV, gfx.GetBufferSRVGpuHandle(instanceBuffer), cl);
    gfx.SetComputeDescriptorTable(kMeshAABBSRV, gfx.GetBufferSRVGpuHandle(meshAABBBuffer), cl);
    // The shader's Hi-Z reads sit behind a uniform hiZValid branch; leaving the
    // table slot unset while the branch is never taken is legal, so bind only
    // when the pyramid exists.
    if (m_cbData.hiZValid)
        gfx.SetComputeDescriptorTable(kHiZSRV, hiZSrv, cl);
    gfx.SetComputeDescriptorTable(kCommandsUAV, gfx.GetBufferUAVGpuHandle(argBuffer), cl);

    const uint32_t groups = (commandCount + 63u) / 64u;
    gfx.DispatchCompute(groups, 1, 1, cl);
}

void CullingPass::ExecuteCompact(RHI::CommandList cl,
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
                                 const RHI::GPUBuffer* visStats)
{
    if (!m_pso.IsValid() || commandCount == 0) return;
    // Compact mode has no partial fallback: the Renderer latched the mode a frame phase earlier
    // (arg counts + per-view CB already committed to it), so a missing resource must be loud.
    if (pristineRemapSrv == 0 || pristineArgsSrv == 0 || cmdMetaSrv == 0)
    {
        LOG_ERROR("CullingPass::ExecuteCompact: missing SRV (remap=%llu args=%llu meta=%llu)",
                  (unsigned long long)pristineRemapSrv,
                  (unsigned long long)pristineArgsSrv,
                  (unsigned long long)cmdMetaSrv);
        return;
    }

    IGraphicsDevice& gfx = *m_gfx;

    m_cbData.viewProj         = m_viewProj;
    m_cbData.commandCount     = commandCount;
    m_cbData.hiZValid         = (hiZSrv != 0 && hiZMips > 0) ? 1u : 0u;
    m_cbData.hiZWidth         = hiZW;
    m_cbData.hiZHeight        = hiZH;
    m_cbData.hiZMipCount      = hiZMips;
    // Zero the shadow-view fields (see Execute).
    m_cbData.shadowViewFlags  = 0;
    m_cbData.requiredViewBits = 0;
    m_cbData.countBase        = 0;
    m_cbData.remapEnabled     = 1;   // compaction REQUIRES the instance remap
    m_cbData.remapIdx         = gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
                                    pristineRemapSrv, "InstanceCull pristine remap", 4u).Bits();
    if (m_cbData.remapIdx == BINDLESS_NULL_STRUCTURED)
    {
        ENGINE_CHECK_ONCE(false, "InstanceCull: the pristine remap SRV has no heap index - culling skipped");
        return;
    }
    m_cbData.compactMode      = 1;
    m_cbData.compactRemapBase = compactRemapBase;
    // The flag and the bind come from one expression, so the shader never writes an unbound
    // UAV (silent under Resource Binding Tier 3).
    const bool wantVisStats   = (visStats != nullptr && visStats->IsValid());
    m_cbData.visStatsEnabled  = wantVisStats ? 1u : 0u;
    const uint64_t cbVA       = CommitCB(gfx);
    if (!cbVA) return;

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, cbVA, cl);
    gfx.SetComputeDescriptorTable(kInstanceSRV, gfx.GetBufferSRVGpuHandle(instanceBuffer), cl);
    gfx.SetComputeDescriptorTable(kMeshAABBSRV, gfx.GetBufferSRVGpuHandle(meshAABBBuffer), cl);
    if (m_cbData.hiZValid)
        gfx.SetComputeDescriptorTable(kHiZSRV, hiZSrv, cl);
    gfx.SetComputeDescriptorTable(kCommandsUAV,     gfx.GetBufferUAVGpuHandle(workingArgBuffer), cl);
    gfx.SetComputeDescriptorTable(kPristineSRV,     pristineArgsSrv, cl);
    gfx.SetComputeDescriptorTable(kCmdMetaSRV,      cmdMetaSrv, cl);
    gfx.SetComputeDescriptorTable(kWorkingRemapUAV, gfx.GetBufferUAVGpuHandle(workingRemap), cl);
    gfx.SetComputeDescriptorTable(kGroupCountsUAV,  gfx.GetBufferUAVGpuHandle(groupCounts), cl);
    if (wantVisStats)
        gfx.SetComputeDescriptorTable(kVisStatsUAV, gfx.GetBufferUAVGpuHandle(*visStats), cl);

    const uint32_t groups = (commandCount + 63u) / 64u;
    gfx.DispatchCompute(groups, 1, 1, cl);
}

void CullingPass::ExecuteCompactShadow(RHI::CommandList cl,
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
                                       bool excludePerObjCasters, bool dynamicOnly,
                                       bool meshletDispatch)
{
    if (!m_pso.IsValid() || commandCount == 0) return;
    // Same all-or-nothing contract as ExecuteCompact: the Renderer latched
    // retained shadow args a frame-phase earlier, so a missing SRV is a bug.
    if (pristineRemapSrv == 0 || pristineArgsSrv == 0 || cmdMetaSrv == 0)
    {
        LOG_ERROR("CullingPass::ExecuteCompactShadow: missing SRV "
                  "(remap=%llu args=%llu meta=%llu)",
                  (unsigned long long)pristineRemapSrv,
                  (unsigned long long)pristineArgsSrv,
                  (unsigned long long)cmdMetaSrv);
        return;
    }

    IGraphicsDevice& gfx = *m_gfx;

    m_cbData.viewProj         = m_viewProj;     // unused (hiZValid = 0)
    m_cbData.commandCount     = commandCount;
    m_cbData.hiZValid         = 0;              // no shadow depth pyramid exists
    m_cbData.hiZWidth         = 0;
    m_cbData.hiZHeight        = 0;
    m_cbData.hiZMipCount      = 0;
    m_cbData.shadowViewFlags  = (excludePerObjCasters ? 1u : 0u) | (dynamicOnly ? 2u : 0u) |
                                (meshletDispatch ? 4u : 0u);
    m_cbData.requiredViewBits = requiredViewBits;
    m_cbData.countBase        = countBase;
    m_cbData.remapEnabled     = 1;   // compaction REQUIRES the instance remap
    m_cbData.remapIdx         = gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
                                    pristineRemapSrv, "InstanceCull pristine remap", 4u).Bits();
    if (m_cbData.remapIdx == BINDLESS_NULL_STRUCTURED)
    {
        ENGINE_CHECK_ONCE(false, "InstanceCull: the pristine remap SRV has no heap index - culling skipped");
        return;
    }
    m_cbData.compactMode      = 2;   // shadow-view variant
    m_cbData.compactRemapBase = 0;   // per-view remap: no pristine lower half
    // NEVER counts. This runs once per CSM cascade / spot slice / point cube - up to 15
    // dispatches a frame - so counting here would inflate the visible-triangle figure about
    // tenfold and make it swing with the sun angle. It does not bind u3 either, so a stale 1
    // would be an unbound-UAV write.
    m_cbData.visStatsEnabled  = 0;
    const uint64_t cbVA       = CommitCB(gfx);
    if (!cbVA) return;

    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBVAddress(kCBSlot, cbVA, cl);
    gfx.SetComputeDescriptorTable(kInstanceSRV, gfx.GetBufferSRVGpuHandle(instanceBuffer), cl);
    gfx.SetComputeDescriptorTable(kMeshAABBSRV, gfx.GetBufferSRVGpuHandle(meshAABBBuffer), cl);
    // Hi-Z slot left unbound: the uniform hiZValid branch never reads it.
    gfx.SetComputeDescriptorTable(kCommandsUAV,     gfx.GetBufferUAVGpuHandle(workingArgBuffer), cl);
    gfx.SetComputeDescriptorTable(kPristineSRV,     pristineArgsSrv, cl);
    gfx.SetComputeDescriptorTable(kCmdMetaSRV,      cmdMetaSrv, cl);
    gfx.SetComputeDescriptorTable(kWorkingRemapUAV, gfx.GetBufferUAVGpuHandle(workingRemap), cl);
    gfx.SetComputeDescriptorTable(kGroupCountsUAV,  gfx.GetBufferUAVGpuHandle(groupCounts), cl);

    const uint32_t groups = (commandCount + 63u) / 64u;
    gfx.DispatchCompute(groups, 1, 1, cl);
}
