#pragma once

// FaceShadingSystem - feeds the head bone's world axes into face materials so the
// SDF face-shadow shader (asset/Shaders/Face.ps.hlsl) knows which way the face points.
// Header-only; the single caller is Renderer::TickAnimationChain. Runs AFTER
// LocalToWorldSystem / SocketSystem (PoseRingBuffer holds SKIN matrices and the
// owner's SkeletonComponent.poseByteOffset is valid) and BEFORE BuildRenderScene, so
// the fresh frameParams reach the per-material b8 CBV and the material slot's frame
// record this same frame.
// Bone-world recovery mirrors SocketSystem:
//   modelBone = bindPose[b] * skin[b]      (skin embeds invBind; bindPose cancels it)
//   worldBone = modelBone   * ownerGlobalTransform
// The face MaterialComponent may live on a DIFFERENT entity than the pose (split
// character): the pose + GlobalTransform are read from the SkeletonRef owner and the
// result is written onto the face entity.

#include "System/TaskSystem.h"
#include "ECS/ECS.h"
#include "ECS/FaceShadingComponent.h"
#include "ECS/AnimationComponents.h"      // SkeletonComponent, SkeletonRef, kInvalidAnimHandle
#include "ECS/Components.h"               // MaterialComponent (frameParams)
#include "ECS/HierarchyComponents.h"      // GlobalTransform
#include "Resource/SkeletonAsset.h"       // SkeletonRegistry, SkeletonAsset, kInvalidSkeletonIndex
#include "Graphics/SkinningBuffers.h"     // PoseRingBuffer

#include <DirectXMath.h>
#include <array>                          // frameParams value type (compare-before-write)
#include <cstring>

namespace FaceShading
{
    // Select a world-space axis (a normalized matrix row) by enum:
    //   0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
    inline DirectX::XMVECTOR PickAxis(const DirectX::XMMATRIX& m, int axis)
    {
        using namespace DirectX;
        switch (axis)
        {
            case 0: return            m.r[0];
            case 1: return XMVectorNegate(m.r[0]);
            case 2: return            m.r[1];
            case 3: return XMVectorNegate(m.r[1]);
            case 4: return            m.r[2];
            case 5: return XMVectorNegate(m.r[2]);
            default:return            m.r[2];
        }
    }

