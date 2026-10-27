// ChainPhysicsPreviewPose.cpp - the Chain Physics Preview panel's IN-PICTURE
// REST-POSE GIZMO. "Pose bones in preview" (the stage tree, m_stagePose) draws
// the pickable chain bones over the picture, a click picks one, and an
// ImGuizmo ROTATE ring edits the STAGED rest pose - panel state; the one
// writer syncs it. The main viewport's ring (EditorLayer::RenderBonePoseGizmo)
// never edits the stage: the panel is the one writer of the stage's live
// component.
//
// THE TWO HALVES (the panel's rule):
//   * PPA (TickPoseSnapshot) reads the World: FK through ctx.buildBones (the
//     overlay's own source, so ring, markers and overlay agree), the seed a new
//     entry starts from, the pickable set. TickPoseGizmo owns the GESTURE: its
//     open / close edges auto-pause / resume the stage clock, and while it is
//     open TickStagedParams neither commits a panel-undo step nor syncs (so a
//     drag is ONE step and ONE structural re-init - ImGuizmo never sets an
//     ImGui ActiveId, so ctx.uiEditActive cannot see a gizmo drag).
//   * Draw (DrawPoseGizmo) reads only the snapshot and writes only panel state
//     (m_staged through ApplyPoseDelta, m_stagePose through a pick).
//
// The delta math is Editor::ChainRestPose::ApplyWorldDelta - the SAME function
// the main viewport's gizmo calls - and the harness drives it without a mouse
// (-chainpreview_pose_gizmo), through ApplyPoseDelta, exactly as the ring does.

#include "Editor/ChainPhysicsPreviewWindow.h"
#include "Editor/ChainRestPoseMath.h"

#include "ECS/AnimationComponents.h"        // SkeletonComponent
#include "ECS/AnimationSystem.h"            // LocalPose (the seed of a new entry)
#include "ECS/ECS.h"
#include "Harness/ExitCodes.h"              // Harness::PreconditionFail
#include "Physics/ChainPhysicsSystem.h"
#include "Renderer/Renderer.h"
#include "Resource/SkeletonAsset.h"
#include "System/Check.h"
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"           // ImGui::GetCurrentWindow (ImGuizmo's hover window)
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Editor
{
using namespace DirectX;

namespace
{
    // The panel gizmo's ImGuizmo ID scope - distinct from the main viewport's
    // entity gizmo and its bone gizmo (0x50534531), so no state aliases.
    constexpr int   kPoseGizmoId   = 0x43505047;   // 'CPPG'
    constexpr float kPickRadiusPx  = 12.f;

    const char* AxisName(int axis)
    {
        switch (axis) { case 0: return "x"; case 1: return "y"; case 2: return "z"; default: return "view"; }
    }

    // Project a world point over the DISPLAYED picture rect (size); false when
    // behind the camera.
    bool ProjectTo(const XMMATRIX& vp, const XMFLOAT3& p, const ImVec2& size, ImVec2& out)
    {
        const XMVECTOR c = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.f), vp);
        const float w = XMVectorGetW(c);
        if (!(w > 0.f)) return false;
        out.x = (XMVectorGetX(c) / w * 0.5f + 0.5f) * size.x;
        out.y = (1.f - (XMVectorGetY(c) / w * 0.5f + 0.5f)) * size.y;
        return true;
    }
}

// ==== Harness registration (parsed by ChainPreviewHarness::ApplyBootFlags) ====
bool ChainPhysicsPreviewWindow::HarnessAddPoseGesture(const std::string& bone, int axis, float deg,
                                                      std::uint32_t frames, std::uint64_t editorFrame)
{
    if (bone.empty() || axis < 0 || axis > 3 || !std::isfinite(deg) || std::fabs(deg) > 360.f ||
        frames == 0 || frames > 600 || editorFrame == 0)
        return false;
    HarnessPoseGesture g;
    g.bone = bone; g.axis = axis; g.deg = deg; g.frames = frames; g.at = editorFrame;
    m_harnessPoseGestures.push_back(g);
    std::stable_sort(m_harnessPoseGestures.begin(), m_harnessPoseGestures.end(),
                     [](const HarnessPoseGesture& a, const HarnessPoseGesture& b) { return a.at < b.at; });
    return true;
}

