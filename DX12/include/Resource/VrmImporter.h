#pragma once

// VRM (0.x and 1.0) model importer.
//
// VRM is a GLB (binary glTF 2.0) container whose JSON `extensions` block describes the
// humanoid bone map, expression presets, meta info and optional spring-bone / first-person /
// MToon data. Mesh, skeleton, materials and animations come from SceneImporter (Assimp reads
// glTF/GLB), so the output set (.meshlib, .iscn, .iskel, .imorph, .imat) matches a plain GLB
// import and SceneInstanceLoader loads it unchanged. Only the VRM extension JSON is parsed here:
//   - humanoid bones -> SceneImporter's HumanoidHint: the .iskel's HumanoidProfile (role -> bone)
//     the runtime retargets with;
//   - expressions -> SceneImporter's PresetHints: the model's `MP` preset records (src=vrm), one
//     per Mesh the expression moves, keyed by target name - what Face.SetExpression plays;
//   - meta (title / author / license) -> the import log.
// Out of scope: SpringBone physics, the MToon material / outline shader, first-person masks.

#include "Resource/ModelImportWrite.h"   // ImportedShapeKeys / ImportedCollision

#include <cstdint>
#include <string>
#include <vector>

namespace Resource
{
    // ==== Sub-records of VrmImportResult ====
    struct VrmMeta
    {
        std::string title;
        std::string version;
        std::string author;
        std::string contactInformation;
        std::string reference;
        std::string licenseName;       // VRM 1.0 licenseUrl / VRM 0.x licenseName
        std::string allowedUserName;   // VRM 0.x consent fields, this and commercialUseName (empty for 1.0)
        std::string commercialUseName;
        std::string licenseTextUrl;    // VRM 1.0 thirdPartyLicenses / VRM 0.x otherLicenseUrl
        std::string thumbnailNodeName; // name of the thumbnail's glTF image (1.0) or texture (0.x); may be empty
    };

    struct VrmHumanoidBone
    {
        std::string humanoidName;  // canonical VRM name: "hips", "spine", "leftUpperArm", ...
        int32_t     nodeIndex = -1; // glTF node index (raw)
        std::string nodeName;       // glTF node name resolved from nodeIndex
    };

    struct VrmExpressionBinding
    {
        // One morph target on one mesh:
        //   meshIndex  - glTF mesh index (not the merged .meshlib entry); a VRM 1.0 bind names a node
        //                (nodeIndex) and this is that node's mesh
        //   morphIndex - index into that mesh's morphTargets
        //   weight     - 0..1 multiplier at full expression strength
        //   morphName  - the target's name (the mesh's extras.targetNames), as the model's .imorph
        //                calls it; "" when the source leaves it unnamed
        int32_t     meshIndex  = -1;
        int32_t     nodeIndex  = -1;
        int32_t     morphIndex = -1;
        float       weight     = 1.f;
        std::string morphName;
    };

    struct VrmExpression
    {
        std::string name;                              // preset name: "happy", "blink", ...
        bool        isPreset = true;                   // false -> custom expression (VRM 1.0 'custom')
        bool        isBinary = false;                  // true -> snap to 0/1 instead of interpolating
        std::vector<VrmExpressionBinding> morphBindings;
        uint32_t    materialBinds = 0;                 // material colour / UV binds (not imported)
    };

    // Output of VrmImporter::Import().
    struct VrmImportResult
    {
        // Detected spec version: 0 = VRM 0.x ("VRM" extension), 1 = VRM 1.0 ("VRMC_vrm").
        int specVersion = -1;

        VrmMeta                       meta;
        std::vector<VrmHumanoidBone>  humanoidBones;
        std::vector<VrmExpression>    expressions;

        bool success = false;
    };

    // One output file; same layout as SceneImporter / PmxImporter use.
    struct VrmExportedFile
    {
        std::string          relativePath;
        std::vector<uint8_t> blob;
    };

    class VrmImporter
    {
    public:
        // Parses the VRM extension data from the GLB JSON chunk in @p data / @p size.
        // Geometry is not decoded here; SceneImporter does that.
        static VrmImportResult Import(const uint8_t* data, size_t size);

        // Builds the full output file set through SceneImporter (.meshlib / .iscn / .iskel / .imorph /
        // .imat), passing the humanoid map and the expressions (as presets) as hints. @p sourcePath
        // gives SceneImporter the file stem and extension hint; @p sourceData is the raw GLB bytes.
        // @p outShapeKeys / @p outCollision (optional) receive what WriteModelImportFiles merges into the
        // .iscn; @p context goes to SceneImporter::Import.
        static std::vector<VrmExportedFile> BuildBlobs(const std::string&              sourcePath,
                                                       const std::vector<uint8_t>&     sourceData,
                                                       const VrmImportResult&          vrm,
                                                       std::vector<ImportedShapeKeys>* outShapeKeys = nullptr,
                                                       std::vector<ImportedCollision>* outCollision = nullptr,
                                                       const ModelImportContext*       context      = nullptr);

        static std::vector<const char*> GetSourceExtensions() { return { ".vrm" }; }
    };
}
