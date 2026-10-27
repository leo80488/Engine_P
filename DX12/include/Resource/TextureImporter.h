#pragma once

#include "Resource/IImporter.h"
#include <cstdint>

namespace Resource
{
    // Explicit output choices exposed by the editor's pre-import review window.
    // Keep Count last: the ImGui combo iterates [0, Count).
    enum class TextureImportFormat : uint8_t
    {
        BC7_SRGB,
        BC7_Linear,
        BC5_Linear,
        BC4_Linear,
        BC6H_UF16,
        R16_UNorm,
        RG16_Float,
        RGBA8_Linear,
        RGBA16_UNorm,
        RGBA16_Float,
        Count
    };

    // How the mip chain (and a maxSize resize) is filtered. Default is DirectXTex's own choice.
    enum class TextureMipFilter : uint8_t
    {
        Default,
        Box,
        Linear,
        Cubic,
        Triangle,
        Point,
        Count
    };

    struct TextureImportSettings
    {
        TextureImportFormat format = TextureImportFormat::BC7_Linear;
        // The rest of what an import decides; at their defaults an import keeps the source's size,
        // channels and DirectXTex's filter. The record of them lives in the .itex's .meta
        // (Resource/TextureImportRecord.h).
        uint32_t         maxSize      = 0;       // the longest edge after import; 0 = the source's
        TextureMipFilter mipFilter    = TextureMipFilter::Default;   // the chain, and a maxSize resize
        bool             flipGreen    = false;   // G = 1 - G: a normal map from the other Y convention
        bool             generateMips = true;    // false: a source without mips stays one mip
    };

    // Converts external image formats (PNG, JPG, TGA, BMP, HDR) to the internal .itex format.
    //
    // .itex blob layout:
    //   [AssetHeader (24 B)] [TextureMetadata] [DDS bytes]
    //
    // The DDS payload comes from DirectX::SaveToDDSMemory, so it is GPU-ready as stored; TextureLoader
    // reads it with DirectX::LoadFromDDSMemory.
    class TextureImporter : public IImporter
    {
    public:
        // IImporter entry point used by ResourceManager: non-interactive loads pick the format from the
        // filename and the source format.
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        // Editor entry point: the selected format is authoritative. This is
        // called only after the user confirms the pre-import review window.
        std::vector<uint8_t> ImportWithSettings(
            const std::string& sourcePath,
            const std::vector<uint8_t>& sourceData,
            const TextureImportSettings& settings);

        // Fast filename/extension-only classification for populating the
        // review window before any source image is decoded or compressed.
        static TextureImportFormat SuggestFormat(const std::string& sourcePath);
        // The names below are defined in TextureImportNames.cpp (EngineCore, both
        // exes: the import record and Asset.textureInfo spell them); the rest of
        // the importer is EditorCore's.
        static const char* GetFormatName(TextureImportFormat format);
        static const char* GetFormatDescription(TextureImportFormat format);
        // The enumerator's own spelling ("BC7_SRGB", "RGBA8_Linear", ...): what
        // scripts pass and what a job reports (Editor.Import).
        static const char* GetFormatId(TextureImportFormat format);
        static bool        ParseFormatId(const std::string& id, TextureImportFormat& out);
        // "default" | "box" | "linear" | "cubic" | "triangle" | "point".
        static const char* GetMipFilterId(TextureMipFilter filter);
        static bool        ParseMipFilterId(const std::string& id, TextureMipFilter& out);

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr" };
        }

        const char* GetInternalExtension() const override { return ".itex"; }

    private:
        std::vector<uint8_t> ImportInternal(
            const std::string& sourcePath,
            const std::vector<uint8_t>& sourceData,
            TextureImportSettings settings,
            bool allowFloatSourcePromotion);
    };
}
