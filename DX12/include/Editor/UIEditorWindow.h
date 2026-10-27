#pragma once

// UIEditorWindow - the UI Editor: a Canvas UI tree edited in a designer viewport
// of its own, the way Unreal's UMG designer edits a widget blueprint. Entered
// from a Canvas: the Inspector's "Edit in UI Editor", the Outliner's context
// menu, Window > Content > UI Editor, Lua Editor.OpenUIEditor, the
// -uieditor_open harness flag.
//
// The picture is a Renderer UI view (Renderer/UIViewService.h): the subject laid
// out at the tab's design resolution - not the main viewport's - by the kernel
// the game screen uses (UI/UICanvasLayout.h), painted into the view's own draw
// list, zoomed and panned as a transform of that list, and drawn by a second
// UIPass into a panel-owned RGBA8 target that ImGui shows 1:1. The game screen's
// UIComputedRect is never written and its interaction pass never runs. Only the
// active tab owns a view; a closed window owns none.
//
// Threads: Draw is a UI handler: it edits the active tab's view state and reads
// the service's status and SRV. ProcessPendingActions (PPA) is the game half's
// serial window: opens / closes, the subject's identity check (an Entity has no
// generation: World::Epoch and the canvas GUID), the view's Desc (sent every
// frame from there, so a harness run with no laid-out window still draws), the
// view's release (never from a UI handler), the harness steps and the shots.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"
#include "ECS/Guid.h"
#include "Editor/UIRectEdit.h"
#include "Renderer/UIView.h"
#include "UI/UICanvasLayout.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <typeindex>
#include <vector>

class Renderer;
class IGraphicsDevice;
namespace Editor::Undo { class IEditAction; struct EditSink; struct EntitySubtreeSnapshot; }
namespace Resource { class AssetManager; }

namespace Editor
{
    struct UIEditorContext
    {
        Renderer*        renderer    = nullptr;
        IGraphicsDevice* gfx         = nullptr;
        World*           world       = nullptr;
        std::uint64_t    editorFrame = 0;
        bool             playing     = false;
        // The editor's selection; scene tabs share it.
        Entity           selected    = NullEntity;
        std::function<void(Entity)> select;
        // The scene's own edits, each one undo step on the editor's stack (scene tabs
        // share it): a component edit, a UI move (EditorLayer::MoveUINode), the
        // Outliner's delete and rename.
        std::function<void(std::unique_ptr<Editor::Undo::IEditAction>)> pushEdit;
        std::function<bool(Entity, Entity, int, std::string*)> moveUINode;
        std::function<void(Entity)> deleteEntity;
        std::function<void(Entity, const std::string&)> rename;
        // What the panels do, each one undo step on the editor's stack too.
        // recordSpawn: whatever @p spawn creates is one create step (RecordSpawn).
        std::function<void(const char*, const std::function<void()>&)> recordSpawn;
        // A copy of @p e with its UI subtree (EditorLayer::DuplicateEntity).
        std::function<Entity(Entity)> duplicate;
        // A UI template (.ipfb) spawned as a detached copy under the UI node @p parent -
        // call it inside recordSpawn. Its root, or NullEntity with @p why (nothing is
        // left behind).
        std::function<Entity(const std::string&, Entity, std::string*)> instantiate;
        // Details: one registered component's Inspector section - its header, its
        // Remove Component, the field-edit undo bracket. False: no editor for it.
        std::function<bool(Entity, std::type_index)> drawComponent;
        // The Inspector's field-edit undo bracket around a body drawn here.
        std::function<void(Entity, std::type_index, const std::function<void()>&)> drawWithUndo;
        // Add Component with undo (applied at the frame boundary).
        std::function<void(Entity, std::type_index)> addComponent;
        // Several steps as one undo entry (EditHistory macros).
        std::function<void(const std::string&)> beginMacro;
        std::function<void()>                   endMacro;
        // An asset tab's stage: textures for SpawnUIAsset, a read-only template loaded
        // as it is (no parent, no adoption), and the active asset tab's own ring stepped
        // with the editor's undo context (false: nothing to step). EditorLayer runs every
        // callback above with the active asset tab's sink in scope: an asset tab's edits
        // are its file's, never the scene's.
        Resource::AssetManager* assets = nullptr;
        std::function<Entity(const std::string&, std::string*)> loadPrefab;
        std::function<bool(bool redo)>                         sinkUndo;
    };

    class UIEditorWindow
    {
    public:
        static constexpr int kMaxTabs = 8;

        // Which screen the subject is laid out for.
        enum class Resolution : std::uint8_t
        {
            Reference = 0,   // the canvas's referenceResolution (the default)
            MatchMain,       // the game viewport, as the game's own paint saw it
            Custom,          // resW x resH
        };

