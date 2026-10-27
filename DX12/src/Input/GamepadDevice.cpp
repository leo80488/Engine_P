// GamepadDevice - the XInput 1.4 backend, the only TU that includes <xinput.h>; above
// GamepadDevice.h the engine speaks PadBtn::Mask / GamepadState (see the header).
// Each rule in Poll() is a shipped bug class: the focus gate, prevButtons kept on a
// neutral pad, one dead-slot probe per kRescanSeconds, the internal QPC clock, rumble
// drained on the main thread. Do not simplify one away.

#include "Input/GamepadDevice.h"
#include "System/Log.h"

#include <xinput.h>
#include <cmath>

// Links xinput.lib through EngineCore.lib (a StaticLibrary with no
// AdditionalDependencies), so every exe inherits it; AudioEngine.cpp does the same for
// xaudio2.lib. pch.h is force-included and pulls full <windows.h>, so a
// WIN32_LEAN_AND_MEAN define here would be a no-op.
#pragma comment(lib, "xinput.lib")

namespace
{
// XInput reports sticks as SHORT and triggers as BYTE.
constexpr float kStickScale   = 1.0f / 32767.0f;
constexpr float kTriggerScale = 1.0f / 255.0f;

// XINPUT_VIBRATION motor speeds are WORD [0, 65535].
constexpr float kMotorScale   = 65535.0f;

// XINPUT_GAMEPAD_TRIGGER_THRESHOLD (30/255): the floor for the synthesized digital LT/RT
// bits, used only when triggerThreshold is 0 (see Poll).
constexpr float kTriggerDigitalFloor = 30.0f / 255.0f;

// XINPUT_GAMEPAD_* -> PadBtn::Mask; keeps every XInput constant inside this file.
struct PadButtonMapping
{
    WORD          xinput;
    std::uint16_t ours;
};

constexpr PadButtonMapping kButtonMap[] =
{
    { XINPUT_GAMEPAD_A,              PadBtn::A      },
    { XINPUT_GAMEPAD_B,              PadBtn::B      },
    { XINPUT_GAMEPAD_X,              PadBtn::X      },
    { XINPUT_GAMEPAD_Y,              PadBtn::Y      },
    { XINPUT_GAMEPAD_LEFT_SHOULDER,  PadBtn::LB     },
    { XINPUT_GAMEPAD_RIGHT_SHOULDER, PadBtn::RB     },
    { XINPUT_GAMEPAD_BACK,           PadBtn::Back   },
    { XINPUT_GAMEPAD_START,          PadBtn::Start  },
    { XINPUT_GAMEPAD_LEFT_THUMB,     PadBtn::LS     },
    { XINPUT_GAMEPAD_RIGHT_THUMB,    PadBtn::RS     },
    { XINPUT_GAMEPAD_DPAD_UP,        PadBtn::DUp    },
    { XINPUT_GAMEPAD_DPAD_DOWN,      PadBtn::DDown  },
    { XINPUT_GAMEPAD_DPAD_LEFT,      PadBtn::DLeft  },
    { XINPUT_GAMEPAD_DPAD_RIGHT,     PadBtn::DRight },
};

// Clamps to [0,1]. Written as !(v > 0) so a NaN (a corrupt settings.json, a Lua call)
// becomes 0 instead of reaching the motors or the deadzone divide.
float Clamp01(float v) noexcept
{
    if (!(v > 0.0f)) return 0.0f;
    return (v > 1.0f) ? 1.0f : v;
}

// SHORT -> [-1,1], clamped per component (-32768 / 32767 = -1.00003). A full diagonal
// still has magnitude 1.414, so ApplyStickDeadzone clamps the magnitude as well.
float NormalizeStickAxis(SHORT raw) noexcept
{
    const float v = static_cast<float>(raw) * kStickScale;
    if (v < -1.0f) return -1.0f;
    if (v >  1.0f) return  1.0f;
    return v;
}

// Radial deadzone, not per-axis. Do not split it into two 1-D tests: per-axis gating
// snaps diagonals toward the cardinals and makes a round stick gate feel square.
// Guarantees: continuous, non-decreasing in magnitude, exactly 0 at mag == dz, exactly 1
// at mag == 1, direction preserved.
void ApplyStickDeadzone(float& x, float& y, float dz, float curveExp) noexcept
{
    // dz >= 1 (or NaN) would divide by zero below: treat everything as deadzone.
    if (!(dz < 1.0f))
    {
        x = 0.0f;
        y = 0.0f;
        return;
    }
    if (!(dz > 0.0f)) dz = 0.0f;
    if (!(curveExp >= 1.0f)) curveExp = 1.0f;

    float mag = std::sqrt(x * x + y * y);
    if (mag <= dz)
    {
        x = 0.0f;                                   // exactly 0 inside the gate
        y = 0.0f;
        return;
    }
    // Take the direction from the true magnitude, before the clamp: dividing by the clamped
    // value leaves a full diagonal at length 1.414 and diagonal look sqrt(2)x too fast.
    const float nx = x / mag;                       // unit direction preserved
    const float ny = y / mag;

    if (mag > 1.0f) mag = 1.0f;                     // see NormalizeStickAxis
    const float t  = std::pow((mag - dz) / (1.0f - dz), curveExp);

    x = nx * t;
    y = ny * t;
}

// Triggers are 1-D, and use the same remap so 0 sits exactly at the threshold.
float ApplyTrigger(float v, float thresh) noexcept
{
    if (!(thresh < 1.0f)) return 0.0f;              // also catches NaN
    if (!(thresh > 0.0f)) thresh = 0.0f;
    return (v <= thresh) ? 0.0f : (v - thresh) / (1.0f - thresh);
}

std::uint16_t MapButtons(WORD wButtons) noexcept
{
    std::uint16_t out = PadBtn::None;
    for (const PadButtonMapping& m : kButtonMap)
    {
        // Explicit cast: `out |= m.ours` promotes to int and narrows back (C4244 at /W3).
        if (wButtons & m.xinput) out = static_cast<std::uint16_t>(out | m.ours);
    }
    return out;
}

// Process-wide monotonic seconds. Frequency and origin are captured once (QPC frequency
// is fixed at boot); subtracting the origin keeps the double precise enough for 0.5 s
// comparisons after days of uptime.
double QpcSeconds() noexcept
{
    static const LONGLONG s_freq = []() noexcept
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    static const LONGLONG s_origin = []() noexcept
    {
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }();

    if (s_freq <= 0) return 0.0;                    // cannot happen on XP+

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - s_origin) / static_cast<double>(s_freq);
}

} // anonymous namespace

