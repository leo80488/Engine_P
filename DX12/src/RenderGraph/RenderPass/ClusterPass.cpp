#include "RenderGraph/RenderPass/ClusterPass.h"
#include "Graphics/ReflectionProbeTypes.h"   // the probe stride
#include "Graphics/GpuLight.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/WordHash.h"

#include <cstring>
#include <string>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

// The light record (64 bytes): one definition for C++ and the shaders
// (shaders/shared/gpu_light.hlsli). Its last two words carry a proposal power and a source
// radius (0 = punctual) as IEEE floats.

// Compute root slots: aliases of the shared compute root signature
// (Graphics/RootSignatureLayout.h), where the values live.
static constexpr auto kCBSlot0   = RHI::Slots::Cmp::CB;      // b0 space2: ClusterCB
static constexpr auto kSRV0      = RHI::Slots::Cmp::SRV_t0;  // t0 space2: cluster AABBs (read)
static constexpr auto kSRV1      = RHI::Slots::Cmp::SRV_t1;  // t1 space2: lights (read)
static constexpr auto kUAV0      = RHI::Slots::Cmp::UAV_u0;  // u0 space2: cluster AABBs (write) / light index list
static constexpr auto kUAV1      = RHI::Slots::Cmp::UAV_u1;  // u1 space2: light grid

void ClusterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::ClusterBuild_CS, RHI::ShaderStage::CS,
                         "ClusterBuild.cs.hlsl", "CSBuildClusters");
    m_shaderLib.Register(ShaderID::ClusterCull_CS, RHI::ShaderStage::CS,
                         "ClusterCull.cs.hlsl", "CSCullLights");
    m_shaderLib.Register(ShaderID::ClusterCullProbes_CS, RHI::ShaderStage::CS,
                         "ClusterCullProbes.cs.hlsl");

    CreatePipelines(gfx);

    // ---- Create GPU buffers ----

    // Light buffer (UPLOAD, CPU writes each frame): one per frame in flight, so the CPU's frame
    // N+1 write cannot stomp the GPU's frame N read.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxLights) * sizeof(GpuShared::GPULight);
        bd.stride     = sizeof(GpuShared::GPULight);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_lightBuffer[i]))
            {
                m_lightMapped[i] = gfx.MapBuffer(m_lightBuffer[i]);
                m_lightsSRV[i]   = gfx.GetBufferSRVGpuHandle(m_lightBuffer[i]);
            }
        }
        bd.size   = static_cast<uint64_t>(kMaxLights) * sizeof(float);
        bd.stride = sizeof(float);
        for (uint32_t i = 0; i < kFrameCount; ++i)
            if (gfx.CreateBuffer(bd, m_mediumScaleBuffer[i]))
            {
                m_mediumScaleMapped[i] = gfx.MapBuffer(m_mediumScaleBuffer[i]);
                m_mediumScaleSRV[i]    = gfx.GetBufferSRVGpuHandle(m_mediumScaleBuffer[i]);
            }
    }

    {
        RHI::GPUBufferDesc bd{};
        bd.size = kWorldWords * sizeof(uint32_t);
        bd.stride = sizeof(uint32_t);
        bd.usage = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        for (uint32_t i = 0; i < kFrameCount; ++i)
            if (gfx.CreateBuffer(bd, m_worldLightBuffer[i]))
                m_worldLightMapped[i] = gfx.MapBuffer(m_worldLightBuffer[i]);
    }

    CreateGrid(m_mainGrid);

    // Atomic counter (single uint); no shader binds it.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(uint32_t);
        bd.stride     = sizeof(uint32_t);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
        if (gfx.CreateBuffer(bd, m_counterBuffer))
            m_counterUAV = gfx.GetBufferUAVGpuHandle(m_counterBuffer);
    }

    // Constant buffer (UPLOAD), one per frame in flight.
    if (!m_clusterCB.Create(gfx, "ClusterPass.CB"))
        LOG_ERROR("ClusterPass: cluster CB create failed");

    // Counter reset buffer (4-byte UPLOAD holding a 0 for a copy-to-counter); nothing copies
    // from it.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = sizeof(uint32_t);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::NONE;
        if (gfx.CreateBuffer(bd, m_counterResetBuf))
        {
            m_counterResetMapped = gfx.MapBuffer(m_counterResetBuf);
            if (m_counterResetMapped)
                *static_cast<uint32_t*>(m_counterResetMapped) = 0;
        }
    }

    LOG_SUCCESS("ClusterPass: initialized (%u clusters, max %u lights, %u index budget)",
                kClusterCount, kMaxLights, kMaxLightIndices);
}

