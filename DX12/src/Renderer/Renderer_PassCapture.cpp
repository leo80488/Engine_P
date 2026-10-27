// Renderer_PassCapture.cpp - a main-graph texture's content right after one
// pass (Renderer/PassCapture.h): the latched request, the frame boundary that
// makes the copy and the picture and arms the graph (RenderGraph::ArmTextureCopy),
// and the restore list that paints the picture and publishes what the graph did.
#include "Renderer/Renderer.h"
#include "Renderer/PassCapture.h"
#include "Graphics/FormatNames.h"
#include "RenderGraph/RenderPass/DebugTargetViewPass.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <mutex>
#include <string>
#include <utility>

namespace
{
    // A copy and a picture nobody asks for go after this many frame boundaries
    // (DestroyTexture is frame-stamped: a picture ImGui sampled outlives it).
    constexpr std::uint32_t kIdleFrames = 300;
    // A one-shot the graph keeps skipping (snapshot frames, no pass run) is given up.
    constexpr std::uint32_t kMaxTries = 8;
    // [RG_CAPTURE] of an every-frame capture: its first result, a change, and every this many frames.
    constexpr std::uint64_t kLogEvery = 60;

    bool SameShape(const RHI::Texture& a, const RHI::TextureDesc& b)
    {
        return a.IsValid() && a.desc.type == b.type && a.desc.width == b.width && a.desc.height == b.height &&
               a.desc.depth == b.depth && a.desc.arraySize == b.arraySize && a.desc.mipLevels == b.mipLevels &&
               a.desc.format == b.format && a.desc.bindFlags == b.bindFlags;
    }
}

// ==== Any thread ====
void Renderer::RequestPassCapture(const PassCapture::Desc& desc)
{
    std::lock_guard<std::mutex> lk(m_passCaptureMutex);
    m_passCapture.requested      = desc;
    m_passCapture.hasRequest     = true;
    m_passCapture.releasePending = false;
    m_passCapture.status.pending = true;
}

void Renderer::ReleasePassCapture()
{
    std::lock_guard<std::mutex> lk(m_passCaptureMutex);
    m_passCapture.hasRequest     = false;
    m_passCapture.releasePending = true;
    m_passCapture.status.pending = false;
}

PassCapture::Status Renderer::GetPassCaptureStatus() const
{
    std::lock_guard<std::mutex> lk(m_passCaptureMutex);
    return m_passCapture.status;
}

// Read in the serial window: stable once the render lane is drained.
const RHI::Texture* Renderer::GetPassCaptureCopy(RHI::ResourceState& state) const
{
    state = m_passCapture.copyState;
    return m_passCapture.copy.IsValid() ? &m_passCapture.copy : nullptr;
}

void Renderer::PublishPassCapture(PassCapture::Status st)
{
    PassCaptureState& s = m_passCapture;
    st.seq = ++s.seq;
    const bool everyFrame = s.applied.everyFrame;
    const std::string key = std::string(st.done ? "1" : "0") + "|" + st.texture + "|" + st.afterPass + "|" + st.why;
    const bool log = key != s.loggedKey || !everyFrame || st.frame >= s.loggedFrame + kLogEvery;
    if (log)
    {
        s.loggedKey   = key;
        s.loggedFrame = st.frame;
        LOG_INFO("[RG_CAPTURE] frame=%llu texture=%s after=%s done=%d size=%ux%u format=%s picture=%d why=%s",
                 static_cast<unsigned long long>(st.frame), st.texture.empty() ? "-" : st.texture.c_str(),
                 st.afterPass.empty() ? "-" : st.afterPass.c_str(), st.done ? 1 : 0, st.width, st.height,
                 st.format ? RHI::FormatName(static_cast<RHI::Format>(st.format)) : "-", st.pictureSrv != 0 ? 1 : 0,
                 st.why.empty() ? "-" : st.why.c_str());
    }
    std::lock_guard<std::mutex> lk(m_passCaptureMutex);
    st.pending = st.pending || s.hasRequest;   // a request latched since this one was applied
    s.status   = std::move(st);
}

// ==== The frame boundary (ServiceEnableGatedResources; render lane parked) ====
// Asked before the passes' OnService, which builds the debug target shader the
// picture is painted with: a request latched since, or the one applied.
bool Renderer::PassCapturePictureWanted() const
{
    std::lock_guard<std::mutex> lk(m_passCaptureMutex);
    if (m_passCapture.hasRequest) return m_passCapture.requested.picture;
    return m_passCapture.live && m_passCapture.applied.picture && !m_passCapture.releasePending;
}

