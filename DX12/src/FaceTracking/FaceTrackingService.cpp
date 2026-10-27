#include "FaceTracking/FaceTrackingService.h"

#include "FaceTracking/CameraSource.h"
#include "FaceTracking/FaceCalibrationFile.h" // face_calibration.json
#include "FaceTracking/FaceImage.h"
#include "FaceTracking/FaceMath.h"
#include "FaceTracking/FacePoseMath.h"  // FacePose::Calibration
#include "FaceTracking/FaceSource.h"
#include "FaceTracking/FaceTake.h"
#include "FaceTracking/ImageCapture.h"
#include "Graphics/IGraphicsDevice.h"    // the preview's texture and QueueTextureUpload
#include "Graphics/StreamUploadTypes.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"         // a recording's take: UTF-8 path, atomic write

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

namespace FaceTracking {
namespace {
// A source's calibration and the capture filling it.
struct CalibrationEntry
{
    FacePose::Calibration cal;
    bool hasNeutral = false, hasRange = false;
    uint64_t generation = 0;
    uint64_t frames = 0;
    // a capture
    bool capturing = false, range = false;
    double seconds = 1.0;
    int64_t startUs = 0;
    int64_t wallStartUs = 0; // SteadyNowUs() at Begin: a capture the source never finishes gives up
    bool started = false;
    double progress = 0.0;
    std::string error;       // why the last capture ended without a calibration ("" = it did not)
    double sum[kArkitChannelCount] = {};
    uint32_t count[kArkitChannelCount] = {};
    float high[kArkitChannelCount] = {};
    double head[4] = {};
    DirectX::XMFLOAT4 headRef = {0, 0, 0, 1};
    uint64_t headCount = 0;
    uint64_t taken = 0;

    // How long, on the wall clock, a capture of `seconds` may wait for its source: no face at all,
    // or a source whose time stands still (a synthetic or replay source in Edit mode or while the
    // game is paused), would otherwise keep it capturing for ever.
    int64_t TimeoutUs() const { return static_cast<int64_t>(std::max(10.0, seconds * 3.0 + 5.0) * 1e6); }

    void Begin(bool isRange, double secs, int64_t wallNowUs)
    {
        capturing = true;
        range = isRange;
        seconds = secs;
        wallStartUs = wallNowUs;
        error.clear();
        started = false;
        progress = 0.0;
        std::fill(std::begin(sum), std::end(sum), 0.0);
        std::fill(std::begin(count), std::end(count), 0u);
        std::fill(std::begin(high), std::end(high), 0.0f);
        std::fill(std::begin(head), std::end(head), 0.0);
        headCount = taken = 0;
    }
    // A found frame of the source; true when the capture just finished (generation bumped).
    bool Take(const FaceFrame& f, uint64_t& nextGeneration)
    {
        if (!capturing || !f.faceFound) return false;
        if (!started)
        {
            started = true;
            startUs = f.sourceTimeUs;
        }
        const double t = static_cast<double>(f.sourceTimeUs - startUs) / 1e6;
        progress = std::min(1.0, seconds > 0.0 ? t / seconds : 1.0);
        if (t > seconds) return Finish(nextGeneration);
        for (int ch = 0; ch < kArkitChannelCount; ++ch)
        {
            if (!f.Has(ch)) continue;
            sum[ch] += f.values[ch];
            ++count[ch];
            high[ch] = std::max(high[ch], f.values[ch]);
        }
        if (f.flags & kFaceHasHead)
        {
            // Quaternions averaged in one hemisphere (q and -q are one rotation).
            DirectX::XMFLOAT4 q = f.headRot;
            if (headCount == 0) headRef = q;
            if (q.x * headRef.x + q.y * headRef.y + q.z * headRef.z + q.w * headRef.w < 0.0f)
                q = {-q.x, -q.y, -q.z, -q.w};
            head[0] += q.x;
            head[1] += q.y;
            head[2] += q.z;
            head[3] += q.w;
            ++headCount;
        }
        ++taken;
        return false;
    }
    bool Finish(uint64_t& nextGeneration)
    {
        capturing = false;
        progress = 1.0;
        if (taken == 0) return false;
        for (int ch = 0; ch < kArkitChannelCount; ++ch)
        {
            if (count[ch] == 0) continue;
            if (range) cal.maximum[ch] = std::max(high[ch], cal.neutral[ch] + 0.05f);
            else
            {
                cal.neutral[ch] = static_cast<float>(sum[ch] / count[ch]);
                // A neutral above a range taken earlier would leave no span: that range goes.
                if (cal.maximum[ch] < cal.neutral[ch] + 0.05f) cal.maximum[ch] = 1.0f;
            }
        }
        if (!range && headCount > 0)
        {
            const double n = std::sqrt(head[0] * head[0] + head[1] * head[1] + head[2] * head[2] + head[3] * head[3]);
            if (n > 1e-9)
                cal.headNeutral = {static_cast<float>(head[0] / n), static_cast<float>(head[1] / n), static_cast<float>(head[2] / n),
                                   static_cast<float>(head[3] / n)};
        }
        (range ? hasRange : hasNeutral) = true;
        frames = taken;
        generation = nextGeneration++;
        return true;
    }
};

// Recording: a take being recorded from a source - the frames its history took, timed from the
// first one. Written when it stops: StopRecording, Close or Shutdown.
constexpr double kRecordMaxRate = 240.0; // frames a second a take may hold: twice the fastest source a person drives

struct Recording
{
    std::string path;
    FaceTake take;
    int64_t t0 = 0;
    bool started = false;
    int64_t maxUs = 0;
    size_t maxFrames = 0;
    bool truncated = false;

