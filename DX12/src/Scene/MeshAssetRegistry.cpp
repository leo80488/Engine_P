#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Scene/MeshAssetRegistry.h"

#include "Graphics/IGraphicsDevice.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetFS.h"
#include "Resource/MeshLibIdentity.h"
#include "Resource/MeshLibrary.h"
#include "Scene/SceneInstanceLoader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <filesystem>
#include <unordered_set>

namespace
{
    bool EndsWithMeshlib(const std::string& p)
    {
        static const char kExt[] = ".meshlib";
        const size_t n = sizeof(kExt) - 1;
        if (p.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
        {
            char c = p[p.size() - n + i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != kExt[i]) return false;
        }
        return true;
    }
}

void MeshAssetRegistry::Clear()
{
    m_models.clear();
    m_libs.clear();
    // m_revisions stays: it records "this file changed under us this session", not a view.
}

void MeshAssetRegistry::InvalidateModel(const std::string& modelPath)
{
    const std::string key = Resource::MeshLibCanonicalPath(modelPath);
    m_models.erase(key);
    ++m_revisions[key];
}

uint32_t MeshAssetRegistry::ModelRevision(const std::string& modelPath) const
{
    if (m_revisions.empty()) return 0;
    auto it = m_revisions.find(Resource::MeshLibCanonicalPath(modelPath));
    return it != m_revisions.end() ? it->second : 0u;
}

Resource::Handle MeshAssetRegistry::LoadLibrary(const std::string& path)
{
    const std::string key = Resource::MeshLibCanonicalPath(path);
    if (auto it = m_libs.find(key); it != m_libs.end()) return it->second;

    // Same sequence as SceneInstanceLoader::Load: one upload batch, then the handle becomes a
    // world library, released at the world clear and never by this registry.
    m_gfx.BeginBufferUploadBatch();
    const Resource::Handle h = m_library.Load(path, m_gfx);
    m_gfx.EndBufferUploadBatch();
    if (h.IsValid() && m_renderer) m_renderer->TrackWorldMeshLibrary(h);
    m_libs.emplace(key, h);   // a failure is remembered too: not retried until the epoch moves
    return h;
}

const MeshAssetRegistry::Model& MeshAssetRegistry::GetModel(const std::string& modelPath, uint32_t worldEpoch)
{
    if (worldEpoch != m_epoch)
    {
        Clear();
        m_epoch = worldEpoch;
    }
    const std::string key = Resource::MeshLibCanonicalPath(modelPath);
    if (auto it = m_models.find(key); it != m_models.end()) return *it->second;

    auto owned = std::make_unique<Model>();
    BuildModel(modelPath, m_library, [this](const std::string& lib) { return LoadLibrary(lib); }, *owned);
    auto [it, added] = m_models.emplace(key, std::move(owned));
    return *it->second;
}

void MeshAssetRegistry::BuildModel(const std::string& modelPath, Resource::MeshLibrary& library,
                                   const LibraryLoader& loadLibrary, Model& model)
{
    const std::string key = Resource::MeshLibCanonicalPath(modelPath);
    model = Model{};
    model.path = key;
    auto addError = [&model](const std::string& e) { model.error += (model.error.empty() ? "" : "; ") + e; };

    std::vector<SceneInstanceLoader::MaterialBinding> binds;
    if (modelPath.empty())
        addError("no model");
    else if (!::Resource::AssetFS::Get().Exists(modelPath))
        addError("the model '" + modelPath + "' does not exist");
    else if (!SceneInstanceLoader::ReadMaterialBindings(modelPath, binds))
        addError("the model '" + modelPath + "' cannot be read");
    else
        model.namesSkeleton = SceneInstanceLoader::NamesSkeleton(modelPath);

    // Libraries in first-seen order, each with the material the model gives each of its entries
    // (the first F-line naming the entry wins; a UE level may name one entry several times with
    // different materials - those are placement overrides, the first is the asset's default).
    // An F-line carrying its SectionKey (`sec=`) names its entry by the key, resolved once the
    // library's identity is known, so a re-imported library that reordered its entries cannot
    // hand one slot's default to another; `mesh=` is then only the hint.
    std::vector<std::string> libOrder;
    std::unordered_map<std::string, std::vector<const SceneInstanceLoader::MaterialBinding*>> bindsOf;
    for (const SceneInstanceLoader::MaterialBinding& b : binds)
    {
        auto [it, added] = bindsOf.try_emplace(b.meshAbsPath);
        if (added) libOrder.push_back(b.meshAbsPath);
        it->second.push_back(&b);
    }
    // entry -> material, first F-line wins; a key the identity has overrides the hint.
    auto entryMaterials = [](const std::vector<const SceneInstanceLoader::MaterialBinding*>& bs,
                             const Resource::MeshIdentity& id)
    {
        std::unordered_map<uint32_t, std::string> out;
        for (const SceneInstanceLoader::MaterialBinding* b : bs)
        {
            uint32_t e = b->meshId;
            if (b->sectionKey != 0)
                if (const int k = id.FindSection(b->sectionKey); k >= 0) e = static_cast<uint32_t>(k);
            out.emplace(e, b->matPath);
        }
        return out;
    };

    std::unordered_set<uint64_t> seenKeys;
    for (const std::string& libPath : libOrder)
    {
        if (!EndsWithMeshlib(libPath)) continue;   // .imsh geometry: no identity to address
        const Resource::Handle h = loadLibrary ? loadLibrary(libPath) : Resource::Handle{};
        if (!h.IsValid())
        {
            addError("cannot load '" + libPath + "'");
            continue;
        }
        const std::shared_ptr<const Resource::MeshIdentity> id = library.GetIdentity(h);
        if (!id)
        {
            addError("'" + libPath + "' has no identity (no provider installed)");
            continue;
        }

        // An entry this model does not place (a placement .iscn over a shared
        // library) takes its default from the library's own model file - the
        // owner rule: the same-stem .iscn defines the library's meshes.
        const std::unordered_map<uint32_t, std::string> mats = entryMaterials(bindsOf[libPath], *id);
        std::unordered_map<uint32_t, std::string> ownerMats;
        bool ownerRead = false;
        auto defaultMaterial = [&](uint32_t entry) -> std::string
        {
            if (auto it = mats.find(entry); it != mats.end()) return it->second;
            if (!ownerRead)
            {
                ownerRead = true;
                std::filesystem::path owner = Sys::U8Path(libPath);
                owner.replace_extension(".iscn");
                const std::string ownerPath = Sys::PathToUtf8(owner);
                std::vector<SceneInstanceLoader::MaterialBinding> ob;
                if (Resource::MeshLibCanonicalPath(ownerPath) != key &&
                    ::Resource::AssetFS::Get().Exists(ownerPath) &&
                    SceneInstanceLoader::ReadMaterialBindings(ownerPath, ob))
                {
                    const std::string libKey = Resource::MeshLibCanonicalPath(libPath);
                    std::vector<const SceneInstanceLoader::MaterialBinding*> mine;
                    for (const auto& b : ob)
                        if (Resource::MeshLibCanonicalPath(b.meshAbsPath) == libKey) mine.push_back(&b);
                    ownerMats = entryMaterials(mine, *id);
                }
            }
            auto it = ownerMats.find(entry);
            return it != ownerMats.end() ? it->second : std::string();
        };

        const bool synthesized = (id->flags & Resource::IDNT_FLAG_SYNTHESIZED) != 0;
        for (const Resource::MeshIdentity::Mesh& m : id->meshes)
        {
            if (!seenKeys.insert(m.key).second)
                LOG_WARNING("MeshAssetRegistry: model '%s' names the mesh key %s in more than one "
                            "library - a reference to it resolves to the first", key.c_str(),
                            Resource::MeshKeyToHex(m.key).c_str());
            Mesh v;
            v.key         = m.key;
            v.name        = m.name;
            v.flags       = m.flags;
            v.libraryPath = libPath;
            v.library     = h;
            v.synthesized = synthesized;
            v.skinned     = Resource::MeshCountsAsSkinned(m.flags, id->flags, model.namesSkeleton);
            v.aabbMin     = { m.aabbMin[0], m.aabbMin[1], m.aabbMin[2] };
            v.aabbMax     = { m.aabbMax[0], m.aabbMax[1], m.aabbMax[2] };
            v.sections.reserve(m.sections.size());
            for (uint32_t e : m.sections)
            {
                if (e >= id->sections.size()) continue;
                Section s;
                s.entry           = e;
                s.key             = id->sections[e].key;
                s.slot            = id->sections[e].slotName;
                s.defaultMaterial = defaultMaterial(e);
                if (const Resource::MeshLibrary::Entry* en = library.GetEntry(h, e))
                {
                    s.aabbMin = en->aabbMin;
                    s.aabbMax = en->aabbMax;
                }
                v.sections.push_back(std::move(s));
            }
            if (!v.sections.empty()) model.meshes.push_back(std::move(v));
        }
    }
    if (model.error.empty() && model.meshes.empty() && !binds.empty())
        addError("the model names no .meshlib geometry");
}

const MeshAssetRegistry::Mesh* MeshAssetRegistry::Find(const Model& model, uint64_t key,
                                                        const std::string& name, bool* outByName)
{
    if (outByName) *outByName = false;
    if (key != 0)
        for (const Mesh& m : model.meshes)
            if (m.key == key) return &m;
    if (!name.empty())
        for (const Mesh& m : model.meshes)
            if (m.name == name)
            {
                if (outByName) *outByName = true;
                return &m;
            }
    return nullptr;
}
