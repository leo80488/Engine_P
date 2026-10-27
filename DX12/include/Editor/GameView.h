#pragma once

// GameView - the editor's Game View panels: dockable windows that show the game as the player sees it - a camera,
// the HUD, a chosen aspect or resolution - beside the Scene viewport, which keeps the editor camera. Up to three
// (Game View, Game View 2, Game View 3), each with its own framing and camera: the game camera (the Main channel)
// or a camera entity picked in its toolbar.
// It decides the main pipeline's ROUTE once a frame: in Play the main view draws the game into the ACTIVE panel -
// the most recently focused visible one that shows the game camera - at its extent, and the game's input, the
// mouse capture and the HUD's hit test are that panel's; the other visible panels and the Scene viewport (the
// editor camera) are secondary views of the same pipeline (Editor/PanelView.h). With no panel showing the game the
// Scene viewport shows the editor camera through the main view; with every panel closed (or F11) nothing changes
// from the single-view editor. In Edit each visible panel shows its camera (the one Play will start from, HUD
// included) through a secondary view at its extent.
// The panel registry draws them (core.gameView, core.gameView2, core.gameView3) and the FocusTracker reports what
// they showed.
// Editor-only: compiled into EditorCore.

#include "ECS/ECS.h"   // Entity
#include "Editor/GameViewLayout.h"
#include "Editor/PanelView.h"

#include <cstdint>
#include <string>

class Renderer;
class World;

namespace Editor
{
    // Which panel the main pipeline draws into, through which camera.
    enum class MainRoute : std::uint8_t
    {
        SceneEditor,   // the editor camera into the Scene viewport: Edit, and Play with no Game View showing it
        SceneGame,     // the game camera into the Scene viewport: Play with no Game View open, or F11
        Game,          // the game camera and HUD into the active Game View: Play with one visible
    };
    const char* MainRouteName(MainRoute route);

    class GameView
    {
    public:
        // In Play the active panel is the main view and the Scene and the other two take the three secondary
        // views (RHI::kMaxViews); in Edit the Scene is the main view and the three panels the secondary ones.
        static constexpr int         kMaxPanels             = 3;
        static constexpr const char* kPanelIds[kMaxPanels]  = { "core.gameView", "core.gameView2", "core.gameView3" };
        static constexpr const char* kWindowIds[kMaxPanels] = { "Game View", "Game View 2", "Game View 3" };
        static constexpr const char* kPanelId  = kPanelIds[0];
        static constexpr const char* kWindowId = kWindowIds[0];

        // Where the window is docked when it opens without a saved place (imgui.ini knows nothing of it),
        // or when the panel's Dock menu asks.
        enum class Dock : std::uint8_t { None, BesideScene, TabWithScene };

        // What a frame of a panel draws with, gathered by EditorLayer.
        struct DrawInputs
        {
            Renderer*          renderer     = nullptr;
            World*             world        = nullptr;
            unsigned long long sceneTexture = 0;       // the main pipeline's picture (an id patched at record)
            bool               simulating   = false;   // Play, Pause or Step
            bool               paused       = false;
            // The window's own pointer (client pixels): what the game's Canvas reads.
            bool               pointerValid = false;
            float              pointerX     = 0.f;
            float              pointerY     = 0.f;
        };

        // The camera a panel renders through, or why there is none.
        struct CameraStatus
        {
            Entity      entity = NullEntity;
            std::string name;
            std::string reason;   // empty when there is a camera
        };

        static GameView& Get();

        // ---- Open state (the registry binds these) ----
        bool IsOpen(int panel = 0) const { return Valid(panel) && m_panels[panel].open; }
        void SetOpen(bool open, int panel = 0);
        bool AnyOpen() const;

        // ---- Route (App: once a frame, after the Edit / Play transition) ----
        // This frame's route and active panel, from the play state, the open states and what the FocusTracker saw
        // drawn and focused in the last frame. @p simulating = Play / Pause / Step; @p fullscreen = F11.
        MainRoute UpdateRoute(bool simulating, bool fullscreen);
        MainRoute Route() const { return m_route; }
        bool      Simulating() const { return m_simulating; }
        // The panel the main view serves on the Game route; -1 when none does.
        int       ActivePanel() const { return m_active; }
        // The editor camera is the Scene viewport's camera in Play (SceneEditor through the main view, Game
        // through its own secondary view): the Scene's own mouse and keys fly it, the game's never do.
        bool SceneObservesPlay() const { return m_simulating && m_route != MainRoute::SceneGame; }