void ClusterPass::CreateGrid(Grid& g)
{
    IGraphicsDevice& gfx = *m_gfx;
    auto make = [&](uint64_t size, uint32_t stride, RHI::GPUBuffer& buf, uint64_t& srv, uint64_t& uav,
                    const char* what)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = size;
        bd.stride     = stride;
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::UNORDERED_ACCESS | RHI::BindFlag::SHADER_RESOURCE;
        if (!gfx.CreateBuffer(bd, buf))
        {
            LOG_ERROR("ClusterPass: %s buffer failed", what);
            return;
        }
        srv = gfx.GetBufferSRVGpuHandle(buf);
        uav = gfx.GetBufferUAVGpuHandle(buf);
    };
    // Cluster AABBs (compute writes); ClusterAABB = 32 bytes.
    make(static_cast<uint64_t>(kClusterCount) * 32, 32, g.aabb, g.aabbSRV, g.aabbUAV, "cluster AABB");
    // Light index list (compute writes, lighting reads).
    make(static_cast<uint64_t>(kMaxLightIndices) * sizeof(uint32_t), sizeof(uint32_t), g.lightIndex,
         g.lightIndexSRV, g.lightIndexUAV, "light index");
    // Light grid; LightGridEntry = 2 x uint = 8 bytes.
    make(static_cast<uint64_t>(kClusterCount) * 8, 8, g.lightGrid, g.lightGridSRV, g.lightGridUAV, "light grid");
    // Probe index list: shaped like the light index list, with a smaller cap.
    make(static_cast<uint64_t>(kMaxProbeIndices) * sizeof(uint32_t), sizeof(uint32_t), g.probeIndex,
         g.probeIndexSRV, g.probeIndexUAV, "probe index");
    // Probe grid (per-cluster offset + count, identical layout to LightGridEntry).
    make(static_cast<uint64_t>(kClusterCount) * 8, 8, g.probeGrid, g.probeGridSRV, g.probeGridUAV, "probe grid");
}

void ClusterPass::ViewGrid::Release(IGraphicsDevice& gfx)
{
    for (RHI::GPUBuffer* b : { &grid.aabb, &grid.lightIndex, &grid.lightGrid, &grid.probeIndex, &grid.probeGrid })
        if (b->IsValid()) gfx.DestroyBuffer(*b);
    grid = Grid{};
}

void ClusterPass::BindView(ViewState& view)
{
    if (view.Slot() == 0)
    {
        m_g = &m_mainGrid;
        return;
    }
    Grid& g = view.Data<ViewGrid>(this).grid;
    if (!g.aabb.IsValid() && m_gfx) CreateGrid(g);
    m_g = &g;
}

// Onto the live members: a failed rebuild keeps the old pipeline.
void ClusterPass::CreatePipelines(IGraphicsDevice& gfx)
{
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterBuild_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_buildPSO))
            LOG_ERROR("ClusterPass: ClusterBuild PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterCull_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_cullPSO))
            LOG_ERROR("ClusterPass: ClusterCull PSO creation failed");
    }
    {
        RHI::PipelineStateDesc d{};
        d.cs = m_shaderLib.GetShader(ShaderID::ClusterCullProbes_CS);
        if (!d.cs || !gfx.CreatePipelineState(d, m_probeCullPSO))
            LOG_ERROR("ClusterPass: ClusterCullProbes PSO creation failed");
    }
}

void ClusterPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

uint32_t ClusterPass::GetWorldLightGridIndex(bool forUpload) const
{
    if (!m_gfx) return ~0u;
    static const bool s_reference = Sys::CommandLine::Get().Has("-rtgi_reference");
    if (s_reference) return ~0u;
    const uint32_t slot = forUpload ? m_gfx->GetUploadSlot() : m_gfx->GetFrameIndex();
    return slot < kFrameCount && m_worldLightMapped[slot] &&
           m_worldLightSlotVersion[slot] != 0
        ? m_gfx->GetBufferSRVHeapIndex(m_worldLightBuffer[slot]) : ~0u;
}

