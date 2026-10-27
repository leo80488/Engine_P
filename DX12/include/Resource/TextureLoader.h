#pragma once

#include "Resource/IResourceLoader.h"
#include "Resource/TextureResource.h"
#include <memory>

namespace Resource
{
    // Loads the internal .itex format produced by TextureImporter. External formats (PNG, JPG, TGA)
    // go through TextureImporter first.
    //
    // Registration:
    //   resourceManager.RegisterLoader(std::make_shared<TextureLoader>());
    class TextureLoader : public IResourceLoader
    {
    public:
        TextureLoader() = default;

        LoadResult  Load(const std::string& path, std::span<const std::uint8_t> data) override;
        const char* GetExtension() const override { return ".itex"; }
    };
}