    void Add(const FaceFrame& f)
    {
        if (truncated) return;
        if (!started)
        {
            started = true;
            t0 = f.sourceTimeUs;
            take.mirrored = (f.flags & kFaceMirrored) != 0;
        }
        const int64_t t = f.sourceTimeUs - t0;
        FaceFrame c = f;
        c.sourceTimeUs = t;
        c.seq = 0; // the player numbers a take's frames
        // One frame an instant: a source whose time stands still (a synthetic source in Edit mode
        // or while the game is paused) replaces the frame it gave at that time instead of growing
        // the take without bound - and the take's length says how long its time really ran.
        if (!take.frames.empty() && take.frames.back().sourceTimeUs == t)
        {
            take.frames.back() = c;
            return;
        }
        if (t > maxUs || take.frames.size() >= maxFrames)
        {
            truncated = true;
            return;
        }
        take.frames.push_back(c);
    }
    double Seconds() const { return take.frames.empty() ? 0.0 : static_cast<double>(take.frames.back().sourceTimeUs) / 1e6; }
};

struct Source
{
    // The ring is declared BEFORE the source, so it is destroyed AFTER it: a network source's
    // destructor joins its receive thread, which pushes into this ring until then.
    std::unique_ptr<FaceFrameRing> ring = std::make_unique<FaceFrameRing>();
    std::unique_ptr<IFaceSource> impl;
    FaceHistory history;
    // The source's clock: the sum of Pump dt since it opened, or (a live source) the wall clock
    // since epochUs, less its delay.
    int64_t clockUs = 0;
    int64_t epochUs = 0;   // SteadyNowUs() when it opened
    uint64_t received = 0;
    uint64_t nextSeq = 1;  // push sources number their frames here
    uint64_t epoch = 0;    // Impl::nextEpoch when it opened (SourceStatus::epoch)
    std::unique_ptr<Recording> rec; // a take being recorded from it
};

// FNV-1a over the fields that matter - never over a struct's padding.
struct Fnv
{
    uint64_t h = 1469598103934665603ull;
    void Bytes(const void* p, size_t n)
    {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    }
    template <class T>
    void Add(const T& v) { Bytes(&v, sizeof(v)); }
};

int64_t NowUs() { return SteadyNowUs(); }

// Writes a stopped recording; nothing when no frame arrived while it ran.
bool FinishRecording(const std::string& name, Recording& rec, RecordingResult& out, std::string& error)
{
    out = RecordingResult{};
    out.path = rec.path;
    out.frames = rec.take.frames.size();
    out.truncated = rec.truncated;
    if (rec.take.frames.empty())
    {
        error = "no frame arrived while '" + name + "' was recorded: nothing written";
        return false;
    }
    if (rec.take.frames.size() >= 2)
    {
        out.seconds = static_cast<double>(rec.take.frames.back().sourceTimeUs - rec.take.frames.front().sourceTimeUs) / 1e6;
        rec.take.fps = out.seconds > 0.0 ? static_cast<double>(rec.take.frames.size() - 1) / out.seconds : 0.0;
    }
    std::ostringstream o;
    if (!WriteTake(o, rec.take, error)) return false;
    const std::string bytes = o.str();
    std::error_code ec;
    const std::filesystem::path parent = Sys::U8Path(rec.path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (!Sys::WriteFileBytesAtomic(rec.path, bytes.data(), bytes.size()))
    {
        error = "cannot write '" + rec.path + "'";
        return false;
    }
    LOG_INFO("[FACETRACK] recorded '%s': %llu frames, %.3f s -> '%s'%s", name.c_str(),
             static_cast<unsigned long long>(out.frames), out.seconds, rec.path.c_str(),
             out.truncated ? " (stopped taking frames at its maxSeconds)" : "");
    return true;
}

void HashFrame(Fnv& h, const FaceFrame& f)
{
    h.Add(f.sourceTimeUs);
    h.Add(f.present);
    for (int ch = 0; ch < kMaxFaceChannels; ++ch)
        if (f.Has(ch)) h.Add(f.values[ch]);
    if (f.flags & kFaceHasHead) h.Add(f.headRot);
    if (f.flags & kFaceHasHeadPos) h.Add(f.headPos);
    if (f.flags & kFaceHasEyes) h.Add(f.eyeYawPitch);
    h.Add(f.faceFound);
    h.Add(f.confidence);
}

#if WITH_HARNESS

// -facetrack_probe_consumers=N: N stand-ins for characters bound to one source; the k-th
// (from 1) reads every k-th Pump.
struct ProbeConsumer
{
    int every = 1;
    FaceCursor cursor;
    uint64_t frames = 0, first = 0, last = 0;
    Fnv hash;
};

void ReadProbe(ProbeConsumer& p, const FaceHistory& history)
{
    history.ReadNew(p.cursor, [&](const FaceFrame& f) {
        if (p.frames == 0) p.first = f.seq;
        p.last = f.seq;
        ++p.frames;
        p.hash.Add(f.seq);
        HashFrame(p.hash, f);
    });
}
#endif

} // namespace

struct Service::Impl
{
    mutable std::mutex mutex;
    std::map<std::string, Source, std::less<>> sources; // ordered: the sample log is deterministic
    uint64_t pumps = 0;
    uint64_t nextEpoch = 1; // every open takes the next: a reopened name is a new epoch
    // Calibrations by source name (they outlive the sources), and where they are kept.
    std::map<std::string, CalibrationEntry, std::less<>> calibrations;
    uint64_t nextCalibration = 1;
    bool persistCalibration = false; // not under -unattended
    std::string calibrationPath;
    int sampleEvery = 0;
    CameraSourceOptions imageOptions; // -facetrack_frame_log, -facetrack_threads, -facetrack_tracker_dump
    bool used = false; // something was opened: Shutdown says so in the log
#if WITH_HARNESS
    std::vector<ProbeConsumer> probes;
#endif

    // The player's camera permission (faceTracking.camera); off until the setting says.
    std::atomic<bool> cameraAllowed{false};

