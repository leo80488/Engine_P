#pragma once
// ECS/StaticMeshComponents.h: one mesh of a model, placed as an object.
//
// The owner carries StaticMeshComponent and is the only thing saved: which model (its main .iscn),
// which Mesh in it (a stable MeshKey, Resource/MeshLibIdentity.h) and per-slot material overrides.
// It draws nothing itself.
//
// The sections are derived: MeshInstanceSystem gives the owner one child entity per material
// section with exactly the components a .iscn spawn gives a mesh part (MeshLibRef,
// MaterialComponent, LocalAabb / WorldAabb, VisibilityComponent, ...), so every renderer path -
// gather, retained store, DDGI, RT, picking, shadows - draws them unchanged. They are tagged
// MeshSectionOf + SceneTransientTag: never saved, never duplicated, rebuilt whenever the owner's
// authored fields change.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>

struct StaticMeshComponent        // serialized as "StaticMesh"
{
    enum class Collision : uint8_t { Asset = 0, None = 1 };

    struct SlotOverride
    {
        uint64_t    sectionKey = 0;             // the slot (SectionKey)
        std::string material;                   // project-relative .imat / .imatinst
    };

    std::string model;            // the model's main .iscn, project-relative
    uint64_t    meshKey = 0;      // Resource::MeshIdentity MeshKey inside that model
    std::string meshName;         // resolution fallback (a key the model lost) + display

    Collision   collision = Collision::Asset;   // Asset: MeshInstanceSystem builds the Mesh's collision; None: none

    std::vector<SlotOverride> slotOverrides;    // sparse: only the overridden slots

    // ---- runtime (MeshInstanceSystem); never serialized ----
    // A resolution that failed is not retried every frame: it is remembered
    // against the authored fields and the world epoch that produced it.
    uint64_t    failedSignature = 0;
    uint32_t    failedEpoch     = 0;
    std::string status;           // "" when drawn as authored; otherwise what went wrong

    // FNV-1a over every authored field. A derived section remembers the value
    // its owner had; any difference - an Inspector edit, Entity.set, an undo
    // restoring an older value, a deserialize - rebuilds the sections.
    uint64_t Signature() const
    {
        uint64_t h = 14695981039346656037ull;
        auto mix = [&h](const void* p, size_t n)
        {
            const auto* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
        };
        auto str = [&](const std::string& s) { const uint64_t n = s.size(); mix(&n, sizeof(n)); mix(s.data(), s.size()); };
        str(model);
        mix(&meshKey, sizeof(meshKey));
        str(meshName);
        const uint8_t c = static_cast<uint8_t>(collision);
        mix(&c, 1);
        for (const SlotOverride& o : slotOverrides) { mix(&o.sectionKey, sizeof(o.sectionKey)); str(o.material); }
        return h ? h : 1ull;
    }

    // The override for @p sectionKey, or nullptr.
    const SlotOverride* FindOverride(uint64_t sectionKey) const
    {
        for (const SlotOverride& o : slotOverrides)
            if (o.sectionKey == sectionKey) return &o;
        return nullptr;
    }
};

// On every derived section entity (runtime only, like the entity itself).
struct MeshSectionOf
{
    Entity   owner           = NullEntity;
    uint32_t ownerGeneration = 0;  // World::GetGeneration(owner): an id reused by a new entity is not the owner
    uint32_t index           = 0;  // position in the Mesh's section list
    uint32_t count          = 0;   // how many sections the owner was given
    uint64_t sectionKey     = 0;
    uint64_t ownerSignature = 0;   // StaticMeshComponent::Signature() at derivation
};
