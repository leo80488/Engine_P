#pragma once
// One canonical face sample. Every source - a camera tracker, an iPhone app, a recorded take,
// a script - produces this, already in the canonical face space of FaceMath.h. Plain data:
// the SPSC ring and the source history copy it by value. Landmarks never travel in it (an
// image source keeps the last few apart for the editor preview: FaceOverlay.h).

#include "FaceTracking/FaceChannels.h"

#include <DirectXMath.h>
#include <cstdint>
#include <type_traits>

namespace FaceTracking {

enum FaceFrameFlags : uint32_t
{
    kFaceHasBlend   = 1u << 0, // values[] / present carry blendshapes
    kFaceHasHead    = 1u << 1, // headRot was measured (identity otherwise)
    kFaceHasHeadPos = 1u << 2, // headPos was measured
    kFaceHasEyes    = 1u << 3, // eyeYawPitch was measured (FaceTrackingSystem derives it from eyeLook* otherwise)
    kFaceMirrored   = 1u << 4, // the source mirrored the face itself (an app option)
};

struct FaceFrame
{
    uint64_t seq = 0;          // per source, from 1, strictly increasing
    int64_t captureQpcUs = 0;  // QPC (microseconds) when the frame reached the engine
    int64_t sourceTimeUs = 0;  // the source's clock: simulation time for takes and scripts, PTS for video
    float values[kMaxFaceChannels] = {};
    uint64_t present[2] = {};  // bit i: values[i] was sent
    DirectX::XMFLOAT4 headRot = {0.0f, 0.0f, 0.0f, 1.0f}; // canonical face space, xyzw
    DirectX::XMFLOAT3 headPos = {0.0f, 0.0f, 0.0f};       // metres, kFaceHasHeadPos
    float eyeYawPitch[2][2] = {};                          // [left, right][yaw, pitch], radians, kFaceHasEyes
    float confidence = 0.0f;   // 0..1
    uint32_t flags = 0;        // FaceFrameFlags
    uint8_t faceFound = 0;     // 0 / 1

    bool Has(int ch) const
    {
        return ch >= 0 && ch < kMaxFaceChannels && ((present[ch >> 6] >> (ch & 63)) & 1ull) != 0;
    }
    void Set(int ch, float v)
    {
        if (ch < 0 || ch >= kMaxFaceChannels) return;
        values[ch] = v;
        present[ch >> 6] |= 1ull << (ch & 63);
        flags |= kFaceHasBlend;
    }
    // Eye side: 0 = the subject's left, 1 = their right.
    void SetEye(int side, float yaw, float pitch)
    {
        if (side < 0 || side > 1) return;
        eyeYawPitch[side][0] = yaw;
        eyeYawPitch[side][1] = pitch;
        flags |= kFaceHasEyes;
    }
    int PresentCount() const
    {
        int n = 0;
        for (uint64_t word : present)
            for (uint64_t w = word; w; w &= w - 1) ++n;
        return n;
    }
};
static_assert(std::is_trivially_copyable_v<FaceFrame>, "the ring and the history copy FaceFrames by value");

} // namespace FaceTracking
