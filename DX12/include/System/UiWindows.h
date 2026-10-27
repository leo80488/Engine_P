#pragma once
// Where the editor's panels are, for code that cannot see ImGui. EngineCore is compiled
// once for both exes and Game.exe links no ImGui, so the editor (EditorLayer) registers a
// provider listing the top-level windows ImGui laid out this frame; HarnessCaptureSystem
// samples it for an app_ui capture, in the frame the picture is of, to name the panels and
// crop one. Game.exe registers nothing: Sample() is empty.
// Header-only: one inline slot, no project registration, free in Retail (never sampled).

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Sys::UiWindows
{
    struct Window
    {
        std::string name;      // ImGui's full name ("Output Log###OutputLog")
        std::string label;     // what its title shows: the name before "##" ("Output Log")
        float       x = 0.f, y = 0.f, w = 0.f, h = 0.f;   // client pixels of the main window
        int         order = 0;        // back to front: a larger order is drawn over a smaller one
        bool        focused = false;  // has keyboard focus
        bool        collapsed = false;
        bool        docked = false;
        // Hides what is under it; false for an overlay that draws no background or takes no
        // mouse input (ImGuizmo's full-window "gizmo").
        bool        background = true;
    };

    using Provider = std::function<std::vector<Window>()>;

    inline Provider& Slot()
    {
        static Provider s_provider;
        return s_provider;
    }
    // The editor's (main thread; replaces any earlier one; an empty one clears).
    inline void SetProvider(Provider p) { Slot() = std::move(p); }
    // Main thread, after the frame's UI was built (PostRender).
    inline std::vector<Window> Sample()
    {
        const Provider& p = Slot();
        return p ? p() : std::vector<Window>{};
    }
}
