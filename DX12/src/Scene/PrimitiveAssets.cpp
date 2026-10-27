#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Scene/PrimitiveAssets.h"

#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"          // PercentEncodeStrict (F-line name=)
#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshletBuilder.h"     // AddMeshletsToMeshLib
#include "Resource/ProceduralMesh.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>

using namespace Resource;

namespace
{
    constexpr const char* kNames[] = { "Cube", "Sphere", "Cone", "Plane", "Torus" };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(PrimitiveMeshType::Count),
                  "one name per PrimitiveMeshType");

    // Laid out in a row for the model's own preview: 1.5 m apart along X.
    constexpr float kSpacing = 1.5f;

    // The engine default material MeshSpawner gives a primitive, relative to
    // engine/meshes/ (F-line paths are relative to the model's folder).
    constexpr const char* kMaterial = "../textures/Default_mat.imat";

    ProceduralMesh::MeshData Shape(int i)
    {
        switch (static_cast<PrimitiveMeshType>(i))
        {
        case PrimitiveMeshType::Cube:   return ProceduralMesh::Cube();
        case PrimitiveMeshType::Sphere: return ProceduralMesh::Sphere();
        case PrimitiveMeshType::Cone:   return ProceduralMesh::Cone();
        case PrimitiveMeshType::Plane:  return ProceduralMesh::Plane();
        default:                        return ProceduralMesh::Torus();
        }
    }

    bool Fail(std::string* err, std::string why)
    {
        if (err) *err = std::move(why);
        LOG_ERROR("[PRIMITIVES] %s", why.c_str());
        return false;
    }
}

const char* PrimitiveAssets::MeshName(int primitiveType)
{
    return primitiveType >= 0 && primitiveType < static_cast<int>(PrimitiveMeshType::Count) ? kNames[primitiveType]
                                                                                            : nullptr;
}

