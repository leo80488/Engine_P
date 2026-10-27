#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

// CaptureTextureToPNG / SaveTextureCubeToITEX / LoadITEXIntoTextureCube:
// synchronous GPU-readback I/O for the harness and editor-window screenshots and the
// reflection-probe bake persistence. Each one FlushAndWaits, then records and
// submits its own one-shot command list on the graphics queue, so each opens with
// a CanHardSyncNow() check and returns false when it fails (false is already their
// published failure result; Resize, by contrast, must latch). Called after pass
// recording started, that list would execute before this frame's recorded lists
// and re-state the texture under them, and the INFINITE fence wait would stall
// with the primary command list open.
// CanHardSyncNow() is not `!IsRecordingFrame()`: it also admits a mid-frame call
// made before any pass recorded (primary CL only, no parallel Phase B), which is
// where the two probe callers live: EditorLayer::ProcessPendingSaveScene (probe
// export) and Renderer::BuildScene_UploadProbes (probe disk restore, from
// Renderer::BeginFrame). An ImGui button handler is refused.

// ==== CaptureTextureToPNG ====
// Synchronous read-back of a 2D texture into a PNG file, for
// Renderer::CaptureViewportToPNG (the harness screenshots) and the editor windows'
// shot flags. The source format is not fixed: HDRCompositionFormat() picks
// R8G8B8A8_UNORM in SDR but R16G16B16A16_FLOAT in either HDR mode, so wider
// formats are read back and down-converted here (see StoredToDisplayByte).

#include "DirectXTex.h"
#include "Resource/AssetHeader.h"   // .itex container: AssetHeader + TextureMetadata
#include "Resource/AssetFS.h"       // pak-first read for baked probe cubemaps
#include <DirectXPackedVector.h>    // XMConvertHalfToFloat: the FP16 decode
#include <cstdio>
#include <fstream>
#include <unordered_set>

namespace
{
// Composition-space value -> 8-bit PNG byte.
// Do not add a tone map or a gamma curve here: both would double-apply. The
// composition space (Graphics/HDROutput.h) stores
//     stored = pow(linearNits / paperWhiteNits, 1/2.2)
// in every mode, so it is already gamma-encoded and display-referred: 1.0 is paper
// white, < 1.0 the SDR range, > 1.0 HDR headroom. The SDR path's RGBA8 buffer
// holds the same encode, clamped. So
//     byte = round(saturate(stored) * 255)
// reproduces the SDR screenshot bit for bit and clips an HDR one's headroom to
// paper white, as an SDR display does, so HDR and SDR captures of a frame agree.
// NaN folds to 0 (a NaN -> uint8_t cast is UB, and a stray 255 would read as a
// blown highlight). scRGB's negative values (out-of-Rec.709 color) clamp to 0, as
// the format's own gamut mapping does.
inline uint8_t StoredToDisplayByte(float stored)
{
    if (!(stored > 0.0f)) return 0;     // false for NaN and for negatives
    if (stored >= 1.0f)   return 255;   // clip the headroom, do NOT tone map
    return static_cast<uint8_t>(stored * 255.0f + 0.5f);
}
} // namespace

