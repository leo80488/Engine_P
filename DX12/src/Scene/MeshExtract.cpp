#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Scene/MeshExtract.h"

#include "Resource/AssetFS.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"          // PercentEncode / PercentEncodeStrict
#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshletBuilder.h"     // the new library's MLET
#include "Scene/MeshGeometry.h"          // the library reader
#include "Scene/SceneInstanceLoader.h"   // ReadMaterialBindings
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Resource;
namespace fs = std::filesystem;

namespace
{
    // One library: its bytes, its layout, and the identity MeshLibrary::Load
    // would give it - the shared reader (Scene/MeshGeometry.h), which the Mesh
    // Editor decodes its overlays from too.
    using SourceLib = MeshGeometry::Library;
    using MeshGeometry::ReadLibrary;
    using MeshGeometry::EntryAt;

    bool EndsWithCI(const std::string& s, const char* ext)
    {
        const size_t n = std::strlen(ext);
        if (s.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != ext[i]) return false;
        return true;
    }

    bool Fail(MeshExtract::Result& out, const char* code, std::string why)
    {
        out.code  = code;
        out.error = std::move(why);
        return false;
    }

    // The .iscn's text payload (header + SceneMetadata + text).
    bool ReadIscnText(const std::string& path, std::string& text)
    {
        std::vector<uint8_t> blob;
        if (!AssetFS::Get().ReadFile(path, blob) || blob.size() < sizeof(AssetHeader) + sizeof(SceneMetadata))
            return false;
        AssetHeader h{};
        std::memcpy(&h, blob.data(), sizeof(h));
        if (h.magic != MAGIC_SCENE || h.metadataSize < sizeof(SceneMetadata)) return false;
        SceneMetadata m{};
        std::memcpy(&m, blob.data() + sizeof(AssetHeader), sizeof(m));
        const size_t at = sizeof(AssetHeader) + h.metadataSize;
        if (at > blob.size()) return false;
        const size_t len = std::min<size_t>(m.textLength, blob.size() - at);
        text.assign(reinterpret_cast<const char*>(blob.data() + at), len);
        return true;
    }

    std::string FileSafe(std::string s)
    {
        for (char& c : s)
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
                c == '*' || static_cast<unsigned char>(c) < 32)
                c = '_';
        while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
        return s.empty() ? std::string("Mesh") : s;
    }

    std::string LibraryPathFor(const std::string& iscn)
    {
        fs::path p = Sys::U8Path(iscn);
        p.replace_extension(".meshlib");
        return Sys::PathToUtf8(p);
    }

    // @p target relative to @p dir, '/' separated; both made absolute first so a
    // project-relative and an absolute spelling still meet.
    std::string RelativeTo(const std::string& target, const fs::path& dir)
    {
        std::error_code ec;
        const fs::path t = fs::absolute(Sys::U8Path(target), ec).lexically_normal();
        const fs::path d = fs::absolute(dir, ec).lexically_normal();
        std::string rel = Sys::PathToUtf8(t.lexically_relative(d));
        for (char& c : rel) if (c == '\\') c = '/';
        return rel.empty() ? Sys::RelativizeAssetPath(target) : rel;
    }
}

std::string MeshExtract::DefaultDestination(const std::string& model, const std::string& meshName)
{
    const fs::path dir  = Sys::U8Path(model).parent_path();
    const std::string stem = FileSafe(meshName);
    for (int i = 0; i < 1000; ++i)
    {
        const std::string name = i == 0 ? stem : stem + "_" + std::to_string(i);
        const std::string iscn = Sys::PathToUtf8(dir / Sys::U8Path(name + ".iscn"));
        if (!Sys::FileExists(iscn) && !Sys::FileExists(LibraryPathFor(iscn)))
        {
            std::string out = iscn;
            for (char& c : out) if (c == '\\') c = '/';
            return out;
        }
    }
    return Sys::PathToUtf8(dir / Sys::U8Path(stem + ".iscn"));
}

