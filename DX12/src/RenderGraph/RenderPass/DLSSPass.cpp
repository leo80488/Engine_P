#include "RenderGraph/RenderPass/DLSSPass.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/ShaderDiskCache.h"
#include "Settings/PassSettings.h"     // pass.dlss.*
#include "System/Log.h"
#include "System/CommandLine.h"

#include <algorithm>
#include <cmath>

namespace
{
    // The radiance-scale pass's private root signature (PrepareRadianceEncoding):
    // one parameter, five 32-bit constants.
    constexpr RHI::Slots::CmpParam kScaleRootConstants{ 0 };

    // Radical inverse in an arbitrary base: the standard Halton construction.
    float Halton(uint32_t index, uint32_t base)
    {
        float f = 1.0f, r = 0.0f;
        while (index > 0)
        {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(index % base);
            index /= base;
        }
        return r;
    }

    // DLSS wants the jitter phase count to grow with the square of the upscale
    // ratio: 8 phases at 1:1 (DLAA), 18 at Quality, 32 at Performance, 72 at
    // Ultra Performance. Clamped so a bad size can never produce 0 (which would
    // divide by zero below) or an absurdly long sequence.
    uint32_t JitterPhaseCount(uint32_t renderW, uint32_t displayW)
    {
        if (renderW == 0 || displayW == 0) return 8;
        const float ratio = static_cast<float>(displayW) / static_cast<float>(renderW);
        const int   n     = static_cast<int>(std::ceil(8.0f * ratio * ratio));
        return static_cast<uint32_t>(n < 8 ? 8 : (n > 128 ? 128 : n));
    }
}

bool DLSSPass::Init(IGraphicsDevice& /*gfx*/)
{
    // Output texture + feature configuration are created lazily by EnsureContext once
    // the sizes are known, as in FSR2Pass.
    return true;
}

void DLSSPass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
}

void DLSSPass::Shutdown(IGraphicsDevice& gfx)
{
    // Turning the feature off frees DLSS's internal history/weight allocations
    // synchronously, and frames recorded up to kFrameCount-1 ago may still read them on
    // the GPU (BeginFrame only waits per slot, it does not drain). Drain first or the GPU
    // reads freed memory: a TDR, the same hazard and fix as FSR2Pass::Shutdown.
    gfx.FlushAndWait();

    if (m_configured)
    {
        if(m_lastRR)
        {
            DirectX::XMFLOAT4X4 identity;
            DirectX::XMStoreFloat4x4(&identity,DirectX::XMMatrixIdentity());
            SL::StreamlineBackend::Get().SetDLSSDOptions(SL::DLSSMode::Off,m_displayW,m_displayH,identity,identity);
            // Streamline allocates the RR viewport on its first evaluate;
            // freeing one that never evaluated is eErrorInvalidParameter.
            if(m_rrEvaluations>0) SL::StreamlineBackend::Get().FreeFeatureResources(SL::Feature::DLSS_RR);
        }
        SL::StreamlineBackend::Get().SetDLSSOptions(SL::DLSSMode::Off,
                                                    m_displayW, m_displayH,
                                                    0.0f, /*useAutoExposure=*/true);
        m_configured = false;
    }
    if (m_rrExposureTex.IsValid()) gfx.DestroyTexture(m_rrExposureTex);
    m_rrExposure = 0.f;
    if (m_rrEncodedInput.IsValid()) gfx.DestroyTexture(m_rrEncodedInput);
    m_rrRadianceScale=1.f;
    if (m_outputCreated && m_outputTex.IsValid())
    {
        gfx.DestroyTexture(m_outputTex);
        m_outputCreated = false;
    }
    m_lastRenderW = m_lastRenderH = m_lastDisplayW = m_lastDisplayH = 0;
    m_lastQualityMode = SL::DLSSMode::Off;
    m_healthy=true; m_rrEvaluations=0;
    // The radiance encoding's pipeline goes with the feature (PrepareRadianceEncoding rebuilds it).
    gfx.DestroyPipelineState(m_scalePSO);
    gfx.DestroyPipelineLayout(m_scaleLayout);
}

