// EditorLayer_GameView.cpp - the editor's side of the Game View (Editor/GameView.h): the panel's inputs from
// the editor's state, its docking beside the Scene viewport, the main pipeline's route handed to App and the
// renderer, the Scene viewport's picture in Play (its own secondary view), and Lua's Editor.GameView /
// SetGameView.

#include "Editor/EditorLayer.h"

#include "ECS/Components.h"                 // EditorCameraTag
#include "Editor/EditorCameraController.h"  // Editor::FlyInput
#include "Editor/EditorLayerShared.h"       // kWindowViewport
#include "Editor/EditorPanels.h"
#include "Editor/FocusTracker.h"
#include "Editor/GameView.h"
#include "Platform/KeyboardState.h"
#include "Platform/Window.h"            // the window's pointer (the Canvas's source)
#include "Renderer/Renderer.h"
#include "RenderGraph/SceneView.h"      // the Scene's ShowFlags
#include "Scripting/LuaDoc.h"
#include "ECS/HierarchyComponents.h"   // GlobalTransform
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // DockBuilder*

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaId::Out (needs sol first)

#include <string>

namespace
{
    // The main view's owners (Renderer::SetMainViewOwner): whose camera the main pipeline renders.
    constexpr std::uint32_t kOwnerEditor = 1;
    constexpr std::uint32_t kOwnerGame   = 2;
}

void EditorLayer::DrawGameViewPanel(int panel)
{
    Editor::GameView::DrawInputs in;
    const ViewportPlayState play = GetPlayState();
    in.renderer     = m_renderer;
    in.world        = m_world;
    in.sceneTexture = m_sceneTextureId;
    in.simulating   = play != ViewportPlayState::Stopped;
    in.paused       = play == ViewportPlayState::Paused;
    if (m_window)
    {
        const auto p = m_window->Pointer().PhysicalPos();
        in.pointerValid = true;
        in.pointerX     = static_cast<float>(p.first);
        in.pointerY     = static_cast<float>(p.second);
    }
    Editor::GameView::Get().Draw(in, panel);
}

void EditorLayer::ApplyGameViewDock()
{
    Editor::GameView& gv = Editor::GameView::Get();
    for (int i = 0; i < Editor::GameView::kMaxPanels; ++i)
    {
        const Editor::GameView::Dock dock = gv.ConsumeDockRequest(i);
        if (dock == Editor::GameView::Dock::None) continue;
        const char* windowId = Editor::GameView::kWindowIds[i];
        // The first panel's lines as they always were; the others name themselves.
        const std::string which = i == 0 ? std::string() : std::string(" (") + windowId + ")";
        ImGuiWindow* scene = ImGui::FindWindowByName(kWindowViewport);
        if (!scene || scene->DockId == 0 || !ImGui::DockBuilderGetNode(scene->DockId))
        {
            LOG_INFO("Editor: the Scene viewport is not docked - %s opens where ImGui puts it", windowId);
            continue;
        }
        const bool beside = dock == Editor::GameView::Dock::BesideScene;
        const char* where = beside ? "beside the Scene viewport" : "as a tab of the Scene viewport";
        ImGuiWindow* game = ImGui::FindWindowByName(windowId);
        if (game && (!game->WasActive || !scene->DockNode || !scene->WasActive))
        {
            // A window that was not drawn last frame (just reopened) is not in a dock node ImGui can move it out
            // of yet: the request waits a frame rather than dock a window ImGui has not placed.
            gv.RequestDock(dock, i);
            continue;
        }
        if (game)
        {
            ImGuiDockNode* own   = game->DockNode;
            ImGuiDockNode* scn   = scene->DockNode;
            const bool     there = beside ? (own && own != scn && own->ParentNode
                                             && own->ParentNode == scn->ParentNode
                                             && own->ParentNode->SplitAxis == ImGuiAxis_X)
                                          : own == scn;
            if (there) continue;
            if (own)
            {
                // A live window moves the way a dragged tab does: out of its node first (a drag floats it), then
                // into the target at the next frame. Docking a still-docked window collapses the node it leaves
                // and can free the very node it was sent to.
                ImGui::DockContextQueueUndockWindow(ImGui::GetCurrentContext(), game);
                gv.RequestDock(dock, i);
                continue;
            }
            ImGui::DockContextQueueDock(ImGui::GetCurrentContext(), scene, scn, game,
                                        beside ? ImGuiDir_Right : ImGuiDir_None, 0.5f, false);
            LOG_INFO("[GAMEVIEW] docked %s%s", where, which.c_str());
            continue;
        }
        // Not created yet (its first open): its settings say where it starts.
        ImGuiID target = scene->DockId;
        if (beside)
        {
            // The Scene viewport's node splits in two: the Scene keeps the left half, the Game View takes the
            // right. DockBuilderSplitNode moves the node's windows into the side it returns second.
            ImGuiID left = 0, right = 0;
            ImGui::DockBuilderSplitNode(scene->DockId, ImGuiDir_Right, 0.5f, &right, &left);
            target = right;
        }
        ImGui::DockBuilderDockWindow(windowId, target);
        if (ImGuiDockNode* node = ImGui::DockBuilderGetNode(target))
            ImGui::DockBuilderFinish(ImGui::DockNodeGetRootNode(node)->ID);
        LOG_INFO("[GAMEVIEW] docked %s%s", where, which.c_str());
    }
}

