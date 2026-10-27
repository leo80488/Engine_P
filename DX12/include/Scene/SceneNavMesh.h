#pragma once

// The scene's navigation mesh as the core sees it: the file a scene names (its navMesh= line).
// The Nav module installs a provider that loads it and reports the file it holds; without the
// module the file is only remembered, so saving the scene keeps the reference.

#include <string>

namespace SceneNavMesh
{
    struct Provider
    {
        // Loads @p path; false when it could not.
        bool        (*load)(const std::string& path) = nullptr;
        // The file the loaded mesh came from or was last saved to ("" when none).
        std::string (*sourcePath)()                  = nullptr;
    };

    // The Nav module, in its Initialize.
    void Install(const Provider& provider);
    bool HasProvider();

    // A scene load named @p path (non-empty): remembered, and loaded when a provider is installed.
    // False only when the provider failed to load it.
    bool LoadForScene(const std::string& path);

    // The file a saved scene names: the provider's source when one is installed, else the last
    // scene's.
    std::string Path();
}
