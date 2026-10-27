#pragma once
// FaceTracking::Service: owns every face source, by name, independent of any World - Play /
// Stop clones of the World never see a device or a take. Once per engine frame Pump() advances
// each source's clock, lets clocked sources produce, and drains every ring into that source's
// history; readers (one binding per character) keep their own FaceCursor into the history, so
// any number of them can read one source.
//
// Flags it reads itself (never in the App files):
//   -facetrack_off                  no sources, Pump does nothing, the Lua calls fail
//   -facetrack_replay=<take.iface>  a replay source named "default"
//   -facetrack_replay_loop=0        ... played once, then held (default: looping)
//   -facetrack_listen=<proto>:<port> a network source named "default" (vmc / ifacialmocap;
//                                   127.0.0.1 only - the LAN is a Lua option)
//   -facetrack_video=<file.mp4>     a video source named "default" (in order, the simulation clock)
//   -facetrack_frame_log            a "[FACETRACK] image" log line per image of every image source
//   -facetrack_fault=denied|busy|lost (harness builds) a fake camera named "default" that fails that
//                                   way (=none: a healthy one)
//   -facetrack_sample_every=N       a "[FACETRACK] sample" log line per source every N Pumps
//   -facetrack_probe_consumers=N    (harness builds) N readers on "default", reader k every
//                                   k+1 Pumps; their totals are logged at Shutdown
//
// Thread model: the animation chain (main thread, game half) calls Pump; the Lua bindings
// call the rest from the main thread. One mutex guards the source table; a source's ring
// is its only hand-off from another producer (a network source's receive thread, a camera
// source's worker). The editor preview is uploaded from Pump - the game half, after the
// frame's upload slot is acquired - never from UI code.

#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceHistory.h"
#include "FaceTracking/FaceOverlay.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class IGraphicsDevice;
namespace RHI { struct Texture; }

namespace FaceTracking {

struct NetworkSourceOptions;
namespace FacePose { struct Calibration; }
struct CameraDevice;
class IImageCapture;

// The editor preview: one RGBA8 texture of a fixed size, uploaded in place at most
// 15 times a second, only while something asked for it this frame or the last.
inline constexpr uint32_t kPreviewWidth = 480, kPreviewHeight = 360;

struct PreviewStats
{
    std::string source;         // what the preview was last asked to show ("" = nothing asked)
    std::string shown;          // whose image the texture holds ("" = none yet)
    bool visible = false;       // asked this frame or the last
    uint64_t uploads = 0;       // QueueTextureUpload calls
    uint64_t queued = 0, replaced = 0, carriedOver = 0, dropped = 0, rejected = 0, fallback = 0;
    uint64_t images = 0;        // images taken from the source for it
};

// What the tracker made of the image the preview texture holds (the Face Tracking window draws it over the
// picture): FaceOverlay.h's fields, its points and crop mapped into the texture's pixels (kPreviewWidth x
// kPreviewHeight, the letterbox included); width / height stay the source image's. Not valid while the
// shown image had no tracker (none in the build, the models still loading) or the source is not an image one.
struct PreviewOverlay
{
    bool valid = false;
    std::string source;         // whose image it is (PreviewStats::shown)
    FaceOverlay face;
};

struct SourceStatus
{
    std::string name;
    std::string kind;   // "replay", "synthetic", "vmc", "ifacialmocap", "camera", "fakecamera", "video"
    std::string detail; // the take's path and length; a socket's address and counts
    uint64_t received = 0;    // frames that reached the history
    uint64_t refused = 0;     // pushes the ring refused plus frames the history refused
    size_t historyFrames = 0;
    // New every time a source is opened, under any name: a reader holding an older one is
    // reading a source that was closed and reopened.
    uint64_t epoch = 0;
    uint64_t newestSeq = 0;
    int64_t newestTimeUs = 0;
    int64_t clockUs = 0;      // the source's clock: simulation time, or the wall clock for live ones
    size_t extras = 0;        // extra channels it registered (a binding recompiles when this grows)
    bool live = false;        // runs on the wall clock (a network or camera source)
    std::string state;        // "running", or a capture's denied / busy / lost / ended / failed
    uint16_t port = 0;        // a network source's bound port (the one taken for port 0)
    std::string lastError;    // a live source's last receive error
    bool faceFound = false;
    float confidence = 0.0f;
    bool finished = false;
    bool recording = false;   // a take is being recorded from it
    uint64_t recordedFrames = 0;
    double recordedSeconds = 0.0; // the take's length so far, on the source's clock (0 while its time stands still)
};

// A source's calibration as Status reports it.
struct CalibrationStatus
{
    bool capturing = false;
    std::string mode;          // "neutral" or "range" while capturing
    double progress = 0.0;     // 0..1 of the capture's seconds, on the source's clock
    uint64_t generation = 0;   // bumped by every change; 0 = never calibrated
    bool hasNeutral = false, hasRange = false;
    uint64_t frames = 0;       // the found frames the last capture took
    // Why the last capture ended without a calibration: no face came, the source's time stood
    // still, the source closed ("" = it did not).
    std::string error;
};

// What StopRecording wrote.
struct RecordingResult
{
    std::string path;
    uint64_t frames = 0;
    double seconds = 0.0;     // the take's span: its first frame to its last
    bool truncated = false;   // it reached maxSeconds and stopped taking frames
};

class Service
{
public:
    static Service& Get();

