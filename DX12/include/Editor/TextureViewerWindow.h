#pragma once

// TextureViewerWindow - the Texture Viewer: look at one .itex the way UE's
// texture editor and RenderDoc's texture viewer do - R / G / B / A alone or
// together, any mip, a cube's faces (one, the cross, the panorama), an HDR
// exposure, a normal map's rebuilt Z, a value range, a comparison with a second
// picture - beside what the file is (format, size, mips and their bytes, GPU
// memory, the import settings, who uses it).
//
// The picture is a Renderer texture view (Renderer/TextureViewService.h), not
// ImGui's shader and not the preview layer: a panel-owned RGBA16F target that
// TextureViewPass paints on the restore list only when something changed, drawn
// here with ImGui::Image 1:1. Only the active tab owns one; a closed window owns
// none. A tab holds a TextureSystem reference to its texture while open, so it
// stays resident and a reimport (same handle, new contents) redraws the picture.
//
// Threads: Draw is a UI handler (a render-side context) that only edits the
// tab's view state and records requests. ProcessPendingActions is the game
// half's serial window: opens / closes (TextureSystem Acquire / Release), the
// view's Desc (sent every frame from there, so a harness run with no laid-out
// window still draws), the harness steps and the shots.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "Renderer/TextureView.h"
#include "Resource/SystemHandles.h"
#include "Resource/TextureAnalysis.h"
#include "Resource/TextureImportRecord.h"
#include "Resource/TextureInfo.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Renderer;
class IGraphicsDevice;
namespace Resource { class TextureSystem; class ResourceManager; }

namespace Editor
{
    struct TextureViewerContext
    {
        Renderer*                  renderer  = nullptr;
        IGraphicsDevice*           gfx       = nullptr;
        Resource::TextureSystem*   textures  = nullptr;
        Resource::ResourceManager* resources = nullptr;
        std::uint64_t              editorFrame = 0;
        // The files that reference @p path (the Content Browser's reference
        // index); null = unknown. Main thread.
        std::function<void(const std::string&, std::vector<std::string>&)> referencingFiles;
        // Show a file in the Content Browser.
        std::function<void(const std::string&)> reveal;
        // Imports @p itex again from @p source with @p settings
        // (EditorLayer::ImportTextureFile: writes the record, swaps the texture in
        // place); false and @p why on failure. Called from ProcessPendingActions.
        std::function<bool(const std::string& itex, const std::string& source,
                           const Resource::TextureImportSettings& settings, std::string& why)> reimport;
    };

    class TextureViewerWindow
    {
    public:
        static constexpr int kMaxTabs = 8;

        // What one tab shows (the Lua / harness surface too).
        struct ViewState
        {
            std::uint32_t            channels   = TextureView::kChRGBA;
            std::uint32_t            mip        = 0;
            std::uint32_t            slice      = 0;     // array slice / 3D slice / cube face
            TextureView::CubeLayout  cubeLayout = TextureView::CubeLayout::Face;
            TextureView::Map         map        = TextureView::Map::Stored;
            float                    ev         = 0.f;
            bool                     tonemap    = true;
            float                    rangeMin   = 0.f, rangeMax = 1.f;
            TextureView::Background  background = TextureView::Background::Checker;
            bool                     filtered   = false;
            bool                     tile       = false;
            // Zoom in output pixels per mip-0 texel; 0 = fit (recomputed from the
            // canvas every frame until the user zooms or pans).
            float                    zoom       = 0.f;
            float                    pan[2]     = { 0.f, 0.f };   // the image's top-left in the canvas, pixels
            // Compare with a picture B - this texture's next mip (compareWith empty: mip N
            // against N+1), or another open tab's texture at the same mip. Split: A left of
            // `split` (a fraction of the output), B right of it; Diff: |A - B| x diffScale.
            TextureView::Compare     compare    = TextureView::Compare::Off;
            std::string              compareWith;
            float                    split      = 0.5f;
            float                    diffScale  = 1.f;
        };

