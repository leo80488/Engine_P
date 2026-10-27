#include "Resource/TextureLoader.h"
#include "Resource/AssetHeader.h"
#include "Resource/ItexDecode.h"      // DecodeItexBlob, shared with TextureAnalysis
#include "System/Log.h"
#include <DirectXTex.h>

namespace Resource
{
    LoadResult TextureLoader::Load(const std::string& path, std::span<const std::uint8_t> data)
    {
        if (data.empty())
        {
            LOG_ERROR("TextureLoader: empty data for '%s'", path.c_str());
            return {};
        }

        // The AssetHeader (magic, version, size) and the DDS payload - the one
        // parse the Texture Viewer's analysis shares (Resource/ItexDecode.h).
        DirectX::TexMetadata  metadata{};
        DirectX::ScratchImage image;
        std::string           error;
        if (!DecodeItexBlob(data.data(), data.size(), metadata, image, error))
        {
            LOG_ERROR("TextureLoader: %s in '%s'", error.c_str(), path.c_str());
            LoadResult refused;
            refused.failReason = std::move(error);
            return refused;
        }

        auto resource = std::make_unique<TextureResource>(metadata, std::move(image));
        return { std::move(resource), nullptr };
    }
}
