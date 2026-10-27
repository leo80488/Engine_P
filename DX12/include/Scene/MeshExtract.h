#pragma once
// MeshExtract: writes one Mesh of a model out as a model of its own (the editor's
// "Extract Mesh...", like Godot's "Save to File").
//
// The new model is a `.meshlib` holding only the Mesh's sections - vertex ranges copied byte
// for byte (every stream the source library has), indices re-based, entries renumbered
// 0..n-1 - with an IDNT tail that keeps the Mesh's key and every SectionKey, plus a `.iscn`
// with one node placing them at the identity with the materials the source model gives them.
// So (newModel, sameKey) draws exactly what (model, key) drew: a StaticMesh is re-pointed at
// the new model by its path alone.
//
// Pure file work (AssetFS reads, Sys writes): no device, no MeshLibrary, no World. The editor
// stamps the two .meta sidecars and re-points references. Skinned and morph-carrying Meshes
// are refused: their .iskel / .imorph data is keyed by the source model's F index, and a
// StaticMesh draws the bind pose anyway.
// TODO(Mesh_Asset_Pipeline_Plan M11): a SkinnedMeshComponent for skinned / morph Meshes to
// be extracted into.

#include <cstdint>
#include <string>

namespace MeshExtract
{
    struct Result
    {
        std::string iscn;          // written, project-relative spelling of the destination
        std::string meshlib;       // beside it, same stem
        std::string meshName;
        uint64_t    key      = 0;
        uint32_t    sections = 0, vertices = 0, indices = 0;
        // On failure: nothing was written (a half-written pair is removed).
        std::string code;          // bad_value | not_found | unsupported | exists | load_failed | write_failed
        std::string error;
    };

    // @p destIscn names the new model's `.iscn`; its `.meshlib` is the same path
    // with that extension. An existing file is refused unless @p overwrite.
    bool Extract(const std::string& model, uint64_t meshKey, const std::string& destIscn,
                 bool overwrite, Result& out);

    // "<model dir>/<mesh name>.iscn", then "_1", "_2", ... past files that exist
    // (either half of the pair). The name is made file-safe.
    std::string DefaultDestination(const std::string& model, const std::string& meshName);
}
