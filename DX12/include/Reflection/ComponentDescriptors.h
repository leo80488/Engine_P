#pragma once

// ComponentDescriptors - registers every core descriptor (ComponentReflection.h) in the runtime
// registry, so EngineCore - Game.exe included - can walk a component's fields from its type_index.

namespace Reflect
{
    // Idempotent; GetComponentRegistry calls it with the serializers. Main thread.
    void RegisterCoreDescriptors();
}
