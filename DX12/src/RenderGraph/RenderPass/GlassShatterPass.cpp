#include "RenderGraph/RenderPass/GlassShatterPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace DirectX;

// Compute root signature slots (shared with every compute pass, space2) -
// aliases of Graphics/RootSignatureLayout.h.
static constexpr auto kCBSlot = RHI::Slots::Cmp::CB;
static constexpr auto kSRV0   = RHI::Slots::Cmp::SRV_t0;
static constexpr auto kSRV1   = RHI::Slots::Cmp::SRV_t1;
static constexpr auto kSRV2   = RHI::Slots::Cmp::SRV_t2;
static constexpr auto kUAV0   = RHI::Slots::Cmp::UAV_u0;
static constexpr auto kUAV1   = RHI::Slots::Cmp::UAV_u1;

// ==== CPU Voronoi mesh generation (Sutherland-Hodgman clipping) ====
namespace
{
    using Vec2 = XMFLOAT2;

    inline Vec2 V(float x, float y) { return {x, y}; }
    inline Vec2 Sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
    inline float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
    inline float Length(Vec2 a) { return std::sqrt(Dot(a, a)); }
    inline Vec2 Normalize(Vec2 a)
    {
        const float l = Length(a);
        return l > 1e-8f ? Vec2{a.x / l, a.y / l} : Vec2{0, 0};
    }

    // Clips polygon @p io in place against the half-plane { p : dot(n, p) <= d }.
    // Keeps CCW order; empty if the whole polygon is outside.
    void ClipHalfPlane(std::vector<Vec2>& io, Vec2 n, float d)
    {
        if (io.empty()) return;
        std::vector<Vec2> out;
        out.reserve(io.size() + 2);
        const size_t k = io.size();
        for (size_t i = 0; i < k; ++i)
        {
            const Vec2  cur  = io[i];
            const Vec2  prev = io[(i + k - 1) % k];
            const float dCur = Dot(n, cur);
            const float dPrv = Dot(n, prev);
            const bool  curIn = (dCur <= d);
            const bool  prvIn = (dPrv <= d);

            if (curIn)
            {
                if (!prvIn)
                {
                    const float t = (d - dPrv) / (dCur - dPrv);
                    out.push_back({prev.x + (cur.x - prev.x) * t,
                                   prev.y + (cur.y - prev.y) * t});
                }
                out.push_back(cur);
            }
            else if (prvIn)
            {
                const float t = (d - dPrv) / (dCur - dPrv);
                out.push_back({prev.x + (cur.x - prev.x) * t,
                               prev.y + (cur.y - prev.y) * t});
            }
        }
        io.swap(out);
    }
}

void GlassShatterPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GlassShatterInit_CS,      RHI::ShaderStage::CS,
                         "GlassShatterInit.cs.hlsl",      "CSMain");
    m_shaderLib.Register(ShaderID::GlassShatterSimulate_CS,  RHI::ShaderStage::CS,
                         "GlassShatterSimulate.cs.hlsl",  "CSMain");
    m_shaderLib.Register(ShaderID::GlassShatterComposite_CS, RHI::ShaderStage::CS,
                         "GlassShatterComposite.cs.hlsl", "CSMain");

    if (!CreatePipelines(gfx)) return;

    GenerateVoronoiMesh();

    if (!m_paramsCB.Create(gfx, "GlassShatterPass.ParamsCB"))
        LOG_ERROR("GlassShatterPass: params CB create failed");

    LOG_SUCCESS("GlassShatterPass: initialized (%u shards)", m_shardCount);
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool GlassShatterPass::CreatePipelines(IGraphicsDevice& gfx)
{
    auto compile = [&](ShaderID id, RHI::PipelineState& outPSO, const char* name)
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(id);
        if (!cs) { LOG_ERROR("GlassShatterPass: %s shader load failed", name); return false; }
        RHI::PipelineStateDesc pd{};
        pd.cs = cs;
        if (!gfx.CreatePipelineState(pd, outPSO))
        {
            LOG_ERROR("GlassShatterPass: %s PSO creation failed", name);
            return false;
        }
        return true;
    };
    const bool okInit      = compile(ShaderID::GlassShatterInit_CS,      m_initPSO,      "Init");
    const bool okSimulate  = compile(ShaderID::GlassShatterSimulate_CS,  m_simulatePSO,  "Simulate");
    const bool okComposite = compile(ShaderID::GlassShatterComposite_CS, m_compositePSO, "Composite");
    return okInit && okSimulate && okComposite;
}