void DLSSPass::ServiceRetry()
{
    if (m_healthy) { m_rrFailed = false; return; }
    if (!m_rrFailed || Sys::CommandLine::Get().Has("-dlss_rr_fail_evaluate")) return;
    const uint32_t wait = 120u << std::min(m_rrRetries, 4u);
    if (++m_rrFailFrames < wait) return;
    m_rrFailed = false; m_rrFailFrames = 0; ++m_rrRetries;
    m_lastRenderW = 0;   // the next EnsureContext rebuilds: RR's viewport freed and re-made
    m_healthy = true;
    LOG_INFO("[DLSS_RR] retry=%u afterFrames=%u", m_rrRetries, wait);
}

bool DLSSPass::RebuildForSizes(IGraphicsDevice& gfx)
{
    auto& backend = SL::StreamlineBackend::Get();
    if (!backend.IsFeatureSupported(SL::Feature::DLSS) || (m_rr && !backend.IsFeatureSupported(SL::Feature::DLSS_RR)))
        return false;

    // Same drain-before-free reasoning as Shutdown.
    gfx.FlushAndWait();
    if(m_configured && m_lastRR)
    {
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity,DirectX::XMMatrixIdentity());
        backend.SetDLSSDOptions(SL::DLSSMode::Off,m_lastDisplayW,m_lastDisplayH,identity,identity);
        // As in Shutdown: nothing to free before the first RR evaluate.
        if(m_rrEvaluations>0) backend.FreeFeatureResources(SL::Feature::DLSS_RR);
    }
    m_configured=false;

    if (m_rrExposureTex.IsValid()) gfx.DestroyTexture(m_rrExposureTex);
    m_rrExposure = 0.f;
    if (m_rrEncodedInput.IsValid()) gfx.DestroyTexture(m_rrEncodedInput);
    m_rrRadianceScale=1.f;
    if (m_outputCreated && m_outputTex.IsValid())
    {
        gfx.DestroyTexture(m_outputTex);
        m_outputCreated = false;
    }

    if (!PrepareRadianceEncoding(gfx)) return false;
    // Explicit diagnostic exposure is a tagged 1x1 texture: the RR SDK's auto-exposure
    // path ignores exposureScale alone. Only when pass.dlss.rrExposure is given.
    if (m_rr && Settings::Pass::dlssRrExposureGiven())
    {
        m_rrExposure = Settings::Pass::dlssRrExposure();
        if (!std::isfinite(m_rrExposure) || m_rrExposure <= 0.f) return false;
        RHI::TextureDesc exposure{};
        exposure.width = exposure.height = 1;
        exposure.format = RHI::Format::R32_FLOAT;
        exposure.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        exposure.usage = RHI::Usage::DEFAULT;
        exposure.layout = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
        exposure.debugName = "DLSSPass.RRExposure";
        RHI::SubresourceData initial{&m_rrExposure,sizeof(float),sizeof(float)};
        if (!gfx.CreateTexture(exposure,m_rrExposureTex,&initial)) return false;
    }
    RHI::TextureDesc td{};
    td.width      = m_displayW;
    td.height     = m_displayH;
    td.format     = m_rrRadianceScale!=1.f ? RHI::Format::R32G32B32A32_FLOAT : RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "DLSSPass.Output";
    if (!gfx.CreateTexture(td, m_outputTex))
    {
        LOG_ERROR("DLSSPass: failed to create output texture (%ux%u)", m_displayW, m_displayH);
        return false;
    }
    m_outputCreated = true;
    m_outputState   = RHI::ResourceState::UNORDERED_ACCESS;

    // The engine's exposure (the Exposure tag) replaces the SDK's metering, unless the explicit RR
    // diagnostic texture is supplied or pass.upscaler.engineExposure is off.
    m_engineExposure = Settings::Pass::upscalerEngineExposure();
    if (!backend.SetDLSSOptions(m_qualityMode, m_displayW, m_displayH,
                                sharpness, /*useAutoExposure=*/m_rrExposure==0.f && !m_engineExposure, preset))
    {
        LOG_ERROR("DLSSPass: SetDLSSOptions failed (%s, preset %s, %ux%u)",
                  SL::DLSSModeName(m_qualityMode), SL::DLSSPresetName(preset),
                  m_displayW, m_displayH);
        gfx.DestroyTexture(m_outputTex);
        m_outputCreated = false;
        return false;
    }

    // A rebuild is the retry point: whatever made evaluation fail may well have
    // been the old configuration.
    m_healthy             = true;
    m_consecutiveFailures = 0;

    m_configured      = true;
    m_lastRenderW     = m_renderW;
    m_lastRenderH     = m_renderH;
    m_lastDisplayW    = m_displayW;
    m_lastDisplayH    = m_displayH;
    m_lastQualityMode = m_qualityMode;
    m_lastPreset      = preset;
    m_lastRR=m_rr; m_lastRRModel=m_rrModel; m_rrEvaluations=0;
    SetReset(true);             // history is empty after a rebuild
    LOG_INFO("[UPSCALER_EXPOSURE] upscaler=dlss engine=%d rr=%d", m_engineExposure ? 1 : 0, m_rr ? 1 : 0);

    LOG_INFO("DLSSPass: %s — %ux%u render -> %ux%u display, preset %s",
             SL::DLSSModeName(m_qualityMode),
             m_renderW, m_renderH, m_displayW, m_displayH,
             SL::DLSSPresetName(preset));
    if(m_rr)
    {
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity,DirectX::XMMatrixIdentity());
        return UpdateRRView(identity,identity);
    }
    return true;
}

