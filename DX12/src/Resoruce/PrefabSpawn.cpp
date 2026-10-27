// Runtime prefab spawns: LoadPrefab inside the spawn template, plus the [SPAWN] census. See
// Resource/PrefabSpawn.h.

#include "Resource/PrefabSpawn.h"

#include "Resource/LoadCounters.h"
#include "Resource/PrefabSerializer.h"   // LoadPrefab, CollectPrefabSubtreeDFS
#include "Resource/PrefabTemplate.h"     // the runtime spawn's caches
#include "Resource/PrefabSpawnQueue.h"   // IsPreloaded (the census)
#include "Renderer/Renderer.h"           // CountColdMaterialInputs, GetSkeletonRegistry
#include "ECS/Components.h"              // MaterialComponent
#include "Resource/SkeletonAsset.h"      // SkeletonRegistry::Count
#include "Diagnostics/Profiler.h"
#include "System/CommandLine.h"          // -spawn_warn_ms
#include "System/Log.h"

#include <string>
#include <unordered_set>

#if defined(WITH_HARNESS) && WITH_HARNESS
#include "ECS/AnimationComponents.h"     // the oracle's runtime half
#include "ECS/HierarchyOps.h"            // Hierarchy::DestroySubtree
#include "Resource/ComponentSerializers.h"
#include "System/CommandLine.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#endif

#include <chrono>
#include <vector>

namespace
{
    Resource::SpawnCensus g_last;
    bool                  g_haveLast = false;

#if defined(WITH_HARNESS) && WITH_HARNESS
    // ---- -prefab_templateverify=N: the template's oracle ----
    // The first N runtime spawns are instantiated again without the template (LoadPrefab as every
    // other caller runs it), and the two instances are compared entity by entity in DFS order:
    // every component the serializer registry writes and the runtime half it does not (the
    // skeleton's content, the skinned part's counts / key / buffers), with every 128-bit Guid
    // masked - each instance draws its own seed. The second instance is destroyed at once.
    struct TemplateVerify
    {
        int      budget     = -1;   // -1: not read yet
        uint32_t checked    = 0;
        uint32_t mismatches = 0;
    };
    TemplateVerify g_verify;

    std::string MaskGuids(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        size_t i = 0;
        while (i < s.size())
        {
            size_t j = i;
            while (j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) ++j;
            if (j - i == 32) { out += "<guid>"; i = j; continue; }
            if (j > i) { out.append(s, i, j - i); i = j; continue; }
            out.push_back(s[i++]);
        }
        return out;
    }

