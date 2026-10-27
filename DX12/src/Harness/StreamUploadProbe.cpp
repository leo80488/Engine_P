// Harness/StreamUploadProbe.cpp - see the header for what it proves.
#include "Harness/StreamUploadProbe.h"

#ifdef WITH_HARNESS
#include "ECS/FrameContext.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/UploadStats.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/TaskSystem.h"   // -streamupload_probe_loader

#include <algorithm>
#include <cstring>
#include <future>

namespace Harness
{
    namespace
    {
        std::vector<UploadReadback>& Readbacks()
        {
            static std::vector<UploadReadback> s_readbacks;
            return s_readbacks;
        }
    }

    void RegisterUploadReadback(UploadReadback readback) { Readbacks().push_back(readback); }
    const std::vector<UploadReadback>& UploadReadbacks() { return Readbacks(); }

    namespace
    {
        // Every texel of frame F: its 24 low bits, and a constant alpha per
        // texture (A and B 0xA5, C 0x5A). The decoy A is far from any frame.
        uint32_t Encode(uint64_t frame)  { return 0xA5000000u | static_cast<uint32_t>(frame & 0xFFFFFFu); }
        uint32_t EncodeC(uint64_t frame) { return 0x5A000000u | static_cast<uint32_t>(frame & 0xFFFFFFu); }
        uint32_t Decoy(uint64_t frame)   { return Encode(frame + 0x400000u); }

        // The 4x4 grid of 16x16 rects: cell @p k's origin.
        uint32_t CellX(uint64_t k) { return static_cast<uint32_t>(k % 4) * 16; }
        uint32_t CellY(uint64_t k) { return static_cast<uint32_t>((k / 4) % 4) * 16; }

        // D's texel (x, y) and E's word i encode where they are, so a copy into the wrong place
        // or the wrong subresource shows, as well as a missing one.
        uint32_t PatternD(uint32_t x, uint32_t y) { return 0xC3000000u | (y << 8) | x; }
        uint32_t PatternE(uint32_t i)             { return 0x3C000000u ^ (i * 2654435761u); }
    }

    StreamUploadProbe& StreamUploadProbe::Get()
    {
        static StreamUploadProbe s_probe;
        return s_probe;
    }

    bool StreamUploadProbe::Requested()
    {
#if defined(WITH_HARNESS) && WITH_HARNESS
        static const bool s_on = Sys::CommandLine::Get().Has("-streamupload_probe") ||
                                 Sys::CommandLine::Get().Has("-streamupload_probe_primary");
        return s_on;
#else
        return false;
#endif
    }

