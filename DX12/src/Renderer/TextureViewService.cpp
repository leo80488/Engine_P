// TextureViewService.cpp - see the header.
#include "Renderer/TextureViewService.h"

#include "Graphics/FrameConstants.h"
#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/TextureViewPass.h"
#include "System/Log.h"

#include <algorithm>
#include <string>

TextureViewService::TextureViewService(IGraphicsDevice& gfx)
    : m_gfx(gfx), m_pass(std::make_unique<TextureViewPass>())
{
    m_pass->Init(gfx);
    m_views.resize(kMaxViews);
    for (auto& r : m_probeRecords) r.resize(kMaxViews);
    static_assert(kMaxViews <= TextureViewPass::kProbeSlots, "a probe result per view");
    static_assert(RHI::kFramesInFlight == 3, "m_probeRecords is sized for three frame slots");
}

TextureViewService::~TextureViewService() = default;

// ==== Main thread ====
std::uint32_t TextureViewService::Acquire()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (std::uint32_t i = 0; i < m_views.size(); ++i)
    {
        View& v = m_views[i];
        if (v.live || v.releasePending) continue;
        v.live          = true;
        v.hasRequest    = false;
        v.requestedHash = 0;
        return i;
    }
    LOG_WARNING("[TEXVIEW] all %u texture views are taken - the picture is not drawn", kMaxViews);
    return kInvalidId;
}

void TextureViewService::Request(std::uint32_t id, const TextureView::Desc& desc)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    View& v = m_views[id];
    if (!v.live || v.releasePending) return;
    v.requested     = desc;
    v.requestedHash = TextureView::Hash(desc);
    v.hasRequest    = true;
}

void TextureViewService::Release(std::uint32_t id)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    View& v = m_views[id];
    if (v.live) v.releasePending = true;
}

void TextureViewService::RequestProbe(std::uint32_t id, std::uint32_t x, std::uint32_t y, int face, bool sideB)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    View& v = m_views[id];
    if (!v.live || v.releasePending) return;
    if (!v.probeWanted || v.probeX != x || v.probeY != y || v.probeFace != face || v.probeSideB != sideB)
        v.probeSeq = m_nextProbeSeq++;
    v.probeWanted = true;
    v.probeX      = x;
    v.probeY      = y;
    v.probeFace   = face;
    v.probeSideB  = sideB;
}

void TextureViewService::StopProbe(std::uint32_t id)
{
    if (id >= m_views.size()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_views[id].probeWanted = false;
}

TextureViewService::ProbeResult TextureViewService::GetProbe(std::uint32_t id) const
{
    ProbeResult r;
    if (id >= m_views.size()) return r;
    const View& v = m_views[id];
    std::uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        r.armed = v.probeWanted;
        seq     = v.probeSeq;
    }
    std::lock_guard<std::mutex> lk(m_statusMutex);
    r.valid = r.armed && v.probeSeqDone == seq && seq != 0;
    r.x     = v.probeDoneX;
    r.y     = v.probeDoneY;
    r.mip   = v.probeDoneMip;
    r.face  = v.probeDoneFace;
    r.sideB = v.probeDoneSideB;
    for (int c = 0; c < 4; ++c) r.value[c] = v.probeValue[c];
    r.frame = v.probeFrame;
    return r;
}

std::uint64_t TextureViewService::Srv(std::uint32_t id) const
{
    if (id >= m_views.size()) return 0;
    const View& v = m_views[id];
    return v.target.IsValid() ? m_gfx.GetTextureSRVGpuHandle(v.target) : 0;
}

const RHI::Texture* TextureViewService::Output(std::uint32_t id) const
{
    if (id >= m_views.size()) return nullptr;
    const View& v = m_views[id];
    return v.target.IsValid() ? &v.target : nullptr;
}

