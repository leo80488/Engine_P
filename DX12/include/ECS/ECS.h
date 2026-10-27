#pragma once

// Simple ECS with sparse-set component storage.
//   - Entity is an opaque uint32_t id (0 = null sentinel).
//   - World owns one ComponentPool<T> per component type, created lazily.
//   - ComponentPool<T> keeps values in a dense std::vector<T> with a direct
//     sparse[Entity] -> dense-index lookup: GetComponent<T>(e) is two array
//     loads, no hash probe, no type-erased boxing. Hot systems call it ~10x per
//     entity per frame, so it must stay that cheap.
//   - ForEach<T>(fn) walks the dense data and visits only entities that have T.

#include "ECS/AccessGuard.h"   // GetPool's DeclareAccess enforcement

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

// Entity: opaque id; NullEntity (0) is the null sentinel.
using Entity = std::uint32_t;

constexpr Entity NullEntity = 0u;

// EntityHandle - Entity + generation for lifetime-safe references.
// Raw Entity ids are recycled by CreateEntity's free list, so a bare Entity field
// can silently start naming a new entity that reuses the slot. A component that
// must survive its target's destruction (Follow targets, event payloads) stores
// an EntityHandle and gates reads on World::IsHandleValid(h). Entity fields that
// are cleared proactively on destroy may stay raw.
struct EntityHandle
{
    Entity   entity     = NullEntity;
    uint32_t generation = 0u;

    bool operator==(const EntityHandle& o) const
    { return entity == o.entity && generation == o.generation; }
    bool operator!=(const EntityHandle& o) const { return !(*this == o); }
};

constexpr EntityHandle NullEntityHandle{ NullEntity, 0u };

// Component base (tag only; the data lives in each component type).
struct ComponentBase {};

// ==== ComponentPool: sparse-set storage for one component type ====
class IComponentPool
{
public:
    virtual ~IComponentPool()                          = default;
    virtual void Erase(Entity e)                        = 0;
    virtual bool Has(Entity e) const                    = 0;
    virtual void Clear()                                = 0;

    // Deep-copy this pool into a fresh pool of the same concrete type (World::CloneState
    // for the Edit/Play snapshot; see docs/design/editor_runtime_mode_architecture.md).
    // Values go through ComponentCloner<T>; GPU handles inside them are shared by
    // refcount, never duplicated.
    virtual std::unique_ptr<IComponentPool> Clone() const = 0;

    // Per-entity value snapshot for editor undo: deep-copy entity e's component into a
    // type-erased holder (null if absent) and restore it onto e later. Uses the same
    // ComponentCloner<T> as Clone(), so move-only components work and GPU handles are
    // shared by refcount, keeping the resource alive while an undo snapshot holds the
    // copy. Works for every component type, including the non-serializable skinned-mesh
    // runtime components (Skeleton/Morph/Animation/MeshLibRef).
    virtual std::shared_ptr<void> CloneComponent(Entity e) const = 0;
    virtual void RestoreComponent(Entity e, const std::shared_ptr<void>& holder) = 0;

    // The dense member list (read by World::StructuralHash).
    virtual const std::vector<Entity>& MemberEntities() const = 0;
};

// ==== ComponentCloner ====
// Customization point for copying one component value when a World is deep-copied.
// The default value-copies (handle/ComPtr/shared_ptr members shared by refcount,
// POD/CPU data duplicated). A MOVE-ONLY component (a unique_ptr to per-instance
// runtime backing, e.g. AIComponent's BTInstance) MUST specialize it: copy the
// authored fields and leave the runtime backing reset (rebuilt when Play starts).
// The specialization MUST live in the header that defines the component (ODR: it
// has to be visible wherever the pool is instantiated).
template <typename T>
struct ComponentCloner
{
    // Marks the DEFAULT cloner as a plain copy (nothing scrubbed), which lets
    // ComponentPool<T>::Clone bulk-copy the dense array instead of cloning per element.
    // Opt-in by omission is the safety mechanism: only the primary template declares
    // it, so every scrubbing specialization falls back to the per-element path.
    // Never add kPlainCopy to a specialization: it would carry live BTInstance
    // pointers, audio voice handles and UI hover state back into the editor world.
    static constexpr bool kPlainCopy = true;

    static T Clone(const T& src)
    {
        if constexpr (std::is_copy_constructible_v<T>)
            return src;
        else
            static_assert(sizeof(T) == 0,
                "Move-only component needs a ComponentCloner<T> specialization "
                "(copy authored fields, reset the unique_ptr runtime backing). "
                "Define it in the same header as the component type.");
    }
};

