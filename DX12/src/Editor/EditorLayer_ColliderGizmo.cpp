// A Collider edited in the main viewport (Edit Collider), fitted to its entity's
// geometry (Fit to Bounds), and Create > Collision.
//
// The collider's frame is the one PhysicsSystem::MakeShape builds (and
// CollisionMeshBaker's wireframe mirrors) on a body posed at the entity's
// scale-stripped world pose:
//     world(p) = T_e( R_e( S_e * offset + R_shape * (S_e * p) ) )
// so the gizmo's matrix for a UNIT primitive (a box's +-1 cube, a sphere's unit
// ball, a capsule's unit radius / half height) is
//     dims x S_e x R_shape x T(S_e * offset) x R_e x T_e
// A drag's result goes back through (R_e x T_e)^-1 and a decomposition:
// offset = t / S_e, R_shape = q (Euler degrees in the engine's RollPitchYaw
// order), dims = s / |S_e|. An entity mirrored by a negative scale keeps the
// transform gizmo: the decomposition cannot tell its reflection from a turn.

#include "Editor/EditorLayer.h"
#include "Editor/Undo/ComponentBlob.h"
#include "Editor/Undo/ComponentEditAction.h"

#include "ECS/HierarchyComponents.h"      // LocalAabb, Parent / Children
#include "ECS/PhysicsComponents.h"
#include "ECS/StaticMeshComponents.h"     // MeshSectionOf (a Static Mesh's derived sections)
#include "Math/MathUtils.h"               // RollPitchYawDegreesFromQuaternion
#include "Physics/CollisionFit.h"
#include "Resource/ComponentSerializers.h"
#include "System/Log.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"          // ImGui::GetCurrentWindow (ImGuizmo's hover window)
#include "imgui/ImGuizmo/ImGuizmo.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <memory>
#include <typeindex>

using namespace DirectX;

namespace
{
    constexpr int kColliderGizmoId = 0x45434C47;   // 'ECLG' (ImGuizmo::PushID)

    struct EntityPose
    {
        XMVECTOR s = XMVectorSplatOne(), q = XMQuaternionIdentity(), t = XMVectorZero();
        bool     ok = false;
        bool     mirrored = false;
    };
    // ReadEntityWorldPose's decomposition of the world matrix.
    EntityPose PoseOf(const GlobalTransform& gt)
    {
        EntityPose p;
        const XMMATRIX m = XMLoadFloat4x4(&gt.matrix);
        p.mirrored = XMVectorGetX(XMMatrixDeterminant(m)) < 0.f;
        p.ok       = XMMatrixDecompose(&p.s, &p.q, &p.t, m);
        return p;
    }

    bool HasSize(ColliderComponent::Shape s)
    {
        return s == ColliderComponent::Shape::Box || s == ColliderComponent::Shape::Sphere ||
               s == ColliderComponent::Shape::Capsule;
    }

    // The unit primitive -> the shape.
    XMFLOAT3 Dims(const ColliderComponent& c)
    {
        switch (c.shape)
        {
        case ColliderComponent::Shape::Box:     return c.halfExtents;
        case ColliderComponent::Shape::Sphere:  return { c.radius, c.radius, c.radius };
        case ColliderComponent::Shape::Capsule: return { c.radius, c.halfHeight, c.radius };
        default:                                return { 1.f, 1.f, 1.f };
        }
    }

    XMVECTOR ShapeRotation(const ColliderComponent& c)
    {
        return XMQuaternionRotationRollPitchYaw(XMConvertToRadians(c.rotationEulerDeg.x),
                                                XMConvertToRadians(c.rotationEulerDeg.y),
                                                XMConvertToRadians(c.rotationEulerDeg.z));
    }

    // dims x S_e x R_shape x T(S_e * offset) x R_e x T_e (see the file comment);
    // withDims = false leaves the unit primitive's scale out (the outline draws
    // the real radii itself).
    XMMATRIX ColliderFrame(const ColliderComponent& c, const EntityPose& e, bool withDims = true)
    {
        XMFLOAT3 s;
        XMStoreFloat3(&s, e.s);
        const XMFLOAT3 d = withDims ? Dims(c) : XMFLOAT3{ 1.f, 1.f, 1.f };
        return XMMatrixScaling(d.x, d.y, d.z) * XMMatrixScalingFromVector(e.s) *
               XMMatrixRotationQuaternion(ShapeRotation(c)) *
               XMMatrixTranslation(c.offset.x * s.x, c.offset.y * s.y, c.offset.z * s.z) *
               XMMatrixRotationQuaternion(e.q) * XMMatrixTranslationFromVector(e.t);
    }

