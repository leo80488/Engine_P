// Image sources as face sources (see CameraSource.h).
//   live (a camera): a worker thread ("FaceTrack.Infer") waits for the capture's newest image, tracks
//                    it, pushes the FaceFrame into the ring and publishes the image to the preview
//                    mailbox - the main thread never waits on it
//   pull (a video):  Produce - on the Pump thread - takes every frame now due on the source's
//                    simulation clock, in order, and tracks it there and then, so a fixed time step
//                    reproduces the frames (back-pressure: the next frame is decoded once this one is done)
// The tracker is made at the first image, on the thread that tracks; one that cannot be made (ONNX
// Runtime absent, a model missing) is not tried again, and the images still reach the preview.
// The worker is not in Diag::Prof's thread registry: its 64 slots are taken for the life of the process,
// and a camera's worker comes and goes with every open. It runs at normal priority: below normal starved
// it whenever every core was busy (none of a camera's first 30 images tracked on a loaded machine), and
// a busy CPU already costs it only images (it takes the newest).

#include "FaceTracking/CameraSource.h"
#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceMath.h"
#include "FaceTracking/FrameMailbox.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

namespace FaceTracking {
namespace {

class CameraFaceSource final : public IFaceSource
{
public:
    CameraFaceSource(std::unique_ptr<IImageCapture> capture, const char* kind, const CameraSourceOptions& options)
        : m_cap(std::move(capture)), m_kind(kind), m_opt(options)
    {
        for (int k = 0; k < Tracker::kBlendshapes; ++k) m_channelOf[k] = FindArkitChannel(Tracker::kBlendshapeNames[static_cast<size_t>(k)]);
    }
    ~CameraFaceSource() override
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
        m_cap->Stop();
    }

    const char* Kind() const override { return m_kind; }
    const FaceSourceChannels& Channels() const override { return m_channels; } // the tracker's ARKit channels
    bool WallClock() const override { return !m_cap->Pull(); }
    int64_t DelayUs() const override { return 50000; }
    const char* StateName() const override { return CaptureStateName(m_cap->State()); }
    std::string LastError() const override
    {
        std::string e = m_cap->LastError();
        if (e.empty())
        {
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            e = m_trackerError;
        }
        return e;
    }
    void OnDeviceChange() override { m_cap->OnDeviceChange(); }

    std::string Describe() const override
    {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "; %llu images, %.1f/s (%llu polls); %llu faces", static_cast<unsigned long long>(m_images.load()),
                      static_cast<double>(m_rate.load()) / 10.0, static_cast<unsigned long long>(m_polls.load()),
                      static_cast<unsigned long long>(m_faces.load()));
        std::string tracker;
        {
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            tracker = m_trackerText;
        }
        return m_cap->Describe() + buf + (tracker.empty() ? std::string() : "; " + tracker);
    }

    bool Start(FaceFrameRing& ring, int64_t epochUs, std::string& error) override
    {
        m_ring = &ring;
        m_epochUs = epochUs;
        if (!m_cap->Start(error)) return false;
        if (!m_cap->Pull()) m_worker = std::thread([this] {
#ifdef TRACY_ENABLE
            tracy::SetThreadName("FaceTrack.Infer");
#endif
            ::SetThreadDescription(::GetCurrentThread(), L"FaceTrack.Infer");
            Work();
        });
        return true;
    }

    void Produce(int64_t clockUs, FaceFrameRing&) override
    {
        if (!m_cap->Pull()) return;
        while (m_cap->Next(clockUs, m_image)) Process(m_image, clockUs);
    }

    bool TakePreview(FaceImage& out) override
    {
        const FaceImage* img = m_preview.TakeNewest();
        if (!img) return false;
        out = *img;
        return true;
    }

    bool OverlayFor(uint64_t imageSeq, FaceOverlay& out) const override
    {
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        for (const FaceOverlay& o : m_overlays)
            if (o.width > 0 && o.imageSeq == imageSeq)
            {
                out = o;
                return true;
            }
        return false;
    }

private:
    static_assert(kOverlayLandmarks == Tracker::kLandmarks && kOverlayBlendshapes == Tracker::kBlendshapes,
                  "FaceOverlay.h spells the tracker's sizes");

