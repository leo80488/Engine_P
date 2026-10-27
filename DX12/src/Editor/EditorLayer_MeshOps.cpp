// The Mesh pipeline's editor operations:
//
//   * Extract Mesh...        writes one Mesh of a model out as a model of its own
//                            (MeshExtract) and optionally re-points the scene's
//                            Static Meshes of it. The re-point is one undo entry;
//                            the files are not undone (like SavePrefab's).
//   * Unpack Model Instance  turns a spawned .iscn instance (a SceneRef root) into
//                            authored nodes whose meshes are Static Meshes, so the
//                            parts can be moved, hidden or deleted and the edit
//                            survives a save (a SceneRef instance respawns its
//                            parts from the file). One undo entry.

#include "Editor/EditorLayer.h"

#include "ECS/AnimationComponents.h"     // MeshSkinnedComponent: a skinned part refuses
#include "ECS/Components.h"
#include "ECS/FaceShadingComponent.h"    // an FS override has nowhere to go
#include "ECS/HierarchyComponents.h"
#include "ECS/PhysicsComponents.h"       // part colliders move to their own entity
#include "ECS/StaticMeshComponents.h"
#include "Editor/Undo/ComponentBlob.h"
#include "Editor/Undo/ComponentEditAction.h"   // AddComponentAction
#include "Editor/Undo/EditHistory.h"
#include "Editor/Undo/StructuralAction.h"
#include "Editor/Undo/ValueEditAction.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetGuid.h"
#include "Resource/AssetRefs.h"          // NormalizeRefPath
#include "Resource/MeshLibIdentity.h"
#include "Resource/SceneSerializer.h"    // ColliderIsSpawnDefault
#include "Scene/MeshAssetRegistry.h"
#include "Scene/MeshExtract.h"
#include "Scene/PrimitiveAssets.h"       // Create > 3D Object as Static Meshes
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    void LinkUnder(World& w, Entity child, Entity parent)
    {
        if (Parent* p = w.GetComponent<Parent>(child)) p->entity = parent;
        else w.AddComponent<Parent>(child, Parent{ parent });
        if (Children* ch = w.GetComponent<Children>(parent)) ch->entities.push_back(child);
        else
        {
            Children nc;
            nc.entities.push_back(child);
            w.AddComponent<Children>(parent, std::move(nc));
        }
    }

    Editor::Undo::RestoreFn StaticMeshValue(StaticMeshComponent v)
    {
        return [v](World& w, Entity e) { if (auto* c = w.GetComponent<StaticMeshComponent>(e)) *c = v; };
    }
}

// ==== Create > 3D Object: a Static Mesh of the engine's Primitives model ====
Entity EditorLayer::CreatePrimitiveStaticMesh(int primitiveType, const DirectX::XMFLOAT3& at)
{
    const char*        name = PrimitiveAssets::MeshName(primitiveType);
    MeshAssetRegistry* reg  = (m_renderer && m_world) ? m_renderer->GetMeshAssets() : nullptr;
    if (!name || !reg) return NullEntity;
    const MeshAssetRegistry::Model& model = reg->GetModel(PrimitiveAssets::kModel, m_world->Epoch());
    const MeshAssetRegistry::Mesh*  mesh  = MeshAssetRegistry::Find(model, 0, name);
    if (!mesh)
    {
        static bool s_logged = false;
        if (!s_logged)
            LOG_WARNING("EditorLayer: the engine's Primitives model '%s' has no '%s' (%s) - rebuild it with "
                        "-assettool=bake_primitives; creating the MeshHandle primitive instead",
                        PrimitiveAssets::kModel, name, model.error.empty() ? "no such mesh" : model.error.c_str());
        s_logged = true;
        return NullEntity;
    }
    return CreateStaticMeshEntity(PrimitiveAssets::kModel, mesh->key, mesh->name, at);
}

