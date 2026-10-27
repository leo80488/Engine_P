// morph_limits.hlsli - the morph-target (blend-shape) cap: one definition read by both C++
// and the GPU. Do not write the cap down anywhere else: a copy that drifts makes the GPU
// silently ignore targets every C++ bound accepts (a face that simply does not move).
// C++ includes it as `#include "../../shaders/morph_limits.hlsli"` (SkeletonAsset.h,
// AnimationComponents.h); Skin.cs.hlsl by plain name, since DXC resolves a quoted include
// against the including file's directory.
// Lives under shaders/ on purpose: package_game.py bundles that tree into game.ipak, so a
// runtime DXC compile in a packed build resolves it. A header under include/ is not packed,
// and every runtime shader compile would fail with "cannot open source file".
// Cost of raising it: the cap sizes three fixed arrays - SkeletonAsset::morphTargetNames[N][64]
// (64 KB at 1024, inside a ~360 KB heap-allocated asset), MorphClipAsset::morphNames[N][64]
// (64 KB per loaded VMD/.ianim morph clip) and MorphComponent::weights[N] (4 KB per skinned
// character root). It does not size the per-job GPU upload region: SkinningPass packs those
// back-to-back at the job's own morphCount (kMorphWeightAlignFloats), so the GPU buffer pays
// only for what content uses.
// Not an on-disk number: .iskel writes `count` then `count * 64` name bytes, and .ianim writes
// its morph channel list length-prefixed. Raising the cap reads every existing asset
// unchanged; an asset authored at a higher cap is only clamped on load by an older binary.
#ifndef MORPH_LIMITS_HLSLI
#define MORPH_LIMITS_HLSLI

// The cap. 1024 covers the VRChat/Booth rigs this engine imports (Fiona 302 usable shapes,
// ICHIGO 558) with room left; MMD/PMX faces sit far below it. Read the cost note above
// before raising it.
#define ENGINE_MAX_MORPH_TARGETS 1024u

// The C++ face of the same number. DXC does not define __cplusplus when compiling HLSL, so
// the shader compiler never sees this block.
#ifdef __cplusplus
inline constexpr unsigned int kMaxMorphTargets = ENGINE_MAX_MORPH_TARGETS;
#endif

#endif // MORPH_LIMITS_HLSLI
