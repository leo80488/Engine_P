#pragma once

// VFXSpawnRequests: "Lane B" mailbox components for the unified VFX spawner.
//
// VFXSpawnSystem (TickPhase::BoneAttachment) spawns the component-driven effect types (Particle /
// Trail / Beam) directly because they are pure ECS. The others need a Renderer-owned GPU pool or
// asset library:
//   - Tracer     -> TracerSystem::Spawn (GPU ring, needs IGraphicsDevice)
//   - Decal      -> DecalSpawner + DecalMaterialLibrary (Renderer-owned)
//   - Afterimage -> AfterimageSystem::Spawn (fixed snapshot pool; snapshots the spawner's skinned
//                   sub-meshes)
//   - Mesh       -> MeshLibrary load (path -> GPU mesh; Renderer-owned)
// So the ECS side holds no IGraphicsDevice / Renderer handle, VFXSpawnSystem only deposits fully
// resolved requests into these mailboxes on the spawner entity. Renderer::BeginFrame (Render phase,
// after BoneAttachment in the same frame) drains them, issues the Spawn() calls and clears the
// vectors - the same producer -> Renderer pattern ParticleSystem uses for
// ParticleEmitterComponent. Drained the same frame, so nothing goes stale across frames.

#include "ECS/ECS.h"

#include <cstdint>
#include <string>
#include <vector>
#include <DirectXMath.h>

// ---- Tracer (TracerSystem::Spawn) ----
struct PendingTracerSpawns
{
    struct Req {
        DirectX::XMFLOAT3 start;
        DirectX::XMFLOAT3 end;
        DirectX::XMFLOAT4 color;             // linear HDR; .a = intensity
        float             width;
        float             lifetime;
        uint32_t          noiseTexBindless;  // 0xFFFFFFFF = procedural
        // Non-empty -> the Renderer resolves this .itex to a bindless index at drain, overriding
        // noiseTexBindless.
        std::string       noiseTexPath;
    };
    std::vector<Req> reqs;
};

// ---- Decal (DecalSpawner::SpawnAtSurface, material via DecalMaterialLibrary) ----
struct PendingDecalSpawns
{
    struct Req {
        std::string       materialName;      // resolved via DecalMaterialLibrary::Find
        DirectX::XMFLOAT3 position;          // world hit point (baked from spawn pose)
        DirectX::XMFLOAT3 normal;            // surface normal the decal projects onto
        DirectX::XMFLOAT2 size;              // decal width/height (world units)
        float             depth;             // projection-box thickness
        float             lifetime;          // seconds; < 0 = static
        float             fadeOutDuration;
        float             rollZ;             // rotation about projection axis (rad)
        DirectX::XMFLOAT4 tintOverride;      // w>0 replaces asset tint
    };
    std::vector<Req> reqs;
};

// ---- Afterimage (AfterimageSystem::Spawn on the spawner's skinned children) ----
struct PendingAfterimageSpawns
{
    struct Req {
        EntityHandle      target;            // spawner character; Renderer fans out via SkeletonRef
        float             lifetime;
        DirectX::XMFLOAT4 color;             // HDR rgb tint (a ignored)
    };
    std::vector<Req> reqs;
};

// ---- Mesh VFX (runtime path -> GPU mesh; the Renderer patches the created entity) ----
// VFXSpawnSystem already created the entity with transform / attach / lifetime; the Renderer only
// resolves the mesh path to a MeshHandle / MeshLibRef and patches it (plus the BoundingVolume)
// onto the entity.
struct PendingMeshVFXSpawns
{
    struct Req {
        EntityHandle      entity;            // pre-created renderable awaiting a GPU mesh
        std::string       meshPath;          // .meshlib
        uint64_t          sectionKey = 0;    // the entry drawn, by key (0: entry 0)
        std::string       materialPath;      // .imat (may be empty -> default material)
        bool              castShadow;
    };
    std::vector<Req> reqs;
};