void Renderer::ServicePassCapture()
{
    PassCaptureState& s = m_passCapture;
#if WITH_HARNESS
    // -rgcapture=<texture>[@<pass>]: capture every frame from the first boundary
    // on (the rg_capture pack; the A/B proving the copy changes no picture).
    static bool s_flagRead = false;
    if (!s_flagRead)
    {
        s_flagRead = true;
        const std::string spec = Sys::CommandLine::Get().Get("-rgcapture");
        if (!spec.empty())
        {
            PassCapture::Desc d;
            const size_t at = spec.find('@');
            d.texture    = spec.substr(0, at);
            d.afterPass  = at == std::string::npos ? std::string() : spec.substr(at + 1);
            d.everyFrame = true;
            RequestPassCapture(d);
            LOG_INFO("[RG_CAPTURE] -rgcapture: '%s' after %s, every frame", d.texture.c_str(),
                     d.afterPass.empty() ? "the last pass that runs" : d.afterPass.c_str());
        }
    }
#endif
    s.armed = false;
    bool have = false, release = false;
    PassCapture::Desc req;
    {
        std::lock_guard<std::mutex> lk(m_passCaptureMutex);
        if (s.hasRequest)
        {
            req          = s.requested;
            s.hasRequest = false;
            have         = true;
        }
        release = std::exchange(s.releasePending, false);
    }
    if (have)
    {
        s.applied = std::move(req);
        s.live    = true;
        s.tries   = 0;
    }
    else if (release)
        s.live = false;

    if (!s.live)
    {
        if ((s.copy.IsValid() || s.picture.IsValid()) && ++s.idle > kIdleFrames)
        {
            if (s.copy.IsValid())    m_gfx.DestroyTexture(s.copy);
            if (s.picture.IsValid()) m_gfx.DestroyTexture(s.picture);
            s.copy         = {};
            s.picture      = {};
            s.copyState    = RHI::ResourceState::UNDEFINED;
            s.pictureState = RHI::ResourceState::SHADER_RESOURCE;
            s.idle         = 0;
        }
        return;
    }
    s.idle = 0;

    // The texture, against the graph as compiled now (Execute resolves the
    // names again after its own Compile and refuses a copy that no longer fits).
    auto refuse = [&](std::string why)
    {
        PassCapture::Status st;
        st.id        = s.applied.id;
        st.texture   = s.applied.texture;
        st.afterPass = s.applied.afterPass;
        st.why       = std::move(why);
        PublishPassCapture(std::move(st));
        if (!s.applied.everyFrame) s.live = false;
    };
    const RG::RGTextureHandle h = m_graph.FindTexture(s.applied.texture);
    const RHI::Texture* tex = h.IsValid() ? m_graph.InspectPhysicalTexture(h) : nullptr;
    if (!h.IsValid()) return refuse("no graph texture '" + s.applied.texture + "'");
    if (!s.applied.afterPass.empty() && m_graph.FindPass(s.applied.afterPass) == ~0u)
        return refuse("no pass '" + s.applied.afterPass + "'");
    if (!tex)
    {
        // No texture to take the desc from yet: an import binds its texture in
        // Execute (none before the first frame), an on-demand one is made when
        // its producer asks. The next boundary looks again.
        PassCapture::Status st;
        st.id        = s.applied.id;
        st.texture   = s.applied.texture;
        st.afterPass = s.applied.afterPass;
        st.pending   = true;
        st.why       = "'" + s.applied.texture + "' has no texture yet (an import before its first frame, or an "
                       "on-demand texture nothing asked for)";
        if (!s.applied.everyFrame && ++s.tries >= kMaxTries)
        {
            s.live     = false;
            st.pending = false;
            st.why    += " - given up after " + std::to_string(kMaxTries) + " frames";
        }
        PublishPassCapture(std::move(st));
        return;
    }

    // The copy: the texture's own desc (a rebuilt texture gets a new copy).
    if (!SameShape(s.copy, tex->desc))
    {
        if (s.copy.IsValid()) m_gfx.DestroyTexture(s.copy);
        s.copy = {};
        RHI::TextureDesc d = tex->desc;
        const std::string name = "RG.Capture." + s.applied.texture;
        d.debugName = name.c_str();
        if (!m_gfx.CreateTexture(d, s.copy))
        {
            s.copy = {};
            return refuse("its copy could not be made");
        }
        s.copyState = d.layout;
    }
    // The picture (the Render Graph panel): RGBA8 at the copy's size, painted on
    // the restore list after the graph with the debug target shader (its pipeline
    // built by the pass's OnService: PassCapturePictureWanted).
    if (s.applied.picture)
    {
        if (!s.picture.IsValid() || s.picture.desc.width != tex->desc.width ||
            s.picture.desc.height != tex->desc.height)
        {
            if (s.picture.IsValid()) m_gfx.DestroyTexture(s.picture);
            s.picture = {};
            RHI::TextureDesc pd{};
            pd.width      = tex->desc.width;
            pd.height     = tex->desc.height;
            pd.depth      = 1;
            pd.mipLevels = 1;
            pd.format     = RHI::Format::R8G8B8A8_UNORM;
            pd.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
            pd.layout     = RHI::ResourceState::SHADER_RESOURCE;
            pd.debugName = "RG.Capture.Picture";
            if (!m_gfx.CreateTexture(pd, s.picture)) s.picture = {};
            s.pictureState = RHI::ResourceState::SHADER_RESOURCE;
        }
    }
    m_graph.ArmTextureCopy(s.applied.texture, s.applied.afterPass, &s.copy, &s.copyState);
    s.armed = true;
}

