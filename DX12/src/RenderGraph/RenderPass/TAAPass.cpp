#include "RenderGraph/RenderPass/TAAPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShadingStencil.h"
#include "System/Check.h"
#include "System/Log.h"
#include <cstring>

// Compute root signature slots, from RHI::Slots::Cmp (Graphics/RootSignatureLayout.h,
// mirrors GraphicsDX12::CreateComputeRootSignature). The local names document what
// TAA binds where:
//   [0]  ROOT_CBV   b0 space2
//   [1]  DESC_TABLE t0 space2 <- current HDR
//   [2]  DESC_TABLE t1 space2 <- depth
//   [3]  DESC_TABLE t2 space2 <- history
//   [4]  DESC_TABLE u0 space2 <- output UAV
//   [6]  DESC_TABLE t5 space2 <- outline stencil
//   [7]  DESC_TABLE t3 space2 <- GBuffer surface (roughness/metallic)
//   [8]  DESC_TABLE t4 space2 <- velocity
//   [12] DESC_TABLE t6 space2 <- prev-frame depth
//   [13] DESC_TABLE t7 space2 <- outline history
//   [14] DESC_TABLE u2 space2 <- prev-depth out UAV (this frame's -> next frame's prev)
//   [15] DESC_TABLE u3 space2 <- outline history out UAV
namespace S = RHI::Slots::Cmp;
static constexpr auto kCBSlot          = S::CB;
static constexpr auto kHdrSRV          = S::SRV_t0;
static constexpr auto kDepthSRV        = S::SRV_t1;
static constexpr auto kHistorySRV      = S::SRV_t2;
static constexpr auto kOutputUAV       = S::UAV_u0;
// Slot 6 (t5 space2) is a generic SRV descriptor table in the shared compute root
// signature; TAA binds the outline stencil SRV there (different PSOs may bind
// different descriptor types to one root slot).
static constexpr auto kOutlineStencil  = S::SRV_t5;
static constexpr auto kGBufferSRV      = S::SRV_t3;
static constexpr auto kVelocitySRV     = S::SRV_t4;
static constexpr auto kPrevDepthSRV    = S::SRV_t6;
static constexpr auto kPrevDepthUAV    = S::UAV_u2;
// Slots 13 (t7 space2) and 15 (u3 space2) also serve SpotShadowPass's per-slice
// viewProj buffer and XeGTAO's prefilter UAV. TAA binds neither, so it uses them for
// the outline-coverage ping-pong (the kOutlineStencil trick), leaving the shared
// compute root signature untouched.
static constexpr auto kOutlineHistSRV  = S::SRV_t7;
static constexpr auto kOutlineHistUAV  = S::UAV_u3;

// The stencil bits OutlinePass and the particle passes stamp
// (Graphics/ShadingStencil.h). The shader tests the masks the CB carries.
static constexpr uint32_t kOutlineStencilBitMask = ShadingStencil::kOutlineRim;   // 0x80
static constexpr uint32_t kParticleVfxBitMask    = ShadingStencil::kTaaExclude;   // 0x40

void TAAPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::TAA_CS, RHI::ShaderStage::CS, "TAA.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    // One CB per frame in flight (Graphics/FrameCB.h): frame N+1's CPU write must not
    // race frame N's GPU read of the same UPLOAD memory.
    m_cb.Create(gfx, "TAA.CB");

    // 1x1 zero-velocity fallback (R16G16_FLOAT) for the velocity slot (t4 space2,
    // root 8); see m_zeroVelocityTex. (0,0) = no reprojection.
    {
        const uint16_t zeroVel[2] = { 0, 0 };
        RHI::TextureDesc td{};
        td.width      = 1;
        td.height     = 1;
        td.format     = RHI::Format::R16G16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        RHI::SubresourceData sub{};
        sub.dataPtr    = zeroVel;
        sub.rowPitch   = sizeof(zeroVel);
        sub.slicePitch = sizeof(zeroVel);
        if (gfx.CreateTexture(td, m_zeroVelocityTex, &sub))
            m_zeroVelocitySrv = gfx.GetTextureSRVGpuHandle(m_zeroVelocityTex);
        else
            LOG_ERROR("TAAPass: failed to create zero-velocity fallback");
    }

    LOG_SUCCESS("TAAPass: initialized");
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool TAAPass::CreatePipelines(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::TAA_CS);
    if (!cs)
    {
        LOG_ERROR("TAAPass: TAA_CS shader not found");
        return false;
    }

    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("TAAPass: PSO creation failed");
        return false;
    }
    return true;
}

void TAAPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void TAAPass::SetFrameData(const DirectX::XMFLOAT4X4& invVPNoJitter,
                           const DirectX::XMFLOAT4X4& prevVP,
                           float historyWeight,
                           bool  hasHistory,
                           float deltaTime)
{
    std::memcpy(m_cbData.invViewProjNoJitter, &invVPNoJitter, sizeof(float) * 16);
    std::memcpy(m_cbData.prevViewProj,        &prevVP,        sizeof(float) * 16);
    m_cbData.historyWeight = historyWeight;
    m_cbData.hasHistory    = hasHistory ? 1.0f : 0.0f;
    m_cbData.deltaTime     = deltaTime;
}

uint64_t TAAPass::GetResolvedSrvHandle() const
{
    // The buffer the last Execute wrote (the history's newest content: Cur once this
    // frame's Execute ran, Prev until then). Not the buffer this frame read, the previous
    // frame's resolve: handing that on would put the whole post chain (and the presented
    // picture) a frame behind, with this frame's depth and normals beside last frame's
    // colour.
    if (!m_gfx || !m_hist) return 0;
    const RHI::Texture& resolved = m_hist->tex.LastWritten(0);
    if (!resolved.IsValid()) return 0;
    return m_gfx->GetTextureSRVGpuHandle(resolved);
}

void TAAPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
}

void TAAPass::History::Release(IGraphicsDevice& gfx)
{
    tex.Release(gfx);   // deferred internally: the colour's two, the depth's, the outline's
    // Zero the size latch so the next PrepareResources rebuilds; history is
    // stale by definition after a release.
    width = height = 0;
    hasHistory = false;
}

void TAAPass::RebuildBuffers()
{
    if (!m_gfx || m_vpW == 0 || m_vpH == 0) return;
    if (!ENGINE_CHECK_ONCE(m_hist != nullptr, "TAAPass: no view bound (PassSet::BindView) - TAA does not run"))
        return;
    History& h = *m_hist;

    // Each of the three ping-pongs (RG::HistoryTexture: colour, prev-depth, outline
    // coverage) is destroyed and made again at the viewport's size, created
    // UNORDERED_ACCESS (from here its state is the device's book's): Cur is slot 0
    // again, nothing written.
    h.tex.Rebuild(*m_gfx, m_vpW, m_vpH);
    h.hasHistory    = false;
    h.width         = m_vpW;
    h.height        = m_vpH;

    LOG_INFO("TAAPass: rebuilt ping-pong buffers %ux%u", m_vpW, m_vpH);
}

void TAAPass::PrepareResources(uint32_t w, uint32_t h)
{
    SetViewportSize(w, h);
    if (!m_hist || m_vpW != m_hist->width || m_vpH != m_hist->height)
        RebuildBuffers();
}

void TAAPass::ReleaseResources()
{
    if (!m_gfx || !m_hist || !m_hist->tex.IsValid()) return;   // already released (or never made)
    m_hist->Release(*m_gfx);
    LOG_INFO("TAAPass: history working set released (another resolve path owns AA)");
}