void GlassShatterPass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

// Builds one fixed Voronoi pattern at startup: a jittered grid of seeds in UV
// [0,1], each clipped against the perpendicular bisectors with all others into
// its convex cell; cell edges are stored as (normal, d) relative to the centroid.
void GlassShatterPass::GenerateVoronoiMesh()
{
    if (!m_gfx) return;

    // ---- Seed points ----
    // 9x9 jittered grid -> 81 seeds, each in the middle 60% of its grid cell
    // (0.2 + 0.6 x jitter), so every seed is strictly inside the unit square and
    // neighbouring seeds do not cluster across cell boundaries.
    constexpr int kGridN = 9;
    std::vector<Vec2> seeds;
    seeds.reserve(kGridN * kGridN);

    std::mt19937 rng(0xC0FFEEu);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);

    for (int j = 0; j < kGridN; ++j)
    for (int i = 0; i < kGridN; ++i)
    {
        const float jx = uni(rng);
        const float jy = uni(rng);
        const float u  = (i + 0.2f + 0.6f * jx) / float(kGridN);
        const float v  = (j + 0.2f + 0.6f * jy) / float(kGridN);
        seeds.push_back(V(u, v));
    }

    // ---- Per-seed clip & edge extraction ----
    std::vector<ShardMeta> meta;
    std::vector<ShardEdge> edges;
    meta.reserve(seeds.size());

    for (size_t s = 0; s < seeds.size(); ++s)
    {
        std::vector<Vec2> cell = { V(0,0), V(1,0), V(1,1), V(0,1) };  // CCW unit square

        for (size_t o = 0; o < seeds.size(); ++o)
        {
            if (o == s) continue;
            const Vec2  diff   = Sub(seeds[o], seeds[s]);
            const float dl     = Length(diff);
            if (dl < 1e-6f) continue;
            const Vec2  nrm    = { diff.x / dl, diff.y / dl };  // points from s toward o
            const Vec2  midPt  = { (seeds[s].x + seeds[o].x) * 0.5f,
                                   (seeds[s].y + seeds[o].y) * 0.5f };
            const float plane  = Dot(nrm, midPt);
            ClipHalfPlane(cell, nrm, plane);
            if (cell.empty()) break;
        }
        if (cell.size() < 3) continue;

        // Centroid (vertex average - close enough for nearly-regular cells).
        Vec2 c = {0, 0};
        for (const auto& v : cell) { c.x += v.x; c.y += v.y; }
        c.x /= cell.size(); c.y /= cell.size();

        // Edges relative to the centroid. The normal points outward (CCW polygon ->
        // right-hand normal of the edge).
        const uint32_t edgeStart = static_cast<uint32_t>(edges.size());
        const size_t   k         = cell.size();
        for (size_t i = 0; i < k; ++i)
        {
            const Vec2 v0 = Sub(cell[i],            c);
            const Vec2 v1 = Sub(cell[(i + 1) % k],  c);
            const Vec2 e  = Sub(v1, v0);
            // Right-hand outward normal of a CCW polygon: e rotated by -90 deg,
            // (e.x, e.y) -> (e.y, -e.x), normalised.
            const Vec2 nrm = Normalize({ e.y, -e.x });
            const float dPlane = Dot(nrm, v0);  // pixel inside iff dot(n, p) <= dPlane
            edges.push_back({ nrm, dPlane, 0.0f });
        }

        ShardMeta m;
        m.centroidUV  = c;
        m.edgeOffset  = edgeStart;
        m.edgeCount   = static_cast<uint32_t>(k);
        meta.push_back(m);

        if (meta.size() >= kTargetShards) break;
    }

    m_shardCount = static_cast<uint32_t>(meta.size());
    if (m_shardCount == 0)
    {
        LOG_ERROR("GlassShatterPass: Voronoi generation produced 0 shards");
        return;
    }

    // ---- Meta buffer (UPLOAD heap, written once) ----
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(meta.size()) * sizeof(ShardMeta);
        bd.stride     = sizeof(ShardMeta);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        if (m_gfx->CreateBuffer(bd, m_metaBuffer))
        {
            void* p = m_gfx->MapBuffer(m_metaBuffer);
            if (p) std::memcpy(p, meta.data(), static_cast<size_t>(bd.size));
        }
    }

    // ---- Edge buffer (UPLOAD heap, written once) ----
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(edges.size()) * sizeof(ShardEdge);
        bd.stride     = sizeof(ShardEdge);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        if (m_gfx->CreateBuffer(bd, m_edgeBuffer))
        {
            void* p = m_gfx->MapBuffer(m_edgeBuffer);
            if (p) std::memcpy(p, edges.data(), static_cast<size_t>(bd.size));
        }
    }

    // ---- Shard buffer (DEFAULT, GPU-managed) ----
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(m_shardCount) * sizeof(ShardGPU);
        bd.stride     = sizeof(ShardGPU);
        bd.usage      = RHI::Usage::DEFAULT;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
        if (!m_gfx->CreateBuffer(bd, m_shardBuffer))
            LOG_ERROR("GlassShatterPass: shard buffer creation failed");
    }
}

