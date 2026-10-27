#include "RenderGraph/RenderPass/DebugTargetViewPass.h"
#include "Graphics/BindlessKind.h"   // BINDLESS_DEBUG_*
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <vector>

// Compute root signature slots (shared with every compute pass, space2).
static constexpr auto kCBSlot = RHI::Slots::Cmp::CB;
static constexpr auto kSRV0   = RHI::Slots::Cmp::SRV_t0;   // t0: the target (a producer: GBuffer1)
static constexpr auto kSRV1   = RHI::Slots::Cmp::SRV_t1;   // a producer: the main depth
static constexpr auto kSRV2   = RHI::Slots::Cmp::SRV_t2;   // a producer: the material table
static constexpr auto kUAV0   = RHI::Slots::Cmp::UAV_u0;   // u0: the view's final output (shadingModel's target)
static constexpr auto kUAV1   = RHI::Slots::Cmp::UAV_u1;   // u1: lightComplexity's target
static constexpr auto kUAV2   = RHI::Slots::Cmp::UAV_u2;   // u2: mipDensity's target

// shaders/DebugTargetView.cs.hlsl: MODE_UNAVAILABLE
static constexpr std::uint32_t kModeUnavailable = 15;

namespace
{
    // A state a compute dispatch may read an SRV in, as is.
    bool Readable(RHI::ResourceState s)
    {
        return s == RHI::ResourceState::SHADER_RESOURCE ||
               s == RHI::ResourceState::SHADER_RESOURCE_COMPUTE ||
               s == RHI::ResourceState::DEPTH_READ_SRV;
    }

    // The producers, in DebugTargetViewPass::Produced order.
    struct ProducerInfo
    {
        ShaderID             id;
        const char*          entry;
        RHI::Format          format;
        const char*          name;
        RHI::Slots::CmpParam uav;         // where its target is bound
        bool                 gbuffer;     // reads GBuffer1 (t0) and binds the material table (t2)
        // The fixed heap slot of the counter the DEBUG_SHADE pixel shaders write and this
        // producer copies out and zeroes (kNoCounter: none).
        std::uint32_t        counter;
        const char*          counterName;
        // Reads the material cost table (BINDLESS_DEBUG_COST_TABLE): made only from the
        // frame's own table.
        bool                 costTable = false;
    };
    constexpr std::uint32_t kNoCounter = ~0u;
    const ProducerInfo kProducers[] = {
        { ShaderID::DebugTargetShadingModel_CS, "CSShadingModel", RHI::Format::R8_UINT, "DebugTargetView.ShadingModel",
          kUAV0, true, kNoCounter, nullptr },
        { ShaderID::DebugTargetLightComplexity_CS, "CSLightComplexity", RHI::Format::R16G16B16A16_FLOAT,
          "DebugTargetView.LightComplexity", kUAV1, false, kNoCounter, nullptr },
        { ShaderID::DebugTargetOverdraw_CS, "CSOverdraw", RHI::Format::R32_UINT, "DebugTargetView.Overdraw",
          kUAV0, false, BINDLESS_DEBUG_OVERDRAW, "DebugTargetView.OverdrawCount" },
        { ShaderID::DebugTargetDensity_CS, "CSDensity", RHI::Format::R16G16_FLOAT, "DebugTargetView.Density",
          kUAV2, false, BINDLESS_DEBUG_DENSITY, "DebugTargetView.DensityPacked" },
        { ShaderID::DebugTargetMaterialCost_CS, "CSMaterialCost", RHI::Format::R32_UINT, "DebugTargetView.MaterialCost",
          kUAV0, true, kNoCounter, nullptr, true },
        { ShaderID::DebugTargetShadingCost_CS, "CSShadingCost", RHI::Format::R32_UINT, "DebugTargetView.ShadingCost",
          kUAV0, false, BINDLESS_DEBUG_COST, "DebugTargetView.CostSum", true },
    };
    static_assert(std::size(kProducers) == static_cast<size_t>(DebugTargetViewPass::Produced::Count),
                  "one row per DebugTargetViewPass::Produced");
}