    uint64_t SkeletonFingerprint(const SkeletonAsset& s)
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        mix(&s.boneCount, sizeof(s.boneCount));
        mix(&s.realBoneCount, sizeof(s.realBoneCount));
        mix(&s.virtualBoneSignature, sizeof(s.virtualBoneSignature));
        for (uint32_t b = 0; b < s.boneCount && b < SkeletonAsset::MAX_BONES; ++b)
        {
            mix(s.boneNames[b], std::strlen(s.boneNames[b]));
            mix(&s.parentIndex[b], sizeof(s.parentIndex[b]));
            mix(&s.inverseBindPose[b], sizeof(s.inverseBindPose[b]));
        }
        return h;
    }

    std::vector<std::string> Describe(World& world, Entity root, Renderer* renderer)
    {
        std::vector<std::pair<std::string, const ComponentSerializer*>> sers;
        for (const auto& [type, ser] : GetComponentRegistry().All()) sers.emplace_back(ser.tag, &ser);
        std::sort(sers.begin(), sers.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        std::vector<Entity> ents;
        ::Resource::CollectPrefabSubtreeDFS(world, root, ents);
        std::vector<std::string> out;
        out.reserve(ents.size());
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            std::ostringstream ss;
            ss << "#" << i << " '" << world.GetName(e) << "'\n";
            for (const auto& [tag, ser] : sers)
                if (ser->has && ser->serialize && ser->has(world, e)) ser->serialize(world, e, ss);
            if (const SkeletonComponent* sk = world.GetComponent<SkeletonComponent>(e))
            {
                ss << "  [skeleton] bones=" << sk->boneCount;
                if (renderer && sk->assetIndex < renderer->GetSkeletonRegistry().Count())
                    ss << " asset=" << SkeletonFingerprint(renderer->GetSkeletonRegistry().Get(sk->assetIndex));
                ss << "\n";
            }
            if (const MeshSkinnedComponent* ms = world.GetComponent<MeshSkinnedComponent>(e))
                ss << "  [skinned] verts=" << ms->vertexCount << " idx=" << ms->indexCount
                   << " morphs=" << ms->morphTargetCount << " key=" << ms->sharedSourceKey
                   << " restPos=" << ms->restPosBuffer.handleId << " blend=" << ms->blendBuffer.handleId
                   << " index=" << ms->indexBuffer.handleId << "\n";
            out.push_back(MaskGuids(ss.str()));
        }
        return out;
    }

    void VerifyAgainstLoadPrefab(const Resource::SpawnCensus& c, Entity root, World& world,
                                 Resource::AssetManager& assetMgr, Renderer* renderer,
                                 Resource::AnimationClipSystem* animClipSys)
    {
        const std::vector<std::string> a = Describe(world, root, renderer);
        const Entity other = ::Resource::LoadPrefab(c.path, world, assetMgr, renderer, animClipSys, nullptr);
        if (other == NullEntity)
        {
            ++g_verify.mismatches;
            LOG_ERROR("[PREFAB_TEMPLATE_VERIFY] '%s': LoadPrefab without the template failed", c.path.c_str());
            return;
        }
        const std::vector<std::string> b = Describe(world, other, renderer);
        Hierarchy::DestroySubtree(world, other);

        uint32_t bad = 0;
        if (a.size() != b.size()) ++bad;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        {
            if (a[i] == b[i]) continue;
            if (++bad <= 3)
                LOG_ERROR("[PREFAB_TEMPLATE_VERIFY] '%s' entity #%zu differs:\n--- template\n%s--- LoadPrefab\n%s",
                          c.path.c_str(), i, a[i].c_str(), b[i].c_str());
        }
        ++g_verify.checked;
        if (bad) ++g_verify.mismatches;
        LOG_INFO("[PREFAB_TEMPLATE_VERIFY] path=%s entities=%zu/%zu differing=%u checked=%u mismatches=%u",
                 c.path.c_str(), a.size(), b.size(), bad, g_verify.checked, g_verify.mismatches);
    }
#endif
}

bool Resource::SpawnStats::Last(SpawnCensus& out)
{
    if (!g_haveLast) return false;
    out = g_last;
    return true;
}

