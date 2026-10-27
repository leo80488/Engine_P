#include "Resource/ComponentWrite.h"

#include "AI/AIBehavior.h"              // AIBehaviorComponent
#include "Audio/AudioComponents.h"      // AudioSourceComponent
#include "ECS/Components.h"             // LightData, DecalComponent
#include "ECS/GuidRegistry.h"           // ReverseFind: what a ref's text says
#include "ECS/PerceptionComponent.h"
#include "ECS/PhysicsComponents.h"      // RigidBodyComponent, ColliderComponent
#include "ECS/IKRigComponent.h"
#include "Scripting/ScriptComponent.h"
#include "ECS/UnresolvedComponents.h"   // appends a line per line, like ScriptComponent
#include "UI/UICanvas.h"               // Canvas UI: the lease, pointer state, playhead, scale
#include "UI/UIDocument.h"             // a UI Document's instance
#include "Reflection/Property.h"        // a reflected type's Runtime fields
#include "Reflection/ReflectRegistry.h"

#include <algorithm>

#include <sstream>
#include <vector>

namespace ComponentWrite
{
namespace
{
    // An AttachmentRef before a rewrite, with the GUID its text carries - what
    // WriteAttachmentRef (ComponentSerializers.cpp) writes for it: its own
    // GUID, else the one its bound entity has been given since, else nothing
    // (a ref bound at runtime to an entity without a GUID is not written).
    struct RefBefore
    {
        AttachmentRef ref;
        ECS::Guid     written;
    };

    template <typename T>
    bool Is(std::type_index type) { return type == std::type_index(typeid(T)); }

    // The modules' cases (RegisterCapture), after Capture's own.
    std::vector<std::pair<std::type_index, CaptureFn>>& ModuleCaptures()
    {
        static std::vector<std::pair<std::type_index, CaptureFn>> s_captures;
        return s_captures;
    }

    // A multi-slot component whose deserializer APPENDS one slot per line:
    // emptied first, so the lines that follow ARE the slot list.
    void ClearSlots(World& w, Entity e, std::type_index type)
    {
        if (Is<ScriptComponent>(type))
            if (auto* sc = w.GetComponent<ScriptComponent>(e)) sc->scripts.clear();
        if (Is<UnresolvedComponents>(type))
            if (auto* u = w.GetComponent<UnresolvedComponents>(e)) u->lines.clear();
    }

    // The authored fields - what the serializer writes. Exact compares: the
    // question is whether the body Jolt built still describes the component.
    bool SameAuthored(const RigidBodyComponent& a, const RigidBodyComponent& b)
    {
        return a.motion == b.motion && a.mass == b.mass && a.linearDamping == b.linearDamping &&
               a.angularDamping == b.angularDamping && a.friction == b.friction &&
               a.restitution == b.restitution && a.gravityFactor == b.gravityFactor &&
               a.lockedAxes == b.lockedAxes;
    }

    bool SameAuthored(const ColliderComponent& a, const ColliderComponent& b)
    {
        auto same3 = [](const DirectX::XMFLOAT3& p, const DirectX::XMFLOAT3& q) {
            return p.x == q.x && p.y == q.y && p.z == q.z;
        };
        return a.shape == b.shape && same3(a.halfExtents, b.halfExtents) && a.radius == b.radius &&
               a.halfHeight == b.halfHeight && a.meshLibPath == b.meshLibPath &&
               a.meshLibMeshId == b.meshLibMeshId && same3(a.offset, b.offset) &&
               same3(a.rotationEulerDeg, b.rotationEulerDeg) && a.isTrigger == b.isTrigger &&
               a.layer == b.layer;
    }

    RefBefore Before(const AttachmentRef& r)
    {
        ECS::Guid g = r.ownerGuid;
        if (!g.IsValid() && r.cachedOwner != NullEntity)
            g = ECS::GuidRegistry::Get().ReverseFind(r.cachedOwner);
        return { r, g };
    }

