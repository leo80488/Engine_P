#pragma once

// StringHash -- stable, content-authored identifiers shared by engine layers.
//
// std::hash<std::string> is deliberately not used: its value is not a portable
// serialization/debug contract.  FNV-1a keeps IDs stable across processes and
// builds while the owning registry retains the source string for diagnostics
// and collision detection.

#include <cstdint>
#include <functional>
#include <string_view>

struct StringHash
{
    uint64_t value = 0;

    constexpr StringHash() = default;
    explicit constexpr StringHash(std::string_view text)
        : value(Hash(text))
    {}

    constexpr bool IsValid() const noexcept { return value != 0; }

    friend constexpr bool operator==(StringHash a, StringHash b) noexcept
    {
        return a.value == b.value;
    }
    friend constexpr bool operator!=(StringHash a, StringHash b) noexcept
    {
        return !(a == b);
    }

    static constexpr uint64_t Hash(std::string_view text) noexcept
    {
        if (text.empty()) return 0;

        uint64_t h = 0xcbf29ce484222325ull;
        for (const unsigned char c : text)
        {
            h ^= c;
            h *= 0x100000001b3ull;
        }
        // Zero is reserved for "no ID".  FNV reaching zero is exceptionally
        // unlikely, but remapping it makes the invalid-value contract total.
        return h ? h : 1ull;
    }
};

namespace std
{
    template <>
    struct hash<StringHash>
    {
        size_t operator()(StringHash id) const noexcept
        {
            return static_cast<size_t>(id.value);
        }
    };
}

