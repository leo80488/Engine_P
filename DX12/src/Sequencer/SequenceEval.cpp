#include "Sequencer/SequenceEval.h"
#include "Sequencer/SequenceTargets.h"

#include "ECS/AnimationComponents.h"
#include "ECS/AnimGraphComponent.h"
#include "ECS/CameraMotionComponent.h"
#include "ECS/CameraStackComponents.h"
#include "ECS/CameraStackSystem.h"   // Camera::PushVCam - registration only, never per cut
#include "ECS/GuidRegistry.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/MotionDirectorComponent.h"
#include "ECS/NotifyTypes.h"          // TimelineComponent + the Pending* mailboxes
#include "Audio/AudioComponents.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "UI/UICanvas.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AssetManager.h"
#include "Resource/PrefabSerializer.h"
#include "Resource/PrefabSpawn.h"   // Resource::SpawnPrefab (a spawnable is a runtime spawn)
#include "ECS/PhysicsComponents.h"
#include "Renderer/Renderer.h"
#include "System/Log.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>       // memcpy - castStamp hashes Guid bytes
#include <unordered_map>

namespace Seq
{

// ==== Asset cache ====
// shared_ptr, not a raw pointer into the map: the editor calls InvalidateSequenceCache on every
// Save, and a raw pointer held across frames would dangle when the user saves mid-preview (same
// reason as s_vfxCache in NotifyConsumerSystems.cpp).

namespace
{
    std::unordered_map<std::string, std::shared_ptr<const Sequence>> s_seqCache;

    // Starts at 1 so it never equals SequenceRuntimeComponent::assetRevision's default of 0: a fresh
    // runtime component always takes the acquire path once, even before anything is published.
    uint32_t s_seqRevision = 1;
}

std::shared_ptr<const Sequence> AcquireSequence(const std::string& path)
{
    if (path.empty()) return nullptr;
    const auto it = s_seqCache.find(path);
    if (it != s_seqCache.end()) return it->second;

    auto seq = std::make_shared<Sequence>();
    if (!LoadSequence(path, *seq))
    {
        // Cache the failure too, or a bad path re-reads (and re-logs) from disk every frame for as long
        // as the player exists.
        s_seqCache.emplace(path, nullptr);
        return nullptr;
    }
    auto shared = std::shared_ptr<const Sequence>(std::move(seq));
    s_seqCache.emplace(path, shared);
    return shared;
}

// Every mutation of the cache bumps the revision, an erase included: otherwise a player holding
// the erased shared_ptr is never told to go back to disk and a mid-preview Save changes nothing on
// screen.
void InvalidateSequenceCache(const std::string& path)
{
    if (s_seqCache.erase(path) != 0) ++s_seqRevision;
}

void ClearSequenceCache()
{
    if (!s_seqCache.empty()) ++s_seqRevision;
    s_seqCache.clear();
}

void PublishSequenceEdit(const std::string& path, const Sequence& seq)
{
    if (path.empty()) return;
    // A COPY, not a reference to the editor's document: the player holds it across frames while the
    // window keeps mutating its own, and the evaluator relies on its asset being immutable for the
    // tick.
    s_seqCache[path] = std::make_shared<const Sequence>(seq);
    ++s_seqRevision;
}

uint32_t SequenceCacheRevision() { return s_seqRevision; }

namespace
{
    // FNV-1a: any stable hash would do; this is the engine's idiom for content signatures and needs
    // no header.
    inline void HashBytes(uint64_t& h, const void* p, std::size_t n)
    {
        const auto* b = static_cast<const uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    template <class T>
    inline void HashPod(uint64_t& h, const T& v) { HashBytes(h, &v, sizeof(T)); }
    inline void HashStr(uint64_t& h, const std::string& s)
    {
        HashBytes(h, s.data(), s.size());
        const uint8_t terminator = 0u;      // "ab"+"c" must not hash as "a"+"bc"
        HashBytes(h, &terminator, 1);
    }
}

uint64_t ArmSignature(const Sequence& seq)
{
    uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis

    const uint64_t nb = seq.bindings.size();
    HashPod(h, nb);
    for (const Binding& b : seq.bindings)
    {
        HashPod(h, b.kind);
        HashPod(h, b.archetype);
        HashStr(h, b.role);
        HashStr(h, b.childPath);
        HashStr(h, b.prefabPath);
        // Hashed field by field, not as raw struct bytes: a future padded field would make hashing the
        // object an uninitialised-memory read.
        HashPod(h, b.defaultGuid.hi);
        HashPod(h, b.defaultGuid.lo);
        // spawnPos / spawnRot / spawnScale are deliberately absent - see ArmSignature in the header.
    }

    const uint64_t nt = seq.tracks.size();
    HashPod(h, nt);
    for (const Track& t : seq.tracks)
    {
        HashPod(h, t.type);
        HashPod(h, t.binding);
        // mute and solo both participate: they decide TrackAudible, and
        // ClaimActors provisions the Fade quad only for an audible Fade track.
        HashPod(h, t.mute);
        HashPod(h, t.solo);
    }
    return h;
}

void ApplySpawnTransforms(World& world, const Sequence& seq,
                          const SequenceRuntimeComponent& rt)
{
    const std::size_t n = std::min(seq.bindings.size(), rt.spawned.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        const Binding& b = seq.bindings[i];
        if (!b.IsSpawnable()) continue;
        const Entity e = rt.spawned[i];
        if (e == NullEntity || !world.IsAlive(e)) continue;
        // Same rule the evaluator follows: never write the transform of the entity the user has a gizmo
        // on, or the handle snaps back to the authored spawn position under the cursor.
        if (Suppress::IsSuppressed(e)) continue;
        if (auto* lt = world.GetComponent<LocalTransform>(e))
        {
            lt->translation = b.spawnPos;
            lt->rotation    = b.spawnRot;
            lt->scale       = b.spawnScale;
        }
    }
}

namespace Preview
{
    namespace { bool s_ownsViewport = false; }
    void SetOwnsViewport(bool v) { s_ownsViewport = v; }
    bool OwnsViewport()          { return s_ownsViewport; }
}

namespace Suppress
{
    namespace { Entity s_entity = NullEntity; }
    void   Set(Entity e)            { s_entity = e; }
    Entity Current()                { return s_entity; }
    bool   IsSuppressed(Entity e)   { return e != NullEntity && e == s_entity; }
}

namespace TimeScale
{
    namespace
    {
        std::vector<std::pair<uint32_t, float>> s_entries;
        void (*s_sink)(float) = nullptr;
        float s_lastApplied   = 1.f;

        void Apply()
        {
            const float v = Resolve();
            if (s_sink && v != s_lastApplied) { s_sink(v); s_lastApplied = v; }
        }
    }

    void SetSink(void (*fn)(float)) { s_sink = fn; }

    void Push(uint32_t ownerId, float value)
    {
        for (auto& e : s_entries)
            if (e.first == ownerId) { e.second = value; Apply(); return; }
        s_entries.emplace_back(ownerId, value);
        Apply();
    }

    void Release(uint32_t ownerId)
    {
        s_entries.erase(std::remove_if(s_entries.begin(), s_entries.end(),
                                       [ownerId](const auto& e) { return e.first == ownerId; }),
                        s_entries.end());
        Apply();
    }

    float Resolve()
    {
        float v = 1.f;
        for (const auto& e : s_entries) v = std::min(v, e.second);
        return v;
    }

    void Clear() { s_entries.clear(); Apply(); }
}

// ==== Spawnables ====
// Every SpawnArchetype has a recipe below. A spawnable that cannot be created (an unknown
// archetype, a FromPrefab with no AssetManager or a failed instantiate) stays NullEntity, which
// makes every track bound to it inert rather than crashing.

namespace
{
    // Above every gameplay camera, below the editor free-cam's own pin, so toggling Camera Lock (not a
    // priority arms race) decides who owns the viewport.
    constexpr int kSeqCameraPriority = 500000;

