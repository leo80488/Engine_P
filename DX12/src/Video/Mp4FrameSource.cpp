#include "Video/Mp4FrameSource.h"
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"   // -video_syncupload, -video_swdecode
#include "System/Log.h"

// ==== FFmpeg implementation (WITH_FFMPEG only) ====
// The #else stub at the end of the file keeps the engine building without FFmpeg.

#ifdef WITH_FFMPEG

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include "Resource/AssetStreamAvio.h"   // a video sealed in game.ipak

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

#include <vector>

namespace Video {

class Mp4FrameSource final : public IDecodedFrameSource
{
public:
    bool Open(IGraphicsDevice& gfx, const std::string& path);
    ~Mp4FrameSource() override { Close(); }

    bool   Tick(IGraphicsDevice& gfx, VideoComponent& vc, double dtSeconds) override;
    void   Restart() override;
    void   Seek(double timeSeconds) override;
    double GetDurationSeconds() const override { return m_durationSec; }

private:
    void   Close();
    bool   EnsureNV12Texture(IGraphicsDevice& gfx, VideoComponent& vc);
    bool   UploadHwFrame(IGraphicsDevice& gfx, VideoComponent& vc);   // D3D12VA path
    bool   UploadSwFrame(IGraphicsDevice& gfx, VideoComponent& vc);   // software path

    static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx, const AVPixelFormat* pixelFormats);

    AVFormatContext* m_fmt        = nullptr;
    AVCodecContext*  m_codec      = nullptr;
    AVFrame*         m_frame      = nullptr;
    AVFrame*         m_nv12Frame  = nullptr;   // sws output staging (sw path only)
    AVPacket*        m_packet     = nullptr;
    SwsContext*      m_sws        = nullptr;
    AVBufferRef*     m_hwDeviceCtx = nullptr;  // D3D12VA hwdevice (null when sw)
    bool             m_hwEnabled  = false;     // D3D12VA path actually being used
    int              m_videoIdx   = -1;
    AVRational       m_streamTb   { 0, 1 };    // stream time base
    int              m_width      = 0;
    int              m_height     = 0;
    double           m_durationSec = -1.0;
    bool             m_eos        = false;
    double           m_lastPts    = -1.0;
    bool             m_textureCreated = false;
    bool             m_ringChecked    = false;   // UploadSwFrame: does a frame fit the stream ring
    bool             m_hwFrameLogged  = false;   // UploadHwFrame: what the decoder's frames are, once
    Resource::AssetStreamAvio m_assetIo;          // m_fmt's reads when the video is in the pak
};

// Codec get_format callback: prefer AV_PIX_FMT_D3D12 when the codec offers it, else
// its first format. libavcodec calls it during avcodec_open2; returning a non-hw
// format falls back to software decode for the rest of the stream.
enum AVPixelFormat Mp4FrameSource::GetHwFormat(AVCodecContext* /*ctx*/,
                                                const AVPixelFormat* pixelFormats)
{
    for (const AVPixelFormat* p = pixelFormats; p && *p != AV_PIX_FMT_NONE; ++p)
    {
        if (*p == AV_PIX_FMT_D3D12) return AV_PIX_FMT_D3D12;
    }
    return (pixelFormats && pixelFormats[0] != AV_PIX_FMT_NONE) ? pixelFormats[0]
                                                        : AV_PIX_FMT_NONE;
}

