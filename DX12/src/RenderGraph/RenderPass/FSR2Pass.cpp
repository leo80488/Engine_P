#include "RenderGraph/RenderPass/FSR2Pass.h"
#include "Graphics/GraphicsDX12.h"
#include "Settings/PassSettings.h"
#include "System/Log.h"

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"

FSR2Pass::~FSR2Pass()
{
    // Call Shutdown(gfx) first: the texture cannot be freed here without a gfx pointer.
    // The backend destroys its context in its own dtor regardless.
}

bool FSR2Pass::Init(IGraphicsDevice& /*gfx*/)
{
    // The output texture + FFX context are created by EnsureContext once the sizes are
    // known; nothing to do here.
    return true;
}

void FSR2Pass::BindView(ViewState& view)
{
    m_hist = &view.History<History>(this);
}

void FSR2Pass::Shutdown(IGraphicsDevice& gfx)
{
    // ffxDestroyContext frees the FFX-owned internal/history GPU textures synchronously.
    // Prior frames' ffxDispatch may still reference them on the GPU (BeginFrame only does
    // a pipelined per-slot wait, not a full drain), so hard-sync first or the GPU reads
    // freed memory -> TDR/DEVICE_REMOVED. This path (leaving FSR, teardown) is rare, so a
    // full flush is acceptable.
    gfx.FlushAndWait();
    m_backend.DestroyUpscaleContext();
    if (m_outputCreated && m_outputTex.IsValid())
    {
        gfx.DestroyTexture(m_outputTex);
        m_outputCreated = false;
    }
    m_lastRenderW = m_lastRenderH = m_lastDisplayW = m_lastDisplayH = 0;
    m_dispatches = m_dispatchesWithExposure = 0;
}

bool FSR2Pass::RebuildForSizes(IGraphicsDevice& gfx)
{
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    ID3D12Device* device = dx12.GetDevice();
    if (!device)
    {
        LOG_ERROR("FSR2Pass: GraphicsDX12::GetDevice returned null");
        return false;
    }

    // Tear down the old context + output. ffxDestroyContext frees FFX GPU resources
    // synchronously, so drain the GPU first: frames still in flight may hold ffxDispatch
    // commands against this context. Resizes / preset changes are rare, so the full
    // flush is acceptable.
    gfx.FlushAndWait();
    m_backend.DestroyUpscaleContext();
    if (m_outputCreated && m_outputTex.IsValid())
    {
        gfx.DestroyTexture(m_outputTex);
        m_outputCreated = false;
    }

    // Recreate output at display res.
    RHI::TextureDesc td{};
    td.width      = m_displayW;
    td.height     = m_displayH;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = "FSR2Pass.Output";
    if (!gfx.CreateTexture(td, m_outputTex))
    {
        LOG_ERROR("FSR2Pass: failed to create output texture");
        return false;
    }
    m_outputCreated = true;
    m_outputState   = RHI::ResourceState::UNORDERED_ACCESS;

    m_engineExposure = Settings::Pass::upscalerEngineExposure();
    if (!m_backend.CreateUpscaleContext(device, m_renderW, m_renderH,
                                         m_displayW, m_displayH, !m_engineExposure))
        return false;

    m_lastRenderW  = m_renderW;
    m_lastRenderH  = m_renderH;
    m_lastDisplayW = m_displayW;
    m_lastDisplayH = m_displayH;
    SetReset(true);  // history is fresh after recreation
    return true;
}

void FSR2Pass::SetSrcTexturesComputeRead(const RHI::Texture* color,
                                         const RHI::Texture* depth,
                                         const RHI::Texture* motion)
{
    m_colorTex        = color;
    m_colorIsHdrScene = (color == nullptr);
    m_depthTex        = depth;
    m_motionTex       = motion;
}

void FSR2Pass::OnService(RG::ServiceContext& c)
{
    if (m_request.wanted)
    {
        SetSizes(m_request.renderW, m_request.renderH, m_request.displayW, m_request.displayH);
        // Once per failing streak: the plan's TAA stand-in resolves every frame until the
        // context comes up.
        const bool ok = EnsureContext(*c.gfx);
        if (!ok && !m_contextFailLogged)
            LOG_WARNING("Renderer: FSR2 EnsureContext failed — TAA resolves until it comes up");
        m_contextFailLogged = !ok;
    }
    else if (IsContextValid())
        Shutdown(*c.gfx);
}

