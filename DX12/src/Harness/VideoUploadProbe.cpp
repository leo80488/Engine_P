// Harness/VideoUploadProbe.cpp - see the header for what it proves.
#include "Harness/VideoUploadProbe.h"

#ifdef WITH_HARNESS
#include "ECS/ECS.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"   // -video_probe_world's transform
#include "Video/Mp4FrameSource.h"
#include "ECS/VideoComponent.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/UploadStats.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <cstdlib>
#include <cstring>

#ifdef WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}
#endif

namespace Harness
{
    namespace
    {
        uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

#ifdef WITH_FFMPEG
        // The clip's frame f: a diagonal ramp that moves, chroma that steps. Any
        // content will do - the check compares decoders, not these values.
        void FillFrame(AVFrame* fr, uint32_t f)
        {
            for (int y = 0; y < fr->height; ++y)
            {
                uint8_t* row = fr->data[0] + static_cast<ptrdiff_t>(y) * fr->linesize[0];
                for (int x = 0; x < fr->width; ++x)
                    row[x] = static_cast<uint8_t>((x + 2 * y + 7 * static_cast<int>(f)) & 0xFF);
            }
            const uint8_t u = static_cast<uint8_t>(64 + 4 * f), v = static_cast<uint8_t>(192 - 4 * f);
            for (int y = 0; y < fr->height / 2; ++y)
            {
                if (fr->format == AV_PIX_FMT_NV12)
                {
                    uint8_t* row = fr->data[1] + static_cast<ptrdiff_t>(y) * fr->linesize[1];
                    for (int x = 0; x < fr->width / 2; ++x) { row[2 * x] = u; row[2 * x + 1] = v; }
                }
                else
                {
                    std::memset(fr->data[1] + static_cast<ptrdiff_t>(y) * fr->linesize[1], u, fr->width / 2);
                    std::memset(fr->data[2] + static_cast<ptrdiff_t>(y) * fr->linesize[2], v, fr->width / 2);
                }
            }
        }

