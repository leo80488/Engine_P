// A video file as a face tracking image source: the opposite trade-off of a camera - every frame,
// in order, reproducibly, over being live.
//   - software decode to NV12 in memory (the Media module's IVideoReader: inference needs the
//     pixels on the CPU, and its conversion is exact on every machine)
//   - pull: Next() decodes the next frame and hands it over only once its PTS is due on the
//     caller's clock (the source's simulation clock): no frame is ever skipped, and under a
//     fixed time step two runs see the same frames at the same times
//   - back-pressure is the caller's: it pulls the next frame when it is done with the last

#include "FaceTracking/ImageCapture.h"
#include "Media/VideoReader.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace FaceTracking {
namespace {

// A decoded frame written straight into a FaceImage's planes.
class FaceImageSink final : public Media::INv12Sink
{
public:
    explicit FaceImageSink(FaceImage& img) : m_img(img) {}

    void CopyFrom(uint32_t width, uint32_t height, const uint8_t* y, size_t yPitch, const uint8_t* uv,
                  size_t uvPitch) override
    {
        m_img.CopyFrom(width, height, y, yPitch, uv, uvPitch);
    }

    void Resize(uint32_t width, uint32_t height, uint8_t*& y, uint8_t*& uv) override
    {
        m_img.Resize(width, height);
        y = m_img.y.data();
        uv = m_img.uv.data();
    }

private:
    FaceImage& m_img;
};

class VideoFileCapture final : public IImageCapture
{
public:
    explicit VideoFileCapture(std::string path) : m_path(std::move(path)) {}
    ~VideoFileCapture() override { Stop(); }

    bool Start(std::string& error) override
    {
        Stop();
        const std::string onDisk = Resource::AssetFS::Get().ResolveOnDisk(m_path);
        const std::string& open = onDisk.empty() ? m_path : onDisk;
        m_reader = Media::OpenVideoReader(open, m_path, error);
        if (!m_reader)
        {
            Set(CaptureState::Failed, error);
            return false;
        }
        Set(CaptureState::Running, {});
        return true;
    }

    void Stop() override
    {
        m_reader.reset();
        m_have = false;
        if (m_state != CaptureState::Failed) Set(CaptureState::Stopped, {});
    }

    bool Pull() const override { return true; }

    bool Next(int64_t untilUs, FaceImage& out) override
    {
        if (m_state != CaptureState::Running) return false;
        if (!m_have)
        {
            if (!DecodeOne(m_pending))
            {
                if (m_reader->AtEnd()) Set(CaptureState::Ended, {});
                return false;
            }
            m_have = true;
        }
        if (m_pending.ptsUs > untilUs) return false;
        out = m_pending;
        m_have = false;
        return true;
    }

    CaptureState State() const override { return m_state.load(); }
    std::string LastError() const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_error;
    }
    std::string Describe() const override
    {
        char buf[320];
        std::snprintf(buf, sizeof(buf), "%s %ux%u, %llu frames decoded", m_path.c_str(), m_reader ? m_reader->Width() : 0u,
                      m_reader ? m_reader->Height() : 0u, static_cast<unsigned long long>(m_decoded));
        return buf;
    }

private:
    void Set(CaptureState s, std::string error)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = std::move(error);
        }
        if (m_state != s) LOG_INFO("[FACETRACK] video %s: %s", m_path.c_str(), CaptureStateName(s));
        m_state = s;
    }

    // The next frame in decode order, as NV12; false at the end of the stream.
    bool DecodeOne(FaceImage& img)
    {
        FaceImageSink sink(img);
        int64_t ptsUs = 0;
        if (!m_reader->ReadFrame(sink, ptsUs)) return false;
        img.ptsUs = ptsUs;
        img.captureUs = 0;
        img.seq = ++m_decoded;
        return true;
    }

    const std::string m_path;
    std::unique_ptr<Media::IVideoReader> m_reader;
    std::atomic<uint64_t> m_decoded{0};
    bool m_have = false;
    FaceImage m_pending;
    std::atomic<CaptureState> m_state{CaptureState::Stopped}; // written by the worker, read by Status
    mutable std::mutex m_mutex;
    std::string m_error;
};

} // namespace

std::unique_ptr<IImageCapture> MakeVideoFileCapture(const std::string& path) { return std::make_unique<VideoFileCapture>(path); }

} // namespace FaceTracking
