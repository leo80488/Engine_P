#pragma once

// JSON persistence for DecalMaterialAsset (.idecalmat). JSON rather than the .imat
// binary+text container: about ten fields, hand-diffable, easy for tools.
// Schema (v2; LoadDecalMaterial also reads v1):
// {
//   "version": 2,
//   "name": "blood_splat_a",
//   "textures": { "baseColor": "asset/Decals/blood_albedo.itex", "normal": "..." },
//   "baseColorTint": [1.0, 0.25, 0.15, 1.0],
//   "scalars": { "opacity": 1.0, "roughness": 0.5, "specular": 0.5, "ao": 1.0,
//                "normalStrength": 1.0, "bumpStrength": 0.1, "cavityStrength": 1.0,
//                "displacementScale": 0.0 },
//   "flags": 1,                 // WRITE_* bitmask
//   "normalBlendMode": "RNM",   // or "Lerp"
//   "angleFadeStart": 0.3,
//   "sortLayer": 0
// }
// "textures" holds the non-empty slots only, keyed by DecalMaterialAsset::SlotKey().

#include <string>

namespace Resource
{
    class DecalMaterialAsset;

    // Write @p asset to @p path as JSON. Returns false on I/O error.
    bool SaveDecalMaterial(const DecalMaterialAsset& asset, const std::string& path);

    // Fills the editor-owned fields of @p asset from the JSON file at @p path and resets its
    // runtime caches (handles, bindless indices) for the next Resolve() / TryPromote(). The
    // handles are dropped, not released: load into a fresh asset. Returns false on an I/O,
    // parse or version error, leaving the asset unmodified.
    bool LoadDecalMaterial(DecalMaterialAsset& asset, const std::string& path);
}
