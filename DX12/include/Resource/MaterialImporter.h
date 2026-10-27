#pragma once

#include "Resource/IImporter.h"

namespace Resource
{
    // Converts .mat key=value text files to the internal .imat format.
    //
    // .imat blob layout:
    //   [AssetHeader (24 B)] [MaterialMetadata] [raw .mat text bytes (null-terminated)]
    //
    // The .mat text is copied verbatim: one `key = value` per line, `#` starts a
    // comment, and the keys are the ones Resource::LoadMaterial reads (e.g.
    // shaderType, blendMode, tex_<slot>).
    class MaterialImporter : public IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override { return { ".mat" }; }
        const char* GetInternalExtension() const override { return ".imat"; }
    };
}
