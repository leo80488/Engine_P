// GuidRegistry.cpp: Guid generation / formatting / parsing, the global Guid <-> Entity map used
// by AttachmentRef, and AttachmentRef's out-of-line members. See Guid.h, GuidComponent.h,
// GuidRegistry.h and docs/design/entity_persistence_architecture.md.

#include "Harness/Determinism.h"
#include "ECS/GuidRegistry.h"
#include "ECS/GuidComponent.h"
#include "ECS/ECS.h"
#include "System/Log.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>

// ==== Guid: generate / format / parse ====
// Generate uses a thread-local mt19937_64 seeded once from random_device and the clock (from the
// -seed value when one is given). UUID v4 sets the version (4) and variant (10xx) nibbles so the
// bytes match RFC 4122 for external tools (asset DBs, profilers, save-game inspectors).

namespace ECS
{
    namespace
    {
        // One mt19937_64 per thread avoids lock contention on Generate. Each thread mixes its arrival
        // index into the seed, so two threads booting in lockstep never share a stream.
        thread_local std::mt19937_64* t_rng = nullptr;

        std::mt19937_64& Rng()
        {
            if (t_rng) return *t_rng;
            static std::atomic<uint64_t> s_counter{ 0 };

            // Arrival counter: which thread gets which index depends on the order threads first mint a Guid,
            // which is nondeterministic. Under -seed, minting is therefore reproducible only when that order
            // is (-workers=1, the golden-pack configuration); a seed alone does not make multi-threaded
            // minting reproducible.
            const uint64_t index = s_counter.fetch_add(1u, std::memory_order_relaxed);

            uint64_t seed;
            const auto& det = Harness::Determinism::Get();
            if (det.HasSeed())
            {
                // Deterministic mix of the user seed and the thread index: no random_device, no clock (those make
                // two runs disagree). splitmix64-style avalanche so adjacent indices do not give correlated
                // streams.
                seed = det.Seed() + 0x9E3779B97F4A7C15ull * (index + 1u);
                seed ^= seed >> 30; seed *= 0xBF58476D1CE4E5B9ull;
                seed ^= seed >> 27; seed *= 0x94D049BB133111EBull;
                seed ^= seed >> 31;
            }
            else
            {
                seed = std::random_device{}()
                    ^ static_cast<uint64_t>(
                        std::chrono::steady_clock::now().time_since_epoch().count())
                    ^ (index << 32);
            }
            t_rng = new std::mt19937_64(seed);
            return *t_rng;
        }
    }

    namespace
    {
        thread_local GuidMintScope* t_mintScope = nullptr;
        std::atomic<int>            g_commitWindow{ 0 };
        std::atomic<uint64_t>       g_mintedInCommit{ 0 };
    }

    GuidMintScope::GuidMintScope(const Guid& seed) : m_seed(seed), m_prev(t_mintScope) { t_mintScope = this; }
    GuidMintScope::~GuidMintScope() { t_mintScope = m_prev; }

    namespace GuidStats
    {
        uint64_t MintedInCommit()     { return g_mintedInCommit.load(std::memory_order_relaxed); }
        void     BeginCommitWindow()  { g_commitWindow.fetch_add(1, std::memory_order_relaxed); }
        void     EndCommitWindow()    { g_commitWindow.fetch_sub(1, std::memory_order_relaxed); }
    }

