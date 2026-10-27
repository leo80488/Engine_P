#pragma once

// The Video inspector's field descriptor (the Video module's editor half; editor-only, like
// Reflection/ComponentReflection.h). Play / Pause / Stop / Restart and the seek slider are the
// inspector's postDraw (VideoEditorModule): they call the Video::* helpers.

#include "Reflection/Reflect.h"
#include "Reflection/ReflectionEditor.h"

#include "ECS/VideoComponent.h"

constexpr Reflect::EnumOption kVideoStateOptions[] = {
    { (int)VideoPlaybackState::Stopped, "Stopped" },
    { (int)VideoPlaybackState::Playing, "Playing" },
    { (int)VideoPlaybackState::Paused,  "Paused"  },
};

constexpr Reflect::EnumOption kVideoCodecOptions[] = {
    { (int)RHI::VideoCodec::H264, "H.264" },
    { (int)RHI::VideoCodec::H265, "H.265" },
};

constexpr Reflect::EnumOption kVideoColorSpaceOptions[] = {
    { 0, "BT.709 (HD)" },
    { 1, "BT.601 (SD)" },
};

REFLECT_BEGIN(VideoComponent)
    REFLECT_HEADER("Playback")
    REFLECT_ENUM  (state,        "State",           kVideoStateOptions)
    REFLECT_DRAG(playRate,     "Speed",            0.0f, 4.0f)
    REFLECT_BOOL  (loop,         "Loop")
    REFLECT_HEADER("Surface")
    REFLECT_BOOL  (worldSpace,   "World-Space Quad")
    REFLECT_FLOAT (worldWidth,   "World Width (m)",  0.01f, 100.f)
    REFLECT_FLOAT (worldHeight,  "World Height (m)", 0.01f, 100.f)
    REFLECT_DRAG(rectUMin,     "Screen U Min",     0.f, 1.f)
    REFLECT_DRAG(rectVMin,     "Screen V Min",     0.f, 1.f)
    REFLECT_DRAG(rectUMax,     "Screen U Max",     0.f, 1.f)
    REFLECT_DRAG(rectVMax,     "Screen V Max",     0.f, 1.f)
    REFLECT_DRAG(renderAlpha,  "Alpha",            0.f, 1.f)
    REFLECT_ENUM  (colorSpace,   "Color Space",      kVideoColorSpaceOptions)
    REFLECT_HEADER("Stream Config (saved with the scene; the frame source decodes)")
    REFLECT_ENUM  (codec,        "Codec",            kVideoCodecOptions)
    REFLECT_UINT    (width,    "Max Width",        16u, 7680u, Speed(Reflect::BoundedIntDragSpeed(16, 7680)))
    REFLECT_UINT    (height,   "Max Height",       16u, 4320u, Speed(Reflect::BoundedIntDragSpeed(16, 4320)))
REFLECT_END()
