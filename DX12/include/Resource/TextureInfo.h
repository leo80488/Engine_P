#pragma once

// TextureInfo - what a .itex file holds, read from the file alone: no device, no upload, no
// reference taken.
//
// The source of truth is the DDS payload's header, not the TextureMetadata that follows the
// AssetHeader: TextureLoader never reads that struct, and its `dimension` is written three ways on
// disk (3 = DirectXTex's TEXTURE2D even for a cube, 6 = the probe bake's "Cube", 2 = a few Python
// tools). When the two disagree the difference is reported (headerMismatch), never trusted.
//
// Consumers: the Content Browser's tooltip and Details columns, the Texture Viewer's info panel,
// Lua Asset.textureInfo (both exes).

#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    struct TextureFileInfo
    {
        struct Mip
        {
            uint32_t width = 0, height = 0, depth = 1;
            uint64_t rowPitch = 0;    // bytes per row (per block row for BCn)
            uint64_t slicePitch = 0;  // bytes of one 2D slice of this mip
            uint64_t bytes = 0;       // slicePitch * depth: one array slice / cube face
        };

        bool        ok = false;
        std::string error;            // why !ok: "missing", "not a .itex", "bad DDS header", ...

        std::string path;             // canonical: asset-relative, '/' separators
        std::string guid;             // the .meta sidecar's guid (hex), empty when none
        uint64_t    fileBytes = 0;
        bool        packed    = false;   // read from the mounted .ipak

        // ---- The DDS header ----
        uint32_t    width = 0, height = 0, depth = 1;
        uint32_t    arraySize = 1;    // slices; a cube counts 6 per cube (DirectXTex's convention)
        uint32_t    mipLevels = 1;
        std::string dimension;        // "1D" | "2D" | "3D" | "Cube"
        bool        isCube    = false;
        uint32_t    cubeCount = 0;    // arraySize / 6 for a cube, else 0

        uint32_t    dxgiFormat = 0;
        std::string formatName;       // "BC7_UNORM_SRGB"
        std::string formatLabel;      // "BC7 sRGB"
        bool        isSRGB       = false;
        bool        isCompressed = false;   // block-compressed (BCn)
        uint32_t    bitsPerPixel = 0;       // BC7 8, BC1 4, RGBA16F 64
        uint32_t    blockSize    = 1;       // texels per block edge: 4 for BCn, else 1
        std::string alphaMode;        // the DDS alpha mode: unknown | straight | premultiplied | opaque | custom

        std::vector<Mip> mips;
        uint64_t    dataBytes = 0;    // every mip of every slice: the pixel payload

        // The TextureImporter preset this format comes from ("BC7_SRGB",
        // "BC5_Linear", ...; TextureImporter::GetFormatId), empty when the
        // format is none of the ten (an older import, a baked probe, a tool).
        std::string preset;

        // ---- The AssetHeader's TextureMetadata, for the diagnostic only ----
        uint32_t    headerDimension = 0;
        std::string headerMismatch;   // non-empty when it disagrees with the DDS header
    };

    // Reads @p path's AssetHeader and DDS header (a few hundred bytes of a
    // loose file; the entry of a mounted .ipak). Cached per path and validated
    // by (size, mtime), so a re-import is seen at once. Thread-safe.
    TextureFileInfo DescribeTextureFile(const std::string& path);

    // One UTF-8 line, fields joined by a middle dot (U+00B7): "2048x2048", "BC7 sRGB", "12 mips",
    // "5.3 MB" (a cube says "Cube 128x128", an array "x4"). Empty for a failed info.
    std::string TextureInfoSummary(const TextureFileInfo& info);

    // Bytes as the editor shows them: "812 B", "5.3 KB", "5.3 MB", "1.2 GB".
    std::string FormatByteSize(uint64_t bytes);
}
