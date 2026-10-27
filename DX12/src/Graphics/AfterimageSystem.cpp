#include "Graphics/AfterimageSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/MeshDescriptorHeap.h"
#include "Graphics/SkinningBuffers.h"
#include "ECS/AnimationComponents.h"
#include "ECS/HierarchyComponents.h" // GlobalTransform
#include "ECS/ECS.h"
#include "System/Log.h"
#include "System/Check.h"   // ENGINE_CHECK_ONCE: lazy pool creation is retried per frame

#include <algorithm>

using namespace DirectX;

// ==== Init / Shutdown ====
void AfterimageSystem::Init(IGraphicsDevice& gfx, MeshDescriptorHeap& /*heap*/)
{
    if (m_initialised) return;

    // Init only records the device and sizes the request queues; the pool and everything GPU-side
    // wait for the first spawn (EnsurePool).
    m_gfx = &gfx;

    m_pendingSpawns.reserve(kMaxSnapshots);
    m_captureJobs.reserve(kMaxSnapshots);

    m_initialised = true;
    LOG_SUCCESS("AfterimageSystem: initialised (%u slots, %u verts/slot, %.1f MB pool "
                "allocated on first ghost)",
                kMaxSnapshots, kVertsPerSlot,
                static_cast<double>(static_cast<uint64_t>(kSlotPosBytes + kSlotNrmBytes)
                                    * kMaxSnapshots) / (1024.0 * 1024.0));
}

bool AfterimageSystem::EnsurePool(MeshDescriptorHeap& heap)
{
    if (m_poolReady) return true;
    if (!m_gfx)      return false;

    const uint64_t poolPosBytes =
        static_cast<uint64_t>(kSlotPosBytes) * kMaxSnapshots;
    const uint64_t poolNrmBytes =
        static_cast<uint64_t>(kSlotNrmBytes) * kMaxSnapshots;

    {
        RHI::GPUBufferDesc bd{};
        bd.size       = poolPosBytes;
        bd.stride     = sizeof(float) * 3;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;

        // ONCE: EnsurePool is retried on every frame that has a pending spawn,
        // so a persistent failure would otherwise write a line per frame.
        if (!ENGINE_CHECK_ONCE(m_gfx->CreateBuffer(bd, m_poolPos),
                               "AfterimageSystem: pool position buffer (%llu B) failed",
                               static_cast<unsigned long long>(poolPosBytes)))
        {
            ReleasePool();
            return false;
        }
        m_poolPosUAV = m_gfx->GetBufferUAVGpuHandle(m_poolPos);
        m_poolPosSRV = m_gfx->GetBufferSRVGpuHandle(m_poolPos);
        m_poolPosBindless = heap.RegisterBuffer(m_poolPos);
    }
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = poolNrmBytes;
        bd.stride     = sizeof(float) * 3;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::NONE;

        if (!ENGINE_CHECK_ONCE(m_gfx->CreateBuffer(bd, m_poolNrm),
                               "AfterimageSystem: pool normal buffer (%llu B) failed",
                               static_cast<unsigned long long>(poolNrmBytes)))
        {
            ReleasePool();
            return false;
        }
        m_poolNrmUAV = m_gfx->GetBufferUAVGpuHandle(m_poolNrm);
        m_poolNrmSRV = m_gfx->GetBufferSRVGpuHandle(m_poolNrm);
        m_poolNrmBindless = heap.RegisterBuffer(m_poolNrm);
    }

    // A dry bindless table hands back kInvalidBufferIndex; a descriptor built
    // on it would make the ghost read garbage geometry, so refuse instead.
    if (!ENGINE_CHECK_ONCE(m_poolPosBindless != RHI::kInvalidBufferIndex &&
                           m_poolNrmBindless != RHI::kInvalidBufferIndex,
                           "AfterimageSystem: bindless buffer registration failed "
                           "(pos=%u nrm=%u) — ghosts disabled this world",
                           m_poolPosBindless, m_poolNrmBindless))
    {
        ReleasePool();
        return false;
    }

    // Reserve a stable MeshDescriptor slot per snapshot. Slot fields are
    // overwritten on each capture; we just need the slot index now.
    for (uint32_t i = 0; i < kMaxSnapshots; ++i)
    {
        RHI::MeshDescriptor desc{};
        desc.position.bufferIndex = m_poolPosBindless;
        desc.position.byteOffset  = i * kSlotPosBytes;
        desc.position.byteStride  = sizeof(float) * 3;
        desc.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
        desc.normal.bufferIndex   = m_poolNrmBindless;
        desc.normal.byteOffset    = i * kSlotNrmBytes;
        desc.normal.byteStride    = sizeof(float) * 3;
        desc.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);
        desc.vertexCount          = 0; // overridden on capture

        const uint32_t slot = heap.RegisterMesh(desc);
        m_slots[i].meshDescSlot = slot;
        ConfigureSlotMaterial(m_slots[i]);
    }

    m_poolReady = true;
    LOG_INFO("AfterimageSystem: snapshot pool allocated (%.1f MB, first ghost spawned)",
             static_cast<double>(poolPosBytes + poolNrmBytes) / (1024.0 * 1024.0));
    return true;
}

