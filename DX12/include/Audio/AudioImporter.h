#pragma once

// AudioImporter - converts raw audio source files into the engine's .aclip blob (the IImporter
// pattern, like AnimationImporter): Import(srcBytes) -> [AssetHeader | AudioClipMetadata | PCM data |
// AudioCueRecord table].
// Sources: .wav, RIFF WAVE_FORMAT_PCM only (8/16/24/32-bit); its cue / labl chunks become the
// AudioCueRecords after the data.
// TODO(audio_system_architecture 6.2): .ogg / .flac / .mp3 (an Import() switch + a third-party
// decoder in the editor build).

#include "Resource/IImporter.h"

namespace Audio
{
    class AudioImporter : public Resource::IImporter
    {
    public:
        std::vector<uint8_t> Import(const std::string&                sourcePath,
                                    const std::vector<uint8_t>&       sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".wav" };
        }

        const char* GetInternalExtension() const override { return ".aclip"; }
    };
}