    Entity SpawnCameraArchetype(World& world, const Binding& b)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, "[Seq] " + b.role);
        // Never reaches a saved scene: a sequencer camera baked into the level would be a second camera
        // nobody placed, at a priority that wins.
        world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});

        LocalTransform lt;
        lt.translation = b.spawnPos;
        lt.rotation    = b.spawnRot;
        lt.scale       = b.spawnScale;
        world.AddComponent<LocalTransform>(e, lt);
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});

        // PushVCam is called ONCE, for registration: it creates the channel singleton and attaches the
        // VCam components. Do not call it per cut: it resumes the blend from its current value, which
        // breaks scrubbing.
        Camera::PushVCamArgs args{};
        args.channelId        = Camera::kMainChannel;
        args.priority         = kSeqCameraPriority;
        args.weight           = 1.f;
        args.blendInDuration  = 0.f;
        args.blendOutDuration = 0.f;
        Camera::PushVCam(world, e, args);

        // Take ownership of the blend, then park it: a camera with no live cut section must contribute
        // nothing, or spawning one would hijack the view the moment the sequence arms.
        world.AddComponent<SeqCameraOwnedTag>(e, SeqCameraOwnedTag{});
        if (auto* prio = world.GetComponent<VCamPriorityComponent>(e))
        { prio->enabled = false; prio->weight = 1.f; prio->priority = kSeqCameraPriority; }
        if (auto* bl = world.GetComponent<VCamBlendComponent>(e))
        { bl->currentBlend = 0.f; bl->state = BlendState::Inactive; }

        return e;
    }

    // A full-screen UI quad. UIImage with an EMPTY texturePath falls back to the renderer's 1x1 white
    // texture (UIPass), so a fade needs no asset and no render code - only a tinted quad whose alpha a
    // curve drives.
    // It is a canvas ROOT plus a CHILD widget, not one entity: a node is laid out only when it has a
    // UIRect and is reachable from a UICanvas through UIParent edges (UICanvas.h), so a single entity
    // with UICanvas + UIRect + UIImage is never drawn. The child is also a SCENE child
    // (Parent/Children) so the despawn subtree walk reaches it; teardown does not follow UIParent.
    Entity MakeUICanvasRoot(World& world, const std::string& name, int sortOrder)
    {
        const Entity root = world.CreateEntity();
        world.SetName(root, name);
        world.AddComponent<SceneTransientTag>(root, SceneTransientTag{});
        world.AddComponent<LocalTransform>(root, LocalTransform{});
        world.AddComponent<GlobalTransform>(root, GlobalTransform{});
        UI::UICanvas canvas;
        canvas.sortOrder = sortOrder;
        world.AddComponent<UI::UICanvas>(root, canvas);
        return root;
    }

    Entity MakeUIChild(World& world, Entity root, const std::string& name)
    {
        const Entity child = world.CreateEntity();
        world.SetName(child, name);
        world.AddComponent<SceneTransientTag>(child, SceneTransientTag{});
        world.AddComponent<LocalTransform>(child, LocalTransform{});
        world.AddComponent<GlobalTransform>(child, GlobalTransform{});

        UI::AttachUIParent(world, child, root);          // the LAYOUT edge

        Parent pr; pr.entity = root;                      // the TEARDOWN edge
        world.AddComponent<Parent>(child, pr);
        if (auto* ch = world.GetComponent<Children>(root)) ch->entities.push_back(child);
        else { Children c; c.entities.push_back(child); world.AddComponent<Children>(root, std::move(c)); }
        return child;
    }

    Entity SpawnFadeQuadArchetype(World& world, const Binding& b)
    {
        // sortOrder above the gameplay HUD: a fade covers everything.
        const Entity root  = MakeUICanvasRoot(world, "[Seq] " + b.role, 30000);
        const Entity quad  = MakeUIChild(world, root, "[Seq] " + b.role + " Quad");

        UI::UIRect rect;
        rect.anchorMin = { 0.f, 0.f };
        rect.anchorMax = { 1.f, 1.f };
        rect.offset    = { 0.f, 0.f };
        rect.size      = { 0.f, 0.f };
        world.AddComponent<UI::UIRect>(quad, rect);

        UI::UIImage img;                    // texturePath deliberately empty -> white
        img.color = { 0.f, 0.f, 0.f, 0.f }; // starts fully transparent
        world.AddComponent<UI::UIImage>(quad, img);
        return root;
    }

    Entity SpawnSubtitleArchetype(World& world, const Binding& b)
    {
        const Entity root = MakeUICanvasRoot(world, "[Seq] " + b.role, 29000); // under the fade
        const Entity node = MakeUIChild(world, root, "[Seq] " + b.role + " Text");

        UI::UIRect rect;
        rect.anchorMin = { 0.f, 0.f };
        rect.anchorMax = { 1.f, 0.f };
        rect.pivot     = { 0.5f, 0.f };
        rect.offset    = { 0.f, 120.f };
        rect.size      = { 0.f, 200.f };
        world.AddComponent<UI::UIRect>(node, rect);

        UI::UIText txt;
        txt.text    = "";
        txt.visible = false;                // no section live yet
        world.AddComponent<UI::UIText>(node, txt);
        return root;
    }

    Entity SpawnPPVolumeArchetype(World& world, const Binding& b)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, "[Seq] " + b.role);
        world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
        world.AddComponent<LocalTransform>(e, LocalTransform{});
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});

        ECS::PostProcessVolumeComponent vol;
        vol.isGlobal    = true;
        vol.priority    = 10000.f;          // a cinematic grade outranks level volumes
        vol.blendWeight = 0.f;              // contributes nothing until a section says so
        world.AddComponent<ECS::PostProcessVolumeComponent>(e, std::move(vol));
        return e;
    }

    Entity SpawnAudioSourceArchetype(World& world, const Binding& b)
    {
        const Entity e = world.CreateEntity();
        world.SetName(e, "[Seq] " + b.role);
        world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
        world.AddComponent<LocalTransform>(e, LocalTransform{});
        world.AddComponent<GlobalTransform>(e, GlobalTransform{});
        Audio::AudioSourceComponent src;
        src.is3D         = false;           // cinematic audio is 2D unless placed
        src.playOnEnable = false;
        world.AddComponent<Audio::AudioSourceComponent>(e, std::move(src));
        return e;
    }

    // Prefab spawnables. `assetMgr` is threaded in from the tick system; without it (headless /
    // tools) the binding stays NullEntity and every track on it is inert, like any other unresolvable
    // binding.
    Entity SpawnPrefabArchetype(World& world, const Binding& b,
                                Resource::AssetManager* assetMgr,
                                Renderer* renderer,
                                Resource::AnimationClipSystem* clips)
    {
        if (!assetMgr || b.prefabPath.empty()) return NullEntity;

        const Entity root = Resource::SpawnPrefab(b.prefabPath, world, *assetMgr, renderer, clips,
                                                  nullptr, "Sequencer.Spawnable");
        if (root == NullEntity)
        {
            LOG_WARNING("Sequencer: spawnable role '%s' could not instantiate '%s'",
                        b.role.c_str(), b.prefabPath.c_str());
            return NullEntity;
        }

        world.SetName(root, "[Seq] " + b.role);

        // SceneTransientTag on the WHOLE subtree, not just the root: the scene serializer skips per
        // entity, so an untagged descendant would be saved as an orphan with no parent.
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            if (!world.HasComponent<SceneTransientTag>(e))
                world.AddComponent<SceneTransientTag>(e, SceneTransientTag{});
            if (const Children* ch = world.GetComponent<Children>(e))
                for (Entity c : ch->entities) stack.push_back(c);
        }

        if (auto* lt = world.GetComponent<LocalTransform>(root))
        {
            lt->translation = b.spawnPos;
            lt->rotation    = b.spawnRot;
            lt->scale       = b.spawnScale;
        }
        return root;
    }

    void SpawnSpawnables(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                         Entity owner,
                         Resource::AssetManager* assetMgr, Renderer* renderer,
                         Resource::AnimationClipSystem* clips)
    {
        rt.spawned.assign(seq.bindings.size(), NullEntity);
        for (std::size_t i = 0; i < seq.bindings.size(); ++i)
        {
            const Binding& b = seq.bindings[i];
            if (!b.IsSpawnable()) continue;
            if (i < rt.bound.size() && rt.bound[i] != NullEntity) continue;

            Entity e = NullEntity;
            switch (b.archetype)
            {
            case SpawnArchetype::Camera:            e = SpawnCameraArchetype(world, b);      break;
            case SpawnArchetype::FadeQuad:          e = SpawnFadeQuadArchetype(world, b);    break;
            case SpawnArchetype::SubtitleText:      e = SpawnSubtitleArchetype(world, b);    break;
            case SpawnArchetype::PostProcessVolume: e = SpawnPPVolumeArchetype(world, b);    break;
            case SpawnArchetype::AudioSource:       e = SpawnAudioSourceArchetype(world, b); break;
            case SpawnArchetype::FromPrefab:
                e = SpawnPrefabArchetype(world, b, assetMgr, renderer, clips); break;
            default:
                break;
            }
            // The ownership edge: everything a sequence creates carries it, so an entity whose owner is gone
            // or un-armed can be collected instead of living forever.
            if (e != NullEntity)
                world.AddComponent<SeqSpawnedTag>(e, SeqSpawnedTag{ static_cast<uint32_t>(owner) });
            rt.spawned[i] = e;
            if (i < rt.bound.size()) rt.bound[i] = e;
        }
    }

    // Takes the list MOVED OUT of the runtime component, not the component: destroying entities
    // compacts pools and can move the component that owned the list mid-loop (see ReleaseActors).
    void DespawnSpawnables(World& world, const std::vector<Entity>& spawned)
    {
        for (Entity root : spawned)
        {
            if (root == NullEntity || !world.IsAlive(root)) continue;
            // Collect first, THEN destroy: DestroyEntity mutates the child lists being walked.
            std::vector<Entity> all, stack{ root };
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (!world.IsAlive(e)) continue;
                all.push_back(e);
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity c : ch->entities) stack.push_back(c);
            }
            for (auto it = all.rbegin(); it != all.rend(); ++it)
                if (world.IsAlive(*it)) world.DestroyEntity(*it);
        }
    }
}

// ==== Helpers ====