bool PrimitiveAssets::Write(const std::string& dirIn, Stats& out, std::string* err)
{
    out = Stats{};
    const std::string dir  = dirIn.empty() ? std::string("engine/meshes") : dirIn;
    const std::string lib  = dir + "/Primitives.meshlib";
    const std::string iscn = dir + "/Primitives.iscn";
    const int         n    = static_cast<int>(PrimitiveMeshType::Count);

    // The library: position / normal / uv0 / tangent, interleaved.
    const uint32_t            flags = MESHLIB_FLAG_HAS_TANGENT;
    const MeshLibVertexLayout L     = ComputeMeshLibVertexLayout(flags);
    std::vector<MeshLibraryEntry> entries;
    std::vector<uint8_t>          vb;
    std::vector<uint32_t>         ib;
    for (int s = 0; s < n; ++s)
    {
        const ProceduralMesh::MeshData m = Shape(s);
        MeshLibraryEntry e{};
        e.vertexStart = static_cast<uint32_t>(vb.size() / L.stride);
        e.vertexCount = static_cast<uint32_t>(m.positions.size());
        e.indexStart  = static_cast<uint32_t>(ib.size());
        e.indexCount  = static_cast<uint32_t>(m.indices.size());
        float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
        for (size_t v = 0; v < m.positions.size(); ++v)
        {
            uint8_t vert[64] = {};
            std::memcpy(vert + L.posOffset, m.positions[v].data(), 12);
            std::memcpy(vert + L.normalOffset, m.normals[v].data(), 12);
            std::memcpy(vert + L.uv0Offset, m.uvs[v].data(), 8);
            std::memcpy(vert + L.tangentOffset, m.tangents[v].data(), 16);
            vb.insert(vb.end(), vert, vert + L.stride);
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], m.positions[v][k]);
                hi[k] = std::max(hi[k], m.positions[v][k]);
            }
        }
        std::memcpy(e.aabbMin, lo, sizeof(lo));
        std::memcpy(e.aabbMax, hi, sizeof(hi));
        e.defaultMaterialIdx = 0;
        for (uint16_t idx : m.indices) ib.push_back(e.vertexStart + idx);   // absolute into the library VB
        entries.push_back(e);
    }

    MeshLibraryMetadata meta{};
    meta.meshCount    = static_cast<uint32_t>(entries.size());
    meta.vertexCount  = static_cast<uint32_t>(vb.size() / L.stride);
    meta.indexCount   = static_cast<uint32_t>(ib.size());
    meta.vertexStride = static_cast<uint16_t>(L.stride);
    meta.indexStride  = 4;
    meta.flags        = flags;
    AssetHeader hdr{};
    hdr.magic        = MAGIC_MESHLIB;
    hdr.version      = CurrentVersion(MAGIC_MESHLIB);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::MeshLibrary);
    hdr.metadataSize = sizeof(MeshLibraryMetadata);
    const size_t entryBytes = entries.size() * sizeof(MeshLibraryEntry);
    hdr.dataSize = static_cast<uint32_t>(entryBytes + vb.size() + ib.size() * 4);
    std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(MeshLibraryMetadata) + hdr.dataSize);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr, sizeof(hdr));           dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta));         dst += sizeof(meta);
    std::memcpy(dst, entries.data(), entryBytes);  dst += entryBytes;
    std::memcpy(dst, vb.data(), vb.size());        dst += vb.size();
    std::memcpy(dst, ib.data(), ib.size() * 4);

    // Identity: one Mesh per shape, keyed by what it is ("primitive:cube"), so a re-bake or a
    // changed shape keeps every StaticMesh's key.
    MeshIdentitySpec spec;
    spec.modelSource = "engine|Primitives";
    spec.sections.resize(entries.size());
    for (int s = 0; s < n; ++s)
    {
        std::string lower = kNames[s];
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        MeshIdentitySpec::Mesh m;
        m.name      = kNames[s];
        m.sourceKey = "primitive:" + lower;
        m.sections  = { static_cast<uint32_t>(s) };
        spec.meshes.push_back(std::move(m));
        spec.sections[static_cast<size_t>(s)].slotName  = "Default";
        spec.sections[static_cast<size_t>(s)].sourceKey = "primitive:" + lower + "/Default#0";
    }
    MeshIdentity id;
    std::string  why;
    if (!BuildMeshIdentity(spec, blob.data(), blob.size(), id, &why) || !WriteMeshLibIdentity(blob, id, &why))
        return Fail(err, "the identity does not fit: " + why);
    // The shapes' meshlets (deterministic, like the rest of the bake).
    MeshletBuildStats mstats;
    if (!AddMeshletsToMeshLib(blob, MeshletBuildOptions{}, &mstats, &why))
        return Fail(err, "the meshlets could not be built: " + why);
    out.meshletClusters = mstats.clusters;

    // The model: a root and one node per shape, in a row.
    std::ostringstream ss;
    ss << "# ISCN engine scene container (the Create > 3D Object shapes - Scene/PrimitiveAssets.cpp)\n";
    ss << "S nodes=" << (n + 1) << " meshfiles=" << n << " source=Primitives\n";
    // The keys are in the file too - each node's `mkey=`, each F-line's `sec=`, one ME record
    // per shape - in the form SceneInstanceLoader::StampModelKeys writes, so a stamp pass changes
    // nothing.
    ss << "N idx=0 parent=-1 tx=0 ty=0 tz=0 qx=0 qy=0 qz=0 qw=1 sx=1 sy=1 sz=1 mc=0 name=Primitives\n";
    for (int s = 0; s < n; ++s)
        ss << "N idx=" << (s + 1) << " parent=0 tx=" << (static_cast<float>(s - n / 2) * kSpacing)
           << " ty=0 tz=0 qx=0 qy=0 qz=0 qw=1 sx=1 sy=1 sz=1 mc=1 mr0=" << s
           << " mkey=" << MeshKeyToHex(id.meshes[static_cast<size_t>(s)].key) << " name=" << kNames[s] << "\n";
    for (int s = 0; s < n; ++s)
        ss << "F idx=" << s << " file=Primitives.meshlib mesh=" << s
           << " sec=" << MeshKeyToHex(id.sections[static_cast<size_t>(s)].key) << " name="
           << AssetRefs::PercentEncodeStrict("Default") << " mat=" << kMaterial << "\n";
    for (int s = 0; s < n; ++s)
        ss << "ME key=" << MeshKeyToHex(id.meshes[static_cast<size_t>(s)].key)
           << " name=" << AssetRefs::PercentEncodeStrict(kNames[s]) << "\n";
    const std::string text = ss.str();
    SceneMetadata smeta{};
    smeta.nodeCount     = static_cast<uint32_t>(n + 1);
    smeta.meshFileCount = static_cast<uint32_t>(n);
    smeta.textLength    = static_cast<uint32_t>(text.size());
    AssetHeader shdr{};
    shdr.magic        = MAGIC_SCENE;
    shdr.version      = CurrentVersion(MAGIC_SCENE);
    shdr.resourceType = static_cast<uint16_t>(ResourceType::Scene);
    shdr.metadataSize = sizeof(SceneMetadata);
    shdr.dataSize     = smeta.textLength + 1;
    std::vector<uint8_t> scene(sizeof(AssetHeader) + sizeof(SceneMetadata) + shdr.dataSize, 0);
    std::memcpy(scene.data(), &shdr, sizeof(shdr));
    std::memcpy(scene.data() + sizeof(shdr), &smeta, sizeof(smeta));
    std::memcpy(scene.data() + sizeof(shdr) + sizeof(smeta), text.data(), text.size());

    Sys::CreateParentDirs(lib);
    if (!Sys::WriteFileBytesAtomic(lib, blob.data(), blob.size())) return Fail(err, "cannot write '" + lib + "'");
    if (!Sys::WriteFileBytesAtomic(iscn, scene.data(), scene.size())) return Fail(err, "cannot write '" + iscn + "'");

    out.iscn     = iscn;
    out.meshlib  = lib;
    out.meshes   = static_cast<uint32_t>(n);
    out.vertices = meta.vertexCount;
    out.indices  = meta.indexCount;
    LOG_INFO("[PRIMITIVES] wrote '%s' + '%s': %u meshes, %u vertices, %u indices, %zu bytes", iscn.c_str(),
             lib.c_str(), out.meshes, out.vertices, out.indices, blob.size());
    return true;
}