    std::string ColliderBlob(World& w, Entity e)
    {
        return Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), w, e,
                                                    std::type_index(typeid(ColliderComponent)));
    }
}

void EditorLayer::SetEditCollider(bool on)
{
    if (m_editCollider == on) return;
    if (!on) CommitColliderDrag();
    m_editCollider = on;
}

// Write a gizmo result (the collider frame in world space) back into the
// component. No undo step here: the drag / the scripted call bracket it.
static bool ApplyColliderFrame(World& world, Entity e, EditorLayer::ColliderGizmoOp op, const XMFLOAT4X4& frame)
{
    ColliderComponent*     c  = world.GetComponent<ColliderComponent>(e);
    const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
    if (!c || !gt) return false;
    const EntityPose pose = PoseOf(*gt);
    if (!pose.ok || pose.mirrored) return false;
    XMVECTOR det;
    const XMMATRIX body  = XMMatrixRotationQuaternion(pose.q) * XMMatrixTranslationFromVector(pose.t);
    const XMMATRIX local = XMLoadFloat4x4(&frame) * XMMatrixInverse(&det, body);
    XMVECTOR s, q, t;
    if (!XMMatrixDecompose(&s, &q, &t, local)) return false;
    XMFLOAT3 S, T, Se;
    XMStoreFloat3(&S, s);
    XMStoreFloat3(&T, t);
    XMStoreFloat3(&Se, pose.s);
    if (std::fabs(Se.x) < 1e-6f || std::fabs(Se.y) < 1e-6f || std::fabs(Se.z) < 1e-6f) return false;
    if (!std::isfinite(S.x + S.y + S.z + T.x + T.y + T.z)) return false;
    const XMFLOAT3 offset = { T.x / Se.x, T.y / Se.y, T.z / Se.z };
    constexpr float kMin = Physics::CollisionFit::kMinHalfThickness;
    switch (op)
    {
    case EditorLayer::ColliderGizmoOp::Translate:
        c->offset = offset;
        break;
    case EditorLayer::ColliderGizmoOp::Rotate:
    {
        XMFLOAT4 qf;
        XMStoreFloat4(&qf, XMQuaternionNormalize(q));
        c->offset           = offset;
        c->rotationEulerDeg = MathUtils::RollPitchYawDegreesFromQuaternion(qf);
        break;
    }
    case EditorLayer::ColliderGizmoOp::Scale:
    {
        if (!HasSize(c->shape)) return false;
        const XMFLOAT3 d = { S.x / std::fabs(Se.x), S.y / std::fabs(Se.y), S.z / std::fabs(Se.z) };
        if (c->shape == ColliderComponent::Shape::Box)
            c->halfExtents = { (std::max)(d.x, kMin), (std::max)(d.y, kMin), (std::max)(d.z, kMin) };
        else if (c->shape == ColliderComponent::Shape::Sphere)
        {
            float r = d.x;   // one handle moves one axis: the one that moved furthest is the radius
            for (const float v : { d.y, d.z })
                if (std::fabs(v - c->radius) > std::fabs(r - c->radius)) r = v;
            c->radius = (std::max)(r, kMin);
        }
        else
        {
            c->radius     = (std::max)(std::fabs(d.x - c->radius) >= std::fabs(d.z - c->radius) ? d.x : d.z, kMin);
            c->halfHeight = (std::max)(d.y, kMin);
        }
        c->offset = offset;
        break;
    }
    }
    c->MarkDirty();
    return true;
}

void EditorLayer::CommitColliderDrag()
{
    const Entity e = m_colliderDragEntity;
    if (e != NullEntity && !m_colliderDragBefore.empty() && m_world && m_world->IsAlive(e) && UndoEnabled())
    {
        const std::string after = ColliderBlob(*m_world, e);
        if (!after.empty() && after != m_colliderDragBefore)
        {
            PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
                m_world->MakeHandle(e), std::type_index(typeid(ColliderComponent)), m_colliderDragBefore, after,
                "Collider"));
            ++m_colliderGizmoGestures;
        }
    }
    m_colliderDragEntity = NullEntity;
    m_colliderDragBefore.clear();
}

