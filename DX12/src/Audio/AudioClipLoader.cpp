#include "Audio/AudioClipLoader.h"
#include "Audio/AudioClipResource.h"
#include "Resource/AssetHeader.h"
#include "System/Log.h"

#include <cstring>
#include <memory>

namespace Audio
{

Resource::LoadResult AudioClipLoader::Load(const std::string&             path,
                                           std::span<const std::uint8_t> data)
{
    using namespace Resource;
    LoadResult result;

    if (!ValidateHeader(data.data(), data.size(), MAGIC_AUDIO, result.failReason))
    {
        LOG_ERROR("AudioClipLoader: bad .aclip header in '%s': %s", path.c_str(), result.failReason.c_str());
        return result;
    }

    const AudioClipMetadata* meta = GetMetadata<AudioClipMetadata>(data.data());

    // Only PCM + FullyLoaded are accepted. The other codec / strategy values are reserved in the format,
    // so supporting them needs no format-version bump: the loader just stops rejecting them.
    if (meta->codec != AUDIO_CODEC_PCM)
    {
        LOG_ERROR("AudioClipLoader: '%s' codec=%u not supported (PCM only in v1)",
                  path.c_str(), meta->codec);
        return result;
    }
    if (meta->strategy != AUDIO_LOAD_FULL)
    {
        LOG_ERROR("AudioClipLoader: '%s' strategy=%u not supported (FullyLoaded only)",
                  path.c_str(), meta->strategy);
        return result;
    }
    if (meta->channels == 0 || meta->bitsPerSample == 0 || meta->sampleRate == 0)
    {
        LOG_ERROR("AudioClipLoader: '%s' invalid format (ch=%u bits=%u rate=%u)",
                  path.c_str(), meta->channels, meta->bitsPerSample, meta->sampleRate);
        return result;
    }

    auto res = std::make_unique<AudioClipResource>();
    res->header = *meta;

    // Build the WAVEFORMATEX here: XAudio2 takes a pointer to it, and AudioClipResource outlives every
    // voice that uses it.
    res->format.wFormatTag      = WAVE_FORMAT_PCM;
    res->format.nChannels       = meta->channels;
    res->format.nSamplesPerSec  = meta->sampleRate;
    res->format.wBitsPerSample  = meta->bitsPerSample;
    res->format.nBlockAlign     = meta->blockAlign != 0
        ? static_cast<WORD>(meta->blockAlign)
        : static_cast<WORD>(meta->channels * (meta->bitsPerSample / 8u));
    res->format.nAvgBytesPerSec = res->format.nBlockAlign * meta->sampleRate;
    res->format.cbSize          = 0;

    // The payload (the header's dataSize bytes) holds the samples followed by the cue table (see
    // AssetHeader.h): slice it.
    const uint8_t* payload     = GetPayload(data.data());
    const AssetHeader* h       = GetHeader(data.data());
    const size_t   payloadSize = h->dataSize;

    const size_t   cueBytes    = static_cast<size_t>(meta->cueCount) * sizeof(AudioCueRecord);
    if (cueBytes > payloadSize)
    {
        LOG_ERROR("AudioClipLoader: '%s' cue table overflows payload (%zu > %zu)",
                  path.c_str(), cueBytes, payloadSize);
        return result;
    }
    const size_t   sampleBytes = payloadSize - cueBytes;

    res->data.assign(payload, payload + sampleBytes);

    if (meta->cueCount)
    {
        const AudioCueRecord* cueArr =
            reinterpret_cast<const AudioCueRecord*>(payload + sampleBytes);
        res->cues.resize(meta->cueCount);
        for (uint32_t i = 0; i < meta->cueCount; ++i)
        {
            res->cues[i].sampleOffset = cueArr[i].sampleOffset;
            std::memcpy(res->cues[i].name, cueArr[i].name, sizeof(res->cues[i].name));
            res->cues[i].name[31] = '\0';
        }
    }

    LOG_INFO("AudioClipLoader: loaded '%s' (%u Hz, %u ch, %u bit, %.2f s, %u cue%s)",
             path.c_str(),
             meta->sampleRate, meta->channels, meta->bitsPerSample,
             meta->duration, meta->cueCount, meta->cueCount == 1 ? "" : "s");

    result.resource = std::move(res);
    return result;
}

} // namespace Audio
