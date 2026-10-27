#pragma once

// ChainRestPoseMath.h - the authored-rest-pose tools' pure math, shared by the
// MAIN viewport's rest-pose gizmo (EditorLayer::RenderBonePoseGizmo, which
// writes a scene character's LIVE ChainPhysicsComponent::restPose), the Chain
// Physics Preview panel's in-picture gizmo (which writes the panel's STAGED
// copy) and the harness that drives the panel's gizmo without a mouse
// (-chainpreview_pose_gizmo): all of them run the SAME delta math. No World,
// no renderer, no ImGui: every input is a value the caller snapshotted.

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

struct ChainPhysicsComponent;
struct SkeletonAsset;

namespace Editor
{
namespace ChainRestPose
{
    // How a WORLD-space gizmo delta becomes a change of the bone's authored
    // LOCAL rotation.
    enum class DeltaOrder : std::uint8_t
    {
        // The engine convention (IKSystem::ComposeIKAnim): base first, the
        // PARENT-frame delta second - the bone turns about the world axis the
        // user dragged.
        ParentFrame,
        // The harness reverse arm (-chainpreview_pose_local_order): the operands
        // swapped, i.e. the delta applied in BONE-local space. Near-identical
        // for small angles, badly wrong for large ones - and nothing else
        // asserts on it.
        BoneLocal,
    };

    // Gram-Schmidt the rotation part of a bone matrix. Bone matrices carry
    // SCALE (both FK build sites multiply in XMMatrixScaling), and conjugating
    // a rotation by a non-uniformly scaled matrix yields SHEAR, which then
    // decomposes to garbage. Translation is dropped.
    DirectX::XMMATRIX OrthonormalizeRotation(DirectX::XMMATRIX m);

    // newLocal = oldLocal * parent * delta * parent^-1 (row vectors), as a
    // normalized quaternion kept in oldLocal's hemisphere (q and -q are the
    // same orientation; a continuous drag must never read as a 360-degree flip
    // downstream). `parentWorld` = the parent bone's world matrix (the entity's
    // for a root bone); its translation is ignored. Returns false - `outLocal`
    // untouched - for a MIRRORED parent frame (determinant <= 0): a reflected
    // frame has no rotation-only delta.
    bool ApplyWorldDelta(const DirectX::XMFLOAT4& oldLocal,
                         const DirectX::XMFLOAT4X4& parentWorld,
                         const DirectX::XMFLOAT4X4& deltaWorld,
                         DeltaOrder order, DirectX::XMFLOAT4& outLocal);

    // Which bones the pose tools may touch: every enabled group's root and
    // (Chain / Cloth) its subtree minus the excludes; focusGroup >= 0 limits
    // it to that group (an out-of-range focus reads as "all groups").
    void CollectChainBones(const SkeletonAsset& skel, const ChainPhysicsComponent& comp,
                           int focusGroup, std::vector<std::uint8_t>& outPickable);

    // The angle between two orientations, degrees (sign-agnostic: q == -q).
    float AngleDeg(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b);
}
}
