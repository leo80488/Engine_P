#pragma once

#include "Resource/Resource.h"
#include "Resource/ResourceHandle.h"
#include "Resource/IPendingGPUUpload.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>

// Loader plugin interface: registered by file extension; ResourceManager dispatches
// each internal blob to the matching loader.
namespace Resource
{
    struct LoadResult
    {
        std::unique_ptr<Resource>          resource;      // ownership transferred to ResourceManager
        std::unique_ptr<IPendingGPUUpload> pendingUpload; // optional deferred GPU upload
        std::string                        failReason;    // set with a null resource: why (the entry's failReason)
    };

    class IResourceLoader
    {
    public:
        virtual ~IResourceLoader() = default;

        // Parse already-loaded bytes and create a Resource; runs on a background thread.
        // May return a pendingUpload for the main-thread GPU upload. @p data is valid for the call
        // only: a resource keeps a copy of what it needs.
        virtual LoadResult Load(
            const std::string& path,
            std::span<const std::uint8_t> data) = 0;

        // The file extension handled (lowercase, with dot), e.g. ".dds", ".wav".
        virtual const char* GetExtension() const = 0;
    };
}

