// The decode loop is Mp4FrameSource's (av_read_frame / send / receive, drain at EOF), kept
// separate rather than shared: that one serves a texture, this one a CPU consumer.

#include "Media/VideoReader.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "Resource/AssetStreamAvio.h"   // a video sealed in game.ipak

#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "swscale.lib")

namespace Media
{
namespace
{
class FfmpegVideoReader final : public IVideoReader
{
public:
    ~FfmpegVideoReader() override
    {
        if (m_sws) sws_freeContext(m_sws);
        if (m_frame) av_frame_free(&m_frame);
        if (m_packet) av_packet_free(&m_packet);
        if (m_codec) avcodec_free_context(&m_codec);
        if (m_fmt) avformat_close_input(&m_fmt);
        m_assetIo.Close();   // after m_fmt: avformat_close_input does not free a custom context
    }

    bool Open(const std::string& path, const std::string& name, std::string& error)
    {
        // A video in the pak is read through AssetFS; a loose one FFmpeg opens by path.
        if (Resource::AssetFS::Get().HasInPak(path))
        {
            m_fmt = avformat_alloc_context();
            if (!m_fmt || !m_assetIo.Open(path)) return Fail(error, "cannot read '" + name + "' from the pak");
            m_fmt->pb = m_assetIo.Context();
            m_fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        }
        if (avformat_open_input(&m_fmt, path.c_str(), nullptr, nullptr) < 0) return Fail(error, "cannot open '" + name + "'");
        if (avformat_find_stream_info(m_fmt, nullptr) < 0) return Fail(error, "no stream info in '" + name + "'");
        const AVCodec* dec = nullptr;
        m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (m_stream < 0 || !dec) return Fail(error, "no video stream in '" + name + "'");
        m_tb = m_fmt->streams[m_stream]->time_base;
        m_codec = avcodec_alloc_context3(dec);
        if (!m_codec || avcodec_parameters_to_context(m_codec, m_fmt->streams[m_stream]->codecpar) < 0)
            return Fail(error, "codec context failed");
        m_codec->thread_count = 2; // frame order is the decoder's, whatever its threads
        if (avcodec_open2(m_codec, dec, nullptr) < 0) return Fail(error, "avcodec_open2 failed");
        m_frame = av_frame_alloc();
        m_packet = av_packet_alloc();
        if (!m_frame || !m_packet) return Fail(error, "out of memory");
        return true;
    }

    bool ReadFrame(INv12Sink& sink, int64_t& ptsUs) override
    {
        for (;;)
        {
            const int got = avcodec_receive_frame(m_codec, m_frame);
            if (got == 0)
            {
                Convert(sink, ptsUs);
                return true;
            }
            if (got == AVERROR_EOF)
            {
                m_eos = true;
                return false;
            }
            if (got != AVERROR(EAGAIN)) return false;
            if (m_eos) return false;
            const int ret = av_read_frame(m_fmt, m_packet);
            if (ret == AVERROR_EOF)
            {
                avcodec_send_packet(m_codec, nullptr); // drain what the decoder holds
                continue;
            }
            if (ret < 0) return false;
            if (m_packet->stream_index == m_stream) avcodec_send_packet(m_codec, m_packet);
            av_packet_unref(m_packet);
        }
    }

    bool AtEnd() const override { return m_eos; }
    uint32_t Width() const override { return m_codec ? static_cast<uint32_t>(m_codec->width) : 0; }
    uint32_t Height() const override { return m_codec ? static_cast<uint32_t>(m_codec->height) : 0; }

private:
    static bool Fail(std::string& error, const std::string& why)
    {
        error = why;
        return false;
    }

    void Convert(INv12Sink& sink, int64_t& ptsUs)
    {
        const int w = m_frame->width & ~1, h = m_frame->height & ~1;
        int64_t pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE ? m_frame->best_effort_timestamp : m_frame->pts;
        if (pts == AV_NOPTS_VALUE) pts = m_lastPts + 1;
        if (m_firstPts == INT64_MIN) m_firstPts = pts;
        m_lastPts = pts;
        ptsUs = av_rescale_q(pts - m_firstPts, m_tb, AVRational{1, 1000000});
        if (m_frame->format == AV_PIX_FMT_NV12)
        {
            sink.CopyFrom(static_cast<uint32_t>(w), static_cast<uint32_t>(h), m_frame->data[0],
                          static_cast<size_t>(m_frame->linesize[0]), m_frame->data[1],
                          static_cast<size_t>(m_frame->linesize[1]));
            return;
        }
        if (!m_sws || m_swsFormat != m_frame->format || m_swsW != w || m_swsH != h)
        {
            if (m_sws) sws_freeContext(m_sws);
            // SWS_POINT: exact and the same on every machine (no filter taps to vary).
            m_sws = sws_getContext(m_frame->width, m_frame->height, static_cast<AVPixelFormat>(m_frame->format), w, h, AV_PIX_FMT_NV12,
                                   SWS_POINT, nullptr, nullptr, nullptr);
            m_swsFormat = m_frame->format;
            m_swsW = w;
            m_swsH = h;
        }
        uint8_t* y = nullptr;
        uint8_t* uv = nullptr;
        sink.Resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h), y, uv);
        uint8_t* dst[4] = {y, uv, nullptr, nullptr};
        int dstLines[4] = {w, w, 0, 0};
        if (m_sws) sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, m_frame->height, dst, dstLines);
    }

    AVFormatContext* m_fmt = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_sws = nullptr;
    int m_swsFormat = -1, m_swsW = 0, m_swsH = 0;
    int m_stream = -1;
    AVRational m_tb{1, 1};
    int64_t m_firstPts = INT64_MIN, m_lastPts = 0;
    bool m_eos = false;
    Resource::AssetStreamAvio m_assetIo;   // m_fmt's reads when the video is in the pak
};
}

std::unique_ptr<IVideoReader> OpenVideoReader(const std::string& path, const std::string& name, std::string& error)
{
    auto reader = std::make_unique<FfmpegVideoReader>();
    if (!reader->Open(path, name, error)) return nullptr;
    return reader;
}
}
