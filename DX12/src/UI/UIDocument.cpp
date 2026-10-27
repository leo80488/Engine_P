// UIDocument - spawns and removes the run-time instances of .iui references; see the header.
#include "UI/UIDocument.h"

#include "ECS/GuidComponent.h"
#include "ECS/HierarchyComponents.h"   // SceneTransientTag
#include "ECS/HierarchyOps.h"          // Hierarchy::DestroySubtree
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"           // Sys::RelativizeAssetPath
#include "UI/UIAsset.h"
#include "UI/UICanvas.h"
#include "UI/UICanvasLayout.h"         // UI::IsUINode, ResolveUIParent

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace UI
{
namespace
{
    using ChildMap = std::unordered_map<Entity, std::vector<Entity>>;

    // parent -> its UI children, from ONE pass over the UIParent pool (UIChildrenOf
    // scans the whole pool per call: a walk of N nodes would be N scans).
    ChildMap UIChildMap(const World& w)
    {
        ChildMap m;
        if (const auto* pool = w.GetPool<UIParent>())
        {
            const auto& ents = pool->Entities();
            const auto& data = pool->Data();
            for (std::size_t i = 0; i < data.size(); ++i)
            {
                if (!w.IsAlive(ents[i])) continue;
                const Entity p = ResolveUIParent(w, data[i]);
                if (p != NullEntity && p != ents[i]) m[p].push_back(ents[i]);
            }
        }
        return m;
    }

    // @p root and every UI node under it (a UIParent cycle stops at its first repeat).
    std::vector<Entity> UISubtree(const World& w, const ChildMap& kids, Entity root)
    {
        std::vector<Entity> out, stack{ root };
        std::unordered_set<Entity> seen;
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!seen.insert(e).second || !w.IsAlive(e)) continue;
            out.push_back(e);
            if (const auto it = kids.find(e); it != kids.end()) stack.insert(stack.end(), it->second.begin(), it->second.end());
        }
        return out;
    }

    // The instance @p d names, trusted only while its root still IS that instance's root: an
    // undo, a Play snapshot or an id the free list handed on can leave a document holding a root
    // id another entity owns now.
    bool HoldsInstance(const World& w, Entity host, const UIDocument& d)
    {
        if (d.root == NullEntity || !w.IsAlive(d.root)) return false;
        const UIDocumentMember* m = w.GetComponent<UIDocumentMember>(d.root);
        return m && m->host == host && m->root == d.root;
    }

    // Take an instance out: its nodes and whatever a script hung under them.
    void DestroyInstance(World& w, Entity root)
    {
        if (root != NullEntity && w.IsAlive(root)) Hierarchy::DestroySubtree(w, root);
    }

    // Every node of the instance under @p root: a run-time part of the scene, never
    // saved, and @p host's. A node the spawn did not make (a scene node saved under an
    // instance node, a script's) joins it - the next respawn would take it anyway -
    // and that is said once.
    std::size_t TagInstance(World& w, Entity root, Entity host, const std::unordered_set<Entity>& made,
                            const std::string& hostName)
    {
        const std::vector<Entity> nodes = UISubtree(w, UIChildMap(w), root);
        std::size_t foreign = 0;
        std::string first;
        for (const Entity e : nodes)
        {
            if (!w.HasComponent<SceneTransientTag>(e)) w.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
            w.AddComponent<UIDocumentMember>(e, UIDocumentMember{ host, root });
            if (!made.count(e) && foreign++ == 0) first = w.GetName(e);
        }
        if (foreign)
            LOG_WARNING("[UIDOC] '%s': %zu node(s) under its instance were not spawned from the asset ('%s' ...) - "
                        "they belong to the instance now: never saved, gone with it (keep one by moving it out of "
                        "the instance)", hostName.c_str(), foreign, first.c_str());
        return nodes.size();
    }

    // Instance nodes whose host is gone or holds another instance now (a new path, a
    // respawn, an undo that took the host away) - with whatever hangs under them, or a
    // script's row would keep a dead parent (layout's unresolved-edge retry every frame).
    void RemoveOrphans(World& w)
    {
        auto* members = w.GetPool<UIDocumentMember>();
        if (!members || members->Entities().empty()) return;
        std::vector<Entity> gone;
        const auto& ents = members->Entities();
        const auto& data = members->Data();
        for (std::size_t i = 0; i < ents.size(); ++i)
        {
            const UIDocumentMember& m = data[i];
            const UIDocument* d = w.IsAlive(m.host) ? w.GetComponent<UIDocument>(m.host) : nullptr;
            if (!d || d->root != m.root) gone.push_back(ents[i]);
        }
        std::size_t roots = 0;
        for (const Entity e : gone)
            if (w.IsAlive(e))
            {
                ++roots;
                Hierarchy::DestroySubtree(w, e);
            }
        if (roots)
            LOG_INFO("[UIDOC] removed %zu node(s) of instances no host holds (and what hung under them)", gone.size());
    }
}

void UpdateDocuments(World& world)
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // Reverse arm of the uidocument_game pack: no instance is ever made.
    static const bool s_off = Sys::CommandLine::Get().Has("-uidocument_off");
    if (s_off) return;