void ClusterPass::UpdateWorldLightGrid(std::span<const ResolvedLight> lights)
{
    if (!m_gfx) return;
    // Dormant lights (zero intensity or black) contribute nothing, so they are not listed: a
    // scene with many switched-off spots (SchoolNight: 154 of 193) would otherwise push dense
    // cells and every GI hit and DI proposal through the many-light path.
    const auto dormant = [](const ResolvedLight& l) {
        return !(l.intensity > 0.f) || !(l.color.x > 0.f || l.color.y > 0.f || l.color.z > 0.f);
    };
    // Exact keys: color / cone / shadow changes cannot enlarge the bounding
    // sphere; only switching a light on or off changes the list.
    const auto keyFor = [&](const ResolvedLight& l) {
        return std::array<uint32_t, 5>{std::bit_cast<uint32_t>(l.position.x),
            std::bit_cast<uint32_t>(l.position.y), std::bit_cast<uint32_t>(l.position.z),
            std::bit_cast<uint32_t>(l.radius), static_cast<uint32_t>(l.type) | (dormant(l) ? 0x100u : 0u)};
    };
    bool changed = !m_worldLightVersion || m_worldLightKey.size() != size_t(m_lightCount) * 5;
    for (uint32_t i = 0; !changed && i < m_lightCount; ++i) {
        const auto key = keyFor(lights[i]);
        changed = !std::equal(key.begin(), key.end(), m_worldLightKey.begin() + size_t(i) * 5);
    }
    if (changed)
    {
        // Keep capacity; unchanged frames compare bits without allocating or
        // rewriting a temporary key vector. Signed zero / NaN bits stay exact.
        m_worldLightKey.resize(size_t(m_lightCount) * 5);
        for (uint32_t i = 0; i < m_lightCount; ++i) {
            const auto key = keyFor(lights[i]);
            std::copy(key.begin(), key.end(), m_worldLightKey.begin() + size_t(i) * 5);
        }
        m_worldLightGrid.assign(kWorldWords, 0);
        float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
        float hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
        bool any = false, valid = true;
        for (uint32_t i = 0; i < m_lightCount; ++i)
        {
            const auto& l = lights[i];
            if (static_cast<uint32_t>(l.type) == 0 || dormant(l)) continue;
            const float p[3] = {l.position.x,l.position.y,l.position.z};
            if (!std::isfinite(l.radius) || l.radius < 0) { valid = false; break; }
            for (uint32_t a = 0; a < 3; ++a)
            {
                valid = valid && std::isfinite(p[a]);
                lo[a] = (std::min)(lo[a], p[a] - l.radius - 0.01f);
                hi[a] = (std::max)(hi[a], p[a] + l.radius + 0.01f);
            }
            any = true;
        }
        const uint32_t dims[3] = {kWorldCellsX,kWorldCellsY,kWorldCellsZ};
        float inv[3]{};
        if (valid && any)
        {
            for (uint32_t a = 0; a < 3; ++a)
            {
                inv[a] = float(dims[a]) / (std::max)(hi[a] - lo[a], 0.02f);
                valid = valid && std::isfinite(inv[a]) && inv[a] > 0;
                m_worldLightGrid[1+a] = std::bit_cast<uint32_t>(lo[a]);
                m_worldLightGrid[4+a] = std::bit_cast<uint32_t>(inv[a]);
            }
            if (valid)
            {
                auto cellsForLight = [&](uint32_t i, auto&& visit) {
                    const auto& l = lights[i];
                    if (static_cast<uint32_t>(l.type) == 0 || dormant(l)) return;
                    const float p[3] = {l.position.x,l.position.y,l.position.z};
                    int first[3], last[3];
                    for (uint32_t a = 0; a < 3; ++a)
                    {
                        first[a] = std::clamp(int(std::floor((p[a]-l.radius-0.01f-lo[a])*inv[a])), 0, int(dims[a])-1);
                        last[a] = std::clamp(int(std::floor((p[a]+l.radius+0.01f-lo[a])*inv[a])), 0, int(dims[a])-1);
                    }
                    for (int z=first[2]; z<=last[2]; ++z)
                    for (int y=first[1]; y<=last[1]; ++y)
                    for (int x=first[0]; x<=last[0]; ++x)
                        visit((z*kWorldCellsY+y)*kWorldCellsX+x);
                };
                // CSR lists: dense cells borrow unused capacity from empty cells.
                // The total pool is bounded, but no cell has a 32-light limit.
                const uint32_t cellCount = kWorldCellsX*kWorldCellsY*kWorldCellsZ;
                for (uint32_t i=0; i<m_lightCount; ++i)
                    cellsForLight(i, [&](uint32_t cell) { ++m_worldLightGrid[kWorldHeader+cell*2]; });
                uint32_t end = kWorldHeader+cellCount*2;
                for (uint32_t cell=0; cell<cellCount; ++cell)
                {
                    uint32_t& count = m_worldLightGrid[kWorldHeader+cell*2];
                    if (count > kWorldWords-end) { count = ~0u; continue; }
                    m_worldLightGrid[kWorldHeader+cell*2+1] = end;
                    end += count;
                }
                std::vector<uint32_t> cursor(cellCount,0);
                for (uint32_t i=0; i<m_lightCount; ++i)
                    cellsForLight(i, [&](uint32_t cell) {
                        if (m_worldLightGrid[kWorldHeader+cell*2] == ~0u) return;
                        const uint32_t offset = m_worldLightGrid[kWorldHeader+cell*2+1];
                        m_worldLightGrid[offset+cursor[cell]++] = i;
                    });
            }
        }
        // 0 = conservative full-list fallback; 2 = no local lights anywhere.
        m_worldLightGrid[0] = valid ? (any ? 1u : 2u) : 0u;
        // Exhaustive conservative coverage audit, opt-in and only on rebuild.
        // Compare independent sphere/cell intersection against the emitted list.
        static const bool s_verify = Sys::CommandLine::Get().Has("-verify_world_lights");
        if (s_verify && valid && any)
        {
            uint32_t occupied = 0, overflow = 0;
            uint64_t entries = 0, checked = 0;
            for (uint32_t z=0; z<kWorldCellsZ; ++z)
            for (uint32_t y=0; y<kWorldCellsY; ++y)
            for (uint32_t x=0; x<kWorldCellsX; ++x)
            {
                const uint32_t base = kWorldHeader + ((z*kWorldCellsY+y)*kWorldCellsX+x)*kWorldCellStride;
                const uint32_t count = m_worldLightGrid[base];
                if (count == ~0u) { ++overflow; continue; }
                if (count) { ++occupied; entries += count; }
                const uint32_t cell[3] = {x,y,z};
                for (uint32_t i=0; i<m_lightCount; ++i)
                {
                    const auto& l = lights[i];
                    if (static_cast<uint32_t>(l.type) == 0 || dormant(l)) continue;
                    const float p[3] = {l.position.x,l.position.y,l.position.z};
                    float d2 = 0;
                    for (uint32_t a=0; a<3; ++a)
                    {
                        const float low = lo[a]+float(cell[a])/inv[a];
                        const float high = lo[a]+float(cell[a]+1)/inv[a];
                        const float d = (std::max)(0.0f,(std::max)(low-p[a],p[a]-high));
                        d2 += d*d;
                    }
                    if (d2 > l.radius*l.radius) continue;
                    ++checked;
                    bool found = false;
                    for (uint32_t j=0; j<count; ++j) found |= m_worldLightGrid[m_worldLightGrid[base+1]+j] == i;
                    ENGINE_CHECK(found, "World light grid dropped an intersecting light");
                }
            }
            LOG_INFO("[WORLDLIGHTAUDIT] lights=%u covered=%llu occupied=%u overflow=%u meanCandidates=%.2f",
                m_lightCount, static_cast<unsigned long long>(checked), occupied, overflow,
                occupied ? double(entries)/occupied : 0.0);
        }
        ++m_worldLightVersion;
    }
    const uint32_t slot = m_gfx->GetUploadSlot();
    if (slot < kFrameCount && m_worldLightMapped[slot] && m_worldLightSlotVersion[slot] != m_worldLightVersion)
    {
        std::memcpy(m_worldLightMapped[slot], m_worldLightGrid.data(), kWorldWords*sizeof(uint32_t));
        m_worldLightSlotVersion[slot] = m_worldLightVersion;
    }
}