bool DLSSPass::UpdateRRView(const DirectX::XMFLOAT4X4& view,const DirectX::XMFLOAT4X4& inverseView)
{
    if(!m_rr || !IsContextValid()) return false;
    const float exposureScale=Settings::Pass::dlssRrExposureScale();
    if(!SL::StreamlineBackend::Get().SetDLSSDOptions(m_qualityMode,m_displayW,m_displayH,view,inverseView,m_rrModel,exposureScale,
        m_rrRadianceScale*Settings::Pass::dlssRrPreExposure()))
    { m_healthy=false; m_rrFailed=true; m_rrFailFrames=0; LOG_WARNING("DLSS-RR: options failed; reverting to PT accumulation"); return false; }
    return true;
}

bool DLSSPass::EnsureContext(IGraphicsDevice& gfx)
{
    if (failContextForTest) return false;
    if (m_renderW == 0 || m_renderH == 0 || m_displayW == 0 || m_displayH == 0)
        return false;

    const bool changed = (m_renderW     != m_lastRenderW)  ||
                         (m_renderH     != m_lastRenderH)  ||
                         (m_displayW    != m_lastDisplayW) ||
                         (m_displayH    != m_lastDisplayH) ||
                         (m_qualityMode != m_lastQualityMode) ||
                         // The preset only reaches the SDK through
                         // SetDLSSOptions, which only runs on a rebuild: omit
                         // it here and changing the model in the editor is a
                         // write nobody observes.
                         (preset        != m_lastPreset) || m_rr!=m_lastRR || m_rrModel!=m_lastRRModel ||
                         m_engineExposure != Settings::Pass::upscalerEngineExposure();
    if (m_configured && m_outputCreated && !changed)
        return true;
    return RebuildForSizes(gfx);
}

void DLSSPass::SetSrcTexturesComputeRead(const RHI::Texture* color,
                                         const RHI::Texture* depth,
                                         const RHI::Texture* motion)
{
    m_colorTex        = color;
    m_colorIsHdrScene = (color == nullptr);
    m_depthTex        = depth;
    m_motionTex       = motion;
}

