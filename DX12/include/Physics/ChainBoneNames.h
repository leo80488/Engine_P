#pragma once
// Physics/ChainBoneNames.h - the two NAME rules that decide which bones chain physics touches
// without anyone authoring a group:
//   ClassifyBone        ChainPhysicsSystem's name-based strand builder (a component with no
//                       authored groups simulates every bone this names)
//   TriggersAutoAttach  SkinnedMeshSubsystem::AttachSkeletonForPending adds a ChainPhysicsComponent
//                       to a freshly loaded skeleton when ANY bone name passes it
// Shared so the Skeleton Editor / `-assettool=skeleton_report` (SkeletonInspect) show which bones a
// skeleton hands to chain physics by name with the runtime's own rule. The two lists differ ON
// PURPOSE (auto-attach is the narrower one): do not merge them.

#include <cstring>

namespace ChainPhysicsNames
{
    // 0 = not physics, 1 = hair, 2 = skirt, 3 = spring bone (single jiggle).
    inline int ClassifyBone(const char* name)
    {
        if (!name || !name[0]) return 0;
        const auto has = [name](const char* s) { return std::strstr(name, s) != nullptr; };

        if (has("Hair") || has("hair") ||
            has("PonyTail") || has("ponytail") || has("Ponytail") ||
            has("Tape") ||
            has("髮") || has("馬尾"))
            return 1;

        if (has("Skirt") || has("skirt") ||
            has("Scarf") || has("Sleeve") || has("Coat"))
            return 2;

        if (has("Chest") || has("chest") ||
            has("Butt") || has("butt"))
            return 3;

        return 0;
    }

    // SkinnedMeshSubsystem::AttachSkeletonForPending's test.
    inline bool TriggersAutoAttach(const char* name)
    {
        if (!name || !name[0]) return false;
        return std::strstr(name, "Hair") || std::strstr(name, "hair") ||
               std::strstr(name, "PonyTail") || std::strstr(name, "ponytail") ||
               std::strstr(name, "Skirt") || std::strstr(name, "skirt") ||
               std::strstr(name, "Chest") || std::strstr(name, "chest") ||
               std::strstr(name, "Butt")  || std::strstr(name, "butt");
    }
}