void ClusterPass::SetLights(std::span<const ResolvedLight> lights)
{
    SetLightsMeta(lights);
    if (m_gfx) UploadLightRecords(lights, m_gfx->GetUploadSlot());
}

void ClusterPass::SetLightsMeta(std::span<const ResolvedLight> lights)
{
    m_lightCount = std::min(static_cast<uint32_t>(lights.size()), kMaxLights);
    // Match RTActiveLocalLight over the capped, uploaded population.
    m_activeLocalLightCount = m_firstActiveLocalLightIndex = 0;
    for (uint32_t i = 0; i < m_lightCount; ++i)
    {
        const auto& light = lights[i];
        if (static_cast<uint32_t>(light.type) != 0u && light.intensity > 0.f && light.radius > 0.f &&
            (light.color.x > 0.f || light.color.y > 0.f || light.color.z > 0.f))
        {
            if (m_activeLocalLightCount++ == 0) m_firstActiveLocalLightIndex = i;
        }
    }
    UpdateWorldLightGrid(lights);
}

void ClusterPass::UploadLightRecords(std::span<const ResolvedLight> lights, uint32_t frameSlot)
{
    // m_lightCount is META's, for the same list: a caller that split the two
    // halves must hand both the same span.
    if (!m_gfx) return;
    if (!ENGINE_CHECK_ONCE(lights.size() >= m_lightCount,
            "ClusterPass::UploadLightRecords: %zu lights for a META count of %u",
            lights.size(), m_lightCount))
        return;
    if (frameSlot >= kFrameCount || !m_lightMapped[frameSlot]) return;
    // Lights v2: a frame with no light still ends the previous list's life.
    static thread_local std::vector<GpuShared::GPULight> t_records;
    t_records.resize(m_lightCount);
    if (m_lightCount == 0) { UploadLightHistory(lights, frameSlot, t_records.data()); return; }

    auto* dst = static_cast<GpuShared::GPULight*>(m_lightMapped[frameSlot]);
    uint64_t fingerprint = Sys::WordHash64(nullptr, 0);
    for (uint32_t i = 0; i < m_lightCount; ++i)
    {
        const ResolvedLight& src = lights[i];
        // Built in cacheable memory, hashed there, then written to the
        // write-combined UPLOAD slot in one store.
        GpuShared::GPULight rec{};
        rec.position     = { src.position.x, src.position.y, src.position.z };
        rec.radius       = src.radius;
        rec.color        = { src.color.x, src.color.y, src.color.z };
        rec.intensity    = src.intensity;
        rec.direction    = { src.direction.x, src.direction.y, src.direction.z };
        rec.spotAngle    = src.spotAngle;
        rec.type           = static_cast<uint32_t>(src.type);
        rec.shadowSliceIdx = src.shadowSliceIdx;
        // Proposal only: preserve the authored radiance/attenuation in shading.
        // A spot's support is its cone; directional entries never enter RIS.
        const float luminance = std::max(0.f, 0.2126f*src.color.x + 0.7152f*src.color.y + 0.0722f*src.color.z);
        const float solidAngle = rec.type == 2u && src.spotAngle > 0.f
            ? 6.28318530718f*(1.f-std::cos(src.spotAngle)) : 12.56637061436f;
        rec.SetPower(rec.type == 0u ? 0.f : luminance*std::max(0.f,src.intensity)*solidAngle);
        // Lights v2: the emitter radius of a Point / Spot (0 = punctual).
        rec.SetSourceRadius(rec.type == 0u || !std::isfinite(src.sourceRadius) ? 0.f : std::max(0.f, src.sourceRadius));
        fingerprint = Sys::WordHashCombine(fingerprint, Sys::WordHash64(&rec, sizeof(rec)));
        dst[i] = rec;
        t_records[i] = rec;
    }
    // The fog scales, only where one is not 1; they are part of what the records mean.
    bool scaled = false;
    for (uint32_t i = 0; i < m_lightCount && !scaled; ++i)
        scaled = lights[i].mediumScale != 1.f;
    m_mediumScaleUsed[frameSlot] = scaled && m_mediumScaleMapped[frameSlot];
    if (m_mediumScaleUsed[frameSlot])
    {
        auto* scales = static_cast<float*>(m_mediumScaleMapped[frameSlot]);
        for (uint32_t i = 0; i < m_lightCount; ++i)
        {
            scales[i] = lights[i].mediumScale;
            fingerprint = Sys::WordHashCombine(fingerprint, Sys::WordHash64(&lights[i].mediumScale, sizeof(float)));
        }
    }
    m_lightFingerprint[frameSlot] = fingerprint;
    UploadLightHistory(lights, frameSlot, t_records.data());
}