        // An H.264 (else MPEG-4) mp4 of @p frames frames at @p path.
        bool EncodeClip(const std::string& path, uint32_t w, uint32_t h, uint32_t frames, uint32_t fps,
                        std::string& encoder)
        {
            static const char* kEncoders[] = { "libx264", "libopenh264", "h264_mf", "mpeg4" };
            const AVCodec* enc = nullptr;
            for (const char* n : kEncoders)
                if ((enc = avcodec_find_encoder_by_name(n)) != nullptr) { encoder = n; break; }
            if (!enc) return false;

            AVFormatContext* fmt = nullptr;
            if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str()) < 0 || !fmt) return false;
            AVCodecContext* c  = avcodec_alloc_context3(enc);
            AVStream*       st = avformat_new_stream(fmt, nullptr);
            AVFrame*        fr = av_frame_alloc();
            AVPacket*       pk = av_packet_alloc();
            bool ok = c && st && fr && pk;
            if (ok)
            {
                c->width        = static_cast<int>(w);
                c->height       = static_cast<int>(h);
                c->time_base    = AVRational{ 1, static_cast<int>(fps) };
                c->framerate    = AVRational{ static_cast<int>(fps), 1 };
                c->gop_size     = 12;
                c->max_b_frames = 0;
                // yuv420p, else nv12: the two FillFrame writes.
                c->pix_fmt = AV_PIX_FMT_NONE;
                const void* cfg = nullptr;
                int n = 0;
                if (avcodec_get_supported_config(c, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &cfg, &n) >= 0 && cfg && n > 0)
                {
                    const auto* pf = static_cast<const AVPixelFormat*>(cfg);
                    for (int i = 0; i < n; ++i) if (pf[i] == AV_PIX_FMT_YUV420P) c->pix_fmt = AV_PIX_FMT_YUV420P;
                    if (c->pix_fmt == AV_PIX_FMT_NONE)
                        for (int i = 0; i < n; ++i) if (pf[i] == AV_PIX_FMT_NV12) c->pix_fmt = AV_PIX_FMT_NV12;
                }
                else c->pix_fmt = AV_PIX_FMT_YUV420P;   // no list: any format
                ok = c->pix_fmt != AV_PIX_FMT_NONE;
                if (ok && (fmt->oformat->flags & AVFMT_GLOBALHEADER)) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                AVDictionary* opts = nullptr;
                if (encoder == "libx264")
                {
                    av_dict_set(&opts, "preset", "ultrafast", 0);
                    av_dict_set(&opts, "tune", "zerolatency", 0);
                }
                ok = ok && avcodec_open2(c, enc, &opts) >= 0;
                av_dict_free(&opts);
            }
            ok = ok && avcodec_parameters_from_context(st->codecpar, c) >= 0;
            if (ok) st->time_base = c->time_base;
            ok = ok && avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) >= 0;
            ok = ok && avformat_write_header(fmt, nullptr) >= 0;
            if (ok)
            {
                fr->format = c->pix_fmt;
                fr->width  = c->width;
                fr->height = c->height;
                ok = av_frame_get_buffer(fr, 32) >= 0;
            }
            for (uint32_t i = 0; ok && i <= frames; ++i)
            {
                AVFrame* in = nullptr;   // the last pass flushes
                if (i < frames)
                {
                    ok = av_frame_make_writable(fr) >= 0;
                    FillFrame(fr, i);
                    fr->pts = i;
                    in = fr;
                }
                ok = ok && avcodec_send_frame(c, in) >= 0;
                while (ok && avcodec_receive_packet(c, pk) == 0)
                {
                    av_packet_rescale_ts(pk, c->time_base, st->time_base);
                    pk->stream_index = st->index;
                    ok = av_interleaved_write_frame(fmt, pk) >= 0;
                }
            }
            if (ok) ok = av_write_trailer(fmt) >= 0;
            if (fmt->pb) avio_closep(&fmt->pb);
            av_packet_free(&pk);
            av_frame_free(&fr);
            avcodec_free_context(&c);
            avformat_free_context(fmt);
            return ok;
        }

        // The clip's LAST frame through a software decoder, as NV12 rows
        // (@p y: w x h, @p uv: w x h/2, interleaved).
        bool DecodeLastFrame(const std::string& path, std::vector<uint8_t>& y, std::vector<uint8_t>& uv,
                             uint32_t& decoded)
        {
            AVFormatContext* fmt = nullptr;
            if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
            const AVCodec* dec = nullptr;
            const int idx = avformat_find_stream_info(fmt, nullptr) >= 0
                ? av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0) : -1;
            AVCodecContext* c  = (idx >= 0 && dec) ? avcodec_alloc_context3(dec) : nullptr;
            AVFrame*        fr = av_frame_alloc();
            AVPacket*       pk = av_packet_alloc();
            bool ok = c && fr && pk && avcodec_parameters_to_context(c, fmt->streams[idx]->codecpar) >= 0 &&
                      avcodec_open2(c, dec, nullptr) >= 0;
            decoded = 0;
            bool formatOk = true;
            auto keep = [&](const AVFrame* f)
            {
                // Checked here: the receive that ends the stream unrefs the frame.
                formatOk &= f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_NV12 ||
                            f->format == AV_PIX_FMT_YUVJ420P;
                const int w = f->width, h = f->height;
                y.assign(static_cast<size_t>(w) * h, 0);
                uv.assign(static_cast<size_t>(w) * (h / 2), 0);
                for (int r = 0; r < h; ++r)
                    std::memcpy(y.data() + static_cast<size_t>(r) * w, f->data[0] + static_cast<ptrdiff_t>(r) * f->linesize[0], w);
                for (int r = 0; r < h / 2; ++r)
                {
                    uint8_t* out = uv.data() + static_cast<size_t>(r) * w;
                    if (f->format == AV_PIX_FMT_NV12)
                        std::memcpy(out, f->data[1] + static_cast<ptrdiff_t>(r) * f->linesize[1], w);
                    else
                        for (int x = 0; x < w / 2; ++x)
                        {
                            out[2 * x]     = f->data[1][static_cast<ptrdiff_t>(r) * f->linesize[1] + x];
                            out[2 * x + 1] = f->data[2][static_cast<ptrdiff_t>(r) * f->linesize[2] + x];
                        }
                }
                ++decoded;
            };
            while (ok && av_read_frame(fmt, pk) >= 0)
            {
                if (pk->stream_index == idx && avcodec_send_packet(c, pk) >= 0)
                    while (avcodec_receive_frame(c, fr) == 0) keep(fr);
                av_packet_unref(pk);
            }
            if (ok && avcodec_send_packet(c, nullptr) >= 0)
                while (avcodec_receive_frame(c, fr) == 0) keep(fr);
            ok = ok && decoded > 0 && formatOk;
            av_packet_free(&pk);
            av_frame_free(&fr);
            avcodec_free_context(&c);
            avformat_close_input(&fmt);
            return ok;
        }
