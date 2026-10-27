#pragma once

// The codec and profile a VideoComponent names: stream metadata, saved as numbers on the scene's
// Video line (codec=, profile=). Decoding is the Video module's frame sources' (FFmpeg software or
// D3D12VA through Mp4FrameSource); these configure no decoder in the engine.

#include <cstdint>

namespace RHI
{
    enum class VideoCodec : uint8_t
    {
        H264,   // AVC - MPEG-4 Part 10
        H265,   // HEVC - MPEG-H Part 2
    };

    // Baseline / Main / High for H.264, Main / Main10 for H.265. The values are saved: never renumber.
    enum class VideoProfile : uint8_t
    {
        H264_Baseline = 0,
        H264_Main     = 1,
        H264_High     = 2,
        H265_Main     = 10,  // 8-bit
        H265_Main10   = 11,  // 10-bit HDR
    };
}
