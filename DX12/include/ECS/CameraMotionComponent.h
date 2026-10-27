#pragma once

// CameraMotionComponent - plays a CAMERA-animation track (cameraClips[0] of a
// .ianim, e.g. from an MMD camera VMD) by driving a dedicated high-priority VCam on
// the camera stack.
// CameraMotionTickSystem (CameraStackSystem.cpp) acquires the .ianim, samples the
// dense per-frame CameraAnimKey stream, writes CameraPoseComponent +
// VirtualCameraComponent on this entity and PushVCam's it, so the stack composites
// and blends it like any other virtual camera (TAA / SSR history clears on cuts via
// requestHardCut).
// Clip poses are in the source's world space (subject at the clip origin, the
// common MMD case). Each sampled pose is composed onto THIS entity's
// GlobalTransform, so the entity's LocalTransform is the "stage root": move /
// rotate / scale it to reposition the whole shot. translationScale converts clip
// translation units first. Identity values play the raw clip verbatim.

#include "Resource/SystemHandles.h"   // Resource::AnimHandle
#include <string>
#include <cstdint>

struct CameraMotionComponent
{
    // ---- Authoring (serialized) ----
    std::string motionPath;          // path to a camera .ianim
    float       speed      = 1.0f;   // playback rate multiplier
    float       translationScale = 1.0f; // clip translation units -> world units
    bool        looping    = false;  // dance cameras usually play once
    bool        playOnLoad = true;   // auto-start when the sim begins (PIE / Game)
    int         priority   = 1000;   // VCam stack priority (beats gameplay cams)

    // ---- Runtime (not serialized) ----
    Resource::AnimHandle handle{};       // resolved from motionPath
    bool     handleRequested = false;
    bool     playing         = false;    // currently driving the VCam
    bool     started         = false;    // playOnLoad one-shot guard
    bool     paused          = false;
    bool     pushed          = false;    // VCam pushed onto the stack
    bool     stopRequested   = false;    // inspector / Lua Stop -> the system pops the VCam
    bool     externalTime    = false;    // a MotionDirector / Sequencer owns the clock
    float    time            = 0.0f;     // playback position (seconds)
    float    prevTime        = -1.0f;    // for cut-crossing / history reset
    uint32_t channelId       = 0;        // 0 -> resolved to Camera::kMainChannel
};
