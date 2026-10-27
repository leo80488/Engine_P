#pragma once
// The face tracking channel table. Indices 0-51 are the 52 ARKit blendshapes, fixed, in
// ARKit's own documentation order (eyes, jaw, mouth, brows, cheeks, nose, tongue); every
// source maps its own order onto these. Indices 52-115 are extras a source names itself
// (VMC's "Joy", a take's extra columns); FaceSourceChannels keeps that source's names.
//
// Runtime lookups are exact, case-sensitive UTF-8 matches; forgiving matches ("_L" for
// "Left", prefixes) belong to the editor's auto-mapping only.
// Header-only: tests/unit/face_core_test.cpp compiles it without the engine library.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FaceTracking {

inline constexpr int kArkitChannelCount = 52;
inline constexpr int kMaxExtraChannels = 64;
inline constexpr int kMaxFaceChannels = kArkitChannelCount + kMaxExtraChannels;

enum class ChannelGroup : uint8_t { Eye, Jaw, Mouth, Brow, Cheek, Nose, Tongue, Extra };

struct ArkitChannelInfo
{
    std::string_view name;
    ChannelGroup group;
};

inline constexpr ArkitChannelInfo kArkitChannels[kArkitChannelCount] = {
    // Eyes (14)
    {"eyeBlinkLeft", ChannelGroup::Eye},     {"eyeLookDownLeft", ChannelGroup::Eye},
    {"eyeLookInLeft", ChannelGroup::Eye},    {"eyeLookOutLeft", ChannelGroup::Eye},
    {"eyeLookUpLeft", ChannelGroup::Eye},    {"eyeSquintLeft", ChannelGroup::Eye},
    {"eyeWideLeft", ChannelGroup::Eye},      {"eyeBlinkRight", ChannelGroup::Eye},
    {"eyeLookDownRight", ChannelGroup::Eye}, {"eyeLookInRight", ChannelGroup::Eye},
    {"eyeLookOutRight", ChannelGroup::Eye},  {"eyeLookUpRight", ChannelGroup::Eye},
    {"eyeSquintRight", ChannelGroup::Eye},   {"eyeWideRight", ChannelGroup::Eye},
    // Jaw (4)
    {"jawForward", ChannelGroup::Jaw}, {"jawLeft", ChannelGroup::Jaw},
    {"jawRight", ChannelGroup::Jaw},   {"jawOpen", ChannelGroup::Jaw},
    // Mouth (23)
    {"mouthClose", ChannelGroup::Mouth},         {"mouthFunnel", ChannelGroup::Mouth},
    {"mouthPucker", ChannelGroup::Mouth},        {"mouthLeft", ChannelGroup::Mouth},
    {"mouthRight", ChannelGroup::Mouth},         {"mouthSmileLeft", ChannelGroup::Mouth},
    {"mouthSmileRight", ChannelGroup::Mouth},    {"mouthFrownLeft", ChannelGroup::Mouth},
    {"mouthFrownRight", ChannelGroup::Mouth},    {"mouthDimpleLeft", ChannelGroup::Mouth},
    {"mouthDimpleRight", ChannelGroup::Mouth},   {"mouthStretchLeft", ChannelGroup::Mouth},
    {"mouthStretchRight", ChannelGroup::Mouth},  {"mouthRollLower", ChannelGroup::Mouth},
    {"mouthRollUpper", ChannelGroup::Mouth},     {"mouthShrugLower", ChannelGroup::Mouth},
    {"mouthShrugUpper", ChannelGroup::Mouth},    {"mouthPressLeft", ChannelGroup::Mouth},
    {"mouthPressRight", ChannelGroup::Mouth},    {"mouthLowerDownLeft", ChannelGroup::Mouth},
    {"mouthLowerDownRight", ChannelGroup::Mouth}, {"mouthUpperUpLeft", ChannelGroup::Mouth},
    {"mouthUpperUpRight", ChannelGroup::Mouth},
    // Brows (5)
    {"browDownLeft", ChannelGroup::Brow},    {"browDownRight", ChannelGroup::Brow},
    {"browInnerUp", ChannelGroup::Brow},     {"browOuterUpLeft", ChannelGroup::Brow},
    {"browOuterUpRight", ChannelGroup::Brow},
    // Cheeks (3)
    {"cheekPuff", ChannelGroup::Cheek}, {"cheekSquintLeft", ChannelGroup::Cheek},
    {"cheekSquintRight", ChannelGroup::Cheek},
    // Nose (2)
    {"noseSneerLeft", ChannelGroup::Nose}, {"noseSneerRight", ChannelGroup::Nose},
    // Tongue (1)
    {"tongueOut", ChannelGroup::Tongue},
};

// The ARKit index of `name`, or -1.
inline int FindArkitChannel(std::string_view name)
{
    for (int i = 0; i < kArkitChannelCount; ++i)
        if (kArkitChannels[i].name == name) return i;
    return -1;
}

inline std::string_view ArkitChannelName(int index)
{
    return (index >= 0 && index < kArkitChannelCount) ? kArkitChannels[index].name : std::string_view{};
}

// One source's channel names: the fixed ARKit table plus the extras it registered, in
// registration order (extra k is channel kArkitChannelCount + k).
class FaceSourceChannels
{
public:
    // The channel index of `name`, or -1.
    int Find(std::string_view name) const
    {
        const int arkit = FindArkitChannel(name);
        if (arkit >= 0) return arkit;
        for (size_t k = 0; k < m_extras.size(); ++k)
            if (m_extras[k] == name) return kArkitChannelCount + static_cast<int>(k);
        return -1;
    }
    // Find, or register `name` as the next extra; -1 when the extras are full or the name is empty.
    int FindOrAdd(std::string_view name)
    {
        const int found = Find(name);
        if (found >= 0 || name.empty()) return found;
        if (static_cast<int>(m_extras.size()) >= kMaxExtraChannels) return -1;
        m_extras.emplace_back(name);
        return kArkitChannelCount + static_cast<int>(m_extras.size()) - 1;
    }
    std::string_view Name(int index) const
    {
        if (index < kArkitChannelCount) return ArkitChannelName(index);
        const size_t k = static_cast<size_t>(index - kArkitChannelCount);
        return k < m_extras.size() ? std::string_view(m_extras[k]) : std::string_view{};
    }
    const std::vector<std::string>& Extras() const { return m_extras; }

private:
    std::vector<std::string> m_extras;
};

} // namespace FaceTracking