bool DLSSPass::QueryJitter(uint32_t frameIndex, float& outX, float& outY) const
{
    if (m_renderW == 0 || m_displayW == 0) return false;
    const uint32_t phases = JitterPhaseCount(m_renderW, m_displayW);
    // Halton is 1-indexed: index 0 yields (0,0), which would waste one phase of
    // every sequence on the unjittered sample.
    const uint32_t idx = (frameIndex % phases) + 1;
    outX = Halton(idx, 2) - 0.5f;
    outY = Halton(idx, 3) - 0.5f;
    return true;
}

void DLSSPass::Execute(RHI::CommandList cl, IGraphicsDevice& gfx, void* frameToken)
{
    // Matches the predicate Renderer uses for `dlssResolved`, so bailing here is
    // consistent: the Renderer also skips our output and sizes the post chain at
    // render res.
    if (!IsContextValid()) return;

    // Past this point the Renderer samples m_outputTex as an SRV this frame: its
    // `dlssResolved` is IsContextValid(), evaluated on the main thread before this worker
    // started, so it cannot learn that the evaluate below was skipped. Every exit from
    // here MUST leave the texture in SHADER_RESOURCE_COMPUTE; left in UNORDERED_ACCESS the
    // post chain reads a resource whose real state disagrees with its descriptor
    // (debug-layer INVALID_SUBRESOURCE_STATE; on hardware, undefined reads with no
    // UAV->SRV flush). Hence the single exit at the end (FSR2Pass's only failure falls
    // through to its closing barrier the same way).
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    bool  evaluated = false;

    // The Renderer hands RHI textures; resolve them to the resources Streamline takes,
    // for this evaluate.
    auto native = [&dx12](const RHI::Texture* t) { return (t && t->IsValid()) ? dx12.GetTextureResource(*t) : nullptr; };
    m_colorRes  = m_colorIsHdrScene ? dx12.GetHdrSceneResource() : native(m_colorTex);
    m_depthRes  = native(m_depthTex);
    m_motionRes = native(m_motionTex);
    for (uint32_t i = 0; i < 4; ++i)
        m_rrGuides[i] = native(m_rrGuideTex[i]);
    m_rrDiffuseDistance         = native(m_rrDiffuseDistanceTex);
    m_rrColorBeforeTransparency = native(m_rrColorBeforeTransparencyTex);
    m_biasCurrentColor          = native(m_biasCurrentColorTex);

    ID3D12GraphicsCommandList* nativeCL = dx12.GetNativeCommandList(cl);

    if (!frameToken)
        LOG_WARNING("DLSSPass: no Streamline frame token — output left unmodified");
    else if (!m_colorRes || !m_depthRes || !m_motionRes)
        LOG_WARNING("DLSSPass: missing input resource(s) — output left unmodified");
    else if(m_rr && (!m_rrGuides[0] || !m_rrGuides[1] || !m_rrGuides[2] || !m_rrGuides[3]))
        LOG_WARNING("DLSS-RR: missing guide resources");
    else if (m_rrRadianceScale!=1.f && m_colorSrvIndex==~0u)
        LOG_WARNING("DLSS-RR: missing color SRV for radiance encoding");
    else if (m_engineExposure && !(m_rr && m_rrExposureTex.IsValid()) && !native(m_exposureTex))
        LOG_WARNING("DLSSPass: missing engine exposure texture — output left unmodified");
    else if (!nativeCL)
        LOG_ERROR("DLSSPass: native command list is null");
    else
        evaluated = true;   // inputs are sound; attempt the real work below

    if (evaluated)
    {

        // DLSS writes the output through a UAV.
        if (m_outputState != RHI::ResourceState::UNORDERED_ACCESS)
        {
            gfx.PushBarrier(RHI::GPUBarrier::Image(
                &m_outputTex, m_outputState,
                RHI::ResourceState::UNORDERED_ACCESS), cl);
            m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
        }

        // The caller's pre-compute list already put all three inputs in
        // NON_PIXEL_SHADER_RESOURCE; report that honestly rather than letting
        // Streamline guess, since it uses the declared state to decide whether
        // it needs to insert its own transitions.
        constexpr uint32_t kComputeRead =
            static_cast<uint32_t>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        constexpr uint32_t kUAV =
            static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // ONLY the colour buffers. Depth and motion vectors are tagged once, by the Renderer,
        // with the longer ValidUntilPresent lifetime, because Frame Generation consumes them
        // at Present (and FG can be on with DLSS SR off); tagging them here, later in the
        // frame, would overwrite that with the shorter lifetime.
        // Inputs are at render resolution, only the output at display res. Zero extents =
        // the whole resource: the render targets are sized to the render resolution, not
        // sub-rects of a display-res allocation.
        ID3D12Resource* colorInput=m_colorRes;
        if(m_rrRadianceScale!=1.f)
        {
            if(m_rrEncodedState!=RHI::ResourceState::UNORDERED_ACCESS)
                gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrEncodedInput,m_rrEncodedState,RHI::ResourceState::UNORDERED_ACCESS),cl);
            ScaleRadiance(cl,gfx,m_colorSrvIndex,m_rrEncodedInput,m_renderW,m_renderH,m_rrRadianceScale);
            gfx.PushBarrier(RHI::GPUBarrier::Image(&m_rrEncodedInput,RHI::ResourceState::UNORDERED_ACCESS,RHI::ResourceState::SHADER_RESOURCE_COMPUTE),cl);
            m_rrEncodedState=RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
            colorInput=dx12.GetTextureResource(m_rrEncodedInput);
        }
        SL::ResourceTag tags[16]{};
        uint32_t tagCount=2;
        tags[0] = { SL::BufferType::ScalingInputColor,  colorInput,  kComputeRead,
                    0, 0, 0, 0, SL::ResourceLife::ValidUntilEvaluate };
        tags[1] = { SL::BufferType::ScalingOutputColor,
                    dx12.GetTextureResource(m_outputTex), kUAV,
                    0, 0, 0, 0, SL::ResourceLife::ValidUntilEvaluate };
        // The reactive mask as DLSS SR's BiasCurrentColor hint, lerp(history, current,
        // bias) - TAA's own floor on the same stencil bits. Tagged on every SR evaluate, null
        // when there is none this frame, so a hint never outlives its mask. Not on RR: its
        // guide documents no such input (UpscalerReactiveMaskPass.h).
        if(!m_rr)
            tags[tagCount++] = { SL::BufferType::BiasCurrentColor, m_biasCurrentColor, kComputeRead,
                                 0, 0, m_biasCurrentColor ? m_renderW : 0u, m_biasCurrentColor ? m_renderH : 0u,
                                 SL::ResourceLife::ValidUntilEvaluate };

        if(m_rr)
        {
            // The packed normal + roughness guide is tagged NormalRoughness (sl 14);
            // pass.dlss.rrNormalTag=normals tags it as plain Normals (sl 5) -
            // tools/verify_rr_truth.py asks the model which of the two it reads.
            const bool normalsTag=Settings::Pass::dlssRrNormalTag()=="normals";
            const SL::BufferType kinds[]={SL::BufferType::Albedo,SL::BufferType::SpecularAlbedo,
                normalsTag?SL::BufferType::Normals:SL::BufferType::NormalRoughness,SL::BufferType::SpecularHitDistance};
            for(uint32_t i=0;i<4;++i)
                tags[tagCount++]={kinds[i],m_rrGuides[i],kComputeRead,0,0,m_renderW,m_renderH,SL::ResourceLife::ValidUntilEvaluate};
            if(m_rrDiffuseDistance)
                tags[tagCount++]={SL::BufferType::DiffuseHitDistance,m_rrDiffuseDistance,kComputeRead,0,0,m_renderW,m_renderH,
                    SL::ResourceLife::ValidUntilEvaluate};
            // The colour before transparency: the one colour-chain input the DLSS-RR guide
            // documents ("helps with ghosting or disappearing particles"). The others are
            // undocumented for RR, and ColorBeforeParticles turns its output black.
            if(m_rrColorBeforeTransparency)
                tags[tagCount++]={SL::BufferType::ColorBeforeTransparency,m_rrColorBeforeTransparency,kComputeRead,0,0,
                    m_renderW,m_renderH,SL::ResourceLife::ValidUntilEvaluate};
        }
        auto& backend = SL::StreamlineBackend::Get();
        // The explicit RR diagnostic texture first, else the engine's exposure.
        const auto exposureRes=(m_rr && m_rrExposureTex.IsValid()) ? dx12.GetTextureResource(m_rrExposureTex)
                              : (m_engineExposure ? native(m_exposureTex) : nullptr);
        if(exposureRes)
            tags[tagCount++]={SL::BufferType::Exposure,exposureRes,kComputeRead,
                0,0,1,1,SL::ResourceLife::ValidUntilEvaluate};
        const bool forceFailure=m_rr && Sys::CommandLine::Get().Has("-dlss_rr_fail_evaluate");
        const bool encodingOnly=m_rr && Sys::CommandLine::Get().Has("-dlss_rr_encoding_only");
        bool succeeded=false;
        if(encodingOnly)
        {
            const uint32_t input=m_rrRadianceScale==1.f?m_colorSrvIndex:
                gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureSRVGpuHandle(m_rrEncodedInput));
            ScaleRadiance(cl,gfx,input,m_outputTex,m_displayW,m_displayH,1.f);
            succeeded=true;
        }
        else succeeded=!forceFailure && backend.TagResourcesForFrame(tags,tagCount,frameToken,nativeCL) &&
            backend.EvaluateFeature(m_rr?SL::Feature::DLSS_RR:SL::Feature::DLSS,frameToken,nativeCL);
        if(succeeded)
        {
            // History is valid from here; the reset was consumed by the constants that
            // accompanied this evaluate - if they carried one. A reset asked for after they were
            // uploaded (the render half's own hard cuts, an AA-mode switch) stays pending for
            // the next frame's constants.
            if(m_rrRadianceScale!=1.f)
            {
                dx12.RebindShaderVisibleHeaps(cl);
                gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_outputTex),cl);
                ScaleRadiance(cl,gfx,~0u,m_outputTex,m_displayW,m_displayH,1.f/m_rrRadianceScale);
            }
            if(m_constantsCarryReset) SetReset(false);
            m_consecutiveFailures = 0;
            if(encodingOnly && (++m_rrEvaluations==1 || m_rrEvaluations%60==0))
                LOG_INFO("[RR_ENCODING] bypass=1 scale=%.9g",m_rrRadianceScale);
            if(m_rr && !encodingOnly && m_rrEvaluations==1)
            {
                uint64_t vram=0;
                const bool known=SL::StreamlineBackend::Get().GetDLSSDVRAMBytes(vram);
                LOG_INFO("[DLSS_RR] vramMB=%.1f known=%u normalTag=%s render=%ux%u display=%ux%u",double(vram)/(1024.0*1024.0),known?1u:0u,
                    Settings::Pass::dlssRrNormalTag().c_str(),m_renderW,m_renderH,m_displayW,m_displayH);
            }
            if(m_rr && !encodingOnly && (++m_rrEvaluations==1 || m_rrEvaluations%60==0))
                LOG_INFO("[DLSS_RR] evaluated=%u model=%s guides=%u colorBeforeTransparency=%u exposureScale=%.9g exposure=%.9g preExposure=%.9g radianceScale=%.9g",m_rrEvaluations,SL::DLSSRRModelName(m_rrModel),m_rrDiffuseDistance?5u:4u,
                    m_rrColorBeforeTransparency?1u:0u,
                    Settings::Pass::dlssRrExposureScale(),m_rrExposure,m_rrRadianceScale*Settings::Pass::dlssRrPreExposure(),m_rrRadianceScale);
        }
        else if(m_rr)
        {
            // Native PT input remains valid for the current frame. Renderer
            // rechecks health before ToneMap, then restores progressive PT.
            m_healthy=false; m_rrFailed=true; m_rrFailFrames=0;
            LOG_WARNING("[DLSS_RR] evaluateFailed=1 fallback=PTAccumulation injected=%u",forceFailure);
        }
        else if (++m_consecutiveFailures >= kMaxConsecutiveFailures)
        {
            // Stop claiming to be a working upscaler. IsContextValid() goes false, so next frame
            // the Renderer sizes the post chain at render res and resolves through TAA: a correct
            // image instead of an unexplained black one. This also bounds the log, which the
            // backend otherwise fills with a warning every frame.
            m_healthy = false;
            LOG_ERROR("DLSSPass: disabling DLSS after %u consecutive evaluate failures "
                      "- falling back to TAA. Re-select the AA mode to retry.",
                      m_consecutiveFailures);
        }

        // Unconditional, exactly like the FFX path: Streamline binds its own
        // descriptor heaps and does not restore ours, and it may well have done
        // so before the call that reported failure. Every following pass assumes
        // the engine heaps are still bound (most never call SetDescriptorHeaps).
        dx12.RebindShaderVisibleHeaps(cl);
        // AutoExposure detaches an SRV before binding a PSO, including its
        // disabled path: the engine's compute root layout must be bound after
        // the evaluate - never our five-DWORD one, nor whatever Streamline
        // left (an earlier dispatch's binding is not a contract).
        dx12.RestoreDefaultComputeRootSignature(cl);
    }

    if(m_rr && !evaluated) { m_healthy=false; m_rrFailed=true; m_rrFailFrames=0; }

    // Single exit. Whatever happened above, hand the Renderer the state its SRV binding
    // requires. On a skipped frame the texture keeps the previous frame's pixels: a stale
    // image rather than an invalid read, as when an FSR2 dispatch fails.
    if (m_outputState != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_outputTex,
            m_outputState,
            RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
        m_outputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    }
}