namespace
{
    // A section is WARM from (startFrame - preRoll). Warm-but-not-live is the preRoll window:
    // stateful subsystems (skinned animation, a Director's performance, cloth and chain physics
    // downstream of them) are driven so they have converged when the section is on screen, but their
    // clock is clamped to the section start and the event lane is suppressed.
    // Stateless absolute tracks (Transform, Property, Fade) do NOT use this: for them preRoll is
    // meaningless and EvaluateWeight's plain geometry is correct.
    bool SectionWarm(const Section& s, float frameF)
    {
        return s.active
            && frameF >= static_cast<float>(s.startFrame - s.preRoll)
            && frameF <  static_cast<float>(s.endFrame);
    }

    // Breadth-first walk of @p root's subtree matching a '/'-separated chain of entity names. Prefab
    // and .iscn descendants have no GUID the asset can name (each prefab instance derives its own), so a
    // binding addresses the GUID'd root plus a name path rather than the descendant directly.
    Entity ResolveChildPath(World& world, Entity root, const std::string& childPath)
    {
        if (root == NullEntity || childPath.empty()) return root;

        Entity cur = root;
        std::size_t pos = 0;
        while (pos <= childPath.size())
        {
            const std::size_t slash = childPath.find('/', pos);
            const std::string seg = childPath.substr(
                pos, slash == std::string::npos ? std::string::npos : slash - pos);
            pos = (slash == std::string::npos) ? childPath.size() + 1 : slash + 1;
            if (seg.empty()) continue;

            Entity found = NullEntity;
            std::vector<Entity> queue{ cur };
            for (std::size_t qi = 0; qi < queue.size() && found == NullEntity; ++qi)
            {
                const Entity e = queue[qi];
                if (!world.IsAlive(e)) continue;
                if (const Children* ch = world.GetComponent<Children>(e))
                    for (Entity c : ch->entities)
                    {
                        if (world.IsAlive(c) && world.GetName(c) == seg) { found = c; break; }
                        queue.push_back(c);
                    }
            }
            if (found == NullEntity) return NullEntity;
            cur = found;
        }
        return cur;
    }

    // Every entity under @p root (inclusive) carrying an AnimationComponent. The authored binding is
    // usually a prefab / scene ROOT whose animation lives on a descendant (MotionDirectorTickSystem
    // resolves the same indirection).
    void CollectAnimEntities(World& world, Entity root, std::vector<Entity>& out)
    {
        if (root == NullEntity) return;
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            if (world.GetComponent<AnimationComponent>(e))
                if (std::find(out.begin(), out.end(), e) == out.end())
                    out.push_back(e);
            if (const Children* ch = world.GetComponent<Children>(e))
                for (Entity c : ch->entities) stack.push_back(c);
        }
    }

    // Defined with the sub-sequence track evaluation further down; declared here because
    // ReleaseActors tears children down. DestroyChildren takes the list MOVED OUT of the runtime
    // component, for the same pool-compaction reason as DespawnSpawnables.
    void ReleaseChildPlayer(World& world, Entity child, EngineMode mode);
    void DestroyChildren(World& world,
                         const std::vector<SequenceRuntimeComponent::ChildPlayer>& children,
                         EngineMode mode);

    // The visual component of a spawned UI archetype lives on a CHILD of the canvas root (see
    // MakeUIChild), so a plain GetComponent on the bound entity finds nothing.
    template <typename T>
    T* FindInSubtree(World& world, Entity root)
    {
        if (root == NullEntity || !world.IsAlive(root)) return nullptr;
        std::vector<Entity> stack{ root };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            if (auto* c = world.GetComponent<T>(e)) return c;
            if (const Children* ch = world.GetComponent<Children>(e))
                for (Entity k : ch->entities) stack.push_back(k);
        }
        return nullptr;
    }

    void PushUnique(std::vector<Entity>& v, Entity e)
    {
        if (e != NullEntity && std::find(v.begin(), v.end(), e) == v.end()) v.push_back(e);
    }

    // Every entity this sequence may WRITE. Drives both the pre-animated capture set and the
    // discontinuity re-arm set, so the two cannot drift apart (a written-but-uncaptured entity is a
    // permanently dirtied scene; a written-but-un-rearmed one replays its notify track on a scrub).
    std::vector<Entity> CollectTouched(World& world, const Sequence& seq,
                                       const SequenceRuntimeComponent& rt)
    {
        std::vector<Entity> out;
        for (const Track& tr : seq.tracks)
        {
            if (tr.binding < 0 || tr.binding >= static_cast<int32_t>(rt.bound.size())) continue;
            const Entity target = rt.bound[static_cast<std::size_t>(tr.binding)];
            if (target == NullEntity) continue;
            PushUnique(out, target);

            switch (tr.type)
            {
            case TrackType::SkeletalAnim:
            {
                std::vector<Entity> anims;
                CollectAnimEntities(world, target, anims);
                for (Entity a : anims) PushUnique(out, a);
                break;
            }
            case TrackType::Director:
            {
                // A Director's own tracks point at other entities, so walking the director's subtree finds
                // nothing: follow its track list instead.
                if (auto* d = world.GetComponent<MotionDirectorComponent>(target))
                    for (MotionDirectorTrack& dtr : d->tracks)
                    {
                        const Entity dt = dtr.target.Resolve(world);
                        if (dt == NullEntity) continue;
                        PushUnique(out, dt);
                        std::vector<Entity> anims;
                        CollectAnimEntities(world, dt, anims);
                        for (Entity a : anims) PushUnique(out, a);
                    }
                break;
            }
            default:
                break;
            }
        }
        return out;
    }

    // Which touched entities need a POSE-AUTHORITY claim (beyond a pre-animated capture): only those
    // driven through the animation pipeline (SkeletalAnim, and a Director's Skinned tracks).
    std::vector<Entity> CollectPoseClaims(World& world, const Sequence& seq,
                                          const SequenceRuntimeComponent& rt)
    {
        std::vector<Entity> out;
        for (const Track& tr : seq.tracks)
        {
            if (tr.type != TrackType::SkeletalAnim && tr.type != TrackType::Director) continue;
            if (tr.binding < 0 || tr.binding >= static_cast<int32_t>(rt.bound.size())) continue;
            const Entity target = rt.bound[static_cast<std::size_t>(tr.binding)];
            if (target == NullEntity) continue;

            if (tr.type == TrackType::SkeletalAnim)
            {
                std::vector<Entity> anims;
                CollectAnimEntities(world, target, anims);
                for (Entity a : anims) PushUnique(out, a);
            }
            else if (auto* d = world.GetComponent<MotionDirectorComponent>(target))
            {
                for (MotionDirectorTrack& dtr : d->tracks)
                {
                    if (dtr.type != MotionDirectorTrack::Skinned) continue;
                    std::vector<Entity> anims;
                    CollectAnimEntities(world, dtr.target.Resolve(world), anims);
                    for (Entity a : anims) PushUnique(out, a);
                }
            }
        }
        return out;
    }
}

// ==== Binding resolution ====

void ResolveBindings(World& world, const Sequence& seq,
                     SequencePlayerComponent& player, SequenceRuntimeComponent& rt)
{
    const std::size_t n = seq.bindings.size();

    // Cache-first. A bound entity that is still alive with its recorded generation IS the binding:
    // entity identity survives renames and re-parents, which is more stable than re-deriving through
    // a name path. A row re-resolves when it was never resolved (NullEntity - the actor may spawn
    // late), died, or its id was recycled (generation mismatch).
    // Cast edits must land mid-play: a stamp over the player's role table (names + target GUIDs,
    // O(roles), no BFS, no allocation) invalidates every cached row when the cast changes.
    uint64_t castStamp = 1469598103934665603ull;
    for (const SequenceRoleBinding& rb : player.roleBindings)
    {
        for (const char c : rb.role)
        { castStamp ^= uint8_t(c); castStamp *= 1099511628211ull; }
        uint8_t gb[sizeof(ECS::Guid)];
        std::memcpy(gb, &rb.target.ownerGuid, sizeof(gb));
        for (const uint8_t byte : gb)
        { castStamp ^= byte; castStamp *= 1099511628211ull; }
    }
    const bool castChanged = (castStamp != rt.castStamp);
    rt.castStamp = castStamp;

    const bool sized = rt.bound.size() == n && rt.boundGen.size() == n;
    if (!sized || castChanged)
    {
        rt.bound.assign(n, NullEntity);
        rt.boundGen.assign(n, 0u);
    }

    for (std::size_t i = 0; i < n; ++i)
    {
        const Binding& b = seq.bindings[i];

        // 0. A Spawnable's entity IS its binding, created by ClaimActors. Re-checked every tick (cheap:
        //    IsAlive) because a spawned entity has no GUID to be found again through.
        if (b.IsSpawnable())
        {
            const Entity sp = (i < rt.spawned.size()) ? rt.spawned[i] : NullEntity;
            if (sp != NullEntity && world.IsAlive(sp))
            {
                rt.bound[i]    = sp;
                rt.boundGen[i] = world.GetGeneration(sp);
            }
            else
            {
                rt.bound[i]    = NullEntity;
                rt.boundGen[i] = 0u;
            }
            continue;
        }

        // Steady path: the generation check.
        if (rt.bound[i] != NullEntity
            && world.IsAlive(rt.bound[i])
            && world.GetGeneration(rt.bound[i]) == rt.boundGen[i])
            continue;
        rt.bound[i]    = NullEntity;
        rt.boundGen[i] = 0u;

        // 1. The CAST on the player wins: bindings are addressed by role so one .iseq serves many
        //    placements.
        Entity root = NullEntity;
        for (SequenceRoleBinding& rb : player.roleBindings)
            if (rb.role == b.role) { root = rb.target.Resolve(world); break; }

        // 2. Fall back to the asset's authoring-time default so a freshly opened .iseq previews
        //    without a cast.
        if (root == NullEntity && b.defaultGuid.IsValid())
            root = ECS::GuidRegistry::Get().Find(b.defaultGuid);

        const Entity e = ResolveChildPath(world, root, b.childPath);
        rt.bound[i]    = e;
        rt.boundGen[i] = (e != NullEntity) ? world.GetGeneration(e) : 0u;
    }

    if (!rt.bindingsDiagnosed)
    {
        rt.bindingsDiagnosed = true;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (rt.bound[i] != NullEntity) continue;
            if (seq.bindings[i].IsSpawnable()) continue;   // spawned later, not an error yet
            LOG_WARNING("Sequencer: role '%s' resolves to no entity — every track bound to "
                        "it is silently inert. Set it in the player's cast, or give the "
                        "asset a default GUID.", seq.bindings[i].role.c_str());
        }
    }
}