bool GraphicsDX12::CaptureTextureToPNG(const RHI::Texture& tex,
                                       RHI::ResourceState  currentState,
                                       const char*         path)
{
    if (!path || !*path)
    {
        LOG_ERROR("CaptureTextureToPNG: null/empty path");
        return false;
    }

    // Park first, then test: CanHardSyncNow() reads m_inFrame, which the render
    // thread owns, so testing before parking is a TOCTOU - the answer can flip before
    // the flush. The harness capture runs in TickPhase::PostRender, after the seam's
    // kick, so this race is real.
    DrainFrameIfNeeded(Diag::DrainCause::Capture);

    // See the frame-boundary note at the top of this file.
    if (!ENGINE_CHECK_ONCE(CanHardSyncNow(),
            "CaptureTextureToPNG('%s') called after pass recording started "
            "(cmdLists=%u, parallelPhaseB=%d) — refused. It flushes the graphics "
            "queue and submits its own one-shot command list, which would run "
            "BEFORE this frame's recorded lists and transition the source "
            "texture out from under them. Queue the capture and run it from a "
            "frame-boundary drain (e.g. EditorLayer::ProcessPendingActions).",
            path,
            m_commandListCount.load(std::memory_order_relaxed),
            static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire))))
        return false;

    ID3D12Resource* res = GetTextureResource(tex);
    if (!res)
    {
        LOG_ERROR("CaptureTextureToPNG: source texture has no D3D12 resource");
        return false;
    }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        LOG_ERROR("CaptureTextureToPNG: only Texture2D supported (got dim=%d)",
                  desc.Dimension);
        return false;
    }
    // Two tiers of accepted format:
    //   * 8-bit: handed to DirectXTex SaveToWICFile untouched.
    //   * wider: read back as-is, then down-converted to R8G8B8A8_UNORM, since PNG
    //     cannot hold the extra range. R16G16B16A16_FLOAT is the composition format
    //     in both HDR modes, so this tier keeps screenshots working with HDR on;
    //     R10G10B10A2_UNORM costs one extra branch.
    const bool needsDownConvert =
        (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
         desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
    if (!needsDownConvert &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    {
        LOG_ERROR("CaptureTextureToPNG: unsupported format %d", desc.Format);
        return false;
    }

    // Compute footprint for a single-subresource readback.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT   numRows      = 0;
    UINT64 rowSizeBytes = 0;
    UINT64 totalSize    = 0;
    m_device->GetCopyableFootprints(&desc, 0, 1, 0,
                                    &footprint, &numRows, &rowSizeBytes, &totalSize);

    // READBACK buffer for the GPU -> CPU copy.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width              = totalSize;
        bd.Height             = 1;
        bd.DepthOrArraySize   = 1;
        bd.MipLevels          = 1;
        bd.Format             = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count   = 1;
        bd.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE,
                &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&readback))))
        {
            LOG_ERROR("CaptureTextureToPNG: readback buffer alloc failed");
            return false;
        }
    }

    // Make sure prior submissions touching the source are done so the state
    // we transition from matches what the renderer last left.
    FlushAndWait();

    // One-shot command list for the copy; no allocator reuse, since this only runs
    // on a tools request.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>     ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>  cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))))
    {
        LOG_ERROR("CaptureTextureToPNG: command allocator alloc failed");
        return false;
    }
    if (FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    {
        LOG_ERROR("CaptureTextureToPNG: command list alloc failed");
        return false;
    }

    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);

    // Transition: caller-provided state -> COPY_SOURCE.
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource         = res;
        src.Type              = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex  = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource         = readback.Get();
        dst.Type              = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint   = footprint;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Transition back so the renderer's tracker stays consistent.
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = fromState;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_queues[0]->ExecuteCommandLists(1, lists);

    // Block until the copy is done: a synchronous read-back is the point.
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        LOG_ERROR("CaptureTextureToPNG: fence alloc failed");
        return false;
    }
    m_queues[0]->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    // Map readback + densify rows (footprint.RowPitch is 256-aligned, the
    // DirectXTex Image we hand to SaveToWICFile wants a tight rowPitch).
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalSize) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        LOG_ERROR("CaptureTextureToPNG: readback Map failed");
        return false;
    }

    const uint64_t denseRowPitch = rowSizeBytes;
    const uint64_t denseSize     = denseRowPitch * static_cast<uint64_t>(numRows);
    std::vector<uint8_t> dense(static_cast<size_t>(denseSize));
    const uint8_t* srcBytes = static_cast<const uint8_t*>(mapped);
    for (UINT row = 0; row < numRows; ++row)
    {
        std::memcpy(dense.data() + row * denseRowPitch,
                    srcBytes + row * footprint.Footprint.RowPitch,
                    static_cast<size_t>(denseRowPitch));
    }

    D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    // Down-convert wide formats to RGBA8 in place of `dense`. From here on everything
    // works off imageFormat / imageRowPitch / imageSlicePitch, so the 8-bit
    // passthrough and the converted path share one save. Per channel this is
    // StoredToDisplayByte: a saturate and a quantize, no tone map, no gamma (read its
    // comment before changing anything here).
    DXGI_FORMAT imageFormat     = footprint.Footprint.Format;
    uint64_t    imageRowPitch   = denseRowPitch;
    uint64_t    imageSlicePitch = denseSize;

    if (needsDownConvert)
    {
        const uint32_t width       = footprint.Footprint.Width;
        const uint64_t dstRowPitch = static_cast<uint64_t>(width) * 4u;
        std::vector<uint8_t> rgba8(static_cast<size_t>(dstRowPitch) * numRows);

        for (UINT y = 0; y < numRows; ++y)
        {
            const uint8_t* srcRow = dense.data() + y * denseRowPitch;
            uint8_t*       dstRow = rgba8.data() + y * dstRowPitch;

            if (footprint.Footprint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                // FP16 decode via DirectXMath's PackedVector helper (ships with DirectXMath,
                // already in the PCH); Jolt's HalfFloatToFloat would drag the physics headers into
                // a capture path.
                const uint16_t* src = reinterpret_cast<const uint16_t*>(srcRow);
                for (uint32_t x = 0; x < width; ++x)
                    for (uint32_t c = 0; c < 4; ++c)
                        dstRow[x * 4 + c] = StoredToDisplayByte(
                            DirectX::PackedVector::XMConvertHalfToFloat(src[x * 4 + c]));
            }
            else // DXGI_FORMAT_R10G10B10A2_UNORM
            {
                // R in bits 0..9, G in 10..19, B in 20..29, A in 30..31. The UNORM decode lands
                // in 0..1, never above paper white, so StoredToDisplayByte only quantizes it.
                const uint32_t* src = reinterpret_cast<const uint32_t*>(srcRow);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t p = src[x];
                    dstRow[x * 4 + 0] = StoredToDisplayByte(((p >>  0) & 0x3FFu) / 1023.0f);
                    dstRow[x * 4 + 1] = StoredToDisplayByte(((p >> 10) & 0x3FFu) / 1023.0f);
                    dstRow[x * 4 + 2] = StoredToDisplayByte(((p >> 20) & 0x3FFu) / 1023.0f);
                    dstRow[x * 4 + 3] = StoredToDisplayByte(((p >> 30) & 0x003u) /    3.0f);
                }
            }
        }

        dense           = std::move(rgba8);
        imageFormat     = DXGI_FORMAT_R8G8B8A8_UNORM;
        imageRowPitch   = dstRowPitch;
        imageSlicePitch = dstRowPitch * numRows;
    }

    // Save via DirectXTex; SaveToWICFile takes a wide path.
    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) { wpath.resize(wlen - 1); MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen); }
    }

    // Create the parent directory (captures/foo.png is the common pick).
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(wpath).parent_path(), ec);
    }

    DirectX::Image img{};
    img.width      = footprint.Footprint.Width;
    img.height     = footprint.Footprint.Height;
    img.format     = imageFormat;
    img.rowPitch   = static_cast<size_t>(imageRowPitch);
    img.slicePitch = static_cast<size_t>(imageSlicePitch);
    img.pixels     = dense.data();

    const HRESULT hr = DirectX::SaveToWICFile(
        img, DirectX::WIC_FLAGS_NONE,
        DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
        wpath.c_str());
    if (FAILED(hr))
    {
        LOG_ERROR("CaptureTextureToPNG: SaveToWICFile failed (hr=0x%08X)",
                  static_cast<unsigned>(hr));
        return false;
    }

    LOG_SUCCESS("CaptureTextureToPNG: saved '%s' (%ux%u)",
                path, img.width, img.height);
    return true;
}

