#pragma once

// Shared bounded drag widgets for editor/tool ImGui panels.
//
// They take the SliderFloat/SliderInt call shape (label, value, min, max, format,
// flags) but render DragFloat/DragInt with a drag speed derived from the authored
// range, so small 0..1 controls stay precise while large world-space/debug ranges
// remain practical.

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>

namespace EditorUI
{
inline float BoundedDragSpeed(float minValue, float maxValue)
{
    const float range = std::fabs(maxValue - minValue);
    if (!std::isfinite(range) || range <= 0.0f) return 0.01f;
    return std::clamp(range / 500.0f, 0.0001f, 10.0f);
}

inline bool DragFloat(const char* label, float* value,
                      float minValue, float maxValue,
                      const char* format = "%.3f",
                      ImGuiSliderFlags flags = 0)
{
    return ImGui::DragFloat(label, value,
                            BoundedDragSpeed(minValue, maxValue),
                            minValue, maxValue, format, flags);
}

inline bool DragInt(const char* label, int* value,
                    int minValue, int maxValue,
                    const char* format = "%d",
                    ImGuiSliderFlags flags = 0)
{
    const double range = static_cast<double>(maxValue)
                       - static_cast<double>(minValue);
    const float speed = std::max(1.0f,
        static_cast<float>(std::fabs(range) / 500.0));
    return ImGui::DragInt(label, value, speed,
                          minValue, maxValue, format, flags);
}
} // namespace EditorUI