// ==== Claim / release ====

void ClaimActors(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                 Entity player, Resource::AssetManager* assetMgr,
                 Renderer* renderer, Resource::AnimationClipSystem* clips)
{
    if (rt.armed) return;

    // ---- 0. spawn ----
    // Before anything reads rt.bound: a Spawnable's entity IS its binding, and a track pointing at an
    // unspawned role would resolve to nothing.
    SpawnSpawnables(world, seq, rt, player, assetMgr, renderer, clips);

    // ---- 1. capture BEFORE claiming ----
    // The pose-authority component added below must not end up inside the snapshot, or restoring
    // would reinstate a claim nobody owns.
    rt.preAnim.clear();
    for (Entity e : CollectTouched(world, seq, rt))
    {
        if (!world.IsAlive(e)) continue;
        // Spawned entities have no pre-animated state worth keeping: they did not exist before the
        // sequence and will not exist after it.
        if (std::find(rt.spawned.begin(), rt.spawned.end(), e) != rt.spawned.end()) continue;
        SequenceRuntimeComponent::PreAnim pa;
        pa.entity     = e;
        pa.generation = world.GetGeneration(e);
        pa.comps      = world.CloneEntityComponents(e);
        rt.preAnim.push_back(std::move(pa));
    }

    // ---- 1b. the Fade track's own quad ----
    // Created here, not lazily during evaluation: having a Fade track is a static property of the
    // asset, and creating entities mid-evaluation reallocates pools under live references.
    if (rt.fadeQuad == NullEntity)
    {
        const bool hasFade = std::any_of(seq.tracks.begin(), seq.tracks.end(),
            [](const Track& t) { return t.type == TrackType::Fade && !t.mute; });
        if (hasFade)
        {
            Binding synthetic;
            synthetic.role = "Fade";
            rt.fadeQuad = SpawnFadeQuadArchetype(world, synthetic);
            if (rt.fadeQuad != NullEntity)
                world.AddComponent<SeqSpawnedTag>(rt.fadeQuad,
                    SeqSpawnedTag{ static_cast<uint32_t>(player) });
        }
    }

    // ---- 2. claim ----
    for (Entity e : CollectPoseClaims(world, seq, rt))
    {
        if (!world.IsAlive(e)) continue;
        if (!world.GetComponent<AnimPoseOverrideComponent>(e))
        {
            AnimPoseOverrideComponent ov;
            ov.owner  = static_cast<uint32_t>(player);
            ov.layers = AnimPoseOverrideComponent::All;
            world.AddComponent<AnimPoseOverrideComponent>(e, ov);
        }
        if (auto* anim = world.GetComponent<AnimationComponent>(e))
            anim->externalTime = true;
    }

    // A Transform track on a Dynamic rigid body loses: PhysicsInterpApplySystem runs later in the
    // frame and overwrites the transform. Warn at bind time instead of leaving the artist to debug it.
    for (const Track& tr : seq.tracks)
    {
        if (tr.type != TrackType::Transform) continue;
        if (tr.binding < 0 || tr.binding >= static_cast<int32_t>(rt.bound.size())) continue;
        const Entity e = rt.bound[static_cast<std::size_t>(tr.binding)];
        if (e == NullEntity) continue;
        if (const auto* rb = world.GetComponent<RigidBodyComponent>(e))
            if (rb->motion == RigidBodyComponent::Motion::Dynamic)
                LOG_WARNING("Sequencer: Transform track '%s' targets entity %u, which has a "
                            "DYNAMIC RigidBody — PhysicsInterpApplySystem runs later in the "
                            "frame and will overwrite the sequenced transform. Make the body "
                            "Kinematic for the shot.",
                            tr.name.c_str(), static_cast<uint32_t>(e));
    }

    rt.armed = true;
}

void ReleaseActors(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                   RestoreMode restoreMode, EngineMode mode)
{
    if (!rt.armed) { rt.preAnim.clear(); return; }

    // ---- 1. drop claims ----
    // RestoreEntityComponents only overwrites the types it captured; a component ADDED after the
    // capture (the claim) is invisible to it and would survive the restore forever.
    for (Entity e : CollectPoseClaims(world, seq, rt))
    {
        if (!world.IsAlive(e)) continue;
        world.RemoveComponent<AnimPoseOverrideComponent>(e);
        // Keep-state playback gives up the external clock without replaying its final pose, so a
        // gameplay driver can blend from this parked sample.
        if (restoreMode == RestoreMode::Never ||
            (restoreMode == RestoreMode::EditorOnly && mode != EngineMode::Edit))
            if (auto* anim = world.GetComponent<AnimationComponent>(e))
            {
                anim->externalTime = false;
                anim->paused = true;
                if (const auto* graph = world.GetComponent<AnimGraphComponent>(e);
                    graph && graph->enabled && !graph->graphPath.empty())
                {
                    // The graph resumes its own clock, keeping the pause intent from before the shot.
                    anim->paused = false;
                    for (const auto& saved : rt.preAnim)
                        if (saved.entity == e && saved.generation == world.GetGeneration(e))
                            for (const auto& [type, value] : saved.comps)
                                if (type == std::type_index(typeid(AnimationComponent)))
                                    anim->paused = static_cast<const AnimationComponent*>(value.get())->paused;
                }
            }
    }
    for (const Track& tr : seq.tracks)
    {
        if (tr.type != TrackType::Director) continue;
        if (tr.binding < 0 || tr.binding >= static_cast<int32_t>(rt.bound.size())) continue;
        const Entity e = rt.bound[static_cast<std::size_t>(tr.binding)];
        if (e == NullEntity || !world.IsAlive(e)) continue;
        if (auto* d = world.GetComponent<MotionDirectorComponent>(e))
        {
            d->externalTime = false;
            d->stopRequested = true;   // director releases its own tracks
        }
    }

    // ---- 2. restore ----
    // Required, not polish: sequencer writes bypass the editor's undo stack and scene-dirty marking,
    // so an unrestored Edit-mode write changes the scene permanently with no undo entry and no save
    // prompt; the user scrubs, saves, and the damage is baked in.
    const bool restore = (restoreMode == RestoreMode::Always)
                      || (restoreMode == RestoreMode::EditorOnly && mode == EngineMode::Edit);

    if (restore)
    {
        // Reverse insertion order, mirroring how the claims were applied.
        for (auto it = rt.preAnim.rbegin(); it != rt.preAnim.rend(); ++it)
        {
            if (!world.IsAlive(it->entity)) continue;
            if (world.GetGeneration(it->entity) != it->generation) continue;  // id recycled
            world.RestoreEntityComponents(it->entity, it->comps);
        }
    }

    // Destroy AFTER the restore pass: a spawned entity is never in the cache, but destroying first
    // would invalidate ids the restore loop iterates.
    // Move the lists out and finish EVERY write through `rt` before the first DestroyEntity: children
    // are players whose runtime components live in this same pool, and Erase is swap-with-back, so
    // the teardown can compact the slot `rt` occupies. Do not touch `rt` after this block.
    std::vector<Entity> spawned;
    spawned.swap(rt.spawned);
    std::vector<SequenceRuntimeComponent::ChildPlayer> children;
    children.swap(rt.children);
    const Entity fadeQuad = rt.fadeQuad;
    rt.fadeQuad = NullEntity;
    rt.preAnim.clear();
    rt.activeKeys.clear();
    rt.armed = false;

    DespawnSpawnables(world, spawned);
    DestroyChildren(world, children, mode);

    if (fadeQuad != NullEntity)
    {
        // Subtree: the quad itself is a scene child of the canvas root.
        std::vector<Entity> all, stack{ fadeQuad };
        while (!stack.empty())
        {
            const Entity e = stack.back();
            stack.pop_back();
            if (!world.IsAlive(e)) continue;
            all.push_back(e);
            if (const Children* ch = world.GetComponent<Children>(e))
                for (Entity k : ch->entities) stack.push_back(k);
        }
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (world.IsAlive(*it)) world.DestroyEntity(*it);
    }
}