// ==== Extract Mesh... ====
bool EditorLayer::ExtractModelMesh(const std::string& model, uint64_t meshKey, const std::string& destIn,
                                   bool retarget, bool overwrite, MeshExtractOutcome& out)
{
    out = MeshExtractOutcome{};
    const std::string modelRef = Sys::RelativizeAssetPath(model);
    std::string dest = destIn;
    if (dest.empty())
    {
        // Beside the model, named after the Mesh (the registry's view names it).
        std::string name = "Mesh";
        if (MeshAssetRegistry* reg = (m_renderer && m_world) ? m_renderer->GetMeshAssets() : nullptr)
            if (const MeshAssetRegistry::Mesh* m =
                    MeshAssetRegistry::Find(reg->GetModel(modelRef, m_world->Epoch()), meshKey, std::string()))
                name = m->name;
        dest = MeshExtract::DefaultDestination(modelRef, name);
    }
    if (!MeshExtract::Extract(modelRef, meshKey, dest, overwrite, out.file)) return false;

    // The new pair gets its identity now (the browser's scan does not stamp),
    // and the browser lists it.
    Resource::AssetGuidIndex::Get().EnsureGuid(out.file.meshlib);
    Resource::AssetGuidIndex::Get().EnsureGuid(out.file.iscn);
    m_needsRescan = true;

    // Re-pointing is an authoring edit: not during Play (the files are written
    // either way).
    if (!retarget || !m_world || !UndoEnabled()) return true;

    // Re-point every Static Mesh of (model, key) in the open scene at the new
    // model - same key, so the same Mesh; MeshInstanceSystem re-derives the
    // sections from the new library on its next pass.
    const std::string srcKey = Resource::MeshLibCanonicalPath(modelRef);
    std::vector<Entity> owners;
    if (auto* pool = m_world->GetPool<StaticMeshComponent>())
    {
        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (size_t i = 0; i < data.size(); ++i)
            if (m_world->IsAlive(ents[i]) && data[i].meshKey == meshKey &&
                Resource::MeshLibCanonicalPath(data[i].model) == srcKey)
                owners.push_back(ents[i]);
    }
    if (owners.empty()) return true;

    SyncedEditContext();
    m_editHistory.BeginMacro("Extract Mesh");
    for (Entity e : owners)
    {
        StaticMeshComponent* sm = m_world->GetComponent<StaticMeshComponent>(e);
        if (!sm) continue;
        const StaticMeshComponent before = *sm;
        sm->model = out.file.iscn;
        const StaticMeshComponent after = *sm;
        PushEdit(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(e), StaticMeshValue(before),
                                                                 StaticMeshValue(after), "Extract Mesh"));
        ++out.retargeted;
    }
    m_editHistory.EndMacro();
    SyncSceneDirtyToHistory();
    LOG_INFO("[MESHEXTRACT] re-pointed %u Static Mesh(es) at '%s'", out.retargeted, out.file.iscn.c_str());
    return true;
}