// One process-wide counter behind every pool's StructureVersion(), so a
// (pool, version) pair can never repeat - not even for a new pool allocated at
// a freed one's address.
inline uint64_t NextPoolStructureVersion()
{
    static std::atomic<uint64_t> s_next{ 0 };
    return s_next.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename T>
class ComponentPool : public IComponentPool
{
public:
    // Add or overwrite the value for entity e. Returns the (new or existing) slot.
    T* Add(Entity e, T value)
    {
        EnsureSparse(e);
        m_structVersion = NextPoolStructureVersion();
        const int32_t existing = m_sparse[e];
        if (existing >= 0)
        {
            m_data[existing] = std::move(value);
            return &m_data[existing];
        }
        const int32_t idx = static_cast<int32_t>(m_data.size());
        m_sparse[e] = idx;
        m_dense.push_back(e);
        m_data.push_back(std::move(value));
        return &m_data.back();
    }

    // Changes whenever an element may have MOVED, a member was added or removed, or a
    // value was REPLACED through Add - i.e. whenever a pointer taken from Get() earlier
    // may dangle, name another entity's value, or be missing / newly present. In-place
    // edits through Get() / Data() do not change it (the owner's dirty flags cover
    // those). The renderer's retained candidate cache uses it to skip re-resolving
    // thousands of MaterialComponent pointers per frame.
    uint64_t StructureVersion() const { return m_structVersion; }

    T* Get(Entity e)
    {
        if (e >= m_sparse.size()) return nullptr;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return nullptr;
        return &m_data[idx];
    }

    const T* Get(Entity e) const
    {
        if (e >= m_sparse.size()) return nullptr;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return nullptr;
        return &m_data[idx];
    }

    void Erase(Entity e) override
    {
        if (e >= m_sparse.size()) return;
        const int32_t idx = m_sparse[e];
        if (idx < 0) return;
        m_structVersion = NextPoolStructureVersion();

        const int32_t lastIdx = static_cast<int32_t>(m_data.size()) - 1;
        // Sparse/dense agreement is THE pool invariant: a sparse slot that does not point
        // back at its dense index means an earlier mutation corrupted the mapping and this
        // erase would move the WRONG entity's data. Report it here, not frames later.
        ENGINE_CHECK_ONCE(idx <= lastIdx && m_dense[idx] == e,
                          "ComponentPool::Erase — sparse/dense mismatch for "
                          "entity %u (idx %d, dense says %u)",
                          static_cast<uint32_t>(e), idx,
                          idx <= lastIdx ? static_cast<uint32_t>(m_dense[idx]) : 0u);
        if (idx != lastIdx)
        {
            // Swap-with-back keeps dense contiguous; patch the sparse slot of the moved entity.
            m_data[idx]  = std::move(m_data[lastIdx]);
            m_dense[idx] = m_dense[lastIdx];
            m_sparse[m_dense[idx]] = idx;
        }
        m_data.pop_back();
        m_dense.pop_back();
        m_sparse[e] = -1;
    }

    bool Has(Entity e) const override
    {
        return e < m_sparse.size() && m_sparse[e] >= 0;
    }

    void Clear() override
    {
        m_structVersion = NextPoolStructureVersion();
        m_sparse.clear();
        m_dense.clear();
        m_data.clear();
    }

    std::unique_ptr<IComponentPool> Clone() const override
    {
        auto p = std::make_unique<ComponentPool<T>>();
        p->m_sparse = m_sparse;   // Entity -> dense index (POD)
        p->m_dense  = m_dense;    // dense index -> owning Entity (POD)
        // Detected, not assumed: only the DEFAULT ComponentCloner declares kPlainCopy, so
        // a scrubbing specialization takes the per-element path below. Do not replace this
        // with is_copy_constructible_v: specializations are copy-constructible too and
        // would wrongly take the bulk path.
        if constexpr (requires { ComponentCloner<T>::kPlainCopy; })
        {
            p->m_data = m_data;   // one vector copy, no per-element move
        }
        else
        {
            p->m_data.reserve(m_data.size());
            for (const T& v : m_data)
                p->m_data.push_back(ComponentCloner<T>::Clone(v));
        }
        return p;
    }

    std::shared_ptr<void> CloneComponent(Entity e) const override
    {
        const T* c = Get(e);
        if (!c) return nullptr;
        return std::make_shared<T>(ComponentCloner<T>::Clone(*c));
    }

    void RestoreComponent(Entity e, const std::shared_ptr<void>& holder) override
    {
        if (holder)
            Add(e, ComponentCloner<T>::Clone(*static_cast<const T*>(holder.get())));
    }

    // Iteration helpers over the dense vectors; m_dense[i] owns m_data[i].
    size_t                       Size()     const { return m_data.size(); }
    const std::vector<Entity>&   Entities() const { return m_dense; }
    const std::vector<Entity>&   MemberEntities() const override { return m_dense; }
    std::vector<T>&              Data()           { return m_data; }
    const std::vector<T>&        Data()     const { return m_data; }

private:
    void EnsureSparse(Entity e)
    {
        if (e >= m_sparse.size())
            m_sparse.resize(static_cast<size_t>(e) + 1, -1);
    }

    std::vector<int32_t> m_sparse; // Entity -> index in m_data (or -1)
    std::vector<Entity>  m_dense;  // m_dense[i] owns m_data[i]
    std::vector<T>       m_data;
    uint64_t             m_structVersion = NextPoolStructureVersion();
};

// ==== EntitiesOf: the pool's dense entity array, or empty ====
// Use this instead of `p ? p->Entities() : std::vector<Entity>{}`. That idiom does
// not bind a reference to the pool's array: [expr.cond] needs a common type, so
// both arms become PRVALUEs and `const auto&` binds a temporary full copy of the
// dense array (malloc + memcpy + free on every call, in hot per-frame loops).
template <typename T>
inline const std::vector<Entity>& EntitiesOf(const ComponentPool<T>* p)
{
    static const std::vector<Entity> kEmpty;
    return p ? p->Entities() : kEmpty;
}

// ==== World: entities, names and sparse-set component storage ====
class World
{
public:
    // Entity-destroy listener callback and handle (see AddEntityDestroyListener).
    // Handle 0 is never issued, so 0 means "no listener".
    using EntityDestroyFn = std::function<void(Entity)>;
    using EntityDestroyListenerHandle = uint32_t;

    World() = default;

    // ---- Entity lifecycle ----
    Entity CreateEntity()
    {
        ECS_SPLITCHECK_STRUCTURAL("CreateEntity");
        ECS_ACCESSGUARD_STRUCTURAL("CreateEntity");
        Entity e;
        if (m_freeList.empty())
        {
            e = m_nextEntity++;
        }
        else
        {
            e = m_freeList.back();
            m_freeList.pop_back();
        }
        m_entityIndex[e] = m_entities.size();
        m_entities.push_back(e);
        if (e >= m_alive.size()) m_alive.resize(static_cast<size_t>(e) + 1, false);
        if (e >= m_generations.size()) m_generations.resize(static_cast<size_t>(e) + 1, 0u);
        m_alive[e] = true;
        // Generation starts at 1 on first creation and bumps on every destroy, so a stale
        // EntityHandle never matches a recycled slot.
        if (m_generations[e] == 0u) m_generations[e] = 1u;
        return e;
    }

    // O(1): swap-with-back removal from m_entities via m_entityIndex (a linear find +
    // erase stalls BuildRenderScene when play spawns/destroys projectiles on a large
    // scene). This reorders m_entities; no caller relies on its order.
    void DestroyEntity(Entity e)
    {
        ECS_SPLITCHECK_STRUCTURAL("DestroyEntity");
        ECS_ACCESSGUARD_STRUCTURAL("DestroyEntity");
        // Fire destroy callbacks BEFORE tearing the entity down, so listeners can still
        // read its components (e.g. MaterialComponent -> release texture handles by path).
        // Iterate a copy of the listener list: a callback may add or remove listeners.
        if (!m_destroyListeners.empty())
        {
            const auto snapshot = m_destroyListeners;
            for (const auto& L : snapshot) L.fn(e);
        }

        for (auto& [typeIdx, pool] : m_pools)
            pool->Erase(e);
        m_names.erase(e);

        auto idxIt = m_entityIndex.find(e);
        if (idxIt != m_entityIndex.end())
        {
            const size_t idx  = idxIt->second;
            const size_t last = m_entities.size() - 1;
            if (idx != last)
            {
                const Entity moved = m_entities[last];
                m_entities[idx]     = moved;
                m_entityIndex[moved] = idx;
            }
            m_entities.pop_back();
            m_entityIndex.erase(idxIt);
        }
        // Quarantined: with deferred render-side destroys the id must not be reissued
        // before ReleaseQuarantinedIds - a new entity inheriting it would have its
        // renderer state freed by the destroy record still pending for the old one.
        if (m_quarantineIds) m_quarantine.push_back(e);
        else                 m_freeList.push_back(e);
        if (e < m_alive.size()) m_alive[e] = false;
        if (e < m_generations.size()) ++m_generations[e];
    }

    // ---- Structural hash ----
    // FNV-1a over the alive entity set (id + generation, ascending) and, for every
    // NON-EMPTY pool in type-name order, its member ids (ascending). Component VALUES
    // are not hashed: this is the structure a parallel schedule must never change
    // (-worldhash_every compares worker counts).
    uint64_t StructuralHash(size_t* outPools = nullptr) const
    {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](uint64_t v)
        {
            for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xFFu; h *= 1099511628211ull; }
        };
        std::vector<Entity> ents(m_entities.begin(), m_entities.end());
        std::sort(ents.begin(), ents.end());
        mix(ents.size());
        for (Entity e : ents)
        {
            mix(e);
            mix(e < m_generations.size() ? m_generations[e] : 0u);
        }
        std::vector<std::pair<std::string, const IComponentPool*>> pools;
        pools.reserve(m_pools.size());
        for (const auto& [ti, pool] : m_pools)
            if (!pool->MemberEntities().empty()) pools.emplace_back(ti.name(), pool.get());
        std::sort(pools.begin(), pools.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<Entity> members;
        for (const auto& [name, pool] : pools)
        {
            for (char c : name) mix(static_cast<uint8_t>(c));
            members = pool->MemberEntities();
            std::sort(members.begin(), members.end());
            mix(members.size());
            for (Entity e : members) mix(e);
        }
        if (outPools) *outPools = pools.size();
        return h;
    }

    // ---- Id quarantine ----
    // While on, DestroyEntity parks freed ids here instead of on the free list;
    // ReleaseQuarantinedIds (App, at the structural sync point, once the renderer has
    // applied its deferred destroys) hands them back in destroy order, so
    // CreateEntity's LIFO reuse is unchanged within a release.
    void SetIdQuarantine(bool on)
    {
        if (!on) ReleaseQuarantinedIds();
        m_quarantineIds = on;
    }
    bool IdQuarantineEnabled() const { return m_quarantineIds; }
    void ReleaseQuarantinedIds()
    {
        if (m_quarantine.empty()) return;
        m_freeList.insert(m_freeList.end(), m_quarantine.begin(), m_quarantine.end());
        m_quarantine.clear();
    }
    size_t QuarantinedIdCount() const { return m_quarantine.size(); }

    // ---- Undo support: create-with-id + id parking ----
    // Recreate an entity with an EXACT id + generation, so editor undo/redo can
    // resurrect a deleted subtree (or re-delete a created one) without new ids. Raw
    // Entity links (Parent/Children, UIParent) are not generation-checked, so restoring
    // the original id keeps them right; restoring the generation revalidates any
    // EntityHandle to it. The id must not be alive; returns NullEntity on misuse.
    Entity CreateEntityWithId(Entity id, uint32_t generation)
    {
        if (id == NullEntity || IsAlive(id)) return NullEntity;

        // Claim the id from wherever it lives (parked, free or quarantined).
        EraseFromVector(m_reserved, id);
        EraseFromVector(m_freeList, id);
        EraseFromVector(m_quarantine, id);

        // Grow the id space if this id is beyond what was issued; skipped ids go to the
        // free list.
        if (id >= m_nextEntity)
        {
            for (Entity g = m_nextEntity; g < id; ++g)
                m_freeList.push_back(g);
            m_nextEntity = id + 1;
        }

        if (id >= m_alive.size())       m_alive.resize(static_cast<size_t>(id) + 1, false);
        if (id >= m_generations.size()) m_generations.resize(static_cast<size_t>(id) + 1, 0u);

        m_entityIndex[id] = m_entities.size();
        m_entities.push_back(id);
        m_alive[id]       = true;
        m_generations[id] = (generation == 0u) ? 1u : generation;
        return id;
    }

    // Park a freed id so CreateEntity won't reissue it while an undo action
    // holds it for possible resurrection. Idempotent.
    void ReserveId(Entity id)
    {
        if (id == NullEntity) return;
        EraseFromVector(m_freeList, id);
        EraseFromVector(m_quarantine, id);
        if (std::find(m_reserved.begin(), m_reserved.end(), id) == m_reserved.end())
            m_reserved.push_back(id);
    }

    // Release a parked id back to the free list (undo action discarded without
    // resurrecting the entity). No-op if the id was not reserved.
    void UnreserveId(Entity id)
    {
        if (EraseFromVector(m_reserved, id))
            m_freeList.push_back(id);
    }

    // ---- Entity-destroy callback registry ----
    // Systems with per-entity caches (renderer textures, skinned prev-pose, chain
    // physics, script state, ...) register here. The callback fires from DestroyEntity
    // BEFORE the components are erased, so it can still read what it needs to clean
    // up. Ids are recycled, so without these callbacks a new entity reusing an id
    // inherits every stale cache entry keyed on it.
    [[nodiscard]] EntityDestroyListenerHandle
    AddEntityDestroyListener(EntityDestroyFn fn)
    {
        const EntityDestroyListenerHandle id = m_nextListenerId++;
        m_destroyListeners.push_back({ id, std::move(fn) });
        return id;
    }

    void RemoveEntityDestroyListener(EntityDestroyListenerHandle h)
    {
        if (h == 0) return;
        auto it = std::find_if(m_destroyListeners.begin(), m_destroyListeners.end(),
            [h](const DestroyListener& L) { return L.id == h; });
        if (it != m_destroyListeners.end()) m_destroyListeners.erase(it);
    }

    void Clear()
    {
        m_pools.clear();
        m_names.clear();
        m_freeList.clear();
        m_reserved.clear();
        m_quarantine.clear();
        m_entities.clear();
        m_entityIndex.clear();
        m_alive.clear();
        // Generations are KEPT and advanced, not reset: Clear() destroys every entity at
        // once, so it bumps every slot the way DestroyEntity bumps one. Ids restart at 1,
        // but an id or EntityHandle from the old world does not match the entity that
        // takes its slot (e.g. an agent's Entity.set after OpenScene, or a script holding
        // an id across a scene switch).
        for (uint32_t& g : m_generations)
            if (g != 0u) ++g;
        m_nextEntity = 1;
        ++m_worldEpoch;   // invalidate cross-frame caches (see m_worldEpoch)
        // Listeners survive Clear(): systems subscribe once per scene / engine lifetime,
        // and the OnWorldClear path (Renderer::OnWorldClear etc.) wipes their caches.
        // Do not clear them here: every system would be unhooked and leak stale caches on
        // the next scene's destroys.
    }

    // Monotonic counter bumped whenever the world's state is replaced wholesale:
    // Clear() and AdoptState() (the editor's Play/Stop restore). Cross-frame caches
    // compare it with a stored value and drop what they cached when it moves.
    // It is not only about dangling handles: after Clear() IsHandleValid catches old
    // handles, but code keyed on RAW ids (restarted at 1) does not; after AdoptState
    // handles stay valid while every pool behind them was swapped, so anything DERIVED
    // from pool contents is stale. A moved epoch means "re-derive everything".
    uint32_t Epoch() const { return m_worldEpoch; }

    // ---- Edit/Play snapshot & restore ----
    // See docs/design/editor_runtime_mode_architecture.md. The editor keeps one live
    // World (every system binds to its address); EnterPlay snapshots the editor state
    // and ExitPlay restores it, so Play mutates freely and Stop returns the scene to
    // exactly how it was authored.

    // Deep-copy entity bookkeeping, every component pool and the names into a detached
    // World, PRESERVING entity ids so intra-world references (Parent, Children,
    // AttachmentRef cache, Follow targets) stay valid without a remap. Destroy
    // listeners are NOT copied: the snapshot is inert data. Values go through
    // ComponentCloner<T> (GPU handles shared, move-only runtime backing reset).
    // Used to snapshot the editor world on EnterPlay.
    World CloneState() const
    {
        World w;
        w.m_entities    = m_entities;
        w.m_entityIndex = m_entityIndex;
        w.m_freeList    = m_freeList;
        w.m_reserved    = m_reserved;
        w.m_quarantine  = m_quarantine;
        w.m_alive       = m_alive;
        w.m_generations = m_generations;
        w.m_nextEntity  = m_nextEntity;
        w.m_names       = m_names;
        for (const auto& [ti, pool] : m_pools)
            w.m_pools.emplace(ti, pool->Clone());
        return w;
    }

    // Replace this world's entity/component/name data with `src`'s, keeping this
    // world's destroy-listener registry. Used to restore the editor snapshot on
    // ExitPlay. The caller MUST run the systems' OnWorldClear teardown (physics bodies,
    // script instances, renderer caches) BEFORE, because no per-entity destroy listener
    // fires, and GuidRegistry::RebuildFromWorld AFTER.
    // Rvalue-only on purpose: it CONSUMES the snapshot (moves the pools, saving a full
    // ComponentCloner pass, ~30 ms per Stop on a 15k-entity world). A const& overload
    // would let a caller silently deep-copy or restore twice from one snapshot.
    // Skipping a second Clone() is safe only because ComponentCloner scrubbing is
    // IDEMPOTENT (every specialization writes constants: computedScale = 1.f, frame = 0,
    // hovered = false, instance = nullptr, ...), and nothing mutates the snapshot during
    // Play (only IsAlive() and CaptureEditorWorldResources(const World&) read it). A
    // non-idempotent ComponentCloner specialization breaks this: restore a Clone() loop.
    void AdoptState(World&& src)
    {
        m_entities    = std::move(src.m_entities);
        m_entityIndex = std::move(src.m_entityIndex);
        m_freeList    = std::move(src.m_freeList);
        m_reserved    = std::move(src.m_reserved);
        m_quarantine  = std::move(src.m_quarantine);
        m_alive       = std::move(src.m_alive);
        m_generations = std::move(src.m_generations);
        m_nextEntity  = src.m_nextEntity;
        m_names       = std::move(src.m_names);
        m_pools       = std::move(src.m_pools);
        // m_destroyListeners + m_nextListenerId intentionally untouched.

        // Every derived cache keyed on this world is now wrong: the pools were replaced
        // wholesale, but ids and generations are RESTORED rather than advanced, so
        // IsHandleValid cannot detect it. Example: ComponentCloner<UIComputedRect> scrubs
        // layout to valid=false; a UI cache that only watched the World address and its
        // own fingerprint would skip the rebuild and paint nothing for the rest of the
        // session.
        ++m_worldEpoch;
    }

    bool IsAlive(Entity e) const
    {
        ECS_SPLITCHECK_READ("IsAlive");
        return e < m_alive.size() && m_alive[e];
    }

    // Exclusive upper bound on every entity id ever issued (alive or dead). O(1): sizes
    // entity-indexed scratch arrays without scanning GetEntities().
    size_t EntityIdBound() const { return m_alive.size(); }

    // Current generation of e (0 if never existed). Used to stamp handles.
    uint32_t GetGeneration(Entity e) const
    {
        return e < m_generations.size() ? m_generations[e] : 0u;
    }

    // Stamp a handle from a live entity. Returns NullEntityHandle if dead.
    EntityHandle MakeHandle(Entity e) const
    {
        if (!IsAlive(e)) return NullEntityHandle;
        return EntityHandle{ e, m_generations[e] };
    }

    // A handle is valid iff its entity is alive AND its generation matches - catches
    // the recycled-slot case that plain IsAlive misses.
    bool IsHandleValid(EntityHandle h) const
    {
        if (h.entity == NullEntity) return false;
        if (!IsAlive(h.entity))     return false;
        return m_generations[h.entity] == h.generation;
    }

    const std::vector<Entity>& GetEntities() const
    {
        ECS_SPLITCHECK_READ("GetEntities");
        return m_entities;
    }

    // ---- Entity naming ----
    void SetName(Entity e, std::string name) { m_names[e] = std::move(name); }

    const std::string& GetName(Entity e) const
    {
        static const std::string kUnnamed = "(unnamed)";
        auto it = m_names.find(e);
        return it != m_names.end() ? it->second : kUnnamed;
    }

    // ---- Component storage ----
    template <typename T>
    void AddComponent(Entity e, T component)
    {
        ECS::AccessGuard::CheckPoolWrite<T>();
        GetOrCreatePool<T>()->Add(e, std::move(component));
    }

    template <typename T>
    T* GetComponent(Entity e)
    {
        auto* p = GetPool<T>();
        return p ? p->Get(e) : nullptr;
    }

    template <typename T>
    const T* GetComponent(Entity e) const
    {
        auto* p = GetPool<T>();
        return p ? p->Get(e) : nullptr;
    }

    template <typename T>
    bool HasComponent(Entity e) const
    {
        const auto* p = GetPool<T>();
        return p && p->Has(e);
    }

    template <typename T>
    void RemoveComponent(Entity e)
    {
        ECS::AccessGuard::CheckPoolWrite<T>();
        auto* p = GetPool<T>();
        if (p) p->Erase(e);
    }

    // Dense iteration: visits only entities that have T, touching only T's dense array.
    // Faster than walking GetEntities() with GetComponent<T>() per entity.
    template <typename T, typename Fn>
    void ForEach(Fn&& fn)
    {
        auto* p = GetPool<T>();
        if (!p) return;
        auto& ents = p->Entities();
        auto& data = p->Data();
        const size_t n = data.size();
        const T* base = data.data();
        for (size_t i = 0; i < n; ++i)
            fn(ents[i], data[i]);
        // Post-hoc, but named: a callback that added/removed THIS component type
        // reallocated or compacted the arrays being iterated, so the cached size and every
        // reference handed to fn were invalid (silent OOB read/write). Route structural
        // changes through CommandBuffer.
        ENGINE_CHECK_ONCE(data.size() == n && data.data() == base,
                          "World::ForEach<%s>: callback mutated the pool "
                          "mid-iteration — the loop just ran on stale/"
                          "reallocated memory", typeid(T).name());
    }

    template <typename T, typename Fn>
    void ForEach(Fn&& fn) const
    {
        const auto* p = GetPool<T>();
        if (!p) return;
        const auto& ents = p->Entities();
        const auto& data = p->Data();
        const size_t n = data.size();
        const T* base = data.data();
        for (size_t i = 0; i < n; ++i)
            fn(ents[i], data[i]);
        ENGINE_CHECK_ONCE(data.size() == n && data.data() == base,
                          "World::ForEach<%s> (const): callback mutated the "
                          "pool mid-iteration", typeid(T).name());
    }

    // Like GetPool<T>() but creates the pool if missing. A system that writes a
    // component every frame should cache this pointer once per tick to skip the
    // m_pools hash lookup.
    template <typename T>
    ComponentPool<T>* EnsurePool() { return GetOrCreatePool<T>(); }

    // Public pool access (GetComponentTypeIndices, the editor's component-tag
    // registry). The AccessGuard check enforces DeclareAccess (ECS/AccessGuard.h): free
    // on the main thread, one set probe inside a worker-batched system, one ERROR line
    // per violating (site, type).
    template <typename T>
    ComponentPool<T>* GetPool()
    {
        ECS::AccessGuard::CheckPoolAccess<T>();
        ECS::AccessGuard::CheckRenderCtxPool<T>();
        const auto it = m_pools.find(std::type_index(typeid(T)));
        return it == m_pools.end()
            ? nullptr
            : static_cast<ComponentPool<T>*>(it->second.get());
    }

    template <typename T>
    const ComponentPool<T>* GetPool() const
    {
        ECS::AccessGuard::CheckPoolAccess<T>();
        ECS::AccessGuard::CheckRenderCtxPool<T>();
        const auto it = m_pools.find(std::type_index(typeid(T)));
        return it == m_pools.end()
            ? nullptr
            : static_cast<const ComponentPool<T>*>(it->second.get());
    }

    // ---- Per-entity value snapshot (editor undo) ----
    // Deep-copy EVERY component on e into type-erased holders (see
    // IComponentPool::CloneComponent), including runtime components with no serializer
    // (skinned-mesh Skeleton/Morph/Animation/MeshLibRef) that a text snapshot drops.
    std::vector<std::pair<std::type_index, std::shared_ptr<void>>>
    CloneEntityComponents(Entity e) const
    {
        std::vector<std::pair<std::type_index, std::shared_ptr<void>>> out;
        out.reserve(m_pools.size());
        for (const auto& [ti, pool] : m_pools)
            if (pool->Has(e))
                if (auto h = pool->CloneComponent(e))
                    out.emplace_back(ti, std::move(h));
        return out;
    }

    // Restore components captured by CloneEntityComponents onto e. Each type's pool
    // still exists (pools are dropped only by Clear()), so no type knowledge is needed.
    void RestoreEntityComponents(
        Entity e,
        const std::vector<std::pair<std::type_index, std::shared_ptr<void>>>& comps)
    {
        for (const auto& [ti, holder] : comps)
        {
            auto it = m_pools.find(ti);
            if (it != m_pools.end())
                it->second->RestoreComponent(e, holder);
        }
    }

    // The std::type_index of every component type attached to e, in unspecified
    // (hash-map) order.
    std::vector<std::type_index> GetComponentTypeIndices(Entity e) const
    {
        std::vector<std::type_index> out;
        out.reserve(m_pools.size());
        for (const auto& [typeIdx, pool] : m_pools)
            if (pool->Has(e))
                out.push_back(typeIdx);
        return out;
    }

    // ---- Type-erased single-component access ----
    // Has/Remove/Clone/Restore for ONE component named by std::type_index, for tools
    // that start from GetComponentTypeIndices() and never have T (the editor's
    // Component Transfer window). Each mirrors its templated twin:
    //   HasComponentByType    == HasComponent<T>
    //   RemoveComponentByType == RemoveComponent<T> (a pool Erase; no listeners)
    //   Clone/RestoreComponentByType == the one-type slice of
    //     Clone/RestoreEntityComponents (same ComponentCloner<T> deep copy), so taking
    //     one component off a skinned character does not deep-copy all ~30.
    // "No pool" and "entity lacks it" are the same answer (pools are created lazily),
    // so all four are no-ops returning false/nullptr on an unknown type.
    // RestoreComponentByType CANNOT create a pool (no T to instantiate), so it fails;
    // that only happens when source and destination are different Worlds.
    // These skip AccessGuard::CheckPoolAccess (templated on T): main-thread editor
    // entry points only, never from a worker-batched system.
    bool HasComponentByType(Entity e, std::type_index ti) const
    {
        const auto it = m_pools.find(ti);
        return it != m_pools.end() && it->second->Has(e);
    }

    bool RemoveComponentByType(Entity e, std::type_index ti)
    {
        const auto it = m_pools.find(ti);
        if (it == m_pools.end() || !it->second->Has(e)) return false;
        it->second->Erase(e);
        return true;
    }

    std::shared_ptr<void> CloneComponentByType(Entity e, std::type_index ti) const
    {
        const auto it = m_pools.find(ti);
        return it == m_pools.end() ? nullptr : it->second->CloneComponent(e);
    }

    bool RestoreComponentByType(Entity e, std::type_index ti,
                                const std::shared_ptr<void>& holder)
    {
        if (!holder) return false;
        const auto it = m_pools.find(ti);
        if (it == m_pools.end()) return false;   // no pool -> no way to construct T
        it->second->RestoreComponent(e, holder);
        return true;
    }

private:
    struct DestroyListener
    {
        EntityDestroyListenerHandle id;
        EntityDestroyFn             fn;
    };

    // Erase the first occurrence of `id` from `v`; returns true if removed.
    static bool EraseFromVector(std::vector<Entity>& v, Entity id)
    {
        auto it = std::find(v.begin(), v.end(), id);
        if (it == v.end()) return false;
        v.erase(it);
        return true;
    }

    template <typename T>
    ComponentPool<T>* GetOrCreatePool()
    {
        ECS_SPLITCHECK_STRUCTURAL("GetOrCreatePool");
        // Enforces the same DeclareAccess contract as GetPool.
        ECS::AccessGuard::CheckPoolAccess<T>();
        const auto ti = std::type_index(typeid(T));
        const auto it = m_pools.find(ti);
        if (it != m_pools.end())
            return static_cast<ComponentPool<T>*>(it->second.get());
        ECS_ACCESSGUARD_STRUCTURAL("GetOrCreatePool (creates the pool)");
        auto pool = std::make_unique<ComponentPool<T>>();
        ComponentPool<T>* raw = pool.get();
        m_pools.emplace(ti, std::move(pool));
        return raw;
    }

    std::vector<Entity>                    m_entities;
    // Entity -> index in m_entities, for O(1) DestroyEntity.
    std::unordered_map<Entity, size_t>     m_entityIndex;
    std::vector<Entity>                    m_freeList;
    // Ids parked by undo/redo (a deleted entity's id held for resurrection).
    // Kept out of m_freeList so CreateEntity can't reissue them; carried by
    // CloneState/AdoptState so the reservation survives Play/Stop.
    std::vector<Entity>                    m_reserved;
    std::vector<Entity> m_quarantine;      // destroyed, not yet reusable (see SetIdQuarantine)
    bool                m_quarantineIds = false;
    std::vector<bool>                      m_alive;
    // Parallel to m_alive: bumped on DestroyEntity so stale EntityHandles
    // fail IsHandleValid even after the slot is reused.
    std::vector<uint32_t>                  m_generations;
    Entity                                 m_nextEntity{ 1u };
    // Bumped by Clear() and AdoptState(). Clear() resets m_nextEntity, so RAW ids
    // restart at 1 and re-alias the old world's, while m_generations advance so an old
    // handle's {id,gen} fails IsHandleValid. A cross-frame consumer that caches raw ids
    // or world-derived data snapshots Epoch() and drops its cache when it changes.
    uint32_t                               m_worldEpoch{ 0u };
    std::unordered_map<Entity, std::string> m_names;

    // One pool per component type. Owned by World; type-erased through
    // IComponentPool so DestroyEntity / Clear can iterate uniformly.
    std::unordered_map<std::type_index, std::unique_ptr<IComponentPool>> m_pools;

    std::vector<DestroyListener> m_destroyListeners;
    EntityDestroyListenerHandle  m_nextListenerId{ 1u };
};

// System base: override Tick(deltaTime) for per-frame work.
struct SystemBase
{
    virtual ~SystemBase() = default;
    virtual void Tick(float /*deltaTime*/) {}
};
