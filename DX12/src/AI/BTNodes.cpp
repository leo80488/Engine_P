#include "AI/BTNodes.h"
#include "AI/BTAsset.h"
#include "AI/ActionRegistry.h"
#include "AI/AIComponents.h"   // BlackboardComponent (BTContext::Blackboard)
#include "ECS/ECS.h"

#include "System/Log.h"

#include <cmath>

namespace AI
{
    // Appends a node's status to the instance trace for the editor trace panel. No-op without an
    // instance (unit tests build contexts without one).
    static void Record(const BTContext& ctx, NodeId id, NodeStatus s)
    {
        if (ctx.instance) ctx.instance->trace.push_back({ id, s });
    }

    NodeStatus Sequence::Tick(BTContext& ctx)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if (s == NodeStatus::Failure || s == NodeStatus::Running)
            {
                Record(ctx, id, s);
                return s;
            }
        }
        Record(ctx, id, NodeStatus::Success);
        return NodeStatus::Success;
    }

    NodeStatus StatefulSequence::Tick(BTContext& ctx)
    {
        size_t start = 0;
        if (ctx.instance)
        {
            auto it = ctx.instance->sequenceCursors.find(id);
            if (it != ctx.instance->sequenceCursors.end()) start = it->second;
            if (start >= children.size()) start = 0;   // stale cursor: restart at child 0
        }

        for (size_t i = start; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if (s == NodeStatus::Running)
            {
                if (ctx.instance)
                    ctx.instance->sequenceCursors[id] = static_cast<uint32_t>(i);
                Record(ctx, id, s);
                return s;
            }
            if (s == NodeStatus::Failure)
            {
                if (ctx.instance) ctx.instance->sequenceCursors.erase(id);
                Record(ctx, id, s);
                return s;
            }
            // Success: advance the cursor so a later child's Running does not re-run this one next tick.
            if (ctx.instance)
                ctx.instance->sequenceCursors[id] = static_cast<uint32_t>(i + 1);
        }

        if (ctx.instance) ctx.instance->sequenceCursors.erase(id);
        Record(ctx, id, NodeStatus::Success);
        return NodeStatus::Success;
    }

    NodeStatus Selector::Tick(BTContext& ctx)
    {
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if (s == NodeStatus::Success || s == NodeStatus::Running)
            {
                Record(ctx, id, s);
                return s;
            }
        }
        Record(ctx, id, NodeStatus::Failure);
        return NodeStatus::Failure;
    }

    NodeStatus Parallel::Tick(BTContext& ctx)
    {
        const uint32_t total      = static_cast<uint32_t>(children.size());
        const uint32_t needSucc   = succeedOn == 0 ? total : succeedOn;
        const uint32_t needFail   = failOn    == 0 ? 1u    : failOn;

        uint32_t succ = 0, fail = 0;
        for (size_t i = 0; i < children.size(); ++i)
        {
            const NodeStatus s = children[i]->Tick(ctx);
            if      (s == NodeStatus::Success) ++succ;
            else if (s == NodeStatus::Failure) ++fail;
        }

        NodeStatus out = NodeStatus::Running;
        if      (fail >= needFail) out = NodeStatus::Failure;
        else if (succ >= needSucc) out = NodeStatus::Success;
        Record(ctx, id, out);
        return out;
    }

    NodeStatus Inverter::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Failure); return NodeStatus::Failure; }
        const NodeStatus s = children[0]->Tick(ctx);
        NodeStatus out = s;
        if      (s == NodeStatus::Success) out = NodeStatus::Failure;
        else if (s == NodeStatus::Failure) out = NodeStatus::Success;
        Record(ctx, id, out);
        return out;
    }

    NodeStatus Repeater::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Success); return NodeStatus::Success; }

        // Each tick runs the child once; a Success bumps the per-instance count.
        const NodeStatus s = children[0]->Tick(ctx);

        if (s == NodeStatus::Running)
        {
            Record(ctx, id, NodeStatus::Running);
            return NodeStatus::Running;
        }

        // A child Failure propagates (and resets the count): reporting Running would keep an enemy
        // repeating an action that cannot succeed while the parent never moves on.
        if (s == NodeStatus::Failure)
        {
            if (ctx.instance) ctx.instance->repeats.erase(id);
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        // count == 0: repeat forever, never report Success
        if (count == 0)
        {
            Record(ctx, id, NodeStatus::Running);
            return NodeStatus::Running;
        }

        if (!ctx.instance)
        {
            // No instance to keep a count: a single run.
            Record(ctx, id, s);
            return s;
        }

        uint32_t& visited = ctx.instance->repeats[id];
        ++visited;
        if (visited >= count)
        {
            visited = 0;
            Record(ctx, id, NodeStatus::Success);
            return NodeStatus::Success;
        }
        Record(ctx, id, NodeStatus::Running);
        return NodeStatus::Running;
    }

    NodeStatus Cooldown::Tick(BTContext& ctx)
    {
        if (children.empty()) { Record(ctx, id, NodeStatus::Failure); return NodeStatus::Failure; }

        if (ctx.instance)
        {
            // An absolute deadline, not a countdown: a countdown advanced only on visited ticks stops cooling
            // down when the tree takes another branch, so "attack again after 5 s" could mean never.
            float& readyAt = ctx.instance->cooldowns[id];
            if (ctx.elapsed < readyAt)
            {
                Record(ctx, id, NodeStatus::Failure);
                return NodeStatus::Failure;
            }

            const NodeStatus s = children[0]->Tick(ctx);
            if (s == NodeStatus::Success)
                readyAt = ctx.elapsed + seconds;
            Record(ctx, id, s);
            return s;
        }

        // No instance: passthrough.
        const NodeStatus s = children[0]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    bool BlackboardCondition::Evaluate(const BlackboardComponent& bb) const
    {
        const auto it = bb.values.find(key);
        if (it == bb.values.end()) return op == BBCmpOp::NotEquals; // a missing key is not equal to anything

        if (op == BBCmpOp::IsSet) return true;

        if (!expected.has_value()) return false;

        // Variant equality by hand: XMFLOAT3 has no operator==, so std::variant's does not compile. Same
        // types compare component-wise (vectors) or with == (the rest); different types are never equal.
        const bool eq = std::visit([](const auto& a, const auto& b) -> bool {
            using A = std::decay_t<decltype(a)>;
            using B = std::decay_t<decltype(b)>;
            if constexpr (!std::is_same_v<A, B>) return false;
            else if constexpr (std::is_same_v<A, DirectX::XMFLOAT3>)
                return a.x == b.x && a.y == b.y && a.z == b.z;
            else if constexpr (std::is_same_v<A, std::vector<DirectX::XMFLOAT3>>)
            {
                // std::vector<XMFLOAT3> has no operator== (XMFLOAT3 lacks one).
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z)
                        return false;
                return true;
            }
            else
                return a == b;
        }, it->second, *expected);
        return op == BBCmpOp::Equals ? eq : !eq;
    }

    NodeStatus BlackboardCondition::Tick(BTContext& ctx)
    {
        BlackboardComponent* bb = ctx.Blackboard();
        if (!bb)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        if (!Evaluate(*bb))
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        if (children.empty())
        {
            Record(ctx, id, NodeStatus::Success);
            return NodeStatus::Success;
        }

        const NodeStatus s = children[0]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    NodeStatus ActionLeaf::Tick(BTContext& ctx)
    {
        NodeStatus s = NodeStatus::Failure;
        if (ctx.actions)
            s = ctx.actions->DispatchAction(actionName, ctx, params);
        Record(ctx, id, s);
        if (s == NodeStatus::Running && ctx.instance)
            ctx.instance->runningNode = id;
        return s;
    }

    NodeStatus ConditionLeaf::Tick(BTContext& ctx)
    {
        NodeStatus s = NodeStatus::Failure;
        if (ctx.actions)
            s = ctx.actions->DispatchCondition(conditionName, ctx, params);

        if (s == NodeStatus::Running)
        {
            // Conditions must be pure: coerce Running to Failure. This warning is logged on every occurrence.
            LOG_WARNING("BT condition '%s' returned Running — coerced to Failure",
                        conditionName.c_str());
            s = NodeStatus::Failure;
        }
        Record(ctx, id, s);
        return s;
    }

    NodeStatus UtilitySelector::Tick(BTContext& ctx)
    {
        if (children.empty())
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        int   bestIdx   = -1;
        float bestScore = -1.f;
        for (size_t i = 0; i < children.size(); ++i)
        {
            float score = 0.f;
            if (i < childScores.size() && childScores[i])
                score = childScores[i](ctx);
            if (std::isnan(score) || score < 0.f) score = 0.f;
            if (score > bestScore)
            {
                bestScore = score;
                bestIdx   = static_cast<int>(i);
            }
        }
        if (bestIdx < 0 || bestScore <= 0.f)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }

        const NodeStatus s = children[bestIdx]->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }

    NodeStatus SubTree::Tick(BTContext& ctx)
    {
        if (!tree || !tree->root)
        {
            Record(ctx, id, NodeStatus::Failure);
            return NodeStatus::Failure;
        }
        const NodeStatus s = tree->root->Tick(ctx);
        Record(ctx, id, s);
        return s;
    }
}

namespace AI
{
    // Out of line so BTNode.h needs no World include; see the declaration for why it is never cached.
    BlackboardComponent* BTContext::Blackboard() const
    {
        if (!world || entity == NullEntity) return nullptr;
        return world->GetComponent<BlackboardComponent>(entity);
    }
}
