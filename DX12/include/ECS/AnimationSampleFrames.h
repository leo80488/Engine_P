#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Anim
{
struct SampleFrames { uint32_t first, second; float alpha; };
inline SampleFrames ResolveSampleFrames(float time, float rate, uint32_t count, bool looping)
{
    if (!count || !(rate > 0.f) || !std::isfinite(time)) return {0, 0, 0.f};
    double frame = static_cast<double>(time) * rate;
    if (looping)
    {
        frame = std::fmod(frame, static_cast<double>(count));
        if (frame < 0) frame += count;
    }
    else
        frame = std::clamp(frame, 0.0, static_cast<double>(count - 1));
    const auto first = static_cast<uint32_t>(frame);
    const auto second = looping ? (first + 1) % count : std::min(first + 1, count - 1);
    return {first, second, static_cast<float>(frame - std::floor(frame))};
}
}
