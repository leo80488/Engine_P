#include "ECS/FacialPresets.h"

#include "ECS/AnimationComponents.h" // SkeletonComponent / SkeletonRef
#include "ECS/AnimEntityResolve.h"   // Anim::ResolveAnimEntityOrSelf
#include "ECS/HierarchyComponents.h" // Children, Parent, SceneSourcePath
#include "Resource/ModelShapeKeys.h" // the model's presets (MP records)

#include <algorithm>
#include <chrono>
#include <mutex>
#include <unordered_set>

namespace Facial
{

Entity FindSkeletonCarrier(World& world, Entity root)
{
    if (root == NullEntity || !world.IsAlive(root)) return NullEntity;
    if (world.GetComponent<SkeletonComponent>(root)) return root;

    if (const auto* ref = world.GetComponent<SkeletonRef>(root))
        if (ref->entity != NullEntity && world.IsAlive(ref->entity) &&
            world.GetComponent<SkeletonComponent>(ref->entity))
            return ref->entity;

    std::unordered_set<Entity> visited;
    std::vector<Entity>        stack{ root };
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!visited.insert(e).second) continue;
        if (world.GetComponent<SkeletonComponent>(e)) return e;
        if (const auto* kids = world.GetComponent<Children>(e))
            for (auto it = kids->entities.rbegin(); it != kids->entities.rend(); ++it)
                stack.push_back(*it);
    }
    return NullEntity;
}

Entity ResolveSkeletonCarrier(World& world, Entity live)
{
    if (live == NullEntity || !world.IsAlive(live)) return NullEntity;

    const Entity anim = Anim::ResolveAnimEntityOrSelf(world, live);
    if (anim != NullEntity && world.GetComponent<SkeletonComponent>(anim)) return anim;

    Entity carrier = FindSkeletonCarrier(world, anim);
    if (carrier == NullEntity && live != anim)
        carrier = FindSkeletonCarrier(world, live);
    return carrier;
}

std::string OwnerModelPath(World& world, Entity owner)
{
    Entity e = owner;
    for (int depth = 0; depth < 64 && e != NullEntity && world.IsAlive(e); ++depth)
    {
        if (const auto* s = world.GetComponent<SceneSourcePath>(e); s && !s->path.empty()) return s->path;
        const auto* p = world.GetComponent<Parent>(e);
        e = p ? p->entity : NullEntity;
    }
    return {};
}

// The merged table per model path, rebuilt when ModelShapeKeysAt hands back a
// new parse (the file changed on disk). That question costs a few file stats, so
// it is asked at most every half second per model: a script may set expressions
// every frame. A Mesh Editor save is seen within that.
std::shared_ptr<const PresetTable> PresetsOfModel(const std::string& path)
{
    struct Entry
    {
        std::shared_ptr<const Resource::ModelShapeKeys> source;
        std::shared_ptr<const PresetTable>              table;
        std::chrono::steady_clock::time_point           checked{};
    };
    static std::mutex                              s_mutex;
    static std::unordered_map<std::string, Entry>  s_cache;
    static const std::shared_ptr<const PresetTable> s_empty = std::make_shared<const PresetTable>();
    if (path.empty()) return s_empty;

    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (const auto it = s_cache.find(path);
            it != s_cache.end() && it->second.table && now - it->second.checked < std::chrono::milliseconds(500))
            return it->second.table;
    }
    const std::shared_ptr<const Resource::ModelShapeKeys> keys = Resource::ModelShapeKeysAt(path);
    std::lock_guard<std::mutex> lock(s_mutex);
    Entry& entry = s_cache[path];
    entry.checked = now;
    if (entry.table && entry.source == keys) return entry.table;

    std::vector<const Resource::MeshShapeKeySetup*> setups;   // by MeshKey: the same table every run
    for (const auto& [key, setup] : keys->meshes)
        if (!setup.orphan) setups.push_back(&setup);
    std::sort(setups.begin(), setups.end(), [](const Resource::MeshShapeKeySetup* a,
                                               const Resource::MeshShapeKeySetup* b) { return a->meshKey < b->meshKey; });
    auto table = std::make_shared<PresetTable>();
    for (const Resource::MeshShapeKeySetup* s : setups)
        for (const Resource::ShapeKeyPreset& p : s->presets)
        {
            auto& weights = (*table)[p.name];
            for (const auto& [target, w] : p.weights)
            {
                const auto it = std::find_if(weights.begin(), weights.end(),
                                             [&](const std::pair<std::string, float>& x) { return x.first == target; });
                if (it == weights.end()) weights.emplace_back(target, w);
                else                     it->second = (std::max)(it->second, w);
            }
        }
    entry.source = keys;
    entry.table  = table;
    return table;
}

} // namespace Facial
