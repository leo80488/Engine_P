#pragma once

// HumanoidProfileBuilder - assembles SkeletonAsset::humanoid (HumanoidProfile) from
// bone names + rest pose (design: docs/design/RETARGET_ARCHITECTURE.md).
//   Build()           - name-based: detect (or take) a convention, map every bone
//                       to a role, then FinalizeDerived().
//   FinalizeDerived() - (re)compute the derived data (oriented rest rotation, hip
//                       height, required-15 validity) from a filled boneForRole[];
//                       the VRM path calls it after setting boneForRole from the
//                       parsed humanoid map.
// The oriented-bind reconstruction only needs to be DETERMINISTIC: source and
// target reconstructions cancel in the bind-relative delta retarget.

#include "Resource/HumanoidBone.h"

struct SkeletonAsset;

namespace Resource
{
    class HumanoidProfileBuilder
    {
    public:
        // Map bone names -> roles and populate skel.humanoid. If conv == Unknown,
        // the convention is auto-detected from the bone names.
        static void Build(SkeletonAsset& skel,
                          HumanoidConvention conv = HumanoidConvention::Unknown);

        // Recompute restModelRot[], hipHeight and valid from the current
        // boneForRole[] map. Safe to call repeatedly.
        static void FinalizeDerived(SkeletonAsset& skel);
    };
}
