#pragma once
// How much a binding's tracked face counts this frame: 1 while a found face keeps arriving,
// held for holdSec after the last found frame (a blink of a stall must not drop the face),
// then faded to 0 over fadeOutSec ("Lost"), and back to 1 over fadeInSec once a face is found
// again. The one value drives the Tracked morph alpha, the bone deltas' Slerp(I, D, a),
// HeadTrack's share (1 - a) and auto-blink's return. The clock is the source's: simulation
// time for takes and scripts, so a pack's fade lasts the same number of frames every run.
// Header-only (tests/unit/face_map_test.cpp).

#include <algorithm>
#include <cstdint>

namespace FaceTracking {

struct FaceAuthorityParams
{
    float holdSec = 0.25f;
    float fadeOutSec = 0.6f;
    float fadeInSec = 0.2f;
};

enum class TrackState : uint8_t
{
    Lost,     // no face (yet, or for longer than the hold and the fade)
    Tracking, // a found face within the hold
    Holding,  // the newest frame says "no face", still inside the hold: everything held
    Fading,   // past the hold: the authority is falling
};

inline const char* TrackStateName(TrackState s)
{
    switch (s)
    {
    case TrackState::Lost: return "lost";
    case TrackState::Tracking: return "tracking";
    case TrackState::Holding: return "holding";
    case TrackState::Fading: return "fading";
    }
    return "?";
}

class FaceAuthority
{
public:
    void Reset()
    {
        m_value = 0.0f;
        m_state = TrackState::Lost;
        m_haveFound = false;
        m_lastFoundUs = 0;
        m_lastUpdateUs = 0;
        m_haveUpdate = false;
    }

    // nowUs: the binding's clock. foundFrameUs: the time of the newest found frame consumed by
    // this update, or a negative value when none was. newestFound: whether the newest frame
    // consumed so far said a face was there.
    void Update(int64_t nowUs, int64_t foundFrameUs, bool newestFound, const FaceAuthorityParams& p)
    {
        const float dt = m_haveUpdate ? static_cast<float>(static_cast<double>(std::max<int64_t>(0, nowUs - m_lastUpdateUs)) / 1e6) : 0.0f;
        m_lastUpdateUs = nowUs;
        m_haveUpdate = true;
        if (foundFrameUs >= 0)
        {
            m_lastFoundUs = std::max(m_haveFound ? m_lastFoundUs : foundFrameUs, foundFrameUs);
            m_haveFound = true;
        }
        if (!m_haveFound)
        {
            m_value = 0.0f;
            m_state = TrackState::Lost;
            return;
        }
        const float since = static_cast<float>(static_cast<double>(std::max<int64_t>(0, nowUs - m_lastFoundUs)) / 1e6);
        if (since <= p.holdSec)
        {
            if (newestFound)
            {
                m_state = TrackState::Tracking;
                m_value = p.fadeInSec > 0.0f ? std::min(1.0f, m_value + dt / p.fadeInSec) : 1.0f;
            }
            else
                m_state = TrackState::Holding;
            return;
        }
        m_value = p.fadeOutSec > 0.0f ? std::max(0.0f, m_value - dt / p.fadeOutSec) : 0.0f;
        m_state = m_value > 0.0f ? TrackState::Fading : TrackState::Lost;
    }

    float Value() const { return m_value; }
    TrackState State() const { return m_state; }

private:
    float m_value = 0.0f;
    TrackState m_state = TrackState::Lost;
    bool m_haveFound = false;
    bool m_haveUpdate = false;
    int64_t m_lastFoundUs = 0;
    int64_t m_lastUpdateUs = 0;
};

} // namespace FaceTracking
