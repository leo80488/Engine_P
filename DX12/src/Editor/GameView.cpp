#include "Editor/GameView.h"

#include "ECS/CameraStackComponents.h"   // CameraStackChannelSingleton, VCamPriorityComponent
#include "ECS/CameraStackSystem.h"       // Camera::FindChannelEntity
#include "ECS/Components.h"              // CameraComponent, ActiveCameraTag, EditorCameraTag
#include "Editor/EditorLayerShared.h"   // kWindowViewport
#include "Editor/FocusTracker.h"
#include "Renderer/Renderer.h"
#include "RenderGraph/SceneView.h"   // the panels' ShowFlags
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"   // FindWindowSettingsByID: does imgui.ini know a place for the window

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    // A view's resize recreates its targets: a dock drag commits its size once it has rested this long
    // (RestingSize); the Scene's view also moves in 16 px steps.
    constexpr double        kResizeDebounceSeconds = 0.2;
    constexpr std::uint32_t kAuxSizeStep           = 16;

    std::uint32_t QuantiseAux(float px)
    {
        const std::uint32_t v = static_cast<std::uint32_t>(std::max(0.f, px)) / kAuxSizeStep * kAuxSizeStep;
        return std::max(v, Editor::GameViewLayout::kMinExtent);
    }

    // Win32 virtual keys, without <Windows.h> in an editor panel.
    constexpr int kVkEscape = 0x1B;
    constexpr int kVkF1     = 0x70;

    void DrawBadge(ImDrawList* dl, float x, float y, ImU32 color, const char* text)
    {
        const ImVec2 tsz = ImGui::CalcTextSize(text);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + tsz.x + 12.f, y + tsz.y + 6.f), IM_COL32(0, 0, 0, 160), 3.f);
        dl->AddText(ImVec2(x + 6.f, y + 3.f), color, text);
    }

    std::string EntityLabel(World& world, Entity e)
    {
        std::string name = world.GetName(e);
        return name.empty() ? "entity " + std::to_string(e) : name;
    }
}

const char* Editor::MainRouteName(MainRoute route)
{
    switch (route)
    {
    case MainRoute::SceneEditor: return "SceneEditor";
    case MainRoute::SceneGame:   return "SceneGame";
    case MainRoute::Game:        return "Game";
    }
    return "?";
}

Editor::GameView& Editor::GameView::Get()
{
    static GameView s_view;
    return s_view;
}

void Editor::GameView::SetOpen(bool open, int panel)
{
    if (!Valid(panel)) return;
    Panel& p = m_panels[panel];
    if (open && !p.open)
    {
        // First open with no remembered place: beside the Scene viewport, not floating over it. A window
        // imgui.ini already docked (or the user floated) keeps its place.
        const ImGuiWindowSettings* settings = ImGui::GetCurrentContext()
                                                  ? ImGui::FindWindowSettingsByID(ImHashStr(kWindowIds[panel]))
                                                  : nullptr;
        if (!settings || settings->DockId == 0)
            p.dockRequest = Dock::BesideScene;
    }
    p.open = open;
}

bool Editor::GameView::AnyOpen() const
{
    for (const Panel& p : m_panels)
        if (p.open) return true;
    return false;
}

void Editor::GameView::SetPreset(int preset, int panel)
{
    m_panels[Clamp(panel)].preset = GameViewLayout::ClampPreset(preset);
}

void Editor::GameView::SetCameraPick(int panel, Entity camera)
{
    if (!Valid(panel)) return;
    if (m_panels[panel].cameraPick != camera)
        LOG_INFO("[GAMEVIEW] %s looks through %s", kWindowIds[panel],
                 camera == NullEntity ? "the game camera" : ("entity " + std::to_string(camera)).c_str());
    m_panels[panel].cameraPick = camera;
}

Editor::GameView::Dock Editor::GameView::ConsumeDockRequest(int panel)
{
    if (!Valid(panel)) return Dock::None;
    const Dock d = m_panels[panel].dockRequest;
    m_panels[panel].dockRequest = Dock::None;
    return d;
}

void Editor::GameView::RequestFocus(Focus focus, int panel)
{
    if (focus == Focus::Scene)
        m_focusScene = true;
    else if (focus == Focus::Game)
        m_panels[Clamp(panel)].focusAsked = true;
}

// ==== Route ====