// ==== PPA: the snapshot Draw reads (after TickOverlay - the camera is this frame's) ====
void ChainPhysicsPreviewWindow::TickPoseSnapshot(const ChainPreviewContext& ctx)
{
    m_poseSnap.valid = false;
    m_poseSnap.pickBone.clear();
    m_poseSnap.pickPos.clear();
    if (!m_stagePose.poseMode || !m_stagedValid || m_stageChain == NullEntity || !ctx.buildBones ||
        !ctx.world || !ctx.renderer || !ctx.world->IsHandleValid(m_stageChainH))
        return;
    World&    world    = *ctx.world;
    Renderer& renderer = *ctx.renderer;
    // The state belongs to ONE carrier and ONE skeleton: a bone index of another
    // never indexes this one.
    if (m_stagePose.owner != m_stageChain) m_stagePose.Reset(m_stageChain);
    const SkeletonComponent* skc = world.GetComponent<SkeletonComponent>(m_stageChain);
    SkeletonRegistry& reg = renderer.GetSkeletonRegistry();
    if (!skc || skc->assetIndex == kInvalidSkeletonIndex || skc->assetIndex >= reg.Count()) return;
    const SkeletonAsset& skel = reg.Get(skc->assetIndex);
    if (m_stagePose.skelAsset != 0xFFFFFFFFu && m_stagePose.skelAsset != skc->assetIndex)
        m_stagePose.poseBone = ~0u;
    m_stagePose.skelAsset = skc->assetIndex;
    if (m_stagePose.poseBone != ~0u && m_stagePose.poseBone >= skel.boneCount)
        m_stagePose.poseBone = ~0u;

    std::vector<XMFLOAT4X4> boneToModel;
    XMMATRIX entWorld;
    if (!ctx.buildBones(renderer, world, m_stageChain, boneToModel, entWorld) ||
        boneToModel.size() < skel.boneCount)
        return;

    // the pickable set (the focus group of THIS tree's pose state)
    std::vector<std::uint8_t> pickable;
    ChainRestPose::CollectChainBones(skel, m_staged, m_stagePose.focusGroup, pickable);
    for (std::uint32_t b = 0; b < skel.boneCount && b < pickable.size(); ++b)
    {
        if (!pickable[b]) continue;
        const XMMATRIX bw = XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[b]), entWorld);
        XMFLOAT3 p;
        XMStoreFloat3(&p, bw.r[3]);
        m_poseSnap.pickBone.push_back(b);
        m_poseSnap.pickPos.push_back(p);
    }

    m_poseSnap.skelAsset = skc->assetIndex;
    m_poseSnap.bone      = m_stagePose.poseBone;
    m_poseSnap.boneName.clear();
    if (m_poseSnap.bone != ~0u)
    {
        const std::uint32_t b = m_poseSnap.bone;
        m_poseSnap.boneName = skel.boneNames[b];
        XMStoreFloat4x4(&m_poseSnap.boneWorld,
                        XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[b]), entWorld));
        const std::int32_t par = skel.parentIndex[b];
        XMStoreFloat4x4(&m_poseSnap.parentWorld,
                        (par >= 0 && static_cast<std::uint32_t>(par) < skel.boneCount)
                            ? XMMatrixMultiply(XMLoadFloat4x4(&boneToModel[par]), entWorld)
                            : entWorld);
        // The seed of a NEW entry: the bone's clean base, so creating one never
        // moves anything - EditorLayer::EnsureBoneRestPoseEntry's rule. While
        // the stage SIMULATES its LocalPose holds solver output for exactly
        // these bones, so the modelled rest is used instead of baking a swing.
        bool seeded = false;
        ChainPhysicsSystem* cps = renderer.GetChainPhysicsSystem();
        if (!(cps && cps->IsSimulating(m_stageChain)))
            if (AnimationSystem* anim = renderer.GetAnimationSystem())
                if (const AnimationSystem::LocalPose* lp = anim->GetLocalPose(m_stageChain);
                    lp && b < skc->boneCount)
                {
                    m_poseSnap.seedRot = lp[b].rot;
                    seeded = true;
                }
        if (!seeded)
        {
            XMVECTOR s, q, t;
            m_poseSnap.seedRot = XMFLOAT4(0.f, 0.f, 0.f, 1.f);
            if (XMMatrixDecompose(&s, &q, &t, XMLoadFloat4x4(&skel.restPoseLocal[b])))
                XMStoreFloat4(&m_poseSnap.seedRot, XMQuaternionNormalize(q));
        }
    }
    m_poseSnap.valid = true;
}

