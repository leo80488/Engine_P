#include "Graphics/TrailSystem.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/ShaderReflection.h"
#include "ECS/ECS.h"
#include "ECS/TrailComponent.h"
#include "ECS/Components.h"               // MaterialComponent - material-driven trail look
#include "ECS/HierarchyComponents.h"
#include "ECS/MaterialReflectionSync.h"   // IsReservedCBuffer + Sync (material-driven trails)
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "System/HostPolicy.h"            // Sys::IsEditorHost - authoring-only reflection sync
#include "System/Log.h"

#include <map>

namespace
{
// Seed value when a var carries no //@default annotation (mirrors the
// material path's DefaultForType).
std::array<float, 4> DefaultForVarType(ShaderReflect::VarType t)
{
    using VT = ShaderReflect::VarType;
    if (t == VT::Float4) return { 1, 1, 1, 1 };
    if (t == VT::Float3) return { 1, 1, 1, 0 };
    return { 0, 0, 0, 0 };
}

uint8_t ComponentCountOfVarType(ShaderReflect::VarType t)
{
    using VT = ShaderReflect::VarType;
    switch (t)
    {
    case VT::Float:  case VT::Int:  case VT::UInt:  case VT::Bool: return 1;
    case VT::Float2: case VT::Int2: case VT::UInt2:                return 2;
    case VT::Float3: case VT::Int3: case VT::UInt3:                return 3;
    default:                                                       return 4;
    }
}

// Seed customParams + customParamMeta from the shader's b8 space0 cbuffer reflection
// (//@default/@min/@max/@label annotations). Runs once per component per shader-path change;
// entries from a previous shader are kept (same policy as the material system: switching
// back keeps the overrides).
void SyncTrailShaderParams(TrailComponent& tc, const ShaderReflect::Reflection& r)
{
    if (tc.lastSyncedShaderPath == tc.customShaderPath) return;
    tc.customParamMeta.clear();
    for (const auto& cb : r.cbuffers)
    {
        if (cb.bindPoint != 8u || cb.space != 0u) continue;
        if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
        for (const auto& v : cb.vars)
        {
            using VT = ShaderReflect::VarType;
            if (v.type == VT::Struct || v.type == VT::Float4x4 || v.elements > 0) continue;
            if (!v.used) continue;   // declared but never read: no widget, no seed

            const std::array<float, 4> defArr =
                v.hasDefault ? v.defaultValue : DefaultForVarType(v.type);
            const DirectX::XMFLOAT4 defVal{ defArr[0], defArr[1], defArr[2], defArr[3] };

            TrailComponent::ShaderParamMeta& meta = tc.customParamMeta[v.name];
            meta.defaultValue  = defVal;
            meta.minValue      = { v.minValue[0], v.minValue[1], v.minValue[2], v.minValue[3] };
            meta.maxValue      = { v.maxValue[0], v.maxValue[1], v.maxValue[2], v.maxValue[3] };
            meta.hasRange      = v.hasRange;
            meta.isTextureSlot = (v.name.rfind("Tex_", 0) == 0);
            meta.numComponents = ComponentCountOfVarType(v.type);
            meta.label         = v.label;

            if (meta.isTextureSlot)
            {
                // Texture slot: the packed value is a bindless index the SYSTEM provides,
                // so no user float param is seeded.
                tc.customTextures.try_emplace(v.name);
                continue;
            }

            if (!tc.customParams.count(v.name))
                tc.customParams.emplace(v.name, defVal);
        }
    }
    tc.lastSyncedShaderPath = tc.customShaderPath;
}
} // namespace

#include <algorithm>
#include <cstring>
#include <cmath>

void TrailSystem::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // ---- Segment pool (DEFAULT, SR+UAV) ----
    // Point-major records (see TrailSystem.h), zero-initialised so no fetch path (Catmull-Rom
    // controls, arc anchors, the CS's previous sample) can read heap garbage. Reads are
    // count-gated anyway; 512 KB of zeroes at init is cheap insurance.
    {
        const uint64_t recordCount =
            static_cast<uint64_t>(kMaxTrails) * kMaxSegmentsPerTrail * kMaxPointsPerTrail;
        std::vector<uint8_t> zero(static_cast<size_t>(recordCount * sizeof(TrailSegmentGPU)), 0);

        RHI::GPUBufferDesc d{};
        d.size       = recordCount * sizeof(TrailSegmentGPU);
        d.usage      = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailSegmentGPU);
        if (!gfx.CreateBuffer(d, m_segmentBuffer, zero.data()))
        {
            LOG_ERROR("TrailSystem: segment buffer creation failed");
            return;
        }
        m_segSrv = gfx.GetBufferSRVGpuHandle(m_segmentBuffer);
        m_segUav = gfx.GetBufferUAVGpuHandle(m_segmentBuffer);
    }

    // ---- Header pool (DEFAULT, SR+UAV) ----
    // MUST be zero-initialised (DEFAULT heap contents are undefined on allocation): the VS gates
    // on hdr.count and the update CS's ring math depends on hdr.head, so a garbage header makes
    // the CS walk bogus samples and the VS draw bogus segment pairs.
    {
        const size_t bytes = static_cast<size_t>(kMaxTrails) * sizeof(TrailHeaderGPU);
        std::vector<uint8_t> zero(bytes, 0);

        RHI::GPUBufferDesc d{};
        d.size       = bytes;
        d.usage      = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailHeaderGPU);
        if (!gfx.CreateBuffer(d, m_headerBuffer, zero.data()))
        {
            LOG_ERROR("TrailSystem: header buffer creation failed");
            return;
        }
        m_hdrSrv = gfx.GetBufferSRVGpuHandle(m_headerBuffer);
        m_hdrUav = gfx.GetBufferUAVGpuHandle(m_headerBuffer);
    }

    // ---- Append request buffers (UPLOAD, SR), one per frame in flight ----
    // Structured, so the update CS indexes gRequests[r] directly; stride =
    // sizeof(TrailAppendRequestGPU).
    {
        RHI::GPUBufferDesc d{};
        d.size       = static_cast<uint64_t>(kMaxRequestsPerFrame) * sizeof(TrailAppendRequestGPU);
        d.usage      = RHI::Usage::UPLOAD;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        d.stride     = sizeof(TrailAppendRequestGPU);
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(d, m_requestBuffer[i]))
            {
                m_requestMapped[i] = gfx.MapBuffer(m_requestBuffer[i]);
                m_requestSrv[i]    = gfx.GetBufferSRVGpuHandle(m_requestBuffer[i]);
            }
            if (!m_requestMapped[i])
                LOG_ERROR("TrailSystem: request buffer[%u] map failed", i);
        }
    }

    // ---- Per-frame globals CB (UPLOAD), one per frame in flight ----
    if (!m_systemCB.Create(gfx, "TrailSystem.SystemCB"))
        LOG_ERROR("TrailSystem: system CB create failed");

    // Shader bucket 0 = engine default PS (empty path).
    if (m_shaderBucketPaths.empty())
        m_shaderBucketPaths.emplace_back();

    // b8 custom-shader param ring: kMaxTrails x 256 B-aligned blocks per frame is 16 KB;
    // 32 KB leaves headroom for larger param cbuffers.
    if (!m_paramRing.Init(gfx, 32 * 1024))
        LOG_ERROR("TrailSystem: param ring init failed (custom trail shaders lose b8 params)");

    LOG_SUCCESS("TrailSystem: initialised (%u trails ? %u samples ? %u pts ? %zu B = %zu KB)",
                kMaxTrails, kMaxSegmentsPerTrail, kMaxPointsPerTrail, sizeof(TrailSegmentGPU),
                (kMaxTrails * kMaxSegmentsPerTrail * kMaxPointsPerTrail * sizeof(TrailSegmentGPU)) / 1024);
}