// ==== ReadTextureRaw / ReadHdrSceneRaw ====
// The pipe's capture {"attachment"} (Harness/AttachmentDecode.h): one plane of
// mip 0 of one array slice, read back exactly as stored. CaptureTextureToPNG's
// discipline (park, CanHardSyncNow, flush, a one-shot list, a fence) without its
// format list or conversion: the caller decodes, knowing what the bytes mean (a
// reverse-Z depth, a packed normal, FP16 radiance), none of which a PNG can hold.
bool GraphicsDX12::ReadTextureRaw(const RHI::Texture& tex, RHI::ResourceState currentState,
                                  std::uint32_t plane, RawTexture& out, std::uint32_t arraySlice)
{
    DrainFrameIfNeeded(Diag::DrainCause::Capture);   // park first: see CaptureTextureToPNG
    ID3D12Resource* res = GetTextureResource(tex);
    if (!res)
    {
        out = RawTexture{};
        out.error = "the texture has no D3D12 resource";
        return false;
    }
    return ReadResourceRaw(res, ToD3D12ResourceState(currentState), plane, out, "ReadTextureRaw", arraySlice);
}

bool GraphicsDX12::ReadHdrSceneRaw(RawTexture& out)
{
    DrainFrameIfNeeded(Diag::DrainCause::Capture);
    // Slot 0 by index, not Hdr(): the CURRENT slot is whichever view last
    // pushed its scope, and this is the main view's picture.
    const TextureDX12* main = HdrRecord(0);
    if (!main)
    {
        out = RawTexture{};
        out.error = "the main view has no HDR scene target";
        return false;
    }
    return ReadResourceRaw(main->resource.Get(), main->state, 0, out, "ReadHdrSceneRaw");
}

bool GraphicsDX12::ReadResourceRaw(ID3D12Resource* res, D3D12_RESOURCE_STATES fromState,
                                   std::uint32_t plane, RawTexture& out, const char* who,
                                   std::uint32_t arraySlice)
{
    out = RawTexture{};
    out.plane      = plane;
    out.arraySlice = arraySlice;
    if (!ENGINE_CHECK_ONCE(CanHardSyncNow(),
            "%s called after pass recording started (cmdLists=%u, parallelPhaseB=%d) - refused: "
            "it flushes the queue and submits its own one-shot list, which would run before this "
            "frame's recorded lists. Call it at a frame boundary (the harness's PostRender).",
            who, m_commandListCount.load(std::memory_order_relaxed),
            static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire))))
    {
        out.error = "refused: pass recording has started - read textures back at a frame boundary";
        return false;
    }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    {
        out.error = "not a 2D texture";
        return false;
    }
    if (desc.SampleDesc.Count > 1)
    {
        out.error = "multisampled - a resolve decides what one texel is, and this reads what is stored";
        return false;
    }
    const UINT planes = D3D12GetFormatPlaneCount(m_device.Get(), desc.Format);
    if (plane >= planes)
    {
        out.error = "plane " + std::to_string(plane) + " of a format with " + std::to_string(planes) + " plane(s)";
        return false;
    }
    if (arraySlice >= desc.DepthOrArraySize)
    {
        out.error = "array slice " + std::to_string(arraySlice) + " of a texture with " +
                    std::to_string(desc.DepthOrArraySize) + " slice(s)";
        return false;
    }
    const UINT sub = D3D12CalcSubresource(0, arraySlice, plane, desc.MipLevels, desc.DepthOrArraySize);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT   rows     = 0;
    UINT64 rowBytes = 0;
    UINT64 total    = 0;
    m_device->GetCopyableFootprints(&desc, sub, 1, 0, &fp, &rows, &rowBytes, &total);
    if (total == 0 || rows == 0 || fp.Footprint.Width == 0 || rowBytes % fp.Footprint.Width != 0)
    {
        out.error = "no texel-addressable copy footprint for this format";
        return false;
    }

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = total;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback))))
        {
            out.error = "readback buffer allocation failed";
            return false;
        }
    }

    FlushAndWait();

    ComPtr<ID3D12CommandAllocator>    ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr,
                                           IID_PPV_ARGS(&cl))))
    {
        out.error = "command list allocation failed";
        return false;
    }

    // Through COPY_SOURCE and back to the state the tracker holds, so the
    // renderer's own bookkeeping stays true.
    const bool transition = fromState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    };
    if (transition) barrier(fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = res;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = sub;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (transition) barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, fromState);
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_queues[0]->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        out.error = "fence allocation failed";
        return false;
    }
    m_queues[0]->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt)
        {
            out.error = "event creation failed";
            return false;
        }
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(total) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        out.error = "readback Map failed";
        return false;
    }
    // Rows packed tight: the footprint's RowPitch is 256-aligned.
    out.bytes.resize(static_cast<size_t>(rowBytes) * rows);
    const std::uint8_t* srcBytes = static_cast<const std::uint8_t*>(mapped);
    for (UINT r = 0; r < rows; ++r)
        std::memcpy(out.bytes.data() + static_cast<size_t>(rowBytes) * r,
                    srcBytes + static_cast<size_t>(fp.Footprint.RowPitch) * r, static_cast<size_t>(rowBytes));
    const D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    out.width          = fp.Footprint.Width;
    out.height         = rows;
    out.bytesPerTexel  = static_cast<std::uint32_t>(rowBytes / fp.Footprint.Width);
    out.resourceFormat = static_cast<std::uint32_t>(desc.Format);
    out.planeFormat    = static_cast<std::uint32_t>(fp.Footprint.Format);
    return true;
}