bool DebugTargetViewPass::EnsurePipeline()
{
    if (m_ready) return true;
    if (m_failed || !m_gfx) return false;
    IGraphicsDevice& gfx = *m_gfx;
    const auto t0 = std::chrono::steady_clock::now();

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::DebugTargetView_CS, RHI::ShaderStage::CS, "DebugTargetView.cs.hlsl", "CSMain");
    for (const ProducerInfo& p : kProducers)
        m_shaderLib.Register(p.id, RHI::ShaderStage::CS, "DebugTargetProduce.cs.hlsl", p.entry);
    bool cbs = m_cb.Create(gfx, "DebugTargetViewPass.CB") && m_cbPreview.Create(gfx, "DebugTargetViewPass.PreviewCB");
    for (size_t k = 0; k < std::size(kProducers); ++k)
        cbs = cbs && m_produceCb[k].Create(gfx, kProducers[k].name);
    if (!CreatePipeline(gfx) || !cbs)
    {
        LOG_ERROR("DebugTargetViewPass: not built (pipeline or constants failed) - the debug target view "
                  "stays off until the next shader reload");
        m_failed = true;
        return false;
    }
    // Bound as t0 when the target is not there: the shader never reads it
    // then, but a descriptor table slot is never left pointing at nothing.
    RHI::TextureDesc td{};
    td.width = 1; td.height = 1; td.depth = 1; td.mipLevels = 1;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.debugName = "DebugTargetViewPass.Fallback";
    const std::uint32_t black = 0;
    RHI::SubresourceData init{ &black, 4, 4 };
    if (!gfx.CreateTexture(td, m_fallback, &init))
        LOG_WARNING("DebugTargetViewPass: fallback texture failed - an unavailable target binds nothing at t0");

    m_ready = true;
    LOG_INFO("DebugTargetViewPass: built in %.1f ms (the first debug target asked for)",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return true;
}

// Onto the live member: a failed rebuild keeps the old pipeline.
bool DebugTargetViewPass::CreatePipeline(IGraphicsDevice& gfx)
{
    const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::DebugTargetView_CS);
    if (!cs)
    {
        LOG_ERROR("DebugTargetViewPass: DebugTargetView.cs.hlsl did not compile");
        return false;
    }
    RHI::PipelineStateDesc pd{};
    pd.cs = cs;
    if (!gfx.CreatePipelineState(pd, m_pso))
    {
        LOG_ERROR("DebugTargetViewPass: pipeline failed");
        return false;
    }
    // The producers: one that does not build leaves its target unavailable and the view
    // working (a rebuild keeps the old pipeline).
    for (size_t k = 0; k < std::size(kProducers); ++k)
    {
        const RHI::Shader* ps = m_shaderLib.GetShader(kProducers[k].id);
        RHI::PipelineStateDesc d{};
        d.cs = ps;
        if (!ps || !gfx.CreatePipelineState(d, m_producePso[k]))
            LOG_ERROR("DebugTargetViewPass: DebugTargetProduce.cs.hlsl's %s did not build - %s stays unavailable "
                      "until a shader reload builds it", kProducers[k].entry, kProducers[k].name);
    }
    return true;
}

// The service point: the pipeline, then the textures of the producers this frame
// runs - made here, where the render lane is parked, so the render half only records
// the dispatch.
void DebugTargetViewPass::OnService(RG::ServiceContext&)
{
    if (m_request.wanted || m_request.produce != 0) EnsurePipeline();
    if (!m_ready) return;
    for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(Produced::Count); ++k)
        if ((m_request.produce & (1u << k)) != 0)
            EnsureProduced(static_cast<Produced>(k), m_request.renderW, m_request.renderH);
    if ((m_request.produce & CostTableMask()) != 0) EnsureCostTable(m_request.materialCapacity);
}

