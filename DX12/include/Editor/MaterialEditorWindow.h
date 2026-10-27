#pragma once

// MaterialEditorWindow - the Material Editor: one .imat / .imatinst per tab, edited the way
// UE's Material (Instance) Editor edits one - a preview of the material as edited, every
// control of the Inspector's material panel, its own undo, and Save / Save As / Save as
// Instance / Revert.
//
// A TAB IS A SESSION (Editor/MaterialEditSession.h): the staged material, its override set,
// the dirty mark, a 64-step undo ring. The panel is the Inspector's
// (Editor::MaterialPropertyPanel) in asset mode; its edits reach the session through
// MaterialPanelContext::onEdit, so in an INSTANCE an edited field joins the override set by
// construction. Ctrl+Z / Ctrl+Y while the window is focused step the ACTIVE tab's history,
// never the scene's.
//
// THE PREVIEW is this window's own MaterialPreviewWidget (not the Inspector's singleton):
// only the active tab is previewed, so the window holds one of the preview layer's four
// views, released at the frame boundary when the window stops drawing. -mateditor_size /
// -mateditor_view reach the widget's harness setters.
//
// LIVE: a dirty tab's staged material is its file's live version, so the scene shows it
// before Save (SyncLive). ENTITY TABS: OpenEntityNow puts one entity's own material in a
// tab; its edits go straight into the entity as its MaterialOverride, one step of the
// scene's undo history per gesture.
//
// THREADS. Draw is a UI handler: it edits the staged material (the panel) and LATCHES
// everything that touches files or other systems (opens, closes, saves, reverts, undo
// requests). ProcessPendingActions (PPA, the game half's serial window) applies them: it
// writes the file, pushes the save to every entity using it
// (MaterialEditorContext::materialSaved), reloads tabs whose file or parent changed, and
// runs the -mateditor_* steps.
//
// GRAPH TABS: a material whose .imat names a materialGraph gets its graph
// (Editor/MaterialGraphDocument) beside its session; CreateGraph gives one to a standard
// material. The graph has its own undo ring. Save of a graph tab is UE's Apply: compile,
// write the generated HLSL (only when its bytes change) and the graph, point the material at
// them (useCustomShader, customShaderPath, materialGraph, the fixed textures' ctex_), then
// save the .imat; the file watcher hot-reloads the shader for every material using it. A
// graph that does not compile is not saved.
//
// Editor-only TU: compiled into EditorCore.

#include "ECS/ECS.h"                         // EntityHandle
#include "ECS/Guid.h"
#include "Editor/MaterialEditSession.h"
#include "Editor/MaterialGraphCanvas.h"
#include "Editor/MaterialGraphDocument.h"
#include "Editor/MaterialPropertyPanel.h"
#include "Resource/SystemHandles.h"          // Resource::TextureHandle (node thumbnails)

#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Renderer;
class World;
class IGraphicsDevice;
namespace Resource { class TextureSystem; class ResourceManager; }

namespace Editor
{
    class MaterialPreviewWidget;

    struct MaterialEditorContext
    {
        Renderer*     renderer    = nullptr;
        World*        world       = nullptr;
        std::uint64_t editorFrame = 0;
        // A texture node's thumbnail streams through these, as the Content Browser's tiles do.
        // Null = no thumbnails (the node keeps its grey square).
        IGraphicsDevice*           gfx       = nullptr;
        Resource::TextureSystem*   textures  = nullptr;
        Resource::ResourceManager* resources = nullptr;
        // A material file was written: every entity whose material reads it (and
        // every instance of it) re-applies it, and its thumbnails re-tile
        // (EditorLayer: ReloadMaterialsAffectedBy + InvalidateMaterialThumbnails).
        std::function<void(const std::string&)> materialSaved;
        // A path's live version changed or @p ended (Resource/MaterialLiveOverlay.h): the entities
        // reading it re-materialize (ReloadMaterialsAffectedBy, live); at the end, the thumbnails
        // that may have been composed from it re-tile.
        std::function<void(const std::string& path, bool ended)> liveChanged;
        // Harness (-mateditor_assign): points the scene entity named @p entity at the material
        // @p path (EditorLayer::AssignMaterial). False with @p why.
        std::function<bool(const std::string& entity, const std::string& path, std::string& why)> assign;
        // Entity tabs (EditorLayer): writes @p staged into the entity - its MaterialComponent, and
        // its MaterialOverride re-derived against its file. @p first / @p last bracket ONE step of
        // the scene's undo history per gesture. False = the entity is gone.
        std::function<bool(const EntityHandle& e, const MaterialComponent& staged, bool first, bool last)> entityWrite;
        // "promote" (its material as a new .imatinst at @p path, re-pointed),
        // "apply" (its overrides written into its asset file), "clear" (dropped).
        std::function<bool(const EntityHandle& e, const std::string& action, const std::string& path,
                           std::string& why)> entityAction;
        // Show a file in the Content Browser.
        std::function<void(const std::string&)> reveal;
    };