Entity Resource::SpawnPrefab(const std::string& path, World& world, AssetManager& assetMgr,
                             Renderer* renderer, AnimationClipSystem* animClipSys,
                             const ECS::Guid* instanceSeed, const char* caller)
{
    PROF_SCOPE("Spawn.Prefab");

    SpawnCensus c;
    c.path   = path;
    c.caller = caller ? caller : "";
    c.frame  = Logger::CurrentFrame();

    // Differences across the call, on this thread (Resource/LoadCounters.h).
    const LoadCounters::Counts before = LoadCounters::ThisThread();
    const uint32_t skelBefore = renderer ? renderer->GetSkeletonRegistry().Count() : 0u;

    const auto t0 = std::chrono::steady_clock::now();
    Entity root = NullEntity;
    {
        // Inside the template: the cached bytes and records, one skeleton registration per model,
        // shared skinned parts not decoded.
        PrefabTemplate::Scope templateScope;
        c.templated = PrefabTemplate::Active();
        // A ready preload of this prefab was holding it.
        c.preloaded = PrefabSpawnQueue::IsPreloaded(path);
        root = ::Resource::LoadPrefab(path, world, assetMgr, renderer, animClipSys, instanceSeed);
    }
    const auto t1 = std::chrono::steady_clock::now();
    c.ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const LoadCounters::Counts after = LoadCounters::ThisThread();
    c.reads         = after.fileReads      - before.fileReads;
    c.coldMaterials = after.materialParses - before.materialParses;
    c.coldMeshlib   = after.meshlibLoads   - before.meshlibLoads;
    c.skeletons     = renderer ? renderer->GetSkeletonRegistry().Count() - skelBefore : 0u;
    c.skelRegistry  = renderer ? renderer->GetSkeletonRegistry().Count() : 0u;
    c.ok            = (root != NullEntity);

    if (c.ok)
    {
        // The census proper is NOT part of the measured span: what it asks the
        // renderer (shader keys, texture residency) is the observer's cost.
        std::vector<Entity> subtree;
        ::Resource::CollectPrefabSubtreeDFS(world, root, subtree);
        c.entities = static_cast<uint32_t>(subtree.size());
        if (renderer)
        {
            std::vector<const MaterialComponent*> materials;
            materials.reserve(subtree.size());
            for (Entity e : subtree)
                if (const MaterialComponent* mc = world.GetComponent<MaterialComponent>(e))
                    materials.push_back(mc);
            renderer->CountColdMaterialInputs(materials, c.coldShaders, c.coldTextures);
            // Their custom shaders resolve at the top of this frame's scene build, before the bind
            // (Renderer::QueueSpawnMaterialResolve).
            renderer->QueueSpawnMaterialResolve(subtree);
        }
    }

    SpawnStats::spawns.fetch_add(1, std::memory_order_relaxed);
    SpawnStats::syncNs.fetch_add(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()), std::memory_order_relaxed);

    LOG_INFO("[SPAWN] path=%s ms=%.2f coldMeshlib=%u coldMaterials=%u coldShaders=%u coldTextures=%u "
             "reads=%u skeletons=%u skelRegistry=%u entities=%u templated=%d preloaded=%d ok=%d caller=%s",
             c.path.c_str(), c.ms, c.coldMeshlib, c.coldMaterials, c.coldShaders, c.coldTextures,
             c.reads, c.skeletons, c.skelRegistry, c.entities, c.templated ? 1 : 0, c.preloaded ? 1 : 0,
             c.ok ? 1 : 0, c.caller.c_str());

    // Warn once per path about a synchronous spawn over the budget (-spawn_warn_ms, default 8)
    // that paid cold costs a preload would have taken off the frame, so the author preloads it
    // (Engine.PreloadPrefab) or spawns it asynchronously (Engine.SpawnPrefabAsync). A warm spawn
    // slowed by a busy machine does not count, nor does an async commit
    // (Profiler.Spawn().async.late counts the late ones).
    {
        const bool cold = c.coldMeshlib || c.coldMaterials || c.coldShaders || c.coldTextures || c.reads ||
                          c.skeletons;
        if (c.ok && cold && !c.preloaded && c.caller != "Engine.SpawnPrefabAsync")
        {
            static const double s_warnMs = [] {
                const double v = Sys::CommandLine::Get().GetFloat("-spawn_warn_ms", 8.0f);
                return v > 0.0 ? v : 8.0;
            }();
            static std::unordered_set<std::string> s_warned;   // main: runtime spawns run there
            if (c.ms > s_warnMs && s_warned.insert(c.path).second)
                LOG_WARNING("[SPAWN] slow path=%s ms=%.2f cold=meshlib:%u,materials:%u,shaders:%u,textures:%u "
                            "reads=%u skeletons=%u preloaded=0 caller=%s - preload it first (Engine.PreloadPrefab) "
                            "or spawn it with Engine.SpawnPrefabAsync",
                            c.path.c_str(), c.ms, c.coldMeshlib, c.coldMaterials, c.coldShaders, c.coldTextures,
                            c.reads, c.skeletons, c.caller.c_str());
        }
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    if (g_verify.budget < 0)
        g_verify.budget = (std::max)(0, Sys::CommandLine::Get().GetInt("-prefab_templateverify", 0));
    if (c.ok && g_verify.budget > 0)
    {
        --g_verify.budget;
        VerifyAgainstLoadPrefab(c, root, world, assetMgr, renderer, animClipSys);
    }
#endif

    g_last     = std::move(c);
    g_haveLast = true;
    return root;
}
