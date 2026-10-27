#pragma once
// The engine's content roots, mounted once at boot: the project and the engine trees, a
// worktree's shared checkout (Resource/SharedContent.h), the identity index over the same
// roots, and game.ipak from the read-only install root.

namespace Resource
{
    void MountContentRoots();

    // A GAME host (not the editor) with neither a sealed game.ipak next to the exe
    // nor a loose shaders/ tree in the working directory: throws std::runtime_error
    // naming both roots, before the renderer turns it into a black window. Call
    // after MountContentRoots.
    void RequireGameContent();
}