TextureView::Status TextureViewService::GetStatus(std::uint32_t id) const
{
    TextureView::Status s;
    if (id >= m_views.size()) return s;
    const View& v = m_views[id];
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        s.live     = v.live && !v.releasePending;
        s.descHash = v.hasRequest ? v.requestedHash : 0;
    }
    s.drawable = v.target.IsValid() && m_pass && m_pass->IsReady();
    s.width    = v.target.IsValid() ? v.target.desc.width : 0;
    s.height   = v.target.IsValid() ? v.target.desc.height : 0;
    s.kind     = v.kind;
    std::lock_guard<std::mutex> lk(m_statusMutex);
    s.renders         = v.renders;
    s.renderedHash    = v.renderedHash;
    s.lastRenderFrame = v.lastRenderFrame;
    s.why             = v.why;
    return s;
}

std::uint32_t TextureViewService::LiveCount() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::uint32_t n = 0;
    for (const View& v : m_views) n += (v.live && !v.releasePending) ? 1u : 0u;
    return n;
}

std::uint64_t TextureViewService::TargetBytes() const
{
    std::uint64_t n = 0;
    for (const View& v : m_views) n += v.target.IsValid() ? v.targetBytes : 0;
    return n;
}

// ==== Serial window ====
bool TextureViewService::EnsureTarget(View& v, std::uint32_t w, std::uint32_t h, std::uint32_t id)
{
    if (v.target.IsValid() && v.target.desc.width == w && v.target.desc.height == h) return false;
    DestroyTarget(v);
    const std::string name = "TextureView." + std::to_string(id);
    RHI::TextureDesc td{};
    td.width      = w;
    td.height     = h;
    td.depth      = 1;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName = name.c_str();
    if (!m_gfx.CreateTexture(td, v.target))
    {
        v.target = {};
        return false;
    }
    v.targetBytes = m_gfx.GetTextureAllocationBytes(v.target);
    return true;
}

void TextureViewService::DestroyTarget(View& v)
{
    // Frame-stamped deferred release (GraphicsDX12::DestroyTexture): the resource and its
    // descriptors outlive every frame already recorded, including an ImGui draw list that
    // captured the SRV.
    if (v.target.IsValid()) m_gfx.DestroyTexture(v.target);
    v.target      = {};
    v.targetBytes = 0;
}