    // What the tracker made of this image, for the preview's overlay (FaceOverlay.h): kept by the image's
    // seq, the last few, because the preview takes an image a moment after it was tracked. Before the image
    // is published to the preview, so its overlay is always there when the preview gets it.
    void PutOverlay(const FaceImage& img)
    {
        FaceOverlay o;
        o.imageSeq = img.seq;
        o.width = img.width;
        o.height = img.height;
        o.found = m_result.found;
        o.detected = m_result.detected;
        o.presence = m_result.presence;
        for (int k = 0; k < 4; ++k) o.ms[k] = m_result.ms[k];
        if (m_result.found)
        {
            const float s = static_cast<float>(Tracker::kLandmarkSize);
            const float corners[4][2] = {{0.0f, 0.0f}, {s, 0.0f}, {s, s}, {0.0f, s}};
            for (int c = 0; c < 4; ++c)
                Tracker::TensorToImage(m_result.roi, Tracker::kLandmarkSize, corners[c][0], corners[c][1], o.roi[c][0], o.roi[c][1]);
            for (int i = 0; i < Tracker::kLandmarks; ++i)
            {
                o.points[static_cast<size_t>(i) * 2] = m_result.landmarks[i * 3];
                o.points[static_cast<size_t>(i) * 2 + 1] = m_result.landmarks[i * 3 + 1];
            }
            const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&m_result.head));
            const float toDeg = 180.0f / FaceMath::kPi;
            o.yawDeg = ypr.yaw * toDeg;
            o.pitchDeg = ypr.pitch * toDeg;
            o.rollDeg = ypr.roll * toDeg;
            for (int k = 0; k < Tracker::kBlendshapes; ++k) o.blendshapes[static_cast<size_t>(k)] = m_result.blendshapes[k];
        }
        std::lock_guard<std::mutex> lock(m_overlayMutex);
        m_overlays[m_overlayNext] = o;
        m_overlayNext = (m_overlayNext + 1) % m_overlays.size();
    }

    // clockUs: the source clock of the Pump that took a pull source's frame (-1 for a camera's,
    // which its worker takes whenever it comes) - the reproducibility test compares it too.
    void Process(const FaceImage& img, int64_t clockUs)
    {
        const uint64_t n = m_images.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m_opt.frameLog)
            LOG_INFO("[FACETRACK] image source=%s n=%llu seq=%llu pts=%lld clock=%lld size=%ux%u hash=%016llx", m_kind,
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(img.seq), static_cast<long long>(img.ptsUs),
                     static_cast<long long>(clockUs), img.width, img.height, static_cast<unsigned long long>(img.Hash()));
#if WITH_HARNESS
        // A slower tracker on demand (OpenFakeCamera's trackDelayMs; face_track_fault's fast camera).
        if (m_opt.trackDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(m_opt.trackDelayMs));
#endif
        Track(img, n - 1);
        m_preview.Back() = img;
        m_preview.Publish();
    }

    // The image through the tracker into the ring (a video's frame at its PTS; a camera's at its arrival
    // since the source opened, the clock live sources share).
    void Track(const FaceImage& img, uint64_t index)
    {
        const bool pull = m_cap->Pull();
        if (!m_tracker && !m_trackerFailed)
        {
            // A camera does not wait for the models to load (its images keep coming; this one is skipped);
            // a video does, so every frame is tracked.
            std::string error;
            bool pending = false;
            m_tracker = MakeFaceTracker(m_opt.tracker, /*wait=*/pull, pending, error);
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            if (!m_tracker && pending)
            {
                m_trackerText = "tracker: loading the models";
                return;
            }
            if (!m_tracker)
            {
                m_trackerFailed = true;
                m_trackerError = "no face tracker: " + error;
                m_trackerText = m_trackerError;
                LOG_WARNING("[FACETRACK] %s source: %s - its images reach the preview, no face does", m_kind, m_trackerError.c_str());
            }
        }
        if (!m_tracker) return;
        const int64_t tUs = pull ? img.ptsUs : img.captureUs - m_epochUs;
        if (!m_tracker->Track(img, pull ? img.ptsUs : img.captureUs, m_result))
        {
            // Logged when it first fails, when the reason changes, and every 300th failure after that.
            const std::string error = m_tracker->LastError();
            ++m_trackFailures;
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            if (error != m_trackerError || m_trackFailures % 300 == 1)
                LOG_WARNING("[FACETRACK] %s source: the tracker failed on an image (%llu so far): %s", m_kind,
                            static_cast<unsigned long long>(m_trackFailures), error.c_str());
            m_trackerError = error;
            m_trackerErrorSet = true;
            return;
        }
        if (m_trackerErrorSet)
        {
            // It works again: the last failure stops being the source's error.
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            m_trackerError.clear();
            m_trackerErrorSet = false;
        }
        PutOverlay(img);
        FaceFrame f;
        f.seq = m_nextSeq++;
        f.sourceTimeUs = tUs;
        f.captureQpcUs = img.captureUs;
        f.faceFound = m_result.found ? 1 : 0;
        f.confidence = m_result.found ? m_result.presence : 0.0f;
        if (m_result.found)
        {
            for (int k = 0; k < Tracker::kBlendshapes; ++k)
                if (m_channelOf[k] >= 0) f.Set(m_channelOf[k], m_result.blendshapes[k]);
            f.headRot = m_result.head;
            f.flags |= kFaceHasHead;
            m_faces.fetch_add(1, std::memory_order_relaxed);
        }
        if (!m_ring->TryPush(f)) m_ringFull.fetch_add(1, std::memory_order_relaxed);
        if ((index & 31) == 0)
        {
            std::lock_guard<std::mutex> lock(m_trackerMutex);
            m_trackerText = "tracker: " + m_tracker->Describe();
            if (m_ringFull.load() > 0) m_trackerText += ", " + std::to_string(m_ringFull.load()) + " frames dropped (ring full)";
        }
#if WITH_HARNESS
        if (!m_opt.trackerDump.empty()) Dump(index);
#endif
    }

