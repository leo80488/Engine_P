#include "Graphics/WaterRippleSystem.h"
#include "ECS/WaterInteractionComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"

#include "ECS/ECS.h"
#include "ECS/WaterComponent.h"
#include "ECS/Components.h"          // MaterialComponent (IsWaterShadingModel)
#include "ECS/HierarchyComponents.h"   // LocalTransform
#include "ECS/PhysicsComponents.h"     // RigidBodyComponent
#include "System/Check.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// Shared compute root signature slots (the table ParticleSimPass uses): absolute compute root
// indices from RHI::Slots::Cmp (Graphics/RootSignatureLayout.h). Do not take these from Gfx::.
static constexpr auto kCSCBSlot = RHI::Slots::Cmp::CB;      // b0 space2 (ROOT_CBV)
static constexpr auto kCSU0Slot = RHI::Slots::Cmp::UAV_u0;  // u0 space2 (gSrc)
static constexpr auto kCSU1Slot = RHI::Slots::Cmp::UAV_u1;  // u1 space2 (gDst)

using namespace DirectX;

void WaterRippleSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::WaterRippleStep_CS, RHI::ShaderStage::CS,
                         "WaterRippleStep.cs.hlsl", "CSMain");

    CreatePipelines(gfx);

    if (!m_cb.Create(gfx, "WaterRippleSystem.CB"))
        LOG_ERROR("WaterRippleSystem: CB ring creation failed");

    // Fixed slot pool - two ping-pong R16G16_FLOAT fields per slot, zero-filled.
    const uint32_t res = kResolution;
    std::vector<uint16_t> zeros(static_cast<size_t>(res) * res * 2, 0);  // 2 channels
    for (auto& slot : m_slots)
    {
        for (int k = 0; k < 2; ++k)
        {
            RHI::TextureDesc td{};
            td.width      = res;
            td.height     = res;
            td.format     = RHI::Format::R16G16_FLOAT;
            td.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
            // Default layout (SHADER_RESOURCE): the zero-fill upload leaves the texture there and the first
            // Execute transitions it to UAV, so the tracked state matches and the first frame gets no
            // spurious or missing barrier.
            RHI::SubresourceData init{ zeros.data(), res * 2u * 2u /* bytes/row */,
                                       static_cast<uint32_t>(zeros.size() * 2) };
            if (!gfx.CreateTexture(td, slot.tex[k], &init))
            {
                LOG_ERROR("WaterRippleSystem: ripple field texture creation failed");
                slot.valid = false;
                break;
            }
            slot.uav[k]   = gfx.GetTextureUAVGpuHandle(slot.tex[k]);
            slot.srv[k]   = gfx.GetTextureSRVGpuHandle(slot.tex[k]);
            slot.state[k] = RHI::ResourceState::SHADER_RESOURCE;
            slot.valid    = true;
        }
        slot.read  = 0;
        slot.owner = NullEntity;
    }

    LOG_SUCCESS("WaterRippleSystem: initialised (%u slots @ %u²)", kMaxBodies, res);
}

// Creates the step PSO straight into m_stepPSO: a failed rebuild keeps the old pipeline.
void WaterRippleSystem::CreatePipelines(IGraphicsDevice& gfx)
{
    if (const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::WaterRippleStep_CS))
    {
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_stepPSO))
            LOG_ERROR("WaterRippleSystem: step PSO creation failed");
    }
    else
    {
        LOG_ERROR("WaterRippleSystem: step shader lookup failed");
    }
}

void WaterRippleSystem::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void WaterRippleSystem::Shutdown()
{
    if (!m_gfx) return;
    for (auto& slot : m_slots)
        for (int k = 0; k < 2; ++k)
            if (slot.tex[k].IsValid())
                m_gfx->DestroyTexture(slot.tex[k]);
    m_cb.Destroy(*m_gfx);
    m_active.clear();
    m_queue.clear();
    m_prevBodyPos.clear();
    m_gfx = nullptr;
}