        // What one tab shows (the Lua / harness surface too).
        struct ViewState
        {
            Resolution         res        = Resolution::Reference;
            std::uint32_t      resW       = 1920, resH = 1080;    // Custom
            // View pixels per design pixel; 0 = fit (recomputed from the canvas
            // every frame until the user zooms or pans).
            float              zoom       = 0.f;
            float              pan[2]     = { 0.f, 0.f };         // the design frame's top-left in the canvas, px
            UIView::Background background = UIView::Background::Checker;
            std::uint32_t      solidRgba  = 0xFF202020u;          // Color32 packing (0xAABBGGRR)
            UI::StatePreview   state      = UI::StatePreview::Live;
            bool               stateSelected = false;   // the state preview on the selection only
            bool               showHidden = false;
            bool               realtime   = true;
            // Overlays (drawn by the panel, never painted into the picture).
            int                safeArea   = 0;      // percent of the frame kept clear: 0 (off), 90, 95
            bool               grid       = false;  // grid lines every gridRef REFERENCE px
            int                gridRef    = 8;
            bool               rulers     = true;
            bool               snap       = true;   // parent / sibling edges and centres, the grid
            // A UI-node (not canvas) subject's preview frame - Fill (the design screen is
            // its parent) or Custom W x H (the root that size, centred; an asset tab of a row
            // template opens at the row's own size).
            UIView::RootFrame  frame;
        };

        // The handles past UIRectEdit's: the anchor box's corners (min, max and the two
        // mixed ones), all four together (a point anchor's flower), the pivot. A drag of
        // any of them keeps the rect where it is on screen.
        static constexpr int kAnchorTL = 16, kAnchorTR = 17, kAnchorBR = 18, kAnchorBL = 19, kAnchorAll = 20,
                             kPivotHandle = 21;

        // The 9-slice border handles of a selected image (UIEditorDrag "slice_l" ...): a
        // drag moves that border, in texture pixels (= design px), clamped so opposite
        // borders never cross. ONE undo step.
        static constexpr int kSliceL = 22, kSliceT = 23, kSliceR = 24, kSliceB = 25;

        struct Issue { Entity e = NullEntity; std::string kind, text; };

        // ---- UI asset tabs ----
        enum class TabKind : std::uint8_t { Scene = 0, Asset, Template };

        struct TabState
        {
            Entity        subject = NullEntity;
            std::string   name;
            std::string   guid;                    // hex; "" when the canvas has none
            ViewState     view;
            std::uint32_t designW = 0, designH = 0;   // the resolution in effect
            float         zoom = 1.f;               // in effect (fit resolved)
            float         pan[2] = { 0.f, 0.f };
            std::uint32_t viewId = ~0u;
            bool          settled = false;
            UIView::Status status;
            Entity        selected = NullEntity;   // a node of this tab, else NullEntity
            std::vector<Entity> designHidden, locked;   // designer-only flags
            // What the tab edits.
            TabKind       kind = TabKind::Scene;
            std::string   path;                    // asset / template: the file
            bool          dirty = false, readOnly = false, diskChanged = false, parked = false;
            std::string   readOnlyWhy;             // why Save is refused ("" = it is not)
            std::size_t   undo = 0, redo = 0;      // an asset tab's own ring
        };
        struct StateView
        {
            bool                  open = false;
            int                   active = -1;
            UI::Vec2              mainScreen{ 0.f, 0.f };
            std::vector<TabState> tabs;
        };

        // ---- Lifetime ----
        static UIEditorWindow& Get();

        void Draw(const UIEditorContext& ctx);
        void ProcessPendingActions(const UIEditorContext& ctx);   // game half, serial window
        void ReleaseAtShutdown(const UIEditorContext& ctx);
        bool  IsOpen() const { return m_open; }
        // A Rect Tool drag or a keyboard nudge is in progress: Ctrl+Z must not pop
        // the history under a gesture not recorded yet (EditorLayer's shortcuts).
        bool  DragActive() const { return m_drag.active || m_nudge.active; }
        // Lay the active tab out again now (a scripted call's nodes may be this
        // frame's; Draw reads the PPA's).
        void  SyncLayout(const UIEditorContext& ctx) { RebuildRows(ctx); }
        bool* OpenFlag()     { return &m_open; }

