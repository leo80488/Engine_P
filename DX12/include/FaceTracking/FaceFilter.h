#pragma once
// One Euro filtering of a face source's frames, one parameter set per channel group. Each
// binding filters the frames it consumes in order, on the frames' own timestamps - never the
// engine's dt - so Edit mode (dt = 0) and a tracker running at a different rate than the
// engine both stay right.
// The head is filtered on its rotation: the speed is the angle between consecutive frames, the
// output a slerp from the previous filtered rotation towards the new one.
// Header-only: tests/unit/face_map_test.cpp drives it without the engine library.

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceMath.h"

#include <cmath>
#include <cstdint>

namespace FaceTracking {

struct OneEuroParams
{
    float minCutoff = 1.0f; // Hz: the cutoff at rest - lower = smoother, laggier
    float beta = 0.0f;      // how fast the cutoff opens with speed - higher = less lag when moving
    float dCutoff = 1.0f;   // Hz: the speed estimate's own low-pass
    bool enabled = true;
};

// Channel groups that share filter settings.
enum class FilterGroup : uint8_t { Eyelid, BrowCheekNose, MouthJaw, Gaze, Extra, Count };

inline FilterGroup FilterGroupOf(int ch)
{
    if (ch < 0 || ch >= kArkitChannelCount) return FilterGroup::Extra;
    const std::string_view n = kArkitChannels[ch].name;
    switch (kArkitChannels[ch].group)
    {
    case ChannelGroup::Eye:
        // eyeLook* is where the eye points; blink / squint / wide is the lid.
        return n.substr(0, 7) == "eyeLook" ? FilterGroup::Gaze : FilterGroup::Eyelid;
    case ChannelGroup::Jaw:
    case ChannelGroup::Mouth:
    case ChannelGroup::Tongue: return FilterGroup::MouthJaw;
    default: return FilterGroup::BrowCheekNose;
    }
}

// The "default" preset: lids fast (a blink is a few frames), brows / cheeks / nose smooth
// (their jitter shows most), mouth and jaw in between (speech must keep up).
struct FaceFilterSettings
{
    OneEuroParams group[static_cast<int>(FilterGroup::Count)] = {
        {4.0f, 1.5f, 1.0f, true},  // Eyelid
        {1.0f, 0.3f, 1.0f, true},  // BrowCheekNose
        {2.0f, 0.8f, 1.0f, true},  // MouthJaw
        {1.5f, 0.4f, 1.0f, true},  // Gaze
        {1.5f, 0.5f, 1.0f, true},  // Extra
    };
    OneEuroParams head = {1.5f, 0.6f, 1.0f, true}; // minCutoff Hz; beta per radian/s
    OneEuroParams eyes = {1.5f, 0.4f, 1.0f, true};
};

namespace OneEuro {
inline float Alpha(float cutoffHz, float dt)
{
    const float tau = 1.0f / (2.0f * FaceMath::kPi * cutoffHz);
    return 1.0f / (1.0f + tau / dt);
}
} // namespace OneEuro

class OneEuroScalar
{
public:
    void Reset() { m_have = false; }
    float Filter(float x, float dt, const OneEuroParams& p)
    {
        if (!p.enabled || !m_have || !(dt > 0.0f))
        {
            if (!m_have || !p.enabled) { m_x = x; m_dx = 0.0f; m_have = true; }
            return m_x;
        }
        const float dx = (x - m_x) / dt;
        m_dx += OneEuro::Alpha(p.dCutoff, dt) * (dx - m_dx);
        const float cutoff = p.minCutoff + p.beta * std::fabs(m_dx);
        m_x += OneEuro::Alpha(cutoff, dt) * (x - m_x);
        return m_x;
    }
    float Value() const { return m_x; }

private:
    bool m_have = false;
    float m_x = 0.0f, m_dx = 0.0f;
};

class OneEuroRotation
{
public:
    void Reset() { m_have = false; }
    DirectX::XMFLOAT4 Filter(const DirectX::XMFLOAT4& q, float dt, const OneEuroParams& p)
    {
        if (!p.enabled || !m_have || !(dt > 0.0f))
        {
            if (!m_have || !p.enabled) { m_q = q; m_speed = 0.0f; m_have = true; }
            return m_q;
        }
        const float d = std::fabs(m_q.x * q.x + m_q.y * q.y + m_q.z * q.z + m_q.w * q.w);
        const float angle = 2.0f * std::acos(std::fmin(1.0f, d));
        m_speed += OneEuro::Alpha(p.dCutoff, dt) * (angle / dt - m_speed);
        const float cutoff = p.minCutoff + p.beta * std::fabs(m_speed);
        m_q = FaceMath::SlerpUnclamped(m_q, q, OneEuro::Alpha(cutoff, dt));
        return m_q;
    }

private:
    bool m_have = false;
    DirectX::XMFLOAT4 m_q = {0, 0, 0, 1};
    float m_speed = 0.0f;
};

// A binding's filter state: one scalar filter per channel, one for the head, two per eye.
class FaceFrameFilter
{
public:
    void Reset()
    {
        for (OneEuroScalar& c : m_channels) c.Reset();
        m_head.Reset();
        for (OneEuroScalar& e : m_eyes) e.Reset();
        m_lastUs = 0;
        m_have = false;
    }

    // Filters `f` in place; frames must come in source-time order (a frame no newer than the
    // last one passes through unchanged - the history refuses such frames anyway).
    void Apply(FaceFrame& f, const FaceFilterSettings& s)
    {
        const float dt = m_have ? static_cast<float>(static_cast<double>(f.sourceTimeUs - m_lastUs) / 1e6) : 0.0f;
        if (m_have && !(dt > 0.0f)) return;
        for (int ch = 0; ch < kMaxFaceChannels; ++ch)
            if (f.Has(ch)) f.values[ch] = m_channels[ch].Filter(f.values[ch], dt, s.group[static_cast<int>(FilterGroupOf(ch))]);
        if (f.flags & kFaceHasHead) f.headRot = m_head.Filter(f.headRot, dt, s.head);
        if (f.flags & kFaceHasEyes)
            for (int side = 0; side < 2; ++side)
                for (int k = 0; k < 2; ++k)
                    f.eyeYawPitch[side][k] = m_eyes[side * 2 + k].Filter(f.eyeYawPitch[side][k], dt, s.eyes);
        m_lastUs = f.sourceTimeUs;
        m_have = true;
    }

private:
    OneEuroScalar m_channels[kMaxFaceChannels];
    OneEuroRotation m_head;
    OneEuroScalar m_eyes[4];
    int64_t m_lastUs = 0;
    bool m_have = false;
};

} // namespace FaceTracking
