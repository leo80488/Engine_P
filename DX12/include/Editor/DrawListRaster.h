#pragma once
// DrawListRaster - an ImDrawList software-rasterized into a PNG, for harness
// shots of panel layers no capture path sees (a panel's draw list is ImGui's,
// never a render target). Vertex colours only, src-over in submission order
// over opaque black; textures are ignored, so text comes out as solid quads.
// Shared by the Chain Physics Preview's overlay shot and the Mesh Editor's
// overlay / UV shots. Editor-only TU.

#include <cstdint>
#include <string>

struct ImDrawList;

namespace Editor
{
    // Rasterize @p dl (drawn in a 0,0 .. w,h pixel rect) and write @p path.
    // Returns the number of pixels any triangle touched, or -1 with @p err set
    // (bad size, the write failed). @p outTris = triangles seen.
    long long RasterizeDrawListToPng(const ImDrawList& dl, std::uint32_t w, std::uint32_t h,
                                     const std::string& path, std::uint32_t* outTris,
                                     std::string* err);
}