    void Initialize();
    void Shutdown();

    // Once per frame, first in the animation chain, with the chain's dt (0 in Edit mode:
    // takes pause; live sources keep the wall clock).
    void Pump(float dt);

    bool Enabled() const { return m_enabled; }

    // Errors fill `error`. Names are unique; an existing name is refused.
    bool OpenReplay(const std::string& path, const std::string& name, bool loop, std::string& error);
    bool CreateSynthetic(const std::string& name, std::string& error);
    // A VMC / iFacialMocap listener (FaceSource.h's NetworkSourceOptions); false when the
    // socket cannot be bound (the error names the address and port).
    bool OpenNetwork(const NetworkSourceOptions& options, const std::string& name, std::string& error);
    // Image sources. A camera by its symbolic link (ListCameras' id); 0 / 0 / 0 = the
    // automatic format (ChooseCameraFormat). A camera that is denied, busy or lost is still
    // opened - its Status says which; false only for a source that cannot exist at all.
    bool OpenCamera(const std::string& deviceId, uint32_t width, uint32_t height, float fps, const std::string& name,
                    std::string& error);
    // Harness builds. video: a file whose frames the fake camera loops instead of its pattern; fps: its
    // rate (30; 1-1000); trackDelayMs: a sleep before each of its images is tracked (a slower CPU).
    bool OpenFakeCamera(const std::string& fault, const std::string& name, std::string& error, const std::string& video = {},
                        int fps = 30, int trackDelayMs = 0);
    bool OpenVideo(const std::string& path, const std::string& name, std::string& error);
    bool Close(const std::string& name);

    // The player's camera permission (the setting faceTracking.camera, default off).
    // While it is off ListCameras and OpenCamera are refused - nothing opens a camera, not
    // even to list it; cameras already open are closed. Fake cameras and videos are not
    // cameras. Thread-safe.
    void SetCameraAllowed(bool allowed);
    bool CameraAllowed() const;

    // The cameras Media Foundation lists, with their native formats; cached until refresh or
    // a device change (listing opens each device).
    bool ListCameras(std::vector<CameraDevice>& out, bool refresh, std::string& error);
    // WM_DEVICECHANGE (App forwards DevicesChanged): every source hears it; the camera list
    // is listed again on next ask.
    void OnDeviceChange();

    // ---- Recording ----
    // Records what `source` delivers - the frames its history takes, before any binding's
    // calibration, mirror or filter, so one take replays under any of them - to a take (.iface)
    // at `path`, timed from its first recorded frame; at most maxSeconds of it (and at most 240
    // frames a second). One frame an instant: while the source's time stands still (a synthetic
    // source in Edit mode or while the game is paused) the newest frame replaces the last one.
    // One recording per source. Closing the source, or Shutdown, stops it and writes the file.
    bool StartRecording(const std::string& source, const std::string& path, double maxSeconds, std::string& error);
    // Stops, and writes the take (atomically; its folder made when missing).
    bool StopRecording(const std::string& source, RecordingResult& out, std::string& error);

