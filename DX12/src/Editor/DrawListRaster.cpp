// DrawListRaster.cpp - see Editor/DrawListRaster.h. The Mesh Editor's shots
// and the chain overlay golden rasterize through this one function: a change
// here changes both.

#include "Editor/DrawListRaster.h"

#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <DirectXTex.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

namespace Editor
{

long long RasterizeDrawListToPng(const ImDrawList& dl, std::uint32_t w, std::uint32_t h,
                                 const std::string& path, std::uint32_t* outTris,
                                 std::string* err)
{
    auto fail = [&](const char* why) { if (err) *err = why; return -1ll; };
    if (w == 0 || h == 0 || w > 8192 || h > 8192) return fail("bad size");

    // RGBA8, opaque black; src-over blending in float, per triangle, in
    // submission order (the painter's order the caller produced).
    std::vector<float> acc(static_cast<std::size_t>(w) * h * 3, 0.f);
    std::vector<std::uint8_t> touched(static_cast<std::size_t>(w) * h, 0);
    std::uint32_t tris = 0;
    for (const ImDrawCmd& cmd : dl.CmdBuffer)
    {
        if (cmd.UserCallback) continue;
        for (unsigned int k = 0; k + 2 < cmd.ElemCount; k += 3)
        {
            const ImDrawVert* v[3];
            bool okIdx = true;
            for (int q = 0; q < 3; ++q)
            {
                const unsigned int vi = cmd.VtxOffset + dl.IdxBuffer[static_cast<int>(cmd.IdxOffset + k) + q];
                if (vi >= static_cast<unsigned int>(dl.VtxBuffer.Size)) { okIdx = false; break; }
                v[q] = &dl.VtxBuffer[static_cast<int>(vi)];
            }
            if (!okIdx) continue;
            ++tris;
            const float x0 = v[0]->pos.x, y0 = v[0]->pos.y, x1 = v[1]->pos.x, y1 = v[1]->pos.y,
                        x2 = v[2]->pos.x, y2 = v[2]->pos.y;
            const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (std::fabs(area) < 1e-12f) continue;
            const int minX = std::max(0, static_cast<int>(std::floor(std::min({ x0, x1, x2 }))));
            const int maxX = std::min(static_cast<int>(w) - 1, static_cast<int>(std::ceil(std::max({ x0, x1, x2 }))));
            const int minY = std::max(0, static_cast<int>(std::floor(std::min({ y0, y1, y2 }))));
            const int maxY = std::min(static_cast<int>(h) - 1, static_cast<int>(std::ceil(std::max({ y0, y1, y2 }))));
            float cr[3], cg[3], cb[3], ca[3];
            for (int q = 0; q < 3; ++q)
            {
                const ImU32 c = v[q]->col;
                cr[q] = static_cast<float>((c >> IM_COL32_R_SHIFT) & 0xFFu) / 255.f;
                cg[q] = static_cast<float>((c >> IM_COL32_G_SHIFT) & 0xFFu) / 255.f;
                cb[q] = static_cast<float>((c >> IM_COL32_B_SHIFT) & 0xFFu) / 255.f;
                ca[q] = static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFFu) / 255.f;
            }
            for (int py = minY; py <= maxY; ++py)
                for (int px = minX; px <= maxX; ++px)
                {
                    const float sx = static_cast<float>(px) + 0.5f, sy = static_cast<float>(py) + 0.5f;
                    const float w0 = ((x1 - sx) * (y2 - sy) - (x2 - sx) * (y1 - sy)) / area;
                    const float w1 = ((x2 - sx) * (y0 - sy) - (x0 - sx) * (y2 - sy)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
                    const float a = w0 * ca[0] + w1 * ca[1] + w2 * ca[2];
                    if (!(a > 0.f)) continue;
                    const std::size_t i = static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px);
                    const float r = w0 * cr[0] + w1 * cr[1] + w2 * cr[2];
                    const float g = w0 * cg[0] + w1 * cg[1] + w2 * cg[2];
                    const float b = w0 * cb[0] + w1 * cb[1] + w2 * cb[2];
                    acc[i * 3 + 0] = r * a + acc[i * 3 + 0] * (1.f - a);
                    acc[i * 3 + 1] = g * a + acc[i * 3 + 1] * (1.f - a);
                    acc[i * 3 + 2] = b * a + acc[i * 3 + 2] * (1.f - a);
                    touched[i] = 1;
                }
        }
    }
    if (outTris) *outTris = tris;

    std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
    long long lit = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
    {
        for (int c = 0; c < 3; ++c)
            px[i * 4 + c] = static_cast<std::uint8_t>(
                std::clamp(acc[i * 3 + c] * 255.f + 0.5f, 0.f, 255.f));
        px[i * 4 + 3] = 255;
        lit += touched[i] ? 1 : 0;
    }

    {
        std::error_code ec;
        std::filesystem::create_directories(Sys::U8Path(path).parent_path(), ec);
    }
    DirectX::Image img{};
    img.width      = w;
    img.height     = h;
    img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    img.rowPitch   = static_cast<std::size_t>(w) * 4;
    img.slicePitch = img.rowPitch * h;
    img.pixels     = px.data();
    const std::wstring wpath = Sys::U8Path(path).wstring();
    const HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
                                              DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                              wpath.c_str());
    if (FAILED(hr)) return fail("SaveToWICFile failed");
    return lit;
}

} // namespace Editor