bool Editor::GameView::Candidate(int panel) const
{
    const Panel& p = m_panels[panel];
    return p.open && p.cameraPick == NullEntity && p.extent.Valid()
           && FocusTracker::Get().IsVisible(kPanelIds[panel]);
}

Editor::MainRoute Editor::GameView::UpdateRoute(bool simulating, bool fullscreen)
{
    const MainRoute before = m_route;
    const FocusTracker& focus = FocusTracker::Get();
    // Play focuses an open Game View that shows the game (the one the main view served last, else the first),
    // its tab coming to the front; Stop hands the focus back to the Scene viewport when that panel still has it -
    // the dock layout itself is never changed.
    if (simulating && !m_simulating && !fullscreen)
    {
        int pick = m_active >= 0 && m_panels[m_active].open ? m_active : -1;
        for (int i = 0; i < kMaxPanels && pick < 0; ++i)
            if (m_panels[i].open && m_panels[i].cameraPick == NullEntity) pick = i;
        if (pick >= 0)
        {
            m_panels[pick].focusAsked = true;
            m_focusedForPlay          = pick;
        }
    }
    else if (!simulating && m_simulating && m_focusedForPlay >= 0)
    {
        if (focus.IsFocused(kPanelIds[m_focusedForPlay])) m_focusScene = true;
        m_focusedForPlay = -1;
    }
    m_simulating = simulating;

    // The panel the main view serves: the focused candidate, else the one it served while it still can, else the
    // first that can.
    int active = -1;
    if (simulating && !fullscreen)
    {
        for (int i = 0; i < kMaxPanels; ++i)
            if (Candidate(i) && focus.IsFocused(kPanelIds[i])) active = i;
        if (active < 0 && m_active >= 0 && Candidate(m_active)) active = m_active;
        for (int i = 0; i < kMaxPanels && active < 0; ++i)
            if (Candidate(i)) active = i;
    }
    if (active != m_active && active >= 0)
        LOG_INFO("[GAMEVIEW] the main view serves %s (%ux%u)", kWindowIds[active], m_panels[active].extent.w,
                 m_panels[active].extent.h);
    m_active = active;

    const bool sceneVisible = focus.IsVisible(FocusTracker::kSceneView);
    if (!simulating)
        m_route = MainRoute::SceneEditor;
    else if (fullscreen || !AnyOpen())
        m_route = MainRoute::SceneGame;
    else if (m_active >= 0)
        m_route = MainRoute::Game;
    else if (sceneVisible)
        m_route = MainRoute::SceneEditor;
    // Neither showing (all behind other tabs): the route stays, nothing is gained by moving it.

    if (m_route != before)
        LOG_INFO("[GAMEVIEW] route %s -> %s (simulating=%d open=%d gameVisible=%d sceneVisible=%d "
                 "fullscreen=%d extent=%ux%u)",
                 MainRouteName(before), MainRouteName(m_route), simulating ? 1 : 0, AnyOpen() ? 1 : 0,
                 m_active >= 0 ? 1 : 0, sceneVisible ? 1 : 0, fullscreen ? 1 : 0, Extent().w, Extent().h);
    return m_route;
}

// ==== Game input and the capture ====

Editor::GameView::InputGate Editor::GameView::UpdateInputGate(bool gameWantsCapture, bool escPressed,
                                                              bool releaseChordPressed)
{
    InputGate gate;
    const bool want      = m_captureOverride >= 0 ? m_captureOverride != 0 : gameWantsCapture;
    const bool gameRoute = m_simulating && m_route == MainRoute::Game;
    const bool focused   = m_active >= 0 && FocusTracker::Get().IsFocused(kPanelIds[m_active]);
    const bool clicked   = m_active >= 0 && m_panels[m_active].pictureClicked;
    if (want && !m_wantedLastFrame) m_escReleased = false;   // a new ask captures again
    m_wantedLastFrame = want;

    if (!gameRoute)
    {
        if (m_captured) LOG_INFO("[GAMEVIEW] capture released: the Game View no longer shows the game");
        m_captured = m_escReleased = false;
    }
    else if (m_captured)
    {
        if (m_escReleasesCapture ? escPressed : releaseChordPressed)
        {
            // The key that releases the capture is the editor's: the game never sees its press.
            m_captured    = false;
            m_escReleased = true;
            gate.quarantineKey = m_escReleasesCapture ? kVkEscape : kVkF1;
            LOG_INFO("[GAMEVIEW] capture released by %s; the game did not see it",
                     m_escReleasesCapture ? "Esc" : "Shift+F1");
        }
        else if (!want || !focused)
        {
            m_captured = false;
            LOG_INFO("[GAMEVIEW] capture released: %s", !want ? "the game stopped asking" : "the panel lost focus");
        }
    }
    else if (want && focused)
    {
        if (clicked) m_escReleased = false;
        if (!m_escReleased)
        {
            m_captured = true;
            LOG_INFO("[GAMEVIEW] captured: the cursor is hidden and kept on the picture");
        }
    }
    for (Panel& p : m_panels) p.pictureClicked = false;

    // The Game View's routes: the game reads input only while the active panel has focus or the capture; the
    // single-view editor keeps the hover rules it always had.
    const bool gameViewRoutes = m_simulating && m_route != MainRoute::SceneGame;
    gate.hostFocus  = !gameViewRoutes || (gameRoute && (focused || m_captured));
    gate.captured   = m_captured;
    m_gateHostFocus = gate.hostFocus;
    return gate;
}

