#pragma once

// RenderGraphWindow - Window > Analysis > Render Graph: the main render graph's
// latest frame capture, read-only - a navigator of passes and textures, the Graph /
// Resources / Frame tabs and an Inspector of what is selected.
//
// THE DATA. RG::FrameCaptures() (RenderGraph/FrameCapture.h): the main graph's
// snapshot of one frame joined with the device's journal of the same frame. A
// capture is built only when someone asks, so the window asks only while it is
// open - once on opening, then every half second while Live is on (Refresh asks
// once; Freeze keeps the one shown). Closed, it asks for nothing and holds
// nothing. Each new capture (the board's sequence) becomes one Model the panes
// draw from; [RG_PANEL] frame= seq= ... is logged once per capture shown - the
// proof of life the rg_panel pack reads.
//
// THREADS. Draw is a UI handler: it reads the thread-safe board and edits the
// window's own state. The only device call is the GPU-times want (the Profiler
// window's mechanism), taken while "GPU times" is ticked.
//
// THE HARNESS. -ui_open_menu=Render Graph@<frame> opens it, Render
// Graph/Graph, /Resources or /Frame also selects that tab (Context::held is
// EditorLayer::HarnessMenuHeldOpen); a later entry that takes the hold (any
// other window) closes it, so a run can show that a closed panel asks for no
// capture (the rg_panel pack, test_rg_panel.py).
//
// THE PREVIEW: the texture Inspector shows the texture as the frame left it
// after a chosen pass (the frame's end by default) - the Renderer's pass capture
// (Renderer/PassCapture.h) copies it in the graph's own chain and paints a
// picture of the copy with the debug target shader; mode / channel / plane /
// range as Render.target takes them; Capture asks once, Live with the panel's
// refresh. Closed, or no texture shown: released.
// -rgpanel_preview=<pass|end> asks for the -rgpanel_pick texture's preview.
//
// SAVE JSON (harness builds only): the capture shown, written by the -rgdump
// builder (Harness/RenderGraphDump.h) to <DiagnosticsDir>/rg/rg_<frame>.json
// - the artifact folder of a harness run, WritableDataDir() otherwise - so
// tools/rg_report.py reads it as it reads a -rgdump file or `rg full`.
// -rgpanel_save does it once, on the first capture shown.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore. It reads the
// capture's structs; only Save JSON calls the harness's JSON builder, so Retail
// builds it without the button.

#include <cstdint>
#include <functional>
#include <array>
#include <memory>
#include <string>
#include <vector>

class IGraphicsDevice;
class Renderer;
namespace RG { struct FrameCapture; }

namespace Editor
{
    class RenderGraphWindow
    {
    public:
        struct Context
        {
            IGraphicsDevice*                 gfx = nullptr;
            std::function<bool(const char*)> held;   // the harness's -ui_open_menu hold
            Renderer*                        renderer = nullptr;   // the pass capture (the texture preview)
        };

        struct Model;   // RenderGraphWindow.cpp: one per capture shown

        static RenderGraphWindow& Get();

        bool IsOpen() const { return m_open; }
        void SetOpen(bool open) { m_open = open; }
        // Every frame, open or not (a harness hold opens it; closing releases).
        void Draw(const Context& ctx);
        // The GPU-times want goes back and the capture is dropped.
        void Shutdown(IGraphicsDevice* gfx);

    private:
        enum class Tab : uint8_t { Graph, Resources, Frame };
        enum class Pick : uint8_t { None, Pass, Texture, List };

        void Refresh();
        void Build(std::shared_ptr<const RG::FrameCapture> cap);
        void DrawToolbar(const Context& ctx);
        void DrawNavigator();
        void DrawTabs(const Context& ctx);
        void DrawPassList();
        void DrawGraph();
        void DrawResources();
        void DrawFrame();
        void DrawInspector();
        void InspectPass(uint32_t index);
        void InspectTexture(uint32_t index);
        void InspectList(uint32_t slot);
        void SetGpuWant(IGraphicsDevice* gfx, bool want);
        void SaveJson();   // harness builds only
        void DrawPreview(uint32_t texture);    // the texture Inspector's
        void ReleasePreview();

        bool     m_open = false;
        bool     m_heldLast = false;   // a harness hold, last frame
        bool     m_live = true;
        bool     m_frozen = false;
        bool     m_refreshNow = false;
        bool     m_pending = false;
        bool     m_gpuTimes = false;
        bool     m_gpuWant = false;
        bool     m_showInactive = true;
        double   m_lastRequest = -1.0;
        double   m_arrived = 0.0;
        uint64_t m_requestedAt = 0;   // the board's sequence when we asked
        Tab      m_tab = Tab::Graph;
        Pick     m_pick = Pick::None;
        uint32_t m_pickIndex = 0;
        char     m_filter[64] = {};
        std::shared_ptr<Model> m_model;
        // The Graph tab: the node editor's context (created on first use, destroyed
        // on close), the toggles, the layout the nodes were last placed by, the pass
        // last pushed into the editor's selection.
        void*    m_nodeContext = nullptr;
        bool     m_listView = false;
        bool     m_showWar = false;
        bool     m_showWaw = false;
        bool     m_showOrder = true;
        int      m_texFilter = -1;   // -1 = every texture's edges
        uint64_t m_placedLayout = 0;
        bool     m_fit = true;
        int64_t  m_syncedPick = -1;
        // A node's size is known only after it is drawn: fit / go to the
        // selection this many frames later.
        int      m_fitDelay = 0;
        int      m_navDelay = 0;
        // The Graph tab's own Inactive (off: only the passes that ran, laid out on this
        // frame's DAG - the static DAG's HDR chain is ~30 layers long).
        bool     m_graphInactive = false;
        // The Frame tab's toggles and list width; then the Graph tab's node layout
        // (m_layoutKey / m_layout), keyed by the pass list and the drawn set.
        bool     m_showWaits = true;
        bool     m_showHandoffs = true;
        float    m_boxWidth = 110.f;
        bool     m_fitLists = true;   // the list width that fits the tab's width
        uint64_t m_layoutKey = 0;
        std::vector<std::array<float, 2>> m_layout;
        std::string m_savedPath;   // the last Save JSON's file
        // The texture Inspector's preview: what it shows, how it is painted, and the
        // last request (its id is the pass capture's).
        Renderer*   m_renderer = nullptr;          // Context::renderer, this frame
        std::string m_previewTexture;              // "" = none asked
        std::string m_previewAfter;                // a pass's name; "" = the frame's end
        int         m_previewMode = 0;             // DebugTargets::Mode (0 = Auto)
        int         m_previewChannel = -1;         // -1 = the texture's own
        int         m_previewPlane = 0;            // a depth-stencil's (1 = stencil)
        bool        m_previewRange = false;        // min / max set here
        float       m_previewMin = 0.f, m_previewMax = 1.f, m_previewExposure = 1.f;
        bool        m_previewLive = false;
        bool        m_previewAsk = false;          // ask at the next draw
        bool        m_previewDrawn = false;        // drawn this frame
        uint64_t    m_previewId = 0;               // the last request's (0 = none out)
        uint64_t    m_previewNextId = 1;
        double      m_previewAskedAt = -1.0;
        uint64_t    m_previewLoggedSeq = 0;        // [RG_PANEL] preview: the last result logged
    };
}