    // ListCameras' cache (listing opens every device: the LEDs blink).
    bool camerasListed = false;
    std::vector<CameraDevice> cameras;

    // ---- Preview ----
    IGraphicsDevice* gfx = nullptr;
    std::string previewSource;
    uint64_t previewAskedPump = 0;   // the Pump of the last RequestPreview
    int64_t previewLastUs = 0;
    RHI::Texture previewTex;
    bool previewTexValid = false;
    FaceImage previewImage;          // scratch
    std::vector<uint8_t> previewRgba, previewScaled;
    PreviewStats preview;
    PreviewOverlay previewOverlay;   // the shown image's (GetPreviewOverlay)

    // Creates the preview texture once, when the device arrives: in SHADER_RESOURCE, never recreated
    // (every upload overwrites it in place). Under the mutex.
    void CreatePreviewTexture()
    {
        if (previewTexValid || !gfx) return;
        RHI::TextureDesc d;
        d.format = RHI::Format::R8G8B8A8_UNORM;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.width = kPreviewWidth;
        d.height = kPreviewHeight;
        d.layout = RHI::ResourceState::SHADER_RESOURCE; // a stream upload target is created in SHADER_RESOURCE
        d.debugName = "FaceTrackingPreview";
        previewTexValid = gfx->CreateTexture(d, previewTex, nullptr);
        if (!previewTexValid) LOG_WARNING("[FACETRACK] the preview texture could not be created: no editor preview");
    }

    // Moves the newest image of the previewed source to the texture, at most 15 Hz, only while
    // asked for this Pump or the last (a hidden panel uploads nothing). Under the mutex.
    void UploadPreview(int64_t now)
    {
        preview.visible = !previewSource.empty() && previewAskedPump + 1 >= pumps;
        if (!preview.visible || !previewTexValid || now - previewLastUs < 66666) return;
        const auto it = sources.find(previewSource);
        if (it == sources.end() || !it->second.impl->TakePreview(previewImage) || previewImage.Empty()) return;
        ++preview.images;
        previewLastUs = now;
        // Fit the image into the fixed texture, letterboxed: an integer box downscale first,
        // then a nearest fit (both cheap at 15 Hz on <= 1080p).
        uint32_t factor = 1;
        while (previewImage.width / (factor + 1) >= kPreviewWidth && previewImage.height / (factor + 1) >= kPreviewHeight / 2) ++factor;
        uint32_t sw = 0, sh = 0;
        Nv12ToRgbaDownscaled(previewImage, factor, previewScaled, sw, sh);
        if (sw == 0 || sh == 0) return;
        const float scale = std::min(float(kPreviewWidth) / float(sw), float(kPreviewHeight) / float(sh));
        const uint32_t dw = std::max(1u, static_cast<uint32_t>(sw * scale)), dh = std::max(1u, static_cast<uint32_t>(sh * scale));
        const uint32_t ox = (kPreviewWidth - dw) / 2, oy = (kPreviewHeight - dh) / 2;
        // The tracker's overlay of this image, mapped the way its pixels are (the box downscale, then the
        // fit and the letterbox): kept once the upload lands, with the image the texture then holds.
        PreviewOverlay overlay;
        overlay.source = previewSource;
        overlay.valid = it->second.impl->OverlayFor(previewImage.seq, overlay.face);
        if (overlay.valid)
        {
            const float k = scale / static_cast<float>(factor);
            auto map = [&](float& x, float& y) {
                x = static_cast<float>(ox) + x * k;
                y = static_cast<float>(oy) + y * k;
            };
            for (auto& corner : overlay.face.roi) map(corner[0], corner[1]);
            for (int i = 0; i < kOverlayLandmarks; ++i)
                map(overlay.face.points[static_cast<size_t>(i) * 2], overlay.face.points[static_cast<size_t>(i) * 2 + 1]);
        }
        previewRgba.assign(static_cast<size_t>(kPreviewWidth) * kPreviewHeight * 4, 0);
        for (size_t k = 3; k < previewRgba.size(); k += 4) previewRgba[k] = 255;
        for (uint32_t y = 0; y < dh; ++y)
        {
            const uint32_t syy = std::min(sh - 1, static_cast<uint32_t>(y / scale));
            for (uint32_t x = 0; x < dw; ++x)
            {
                const uint32_t sxx = std::min(sw - 1, static_cast<uint32_t>(x / scale));
                std::memcpy(&previewRgba[((static_cast<size_t>(oy) + y) * kPreviewWidth + ox + x) * 4],
                            &previewScaled[(static_cast<size_t>(syy) * sw + sxx) * 4], 4);
            }
        }
        RHI::SubresourceData plane;
        plane.dataPtr = previewRgba.data();
        plane.rowPitch = kPreviewWidth * 4;
        plane.slicePitch = plane.rowPitch * kPreviewHeight;
        StreamUpload::Desc desc;
        desc.planes = &plane;
        desc.planeCount = 1;
        desc.policy = StreamUpload::Policy::LatestWins;
        ++preview.uploads;
        bool landed = true;
        switch (gfx->QueueTextureUpload(previewTex, desc))
        {
        case StreamUpload::Result::Queued: ++preview.queued; break;
        case StreamUpload::Result::Replaced: ++preview.replaced; break;
        case StreamUpload::Result::CarriedOver: ++preview.carriedOver; break;
        case StreamUpload::Result::Fallback: ++preview.fallback; break;
        case StreamUpload::Result::Dropped: ++preview.dropped; landed = false; break;
        case StreamUpload::Result::Rejected: ++preview.rejected; landed = false; break;
        }
        if (landed)
        {
            preview.shown = previewSource;
            previewOverlay = overlay;
        }
    }

