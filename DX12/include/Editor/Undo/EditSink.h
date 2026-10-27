#pragma once

// EditSink - where an editor edit goes when it is not the scene's.
//
// A UI Editor ASSET tab edits a .iui file through a stage in the main World: the
// same helpers the scene's edits run (EditorLayer::PushEdit, RecordSpawn, the
// Inspector's field bracket, DeleteEntities, MoveUINode, the macros), but each
// step belongs to the FILE - its own ring, its own dirty flag - never to the
// scene's history or the scene's "unsaved changes". EditorLayer routes those
// helpers into the sink in scope (EditorLayer::SinkScope); with none they go to
// the scene's history.
//
// Shared ownership on purpose: a deferred edit (Add Component runs at the frame
// boundary) holds the sink it was made in, so a tab closed in between leaves it
// a history no one reads - never a dangling pointer.
//
// Header-only (like EditHistory.h): no vcxproj registration.

#include "Editor/Undo/EditHistory.h"

#include <functional>

namespace Editor::Undo
{
struct EditSink
{
    EditHistory history;              // the owner's own undo / redo ring
    // A step landed in the ring, or one was undone / redone: the owner's state
    // changed (the asset tab re-tags its stage and captures it).
    std::function<void()> changed;
};
} // namespace Editor::Undo
