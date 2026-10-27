// EditorUITests.cpp - Dear ImGui Test Engine for the editor (Editor/EditorUITests.h): the engine's
// lifetime, the queue a script asks for, the job that reports it, and the tests themselves. A test
// drives the real widgets (focus, key chords, clicks, drags) and then reads the editor's state
// through EditorUITestAccess, the one friend of EditorLayer that exists for this file.
//
// Test functions run on the engine's coroutine thread while the main thread waits inside the frame,
// so they may read editor state; anything that touches the GPU or the world is requested and left to
// ProcessPendingActions, and the test yields frames until it is done.

#include "Editor/EditorUITests.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorLayer.h"
#include "Editor/EditorLayerShared.h"   // kWindow* titles (the default dock tree)
#include "Editor/EditorViewCube.h"
#include "Editor/MaterialEditSession.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/EditorNotify.h"
#include "Editor/EditorPanels.h"
#include "Editor/FocusTracker.h"
#include "Editor/GameView.h"
#include "ECS/PlayModeController.h"
#include "Input/InputSystem.h"
#include "Renderer/Renderer.h"
#include "ECS/PhysicsComponents.h"
#include "Harness/ControlChannelJobs.h"
#include "Reflection/Reflect.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_coroutine.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_exporters.h"
#include "imgui_test_engine/imgui_te_internal.h"   // ImGuiTestEngine_PassFilter: a filter checked before it is queued

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <typeindex>
#include <vector>

// The editor state the tests read and the few requests they make.
struct EditorUITestAccess
{
    static bool        SceneDirty(EditorLayer& l)              { return l.m_sceneDirty; }
    static std::size_t SceneUndoCount(EditorLayer& l)          { return l.m_editHistory.UndoCount(); }
    static std::string ActiveDocument(EditorLayer& l)          { return l.m_documents.Active().kind; }
    static bool        ShortcutsOpen(EditorLayer& l)           { return l.m_showKeyboardShortcuts; }
    static void        SetShortcutsOpen(EditorLayer& l, bool v) { l.m_showKeyboardShortcuts = v; }
    static void        BeginRebind(EditorLayer& l, const char* id) { l.m_rebindCommandId = id; }
    static bool        NewSceneQueued(EditorLayer& l)          { return l.m_pendingNewScene; }
    static bool        ExitApproved(EditorLayer& l)            { return l.m_exitApproved; }
    static bool        RequestExit(EditorLayer& l)             { return l.RequestExit(); }
    static const std::vector<EditorLayer::UnsavedDocument>& ExitDocuments(EditorLayer& l) { return l.m_unsavedDocuments; }
    static std::string LastSaveResult(EditorLayer& l)          { return l.m_lastSaveResult; }
    static int         ViewMode(EditorLayer& l)                { return static_cast<int>(l.m_editorViewMode); }
    static void        SetViewMode(EditorLayer& l, int v)      { l.m_editorViewMode = static_cast<EditorLayer::EditorViewMode>(v); }
    static void        ForceViewCube(EditorLayer& l, bool v)   { l.m_viewCubeForTests = v; }
    static const float* ViewCubeRect(EditorLayer& l)           { return l.m_viewCubeRect; }
    static const DirectX::XMFLOAT4X4& View(EditorLayer& l)     { return l.m_renderView.viewMatrix; }
    static Entity      Selected(EditorLayer& l)                { return l.m_selectedEntity; }
    static void        Select(EditorLayer& l, Entity e, bool add)
    {
        l.SelectEntity(e, add ? EditorLayer::SelectMode::Add : EditorLayer::SelectMode::Replace);
    }
    // The Inspector's Add Component: deferred to the frame boundary, one undo step.
    static void        AddComponent(EditorLayer& l, Entity e, std::type_index type)
    {
        const auto it = l.m_componentEditors.find(type);
        if (it != l.m_componentEditors.end()) l.AddComponentWithUndo(e, type, it->second.add);
    }
    template <class T>
    static T*          Get(EditorLayer& l, Entity e)           { return l.m_world ? l.m_world->GetComponent<T>(e) : nullptr; }
    static std::vector<const Reflect::FieldDescriptor*> Mixed(EditorLayer& l, Entity e, std::type_index type)
    {
        return l.MixedReflectedFields(e, type);
    }
    static bool        InspectorLocked(EditorLayer& l)         { return l.m_inspectorLocked; }
    static bool        HierarchyShown(EditorLayer& l)          { return l.m_showHierarchy; }
    static void        ShowProjectSettings(EditorLayer& l, bool v) { l.m_showGraphicsSettings = v; }
    static std::shared_ptr<const Editor::AssetCatalog::Snapshot> Catalog(EditorLayer& l) { return l.m_assetCatalog.Current(); }
    static bool        SearchAll(EditorLayer& l)               { return l.m_assetSearchAll; }
    static void        ResetAssetSearch(EditorLayer& l)
    {
        l.m_assetFilter[0] = '\0';
        l.m_assetSearchAll = false;
    }
    static void        ClearInspectorSearch(EditorLayer& l)
    {
        l.m_inspectorSearch[0] = '\0';
        l.m_inspectorFilter = {};
        l.m_inspectorLocked = false;
    }
    // ---- The Game View ----
    // The transport as the toolbar's buttons drive it (applied by App at the next frame).
    static void        Play(EditorLayer& l)                    { l.m_playModeController->RequestPlay(); }
    static void        Pause(EditorLayer& l)                   { l.m_playModeController->RequestPause(); }
    static void        Stop(EditorLayer& l)                    { l.m_playModeController->RequestStop(); }
    static EngineMode  Mode(EditorLayer& l)                    { return l.m_playModeController->Mode(); }
    static std::string Owner(EditorLayer& l)                   { return l.m_renderer ? l.m_renderer->MainViewOwnerName() : ""; }
    static std::uint32_t RenderWidth(EditorLayer& l)           { return l.m_renderer ? l.m_renderer->GetRenderWidth() : 0; }
    static std::uint32_t RenderHeight(EditorLayer& l)          { return l.m_renderer ? l.m_renderer->GetRenderHeight() : 0; }
    static void        MapPointer(EditorLayer& l, float& x, float& y, float& sx, float& sy, bool& host)
    {
        l.MapGamePointer(x, y, sx, sy, host);
    }
};

namespace
{
    struct Run
    {
        std::string   filter;
        std::string   junitPath;
        std::string   scratchDir;
        std::uint64_t job = 0;
        bool          active = false;
        bool          queued = false;   // handed to the engine (it must have seen frames first)
    };

    // The engine refuses a queue while its frame count lags the ImGui context's (it takes that for
    // missing hooks): a run waits this many of its frames after a start.
    constexpr int kFramesBeforeQueue = 3;
    int g_framesSinceStart = 0;

    ImGuiTestEngine*                  g_engine = nullptr;
    EditorLayer*                      g_layer  = nullptr;
    Run                               g_run;
    std::vector<Editor::UITests::Result> g_results;

    const char* StatusName(ImGuiTestStatus s)
    {
        switch (s)
        {
            case ImGuiTestStatus_Success:   return "Success";
            case ImGuiTestStatus_Queued:    return "Queued";
            case ImGuiTestStatus_Running:   return "Running";
            case ImGuiTestStatus_Error:     return "Error";
            case ImGuiTestStatus_Suspended: return "Suspended";
            default:                        return "Unknown";
        }
    }