void DebugTargetViewPass::EnsureProduced(Produced kind, std::uint32_t w, std::uint32_t h)
{
    ProducedTarget& t = m_produced[static_cast<size_t>(kind)];
    if (!m_gfx || w == 0 || h == 0 || (t.tex.IsValid() && t.tex.desc.width == w && t.tex.desc.height == h)) return;
    if (!t.tex.IsValid() && t.failedW == w && t.failedH == h) return;   // failed at this size: once, not every frame
    IGraphicsDevice& gfx = *m_gfx;
    if (t.tex.IsValid()) gfx.DestroyTexture(t.tex);   // frame-stamped: a frame still reading it keeps it
    const ProducerInfo& p = kProducers[static_cast<size_t>(kind)];
    RHI::TextureDesc td{};
    td.width = w; td.height = h; td.depth = 1; td.mipLevels = 1;
    td.format     = p.format;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.layout     = RHI::ResourceState::UNORDERED_ACCESS;
    td.debugName = p.name;
    if (!gfx.CreateTexture(td, t.tex))
    {
        LOG_WARNING("DebugTargetViewPass: %s (%ux%u) was not made - that debug target stays unavailable", p.name, w, h);
        t = ProducedTarget{};
        t.failedW = w;
        t.failedH = h;
        return;
    }
    t.state = RHI::ResourceState::UNORDERED_ACCESS;
    t.frame = ~0ull;

    // The counter the DEBUG_SHADE pixel shaders write, at the same size and at its fixed
    // heap slot - made zero from initial data (a committed resource is only usually
    // zeroed by the driver), so the first frame counts from 0; after that the producer
    // zeroes it as it copies it out.
    if (p.counter == kNoCounter) return;
    if (t.counter.IsValid()) gfx.DestroyTexture(t.counter);   // frame-stamped, like the target
    t.counterReady = false;
    RHI::TextureDesc cd = td;
    cd.format     = RHI::Format::R32_UINT;
    cd.debugName = p.counterName;
    const std::vector<std::uint32_t> zeros(static_cast<size_t>(w) * h, 0u);
    const RHI::SubresourceData zero{ zeros.data(), w * 4u, w * h * 4u };
    if (!gfx.CreateTexture(cd, t.counter, &zero) || !gfx.SetFixedDebugUav(p.counter, &t.counter))
    {
        LOG_WARNING("DebugTargetViewPass: %s (%ux%u) was not made or not put at heap %u - that debug target "
                    "stays unavailable", p.counterName, w, h, p.counter);
        if (t.counter.IsValid()) gfx.DestroyTexture(t.counter);
        gfx.SetFixedDebugUav(p.counter, nullptr);
        return;
    }
    t.counterReady = true;
}

std::uint32_t DebugTargetViewPass::CountedMask()
{
    std::uint32_t mask = 0;
    for (size_t k = 0; k < std::size(kProducers); ++k)
        if (kProducers[k].counter != kNoCounter) mask |= 1u << k;
    return mask;
}

std::uint32_t DebugTargetViewPass::CostTableMask()
{
    std::uint32_t mask = 0;
    for (size_t k = 0; k < std::size(kProducers); ++k)
        if (kProducers[k].costTable) mask |= 1u << k;
    return mask;
}