#endif
    RemoveOrphans(world);
    auto* pool = world.GetPool<UIDocument>();
    if (!pool || pool->Entities().empty()) return;
    const std::uint64_t gen = UIAssetGeneration();

    // Spawning adds entities and pools: walk a copy of the hosts.
    const std::vector<Entity> hosts(pool->Entities().begin(), pool->Entities().end());
    for (const Entity host : hosts)
    {
        UIDocument* d = world.GetComponent<UIDocument>(host);
        if (!d) continue;
        // Per document, not per process: the documents of a world that Stop or an undo
        // handed back never saw the asset re-read while they were away.
        const bool recheck  = d->seenGeneration != gen;
        d->seenGeneration   = gen;
        const bool alive    = HoldsInstance(world, host, *d);
        const bool samePath = d->spawnedPath == d->path;
        if (samePath && !recheck && (alive || !d->error.empty() || d->path.empty())) continue;
        // Only an instance this host really holds is ever destroyed here.
        const Entity old = alive ? d->root : NullEntity;
        if (d->path.empty())
        {
            DestroyInstance(world, old);
            if (!(d = world.GetComponent<UIDocument>(host))) continue;   // the host was under its own instance
            d->root = NullEntity;
            d->asset.reset();
            d->spawnedPath.clear();
            d->error.clear();
            continue;
        }
        std::string why;
        const std::shared_ptr<const UIAsset> asset = LoadUIAsset(d->path, &why);
        if (alive && samePath && asset && asset == d->asset) continue;   // the generation moved for another file
        const std::string hostName = world.GetName(host);   // a copy: spawning may move the names
        if (!asset)
        {
            // A broken file keeps what was there (an edit in progress must not blank
            // the screen); said once per path.
            if (d->error != why || !samePath)
                LOG_ERROR("[UIDOC] '%s': %s", hostName.c_str(), why.c_str());
            d->error       = why;
            d->spawnedPath = d->path;
            continue;
        }
        const bool canvasAsset = asset->root.Block("CvCanvas") != nullptr;
        const bool hostIsUI    = IsUINode(world, host);
        std::string refusal;
        if (!canvasAsset && !hostIsUI)
            refusal = "'" + d->path + "' is a UI node: its CvDoc goes on a canvas or a UI node, which it is placed in";
        else if (canvasAsset && hostIsUI)
            refusal = "'" + d->path + "' is a canvas, a root of its own: its CvDoc goes on an entity that is no UI node";
        if (!refusal.empty())
        {
            if (d->error != refusal) LOG_ERROR("[UIDOC] '%s': %s", hostName.c_str(), refusal.c_str());
            DestroyInstance(world, old);
            if (!(d = world.GetComponent<UIDocument>(host))) continue;
            d->root        = NullEntity;
            d->asset.reset();
            d->error       = refusal;
            d->spawnedPath = d->path;
            continue;
        }
        const bool again = old != NullEntity;
        DestroyInstance(world, old);
        if (!(d = world.GetComponent<UIDocument>(host))) continue;   // the host went with its own instance
        // The host's GUID seeds the nodes' GUIDs: the same instance GUIDs at every load
        // (a host without one gets fresh ones per spawn).
        UISpawnOptions o;
        o.parent   = canvasAsset ? NullEntity : host;
        o.assetKey = Sys::RelativizeAssetPath(d->path);
        if (const GuidComponent* gc = world.GetComponent<GuidComponent>(host); gc && gc->guid.IsValid()) o.seed = gc->guid;
        std::vector<std::pair<std::string, Entity>> made;
        const Entity root = SpawnUIAsset(world, *asset, o, &why, &made);
        if (!(d = world.GetComponent<UIDocument>(host))) continue;
        if (root == NullEntity)
        {
            if (d->error != why) LOG_ERROR("[UIDOC] '%s': %s", hostName.c_str(), why.c_str());
            d->root        = NullEntity;
            d->asset.reset();
            d->error       = why;
            d->spawnedPath = d->path;
            continue;
        }
        std::unordered_set<Entity> madeSet;
        for (const auto& m : made) madeSet.insert(m.second);
        const std::size_t n = TagInstance(world, root, host, madeSet, hostName);
        if (!(d = world.GetComponent<UIDocument>(host))) continue;
        d->root        = root;
        d->asset       = asset;
        d->spawnedPath = d->path;
        d->error.clear();
        LOG_INFO("[UIDOC] '%s' <- %s: %zu node(s)%s", hostName.c_str(), d->path.c_str(), n,
                 again ? " (again: the asset or the path changed)" : "");
    }
}

Entity DocumentHostOf(const World& world, Entity e)
{
    if (e == NullEntity || !world.IsAlive(e)) return NullEntity;
    const UIDocumentMember* m = world.GetComponent<UIDocumentMember>(e);
    return m ? m->host : NullEntity;
}

std::size_t DocumentNodeCount(const World& world, Entity host)
{
    const UIDocument* d = world.IsAlive(host) ? world.GetComponent<UIDocument>(host) : nullptr;
    if (!d || !HoldsInstance(world, host, *d)) return 0;
    std::size_t n = 0;
    if (const auto* members = world.GetPool<UIDocumentMember>())
        for (const UIDocumentMember& m : members->Data())
            if (m.host == host && m.root == d->root) ++n;
    return n;
}

std::size_t DocumentNodesUnder(const World& world, Entity root)
{
    if (root == NullEntity || !world.IsAlive(root)) return 0;
    std::size_t n = 0;
    for (const Entity e : UISubtree(world, UIChildMap(world), root))
        if (world.HasComponent<UIDocumentMember>(e) || world.HasComponent<UIDocument>(e)) ++n;
    return n;
}

} // namespace UI
