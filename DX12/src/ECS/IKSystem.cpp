#include "ECS/IKSystem.h"
#include "Anim/IK/IKSolvers.h"
#include "System/TaskSystem.h"
using namespace DirectX;
XMMATRIX IKSystem::LocalPoseToMatrix(const AnimationSystem::LocalPose& p)
{ return Anim::IK::LocalMatrix(p); }
XMMATRIX IKSystem::ComputeBoneWorldTransform(uint32_t b,
    const AnimationSystem::LocalPose* p, const SkeletonAsset& s)
{ return Anim::IK::BoneModel(b, p, s); }
void IKSystem::Update(World& world, const std::unordered_set<Entity>* activeSet)
{
    // Editor toggle (IKSystem::SetEnabled). Disabled, skeletons keep the raw pose
    // AnimationSystem::Update wrote: no IK pass, no per-skeleton iteration.
    if (!m_enabled) return;

    struct IKJob {
        AnimationSystem::LocalPose* poses;
        const SkeletonAsset*        asset;
        uint32_t                    boneCount;
        const AnimationComponent* anim;
    };
    std::vector<IKJob> jobs;

    auto* pSkel = world.GetPool<SkeletonComponent>();
    auto* pAnim = world.GetPool<AnimationComponent>();
    const size_t skelN = pSkel ? pSkel->Data().size() : 0;
    const auto&  skelEnts = EntitiesOf(pSkel);
    for (size_t si = 0; si < skelN; ++si)
    {
        const Entity e = skelEnts[si];
        if (activeSet && activeSet->find(e) == activeSet->end()) continue;

        auto* skel = &pSkel->Data()[si];
        if (skel->assetIndex == kInvalidAnimHandle) continue;

        // Skip IK on un-animated skeletons: their rest-pose LocalPose exists (so morphs can deform), but
        // the IK target bones sit at their bind positions and the solver would yank chain links toward
        // them. "Un-animated" means no live pose source, not "no primaryClip": locomotion-only and
        // AnimLayer-only characters still reach the leg-drive check below.
        auto* anim = pAnim ? pAnim->Get(e) : nullptr;
        if (!anim || !m_animSys.HasLivePoseSource(world, e)) continue;

        // Per-clip leg drive (docs/design/PMX_Humanoid_Retarget_Architecture.md): retargeted clips bake FK
        // legs (LegDriveMode::FK, leg IK weight 0), which the MMD CCD must not re-solve; native VMD clips
        // default to IK. A weight below 1 blends the solved rotations with the clip's.
        if (anim->legIKWeight <= 1e-5f) continue;

        AnimationSystem::LocalPose* poses = m_animSys.GetMutableLocalPose(e);
        if (!poses) continue;

        const SkeletonAsset& asset = m_skeletons.Get(skel->assetIndex);
        if (asset.ikChains.empty()) continue;

        jobs.push_back({ poses, &asset, skel->boneCount, anim });
    }

    auto solve = [](IKJob& j) {
        Anim::IK::ModelPose model(j.poses, *j.asset);
        Anim::IK::MmdCcd solver(model);
        const float weight = j.anim->legIKWeight;
        std::vector<Anim::IK::LocalPose> base(j.poses,j.poses+j.boneCount);
        for (uint32_t i=0; i<j.asset->ikChains.size(); ++i) {
            if (!j.anim->IKChainEnabled(i)) continue;
            const auto& chain = j.asset->ikChains[i];
            const auto bone = chain.ikBoneIndex;
            if (bone >= j.boneCount) continue;
            XMVECTOR s,r,t;
            XMMatrixDecompose(&s,&r,&t,XMLoadFloat4x4(&j.asset->restPoseLocal[bone]));
            XMStoreFloat3(&j.poses[bone].pos,t+(XMLoadFloat3(&base[bone].pos)-t)/weight);
            model.RecomputeSubtree(bone);
            solver.SolveChain(chain, j.poses, *j.asset, j.boneCount);
            j.poses[bone].pos=base[bone].pos;
            model.RecomputeSubtree(bone);
        }
        if (weight < 1.f) {
            for (uint32_t b=0; b<j.boneCount; ++b)
                XMStoreFloat4(&j.poses[b].rot,XMQuaternionSlerp(XMLoadFloat4(&base[b].rot),XMLoadFloat4(&j.poses[b].rot),weight));
        }
    };
    // Solve the chains per entity in parallel: each entity's LocalPose array is independent.
    if (jobs.size() > 1)
    {
        TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(jobs.size()),
            [&](uint32_t i)
            {
                auto& j = jobs[i];
                solve(j);
            });
    }
    else if (!jobs.empty())
    {
        auto& j = jobs[0];
        solve(j);
    }
}