// ==== The render half: the restore list, after the graph ====
void Renderer::ExecutePassCapture(RHI::CommandList cl)
{
    PassCaptureState& s = m_passCapture;
    if (!s.armed) return;
    s.armed = false;
    const RG::RenderGraph::TextureCopyResult r = m_graph.TakeTextureCopyResult();

    PassCapture::Status st;
    st.id        = s.applied.id;
    st.texture   = s.applied.texture;
    st.afterPass = r.armed ? r.afterPass : s.applied.afterPass;
    st.frame     = r.armed ? r.frame : static_cast<std::uint64_t>(FrameViewRead().currentFrame);
    if (s.copy.IsValid())
    {
        st.width     = s.copy.desc.width;
        st.height    = s.copy.desc.height;
        st.arraySize = s.copy.desc.arraySize;
        st.format    = static_cast<std::uint32_t>(s.copy.desc.format);
        st.depth     = RHI::HasFlag(s.copy.desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL);
    }
    // A frame the graph skipped (a snapshot frame) or ran no pass in: a
    // one-shot is armed again, a few times.
    auto retry = [&](std::string why)
    {
        st.pending = true;
        st.why     = std::move(why);
        if (!s.applied.everyFrame && ++s.tries >= kMaxTries)
        {
            s.live     = false;
            st.pending = false;
            st.why    += " - given up after " + std::to_string(kMaxTries) + " frames";
        }
    };
    if (!r.armed)
        retry("the graph ran no pass this frame");
    else if (r.deferred)
        retry(r.why);
    else if (!r.done)
    {
        st.why = r.why;
        if (!s.applied.everyFrame) s.live = false;
    }
    else
    {
        st.done = true;
        if (!s.applied.everyFrame) s.live = false;
        if (s.applied.picture)
        {
            if (!s.picture.IsValid())
                st.pictureWhy = "the picture could not be made";
            else
            {
                const std::uint32_t region = m_gfx.BeginGPUTimestamp(cl, "RG.CapturePicture");
                if (PaintPassCapture(cl, s.applied.texture, s.copy, s.copyState, s.applied.plane, s.applied.view,
                                     s.picture, s.pictureState, st.shown, st.pictureWhy))
                {
                    st.pictureSrv = m_gfx.GetTextureSRVGpuHandle(s.picture);
                    st.pictureW   = s.picture.desc.width;
                    st.pictureH   = s.picture.desc.height;
                }
                m_gfx.EndGPUTimestamp(cl, region);
            }
        }
    }
    PublishPassCapture(std::move(st));
}

void Renderer::ShutdownPassCapture()
{
    PassCaptureState& s = m_passCapture;
    if (s.copy.IsValid())    m_gfx.DestroyTexture(s.copy);
    if (s.picture.IsValid()) m_gfx.DestroyTexture(s.picture);
    s.copy    = {};
    s.picture = {};
    s.live    = false;
    s.armed   = false;
}