    // face_calibration.json, through FaceCalibrationFile.h. Under the mutex.
    void SaveCalibrations() const
    {
        if (!persistCalibration || calibrationPath.empty()) return;
        StoredCalibrations all;
        for (const auto& [name, c] : calibrations)
            if (c.hasNeutral || c.hasRange) all[name] = StoredCalibration{c.cal, c.hasNeutral, c.hasRange, c.frames};
        const std::string text = WriteCalibrationFile(all);
        if (!Sys::WriteFileBytesAtomic(calibrationPath, text.data(), text.size()))
            LOG_WARNING("[FACETRACK] calibration: cannot write '%s'", calibrationPath.c_str());
    }
    // The file is the user's: a bad one costs the calibrations in it, never the start of the program.
    void LoadCalibrations()
    {
        std::string text;
        if (!Sys::ReadFileText(calibrationPath, text)) return;
        StoredCalibrations all;
        std::string why;
        int skipped = 0;
        if (!ReadCalibrationFile(text, all, why, skipped))
        {
            LOG_WARNING("[FACETRACK] calibration: '%s' is not a calibration file this build reads (%s) - ignored; the next "
                        "calibration replaces it", calibrationPath.c_str(), why.c_str());
            return;
        }
        for (const auto& [name, stored] : all)
        {
            CalibrationEntry c;
            c.cal = stored.cal;
            c.hasNeutral = stored.hasNeutral;
            c.hasRange = stored.hasRange;
            c.frames = stored.frames;
            c.generation = nextCalibration++;
            calibrations[name] = c;
        }
        if (skipped > 0)
            LOG_WARNING("[FACETRACK] calibration: '%s': %d entr(ies) or value(s) that are not what the format says were left out",
                        calibrationPath.c_str(), skipped);
        LOG_INFO("[FACETRACK] calibration: %zu source(s) from '%s'", calibrations.size(), calibrationPath.c_str());
    }

    void LogSample(const std::string& name, const Source& s) const
    {
        FaceFrame f;
        const SampleState st = s.history.Sample(s.clockUs, f);
        Fnv h;
        h.Add(st);
        if (st != SampleState::Empty) HashFrame(h, f);
        char head[96] = "head=-";
        if (st != SampleState::Empty && (f.flags & kFaceHasHead))
        {
            const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&f.headRot));
            const float toDeg = 180.0f / FaceMath::kPi;
            std::snprintf(head, sizeof(head), "yaw=%.4f pitch=%.4f roll=%.4f", ypr.yaw * toDeg, ypr.pitch * toDeg,
                          ypr.roll * toDeg);
        }
        std::string values;
        int shown = 0;
        if (st != SampleState::Empty)
        {
            for (int ch = 0; ch < kMaxFaceChannels && shown < 8; ++ch)
            {
                if (!f.Has(ch)) continue;
                char one[96];
                std::snprintf(one, sizeof(one), "%s%.*s:%.6f", shown ? "," : "",
                              static_cast<int>(s.impl->Channels().Name(ch).size()), s.impl->Channels().Name(ch).data(),
                              f.values[ch]);
                values += one;
                ++shown;
            }
        }
        LOG_INFO("[FACETRACK] sample pump=%llu source=%s t=%lld state=%s seq=%llu found=%d conf=%.4f %s v=%s hash=%016llx",
                 static_cast<unsigned long long>(pumps), name.c_str(), static_cast<long long>(s.clockUs),
                 SampleStateName(st), static_cast<unsigned long long>(f.seq), st == SampleState::Empty ? 0 : f.faceFound,
                 st == SampleState::Empty ? 0.0f : f.confidence, head, values.empty() ? "-" : values.c_str(),
                 static_cast<unsigned long long>(h.h));
    }
};

Service& Service::Get()
{
    static Service s_instance;
    return s_instance;
}

Service::Service() : m(std::make_unique<Impl>()) {}
Service::~Service() = default;

static const char* kCameraOff =
    "the camera is off: face tracking may use it only while the player allows it (the setting faceTracking.camera)";

void Service::Initialize()
{
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    if (cl.Has("-facetrack_off"))
    {
        m_enabled = false;
        LOG_INFO("[FACETRACK] off (-facetrack_off): no sources; FaceTracking.* calls fail");
        return;
    }
    m_enabled = true;
    m->sampleEvery = std::max(0, cl.GetInt("-facetrack_sample_every", 0));
    m->imageOptions.frameLog = cl.Has("-facetrack_frame_log");
    // The built-in tracker's ONNX Runtime threads per image source (default 2).
    m->imageOptions.tracker.threads = std::clamp(cl.GetInt("-facetrack_threads", 2), 1, 8);
#if WITH_HARNESS
    // -facetrack_tracker_dump=<file>: every tracked image as a JSON line (tools/face_golden.py compare); a bare
    // name lands in the run's artifact folder.
    if (const std::string dump = cl.Get("-facetrack_tracker_dump"); !dump.empty())
        m->imageOptions.trackerDump = (dump.find_first_of("/\\") == std::string::npos && Sys::HasArtifactDir())
                                          ? Sys::ArtifactDir() + "/" + dump
                                          : dump;
#endif
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        m->persistCalibration = !cl.Has("-unattended");
        m->calibrationPath = Sys::UserDataDir() + "/face_calibration.json";
        if (m->persistCalibration) m->LoadCalibrations();
    }
#if WITH_HARNESS
    const int probes = std::clamp(cl.GetInt("-facetrack_probe_consumers", 0), 0, 8);
    m->probes.assign(static_cast<size_t>(probes), ProbeConsumer{});
    for (int k = 0; k < probes; ++k) m->probes[static_cast<size_t>(k)].every = k + 1;