// A picture's metadata: a few bytes of a GPU buffer (the exposure the tonemap
// read), under the same discipline as ReadResourceRaw.
bool GraphicsDX12::ReadBufferRaw(const RHI::GPUBuffer& buffer, RHI::ResourceState currentState,
                                 std::uint64_t offset, std::uint64_t size,
                                 std::vector<std::uint8_t>& out, std::string& error)
{
    out.clear();
    DrainFrameIfNeeded(Diag::DrainCause::Capture);
    if (!ENGINE_CHECK_ONCE(CanHardSyncNow(),
            "ReadBufferRaw called after pass recording started (cmdLists=%u, parallelPhaseB=%d) - refused: "
            "call it at a frame boundary (the harness's PostRender).",
            m_commandListCount.load(std::memory_order_relaxed),
            static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire))))
    {
        error = "refused: pass recording has started - read buffers back at a frame boundary";
        return false;
    }
    ID3D12Resource* res = GetBufferResource(buffer);
    if (!res)
    {
        error = "the buffer has no D3D12 resource";
        return false;
    }
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (size == 0 || offset > desc.Width || size > desc.Width - offset)
    {
        error = "bytes " + std::to_string(offset) + "+" + std::to_string(size) + " of a " +
                std::to_string(desc.Width) + "-byte buffer";
        return false;
    }

    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(size);
        if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback))))
        {
            error = "readback buffer allocation failed";
            return false;
        }
    }

    FlushAndWait();

    ComPtr<ID3D12CommandAllocator>    ca;
    ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr,
                                           IID_PPV_ARGS(&cl))))
    {
        error = "command list allocation failed";
        return false;
    }
    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);
    const bool transition = fromState != D3D12_RESOURCE_STATE_COPY_SOURCE;
    auto barrier = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        const D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(res, before, after);
        cl->ResourceBarrier(1, &b);
    };
    if (transition) barrier(fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(readback.Get(), 0, res, offset, size);
    if (transition) barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, fromState);
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_queues[0]->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    {
        error = "fence allocation failed";
        return false;
    }
    m_queues[0]->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt)
        {
            error = "event creation failed";
            return false;
        }
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }
    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(size) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    {
        error = "readback Map failed";
        return false;
    }
    out.assign(static_cast<const std::uint8_t*>(mapped), static_cast<const std::uint8_t*>(mapped) + size);
    const D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);
    return true;
}

// ==== Cubemap-array slice <-> .itex: reflection-probe bake persistence ====
// Probe cubemaps are stored in the engine's .itex container
// ([AssetHeader][TextureMetadata][DDS bytes]), as TextureImporter writes and
// TextureLoader reads, so they are ordinary engine assets. One cube = 6
// consecutive array slices x all mips; a Texture2D array's subresource is
// arraySlice*mipLevels + mip, so cube N owns the contiguous block
// [N*6*mipLevels, +6*mipLevels). Both helpers are synchronous (FlushAndWait +
// one-shot CL + fence), like CaptureTextureToPNG; see the header for call-site
// rules.

bool GraphicsDX12::SaveTextureCubeToITEX(const RHI::Texture& cubeArrayTex,
                                         uint32_t            cubeIndex,
                                         RHI::ResourceState  currentState,
                                         const char*         path)
{
    if (!path || !*path) { LOG_ERROR("SaveTextureCubeToITEX: null/empty path"); return false; }

    // See the frame-boundary note at the top of this file. The caller chain is
    // Renderer::ExportBakedProbeCubemaps <- EditorLayer::SaveSceneToPath, run from
    // ProcessPendingSaveScene (frame-boundary drain, primary CL only), not from the
    // File menu / Ctrl+S handler.
    if (!ENGINE_CHECK_ONCE(CanHardSyncNow(),
            "SaveTextureCubeToITEX('%s') called after pass recording started "
            "(cmdLists=%u, parallelPhaseB=%d) — refused, the probe was NOT "
            "exported. It flushes the graphics queue and submits its own "
            "one-shot command list ahead of this frame's recorded lists. Route "
            "scene saving through EditorLayer::RequestSaveScene so the write "
            "lands in the ProcessPendingActions drain.",
            path,
            m_commandListCount.load(std::memory_order_relaxed),
            static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire))))
        return false;

    ID3D12Resource* res = GetTextureResource(cubeArrayTex);
    if (!res) { LOG_ERROR("SaveTextureCubeToITEX: texture has no D3D12 resource"); return false; }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
    { LOG_ERROR("SaveTextureCubeToITEX: source is not a Texture2D array"); return false; }

    const uint32_t mipLevels = desc.MipLevels;
    const uint32_t firstFace = cubeIndex * 6;
    if (firstFace + 6 > desc.DepthOrArraySize)
    {
        LOG_ERROR("SaveTextureCubeToITEX: cubeIndex %u out of range (arraySize=%u)",
                  cubeIndex, desc.DepthOrArraySize);
        return false;
    }

    const uint32_t subCount = 6u * mipLevels;
    const uint32_t firstSub = firstFace * mipLevels;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subCount);
    std::vector<UINT>   numRows(subCount);
    std::vector<UINT64> rowSizes(subCount);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&desc, firstSub, subCount, 0,
                                    footprints.data(), numRows.data(),
                                    rowSizes.data(), &totalSize);

    // READBACK buffer for the GPU -> CPU copy of all 6 x mips subresources.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalSize;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
        { LOG_ERROR("SaveTextureCubeToITEX: readback buffer alloc failed"); return false; }
    }

    FlushAndWait();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    { LOG_ERROR("SaveTextureCubeToITEX: command list alloc failed"); return false; }

    const D3D12_RESOURCE_STATES fromState = ToD3D12ResourceState(currentState);

    // Per-subresource transitions: only this cube's slices, leaving the rest of the
    // shared probe array untouched.
    std::vector<D3D12_RESOURCE_BARRIER> toCopy(subCount), backState(subCount);
    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_RESOURCE_BARRIER& b = toCopy[i];
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = firstSub + i;
        backState[i] = b;
        std::swap(backState[i].Transition.StateBefore, backState[i].Transition.StateAfter);
    }
    cl->ResourceBarrier(subCount, toCopy.data());

    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = res;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = firstSub + i;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprints[i];
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    cl->ResourceBarrier(subCount, backState.data());
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_queues[0]->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    { LOG_ERROR("SaveTextureCubeToITEX: fence alloc failed"); return false; }
    m_queues[0]->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalSize) };
    if (FAILED(readback->Map(0, &readRange, &mapped)))
    { LOG_ERROR("SaveTextureCubeToITEX: readback Map failed"); return false; }

    // Assemble a DirectXTex cube (6 faces x mipLevels), densifying each row from the
    // 256-aligned readback footprint into the tight DDS row pitch.
    DirectX::ScratchImage scratch;
    HRESULT hr = scratch.InitializeCube(desc.Format,
        footprints[0].Footprint.Width, footprints[0].Footprint.Height, 1, mipLevels);
    if (FAILED(hr))
    {
        D3D12_RANGE wn{ 0, 0 }; readback->Unmap(0, &wn);
        LOG_ERROR("SaveTextureCubeToITEX: InitializeCube failed (hr=0x%08X)", (unsigned)hr);
        return false;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t i = face * mipLevels + mip;
            const DirectX::Image* img = scratch.GetImage(mip, face, 0);
            if (!img) continue;
            const uint8_t* srcRows = base + footprints[i].Offset;
            const size_t   copyPitch =
                static_cast<size_t>(std::min<UINT64>(rowSizes[i], img->rowPitch));
            for (UINT row = 0; row < numRows[i]; ++row)
                std::memcpy(img->pixels + row * img->rowPitch,
                            srcRows + row * footprints[i].Footprint.RowPitch,
                            copyPitch);
        }

    D3D12_RANGE writeNothing{ 0, 0 };
    readback->Unmap(0, &writeNothing);

    // Encode the cube to a DDS blob in memory, then wrap it in the engine's .itex
    // container ([AssetHeader][TextureMetadata][DDS bytes]) like
    // Resource::TextureImporter does.
    DirectX::Blob ddsBlob;
    hr = DirectX::SaveToDDSMemory(scratch.GetImages(), scratch.GetImageCount(),
                                  scratch.GetMetadata(), DirectX::DDS_FLAGS_NONE, ddsBlob);
    if (FAILED(hr))
    {
        LOG_ERROR("SaveTextureCubeToITEX: SaveToDDSMemory failed (hr=0x%08X)", (unsigned)hr);
        return false;
    }

    const DirectX::TexMetadata& m = scratch.GetMetadata();
    Resource::TextureMetadata texMeta{};
    texMeta.width     = static_cast<uint32_t>(m.width);
    texMeta.height    = static_cast<uint32_t>(m.height);
    texMeta.depth     = static_cast<uint32_t>(m.depth);
    texMeta.mipLevels = static_cast<uint16_t>(m.mipLevels);
    texMeta.format    = static_cast<uint16_t>(m.format);
    texMeta.dimension = 6; // Cube

    Resource::AssetHeader header{};
    header.magic        = Resource::MAGIC_TEXTURE;
    header.version      = Resource::CurrentVersion(Resource::MAGIC_TEXTURE);
    header.resourceType = static_cast<uint16_t>(Resource::ResourceType::Texture);
    header.metadataSize = sizeof(Resource::TextureMetadata);
    header.dataSize     = static_cast<uint32_t>(ddsBlob.GetBufferSize());

    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, nullptr, 0);
        if (wlen > 0) { wpath.resize(wlen - 1); MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath.data(), wlen); }
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(wpath).parent_path(), ec);
    }

    std::ofstream out(wpath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LOG_ERROR("SaveTextureCubeToITEX: cannot open '%s' for writing", path);
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header),  sizeof(header));
    out.write(reinterpret_cast<const char*>(&texMeta), sizeof(texMeta));
    out.write(reinterpret_cast<const char*>(ddsBlob.GetBufferPointer()),
              static_cast<std::streamsize>(ddsBlob.GetBufferSize()));
    if (!out)
    {
        LOG_ERROR("SaveTextureCubeToITEX: write failed for '%s'", path);
        return false;
    }

    LOG_SUCCESS("SaveTextureCubeToITEX: saved '%s' (cube %u, %u mips)",
                path, cubeIndex, mipLevels);
    return true;
}

