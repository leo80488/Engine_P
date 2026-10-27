#include "Resource/BCCompressor.h"
#include "System/Log.h"

#pragma comment(lib, "d3d11.lib")

namespace Resource
{
    namespace
    {
        bool SupportsDirectComputeCompression(DXGI_FORMAT format)
        {
            // DirectXTex's DirectCompute codec implements only BC6H and BC7 (texconv routes the same way).
            switch (format)
            {
            case DXGI_FORMAT_BC6H_TYPELESS:
            case DXGI_FORMAT_BC6H_UF16:
            case DXGI_FORMAT_BC6H_SF16:
            case DXGI_FORMAT_BC7_TYPELESS:
            case DXGI_FORMAT_BC7_UNORM:
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return true;
            default:
                return false;
            }
        }
    }

    BCCompressor& BCCompressor::Get()
    {
        static BCCompressor s_instance; // constructed once, thread-safe (C++11)
        return s_instance;
    }

    BCCompressor::BCCompressor()
    {
        InitDevice();
    }

    void BCCompressor::InitDevice()
    {
        // Hardware device first, WARP if that fails.
        const D3D_DRIVER_TYPE driverTypes[] =
        {
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,   // software fallback still faster than CPU BC7
        };

        // BC6H / BC7 require feature level 11.0.
        const D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        for (D3D_DRIVER_TYPE dt : driverTypes)
        {
            D3D_FEATURE_LEVEL actualLevel{};
            HRESULT hr = D3D11CreateDevice(
                nullptr,                   // default adapter
                dt,
                nullptr,                   // no software module
                0,                         // no debug / other flags
                featureLevels,
                static_cast<UINT>(std::size(featureLevels)),
                D3D11_SDK_VERSION,
                m_device.GetAddressOf(),
                &actualLevel,
                nullptr                    // immediate context not needed
            );

            if (SUCCEEDED(hr))
            {
                const char* dtName = (dt == D3D_DRIVER_TYPE_HARDWARE) ? "hardware" : "WARP";
                LOG_SUCCESS("BCCompressor: D3D11 %s device ready (FL 0x%X) for GPU BC6H/BC7 compression",
                            dtName, static_cast<unsigned>(actualLevel));
                return;
            }
        }

        LOG_WARNING("BCCompressor: D3D11 device creation failed — BC compression will use CPU fallback");
        m_device = nullptr;
    }

    HRESULT BCCompressor::Compress(const DirectX::Image*       srcImages,
                                    size_t                       nimages,
                                    const DirectX::TexMetadata&  metadata,
                                    DXGI_FORMAT                  format,
                                    DirectX::ScratchImage&       outImage)
    {
        // GPU path: BC6H / BC7 only; D3D11 calls are serialized by m_mutex.
        if (m_device && SupportsDirectComputeCompression(format))
        {
            std::lock_guard<std::mutex> lock(m_mutex);

            HRESULT hr = DirectX::Compress(
                m_device.Get(),
                srcImages, nimages, metadata,
                format,
                DirectX::TEX_COMPRESS_DEFAULT,
                DirectX::TEX_ALPHA_WEIGHT_DEFAULT,
                outImage);

            if (SUCCEEDED(hr))
                return hr;

            LOG_WARNING("BCCompressor: GPU Compress failed (fmt=0x%X hr=0x%08X), falling back to CPU",
                        static_cast<unsigned>(format),
                        static_cast<unsigned>(hr));
        }

        // CPU codec: BC1-BC5 always, BC6H / BC7 as the fallback.
        DirectX::TEX_COMPRESS_FLAGS cpuFlags = DirectX::TEX_COMPRESS_PARALLEL;
        if (format == DXGI_FORMAT_BC7_UNORM ||
            format == DXGI_FORMAT_BC7_UNORM_SRGB)
            cpuFlags |= DirectX::TEX_COMPRESS_BC7_QUICK;

        return DirectX::Compress(
            srcImages, nimages, metadata,
            format, cpuFlags,
            DirectX::TEX_THRESHOLD_DEFAULT,
            outImage);
    }

    void BCCompressor::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_device.Reset();
        LOG_INFO("BCCompressor: shutdown");
    }
}
