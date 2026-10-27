// EditorLayer_Commands.cpp - the editor's actions as commands (Editor/EditorCommands.h), the
// documents they act on (Editor/EditorDocuments.h), and the one shortcut dispatcher. The menus, the
// command palette and Help > Keyboard Shortcuts read the same registry, so a chord or a label is
// written once.

#include "Editor/EditorLayer.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorLayerShared.h"   // kWindowOutputLog, WriteStartupSceneToManifest
#include "Editor/EditorTheme.h"
#include "Editor/FontAwesomeIcons.h"
#include "Editor/GameView.h"   // the game's keyboard on the Game View's route
#include "Editor/InputBindingsWindow.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/MeshEditorWindow.h"
#include "Editor/SkeletonEditorWindow.h"
#include "Editor/UIEditorWindow.h"
#include "Harness/ControlChannel.h"
#include "Renderer/Renderer.h"
#include "Scene/SceneDefaults.h"
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // MenuItemEx (the icon column)
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace
{
    using Editor::Command;
    using Editor::CommandScope;
    using Editor::ParseChord;

    constexpr const char* kPlayGuard = "Disabled during Play: the played world is runtime state - press Stop first";

    // The free-function editors' Control verbs: "save" / "undo" / "redo" act on their document.
    void ControlVerb(std::string (*control)(const std::string&), const char* verb)
    {
        const std::string reply = control(std::string(R"({"action":")") + verb + "\"}");
        const nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
        if (!j.is_discarded() && !j.value("ok", false))
            LOG_WARNING("Editor: %s failed: %s", verb, j.value("error", std::string("?")).c_str());
    }

    std::string BaseName(const std::string& path)
    {
        const std::size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // The chords a panel that reads S / Z / Y / C / V itself keeps (the animation documents).
    bool IsDocumentKeyCommand(const std::string& id)
    {
        return id == "doc.save" || id == "doc.save_as" || id == "doc.undo" || id == "doc.redo" ||
               id == "edit.copy" || id == "edit.paste";
    }
}

// ==== Documents ====

void EditorLayer::RegisterDocuments()
{
    using Editor::Document;
    auto& docs = m_documents;

    // ---- Scene: the fallback, what every panel without its own document edits ----
    Document scene;
    scene.kind  = "scene";
    scene.title = [this] {
        return m_currentScenePath.empty() ? std::string("Scene") : "Scene '" + BaseName(m_currentScenePath) + "'";
    };
    // No canSave: Ctrl+S in Play reaches SaveSceneNow, which refuses with its own log line (the
    // File menu's Save Scene item greys out instead).
    scene.save      = [this] { SaveSceneNow(); };
    scene.saveAs    = [this] { SaveSceneAs(); };
    scene.canUndo   = [this] { return m_editHistory.CanUndo() && UndoEnabled(); };
    scene.canRedo   = [this] { return m_editHistory.CanRedo() && UndoEnabled(); };
    scene.undoLabel = [this] {
        if (!m_editHistory.CanUndo()) return std::string();
        return std::string(m_editHistory.UndoByAgent() ? "AI: " : "") + m_editHistory.UndoLabel();
    };
    scene.redoLabel = [this] {
        if (!m_editHistory.CanRedo()) return std::string();
        return std::string(m_editHistory.RedoByAgent() ? "AI: " : "") + m_editHistory.RedoLabel();
    };
    scene.undo = [this] { EditUndo(); };
    scene.redo = [this] { EditRedo(); };
    docs.SetFallback(std::move(scene));

    // ---- The panels that read their own keys (they handle the chords in Draw) ----
    // The Sequencer owns every key while focused (keyframes take Delete, its own Ctrl+S).
    {
        Document d;
        d.kind       = "sequence";
        d.title      = [this] { return "Sequence '" + BaseName(m_sequencerWindow.Path()) + "'"; };
        d.hasFocus   = [this] { return m_focusSequencer; };
        d.canSave    = [this](std::string& why) {
            if (!m_sequencerWindow.Path().empty()) return true;
            why = "the sequence has no file yet";
            return false;
        };
        d.save       = [this] { m_sequencerWindow.Save(); };
        d.undo       = [this] { m_sequencerWindow.Undo(); };
        d.redo       = [this] { m_sequencerWindow.Redo(); };
        d.handlesKey = [](const std::string&) { return true; };
        docs.Add(std::move(d));
    }
    // The notify tracks own Ctrl+Z / Y (their ring). On an entity's Timeline, Save is the scene's:
    // the tracks are its TimelineComponent. A clip's tracks are saved by the Timeline panel.
    {
        Document d;
        d.kind     = "notify";
        d.title    = [] { return std::string("Notify Tracks"); };
        d.hasFocus = [this] { return m_focusNotify; };
        d.canSave  = [this](std::string& why) {
            if (m_notifyTrackEditor.GetTarget() == &m_clipScratch)
            {
                why = "a clip's tracks are saved with the clip (Save in the Timeline panel)";
                return false;
            }
            if (GetPlayState() == ViewportPlayState::Stopped) return true;
            why = kPlayGuard;
            return false;
        };
        d.save       = [this] { SaveSceneNow(); };
        d.undo       = [this] { m_notifyTrackEditor.Undo(); };
        d.redo       = [this] { m_notifyTrackEditor.Redo(); };
        d.handlesKey = [](const std::string& id) { return id != "doc.save" && id != "doc.save_as"; };
        docs.Add(std::move(d));
    }
    // The animation graph and set documents read S / Z / Y / C / V in their Draw.
    {
        Document d;
        d.kind       = "animgraph";
        d.title      = [] { return std::string("Animation Graph"); };
        d.hasFocus   = [this] { return m_focusAnimGraph; };
        d.save       = [] { ControlVerb(&Editor::AnimGraphEditor::Control, "save"); };
        d.undo       = [] { ControlVerb(&Editor::AnimGraphEditor::Control, "undo"); };
        d.redo       = [] { ControlVerb(&Editor::AnimGraphEditor::Control, "redo"); };
        d.handlesKey = [](const std::string& id) { return IsDocumentKeyCommand(id); };
        docs.Add(std::move(d));
    }
    {
        Document d;
        d.kind       = "animset";
        d.title      = [] { return std::string("Animation Set"); };
        d.hasFocus   = [this] { return m_focusAnimSet; };
        d.save       = [] { ControlVerb(&Editor::AnimSetEditor::Control, "save"); };
        d.undo       = [] { ControlVerb(&Editor::AnimSetEditor::Control, "undo"); };
        d.redo       = [] { ControlVerb(&Editor::AnimSetEditor::Control, "redo"); };
        d.handlesKey = [](const std::string& id) { return IsDocumentKeyCommand(id); };
        docs.Add(std::move(d));
    }

    // ---- Asset documents the dispatcher drives ----
    // The UI Editor's asset tab (a scene tab claims nothing): its file, its ring, its duplicate. No
    // clipboard for a stage (Copy refuses preview stages), and not the scene's either.
    {
        Document d;
        d.kind      = "ui";
        d.title     = [] { return std::string("UI Asset"); };
        d.hasFocus  = [] { return Editor::UIEditorWindow::Get().OwnsShortcuts(); };
        d.save      = [] { Editor::UIEditorWindow::Get().RequestSave(false); };
        d.saveAs    = [] { Editor::UIEditorWindow::Get().RequestSave(true); };
        d.undo      = [] { Editor::UIEditorWindow::Get().RequestUndo(false); };
        d.redo      = [] { Editor::UIEditorWindow::Get().RequestUndo(true); };
        d.duplicate = [] { Editor::UIEditorWindow::Get().RequestDuplicate(); };
        d.copy      = [] {};
        d.paste     = [] {};
        docs.Add(std::move(d));
    }
    // The Material Editor's file tab (an entity tab's edits are scene history steps).
    {
        Document d;
        d.kind     = "material";
        d.title    = [] {
            Editor::MaterialEditSession* s = Editor::MaterialEditorWindow::Get().ActiveSession();
            return s ? "Material '" + BaseName(s->Path()) + "'" : std::string("Material");
        };
        d.hasFocus = [] {
            auto& mw = Editor::MaterialEditorWindow::Get();
            return mw.IsOpen() && mw.OwnsUndoShortcut();
        };
        d.save = [] { Editor::MaterialEditorWindow::Get().QueueSave(std::string(), false, 0); };
        d.undo = [] { Editor::MaterialEditorWindow::Get().RequestUndo(); };
        d.redo = [] { Editor::MaterialEditorWindow::Get().RequestRedo(); };
        docs.Add(std::move(d));
    }
    // The Mesh Editor's staged slot edits: a model file's, never scene state.
    {
        Document d;
        d.kind     = "mesh";
        d.title    = [] { return "Model '" + BaseName(Editor::MeshEditorWindow::Get().DocumentPath()) + "'"; };
        d.hasFocus = [] {
            auto& me = Editor::MeshEditorWindow::Get();
            return me.IsOpen() && me.OwnsUndoShortcut();
        };
        d.save = [] { Editor::MeshEditorWindow::Get().RequestSave(); };
        d.undo = [] { Editor::MeshEditorWindow::Get().RequestUndo(); };
        d.redo = [] { Editor::MeshEditorWindow::Get().RequestRedo(); };
        docs.Add(std::move(d));
    }
    // The Skeleton Editor's staged sockets: its own history and clipboard (SO records).
    {
        Document d;
        d.kind     = "skeleton";
        d.title    = [] { return "Skeleton '" + BaseName(Editor::SkeletonEditorWindow::Get().DocumentPath()) + "'"; };
        d.hasFocus = [] {
            auto& se = Editor::SkeletonEditorWindow::Get();
            return se.IsOpen() && se.OwnsUndoShortcut();
        };
        d.save  = [] { Editor::SkeletonEditorWindow::Get().RequestSave(); };
        d.undo  = [] { Editor::SkeletonEditorWindow::Get().RequestUndo(); };
        d.redo  = [] { Editor::SkeletonEditorWindow::Get().RequestRedo(); };
        d.copy  = [] { Editor::SkeletonEditorWindow::Get().RequestCopy(); };
        d.paste = [] { Editor::SkeletonEditorWindow::Get().RequestPaste(); };
        docs.Add(std::move(d));
    }
    // The Chain Physics Preview's staged values: its history; Apply (to a destination the user
    // picks) is the commit, so Save has nothing to write.
    {
        Document d;
        d.kind     = "chainpreview";
        d.title    = [] { return std::string("Chain Physics Preview"); };
        d.hasFocus = [this] { return Editor::ChainPhysicsPreviewWindow::Get().OwnsUndoShortcut() && !m_chainColliderUndo; };
        d.canSave  = [](std::string& why) {
            why = "staged chain values are committed by Apply in the panel";
            return false;
        };
        d.save = [] {};
        d.undo = [] { Editor::ChainPhysicsPreviewWindow::Get().RequestUndo(); };
        d.redo = [] { Editor::ChainPhysicsPreviewWindow::Get().RequestRedo(); };
        docs.Add(std::move(d));
    }
}

// ==== Commands ====

void EditorLayer::RegisterEditorCommands()
{
    auto& reg = Editor::CommandRegistry::Get();
    const auto editMode = [this](std::string& why) {
        if (GetPlayState() == ViewportPlayState::Stopped) return true;
        why = kPlayGuard;
        return false;
    };
    const auto hasSelection = [this] {
        return m_world && m_selectedEntity != NullEntity && m_world->IsAlive(m_selectedEntity);
    };
    auto add = [&reg](Command c) { reg.Register(std::move(c)); };

    // ---- File ----
    add({ .id = "file.new_scene", .category = "File", .label = "New Scene", .icon = FontAwesome::kFile,
          .defaultChord = ParseChord("Ctrl+N"), .run = [this] { RequestNewScene(); } });
    add({ .id = "file.open_scene", .category = "File", .label = "Open Scene...", .icon = FontAwesome::kFolderOpen,
          .defaultChord = ParseChord("Ctrl+O"), .run = [this] { OpenSceneWithDialog(); } });
    // The active document's Save: the scene, or the asset tab that had focus.
    add({ .id = "doc.save", .category = "File", .label = "Save", .keywords = "write document",
          .icon = FontAwesome::kFloppy, .defaultChord = ParseChord("Ctrl+S"), .scope = CommandScope::Document,
          .run = [this] { if (m_documents.Active().save) m_documents.Active().save(); },
          .enabled = [this](std::string& why) {
              Editor::Document& d = m_documents.Active();
              if (!d.save) { why = "this document has nothing to save"; return false; }
              return !d.canSave || d.canSave(why);
          },
          .dynamicLabel = [this] { return m_documents.SaveLabel(); } });
    add({ .id = "doc.save_as", .category = "File", .label = "Save As...", .defaultChord = ParseChord("Ctrl+Shift+S"),
          .scope = CommandScope::Document,
          .run = [this] { if (m_documents.Active().saveAs) m_documents.Active().saveAs(); },
          .enabled = [this](std::string& why) {
              Editor::Document& d = m_documents.Active();
              if (!d.saveAs) { why = "this document has no Save As"; return false; }
              return !d.canSave || d.canSave(why);
          },
          .dynamicLabel = [this] { return m_documents.SaveLabel() + " As..."; } });
    add({ .id = "scene.save", .category = "File", .label = "Save Scene", .icon = FontAwesome::kFloppy,
          .run = [this] { SaveSceneNow(); },
          .enabled = editMode,
          .tooltip = [this] {
              return m_currentScenePath.empty() ? std::string("Save the scene (prompts for a path - nothing is open yet)")
                                                : std::string("Save over the currently open .iscene");
          } });
    add({ .id = "scene.save_as", .category = "File", .label = "Save Scene As...", .run = [this] { SaveSceneAs(); },
          .enabled = editMode,
          .tooltip = [] { return std::string("Save to a new .iscene and make it the open scene"); } });
    // The scene (when dirty) and every window's unsaved documents, written in PPA.
    add({ .id = "file.save_all", .category = "File", .label = "Save All", .keywords = "documents",
          .defaultChord = ParseChord("Ctrl+Alt+S"),
          .run = [this] {
              if (m_sceneDirty) SaveSceneNow();
              m_pendingSaveDocuments = true;
          },
          .enabled = editMode });
    add({ .id = "file.exit", .category = "File", .label = "Exit", .icon = FontAwesome::kDoorOpen,
          .defaultChord = ParseChord("Alt+F4"), .displayOnly = true, .run = [this] { RequestAppExit(); } });

    // ---- Edit ----
    add({ .id = "doc.undo", .category = "Edit", .label = "Undo", .icon = FontAwesome::kUndo,
          .defaultChord = ParseChord("Ctrl+Z"), .scope = CommandScope::Document,
          .run = [this] { if (m_documents.Active().undo) m_documents.Active().undo(); },
          .enabled = [this](std::string&) {
              Editor::Document& d = m_documents.Active();
              return d.undo && (!d.canUndo || d.canUndo());
          },
          .dynamicLabel = [this] { return m_documents.UndoLabel(); } });
    add({ .id = "doc.redo", .category = "Edit", .label = "Redo", .icon = FontAwesome::kRedo,
          .defaultChord = ParseChord("Ctrl+Y"), .altChord = ParseChord("Ctrl+Shift+Z"), .scope = CommandScope::Document,
          .run = [this] { if (m_documents.Active().redo) m_documents.Active().redo(); },
          .enabled = [this](std::string&) {
              Editor::Document& d = m_documents.Active();
              return d.redo && (!d.canRedo || d.canRedo());
          },
          .dynamicLabel = [this] { return m_documents.RedoLabel(); } });
    // Copy / paste / duplicate record undo and so are Edit-mode only, unless the active document
    // has its own.
    add({ .id = "edit.copy", .category = "Edit", .label = "Copy", .icon = FontAwesome::kCopy,
          .defaultChord = ParseChord("Ctrl+C"), .scope = CommandScope::Document,
          .run = [this] {
              if (m_documents.Active().copy) m_documents.Active().copy();
              else CopySelected();
          },
          .enabled = [this, hasSelection](std::string&) {
              return m_documents.Active().copy || (hasSelection() && UndoEnabled());
          } });
    add({ .id = "edit.paste", .category = "Edit", .label = "Paste", .icon = FontAwesome::kPaste,
          .defaultChord = ParseChord("Ctrl+V"), .scope = CommandScope::Document,
          .run = [this] {
              if (m_documents.Active().paste) m_documents.Active().paste();
              else PasteClipboard();
          },
          .enabled = [this](std::string&) { return m_documents.Active().paste || UndoEnabled(); } });
    add({ .id = "edit.duplicate", .category = "Edit", .label = "Duplicate", .defaultChord = ParseChord("Ctrl+D"),
          .scope = CommandScope::Document,
          .run = [this] {
              if (m_documents.Active().duplicate) { m_documents.Active().duplicate(); return; }
              if (m_selectedEntities.size() > 1) DuplicateSelection();
              else                               DuplicateEntity(m_selectedEntity);
          },
          .enabled = [this, hasSelection](std::string&) {
              return m_documents.Active().duplicate || (hasSelection() && UndoEnabled());
          } });
    // Unmodified selection keys: only while the Hierarchy or the Viewport had keyboard focus last
    // frame and no widget is active (the Sequencer, the notify tracks and the curve editor bind
    // Delete to their own content; the Hierarchy's inline rename field owns Esc / F2).
    const auto selectionKeys = [this] { return SelectionPanelFocused() && !ImGui::IsAnyItemActive(); };
    add({ .id = "edit.delete", .category = "Edit", .label = "Delete", .icon = FontAwesome::kTrash,
          .defaultChord = ParseChord("Del"), .scope = CommandScope::Panel, .panel = "selection",
          .run = [this] { ReconcileSelection(); DeleteEntities(SelectionRoots()); },
          .enabled = [hasSelection](std::string&) { return hasSelection(); }, .chordLive = selectionKeys });
    add({ .id = "edit.rename", .category = "Edit", .label = "Rename", .icon = FontAwesome::kPen,
          .defaultChord = ParseChord("F2"), .scope = CommandScope::Panel, .panel = "selection",
          .run = [this] { m_hierarchyRenameRequest = m_selectedEntity; },
          .enabled = [hasSelection](std::string&) { return hasSelection(); }, .chordLive = selectionKeys });
    add({ .id = "edit.select_all", .category = "Edit", .label = "Select All", .defaultChord = ParseChord("Ctrl+A"),
          .displayOnly = true,   // the Hierarchy reads Ctrl+A itself while focused
          .run = [this] {
              // The rows the Hierarchy last drew (filter-aware), like its own Ctrl+A.
              m_selectedEntities       = m_hierarchyRowOrder;
              m_selectedEntity         = m_selectedEntities.empty() ? NullEntity : m_selectedEntities.back();
              m_selectionRangeAnchor   = m_selectedEntity;
              m_hierarchyHighlight     = m_selectedEntity;
              m_lastSeenSelectedEntity = m_selectedEntity;
          },
          .enabled = [this](std::string&) { return !m_hierarchyRowOrder.empty(); } });
    add({ .id = "edit.deselect", .category = "Edit", .label = "Deselect All", .defaultChord = ParseChord("Esc"),
          .scope = CommandScope::Panel, .panel = "selection", .run = [this] { ClearEntitySelection(); },
          .enabled = [hasSelection](std::string&) { return hasSelection(); }, .chordLive = selectionKeys });
    // The viewport reads F and End itself while hovered.
    add({ .id = "edit.focus", .category = "Edit", .label = "Focus Selected", .icon = FontAwesome::kCrosshairs,
          .defaultChord = ParseChord("F"), .displayOnly = true, .run = [this] { FocusSelectedEntity(); },
          .enabled = [hasSelection](std::string&) { return hasSelection(); } });
    add({ .id = "edit.snap_floor", .category = "Edit", .label = "Snap to Floor", .icon = FontAwesome::kArrowDown,
          .defaultChord = ParseChord("End"), .displayOnly = true, .run = [this] { SnapSelectedToFloor(); },
          .enabled = [hasSelection](std::string&) { return hasSelection(); } });
    add({ .id = "edit.project_settings", .category = "Edit", .label = "Project Settings...",
          .icon = FontAwesome::kSliders,
          .run = [this] {
              m_showGraphicsSettings = true;
              ImGui::SetWindowFocus("Project Settings###Graphics Settings");
          },
          .tooltip = [] { return std::string("settings.json / engine.json - shared by everyone on the project"); } });
    add({ .id = "edit.preferences", .category = "Edit", .label = "Editor Preferences...", .icon = FontAwesome::kPalette,
          .run = [this] {
              m_showEditorPreferences = true;
              ImGui::SetWindowFocus("Editor Preferences");
          },
          .tooltip = [] {
              return std::string("editor_prefs.ini - your own editor behaviour (font size, snapping,\n"
                                 "grid, panels, autosave, keyboard shortcuts)");
          } });

    // ---- Play ----
    add({ .id = "play.toggle", .category = "Play", .label = "Play / Pause", .defaultChord = ParseChord("Ctrl+P"),
          .run = [this] { TogglePlayPause(); },
          .dynamicLabel = [this] {
              return std::string(GetPlayState() == ViewportPlayState::Playing ? "Pause" : "Play");
          } });

    // ---- Viewport ----
    add({ .id = "view.grid", .category = "Viewport", .label = "Grid", .run = [this] { m_showEditorGrid = !m_showEditorGrid; },
          .checked = [this] { return m_showEditorGrid; } });
    add({ .id = "view.snapping", .category = "Viewport", .label = "Snapping",
          .run = [this] { m_snapEnabled = !m_snapEnabled; }, .checked = [this] { return m_snapEnabled; } });
    add({ .id = "view.view_cube", .category = "Viewport", .label = "View Cube",
          .run = [this] { m_showViewCube = !m_showViewCube; }, .checked = [this] { return m_showViewCube; } });
    // The viewport reads W / E / R itself (no widget active, not typing).
    add({ .id = "gizmo.move", .category = "Viewport", .label = "Gizmo: Move", .defaultChord = ParseChord("W"),
          .displayOnly = true, .run = [this] { m_gizmoOperation = ImGuizmo::TRANSLATE; } });
    add({ .id = "gizmo.rotate", .category = "Viewport", .label = "Gizmo: Rotate", .defaultChord = ParseChord("E"),
          .displayOnly = true, .run = [this] { m_gizmoOperation = ImGuizmo::ROTATE; } });
    add({ .id = "gizmo.scale", .category = "Viewport", .label = "Gizmo: Scale", .defaultChord = ParseChord("R"),
          .displayOnly = true, .run = [this] { m_gizmoOperation = ImGuizmo::SCALE; } });
    add({ .id = "gizmo.space", .category = "Viewport", .label = "Gizmo Space: Local",
          .run = [this] { m_gizmoMode = (m_gizmoMode == ImGuizmo::LOCAL) ? ImGuizmo::WORLD : ImGuizmo::LOCAL; },
          .checked = [this] { return m_gizmoMode == ImGuizmo::LOCAL; } });

    // ---- Console / Tools / Build / Layout / Help ----
    add({ .id = "console.focus", .category = "Console", .label = "Focus command line",
          .run = [this] {
              m_showOutputLog = true;
              m_outputLog.FocusCommandLine();
          } });
    add({ .id = "tools.palette", .category = "Tools", .label = "Command Palette", .icon = FontAwesome::kBolt,
          .defaultChord = ParseChord("Ctrl+Shift+P"), .run = [this] { m_commandPaletteRequest = true; },
          .tooltip = [] { return std::string("Search every panel and editor command by name"); } });
    if constexpr (Harness::ControlChannel::kCompiled)
        add({ .id = "tools.control_channel", .category = "Tools", .label = "AI Control Channel",
              .run = [this] { ToggleControlChannel(); },
              .checked = [] { return Harness::ControlChannel::Get().Enabled(); },
              .tooltip = [] {
                  return std::string("Open a local pipe so an AI agent can read and edit this editor live\n"
                                     "(python tools/engine_ctl.py list; SDK Engine.attach(prefer=\"interactive\")).\n"
                                     "Same Windows user only, and NOT read-only: every request is logged\n"
                                     "in the Output Log (source control). Click again to close.");
              } });
    add({ .id = "tools.bake_collision", .category = "Tools", .label = "Bake Collision Meshes...",
          .run = [this] { m_bakeCollisionOpenRequest = true; } });
    add({ .id = "tools.bake_probes", .category = "Tools", .label = "Bake All Reflection Probes",
          .run = [this] { if (m_renderer) m_renderer->BakeAllProbes(); },
          .enabled = [this](std::string& why) {
              if (m_renderer) return true;
              why = "no renderer";
              return false;
          } });
    add({ .id = "tools.retarget", .category = "Tools", .label = "Animation Retarget",
          .run = [this] { m_showRetargetWindow = true; } });
    add({ .id = "tools.component_transfer", .category = "Tools", .label = "Component Transfer",
          .run = [this] { m_showComponentTransfer = true; } });
    add({ .id = "tools.references", .category = "Tools", .label = "Asset References",
          .run = [this] {
              m_showReferencesWindow = true;
              if (!m_refScanner.Built()) m_refScanner.Rebuild(m_assetDir);
          } });
    add({ .id = "build.reset_startup", .category = "Build", .label = "Reset Startup Scene (Base Scene)",
          .run = [] { WriteStartupSceneToManifest(Scene::kBaseScenePath); } });
    // Rebuilt by SetupDockSpace right after the menu bar returns: the DockBuilder block there runs
    // before any panel's Begin this frame.
    add({ .id = "layout.reset", .category = "Layout", .label = "Reset Layout", .icon = FontAwesome::kLayerGroup,
          .run = [this] {
              m_resetLayoutRequested = true;
              m_showHierarchy = m_showInspector = m_showContentBrowser = m_showOutputLog = true;
          },
          .tooltip = [] {
              return std::string("Put Hierarchy / Viewport / Inspector / Content Browser /\n"
                                 "Output Log back in the default arrangement. Tool windows\n"
                                 "that were docked into the main area become floating.");
          } });
    add({ .id = "layout.status_bar", .category = "Layout", .label = "Status Bar",
          .run = [this] { m_showStatusBar = !m_showStatusBar; }, .checked = [this] { return m_showStatusBar; } });
    add({ .id = "help.shortcuts", .category = "Help", .label = "Keyboard Shortcuts", .icon = FontAwesome::kKeyboard,
          .defaultChord = ParseChord("F1"), .run = [this] { m_showKeyboardShortcuts = !m_showKeyboardShortcuts; } });
    add({ .id = "help.about", .category = "Help", .label = "About", .icon = FontAwesome::kInfo,
          .run = [this] { m_showAbout = true; } });
}

void EditorLayer::EnsureCommandsRegistered()
{
    if (m_commandsRegistered) return;
    m_commandsRegistered = true;
    RegisterDocuments();
    RegisterEditorCommands();
    ApplyKeymapPrefs();
}

// ==== Shortcuts ====

void EditorLayer::HandleUndoShortcuts()
{
    EnsureCommandsRegistered();

    // Panels that read their own keys report focus through one-shot flags, consumed every frame
    // and before any other early-out so a panel that stopped rendering cannot leave a stale claim.
    m_focusSequencer = m_sequencerWindow.ConsumeFocusedLastDraw();
    m_focusNotify    = m_notifyTrackEditor.ConsumeFocusedLastRender();
    m_focusAnimGraph = Editor::AnimGraphEditor::ConsumeFocusedLastDraw();
    m_focusAnimSet   = Editor::AnimSetEditor::ConsumeFocusedLastDraw();

    // This runs at the TOP of OnUIRender, outside any Begin/End pair: "a popup is open" is read off
    // ImGui's popup stack, not a window focus test. While one is (a menu, the palette, a modal) the
    // active document stays the one before it opened.
    const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    m_documents.UpdateActive(popupOpen);

    // A rect drag brackets one undo entry across press..release, and Ctrl is its own quantise
    // modifier - so Ctrl+Z mid-drag is easy to hit by accident and would pop the history underneath
    // a gesture that has not been recorded yet.
    if (m_uiRectDrag.active || Editor::UIEditorWindow::Get().DragActive()) return;
    // A text field keeps its own keys (ImGui's undo, Ctrl+A, Delete). -uitest_reverse=commands.text_field_keeps_keys
    // drops this.
    if (ImGui::GetIO().WantTextInput && !Editor::UITests::Reverted(Editor::UITests::Fix::TextFieldKeepsKeys)) return;
    // The rebind panel owns the keyboard while it is capturing - and for the remainder of the frame
    // in which the capture resolved, because that key edge is still visible here. The same holds
    // for this editor's own rebind in Help > Keyboard Shortcuts.
    if (Editor::InputBindingsWindow::Get().WantsExclusiveKeyboard() || !m_rebindCommandId.empty()) return;
    // A popup owns the keyboard: without this Ctrl+S fires while the unsaved-changes modal is up,
    // re-entering the very save the modal is waiting on. -uitest_reverse=commands.popup_owns_keys drops this.
    if (popupOpen && !Editor::UITests::Reverted(Editor::UITests::Fix::PopupOwnsKeys)) return;
    // The Game View has the game's keyboard (Play, focused or captured): the editor keeps its Ctrl / Alt chords
    // (Ctrl+P stops Play), every other key is the game's.
    if (Editor::GameView::Get().GameHasKeyboard() && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyAlt) return;

    auto& reg = Editor::CommandRegistry::Get();
    const Editor::Document& doc = m_documents.Active();
    for (const Command& c : reg.All())
    {
        if (c.displayOnly) continue;
        if (!Editor::ChordPressed(reg.ChordOf(c)) && !Editor::ChordPressed(c.altChord)) continue;
        // The focused panel reads this chord in its own Draw.
        if (doc.handlesKey && doc.handlesKey(c.id)) return;
        if (c.chordLive && !c.chordLive()) continue;
        std::string why;
        if (!reg.Execute(c.id, &why) && !why.empty())
            LOG_INFO("Editor: %s - %s", reg.LabelOf(c).c_str(), why.c_str());
        return;
    }
}

// ==== Menus ====

bool EditorLayer::CommandMenuItem(const char* id, const char* overrideLabel, const char* chordOf)
{
    auto& reg = Editor::CommandRegistry::Get();
    const Command* c = reg.Find(id);
    if (!c) return false;
    std::string why;
    const bool enabled = reg.IsEnabled(*c, &why);
    const std::string chord = Editor::FormatChord(chordOf ? reg.ChordOf(chordOf) : reg.ChordOf(*c));
    // "###<id>" keeps the item's ImGui id stable while a dynamic label changes.
    const std::string label = std::string(overrideLabel ? overrideLabel : reg.LabelOf(*c).c_str()) + "###" + c->id;
    const bool checked = c->checked && c->checked();
    const bool clicked = ImGui::MenuItemEx(label.c_str(), c->icon ? EditorTheme::Icon(c->icon) : nullptr,
                                           chord.empty() ? nullptr : chord.c_str(), checked, enabled);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (!enabled && !why.empty()) ImGui::SetTooltip("%s", why.c_str());
        else if (c->tooltip)
        {
            const std::string tip = c->tooltip();
            if (!tip.empty()) ImGui::SetTooltip("%s", tip.c_str());
        }
    }
    if (clicked) reg.Execute(id);
    return clicked;
}

