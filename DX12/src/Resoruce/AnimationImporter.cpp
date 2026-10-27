#include "Resource/AnimationImporter.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/camera.h>
#include <assimp/postprocess.h>
#include <assimp/anim.h>

#ifdef _DEBUG
#pragma comment(lib, "assimp-vc143-mtd.lib")
#else
#pragma comment(lib, "assimp-vc143-mt.lib")
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>
#include <DirectXMath.h>

namespace Resource
{
    // ==== Interpolation helpers ====
    // The same algorithm as SkeletonImporter::BuildClip.
    static DirectX::XMFLOAT3 SamplePos(const aiVectorKey* keys, unsigned count, double t)
    {
        if (count == 0) return { 0.f, 0.f, 0.f };
        if (count == 1) return { keys[0].mValue.x, keys[0].mValue.y, keys[0].mValue.z };

        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return { keys[count - 1].mValue.x, keys[count - 1].mValue.y, keys[count - 1].mValue.z };

        const unsigned lo = hi;
        hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;

        return {
            keys[lo].mValue.x + f * (keys[hi].mValue.x - keys[lo].mValue.x),
            keys[lo].mValue.y + f * (keys[hi].mValue.y - keys[lo].mValue.y),
            keys[lo].mValue.z + f * (keys[hi].mValue.z - keys[lo].mValue.z)
        };
    }

    static DirectX::XMFLOAT4 SampleRot(const aiQuatKey* keys, unsigned count, double t)
    {
        if (count == 0) return { 0.f, 0.f, 0.f, 1.f };
        if (count == 1) return { keys[0].mValue.x, keys[0].mValue.y, keys[0].mValue.z, keys[0].mValue.w };

        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return { keys[count - 1].mValue.x, keys[count - 1].mValue.y, keys[count - 1].mValue.z, keys[count - 1].mValue.w };

        const unsigned lo = hi;
        hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;

        aiQuaternion q;
        aiQuaternion::Interpolate(q, keys[lo].mValue, keys[hi].mValue, f);
        q.Normalize();
        return { q.x, q.y, q.z, q.w };
    }

    // Builds one AnimClipData from an aiAnimation. Channels are stored by bone name,
    // not skeleton bone index, so the clip binds to any compatible skeleton via
    // BindToSkeleton().
    static AnimClipData BuildAnimClipData(const aiAnimation* anim, float sampleRate)
    {
        AnimClipData clip;

        const char* rawName = anim->mName.C_Str();
        strncpy_s(clip.name, rawName[0] ? rawName : "unnamed", 64);

        // Assimp times are in ticks (25 per second when unset); convert to seconds.
        const double ticksPerSec = (anim->mTicksPerSecond > 1e-9)
            ? anim->mTicksPerSecond : 25.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        const double requestedRate = sampleRate == 0.f ? anim->mTicksPerSecond : sampleRate;
        clip.frameRate = std::isfinite(requestedRate) && requestedRate >= 1.0 && requestedRate <= 240.0
            ? static_cast<float>(requestedRate) : 30.f;
        clip.duration   = static_cast<float>(durationSec);
        clip.frameCount = std::max(1u, static_cast<uint32_t>(
            std::ceil(durationSec * clip.frameRate)) + 1u);

        clip.channels.resize(anim->mNumChannels);

        for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
        {
            const aiNodeAnim* ch = anim->mChannels[ci];
            AnimClipChannel& out = clip.channels[ci];

            strncpy_s(out.name, ch->mNodeName.C_Str(), 64);
            out.positions.resize(clip.frameCount);
            out.rotations.resize(clip.frameCount);
            out.scales.resize(clip.frameCount);

            for (uint32_t fi = 0; fi < clip.frameCount; ++fi)
            {
                const double t = (fi / static_cast<double>(clip.frameRate)) * ticksPerSec;
                out.positions[fi] = SamplePos(ch->mPositionKeys, ch->mNumPositionKeys, t);
                out.rotations[fi] = SampleRot(ch->mRotationKeys, ch->mNumRotationKeys, t);
                out.scales[fi]    = SamplePos(ch->mScalingKeys,  ch->mNumScalingKeys,  t);
            }
        }

        return clip;
    }