uint64_t DLSSPass::GetOutputSrvGpuHandle(const IGraphicsDevice& gfx) const
{
    if (!m_outputCreated || !m_outputTex.IsValid()) return 0;
    return gfx.GetTextureSRVGpuHandle(m_outputTex);
}

// Reversible HDR encoding for RR. Scene lighting and post-AA radiance units stay
// unchanged; guides are reflectance/geometry and must not be scaled.
bool DLSSPass::PrepareRadianceEncoding(IGraphicsDevice& gfx)
{
    m_rrRadianceScale=m_rr?Settings::Pass::dlssRrRadianceScale():1.f;
    if(!std::isfinite(m_rrRadianceScale) || m_rrRadianceScale<1e-6f || m_rrRadianceScale>1e6f) return false;
    if(!m_rr || (m_rrRadianceScale==1.f && !Sys::CommandLine::Get().Has("-dlss_rr_encoding_only"))) return true;
    if(!m_scalePSO.IsValid())
    {
        if(!m_scaleLayout.IsValid())
        {
            RHI::PipelineLayoutBuilder b("DLSS.RadianceScale");
            b.Constants(kScaleRootConstants,5,0).Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
            if(!gfx.CreatePipelineLayout(b.Desc(),m_scaleLayout)) return false;
        }
        if(!CreateScalePSO(gfx)) return false;
    }
    RHI::TextureDesc td{};
    td.width=m_renderW; td.height=m_renderH;
    td.format=RHI::Format::R32G32B32A32_FLOAT;
    td.bindFlags=RHI::BindFlag::SHADER_RESOURCE|RHI::BindFlag::UNORDERED_ACCESS;
    td.usage=RHI::Usage::DEFAULT; td.layout=RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName="DLSSPass.EncodedInput";
    m_rrEncodedState=td.layout;
    return gfx.CreateTexture(td,m_rrEncodedInput);
}