void DebugTargetViewPass::EnsureCostTable(std::uint32_t capacity)
{
    CostTable& t = m_costTable;
    if (!m_gfx || capacity == 0 || t.ready || t.failed) return;
    IGraphicsDevice& gfx = *m_gfx;
    RHI::GPUBufferDesc d;
    d.size       = std::uint64_t(capacity) * sizeof(std::uint32_t);
    d.stride     = sizeof(std::uint32_t);
    d.usage      = RHI::Usage::DEFAULT;
    d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED;
    // Zero from initial data: a frame that reads it before its first upload reads 0.
    const std::vector<std::uint32_t> zeros(capacity, 0u);
    bool ok = gfx.CreateBuffer(d, t.gpu, zeros.data());
    RHI::GPUBufferDesc u = d;
    u.usage = RHI::Usage::UPLOAD;
    for (std::uint32_t s = 0; ok && s < RHI::kFramesInFlight; ++s)
    {
        ok = gfx.CreateBuffer(u, t.upload[s]);
        t.mapped[s] = ok ? gfx.MapBuffer(t.upload[s]) : nullptr;
        ok = ok && t.mapped[s] != nullptr;
    }
    ok = ok && gfx.SetFixedDebugBuffer(BINDLESS_DEBUG_COST_TABLE, &t.gpu);
    if (!ok)
    {
        LOG_WARNING("DebugTargetViewPass: the material cost table (%u slots) was not made or not put at heap %u - "
                    "materialCost stays unavailable", capacity, BINDLESS_DEBUG_COST_TABLE);
        gfx.SetFixedDebugBuffer(BINDLESS_DEBUG_COST_TABLE, nullptr);
        for (std::uint32_t s = 0; s < RHI::kFramesInFlight; ++s)
        {
            if (t.mapped[s]) gfx.UnmapBuffer(t.upload[s]);
            if (t.upload[s].IsValid()) gfx.DestroyBuffer(t.upload[s]);
            t.mapped[s] = nullptr;
        }
        if (t.gpu.IsValid()) gfx.DestroyBuffer(t.gpu);
        t.failed = true;
        return;
    }
    t.capacity = capacity;
    t.ready = true;
    LOG_INFO("DebugTargetViewPass: the material cost table (%u slots) at heap %u", capacity, BINDLESS_DEBUG_COST_TABLE);
    EnsureCoverage(capacity);
}

void DebugTargetViewPass::EnsureCoverage(std::uint32_t capacity)
{
    Coverage& c = m_coverage;
    if (!m_gfx || c.ready) return;
    IGraphicsDevice& gfx = *m_gfx;
    RHI::GPUBufferDesc d;
    d.size       = std::uint64_t(capacity) * sizeof(std::uint32_t);
    d.stride     = sizeof(std::uint32_t);
    d.usage      = RHI::Usage::DEFAULT;
    d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    d.miscFlags = RHI::ResourceMiscFlag::BUFFER_STRUCTURED | RHI::ResourceMiscFlag::UAV_CLEARABLE;   // Produce's clear
    const std::vector<std::uint32_t> zeros(capacity, 0u);
    bool ok = gfx.CreateBuffer(d, c.counts, zeros.data());
    RHI::GPUBufferDesc r = d;
    r.usage      = RHI::Usage::READBACK;
    r.bindFlags = RHI::BindFlag::NONE;
    r.miscFlags = RHI::ResourceMiscFlag::NONE;
    for (std::uint32_t s = 0; ok && s < RHI::kFramesInFlight; ++s)
        ok = gfx.CreateBuffer(r, c.readback[s]);
    if (ok) c.handle = gfx.UavStructured<std::uint32_t>(c.counts, "DebugTargetView.coverage").Bits();
    if (!ok || c.handle == 0)
    {
        LOG_WARNING("DebugTargetViewPass: the material coverage buffers (%u slots) were not made - "
                    "Render.materialCosts() has no pixels", capacity);
        for (std::uint32_t s = 0; s < RHI::kFramesInFlight; ++s)
            if (c.readback[s].IsValid()) gfx.DestroyBuffer(c.readback[s]);
        if (c.counts.IsValid()) gfx.DestroyBuffer(c.counts);
        c = Coverage{};
        return;
    }
    c.ready = true;
}