    // ==== Bone clip serialization ====
    // Per bone clip:
    //   uint32 channelCount, frameCount
    //   float  duration, frameRate
    //   char   name[64]
    //   per channel: char boneName[64]; float3 positions[frameCount];
    //                float4 rotations[frameCount]; float3 scales[frameCount]
    //   uint32 eventCount; per event: float time; uint32 nameHash
    static size_t ClipPayloadSize(const AnimClipData& clip)
    {
        size_t sz = 0;
        sz += 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        for (const auto& ch : clip.channels)
        {
            sz += 64; // boneName
            sz += clip.frameCount * (3 + 4 + 3) * sizeof(float);
        }
        sz += sizeof(uint32_t); // eventCount
        sz += clip.events.size() * (sizeof(float) + sizeof(uint32_t));
        return sz;
    }

    static void WriteClip(const AnimClipData& clip, uint8_t*& dst)
    {
        const uint32_t cc = static_cast<uint32_t>(clip.channels.size());
        const uint32_t fc = clip.frameCount;
        std::memcpy(dst, &cc,           sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &fc,           sizeof(uint32_t)); dst += sizeof(uint32_t);
        std::memcpy(dst, &clip.duration,  sizeof(float));  dst += sizeof(float);
        std::memcpy(dst, &clip.frameRate, sizeof(float));  dst += sizeof(float);
        std::memcpy(dst, clip.name,       64);             dst += 64;

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

    // ==== Morph clip serialization ====
    // Per morph clip: uint32 channelCount, frameCount; float duration, frameRate;
    // char name[64]; per channel: char morphName[64]; float weights[frameCount]
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

    // ==== Camera clip serialization ====
    // Read back by AnimationLoader::ReadCameraClip: uint32 channelCount, frameCount;
    // float duration, frameRate; char name[64]; per channel: char name[64];
    // CameraAnimKey[frameCount] (48-byte POD each)
    static size_t CameraClipPayloadSize(const CameraAnimClipData& clip)
    {
        size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64; // header
        for (const auto& ch : clip.channels)
        {
            (void)ch;
            sz += 64;
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

    // Extracts the morph-weight animation of one aiAnimation, sampled at frameRate.
    // Returns an empty MorphClipData (no channels) when it has no morph tracks.
    static MorphClipData BuildMorphClipData(const aiAnimation* anim,
                                             const aiScene*     scene,
                                             float              frameRate)
    {
        MorphClipData clip;
        if (anim->mNumMorphMeshChannels == 0) return clip;

        const double ticksPerSec = (anim->mTicksPerSecond > 1e-9)
            ? anim->mTicksPerSecond : 25.0;
        const double durationSec = anim->mDuration / ticksPerSec;

        const char* rawName = anim->mName.C_Str();
        strncpy_s(clip.name, rawName[0] ? rawName : "unnamed", 64);
        clip.frameRate  = frameRate;
        clip.frameCount = std::max(1u, static_cast<uint32_t>(
            std::ceil(durationSec * frameRate)));
        clip.duration   = static_cast<float>(durationSec);

        for (unsigned mi = 0; mi < anim->mNumMorphMeshChannels; ++mi)
        {
            const aiMeshMorphAnim* mma = anim->mMorphMeshChannels[mi];
            if (!mma || mma->mNumKeys == 0) continue;

            // The mesh whose target list the keys' indices address - its targets carry the
            // names the model's .imorph uses. A mesh of the channel's name with targets, else
            // the node of that name -> the first of its meshes with targets: glTF names a morph
            // channel after its node, and assimp splits that node's mesh into one aiMesh per
            // primitive ("<mesh>-<p>"), all sharing the list. Without a mesh the channels get
            // index names ("morph_N") that bind to nothing on the rig.
            const aiMesh* mesh = nullptr;
            for (unsigned si = 0; si < scene->mNumMeshes && !mesh; ++si)
                if (scene->mMeshes[si] && scene->mMeshes[si]->mNumAnimMeshes > 0 &&
                    std::strcmp(scene->mMeshes[si]->mName.C_Str(), mma->mName.C_Str()) == 0)
                    mesh = scene->mMeshes[si];
            if (!mesh && scene->mRootNode)
                if (const aiNode* node = scene->mRootNode->FindNode(mma->mName))
                    for (unsigned k = 0; k < node->mNumMeshes && !mesh; ++k)
                    {
                        const unsigned m = node->mMeshes[k];
                        if (m < scene->mNumMeshes && scene->mMeshes[m] && scene->mMeshes[m]->mNumAnimMeshes > 0)
                            mesh = scene->mMeshes[m];
                    }
            if (!mesh)
                LOG_WARNING("AnimationImporter: morph channel '%s' names no mesh or node with shape keys - its "
                            "targets keep index names (morph_N) and bind to nothing", mma->mName.C_Str());

            // One channel per target index, up to the highest index any key mentions.
            uint32_t maxTargetIdx = 0;
            for (unsigned ki = 0; ki < mma->mNumKeys; ++ki)
                for (unsigned ti = 0; ti < mma->mKeys[ki].mNumValuesAndWeights; ++ti)
                    if (mma->mKeys[ki].mValues[ti] > maxTargetIdx)
                        maxTargetIdx = mma->mKeys[ki].mValues[ti];

            const uint32_t numTargets = maxTargetIdx + 1;

            for (uint32_t ti = 0; ti < numTargets; ++ti)
            {
                MorphClipChannel ch;
                // Name: the target's own (cut to 63 bytes, as SceneImporter cuts
                // it), else "morph_<index>" - never one shared "morph" for every
                // unnamed target.
                const char* tname = (mesh && ti < mesh->mNumAnimMeshes && mesh->mAnimMeshes[ti])
                                  ? mesh->mAnimMeshes[ti]->mName.C_Str() : nullptr;
                if (tname && tname[0])
                    strncpy_s(ch.name, tname, _TRUNCATE);
                else
                {
                    char fallback[64];
                    std::snprintf(fallback, sizeof(fallback), "morph_%u", ti);
                    strncpy_s(ch.name, fallback, _TRUNCATE);
                }
                // One channel per NAME: a second channel over the same targets
                // (another primitive of the node, another split of the mesh) would
                // bind the same morph twice - the runtime drives names.
                if (std::any_of(clip.channels.begin(), clip.channels.end(),
                                [&](const MorphClipChannel& c) { return std::strcmp(c.name, ch.name) == 0; }))
                    continue;

                ch.weights.resize(clip.frameCount, 0.f);

                // Sample this target's weight track at each frame
                for (uint32_t fi = 0; fi < clip.frameCount; ++fi)
                {
                    const double t = (fi / static_cast<double>(frameRate)) * ticksPerSec;
                    // Find surrounding keyframes
                    unsigned lo = 0;
                    while (lo + 1 < mma->mNumKeys && mma->mKeys[lo + 1].mTime <= t) ++lo;
                    const unsigned hi = lo + 1;

                    // Extract weights for target ti from lo and hi keys
                    auto GetWeight = [&](const aiMeshMorphKey& key) -> float {
                        for (unsigned w = 0; w < key.mNumValuesAndWeights; ++w)
                            if (key.mValues[w] == ti)
                                return static_cast<float>(key.mWeights[w]);
                        return 0.f;
                    };

                    if (hi >= mma->mNumKeys)
                    {
                        ch.weights[fi] = GetWeight(mma->mKeys[lo]);
                    }
                    else
                    {
                        const double span = mma->mKeys[hi].mTime - mma->mKeys[lo].mTime;
                        const float  alpha = (span > 1e-9) ? static_cast<float>((t - mma->mKeys[lo].mTime) / span) : 0.f;
                        const float  w0 = GetWeight(mma->mKeys[lo]);
                        const float  w1 = GetWeight(mma->mKeys[hi]);
                        ch.weights[fi] = w0 + alpha * (w1 - w0);
                    }
                }

                clip.channels.push_back(std::move(ch));
            }
        }
        return clip;
    }

    // ==== Camera animation extraction ====
    // Reads aiScene::mCameras and the camera node's animated world transform and
    // normalizes them into the engine's generic CameraAnimKey (the asset a VMD camera
    // produces too). Assimp data is already ConvertToLeftHanded, so it lands in the
    // engine's LH space directly.
    static aiVector3D SampleAiVec(const aiVectorKey* keys, unsigned count, double t)
    {
        if (count == 0) return aiVector3D(0.f, 0.f, 0.f);
        if (count == 1) return keys[0].mValue;
        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return keys[count - 1].mValue;
        const unsigned lo = hi; hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;
        return keys[lo].mValue + (keys[hi].mValue - keys[lo].mValue) * f;
    }

    static aiQuaternion SampleAiQuat(const aiQuatKey* keys, unsigned count, double t)
    {
        if (count == 0) return aiQuaternion(1.f, 0.f, 0.f, 0.f);
        if (count == 1) return keys[0].mValue;
        unsigned hi = 0;
        while (hi < count - 1 && keys[hi + 1].mTime <= t) ++hi;
        if (hi >= count - 1) return keys[count - 1].mValue;
        const unsigned lo = hi; hi = lo + 1;
        const double span = keys[hi].mTime - keys[lo].mTime;
        const float  f    = (span > 1e-9) ? static_cast<float>((t - keys[lo].mTime) / span) : 0.f;
        aiQuaternion q;
        aiQuaternion::Interpolate(q, keys[lo].mValue, keys[hi].mValue, f);
        q.Normalize();
        return q;
    }

    // Node's LOCAL transform at time t: the animated TRS if a channel targets it,
    // else its static bind transform.
    static aiMatrix4x4 NodeLocalAt(const aiNode*                                          node,
                                   const std::unordered_map<std::string, const aiNodeAnim*>& chans,
                                   double                                                 t)
    {
        auto it = chans.find(node->mName.C_Str());
        if (it == chans.end()) return node->mTransformation;
        const aiNodeAnim* ch = it->second;
        const aiVector3D   p = SampleAiVec(ch->mPositionKeys, ch->mNumPositionKeys, t);
        const aiQuaternion r = SampleAiQuat(ch->mRotationKeys, ch->mNumRotationKeys, t);
        const aiVector3D   s = SampleAiVec(ch->mScalingKeys,  ch->mNumScalingKeys,  t);
        return aiMatrix4x4(s, r, p);
    }

    static void MapNodesRec(const aiNode* n,
                            std::unordered_map<std::string, const aiNode*>& out)
    {
        if (!n) return;
        out[n->mName.C_Str()] = n;
        for (unsigned i = 0; i < n->mNumChildren; ++i) MapNodesRec(n->mChildren[i], out);
    }

    static std::vector<CameraAnimClipData> BuildCameraClips(const aiScene* scene)
    {
        using namespace DirectX;
        std::vector<CameraAnimClipData> out;
        if (scene->mNumCameras == 0) return out;

        std::unordered_map<std::string, const aiNode*> nodeMap;
        MapNodesRec(scene->mRootNode, nodeMap);

        for (unsigned ax = 0; ax < scene->mNumAnimations; ++ax)
        {
            const aiAnimation* anim = scene->mAnimations[ax];
            const double tps    = (anim->mTicksPerSecond > 1e-9) ? anim->mTicksPerSecond : 25.0;
            const double durSec = anim->mDuration / tps;
            const float  frameRate  = 30.f;
            const uint32_t frameCount = std::max(1u,
                static_cast<uint32_t>(std::ceil(durSec * frameRate)));

            std::unordered_map<std::string, const aiNodeAnim*> chans;
            for (unsigned ci = 0; ci < anim->mNumChannels; ++ci)
                chans[anim->mChannels[ci]->mNodeName.C_Str()] = anim->mChannels[ci];

            CameraAnimClipData clip;
            const char* aname = anim->mName.C_Str();
            strncpy_s(clip.name, (aname && aname[0]) ? aname : "fbx_camera", 64);
            clip.frameRate  = frameRate;
            clip.frameCount = frameCount;
            clip.duration   = static_cast<float>(durSec);

            for (unsigned camI = 0; camI < scene->mNumCameras; ++camI)
            {
                const aiCamera* cam = scene->mCameras[camI];
                auto nit = nodeMap.find(cam->mName.C_Str());
                if (nit == nodeMap.end()) continue;

                // Ancestor chain root -> camera node.
                std::vector<const aiNode*> chain;
                for (const aiNode* n = nit->second; n; n = n->mParent) chain.push_back(n);
                std::reverse(chain.begin(), chain.end());

                // Skip static cameras (no animated node in the chain): they carry no motion.
                bool animated = false;
                for (const aiNode* n : chain)
                    if (chans.find(n->mName.C_Str()) != chans.end()) { animated = true; break; }
                if (!animated) continue;

                // Lens: aiCamera.mHorizontalFOV is the HALF horizontal angle (rad).
                // Engine wants the FULL VERTICAL fov: vFov = 2*atan(tan(hHalf)/aspect).
                const float hHalf  = cam->mHorizontalFOV;
                const float aspect = (cam->mAspect > 1e-3f) ? cam->mAspect : (16.f / 9.f);
                float vFovY = 2.f * std::atan(std::tan(hHalf) / aspect);
                vFovY = std::clamp(vFovY, 0.01f, XM_PI - 0.01f);
                const float nearZ = (cam->mClipPlaneNear > 0.f) ? cam->mClipPlaneNear : 0.1f;
                const float farZ  = (cam->mClipPlaneFar > nearZ) ? cam->mClipPlaneFar : 1000.f;

                const XMVECTOR mLook = XMVectorSet(cam->mLookAt.x,   cam->mLookAt.y,   cam->mLookAt.z,   0.f);
                const XMVECTOR mUp   = XMVectorSet(cam->mUp.x,       cam->mUp.y,       cam->mUp.z,       0.f);
                const XMVECTOR mPos  = XMVectorSet(cam->mPosition.x, cam->mPosition.y, cam->mPosition.z, 0.f);

                CameraAnimChannel ch;
                strncpy_s(ch.name, (cam->mName.length > 0) ? cam->mName.C_Str() : "camera", _TRUNCATE);
                ch.keys.resize(frameCount);

                for (uint32_t fi = 0; fi < frameCount; ++fi)
                {
                    const double t = (fi / static_cast<double>(frameRate)) * tps;

                    aiMatrix4x4 world; // identity
                    for (const aiNode* n : chain) world = world * NodeLocalAt(n, chans, t);

                    aiVector3D wscl, wpos; aiQuaternion wrot;
                    world.Decompose(wscl, wrot, wpos);

                    const XMVECTOR q   = XMQuaternionNormalize(
                        XMVectorSet(wrot.x, wrot.y, wrot.z, wrot.w));
                    const XMVECTOR fwd = XMVector3Normalize(XMVector3Rotate(mLook, q));
                    const XMVECTOR up  = XMVector3Normalize(XMVector3Rotate(mUp,   q));
                    const XMVECTOR eye = XMVectorAdd(
                        XMVectorSet(wpos.x, wpos.y, wpos.z, 0.f), XMVector3Rotate(mPos, q));

                    // Build a rotation whose +Z = fwd, +Y = up (engine convention).
                    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(up, fwd)); // LH
                    const XMVECTOR up2   = XMVector3Cross(fwd, right);
                    XMMATRIX R = XMMatrixIdentity();
                    R.r[0] = XMVectorSetW(right, 0.f);
                    R.r[1] = XMVectorSetW(up2,   0.f);
                    R.r[2] = XMVectorSetW(fwd,   0.f);
                    const XMVECTOR camQ = XMQuaternionNormalize(XMQuaternionRotationMatrix(R));

                    CameraAnimKey key{};
                    XMStoreFloat3(&key.position, eye);
                    XMStoreFloat4(&key.rotation, camQ);
                    key.fovY        = vFovY;
                    key.nearZ       = nearZ;
                    key.farZ        = farZ;
                    key.orthoHeight = 0.f;
                    key.flags       = 0;
                    ch.keys[fi] = key;
                }
                clip.channels.push_back(std::move(ch));
            }

            if (!clip.channels.empty())
            {
                LOG_INFO("AnimationImporter:   camera clip '%s' — %u frames, %.2fs, %u camera(s)",
                         clip.name, clip.frameCount, clip.duration,
                         static_cast<unsigned>(clip.channels.size()));
                out.push_back(std::move(clip));
            }
        }
        return out;
    }

    // ==== AnimationImporter::Import ====
    std::vector<uint8_t> AnimationImporter::Import(const std::string&          sourcePath,
                                                     const std::vector<uint8_t>& sourceData)
    {
        if (sourceData.empty())
        {
            LOG_ERROR("AnimationImporter: empty source data for '%s'", sourcePath.c_str());
            return {};
        }

        // Load with Assimp: animation data only, no mesh processing.
        std::string ext = Sys::PathToUtf8(Sys::U8Path(sourcePath).extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const char* hint = ext.empty() ? "" : ext.c_str() + 1;

        Assimp::Importer importer;
        // Animation-only FBX has no meshes, so no mesh steps (Triangulate /
        // LimitBoneWeights / GenNormals): on a meshless file they flag the scene
        // AI_SCENE_FLAGS_INCOMPLETE.
        constexpr unsigned int flags =
            aiProcess_ConvertToLeftHanded |
            aiProcess_PopulateArmatureData;
        const aiScene* scene = importer.ReadFileFromMemory(
            sourceData.data(), sourceData.size(), flags, hint);

        // Do not reject on AI_SCENE_FLAGS_INCOMPLETE: for animation-only files the missing
        // mesh / material data is expected, and Assimp sets the flag without an error string.
        if (!scene || !scene->mRootNode)
        {
            LOG_ERROR("AnimationImporter: Assimp failed on '%s': %s",
                      sourcePath.c_str(), importer.GetErrorString());
            return {};
        }

        if (scene->mNumAnimations == 0)
        {
            LOG_WARNING("AnimationImporter: no animations in '%s'", sourcePath.c_str());
            return {};
        }

        LOG_INFO("AnimationImporter: '%s' — %u animation(s)",
                 sourcePath.c_str(), scene->mNumAnimations);

        // Bone clips, with root motion extracted from m_rootBone when it is set.
        std::vector<AnimClipData>  clips;
        std::vector<MorphClipData> morphClips;
        clips.reserve(scene->mNumAnimations);
        morphClips.reserve(scene->mNumAnimations);

        for (unsigned ai = 0; ai < scene->mNumAnimations; ++ai)
        {
            AnimClipData clip = BuildAnimClipData(scene->mAnimations[ai], m_sampleRate);
            LOG_INFO("AnimationImporter:   clip[%u] '%s' — %u frames, %.2fs, %u bone channels",
                     ai, clip.name, clip.frameCount, clip.duration,
                     static_cast<unsigned>(clip.channels.size()));
            if(!m_rootBone.empty())for(auto& channel:clip.channels)if(m_rootBone==channel.name){Anim::ExtractRootMotion(channel.positions,channel.rotations,0,clip.frameCount,clip.frameRate,clip.duration,clip.rootMotion);break;}
            clips.push_back(std::move(clip));

            // The morph clip of the same animation, at the bone clip's frame rate.
            MorphClipData morphClip = BuildMorphClipData(
                scene->mAnimations[ai], scene, clips.back().frameRate);
            if (!morphClip.channels.empty())
            {
                LOG_INFO("AnimationImporter:   morph clip[%u] — %u targets",
                         ai, static_cast<unsigned>(morphClip.channels.size()));
                morphClips.push_back(std::move(morphClip));
            }
        }

        // Camera clips (aiScene::mCameras with an animated node).
        std::vector<CameraAnimClipData> cameraClips = BuildCameraClips(scene);

        if(!m_rootBone.empty()){AnimationResource resource;resource.clips=std::move(clips);resource.morphClips=std::move(morphClips);resource.cameraClips=std::move(cameraClips);return SerializeAnimation(resource);}
        size_t payloadSz = 0;
        for (const auto& c  : clips)       payloadSz += ClipPayloadSize(c);
        for (const auto& m  : morphClips)  payloadSz += MorphClipPayloadSize(m);
        for (const auto& cc : cameraClips) payloadSz += CameraClipPayloadSize(cc);

        // The blob: header, metadata, then the bone, morph and camera clips.
        AnimationMetadata meta{};
        meta.clipCount       = static_cast<uint32_t>(clips.size());
        meta.morphClipCount  = static_cast<uint32_t>(morphClips.size());
        meta.flags           = 0;
        meta.cameraClipCount = static_cast<uint32_t>(cameraClips.size());

        AssetHeader header{};
        header.magic        = MAGIC_ANIMATION;
        header.version      = CurrentVersion(MAGIC_ANIMATION);
        header.resourceType = static_cast<uint16_t>(ResourceType::Animation);
        header.metadataSize = sizeof(AnimationMetadata);
        header.dataSize     = static_cast<uint32_t>(payloadSz);
        header.flags        = 0;
        header.reserved     = 0;

        const size_t totalSz = sizeof(AssetHeader) + sizeof(AnimationMetadata) + payloadSz;
        std::vector<uint8_t> blob(totalSz, 0);
        uint8_t* dst = blob.data();

        std::memcpy(dst, &header, sizeof(AssetHeader));         dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(AnimationMetadata));   dst += sizeof(AnimationMetadata);

        for (const AnimClipData& c : clips)
            WriteClip(c, dst);
        for (const MorphClipData& m : morphClips)
            WriteMorphClip(m, dst);
        for (const CameraAnimClipData& cc : cameraClips)
            WriteCameraClip(cc, dst);

        LOG_INFO("AnimationImporter: done — %zu bone clips, %zu morph clips, "
                 "%zu camera clips, %zu bytes",
                 clips.size(), morphClips.size(), cameraClips.size(), totalSz);

        return blob;
    }

} // namespace Resource
