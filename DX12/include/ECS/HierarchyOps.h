#pragma once

// Hierarchy-aware structural operations on the World.
//
// World::DestroyEntity is deliberately flat: it destroys exactly the entity it is given. That is
// the right primitive, but not what "delete this thing" means for an entity that owns a subtree:
// its orphaned children stay alive and drawn, unreachable from scripts, and their GPU slots
// (SkinnedMeshSubsystem::OnEntityDestroyed -> MeshDescriptorHeap::FreeMesh) are never released.
// DestroySubtree is that operation; tests/scenarios/entity_churn_render covers it.

#include "ECS/ECS.h"

#include <cstdint>
#include <vector>

namespace Hierarchy
{

// Destroy `root` and everything it owns, deepest-first, and unlink `root` from its parent's
// Children list.
//
// Ownership is the union of three edges, because no single one covers a prefab instance:
//   * Children             - the scene-graph hierarchy.
//   * UI::UIParent         - Canvas widgets, which do not use Children.
//   * PrefabInstanceMember - entities LoadPrefab created that attach by socket/follow rather than
//     parenting (an equipped weapon, its trail). Honoured only while `root` still carries
//     PrefabSourcePath, the condition SaveScene uses too; a detached instance owns nothing.
//
// Deepest-first matters: a parent must never be freed while a live child still points at it, since
// Parent/UIParent references are raw ids, not generation-checked.
//
// Returns the number of entities actually destroyed.
uint32_t DestroySubtree(World& world, Entity root);

// Collect `root` and everything it owns into `out`, shallowest-first, without destroying anything.
// Same ownership rules as DestroySubtree.
//
// For inspecting or counting a subtree, not for deferred destroys: ScriptSystem::FlushPendingDestroys
// queues the root alone and calls DestroySubtree at flush time, so a reparent inside the deferral
// window is respected. A set captured early goes wrong both ways: it misses children adopted
// since, and kills children moved away.
void CollectSubtree(const World& world, Entity root, std::vector<Entity>& out);

} // namespace Hierarchy
