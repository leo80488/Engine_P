#pragma once

// BTNode - base of every node type. Trees are pure data shared across entities; running state,
// cooldowns and repeat counts live in BTInstance, keyed by NodeId.
// Ownership: BTAsset owns the root and each node owns its children (unique_ptr); traversal uses raw
// pointers.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ECS/ECS.h"

class World;
struct BlackboardComponent;

namespace AI
{
    using NodeId = uint32_t;
    constexpr NodeId kInvalidNodeId = 0u;

    enum class NodeStatus : uint8_t
    {
        Success,
        Failure,
        Running,
        Invalid
    };

    struct BTInstance;
    class  ActionRegistry;

    // Passed by reference into every Tick: the per-tick ECS hooks and the entity's instance. Never
    // store it past the tick.
    struct BTContext
    {
        Entity                entity     = NullEntity;
        World*                world      = nullptr;
        BTInstance*           instance   = nullptr;   // heap (unique_ptr on AIComponent): stable
        ActionRegistry*       actions    = nullptr;   // owned by AISystem: stable
        float                 deltaTime  = 0.f;
        float                 elapsed    = 0.f;

        // Fetched on each access, never cached here: the pool is a std::vector, and a component add during
        // the tick (Engine.SpawnPrefab / Engine.AddComponent from a Lua leaf; unlike DestroyEntity these are
        // not deferred) reallocates it under a cached pointer. Same rule as the Lua component proxies, which
        // re-fetch through ScriptSystem::LuaWorld.
        BlackboardComponent* Blackboard() const;
    };

    // BTInstance - per-entity runtime state, owned by AIComponent: every value that would otherwise
    // have to live on a shared node.
    struct BTInstance
    {
        struct TraceEntry { NodeId id; NodeStatus status; };

        // Last ActionLeaf that returned Running. Written for inspection only: nothing reads it for control
        // flow (trees tick from the root) and the trace panel uses trace.
        NodeId runningNode = kInvalidNodeId;

        // Cooldown decorator: the ctx.elapsed time at which each node is ready again. A missing entry reads
        // as 0 (ready); not pruned when a branch is abandoned (see AISystem::TickEntity).
        std::unordered_map<NodeId, float> cooldowns;

        // Repeater decorator: successful child runs so far, counting up to the node's count.
        std::unordered_map<NodeId, uint32_t> repeats;

        // StatefulSequence: index of the child to resume at after a Running result; erased when the
        // sequence succeeds or fails.
        std::unordered_map<NodeId, uint32_t> sequenceCursors;

        // Nodes visited on the most recent tick with the status each returned, in return (post-) order, for
        // the editor trace panel. Cleared at the start of each tick that runs (AISystem::TickEntity), whose
        // abort-prune then drops sequenceCursors / repeats entries not in it: not editor-only data.
        std::vector<TraceEntry> trace;

        // When true, the next tick clears cooldowns, repeats, sequenceCursors, runningNode and the
        // blackboard's '_' scratch keys. Hot reload does not use it: AIComponent::SetTree replaces the
        // whole instance.
        bool resetRequested = false;
    };

    class BTNode
    {
    public:
        virtual ~BTNode() = default;

        virtual NodeStatus Tick(BTContext& ctx) = 0;

        // Not called by the traversal and not overridden by any node: an abandoned branch's state is pruned
        // by AISystem::TickEntity instead.
        virtual void OnEnter(BTContext& /*ctx*/) {}
        virtual void OnExit (BTContext& /*ctx*/, NodeStatus /*status*/) {}

        // Node type name ("Sequence", "Action", ...); DebugLabel adds the registry and designer names.
        virtual const char* TypeName() const = 0;

        BTNode* GetChild(size_t i) const
        {
            return i < children.size() ? children[i].get() : nullptr;
        }

        size_t ChildCount() const { return children.size(); }

        // Name for the trace panel and error messages: type plus the optional designer label. Leaves
        // append their registry name, so two Action leaves are distinguishable.
        virtual std::string DebugLabel() const
        {
            return name.empty() ? std::string(TypeName())
                                : std::string(TypeName()) + " \"" + name + "\"";
        }

        // Filled by composites / decorators, empty for leaves; GetChild gives raw access during traversal.
        std::vector<std::unique_ptr<BTNode>> children;

        // Assigned by BTAsset::AssignIds (pre-order, from 1 per asset). Meaningful only within one tree:
        // BTInstance state keyed by it must be reset when the tree changes (AIComponent::SetTree).
        NodeId id = kInvalidNodeId;

        // Optional designer label (e.g. "Attack if target visible") for the trace view; no logic reads it.
        std::string name;

        // Depth from the root (root = 0), set by AssignIds so the trace panel can indent and show which
        // Selector branch ran.
        uint16_t depth = 0;
    };
}
