#pragma once

// AnimationSerializer - writes an AnimationResource to a .ianim blob, the inverse of
// AnimationLoader::Load. The bone / morph / camera clips use AnimationImporter's
// layout byte for byte; the optional tails follow - AnimNotify tracks
// (ANIM_FLAG_HAS_NOTIFIES), root motion and float curves (ANIM_FLAG_HAS_TRACKS), IK
// enable tracks (ANIM_FLAG_IK_ENABLE) - so the animation timeline's Save round-trips
// a clip's edited tracks.

#include <cstdint>
#include <vector>

namespace Resource
{
    class AnimationResource;

    // Serializes res to a .ianim blob (header, metadata, bone / morph / camera clips,
    // then the optional tails). The offset and leg-FK flags come from the per-clip
    // flags, so a VMD-sourced clip round-trips its additive-pose semantics. Empty only
    // when res has no bone, morph or camera clips.
    std::vector<uint8_t> SerializeAnimation(const AnimationResource& res);
}
