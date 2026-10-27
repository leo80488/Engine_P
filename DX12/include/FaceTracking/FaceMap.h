#pragma once
// A character's face mapping, .ifacemap: JSON, UTF-8 - channel -> morph rules, the vowel
// solver, and the bone assignment (head chain, eyes, jaw). A mapping belongs to a model;
// calibration belongs to the performer and lives elsewhere.
//
//   {"version": 1, "name": "...", "basePreset": "mmd_jp",
//    "morphs": [{"target": "<morph>" | "preset:<name>", "op": "direct|max|min|avg|sum|sub|mul|weighted",
//                "src": "<channel>" | ["<channel>", "@<other target>", ...] | [{"name": ..., "weight": w}],
//                "gain": 1, "offset": 0, "curve": [[x, y], ...] | "gamma": g, "min": 0, "max": 1,
//                "suppressBy": "<channel or @target>", "suppressK": 1}],
//    "vowels": {"enabled": true, "targets": {"a": "...", "i": ..., "u": ..., "e": ..., "o": ...},
//               "model": "default" | {"matrix": [[...6] x5], "bias": [...5]}},
//    "bones": {"chain": [{"role": "Neck", "weight": 0.35, "fallbackRole": "...", "bone": "<name>"}],
//              "gain": {"yaw": 1, "pitch": 1, "roll": 1}, "limitsDeg": {"yaw": 70, "pitch": 45, "roll": 35},
//              "faceBasis": "auto" | {"forward": "+Z", "up": "+Y"},
//              "eyes": {"mode": "bones|morphs|none", "left": "<bone>", "right": "<bone>",
//                       "maxYawDeg": 20, "maxPitchDeg": 15},
//              "jaw": {"mode": "bone|none", "bone": "<bone>", "maxDeg": 20}},
//    "filters": {"preset": "default"}}
//
// basePreset names a built-in mapping (engine/facemaps/<name>.ifacemap): its rules come first,
// and a rule here replaces every base rule with the same target; "vowels" / "bones" / "filters"
// here replace the base's sections. Several rules on one target: the max wins (order-free).
// Runtime names are exact. No engine dependencies (nlohmann/json only).

#include "FaceTracking/FaceChannels.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FaceTracking {

enum class MapOp : uint8_t { Direct, Max, Min, Avg, Sum, Sub, Mul, Weighted };

struct MapSource
{
    std::string name;    // an ARKit channel, one of the source's extras, or "@<target>"
    float weight = 1.0f; // op "weighted" only
};

struct MapRule
{
    std::string target; // a morph name, or "preset:<name>"
    MapOp op = MapOp::Direct;
    std::vector<MapSource> src;
    float gain = 1.0f, offset = 0.0f;
    std::vector<std::pair<float, float>> curve; // piecewise linear, x ascending
    float gamma = 0.0f;                         // > 0: v = v^gamma after the curve
    float minV = 0.0f, maxV = 1.0f;
    std::string suppressBy; // v *= 1 - s * suppressK
    float suppressK = 1.0f;
};

inline constexpr int kVowelCount = 5;  // a i u e o
inline constexpr int kVowelInputs = 6; // jawOpen, mouthFunnel, mouthPucker, stretch (avg), smile (avg), mouthClose

struct VowelMap
{
    bool enabled = false;
    std::string targets[kVowelCount];
    float matrix[kVowelCount][kVowelInputs] = {};
    float bias[kVowelCount] = {};
};

// The default linear vowel model; tools/face_take.py fit-vowels fits better coefficients
// from a recorded take.
void DefaultVowelModel(VowelMap& v);

enum class EyesMode : uint8_t { None, Bones, Morphs };
enum class JawMode : uint8_t { None, Bone };

struct ChainBone
{
    std::string role;         // HumanoidBone role name (Neck, Head, Chest, UpperChest, ...)
    std::string bone;         // an explicit bone name, used when the role does not resolve
    std::string fallbackRole; // tried before renormalizing the weights
    float weight = 0.0f;
};

struct BoneMap
{
    std::vector<ChainBone> chain;
    float gainYaw = 1.0f, gainPitch = 1.0f, gainRoll = 1.0f;
    float limitYawDeg = 70.0f, limitPitchDeg = 45.0f, limitRollDeg = 35.0f;
    bool basisAuto = true;
    std::string forward = "+Z", up = "+Y"; // the head bone's local axes when not auto
    EyesMode eyes = EyesMode::None;
    std::string leftEye, rightEye; // bone names; empty = the LeftEye / RightEye roles
    float eyeMaxYawDeg = 20.0f, eyeMaxPitchDeg = 15.0f;
    JawMode jaw = JawMode::None;
    std::string jawBone; // empty = the Jaw role
    float jawMaxDeg = 20.0f;
};

struct FaceMapProfile
{
    int version = 1;
    std::string name;
    std::string basePreset;
    std::vector<MapRule> morphs;
    VowelMap vowels;
    bool hasVowels = false;
    BoneMap bones;
    bool hasBones = false;
    std::string filterPreset = "default";
    bool hasFilters = false;
};