void WaterRippleSystem::AddRipple(float wx, float wz, float radius, float strength)
{
    m_queue.push_back({ wx, wz, radius, strength });
}

int WaterRippleSystem::SlotForEntity(Entity e)
{
    for (int i = 0; i < static_cast<int>(kMaxBodies); ++i)
        if (m_slots[i].valid && m_slots[i].owner == e) return i;
    for (int i = 0; i < static_cast<int>(kMaxBodies); ++i)
        if (m_slots[i].valid && m_slots[i].owner == NullEntity)
        {
            m_slots[i].owner      = e;
            m_slots[i].needsClear = true;   // wipe any prior owner's field / GPU garbage
            return i;
        }
    return -1;
}

void WaterRippleSystem::Sync(World& world, float dt, float camX, float camZ)
{
    if (!m_gfx) return;
    m_active.clear();
    std::vector<WaterComponent*> wcs;   // parallel to m_active (for publishing)

    bool touched[kMaxBodies] = {};
    // Ripple-enabled bodies that wanted a slot and did not get one. They still render
    // (WaterPass::kMaxWaterBodies is 16, twice this pool), just flat - on screen indistinguishable from
    // an unticked enableRipples - so they are counted and reported once below.
    uint32_t rippleSlotDenied = 0;

    // ---- Pass 1: pick active bodies, set water footprint + params ----
    auto* pool = world.GetPool<WaterComponent>();
    if (pool && pool->Size() > 0)
    {
        const auto& ents = pool->Entities();
        auto&       data = pool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (e == NullEntity) continue;
            WaterComponent& wc = data[i];
            wc.rippleFieldSRV = 0;                       // default: no ripples
            if (!wc.enabled || !wc.enableRipples || wc.worldSize <= 0.0f) continue;

            const int si = SlotForEntity(e);
            if (si < 0) { ++rippleSlotDenied; continue; }  // pool full this frame
            Slot& s = m_slots[si];
            touched[si] = true;

            const float half = wc.worldSize * 0.5f;
            s.waterOriginX = wc.worldCenter.x - half;
            s.waterOriginZ = wc.worldCenter.z - half;
            s.waterSize    = wc.worldSize;
            s.level        = wc.worldCenter.y;
            // Capped well under the 2D CFL limit (0.5) so the step stays stable.
            s.c2        = std::clamp(wc.ripplePropagation, 0.0f, 0.30f);
            s.damping   = std::clamp(wc.rippleDamping, 0.0f, 1.0f);
            s.amplitude = std::max(wc.rippleAmplitude, 0.0f);
            // Sim window - high-detail square that follows the action (<= body).
            s.areaSize  = std::clamp(wc.rippleAreaSize, 8.0f,
                                     std::max(wc.worldSize, 8.0f));
            s.raw.clear();
            s.points.clear();

            m_active.push_back(si);
            wcs.push_back(&wc);
        }
    }

    // ---- Pass 1b: mesh-driven water (ShadingModel::Water) ----
    // A plane + a Water-domain material, plus an optional WaterInteractionComponent for ripples; the
    // footprint comes from the entity transform, not from authored fields.
    // The ripple sim is an axis-aligned square with one scalar level, so a rotated / tilted surface is
    // not representable here (rendering is unaffected). The XZ extent comes from the transform's basis
    // lengths, exact for the common axis-aligned case.
    std::vector<const WaterInteractionComponent*> wics;
    if (auto* wiPool = world.GetPool<WaterInteractionComponent>())
    {
        const auto& ents = wiPool->Entities();
        auto&       data = wiPool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const Entity e = ents[i];
            if (e == NullEntity) continue;
            const WaterInteractionComponent& wi = data[i];
            wi.rippleBindlessIndex = 0;              // default: no field
            if (!wi.enabled) continue;

            // Must actually be water: a stray interaction component on a crate would otherwise burn a ~2 MiB
            // ripple slot.
            const MaterialComponent* mc = world.GetComponent<MaterialComponent>(e);
            if (!mc || !mc->IsWaterShadingModel()) continue;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(e);
            if (!gt) continue;

            const XMMATRIX m  = XMLoadFloat4x4(&gt->matrix);
            XMFLOAT3 axisX, axisZ, origin;
            XMStoreFloat3(&axisX,  m.r[0]);
            XMStoreFloat3(&axisZ,  m.r[2]);
            XMStoreFloat3(&origin, m.r[3]);
            const float extX = std::sqrt(axisX.x * axisX.x + axisX.z * axisX.z);
            const float extZ = std::sqrt(axisZ.x * axisZ.x + axisZ.z * axisZ.z);
            const float size = std::max(std::max(extX, extZ), 0.01f);
            if (size <= 0.0f) continue;

            const int si = SlotForEntity(e);
            if (si < 0) { ++rippleSlotDenied; continue; }
            Slot& s = m_slots[si];
            touched[si] = true;

            const float half = size * 0.5f;
            s.waterOriginX = origin.x - half;
            s.waterOriginZ = origin.z - half;
            s.waterSize    = size;
            s.level        = origin.y;
            s.c2        = std::clamp(wi.propagation, 0.0f, 0.30f);
            s.damping   = std::clamp(wi.damping, 0.0f, 1.0f);
            s.amplitude = std::max(wi.amplitude, 0.0f);
            s.areaSize  = std::clamp(wi.areaSize, 8.0f, std::max(size, 8.0f));
            s.raw.clear();
            s.points.clear();

            m_active.push_back(si);
            wcs.push_back(nullptr);      // parallel arrays: no WaterComponent
            wics.push_back(&wi);
        }
    }
    while (wics.size() < wcs.size()) wics.insert(wics.begin(), nullptr);

    // _ONCE: Sync runs every frame and a scene over the ripple budget stays over it, so an unlatched
    // line would repeat forever.
    ENGINE_CHECK_ONCE(rippleSlotDenied == 0,
        "WaterRippleSystem: no free ripple slot (pool is %u; a slot can also be "
        "unusable if its Init texture creation failed) — %u ripple-enabled "
        "water bodies get a FLAT surface this frame. This pool is smaller than "
        "WaterPass::kMaxWaterBodies on purpose (each slot is a %ux%u ping-pong "
        "field pair, ~2 MB); raise WaterRippleSystem::kMaxBodies only if that "
        "VRAM is worth it. Slots go to the first bodies in WaterComponent pool "
        "order, which is arbitrary — there is no distance/importance ranking.",
        kMaxBodies, rippleSlotDenied, kResolution, kResolution);

    // Free slots whose owner is gone / disabled ripples this frame.
    for (int i = 0; i < static_cast<int>(kMaxBodies); ++i)
        if (m_slots[i].owner != NullEntity && !touched[i])
            m_slots[i].owner = NullEntity;

    // ---- Pass 2: gather raw world-space sources ----
    // Script / gameplay (Engine.RippleAt).
    for (const QueuedRipple& q : m_queue)
        GatherWorldImpulse(q.x, q.z, q.radius, q.strength);
    m_queue.clear();
    // Physics - dynamic bodies crossing / skimming a water surface.
    ScanDynamicBodies(world, dt);

    // ---- Pass 3: place each window, build window-uv points, publish ----
    m_activeBodies.clear();
    for (size_t k = 0; k < m_active.size(); ++k)
    {
        Slot& s = m_slots[m_active[k]];
        PlaceWindowAndBuildPoints(s, camX, camZ);

        const int w = 1 - static_cast<int>(s.read);

        // WaterComponent path: publishes a raw descriptor handle, because Water.ps binds the field
        // through a root descriptor table.
        if (k < wcs.size() && wcs[k])
        {
            WaterComponent* wc   = wcs[k];
            wc->rippleFieldSRV   = s.srv[w];
            wc->rippleWinOriginX = s.winOriginX;
            wc->rippleWinOriginZ = s.winOriginZ;
            wc->rippleWinSize    = s.areaSize;
        }

        // Mesh-driven path: publishes the bindless index, because water_common.hlsli opens the field with
        // ResourceDescriptorHeap[]. The two are different numbers; srv here would index the heap with a
        // descriptor address and read garbage.
        const uint32_t bindless =
            (s.tex[w].handleId != ~0u) ? s.tex[w].handleId : 0u;
        if (k < wics.size() && wics[k])
        {
            const WaterInteractionComponent* wi = wics[k];
            wi->rippleBindlessIndex = bindless;
            wi->winOriginX = s.winOriginX;
            wi->winOriginZ = s.winOriginZ;
            wi->winSize    = s.areaSize;
        }

        ActiveBody ab;
        ab.winOriginX   = s.winOriginX;
        ab.winOriginZ   = s.winOriginZ;
        ab.winSize      = s.areaSize;
        ab.bodyOriginX  = s.waterOriginX;
        ab.bodyOriginZ  = s.waterOriginZ;
        ab.bodySize     = s.waterSize;
        ab.level        = s.level;
        ab.bindlessIndex = bindless;
        m_activeBodies.push_back(ab);
    }
}

