#include "RenderGraph/RenderPass/BeamSimPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/BeamSystem.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

// Aliases of the shared compute root signature (Graphics/RootSignatureLayout.h), same convention
// as ParticleSimPass / SkinningPass: b0 = BeamGenParams CBV, t0 = control points SRV, u0..u3 =
// pos / normal / tangent / uv UAVs (all space2).
static constexpr auto kCSCBSlot         = RHI::Slots::Cmp::CB;
static constexpr auto kCSCPSlot         = RHI::Slots::Cmp::SRV_t0;
static constexpr auto kCSPosUavSlot     = RHI::Slots::Cmp::UAV_u0;
static constexpr auto kCSNormalUavSlot  = RHI::Slots::Cmp::UAV_u1;
static constexpr auto kCSTangentUavSlot = RHI::Slots::Cmp::UAV_u2;
static constexpr auto kCSUvUavSlot      = RHI::Slots::Cmp::UAV_u3;

void BeamSimPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::BeamTubeGen_CS, RHI::ShaderStage::CS,
                         "BeamTubeGen.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    LOG_SUCCESS("BeamSimPass: initialised");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool BeamSimPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::BeamTubeGen_CS);
    if (!cs)
    {
        LOG_ERROR("BeamSimPass: BeamTubeGen_CS shader lookup failed");
        return false;
    }

    RHI::PipelineStateDesc d{};
    d.cs = cs;
    if (!gfx.CreatePipelineState(d, m_genPSO))
        LOG_ERROR("BeamSimPass: gen PSO creation failed");
    return true;
}

void BeamSimPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void BeamSimPass::Execute(RHI::CommandList cl)
{
    if (!m_sys || !m_genPSO.IsValid()) return;
    const auto& active = m_sys->GetActiveBeams();
    if (active.empty()) return;

    auto& gfx = *m_gfx;

    gfx.BindComputePipelineState(m_genPSO, cl);

    // SRV + 4 UAVs are constant across all beams; bind once.
    gfx.SetComputeDescriptorTable(kCSCPSlot,         m_sys->GetControlPointsSrv(), cl);
    gfx.SetComputeDescriptorTable(kCSPosUavSlot,     m_sys->GetPosUav(),     cl);
    gfx.SetComputeDescriptorTable(kCSNormalUavSlot,  m_sys->GetNormalUav(),  cl);
    gfx.SetComputeDescriptorTable(kCSTangentUavSlot, m_sys->GetTangentUav(), cl);
    gfx.SetComputeDescriptorTable(kCSUvUavSlot,      m_sys->GetUvUav(),      cl);

    // Per-beam CBV swap + dispatch; thread groups = ceil(kAxialRingCount / 8).
    constexpr uint32_t kGroupCount =
        (BeamSystem::kAxialRingCount + 7) / 8;

    for (const auto& ab : active)
    {
        gfx.SetComputeRootCBV(kCSCBSlot, m_sys->GetParamsBuffer(), ab.cbOffset, cl);
        gfx.DispatchCompute(kGroupCount, 1, 1, cl);
    }

    // UAV -> SRV barrier so GBuffer / Transparent see the fresh vertex data through the bindless SRV
    // view of the same buffers (bound as UAV and SRV in different heaps; D3D12 needs the barrier).
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetPosBuffer()),     cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetNormalBuffer()),  cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetTangentBuffer()), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_sys->GetUvBuffer()),      cl);
}
