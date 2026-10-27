#pragma once
// Harness/AttachmentDecode.h - what the pipe's capture {"attachment"} makes of a render target
// read back as stored: numbers first, a picture second.
//
// A PNG of a depth buffer, a packed normal or FP16 radiance is not the data: it clamps, quantizes
// and hides NaN. So the capture keeps three things:
//   <stem>.bin   the texels exactly as the GPU stored them (rows packed tight, little-endian) -
//                IGraphicsDevice::ReadTextureRaw's bytes;
//   <stem>.json  the manifest: format, how to decode each channel, what it means, per-channel
//                statistics over every texel (min / max / mean of the finite ones, NaN, Inf and
//                zero counts), the camera the frame was drawn with, and the preview's mapping;
//   <stem>.png   a preview under a fixed mapping stated in the manifest - never normalised per
//                image, so two previews compare - with NaN / Inf texels in magenta so they cannot
//                hide.
//
// The attachments (the main view's, as the last frame left them):
//   albedo    GBuffer0  R11G11B10_FLOAT     baseColor, linear (no emissive)
//   normal    GBuffer1  R16G16B16A16_UNORM  world normal * 0.5 + 0.5; .a = 16-bit int / 65535:
//                                           bits 0..14 matIdx, bit 15 = excluded from XeGTAO
//   surface   GBuffer2  R8G8B8A8_UNORM      roughness, metalness, ao, reflectance
//   depth     GBuffer_Depth plane 0 (D32)   reverse-Z: 1 at near, 0 at far (sky)
//   velocity  GBuffer3  R16G16_FLOAT        NDC motion vector
//   extra     GBuffer4  R16G16B16A16_FLOAT  shading-model scratch (the graph calls it
//                                           GBuffer4_Emissive)
//   hdr       the device's HDR scene target: lit scene + sky + transparent + in-place effects,
//             before AA / upscale and tonemap (linear)
// and every other target the debug target registry holds (Renderer/DebugTargets.h: ssao, rtao,
// rtshadow, any render-graph texture by its graph name), described by its registry entry - its
// meaning, channel names and semantic, which picks the decode notes and the preview mapping:
// color (sRGB), radiance (Reinhard), depth (the log ramp), velocity, and the stored value for
// normal / surface / mask / data (one channel as grey). The manifest states the semantic, so a
// reader keys on it, not on the name.
// The decoder reads the format, not the name: a format it does not know is an error, never a
// guess.

#include "Graphics/IGraphicsDevice.h"   // IGraphicsDevice::RawTexture
#include "Renderer/DebugTargets.h"      // DebugTargets::Info

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Harness::Attachments
{
    // The names the verb takes: the debug target registry's as the renderer
    // published them, or - before the first Compile - the seven above.
    std::vector<std::string> Names();
    bool IsKnown(const std::string& name);   // case-insensitive

    // The camera the frame was drawn with (Renderer::AttachmentCamera).
    struct Camera
    {
        float         nearZ = 0.f, farZ = 0.f, fovY = 0.f;
        bool          orthographic = false;
        float         position[3] = { 0.f, 0.f, 0.f };
        float         forward[3]  = { 0.f, 0.f, 1.f };
        std::uint32_t renderWidth = 0, renderHeight = 0;
    };

    struct Decoded
    {
        nlohmann::json            manifest;      // see the header comment
        std::vector<std::uint8_t> previewRgba;   // width * height * 4
        std::string               error;         // why not (a format this decoder does not read)
    };

    // Decode @p raw as attachment @p name (the registry's spelling); @p info
    // is its registry entry - what a name without words of its own here
    // (anything but the seven above) is described by. False with out.error
    // for a format the decoder does not know or bytes that do not fit the size.
    bool Decode(const std::string& name, const IGraphicsDevice::RawTexture& raw, const Camera& camera,
                Decoded& out, const DebugTargets::Info* info = nullptr);
}