        // ---- Tabs ----
        // From a UI handler (Inspector, Outliner): queued, opened in
        // ProcessPendingActions. @p e is a canvas or any UI node under one; the
        // tab is its canvas and the node becomes the selection.
        void RequestOpen(Entity e);
        // GAME half (Lua, harness): now. An open tab of the same canvas is focused.
        // Refused past kMaxTabs, or when @p e is not a UI node. @p tabOut: the tab.
        bool OpenNow(const UIEditorContext& ctx, Entity e, std::string& why, int* tabOut = nullptr);
        // The first UICanvas root named @p name (the -uieditor_open subject).
        static Entity FindCanvasByName(World& world, const std::string& name);
        // -1 = the active one. An asset tab with unsaved edits closes only with
        // @p discard (false and @p why otherwise); its stage goes with it.
        bool CloseTab(const UIEditorContext& ctx, int tab, bool discard = true, std::string* why = nullptr);
        void CloseAll(const UIEditorContext& ctx);
        bool Activate(int tab);
        int  ActiveTab() const { return m_active; }
        int  TabCount()  const { return static_cast<int>(m_tabs.size()); }

        // ---- Scripted surface (Lua Editor.UIEditor*, the harness) ----
        // One key of the active tab: res ("WxH" | "reference" | "match_main") /
        // zoom ("fit" | n) / pan ("x:y") / bg ("checker" | "none" | "solid[:r,g,b]")
        // / state (live | normal | hovered | pressed | disabled) / hidden / realtime
        // / size ("WxH" = pin the view box, "0x0" = follow the canvas).
        bool Set(const std::string& key, const std::string& value, std::string& why);
        // A spec "key=value;key=value" - all or nothing.
        bool ApplySpec(const std::string& spec, std::string& why);

        // ---- Selection and the Rect Tool (design pixels) ----
        // The active tab's node under @p designPt, front to back, skipping culled
        // and (unless shown) hidden nodes - the canvas root when it is empty space.
        // @p cycle (Alt+click): the next node under the point behind @p current.
        Entity HitTest(const UIEditorContext& ctx, UI::Vec2 designPt, bool cycle = false,
                       Entity current = NullEntity);
        // Select @p e (a node of the active tab): scene tabs write the editor's selection.
        bool Select(const UIEditorContext& ctx, Entity e, std::string& why);
        // A drag of @p handle (UIRectEdit::Handle) by @p delta DESIGN px on the
        // selection, through the same code the mouse runs (snapping unless @p alt,
        // Ctrl quantises, Shift keeps a corner's aspect) - ONE undo step.
        bool Drag(const UIEditorContext& ctx, int handle, UI::Vec2 delta, bool ctrl, bool shift, bool alt,
                  std::string& why);
        static int HandleFromName(const std::string& name);   // "body", "tl" ... "l", "anchor_min" ...; -1 when none
        static const char* HandleName(int handle);

        // A 4x4 anchor preset on the selection (top_left .. stretch_full): the
        // anchors change and offset / size are solved so the rect stays (unless
        // !keepVisual - the raw values, the uieditor_anchor pack's control); @p setPivot
        // (Alt) also takes the preset's pivot, @p setPosition (Shift) puts the rect
        // onto the anchors. ONE undo step.
        bool ApplyAnchorPreset(const UIEditorContext& ctx, const std::string& name, bool setPivot, bool setPosition,
                               bool keepVisual, std::string& why);
        static bool IsAnchorPreset(const std::string& name);

        // ---- Panels: Palette, Hierarchy, Details ----
        // A built-in widget ("panel" ... "empty_rect", Editor::UITemplates) or a UI
        // template (an .ipfb path) under @p parent - NullEntity: the selection when it
        // is a node of the open canvas, else the canvas - last among its siblings,
        // its pivot on @p at (design px) when @p hasAt, else centred. ONE undo step;
        // the new root becomes the selection. NullEntity with @p why on a refusal.
        Entity CreateWidget(const UIEditorContext& ctx, const std::string& type, Entity parent, bool hasAt,
                            UI::Vec2 at, std::string& why);
        // @p e moved @p delta places later (> 0) or earlier (< 0) among its siblings -
        // paint order - clamped at the ends. ONE undo step (EditorLayer::MoveUINode).
        bool   Reorder(const UIEditorContext& ctx, Entity e, int delta, std::string& why);
        // The Hierarchy's drop as a call: @p e under @p parent (a node of the active
        // tab, its root included) at @p index among the parent's children in paint order
        // (-1 = last). ONE undo step (EditorLayer::MoveUINode: an asset tab's own ring).
        // @p why starts with its code ("cycle: ...", "refused: ..."), as MoveUINode's
        // refusals do.
        bool   Reparent(const UIEditorContext& ctx, Entity e, Entity parent, int index, std::string& why);
        // Designer-only flags of the active tab (never saved, never in the game):
        // hidden in the designer - neither painted nor hit, its subtree too - and
        // locked - a click goes through it (the Hierarchy still selects it).
        bool   SetDesignerFlag(const UIEditorContext& ctx, Entity e, bool locked, bool on, std::string& why);
        bool   DesignerHidden(Entity e) const;
        bool   Locked(Entity e) const;
        // The inline text editor's commit: @p e's UIText text. ONE undo step.
        bool   SetText(const UIEditorContext& ctx, Entity e, const std::string& text, std::string& why);
        // A texture dropped at @p at (design px): onto the image under it (its
        // texture), else a new Image node there at the texture's native size. The node
        // changed or made; NullEntity with @p why. ONE undo step.
        Entity DropTexture(const UIEditorContext& ctx, const std::string& path, UI::Vec2 at, std::string& why);
        // @p e sized to its image: the texture's pixels across its uv window (a sprite
        // sheet's frame), one reference px each; the pivot stays put. ONE undo step.
        bool   NativeSize(const UIEditorContext& ctx, Entity e, std::string& why);
        // @p e wrapped in a new Empty Rect that takes its place and its rect (the node
        // then fills it: the picture does not change); the wrapper becomes the
        // selection. ONE undo step.
        bool   WrapInRect(const UIEditorContext& ctx, Entity e, std::string& why);