#endif
    const std::string replay = cl.Get("-facetrack_replay");
    if (!replay.empty())
    {
        const bool loop = cl.GetInt("-facetrack_replay_loop", 1) != 0;
        std::string error;
        if (!OpenReplay(replay, "default", loop, error))
            LOG_WARNING("[FACETRACK] -facetrack_replay=%s: %s", replay.c_str(), error.c_str());
    }
    // -facetrack_listen=vmc:39539 (or ifacialmocap:49983; the port may be left out): a network
    // source named "default", loopback only - the LAN is FaceTracking.OpenNetwork's lan = true.
    const std::string listen = cl.Get("-facetrack_listen");
    if (!listen.empty())
    {
        const size_t colon = listen.find(':');
        NetworkSourceOptions o;
        uint16_t defaultPort = 0;
        std::string error;
        if (!ParseNetworkProtocol(std::string_view(listen).substr(0, colon), o.protocol, defaultPort))
            error = "the protocol is vmc or ifacialmocap";
        else
        {
            o.port = defaultPort;
            if (colon != std::string::npos)
            {
                const int port = std::atoi(listen.c_str() + colon + 1);
                if (port < 0 || port > 65535) error = "the port is 0..65535";
                o.port = static_cast<uint16_t>(std::clamp(port, 0, 65535));
            }
        }
        if (error.empty()) OpenNetwork(o, "default", error);
        if (!error.empty()) LOG_WARNING("[FACETRACK] -facetrack_listen=%s: %s", listen.c_str(), error.c_str());
    }
    // -facetrack_video=<file>: a video source named "default" - every frame, in order, on the
    // simulation clock.
    const std::string video = cl.Get("-facetrack_video");
    if (!video.empty())
    {
        std::string error;
        if (!OpenVideo(video, "default", error))
            LOG_WARNING("[FACETRACK] -facetrack_video=%s: %s", video.c_str(), error.c_str());
    }
#if WITH_HARNESS
    // -facetrack_fault=denied|busy|lost (none: a healthy one): a fake camera named "default".
    if (cl.Has("-facetrack_fault"))
    {
        std::string fault = cl.Get("-facetrack_fault");
        if (fault == "none") fault.clear();
        std::string error;
        if (!OpenFakeCamera(fault, "default", error))
            LOG_WARNING("[FACETRACK] -facetrack_fault=%s: %s", fault.c_str(), error.c_str());
    }
#endif
}

void Service::Shutdown()
{
    std::lock_guard<std::mutex> lock(m->mutex);
#if WITH_HARNESS
    if (!m->probes.empty())
    {
        const auto it = m->sources.find("default");
        for (size_t k = 0; k < m->probes.size(); ++k)
        {
            ProbeConsumer& p = m->probes[k];
            if (it != m->sources.end()) ReadProbe(p, it->second.history); // catch up first
            LOG_INFO("[FACETRACK] consumer=%zu source=default every=%d frames=%llu first=%llu last=%llu missed=%llu "
                     "hash=%016llx",
                     k, p.every, static_cast<unsigned long long>(p.frames), static_cast<unsigned long long>(p.first),
                     static_cast<unsigned long long>(p.last), static_cast<unsigned long long>(p.cursor.missed),
                     static_cast<unsigned long long>(p.hash.h));
        }
        m->probes.clear();
    }
#endif
    if (m->used)
        LOG_INFO("[FACETRACK] shutdown: %zu source(s) closed after %llu pumps", m->sources.size(),
                 static_cast<unsigned long long>(m->pumps));
    if (m->preview.uploads > 0)
        LOG_INFO("[FACETRACK] preview: %llu uploads (%llu queued, %llu replaced, %llu carried over, %llu dropped, "
                 "%llu rejected, %llu fallback) of %llu images",
                 static_cast<unsigned long long>(m->preview.uploads), static_cast<unsigned long long>(m->preview.queued),
                 static_cast<unsigned long long>(m->preview.replaced), static_cast<unsigned long long>(m->preview.carriedOver),
                 static_cast<unsigned long long>(m->preview.dropped), static_cast<unsigned long long>(m->preview.rejected),
                 static_cast<unsigned long long>(m->preview.fallback), static_cast<unsigned long long>(m->preview.images));
    for (auto& [name, s] : m->sources)
        if (s.rec)
        {
            s.rec->take.channels = s.impl->Channels();
            RecordingResult r;
            std::string err;
            if (!FinishRecording(name, *s.rec, r, err)) LOG_WARNING("[FACETRACK] recording of '%s': %s", name.c_str(), err.c_str());
        }
    m->sources.clear();
    ReleaseFaceTrackerModels(); // every tracker went with its source
    // No frame is in flight (App::ShutdownAllSystems): the preview texture can go now.
    if (m->previewTexValid && m->gfx) m->gfx->DestroyTexture(m->previewTex);
    m->previewTexValid = false;
    m->previewSource.clear();
    m->preview.shown.clear();
    m->gfx = nullptr;
}

