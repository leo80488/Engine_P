#pragma once

// Media::IVideoReader - a video file's first video stream, decoded in order on the CPU into NV12
// (the Media module, FFmpeg). Software decode only, and an exact, machine-independent
// conversion (SWS_POINT) when the decoder emits anything but NV12, so two runs see the same
// pixels. For a GPU texture instead, Video/Mp4FrameSource.h (the Video module).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace Media
{
    // Where a decoded frame goes. Width and height are even.
    class INv12Sink
    {
    public:
        virtual ~INv12Sink() = default;
        // A frame the decoder already holds as NV12: copy its planes.
        virtual void CopyFrom(uint32_t width, uint32_t height, const uint8_t* y, size_t yPitch,
                              const uint8_t* uv, size_t uvPitch) = 0;
        // Tight planes (pitch = width) for a converted frame, written after this returns.
        virtual void Resize(uint32_t width, uint32_t height, uint8_t*& y, uint8_t*& uv) = 0;
    };

    class IVideoReader
    {
    public:
        virtual ~IVideoReader() = default;
        // The next frame in decode order into @p sink, @p ptsUs its presentation time in us from
        // the stream's first frame; false at the end of the stream (AtEnd) or on a decode error.
        virtual bool ReadFrame(INv12Sink& sink, int64_t& ptsUs) = 0;
        virtual bool AtEnd() const = 0;
        virtual uint32_t Width() const = 0;
        virtual uint32_t Height() const = 0;
    };

    // Open @p path (a file on disk); @p name is what an error calls it. Null with @p error set
    // when the file, its stream info, a video stream or the decoder cannot be opened.
    std::unique_ptr<IVideoReader> OpenVideoReader(const std::string& path, const std::string& name,
                                                  std::string& error);
}
