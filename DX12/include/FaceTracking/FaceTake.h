#pragma once
// The take format, .iface: JSON Lines, UTF-8. Line 1 is the header; every later line is one
// frame, as a source delivered it (before calibration and filtering, so one take replays
// under any calibration and mapping):
//
//   {"iface":1,"source":"synthetic","convention":1,"mirrored":false,"fps":60,
//    "channels":["eyeBlinkLeft","jawOpen","Joy"]}
//   {"t":0.0,"found":true,"conf":0.9,"v":{"eyeBlinkLeft":0.5,"jawOpen":0.25,"Joy":1},
//    "head":[x,y,z,w],"pos":[x,y,z],"eyes":[[yawL,pitchL],[yawR,pitchR]]}
//
// t is seconds from the take's start, non-decreasing; v lists only the channels present in
// that frame, each named in the header's "channels" (ARKit names and the take's extras);
// head is the canonical face-space quaternion of FaceMath.h (convention 1), pos metres, eyes
// radians. "head", "pos" and "eyes" are optional per frame. Blank lines are skipped.
// No engine dependencies (nlohmann/json only): tests/unit/face_core_test.cpp compiles it.

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace FaceTracking {

inline constexpr int kFaceTakeVersion = 1;
inline constexpr int kFaceConvention = 1; // FaceMath.h's sign convention

struct FaceTake
{
    std::string sourceKind;      // who recorded it ("synthetic", "vmc", ...)
    bool mirrored = false;       // the recording is mirrored already
    double fps = 0.0;            // nominal; informational only
    FaceSourceChannels channels; // the extras the take names (ARKit channels are implicit)
    std::vector<FaceFrame> frames; // sourceTimeUs from "t"; seq 0 - the player numbers frames
};

// Errors name the line ("line 7: ..."); `out` is unspecified after a failure.
bool ParseTake(std::istream& in, FaceTake& out, std::string& error);
bool ReadTakeFile(const std::string& path, FaceTake& out, std::string& error);
bool WriteTake(std::ostream& out, const FaceTake& take, std::string& error);
bool WriteTakeFile(const std::string& path, const FaceTake& take, std::string& error);

} // namespace FaceTracking