    bool StreamUploadProbe::Init(IGraphicsDevice& gfx)
    {
        m_initTried = true;
        m_primary = Sys::CommandLine::Get().Has("-streamupload_probe_primary");

        // Created once, initial state SHADER_RESOURCE, no initial data - a stream target's
        // rules.
        RHI::TextureDesc td{};
        td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
        td.width      = kSize;
        td.height     = kSize;
        td.depth      = 1;
        td.format     = RHI::Format::R8G8B8A8_UNORM;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        td.debugName = "StreamUploadProbe.A";
        if (!gfx.CreateTexture(td, m_texA)) return false;
        td.debugName = "StreamUploadProbe.B";
        if (!gfx.CreateTexture(td, m_texB)) return false;
        td.debugName = "StreamUploadProbe.C";
        if (!gfx.CreateTexture(td, m_texC)) return false;

        // D and E with initial data: creation copies that go on the COPY queue (unless
        // -upload_directqueue); never stream targets. -streamupload_probe_loader makes them on a
        // TaskSystem worker inside the device's loader upload scope (the load_upload_async
        // pack), so every readback judges that path's copies; loader=1 in the line only when the
        // scope was taken.
        auto makeDE = [&]() -> bool
        {
            std::vector<uint32_t> pix(size_t(kSize) * kSize);
            for (uint32_t y = 0; y < kSize; ++y)
                for (uint32_t x = 0; x < kSize; ++x)
                    pix[size_t(y) * kSize + x] = PatternD(x, y);
            RHI::SubresourceData sd{};
            sd.dataPtr    = pix.data();
            sd.rowPitch   = kSize * 4;
            sd.slicePitch = kSize * kSize * 4;
            td.debugName  = "StreamUploadProbe.D";
            if (!gfx.CreateTexture(td, m_texD, &sd)) return false;

            std::vector<uint32_t> words(kBufBytes / 4);
            for (uint32_t i = 0; i < kBufBytes / 4; ++i) words[i] = PatternE(i);
            RHI::GPUBufferDesc ed{};
            ed.size       = kBufBytes;
            ed.usage      = RHI::Usage::DEFAULT;
            ed.bindFlags = RHI::BindFlag::NONE;   // no view: only ever a copy source
            ed.debugName = "StreamUploadProbe.E";
            if (!gfx.CreateBuffer(ed, m_bufE, words.data())) return false;
            return true;
        };
        bool madeDE = false;
        if (Sys::CommandLine::Get().Has("-streamupload_probe_loader"))
        {
            std::promise<bool> made;
            std::future<bool>  done = made.get_future();
            TaskSystem::Get().Push([&]()
            {
                bool ok = false;
                try
                {
                    LoaderUploadScope scope(gfx);
                    m_loader = scope.Active();
                    ok = makeDE();
                }   // the copies done, on this worker
                catch (...) { ok = false; }
                made.set_value(ok);
            });
            madeDE = done.get();
        }
        else
            madeDE = makeDE();
        if (!madeDE) return false;

        uint32_t rows = 0;
        uint64_t bytes = 0;
        if (!gfx.GetTexture2DReadbackFootprint(m_texA, m_rowPitch, rows, bytes)) return false;
        RHI::GPUBufferDesc bd{};
        bd.size       = bytes;
        bd.usage      = RHI::Usage::READBACK;
        bd.bindFlags = RHI::BindFlag::NONE;
        RHI::GPUBufferDesc be = bd;
        be.size = kBufBytes;
        for (uint32_t s = 0; s < kSlots; ++s)
            if (!gfx.CreateBuffer(bd, m_readA[s]) || !gfx.CreateBuffer(bd, m_readB[s]) ||
                !gfx.CreateBuffer(bd, m_readC[s]) || !gfx.CreateBuffer(bd, m_readD[s]) ||
                !gfx.CreateBuffer(be, m_readE[s]))
                return false;

        m_pixA.assign(size_t(kSize) * kSize, 0u);
        m_pixB.assign(size_t(kRect) * kRect, 0u);
        m_pixC.assign(size_t(kRect) * kRect, 0u);
        LOG_INFO("[UPLOAD_STREAM_PROBE] armed: %ux%u RGBA8 x3 (A whole LatestWins with an in-place merge, B a "
                 "%ux%u MustDeliver rect walking a 4x4 grid, C the same from GameplayPreLogic), reader in the "
                 "GBuffer stage, checks from frame %llu%s",
                 kSize, kSize, kRect, kRect, static_cast<unsigned long long>(kCheckFrom),
                 m_primary ? " - REVERSE ARM: copies in the primary list" : "");
        m_ready.store(true, std::memory_order_release);
        return true;
    }

    bool StreamUploadProbe::ReadRect(const uint8_t* p, uint32_t x, uint32_t y, uint32_t& value) const
    {
        std::memcpy(&value, p + size_t(y) * m_rowPitch + size_t(x) * 4, 4);
        for (uint32_t r = 0; r < kRect; ++r)
        {
            const uint8_t* row = p + size_t(y + r) * m_rowPitch + size_t(x) * 4;
            for (uint32_t c = 0; c < kRect; ++c)
            {
                uint32_t v = 0;
                std::memcpy(&v, row + size_t(c) * 4, 4);
                if (v != value) return false;
            }
        }
        return true;
    }

