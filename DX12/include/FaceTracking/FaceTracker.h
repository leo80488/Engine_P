#pragma once
// The built-in face tracker: an image source's NV12 images -> faces. MediaPipe Face Landmarker's three
// models (engine/models/face/, converted by tools/face_models.py) on ONNX Runtime's CPU execution provider:
// the detector while there is no face (or it was lost), then the landmarks inside the region the last ones
// give (tracking), MediaPipe's landmark smoothing, the 52 blendshape scores from 146 landmarks, and the
// head's turn fitted from the canonical face.
// FaceTrackerMath.h holds the math; tools/face_golden.py holds the result to Python MediaPipe's.
//
// ONNX Runtime is loaded when the first tracker is made, by its full path next to the exe (never a copy
// in System32), and never linked: a build without its headers (ENGINE_WITH_ONNXRUNTIME=0) or a machine
// without the DLL gets no tracker, and MakeFaceTracker says why - the image sources still run.
// The three models load ONCE per process into sessions every tracker shares (ONNX Runtime's Run is safe
// from several threads), so opening a source again costs nothing; ReleaseFaceTrackerModels frees them.
// A tracker - its region of interest, its smoothing - is used from one thread at a time (its source's
// worker, or the Pump for a video).

#include "FaceTracking/FaceImage.h"
#include "FaceTracking/FaceTrackerMath.h"

#include <DirectXMath.h>

#include <memory>
#include <string>

namespace FaceTracking {

struct FaceTrackerOptions
{
    int threads = 2;           // ONNX Runtime's intra-op threads, not spinning between runs
    float minDetection = 0.5f; // the detector's score that takes a face
    float minPresence = 0.5f;  // the landmark model's presence that keeps it (below: detect on the next image)
    bool smooth = true;        // MediaPipe's landmark smoothing (its VIDEO mode)
};

struct FaceTrackerResult
{
    bool found = false;
    bool detected = false;                         // the detector ran for this image
    float presence = 0.0f;                         // the landmark model's face presence, 0..1
    float detectionScore = 0.0f;                   // when the detector ran and took a face
    float blendshapes[Tracker::kBlendshapes] = {}; // in Tracker::kBlendshapeNames' order
    float landmarks[Tracker::kLandmarks * 3] = {}; // image pixels (x, y, z), smoothed
    DirectX::XMFLOAT4 head = {0.0f, 0.0f, 0.0f, 1.0f}; // FaceMath's frame (the subject's)
    float ms[4] = {};                              // detector, landmarks, blendshapes, the whole image
    Tracker::Roi roi;                              // the landmark model's crop of this image, when found
};

class IFaceTracker
{
public:
    virtual ~IFaceTracker() = default;
    // One image at tUs (its source's time, rising). False only when inference failed (LastError).
    virtual bool Track(const FaceImage& image, int64_t tUs, FaceTrackerResult& out) = 0;
    virtual void Reset() = 0; // forget the face: the next image runs the detector
    virtual std::string Describe() const = 0;
    virtual std::string LastError() const = 0;
};

// A tracker on the shared models. wait = false (a camera's worker): while the models are still loading on
// their own thread it answers null with pending = true - skip this image, ask again with the next; wait =
// true (a video's Pump, every frame tracked): it waits for them. Null with pending = false, and why, when
// ONNX Runtime is not in this build or not beside the exe, or a model is missing (not retried).
std::unique_ptr<IFaceTracker> MakeFaceTracker(const FaceTrackerOptions& options, bool wait, bool& pending, std::string& error);

// Frees the shared models and their sessions (Service::Shutdown, once every source is closed).
void ReleaseFaceTrackerModels();

} // namespace FaceTracking
