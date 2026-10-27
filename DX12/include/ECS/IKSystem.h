#pragma once

// IKSystem: CCD-IK solver for MMD/PMX models, ported from saba (benikabocha).
//
// Call order each frame (Renderer::TickAnimationChain):
//   1. AnimationSystem::Update()          - samples clips, applies pre-IK grants
//   2. IKSystem::Update()                 - solves IK chains (modifies local poses)
//   3. AnimationSystem::ApplyPostIKGrants - post-IK grants (D-bones)
//   4. LocalToWorldSystem::Update()       - accumulates world-space bone matrices
//
// The solver takes the IK bone's animated position as the target and rotates the chain link bones
// so the effector (target bone) reaches it.

#include "ECS/ECS.h"
#include "ECS/AnimationSystem.h"
#include "Resource/SkeletonAsset.h"

#include <DirectXMath.h>
#include <unordered_set>
#include <vector>

class IKSystem
{
public:
    explicit IKSystem(AnimationSystem& animSys, SkeletonRegistry& skeletons)
        : m_animSys(animSys), m_skeletons(skeletons) {}

    // Solve IK chains for all entities with active animation.
    void Update(World& world, const std::unordered_set<Entity>* activeSet = nullptr);

    // Editor toggle: when false, Update() returns at once and every skeleton keeps its raw animated
    // pose. For A/B-comparing rig authoring, debugging foot slide, or disabling IK on characters whose
    // clips already bake the end-effector trajectory.
    bool IsEnabled() const { return m_enabled; }
    void SetEnabled(bool v) { m_enabled = v; }

    // Pure kinematics helpers (thin wrappers over Anim::IK::LocalMatrix / Anim::IK::BoneModel) for a
    // pass that reads the skeleton's pre-IK pose to find where an effector sits.
    static DirectX::XMMATRIX LocalPoseToMatrix(const AnimationSystem::LocalPose& p);
    static DirectX::XMMATRIX ComputeBoneWorldTransform(
        uint32_t                          boneIndex,
        const AnimationSystem::LocalPose* poses,
        const SkeletonAsset&              skel);

private:
    AnimationSystem&  m_animSys;
    SkeletonRegistry& m_skeletons;
    bool              m_enabled = true;
};
