#include "Graphics/FidelityFXBackend.h"
#include "System/Log.h"

#include "ffx_api.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace FFX
{

FidelityFXBackend::~FidelityFXBackend()
{
    DestroyUpscaleContext();
}

bool FidelityFXBackend::CreateUpscaleContext(ID3D12Device* device,
                                              uint32_t      maxRenderW,
                                              uint32_t      maxRenderH,
                                              uint32_t      maxUpscaleW,
                                              uint32_t      maxUpscaleH,
                                              bool          autoExposure)
{
    if (m_context)
    {
        LOG_WARNING("FidelityFXBackend: context already created — destroy first");
        return false;
    }
    if (!device)
    {
        LOG_ERROR("FidelityFXBackend: null device");
        return false;
    }

    ffxCreateBackendDX12Desc backendDesc{};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backendDesc.device      = device;

    ffxCreateContextDescUpscale upscaleDesc{};
    upscaleDesc.header.type   = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscaleDesc.header.pNext  = &backendDesc.header;
    // Input is HDR linear scene color (R16G16B16A16_FLOAT) and reversed-Z depth
    // (near=1, far=0) with a finite far plane (no INFINITE flag). AUTO_EXPOSURE: FSR
    // computes its own exposure from the HDR input; without it every dispatch carries the
    // engine's (AutoExposurePass runs ahead of the upscaler, so it is this frame's).
    upscaleDesc.flags         = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                              | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
                              | (autoExposure ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0);
    upscaleDesc.maxRenderSize  = { maxRenderW,  maxRenderH  };
    upscaleDesc.maxUpscaleSize = { maxUpscaleW, maxUpscaleH };
    upscaleDesc.fpMessage      = nullptr;  // no message callback: FFX's own diagnostics are not logged

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = ffxCreateContext(&ctx, &upscaleDesc.header, nullptr);
    if (rc != FFX_API_RETURN_OK || !ctx)
    {
        LOG_ERROR("FidelityFXBackend: ffxCreateContext failed (rc=%u)", rc);
        return false;
    }
    m_context = ctx;
    LOG_INFO("FidelityFXBackend: upscale context created (%ux%u render → %ux%u display)",
             maxRenderW, maxRenderH, maxUpscaleW, maxUpscaleH);
    LOG_INFO("[UPSCALER_EXPOSURE] upscaler=fsr2 engine=%d", autoExposure ? 0 : 1);
    return true;
}

void FidelityFXBackend::DestroyUpscaleContext()
{
    if (!m_context) return;
    ffxContext ctx = static_cast<ffxContext>(m_context);
    ffxReturnCode_t rc = ffxDestroyContext(&ctx, nullptr);
    if (rc != FFX_API_RETURN_OK)
        LOG_WARNING("FidelityFXBackend: ffxDestroyContext rc=%u", rc);
    m_context = nullptr;
}

bool FidelityFXBackend::QueryJitterPhaseCount(uint32_t renderW, uint32_t displayW,
                                              int32_t& outCount) const
{
    if (!m_context) return false;
    int32_t count = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type     = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth     = renderW;
    q.displayWidth    = displayW;
    q.pOutPhaseCount  = &count;
    ffxContext ctx = static_cast<ffxContext>(m_context);
    ffxReturnCode_t rc = ffxQuery(&ctx, &q.header);
    if (rc != FFX_API_RETURN_OK)
    {
        LOG_WARNING("FidelityFXBackend: QueryJitterPhaseCount rc=%u", rc);
        return false;
    }
    outCount = count;
    return true;
}

bool FidelityFXBackend::QueryJitterOffset(int32_t index, int32_t phaseCount,
                                          float& outX, float& outY) const
{
    if (!m_context) return false;
    ffxQueryDescUpscaleGetJitterOffset q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    q.index       = index;
    q.phaseCount  = phaseCount;
    q.pOutX       = &outX;
    q.pOutY       = &outY;
    ffxContext ctx = static_cast<ffxContext>(m_context);
    ffxReturnCode_t rc = ffxQuery(&ctx, &q.header);
    if (rc != FFX_API_RETURN_OK)
    {
        LOG_WARNING("FidelityFXBackend: QueryJitterOffset rc=%u", rc);
        return false;
    }
    return true;
}

} // namespace FFX