// ==== Unpack Model Instance ====
bool EditorLayer::UnpackModelInstance(Entity root, UnpackOutcome& out)
{
    out = UnpackOutcome{};
    auto fail = [&out](const char* code, std::string why)
    {
        out.code  = code;
        out.error = std::move(why);
        return false;
    };
    if (!m_world || root == NullEntity || !m_world->IsAlive(root))
        return fail("stale_handle", "the entity is not alive");
    if (!UndoEnabled())
        return fail("requires_edit_mode", "stop Play first - an unpack is an authoring edit");
    const SceneSourcePath* ssp = m_world->GetComponent<SceneSourcePath>(root);
    if (!ssp || ssp->path.empty())
        return fail("bad_value", "the entity is not a model instance's root (it has no SceneRef)");
    MeshAssetRegistry* reg = m_renderer ? m_renderer->GetMeshAssets() : nullptr;
    if (!reg) return fail("no_world", "the editor has no mesh registry");
    const std::string              modelPath = Sys::RelativizeAssetPath(ssp->path);
    const MeshAssetRegistry::Model& model    = reg->GetModel(modelPath, m_world->Epoch());
    if (model.meshes.empty())
        return fail("load_failed", model.error.empty() ? "the model names no mesh" : model.error);

    // (library, entry) -> (Mesh, section) of the model's view.
    std::unordered_map<std::string, std::pair<size_t, size_t>> where;
    for (size_t m = 0; m < model.meshes.size(); ++m)
        for (size_t s = 0; s < model.meshes[m].sections.size(); ++s)
            where.emplace(Resource::MeshLibCanonicalPath(model.meshes[m].libraryPath) + "#" +
                              std::to_string(model.meshes[m].sections[s].entry),
                          std::make_pair(m, s));

    // ---- The walk: every part under its node ----
    // Refused before anything changes: any per-part data from the file that a
    // Static Mesh cannot carry.
    struct Part { Entity e; size_t mesh, section; };
    std::vector<std::pair<Entity, std::vector<Part>>> nodes;   // node -> its parts, walk order
    std::unordered_map<Entity, size_t> nodeAt;
    std::vector<Entity> stack{ root };
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (const Children* ch = m_world->GetComponent<Children>(e))
            for (auto it = ch->entities.rbegin(); it != ch->entities.rend(); ++it) stack.push_back(*it);
        const MeshLibRef* ref = m_world->GetComponent<MeshLibRef>(e);
        if (!ref || !ref->IsValid()) continue;
        const std::string name = m_world->GetName(e);
        if (m_world->GetComponent<MeshSkinnedComponent>(e))
            return fail("unsupported", "the model is skinned ('" + name + "') - a Static Mesh draws the bind pose, "
                                       "so unpacking it is not supported yet");
        const MeshSourcePath* src = m_world->GetComponent<MeshSourcePath>(e);
        const auto w = src ? where.find(Resource::MeshLibCanonicalPath(src->path) + "#" + std::to_string(ref->meshId))
                           : where.end();
        if (w == where.end())
            return fail("unsupported", "part '" + name + "' is not a section of any of the model's Meshes");
        if (const MaterialOverride* mo = m_world->GetComponent<MaterialOverride>(e); mo && !mo->IsEmpty())
            return fail("unsupported", "part '" + name + "' carries a material override bag (the scene's MO / MOI "
                                       "lines) - a Static Mesh slot holds a material path only");
        if (m_world->GetComponent<FaceShadingComponent>(e))
            return fail("unsupported", "part '" + name + "' carries face shading (the scene's FS line) - a Static "
                                       "Mesh cannot carry it");
        const Parent* p    = m_world->GetComponent<Parent>(e);
        const Entity  node = p ? p->entity : NullEntity;
        if (node == NullEntity || !m_world->IsAlive(node))
            return fail("unsupported", "part '" + name + "' has no node");
        auto [it, added] = nodeAt.try_emplace(node, nodes.size());
        if (added) nodes.push_back({ node, {} });
        nodes[it->second].second.push_back({ e, w->second.first, w->second.second });
    }
    if (nodes.empty()) return fail("bad_value", "the instance has no mesh parts");

    // ---- Per node: the Meshes it places, each one whole ----
    struct Owner { Entity node; size_t mesh; std::vector<Part> parts; bool onNode; };
    std::vector<Owner> owners;
    for (auto& [node, parts] : nodes)
    {
        std::vector<std::pair<size_t, std::vector<Part>>> byMesh;   // first-seen order
        for (const Part& pt : parts)
        {
            auto g = std::find_if(byMesh.begin(), byMesh.end(), [&](const auto& x) { return x.first == pt.mesh; });
            if (g == byMesh.end()) byMesh.push_back({ pt.mesh, { pt } });
            else g->second.push_back(pt);
        }
        bool first = true;
        for (auto& [meshIdx, group] : byMesh)
        {
            const MeshAssetRegistry::Mesh& mesh = model.meshes[meshIdx];
            std::unordered_set<size_t> secs;
            for (const Part& pt : group) secs.insert(pt.section);
            if (group.size() != mesh.sections.size() || secs.size() != mesh.sections.size())
                return fail("unsupported", "node '" + m_world->GetName(node) + "' places " +
                                           std::to_string(group.size()) + " part(s) of mesh '" + mesh.name +
                                           "', which has " + std::to_string(mesh.sections.size()) +
                                           " section(s) - a Static Mesh draws a whole Mesh");
            owners.push_back({ node, meshIdx, std::move(group), first });
            first = false;
        }
    }

    // ---- What each owner will carry, read from its parts before they go ----
    // A part whose material is not the model's default for its section (the
    // scene's MR / MRI line) becomes a slot override.
    std::vector<StaticMeshComponent> comps;
    comps.reserve(owners.size());
    for (const Owner& o : owners)
    {
        const MeshAssetRegistry::Mesh& mesh = model.meshes[o.mesh];
        StaticMeshComponent sm;
        sm.model    = modelPath;
        sm.meshKey  = mesh.key;
        sm.meshName = mesh.name;
        for (const Part& pt : o.parts)
        {
            const MeshAssetRegistry::Section& sec = mesh.sections[pt.section];
            const MaterialSourcePath*         msp = m_world->GetComponent<MaterialSourcePath>(pt.e);
            const std::string have = msp ? Sys::RelativizeAssetPath(msp->path) : std::string();
            if (!have.empty() && Resource::AssetRefs::NormalizeRefPath(have) !=
                                 Resource::AssetRefs::NormalizeRefPath(sec.defaultMaterial))
            {
                sm.slotOverrides.push_back({ sec.key, have });
                ++out.slotOverrides;
            }
        }
        comps.push_back(std::move(sm));
    }
    const SceneSourcePath wasRef = *ssp;   // by value: the edits below move pools

    // ---- The edit: one undo entry ----
    const std::string rootName = m_world->GetName(root);
    const bool        undo     = UndoEnabled();
    if (undo)
    {
        SyncedEditContext();
        m_editHistory.BeginMacro("Unpack " + rootName);
    }
    ComponentSerializerRegistry& creg = GetComponentRegistry();

    // 1. A collider riding on a part (the file's first C record, or a scene MC
    //    override) moves to a child entity of its own, as authored data - the
    //    part is about to go, and its sections come back derived and transient.
    //    A serializer round trip, not a value copy: the new entity gets a body
    //    of its own instead of sharing the part's.
    std::unordered_set<Entity> nodesWithColliders;
    for (const Owner& o : owners)
        for (const Part& pt : o.parts)
        {
            if (!m_world->GetComponent<ColliderComponent>(pt.e)) continue;
            // A collider the model's asset collision gave the part, unchanged, is not
            // carried over: the node's StaticMesh derives it again (moved as well, it
            // would collide twice).
            bool fromAsset = false;
            if (Resource::ColliderIsSpawnDefault(*m_world, pt.e, &fromAsset) && fromAsset) continue;
            const std::string col = Editor::Undo::SerializeComponentBlob(creg, *m_world, pt.e, typeid(ColliderComponent));
            const std::string rb  = Editor::Undo::SerializeComponentBlob(creg, *m_world, pt.e, typeid(RigidBodyComponent));
            LocalTransform lt{};
            if (const LocalTransform* plt = m_world->GetComponent<LocalTransform>(pt.e)) lt = *plt;
            const std::string name = m_world->GetName(pt.e) + "_col";
            const Entity      node = o.node;
            RecordSpawn("Unpack Collider", [&]
            {
                const Entity c = m_world->CreateEntity();
                m_world->SetName(c, name);
                m_world->AddComponent<LocalTransform>(c, lt);
                m_world->AddComponent<GlobalTransform>(c, GlobalTransform{});
                LinkUnder(*m_world, c, node);
                Editor::Undo::DeserializeComponentBlob(creg, *m_world, c, typeid(ColliderComponent), col, m_assetMgr);
                if (!rb.empty())
                    Editor::Undo::DeserializeComponentBlob(creg, *m_world, c, typeid(RigidBodyComponent), rb, m_assetMgr);
            });
            nodesWithColliders.insert(o.node);
            ++out.colliders;
        }

    // 2. The parts go (MeshLibRef has no serializer: saved as they are they
    //    would load back with no geometry).
    for (const Owner& o : owners)
        for (const Part& pt : o.parts)
        {
            if (!m_world->IsAlive(pt.e)) continue;
            if (undo)
                m_editHistory.Execute(Editor::Undo::MakeDeleteAction(
                    Editor::Undo::EntitySubtreeSnapshot::Capture(*m_world, pt.e), m_world->GetName(pt.e)));
            else
                m_world->DestroyEntity(pt.e);
            ++out.parts;
        }

    // 3. Each Mesh gets its owner: the node itself, or - a second Mesh under
    //    the same node - a child entity at the node's origin.
    for (size_t i = 0; i < owners.size(); ++i)
    {
        const Owner& o = owners[i];
        StaticMeshComponent& sm = comps[i];
        // Explicit colliders were kept as entities of their own; the StaticMesh's
        // asset collision must not add a second set on top of them.
        if (nodesWithColliders.count(o.node)) sm.collision = StaticMeshComponent::Collision::None;
        if (!o.onNode)
        {
            const Entity node = o.node;
            RecordSpawn("Unpack Mesh", [&]
            {
                const Entity c = m_world->CreateEntity();
                m_world->SetName(c, sm.meshName.empty() ? std::string("Static Mesh") : sm.meshName);
                m_world->AddComponent<LocalTransform>(c, LocalTransform{});
                m_world->AddComponent<GlobalTransform>(c, GlobalTransform{});
                m_world->AddComponent<VisibilityComponent>(c, VisibilityComponent{});
                LinkUnder(*m_world, c, node);
                m_world->AddComponent<StaticMeshComponent>(c, StaticMeshComponent(sm));
            });
            ++out.owners;
            continue;
        }
        const StaticMeshComponent value = sm;
        auto add = [value](World& w, Entity e)
        {
            if (auto* c = w.GetComponent<StaticMeshComponent>(e)) *c = value;
            else w.AddComponent<StaticMeshComponent>(e, StaticMeshComponent(value));
        };
        auto remove = [](World& w, Entity e)
        {
            if (w.GetComponent<StaticMeshComponent>(e)) w.RemoveComponent<StaticMeshComponent>(e);
        };
        if (undo)
            m_editHistory.Execute(std::make_unique<Editor::Undo::AddComponentAction>(m_world->MakeHandle(o.node), add,
                                                                                     remove, "Static Mesh"));
        else
            add(*m_world, o.node);
        ++out.owners;
    }

    // 4. The root stops being a reference: the whole subtree becomes the scene's
    //    own data, saved node by node.
    auto restoreRef = [wasRef](World& w, Entity e)
    {
        if (auto* c = w.GetComponent<SceneSourcePath>(e)) *c = wasRef;
        else w.AddComponent<SceneSourcePath>(e, SceneSourcePath(wasRef));
    };
    auto dropRef = [](World& w, Entity e)
    {
        if (w.GetComponent<SceneSourcePath>(e)) w.RemoveComponent<SceneSourcePath>(e);
    };
    if (undo)
        m_editHistory.Execute(std::make_unique<Editor::Undo::ValueEditAction>(m_world->MakeHandle(root), restoreRef,
                                                                              dropRef, "Unpack"));
    else
        dropRef(*m_world, root);

    if (undo)
    {
        m_editHistory.EndMacro();
        SyncSceneDirtyToHistory();
    }
    else
    {
        MarkSceneDirty();
    }
    MarkHierarchyDirty();
    LOG_INFO("[UNPACK] '%s' (%s): %u Static Mesh owner(s), %u part(s) removed, %u collider(s) kept, "
             "%u slot override(s)", rootName.c_str(), modelPath.c_str(), out.owners, out.parts, out.colliders,
             out.slotOverrides);
    return true;
}
