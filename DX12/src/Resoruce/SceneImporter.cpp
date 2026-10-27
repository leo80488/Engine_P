#include "Resource/SceneImporter.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/MeshLibIdentity.h"   // IDNT - the .meshlib's Mesh/Section keys
#include "Resource/MorphLibrary.h"      // .imorph v2 (MAGIC_MORPHLIB2)
#include "Resource/ModelMaterialImport.h"   // the source's materials and textures
#include "Resource/SkeletonImporter.h"
#include "Physics/CollisionFit.h"      // a collision node's shape
#include "Resource/SkeletonAsset.h"
#include "Resource/HumanoidProfileBuilder.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/AssetRefs.h"   // PercentEncodeStrict - .iscn F-line name=
#include "ECS/Components.h"                // MaterialComponent
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"              // UTF-8 path stem/extension (non-ASCII source names)

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>
#include <assimp/IOSystem.hpp>
#include <assimp/IOStream.hpp>

#ifdef _DEBUG
#pragma comment(lib, "assimp-vc143-mtd.lib")
#else
#pragma comment(lib, "assimp-vc143-mt.lib")
#endif

#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace DirectX;

namespace Resource
{
    // PackedVertex: the 48-byte base vertex with tangent - pos float3 @0, normal float3 @12, uv0 float2
    // @24, tangent float4 @32 (xyz = direction, w = bitangent handedness +-1). BuildMeshLibBlob writes the
    // flag-derived layout (ComputeMeshLibVertexLayout): this base plus the optional uv1 / uv2 / uv3 /
    // color streams, always with MESHLIB_FLAG_HAS_TANGENT. A .meshlib without that flag has 32-byte
    // vertices; the loader accepts it and GBuffer.vs synthesises the TBN.
    // Keep these structs in the anonymous namespace: PmxImporter.cpp and SceneInstanceLoader.cpp define a
    // 32-byte PackedVertex of their own, and with external linkage ICF / COMDAT folding merges
    // vector<PackedVertex> across TUs onto one sizeof - reserve(N) then allocates N x 32 B while
    // push_back writes 48 B structs and overruns the buffer.
    namespace {
        struct PackedVertex
        {
            float px, py, pz;     // position    (12 B)
            float nx, ny, nz;     // normal      (12 B)
            float u,  v;          // UV0         ( 8 B)
            float tx, ty, tz, tw; // tangent xyz + handedness  (16 B)
        };
        static_assert(sizeof(PackedVertex) == 48, "PackedVertex must be 48 bytes");
    } // anonymous

    // Sanitise a node / mesh name for filenames and the .iscn text: spaces, slashes and ':' become '_',
    // ASCII alphanumerics and '-' '.' '_' are kept, other ASCII is dropped, and bytes >= 0x80 pass
    // through unchanged (UTF-8 sequences of non-Latin names). An empty result becomes "node".
    static std::string SanitiseName(const std::string& raw)
    {
        std::string out;
        out.reserve(raw.size());
        for (unsigned char c : raw)
        {
            if (c == ' ' || c == '/' || c == '\\' || c == ':')
                out += '_';
            else if (c > 127)
                out += static_cast<char>(c);  // UTF-8 multi-byte sequence byte
            else if (std::isalnum(c) || c == '-' || c == '.' || c == '_')
                out += static_cast<char>(c);
            // drop other ASCII control / special chars
        }
        return out.empty() ? "node" : out;
    }

