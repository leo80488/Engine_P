#include "RenderGraph/RenderPass/NPRStabilizePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"          // CreateTexturePlaced / alloc info
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/TransientAliasArena.h"
#include "System/Log.h"

namespace S = RHI::Slots::Cmp;

void NPRStabilizePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;
    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::NPRStabilize_CS, RHI::ShaderStage::CS,
                         "NPRStabilize.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    m_cb.Create(gfx, "NPRStabilize.CB");
    LOG_SUCCESS("NPRStabilizePass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool NPRStabilizePass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::NPRStabilize_CS);
    if (!cs)
    {
        LOG_ERROR("NPRStabilizePass: shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("NPRStabilizePass: PSO creation failed");
        return false;
    }
    return true;
}

void NPRStabilizePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void NPRStabilizePass::BindView(ViewState& view)
{
    m_t = view.Slot() == 0 ? &m_main : &view.Data<ViewTarget>(this).target;
}

void NPRStabilizePass::ViewTarget::Release(IGraphicsDevice& gfx)
{
    if (target.output.IsValid())      gfx.DestroyTexture(target.output);
    if (target.litSnapshot.IsValid()) gfx.DestroyTexture(target.litSnapshot);
    target = Target{};
}

void NPRStabilizePass::PrepareResources(uint32_t w, uint32_t h)
{
    Target& t = *m_t;
    if (w != t.width || h != t.height)
    {
        t.width = w;
        t.height = h;
        t.dirty = true;
    }
    // Re-place when the shared heap was replaced under us, or when Bloom's
    // published base moved (see TransientAliasArena::GroupDBase). Both are
    // silent otherwise: the old texture keeps working, it just sits on the
    // wrong heap or, worse, inside Bloom's grown byte range.
    const bool arenaStale =
        m_aliasArena && t.placed &&
        (t.placedEpoch != m_aliasArena->Epoch() ||
         t.placedBase  != m_aliasArena->GroupDBase());
    if (t.dirty || arenaStale) RebuildTexture(/*allowPlaced=*/true);  // main thread
}

void NPRStabilizePass::RebuildTexture(bool allowPlaced)
{
    Target& t = *m_t;
    if (!m_gfx || t.width == 0 || t.height == 0) return;
    if (t.output.IsValid()) m_gfx->DestroyTexture(t.output);
    t.placed = false;

    RHI::TextureDesc td{};
    td.width      = t.width;
    td.height     = t.height;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "NPRStabilizedHDR";

    // Group D's late joiner: pack above Bloom's chain, which shares computeCL with this pass.
    // Only groups A/B/C (RTR / XeGTAO / VolFog, all direct-queue graph passes fenced behind
    // preComputeCL) share these bytes.
    if (allowPlaced && m_aliasArena && m_t == &m_main)
    {
        uint64_t sz = 0, al = 0;
        if (m_gfx->GetTextureAllocationInfo(td, sz, al))
        {
            const uint64_t base = m_aliasArena->GroupDBase();
            const uint64_t off  = TransientAliasArena::AlignUp(base, al);
            if (m_gfx->EnsureAliasArena(*m_aliasArena, off + sz))
                if (m_gfx->CreateTexturePlaced(td, *m_aliasArena, off, t.output))
                {
                    t.placed      = true;
                    t.placedBase  = base;
                    t.placedEpoch = m_aliasArena->Epoch();
                }
        }
    }

    if (!t.placed && !m_gfx->CreateTexture(td, t.output))
        LOG_ERROR("NPRStabilizePass: output creation failed (%ux%u)", t.width, t.height);

    // Placement receipt: a clean debug layer proves nothing unless the placed path ran. This line
    // says whether it did, and the group-D base the texture sits above.
    LOG_INFO("NPRStabilizePass: output %ux%u %s (base=%llu KiB, epoch %u)%s",
             t.width, t.height, t.placed ? "PLACED in alias arena" : "committed",
             (unsigned long long)(t.placedBase >> 10),
             m_aliasArena ? m_aliasArena->Epoch() : 0u, m_t == &m_main ? "" : " (secondary view)");

    t.outputState = RHI::ResourceState::UNORDERED_ACCESS;
    t.dirty = false;
}

uint64_t NPRStabilizePass::GetOutputSrvHandle() const
{
    if (!m_gfx || !m_t->output.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_t->output);
}

void NPRStabilizePass::PrepareLitSnapshot(IGraphicsDevice& gfx)
{
    Target& t = *m_t;
    t.litSnapshotReady = false;
    const uint32_t w = gfx.GetRenderWidth();
    const uint32_t h = gfx.GetRenderHeight();
    if (w == 0 || h == 0) return;

    if (!t.litSnapshot.IsValid() || t.litW != w || t.litH != h)
    {
        if (t.litSnapshot.IsValid()) gfx.DestroyTexture(t.litSnapshot);

        // Must match the HDR scene target exactly: CopyHdrSceneToScoped is a full-resource copy that
        // needs identical format and size.
        RHI::TextureDesc td{};
        td.width      = w;
        td.height     = h;
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::COPY_DST;
        td.debugName = "NPRLitSnapshot";
        if (!gfx.CreateTexture(td, t.litSnapshot))
        {
            LOG_ERROR("NPRStabilizePass: lit snapshot creation failed (%ux%u)",
                      w, h);
            return;
        }
        t.litW = w;
        t.litH = h;
        t.litSnapshotState = RHI::ResourceState::COPY_DST;
    }
    t.litSnapshotReady = true;
}

void NPRStabilizePass::CopyLitSnapshot(IGraphicsDevice& gfx, RHI::CommandList cl)
{
    Target& t = *m_t;
    if (!t.litSnapshotReady || !t.litSnapshot.IsValid()) return;

    if (t.litSnapshotState != RHI::ResourceState::COPY_DST)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &t.litSnapshot, t.litSnapshotState,
            RHI::ResourceState::COPY_DST), cl);
        t.litSnapshotState = RHI::ResourceState::COPY_DST;
    }

    // Scoped form: this records on a graph worker concurrently with other passes, so the shared
    // HDR state tracker must not be touched (see IGraphicsDevice::CopyHdrSceneToScoped).
    gfx.CopyHdrSceneToScoped(t.litSnapshot, cl);

    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &t.litSnapshot, RHI::ResourceState::COPY_DST,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    t.litSnapshotState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
}

