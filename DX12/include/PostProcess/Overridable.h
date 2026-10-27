#pragma once

// PostProcess::Overridable<T> - the lowest-level data primitive of the
// post-process volume system. Like Unreal's bOverride_X / Unity's overrideState,
// every authored property carries an explicit "do I override this?" flag beside
// its value.
//
// Blend rule (ResolvedPostProcessSettings.h): ONLY properties whose
// overrideState == true take part in the lerp; untouched ones let the
// lower-priority layer (or the engine default) show through.
// PostProcessProfile is built entirely from Overridable<T> members; the flat
// ResolvedPostProcessSettings holds the same fields as bare T (no flag).

#include <DirectXMath.h>
#include <cmath>
#include <cstdint>

namespace PostProcess
{

template <typename T>
struct Overridable
{
    bool overrideState = false;   // does this layer override the property?
    T    value{};                 // the authored value (only used when overriding)

    Overridable() = default;
    explicit Overridable(const T& v, bool ov = true) : overrideState(ov), value(v) {}

    // Set the value AND mark it overriding in one call (editor / Lua).
    void Set(const T& v) { value = v; overrideState = true; }
    void Clear()         { overrideState = false; }
};

// ==== Typed blend helpers ====
// For the X-macro resolve loop (BlendProfileInto, which blends volumes and
// transient overrides alike), so every layer blends a type the same way.

// Bool: snap to the override side once the weight crosses 0.5, so toggles
// switch decisively instead of flickering mid-transition.
inline bool BlendValue(bool a, bool b, float t) { return (t >= 0.5f) ? b : a; }

inline float BlendValue(float a, float b, float t) { return a + t * (b - a); }

inline DirectX::XMFLOAT3 BlendValue(const DirectX::XMFLOAT3& a,
                                    const DirectX::XMFLOAT3& b, float t)
{
    return DirectX::XMFLOAT3{
        a.x + t * (b.x - a.x),
        a.y + t * (b.y - a.y),
        a.z + t * (b.z - a.z),
    };
}

// Integer counts (e.g. the lens-flare ghost count) lerp in float space, then
// round.
inline uint32_t BlendValue(uint32_t a, uint32_t b, float t)
{
    const float r = static_cast<float>(a) + t * (static_cast<float>(b) - static_cast<float>(a));
    return static_cast<uint32_t>(std::lround(r));
}

// Exposure multipliers (manual, min, max) blend in stops: halfway from 1 to 4 is 2, not 2.5. A value at or
// below 0 has no logarithm and blends linearly.
inline float BlendLog2(float a, float b, float t)
{
    if (!(a > 0.0f) || !(b > 0.0f)) return BlendValue(a, b, t);
    return std::exp2(std::log2(a) + t * (std::log2(b) - std::log2(a)));
}

// An angle in degrees (-180..180) takes the short way round the circle: from 170 toward -170 passes 180.
inline float BlendAngleDeg(float a, float b, float t)
{
    float d = std::fmod(b - a, 360.0f);
    if (d > 180.0f)       d -= 360.0f;
    else if (d < -180.0f) d += 360.0f;
    float r = a + t * d;
    if (r > 180.0f)       r -= 360.0f;
    else if (r < -180.0f) r += 360.0f;
    return r;
}

} // namespace PostProcess
