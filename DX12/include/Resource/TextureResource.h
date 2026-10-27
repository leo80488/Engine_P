#pragma once

#include "Resource/Resource.h"
#include <DirectXTex.h>
#include <cstddef>

namespace Resource
{
    // Texture resource: the CPU-side image (a DirectXTex ScratchImage + its metadata) decoded from a
    // .itex by TextureLoader.
    class TextureResource : public Resource
    {
    public:
        TextureResource() = default;
        TextureResource(DirectX::TexMetadata metadata, DirectX::ScratchImage&& image)
            : m_metadata(metadata), m_image(std::move(image)) {}

        const DirectX::TexMetadata& GetMetadata() const { return m_metadata; }
        const DirectX::ScratchImage& GetImage() const { return m_image; }
        DirectX::ScratchImage& GetImage() { return m_image; }

        size_t GetWidth() const { return m_metadata.width; }
        size_t GetHeight() const { return m_metadata.height; }
        DXGI_FORMAT GetFormat() const { return m_metadata.format; }
        bool IsCubemap() const { return m_metadata.IsCubemap(); }

    private:
        DirectX::TexMetadata m_metadata{};
        DirectX::ScratchImage m_image;
    };
}
