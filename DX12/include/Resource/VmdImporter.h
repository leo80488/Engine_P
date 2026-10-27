#pragma once

// VmdImporter - VMD (Vocaloid Motion Data) importer: MikuMikuDance's binary motion format -> .ianim.
//
// Imports the bone tracks (one clip), the morph / face tracks (a morph clip on the same timeline),
// the camera tracks (a camera clip of CameraAnimKey frames) and the show-IK enable tracks; the light
// and self-shadow sections are skipped.
//
// Coordinate convention: identity (no axis negation), matching PmxImporter.
//   Position:   (x, y, z)_MMD -> (x, y, z)  unchanged
//   Quaternion: (x, y, z, w)_MMD -> (x, y, z, w)  unchanged
//
// Bone and morph names are Shift-JIS in VMD; they are converted to UTF-8 (Win32
// MultiByteToWideChar) before they become .ianim channel names, so they match the skeleton bone names
// imported from PMX/FBX.
//
// Supported variants:
//   VMD2 - "Vocaloid Motion Data 0002"  (20-byte model name)
//   VMD1 - "Vocaloid Motion Data file"  (10-byte model name)
//
// Registered with ResourceManager for: .vmd
// Produces: .ianim

#include "Resource/IImporter.h"

namespace Resource
{
    class VmdImporter : public IImporter
    {
    public:
        // Converts raw VMD bytes to a .ianim blob (bone, morph and camera clips, as present). Returns an
        // empty vector if the file is not a valid VMD or has no bone, morph or camera keyframes.
        std::vector<uint8_t> Import(const std::string& sourcePath,
                                     const std::vector<uint8_t>& sourceData) override;

        std::vector<const char*> GetSourceExtensions() const override
        {
            return { ".vmd" };
        }

        const char* GetInternalExtension() const override { return ".ianim"; }
    };
}
