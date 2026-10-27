#include "Editor/EditorCameraController.h"

#include "Editor/EditorLayer.h"
#include "Editor/EditorOrthoViews.h"
#include "ECS/CameraLook.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraStackSystem.h"
#include "ECS/CameraSystem.h"
#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "Renderer/Renderer.h"
#include "Scene/GameModeContext.h"
#include "Sequencer/SequenceEval.h"   // Seq::Preview::OwnsViewport (Camera Lock)
#include "System/Log.h"

#include <cmath>

bool Editor::EditorCameraController::Tick(World& world, EngineMode mode, const GameModeContext& ctx,
                                          float dt, const FlyInput& sceneFly)
{
    // ---- Edit mode: the editor's own transient free camera owns the view ----
    // The scene camera entity is left completely untouched (no pose writes, no
    // component backfill) so editor navigation never pollutes authored scenes.
    // A Game build pins EngineMode::Playing at boot, so it never enters here
    // and App::TickCamera drives the scene camera instead.
    if (mode == EngineMode::Edit)
    {
        EnsureEditorCamera(world);
        auto* ctrl = world.GetComponent<CameraControllerComponent>(m_editorCamEntity);
        auto* lt   = world.GetComponent<LocalTransform>(m_editorCamEntity);
        if (ctrl && lt)
        {
            if (ctx.cameraLookActive)
                Camera::ApplyCameraLook(*ctrl, *lt, ctx, dt);
            else
            {
                // Idle: something other than the controller may own the pose:
                // keep yaw/pitch tracking it so re-grabbing doesn't snap. This
                // is exactly why cameraLookActive must be its own flag: gating
                // on RMB alone would round-trip every stick write away here.
                CameraSystem::SyncControllerFromTransform(*ctrl, *lt);
                if (ctx.viewportRightMouseHeld && dt > 0.f)
                    CameraSystem::ApplyFly(*ctrl, *lt, dt);
            }

            // Root entity: patch GlobalTransform now (pre-Propagate readers).
            if (auto* gt = world.GetComponent<GlobalTransform>(m_editorCamEntity))
                DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());

            // Cache the pose so the view survives world clears (scene loads
            // destroy the transient entity; EnsureEditorCamera recreates it).
            m_editorCamPos       = lt->translation;
            m_editorCamYaw       = ctrl->yaw;
            m_editorCamPitch     = ctrl->pitch;
            m_editorCamPoseValid = true;
        }
        // Main channel must exist for CameraResolve to publish LiveCamera.
        Camera::GetOrCreateChannelEntity(world, Camera::kMainChannel);
        return true;
    }

    // Play/Paused/Stepping: park the editor VCam so gameplay cameras win.
    if (m_editorCamEntity != NullEntity && world.IsAlive(m_editorCamEntity))
        if (auto* p = world.GetComponent<VCamPriorityComponent>(m_editorCamEntity))
            p->enabled = false;
    // Parked, it still flies when the Scene viewport shows it: from the Scene's own input, never the game's.
    // The pose is not cached, so Stop puts the Scene back where it was before Play.
    if (sceneFly.active)
        FlyFromScene(world, sceneFly, dt);
    return false;
}