    // Writes [AssetHeader][MeshLibraryMetadata][MeshLibraryEntry x N][vertices x totalVerts, the flags'
    // stride][uint32 x totalIndices], one entry (vertex / index range, AABB, defaultMaterialIdx) per
    // source; the contract is at the declaration.
    std::vector<uint8_t> SceneImporter::BuildMeshLibBlob(
        const std::vector<const aiMesh*>& sources,
        const std::vector<uint32_t>&      aiMaterialIdx,
        bool                              flipX,
        const std::vector<uint8_t>*       mirrorAxes,
        const std::vector<XMFLOAT4X4>*    preTransforms)
    {
        // A short aiMaterialIdx would be read past its end. An empty blob is what both callers treat as a
        // failed build.
        if (!ENGINE_CHECK(sources.size() == aiMaterialIdx.size(),
                "SceneImporter::BuildMeshLibBlob: %zu source meshes but %zu material indices",
                sources.size(), aiMaterialIdx.size()))
            return {};
        if (sources.empty()) return {};

        uint32_t totalVerts = 0, totalIdx = 0;
        for (const aiMesh* ai : sources)
        {
            totalVerts += ai ? ai->mNumVertices   : 0u;
            totalIdx   += ai ? ai->mNumFaces * 3u : 0u;
        }
        if (totalVerts == 0 || totalIdx == 0) return {};

        // Library-wide vertex layout: the shared VB has ONE interleaved layout, so a stream is included for
        // the whole library if ANY source carries it. Tangent is always present (aiProcess_CalcTangentSpace
        // fills it, else a (1,0,0,1) sentinel); uv1 / uv2 / uv3 and vertex color follow the sources.
        bool anyUV1 = false, anyUV2 = false, anyUV3 = false, anyColor = false;
        for (const aiMesh* ai : sources)
        {
            if (!ai) continue;
            if (ai->HasTextureCoords(1)) anyUV1   = true;
            if (ai->HasTextureCoords(2)) anyUV2   = true;
            if (ai->HasTextureCoords(3)) anyUV3   = true;
            if (ai->HasVertexColors(0))  anyColor = true;
        }

        uint32_t libFlags = MESHLIB_FLAG_HAS_TANGENT;
        if (anyUV1)   libFlags |= MESHLIB_FLAG_HAS_UV1;
        if (anyUV2)   libFlags |= MESHLIB_FLAG_HAS_UV2;
        if (anyUV3)   libFlags |= MESHLIB_FLAG_HAS_UV3;
        if (anyColor) libFlags |= MESHLIB_FLAG_HAS_COLOR;
        const MeshLibVertexLayout L = ComputeMeshLibVertexLayout(libFlags);

        std::vector<uint8_t>          vbytes(size_t(totalVerts) * L.stride, 0u);
        std::vector<uint32_t>         indices; indices.reserve(totalIdx);
        std::vector<MeshLibraryEntry> entries; entries.reserve(sources.size());

        // Interleaved byte-writer into the shared VB.
        auto wF = [](uint8_t* p, float f) { std::memcpy(p, &f, sizeof(float)); };

        uint32_t vOffset = 0;  // running base vertex in the library-wide pool
        uint32_t iOffset = 0;  // running index offset (= entries.back().indexStart)
        uint32_t maxUVSets = 0;  // diagnostic only
        for (size_t s = 0; s < sources.size(); ++s)
        {
            const aiMesh* ai = sources[s];
            if (!ai || ai->mNumVertices == 0 || ai->mNumFaces == 0) continue;

            const bool hasNor   = ai->HasNormals();
            const bool hasUV    = ai->HasTextureCoords(0);
            const bool hasUV1   = ai->HasTextureCoords(1);
            const bool hasUV2   = ai->HasTextureCoords(2);
            const bool hasUV3   = ai->HasTextureCoords(3);
            const bool hasTan   = ai->HasTangentsAndBitangents();
            const bool hasColor = ai->HasVertexColors(0);
            if (ai->GetNumUVChannels() > maxUVSets) maxUVSets = ai->GetNumUVChannels();

            const uint32_t vStart = vOffset;
            const uint32_t iStart = iOffset;

            // Per-source axis mirror. An odd number of mirrored axes has a negative determinant, which reverses
            // the triangle winding and flips tangent handedness - both are compensated below so the mirrored
            // copy still front-faces like the original.
            uint8_t axes = flipX ? 0x1u : 0x0u;
            if (mirrorAxes && s < mirrorAxes->size()) axes |= (*mirrorAxes)[s];
            const float mirX = (axes & 0x1u) ? -1.f : 1.f;
            const float mirY = (axes & 0x2u) ? -1.f : 1.f;
            const float mirZ = (axes & 0x4u) ? -1.f : 1.f;

            // Per-source pre-transform (the mesh node's model-space matrix for
            // a skinned mesh). Applied BEFORE the mirror. Detect identity once
            // so the overwhelmingly common static case costs a branch, not a
            // matrix multiply per vertex.
            XMMATRIX pre = XMMatrixIdentity();
            bool preIsIdentity = true;
            float preDet = 1.f;
            if (preTransforms && s < preTransforms->size())
            {
                const XMFLOAT4X4& P = (*preTransforms)[s];
                XMFLOAT4X4 I; XMStoreFloat4x4(&I, XMMatrixIdentity());
                for (int k = 0; k < 16 && preIsIdentity; ++k)
                    if (std::fabs((&P._11)[k] - (&I._11)[k]) > 1e-6f) preIsIdentity = false;
                if (!preIsIdentity)
                {
                    pre = XMLoadFloat4x4(&P);
                    preDet = P._11 * (P._22 * P._33 - P._23 * P._32)
                           - P._12 * (P._21 * P._33 - P._23 * P._31)
                           + P._13 * (P._21 * P._32 - P._22 * P._31);
                }
            }
            // Winding reverses once per determinant-negative stage; two of them
            // cancel, which is why this is an XOR and not an OR.
            const bool  mirrorFlipsWinding =
                ((mirX * mirY * mirZ) < 0.f) != (preDet < 0.f);

            // Per-mesh AABB, accumulated while packing the vertices.
            float bmin[3] = {  FLT_MAX,  FLT_MAX,  FLT_MAX };
            float bmax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

            for (uint32_t i = 0; i < ai->mNumVertices; ++i)
            {
                uint8_t* vp = vbytes.data() + size_t(vOffset + i) * L.stride;

                float px = ai->mVertices[i].x;
                float py = ai->mVertices[i].y;
                float pz = ai->mVertices[i].z;
                float nx = hasNor ? ai->mNormals[i].x : 0.f;
                float ny = hasNor ? ai->mNormals[i].y : 1.f;
                float nz = hasNor ? ai->mNormals[i].z : 0.f;
                float u  = hasUV  ? ai->mTextureCoords[0][i].x : 0.f;
                float vt = hasUV  ? ai->mTextureCoords[0][i].y : 0.f;

                // Tangent + handedness: Assimp gives T and B; the engine stores T.xyz and one sign, B =
                // cross(N, T) * sign, the sign saying whether the authored B agrees with cross(N, T). Absent: the
                // (1,0,0,1) sentinel, FETCH_TANGENT4's fallback, so GBuffer.vs synthesises a basis from N.
                float tx = 1.f, ty = 0.f, tz = 0.f, tw = 1.f;
                if (hasTan && hasNor)
                {
                    const aiVector3D& T = ai->mTangents[i];
                    const aiVector3D& B = ai->mBitangents[i];
                    const float NxT_x = ny * T.z - nz * T.y;
                    const float NxT_y = nz * T.x - nx * T.z;
                    const float NxT_z = nx * T.y - ny * T.x;
                    const float sign  = (NxT_x * B.x + NxT_y * B.y + NxT_z * B.z) < 0.f ? -1.f : 1.f;
                    tx = T.x; ty = T.y; tz = T.z; tw = sign;
                }

                if (!preIsIdentity)
                {
                    // Positions by the full affine; directions by the linear part only (no translation), then
                    // renormalised: a node matrix can carry scale, and a non-unit normal scales every lighting term.
                    XMFLOAT3 tmp;
                    XMStoreFloat3(&tmp, XMVector3TransformCoord(XMVectorSet(px, py, pz, 1.f), pre));
                    px = tmp.x; py = tmp.y; pz = tmp.z;
                    XMStoreFloat3(&tmp, XMVector3Normalize(
                        XMVector3TransformNormal(XMVectorSet(nx, ny, nz, 0.f), pre)));
                    nx = tmp.x; ny = tmp.y; nz = tmp.z;
                    XMStoreFloat3(&tmp, XMVector3Normalize(
                        XMVector3TransformNormal(XMVectorSet(tx, ty, tz, 0.f), pre)));
                    tx = tmp.x; ty = tmp.y; tz = tmp.z;
                    // Handedness is flipped ONCE below, by mirrorFlipsWinding,
                    // which already folds in this stage's determinant sign.
                }

                px *= mirX; py *= mirY; pz *= mirZ;
                nx *= mirX; ny *= mirY; nz *= mirZ;
                tx *= mirX; ty *= mirY; tz *= mirZ;
                // Handedness only inverts when the COMBINED (pre-transform +
                // mirror) determinant is negative; two negative stages are a
                // rotation and preserve it, same as two mirrored axes.
                if (mirrorFlipsWinding) tw = -tw;

                wF(vp + L.posOffset + 0, px);
                wF(vp + L.posOffset + 4, py);
                wF(vp + L.posOffset + 8, pz);
                wF(vp + L.normalOffset + 0, nx);
                wF(vp + L.normalOffset + 4, ny);
                wF(vp + L.normalOffset + 8, nz);
                wF(vp + L.uv0Offset + 0, u);
                wF(vp + L.uv0Offset + 4, vt);

                if (L.tangentOffset != 0xFFFFFFFFu)
                {
                    wF(vp + L.tangentOffset + 0,  tx);
                    wF(vp + L.tangentOffset + 4,  ty);
                    wF(vp + L.tangentOffset + 8,  tz);
                    wF(vp + L.tangentOffset + 12, tw);
                }
                if (L.uv1Offset != 0xFFFFFFFFu)
                {
                    // Default to uv0 when this particular mesh lacks this set,
                    // so a uv1-driven material still samples sane coordinates.
                    float u1 = hasUV1 ? ai->mTextureCoords[1][i].x : u;
                    float v1 = hasUV1 ? ai->mTextureCoords[1][i].y : vt;
                    wF(vp + L.uv1Offset + 0, u1);
                    wF(vp + L.uv1Offset + 4, v1);
                }
                if (L.uv2Offset != 0xFFFFFFFFu)
                {
                    // Default to uv0 when this particular mesh lacks this set,
                    // so a uv2-driven material still samples sane coordinates.
                    float u1 = hasUV2 ? ai->mTextureCoords[2][i].x : u;
                    float v1 = hasUV2 ? ai->mTextureCoords[2][i].y : vt;
                    wF(vp + L.uv2Offset + 0, u1);
                    wF(vp + L.uv2Offset + 4, v1);
                }
                if (L.uv3Offset != 0xFFFFFFFFu)
                {
                    // Default to uv0 when this particular mesh lacks this set,
                    // so a uv3-driven material still samples sane coordinates.
                    float u1 = hasUV3 ? ai->mTextureCoords[3][i].x : u;
                    float v1 = hasUV3 ? ai->mTextureCoords[3][i].y : vt;
                    wF(vp + L.uv3Offset + 0, u1);
                    wF(vp + L.uv3Offset + 4, v1);
                }
                if (L.colorOffset != 0xFFFFFFFFu)
                {
                    uint32_t packed = 0xFFFFFFFFu;  // opaque white default
                    if (hasColor)
                    {
                        const aiColor4D& c = ai->mColors[0][i];
                        packed = PackColorRGBA8(c.r, c.g, c.b, c.a);
                    }
                    std::memcpy(vp + L.colorOffset, &packed, sizeof(uint32_t));
                }

                if (px < bmin[0]) bmin[0] = px;
                if (py < bmin[1]) bmin[1] = py;
                if (pz < bmin[2]) bmin[2] = pz;
                if (px > bmax[0]) bmax[0] = px;
                if (py > bmax[1]) bmax[1] = py;
                if (pz > bmax[2]) bmax[2] = pz;
            }

            for (uint32_t f = 0; f < ai->mNumFaces; ++f)
            {
                const aiFace& face = ai->mFaces[f];
                for (unsigned j = 0; j < face.mNumIndices; ++j)
                    indices.push_back(face.mIndices[j] + vOffset);
            }
            // Winding flip for a determinant-negative pre-transform / mirror. Triangulate + SortByPType
            // guarantee a triangle list, so strides of 3 are safe.
            if (mirrorFlipsWinding)
            {
                for (size_t f = iStart; f + 2 < indices.size(); f += 3)
                    std::swap(indices[f + 1], indices[f + 2]);
            }

            MeshLibraryEntry entry{};
            entry.vertexStart        = vStart;
            entry.vertexCount        = ai->mNumVertices;
            entry.indexStart         = iStart;
            entry.indexCount         = static_cast<uint32_t>(indices.size()) - iStart;
            entry.aabbMin[0]         = bmin[0]; entry.aabbMin[1] = bmin[1]; entry.aabbMin[2] = bmin[2];
            entry.aabbMax[0]         = bmax[0]; entry.aabbMax[1] = bmax[1]; entry.aabbMax[2] = bmax[2];
            entry.defaultMaterialIdx = aiMaterialIdx[s];
            entry.flags              = 0;
            entries.push_back(entry);

            vOffset += ai->mNumVertices;
            iOffset  = static_cast<uint32_t>(indices.size());
        }

        // Trim to the vertices written (a source skipped after the up-front totalVerts tally leaves a gap;
        // callers pre-filter, so normally none): vertexCount, the VB byte length and MeshLibrary::Load's
        // index-blob offset must agree.
        vbytes.resize(size_t(vOffset) * L.stride);

        if (vbytes.empty() || indices.empty() || entries.empty()) return {};

        LOG_INFO("SceneImporter: meshlib layout stride=%u flags=0x%X [tangent%s uv1%s uv2%s uv3%s color%s], "
                 "max UV sets in source=%u",
                 L.stride, libFlags,
                 (libFlags & MESHLIB_FLAG_HAS_TANGENT) ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_UV1)     ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_UV2)     ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_UV3)     ? "+" : "-",
                 (libFlags & MESHLIB_FLAG_HAS_COLOR)   ? "+" : "-",
                 maxUVSets);
        if (maxUVSets > 4)
            LOG_WARNING("SceneImporter: source carries %u UV sets — only uv0 through uv3 are "
                        "preserved (extra sets dropped)", maxUVSets);

        const uint32_t entryBytes  = static_cast<uint32_t>(entries.size() * sizeof(MeshLibraryEntry));
        const uint32_t vbBytes     = static_cast<uint32_t>(vbytes.size());
        const uint32_t ibBytes     = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

        MeshLibraryMetadata meta{};
        meta.meshCount    = static_cast<uint32_t>(entries.size());
        meta.vertexCount  = vOffset;
        meta.indexCount   = static_cast<uint32_t>(indices.size());
        meta.vertexStride = static_cast<uint16_t>(L.stride);
        meta.indexStride  = 4;
        meta.flags        = libFlags;
        meta.reserved     = 0;

        AssetHeader header{};
        header.magic        = MAGIC_MESHLIB;
        header.version      = CurrentVersion(MAGIC_MESHLIB);
        header.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
        header.metadataSize = sizeof(MeshLibraryMetadata);
        header.dataSize     = entryBytes + vbBytes + ibBytes;

        const size_t total = sizeof(AssetHeader) + sizeof(MeshLibraryMetadata)
                           + entryBytes + vbBytes + ibBytes;
        std::vector<uint8_t> blob(total);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header,        sizeof(AssetHeader));         dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,          sizeof(MeshLibraryMetadata)); dst += sizeof(MeshLibraryMetadata);
        std::memcpy(dst, entries.data(), entryBytes);                  dst += entryBytes;
        std::memcpy(dst, vbytes.data(),  vbBytes);                      dst += vbBytes;
        std::memcpy(dst, indices.data(), ibBytes);
        return blob;
    }

    // A node of the .iscn payload.
    struct NodeRecord
    {
        int         index       = -1;
        int         parent      = -1;
        std::string name;           // sanitised
        std::string rawName;        // as the source file spells it - the identity layer's Mesh name
        float tx, ty, tz;           // translation
        float qx, qy, qz, qw;      // rotation quaternion
        float sx, sy, sz;           // scale
        std::vector<int> meshFileIndices;   // raw aiMesh indices, F-line indices after Import's remap
    };

    // Append a NodeRecord for every node under @p root in DFS pre-order (index = its position in
    // @p records, @p rootParentIdx as the root's parent), with the decomposed local TRS and the raw
    // aiMesh indices.
    static void CollectNodes(const aiNode*              root,
                             int                        rootParentIdx,
                             std::vector<NodeRecord>&   records)
    {
        // Iterative pre-order DFS: some rigs export very deep single-child bone chains, and one call frame
        // per level would overflow the stack. Children are pushed in REVERSE so they pop in ascending order;
        // idx = records.size() at the visit, so every parent= is a strictly smaller idx.
        std::vector<std::pair<const aiNode*, int>> stack;
        stack.emplace_back(root, rootParentIdx);
        while (!stack.empty())
        {
            const aiNode* node      = stack.back().first;
            const int     parentIdx = stack.back().second;
            stack.pop_back();
            if (!node) continue;

            const int myIdx = static_cast<int>(records.size());
            records.emplace_back();
            NodeRecord& rec = records.back();
            rec.index  = myIdx;
            rec.parent = parentIdx;
            rec.rawName = node->mName.C_Str()[0] ? node->mName.C_Str() : "node";
            rec.name    = SanitiseName(rec.rawName);

            // Decompose the node's local matrix into TRS (Assimp: v' = M * v, column vectors, row-major
            // storage).
            aiVector3D   aiTrans, aiScale;
            aiQuaternion aiRot;
            node->mTransformation.Decompose(aiScale, aiRot, aiTrans);

            rec.tx = aiTrans.x; rec.ty = aiTrans.y; rec.tz = aiTrans.z;
            rec.qx = aiRot.x;   rec.qy = aiRot.y;   rec.qz = aiRot.z; rec.qw = aiRot.w;
            rec.sx = aiScale.x; rec.sy = aiScale.y; rec.sz = aiScale.z;

            // Raw aiMesh indices for now; Import remaps them to submesh (F-line) indices once the library is
            // built.
            for (unsigned m = 0; m < node->mNumMeshes; ++m)
                rec.meshFileIndices.push_back(static_cast<int>(node->mMeshes[m]));

            // Push children in reverse so the first child pops (and is indexed) first.
            for (unsigned c = node->mNumChildren; c-- > 0; )
                stack.emplace_back(node->mChildren[c], myIdx);
        }
    }

    // An F-line record: one section of the model - MeshLibraryEntry subIdx of the .meshlib meshFile,
    // drawn with matFile.
    struct MeshFileRef
    {
        std::string meshFile;   // the .meshlib's filename (a sibling of the .iscn)
        std::string matFile;    // optional .imat
        // The material's RAW authored name, for the F-line `name=`. It is both
        // the mesh entity's label in the Outliner and the only key a re-import
        // can match old bindings by that survives adding/removing a material.
        std::string name;
        uint32_t    subIdx;     // the MeshLibraryEntry index (the F-line's mesh=)
    };

    static std::string BuildIscnText(const std::string&              sourceStem,
                                     const std::vector<NodeRecord>&   nodes,
                                     const std::vector<MeshFileRef>&  fileRefs,
                                     const std::string&               skelFileName = "",
                                     const std::vector<int>&          morphMeshFileIdx = {},
                                     const std::string&               morphFileName = "")
    {
        std::ostringstream ss;
        ss << "# ISCN engine scene container\n";
        ss << "S nodes=" << nodes.size()
           << " meshfiles=" << fileRefs.size()
           << " source=" << sourceStem << "\n";

        for (const NodeRecord& n : nodes)
        {
            ss << "N idx=" << n.index
               << " parent=" << n.parent
               << " tx=" << n.tx << " ty=" << n.ty << " tz=" << n.tz
               << " qx=" << n.qx << " qy=" << n.qy
               << " qz=" << n.qz << " qw=" << n.qw
               << " sx=" << n.sx << " sy=" << n.sy << " sz=" << n.sz
               << " mc=" << n.meshFileIndices.size();

            for (int k = 0; k < static_cast<int>(n.meshFileIndices.size()); ++k)
                ss << " mr" << k << "=" << n.meshFileIndices[k];

            ss << " name=" << n.name << "\n";
        }

        for (int i = 0; i < static_cast<int>(fileRefs.size()); ++i)
        {
            // mesh=K is the MeshLibraryEntry index inside the .meshlib that file= names.
            ss << "F idx=" << i << " file=" << fileRefs[i].meshFile
               << " mesh=" << fileRefs[i].subIdx;
            // name= MUST sit between mesh= and mat=: ParseFLine reads mat= to the end of the line (material
            // filenames may contain spaces), so anything after it is swallowed into the path and every material
            // fails to load. Percent-encoded because the value ends at the next space. Without name= a re-import
            // that adds or removes a material shifts every meshId and re-points each binding onto its neighbour.
            if (!fileRefs[i].name.empty())
                ss << " name=" << Resource::AssetRefs::PercentEncodeStrict(fileRefs[i].name);
            if (!fileRefs[i].matFile.empty())
                ss << " mat=" << fileRefs[i].matFile;
            ss << "\n";
        }

        if (!skelFileName.empty())
            ss << "K file=" << skelFileName << "\n";

        // M-lines: one per F-line index that has a morph block inside the single
        // combined .imorph. Same shape PmxImporter emits, and SceneInstanceLoader
        // reads `file=` to end of line, so nothing may follow it.
        if (!morphFileName.empty())
            for (int fi : morphMeshFileIdx)
                ss << "M idx=" << fi << " file=" << morphFileName << "\n";

        return ss.str();
    }

    // Build the .iscn blob: AssetHeader + SceneMetadata + the null-terminated text.
    static std::vector<uint8_t> BuildIscnBlob(const std::string&              sourceStem,
                                               const std::vector<NodeRecord>&  nodes,
                                               const std::vector<MeshFileRef>& fileRefs,
                                               const std::string&              skelFileName = "",
                                               const std::vector<int>&         morphMeshFileIdx = {},
                                               const std::string&              morphFileName = "")
    {
        const std::string text  = BuildIscnText(sourceStem, nodes, fileRefs, skelFileName,
                                                morphMeshFileIdx, morphFileName);
        const uint32_t textLen  = static_cast<uint32_t>(text.size());
        const uint32_t payloadSz = textLen + 1;  // +1 for null terminator

        SceneMetadata meta{};
        meta.nodeCount     = static_cast<uint32_t>(nodes.size());
        meta.meshFileCount = static_cast<uint32_t>(fileRefs.size());
        meta.textLength    = textLen;
        meta.reserved      = 0;

        AssetHeader header{};
        header.magic        = MAGIC_SCENE;
        header.version      = CurrentVersion(MAGIC_SCENE);
        header.resourceType = static_cast<uint16_t>(ResourceType::Scene);
        header.metadataSize = sizeof(SceneMetadata);
        header.dataSize     = payloadSz;

        const size_t total = sizeof(AssetHeader) + sizeof(SceneMetadata) + payloadSz;
        std::vector<uint8_t> blob(total, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));    dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(SceneMetadata));  dst += sizeof(SceneMetadata);
        std::memcpy(dst, text.c_str(), textLen);
        // last byte already 0 (null terminator)

        return blob;
    }

    // Per F-line index: the (target name, source aiAnimMesh::mWeight) pairs its morph block carries.
    using TargetsOfFile = std::vector<std::vector<std::pair<std::string, float>>>;

    // Build the combined .imorph (MAGIC_MORPHLIB2, MorphLibrary::Build) from the Assimp blend shapes.
    // Names form one skeleton-wide ordered union - what the .iskel EXTENDED block serialises and
    // Facial::FindMorphSlot resolves. Each per-mesh block is sparse (MORPHLIB_FLAG_SPARSE_SLOTS): only
    // the targets with a real delta on that mesh, behind a strictly increasing uint32 skeletonSlot[]
    // table; the runtime scatters them (SceneInstanceLoader -> MeshSkinnedComponent::morphSlotOfLocal /
    // morphLocalOfSlot -> BuildSkinJobs). A dense block over the whole union would be mostly zero
    // padding (93% on a 700-shape, 31-part rig). PmxImporter writes the same format.
    // Returns the blob and fills @p outMeshFileIdx with the F-line indices that got a block (the M-line
    // list), skel.morphTargetNames / Count, and @p outTargetsOfFile: per F index, the targets its block
    // carries with the source's aiAnimMesh::mWeight.
    static std::vector<uint8_t> BuildMorphLibBlob(
        const std::vector<const aiMesh*>& sources,
        const std::vector<XMFLOAT4X4>&    preTransforms,
        SkeletonAsset&                    skel,
        std::vector<int>&                 outMeshFileIdx,
        TargetsOfFile&                    outTargetsOfFile,
        ModelImportOptions::MorphNormals  morphNormals = ModelImportOptions::MorphNormals::Auto)
    {
        outMeshFileIdx.clear();
        outTargetsOfFile.assign(sources.size(), {});

        // Pass 1: the skeleton-wide ordered union of shape names. Its order must not depend on Assimp's
        // aiAnimMesh order: importing the same FBX twice (Assimp 6.0.2) gives the same meshes and bones but
        // a different permutation of each mesh's blend shapes, which would change the morph slots - and,
        // under the MAX_MORPH_TARGETS cap, which shapes survive - on every re-import. Sort key: (first source
        // mesh that carries the shape, name); mesh order is stable, so the face parts claim the low indices
        // the cap keeps.
        std::vector<std::string>                  unionNames;
        std::unordered_map<std::string, uint32_t> nameToIdx;      // name -> final index
        std::unordered_map<std::string, uint32_t> nameFirstSource; // name -> first mesh
        uint32_t droppedNames = 0, emptyShapes = 0, mismatched = 0;

        auto shapeName = [](const aiAnimMesh* am, size_t s, unsigned a) {
            std::string n = am->mName.C_Str();
            if (n.empty()) n = "morph_" + std::to_string(s) + "_" + std::to_string(a);
            if (n.size() > 63) n.resize(63);   // .iskel / .imorph name field is char[64]
            return n;
        };

        // A shape that moves nothing visible is a Blender shape-key separator ("----------ARKit----------",
        // "======= BROW ======="), left in the list as a heading. The bar is 0.1 mm, not zero: the reference
        // rigs' separators carry sub-micron noise, and a separator that takes a name slot costs a real
        // expression on a rig already over MAX_MORPH_TARGETS.
        constexpr float kMinShapeMotion = SkeletonImporter::kMinShapeMotion;   // shared with the rigid skin
        auto shapeMoves = [](const aiMesh* m, const aiAnimMesh* am) {
            if (!am->mVertices || am->mNumVertices != m->mNumVertices) return false;
            for (uint32_t v = 0; v < am->mNumVertices; ++v)
            {
                const aiVector3D d = am->mVertices[v] - m->mVertices[v];
                if (std::fabs(d.x) > kMinShapeMotion ||
                    std::fabs(d.y) > kMinShapeMotion ||
                    std::fabs(d.z) > kMinShapeMotion)
                    return true;
            }
            return false;
        };

        // Per mesh, "this shape deforms this mesh", memoised: it decides the union in pass 1 AND which
        // targets a block emits in pass 2, and each answer is an O(vertexCount) scan (computing it twice
        // costs minutes on a 31-part / 698-shape rig).
        std::vector<std::vector<uint8_t>> movesCache(sources.size());

        for (size_t s = 0; s < sources.size(); ++s)
        {
            const aiMesh* m = sources[s];
            if (!m) continue;
            movesCache[s].assign(m->mNumAnimMeshes, 0);
            for (unsigned a = 0; a < m->mNumAnimMeshes; ++a)
            {
                const aiAnimMesh* am = m->mAnimMeshes[a];
                if (!am) continue;
                // GUARD: JoinIdenticalVertices / ConvertToLeftHanded DO update
                // anim meshes in Assimp 6.0.2, but a desynced count would index
                // out of bounds, so it is checked rather than trusted.
                if (!am->mVertices || am->mNumVertices != m->mNumVertices)
                { ++mismatched; continue; }
                if (!shapeMoves(m, am)) { ++emptyShapes; continue; }
                movesCache[s][a] = 1;

                const std::string n = shapeName(am, s, a);
                nameFirstSource.emplace(n, static_cast<uint32_t>(s));
            }
        }

        {
            std::vector<std::pair<uint32_t, std::string>> ordered;
            ordered.reserve(nameFirstSource.size());
            for (const auto& [n, first] : nameFirstSource) ordered.emplace_back(first, n);
            std::sort(ordered.begin(), ordered.end());
            if (ordered.size() > SkeletonAsset::MAX_MORPH_TARGETS)
            {
                droppedNames = static_cast<uint32_t>(ordered.size() - SkeletonAsset::MAX_MORPH_TARGETS);
                // Name the casualties: "Face.SetWeight can't find X" is otherwise
                // indistinguishable from a typo. Bounded so a 558-shape rig does
                // not push everything else out of the log.
                uint32_t shown = 0;
                for (size_t i = SkeletonAsset::MAX_MORPH_TARGETS;
                     i < ordered.size() && shown < 64; ++i, ++shown)
                    LOG_INFO("SceneImporter:   dropped morph '%s'", ordered[i].second.c_str());
                if (droppedNames > shown)
                    LOG_INFO("SceneImporter:   ... and %u more dropped morph name(s)",
                             droppedNames - shown);
                ordered.resize(SkeletonAsset::MAX_MORPH_TARGETS);
            }
            unionNames.reserve(ordered.size());
            for (auto& [first, n] : ordered)
            {
                nameToIdx.emplace(n, static_cast<uint32_t>(unionNames.size()));
                unionNames.push_back(std::move(n));
            }
        }
        // The kept list is the ONLY thing Face.* can address, so it is logged in full: a missing expression
        // is otherwise a silent lookup miss with nothing to compare against.
        for (size_t i = 0; i < unionNames.size(); ++i)
            LOG_INFO("SceneImporter:   morph[%zu] = '%s'", i, unionNames[i].c_str());

        if (mismatched > 0)
            LOG_WARNING("SceneImporter: %u blend shape(s) had a vertex count differing from their "
                        "mesh — skipped", mismatched);
        if (unionNames.empty())
        {
            if (emptyShapes > 0)
                LOG_INFO("SceneImporter: %u blend shape(s) are zero-delta separators — no .imorph",
                         emptyShapes);
            return {};
        }
        if (droppedNames > 0)
            LOG_WARNING("SceneImporter: source has more blend shapes than SkeletonAsset::"
                        "MAX_MORPH_TARGETS (%u) — %u name(s) dropped; kept are the first %zu "
                        "by (mesh order, name), which favours the face parts",
                        SkeletonAsset::MAX_MORPH_TARGETS, droppedNames, unionNames.size());

        // Pass 2: one sparse block per mesh that uses any kept target, written by MorphLibrary::Build as
        // MAGIC_MORPHLIB2: each block also carries its section's key (0 here - WriteModelImportFiles stamps
        // the library's final keys once they are adopted) and each target its NORMAL delta - the source's
        // (aiAnimMesh::mNormals: the change of the unit normal in the baked space) or, where the source has
        // none, the change of the area-weighted smooth normal (MorphLibrary::ComputeNormalDeltas).
        std::vector<MorphLibrary::BlockOut> blockOuts;
        float    maxDelta   = 0.f;
        // Density census: equal by construction under the sparse layout; kept as the number that says so.
        uint32_t realTargets = 0, emittedTargets = 0;
        uint32_t normalsFromSource = 0, normalsComputed = 0;

        for (size_t s = 0; s < sources.size(); ++s)
        {
            const aiMesh* m = sources[s];
            if (!m || m->mNumAnimMeshes == 0) continue;

            // Which kept targets actually DEFORM this mesh? The block carries
            // exactly these, in ascending skeleton-slot order (the format
            // requires the slot table to be strictly increasing, and the
            // ordering also keeps a block's layout independent of Assimp's
            // unstable aiAnimMesh permutation).
            std::vector<std::pair<uint32_t, int>> localTargets;  // (skeleton slot, anim mesh idx)
            for (unsigned a = 0; a < m->mNumAnimMeshes; ++a)
            {
                const aiAnimMesh* am = m->mAnimMeshes[a];
                if (!am || !am->mVertices || am->mNumVertices != m->mNumVertices) continue;
                if (a >= movesCache[s].size() || !movesCache[s][a]) continue;  // zero on THIS mesh
                auto it = nameToIdx.find(shapeName(am, s, a));
                if (it == nameToIdx.end()) continue;                          // dropped by the cap
                localTargets.emplace_back(it->second, static_cast<int>(a));
            }
            if (localTargets.empty()) continue;
            std::sort(localTargets.begin(), localTargets.end());
            // Two anim meshes with the same name on one mesh would break the
            // strictly-increasing invariant; keep the first.
            localTargets.erase(std::unique(localTargets.begin(), localTargets.end(),
                                           [](const auto& x, const auto& y)
                                           { return x.first == y.first; }),
                               localTargets.end());

            const uint32_t nv = m->mNumVertices;

            // Deltas live in the space of the baked rest positions, so they take the LINEAR part of the mesh
            // node's transform (a direction, never the translation). Normals take the same linear part,
            // renormalised, as BuildMeshLibBlob bakes the base's, so a normal delta is relative to the normal
            // the mesh really has.
            XMMATRIX preLinear = XMMatrixIdentity();
            if (s < preTransforms.size())
            {
                XMFLOAT4X4 P = preTransforms[s];
                P._41 = P._42 = P._43 = 0.f;
                preLinear = XMLoadFloat4x4(&P);
            }

            // The base in the baked space and its triangles: what a computed
            // normal delta is taken over (built only when a target needs it).
            std::vector<XMFLOAT3> basePos, baseNrm;
            std::vector<uint32_t> triIdx;
            auto ensureBase = [&]()
            {
                if (!basePos.empty()) return;
                basePos.resize(nv);
                for (uint32_t v = 0; v < nv; ++v)
                    XMStoreFloat3(&basePos[v], XMVector3TransformNormal(
                        XMVectorSet(m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z, 0.f), preLinear));
                if (m->mNormals)
                {
                    baseNrm.resize(nv);
                    for (uint32_t v = 0; v < nv; ++v)
                        XMStoreFloat3(&baseNrm[v], XMVector3TransformNormal(
                            XMVectorSet(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z, 0.f), preLinear));
                }
                for (unsigned f = 0; f < m->mNumFaces; ++f)
                    if (m->mFaces[f].mNumIndices == 3)
                        triIdx.insert(triIdx.end(), { m->mFaces[f].mIndices[0], m->mFaces[f].mIndices[1],
                                                      m->mFaces[f].mIndices[2] });
            };

            MorphLibrary::BlockOut bo;
            bo.meshFileIndex = static_cast<uint32_t>(s);   // F-line index
            bo.vertexCount   = nv;
            for (const auto& [slot, a] : localTargets)
            {
                const aiAnimMesh* am = m->mAnimMeshes[a];
                MorphLibrary::TargetOut t;
                t.name = unionNames[slot];
                t.pos.resize(nv);
                // morphNormals = None: no normal deltas (MorphLibrary::Build sets HAS_NORMALS only when every
                // target has them, so the file carries none).
                const bool withNormals = morphNormals != ModelImportOptions::MorphNormals::None;
                if (withNormals) t.nrm.resize(nv);
                ++emittedTargets;
                ++realTargets;
                for (uint32_t v = 0; v < nv; ++v)
                {
                    const aiVector3D d = am->mVertices[v] - m->mVertices[v];
                    XMStoreFloat3(&t.pos[v], XMVector3TransformNormal(XMVectorSet(d.x, d.y, d.z, 0.f), preLinear));
                    maxDelta = (std::max)(maxDelta, (std::max)((std::max)(std::fabs(t.pos[v].x),
                                                                          std::fabs(t.pos[v].y)),
                                                               std::fabs(t.pos[v].z)));
                }
                // The source's own normals, when it has them. An importer without normals for a shape may still
                // hand over a copy of the base's (assimp's FBX path: base + a zero delta, renormalised, equal up to
                // rounding). A shape whose normals nowhere turn by more than 1e-3 counts as having none and gets
                // computed ones - zero too where the shape only translates.
                bool srcNormals = false;
                if (withNormals && morphNormals == ModelImportOptions::MorphNormals::Auto && am->mNormals && m->mNormals)
                {
                    float maxTurn = 0.f;
                    for (uint32_t v = 0; v < nv; ++v)
                    {
                        const XMVECTOR nb = XMVector3Normalize(XMVector3TransformNormal(
                            XMVectorSet(m->mNormals[v].x, m->mNormals[v].y, m->mNormals[v].z, 0.f), preLinear));
                        const XMVECTOR na = XMVector3Normalize(XMVector3TransformNormal(
                            XMVectorSet(am->mNormals[v].x, am->mNormals[v].y, am->mNormals[v].z, 0.f), preLinear));
                        const XMVECTOR d = XMVectorSubtract(na, nb);
                        XMStoreFloat3(&t.nrm[v], d);
                        maxTurn = (std::max)(maxTurn, XMVectorGetX(XMVector3LengthSq(d)));
                    }
                    srcNormals = maxTurn > 1e-3f * 1e-3f;
                }
                if (srcNormals)
                    ++normalsFromSource;
                else if (withNormals)   // none asked for: neither
                {
                    ensureBase();
                    MorphLibrary::ComputeNormalDeltas(basePos.data(), nv, triIdx.data(), triIdx.size(),
                                                      t.pos.data(), t.nrm.data(),
                                                      baseNrm.empty() ? nullptr : baseNrm.data());
                    ++normalsComputed;
                }
                outTargetsOfFile[s].emplace_back(t.name, am->mWeight);
                bo.slots.push_back(slot);
                bo.targets.push_back(std::move(t));
            }
            blockOuts.push_back(std::move(bo));
            outMeshFileIdx.push_back(static_cast<int>(s));
        }

        if (blockOuts.empty()) return {};

        // Publish the names into the skeleton: the .iskel EXTENDED block serialises them and
        // Facial::FindMorphSlot resolves against them.
        skel.morphTargetCount = static_cast<uint32_t>(unionNames.size());
        for (uint32_t i = 0; i < skel.morphTargetCount; ++i)
        {
            strncpy_s(skel.morphTargetNames[i], 64, unionNames[i].c_str(), _TRUNCATE);
            skel.morphTargetNames[i][63] = '\0';
        }

        std::vector<uint8_t> blob = MorphLibrary::Build(blockOuts);
        const uint32_t entryCount = static_cast<uint32_t>(blockOuts.size());
        LOG_INFO("SceneImporter: morph normals - %u target(s) from the source, %u computed", normalsFromSource,
                 normalsComputed);

        // A blend shape that moves a vertex a metre is almost always a unit or space mismatch, not an
        // expression: worth a log line.
        if (maxDelta > 1.0f)
            LOG_WARNING("SceneImporter: largest morph delta is %.3f m — suspiciously large; "
                        "check the source's scene units", maxDelta);

        std::string preview;
        for (size_t i = 0; i < unionNames.size() && i < 8; ++i)
        { preview += (i ? ", " : ""); preview += unionNames[i]; }
        if (unionNames.size() > 8) preview += ", ...";
        LOG_INFO("SceneImporter: morph targets — %zu name(s) across %u mesh block(s), "
                 "%u zero-delta separator(s) skipped, %.2f MB, max|delta|=%.4f m [%s]",
                 unionNames.size(), entryCount, emptyShapes,
                 blob.size() / (1024.0 * 1024.0), maxDelta, preview.c_str());

        // Density census: with sparse blocks (MORPHLIB_FLAG_SPARSE_SLOTS) this reports 100% by
        // construction. It is a tripwire: below 100 means a writer emits zero padding.
        if (emittedTargets > 0)
        {
            const double mb = blob.size() / (1024.0 * 1024.0);
            LOG_INFO("SceneImporter: morph density — %u of %u emitted target-block(s) carry "
                     "real deltas (%.0f%%); sparse per-mesh blocks, so padding is 0 by "
                     "construction",
                     realTargets, emittedTargets,
                     100.0 * realTargets / emittedTargets);
            if (mb > 64.0)
                LOG_WARNING("SceneImporter: .imorph is %.1f MB — this becomes the same number of "
                            "bytes of GPU morph-delta buffers at load. The blocks are already "
                            "sparse, so this is authored data: the rig has that many shapes x "
                            "that many vertices.", mb);
        }
        return blob;
    }

    // FlipSkeletonX - mirror right-handed PMX skeleton data into DX left-handed by reflecting across
    // the YZ plane (negate X):
    //   4x4 matrices : T' = M * T * M, M = Scale(-1,1,1)
    //   positions    : x = -x
    //   rotations    : Q' = (qx, -qy, -qz, qw)
    static void FlipSkeletonX(SkeletonImportResult& skel)
    {
        using namespace DirectX;
        const XMMATRIX M = XMMatrixScaling(-1.f, 1.f, 1.f);

        auto FlipMat = [&](XMFLOAT4X4& mat)
        {
            XMMATRIX T = XMLoadFloat4x4(&mat);
            XMStoreFloat4x4(&mat, M * T * M);
        };

        for (uint32_t b = 0; b < skel.skeleton.boneCount; ++b)
        {
            FlipMat(skel.skeleton.inverseBindPose[b]);
            FlipMat(skel.skeleton.bindPose[b]);
            FlipMat(skel.skeleton.restPoseLocal[b]);

            // Bind-model-space rest boxes mirror with the pose: negating X
            // swaps which side is min/max on that axis.
            auto& ab = skel.skeleton.boneRestAABBs[b];
            if (ab.localMin.x <= ab.localMax.x)
            {
                const float oldMin = ab.localMin.x;
                ab.localMin.x = -ab.localMax.x;
                ab.localMax.x = -oldMin;
            }
        }

        for (ClipAsset& clip : skel.clips)
        {
            const uint32_t total = clip.boneCount * clip.frameCount;
            for (uint32_t i = 0; i < total; ++i)
            {
                clip.positions[i].x = -clip.positions[i].x;
                // Q' = (qx, -qy, -qz, qw)
                clip.rotations[i].y = -clip.rotations[i].y;
                clip.rotations[i].z = -clip.rotations[i].z;
            }
        }
    }

    // Serialise a SkeletonImportResult into a .iskel blob. Payload, in order:
    //   uint32 meshBlendCount
    //   int32 parentIndex[bones]; float inverseBindPose[bones*16], bindPose[bones*16],
    //     restPoseLocal[bones*16]; char boneNames[bones*64]; float boneRestAABBs[bones*6]
    //   per clip: uint32 boneCount, frameCount; float duration, frameRate; float3 positions[],
    //     float4 rotations[], float3 scales[] (boneCount*frameCount each)
    //   per blend record: uint32 meshIndex, vertexCount; BlendVertex[vertexCount]
    //   the extended block (ISKEL_FLAG_EXTENDED), then the humanoid profile (ISKEL_FLAG_HUMANOID)
    // @p aiMeshToSubIdx maps a RAW aiMesh index (the index space of skel.perMeshBlendData) to the
    // submesh index of the .meshlib entries and .iscn F-lines (-1: the aiMesh was skipped). A blend
    // record's meshIndex is written in the SUBMESH space, the only one its readers speak
    // (SceneInstanceLoader::Load reads it as an F-line index, LoadPreviewMeshes as a meshId; here
    // F idx == meshId == submesh idx): a raw aiMesh index would shift every later part's bone weights
    // and morphs onto the wrong geometry once a skipped aiMesh precedes a skinned one.
    static std::vector<uint8_t> BuildIskelBlob(const SkeletonImportResult& skel,
                                                uint32_t numSceneMeshes,
                                                const std::vector<int>& aiMeshToSubIdx)
    {
        const SkeletonAsset& sk   = skel.skeleton;
        const uint32_t boneCount  = sk.boneCount;
        const uint32_t clipCount  = static_cast<uint32_t>(skel.clips.size());

        // The submesh (F-line) index of raw aiMesh `mi`, or -1 when it has no
        // blend record to write: no blend data, or an aiMesh that was skipped.
        auto blendRecordIndex = [&](uint32_t mi) -> int
        {
            if (mi >= static_cast<uint32_t>(skel.perMeshBlendData.size())) return -1;
            if (skel.perMeshBlendData[mi].empty()) return -1;
            if (mi >= static_cast<uint32_t>(aiMeshToSubIdx.size())) return -1;
            return aiMeshToSubIdx[mi];
        };

        // Count the blend records to write (blend data on an aiMesh that became a submesh).
        uint32_t meshBlendCount = 0;
        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            if (blendRecordIndex(mi) >= 0)
                ++meshBlendCount;
        }

        // Payload byte size.
        size_t payloadSz = sizeof(uint32_t);  // meshBlendCount
        payloadSz += boneCount * sizeof(int32_t);          // parentIndex
        payloadSz += boneCount * 16 * sizeof(float);       // inverseBindPose
        payloadSz += boneCount * 16 * sizeof(float);       // bindPose
        payloadSz += boneCount * 16 * sizeof(float);       // restPoseLocal
        payloadSz += boneCount * 64;                        // boneNames
        payloadSz += boneCount * 6 * sizeof(float);        // boneRestAABBs (min3 + max3)

        for (const ClipAsset& c : skel.clips)
        {
            const size_t kf = (size_t)c.boneCount * c.frameCount;
            payloadSz += 2 * sizeof(uint32_t) + 2 * sizeof(float); // header
            payloadSz += kf * 3 * sizeof(float);  // positions
            payloadSz += kf * 4 * sizeof(float);  // rotations
            payloadSz += kf * 3 * sizeof(float);  // scales
        }

        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            if (blendRecordIndex(mi) < 0) continue;
            payloadSz += 2 * sizeof(uint32_t) + skel.perMeshBlendData[mi].size() * sizeof(BlendVertex);
        }

        // Extended block (ISKEL_FLAG_EXTENDED), always written: it is the only place
        // SceneInstanceLoader::LoadIskel reads morph target NAMES from - without it every Face.* lookup
        // misses and the .imorph deltas are unreachable. The grant table comes from
        // SkeletonImporter::BuildGrantTable; transformOrder is 0 for every bone (AnimationSystem compares it
        // only against the minimum IK order, vacuous without IK chains); ikCount is 0 (Assimp sources carry
        // no PMX-style IK).
        //   grantSource[boneCount] i32, grantRatio[boneCount] f32,
        //   transformOrder[boneCount] i32, ikCount u32 (== 0),
        //   morphTargetCount u32, morphTargetNames[count][64]
        payloadSz += boneCount * sizeof(int32_t);   // grantSource
        payloadSz += boneCount * sizeof(float);     // grantRatio
        payloadSz += boneCount * sizeof(int32_t);   // transformOrder
        payloadSz += sizeof(uint32_t);              // ikChainCount (0)
        payloadSz += sizeof(uint32_t);              // morphTargetCount
        payloadSz += static_cast<size_t>(sk.morphTargetCount) * 64;

        // Humanoid profile (retarget role map), appended at the payload tail when a convention was
        // recognized. See docs/design/RETARGET_ARCHITECTURE.md.
        const bool writeHumanoid =
            (sk.humanoid.convention != static_cast<uint8_t>(HumanoidConvention::Unknown));
        if (writeHumanoid)
            payloadSz += sizeof(HumanoidProfile);

        // Header + metadata.
        SkeletonMetadata meta{};
        meta.boneCount   = boneCount;
        meta.clipCount   = clipCount;
        meta.reserved[0] = meshBlendCount;  // the first reserved slot holds meshBlendCount
        meta.reserved[1] = ISKEL_FLAG_EXTENDED
                         | (writeHumanoid ? ISKEL_FLAG_HUMANOID : 0)
                         | (sk.hasBoneAABBs ? ISKEL_FLAG_BONE_AABB_MODEL : 0);

        AssetHeader header{};
        header.magic        = MAGIC_SKELETON;
        header.version      = CurrentVersion(MAGIC_SKELETON);
        header.resourceType = static_cast<uint16_t>(ResourceType::Skeleton);
        header.metadataSize = sizeof(SkeletonMetadata);
        header.dataSize     = static_cast<uint32_t>(payloadSz);
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSz = sizeof(AssetHeader) + sizeof(SkeletonMetadata) + payloadSz;
        std::vector<uint8_t> blob(totalSz, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));      dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(SkeletonMetadata)); dst += sizeof(SkeletonMetadata);

        // meshBlendCount
        std::memcpy(dst, &meshBlendCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

        // parentIndex
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.parentIndex[i], sizeof(int32_t)); dst += sizeof(int32_t); }

        // inverseBindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.inverseBindPose[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // bindPose
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.bindPose[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // restPoseLocal
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.restPoseLocal[i], 16 * sizeof(float)); dst += 16 * sizeof(float); }

        // boneNames
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, sk.boneNames[i], 64); dst += 64; }

        // boneRestAABBs (6 floats per bone: min xyz + max xyz)
        for (uint32_t i = 0; i < boneCount; ++i)
        {
            std::memcpy(dst, &sk.boneRestAABBs[i].localMin, 3 * sizeof(float)); dst += 3 * sizeof(float);
            std::memcpy(dst, &sk.boneRestAABBs[i].localMax, 3 * sizeof(float)); dst += 3 * sizeof(float);
        }

        // clips
        for (const ClipAsset& c : skel.clips)
        {
            const uint32_t bc = c.boneCount, fc = c.frameCount;
            const size_t   kf = (size_t)bc * fc;
            std::memcpy(dst, &bc,          sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &fc,          sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &c.duration,  sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &c.frameRate, sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, c.positions.data(), kf * 3 * sizeof(float)); dst += kf * 3 * sizeof(float);
            std::memcpy(dst, c.rotations.data(), kf * 4 * sizeof(float)); dst += kf * 4 * sizeof(float);
            std::memcpy(dst, c.scales.data(),    kf * 3 * sizeof(float)); dst += kf * 3 * sizeof(float);
        }

        // per-mesh blend data, keyed by SUBMESH (F-line) index (see the function comment)
        for (uint32_t mi = 0; mi < numSceneMeshes; ++mi)
        {
            const int sub = blendRecordIndex(mi);
            if (sub < 0) continue;
            const auto& bd = skel.perMeshBlendData[mi];
            const uint32_t vc = static_cast<uint32_t>(bd.size());
            const uint32_t recordIndex = static_cast<uint32_t>(sub);
            std::memcpy(dst, &recordIndex, sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, &vc, sizeof(uint32_t)); dst += sizeof(uint32_t);
            std::memcpy(dst, bd.data(), vc * sizeof(BlendVertex)); dst += vc * sizeof(BlendVertex);
        }

        // Extended block. The field ORDER is the contract with SceneInstanceLoader::LoadIskel, which reads
        // positionally: a reordered or omitted field mis-parses everything after it (the humanoid tail too).
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.grantSource[i], sizeof(int32_t)); dst += sizeof(int32_t); }
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.grantRatio[i], sizeof(float)); dst += sizeof(float); }
        for (uint32_t i = 0; i < boneCount; ++i)
        { std::memcpy(dst, &sk.transformOrder[i], sizeof(int32_t)); dst += sizeof(int32_t); }
        const uint32_t ikCount = 0;   // no PMX-style IK chains from an Assimp source
        std::memcpy(dst, &ikCount, sizeof(uint32_t)); dst += sizeof(uint32_t);

        std::memcpy(dst, &sk.morphTargetCount, sizeof(uint32_t)); dst += sizeof(uint32_t);
        for (uint32_t i = 0; i < sk.morphTargetCount; ++i)
        { std::memcpy(dst, sk.morphTargetNames[i], 64); dst += 64; }

        // Humanoid profile (tail, gated by ISKEL_FLAG_HUMANOID)
        if (writeHumanoid)
        {
            std::memcpy(dst, &sk.humanoid, sizeof(HumanoidProfile)); dst += sizeof(HumanoidProfile);
        }

        return blob;
    }

    // ==== Assimp IO redirection: sibling files resolve next to the source model ====
    // Import() feeds Assimp the model bytes with ReadFileFromMemory(), so the "current file" Assimp sees
    // is its in-memory magic name ("$$$___magic___$$$"), whose directory is EMPTY. Every format that
    // names sibling files by relative URI (.gltf -> .bin, .obj -> .mtl, .fbx -> .fbm) would resolve them
    // against the process working directory and fail with "could not open referenced file".
    // MemoryIOSystem serves only the magic name and forwards every other name to the IOSystem installed
    // BEFORE ReadFileFromMemory(), so SourceDirIOSystem, installed there, rebases relative names onto the
    // source file's directory. It opens through the Sys:: UTF-8 helpers, so non-ASCII model folders work
    // (Assimp's DefaultIOSystem is bypassed).
    namespace {

        // Read-only stdio stream. Seek follows DefaultIOStream's convention (offset passed straight to
        // fseek; callers use NEGATIVE offsets with aiOrigin_END), not MemoryIOStream's inverted one.
        class SourceFileIOStream final : public Assimp::IOStream
        {
        public:
            explicit SourceFileIOStream(FILE* f) : m_f(f)
            {
                _fseeki64(m_f, 0, SEEK_END);
                m_size = static_cast<size_t>(_ftelli64(m_f));
                _fseeki64(m_f, 0, SEEK_SET);
            }
            ~SourceFileIOStream() override { if (m_f) std::fclose(m_f); }

            size_t Read(void* buf, size_t size, size_t count) override
            {
                return (size == 0) ? 0 : std::fread(buf, size, count, m_f);
            }
            size_t Write(const void*, size_t, size_t) override { return 0; }  // read-only

            aiReturn Seek(size_t offset, aiOrigin origin) override
            {
                const int whence = (origin == aiOrigin_CUR) ? SEEK_CUR
                                 : (origin == aiOrigin_END) ? SEEK_END
                                                            : SEEK_SET;
                const long long delta =
                    static_cast<long long>(static_cast<std::ptrdiff_t>(offset));
                return _fseeki64(m_f, delta, whence) == 0 ? AI_SUCCESS : AI_FAILURE;
            }
            size_t Tell()     const override { return static_cast<size_t>(_ftelli64(m_f)); }
            size_t FileSize() const override { return m_size; }
            void   Flush()          override {}

        private:
            FILE*  m_f    = nullptr;
            size_t m_size = 0;
        };

        class SourceDirIOSystem final : public Assimp::IOSystem
        {
        public:
            explicit SourceDirIOSystem(std::string baseDirUtf8)
                : m_baseDir(std::move(baseDirUtf8)) {}

            bool Exists(const char* file) const override { return !Resolve(file).empty(); }
            char getOsSeparator() const override { return '/'; }

            Assimp::IOStream* Open(const char* file, const char* mode) override
            {
                // Import is read-only: never let an importer truncate an asset.
                if (mode && (std::strchr(mode, 'w') || std::strchr(mode, 'a')))
                    return nullptr;

                const std::string path = Resolve(file);
                if (path.empty()) return nullptr;

                FILE* f = nullptr;
                if (_wfopen_s(&f, Sys::Utf8ToWide(path).c_str(), L"rb") != 0 || !f)
                    return nullptr;
                return new SourceFileIOStream(f);
            }

            void Close(Assimp::IOStream* stream) override { delete stream; }

        private:
            // As given first (absolute paths, or a CWD-relative name that really
            // does exist), then rebased onto the model's own directory.
            std::string Resolve(const char* file) const
            {
                if (!file || !*file) return {};
                std::string p(file);
                if (Sys::FileExists(p)) return p;
                std::string rebased;
                if (!m_baseDir.empty())
                {
                    rebased = Sys::PathToUtf8(Sys::U8Path(m_baseDir) / Sys::U8Path(p));
                    if (Sys::FileExists(rebased)) return rebased;
                }
                // Neither is on this checkout's disk: a worktree sharing the
                // main checkout's binary assets reads the source from there
                // (Resource/SharedContent.h). Read-only - Open refuses writes.
                const auto& afs = Resource::AssetFS::Get();
                if (std::string s = afs.ResolveOnDisk(p); !s.empty()) return s;
                if (!rebased.empty())
                    if (std::string s = afs.ResolveOnDisk(rebased); !s.empty()) return s;
                return {};
            }

            std::string m_baseDir;
        };

    } // anonymous

    // ==== SceneImporter::Import ====
    // The source's shape-key records, per Mesh of @p spec (whose sections are F indices). Each preset is
    // cut to the targets the Mesh's sections carry (a VRM expression binding the face and the teeth
    // becomes one MP record on each); @p defaultsFromWeights turns a non-zero aiAnimMesh::mWeight into
    // MK default= - a glTF's mesh.weights, the default a viewer shows (FBX's mWeight is an in-between's
    // full weight, 1 for every plain shape, not a default: never read there).
    static std::vector<ImportedShapeKeys> SourceShapeKeys(const MeshIdentitySpec& spec,
                                                          const TargetsOfFile&    targetsOfFile,
                                                          const std::vector<SceneImporter::PresetHint>* presets,
                                                          bool                    defaultsFromWeights)
    {
        std::vector<ImportedShapeKeys>  out;
        std::unordered_set<std::string> anywhere;   // every target some section carries
        for (const auto& targets : targetsOfFile)
            for (const auto& [n, w] : targets) anywhere.insert(n);
        if (anywhere.empty()) return out;

        for (const MeshIdentitySpec::Mesh& m : spec.meshes)
        {
            // The Mesh's targets (first-seen order), each with the largest source
            // weight any of its sections gives it.
            std::vector<std::pair<std::string, float>> mine;
            std::unordered_map<std::string, size_t>    at;
            for (const uint32_t s : m.sections)
            {
                if (s >= targetsOfFile.size()) continue;
                for (const auto& [n, w] : targetsOfFile[s])
                {
                    const auto [it, added] = at.emplace(n, mine.size());
                    if (added) mine.emplace_back(n, w);
                    else       mine[it->second].second = (std::max)(mine[it->second].second, w);
                }
            }
            if (mine.empty()) continue;

            ImportedShapeKeys is;
            is.meshSourceKey = m.sourceKey;
            is.meshName      = m.name;
            if (defaultsFromWeights)
                for (const auto& [n, w] : mine)
                    if (w > 0.f)
                    {
                        ShapeKeyMeta k;
                        k.name   = n;
                        k.def    = (std::min)(w, 1.f);
                        k.source = "gltf";
                        is.setup.meta.push_back(std::move(k));
                    }
            if (presets)
                for (const SceneImporter::PresetHint& h : *presets)
                {
                    ShapeKeyPreset p;
                    p.name   = h.name;
                    p.source = h.source;
                    for (const auto& [n, w] : h.weights)
                        if (w > 0.f && at.count(n)) p.weights.emplace_back(n, (std::min)(w, 1.f));
                    if (!p.weights.empty()) is.setup.presets.push_back(std::move(p));
                }
            if (!is.setup.Empty()) out.push_back(std::move(is));
        }

        // A weight on a target no section carries (a zero-delta shape, one past the
        // morph cap, one the source left unnamed) has nowhere to go - said, per preset.
        if (presets)
            for (const SceneImporter::PresetHint& h : *presets)
            {
                uint32_t placed = 0;
                std::string lost;
                for (const auto& [n, w] : h.weights)
                {
                    if (anywhere.count(n)) { ++placed; continue; }
                    if (lost.size() < 200) lost += (lost.empty() ? "'" : ", '") + n + "'";
                }
                if (!lost.empty())
                    LOG_WARNING("SceneImporter:   preset '%s': weight(s) on %s - no shape key of the model has that "
                                "name - dropped", h.name.c_str(), lost.c_str());
                if (placed == 0)
                    LOG_INFO("SceneImporter:   preset '%s' moves no shape key of the model - no record", h.name.c_str());
            }
        return out;
    }

    SceneImporter::ImportResult SceneImporter::Import(const std::string&             sourcePath,
                                                       const std::vector<uint8_t>&    sourceData,
                                                       const HumanoidHint*            humanoidHint,
                                                       HumanoidConvention             humanoidConv,
                                                       const std::vector<PresetHint>* presets,
                                                       const ModelImportContext*      context)
    {
        ImportResult result;

        if (sourceData.empty())
        {
            LOG_ERROR("SceneImporter: empty source data for '%s'", sourcePath.c_str());
            return result;
        }

        // Load with Assimp from memory; the file extension is the format hint.
        std::string ext = Sys::PathToUtf8(Sys::U8Path(sourcePath).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        // VRM is a GLB container with custom extension JSON: Assimp parses it as glb.
        if (ext == ".vrm") ext = ".glb";
        // A glTF's mesh.weights reach aiAnimMesh::mWeight as the default a viewer shows; no other format's
        // mWeight means that (SourceShapeKeys).
        const bool gltfFamily = (ext == ".gltf" || ext == ".glb");
        const char* hint = ext.empty() ? "" : ext.c_str() + 1;  // strip leading '.'

        Assimp::Importer importer;
        // Sibling files (.gltf -> .bin, .obj -> .mtl) must resolve against the MODEL's directory, not the
        // process CWD (SourceDirIOSystem). The Importer takes ownership; ReadFileFromMemory() wraps this
        // handler in its MemoryIOSystem and restores it afterwards.
        importer.SetIOHandler(new SourceDirIOSystem(
            Sys::PathToUtf8(Sys::U8Path(sourcePath).parent_path())));
        // FBX stores each node's transform as a chain of pivot / pre-rotation / offset helpers. Preserved
        // (Assimp's default), they become extra nodes "<Bone>_$AssimpFbx$_RotationPivot" / "_Rotation" /
        // "_Translation" / "_Scaling" with the bare "<Bone>" node ~identity: helper nodes litter the .iscn,
        // every bone's restPoseLocal collapses to the origin (SkeletonImporter reads node->mTransformation by
        // bone name), and clip channels split across the helpers. Not preserving them bakes the chain into
        // each node's mTransformation. FBX-only; ignored for glTF / PMX / OBJ.
        importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
        // The import's options (the defaults without a context).
        static const ModelImportOptions kDefaultOptions;
        const ModelImportOptions& options = context ? context->options : kDefaultOptions;
        unsigned flags =
            aiProcess_Triangulate            |
            aiProcess_ConvertToLeftHanded    |
            aiProcess_GenSmoothNormals       |
            aiProcess_CalcTangentSpace       |  // ensures mTangents / mBitangents populated
            aiProcess_JoinIdenticalVertices  |
            aiProcess_SortByPType;
        // Normals made where they are missing - smoothed across edges up to the
        // option's angle; normals = compute drops the source's first, so every
        // mesh gets made ones (and tangents from them).
        importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, options.smoothAngle);
        if (options.normals == ModelImportOptions::Normals::Compute)
        {
            importer.SetPropertyInteger(AI_CONFIG_PP_RVC_FLAGS, aiComponent_NORMALS | aiComponent_TANGENTS_AND_BITANGENTS);
            flags |= aiProcess_RemoveComponent;
        }

        const aiScene* scene = importer.ReadFileFromMemory(
            sourceData.data(), sourceData.size(), flags, hint);

        if (!scene || !scene->mRootNode)
        {
            LOG_ERROR("SceneImporter: Assimp failed on '%s': %s",
                      sourcePath.c_str(), importer.GetErrorString());
            return result;
        }
        // Assimp flags MESH-LESS scenes AI_SCENE_FLAGS_INCOMPLETE. Animation-only FBX (UE anim exports,
        // Mixamo "without skin") land here and are still valid retarget sources: the skeleton is rebuilt
        // from the animated node hierarchy (SkeletonImporter fallback). Only an INCOMPLETE scene with no
        // animations either is rejected.
        if ((scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) && scene->mNumAnimations == 0)
        {
            LOG_ERROR("SceneImporter: '%s' is incomplete with no meshes and no "
                      "animations — nothing to import (%s)",
                      sourcePath.c_str(), importer.GetErrorString());
            return result;
        }

        // options.morphs off: the shape keys go before anything reads them - no .imorph, no MK / MP records,
        // no one-bone rigid skin for a bone-less mesh they would have moved. Assimp has no RemoveComponent
        // flag for anim meshes; the scene is the importer's to free, so it frees them.
        if (!options.morphs)
        {
            uint32_t stripped = 0;
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
            {
                aiMesh* mesh = scene->mMeshes[mi];
                if (!mesh || mesh->mNumAnimMeshes == 0 || !mesh->mAnimMeshes) continue;
                for (unsigned a = 0; a < mesh->mNumAnimMeshes; ++a) delete mesh->mAnimMeshes[a];
                delete[] mesh->mAnimMeshes;
                stripped += mesh->mNumAnimMeshes;
                mesh->mAnimMeshes    = nullptr;
                mesh->mNumAnimMeshes = 0;
            }
            if (stripped)
                LOG_INFO("SceneImporter: morphs = 0 - %u shape key(s) not imported", stripped);
        }

        // The model's name: a re-import keeps the model's own (context->stem), whatever the source file is
        // called now.
        const std::string stem = (context && !context->stem.empty())
                               ? context->stem : Sys::PathToUtf8(Sys::U8Path(sourcePath).stem());

        // Skeleton extraction (optional). On the heap: the fixed 2048-bone tables are too large for
        // import-thread stacks.
        auto skelStorage = std::make_unique<SkeletonImportResult>();
        auto& skelResult = *skelStorage;
        const bool hasSkel = SkeletonImporter::Import(scene, skelResult);
        std::string skelFileName;  // empty = no skeleton

        if (hasSkel && skelResult.synthesizedSkeleton)
        {
            // A synthesized one-bone skeleton (shape keys only): no humanoid profile to find.
            LOG_INFO("SceneImporter: %u static mesh(es) with shape keys -> a one-bone rigid skeleton",
                     skelResult.rigidMeshes);
            skelFileName = stem + ".iskel";
        }
        else if (hasSkel)
        {
            LOG_INFO("SceneImporter: skeleton detected — %u bones, %zu clips",
                     skelResult.skeleton.boneCount, skelResult.clips.size());

            // Build the canonical humanoid role map. If the caller supplied a hint
            // (VRM humanoid map: role -> nodeName), join it against the skeleton by
            // name; otherwise auto-detect from bone names (Mixamo / Unreal / generic).
            SkeletonAsset& sk = skelResult.skeleton;
            if (humanoidHint && !humanoidHint->empty())
            {
                sk.humanoid.Reset();
                sk.humanoid.convention = static_cast<uint8_t>(
                    humanoidConv == HumanoidConvention::Unknown ? HumanoidConvention::Vrm1 : humanoidConv);
                for (const auto& [role, nodeName] : *humanoidHint)
                {
                    for (uint32_t i = 0; i < sk.boneCount; ++i)
                        if (std::strcmp(sk.boneNames[i], nodeName.c_str()) == 0)
                        { sk.humanoid.Set(role, static_cast<int32_t>(i)); break; }
                }
                HumanoidProfileBuilder::FinalizeDerived(sk);
            }
            else
            {
                HumanoidProfileBuilder::Build(sk);
            }
            if (sk.humanoid.convention != static_cast<uint8_t>(HumanoidConvention::Unknown))
                LOG_INFO("SceneImporter:   humanoid profile — convention=%u, valid=%u",
                         sk.humanoid.convention, sk.humanoid.valid);

            skelFileName = stem + ".iskel";  // bare filename, sibling of .iscn
            // The .iskel blob is written later, after the morph union is folded into
            // skelResult.skeleton.morphTargetNames: the runtime reads morph names from the .iskel.
        }

        // Traverse the scene graph first for the per-node aiMesh groupings. The geometry goes into ONE
        // .meshlib for the whole file, one entry per kept aiMesh. Assimp splits every node's mesh by
        // material, so an entry is a material SECTION; the node's mrK list in the .iscn groups sections back
        // into the author's object.
        std::vector<NodeRecord> nodes;
        nodes.reserve(32);
        CollectNodes(scene->mRootNode, -1, nodes);

        // Collision nodes. Unreal's naming: a node called UCX_<Mesh>[_NN] is a convex hull, UBX_ a box,
        // USP_ a sphere, UCP_ a capsule - its geometry IS the shape and is never drawn; <Mesh> names the
        // node that draws the Mesh it belongs to (else a sibling that draws, else the parent). Godot's
        // suffixes on a node that draws: -col = its own triangles (complexity=complex), -convcol = the hull
        // of its vertices. Every shape goes into the target's own space (where the Mesh's vertices are),
        // named by the target NODE until the identity below says which Mesh (source key) that is;
        // WriteModelImportFiles keys it.
        std::unordered_set<unsigned>                    collisionOnlyMeshes;   // aiMeshes only shapes use
        std::vector<std::pair<int, ImportedCollision>> nodeCollision;         // (target node, its collision)
        {
            enum Kind { kNone, kConvex, kBox, kSphere, kCapsule, kOwnTrimesh, kOwnConvex };
            auto kindOf = [](const std::string& raw) -> int
            {
                if (raw.size() > 4 && raw[3] == '_')
                {
                    char p[4] = { static_cast<char>(std::toupper(static_cast<unsigned char>(raw[0]))),
                                  static_cast<char>(std::toupper(static_cast<unsigned char>(raw[1]))),
                                  static_cast<char>(std::toupper(static_cast<unsigned char>(raw[2]))), 0 };
                    if (!std::strcmp(p, "UCX")) return kConvex;
                    if (!std::strcmp(p, "UBX")) return kBox;
                    if (!std::strcmp(p, "USP")) return kSphere;
                    if (!std::strcmp(p, "UCP")) return kCapsule;
                }
                auto ends = [&](const char* s)
                {
                    const size_t n = std::strlen(s);
                    if (raw.size() <= n) return false;
                    for (size_t i = 0; i < n; ++i)
                        if (std::tolower(static_cast<unsigned char>(raw[raw.size() - n + i])) != s[i]) return false;
                    return true;
                };
                if (ends("-convcol")) return kOwnConvex;
                if (ends("-col")) return kOwnTrimesh;
                return kNone;
            };
            auto isShape = [](int k) { return k == kConvex || k == kBox || k == kSphere || k == kCapsule; };
            std::vector<int> kind(nodes.size(), kNone);
            bool any = false;
            for (size_t n = 0; n < nodes.size(); ++n)
            {
                if (!nodes[n].meshFileIndices.empty()) kind[n] = kindOf(nodes[n].rawName);
                any |= kind[n] != kNone;
            }
            if (any)
            {
                // Every node's world, in the engine's convention (S x R x T, row
                // vectors - what the spawned entities compose).
                std::vector<XMFLOAT4X4> world(nodes.size());
                for (size_t n = 0; n < nodes.size(); ++n)
                {
                    const NodeRecord& r = nodes[n];
                    const XMMATRIX local = XMMatrixScaling(r.sx, r.sy, r.sz) *
                                           XMMatrixRotationQuaternion(XMVectorSet(r.qx, r.qy, r.qz, r.qw)) *
                                           XMMatrixTranslation(r.tx, r.ty, r.tz);
                    XMStoreFloat4x4(&world[n], (r.parent >= 0 && static_cast<size_t>(r.parent) < n)
                                                   ? local * XMLoadFloat4x4(&world[static_cast<size_t>(r.parent)])
                                                   : local);
                }
                auto drawn = [&](int n)
                {
                    return n >= 0 && static_cast<size_t>(n) < nodes.size() &&
                           !nodes[static_cast<size_t>(n)].meshFileIndices.empty() && !isShape(kind[static_cast<size_t>(n)]);
                };
                std::unordered_map<std::string, int> drawnByName;   // raw name -> the first node that draws it
                for (size_t n = 0; n < nodes.size(); ++n)
                    if (drawn(static_cast<int>(n))) drawnByName.emplace(nodes[n].rawName, static_cast<int>(n));
                auto resolve = [&](size_t c) -> int
                {
                    const std::string name = nodes[c].rawName.substr(4);
                    if (const auto it = drawnByName.find(name); it != drawnByName.end()) return it->second;
                    // UCX_Rock_01 / UCX_Rock.001: a trailing number counts the shapes.
                    const size_t cut = name.find_last_of("_.-");
                    if (cut != std::string::npos && cut + 1 < name.size() &&
                        std::all_of(name.begin() + static_cast<std::ptrdiff_t>(cut) + 1, name.end(),
                                    [](unsigned char ch) { return ch >= '0' && ch <= '9'; }))
                        if (const auto it = drawnByName.find(name.substr(0, cut)); it != drawnByName.end())
                            return it->second;
                    for (size_t s = 0; s < nodes.size(); ++s)
                        if (s != c && nodes[s].parent == nodes[c].parent && drawn(static_cast<int>(s)))
                            return static_cast<int>(s);
                    return drawn(nodes[c].parent) ? nodes[c].parent : -1;
                };
                auto skinnedNode = [&](int n)
                {
                    for (int mi : nodes[static_cast<size_t>(n)].meshFileIndices)
                        if (hasSkel && mi >= 0 && static_cast<size_t>(mi) < skelResult.meshIsSkinned.size() &&
                            skelResult.meshIsSkinned[static_cast<size_t>(mi)])
                            return true;
                    return false;
                };
                auto pointsOf = [&](size_t n)
                {
                    std::vector<XMFLOAT3> pts;
                    for (int mi : nodes[n].meshFileIndices)
                    {
                        const aiMesh* m = (mi >= 0 && static_cast<unsigned>(mi) < scene->mNumMeshes) ? scene->mMeshes[mi] : nullptr;
                        if (!m) continue;
                        for (unsigned v = 0; v < m->mNumVertices; ++v)
                            pts.push_back({ m->mVertices[v].x, m->mVertices[v].y, m->mVertices[v].z });
                    }
                    return pts;
                };
                auto transformed = [](std::vector<XMFLOAT3> pts, const XMMATRIX& m)
                {
                    for (XMFLOAT3& p : pts) XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&p), m));
                    return pts;
                };

                std::map<int, ImportedCollision> byTarget;   // ordered: deterministic output
                std::unordered_set<int>          usedByDrawn;
                for (size_t n = 0; n < nodes.size(); ++n)
                    if (!isShape(kind[n]))
                        for (int mi : nodes[n].meshFileIndices) usedByDrawn.insert(mi);
                uint32_t shapes = 0, dropped = 0;
                constexpr float kMin = Physics::CollisionFit::kMinHalfThickness;
                for (size_t c = 0; c < nodes.size(); ++c)
                {
                    if (kind[c] == kNone) continue;
                    const bool own = !isShape(kind[c]);
                    if (!own)
                        for (int mi : nodes[c].meshFileIndices)
                            if (!usedByDrawn.count(mi)) collisionOnlyMeshes.insert(static_cast<unsigned>(mi));
                    const int t = own ? static_cast<int>(c) : resolve(c);
                    if (t < 0 || skinnedNode(t))
                    {
                        LOG_WARNING("SceneImporter: collision node '%s' %s - skipped", nodes[c].rawName.c_str(),
                                    t < 0 ? "names no node that draws" : "belongs to a skinned mesh (its collision is its skeleton's)");
                        ++dropped;
                        continue;
                    }
                    ImportedCollision& ic = byTarget[t];
                    const XMMATRIX rel = own ? XMMatrixIdentity()
                                             : XMLoadFloat4x4(&world[c]) *
                                                   XMMatrixInverse(nullptr, XMLoadFloat4x4(&world[static_cast<size_t>(t)]));
                    XMVECTOR rs, rq, rt;
                    if (!XMMatrixDecompose(&rs, &rq, &rt, rel)) { rs = XMVectorSplatOne(); rq = XMQuaternionIdentity(); }
                    XMFLOAT3 S;
                    XMStoreFloat3(&S, XMVectorAbs(rs));
                    const std::vector<XMFLOAT3> local = pointsOf(c);
                    CollisionPrim prim;
                    std::vector<XMFLOAT3>   hull;
                    std::string             err;
                    bool                    ok = false;
                    switch (kind[c])
                    {
                    case kOwnTrimesh:
                        ic.setup.complexity = MeshBodySetup::Complexity::Complex;
                        ++shapes;
                        continue;
                    case kOwnConvex:
                    case kConvex:
                        ok = Physics::CollisionFit::ConvexHull(transformed(local, rel), hull, 256, &err);
                        prim.type = CollisionPrim::Type::Hull;
                        break;
                    case kBox:
                        // Its own axes: the AABB in its node's space, then that frame into the target's.
                        if ((ok = Physics::CollisionFit::FitBox(local, prim)))
                        {
                            XMStoreFloat3(&prim.center, XMVector3TransformCoord(XMLoadFloat3(&prim.center), rel));
                            prim.halfExtents = { (std::max)(prim.halfExtents.x * S.x, kMin),
                                                 (std::max)(prim.halfExtents.y * S.y, kMin),
                                                 (std::max)(prim.halfExtents.z * S.z, kMin) };
                            XMStoreFloat4(&prim.rotation, XMQuaternionNormalize(rq));
                        }
                        break;
                    case kSphere:
                        ok = Physics::CollisionFit::FitSphere(transformed(local, rel), prim);
                        break;
                    case kCapsule:
                        if ((ok = Physics::CollisionFit::FitCapsule(local, prim)))
                        {
                            const float s = (S.x + S.y + S.z) / 3.f;
                            XMStoreFloat3(&prim.center, XMVector3TransformCoord(XMLoadFloat3(&prim.center), rel));
                            XMStoreFloat4(&prim.rotation,
                                          XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&prim.rotation), rq)));
                            prim.radius     = (std::max)(prim.radius * s, kMin);
                            prim.halfHeight = (std::max)(prim.halfHeight * s, kMin);
                        }
                        break;
                    default:
                        break;
                    }
                    if (!ok)
                    {
                        LOG_WARNING("SceneImporter: collision node '%s' - no shape (%s)", nodes[c].rawName.c_str(),
                                    err.empty() ? "no geometry" : err.c_str());
                        ++dropped;
                        continue;
                    }
                    prim.name = nodes[c].rawName;
                    ic.setup.prims.push_back(prim);
                    ic.hullPoints.push_back(std::move(hull));
                    ++shapes;
                }
                // A shape node draws nothing; one without children is not kept either.
                std::vector<int> childCount(nodes.size(), 0);
                for (const NodeRecord& r : nodes)
                    if (r.parent >= 0) ++childCount[static_cast<size_t>(r.parent)];
                std::vector<int>        remap(nodes.size(), -1);
                std::vector<NodeRecord> kept;
                kept.reserve(nodes.size());
                for (size_t n = 0; n < nodes.size(); ++n)
                {
                    if (isShape(kind[n])) nodes[n].meshFileIndices.clear();
                    if (isShape(kind[n]) && childCount[n] == 0) continue;
                    remap[n] = static_cast<int>(kept.size());
                    NodeRecord r = nodes[n];
                    r.index  = remap[n];
                    r.parent = r.parent >= 0 ? remap[static_cast<size_t>(r.parent)] : -1;
                    kept.push_back(std::move(r));
                }
                const size_t removed = nodes.size() - kept.size();
                nodes.swap(kept);
                for (auto& [t, ic] : byTarget)
                    if (remap[static_cast<size_t>(t)] >= 0) nodeCollision.emplace_back(remap[static_cast<size_t>(t)], std::move(ic));
                LOG_INFO("SceneImporter: collision - %u shape(s) for %zu mesh node(s), %u skipped; %zu shape node(s) "
                         "removed, %zu mesh(es) only they used left out of the library",
                         shapes, nodeCollision.size(), dropped, removed, collisionOnlyMeshes.size());
            }
        }

        // One .imat per Assimp material: F-lines reference materials per line, and aiMaterial indices do
        // not change with the per-node grouping.
        std::vector<std::string> matFileNameByAiMat(scene->mNumMaterials);
        // The UNSANITISED name too: the filename has had punctuation replaced and
        // may have been deduplicated, so it is not the author's string. The F-line
        // `name=` wants the author's.
        std::vector<std::string> matRawNameByAiMat(scene->mNumMaterials);
        {
            // The source's factors and textures. Textures are cooked beside the model when options.textures is
            // on and the caller says where it will be written (context->materialRoot); else factors only.
            ModelTextureCook cook(context && options.textures ? context->materialRoot : std::string(), stem,
                                  Sys::PathToUtf8(Sys::U8Path(sourcePath).parent_path()));
            ImportedMaterialNotes matNotes;
            std::vector<char> matHasColors(scene->mNumMaterials, 0);   // glTF COLOR_0 multiplies base colour
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
                if (scene->mMeshes[mi] && scene->mMeshes[mi]->HasVertexColors(0) &&
                    scene->mMeshes[mi]->mMaterialIndex < scene->mNumMaterials)
                    matHasColors[scene->mMeshes[mi]->mMaterialIndex] = 1;
            std::unordered_map<unsigned, std::string> matIdxToFile;
            for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
            {
                const aiMesh* ai = scene->mMeshes[mi];
                const unsigned matIdx = ai->mMaterialIndex;
                if (matIdxToFile.count(matIdx)) continue;

                const aiMaterial* aiMat = scene->mMaterials[matIdx];
                MaterialComponent mc{};
                aiString aiName;
                aiMat->Get(AI_MATKEY_NAME, aiName);
                std::string matName = SanitiseName(
                    aiName.length > 0 ? aiName.C_Str()
                                      : ("material_" + std::to_string(matIdx)).c_str());

                FillMaterialFromAssimp(*aiMat, *scene, gltfFamily, matHasColors[matIdx] != 0, cook, mc, matNotes,
                                       options.flipGreen);
                const std::string matFileName = "Materials/" + matName + ".imat";
                const std::string matRelPath  = stem + "/" + matFileName;
                result.files.push_back({ matRelPath, Resource::BuildMaterialBlob(mc) });

                matIdxToFile[matIdx]          = matFileName;
                matFileNameByAiMat[matIdx]    = matFileName;
                matRawNameByAiMat[matIdx]     = (aiName.length > 0)
                                              ? std::string(aiName.C_Str())
                                              : ("material_" + std::to_string(matIdx));
            }
            LOG_INFO("SceneImporter: generated %zu .imat files", matIdxToFile.size());
            if (cook.Enabled())
                LOG_INFO("SceneImporter: materials from the source - %u textured, %u texture(s) cooked into "
                         "'%s/Textures', %u failed, %u reference(s) unresolved, %u on another UV set (sampled at 0)",
                         matNotes.textured, cook.Cooked(), stem.c_str(), cook.Failed(), matNotes.missingImages,
                         matNotes.otherUvSet);
            if (matNotes.transmissive)
                LOG_INFO("SceneImporter: %u Transmissive material(s) from glTF transmission, %u with a "
                         "transmission / thickness map the engine does not carry (its factor stands alone)",
                         matNotes.transmissive, matNotes.transmissionMaps);
            for (auto& [rel, blob] : cook.Files()) result.files.push_back({ rel, std::move(blob) });
        }

        // Merge every drawn aiMesh into ONE .meshlib for the scene; node management lives in the .iscn.
        // Each F-line points at a submesh (entry) index in the library. Submesh K is the K-th kept aiMesh
        // (empty and collision-only aiMeshes are skipped), so after the first skip the submesh and raw
        // aiMesh indices differ. aiMeshToSubIdx is the one translation between the two; everything written
        // into a file uses the submesh space.
        std::vector<const aiMesh*> allMeshes;
        std::vector<uint32_t>      allMatIdx;
        std::vector<uint32_t>      allAiIdx;   // submesh idx -> original aiMesh idx
        std::vector<int>           aiMeshToSubIdx(scene->mNumMeshes, -1);
        allMeshes.reserve(scene->mNumMeshes);
        allMatIdx.reserve(scene->mNumMeshes);
        allAiIdx.reserve(scene->mNumMeshes);

        for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi)
        {
            const aiMesh* ai = scene->mMeshes[mi];
            if (!ai || ai->mNumVertices == 0 || ai->mNumFaces == 0)
            {
                LOG_WARNING("SceneImporter: aiMesh[%u] empty — skipping", mi);
                continue;
            }
            if (collisionOnlyMeshes.count(mi)) continue;   // a collision shape's geometry: never drawn
            aiMeshToSubIdx[mi] = static_cast<int>(allMeshes.size());
            allMeshes.push_back(ai);
            allMatIdx.push_back(ai->mMaterialIndex);
            allAiIdx.push_back(mi);
        }

        // Bake the mesh node's transform into SKINNED geometry. Skinning output is in MODEL space
        // (skin[b] = inverseBindPose[b] * localToModel[b]), but the mesh entity is drawn with its own
        // GlobalTransform (rootGT * mesh node TRS), and Assimp's FBX convention also folds the node
        // transform into aiBone::mOffsetMatrix - unbaked it would apply twice to the mesh and once to the
        // AABB. So SkeletonImporter divides G (the mesh node's global transform) out of the inverse bind pose
        // and this bakes G into the vertices: the .meshlib entry is model-space bind geometry (as the PMX path
        // produces) and the N-record gets an identity TRS below. Static meshes keep the node transform on the
        // entity.
        std::vector<XMFLOAT4X4> preTransforms;
        uint32_t bakedMeshes = 0;
        if (hasSkel)
        {
            XMFLOAT4X4 identity; XMStoreFloat4x4(&identity, XMMatrixIdentity());
            preTransforms.assign(allMeshes.size(), identity);
            for (size_t s = 0; s < allMeshes.size(); ++s)
            {
                const uint32_t mi = allAiIdx[s];
                if (mi >= skelResult.meshIsSkinned.size() || !skelResult.meshIsSkinned[mi]) continue;
                if (mi >= skelResult.meshNodeGlobal.size()) continue;
                preTransforms[s] = skelResult.meshNodeGlobal[mi];
                ++bakedMeshes;
            }
        }

        std::vector<MeshFileRef> fileRefs;
        const std::string mergedBase = stem + ".meshlib";
        size_t meshlibFileIdx = SIZE_MAX;   // its slot in result.files (the identity is added below)
        if (!allMeshes.empty())
        {
            // One .meshlib with an entry per kept aiMesh. flipX=false: aiProcess_ConvertToLeftHanded already
            // converted the coordinate space.
            std::vector<uint8_t> blob = BuildMeshLibBlob(
                allMeshes, allMatIdx, false, nullptr,
                preTransforms.empty() ? nullptr : &preTransforms);
            if (blob.empty())
            {
                LOG_ERROR("SceneImporter: BuildMeshLibBlob failed on '%s'",
                          sourcePath.c_str());
                return result;
            }
            meshlibFileIdx = result.files.size();
            result.files.push_back({ stem + "/" + mergedBase, std::move(blob) });

            // One F-line per kept aiMesh (library entry). All F-lines share file= (the one .meshlib); mesh=K
            // picks the MeshLibraryEntry by its index (meshId).
            fileRefs.reserve(allMeshes.size());
            for (size_t s = 0; s < allMeshes.size(); ++s)
            {
                MeshFileRef fr;
                fr.meshFile = mergedBase;
                fr.subIdx   = static_cast<uint32_t>(s);  // the meshId (MeshLibraryEntry index)
                fr.matFile  = (allMatIdx[s] < matFileNameByAiMat.size())
                              ? matFileNameByAiMat[allMatIdx[s]] : "";
                fr.name     = (allMatIdx[s] < matRawNameByAiMat.size())
                              ? matRawNameByAiMat[allMatIdx[s]] : std::string();
                fileRefs.push_back(std::move(fr));
            }
        }

        // Morph targets (blend shapes), only alongside a skeleton: the consumers (MorphComponent /
        // FacialAnimationSystem / BuildSkinJobs) resolve a morph by NAME through
        // SkeletonAsset::morphTargetNames and reach the deltas through the MeshSkinnedComponent only a
        // skinned mesh has. Every mesh whose shape keys move has one: a bone-less one is skinned rigidly
        // (SkeletonImportResult::synthesizedSkeleton).
        std::vector<int> morphMeshFileIdx;      // F-line indices carrying a block
        TargetsOfFile    targetsOfFile;         // per F index, its targets (for SourceShapeKeys)
        const std::string morphFileName = stem + ".imorph";
        if (hasSkel && !allMeshes.empty())
        {
            std::vector<uint8_t> morphBlob =
                BuildMorphLibBlob(allMeshes, preTransforms,
                                  skelResult.skeleton, morphMeshFileIdx, targetsOfFile, options.morphNormals);
            if (!morphBlob.empty())
                result.files.push_back({ stem + "/" + morphFileName, std::move(morphBlob) });
        }

        // .iskel, now that morphTargetNames are filled.
        if (hasSkel)
        {
            std::vector<uint8_t> iskelBlob =
                BuildIskelBlob(skelResult, scene->mNumMeshes, aiMeshToSubIdx);
            const std::string iskelRel = stem + "/" + skelFileName;
            result.files.push_back({ iskelRel, std::move(iskelBlob) });
            LOG_INFO("SceneImporter:   skeleton → '%s'", iskelRel.c_str());
        }

        // Remap each node's meshFileIndices from aiMesh idx to submesh idx (= F-line idx: F-lines are
        // emitted in submesh order), dropping refs to skipped aiMeshes. The raw aiMesh index is kept
        // alongside so the identity-TRS pass below can ask whether a node's meshes are skinned.
        std::vector<std::vector<int>> nodeAiMeshes(nodes.size());
        for (size_t n = 0; n < nodes.size(); ++n)
        {
            NodeRecord& nrec = nodes[n];
            std::vector<int> keptRefs, keptAi;
            keptRefs.reserve(nrec.meshFileIndices.size());
            keptAi.reserve(nrec.meshFileIndices.size());
            for (int aiRef : nrec.meshFileIndices)
            {
                const int sub = (aiRef >= 0 && aiRef < static_cast<int>(aiMeshToSubIdx.size()))
                                ? aiMeshToSubIdx[aiRef] : -1;
                if (sub < 0) continue;              // aiMesh skipped (empty or collision-only)
                keptRefs.push_back(sub);
                keptAi.push_back(aiRef);            // stays index-parallel with keptRefs
            }
            nrec.meshFileIndices.swap(keptRefs);
            nodeAiMeshes[n].swap(keptAi);
        }

        // Identity layer: name and key the library's Meshes and Sections. Grouped here: after the remap
        // above put every node's refs in submesh (= entry) space, and before the neutralise pass below
        // moves skinned refs onto the root - a Mesh is the author's object as the SOURCE has it, not as the
        // spawn layout needs it. One Mesh per node's set of entries; a node repeating an earlier node's set
        // is the same Mesh placed again. Keys are derived here and re-adopted from the previous file on
        // re-import (WriteModelImportFiles).
        if (meshlibFileIdx != SIZE_MAX)
        {
            const size_t S = allMeshes.size();
            std::vector<std::string> nodePath(nodes.size());
            for (size_t n = 0; n < nodes.size(); ++n)
            {
                const int p = nodes[n].parent;   // pre-order: a parent precedes its children
                nodePath[n] = (p >= 0 && static_cast<size_t>(p) < n)
                            ? nodePath[static_cast<size_t>(p)] + "/" + nodes[n].rawName
                            : nodes[n].rawName;
            }
            std::vector<char> sectionHasMorph(S, 0);
            for (int fi : morphMeshFileIdx)   // F index == submesh index
                if (fi >= 0 && static_cast<size_t>(fi) < S) sectionHasMorph[static_cast<size_t>(fi)] = 1;
            auto sectionSkinned = [&](size_t s)
            {
                const uint32_t mi = allAiIdx[s];
                return hasSkel && mi < skelResult.meshIsSkinned.size() && skelResult.meshIsSkinned[mi] != 0;
            };

            MeshIdentitySpec spec;
            spec.modelSource = "assimp|" + stem;
            spec.sections.resize(S);
            for (size_t s = 0; s < S; ++s)
                spec.sections[s].slotName = (allMatIdx[s] < matRawNameByAiMat.size())
                                          ? matRawNameByAiMat[allMatIdx[s]] : std::string();

            std::vector<int>                owner(S, -1);
            std::unordered_map<std::string, int> seenSets;          // a node's entry set -> its Mesh
            std::vector<int>                meshOfNode(nodes.size(), -1);   // a node's Mesh: where its collision goes
            for (size_t n = 0; n < nodes.size(); ++n)
            {
                const std::vector<int>& refs = nodes[n].meshFileIndices;
                if (refs.empty()) continue;
                std::string sig;
                for (int r : refs) sig += std::to_string(r) + ',';
                const auto [seen, added] = seenSets.emplace(sig, static_cast<int>(spec.meshes.size()));
                meshOfNode[n] = seen->second;
                if (!added) continue;

                MeshIdentitySpec::Mesh m;
                m.name      = nodes[n].rawName;
                m.sourceKey = "n:" + nodePath[n];
                for (int r : refs)
                {
                    const size_t s = static_cast<size_t>(r);
                    m.sections.push_back(static_cast<uint32_t>(r));
                    if (sectionSkinned(s))     m.flags |= MESH_IDENT_SKINNED;
                    if (sectionHasMorph[s])    m.flags |= MESH_IDENT_HAS_MORPH;
                    if (owner[s] < 0) owner[s] = static_cast<int>(spec.meshes.size());
                }
                spec.meshes.push_back(std::move(m));
            }
            // A section's source key is its owner's plus its slot, "#k" telling
            // apart two sections of one object that share a material.
            std::unordered_map<std::string, uint32_t> occurrence;
            for (size_t s = 0; s < S; ++s)
            {
                MeshIdentitySpec::Section& sec = spec.sections[s];
                const std::string base = (owner[s] >= 0)
                    ? spec.meshes[static_cast<size_t>(owner[s])].sourceKey + "/" + sec.slotName
                    : "a:" + std::string(allMeshes[s]->mName.C_Str()) + "/" + sec.slotName;
                sec.sourceKey = base + "#" + std::to_string(occurrence[base]++);
            }

            // Each collision target node's Mesh, by the source key it is keyed from.
            for (auto& [node, ic] : nodeCollision)
            {
                const int m = (node >= 0 && static_cast<size_t>(node) < meshOfNode.size()) ? meshOfNode[static_cast<size_t>(node)] : -1;
                if (m < 0) continue;
                ic.meshSourceKey = spec.meshes[static_cast<size_t>(m)].sourceKey;
                ic.meshName      = spec.meshes[static_cast<size_t>(m)].name;
                result.collision.push_back(std::move(ic));
            }

            // Each Mesh's shape-key records from the source (presets, defaults).
            result.shapeKeys = SourceShapeKeys(spec, targetsOfFile, presets, gltfFamily);
            if (!result.shapeKeys.empty())
            {
                size_t nPresets = 0, nDefaults = 0;
                for (const ImportedShapeKeys& is : result.shapeKeys)
                {
                    nPresets  += is.setup.presets.size();
                    nDefaults += is.setup.meta.size();
                }
                LOG_INFO("SceneImporter: shape-key records from the source - %zu preset record(s), %zu default "
                         "weight(s) over %zu Mesh(es)", nPresets, nDefaults, result.shapeKeys.size());
            }

            std::vector<uint8_t>& libBlob = result.files[meshlibFileIdx].blob;
            MeshIdentity id;
            std::string  why;
            if (BuildMeshIdentity(spec, libBlob.data(), libBlob.size(), id, &why) &&
                WriteMeshLibIdentity(libBlob, id, &why))
                LOG_INFO("SceneImporter: identity - %zu mesh(es) over %zu section(s)",
                         id.meshes.size(), id.sections.size());
            else
                LOG_WARNING("SceneImporter: identity layer not written (%s) - the model "
                            "loads, but its meshes have no stable keys", why.c_str());
        }

        // Neutralise the N-record of nodes whose geometry was baked: the baked vertices already carry the
        // node's transform, so the spawned entity must not apply it again. Two shapes:
        //   * a node whose meshes are ALL skinned, with no children and only identity ancestors below the
        //     root: keep the node (its Outliner grouping and name survive) with an identity TRS. An
        //     ancestor that moves it (a mesh under a bone's node: a rigid skin to that bone) needs the
        //     other shape;
        //   * anything else (children, or a mix with static meshes): the node keeps its transform for
        //     those, and the skinned refs move up to the root N-record, which is model space by definition.
        // F-line ORDER is untouched either way: prefab per-part data is applied by F-line position.
        uint32_t identityNodes = 0, movedRefs = 0;
        if (hasSkel && bakedMeshes > 0 && !nodes.empty())
        {
            std::vector<bool> hasChild(nodes.size(), false);
            for (const NodeRecord& n : nodes)
                if (n.parent >= 0 && n.parent < static_cast<int>(hasChild.size()))
                    hasChild[n.parent] = true;

            // Is everything between a node and the root the identity? (Pre-order:
            // a parent precedes its children, and a node with children is never
            // neutralised below, so these are the source's own transforms.)
            auto identityTrs = [](const NodeRecord& r)
            {
                constexpr float e = 1e-6f;
                return std::fabs(r.tx) < e && std::fabs(r.ty) < e && std::fabs(r.tz) < e &&
                       std::fabs(r.qx) < e && std::fabs(r.qy) < e && std::fabs(r.qz) < e &&
                       std::fabs(std::fabs(r.qw) - 1.f) < e && std::fabs(r.sx - 1.f) < e &&
                       std::fabs(r.sy - 1.f) < e && std::fabs(r.sz - 1.f) < e;
            };
            std::vector<bool> chainIdentity(nodes.size(), true);   // ancestors below the root
            for (size_t n = 1; n < nodes.size(); ++n)
            {
                const int par = nodes[n].parent;
                if (par > 0 && par < static_cast<int>(n))
                    chainIdentity[n] = chainIdentity[static_cast<size_t>(par)] &&
                                       identityTrs(nodes[static_cast<size_t>(par)]);
            }

            auto isSkinned = [&](int aiIdx) {
                return aiIdx >= 0 && aiIdx < static_cast<int>(skelResult.meshIsSkinned.size())
                    && skelResult.meshIsSkinned[aiIdx] != 0;
            };

            for (size_t n = 0; n < nodes.size(); ++n)
            {
                NodeRecord& nr = nodes[n];
                if (nr.meshFileIndices.empty()) continue;
                if (static_cast<int>(n) == 0) continue;   // root is model space already

                uint32_t skinned = 0;
                for (int aiIdx : nodeAiMeshes[n]) if (isSkinned(aiIdx)) ++skinned;
                if (skinned == 0) continue;               // purely static node: untouched

                const bool allSkinned = (skinned == nodeAiMeshes[n].size());
                if (allSkinned && !hasChild[n] && chainIdentity[n])
                {
                    nr.tx = nr.ty = nr.tz = 0.f;
                    nr.qx = nr.qy = nr.qz = 0.f; nr.qw = 1.f;
                    nr.sx = nr.sy = nr.sz = 1.f;
                    ++identityNodes;
                }
                else
                {
                    // Move only the skinned refs onto node 0.
                    std::vector<int> keep;
                    for (size_t k = 0; k < nr.meshFileIndices.size(); ++k)
                    {
                        const int aiIdx = (k < nodeAiMeshes[n].size()) ? nodeAiMeshes[n][k] : -1;
                        if (isSkinned(aiIdx))
                        { nodes[0].meshFileIndices.push_back(nr.meshFileIndices[k]); ++movedRefs; }
                        else keep.push_back(nr.meshFileIndices[k]);
                    }
                    nr.meshFileIndices.swap(keep);
                }
            }
            LOG_INFO("SceneImporter: baked node transforms into %u skinned mesh(es) — "
                     "%u node record(s) neutralised, %u ref(s) re-parented to the root",
                     bakedMeshes, identityNodes, movedRefs);
        }

        // Build and append the .iscn blob (<stem>/<stem>.iscn, beside the other files).
        const std::string iscnRel  = stem + "/" + stem + ".iscn";
        std::vector<uint8_t> iscnBlob = BuildIscnBlob(stem, nodes, fileRefs, skelFileName,
                                                      morphMeshFileIdx, morphFileName);
        result.files.push_back({ iscnRel, std::move(iscnBlob) });

        result.success = true;
        if (allMeshes.empty())
            LOG_INFO("SceneImporter: done — animation-only import: 1 .iscn%s, %zu nodes "
                     "(no meshes; use the .iskel as a retarget source)",
                     hasSkel ? " + 1 .iskel" : "", nodes.size());
        else
            LOG_INFO("SceneImporter: done — 1 .meshlib (%zu meshes) + 1 .iscn%s%s, %zu nodes",
                     fileRefs.size(),
                     hasSkel ? " + 1 .iskel" : "",
                     morphMeshFileIdx.empty() ? "" : " + 1 .imorph", nodes.size());
        return result;
    }

} // namespace Resource
