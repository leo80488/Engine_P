#pragma once
// What the Face.* Lua bindings and face tracking share, so both resolve a character and expand
// a preset the same way:
//   - the SkeletonComponent carrier of a character (the only entity BuildSkinJobs reads
//     FacialAnimationComponent - and so the Tracked layer - from);
//   - a model's shape-key presets: the MP records of the model an entity was spawned from (VRM
//     expressions, PMX group morphs) as one table by preset name of (morph name, bind weight)
//     pairs. Face.SetExpression and the .ifacemap "preset:<name>" targets both expand through it.

#include "ECS/ECS.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Facial
{
    // The SkeletonComponent carrier under `root`: root itself, its SkeletonRef, or the first
    // one a visited-set-guarded walk over Children finds (an authored Children cycle must not
    // hang the caller). NullEntity when none.
    Entity FindSkeletonCarrier(World& world, Entity root);

    // From any entity of a character (a live id): Anim::ResolveAnimEntityOrSelf first - it
    // keys on AnimationComponent, so a rig with morphs but no AnimationComponent lands on the
    // character root - then the carrier under that, then under `live` itself
    // (ResolveAnimEntityOrSelf can hop sideways through SkeletonRef). NullEntity when none.
    Entity ResolveSkeletonCarrier(World& world, Entity live);

    using PresetTable = std::unordered_map<std::string, std::vector<std::pair<std::string, float>>>;

    // The SceneSourcePath of `owner` or of its nearest ancestor that has one; "" when none.
    std::string OwnerModelPath(World& world, Entity owner);

    // The merged table of the model at `path` (every live Mesh's records; a target two
    // Meshes' records name keeps its larger weight). Re-checked against the file at most
    // every half second per model; never null (an empty table for "" or a model without
    // presets). Thread-safe.
    std::shared_ptr<const PresetTable> PresetsOfModel(const std::string& path);
}
