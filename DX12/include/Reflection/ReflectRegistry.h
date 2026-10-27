#pragma once
// Reflection/ReflectRegistry.h - which component types have a reflection descriptor, at run
// time, keyed by std::type_index.
// Reflect::Describe<T>() is compile-time; EngineCore registers every core descriptor here
// (Reflect::RegisterCoreDescriptors, with the serializers), and the editor re-registers the ones it
// draws with their inspector labels (EditorLayer::RegisterReflectedComponent), so tools that start
// from a type_index (the -dump_catalog harness dump) read field names, kinds, ranges and labels in
// every exe. Main thread only.

#include <cstddef>
#include <string_view>
#include <typeindex>
#include <vector>

namespace Reflect
{
    struct TypeDescriptor;

    // @p desc must outlive the process (Describe<T>() returns a function-local
    // static); @p label is the inspector's name for the component (a literal).
    void RegisterRuntime(std::type_index type, const TypeDescriptor* desc, const char* label);

    struct RuntimeEntry
    {
        const TypeDescriptor* desc  = nullptr;
        const char*           label = "";
        // The component's serializer tag ("RigidBody"); nullptr for a type no serializer writes.
        const char*           tag   = nullptr;
    };
    // nullptr when no descriptor was registered for @p type.
    const RuntimeEntry* FindRuntime(std::type_index type);
    // Names @p type's serializer tag (@p tag must outlive the process). Kept for a descriptor
    // registered later (a module's, by the editor). GetComponentRegistry names every serializer's.
    void SetRuntimeTag(std::type_index type, const char* tag);
    // nullptr when no registered descriptor has that serializer tag.
    const RuntimeEntry* FindRuntimeByTag(std::string_view tag);
    std::size_t RuntimeCount();
    // Every registered entry, in no particular order (the descriptor self-test, tools).
    std::vector<RuntimeEntry> AllRuntime();
}