void GlassShatterPass::SetCrackColor(float r, float g, float b)
{
    m_crackColor = { r, g, b };
}

void GlassShatterPass::Trigger(float impactU, float impactV)
{
    m_impactUV          = { std::clamp(impactU, 0.0f, 1.0f),
                            std::clamp(impactV, 0.0f, 1.0f) };
    m_active            = true;
    m_pendingInit       = true;
    m_timeSinceTrigger  = 0.0f;
}

// Lazily (re)creates the shatter source to match the Tonemap output EXACTLY - its
// format as well as its size (the hazard is at m_targetFormat / m_sourceFormat).
void GlassShatterPass::EnsureSourceTexture(uint32_t w, uint32_t h)
{
    if (m_shatterSource.IsValid() && w == m_sourceW && h == m_sourceH &&
        m_targetFormat == m_sourceFormat)
        return;

    // Recreating throws away the frozen frame, and the capture only runs on the
    // Execute right after Trigger() (m_pendingInit). Re-arm it, or a recreate during a
    // running shatter (a resize, an HDR toggle) leaves a never-written COPY_DST texture
    // that the composite samples as the frozen background. The effect visibly
    // restarts from the current frame instead.
    if (m_shatterSource.IsValid() && m_active) m_pendingInit = true;

    if (m_shatterSource.IsValid()) m_gfx->DestroyTexture(m_shatterSource);

    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.format     = m_targetFormat;   // matches ToneMapPass output
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::COPY_DST;
    if (!m_gfx->CreateTexture(td, m_shatterSource))
    {
        LOG_ERROR("GlassShatterPass: source texture create failed (%ux%u)", w, h);
        m_sourceW = m_sourceH = 0;
        m_sourceFormat = RHI::Format::UNKNOWN;
        return;
    }
    m_shatterSourceState = RHI::ResourceState::COPY_DST;
    m_sourceW = w;
    m_sourceH = h;
    m_sourceFormat = m_targetFormat;
}

void GlassShatterPass::UploadCB(uint32_t w, uint32_t h)
{
    if (!m_gfx) return;
    auto* slot = m_paramsCB.Current(*m_gfx);
    if (!slot) return;
    ShatterCB c{};
    c.TimeSinceTrigger     = m_timeSinceTrigger;
    c.DeltaTime            = 0.0f;  // overwritten per-Execute
    c.Duration             = m_duration;
    c.ShardCount           = m_shardCount;
    c.ImpactPointUV        = m_impactUV;
    c.ImpactRadialStrength = m_impactStrength;
    c.GravityNDC           = m_gravityNDC;
    c.AirDrag              = m_airDrag;
    c.AngularDamping       = m_angularDamping;
    c.CrackWidth           = m_crackWidth;
    c.HoldDuration         = m_holdDuration;
    c.CrackColor           = m_crackColor;
    c.Width                = w;
    c.Height               = h;
    *slot = c;
}