void WaterRippleSystem::OnWorldClear()
{
    // Entity ids are recycled across worlds and slots are keyed by owner, so a
    // new entity reusing an old id would otherwise adopt the previous world's
    // height field. needsClear makes the next step zero the pair before use.
    for (Slot& s : m_slots)
    {
        s.owner      = NullEntity;
        s.needsClear = true;
        s.winInit    = false;
        s.raw.clear();
        s.points.clear();
    }
    m_active.clear();
    m_activeBodies.clear();
    m_queue.clear();
    // Stale positions would be diffed against the new world's bodies on frame 1
    // and spray phantom splashes wherever an id collided.
    m_prevBodyPos.clear();
}

void WaterRippleSystem::GatherWorldImpulse(float wx, float wz,
                                           float radius, float strength)
{
    for (int si : m_active)
    {
        Slot& s = m_slots[si];
        const float u = (wx - s.waterOriginX) / s.waterSize;
        const float v = (wz - s.waterOriginZ) / s.waterSize;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        AddRawSource(s, wx, wz, radius, strength * s.amplitude);
    }
}

void WaterRippleSystem::ScanDynamicBodies(World& world, float dt)
{
    if (m_active.empty()) { m_prevBodyPos.clear(); return; }
    const float invDt = (dt > 1e-4f) ? 1.0f / dt : 0.0f;

    std::unordered_map<Entity, XMFLOAT3> nextPrev;

    world.ForEach<RigidBodyComponent>([&](Entity e, RigidBodyComponent& rb)
    {
        if (rb.motion != RigidBodyComponent::Motion::Dynamic) return;
        const LocalTransform* lt = world.GetComponent<LocalTransform>(e);
        if (!lt) return;

        const XMFLOAT3 cur = lt->translation;
        XMFLOAT3 prev = cur;
        if (auto it = m_prevBodyPos.find(e); it != m_prevBodyPos.end()) prev = it->second;
        nextPrev[e] = cur;

        const float vy = (cur.y - prev.y) * invDt;
        const float vx = (cur.x - prev.x) * invDt;
        const float vz = (cur.z - prev.z) * invDt;
        const float horizSpeed = std::sqrt(vx * vx + vz * vz);

        for (int si : m_active)
        {
            Slot& s = m_slots[si];
            const float u = (cur.x - s.waterOriginX) / s.waterSize;
            const float v = (cur.z - s.waterOriginZ) / s.waterSize;
            if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;

            const bool crossed = (prev.y - s.level) * (cur.y - s.level) < 0.0f;
            if (crossed)
            {
                // Splash - a depression that rebounds, scaled by entry speed.
                const float impulse = -std::clamp(std::abs(vy) * 0.03f, 0.05f, 0.5f);
                AddRawSource(s, cur.x, cur.z, 1.4f, impulse * s.amplitude);
            }
            else if (std::abs(cur.y - s.level) < 0.45f && horizSpeed > 1.0f)
            {
                // Wake - a body skimming through the surface.
                const float impulse = std::clamp(horizSpeed * 0.006f, 0.0f, 0.15f);
                AddRawSource(s, cur.x, cur.z, 0.9f, impulse * s.amplitude);
            }
        }
    });

    m_prevBodyPos.swap(nextPrev);
}

