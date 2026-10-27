#pragma once

// BTAsset - shared, parsed behaviour tree: one tree, many entities holding shared_ptr<BTAsset>
// (like SkeletonAsset / MeshLibRef). Per-entity execution state lives in BTInstance (BTNode.h).

#include "AI/BTNode.h"
#include <memory>
#include <string>

namespace AI
{
    class BTAsset
    {
    public:
        // Assigns every node a unique NodeId from 1 in pre-order, plus its depth. Call once after parse /
        // construction, and again after any change to the tree (only the loader changes one).
        void AssignIds();

        // Linear lookup by id, for the editor's trace panel (runs at ImGui rate).
        const BTNode* FindNode(NodeId id) const;

        std::unique_ptr<BTNode> root;

        // Path the asset was parsed from (hot-reload key, editor display); empty for trees built in code.
        std::string sourcePath;

        // Total node count (root + descendants), filled by AssignIds.
        uint32_t nodeCount = 0;
    };
}