    // A fixture copied under the run's scratch folder: tests save it, the tracked file stays as it is.
    std::string ScratchCopy(const char* fixture)
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(Sys::U8Path(g_run.scratchDir), ec);
        const fs::path src = Sys::U8Path(fixture);
        const fs::path dst = Sys::U8Path(g_run.scratchDir) / src.filename();
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        return Sys::PathToUtf8(dst);
    }

    // Yields frames until @p done, at most @p frames; true when it happened.
    template <class Done>
    bool YieldUntil(ImGuiTestContext* ctx, int frames, Done done)
    {
        for (int i = 0; i < frames; ++i)
        {
            if (done()) return true;
            ctx->Yield();
        }
        return done();
    }

    // Ctrl+Z on the Hierarchy until the scene's history is back to @p count.
    void UndoBackTo(ImGuiTestContext* ctx, EditorLayer& l, std::size_t count)
    {
        for (int guard = 0; guard < 16 && EditorUITestAccess::SceneUndoCount(l) > count; ++guard)
        {
            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
        }
    }

    std::string ReadFile(const std::string& path)
    {
        std::ifstream f(Sys::U8Path(path), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // The ImGui id of the Material Editor's tab for @p sessionPath (MaterialEditorWindow::Draw: a "##mattabs"
    // bar at the window's root, each tab labelled "<name>[ *]###mattab<path>"; the hash restarts at "###").
    // 0 when that tab is not in the bar.
    ImGuiID MaterialTabId(ImGuiTestContext* ctx, const std::string& sessionPath)
    {
        ImGuiWindow* w = ctx->GetWindowByRef("//Material Editor###MaterialEditor");
        if (!w) return 0;
        const ImGuiID bar = ImHashStr("##mattabs", 0, w->ID);
        const ImGuiID tab = ImHashStr(("###mattab" + sessionPath).c_str(), 0, bar);
        ImGuiTabBar* tabBar = ImGui::GetCurrentContext()->TabBars.GetByKey(bar);
        return tabBar && ImGui::TabBarFindTabByID(tabBar, tab) ? tab : 0;
    }

    // Where a window of the default dock tree sits.
    struct DockedRect
    {
        bool    docked = false;
        ImGuiID root = 0;          // the dock tree it is in
        ImGuiID node = 0;
        bool    frontTab = false;  // its tab is the node's selected one
        ImVec2  pos, size;
    };

    DockedRect DockedRectOf(const char* windowName)
    {
        DockedRect r;
        const ImGuiWindow* w = ImGui::FindWindowByName(windowName);
        if (!w) return r;
        r.docked = w->DockIsActive && w->DockNode;
        if (r.docked)
        {
            r.root     = ImGui::DockNodeGetRootNode(w->DockNode)->ID;
            r.node     = w->DockNode->ID;
            r.frontTab = w->DockNode->SelectedTabId == w->TabId;
        }
        r.pos  = w->Pos;
        r.size = w->Size;
        return r;
    }

    constexpr const char* kDefaultDocked[] = { kWindowHierarchy, kWindowViewport, kWindowInspector,
                                               kWindowContentBrowser, kWindowOutputLog };

    // ==== The tests ====

    void RegisterCommandTests(ImGuiTestEngine* e)
    {
        // F1 opens and closes Help > Keyboard Shortcuts through the registry's chord.
        ImGuiTest* t = IM_REGISTER_TEST(e, "commands", "f1_toggles_shortcuts");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            EditorUITestAccess::SetShortcutsOpen(l, false);
            ctx->Yield(2);
            ctx->WindowFocus("//Hierarchy");
            ctx->KeyPress(ImGuiKey_F1);
            ctx->Yield(2);
            IM_CHECK(EditorUITestAccess::ShortcutsOpen(l));
            ctx->WindowFocus("//Hierarchy");
            ctx->KeyPress(ImGuiKey_F1);
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::ShortcutsOpen(l));
        };

        // A popup owns the keyboard: with the command palette open, Ctrl+N is not New Scene; with a modal that
        // types nothing up (the exit guard's), F1 does not open the shortcuts.
        t = IM_REGISTER_TEST(e, "commands", "popup_blocks_chords");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            ctx->WindowFocus("//Hierarchy");
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P);
            ctx->Yield(3);
            IM_CHECK(ImGui::IsPopupOpen("##CommandPalette", ImGuiPopupFlags_AnyPopupLevel));
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_N);
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::NewSceneQueued(l));
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield(2);
            IM_CHECK(!ImGui::IsPopupOpen("##CommandPalette", ImGuiPopupFlags_AnyPopupLevel));

            // The palette's filter field holds the keys by itself; the modal holds them only as a popup.
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            EditorUITestAccess::SetShortcutsOpen(l, false);
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Create/Empty Entity");
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::RequestExit(l));
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists("//Unsaved Changes/Cancel"));
            IM_CHECK(!ImGui::GetIO().WantTextInput);
            ctx->KeyPress(ImGuiKey_F1);
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::ShortcutsOpen(l));
            ctx->SetRef("//Unsaved Changes");
            ctx->ItemClick("Cancel");
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::ExitApproved(l));
            UndoBackTo(ctx, l, base);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), base);
        };

        // A scene edit made through the menu, undone with the chord, is clean again (the save point);
        // a text field's Ctrl+Z stays the field's.
        t = IM_REGISTER_TEST(e, "commands", "undo_to_save_point_and_text_field");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            IM_CHECK(!EditorUITestAccess::SceneDirty(l));
            const std::size_t before = EditorUITestAccess::SceneUndoCount(l);
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Create/Empty Entity");
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), before + 1);
            IM_CHECK(EditorUITestAccess::SceneDirty(l));

            // Typing in the Hierarchy's filter and pressing Ctrl+Z there leaves the scene edit alone.
            ctx->SetRef("//Hierarchy");
            ctx->ItemClick("##hierFilter");
            ctx->KeyChars("zz");
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), before + 1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_A);
            ctx->KeyPress(ImGuiKey_Delete);
            ctx->KeyPress(ImGuiKey_Enter);

            // The chord on the Hierarchy undoes it, back to the state the scene was loaded in.
            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(2);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), before);
            IM_CHECK(!EditorUITestAccess::SceneDirty(l));
            IM_CHECK_EQ(EditorUITestAccess::ActiveDocument(l), std::string("scene"));
        };

        // Ctrl+S with a material file tab focused saves the material, never the scene; the Edit
        // menu's Undo acts on that tab; closing the editor lists the unsaved material and Cancel
        // keeps the editor open.
        t = IM_REGISTER_TEST(e, "commands", "material_tab_owns_save_and_exit_lists_it");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            auto& mw = Editor::MaterialEditorWindow::Get();
            const std::string path = ScratchCopy("tests/fixtures/mateditor/M_base.imat");
            mw.RequestOpen(path);
            IM_CHECK(YieldUntil(ctx, 120, [&] { return mw.ActiveSession() != nullptr; }));
            std::string why;
            IM_CHECK(mw.ActiveSession()->Set({ { "roughnessMin", "0.25" } }, why));
            ctx->Yield(2);
            IM_CHECK(mw.ActiveSession()->Dirty());

            ctx->WindowFocus("//Material Editor###MaterialEditor");
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::ActiveDocument(l), std::string("material"));

            // The exit guard lists it; Cancel keeps the editor.
            IM_CHECK(!EditorUITestAccess::RequestExit(l));
            bool listed = false;
            for (const auto& d : EditorUITestAccess::ExitDocuments(l))
                listed = listed || (d.owner == "Material Editor" && d.name.find("M_base.imat") != std::string::npos);
            IM_CHECK(listed);
            ctx->Yield(3);
            ctx->SetRef("//Unsaved Changes");
            ctx->ItemClick("Cancel");
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::ExitApproved(l));

            // Ctrl+S: the material's file, not the scene's.
            const std::string sceneSave = EditorUITestAccess::LastSaveResult(l);
            ctx->WindowFocus("//Material Editor###MaterialEditor");
            ctx->Yield(2);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_S);
            IM_CHECK(YieldUntil(ctx, 120, [&] { return mw.ActiveSession() && !mw.ActiveSession()->Dirty(); }));
            IM_CHECK_EQ(EditorUITestAccess::LastSaveResult(l), sceneSave);

            // Edit > Undo while the tab is the active document: the material's own history.
            IM_CHECK(mw.ActiveSession()->Set({ { "roughnessMin", "0.5" } }, why));
            ctx->WindowFocus("//Material Editor###MaterialEditor");
            ctx->Yield(2);
            const std::size_t sceneUndo = EditorUITestAccess::SceneUndoCount(l);
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Edit/###doc.undo");
            IM_CHECK(YieldUntil(ctx, 60, [&] { return mw.ActiveSession() && !mw.ActiveSession()->Dirty(); }));
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), sceneUndo);

            std::string closeWhy;
            mw.CloseAll(true, closeWhy);
            ctx->Yield(2);
        };

        // A dirty tab's x asks before it closes: Cancel keeps the tab and its edits, Discard closes it with the
        // file as it was, Save writes the file (not the scene) and then closes the tab.
        t = IM_REGISTER_TEST(e, "commands", "dirty_tab_close_asks");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            auto& mw = Editor::MaterialEditorWindow::Get();
            const std::string path = ScratchCopy("tests/fixtures/mateditor/M_base.imat");
            const std::string onDisk = ReadFile(path);
            IM_CHECK(!onDisk.empty());
            const int tabsBefore = mw.TabCount();
            const char* kAsk = "//Unsaved material edits##mateditor";
            const std::string askCancel = std::string(kAsk) + "/Cancel";
            std::string why;

            // The tab open with an edit: its id in the tab bar, 0 when that did not happen.
            const auto openWithEdit = [&]() -> ImGuiID {
                mw.RequestOpen(path);
                if (!YieldUntil(ctx, 120, [&] { return mw.ActiveSession() != nullptr; })) return 0;
                if (!mw.ActiveSession()->Set({ { "roughnessMin", "0.25" } }, why)) return 0;
                ctx->Yield(2);
                return mw.ActiveSession()->Dirty() ? MaterialTabId(ctx, mw.ActiveSession()->Path()) : 0;
            };

            ImGuiID tab = openWithEdit();
            IM_CHECK(tab != 0);
            ctx->TabClose(tab);
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists(askCancel.c_str()));   // the x asked
            IM_CHECK_EQ(mw.TabCount(), tabsBefore + 1);     // and closed nothing
            ctx->SetRef(kAsk);
            ctx->ItemClick("Cancel");
            ctx->Yield(2);
            IM_CHECK_EQ(mw.TabCount(), tabsBefore + 1);
            IM_CHECK(mw.ActiveSession() && mw.ActiveSession()->Dirty());

            tab = openWithEdit();   // the open tab, focused again
            IM_CHECK(tab != 0);
            ctx->TabClose(tab);
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists(askCancel.c_str()));
            ctx->SetRef(kAsk);
            ctx->ItemClick("Discard");
            IM_CHECK(YieldUntil(ctx, 30, [&] { return mw.TabCount() == tabsBefore; }));
            IM_CHECK(ReadFile(path) == onDisk);

            const std::string sceneSave = EditorUITestAccess::LastSaveResult(l);
            tab = openWithEdit();
            IM_CHECK(tab != 0);
            ctx->TabClose(tab);
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists(askCancel.c_str()));
            ctx->SetRef(kAsk);
            ctx->ItemClick("Save");
            IM_CHECK(YieldUntil(ctx, 120, [&] { return mw.TabCount() == tabsBefore; }));
            IM_CHECK(ReadFile(path) != onDisk);
            IM_CHECK_EQ(EditorUITestAccess::LastSaveResult(l), sceneSave);

            *mw.OpenFlag() = false;
            ctx->Yield(2);
        };

        // A chord changed in Help > Keyboard Shortcuts is the command's until reset.
        t = IM_REGISTER_TEST(e, "commands", "rebind_in_shortcuts_window");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            auto& reg = Editor::CommandRegistry::Get();
            EditorUITestAccess::SetShortcutsOpen(l, true);
            ctx->Yield(3);
            // What a click on the About row's chord cell does; the chord then comes from the keyboard.
            EditorUITestAccess::BeginRebind(l, "help.about");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_K);
            ctx->Yield(2);
            IM_CHECK_EQ(Editor::FormatChord(reg.ChordOf("help.about")), std::string("Ctrl+Alt+K"));
            reg.ResetChord("help.about");
            IM_CHECK(Editor::FormatChord(reg.ChordOf("help.about")).empty());
            EditorUITestAccess::SetShortcutsOpen(l, false);
            ctx->Yield(2);
        };
    }

    void RegisterViewportTests(ImGuiTestEngine* e)
    {
        // Clicking the view cube's Top face selects the Top ortho view (Editor::OrthoViews).
        ImGuiTest* t = IM_REGISTER_TEST(e, "viewport", "view_cube_top_face");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            EditorUITestAccess::SetViewMode(l, 0);
            EditorUITestAccess::ForceViewCube(l, true);
            ctx->Yield(3);
            const float* r = EditorUITestAccess::ViewCubeRect(l);   // x, y, size (0 = not drawn)
            IM_CHECK(r[2] > 0.f);
            // The Top face's centre, projected the way the cube draws it.
            const Editor::ViewCube::Basis b = Editor::ViewCube::BasisOfView(EditorUITestAccess::View(l));
            const DirectX::XMFLOAT3 c = Editor::ViewCube::ToView(b, Editor::ViewCube::FaceNormal(0));
            const float half = r[2] * 0.5f, edge = half * 0.52f;
            IM_CHECK(c.z < -0.1f);   // the default view looks down at the scene: Top is visible
            ctx->MouseMoveToPos(ImVec2(r[0] + half + c.x * edge, r[1] + half - c.y * edge));
            ctx->MouseClick(0);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::ViewMode(l), 1);   // EditorViewMode::Top
            EditorUITestAccess::SetViewMode(l, 0);
            EditorUITestAccess::ForceViewCube(l, false);
            ctx->Yield(2);
        };
    }

    // A new entity with a RigidBody, made the way a user makes one (Create > Empty Entity, then the
    // Inspector's Add Component): two undo steps. NullEntity when either did not happen.
    Entity NewRigidBody(ImGuiTestContext* ctx, EditorLayer& l)
    {
        ctx->SetRef("//EditorHost");
        ctx->MenuClick("Create/Empty Entity");
        ctx->Yield(2);
        const Entity e = EditorUITestAccess::Selected(l);
        if (e == NullEntity) return NullEntity;
        EditorUITestAccess::AddComponent(l, e, std::type_index(typeid(RigidBodyComponent)));
        if (!YieldUntil(ctx, 30, [&] { return EditorUITestAccess::Get<RigidBodyComponent>(l, e) != nullptr; }))
            return NullEntity;
        return e;
    }

    void RegisterInspectorTests(ImGuiTestEngine* e)
    {
        // A bit set drawn by the generic Flags kind: its preset button ORs the bits in, one undo step,
        // and the undo puts back only that field.
        ImGuiTest* t = IM_REGISTER_TEST(e, "inspector", "flag_preset_and_undo");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity body = NewRigidBody(ctx, l);
            IM_CHECK(body != NullEntity);
            RigidBodyComponent* rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            rb->motion = RigidBodyComponent::Motion::Dynamic;
            rb->lockedAxes = RigidBodyComponent::LockTranslationX;
            EditorUITestAccess::Select(l, body, false);
            ctx->Yield(3);
            const std::size_t edits = EditorUITestAccess::SceneUndoCount(l);

            ctx->SetRef("//Inspector");
            ctx->ItemClick("**/Lock Rot");
            ctx->Yield(2);
            rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            IM_CHECK_EQ(static_cast<int>(rb->lockedAxes),
                        static_cast<int>(RigidBodyComponent::LockTranslationX | RigidBodyComponent::LockRotationAll));
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), edits + 1);

            rb->friction = 0.33f;   // a value no recorded field holds: the undo must leave it
            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            IM_CHECK_EQ(static_cast<int>(rb->lockedAxes), static_cast<int>(RigidBodyComponent::LockTranslationX));
            IM_CHECK_EQ(rb->friction, 0.33f);
            UndoBackTo(ctx, l, base);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), base);
        };

        // With two bodies selected, typing a mass writes the mass of both and nothing else: one undo
        // step, the friction each had kept, the fields that differ drawn mixed.
        t = IM_REGISTER_TEST(e, "inspector", "multi_edit_writes_one_field");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity a = NewRigidBody(ctx, l);
            const Entity b = NewRigidBody(ctx, l);
            IM_CHECK(a != NullEntity && b != NullEntity);
            RigidBodyComponent* ra = EditorUITestAccess::Get<RigidBodyComponent>(l, a);
            RigidBodyComponent* rb = EditorUITestAccess::Get<RigidBodyComponent>(l, b);
            ra->motion = rb->motion = RigidBodyComponent::Motion::Dynamic;
            ra->mass = 2.f;  ra->friction = 0.2f;
            rb->mass = 3.f;  rb->friction = 0.8f;
            EditorUITestAccess::Select(l, b, false);
            EditorUITestAccess::Select(l, a, true);   // a is the primary the Inspector shows
            ctx->Yield(3);

            std::vector<std::string> mixed;
            for (const Reflect::FieldDescriptor* f : EditorUITestAccess::Mixed(l, a, std::type_index(typeid(RigidBodyComponent))))
                mixed.push_back(f->name);
            IM_CHECK(std::find(mixed.begin(), mixed.end(), "mass") != mixed.end());
            IM_CHECK(std::find(mixed.begin(), mixed.end(), "friction") != mixed.end());
            IM_CHECK(std::find(mixed.begin(), mixed.end(), "restitution") == mixed.end());

            const std::size_t edits = EditorUITestAccess::SceneUndoCount(l);
            ctx->SetRef("//Inspector");
            ctx->ItemInputValue("**/Mass (kg)", 5.0f);
            ctx->Yield(3);
            ra = EditorUITestAccess::Get<RigidBodyComponent>(l, a);
            rb = EditorUITestAccess::Get<RigidBodyComponent>(l, b);
            IM_CHECK_EQ(ra->mass, 5.f);
            IM_CHECK_EQ(rb->mass, 5.f);
            IM_CHECK_EQ(ra->friction, 0.2f);
            IM_CHECK_EQ(rb->friction, 0.8f);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), edits + 1);

            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            ra = EditorUITestAccess::Get<RigidBodyComponent>(l, a);
            rb = EditorUITestAccess::Get<RigidBodyComponent>(l, b);
            IM_CHECK_EQ(ra->mass, 2.f);
            IM_CHECK_EQ(rb->mass, 3.f);
            UndoBackTo(ctx, l, base);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), base);
        };

        // One drag on a number is one undo step: the value follows the mouse over several frames while
        // nothing is recorded, the release records the gesture, and Ctrl+Z puts back the value from before
        // the drag (not one along it).
        t = IM_REGISTER_TEST(e, "inspector", "drag_is_one_undo_step");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity body = NewRigidBody(ctx, l);
            IM_CHECK(body != NullEntity);
            RigidBodyComponent* rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            rb->motion = RigidBodyComponent::Motion::Dynamic;
            rb->mass = 2.f;
            EditorUITestAccess::Select(l, body, false);
            ctx->Yield(3);
            const std::size_t edits = EditorUITestAccess::SceneUndoCount(l);

            ctx->SetRef("//Inspector");
            ctx->MouseMove("**/Mass (kg)");   // scrolled into view, the mouse at its centre
            const ImVec2 start = ctx->Inputs->MousePosValue;
            ctx->MouseDown(0);
            float last = 2.f;
            for (int step = 1; step <= 4; ++step)   // each step one frame with the mouse moved, one without
            {
                ctx->MouseMoveToPos(ImVec2(start.x + 16.f * step, start.y));
                const float now = EditorUITestAccess::Get<RigidBodyComponent>(l, body)->mass;
                IM_CHECK_GT(now, last);   // the drag writes as it goes
                last = now;
                IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), edits);   // held: nothing recorded yet
            }
            ctx->MouseUp(0);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), edits + 1);
            IM_CHECK_EQ(EditorUITestAccess::Get<RigidBodyComponent>(l, body)->mass, last);

            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::Get<RigidBodyComponent>(l, body)->mass, 2.f);
            UndoBackTo(ctx, l, base);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), base);
        };
    }

    void RegisterInspectorRowTests(ImGuiTestEngine* e)
    {
        // The Inspector's search draws only the fields it matches.
        ImGuiTest* t = IM_REGISTER_TEST(e, "inspector", "search_shows_matching_fields");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity body = NewRigidBody(ctx, l);
            IM_CHECK(body != NullEntity);
            EditorUITestAccess::Get<RigidBodyComponent>(l, body)->motion = RigidBodyComponent::Motion::Dynamic;
            EditorUITestAccess::Select(l, body, false);
            ctx->Yield(3);
            ctx->SetRef("//Inspector");
            IM_CHECK(ctx->ItemExists("**/Mass (kg)"));
            ctx->ItemInputValue("**/##inspectorSearch", "damp");
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists("**/Linear Damping"));
            IM_CHECK(ctx->ItemExists("**/Angular Damping"));
            IM_CHECK(!ctx->ItemExists("**/Mass (kg)"));
            EditorUITestAccess::ClearInspectorSearch(l);
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists("**/Mass (kg)"));
            UndoBackTo(ctx, l, base);
        };

        // A field's right-click menu: Reset to Default writes the default instance's value as one undo
        // step; the marker's state follows.
        t = IM_REGISTER_TEST(e, "inspector", "context_reset_to_default");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity body = NewRigidBody(ctx, l);
            IM_CHECK(body != NullEntity);
            RigidBodyComponent* rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            const float defaultMass = RigidBodyComponent{}.mass;
            rb->motion = RigidBodyComponent::Motion::Dynamic;
            rb->mass = 7.f;
            EditorUITestAccess::Select(l, body, false);
            ctx->Yield(3);
            const std::size_t edits = EditorUITestAccess::SceneUndoCount(l);
            ctx->SetRef("//Inspector");
            ctx->ItemClick("**/Mass (kg)", ImGuiMouseButton_Right);
            ctx->Yield(2);
            ctx->SetRef("//$FOCUSED");
            ctx->ItemClick("Reset to Default");
            ctx->Yield(3);
            rb = EditorUITestAccess::Get<RigidBodyComponent>(l, body);
            IM_CHECK_EQ(rb->mass, defaultMass);
            IM_CHECK_EQ(EditorUITestAccess::SceneUndoCount(l), edits + 1);
            ctx->WindowFocus("//Hierarchy");
            ctx->Yield(1);
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
            ctx->Yield(2);
            IM_CHECK_EQ(EditorUITestAccess::Get<RigidBodyComponent>(l, body)->mass, 7.f);
            UndoBackTo(ctx, l, base);
        };

        // Locked, the Inspector keeps its entity while the selection moves on, and its edits reach that
        // entity, not the selection.
        t = IM_REGISTER_TEST(e, "inspector", "lock_keeps_entity");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity a = NewRigidBody(ctx, l);
            const Entity b = NewRigidBody(ctx, l);
            IM_CHECK(a != NullEntity && b != NullEntity);
            EditorUITestAccess::Get<RigidBodyComponent>(l, a)->motion = RigidBodyComponent::Motion::Dynamic;
            EditorUITestAccess::Get<RigidBodyComponent>(l, b)->motion = RigidBodyComponent::Motion::Dynamic;
            EditorUITestAccess::Get<RigidBodyComponent>(l, a)->mass = 2.f;
            EditorUITestAccess::Get<RigidBodyComponent>(l, b)->mass = 3.f;
            EditorUITestAccess::Select(l, a, false);
            ctx->Yield(3);
            ctx->SetRef("//Inspector");
            ctx->ItemClick("**/Lock");
            ctx->Yield(2);
            IM_CHECK(EditorUITestAccess::InspectorLocked(l));
            EditorUITestAccess::Select(l, b, false);
            ctx->Yield(3);
            IM_CHECK_EQ(EditorUITestAccess::Selected(l), b);   // the selection moved
            ctx->SetRef("//Inspector");
            ctx->ItemInputValue("**/Mass (kg)", 9.0f);
            ctx->Yield(3);
            IM_CHECK_EQ(EditorUITestAccess::Get<RigidBodyComponent>(l, a)->mass, 9.f);
            IM_CHECK_EQ(EditorUITestAccess::Get<RigidBodyComponent>(l, b)->mass, 3.f);
            IM_CHECK_EQ(EditorUITestAccess::Selected(l), b);   // and stayed where it was
            ctx->ItemClick("**/Locked");
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::InspectorLocked(l));
            UndoBackTo(ctx, l, base);
        };

        // Project Settings' search lists the matching rows on one page, with the same row chrome:
        // Copy Path names the row.
        t = IM_REGISTER_TEST(e, "settings", "search_lists_rows_with_chrome");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            EditorUITestAccess::ShowProjectSettings(l, true);
            ctx->Yield(3);
            ctx->SetRef("//Project Settings###Graphics Settings");
            ctx->ItemInputValue("**/##settingsSearch", "vsync");
            ctx->Yield(2);
            ctx->ItemClick("**/Matching settings");
            ctx->Yield(3);
            IM_CHECK(ctx->ItemExists("**/VSync##display.vsync"));
            ctx->ItemClick("**/VSync##display.vsync", ImGuiMouseButton_Right);
            ctx->Yield(2);
            ctx->SetRef("//$FOCUSED");
            ctx->ItemClick("Copy Path");
            ctx->Yield(2);
            const char* clip = ImGui::GetClipboardText();
            IM_CHECK_STR_EQ(clip ? clip : "", "display.vsync");
            ctx->SetRef("//Project Settings###Graphics Settings");
            ctx->ItemInputValue("**/##settingsSearch", "");
            ctx->Yield(2);
            EditorUITestAccess::ShowProjectSettings(l, false);
            ctx->Yield(2);
        };
    }

    void RegisterPanelTests(ImGuiTestEngine* e)
    {
        // A registered tool panel opens from the Window menu through the registry, and a workspace puts
        // back the panels it was saved with.
        ImGuiTest* t = IM_REGISTER_TEST(e, "panels", "registry_menu_and_workspace");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            Editor::PanelRegistry& reg = Editor::PanelRegistry::Get();
            IM_CHECK(reg.Find("core.poolWatermarks") != nullptr);
            reg.SetOpen("core.poolWatermarks", false);
            ctx->Yield(2);
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Window/Pool Watermarks");
            ctx->Yield(3);
            IM_CHECK(reg.IsOpen("core.poolWatermarks"));
            IM_CHECK(ImGui::FindWindowByName("Pool Watermarks") != nullptr);

            reg.SaveWorkspace("uitest", "");
            reg.SetOpen("core.poolWatermarks", false);
            ctx->Yield(2);
            IM_CHECK(!reg.IsOpen("core.poolWatermarks"));
            std::string layout;
            IM_CHECK(reg.OpenWorkspace("uitest", layout));
            ctx->Yield(2);
            IM_CHECK(reg.IsOpen("core.poolWatermarks"));
            reg.DeleteWorkspace("uitest");
            reg.SetOpen("core.poolWatermarks", false);
            ctx->Yield(2);
        };

        // A component header's right-click View Bytes opens the Byte View on that component.
        t = IM_REGISTER_TEST(e, "panels", "view_bytes_from_inspector");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            const std::size_t base = EditorUITestAccess::SceneUndoCount(l);
            const Entity body = NewRigidBody(ctx, l);
            IM_CHECK(body != NullEntity);
            EditorUITestAccess::Select(l, body, false);
            ctx->Yield(3);
            Editor::PanelRegistry::Get().SetOpen("core.byteView", false);
            ctx->SetRef("//Inspector");
            ctx->ItemClick("**/Rigid Body", ImGuiMouseButton_Right);
            ctx->Yield(2);
            ctx->SetRef("//$FOCUSED");
            ctx->ItemClick("View Bytes");
            ctx->Yield(3);
            IM_CHECK(Editor::PanelRegistry::Get().IsOpen("core.byteView"));
            IM_CHECK(ImGui::FindWindowByName("Byte View") != nullptr);
            Editor::PanelRegistry::Get().SetOpen("core.byteView", false);
            UndoBackTo(ctx, l, base);
        };

        // A job that finishes while the editor watches posts a notice (the toast's text names it).
        t = IM_REGISTER_TEST(e, "panels", "finished_job_posts_a_notice");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            const std::uint64_t job = Harness::Jobs::Create("uitest_job", "notice check", 0);
            IM_CHECK(job != 0);
            ctx->Yield(3);
            Harness::Jobs::Start(job);
            ctx->Yield(2);
            Harness::Jobs::Succeed(job, "{}");
            bool posted = false;
            for (int i = 0; i < 20 && !posted; ++i)
            {
                ctx->Yield(1);
                for (const Editor::Notice& n : Editor::Notices::Get().Recent())
                    posted = posted || n.text.find("uitest_job: notice check") != std::string::npos;
            }
            IM_CHECK(posted);
        };

        // Window > Reset Layout puts the default dock tree back after the panels were moved: every window in
        // its old rect, the Content Browser in front of the Output Log, a hidden Hierarchy shown again. The
        // harness's packs measure the Viewport in this layout.
        t = IM_REGISTER_TEST(e, "panels", "reset_layout_restores_default");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Window/###layout.reset");
            ctx->Yield(3);
            DockedRect before[std::size(kDefaultDocked)];
            for (std::size_t i = 0; i < std::size(kDefaultDocked); ++i)
            {
                before[i] = DockedRectOf(kDefaultDocked[i]);
                IM_CHECK(before[i].docked);
            }
            const DockedRect& browser = before[3];
            const DockedRect& log     = before[4];
            IM_CHECK(browser.node == log.node && browser.frontTab);

            // Moved: the Inspector floating, the log in front, the Hierarchy hidden.
            ctx->UndockWindow((std::string("//") + kWindowInspector).c_str());   // a bare name is under the ref
            ctx->Yield(2);
            IM_CHECK(!DockedRectOf(kWindowInspector).docked);
            ctx->WindowFocus("//Output Log###OutputLog");
            ctx->Yield(2);
            IM_CHECK(!DockedRectOf(kWindowContentBrowser).frontTab);
            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Window/Hierarchy");
            ctx->Yield(2);
            IM_CHECK(!EditorUITestAccess::HierarchyShown(l));
            IM_CHECK(DockedRectOf(kWindowViewport).size.x != before[1].size.x);   // the tree changed shape

            ctx->SetRef("//EditorHost");
            ctx->MenuClick("Window/###layout.reset");
            ctx->Yield(3);
            IM_CHECK(EditorUITestAccess::HierarchyShown(l));
            for (std::size_t i = 0; i < std::size(kDefaultDocked); ++i)
            {
                const DockedRect after = DockedRectOf(kDefaultDocked[i]);
                ctx->LogInfo("%s: %.0f,%.0f %.0fx%.0f (was %.0f,%.0f %.0fx%.0f)", kDefaultDocked[i], after.pos.x,
                             after.pos.y, after.size.x, after.size.y, before[i].pos.x, before[i].pos.y,
                             before[i].size.x, before[i].size.y);
                IM_CHECK(after.docked);
                IM_CHECK(after.root == before[i].root);
                IM_CHECK(std::fabs(after.pos.x - before[i].pos.x) < 0.5f);
                IM_CHECK(std::fabs(after.pos.y - before[i].pos.y) < 0.5f);
                IM_CHECK(std::fabs(after.size.x - before[i].size.x) < 0.5f);
                IM_CHECK(std::fabs(after.size.y - before[i].size.y) < 0.5f);
            }
            IM_CHECK(DockedRectOf(kWindowContentBrowser).node == DockedRectOf(kWindowOutputLog).node);
            IM_CHECK(DockedRectOf(kWindowContentBrowser).frontTab);
        };
    }

    void RegisterContentTests(ImGuiTestEngine* e)
    {
        // The Content Browser's filter over every folder: the background catalog lists an asset of
        // another folder, and the walk's and the search's times are logged.
        ImGuiTest* t = IM_REGISTER_TEST(e, "content", "search_every_folder");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            ctx->SetRef("//Content Browser");
            ctx->ItemInputValue("**/##AssetFilter", "Default_mat");
            ctx->ItemCheck("**/All folders");
            ctx->Yield(2);
            IM_CHECK(EditorUITestAccess::SearchAll(l));
            IM_CHECK(YieldUntil(ctx, 600, [&] { return EditorUITestAccess::Catalog(l) != nullptr; }));
            const auto snap = EditorUITestAccess::Catalog(l);
            bool found = false;
            for (const Editor::AssetCatalog::Entry& entry : snap->entries)
                found = found || entry.lowerName == "default_mat.imat";
            IM_CHECK(found);   // engine/textures/Default_mat.imat, outside the browser's folder
            double micros = 0.0;
            const auto hits = Editor::AssetCatalog().Search("x", 1, &micros);   // an empty catalog answers nothing
            IM_CHECK(hits.empty());
            LOG_INFO("[ASSETCAT] entries=%zu walkMs=%.1f", snap->entries.size(), snap->buildMs);
            ctx->Yield(2);
            IM_CHECK(ctx->ItemExists("**/Default_mat.imat"));   // the result row (the browser shows asset/)
            EditorUITestAccess::ResetAssetSearch(l);
            ctx->Yield(2);
        };
    }

    // ---- The Game View ----

    bool WaitMode(ImGuiTestContext* ctx, EditorLayer& l, EngineMode mode)
    {
        return YieldUntil(ctx, 120, [&] { return EditorUITestAccess::Mode(l) == mode; });
    }

    // Back to Edit with the panel closed, whatever a failed check left behind.
    void ResetGameView(ImGuiTestContext* ctx, EditorLayer& l)
    {
        Editor::GameView& gv = Editor::GameView::Get();
        gv.SetCaptureRequestOverride(-1);
        gv.SetPreset(0);
        if (EditorUITestAccess::Mode(l) != EngineMode::Edit)
        {
            EditorUITestAccess::Stop(l);
            WaitMode(ctx, l, EngineMode::Edit);
        }
        Editor::PanelRegistry::Get().SetOpen(Editor::GameView::kPanelId, false);
        ctx->Yield(3);
    }

    // The Game View open beside the Scene viewport, both showing, in Edit - from a clean state, since a
    // failed check ends a test where it stands.
    bool OpenGameViewBeside(ImGuiTestContext* ctx, EditorLayer& l)
    {
        ResetGameView(ctx, l);
        // A floating window an earlier test left open (the Material Editor) would sit over the picture.
        *Editor::MaterialEditorWindow::Get().OpenFlag() = false;
        Editor::GameView& gv = Editor::GameView::Get();
        Editor::PanelRegistry::Get().SetOpen(Editor::GameView::kPanelId, true);
        gv.RequestDock(Editor::GameView::Dock::BesideScene);
        return YieldUntil(ctx, 30, [] {
            const Editor::FocusTracker& f = Editor::FocusTracker::Get();
            return f.IsVisible(Editor::FocusTracker::kGameView) && f.IsVisible(Editor::FocusTracker::kSceneView);
        });
    }

    void RegisterGameViewTests(ImGuiTestEngine* e)
    {
        // Edit / Play / Pause / Stop with both views showing: the main pipeline follows the game into the Game
        // View at its extent (owner game, the history cut), the Scene keeps the editor camera through the
        // auxiliary view, Stop gives everything back and the auxiliary view goes.
        ImGuiTest* t = IM_REGISTER_TEST(e, "gameview", "route_follows_play_state");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            Editor::GameView& gv = Editor::GameView::Get();
            IM_CHECK(OpenGameViewBeside(ctx, l));
            ctx->Yield(3);
            IM_CHECK(gv.Route() == Editor::MainRoute::SceneEditor);
            IM_CHECK_EQ(EditorUITestAccess::Owner(l), std::string("editor"));
            IM_CHECK(YieldUntil(ctx, 60, [&] { return gv.PanelPicture(0).Live(); }));   // the Edit preview
            IM_CHECK(gv.Camera().reason.empty());

            EditorUITestAccess::Play(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Playing));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::Game; }));
            IM_CHECK_EQ(EditorUITestAccess::Owner(l), std::string("game"));
            IM_CHECK(YieldUntil(ctx, 30, [&] {
                return EditorUITestAccess::RenderWidth(l) == gv.Extent().w
                       && EditorUITestAccess::RenderHeight(l) == gv.Extent().h;
            }));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.SceneAux().Live() && !gv.PanelPicture(0).Live(); }));

            EditorUITestAccess::Pause(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Paused));
            ctx->Yield(3);
            IM_CHECK(gv.Route() == Editor::MainRoute::Game);   // Pause keeps the game's picture
            IM_CHECK(gv.SceneAux().Live());

            EditorUITestAccess::Stop(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Edit));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::SceneEditor; }));
            IM_CHECK_EQ(EditorUITestAccess::Owner(l), std::string("editor"));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return !gv.SceneAux().Live(); }));
            ResetGameView(ctx, l);
        };

        // Only one view showing, or the panel closed: a hidden Game View gives the main pipeline back to the
        // Scene (the editor camera, no auxiliary view), a closed one is the single-view editor.
        t = IM_REGISTER_TEST(e, "gameview", "route_follows_visibility");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            Editor::GameView& gv = Editor::GameView::Get();
            IM_CHECK(OpenGameViewBeside(ctx, l));
            gv.RequestDock(Editor::GameView::Dock::TabWithScene);
            const auto oneShown = [] {
                const Editor::FocusTracker& f = Editor::FocusTracker::Get();
                return f.IsVisible(Editor::FocusTracker::kGameView) != f.IsVisible(Editor::FocusTracker::kSceneView);
            };
            IM_CHECK(YieldUntil(ctx, 30, oneShown));   // one node, one tab in front
            EditorUITestAccess::Play(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Playing));

            gv.RequestFocus(Editor::GameView::Focus::Game);
            IM_CHECK(YieldUntil(ctx, 30, [&] {
                return gv.Route() == Editor::MainRoute::Game
                       && !Editor::FocusTracker::Get().IsVisible(Editor::FocusTracker::kSceneView);
            }));
            ctx->Yield(3);
            IM_CHECK(!gv.SceneAux().Live());   // a hidden Scene renders nothing

            gv.RequestFocus(Editor::GameView::Focus::Scene);
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::SceneEditor; }));
            IM_CHECK_EQ(EditorUITestAccess::Owner(l), std::string("editor"));
            IM_CHECK(l.MainViewObservesPlay());
            ctx->Yield(3);
            IM_CHECK(!gv.SceneAux().Live());

            Editor::PanelRegistry::Get().SetOpen(Editor::GameView::kPanelId, false);
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::SceneGame; }));
            IM_CHECK_EQ(EditorUITestAccess::Owner(l), std::string("game"));
            ResetGameView(ctx, l);
        };

        // The game reads input only while the Game View has focus: focus elsewhere and the input snapshot
        // reads nothing (Input's hostFocus), the editor's single-key shortcuts come back.
        t = IM_REGISTER_TEST(e, "gameview", "focus_gates_game_input");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            Editor::GameView& gv = Editor::GameView::Get();
            IM_CHECK(OpenGameViewBeside(ctx, l));
            EditorUITestAccess::Play(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Playing));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::Game; }));

            ctx->WindowFocus("//Game View");
            IM_CHECK(YieldUntil(ctx, 10, [&] { return gv.GameHasKeyboard() && Input::Get().LastInputs().hostFocus; }));
            ctx->WindowFocus("//Hierarchy");
            IM_CHECK(YieldUntil(ctx, 10, [&] { return !gv.GameHasKeyboard() && !Input::Get().LastInputs().hostFocus; }));
            ctx->WindowFocus("//Viewport");
            ctx->Yield(3);
            IM_CHECK(!Input::Get().LastInputs().hostFocus);   // the Scene's focus is the editor's
            ResetGameView(ctx, l);
            IM_CHECK(Input::Get().LastInputs().hostFocus);    // Edit: the single-view rules again
        };

        // Under the game's capture Esc releases it first (and the game does not get it), a click on the
        // picture captures again; without the game's ask nothing is captured.
        t = IM_REGISTER_TEST(e, "gameview", "esc_releases_capture_first");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            Editor::GameView& gv = Editor::GameView::Get();
            IM_CHECK(OpenGameViewBeside(ctx, l));
            EditorUITestAccess::Play(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Playing));
            IM_CHECK(YieldUntil(ctx, 30, [&] { return gv.Route() == Editor::MainRoute::Game; }));
            ctx->WindowFocus("//Game View");
            ctx->Yield(3);
            IM_CHECK(!gv.Captured());   // the game has not asked
            const bool escPref = gv.EscReleasesCapture();   // the developer's editor_prefs.ini may have it off
            gv.SetEscReleasesCapture(true);

            gv.SetCaptureRequestOverride(1);
            IM_CHECK(YieldUntil(ctx, 10, [&] { return gv.Captured(); }));
            ctx->KeyPress(ImGuiKey_Escape);
            IM_CHECK(YieldUntil(ctx, 10, [&] { return !gv.Captured(); }));
            ctx->Yield(5);
            IM_CHECK(!gv.Captured());   // released stays released while the game still asks
            IM_CHECK(gv.GameHasKeyboard());   // and the next Esc is the game's

            const Editor::GameViewLayout::Rect r = gv.ImageRect();
            ctx->MouseMoveToPos(ImVec2(r.x + r.w * 0.5f, r.y + r.h * 0.5f));
            const bool hoveredPicture = YieldUntil(ctx, 10, [&] { return gv.PictureHovered(); });
            if (!hoveredPicture)
            {
                const ImGuiContext& g = *ImGui::GetCurrentContext();
                LOG_INFO("[UITEST] gameview: not hovered at %.0f,%.0f - hovered window '%s', image %.0f,%.0f %.0fx%.0f",
                         g.IO.MousePos.x, g.IO.MousePos.y, g.HoveredWindow ? g.HoveredWindow->Name : "(none)",
                         r.x, r.y, r.w, r.h);
            }
            IM_CHECK(hoveredPicture);
            ctx->MouseClick(0);
            IM_CHECK(YieldUntil(ctx, 10, [&] { return gv.Captured(); }));   // a click on the picture takes it again

            ctx->WindowFocus("//Hierarchy");
            IM_CHECK(YieldUntil(ctx, 10, [&] { return !gv.Captured(); }));   // focus elsewhere releases
            gv.SetEscReleasesCapture(escPref);
            ResetGameView(ctx, l);
            IM_CHECK(!gv.Captured());
        };

        // A fixed resolution renders at that size and is shown fitted with bars; the Canvas pointer maps through
        // the picture's scale. Free Aspect renders at the panel's size.
        t = IM_REGISTER_TEST(e, "gameview", "presets_letterbox_pointer");
        t->TestFunc = [](ImGuiTestContext* ctx) {
            EditorLayer& l = *g_layer;
            Editor::GameView& gv = Editor::GameView::Get();
            IM_CHECK(OpenGameViewBeside(ctx, l));
            int fixed = -1;
            for (int i = 0; i < Editor::GameViewLayout::kPresetCount; ++i)
                if (std::string(Editor::GameViewLayout::kPresets[i].label) == "1920x1080") fixed = i;
            IM_CHECK(fixed >= 0);
            gv.SetPreset(fixed);
            ctx->Yield(3);
            IM_CHECK(gv.Extent().w == 1920 && gv.Extent().h == 1080);
            const Editor::GameViewLayout::Rect r = gv.ImageRect();
            IM_CHECK(r.w > 0.f && r.h > 0.f);
            IM_CHECK(std::fabs(r.w / r.h - 1920.f / 1080.f) < 0.02f);   // its aspect, never stretched

            EditorUITestAccess::Play(l);
            IM_CHECK(WaitMode(ctx, l, EngineMode::Playing));
            IM_CHECK(YieldUntil(ctx, 30, [&] {
                return EditorUITestAccess::RenderWidth(l) == 1920 && EditorUITestAccess::RenderHeight(l) == 1080;
            }));
            float x = 0.f, y = 0.f, sx = 1.f, sy = 1.f;
            bool host = false;
            EditorUITestAccess::MapPointer(l, x, y, sx, sy, host);
            IM_CHECK(std::fabs(x - r.x) < 0.5f && std::fabs(y - r.y) < 0.5f);
            IM_CHECK(std::fabs(sx - 1920.f / r.w) < 1e-3f && std::fabs(sy - 1080.f / r.h) < 1e-3f);

            gv.SetPreset(0);   // Free Aspect: the panel's own size
            IM_CHECK(YieldUntil(ctx, 30, [&] {
                const Editor::GameViewLayout::Rect fr = gv.ImageRect();
                return EditorUITestAccess::RenderWidth(l) == static_cast<std::uint32_t>(fr.w)
                       && EditorUITestAccess::RenderHeight(l) == static_cast<std::uint32_t>(fr.h);
            }));
            ResetGameView(ctx, l);
        };
    }

    void EnsureEngine(EditorLayer& layer)
    {
        if (g_engine) return;
        g_layer  = &layer;
        g_engine = ImGuiTestEngine_CreateContext();
        ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(g_engine);
        io.ConfigVerboseLevel        = ImGuiTestVerboseLevel_Info;
        io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
        io.ConfigRunSpeed            = ImGuiTestRunSpeed_Fast;
        io.ConfigSavedSettings       = false;   // never into imgui.ini (the harness layout has none)
        io.ConfigCaptureEnabled      = false;   // pictures are the harness's (eng.capture)
        io.ConfigMouseDrawCursor     = false;
        io.ConfigLogToTTY            = false;
        io.CoroutineFuncs            = Coroutine_ImplStdThread_GetInterface();
        RegisterCommandTests(g_engine);
        RegisterViewportTests(g_engine);
        RegisterInspectorTests(g_engine);
        RegisterInspectorRowTests(g_engine);
        RegisterPanelTests(g_engine);
        RegisterContentTests(g_engine);
        RegisterGameViewTests(g_engine);
        ImGuiTestEngine_Start(g_engine, ImGui::GetCurrentContext());
        g_framesSinceStart = 0;
        LOG_INFO("[UITEST] test engine started (imgui_test_engine c043664)");
    }

    void FinishRun()
    {
        g_results.clear();
        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(g_engine, &tests);
        nlohmann::json list = nlohmann::json::array();
        int passed = 0, failed = 0;
        for (ImGuiTest* t : tests)
        {
            const ImGuiTestStatus s = t->Output.Status;
            if (s == ImGuiTestStatus_Unknown) continue;   // not part of this run
            Editor::UITests::Result r{ t->Category, t->Name, s == ImGuiTestStatus_Success, StatusName(s) };
            (r.passed ? passed : failed) += 1;
            LOG_INFO("[UITEST] %s/%s: %s", r.category.c_str(), r.name.c_str(), r.status.c_str());
            list.push_back({ { "category", r.category }, { "name", r.name }, { "passed", r.passed }, { "status", r.status } });
            g_results.push_back(std::move(r));
        }
        if (!g_run.junitPath.empty()) ImGuiTestEngine_Export(g_engine);
        LOG_INFO("[UITEST] run done: passed=%d failed=%d", passed, failed);
        const nlohmann::json out{ { "passed", passed }, { "failed", failed }, { "tests", list },
                                  { "junit", g_run.junitPath } };
        if (failed == 0) Harness::Jobs::Succeed(g_run.job, out.dump());
        else             Harness::Jobs::Fail(g_run.job, "tests_failed", std::to_string(failed) + " UI test(s) failed", {},
                                             out.dump());
        g_run.active = false;
    }
}