    class MaterialEditorWindow
    {
    public:
        static constexpr int kMaxTabs = 8;

        struct TabState
        {
            std::string              path, parent, kind;
            bool                     dirty = false, instance = false, explicitFormat = false;
            std::vector<std::string> overrides, unset;   // sorted
            std::size_t              undo = 0, redo = 0;
            bool                     diskChanged = false;
            bool                     live = false;       // the scene sees its unsaved edits
            EntityHandle             entity = NullEntityHandle;   // an entity tab's entity
            std::string              graph;              // the tab's graph ("" = none)
            bool                     graphDirty = false;
            // Where rays get its surface (they never run a custom shader). shadowMaskGap: alpha-tested
            // with a custom / graph mask and no engine slot holding it, so the RT shadow is the whole
            // polygon; mirrorMask: the texture Details can bind to OPACITYMAP.
            std::string              rtProxy;
            bool                     shadowMaskGap = false;
            std::string              mirrorMask;
        };
        struct StateView
        {
            bool                  open = false;
            int                   active = -1;
            std::uint32_t         viewId = ~0u;          // the preview's view (the active tab's)
            std::uint64_t         livePushes = 0;        // live versions pushed to the scene, this session
            std::vector<TabState> tabs;
        };

        struct GraphLiveView
        {
            bool        active = false;     // the preview reads the edit file
            bool        pending = false;    // DXC is running on a worker
            std::uint64_t built = 0;        // the graph revision last compiled (settled: == the graph's revision, !pending)
            std::string edit;               // <Generated>/.edit/<graph>.hlsl
            int         compiles = 0, failures = 0;
            double      lastMs = 0.0;       // the last worker compile (every permutation it built)
            std::size_t dxcErrors = 0;
            std::vector<std::pair<std::uint32_t, std::string>> dxc;   // DXC's errors: node (0 = none), message
        };

        static MaterialEditorWindow& Get();

        MaterialEditorWindow();
        ~MaterialEditorWindow();

        // ---- Lifetime ----
        void Draw(const MaterialEditorContext& ctx);
        void ProcessPendingActions(const MaterialEditorContext& ctx);   // GAME half, serial window
        void ReleaseViewsAtShutdown(Renderer* renderer);
        bool  IsOpen() const { return m_open; }
        bool* OpenFlag()     { return &m_open; }
        // The menu's close: closes, or asks first when a tab has unsaved edits.
        void RequestClose();

        // ---- Tabs ----
        void RequestOpen(const std::string& path);     // UI handler: opened in PPA
        // GAME half (Lua, harness): opens now. An open tab of the file is focused.
        bool OpenNow(const std::string& path, std::string& why);
        // @p e's own material in a tab (kind "entity"); an open one is focused. Call from any
        // thread that may touch @p world (the Inspector's row, Lua, PPA).
        bool OpenEntityNow(World& world, Entity e, std::string& why);
        // The active tab's entity (false: no tab, or a file tab).
        bool ActiveEntity(EntityHandle& out) const;
        // -1 = the active tab. Refused ("dirty") with unsaved edits unless @p discard.
        bool CloseTab(int tab, bool discard, std::string& why);
        bool CloseAll(bool discard, std::string& why);
        bool Activate(int tab);
        int  ActiveTab() const { return m_active; }
        int  TabCount() const  { return static_cast<int>(m_tabs.size()); }
        MaterialEditSession* ActiveSession();

        // ---- Undo ----
        // The window's own; EditorLayer routes Ctrl+Z / Ctrl+Y here. Not for an entity tab: its
        // edits are steps of the scene's history.
        bool OwnsUndoShortcut() const;
        void RequestUndo() { ++m_pendingUndo; }
        void RequestRedo() { ++m_pendingRedo; }