// Places the slot's sim window (follows the latest out-of-window source, else the camera),
// computes the texel content shift since last frame, and converts the raw world sources into
// window-uv impulse points.
void WaterRippleSystem::PlaceWindowAndBuildPoints(Slot& s, float camX, float camZ)
{
    const float area       = std::max(s.areaSize, 1.0f);
    const float texelWorld = area / static_cast<float>(kResolution);
    const float halfA      = area * 0.5f;

    float cx = s.winCenterX, cz = s.winCenterZ;
    if (!s.winInit) { cx = camX; cz = camZ; s.winInit = true; }
    // Recenter onto the first source outside the current window, so a ripple triggered anywhere snaps
    // the window over it (a large jump scrolls the old field out - a clean slate there).
    for (const XMFLOAT4& r : s.raw)
        if (std::abs(r.x - cx) > halfA || std::abs(r.y - cz) > halfA)
        { cx = r.x; cz = r.y; break; }

    // Keep the window over the water body when the body is larger than it.
    if (s.waterSize > area)
    {
        cx = std::clamp(cx, s.waterOriginX + halfA, s.waterOriginX + s.waterSize - halfA);
        cz = std::clamp(cz, s.waterOriginZ + halfA, s.waterOriginZ + s.waterSize - halfA);
    }

    // Snap the centre to the texel grid so the content shift is an exact integer.
    const float cxs = std::round(cx / texelWorld) * texelWorld;
    const float czs = std::round(cz / texelWorld) * texelWorld;
    s.shiftX = static_cast<int>(std::lround((cxs - s.winCenterX) / texelWorld));
    s.shiftZ = static_cast<int>(std::lround((czs - s.winCenterZ) / texelWorld));
    s.winCenterX = cxs;  s.winCenterZ = czs;
    s.winOriginX = cxs - halfA;
    s.winOriginZ = czs - halfA;

    // Raw world sources -> window uv (drop any outside the placed window).
    s.points.clear();
    for (const XMFLOAT4& r : s.raw)
    {
        const float u = (r.x - s.winOriginX) / area;
        const float v = (r.y - s.winOriginZ) / area;
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) continue;
        if (s.points.size() >= kMaxRipplePoints) break;
        const float rUv = std::clamp(r.z / area, 2.0f / static_cast<float>(kResolution), 0.25f);
        s.points.push_back(XMFLOAT4{ u, v, rUv, r.w });
    }
}