bool EditorLayer::RenderColliderGizmo(ImVec2 vpMin, ImVec2 vpMax)
{
    const Entity sel     = m_selectedEntity;
    const bool   stopped = GetPlayState() == ViewportPlayState::Stopped;
    // A drag whose target went away (the toggle, the selection, Play) is committed.
    if (m_colliderDragEntity != NullEntity && (!m_editCollider || !stopped || sel != m_colliderDragEntity))
        CommitColliderDrag();
    m_colliderGizmoOver = false;
    if (!m_editCollider || !stopped || !m_world || sel == NullEntity || !m_world->IsAlive(sel))
    {
        m_colliderGizmoUsing = false;
        return false;
    }
    ColliderComponent*     c  = m_world->GetComponent<ColliderComponent>(sel);
    const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(sel);
    if (!c || !gt) return false;
    const EntityPose pose = PoseOf(*gt);
    if (!pose.ok || pose.mirrored) return false;   // the transform gizmo stays

    ColliderGizmoOp op = m_gizmoOperation == ImGuizmo::ROTATE ? ColliderGizmoOp::Rotate
                       : m_gizmoOperation == ImGuizmo::SCALE  ? ColliderGizmoOp::Scale : ColliderGizmoOp::Translate;
    if (op == ColliderGizmoOp::Scale && !HasSize(c->shape)) op = ColliderGizmoOp::Translate;
    if (op == ColliderGizmoOp::Rotate && c->shape == ColliderComponent::Shape::Sphere) op = ColliderGizmoOp::Translate;

    const XMMATRIX view = XMLoadFloat4x4(&m_renderView.viewMatrix);
    const XMMATRIX proj = XMLoadFloat4x4(&m_renderView.projMatrixNoJitter);
    const XMMATRIX vp   = XMMatrixMultiply(view, proj);
    const float    w = vpMax.x - vpMin.x, h = vpMax.y - vpMin.y;
    auto project = [&](XMVECTOR p, ImVec2& out) -> bool
    {
        const XMVECTOR c4 = XMVector4Transform(XMVectorSetW(p, 1.f), vp);
        const float cw = XMVectorGetW(c4);
        if (cw <= 1e-5f) return false;
        out = ImVec2(vpMin.x + (XMVectorGetX(c4) / cw * 0.5f + 0.5f) * w,
                     vpMin.y + (0.5f - XMVectorGetY(c4) / cw * 0.5f) * h);
        return true;
    };

    // ---- The collider, outlined (the overlay may be off while it is edited) ----
    const XMMATRIX frame = ColliderFrame(*c, pose, /*withDims=*/false);
    ImVec2 centre(0.f, 0.f);
    project(XMVector3TransformCoord(XMVectorZero(), frame), centre);
    ImDrawList* dl = ViewportGizmoDrawList("collider", centre);
    dl->PushClipRect(vpMin, vpMax, true);
    {
        const ImU32 col = IM_COL32(255, 255, 255, 220);
        auto seg = [&](float ax, float ay, float az, float bx, float by, float bz)
        {
            ImVec2 a, b;
            if (project(XMVector3TransformCoord(XMVectorSet(ax, ay, az, 1.f), frame), a) &&
                project(XMVector3TransformCoord(XMVectorSet(bx, by, bz, 1.f), frame), b))
                dl->AddLine(a, b, col, 1.5f);
        };
        constexpr float kTau = 6.28318531f;
        if (c->shape == ColliderComponent::Shape::Box)
        {
            const float he[3] = { c->halfExtents.x, c->halfExtents.y, c->halfExtents.z };
            for (int e = 0; e < 12; ++e)
            {
                const int axis = e / 4, k = e % 4, o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
                float a[3], b[3];
                a[axis] = -he[axis];
                b[axis] = he[axis];
                a[o1] = b[o1] = (k & 1) ? he[o1] : -he[o1];
                a[o2] = b[o2] = (k & 2) ? he[o2] : -he[o2];
                seg(a[0], a[1], a[2], b[0], b[1], b[2]);
            }
        }
        else if (c->shape == ColliderComponent::Shape::Sphere)
        {
            const float r = c->radius;
            for (int k = 0; k < 32; ++k)
            {
                const float a0 = kTau * k / 32, a1 = kTau * (k + 1) / 32;
                const float c0 = r * std::cos(a0), s0 = r * std::sin(a0), c1 = r * std::cos(a1), s1 = r * std::sin(a1);
                seg(c0, s0, 0.f, c1, s1, 0.f);
                seg(c0, 0.f, s0, c1, 0.f, s1);
                seg(0.f, c0, s0, 0.f, c1, s1);
            }
        }
        else if (c->shape == ColliderComponent::Shape::Capsule)
        {
            const float r = c->radius, hh = c->halfHeight;
            for (int k = 0; k < 24; ++k)
            {
                const float a0 = kTau * k / 24, a1 = kTau * (k + 1) / 24;
                for (const float y : { -hh, hh })
                    seg(r * std::cos(a0), y, r * std::sin(a0), r * std::cos(a1), y, r * std::sin(a1));
            }
            for (int k = 0; k < 4; ++k)
            {
                const float a = 0.25f * kTau * k;
                seg(r * std::cos(a), -hh, r * std::sin(a), r * std::cos(a), hh, r * std::sin(a));
            }
            for (int k = 0; k < 12; ++k)
            {
                const float a0 = 0.5f * kTau * k / 12, a1 = 0.5f * kTau * (k + 1) / 12;
                const float c0 = r * std::cos(a0), s0 = r * std::sin(a0), c1 = r * std::cos(a1), s1 = r * std::sin(a1);
                seg(c0, hh + s0, 0.f, c1, hh + s1, 0.f);
                seg(0.f, hh + s0, c0, 0.f, hh + s1, c1);
                seg(c0, -hh - s0, 0.f, c1, -hh - s1, 0.f);
                seg(0.f, -hh - s0, c0, 0.f, -hh - s1, c1);
            }
        }
    }
    dl->PopClipRect();

    // ---- Gizmo ----
    // Freeze the fed matrix while hovered / held (the transform gizmo's rule).
    XMFLOAT4X4 live;
    XMStoreFloat4x4(&live, ColliderFrame(*c, pose));
    if (m_colliderGizmoEntity != sel || (!m_colliderGizmoUsing && !m_colliderGizmoOver))
    {
        m_colliderGizmoEntity = sel;
        m_colliderGizmoMatrix = live;
    }
    float matrix[16];
    std::memcpy(matrix, &m_colliderGizmoMatrix, sizeof(matrix));
    float snapVals[3] = { 0.f, 0.f, 0.f };
    const float* snap = nullptr;
    if (m_snapEnabled != ImGui::GetIO().KeyCtrl && op != ColliderGizmoOp::Scale)
    {
        if (op == ColliderGizmoOp::Rotate) snapVals[0] = m_snapRotate;
        else snapVals[0] = snapVals[1] = snapVals[2] = m_snapTranslate;
        snap = snapVals;
    }
    static const float kUnitBounds[6] = { -1.f, -1.f, -1.f, 1.f, 1.f, 1.f };
    const bool bounds = op == ColliderGizmoOp::Scale && c->shape == ColliderComponent::Shape::Box;
    const ImGuizmo::OPERATION gop = op == ColliderGizmoOp::Translate ? ImGuizmo::TRANSLATE
                                  : op == ColliderGizmoOp::Rotate    ? ImGuizmo::ROTATE
                                  : bounds                           ? ImGuizmo::BOUNDS : ImGuizmo::SCALE;
    ImGuizmo::SetOrthographic(m_editorViewMode != EditorViewMode::Perspective);
    ImGuizmo::SetRect(vpMin.x, vpMin.y, w, h);
    ImGuizmo::SetDrawlist(dl);
    ImGuizmo::SetAlternativeWindow(ImGui::GetCurrentWindow());
    ImGuizmo::PushID(kColliderGizmoId);
    dl->PushClipRect(vpMin, vpMax, true);
    const bool manipulated = ImGuizmo::Manipulate(reinterpret_cast<const float*>(&m_renderView.viewMatrix),
                                                  reinterpret_cast<const float*>(&m_renderView.projMatrixNoJitter),
                                                  gop, op == ColliderGizmoOp::Translate ? m_gizmoMode : ImGuizmo::LOCAL,
                                                  matrix, nullptr, snap, bounds ? kUnitBounds : nullptr);
    dl->PopClipRect();
    const bool usingNow = ImGuizmo::IsUsing();
    m_colliderGizmoOver = ImGuizmo::IsOver() || usingNow;
    ImGuizmo::PopID();
    m_gizmoSubmittedThisFrame = true;   // picking asks GizmoIsOver / GizmoIsUsingAny

    if (manipulated && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (m_colliderDragEntity == NullEntity)
        {
            m_colliderDragEntity = sel;
            m_colliderDragBefore = ColliderBlob(*m_world, sel);
        }
        XMFLOAT4X4 result;
        std::memcpy(&result, matrix, sizeof(result));
        if (ApplyColliderFrame(*m_world, sel, op, result)) m_colliderGizmoMatrix = result;
    }
    if (!usingNow && m_colliderGizmoUsing) CommitColliderDrag();
    m_colliderGizmoUsing = usingNow;
    return true;
}

