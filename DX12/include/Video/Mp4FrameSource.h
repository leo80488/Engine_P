#pragma once

// Mp4FrameSource - FFmpeg-backed Video::IDecodedFrameSource.
//
// Built on libavformat (demux) + libavcodec (decode) + libswscale (NV12 conversion when the codec
// does not emit NV12). Each Tick() takes a VideoComponent, advances the decoder until the next
// frame's PTS exceeds the component clock, then queues the frame into VideoComponent::dpb[0]:
// software decode through IGraphicsDevice::QueueTextureUpload (the NV12 planes), D3D12VA through
// GraphicsDX12::QueueNativeTextureCopy (a GPU copy that waits on the decoder's fence); LatestWins,
// no CPU wait. -video_syncupload selects the synchronous UpdateTexture /
// CopyD3D12ResourceToTexture path; -video_swdecode disables D3D12VA.
//
// Part of the Video module, whose project defines WITH_FFMPEG (FFmpeg's headers, import libraries
// and DLLs are vendored in external/ffmpeg); without the define Video::OpenMp4 returns nullptr and
// logs that FFmpeg is missing.

#include "ECS/VideoComponent.h"

#include <memory>
#include <string>

class IGraphicsDevice;

namespace Video
{
    // Open an mp4 / mkv / mov file and return a decoded-frame source; nullptr when WITH_FFMPEG is not
    // defined, the file is missing, or the first video stream cannot be opened. On success the caller
    // stores it in VideoComponent::decodedFrameSource; VideoSystem takes the decoded path whenever that
    // pointer is set.
    //
    // The source tries D3D12VA HARDWARE decode first (FFmpeg's AV_HWDEVICE_TYPE_D3D12VA hwaccel on the
    // engine's ID3D12Device); on any setup failure it falls back to software decode + libswscale NV12
    // conversion.
    std::shared_ptr<IDecodedFrameSource> OpenMp4(IGraphicsDevice& gfx,
                                                  const std::string& path);
} // namespace Video