// ==== Keymap preferences ====
// editor_prefs.ini keeps a user's chord overrides as "keymap.<id>=<chord>" ("" = unbound).

void EditorLayer::ApplyKeymapPrefs()
{
    auto& reg = Editor::CommandRegistry::Get();
    reg.ClearUserChords();
    for (const auto& [id, text] : m_keymapPrefs)
    {
        if (!reg.Find(id)) continue;   // a command this build does not have: kept in the file, ignored
        const Editor::Chord chord = ParseChord(text);
        if (!text.empty() && chord.Empty())
        {
            LOG_WARNING("Editor: editor_prefs.ini keymap.%s='%s' is not a chord - the default stays", id.c_str(),
                        text.c_str());
            continue;
        }
        reg.SetUserChord(id, chord);
    }
}

void EditorLayer::StoreKeymapPrefs()
{
    auto& reg = Editor::CommandRegistry::Get();
    // Keep the overrides of commands this build does not have (another branch's, a module's).
    std::map<std::string, std::string> kept;
    for (const auto& [id, text] : m_keymapPrefs)
        if (!reg.Find(id)) kept[id] = text;
    for (const auto& [id, chord] : reg.UserChords()) kept[id] = Editor::FormatChord(chord);
    m_keymapPrefs = std::move(kept);
    SaveEditorPrefs();
}

