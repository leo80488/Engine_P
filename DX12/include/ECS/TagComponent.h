#pragma once

// TagComponent: lightweight string tags for gameplay filtering.
//
// POD and fixed-size so it fits a ComponentPool. MAX tags per entity covers weapon / enchant / team
// markers; raise MAX if a real use needs more. Tags are null-terminated and compared by strcmp - no
// interning or hashing, O(N) Has() with a tiny N.
//
// Typical Lua usage:
//   Engine.AddTag(weaponId, "FireEnchanted")
//   if Engine.HasTag(weaponId, "FireEnchanted") then ... end
//   Engine.RemoveTag(weaponId, "FireEnchanted")

#include "ECS/ECS.h"

#include <cstdint>
#include <cstring>

struct TagComponent
{
    static constexpr uint32_t MAX = 8;
    static constexpr std::size_t LEN = 32;      // per-tag capacity incl. null

    char     tags[MAX][LEN] = {};
    uint32_t count = 0;

    bool Has(const char* tag) const
    {
        if (!tag) return false;
        for (uint32_t i = 0; i < count; ++i)
            if (std::strcmp(tags[i], tag) == 0) return true;
        return false;
    }

    // Returns true if the tag was added (or was already present). Returns
    // false only when MAX is reached and the tag wasn't already there.
    bool Add(const char* tag)
    {
        if (!tag || !*tag) return false;
        if (Has(tag))      return true;
        if (count >= MAX)  return false;

        const std::size_t len = std::strlen(tag);
        const std::size_t copy = (len < LEN - 1) ? len : (LEN - 1);
        std::memcpy(tags[count], tag, copy);
        tags[count][copy] = '\0';
        ++count;
        return true;
    }

    bool Remove(const char* tag)
    {
        if (!tag) return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (std::strcmp(tags[i], tag) == 0)
            {
                // Shift remaining entries down to keep the array contiguous.
                for (uint32_t j = i; j + 1 < count; ++j)
                    std::memcpy(tags[j], tags[j + 1], LEN);
                --count;
                tags[count][0] = '\0';
                return true;
            }
        }
        return false;
    }
};
