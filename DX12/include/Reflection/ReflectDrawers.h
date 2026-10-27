#pragma once

// ReflectDrawers - the editor's widgets for REFLECT_CUSTOM fields, by the drawer id a descriptor names
// ("quaternionEuler", "tags", ...). Descriptors are data that EngineCore compiles too; the ImGui half
// lives here, in EditorCore. Main thread.

#include "Reflection/Reflect.h"

namespace Reflect
{
    // Draws the field at @p fieldPtr; true when it changed the value this frame.
    using DrawerFn = bool (*)(void* fieldPtr, const FieldDescriptor& f);

    // Registers @p fn as the drawer named @p id (a module's own widget); the core ones are built in.
    // @p id must be a string literal (it is kept, not copied).
    void     RegisterDrawer(const char* id, DrawerFn fn);
    // nullptr when no drawer has that id.
    DrawerFn FindDrawer(const char* id);
}
