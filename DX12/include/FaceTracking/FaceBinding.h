#pragma once
// One character's face tracking state: what FaceTrackingSystem compiled for it, the frames it
// consumed, and this frame's outputs. Owned by FaceTrackingComponent::rt; read by
// FaceTracking.GetDebug.

#include "ECS/ECS.h"
#include "FaceTracking/FaceAuthority.h"
#include "FaceTracking/FaceFilter.h"
#include "FaceTracking/FaceHistory.h"
#include "FaceTracking/FaceMap.h"
#include "FaceTracking/FacePoseMath.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace FaceTracking {

inline constexpr int kMaxChainBones = 6;

struct BindingRuntime
{
    // The entity this runtime belongs to: a component copied by value onto another entity
    // (a duplicate) must not share it.
    Entity owner = NullEntity;
    uint32_t ownerGeneration = 0;

    // ---- the mapping, compiled against this skeleton, model and source ----
    std::string profileRequested;      // FaceTrackingComponent::profile when compiled
    std::string profilePath;           // what was loaded ("" asks for the automatic built-in)
    uint64_t profileGeneration = 0;    // FaceTrackingSystem's .ifacemap reload counter then
    uint32_t skeletonAsset = ~0u;
    std::string sourceName;
    bool mirror = true;                // the mirror the processed frames were made with
    size_t sourceExtras = 0;           // a source that registered more extras recompiles
    bool compiled = false;
    bool compileFailed = false;        // latched until the profile, skeleton or source changes
    std::string compileError;
    FaceMapProfile profile;
    CompiledFaceMap map;
    FaceFilterSettings filters;

    // ---- the frames ----
    FaceCursor cursor;
    FaceFrameFilter filter;
    FacePose::Calibration calibration; // the source's (Service::GetCalibration), identity without one
    uint64_t calibrationGeneration = 0; // the one applied; 0 = none
    FaceHistory processed;             // found frames only: calibrated, mirrored, filtered
    FaceAuthority authority;
    int64_t nowUs = 0;                 // the clock the binding samples at (its source's)
    bool newestFound = false;          // the newest frame consumed said a face was there
    SampleState sampleState = SampleState::Empty;
    FaceFrame sampled;
    uint64_t framesConsumed = 0;
    bool sourceMissing = true;
    uint64_t sourceEpoch = 0;          // SourceStatus::epoch of the source being read

    // ---- this frame's morph output (also on FacialAnimationComponent::tracked) ----
    CompiledFaceMap::Scratch scratch;
    std::vector<std::pair<int, float>> slotWeights;
    uint32_t trackedCount = 0;
    float morphAlpha = 0.0f;
    bool blinkHeld = false;            // with hysteresis: on above 0.55, off below 0.45

    // ---- this frame's bone targets (Resolve fills them, ApplyPose applies them) ----
    float authorityValue = 0.0f;
    bool haveHead = false;
    DirectX::XMFLOAT4 headTarget = {0, 0, 0, 1};  // face space: shaped, then shared by authority x weightHead
    bool haveEyes = false;
    DirectX::XMFLOAT4 eyeTarget[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}}; // [left, right], face space
    float eyeYawPitchDeg[2][2] = {};
    bool haveJaw = false;
    float jawAngle = 0.0f;             // radians, already x authority x weightMorph

    // ---- the bones, resolved per skeleton asset and mapping ----
    uint32_t bonesFor = ~0u;
    uint64_t bonesStamp = ~0ull;       // the compile they were resolved for
    bool bonesTried = false;
    int chainCount = 0;
    int chainBone[kMaxChainBones] = {-1, -1, -1, -1, -1, -1}; // ascending bone index: parents first
    float chainWeight[kMaxChainBones] = {};                   // renormalized to sum 1
    int chainRootParent = -1;
    int headBone = -1, leftEye = -1, rightEye = -1, jawBone = -1;
    DirectX::XMFLOAT4 basis = {0, 0, 0, 1};          // face axes -> model axes, at rest
    bool basisFromEyes = false;
    DirectX::XMFLOAT4 rootParentRest = {0, 0, 0, 1}; // the chain root's parent, model space, at rest
    DirectX::XMFLOAT4 headRest = {0, 0, 0, 1};
    uint64_t compileStamp = 0;         // bumped by every compile

    std::vector<std::string> warnings; // what compiling and binding found (logged once per compile)
};

} // namespace FaceTracking