        // ---- Scripted surface (Lua Editor.MaterialEditor*, the harness) ----
        // Saves the active tab: to its file, to @p as (Save As), or with @p instance as a new
        // .imatinst of it at @p as (opened in a new tab). A job, run in ProcessPendingActions
        // (0 in Retail: no jobs).
        std::uint64_t QueueSave(const std::string& as, bool instance, std::uint32_t requestId);
        // The editor's unsaved-documents guard: each tab with edits not on disk, by its file.
        std::vector<std::string> UnsavedFiles() const;
        // GAME half (PPA): saves every tab with unsaved edits now. False with @p why naming the
        // first file that could not be written; the others are still saved.
        bool SaveAllNow(const MaterialEditorContext& ctx, std::string& why);

        StateView State() const;
        // The preview's view mode (a Renderer::ViewMode value; MeshEditorWindow names).
        bool SetViewMode(const std::string& name, std::string& why);

        // ---- Material graph ----
        // The active tab's graph (nullptr: not a graph tab).
        MaterialGraphDocument* ActiveGraph();
        // What the active graph compiles with: its material's shading model, blend mode and static
        // switches (the staged values), and the graph's path for the header.
        MaterialGraph::CompileOptions ActiveGraphOptions() const;
        // Gives the active FILE tab's standalone material a graph at
        // asset/Shaders/Generated/<name>_<guid8>.imatgraph; nothing is written until Save.
        // @p from: "standard" (a Standard Surface node wired to the Output: the material as it
        // renders now) or "empty" (the Output only).
        bool CreateGraph(const std::string& from, std::string& why);

        // A texture node's thumbnail: the .itex streamed through the TextureSystem as the Content
        // Browser's tiles are (2D only), its preview descriptor resolved every frame. 0 while it
        // streams. Released once the window is closed, at the serial window (never while an ImGui
        // frame may still draw it).
        std::uint64_t NodeThumbnail(const std::string& itexPath);
        // For Editor.MaterialGraph(): the texture is resident and drawable (a thumbnail shows).
        bool          NodeThumbnailResident(const std::string& itexPath) const;
        // A new Material Function at @p path (.imatfunc) in a tab of its own: Function Input "In"
        // wired to Function Output "Result"; nothing is written until Save.
        bool CreateFunction(const std::string& path, std::string& why);
        // Why the active tab's graph did not open ("" = it did, or there is none).
        std::string ActiveGraphError() const;
        // Reverts the active tab: its material (one undo step) and its graph.
        bool RevertActive(std::string& why);
        // The active graph's canvas: frame everything / the selection, select nodes
        // (applied when it next draws). False: not a graph tab.
        bool GraphView(bool fit, bool frameSelection, const std::vector<std::uint32_t>* select);
        std::vector<std::uint32_t> GraphSelection() const;
        // Hide Unrelated on the active canvas (the selection's up- and downstream stay).
        bool SetGraphHideUnrelated(bool on);
        bool GraphHideUnrelated() const;
        // Unreal's "Start Previewing Node": the preview (never the scene) shows @p node's output
        // @p pin ("" = its first) unlit, compiled live; node 0 stops. The tab keeps it until
        // stopped, reverted, or the node goes.
        bool SetGraphPreview(std::uint32_t node, const std::string& pin, std::string& why);
        std::pair<std::uint32_t, std::string> GraphPreview() const;   // {0, ""}: the material
        // Destroys the node editors' contexts while ImGui is alive (EditorLayer's shutdown) and
        // waits for the live compiles still on a worker.
        void Shutdown();

        // The active graph tab's live shader: its unsaved graph compiled into one .edit file the
        // preview reads until Save (the scene keeps the saved shader).
        GraphLiveView ActiveGraphLive() const;

        // ---- Harness ----
        bool HarnessConfigure(std::string& why);   // App: argv time; false = exit 12
        bool HarnessFailed() const { return m_harnessFailed; }