        // The Check page: what is wrong with the open canvas - sizes, nodes off every
        // preset screen, missing textures / fonts, sibling names, unreachable nodes.
        std::vector<Issue> Check(const UIEditorContext& ctx);

        // ---- UI asset tabs ----
        // A tab edits a scene's canvas live (Scene), a .iui file through a STAGE of its
        // own (Asset), or shows a UI .ipfb read-only (Template). A stage is the asset
        // spawned into the main World (GuidRegistry is one per process, so no private
        // World), every node SceneTransientTag + PreviewStageTag{kUIEditor} (no scene
        // file, Outliner, pick or Editor.* call sees it) and a canvas root
        // UICanvasOffscreen (the game screen does not draw it). Its edits go to the
        // tab's own ring through EditorLayer's EditSink; the tab is dirty while its
        // stage, captured, differs from the file as loaded / saved. A scene load or
        // Play takes the stage away; the tab spawns it again from that capture.
        static const char* TabKindName(TabKind k);
        // From a UI handler (the Content Browser): queued, opened in ProcessPendingActions.
        void RequestOpenAsset(const std::string& path);
        // GAME half (Lua, harness): a .iui (an asset tab) or a UI .ipfb (a template
        // tab); an open tab of the file is focused. @p tabOut: the tab.
        bool OpenAssetNow(const UIEditorContext& ctx, const std::string& path, std::string& why, int* tabOut = nullptr);
        // A .ipfb whose root node is a UI node (a canvas or a UIRect).
        static bool IsUITemplateFile(const std::string& path);
        // The active tab's ring when it is an editable asset tab with a stage, else
        // nullptr - EditorLayer routes its edit helpers there for a UI Editor call.
        std::shared_ptr<Undo::EditSink> ActiveSink() const;
        // The ring of the asset tab whose stage holds @p e (Entity.set on a stage node).
        std::shared_ptr<Undo::EditSink> SinkForStageEntity(World& world, Entity e) const;
        // The active tab's selection: a scene tab's is the editor's, an asset / template
        // tab's its own.
        Entity Selection(const UIEditorContext& ctx) const { return Sel(ctx); }
        // @p e is the active non-scene tab's selection (its Details' field bracket is
        // not stale for the Inspector - the scene's selection is something else).
        bool  EditsEntity(Entity e) const;
        // The window had keyboard focus last frame and its active tab is an asset or a
        // template: Ctrl+S / Z / Y / D are its (EditorLayer::HandleUndoShortcuts).
        bool  OwnsShortcuts() const;
        void  RequestSave(bool saveAs);   // Save As asks for the path here (a UI handler)
        void  RequestUndo(bool redo);
        void  RequestDuplicate();
        // The active tab's file - scripted (Lua) and the PPA's requests. Save: the stage
        // captured, read back, written atomically (@p asPath: Save As - the tab follows
        // the new file; an existing other file only with @p overwrite - the Save As
        // dialog asked already, a script says so). Revert: the file again, the ring cleared.
        bool  Save(const UIEditorContext& ctx, const std::string& asPath, std::string& why,
                   std::size_t* bytes = nullptr, std::size_t* nodes = nullptr, bool overwrite = true);
        bool  Revert(const UIEditorContext& ctx, std::string& why);
        bool  Undo(const UIEditorContext& ctx, bool redo, std::string& why);
        // @p e (a node of the active tab, not its root) copied beside itself with its
        // subtree - one undo step; the copy becomes the selection.
        Entity Duplicate(const UIEditorContext& ctx, Entity e, std::string& why);
        // A template tab written as a .iui at @p path, opened in its own tab (the
        // template's closes). Refuses an existing file unless @p overwrite.
        bool  Convert(const UIEditorContext& ctx, const std::string& path, bool overwrite, std::string& why);
        // @p e (a node of the active tab) and its subtree as a new .iui; @p open: then
        // in a tab. Refuses an existing file unless @p overwrite.
        bool  SaveBranch(const UIEditorContext& ctx, Entity e, const std::string& path, bool overwrite, bool open,
                         std::string& why, std::size_t* nodes = nullptr);
        // Extract to UI Asset: @p e (a node of the active SCENE tab) and its subtree
        // written as a .iui, and replaced in the scene by a reference to it: a UI node
        // filling @p e's parent at its place with a UI Document (a canvas: an entity of
        // its own carrying it). The picture does not change; ONE undo step. The host, or
        // NullEntity with @p why.
        Entity Extract(const UIEditorContext& ctx, Entity e, const std::string& path, bool overwrite, std::string& why);
        // The paths of the asset tabs with unsaved edits (the editor's exit prompt) /
        // save them all (false and @p why at the first that fails).
        std::vector<std::string> UnsavedAssets() const;
        bool  SaveAllUnsaved(const UIEditorContext& ctx, std::string& why);
        // Enter Play (App's beforeSnapshot): every stage captured and DETACHED (the
        // undo snapshot's Detach: destroyed, its ids parked - CloneState carries the
        // parking) - the played world never holds one. After Stop it is attached again
        // with the same ids, so the tab's undo ring stays valid; spawned from its
        // capture when that fails. Meanwhile the tab shows its last picture
        // (UIView::Desc::freeze).
        void  TearDownForPlay(Renderer* renderer, World* world);
        // A Rect Tool drag or a keyboard nudge in progress when Play starts is
        // committed - one undo step - before the snapshot, never dropped.
        void  CommitGestures(const UIEditorContext& ctx);

