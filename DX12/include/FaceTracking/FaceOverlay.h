#pragma once
// What the built-in tracker made of one image, kept for the editor preview's overlay (the Face Tracking
// window draws it over the picture): the image it belongs to (its seq and size), whether a face was found
// and how (detected anew, or tracked from the last image's landmarks), the landmark model's crop, the 478
// landmarks, the head's yaw / pitch / roll, the 52 blendshape scores (raw, before any character's
// calibration) and the stage times. An image source with a tracker keeps the last few
// (IFaceSource::OverlayFor); Service::GetPreviewOverlay hands out the one of the image the preview
// texture holds, its points mapped into the texture's pixels.

#include <array>
#include <cstdint>

namespace FaceTracking {

// Tracker::kLandmarks and Tracker::kBlendshapes (FaceTrackerMath.h, static_asserted where it is filled):
// spelled here so this header stays light.
inline constexpr int kOverlayLandmarks = 478;
inline constexpr int kOverlayBlendshapes = 52;

struct FaceOverlay
{
    uint64_t imageSeq = 0;          // FaceImage::seq of the image
    uint32_t width = 0, height = 0; // that image's size
    bool found = false;
    bool detected = false;          // the detector ran on this image (no face yet, or it was lost)
    float presence = 0.0f;          // the landmark model's face presence, 0..1
    float roi[4][2] = {};           // the landmark crop's corners (the tensor's corners in the image), when found
    std::array<float, kOverlayLandmarks * 2> points{}; // x, y of each landmark (image pixels), when found
    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f; // FaceMath::DecomposeHead of the head, when found
    std::array<float, kOverlayBlendshapes> blendshapes{}; // Tracker::kBlendshapeNames' order, when found
    float ms[4] = {};               // detector, landmarks, blendshapes, the whole image
};

} // namespace FaceTracking
