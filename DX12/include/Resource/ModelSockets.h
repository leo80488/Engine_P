#pragma once
// Resource/ModelSockets.h - a model's SOCKET records: the `SO` lines of its main .iscn.
//   SO name=<pct> bone=<pct> t=x,y,z q=x,y,z,w s=x,y,z [src=<importer>] [orphan=1]
//      a skeleton socket: a named attachment point on a bone of the model's skeleton (its K line),
//      by the bone's NAME - a re-import that reorders the bones keeps it;
//   SO mesh=<MeshKey> name=<pct> t=x,y,z q=x,y,z,w s=x,y,z [src=] [orphan=1]
//      a mesh socket: relative to one Mesh of the model, in the Mesh's own space (the Mesh
//      Editor's picture). DERIVED onto a StaticMesh owner that places that Mesh
//      (MeshInstanceSystem, SceneInstanceLoader::DeriveMeshSockets), placed at offset x the
//      owner's world; the Mesh Editor edits them. A model spawned whole (a SceneRef) does not
//      publish them.
// t / q / s are the socket's offset in the BONE's local space - exactly
// SocketComponent::Socket::localOffset (world = offset x boneModel x rootWorld), composed S x R x T
// (row vectors). Every value is one token, names percent-encoded (PercentEncodeStrict): a bone name
// may hold spaces. Two-letter prefix, so a loader that does not know it skips the line.
// src=<importer>: the SOURCE's, which a re-import drops (no importer reports sockets); without it
// the AUTHOR's (the Skeleton Editor's), which a re-import keeps - orphan=1 when the new skeleton has
// no such bone (for a mesh socket, the new library no such Mesh).
// At spawn the skeleton sockets are DERIVED into the skeleton root's SocketComponent
// (SceneInstanceLoader::DeriveModelSockets), never saved with it.

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Resource
{
    struct ModelSocket
    {
        std::string       name;
        std::string       bone;                           // a skeleton socket's bone ("" for a mesh socket)
        uint64_t          meshKey = 0;                    // a mesh socket's Mesh (0 = a skeleton socket)
        DirectX::XMFLOAT3 t = { 0.f, 0.f, 0.f };
        DirectX::XMFLOAT4 q = { 0.f, 0.f, 0.f, 1.f };
        DirectX::XMFLOAT3 s = { 1.f, 1.f, 1.f };
        std::string       source;                         // "" = authored; else the importer (src=)
        bool              orphan = false;                 // kept by a re-import that lost its bone / Mesh

        // The offset in the bone's local space: S x R x T (row vectors).
        DirectX::XMFLOAT4X4 LocalOffset() const;
        // t / q / s of an affine @p m (false - and unchanged - when it does not decompose).
        bool SetFromOffset(const DirectX::XMFLOAT4X4& m);
        bool IsSkeletonSocket() const { return meshKey == 0; }
        bool operator==(const ModelSocket& o) const;      // every field, floats exactly
        bool operator!=(const ModelSocket& o) const { return !(*this == o); }
    };

    struct ModelSockets
    {
        std::vector<ModelSocket> sockets;                 // in file order
        // A skeleton socket by name (exact, case-sensitive - as Engine.EquipToSocket asks).
        const ModelSocket* Find(const std::string& name) const;
        std::size_t SkeletonSocketCount() const;
    };

    // The SO records of a model file's TEXT. A malformed record is skipped and
    // described in @p warnings (optional); a repeated skeleton socket name keeps
    // the LAST record.
    void ParseModelSockets(const std::string& text, ModelSockets& out, std::vector<std::string>* warnings = nullptr);

    // The model file's records, read through AssetFS and cached per path (re-read
    // when the loose file's size / write time changes). Never null. Thread-safe.
    std::shared_ptr<const ModelSockets> ModelSocketsAt(const std::string& iscnPath);

    // One record, ending in "\n" ("" for a nameless socket).
    std::string FormatSocket(const ModelSocket& s);
    std::string FormatSockets(const std::vector<ModelSocket>& list);

    // A socket name that is legal (not empty, at most 63 bytes - SocketComponent's
    // name[64]) - "" = fine, else why not.
    std::string SocketNameProblem(const std::string& name);
}