uint64_t NPRStabilizePass::GetLitSnapshotSrv() const
{
    if (!m_gfx || !m_t->litSnapshotReady || !m_t->litSnapshot.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(m_t->litSnapshot);
}

bool NPRStabilizePass::Execute(RHI::CommandList cl)
{
    Target& t = *m_t;
    if (!m_pso.IsValid() || !m_hdrSrv || !m_normalSrv || !m_depthSrv ||
        !m_materialSrv || !m_exposureSrv || t.width == 0 || t.height == 0)
        return false;

    if (t.dirty) RebuildTexture();          // committed fallback: this runs on a worker
    if (!t.output.IsValid()) return false;

    // Arena acquire, after every decline return on purpose: false tells the Renderer not to read
    // the output, so an earlier barrier would fire on frames the texture is neither written nor
    // read. From here the dispatch is unconditional and covers every texel, as arena rule 2
    // requires. Do not touch the output's state here: state tracking runs through aliasing barriers.
    if (t.placed)
        m_gfx->PushBarrier(RHI::GPUBarrier::Aliasing(nullptr, &t.output), cl);

    if (auto* slot = m_cb.Current(*m_gfx))
    {
        StabilizeCB cb{};
        cb.width                   = t.width;
        cb.height                  = t.height;
        cb.sceneMaxBrightness      = m_sceneMaxBrightness;
        cb.exposureProtectionLimit = 0.5f;
        cb.manualExposure          = m_manualExposure;
        cb.useManualExposure       = m_useManualExposure ? 1u : 0u;
        cb.useLitSnapshot          = (m_litSrv != 0) ? 1u : 0u;
        *slot = cb;
    }

    if (t.outputState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        m_gfx->PushBarrier(RHI::GPUBarrier::Image(
            &t.output, t.outputState, RHI::ResourceState::UNORDERED_ACCESS), cl);
        t.outputState = RHI::ResourceState::UNORDERED_ACCESS;
    }

    m_gfx->BindComputePipelineState(m_pso, cl);
    m_gfx->SetComputeRootCBV(S::CB, m_cb.CurrentBuffer(*m_gfx), cl);
    m_gfx->SetComputeDescriptorTable(S::SRV_t0, m_hdrSrv, cl);
    m_gfx->SetComputeDescriptorTable(S::SRV_t1, m_normalSrv, cl);
    m_gfx->SetComputeDescriptorTable(S::SRV_t2, m_materialSrv, cl);
    m_gfx->SetComputeDescriptorTable(S::SRV_t3, m_depthSrv, cl);
    // t4: the lit-scene snapshot. When absent (the frame after a resize) the HDR SRV is bound as
    // a typed placeholder; the CB flag keeps the shader off it.
    m_gfx->SetComputeDescriptorTable(S::SRV_t4,
                                     m_litSrv ? m_litSrv : m_hdrSrv, cl);
    m_gfx->SetComputeDescriptorTable(S::SRV_t5, m_exposureSrv, cl);
    m_gfx->SetComputeDescriptorTable(S::UAV_u0,
                                     m_gfx->GetTextureUAVGpuHandle(t.output), cl);
    m_gfx->DispatchCompute((t.width + 7) / 8, (t.height + 7) / 8, 1, cl);

    m_gfx->PushBarrier(RHI::GPUBarrier::Image(
        &t.output, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    t.outputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    return true;
}