        // ---- Cameras (frame boundary) ----
        // The game camera: in Play the Main channel's highest enabled camera that is not the editor's; in Edit
        // the camera Play will start from (the ActiveCameraTag one, else the first CameraComponent). Then each
        // panel's (PanelCamera).
        void UpdateCamera(World* world, bool simulating);
        const CameraStatus& Camera() const { return m_camera; }
        // What panel @p panel renders through this frame: the game camera, or the entity its toolbar picked.
        const CameraStatus& PanelCamera(int panel) const { return m_panels[Clamp(panel)].camera; }
        // The pick: NullEntity = the game camera. A panel with a picked camera is never the main view's.
        Entity CameraPick(int panel) const { return m_panels[Clamp(panel)].cameraPick; }
        void   SetCameraPick(int panel, Entity camera);

        // ---- Framing ----
        int  Preset(int panel = 0) const { return m_panels[Clamp(panel)].preset; }
        void SetPreset(int preset, int panel = 0);
        // The active panel's (the first panel's while none is active): the last valid render extent its picture
        // box gave (kept while the box is collapsed) - the main view's size on the Game route - and where its
        // picture is shown, in screen pixels (its letterbox bars are the rest of the box).
        GameViewLayout::Extent Extent() const { return Focal().extent; }
        GameViewLayout::Rect   ImageRect() const { return Focal().image; }
        GameViewLayout::Extent PanelExtent(int panel) const { return m_panels[Clamp(panel)].extent; }
        GameViewLayout::Rect   PanelImage(int panel) const { return m_panels[Clamp(panel)].image; }

        // ---- The Scene viewport's view (Game route) and the panels' own ----
        // This frame's picture of @p editorCamera for a Scene picture box of @p boxW x @p boxH, showing @p show
        // (RG::ShowFlag bits; UI pass).
        PanelView::Picture SceneAuxPicture(Renderer& renderer, Entity editorCamera, float boxW, float boxH,
                                           std::uint32_t show);
        const PanelView& SceneAux() const { return m_sceneAux; }
        // A panel's secondary view: in Edit its preview, in Play its picture while the main view serves another.
        const PanelView& PanelPicture(int panel) const { return m_panels[Clamp(panel)].view; }

        // ---- Game input and the mouse capture (App, before the input snapshot) ----
        struct InputGate
        {
            bool hostFocus      = true;    // the game may read keys, buttons, the pad and look
            int  quarantineKey  = 0;       // a key the editor took (the Esc that released the capture)
            bool captured       = false;   // the game holds the mouse: hidden, clipped to the picture
        };
        // This frame's gate. On the Game route the game reads input only while the active panel has focus or the
        // capture; it is captured while the game asks (@p gameWantsCapture) and that panel has focus, until
        // Esc (or Shift+F1, see EscReleasesCapture) releases it - then only a click on the picture, or a new
        // ask, captures again. @p escPressed / @p releaseChordPressed: the window keyboard's edges this frame,
        // seen before the game's snapshot. The other routes: the Scene showing the editor camera gives the
        // game nothing; the single-view editor gives it everything, as before.
        InputGate UpdateInputGate(bool gameWantsCapture, bool escPressed, bool releaseChordPressed);
        bool Captured() const { return m_captured; }
        // The game reads the keyboard this frame (the Game route with focus or the capture): the editor keeps
        // only its Ctrl / Alt chords.
        bool GameHasKeyboard() const { return m_simulating && m_route == MainRoute::Game && m_gateHostFocus; }
        // Esc releases the capture before the game sees it (the default; editor_prefs
        // gameView.escReleasesCapture). Off, Esc is always the game's and Shift+F1 releases.
        bool EscReleasesCapture() const { return m_escReleasesCapture; }
        void SetEscReleasesCapture(bool on) { m_escReleasesCapture = on; }
        // Test seam: answer "the game asks for a capture" without a game flow (1 / 0; -1 = ask the flow).
        void SetCaptureRequestOverride(int answer) { m_captureOverride = answer; }

        // ---- Mouse over the active panel's picture (last frame's UI) ----
        bool PictureHovered() const { return Focal().pictureHovered; }
        // The window's pointer is on the picture and the panel is the window under it: whose pointer the game's
        // Canvas gets. The window's pointer, not ImGui's hover - the Canvas reads that one, and a posted window
        // message (Test.Window) moves it without ever giving ImGui a hover.
        bool PointerOnPicture() const { return Focal().pointerOnPicture; }
        // Another ImGui widget owns the mouse (a slider dragged over the picture).
        bool OtherItemActive() const { return Focal().otherItemActive; }
        bool PictureRightDragging() const { return Focal().rightDragging; }
        float PictureMouseDX() const { return Focal().mouseDX; }
        float PictureMouseDY() const { return Focal().mouseDY; }