// ==== The route, for App ====

void EditorLayer::UpdateMainViewRoute(bool simulating, bool fullscreen, unsigned int& viewportW,
                                      unsigned int& viewportH)
{
    Editor::GameView& gv = Editor::GameView::Get();
    const Editor::MainRoute route = gv.UpdateRoute(simulating, fullscreen);
    if (route == Editor::MainRoute::Game)
    {
        viewportW = gv.Extent().w;
        viewportH = gv.Extent().h;
    }
    // The editor camera's view and the game's never share temporal history: a change of owner cuts it.
    if (m_renderer)
    {
        if (route == Editor::MainRoute::SceneEditor)
            m_renderer->SetMainViewOwner(kOwnerEditor, "editor");
        else
            m_renderer->SetMainViewOwner(kOwnerGame, "game");
        // The Game View's picture is the player's: no grid, debug wires, icons or selection outline on it.
        m_renderer->SetMainViewEditorOverlays(route != Editor::MainRoute::Game);
    }
}

bool EditorLayer::MainViewObservesPlay() const
{
    const Editor::GameView& gv = Editor::GameView::Get();
    return gv.Simulating() && gv.Route() == Editor::MainRoute::SceneEditor;
}

void EditorLayer::GetSceneFlyInput(Editor::FlyInput& out) const
{
    // Only while the Scene shows the editor camera in Play: a drag latched before the route moved is dropped.
    out = Editor::GameView::Get().SceneObservesPlay() ? m_sceneFly : Editor::FlyInput{};
}

void EditorLayer::GetGameMouseLook(bool& held, float& dx, float& dy) const
{
    const Editor::GameView& gv = Editor::GameView::Get();
    if (!gv.Simulating() || gv.Route() == Editor::MainRoute::SceneGame)
    {
        // Edit, and the single-view editor: the Scene viewport's right drag.
        held = m_viewportRightDragging;
        dx   = m_vpMouseDX;
        dy   = m_vpMouseDY;
        return;
    }
    if (gv.Route() == Editor::MainRoute::Game)
    {
        held = gv.PictureRightDragging();
        dx   = gv.PictureMouseDX();
        dy   = gv.PictureMouseDY();
        return;
    }
    // The Scene viewport shows the editor camera: its drag flies that camera, the game looks nowhere.
    held = false;
    dx = dy = 0.f;
}

void EditorLayer::MapGamePointer(float& minX, float& minY, float& scaleX, float& scaleY,
                                 bool& hostOwnsPointer) const
{
    const Editor::GameView& gv = Editor::GameView::Get();
    if (!gv.Simulating()) return;
    if (gv.Route() == Editor::MainRoute::Game)
    {
        // The Canvas is the Game View's picture: its top-left and its scale (a fixed render size is shown
        // scaled; the bars are no one's Canvas).
        const Editor::GameViewLayout::Rect   r = gv.ImageRect();
        const Editor::GameViewLayout::Extent e = gv.Extent();
        if (r.w > 0.f && r.h > 0.f && e.Valid())
        {
            minX   = r.x;
            minY   = r.y;
            scaleX = static_cast<float>(e.w) / r.w;
            scaleY = static_cast<float>(e.h) / r.h;
        }
        hostOwnsPointer = !gv.PointerOnPicture() || gv.OtherItemActive();
    }
    else if (gv.Route() == Editor::MainRoute::SceneEditor)
        hostOwnsPointer = true;   // the Scene shows the editor camera; the HUD is not on it
}