namespace Editor::UITests
{
    bool Queue(EditorLayer& layer, const std::string& filter, const std::string& junitPath,
               const std::string& scratchDir, std::uint64_t jobId, std::string& why)
    {
        if (g_run.active) { why = "a UI test run is still going"; return false; }
        EnsureEngine(layer);
        // Each run reports only its own tests: the earlier statuses are cleared.
        ImVector<ImGuiTest*> tests;
        ImGuiTestEngine_GetTestList(g_engine, &tests);
        int matching = 0;
        for (ImGuiTest* t : tests)
        {
            t->Output.Status = ImGuiTestStatus_Unknown;
            if (filter.empty() || ImGuiTestEngine_PassFilter(t, filter.c_str())) ++matching;
        }
        if (matching == 0) { why = "no UI test matches '" + filter + "'"; return false; }

        g_run = Run{ filter, junitPath, scratchDir.empty() ? Sys::ArtifactDir() + "/uitests" : scratchDir, jobId, true,
                     false };
        ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(g_engine);
        io.ExportResultsFilename = g_run.junitPath.empty() ? nullptr : g_run.junitPath.c_str();
        io.ExportResultsFormat   = ImGuiTestEngineExportFormat_JUnitXml;
        Harness::Jobs::Start(jobId);
        return true;
    }