bool Mp4FrameSource::Open(IGraphicsDevice& gfx, const std::string& path)
{
    // A video in the pak is read through AssetFS. FFmpeg opens a loose one by path: resolve it
    // the way every asset resolves (the engine root, a worktree's shared root - Resource/
    // SharedContent.h); as given when no root has it (an absolute path).
    std::string openPath = path;
    if (Resource::AssetFS::Get().HasInPak(path))
    {
        m_fmt = avformat_alloc_context();
        if (!m_fmt || !m_assetIo.Open(path))
        {
            LOG_ERROR("Mp4FrameSource: cannot read '%s' from the pak", path.c_str());
            Close();
            return false;
        }
        m_fmt->pb = m_assetIo.Context();
        m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    else if (std::string onDisk = Resource::AssetFS::Get().ResolveOnDisk(path); !onDisk.empty())
    {
        openPath = std::move(onDisk);
    }
    if (avformat_open_input(&m_fmt, openPath.c_str(), nullptr, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avformat_open_input failed on '%s'", openPath.c_str());
        return false;
    }
    if (avformat_find_stream_info(m_fmt, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avformat_find_stream_info failed");
        Close();
        return false;
    }

    const AVCodec* dec = nullptr;
    m_videoIdx = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_videoIdx < 0 || !dec)
    {
        LOG_ERROR("Mp4FrameSource: no video stream / decoder for '%s'", path.c_str());
        Close();
        return false;
    }

    AVStream* stream = m_fmt->streams[m_videoIdx];
    m_streamTb = stream->time_base;
    m_durationSec = (stream->duration > 0 && stream->time_base.den > 0)
        ? double(stream->duration) * av_q2d(stream->time_base)
        : (m_fmt->duration > 0 ? double(m_fmt->duration) / AV_TIME_BASE : -1.0);

    m_codec = avcodec_alloc_context3(dec);
    if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0)
    {
        LOG_ERROR("Mp4FrameSource: codec context init failed");
        Close();
        return false;
    }

    // ---- D3D12VA hwaccel on the engine's own ID3D12Device ----
    // The hwdevice context is allocated by hand (not av_hwdevice_ctx_create) so FFmpeg
    // uses our device instead of creating a second one. Any failure here falls through
    // to software decode.
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    // -video_swdecode: software decode even where D3D12VA works (the video probe tests
    // both upload paths on one machine).
    static const bool s_swOnly = Sys::CommandLine::Get().Has("-video_swdecode");
    if (dx12 && dx12->GetDevice() && !s_swOnly)
    {
        m_hwDeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D12VA);
        if (m_hwDeviceCtx)
        {
            auto* hwctx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
            auto* d3d12 = (AVD3D12VADeviceContext*)hwctx->hwctx;

            // av_hwdevice_ctx_init takes ownership of the device and Releases it on shutdown:
            // AddRef ours so the engine's ID3D12Device survives that Release.
            d3d12->device = dx12->GetDevice();
            d3d12->device->AddRef();

            if (av_hwdevice_ctx_init(m_hwDeviceCtx) == 0)
            {
                m_codec->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                m_codec->get_format    = &Mp4FrameSource::GetHwFormat;
                m_hwEnabled = true;
                LOG_INFO("Mp4FrameSource: D3D12VA hwaccel enabled (codec=%s)", dec->name);
            }
            else
            {
                LOG_WARNING("Mp4FrameSource: av_hwdevice_ctx_init failed — "
                            "falling back to software decode");
                av_buffer_unref(&m_hwDeviceCtx);
                m_hwDeviceCtx = nullptr;
            }
        }
        else
        {
            LOG_WARNING("Mp4FrameSource: av_hwdevice_ctx_alloc(D3D12VA) failed");
        }
    }

    if (avcodec_open2(m_codec, dec, nullptr) < 0)
    {
        LOG_ERROR("Mp4FrameSource: avcodec_open2 failed");
        Close();
        return false;
    }

    m_width  = m_codec->width;
    m_height = m_codec->height;
    if ((m_width & 1) || (m_height & 1))
    {
        LOG_ERROR("Mp4FrameSource: odd dimensions %dx%d (NV12 needs even)",
                  m_width, m_height);
        Close();
        return false;
    }

    m_frame      = av_frame_alloc();
    m_nv12Frame  = av_frame_alloc();
    m_packet     = av_packet_alloc();
    if (!m_frame || !m_nv12Frame || !m_packet) { Close(); return false; }

    // The sws_scale target, allocated once at the stream's size.
    m_nv12Frame->format = AV_PIX_FMT_NV12;
    m_nv12Frame->width  = m_width;
    m_nv12Frame->height = m_height;
    if (av_frame_get_buffer(m_nv12Frame, 32) < 0)
    {
        LOG_ERROR("Mp4FrameSource: NV12 frame buffer alloc failed");
        Close();
        return false;
    }

    LOG_SUCCESS("Mp4FrameSource: opened '%s' %dx%d codec=%s duration=%.3fs",
                path.c_str(), m_width, m_height, dec->name, m_durationSec);
    return true;
}

void Mp4FrameSource::Close()
{
    if (m_sws)         { sws_freeContext(m_sws); m_sws = nullptr; }
    if (m_packet)      { av_packet_free(&m_packet); }
    if (m_nv12Frame)   { av_frame_free(&m_nv12Frame); }
    if (m_frame)       { av_frame_free(&m_frame); }
    if (m_codec)       { avcodec_free_context(&m_codec); }
    if (m_hwDeviceCtx) { av_buffer_unref(&m_hwDeviceCtx); }
    if (m_fmt)         { avformat_close_input(&m_fmt); }
    m_assetIo.Close();   // after m_fmt: avformat_close_input does not free a custom context
    m_hwEnabled = false;
    m_textureCreated = false;
    m_eos = false;
    m_lastPts = -1.0;
}

