#pragma once

// TextureAnalysis - what a .itex's texels are, measured on the CPU from the file: the Texture
// Viewer's statistics, histogram and stored-value read-out. Synchronous, so the viewer runs it on a
// worker (a 4K BC7 mip decodes in about a second).
//
// Values are the stored ones. An sRGB texture is measured as its stored (gamma) bytes / 255 (the
// format is reinterpreted as its linear twin first), because that is what the Stored mapping shows
// and what an artist means by "this pixel is 128". BC formats are decompressed by DirectXTex; BC1-3
// may differ from the GPU's decode by 1 LSB (the D3D spec's tolerance), BC4 / BC5 / BC7 are exact.
//
// EngineCore, no device: Lua and tools can use it as well as the editor.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    struct TextureStats
    {
        bool        ok = false;
        std::string error;
        std::uint32_t mip = 0, slice = 0;
        std::uint32_t width = 0, height = 0, depth = 1;
        std::uint64_t texels = 0;
        std::uint32_t channels = 4;             // channels the format stores (R8 = 1, BC5 = 2, ...)
        bool        hasAlpha = false;

        // Per channel R, G, B, A, over the finite values.
        std::array<double, 4>        min{}, max{}, mean{}, stddev{};
        std::array<std::uint64_t, 4> nan{}, inf{};
        // 256 bins per channel over [histLo, histHi] (per channel; [0,1] for
        // UNORM, [-1,1] for SNORM, the finite range for float formats).
        std::array<std::array<std::uint32_t, 256>, 4> histogram{};
        std::array<double, 4>        histLo{}, histHi{};

        // "opaque" (every A = 1), "binary" (only 0 or 1, +-1/255), "translucent",
        // or "none" (the format stores no alpha).
        std::string alpha;
        bool        monochrome = false;         // every texel the same colour (a placeholder?)

        // A tangent-space normal map read from RG (the Normal mapping's
        // reconstruction): the share of texels whose RG lies outside the unit
        // disc (Z cannot be rebuilt), and for 3-channel maps the mean | |n| - 1 |
        // and the share with z < 0.
        bool        normalChecked = false;
        double      normalOutsideDisc = 0.0;    // fraction
        double      normalLengthDev = 0.0;      // mean | |n| - 1 | (RGB maps)
        double      normalZNegative = 0.0;      // fraction (RGB maps)

        // The mip chain: each mip's mean against mip 0's (per channel); a broken
        // mip generator drifts. Filled only when asked (every mip is decoded).
        std::vector<std::array<double, 4>> mipMeans;
        double      mipMaxDrift = 0.0;

        double      ms = 0.0;                   // decode + measure time
    };

    struct AnalyzeOptions
    {
        std::uint32_t mip = 0;
        std::uint32_t slice = 0;                // array slice / cube face / 3D slice (all 3D slices when ~0u)
        bool          normalCheck = false;
        bool          mipChain = false;
    };
    TextureStats AnalyzeTexture(const std::string& path, const AnalyzeOptions& o);

    // One stored texel: its value as the file stores it (see above) and the raw
    // bytes - the texel's, or for a block-compressed format its 4x4 block's -
    // as hex, with the block's coordinates.
    struct StoredTexel
    {
        bool        ok = false;
        std::string error;
        std::array<float, 4> value{};
        std::string raw;                        // hex bytes
        bool        block = false;              // raw is a BC block
        std::uint32_t blockX = 0, blockY = 0;
    };
    StoredTexel ReadStoredTexel(const std::string& path, std::uint32_t mip, std::uint32_t slice,
                                std::uint32_t x, std::uint32_t y);

    // The decoded files ReadStoredTexel / AnalyzeTexture keep (the as-stored
    // ScratchImages, keyed by path, validated by size + mtime; least recently used
    // out past the byte cap). Thread-safe.
    void          SetTextureAnalysisCacheBytes(std::uint64_t cap);   // default 256 MB
    std::uint64_t TextureAnalysisCacheBytes();
    void          ClearTextureAnalysisCache();
}