bool FSR2Pass::EnsureContext(IGraphicsDevice& gfx)
{
    if (failContextForTest) return false;
    if (m_renderW == 0 || m_renderH == 0 || m_displayW == 0 || m_displayH == 0)
        return false;
    const bool sizeChanged = (m_renderW  != m_lastRenderW)  ||
                              (m_renderH  != m_lastRenderH)  ||
                              (m_displayW != m_lastDisplayW) ||
                              (m_displayH != m_lastDisplayH);
    if (m_backend.IsValid() && m_outputCreated && !sizeChanged &&
        m_engineExposure == Settings::Pass::upscalerEngineExposure())
        return true;
    return RebuildForSizes(gfx);
}

bool FSR2Pass::QueryJitter(uint32_t frameIndex, float& outX, float& outY)
{
    if (!m_backend.IsValid()) return false;
    if (m_renderW == 0 || m_displayW == 0) return false;
    int32_t phaseCount = 0;
    if (!m_backend.QueryJitterPhaseCount(m_renderW, m_displayW, phaseCount) ||
        phaseCount <= 0)
        return false;
    int32_t idx = static_cast<int32_t>(frameIndex % static_cast<uint32_t>(phaseCount));
    return m_backend.QueryJitterOffset(idx, phaseCount, outX, outY);
}

