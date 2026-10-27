#pragma once

// AudioClipLoader - turns an .aclip blob into an AudioClipResource on a background worker. CPU-only
// (no GPU upload), so LoadResult carries no IPendingGPUUpload; the same shape as AnimationLoader.

#include "Resource/IResourceLoader.h"

namespace Audio
{
    class AudioClipLoader : public Resource::IResourceLoader
    {
    public:
        Resource::LoadResult Load(const std::string& path,
                                  std::span<const std::uint8_t> data) override;

        const char* GetExtension() const override { return ".aclip"; }
    };
}