// Replaces m_scalePSO only on success, so a failed rebuild keeps the old one.
bool DLSSPass::CreateScalePSO(IGraphicsDevice& gfx)
{
    auto shader=ShaderDiskCache::LoadOrCompile("shaders/RRRadianceScale.cs.hlsl","main",RHI::ShaderStage::CS,{},"RRRadianceScale");
    if(shader.empty()) return false;
    RHI::Shader cs;
    if(!gfx.CreateShader(RHI::ShaderStage::CS,shader.data(),shader.size(),cs)) return false;
    RHI::PipelineStateDesc pd{};
    pd.cs=&cs; pd.layout=&m_scaleLayout;
    const bool ok=gfx.CreatePipelineState(pd,m_scalePSO);   // retires the old pipeline only on success
    gfx.DestroyShader(cs);
    return ok;
}

// Hot reload runs after FlushAndWait, so the old PSO can be released here. Not
// built yet (RR off): the first PrepareRadianceEncoding compiles the new source.
void DLSSPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if(!m_scalePSO.IsValid()) return;
    if(!CreateScalePSO(gfx))
        LOG_ERROR("DLSSPass: RRRadianceScale rebuild after shader reload failed - keeping the old pipeline");
}

void DLSSPass::ScaleRadiance(RHI::CommandList cl,IGraphicsDevice& gfx,uint32_t inputIndex,
    RHI::Texture& output,uint32_t width,uint32_t height,float scale)
{
    struct {uint32_t width,height,inputIndex,outputIndex; float scale;} c{
        width,height,inputIndex,gfx.DescriptorHeapIndexFromGpuHandle(gfx.GetTextureUAVGpuHandle(output)),scale};
    gfx.BindComputePipelineState(m_scalePSO,cl);
    gfx.SetComputeRootConstants(kScaleRootConstants,5,&c,0,cl);
    gfx.DispatchCompute((width+7)/8,(height+7)/8,1,cl);
}