void ClusterPass::EnsureLightHistory(bool mapAndPrevious, bool alias)
{
    if (!m_gfx) return;
    auto& gfx = *m_gfx;
    auto ring = [&](RHI::GPUBuffer* buffers, void** mapped, uint64_t* srv, uint64_t size, uint32_t stride, const char* what)
    {
        if (buffers[0].IsValid()) return;
        RHI::GPUBufferDesc bd{};
        bd.size = size; bd.stride = stride;
        bd.usage = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (!gfx.CreateBuffer(bd, buffers[i]) || !(mapped[i] = gfx.MapBuffer(buffers[i])))
            { LOG_ERROR("ClusterPass: %s ring allocation failed", what); return; }
            srv[i] = gfx.GetBufferSRVGpuHandle(buffers[i]);
        }
        LOG_INFO("[LIGHTS_V2] ring=%s bytes=%llu", what, static_cast<unsigned long long>(size) * kFrameCount);
    };
    if (mapAndPrevious)
    {
        ring(m_lightMapBuffer, m_lightMapMapped, m_lightMapSRV, uint64_t(kLightMapWords) * sizeof(uint32_t), sizeof(uint32_t), "map");
        ring(m_prevLightBuffer, m_prevLightMapped, m_prevLightSRV, uint64_t(kMaxLights) * sizeof(GpuShared::GPULight), sizeof(GpuShared::GPULight), "previous");
    }
    if (alias)
        ring(m_lightAliasBuffer, m_lightAliasMapped, m_lightAliasSRV, uint64_t(kMaxLights) * 32u, 32u, "alias");
}