// ==== Cameras ====

void Editor::GameView::UpdateCamera(World* world, bool simulating)
{
    CameraStatus status;
    if (!world)
    {
        status.reason = "no world";
        m_camera = status;
        for (Panel& p : m_panels) p.camera = status;
        return;
    }
    if (simulating)
    {
        // What the main pipeline renders in Play: the Main channel's highest enabled camera. The editor's own
        // camera is parked there and never counts, so a scene without one shows the reason, never the
        // editor's view.
        const Entity channel = Camera::FindChannelEntity(*world, Camera::kMainChannel);
        if (const auto* stack = channel != NullEntity
                                    ? world->GetComponent<CameraStackChannelSingleton>(channel) : nullptr)
            for (const CameraStackChannelSingleton::StackEntry& e : stack->entries)
            {
                if (!world->IsAlive(e.vcam) || world->HasComponent<EditorCameraTag>(e.vcam)) continue;
                const auto* prio = world->GetComponent<VCamPriorityComponent>(e.vcam);
                if (!prio || !prio->enabled) continue;
                status.entity = e.vcam;
                break;
            }
        if (status.entity == NullEntity && ++m_cameraMissFrames < 2 && m_camera.reason.empty())
            status = m_camera;   // the first Play frame: the scene camera joins the stack on the next one
        else if (status.entity == NullEntity)
        {
            if (!m_lastCameraName.empty())
                status.reason = "The game camera '" + m_lastCameraName + "' is gone (destroyed or disabled), and "
                                "no other enabled camera is on the Main channel.";
            else
                status.reason = "No game camera: the scene has no CameraComponent and no enabled VirtualCamera "
                                "on the Main channel.";
        }
    }
    else
    {
        // The camera Play will start from, by App::RefreshMainCamera's rule.
        world->ForEach<ActiveCameraTag>([&](Entity e, ActiveCameraTag&)
        {
            if (status.entity == NullEntity && world->IsAlive(e) && world->HasComponent<CameraComponent>(e))
                status.entity = e;
        });
        if (status.entity == NullEntity)
            for (Entity e : world->GetEntities())
                if (world->IsAlive(e) && world->HasComponent<CameraComponent>(e))
                {
                    status.entity = e;
                    break;
                }
        if (status.entity == NullEntity)
            status.reason = "No game camera: the scene has no entity with a CameraComponent, so Play would have "
                            "nothing to render the game through.";
    }
    if (status.entity != NullEntity && status.name.empty())
    {
        m_cameraMissFrames = 0;
        status.name = EntityLabel(*world, status.entity);
        m_lastCameraName = simulating ? status.name : std::string();
    }
    if (status.entity != m_camera.entity || status.reason != m_camera.reason)
    {
        if (status.entity != NullEntity)
            LOG_INFO("[GAMEVIEW] game camera: '%s' (entity %u, %s)", status.name.c_str(), status.entity,
                     simulating ? "Play" : "Edit");
        else
            LOG_INFO("[GAMEVIEW] game camera: none - %s", status.reason.c_str());
    }
    m_camera = std::move(status);

    // Each panel's: the game camera, or the camera entity it picked while that one lasts.
    for (int i = 0; i < kMaxPanels; ++i)
    {
        Panel& p = m_panels[i];
        if (p.cameraPick == NullEntity)
        {
            p.camera = m_camera;
            continue;
        }
        CameraStatus picked;
        if (world->IsAlive(p.cameraPick) && world->HasComponent<CameraComponent>(p.cameraPick))
        {
            picked.entity = p.cameraPick;
            picked.name   = EntityLabel(*world, p.cameraPick);
        }
        else
            picked.reason = "The camera this view picked (entity " + std::to_string(p.cameraPick) + ") is gone. "
                            "Pick another in the toolbar.";
        p.camera = std::move(picked);
    }
}

