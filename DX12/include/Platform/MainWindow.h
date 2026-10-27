#pragma once
// Platform/MainWindow.h - the process's main window, for code that must not include
// Platform/Window.h: the harness's window probe (Lua Test.Window / Test.WindowState, WITH_HARNESS
// only) and the control channel's capture refusal (any thread).
//
// Every function is safe on any thread unless it says otherwise, and answers "nothing" before the
// window exists and after it is destroyed. Implemented in src/Platform/Window.cpp.

#include <Windows.h>
#include <cstdint>
#include <string>

namespace Plat
{
    // The main window's handle; nullptr when there is none.
    HWND MainWindowHandle() noexcept;

    // The main window is minimized right now (IsIconic). The control channel's
    // `capture` asks it from the pipe thread: a minimized window presents
    // nothing an app_ui picture could show.
    bool MainWindowMinimized() noexcept;

    // The application-foreground state the window latched from WM_ACTIVATEAPP:
    // what the AppFocusChanged event reports.
    bool MainWindowAppFocused() noexcept;

    // How many SC_KEYMENU requests (a lone Alt or F10, Alt+letter) the window
    // swallowed instead of letting DefWindowProc enter its menu loop. A pack
    // reads it to prove the path it drove was the one it meant to.
    std::uint32_t MainWindowKeyMenuSwallowed() noexcept;

    // MAIN THREAD ONLY. The text typed into the main window since the last call
    // (KeyboardState::ConsumeText, UTF-8), for the harness (Test.TypedText);
    // empty when there is no window.
    std::string MainWindowConsumeTypedText();

    // MAIN THREAD ONLY. How many times the WndProc recorded @p vk going down (KeyboardState::DownEdges),
    // for the harness (Test.KeyEdges); 0 without a window.
    std::uint32_t MainWindowKeyDownEdges(int vk) noexcept;

    // What the window counted over its life: the harness reads it (Test.WindowState) and the window
    // logs it when it goes. All zero without a window.
    struct MainWindowStats
    {
        // Visible to the person: a window created hidden waits for the first presented frame.
        bool          shown = false;
        // The longest time between two Pump calls since it was shown - what reads as "not responding".
        double        longestUnpumpedMs = 0.0;
        std::uint64_t pumps = 0;
        // Message pumps run while the main thread waited for the render-owner lane, and the messages
        // they dispatched.
        std::uint32_t nestedPumps = 0;
        std::uint32_t nestedMessages = 0;
        // Notifications the bounded queue refused, and ones folded into a queued one of their kind.
        std::uint32_t eventsDropped = 0;
        std::uint32_t eventsMerged = 0;
        // A borderless fullscreen window placed on its monitor again after a display change.
        std::uint32_t displayRefits = 0;
        // WM_QUERYENDSESSION / WM_ENDSESSION(TRUE) seen, the blocks the window put up for unsaved work.
        std::uint32_t queryEndSessions = 0;
        std::uint32_t endSessions = 0;
        std::uint32_t shutdownBlocks = 0;
        std::uint32_t powerSuspends = 0;
        std::uint32_t powerResumes = 0;
        // Present reported the window covered (DXGI_STATUS_OCCLUDED in a row), and the loop is being
        // held to ~10 Hz for it or for a minimize.
        bool          occluded = false;
        bool          throttling = false;
    };
    // MAIN THREAD ONLY.
    MainWindowStats MainWindowStatsNow();
}