bool DebugTargetViewPass::TakeCoverage(std::uint32_t frameSlot, std::vector<std::uint32_t>& pixels,
                                       std::uint64_t& frame)
{
    Coverage& c = m_coverage;
    if (!c.ready || !m_gfx || frameSlot >= RHI::kFramesInFlight || c.frameOf[frameSlot] == 0) return false;
    const void* p = m_gfx->MapBuffer(c.readback[frameSlot]);
    if (!p) return false;
    pixels.assign(static_cast<const std::uint32_t*>(p), static_cast<const std::uint32_t*>(p) + m_costTable.capacity);
    m_gfx->UnmapBuffer(c.readback[frameSlot]);
    frame = c.frameOf[frameSlot] - 1;
    c.frameOf[frameSlot] = 0;
    return true;
}

bool DebugTargetViewPass::UploadCostTable(RHI::CommandList cl, std::uint32_t frameSlot, const std::uint32_t* costs,
                                          std::uint32_t count, std::uint64_t frame)
{
    CostTable& t = m_costTable;
    if (!t.ready || !m_gfx || frameSlot >= RHI::kFramesInFlight || !t.mapped[frameSlot]) return false;
    IGraphicsDevice& gfx = *m_gfx;
    const std::uint32_t n = costs ? (std::min)(count, t.capacity) : 0u;
    auto* dst = static_cast<std::uint32_t*>(t.mapped[frameSlot]);
    if (n != 0) std::memcpy(dst, costs, size_t(n) * sizeof(std::uint32_t));
    if (n < t.capacity) std::memset(dst + n, 0, size_t(t.capacity - n) * sizeof(std::uint32_t));
    // The material table's pattern (Renderer_RenderAsync.cpp): COMMON-decayed,
    // promoted to COPY_DEST by the copy, put back to a shader read here.
    gfx.CopyBufferRegion(t.upload[frameSlot], 0, t.gpu, 0, std::uint64_t(t.capacity) * sizeof(std::uint32_t), cl);
    gfx.PushBarrier(RHI::GPUBarrier::Buffer(&t.gpu, RHI::ResourceState::COPY_DST,
                                            RHI::ResourceState::SHADER_RESOURCE), cl);
    t.frame = frame;
    return true;
}

bool DebugTargetViewPass::DebugShadeReady(std::uint32_t produce) const
{
    for (size_t k = 0; k < std::size(kProducers); ++k)
        if (kProducers[k].counter != kNoCounter && (produce & (1u << k)) != 0 && m_produced[k].counterReady)
            return true;
    return false;
}