void GlassShatterPass::Execute(RHI::CommandList cl,
                               const RHI::Texture* tonemapOutput,
                               RHI::ResourceState tonemapEntryExitState,
                               uint32_t vpW, uint32_t vpH,
                               float deltaTime)
{
    if (!m_active || !tonemapOutput || vpW == 0 || vpH == 0) return;
    if (!m_initPSO.IsValid() || !m_simulatePSO.IsValid() || !m_compositePSO.IsValid()) return;
    if (!m_shardBuffer.IsValid() || !m_metaBuffer.IsValid() || !m_edgeBuffer.IsValid()) return;

    // Auto-deactivate when duration elapsed.
    m_timeSinceTrigger += deltaTime;
    if (m_timeSinceTrigger >= m_duration)
    {
        m_active            = false;
        m_pendingInit       = false;
        m_timeSinceTrigger  = 0.0f;
        return;
    }

    EnsureSourceTexture(vpW, vpH);
    if (!m_shatterSource.IsValid()) return;

    auto& gfx = *m_gfx;

    // Skips same-state transitions: D3D12 rejects a barrier with before == after.
    auto SafeImage = [&](const RHI::Texture* tex,
                         RHI::ResourceState from,
                         RHI::ResourceState to)
    {
        if (from == to || !tex) return;
        gfx.PushBarrier(RHI::GPUBarrier::Image(tex, from, to), cl);
    };

    // ---- Trigger frame: capture the Tonemap output into the shatter source ----
    if (m_pendingInit)
    {
        SafeImage(tonemapOutput, tonemapEntryExitState, RHI::ResourceState::COPY_SRC);
        SafeImage(&m_shatterSource, m_shatterSourceState, RHI::ResourceState::COPY_DST);
        m_shatterSourceState = RHI::ResourceState::COPY_DST;

        m_gfx->CopyTextureSubresource(*tonemapOutput, 0, 0,
                                      m_shatterSource,  0, 0, cl);

        SafeImage(&m_shatterSource, RHI::ResourceState::COPY_DST,
                  RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
        m_shatterSourceState = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;

        SafeImage(tonemapOutput, RHI::ResourceState::COPY_SRC,
                  RHI::ResourceState::UNORDERED_ACCESS);
    }
    else
    {
        SafeImage(tonemapOutput, tonemapEntryExitState,
                  RHI::ResourceState::UNORDERED_ACCESS);
    }

    UploadCB(vpW, vpH);
    // UploadCB leaves DeltaTime 0: patch it here.
    if (auto* slot = m_paramsCB.Current(gfx))
        slot->DeltaTime = deltaTime;

    const RHI::GPUBuffer& paramsCBBuf = m_paramsCB.CurrentBuffer(gfx);

    // ---- Init dispatch (trigger frame only) ----
    if (m_pendingInit)
    {
        gfx.BindComputePipelineState(m_initPSO, cl);
        gfx.SetComputeRootCBV(kCBSlot, paramsCBBuf, cl);
        gfx.SetComputeDescriptorTable(kSRV0, gfx.GetBufferSRVGpuHandle(m_metaBuffer), cl);
        gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
        gfx.DispatchCompute((m_shardCount + 63) / 64, 1, 1, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shardBuffer), cl);
        m_pendingInit = false;
    }

    // ---- Simulate dispatch ----
    gfx.BindComputePipelineState(m_simulatePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, paramsCBBuf, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
    gfx.DispatchCompute((m_shardCount + 63) / 64, 1, 1, cl);
    gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_shardBuffer), cl);

    // ---- Composite dispatch ----
    // Reads the edges (UPLOAD-heap SRV), the shatter source (compute SRV) and the
    // shards (the RW buffer, read only), writes the Tonemap output UAV.
    gfx.BindComputePipelineState(m_compositePSO, cl);
    gfx.SetComputeRootCBV(kCBSlot, paramsCBBuf, cl);
    gfx.SetComputeDescriptorTable(kSRV0, gfx.GetBufferSRVGpuHandle(m_edgeBuffer),  cl);
    gfx.SetComputeDescriptorTable(kSRV1, gfx.GetTextureSRVGpuHandle(m_shatterSource), cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(*tonemapOutput), cl);
    gfx.SetComputeDescriptorTable(kUAV1, gfx.GetBufferUAVGpuHandle(m_shardBuffer), cl);
    gfx.DispatchCompute((vpW + 7) / 8, (vpH + 7) / 8, 1, cl);

    // Restore Tonemap output to its entry state so the caller's state-tracking
    // (ToneMapPass::m_finalOutputState) stays correct.
    SafeImage(tonemapOutput, RHI::ResourceState::UNORDERED_ACCESS, tonemapEntryExitState);
}
