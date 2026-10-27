#pragma once

// HumanoidNameTable - maps rig-specific bone names to canonical HumanoidBone roles
// per convention, and detects a skeleton's convention from its bone names. Pure
// lookup / heuristics; profile assembly (oriented-bind reconstruction, validation)
// is HumanoidProfileBuilder's.
//
// PMX names are Japanese (PmxImporter stores nameJP). The TU is compiled with
// /utf-8, so its plain narrow literals are UTF-8 bytes matching
// SkeletonAsset::boneNames verbatim. Do NOT use u8"" (char8_t in C++20).

#include "Resource/HumanoidBone.h"

struct SkeletonAsset;

namespace Resource
{
    // Reverse of HumanoidRoleName (VRM 1.0 vocabulary). Returns HumanoidBone::Count
    // when the name is not a canonical role name. Used for the VRM humanoid map.
    HumanoidBone RoleFromCanonicalName(const char* name);

    // Map one bone name of the given convention to a canonical role. Returns
    // HumanoidBone::Count when the bone has no humanoid role (e.g. MMD center / IK /
    // twist / physics bones, Mixamo _End tips, UE ik_* / twist).
    HumanoidBone MapBoneNameToRole(HumanoidConvention conv, const char* boneName);

    // Classify a skeleton's naming convention from its bone names.
    HumanoidConvention DetectConvention(const SkeletonAsset& skel);
}
