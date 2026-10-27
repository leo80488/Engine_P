// ComponentRegistryCheck.cpp: the drift guard between the two lists of "all components" - the
// serializer registry (what survives a scene save, a SaveGame snapshot and an undo blob) and the
// reflection descriptors (what the inspector, Entity.set and the catalog know). A component described
// but not serialized is silently dropped by every one of those, which is how the whole VCam stack once
// lost its save / load / undo. The guard is structural, so it needs no count to keep: a described type
// must have a serializer unless it is listed below as persisting some other way, or not at all.

#include "Resource/ComponentSerializers.h"

#include "Reflection/ReflectRegistry.h"
#include "Reflection/Reflect.h"
#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace
{
    // Described types with no serializer, by descriptor name. Every one is deliberate.
    constexpr std::string_view kNotSerialized[] = {
        // Rebuilt every frame from their sources, never authored.
        "LiveCameraComponent", "TODOutputComponent",
        // A transient runtime spawn, and a sub-struct of BeamComponent (not a component).
        "UI::DamageNumberComponent", "BeamControlPoint",
        // Persisted, but not as a registry line: the scene's N line, SceneRef, AnimRef, the model's records.
        "LocalTransform", "RenderLayer", "LocalAabb", "AnimationComponent", "SkeletonComponent", "SkeletonRef",
        "MorphComponent", "CameraData",
    };

    bool Exempt(std::string_view name)
    {
        return std::find(std::begin(kNotSerialized), std::end(kNotSerialized), name) != std::end(kNotSerialized);
    }
}

std::vector<std::string> FindRegistryDrift(const ComponentSerializerRegistry& reg)
{
    std::vector<std::string> problems;
    for (const Reflect::RuntimeEntry& entry : Reflect::AllRuntime())
    {
        if (!entry.desc) continue;
        const std::string_view name = entry.desc->name;
        const bool serialized = entry.tag && reg.FindByTag(entry.tag);
        if (!serialized && !Exempt(name))
            problems.push_back(std::string(name) + " has a descriptor but no serializer: a scene, a save and an undo "
                               "blob drop it. Register one, or list it in kNotSerialized (ComponentRegistryCheck.cpp)");
        else if (serialized && Exempt(name))
            problems.push_back(std::string(name) + " is listed in kNotSerialized but has a serializer: delete the row");
    }
    std::sort(problems.begin(), problems.end());
    return problems;
}

void ValidateComponentRegistries(const ComponentSerializerRegistry& reg)
{
    for (const std::string& problem : FindRegistryDrift(reg))
        ENGINE_CHECK(false, "component registries disagree: %s", problem.c_str());
}