void Mp4FrameSource::Restart()
{
    if (!m_fmt) return;
    av_seek_frame(m_fmt, m_videoIdx, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_eos = false;
    m_lastPts = -1.0;
}

void Mp4FrameSource::Seek(double timeSeconds)
{
    if (!m_fmt) return;
    if (timeSeconds < 0.0) timeSeconds = 0.0;
    const int64_t ts = static_cast<int64_t>(timeSeconds / av_q2d(m_streamTb));
    av_seek_frame(m_fmt, m_videoIdx, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    m_eos = false;
    m_lastPts = -1.0;
}

bool Mp4FrameSource::EnsureNV12Texture(IGraphicsDevice& gfx, VideoComponent& vc)
{
    if (m_textureCreated && !vc.dpb.empty() && vc.dpb[0].IsValid())
        return true;

    // Free anything that was there before (test pattern, prior open, etc.).
    for (auto& tex : vc.dpb) if (tex.IsValid()) gfx.DestroyTexture(tex);
    vc.dpb.clear();
    vc.dpb.resize(1);

    RHI::TextureDesc td{};
    td.type        = RHI::TextureDesc::Type::TEXTURE_2D;
    td.format      = RHI::Format::NV12;
    td.usage       = RHI::Usage::DEFAULT;
    td.width       = static_cast<uint32_t>(m_width);
    td.height      = static_cast<uint32_t>(m_height);
    td.arraySize  = 1;
    td.mipLevels  = 1;
    td.bindFlags  = RHI::BindFlag::SHADER_RESOURCE;
    td.miscFlags  = RHI::ResourceMiscFlag::VIDEO_DECODE
                   | RHI::ResourceMiscFlag::VIDEO_COMPATIBILITY_H264;
    td.layout      = RHI::ResourceState::SHADER_RESOURCE;
    td.debugName  = "Mp4FrameSource.NV12";

    if (!gfx.CreateTexture(td, vc.dpb[0], nullptr))
    {
        LOG_ERROR("Mp4FrameSource: NV12 dpb texture create failed");
        vc.dpb.clear();
        return false;
    }
    vc.currentDpbSlot = 0;
    vc.width  = static_cast<uint32_t>(m_width);
    vc.height = static_cast<uint32_t>(m_height);
    m_textureCreated = true;
    return true;
}

// D3D12VA path: the decoded frame is already an NV12 ID3D12Resource on the engine's
// device; GPU-copy it into vc.dpb[0].
bool Mp4FrameSource::UploadHwFrame(IGraphicsDevice& gfx, VideoComponent& vc)
{
    auto* d3d12Frame = reinterpret_cast<AVD3D12VAFrame*>(m_frame->data[0]);
    if (!d3d12Frame || !d3d12Frame->texture)
    {
        LOG_ERROR("Mp4FrameSource: hwaccel frame has no texture");
        return false;
    }
    auto* dx12 = dynamic_cast<GraphicsDX12*>(&gfx);
    if (!dx12)
    {
        LOG_ERROR("Mp4FrameSource: hwaccel requires GraphicsDX12 backend");
        return false;
    }
    if (!m_hwFrameLogged)
    {
        // Log what the decoder hands out, once. Its frames are textures of their own at the
        // picture's size, so CopyResource fits them: FFmpeg's texture-array mode is opt-in
        // (never requested here) and its D3D12VA decoder writes subresource 0 of the frame's
        // texture either way. A frame that does not fit is refused.
        m_hwFrameLogged = true;
        const D3D12_RESOURCE_DESC sd = d3d12Frame->texture->GetDesc();
        LOG_INFO("Mp4FrameSource: D3D12VA frames are %s (%llux%u, %u slice(s))",
                 (d3d12Frame->flags & AV_D3D12VA_FRAME_FLAG_TEXTURE_ARRAY) ? "slices of a texture array"
                                                                           : "textures of their own",
                 static_cast<unsigned long long>(sd.Width), sd.Height, sd.DepthOrArraySize);
    }
    static const bool s_sync = Sys::CommandLine::Get().Has("-video_syncupload");
    if (s_sync)
        return dx12->CopyD3D12ResourceToTexture(vc.dpb[0], d3d12Frame->texture, d3d12Frame->sync_ctx.fence,
                                                d3d12Frame->sync_ctx.fence_value);

    // The copy is recorded into this frame's StreamUpload list, the graphics queue waits
    // for the decode's fence before it, and the CPU never waits. The decoder recycles its
    // surfaces, so the copy holds a reference to this frame until its slot retires.
    // LatestWins: a decode-ahead loop copies only its last frame. The surface is in COMMON
    // after the decode (see CopyD3D12ResourceToTexture) and the copy reads it there, with
    // no barrier of ours.
    // Accepted race: with no CPU wait, the copy of frame N can run on the graphics queue
    // while the decoder's queue decodes N+1 with N as a reference - two queues reading one
    // surface. Both only read a finished frame (the fence wait; keepAlive stops its reuse
    // as an output), which is how players sample decoded surfaces, and our queue never
    // transitions it. -video_syncupload uses a serialising CPU wait instead.
    AVFrame* ref = av_frame_alloc();
    if (!ref || av_frame_ref(ref, m_frame) < 0)
    {
        av_frame_free(&ref);
        LOG_ERROR("Mp4FrameSource: could not keep the decoded frame for its copy");
        return false;
    }
    std::shared_ptr<void> keep(ref, [](void* p) { AVFrame* f = static_cast<AVFrame*>(p); av_frame_free(&f); });
    const StreamUpload::Result r = dx12->QueueNativeTextureCopy(
        vc.dpb[0], d3d12Frame->texture, d3d12Frame->sync_ctx.fence, d3d12Frame->sync_ctx.fence_value,
        std::move(keep), StreamUpload::Policy::LatestWins);
    return r != StreamUpload::Result::Rejected && r != StreamUpload::Result::Dropped;
}

// Software decode fallback: sws_scale into an NV12 frame, then queue both planes with
// QueueTextureUpload, LatestWins, so a decode-ahead loop that catches up several
// frames in one tick copies only the last one and nothing waits for the GPU.
// -video_syncupload uses UpdateTexture instead (a CPU wait per frame).
bool Mp4FrameSource::UploadSwFrame(IGraphicsDevice& gfx, VideoComponent& vc)
{
    if (!m_sws || m_codec->pix_fmt != m_frame->format)
    {
        if (m_sws) { sws_freeContext(m_sws); m_sws = nullptr; }
        m_sws = sws_getContext(m_width, m_height, (AVPixelFormat)m_frame->format,
                               m_width, m_height, AV_PIX_FMT_NV12,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws)
        {
            LOG_ERROR("Mp4FrameSource: sws_getContext failed (pix_fmt=%d)",
                      m_frame->format);
            return false;
        }
    }
    sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_height,
              m_nv12Frame->data, m_nv12Frame->linesize);

    RHI::SubresourceData planes[2]{};
    planes[0].dataPtr    = m_nv12Frame->data[0];
    planes[0].rowPitch   = static_cast<uint32_t>(m_nv12Frame->linesize[0]);
    planes[0].slicePitch = planes[0].rowPitch * static_cast<uint32_t>(m_height);
    planes[1].dataPtr    = m_nv12Frame->data[1];
    planes[1].rowPitch   = static_cast<uint32_t>(m_nv12Frame->linesize[1]);
    planes[1].slicePitch = planes[1].rowPitch * static_cast<uint32_t>(m_height / 2);

    static const bool s_sync = Sys::CommandLine::Get().Has("-video_syncupload");
    if (s_sync) return gfx.UpdateTexture(vc.dpb[0], planes, 2);
    StreamUpload::Desc d{};
    d.planes     = planes;
    d.planeCount = 2;
    d.policy     = StreamUpload::Policy::LatestWins;
    const StreamUpload::Result r = gfx.QueueTextureUpload(vc.dpb[0], d);
    // A LatestWins plane that does not fit its frame's ring is dropped (at once, or when
    // carried over to the next frame), so a frame larger than the ring (4K NV12 is 12.4 MB,
    // the default ring 8 MiB) never lands whole: the luma freezes while the chroma moves
    // and only the dropped counter shows it. Warn once, as soon as the uploader exists
    // (the first request creates it).
    if (!m_ringChecked)
    {
        const StreamUpload::Stats st = gfx.GetStreamUploadStats();
        if (st.active)
        {
            m_ringChecked = true;
            // What the ring holds for it: each plane's rows at the copy's
            // 256-byte pitch (both NV12 planes are m_width bytes a row), each
            // placement 512-aligned - not the decoder's linesizes.
            const uint64_t pitch      = (uint64_t(m_width) + 255) & ~uint64_t(255);
            const uint64_t frameBytes = ((pitch * uint64_t(m_height) + 511) & ~uint64_t(511)) +
                                        ((pitch * uint64_t(m_height / 2) + 511) & ~uint64_t(511));
            if (frameBytes > st.ringBytesPerSlot)
                LOG_WARNING("Mp4FrameSource: a %dx%d frame (%llu bytes) does not fit the stream upload ring "
                            "(%llu bytes a frame), so its software-decoded frames never land whole - raise "
                            "engine.json upload.streamRingMiB (or decode on the GPU)", m_width, m_height,
                            static_cast<unsigned long long>(frameBytes),
                            static_cast<unsigned long long>(st.ringBytesPerSlot));
        }
    }
    return r != StreamUpload::Result::Rejected && r != StreamUpload::Result::Dropped;
}

bool Mp4FrameSource::Tick(IGraphicsDevice& gfx, VideoComponent& vc, double /*dt*/)
{
    if (!m_fmt) return false;

    // EOS: honour VideoComponent::loop - restart when loop is on, else hold the last
    // frame until the user calls Restart() / Stop().
    if (m_eos)
    {
        if (!vc.loop) return true;
        Restart();
        vc.clockSeconds   = 0.0;
        vc.lastDecodedPts = -1.0;
    }

    if (!EnsureNV12Texture(gfx, vc)) return false;

    // Decode-ahead loop: read packets until decoded frame PTS catches up
    // with `vc.clockSeconds`. Stops as soon as we have a frame at or past
    // the clock so subsequent ticks can advance one frame at a time.
    while (true)
    {
        const double target = vc.clockSeconds;
        if (m_lastPts >= 0.0 && m_lastPts >= target) break;

        int ret = av_read_frame(m_fmt, m_packet);
        if (ret == AVERROR_EOF)
        {
            // EOF: send a null packet to flush the decoder, then drain its frames.
            avcodec_send_packet(m_codec, nullptr);
            ret = avcodec_receive_frame(m_codec, m_frame);
            if (ret == 0)
            {
                m_lastPts = m_frame->pts * av_q2d(m_streamTb);
                vc.lastDecodedPts = m_lastPts;
                ++vc.framesDecoded;
                if (m_frame->format == AV_PIX_FMT_D3D12) UploadHwFrame(gfx, vc);
                else                                     UploadSwFrame(gfx, vc);
                continue;
            }
            // True end of stream: return true to keep the source alive; the next Tick restarts
            // it when vc.loop is set, else holds the last frame.
            m_eos = true;
            return true;
        }
        if (ret < 0)
        {
            LOG_ERROR("Mp4FrameSource: av_read_frame error %d", ret);
            return false;
        }

        if (m_packet->stream_index != m_videoIdx)
        {
            av_packet_unref(m_packet);
            continue;
        }

        if (avcodec_send_packet(m_codec, m_packet) < 0)
        {
            av_packet_unref(m_packet);
            continue;
        }
        av_packet_unref(m_packet);

        while (avcodec_receive_frame(m_codec, m_frame) == 0)
        {
            m_lastPts = m_frame->pts * av_q2d(m_streamTb);
            vc.lastDecodedPts = m_lastPts;
            ++vc.framesDecoded;
            if (m_frame->format == AV_PIX_FMT_D3D12) UploadHwFrame(gfx, vc);
            else                                     UploadSwFrame(gfx, vc);
            if (m_lastPts >= target) break;
        }
    }
    return true;
}

std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& gfx, const std::string& path)
{
    auto src = std::make_shared<Mp4FrameSource>();
    if (!src->Open(gfx, path)) return nullptr;
    return src;
}

} // namespace Video

#else  // WITH_FFMPEG not defined

namespace Video {
std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& /*gfx*/, const std::string& path)
{
    LOG_WARNING("Video::OpenMp4('%s') called but WITH_FFMPEG is not defined. "
                "FFmpeg is vendored in external/ffmpeg; the Video module's project defines it.",
                path.c_str());
    return nullptr;
}
} // namespace Video

#endif // WITH_FFMPEG