        // ---- Editor prefs ----
        // The per-canvas resolution (editor_prefs.ini "uieditor.res.<guid>=").
        void LoadPref(const std::string& key, const std::string& value);
        void WritePrefs(std::ostream& out) const;

        // ---- Scripted surface (Lua Editor.UIEditor*, the harness) ----
        // The names Set accepts and State reports.
        static const char* ResolutionName(Resolution r);
        static const char* BackgroundName(UIView::Background b);
        static const char* StateName(UI::StatePreview s);
        // Pin the view box (harness determinism, -uieditor_size); 0 = the canvas.
        void PinSize(std::uint32_t w, std::uint32_t h) { m_pinW = w; m_pinH = h; }

        StateView State(const UIEditorContext& ctx) const;

        // The active tab's subject laid out exactly as its view paints it (design
        // pixels): the geometry oracle. False with @p why when there is nothing.
        bool Layout(const UIEditorContext& ctx, std::vector<UI::LayoutNode>& out, float& scale,
                    UI::Vec2& designSize, std::string& why) const;

        // Write the active tab's picture (its target, the view box) to a PNG - the
        // serial window only (CaptureTextureToPNG syncs). False with @p why.
        bool WriteShot(const UIEditorContext& ctx, const std::string& path, std::string& why);
        // The active tab's target holds the picture of its CURRENT description, no
        // image of it is still loading, and kSettleFrames frames have passed: a
        // shot now is final.
        bool Settled() const;
        // Lua Editor.UIEditorShot: a job (Harness/ControlChannelJobs.h) that writes
        // the active tab's picture once it has settled (ProcessPendingActions), or
        // fails after kShotDeadline editor frames. 0 in Retail (no jobs).
        std::uint64_t QueueShot(const std::string& path, std::uint32_t requestId);

        // ---- Harness ----
        bool HarnessConfigure(std::string& why);   // argv time; false = exit 12
        bool HarnessFailed() const { return m_harnessFailed; }

    private:
        struct Tab
        {
            Entity        subject = NullEntity;
            ECS::Guid     guid{};
            std::uint32_t epoch = 0;
            std::string   name;
            ViewState     view;
            // The resolution in effect, refreshed every PPA (Draw reads it: a UI
            // handler does not read the World).
            float         designW = 0.f, designH = 0.f;
            // Designer-only (never saved): hidden in the designer, locked, and the
            // Hierarchy's folded rows.
            std::vector<Entity> designHidden, locked, folded;
            // ---- Asset / template tab ----
            TabKind       kind = TabKind::Scene;
            std::uint32_t uid  = 0;                 // stable while tabs move (the sink finds it)
            std::string   path;                     // the file (canonical, root-relative)
            std::string   savedText;                // asset: the file as loaded / last saved (canonical)
            std::string   stagedText;               // the stage as last captured (what respawns it)
            ECS::Guid     seed{};                   // the stage's node GUIDs: Derive(path, id, seed)
            std::shared_ptr<Undo::EditSink> sink;   // asset: its own ring (EditorLayer's EditSink)
            Entity        sel = NullEntity;         // its own selection (a stage is no scene selection)
            std::string   selId;                    // ... its node id: found again after a respawn
            std::int64_t  diskStamp = 0;            // the file as this tab last read / wrote it
            std::uint64_t diskSize = 0;
            bool          diskChanged = false;      // changed on disk under unsaved edits
            bool          parked = false;           // no stage: Play (attached again after Stop)
            std::string   readOnlyWhy;              // Save refused ("" = it is not)
            // The stage detached for Play (ids parked); subject keeps its root's id.
            std::shared_ptr<Undo::EntitySubtreeSnapshot> parkedStage;
        };