#if WITH_HARNESS
    void Dump(uint64_t index)
    {
        if (!m_dump.is_open())
        {
            m_dump = Sys::OpenOfstream(m_opt.trackerDump, std::ios::out | std::ios::trunc);
            if (!m_dump.is_open())
            {
                LOG_WARNING("[FACETRACK] -facetrack_tracker_dump: cannot write '%s'", m_opt.trackerDump.c_str());
                m_opt.trackerDump.clear();
                return;
            }
        }
        const FaceTrackerResult& r = m_result;
        std::string line = "{\"frame\":" + std::to_string(index) + ",\"found\":" + (r.found ? "true" : "false");
        char num[48];
        std::snprintf(num, sizeof(num), ",\"presence\":%.4f,\"detected\":%s", r.presence, r.detected ? "true" : "false");
        line += num;
        if (r.found)
        {
            line += ",\"landmarks\":[";
            for (int i = 0; i < Tracker::kLandmarks; ++i)
            {
                std::snprintf(num, sizeof(num), "%s[%.2f,%.2f]", i ? "," : "", r.landmarks[i * 3], r.landmarks[i * 3 + 1]);
                line += num;
            }
            line += "],\"blendshapes\":[";
            for (int k = 0; k < Tracker::kBlendshapes; ++k)
            {
                std::snprintf(num, sizeof(num), "%s%.4f", k ? "," : "", r.blendshapes[k]);
                line += num;
            }
            const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&r.head));
            const float toDeg = 180.0f / FaceMath::kPi;
            std::snprintf(num, sizeof(num), "],\"ypr\":[%.3f,%.3f,%.3f]", ypr.yaw * toDeg, ypr.pitch * toDeg, ypr.roll * toDeg);
            line += num;
        }
        std::snprintf(num, sizeof(num), ",\"ms\":[%.2f,%.2f,%.2f,%.2f]}\n", r.ms[0], r.ms[1], r.ms[2], r.ms[3]);
        line += num;
        m_dump << line;
        m_dump.flush();
    }
#endif

    void Work()
    {
        auto windowStart = std::chrono::steady_clock::now();
        uint64_t windowImages = m_images.load();
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                // Cameras deliver at most 60 Hz; polling at 2 ms keeps the added latency small
                // without a wake-up channel into every capture backend.
                if (m_cv.wait_for(lock, std::chrono::milliseconds(2), [this] { return m_stop.load(); })) return;
            }
            m_polls.fetch_add(1, std::memory_order_relaxed);
            // A camera faster than its tracker always has a newer image by the time one is done: the
            // stop is checked between images, or closing it would wait for ever.
            while (!m_stop.load(std::memory_order_relaxed) && m_cap->Next(0, m_image)) Process(m_image, -1);
            const auto now = std::chrono::steady_clock::now();
            const double span = std::chrono::duration<double>(now - windowStart).count();
            if (span >= 1.0)
            {
                const uint64_t images = m_images.load();
                m_rate.store(static_cast<uint32_t>(10.0 * static_cast<double>(images - windowImages) / span));
                windowStart = now;
                windowImages = images;
            }
        }
    }

    std::unique_ptr<IImageCapture> m_cap;
    const char* m_kind;
    CameraSourceOptions m_opt;
    FaceSourceChannels m_channels;
    int m_channelOf[Tracker::kBlendshapes] = {};
    FaceFrameRing* m_ring = nullptr;
    int64_t m_epochUs = 0;
    FaceImage m_image;   // the worker's (live) or the Pump thread's (pull) scratch
    FrameMailbox m_preview;
    mutable std::mutex m_overlayMutex;     // the tracking thread writes, the Pump reads (OverlayFor)
    std::array<FaceOverlay, 4> m_overlays; // the last few images' (PutOverlay)
    size_t m_overlayNext = 0;
    // the tracking thread's (the worker's, or the Pump's for a video)
    std::unique_ptr<IFaceTracker> m_tracker;
    bool m_trackerFailed = false;
    bool m_trackerErrorSet = false; // m_trackerError holds a Track failure (cleared by the next success)
    uint64_t m_trackFailures = 0;
    FaceTrackerResult m_result;
    uint64_t m_nextSeq = 1;
#if WITH_HARNESS
    std::ofstream m_dump;
#endif
    mutable std::mutex m_trackerMutex; // what Describe / LastError show
    std::string m_trackerText, m_trackerError;
    std::atomic<uint64_t> m_images{0}, m_faces{0}, m_ringFull{0}, m_polls{0};
    std::atomic<uint32_t> m_rate{0}; // images per second x 10
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false}; // set under m_mutex (the wait's predicate); read between images too
    std::thread m_worker;
};

} // namespace

std::unique_ptr<IFaceSource> MakeCameraSource(std::unique_ptr<IImageCapture> capture, const char* kind, const CameraSourceOptions& options)
{
    return std::make_unique<CameraFaceSource>(std::move(capture), kind, options);
}

} // namespace FaceTracking
