#pragma once

// ItexDecode - a .itex blob decoded as stored: [AssetHeader][TextureMetadata][DDS]
// -> DirectX::ScratchImage; the one parse TextureLoader::Load and
// Resource/TextureAnalysis share.
//
// Pulls in DirectXTex.h: include it only from a .cpp that uses DirectXTex anyway.

#include <DirectXTex.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace Resource
{
    // Validates the AssetHeader (magic, version, the blob holds what it promises)
    // and decodes the DDS payload. False with @p error (DescribeHeaderCheck's text for a bad
    // header, "LoadFromDDSMemory failed (hr=0x...)").
    bool DecodeItexBlob(const std::uint8_t* blob, std::size_t size, DirectX::TexMetadata& metadata,
                        DirectX::ScratchImage& image, std::string& error);
}