bool EditorLayer::ColliderGizmoDrag(Entity e, ColliderGizmoOp op, const XMFLOAT3& v, float deg, std::string& why)
{
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) { why = "no such entity"; return false; }
    if (GetPlayState() != ViewportPlayState::Stopped) { why = "stop Play first"; return false; }
    ColliderComponent*     c  = m_world->GetComponent<ColliderComponent>(e);
    const GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(e);
    if (!c || !gt) { why = "the entity has no Collider"; return false; }
    const EntityPose pose = PoseOf(*gt);
    if (!pose.ok) { why = "the entity's transform does not decompose"; return false; }
    if (pose.mirrored) { why = "a mirrored entity (a negative scale) keeps the transform gizmo"; return false; }
    if (op == ColliderGizmoOp::Scale && !HasSize(c->shape)) { why = "only a box, sphere or capsule has a size"; return false; }
    if (op == ColliderGizmoOp::Rotate && c->shape == ColliderComponent::Shape::Sphere)
    {
        why = "a sphere has no orientation";
        return false;
    }
    if (!std::isfinite(v.x + v.y + v.z + deg)) { why = "a value is not a number"; return false; }

    XMMATRIX G = ColliderFrame(*c, pose);
    if (op == ColliderGizmoOp::Translate)
        G = XMMatrixMultiply(G, XMMatrixTranslation(v.x, v.y, v.z));   // world units, world axes
    else
    {
        // ImGuizmo's local rotation / scale: the unit primitive's scale kept apart.
        const XMFLOAT3 d = Dims(*c);
        XMFLOAT3 se;
        XMStoreFloat3(&se, pose.s);
        XMMATRIX local = XMMatrixScaling(d.x, d.y, d.z) * XMMatrixScalingFromVector(pose.s);
        if (op == ColliderGizmoOp::Rotate)
        {
            const XMVECTOR axis = XMVector3Normalize(XMVectorSet(v.x, v.y, v.z, 0.f));
            if (XMVector3Equal(axis, XMVectorZero()) || XMVector3IsNaN(axis)) { why = "the axis is zero"; return false; }
            local = local * XMMatrixRotationAxis(axis, XMConvertToRadians(deg));
        }
        else
        {
            if (!(v.x > 0.f && v.y > 0.f && v.z > 0.f)) { why = "scale factors must be positive"; return false; }
            local = local * XMMatrixScaling(v.x, v.y, v.z);
        }
        G = local * XMMatrixRotationQuaternion(ShapeRotation(*c)) *
            XMMatrixTranslation(c->offset.x * se.x, c->offset.y * se.y, c->offset.z * se.z) *
            XMMatrixRotationQuaternion(pose.q) * XMMatrixTranslationFromVector(pose.t);
    }
    XMFLOAT4X4 frame;
    XMStoreFloat4x4(&frame, G);
    const std::string before = ColliderBlob(*m_world, e);
    if (!ApplyColliderFrame(*m_world, e, op, frame)) { why = "the result is not a valid collider"; return false; }
    const std::string after = ColliderBlob(*m_world, e);
    if (UndoEnabled() && !before.empty() && after != before)
        PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
            m_world->MakeHandle(e), std::type_index(typeid(ColliderComponent)), before, after, "Collider"));
    ++m_colliderGizmoGestures;
    return true;
}

