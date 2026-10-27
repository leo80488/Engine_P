#pragma once

// TextureImportRecord - how a .itex was made, kept in its .meta sidecar as import.* keys: the
// source image and every TextureImportSettings choice.
//
//   import.source    = asset/Textures/Rock_Albedo.png
//   import.format    = BC7_SRGB
//   import.maxSize   = 0
//   import.mipFilter = default
//   import.flipGreen = 0
//   import.mips      = 1
//
// Written by every editor texture import (EditorLayer::ImportTextureFile: the review popup,
// Editor.Import, Editor.Reimport, the Texture Viewer's Reimport) after the sidecar's guid exists;
// the sidecar's other lines are kept. Read by Editor.Reimport, the viewer's Import section and Lua
// Asset.textureInfo(path).import.
//
// A .itex without a record: TextureImportRecordFor guesses one from what the file is (its DXGI
// format names the preset, as TextureInfo's `preset` does) and a same-named source image beside it,
// with recorded = false.
//
// engine/**.meta is not in git (.gitignore lets only asset/**/*.meta through), so an engine
// texture's record stays on the machine that wrote it; the guess covers the rest.

#include "Resource/TextureImporter.h"

#include <string>

namespace Resource
{
    struct TextureImportRecord
    {
        bool                  recorded = false;   // read from the .meta (false: guessed, or nothing known)
        bool                  known    = false;   // the format is named: recorded, or guessed from the file
        std::string           source;             // the source image as the import was given it; "" = unknown
        TextureImportSettings settings;
    };

    // The import.* keys of <itexPath>.meta; false when the sidecar has none.
    bool ReadTextureImportRecord(const std::string& itexPath, TextureImportRecord& out);

    // Replace the import.* keys of <itexPath>.meta - every other line (the
    // guid) kept, a missing sidecar created - atomically. False and @p why on
    // a failed write.
    bool WriteTextureImportRecord(const std::string& itexPath, const TextureImportRecord& rec,
                                  std::string* why = nullptr);

    // The record, or - for a .itex without one - the guess (recorded = false).
    TextureImportRecord TextureImportRecordFor(const std::string& itexPath);

    // A same-named image the texture importer reads (.png .jpg .jpeg .tga .bmp
    // .hdr) beside @p itexPath, as a path like @p itexPath's; "" = none.
    std::string FindTextureImportSource(const std::string& itexPath);

    // The import.* text of a record (the lines WriteTextureImportRecord writes).
    std::string TextureImportRecordText(const TextureImportRecord& rec);
}
