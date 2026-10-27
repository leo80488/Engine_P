#pragma once
// One camera / video image on its way to a tracker: NV12 - a full-size luma plane, then a
// half-size interleaved CbCr plane - the one format the capture paths deliver (Media
// Foundation converts to it, the video decoder is asked for it) and the inference front end
// reads. Plain data: the mailbox swaps them. Header-only (tests/unit/face_capture_test.cpp).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace FaceTracking {

struct FaceImage
{
    uint32_t width = 0, height = 0;   // even, both (NV12)
    std::vector<uint8_t> y;           // width x height, tightly packed
    std::vector<uint8_t> uv;          // width x height / 2: Cb, Cr interleaved per 2x2 block
    int64_t ptsUs = 0;                // the stream's own time (a video file's PTS; a camera's sample time)
    int64_t captureUs = 0;            // SteadyNowUs() when it arrived
    uint64_t seq = 0;                 // per source, from 1

    bool Empty() const { return width == 0 || height == 0; }

    void Resize(uint32_t w, uint32_t h)
    {
        width = w & ~1u;
        height = h & ~1u;
        y.resize(static_cast<size_t>(width) * height);
        uv.resize(static_cast<size_t>(width) * height / 2);
    }

    // Copies a strided NV12 source in (MF's IMF2DBuffer pitch, FFmpeg's linesize).
    void CopyFrom(uint32_t w, uint32_t h, const uint8_t* srcY, size_t pitchY, const uint8_t* srcUV, size_t pitchUV)
    {
        Resize(w, h);
        for (uint32_t row = 0; row < height; ++row)
            std::copy(srcY + row * pitchY, srcY + row * pitchY + width, y.begin() + static_cast<ptrdiff_t>(row) * width);
        for (uint32_t row = 0; row < height / 2; ++row)
            std::copy(srcUV + row * pitchUV, srcUV + row * pitchUV + width, uv.begin() + static_cast<ptrdiff_t>(row) * width);
    }

    // Converts a strided YUY2 (Y0 U Y1 V per two pixels) source to NV12: luma as is, chroma the mean of each
    // 2x2 block's two rows. A camera's native YUY2 taken this way skips Media Foundation's video processor,
    // which delivers a 60 Hz camera's frames four at a time.
    void FromYuy2(uint32_t w, uint32_t h, const uint8_t* src, size_t pitch)
    {
        Resize(w, h);
        for (uint32_t row = 0; row < height; row += 2)
        {
            const uint8_t* a = src + row * pitch;
            const uint8_t* b = a + pitch;
            uint8_t* y0 = y.data() + static_cast<size_t>(row) * width;
            uint8_t* y1 = y0 + width;
            uint8_t* c = uv.data() + static_cast<size_t>(row / 2) * width;
            for (uint32_t x = 0; x < width; x += 2, a += 4, b += 4, c += 2)
            {
                y0[x] = a[0];
                y0[x + 1] = a[2];
                y1[x] = b[0];
                y1[x + 1] = b[2];
                c[0] = static_cast<uint8_t>((a[1] + b[1] + 1) >> 1);
                c[1] = static_cast<uint8_t>((a[3] + b[3] + 1) >> 1);
            }
        }
    }

    // FNV-1a over both planes: a video source's frames are compared by it run to run.
    uint64_t Hash() const
    {
        uint64_t h = 1469598103934665603ull;
        auto add = [&h](const std::vector<uint8_t>& v) {
            for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
        };
        h = (h ^ width) * 1099511628211ull;
        h = (h ^ height) * 1099511628211ull;
        add(y);
        add(uv);
        return h;
    }
};

// NV12 -> RGBA8, box-downscaled by an integer factor (the editor preview: <= 480 wide).
// BT.601 limited range, what webcams and the decoder produce.
inline void Nv12ToRgbaDownscaled(const FaceImage& src, uint32_t factor, std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH)
{
    factor = factor == 0 ? 1 : factor;
    outW = src.width / factor;
    outH = src.height / factor;
    rgba.resize(static_cast<size_t>(outW) * outH * 4);
    for (uint32_t oy = 0; oy < outH; ++oy)
        for (uint32_t ox = 0; ox < outW; ++ox)
        {
            const uint32_t sx = ox * factor + factor / 2, sy = oy * factor + factor / 2;
            const int Y = src.y[static_cast<size_t>(sy) * src.width + sx];
            const size_t c = static_cast<size_t>(sy / 2) * src.width + (sx & ~1u);
            const int U = src.uv[c] - 128, V = src.uv[c + 1] - 128;
            const int C = (Y - 16) * 298;
            auto clamp8 = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
            uint8_t* p = &rgba[(static_cast<size_t>(oy) * outW + ox) * 4];
            p[0] = clamp8((C + 409 * V + 128) >> 8);
            p[1] = clamp8((C - 100 * U - 208 * V + 128) >> 8);
            p[2] = clamp8((C + 516 * U + 128) >> 8);
            p[3] = 255;
        }
}

} // namespace FaceTracking
