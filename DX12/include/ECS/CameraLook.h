#pragma once
// Device-agnostic camera look: mouse displacement and stick rate summed in radians,
// then the RMB fly. The scene's free-look camera (App::TickCamera) and the editor's
// (EditorCameraController) both drive their controller through ApplyCameraLook.

struct CameraControllerComponent;
struct LocalTransform;
struct GameModeContext;

namespace Camera
{
    void ApplyCameraLook(CameraControllerComponent& ctrl, LocalTransform& lt,
                         const GameModeContext& ctx, float dt);
}