        struct TabState
        {
            std::string   path;
            ViewState     view;
            Resource::TextureFileInfo info;
            bool          resident = false;        // the GPU copy is ready
            std::string   residentWhy;             // Loading / the failure reason
            std::uint32_t viewId = ~0u;
            std::uint64_t renders = 0;
            bool          settled = false;         // drawn with the current desc, and quiet since
            std::uint32_t outW = 0, outH = 0;      // the output target
            std::uint64_t gpuBytes = 0;
        };
        struct StateView
        {
            bool                  open = false;
            int                   active = -1;
            std::vector<TabState> tabs;
        };

        // ---- What a texel is ----
        // The GPU probe: texel (x, y) of the active tab's shown mip (a cube's face
        // @p face, else the tab's) read by the texture view's own pass - the value the
        // shaders see - plus the CPU's stored value and raw bytes
        // (Resource::ReadStoredTexel). Lua Editor.TextureViewerProbe: a job that answers
        // once the GPU value for exactly these coordinates came back.
        struct ProbeReport
        {
            bool          gpuValid = false;
            float         gpu[4] = { 0.f, 0.f, 0.f, 0.f };
            std::uint32_t x = 0, y = 0, mip = 0, face = 0;
            bool          sideB = false;             // read from the comparison's picture B
            std::string   path;                      // the texture it was read from
            Resource::StoredTexel stored;
        };

        // ---- Lifetime ----
        static TextureViewerWindow& Get();

        void Draw(const TextureViewerContext& ctx);
        void ProcessPendingActions(const TextureViewerContext& ctx);   // game half, serial window
        void ReleaseAtShutdown(const TextureViewerContext& ctx);
        bool  IsOpen() const { return m_open; }
        bool* OpenFlag()     { return &m_open; }

        // ---- Tabs ----
        // From a UI handler: queued, opened in ProcessPendingActions.
        void RequestOpen(const std::string& path);
        // Opens it with its Import section unfolded (the Content Browser's Import
        // Settings...).
        void RequestImportSettings(const std::string& path);
        // Game half (Lua, harness): opens now; an open tab of the same file is focused.
        // Refused past kMaxTabs, or when the path is not a readable .itex.
        bool OpenNow(const TextureViewerContext& ctx, const std::string& path, std::string& why);
        bool CloseTab(const TextureViewerContext& ctx, int tab);   // -1 = the active one
        void CloseAll(const TextureViewerContext& ctx);
        bool Activate(int tab);
        int  ActiveTab() const { return m_active; }
        int  TabCount()  const { return static_cast<int>(m_tabs.size()); }

        // ---- Scripted surface (Lua Editor.TextureViewer*, the harness) ----
        // One key of the active tab: ch / mip / face / slice / layout / map / ev /
        // tonemap / range ("a:b") / bg / zoom ("fit" | N) / pan ("x:y") /
        // filtered / tile / size ("WxH", 0x0 = follow the canvas).
        bool Set(const std::string& key, const std::string& value, std::string& why);
        // A spec "key=value;key=value" (the -texviewer_view step) - all or nothing.
        bool ApplySpec(const std::string& spec, std::string& why);
        // Pin the output size (harness determinism, -texviewer_size); 0 = the canvas.
        void PinSize(std::uint32_t w, std::uint32_t h) { m_pinW = w; m_pinH = h; }

        StateView State(const TextureViewerContext& ctx) const;
        // Writes the active tab's picture (its output target) to a PNG; serial window
        // only (CaptureTextureToPNG syncs). False with @p why.
        bool WriteShot(const TextureViewerContext& ctx, const std::string& path, std::string& why);
        // The active tab's picture has been drawn with its current description
        // and nothing has redrawn it for a few frames: a shot now is final.
        bool Settled() const;
        // Lua Editor.TextureViewerShot: a job (Harness/ControlChannelJobs.h) that
        // writes the active tab's picture once it has settled (in ProcessPendingActions),
        // or fails after kShotDeadline editor frames. 0 in Retail (no jobs).
        std::uint64_t QueueShot(const std::string& path, std::uint32_t requestId);