void TrailSystem::Shutdown(IGraphicsDevice& gfx)
{
    m_paramRing.Shutdown();
    m_systemCB.Destroy(gfx);
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_requestMapped[i])  { gfx.UnmapBuffer(m_requestBuffer[i]); m_requestMapped[i] = nullptr; }
        if (m_requestBuffer[i].IsValid()) gfx.DestroyBuffer(m_requestBuffer[i]);
        m_requestSrv[i] = 0;
    }
    if (m_headerBuffer.IsValid())  gfx.DestroyBuffer(m_headerBuffer);
    if (m_segmentBuffer.IsValid()) gfx.DestroyBuffer(m_segmentBuffer);
    m_gfx = nullptr;
}

const RHI::GPUBuffer& TrailSystem::GetRequestBuffer(IGraphicsDevice& gfx) const
{
    return m_requestBuffer[gfx.GetFrameIndex()];
}

void TrailSystem::OnWorldClear(IGraphicsDevice& gfx)
{
    // TextureSystem::Release defers the GPU destroy to its next Tick, so this
    // is safe to call mid-frame during teardown (same as ParticleSystem).
    if (m_texSys)
    {
        for (auto& [_e, entry] : m_trailTexCache)
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, gfx);
        for (auto& [_e, slots] : m_customTexCache)
            for (auto& [_n, entry] : slots)
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, gfx);
    }
    m_trailTexCache.clear();
    m_customTexCache.clear();
}

uint32_t TrailSystem::AllocateSlot()
{
    for (uint32_t i = 0; i < kMaxTrails; ++i)
    {
        const uint64_t bit = 1ULL << i;
        if ((m_slotUsedMask & bit) == 0)
        {
            m_slotUsedMask |= bit;
            if (i + 1 > m_maxActiveSlot) m_maxActiveSlot = i + 1;
            return i;
        }
    }
    return ~0u;
}

void TrailSystem::ReleaseSlot(uint32_t slot)
{
    if (slot >= kMaxTrails) return;
    m_slotUsedMask &= ~(1ULL << slot);
}

void TrailSystem::UpdateSystemCB(float dt)
{
    if (!m_gfx) return;
    auto* slot = m_systemCB.Upload(*m_gfx);
    if (!slot) return;
    TrailSystemCB cb{};
    cb.deltaTime    = dt;
    cb.requestCount = m_requestCountThisFrame;
    cb.maxSegments  = kMaxSegmentsPerTrail;
    cb.maxTrails    = kMaxTrails;
    *slot = cb;
}