        // ---- Layout rows ----
        struct Row
        {
            Entity   e = NullEntity;
            Entity   parent = NullEntity;    // its UI parent (NullEntity for the root)
            UI::Rect rect, clip, parentRect;
            int      depth = 0;
            bool     culled = false, hidden = false, isRoot = false, isRect = false;
            // Hidden in the designer (itself or an ancestor), locked, has children.
            bool     skip = false, locked = false, hasKids = false;
            UI::UIRect authored;             // its UIRect (isRect)
        };

        struct DragState
        {
            bool             active = false;
            Entity           e = NullEntity;
            int              handle = -1;
            UI::UIRect       start;
            UI::Rect         startRect;
            UIRectEdit::Ctx  rctx;
            float            startMouse[2] = { 0.f, 0.f };   // view px (mouse drags)
            std::string      beforeBlob;
            std::vector<float> snapX, snapY;               // design px
            float            guideX = 0.f, guideY = 0.f;
            bool             hasGuideX = false, hasGuideY = false;
            bool             moved = false;
            // A 9-slice border handle (kSliceL..kSliceB) edits the UIImage instead.
            bool             slice = false;
            float            startBorder[4] = { 0.f, 0.f, 0.f, 0.f };
        };

        struct Nudge { bool active = false; Entity e = NullEntity; std::string beforeBlob; };

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

        void        RebuildRows(const UIEditorContext& ctx);
        const Row*  FindRow(Entity e) const;

        bool  BeginDrag(const UIEditorContext& ctx, Entity e, int handle, std::string& why);
        void  UpdateDrag(const UIEditorContext& ctx, UI::Vec2 delta, bool ctrl, bool shift, bool alt);
        void  EndDrag(const UIEditorContext& ctx);

        void  NudgeSelection(const UIEditorContext& ctx, float dx, float dy);
        void  EndNudge(const UIEditorContext& ctx);

        void  ApplyResPref(Tab& t) const;
        void  RememberRes(const Tab& t);
        void  FrameSelection(const UIEditorContext& ctx, Tab& t);
        void  DrawAnchorPresetPopup(const UIEditorContext& ctx);
        void  DrawOverlay(const UIEditorContext& ctx, Tab& t, float originX, float originY, float z,
                          const float pan[2], float width, float height);

        // ---- Panels ----
        void  DrawPalette(const UIEditorContext& ctx, Tab& t);
        void  DrawHierarchy(const UIEditorContext& ctx, Tab& t);
        void  DrawDetails(const UIEditorContext& ctx, Tab& t);
        void  DrawRectDetails(const UIEditorContext& ctx, const Row& r);
        void  DrawCheck(const UIEditorContext& ctx);
        void  DrawTextEditor(const UIEditorContext& ctx);
        void  ScanTemplates();
        // Where a drop at @p at lands: the frontmost node under it that is not locked
        // or hidden in the designer - a text node's parent (text is a leaf) - else the root.
        Entity DropParent(const UIEditorContext& ctx, UI::Vec2 at);

        Tab*       ActiveTabPtr();
        const Tab* ActiveTabPtr() const;

