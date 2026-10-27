#pragma once

// FormatNames - the spelling of a texture format, for diagnostics and tools (the
// Texture Viewer's info panel, the Content Browser's tooltip, the debug target
// registry, Lua Asset.textureInfo).

#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <string>

namespace RHI
{
    // The enumerator's name: "BC7_UNORM_SRGB"; "UNKNOWN" for anything else.
    const char* FormatName(Format f);

    // A DXGI_FORMAT value's enumerator name without the DXGI_FORMAT_ prefix
    // ("BC7_UNORM_SRGB", "R16G16B16A16_FLOAT"); nullptr for a value the
    // Windows SDK does not name.
    const char* DxgiFormatName(uint32_t dxgiFormat);

    // A short label for a DXGI_FORMAT, the way an artist says it: "BC7 sRGB",
    // "BC5", "RGBA16F", "RGBA8 sRGB", "R16". Falls back to DxgiFormatName, then
    // to "DXGI <n>".
    std::string DxgiFormatLabel(uint32_t dxgiFormat);
}