bool DebugTargetViewPass::Produce(RHI::CommandList cl, Produced kind, const ProduceInputs& in, std::uint64_t frame)
{
    const size_t k = static_cast<size_t>(kind);
    const ProducerInfo& p = kProducers[k];
    ProducedTarget& t = m_produced[k];
    const bool counted = p.counter != kNoCounter;
    if (!m_ready || !m_gfx || !m_producePso[k].IsValid() || !m_produceCb[k].IsValid() || !t.tex.IsValid() ||
        !in.depth || (p.gbuffer && (!in.normal || in.materialSrv == 0)) || (counted && !t.counterReady) ||
        (p.costTable && (!m_costTable.ready || m_costTable.frame != frame)))
        return false;
    IGraphicsDevice& gfx = *m_gfx;
    const std::uint64_t normalSrv = p.gbuffer ? gfx.GetTextureSRVGpuHandle(*in.normal) : 0;
    const std::uint64_t depthSrv  = gfx.GetTextureSRVGpuHandle(*in.depth);
    if (depthSrv == 0 || (p.gbuffer && normalSrv == 0)) return false;

    auto barrier = [&](const RHI::Texture* tex, RHI::ResourceState from, RHI::ResourceState to)
    {
        if (tex && from != to) gfx.PushBarrier(RHI::GPUBarrier::Image(tex, from, to), cl);
    };
    const bool moveNormal = p.gbuffer && !Readable(in.normalState);
    const bool moveDepth  = !Readable(in.depthState);
    if (moveNormal) barrier(in.normal, in.normalState, RHI::ResourceState::SHADER_RESOURCE);
    // The depth plane alone when the graph tracks the planes apart: a barrier on every
    // plane from the depth plane's state would drag the stencil plane along, out of the
    // graph's book.
    auto depthBarrier = [&](RHI::ResourceState from, RHI::ResourceState to)
    {
        if (from != to)
            gfx.PushBarrier(RHI::GPUBarrier::Image(in.depth, from, to, -1, -1, in.depthPlane), cl);
    };
    if (moveDepth)  depthBarrier(in.depthState, RHI::ResourceState::SHADER_RESOURCE);
    barrier(&t.tex, t.state, RHI::ResourceState::UNORDERED_ACCESS);

    if (ProduceCBData* c = m_produceCb[k].Current(gfx))
    {
        *c = ProduceCBData{};
        c->w = t.tex.desc.width;
        c->h = t.tex.desc.height;
        c->materialCount = in.materialCount;
        c->gridFlags = in.gridFlags;
        c->nearZ = in.nearZ;
        c->farZ = in.farZ;
        c->orthographic = in.orthographic ? 1u : 0u;
        c->lightGrid = in.lightGrid;
        c->decalGrid = in.decalGrid;
        c->probeGrid = in.probeGrid;
        c->coverage = (kind == Produced::MaterialCost && m_coverage.ready) ? m_coverage.handle : 0u;
    }
    // materialCost's coverage starts from zero: a UAV clear (the buffer COMMON-decayed
    // and promoted by it), ordered before the dispatch's adds by a UAV barrier.
    const bool coverage = kind == Produced::MaterialCost && m_coverage.ready;
    if (coverage)
    {
        static constexpr std::uint32_t kZero[4] = {};
        gfx.ClearUavUint(m_coverage.counts, kZero, cl);
        gfx.PushBarrier(RHI::GPUBarrier::Memory(&m_coverage.counts), cl);
    }
    gfx.BindComputePipelineState(m_producePso[k], cl);
    gfx.SetComputeRootCBV(kCBSlot, m_produceCb[k].CurrentBuffer(gfx), cl);
    gfx.SetComputeDescriptorTable(kSRV1, depthSrv, cl);
    if (p.gbuffer)
    {
        gfx.SetComputeDescriptorTable(kSRV0, normalSrv, cl);
        gfx.SetComputeDescriptorTable(kSRV2, in.materialSrv, cl);
    }
    gfx.SetComputeDescriptorTable(p.uav, gfx.GetTextureUAVGpuHandle(t.tex), cl);
    // The counter the pixel shaders wrote in this frame's earlier lists: their writes
    // done before the copy-and-zero, and the zeroing done before the next frame's writes.
    if (counted) gfx.PushBarrier(RHI::GPUBarrier::Memory(&t.counter), cl);
    gfx.DispatchCompute((t.tex.desc.width + 7) / 8, (t.tex.desc.height + 7) / 8, 1, cl);
    if (counted) gfx.PushBarrier(RHI::GPUBarrier::Memory(&t.counter), cl);
    if (coverage)
    {
        // Into this frame slot's READBACK buffer; TakeCoverage reads it once the
        // slot comes round again (its frame done). frameOf keeps frame + 1: 0 = none.
        const std::uint32_t slot = gfx.GetFrameIndex() % RHI::kFramesInFlight;
        gfx.PushBarrier(RHI::GPUBarrier::Buffer(&m_coverage.counts, RHI::ResourceState::UNORDERED_ACCESS,
                                                RHI::ResourceState::COPY_SRC), cl);
        gfx.CopyBufferRegion(m_coverage.counts, 0, m_coverage.readback[slot], 0,
                             std::uint64_t(m_costTable.capacity) * sizeof(std::uint32_t), cl);
        m_coverage.frameOf[slot] = frame + 1;
    }

    barrier(&t.tex, RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::SHADER_RESOURCE);
    t.state = RHI::ResourceState::SHADER_RESOURCE;
    if (moveDepth)  depthBarrier(RHI::ResourceState::SHADER_RESOURCE, in.depthState);
    if (moveNormal) barrier(in.normal, RHI::ResourceState::SHADER_RESOURCE, in.normalState);
    t.frame = frame;
    return true;
}