void ClusterPass::UploadLightHistory(std::span<const ResolvedLight> lights, uint32_t frameSlot, const void* records)
{
    const uint32_t n = m_lightCount;
    if (m_lightHistoryReset.exchange(false, std::memory_order_acq_rel))
    { m_prevKeys.clear(); m_prevRecordBytes.clear(); }
    const uint32_t pn = static_cast<uint32_t>(m_prevKeys.size());
    static thread_local std::vector<uint32_t> t_keys, t_prev2cur, t_cur2prev;
    t_keys.resize(n); t_prev2cur.assign(pn, kInvalidLight); t_cur2prev.assign(n, kInvalidLight);
    bool keyed = true;
    for (uint32_t i = 0; i < n; ++i)
    {
        t_keys[i] = lights[i].key;
        // The gather sorts by key: a usable key list is strictly increasing.
        keyed = keyed && t_keys[i] != 0 && (i == 0 || t_keys[i] > t_keys[i - 1]);
    }
    for (uint32_t j = 0; keyed && j < pn; ++j)
        keyed = m_prevKeys[j] != 0 && (j == 0 || m_prevKeys[j] > m_prevKeys[j - 1]);
    LightHistory h;
    h.prevCount = pn; h.keyed = keyed;
    if (keyed)
    {
        // Merge join of the two sorted key lists.
        uint32_t i = 0, j = 0;
        while (i < n && j < pn)
        {
            if (t_keys[i] == m_prevKeys[j]) { t_cur2prev[i] = j; t_prev2cur[j] = i; ++h.kept; ++i; ++j; }
            else if (t_keys[i] < m_prevKeys[j]) ++i;
            else ++j;
        }
    }
    else if (n == pn)
    {
        // Unknown identities (a preview list): positional.
        for (uint32_t i = 0; i < n; ++i) { t_cur2prev[i] = i; t_prev2cur[i] = i; }
        h.kept = n;
    }
    h.added = n - h.kept; h.removed = pn - h.kept;
    h.identity = n == pn && h.kept == n;
    for (uint32_t i = 0; h.identity && i < n; ++i) h.identity = t_cur2prev[i] == i;
    if (m_lightMapMapped[frameSlot])
    {
        auto* map = static_cast<uint32_t*>(m_lightMapMapped[frameSlot]);
        if (pn) std::memcpy(map, t_prev2cur.data(), pn * sizeof(uint32_t));
        if (n) std::memcpy(map + kMaxLights, t_cur2prev.data(), n * sizeof(uint32_t));
        // The removed local lights' reach (from the previous records). DI
        // never samples a directional light, so its removal changes no
        // reservoir; a local light with no reach lit nothing.
        static thread_local std::vector<float> t_removed;
        t_removed.clear();
        uint32_t removedCount = 0;
        const auto* prevRecs = reinterpret_cast<const GpuShared::GPULight*>(m_prevRecordBytes.data());
        for (uint32_t j = 0; j < pn && removedCount != kInvalidLight; ++j)
        {
            if (t_prev2cur[j] != kInvalidLight) continue;
            const GpuShared::GPULight& r = prevRecs[j];
            if (r.type == 0u || !(r.radius > 0.f) || !(r.intensity > 0.f)) continue;
            if (removedCount == kMaxRemoved || !std::isfinite(r.radius)) { removedCount = kInvalidLight; break; }
            t_removed.insert(t_removed.end(), { r.position.x, r.position.y, r.position.z, r.radius });
            ++removedCount;
        }
        map[kRemovedOffset] = removedCount;
        if (removedCount != kInvalidLight && removedCount)
            std::memcpy(map + kRemovedOffset + 4, t_removed.data(), t_removed.size() * sizeof(float));
        h.mapSRV = m_lightMapSRV[frameSlot];
    }
    if (m_prevLightMapped[frameSlot])
    {
        if (!m_prevRecordBytes.empty())
            std::memcpy(m_prevLightMapped[frameSlot], m_prevRecordBytes.data(), m_prevRecordBytes.size());
        h.prevLightsSRV = m_prevLightSRV[frameSlot];
    }
    m_lightHistory[frameSlot] = h;
    static uint32_t s_changes = 0;   // one writer per frame (main or the render owner)
    if (!h.identity && (++s_changes <= 64 || s_changes % 600 == 0))
        LOG_INFO("[LIGHTS_V2] slot=%u lights=%u previous=%u kept=%u added=%u removed=%u keyed=%u map=%u",
            frameSlot, n, pn, h.kept, h.added, h.removed, h.keyed ? 1u : 0u, h.mapSRV ? 1u : 0u);
    if (!h.identity && Sys::CommandLine::Get().Has("-lights_v2_log"))
    {
        std::string text;
        for (uint32_t j = 0; j < pn; ++j) text += std::to_string(m_prevKeys[j]) + ">" + (t_prev2cur[j] == kInvalidLight ? std::string("x") : std::to_string(t_prev2cur[j])) + " ";
        LOG_INFO("[LIGHTS_V2_MAP] %s", text.c_str());
    }

    // Vose's alias method over the active local lights by power. One uniform picks a row and
    // its coin, so a path vertex still draws exactly one random number for the light choice.
    m_lightAliasCount[frameSlot] = 0;
    if (m_lightAliasMapped[frameSlot] && n)
    {
        const auto* recs = static_cast<const GpuShared::GPULight*>(records);
        static thread_local std::vector<uint32_t> t_active, t_small, t_large;
        static thread_local std::vector<float> t_scaled;
        struct Row { uint32_t light, aliasLight; float threshold, pdfLight, pdfAlias; uint32_t pad[3]; };
        static_assert(sizeof(Row) == 32);
        static thread_local std::vector<Row> t_rows;
        t_active.clear();
        double total = 0.0;
        for (uint32_t i = 0; i < n; ++i)
        {
            const auto& r = recs[i];
            const bool active = r.type != 0u && r.intensity > 0.f && r.radius > 0.f && r.Power() > 0.f && std::isfinite(r.Power());
            if (active) { t_active.push_back(i); total += r.Power(); }
        }
        const uint32_t m = static_cast<uint32_t>(t_active.size());
        if (m && total > 0.0)
        {
            t_scaled.resize(m); t_rows.assign(m, Row{});
            t_small.clear(); t_large.clear();
            for (uint32_t k = 0; k < m; ++k)
            {
                t_scaled[k] = static_cast<float>(recs[t_active[k]].Power() * m / total);
                (t_scaled[k] < 1.f ? t_small : t_large).push_back(k);
            }
            while (!t_small.empty() && !t_large.empty())
            {
                const uint32_t s = t_small.back(); t_small.pop_back();
                const uint32_t l = t_large.back();
                t_rows[s].threshold = t_scaled[s];
                t_rows[s].aliasLight = t_active[l];
                t_scaled[l] = (t_scaled[l] + t_scaled[s]) - 1.f;
                if (t_scaled[l] < 1.f) { t_large.pop_back(); t_small.push_back(l); }
            }
            for (uint32_t k : t_large) { t_rows[k].threshold = 1.f; t_rows[k].aliasLight = t_active[k]; }
            for (uint32_t k : t_small) { t_rows[k].threshold = 1.f; t_rows[k].aliasLight = t_active[k]; }
            for (uint32_t k = 0; k < m; ++k)
            {
                t_rows[k].light = t_active[k];
                t_rows[k].pdfLight = static_cast<float>(recs[t_rows[k].light].Power() / total);
                t_rows[k].pdfAlias = static_cast<float>(recs[t_rows[k].aliasLight].Power() / total);
            }
            std::memcpy(m_lightAliasMapped[frameSlot], t_rows.data(), m * sizeof(Row));
            m_lightAliasCount[frameSlot] = m;
        }
    }

    // -lightprepdigest: the keys and the mapping this upload produced.
    uint64_t digest = Sys::WordHash64(t_keys.data(), n * sizeof(uint32_t));
    digest = Sys::WordHashCombine(digest, Sys::WordHash64(t_cur2prev.data(), n * sizeof(uint32_t)));
    digest = Sys::WordHashCombine(digest, Sys::WordHash64(t_prev2cur.data(), pn * sizeof(uint32_t)));
    m_lightHistoryDigest = digest;

    m_prevKeys.assign(t_keys.begin(), t_keys.end());
    const auto* bytes = static_cast<const uint8_t*>(records);
    m_prevRecordBytes.assign(bytes, bytes + size_t(n) * sizeof(GpuShared::GPULight));
}

