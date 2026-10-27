#pragma once
// iFacialMocap: UDP, default port 49983, one text packet per frame, tokens separated by '|':
//   <name>-<0..100>        a blendshape, ARKit names with "_L" / "_R" for "Left" / "Right"
//                          ("eyeBlink_L-35"); '&' is read as the separator too (older versions)
//   head#rx,ry,rz,px,py,pz the head: Euler degrees, then position (a leading '=' is tolerated)
//   rightEye#rx,ry,rz      the eyes: Euler degrees
//   leftEye#rx,ry,rz
// Not a published format: this follows the community's description (UnityIFM, Faceit) and must
// be confirmed against recorded packets (tools/face_udp_capture.py) - the separators, and the
// Euler mapping below, which reads x as pitch (positive = looking DOWN, hence the minus), y as
// yaw (positive = to the subject's right) and z as roll (positive = tilting to the LEFT
// shoulder, hence the minus). UNVERIFIED against a device.
// The app starts streaming once the receiver answers its first packet (UnityIFM's behaviour):
// kIFacialMocapHandshake goes back to the sender's address.
// Header-only (tests/unit/face_parsers_test.cpp).

#include "FaceTracking/FaceChannels.h"
#include "FaceTracking/FaceFrame.h"
#include "FaceTracking/FaceMath.h"

#include <DirectXMath.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace FaceTracking {

inline constexpr std::string_view kIFacialMocapHandshake = "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719";
inline constexpr uint16_t kIFacialMocapPort = 49983;

struct IFacialMocapStats
{
    uint64_t packets = 0;
    uint64_t malformed = 0;      // packets with no readable blendshape, head or eye token (or empty / over 16 KiB)
    uint64_t droppedTokens = 0;  // tokens that are none of those
};

namespace IFacialMocap {

inline bool ParseFloat(std::string_view s, float& out)
{
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    if (s.empty() || s.size() > 32) return false;
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size() && std::isfinite(out);
}

// "a,b,c[,...]" into up to n floats; the number that parsed before the first that did not.
inline int ParseList(std::string_view s, float* out, int n)
{
    int k = 0;
    while (k < n)
    {
        const size_t comma = s.find(',');
        if (!ParseFloat(s.substr(0, comma), out[k])) return k;
        ++k;
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return k;
}

// "eyeBlink_L" -> eyeBlinkLeft's channel; exact ARKit names too.
inline int ChannelOf(std::string_view name)
{
    if (name.size() > 2 && name[name.size() - 2] == '_' && (name.back() == 'L' || name.back() == 'R'))
    {
        char buf[48];
        const std::string_view stem = name.substr(0, name.size() - 2);
        const std::string_view side = name.back() == 'L' ? "Left" : "Right";
        if (stem.size() + side.size() > sizeof(buf)) return -1;
        stem.copy(buf, stem.size());
        side.copy(buf + stem.size(), side.size());
        return FindArkitChannel(std::string_view(buf, stem.size() + side.size()));
    }
    return FindArkitChannel(name);
}

// Euler degrees as the app sends them -> the canonical rotation (see the header comment).
inline DirectX::XMVECTOR HeadFromEuler(float rx, float ry, float rz)
{
    const float d = FaceMath::kPi / 180.0f;
    return FaceMath::ComposeHead(ry * d, -rx * d, -rz * d);
}

} // namespace IFacialMocap

// One packet into `out` (seq and time left to the caller). False when it is not a frame.
inline bool ParseIFacialMocap(std::string_view text, FaceFrame& out, IFacialMocapStats& stats)
{
    ++stats.packets;
    out = FaceFrame{};
    if (text.empty() || text.size() > 16384)
    {
        ++stats.malformed;
        return false;
    }
    int read = 0;
    while (!text.empty())
    {
        const size_t bar = text.find('|');
        std::string_view token = text.substr(0, bar);
        text = bar == std::string_view::npos ? std::string_view{} : text.substr(bar + 1);
        while (!token.empty() && (token.front() == '=' || token.front() == ' ')) token.remove_prefix(1);
        if (token.empty()) continue;
        const size_t hash = token.find('#');
        if (hash != std::string_view::npos)
        {
            const std::string_view key = token.substr(0, hash);
            float v[6];
            const int n = IFacialMocap::ParseList(token.substr(hash + 1), v, 6);
            if (key == "head" && n >= 3)
            {
                DirectX::XMStoreFloat4(&out.headRot, IFacialMocap::HeadFromEuler(v[0], v[1], v[2]));
                out.flags |= kFaceHasHead;
                if (n >= 6)
                {
                    out.headPos = {v[3] * 0.01f, v[4] * 0.01f, v[5] * 0.01f}; // centimetres -> metres
                    out.flags |= kFaceHasHeadPos;
                }
                ++read;
            }
            else if ((key == "leftEye" || key == "rightEye") && n >= 2)
            {
                // The same reading as the head: x = pitch (down positive), y = yaw.
                const float d = FaceMath::kPi / 180.0f;
                out.SetEye(key == "leftEye" ? 0 : 1, v[1] * d, -v[0] * d);
                ++read;
            }
            else
                ++stats.droppedTokens;
            continue;
        }
        // name-value (the last '-' or '&': names never hold either)
        const size_t sep = token.find_last_of("-&");
        if (sep == std::string_view::npos || sep == 0)
        {
            ++stats.droppedTokens;
            continue;
        }
        const int ch = IFacialMocap::ChannelOf(token.substr(0, sep));
        float v = 0.0f;
        if (ch < 0 || !IFacialMocap::ParseFloat(token.substr(sep + 1), v))
        {
            ++stats.droppedTokens;
            continue;
        }
        out.Set(ch, std::fmin(1.0f, std::fmax(0.0f, v * 0.01f)));
        ++read;
    }
    if (read == 0)
    {
        ++stats.malformed;
        return false;
    }
    out.faceFound = 1;
    out.confidence = 1.0f;
    return true;
}

} // namespace FaceTracking