void GamepadDevice::Poll(bool focused, const InputBindings& cfg) noexcept
{
    // 1. Drain queued rumble first. QueueRumble() runs on workers and in behaviour-tree Lua,
    // so the pending flags are read and cleared before anything can early-out; doing it last
    // or behind the `focused` check would strand a request and leave a motor silent or
    // spinning. The request is applied at the end of Poll, after the cook loop refreshes
    // pad.connected: testing it here would use last frame's value and drop the first buzz
    // after a hot-plug.
    struct RumbleReq { bool want = false; float lo = 0.f, hi = 0.f; };
    RumbleReq req[kMaxPads];

    for (int slot = 0; slot < kMaxPads; ++slot)
    {
        PendingRumble& r = m_rumble[slot];
        // Acquire pairs with the release in QueueRumble: seeing the flag
        // guarantees the amplitudes written before it are visible too.
        if (!r.pending.load(std::memory_order_acquire)) continue;

        req[slot].want = true;
        req[slot].lo   = r.lo;
        req[slot].hi   = r.hi;
        r.pending.store(false, std::memory_order_relaxed);   // consumed
    }

    // 2. Edge reports describe this Poll only: a caller that skips a frame sees nothing
    // rather than a stale event.
    m_justConnected    = 0;
    m_justDisconnected = 0;

    // 3. Clock, lazily initialised so the first Poll does not treat the rescan as long
    // overdue and burn a sweep it does not owe.
    const double now = QpcSeconds();
    if (!m_clockInit)
    {
        m_clockInit  = true;
        m_nextRescan = now;
    }

    // 4. Choose the slots to probe: every connected slot each frame, and at most one dead
    // slot per kRescanSeconds, round-robin (XInputGetState on an empty slot is the slow path,
    // ~1 ms, a frame-time spike), so a four-slot sweep takes ~2 s. A WM_DEVICECHANGE sweeps
    // all slots at once, which makes hot-plug instant.
    bool probe[kMaxPads] = {};
    const bool probeAll = m_deviceChangeDirty.exchange(false, std::memory_order_relaxed);

    if (probeAll)
    {
        for (int slot = 0; slot < kMaxPads; ++slot) probe[slot] = true;

        // A full sweep just ran; the round-robin has nothing to find for another interval.
        m_nextRescan = now + kRescanSeconds;
    }
    else
    {
        for (int slot = 0; slot < kMaxPads; ++slot) probe[slot] = m_pads[slot].connected;

        if (now >= m_nextRescan)
        {
            for (int n = 0; n < kMaxPads; ++n)
            {
                const int slot = (m_rescanCursor + n) % kMaxPads;
                if (!m_pads[slot].connected)
                {
                    probe[slot]    = true;
                    m_rescanCursor = (slot + 1) % kMaxPads;
                    break;                          // EXACTLY one per interval
                }
            }

            // Advance even when every slot was already connected, otherwise the
            // deadline stays in the past and re-fires every single frame.
            m_nextRescan += kRescanSeconds;

            // Catch-up guard: a breakpoint, a level load or a minimised window can leave the
            // deadline far behind; do not burn a dead-slot probe per frame to make up the time.
            if (m_nextRescan < now) m_nextRescan = now + kRescanSeconds;
        }
    }

    // 5. Cook every slot.
    const float dz        = cfg.stickDeadzone;
    const float curveExp  = cfg.stickCurveExp;
    const float trigThres = cfg.triggerThreshold;

    int active = -1;

    for (int slot = 0; slot < kMaxPads; ++slot)
    {
        GamepadState& pad = m_pads[slot];

        // Roll prevButtons once per slot per Poll, before the new buttons are written, on every
        // slot including unprobed ones: an unprobed connected slot keeps prevButtons == buttons
        // (no edge); an unprobed disconnected slot reports 0 and fires one release edge, then
        // stays quiet.
        pad.prevButtons = pad.buttons;

        bool          connected  = pad.connected;
        bool          haveFresh  = false;
        XINPUT_STATE  xs{};

        if (probe[slot])
        {
            const DWORD res = XInputGetState(static_cast<DWORD>(slot), &xs);
            if (res == ERROR_SUCCESS)
            {
                if (!connected)
                {
                    connected = true;
                    m_justConnected = static_cast<std::uint8_t>(m_justConnected | (1u << slot));
                    // Transition only. Never log per frame here: this is the frame hot path.
                    LOG_INFO("[Input] Gamepad connected in slot %d", slot);
                }
                pad.packet = static_cast<std::uint32_t>(xs.dwPacketNumber);
                haveFresh  = true;
            }
            else
            {
                // ERROR_DEVICE_NOT_CONNECTED is the normal answer for an empty slot. Any other error (a
                // driver hiccup) is treated the same: no trustworthy state, so the pad is gone.
                if (connected)
                {
                    m_justDisconnected = static_cast<std::uint8_t>(m_justDisconnected | (1u << slot));
                    LOG_INFO("[Input] Gamepad disconnected from slot %d", slot);
                }
                connected  = false;
                pad.packet = 0;
            }
        }

        pad.connected = connected;

        // Values only while connected, focused and gamepad support is on. This gates the values,
        // not the polling: connect / disconnect detection and the editor readout stay live while
        // unfocused.
        const bool live = connected && focused && cfg.gamepadEnabled;

        if (!live)
        {
            // Neutral, but prevButtons still holds the last live frame, so each held button fires
            // one release edge: the stuck-sprint-after-alt-tab / unplug guard. Do not skip the roll
            // above.
            pad.buttons = PadBtn::None;
            pad.lx = pad.ly = 0.0f;
            pad.rx = pad.ry = 0.0f;
            pad.lt = pad.rt = 0.0f;
            pad.rawLx = pad.rawLy = 0.0f;
            pad.rawRx = pad.rawRy = 0.0f;
        }
        else if (haveFresh)
        {
            const XINPUT_GAMEPAD& g = xs.Gamepad;

            float lx = NormalizeStickAxis(g.sThumbLX);
            float ly = NormalizeStickAxis(g.sThumbLY);
            float rx = NormalizeStickAxis(g.sThumbRX);
            float ry = NormalizeStickAxis(g.sThumbRY);

            // Raw snapshot before the deadzone remap: the editor readout plots these, so the
            // deadzone ring it draws is the boundary the dot actually crosses.
            pad.rawLx = lx; pad.rawLy = ly;
            pad.rawRx = rx; pad.rawRy = ry;

            // XInput's Y is already +up on both sticks. Do not negate here: the look sign (and
            // padInvertLookY) is applied once, at the camera.
            ApplyStickDeadzone(lx, ly, dz, curveExp);
            ApplyStickDeadzone(rx, ry, dz, curveExp);

            const float lt = ApplyTrigger(static_cast<float>(g.bLeftTrigger)  * kTriggerScale, trigThres);
            const float rt = ApplyTrigger(static_cast<float>(g.bRightTrigger) * kTriggerScale, trigThres);

            std::uint16_t buttons = MapButtons(g.wButtons);

            // Synthesize LT / RT so a trigger binds like a button. ApplyTrigger returns 0 at or
            // below triggerThreshold, so `> 0` is the threshold test (a second test would raise the
            // bite point). A threshold of 0 is legal (range [0, 0.9]) and would let the digital bit
            // chatter on resting-trigger noise, so then the digital bit uses XInput's recommended
            // floor; the analog value stays untouched.
            const float digitalFloor = (trigThres > 0.f) ? 0.f : kTriggerDigitalFloor;
            if (lt > digitalFloor) buttons = static_cast<std::uint16_t>(buttons | PadBtn::LT);
            if (rt > digitalFloor) buttons = static_cast<std::uint16_t>(buttons | PadBtn::RT);

            pad.buttons = buttons;
            pad.lx = lx; pad.ly = ly;
            pad.rx = rx; pad.ry = ry;
            pad.lt = lt; pad.rt = rt;
        }
        // Else (connected, live, not probed this frame): keep last frame's cooked values;
        // prevButtons == buttons, so no edge is manufactured.

        // Lowest connected slot wins. Presence-based on purpose: rumble and the
        // editor readout should still name a pad while the window is unfocused.
        if (connected && active < 0) active = slot;
    }

    m_activeSlot = active;

#ifdef WITH_HARNESS
    // The synthetic pad (SetSynthetic): its edge roll, then its state. It is
    // focus-independent on purpose - a hidden test run has no focus - and it
    // outranks a physical pad while it is set.
    m_synth.prevButtons = m_synth.buttons;
    if (m_synthOn)
    {
        const std::uint16_t prev = m_synth.prevButtons;
        m_synth             = m_synthNext;
        m_synth.prevButtons = prev;
        m_synth.connected   = true;
        m_activeSlot        = kSynthSlot;
    }
    else if (m_synth.connected)
    {
        // Leaving: one neutral frame so every held button fires its release,
        // then the slot is gone and the physical pads are active again.
        const std::uint16_t prev = m_synth.prevButtons;
        m_synth             = GamepadState{};
        m_synth.prevButtons = prev;
        m_synth.connected   = (prev != 0);
        if (m_synth.connected) m_activeSlot = kSynthSlot;
    }
#endif

    // 6. Apply the rumble consumed in step 1, now that connected reflects this frame. Gated
    // on `focused` like every other output: XInput is global, and a buzz while alt-tabbed
    // looks like a stuck motor. The request is still consumed; the zero amplitude doubles as
    // the stop for a buzz in flight when focus is lost.
    const bool motorsLive = focused && cfg.gamepadEnabled;
    for (int slot = 0; slot < kMaxPads; ++slot)
    {
        if (!req[slot].want || !m_pads[slot].connected) continue;

        // The vibration call is XInputSetState; there is no XInputSetVibration in <xinput.h>.
        XINPUT_VIBRATION vib{};
        if (motorsLive)
        {
            vib.wLeftMotorSpeed  = static_cast<WORD>(req[slot].lo * kMotorScale + 0.5f);  // low-freq / heavy
            vib.wRightMotorSpeed = static_cast<WORD>(req[slot].hi * kMotorScale + 0.5f);  // high-freq / light
        }
        XInputSetState(static_cast<DWORD>(slot), &vib);
    }
}

