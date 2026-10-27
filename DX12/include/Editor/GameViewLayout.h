#pragma once

// GameViewLayout - the Game View's framing, header-only (python tests/unit/editor_game_view_layout_test.py):
// its presets, the render extent a preset gives inside the picture box, where that extent is shown (scaled
// to fit and centred: the rest of the box is letterbox / pillarbox bars), and a screen point mapped into the
// extent's pixels. In Play the main pipeline renders at the extent and the Canvas lays out and hit-tests at
// it, so the mapping here is the one every game pointer goes through.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Editor::GameViewLayout
{
    enum class Kind : std::uint8_t
    {
        Free,     // the extent is the picture box
        Aspect,   // the largest extent of that ratio inside the box
        Fixed,    // that render size, whatever the box; shown scaled into it
    };

    struct Preset
    {
        const char*   label;
        Kind          kind;
        std::uint32_t w;   // the ratio's terms (Aspect) or the render size (Fixed); 0 for Free
        std::uint32_t h;
    };

    inline constexpr Preset kPresets[] = {
        { "Free Aspect", Kind::Free,   0,    0    },
        { "16:9",        Kind::Aspect, 16,   9    },
        { "16:10",       Kind::Aspect, 16,   10   },
        { "4:3",         Kind::Aspect, 4,    3    },
        { "21:9",        Kind::Aspect, 21,   9    },
        { "1:1",         Kind::Aspect, 1,    1    },
        { "9:16",        Kind::Aspect, 9,    16   },
        { "1280x720",    Kind::Fixed,  1280, 720  },
        { "1920x1080",   Kind::Fixed,  1920, 1080 },
        { "2560x1440",   Kind::Fixed,  2560, 1440 },
    };
    inline constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

    // The smallest extent handed to the renderer on either axis: the Scene viewport's floor, because post
    // effects build mip chains and dispatch fixed-size groups.
    inline constexpr std::uint32_t kMinExtent = 64;

    struct Extent
    {
        std::uint32_t w = 0;
        std::uint32_t h = 0;

        bool Valid() const { return w >= kMinExtent && h >= kMinExtent; }
    };

    // A rectangle in screen pixels.
    struct Rect
    {
        float x = 0.f;
        float y = 0.f;
        float w = 0.f;
        float h = 0.f;
    };

    inline int ClampPreset(int preset) { return std::clamp(preset, 0, kPresetCount - 1); }

    // The render extent @p preset gives inside a picture box of @p boxW x @p boxH screen pixels. Invalid
    // ({0, 0}) when the box, or the ratio's extent inside it, is under kMinExtent on an axis: a dock node
    // collapsing for a frame, whose size the caller must not hand to the renderer.
    inline Extent ExtentFor(int preset, float boxW, float boxH)
    {
        const Preset& p = kPresets[ClampPreset(preset)];
        if (!(boxW >= static_cast<float>(kMinExtent)) || !(boxH >= static_cast<float>(kMinExtent)))
            return {};
        Extent e;
        switch (p.kind)
        {
        case Kind::Free:
            e = { static_cast<std::uint32_t>(boxW), static_cast<std::uint32_t>(boxH) };
            break;
        case Kind::Aspect:
        {
            const double ratio = static_cast<double>(p.w) / static_cast<double>(p.h);
            if (static_cast<double>(boxW) / static_cast<double>(boxH) > ratio)
                e = { static_cast<std::uint32_t>(std::floor(boxH * ratio)), static_cast<std::uint32_t>(boxH) };
            else
                e = { static_cast<std::uint32_t>(boxW), static_cast<std::uint32_t>(std::floor(boxW / ratio)) };
            break;
        }
        case Kind::Fixed:
            e = { p.w, p.h };
            break;
        }
        return e.Valid() ? e : Extent{};
    }

    // Where @p e is shown inside the box at (@p boxX, @p boxY): the largest uniform scale that fits, centred,
    // on whole pixels. An extent equal to the box fills it at scale 1.
    inline Rect FitRect(Extent e, float boxX, float boxY, float boxW, float boxH)
    {
        if (!e.Valid() || !(boxW > 0.f) || !(boxH > 0.f))
            return { boxX, boxY, 0.f, 0.f };
        const float scale = std::min(boxW / static_cast<float>(e.w), boxH / static_cast<float>(e.h));
        const float w = std::min(boxW, std::round(static_cast<float>(e.w) * scale));
        const float h = std::min(boxH, std::round(static_cast<float>(e.h) * scale));
        return { std::floor(boxX + (boxW - w) * 0.5f), std::floor(boxY + (boxH - h) * 0.5f), w, h };
    }

    // The screen point (@p sx, @p sy) in the extent's pixels, through the shown rectangle @p r. False when the
    // point is outside it - on a bar or off the picture - and then the host owns the pointer.
    inline bool ToGame(const Rect& r, Extent e, float sx, float sy, float& gx, float& gy)
    {
        if (!e.Valid() || !(r.w > 0.f) || !(r.h > 0.f))
            return false;
        if (sx < r.x || sy < r.y || sx >= r.x + r.w || sy >= r.y + r.h)
            return false;
        gx = (sx - r.x) * static_cast<float>(e.w) / r.w;
        gy = (sy - r.y) * static_cast<float>(e.h) / r.h;
        return true;
    }

    // The game pixel (@p gx, @p gy) back on the screen: the inverse of ToGame (a HUD element's on-screen spot).
    inline void ToScreen(const Rect& r, Extent e, float gx, float gy, float& sx, float& sy)
    {
        sx = r.x + (e.w ? gx * r.w / static_cast<float>(e.w) : 0.f);
        sy = r.y + (e.h ? gy * r.h / static_cast<float>(e.h) : 0.f);
    }
}