void FSR2Pass::Execute(RHI::CommandList cl, IGraphicsDevice& gfx)
{
    if (m_renderW == 0 || m_renderH == 0 || m_displayW == 0 || m_displayH == 0)
        return;
    // The Renderer hands RHI textures; resolve them to the resources FFX takes, all
    // compute-read.
    auto& dx12 = static_cast<GraphicsDX12&>(gfx);
    auto native = [&dx12](const RHI::Texture* t) { return (t && t->IsValid()) ? dx12.GetTextureResource(*t) : nullptr; };
    SetSrcResources(m_colorIsHdrScene ? dx12.GetHdrSceneResource() : native(m_colorTex),
                    FFX_API_RESOURCE_STATE_COMPUTE_READ,
                    native(m_depthTex),  FFX_API_RESOURCE_STATE_COMPUTE_READ,
                    native(m_motionTex), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    m_reactiveRes = native(m_reactiveTex);
    if (!m_colorRes || !m_depthRes || !m_motionRes)
    {
        LOG_WARNING("FSR2Pass: missing input resource(s)");
        return;
    }

    // EnsureContext is the caller's job (main thread); bail if it has not succeeded -
    // the very first frame, before the sizes are set.
    if (!m_backend.IsValid() || !m_outputCreated)
    {
        LOG_WARNING("FSR2Pass: Execute called without EnsureContext — skipping");
        return;
    }

    // Transition our output to UAV (FSR writes via UAV).
    if (m_outputState != RHI::ResourceState::UNORDERED_ACCESS)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            &m_outputTex, m_outputState,
            RHI::ResourceState::UNORDERED_ACCESS), cl);
        m_outputState = RHI::ResourceState::UNORDERED_ACCESS;
    }
    // PushBarrier records the transition into @p cl at once (the engine does not batch).
    // Grab the native CL for the FFX dispatch.
    ID3D12GraphicsCommandList* nativeCL = dx12.GetNativeCommandList(cl);
    if (!nativeCL)
    {
        LOG_ERROR("FSR2Pass: native command list is null");
        return;
    }

    // Build FfxApiResource for each input/output.
    FfxApiResource color  = ffxApiGetResourceDX12(m_colorRes,  m_colorStateFfx);
    FfxApiResource depth  = ffxApiGetResourceDX12(m_depthRes,  m_depthStateFfx);
    FfxApiResource motion = ffxApiGetResourceDX12(m_motionRes, m_motionStateFfx);
    FfxApiResource output = ffxApiGetResourceDX12(
        dx12.GetTextureResource(m_outputTex),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS,
        FFX_API_RESOURCE_USAGE_UAV);

    ffxDispatchDescUpscale d{};
    d.header.type           = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList           = nativeCL;
    d.color                 = color;
    d.depth                 = depth;
    d.motionVectors         = motion;
    // The engine's exposure when the context was made for it (else the AUTO_EXPOSURE flag meters).
    const bool haveExposure = m_engineExposure && m_exposureTex && m_exposureTex->IsValid();
    d.exposure              = haveExposure
        ? ffxApiGetResourceDX12(dx12.GetTextureResource(*m_exposureTex), FFX_API_RESOURCE_STATE_COMPUTE_READ)
        : FfxApiResource{};
    // The stencil-derived reactive mask (particles that opted out of TAA, the outline
    // rim) when the renderer made one this frame. The transparency-and-composition mask
    // stays unused: forward transparency writes no coverage the engine could hand over.
    d.reactive              = m_reactiveRes
        ? ffxApiGetResourceDX12(m_reactiveRes, FFX_API_RESOURCE_STATE_COMPUTE_READ)
        : FfxApiResource{};
    d.transparencyAndComposition = FfxApiResource{};          // optional, unused
    d.output                = output;
    d.jitterOffset          = { m_jitterX, m_jitterY };
    // Engine GBuffer outputs currentNDC - previousNDC, while FSR reprojects
    // previousUV = currentUV + motion. Convert to previousUV - currentUV with
    // scale (-renderW/2, +renderH/2).
    d.motionVectorScale     = { m_motionScaleX, m_motionScaleY };
    d.renderSize            = { m_renderW,  m_renderH  };
    d.upscaleSize           = { m_displayW, m_displayH };
    d.enableSharpening      = sharpeningEnabled;
    d.sharpness             = std::clamp(sharpness, 0.0f, 1.0f);
    d.frameTimeDelta        = m_frameTimeMs;
    d.preExposure           = 1.0f;
    d.reset                 = !m_hist || m_hist->reset;
    d.cameraNear            = m_cameraNear;
    d.cameraFar             = m_cameraFar;
    d.cameraFovAngleVertical = m_cameraFovY;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags                 = 0;

    ffxContext ctx = static_cast<ffxContext>(m_backend.GetContext());
    ffxReturnCode_t rc = ffxDispatch(&ctx, &d.header);
    if (rc != FFX_API_RETURN_OK)
        LOG_WARNING("FSR2Pass: ffxDispatch rc=%u", rc);
    ++m_dispatches;
    if (haveExposure) ++m_dispatchesWithExposure;
    if (m_dispatches == 60)
        LOG_INFO("[UPSCALER_EXPOSURE] upscaler=fsr2 dispatches=%u withExposure=%u engine=%d", m_dispatches,
                 m_dispatchesWithExposure, m_engineExposure ? 1 : 0);

    SetReset(false);  // history is now valid for next frame

    // FFX swaps descriptor heaps internally and does not restore them.
    // Re-bind the engine's shader-visible heaps so subsequent passes that
    // skip SetDescriptorHeaps (most do) still see the right heap.
    dx12.RebindShaderVisibleHeaps(cl);
    // FFX also leaves its own 3-parameter compute root signature bound, and AutoExposure
    // sets the shared layout's t5 before binding a pipeline: with no CAS / DoF in between
    // (each binds a PSO) that writes root parameter 6 of FFX's signature - a debug-layer
    // error, then a driver access violation. Restore whatever the return code (as DLSS
    // does after its evaluate): FFX may have bound its layout before failing.
    if (restoreEngineLayout)
        dx12.RestoreDefaultComputeRootSignature(cl);

    // Transition the output UAV -> SHADER_RESOURCE_COMPUTE so downstream Bloom/Tonemap
    // sample it without further barriers.
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        &m_outputTex,
        RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE_COMPUTE), cl);
    m_outputState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
}

uint64_t FSR2Pass::GetOutputSrvGpuHandle(const IGraphicsDevice& gfx) const
{
    if (!m_outputCreated || !m_outputTex.IsValid()) return 0;
    return gfx.GetTextureSRVGpuHandle(m_outputTex);
}