bool ParseFaceMap(const std::string& text, FaceMapProfile& out, std::string& error);
FaceMapProfile MergeFaceMaps(const FaceMapProfile& base, const FaceMapProfile& over);

// ==== Compiled form: per frame only (slot, weight) arithmetic, no strings ====

struct FaceMapBindHooks
{
    // A morph name -> the skeleton's morph slot, -1 when the model has none of that name.
    std::function<int(std::string_view)> findMorphSlot;
    // A preset name -> its (morph name, bind weight) list; false when the model has no such preset.
    std::function<bool(std::string_view, std::vector<std::pair<std::string, float>>&)> expandPreset;
};

class CompiledFaceMap
{
public:
    struct Input
    {
        int channel = -1; // >= 0: a channel value
        int node = -1;    // >= 0: an earlier target's value
        float weight = 1.0f;
    };
    struct Rule
    {
        MapOp op = MapOp::Direct;
        std::vector<Input> in;
        Input suppress;
        bool hasSuppress = false;
        float gain = 1.0f, offset = 0.0f, gamma = 0.0f, minV = 0.0f, maxV = 1.0f, suppressK = 1.0f;
        std::vector<std::pair<float, float>> curve;
    };
    struct Target
    {
        std::string name;                       // for diagnostics only
        std::vector<Rule> rules;                // max over them
        std::vector<std::pair<int, float>> out; // (slot, scale): one morph, or a preset's morphs
    };

    // Per-target working storage, kept by the caller so a warm Evaluate allocates nothing.
    struct Scratch
    {
        std::vector<float> value;
        std::vector<uint8_t> live;
    };

    // Every resolved slot's weight for one face (values / present indexed by channel); a slot
    // reached twice gets the max. Only LIVE targets emit: a rule is live when one of its inputs
    // is a channel the face carries or a live @target (suppressBy does not count), a target
    // when one of its rules is; the vowels when one of their six inputs is there. A source
    // that never sends a channel (VMC's five presets, a script pushing two values) so leaves
    // the morphs it does not track to whatever drives them underneath.
    void Evaluate(const float* values, const uint64_t present[2], Scratch& scratch,
                  std::vector<std::pair<int, float>>& slotWeights) const;

    // Targets in dependency order (a target's @refs come before it).
    std::vector<Target> targets;
    bool vowelsEnabled = false;
    VowelMap vowels;
    std::vector<int> vowelSlots[kVowelCount]; // a vowel target may be a preset: several slots
    std::vector<float> vowelScales[kVowelCount];
    size_t extrasSeen = 0;                    // the source's extras count when compiled
    std::vector<std::string> warnings;        // unresolved targets and channels, once each
};

// Errors (a cycle through @refs, an unknown op) refuse the whole mapping; unresolved morphs
// and channels only warn - the model may lack them.
bool CompileFaceMap(const FaceMapProfile& profile, const FaceSourceChannels& channels, const FaceMapBindHooks& hooks,
                    CompiledFaceMap& out, std::string& error);

// The five vowel weights from the mouth channels (clamped, then scaled so they never sum past
// the opening jawOpen allows).
void SolveVowels(const VowelMap& v, const float* values, const uint64_t present[2], float out[kVowelCount]);

// ==== The editor's automatic mapping ====
// Fuzzy name matching for the EDITOR: it writes exact rules, and the runtime keeps matching
// names exactly. A name's key: a "blendShape1."-style prefix dropped, a side marker
// (_L / .L / -L / Left, and R) moved to the end as "left" / "right", ASCII lower-cased,
// separators (_ - . space) removed, half-width katakana made full-width.
std::string MorphNameKey(std::string_view name);

struct AutoMapResult
{
    FaceMapProfile profile; // basePreset + the rules found; save it with FaceMapToJson
    std::vector<std::pair<std::string, std::string>> matches; // an ARKit channel -> the model's morph (1:1)
    std::vector<std::pair<std::string, std::string>> renamed; // a base rule's target -> the model's name for it
    std::vector<std::string> unmappedChannels; // ARKit channels no rule on one of the model's morphs reads
    std::vector<std::string> undrivenMorphs;   // the model's morphs no rule writes
};

// `morphs`: the model's morph names. `base`: the built-in mapping to start from (`baseName` its
// preset name, e.g. "mmd_jp"): a base rule whose target the model lacks but has under another
// spelling is copied under the model's name; an ARKit channel whose key is a morph's key - and
// that morph not driven by the base already - gets a direct rule. Then the coverage of the result
// (the base and the new rules together).
AutoMapResult AutoMapMorphs(const std::vector<std::string>& morphs, const FaceMapProfile& base, const std::string& baseName);

// A profile as .ifacemap JSON (UTF-8, indented): every section it has, ParseFaceMap's format.
std::string FaceMapToJson(const FaceMapProfile& p);

} // namespace FaceTracking