void GamepadDevice::Reset() noexcept
{
    for (int slot = 0; slot < kMaxPads; ++slot)
    {
        // Stop the motors first: a vibration latches in the device, so dropping the queue alone
        // leaves a pad buzzing across a world reload.
        if (m_pads[slot].connected)
        {
            XINPUT_VIBRATION vib{};                 // both motors zeroed
            XInputSetState(static_cast<DWORD>(slot), &vib);
        }

        m_pads[slot] = GamepadState{};              // including prevButtons
        // PendingRumble holds an atomic and is not assignable: clear its members instead.
        m_rumble[slot].pending.store(false, std::memory_order_relaxed);
        m_rumble[slot].lo = 0.f;
        m_rumble[slot].hi = 0.f;
    }

    m_activeSlot       = -1;
    m_justConnected    = 0;
    m_justDisconnected = 0;

    // With prevButtons gone no release edge fires (the point of Reset). Every slot now reads
    // disconnected, so force a full sweep on the next Poll; otherwise a pad still plugged in
    // waits out the round-robin.
    m_deviceChangeDirty.store(true, std::memory_order_relaxed);
}

float GamepadDevice::SecondsToNextRescan() const noexcept
{
    // A probe is already owed: before the first Poll (the clock has no origin
    // yet) and whenever a device change is still pending.
    if (!m_clockInit) return 0.0f;
    if (m_deviceChangeDirty.load(std::memory_order_relaxed)) return 0.0f;

    const double remain = m_nextRescan - QpcSeconds();
    return (remain > 0.0) ? static_cast<float>(remain) : 0.0f;
}

// Callable from any thread.
void GamepadDevice::QueueRumble(int slot, float lowFreq, float highFreq) noexcept
{
    // slot < 0 = the active pad. m_activeSlot is a plain int read from a worker: a value one
    // frame stale at worst lands the buzz on the wrong slot or drops it, so it is not atomic.
    if (slot < 0) slot = m_activeSlot;
    if (slot < 0 || slot >= kMaxPads) return;       // no pad at all: drop silently

    PendingRumble& r = m_rumble[slot];

    // Amplitudes first, then the flag with release ordering: it publishes both floats to
    // Poll's acquire load. A weaker store would let the drain see the flag with stale
    // amplitudes.
    r.lo = Clamp01(lowFreq);
    r.hi = Clamp01(highFreq);
    r.pending.store(true, std::memory_order_release);
}