void Editor::EditorCameraController::FlyFromScene(World& world, const FlyInput& in, float dt)
{
    if (m_editorCamEntity == NullEntity || !world.IsAlive(m_editorCamEntity)) return;
    auto* ctrl = world.GetComponent<CameraControllerComponent>(m_editorCamEntity);
    auto* lt   = world.GetComponent<LocalTransform>(m_editorCamEntity);
    if (!ctrl || !lt) return;

    // Screen +Y is down: negated so pushing the mouse down looks up.
    CameraSystem::ApplyLook(*ctrl, *lt, in.lookDX * ctrl->mouseSensitivity, -in.lookDY * ctrl->mouseSensitivity);
    if (dt > 0.f)
    {
        using namespace DirectX;
        const float speed = ctrl->moveSpeed * dt * (in.fast ? 3.f : 1.f);
        const float cp = std::cos(ctrl->pitch), sp = std::sin(ctrl->pitch);
        const float cy = std::cos(ctrl->yaw), sy = std::sin(ctrl->yaw);
        const XMVECTOR forward = XMVectorSet(sy * cp, -sp, cy * cp, 0.f);
        const XMVECTOR up      = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        const XMVECTOR right   = XMVector3Normalize(XMVector3Cross(up, forward));
        XMVECTOR pos = XMLoadFloat3(&lt->translation);
        if (in.forward) pos = XMVectorAdd(pos, XMVectorScale(forward, speed));
        if (in.back)    pos = XMVectorSubtract(pos, XMVectorScale(forward, speed));
        if (in.right)   pos = XMVectorAdd(pos, XMVectorScale(right, speed));
        if (in.left)    pos = XMVectorSubtract(pos, XMVectorScale(right, speed));
        if (in.up)      pos = XMVectorAdd(pos, XMVectorScale(up, speed));
        if (in.down)    pos = XMVectorSubtract(pos, XMVectorScale(up, speed));
        XMStoreFloat3(&lt->translation, pos);
    }
    // Root entity: patch GlobalTransform now (the secondary view's extract and the observer seam read it).
    if (auto* gt = world.GetComponent<GlobalTransform>(m_editorCamEntity))
        DirectX::XMStoreFloat4x4(&gt->matrix, lt->ToMatrix());
}

void Editor::EditorCameraController::EnsureEditorCamera(World& world)
{
    if (m_editorCamEntity != NullEntity && world.IsAlive(m_editorCamEntity)
        && world.HasComponent<EditorCameraTag>(m_editorCamEntity))
    {
        // Re-arm every Edit frame (Play parks it by clearing enabled) - UNLESS
        // the Sequencer's Camera Lock is on, in which case the shot camera is
        // supposed to own the viewport.
        //
        // Parking the free-cam is the only robust way to do this. Out-bidding
        // it on priority is not: this camera sits at 1'000'000 and would still
        // be enabled and contributing, so any weight the shot camera failed to
        // reach would blend the two together.
        if (auto* p = world.GetComponent<VCamPriorityComponent>(m_editorCamEntity))
            p->enabled = !Seq::Preview::OwnsViewport();
        return;
    }

    // (Re)create: the previous instance died with a world clear / scene load.
    Entity e = world.CreateEntity();
    world.AddComponent<EditorCameraTag>(e, EditorCameraTag{});
    // SceneTransientTag -> SceneSerializer skips this entity entirely, so the
    // editor camera can never leak into saved scenes.
    world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});

    CameraControllerComponent ctrl{};
    ctrl.moveSpeed = 15.f;
    if (m_editorCamPoseValid)
    {
        ctrl.yaw   = m_editorCamYaw;
        ctrl.pitch = m_editorCamPitch;
    }
    else
    {
        // Default view: at (4,3,5) looking back toward the origin.
        ctrl.yaw   = std::atan2(-4.f, -5.f);
        ctrl.pitch = std::asin(3.f / std::sqrt(4.f * 4.f + 3.f * 3.f + 5.f * 5.f));
    }

    LocalTransform lt = CameraSystem::MakeTransform(
        ctrl, m_editorCamPoseValid ? m_editorCamPos : DirectX::XMFLOAT3{ 4.f, 3.f, 5.f });
    world.AddComponent<LocalTransform>(e, lt);
    GlobalTransform gt{};
    DirectX::XMStoreFloat4x4(&gt.matrix, lt.ToMatrix());
    world.AddComponent<GlobalTransform>(e, gt);
    world.AddComponent<CameraControllerComponent>(e, std::move(ctrl));

    // VCam quad: joins the stack via the GlobalTransform -> CameraPose bridge
    // (no behavior tags). Editor lens: wider far plane than the scene default.
    VirtualCameraComponent vc{};
    vc.fov   = DirectX::XM_PI / 3.f;
    vc.nearZ = 0.1f;
    vc.farZ  = 1000.f;
    world.AddComponent<VirtualCameraComponent>(e, vc);
    world.AddComponent<CameraPoseComponent>(e, CameraPoseComponent{});
    VCamPriorityComponent prio{};
    prio.priority = 1'000'000;   // outranks any gameplay VCam while editing
    prio.weight   = 1.f;
    prio.enabled  = true;
    world.AddComponent<VCamPriorityComponent>(e, std::move(prio));
    VCamBlendComponent blend{};
    blend.currentBlend     = 1.f;
    blend.state            = BlendState::Active;
    blend.blendInDuration  = 0.f;
    blend.blendOutDuration = 0.f;
    world.AddComponent<VCamBlendComponent>(e, std::move(blend));

    m_editorCamEntity = e;
    LOG_INFO("App: editor camera entity %u created", e);
}

