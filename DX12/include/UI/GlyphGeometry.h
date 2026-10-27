#pragma once

// Where a glyph's SDF cell goes, known before it is rendered. FreeType's
// bitmap for FT_RENDER_MODE_NORMAL is the outline's control box with the
// minimum floored and the maximum ceiled to whole pixels
// (ft_glyphslot_preset_bitmap, no origin), and its sdf and bsdf renderers pad
// that box by their spread on every side. So a cell's size and bearing follow
// from FT_Load_Char alone - about a microsecond - before the ~2 ms SDF render.
// tests/unit/glyph_metrics_test.cpp holds the engine to it on every glyph of
// every font it ships.
//
// Plain integers on purpose: the FreeType headers stay out of the engine's
// include graph (Font.h), and the unit test uses this very function.

namespace UI::GlyphGeometry
{
    struct Cell
    {
        int w = 0, h = 0;        // pixels, the spread on both sides included
        int left = 0, top = 0;   // the padded bitmap's bitmap_left / bitmap_top
        bool Empty() const { return w == 0 || h == 0; }
    };

    // @p xMin .. @p yMax: FT_Outline_Get_CBox, 26.6 fixed point. A box that
    // rounds to no pixel in either axis is an EMPTY cell - the case in which
    // FreeType's SDF renderer succeeds and leaves no bitmap (arial.ttf's
    // U+2000-U+200A and U+202F are one contour of one point): the glyph is an
    // advance and nothing to draw.
    inline Cell FromControlBox(long long xMin, long long yMin, long long xMax, long long yMax, int spread)
    {
        const long long x0 = xMin >> 6, y0 = yMin >> 6;                 // floor
        const long long x1 = (xMax + 63) >> 6, y1 = (yMax + 63) >> 6;   // ceil
        Cell c;
        if (x1 <= x0 || y1 <= y0) return c;
        c.w    = static_cast<int>(x1 - x0) + 2 * spread;
        c.h    = static_cast<int>(y1 - y0) + 2 * spread;
        c.left = static_cast<int>(x0) - spread;
        c.top  = static_cast<int>(y1) + spread;
        return c;
    }
}
