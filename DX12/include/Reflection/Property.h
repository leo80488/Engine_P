#pragma once

// Property - one field of a reflected object addressed by a path, read and written as a typed value.
// The Inspector's undo, a multi-selection edit, the editor's Lua property calls and the catalog
// share it, so every writer of a reflected field goes through the same checks (WriteProperty).
//
// A path names fields by key (Reflect::KeyOf: the serializer's key, the C++ name otherwise) or by
// C++ name, then optional steps:
//     "linDamp"            a field of the type (RigidBodyComponent::linearDamping)
//     "aMinX"              one part of a split key (UIRect anchorMin.x)
//     "translation.y"      a component of a float vector or quaternion (x y z w)
//     "points[2].width"    an element of a vector / fixed array, then a field of the element
//     "settings.gain"      a field of a nested struct
// Main thread, like every reflected object it edits.

#include "Reflection/Reflect.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <typeindex>
#include <variant>
#include <vector>

namespace Reflect
{
    // Up to four floats: a float vector, a colour, a quaternion (xyzw).
    struct FloatVec
    {
        float v[4] = {};
        int   n = 0;
    };

    // A field's value: bool, an integer (enums and bit sets too), a float (as double), a float
    // vector, or a string (strings and asset paths).
    using PropertyValue = std::variant<std::monostate, bool, int64_t, double, FloatVec, std::string>;

    struct PathStep
    {
        const FieldDescriptor* field = nullptr;
        int component = -1;   // a float vector's x y z w, or the part of a split key
        int index = -1;       // an element of a vector / fixed array
    };

    struct PropertyPath
    {
        const TypeDescriptor* type = nullptr;
        std::vector<PathStep> steps;
        std::string           text;

        bool Valid() const { return type && !steps.empty(); }
        const FieldDescriptor* Leaf() const { return steps.empty() ? nullptr : steps.back().field; }
    };

    // Parses @p text against @p type. False with @p error naming the step it could not resolve.
    bool ParsePath(const TypeDescriptor& type, std::string_view text, PropertyPath& out, std::string& error);

    // The field's address in @p obj (the element / component the path names), nullptr when an
    // element index is out of range.
    void*       Address(void* obj, const PropertyPath& path);
    const void* Address(const void* obj, const PropertyPath& path);

    // The value the path names in @p obj; monostate for a kind with no value (custom, a whole vector).
    PropertyValue Get(const void* obj, const PropertyPath& path);

    bool Equal(const PropertyValue& a, const PropertyValue& b);
    // The line format's spelling: 1 / 0, integers, %g floats, x_y_z vectors, the raw string.
    std::string ToText(const PropertyValue& v);
    // A value of @p path's kind read from text in ToText's form (bools also take true / false).
    bool FromText(const PropertyPath& path, std::string_view text, PropertyValue& out);

    // The paths (by key) of the data fields whose values differ between two objects of @p type.
    // A vector / array is compared element by element and reported whole - or, with
    // @p intoElements and the same element count, as the paths inside its changed elements
    // ("points[1].radius"); a custom field is compared byte-wise.
    std::vector<std::string> Diff(const TypeDescriptor& type, const void* a, const void* b, bool intoElements = false);

    // True when a path can be written as a value: a leaf that is not a custom field, a whole vector /
    // struct or a whole element.
    bool Writable(const PropertyPath& path);

    // ---- Writing ----
    enum class WriteOrigin : uint8_t
    {
        Inspector,   // the editor's widgets: a value past the hard range is clamped
        Undo,        // restoring a recorded value: written as recorded
        Lua,         // a script / the harness: needs LuaWrite; refuses past the hard range unless ClampHard
        Tool,        // other C++ callers: the field's own policy (refuse, or clamp under ClampHard)
    };

    struct WriteResult
    {
        bool          ok = false;
        bool          clamped = false;   // the value written is not the value given
        PropertyValue before;            // the value the field held
        PropertyValue written;           // the value it holds now
        std::string   error;             // why it was refused
    };

    // The one writer of a reflected field: resolves nothing (the path is parsed), refuses ReadOnly
    // (and, from Lua, fields without LuaWrite), converts the value to the field's kind, enforces the
    // hard range by @p origin, writes, then runs the type's change hooks.
    WriteResult WriteProperty(void* obj, const PropertyPath& path, const PropertyValue& value, WriteOrigin origin);

    // Writes the default instance's value of @p path (TypeDescriptor::defaults) with WriteProperty.
    WriteResult ResetToDefault(void* obj, const PropertyPath& path, WriteOrigin origin);

    // Runs after every successful WriteProperty on an object of @p type (Describe<T>(); re-derive a
    // cache, mark a rebuild): the place for a side effect the Inspector, Undo and Lua must all trigger.
    using PropertyChangedFn = void (*)(void* obj, const PropertyPath& path);
    void AddPropertyChangedHook(const TypeDescriptor& type, PropertyChangedFn fn);
}
