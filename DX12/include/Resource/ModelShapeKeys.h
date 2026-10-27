#pragma once
// Resource/ModelShapeKeys.h - a model's shape-key AUTHORING records: the `MK` / `MP` records of its
// main .iscn, keyed by MeshKey.
//   MK mesh=<MeshKey> name=<pct> [cat=expression|viseme|body|corrective]
//      [default=<f>] [min=<f>] [max=<f>] [label=<pct>] [src=<importer>] [orphan=1]
//   MP mesh=<MeshKey> name=<pct> w=<pct>:<f>,<pct>:<f>... [src=<importer>] [orphan=1]
// MK is ONE target's metadata. A target without one has no category, default 0, range 0..1 and its
// own name as the label, so only targets that differ get a record (plus an author's record with
// nothing else in it, which keeps a source's src= record for that target from coming back on a
// re-import). MP is a PRESET: a named set of weights over the Mesh's targets.
// Two-letter prefixes, so a loader that does not know them skips them. Every value is one token,
// percent-encoded (PercentEncodeStrict: a target name may hold spaces, ':' and ','). orphan=1: kept
// by a re-import whose source lacks that Mesh.
// src=<importer>: the record came from the SOURCE - "vrm" (a VRM expression), "pmx" (a PMX group
// morph), "gltf" (a glTF default weight) - so a re-import refreshes it from the source; without
// src= the record is the author's (the Mesh Editor's) and a re-import keeps it, winning over the
// source's by name.
// Weights are 0..1 everywhere (the runtime clamps there; Skin.cs skips <= 0).

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Resource
{
    struct ShapeKeyMeta
    {
        std::string name;               // the target, as the .imorph names it
        std::string category;           // "" or one of ShapeKeyCategories()
        float       def = 0.f;          // what the Mesh Editor's Reset returns to
        float       min = 0.f, max = 1.f;   // the slider's range, inside 0..1
        std::string label;              // "" = the name
        std::string source;             // "" = authored; else the importer that made it (src=)
        bool IsDefault() const
        {
            return category.empty() && def == 0.f && min == 0.f && max == 1.f && label.empty();
        }
        bool operator==(const ShapeKeyMeta&) const = default;
    };

    struct ShapeKeyPreset
    {
        std::string                                name;
        std::vector<std::pair<std::string, float>> weights;   // target -> weight, as authored
        std::string                                source;    // "" = authored; else the importer (src=)
        bool operator==(const ShapeKeyPreset&) const = default;
    };

    struct MeshShapeKeySetup
    {
        uint64_t                    meshKey = 0;
        std::vector<ShapeKeyMeta>   meta;      // only the targets whose metadata is not the default
        std::vector<ShapeKeyPreset> presets;
        bool                        orphan = false;
        bool Empty() const { return meta.empty() && presets.empty(); }
        const ShapeKeyMeta*   FindMeta(const std::string& name) const;
        const ShapeKeyPreset* FindPreset(const std::string& name) const;
        bool operator==(const MeshShapeKeySetup&) const = default;
    };

    struct ModelShapeKeys
    {
        std::unordered_map<uint64_t, MeshShapeKeySetup> meshes;   // by MeshKey
        const MeshShapeKeySetup* Find(uint64_t meshKey) const
        {
            const auto it = meshes.find(meshKey);
            return it == meshes.end() ? nullptr : &it->second;
        }
    };

    // "expression", "viseme", "body", "corrective".
    const std::vector<std::string>& ShapeKeyCategories();
    bool IsShapeKeyCategory(const std::string& c);   // "" or one of them

    // The MK / MP records of a model file's TEXT. A malformed record is skipped
    // and described in @p warnings (optional).
    void ParseModelShapeKeys(const std::string& text, ModelShapeKeys& out,
                             std::vector<std::string>* warnings = nullptr);

    // The model file's records, read through AssetFS and cached per path (re-read
    // when the loose file's size / write time changes). Never null. Thread-safe.
    std::shared_ptr<const ModelShapeKeys> ModelShapeKeysAt(const std::string& iscnPath);

    // The text of one setup: its MK lines, then its MP lines, each ending in "\n".
    std::string FormatShapeKeys(const MeshShapeKeySetup& setup);
}