void ReleaseOrphanedPlayerState(World& world, OrphanedPlayerRelease& rec)
{
    if (!rec.asset) return;

    // Mirror of ReleaseActors' claim-drop, driven by the listener's snapshot: the player's own
    // components are already gone, so a shim carries the binding table the collectors read.
    SequenceRuntimeComponent shim;
    shim.bound = std::move(rec.bound);

    for (Entity e : CollectPoseClaims(world, *rec.asset, shim))
    {
        if (!world.IsAlive(e)) continue;
        world.RemoveComponent<AnimPoseOverrideComponent>(e);
    }
    for (const Track& tr : rec.asset->tracks)
    {
        if (tr.type != TrackType::Director) continue;
        if (tr.binding < 0 || tr.binding >= static_cast<int32_t>(shim.bound.size())) continue;
        const Entity e = shim.bound[static_cast<std::size_t>(tr.binding)];
        if (e == NullEntity || !world.IsAlive(e)) continue;
        if (auto* d = world.GetComponent<MotionDirectorComponent>(e))
        {
            d->externalTime  = false;
            d->stopRequested = true;   // director releases its own tracks
        }
    }

    // RestoreMode::Always regardless of the asset's policy: the player was destroyed out from under
    // the system, and "leave the actors posed" only means posed under a player that still exists.
    for (auto it = rec.preAnim.rbegin(); it != rec.preAnim.rend(); ++it)
    {
        if (!world.IsAlive(it->entity)) continue;
        if (world.GetGeneration(it->entity) != it->generation) continue;  // id recycled
        world.RestoreEntityComponents(it->entity, it->comps);
    }
    rec.preAnim.clear();
}

// ==== Discontinuity ====

void OnDiscontinuity(World& world, const Sequence& seq, SequenceRuntimeComponent& rt,
                     Renderer* renderer, bool quietHistoryReset)
{
    // 1. Re-arm every cross-time notify cursor. TimelineSystem::CrossedTime reads prev > curr as a
    //    LOOP WRAP, so without this a backward scrub mass-fires every notify in the clip. -1 makes
    //    it snapshot silently (the idiom Animator.SetTime uses).
    for (Entity e : CollectTouched(world, seq, rt))
    {
        if (!world.IsAlive(e)) continue;
        if (auto* anim = world.GetComponent<AnimationComponent>(e))
        {
            anim->prevNotifyTime = -1.f;
            anim->lastNotifyClip = kInvalidAnimHandle;
        }
        if (auto* tl = world.GetComponent<TimelineComponent>(e))
            tl->lastObservedTime = -1.f;
        // 2. A camera-motion clip detects cuts by comparing against prevTime.
        if (auto* cm = world.GetComponent<CameraMotionComponent>(e))
            cm->prevTime = -1.f;
    }

    // 3. Clear the range-state key set. Nothing fills it: range-state tracks re-derive their live
    //    sections from the frame every tick, and sub-sequences track theirs in rt.children.
    rt.activeKeys.clear();

    // 4. TAA / SSR / FSR2 accumulate across frames with motion vectors, which cannot describe a cut;
    //    without this the pre-seek image smears into the new one for several frames.
    if (renderer) renderer->InvalidateTemporalHistory(quietHistoryReset);
}

// ==== Track evaluation ====