#endif
    }

    VideoUploadProbe& VideoUploadProbe::Get()
    {
        static VideoUploadProbe s_probe;
        return s_probe;
    }

    bool VideoUploadProbe::Requested()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS && defined(WITH_FFMPEG)
        static const bool s_on = Sys::CommandLine::Get().Has("-video_probe");
        return s_on;
#else
        return false;
#endif
    }

    bool VideoUploadProbe::Start(World& world, IGraphicsDevice& gfx)
    {
#ifdef WITH_FFMPEG
        m_loop = Sys::CommandLine::Get().Has("-video_probe_loop");
        m_hw   = !Sys::CommandLine::Get().Has("-video_swdecode");
        m_clip = Sys::DiagnosticsDir() + "/video_probe.mp4";
        if (const std::string size = Sys::CommandLine::Get().Get("-video_probe_size", ""); !size.empty())
        {
            char* end = nullptr;
            const unsigned long w = std::strtoul(size.c_str(), &end, 10);
            const unsigned long h = (*end == 'x') ? std::strtoul(end + 1, &end, 10) : 0;
            if (*end != '\0' || w < 16 || h < 16 || w > 4096 || h > 4096 || (w & 1u) || (h & 1u))
            {
                LOG_ERROR("[VIDEO_PROBE] -video_probe_size=%s is not an even WxH within 16..4096", size.c_str());
                return false;
            }
            m_width  = static_cast<uint32_t>(w);
            m_height = static_cast<uint32_t>(h);
        }
        if (!EncodeClip(m_clip, m_width, m_height, kFrames, kFps, m_encoder))
        {
            LOG_ERROR("[VIDEO_PROBE] could not encode the test clip '%s'", m_clip.c_str());
            return false;
        }
        uint32_t refFrames = 0;
        if (!DecodeLastFrame(m_clip, m_expectY, m_expectUV, refFrames) || refFrames != kFrames)
        {
            LOG_ERROR("[VIDEO_PROBE] the reference decode failed (%u of %u frames)", refFrames, kFrames);
            return false;
        }
        auto source = Video::OpenMp4(gfx, m_clip);
        if (!source)
        {
            LOG_ERROR("[VIDEO_PROBE] Video::OpenMp4 refused the test clip");
            return false;
        }
        const Entity e = world.CreateEntity();
        VideoComponent vc;
        vc.width   = m_width;
        vc.height  = m_height;
        vc.state   = VideoPlaybackState::Playing;
        vc.loop    = m_loop;
        vc.decodedFrameSource = std::move(source);
        // -video_probe_world=x,y,z: the clip on a 2.4 x 1.35 m world-space quad there (VideoQuadPass)
        // instead of over the screen (VideoPass).
        if (const std::string at = Sys::CommandLine::Get().Get("-video_probe_world", ""); !at.empty())
        {
            float xyz[3] = {};
            const char* p = at.c_str();
            char* end = nullptr;
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k)
            {
                xyz[k] = std::strtof(p, &end);
                ok = end != p && (*end == (k < 2 ? ',' : '\0'));
                p = end + 1;
            }
            const float x = xyz[0], y = xyz[1], z = xyz[2];
            if (!ok)
            {
                LOG_ERROR("[VIDEO_PROBE] -video_probe_world=%s is not x,y,z", at.c_str());
                return false;
            }
            LocalTransform lt;
            lt.translation = { x, y, z };
            world.AddComponent<LocalTransform>(e, lt);
            world.AddComponent<GlobalTransform>(e, {});
            vc.worldSpace  = true;
            vc.worldWidth  = 2.4f;
            vc.worldHeight = 1.35f;
        }
        world.AddComponent<VideoComponent>(e, std::move(vc));
        m_entity = e;
        LOG_INFO("[VIDEO_PROBE] armed: %ux%u, %u frames at %u fps, encoder %s, %s decode%s - '%s'",
                 m_width, m_height, kFrames, kFps, m_encoder.c_str(), m_hw ? "D3D12VA" : "software",
                 m_loop ? ", looping (no check)" : "", m_clip.c_str());
        return true;
