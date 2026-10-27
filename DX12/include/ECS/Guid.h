#pragma once

// Guid: 128-bit identifier, the stable cross-session identity of entities that must survive a
// scene reload or a save game (docs/design/entity_persistence_architecture.md).
//
// 128-bit because the birthday-collision chance stays negligible (1 in 2^64 after ~2.7x10^9 ids),
// so nothing ever deduplicates, and the UUID v4 wire format lets every external tool read it.
// Two uint64_t: trivially copyable, no allocation, two compares; std::hash specialization at the
// bottom.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace ECS
{

struct Guid
{
    // hi/lo hold the 16 bytes big-endian (hi = bytes 0-7); ToString prints the canonical
    // 8-4-4-4-12 UUID layout so it round-trips with external GUID tools. Otherwise hi/lo are opaque
    // random bits.
    uint64_t hi = 0u;
    uint64_t lo = 0u;

    constexpr bool IsValid() const noexcept { return (hi | lo) != 0u; }

    constexpr bool operator==(const Guid& o) const noexcept
    { return hi == o.hi && lo == o.lo; }
    constexpr bool operator!=(const Guid& o) const noexcept
    { return !(*this == o); }
    // Ordering only for std::map convenience; semantically Guids are not
    // ordered. Compare lexicographically by (hi,lo).
    constexpr bool operator<(const Guid& o) const noexcept
    { return hi != o.hi ? hi < o.hi : lo < o.lo; }

    // Generate a new random Guid with the UUID v4 version/variant nibbles set, so it round-trips
    // through RFC 4122 tools. Thread-safe. Inside a GuidMintScope it derives from the scope's seed.
    static Guid Generate();

    // Deterministic remap: Derive(base, seed) is pure, so the same pair always yields the same Guid
    // and different seeds yield unrelated ones. LoadPrefab uses it to give each prefab instance its
    // own copy of the identities baked into the .ipfb: a random Guid per instantiation would change
    // on every load and dangle every reference to a prefab member. Seeding from the instance root's
    // Guid makes the whole instance reproducible.
    static Guid Derive(const Guid& base, const Guid& seed);

    // Canonical 36-char UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
    std::string ToString() const;

    // Parse a 36-char UUID string. Returns invalid Guid (hi=lo=0) on bad input.
    // Accepts upper or lower hex; dashes optional.
    static Guid Parse(const char* s);
    static Guid Parse(const std::string& s) { return Parse(s.c_str()); }

    // Compact 32-char hex (no dashes), for the .iscn key=value text format.
    std::string ToHex() const;
    static Guid FromHex(const char* s);
    static Guid FromHex(const std::string& s) { return FromHex(s.c_str()); }
};

inline constexpr Guid kInvalidGuid{ 0u, 0u };

// While one is open on this thread, Guid::Generate() derives from the scope's seed
// (Derive({salt, n}, seed), n counting up from 1) instead of drawing from the RNG. An async
// spawn's commit runs inside one with its request's seed, so every identity the instantiation
// mints (a follow target's, an exposed entity var's) depends on the request alone, never on the
// frame that commits it. Nestable: the innermost answers.
class GuidMintScope
{
public:
    explicit GuidMintScope(const Guid& seed);
    ~GuidMintScope();
    GuidMintScope(const GuidMintScope&)            = delete;
    GuidMintScope& operator=(const GuidMintScope&) = delete;

    uint64_t Derived() const { return m_count; }   // Generate() calls it answered

private:
    friend struct Guid;
    Guid           m_seed;
    uint64_t       m_count = 0;
    GuidMintScope* m_prev  = nullptr;
};

namespace GuidStats
{
    // Generate() calls that reached the RNG while an async spawn's commit window
    // was open, on any thread (Profiler.Spawn().async.guidsMintedInCommit): 0 by
    // construction - the commit mints inside a GuidMintScope - so a non-zero
    // count is a mint the scope did not see (another thread, a path outside it).
    uint64_t MintedInCommit();
    // Main: the async commit's window (nestable counter).
    void BeginCommitWindow();
    void EndCommitWindow();
}

} // namespace ECS

// std::hash specialization so Guid is a valid unordered_map key without the
// caller writing a hasher.
namespace std
{
    template<> struct hash<ECS::Guid>
    {
        size_t operator()(const ECS::Guid& g) const noexcept
        {
            // hash_combine-style mix: both halves are already random, so no avalanche is needed.
            uint64_t h = g.hi ^ (g.lo + 0x9e3779b97f4a7c15ull
                                + (g.hi << 6) + (g.hi >> 2));
            return static_cast<size_t>(h);
        }
    };
}