EditorLayer::GameInputGate EditorLayer::UpdateGameInputGate(const Plat::KeyboardState& keyboard,
                                                            bool gameWantsCapture)
{
    // Edges of the window's own keyboard, before Input::Update takes the frame's table (Win32 virtual keys).
    constexpr int kVkEscape = 0x1B, kVkF1 = 0x70, kVkShift = 0x10;
    const bool escDown    = keyboard.DownOrTapped(kVkEscape);
    const bool f1Down     = keyboard.DownOrTapped(kVkF1);
    const bool escPressed = escDown && !m_gateEscDown;
    const bool chord      = f1Down && !m_gateF1Down && keyboard.IsDown(kVkShift);
    m_gateEscDown = escDown;
    m_gateF1Down  = f1Down;

    Editor::GameView& gv = Editor::GameView::Get();
    const Editor::GameView::InputGate g = gv.UpdateInputGate(gameWantsCapture, escPressed, chord);
    GameInputGate out;
    out.hostFocus      = g.hostFocus;
    out.quarantineKey  = g.quarantineKey;
    out.captured       = g.captured;
    return out;
}

EditorLayer::WindowCursor EditorLayer::DesiredWindowCursor(bool flowWantsCapture) const
{
    const Editor::GameView& gv = Editor::GameView::Get();
    WindowCursor out;
    if (!gv.Simulating())
        return out;
    if (gv.Route() == Editor::MainRoute::SceneGame)
    {
        out.captured = flowWantsCapture;
        return out;
    }
    out.captured = gv.Captured();
    if (out.captured)
    {
        const Editor::GameViewLayout::Rect r = gv.ImageRect();
        out.clipped = true;
        out.clipX   = static_cast<long>(r.x);
        out.clipY   = static_cast<long>(r.y);
        out.clipW   = static_cast<long>(r.w);
        out.clipH   = static_cast<long>(r.h);
    }
    return out;
}

bool EditorLayer::SuppressGameplayMouse(bool imguiWantsMouse) const
{
    const Editor::GameView& gv = Editor::GameView::Get();
    if (!gv.Simulating() || gv.Route() == Editor::MainRoute::SceneGame)
        return imguiWantsMouse;
    if (gv.Route() != Editor::MainRoute::Game)
        return true;
    if (gv.Captured())
        return false;
    return !(gv.PointerOnPicture() && gv.GameHasKeyboard()) || gv.OtherItemActive();
}

// ==== The Scene viewport's auxiliary picture (Game route) ====

void EditorLayer::DrawSceneAuxPicture(ImDrawList* dl, const ImVec2& vpMin, const ImVec2& vpMax)
{
    Editor::GameView& gv = Editor::GameView::Get();
    dl->AddRectFilled(vpMin, vpMax, IM_COL32(12, 12, 14, 255));
    Entity editorCam = NullEntity;
    if (m_world)
        if (auto* tags = m_world->GetPool<EditorCameraTag>())
            for (Entity e : tags->Entities())
                if (m_world->IsAlive(e)) { editorCam = e; break; }

    const float boxW = vpMax.x - vpMin.x, boxH = vpMax.y - vpMin.y;
    // What the Scene shows in Edit: the editor's overlays, and the Canvas unless Show > Screen UI is off.
    std::uint32_t show = RG::kShowAll;
    if (m_renderer && m_renderer->HideCanvasUI()) show &= ~static_cast<std::uint32_t>(RG::ShowFlag::Hud);
    Editor::PanelView::Picture pic;
    if (m_renderer && editorCam != NullEntity)
        pic = gv.SceneAuxPicture(*m_renderer, editorCam, boxW, boxH, show);
    else
        pic.status = editorCam == NullEntity ? "no editor camera" : "no renderer";

    if (pic.srv && pic.w && pic.h)
    {
        const Editor::GameViewLayout::Rect r =
            Editor::GameViewLayout::FitRect({ pic.w, pic.h }, vpMin.x, vpMin.y, boxW, boxH);
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(pic.srv)), ImVec2(r.x, r.y),
                     ImVec2(r.x + r.w, r.y + r.h));
    }
    else
    {
        const char* text = pic.status.empty() ? "Rendering..." : pic.status.c_str();
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(vpMin.x + (boxW - tsz.x) * 0.5f, vpMin.y + (boxH - tsz.y) * 0.5f),
                    IM_COL32(150, 150, 165, 220), text);
    }
}

// ==== Lua ====

