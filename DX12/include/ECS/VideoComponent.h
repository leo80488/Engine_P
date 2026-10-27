#pragma once

// Per-entity video playback state. The caller's frame source owns the decode (demuxing and decoding,
// FFmpeg / Media Foundation: see "Frame source" below) and writes NV12 frames into dpb[];
// dpb[currentDpbSlot] is the texture VideoPass / VideoQuadPass sample. Destroying the component does
// not free its GPU resources: call Video::ReleaseGPUResources first.

#include "Graphics/GraphicsStruct.h"   // RHI::Texture
#include "Graphics/VideoCodec.h"

#include <cstdint>
#include <memory>
#include <vector>

class IGraphicsDevice;
struct VideoComponent;   // forward decl for Video::IDecodedFrameSource::Tick

namespace Video
{
    // Decoded-frame source: the implementation owns the whole decode (FFmpeg / Media Foundation) and
    // puts finished NV12 frames into the component's dpb[] each tick. Mp4FrameSource (the Video
    // module) implements it.
    class IDecodedFrameSource
    {
    public:
        virtual ~IDecodedFrameSource() = default;

        // Called by VideoSystem every frame while Playing, after it advanced vc.clockSeconds. Decode up
        // to vc.clockSeconds, create vc.dpb[] (one NV12 texture of the frame size) on the first frame,
        // put the frame in vc.dpb[0] and set vc.currentDpbSlot = 0. Return false on a hard failure
        // (file gone, decoder reset failed); VideoSystem then stops the component.
        virtual bool Tick(IGraphicsDevice& gfx, VideoComponent& vc,
                          double deltaSeconds) = 0;

        // Rewind / keyframe seek (to the nearest keyframe at or before the time) and the stream duration
        // in seconds (negative = unknown / live; only the inspector's progress bar reads it).
        // VideoSystem never calls Restart: the source handles end of stream itself, honouring vc.loop.
        virtual void   Restart() {}
        virtual void   Seek(double /*timeSeconds*/) {}
        virtual double GetDurationSeconds() const { return -1.0; }
    };
} // namespace Video

enum class VideoPlaybackState : uint8_t
{
    Stopped, // No work: clock frozen, no decode.
    Playing, // Active decode driven by deltaTime.
    Paused,  // Clock frozen; the decoder keeps the most recent output frame.
};

struct VideoComponent
{
    // ---- Stream config (set before VideoSystem first ticks this entity) ----
    // Saved with the scene (the Video line). codec, profile, bitDepth and dpbSlotCount describe the
    // stream for the author; the decode is the frame source's, which reads its own file.
    RHI::VideoCodec   codec       = RHI::VideoCodec::H264;
    RHI::VideoProfile profile     = RHI::VideoProfile::H264_High;
    uint32_t          width       = 1920;
    uint32_t          height      = 1080;
    uint32_t          bitDepth    = 8;
    // DPB depth of the stream: H.264 Level 4.1 / 1080p typically needs 4-5.
    uint32_t          dpbSlotCount = 8;

    // ---- Playback ----
    VideoPlaybackState state      = VideoPlaybackState::Stopped;
    bool               loop       = false;
    // Playback rate multiplier (1 = realtime, 2 = double speed); VideoSystem clamps it to >= 0.
    float              playRate   = 1.0f;

    // ---- Frame source ----
    // The caller decodes (FFmpeg software or D3D12VA via Mp4FrameSource, Media Foundation) and Tick()
    // uploads or GPU-copies NV12 into dpb[]. Null: VideoSystem leaves dpb[] alone (LoadTestPattern,
    // manual upload).
    std::shared_ptr<Video::IDecodedFrameSource> decodedFrameSource;

    // ---- Surface mode ----
    // false: VideoPass composites into the HDR target in screen space over rectUMin..rectVMax.
    // true:  VideoQuadPass draws a worldWidth x worldHeight quad at the entity's GlobalTransform
    //        (identity if absent), facing +Z in local space, centred on the origin, depth-tested
    //        against the scene.
    // Only the first screen-space component that is not Stopped and has a decoded frame is drawn.
    bool             worldSpace  = false;
    float            worldWidth  = 1.0f;
    float            worldHeight = 1.0f;

    // ---- Screen-space surface (VideoPass; ignored when worldSpace) ----
    // Sub-rectangle of the HDR target the video covers, 0..1 from top-left to bottom-right;
    // the default is fullscreen.
    float            rectUMin   = 0.0f;
    float            rectVMin   = 0.0f;
    float            rectUMax   = 1.0f;
    float            rectVMax   = 1.0f;

    // ---- Surface (both passes) ----
    // renderAlpha multiplies the composited RGB (1 = opaque, < 1 lets the scene show through).
    // colorSpace picks the YUV -> RGB matrix: 0 = BT.709 (HD, default), 1 = BT.601 (SD).
    float            renderAlpha = 1.0f;
    uint32_t         colorSpace  = 0;

    // ---- Runtime state (VideoSystem and the frame source own it; author code must not write it) ----
    std::vector<RHI::Texture> dpb;          // the NV12 frame texture(s): one, the frame source's
    // dpb[] index of the newest decoded frame, the one the renderer samples; ~0u until the first
    // decode.
    uint32_t                  currentDpbSlot = ~0u;
    // Stream clock in seconds, advanced by VideoSystem by deltaTime x playRate while Playing.
    double                    clockSeconds   = 0.0;
    // PTS of the last decoded frame; -1 = none yet.
    double                    lastDecodedPts = -1.0;
    // Frames decoded so far (the inspector shows it).
    uint64_t                  framesDecoded  = 0;
};