// ==== The Scene viewport's view ====

void Editor::GameView::RestingSize::Offer(std::uint32_t wantW, std::uint32_t wantH, double now)
{
    if (w == 0 || h == 0)
    {
        w = pendW = wantW;
        h = pendH = wantH;
    }
    else if (wantW != pendW || wantH != pendH)
    {
        pendW  = wantW;
        pendH  = wantH;
        pendAt = now;
    }
    else if ((pendW != w || pendH != h) && now - pendAt >= kResizeDebounceSeconds)
    {
        w = pendW;
        h = pendH;
    }
}

Editor::PanelView::Picture Editor::GameView::SceneAuxPicture(Renderer& renderer, Entity editorCamera, float boxW,
                                                             float boxH, std::uint32_t show)
{
    m_auxSize.Offer(QuantiseAux(boxW), QuantiseAux(boxH), ImGui::GetTime());
    return m_sceneAux.Request(renderer, editorCamera, m_auxSize.w, m_auxSize.h, show);
}

// ==== Frame boundary ====

void Editor::GameView::ProcessPendingActions(Renderer* renderer, bool simulating)
{
    // At the Play / Stop edge, at once: the main view takes one of the panels' pictures over at Play, the Scene's
    // at Stop, and the slots go back before anything asks for a new one.
    if (simulating && !m_boundarySimulating)
        for (Panel& p : m_panels) p.view.ReleaseNow(renderer, "Play started");
    else if (!simulating && m_boundarySimulating)
        m_sceneAux.ReleaseNow(renderer, "Play ended");
    m_boundarySimulating = simulating;
    m_sceneAux.ProcessPendingActions(renderer);
    if (!m_sceneAux.Live()) m_auxSize.Reset();
    for (Panel& p : m_panels)
    {
        p.view.ProcessPendingActions(renderer);
        if (!p.view.Live()) p.viewSize.Reset();
    }
}

void Editor::GameView::ReleaseViewsAtShutdown(Renderer* renderer)
{
    m_sceneAux.ReleaseAtShutdown(renderer);
    for (Panel& p : m_panels) p.view.ReleaseAtShutdown(renderer);
}

// ==== The windows ====

