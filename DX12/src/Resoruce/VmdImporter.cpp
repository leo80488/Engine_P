#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <DirectXMath.h>
#include <windows.h>   // MultiByteToWideChar / WideCharToMultiByte

namespace Resource
{
    // ==== VMD binary structures (little-endian, tightly packed) ====
    // Reference: https://mikumikudance.fandom.com/wiki/VMD_file_format
#pragma pack(push, 1)

    // VMD2 header: 30-byte magic + 20-byte model name = 50 bytes total.
    // VMD1 header: 30-byte magic + 10-byte model name = 40 bytes total.
    struct VmdMagic
    {
        char magic[30];  // "Vocaloid Motion Data 0002" (VMD2) or "Vocaloid Motion Data file" (VMD1)
    };

    // One bone keyframe: 111 bytes.
    struct VmdBoneFrame
    {
        char     boneName[15];      // bone name, Shift-JIS, null-padded (not always null-terminated)
        uint32_t frameNo;           // frame index at 30 fps
        float    pos[3];            // local position offset in MMD units (x, y, z)
        float    rot[4];            // local rotation quaternion (x, y, z, w)
        uint8_t  interpolation[64]; // 4 bezier curves (x/y/z/r) x {x1,y1,x2,y2} x 4 copied sets
    };

    // One morph (face) keyframe: 23 bytes.
    struct VmdMorphFrame
    {
        char     morphName[15];
        uint32_t frameNo;
        float    weight;
    };

    // One camera keyframe: 61 bytes. Present only in camera/lighting VMDs (whose model name is the
    // Japanese for "camera / lighting"); model-motion VMDs have zero of these.
    struct VmdCameraFrame
    {
        uint32_t frameNo;            // frame index at 30 fps
        float    distance;           // camera<->interest distance (usually NEGATIVE = camera behind)
        float    interest[3];        // gaze/target point in MMD space (== engine space, identity)
        float    rotation[3];        // euler radians (rx=pitch, ry=yaw, rz=roll)
        uint8_t  interpolation[24];  // 6 channels (x,y,z,rot,distance,angle) x {x1,x2,y1,y2}
        uint32_t viewAngle;          // vertical FOV in DEGREES
        uint8_t  perspective;        // 0 = perspective ON, 1 = OFF (orthographic)
    };

    struct BezierCurve { float x1, y1, x2, y2; };

#pragma pack(pop)

    static_assert(sizeof(VmdMagic)       == 30,  "VmdMagic size mismatch");
    static_assert(sizeof(VmdBoneFrame)   == 111, "VmdBoneFrame size mismatch");
    static_assert(sizeof(VmdMorphFrame)  == 23,  "VmdMorphFrame size mismatch");
    static_assert(sizeof(VmdCameraFrame) == 61,  "VmdCameraFrame size mismatch");

    // ==== Shift-JIS -> UTF-8 (Win32; the raw bytes when a conversion fails) ====
    static std::string SjisToUtf8(const char* src, int srcBytes)
    {
        // Determine actual length (may not be null-terminated within the field)
        int actualLen = 0;
        while (actualLen < srcBytes && src[actualLen] != '\0')
            ++actualLen;
        if (actualLen == 0)
            return {};

        int wLen = MultiByteToWideChar(932 /*CP_SJIS*/, 0, src, actualLen, nullptr, 0);
        if (wLen <= 0)
            return std::string(src, static_cast<size_t>(actualLen));

        std::wstring wide(static_cast<size_t>(wLen), L'\0');
        MultiByteToWideChar(932, 0, src, actualLen, wide.data(), wLen);

        int uLen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wLen, nullptr, 0, nullptr, nullptr);
        if (uLen <= 0)
            return std::string(src, static_cast<size_t>(actualLen));