        // ---- What a texel is ----
        // @p sideB: a texel of the comparison's picture B (its shown mip).
        std::uint64_t QueueProbe(std::uint32_t x, std::uint32_t y, int face, std::uint32_t requestId,
                                 bool sideB = false);
        // Statistics of the active tab's (mip, slice) (Resource::AnalyzeTexture) on a
        // worker; Lua Editor.TextureViewerStats is a job answering with them.
        std::uint64_t QueueStats(const Resource::AnalyzeOptions& o, std::uint32_t requestId);
        // The last finished statistics of the active tab (null while none / computing).
        const Resource::TextureStats* Stats() const;

        // ---- Harness ----
        bool HarnessConfigure(std::string& why);   // argv time; false = exit 12
        bool HarnessFailed() const { return m_harnessFailed; }

    private:
        struct Tab
        {
            std::string                path;
            std::string                name;
            Resource::TextureHandle    handle = Resource::kInvalidTextureHandle;
            Resource::TextureFileInfo  info;
            ViewState                  view;
            bool                       userMoved = false;   // zoomed / panned: stop fitting
            // Who uses it (refreshed on open and on demand).
            std::vector<std::string>   files;
            std::uint32_t              entities = 0;
            bool                       usersKnown = false;
            std::string                sourceFile;           // a same-named png / jpg / tga / hdr / dds
            // TextureSystem::Info::revision last seen: a reimport (same handle, new
            // contents - maybe a new size or format) re-reads the file's description.
            std::uint64_t              revision = 0;
            // How the file was made (its .meta record, or the guess) and the Import
            // section's edits of it; re-read after the file changes.
            bool                          importLoaded = false;
            Resource::TextureImportRecord importRecord;
            Resource::TextureImportSettings importEdit;
            std::string                   importSource;
            bool                          importSourceThere = false;
            std::string                   importStatus;       // the last Reimport's outcome
            bool                          importFailed = false;
        };

        struct PendingReimport
        {
            std::string                     path, source;
            Resource::TextureImportSettings settings;
        };

        struct Step
        {
            int           kind = 0;
            std::uint64_t at = 0;
            std::string   text;
            bool          done = false;
        };

        struct PendingShot
        {
            std::string   path;
            std::uint64_t job = 0;
            std::uint64_t queuedAt = 0;   // editor frame
        };

        struct StatsTask
        {
            std::string              path;
            std::uint64_t            revision = 0;
            Resource::AnalyzeOptions options;
            Resource::TextureStats   result;
            std::atomic<bool>        finished{ false };
            std::vector<std::uint64_t> jobs;         // Lua jobs waiting for it
            bool                     harness = false; // -texviewer_stats: log the [TEXVIEWER] stats line
        };

        struct PendingProbe
        {
            std::uint32_t x = 0, y = 0;
            int           face = -1;
            std::uint64_t job = 0;
            std::uint64_t queuedAt = 0;
            bool          harness = false;           // log the [TEXVIEWER] probe line
            bool          sideB = false;             // picture B's texel
        };

        Tab*       ActiveTabPtr();
        const Tab* ActiveTabPtr() const;
        // Picture B of @p a's comparison: the tab it reads (@p a itself for its next
        // mip) and B's mip in @p bMip; null when it compares nothing (off, or B's tab is
        // gone).
        const Tab* CompareTab(const Tab& a, std::uint32_t& bMip) const;
        // The desc the active tab asks for this frame (canvas or pinned size).
        bool       BuildDesc(const TextureViewerContext& ctx, Tab& t, TextureView::Desc& out) const;
        // The picture's extent in mip-0 texels (a cross is 4x3 faces, a panorama 2:1).
        void       ImageExtent(const Tab& t, float& w, float& h) const;
        void       FitView(Tab& t, float canvasW, float canvasH) const;
        void       ReleaseView(const TextureViewerContext& ctx);
        void       RefreshUsers(const TextureViewerContext& ctx, Tab& t);
        void       DrawToolbar(Tab& t);
        void       DrawCanvas(const TextureViewerContext& ctx, Tab& t, float width, float height);
        void       DrawInfo(const TextureViewerContext& ctx, Tab& t);
        void       DrawImport(const TextureViewerContext& ctx, Tab& t);
        static void LoadImport(Tab& t);                                    // the import record into the tab
        void       TickHarness(const TextureViewerContext& ctx);

        static std::string StatsJson(const Resource::TextureStats& s);

