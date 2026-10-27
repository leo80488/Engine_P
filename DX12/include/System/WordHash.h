#pragma once

// WordHash - change-detection hash over a byte range, 32 bytes per step.
//
// Four interleaved 64-bit FNV-style lanes folded at the end (the mixing of DDGISceneAS's
// TLAS instance fingerprint): a single byte-at-a-time accumulator is bound by one serial
// xor->imul chain. Only for "did these bytes change since last frame": the value is not a
// serialization or cross-build contract; compare it only against an earlier value from the
// same function in the same process.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Sys
{
inline uint64_t WordHash64(const void* data, size_t bytes,
                           uint64_t seed = 1469598103934665603ull)
{
    constexpr uint64_t kPrime = 1099511628211ull;
    constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h0 = seed ^ static_cast<uint64_t>(bytes), h1 = kGolden,
             h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32)
    {
        uint64_t w[4];
        std::memcpy(w, p + i, sizeof(w));
        h0 ^= w[0]; h0 *= kPrime;
        h1 ^= w[1]; h1 *= kPrime;
        h2 ^= w[2]; h2 *= kPrime;
        h3 ^= w[3]; h3 *= kPrime;
    }
    uint64_t h = h0 ^ (h1 + kGolden + (h0 << 6) + (h0 >> 2));
    h ^= h2 + kGolden + (h << 6) + (h >> 2);
    h ^= h3 + kGolden + (h << 6) + (h >> 2);
    for (; i + 8 <= bytes; i += 8)
    {
        uint64_t w;
        std::memcpy(&w, p + i, sizeof(w));
        h ^= w; h *= kPrime;
    }
    for (; i < bytes; ++i) { h ^= p[i]; h *= kPrime; }
    return h;
}

// Fold one more value into a running WordHash64 result (order-sensitive).
inline uint64_t WordHashCombine(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
} // namespace Sys