void Service::Pump(float dt)
{
    if (!m_enabled) return;
    std::lock_guard<std::mutex> lock(m->mutex);
    ++m->pumps;
    const int64_t stepUs = (dt > 0.0f && std::isfinite(dt)) ? std::llround(static_cast<double>(dt) * 1e6) : 0;
    const int64_t now = NowUs();
    for (auto& [name, s] : m->sources)
    {
        // A live source's clock is the wall clock less its delay, and never runs backwards.
        if (s.impl->WallClock()) s.clockUs = std::max(s.clockUs, now - s.epochUs - s.impl->DelayUs());
        else s.clockUs += stepUs;
        s.impl->Produce(s.clockUs, *s.ring);
        const bool stamp = s.impl->StampsOnDrain();
        FaceFrame f;
        while (s.ring->TryPop(f))
        {
            if (stamp) f.sourceTimeUs = s.clockUs;
            if (s.history.Append(f))
            {
                ++s.received;
                if (s.rec) s.rec->Add(f);
                if (const auto c = m->calibrations.find(name); c != m->calibrations.end() && c->second.capturing)
                    if (c->second.Take(f, m->nextCalibration))
                    {
                        LOG_INFO("[FACETRACK] calibration '%s': %s from %llu frames (generation %llu)", name.c_str(),
                                 c->second.range ? "range" : "neutral", static_cast<unsigned long long>(c->second.frames),
                                 static_cast<unsigned long long>(c->second.generation));
                        m->SaveCalibrations();
                    }
            }
        }
        s.history.Trim();
    }
    for (auto& [name, c] : m->calibrations)
        if (c.capturing && now - c.wallStartUs > c.TimeoutUs())
        {
            const double waited = static_cast<double>(now - c.wallStartUs) / 1e6;
            char why[256];
            if (!c.started)
                std::snprintf(why, sizeof(why), "no face came from the source in %.0f s", waited);
            else
                std::snprintf(why, sizeof(why), "the source's time ran %.2f of the %.2f s in %.0f s (a synthetic or replay "
                              "source's time stands still in Edit mode or while the game is paused)",
                              c.progress * c.seconds, c.seconds, waited);
            c.capturing = false;
            c.error = why;
            LOG_WARNING("[FACETRACK] calibration '%s': %s - nothing taken", name.c_str(), why);
        }
#if WITH_HARNESS
    if (!m->probes.empty())
    {
        const auto it = m->sources.find("default");
        if (it != m->sources.end())
            for (ProbeConsumer& p : m->probes)
                if ((m->pumps - 1) % static_cast<uint64_t>(p.every) == 0) ReadProbe(p, it->second.history);
    }
#endif
    if (m->sampleEvery > 0 && (m->pumps - 1) % static_cast<uint64_t>(m->sampleEvery) == 0)
        for (const auto& [name, s] : m->sources) m->LogSample(name, s);
    m->UploadPreview(now);
}

bool Service::OpenReplay(const std::string& path, const std::string& name, bool loop, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (name.empty())
    {
        error = "a source needs a name";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false;
        }
    }
    std::string text;
    if (!Resource::AssetFS::Get().ReadFileText(path, text))
    {
        error = "cannot read '" + path + "'";
        return false;
    }
    FaceTake take;
    std::istringstream in(text);
    if (!ParseTake(in, take, error))
    {
        error = path + ": " + error;
        return false;
    }
    if (take.frames.empty())
    {
        error = path + ": the take has a header but no frames";
        return false;
    }
    std::unique_ptr<IFaceSource> replay = MakeReplaySource(std::move(take), path, loop);
    const std::string detail = replay->Describe();
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false;
        }
        Source s;
        s.impl = std::move(replay);
        s.epoch = m->nextEpoch++;
        m->sources.emplace(name, std::move(s));
        m->used = true;
    }
    LOG_INFO("[FACETRACK] source '%s' opened: replay %s", name.c_str(), detail.c_str());
    return true;
}

bool Service::CreateSynthetic(const std::string& name, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (name.empty())
    {
        error = "a source needs a name";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false;
        }
        Source s;
        s.impl = std::make_unique<SyntheticFaceSource>();
        s.epoch = m->nextEpoch++;
        m->sources.emplace(name, std::move(s));
        m->used = true;
    }
    LOG_INFO("[FACETRACK] source '%s' opened: synthetic", name.c_str());
    return true;
}

bool Service::OpenNetwork(const NetworkSourceOptions& options, const std::string& name, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (name.empty())
    {
        error = "a source needs a name";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false;
        }
    }
    // Bound outside the lock (a bind can take a moment); the ring is heap-allocated, so moving
    // the Source into the table leaves the pointer the receive thread holds valid.
    Source s;
    s.impl = MakeNetworkSource(options);
    s.epochUs = NowUs();
    if (!s.impl->Start(*s.ring, s.epochUs, error)) return false;
    const std::string detail = s.impl->Describe();
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false; // s (and its thread) goes away here, outside the table
        }
        s.epoch = m->nextEpoch++;
        m->sources.emplace(name, std::move(s));
        m->used = true;
    }
    LOG_INFO("[FACETRACK] source '%s' opened: %s", name.c_str(), detail.c_str());
    return true;
}

bool Service::Close(const std::string& name)
{
    Source closed; // destroyed after the lock: a network source joins its receive thread
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        const auto it = m->sources.find(name);
        if (it == m->sources.end()) return false;
        closed = std::move(it->second);
        m->sources.erase(it);
        if (const auto c = m->calibrations.find(name); c != m->calibrations.end() && c->second.capturing)
        {
            c->second.capturing = false;
            c->second.error = "the source was closed";
        }
    }
    if (closed.rec)
    {
        closed.rec->take.channels = closed.impl->Channels();
        RecordingResult r;
        std::string err;
        if (!FinishRecording(name, *closed.rec, r, err)) LOG_WARNING("[FACETRACK] recording of '%s': %s", name.c_str(), err.c_str());
    }
    LOG_INFO("[FACETRACK] source '%s' closed", name.c_str());
    return true;
}

bool Service::StartCalibration(const std::string& source, bool range, double seconds, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (!(seconds > 0.0 && seconds <= 30.0))
    {
        error = "seconds is more than 0 and at most 30";
        return false;
    }
    std::lock_guard<std::mutex> lock(m->mutex);
    if (!m->sources.count(source))
    {
        error = "no source named '" + source + "'";
        return false;
    }
    m->calibrations[source].Begin(range, seconds, NowUs());
    LOG_INFO("[FACETRACK] calibration '%s': capturing %s for %.2f s", source.c_str(), range ? "range" : "neutral", seconds);
    return true;
}

void Service::CancelCalibration(const std::string& source)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    if (const auto it = m->calibrations.find(source); it != m->calibrations.end()) it->second.capturing = false;
}

