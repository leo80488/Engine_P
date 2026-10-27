#pragma once
// Resource/ModelCollision.h - a model's ASSET collision: the `BO` / `PR` records of its main .iscn,
// keyed by MeshKey.
//
//   BO mesh=<MeshKey> complexity=simple|complex|none [layer=<n>] [trig=1]
//      [friction=<f>] [restitution=<f>] [src=<names|box|convex|complex>] [orphan=1]
//   PR mesh=<MeshKey> type=box     c=x_y_z he=x_y_z [q=x_y_z_w] [name=<pct>]
//   PR mesh=<MeshKey> type=sphere  c=x_y_z r=<f>
//   PR mesh=<MeshKey> type=capsule c=x_y_z r=<f> hh=<f> [q=x_y_z_w]
//   PR mesh=<MeshKey> type=hull    lib=<pct> sec=<SectionKey> [c=x_y_z] [q=x_y_z_w]
//   PR mesh=<MeshKey> type=mesh    [lib=<pct>] sec=<SectionKey>
//
// Two-letter prefixes, so a loader that does not know them skips them. Every value is one token
// (percent-encoded); vectors are `x_y_z` like the C / V records and the component serializers.
// `lib=` is RELATIVE TO THE MODEL'S FOLDER.
//
// Runtime meaning (SceneInstanceLoader and MeshInstanceSystem apply the rules):
//   * the BODY prims - box / sphere / capsule / hull, and a `mesh` prim naming another file
//     (`lib=`) - are ONE body per placed Mesh: a ColliderComponent{MeshAsset} whose shape
//     PhysicsSystem builds from these records (a StaticCompoundShape when there are several);
//   * a `mesh` prim WITHOUT `lib=` is that section of the model's own library, its triangles a
//     trimesh on the entity that draws the section (one body per section, like the per-section
//     collider of a .iscn C record);
//   * complexity=complex = every section of the Mesh that way;
//   * complexity=none    = no collision (the prims, if any, are kept for editing).

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource
{
    struct CollisionPrim
    {
        enum class Type : uint8_t { Box, Sphere, Capsule, Hull, Mesh };
        Type              type        = Type::Box;
        std::string       name;
        DirectX::XMFLOAT3 center      = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 rotation    = { 0.f, 0.f, 0.f, 1.f };   // quaternion xyzw
        DirectX::XMFLOAT3 halfExtents = { 0.5f, 0.5f, 0.5f };      // box
        float             radius      = 0.5f;                      // sphere / capsule
        float             halfHeight  = 0.5f;                      // capsule (cylinder half-height)
        std::string       lib;          // hull / mesh: as WRITTEN (model-relative); "" = the Mesh's own library
        uint64_t          sectionKey  = 0;                          // hull / mesh

        // Part of the Mesh's single body (everything but a section of its own library).
        bool InBody() const { return !(type == Type::Mesh && lib.empty()); }
    };

    struct MeshBodySetup
    {
        enum class Complexity : uint8_t { Simple, Complex, None };
        uint64_t                   meshKey     = 0;
        Complexity                 complexity  = Complexity::Simple;
        uint8_t                    layer       = 0;       // ColliderComponent::CollisionLayer raw (0 = Auto)
        bool                       trigger     = false;
        float                      friction    = -1.f;    // < 0: the RigidBody default
        float                      restitution = -1.f;
        // BO's orphan=1: kept by a re-import whose library lacks the Mesh - inert (no Mesh has the key),
        // kept for the author; a later re-import that brings the Mesh back clears it.
        bool                       orphan      = false;
        // BO's src=: who wrote the records - "" the AUTHOR (the Mesh Editor, the bake: kept by a
        // re-import), "names" the source's collision nodes, "box" / "convex" / "complex" the import's
        // collision option. A re-import replaces every src= record with what the source and the options
        // give now; an edit makes a record the author's.
        std::string                source;
        std::vector<CollisionPrim> prims;

        bool HasBody() const;                             // a body prim, and complexity is not none
        // No prims and every field at its default: the same as no records at all
        // (SceneInstanceLoader::ReplaceModelCollision writes none for it).
        bool Empty() const
        {
            return prims.empty() && complexity == Complexity::Simple && layer == 0 && !trigger &&
                   friction < 0.f && restitution < 0.f;
        }
        // The own-library sections that get a trimesh of their own: complexity=complex
        // -> every section (@p allSections), else the `mesh` prims without `lib=`.
        std::vector<uint64_t> SectionTrimeshes(const std::vector<uint64_t>& allSections) const;
    };

    struct ModelCollision
    {
        std::unordered_map<uint64_t, MeshBodySetup> meshes;   // by MeshKey
        const MeshBodySetup* Find(uint64_t meshKey) const
        {
            const auto it = meshes.find(meshKey);
            return it == meshes.end() ? nullptr : &it->second;
        }
    };

    // The BO / PR records of a model file's TEXT (the payload after the header).
    // A PR before its BO, or with no BO at all, gets a default one (simple). A
    // malformed record is skipped and described in @p warnings (optional).
    void ParseModelCollision(const std::string& text, ModelCollision& out,
                             std::vector<std::string>* warnings = nullptr);

    // The model file's asset collision, read through AssetFS and cached per path
    // (re-read when the loose file's size / write time changes). Never null; empty
    // when the model has none or does not read. Thread-safe.
    std::shared_ptr<const ModelCollision> ModelCollisionAt(const std::string& iscnPath);

    // A `lib=` value (model-relative, as written) -> the project-relative path the
    // loaders and PhysicsSystem open.
    std::string ResolveModelRelative(const std::string& iscnPath, const std::string& lib);

    // The text of one setup: its BO line and its PR lines, each ending in "\n".
    std::string FormatBodySetup(const MeshBodySetup& setup);
}