    inline void Update(World& world, PoseRingBuffer& poseBuffer, SkeletonRegistry& skeletons)
    {
        using namespace DirectX;

        auto* facePool = world.GetPool<FaceShadingComponent>();
        if (!facePool) return;

        const auto& ents = facePool->Entities();
        auto&       data = facePool->Data();
        const size_t n = data.size();

        // Each iteration touches only its own FaceShadingComponent and face
        // MaterialComponent (plus reads of the skeleton, the pose and GlobalTransform, all
        // final by now), so from 16 faces up they run on the task pool (serially they cost
        // ~0.2 ms of main-thread time at 200 characters).
        auto processFace = [&](size_t i)
        {
            const Entity faceEnt = ents[i];
            FaceShadingComponent& fsc = data[i];

            // The frameParams we write must land on the FACE material.
            MaterialComponent* mc = world.GetComponent<MaterialComponent>(faceEnt);
            if (!mc) return;

            // Resolve the skeleton OWNER: self if it holds SkeletonComponent, else
            // follow SkeletonRef to the character root.
            Entity ownerEnt = faceEnt;
            const SkeletonComponent* skel = world.GetComponent<SkeletonComponent>(faceEnt);
            if (!skel)
            {
                const SkeletonRef* ref = world.GetComponent<SkeletonRef>(faceEnt);
                if (ref && ref->entity != NullEntity)
                {
                    ownerEnt = ref->entity;
                    skel = world.GetComponent<SkeletonComponent>(ownerEnt);
                }
            }
            if (!skel) return;
            if (skel->assetIndex == kInvalidAnimHandle) return;
            if (skel->poseByteOffset == ~0u) return;          // no pose this frame

            const SkeletonAsset& asset = skeletons.Get(skel->assetIndex);

            // Resolve the head bone index once by direct name scan (robust against any FNV hash
            // mismatch; boneCount is small and the result is cached).
            if (fsc.cachedBoneIdx == ~0u)
            {
                const char* want = fsc.headBoneName.c_str();
                const size_t wantLen = fsc.headBoneName.size();
                for (uint32_t b = 0; b < asset.boneCount; ++b)
                {
                    if (std::strcmp(asset.boneNames[b], want) == 0)
                    {
                        fsc.cachedBoneIdx = b;
                        break;
                    }
                }
                // SUFFIX fallback: imported rigs prefix their bones ("Bip001_Head",
                // "mixamorig:Head", "Armature|Head"), and a miss leaves the face material on its
                // (1,1,1,1) sentinel with the SDF face shadow silently off. Match a
                // separator-delimited suffix, so "Head" finds "Bip001_Head" but never "Forehead".
                if (fsc.cachedBoneIdx == ~0u && wantLen > 0)
                {
                    for (uint32_t b = 0; b < asset.boneCount; ++b)
                    {
                        const char*  nm  = asset.boneNames[b];
                        const size_t len = std::strlen(nm);
                        if (len <= wantLen) continue;
                        if (std::strcmp(nm + (len - wantLen), want) != 0) continue;
                        const char sep = nm[len - wantLen - 1];
                        if (sep != '_' && sep != ':' && sep != '|' && sep != '.' && sep != ' ')
                            continue;
                        fsc.cachedBoneIdx = b;
                        LOG_INFO("FaceShadingSystem: head bone '%s' matched '%s' by suffix",
                                 want, nm);
                        break;
                    }
                }
                if (fsc.cachedBoneIdx == ~0u)
                {
                    // Warn ONCE per component, so a mistyped bone shows in the log instead of as "the
                    // face SDF does nothing".
                    if (!fsc.warnedMissingBone)
                    {
                        fsc.warnedMissingBone = true;
                        LOG_WARNING("FaceShadingSystem: head bone '%s' not found in the "
                                    "skeleton (%u bones) — the SDF face shadow stays OFF "
                                    "for this material. Set FaceShadingComponent."
                                    "headBoneName to the real bone name.",
                                    want, asset.boneCount);
                    }
                    return;          // name not found (yet)
                }
            }
            if (fsc.cachedBoneIdx >= skel->boneCount) return;

            const XMFLOAT4X4* skin = poseBuffer.ReadMapped(skel->poseByteOffset);
            if (!skin) return;

            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(ownerEnt);
            const XMMATRIX rootWorld = gt ? XMLoadFloat4x4(&gt->matrix) : XMMatrixIdentity();

            const uint32_t b = fsc.cachedBoneIdx;
            const XMMATRIX modelBone = XMLoadFloat4x4(&asset.bindPose[b]) * XMLoadFloat4x4(&skin[b]);
            const XMMATRIX worldBone = modelBone * rootWorld;

            XMVECTOR fwd = XMVector3Normalize(PickAxis(worldBone, fsc.forwardAxis));
            XMVECTOR rgt = XMVector3Normalize(PickAxis(worldBone, fsc.rightAxis));

            if (fsc.horizontalize)
            {
                const XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
                const XMVECTOR fFlat = XMVectorSetY(fwd, 0.f);
                if (XMVectorGetX(XMVector3LengthSq(fFlat)) > 1e-6f)   // guard near-vertical forward
                    fwd = XMVector3Normalize(fFlat);
                rgt = XMVector3Normalize(XMVector3Cross(up, fwd));     // right = up x forward
            }

            XMStoreFloat3(&fsc.faceForward, fwd);
            XMStoreFloat3(&fsc.faceRight,   rgt);

            // One-time diagnostic. forwardAxis / rightAxis are a per-RIG guess (+Z/+X is a DCC
            // convention; 3ds Max Biped heads usually run +X along the bone), and a wrong pick
            // does not fail loudly: the SDF sweeps around the wrong axis. Print the resolved
            // world axes once so the pick can be checked against the character's facing:
            //   * `forward` should point out of the face.
            //   * `right`'s Y should be ~0 on an upright head; a large one means the bone is
            //     rolled and the .xz flatten rotates the sweep (horizontalize = true rebuilds
            //     right from world up).
            if (!fsc.loggedAxes)
            {
                fsc.loggedAxes = true;
                LOG_INFO("FaceShadingSystem: bone '%s' -> forward (%.3f, %.3f, %.3f)  "
                         "right (%.3f, %.3f, %.3f)  [right.y = %.3f; ~0 expected on an "
                         "upright head] horizontalize=%d",
                         asset.boneNames[b],
                         fsc.faceForward.x, fsc.faceForward.y, fsc.faceForward.z,
                         fsc.faceRight.x,   fsc.faceRight.y,   fsc.faceRight.z,
                         fsc.faceRight.y, fsc.horizontalize ? 1 : 0);
            }

            XMFLOAT3 f, r;
            XMStoreFloat3(&f, fwd);
            XMStoreFloat3(&r, rgt);

            // These values follow the live pose every frame, so they go into the material's
            // frameParams (runtime-only, outside matHash). The flag makes the renderer key this
            // material's slot by identity, repack its b8 every frame and write its frame record
            // (MaterialComponent::PER_FRAME_PARAMS); writing them into customParams instead
            // would change matHash and mint a new material slot per face every frame.
            mc->SetPerFrameParams();

            // Write only on change: in Play the head moves every frame, in Edit (frozen pose)
            // the values are bit-identical and the map is left untouched.
            auto setParam = [mc](const char* name, const std::array<float, 4>& v)
            {
                auto it = mc->frameParams.find(name);
                if (it == mc->frameParams.end()) mc->frameParams.emplace(name, v);
                else if (it->second != v)        it->second = v;
            };
            setParam(MaterialComponent::kFrameHeadForward, { f.x, f.y, f.z, 0.f });
            setParam(MaterialComponent::kFrameHeadRight,   { r.x, r.y, r.z, 0.f });

            // Head ORIGIN in world space, for a face shader using the "spherical head normal"
            // trick (bend the mesh normal toward normalize(worldPos - headCenter) to kill
            // nose / eye-socket noise under a toon terminator). Written only when the material
            // declares the param (reflection seeded it into customParams).
            if (mc->customParams.find(MaterialComponent::kFrameHeadCenter) != mc->customParams.end())
            {
                XMFLOAT3 hc;
                XMStoreFloat3(&hc, worldBone.r[3]);
                setParam(MaterialComponent::kFrameHeadCenter, { hc.x, hc.y, hc.z, 0.f });
            }
        };
        if (n >= 16)
            TaskSystem::Get().ParallelFor(0, static_cast<uint32_t>(n),
                                          [&](uint32_t i) { processFace(i); });
        else
            for (size_t i = 0; i < n; ++i) processFace(i);
    }
}