namespace
{
    // ---- Transform: absolute, multi-section weighted ----
    // Nine independent channels. An ABSENT channel is not driven at all (the component value
    // survives) rather than pulled to zero, so a single X-translation key does not slam Y, Z,
    // rotation and scale to the origin.
    void EvalTransform(World& world, const Track& tr, Entity target, float frameF)
    {
        // The user is dragging this entity's gizmo: writing it would fight the drag every frame and the
        // handle would never move (Record mode depends on this).
        if (Suppress::IsSuppressed(target)) return;

        LocalTransform* lt = world.GetComponent<LocalTransform>(target);
        if (!lt) return;

        float acc[9]  = {};
        float wsum[9] = {};
        for (const Section& s : tr.sections)
        {
            const float sw = s.EvaluateWeight(frameF);
            if (sw <= 0.f) continue;
            for (int r = 0; r < 9; ++r)
                if (const Channel* c = s.Find(static_cast<uint16_t>(r)))
                {
                    acc[r]  += c->Evaluate(frameF) * sw;
                    wsum[r] += sw;
                }
        }

        auto driven = [&](int r, float& outV)
        {
            if (wsum[r] <= 0.f) return false;
            outV = acc[r] / wsum[r];
            return true;
        };

        float v = 0.f;
        if (driven(Role::LocX, v)) lt->translation.x = v;
        if (driven(Role::LocY, v)) lt->translation.y = v;
        if (driven(Role::LocZ, v)) lt->translation.z = v;
        if (driven(Role::ScaleX, v)) lt->scale.x = v;
        if (driven(Role::ScaleY, v)) lt->scale.y = v;
        if (driven(Role::ScaleZ, v)) lt->scale.z = v;

        // Rotation is authored as three Euler DEGREE curves so each axis has its own tangents (a packed
        // quaternion key cannot). Overlapping sections blend in Euler space, as an animator reading two
        // overlapping curves expects; a rotation that crosses a 180 deg boundary should therefore be one
        // section with keys, not two overlapping ones.
        const bool anyRot = wsum[Role::RotX] > 0.f || wsum[Role::RotY] > 0.f || wsum[Role::RotZ] > 0.f;
        if (anyRot)
        {
            using namespace DirectX;
            // Seed from the current pose so a partially-keyed rotation keeps
            // its un-keyed axes.
            XMFLOAT3 euler{};
            const XMVECTOR q0 = XMQuaternionNormalize(XMLoadFloat4(&lt->rotation));
            XMFLOAT4X4 m{};
            XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(q0));
            // DirectX RollPitchYaw order is Rz * Rx * Ry, matching the engine's QuatToEulerDeg convention
            // (the .ivfx writer).
            const float sp = -m._32;
            if (sp >= 0.9999f || sp <= -0.9999f)
            {
                euler.x = std::asin(std::max(-1.f, std::min(1.f, sp)));
                euler.y = std::atan2(-m._13, m._11);
                euler.z = 0.f;
            }
            else
            {
                euler.x = std::asin(sp);
                euler.y = std::atan2(m._31, m._33);
                euler.z = std::atan2(m._12, m._22);
            }
            float rx = XMConvertToDegrees(euler.x);
            float ry = XMConvertToDegrees(euler.y);
            float rz = XMConvertToDegrees(euler.z);
            driven(Role::RotX, rx);
            driven(Role::RotY, ry);
            driven(Role::RotZ, rz);

            const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(rx), XMConvertToRadians(ry), XMConvertToRadians(rz));
            XMStoreFloat4(&lt->rotation, XMQuaternionNormalize(q));
        }
    }

    // ---- One-shot lane ----
    // A one-shot fires when the playhead CROSSES its trigger frame in the play direction, and only
    // when the step was continuous. In reverse the trigger edge flips to the section's END, so
    // reverse playback still fires things (a `frame < prevFrame` predicate would make it inert).
    bool CrossedForward(int32_t prev, int32_t curr, int32_t at)
    {
        return at > prev && at <= curr;     // half-open at prev, closed at curr
    }
    bool CrossedBackward(int32_t prev, int32_t curr, int32_t at)
    {
        return at < prev && at >= curr;
    }
    bool SectionTriggered(const Section& s, const EvalContext& ec)
    {
        if (!s.active || !ec.continuous) return false;
        return (ec.dir >= 0) ? CrossedForward (ec.prevFrame, ec.frame, s.startFrame)
                             : CrossedBackward(ec.prevFrame, ec.frame, s.endFrame);
    }

    // ---- Skeletal Animation: absolute ----
    // The pose is driven by writing primaryTime absolutely. Binding the CLIP is a separate,
    // edge-triggered concern, and it is guarded: AnimationClipSystem's same-clip dedupe does NOT
    // apply to a finished one-shot (`!looping && primaryTime >= duration - 1e-4` sets restart = true),
    // and a sequencer parks primaryTime at the clip's last frame at every section end and on any scrub
    // that lands there, so without the guard the clip would re-trigger every frame.
    void EvalSkeletalAnim(World& world, const Track& tr, Entity target,
                          const EvalContext& ec, Resource::AnimationClipSystem* clips)
    {
        std::vector<Entity> anims;
        CollectAnimEntities(world, target, anims);
        if (anims.empty()) return;

        for (const Section& s : tr.sections)
        {
            if (!SectionWarm(s, ec.frameF)) continue;

            const std::string alias = s.params.GetString("clip");
            const float localSec = (ec.frameRate > 0.f)
                ? (ec.frameF - static_cast<float>(s.startFrame)) / ec.frameRate : 0.f;
            const float offset = s.params.GetFloat("startOffset", 0.f);
            // Clamped at 0 so the preRoll window holds the first pose while cloth and chain physics settle,
            // instead of sampling before the clip's frame 0.
            const float t = std::max(0.f, s.ChannelValue(Role::AnimTime, ec.frameF,
                                                         localSec + offset));

            for (Entity ae : anims)
            {
                auto* anim = world.GetComponent<AnimationComponent>(ae);
                if (!anim) continue;

                if (!alias.empty() && clips)
                {
                    const bool alreadyCurrent = clips->GetCurrentAnimatorClip(world, ae) == alias;
                    const bool alreadyQueued  = clips->GetTargetAnimatorClip (world, ae) == alias;
                    if (!alreadyCurrent && !alreadyQueued)
                        clips->QueueAnimatorPlay(world, ae, alias, 0.f, false);
                }

                anim->externalTime = true;   // sample at t, never self-advance
                anim->paused       = false;
                anim->primaryTime  = t;
            }
            return;   // one live section wins; sections are row-sorted
        }
    }

    // ---- Audio: one-shot, with a real seek ----
    void EvalAudio(World& world, const Track& tr, Entity target, const EvalContext& ec)
    {
        auto* src = world.GetComponent<Audio::AudioSourceComponent>(target);
        if (!src) return;

        for (const Section& s : tr.sections)
        {
            const bool live = s.active && ec.frameF >= static_cast<float>(s.startFrame)
                                       && ec.frameF <  static_cast<float>(s.endFrame);
            if (!live) continue;

            src->volume = s.ChannelValue(Role::Volume, ec.frameF, src->volume);
            src->pitch  = s.ChannelValue(Role::Pitch,  ec.frameF, src->pitch);

            if (SectionTriggered(s, ec))
            {
                // Continuous playback crossed the trigger edge (either direction): start from the top of the
                // section.
                src->playRequested      = true;
                src->playStartOffsetSec = 0.f;
            }
            else if (!ec.continuous && ec.mode != EngineMode::Edit)
            {
                // A SEEK landed inside the section. Audio has no continuous scrub, so restart from the correct
                // offset: VO lines up after a jump instead of always restarting.
                // Not done in Edit: re-triggering a voice on every frame of a scrub drag is a machine-gun, not a
                // preview.
                const float into = (ec.frameF - static_cast<float>(s.startFrame))
                                 / std::max(1e-3f, ec.frameRate);
                src->playRequested      = true;
                src->playStartOffsetSec = std::max(0.f, into);
            }
            return;
        }

        // Outside every section: a voice this track started must not outlive it.
        if (src->state == Audio::PlayState::Playing) src->stopRequested = true;
    }

    // ---- VFX / Camera Shake / Event: one-shot into the existing mailboxes ----
    void EvalOneShotMailboxes(World& world, const Track& tr, Entity target,
                              const EvalContext& ec)
    {
        for (const Section& s : tr.sections)
        {
            if (!SectionTriggered(s, ec)) continue;

            switch (tr.type)
            {
            case TrackType::VFX:
            {
                auto* box = world.GetComponent<PendingVFXSpawns>(target);
                if (!box) { world.AddComponent<PendingVFXSpawns>(target, {}); box = world.GetComponent<PendingVFXSpawns>(target); }
                if (!box) break;
                PendingVFXSpawns::Spawn sp;
                sp.assetPath = s.params.GetString("asset");
                sp.mode      = static_cast<VFXAttachMode>(s.params.GetInt("mode", 0));
                sp.boneIndex = s.params.GetInt("bone", -1);
                sp.localPos  = s.params.GetVec3("offset", { 0, 0, 0 });
                sp.duration  = s.params.GetFloat("duration", 1.f);
                if (!sp.assetPath.empty()) box->spawns.push_back(std::move(sp));
                break;
            }
            case TrackType::CameraShake:
            {
                auto* box = world.GetComponent<PendingCameraEffects>(target);
                if (!box) { world.AddComponent<PendingCameraEffects>(target, {}); box = world.GetComponent<PendingCameraEffects>(target); }
                if (!box) break;
                PendingCameraEffects::Effect fx;
                fx.type = CameraEffectType::Shake;
                fx.params.shakeAmplitude = s.params.GetFloat("amplitude", 0.2f);
                fx.params.shakeFrequency = s.params.GetFloat("frequency", 20.f);
                fx.params.duration       = s.params.GetFloat("duration", 0.3f);
                box->effects.push_back(fx);
                break;
            }
            case TrackType::Event:
            {
                auto* box = world.GetComponent<PendingGenericNotifies>(target);
                if (!box) { world.AddComponent<PendingGenericNotifies>(target, {}); box = world.GetComponent<PendingGenericNotifies>(target); }
                if (!box) break;
                const std::string name = s.params.GetString("name");
                if (name.empty()) break;
                PendingGenericNotifies::Notify n;
                n.typeId = StringID(name);
                n.params = s.params;          // the whole bag reaches Lua verbatim
                box->notifies.push_back(std::move(n));
                break;
            }
            default: break;
            }
        }
    }

    // ---- Range-state lane ----
    // Which sections are live is derived from the frame ALONE. SectionKey names a (track, section)
    // pair; a Sub-Sequence compares the live set with the children it holds under these keys, so its
    // enter/exit edges come from set arithmetic and match on a forward pass and a backward scrub.
    uint64_t SectionKey(std::size_t track, std::size_t section)
    {
        return (static_cast<uint64_t>(track) << 32) | static_cast<uint32_t>(section);
    }

    // ---- Fade: absolute ----
    void EvalFade(World& world, const Sequence& seq, const Track& tr,
                  const SequenceRuntimeComponent& rt, float frameF)
    {
        // A Fade is a master track, so it drives every FadeQuad of the sequence. Alpha is the section's
        // weight envelope times an optional Weight curve; colour comes from the section param.
        float alpha = 0.f;
        DirectX::XMFLOAT4 colour{ 0.f, 0.f, 0.f, 1.f };
        for (const Section& s : tr.sections)
        {
            const float w = s.EvaluateWeight(frameF);
            if (w <= 0.f) continue;
            alpha  = std::clamp(w * s.ChannelValue(Role::Weight, frameF, 1.f), 0.f, 1.f);
            colour = s.params.GetVec4("color", colour);
        }

        auto apply = [&](Entity root)
        {
            if (root == NullEntity || !world.IsAlive(root)) return;
            if (auto* img = FindInSubtree<UI::UIImage>(world, root))
                img->color = { colour.x, colour.y, colour.z, alpha };
        };

        // The quad the sequence provisioned for itself...
        apply(rt.fadeQuad);

        // ...plus any FadeQuad spawnable the author placed, for a shot that wants two of them (a colour
        // wipe over a black fade).
        for (std::size_t i = 0; i < rt.spawned.size(); ++i)
        {
            if (i >= seq.bindings.size()) break;
            if (!seq.bindings[i].IsSpawnable()) continue;
            if (seq.bindings[i].archetype != SpawnArchetype::FadeQuad) continue;
            apply(rt.spawned[i]);
        }
    }

    // ---- Subtitle: range-state ----
    void EvalSubtitle(World& world, const Track& tr, Entity target, float frameF)
    {
        auto* txt = FindInSubtree<UI::UIText>(world, target);
        if (!txt) return;

        for (const Section& s : tr.sections)
        {
            if (!s.active) continue;
            if (frameF < static_cast<float>(s.startFrame) ||
                frameF >= static_cast<float>(s.endFrame)) continue;
            txt->text    = s.params.GetString("text");
            const DirectX::XMFLOAT4 c = s.params.GetVec4("color", { 1, 1, 1, 1 });
            // Fade the line in and out with the section's own ease envelope, so
            // a subtitle never pops.
            txt->color   = { c.x, c.y, c.z, c.w * s.EvaluateWeight(frameF) };
            txt->visible = true;
            return;
        }
        // No live section: hidden. Absolute, so a scrub cannot leave a stale line on screen.
        txt->visible = false;
    }

    // ---- Visibility: range-state ----
    void EvalVisibility(World& world, const Track& tr, Entity target, float frameF)
    {
        auto* vis = world.GetComponent<VisibilityComponent>(target);
        if (!vis) return;

        bool visible = false;
        for (const Section& s : tr.sections)
        {
            if (!s.active) continue;
            if (frameF < static_cast<float>(s.startFrame) ||
                frameF >= static_cast<float>(s.endFrame)) continue;
            visible = true;
            const int mask = s.params.GetInt("viewMask", -1);
            if (mask >= 0) vis->viewMask = static_cast<uint32_t>(mask);
            break;
        }
        // "Section present == visible" is the whole rule. Pre-animated restore
        // puts the authored flags back on stop.
        vis->SetVisible(visible);
    }

    // ---- Post Process: absolute ----
    void EvalPostProcess(World& world, const Track& tr, Entity target, float frameF)
    {
        auto* vol = world.GetComponent<ECS::PostProcessVolumeComponent>(target);
        if (!vol) return;

        float weight = 0.f;
        for (const Section& s : tr.sections)
        {
            const float w = s.EvaluateWeight(frameF);
            if (w <= 0.f) continue;
            weight = std::clamp(w * s.ChannelValue(Role::Weight, frameF, 1.f), 0.f, 1.f);
            const std::string prof = s.params.GetString("profile");
            // profilePath is the serialization source of truth; the engine's resolve step turns it into a
            // handle. Setting it only on change keeps that resolve off the per-frame path.
            if (!prof.empty() && vol->profilePath != prof) vol->profilePath = prof;
        }
        vol->blendWeight = weight;
    }

    // ---- Property / Material: absolute, via the SeqTarget registry ----
    // Both track types share one implementation (they differ only in which table rows they address),
    // so the "route material writes through the real setters" rule cannot be honoured in one and
    // forgotten in the other.
    void EvalProperty(World& world, const Track& tr, Entity target, float frameF)
    {
        const SeqTarget* tgt = FindSeqTarget(tr.targetId);
        if (!tgt || !tgt->Set) return;

        // Seed from the CURRENT value so a target with four components but one keyed channel leaves the
        // other three alone, like the Transform track's per-axis rule.
        float v[4] = { 0.f, 0.f, 0.f, 0.f };
        if (tgt->Get && !tgt->Get(world, target, v)) return;   // component absent
        const bool  haveCur = (tgt->Get != nullptr);
        const float cur[4]  = { v[0], v[1], v[2], v[3] };

        float acc[4]  = {};
        float wsum[4] = {};
        for (const Section& s : tr.sections)
        {
            const float sw = s.EvaluateWeight(frameF);
            if (sw <= 0.f) continue;
            for (int c = 0; c < tgt->count && c < 4; ++c)
            {
                const uint16_t role = static_cast<uint16_t>(Role::Scalar0 + c);
                if (const Channel* ch = s.Find(role))
                {
                    acc[c]  += ch->Evaluate(frameF) * sw;
                    wsum[c] += sw;
                }
            }
        }

        bool any = false;
        for (int c = 0; c < tgt->count && c < 4; ++c)
            if (wsum[c] > 0.f) { v[c] = acc[c] / wsum[c]; any = true; }

        // No-change guard (same rationale as MatFloatProp in ScriptSystem_Bindings.cpp): a material Set*
        // always calls SetDirty(), and a non-empty matDirty list disables the retained candidate cache AND
        // the DDGI/RT retained gather for the whole world that frame, so a constant section would
        // re-gather the scene every frame of the cutscene. Exact compare on purpose: the curves are
        // deterministic in frameF, so a flat section reproduces bit-identical floats.
        if (any && haveCur)
        {
            bool changed = false;
            for (int c = 0; c < tgt->count && c < 4; ++c)
                if (v[c] != cur[c]) { changed = true; break; }
            if (!changed) return;
        }

        if (any) tgt->Set(world, target, v);
    }

    // ---- Sub-Sequence / Shot: range-state ----
    // A live section owns a CHILD player entity, seeked every frame from the parent's playhead, so a
    // nested cutscene is exactly as scrubbable as a flat one: the child never advances its own clock.
    // A Shot is evaluated exactly like a Sub-Sequence: the child's own Camera Cut track picks the
    // camera, so there is no second camera code path.
    constexpr int32_t kMaxSubSequenceDepth = 8;

    // Takes the PLAYER ENTITY, not a runtime reference, and re-fetches through the pool after every
    // structural change: creating a child adds a SequencePlayerComponent and a
    // SequenceRuntimeComponent, either of which can reallocate its pool and dangle any reference held
    // across the call (including the tick system's).
    void EvalSubSequence(World& world, const Track& tr, std::size_t trackIdx,
                         Entity playerEnt, const EvalContext& ec)
    {
        auto Rt = [&]() -> SequenceRuntimeComponent*
        { return world.GetComponent<SequenceRuntimeComponent>(playerEnt); };

        SequenceRuntimeComponent* rtp = Rt();
        if (!rtp) return;

        if (rtp->depth >= kMaxSubSequenceDepth)
        {
            SequenceRuntimeComponent& rt = *rtp;
            // Once, not every frame: a self-referencing asset would otherwise fill the log as fast as it
            // fills the entity pool.
            if (!rt.bindingsDiagnosed)
                LOG_ERROR("Sequencer: sub-sequence nesting exceeded %d — a .iseq that "
                          "references itself, directly or through a chain",
                          kMaxSubSequenceDepth);
            return;
        }

        for (std::size_t si = 0; si < tr.sections.size(); ++si)
        {
            rtp = Rt();
            if (!rtp) return;
            SequenceRuntimeComponent& rt = *rtp;

            const Section& s = tr.sections[si];
            const uint64_t key = SectionKey(trackIdx, si);

            const bool live = s.active
                           && ec.frameF >= static_cast<float>(s.startFrame - s.preRoll)
                           && ec.frameF <  static_cast<float>(s.endFrame);

            auto it = std::find_if(rt.children.begin(), rt.children.end(),
                                   [key](const SequenceRuntimeComponent::ChildPlayer& c)
                                   { return c.key == key; });

            if (!live)
            {
                // Range EXIT: tear the child down. Derived from the frame alone, so a backward scrub past the
                // shot tears it down exactly as playing past it does.
                if (it != rt.children.end())
                {
                    const Entity childEnt = it->entity;
                    // Erase while `rt` is still valid: the release below destroys grandchildren and can compact
                    // this pool, moving `rt` out from under the iterator.
                    rt.children.erase(it);
                    if (world.IsAlive(childEnt))
                    {
                        // Released, not just flagged: `stopRequested` on an entity destroyed on the next line is never
                        // seen by the tick system, so the restore, the pose claims and the TimeScale hold are paid
                        // BEFORE the destroy.
                        ReleaseChildPlayer(world, childEnt, ec.mode);
                        if (world.IsAlive(childEnt)) world.DestroyEntity(childEnt);
                    }
                }
                continue;
            }

            const std::string childPath = s.params.GetString("sequence");
            if (childPath.empty()) continue;

            Entity childEnt = (it != rt.children.end()) ? it->entity : NullEntity;

            if (childEnt == NullEntity)
            {
                const int32_t childDepth = rt.depth + 1;

                const Entity ce = world.CreateEntity();
                world.SetName(ce, "[Seq Child] " + childPath);
                world.AddComponent<SceneTransientTag>(ce, SceneTransientTag{});
                SequencePlayerComponent cp;
                cp.sequencePath = childPath;
                // The child NEVER runs its own clock - the parent places it every frame. rate 0 makes that
                // explicit even if something else sets `playing`.
                cp.rate    = 0.f;
                cp.playing = true;
                cp.started = true;
                world.AddComponent<SequencePlayerComponent>(ce, std::move(cp));

                SequenceRuntimeComponent crt;
                crt.depth = childDepth;
                world.AddComponent<SequenceRuntimeComponent>(ce, std::move(crt));
                world.AddComponent<SeqSpawnedTag>(ce,
                    SeqSpawnedTag{ static_cast<uint32_t>(playerEnt) });

                // Both AddComponents above may have reallocated their pools: re-fetch every reference taken
                // before them.
                rtp = Rt();
                if (!rtp) return;
                rtp->children.push_back({ key, ce });
                childEnt = ce;
            }

            if (!world.IsAlive(childEnt)) continue;
            auto* cp = world.GetComponent<SequencePlayerComponent>(childEnt);
            if (!cp) continue;

            // Place the child. timeScale lets a shot run in slow motion inside a master timeline without the
            // child needing its own clock.
            const float scale  = s.params.GetFloat("timeScale", 1.f);
            const float local  = (ec.frameF - static_cast<float>(s.startFrame)) * scale;
            const int32_t want = std::max(0, static_cast<int32_t>(local));
            if (cp->frame != want || !cp->playing)
            {
                cp->playing         = true;
                cp->seekRequested   = true;
                cp->seekTargetFrame = want;
            }
            // Never paused on its own: the parent places it every frame, so the child freezes with the
            // parent.
            cp->paused = false;
        }
    }

    // A child torn down by DestroyEntity never passes through the tick system's stop edge, so
    // everything ReleaseActors owes the scene for it (the pre-animated restore, the pose-authority
    // claims, its TimeScale hold) is paid here, BEFORE the destroy. Otherwise the cast freezes in
    // the last sequenced pose and a child TimeScale track leaves the game in slow motion.
    void ReleaseChildPlayer(World& world, Entity child, EngineMode mode)
    {
        if (auto* crt = world.GetComponent<SequenceRuntimeComponent>(child); crt && crt->asset)
        {
            // Keep the asset alive across the call: releasing grandchildren can compact the pool slot `crt`
            // lives in.
            const std::shared_ptr<const Sequence> asset = crt->asset;
            ReleaseActors(world, *asset, *crt, asset->restoreMode, mode);
        }
        TimeScale::Release(static_cast<uint32_t>(child));
    }

    void DestroyChildren(World& world,
                         const std::vector<SequenceRuntimeComponent::ChildPlayer>& children,
                         EngineMode mode)
    {
        for (const auto& c : children)
        {
            if (!world.IsAlive(c.entity)) continue;
            ReleaseChildPlayer(world, c.entity, mode);
            if (world.IsAlive(c.entity)) world.DestroyEntity(c.entity);
        }
    }

    // ---- Camera Cut ----
    // Writes VCamPriorityComponent + VCamBlendComponent DIRECTLY. Do not call PushVCam / PopVCam /
    // HardCutTo here: they are stateful (PushVCam resumes from the current blend, and
    // CameraStackTickSystem integrates `currentBlend += dt / blendInDuration`), which would make the
    // on-screen camera a function of elapsed history instead of the playhead, and a scrub could not
    // reproduce playback.
    // Every live section's weight is EvaluateWeight(frameF), pure section geometry. Overlapping
    // sections at equal priority are normalized by the stack's own sumW, which is exactly a
    // cross-blend, identical whether the frame was reached forwards or backwards.
    void EvalCameraCut(World& world, const Sequence& seq, const Track& tr,
                       const SequenceRuntimeComponent& rt, float frameF)
    {
        // Weight per binding, accumulated from every live section.
        std::vector<float> camWeight(seq.bindings.size(), 0.f);
        std::vector<float> camFov   (seq.bindings.size(), -1.f);

        for (const Section& s : tr.sections)
        {
            const float w = s.EvaluateWeight(frameF);
            if (w <= 0.f) continue;
            const int bIdx = s.params.GetInt("binding", -1);
            if (bIdx < 0 || bIdx >= static_cast<int>(camWeight.size())) continue;

            camWeight[static_cast<std::size_t>(bIdx)] +=
                w * s.ChannelValue(Role::Weight, frameF, 1.f);

            const float fovDeg = s.ChannelValue(Role::Fov, frameF, -1.f);
            if (fovDeg > 0.f) camFov[static_cast<std::size_t>(bIdx)] = fovDeg;
        }

        // Apply to EVERY binding this track could address, not just the live ones: a camera live last
        // frame and not now must be driven to zero, or it keeps contributing forever.
        for (std::size_t i = 0; i < camWeight.size(); ++i)
        {
            if (i >= rt.bound.size()) break;
            const Entity cam = rt.bound[i];
            if (cam == NullEntity || !world.IsAlive(cam)) continue;
            auto* prio  = world.GetComponent<VCamPriorityComponent>(cam);
            auto* blend = world.GetComponent<VCamBlendComponent>(cam);
            if (!prio || !blend) continue;

            const float w = std::clamp(camWeight[i], 0.f, 1.f);
            prio->enabled = (w > 0.f);
            prio->weight  = 1.f;
            blend->currentBlend = w;
            blend->state = (w >= 1.f) ? BlendState::Active
                         : (w >  0.f) ? BlendState::BlendingIn
                                      : BlendState::Inactive;

            if (camFov[i] > 0.f)
                if (auto* vc = world.GetComponent<VirtualCameraComponent>(cam))
                    vc->fov = DirectX::XMConvertToRadians(camFov[i]);

        }
    }

    // The stack reads CameraPoseComponent, never the transform. Runs as a SECOND pass after every
    // track, because a Transform track and the Camera Cut track can appear in either order and the
    // pose must reflect this frame's transform.
    // Copied from LocalTransform, not GlobalTransform: TransformPropagateSystem runs in a LATER phase,
    // so GlobalTransform here is one frame stale (visible as camera lag). Sequencer cameras spawn as
    // roots, so local IS world.
    void PublishSeqCameraPoses(World& world, const SequenceRuntimeComponent& rt)
    {
        for (Entity cam : rt.spawned)
        {
            if (cam == NullEntity || !world.IsAlive(cam)) continue;
            if (!world.HasComponent<SeqCameraOwnedTag>(cam)) continue;
            auto* pose = world.GetComponent<CameraPoseComponent>(cam);
            const auto* lt = world.GetComponent<LocalTransform>(cam);
            if (!pose || !lt) continue;
            pose->position = lt->translation;
            pose->rotation = lt->rotation;
        }
    }

    // ---- Camera Motion: absolute, hands a .ianim camera clip its time ----
    void EvalCameraMotion(World& world, const Track& tr, Entity target, float frameF, float fps)
    {
        auto* cm = world.GetComponent<CameraMotionComponent>(target);
        if (!cm) return;

        for (const Section& s : tr.sections)
        {
            if (!SectionWarm(s, frameF)) continue;
            const float localSec = (fps > 0.f)
                                 ? (frameF - static_cast<float>(s.startFrame)) / fps : 0.f;
            cm->externalTime  = true;   // sample at our time, do not self-advance
            cm->time          = std::max(0.f, s.ChannelValue(Role::AnimTime, frameF, localSec));
            cm->playing       = true;
            cm->started       = true;
            cm->paused        = false;
            cm->stopRequested = false;
            return;
        }
    }

    // ---- Director: absolute, drives MotionDirectorComponent::masterTime ----
    void EvalDirector(World& world, const Track& tr, Entity target, float frameF, float fps)
    {
        MotionDirectorComponent* d = world.GetComponent<MotionDirectorComponent>(target);
        if (!d) return;

        for (const Section& s : tr.sections)
        {
            if (!SectionWarm(s, frameF)) continue;

            const float localFrames = frameF - static_cast<float>(s.startFrame);
            const float localSec    = (fps > 0.f) ? localFrames / fps : 0.f;
            // Clamped at 0: during the preRoll window localSec is negative, and a negative master time fans
            // out to AnimationComponent::primaryTime, where clip sampling would index before frame 0. The
            // performance holds its first pose while cloth and chain physics settle.
            const float t = std::max(0.f, s.ChannelValue(Role::AnimTime, frameF, localSec));

            d->externalTime  = true;    // we own masterTime; the director must not advance it
            d->masterTime    = t;
            d->playing       = true;
            d->started       = true;    // suppress its own playOnLoad
            d->paused        = false;
            d->stopRequested = false;
            return;                     // one live section wins; sections are row-sorted
        }
    }
}

