#pragma once

// BCCompressor: process-wide singleton owning a dedicated D3D11 device for DirectXTex's GPU
// BC6H / BC7 compression (DirectXTex implements it with D3D11 compute shaders); a separate device
// leaves the engine's D3D12 device and descriptor heaps untouched. Typical cost per 1024x1024:
// GPU ~50-80 ms, CPU codec ~2-15 s. BC1-BC5 always use DirectXTex's CPU codec.
// Thread safety: D3D11 calls are serialized by an internal mutex and the CPU codec has no shared
// mutable state, so concurrent ResourceManager workers may call Compress.

#include <d3d11.h>
#include <DirectXTex.h>
#include <wrl/client.h>
#include <mutex>

namespace Resource
{
    class BCCompressor
    {
    public:
        // Returns the process-wide singleton.
        static BCCompressor& Get();

        // BC6H / BC7 use the GPU when available and fall back to the CPU codec; BC1-BC5 go straight to
        // the parallel CPU codec (DirectXTex's D3D11 overload does not implement them).
        HRESULT Compress(const DirectX::Image*        srcImages,
                         size_t                        nimages,
                         const DirectX::TexMetadata&   metadata,
                         DXGI_FORMAT                   format,
                         DirectX::ScratchImage&        outImage);

        bool IsGPUAvailable() const { return m_device != nullptr; }

        // Release the D3D11 device; optional before process exit.
        void Shutdown();

    private:
        BCCompressor();
        ~BCCompressor() = default;
        BCCompressor(const BCCompressor&)            = delete;
        BCCompressor& operator=(const BCCompressor&) = delete;

        void InitDevice();

        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        std::mutex                           m_mutex;
    };
}