        std::string utf8(static_cast<size_t>(uLen), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wLen, utf8.data(), uLen, nullptr, nullptr);
        return utf8;
    }

    // ==== Coordinates: identity ====
    // MMD and DX12 are both left-handed Y-up; the Z direction (toward the viewer vs into the screen) is
    // handled by camera placement. Must match PmxImporter (identity conversion).
    static DirectX::XMFLOAT3 ConvertPos(const float p[3])
    {
        return { p[0], p[1], p[2] };
    }

    static DirectX::XMFLOAT4 ConvertRot(const float q[4])
    {
        return { q[0], q[1], q[2], q[3] };
    }

    static BezierCurve UnpackBezier(const uint8_t interp[64], int curveIndex)
    {
        // VMD stores 4 redundant copies; first copy starts at byte 0
        // Within the first copy, each axis occupies columns 0..3
        // Layout: [Xx1][Yx1][Zx1][Rx1]  [Xy1][Yy1][Zy1][Ry1]
        //         [Xx2][Yx2][Zx2][Rx2]  [Xy2][Yy2][Zy2][Ry2]
        // So curveIndex = 0..3 selects the column
        return {
            interp[curveIndex + 0] / 127.0f,   // x1
            interp[curveIndex + 4] / 127.0f,   // y1
            interp[curveIndex + 8] / 127.0f,   // x2
            interp[curveIndex + 12] / 127.0f,  // y2
        };
    }
    static float BezierValue(float x1, float y1, float x2, float y2, float ratio)
    {
        float tt = ratio;
        for (int i = 0; i < 8; ++i)
        {
            float tm = 1.0f - tt;
            float fxt = 3.0f * tt * tm * tm * x1 + 3.0f * tt * tt * tm * x2 + tt * tt * tt;
            float dxt = 3.0f * tm * tm * x1 + 6.0f * tt * tm * (x2 - x1) + 3.0f * tt * tt * (1.0f - x2);
            if (fabsf(dxt) < 1e-6f) break;
            tt -= (fxt - ratio) / dxt;
            tt = std::clamp(tt, 0.0f, 1.0f);
        }
        float tm = 1.0f - tt;
        return 3.0f * tt * tm * tm * y1 + 3.0f * tt * tt * tm * y2 + tt * tt * tt;
    }

    // ==== Per-bone sparse keyframe storage ====
    struct BoneKey
    {
        uint32_t          frameNo;
        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT4 rot;
        BezierCurve       bezierX, bezierY, bezierZ, bezierR;
    };

    // Spherical linear interpolation of rotations between two keyframes.
    static DirectX::XMFLOAT4 SlerpRot(const DirectX::XMFLOAT4& a,
                                       const DirectX::XMFLOAT4& b,
                                       float t)
    {
        using namespace DirectX;
        XMVECTOR qa = XMLoadFloat4(&a);
        XMVECTOR qb = XMLoadFloat4(&b);
        // Ensure shortest-path slerp by negating if dot product is negative.
        if (XMVectorGetX(XMVector4Dot(qa, qb)) < 0.f)
            qb = XMVectorNegate(qb);
        XMFLOAT4 result;
        XMStoreFloat4(&result, XMQuaternionSlerp(qa, qb, t));
        return result;
    }

    // Sample the sparse keyframe list at a given (fractional) frame position.
    static void SampleKeys(const std::vector<BoneKey>& keys,
                            float                        sampleFrame,
                            DirectX::XMFLOAT3&           outPos,
                            DirectX::XMFLOAT4&           outRot)
    {
        if (keys.empty())
        {
            outPos = { 0.f, 0.f, 0.f };
            outRot = { 0.f, 0.f, 0.f, 1.f };
            return;
        }
        // Clamp to first/last key
        if (sampleFrame <= static_cast<float>(keys.front().frameNo))
        {
            outPos = keys.front().pos;
            outRot = keys.front().rot;
            return;
        }
        if (sampleFrame >= static_cast<float>(keys.back().frameNo))
        {
            outPos = keys.back().pos;
            outRot = keys.back().rot;
            return;
        }
        // Binary search for the surrounding pair
        size_t lo = 0, hi = keys.size() - 1;
        while (lo + 1 < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (keys[mid].frameNo <= static_cast<uint32_t>(sampleFrame))
                lo = mid;
            else
                hi = mid;
        }
        const float span = static_cast<float>(keys[hi].frameNo - keys[lo].frameNo);
        const float ratio = (span > 1e-6f) ? (sampleFrame - static_cast<float>(keys[lo].frameNo)) / span : 0.f;

        const auto& bx = keys[hi].bezierX;
        const auto& by = keys[hi].bezierY;
        const auto& bz = keys[hi].bezierZ;
        const auto& br = keys[hi].bezierR;

        float tx = BezierValue( bx.x1, bx.y1, bx.x2, bx.y2, ratio);
        float ty = BezierValue( by.x1, by.y1, by.x2, by.y2, ratio);
        float tz = BezierValue( bz.x1, bz.y1, bz.x2, bz.y2, ratio);
        float tr = BezierValue( br.x1, br.y1, br.x2, br.y2, ratio);

        outPos.x = keys[lo].pos.x + tx * (keys[hi].pos.x - keys[lo].pos.x);
        outPos.y = keys[lo].pos.y + ty * (keys[hi].pos.y - keys[lo].pos.y);
        outPos.z = keys[lo].pos.z + tz * (keys[hi].pos.z - keys[lo].pos.z);
        outRot = SlerpRot(keys[lo].rot, keys[hi].rot, tr);
    }

    // ==== Sparse morph keyframe ====
    struct MorphKey
    {
        uint32_t frameNo;
        float    weight;
    };

    static float SampleMorphKeys(const std::vector<MorphKey>& keys, float sampleFrame)
    {
        if (keys.empty()) return 0.f;
        if (sampleFrame <= static_cast<float>(keys.front().frameNo)) return keys.front().weight;
        if (sampleFrame >= static_cast<float>(keys.back().frameNo))  return keys.back().weight;

        size_t lo = 0, hi = keys.size() - 1;
        while (lo + 1 < hi)
        {
            const size_t mid = (lo + hi) / 2;
            if (keys[mid].frameNo <= static_cast<uint32_t>(sampleFrame)) lo = mid;
            else hi = mid;
        }
        const float span = static_cast<float>(keys[hi].frameNo - keys[lo].frameNo);
        const float t    = (span > 1e-6f) ? (sampleFrame - static_cast<float>(keys[lo].frameNo)) / span : 0.f;
        return keys[lo].weight + t * (keys[hi].weight - keys[lo].weight);
    }

    // ==== Camera keyframes ====
    // Parse, interpolate (per-channel bezier) and normalize the MMD camera model into the engine's
    // format-agnostic CameraAnimKey.

    // Sparse camera keyframe (raw MMD parameters + per-channel bezier easing).
    struct CameraRawKey
    {
        uint32_t          frameNo;
        float             distance;
        DirectX::XMFLOAT3 interest;   // identity-mapped MMD->engine
        DirectX::XMFLOAT3 euler;      // radians (rx, ry, rz)
        float             fovDeg;     // vertical FOV, degrees
        uint8_t           ortho;      // 1 = orthographic frame
        // Bezier easing per channel: 0=interestX 1=interestY 2=interestZ
        //                            3=rotation  4=distance  5=angle(FOV)
        BezierCurve       bz[6];
    };

    // Interpolated MMD camera parameters at a (fractional) frame.
    struct CameraSample
    {
        DirectX::XMFLOAT3 interest{ 0.f, 0.f, 0.f };
        float             distance = -45.f;
        DirectX::XMFLOAT3 euler{ 0.f, 0.f, 0.f };
        float             fovDeg   = 30.f;
        uint8_t           ortho    = 0;
    };

    // Camera interpolation: 24 bytes = 6 channels x 4 bytes, each channel laid out (x1, x2, y1, y2),
    // unlike the bone interpolation packing.
    static BezierCurve UnpackCameraBezier(const uint8_t interp[24], int channel)
    {
        const int b = channel * 4;
        return {
            interp[b + 0] / 127.0f,   // x1
            interp[b + 2] / 127.0f,   // y1
            interp[b + 1] / 127.0f,   // x2
            interp[b + 3] / 127.0f,   // y2
        };
    }

    // Sample the sparse camera keyframes at `sampleFrame`. "Hold last, ease to
    // next": lo = last key with frameNo <= f (so a duplicate-frame CUT yields its
    // POST-cut state), hi = first key with frameNo > f.
    static CameraSample SampleCameraKeys(const std::vector<CameraRawKey>& keys, float sampleFrame)
    {
        CameraSample s;
        if (keys.empty()) return s;

        auto fillFrom = [&](const CameraRawKey& k) {
            s.interest = k.interest; s.distance = k.distance;
            s.euler = k.euler; s.fovDeg = k.fovDeg; s.ortho = k.ortho;
        };

        if (sampleFrame <= static_cast<float>(keys.front().frameNo)) { fillFrom(keys.front()); return s; }
        if (sampleFrame >= static_cast<float>(keys.back().frameNo))  { fillFrom(keys.back());  return s; }

        // upper_bound on frameNo: first key strictly after f.
        size_t hi = 0;
        while (hi < keys.size() && static_cast<float>(keys[hi].frameNo) <= sampleFrame) ++hi;
        if (hi == 0)            { fillFrom(keys.front()); return s; }
        if (hi >= keys.size())  { fillFrom(keys.back());  return s; }
        const size_t lo = hi - 1;

        const float span  = static_cast<float>(keys[hi].frameNo - keys[lo].frameNo);
        const float ratio = (span > 1e-6f)
            ? (sampleFrame - static_cast<float>(keys[lo].frameNo)) / span : 0.f;

        const BezierCurve* bz = keys[hi].bz; // easing belongs to the segment's END key
        auto ease = [&](int ch) {
            return BezierValue(bz[ch].x1, bz[ch].y1, bz[ch].x2, bz[ch].y2, ratio);
        };
        const float tX = ease(0), tY = ease(1), tZ = ease(2);
        const float tR = ease(3), tD = ease(4), tA = ease(5);

        const CameraRawKey& a = keys[lo];
        const CameraRawKey& b = keys[hi];
        s.interest = { a.interest.x + tX * (b.interest.x - a.interest.x),
                       a.interest.y + tY * (b.interest.y - a.interest.y),
                       a.interest.z + tZ * (b.interest.z - a.interest.z) };
        // MMD interpolates the camera euler angles directly (not quaternions).
        s.euler    = { a.euler.x + tR * (b.euler.x - a.euler.x),
                       a.euler.y + tR * (b.euler.y - a.euler.y),
                       a.euler.z + tR * (b.euler.z - a.euler.z) };
        s.distance = a.distance + tD * (b.distance - a.distance);
        s.fovDeg   = a.fovDeg   + tA * (b.fovDeg   - a.fovDeg);
        s.ortho    = a.ortho;  // step (no interpolation of projection mode)
        return s;
    }

    // Converts one interpolated MMD camera sample into the engine's format-agnostic CameraAnimKey. MMD
    // and this engine are both left-handed Y-up and PMX/VMD geometry loads with identity (see
    // ConvertPos), so rebuilding MMD's own camera transform keeps the framing.
    //
    // MMD camera model: the camera orbits an `interest` point. With euler = 0 it sits at
    // interest + (0,0,distance) (distance negative => behind) and looks toward +Z; the euler
    // (rx=pitch/X, ry=yaw/Y, rz=roll/Z) rotates that orbit.
    //
    // Axis sign: MMD measures all three camera euler angles with the opposite sense to a same-sign
    // DirectX build, so each angle is negated here; the compose order is pitch(X) -> roll(Z) -> yaw(Y).
    // This matches saba (the reference MMD C++ renderer: rot = Ry(ry)*Rz(-rz)*Rx(rx) in its RH frame,
    // mapped to this LH frame by reflecting Z) and three.js's MMDLoader (euler.set(-rx,-ry,-rz)). Do not
    // build the quaternion from the un-negated angles (e.g. XMQuaternionRotationRollPitchYaw): that
    // mirror-images the orbit/tilt/bank. This is the only place the MMD camera math lives.
    static CameraAnimKey MmdCameraToPose(const CameraSample& s, bool isCut)
    {
        using namespace DirectX;
        const XMMATRIX rotM = XMMatrixRotationX(-s.euler.x)    // pitch
                            * XMMatrixRotationZ(-s.euler.z)    // roll
                            * XMMatrixRotationY(-s.euler.y);   // yaw
        const XMVECTOR q   = XMQuaternionRotationMatrix(rotM);
        const XMVECTOR fwd = XMVector3Rotate(XMVectorSet(0.f, 0.f, 1.f, 0.f), q); // +Z forward (LH)
        const XMVECTOR eye = XMVectorAdd(XMLoadFloat3(&s.interest),
                                         XMVectorScale(fwd, s.distance));         // distance<0 => behind

        CameraAnimKey key{};
        XMStoreFloat3(&key.position, eye);
        XMStoreFloat4(&key.rotation, XMQuaternionNormalize(q));
        float fovY = s.fovDeg * (XM_PI / 180.f);
        fovY = std::clamp(fovY, 0.01f, XM_PI - 0.01f);
        key.fovY        = fovY;
        key.nearZ       = 0.1f;
        key.farZ        = 1000.f;
        key.orthoHeight = 0.f;     // no orthographic camera path: approximated as perspective
        key.flags       = isCut ? CAMERA_KEY_CUT : 0u;
        return key;
    }

    // ==== Serialization helpers: bone clips (the same layout as AnimationImporter) ====
    static size_t ClipPayloadSize(const AnimClipData& clip)
    {
        size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // clip header
        for (const auto& ch : clip.channels)
        {
            (void)ch;
            sz += 64;                                               // boneName
            sz += clip.frameCount * (3 + 4 + 3) * sizeof(float);  // pos + rot + scale
        }
        sz += sizeof(uint32_t);                                     // eventCount
        sz += clip.events.size() * (sizeof(float) + sizeof(uint32_t));
        return sz;
    }

    static void WriteClip(const AnimClipData& clip, uint8_t*& dst)
    {
        const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
        const uint32_t fc = clip.frameCount;
        std::memcpy(dst, &cc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &fc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &clip.duration,  sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, &clip.frameRate, sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, clip.name,       64);               dst += 64;

        for (const AnimClipChannel& ch : clip.channels)
        {
            std::memcpy(dst, ch.name, 64); dst += 64;
            std::memcpy(dst, ch.positions.data(), fc * 3 * sizeof(float)); dst += fc * 3 * sizeof(float);
            std::memcpy(dst, ch.rotations.data(), fc * 4 * sizeof(float)); dst += fc * 4 * sizeof(float);
            std::memcpy(dst, ch.scales.data(),    fc * 3 * sizeof(float)); dst += fc * 3 * sizeof(float);
        }

        const uint32_t ec = static_cast<uint32_t>(clip.events.size());
        std::memcpy(dst, &ec, sizeof(uint32_t)); dst += sizeof(uint32_t);
        for (const ClipAsset::AnimEvent& ev : clip.events)
        {
            std::memcpy(dst, &ev.time,     sizeof(float));    dst += sizeof(float);
            std::memcpy(dst, &ev.nameHash, sizeof(uint32_t)); dst += sizeof(uint32_t);
        }
    }

    // ==== Serialization helpers: morph clips ====
    // Per-morph-clip layout:
    //   uint32  channelCount, frameCount
    //   float   duration, frameRate
    //   char    name[64]
    //   For each channel:
    //     char  morphName[64]
    //     float weights[frameCount]
    static size_t MorphClipPayloadSize(const MorphClipData& clip)
    {
        size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        sz += clip.channels.size() * (64 + static_cast<size_t>(clip.frameCount) * sizeof(float));
        return sz;
    }

    static void WriteMorphClip(const MorphClipData& clip, uint8_t*& dst)
    {
        const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
        const uint32_t fc = clip.frameCount;
        std::memcpy(dst, &cc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &fc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &clip.duration,  sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, &clip.frameRate, sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, clip.name,       64);               dst += 64;

        for (const MorphClipChannel& ch : clip.channels)
        {
            std::memcpy(dst, ch.name, 64); dst += 64;
            std::memcpy(dst, ch.weights.data(), fc * sizeof(float)); dst += fc * sizeof(float);
        }
    }

    // ==== Serialization helpers: camera clips (matches AnimationLoader::ReadCameraClip) ====
    //   uint32 channelCount, frameCount; float duration, frameRate; char name[64]
    //   per channel: char name[64]; CameraAnimKey[frameCount]  (48B POD each)
    static size_t CameraClipPayloadSize(const CameraAnimClipData& clip)
    {
        size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        for (const auto& ch : clip.channels)
        {
            (void)ch;
            sz += 64;                                                       // channel name
            sz += static_cast<size_t>(clip.frameCount) * sizeof(CameraAnimKey);
        }
        return sz;
    }

    static void WriteCameraClip(const CameraAnimClipData& clip, uint8_t*& dst)
    {
        const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
        const uint32_t fc = clip.frameCount;
        std::memcpy(dst, &cc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &fc,             sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &clip.duration,  sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, &clip.frameRate, sizeof(float));    dst += sizeof(float);
        std::memcpy(dst, clip.name,       64);               dst += 64;
        for (const CameraAnimChannel& ch : clip.channels)
        {
            std::memcpy(dst, ch.name, 64); dst += 64;
            const size_t kBytes = static_cast<size_t>(fc) * sizeof(CameraAnimKey);
            if (ch.keys.size() >= fc) std::memcpy(dst, ch.keys.data(), kBytes);
            dst += kBytes;
        }
    }

    // ==== Read helpers: advance a cursor through the raw byte buffer safely ====
    template<typename T>
    static bool ReadValue(const uint8_t*& p, const uint8_t* end, T& out)
    {
        if (p + sizeof(T) > end) return false;
        std::memcpy(&out, p, sizeof(T));
        p += sizeof(T);
        return true;
    }

    static bool ReadBytes(const uint8_t*& p, const uint8_t* end, void* dst, size_t n)
    {
        if (p + n > end) return false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }

    // ==== VmdImporter::Import ====
    std::vector<uint8_t> VmdImporter::Import(const std::string&          sourcePath,
                                              const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("VmdImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        const uint8_t* p   = sourceData.data();
        const uint8_t* end = p + sourceData.size();

        // ---- Validate magic ----
        if (p + sizeof(VmdMagic) > end)
        {
            LOG_ERROR("VmdImporter: file too small to contain header '%s'", sourcePath.c_str());
            return {};
        }

        char magic[30];
        ReadBytes(p, end, magic, 30);

        const bool isVmd2 = (std::strncmp(magic, "Vocaloid Motion Data 0002", 25) == 0);
        const bool isVmd1 = (std::strncmp(magic, "Vocaloid Motion Data file", 25) == 0);

        if (!isVmd2 && !isVmd1)
        {
            LOG_ERROR("VmdImporter: unrecognised magic in '%s' (not a VMD file)", sourcePath.c_str());
            return {};
        }

        // ---- Model name (20 bytes for VMD2, 10 bytes for VMD1) ----
        const int modelNameLen = isVmd2 ? 20 : 10;
        char modelName[21] = {};
        if (!ReadBytes(p, end, modelName, static_cast<size_t>(modelNameLen)))
        {
            LOG_ERROR("VmdImporter: truncated header in '%s'", sourcePath.c_str());
            return {};
        }
        const std::string modelNameUtf8 = SjisToUtf8(modelName, modelNameLen);

        LOG_INFO("VmdImporter: '%s' — VMD%s, model='%s'",
                 sourcePath.c_str(), isVmd2 ? "2" : "1", modelNameUtf8.c_str());

        // ---- Bone keyframes ----
        uint32_t boneFrameCount = 0;
        if (!ReadValue(p, end, boneFrameCount))
        {
            LOG_ERROR("VmdImporter: truncated before bone frame count in '%s'", sourcePath.c_str());
            return {};
        }

        const size_t expectedBoneBytes = static_cast<size_t>(boneFrameCount) * sizeof(VmdBoneFrame);
        if (p + expectedBoneBytes > end)
        {
            LOG_ERROR("VmdImporter: bone frame data overflows file bounds in '%s'", sourcePath.c_str());
            return {};
        }

        // Accumulate per-bone sparse keyframes.
        // Key: UTF-8 bone name.  Value: keyframes sorted by frameNo.
        std::unordered_map<std::string, std::vector<BoneKey>> boneMap;
        boneMap.reserve(64);

        uint32_t maxFrame = 0;

        for (uint32_t i = 0; i < boneFrameCount; ++i)
        {
            VmdBoneFrame raw;
            ReadBytes(p, end, &raw, sizeof(VmdBoneFrame));  // bounds already checked above

            const std::string boneName = SjisToUtf8(raw.boneName, 15);
            if (boneName.empty()) continue;

            BoneKey key;
            key.frameNo = raw.frameNo;
            key.pos     = ConvertPos(raw.pos);
            key.rot     = ConvertRot(raw.rot);
            key.bezierX = UnpackBezier(raw.interpolation, 0);
            key.bezierY = UnpackBezier(raw.interpolation, 1);
            key.bezierZ = UnpackBezier(raw.interpolation, 2);
            key.bezierR = UnpackBezier(raw.interpolation, 3);

            boneMap[boneName].push_back(key);
            if (raw.frameNo > maxFrame)
                maxFrame = raw.frameNo;
        }

        // Do not bail when there are no bones: a camera/lighting VMD has zero bone (and morph) frames and
        // only a camera section. The import fails only if bone, morph and camera are all empty.
        if (boneMap.empty())
            LOG_INFO("VmdImporter: no bone keyframes in '%s' (camera/lighting VMD?)",
                     sourcePath.c_str());

        // Sort each bone's track by frame number (VMD files are not always ordered).
        for (auto& [name, keys] : boneMap)
            std::sort(keys.begin(), keys.end(),
                      [](const BoneKey& a, const BoneKey& b) { return a.frameNo < b.frameNo; });

        LOG_INFO("VmdImporter:   %u bone frames, %u unique bones, maxFrame=%u",
                 boneFrameCount, static_cast<unsigned>(boneMap.size()), maxFrame);

        // ---- Morph / face keyframes (optional section) ----
        std::unordered_map<std::string, std::vector<MorphKey>> morphMap;
        uint32_t maxMorphFrame = 0;

        if (p + sizeof(uint32_t) <= end)
        {
            uint32_t morphFrameCount = 0;
            ReadValue(p, end, morphFrameCount);

            const size_t expectedMorphBytes =
                static_cast<size_t>(morphFrameCount) * sizeof(VmdMorphFrame);

            if (p + expectedMorphBytes <= end)
            {
                morphMap.reserve(64);
                for (uint32_t i = 0; i < morphFrameCount; ++i)
                {
                    VmdMorphFrame raw;
                    ReadBytes(p, end, &raw, sizeof(VmdMorphFrame));

                    const std::string morphName = SjisToUtf8(raw.morphName, 15);
                    if (morphName.empty()) continue;

                    MorphKey key{ raw.frameNo, raw.weight };
                    morphMap[morphName].push_back(key);
                    if (raw.frameNo > maxMorphFrame)
                        maxMorphFrame = raw.frameNo;
                }

                // Sort by frame number
                for (auto& [name, keys] : morphMap)
                    std::sort(keys.begin(), keys.end(),
                              [](const MorphKey& a, const MorphKey& b) {
                                  return a.frameNo < b.frameNo; });

                LOG_INFO("VmdImporter:   %u morph frames, %u unique targets, maxMorphFrame=%u",
                         morphFrameCount, static_cast<unsigned>(morphMap.size()), maxMorphFrame);
            }
            else
            {
                LOG_WARNING("VmdImporter: morph frame data overflows file bounds — skipped");
            }
        }

        // Unified max frame (bone and morph tracks share timeline)
        maxFrame = std::max(maxFrame, maxMorphFrame);

        // ---- Camera keyframes (optional section, after morph) ----
        // Present only in camera/lighting VMDs; a model-motion VMD has a zero count or ends here.
        std::vector<CameraRawKey>     camKeys;
        std::unordered_set<uint32_t>  camCutFrames; // duplicate-frameNo == hard cut
        uint32_t                      maxCamFrame = 0;
        bool                          anyOrtho    = false;

        if (p + sizeof(uint32_t) <= end)
        {
            uint32_t camFrameCount = 0;
            ReadValue(p, end, camFrameCount);

            const size_t expectedCamBytes =
                static_cast<size_t>(camFrameCount) * sizeof(VmdCameraFrame);

            if (camFrameCount > 0 && p + expectedCamBytes <= end)
            {
                camKeys.reserve(camFrameCount);
                for (uint32_t i = 0; i < camFrameCount; ++i)
                {
                    VmdCameraFrame raw;
                    ReadBytes(p, end, &raw, sizeof(VmdCameraFrame));

                    CameraRawKey k;
                    k.frameNo  = raw.frameNo;
                    k.distance = raw.distance;
                    k.interest = ConvertPos(raw.interest);   // identity MMD->engine
                    k.euler    = { raw.rotation[0], raw.rotation[1], raw.rotation[2] };
                    k.fovDeg   = static_cast<float>(raw.viewAngle);
                    k.ortho    = raw.perspective;
                    for (int c = 0; c < 6; ++c)
                        k.bz[c] = UnpackCameraBezier(raw.interpolation, c);
                    if (k.ortho) anyOrtho = true;

                    camKeys.push_back(k);
                    if (raw.frameNo > maxCamFrame) maxCamFrame = raw.frameNo;
                }

                // Stable sort by frameNo so duplicate-frame CUTs keep file order
                // (the SECOND duplicate is the post-cut state).
                std::stable_sort(camKeys.begin(), camKeys.end(),
                    [](const CameraRawKey& a, const CameraRawKey& b) { return a.frameNo < b.frameNo; });

                for (size_t i = 1; i < camKeys.size(); ++i)
                    if (camKeys[i].frameNo == camKeys[i - 1].frameNo)
                        camCutFrames.insert(camKeys[i].frameNo);

                LOG_INFO("VmdImporter:   %u camera frames, maxCamFrame=%u, cuts=%u%s",
                         camFrameCount, maxCamFrame,
                         static_cast<unsigned>(camCutFrames.size()),
                         anyOrtho ? " (orthographic frames present — approximated as perspective)" : "");
            }
            else if (camFrameCount > 0)
            {
                LOG_WARNING("VmdImporter: camera frame data overflows file bounds — skipped");
            }
        }

        // Optional light, self-shadow and show-IK sections. Old VMDs may end
        // at any section boundary; reject counts before advancing pointers.
        std::vector<IKEnableTrack> ikTracks;
        bool tailOK = true;
        for (size_t stride : {size_t(28), size_t(9)}) {
            if (p == end) { tailOK = false; break; }
            uint32_t count = 0;
            if (!ReadValue(p, end, count) || count > size_t(end - p) / stride) { tailOK = false; break; }
            p += count * stride;
        }
        if (tailOK && size_t(end - p) >= 4) {
            uint32_t frames = 0; ReadValue(p, end, frames);
            if (frames > size_t(end - p) / 9) tailOK = false;
            for (uint32_t f = 0; tailOK && f < frames; ++f) {
                uint32_t frame = 0, count = 0; uint8_t show = 1;
                tailOK = ReadValue(p, end, frame) && ReadValue(p, end, show) && ReadValue(p, end, count);
                if (!tailOK || count > size_t(end - p) / 21) { tailOK = false; break; }
                for (uint32_t i = 0; i < count; ++i) {
                    char name[20]; uint8_t enabled = 1;
                    ReadBytes(p, end, name, 20); ReadValue(p, end, enabled);
                    const auto utf8 = SjisToUtf8(name, 20);
                    auto it = std::find_if(ikTracks.begin(), ikTracks.end(),
                        [&](const auto& t) { return t.name == utf8; });
                    if (it == ikTracks.end()) { ikTracks.push_back({utf8}); it = ikTracks.end() - 1; }
                    it->keys.push_back({frame, enabled});
                }
            }
            if (!tailOK) { ikTracks.clear(); LOG_WARNING("VmdImporter: truncated show-IK tail"); }
            for (auto& track : ikTracks) std::stable_sort(track.keys.begin(), track.keys.end(),
                [](const auto& a, const auto& b) { return a.frame < b.frame; });
        }
        const bool hasBone   = !boneMap.empty();
        const bool hasCamera = !camKeys.empty();
        if (!hasBone && morphMap.empty() && !hasCamera)
        {
            LOG_WARNING("VmdImporter: '%s' has no bone, morph, or camera keyframes",
                        sourcePath.c_str());
            return {};
        }

        // ---- Build AnimClipData (bone) ----
        const std::string stem = Sys::PathToUtf8(Sys::U8Path(sourcePath).stem());

        AnimClipData clip;
        strncpy_s(clip.name, stem.empty() ? "vmd_clip" : stem.c_str(), 64);
        clip.frameRate  = 30.f;
        clip.frameCount = maxFrame + 1;
        clip.duration   = static_cast<float>(clip.frameCount) / clip.frameRate;
        clip.positionsAreOffsets = true;
        clip.rotationsAreOffsets = false;
        clip.channels.reserve(boneMap.size());

        for (auto& [boneName, keys] : boneMap)
        {
            AnimClipChannel ch;
            strncpy_s(ch.name, boneName.c_str(), _TRUNCATE);
            ch.positions.resize(clip.frameCount);
            ch.rotations.resize(clip.frameCount);
            ch.scales.assign(clip.frameCount, { 1.f, 1.f, 1.f });
            for (uint32_t fi = 0; fi < clip.frameCount; ++fi)
                SampleKeys(keys, static_cast<float>(fi), ch.positions[fi], ch.rotations[fi]);
            clip.channels.push_back(std::move(ch));
        }

        if (hasBone)
            LOG_INFO("VmdImporter:   bone clip '%s' — %u frames, %.2fs, %u channels",
                     clip.name, clip.frameCount, clip.duration,
                     static_cast<unsigned>(clip.channels.size()));

        // ---- Build MorphClipData (morph) ----
        MorphClipData morphClip;
        strncpy_s(morphClip.name, stem.empty() ? "vmd_clip" : stem.c_str(), 64);
        morphClip.frameRate  = 30.f;
        morphClip.frameCount = clip.frameCount;  // same timeline
        morphClip.duration   = clip.duration;
        morphClip.channels.reserve(morphMap.size());

        for (auto& [morphName, keys] : morphMap)
        {
            MorphClipChannel ch;
            strncpy_s(ch.name, morphName.c_str(), _TRUNCATE);
            ch.weights.resize(morphClip.frameCount, 0.f);
            for (uint32_t fi = 0; fi < morphClip.frameCount; ++fi)
                ch.weights[fi] = SampleMorphKeys(keys, static_cast<float>(fi));
            morphClip.channels.push_back(std::move(ch));
        }

        if (!morphClip.channels.empty())
            LOG_INFO("VmdImporter:   morph clip — %u targets",
                     static_cast<unsigned>(morphClip.channels.size()));

        // ---- Build CameraAnimClipData (camera) ----
        // Bezier-interpolate the raw MMD parameters per frame, then normalize each baked frame into the
        // engine's format-agnostic CameraAnimKey.
        CameraAnimClipData camClip;
        if (hasCamera)
        {
            strncpy_s(camClip.name, stem.empty() ? "vmd_camera" : stem.c_str(), 64);
            camClip.frameRate  = 30.f;
            camClip.frameCount = maxCamFrame + 1;
            camClip.duration   = static_cast<float>(camClip.frameCount) / camClip.frameRate;

            CameraAnimChannel ch;
            strncpy_s(ch.name, "camera", _TRUNCATE);
            ch.keys.resize(camClip.frameCount);
            for (uint32_t fi = 0; fi < camClip.frameCount; ++fi)
            {
                const CameraSample s = SampleCameraKeys(camKeys, static_cast<float>(fi));
                const bool isCut = camCutFrames.find(fi) != camCutFrames.end();
                ch.keys[fi] = MmdCameraToPose(s, isCut);
            }
            camClip.channels.push_back(std::move(ch));

            LOG_INFO("VmdImporter:   camera clip '%s' — %u frames, %.2fs",
                     camClip.name, camClip.frameCount, camClip.duration);
        }

        const bool hasMorph = !morphClip.channels.empty();

        AnimationResource resource;
        clip.ikEnableTracks = std::move(ikTracks);
        if (hasBone) resource.clips.push_back(std::move(clip));
        if (hasMorph) resource.morphClips.push_back(std::move(morphClip));
        if (hasCamera) resource.cameraClips.push_back(std::move(camClip));
        return SerializeAnimation(resource);
    }

} // namespace Resource
