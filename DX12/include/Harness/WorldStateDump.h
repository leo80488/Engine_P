#pragma once
// Harness/WorldStateDump.h - a read-only, deterministically ordered snapshot of the ECS
// world, for diffing one run against another.
// Not Resource::SaveScene: it MUTATES the world it observes (for an entity with a
// MaterialComponent and no MaterialSourcePath it writes generated .imat files and adds
// MaterialSourcePath to the live entity), which breaks before/after diffing and dirties
// the repo. This writes one file and touches nothing else.
// Uses ComponentSerializerRegistry instead, skipping the serializers that are not
// read-only (kMutatingTags in the .cpp); the skipped list is written into the file so a
// reader sees the gap.
// Determinism - an unstable dump produces diffs that look like regressions, so three
// sources of ordering instability are sorted away:
//   * World::GetEntities() is a vector DestroyEntity reorders with swap-to-back, and
//     CreateEntity pops the free list LIFO.
//   * World::GetComponentTypeIndices() order is unspecified (hash-map order; pools are
//     created lazily, so it varies with async asset-load order).
//   * The registry itself is an unordered_map keyed by std::type_index.
//   Entities are emitted ascending by id, components ascending by tag.
// Format: JSON, each component's payload the registry's own `key=val key=val` .iscene
// line, verbatim, as a JSON string - re-parsing its percent-encoded tokens would add a
// second format to keep in sync for no diffing benefit.

#include <string>

class World;

namespace Harness
{
    /// Write the snapshot to @p utf8Path; false on any I/O failure. Never throws: it is
    /// reachable from a PostRender system, and the Scheduler's serial path has no try/catch.
    /// @p world is non-const only because the registry's `has` and `serialize` callbacks take
    /// World&; nothing on this path modifies it.
    bool WriteWorldStateDump(World& world, const std::string& utf8Path);
}
