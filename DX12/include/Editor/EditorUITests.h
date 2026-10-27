#pragma once

// EditorUITests - the editor's UI tests on Dear ImGui Test Engine (external/imgui_test_engine): real
// key presses, clicks and drags against the editor's windows, then a check of the editor's state.
// The engine exists only once a run asks for tests (Lua Editor.RunUITests); before that ImGui's
// hooks find no engine and cost a pointer test per item. Editor.exe only, main thread (Reverted: any).

#include <cstdint>
#include <string>
#include <vector>

class EditorLayer;

namespace Editor::UITests
{
    struct Result
    {
        std::string category;
        std::string name;
        bool        passed = false;
        std::string status;   // the test engine's status name ("Success", "Error", ...)
    };

    // Queues the registered tests whose "category/name" matches @p filter (the test engine's syntax:
    // a substring, "^" anchors, "-" excludes; "" = all), starting the engine on the first call.
    // @p junitPath (optional) receives the run's JUnit XML; @p scratchDir is where tests copy the
    // fixtures they edit. False with @p why while a run is still going or when nothing matches.
    bool Queue(EditorLayer& layer, const std::string& filter, const std::string& junitPath,
               const std::string& scratchDir, std::uint64_t jobId, std::string& why);
    bool Running();
    // After ImGui::Render each frame: the engine's PostSwap, and the end of a run once its queue is
    // empty (the job then succeeds or fails with the results).
    void PostFrame();
    const std::vector<Result>& LastResults();

    // Shutdown: Stop before ImGui::DestroyContext, Destroy after it (the engine's own order).
    void StopEngine();
    void DestroyEngine();

    // ---- Reverse arms ----
    // A fix a UI test guards. -uitest_reverse=<name> (harness) puts one back the way it was, so that
    // test can be shown to fail without it (tests/unit/test_editor_ui_tests.py).
    enum class Fix
    {
        None,
        InspectorDragBracket,   // inspector.drag_bracket: one undo step per held gesture
        TabCloseAsks,           // material.tab_close_asks: a dirty tab's x asks first
        LayoutReset,            // layout.reset: Reset Layout rebuilds a tree imgui.ini restored
        PopupOwnsKeys,          // commands.popup_owns_keys: no chord while a popup is open
        TextFieldKeepsKeys,     // commands.text_field_keeps_keys: no chord while a text field types
        SavePoint,              // history.save_point: an undo back to the save point is clean
    };
    // True when this run reverts @p fix; the call site then takes the path the fix replaced.
    bool Reverted(Fix fix);
}
