#pragma once
// VMC protocol: OSC over UDP, default port 39539.
//   /VMC/Ext/Blend/Val  (s name, f value)   one blendshape; Perfect Sync senders use ARKit names
//                                            (PascalCase: "EyeBlinkLeft"), others VRM presets
//   /VMC/Ext/Blend/Apply                     the frame so far is complete
//   /VMC/Ext/Bone/Pos   (s name, f px py pz, f qx qy qz qw)   Unity HumanBodyBones, local
//   /VMC/Ext/OK         (i loaded [, i calibrationState, i calibrationMode, i trackingStatus])
//   /VMC/Ext/T          (f time)             ignored: frames are stamped on arrival
// Blend names: an ARKit name (exactly, or with its first letter upper-case) is that channel;
// a VRM preset name (0.x and 1.0) is one of the source's extras, registered up front so the
// channel table never changes while a reader holds it; anything else is counted and dropped.
// Bones: Unity is left-handed, Y up, an avatar faces +Z - the canonical face space of
// FaceMath.h - so a local rotation reads as is. The head is Head after Neck (QMul(head, neck):
// the turn relative to the chest, which the binding spreads over its own chain); an eye bone
// becomes yaw / pitch by FaceMath::DecomposeHead. UNVERIFIED against a device: the protocol does
// not define the rest orientation of a sender's avatar - confirm with a recorded "turn right /
// look up / tilt right" capture (tools/face_udp_capture.py).
// Header-only (tests/unit/face_parsers_test.cpp).

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceMath.h"
#include "FaceTracking/OscParser.h"

#include <DirectXMath.h>

#include <cmath>
#include <cstdint>
#include <string_view>
#include <vector>

namespace FaceTracking {

// VRM 0.x and 1.0 expression presets, the extras a VMC source registers.
inline constexpr std::string_view kVrmPresetNames[] = {
    // VRM 0.x
    "A", "I", "U", "E", "O", "Blink", "Blink_L", "Blink_R", "Joy", "Angry", "Sorrow", "Fun", "Surprised", "Neutral",
    "LookUp", "LookDown", "LookLeft", "LookRight",
    // VRM 1.0
    "aa", "ih", "ou", "ee", "oh", "blink", "blinkLeft", "blinkRight", "happy", "angry", "sad", "relaxed", "surprised",
    "neutral", "lookUp", "lookDown", "lookLeft", "lookRight",
};

struct VmcStats
{
    uint64_t packets = 0;
    uint64_t malformed = 0;
    uint64_t frames = 0;
    uint64_t droppedNames = 0;  // blend names neither ARKit nor a VRM preset
    uint64_t otherMessages = 0; // addresses face tracking does not read
};

class VmcParser
{
public:
    VmcParser()
    {
        for (std::string_view n : kVrmPresetNames) m_channels.FindOrAdd(n);
    }

    // The source's channel table: the ARKit 52 plus the VRM presets, fixed from construction.
    const FaceSourceChannels& Channels() const { return m_channels; }
    const VmcStats& Stats() const { return m_stats; }

    // One packet; appends the frames it completed (one per /VMC/Ext/Blend/Apply, or - from a
    // sender that never applies blendshapes - one per packet that carried bones). The frames
    // carry no seq or time: the caller stamps them.
    void Feed(const uint8_t* data, size_t size, std::vector<FaceFrame>& out)
    {
        ++m_stats.packets;
        if (!Osc::ParsePacket(data, size, m_messages))
        {
            ++m_stats.malformed;
            return;
        }
        bool bones = false;
        for (const Osc::Message& m : m_messages)
        {
            if (m.address == "/VMC/Ext/Blend/Val")
            {
                std::string_view name;
                float v = 0.0f;
                if (!m.Str(0, name) || !m.Float(1, v) || !std::isfinite(v)) continue;
                const int ch = ChannelOf(name);
                if (ch < 0) { ++m_stats.droppedNames; continue; }
                m_pending.Set(ch, v);
            }
            else if (m.address == "/VMC/Ext/Blend/Apply")
            {
                m_sawApply = true;
                Emit(out);
            }
            else if (m.address == "/VMC/Ext/Bone/Pos")
            {
                std::string_view name;
                float q[4];
                if (!m.Str(0, name) || !m.Float(4, q[0]) || !m.Float(5, q[1]) || !m.Float(6, q[2]) || !m.Float(7, q[3])) continue;
                const float len2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
                if (!std::isfinite(len2) || len2 < 1e-12f) continue;
                const float inv = 1.0f / std::sqrt(len2);
                const DirectX::XMFLOAT4 rot = {q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv};
                if (name == "Head") { m_head = rot; m_haveHead = true; bones = true; }
                else if (name == "Neck") { m_neck = rot; m_haveNeck = true; bones = true; }
                else if (name == "LeftEye") { m_eye[0] = rot; m_haveEye[0] = true; bones = true; }
                else if (name == "RightEye") { m_eye[1] = rot; m_haveEye[1] = true; bones = true; }
            }
            else if (m.address == "/VMC/Ext/OK")
            {
                int32_t status = 1;
                if (m.argc >= 4 && m.Int(3, status)) m_found = status != 0;
            }
            else
                ++m_stats.otherMessages;
        }
        if (bones && !m_sawApply) Emit(out);
    }

private:
    int ChannelOf(std::string_view name) const
    {
        const int exact = m_channels.Find(name);
        if (exact >= 0) return exact;
        // Perfect Sync: "EyeBlinkLeft" for "eyeBlinkLeft".
        if (name.empty() || name.size() > 40 || !(name[0] >= 'A' && name[0] <= 'Z')) return -1;
        char buf[41];
        name.copy(buf, name.size());
        buf[0] = static_cast<char>(buf[0] - 'A' + 'a');
        return FindArkitChannel(std::string_view(buf, name.size()));
    }

    void Emit(std::vector<FaceFrame>& out)
    {
        FaceFrame f = m_pending; // blend values persist until a sender overwrites them
        f.faceFound = m_found ? 1 : 0;
        f.confidence = m_found ? 1.0f : 0.0f;
        if (m_haveHead || m_haveNeck)
        {
            using namespace DirectX;
            XMVECTOR q = m_haveHead ? XMLoadFloat4(&m_head) : XMQuaternionIdentity();
            if (m_haveNeck) q = FaceMath::QMul(q, XMLoadFloat4(&m_neck)); // head, then neck: relative to the chest
            XMStoreFloat4(&f.headRot, XMQuaternionNormalize(q));
            f.flags |= kFaceHasHead;
        }
        if (m_haveEye[0] || m_haveEye[1])
        {
            for (int side = 0; side < 2; ++side)
            {
                const int from = m_haveEye[side] ? side : 1 - side;
                const FaceMath::YawPitchRoll ypr = FaceMath::DecomposeHead(DirectX::XMLoadFloat4(&m_eye[from]));
                f.SetEye(side, ypr.yaw, ypr.pitch);
            }
        }
        out.push_back(f);
        ++m_stats.frames;
    }

    FaceSourceChannels m_channels;
    std::vector<Osc::Message> m_messages;
    FaceFrame m_pending;
    DirectX::XMFLOAT4 m_head = {0, 0, 0, 1}, m_neck = {0, 0, 0, 1}, m_eye[2] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    bool m_haveHead = false, m_haveNeck = false, m_haveEye[2] = {false, false};
    bool m_found = true;
    bool m_sawApply = false;
    VmcStats m_stats;
};

} // namespace FaceTracking
