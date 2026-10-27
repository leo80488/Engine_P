#pragma once
// Platform/BackgroundPolicy.h - what the engine does while the person is not looking at it.
//
// Three situations, each with its own answer:
//   * minimized, or covered by another window (Present reports DXGI_STATUS_OCCLUDED): nobody can see a
//     frame, so the loop is held to about 10 Hz (Window::ThrottleInBackground);
//   * the application lost the foreground (alt-tab, a click elsewhere): a game mutes its audio, and a
//     project that wants it pauses through GameFlow (its own pause owner: it stacks with the menu key's
//     and a script's, and the game runs again when the last is released);
//   * a harness run (-hidden is a minimized window by construction, -unattended is not a person) and an open
//     control channel are exempt from all of it, or every pack would run at a tenth of its speed.
// The editor is a tool: it throttles when hidden, but it never mutes or pauses a scene because the person
// looked at another window.
//
// DecideBackground is a pure function of the policy, the state and the host, so a unit test
// (tests/unit/background_policy_test.cpp) judges every combination with no window.

namespace Plat
{
    // The project's choices (Lua Platform.setBackgroundPolicy; the defaults are the shipped game's).
    struct BackgroundPolicy
    {
        bool throttleMinimized = true;    // hold the loop to ~10 Hz while minimized
        bool throttleOccluded  = true;    // ... and while another window covers it
        bool muteOnFocusLoss   = true;    // Game.exe: audio off while the application is not the foreground
        bool pauseOnFocusLoss  = false;   // Game.exe: GameFlow pause (its own owner) while not the foreground
    };

    struct BackgroundState
    {
        bool minimized = false;
        bool occluded  = false;   // Present reported DXGI_STATUS_OCCLUDED on consecutive frames
        bool focused   = true;    // the application is the foreground (WM_ACTIVATEAPP)
    };

    struct BackgroundHost
    {
        bool editor              = false;
        bool harnessRun          = false;   // -hidden or -unattended
        bool controlChannelOpen  = false;   // an agent talks to this engine
        bool forced              = false;   // -throttle_minimized / -background_policy: apply it to a harness run
        bool disabled            = false;   // -background_policy_off: the engine does nothing in the background
    };

    struct BackgroundDecision
    {
        bool        throttle = false;
        bool        mute     = false;
        bool        pause    = false;
        // Why it throttles: "minimized" or "covered"; null when it does not.
        const char* throttleReason = nullptr;
    };

    inline BackgroundDecision DecideBackground(const BackgroundPolicy& policy, const BackgroundState& state,
                                               const BackgroundHost& host) noexcept
    {
        BackgroundDecision d;
        if (host.disabled)
            return d;
        const bool exempt = host.controlChannelOpen || (host.harnessRun && !host.forced);
        if (!exempt)
        {
            if (state.minimized && policy.throttleMinimized)
            {
                d.throttle       = true;
                d.throttleReason = "minimized";
            }
            else if (state.occluded && !state.minimized && policy.throttleOccluded)
            {
                d.throttle       = true;
                d.throttleReason = "covered";
            }
        }
        // Focus is the application's, not the window's: it applies to a game only, and a harness run is
        // never "away" unless the pack says so.
        const bool focusPolicy = !host.editor && (!host.harnessRun || host.forced);
        if (focusPolicy && !state.focused)
        {
            d.mute  = policy.muteOnFocusLoss;
            d.pause = policy.pauseOnFocusLoss;
        }
        return d;
    }

    // The project's policy (main thread).
    BackgroundPolicy& CurrentBackgroundPolicy();
}