    // The rewrite read back what was written for the ref -> the write left it
    // alone: the old ref whole, its binding (and resolve cache) included. A
    // different or cleared GUID is the text's change and stays.
    void KeepUnchangedRef(const RefBefore& old, AttachmentRef& now)
    {
        if (now.ownerGuid == old.written) now = old.ref;
    }
}

RuntimeState Capture(World& w, Entity e, std::type_index type)
{
    if (Is<IKRigComponent>(type))
    {
        const auto* rig = w.GetComponent<IKRigComponent>(e);
        if (!rig) return {};
        const IKRigComponent old = *rig;
        return RuntimeState([old](World& world, Entity ent) {
            auto* now = world.GetComponent<IKRigComponent>(ent);
            if (!now) return;
            const auto authored = ComponentCloner<IKRigComponent>::Clone(*now);
            *now = old;
            now->enabled = authored.enabled; now->debugDraw = authored.debugDraw;
            now->masterWeight = authored.masterWeight; now->ground = authored.ground;
            now->goals = authored.goals; now->maxDistance = authored.maxDistance;
            now->probeEveryFrames = authored.probeEveryFrames;
            // Refresh contacts for changed probe geometry; bone names validate
            // against ResolveRig's fingerprint on the next Prepare. Scripted
            // targets, AttachmentRefs and blend clocks survive the rewrite.
            if (old.ground.probeUp != now->ground.probeUp || old.ground.probeDown != now->ground.probeDown
                || old.ground.probeRadius != now->ground.probeRadius || old.ground.layerMask != now->ground.layerMask
                || old.ground.toeProbe != now->ground.toeProbe)
                now->groundInitialized = false;
        });
    }
    if (Is<LightData>(type))
    {
        const LightData* l = w.GetComponent<LightData>(e);
        if (!l) return {};
        const uint32_t seq = l->gatherSeq;
        return RuntimeState([seq](World& world, Entity ent) {
            if (auto* l = world.GetComponent<LightData>(ent)) l->gatherSeq = seq;
        });
    }
    if (Is<DecalComponent>(type))
    {
        const DecalComponent* d = w.GetComponent<DecalComponent>(e);
        if (!d) return {};
        auto material = d->material;
        const float fade = d->fadeAlpha;
        return RuntimeState([material, fade](World& world, Entity ent) {
            if (auto* d = world.GetComponent<DecalComponent>(ent)) { d->material = material; d->fadeAlpha = fade; }
        });
    }
    if (Is<Audio::AudioSourceComponent>(type))
    {
        const auto* a = w.GetComponent<Audio::AudioSourceComponent>(e);
        if (!a) return {};
        // The voice and the clip reference move to the new component; exactly
        // one component owns them before and after. If the rewrite changed the
        // clip, clipPath differs from boundClipPath and AudioSystem stops the
        // old voice and releases the old clip on its next update.
        const Audio::AudioSourceComponent old = *a;
        return RuntimeState([old](World& world, Entity ent) {
            auto* a = world.GetComponent<Audio::AudioSourceComponent>(ent);
            if (!a) return;
            a->clipHandle         = old.clipHandle;
            a->boundClipPath      = old.boundClipPath;
            a->activeVoice        = old.activeVoice;
            a->state              = old.state;
            a->playRequested      = old.playRequested;
            a->playStartOffsetSec = old.playStartOffsetSec;
            a->stopRequested      = old.stopRequested;
            a->paused             = old.paused;
            a->autoPlayFired      = old.autoPlayFired;
        });
    }
    if (Is<RigidBodyComponent>(type))
    {
        // Kept or rebuilt, decided on what the rewrite produced. Every authored field
        // equal (an idempotent Engine.AddComponent, an undo that restores what is there)
        // -> the live body comes back (bodyId + the sync state) and nothing is rebuilt.
        // Anything authored changed -> the new component has no body: PhysicsSystem
        // reaps the old one, handing its velocity over, and builds one from the new
        // values. A launch velocity the body has not consumed yet is carried either way.
        const RigidBodyComponent* rb = w.GetComponent<RigidBodyComponent>(e);
        if (!rb) return {};
        const RigidBodyComponent old = *rb;
        return RuntimeState([old](World& world, Entity ent) {
            RigidBodyComponent* now = world.GetComponent<RigidBodyComponent>(ent);
            if (!now) return;
            if (SameAuthored(old, *now))
                *now = old;   // the authored half is equal; this puts the runtime half back
            else if (old.hasInitialVelocity)
            {
                now->initialVelocity        = old.initialVelocity;
                now->initialAngularVelocity = old.initialAngularVelocity;
                now->hasInitialVelocity     = true;
            }
        });
    }
    if (Is<ColliderComponent>(type))
    {
        // An unchanged shape keeps its generation (and prewarm memo), so a live
        // body stays. A changed one is bumped - MarkDirty - so the body rebuilds
        // on the next step (a same-length meshPath edit would otherwise wait
        // for the sliced path hash).
        const ColliderComponent* c = w.GetComponent<ColliderComponent>(e);
        if (!c) return {};
        const ColliderComponent old = *c;
        return RuntimeState([old](World& world, Entity ent) {
            ColliderComponent* now = world.GetComponent<ColliderComponent>(ent);
            if (!now) return;
            if (SameAuthored(old, *now))
            {
                now->generation       = old.generation;
                now->shapePrewarmMemo = old.shapePrewarmMemo;
            }
            else
                now->generation = old.generation + 1u;
        });
    }
    // ---- Canvas UI (Entity.set takes Cv* tags) ----
    if (Is<UI::UIImage>(type))
    {
        // The texture lease's view of the image: the handle, the path it was
        // resolved from, the SRV and the source size. UIFramePainter re-resolves
        // them from the lease every frame, so this only spares the frame between
        // the write and the next paint a blank image - and only while the path is
        // the same (a new path is the lease's to swap).
        const UI::UIImage* im = w.GetComponent<UI::UIImage>(e);
        if (!im) return {};
        const UI::UIImage old = *im;
        return RuntimeState([old](World& world, Entity ent) {
            UI::UIImage* now = world.GetComponent<UI::UIImage>(ent);
            if (!now || now->texturePath != old.resolvedTexturePath) return;
            now->texture             = old.texture;
            now->resolvedTexturePath = old.resolvedTexturePath;
            now->srvGpuHandle        = old.srvGpuHandle;
            now->texWidth            = old.texWidth;
            now->texHeight           = old.texHeight;
        });
    }
    if (Is<UI::UIInteractable>(type))
    {
        // Pointer and focus state the interaction pass owns: a write mid-press
        // must not drop the press (the release would then click nothing) nor
        // the focus.
        const UI::UIInteractable* it = w.GetComponent<UI::UIInteractable>(e);
        if (!it) return {};
        const UI::UIInteractable old = *it;
        return RuntimeState([old](World& world, Entity ent) {
            UI::UIInteractable* now = world.GetComponent<UI::UIInteractable>(ent);
            if (!now) return;
            now->hovered = old.hovered;
            now->pressed = old.pressed;
            now->clicked = old.clicked;
            now->focused = old.focused;
        });
    }
    if (Is<UI::UISpriteAnimComponent>(type))
    {
        // The playhead: a write of the speed or the loop flag does not restart
        // the animation. A frame past the new grid is the grid's last.
        const UI::UISpriteAnimComponent* sa = w.GetComponent<UI::UISpriteAnimComponent>(e);
        if (!sa) return {};
        const UI::UISpriteAnimComponent old = *sa;
        return RuntimeState([old](World& world, Entity ent) {
            UI::UISpriteAnimComponent* now = world.GetComponent<UI::UISpriteAnimComponent>(ent);
            if (!now) return;
            const int cells = now->frameCount > 0 ? now->frameCount : (std::max)(1, now->columns * now->rows);
            now->elapsed = old.elapsed;
            now->frame   = (std::min)(old.frame, cells - 1);
        });
    }
    if (Is<UI::UICanvas>(type))
    {
        // The scale the last layout computed (the Inspector shows it; the next
        // layout writes it again).
        const UI::UICanvas* cv = w.GetComponent<UI::UICanvas>(e);
        if (!cv) return {};
        const float scale = cv->computedScale;
        return RuntimeState([scale](World& world, Entity ent) {
            if (UI::UICanvas* now = world.GetComponent<UI::UICanvas>(ent)) now->computedScale = scale;
        });
    }
    if (Is<UI::UIDocument>(type))
    {
        // The instance the document holds. A write of the SAME path keeps it (with every
        // handle a script took through FindUINode, a press in flight, a script's edits on
        // it); a new path is UpdateDocuments' to swap - spawnedPath tells it the path
        // changed.
        const UI::UIDocument* d = w.GetComponent<UI::UIDocument>(e);
        if (!d) return {};
        const UI::UIDocument old = *d;
        return RuntimeState([old](World& world, Entity ent) {
            UI::UIDocument* now = world.GetComponent<UI::UIDocument>(ent);
            if (!now) return;
            now->root           = old.root;
            now->asset          = old.asset;
            now->spawnedPath    = old.spawnedPath;
            now->error          = old.error;
            now->seenGeneration = old.seenGeneration;
        });
    }
    if (Is<PerceptionComponent>(type))
    {
        // What PerceptionSystem found, none of it authored: the sightings
        // (firstSeenTime says how long a target has been in view), the sticky
        // threat and its memory clock, the heard sounds, the tick phase and the
        // last query counts. Kept whatever changed - the next perception tick
        // rebuilds the sightings under the new tunables.
        const PerceptionComponent* p = w.GetComponent<PerceptionComponent>(e);
        if (!p) return {};
        const PerceptionComponent old = *p;
        return RuntimeState([old](World& world, Entity ent) {
            PerceptionComponent* now = world.GetComponent<PerceptionComponent>(ent);
            if (!now) return;
            now->visibleEntities         = old.visibleEntities;
            now->heardSounds             = old.heardSounds;
            now->lastKnownThreat         = old.lastKnownThreat;
            now->lastKnownThreatPosition = old.lastKnownThreatPosition;
            now->lastSeenTime            = old.lastSeenTime;
            now->timeSinceLastTick       = old.timeSinceLastTick;
            now->phaseSeeded             = old.phaseSeeded;
            now->lastCandidates          = old.lastCandidates;
            now->lastRays                = old.lastRays;
        });
    }
    if (Is<AIBehaviorComponent>(type))
    {
        // fixedTarget and each patrol point are AttachmentRefs: the text has
        // only a GUID, the entity a ref is bound to is runtime-only. Each ref
        // the write left alone comes back whole (see KeepUnchangedRef), so a
        // binding without a GUID survives and a bound ref keeps its entity; a
        // ref given another GUID, a cleared one, or a point past the old route
        // is the text's. The working memory is on BlackboardComponent, which a
        // rewrite does not touch.
        const AIBehaviorComponent* b = w.GetComponent<AIBehaviorComponent>(e);
        if (!b) return {};
        const RefBefore fixed = Before(b->fixedTarget);
        std::vector<RefBefore> points;
        points.reserve(b->patrolPoints.size());
        for (const AttachmentRef& r : b->patrolPoints) points.push_back(Before(r));
        return RuntimeState([fixed, points](World& world, Entity ent) {
            AIBehaviorComponent* now = world.GetComponent<AIBehaviorComponent>(ent);
            if (!now) return;
            KeepUnchangedRef(fixed, now->fixedTarget);
            for (size_t i = 0; i < now->patrolPoints.size() && i < points.size(); ++i)
                KeepUnchangedRef(points[i], now->patrolPoints[i]);
        });
    }
    for (const auto& [t, capture] : ModuleCaptures())
        if (t == type) return capture(w, e);
    // Any other reflected type: its Runtime fields (Reflect::FieldFlag::Runtime) are state the text
    // does not carry, put back field by field over the rewritten component.
    const Reflect::RuntimeEntry* reflected = Reflect::FindRuntime(type);
    if (!reflected || !reflected->desc) return {};
    const Reflect::TypeDescriptor& desc = *reflected->desc;
    std::vector<std::pair<Reflect::PropertyPath, Reflect::PropertyValue>> kept;
    const std::shared_ptr<void> before = w.CloneComponentByType(e, type);
    if (!before) return {};
    for (const Reflect::FieldDescriptor& f : desc.fields)
    {
        if (Reflect::IsLayout(f.kind) || !Reflect::Has(f, Reflect::FieldFlag::Runtime)) continue;
        Reflect::PropertyPath path;
        path.type = &desc;
        path.steps.push_back({ &f, -1, -1 });
        path.text = f.name;
        if (Reflect::Writable(path)) kept.emplace_back(path, Reflect::Get(before.get(), path));
    }
    if (kept.empty()) return {};
    return RuntimeState([kept, type](World& world, Entity ent) {
        const std::shared_ptr<void> now = world.CloneComponentByType(ent, type);
        if (!now) return;
        for (const auto& [path, value] : kept)
            Reflect::WriteProperty(now.get(), path, value, Reflect::WriteOrigin::Undo);
        world.RestoreComponentByType(ent, type, now);
    });
}

void RegisterCapture(std::type_index type, CaptureFn capture)
{
    ModuleCaptures().emplace_back(type, capture);
}

KVMap ParseLine(const std::string& line)
{
    KVMap kv;
    const auto colon = line.find(':');
    if (colon == std::string::npos) return kv;
    std::istringstream tok(line.substr(colon + 1));
    std::string t;
    while (tok >> t)
    {
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        kv[t.substr(0, eq)] = t.substr(eq + 1);
    }
    return kv;
}

bool RewriteFromBlob(ComponentSerializerRegistry& reg, World& w, Entity e,
                     std::type_index type, const std::string& blob,
                     Resource::AssetManager* assets)
{
    const auto& all = reg.All();
    const auto it = all.find(type);
    if (it == all.end() || !it->second.deserialize) return false;

    std::vector<KVMap> lines;
    {
        std::istringstream in(blob);
        std::string line;
        while (std::getline(in, line))
            if (line.find(':') != std::string::npos) lines.push_back(ParseLine(line));
    }
    if (lines.empty()) return false;

    const RuntimeState carry = Capture(w, e, type);
    ClearSlots(w, e, type);
    for (const KVMap& kv : lines) it->second.deserialize(w, e, kv, assets);
    carry.Restore(w, e);
    return true;
}

void RewriteFromKV(const ComponentSerializer& s, std::type_index type, World& w, Entity e,
                   const KVMap& kv, Resource::AssetManager* assets)
{
    if (!s.deserialize) return;
    const RuntimeState carry = Capture(w, e, type);
    ClearSlots(w, e, type);
    s.deserialize(w, e, kv, assets);
    carry.Restore(w, e);
}

bool RewriteFromKV(ComponentSerializerRegistry& reg, const std::string& tag, World& w, Entity e,
                   const KVMap& kv, Resource::AssetManager* assets)
{
    for (const auto& [type, s] : reg.All())
        if (s.tag == tag)
        {
            if (!s.deserialize) return false;
            RewriteFromKV(s, type, w, e, kv, assets);
            return true;
        }
    return false;
}
}