void AfterimageSystem::ReleasePool()
{
    if (m_gfx)
    {
        // DestroyBuffer defers the resource AND its descriptors kFrameCount frames, so an in-flight
        // capture dispatch finishes safely. The MeshDescriptorHeap slots are world-scoped and rewound by
        // the heap's own OnWorldClear right after ours: nothing to hand back here.
        if (m_poolPos.IsValid()) m_gfx->DestroyBuffer(m_poolPos);
        if (m_poolNrm.IsValid()) m_gfx->DestroyBuffer(m_poolNrm);
    }
    m_poolPos = {};
    m_poolNrm = {};
    m_poolPosUAV = m_poolNrmUAV = 0;
    m_poolPosSRV = m_poolNrmSRV = 0;
    m_poolPosBindless = m_poolNrmBindless = 0xFFFFFFFFu;
    for (auto& s : m_slots) s.meshDescSlot = 0xFFFFFFFFu;
    m_poolReady = false;
}

void AfterimageSystem::Shutdown(IGraphicsDevice& /*gfx*/)
{
    // The pool buffers go with the device's buffer pool at its destruction; the bindless and
    // MeshDescriptor slots are world-scoped and rewound by MeshDescriptorHeap.
    m_pendingSpawns.clear();
    m_captureJobs.clear();
    for (auto& s : m_slots) s.alive = false;
    m_initialised = false;
}

// ==== Spawn / lifetime management ====
void AfterimageSystem::Spawn(Entity e, float lifetime, const XMFLOAT4& color)
{
    if (!m_initialised) return;
    if (lifetime <= 0.f)       return;
    if (e == NullEntity)       return;
    m_pendingSpawns.push_back({ e, lifetime, color });
}

void AfterimageSystem::OnWorldClear()
{
    m_pendingSpawns.clear();
    m_captureJobs.clear();
    for (auto& s : m_slots) s.alive = false;
    // Release the pool with the world (see EnsurePool). Renderer::OnWorldClear's caller contract
    // guarantees WaitIdleAndReleaseDeferred already ran and this frame has recorded nothing.
    ReleasePool();
}

void AfterimageSystem::OnEntityDestroyed(Entity e)
{
    // Live snapshots already captured stay: they do not reference the source entity.
    m_pendingSpawns.erase(
        std::remove_if(m_pendingSpawns.begin(), m_pendingSpawns.end(),
                       [e](const PendingSpawn& p) { return p.entity == e; }),
        m_pendingSpawns.end());
}

void AfterimageSystem::SetGhostParams(float fresnelPower, float baseAlpha, float rimIntensity)
{
    m_fresnelPower = (std::max)(fresnelPower, 0.01f);
    m_baseAlpha    = std::clamp(baseAlpha, 0.0f, 1.0f);
    m_rimIntensity = (std::max)(rimIntensity, 0.0f);
}

uint32_t AfterimageSystem::FindFreeSlot()
{
    for (uint32_t i = 0; i < kMaxSnapshots; ++i)
        if (!m_slots[i].alive) return i;
    return 0xFFFFFFFFu;
}

void AfterimageSystem::ConfigureSlotMaterial(Slot& slot)
{
    MaterialComponent& mc = slot.material;
    mc.useCustomShader   = true;
    mc.customShaderPath  = "shaders/Afterimage_Ghost.ps.hlsl";
    mc.userBlendMode     = BlendMode::Additive;
    // The SHADING MODEL routes the lighting (ShadingStencil::RefFor); shaderType is kept only for the
    // on-disk format. Setting shaderType alone would route the additive ghost through the PBR
    // lighting variant and give it a BRDF.
    mc.customShadingModel = ShadingModel::Unlit;
    mc.shaderType        = MaterialComponent::SHADERTYPE_UNLIT;
    mc.flags |= MaterialComponent::DOUBLE_SIDED;
    mc.flags &= ~MaterialComponent::CAST_SHADOW;
    mc.SetDirty();
}

