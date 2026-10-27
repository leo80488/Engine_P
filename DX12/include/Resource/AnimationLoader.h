#pragma once

// AnimationLoader - loads .ianim blobs into AnimationResource objects; registered
// with ResourceManager for ".ianim". No GPU upload: the clip data is CPU-only.

#include "Resource/IResourceLoader.h"

namespace Resource
{
    class AnimationLoader : public IResourceLoader
    {
    public:
        LoadResult Load(const std::string& path,
                        std::span<const std::uint8_t> data) override;

        const char* GetExtension() const override { return ".ianim"; }
    };
}
