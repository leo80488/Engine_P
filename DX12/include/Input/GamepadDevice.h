#pragma once

// GamepadDevice - the gamepad backend seam.
//
// This class is the whole contract a second backend (GameInput, Windows.Gaming.Input,
// Raw-HID) must satisfy: implement Poll / QueueRumble / NotifyDeviceChange and nothing
// above this header changes, including bindings saved in settings.json, because
// PadBtn::Mask is the engine's own vocabulary, not XInput's.
//
// Backend: XInput 1.4 (ships with the Windows 10 SDK, no redistributable); only
// GamepadDevice.cpp includes <xinput.h>. Design: docs/design/gamepad_input_architecture.md

#include "Input/InputTypes.h"

#include <atomic>

class GamepadDevice
{
public:
    static constexpr int kMaxPads = 4;

    GamepadDevice() = default;
    GamepadDevice(const GamepadDevice&)            = delete;
    GamepadDevice& operator=(const GamepadDevice&) = delete;

    // Called exactly once per frame from Input::Update(), on the main thread.
    // focused == false reports every pad neutral but keeps prevButtons, so each held button
    // fires one release edge and nothing latches (the keyboard focus gate's rule). XInput
    // reads pads regardless of the foreground window, so without this the game would react
    // to a pad while alt-tabbed.
    // Timing comes from an internal QueryPerformanceCounter, not a caller dt: Input::Update()
    // has no dt to pass, and a frozen rescan timer would hide a pad plugged in after startup.
    void Poll(bool focused, const InputBindings& cfg) noexcept;

    // Zeroes every pad and its prevButtons, so no release edges fire. Used by
    // Input::Reset() (focus-loss teardown, world reload).
    void Reset() noexcept;

    const GamepadState& State(int slot) const noexcept
    {
#ifdef WITH_HARNESS
        if (slot == kSynthSlot) return m_synth;
#endif
        return m_pads[slot & 3];
    }

#ifdef WITH_HARNESS
    // The harness's synthetic pad (Input::SynthPad, input replay): a fifth, virtual slot,
    // never one of XInput's four, so no real pad is overwritten. While set it is the active
    // pad; its cooked state is taken as given (no deadzone or curve re-applied) and its
    // edges roll once per Poll like a real slot's. When cleared it reports one neutral frame
    // (every held button's release edge), then leaves. @p s nullptr = off. Main thread,
    // before Poll.
    static constexpr int kSynthSlot = kMaxPads;
    void SetSynthetic(const GamepadState* s) noexcept
    {
        m_synthOn = (s != nullptr);
        if (s) m_synthNext = *s;
    }
#endif
    int  ActiveSlot()   const noexcept { return m_activeSlot; }   // -1 = none
    bool AnyConnected() const noexcept { return m_activeSlot >= 0; }
    bool Connected(int slot) const noexcept
    { return (slot >= 0 && slot < kMaxPads) && m_pads[slot].connected; }

    // Seconds until the next probe of a currently-dead slot (editor readout).
    float SecondsToNextRescan() const noexcept;

    // Queues rumble; the next Poll() applies it on the main thread, so this is safe from a
    // Scheduler worker or from Lua in a behaviour-tree leaf (TickPhase::AI runs in
    // parallel). slot < 0 = the active pad; lowFreq / highFreq are clamped to [0,1].
    void QueueRumble(int slot, float lowFreq, float highFreq) noexcept;

    // A WM_DEVICECHANGE arrived: probe all four slots on the next Poll(). Atomic, called
    // from the WndProc thread; makes no XInput call.
    void NotifyDeviceChange() noexcept { m_deviceChangeDirty.store(true, std::memory_order_relaxed); }

    // Edge reports for the frame that just polled, so the caller can raise
    // script events without diffing state itself. Bit N = slot N.
    std::uint8_t JustConnected()    const noexcept { return m_justConnected; }
    std::uint8_t JustDisconnected() const noexcept { return m_justDisconnected; }

private:
    // Probing a disconnected XInput slot is the documented slow path (~milliseconds while a
    // device layer enumerates). Connected slots poll every frame; dead slots are probed one
    // per kRescanSeconds, round-robin, so the worst case is one extra call per frame and a
    // full sweep takes ~2 s. WM_DEVICECHANGE bypasses the schedule.
    static constexpr double kRescanSeconds = 0.5;

    // `pending` is written on a worker and read on the main thread. Release/acquire on it
    // publishes lo / hi with it; a plain bool would race and could let the main thread see
    // the flag with stale amplitudes.
    struct PendingRumble
    {
        std::atomic<bool> pending { false };
        float             lo = 0.f, hi = 0.f;
    };

    GamepadState  m_pads[kMaxPads] = {};
    PendingRumble m_rumble[kMaxPads] = {};
    int           m_activeSlot   = -1;
    int           m_rescanCursor = 0;
    double        m_nextRescan   = 0.0;   // seconds on the internal QPC clock
    bool          m_clockInit    = false;
    std::uint8_t  m_justConnected    = 0;
    std::uint8_t  m_justDisconnected = 0;

    std::atomic<bool> m_deviceChangeDirty { true };   // probe all slots on frame 1

#ifdef WITH_HARNESS
    GamepadState m_synth{};       // what State(kSynthSlot) reports this frame
    GamepadState m_synthNext{};   // what SetSynthetic asked for
    bool         m_synthOn = false;
#endif
};