// ==== The one edit path (ring + harness). Panel state only. ====
bool ChainPhysicsPreviewWindow::ApplyPoseDelta(const XMFLOAT4X4& deltaWorld)
{
    if (!m_stagedValid || !m_poseSnap.valid || m_poseSnap.bone == ~0u ||
        m_poseSnap.bone != m_stagePose.poseBone || m_poseSnap.boneName.empty())
        return false;
    BoneRestPose* entry = nullptr;
    for (BoneRestPose& r : m_staged.restPose)
        if (m_poseSnap.boneName == r.boneName) { entry = &r; break; }
    if (!entry)
    {
        if (m_staged.restPose.size() >= static_cast<std::size_t>(ChainPhysicsComponent::MAX_REST_POSE_BONES))
        {
            ++m_poseRefusals;
            m_poseStatus = "The authored rest pose is full (" +
                           std::to_string(ChainPhysicsComponent::MAX_REST_POSE_BONES) + " bones).";
            return false;
        }
        BoneRestPose r{};
        std::snprintf(r.boneName, sizeof(r.boneName), "%s", m_poseSnap.boneName.c_str());
        r.rot = m_poseSnap.seedRot;
        m_staged.restPose.push_back(r);
        entry = &m_staged.restPose.back();
        ++m_poseCreated;
    }
    XMFLOAT4 out;
    const ChainRestPose::DeltaOrder order = m_harnessPoseLocalOrder
        ? ChainRestPose::DeltaOrder::BoneLocal : ChainRestPose::DeltaOrder::ParentFrame;
    if (!ChainRestPose::ApplyWorldDelta(entry->rot, m_poseSnap.parentWorld, deltaWorld, order, out))
    {
        ++m_poseRefusals;
        m_poseStatus = "Mirrored parent frame: a reflected frame has no rotation-only delta.";
        return false;
    }
    entry->rot = out;
    // A rest-pose rotation is STRUCTURAL: the sync re-initialises the chains
    // once, when the gesture closes (PoseGestureHeld).
    m_stagedStructural = true;
    ++m_poseEdits;
    m_poseStatus.clear();
    return true;
}

bool ChainPhysicsPreviewWindow::PoseViewProj(const ImVec2& size, XMMATRIX& view, XMMATRIX& proj) const
{
    if (!m_overlayCam.valid) return false;
    // The overlay's aspect: the preview OUTPUT's (the displayed rect is that
    // output fit-scaled uniformly, so the two agree).
    const float ow = m_overlayOutW ? static_cast<float>(m_overlayOutW) : size.x;
    const float oh = m_overlayOutH ? static_cast<float>(m_overlayOutH) : size.y;
    ChainOverlay::ViewAndProj(m_overlayCam, oh > 0.f ? ow / oh : 1.f, false, view, proj);
    return true;
}