        // ---- Asset tabs ----
        // The selection the panels work on: a scene tab's is the editor's, an
        // asset / template tab's its own.
        Entity     Sel(const UIEditorContext& ctx) const;
        void       SelectNode(const UIEditorContext& ctx, Entity e);
        // Nothing may change the active tab now: Play, a read-only template, a parked
        // stage - and the reason, for a refusal.
        bool        Frozen(const UIEditorContext& ctx) const;
        std::string FrozenWhy(const UIEditorContext& ctx) const;
        // @p e is a node of a UI Document instance (the .iui's, made at run time):
        // read-only here (refused with @p why); its asset opens in a tab of its own.
        bool        InstanceNode(const UIEditorContext& ctx, Entity e, std::string* why = nullptr) const;
        Tab*       TabByUid(std::uint32_t uid);
        int        TabIndexByPath(const std::string& path) const;
        // The stage: spawned from @p text (a canonical .iui), tagged, the subject; its
        // nodes re-tagged (a copy made by Duplicate lost the tags); captured.
        bool       SpawnStage(const UIEditorContext& ctx, Tab& t, const std::string& text, std::string& why);
        void       DestroyStage(World& world, Tab& t);
        void       Retag(World& world, const Tab& t) const;
        bool       CaptureStage(World& world, const Tab& t, std::string& text, std::string* why,
                                std::vector<std::pair<Entity, std::string>>* ids = nullptr) const;
        // A stage change landed (the sink's changed()): re-tag, capture.
        void       OnStageChanged(std::uint32_t uid, World* world);
        // A non-scene tab's stage still there (else found by GUID, else spawned again).
        bool       RevalidateStage(const UIEditorContext& ctx, Tab& t, std::string& why);
        bool       SaveTab(const UIEditorContext& ctx, Tab& t, const std::string& asPath, std::string& why,
                           std::size_t* bytes, std::size_t* nodes, bool overwrite = true);
        bool       ReloadTab(const UIEditorContext& ctx, Tab& t, std::string& why);
        void       PollDisk(const UIEditorContext& ctx, Tab& t);
        void       DrawAssetBar(const UIEditorContext& ctx, Tab& t);
        void       DrawConfirmClose(const UIEditorContext& ctx);
        // The frame an asset tab of a rect root opens at: its own point-anchored size.
        void       DefaultFrame(World& world, Tab& t) const;

        // ---- Window, tabs and the view ----
        // Before the active tab changes, a gesture still held (drag, nudge) commits
        // into ITS tab's ring.
        void       SwitchTab(const UIEditorContext& ctx, int tab);
        // The resolution the tab lays out for now (Reference reads the canvas).
        void       DesignSize(const UIEditorContext& ctx, const Tab& t, float& w, float& h) const;
        // The view box (target) size: the pinned size, else the canvas quantised.
        void       BoxSize(std::uint32_t& w, std::uint32_t& h) const;
        // The zoom / pan in effect: the tab's, or fit when it has none.
        void       EffectiveView(const Tab& t, float designW, float designH, float& zoom, float pan[2]) const;
        bool       BuildDesc(const UIEditorContext& ctx, const Tab& t, UIView::Desc& out) const;
        // A tab's subject still the canvas it opened? Re-resolved by GUID after a
        // world swap; false (and @p why) when it is gone.
        bool       Revalidate(const UIEditorContext& ctx, Tab& t, std::string& why);
        void       ReleaseView(const UIEditorContext& ctx);
        void       DrawToolbar(const UIEditorContext& ctx, Tab& t);
        void       DrawCanvas(const UIEditorContext& ctx, Tab& t, float width, float height);
        // ---- Harness ----
        void       TickHarness(const UIEditorContext& ctx);

        // ---- Window, tabs and the view ----
        void                     UpdateSettle(const UIView::Status& st);

        // ---- Layout rows ----
        // The active tab's layout as its view paints it, built in PPA (and on demand
        // for a scripted call): Draw's overlay and hit test read it - a UI handler
        // does not lay out.
        std::vector<Row> m_rows;
        float            m_rowsScale = 1.f;
        UI::Vec2         m_rowsDesign{ 0.f, 0.f };
        int              m_rowsTab = -1;
        std::uint64_t    m_rowsFrame = ~0ull;

        // ---- Mouse and keyboard input ----
        DragState        m_drag;

        // Keyboard nudges: one undo step for as long as an arrow key is held.
        Nudge            m_nudge;

        // Rename popup (F2).
        Entity           m_renaming = NullEntity;
        char             m_renameBuf[256] = {};
        bool             m_renameFocus = false;
        // Space + left drag pans.
        bool             m_spacePan = false;
        Entity           m_hoverEntity = NullEntity;   // the node under the cursor (Draw)
        // ---- Editor prefs ----
        // editor_prefs.ini: canvas GUID hex -> "reference" | "match_main" | "WxH".
        std::vector<std::pair<std::string, std::string>> m_resPrefs;

        // ---- Panels ----
        float  m_leftW = 240.f, m_rightW = 340.f;        // the side columns (editor_prefs.ini)
        bool   m_panels = true;                          // the side columns shown
        bool   m_keepVisual = true;                      // Details: anchors / pivot keep the rect
        int    m_sideTab = 0;                            // the right column: 0 Details, 1 Check
        // The inline text editor (double-click a text node; Enter commits, Ctrl+Enter
        // a new line, Esc cancels).
        Entity            m_textEdit = NullEntity;
        std::vector<char> m_textBuf;
        bool              m_textFocus = false;
        float             m_textAt[2] = { 0.f, 0.f };    // screen px of the editor popup
        // The Palette's UI templates: .ipfb files whose root is a UI node.
        std::vector<std::string> m_templates;
        bool                     m_templatesScanned = false;
        std::vector<Issue>       m_issues;               // the Check page's last run
        bool                     m_issuesRun = false;
        Entity                   m_issuesSubject = NullEntity;   // ... of which canvas

