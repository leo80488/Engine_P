#pragma once

// ECS components for the Behavior Tree AI system.
//   AIComponent         - opts an entity in to BT execution: shared tree, per-entity
//                         instance state, tick frequency.
//   BlackboardComponent - string-keyed working memory shared by BT nodes and other
//                         systems (Perception, Movement, Damage), readable without BT internals.
// Tree assets are shared (one parsed BT, many BTInstances). Per-entity state (running node,
// cooldowns, repeat counts) lives in BTInstance, never on BTNode.

#include <DirectXMath.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ECS/ECS.h"
#include "AI/BTNode.h"   // complete type for unique_ptr<BTInstance>

namespace AI
{
    class BTAsset;

    // ==== Blackboard value types ====
    // Every type a BT node, gameplay system or Lua action writes. Vector3 is XMFLOAT3. Entity is the
    // raw uint32; store an EntityHandle under a separate key when lifetime safety matters.
    using BBValue = std::variant<
        bool,
        int,
        float,
        std::string,
        Entity,
        DirectX::XMFLOAT3,
        // Waypoint list (e.g. PatrolPoints); round-trips to Lua as { {x,y,z}, ... }.
        std::vector<DirectX::XMFLOAT3>,
        // String list (e.g. SetRandomState's candidates = { "WALK_1", "WALK_2" }). Without it the
        // Lua -> BBValue parser takes the XMFLOAT3 branch and collapses the list to a zero vector.
        std::vector<std::string>>;
}

struct BlackboardComponent
{
    std::unordered_map<std::string, AI::BBValue> values;

    // Typed accessors. Get returns nullptr when the key is missing or holds another type; the caller
    // decides whether that is an error.
    template <typename T>
    void Set(const std::string& key, T value)
    {
        values[key] = std::move(value);
    }

    template <typename T>
    const T* Get(const std::string& key) const
    {
        auto it = values.find(key);
        if (it == values.end()) return nullptr;
        return std::get_if<T>(&it->second);
    }

    template <typename T>
    T* GetMut(const std::string& key)
    {
        auto it = values.find(key);
        if (it == values.end()) return nullptr;
        return std::get_if<T>(&it->second);
    }

    bool Has(const std::string& key) const
    {
        return values.find(key) != values.end();
    }

    void Erase(const std::string& key) { values.erase(key); }
};

struct AIComponent
{
    // Shared parsed tree; entities using the same asset differ only by their BTInstance.
    std::shared_ptr<AI::BTAsset> tree;

    // Per-entity execution state (running node, cooldowns, repeat counts, debug trace). Heap-held so
    // the component moves cheaply and the instance stays stable across ComponentPool resizes.
    std::unique_ptr<AI::BTInstance> instance;

    // Seconds between ticks; 0 = every frame. AILODSystem rewrites it each frame from the LOD tier.
    float tickInterval     = 0.1f;
    float timeSinceLastTick = 0.f;

    // Runtime-only: set once the anti-convoy phase offset (hashed from the entity id on the first AI
    // tick) has been applied. Not serialized or cloned: a restored or reloaded agent must re-seed from
    // its new id, and a persisted value would skip the offset.
    bool  phaseSeeded = false;

    bool  enabled = true;

    // Ticks the next frame regardless of tickInterval, then clears. The BT polls at ~10 Hz (slower with
    // distance), too late for edge events such as an animation ending, a hit or a trigger. A wake flag
    // covers that for one bool and one branch; an event-driven BT (conditional aborts) would change the
    // traversal model for no extra benefit. Set from Lua via AI.Wake(entity) or from C++ on a
    // notify / damage event.
    bool  wakeRequested = false;

    // Source path for hot reload, debug labels and the inspector; empty when the tree was set in code.
    std::string treePath;

    // Always assign a tree through this. BTInstance state (cooldowns, repeats, sequenceCursors,
    // runningNode) is keyed by NodeId, and BTAsset::AssignIds numbers each asset separately: keeping the
    // instance across a tree swap aliases old state onto unrelated nodes (a cooldown blocking the wrong
    // node, a sequence resuming at a missing child). Resetting is cheap; preserving it is never correct.
    void SetTree(std::shared_ptr<AI::BTAsset> newTree, std::string sourcePath = {})
    {
        tree     = std::move(newTree);
        treePath = std::move(sourcePath);
        instance = tree ? std::make_unique<AI::BTInstance>() : nullptr;
        timeSinceLastTick = 0.f;
        phaseSeeded       = false;   // re-offset on the next tick
    }
};

// Snapshot/restore (Edit <-> Play) clone hook. AIComponent is move-only (unique_ptr<BTInstance>):
// copy the authored fields and leave the instance null; it is rebuilt lazily on the first AI tick.
// Must stay in this header so every ComponentPool<AIComponent> instantiation sees it.
template <>
struct ComponentCloner<AIComponent>
{
    static AIComponent Clone(const AIComponent& src)
    {
        AIComponent dst;
        dst.tree              = src.tree;             // shared BT asset (refcount)
        dst.tickInterval      = src.tickInterval;
        dst.timeSinceLastTick = src.timeSinceLastTick;
        dst.enabled           = src.enabled;
        dst.treePath          = src.treePath;
        // dst.instance stays null (rebuilt on next tick).
        return dst;
    }
};
