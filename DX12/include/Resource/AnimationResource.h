#pragma once

// Animation data as loaded from one .ianim file, addressed by bone / target name:
//   AnimClipChannel   - one bone's keyframe track, identified by bone name
//   AnimClipData      - a clip of named channels, not yet bound to a skeleton
//   MorphClipData     - a morph-weight clip; CameraAnimClipData - a camera clip
//   AnimationResource - every clip of one file
// Use: AnimationClipSystem::AcquireClip loads the .ianim, AnimClipData::BindToSkeleton
// makes a ClipAsset for one skeleton, ClipLibrary::Register returns the index
// AnimationComponent::primaryClip takes.

#include "Resource/Resource.h"
#include "Resource/SkeletonAsset.h"  // ClipAsset, SkeletonAsset, AnimEvent

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{
    // The hash a clip's channels and a skeleton's bones are matched by: FNV-32
    // of the name's exact bytes - case-sensitive ("Hips" is not "hips"), and a
    // Japanese (PMX / VMD) name never matches an English one.
    inline uint32_t BoneNameHash(const char* s)
    {
        uint32_t h = 2166136261u;
        for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
        return h;
    }

    // Which clip of a multi-clip source an AnimationSet entry's `bone` selector
    // names: "" = the first, "#N" = index N (unnamed or duplicate names),
    // otherwise the clip whose name is exactly the selector; -1 = none.
    // AnimationClipSystem resolves aliases with it, so a tool that picks a clip
    // this way picks the one playback will.
    template <typename ClipVector>
    int32_t FindSourceClip(const ClipVector& clips, const std::string& selector)
    {
        if (clips.empty()) return -1;
        if (selector.empty()) return 0;
        if (selector.size() > 1 && selector.front() == '#')
        {
            uint32_t index = 0;
            const char* first = selector.data() + 1;
            const char* last  = selector.data() + selector.size();
            const auto parsed = std::from_chars(first, last, index);
            if (parsed.ec == std::errc{} && parsed.ptr == last && index < clips.size())
                return static_cast<int32_t>(index);
        }
        for (size_t i = 0; i < clips.size(); ++i)
            if (selector == clips[i].name)
                return static_cast<int32_t>(i);
        return -1;
    }

    // One bone's full keyframe track; frameCount == the parent AnimClipData::frameCount.
    struct AnimClipChannel
    {
        char name[64] = {};  // bone name, matched to a skeleton bone

        // Parallel arrays; each has frameCount entries.
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT4> rotations;  // quaternion xyzw
        std::vector<DirectX::XMFLOAT3> scales;
    };

    // One animation clip with named channels, not bound to a skeleton:
    // BindToSkeleton() resolves the bone order.
    struct AnimClipData
    {
        char     name[64]   = {};   // clip name (e.g. "Walk", "Run_01")
        uint32_t frameCount = 0;
        float    duration   = 0.f;  // clip length in seconds
        float    frameRate  = 30.f;

        // VMD clips store position / rotation as offsets from the rest pose, FBX / glTF
        // clips absolute local transforms. With these flags set, BindToSkeleton adds the
        // positions to and composes the rotations with the rest pose instead of replacing it.
        bool positionsAreOffsets = false;
        bool rotationsAreOffsets = false;

        // Retargeted clip with FK-baked legs: BindToSkeleton stamps the ClipAsset
        // LegDriveMode::FK so IKSystem skips the MMD leg CCD. Stored as the file-level
        // ANIM_FLAG_LEG_FK in .ianim.
        bool legDriveFK = false;

        std::vector<AnimClipChannel>      channels;
        std::vector<ClipAsset::AnimEvent> events;

        // Authored per-clip tracks, copied into the bound ClipAsset: IK enable tracks
        // (matched to the skeleton's IK chains at bind), AnimNotify tracks (time-based,
        // skeleton-independent; the .ianim notify section), root motion and float curves.
        std::vector<IKEnableTrack> ikEnableTracks;
        std::vector<NotifyTrack> notifyTracks;
    std::vector<Anim::RootMotionKey> rootMotion;
    std::vector<Anim::FloatCurve> floatCurves;
        uint32_t                 nextNotifyId = 1; // editor id allocator

        // Which channel drives each of @p skeleton's bones: [bi] = the channel
        // index, or -1 - the bone keeps its rest pose. The rule is
        // BindToSkeleton's own (BoneNameHash of the exact names; of two
        // channels with one name the later wins; a channel no bone takes is
        // never read, silently), so a check made with it is what playback
        // does - the offline asset tool's `compat`.
        std::vector<int32_t> ChannelBinding(const SkeletonAsset& skeleton) const;

        // Returns a ClipAsset bound to @p skeleton (boneCount == skeleton.boneCount, ready
        // for ClipLibrary). Channels match bones by name (ChannelBinding); a bone without a
        // channel keeps its rest pose. @p boundBones (optional) receives how many bones a
        // channel drives - 0 means the clip plays the rest pose, and nothing else says so.
        ClipAsset BindToSkeleton(const SkeletonAsset& skeleton, uint32_t* boundBones = nullptr) const;
    };

    // One morph target's weight track (dense); frameCount == the parent MorphClipData::frameCount.
    struct MorphClipChannel
    {
        char name[64] = {};          // morph target name, matched against the mesh at bind time
        std::vector<float> weights;  // frameCount entries in [0, 1]
    };

    // One morph / face animation clip with named morph-target channels, not bound to
    // a mesh: the names are resolved against the mesh at runtime.
    struct MorphClipData
    {
        char     name[64]   = {};   // clip name (matches paired bone clip name)
        uint32_t frameCount = 0;
        float    duration   = 0.f;
        float    frameRate  = 30.f;
        std::vector<MorphClipChannel> channels;

        // Convert to a runtime SOA MorphClipAsset ready for MorphClipLibrary.
        MorphClipAsset ToRuntimeClip() const;
    };

    // Camera animation, format-agnostic. Importers (VMD, FBX / glTF, ...) normalize
    // their camera model into one representation: an absolute per-frame world-space
    // pose in the engine's left-handed Y-up convention (+Z forward, +Y up) with a
    // vertical FOV in radians. Every format quirk (MMD's interest + distance + euler,
    // FBX's focal length / sensor / aim node, handedness) is resolved at import, so one
    // runtime player serves every format.
    //
    // Keys are dense (one per frame, like AnimClipChannel); the runtime lerps position
    // and nlerps rotation between adjacent keys. A clip usually has one channel (one
    // camera); FBX scenes may carry several.
    struct CameraAnimKey
    {
        DirectX::XMFLOAT3 position;    // engine world space (before any stage-root transform)
        DirectX::XMFLOAT4 rotation;    // quaternion; +Z = forward, +Y = up
        float    fovY        = 0.f;    // vertical FOV, radians (perspective)
        float    nearZ       = 0.1f;
        float    farZ        = 1000.f;
        float    orthoHeight = 0.f;    // >0 => orthographic (view height); 0 => perspective
        uint32_t flags       = 0;      // see CAMERA_KEY_* below
    };
    static_assert(sizeof(CameraAnimKey) == 48,
                  "CameraAnimKey must be 48 bytes (no padding) — it is blob-copied verbatim");

    // Hard cut at this frame: temporal-history-bearing passes (TAA, SSR, GTAO,
    // VolumetricFog) must clear for one frame so a shot boundary doesn't smear.
    constexpr uint32_t CAMERA_KEY_CUT = 0x1;

    struct CameraAnimChannel
    {
        char name[64] = {};               // source camera node name (informational)
        std::vector<CameraAnimKey> keys;  // frameCount entries (dense)
    };

    struct CameraAnimClipData
    {
        char     name[64]   = {};
        uint32_t frameCount = 0;
        float    duration   = 0.f;     // seconds
        float    frameRate  = 30.f;    // 30 for VMD; source-native otherwise
        std::vector<CameraAnimChannel> channels;
    };

    // All clips of one .ianim source: any mix of bone, morph and camera tracks.
    // Owned by ResourceManager; reached through an AnimHandle from AnimationClipSystem.
    class AnimationResource : public Resource
    {
    public:
        std::vector<AnimClipData>       clips;       // bone animation clips
        std::vector<MorphClipData>      morphClips;  // morph/face animation clips
        std::vector<CameraAnimClipData> cameraClips; // camera animation clips (format-agnostic)
    };

    // ==== Inline implementations ====
    inline MorphClipAsset MorphClipData::ToRuntimeClip() const
    {
        MorphClipAsset out;
        out.morphCount = static_cast<uint32_t>(
            (std::min)(channels.size(),
                     static_cast<size_t>(MorphClipAsset::MAX_MORPHS)));
        out.frameCount = frameCount;
        out.duration   = duration;
        out.frameRate  = frameRate;
        strncpy_s(out.name, name, _TRUNCATE);

        out.weights.assign(static_cast<size_t>(out.morphCount) * frameCount, 0.f);

        for (uint32_t mi = 0; mi < out.morphCount; ++mi)
        {
            const MorphClipChannel& ch = channels[mi];
			strncpy_s(out.morphNames[mi], ch.name, 64);

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(mi) * frameCount + fi;
                out.weights[idx] = (fi < static_cast<uint32_t>(ch.weights.size()))
                                   ? ch.weights[fi] : 0.f;
            }
        }
        return out;
    }

    inline std::vector<int32_t> AnimClipData::ChannelBinding(const SkeletonAsset& skeleton) const
    {
        // Channel name-hash -> channel index (a repeated name: the later wins).
        std::unordered_map<uint32_t, uint32_t> hashToChannel;
        hashToChannel.reserve(channels.size());
        for (uint32_t ci = 0; ci < static_cast<uint32_t>(channels.size()); ++ci)
            hashToChannel[BoneNameHash(channels[ci].name)] = ci;

        std::vector<int32_t> binding(skeleton.boneCount, -1);
        for (uint32_t bi = 0; bi < skeleton.boneCount; ++bi)
        {
            const auto it = hashToChannel.find(BoneNameHash(skeleton.boneNames[bi]));
            if (it != hashToChannel.end()) binding[bi] = static_cast<int32_t>(it->second);
        }
        return binding;
    }

    inline ClipAsset AnimClipData::BindToSkeleton(const SkeletonAsset& skeleton, uint32_t* boundBones) const
    {
        using namespace DirectX;

        ClipAsset out;
        out.boneCount    = skeleton.boneCount;
        out.frameCount   = frameCount;
        out.duration     = duration;
        out.frameRate    = frameRate;
        out.events       = events;
        // Retargeted clips carry FK-baked legs; IKSystem skips the leg CCD.
        out.legDriveMode = legDriveFK ? LegDriveMode::FK : LegDriveMode::IK;
        // IK enable tracks are matched to the skeleton's IK chains by canonical name;
        // notifies are time-based and copied verbatim, so the runtime clip drives
        // TimelineSystem for any entity that plays it.
        out.ikEnableTracks = ikEnableTracks;
        for (auto& track : out.ikEnableTracks)
            for (uint32_t i = 0; i < skeleton.ikChains.size(); ++i)
                if (Anim::IK::CanonicalIKName(skeleton.boneNames[skeleton.ikChains[i].ikBoneIndex])
                    == Anim::IK::CanonicalIKName(track.name)) { track.chainIndex = i; break; }
        out.notifyTracks = notifyTracks;
        out.rootMotion = rootMotion; out.floatCurves = floatCurves;
        out.nextNotifyId = nextNotifyId;

        const size_t kf = static_cast<size_t>(out.boneCount) * out.frameCount;
        out.positions.resize(kf);
        out.rotations.resize(kf);
        out.scales   .resize(kf);

        // Every bone starts at its rest-pose local TRS, so a bone no channel keys (VMD,
        // partial FBX clips) stays at rest instead of collapsing to its parent's origin and
        // pulling its vertices to the root.
        for (uint32_t bi = 0; bi < skeleton.boneCount; ++bi)
        {
            XMVECTOR sc, rot, tr;
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            XMFLOAT3 restScl = { 1.f, 1.f, 1.f };

            if (XMMatrixDecompose(&sc, &rot, &tr,
                                  XMLoadFloat4x4(&skeleton.restPoseLocal[bi])))
            {
                XMStoreFloat3(&restPos, tr);
                XMStoreFloat4(&restRot, rot);
                XMStoreFloat3(&restScl, sc);
            }

            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(bi) * frameCount + fi;
                out.positions[idx] = restPos;
                out.rotations[idx] = restRot;
                out.scales[idx]    = restScl;
            }
        }

        // Overwrite the rest-pose defaults with the keyframes of animated bones. With
        // positionsAreOffsets / rotationsAreOffsets set (VMD clips) the channel values are
        // offsets from the rest pose, not absolute local TRS.
        const std::vector<int32_t> binding = ChannelBinding(skeleton);
        uint32_t bound = 0;
        for (uint32_t bi = 0; bi < skeleton.boneCount; ++bi)
        {
            if (binding[bi] < 0) continue;
            ++bound;

            // Decompose rest pose for offset composition
            XMFLOAT3 restPos = { 0.f, 0.f, 0.f };
            XMFLOAT4 restRot = { 0.f, 0.f, 0.f, 1.f };
            if (positionsAreOffsets || rotationsAreOffsets)
            {
                XMVECTOR sc, rot, tr;
                if (XMMatrixDecompose(&sc, &rot, &tr,
                                      XMLoadFloat4x4(&skeleton.restPoseLocal[bi])))
                {
                    XMStoreFloat3(&restPos, tr);
                    XMStoreFloat4(&restRot, rot);
                }
            }

            const AnimClipChannel& ch = channels[static_cast<size_t>(binding[bi])];
            for (uint32_t fi = 0; fi < frameCount; ++fi)
            {
                const size_t idx = static_cast<size_t>(bi) * frameCount + fi;

                if (fi < static_cast<uint32_t>(ch.positions.size()))
                {
                    if (positionsAreOffsets)
                    {
                        out.positions[idx] = { restPos.x + ch.positions[fi].x,
                                               restPos.y + ch.positions[fi].y,
                                               restPos.z + ch.positions[fi].z };
                    }
                    else
                    {
                        out.positions[idx] = ch.positions[fi];
                    }
                }

                if (fi < static_cast<uint32_t>(ch.rotations.size()))
                {
                    if (rotationsAreOffsets)
                    {
                        XMVECTOR qRest = XMLoadFloat4(&restRot);
                        XMVECTOR qAnim = XMLoadFloat4(&ch.rotations[fi]);
                        XMStoreFloat4(&out.rotations[idx],
                                      XMQuaternionMultiply(qRest, qAnim));
                    }
                    else
                    {
                        out.rotations[idx] = ch.rotations[fi];
                    }
                }

                if (fi < static_cast<uint32_t>(ch.scales.size()))
                    out.scales[idx] = ch.scales[fi];
            }
        }

        if (boundBones) *boundBones = bound;
        return out;
    }
}
