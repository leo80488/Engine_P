#pragma once

// AudioClipResource - runtime form of a .aclip asset (MAGIC_AUDIO), in the texture / mesh /
// animation pattern: the playable bytes plus enough metadata to start a voice and react to cue points
// and loop ranges. Only AUDIO_LOAD_FULL + AUDIO_CODEC_PCM is supported: data is the PCM that XAudio2
// submits. Compressed and streaming values are reserved in the AssetHeader.h enums; the loader
// rejects them.

#include "Resource/Resource.h"
#include "Resource/AssetHeader.h"   // AudioClipMetadata + enums

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmreg.h>                  // WAVEFORMATEX

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Audio
{

// CPU-side cue: a named marker at a sample offset, for "play from cue X" or an animation notify
// that must land on a beat.
struct AudioCue
{
    uint32_t sampleOffset = 0;
    char     name[32]     = {};
};

// Inside a class deriving from Resource::Resource the unqualified name Resource resolves to the
// base class, which breaks a later Resource::FOO lookup in members: write ::Resource:: below.
class AudioClipResource : public ::Resource::Resource
{
public:
    // ---- Accessors for AudioEngine / AudioSystem ----
    bool IsPCM() const { return header.codec == ::Resource::AUDIO_CODEC_PCM; }
    bool IsFullyLoaded() const { return header.strategy == ::Resource::AUDIO_LOAD_FULL; }

    std::span<const uint8_t> PCM() const { return { data.data(), data.size() }; }

    const AudioCue* FindCue(std::string_view name) const
    {
        for (const AudioCue& c : cues)
            if (name == c.name) return &c;
        return nullptr;
    }

    // Copy of the blob's AudioClipMetadata, so nothing re-parses the blob.
    ::Resource::AudioClipMetadata header{};

    // WAVEFORMATEX built from the metadata, handed straight to CreateSourceVoice.
    WAVEFORMATEX                format{};

    // Contiguous sample data (raw PCM). XAudio2 buffers point into it: never resize it after Load.
    std::vector<uint8_t>        data;

    // Cue / marker list parsed from the source file.
    std::vector<AudioCue>       cues;
};

} // namespace Audio
