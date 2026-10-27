#include "Resource/ModelImportWrite.h"

#include "Resource/AssetHeader.h"
#include "Resource/AssetRefs.h"          // AugmentImatText - a new .imat's texture guids
#include "Resource/CollisionLibrary.h"   // a source's hulls
#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshletBuilder.h"     // the MLET chunk
#include "Resource/MorphLibrary.h"       // the .imorph's section keys
#include "Resource/ModelShapeKeys.h"     // the MK / MP records a re-import keeps
#include "Resource/ModelSockets.h"       // the SO records a re-import keeps
#include "Resource/ModelVirtualBones.h"  // the VB records a re-import keeps
#include "Resource/SkeletonAsset.h"      // the new skeleton's bone names (orphan sockets)
#include "Scene/SceneInstanceLoader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Case-insensitive extension test on a relative path. ASCII-only on purpose:
    // the extension is always ASCII, while the STEM routinely is not (this project
    // ships hundreds of non-ASCII asset names), and ::tolower on a UTF-8 byte is
    // locale-dependent.
    bool HasExt(const std::string& path, const char* ext)
    {
        const size_t n = std::strlen(ext);
        if (path.size() <= n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            char c = path[path.size() - n + i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != ext[i]) return false;
        }
        return true;
    }

    // A project path as compared: '/' separators, lower case (ASCII).
    std::string PathKey(std::string p)
    {
        for (char& c : p) if (c == '\\') c = '/';
        return Resource::AssetRefs::ToLowerAscii(std::move(p));
    }

    // materials = reuse: for each .imat an importer emitted (<stem>/Materials/<name>.imat), the ONE
    // project .imat called <name>.imat (case folded) - under asset/ or the folder the model is written
    // to, never in the model's own Materials folder. None, or several (a name that is not one
    // material): nothing - the importer's own is made.
    std::unordered_map<std::string, std::string> FindReusableMaterials(const std::filesystem::path& outDir,
                                                                       const std::vector<Resource::ImportFileOut>& files)
    {
        namespace fs = std::filesystem;
        std::unordered_set<std::string> ownFolders;
        for (const auto& f : files)
            if (HasExt(f.relativePath, ".imat"))
                ownFolders.insert(PathKey(Sys::RelativizeAssetPath(
                    Sys::PathToUtf8((outDir / Sys::U8Path(f.relativePath)).parent_path()))));
        std::unordered_map<std::string, std::vector<std::string>> byName;   // lower-case file name -> project paths
        std::unordered_set<std::string>                            seen;
        for (const fs::path& root : { fs::path("asset"), outDir })
        {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;
            for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                std::error_code fe;
                if (!it->is_regular_file(fe)) continue;
                const std::string file = Sys::PathToUtf8(it->path());
                if (!HasExt(file, ".imat")) continue;
                std::string rel = Sys::RelativizeAssetPath(file);
                for (char& c : rel) if (c == '\\') c = '/';
                if (!seen.insert(PathKey(rel)).second) continue;   // both roots reach it
                if (ownFolders.count(PathKey(Sys::PathToUtf8(Sys::U8Path(rel).parent_path())))) continue;
                byName[PathKey(Sys::PathToUtf8(Sys::U8Path(rel).filename()))].push_back(rel);
            }
        }
        std::unordered_map<std::string, std::string> out;
        for (const auto& f : files)
        {
            if (!HasExt(f.relativePath, ".imat")) continue;
            const auto it = byName.find(PathKey(Sys::PathToUtf8(Sys::U8Path(f.relativePath).filename())));
            if (it == byName.end()) continue;
            if (it->second.size() != 1)
            {
                LOG_INFO("ModelImportWrite:   materials = reuse: '%s' names %zu project materials (e.g. '%s') - "
                         "its own is made", f.relativePath.c_str(), it->second.size(), it->second[0].c_str());
                continue;
            }
            out.emplace(f.relativePath, it->second[0]);
        }
        return out;
    }

    // The text of a model file (.iscn): the payload after the header, up to its NUL.
    bool ReadModelText(const std::string& path, std::string& out)
    {
        std::vector<uint8_t> blob;
        if (!Sys::ReadFileBytes(path, blob) || !Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_SCENE))
            return false;
        const auto*  hdr  = reinterpret_cast<const Resource::AssetHeader*>(blob.data());
        const char*  payl = reinterpret_cast<const char*>(Resource::GetPayload(blob.data()));
        const size_t off  = static_cast<size_t>(payl - reinterpret_cast<const char*>(blob.data()));
        if (off > blob.size()) return false;
        size_t n = (std::min)(static_cast<size_t>(hdr->dataSize), blob.size() - off);
        if (const void* nul = std::memchr(payl, 0, n)) n = static_cast<size_t>(static_cast<const char*>(nul) - payl);
        out.assign(payl, n);
        return true;
    }
}

namespace Resource
{
    bool AdoptExistingMeshLibKeys(const std::string& outPath, std::vector<uint8_t>& freshBlob,
                                  MeshIdentityAdoption* stats, const MeshAdoptOptions* options)
    {
        if (stats) *stats = MeshIdentityAdoption{};
        std::vector<uint8_t> prev;
        if (!Sys::FileExists(outPath) || !Sys::ReadFileBytes(outPath, prev) ||
            !ValidateHeader(prev.data(), prev.size(), MAGIC_MESHLIB))
            return false;   // first import (or nothing usable to inherit from)

        MeshIdentity fresh;
        std::string  why;
        if (ParseMeshLibIdentity(freshBlob.data(), freshBlob.size(), fresh, &why) != MeshIdentityParse::Ok)
            return false;   // the importer wrote no identity - nothing to carry keys into

        MeshIdentity            previous;
        const MeshIdentityParse pp = ParseMeshLibIdentity(prev.data(), prev.size(), previous, &why);
        if (pp == MeshIdentityParse::Malformed)
        {
            LOG_WARNING("ModelImportWrite:   previous '%s' has an unreadable identity (%s) - "
                        "its mesh keys are NOT carried over", outPath.c_str(), why.c_str());
            return false;
        }
        const bool synthesized = (pp != MeshIdentityParse::Ok);
        if (synthesized)
            previous = SceneInstanceLoader::SynthesizeMeshLibIdentity(outPath, prev.data(), prev.size());

        MeshIdentityAdoption a = AdoptMeshIdentityKeys(previous, prev.data(), prev.size(),
                                                       fresh, freshBlob.data(), freshBlob.size(), options);
        if (!WriteMeshLibIdentity(freshBlob, fresh, &why))
        {
            LOG_WARNING("ModelImportWrite:   '%s': adopted keys could not be written (%s) - "
                        "the library keeps freshly derived keys", outPath.c_str(), why.c_str());
            return false;
        }
        LOG_INFO("ModelImportWrite:   '%s' keys: meshes %u kept (%zu renamed), %u new, %u gone; sections %u kept, "
                 "%u new, %u gone%s", outPath.c_str(), a.meshesKept, a.renamedMeshes.size(), a.meshesNew,
                 a.meshesGone, a.sectionsKept, a.sectionsNew, a.sectionsGone,
                 synthesized ? " (the previous file had no identity: its legacy keys were adopted)" : "");
        for (const auto& r : a.renamedMeshes)
            if (r.how != MeshIdentityAdoption::MeshRenamed::How::Name)
                LOG_INFO("ModelImportWrite:     Mesh '%s' is '%s' now - it keeps its key %s (%s)", r.from.c_str(),
                         r.to.c_str(), MeshKeyToHex(r.key).c_str(),
                         r.how == MeshIdentityAdoption::MeshRenamed::How::Explicit ? "a rename asked for"
                                                                                  : "the geometry agrees");
        for (const std::string& u : a.unresolvedRenames)
            LOG_WARNING("ModelImportWrite:     rename '%s' names no such Mesh - not applied", u.c_str());
        if (stats) *stats = std::move(a);
        return true;
    }

