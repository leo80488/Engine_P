#pragma once

// ComponentWrite: rewrite a component an entity already has through its
// ComponentSerializerRegistry entry, without losing what the text cannot carry.
// A deserializer builds a default-constructed T from the key/values and World::AddComponent
// overwrites the slot: a missing key resets to its default and every runtime-only field is lost.
// Right for a scene load; wrong for a live component (undo / redo, a multi-select field edit,
// Engine.AddComponent or Entity.set on an existing component, Component Transfer, a save-game
// patch). What Capture carries, and what a bare rewrite would break:
//   LightData             gatherSeq (else the light becomes the newest: a directional light
//                         becomes the sun, a spot / point light loses its shadow-slice priority)
//   DecalComponent        the material template (not serialized: a library decal stops
//                         drawing) and fadeAlpha
//   AudioSourceComponent  the live voice and the acquired clip (else the voice plays on
//                         unstoppable, the clip leaks and playOnEnable starts a second voice);
//                         a changed clip is AudioSystem's to swap (boundClipPath)
//   RigidBodyComponent    the live Jolt body when every authored field is unchanged; after a real
//                         change the new component has no body, and PhysicsSystem reaps the old
//                         one (handing over its velocity) and rebuilds. An unconsumed launch
//                         velocity (Physics.SpawnBall) is carried either way
//   ColliderComponent     generation: kept when the shape is unchanged, bumped when it changed
//                         so a live body rebuilds next step (a same-length meshPath change would
//                         otherwise wait for the sliced path hash)
//   PerceptionComponent   everything PerceptionSystem found (sightings and their view time, the
//                         sticky threat and its memory clock, heard sounds, the tick phase, the
//                         last query counts); else the agent goes blind and forgets its threat
//                         until its next tick
//   AIBehaviorComponent   fixedTarget / patrolPoints bindings: the text holds a GUID only (a ref
//                         to an entity without one is not written), so a ref the write left alone
//                         comes back whole; a changed or cleared GUID is the text's
//   ScriptComponent       nothing, but its deserializer appends a slot per line, so a rewrite
//                         clears the slot list first
//   UI::UIDocument        its instance (root, asset, spawnedPath): a write of the same path
//                         keeps every FindUINode handle, a press in flight, a script's edits
//   IKRigComponent and the Canvas UI image / interactable / sprite-anim / canvas components carry
//   their runtime state too (see Capture).
//   Any other reflected component: its fields marked Reflect::FieldFlag::Runtime, copied back over
//   the rewritten component (a new run-time field needs only that flag on its descriptor).
// Capture / RuntimeState::Restore bracket a rewrite of any shape (deserialize over the slot, or
// remove + re-add); RewriteFromBlob / RewriteFromKV are the one-call forms. A type whose
// runtime-only state must survive such a write adds its case to Capture (ComponentWrite.cpp); a
// module's component registers its case with RegisterCapture.

#include "ECS/ECS.h"
#include "Resource/ComponentSerializers.h"   // KVMap, ComponentSerializer(Registry)

#include <functional>
#include <string>
#include <typeindex>
#include <utility>

namespace Resource { class AssetManager; }

namespace ComponentWrite
{
    // The runtime-only state of one component, taken before a rewrite. Empty
    // for a type with nothing to carry, or when the entity lacks the component.
    class RuntimeState
    {
    public:
        RuntimeState() = default;
        explicit RuntimeState(std::function<void(World&, Entity)> restore)
            : m_restore(std::move(restore)) {}

        bool Empty() const { return !m_restore; }
        // Put it back onto @p e's component of the same type (no-op if it has none).
        void Restore(World& w, Entity e) const { if (m_restore) m_restore(w, e); }

    private:
        std::function<void(World&, Entity)> m_restore;
    };

    RuntimeState Capture(World& w, Entity e, std::type_index type);

    // A module's component whose runtime-only state must survive a rewrite
    // (Module::IEngineModule::RegisterTypes): @p capture is its Capture case.
    using CaptureFn = RuntimeState (*)(World& w, Entity e);
    void RegisterCapture(std::type_index type, CaptureFn capture);

    // Rewrite @p e's component @p type from a serializer blob ("  Tag: k=v ...\n", one line per
    // slot): afterwards its serialized fields are the text's and its runtime state is what it was.
    // Creates the component when @p e lacks it (nothing to carry then). False when the type has no
    // deserializer or the blob holds no line.
    bool RewriteFromBlob(ComponentSerializerRegistry& reg, World& w, Entity e,
                         std::type_index type, const std::string& blob,
                         Resource::AssetManager* assets);

    // The same for one key/value line of a component serializer `s` (of `type`).
    void RewriteFromKV(const ComponentSerializer& s, std::type_index type, World& w, Entity e,
                       const KVMap& kv, Resource::AssetManager* assets);
    // By serializer tag (scans the registry). False for an unknown tag.
    bool RewriteFromKV(ComponentSerializerRegistry& reg, const std::string& tag, World& w, Entity e,
                       const KVMap& kv, Resource::AssetManager* assets);

    // One "  Tag: k=v k=v" line as its key/values: tokens split on whitespace,
    // each at its first '='. The tokenization DeserializeComponentBlob uses.
    KVMap ParseLine(const std::string& line);
}