void Service::ResetCalibration(const std::string& source)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->calibrations.find(source);
    if (it == m->calibrations.end()) return;
    m->calibrations.erase(it);
    m->SaveCalibrations();
    LOG_INFO("[FACETRACK] calibration '%s': reset", source.c_str());
}

CalibrationStatus Service::GetCalibrationStatus(const std::string& source) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    CalibrationStatus out;
    const auto it = m->calibrations.find(source);
    if (it == m->calibrations.end()) return out;
    const CalibrationEntry& c = it->second;
    out.capturing = c.capturing;
    out.mode = c.capturing ? (c.range ? "range" : "neutral") : "";
    out.progress = c.progress;
    out.generation = c.generation;
    out.hasNeutral = c.hasNeutral;
    out.hasRange = c.hasRange;
    out.frames = c.frames;
    out.error = c.error;
    return out;
}

bool Service::GetCalibration(const std::string& source, FacePose::Calibration& out, uint64_t& generation) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->calibrations.find(source);
    if (it == m->calibrations.end() || it->second.generation == 0) return false;
    out = it->second.cal;
    generation = it->second.generation;
    return true;
}

bool Service::StartRecording(const std::string& source, const std::string& path, double maxSeconds, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (path.empty())
    {
        error = "a recording needs a path (.iface)";
        return false;
    }
    if (!(maxSeconds > 0.0 && maxSeconds <= 3600.0))
    {
        error = "maxSeconds is more than 0 and at most 3600";
        return false;
    }
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(source);
    if (it == m->sources.end())
    {
        error = "no source named '" + source + "'";
        return false;
    }
    Source& s = it->second;
    if (s.rec)
    {
        error = "'" + source + "' is being recorded already (to '" + s.rec->path + "')";
        return false;
    }
    auto rec = std::make_unique<Recording>();
    rec->path = path;
    rec->maxUs = static_cast<int64_t>(maxSeconds * 1e6);
    rec->maxFrames = static_cast<size_t>(maxSeconds * kRecordMaxRate) + 1;
    rec->take.sourceKind = s.impl->Kind();
    rec->take.channels = s.impl->Channels();
    s.rec = std::move(rec);
    LOG_INFO("[FACETRACK] recording '%s' -> '%s' (at most %.0f s)", source.c_str(), path.c_str(), maxSeconds);
    return true;
}

bool Service::StopRecording(const std::string& source, RecordingResult& out, std::string& error)
{
    std::unique_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        const auto it = m->sources.find(source);
        if (it == m->sources.end())
        {
            error = "no source named '" + source + "'";
            return false;
        }
        if (!it->second.rec)
        {
            error = "'" + source + "' is not being recorded";
            return false;
        }
        rec = std::move(it->second.rec);
        rec->take.channels = it->second.impl->Channels(); // extras registered while it recorded
    }
    return FinishRecording(source, *rec, out, error); // written outside the lock
}

bool Service::OpenCapture(std::unique_ptr<IImageCapture> capture, const char* kind, const std::string& name,
                          std::string& error, int trackDelayMs)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (name.empty())
    {
        error = "a source needs a name";
        return false;
    }
    if (!capture)
    {
        error = std::string(kind) + " sources are not in this build";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false;
        }
    }
    // Started outside the lock, as a network source is: opening a file or a device takes a
    // moment, and a camera's worker never takes the service's mutex.
    Source s;
    CameraSourceOptions options = m->imageOptions;
    options.trackDelayMs = trackDelayMs;
    s.impl = MakeCameraSource(std::move(capture), kind, options);
    s.epochUs = NowUs();
    if (!s.impl->Start(*s.ring, s.epochUs, error)) return false;
    const std::string detail = s.impl->Describe();
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->sources.count(name))
        {
            error = "a source named '" + name + "' is open already";
            return false; // s (and its worker) goes away here, outside the table
        }
        s.epoch = m->nextEpoch++;
        m->sources.emplace(name, std::move(s));
        m->used = true;
    }
    LOG_INFO("[FACETRACK] source '%s' opened: %s %s", name.c_str(), kind, detail.c_str());
    return true;
}

void Service::SetCameraAllowed(bool allowed)
{
    const bool was = m->cameraAllowed.exchange(allowed);
    if (was == allowed) return;
    LOG_INFO("[FACETRACK] camera %s (faceTracking.camera)", allowed ? "allowed" : "off");
    if (allowed) return;
    // Turned off: every camera source closes now, and the list is forgotten.
    std::vector<std::string> cameras;
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        m->camerasListed = false;
        m->cameras.clear();
        for (const auto& [name, s] : m->sources)
            if (std::strcmp(s.impl->Kind(), "camera") == 0) cameras.push_back(name);
    }
    for (const std::string& name : cameras) Close(name);
}

bool Service::CameraAllowed() const { return m->cameraAllowed.load(); }

bool Service::OpenCamera(const std::string& deviceId, uint32_t width, uint32_t height, float fps, const std::string& name,
                         std::string& error)
{
    if (!m->cameraAllowed.load())
    {
        error = kCameraOff;
        return false;
    }
    std::string id = deviceId;
    if (id.empty())
    {
        // "" = the first camera Media Foundation lists.
        std::vector<CameraDevice> cameras;
        if (!ListCameras(cameras, false, error)) return false;
        if (cameras.empty())
        {
            error = "no camera is connected";
            return false;
        }
        id = cameras.front().id;
    }
    return OpenCapture(MakeMfCamera(id, width, height, fps), "camera", name, error);
}

bool Service::OpenFakeCamera(const std::string& fault, const std::string& name, std::string& error, const std::string& video, int fps,
                             int trackDelayMs)
{
    return OpenCapture(MakeFakeCamera(fault, video, fps), "fakecamera", name, error, trackDelayMs);
}

bool Service::OpenVideo(const std::string& path, const std::string& name, std::string& error)
{
    return OpenCapture(MakeVideoFileCapture(path), "video", name, error);
}

