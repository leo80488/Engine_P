#pragma once

// EditorFolderComponent - editor-only folder membership for the Hierarchy outliner.
// `path` is a "/"-separated folder path, e.g. "Enemies/Melee"; empty or absent =
// top level. It has NO runtime effect and is read only for ROOT entities (a child
// nests under its real Parent). It is serialized with the scene so folders persist;
// the Game build ignores it.

#include <string>

struct EditorFolderComponent
{
    std::string path;   // "Enemies/Melee"; empty = root (no folder)
};