void TrailSystem::CollectTrails(World& world, float dt)
{
    m_requestCountThisFrame = 0;
    if (!m_gfx) return;
    const uint32_t frameSlot = m_gfx->GetUploadSlot();
    if (frameSlot >= kFrameCount || !m_requestMapped[frameSlot]) return;

    // Custom-shader bucket id for a path ("" = 0 = default PS). Ids are session-stable and
    // ride in renderFlags bits 8-15 (<= 255 paths).
    auto shaderBucketOf = [&](const std::string& path) -> uint32_t
    {
        if (path.empty()) return 0;
        auto it = m_shaderBucketByPath.find(path);
        if (it != m_shaderBucketByPath.end()) return it->second;
        if (m_shaderBucketPaths.size() > 255)
        {
            LOG_WARNING("TrailSystem: >255 distinct custom shaders ??'%s' falls back to default",
                        path.c_str());
            return 0;
        }
        const uint32_t id = static_cast<uint32_t>(m_shaderBucketPaths.size());
        m_shaderBucketPaths.push_back(path);
        m_shaderBucketByPath.emplace(path, id);
        return id;
    };

    // Full header renderFlags for a component: bits 0-3 = TrailRenderFlag (arc UV, additive,
    // TAA exclude, world-anchored UV), bits 8-15 = shader bucket. @p mc (nullable) is the
    // entity's look material (TrailComponent = SHAPE, MaterialComponent = LOOK). The material's
    // custom shader (when enabled) picks the PS; its blend mode wins only when it is a
    // forward-transparent mode. Opaque (the material default) means "unset" for a ribbon and
    // falls back to tc.blendMode, so adding an empty Material never changes a trail's look.
    auto renderFlagsOf = [&](const TrailComponent& tc, const MaterialComponent* mc) -> uint32_t
    {
        const bool mcFx = mc && mc->useCustomShader && !mc->customShaderPath.empty();
        const std::string& fxPath = mcFx ? mc->customShaderPath : tc.customShaderPath;
        // The trail PSO blend is ALWAYS premultiplied; "additive" is a shader-side output mode
        // (a = 0). Material Alpha / Premultiplied / Multiply all take the alpha path; only Additive
        // selects the additive output.
        bool additive = (tc.blendMode == 1);
        if (mc)
        {
            const BlendMode bm = mc->GetBlendMode();
            // Masked is a GBuffer surface mode, not a ribbon blend: like Opaque it keeps tc.blendMode.
            if (IsTransparentBlendMode(bm)) additive = (bm == BlendMode::Additive);
        }
        return (tc.uvMode >= 1 ? TrailRenderFlag_ArcLengthUV : 0u)
             | (tc.uvMode == 2 ? TrailRenderFlag_UVWorldAnchor : 0u)
             | (additive ? TrailRenderFlag_Additive : 0u)
             | (tc.excludeFromTAA ? TrailRenderFlag_TAAExclude : 0u)
             | (shaderBucketOf(fxPath) << 8);
    };

    // FNV-1a over every visual param that lives in the header; drives the RefreshParams path.
    // Without it an idle (gated / disabled) trail's edited params never reach the GPU, and its
    // draw bucket (computed from the component) would disagree with the header's, so the trail
    // would vanish from every draw.
    auto paramHashOf = [&](const TrailComponent& tc, const MaterialComponent* mc,
                           uint32_t pc) -> uint32_t
    {
        uint32_t h = 2166136261u;
        auto mix = [&h](const void* p, size_t n)
        {
            const uint8_t* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
        };
        mix(&tc.width, 4); mix(&tc.maxAge, 4);
        mix(&tc.startColor, 16); mix(&tc.endColor, 16);
        mix(&pc, 4); mix(&tc.textureBindlessIdx, 4);
        const uint32_t rf = renderFlagsOf(tc, mc); mix(&rf, 4);
        mix(&tc.subdivisions, 4); mix(&tc.uvTileLength, 4); mix(&tc.hdrIntensity, 4);
        return h ? h : 1u;   // 0 = "unset" sentinel in lastParamHash
    };

    // Writes one TrailAppendRequestGPU into this frame's upload ring; @p flags selects
    // reset / reset-only / refresh (TrailRequestFlag). Drops the request once
    // kMaxRequestsPerFrame is reached. Sample appends stop at kMaxAppendsPerFrame (the trail
    // loop's guard), so the freed-slot ResetOnly sweep always fits (budget proof at
    // kMaxRequestsPerFrame). @p pts = @p ptCount world-space positions (nullptr for reset-only
    // and refresh requests); @p ageOffset staggers CPU substeps (0 = the frame's newest sample).
    auto emitRequest = [&](uint32_t slot, const DirectX::XMFLOAT3* pts, uint32_t ptCount,
                           const TrailComponent& tc, const MaterialComponent* mc,
                           uint32_t flags, float ageOffset)
    {
        if (m_requestCountThisFrame >= kMaxRequestsPerFrame) return;
        TrailAppendRequestGPU* req = reinterpret_cast<TrailAppendRequestGPU*>(
            static_cast<uint8_t*>(m_requestMapped[frameSlot])
            + m_requestCountThisFrame * sizeof(TrailAppendRequestGPU));
        const DirectX::XMFLOAT3 zero{ 0.f, 0.f, 0.f };
        auto pt = [&](uint32_t p) { return (pts && p < ptCount) ? pts[p] : zero; };
        req->position   = pt(0);
        req->dt         = dt;
        req->trailSlot  = slot;
        req->width      = tc.width;
        req->maxAge     = tc.maxAge;
        req->flags      = flags;
        req->startColor = tc.startColor;
        req->endColor   = tc.endColor;
        const DirectX::XMFLOAT3 p1 = pt(1), p2 = pt(2), p3 = pt(3);
        req->position1  = { p1.x, p1.y, p1.z, tc.uvTileLength };
        req->position2  = { p2.x, p2.y, p2.z, tc.hdrIntensity };
        req->position3  = { p3.x, p3.y, p3.z, ageOffset };
        req->pointCount         = ptCount;
        req->textureBindlessIdx = (tc.textureBindlessIdx >= 0)
                                    ? static_cast<uint32_t>(tc.textureBindlessIdx) : 0xFFFFFFFFu;
        req->renderFlags        = renderFlagsOf(tc, mc);
        uint32_t subdiv = tc.subdivisions;
        if (subdiv < 1) subdiv = 1;
        if (subdiv > kMaxSubdivisions) subdiv = kMaxSubdivisions;
        req->subdivisions       = subdiv;
        ++m_requestCountThisFrame;
    };

    // The trail loop iterates the TrailComponent pool directly (Entities() / Data()).
    auto* pTrail   = world.GetPool<TrailComponent>();
    auto* pGlobal  = world.GetPool<GlobalTransform>();
    auto* pMat     = world.GetPool<MaterialComponent>();  // material-driven look
    auto* pMeshRef = world.GetPool<MeshLibRef>();         // domain gate: a mesh entity's material is not a trail look

    // ---- Texture cache sweep + resolve ----
    // The same entity-keyed Acquire/Release cache as ParticleSystem's. Swept here rather than via
    // destroy listeners, like the slot reconcile below: an entry whose owner left the pool is
    // released.
    if (m_texSys && m_gfx)
    {
        for (auto it = m_trailTexCache.begin(); it != m_trailTexCache.end(); )
        {
            if (!pTrail || !pTrail->Get(it->first))
            {
                if (it->second.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(it->second.handle, *m_gfx);
                it = m_trailTexCache.erase(it);
            }
            else
                ++it;
        }
        for (auto it = m_customTexCache.begin(); it != m_customTexCache.end(); )
        {
            if (!pTrail || !pTrail->Get(it->first))
            {
                for (auto& [_n, entry] : it->second)
                    if (entry.handle != Resource::kInvalidTextureHandle)
                        m_texSys->Release(entry.handle, *m_gfx);
                it = m_customTexCache.erase(it);
            }
            else
                ++it;
        }
    }
    // Same owner-vanished sweep for the dynamic-shader resolve cache (ids and reflection
    // pointers hold no resources: just drop the rows).
    for (auto it = m_trailReflCache.begin(); it != m_trailReflCache.end(); )
    {
        if (!pTrail || !pTrail->Get(it->first)) it = m_trailReflCache.erase(it);
        else                                    ++it;
    }

    // Resolve the trail-mask texture path -> bindless index (written back to
    // tc.textureBindlessIdx). @p path is the EFFECTIVE mask path: the material's Base Color
    // slot when a material drives the look, else tc.texturePath.
    auto resolveTexture = [&](Entity e, TrailComponent& tc, const std::string& path)
    {
        if (!m_texSys || !m_resMgr || !m_gfx) return;
        auto [cacheIt, _ins] = m_trailTexCache.try_emplace(e);
        auto& entry = cacheIt->second;
        if (path != entry.path)
        {
            if (entry.handle != Resource::kInvalidTextureHandle)
                m_texSys->Release(entry.handle, *m_gfx);
            entry.path   = path;
            entry.handle = path.empty()
                ? Resource::kInvalidTextureHandle
                : m_texSys->Acquire(path, *m_resMgr, *m_gfx);
            tc.textureBindlessIdx = -1;
        }
        if (tc.textureBindlessIdx < 0
            && entry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(entry.handle))
            if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                tc.textureBindlessIdx = static_cast<int32_t>(tex->handleId);
    };

    // ---- Reconcile slot ownership from LIVE components ----
    // Nothing calls ReleaseSlot when a TrailComponent is destroyed (LifetimeSystem removes
    // notify-spawned VFX trails), so the used-slot mask is rebuilt from the live pool each
    // frame: a slot whose owner vanished is freed here and gets a reset-only request below so
    // its stale ribbon stops drawing.
    uint64_t liveMask = 0;
    if (pTrail)
    {
        auto& data = pTrail->Data();
        for (const TrailComponent& tc : data)
            if (tc.trailSlot != 0xFFFFFFFFu && tc.trailSlot < kMaxTrails)
                liveMask |= (1ULL << tc.trailSlot);
    }
    const uint64_t freed = m_slotUsedMask & ~liveMask;
    m_slotUsedMask  = liveMask;
    m_maxActiveSlot = 0;                  // recomputed over live + new slots below

    uint64_t allocatedThisFrame = 0;
    uint64_t touchedMask        = 0;      // slots that got an append or reset request

    m_frameMaxPointCount = 1;
    m_frameMaxSubdiv     = 1;
    m_paramRing.BeginFrame(frameSlot);   // own ring: the renderer's material ring rotates after CollectTrails

    // Per-frame draw buckets: key = (shaderBucket << 1) | taaVariant. Custom buckets collect
    // one SlotDraw per trail (each needs its own b8 CBV). Collected flat and sorted once, with
    // no per-frame map or per-key vector allocations; s_seenKeys makes the default bucket
    // appear with an empty slotDraws list, and buckets come out in ascending key order.
    static thread_local std::vector<uint32_t>                       t_seenKeys;
    static thread_local std::vector<std::pair<uint32_t, SlotDraw>>  t_slotDraws;
    t_seenKeys.clear();
    t_slotDraws.clear();

    // Resolve every "Tex_*" slot's path -> bindless index (entity + name keyed Acquire/Release
    // cache, same policy as the main trail texture). Paths come from the material's
    // customTextures when it drives the look, else from tc.customTextures; the resolved
    // indices land in tc.customTexIndices (runtime-only, per entity) either way.
    auto resolveCustomTextures = [&](Entity e, TrailComponent& tc, const MaterialComponent* mc)
    {
        if (!m_texSys || !m_resMgr || !m_gfx) return;
        if (mc ? mc->customTextures.empty() : tc.customTextures.empty()) return;
        auto& slots = m_customTexCache[e];
        auto resolveOne = [&](const std::string& name, const std::string& path)
        {
            auto& entry = slots[name];
            if (path != entry.path)
            {
                if (entry.handle != Resource::kInvalidTextureHandle)
                    m_texSys->Release(entry.handle, *m_gfx);
                entry.path   = path;
                entry.handle = path.empty()
                    ? Resource::kInvalidTextureHandle
                    : m_texSys->Acquire(path, *m_resMgr, *m_gfx);
                tc.customTexIndices[name] = -1;
            }
            auto idxIt = tc.customTexIndices.find(name);
            const bool unresolved = (idxIt == tc.customTexIndices.end() || idxIt->second < 0);
            if (unresolved
                && entry.handle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(entry.handle))
                if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                    tc.customTexIndices[name] = static_cast<int32_t>(tex->handleId);
        };
        if (mc)
            for (const auto& [name, texMap] : mc->customTextures)
                resolveOne(name, texMap.name);
        else
            for (const auto& [name, path] : tc.customTextures)
                resolveOne(name, path);
    };

    // Register, reflect and Sync one custom trail's PS. SLOT-INDEPENDENT on purpose: this is
    // the ONLY path that fills a trail-look material's customParams / customTextures /
    // customParamMeta, and the material Inspector shows its Custom Parameters / Custom Textures
    // sections only when those are non-empty. trailSlot is CPU-only, starts at 0xFFFFFFFF on
    // every load and is leased after the !tc.enabled early-out, so a slot-gated sync would hide
    // the parameters of a trail never enabled since load.
    //
    // @p resolveTextures = false for the authoring-only call: the Inspector reads texture SLOTS
    // off the material, not tc.customTexIndices, so a slot-less trail need not Acquire() its
    // textures just to show the fields. Returns the reflection (null = no shader / not
    // compiled yet).
    auto syncCustomShaderRefl = [&](Entity e, TrailComponent& tc,
                                    MaterialComponent* mc,
                                    bool resolveTextures)
        -> const ShaderReflect::Reflection*
    {
        if (!m_shaderLib) return nullptr;
        const std::string& fxPath = mc ? mc->customShaderPath : tc.customShaderPath;
        if (fxPath.empty()) return nullptr;

        // Entity-keyed resolve cache: the id never changes for a given path and the reflection
        // pointer is stable until the library's reload epoch bumps, so RegisterDynamic (string
        // key + global mutex) runs only when the path changes.
        uint32_t fxHash = 2166136261u;
        for (const char c : fxPath) { fxHash ^= uint8_t(c); fxHash *= 16777619u; }
        const uint32_t epoch = m_shaderLib->GetReloadEpoch();

        TrailReflCache& rc = m_trailReflCache[e];
        if (rc.pathHash != fxHash)
        {
            rc.pathHash = fxHash;
            rc.dynId    = m_shaderLib->RegisterDynamic(
                fxPath.c_str(), RHI::ShaderStage::PS, "main");
            rc.refl      = nullptr;
            rc.reflEpoch = 0;
        }
        if (rc.dynId == ShaderLibrary::kInvalidDynShaderID) return nullptr;
        if (rc.reflEpoch != epoch) { rc.refl = nullptr; rc.reflEpoch = epoch; }
        if (!rc.refl) rc.refl = m_shaderLib->GetDynamicReflection(rc.dynId);
        const ShaderReflect::Reflection* refl = rc.refl;
        if (!refl) return nullptr;

        if (mc)
        {
            // Epoch / path-stamped: the string-map walk runs once, not per frame
            // (see MaterialReflectionSync::StageSynced).
            if (!MaterialReflectionSync::StageSynced(*mc, epoch, /*vs*/ false))
            {
                MaterialReflectionSync::Sync(*mc, *refl);
                MaterialReflectionSync::MarkStageSynced(*mc, epoch, /*vs*/ false);
            }
            // Trail look materials are PS-only: clear customParamsLegacyFormat here. Mesh materials
            // get it cleared by Renderer::ResolveCustomVSID after both stages sync, but a mesh-less
            // trail entity never runs that path, and a set flag re-runs the sentinel migration (and
            // its log line) EVERY frame.
            mc->customParamsLegacyFormat = false;
        }
        else    SyncTrailShaderParams(tc, *refl);
        if (resolveTextures) resolveCustomTextures(e, tc, mc);
        return refl;
    };

    // Reflect + pack one custom trail's b8 params. Returns the SlotDraw for its bucket.
    // ready = false: the shader is not compiled / reflected yet (first frame), so the draw is
    // skipped rather than binding a b8 the shader may read. When a material drives the look
    // (@p mc != null), params / textures / meta live on the MaterialComponent and sync through
    // the material path (MaterialReflectionSync::Sync: same annotations, same Tex_ handling).
    auto packCustomParams = [&](Entity e, TrailComponent& tc,
                                MaterialComponent* mc) -> SlotDraw
    {
        SlotDraw sd{ tc.trailSlot, 0, false };
        const ShaderReflect::Reflection* refl =
            syncCustomShaderRefl(e, tc, mc, /*resolveTextures=*/true);
        if (!refl) return sd;

        // cbuffer selection: register b8 space0 with engine-reserved names rejected, the same
        // rule as the material path's custom CBV pack.
        uint32_t cbSize = 0;
        for (const auto& cb : refl->cbuffers)
            if (cb.bindPoint == 8u && cb.space == 0u &&
                !MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str()) &&
                cb.sizeBytes > cbSize)
                cbSize = cb.sizeBytes;

        if (cbSize == 0) { sd.ready = true; return sd; }   // no b8 declared: draw without a bind

        auto slice = m_paramRing.Allocate(cbSize);
        if (!slice.cpu) return sd;             // ring full: not ready (an unbound b8 read faults)
        sd.ready = true;
        std::memset(slice.cpu, 0, cbSize);
        for (const auto& cb : refl->cbuffers)
        {
            if (cb.bindPoint != 8u || cb.space != 0u) continue;
            if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
            for (const auto& v : cb.vars)
            {
                if (v.name == "Engine_TrailAge")
                {
                    if (v.offset + sizeof(float) <= cbSize)
                        std::memcpy(static_cast<uint8_t*>(slice.cpu) + v.offset,
                                    &tc.emitterAge, sizeof(float));
                    continue;
                }
                // Texture slots ("Tex_*"): the SYSTEM writes the bindless index as a float VALUE (exact
                // up to 2^24; -1 = unbound, the shader falls back to its procedural path). Resolved
                // indices live in tc.customTexIndices for BOTH look sources.
                const bool isTexSlot = mc
                    ? (v.name.rfind("Tex_", 0) == 0)
                    : [&]{ auto mit = tc.customParamMeta.find(v.name);
                           return mit != tc.customParamMeta.end() &&
                                  mit->second.isTextureSlot; }();
                if (isTexSlot)
                {
                    float idx = -1.0f;
                    if (auto ti = tc.customTexIndices.find(v.name);
                        ti != tc.customTexIndices.end())
                        idx = static_cast<float>(ti->second);
                    if (v.offset + sizeof(float) <= cbSize)
                        std::memcpy(static_cast<uint8_t*>(slice.cpu) + v.offset,
                                    &idx, sizeof(float));
                    continue;
                }

                // Value: user override -> synced default -> skip (left zero).
                float srcVal[4];
                const float* src = nullptr;
                if (mc)
                {
                    if (auto it = mc->customParams.find(v.name); it != mc->customParams.end())
                        src = it->second.data();
                    else if (auto mit = mc->customParamMeta.find(v.name);
                             mit != mc->customParamMeta.end())
                        src = mit->second.defaultValue.data();
                }
                else
                {
                    if (auto it = tc.customParams.find(v.name); it != tc.customParams.end())
                    { std::memcpy(srcVal, &it->second, 16); src = srcVal; }
                    else if (auto mit = tc.customParamMeta.find(v.name);
                             mit != tc.customParamMeta.end())
                    { std::memcpy(srcVal, &mit->second.defaultValue, 16); src = srcVal; }
                }
                if (!src) continue;
                const uint32_t copy = (v.size < 16u) ? v.size : 16u;
                if (v.offset + copy <= cbSize)
                    std::memcpy(static_cast<uint8_t*>(slice.cpu) + v.offset, src, copy);
            }
        }
        sd.paramVA = slice.gpu;
        return sd;
    };

    if (pTrail)
    {
        using namespace DirectX;

        auto& ents = pTrail->Entities();
        auto& data = pTrail->Data();
        const size_t n = data.size();

        for (size_t i = 0; i < n && m_requestCountThisFrame < kMaxAppendsPerFrame; ++i)
        {
            const Entity e = ents[i];
            TrailComponent& tc = data[i];
            if (tc.restartRequested || tc.trailSlot == 0xFFFFFFFFu)
                tc.emitterAge = 0.0f;
            else
                tc.emitterAge += (std::max)(dt, 0.0f);

            // ---- One-time look migration (shape / shading split) ----
            // A trail still carrying LOOK fields on its TrailComponent (texture / blend / custom
            // shader + params) and no material gets a MaterialComponent synthesized from them, then
            // those fields are CLEARED: Material is the single shading owner. Covers old scenes, old
            // .ivfx spawns and hand-built entities alike. Mesh-bearing entities are skipped (their
            // material belongs to the mesh; the TrailComponent look fields still render those trails).
            const bool hasLegacyLook = !tc.texturePath.empty() || tc.blendMode != 0 ||
                                       !tc.customShaderPath.empty();
            if (hasLegacyLook && !(pMeshRef && pMeshRef->Get(e)) &&
                !world.GetComponent<MaterialComponent>(e))
            {
                MaterialComponent mig;
                mig.userBlendMode = (tc.blendMode == 1) ? BlendMode::Additive
                                                        : BlendMode::Alpha;
                if (!tc.texturePath.empty())
                    mig.textures[MaterialComponent::BASECOLORMAP].name = tc.texturePath;
                if (!tc.customShaderPath.empty())
                {
                    mig.useCustomShader    = true;
                    mig.customShaderPath   = tc.customShaderPath;
                    mig.customShadingModel = ShadingModel::Unlit;   // trail PSes are unlit
                    for (const auto& [n, v4] : tc.customParams)
                        mig.customParams[n] = { v4.x, v4.y, v4.z, v4.w };
                    for (const auto& [n, p] : tc.customTextures)
                        if (!p.empty())
                        {
                            MaterialComponent::TextureMap t; t.name = p;
                            mig.customTextures.emplace(n, std::move(t));
                        }
                }
                mig.SetDirty();
                world.AddComponent<MaterialComponent>(e, std::move(mig));
                pMat = world.GetPool<MaterialComponent>();   // pool may have just been created
                tc.texturePath.clear();
                tc.blendMode = 0;
                tc.customShaderPath.clear();
                tc.customParams.clear();
                tc.customTextures.clear();
                tc.customParamMeta.clear();
                tc.customTexIndices.clear();
                tc.lastSyncedShaderPath.clear();
                tc.textureBindlessIdx = -1;
                LOG_INFO("TrailSystem: migrated legacy trail look on entity %u into its MaterialComponent", e);
            }

            // TrailComponent = SHAPE (width / points / uv / sampling / gradient), MaterialComponent =
            // LOOK (blend, mask texture, custom shader + params / textures). matMc = the look material;
            // fxMc = the same, only when its custom shader drives the PS and params. tc's own look
            // fields stay the no-material fallback. Domain gate: a material on a MESH-bearing entity
            // belongs to the mesh draw (dissolve / NPR surface shaders would otherwise hijack the
            // trail PS and break its PSO).
            MaterialComponent* matMc = pMat ? pMat->Get(e) : nullptr;
            if (matMc && pMeshRef && pMeshRef->Get(e))
                matMc = nullptr;
            // Decal-domain materials author a projected decal (DecalComponent consumes them in the
            // Renderer), never a trail look.
            if (matMc && matMc->IsDecalDomain())
                matMc = nullptr;
            // Water-shading-model materials author a water surface, never a trail look
            // (their PS emits 4 render targets; a trail PSO declares 1).
            if (matMc && matMc->IsWaterShadingModel())
                matMc = nullptr;
            MaterialComponent* fxMc = (matMc && matMc->useCustomShader &&
                                       !matMc->customShaderPath.empty()) ? matMc : nullptr;

            // ---- Tuning adoption (material added by hand) ----
            // The no-material migration above never runs when a material already exists, but a trail
            // tuned before the shape / shading split still carries its values in tc.customParams /
            // customTextures / texturePath. Sync seeds the MATERIAL's params at shader DEFAULTS, which
            // would silently discard that tuning (e.g. the mask's Trail Scale U resets to 1 and the
            // pattern repeats per segment instead of stretching over the ribbon). Adopt tc's values into
            // every mc param still at its reflected default (params re-tuned ON the material differ
            // from the default and are kept), fill unbound texture slots and the mask, then clear the
            // tc bags. Gated on non-empty meta: adoption needs the CURRENT shader's reflected defaults
            // to compare against.
            if (fxMc && !fxMc->customParamMeta.empty() &&
                (!tc.customParams.empty() || !tc.customTextures.empty() ||
                 !tc.texturePath.empty() || tc.blendMode != 0))
            {
                for (const auto& [n, v4] : tc.customParams)
                {
                    auto mIt = fxMc->customParamMeta.find(n);
                    if (mIt == fxMc->customParamMeta.end()) continue;   // not in the current shader
                    auto pIt = fxMc->customParams.find(n);
                    const bool atDefault = (pIt == fxMc->customParams.end()) ||
                        (pIt->second == mIt->second.defaultValue);
                    if (atDefault)
                        fxMc->customParams[n] = { v4.x, v4.y, v4.z, v4.w };
                }
                for (const auto& [n, p] : tc.customTextures)
                {
                    if (p.empty()) continue;
                    auto tIt = fxMc->customTextures.find(n);
                    if (tIt == fxMc->customTextures.end())
                        { MaterialComponent::TextureMap t; t.name = p;
                          fxMc->customTextures.emplace(n, std::move(t)); }
                    else if (tIt->second.name.empty())
                        tIt->second.name = p;
                }
                static const std::string kDefaultBC =
                    MaterialComponent{}.textures[MaterialComponent::BASECOLORMAP].name;
                if (!tc.texturePath.empty())
                {
                    auto& bc = fxMc->textures[MaterialComponent::BASECOLORMAP].name;
                    if (bc.empty() || bc == kDefaultBC) bc = tc.texturePath;
                }
                if (tc.blendMode == 1 && fxMc->GetBlendMode() == BlendMode::Opaque)
                    fxMc->userBlendMode = BlendMode::Additive;
                fxMc->SetDirty();
                tc.texturePath.clear();
                tc.blendMode = 0;
                tc.customShaderPath.clear();
                tc.customParams.clear();
                tc.customTextures.clear();
                tc.customParamMeta.clear();
                tc.customTexIndices.clear();
                tc.lastSyncedShaderPath.clear();
                tc.textureBindlessIdx = -1;
                LOG_INFO("TrailSystem: adopted legacy trail tuning on entity %u into its Material", e);
            }

            uint32_t pc = tc.pointCount;
            if (pc < 1) pc = 1;
            if (pc > kMaxPointsPerTrail) pc = kMaxPointsPerTrail;

            // AUTHORING PATH: harvest the custom shader's reflection even when this trail owns no slot
            // yet (never enabled since load) or is disabled; otherwise the material Inspector shows no
            // parameters and no texture slots until the trail runs. Idempotent and cheap:
            // RegisterDynamic dedups by path and Sync is add-only, so packCustomParams below repeats it
            // for free. Editor host only: the Inspector is the sole consumer, and packCustomParams
            // already syncs every trail that renders.
            if (fxMc && Sys::IsEditorHost())
                syncCustomShaderRefl(e, tc, fxMc, /*resolveTextures=*/false);

            // Disabled trails keep their leased slot and frozen ribbon: count them toward the draw
            // range AND the per-frame instance-decode budget (their ribbon still draws), but emit no
            // sample.
            if (tc.trailSlot != 0xFFFFFFFFu && tc.trailSlot < kMaxTrails)
            {
                if (tc.trailSlot + 1 > m_maxActiveSlot) m_maxActiveSlot = tc.trailSlot + 1;
                if (pc > m_frameMaxPointCount) m_frameMaxPointCount = pc;
                if (pc >= 2)
                {
                    uint32_t sd = tc.subdivisions;
                    if (sd < 1) sd = 1;
                    if (sd > kMaxSubdivisions) sd = kMaxSubdivisions;
                    if (sd > m_frameMaxSubdiv) m_frameMaxSubdiv = sd;
                }
                // Draw-bucket occupancy: valid-slot components only. Custom buckets get one SlotDraw per
                // trail (own b8 param CBV), packed here even for disabled trails (frozen ribbons draw).
                const uint32_t bucketId = shaderBucketOf(
                    fxMc ? fxMc->customShaderPath : tc.customShaderPath);
                const uint32_t bkey = (bucketId << 1) | (tc.excludeFromTAA ? 1u : 0u);
                t_seenKeys.push_back(bkey);   // key exists even with zero SlotDraws
                if (bucketId != 0)
                    t_slotDraws.emplace_back(bkey, packCustomParams(e, tc, fxMc));
            }

            // Effective trail-mask texture: the material's Base Color slot wins only when the author
            // DELIBERATELY set it. The constructor seeds every slot with the engine default texture, so
            // compare against that seed (a plain .empty() check would ALWAYS divert the mask to
            // Default_BaseColor.itex and silently kill tc.texturePath).
            static const std::string kDefaultBaseColor =
                MaterialComponent{}.textures[MaterialComponent::BASECOLORMAP].name;
            const std::string* maskPath = &tc.texturePath;
            if (matMc)
            {
                const std::string& bc = matMc->textures[MaterialComponent::BASECOLORMAP].name;
                if (!bc.empty() && bc != kDefaultBaseColor)
                    maskPath = &bc;
            }
            resolveTexture(e, tc, *maskPath);
            const uint32_t hash = paramHashOf(tc, matMc, pc);

            // Disabled trails keep drawing their frozen ribbon, so param edits (color / shader /
            // texture) still need to reach the header.
            if (!tc.enabled)
            {
                if (tc.trailSlot != 0xFFFFFFFFu && tc.trailSlot < kMaxTrails &&
                    hash != tc.lastParamHash)
                {
                    emitRequest(tc.trailSlot, nullptr, pc, tc, matMc, TrailRequest_RefreshParams, 0.f);
                    tc.lastParamHash = hash;
                }
                continue;
            }

            uint32_t flags = 0;

            // Lazy slot allocation. A recycled slot still holds the previous
            // trail's head/count, so its first append MUST reset the header.
            if (tc.trailSlot == 0xFFFFFFFFu)
            {
                tc.trailSlot = AllocateSlot();
                if (tc.trailSlot == 0xFFFFFFFFu)
                    continue;             // pool full this frame; render nothing
                allocatedThisFrame |= (1ULL << tc.trailSlot);
                flags |= TrailRequest_ResetHeader;
                tc.hasLastSample = false; // sample immediately at the seeded pose
                tc.hasLastXform  = false;
                if (tc.trailSlot + 1 > m_maxActiveSlot) m_maxActiveSlot = tc.trailSlot + 1;
                if (pc > m_frameMaxPointCount) m_frameMaxPointCount = pc;
            }

            // Live pointCount edit: rows of different pc interleaved in one ring are meaningless, so
            // restart the ribbon and sample immediately.
            if (tc.lastPointCount != 0 && tc.lastPointCount != pc)
            {
                flags |= TrailRequest_ResetHeader;
                tc.hasLastSample = false;
                tc.hasLastXform  = false;
            }
            tc.lastPointCount = pc;

            // Gameplay re-arm (AnimFXGateSystem): start a NEW ribbon rather
            // than splicing onto whatever is left of the previous swing. Read
            // here and not in the disabled branch above, so the request
            // survives however long the trail stays off.
            if (tc.restartRequested)
            {
                tc.restartRequested = false;
                flags |= TrailRequest_ResetHeader;
                tc.hasLastSample = false;
                tc.hasLastXform  = false;
            }

            // ---- World points ----
            // Fast path (pc == 1, zero local offset): read the translation column directly, the exact
            // entity origin with no transform rounding.
            XMFLOAT3 worldPts[kMaxPointsPerTrail] = {};
            const GlobalTransform* gt = pGlobal ? pGlobal->Get(e) : nullptr;
            const bool legacyFastPath =
                (pc == 1 &&
                 tc.localPoints[0].x == 0.f && tc.localPoints[0].y == 0.f &&
                 tc.localPoints[0].z == 0.f);
            if (gt)
            {
                if (legacyFastPath)
                {
                    worldPts[0] = { gt->matrix._41, gt->matrix._42, gt->matrix._43 };
                }
                else
                {
                    const XMMATRIX m = XMLoadFloat4x4(&gt->matrix);
                    for (uint32_t p = 0; p < pc; ++p)
                    {
                        const XMVECTOR wp = XMVector3TransformCoord(
                            XMLoadFloat3(&tc.localPoints[p]), m);
                        XMStoreFloat3(&worldPts[p], wp);
                    }
                }
            }
            const XMFLOAT3 tip = worldPts[pc - 1];   // fastest-moving point
            tc.sampleElapsed = tc.hasLastSample
                ? tc.sampleElapsed + (std::max)(dt, 0.f) : 0.f;
            // Rate-limit samples (pc >= 2) so the 64-sample ring spans maxAge: unthrottled substeps
            // (K = 4) would wrap it in 16 frames and squeeze the whole texture into a short, abruptly
            // cut patch. The "- 3" reserves the endpoints; VS subdivisions give spatial smoothness.
            const float sampleInterval = pc >= 2
                ? (std::max)(tc.maxAge, 0.f) / float(kMaxSegmentsPerTrail - 3) : 0.f;

            // ---- Pose decompose (rotation gate + slerp substeps, pc >= 2) ----
            XMVECTOR curScaleV = XMVectorSplatOne();
            XMVECTOR curRotV   = XMQuaternionIdentity();
            XMVECTOR curTransV = XMVectorZero();
            bool  haveDecomp = false;
            float angleDeg   = 0.f;
            if (pc >= 2 && tc.subSampleAngleDeg > 0.f && gt)
            {
                const XMMATRIX m = XMLoadFloat4x4(&gt->matrix);
                if (XMMatrixDecompose(&curScaleV, &curRotV, &curTransV, m))
                {
                    haveDecomp = true;
                    if (tc.hasLastXform)
                    {
                        XMVECTOR lastR = XMLoadFloat4(&tc.lastXformRot);
                        // Shortest path across the quaternion double cover: without the negate a swing crossing
                        // it sprays ~350 deg of substeps.
                        if (XMVectorGetX(XMQuaternionDot(lastR, curRotV)) < 0.f)
                            curRotV = XMVectorNegate(curRotV);
                        float d = XMVectorGetX(XMQuaternionDot(lastR, curRotV));
                        d = (std::min)(std::fabs(d), 1.f);
                        angleDeg = XMConvertToDegrees(2.f * std::acos(d));
                    }
                }
            }

            // ---- Sample gate ----
            // Distance moved by the TIP (pc == 1: the tip is point 0) OR rotation angle (catches
            // in-place blade spin).
            if (tc.hasLastSample)
            {
                const float dx = tip.x - tc.lastSamplePos.x;
                const float dy = tip.y - tc.lastSamplePos.y;
                const float dz = tip.z - tc.lastSamplePos.z;
                const bool movedEnough =
                    dx*dx + dy*dy + dz*dz >= tc.minSampleDistance * tc.minSampleDistance;
                const bool spunEnough =
                    haveDecomp && tc.hasLastXform && angleDeg >= tc.subSampleAngleDeg;
                if ((!movedEnough && !spunEnough) || tc.sampleElapsed < sampleInterval)
                {
                    // Gated out, but a just-allocated / pc-edited slot still
                    // needs its stale header cleared, else it draws ghosts.
                    if (flags & TrailRequest_ResetHeader)
                    {
                        emitRequest(tc.trailSlot, nullptr, pc, tc, matMc, TrailRequest_ResetOnly, 0.f);
                        touchedMask |= (1ULL << tc.trailSlot);
                    }
                    // Idle param edit -> header refresh (see paramHashOf).
                    if (hash != tc.lastParamHash)
                    {
                        emitRequest(tc.trailSlot, nullptr, pc, tc, matMc, TrailRequest_RefreshParams, 0.f);
                        tc.lastParamHash = hash;
                    }
                    continue;
                }
            }

            // ---- CPU slerp sub-sampling (pc >= 2) ----
            // A fast swing covers a large arc in one frame; VS Catmull-Rom alone cannot reconstruct the
            // blade orientation between poses, so the in-between poses are created here, at the source.
            uint32_t K = 1;
            if (haveDecomp && tc.hasLastXform && tc.hasLastSample)
            {
                const float dx = tip.x - tc.lastSamplePos.x;
                const float dy = tip.y - tc.lastSamplePos.y;
                const float dz = tip.z - tc.lastSamplePos.z;
                const float tipDist = std::sqrt(dx*dx + dy*dy + dz*dz);
                const float byAngle = angleDeg / tc.subSampleAngleDeg;
                const float byDist  = tipDist / (std::max)(4.f * tc.minSampleDistance, 1e-3f);
                const float steps   = std::ceil((std::max)(byAngle, byDist));
                K = (uint32_t)(std::max)(1.f, (std::min)(steps, (float)kMaxCpuSubSteps));
                if (sampleInterval > 0.f)
                    K = (std::min)(K, (uint32_t)(std::max)(1.f,
                        std::floor(tc.sampleElapsed / sampleInterval)));
            }

            if (K > 1)
            {
                const XMVECTOR lastR = XMLoadFloat4(&tc.lastXformRot);
                const XMVECTOR lastT = XMLoadFloat3(&tc.lastXformPos);
                const XMVECTOR lastS = XMLoadFloat3(&tc.lastXformScale);
                for (uint32_t k = 1; k < K; ++k)   // oldest -> newest, before the exact sample
                {
                    const float a = (float)k / (float)K;
                    const XMVECTOR r = XMQuaternionNormalize(XMQuaternionSlerp(lastR, curRotV, a));
                    const XMVECTOR s = XMVectorLerp(lastS, curScaleV, a);
                    const XMVECTOR tr = XMVectorLerp(lastT, curTransV, a);
                    const XMMATRIX mk = XMMatrixAffineTransformation(s, XMVectorZero(), r, tr);
                    XMFLOAT3 subPts[kMaxPointsPerTrail] = {};
                    for (uint32_t p = 0; p < pc; ++p)
                        XMStoreFloat3(&subPts[p],
                                      XMVector3TransformCoord(XMLoadFloat3(&tc.localPoints[p]), mk));
                    emitRequest(tc.trailSlot, subPts, pc, tc, matMc,
                                flags, tc.sampleElapsed * (float)(K - k) / (float)K);
                    flags = 0;   // ResetHeader (if any) rides the FIRST request only
                }
            }

            // Newest sample: the exact current transform (not recomposed).
            emitRequest(tc.trailSlot, worldPts, pc, tc, matMc, flags, 0.f);
            touchedMask |= (1ULL << tc.trailSlot);
            tc.lastParamHash = hash;   // appends refresh the header anyway

            tc.lastSamplePos = tip;
            tc.hasLastSample = true;
            tc.sampleElapsed = 0.f;
            if (haveDecomp)
            {
                XMStoreFloat4(&tc.lastXformRot,   curRotV);
                XMStoreFloat3(&tc.lastXformPos,   curTransV);
                XMStoreFloat3(&tc.lastXformScale, curScaleV);
                tc.hasLastXform = true;
            }
            else
            {
                tc.hasLastXform = false;
            }
        }
    }

    // ---- Clear the headers of slots freed this frame that no live trail reused ----
    // Otherwise a destroyed trail's frozen ribbon keeps rendering until its slot is reallocated.
    const uint64_t freedNotReused = freed & ~allocatedThisFrame & ~touchedMask;
    if (freedNotReused)
    {
        TrailComponent dummy{};           // width/colours unused for reset-only
        for (uint32_t s = 0; s < kMaxTrails && m_requestCountThisFrame < kMaxRequestsPerFrame; ++s)
            if (freedNotReused & (1ULL << s))
                emitRequest(s, nullptr, 1, dummy, nullptr, TrailRequest_ResetOnly, 0.f);
    }

    // ---- Build the per-frame draw-bucket list ----
    m_drawBuckets.clear();
    std::sort(t_seenKeys.begin(), t_seenKeys.end());
    t_seenKeys.erase(std::unique(t_seenKeys.begin(), t_seenKeys.end()),
                     t_seenKeys.end());
    std::stable_sort(t_slotDraws.begin(), t_slotDraws.end(),
                     [](const std::pair<uint32_t, SlotDraw>& a,
                        const std::pair<uint32_t, SlotDraw>& b)
                     { return a.first < b.first; });
    std::size_t cursor = 0;
    for (const uint32_t key : t_seenKeys)
    {
        DrawBucket db{ key >> 1, key & 1u, {} };
        while (cursor < t_slotDraws.size() && t_slotDraws[cursor].first == key)
            db.slotDraws.push_back(std::move(t_slotDraws[cursor++].second));
        m_drawBuckets.push_back(std::move(db));
    }
}