bool Service::ListCameras(std::vector<CameraDevice>& out, bool refresh, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return false;
    }
    if (!m->cameraAllowed.load())
    {
        error = kCameraOff;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        if (m->camerasListed && !refresh)
        {
            out = m->cameras;
            return true;
        }
    }
    std::vector<CameraDevice> cameras; // listed outside the lock: it opens every device
    if (!EnumerateCameras(cameras, error)) return false;
    std::lock_guard<std::mutex> lock(m->mutex);
    m->cameras = cameras;
    m->camerasListed = true;
    out = std::move(cameras);
    return true;
}

void Service::OnDeviceChange()
{
    std::lock_guard<std::mutex> lock(m->mutex);
    m->camerasListed = false;
    for (auto& [name, s] : m->sources) s.impl->OnDeviceChange();
}

void Service::SetGraphicsDevice(IGraphicsDevice* gfx)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    m->gfx = gfx;
    if (m_enabled) m->CreatePreviewTexture();
}

void Service::RequestPreview(const std::string& source)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    if (source != m->previewSource)
    {
        m->previewSource = source;
        m->previewLastUs = 0; // a new source shows at once
    }
    m->previewAskedPump = m->pumps;
}

bool Service::PreviewTexture(RHI::Texture& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    if (!m->previewTexValid || m->preview.uploads == 0) return false;
    out = m->previewTex;
    return true;
}

PreviewStats Service::GetPreviewStats() const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    PreviewStats out = m->preview;
    out.source = m->previewSource;
    return out;
}

bool Service::GetPreviewOverlay(PreviewOverlay& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    out = m->previewOverlay;
    return out.valid && out.source == m->preview.shown;
}

uint64_t Service::Push(const std::string& name, const FaceFrame& frame, std::string& error)
{
    if (!m_enabled)
    {
        error = "face tracking is off (-facetrack_off)";
        return 0;
    }
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    if (it == m->sources.end())
    {
        error = "no source named '" + name + "'";
        return 0;
    }
    Source& s = it->second;
    if (!s.impl->StampsOnDrain())
    {
        error = "'" + name + "' is a " + s.impl->Kind() + " source; frames are pushed to synthetic sources only";
        return 0;
    }
    FaceFrame f = frame;
    f.seq = s.nextSeq;
    f.captureQpcUs = NowUs();
    if (!s.ring->TryPush(f))
    {
        error = "the source's ring is full - nothing drained it (Pump runs in the animation chain)";
        return 0;
    }
    ++s.nextSeq;
    return f.seq;
}

int Service::FindChannel(const std::string& source, std::string_view channel, bool addExtra)
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(source);
    if (it == m->sources.end()) return FindArkitChannel(channel);
    if (addExtra)
        if (auto* synthetic = dynamic_cast<SyntheticFaceSource*>(it->second.impl.get()))
            return synthetic->MutableChannels().FindOrAdd(channel);
    return it->second.impl->Channels().Find(channel);
}

std::string Service::ChannelName(const std::string& source, int channel) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(source);
    if (it == m->sources.end()) return std::string(ArkitChannelName(channel));
    return std::string(it->second.impl->Channels().Name(channel));
}

bool Service::Channels(const std::string& source, FaceSourceChannels& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(source);
    if (it == m->sources.end()) return false;
    out = it->second.impl->Channels();
    return true;
}

std::vector<SourceStatus> Service::Sources() const
{
    std::vector<SourceStatus> out;
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(m->mutex);
        for (const auto& [name, s] : m->sources) names.push_back(name);
    }
    for (const std::string& name : names)
    {
        SourceStatus st;
        if (Status(name, st)) out.push_back(std::move(st));
    }
    return out;
}

bool Service::Status(const std::string& name, SourceStatus& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    if (it == m->sources.end()) return false;
    const Source& s = it->second;
    out = SourceStatus{};
    out.name = name;
    out.kind = s.impl->Kind();
    out.detail = s.impl->Describe();
    out.epoch = s.epoch;
    out.received = s.received;
    out.refused = s.ring->Refused() + s.history.Refused();
    out.historyFrames = s.history.Size();
    out.clockUs = s.clockUs;
    out.extras = s.impl->Channels().Extras().size();
    out.live = s.impl->WallClock();
    out.port = s.impl->Port();
    out.lastError = s.impl->LastError();
    out.state = s.impl->StateName();
    out.finished = s.impl->Finished();
    out.recording = s.rec != nullptr;
    out.recordedFrames = s.rec ? s.rec->take.frames.size() : 0;
    out.recordedSeconds = s.rec ? s.rec->Seconds() : 0.0;
    if (const FaceFrame* newest = s.history.Newest())
    {
        out.newestSeq = newest->seq;
        out.newestTimeUs = newest->sourceTimeUs;
        out.faceFound = newest->faceFound != 0;
        out.confidence = newest->confidence;
    }
    return true;
}

bool Service::Newest(const std::string& name, FaceFrame& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    if (it == m->sources.end() || !it->second.history.Newest()) return false;
    out = *it->second.history.Newest();
    return true;
}

SampleState Service::Sample(const std::string& name, int64_t tUs, FaceFrame& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    return it == m->sources.end() ? SampleState::Empty : it->second.history.Sample(tUs, out);
}

SampleState Service::SampleNow(const std::string& name, FaceFrame& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    return it == m->sources.end() ? SampleState::Empty : it->second.history.Sample(it->second.clockUs, out);
}

bool Service::ReadNew(const std::string& name, FaceCursor& cursor, std::vector<FaceFrame>& out) const
{
    std::lock_guard<std::mutex> lock(m->mutex);
    const auto it = m->sources.find(name);
    if (it == m->sources.end()) return false;
    it->second.history.ReadNew(cursor, [&](const FaceFrame& f) { out.push_back(f); });
    return true;
}

} // namespace FaceTracking