        void              TickStats(const TextureViewerContext& ctx);
        void              StartStats(const Resource::AnalyzeOptions& o, std::uint64_t job);
        void              DrawStats(Tab& t);

        void              TickProbes(const TextureViewerContext& ctx);
        void              LogProbe(const ProbeReport& r) const;

        bool                     m_open = false;
        std::vector<Tab>         m_tabs;
        int                      m_active = -1;
        int                      m_focusTab = -1;          // select this tab on the next Draw
        std::vector<std::string> m_pendingOpen;             // RequestOpen
        std::string              m_focusImport;             // unfold this tab's Import section
        int                      m_focusImportFrames = 0;   // ... and scroll to it, for a few frames

        // A Reimport the Import section asked for (run in ProcessPendingActions).
        std::vector<PendingReimport> m_pendingReimports;
        std::vector<int>         m_pendingClose;            // a tab's x (by index at the time)
        std::uint32_t            m_viewId = ~0u;            // the active tab's texture view
        int                      m_viewTab = -1;            // which tab owns it
        std::uint64_t            m_lastRenders = 0;
        std::uint64_t            m_lastRendersFrame = 0;
        std::uint64_t            m_lastDescHash = 0;
        std::uint64_t            m_descChangedFrame = 0;
        // The canvas the last Draw laid out (0 = never drawn); the pinned size wins.
        float                    m_canvasW = 0.f, m_canvasH = 0.f;
        std::uint32_t            m_outW = 0, m_outH = 0;    // quantised, debounced
        float                    m_sizeChangedAt = -1.f;
        std::uint32_t            m_pinW = 0, m_pinH = 0;
        // Hover read-out (Draw): the texel under the cursor, -1 = none.
        int                      m_hoverX = -1, m_hoverY = -1;
        float                    m_hoverU = 0.f, m_hoverV = 0.f;
        bool                     m_hoverSideB = false;      // the cursor is over picture B (split)
        bool                     m_splitDrag  = false;      // the split line is being dragged
        std::uint64_t            m_frame = 0;               // editor frame of the last ProcessPendingActions

        // Harness (-texviewer_*): the subject, its steps and the reverse arms.
        std::string       m_harnessOpen;               // the flag's value (a comma list)
        std::vector<std::string> m_harnessOpenList;    // every subject, the first one active
        bool              m_harnessOpened = false;
        bool              m_harnessFailed = false;
        bool              m_harnessChannelOff = false;   // reverse arm: the channel mask is ignored
        bool              m_harnessMipOff = false;       // reverse arm: always mip 0
        bool              m_harnessFaceOff = false;      // reverse arm: always face 0, face layout
        bool              m_harnessMapOff = false;       // reverse arm: every mapping is Stored
        bool              m_harnessTrustHeader = false;  // reverse arm: the dimension from TextureMetadata
        bool              m_harnessRedrawAll = false;    // reverse arm: the picture is redrawn every frame
        bool              m_harnessCompareOff = false;   // reverse arm: picture B is never drawn
        std::vector<Step> m_steps;

        std::vector<PendingShot> m_shots;

        // ---- Texel probe and statistics ----
        // The hover probe (Draw records it, ProcessPendingActions arms / stops the view's
        // probe).
        bool              m_hoverProbe = false;
        std::uint32_t     m_hoverTexX = 0, m_hoverTexY = 0;
        int               m_hoverFace = -1;
        ProbeReport       m_probe;                   // the last one the status line shows
        std::string       m_storedKey;               // path|rev|mip|face|x|y of m_probe.stored

        // Statistics: one worker task at a time; the result keyed by what it measured.
        std::shared_ptr<StatsTask> m_statsTask;      // running
        std::shared_ptr<StatsTask> m_statsDone;      // the last finished
        // Requests made while one runs (a job id, or 0 = a harness step), in order.
        std::vector<std::pair<Resource::AnalyzeOptions, std::uint64_t>> m_statsQueue;

        bool              m_statsAuto = true;        // recompute when the mip / slice changes
        bool              m_histLog = false;
        int               m_histDragFrom = -1;       // a drag over the histogram sets the Range

        std::vector<PendingProbe> m_probes;          // scripted / harness probes, in order
    };
}
