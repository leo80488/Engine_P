#pragma once

// FidelityFXBackend - thin RAII wrapper around the FFX upscaler context.
//
// FSR2Pass owns one. The backend owns the ffxContext lifetime and the query
// helpers; the pass builds the per-frame dispatch description (FfxApiResource
// needs <d3d12.h>, which stays out of this header).
// Lifecycle: CreateUpscaleContext once (on resize, DestroyUpscaleContext and create
// again), the pass calls ffxDispatch(GetContext(), &desc, nullptr) per frame, the
// destructor releases the context.

#include <cstdint>

struct ID3D12Device;

namespace FFX
{
    class FidelityFXBackend
    {
    public:
        FidelityFXBackend()  = default;
        ~FidelityFXBackend();
        FidelityFXBackend(const FidelityFXBackend&)            = delete;
        FidelityFXBackend& operator=(const FidelityFXBackend&) = delete;

        // Creates an FSR upscale context; returns false if one already exists or creation
        // fails. The max render / upscale sizes are the worst case the context must
        // support; exact frame sizes are fine if the context is recreated on resize.
        // @p autoExposure: FSR meters its own exposure (the context flag); false means every dispatch is
        // given one.
        bool CreateUpscaleContext(ID3D12Device* device,
                                  uint32_t      maxRenderW,
                                  uint32_t      maxRenderH,
                                  uint32_t      maxUpscaleW,
                                  uint32_t      maxUpscaleH,
                                  bool          autoExposure);

        // Release the upscale context. Safe to call multiple times.
        void DestroyUpscaleContext();

        // True iff CreateUpscaleContext succeeded and DestroyUpscaleContext
        // has not yet been called.
        bool IsValid() const { return m_context != nullptr; }

        // Raw ffxContext for ffxDispatch / ffxQuery (`ffxContext` is a void* alias; the
        // caller casts). Null when !IsValid().
        void* GetContext() const { return m_context; }

        // Wraps ffxQuery(FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT).
        // Phase count = ceil(8 * (displayWidth / renderWidth)^2) per the FSR docs.
        // Returns false on backend error / no context.
        bool QueryJitterPhaseCount(uint32_t renderW, uint32_t displayW,
                                   int32_t& outCount) const;

        // Wraps ffxQuery(FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET): the subpixel
        // jitter for @p index in FSR's Halton sequence, in render-resolution pixels
        // (roughly -0.5..0.5). Returns false on backend error / no context.
        bool QueryJitterOffset(int32_t index, int32_t phaseCount,
                               float& outX, float& outY) const;

    private:
        // Opaque ffxContext (void* per the FFX API), kept as void* so this header does not
        // include ffx_api.h (which pulls in d3d12.h through the dx12 helpers).
        void* m_context = nullptr;
    };
} // namespace FFX
