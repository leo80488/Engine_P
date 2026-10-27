#include "Resource/AnimationSerializer.h"
#include "Resource/AnimTrackSerialization.h"
#include "Resource/AnimationResource.h"
#include "Resource/AssetHeader.h"
#include "ECS/NotifySerialization.h"
#include "System/Log.h"

#include <cstring>
#include <string>
#include <vector>

namespace Resource
{
    namespace
    {
        // ---- Bone clip ----
        // Layout (the importer's WriteClip, AnimationLoader's ReadClip):
        //   uint32 channelCount, frameCount; float duration, frameRate; char name[64]
        //   per channel: char boneName[64]; float3[fc]; float4[fc]; float3[fc]
        //   uint32 eventCount; { float time; uint32 nameHash }[]
        size_t ClipPayloadSize(const AnimClipData& clip)
        {
            size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64;
            for (const auto& ch : clip.channels)
            {
                (void)ch;
                sz += 64;
                sz += static_cast<size_t>(clip.frameCount) * (3 + 4 + 3) * sizeof(float);
            }
            sz += sizeof(uint32_t);
            sz += clip.events.size() * (sizeof(float) + sizeof(uint32_t));
            return sz;
        }

        void WriteClip(const AnimClipData& clip, uint8_t*& dst)
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
                // The importer gives positions / rotations / scales frameCount entries each; the
                // size checks keep a malformed editor-made channel from reading past its data (the
                // zeroed blob keeps zeros there).
                const size_t pBytes = static_cast<size_t>(fc) * 3 * sizeof(float);
                const size_t rBytes = static_cast<size_t>(fc) * 4 * sizeof(float);
                const size_t sBytes = static_cast<size_t>(fc) * 3 * sizeof(float);
                if (ch.positions.size() >= fc) std::memcpy(dst, ch.positions.data(), pBytes);
                dst += pBytes;
                if (ch.rotations.size() >= fc) std::memcpy(dst, ch.rotations.data(), rBytes);
                dst += rBytes;
                if (ch.scales.size() >= fc)    std::memcpy(dst, ch.scales.data(),    sBytes);
                dst += sBytes;
            }

            const uint32_t ec = static_cast<uint32_t>(clip.events.size());
            std::memcpy(dst, &ec, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (const ClipAsset::AnimEvent& ev : clip.events)
            {
                std::memcpy(dst, &ev.time,     sizeof(float));    dst += sizeof(float);
                std::memcpy(dst, &ev.nameHash, sizeof(uint32_t)); dst += sizeof(uint32_t);
            }
        }

        // ---- Morph clip ----
        size_t MorphClipPayloadSize(const MorphClipData& clip)
        {
            size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64;
            sz += clip.channels.size() *
                  (64 + static_cast<size_t>(clip.frameCount) * sizeof(float));
            return sz;
        }

