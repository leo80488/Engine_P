#pragma once

#include <string>
#include <vector>

// Importer plugin interface: one-time CPU-side conversion from an external source
// format to the engine's internal binary format.
//   IImporter       - raw source bytes -> internal blob (AssetHeader + Metadata +
//                     Payload). No GPU calls.
//   IResourceLoader - internal blob -> Resource object; may queue a GPU upload
//                     (IPendingGPUUpload).
// ResourceManager drives it: the source extension finds the importer, the cache
// path is the same directory with the internal extension, an existing cache is
// loaded directly, otherwise the source is read, Import()ed, the cache written,
// and the blob loaded.
namespace Resource
{
    class IImporter
    {
    public:
        virtual ~IImporter() = default;

        // Convert raw source bytes to an internal-format blob. An empty vector means
        // failure; ResourceManager marks the entry Failed.
        virtual std::vector<uint8_t> Import(const std::string& sourcePath,
                                             const std::vector<uint8_t>& sourceData) = 0;

        // Source extensions this importer handles (lowercase with dot).
        // Multiple extensions may share one importer instance (e.g. ".jpg", ".jpeg").
        virtual std::vector<const char*> GetSourceExtensions() const = 0;

        // The single internal extension this importer produces (".itex", ".ishdr", etc.).
        virtual const char* GetInternalExtension() const = 0;
    };
}
