#include "Audio/AudioImporter.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmreg.h>          // WAVE_FORMAT_PCM

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace Audio
{

namespace
{

// Little-endian unaligned reads: RIFF chunks are LE on disk.
inline uint16_t LeU16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
inline uint32_t LeU32(const uint8_t* p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

struct WavFmt
{
    uint16_t formatTag;
    uint16_t channels;
    uint32_t sampleRate;
    uint32_t bytesPerSec;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};

// One parsed cue; becomes one AudioCueRecord in the output.
struct ParsedCue
{
    uint32_t cueId       = 0;     // RIFF cue table key; labl chunks match it by id
    uint32_t sampleOffset = 0;
    std::string name;             // empty until matched against a labl chunk
};

// Parses a "cue " chunk. Each cue is 24 bytes: id, position, dataChunkId (4cc), chunkStart,
// blockStart, sampleOffset (all u32); only id and sampleOffset are kept.
void ParseCueChunk(const uint8_t* p, uint32_t size, std::vector<ParsedCue>& out)
{
    if (size < 4) return;
    const uint32_t count = LeU32(p);
    p += 4;
    if (4 + count * 24u > size) return;

    out.reserve(out.size() + count);
    for (uint32_t i = 0; i < count; ++i)
    {
        ParsedCue c;
        c.cueId        = LeU32(p +  0);
        c.sampleOffset = LeU32(p + 20);
        out.push_back(c);
        p += 24;
    }
}

// Walk a LIST/adtl chunk for `labl` sub-chunks: 4-byte cue id + null-terminated
// string. RIFF aligns sub-chunks to even byte boundaries.
void ParseLabelChunk(const uint8_t* p, uint32_t size, std::vector<ParsedCue>& cues)
{
    // First 4 bytes must be "adtl"; otherwise it's not the list type we care about.
    if (size < 4 || std::memcmp(p, "adtl", 4) != 0) return;
    p += 4; size -= 4;

    while (size >= 8)
    {
        const char*    subId  = reinterpret_cast<const char*>(p);
        const uint32_t subSz  = LeU32(p + 4);
        const uint8_t* sub    = p + 8;
        if (8u + subSz > size) break;

        if (subSz >= 4 && std::memcmp(subId, "labl", 4) == 0)
        {
            const uint32_t cueId = LeU32(sub);
            const char*    text  = reinterpret_cast<const char*>(sub + 4);
            const size_t   maxLen = subSz - 4;
            const size_t   tlen  = std::min<size_t>(maxLen, std::strlen(text));
            for (auto& c : cues)
                if (c.cueId == cueId)
                {
                    c.name.assign(text, text + tlen);
                    break;
                }
        }

        const uint32_t advance = 8 + subSz + (subSz & 1u);
        if (advance > size) break;
        p   += advance;
        size -= advance;
    }
}

bool ParseWav(const std::vector<uint8_t>& src,
              WavFmt&                     fmtOut,
              std::vector<uint8_t>&       dataOut,
              std::vector<ParsedCue>&     cuesOut,
              const std::string&          path)
{
    if (src.size() < 12)
    {
        LOG_ERROR("AudioImporter: '%s' too small to be RIFF", path.c_str());
        return false;
    }
    if (std::memcmp(src.data(), "RIFF", 4) != 0
     || std::memcmp(src.data() + 8, "WAVE", 4) != 0)
    {
        LOG_ERROR("AudioImporter: '%s' not RIFF/WAVE", path.c_str());
        return false;
    }

    bool gotFmt = false, gotData = false;
    size_t off = 12;
    while (off + 8 <= src.size())
    {
        const uint8_t* hdr  = src.data() + off;
        const uint32_t sz   = LeU32(hdr + 4);
        const size_t   body = off + 8;
        if (body + sz > src.size())
        {
            LOG_ERROR("AudioImporter: '%s' truncated chunk", path.c_str());
            return false;
        }
        const uint8_t* p = src.data() + body;

        if (std::memcmp(hdr, "fmt ", 4) == 0)
        {
            if (sz < 16) { LOG_ERROR("AudioImporter: '%s' short fmt", path.c_str()); return false; }
            fmtOut.formatTag     = LeU16(p +  0);
            fmtOut.channels      = LeU16(p +  2);
            fmtOut.sampleRate    = LeU32(p +  4);
            fmtOut.bytesPerSec   = LeU32(p +  8);
            fmtOut.blockAlign    = LeU16(p + 12);
            fmtOut.bitsPerSample = LeU16(p + 14);
            if (fmtOut.formatTag != WAVE_FORMAT_PCM)
            {
                LOG_ERROR("AudioImporter: '%s' formatTag=%u not PCM (compressed sources reserved for later)",
                          path.c_str(), fmtOut.formatTag);
                return false;
            }
            gotFmt = true;
        }
        else if (std::memcmp(hdr, "data", 4) == 0)
        {
            dataOut.assign(p, p + sz);
            gotData = true;
        }
        else if (std::memcmp(hdr, "cue ", 4) == 0)
        {
            ParseCueChunk(p, sz, cuesOut);
        }
        else if (std::memcmp(hdr, "LIST", 4) == 0)
        {
            ParseLabelChunk(p, sz, cuesOut);
        }
        // Other chunks (fact, INFO, bext, ...) ignored.

        off = body + sz + (sz & 1u);   // word-align
    }

    if (!gotFmt || !gotData)
    {
        LOG_ERROR("AudioImporter: '%s' missing fmt or data chunk", path.c_str());
        return false;
    }
    return true;
}

// Peak amplitude (0..1) of 16-bit PCM; the caller uses it only for 16-bit sources.
float ComputePeak16(const std::vector<uint8_t>& pcm)
{
    if (pcm.empty() || (pcm.size() & 1u)) return 0.f;
    const int16_t* p = reinterpret_cast<const int16_t*>(pcm.data());
    const size_t   n = pcm.size() / 2;
    int32_t maxAbs = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const int32_t v = p[i] < 0 ? -static_cast<int32_t>(p[i]) : p[i];
        if (v > maxAbs) maxAbs = v;
    }
    return maxAbs / 32768.f;
}

} // anonymous

std::vector<uint8_t> AudioImporter::Import(const std::string&            sourcePath,
                                            const std::vector<uint8_t>&  sourceData)
{
    using namespace Resource;

    WavFmt fmt{};
    std::vector<uint8_t>   pcm;
    std::vector<ParsedCue> cues;
    if (!ParseWav(sourceData, fmt, pcm, cues, sourcePath))
        return {};

    // ---- .aclip metadata ----
    AudioClipMetadata meta{};
    meta.codec         = AUDIO_CODEC_PCM;
    meta.channels      = fmt.channels;
    meta.bitsPerSample = fmt.bitsPerSample;
    meta.sampleRate    = fmt.sampleRate;
    meta.blockAlign    = fmt.blockAlign;
    if (meta.blockAlign == 0)
        meta.blockAlign = static_cast<uint32_t>(fmt.channels * (fmt.bitsPerSample / 8u));

    meta.sampleCount = meta.blockAlign
                       ? static_cast<uint32_t>(pcm.size() / meta.blockAlign)
                       : 0u;
    meta.duration    = (fmt.bytesPerSec > 0)
                       ? static_cast<float>(pcm.size()) / static_cast<float>(fmt.bytesPerSec)
                       : 0.f;

    // Peak for 16-bit only; other depths leave 0 (= not measured).
    meta.peakAmplitude = (fmt.bitsPerSample == 16) ? ComputePeak16(pcm) : 0.f;
    meta.loudnessLUFS  = 0.f;     // 0 = not measured. TODO(audio_system_architecture 6.2): an LUFS pass

    meta.strategy  = AUDIO_LOAD_FULL;
    meta.loopStart = 0xFFFFFFFFu; // 0xFFFFFFFF = no loop. TODO(audio_system_architecture 6.3): read the smpl chunk
    meta.loopEnd   = 0xFFFFFFFFu;
    meta.cueCount  = static_cast<uint32_t>(cues.size());
    meta.reserved  = 0;

    // ---- Pack: header | metadata | PCM | cue records ----
    const size_t cueBytes = cues.size() * sizeof(AudioCueRecord);
    const size_t payloadSz = pcm.size() + cueBytes;

    AssetHeader header{};
    header.magic        = MAGIC_AUDIO;
    header.version      = CurrentVersion(MAGIC_AUDIO);
    header.resourceType = static_cast<uint16_t>(ResourceType::AudioClip);
    header.metadataSize = sizeof(AudioClipMetadata);
    header.dataSize     = static_cast<uint32_t>(payloadSz);
    header.flags        = 0;
    header.reserved     = 0;

    std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(AudioClipMetadata) + payloadSz);
    uint8_t* dst = blob.data();

    std::memcpy(dst, &header, sizeof(AssetHeader));         dst += sizeof(AssetHeader);
    std::memcpy(dst, &meta,   sizeof(AudioClipMetadata));   dst += sizeof(AudioClipMetadata);
    if (!pcm.empty())
    {
        std::memcpy(dst, pcm.data(), pcm.size());
        dst += pcm.size();
    }
    for (const ParsedCue& c : cues)
    {
        AudioCueRecord rec{};
        rec.sampleOffset = c.sampleOffset;
        const size_t n = std::min<size_t>(c.name.size(), sizeof(rec.name) - 1);
        std::memcpy(rec.name, c.name.data(), n);
        rec.name[n] = '\0';
        std::memcpy(dst, &rec, sizeof(AudioCueRecord));
        dst += sizeof(AudioCueRecord);
    }

    LOG_INFO("AudioImporter: '%s' → .aclip (%u Hz, %u ch, %u bit, %.2f s, %zu cue%s, %zu bytes)",
             sourcePath.c_str(),
             meta.sampleRate, meta.channels, meta.bitsPerSample, meta.duration,
             cues.size(), cues.size() == 1 ? "" : "s", blob.size());
    return blob;
}

} // namespace Audio
