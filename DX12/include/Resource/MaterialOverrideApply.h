#pragma once

// MaterialOverrideApply - the per-entity MaterialOverride bag: parse it, apply it
// onto a MaterialComponent, diff a live material into one, and the hot-reload entry
// point that re-applies it.
//
// In EngineCore, not EditorCore: the apply runs on the SCENE LOAD path, which
// Game.exe walks too. One TU for every caller (SceneSerializer, PrefabSerializer,
// the asset preview, ReloadMaterialsAffectedBy), so an override cannot silently
// stop working in one loader but not another.

#include "ECS/ECS.h"

#include <string>
#include <unordered_map>

struct MaterialComponent;
struct MaterialOverride;

namespace Resource
{
    // Parse the SERIALIZED form of a `MaterialOverride:` block - the key=value map
    // ComponentSerializerRegistry hands a deserializer - into a bag. The ONE parser:
    // the component deserializer and the asset preview (which reads a .ipfb as data,
    // with no World or entity) both call it; a second copy would be a silently
    // diverging deserializer, which PrefabSerializer.h's PrefabNodeView comment
    // forbids.
    //
    // Keys and `t:` texture paths arrive PERCENT-ENCODED (the writer PercentEncodes
    // both); this decodes them. An unparseable entry is skipped, never guessed - the
    // same contract as ApplyMaterialOverride.
    void ParseMaterialOverrideKV(const std::unordered_map<std::string, std::string>& kv,
                                 MaterialOverride& out);

    // Apply a per-entity override bag onto an already-resolved material.
    // Unknown keys are skipped silently and on purpose: the bag is string-keyed
    // so a shader losing a parameter degrades instead of failing to load.
    void ApplyMaterialOverride(MaterialComponent& mat, const MaterialOverride& ovr);

    // The INVERSE: diff a live MaterialComponent against the material file it came
    // from and fill @p out with every difference. False (and @p out empty) when
    // @p sourcePath cannot be loaded - do NOT read that as "no differences": a diff
    // against a default-constructed material is a full ~50-key snapshot, not an
    // override.
    //
    // Shared by the editor's "Save as Override" and the scene / prefab SAVE path, which
    // needs it to keep unsaved edits on entities that already have a
    // MaterialSourcePath (every .iscn-spawned mesh part).
    bool BuildMaterialOverride(const MaterialComponent& mat,
                               const std::string&       sourcePath,
                               MaterialOverride&        out);

    // Re-resolve from disk every entity whose material depends on @p changedPath,
    // including instances inheriting from it at any depth. Returns how many entities
    // were re-materialized.
    //
    // SCOPED BY DESIGN: each reload calls SetDirty(), which advances m_sceneMatDirty,
    // and the renderer's retained-instance gate needs that set empty; reloading the
    // world on every file touch would switch off the candidate cache and the DDGI / RT
    // retained gather. Drive it from save / watcher events only, never from a polling
    // tick.
    //
    // Calls Resource::LoadMaterial directly: the MaterialRef deserializer
    // short-circuits when the entity's MaterialComponent already has a matching source
    // path, which is exactly this case.
    //
    // @p live: a Material Editor's live push (Resource/MaterialLiveOverlay.h), up to 10
    // a second while a control is dragged, so no log line; an entity with unsaved edits
    // of its own (ASSET_DIRTY) is SKIPPED - re-materializing would drop them - and takes
    // the file's look when the file is saved.
    int ReloadMaterialsAffectedBy(World& world, const std::string& changedPath, bool live = false);
}
