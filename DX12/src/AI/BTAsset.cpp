#include "AI/BTAsset.h"

#include <vector>

namespace AI
{
    void BTAsset::AssignIds()
    {
        nodeCount = 0;
        if (!root) return;

        // Children pushed in reverse, so ids come out in pre-order: id order is the order written in the
        // .bt.lua. The trace panel sorts the (post-order) trace by id to show the authored tree, and indents
        // by depth.
        struct Item { BTNode* n; uint16_t depth; };
        std::vector<Item> stack;
        stack.push_back({ root.get(), 0 });

        while (!stack.empty())
        {
            const Item it0 = stack.back();
            stack.pop_back();
            BTNode* n = it0.n;
            n->id    = ++nodeCount;
            n->depth = it0.depth;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back({ it->get(), static_cast<uint16_t>(it0.depth + 1) });
        }
    }

    const BTNode* BTAsset::FindNode(NodeId id) const
    {
        if (!root || id == kInvalidNodeId) return nullptr;

        std::vector<const BTNode*> stack;
        stack.push_back(root.get());
        while (!stack.empty())
        {
            const BTNode* n = stack.back();
            stack.pop_back();
            if (n->id == id) return n;
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
                stack.push_back(it->get());
        }
        return nullptr;
    }
}
