#pragma once

// SceneLoadContext - per-load helper that lets component deserializers remap the raw entity ids
// older .iscene files store (e.g. "camEntity=42"). Current files store entity references as GUIDs
// (GuidRegistry); for raw ids LoadScene binds a saved-idx -> new Entity map for one load.
// A deserializer that resolves an id (ResolveLegacyEntityIdx) stamps a GUID on the target
// (AttachmentRef::BindAndStamp), so the next save writes GUIDs only.
// The binding is thread_local: loads on different threads do not share it.

#include "ECS/ECS.h"

#include <cstdint>
#include <unordered_map>

namespace Resource
{

// Bind the saved-idx -> Entity map for this thread; nullptr clears it. The caller owns @p map and
// keeps it alive until it unbinds.
void SetLegacyEntityMap(const std::unordered_map<int, Entity>* map);

// Map a saved entity idx to the current Entity. NullEntity when no map is bound or @p oldIdx is
// not in it.
Entity ResolveLegacyEntityIdx(uint32_t oldIdx);

} // namespace Resource
