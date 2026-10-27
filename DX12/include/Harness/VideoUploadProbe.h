#pragma once
// Harness/VideoUploadProbe.h - -video_probe, the video upload paths' probe.
// Video::OpenMp4 has no caller in the engine, so Mp4FrameSource's two upload paths -
// software decode (QueueTextureUpload, the NV12 planes) and D3D12VA decode
// (QueueNativeTextureCopy, a GPU copy waiting on the decoder's fence) - run only here.
// From frame kStartFrame it encodes a short H.264 clip (libavcodec; 256x144 or
// -video_probe_size=WxH, kFrames frames, the first of libx264 / libopenh264 / h264_mf /
// mpeg4 the build has) into the diagnostics dir, decodes it once on the CPU for the
// reference (its LAST frame as NV12), and plays it through the real path: an entity with
// a VideoComponent whose decodedFrameSource is Video::OpenMp4 (D3D12VA unless
// -video_swdecode), ticked by VideoSystem. kSettle frames after the last decode the
// probe's reader (the StreamUploadProbe feature's graph pass) copies vc.dpb[0]'s two
// planes into a READBACK buffer; once that slot retires both planes must equal the
// reference byte for byte (H.264 decoding is bit-exact, so the GPU decoder's NV12 is the
// CPU decoder's). Flushes are counted from the first decoded frame to the last: the
// streaming paths make none (-video_syncupload, the serialising path, one a frame).
// -video_probe_loop: loop the clip and never check (a soak).
// -video_probe_size=WxH (even, 16..4096): another size. 1920x1080 (not a multiple of 16)
// works: FFmpeg's D3D12VA surface is the picture's size, not the macroblock grid's (no
// 1088), so the hardware path's CopyResource fits; in software decode each frame moves
// 3 MB through the stream ring.
// The line: [VIDEO_PROBE] mode= encoder= frames= decoded= yMatch= uvMatch= playFlushes=
// playCopies= playBytes= checked= loop= failed= at= - at shutdown (and once when the
// check ran). playCopies / playBytes: the stream uploader's recorded copies and queued
// CPU bytes from the first decoded frame to the read - which path the frames took
// (D3D12VA: a copy each and no bytes; software: two planes of bytes).

#include "ECS/ISystem.h"
#include "Graphics/GraphicsStruct.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class IGraphicsDevice;

namespace Harness
{
    class VideoUploadProbe
    {
    public:
        static VideoUploadProbe& Get();
        // -video_probe (harness builds with WITH_FFMPEG only).
        static bool Requested();

        void GameTick(World& world, IGraphicsDevice& gfx, uint64_t frame);
        // The render half, from the StreamUploadProbe feature's pass.
        void RecordReadback(IGraphicsDevice& gfx, RHI::CommandList cl);
        void Report(const char* when) const;

    private:
        static constexpr uint32_t kFrames     = 36;
        static constexpr uint32_t kFps        = 30;
        static constexpr uint64_t kStartFrame = 120;   // past the scene's own uploads
        static constexpr uint64_t kSettle     = 8;     // frames after the last decode before the read
        static constexpr uint32_t kSlots      = 3;

        enum class Phase { Idle, Playing, Settling, Reading, Done, Failed };

        bool Start(World& world, IGraphicsDevice& gfx);
        void Check(IGraphicsDevice& gfx);

        Phase         m_phase = Phase::Idle;
        // The clip's size: 256x144 unless -video_probe_size.
        uint32_t      m_width = 256, m_height = 144;
        uint32_t      m_entity = 0;            // the Entity id (an Entity is a uint32_t)
        std::string   m_encoder, m_clip;
        bool          m_hw = false;
        bool          m_loop = false;
        std::vector<uint8_t> m_expectY, m_expectUV;   // the reference last frame, tight rows
        RHI::GPUBuffer m_read;
        uint32_t      m_rowPitch = 0, m_rows = 0;
        uint64_t      m_readBytes = 0;
        RHI::Texture  m_readSrc{};                   // vc.dpb[0], handed to the render half
        std::atomic<uint64_t> m_readFrame{ UINT64_MAX };   // the frame whose reader copies it
        std::atomic<bool>     m_readRecorded{ false };
        uint64_t      m_settleUntil = 0, m_checkAt = 0;
        uint64_t      m_flushAtFirst = 0, m_playFlushes = 0;
        uint64_t      m_copiesAtFirst = 0, m_bytesAtFirst = 0, m_playCopies = 0, m_playBytes = 0;
        uint32_t      m_decoded = 0;
        bool          m_firstSeen = false;
        int           m_yMatch = -1, m_uvMatch = -1;
        uint32_t      m_checked = 0;
    };

    class VideoUploadProbeSystem : public SystemInPhase<TickPhase::PreRender>
    {
    public:
        explicit VideoUploadProbeSystem(IGraphicsDevice& gfx) : m_gfx(gfx) {}
        ~VideoUploadProbeSystem() override;
        const char* GetName() const override { return "VideoUploadProbeSystem"; }
        SystemRunMask RunMask() const override { return SystemRunMask::Always; }
        void Update(World& world, const FrameContext& ctx) override;

    private:
        IGraphicsDevice& m_gfx;
    };
}