    bool Running() { return g_run.active; }

    void PostFrame()
    {
        if (!g_engine) return;
        ImGuiTestEngine_PostSwap(g_engine);
        ++g_framesSinceStart;
        if (!g_run.active) return;
        if (!g_run.queued)
        {
            if (g_framesSinceStart < kFramesBeforeQueue) return;
            ImGuiTestEngine_QueueTests(g_engine, ImGuiTestGroup_Tests,
                                       g_run.filter.empty() ? nullptr : g_run.filter.c_str());
            g_run.queued = true;
            return;
        }
        if (ImGuiTestEngine_IsTestQueueEmpty(g_engine)) FinishRun();
    }

    const std::vector<Result>& LastResults() { return g_results; }

    void StopEngine()
    {
        if (g_engine) ImGuiTestEngine_Stop(g_engine);
    }

    void DestroyEngine()
    {
        if (!g_engine) return;
        ImGuiTestEngine_DestroyContext(g_engine);
        g_engine = nullptr;
        g_layer  = nullptr;
    }
}

// ==== Reverse arms ====

namespace
{
    struct FixName
    {
        Editor::UITests::Fix fix;
        const char*          name;
    };

    constexpr FixName kFixNames[] = {
        { Editor::UITests::Fix::InspectorDragBracket, "inspector.drag_bracket" },
        { Editor::UITests::Fix::TabCloseAsks,         "material.tab_close_asks" },
        { Editor::UITests::Fix::LayoutReset,          "layout.reset" },
        { Editor::UITests::Fix::PopupOwnsKeys,        "commands.popup_owns_keys" },
        { Editor::UITests::Fix::TextFieldKeepsKeys,   "commands.text_field_keeps_keys" },
        { Editor::UITests::Fix::SavePoint,            "history.save_point" },
    };

    Editor::UITests::Fix ReadReverseFlag()
    {
        // -uitest_reverse=<fix> (harness): puts one fix a UI test guards back the way it was, so that test must
        // fail - the reverse arms of tests/unit/test_editor_ui_tests.py. <fix> is one of inspector.drag_bracket,
        // material.tab_close_asks, layout.reset, commands.popup_owns_keys, commands.text_field_keeps_keys,
        // history.save_point (Editor::UITests::Fix).
        const std::string name = Sys::CommandLine::Get().Get("-uitest_reverse");
        if (name.empty()) return Editor::UITests::Fix::None;
        for (const FixName& f : kFixNames)
            if (name == f.name)
            {
                LOG_WARNING("[UITEST] reverse arm: %s is reverted", f.name);
                return f.fix;
            }
        LOG_ERROR("[UITEST] -uitest_reverse=%s names no fix", name.c_str());
        return Editor::UITests::Fix::None;
    }
}

namespace Editor::UITests
{
    bool Reverted(Fix fix)
    {
        static const Fix s_reverted = ReadReverseFlag();
        return fix != Fix::None && fix == s_reverted;
    }
}