#else
        (void)world; (void)gfx;
        return false;
#endif
    }

    void VideoUploadProbe::GameTick(World& world, IGraphicsDevice& gfx, uint64_t frame)
    {
        auto fail = [this](const char* why)
        {
            LOG_ERROR("[VIDEO_PROBE] %s", why);
            m_phase = Phase::Failed;
        };
        switch (m_phase)
        {
        case Phase::Idle:
            if (frame >= kStartFrame) m_phase = Start(world, gfx) ? Phase::Playing : Phase::Failed;
            break;
        case Phase::Playing:
        {
            const VideoComponent* vc = world.GetComponent<VideoComponent>(m_entity);
            if (!vc) { fail("the probe's VideoComponent is gone"); break; }
            if (vc->state == VideoPlaybackState::Stopped) { fail("playback stopped (the frame source failed)"); break; }
            if (!m_firstSeen && vc->framesDecoded > 0)
            {
                m_firstSeen     = true;
                m_flushAtFirst  = UploadStats::flushes.load(std::memory_order_relaxed);
                // The first frame's request is already counted (VideoSystem
                // queued it earlier this tick), so the totals from here cover
                // the frames after it.
                m_copiesAtFirst = UploadStats::streamCopies.load(std::memory_order_relaxed);
                m_bytesAtFirst  = UploadStats::streamBytes.load(std::memory_order_relaxed);
            }
            m_decoded = static_cast<uint32_t>(vc->framesDecoded);
            if (!m_loop && vc->framesDecoded >= kFrames)
            {
                m_playFlushes = UploadStats::flushes.load(std::memory_order_relaxed) - m_flushAtFirst;
                m_settleUntil = frame + kSettle;
                m_phase       = Phase::Settling;
            }
            break;
        }
        case Phase::Settling:
        {
            if (frame < m_settleUntil) break;
            // Every frame's copy is recorded by now (the render half records a
            // frame's list right after its game half queues it).
            m_playCopies = UploadStats::streamCopies.load(std::memory_order_relaxed) - m_copiesAtFirst;
            m_playBytes  = UploadStats::streamBytes.load(std::memory_order_relaxed) - m_bytesAtFirst;
            const VideoComponent* vc = world.GetComponent<VideoComponent>(m_entity);
            if (!vc || vc->dpb.empty() || !vc->dpb[0].IsValid()) { fail("the video texture is missing"); break; }
            m_readSrc = vc->dpb[0];
            uint32_t rows = 0;
            if (!gfx.GetTexture2DReadbackFootprint(m_readSrc, m_rowPitch, rows, m_readBytes))
            {
                fail("no readback footprint for the video texture");
                break;
            }
            m_rows = rows;
            RHI::GPUBufferDesc bd{};
            bd.size       = m_readBytes;
            bd.usage      = RHI::Usage::READBACK;
            bd.bindFlags = RHI::BindFlag::NONE;
            if (!gfx.CreateBuffer(bd, m_read)) { fail("no readback buffer"); break; }
            m_readFrame.store(frame, std::memory_order_release);
            // AcquireUploadSlot(frame + kSlots) waits for this frame's slot -
            // and the one before it, which may record the read on overlap.
            m_checkAt = frame + kSlots;
            m_phase   = Phase::Reading;
            break;
        }
        case Phase::Reading:
            if (frame >= m_checkAt)
            {
                Check(gfx);
                m_phase = Phase::Done;
                Report("checked");
            }
            break;
        default:
            break;
        }
    }

    void VideoUploadProbe::Check(IGraphicsDevice& gfx)
    {
        if (!m_readRecorded.load(std::memory_order_acquire))
        {
            LOG_ERROR("[VIDEO_PROBE] the reader never ran");
            return;
        }
        const uint8_t* p = static_cast<const uint8_t*>(gfx.MapBuffer(m_read));
        if (!p) { LOG_ERROR("[VIDEO_PROBE] the readback did not map"); return; }
        ++m_checked;
        // Plane 0 (Y) at 0, plane 1 (UV, w bytes a row - the same row size, so the same pitch)
        // at the next 512-byte placement: GetCopyableFootprints' layout for the two planes. This
        // NV12 texture's pitch is 512 for a 256-texel row, not the 256 the row size alone gives.
        bool y = true, uv = true;
        for (uint32_t r = 0; r < m_height && y; ++r)
            y = std::memcmp(p + static_cast<size_t>(r) * m_rowPitch, m_expectY.data() + static_cast<size_t>(r) * m_width,
                            m_width) == 0;
        const uint64_t uvOffset = AlignUp(static_cast<uint64_t>(m_rowPitch) * m_rows, 512);
        const uint64_t uvPitch  = m_rowPitch;
        for (uint32_t r = 0; r < m_height / 2 && uv; ++r)
        {
            const uint8_t* got  = p + uvOffset + r * uvPitch;
            const uint8_t* want = m_expectUV.data() + static_cast<size_t>(r) * m_width;
            uv = uvOffset + r * uvPitch + m_width <= m_readBytes && std::memcmp(got, want, m_width) == 0;
            if (!uv && uvOffset + r * uvPitch + m_width <= m_readBytes)
            {
                uint32_t c = 0;
                while (c < m_width && got[c] == want[c]) ++c;
                LOG_WARNING("[VIDEO_PROBE] UV row %u differs from byte %u: read %u %u %u %u, expected %u %u %u %u "
                            "(offset %llu, pitch %llu, %llu bytes read back)", r, c, got[c], got[c + 1], got[c + 2],
                            got[c + 3], want[c], want[c + 1], want[c + 2], want[c + 3],
                            static_cast<unsigned long long>(uvOffset), static_cast<unsigned long long>(uvPitch),
                            static_cast<unsigned long long>(m_readBytes));
            }
        }
        gfx.UnmapBuffer(m_read);
        m_yMatch  = y ? 1 : 0;
        m_uvMatch = uv ? 1 : 0;
        if (!y || !uv)
            LOG_WARNING("[VIDEO_PROBE] the video texture is not the clip's last frame (Y %s, UV %s)",
                        y ? "matches" : "differs", uv ? "matches" : "differs");
    }

    void VideoUploadProbe::RecordReadback(IGraphicsDevice& gfx, RHI::CommandList cl)
    {
        if (m_readFrame.load(std::memory_order_acquire) == UINT64_MAX ||
            m_readRecorded.load(std::memory_order_relaxed))
            return;
        gfx.CopyTexture2DToBufferInPlace(m_readSrc, m_read, cl);
        m_readRecorded.store(true, std::memory_order_release);
    }

    void VideoUploadProbe::Report(const char* when) const
    {
        LOG_INFO("[VIDEO_PROBE] mode=%s encoder=%s frames=%u decoded=%u yMatch=%d uvMatch=%d playFlushes=%llu "
                 "playCopies=%llu playBytes=%llu checked=%u loop=%d failed=%d at=%s",
                 m_hw ? "hw" : "sw", m_encoder.empty() ? "none" : m_encoder.c_str(), kFrames, m_decoded,
                 m_yMatch, m_uvMatch, static_cast<unsigned long long>(m_playFlushes),
                 static_cast<unsigned long long>(m_playCopies), static_cast<unsigned long long>(m_playBytes),
                 m_checked, m_loop ? 1 : 0, m_phase == Phase::Failed ? 1 : 0, when);
    }

    VideoUploadProbeSystem::~VideoUploadProbeSystem()
    {
        if (VideoUploadProbe::Requested()) VideoUploadProbe::Get().Report("shutdown");
    }

    void VideoUploadProbeSystem::Update(World& world, const FrameContext& ctx)
    {
        if (!VideoUploadProbe::Requested()) return;
        VideoUploadProbe::Get().GameTick(world, m_gfx, ctx.frameIndex);
    }
}

#endif  // WITH_HARNESS
