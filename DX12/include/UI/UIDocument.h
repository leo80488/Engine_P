#pragma once

// UIDocument - a scene's REFERENCE to a .iui UI asset: the scene keeps one line
// (CvDoc: path=...), and the asset's nodes exist only at run time - spawned through the
// .iui codec (UI/UIAsset.h), each one SceneTransientTag (no scene file holds it) and
// UIDocumentMember (read-only in the editor: edit the asset in its own UI Editor tab).
// An instance is spawned again whenever its asset is re-read (a save, the hot-reload
// watcher), its path changes, or it went missing, so every referencing scene follows edits.
//
// A RECT-root asset is spawned under its host (a canvas or a UI node, which its anchors
// place it in). A CANVAS asset is a root of its own: its host is an entity that is no UI
// node (a canvas takes no parent).
//
// EngineCore, both exes, Edit and Play: UICanvasSystem::UpdateInput calls UpdateDocuments
// once a frame before layout, input and scripts, so an instance exists for that frame's
// scripts (OnSpawn, OnSceneEnter) and hit tests.

#include "ECS/ECS.h"

#include <memory>
#include <string>

namespace UI
{
    struct UIAsset;

    struct UIDocument
    {
        std::string path;                        // authored: the .iui (CvDoc path=)
        // ---- runtime (never saved), kept by UpdateDocuments ----
        Entity      root = NullEntity;           // the instance's root
        std::shared_ptr<const UIAsset> asset;    // what it was spawned from (a re-read is another one)
        std::string spawnedPath;                 // ... for which path ("" = never tried)
        std::string error;                       // why there is no instance ("" = there is one, or no path)
        // UIAssetGeneration() at its last look. Per document: a world Stop or an undo hands back
        // documents that missed every re-read.
        std::uint64_t seenGeneration = 0;
    };

    // Runtime only, on every node of an instance: whose instance it is. A node whose
    // host is gone, or holds another instance now, is removed by UpdateDocuments.
    struct UIDocumentMember
    {
        Entity host = NullEntity;
        Entity root = NullEntity;
    };

    // Spawn every missing / stale instance, remove the orphaned ones. Once a frame.
    void UpdateDocuments(World& world);

    // The host of @p e when it is a node of a UIDocument instance, else NullEntity.
    Entity DocumentHostOf(const World& world, Entity e);

    // The number of nodes of @p host's instance (0 = none).
    std::size_t DocumentNodeCount(const World& world, Entity host);

    // Instance nodes and hosts in @p root's UI subtree: what a capture of that subtree writes
    // inline (a .iui holds no references); the callers report it.
    std::size_t DocumentNodesUnder(const World& world, Entity root);
}
