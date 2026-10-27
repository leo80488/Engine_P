#pragma once
// MeshAssetRegistry: (model, MeshKey) -> the library and sections to draw.
//
// A model is its main .iscn. Its F-lines name the .meshlib(s) holding its geometry and each
// section's default material; the libraries' identity layer (MeshLibrary::GetIdentity - the
// file's own, or one synthesized for a library without one) names the Meshes. Identity and
// storage stay apart: a reference is always (model, key); where the geometry lives is the
// model's business.
//
// Every library is loaded through MeshLibrary and handed to the Renderer as a world library
// (TrackWorldMeshLibrary), as a .iscn spawn does, so it is released at the world clear and
// never by a caller. Views are cached until the world epoch changes (a clear, or Play/Stop's
// AdoptState).
//
// Main thread only: MeshInstanceSystem (GameplayPostLogic) and the editor.

#include "Resource/SystemHandles.h"

#include <DirectXMath.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Resource { class MeshLibrary; }
class IGraphicsDevice;
class Renderer;

class MeshAssetRegistry
{
public:
    struct Section
    {
        uint32_t          entry = 0;          // meshId in the Mesh's library
        uint64_t          key   = 0;          // SectionKey
        std::string       slot;               // material slot name
        std::string       defaultMaterial;    // project-relative, from the model's F-line; "" = none
        DirectX::XMFLOAT3 aabbMin{ 0, 0, 0 };
        DirectX::XMFLOAT3 aabbMax{ 0, 0, 0 };
    };
    struct Mesh
    {
        uint64_t             key   = 0;
        std::string          name;
        uint32_t             flags = 0;       // Resource::MESH_IDENT_*
        std::vector<Section> sections;
        std::string          libraryPath;     // spelled as the loader spells it (MeshSourcePath)
        Resource::Handle     library{};
        DirectX::XMFLOAT3    aabbMin{ 0, 0, 0 };
        DirectX::XMFLOAT3    aabbMax{ 0, 0, 0 };
        bool                 synthesized = false;   // the library has no identity of its own yet
        // It moves with the model's skeleton (Resource::MeshCountsAsSkinned: the
        // identity's flag; for an index-derived identity, the model naming one).
        bool                 skinned = false;
    };
    struct Model
    {
        std::string       path;               // canonical (Resource::MeshLibCanonicalPath)
        std::vector<Mesh> meshes;             // every Mesh of every library the model names
        std::string       error;              // non-empty: some or all of it could not be read
        // The model names a skeleton (a K line; SceneInstanceLoader::NamesSkeleton).
        // NOT "its meshes are skinned" - that is Mesh::skinned, per Mesh.
        bool              namesSkeleton = false;
    };

    using LibraryLoader = std::function<Resource::Handle(const std::string& meshlibPath)>;

    MeshAssetRegistry(Resource::MeshLibrary& library, IGraphicsDevice& gfx, Renderer* renderer)
        : m_library(library), m_gfx(gfx), m_renderer(renderer) {}

    // The model's meshes, its libraries loaded. Never null; a model that could not be read comes
    // back with `error` set and no meshes (and is not re-read until the epoch changes).
    const Model& GetModel(const std::string& modelPath, uint32_t worldEpoch);

    // The Mesh with @p key, else (a key the model no longer has) the first Mesh named @p name
    // (*outByName = true). nullptr when neither.
    static const Mesh* Find(const Model& model, uint64_t key, const std::string& name,
                            bool* outByName = nullptr);

    // What GetModel caches, built from the files with @p loadLibrary loading each library the
    // model names (an invalid handle = it did not load). The registry passes its world-library
    // loader; the asset preview passes its own, so a Mesh item's thumbnail is composed from the
    // same view a StaticMesh of it draws without making its library a world library.
    static void BuildModel(const std::string& modelPath, Resource::MeshLibrary& library,
                           const LibraryLoader& loadLibrary, Model& out);

    // Forget every view (the libraries stay tracked by the Renderer).
    void Clear();

    // The model's file changed in place (the Mesh Editor saved new default materials): drop its
    // view so the next GetModel re-reads it, and bump its revision so MeshInstanceSystem
    // re-derives every StaticMesh of it - their authored fields did not change, so their
    // signature alone would not notice.
    void InvalidateModel(const std::string& modelPath);
    // 0 for a model never invalidated this session (the common case: no map lookup at all while
    // nothing was).
    uint32_t ModelRevision(const std::string& modelPath) const;

private:
    Resource::Handle LoadLibrary(const std::string& path);

    Resource::MeshLibrary& m_library;
    IGraphicsDevice&       m_gfx;
    Renderer*              m_renderer = nullptr;
    uint32_t               m_epoch    = 0xFFFFFFFFu;
    std::unordered_map<std::string, std::unique_ptr<Model>> m_models;   // canonical model path
    std::unordered_map<std::string, Resource::Handle>        m_libs;     // canonical library path
    std::unordered_map<std::string, uint32_t>                m_revisions; // canonical model path
};