bool GraphicsDX12::LoadITEXIntoTextureCube(RHI::Texture&      cubeArrayTex,
                                           uint32_t           cubeIndex,
                                           RHI::ResourceState currentState,
                                           const char*        path)
{
    if (!path || !*path) { LOG_ERROR("LoadITEXIntoTextureCube: null/empty path"); return false; }

    // See the frame-boundary note at the top of this file. The live caller,
    // Renderer::BuildScene_UploadProbes (<- Renderer::BeginFrame), runs with the
    // primary CL open and nothing else recorded, which CanHardSyncNow() admits. It is
    // still a per-frame disk read plus a full queue stall mid scene build, throttled
    // only by kMaxProbeDiskLoadsPerFrame; a frame-boundary servicer would remove the
    // stall, and this guard fires if the call ever moves later in the frame.
    if (!ENGINE_CHECK_ONCE(CanHardSyncNow(),
            "LoadITEXIntoTextureCube('%s') called after pass recording started "
            "(cmdLists=%u, parallelPhaseB=%d) — refused, the probe keeps its "
            "un-restored contents. It flushes the graphics queue and submits its "
            "own one-shot command list ahead of this frame's recorded lists.",
            path,
            m_commandListCount.load(std::memory_order_relaxed),
            static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire))))
        return false;

    ID3D12Resource* res = GetTextureResource(cubeArrayTex);
    if (!res) { LOG_ERROR("LoadITEXIntoTextureCube: texture has no D3D12 resource"); return false; }

    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    const uint32_t mipLevels = desc.MipLevels;
    const uint32_t firstFace = cubeIndex * 6;
    if (firstFace + 6 > desc.DepthOrArraySize)
    { LOG_ERROR("LoadITEXIntoTextureCube: cubeIndex %u out of range", cubeIndex); return false; }

    // Read the whole .itex blob through AssetFS (game.ipak first, then loose disk; in
    // a packed build the baked probe lives only in the pak), then parse it like
    // TextureLoader: ValidateHeader -> GetPayload -> LoadFromDDSMemory.
    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(path, blob) || blob.empty())
    {
        LOG_WARNING("LoadITEXIntoTextureCube: cannot read '%s'", path);
        return false;
    }

    if (!Resource::ValidateHeader(blob.data(), blob.size(), Resource::MAGIC_TEXTURE))
    {
        LOG_WARNING("LoadITEXIntoTextureCube: '%s' is not a valid .itex texture", path);
        return false;
    }
    const Resource::AssetHeader* hdr     = Resource::GetHeader(blob.data());
    const uint8_t*               ddsData = Resource::GetPayload(blob.data());

    DirectX::TexMetadata  meta{};
    DirectX::ScratchImage scratch;
    HRESULT hr = DirectX::LoadFromDDSMemory(ddsData, hdr->dataSize,
                                            DirectX::DDS_FLAGS_NONE, &meta, scratch);
    if (FAILED(hr))
    {
        LOG_WARNING("LoadITEXIntoTextureCube: LoadFromDDSMemory('%s') failed (hr=0x%08X)",
                    path, (unsigned)hr);
        return false;
    }

    // Strict layout match: there is no resize / convert path, and a mismatch makes
    // the caller fall back to a fresh bake.
    if (!meta.IsCubemap() || meta.arraySize < 6 ||
        meta.width     != desc.Width  ||
        meta.height    != desc.Height ||
        meta.mipLevels != mipLevels   ||
        meta.format    != desc.Format)
    {
        LOG_WARNING("LoadITEXIntoTextureCube: '%s' layout mismatch — "
                    "got %zux%zu mips=%zu fmt=%d cube=%d; want %llux%llu mips=%u fmt=%d",
                    path, meta.width, meta.height, meta.mipLevels, (int)meta.format,
                    meta.IsCubemap() ? 1 : 0,
                    (unsigned long long)desc.Width, (unsigned long long)desc.Height,
                    mipLevels, (int)desc.Format);
        return false;
    }

    const uint32_t subCount = 6u * mipLevels;
    const uint32_t firstSub = firstFace * mipLevels;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subCount);
    std::vector<UINT>   numRows(subCount);
    std::vector<UINT64> rowSizes(subCount);
    UINT64 totalSize = 0;
    m_device->GetCopyableFootprints(&desc, firstSub, subCount, 0,
                                    footprints.data(), numRows.data(),
                                    rowSizes.data(), &totalSize);

    // UPLOAD staging buffer, filled with the DDS subresources repitched to the
    // 256-aligned footprint row pitch.
    Microsoft::WRL::ComPtr<ID3D12Resource> upload;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width            = totalSize;
        bd.Height           = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels        = 1;
        bd.Format           = DXGI_FORMAT_UNKNOWN;
        bd.SampleDesc.Count = 1;
        bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))))
        { LOG_ERROR("LoadITEXIntoTextureCube: upload buffer alloc failed"); return false; }
    }

    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(upload->Map(0, &noRead, &mapped)))
    { LOG_ERROR("LoadITEXIntoTextureCube: upload Map failed"); return false; }

    uint8_t* base = static_cast<uint8_t*>(mapped);
    for (uint32_t face = 0; face < 6; ++face)
        for (uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            const uint32_t i = face * mipLevels + mip;
            const DirectX::Image* img = scratch.GetImage(mip, face, 0);
            if (!img) continue;
            uint8_t*     dstRows   = base + footprints[i].Offset;
            const size_t copyPitch =
                static_cast<size_t>(std::min<UINT64>(rowSizes[i], img->rowPitch));
            for (UINT row = 0; row < numRows[i]; ++row)
                std::memcpy(dstRows + row * footprints[i].Footprint.RowPitch,
                            img->pixels + row * img->rowPitch,
                            copyPitch);
        }
    upload->Unmap(0, nullptr);

    FlushAndWait();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    ca;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    if (FAILED(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca))) ||
        FAILED(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca.Get(), nullptr, IID_PPV_ARGS(&cl))))
    { LOG_ERROR("LoadITEXIntoTextureCube: command list alloc failed"); return false; }

    const D3D12_RESOURCE_STATES homeState = ToD3D12ResourceState(currentState);

    std::vector<D3D12_RESOURCE_BARRIER> toCopy(subCount), backState(subCount);
    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_RESOURCE_BARRIER& b = toCopy[i];
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = homeState;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = firstSub + i;
        backState[i] = b;
        std::swap(backState[i].Transition.StateBefore, backState[i].Transition.StateAfter);
    }
    cl->ResourceBarrier(subCount, toCopy.data());

    for (uint32_t i = 0; i < subCount; ++i)
    {
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource       = upload.Get();
        src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[i];
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource        = res;
        dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = firstSub + i;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    cl->ResourceBarrier(subCount, backState.data());
    cl->Close();
    ID3D12CommandList* lists[] = { cl.Get() };
    m_queues[0]->ExecuteCommandLists(1, lists);

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
    { LOG_ERROR("LoadITEXIntoTextureCube: fence alloc failed"); return false; }
    m_queues[0]->Signal(fence.Get(), 1);
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt) return false;
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, INFINITE);
        CloseHandle(evt);
    }

    LOG_SUCCESS("LoadITEXIntoTextureCube: loaded '%s' into cube %u (%u mips)",
                path, cubeIndex, mipLevels);
    return true;
}