void Editor::GameView::DrawToolbar(const DrawInputs& in, int panel)
{
    Panel& p = m_panels[panel];
    ImGui::PushID(panel);
    ImGui::SetNextItemWidth(130.f);
    if (ImGui::BeginCombo("##gameViewPreset", GameViewLayout::kPresets[p.preset].label))
    {
        for (int i = 0; i < GameViewLayout::kPresetCount; ++i)
        {
            if (i > 0 && GameViewLayout::kPresets[i].kind != GameViewLayout::kPresets[i - 1].kind)
                ImGui::Separator();
            if (ImGui::Selectable(GameViewLayout::kPresets[i].label, i == p.preset))
                SetPreset(i, panel);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Free Aspect renders at the panel's size; a ratio renders the largest picture of\n"
                          "that shape inside it; a resolution renders at exactly that size, shown scaled.\n"
                          "The HUD lays out at the rendered size.");
    ImGui::SameLine();
    if (p.extent.Valid() && p.image.w > 0.f)
        ImGui::TextDisabled("%ux%u  %.0f%%", p.extent.w, p.extent.h,
                            100.f * p.image.w / static_cast<float>(p.extent.w));
    else
        ImGui::TextDisabled("(no size)");

    // The camera: the game's, or a camera entity of the scene.
    ImGui::SameLine();
    std::string current = p.cameraPick == NullEntity
                              ? "Game camera" + (p.camera.name.empty() ? std::string() : " (" + p.camera.name + ")")
                              : (p.camera.reason.empty() ? p.camera.name : std::string("(gone)"));
    ImGui::SetNextItemWidth(170.f);
    if (ImGui::BeginCombo("##gameViewCamera", current.c_str()))
    {
        if (ImGui::Selectable("Game camera", p.cameraPick == NullEntity))
            SetCameraPick(panel, NullEntity);
        if (in.world)
        {
            World& world = *in.world;
            world.ForEach<CameraComponent>([&](Entity e, CameraComponent&)
            {
                if (!world.IsAlive(e) || world.HasComponent<EditorCameraTag>(e)) return;
                ImGui::PushID(static_cast<int>(e));
                if (ImGui::Selectable(EntityLabel(world, e).c_str(), p.cameraPick == e))
                    SetCameraPick(panel, e);
                ImGui::PopID();
            });
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", !p.camera.reason.empty()
                                    ? p.camera.reason.c_str()
                                    : p.cameraPick != NullEntity
                                        ? "This view looks through the picked camera; the game's input and the HUD's\n"
                                          "clicks stay with the Game View that shows the game camera."
                                        : (in.simulating ? "The camera the game renders through."
                                                         : "The camera Play will start from."));

    ImGui::SameLine();
    if (ImGui::SmallButton("Dock"))
        ImGui::OpenPopup("##gameViewDock");
    if (ImGui::BeginPopup("##gameViewDock"))
    {
        if (ImGui::MenuItem("Beside the Scene viewport")) p.dockRequest = Dock::BesideScene;
        if (ImGui::MenuItem("Tab with the Scene viewport")) p.dockRequest = Dock::TabWithScene;
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void Editor::GameView::DrawPicture(const DrawInputs& in, int panel, float boxX, float boxY, float boxW, float boxH)
{
    Panel& p = m_panels[panel];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 imgMin(p.image.x, p.image.y);
    const ImVec2 imgMax(p.image.x + p.image.w, p.image.y + p.image.h);
    // The bars: the box outside the picture.
    dl->AddRectFilled(ImVec2(boxX, boxY), ImVec2(boxX + boxW, boxY + boxH), IM_COL32(12, 12, 14, 255));

    auto message = [&](const char* text, ImU32 color)
    {
        dl->AddRectFilled(imgMin, imgMax, IM_COL32(40, 44, 52, 255));
        const float wrap = std::max(120.f, p.image.w - 40.f);
        const ImVec2 tsz = ImGui::CalcTextSize(text, nullptr, false, wrap);
        dl->AddText(nullptr, 0.f,
                    ImVec2(imgMin.x + std::max(10.f, (p.image.w - tsz.x) * 0.5f),
                           imgMin.y + (p.image.h - tsz.y) * 0.5f),
                    color, text, nullptr, wrap);
    };

    if (m_route == MainRoute::Game && panel == m_active)
    {
        // The main pipeline's picture at the panel's extent - unless there is no game camera: then the reason,
        // never the editor camera's view the pipeline may still hold.
        if (!m_camera.reason.empty())
            message(m_camera.reason.c_str(), IM_COL32(255, 175, 90, 255));
        else if (in.sceneTexture != 0)
            dl->AddImage(ImTextureRef(static_cast<ImTextureID>(in.sceneTexture)), imgMin, imgMax);
        return;
    }

    // Its own secondary view of the main pipeline at its extent: in Edit the camera Play will start from (or its
    // pick) - the picture Play will show, HUD included; in Play a panel the main view does not serve.
    if (!p.camera.reason.empty() || !in.renderer)
    {
        message(!p.camera.reason.empty() ? p.camera.reason.c_str() : "No renderer.", IM_COL32(255, 175, 90, 255));
        return;
    }
    p.viewSize.Offer(p.extent.w, p.extent.h, ImGui::GetTime());
    const std::uint32_t show = RG::kShowAll & ~static_cast<std::uint32_t>(RG::ShowFlag::EditorOverlays);
    const PanelView::Picture pic =
        p.view.Request(*in.renderer, p.camera.entity, p.viewSize.w, p.viewSize.h, show);
    if (pic.srv)
        dl->AddImage(ImTextureRef(static_cast<ImTextureID>(pic.srv)), imgMin, imgMax);
    else
        message(pic.status.empty() ? "Rendering..." : pic.status.c_str(), IM_COL32(150, 150, 165, 220));
    if (!in.simulating)
    {
        DrawBadge(dl, imgMin.x + 8.f, imgMin.y + 8.f, IM_COL32(150, 200, 255, 255), "EDIT PREVIEW");
        if (ImGui::IsMouseHoveringRect(imgMin, imgMax) && ImGui::IsWindowHovered())
            ImGui::SetTooltip("This view's camera through the full renderer with the HUD.\n"
                              "The game does not run: press Play to play it here.");
    }
    else if (ImGui::IsMouseHoveringRect(imgMin, imgMax) && ImGui::IsWindowHovered())
        ImGui::SetTooltip("%s", p.cameraPick == NullEntity
                                    ? "The game through another Game View: click to play here (input and the HUD's\n"
                                      "clicks follow the focused Game View that shows the game camera)."
                                    : "The picked camera's view: the game's input stays with the Game View that\n"
                                      "shows the game camera.");
}

void Editor::GameView::Draw(const DrawInputs& in, int panel)
{
    if (!Valid(panel)) return;
    Panel& p = m_panels[panel];
    p.pictureHovered = p.pointerOnPicture = p.otherItemActive = p.rightDragging = false;
    p.mouseDX = p.mouseDY = 0.f;

    if (p.focusAsked)
    {
        ImGui::SetNextWindowFocus();
        p.focusAsked = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool open = p.open;
    const bool visible = ImGui::Begin(kWindowIds[panel], &open);
    ImGui::PopStyleVar();
    FocusTracker::Get().Note(kPanelIds[panel], visible);
    if (!open) SetOpen(false, panel);
    if (!visible)
    {
        ImGui::End();
        FocusSceneIfAsked();
        return;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + 6.f, ImGui::GetCursorPosY() + 3.f));
    DrawToolbar(in, panel);
    ImGui::PopStyleVar();
    ImGui::Separator();

    const ImVec2 box    = ImGui::GetContentRegionAvail();
    const ImVec2 boxMin = ImGui::GetCursorScreenPos();
    if (const GameViewLayout::Extent e = GameViewLayout::ExtentFor(p.preset, box.x, box.y); e.Valid())
        p.extent = e;
    p.image = GameViewLayout::FitRect(p.extent, boxMin.x, boxMin.y, box.x, box.y);

    if (box.x > 0.f && box.y > 0.f)
    {
        DrawPicture(in, panel, boxMin.x, boxMin.y, box.x, box.y);

        // The picture takes the clicks (focus, the game's capture) and owns ImGui's hover there; the
        // InvisibleButton holds the ActiveId for a press, which OtherItemActive exempts.
        ImGui::SetCursorScreenPos(boxMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##gameViewPicture", box,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight
                                   | ImGuiButtonFlags_MouseButtonMiddle);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (in.pointerValid)
        {
            const ImVec2 ptr(in.pointerX, in.pointerY);
            if (ptr.x >= p.image.x && ptr.y >= p.image.y && ptr.x < p.image.x + p.image.w
                && ptr.y < p.image.y + p.image.h)
            {
                ImGuiWindow* top = nullptr;
                ImGui::FindHoveredWindowEx(ptr, false, &top, nullptr);
                p.pointerOnPicture = top && top->RootWindow == ImGui::GetCurrentWindow()->RootWindow;
            }
        }
        const ImVec2 mp = ImGui::GetIO().MousePos;
        p.pictureHovered = hovered && mp.x >= p.image.x && mp.y >= p.image.y && mp.x < p.image.x + p.image.w
                           && mp.y < p.image.y + p.image.h;
        p.otherItemActive = ImGui::IsAnyItemActive() && !ImGui::IsItemActive();
        // A press on the picture takes the capture back after an Esc (UpdateInputGate).
        if (p.pictureHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                                 || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            p.pictureClicked = true;
        // Esc seen by ImGui while captured (a key the window's keyboard did not carry, as a UI test's): the
        // same release, and nothing of it reached the game's input.
        if (panel == m_active && m_captured && m_escReleasesCapture
            && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            m_captured    = false;
            m_escReleased = true;
            LOG_INFO("[GAMEVIEW] capture released by Esc (ImGui)");
        }
        p.rightDragging = p.pictureHovered && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (p.rightDragging)
        {
            p.mouseDX = ImGui::GetIO().MouseDelta.x;
            p.mouseDY = ImGui::GetIO().MouseDelta.y;
        }
    }
    ImGui::End();
    FocusSceneIfAsked();
}

void Editor::GameView::FocusSceneIfAsked()
{
    if (!m_focusScene) return;
    ImGui::SetWindowFocus(kWindowViewport);
    m_focusScene = false;
}
