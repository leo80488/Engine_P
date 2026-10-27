#pragma once

#include "Resource/IImporter.h"

namespace Resource
{
    // Compiles .hlsl source (DXC, entry "main", SM 6.6) to the internal .ishdr format:
    //   [AssetHeader (24 B)] [ShaderMetadata] [DXIL container]
    // ShaderLoader only reads the DXIL back; nothing compiles at load time.
    // The stage comes from the filename suffix: .vs/.ps/.cs/.gs/.hs/.ds.hlsl -> VS/PS/CS/GS/HS/DS
    // (anything else -> PS).
    class ShaderImporter : public IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override { return { ".hlsl" }; }
        const char* GetInternalExtension() const override { return ".ishdr"; }
    };
}