RHI::CommandList TAAPass::Execute(RHI::CommandList cl)
{
    // Disabled (editor toggle / AA mode): skip the resolve; the Renderer routes
    // downstream passes to the raw HDR scene SRV. A stand-in frame (SetStandIn) resolves.
    if (!m_enabled && !m_standIn) return cl;

    if (!m_pso.IsValid() || m_vpW == 0 || m_vpH == 0) return cl;

    // Resize on viewport change.
    if (!m_hist || m_vpW != m_hist->width || m_vpH != m_hist->height)
        RebuildBuffers();
    if (!m_hist) return cl;   // no view bound: RebuildBuffers said so
    History& h = *m_hist;

    // The ping-pong roles this frame: Cur, the resolve this frame writes; Prev, the
    // history - fixed since the frame's OnPrepare flipped them.
    const RHI::Texture& resolved      = h.tex.Cur(0);
    const RHI::Texture& history       = h.tex.Prev(0);
    const RHI::Texture& prevDepthOut  = h.tex.Cur(1);
    const RHI::Texture& prevDepthIn   = h.tex.Prev(1);
    const RHI::Texture& outlineOut    = h.tex.Cur(2);
    const RHI::Texture& outlineIn     = h.tex.Prev(2);
    if (!resolved.IsValid() || !history.IsValid()) return cl;
    // A second Execute without the frame's OnPrepare would resolve into the
    // buffer it wrote last and read the one before that as its history.
    ENGINE_CHECK_ONCE(!h.tex.WrittenThisFrame(),
        "TAAPass: Execute ran again without the frame's OnPrepare - the history did not flip");

    auto& gfx = *m_gfx;

    // Upload TAACB.
    m_cbData.width  = m_vpW;
    m_cbData.height = m_vpH;
    if (!h.hasHistory) m_cbData.hasHistory = 0.0f;  // override if no history
    m_cbData.colorBoxSigma         = colorBoxSigma;
    m_cbData.colorBoxSigmaSpecular = colorBoxSigmaSpecular;
    m_cbData.specularRoughnessMax  = specularRoughnessMax;
    m_cbData.antiFlicker           = antiFlicker ? 1u : 0u;
    m_cbData.velocityWiden         = velocityWiden;
    m_cbData.sharpenStrength       = sharpenStrength;
    m_cbData.motionVelocityStartPx = motionVelocityStartPx;
    m_cbData.motionVelocityEndPx   = motionVelocityEndPx;
    m_cbData.motionAlphaFloor      = motionAlphaFloor;
    m_cbData.motionRawBlend        = motionRawBlend;
    m_cbData.outlineMinAlpha       = outlineMinAlpha;
    // No stencil SRV plumbed in: disable the lookup. The shader samples only when this
    // bit is non-zero, so an unbound stencil descriptor is harmless.
    m_cbData.outlineStencilBit     = m_outlineStencilSrvHandle ? kOutlineStencilBitMask : 0u;
    m_cbData.outlineStencilDebug   = outlineStencilDebug ? 1u : 0u;
    m_cbData.outlineVacatedAlpha   = outlineVacatedAlpha;
    // Zero also disables the shader's reads/writes, so a failed texture create
    // degrades to stock behaviour instead of an unbound descriptor.
    m_cbData.hasOutlineHist        = (outlineOut.IsValid() && outlineIn.IsValid()) ? 1u : 0u;
    // Particle-VFX exclusion shares the same stencil SRV (different bit). Gated
    // on the SRV being plumbed in, exactly like the outline bit.
    m_cbData.particleVfxMinAlpha    = particleVfxMinAlpha;
    m_cbData.particleVfxBit         = m_outlineStencilSrvHandle ? kParticleVfxBitMask : 0u;
    if (auto* slot = m_cb.Current(gfx))
        *slot = m_cbData;

    // The textures' states are the device's book's: each TransitionTo records nothing
    // when the texture is there already.
    // The write buffer -> UAV for the dispatch output.
    RG::TransitionTo(gfx, cl, resolved, RHI::ResourceState::UNORDERED_ACCESS);

    // History -> SRV, even on the first frame (h.hasHistory == false): the shader
    // ignores the sample then, but the resource must be in a valid SRV state before the
    // descriptor is bound.
    RG::TransitionTo(gfx, cl, history, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    // Prev depth: read = SRV, write = UAV.
    RG::TransitionTo(gfx, cl, prevDepthIn,  RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    RG::TransitionTo(gfx, cl, prevDepthOut, RHI::ResourceState::UNORDERED_ACCESS);

    // Outline history: read = SRV, write = UAV.
    RG::TransitionTo(gfx, cl, outlineIn,  RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
    RG::TransitionTo(gfx, cl, outlineOut, RHI::ResourceState::UNORDERED_ACCESS);

    // Dispatch.
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, m_cb.CurrentBuffer(gfx), cl);

    if (m_hdrSrvHandle)
        gfx.SetComputeDescriptorTable(kHdrSRV, m_hdrSrvHandle, cl);
    if (m_depthSrvHandle)
        gfx.SetComputeDescriptorTable(kDepthSRV, m_depthSrvHandle, cl);
    if (m_gbufferSrvHandle)
        gfx.SetComputeDescriptorTable(kGBufferSRV, m_gbufferSrvHandle, cl);
    // Always bind slot 8: TAA.cs reads gVelocity unconditionally, so fall back to the
    // zero-velocity texture (no reprojection) when no GBuffer velocity SRV is available;
    // the root argument is never uninitialized (GBV).
    gfx.SetComputeDescriptorTable(kVelocitySRV,
        m_velocitySrvHandle ? m_velocitySrvHandle : m_zeroVelocitySrv, cl);
    if (m_outlineStencilSrvHandle)
        gfx.SetComputeDescriptorTable(kOutlineStencil, m_outlineStencilSrvHandle, cl);

    // Bind the history (even on the first frame: the shader ignores it when
    // hasHistory = 0).
    const uint64_t historySrv = m_gfx->GetTextureSRVGpuHandle(history);
    if (historySrv)
        gfx.SetComputeDescriptorTable(kHistorySRV, historySrv, cl);

    const uint64_t outputUav = gfx.GetTextureUAVGpuHandle(resolved);
    if (outputUav)
        gfx.SetComputeDescriptorTable(kOutputUAV, outputUav, cl);

    // Prev-depth bindings (read=SRV from Prev, write=UAV into Cur).
    const uint64_t prevDepthSrv = m_gfx->GetTextureSRVGpuHandle(prevDepthIn);
    if (prevDepthSrv)
        gfx.SetComputeDescriptorTable(kPrevDepthSRV, prevDepthSrv, cl);
    const uint64_t prevDepthUav = gfx.GetTextureUAVGpuHandle(prevDepthOut);
    if (prevDepthUav)
        gfx.SetComputeDescriptorTable(kPrevDepthUAV, prevDepthUav, cl);

    const uint64_t outlineHistSrv = m_gfx->GetTextureSRVGpuHandle(outlineIn);
    const uint64_t outlineHistUav = gfx.GetTextureUAVGpuHandle(outlineOut);
    if (outlineHistSrv)
        gfx.SetComputeDescriptorTable(kOutlineHistSRV, outlineHistSrv, cl);
    if (outlineHistUav)
        gfx.SetComputeDescriptorTable(kOutlineHistUAV, outlineHistUav, cl);

    gfx.DispatchCompute((m_vpW + 7) / 8, (m_vpH + 7) / 8, 1, cl);

    // Resolved buffer -> SRV for downstream passes; it is also next frame's history,
    // read in this state.
    RG::TransitionTo(gfx, cl, resolved, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    // Prev-depth: this frame's UAV write is next frame's SRV read. Write -> SRV; the
    // read slot stays SRV.
    RG::TransitionTo(gfx, cl, prevDepthOut, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    // Outline history: the same hand-off (the next frame's OnPrepare flips the roles).
    RG::TransitionTo(gfx, cl, outlineOut, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);

    // This frame's resolve is Cur from here (GetResolvedSrvHandle).
    h.tex.MarkWritten();
    h.hasHistory = true;

    return cl;
}