bool EditorLayer::FitColliderToBounds(Entity e, ColliderComponent& c, std::string& why)
{
    if (!m_world || e == NullEntity || !m_world->IsAlive(e)) { why = "no such entity"; return false; }
    if (!HasSize(c.shape)) { why = "only a box, sphere or capsule fits to bounds"; return false; }
    // The entity's own geometry, in its local space: its LocalAabb, else its
    // Static Mesh's derived sections' (children at the owner's transform).
    float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    bool any = false;
    auto add = [&](const LocalAabb& b)
    {
        const float mn[3] = { b.min.x, b.min.y, b.min.z }, mx[3] = { b.max.x, b.max.y, b.max.z };
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = (std::min)(lo[k], mn[k]);
            hi[k] = (std::max)(hi[k], mx[k]);
        }
        any = true;
    };
    if (const LocalAabb* b = m_world->GetComponent<LocalAabb>(e)) add(*b);
    else if (const Children* ch = m_world->GetComponent<Children>(e))
        for (Entity s : ch->entities)
        {
            const MeshSectionOf* so = m_world->IsAlive(s) ? m_world->GetComponent<MeshSectionOf>(s) : nullptr;
            if (!so || so->owner != e) continue;
            if (const LocalAabb* b = m_world->GetComponent<LocalAabb>(s)) add(*b);
        }
    if (!any || !(hi[0] >= lo[0] && hi[1] >= lo[1] && hi[2] >= lo[2]))
    {
        why = "the entity has no mesh bounds to fit to";
        return false;
    }
    Physics::CollisionFit::Points corners;
    for (int k = 0; k < 8; ++k)
        corners.push_back({ (k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1], (k & 4) ? hi[2] : lo[2] });
    Resource::CollisionPrim prim;
    const bool ok = c.shape == ColliderComponent::Shape::Box    ? Physics::CollisionFit::FitBox(corners, prim)
                  : c.shape == ColliderComponent::Shape::Sphere ? Physics::CollisionFit::FitSphere(corners, prim)
                                                                : Physics::CollisionFit::FitCapsule(corners, prim);
    if (!ok) { why = "the fit failed"; return false; }
    c.offset           = prim.center;
    c.rotationEulerDeg = MathUtils::RollPitchYawDegreesFromQuaternion(prim.rotation);
    if (c.shape == ColliderComponent::Shape::Box) c.halfExtents = prim.halfExtents;
    else c.radius = prim.radius;
    if (c.shape == ColliderComponent::Shape::Capsule) c.halfHeight = prim.halfHeight;
    c.MarkDirty();
    return true;
}

