#pragma once

// AppEvents - events published by the application host itself rather than a world
// system: the alt-tab edge.
//
// Its own header, because it belongs to neither end: the producer is App, but App.h is
// per-exe (it has #ifdef WITH_EDITOR members) and no engine TU may include it; the
// origin is Window, which must not know EventBus (it hands the edge to a hook, which
// keeps Game.exe linking a hookless Window). Like Audio/AudioEvents.h and
// ECS/FollowEvents.h it is a small header that includes nothing. System/ rather than
// ECS/: its subscribers (the audio engine, the pause menu, a Lua handler) need no World.

namespace Sys
{
    // The application gained or lost the foreground (alt-tab), published once per genuine
    // edge: never once per frame, never twice for the same value. Raised by Window's
    // WM_ACTIVATEAPP hook, latched by App, published from the main loop.
    // Do not publish it from inside the window procedure: the WndProc is re-entrant (anything
    // that pumps messages - SetWindowPos, ShowWindow, a DXGI fullscreen transition - can run
    // it, including a subscriber inside EventBus::DispatchAll), and the first Publish of a
    // type inserts its channel into the unordered_map DispatchAll is iterating. The loop
    // top costs at most one frame of latency.
    // `focused == false` means another application owns the foreground; it is not raised
    // for a modal dialog owned by this process.
    struct AppFocusChangedEvent
    {
        bool focused = false;
    };
}