void WaterRippleSystem::Execute(RHI::CommandList cl)
{
    if (!m_gfx || !m_stepPSO.IsValid() || m_active.empty() || !m_cb.IsValid()) return;
    auto& gfx = *m_gfx;

    // Upload per-body CBs (one 256 B slot per active body).
    CBFrame* frame = m_cb.Current(*m_gfx);
    if (!frame) return;
    for (size_t idx = 0; idx < m_active.size(); ++idx)
    {
        const Slot&   s  = m_slots[m_active[idx]];
        RippleStepCB& cb = (*frame)[idx];
        cb.resolution = kResolution;
        cb.c2         = s.c2;
        cb.damping    = s.damping;
        cb.clear      = s.needsClear ? 1u : 0u;
        cb.shiftX     = s.needsClear ? 0 : s.shiftX;
        cb.shiftZ     = s.needsClear ? 0 : s.shiftZ;
        cb.pointCount = s.needsClear ? 0u : static_cast<uint32_t>(
            std::min<size_t>(s.points.size(), kMaxRipplePoints));
        for (uint32_t p = 0; p < cb.pointCount; ++p) cb.points[p] = s.points[p];
    }

    gfx.BindComputePipelineState(m_stepPSO, cl);
    const RHI::GPUBuffer& cbBuf  = m_cb.CurrentBuffer(*m_gfx);
    const uint32_t        groups = (kResolution + 7u) / 8u;

    auto transition = [&](Slot& s, int k, RHI::ResourceState to)
    {
        if (s.state[k] == to) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(&s.tex[k], s.state[k], to), cl);
        s.state[k] = to;
    };

    for (size_t idx = 0; idx < m_active.size(); ++idx)
    {
        Slot&     s   = m_slots[m_active[idx]];
        const int r   = static_cast<int>(s.read);
        const int w   = 1 - r;
        const uint32_t cbOff = static_cast<uint32_t>(idx * sizeof(RippleStepCB));

        if (s.needsClear)
        {
            // Zero both ping-pong fields (cb.clear == 1: the shader writes 0 and ignores src) so a newly
            // claimed slot never flashes a prior owner's field or undefined GPU memory.
            transition(s, 0, RHI::ResourceState::UNORDERED_ACCESS);
            transition(s, 1, RHI::ResourceState::UNORDERED_ACCESS);
            gfx.SetComputeRootCBV(kCSCBSlot, cbBuf, cbOff, cl);
            gfx.SetComputeDescriptorTable(kCSU0Slot, s.uav[0], cl);  // unused by clear
            gfx.SetComputeDescriptorTable(kCSU1Slot, s.uav[0], cl);
            gfx.DispatchCompute(groups, groups, 1, cl);
            gfx.SetComputeDescriptorTable(kCSU1Slot, s.uav[1], cl);
            gfx.DispatchCompute(groups, groups, 1, cl);
            gfx.PushBarrier(RHI::GPUBarrier::Memory(&s.tex[0]), cl);
            gfx.PushBarrier(RHI::GPUBarrier::Memory(&s.tex[1]), cl);
            transition(s, w, RHI::ResourceState::SHADER_RESOURCE);   // the one Sync published
            s.read       = static_cast<uint32_t>(w);
            s.needsClear = false;
            continue;
        }

        transition(s, r, RHI::ResourceState::UNORDERED_ACCESS);  // src (read)
        transition(s, w, RHI::ResourceState::UNORDERED_ACCESS);  // dst (write)

        gfx.SetComputeRootCBV(kCSCBSlot, cbBuf, cbOff, cl);
        gfx.SetComputeDescriptorTable(kCSU0Slot, s.uav[r], cl);
        gfx.SetComputeDescriptorTable(kCSU1Slot, s.uav[w], cl);
        gfx.DispatchCompute(groups, groups, 1, cl);

        // Write must land, then hand the field to the water pass as an SRV.
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&s.tex[w]), cl);
        transition(s, w, RHI::ResourceState::SHADER_RESOURCE);

        s.read = static_cast<uint32_t>(w);   // ping-pong: newest is next frame's src
    }
}

// Stashes a raw world-space source on the slot; PlaceWindowAndBuildPoints converts it to field uv
// after the window (which may move this frame) is placed.
void WaterRippleSystem::AddRawSource(Slot& s, float wx, float wz,
                                     float worldRadius, float impulse)
{
    if (s.raw.size() >= kMaxRipplePoints) return;
    s.raw.push_back(DirectX::XMFLOAT4{ wx, wz, worldRadius, impulse });
}