void DLSSPass::OnService(RG::ServiceContext& c)
{
    if (c.stage == RG::ServiceStage::Upscalers)
    {
        if (m_request.upscaler)
        {
            SetRayReconstruction(m_request.rr, m_request.rrModel);
            SetQualityMode(m_request.quality);   // RR too: Native = DLAA
            SetSizes(m_request.renderW, m_request.renderH, m_request.displayW, m_request.displayH);
            // Once per failing streak: the plan's TAA stand-in resolves every frame until the
            // context comes up.
            const bool ok = EnsureContext(*c.gfx);
            if (!ok && !m_contextFailLogged)
                LOG_WARNING("Renderer: DLSS EnsureContext failed — TAA resolves until it comes up");
            m_contextFailLogged = !ok;
        }
        else if (HasResources())
            Shutdown(*c.gfx);
        return;
    }
    if (c.stage == RG::ServiceStage::Retry)
    {
        ServiceRetry();
        return;
    }
    // A ReSTIR history reset requested at this service point - a backend switch, a
    // settings change that rebuilds the working set, RR's own switch, a new pass - resets
    // RR's history too (UploadFrameData follows in the game half). A camera cut already
    // resets it (camera.historyValid). Only a pass that will run: an idle one keeps its
    // flag until it does.
    if (!m_request.restirReset) return;
    const bool di = c.board.diHistoryReset, gi = c.board.giHistoryReset;
    if ((di || gi) && !IsResetPending())
    {
        SetReset(true);
        ++m_restirResets;
        LOG_INFO("[DLSS_RR] restirHistoryReset=1 di=%u gi=%u count=%u", di ? 1u : 0u, gi ? 1u : 0u, m_restirResets);
    }
}
