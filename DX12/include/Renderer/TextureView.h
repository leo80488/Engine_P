#pragma once

// TextureView - what one Texture Viewer picture shows.
//
// A view is a panel-owned RGBA16F target that TextureViewPass paints from ONE
// texture - a channel mask, a mip, an array slice or cube face (or the whole cube as
// a cross / panorama), a value mapping - and the editor draws with ImGui::Image. It
// never goes through the preview layer (its four views are for 3D pictures) nor
// through an ImDrawCallback (one non-reset callback turns frame overlap off for the
// whole frame).
//
// Plain data on purpose: the editor fills a Desc on the game half, the
// TextureViewService copies it, and only the parked serial window hands it to the
// render half.

#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <string>

namespace TextureView
{
    // How stored values become display values (TextureView.cs.hlsl MAP_*).
    enum class Map : std::uint8_t
    {
        Stored = 0,   // as stored: an sRGB texture shows its stored (gamma) bytes
        Linear = 1,   // the decoded value the shaders read (an sRGB texture looks darker)
        Hdr    = 2,   // exposure (EV), optional x/(1+x), then gamma 2.2
        Normal = 3,   // a tangent-space normal from RG, Z rebuilt
        Range  = 4,   // [rangeMin, rangeMax] -> [0, 1]
    };
    enum class Background : std::uint8_t { Checker = 0, Black, Grey, White };
    enum class CubeLayout : std::uint8_t { Face = 0, Cross, Panorama };
    // A second picture B against this one.
    enum class Compare : std::uint8_t { Off = 0, Split, Diff };

    // How the pass reads the source's default SRV - the view type CreateTexture
    // built it with. Unsupported: a view the pass never reads (MSAA, 1D, cube arrays).
    enum class SourceKind : std::uint8_t { Tex2D = 0, Tex2DArray = 1, Cube = 2, Tex3D = 3, Unsupported = 255 };
    SourceKind  KindOf(const RHI::TextureDesc& d);
    const char* KindName(SourceKind k);          // "2D" | "2DArray" | "Cube" | "3D" | "unsupported"
    bool        IsSrgbFormat(RHI::Format f);
    bool        IsOneChannelFormat(RHI::Format f);   // R-only: shown grey

    constexpr std::uint32_t kChR = 1u, kChG = 2u, kChB = 4u, kChA = 8u;
    constexpr std::uint32_t kChRGB = 7u, kChRGBA = 15u;

    struct Desc
    {
        // The texture. The caller holds a TextureSystem reference to it for as
        // long as the view shows it; an invalid handle paints the unavailable
        // pattern. sourceRevision = TextureSystem::Info::revision: a reload
        // (same handle, new contents) draws again.
        RHI::Texture  source;
        std::uint64_t sourceRevision = 0;

        std::uint32_t width = 0, height = 0;   // the output target, in pixels
        std::uint32_t mip = 0;
        std::uint32_t slice = 0;               // array slice / 3D slice / cube face (Face layout)
        CubeLayout    cubeLayout = CubeLayout::Face;
        std::uint32_t channels = kChRGBA;
        Map           map = Map::Stored;
        float         exposureEV = 0.f;        // Hdr
        bool          tonemap = true;          // Hdr
        float         rangeMin = 0.f, rangeMax = 1.f;   // Range
        Background    background = Background::Checker;
        bool          filtered = false;        // bilinear inside the mip (zoomed out); point otherwise
        bool          tile = false;            // repeat (the 3x3 tiling preview)

        // Where the picture sits: uv of the output's pixel (0,0) CORNER and the
        // uv step per output pixel (zoom and pan, computed by the panel).
        float uvOrigin[2]   = { 0.f, 0.f };
        float uvPerPixel[2] = { 0.f, 0.f };

        // Compare: B is drawn with THIS mapping, channels, background and uv - it samples
        // its own texels, so two sizes line up (a smaller B shows magnified). Split: A left
        // of splitX (a fraction of the output width), B right of it. Diff: |A - B| x
        // diffScale of the mapped values. The caller holds B's TextureSystem reference as it
        // holds A's; B must be a cube exactly when A is (the layout is A's).
        Compare       compare = Compare::Off;
        RHI::Texture  compareSource;
        std::uint64_t compareRevision = 0;
        std::uint32_t compareMip = 0, compareSlice = 0;
        float         splitX    = 0.5f;
        float         diffScale = 1.f;
    };

    // A stable hash of everything a Desc draws (the dirty key).
    std::uint64_t Hash(const Desc& d);

    struct Status
    {
        bool          live = false;           // the id is acquired
        bool          drawable = false;       // a target exists and the pass is built
        std::uint64_t renders = 0;            // dispatches recorded for this view
        std::uint64_t descHash = 0;           // what the view was last asked to show (0 = nothing yet)
        std::uint64_t renderedHash = 0;       // what its last dispatch drew
        std::uint64_t lastRenderFrame = 0;    // the device frame of that dispatch
        std::uint32_t width = 0, height = 0;  // the output target
        SourceKind    kind = SourceKind::Unsupported;
        std::string   why;                    // non-empty: why it does not draw
    };

    // Names for Lua and the harness.
    const char* MapName(Map m);                  // "stored" | "linear" | "hdr" | "normal" | "range"
    bool        ParseMap(const std::string& s, Map& out);
    const char* BackgroundName(Background b);    // "checker" | "black" | "grey" | "white"
    bool        ParseBackground(const std::string& s, Background& out);
    const char* CubeLayoutName(CubeLayout l);    // "face" | "cross" | "pano"
    bool        ParseCubeLayout(const std::string& s, CubeLayout& out);
    const char* CompareName(Compare c);          // "off" | "split" | "diff"
    bool        ParseCompare(const std::string& s, Compare& out);
    std::string ChannelsName(std::uint32_t mask);   // "rgba", "r", "rgb", "" for none
    bool        ParseChannels(const std::string& s, std::uint32_t& out);
    const char* FaceName(std::uint32_t face);    // "+X" "-X" "+Y" "-Y" "+Z" "-Z"
}