// ==== Back-buffer capture ====
// -ui_screenshot (the presented back buffer) and the pipe's
// `capture {"source": "app_ui"}`, in either exe.
// No other capture sees the editor's ImGui layer: -screenshot is the ToneMap
// output, -preview_screenshot / -chainpreview_shot are preview targets, and the
// FLIP_DISCARD swap chain's back buffer is undefined once Present has run. So the
// copy rides this frame's primary command list after everything (ImGui included)
// has drawn, in place of its RENDER_TARGET -> PRESENT barrier; the PNG is written
// after Present, once a private fence says the GPU got there. It stalls that one
// frame: harness callers only. Every request ends in TakeBackBufferShot's mailbox,
// written or not, so a waiting caller is never left waiting.
// SurfaceDX12 members (the back buffer is the surface's), defined here beside the
// other captures and their pixel helpers.
namespace
{
#if defined(WITH_HARNESS) && WITH_HARNESS
    // -present_probe=<x>,<y>,<w>,<h>[;<x>,<y>,<w>,<h>...] (harness): one [PRESENT_PROBE] line per back-buffer
    // capture, the n-th capture reading the n-th box (the last one repeats) - the raw codes of the box
    // (R10G10B10A2: 0..1023; FP16: the half's value; 8-bit: 0..255) per channel: min, max, mean and how many
    // distinct codes it holds. The PNG is 8 bits and holds none of a 10-bit signal's last two, which is
    // where the HDR10 dither lives.
    void LogPresentProbe(const std::string& path, DXGI_FORMAT fmt, const uint8_t* base, UINT rowPitch, UINT w, UINT h)
    {
        static const std::string s_spec = Sys::CommandLine::Get().Get("-present_probe", "");
        static size_t s_captures = 0;
        if (s_spec.empty()) return;
        std::vector<std::string> boxes;
        for (size_t at = 0; at <= s_spec.size();)
        {
            size_t semi = s_spec.find(';', at);
            if (semi == std::string::npos) semi = s_spec.size();
            boxes.push_back(s_spec.substr(at, semi - at));
            at = semi + 1;
        }
        const std::string& spec = boxes[(std::min)(s_captures++, boxes.size() - 1)];
        unsigned bx = 0, by = 0, bw = 0, bh = 0;
        if (sscanf_s(spec.c_str(), "%u,%u,%u,%u", &bx, &by, &bw, &bh) != 4) return;
        bw = (std::min)(bw, w - (std::min)(bx, w));
        bh = (std::min)(bh, h - (std::min)(by, h));
        if (bw == 0 || bh == 0) return;
        double sum[3] = {}, lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
        std::unordered_set<uint32_t> codes[3];
        for (unsigned y = by; y < by + bh; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
            for (unsigned x = bx; x < bx + bw; ++x)
                for (int c = 0; c < 3; ++c)
                {
                    uint32_t key = 0;
                    double   v   = 0.0;
                    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
                    {
                        key = reinterpret_cast<const uint16_t*>(row)[x * 4u + c];
                        v   = DirectX::PackedVector::XMConvertHalfToFloat(static_cast<uint16_t>(key));
                    }
                    else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
                    {
                        key = (reinterpret_cast<const uint32_t*>(row)[x] >> (10 * c)) & 0x3FFu;
                        v   = key;
                    }
                    else
                    {
                        key = row[x * 4u + (fmt == DXGI_FORMAT_B8G8R8A8_UNORM ? 2 - c : c)];
                        v   = key;
                    }
                    sum[c] += v;
                    lo[c] = (std::min)(lo[c], v);
                    hi[c] = (std::max)(hi[c], v);
                    codes[c].insert(key);
                }
        }
        const double n = static_cast<double>(bw) * bh;
        const size_t slash = path.find_last_of("/\\");
        const int bits = fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 16 : fmt == DXGI_FORMAT_R10G10B10A2_UNORM ? 10 : 8;
        LOG_INFO("[PRESENT_PROBE] file=%s bits=%d x=%u y=%u w=%u h=%u "
                 "rMin=%.6f rMax=%.6f rMean=%.6f rDistinct=%zu gMin=%.6f gMax=%.6f gMean=%.6f gDistinct=%zu "
                 "bMin=%.6f bMax=%.6f bMean=%.6f bDistinct=%zu",
                 path.substr(slash == std::string::npos ? 0 : slash + 1).c_str(), bits, bx, by, bw, bh,
                 lo[0], hi[0], sum[0] / n, codes[0].size(), lo[1], hi[1], sum[1] / n, codes[1].size(),
                 lo[2], hi[2], sum[2] / n, codes[2].size());
    }
#endif
}

