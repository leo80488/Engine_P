#pragma once
// Entity.*: read any serializable component, page through queries, write allow-listed ones.
// Bespoke bindings (Engine.GetLight, Engine.GetMaterial, ...) cover one component and a few
// fields. Entity.get / Entity.find cover every component the ComponentSerializerRegistry
// knows (the registry .iscene save, SaveGame and undo use) by serializing it and returning
// its key/value pairs as a Lua table:
//
//   Entity.components()             -> { readable, dedicated, pseudo, writable }
//   Entity.get(id [, select, opts]) -> nil | { id, session, epoch, name, components = {...}, [missing] }
//   Entity.schema(tag)              -> patch contract, or nil for a tag without one
//   Entity.referenceVersion         -> 1 (session / epoch checks supported)
//   Entity.find{ name=, tag=, component=, components=, fields=, all=, limit= }
//                                   -> { rows = {...}, count, truncated, scanned }
//   Entity.census()                 -> { entities, pools, structuralHash, epoch }
//   Entity.page{filters/projection, subtree?, limit=128, maxBytes=32768}
//   Entity.page{cursor=nextCursor, limit?, maxBytes?}
//     -> {rows,totalMatched,returned,revision,nextCursor?,frame,session,epoch,
//         consistency="snapshot",scanned,scanMs,elapsedMs,bytes,schemaVersion=1}
// Each row carries the session + epoch it was read in. get / set opts {session, epoch}
// refuse with stale_session / stale_epoch; the Python SDK's EntityRef passes them, while raw
// integers and Lua calls without opts skip that check (ai_authoring_contract.md).
// Paging (pageVersion = 1): at most four retained immutable snapshots, 120 s each, 8 MiB of
// row JSON each / 16 MiB total (not allocator accounting); Clear / restore, eviction or
// expiry -> stale_cursor. Edits do not change a snapshot; its ids still need get/set
// liveness checks. bytes = canonical page JSON length without the run wrapper. limit
// 1..1000 rows, maxBytes 1024..32768; an oversized row or snapshot fails explicitly, never
// a partial row. subtree includes its root.
//
// select / components / fields name what to read: "Tag" (the whole component) or
// "Tag.key" (one field), alone or in a list. An unknown tag is an ERROR, not an empty
// result: a typo must not read as "no such component in the world". A requested field the
// component did not write lands in `missing`.
//
// Values: a component is a table of its fields, or a list of such tables when its
// serializer writes more than one line. A field is what the serializer wrote (what a
// .iscene holds), converted: numbers -> numbers, "a_b_c" of numbers (the vector form) ->
// a list, true/false -> booleans, other text -> the percent-decoded string. GUIDs (hex=,
// *Guid=) stay strings. Transforms are not in the registry (the .iscene N line carries
// them): the "Transform" pseudo tag reads LocalTransform.
//
// Read-only by construction. Four serializers are not (FollowSocket, FollowEntity, Script
// and CvParent call ECS::EnsureGuidOn, stamping a GUID on the entity their reference points
// at), so they are never serialized here: a dedicated reader returns the same keys with the
// reference as the target's live id (plus its GUID when it has one); `dedicated` lists
// them. Nothing here writes to the world, a dirty flag or disk; Entity.census()
// (World::StructuralHash) is how a test proves it.

#include <functional>
#include <string>
#include <typeindex>
#include <vector>

#include "ECS/ECS.h"   // Entity

namespace sol { class state; }
struct LocalTransform;

void RegisterLuaEntityBindings(sol::state& lua, World& world);

// The serializer tags Entity.* reads with a dedicated read-only reader because
// their serializer is NOT read-only (sorted). The -dump_catalog dump marks them.
std::vector<std::string> LuaEntityDedicatedTags();

// Entity.set(id, tag, fields [, {session, epoch}])
//   -> { id, session, epoch, tag, applied = {keys}, before = {...}, after = {...}, [warnings] }
// Changes an existing component through its own deserializer, merged over its current
// values (a key not passed keeps its value), only for tags whose post-write needs are known.
// Refusals raise "Entity.set: <code>: <message>" with code stale_handle (dead, or the slot
// recycled), stale_session, stale_epoch (the world was cleared / reloaded since `epoch`),
// unknown_component, unsupported_write (the tag and why), no_component, unknown_key (lists
// the valid keys), bad_value. "Transform" is a pseudo tag: position / rotation (quaternion
// xyzw) / scale of LocalTransform.
// LuaEntityWritableTags returns those tags, sorted, Transform included.
std::vector<std::string> LuaEntityWritableTags();

// A module's component joins the writable tags (Module::IEngineModule::RegisterTypes). @p tag
// must outlive the process (a literal); @p warn, when set, adds the warnings a write reports
// after it landed (what else decides what the entity does with the value).
using LuaEntityWarnHook = void (*)(World&, Entity, std::vector<std::string>& warnings);
void RegisterLuaEntityWritable(const char* tag, LuaEntityWarnHook warn);

// Told about every write Entity.set made, after it landed: the editor records
// undo and marks the scene dirty (EngineCore cannot see the editor). Either
// callback may be empty. Main thread.
struct LuaEntityWriteObserver
{
    std::function<void(Entity, std::type_index, const std::string& blobBefore,
                       const std::string& blobAfter)> component;
    std::function<void(Entity, const LocalTransform& before, const LocalTransform& after)> transform;
};
void SetLuaEntityWriteObserver(LuaEntityWriteObserver observer);
