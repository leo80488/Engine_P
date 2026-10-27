#include "RenderGraph/RenderGraphDag.h"

#include <algorithm>

namespace RG
{
namespace
{
    using Snap = GraphSnapshot;

    // A shader-resource read: one beside a write of the same texture asks for
    // two states (ConflictInPass).
    bool SrvRead(Snap::Access a) { return a == Snap::Access::Read || a == Snap::Access::History; }
    // A read that writes nothing. Scoped is one: the pass reads the texture inside
    // its own list at a state it may also bind it at (the HDR copy before the
    // Lighting draw: both RENDER_TARGET), so it is a read for the edges and no
    // conflict. So are a depth / stencil test through the DSV and a copy's source.
    bool PureRead(Snap::Access a)
    {
        return SrvRead(a) || a == Snap::Access::Scoped || a == Snap::Access::DsvRead || a == Snap::Access::CopySrc;
    }
    bool Reads(Snap::Access a)
    {
        return PureRead(a) || a == Snap::Access::UAV || a == Snap::Access::Manual;
    }
    bool Writes(Snap::Access a) { return !PureRead(a); }
    bool WritesForConflict(Snap::Access a)
    {
        return a == Snap::Access::RenderTarget || a == Snap::Access::Depth || a == Snap::Access::UAV ||
               a == Snap::Access::CopyDst;
    }
    // A read that depends on a stencil bit's value - the test through the DSV, or
    // a shader read of the stencil plane. A copy of the plane is a snapshot, taken
    // at whatever point the pass chose.
    bool Tests(Snap::Access a) { return a == Snap::Access::DsvRead || SrvRead(a); }
}

void DeriveDag(const std::vector<std::vector<GraphSnapshot::PassAccess>>& accesses,
               const std::vector<uint8_t>& ran, size_t textureCount,
               Dag& out, DagScratch& s, const std::vector<uint8_t>* freshAtEntry,
               const std::vector<uint8_t>* planar)
{
    auto fresh = [freshAtEntry](uint32_t tex)
    {
        return freshAtEntry && tex < freshAtEntry->size() && (*freshAtEntry)[tex] != 0;
    };
    // A depth-stencil texture is its depth plane (slot 0) and each bit of its
    // stencil plane (slots 1-8); any other texture is slot 0 alone.
    auto twoPlanes = [planar](uint32_t tex)
    {
        return planar && tex < planar->size() && (*planar)[tex] != 0;
    };
    const size_t resources = textureCount * kDagSlots;
    out.Clear();
    s.writer.assign(resources, -1);
    if (s.readers.size() < resources) s.readers.resize(resources);
    if (s.testers.size() < resources) s.testers.resize(resources);
    for (size_t r = 0; r < resources; ++r)
    {
        s.readers[r].clear();
        s.testers[r].clear();
    }
    s.uavPass.assign(resources, -1);

    // One edge per (from, to, texture, kind), whichever planes carry it.
    auto edge = [&](uint32_t from, uint32_t to, uint32_t tex, Snap::Edge kind, uint8_t planeBit)
    {
        if (from == to) return;
        for (Snap::Dependency& e : out.edges)   // a few hundred at most
            if (e.from == from && e.to == to && e.texture == tex && e.kind == kind)
            {
                e.planes = static_cast<uint8_t>(e.planes | planeBit);
                return;
            }
        Snap::Dependency d{ from, to, tex, kind };
        d.planes = planeBit;
        out.edges.push_back(d);
        switch (kind)
        {
        case Snap::Edge::RAW: ++out.raw; break;
        case Snap::Edge::WAR: ++out.war; break;
        case Snap::Edge::WAW: ++out.waw; break;
        }
    };
    auto find = [&](Snap::Check check, uint32_t pass, uint32_t tex, uint32_t other)
    {
        for (const Snap::Finding& f : out.findings)
            if (f.check == check && f.pass == pass && f.texture == tex) return;
        out.findings.push_back({ check, pass, tex, other });
    };
    // One pass's accesses, per resource: the whole of a two-plane texture is its
    // depth plane and every stencil bit, a stencil-plane access the bits of its mask.
    auto expand = [&](const std::vector<Snap::PassAccess>& in)
    {
        s.acc.clear();
        for (const Snap::PassAccess& a : in)
        {
            if (a.texture >= textureCount) continue;
            const uint32_t base = a.texture * kDagSlots;
            if (!twoPlanes(a.texture) || a.plane == 0)
            {
                s.acc.push_back({ base, a.texture, 1, a.kind, a.load });
                continue;
            }
            if (a.plane < 0)
                s.acc.push_back({ base, a.texture, 1, a.kind, a.load });
            const uint32_t bits = a.plane < 0 ? 0xFFu : a.stencilMask;
            for (uint32_t b = 0; b < 8; ++b)
                if (bits & (1u << b))
                    s.acc.push_back({ base + 1 + b, a.texture, 2, a.kind, a.load });
        }
    };

    const size_t passes = std::min(accesses.size(), ran.size());

    // The first pass that DECLARES a write of each resource, active or not:
    // what tells a conditional read (its producer is declared earlier but did
    // not run) from a read declared before its producer, or of a texture no
    // pass declares a write of at all.
    s.firstWriter.assign(resources, -1);
    for (uint32_t i = 0; i < static_cast<uint32_t>(passes); ++i)
    {
        expand(accesses[i]);
        for (const DagScratch::Res& a : s.acc)
            if (Writes(a.kind) && s.firstWriter[a.res] < 0)
                s.firstWriter[a.res] = static_cast<int32_t>(i);
    }

    for (uint32_t i = 0; i < static_cast<uint32_t>(passes); ++i)
    {
        if (!ran[i]) continue;
        if (accesses[i].empty())
        {
            find(Snap::Check::OpaquePass, i, ~0u, ~0u);
            continue;
        }
        expand(accesses[i]);
        const std::vector<DagScratch::Res>& acc = s.acc;

        // A read and a write of one resource in one pass ask for two states
        // (one plane's write beside the other plane's read does not).
        for (const DagScratch::Res& a : acc)
        {
            if (!SrvRead(a.kind)) continue;
            for (const DagScratch::Res& b : acc)
                if (b.res == a.res && WritesForConflict(b.kind))
                    find(Snap::Check::ConflictInPass, i, a.tex, ~0u);
        }

        // RAW edges. A ReadSRV with no writer before it this frame: its
        // producer is declared earlier and did not run (a conditional read),
        // or it is declared later / nowhere - the reader gets last frame's
        // content (StaleRead). A ReadHistory means exactly that: no RAW edge.
        // A Fresh import's writer is outside, before the graph.
        for (const DagScratch::Res& a : acc)
        {
            if (!Reads(a.kind) || a.kind == Snap::Access::History) continue;
            const int32_t w = s.writer[a.res];
            if (w >= 0)
                edge(static_cast<uint32_t>(w), i, a.tex, Snap::Edge::RAW, a.bit);
            else if (a.kind == Snap::Access::Read && !fresh(a.tex))
            {
                const int32_t first = s.firstWriter[a.res];
                const bool earlierProducer = first >= 0 && first < static_cast<int32_t>(i);
                find(earlierProducer ? Snap::Check::InactiveProducer : Snap::Check::StaleRead,
                     i, a.tex, earlierProducer ? static_cast<uint32_t>(first) : ~0u);
            }
        }
        // ClearAfterLive: a Load::Clear render target on a texture an active pass
        // already wrote this frame wipes that content (e.g. a misplaced HdrSceneColor
        // clear gives an all-black picture).
        for (const DagScratch::Res& a : acc)
        {
            if (a.kind != Snap::Access::RenderTarget || a.load != 1) continue;
            const int32_t w = s.writer[a.res];
            if (w >= 0 && w != static_cast<int32_t>(i))
                find(Snap::Check::ClearAfterLive, i, a.tex, static_cast<uint32_t>(w));
        }
        // WAR / WAW, before this pass becomes the writer. StencilOverlap: a pass
        // tested this stencil bit earlier this frame - what it tested is the bit before
        // this write, so moving either pass changes it.
        for (const DagScratch::Res& a : acc)
        {
            if (!Writes(a.kind)) continue;
            for (uint32_t r : s.readers[a.res]) edge(r, i, a.tex, Snap::Edge::WAR, a.bit);
            for (uint32_t t : s.testers[a.res])
                if (t != i) find(Snap::Check::StencilOverlap, t, a.tex, i);
            const int32_t w = s.writer[a.res];
            if (w >= 0) edge(static_cast<uint32_t>(w), i, a.tex, Snap::Edge::WAW, a.bit);
        }
        for (const DagScratch::Res& a : acc)
        {
            if (!Writes(a.kind)) continue;
            s.writer[a.res] = static_cast<int32_t>(i);
            s.readers[a.res].clear();
            s.testers[a.res].clear();
        }
        for (const DagScratch::Res& a : acc)
        {
            if (Writes(a.kind)) continue;   // pure reads only
            if (s.writer[a.res] == static_cast<int32_t>(i)) continue;
            std::vector<uint32_t>& rs = s.readers[a.res];
            if (std::find(rs.begin(), rs.end(), i) == rs.end()) rs.push_back(i);
            if (a.bit == 2 && Tests(a.kind))   // a stencil bit's value
            {
                std::vector<uint32_t>& ts = s.testers[a.res];
                if (std::find(ts.begin(), ts.end(), i) == ts.end()) ts.push_back(i);
            }
        }

        // UAV after UAV by another pass, nothing in between. Walked in Phase A's
        // order, the same rule EmitBarriersBeforePass applies.
        for (const DagScratch::Res& a : acc)
        {
            int32_t& last = s.uavPass[a.res];
            if (a.kind == Snap::Access::UAV)
            {
                if (last >= 0 && last != static_cast<int32_t>(i))
                    find(Snap::Check::UavNeedsBarrier, i, a.tex, static_cast<uint32_t>(last));
                last = static_cast<int32_t>(i);
            }
            else
                last = -1;
        }
    }
}
}