// ==== Help > Keyboard Shortcuts (F1) ====

void EditorLayer::RenderKeyboardShortcutsWindow()
{
    EnsureCommandsRegistered();
    ImGui::SetNextWindowSize(ImVec2(680.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard Shortcuts", &m_showKeyboardShortcuts))
    {
        ImGui::End();
        m_rebindCommandId.clear();
        return;
    }
    auto& reg = Editor::CommandRegistry::Get();

    // A rebind in progress takes the next chord; Esc cancels, Backspace unbinds.
    if (!m_rebindCommandId.empty())
    {
        const ImGuiIO& io = ImGui::GetIO();
        Editor::Chord picked;
        bool  done = false;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) done = true;
        else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) { done = true; reg.SetUserChord(m_rebindCommandId, Editor::Chord{}); }
        else
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && !done; ++k)
            {
                const ImGuiKey key = static_cast<ImGuiKey>(k);
                if (key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift ||
                    key == ImGuiKey_RightShift || key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt ||
                    key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper || key >= ImGuiKey_GamepadStart)
                    continue;
                if (!ImGui::IsKeyPressed(key, false)) continue;
                picked = Editor::Chord{ key, io.KeyCtrl, io.KeyShift, io.KeyAlt };
                if (!Editor::FormatChord(picked).empty())
                {
                    reg.SetUserChord(m_rebindCommandId, picked);
                    done = true;
                }
            }
        if (done)
        {
            m_rebindCommandId.clear();
            StoreKeymapPrefs();
        }
    }

    const auto conflicts = reg.Conflicts();
    if (!conflicts.empty())
    {
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%zu chord conflict(s): the first command listed wins.",
                           conflicts.size());
        for (const auto& [a, b] : conflicts)
            ImGui::BulletText("%s  =  %s  /  %s", Editor::FormatChord(reg.ChordOf(a)).c_str(), a.c_str(), b.c_str());
        ImGui::Separator();
    }
    ImGui::TextDisabled("Click a chord to change it (Esc cancels, Backspace unbinds); right-click resets it.");

    // Commands, grouped by category in registration order.
    std::vector<std::string> categories;
    for (const Command& c : reg.All())
        if (std::find(categories.begin(), categories.end(), c.category) == categories.end())
            categories.push_back(c.category);
    for (const std::string& cat : categories)
    {
        EditorTheme::Heading(cat.c_str());
        if (!ImGui::BeginTable(cat.c_str(), 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) continue;
        ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        for (const Command& c : reg.All())
        {
            if (c.category != cat) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(c.id.c_str());
            std::string keys = m_rebindCommandId == c.id ? std::string("press a chord...")
                                                          : Editor::FormatChord(reg.ChordOf(c));
            if (!c.altChord.Empty()) keys += (keys.empty() ? "" : " / ") + Editor::FormatChord(c.altChord);
            if (keys.empty()) keys = "-";
            EditorTheme::PushBold();
            if (ImGui::Selectable(keys.c_str(), m_rebindCommandId == c.id) && !c.displayOnly)
                m_rebindCommandId = c.id;
            EditorTheme::PopBold();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && reg.HasUserChord(c.id))
            {
                reg.ResetChord(c.id);
                StoreKeymapPrefs();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(c.displayOnly ? "Read by the panel it belongs to: not rebindable here."
                                  : reg.HasUserChord(c.id) ? "Your chord (right-click: back to the default)."
                                                           : "The default chord.");
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(reg.LabelOf(c).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", c.id.c_str());
        }
        ImGui::EndTable();
    }

    // Mouse gestures and panel keys that are not commands.
    struct Row { const char* keys; const char* what; };
    auto section = [](const char* title, std::initializer_list<Row> rows) {
        EditorTheme::Heading(title);
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, 190.0f);
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch);
            for (const Row& r : rows)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                EditorTheme::PushBold();
                ImGui::TextUnformatted(r.keys);
                EditorTheme::PopBold();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.what);
            }
            ImGui::EndTable();
        }
    };
    section("Mouse and panel keys", {
        { "F11",                   "Fullscreen viewport (game view) on / off" },
        { "Click",                 "Select" },
        { "Ctrl+Click",            "Add / remove from the selection" },
        { "Shift+Click",           "Hierarchy: select range  /  Viewport: add" },
        { "Ctrl (while dragging)", "Invert snapping for this drag" },
        { "Right mouse drag",      "Look around (perspective) / pan (ortho views)" },
        { "RMB + W A S D",         "Fly;  Q / E down / up;  Shift = 3x speed" },
        { "Mouse wheel",           "Zoom (ortho views)" },
        { "View cube",             "Click a face: that ortho view;  drag: turn the camera" },
        { "Ctrl+Shift+1..4",       "Store camera bookmark" },
        { "Ctrl+1..4",             "Recall camera bookmark" },
    });
    ImGui::End();
}
