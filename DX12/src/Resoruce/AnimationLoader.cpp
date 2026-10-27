#include "Resource/AnimationLoader.h"
#include "Resource/AnimTrackSerialization.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "ECS/NotifySerialization.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace Resource
{
    // Bounds-checked sequential reader over the untrusted .ianim payload. Every read is
    // checked against `end`; on the first shortfall `ok` latches false and every later
    // read is a no-op, so callers return a failed LoadResult instead of resizing on a
    // bogus count or copying past the buffer: a corrupt or truncated file cannot
    // bad_alloc or read out of bounds.
    struct Cursor
    {
        const uint8_t* src;
        const uint8_t* end;
        bool           ok = true;

        size_t Remaining() const { return ok ? static_cast<size_t>(end - src) : 0; }

        // True if `count` elements of `elemBytes` each still fit (no size_t
        // overflow). Use to gate any resize() driven by a file-supplied count.
        bool CanHold(size_t count, size_t elemBytes) const
        {
            if (!ok)            return false;
            if (elemBytes == 0) return true;
            return count <= static_cast<size_t>(end - src) / elemBytes;
        }

        bool Read(void* dst, size_t n)
        {
            if (!ok || static_cast<size_t>(end - src) < n) { ok = false; return false; }
            std::memcpy(dst, src, n);
            src += n;
            return true;
        }
        template <typename T> bool Read(T& v) { return Read(&v, sizeof(T)); }

        // Returns a pointer to `n` bytes and advances past them, or nullptr
        // (latching !ok) if fewer than n remain. Caller reads at most n bytes.
        const uint8_t* Take(size_t n)
        {
            if (!ok || static_cast<size_t>(end - src) < n) { ok = false; return nullptr; }
            const uint8_t* p = src;
            src += n;
            return p;
        }

        // Bulk-read `count` POD elements into out, validating the byte span
        // BEFORE resize() so a corrupt count can't trigger a huge alloc or OOB.
        template <typename T> bool ReadVec(std::vector<T>& out, size_t count)
        {
            if (!CanHold(count, sizeof(T))) { ok = false; return false; }
            out.resize(count);                          // count*sizeof(T) <= Remaining()
            const size_t bytes = count * sizeof(T);
            if (bytes) { std::memcpy(out.data(), src, bytes); src += bytes; }
            return true;
        }
    };

    // Reads one MorphClipData; latches cur.ok = false on a shortfall.
    static MorphClipData ReadMorphClip(Cursor& cur)
    {
        MorphClipData clip;

        uint32_t cc = 0, fc = 0;
        cur.Read(cc);
        cur.Read(fc);
        cur.Read(clip.duration);
        cur.Read(clip.frameRate);
        cur.Read(clip.name, 64);
        if (!cur.ok) return clip;

        clip.frameCount = fc;
        if (!cur.CanHold(cc, 64)) { cur.ok = false; return clip; } // each channel >= 64B name
        clip.channels.resize(cc);

        for (uint32_t ci = 0; ci < cc; ++ci)
        {
            MorphClipChannel& ch = clip.channels[ci];
            cur.Read(ch.name, 64);
            cur.ReadVec(ch.weights, fc);
            if (!cur.ok) return clip;
        }

        return clip;
    }

    // Reads one AnimClipData; latches cur.ok = false on a shortfall.
    static AnimClipData ReadClip(Cursor& cur)
    {
        AnimClipData clip;

        uint32_t cc = 0, fc = 0;
        cur.Read(cc);
        cur.Read(fc);
        cur.Read(clip.duration);
        cur.Read(clip.frameRate);
        cur.Read(clip.name, 64);
        if (!cur.ok) return clip;

        clip.frameCount = fc;
        if (!cur.CanHold(cc, 64)) { cur.ok = false; return clip; } // each channel >= 64B name
        clip.channels.resize(cc);

        for (uint32_t ci = 0; ci < cc; ++ci)
        {
            AnimClipChannel& ch = clip.channels[ci];
            cur.Read(ch.name, 64);
            cur.ReadVec(ch.positions, fc);   // XMFLOAT3 = 3*float
            cur.ReadVec(ch.rotations, fc);   // XMFLOAT4 = 4*float
            cur.ReadVec(ch.scales,    fc);   // XMFLOAT3 = 3*float
            if (!cur.ok) return clip;
        }

        uint32_t ec = 0;
        cur.Read(ec);
        if (!cur.ok) return clip;
        // Each event is time(float) + nameHash(uint32) = 8B on the wire.
        if (!cur.CanHold(ec, sizeof(float) + sizeof(uint32_t))) { cur.ok = false; return clip; }
        clip.events.resize(ec);
        for (uint32_t ei = 0; ei < ec; ++ei)
        {
            cur.Read(clip.events[ei].time);
            cur.Read(clip.events[ei].nameHash);
            if (!cur.ok) return clip;
        }

        return clip;
    }

    // Reads one CameraAnimClipData; latches cur.ok = false on a shortfall. Mirror of
    // AnimationSerializer's WriteCameraClip:
    //   uint32 channelCount, frameCount; float duration, frameRate; char name[64]
    //   per channel: char name[64]; CameraAnimKey[frameCount] (48-byte POD each)
    static CameraAnimClipData ReadCameraClip(Cursor& cur)
    {
        CameraAnimClipData clip;

        uint32_t cc = 0, fc = 0;
        cur.Read(cc);
        cur.Read(fc);
        cur.Read(clip.duration);
        cur.Read(clip.frameRate);
        cur.Read(clip.name, 64);
        if (!cur.ok) return clip;

        clip.frameCount = fc;
        if (!cur.CanHold(cc, 64)) { cur.ok = false; return clip; } // each channel >= 64B name
        clip.channels.resize(cc);

        for (uint32_t ci = 0; ci < cc; ++ci)
        {
            CameraAnimChannel& ch = clip.channels[ci];
            cur.Read(ch.name, 64);
            cur.ReadVec(ch.keys, fc);   // CameraAnimKey = 48B POD, blob-copied verbatim
            if (!cur.ok) return clip;
        }

        return clip;
    }

    // ==== AnimationLoader::Load ====
    LoadResult AnimationLoader::Load(const std::string&          path,
                                      std::span<const std::uint8_t> data)
    {
        LoadResult result;

        if (!ValidateHeader(data.data(), data.size(), MAGIC_ANIMATION, result.failReason))
        {
            LOG_ERROR("AnimationLoader: bad .ianim header in '%s': %s", path.c_str(), result.failReason.c_str());
            return result;
        }

        const AnimationMetadata* meta = GetMetadata<AnimationMetadata>(data.data());
        const uint32_t clipCount       = meta->clipCount;
        const uint32_t morphClipCount  = meta->morphClipCount;
        const uint32_t cameraClipCount = meta->cameraClipCount;
        const bool posOfs = (meta->flags & 0x1) != 0;
        const bool rotOfs = (meta->flags & 0x2) != 0;
        const bool legFK  = (meta->flags & ANIM_FLAG_LEG_FK) != 0; // retargeted FK legs

        auto res = std::make_unique<AnimationResource>();

        Cursor cur{ GetPayload(data.data()), data.data() + data.size() };

        // The clip counts come from the untrusted header. Cap each reserve to what the
        // remaining payload can hold (every clip has >= 80 bytes of fixed fields:
        // 2 x u32 + 2 x float + 64-byte name), so a corrupt count cannot request a
        // multi-GB allocation up front.
        constexpr size_t kMinClipBytes = 80;
        res->clips.reserve(
            (std::min)(static_cast<size_t>(clipCount),      cur.Remaining() / kMinClipBytes));
        res->morphClips.reserve(
            (std::min)(static_cast<size_t>(morphClipCount), cur.Remaining() / kMinClipBytes));

        for (uint32_t ci = 0; ci < clipCount; ++ci)
        {
            AnimClipData clip = ReadClip(cur);
            if (!cur.ok)
            {
                LOG_ERROR("AnimationLoader: '%s' truncated/corrupt in bone clip %u of %u",
                          path.c_str(), ci, clipCount);
                return result;
            }
            clip.positionsAreOffsets = posOfs;
            clip.rotationsAreOffsets = rotOfs;
            clip.legDriveFK          = legFK;
            res->clips.push_back(std::move(clip));
        }
        for (uint32_t mi = 0; mi < morphClipCount; ++mi)
        {
            MorphClipData clip = ReadMorphClip(cur);
            if (!cur.ok)
            {
                LOG_ERROR("AnimationLoader: '%s' truncated/corrupt in morph clip %u of %u",
                          path.c_str(), mi, morphClipCount);
                return result;
            }
            res->morphClips.push_back(std::move(clip));
        }

        // Camera clips (optional, between the morph clips and the notify tail); a file
        // without them has cameraClipCount 0 and skips this (see AnimationMetadata).
        res->cameraClips.reserve(
            (std::min)(static_cast<size_t>(cameraClipCount), cur.Remaining() / kMinClipBytes));
        for (uint32_t cci = 0; cci < cameraClipCount; ++cci)
        {
            CameraAnimClipData clip = ReadCameraClip(cur);
            if (!cur.ok)
            {
                LOG_ERROR("AnimationLoader: '%s' truncated/corrupt in camera clip %u of %u",
                          path.c_str(), cci, cameraClipCount);
                return result;
            }
            res->cameraClips.push_back(std::move(clip));
        }

        // AnimNotify section (optional tail): present only when the writer set
        // ANIM_FLAG_HAS_NOTIFIES; importer output has none.
        uint32_t notifiedClips = 0;
        if (meta->flags & ANIM_FLAG_HAS_NOTIFIES)
        {
            uint32_t notifyClipCount = 0;
            cur.Read(notifyClipCount);
            for (uint32_t ci = 0; cur.ok && ci < notifyClipCount; ++ci)
            {
                uint32_t len = 0;
                if (!cur.Read(len)) break;
                const uint8_t* jp = cur.Take(len);
                if (!jp) break;
                std::string json(reinterpret_cast<const char*>(jp), len);
                if (ci < res->clips.size())
                {
                    AnimClipData& clip = res->clips[ci];
                    if (NotifyIO::TracksFromJsonString(json, clip.notifyTracks,
                                                       nullptr, &clip.nextNotifyId)
                        && !clip.notifyTracks.empty())
                        ++notifiedClips;
                }
            }
            // The notifies are an optional tail: a truncated one keeps the valid clip data (stop
            // reading and warn), but a tracks tail after it then fails the whole load.
            if (!cur.ok)
                LOG_WARNING("AnimationLoader: '%s' notify section truncated; "
                            "remaining notifies skipped", path.c_str());
        }

        if(meta->flags & ANIM_FLAG_HAS_TRACKS){uint32_t count=0;
            if(!cur.Read(count) || count!=res->clips.size()){LOG_ERROR("AnimationLoader: invalid graph track count '%s'",path.c_str());return {};}
            for(uint32_t i=0;i<count;++i){uint32_t length=0;if(!cur.Read(length) || length>64*1024*1024)return {};const auto* data=cur.Take(length);if(!data || !DecodeAnimTracks({reinterpret_cast<const char*>(data),length},res->clips[i].duration,res->clips[i].rootMotion,res->clips[i].floatCurves)){LOG_ERROR("AnimationLoader: invalid graph tracks '%s' clip=%u",path.c_str(),i);return {};}}}
        if ((meta->flags & ANIM_FLAG_IK_ENABLE) && cur.ok) {
            uint32_t clips = 0; cur.Read(clips);
            if (clips != res->clips.size()) cur.ok = false;
            for (uint32_t i = 0; cur.ok && i < clips; ++i) {
                uint32_t tracks = 0; cur.Read(tracks);
                if (!cur.CanHold(tracks, 8)) { cur.ok = false; break; }
                auto& out = res->clips[i].ikEnableTracks;
                for (uint32_t t = 0; cur.ok && t < tracks; ++t) {
                    IKEnableTrack track; uint32_t len = 0, count = 0;
                    cur.Read(len); const auto* name = cur.Take(len);
                    if (!name || len > 255) { cur.ok = false; break; }
                    track.name.assign(reinterpret_cast<const char*>(name), len);
                    cur.Read(count);
                    if (!cur.CanHold(count, 5)) { cur.ok = false; break; }
                    track.keys.resize(count);
                    for (auto& key : track.keys) { cur.Read(key.frame); cur.Read(key.enabled); }
                    std::stable_sort(track.keys.begin(), track.keys.end(),
                        [](const auto& a, const auto& b) { return a.frame < b.frame; });
                    out.push_back(std::move(track));
                }
            }
            if (!cur.ok) {
                LOG_WARNING("AnimationLoader: '%s' IK tail truncated; tracks discarded", path.c_str());
                for (auto& clip : res->clips) clip.ikEnableTracks.clear();
            }
        }
        LOG_INFO("AnimationLoader: loaded '%s' — %u bone clip(s), %u morph clip(s), "
                 "%u camera clip(s), %u clip(s) with notifies",
                 path.c_str(), clipCount, morphClipCount, cameraClipCount, notifiedClips);

        result.resource = std::move(res);
        return result;
    }

} // namespace Resource
