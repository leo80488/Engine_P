#pragma once

#include "System/Check.h"

#include <cstdint>
// Unified resource handle, bit-packed into 32 bits.
// Layout (MSB -> LSB): [Gen:10][TypeTag:6][Index:16]
//   Index      (bits 15-0):  up to 65536 slots
//   TypeTag    (bits 21-16): up to 64 resource types
//   Generation (bits 31-22): 1..1023 per slot (0 is never assigned)
//
// When a slot is freed its generation is incremented, so every handle issued
// earlier for that slot is stale at once: ResourceManager's generation check
// rejects it (Handle::IsValid only tests for the null handle).
namespace Resource
{
    enum class ResourceType : uint8_t
    {
        Unknown = 0,
        Texture  = 1,
        Mesh     = 2,
        Shader   = 3,
        Material = 4,
        Scene     = 5,
        Skeleton  = 6,
        Animation = 7,
        MeshLibrary = 8, // first-class mesh-pool container
        AudioClip   = 9, // .aclip - PCM/compressed audio sample with metadata
        PostProcessProfile = 10, // .ppprofile - shared post-process look (volume system)
        AnimGraph = 11,
        AnimSet = 12,
        Count
    };

    struct Handle
    {
        static constexpr uint32_t INDEX_BITS = 16;   // max 65536 slots
        static constexpr uint32_t TYPE_BITS = 6;    // max 64 types
        static constexpr uint32_t GEN_BITS = 10;   // max 1023 generations (0 = null)

        static constexpr uint32_t INDEX_MASK = (1u << INDEX_BITS) - 1;
        static constexpr uint32_t TYPE_MASK = (1u << TYPE_BITS) - 1;
        static constexpr uint32_t GEN_MASK = (1u << GEN_BITS) - 1;

        static constexpr uint32_t MAX_INDEX = INDEX_MASK;
        static constexpr uint32_t MAX_GEN = GEN_MASK;        // 1023
        static constexpr uint32_t INVALID = 0;               // generation=0

        uint32_t packed = INVALID;

        // A value that does not fit its field is refused with the null handle,
        // which every lookup rejects (checked in Release too): masking would make an
        // index past INDEX_MASK name ANOTHER slot and a generation past GEN_MASK
        // alias an older one.
        static Handle Make(uint32_t index, ResourceType type, uint32_t generation)
        {
            if (!ENGINE_CHECK_ONCE(index <= INDEX_MASK,
                    "Resource::Handle::Make: slot index %u does not fit the %u-bit index (type %u)",
                    index, INDEX_BITS, static_cast<uint32_t>(type)))
                return Handle{};
            if (!ENGINE_CHECK_ONCE(generation != 0 && generation <= GEN_MASK,  // 0 is reserved for null
                    "Resource::Handle::Make: generation %u is outside 1..%u (slot %u, type %u)",
                    generation, GEN_MASK, index, static_cast<uint32_t>(type)))
                return Handle{};
            Handle h;
            h.packed = ((generation & GEN_MASK) << (INDEX_BITS + TYPE_BITS))
                | ((static_cast<uint32_t>(type) & TYPE_MASK) << INDEX_BITS)
                | (index & INDEX_MASK);
            return h;
        }

        uint32_t     Index()      const { return  packed & INDEX_MASK; }
        ResourceType Type()       const { return  static_cast<ResourceType>((packed >> INDEX_BITS) & TYPE_MASK); }
        uint32_t     Generation() const { return (packed >> (INDEX_BITS + TYPE_BITS)) & GEN_MASK; }

        bool IsValid()            const { return Generation() != 0; }  // 0 = null sentinel
        explicit operator bool()  const { return IsValid(); }
    };

    // Raw handle id (equals Handle::packed) for code that stores raw IDs.
    using HandleId = uint32_t;
    constexpr HandleId kInvalidHandle = Handle::INVALID;

    inline bool operator==(Handle a, Handle b) { return a.packed == b.packed; }
    inline bool operator!=(Handle a, Handle b) { return a.packed != b.packed; }
}
