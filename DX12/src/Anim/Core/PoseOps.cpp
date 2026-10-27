#include "ECS/AnimationSampleFrames.h"
#include "Anim/Core/PoseOps.h"
#include "Resource/SkeletonAsset.h"
#include "ECS/LocomotionComponents.h"
#include <algorithm>
#include <cmath>
#include <cstring>
using namespace DirectX;
namespace Anim::PoseOps {
XMFLOAT3 Lerp3(const XMFLOAT3& a, const XMFLOAT3& b, float t)
{
    return { a.x + (b.x - a.x) * t,
             a.y + (b.y - a.y) * t,
             a.z + (b.z - a.z) * t };
}

XMFLOAT4 Slerp4(const XMFLOAT4& a, const XMFLOAT4& b, float t)
{
    XMVECTOR qa = XMLoadFloat4(&a);
    XMVECTOR qb = XMLoadFloat4(&b);
    XMVECTOR qr = XMQuaternionSlerp(qa, qb, t);
    XMFLOAT4 out;
    XMStoreFloat4(&out, qr);
    return out;
}

float WrapAngle(float a)
{
    a = std::fmod(a + 180.f, 360.f);
    if (a < 0.f) a += 360.f;
    return a - 180.f;
}

void ResolveBlendSpace(LocomotionComponent* lc, const ClipLibrary& clips,
                              float dt, bool paused)
{
    lc->outCount = 0;
    for (auto& c : lc->outClip) c = ~0u;
    if (!lc->enabled) return;

    // Smooth the inputs (exponential, framerate-independent; angle wrap-aware).
    const float k = (lc->damping <= 0.f) ? 1.f : 1.f - std::exp(-lc->damping * dt);
    lc->smoothedSpeed += (lc->speed - lc->smoothedSpeed) * k;
    lc->smoothedDir = WrapAngle(lc->smoothedDir + WrapAngle(lc->dirDeg - lc->smoothedDir) * k);

    // Gather resolved samples sorted by speed (insertion sort: n <= 24).
    int idx[LocomotionComponent::MAX_SAMPLES];
    int n = 0;
    const int total = std::min(lc->sampleCount,
                               static_cast<int>(LocomotionComponent::MAX_SAMPLES));
    for (int i = 0; i < total; ++i)
    {
        const LocomotionSample& s = lc->samples[i];
        if (s.clip >= clips.Count()) continue;
        if (clips.Get(s.clip).frameCount == 0) continue;
        int j = n++;
        while (j > 0 && lc->samples[idx[j - 1]].speed > s.speed)
        { idx[j] = idx[j - 1]; --j; }
        idx[j] = i;
    }
    if (n == 0) return;

    struct Pick { int s; float w; };
    Pick picks[LocomotionComponent::MAX_BLEND];
    int  np = 0;
    auto addPick = [&](int s, float w)
    {
        if (w <= 1e-4f) return;
        for (int i = 0; i < np; ++i)
            if (picks[i].s == s) { picks[i].w += w; return; }
        if (np < static_cast<int>(LocomotionComponent::MAX_BLEND))
            picks[np++] = { s, w };
    };

    if (lc->mode == static_cast<uint8_t>(LocomotionMode::Blend1D))
    {
        int a = 0, b = 0;
        for (int i = 0; i < n; ++i)
        {
            if (lc->samples[idx[i]].speed <= lc->smoothedSpeed) { a = i; b = i; }
            else { b = i; break; }
        }
        const float sa = lc->samples[idx[a]].speed;
        const float sb = lc->samples[idx[b]].speed;
        const float w = (b != a && sb > sa + 1e-4f)
            ? std::clamp((lc->smoothedSpeed - sa) / (sb - sa), 0.f, 1.f) : 0.f;
        addPick(idx[a], 1.f - w);
        addPick(idx[b], w);
    }
    else // Blend2D: polar bilinear (speed rings x direction spokes)
    {
        // Group the speed-sorted samples into rings (quantized by epsilon).
        int ringStart[LocomotionComponent::MAX_SAMPLES];
        int ringCount[LocomotionComponent::MAX_SAMPLES];
        float ringSpeed[LocomotionComponent::MAX_SAMPLES];
        int rings = 0;
        for (int i = 0; i < n; ++i)
        {
            const float sp = lc->samples[idx[i]].speed;
            if (rings == 0 || sp > ringSpeed[rings - 1] + 0.01f)
            { ringStart[rings] = i; ringCount[rings] = 1; ringSpeed[rings] = sp; ++rings; }
            else ++ringCount[rings - 1];
        }

        // Bracket rings by speed.
        int rA = 0, rB = 0;
        for (int r = 0; r < rings; ++r)
        {
            if (ringSpeed[r] <= lc->smoothedSpeed) { rA = r; rB = r; }
            else { rB = r; break; }
        }
        const float wR = (rB != rA && ringSpeed[rB] > ringSpeed[rA] + 1e-4f)
            ? std::clamp((lc->smoothedSpeed - ringSpeed[rA]) /
                         (ringSpeed[rB] - ringSpeed[rA]), 0.f, 1.f) : 0.f;

        // Within a ring: bracket the input direction between adjacent spokes
        // (cyclic, wrap-aware). A 1-sample ring (idle at the center) covers all
        // directions.
        auto ringPicks = [&](int r, float ringW)
        {
            if (ringW <= 1e-4f) return;
            const int  cnt   = ringCount[r];
            const int* base  = idx + ringStart[r];
            if (cnt == 1) { addPick(base[0], ringW); return; }

            // Sort ring members by angle in [0,360) (insertion sort: cnt <= 24).
            int   order[LocomotionComponent::MAX_SAMPLES];
            float ang[LocomotionComponent::MAX_SAMPLES];
            for (int i = 0; i < cnt; ++i)
            {
                float a = WrapAngle(lc->samples[base[i]].angleDeg);
                if (a < 0.f) a += 360.f;
                int j = i;
                while (j > 0 && ang[j - 1] > a)
                { ang[j] = ang[j - 1]; order[j] = order[j - 1]; --j; }
                ang[j] = a; order[j] = base[i];
            }

            float d = WrapAngle(lc->smoothedDir);
            if (d < 0.f) d += 360.f;

            int i0 = cnt - 1, i1 = 0;               // default: the wrap segment
            float span = ang[0] + 360.f - ang[cnt - 1];
            float off  = d + (d < ang[cnt - 1] ? 360.f : 0.f) - ang[cnt - 1];
            for (int i = 0; i + 1 < cnt; ++i)
            {
                if (d >= ang[i] && d < ang[i + 1])
                {
                    i0 = i; i1 = i + 1;
                    span = ang[i + 1] - ang[i];
                    off  = d - ang[i];
                    break;
                }
            }
            const float w = (span > 1e-3f) ? std::clamp(off / span, 0.f, 1.f) : 0.f;
            addPick(order[i0], ringW * (1.f - w));
            addPick(order[i1], ringW * w);
        };
        ringPicks(rA, 1.f - wR);
        if (rB != rA) ringPicks(rB, wR);
    }

    if (np == 0) return;

    // Normalize (dup-merge and epsilon-drops can leave sum slightly < 1).
    float sum = 0.f;
    for (int i = 0; i < np; ++i) sum += picks[i].w;
    if (sum <= 1e-4f) return;

    // Shared-phase advance with the blended cycle duration.
    float dur = 0.f;
    for (int i = 0; i < np; ++i)
        dur += (picks[i].w / sum) * std::max(clips.Get(lc->samples[picks[i].s].clip).duration, 1e-3f);
    if (!paused)
        lc->phase = std::fmod(lc->phase + dt * lc->playRate / std::max(dur, 1e-3f), 1.f);
    if (lc->phase < 0.f) lc->phase += 1.f;

    // Emit picks, heaviest first (sampleEntity mirrors legDrive from [0]).
    for (int i = 1; i < np; ++i)
        for (int j = i; j > 0 && picks[j].w > picks[j - 1].w; --j)
            std::swap(picks[j], picks[j - 1]);
    for (int i = 0; i < np; ++i)
    {
        const LocomotionSample& s = lc->samples[picks[i].s];
        lc->outClip[i]   = s.clip;
        lc->outSample[i] = static_cast<uint8_t>(picks[i].s);
        lc->outWeight[i] = picks[i].w / sum;
        lc->outTime[i]   = lc->phase * std::max(clips.Get(s.clip).duration, 1e-3f);
    }
    lc->outCount = np;
}

// ==== Bone masks for animation layers ====
// Subtree masks use the skeleton's root-first invariant (parentIndex[i] < i): one forward pass marks
// the seed bone and every descendant.
//   UpperBody  = Spine role subtree (torso, arms, head; the legs hang off the Hips / PMX lower-body
//                sibling, so this is clean on PMX and standard rigs)
//   LowerBody  = complement of UpperBody
//   CustomRoot = named bone + descendants
// Returns false when the seed cannot be resolved and the mask falls back to full body (all 1). Callers
// must surface that: a silent full-body mask makes a layer meant for a tail overwrite the whole
// character, which reads as "the base animation stopped" instead of "a bone name is misspelled".
bool BuildBoneMask(const SkeletonAsset& sk, uint8_t mode,
                          const std::string& customRoot, std::vector<float>& w)
{
    w.assign(sk.boneCount, 1.f);
    if (mode == static_cast<uint8_t>(BoneMaskMode::FullBody)) return true;

    int32_t seed = -1;
    if (mode == static_cast<uint8_t>(BoneMaskMode::CustomRoot))
    {
        for (uint32_t i = 0; i < sk.boneCount && seed < 0; ++i)
            if (std::strcmp(sk.boneNames[i], customRoot.c_str()) == 0)
                seed = static_cast<int32_t>(i);
    }
    else
    {
        seed = sk.humanoid.Bone(HumanoidBone::Spine);
    }
    if (seed < 0 || static_cast<uint32_t>(seed) >= sk.boneCount) return false; // unresolved -> full body

    std::vector<uint8_t> in(sk.boneCount, 0);
    in[seed] = 1;
    for (uint32_t i = static_cast<uint32_t>(seed) + 1; i < sk.boneCount; ++i)
    {
        const int32_t p = sk.parentIndex[i];
        if (p >= 0 && in[p]) in[i] = 1;
    }

    const bool invert = (mode == static_cast<uint8_t>(BoneMaskMode::LowerBody));
    for (uint32_t i = 0; i < sk.boneCount; ++i)
        w[i] = (in[i] != 0) != invert ? 1.f : 0.f;
    return true;
}

void SampleClip(const ClipAsset& clip, float time,
                                  uint32_t boneCount, LocalPose* out, bool looping)
{
    if (clip.frameCount == 0 || clip.frameRate <= 0.f) return;

    const auto frames = Anim::ResolveSampleFrames(time, clip.frameRate, clip.frameCount, looping);
    const uint32_t f0 = frames.first, f1 = frames.second;
    const float alpha = frames.alpha;
    const uint32_t n = std::min(boneCount, clip.boneCount);
    for (uint32_t b = 0; b < n; ++b)
    {
        const uint32_t i0 = b * clip.frameCount + f0;
        const uint32_t i1 = b * clip.frameCount + f1;
        out[b].pos = Lerp3(clip.positions[i0], clip.positions[i1], alpha);
        out[b].rot = Slerp4(clip.rotations[i0], clip.rotations[i1], alpha);
        out[b].scl = Lerp3(clip.scales[i0],     clip.scales[i1],     alpha);
    }
}


void RefPose(const SkeletonAsset& sk, LocalPose* out) {
    for (uint32_t b = 0; b < sk.boneCount; ++b) {
        XMVECTOR scale, rotation, translation;
        XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&sk.restPoseLocal[b]));
        XMStoreFloat3(&out[b].pos, translation);
        XMStoreFloat4(&out[b].rot, rotation);
        XMStoreFloat3(&out[b].scl, scale);
    }
}
void ApplyGrants(const SkeletonAsset& skeleton, LocalPose* pose, uint32_t bones, bool postIK) {
    int32_t minIKOrder = INT32_MAX;
    for (const auto& chain : skeleton.ikChains)
        if (chain.ikBoneIndex < bones)
            minIKOrder = std::min(minIKOrder, skeleton.transformOrder[chain.ikBoneIndex]);
    const XMVECTOR identityQ = XMQuaternionIdentity();
    for (uint32_t b = 0; b < bones; ++b) {
        const int32_t source = skeleton.grantSource[b];
        const float ratio = skeleton.grantRatio[b], amount = std::abs(ratio);
        if (source < 0 || uint32_t(source) >= bones || amount < .001f) continue;
        if ((skeleton.transformOrder[b] >= minIKOrder) != postIK) continue;
        XMVECTOR rotation = XMLoadFloat4(&pose[source].rot);
        if (ratio < 0) rotation = XMQuaternionConjugate(rotation);
        const XMVECTOR grant = amount >= .999f ? rotation : XMQuaternionSlerp(identityQ, rotation, amount);
        XMStoreFloat4(&pose[b].rot, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&pose[b].rot), grant)));
    }
}
void Blend(LocalPose* base, const LocalPose* other, uint32_t bones, float weight, const float* mask) {
    for (uint32_t b = 0; b < bones; ++b) {
        const float w = weight * (mask ? mask[b] : 1.f);
        if (mask && w <= .001f) continue;
        base[b].pos = Lerp3(base[b].pos, other[b].pos, w);
        base[b].rot = Slerp4(base[b].rot, other[b].rot, w);
        base[b].scl = Lerp3(base[b].scl, other[b].scl, w);
    }
}
void Additive(LocalPose* base, const LocalPose* other, const LocalPose* reference,
              uint32_t bones, float weight, const float* mask) {
    for (uint32_t b = 0; b < bones; ++b) {
        const float w = weight * (mask ? mask[b] : 1.f);
        base[b].pos.x += (other[b].pos.x - reference[b].pos.x) * w;
        base[b].pos.y += (other[b].pos.y - reference[b].pos.y) * w;
        base[b].pos.z += (other[b].pos.z - reference[b].pos.z) * w;
        const XMVECTOR delta = XMQuaternionMultiply(XMQuaternionInverse(XMLoadFloat4(&reference[b].rot)), XMLoadFloat4(&other[b].rot));
        XMStoreFloat4(&base[b].rot, XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&base[b].rot), XMQuaternionSlerp(XMQuaternionIdentity(), delta, w))));
        float* dst = &base[b].scl.x;
        const float* src = &other[b].scl.x;
        const float* ref = &reference[b].scl.x;
        for (int a = 0; a < 3; ++a) if (fabsf(ref[a]) > 1e-8f) dst[a] *= 1.f + (src[a] / ref[a] - 1.f) * w;
    }
}

}