    // ==== The model's source (SR) ====
    std::string FormatModelSource(const ModelSourceRecord& s)
    {
        return "SR path=" + AssetRefs::PercentEncodeStrict(s.path) + " hash=" + MeshKeyToHex(s.hash) +
               " importer=" + AssetRefs::PercentEncodeStrict(s.importer) + "\n";
    }

    bool ParseModelSource(const std::string& text, ModelSourceRecord& out)
    {
        out = ModelSourceRecord{};
        for (size_t b = 0; b < text.size();)
        {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(b, e - b);
            b = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("SR ", 0) != 0) continue;
            // Every value is one token (percent-encoded).
            for (size_t p = 3; p < line.size();)
            {
                size_t q = line.find(' ', p);
                if (q == std::string::npos) q = line.size();
                const std::string tok = line.substr(p, q - p);
                p = q + 1;
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
                if (k == "path")          out.path     = AssetRefs::PercentDecode(v);
                else if (k == "importer") out.importer = AssetRefs::PercentDecode(v);
                else if (k == "hash")     MeshKeyFromHex(v, out.hash);
            }
            return out.Valid();
        }
        return false;
    }

    bool ReadModelSource(const std::string& iscnPath, ModelSourceRecord& out)
    {
        out = ModelSourceRecord{};
        std::string text;
        return ReadModelText(iscnPath, text) && ParseModelSource(text, out);
    }

    // ==== The import options (IM) ====
    namespace
    {
        using Opt = ModelImportOptions;
        const char* NameOf(Opt::Normals v)      { return v == Opt::Normals::Compute ? "compute" : "import"; }
        const char* NameOf(Opt::MorphNormals v)
        {
            return v == Opt::MorphNormals::Compute ? "compute" : v == Opt::MorphNormals::None ? "none" : "auto";
        }
        const char* NameOf(Opt::Collision v)
        {
            switch (v)
            {
            case Opt::Collision::None:    return "none";
            case Opt::Collision::Box:     return "box";
            case Opt::Collision::Convex:  return "convex";
            case Opt::Collision::Complex: return "complex";
            default:                      return "names";
            }
        }
        const char* NameOf(Opt::Materials v)    { return v == Opt::Materials::Reuse ? "reuse" : "create"; }

        bool ParseBool(const std::string& v, bool& out)
        {
            if (v == "1" || v == "true")  { out = true;  return true; }
            if (v == "0" || v == "false") { out = false; return true; }
            return false;
        }
    }

    std::string FormatImportOptions(const ModelImportOptions& o)
    {
        char smooth[32];
        std::snprintf(smooth, sizeof(smooth), "%g", o.smoothAngle);
        return std::string("IM normals=") + NameOf(o.normals) + " smooth=" + smooth +
               " morphs=" + (o.morphs ? "1" : "0") + " morphNormals=" + NameOf(o.morphNormals) +
               " collision=" + NameOf(o.collision) + " materials=" + NameOf(o.materials) +
               " textures=" + (o.textures ? "1" : "0") + " flipGreen=" + (o.flipGreen ? "1" : "0") +
               " animations=" + (o.animations ? "1" : "0") + " meshlets=" + (o.meshlets ? "1" : "0") +
               " lod=" + (o.lod ? "1" : "0") + "\n";
    }

    bool SetImportOption(ModelImportOptions& o, const std::string& name, const std::string& value, std::string* error)
    {
        auto bad = [&](const char* known)
        {
            if (error) *error = "'" + name + "' = '" + value + "' (" + known + ")";
            return false;
        };
        if (name == "normals")
        {
            if (value == "import")       o.normals = Opt::Normals::Import;
            else if (value == "compute") o.normals = Opt::Normals::Compute;
            else return bad("import | compute");
        }
        else if (name == "smooth")
        {
            char* end = nullptr;
            const float f = std::strtof(value.c_str(), &end);
            if (value.empty() || !end || *end != '\0' || !(f >= 0.f && f <= 180.f)) return bad("0 .. 180 degrees");
            o.smoothAngle = f;
        }
        else if (name == "morphs")    { if (!ParseBool(value, o.morphs))    return bad("1 | 0"); }
        else if (name == "textures")  { if (!ParseBool(value, o.textures))  return bad("1 | 0"); }
        else if (name == "flipGreen") { if (!ParseBool(value, o.flipGreen)) return bad("1 | 0"); }
        else if (name == "animations") { if (!ParseBool(value, o.animations)) return bad("1 | 0"); }
        else if (name == "meshlets")   { if (!ParseBool(value, o.meshlets))   return bad("1 | 0"); }
        else if (name == "lod")        { if (!ParseBool(value, o.lod))        return bad("1 | 0"); }
        else if (name == "morphNormals")
        {
            if (value == "auto")         o.morphNormals = Opt::MorphNormals::Auto;
            else if (value == "compute") o.morphNormals = Opt::MorphNormals::Compute;
            else if (value == "none")    o.morphNormals = Opt::MorphNormals::None;
            else return bad("auto | compute | none");
        }
        else if (name == "collision")
        {
            if (value == "names")        o.collision = Opt::Collision::Names;
            else if (value == "none")    o.collision = Opt::Collision::None;
            else if (value == "box")     o.collision = Opt::Collision::Box;
            else if (value == "convex")  o.collision = Opt::Collision::Convex;
            else if (value == "complex") o.collision = Opt::Collision::Complex;
            else return bad("names | none | box | convex | complex");
        }
        else if (name == "materials")
        {
            if (value == "create")     o.materials = Opt::Materials::Create;
            else if (value == "reuse") o.materials = Opt::Materials::Reuse;
            else return bad("create | reuse");
        }
        else
        {
            if (error) *error = "unknown option '" + name + "' (normals, smooth, morphs, morphNormals, collision, "
                                "materials, textures, flipGreen, animations, meshlets, lod)";
            return false;
        }
        return true;
    }

    bool ParseImportOptions(const std::string& text, ModelImportOptions& out)
    {
        out = ModelImportOptions{};
        for (size_t b = 0; b < text.size();)
        {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(b, e - b);
            b = e + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("IM ", 0) != 0) continue;
            for (size_t p = 3; p < line.size();)
            {
                size_t q = line.find(' ', p);
                if (q == std::string::npos) q = line.size();
                const std::string tok = line.substr(p, q - p);
                p = q + 1;
                const size_t eq = tok.find('=');
                if (eq == std::string::npos) continue;
                std::string why;
                if (!SetImportOption(out, tok.substr(0, eq), tok.substr(eq + 1), &why))
                    LOG_WARNING("ModelImportWrite: an IM record's %s - the default is used", why.c_str());
            }
            return true;
        }
        return false;
    }

    bool ReadImportOptions(const std::string& iscnPath, ModelImportOptions& out)
    {
        out = ModelImportOptions{};
        std::string text;
        return ReadModelText(iscnPath, text) && ParseImportOptions(text, out);
    }

    ModelImportStats WriteModelImportFiles(const std::string&                    outDirUtf8,
                                           const std::vector<ImportFileOut>&     files,
                                           const char*                           label,
                                           const std::vector<ImportedCollision>* collision,
                                           const std::vector<ImportedShapeKeys>* shapeKeys,
                                           const ModelWriteOptions*              options)
    {
        namespace fs = std::filesystem;
        ModelImportStats st;
        static const ModelWriteOptions kDefaults;
        const ModelWriteOptions& opt = options ? *options : kDefaults;
        MeshAdoptOptions adoptOpt;
        adoptOpt.geometryRenames = opt.geometryRenames;
        if (opt.renames) adoptOpt.renames = *opt.renames;

        const fs::path outDir = Sys::U8Path(outDirUtf8);

        // ---- Read the OUTGOING .iscn's bindings BEFORE anything is overwritten ----
        std::string iscnOutPath;
        for (const auto& f : files)
            if (HasExt(f.relativePath, ".iscn"))
                iscnOutPath = Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath));

        std::vector<SceneInstanceLoader::MaterialBinding> oldBindings;
        const bool hadIscn = !iscnOutPath.empty() && Sys::FileExists(iscnOutPath);
        if (hadIscn && !SceneInstanceLoader::ReadMaterialBindings(iscnOutPath, oldBindings))
            oldBindings.clear();   // unreadable - fall through to the fresh defaults
        // And its asset collision (a Mesh Editor's edits live there), shape-key records, sockets and
        // virtual bones.
        ModelCollision           oldCollision;
        ModelShapeKeys           oldShapeKeys;
        ModelSockets             oldSockets;
        std::vector<std::string> oldSocketProblems;   // a malformed SO record is not carried (said below)
        ModelVirtualBones        oldVirtualBones;
        if (hadIscn)
            if (std::string oldText; ReadModelText(iscnOutPath, oldText))
            {
                ParseModelCollision(oldText, oldCollision);
                ParseModelShapeKeys(oldText, oldShapeKeys);
                ParseModelSockets(oldText, oldSockets, &oldSocketProblems);
                ParseModelVirtualBones(oldText, oldVirtualBones, &oldSocketProblems);
            }

        // ---- Carry the OUTGOING .meshlib's keys into the new one ----
        // Before anything is overwritten: a previous library without an identity is read through the one
        // its OLD .iscn implies, and that .iscn is about to be replaced. The blobs are borrowed, so an
        // adopted library is a copy.
        std::unordered_map<const std::vector<uint8_t>*, std::vector<uint8_t>> adopted;
        for (const auto& f : files)
        {
            if (!f.blob || !HasExt(f.relativePath, ".meshlib")) continue;
            std::vector<uint8_t> out = *f.blob;
            MeshIdentityAdoption a;
            if (!AdoptExistingMeshLibKeys(Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath)), out, &a, &adoptOpt))
                continue;
            adopted.emplace(f.blob, std::move(out));
            st.identity.meshesKept   += a.meshesKept;   st.identity.meshesNew    += a.meshesNew;
            st.identity.meshesGone   += a.meshesGone;   st.identity.sectionsKept += a.sectionsKept;
            st.identity.sectionsNew  += a.sectionsNew;  st.identity.sectionsGone += a.sectionsGone;
            // The report is the MODEL library's (the one that is not a collision library; an importer
            // writes one).
            if (!HasExt(f.relativePath, "_collision.meshlib"))
            {
                const MeshIdentityAdoption counts = st.identity;
                st.identity = std::move(a);
                st.identity.meshesKept   = counts.meshesKept;   st.identity.meshesNew    = counts.meshesNew;
                st.identity.meshesGone   = counts.meshesGone;   st.identity.sectionsKept = counts.sectionsKept;
                st.identity.sectionsNew  = counts.sectionsNew;  st.identity.sectionsGone = counts.sectionsGone;
            }
        }
        if (opt.dryRun) return st;   // what a re-import would do to the keys - nothing written

        // ---- Key the .imorph's blocks ----
        // An importer writes its v2 blocks keyed 0: the keys are the LIBRARY's, final only now that the
        // previous file's are adopted. A block names its part by F-line index, and every importer writes
        // one F-line per entry of the one model library, in entry order - so F-line index == entry id.
        {
            const std::vector<uint8_t>* lib  = nullptr;
            uint32_t                    libs = 0;
            for (const auto& f : files)
                if (f.blob && HasExt(f.relativePath, ".meshlib") && !HasExt(f.relativePath, "_collision.meshlib"))
                {
                    ++libs;
                    const auto ad = adopted.find(f.blob);
                    lib = (ad != adopted.end()) ? &ad->second : f.blob;
                }
            MeshIdentity id;
            std::string  why = "not exactly one model library";
            const bool   haveId = libs == 1 &&
                ParseMeshLibIdentity(lib->data(), lib->size(), id, &why) == MeshIdentityParse::Ok;
            for (const auto& f : files)
            {
                if (!f.blob || !HasExt(f.relativePath, ".imorph")) continue;
                std::vector<uint8_t> out = *f.blob;
                uint32_t keyed = 0, unknown = 0;
                const bool v2 = MorphLibrary::RekeySections(out, [&](uint32_t entry) -> uint64_t
                {
                    if (haveId && entry < id.sections.size()) { ++keyed; return id.sections[entry].key; }
                    ++unknown;
                    return 0;
                });
                if (!v2) continue;   // not MAGIC_MORPHLIB2: no section keys to write
                if (!haveId)
                    LOG_WARNING("ModelImportWrite:   '%s': morph blocks stay unkeyed (%s) - they bind by "
                                "F-line index", f.relativePath.c_str(), why.c_str());
                else if (unknown)
                    LOG_WARNING("ModelImportWrite:   '%s': %u morph block(s) name no entry of the library - "
                                "left unkeyed", f.relativePath.c_str(), unknown);
                if (keyed) adopted.emplace(f.blob, std::move(out));
            }
        }

        // ---- The libraries' meshlets (MLET) ----
        // Built here, once the keys are adopted (WriteMeshLibIdentity keeps every other chunk, and the
        // MLET depends on the geometry alone), for the STATIC sections of every library that draws -
        // never a collision library. A library with no static section carries no MLET: skinned sections
        // never read the library's vertex buffer.
        if (!opt.importOptions || opt.importOptions->meshlets)
        {
            for (const auto& f : files)
            {
                if (!f.blob || !HasExt(f.relativePath, ".meshlib") || HasExt(f.relativePath, "_collision.meshlib"))
                    continue;
                const auto ad = adopted.find(f.blob);
                std::vector<uint8_t> out = (ad != adopted.end()) ? ad->second : *f.blob;
                MeshletBuildOptions mo;
                mo.lod = !opt.importOptions || opt.importOptions->lod;
                MeshIdentity id;
                if (ParseMeshLibIdentity(out.data(), out.size(), id, nullptr) == MeshIdentityParse::Ok)
                    mo.entryMask = StaticEntryMask(id, static_cast<uint32_t>(id.sections.size()), false);
                MeshletBuildStats ms;
                std::string       why;
                const std::string outPath = Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath));
                if (!AddMeshletsToMeshLib(out, mo, &ms, &why))
                {
                    LOG_WARNING("ModelImportWrite:   '%s': no meshlets (%s) - it draws through the vertex shader",
                                f.relativePath.c_str(), why.c_str());
                    continue;
                }
                if (ms.entriesBuilt == 0) continue;   // every section skinned: nothing written
                LogMeshletBuild("import", outPath, ms, true);
                ++st.meshletLibraries;
                st.meshletClusters += ms.clusters;
                adopted[f.blob] = std::move(out);
            }
        }

        // ---- materials = reuse: the project's .imat of the same name ----
        std::unordered_map<std::string, std::string> reuseOf;   // the importer's .imat (relative) -> the project's
        if (opt.importOptions && opt.importOptions->materials == ModelImportOptions::Materials::Reuse)
            reuseOf = FindReusableMaterials(outDir, files);

        // ---- Write, skipping .imat files that already exist ----
        // Two passes: everything else first - the textures a material names must be on disk before its
        // text is augmented with their guids - then the materials.
        bool wroteTextures = false;
        auto writeOne = [&](const ImportFileOut& f)
        {
            const std::string outPath = Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath));
            const bool imat = HasExt(f.relativePath, ".imat");
            if (imat && Sys::FileExists(outPath))
            {
                ++st.keptMaterials;
                return;
            }
            if (imat && reuseOf.count(f.relativePath)) return;   // the project's is bound instead

            const auto            ad    = adopted.find(f.blob);
            std::vector<uint8_t>  augmented;
            const std::vector<uint8_t>* bytes = (ad != adopted.end()) ? &ad->second : f.blob;
            if (imat && wroteTextures && bytes->size() > sizeof(AssetHeader) + sizeof(uint32_t))
            {
                // The payload text again through AugmentImatText (idempotent), now
                // that the textures it names exist; header and metadata re-sized.
                AssetHeader hdr{};
                std::memcpy(&hdr, bytes->data(), sizeof(hdr));
                const size_t textAt = sizeof(AssetHeader) + hdr.metadataSize;
                uint32_t textLen = 0;
                std::memcpy(&textLen, bytes->data() + sizeof(AssetHeader), sizeof(textLen));
                if (hdr.magic == MAGIC_MATERIAL && textAt + textLen <= bytes->size())
                {
                    const std::string text(reinterpret_cast<const char*>(bytes->data() + textAt), textLen);
                    const std::string aug = AssetRefs::AugmentImatText(text);
                    const uint32_t newLen = static_cast<uint32_t>(aug.size());
                    augmented.assign(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(textAt));
                    hdr.dataSize = newLen + 1;
                    std::memcpy(augmented.data(), &hdr, sizeof(hdr));
                    std::memcpy(augmented.data() + sizeof(AssetHeader), &newLen, sizeof(newLen));
                    augmented.insert(augmented.end(), aug.begin(), aug.end());
                    augmented.push_back(0);
                    bytes = &augmented;
                }
            }
            Sys::CreateParentDirs(outPath);
            if (Sys::WriteFileBytes(outPath, *bytes))
            {
                LOG_INFO("ModelImportWrite:   wrote '%s'", outPath.c_str());
                ++st.written;
                if (HasExt(f.relativePath, ".itex")) { ++st.textures; wroteTextures = true; }
            }
            else
            {
                LOG_ERROR("ModelImportWrite:   failed to write '%s'", outPath.c_str());
                ++st.failed;
            }
        };
        for (const auto& f : files)
            if (f.blob && !HasExt(f.relativePath, ".imat")) writeOne(f);
        if (wroteTextures) AssetRefs::ForgetExistenceCache();   // the guid index sees the new files
        for (const auto& f : files)
            if (f.blob && HasExt(f.relativePath, ".imat")) writeOne(f);

        // ---- The new model's keys ----
        // `sec=` / `mkey=` / ME from the libraries just written. Their keys were adopted from the previous
        // files, so a section that survived the re-import keeps its key, and every reference by key keeps
        // pointing at it whatever its position now.
        if (!iscnOutPath.empty() && Sys::FileExists(iscnOutPath))
        {
            SceneInstanceLoader::LibraryIdentityCache libs;
            SceneInstanceLoader::ModelKeyReport       kr;
            std::string                               kerr;
            if (SceneInstanceLoader::StampModelKeys(iscnOutPath, true, kr, libs, &kerr))
                LOG_INFO("ModelImportWrite:   keys: %u F-line(s) keyed (%u unresolved), %u N-line(s) keyed, "
                         "%u ME record(s)", kr.fAdded + kr.fKept + kr.fHintFixed, kr.fUnresolved,
                         kr.nAdded + kr.nKept + kr.nChanged, kr.meLines);
            else
                LOG_WARNING("ModelImportWrite:   '%s': keys not written (%s)", iscnOutPath.c_str(), kerr.c_str());
        }

        // ---- Re-apply the previous material bindings onto the new .iscn ----
        if (hadIscn && !oldBindings.empty())
        {
            std::vector<SceneInstanceLoader::MaterialBinding> newBindings;
            if (SceneInstanceLoader::ReadMaterialBindings(iscnOutPath, newBindings))
            {
                // A FIFO per name, not a single entry: the F-line `name=` is the RAW authored material name and
                // is NOT unique - the importer dedups filenames, never names, and shipped PMX files routinely
                // carry two materials called "HairA". A plain emplace would map both new rows to the first old
                // one and drop the second binding, so the k-th occurrence of a name matches the k-th in the old
                // file.
                std::unordered_map<std::string, std::vector<const SceneInstanceLoader::MaterialBinding*>> byName;
                std::unordered_map<uint32_t, const SceneInstanceLoader::MaterialBinding*> byMesh;
                // An old F-line with a section key is matched BY IT first: the adopted keys make a surviving
                // section keep its key, which is exact where a name is not (two materials called "HairA").
                std::unordered_map<uint64_t, const SceneInstanceLoader::MaterialBinding*> byKey;
                for (const auto& b : oldBindings)
                {
                    if (!b.name.empty()) byName[b.name].push_back(&b);
                    byMesh.emplace(b.meshId, &b);
                    if (b.sectionKey) byKey.emplace(b.sectionKey, &b);
                }
                std::unordered_map<std::string, size_t> nameCursor;

                std::vector<SceneInstanceLoader::MaterialBinding> want;
                for (const auto& nb : newBindings)
                {
                    const SceneInstanceLoader::MaterialBinding* ob = nullptr;
                    if (nb.sectionKey)
                        if (const auto it = byKey.find(nb.sectionKey); it != byKey.end()) ob = it->second;
                    if (!ob && !nb.name.empty())
                    {
                        const auto it = byName.find(nb.name);
                        if (it != byName.end())
                        {
                            size_t& k = nameCursor[nb.name];
                            if (k < it->second.size()) ob = it->second[k++];
                        }
                    }
                    if (!ob)
                    {
                        // Fall back to meshId ONLY when the old entry has no name (an .iscn without `name=`). When the
                        // old file carries names and none matched, this material is new and keeps its fresh default.
                        const auto it = byMesh.find(nb.meshId);
                        if (it != byMesh.end() && it->second->name.empty()) ob = it->second;
                    }
                    if (!ob || ob->matPath.empty() || ob->matPath == nb.matPath) continue;

                    // Never repoint at a material missing on disk: that trades a blank-but-loadable default for a
                    // broken reference.
                    if (!Sys::FileExists(ob->matPath)) continue;

                    SceneInstanceLoader::MaterialBinding w;
                    w.meshAbsPath = nb.meshAbsPath;   // addresses the exact F-line
                    w.meshId      = nb.meshId;
                    w.sectionKey  = nb.sectionKey;
                    w.matPath     = ob->matPath;
                    want.push_back(std::move(w));
                }

                if (!want.empty())
                    SceneInstanceLoader::WriteMaterialBindings(iscnOutPath, want,
                                                                &st.restoredBindings);
            }
        }

        // ---- materials = reuse ----
        // An F-line still on the importer's own material (not one the author bound since - those were
        // restored above) is bound to the project's.
        if (!reuseOf.empty() && !iscnOutPath.empty() && Sys::FileExists(iscnOutPath))
        {
            std::unordered_map<std::string, std::string> byOwnPath;
            for (const auto& [rel, target] : reuseOf)
                byOwnPath.emplace(PathKey(Sys::RelativizeAssetPath(Sys::PathToUtf8(outDir / Sys::U8Path(rel)))), target);
            std::vector<SceneInstanceLoader::MaterialBinding> current, want;
            std::unordered_set<std::string>                    bound;
            if (SceneInstanceLoader::ReadMaterialBindings(iscnOutPath, current))
                for (const auto& b : current)
                    if (const auto it = byOwnPath.find(PathKey(b.matPath)); it != byOwnPath.end())
                    {
                        SceneInstanceLoader::MaterialBinding w;
                        w.meshAbsPath = b.meshAbsPath;
                        w.meshId      = b.meshId;
                        w.sectionKey  = b.sectionKey;
                        w.matPath     = it->second;
                        want.push_back(std::move(w));
                        bound.insert(it->second);
                    }
            int rebound = 0;
            if (!want.empty()) SceneInstanceLoader::WriteMaterialBindings(iscnOutPath, want, &rebound);
            st.reusedMaterials = static_cast<int>(bound.size());
            LOG_INFO("ModelImportWrite:   materials = reuse: %zu project material(s) bound to %d F-line(s)",
                     bound.size(), rebound);
        }

        // ---- The model's asset collision ----
        // Three writers (MeshBodySetup::source): the SOURCE's collision nodes (src=names, named by Mesh
        // SOURCE key: the adopted library says which MeshKey that is now), the AUTHOR's (no src=: the Mesh
        // Editor, the bake) and the import's collision OPTION (src=box|convex|complex). Per Mesh: the
        // source's replaces the rest; else the author's stays (a gone Mesh's as orphan=1); else the option
        // makes one. The previous file's src= records are remade from what is here now, never carried
        // over.
        using CollMode = ModelImportOptions::Collision;
        const CollMode collMode = opt.importOptions ? opt.importOptions->collision : CollMode::Names;
        const std::vector<ImportedCollision>* fromSource = collMode == CollMode::None ? nullptr : collision;
        if (collMode == CollMode::None && collision && !collision->empty())
            LOG_INFO("ModelImportWrite:   collision = none: the source's shapes for %zu Mesh(es) dropped",
                     collision->size());
        const bool makeCollision = collMode == CollMode::Box || collMode == CollMode::Convex ||
                                   collMode == CollMode::Complex;
        if (!iscnOutPath.empty() && Sys::FileExists(iscnOutPath) &&
            ((fromSource && !fromSource->empty()) || !oldCollision.meshes.empty() || makeCollision))
        {
            namespace fs = std::filesystem;
            // The model's own library: the .meshlib written beside the .iscn that is
            // not a collision library.
            const fs::path iscnDir = Sys::U8Path(iscnOutPath).parent_path();
            MeshIdentity   id;
            bool           haveId = false;
            std::string    modelLib;   // its path: collision = convex reads its points
            for (const auto& f : files)
            {
                if (!HasExt(f.relativePath, ".meshlib") || HasExt(f.relativePath, "_collision.meshlib")) continue;
                const std::string path = Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath));
                std::vector<uint8_t> bytes;
                std::string why;
                if (Sys::ReadFileBytes(path, bytes) && ValidateHeader(bytes.data(), bytes.size(), MAGIC_MESHLIB) &&
                    ParseMeshLibIdentity(bytes.data(), bytes.size(), id, &why) == MeshIdentityParse::Ok)
                {
                    haveId   = true;
                    modelLib = path;
                    break;
                }
            }
            std::unordered_map<std::string, uint64_t> keyOfSource;
            std::unordered_set<uint64_t>               meshKeys;
            for (const MeshIdentity::Mesh& m : id.meshes)
            {
                keyOfSource.emplace(m.sourceKey, m.key);
                meshKeys.insert(m.key);
            }
            const std::string stem    = Sys::PathToUtf8(Sys::U8Path(iscnOutPath).stem());
            const std::string libName = stem + "_collision.meshlib";
            const std::string libPath = Sys::PathToUtf8(iscnDir / Sys::U8Path(libName));

            std::vector<MeshBodySetup>   setups;
            std::unordered_set<uint64_t> covered;
            // Records an importer wrote into the new file itself (the UE importer's, src=names) are the
            // source's too - an author's for the same Mesh does not replace them.
            {
                std::string    freshText;
                ModelCollision fresh;
                if (ReadModelText(iscnOutPath, freshText)) ParseModelCollision(freshText, fresh);
                for (const auto& [key, setup] : fresh.meshes) covered.insert(key);
            }
            static const std::vector<ImportedCollision> kNone;
            for (const ImportedCollision& ic : (fromSource ? *fromSource : kNone))
            {
                const auto k = keyOfSource.find(ic.meshSourceKey);
                if (!haveId || k == keyOfSource.end())
                {
                    LOG_WARNING("ModelImportWrite:   collision for '%s' dropped: its Mesh ('%s') is not in the "
                                "library's identity", ic.meshName.c_str(), ic.meshSourceKey.c_str());
                    continue;
                }
                MeshBodySetup setup = ic.setup;
                setup.meshKey = k->second;
                setup.source  = "names";
                std::vector<CollisionLibrary::PointSet> hulls;
                std::vector<size_t>                     hullPrims;
                for (size_t i = 0; i < setup.prims.size() && i < ic.hullPoints.size(); ++i)
                    if (!ic.hullPoints[i].empty())
                    {
                        hulls.push_back(ic.hullPoints[i]);
                        hullPrims.push_back(i);
                    }
                if (!hulls.empty())
                {
                    std::vector<uint64_t> keys;
                    std::string           err;
                    uint32_t              appended = 0;
                    if (CollisionLibrary::AppendHulls(libPath, hulls, setup.meshKey, ic.meshName, keys, &err, &appended))
                    {
                        for (size_t h = 0; h < hullPrims.size(); ++h)
                        {
                            setup.prims[hullPrims[h]].lib        = libName;
                            setup.prims[hullPrims[h]].sectionKey = keys[h];
                        }
                        st.collisionHulls += static_cast<int>(appended);
                    }
                    else
                    {
                        LOG_WARNING("ModelImportWrite:   '%s': its hulls were not written (%s) - dropped",
                                    ic.meshName.c_str(), err.c_str());
                        for (size_t h = hullPrims.size(); h-- > 0;)
                            setup.prims.erase(setup.prims.begin() + static_cast<std::ptrdiff_t>(hullPrims[h]));
                    }
                }
                // Two sources for one Mesh (it is placed by two nodes): one setup.
                auto same = std::find_if(setups.begin(), setups.end(),
                                         [&](const MeshBodySetup& s) { return s.meshKey == setup.meshKey; });
                if (same == setups.end())
                {
                    setups.push_back(std::move(setup));
                    covered.insert(setups.back().meshKey);
                    ++st.collisionMeshes;
                }
                else
                {
                    same->prims.insert(same->prims.end(), setup.prims.begin(), setup.prims.end());
                    if (setup.complexity != MeshBodySetup::Complexity::Simple) same->complexity = setup.complexity;
                }
            }
            // The previous file's AUTHORED records, in key order (the map's order is not the file's): a Mesh
            // gone from the new library keeps its records as orphan=1 (nothing authored is dropped silently),
            // and an orphan whose Mesh came back is live again. Without the new identity nothing can be told
            // gone, and every one is kept as it was.
            std::vector<uint64_t> oldKeys;
            for (const auto& [key, setup] : oldCollision.meshes) oldKeys.push_back(key);
            std::sort(oldKeys.begin(), oldKeys.end());
            for (const uint64_t key : oldKeys)
            {
                if (covered.count(key)) continue;                      // the source's replaces it
                MeshBodySetup kept = oldCollision.meshes.at(key);
                if (!kept.source.empty()) continue;                    // the source's / made: remade below
                if (haveId) kept.orphan = !meshKeys.count(key);
                ++(kept.orphan ? st.orphanCollision : st.keptCollision);
                setups.push_back(std::move(kept));
            }
            // The collision option's, for every static Mesh nothing above gave collision - a made shape never
            // replaces the source's or the author's.
            if (makeCollision && haveId)
            {
                std::unordered_set<uint64_t> has(covered.begin(), covered.end());
                for (const MeshBodySetup& s : setups) has.insert(s.meshKey);
                std::vector<CollisionLibrary::PointSet> entryPoints;
                bool pointsRead = false;
                for (const MeshIdentity::Mesh& m : id.meshes)
                {
                    if (has.count(m.key) || (m.flags & (MESH_IDENT_SKINNED | MESH_IDENT_HIDDEN))) continue;
                    MeshBodySetup s;
                    s.meshKey = m.key;
                    // A flat Mesh (a plane, a decal quad) has no volume to hull: it
                    // gets its bounds' slab, as the box option would give it.
                    const bool flat = m.aabbMax[0] - m.aabbMin[0] < 1e-4f || m.aabbMax[1] - m.aabbMin[1] < 1e-4f ||
                                      m.aabbMax[2] - m.aabbMin[2] < 1e-4f;
                    if (collMode == CollMode::Complex)
                    {
                        s.complexity = MeshBodySetup::Complexity::Complex;   // its own triangles
                        s.source     = "complex";
                    }
                    else if (collMode == CollMode::Box || flat)
                    {
                        CollisionPrim p;
                        p.type = CollisionPrim::Type::Box;
                        p.name = "box";
                        constexpr float kMinHalf = 0.005f;   // a flat Mesh still gets a slab
                        p.center      = { (m.aabbMin[0] + m.aabbMax[0]) * 0.5f, (m.aabbMin[1] + m.aabbMax[1]) * 0.5f,
                                          (m.aabbMin[2] + m.aabbMax[2]) * 0.5f };
                        p.halfExtents = { (std::max)((m.aabbMax[0] - m.aabbMin[0]) * 0.5f, kMinHalf),
                                          (std::max)((m.aabbMax[1] - m.aabbMin[1]) * 0.5f, kMinHalf),
                                          (std::max)((m.aabbMax[2] - m.aabbMin[2]) * 0.5f, kMinHalf) };
                        s.prims.push_back(std::move(p));
                        s.source = NameOf(collMode);   // "box", or "convex" for a flat Mesh
                    }
                    else
                    {
                        // The hull of the Mesh's vertices: every section's points, the
                        // duplicates (a seam's split vertices) dropped.
                        std::string err;
                        if (!pointsRead)
                        {
                            pointsRead = true;
                            if (!CollisionLibrary::ReadPoints(modelLib, entryPoints, &err))
                                LOG_WARNING("ModelImportWrite:   collision = convex: '%s' does not read (%s)",
                                            modelLib.c_str(), err.c_str());
                        }
                        CollisionLibrary::PointSet pts;
                        for (uint32_t e : m.sections)
                            if (e < entryPoints.size())
                                pts.insert(pts.end(), entryPoints[e].begin(), entryPoints[e].end());
                        std::sort(pts.begin(), pts.end(), [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
                                  { return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z); });
                        pts.erase(std::unique(pts.begin(), pts.end(), [](const DirectX::XMFLOAT3& a,
                                                                         const DirectX::XMFLOAT3& b)
                                              { return a.x == b.x && a.y == b.y && a.z == b.z; }), pts.end());
                        if (pts.size() < 4) continue;   // no volume to hull
                        std::vector<uint64_t> keys;
                        uint32_t              appended = 0;
                        if (!CollisionLibrary::AppendHulls(libPath, { pts }, m.key, m.name, keys, &err, &appended) ||
                            keys.empty())
                        {
                            LOG_WARNING("ModelImportWrite:   '%s': its hull was not written (%s)", m.name.c_str(),
                                        err.c_str());
                            continue;
                        }
                        CollisionPrim p;
                        p.type       = CollisionPrim::Type::Hull;
                        p.name       = "hull";
                        p.lib        = libName;
                        p.sectionKey = keys[0];
                        s.prims.push_back(std::move(p));
                        s.source = "convex";
                        st.collisionHulls += static_cast<int>(appended);
                    }
                    setups.push_back(std::move(s));
                    ++st.madeCollision;
                }
            }
            if (!setups.empty())
            {
                std::string before, after, err;
                if (!SceneInstanceLoader::ReplaceModelCollision(iscnOutPath, setups, before, after, &err))
                    LOG_WARNING("ModelImportWrite:   '%s': the collision records were not written (%s)",
                                iscnOutPath.c_str(), err.c_str());
                else
                    LOG_INFO("ModelImportWrite:   collision: %d Mesh(es) from the source, %d made (%s), %d hull(s) "
                             "appended to '%s', %d Mesh(es) kept from the previous file, %d kept as orphan",
                             st.collisionMeshes, st.madeCollision, NameOf(collMode), st.collisionHulls,
                             libName.c_str(), st.keptCollision, st.orphanCollision);
            }
        }

        // ---- The model's shape-key records ----
        // Two kinds (ModelShapeKeys.h). The AUTHOR's (no src=: the Mesh Editor's) come back as they were -
        // a Mesh gone from the new library keeps its own marked orphan=1; nothing written by hand is
        // dropped silently. The previous file's src= records were the SOURCE's: @p shapeKeys replaces
        // them, filling in every name the author has no record of - the author's wins by name.
        if (!iscnOutPath.empty() && Sys::FileExists(iscnOutPath) &&
            (!oldShapeKeys.meshes.empty() || (shapeKeys && !shapeKeys->empty())))
        {
            MeshIdentity id;
            bool         haveId = false;
            for (const auto& f : files)
            {
                if (!HasExt(f.relativePath, ".meshlib") || HasExt(f.relativePath, "_collision.meshlib")) continue;
                std::vector<uint8_t> bytes;
                std::string why;
                if (Sys::ReadFileBytes(Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath)), bytes) &&
                    ValidateHeader(bytes.data(), bytes.size(), MAGIC_MESHLIB) &&
                    ParseMeshLibIdentity(bytes.data(), bytes.size(), id, &why) == MeshIdentityParse::Ok)
                {
                    haveId = true;
                    break;
                }
            }
            std::unordered_map<std::string, uint64_t> keyOfSource;
            std::unordered_set<uint64_t>               meshKeys;
            for (const MeshIdentity::Mesh& m : id.meshes)
            {
                keyOfSource.emplace(m.sourceKey, m.key);
                meshKeys.insert(m.key);
            }

            // The source's, by the MeshKey its Mesh has now (two sources for one
            // Mesh - it is placed by two nodes - are one setup, first name wins).
            std::unordered_map<uint64_t, MeshShapeKeySetup> fromSource;
            static const std::vector<ImportedShapeKeys> kNoKeys;
            for (const ImportedShapeKeys& is : (shapeKeys ? *shapeKeys : kNoKeys))
            {
                const auto k = keyOfSource.find(is.meshSourceKey);
                if (!haveId || k == keyOfSource.end())
                {
                    LOG_WARNING("ModelImportWrite:   shape-key records for '%s' dropped: its Mesh ('%s') is not in "
                                "the library's identity", is.meshName.c_str(), is.meshSourceKey.c_str());
                    continue;
                }
                MeshShapeKeySetup& s = fromSource[k->second];
                s.meshKey = k->second;
                for (const ShapeKeyMeta& m : is.setup.meta)
                    if (!s.FindMeta(m.name)) s.meta.push_back(m);
                for (const ShapeKeyPreset& p : is.setup.presets)
                    if (!s.FindPreset(p.name)) s.presets.push_back(p);
            }

            // One setup per Mesh either side names: the author's records (their
            // order), then the source's under names the author has not used. In
            // the library's Mesh order, orphans after by key - a re-import writes
            // the same text for the same inputs.
            auto merged = [&](uint64_t key, bool orphan)
            {
                MeshShapeKeySetup out;
                out.meshKey = key;
                out.orphan  = orphan;
                if (const auto o = oldShapeKeys.meshes.find(key); o != oldShapeKeys.meshes.end())
                {
                    for (const ShapeKeyMeta& m : o->second.meta)
                        if (m.source.empty()) out.meta.push_back(m);
                    for (const ShapeKeyPreset& p : o->second.presets)
                        if (p.source.empty()) out.presets.push_back(p);
                }
                if (orphan) return out;   // a gone Mesh: the source has nothing for it
                if (const auto n = fromSource.find(key); n != fromSource.end())
                {
                    for (const ShapeKeyMeta& m : n->second.meta)
                        if (!out.FindMeta(m.name)) { out.meta.push_back(m); ++st.sourceDefaults; }
                    for (const ShapeKeyPreset& p : n->second.presets)
                        if (!out.FindPreset(p.name)) { out.presets.push_back(p); ++st.sourcePresets; }
                    ++st.sourceShapeKeyMeshes;
                }
                return out;
            };
            auto authored = [](const MeshShapeKeySetup& s)
            {
                return std::any_of(s.meta.begin(), s.meta.end(), [](const ShapeKeyMeta& m) { return m.source.empty(); }) ||
                       std::any_of(s.presets.begin(), s.presets.end(), [](const ShapeKeyPreset& p) { return p.source.empty(); });
            };
            std::vector<MeshShapeKeySetup> setups;   // an Empty() one only removes its Mesh's old records
            std::unordered_set<uint64_t>   placed;
            for (const MeshIdentity::Mesh& m : id.meshes)
            {
                if (!oldShapeKeys.meshes.count(m.key) && !fromSource.count(m.key)) continue;
                if (!placed.insert(m.key).second) continue;
                setups.push_back(merged(m.key, false));
                if (authored(setups.back())) ++st.keptShapeKeys;
            }
            // The old file's Meshes the new library does not have: orphans. Without the new identity nothing
            // can be told gone, and every one is kept as it was.
            std::vector<uint64_t> rest;
            for (const auto& [key, setup] : oldShapeKeys.meshes)
                if (!placed.count(key)) rest.push_back(key);
            std::sort(rest.begin(), rest.end());
            for (const uint64_t key : rest)
            {
                setups.push_back(merged(key, haveId));
                if (authored(setups.back())) (haveId ? st.orphanShapeKeys : st.keptShapeKeys) += 1;
            }
            std::string before, after, err;
            if (!SceneInstanceLoader::ReplaceModelShapeKeys(iscnOutPath, setups, before, after, &err))
                LOG_WARNING("ModelImportWrite:   '%s': the shape-key records were not written (%s)",
                            iscnOutPath.c_str(), err.c_str());
            else
                LOG_INFO("ModelImportWrite:   shape keys: %d preset(s) and %d default(s) from the source over %d "
                         "Mesh(es); %d Mesh(es) kept their authored MK / MP records, %d kept as orphan",
                         st.sourcePresets, st.sourceDefaults, st.sourceShapeKeyMeshes, st.keptShapeKeys,
                         st.orphanShapeKeys);
        }

        // ---- The model's socket records ----
        // The AUTHOR's (no src=: the Skeleton Editor's) come back as they were, by name; one whose bone
        // the NEW skeleton lacks is marked orphan=1 (an orphan whose bone came back is live again) -
        // nothing written by hand is dropped silently. Without the new skeleton nothing can be told gone:
        // kept as they were. A src= record was the SOURCE's; no importer reports sockets, so it is
        // dropped (counted). A mesh socket is kept too, marked orphan=1 when the new library has no Mesh
        // of its key.
        for (const std::string& why : oldSocketProblems)
            LOG_WARNING("ModelImportWrite:   '%s': the previous file's socket records: %s", iscnOutPath.c_str(),
                        why.c_str());
        // The virtual bones likewise - both of a VB's bones must be in the new skeleton; a socket may
        // sit on a VB that stays, so the VBs go first.
        if (!iscnOutPath.empty() && Sys::FileExists(iscnOutPath) &&
            (!oldSockets.sockets.empty() || !oldVirtualBones.bones.empty()))
        {
            std::unordered_set<std::string> bones;
            bool haveSkeleton = false;
            if (const std::string iskel = SceneInstanceLoader::ModelSkeletonPath(iscnOutPath); !iskel.empty())
            {
                auto skel = std::make_unique<SkeletonAsset>();   // ~656 KB: never on the stack
                std::vector<ClipAsset> clips;
                if (SceneInstanceLoader::LoadSkeletonFile(iskel, *skel, clips))
                {
                    haveSkeleton = true;
                    for (uint32_t b = 0; b < skel->boneCount; ++b)
                        bones.emplace(skel->boneNames[b], strnlen(skel->boneNames[b], sizeof(skel->boneNames[b])));
                }
            }
            if (!oldVirtualBones.bones.empty())
            {
                std::vector<ModelVirtualBone> keptVb;
                int live = 0, orphan = 0;
                for (const ModelVirtualBone& v : oldVirtualBones.bones)
                {
                    ModelVirtualBone k = v;
                    if (haveSkeleton) k.orphan = !bones.count(k.from) || !bones.count(k.to);
                    ++(k.orphan ? orphan : live);
                    keptVb.push_back(std::move(k));
                }
                std::string before, after, err;
                if (!SceneInstanceLoader::ReplaceModelVirtualBones(iscnOutPath, keptVb, before, after, &err))
                    LOG_WARNING("ModelImportWrite:   '%s': the virtual bone records were not written (%s)",
                                iscnOutPath.c_str(), err.c_str());
                else
                    LOG_INFO("ModelImportWrite:   virtual bones: %d kept, %d kept as orphan (a bone of theirs is gone)",
                             live, orphan);
                for (const ModelVirtualBone& k : keptVb)
                    if (!k.orphan) bones.insert(k.name);   // a socket on it stays live
            }
            // A mesh socket's Mesh by the NEW library's identity: one whose Mesh is gone is an orphan (as its
            // shape keys and collision are), one whose Mesh came back is live. Without the identity: kept as
            // they were.
            std::unordered_set<uint64_t> meshKeys;
            bool                         haveId = false;
            const bool anyMeshSocket = std::any_of(oldSockets.sockets.begin(), oldSockets.sockets.end(),
                                                   [](const ModelSocket& s) { return !s.IsSkeletonSocket(); });
            for (const auto& f : files)
            {
                if (!anyMeshSocket) break;
                if (!HasExt(f.relativePath, ".meshlib") || HasExt(f.relativePath, "_collision.meshlib")) continue;
                std::vector<uint8_t> bytes;
                std::string why;
                MeshIdentity id;
                if (Sys::ReadFileBytes(Sys::PathToUtf8(outDir / Sys::U8Path(f.relativePath)), bytes) &&
                    ValidateHeader(bytes.data(), bytes.size(), MAGIC_MESHLIB) &&
                    ParseMeshLibIdentity(bytes.data(), bytes.size(), id, &why) == MeshIdentityParse::Ok)
                {
                    haveId = true;
                    for (const MeshIdentity::Mesh& m : id.meshes) meshKeys.insert(m.key);
                    break;
                }
            }
            std::vector<ModelSocket> kept;
            for (const ModelSocket& s : oldSockets.sockets)
            {
                if (!s.source.empty()) { ++st.droppedSourceSockets; continue; }
                ModelSocket k = s;
                if (k.IsSkeletonSocket() && haveSkeleton) k.orphan = !bones.count(k.bone);
                if (!k.IsSkeletonSocket() && haveId) k.orphan = !meshKeys.count(k.meshKey);
                ++(k.orphan ? st.orphanSockets : st.keptSockets);
                kept.push_back(std::move(k));
            }
            std::string before, after, err;
            if (oldSockets.sockets.empty()) {}
            else if (!SceneInstanceLoader::ReplaceModelSockets(iscnOutPath, kept, before, after, &err))
                LOG_WARNING("ModelImportWrite:   '%s': the socket records were not written (%s)",
                            iscnOutPath.c_str(), err.c_str());
            else
                LOG_INFO("ModelImportWrite:   sockets: %d authored socket(s) kept, %d kept as orphan (their bone or "
                         "Mesh is gone%s), %d source socket(s) dropped", st.keptSockets, st.orphanSockets,
                         haveSkeleton ? "" : " - no skeleton to tell by: none marked", st.droppedSourceSockets);
        }

        // ---- The model's source (SR) and options (IM) ----
        // The new values win: one line each before the ME block (StampModelKeys keeps ME last), every
        // previous SR / IM line gone.
        const bool writeSource  = opt.source && opt.source->Valid();
        const bool writeOptions = opt.importOptions != nullptr;
        if ((writeSource || writeOptions) && !iscnOutPath.empty() && Sys::FileExists(iscnOutPath))
        {
            std::string text;
            if (ReadModelText(iscnOutPath, text))
            {
                const std::string eol = (text.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
                std::string records;
                for (std::string r : { writeSource ? FormatModelSource(*opt.source) : std::string(),
                                       writeOptions ? FormatImportOptions(*opt.importOptions) : std::string() })
                    if (!r.empty())
                    {
                        r.pop_back();   // its '\n'
                        records += r + eol;
                    }
                std::string rebuilt;
                rebuilt.reserve(text.size() + records.size());
                bool placed = false;
                for (size_t b = 0; b < text.size();)
                {
                    size_t e = text.find('\n', b);
                    e = (e == std::string::npos) ? text.size() : e + 1;
                    const std::string_view line(text.data() + b, e - b);
                    b = e;
                    if ((writeSource && line.rfind("SR ", 0) == 0) || (writeOptions && line.rfind("IM ", 0) == 0))
                        continue;
                    if (!placed && line.rfind("ME ", 0) == 0)
                    {
                        if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
                        rebuilt += records;
                        placed = true;
                    }
                    rebuilt += line;
                }
                if (!placed)
                {
                    if (!rebuilt.empty() && rebuilt.back() != '\n') rebuilt += eol;
                    rebuilt += records;
                }
                std::string err;
                if (!SceneInstanceLoader::WriteModelText(iscnOutPath, rebuilt, &err))
                    LOG_WARNING("ModelImportWrite:   '%s': the source / options records were not written (%s)",
                                iscnOutPath.c_str(), err.c_str());
            }
        }

        if (st.keptMaterials || st.restoredBindings)
            LOG_SUCCESS("ModelImportWrite: %s re-import kept %d existing .imat file(s) and "
                        "restored %d material binding(s). Delete the model folder first if "
                        "you meant to reset them.",
                        label, st.keptMaterials, st.restoredBindings);

        return st;
    }
}