        void WriteMorphClip(const MorphClipData& clip, uint8_t*& dst)
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
                const size_t wBytes = static_cast<size_t>(fc) * sizeof(float);
                if (ch.weights.size() >= fc) std::memcpy(dst, ch.weights.data(), wBytes);
                dst += wBytes;
            }
        }

        // ---- Camera clip ----
        // Layout (AnimationLoader's ReadCameraClip):
        //   uint32 channelCount, frameCount; float duration, frameRate; char name[64]
        //   per channel: char name[64]; CameraAnimKey[fc] (48-byte POD each)
        size_t CameraClipPayloadSize(const CameraAnimClipData& clip)
        {
            size_t sz = 2 * sizeof(uint32_t) + 2 * sizeof(float) + 64;
            for (const auto& ch : clip.channels)
            {
                (void)ch;
                sz += 64;
                sz += static_cast<size_t>(clip.frameCount) * sizeof(CameraAnimKey);
            }
            return sz;
        }

        void WriteCameraClip(const CameraAnimClipData& clip, uint8_t*& dst)
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
    } // namespace

    std::vector<uint8_t> SerializeAnimation(const AnimationResource& res)
    {
        if (res.clips.empty() && res.morphClips.empty() && res.cameraClips.empty())
            return {};

        // Pre-encode the notify section (one JSON string per bone clip). It is written only
        // when a clip has notify tracks, so notify-free files match the importer's output.
        bool anyNotifies = false;
        std::vector<std::string> notifyJson;
        notifyJson.reserve(res.clips.size());
        for (const AnimClipData& c : res.clips)
        {
            notifyJson.push_back(
                NotifyIO::TracksToJsonString(c.notifyTracks, c.duration, c.nextNotifyId));
            if (!c.notifyTracks.empty()) anyNotifies = true;
        }

        // File flags from the per-clip flags (the VMD offset round-trip, FK legs).
        uint32_t flags = 0;
        for (const AnimClipData& c : res.clips)
        {
            if (c.positionsAreOffsets) flags |= ANIM_FLAG_POS_OFFSETS;
            if (c.rotationsAreOffsets) flags |= ANIM_FLAG_ROT_OFFSETS;
            if (c.legDriveFK)          flags |= ANIM_FLAG_LEG_FK;   // retargeted FK legs
        }

        size_t payloadSz = 0;
        for (const auto& c : res.clips)       payloadSz += ClipPayloadSize(c);
        for (const auto& m : res.morphClips)  payloadSz += MorphClipPayloadSize(m);
        for (const auto& cc : res.cameraClips) payloadSz += CameraClipPayloadSize(cc);

        size_t notifySectionSz = 0;
        if (anyNotifies)
        {
            flags |= ANIM_FLAG_HAS_NOTIFIES;
            notifySectionSz += sizeof(uint32_t); // notifyClipCount
            for (const std::string& s : notifyJson)
                notifySectionSz += sizeof(uint32_t) + s.size();
            payloadSz += notifySectionSz;
        }

        bool anyTracks=false;std::vector<std::string> tracks;
        for(const auto& clip:res.clips){tracks.push_back(EncodeAnimTracks(clip.rootMotion,clip.floatCurves));anyTracks=anyTracks || !clip.rootMotion.empty() || !clip.floatCurves.empty();}
        if(anyTracks){flags|=ANIM_FLAG_HAS_TRACKS;payloadSz+=sizeof(uint32_t);for(const auto& text:tracks)payloadSz+=sizeof(uint32_t)+text.size();}
        bool anyIK = false;
        for (const auto& clip : res.clips) anyIK |= !clip.ikEnableTracks.empty();
        if (anyIK) {
            flags |= ANIM_FLAG_IK_ENABLE;
            payloadSz += sizeof(uint32_t);
            for (const auto& clip : res.clips) {
                payloadSz += sizeof(uint32_t);
                for (const auto& t : clip.ikEnableTracks)
                    payloadSz += 8 + t.name.size() + t.keys.size() * 5;
            }
        }
        // The blob: header, metadata, the bone / morph / camera clips, then the notify,
        // track and IK tails.
        AnimationMetadata meta{};
        meta.clipCount       = static_cast<uint32_t>(res.clips.size());
        meta.morphClipCount  = static_cast<uint32_t>(res.morphClips.size());
        meta.flags           = flags;
        meta.cameraClipCount = static_cast<uint32_t>(res.cameraClips.size());

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

        std::memcpy(dst, &header, sizeof(AssetHeader));       dst += sizeof(AssetHeader);
        std::memcpy(dst, &meta,   sizeof(AnimationMetadata)); dst += sizeof(AnimationMetadata);

        for (const AnimClipData& c : res.clips)             WriteClip(c, dst);
        for (const MorphClipData& m : res.morphClips)       WriteMorphClip(m, dst);
        for (const CameraAnimClipData& cc : res.cameraClips) WriteCameraClip(cc, dst);

        if (anyNotifies)
        {
            const uint32_t clipCount = static_cast<uint32_t>(notifyJson.size());
            std::memcpy(dst, &clipCount, sizeof(uint32_t)); dst += sizeof(uint32_t);
            for (const std::string& s : notifyJson)
            {
                const uint32_t len = static_cast<uint32_t>(s.size());
                std::memcpy(dst, &len, sizeof(uint32_t)); dst += sizeof(uint32_t);
                if (len) { std::memcpy(dst, s.data(), len); dst += len; }
            }
        }

        if(anyTracks){const uint32_t count=static_cast<uint32_t>(tracks.size());std::memcpy(dst,&count,4);dst+=4;
            for(const auto& text:tracks){uint32_t length=static_cast<uint32_t>(text.size());std::memcpy(dst,&length,4);dst+=4;std::memcpy(dst,text.data(),length);dst+=length;}}
        if (anyIK) {
            auto put = [&](const auto& v) { std::memcpy(dst, &v, sizeof(v)); dst += sizeof(v); };
            put(static_cast<uint32_t>(res.clips.size()));
            for (const auto& clip : res.clips) {
                put(static_cast<uint32_t>(clip.ikEnableTracks.size()));
                for (const auto& t : clip.ikEnableTracks) {
                    put(static_cast<uint32_t>(t.name.size()));
                    std::memcpy(dst, t.name.data(), t.name.size()); dst += t.name.size();
                    put(static_cast<uint32_t>(t.keys.size()));
                    for (const auto& k : t.keys) { put(k.frame); put(k.enabled); }
                }
            }
        }
        LOG_INFO("AnimationSerializer: wrote %zu bone clip(s), %zu morph clip(s), "
                 "%zu camera clip(s), notifies=%s, %zu bytes",
                 res.clips.size(), res.morphClips.size(), res.cameraClips.size(),
                 anyNotifies ? "yes" : "no", totalSz);

        return blob;
    }
} // namespace Resource