void TextureViewService::ServiceFrameBoundary()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    bool anyLive = false;
    for (View& v : m_views)
    {
        if (v.releasePending)
        {
            DestroyTarget(v);
            v = View{};
            continue;
        }
        anyLive |= v.live;
    }
    m_anyLive = anyLive;
    if (!anyLive) return;
    if (!m_pass->IsReady() && !m_pass->EnsurePipeline())
    {
        std::lock_guard<std::mutex> sk(m_statusMutex);
        for (View& v : m_views)
            if (v.live) v.why = "the texture view pipeline did not build (see the log)";
        return;
    }

    // New targets draw in the frame they are made (ImGui must never sample an
    // unwritten one), so at most kMaxPerFrame are made per boundary; the rest
    // wait a frame with no target (Srv() = 0: the panel's "loading").
    std::uint32_t created = 0;
    for (std::uint32_t id = 0; id < m_views.size(); ++id)
    {
        View& v = m_views[id];
        // The probe intent, applied whatever else happens to the view.
        v.probeArmedApplied = v.live && v.probeWanted;
        v.probeXApplied     = v.probeX;
        v.probeYApplied     = v.probeY;
        v.probeFaceApplied  = v.probeFace;
        v.probeSideBApplied = v.probeSideB;
        v.probeSeqApplied   = v.probeSeq;
        if (!v.live || !v.hasRequest) continue;
        const TextureView::Desc& d = v.requested;
        const std::uint32_t w = std::clamp<std::uint32_t>(d.width, 1u, kMaxSize);
        const std::uint32_t h = std::clamp<std::uint32_t>(d.height, 1u, kMaxSize);
        const bool needsNew = !v.target.IsValid() || v.target.desc.width != w || v.target.desc.height != h;
        if (needsNew && created >= TextureViewPass::kMaxPerFrame) continue;
        const bool made = EnsureTarget(v, w, h, id);
        if (!v.target.IsValid())
        {
            std::lock_guard<std::mutex> sk(m_statusMutex);
            v.why = "the output target could not be created";
            continue;
        }
        if (made) ++created;
        if (!made && v.requestedHash == v.appliedHash) continue;

        v.applied     = d;
        v.appliedHash = v.requestedHash;
        v.dirty       = true;
        v.fresh       = v.fresh || made;

        // The source: its view type, its SRV, the shown mip's size - resolved
        // here from the desc the caller copied out of the TextureSystem.
        std::string why;
        v.kind      = d.source.IsValid() ? TextureView::KindOf(d.source.desc) : TextureView::SourceKind::Unsupported;
        v.sourceSrv = d.source.IsValid() ? m_gfx.GetTextureSRVGpuHandle(d.source) : 0;
        if (!d.source.IsValid())
            why = "no texture";
        else if (v.kind == TextureView::SourceKind::Unsupported)
            why = "this texture's view type is not shown (1D, MSAA or a cube array)";
        else if (v.sourceSrv == 0)
            why = "the texture has no shader resource view";
        const RHI::TextureDesc& sd = d.source.desc;
        const std::uint32_t mips = (std::max)(1u, sd.mipLevels);
        v.applied.mip = (std::min)(d.mip, mips - 1u);
        v.mipW = (std::max)(1u, sd.width >> v.applied.mip);
        v.mipH = (std::max)(1u, sd.height >> v.applied.mip);
        if (v.kind == TextureView::SourceKind::Cube)
            v.applied.slice = (std::min)(d.slice, 5u);
        else if (v.kind == TextureView::SourceKind::Tex2DArray)
            v.applied.slice = (std::min)(d.slice, (std::max)(1u, sd.arraySize) - 1u);
        else if (v.kind == TextureView::SourceKind::Tex3D)
            v.applied.slice = (std::min)(d.slice, (std::max)(1u, sd.depth >> v.applied.mip) - 1u);
        else
            v.applied.slice = 0;

        // The comparison's picture B, resolved the same way. One it cannot show turns the
        // comparison off - A still draws - and says why.
        v.kindB      = TextureView::SourceKind::Unsupported;
        v.sourceSrvB = 0;
        v.mipWB = v.mipHB = 0;
        if (d.compare != TextureView::Compare::Off)
        {
            const RHI::Texture& b = d.compareSource;
            std::string whyB;
            if (!b.IsValid())
                whyB = "no texture B";
            else
            {
                v.kindB      = TextureView::KindOf(b.desc);
                v.sourceSrvB = m_gfx.GetTextureSRVGpuHandle(b);
                if (v.kindB == TextureView::SourceKind::Unsupported)
                    whyB = "B's view type is not shown";
                else if (v.sourceSrvB == 0)
                    whyB = "B has no shader resource view";
                else if ((v.kindB == TextureView::SourceKind::Cube) != (v.kind == TextureView::SourceKind::Cube))
                    whyB = "A and B must both be cubes, or neither";
            }
            if (whyB.empty())
            {
                const std::uint32_t mipsB = (std::max)(1u, b.desc.mipLevels);
                v.applied.compareMip = (std::min)(d.compareMip, mipsB - 1u);
                v.mipWB = (std::max)(1u, b.desc.width >> v.applied.compareMip);
                v.mipHB = (std::max)(1u, b.desc.height >> v.applied.compareMip);
                if (v.kindB == TextureView::SourceKind::Cube)
                    v.applied.compareSlice = (std::min)(d.compareSlice, 5u);
                else if (v.kindB == TextureView::SourceKind::Tex2DArray)
                    v.applied.compareSlice = (std::min)(d.compareSlice, (std::max)(1u, b.desc.arraySize) - 1u);
                else if (v.kindB == TextureView::SourceKind::Tex3D)
                    v.applied.compareSlice =
                        (std::min)(d.compareSlice, (std::max)(1u, b.desc.depth >> v.applied.compareMip) - 1u);
                else
                    v.applied.compareSlice = 0;
            }
            else
            {
                v.applied.compare = TextureView::Compare::Off;
                v.kindB      = TextureView::SourceKind::Unsupported;
                v.sourceSrvB = 0;
                if (why.empty()) why = "compare: " + whyB;
            }
        }
        std::lock_guard<std::mutex> sk(m_statusMutex);
        v.why = why;
    }
}

