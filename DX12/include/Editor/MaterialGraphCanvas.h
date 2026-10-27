#pragma once

// MaterialGraphCanvas - the Material Editor's node canvas: one MaterialGraphDocument drawn
// with thedmd/imgui-node-editor (external/imgui-node-editor), and everything the user does
// turned into the document's ops - the vocabulary Editor.MaterialGraphEdit speaks, so a
// scripted edit and a mouse edit take the same path (and the same undo ring):
//
//   * nodes coloured by category, typed pins (the component count's colour), unconnected
//     inputs' values edited in place (a drag is one undo step); a node with an error is
//     outlined red, a warning yellow (the tooltip says which);
//   * a link dragged from an output to an input connects (an input's old link goes); one
//     that would close a cycle, join two outputs or two inputs, or widen a vector (float2
//     into float3) is refused while dragging; dropped on empty space it opens the palette
//     filtered to the node types that can take (or feed) that pin and connects the added
//     node on its first compatible pin; Delete removes links and nodes (never the Output);
//   * a texture node shows its texture (the window's thumbnail source);
//   * the palette (right-click, or Space) adds a node, with its props' defaults, where it
//     opened; double-clicking a link inserts a Reroute; Ctrl+C / X / V / D go through the
//     system clipboard as .imatgraph JSON (they paste across materials); C boxes the
//     selection in a comment; F frames the selection, Home everything;
//   * a node moved by the mouse becomes ONE move batch when the drag ends;
//   * a node's context menu starts / stops previewing it (the window compiles the
//     preview); Hide Unrelated dims what the selection neither reads nor feeds.
//
// Positions: the model's positions are pushed into the editor only when the document's
// revision moves (an op, an undo), so a drag is never fought.
// Settings: no NodeEditor.json (Config::SettingsFile = nullptr) - the graph file stores the
// positions; no smooth zoom (captures are deterministic).
//
// Editor-only TU: compiled into EditorCore.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"

namespace MaterialGraph { struct CompileResult; }

namespace Editor
{
    class MaterialGraphDocument;

    class MaterialGraphCanvas
    {
    public:
        using ThumbnailSource = std::function<std::uint64_t(const std::string& itexPath)>;

        static constexpr float kThumbnailSize = 64.0f;     // canvas units

        struct Stats
        {
            int nodes = 0, links = 0, comments = 0, errors = 0, warnings = 0;
        };

        MaterialGraphCanvas();
        ~MaterialGraphCanvas();
        MaterialGraphCanvas(const MaterialGraphCanvas&) = delete;
        MaterialGraphCanvas& operator=(const MaterialGraphCanvas&) = delete;

        // Destroys the editor context while ImGui is alive (the destructor does too).
        void Shutdown();

        // The canvas, filling the current region. @p compile marks the nodes'
        // diagnostics. @p logName: the [MATGRAPH] canvas line's name.
        void Draw(MaterialGraphDocument& doc, const MaterialGraph::CompileResult& compile, const std::string& logName);
        // The selected node's props (exactly one selected), for the window's side panel.
        void DrawNodePanel(MaterialGraphDocument& doc);

        // Requests, applied at the next Draw (Lua, the harness, the Code tab).
        void RequestFit()                                  { m_fit = true; }
        void RequestFrameSelection()                       { m_frameSelection = true; }
        void RequestSelect(const std::vector<std::uint32_t>& ids) { m_select = ids; m_hasSelect = true; }

        // The previewed node (the window owns it; the canvas badges it) and a "Start / Stop
        // Previewing Node" chosen from a node's context menu.
        void SetPreviewNode(std::uint32_t node)            { m_previewNode = node; }
        bool TakePreviewRequest(std::uint32_t& node, std::string& pin);
        // Hide Unrelated: nodes neither upstream nor downstream of the selection are dimmed.
        void SetHideUnrelated(bool on)                     { m_hideUnrelated = on; }
        bool HideUnrelated() const                         { return m_hideUnrelated; }

        // A texture node's picture: the GPU descriptor of an .itex path, 0 while it streams in
        // (the node keeps a grey square of the same size, so it does not grow when it lands).
        // Resolved every frame, never cached. Unset = none.
        void SetThumbnailSource(ThumbnailSource source)    { m_thumbnails = std::move(source); }

        const std::vector<std::uint32_t>& Selection() const { return m_selection; }
        bool  Hovered() const { return m_hovered; }        // Ctrl+Z goes to the graph's ring
        float Scale() const   { return m_scale; }

        Stats LastStats() const { return m_stats; }

    private:
        void  SyncPositions(const MaterialGraphDocument& doc);
        void  DrawPalettePopup(MaterialGraphDocument& doc);
        // @p pin: the new node's pin the dragged wire connects to ("" = none dragged).
        void  AddFromPalette(MaterialGraphDocument& doc, const char* type, const std::string& pin);

        void*                                   m_ctx = nullptr;     // ax::NodeEditor::EditorContext*
        std::uint64_t                           m_seenRevision = 0;
        std::unordered_map<std::uint32_t, ImVec2> m_pushedPos;       // what the editor was last told
        std::unordered_map<std::uint32_t, ImVec2> m_pushedSize;      // comments: the group area pushed
        std::unordered_map<std::uint32_t, ImVec2> m_commentFrame;    // ... and the title + padding around it
        int                                     m_draws = 0;
        bool                                    m_fit = false, m_frameSelection = false, m_hasSelect = false;
        std::vector<std::uint32_t>              m_select;
        std::vector<std::uint32_t>              m_selection;
        bool                                    m_hovered = false;
        float                                   m_scale = 1.0f;
        // The palette: where it opened (canvas units), the pin a link was dragged from.
        bool                                    m_openPalette = false;
        ImVec2                                  m_paletteAt = ImVec2(0, 0);
        std::uint64_t                           m_paletteFromPin = 0;
        char                                    m_search[64] = {};
        std::uint64_t                           m_contextNode = 0, m_contextLink = 0;
        Stats                                   m_stats;
        std::string                             m_logged;            // the last [MATGRAPH] canvas line's content
        std::uint32_t                           m_previewNode = 0;
        bool                                    m_previewRequested = false;
        std::uint32_t                           m_previewRequestNode = 0;
        std::string                             m_previewRequestPin;
        bool                                    m_hideUnrelated = false;
        std::vector<char>                       m_codeBuf;           // a Custom node's code while it is edited
        std::uint32_t                           m_codeNode = 0;
        bool                                    m_codeEditing = false;
        ThumbnailSource                         m_thumbnails;        // texture nodes' pictures
    };
}
