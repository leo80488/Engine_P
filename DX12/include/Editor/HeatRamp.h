#pragma once

// Editor/HeatRamp.h - the editors' one heatmap ramp: the Mesh Editor's shape-key
// heatmap (MeshShapeKeys: |dp| of a target) and the Skeleton Editor's weight
// heatmap (BoneWeightHeat: one bone's weight) colour the same way, so a picture
// reads alike in both.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Editor
{
    // RGBA8 (byte 0 = R) for @p t in [0, 1]: dark blue at 0, then cyan, green,
    // yellow, red at 1.
    inline std::uint32_t HeatColour(float t)
    {
        static const float kStops[5][3] = { { 0.05f, 0.05f, 0.30f }, { 0.00f, 0.55f, 1.00f },
                                            { 0.00f, 0.90f, 0.35f }, { 1.00f, 0.85f, 0.00f },
                                            { 1.00f, 0.10f, 0.05f } };
        t = std::clamp(t, 0.f, 1.f) * 4.f;
        const int   i = std::min(3, static_cast<int>(t));
        const float f = t - static_cast<float>(i);
        std::uint32_t out = 0xFF000000u;
        for (int c = 0; c < 3; ++c)
        {
            const float v = kStops[i][c] + (kStops[i + 1][c] - kStops[i][c]) * f;
            out |= static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)) << (8 * c);
        }
        return out;
    }
}