void TextureViewService::ReloadShaders()
{
    m_pass->ReloadShaders();
    for (View& v : m_views)
        if (v.target.IsValid()) v.dirty = true;   // redraw with the new shader
}

void TextureViewService::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (View& v : m_views)
    {
        DestroyTarget(v);
        v = View{};
    }
    m_pass->Shutdown();
    m_anyLive = false;
}

// ==== Render half ====
void TextureViewService::Execute(RHI::CommandList cl, std::uint64_t frame)
{
    if (!m_anyLive || !m_pass->IsReady()) return;

    // This frame slot's probe results, recorded kFramesInFlight frames ago (the slot
    // BeginFrame just fenced); taken before this frame's probes overwrite it.
    const std::uint32_t frameSlot = m_gfx.GetFrameIndex() % RHI::kFramesInFlight;
    if (const float* res = m_pass->ProbeResults(frameSlot))
    {
        std::lock_guard<std::mutex> sk(m_statusMutex);
        for (std::uint32_t id = 0; id < m_views.size(); ++id)
        {
            ProbeRecord& rec = m_probeRecords[frameSlot][id];
            if (rec.seq == 0) continue;
            View& v = m_views[id];
            v.probeSeqDone  = rec.seq;
            v.probeDoneX    = rec.x;
            v.probeDoneY    = rec.y;
            v.probeDoneMip  = rec.mip;
            v.probeDoneFace = rec.face;
            v.probeDoneSideB = rec.sideB;
            v.probeFrame    = rec.frame;
            for (int c = 0; c < 4; ++c) v.probeValue[c] = res[id * 4 + c];
            rec = ProbeRecord{};
        }
    }

    std::uint32_t slot = 0;
    std::uint32_t timer = ~0u;
    // Opened at most once, closed iff opened. Not keyed on `timer`: with the profiler off
    // it stays ~0u, but BeginGPUTimestamp still opens a capture marker that
    // EndGPUTimestamp must close.
    bool timerOpen = false;
    // Fresh targets first: they MUST draw this frame (ServiceFrameBoundary made
    // at most kMaxPerFrame of them); the other dirty views take what is left
    // and the rest wait for the next frame.
    for (int pass = 0; pass < 2; ++pass)
    for (View& v : m_views)
    {
        if (!v.dirty || !v.target.IsValid()) continue;
        if ((pass == 0) != v.fresh) continue;
        if (slot >= TextureViewPass::kMaxPerFrame) break;
        if (!timerOpen) { timer = m_gfx.BeginGPUTimestamp(cl, "TextureView"); timerOpen = true; }
        TextureViewPass::Job job;
        job.output     = &v.target;
        job.sourceSrv  = v.sourceSrv;
        job.kind       = v.kind;
        job.mipW       = v.mipW;
        job.mipH       = v.mipH;
        job.srgb       = TextureView::IsSrgbFormat(v.applied.source.desc.format);
        job.oneChannel = TextureView::IsOneChannelFormat(v.applied.source.desc.format);
        job.desc       = v.applied;
        job.sourceSrvB  = v.sourceSrvB;
        job.kindB       = v.kindB;
        job.mipWB       = v.mipWB;
        job.mipHB       = v.mipHB;
        job.srgbB       = TextureView::IsSrgbFormat(v.applied.compareSource.desc.format);
        job.oneChannelB = TextureView::IsOneChannelFormat(v.applied.compareSource.desc.format);
        const bool ok = m_pass->Execute(cl, slot, job);
        v.dirty = false;
        v.fresh = false;
        if (!ok) continue;
        ++slot;
        std::lock_guard<std::mutex> sk(m_statusMutex);
        ++v.renders;
        v.renderedHash    = v.appliedHash;
        v.lastRenderFrame = frame;
    }
    // ---- This frame's probes (one thread each) ----
    TextureViewPass::Probe probes[TextureViewPass::kMaxProbesPerFrame];
    std::uint32_t          probeIds[TextureViewPass::kMaxProbesPerFrame];
    bool                   probeSideB[TextureViewPass::kMaxProbesPerFrame] = {};
    std::uint32_t          n = 0;
    for (std::uint32_t id = 0; id < m_views.size() && n < TextureViewPass::kMaxProbesPerFrame; ++id)
    {
        View& v = m_views[id];
        if (!v.probeArmedApplied || v.kind == TextureView::SourceKind::Unsupported || !v.sourceSrv) continue;
        TextureViewPass::Probe& p = probes[n];
        p.job.sourceSrv  = v.sourceSrv;
        p.job.kind       = v.kind;
        p.job.mipW       = v.mipW;
        p.job.mipH       = v.mipH;
        p.job.srgb       = TextureView::IsSrgbFormat(v.applied.source.desc.format);
        p.job.oneChannel = TextureView::IsOneChannelFormat(v.applied.source.desc.format);
        p.job.desc       = v.applied;
        p.job.desc.compare = TextureView::Compare::Off;
        // Picture B's texel instead: B written into A's fields.
        const bool sideB = v.probeSideBApplied && v.applied.compare != TextureView::Compare::Off && v.sourceSrvB &&
                           v.kindB != TextureView::SourceKind::Unsupported;
        if (sideB)
        {
            p.job.sourceSrv  = v.sourceSrvB;
            p.job.kind       = v.kindB;
            p.job.mipW       = v.mipWB;
            p.job.mipH       = v.mipHB;
            p.job.srgb       = TextureView::IsSrgbFormat(v.applied.compareSource.desc.format);
            p.job.oneChannel = TextureView::IsOneChannelFormat(v.applied.compareSource.desc.format);
            p.job.desc.mip   = v.applied.compareMip;
            p.job.desc.slice = v.applied.compareSlice;
        }
        if (v.probeFaceApplied >= 0) p.job.desc.slice = static_cast<std::uint32_t>(v.probeFaceApplied);
        p.x     = (std::min)(v.probeXApplied, p.job.mipW ? p.job.mipW - 1u : 0u);
        p.y     = (std::min)(v.probeYApplied, p.job.mipH ? p.job.mipH - 1u : 0u);
        p.index = id;
        probeSideB[n] = sideB;
        probeIds[n++] = id;
    }
    if (n)
    {
        if (!timerOpen) { timer = m_gfx.BeginGPUTimestamp(cl, "TextureView"); timerOpen = true; }
        const std::uint32_t done = m_pass->ExecuteProbes(cl, probes, n);
        for (std::uint32_t i = 0; i < done; ++i)
        {
            const View& v = m_views[probeIds[i]];
            ProbeRecord& rec = m_probeRecords[frameSlot][probeIds[i]];
            rec.seq   = v.probeSeqApplied;
            rec.x     = probes[i].x;
            rec.y     = probes[i].y;
            rec.mip   = probes[i].job.desc.mip;
            rec.face  = probes[i].job.desc.slice;
            rec.sideB = probeSideB[i];
            rec.frame = frame;
        }
    }
    if (timerOpen) m_gfx.EndGPUTimestamp(cl, timer);
}
