// The harness's fake camera: a synthetic 640x480 NV12 stream from a thread of its own - the same
// push shape as a Media Foundation camera, so CameraFaceSource cannot tell them apart - plus the
// three failures a CI machine without a camera cannot produce:
//   denied  the privacy setting: Denied from the start, nothing delivered
//   busy    in use elsewhere: Busy, retried every 3 s, never succeeding
//   lost    unplugged: Running for 30 frames, then Lost - until a device change "plugs it back"
// With a video, its frames (decoded once at Start) replace the pattern, in a loop: a live source
// with a face for the tracker. The rate is 30 Hz unless asked otherwise (1-1000); a rate faster
// than the tracker keeps up with models a camera faster than its tracker.
// Harness builds only (Retail has no fake devices); MakeFakeCamera answers null otherwise.

#include "FaceTracking/ImageCapture.h"

#if WITH_HARNESS

#include "FaceTracking/FaceSource.h" // SteadyNowUs
#include "FaceTracking/FrameMailbox.h"
#include "System/Log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace FaceTracking {
namespace {

class FakeCamera final : public IImageCapture
{
public:
    FakeCamera(std::string fault, std::string video, int fps) : m_fault(std::move(fault)), m_video(std::move(video)), m_fps(fps) {}
    ~FakeCamera() override { Stop(); }

