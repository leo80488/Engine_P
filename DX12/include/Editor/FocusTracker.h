#pragma once

// FocusTracker - which editor windows are visible, focused and hovered, for code that runs outside their
// Begin / End (App's game-input gate at the top of the frame, the shortcut dispatcher), and the one window
// that holds the game's mouse capture. A tracked window reports itself once per ImGui frame right after its
// Begin (Note); one that did not report in this frame or the previous one reads as not visible, so a closed,
// collapsed or skipped window can never leave a stale claim. UI thread.
//
// Tracked today: the Scene viewport (kSceneView) and the Game View (kGameView). The shortcut order it
// serves: capture, text input, popup, the focused panel's commands, the active document's, global ones.

#include <cstdint>
#include <map>
#include <string>

namespace Editor
{
    struct WindowFocus
    {
        bool          visible = false;   // Begin returned true: not collapsed, not a hidden dock tab
        bool          focused = false;   // ImGui's focused window is it or one of its children
        bool          hovered = false;   // the mouse is over it (children included), nothing above it
        std::uint64_t frame   = 0;       // the ImGui frame it last reported in
    };

    class FocusTracker
    {
    public:
        static constexpr const char* kSceneView = "core.viewport";
        static constexpr const char* kGameView  = "core.gameView";

        static FocusTracker& Get();

        // Inside the window's Begin / End, after Begin: records @p id's state for this ImGui frame.
        // @p visible is Begin's return value.
        void Note(const char* id, bool visible);
        // @p id's last report, or the default (not visible) when it is older than the previous frame.
        WindowFocus State(const char* id) const;
        bool        IsVisible(const char* id) const { return State(id).visible; }
        bool        IsFocused(const char* id) const { return State(id).focused; }

        // ---- Capture ----
        // The window holding the game's mouse capture; empty = none.
        void               SetCapture(const char* id) { m_capture = id ? id : ""; }
        void               ReleaseCapture()           { m_capture.clear(); }
        bool               HasCapture(const char* id) const { return id && m_capture == id; }
        const std::string& Capture() const            { return m_capture; }

        // Test seam: forget every report and the capture.
        void Clear();

    private:
        std::map<std::string, WindowFocus> m_windows;
        std::string                        m_capture;
    };
}
