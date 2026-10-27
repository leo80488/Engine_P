#pragma once
// Where a camera source's images come from: a Media Foundation camera, the harness's fake camera,
// or a video file. Two shapes:
//   push - a camera: its own thread delivers images as they come; Next() hands over the newest
//          one (latest wins: an older unread one is dropped - FrameMailbox)
//   pull - a video file: Next() decodes the next frame in order, only once its time has come
//          (no dropping, reproducible under a fixed time step)
// Either way a CameraFaceSource's worker thread calls Next(); nothing here touches the engine.

#include "FaceTracking/FaceImage.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace FaceTracking {

// Capture states, each with its own UI text and Lua lastError.
enum class CaptureState : uint8_t
{
    Opening,
    Running,
    Denied,  // the Windows privacy setting refuses desktop apps the camera
    Busy,    // another program holds it / not enough bandwidth: retried every 3 s
    Lost,    // unplugged: reopened when DevicesChanged brings the same device back
    Ended,   // a video file played to its end
    Failed,  // nothing usable (no format, no decoder)
    Stopped,
};

inline const char* CaptureStateName(CaptureState s)
{
    switch (s)
    {
    case CaptureState::Opening: return "opening";
    case CaptureState::Running: return "running";
    case CaptureState::Denied: return "denied";
    case CaptureState::Busy: return "busy";
    case CaptureState::Lost: return "lost";
    case CaptureState::Ended: return "ended";
    case CaptureState::Failed: return "failed";
    case CaptureState::Stopped: return "stopped";
    }
    return "?";
}

// FourCCs as Media Foundation spells its video subtypes' first word.
inline constexpr uint32_t FourCC(char a, char b, char c, char d)
{
    return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}
inline constexpr uint32_t kFourccNV12 = FourCC('N', 'V', '1', '2');
inline constexpr uint32_t kFourccYUY2 = FourCC('Y', 'U', 'Y', '2');
inline constexpr uint32_t kFourccMJPG = FourCC('M', 'J', 'P', 'G');
inline constexpr uint32_t kFourccRGB24 = 20; // MFVideoFormat_RGB24's first word is D3DFMT_R8G8B8

struct CameraFormat
{
    uint32_t fourcc = 0;
    uint32_t width = 0, height = 0;
    float fps = 0.0f;
};

struct CameraDevice
{
    std::string id;   // the symbolic link: stable across replugging, unlike an index
    std::string name; // the friendly name
    std::vector<CameraFormat> formats;
};

// Picks a format: native NV12, then YUY2, then MJPG (decoded by MF), then RGB24; at least
// 640x480 when the camera has it, the nearest to the asked size; 60 fps where a native NV12 / YUY2
// mode of that size has it, else 30 (or the nearest the camera has). A size asked for explicitly
// outweighs the format family (720p asked is 720p MJPG over 480p NV12); the automatic one
// (0 x 0 = 640 x 480) does not. wantFps 0 = the automatic rate. Returns the index of the chosen
// format, -1 when none can give NV12.
inline int ChooseCameraFormat(const std::vector<CameraFormat>& f, uint32_t wantW, uint32_t wantH, float wantFps)
{
    if (f.empty()) return -1;
    const bool explicitSize = wantW != 0 && wantH != 0;
    if (!explicitSize) { wantW = 640; wantH = 480; }
    auto rank = [](uint32_t cc) {
        if (cc == kFourccNV12) return 0;
        if (cc == kFourccYUY2) return 1;
        if (cc == kFourccMJPG) return 2;
        if (cc == kFourccRGB24) return 3;
        return 4;
    };
    // The automatic fps: 60 when an uncompressed mode >= 640x480 offers it.
    float target = wantFps;
    if (!(target > 0.0f))
    {
        target = 30.0f;
        for (const CameraFormat& c : f)
            if (rank(c.fourcc) <= 1 && c.width >= 640 && c.height >= 480 && c.fps >= 59.0f) target = 60.0f;
    }
    int best = -1;
    double bestScore = 1e300;
    for (size_t i = 0; i < f.size(); ++i)
    {
        const CameraFormat& c = f[i];
        if (rank(c.fourcc) == 4) continue;
        double score = rank(c.fourcc) * 1e6;
        if (!explicitSize && (c.width < 640 || c.height < 480)) score += 5e6; // too small to find a distant face
        score += std::abs(double(c.width) * c.height - double(wantW) * wantH) / 1000.0 * (explicitSize ? 1e4 : 1.0);
        if (c.fps + 0.5f < target) score += 2e6 + (target - c.fps) * 1e3; // too slow
        else score += (c.fps - target) * 10.0;                            // faster than needed costs a little
        if (score < bestScore)
        {
            bestScore = score;
            best = static_cast<int>(i);
        }
    }
    return best;
}

class IImageCapture
{
public:
    virtual ~IImageCapture() = default;

    // Opens the device / file and (push) starts delivering. False: State() and LastError()
    // say why - a Denied or Busy camera is not a failure to open the source, it is a state.
    virtual bool Start(std::string& error) = 0;
    virtual void Stop() = 0;

    virtual bool Pull() const = 0;
    // push: the newest image since the last call, into `out`; false when none arrived.
    // pull: the next frame whose time is <= untilUs, decoded now; false when it is not due
    // yet or the file has ended.
    virtual bool Next(int64_t untilUs, FaceImage& out) = 0;

    virtual CaptureState State() const = 0;
    virtual std::string LastError() const = 0;
    virtual std::string Describe() const = 0;

    // WM_DEVICECHANGE (App forwards DevicesChanged): a Lost camera whose device is back reopens.
    virtual void OnDeviceChange() {}
};

// Media Foundation (src/FaceTracking/Capture/MfCamera.cpp).
bool EnumerateCameras(std::vector<CameraDevice>& out, std::string& error);
std::unique_ptr<IImageCapture> MakeMfCamera(const std::string& deviceId, uint32_t width, uint32_t height, float fps);
// The harness's synthetic camera: 640x480 NV12 at `fps` (30; 1-1000) on the wall clock; `fault`
// "" / "denied" / "busy" / "lost" drives the matching state; `video` (a file) replaces the pattern
// with its frames, decoded once at Start and looped - a live source with a face. A rate above what
// the tracker keeps up with models a camera faster than its tracker. Null without the harness.
std::unique_ptr<IImageCapture> MakeFakeCamera(const std::string& fault, const std::string& video = {}, int fps = 30);
// A video file, software-decoded to NV12 in order. Null without FFmpeg.
std::unique_ptr<IImageCapture> MakeVideoFileCapture(const std::string& path);

} // namespace FaceTracking