bool SurfaceDX12::RequestBackBufferCapture(const char* path)
{
    if (!path || !*path) return false;
    std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
    if (!m_uiCaptureRequest.empty() || m_uiCapture.armed) return false;
    m_uiCaptureRequest = path;
    return true;
}

bool SurfaceDX12::TakeBackBufferShot(BackBufferShot& out)
{
    std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
    if (m_uiCaptureShots.empty()) return false;
    out = std::move(m_uiCaptureShots.front());
    m_uiCaptureShots.erase(m_uiCaptureShots.begin());
    return true;
}

void SurfaceDX12::NoteBackBufferShot(const std::string& path, bool ok, std::uint32_t w, std::uint32_t h,
                                      std::string error, bool uniform)
{
    std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
    // Bounded: -ui_screenshot's outcomes are never taken.
    if (m_uiCaptureShots.size() >= 8) m_uiCaptureShots.erase(m_uiCaptureShots.begin());
    m_uiCaptureShots.push_back(BackBufferShot{ path, ok, w, h, std::move(error), uniform });
}

bool SurfaceDX12::RecordBackBufferCapture(ID3D12GraphicsCommandList* cl, ID3D12Resource* backBuffer)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
        if (m_uiCaptureRequest.empty() || m_uiCapture.armed) return false;
        path.swap(m_uiCaptureRequest);
    }
    if (!cl || !backBuffer)
    {
        NoteBackBufferShot(path, false, 0, 0, "no command list or back buffer to copy from");
        return false;
    }
    const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
    const DXGI_FORMAT f = desc.Format;
    if (f != DXGI_FORMAT_R8G8B8A8_UNORM && f != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        f != DXGI_FORMAT_B8G8R8A8_UNORM && f != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        f != DXGI_FORMAT_R10G10B10A2_UNORM && f != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        LOG_ERROR("[UI_SCREENSHOT] back buffer format %d is not supported - '%s' not written",
                  static_cast<int>(f), path.c_str());
        NoteBackBufferShot(path, false, 0, 0, "back buffer format " + std::to_string(static_cast<int>(f)) +
                                              " is not supported");
        return false;
    }
    UiCapture cap;
    UINT64 total = 0;
    m_gfx.m_device->GetCopyableFootprints(&desc, 0, 1, 0, &cap.footprint, &cap.rows, &cap.rowBytes, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.Format           = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_gfx.m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&cap.readback))))
    {
        LOG_ERROR("[UI_SCREENSHOT] readback buffer allocation failed - '%s' not written",
                  path.c_str());
        NoteBackBufferShot(path, false, 0, 0, "the readback buffer could not be allocated");
        return false;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = backBuffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    GfxDiag::NoteNativeBarriers(cl, &b, 1);   // the primary list's barrier journal
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = backBuffer;
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource        = cap.readback.Get();
    dst.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint  = cap.footprint;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    GfxDiag::NoteNativeBarriers(cl, &b, 1);
    cap.path  = std::move(path);
    cap.armed = true;
    std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
    m_uiCapture = std::move(cap);
    return true;
}

