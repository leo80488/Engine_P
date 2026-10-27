#pragma once
// The editor's free-fly camera: a SceneTransientTag entity (never serialized) on
// the VCam stack at very high priority while in Edit mode, parked during Play so
// the scene's cameras take over, its pose cached here so the editor view survives
// scene loads / world clears (the entity does not); and the axis-locked ortho
// views' override of the resolved camera. In Play, while the Scene viewport shows
// it (the Game View's routes), it stays parked on the stack and flies from the
// Scene viewport's own input; the main view renders it only when the Scene
// viewport is the main pipeline's panel.

#include "ECS/ECS.h"          // Entity, World
#include "ECS/EngineMode.h"   // EngineMode

#include <DirectXMath.h>

class EditorLayer;
class Renderer;
struct GameModeContext;

namespace Editor
{
    // The Scene viewport's own fly input for the editor camera in Play: its right drag and the editor's keys
    // (ImGui's), never the game's ActionState.
    struct FlyInput
    {
        bool  active  = false;   // the Scene viewport is right-dragged while it shows the editor camera
        float lookDX  = 0.f;     // pixels
        float lookDY  = 0.f;
        bool  forward = false;
        bool  back    = false;
        bool  left    = false;
        bool  right   = false;
        bool  up      = false;
        bool  down    = false;
        bool  fast    = false;
    };

    class EditorCameraController
    {
    public:
        // App::TickCamera, first: in Edit mode drive the free camera and answer true
        // (the scene camera is left completely untouched); otherwise park its VCam,
        // fly it from @p sceneFly when the Scene viewport observes Play, and answer false.
        bool Tick(World& world, EngineMode mode, const GameModeContext& ctx, float dt, const FlyInput& sceneFly);
        // The loop's camera seam, after the resolved camera reached the renderer: an
        // ortho view (Edit mode) overrides it, and a view switch cuts history.
        void ApplyOrthoView(Renderer& renderer, World& world, EditorLayer& editorLayer,
                            EngineMode mode);
        // The same seam in Play: while @p observe (the Scene viewport is the main pipeline's panel and shows the
        // editor camera) the renderer's camera is the editor camera. Only the renderer's: the game's cameras and
        // every gameplay reader of the Main channel keep the game's.
        void ApplyObserverView(Renderer& renderer, World& world, bool observe);
        // The free camera's entity (NullEntity before the first Edit frame).
        ::Entity CameraEntity() const { return m_editorCamEntity; }

    private:
        void EnsureEditorCamera(World& world);
        void FlyFromScene(World& world, const FlyInput& in, float dt);

        ::Entity          m_editorCamEntity{ NullEntity };
        DirectX::XMFLOAT3 m_editorCamPos{ 4.f, 3.f, 5.f };
        float             m_editorCamYaw = 0.f, m_editorCamPitch = 0.f;
        bool              m_editorCamPoseValid = false;
        // Last editor viewport view mode (0 = perspective, 1.. = ortho axis views),
        // used to fire a temporal cut when the view changes.
        int               m_lastEditorViewMode = 0;
        // The renderer looked through the editor camera in Play last frame (ApplyObserverView): its first
        // frame starts without history.
        bool              m_observedLastFrame = false;
    };
}
