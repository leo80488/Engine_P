#include "ECS/CameraLook.h"

#include "ECS/CameraSystem.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Input/InputSystem.h"
#include "Scene/GameModeContext.h"

#include <DirectXMath.h>

// The one place mouse and stick look are converted to radians and summed, so both
// devices steer in the same frame and neither wins:
//   mouse -> PIXELS  x mouseSensitivity         (never dt-scaled)
//   stick -> [-1,1]  x lookRateDegPerSec x dt   (always dt-scaled)
// ActionState carries the two in separately named fields with different units, so
// look speed cannot scale with the frame rate.
void Camera::ApplyCameraLook(CameraControllerComponent& ctrl, LocalTransform& lt,
                             const GameModeContext& ctx, float dt)
{
    const ActionState&   cmd = Input::Get().Cmd();
    const InputBindings& cfg = Input::Get().Bindings();

    // A frame hitch must not snap the camera through a wall.
    const float lookDt  = (dt < 1.f / 30.f) ? dt : (1.f / 30.f);
    const float rateRad = DirectX::XMConvertToRadians(cfg.lookRateDegPerSec);

    // forward.y = -sin(pitch): a NEGATIVE pitch delta looks UP. Screen +Y
    // is down and stick +Y is up, so both get negated by default and each
    // device carries its own invert toggle.
    const float mSignY = cfg.mouseInvertLookY ? +1.f : -1.f;
    const float pSignY = cfg.padInvertLookY   ? +1.f : -1.f;

    const float yawRad   = cmd.lookMouseDX * ctrl.mouseSensitivity
                         + cmd.lookStickX  * rateRad * lookDt;
    const float pitchRad = mSignY * cmd.lookMouseDY * ctrl.mouseSensitivity
                         + pSignY * cmd.lookStickY  * rateRad * lookDt;

    CameraSystem::ApplyLook(ctrl, lt, yawRad, pitchRad);

    // FLY is gated on RMB only, never on cameraLookActive: a resting stick must not arm
    // WASDQE while the editor's W/E/R gizmo shortcuts are live.
    if (ctx.viewportRightMouseHeld && dt > 0.f)
        CameraSystem::ApplyFly(ctrl, lt, dt);
}
