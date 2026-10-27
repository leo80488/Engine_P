#pragma once

// FaceShadingComponent - drives the anime/NPR SDF face shadow (asset/Shaders/Face.ps.hlsl).
// The shader needs the head's WORLD-space forward + right axes to compute the key
// light's yaw relative to the face. FaceShadingSystem reads this entity's head bone
// every frame (from its own SkeletonComponent or the one behind SkeletonRef), builds
// the basis and writes it into the FACE MaterialComponent's frameParams
// "HeadForward" / "HeadRight" (read at b8 by the face shader and, through the
// material slot's frame record, by the deferred toon).
// Place it on the entity that owns the FACE MaterialComponent. A separate face part
// entity (split character) needs a SkeletonRef to the root that owns the
// SkeletonComponent + pose.

#include "ECS/ECS.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>

struct FaceShadingComponent
{
    // Head bone name in the skeleton. PMX/MMD models usually use the Japanese kanji
    // for "head" (U+982D); FBX/Assimp rigs are often "Head" / "head".
    std::string headBoneName = "Head";

    // Which head-bone LOCAL axis maps to face FORWARD / RIGHT:
    //   0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
    // PMX/MMD head bones usually run +Y along the bone, so the default Z-forward /
    // X-right is OFTEN WRONG for them: pick the axis (and sign) that points out of / to
    // the side of the face, and check the read-only Face Forward / Face Right vectors
    // in the inspector.
    int forwardAxis = 4;   // +Z
    int rightAxis   = 0;   // +X

    // Flatten the basis onto the world-horizontal plane so the shadow boundary sweeps
    // with light YAW only (ignores head pitch/roll). Off = the boundary tilts with the
    // head (the classic look).
    bool horizontalize = false;

    // ---- Runtime (not authored) ----
    uint32_t cachedBoneIdx = ~0u;   // resolved once from headBoneName by name scan
    bool     warnedMissingBone = false;  // log the "bone not found" once, not per frame
    bool     loggedAxes        = false;  // print the resolved world axes once (axis-pick sanity)

    // Read-only diagnostics, written every frame and shown in the inspector so authors
    // can confirm the basis points the right way.
    DirectX::XMFLOAT3 faceForward = { 0.f, 0.f, 1.f };
    DirectX::XMFLOAT3 faceRight   = { 1.f, 0.f, 0.f };
};