    private:
        // A worker's DXC run over the live HLSL: every permutation, or the error.
        struct LiveCompileResult
        {
            bool                                                      ok = false;
            std::vector<std::pair<std::string, std::vector<uint8_t>>> containers;   // .ishdr path -> bytes
            std::string                                               error;
            double                                                    ms = 0.0;
            int                                                       perms = 0;
        };
        struct GraphLive
        {
            std::uint64_t                          seenRevision = 0;    // the graph revision last seen
            double                                 changedAt = 0.0;     // when it moved (the debounce)
            std::uint64_t                          builtRevision = 0;   // compiled (CPU) for live
            std::uint64_t                          seenEpoch = 0, builtEpoch = 0;   // MaterialGraph::FunctionEpoch()
            std::uint64_t                          seenMaterial = 0, builtMaterial = 0;   // the session's revision
            std::string                            editPath;
            std::string                            hlsl;                // what the edit file holds
            std::string                            pendingHlsl;         // on the worker
            std::future<LiveCompileResult>         job;
            bool                                   active = false;
            std::vector<MaterialGraph::Diagnostic> dxc;                 // DXC's errors, on their nodes
            std::uint64_t                          version = 0;         // the live shader changed
            int                                    compiles = 0, failures = 0;
            double                                 lastMs = 0.0;
            MaterialComponent                      material;            // what the preview reads while active
            std::uint64_t                          materialFor = ~0ull;
            // Saved while live: the file's loaded shader still has the old bytes, so the preview keeps
            // the live one (the same new bytes) until the file's reload is in effect (the library's
            // reload epoch moves; retireBy caps it at 5 s), so no frame shows the old look.
            bool                                   retiring = false;
            std::uint32_t                          retireEpoch = 0;
            double                                 retireBy = 0.0;
        };
        struct Tab
        {
            std::unique_ptr<MaterialEditSession> session;
            std::string                          name;
            bool                                 diskChanged = false;   // changed on disk (or its parent) while dirty
            // The live overlay: this tab's staged material is what the scene reads for its file
            // (pushed at liveRevision, at most 10 a second while a control is held).
            bool                                 live = false;
            std::uint64_t                        liveRevision = 0;
            double                               livePushedAt = 0.0;    // seconds (steady clock)
            std::uint64_t                        livePushes = 0;        // of this live period
            // An entity tab: the entity, and how to find it again in a new world (a scene switch,
            // Play / Stop): its Guid, else its name if unique.
            EntityHandle                         entity = NullEntityHandle;
            ECS::Guid                            guid{};
            std::string                          entityName;
            std::uint32_t                        epoch = 0;
            // The material's graph, or why it did not open, and its canvas.
            std::unique_ptr<MaterialGraphDocument> graph;
            std::string                          graphError;
            std::unique_ptr<MaterialGraphCanvas> canvas;
            int                                  graphPane = 0;          // 0 Graph, 1 Material, 2 Code, 3 Parameters
            bool                                 selectGraphPane = false;
            GraphLive                            graphLive;              // the live shader (SyncGraphLive)
            // The node the preview shows instead of the material (0 = none), and the Graph toolbar's
            // Find box.
            std::uint32_t                        previewNode = 0;
            std::string                          previewPin;
            char                                 find[64] = {};
            int                                  findAt = -1;            // the match last jumped to
            // An instance of a graph material: its parent's graph, read-only (the switches it may
            // override, the combination its Save compiles).
            std::unique_ptr<MaterialGraphDocument> parentGraph;
            std::string                          parentGraphError;
        };
        struct PendingSave
        {
            int           tab = -1;
            std::string   as;
            bool          instance = false;
            std::uint64_t job = 0;
            bool          closeAfter = false;
        };

        static constexpr std::size_t      kMaxNodeThumbnails = 64;

        struct Step
        {
            int           kind = 0;
            std::uint64_t at = 0;
            std::string   text;
            bool          done = false;
        };