void SurfaceDX12::FinishBackBufferCapture()
{
    UiCapture cap;
    {
        std::lock_guard<std::mutex> lk(m_uiCaptureMutex);
        if (!m_uiCapture.armed) return;
        cap = std::move(m_uiCapture);
        m_uiCapture = UiCapture{};
    }
    // This frame's lists are submitted: a private fence on the graphics queue
    // passes once the copy (recorded on the primary list) has executed.
    ComPtr<ID3D12Fence> fence;
    if (FAILED(m_gfx.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
        FAILED(m_gfx.m_queues[0]->Signal(fence.Get(), 1)))
    {
        LOG_ERROR("[UI_SCREENSHOT] fence failed - '%s' not written", cap.path.c_str());
        NoteBackBufferShot(cap.path, false, 0, 0, "the fence after the copy failed");
        return;
    }
    if (fence->GetCompletedValue() < 1)
    {
        HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!evt)
        {
            NoteBackBufferShot(cap.path, false, 0, 0, "no event to wait for the copy on");
            return;
        }
        fence->SetEventOnCompletion(1, evt);
        WaitForSingleObject(evt, 10000);
        CloseHandle(evt);
    }
    if (fence->GetCompletedValue() < 1)
    {
        LOG_ERROR("[UI_SCREENSHOT] the GPU did not reach the copy in 10 s - '%s' not written",
                  cap.path.c_str());
        NoteBackBufferShot(cap.path, false, 0, 0, "the GPU did not reach the copy in 10 s");
        return;
    }
    void* mapped = nullptr;
    const UINT64 total = static_cast<UINT64>(cap.footprint.Footprint.RowPitch) * cap.rows;
    D3D12_RANGE rr{ 0, static_cast<SIZE_T>(total) };
    if (FAILED(cap.readback->Map(0, &rr, &mapped)) || !mapped)
    {
        LOG_ERROR("[UI_SCREENSHOT] Map failed - '%s' not written", cap.path.c_str());
        NoteBackBufferShot(cap.path, false, 0, 0, "the readback could not be mapped");
        return;
    }
    // Always RGBA8 with alpha forced opaque: the ImGui layer leaves arbitrary
    // alpha in the back buffer, which a PNG viewer would show as holes.
    const UINT w = cap.footprint.Footprint.Width, h = cap.rows;
    const DXGI_FORMAT fmt = cap.footprint.Footprint.Format;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4u);
    bool uniform = true;   // every pixel the first one's colour: a blank picture
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < h; ++y)
    {
        const uint8_t* row = base + static_cast<size_t>(y) * cap.footprint.Footprint.RowPitch;
        uint8_t*       out = rgba.data() + static_cast<size_t>(y) * w * 4u;
        for (UINT x = 0; x < w; ++x)
        {
            uint8_t* o = out + x * 4u;
            if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(row) + x * 4u;
                for (int c = 0; c < 3; ++c)
                    o[c] = StoredToDisplayByte(DirectX::PackedVector::XMConvertHalfToFloat(p[c]));
            }
            else if (fmt == DXGI_FORMAT_R10G10B10A2_UNORM)
            {
                const uint32_t p = reinterpret_cast<const uint32_t*>(row)[x];
                o[0] = StoredToDisplayByte(((p >>  0) & 0x3FFu) / 1023.0f);
                o[1] = StoredToDisplayByte(((p >> 10) & 0x3FFu) / 1023.0f);
                o[2] = StoredToDisplayByte(((p >> 20) & 0x3FFu) / 1023.0f);
            }
            else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            {
                const uint8_t* p = row + x * 4u;
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
            }
            else
            {
                const uint8_t* p = row + x * 4u;
                o[0] = p[0]; o[1] = p[1]; o[2] = p[2];
            }
            o[3] = 255;
            uniform = uniform && o[0] == rgba[0] && o[1] == rgba[1] && o[2] == rgba[2];
        }
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    LogPresentProbe(cap.path, fmt, base, cap.footprint.Footprint.RowPitch, w, h);
#endif
    D3D12_RANGE none{ 0, 0 };
    cap.readback->Unmap(0, &none);

    std::wstring wpath;
    {
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, cap.path.c_str(), -1, nullptr, 0);
        if (wlen > 0)
        {
            wpath.resize(static_cast<size_t>(wlen - 1));
            MultiByteToWideChar(CP_UTF8, 0, cap.path.c_str(), -1, wpath.data(), wlen);
        }
    }
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(wpath).parent_path(), ec);
    }
    DirectX::Image img{};
    img.width      = w;
    img.height     = h;
    img.format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    img.rowPitch   = static_cast<size_t>(w) * 4u;
    img.slicePitch = img.rowPitch * h;
    img.pixels     = rgba.data();
    const HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
                                              DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG),
                                              wpath.c_str());
    if (FAILED(hr))
    {
        LOG_ERROR("[UI_SCREENSHOT] SaveToWICFile failed (hr=0x%08X) - '%s'",
                  static_cast<unsigned>(hr), cap.path.c_str());
        char why[64];
        std::snprintf(why, sizeof(why), "the PNG could not be written (hr=0x%08X)", static_cast<unsigned>(hr));
        NoteBackBufferShot(cap.path, false, 0, 0, why);
        return;
    }
    LOG_INFO("[UI_SCREENSHOT] wrote '%s' w=%u h=%u format=%d presented=%llu", cap.path.c_str(),
             w, h, static_cast<int>(fmt),
             static_cast<unsigned long long>(m_presentedFrames.load(std::memory_order_relaxed)));
    NoteBackBufferShot(cap.path, true, w, h, {}, uniform);
}