bool MeshExtract::Extract(const std::string& model, uint64_t meshKey, const std::string& destIscn,
                          bool overwrite, Result& out)
{
    out = Result{};
    out.key = meshKey;
    if (model.empty() || !EndsWithCI(model, ".iscn")) return Fail(out, "bad_value", "the model is a .iscn path");
    if (destIscn.empty() || !EndsWithCI(destIscn, ".iscn"))
        return Fail(out, "bad_value", "the destination is a .iscn path");
    if (!AssetFS::Get().Exists(model)) return Fail(out, "load_failed", "no such model: " + model);
    const std::string destLib = LibraryPathFor(destIscn);
    if (MeshLibCanonicalPath(destIscn) == MeshLibCanonicalPath(model))
        return Fail(out, "bad_value", "the destination is the model itself");

    // The Mesh: the first library the model names that has the key (MeshAssetRegistry::BuildModel's
    // order, so this is what a StaticMesh of it draws).
    std::vector<SceneInstanceLoader::MaterialBinding> binds;
    if (!SceneInstanceLoader::ReadMaterialBindings(model, binds))
        return Fail(out, "load_failed", "the model '" + model + "' cannot be read");
    std::vector<std::string> libOrder;
    for (const auto& b : binds)
        if (EndsWithCI(b.meshAbsPath, ".meshlib") &&
            std::find(libOrder.begin(), libOrder.end(), b.meshAbsPath) == libOrder.end())
            libOrder.push_back(b.meshAbsPath);

    SourceLib lib;
    int       meshIdx = -1;
    for (const std::string& path : libOrder)
    {
        std::string why;
        SourceLib   candidate;
        if (!ReadLibrary(path, candidate, why)) return Fail(out, "load_failed", why);
        const int m = candidate.id.FindMesh(meshKey);
        if (m >= 0) { lib = std::move(candidate); meshIdx = m; break; }
    }
    if (meshIdx < 0)
        return Fail(out, "not_found", "the model has no mesh with key " + MeshKeyToHex(meshKey) +
                                      " (Asset.meshes lists them)");
    const MeshIdentity::Mesh& mesh = lib.id.meshes[static_cast<size_t>(meshIdx)];
    out.meshName = mesh.name;
    if (mesh.sections.empty()) return Fail(out, "not_found", "the mesh has no sections");
    if (MeshLibCanonicalPath(destLib) == MeshLibCanonicalPath(lib.path))
        return Fail(out, "bad_value", "the destination's .meshlib is the source library");

    // Refusals: skinned / morph Meshes (their data is keyed by the source model's F index).
    const std::string libKey = MeshLibCanonicalPath(lib.path);
    std::unordered_set<uint32_t> mine(mesh.sections.begin(), mesh.sections.end());
    if (mesh.flags & (MESH_IDENT_SKINNED | MESH_IDENT_HAS_MORPH))
        return Fail(out, "unsupported", "'" + mesh.name + "' is skinned or has shape keys - extracting it is "
                                        "not supported yet (its skin / morph data belongs to the source model)");
    {
        std::string text;
        if (ReadIscnText(model, text))
        {
            std::istringstream in(text);
            std::string        line;
            while (std::getline(in, line))
            {
                // An index-derived identity carries no skinned flag: a model naming
                // a skeleton is skinned throughout (Resource::MeshCountsAsSkinned).
                // An importer's is exact - its static Meshes extract like any other.
                if (line.rfind("K ", 0) == 0 && IsIndexDerivedIdentity(lib.id.flags))
                    return Fail(out, "unsupported", "the model is skinned (it names a skeleton) - extracting one of "
                                                    "its meshes is not supported yet");
                if (line.rfind("M ", 0) == 0)
                {
                    const size_t at = line.find("idx=");
                    const uint32_t fi = at == std::string::npos ? UINT32_MAX
                                                                : static_cast<uint32_t>(std::strtoul(line.c_str() + at + 4, nullptr, 10));
                    if (fi < binds.size() && MeshLibCanonicalPath(binds[fi].meshAbsPath) == libKey &&
                        mine.count(binds[fi].meshId))
                        return Fail(out, "unsupported", "'" + mesh.name + "' has shape keys - extracting it is not "
                                                        "supported yet");
                }
            }
        }
    }
    if (!overwrite && (Sys::FileExists(destIscn) || Sys::FileExists(destLib)))
        return Fail(out, "exists", "'" + destIscn + "' (or its .meshlib) already exists");

    // Default materials, by MeshAssetRegistry::BuildModel's rule: the first F-line of the model
    // naming (library, entry), else the library's own model (its same-stem .iscn).
    std::unordered_map<uint32_t, std::string> matOf;
    for (const auto& b : binds)
        if (MeshLibCanonicalPath(b.meshAbsPath) == libKey) matOf.emplace(b.meshId, b.matPath);
    {
        fs::path owner = Sys::U8Path(lib.path);
        owner.replace_extension(".iscn");
        const std::string ownerPath = Sys::PathToUtf8(owner);
        std::vector<SceneInstanceLoader::MaterialBinding> ob;
        if (MeshLibCanonicalPath(ownerPath) != MeshLibCanonicalPath(model) && AssetFS::Get().Exists(ownerPath) &&
            SceneInstanceLoader::ReadMaterialBindings(ownerPath, ob))
            for (const auto& b : ob)
                if (MeshLibCanonicalPath(b.meshAbsPath) == libKey) matOf.emplace(b.meshId, b.matPath);
    }

    // The new library: the sections' vertex ranges, indices re-based.
    const uint32_t stride = lib.meta.vertexStride;
    std::vector<MeshLibraryEntry> entries;
    std::vector<uint8_t>          vb;
    std::vector<uint32_t>         ib;
    for (uint32_t e : mesh.sections)
    {
        if (e >= lib.meta.meshCount) return Fail(out, "load_failed", "a section names an entry past the library's end");
        const MeshLibraryEntry src = EntryAt(lib, e);
        if (size_t(src.indexStart) + src.indexCount > lib.meta.indexCount)
            return Fail(out, "load_failed", "an entry's indices run past the library's index buffer");
        const uint8_t* srcIdx = lib.blob.data() + lib.ibAt + size_t(src.indexStart) * 4;
        // The vertex range the indices really land in (normally exactly the
        // entry's own), so an entry whose indices stray is still copied whole.
        uint32_t lo = src.vertexStart, hi = src.vertexStart + src.vertexCount;
        for (uint32_t i = 0; i < src.indexCount; ++i)
        {
            uint32_t v = 0;
            std::memcpy(&v, srcIdx + size_t(i) * 4, 4);
            lo = std::min(lo, v);
            hi = std::max(hi, v + 1);
        }
        if (hi > lib.meta.vertexCount || lo > hi)
            return Fail(out, "load_failed", "an entry's indices run past the library's vertex buffer");

        MeshLibraryEntry dst = src;
        dst.vertexStart        = static_cast<uint32_t>(vb.size() / stride);
        dst.vertexCount        = hi - lo;
        dst.indexStart         = static_cast<uint32_t>(ib.size());
        dst.defaultMaterialIdx = static_cast<uint32_t>(entries.size());
        const uint8_t* srcVb = lib.blob.data() + lib.vbAt + size_t(lo) * stride;
        vb.insert(vb.end(), srcVb, srcVb + size_t(hi - lo) * stride);
        for (uint32_t i = 0; i < src.indexCount; ++i)
        {
            uint32_t v = 0;
            std::memcpy(&v, srcIdx + size_t(i) * 4, 4);
            ib.push_back(v - lo + dst.vertexStart);
        }
        entries.push_back(dst);
    }

    MeshLibraryMetadata meta = lib.meta;
    meta.meshCount   = static_cast<uint32_t>(entries.size());
    meta.vertexCount = static_cast<uint32_t>(vb.size() / stride);
    meta.indexCount  = static_cast<uint32_t>(ib.size());
    meta.flags      &= ~MESHLIB_FLAG_HAS_TAIL;
    meta.reserved    = 0;
    AssetHeader hdr{};
    std::memcpy(&hdr, lib.blob.data(), sizeof(hdr));
    const size_t entryBytes = entries.size() * sizeof(MeshLibraryEntry);
    const size_t ibBytes    = ib.size() * 4;
    hdr.metadataSize = sizeof(MeshLibraryMetadata);
    hdr.dataSize     = static_cast<uint32_t>(entryBytes + vb.size() + ibBytes);
    std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(MeshLibraryMetadata) + hdr.dataSize);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr, sizeof(hdr));                 dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta));               dst += sizeof(meta);
    std::memcpy(dst, entries.data(), entryBytes);        dst += entryBytes;
    if (!vb.empty()) { std::memcpy(dst, vb.data(), vb.size()); dst += vb.size(); }
    if (!ib.empty()) std::memcpy(dst, ib.data(), ibBytes);

    // The identity keeps the same Mesh key and SectionKeys: that makes (new model, key) a drop-in
    // for (model, key).
    MeshIdentity id;
    id.modelKey = MeshIdentityHash("model|extract|" + MeshLibCanonicalPath(destIscn));
    id.flags    = 0;
    MeshIdentity::Mesh nm;
    nm.key       = mesh.key;
    nm.name      = mesh.name;
    nm.sourceKey = mesh.sourceKey;
    nm.flags     = mesh.flags;
    std::memcpy(nm.aabbMin, mesh.aabbMin, sizeof(nm.aabbMin));
    std::memcpy(nm.aabbMax, mesh.aabbMax, sizeof(nm.aabbMax));
    for (uint32_t i = 0; i < entries.size(); ++i)
    {
        nm.sections.push_back(i);
        const uint32_t e = mesh.sections[i];
        MeshIdentity::Section s = e < lib.id.sections.size() ? lib.id.sections[e] : MeshIdentity::Section{};
        if (s.key == 0) s.key = SectionKeyOfEntry(lib.blob.data(), lib.blob.size(), e);
        s.ownerMesh = 0;
        id.sections.push_back(std::move(s));
    }
    id.meshes.push_back(std::move(nm));
    id.RebuildIndex();
    std::string why;
    if (!WriteMeshLibIdentity(blob, id, &why))
        return Fail(out, "write_failed", "the identity does not fit the new library: " + why);
    // A new library is new geometry, so it gets its own meshlets (the source's MLET indexes the
    // source's vertex buffer). Extract refuses skinned Meshes, so every section is static.
    {
        MeshletBuildStats ms;
        if (AddMeshletsToMeshLib(blob, MeshletBuildOptions{}, &ms, &why))
        {
            if (ms.entriesBuilt) LogMeshletBuild("extract", destLib, ms, false);
        }
        else
            LOG_WARNING("[MESHEXTRACT] '%s': no meshlets (%s) - it draws through the vertex shader",
                        destLib.c_str(), why.c_str());
    }

    // The new model's .iscn text.
    const fs::path    destDir  = Sys::U8Path(destIscn).parent_path();
    const std::string libFile  = Sys::PathToUtf8(Sys::U8Path(destLib).filename());
    std::ostringstream ss;
    ss << "# ISCN engine scene container (Extract Mesh: " << MeshKeyToHex(mesh.key) << " of "
       << MeshLibCanonicalPath(model) << ")\n";
    ss << "S nodes=1 meshfiles=" << entries.size() << " source=" << AssetRefs::PercentEncode(mesh.name) << "\n";
    // The keys in the file, in SceneInstanceLoader::StampModelKeys's form so a stamp pass over it
    // changes nothing: each F-line's `sec=`, the node's `mkey=`, one ME record.
    ss << "N idx=0 parent=-1 tx=0 ty=0 tz=0 qx=0 qy=0 qz=0 qw=1 sx=1 sy=1 sz=1 mc=" << entries.size();
    for (size_t i = 0; i < entries.size(); ++i) ss << " mr" << i << "=" << i;
    ss << " mkey=" << MeshKeyToHex(mesh.key) << " name=" << AssetRefs::PercentEncode(mesh.name) << "\n";
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const uint32_t e = mesh.sections[i];
        ss << "F idx=" << i << " file=" << libFile << " mesh=" << i << " sec=" << MeshKeyToHex(id.sections[i].key);
        const std::string& slot = id.sections[i].slotName;
        if (!slot.empty()) ss << " name=" << AssetRefs::PercentEncodeStrict(slot);
        if (auto it = matOf.find(e); it != matOf.end() && !it->second.empty())
            ss << " mat=" << RelativeTo(it->second, destDir);
        ss << "\n";
    }
    ss << "ME key=" << MeshKeyToHex(mesh.key) << " name=" << AssetRefs::PercentEncodeStrict(mesh.name) << "\n";
    const std::string text = ss.str();
    SceneMetadata smeta{};
    smeta.nodeCount     = 1;
    smeta.meshFileCount = static_cast<uint32_t>(entries.size());
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

    // Write the library first, then the model that names it (the library is removed if the model
    // write fails).
    Sys::CreateParentDirs(destIscn);
    if (!Sys::WriteFileBytesAtomic(destLib, blob.data(), blob.size()))
        return Fail(out, "write_failed", "cannot write '" + destLib + "'");
    if (!Sys::WriteFileBytesAtomic(destIscn, scene.data(), scene.size()))
    {
        std::error_code ec;
        fs::remove(Sys::U8Path(destLib), ec);
        return Fail(out, "write_failed", "cannot write '" + destIscn + "'");
    }

    out.iscn     = Sys::RelativizeAssetPath(destIscn);
    out.meshlib  = Sys::RelativizeAssetPath(destLib);
    out.sections = static_cast<uint32_t>(entries.size());
    out.vertices = meta.vertexCount;
    out.indices  = meta.indexCount;
    LOG_INFO("[MESHEXTRACT] '%s' (%s) of '%s' -> '%s' + '%s': %u section(s), %u vertices, %u indices",
             mesh.name.c_str(), MeshKeyToHex(mesh.key).c_str(), model.c_str(), out.iscn.c_str(),
             out.meshlib.c_str(), out.sections, out.vertices, out.indices);
    return true;
}