    void StreamUploadProbe::Check(IGraphicsDevice& gfx, uint32_t s)
    {
        Slot& sl = m_slots[s];
        if (!sl.valid || sl.frame < kCheckFrom) return;
        if (!sl.recorded.load(std::memory_order_acquire)) { ++m_skipped; return; }   // the reader did not run
        const uint32_t want = Encode(sl.frame) & 0xFFFFFFu;

        auto detail = [&](const char* what, uint32_t got, uint32_t expected)
        {
            if (m_detailLogged++ < 6)
                LOG_WARNING("[UPLOAD_STREAM_PROBE] frame %llu %s: read %u, expected %u%s",
                            static_cast<unsigned long long>(sl.frame), what, got, expected,
                            got < expected && expected - got <= 4 ? " (an earlier frame's - the copy came late)" : "");
        };

        if (!sl.aQueued) ++m_skipped;
        else if (const uint8_t* p = static_cast<const uint8_t*>(gfx.MapBuffer(m_readA[s])))
        {
            uint32_t first = 0;
            std::memcpy(&first, p, 4);
            first &= 0xFFFFFFu;
            bool uniform = true;
            for (uint32_t y = 0; y < kSize && uniform; ++y)
            {
                const uint8_t* row = p + size_t(y) * m_rowPitch;
                for (uint32_t x = 0; x < kSize; ++x)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, row + size_t(x) * 4, 4);
                    if ((v & 0xFFFFFFu) != first) { uniform = false; break; }
                }
            }
            gfx.UnmapBuffer(m_readA[s]);
            ++m_checked;
            if (!m_firstChecked) m_firstChecked = sl.frame;
            m_lastChecked = sl.frame;
            if (!uniform) { ++m_torn; detail("A is torn (not one frame's data)", first, want); }
            else if (first != want)
            {
                ++m_mismatches;
                if (first < want && want - first <= 4) ++m_late;
                detail(first == (Decoy(sl.frame) & 0xFFFFFFu) ? "A (the decoy: the merge did not replace it)" : "A",
                       first, want);
            }
        }

        if (!sl.bQueued) ++m_skipped;
        else if (const uint8_t* p = static_cast<const uint8_t*>(gfx.MapBuffer(m_readB[s])))
        {
            uint32_t got = 0;
            const bool uniform = ReadRect(p, sl.bX, sl.bY, got);
            gfx.UnmapBuffer(m_readB[s]);
            got &= 0xFFFFFFu;
            ++m_mustChecked;
            if (!uniform) { ++m_torn; detail("B's rect is torn", got, want); }
            else if (got != want) { ++m_mustMismatches; detail("B's rect", got, want); }
        }

        // C: every rect whose request named this frame.
        const uint8_t* pc = nullptr;
        for (Expected& e : m_expected)
        {
            if (!e.valid || e.frame > sl.frame) continue;
            if (e.frame < sl.frame) { e.valid = false; ++m_skipped; continue; }   // its frame's check never came
            if (!pc) pc = static_cast<const uint8_t*>(gfx.MapBuffer(m_readC[s]));
            if (!pc) break;
            uint32_t got = 0;
            const bool uniform = ReadRect(pc, e.x, e.y, got);
            ++m_cChecked;
            if (!uniform) { ++m_torn; detail("C's rect is torn", got & 0xFFFFFFu, e.value & 0xFFFFFFu); }
            else if (got != e.value) { ++m_cMismatches; detail("C's rect", got & 0xFFFFFFu, e.value & 0xFFFFFFu); }
            e.valid = false;
        }
        if (pc) gfx.UnmapBuffer(m_readC[s]);

        // D and E: the pattern they were created with, every check.
        if (const uint8_t* p = static_cast<const uint8_t*>(gfx.MapBuffer(m_readD[s])))
        {
            uint32_t bad = 0, firstX = 0, firstY = 0, got = 0;
            for (uint32_t y = 0; y < kSize; ++y)
            {
                const uint8_t* row = p + size_t(y) * m_rowPitch;
                for (uint32_t x = 0; x < kSize; ++x)
                {
                    uint32_t v = 0;
                    std::memcpy(&v, row + size_t(x) * 4, 4);
                    if (v != PatternD(x, y) && bad++ == 0) { firstX = x; firstY = y; got = v; }
                }
            }
            gfx.UnmapBuffer(m_readD[s]);
            ++m_dChecked;
            if (bad)
            {
                ++m_dMismatches;
                if (m_detailLogged++ < 6)
                    LOG_WARNING("[UPLOAD_STREAM_PROBE] frame %llu D (created with initial data): %u texels differ, "
                                "first (%u,%u) read 0x%08X expected 0x%08X",
                                static_cast<unsigned long long>(sl.frame), bad, firstX, firstY, got,
                                PatternD(firstX, firstY));
            }
        }
        if (const uint8_t* p = static_cast<const uint8_t*>(gfx.MapBuffer(m_readE[s])))
        {
            uint32_t bad = 0, first = 0;
            for (uint32_t i = 0; i < kBufBytes / 4; ++i)
            {
                uint32_t v = 0;
                std::memcpy(&v, p + size_t(i) * 4, 4);
                if (v != PatternE(i) && bad++ == 0) first = i;
            }
            gfx.UnmapBuffer(m_readE[s]);
            if (bad)
            {
                ++m_eMismatches;
                if (m_detailLogged++ < 6)
                    LOG_WARNING("[UPLOAD_STREAM_PROBE] frame %llu E (created with initial data): %u words differ, "
                                "first at %u", static_cast<unsigned long long>(sl.frame), bad, first);
            }
        }
    }

    void StreamUploadProbe::GameTick(IGraphicsDevice& gfx, uint64_t frame)
    {
        if (!m_initTried && !Init(gfx))
            LOG_ERROR("[UPLOAD_STREAM_PROBE] could not create its textures / readback buffers - not armed");
        if (!m_ready.load(std::memory_order_acquire)) return;
        m_lastFrame = frame;

        // AcquireUploadSlot(frame) has waited for this slot's previous frame to
        // retire: its readback is complete, and nothing writes it until this
        // frame's reader runs on the render half after the hand-off.
        const uint32_t s = static_cast<uint32_t>(frame % kSlots);
        Check(gfx, s);

        Slot& sl = m_slots[s];
        sl.recorded.store(false, std::memory_order_relaxed);
        sl.frame   = frame;
        sl.valid   = true;
        sl.bX      = CellX(frame);
        sl.bY      = CellY(frame);

        RHI::SubresourceData sa{};
        sa.dataPtr    = m_pixA.data();
        sa.rowPitch   = kSize * 4;
        sa.slicePitch = kSize * kSize * 4;
        RHI::SubresourceData sb{};
        sb.dataPtr    = m_pixB.data();
        sb.rowPitch   = kRect * 4;
        sb.slicePitch = kRect * kRect * 4;

        StreamUpload::Desc da{};
        da.planes = &sa;
        da.policy = StreamUpload::Policy::LatestWins;
        StreamUpload::Desc db{};
        db.planes = &sb;
        db.policy = StreamUpload::Policy::MustDeliver;
        db.rect   = { sl.bX, sl.bY, kRect, kRect };

        auto count = [this](StreamUpload::Result r)
        {
            switch (r)
            {
            case StreamUpload::Result::Queued:      ++m_queued; break;
            case StreamUpload::Result::Replaced:    ++m_replaced; break;
            case StreamUpload::Result::CarriedOver: ++m_carried; break;
            case StreamUpload::Result::Rejected:    ++m_rejected; break;
            case StreamUpload::Result::Fallback:    ++m_fallback; break;
            default:                                ++m_other; break;
            }
        };
        // A: a decoy, then the frame's data over it - the same key and size, so
        // it merges in place and the decoy must never be read.
        std::fill(m_pixA.begin(), m_pixA.end(), Decoy(frame));
        const StreamUpload::Result ra0 = gfx.QueueTextureUpload(m_texA, da);
        std::fill(m_pixA.begin(), m_pixA.end(), Encode(frame));
        const StreamUpload::Result ra = gfx.QueueTextureUpload(m_texA, da);
        std::fill(m_pixB.begin(), m_pixB.end(), Encode(frame));
        const StreamUpload::Result rb = gfx.QueueTextureUpload(m_texB, db);
        // Counted in the checked window only: the uploader comes alive on its first request,
        // so that frame's requests are carried over to the next - by design, and long before
        // kCheckFrom.
        if (frame >= kCheckFrom)
        {
            count(ra0);
            count(ra);
            count(rb);
        }
        // Only a request that landed in THIS frame says what this frame's
        // reader must see; a carried-over one is visible a frame later.
        sl.aQueued = ra == StreamUpload::Result::Queued || ra == StreamUpload::Result::Replaced;
        sl.bQueued = rb == StreamUpload::Result::Queued || rb == StreamUpload::Result::Fallback;

        if (!m_flushBaseTaken && frame >= kFlushFrom)
        {
            m_flushBase      = UploadStats::flushes.load(std::memory_order_relaxed);
            m_flushBaseTaken = true;
        }
        if (frame % 120 == 0) Report("periodic");
    }

    void StreamUploadProbe::EarlyTick(IGraphicsDevice& gfx, uint64_t frame)
    {
        // -streamupload_probe_noearly: no C (the fallback pack - a ring sized for
        // A alone, where a C carried in ahead of A would push it out).
        static const bool s_off = Sys::CommandLine::Get().Has("-streamupload_probe_noearly");
        if (s_off || !m_ready.load(std::memory_order_acquire) || frame == 0) return;

        // C's cell for this frame, encoding it. Before AcquireUploadSlot(frame)
        // the open frame is still frame - 1.
        const uint32_t x = CellX(frame), y = CellY(frame);
        std::fill(m_pixC.begin(), m_pixC.end(), EncodeC(frame));
        RHI::SubresourceData sc{};
        sc.dataPtr    = m_pixC.data();
        sc.rowPitch   = kRect * 4;
        sc.slicePitch = kRect * kRect * 4;
        StreamUpload::Desc dc{};
        dc.planes = &sc;
        dc.policy = StreamUpload::Policy::MustDeliver;
        dc.rect   = { x, y, kRect, kRect };
        const StreamUpload::Result r = gfx.QueueTextureUpload(m_texC, dc);

        uint64_t target = 0;
        bool     early  = false;
        switch (r)
        {
        case StreamUpload::Result::Queued:
        case StreamUpload::Result::Fallback:    target = frame - 1; early = true; break;
        case StreamUpload::Result::CarriedOver: target = frame; break;
        default:                                if (frame >= kCheckFrom) ++m_other; return;
        }
        if (target < kCheckFrom) return;   // the warm-up: no frame that early is checked
        ++(early ? m_cEarly : m_cCarried);
        for (Expected& e : m_expected)
            if (!e.valid)
            {
                e = { target, x, y, EncodeC(frame), true };
                return;
            }
        ++m_skipped;   // more awaiting than kExpected: never expected with checks every frame
    }

    void StreamUploadProbe::RecordReadback(IGraphicsDevice& gfx, RHI::CommandList cl)
    {
        if (!m_ready.load(std::memory_order_acquire)) return;
        const uint32_t s = gfx.GetFrameIndex() % kSlots;
        gfx.CopyTexture2DToBufferInPlace(m_texA, m_readA[s], cl);
        gfx.CopyTexture2DToBufferInPlace(m_texB, m_readB[s], cl);
        gfx.CopyTexture2DToBufferInPlace(m_texC, m_readC[s], cl);
        // D's barrier starts from its ledger state (SHADER_RESOURCE) while the resource is in
        // COMMON after its copy-queue upload - legal because that state can be promoted from
        // COMMON. E, a buffer, is promoted to COPY_SOURCE by the copy itself (CopyBuffer records
        // no barrier).
        gfx.CopyTexture2DToBufferInPlace(m_texD, m_readD[s], cl);
        gfx.CopyBuffer(m_bufE, m_readE[s], kBufBytes, cl);
        m_slots[s].recorded.store(true, std::memory_order_release);
    }

    void StreamUploadProbe::Report(const char* when) const
    {
        if (!m_ready.load(std::memory_order_acquire)) return;
        const uint64_t flushes = m_flushBaseTaken
            ? UploadStats::flushes.load(std::memory_order_relaxed) - m_flushBase : 0;
        LOG_INFO("[UPLOAD_STREAM_PROBE] checked=%llu mismatches=%llu late=%llu torn=%llu mustChecked=%llu "
                 "mustMismatches=%llu skipped=%llu flushes=%llu queued=%llu replaced=%llu carried=%llu "
                 "rejected=%llu other=%llu fallback=%llu cChecked=%llu cMismatches=%llu cEarly=%llu cCarried=%llu "
                 "dChecked=%llu dMismatches=%llu eMismatches=%llu window=%llu-%llu primary=%d loader=%d at=%s",
                 static_cast<unsigned long long>(m_checked), static_cast<unsigned long long>(m_mismatches),
                 static_cast<unsigned long long>(m_late), static_cast<unsigned long long>(m_torn),
                 static_cast<unsigned long long>(m_mustChecked),
                 static_cast<unsigned long long>(m_mustMismatches),
                 static_cast<unsigned long long>(m_skipped), static_cast<unsigned long long>(flushes),
                 static_cast<unsigned long long>(m_queued), static_cast<unsigned long long>(m_replaced),
                 static_cast<unsigned long long>(m_carried), static_cast<unsigned long long>(m_rejected),
                 static_cast<unsigned long long>(m_other), static_cast<unsigned long long>(m_fallback),
                 static_cast<unsigned long long>(m_cChecked), static_cast<unsigned long long>(m_cMismatches),
                 static_cast<unsigned long long>(m_cEarly), static_cast<unsigned long long>(m_cCarried),
                 static_cast<unsigned long long>(m_dChecked), static_cast<unsigned long long>(m_dMismatches),
                 static_cast<unsigned long long>(m_eMismatches),
                 static_cast<unsigned long long>(m_firstChecked), static_cast<unsigned long long>(m_lastChecked),
                 m_primary ? 1 : 0, m_loader ? 1 : 0, when);
    }

    StreamUploadProbeSystem::~StreamUploadProbeSystem()
    {
        if (StreamUploadProbe::Requested()) StreamUploadProbe::Get().Report("shutdown");
    }

    void StreamUploadProbeSystem::Update(World&, const FrameContext& ctx)
    {
        if (!StreamUploadProbe::Requested()) return;
        StreamUploadProbe::Get().GameTick(m_gfx, ctx.frameIndex);
    }

    void StreamUploadProbeEarlySystem::Update(World&, const FrameContext& ctx)
    {
        if (!StreamUploadProbe::Requested()) return;
        StreamUploadProbe::Get().EarlyTick(m_gfx, ctx.frameIndex);
    }
}

#endif  // WITH_HARNESS