        // ---- Docking (SetupDockSpace, before any window of the frame is submitted) ----
        void RequestDock(Dock dock, int panel = 0) { m_panels[Clamp(panel)].dockRequest = dock; }
        Dock ConsumeDockRequest(int panel);

        // ---- Focus ----
        // Gives a Game View or the Scene viewport keyboard focus in the next frame's UI (inside an ImGui frame: a
        // request from Lua or the frame boundary arrives between frames).
        enum class Focus : std::uint8_t { None, Game, Scene };
        void RequestFocus(Focus focus, int panel = 0);

        // A panel's window, its own Begin / End; the registry calls it while open.
        void Draw(const DrawInputs& in, int panel = 0);

        // ---- Frame boundary and shutdown ----
        // Hands back the views nothing showed in the last frame, the panels' at the Play edge (the main view takes
        // one of them over) and the Scene's at the Stop edge.
        void ProcessPendingActions(Renderer* renderer, bool simulating);
        void ReleaseViewsAtShutdown(Renderer* renderer);

    private:
        // A view size that commits once it has rested: a view's resize recreates its HDR target, its graph
        // textures and its passes' sized data, so a dock drag must not resize it every frame. The first size
        // commits at once.
        struct RestingSize
        {
            std::uint32_t w = 0, h = 0;
            std::uint32_t pendW = 0, pendH = 0;
            double        pendAt = 0.0;

            void Offer(std::uint32_t wantW, std::uint32_t wantH, double now);
            void Reset() { w = h = pendW = pendH = 0; }
        };

        // One Game View window.
        struct Panel
        {
            explicit Panel(const char* viewName) : view(viewName) {}

            bool                   open        = false;
            int                    preset      = 0;
            GameViewLayout::Extent extent{};
            GameViewLayout::Rect   image{};
            Dock                   dockRequest = Dock::None;
            bool                   focusAsked  = false;
            Entity                 cameraPick  = NullEntity;   // NullEntity = the game camera
            CameraStatus           camera;                     // what it renders through this frame
            PanelView              view;
            RestingSize            viewSize;
            // The mouse over its picture (last frame's UI).
            bool                   pictureHovered   = false;
            bool                   pointerOnPicture = false;
            bool                   otherItemActive  = false;
            bool                   rightDragging    = false;
            bool                   pictureClicked   = false;   // a press on the picture since the last gate
            float                  mouseDX          = 0.f;
            float                  mouseDY          = 0.f;
        };

        GameView() = default;

        static bool Valid(int panel) { return panel >= 0 && panel < kMaxPanels; }
        static int  Clamp(int panel) { return Valid(panel) ? panel : 0; }
        // The active panel, or the first while none is.
        const Panel& Focal() const { return m_panels[m_active >= 0 ? m_active : 0]; }
        // @p panel can be the main view's on the Game route: open, shown, sized, looking through the game camera.
        bool Candidate(int panel) const;
        void DrawToolbar(const DrawInputs& in, int panel);
        void DrawPicture(const DrawInputs& in, int panel, float boxX, float boxY, float boxW, float boxH);
        // A Scene focus request, served inside the frame after a Game View's own window.
        void FocusSceneIfAsked();

        // ---- State ----
        Panel m_panels[kMaxPanels] = { Panel("game preview"), Panel("game view 2"), Panel("game view 3") };
        int   m_active     = -1;
        bool  m_focusScene = false;

        // ---- Route ----
        MainRoute m_route      = MainRoute::SceneEditor;
        bool      m_simulating = false;
        // Play focused a Game View: Stop gives the focus back to the Scene viewport (if that panel still has it).
        int       m_focusedForPlay = -1;
        // The play state the last frame boundary saw (its Play / Stop edges).
        bool      m_boundarySimulating = false;

        // ---- Game camera ----
        CameraStatus m_camera;
        std::string  m_lastCameraName;   // the last camera Play had, to say which one went away
        // Consecutive Play frames without one: the stack is rebuilt a frame after Play starts, so one miss is
        // not yet a missing camera.
        int          m_cameraMissFrames = 0;

        // ---- Game input and the capture ----
        bool m_captured           = false;
        bool m_escReleased        = false;   // the user took the capture back: wait for a click or a new ask
        bool m_wantedLastFrame    = false;
        bool m_gateHostFocus      = true;
        bool m_escReleasesCapture = true;
        int  m_captureOverride    = -1;

        // ---- The Scene viewport's view ----
        PanelView   m_sceneAux{ "scene" };
        RestingSize m_auxSize;
    };
}
