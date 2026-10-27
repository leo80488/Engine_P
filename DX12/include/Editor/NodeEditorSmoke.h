#pragma once

// NodeEditorSmoke - the node-editor library's acceptance run (thedmd/imgui-node-editor,
// vendored in external/imgui-node-editor with the patches its README lists).
//
// Harness-only (-nodeeditor_smoke=<nodes>): a window with N nodes in a grid, each with an
// input and an output pin, chained by N-1 links. It measures:
//   * the CPU cost of one frame of the editor (ed::Begin .. ed::End), p50 / p99;
//   * that the canvas leaves no draw callback of its own behind (a leftover one forces
//     the render owner to finish before the next simulation - the frame-overlap cost
//     ImGuiManager guards);
//   * text at a zoomed-in view (patch 2: glyphs baked at the view's density) and a
//     zoomed-out one, via -nodeeditor_smoke_zoom=<fit|in>@<frame>;
//   * determinism: smooth zoom and link flow off, no settings file - two captures of an
//     unchanged view are the same pixels.
// One summary line at -nodeeditor_smoke_report=<frame>:
//   [NODEEDITOR] smoke nodes= links= frames= msP50= msP99= msMax= scale= density= leftoverCallbacks=
// -nodeeditor_smoke_density_off is patch 2's reverse arm (text baked at the base size).
//
// Editor-only TU: compiled into EditorCore.

#include <cstdint>
#include <string>
#include <vector>

namespace Editor
{
    class NodeEditorSmoke
    {
    public:
        static NodeEditorSmoke& Get();
        ~NodeEditorSmoke();

        // argv: -nodeeditor_smoke=<nodes> [-nodeeditor_smoke_zoom=<fit|in>@<frame>,...]
        // [-nodeeditor_smoke_report=<frame>]. False + why = a malformed value (exit 12).
        bool HarnessConfigure(std::string& why);
        bool Enabled() const { return m_nodes > 0; }
        // The UI pass: the window (nothing when not enabled).
        void Draw(std::uint64_t editorFrame);
        // Editor shutdown, while ImGui is alive (the context's teardown touches it).
        void Shutdown();

    private:
        struct ZoomStep { std::string mode; std::uint64_t at = 0; bool done = false; };

        NodeEditorSmoke() = default;

        int                     m_nodes = 0;
        std::vector<ZoomStep>   m_zoom;
        std::uint64_t           m_reportAt = 0;
        bool                    m_reported = false;
        void*                   m_ctx = nullptr;   // ax::NodeEditor::EditorContext*
        bool                    m_placed = false;
        int                     m_drawn = 0;       // frames the editor drew
        std::vector<double>     m_ms;              // per-frame editor cost, after the warm-up
        int                     m_leftover = 0;    // canvas callbacks left in the draw list
        float                   m_scale = 1.0f;     // the view's scale (screen px per canvas unit)
        std::string             m_zoomLabel = "default";
    };
}
