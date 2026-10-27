#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <DirectXMath.h>

namespace Anim::Graph {
using NodeIndex = uint16_t;
constexpr NodeIndex kNoNode = 0xffff;
constexpr float kRelevanceEps = 1e-4f;
constexpr uint32_t kInvalidClip = ~0u;
inline uint32_t Hash(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}
enum class ParamType : uint8_t { Float, Int, Bool, Trigger, Vector3, Id };
struct ParamHandle {NodeIndex index=kNoNode;uint64_t definitionHash=0;std::string name;};
struct StateInfo {std::string state,source,target;float timeInState=0,progress=1;bool transitioning=false;};
struct ParamValue {
    std::array<float, 3> vector{};
    uint32_t integer = 0;
    float Float() const { return vector[0]; }
    bool Bool() const { return integer != 0; }
    static ParamValue FromFloat(float x) { ParamValue v; v.vector[0] = x; return v; }
    static ParamValue FromInt(int32_t x) { ParamValue v; v.integer = static_cast<uint32_t>(x); v.vector[0]=static_cast<float>(x);return v; }
};
static_assert(sizeof(ParamValue) == 16);
struct NodeState {
    float time = 0, previousTime = 0, sampleTime = 0, weight = 0, elapsed = 0;
    bool relevant = false;
    int active = -1, source = -1;
    ParamValue value;
    uint32_t valueFrame = 0;
    int task = -1;
    float velocity = 0, duration = 0, stateTime = 0, unwrappedPhase = 0;
    int transition = -1, departing = -1;
    bool inertialTransition = false;
    bool Transitioning() const { return source != -1 || inertialTransition; }
};
enum class NodeType : uint16_t {
    Output, RefPose, Clip, Blend2, BlendByBool, Param, Constant, Compare, Not,
    BlendByInt, BlendSpace1D, BlendSpace2D, LayeredBlend, ApplyAdditive,
    StateMachine, Slot, CachedPose, TimeScale, Inertialization,
    Math, And, Or, Xor, Select, Remap, Damp, DeltaAngle, WrapAngle,
    StateTime, StateWeight, IsInState, IsTransitioning, ClipTimeRemaining,
    ClipNormalizedTime, EventFired, Random, ClipCurve, AimOffset
};
enum class SlotStatus : uint8_t { Pending, Playing, BlendingOut, Interrupted, Finished, Rejected };
struct SlotHandle { uint32_t id = 0, generation = 0; };
struct SlotRequestDesc {
    std::string slot, alias;
    float blendIn = .1f, blendOut = .2f, rate = 1.f;
    bool interrupt = false, queueUntilReady = false, chainAtEnd = false;
    bool holdLastPose = false, looping = false;
};
struct ParamDecl {
    std::string name;
    ParamType type = ParamType::Float;
    ParamValue value;
    float holdSec = 0.f;
};
}
