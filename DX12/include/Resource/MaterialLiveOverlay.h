#pragma once

// MaterialLiveOverlay - an editor's UNSAVED material, served in place of its file.
//
// While a Material Editor tab has unsaved edits, its staged material is what every
// LoadMaterial of that path resolves to - for the material itself AND as the parent
// of any instance of it - so the scene shows the edit before Save. The editor
// pushes a new version (at most 10 times a second while a control is dragged, at
// once on release) and then re-materializes the entities that read the path
// (ReloadMaterialsAffectedBy with live = true: an entity with unsaved edits of its
// own keeps them). Save writes the file and clears the entry BEFORE the reload,
// so the reload reads the file; closing a tab without saving clears it and reloads
// the file's look.
//
// Implemented in MaterialSerializer.cpp, in front of the resolved-material cache:
// an overlaid path is never cached, and an instance resolved on top of one records
// the path as a dependency, so clearing the entry (InvalidateMaterialsDependingOn)
// re-resolves it. Empty in Game.exe: only the editor sets entries, and an empty
// overlay costs one atomic load per resolve.

#include <cstddef>
#include <string>

struct MaterialComponent;

namespace Resource
{
    struct MaterialLoadInfo;

    // Replace @p path's live version (the resolved staged material and its load
    // info) and invalidate the resolved cache for the path and its dependents.
    void SetLiveMaterial(const std::string& path, const MaterialComponent& mat, const MaterialLoadInfo& info);
    // Drop @p path's live version (and invalidate as above). True when it had one.
    bool ClearLiveMaterial(const std::string& path);
    // @p path's live version, if any (the resolver's lookup).
    bool GetLiveMaterial(const std::string& path, MaterialComponent& mat, MaterialLoadInfo* info = nullptr);
    std::size_t LiveMaterialCount();

    // While one is in scope, THIS thread's LoadMaterial reads the files, not the
    // overlay: the editor's "as on disk" reads (a tab's Revert and reload, Save
    // as Instance's diff against the file) would otherwise read its own edits.
    struct LiveMaterialBypass
    {
        LiveMaterialBypass();
        ~LiveMaterialBypass();
        LiveMaterialBypass(const LiveMaterialBypass&) = delete;
        LiveMaterialBypass& operator=(const LiveMaterialBypass&) = delete;
    };
}