    Guid Guid::Generate()
    {
        // Inside a GuidMintScope the identity is the seed's, not the RNG's.
        if (GuidMintScope* scope = t_mintScope)
            return Derive(Guid{ 0x6D696E7453636F70ull, ++scope->m_count }, scope->m_seed);
        if (g_commitWindow.load(std::memory_order_relaxed) > 0)
            g_mintedInCommit.fetch_add(1, std::memory_order_relaxed);
        auto& rng = Rng();
        Guid g;
        g.hi = rng();
        g.lo = rng();
        // RFC 4122 v4: top 4 bits of byte 6 = 0100; top 2 bits of byte 8 = 10.
        // hi/lo are big-endian conceptually, byte 6 sits in hi at bits 8-15
        // from the low end, byte 8 in lo at bits 56-63.
        g.hi = (g.hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        g.lo = (g.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        // Avoid (1-in-2^128) all-zero output collapsing to invalid sentinel.
        if (!g.IsValid()) g.lo = 1u;
        return g;
    }

    namespace
    {
        // SplitMix64: the finalizer of Vigna's splitmix64. One round per half is enough: this needs
        // decorrelation, not crypto.
        constexpr uint64_t SplitMix64(uint64_t x) noexcept
        {
            x += 0x9E3779B97F4A7C15ull;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
            x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
            return x ^ (x >> 31);
        }
    }

    Guid Guid::Derive(const Guid& base, const Guid& seed)
    {
        // Independent chains for hi/lo, then a cross-feed so two bases that
        // differ only in `lo` still land on different `hi` halves (baked prefab
        // Guids are v4, so their fixed version/variant nibbles must not make
        // whole regions of the output space unreachable).
        uint64_t h = SplitMix64(base.hi ^ SplitMix64(seed.hi));
        uint64_t l = SplitMix64(base.lo ^ SplitMix64(seed.lo ^ 0xA5A5A5A5A5A5A5A5ull));
        h ^= SplitMix64(l);
        l ^= SplitMix64(h);

        Guid g{ h, l };
        // Same RFC 4122 v4 stamping as Generate(): a derived Guid must be indistinguishable from a
        // generated one downstream.
        g.hi = (g.hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
        g.lo = (g.lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
        if (!g.IsValid()) g.lo = 1u;
        return g;
    }

    std::string Guid::ToString() const
    {
        char buf[37];
        // Canonical UUID layout: 8-4-4-4-12. Decompose hi/lo into the 16
        // bytes in big-endian order so the printed string matches what most
        // tools expect.
        const uint32_t a = static_cast<uint32_t>(hi >> 32);
        const uint16_t b = static_cast<uint16_t>(hi >> 16);
        const uint16_t c = static_cast<uint16_t>(hi);
        const uint16_t d = static_cast<uint16_t>(lo >> 48);
        const uint64_t e = lo & 0x0000FFFFFFFFFFFFull;
        std::snprintf(buf, sizeof(buf),
            "%08x-%04x-%04x-%04x-%012llx",
            a, b, c, d, static_cast<unsigned long long>(e));
        return std::string(buf);
    }

    std::string Guid::ToHex() const
    {
        char buf[33];
        std::snprintf(buf, sizeof(buf),
            "%016llx%016llx",
            static_cast<unsigned long long>(hi),
            static_cast<unsigned long long>(lo));
        return std::string(buf);
    }

    namespace
    {
        // Returns -1 if c is not a hex digit.
        int HexNibble(char c) noexcept
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        }

        // Reads up to 32 hex chars from `s`, skipping dashes, returns true
        // on success. Fills hi (first 16 chars) and lo (next 16 chars).
        bool ParseHex(const char* s, uint64_t& hi, uint64_t& lo) noexcept
        {
            hi = 0u;
            lo = 0u;
            int consumed = 0;
            for (const char* p = s; *p && consumed < 32; ++p)
            {
                if (*p == '-') continue;
                const int n = HexNibble(*p);
                if (n < 0) return false;
                if (consumed < 16) hi = (hi << 4) | static_cast<uint64_t>(n);
                else               lo = (lo << 4) | static_cast<uint64_t>(n);
                ++consumed;
            }
            return consumed == 32;
        }
    }

    Guid Guid::Parse(const char* s)
    {
        if (!s) return kInvalidGuid;
        Guid g;
        if (!ParseHex(s, g.hi, g.lo)) return kInvalidGuid;
        return g;
    }

    Guid Guid::FromHex(const char* s)
    {
        if (!s) return kInvalidGuid;
        Guid g;
        if (!ParseHex(s, g.hi, g.lo)) return kInvalidGuid;
        return g;
    }

} // namespace ECS

// ==== GuidRegistry ====

namespace ECS
{
    GuidRegistry& GuidRegistry::Get()
    {
        static GuidRegistry s_instance;
        return s_instance;
    }

    void GuidRegistry::Register(Guid guid, Entity e)
    {
        if (!guid.IsValid() || e == NullEntity) return;
        // Unregister any prior binding so the maps don't get out of sync.
        if (auto it = m_guidToEntity.find(guid); it != m_guidToEntity.end())
        {
            if (it->second == e) return;  // already registered
            m_entityToGuid.erase(it->second);
        }
        if (auto it = m_entityToGuid.find(e); it != m_entityToGuid.end())
        {
            m_guidToEntity.erase(it->second);
        }
        m_guidToEntity[guid] = e;
        m_entityToGuid[e]    = guid;
    }

    void GuidRegistry::Unregister(Guid guid)
    {
        auto it = m_guidToEntity.find(guid);
        if (it == m_guidToEntity.end()) return;
        m_entityToGuid.erase(it->second);
        m_guidToEntity.erase(it);
    }

    void GuidRegistry::UnregisterEntity(Entity e)
    {
        auto it = m_entityToGuid.find(e);
        if (it == m_entityToGuid.end()) return;
        m_guidToEntity.erase(it->second);
        m_entityToGuid.erase(it);
    }

    Entity GuidRegistry::Find(Guid guid) const noexcept
    {
        if (!guid.IsValid()) return NullEntity;
        auto it = m_guidToEntity.find(guid);
        return it != m_guidToEntity.end() ? it->second : NullEntity;
    }

    Guid GuidRegistry::ReverseFind(Entity e) const noexcept
    {
        if (e == NullEntity) return kInvalidGuid;
        auto it = m_entityToGuid.find(e);
        return it != m_entityToGuid.end() ? it->second : kInvalidGuid;
    }

    void GuidRegistry::Clear() noexcept
    {
        m_guidToEntity.clear();
        m_entityToGuid.clear();
    }

    void GuidRegistry::RegisterWithWorld(World& world)
    {
        if (m_hookedWorld == &world) return;
        if (m_hookedWorld) DetachFromWorld(*m_hookedWorld);

        m_hookedWorld = &world;
        m_listenerHandle = world.AddEntityDestroyListener(
            [this](Entity e) { UnregisterEntity(e); });
    }

    void GuidRegistry::DetachFromWorld(World& world)
    {
        if (m_hookedWorld != &world) return;
        if (m_listenerHandle != 0u)
            world.RemoveEntityDestroyListener(m_listenerHandle);
        m_listenerHandle = 0u;
        m_hookedWorld    = nullptr;
    }

    void GuidRegistry::RebuildFromWorld(World& world)
    {
        Clear();
        world.ForEach<GuidComponent>([&](Entity e, GuidComponent& gc)
        {
            if (!gc.guid.IsValid())
                gc.guid = Guid::Generate();  // shouldn't happen post-load but heal silently
            Register(gc.guid, e);
        });
    }

    Guid EnsureGuidOn(World& world, Entity e)
    {
        if (!world.IsAlive(e)) return kInvalidGuid;
        auto* gc = world.GetComponent<GuidComponent>(e);
        if (!gc)
        {
            GuidComponent newGc{};
            newGc.guid = Guid::Generate();
            world.AddComponent<GuidComponent>(e, newGc);
            GuidRegistry::Get().Register(newGc.guid, e);
            return newGc.guid;
        }
        if (!gc->guid.IsValid())
        {
            gc->guid = Guid::Generate();
        }
        // Make sure the registry knows even if the component pre-existed
        // without a registry entry (e.g. loaded from disk before
        // RebuildFromWorld ran).
        GuidRegistry::Get().Register(gc->guid, e);
        return gc->guid;
    }

} // namespace ECS

// ==== AttachmentRef::Resolve / Bind / BindAndStamp ====
// Out of line here so GuidComponent.h needs neither GuidRegistry.h nor the full World type.

Entity AttachmentRef::Resolve(World& world) const
{
    if (!ownerGuid.IsValid()) return NullEntity;

    // Steady-state fast path: cached entity is still alive AND its generation
    // matches what we cached. One branch + one array load.
    if (cachedOwner != NullEntity
        && world.IsAlive(cachedOwner)
        && world.GetGeneration(cachedOwner) == cachedGeneration)
    {
        return cachedOwner;
    }

    // Slow path: registry lookup. Update cache for the next call.
    const Entity e = ECS::GuidRegistry::Get().Find(ownerGuid);
    if (e == NullEntity)
    {
        cachedOwner      = NullEntity;
        cachedGeneration = 0u;
        return NullEntity;
    }
    cachedOwner      = e;
    cachedGeneration = world.GetGeneration(e);
    return e;
}

void AttachmentRef::Bind(World& world, Entity target)
{
    if (target == NullEntity)
    {
        Clear();
        return;
    }
    auto* gc = world.GetComponent<GuidComponent>(target);
    if (!gc)
    {
        // The caller did not stamp the target: leave the ref empty (BindAndStamp is the auto-stamping
        // variant).
        Clear();
        return;
    }
    ownerGuid        = gc->guid;
    cachedOwner      = target;
    cachedGeneration = world.GetGeneration(target);
}

ECS::Guid AttachmentRef::BindAndStamp(World& world, Entity target)
{
    if (target == NullEntity)
    {
        Clear();
        return ECS::kInvalidGuid;
    }
    const ECS::Guid g = ECS::EnsureGuidOn(world, target);
    ownerGuid        = g;
    cachedOwner      = target;
    cachedGeneration = world.GetGeneration(target);
    return g;
}