        Tab* ActiveTabPtr();
        static bool TabDirty(const Tab& t);        // the material's edits or the graph's
        bool AnyDirty() const;
        // Save of a graph tab: compile, write the HLSL and the graph, point the material at them.
        bool ApplyGraph(const MaterialEditorContext& ctx, Tab& t, std::string& json, std::string& why);
        void RevertGraph(Tab& t);                 // with the session's Revert
        void DrawGraphPanes(const MaterialEditorContext& ctx, Tab& t);
        void DrawCode(Tab& t);
        void DrawParameters(Tab& t);    // the Parameters pane
        void DrawMaterialPanel(const MaterialEditorContext& ctx, Tab& t);
        // Compiles each graph tab's changes (debounced) on a worker; a result that compiled
        // becomes the tab's live shader.
        void SyncGraphLive(const MaterialEditorContext& ctx);
        // What the preview draws for the tab: its staged material, or - while its
        // graph is live - a copy pointed at the live shader.
        const MaterialComponent& PreviewMaterial(Tab& t);
        MaterialGraph::CompileOptions GraphOptions(const Tab& t) const;   // the material
        MaterialGraph::CompileOptions LiveOptions(const Tab& t) const;    // ... or the previewed node
        bool SetPreviewOn(Tab& t, std::uint32_t node, const std::string& pin, std::string& why);
        // The Static Switches section (a graph tab: the material's values; an instance of a graph
        // material: its overrides), and an instance's Save of its combination (compiled from the
        // parent's graph, customShaderPath).
        MaterialGraphDocument* InstanceGraph(Tab& t);
        MaterialGraph::CompileOptions InstanceGraphOptions(const Tab& t) const;
        void DrawSwitches(Tab& t);
        bool ApplyInstanceSwitches(Tab& t, std::string& why);
        void DrawToolbar(const MaterialEditorContext& ctx, Tab& t);
        void DrawDetails(const MaterialEditorContext& ctx, Tab& t);
        void DrawAskModal();
        void RunSave(const MaterialEditorContext& ctx, const PendingSave& s);
        void ReloadDependents(const std::string& saved);   // tabs that read @p saved
        // Pushes each dirty tab's staged material to the live overlay (and the scene); ends the
        // live period of a clean or closed one.
        void SyncLive(const MaterialEditorContext& ctx);
        void EndLive(const MaterialEditorContext& ctx, Tab& t, const char* why);
        // Finds each entity tab's entity (again, after a world change) and mirrors it; a gone one
        // closes. Writes what a tab owes its entity.
        void SyncEntityTabs(const MaterialEditorContext& ctx);
        void FlushEntity(const MaterialEditorContext& ctx, Tab& t);
        void DrawEntityToolbar(const MaterialEditorContext& ctx, Tab& t);
        void LogOpen(const MaterialEditSession& s) const;
        void LogDump(const MaterialEditSession& s, const std::string& keys) const;
        void TickHarness(const MaterialEditorContext& ctx);

        // Texture nodes' thumbnails (NodeThumbnail).
        void                              ReleaseNodeThumbnails();

        bool                              m_open = false;
        std::vector<Tab>                  m_tabs;
        int                               m_active = -1;
        int                               m_focusTab = -1;
        bool                              m_focused = false;
        std::vector<std::string>          m_pendingOpen;
        std::vector<PendingSave>          m_pendingSaves;
        std::vector<int>                  m_pendingRevert;
        int                               m_pendingUndo = 0, m_pendingRedo = 0;
        // The unsaved-edits question: the tab it is for (-2 = closing the window).
        int                               m_askFor = -1;
        bool                              m_askOpenPopup = false;
        std::uint64_t                     m_frame = 0;
        std::uint64_t                     m_lastDiskCheck = 0;
        // Files whose tab closed while live: cleared and re-materialized in PPA.
        std::vector<std::string>          m_liveEnded;
        std::uint64_t                     m_livePushes = 0;
        const Renderer*                   m_renderer = nullptr;   // State()'s rtProxy (the last PPA's)

        IGraphicsDevice*                  m_gfx = nullptr;
        Resource::TextureSystem*          m_texSys = nullptr;
        Resource::ResourceManager*        m_resMgr = nullptr;
        std::unordered_map<std::string, Resource::TextureHandle> m_nodeThumbs;

        // Ctrl+Z over the canvas steps the graph's ring, elsewhere the material's.
        bool                              m_undoToGraph = false;
        // -mateditor_rect: the window pinned (x, y, w, h) for deterministic captures.
        bool                              m_pinRect = false;
        float                             m_rect[4] = { 0, 0, 0, 0 };
        bool                              m_canvasOff = false;   // -matgraph_canvas_off (reverse arm)

        MaterialPropertyPanel                  m_panel;      // one: the active tab's
        std::unique_ptr<MaterialPreviewWidget> m_preview;    // the active tab's material

        // Harness (-mateditor_*).
        std::vector<std::string> m_harnessOpen;
        bool                     m_harnessOpened = false;
        bool                     m_harnessFailed = false;
        std::vector<Step>        m_steps;
    };
}