void EditorLayer::RegisterGameViewLua(lua_State* L)
{
    sol::state_view lua(L);
    sol::table ed = lua["Editor"].get_or_create<sol::table>();
    LuaDoc::Fn(ed, "Editor", "GameView",
        [this](sol::this_state ts) -> sol::table
        {
            EnsurePanelsRegistered();
            const Editor::GameView& gv = Editor::GameView::Get();
            const Editor::FocusTracker& focus = Editor::FocusTracker::Get();
            sol::state_view view(ts);
            sol::table t = view.create_table();
            t["open"]          = gv.IsOpen();
            t["visible"]       = focus.IsVisible(Editor::FocusTracker::kGameView);
            t["focused"]       = focus.IsFocused(Editor::FocusTracker::kGameView);
            t["sceneVisible"]  = focus.IsVisible(Editor::FocusTracker::kSceneView);
            t["route"]         = Editor::MainRouteName(gv.Route());
            t["owner"]         = m_renderer ? m_renderer->MainViewOwnerName() : "";
            t["active"]        = gv.ActivePanel() + 1;
            t["preset"]        = Editor::GameViewLayout::kPresets[gv.Preset()].label;
            t["extentW"]       = gv.Extent().w;
            t["extentH"]       = gv.Extent().h;
            sol::table img     = view.create_table();
            img["x"] = gv.ImageRect().x;
            img["y"] = gv.ImageRect().y;
            img["w"] = gv.ImageRect().w;
            img["h"] = gv.ImageRect().h;
            t["image"]         = img;
            t["camera"]        = gv.Camera().name;
            t["cameraEntity"]  = static_cast<std::uint32_t>(gv.Camera().entity);
            t["cameraReason"]  = gv.Camera().reason;
            t["sceneAuxView"]  = gv.SceneAux().ViewId();
            t["sceneAuxLease"] = gv.SceneAux().Lease();
            t["previewView"]   = gv.PanelPicture(0).ViewId();
            t["previewLease"]  = gv.PanelPicture(0).Lease();
            t["captured"]      = gv.Captured();
            t["windowCursor"]  = m_window && !m_window->CursorEnabled() ? "hidden" : "shown";
            sol::table panels  = view.create_table();
            for (int i = 0; i < Editor::GameView::kMaxPanels; ++i)
            {
                const Editor::GameView::CameraStatus& cam = gv.PanelCamera(i);
                const Editor::GameViewLayout::Rect r = gv.PanelImage(i);
                sol::table p = view.create_table();
                p["window"]       = Editor::GameView::kWindowIds[i];
                p["open"]         = gv.IsOpen(i);
                p["visible"]      = focus.IsVisible(Editor::GameView::kPanelIds[i]);
                p["focused"]      = focus.IsFocused(Editor::GameView::kPanelIds[i]);
                p["active"]       = gv.ActivePanel() == i;
                p["preset"]       = Editor::GameViewLayout::kPresets[gv.Preset(i)].label;
                p["extentW"]      = gv.PanelExtent(i).w;
                p["extentH"]      = gv.PanelExtent(i).h;
                p["image"]        = view.create_table_with("x", r.x, "y", r.y, "w", r.w, "h", r.h);
                p["camera"]       = cam.name;
                p["cameraEntity"] = static_cast<std::uint32_t>(cam.entity);
                p["cameraReason"] = cam.reason;
                p["cameraPick"]   = static_cast<std::uint32_t>(gv.CameraPick(i));
                p["view"]         = gv.PanelPicture(i).ViewId();
                p["lease"]        = gv.PanelPicture(i).Lease();
                panels[i + 1] = p;
            }
            t["panels"] = panels;
            // The Scene viewport's camera: the editor's free camera, which it keeps in Play.
            if (m_world)
                if (auto* tags = m_world->GetPool<EditorCameraTag>())
                    for (Entity e : tags->Entities())
                    {
                        if (!m_world->IsAlive(e)) continue;
                        t["sceneCamera"] = LuaId::Out(*m_world, e);
                        if (const auto* gt = m_world->GetComponent<GlobalTransform>(e))
                            t["sceneCameraPos"] = view.create_table_with(1, gt->matrix._41, 2, gt->matrix._42, 3,
                                                                         gt->matrix._43);
                        break;
                    }
            return t;
        },
        { .doc = "The Game Views and the main pipeline's route: the route (SceneEditor, SceneGame or Game), the main "
                 "view's owner (editor or game), the panel it serves (active), that panel's render extent and "
                 "picture rectangle, the game camera (or why there is none), the first Game View's open / shown / "
                 "focused state, framing and view as before, every panel's (panels), the Scene's view, the mouse "
                 "capture and the window cursor.",
          .returns = "table",
          .returnsDoc = "{open, visible, focused, sceneVisible, route, owner, active (1..3, 0 = none), preset, "
                        "extentW, extentH, image = {x, y, w, h}, camera, cameraEntity, cameraReason, sceneAuxView, "
                        "sceneAuxLease, previewView, previewLease (a secondary view's id and lease as Render.views() "
                        "and a capture name it; -1 / 0 without one), panels = {{window, open, visible, focused, "
                        "active, preset, extentW, extentH, image, camera, cameraEntity, cameraReason, cameraPick "
                        "(0 = the game camera), view, lease}, ...}, captured, windowCursor = \"hidden\"|\"shown\", "
                        "sceneCamera, sceneCameraPos = {x, y, z}}",
          .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Editor });
    LuaDoc::Fn(ed, "Editor", "SetGameView",
        [this](sol::table opts) -> bool
        {
            EnsurePanelsRegistered();
            Editor::GameView& gv = Editor::GameView::Get();
            int panel = 0;
            if (sol::optional<int> n = opts["panel"])
            {
                if (*n < 1 || *n > Editor::GameView::kMaxPanels)
                    throw sol::error("Editor.SetGameView: bad_panel: " + std::to_string(*n) + " (1, 2 or 3)");
                panel = *n - 1;
            }
            if (sol::optional<std::string> preset = opts["preset"])
            {
                int found = -1;
                for (int i = 0; i < Editor::GameViewLayout::kPresetCount; ++i)
                    if (*preset == Editor::GameViewLayout::kPresets[i].label) found = i;
                if (found < 0)
                    throw sol::error("Editor.SetGameView: bad_preset: '" + *preset
                                     + "' (Free Aspect, 16:9, 16:10, 4:3, 21:9, 1:1, 9:16, 1280x720, 1920x1080, "
                                       "2560x1440)");
                gv.SetPreset(found, panel);
            }
            if (sol::object camera = opts["camera"]; camera.valid() && camera != sol::lua_nil)
            {
                if (camera.is<std::string>() && camera.as<std::string>() == "game")
                    gv.SetCameraPick(panel, NullEntity);
                else if (camera.is<LuaEntity>() && m_world)
                {
                    const Entity e = LuaId::ResolveLive(*m_world, camera.as<LuaEntity>());
                    if (e == NullEntity || !m_world->HasComponent<CameraComponent>(e))
                        throw sol::error("Editor.SetGameView: bad_camera: not a live entity with a CameraComponent");
                    gv.SetCameraPick(panel, e);
                }
                else
                    throw sol::error("Editor.SetGameView: bad_camera: \"game\" or a camera entity");
            }
            if (sol::optional<bool> open = opts["open"])
                Editor::PanelRegistry::Get().SetOpen(Editor::GameView::kPanelIds[panel], *open);
            if (sol::optional<std::string> dock = opts["dock"])
            {
                if (*dock == "beside")   gv.RequestDock(Editor::GameView::Dock::BesideScene, panel);
                else if (*dock == "tab") gv.RequestDock(Editor::GameView::Dock::TabWithScene, panel);
                else throw sol::error("Editor.SetGameView: bad_dock: '" + *dock + "' (beside, tab)");
            }
            if (sol::optional<std::string> focus = opts["focus"])
            {
                if (*focus == "game")       gv.RequestFocus(Editor::GameView::Focus::Game, panel);
                else if (*focus == "scene") gv.RequestFocus(Editor::GameView::Focus::Scene);
                else throw sol::error("Editor.SetGameView: bad_focus: '" + *focus + "' (game, scene)");
            }
            return gv.IsOpen(panel);
        },
        { .doc = "Sets a Game View as its toolbar and the Window menu do: open or close it, the framing preset, the "
                 "camera (the game's, or a camera entity), dock it beside the Scene viewport or as its tab, focus it "
                 "or the Scene viewport. panel picks which (1, the default, 2 or 3). A dock or focus applies at the "
                 "next frame.",
          .params = { { "options", "{panel = 1 | 2 | 3, open = bool, preset = \"16:9\", camera = \"game\" | entity, "
                                   "dock = \"beside\" | \"tab\", focus = \"game\" | \"scene\"}", "table" } },
          .returns = "boolean", .returnsDoc = "whether that panel is open",
          .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Editor,
          .errors = { "bad_panel", "bad_preset", "bad_camera", "bad_dock", "bad_focus" } });
}
