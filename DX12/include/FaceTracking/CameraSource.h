#pragma once
// Image sources as face sources: a camera, the harness's fake camera, or a video file. Each image
// goes through the built-in tracker (FaceTracker.h; made at the first image) into a FaceFrame for the
// source's ring - found or not - and is kept for the editor preview. A camera's images are taken, the
// newest one at a time, on the source's worker thread ("FaceTrack.Infer"); a video's, every one in
// order, in Produce on the Pump thread (the frames a Pump sees depend only on the clock). Without a
// tracker (ONNX Runtime not in the build or not beside the exe, a model missing) the images still
// reach the preview, no frame is made, and the source's detail says why.

#include "FaceTracking/FaceSource.h"
#include "FaceTracking/FaceTracker.h"
#include "FaceTracking/ImageCapture.h"

#include <memory>
#include <string>

namespace FaceTracking {

struct CameraSourceOptions
{
    // A "[FACETRACK] image" log line per image (seq, pts, hash): -facetrack_frame_log; the video source's
    // reproducibility test reads it.
    bool frameLog = false;
    // The tracker's settings (-facetrack_threads).
    FaceTrackerOptions tracker;
    // Harness builds: every tracked image as a JSON line {frame, found, presence, landmarks, blendshapes,
    // ypr} here (-facetrack_tracker_dump; tools/face_golden.py compare reads it). Empty: none.
    std::string trackerDump;
    // Harness builds: a sleep before every image is tracked - a slower CPU's tracker (OpenFakeCamera's
    // trackDelayMs; face_track_fault: a camera faster than its tracker still closes at once).
    int trackDelayMs = 0;
};

// kind: "camera" / "fakecamera" (live: the wall clock, frames timed by their arrival) or "video" (in
// order: the simulation clock, frames timed by their PTS).
std::unique_ptr<IFaceSource> MakeCameraSource(std::unique_ptr<IImageCapture> capture, const char* kind, const CameraSourceOptions& options);

} // namespace FaceTracking
