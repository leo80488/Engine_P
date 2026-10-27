#pragma once

// HeightField - CPU-side heightmap grid for terrain collision.
//
// Mirrors the GPU heightmap (R16_UNORM Texture2D) as a row-major uint16 buffer.
// Sampling returns a world-space Y from the caller-supplied (baseY, heightScale);
// world placement and the XZ -> UV remap are the caller's job.
//
// A CPU copy rather than a GPU readback: collision queries run on the main thread
// and a staging round-trip would add a frame of latency, while the data changes
// only with the asset, so the duplicate memory is paid once at load.
// Memory: width x height x 2 bytes (a 4K x 4K heightmap is 32 MB).

#include <cstdint>
#include <vector>
#include <algorithm>

namespace Resource
{
    struct HeightField
    {
        // ---- Grid data ----
        uint32_t              width  = 0;       // texel count along U
        uint32_t              height = 0;       // texel count along V
        std::vector<uint16_t> samples;          // row-major; size == width * height

        // ---- World-Y mapping (mirrors TerrainComponent placement) ----
        // Sample 0 -> worldY = baseY; sample 65535 -> worldY = baseY + heightScale.
        // The Renderer stores the RE-ANCHORED (effective) pair here (its terrain
        // height-range re-anchoring), so collision matches the rendered surface exactly.
        float baseY       = 0.0f;
        float heightScale = 1.0f;

        // ---- Source-data range (normalized 0..1) ----
        // Min / max of the decoded samples, computed once at decode. Heightmaps rarely span
        // the full [0,1]; the Renderer re-anchors the surface with this so the lowest valley
        // stays at TerrainComponent::worldCenter.y whatever heightScale is - otherwise
        // scaling lifts the whole tile by dataMin01 x heightScale.
        float dataMin01 = 0.0f;
        float dataMax01 = 1.0f;

        bool IsValid() const
        {
            return width > 0 && height > 0
                && samples.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
        }

        // Raw uint16 sample at integer coordinates. Out-of-bounds queries clamp to the
        // edge so collision does not pop when an entity slides past the tile border.
        uint16_t SampleRaw(int32_t x, int32_t y) const
        {
            if (!IsValid()) return 0;
            x = std::clamp(x, 0, static_cast<int32_t>(width  - 1));
            y = std::clamp(y, 0, static_cast<int32_t>(height - 1));
            return samples[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
        }

        // Bilinear sample at UV in [0,1]; returns world-space Y. The caller converts world
        // XZ -> UV with the terrain's worldOrigin / worldSize / heightmapUVOffset /
        // heightmapUVScale.
        float SampleHeightWorld(float u, float v) const
        {
            if (!IsValid()) return baseY;

            // Map UV to texel-centred coordinates (matches GPU bilinear).
            const float fx = u * static_cast<float>(width)  - 0.5f;
            const float fy = v * static_cast<float>(height) - 0.5f;

            const int32_t x0 = static_cast<int32_t>(std::floor(fx));
            const int32_t y0 = static_cast<int32_t>(std::floor(fy));
            const float   tx = fx - static_cast<float>(x0);
            const float   ty = fy - static_cast<float>(y0);

            const float a = static_cast<float>(SampleRaw(x0,     y0));
            const float b = static_cast<float>(SampleRaw(x0 + 1, y0));
            const float c = static_cast<float>(SampleRaw(x0,     y0 + 1));
            const float d = static_cast<float>(SampleRaw(x0 + 1, y0 + 1));

            const float ab = a + (b - a) * tx;
            const float cd = c + (d - c) * tx;
            const float h  = ab + (cd - ab) * ty;        // [0, 65535]

            constexpr float kInv65535 = 1.0f / 65535.0f;
            return baseY + h * kInv65535 * heightScale;
        }
    };
}