const RHI::Texture* DebugTargetViewPass::GetProduced(Produced kind, RHI::ResourceState& state, std::uint64_t& frame) const
{
    const ProducedTarget& t = m_produced[static_cast<size_t>(kind)];
    if (!t.tex.IsValid()) return nullptr;
    state = t.state;
    frame = t.frame;
    return &t.tex;
}

void DebugTargetViewPass::ReloadShaders(IGraphicsDevice& gfx)
{
    m_failed = false;   // the next EnsurePipeline tries again
    if (m_ready && m_shaderLib.ClearCaches()) CreatePipeline(gfx);
}

bool DebugTargetViewPass::Execute(RHI::CommandList cl, const Source& src,
                                  const RHI::Texture& output, RHI::ResourceState outputState,
                                  std::uint32_t outW, std::uint32_t outH, const Params& p, Slot slot)
{
    FrameCB<CBData>& cb = slot == Slot::Preview ? m_cbPreview : m_cb;
    if (!m_ready || !m_gfx || !m_pso.IsValid() || !cb.IsValid() || !output.IsValid() || outW == 0 || outH == 0)
        return false;
    IGraphicsDevice& gfx = *m_gfx;
    const bool have = (src.srv != 0 || src.uintMode != 0) && src.width != 0 && src.height != 0;

    auto barrier = [&](const RHI::Texture* tex, RHI::ResourceState from, RHI::ResourceState to)
    {
        if (tex && from != to) gfx.PushBarrier(RHI::GPUBarrier::Image(tex, from, to), cl);
    };
    const bool moveSrc = have && src.texture && !Readable(src.state);
    if (moveSrc) barrier(src.texture, src.state, RHI::ResourceState::SHADER_RESOURCE);
    barrier(&output, outputState, RHI::ResourceState::UNORDERED_ACCESS);

    if (CBData* c = cb.Current(gfx))
    {
        c->srcW     = have ? src.width : 0;
        c->srcH     = have ? src.height : 0;
        c->dstW     = outW;
        c->dstH     = outH;
        c->mode     = have ? static_cast<std::uint32_t>(p.mode) : kModeUnavailable;
        c->channel  = p.channel;
        c->rangeMin = p.rangeMin;
        c->rangeMax = p.rangeMax;
        c->exposure = p.exposure;
        c->scale    = p.scale;
        c->nearZ    = p.nearZ;
        c->farZ     = p.farZ;
        c->orthographic = p.orthographic ? 1u : 0u;
        c->srcUint      = have ? src.uintMode : 0u;
        c->srcHandle    = have ? src.uintHandle : 0u;
        c->mask         = p.mask;
    }
    // t0 is a float view: a uint target is read through its handle, and t0 holds the
    // fallback.
    const bool floatSrc = have && src.uintMode == 0;
    const std::uint64_t srv = floatSrc ? src.srv : (m_fallback.IsValid() ? gfx.GetTextureSRVGpuHandle(m_fallback) : 0);
    gfx.BindComputePipelineState(m_pso, cl);
    gfx.SetComputeRootCBV(kCBSlot, cb.CurrentBuffer(gfx), cl);
    if (srv) gfx.SetComputeDescriptorTable(kSRV0, srv, cl);
    gfx.SetComputeDescriptorTable(kUAV0, gfx.GetTextureUAVGpuHandle(output), cl);
    gfx.DispatchCompute((outW + 7) / 8, (outH + 7) / 8, 1, cl);

    barrier(&output, RHI::ResourceState::UNORDERED_ACCESS, outputState);
    if (moveSrc) barrier(src.texture, RHI::ResourceState::SHADER_RESOURCE, src.state);
    return true;
}
