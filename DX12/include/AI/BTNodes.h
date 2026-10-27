#pragma once

// Concrete BT node types: composites, decorators, leaves.
// Nodes are stateless so one tree can be shared by many entities; all per-entity state (sequence
// cursors, cooldown deadlines, repeat counts) lives in BTInstance, keyed by NodeId.

#include "AI/BTNode.h"
#include "AI/AIComponents.h"     // BBValue

#include <functional>
#include <optional>
#include <string>

namespace AI
{
    // ==== Composites ====

    // Sequence - ticks children left to right; stops at the first Failure or Running, Success only if
    // every child succeeds. No memory: after a Running child the next tick starts again at child 0, so
    // a child that must keep its progress does so itself (or use StatefulSequence).
    class Sequence final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Sequence"; }
    };

    // StatefulSequence (alias MemSequence) - a Sequence with memory: after a Running child the next
    // tick resumes at that child, so earlier successes are not re-run within one pass. The cursor lives
    // in BTInstance::sequenceCursors and resets on Failure and on completion. Multi-phase behaviours
    // compose from separate leaves:
    //   { type="StatefulSequence", children={ {Action WalkToRandomPoint},
    //                                        {Action IdleFor} } }
    class StatefulSequence final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "StatefulSequence"; }
    };

    // Selector - ticks children left to right; stops at the first Success or Running, Failure only if
    // every child fails.
    class Selector final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Selector"; }
    };

    // Parallel - ticks every child every tick.
    //   succeedOn - Success count that makes the whole Success (0 = all children)
    //   failOn    - Failure count that makes the whole Failure (0 is treated as 1); checked first
    // Otherwise Running. Nothing is remembered between ticks: a child that must run once gates itself
    // on a blackboard flag.
    class Parallel final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Parallel"; }

        uint32_t succeedOn = 0;   // 0 = all children must succeed
        uint32_t failOn    = 1;   // any single failure kills the parallel
    };

    // ==== Decorators (exactly one child) ====

    class Inverter final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Inverter"; }
    };

    // Repeater - each tick runs the child once; after count successes returns Success (Running until
    // then). A child Failure propagates and resets the count. count == 0 repeats forever (never Success).
    class Repeater final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Repeater"; }

        uint32_t count = 1;
    };

    // Cooldown - after the child succeeds, blocks it for seconds of AI time (ctx.elapsed). While
    // blocked returns Failure, never Running, so a parent Selector falls through to a sibling. Without
    // an instance it is a passthrough.
    class Cooldown final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Cooldown"; }

        float seconds = 1.f;
    };

    // BlackboardCondition - gates the optional child on a blackboard predicate:
    //   IsSet     - the key exists
    //   Equals    - the value has expected's type and equals it
    //   NotEquals - the key is missing, or the value differs in type or value
    // With expected unset, Equals and NotEquals fail on a present key.
    enum class BBCmpOp : uint8_t { IsSet, Equals, NotEquals };

    class BlackboardCondition final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "BlackboardCondition"; }

    private:
        bool Evaluate(const BlackboardComponent& bb) const;

    public:
        std::string             key;
        BBCmpOp                 op = BBCmpOp::IsSet;
        std::optional<BBValue>  expected;
    };

    // ==== Leaves ====

    // ActionLeaf - dispatches actionName through ActionRegistry: a native handler, else Lua
    // Actions[name]. params is forwarded to the handler as is.
    class ActionLeaf final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Action"; }
        // Adds the registry name so the trace panel can tell Action leaves apart.
        std::string DebugLabel() const override
        {
            std::string s = "Action:" + actionName;
            if (!name.empty()) s += " \"" + name + "\"";
            return s;
        }

        std::string                                actionName;
        std::unordered_map<std::string, BBValue>   params;
    };

    // ConditionLeaf - like ActionLeaf, from the Conditions table. Conditions must be pure: a Running
    // result is coerced to Failure with a warning.
    class ConditionLeaf final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "Condition"; }
        std::string DebugLabel() const override
        {
            std::string s = "Condition:" + conditionName;
            if (!name.empty()) s += " \"" + name + "\"";
            return s;
        }

        std::string                                conditionName;
        std::unordered_map<std::string, BBValue>   params;
    };

    // ==== Advanced: built in C++ only (ParseNode has no case for these) ====

    // UtilitySelector - ticks the child with the highest utility score. Negative or NaN scores count
    // as 0; a child scoring 0 is never chosen, and Failure is returned when no child scores above 0.
    // Score functions read the blackboard (and optionally the world) and must not change either.
    class UtilitySelector final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "UtilitySelector"; }

        // childScores[i] scores children[i]; a missing or empty entry scores 0.
        std::vector<std::function<float(const BTContext&)>> childScores;
    };

    // SubTree - ticks another BTAsset's root with the same context; owns no children. The shared_ptr
    // keeps the asset alive while any entity is mid-tick. Its NodeIds are not namespaced: they collide
    // with the outer tree's in BTInstance (see AISystem::TickEntity).
    class SubTree final : public BTNode
    {
    public:
        NodeStatus  Tick(BTContext& ctx) override;
        const char* TypeName() const override { return "SubTree"; }

        std::shared_ptr<class BTAsset> tree;
    };
}