ClusterPass::LightHistory ClusterPass::GetLightHistory() const
{
    if (!m_gfx) return {};
    const uint32_t s = m_gfx->GetFrameIndex();
    return s < kFrameCount ? m_lightHistory[s] : LightHistory{};
}

ClusterPass::LightAlias ClusterPass::GetLightAlias() const
{
    if (!m_gfx) return {};
    const uint32_t s = m_gfx->GetFrameIndex();
    if (s >= kFrameCount || !m_lightAliasCount[s]) return {};
    return { m_lightAliasSRV[s], m_lightAliasCount[s] };
}

void ClusterPass::SetCamera(const DirectX::XMFLOAT4X4& invProj,
                             const DirectX::XMFLOAT4X4& viewMatrix,
                             float nearZ, float farZ,
                             uint32_t screenW, uint32_t screenH,
                             bool orthographic)
{
    SetCameraOf(m_mainGrid, invProj, viewMatrix, nearZ, farZ, screenW, screenH, orthographic, m_lightCount);
}

void ClusterPass::SetViewCamera(ViewState& view, const DirectX::XMFLOAT4X4& invProj,
                                const DirectX::XMFLOAT4X4& viewMatrix, float nearZ, float farZ,
                                uint32_t screenW, uint32_t screenH, bool orthographic)
{
    if (view.Slot() == 0) return;
    // The light count the main view's grid was given this frame: the lights are the scene's.
    SetCameraOf(view.Data<ViewGrid>(this).grid, invProj, viewMatrix, nearZ, farZ, screenW, screenH, orthographic,
                m_mainGrid.cb.lightCount);
}

void ClusterPass::SetCameraOf(Grid& g, const DirectX::XMFLOAT4X4& invProj, const DirectX::XMFLOAT4X4& viewMatrix,
                              float nearZ, float farZ, uint32_t screenW, uint32_t screenH, bool orthographic,
                              uint32_t lightCount)
{
    // AABBs are rebuilt lazily, but they depend on EVERY projection input: invProj (fov /
    // aspect -> tile XY extents), nearZ / farZ (exponential slice bounds) and the projection
    // mode. Missing one (editor free-cam vs scene camera, VCam zoom, Edit vs Play) leaves stale
    // AABBs: the sphere-vs-AABB binning then disagrees with GetClusterCoord in Lighting.ps /
    // DecalApply.cs and culls lights and decals that are in view. Compare the raw bytes BEFORE
    // overwriting the grid's CB data.
    const uint32_t ortho = orthographic ? 1u : 0u;
    if (screenW != g.lastScreenW || screenH != g.lastScreenH
        || ortho != g.cb.isOrtho
        || nearZ != g.cb.nearZ || farZ != g.cb.farZ
        || std::memcmp(g.cb.invProj, &invProj, 64) != 0)
    {
        g.needsRebuild = true;
        g.lastScreenW  = screenW;
        g.lastScreenH  = screenH;
    }

    std::memcpy(g.cb.invProj, &invProj, 64);
    g.cb.nearZ      = nearZ;
    g.cb.farZ       = farZ;
    g.cb.screenW    = screenW;
    g.cb.screenH    = screenH;
    g.cb.lightCount = lightCount;
    g.cb.probeCount = m_probeCount;
    std::memcpy(g.cb.viewMatrix, &viewMatrix, 64);
    g.cb.isOrtho = ortho;
}