void Editor::EditorCameraController::ApplyOrthoView(Renderer& renderer, World& world,
                                                    EditorLayer& editorLayer, EngineMode mode)
{
    // Axis-locked ORTHOGRAPHIC editor views (Edit mode only) override the
    // resolved camera at this seam. Play always uses the scene cameras.
    if (mode == EngineMode::Edit)
    {
        const int vm = editorLayer.GetEditorViewMode();
        if (vm != 0)
        {
            const DirectX::XMFLOAT3 focus = editorLayer.GetOrthoFocus();
            const DirectX::XMFLOAT3& f    = OrthoViews::kForward[vm - 1];
            // Eye pulled far back along the view axis: the whole scene
            // around the focus fits between near and far, and shaders that
            // build V = normalize(cameraPos - worldPos) get a near-constant
            // direction (the standard editor-ortho compromise).
            constexpr float kPull = 2000.f;
            RenderCamera rc;
            rc.position = { focus.x - f.x * kPull,
                            focus.y - f.y * kPull,
                            focus.z - f.z * kPull };
            rc.forward         = f;
            rc.up              = OrthoViews::kUp[vm - 1];
            rc.nearZ           = 0.1f;
            rc.farZ            = kPull * 2.f;
            rc.orthographic    = true;
            rc.orthoHalfHeight = editorLayer.GetOrthoHalfHeight();
            rc.historyValid    = (vm == m_lastEditorViewMode);
            renderer.SetCamera(rc);
        }
        if (vm != m_lastEditorViewMode)
        {
            // View switched (persp <-> ortho or between axis views): temporal
            // history was shaded under a different projection - hard cut.
            renderer.InvalidateTemporalHistory();
            Camera::HardCutTo(world, NullEntity, Camera::kMainChannel);
            m_lastEditorViewMode = vm;
        }
    }
}

void Editor::EditorCameraController::ApplyObserverView(Renderer& renderer, World& world, bool observe)
{
    const auto* gt = (observe && m_editorCamEntity != NullEntity && world.IsAlive(m_editorCamEntity))
                         ? world.GetComponent<GlobalTransform>(m_editorCamEntity) : nullptr;
    const auto* vc = gt ? world.GetComponent<VirtualCameraComponent>(m_editorCamEntity) : nullptr;
    if (!gt || !vc)
    {
        m_observedLastFrame = false;
        return;
    }
    const DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&gt->matrix);
    RenderCamera rc;
    DirectX::XMStoreFloat3(&rc.position, m.r[3]);
    DirectX::XMStoreFloat3(&rc.forward, DirectX::XMVector3Normalize(m.r[2]));
    DirectX::XMStoreFloat3(&rc.up, DirectX::XMVector3Normalize(m.r[1]));
    rc.fov          = vc->fov;
    rc.nearZ        = vc->nearZ;
    rc.farZ         = vc->farZ;
    rc.historyValid = m_observedLastFrame;
    renderer.SetCamera(rc);
    m_observedLastFrame = true;
}
