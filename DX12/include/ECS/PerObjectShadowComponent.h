#pragma once

// PerObjectShadowComponent - opt-in tag: this entity and its descendant subtree are rendered into
// the high-resolution PerObjectShadowPass map (a directional shadow fitted tightly to the object),
// composited over CSM in Lighting.ps as shadow = min(csm, perObject). Put it on the ROOT of a
// character that needs crisp self-shadow and ground contact (editor: Add Component ->
// "Per-Object Shadow").
// The renderer also includes any PlayerComponent subtree by itself; this tag covers every other
// character, and projects with no player system.

#include "ECS/ECS.h"

struct PerObjectShadowComponent : ComponentBase
{
    // Not read by the renderer: reserved for per-object tuning (fit margin, resolution tier). A real
    // field so the inspector has something to show and the serializer round-trips a stable value.
    float radiusScale = 1.0f;
};