    // ---- Calibration ----
    // The performer's calibration, kept by source NAME (the same phone or camera opened again
    // keeps it): per ARKit channel the neutral face's value and the most it reaches, and the
    // neutral head. Every binding on the source calibrates its frames with it. A capture takes
    // the source's own found frames for `seconds` of its clock: neutral averages them (channels
    // and head), range keeps each channel's highest. A capture its source does not finish within
    // max(10, 3 x seconds + 5) s of the wall clock - no face came, or the source's time stood
    // still - gives up and says why (CalibrationStatus::error). Saved beside the user's settings
    // (UserDataDir/face_calibration.json) - never in an asset, never read or written under
    // -unattended (a test starts from no calibration and leaves none behind).
    bool StartCalibration(const std::string& source, bool range, double seconds, std::string& error);
    void CancelCalibration(const std::string& source);
    void ResetCalibration(const std::string& source);
    CalibrationStatus GetCalibrationStatus(const std::string& source) const;
    // The source's calibration and its generation; false (out untouched) when it has none.
    bool GetCalibration(const std::string& source, FacePose::Calibration& out, uint64_t& generation) const;

    // ---- The editor preview ----
    // App hands the device over once it exists; without it nothing is uploaded.
    void SetGraphicsDevice(IGraphicsDevice* gfx);
    // "Show this source's images": once per frame while visible (the editor panel, Lua).
    void RequestPreview(const std::string& source);
    // The texture the preview is uploaded to (valid once the first image arrived), and counts.
    bool PreviewTexture(RHI::Texture& out) const;
    PreviewStats GetPreviewStats() const;
    // The shown image's overlay (false while there is none: PreviewOverlay above).
    bool GetPreviewOverlay(PreviewOverlay& out) const;

    // A synthetic source's next frame. Its values are indexed by channel: the caller resolves
    // names with FindChannel(name, channel, /*addExtra=*/true) first. The frame's time is the
    // source clock of the Pump that drains it. Returns the frame's seq, 0 on error.
    uint64_t Push(const std::string& name, const FaceFrame& frame, std::string& error);
    // A channel index for `source`: ARKit names always resolve; extras resolve when the source
    // has them, or - addExtra on a synthetic source - get registered. -1 otherwise.
    int FindChannel(const std::string& source, std::string_view channel, bool addExtra);
    std::string ChannelName(const std::string& source, int channel) const;
    // A copy of the source's channel table (what a binding compiles its mapping against);
    // false when the source does not exist.
    bool Channels(const std::string& source, FaceSourceChannels& out) const;

    std::vector<SourceStatus> Sources() const;
    bool Status(const std::string& name, SourceStatus& out) const;
    bool Newest(const std::string& name, FaceFrame& out) const;
    // The source's history at time t of its own clock (FaceHistory::Sample).
    SampleState Sample(const std::string& name, int64_t tUs, FaceFrame& out) const;
    // The source's history at its current clock.
    SampleState SampleNow(const std::string& name, FaceFrame& out) const;
    // Every frame after cursor.lastSeq, oldest first; false when the source does not exist.
    bool ReadNew(const std::string& name, FaceCursor& cursor, std::vector<FaceFrame>& out) const;

private:
    struct Impl;

    Service();
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    // OpenCamera / OpenFakeCamera / OpenVideo: the capture behind a CameraFaceSource. trackDelayMs: the
    // harness's slower tracker for this source (CameraSourceOptions::trackDelayMs).
    bool OpenCapture(std::unique_ptr<IImageCapture> capture, const char* kind, const std::string& name, std::string& error,
                     int trackDelayMs = 0);

    std::unique_ptr<Impl> m;
    bool m_enabled = true;
};

} // namespace FaceTracking