// ==== Evaluate ====

void Evaluate(const Sequence& seq, SequenceRuntimeComponent& rt, const EvalContext& ec)
{
    World& world = *ec.world;

    for (std::size_t ti = 0; ti < seq.tracks.size(); ++ti)
    {
        const Track& tr = seq.tracks[ti];
        if (!seq.TrackAudible(tr)) continue;

        // ---- master tracks (no binding of their own) ----
        if (tr.binding < 0)
        {
            switch (tr.type)
            {
            case TrackType::CameraCut: EvalCameraCut(world, seq, tr, rt, ec.frameF); break;
            case TrackType::Fade:      EvalFade      (world, seq, tr, rt, ec.frameF); break;
            case TrackType::SubSequence:
            case TrackType::Shot:
                EvalSubSequence(world, tr, ti, ec.player, ec);
                break;
            case TrackType::TimeDilation:
            {
                // Absolute: recomputed from the playhead every tick and pushed under this player's id, so two
                // sequences compose instead of clobbering each other.
                bool live = false;
                float scale = 1.f;
                for (const Section& s : tr.sections)
                {
                    const float w = s.EvaluateWeight(ec.frameF);
                    if (w <= 0.f) continue;
                    live  = true;
                    scale = s.ChannelValue(Role::TimeScale, ec.frameF,
                                           s.params.GetFloat("scale", 1.f));
                }
                if (live) TimeScale::Push(static_cast<uint32_t>(ec.player), std::max(0.f, scale));
                else      TimeScale::Release(static_cast<uint32_t>(ec.player));
                break;
            }
            // Marker is annotation only.
            // TODO(sequencer_system_plan M6): the Scene Load track (SceneManager::RequestLoad) has no evaluator.
            default: break;
            }
            continue;
        }

        // ---- object tracks ----
        if (tr.binding >= static_cast<int32_t>(rt.bound.size())) continue;
        const Entity target = rt.bound[static_cast<std::size_t>(tr.binding)];
        if (target == NullEntity || !world.IsAlive(target)) continue;

        switch (tr.type)
        {
        case TrackType::Transform:
            EvalTransform(world, tr, target, ec.frameF);
            break;
        case TrackType::CameraMotion:
            EvalCameraMotion(world, tr, target, ec.frameF, ec.frameRate);
            break;
        case TrackType::SkeletalAnim:
            EvalSkeletalAnim(world, tr, target, ec, ec.clips);
            break;
        case TrackType::Audio:
            EvalAudio(world, tr, target, ec);
            break;
        case TrackType::VFX:
        case TrackType::CameraShake:
        case TrackType::Event:
            EvalOneShotMailboxes(world, tr, target, ec);
            break;
        case TrackType::Subtitle:
            EvalSubtitle(world, tr, target, ec.frameF);
            break;
        case TrackType::Visibility:
            EvalVisibility(world, tr, target, ec.frameF);
            break;
        case TrackType::PostProcess:
            EvalPostProcess(world, tr, target, ec.frameF);
            break;
        case TrackType::Property:
        case TrackType::MaterialParam:
            EvalProperty(world, tr, target, ec.frameF);
            break;
        case TrackType::Director:
            EvalDirector(world, tr, target, ec.frameF, ec.frameRate);
            break;
        default:
            // Deliberately silent for a track type with no evaluator here: a per-frame warning would be
            // worse than the missing behaviour.
            break;
        }
    }

    // Sub-sequence creation may have reallocated the runtime pool, so `rt` (a reference the caller
    // took from that pool) can be stale: re-fetch before the final pass.
    if (auto* live = world.GetComponent<SequenceRuntimeComponent>(ec.player))
        PublishSeqCameraPoses(world, *live);
}

} // namespace Seq