void ClusterPass::Execute(RHI::CommandList cl)
{
    Grid& g = *m_g;
    g.lightGridWritten = g.probeGridWritten = false;   // set below when a cull runs
    if (!m_gfx || !m_buildPSO.IsValid() || !m_cullPSO.IsValid() || !g.aabb.IsValid()) return;

    auto& gfx = *m_gfx;

    // probeCount lives on the pass and may be set after SetCamera: refresh it so
    // ClusterCullProbes sees the latest value.
    g.cb.probeCount = m_probeCount;
    // ClusterCullProbes opens the probe buffer from the heap: the index is recomputed every
    // frame from the current handle, never cached. A typed handle with the stride checked; no
    // buffer -> the stride's null (the probe dispatch is skipped then anyway).
    g.cb.probeBufferIdx = gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
        m_probeBufferSRV, "ClusterCB probe buffer", static_cast<uint32_t>(sizeof(GpuShared::ReflectionProbe))).Bits();

    // Upload CB into this frame's slot.
    if (auto* p = m_clusterCB.Current(gfx))
        *p = g.cb;

    const uint32_t frameSlot = gfx.GetFrameIndex();
    const RHI::GPUBuffer& cbBuf = m_clusterCB.CurrentBuffer(gfx);

    // ---- Pass 1: build the cluster AABBs (when a projection input changed) ----
    if (g.needsRebuild)
    {
        gfx.BindComputePipelineState(m_buildPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);
        gfx.SetComputeDescriptorTable(kUAV0, g.aabbUAV, cl);

        // 4x4x4 threads per group (ClusterBuild.cs.hlsl); the shader's own tile / slice bounds
        // check discards the overhang.
        gfx.DispatchCompute((kTileX + 3) / 4, (kTileY + 3) / 4, (kSlices + 3) / 4, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.aabb), cl);
        g.needsRebuild = false;
    }

    // ---- Pass 2: Cull lights into clusters ----
    if (m_lightCount > 0)
    {
        // Fixed-layout culling: each cluster writes its own pre-allocated slice, so no atomic
        // counter is needed.
        gfx.BindComputePipelineState(m_cullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);

        gfx.SetComputeDescriptorTable(kSRV0, g.aabbSRV, cl);
        gfx.SetComputeDescriptorTable(kSRV1, m_lightsSRV[frameSlot], cl);
        gfx.SetComputeDescriptorTable(kUAV0, g.lightIndexUAV, cl);
        gfx.SetComputeDescriptorTable(kUAV1, g.lightGridUAV, cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.lightIndex), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.lightGrid), cl);
        g.lightGridWritten = true;
    }

    // ---- Pass 3: Cull reflection probes into clusters ----
    // Skipped when no probes exist or the probe SRV is not wired yet; the lighting shader's
    // `probeGrid[idx].count == 0` short-circuit covers the resulting all-zero grid.
    if (m_probeCullPSO.IsValid() && m_probeCount > 0 && m_probeBufferSRV != 0)
    {
        gfx.BindComputePipelineState(m_probeCullPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot0, cbBuf, 0, cl);

        // Reuse t0 (cluster AABBs); the probe buffer comes from the heap
        // (probeBufferIdx); UAVs u0 / u1 swap from light grid to probe grid.
        gfx.SetComputeDescriptorTable(kSRV0, g.aabbSRV, cl);
        gfx.SetComputeDescriptorTable(kUAV0, g.probeIndexUAV, cl);
        gfx.SetComputeDescriptorTable(kUAV1, g.probeGridUAV, cl);

        gfx.DispatchCompute((kClusterCount + 63) / 64, 1, 1, cl);

        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.probeIndex), cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&g.probeGrid), cl);
        g.probeGridWritten = true;
    }
}

// Per-frame SRV: the slot matching gfx.GetFrameIndex(), so LightingPass / DDGIPass sample
// the buffer the CPU wrote for this frame.
uint64_t ClusterPass::GetLightsSRVHandle() const
{
    if (!m_gfx) return 0;
    const uint32_t s = m_gfx->GetFrameIndex();
    return (s < kFrameCount) ? m_lightsSRV[s] : 0;
}

uint64_t ClusterPass::GetLightMediumScaleSRVHandle() const
{
    if (!m_gfx || !m_lightCount) return 0;
    const uint32_t s = m_gfx->GetFrameIndex();
    return (s < kFrameCount && m_mediumScaleUsed[s]) ? m_mediumScaleSRV[s] : 0;
}

uint64_t ClusterPass::GetLightsFingerprint() const
{
    // The hash UploadLightRecords computed while building this slot's records; never read
    // back from the mapped UPLOAD heap.
    if (!m_gfx || !m_lightCount) return 1469598103934665603ull;
    const auto slot = m_gfx->GetFrameIndex();
    if (slot >= kFrameCount || !m_lightMapped[slot]) return 0;
    return m_lightFingerprint[slot];
}

void ClusterPass::OnService(RG::ServiceContext& c)
{
    if (c.gfx->GetCapabilities().inlineRaytracing)
        EnsureLightHistory(m_request.mapAndPrevious, m_request.alias);
}