// ==== BeginFrame: tick, free, then drain pending spawns into slots ====
void AfterimageSystem::BeginFrame(World&              world,
                                   MeshDescriptorHeap& heap,
                                   SkinnedVertexRing&  vertRing,
                                   float               dt)
{
    if (!m_initialised) return;

    m_captureJobs.clear();

    // ---- Decay & retire ----
    for (auto& s : m_slots)
    {
        if (!s.alive) continue;
        s.lifetime -= dt;
        if (s.lifetime <= 0.f)
        {
            s.alive       = false;
            s.lifetime    = 0.f;
            s.vertexCount = 0;
            continue;
        }
        // Refresh per-slot ghost CB uniforms.
        const float lifeFade = (s.maxLifetime > 0.f)
            ? std::clamp(s.lifetime / s.maxLifetime, 0.f, 1.f)
            : 0.f;
        // GhostColor in shader: rgb tint, .a = lifeFade.
        s.material.customParams["GhostColor"]  = { s.color.x, s.color.y, s.color.z, lifeFade };
        s.material.customParams["GhostParams"] = { m_fresnelPower, m_baseAlpha, m_rimIntensity, 0.f };
        s.material.SetDirty();
    }

    if (m_pendingSpawns.empty()) return;

    // First ghost of this world -> materialise the snapshot pool. Here, in Renderer::BeginFrame
    // (main thread, before any pass records): buffer creation mints descriptors and would race the
    // graph's worker recorders anywhere later.
    if (!EnsurePool(heap))
    {
        m_pendingSpawns.clear();
        return;
    }

    // ---- Resolve pending spawns ----
    auto* pSkinned    = world.GetPool<MeshSkinnedComponent>();
    auto* pSkinOut    = world.GetPool<SkinningOutputComponent>();
    auto* pGlobal     = world.GetPool<GlobalTransform>();
    auto* pSkeletonRef = world.GetPool<SkeletonRef>();

    if (!pSkinned || !pSkinOut)
    {
        m_pendingSpawns.clear();
        return;
    }

    // Resolve a spawn target to one-or-more concrete mesh entities. The
    // caller may pass either:
    //   (a) the mesh entity itself (has MeshSkinnedComponent), or
    //   (b) the character/skeleton root entity. We then walk SkeletonRef and
    //       fan out to every mesh entity owned by that root (body, clothes,
    //       hair, etc). This is what callers expect: "spawn a ghost of this
    //       character" without knowing the internal entity layout.
    auto resolveTargets = [&](Entity src, std::vector<Entity>& out)
    {
        if (pSkinned->Get(src) != nullptr)
        {
            out.push_back(src);
            return;
        }
        if (!pSkeletonRef) return;
        auto& refEnts = pSkeletonRef->Entities();
        auto& refData = pSkeletonRef->Data();
        const size_t n = refData.size();
        for (size_t i = 0; i < n; ++i)
        {
            if (refData[i].entity == src && pSkinned->Get(refEnts[i]) != nullptr)
                out.push_back(refEnts[i]);
        }
    };

    std::vector<Entity> targets;
    targets.reserve(8);

    uint32_t frameCaptured = 0;
    uint32_t frameDroppedPool = 0;
    uint32_t frameDroppedOversize = 0;
    uint32_t frameDroppedNoMesh = 0;

    for (const PendingSpawn& req : m_pendingSpawns)
    {
        if (!world.IsAlive(req.entity)) continue;

        targets.clear();
        resolveTargets(req.entity, targets);
        if (targets.empty())
        {
            ++frameDroppedNoMesh;
            continue;
        }

        for (Entity meshEnt : targets)
        {
        const MeshSkinnedComponent*   skinned = pSkinned->Get(meshEnt);
        const SkinningOutputComponent* skinOut = pSkinOut->Get(meshEnt);
        if (!skinned || !skinOut) continue;
        if (skinned->vertexCount == 0)                continue;
        if (skinned->vertexCount > kVertsPerSlot)
        {
            ++frameDroppedOversize;
            continue;
        }
        if (skinOut->poseByteOffset == ~0u) continue; // not skinned this frame

        const uint32_t slotIdx = FindFreeSlot();
        if (slotIdx == 0xFFFFFFFFu)
        {
            ++frameDroppedPool;
            continue;
        }
        Slot& slot = m_slots[slotIdx];

        // ---- Snapshot CPU-side state ----
        slot.alive       = true;
        slot.lifetime    = req.lifetime;
        slot.maxLifetime = req.lifetime;
        slot.color       = req.color;
        slot.vertexCount = skinned->vertexCount;
        slot.indexCount  = skinned->indexCount;

        // World transform: the skinned vertices in the pool are still MESH-local (Skin.cs.hlsl outputs
        // bone-deformed, un-world-transformed positions). Read GlobalTransform from the MESH entity, not
        // req.entity: a scene-graph mesh entity's LocalTransform is identity, so its world pose lives on
        // its GlobalTransform, while the skeleton root may be identity or a different transform entirely.
        if (pGlobal)
        {
            if (const GlobalTransform* gt = pGlobal->Get(meshEnt))
                slot.worldMatrix = gt->matrix;
            else
                XMStoreFloat4x4(&slot.worldMatrix, XMMatrixIdentity());
        }
        else
            XMStoreFloat4x4(&slot.worldMatrix, XMMatrixIdentity());

        // World-space AABB center for transparent painter's-algorithm sort.
        const XMFLOAT3 localCenter{
            0.5f * (skinned->aabbMin.x + skinned->aabbMax.x),
            0.5f * (skinned->aabbMin.y + skinned->aabbMax.y),
            0.5f * (skinned->aabbMin.z + skinned->aabbMax.z) };
        XMVECTOR wc = XMVector3Transform(XMLoadFloat3(&localCenter),
                                          XMLoadFloat4x4(&slot.worldMatrix));
        XMStoreFloat3(&slot.worldCenter, wc);

        // ---- Point the slot's bindless MeshDescriptor at the snapshot ----
        // pos / nrm come from this slot's pool slice, UV / index / tangent from the source skinned mesh's
        // static streams.
        RHI::MeshDescriptor desc = skinned->baseMeshDesc;
        desc.position.bufferIndex = m_poolPosBindless;
        desc.position.byteOffset  = slotIdx * kSlotPosBytes;
        desc.position.byteStride  = sizeof(float) * 3;
        desc.position.format      = static_cast<uint32_t>(RHI::VertexFormat::Float3);
        desc.normal.bufferIndex   = m_poolNrmBindless;
        desc.normal.byteOffset    = slotIdx * kSlotNrmBytes;
        desc.normal.byteStride    = sizeof(float) * 3;
        desc.normal.format        = static_cast<uint32_t>(RHI::VertexFormat::Float3);
        desc.vertexCount          = skinned->vertexCount;
        heap.UpdateMesh(slot.meshDescSlot, desc);

        // ---- Queue a copy job for AfterimageCapturePass ----
        CaptureJob job{};
        job.srcPosSrvHandle    = vertRing.UploadPosSRVHandle();
        job.srcNrmSrvHandle    = vertRing.UploadNrmSRVHandle();
        job.srcPosElementBase  = skinOut->outPosByteOffset / 12u;
        job.srcNrmElementBase  = skinOut->outNrmByteOffset / 12u;
        job.dstPosElementBase  = slotIdx * kVertsPerSlot;
        job.dstNrmElementBase  = slotIdx * kVertsPerSlot;
        job.vertexCount        = skinned->vertexCount;
        m_captureJobs.push_back(job);

        // Pre-populate this frame's ghost CB uniforms (so the first draw doesn't
        // sample uninitialised customParams).
        slot.material.customParams["GhostColor"]  = { slot.color.x, slot.color.y, slot.color.z, 1.f };
        slot.material.customParams["GhostParams"] = { m_fresnelPower, m_baseAlpha, m_rimIntensity, 0.f };
        slot.material.SetDirty();

        ++frameCaptured;
        }
    }

    m_pendingSpawns.clear();

    // One consolidated summary per frame: never a warning per dropped spawn.
    if (frameCaptured > 0 || frameDroppedPool > 0 ||
        frameDroppedOversize > 0 || frameDroppedNoMesh > 0)
    {
        if (frameDroppedPool > 0 || frameDroppedOversize > 0 || frameDroppedNoMesh > 0)
        {
            LOG_WARNING("AfterimageSystem: this frame — captured=%u, dropped: pool=%u "
                        "oversize=%u noMesh=%u (pool=%u slots × %u verts; "
                        "consider raising kMaxSnapshots if pool=N consistently)",
                        frameCaptured, frameDroppedPool, frameDroppedOversize,
                        frameDroppedNoMesh, kMaxSnapshots, kVertsPerSlot);
        }
        else
        {
            LOG_INFO("AfterimageSystem: captured %u snapshot%s this frame",
                     frameCaptured, frameCaptured == 1 ? "" : "s");
        }
    }
}