Entity EditorLayer::CreateCollisionEntity(ColliderComponent::Shape shape, Entity parent)
{
    if (!m_world || !HasSize(shape)) return NullEntity;
    if (parent != NullEntity && !m_world->IsAlive(parent)) parent = NullEntity;
    const char* name = shape == ColliderComponent::Shape::Box    ? "Box Collision"
                     : shape == ColliderComponent::Shape::Sphere ? "Sphere Collision" : "Capsule Collision";
    const Entity e = m_world->CreateEntity();
    m_world->SetName(e, name);
    LocalTransform lt{};
    if (parent == NullEntity) lt.translation = ComputeSpawnPosition();
    m_world->AddComponent<LocalTransform>(e, lt);
    m_world->AddComponent<GlobalTransform>(e, GlobalTransform{});
    ColliderComponent col{};
    col.shape = shape;
    if (shape == ColliderComponent::Shape::Capsule) { col.radius = 0.3f; col.halfHeight = 0.5f; }
    RigidBodyComponent rb{};
    rb.motion = RigidBodyComponent::Motion::Static;
    if (parent != NullEntity)
    {
        m_world->AddComponent<Parent>(e, Parent{ parent });
        if (Children* ch = m_world->GetComponent<Children>(parent)) ch->entities.push_back(e);
        else
        {
            Children c;
            c.entities.push_back(e);
            m_world->AddComponent<Children>(parent, std::move(c));
        }
        // Sized to the parent's geometry (the child sits at the parent's origin),
        // and Kinematic: it follows whatever moves the parent.
        std::string why;
        FitColliderToBounds(parent, col, why);
        rb.motion = RigidBodyComponent::Motion::Kinematic;
        MarkHierarchyDirty();
    }
    m_world->AddComponent<ColliderComponent>(e, col);
    m_world->AddComponent<RigidBodyComponent>(e, rb);
    LOG_INFO("EditorLayer: Create > Collision - '%s' (entity %u) %s", name, static_cast<unsigned>(e),
             parent != NullEntity ? "under the selection, fitted to it, Kinematic" : "Static, in front of the camera");
    return e;
}