        // ---- Asset tabs ----
        std::uint32_t m_nextUid = 1;
        std::vector<std::string> m_pendingOpenAssets;     // RequestOpenAsset
        int        m_pendingSave = 0;                     // 1 save, 2 save as (m_pendingSaveAs)
        std::string m_pendingSaveAs;
        int        m_pendingUndo = 0;                     // > 0 redo steps, < 0 undo steps
        bool       m_pendingDuplicate = false;
        bool       m_focused = false;                     // keyboard focus in the last Draw
        std::uint32_t m_confirmClose = 0;                 // a dirty tab's X: its uid
        bool       m_confirmCloseAll = false;             // the window's X with dirty tabs
        bool       m_pendingRevert = false;               // the asset bar's Revert / Reload (PPA)
        // The last description sent (a parked tab's view keeps showing it, frozen), and
        // parked stages of tabs closed during Play (their ids unparked at Stop).
        UIView::Desc m_lastDesc;
        bool         m_hasLastDesc = false;
        std::vector<std::shared_ptr<Undo::EntitySubtreeSnapshot>> m_parkedDiscards;
        bool         m_harnessOffscreenOff = false;      // reverse arm: a canvas stage is not UICanvasOffscreen
        // Draw is running: nothing may add or close a tab (Draw holds a Tab*); opens
        // are queued (RequestOpenAsset), closes too (m_pendingClose).
        bool         m_inDraw = false;
        // The close-all question answered Discard: the PPA's close may throw unsaved
        // assets away (any other close of the window asks first).
        bool         m_closeConsented = false;
        bool         m_unfold = false;                   // the next Draw unfolds the window (to ask)
        // The rings of tabs closed during Play - cleared (their parked ids handed back)
        // once the edit world is back.
        std::vector<std::shared_ptr<Undo::EditSink>> m_parkedSinks;

        // ---- Window, tabs and the view ----
        bool                     m_open = false;
        std::vector<Tab>         m_tabs;
        int                      m_active = -1;
        int                      m_focusTab = -1;          // select this tab on the next Draw
        bool                     m_focusWindow = false;    // ... and bring the window forward
        std::vector<Entity>      m_pendingOpen;             // RequestOpen
        std::vector<std::uint32_t> m_pendingClose;          // a tab's x: its uid (an index shifts)
        std::uint32_t            m_viewId = ~0u;            // the active tab's UI view
        int                      m_viewTab = -1;            // which tab owns it
        std::uint64_t            m_descSerial = 0;          // Desc::serial of the last sent description
        std::uint64_t            m_lastDescHash = 0;
        // Settling (PPA): since which editor frame the target has held the picture
        // of the current description, and since which it has also been complete
        // (no image still loading, or kLoadGrace frames of waiting for one).
        bool                     m_shown = false;
        std::uint64_t            m_shownSince = 0;
        bool                     m_complete = false;
        std::uint64_t            m_completeSince = 0;

        // Draw laid the panel out since the last PPA. A panel that was not laid out (a
        // folded window, a hidden dock tab, F11) hands the view back.
        bool                     m_visibleThisFrame = false;
        // The canvas the last Draw laid out (0 = never drawn); the pinned size wins.
        float                    m_canvasW = 0.f, m_canvasH = 0.f;
        std::uint32_t            m_outW = 0, m_outH = 0;    // quantised, debounced
        float                    m_sizeChangedAt = -1.f;
        std::uint32_t            m_pinW = 0, m_pinH = 0;
        std::uint64_t            m_frame = 0;               // editor frame of the last PPA
        UI::Vec2                 m_mainScreen{ 0.f, 0.f };  // the service's, read in PPA
        // Hover read-out (Draw): the design pixel under the cursor.
        bool                     m_hover = false;
        float                    m_hoverX = 0.f, m_hoverY = 0.f;

        // ---- Harness ----
        // Harness (-uieditor_*): the subject, its steps and the reverse arms.
        std::string       m_harnessOpen;                 // -uieditor_open (a canvas name, or a .iui / .ipfb path)
        bool              m_harnessOpenAsset = false;    // ... it is a path: an asset / template tab
        std::string       m_harnessBg;                   // -uieditor_bg (applied at open)
        bool              m_harnessOpened = false;
        bool              m_harnessFailed = false;
        bool              m_harnessParitySkew = false;   // reverse arm: the design screen is 1 px wider
        std::uint8_t      m_harnessDebug = 0;            // reverse arms: UIView::kDebug* (Desc::debug)
        bool              m_harnessAnchorRaw = false;    // reverse arm: anchor presets skip the keep-visual solve
        std::vector<Step> m_steps;

        std::vector<PendingShot> m_shots;
    };
}
