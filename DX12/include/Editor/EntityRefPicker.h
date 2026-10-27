#pragma once

// Inspector helpers for the GUID-based entity reference system.
//
// AttachmentRef (ECS/GuidComponent.h) is the cross-entity reference type.
// Components that hold one (camera, follow and look targets, patrol points, ...)
// get a target picker in their Inspector row via DrawAttachmentRef.
//
// GuidComponent gets a read-only display + regenerate popup via
// DrawGuidComponentInspector. There is no explicit "Stamp GUID" step: adding
// GuidComponent through Add Component shows that inspector, and BindAndStamp from
// any picker adds the component on the target.

#include "ECS/ECS.h"

struct AttachmentRef;
struct GuidComponent;
class World;

namespace Editor
{

// Returns true if the user touched the ref this frame (binding, picking a new
// target, or clearing). selfEntity is excluded from the picker so a component
// cannot bind to its own host entity.
bool DrawAttachmentRef(const char* label,
                       World& world,
                       Entity selfEntity,
                       AttachmentRef& ref);

void DrawGuidComponentInspector(GuidComponent& gc, World& world, Entity self);

} // namespace Editor