    bool Start(std::string& error) override
    {
        Stop();
        if (!m_fault.empty() && m_fault != "denied" && m_fault != "busy" && m_fault != "lost")
        {
            error = "fault is denied, busy or lost, not '" + m_fault + "'";
            return false;
        }
        if (m_fps < 1 || m_fps > 1000)
        {
            error = "fps is 1 to 1000, not " + std::to_string(m_fps);
            return false;
        }
        if (!m_video.empty() && m_frames.empty() && !LoadVideo(error)) return false;
        m_stop = false;
        m_thread = std::thread([this] { Run(); });
        return true;
    }
    void Stop() override
    {
        if (!m_thread.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        m_thread.join();
        m_state = CaptureState::Stopped;
    }
    bool Pull() const override { return false; }
    bool Next(int64_t, FaceImage& out) override
    {
        const FaceImage* img = m_mailbox.TakeNewest();
        if (!img) return false;
        out = *img;
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
        char buf[192];
        std::snprintf(buf, sizeof(buf), "fake camera%s%s %s NV12@%d, %llu frames (%llu dropped), attempts=%llu",
                      m_fault.empty() ? "" : " fault=", m_fault.c_str(), m_video.empty() ? "640x480" : "video", m_fps,
                      static_cast<unsigned long long>(m_mailbox.Published()), static_cast<unsigned long long>(m_mailbox.Dropped()),
                      static_cast<unsigned long long>(m_attempts.load()));
        return m_video.empty() ? std::string(buf) : std::string(buf) + " of " + m_video + " (" + std::to_string(m_frames.size()) + " frames, looping)";
    }
    void OnDeviceChange() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_replugged = true;
        }
        m_cv.notify_all();
    }

private:
    void Set(CaptureState s, const char* error)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = error;
        }
        if (m_state.exchange(s) != s) LOG_INFO("[FACETRACK] fake camera: %s%s%s", CaptureStateName(s), *error ? " - " : "", error);
    }

    // Every frame of the video (at most 3000), in order, decoded once on the opening thread: meant
    // for a short harness clip.
    bool LoadVideo(std::string& error)
    {
        std::unique_ptr<IImageCapture> cap = MakeVideoFileCapture(m_video);
        if (!cap || !cap->Start(error))
        {
            if (error.empty()) error = "this build cannot decode video";
            return false;
        }
        FaceImage img;
        while (cap->Next(INT64_MAX, img) && m_frames.size() < 3000) m_frames.push_back(img);
        cap->Stop();
        if (m_frames.empty())
        {
            error = "no frame decoded from '" + m_video + "'";
            return false;
        }
        return true;
    }

    // A frame that differs every time and can be told apart in a preview: a gradient that
    // moves with the frame number, and a white square that walks across - or the video's next frame.
    void Paint(FaceImage& img)
    {
        if (!m_frames.empty())
        {
            img = m_frames[static_cast<size_t>((m_seq - 1) % m_frames.size())];
            return;
        }
        img.Resize(640, 480);
        const uint32_t k = static_cast<uint32_t>(m_seq);
        for (uint32_t y = 0; y < 480; ++y)
            for (uint32_t x = 0; x < 640; ++x)
            {
                const bool square = ((x + k * 4) % 640) < 64 && y >= 208 && y < 272;
                img.y[y * 640 + x] = square ? 235 : static_cast<uint8_t>(16 + ((x + y + k) & 0x7F));
            }
        for (uint32_t i = 0; i < img.uv.size(); i += 2)
        {
            img.uv[i] = static_cast<uint8_t>(128 + ((k * 3) & 0x1F));
            img.uv[i + 1] = 128;
        }
    }

    void Run()
    {
        if (m_fault == "denied")
        {
            Set(CaptureState::Denied, "camera access is off for desktop apps (Settings > Privacy & security > Camera) - fake");
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return m_stop; });
            return;
        }
        ++m_attempts;
        if (m_fault == "busy")
        {
            for (;;)
            {
                Set(CaptureState::Busy, "the camera is in use by another program - retrying every 3 s - fake");
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_cv.wait_for(lock, std::chrono::seconds(3), [this] { return m_stop; })) return;
                lock.unlock();
                Set(CaptureState::Opening, "");
                ++m_attempts; // the retry
            }
        }
        Set(CaptureState::Running, "");
        const auto period = std::chrono::microseconds(1000000 / m_fps);
        auto next = std::chrono::steady_clock::now();
        uint64_t sinceOpen = 0;
        for (;;)
        {
            next += period;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_cv.wait_until(lock, next, [this] { return m_stop; })) return;
            }
            if (m_fault == "lost" && !m_replaced && sinceOpen >= 30 && m_state.load() == CaptureState::Running)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_replugged = false; // only a device change from now on plugs it back
                }
                Set(CaptureState::Lost, "the camera is not connected - it reopens when it is plugged in again - fake");
            }
            if (m_state.load() == CaptureState::Lost)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this] { return m_stop || m_replugged; });
                if (m_stop) return;
                m_replugged = false;
                lock.unlock();
                Set(CaptureState::Running, "");
                sinceOpen = 0;
                m_replaced = true; // plugged back: it stays (only this thread reads it)
                next = std::chrono::steady_clock::now();
                continue;
            }
            FaceImage& img = m_mailbox.Back();
            ++m_seq;
            Paint(img);
            img.ptsUs = static_cast<int64_t>(m_seq) * (1000000 / m_fps);
            img.captureUs = SteadyNowUs();
            img.seq = m_seq;
            m_mailbox.Publish();
            ++sinceOpen;
        }
    }

    const std::string m_fault; // read by Describe on the main thread: never written after construction
    const std::string m_video; // likewise
    const int m_fps;
    std::vector<FaceImage> m_frames; // the video's, filled by Start before the thread runs; read-only after
    bool m_replaced = false;
    std::atomic<CaptureState> m_state{CaptureState::Stopped};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false, m_replugged = false;
    std::string m_error;
    uint64_t m_seq = 0;
    std::atomic<uint64_t> m_attempts{0}; // opens tried (a busy camera retries every 3 s)
    FrameMailbox m_mailbox;
    std::thread m_thread;
};

} // namespace

std::unique_ptr<IImageCapture> MakeFakeCamera(const std::string& fault, const std::string& video, int fps)
{
    return std::make_unique<FakeCamera>(fault, video, fps);
}

} // namespace FaceTracking

#else

namespace FaceTracking {
std::unique_ptr<IImageCapture> MakeFakeCamera(const std::string&, const std::string&, int) { return nullptr; }
} // namespace FaceTracking

#endif
