// ComponentFlags.cpp: the flags of the components whose serializer is hand-written. A component
// described by a reflection descriptor states its flags where it registers its serializer
// (RegisterReflectedSerializer<T>(reg, tag, numbers, ComponentFlag::LuaWritable)); these are the tags
// that cannot, because their fields are not reflected or their lines need code (a migration, a
// conditional key, an asset lookup).

#include "Resource/ComponentSerializers.h"

#include "System/Check.h"

namespace
{
    // Entity.set may change them: their systems read the component every frame, or
    // ComponentWrite carries the runtime state across the rewrite (Resource/ComponentWrite.h). What a
    // write of each needs is in Scripting/LuaEntityBindings.cpp (the warnings) and ComponentWrite.cpp.
    constexpr const char* kLuaWritableTags[] = {
        "AIBehavior",
        // Canvas UI: read every frame by layout / paint / interaction. CvParent is not here: the edge
        // moves with Editor.Reparent (one undo step, the GUID kept).
        "CvImage", "CvSpriteAnim", "CvText",
        "AnimationSet", "Atmosphere", "AudioSource", "Camera", "Clouds", "Collider", "DDGIVolume", "Decal",
        "GameplayTags", "IndirectLighting", "IKRig", "LightData", "Perception", "RigidBody", "Skybox",
        // A write merges into the entity's own sockets and moves the generation on.
        "Sockets",
        // MeshInstanceSystem compares every owner's fields with the signature its sections were
        // derived from, every frame: a rewrite rebuilds them.
        "StaticMesh",
        // TimelineSystem reads the tracks every frame and snaps its cursor on the first frame after a rewrite.
        "Timeline",
        "TODConfig", "Visibility", "WorldEnv",
    };
}

void ApplyHandWrittenComponentFlags(ComponentSerializerRegistry& reg)
{
    for (const char* tag : kLuaWritableTags)
        ENGINE_CHECK(reg.AddFlags(tag, ComponentFlag::LuaWritable),
                     "ApplyHandWrittenComponentFlags: '%s' has no serializer to flag", tag);
}