// ==== PPA: the gesture (edges, auto-pause) + the harness driver ====
// Before TickStagedParams, so the commit / sync hold sees this frame's state.
void ChainPhysicsPreviewWindow::TickPoseGizmo(const ChainPreviewContext& ctx)
{
    if (m_stageRoot == NullEntity || !m_stagedValid)
    {
        m_poseGestureHarness = false;
        ReleasePoseGesture();   // unlatch ImGuizmo
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    // ---- -chainpreview_pose_gizmo: one gesture at a time ----
    if (m_harnessPoseNext < m_harnessPoseGestures.size() && ctx.world && ctx.renderer)
    {
        HarnessPoseGesture& g = m_harnessPoseGestures[m_harnessPoseNext];
        const unsigned long long frame = static_cast<unsigned long long>(m_tick);
        auto refuse = [&](const char* why)
        {
            LOG_ERROR("[CHAINPREVIEW] -chainpreview_pose_gizmo '%s' at editor frame %llu: %s. "
                      "Refusing to run.", g.bone.c_str(), frame, why);
            m_poseGestureHarness = false;
            g.phase = 4;
            ++m_harnessPoseNext;
            PostQuitMessage(Harness::PreconditionFail);
        };
        switch (g.phase)
        {
        case 0:
        {
            if (m_tick < g.at || m_stageChain == NullEntity) break;
            SkeletonRegistry& reg = ctx.renderer->GetSkeletonRegistry();
            const SkeletonComponent* skc = ctx.world->GetComponent<SkeletonComponent>(m_stageChain);
            if (!skc || skc->assetIndex == kInvalidSkeletonIndex || skc->assetIndex >= reg.Count())
            { refuse("the stage has no skeleton"); break; }
            const SkeletonAsset& sk = reg.Get(skc->assetIndex);
            std::uint32_t idx = ~0u;
            if (g.bone.size() > 1 && g.bone[0] == '#')
            {
                char* end = nullptr;
                const unsigned long v = std::strtoul(g.bone.c_str() + 1, &end, 10);
                if (end && *end == '\0' && v < sk.boneCount) idx = static_cast<std::uint32_t>(v);
            }
            else
                for (std::uint32_t b = 0; b < sk.boneCount; ++b)
                    if (g.bone == sk.boneNames[b]) { idx = b; break; }
            if (idx == ~0u) { refuse("no such bone on the stage's skeleton"); break; }
            // What a row's Select / a click in the picture does.
            if (m_stagePose.owner != m_stageChain) m_stagePose.Reset(m_stageChain);
            m_stagePose.poseMode  = true;
            m_stagePose.poseBone  = idx;
            m_stagePose.skelAsset = skc->assetIndex;
            g.boneIdx  = idx;
            g.waitFrom = m_tick;
            g.phase    = 1;
            LOG_INFO("[CHAINPREVIEW] -chainpreview_pose_gizmo: bone %u '%s' selected (pose mode on) "
                     "at editor frame %llu", idx, sk.boneNames[idx], frame);
            break;
        }
        case 1:
        case 2:
        {
            if (g.phase == 1)
            {
                // the snapshot of THIS bone (taken at the end of a previous PPA)
                if (!(m_poseSnap.valid && m_poseSnap.bone == g.boneIdx))
                {
                    if (m_tick > g.waitFrom + 240u)
                        refuse("no pose snapshot of that bone in 240 editor frames (no preview view?)");
                    break;
                }
                g.created = true;
                g.q0 = m_poseSnap.seedRot;
                for (const BoneRestPose& r : m_staged.restPose)
                    if (m_poseSnap.boneName == r.boneName) { g.q0 = r.rot; g.created = false; break; }
                g.name         = m_poseSnap.boneName;
                g.parent       = m_poseSnap.parentWorld;
                g.commits0     = m_commits;
                g.rebuilds0    = m_rebuilds;
                g.submits0     = m_poseGizmoSubmits;
                g.entries0     = m_staged.restPose.size();
                g.pausedBefore = m_clock.paused;
                g.simulating   = m_simulate;
                g.axisW = g.axis == 0 ? XMFLOAT3(1.f, 0.f, 0.f)
                        : g.axis == 1 ? XMFLOAT3(0.f, 1.f, 0.f)
                        : g.axis == 2 ? XMFLOAT3(0.f, 0.f, 1.f)
                        : m_overlayCam.forward;
                g.phase = 2;
                m_poseGestureHarness = true;   // the gesture OPENS (edges below)
            }
            // one drag frame: the per-frame WORLD delta, through the ring's path
            const float rad = XMConvertToRadians(g.deg) / static_cast<float>(g.frames);
            XMFLOAT4X4 d;
            XMStoreFloat4x4(&d, XMMatrixRotationAxis(XMVector3Normalize(XMLoadFloat3(&g.axisW)), rad));
            if (!ApplyPoseDelta(d)) ++g.refused;
            if (++g.k >= g.frames)
            {
                m_poseGestureHarness = false;   // released: THIS frame's TickStagedParams commits
                g.phase    = 3;
                g.reportAt = m_tick + 2;
            }
            break;
        }
        case 3:
        {
            if (m_tick < g.reportAt) break;
            // The read-back: the COMMITTED staged entry vs the geometric
            // definition of "turn the bone about a WORLD axis" -
            // newLocal = q0 * P * D * P^-1 (row vectors; P = the parent frame
            // at the start, orthonormalized), in quaternion form, independent
            // of ApplyWorldDelta's matrix path.
            XMFLOAT4 qNow = g.q0;
            bool found = false;
            for (const BoneRestPose& r : m_staged.restPose)
                if (g.name == r.boneName) { qNow = r.rot; found = true; break; }
            const XMVECTOR p = XMQuaternionNormalize(XMQuaternionRotationMatrix(
                ChainRestPose::OrthonormalizeRotation(XMLoadFloat4x4(&g.parent))));
            const XMVECTOR dq = XMQuaternionRotationAxis(XMVector3Normalize(XMLoadFloat3(&g.axisW)),
                                                         XMConvertToRadians(g.deg));
            const XMVECTOR local = XMQuaternionMultiply(XMQuaternionMultiply(p, dq),
                                                        XMQuaternionInverse(p));
            XMFLOAT4 qe;
            XMStoreFloat4(&qe, XMQuaternionNormalize(
                XMQuaternionMultiply(XMQuaternionNormalize(XMLoadFloat4(&g.q0)), local)));
            ++m_harnessPoseReports;
            LOG_INFO("[CHAINPREVIEW] pose gizmo %u: bone=%u name='%s' axis=%s deg=%.2f frames=%u "
                     "found=%d created=%d entriesBefore=%u entriesAfter=%u commitsDelta=%llu "
                     "rebuildsDelta=%llu simulating=%d autoPaused=%d pausedBefore=%d pausedAfter=%d "
                     "rotatedDeg=%.3f errDeg=%.4f refused=%u uiSubmits=%llu localOrder=%d "
                     "gestureOff=%d frame=%llu",
                     m_harnessPoseReports, g.boneIdx, g.name.c_str(), AxisName(g.axis),
                     g.deg, g.frames, found ? 1 : 0, g.created ? 1 : 0,
                     static_cast<unsigned>(g.entries0), static_cast<unsigned>(m_staged.restPose.size()),
                     static_cast<unsigned long long>(m_commits - g.commits0),
                     static_cast<unsigned long long>(m_rebuilds - g.rebuilds0),
                     g.simulating ? 1 : 0, m_poseLastAutoPaused ? 1 : 0, g.pausedBefore ? 1 : 0,
                     m_clock.paused ? 1 : 0, ChainRestPose::AngleDeg(g.q0, qNow),
                     ChainRestPose::AngleDeg(qe, qNow), g.refused,
                     static_cast<unsigned long long>(m_poseGizmoSubmits - g.submits0),
                     m_harnessPoseLocalOrder ? 1 : 0, m_harnessPoseGestureOff ? 1 : 0, frame);
            g.phase = 4;
            ++m_harnessPoseNext;
            break;
        }
        default:
            ++m_harnessPoseNext;
            break;
        }
    }
#else
    (void)ctx;
#endif

    // ---- The gesture's edges: auto-pause for the drag, resume after ----
    const bool open = m_poseGestureUi || m_poseGestureHarness;
    if (open && !m_poseGestureWas)
    {
        ++m_poseGestures;
        m_poseLastAutoPaused = false;
        // The stage's bones move while it simulates or a clip plays: the ring would
        // chase them and the parent frame of the delta would move under the drag, so
        // ANY running stage clock is paused. (The reverse arm is the naive shape -
        // nothing held, nothing paused.)
        const bool clipRunning = m_controller == Controller::Clip && m_playing &&
                                 !m_clipPath.empty();
        if (!m_harnessPoseGestureOff && (m_simulate || clipRunning) && !m_clock.paused)
        {
            m_clock.paused       = true;
            m_poseAutoPaused     = true;
            m_poseLastAutoPaused = true;
            LogTransport("pause (pose gizmo)");
        }
    }
    else if (!open && m_poseGestureWas)
    {
        if (m_poseAutoPaused)
        {
            m_poseAutoPaused = false;
            m_clock.paused   = false;
            LogTransport("resume (pose gizmo)");
        }
    }
    m_poseGestureWas = open;
}

// ==== Draw: markers, the ring, the pick ====
// Before ##cpv_orbit (ImGuizmo's patched CanActivate needs HoveredId == 0 &&
// ActiveId == 0 on the click frame).
void ChainPhysicsPreviewWindow::DrawPoseGizmo(const ImVec2& origin, const ImVec2& size)
{
    m_poseGizmoOver = false;
    m_poseDotsDrawn = 0;
    bool submitted = false;
    XMMATRIX view, proj;
    const bool want = m_stagePose.poseMode && EffectiveDisplay() != DisplayMode::CollidersOnly &&
                      m_poseSnap.valid && size.x > 1.f && size.y > 1.f &&
                      PoseViewProj(size, view, proj);
    if (want)
    {
        const XMMATRIX vp = XMMatrixMultiply(view, proj);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 max(origin.x + size.x, origin.y + size.y);
        dl->PushClipRect(origin, max, true);
        // The pickable bones (what a click can pick) and the picked one. They are
        // OVERLAY, so Mesh only hides them (a click still picks the nearest bone);
        // the ring below is the tool and stays in every mode.
        const bool dots = EffectiveDisplay() != DisplayMode::MeshOnly;
        for (std::size_t i = 0; dots && i < m_poseSnap.pickPos.size(); ++i)
        {
            ImVec2 s;
            if (!ProjectTo(vp, m_poseSnap.pickPos[i], size, s)) continue;
            const ImVec2 c(origin.x + s.x, origin.y + s.y);
            ++m_poseDotsDrawn;
            if (m_poseSnap.pickBone[i] == m_stagePose.poseBone)
                dl->AddCircleFilled(c, 6.f, IM_COL32(255, 210, 60, 240), 14);
            else
            {
                dl->AddCircleFilled(c, 3.5f, IM_COL32(20, 20, 20, 170), 10);
                dl->AddCircle(c, 3.5f, IM_COL32(240, 240, 240, 220), 10, 1.2f);
            }
        }

        if (m_poseSnap.bone != ~0u && m_poseSnap.bone == m_stagePose.poseBone)
        {
            // Freeze the fed matrix while hovered / dragged (the main gizmo's
            // rule): a bone matrix re-derived from an animating pose every frame
            // would make the hit test flicker.
            if (m_poseGizmoBone != m_poseSnap.bone)
            {
                m_poseGizmoBone   = m_poseSnap.bone;
                m_poseGizmoMatrix = m_poseSnap.boneWorld;
            }
            else if (!m_poseGestureUi && !m_poseGestureHarness && !ImGuizmo::IsUsingAny())
                m_poseGizmoMatrix = m_poseSnap.boneWorld;

            float worldMut[16], deltaMut[16];
            std::memcpy(worldMut, &m_poseGizmoMatrix, sizeof(worldMut));
            XMFLOAT4X4 viewF, projF;
            XMStoreFloat4x4(&viewF, view);
            XMStoreFloat4x4(&projF, proj);
            // All four, every call (the main viewport's gizmos set them too, and
            // ImGuizmo::BeginFrame resets the draw list): the DISPLAYED rect,
            // THIS child's draw list, THIS window as the hover window.
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);
            ImGuizmo::SetDrawlist(dl);
            ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
            ImGuizmo::PushID(kPoseGizmoId);
            const bool manipulated = ImGuizmo::Manipulate(
                reinterpret_cast<const float*>(&viewF), reinterpret_cast<const float*>(&projF),
                ImGuizmo::ROTATE, ImGuizmo::LOCAL, worldMut, deltaMut, nullptr);
            // ID-SCOPED queries (IsUsing compares the current ID) - inside PushID.
            m_poseGizmoOver = ImGuizmo::IsOver();
            const bool usingNow = ImGuizmo::IsUsing();
            ImGuizmo::PopID();
            submitted = true;
            ++m_poseGizmoSubmits;
            if (manipulated && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
                XMFLOAT4X4 d;
                std::memcpy(&d, deltaMut, sizeof(d));
                if (ApplyPoseDelta(d))
                    std::memcpy(&m_poseGizmoMatrix, worldMut, sizeof(m_poseGizmoMatrix));
            }
            m_poseGestureUi = usingNow;
        }
        dl->PopClipRect();
    }
    if (!submitted && m_poseGestureUi)
    {
        // A drag whose ring vanished (pose mode off, the bone dropped, the
        // picture frozen): never leave ImGuizmo's global "using" latched - the
        // main viewport would read it as a permanently held gizmo.
        ReleasePoseGesture();
    }
}

