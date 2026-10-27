#pragma once
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
#include <string>

struct ClipAsset;
struct SkeletonAsset;
struct LocomotionComponent;
class ClipLibrary;

namespace Anim {
struct LocalPose {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 rot;
    DirectX::XMFLOAT3 scl;
};
static_assert(sizeof(LocalPose) == 40);
namespace PoseOps {
DirectX::XMFLOAT3 Lerp3(const DirectX::XMFLOAT3&, const DirectX::XMFLOAT3&, float);
DirectX::XMFLOAT4 Slerp4(const DirectX::XMFLOAT4&, const DirectX::XMFLOAT4&, float);
void SampleClip(const ClipAsset&, float time, uint32_t bones, LocalPose*, bool looping = true);
void RefPose(const SkeletonAsset&, LocalPose*);
void ApplyGrants(const SkeletonAsset&, LocalPose*, uint32_t bones, bool postIK);
void Blend(LocalPose* base, const LocalPose* other, uint32_t bones, float weight,
           const float* mask = nullptr);
void Additive(LocalPose* base, const LocalPose* other, const LocalPose* reference,
              uint32_t bones, float weight, const float* mask = nullptr);
bool BuildBoneMask(const SkeletonAsset&, uint8_t mode, const std::string& root,
                   std::vector<float>& weights);
void ResolveBlendSpace(LocomotionComponent*, const ClipLibrary&, float dt, bool paused);
float WrapAngle(float);
}
}