// EVERY path that ends a ring drag from our side (this Draw, a panel that did
// not draw, a stage teardown, no stage) hands ImGuizmo's global mbUsing /
// editing ID back - otherwise every other gizmo's activation (HandleRotation /
// Translation reach CanActivate only under !mbUsing) stays dead until restart.
// Enable() only writes ImGuizmo's static context, so PPA (serial window, ImGui
// context alive) may call it.
void ChainPhysicsPreviewWindow::ReleasePoseGesture()
{
    if (!m_poseGestureUi) return;
    ImGuizmo::Enable(false);
    ImGuizmo::Enable(true);
    m_poseGestureUi = false;
    ++m_poseGestureReleases;
}

void ChainPhysicsPreviewWindow::PickPoseBone(const ImVec2& local, const ImVec2& size)
{
    XMMATRIX view, proj;
    if (!m_poseSnap.valid || !PoseViewProj(size, view, proj)) return;
    const XMMATRIX vp = XMMatrixMultiply(view, proj);
    std::uint32_t best = ~0u;
    float bestD2 = kPickRadiusPx * kPickRadiusPx;
    for (std::size_t i = 0; i < m_poseSnap.pickPos.size(); ++i)
    {
        ImVec2 s;
        if (!ProjectTo(vp, m_poseSnap.pickPos[i], size, s)) continue;
        const float dx = s.x - local.x, dy = s.y - local.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = m_poseSnap.pickBone[i]; }
    }
    if (best == ~0u) return;
    m_stagePose.poseBone  = best;
    m_stagePose.skelAsset = m_poseSnap.skelAsset;
    ++m_posePicks;
}
}
