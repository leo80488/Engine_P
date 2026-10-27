#include "RenderGraph/FrameCapture.h"

#include <algorithm>
#include <unordered_map>

namespace RG
{
namespace
{
    using Snap   = GraphSnapshot;
    using Device = GfxDiag::FrameSnapshot;

    // D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY: the first half of a split
    // barrier, which changes nothing yet (the END_ONLY half does).
    constexpr uint8_t  kBeginOnly   = 0x1;
    // D3D12_RESOURCE_STATE_COMMON: what decay leaves between ExecuteCommandLists.
    constexpr uint32_t kStateCommon = 0;

    // Transitive closure over a DAG given as "node -> nodes it points at",
    // nodes visited so that every successor is done first. Bit j of row i set =
    // j is reachable from i. Rows are 64-bit words.
    struct Reach
    {
        size_t words = 0;
        std::vector<uint64_t> bits;

        void Init(size_t n) { words = (n + 63) / 64; bits.assign(n * words, 0ull); }
        uint64_t*       Row(size_t i)       { return bits.data() + i * words; }
        const uint64_t* Row(size_t i) const { return bits.data() + i * words; }
        bool Has(size_t from, size_t to) const
        {
            return words && (Row(from)[to / 64] >> (to % 64)) & 1ull;
        }
        void Add(size_t from, size_t to)
        {
            Row(from)[to / 64] |= 1ull << (to % 64);
            const uint64_t* src = Row(to);
            uint64_t*       dst = Row(from);
            for (size_t w = 0; w < words; ++w) dst[w] |= src[w];
        }
    };

    // A texture a pass declared, by the name the journal knows it by, and the
    // planes it writes: bit 0 the depth plane (the whole of any other texture),
    // bit 1 the stencil plane. The ReadRead class needs the split.
    struct Declared
    {
        const std::string* name   = nullptr;
        uint8_t            writes = 0;
        bool               planar = false;
    };

    // The state a resource (or one subresource) was last left in, and by whom.
    struct Last
    {
        bool     valid = false;
        uint32_t state = 0;
        uint32_t list  = 0;
    };
    struct Track
    {
        Last                                   all;
        std::unordered_map<uint32_t, Last>     subs;   // per-subresource, newer than `all`
    };

    // A read that writes nothing (the same rule RenderGraphDag uses).
    bool PureRead(Snap::Access a)
    {
        return a == Snap::Access::Read || a == Snap::Access::History || a == Snap::Access::Scoped ||
               a == Snap::Access::DsvRead || a == Snap::Access::CopySrc;
    }
}

std::shared_ptr<FrameCapture> BuildFrameCapture(std::shared_ptr<const GraphSnapshot> graph,
                                                std::shared_ptr<const GfxDiag::FrameSnapshot> device,
                                                const std::string& ignorePass)
{
    auto cap = std::make_shared<FrameCapture>();
    FrameCapture& c = *cap;
    c.graph       = std::move(graph);
    c.device      = std::move(device);
    c.ignoredPass = ignorePass;
    if (!c.device) return cap;
    const Device& d = *c.device;
    c.frame = d.frame;

    const size_t lists = d.commandLists.size();
    c.passOfList.assign(lists, -1);

    // ---- graph side: list -> pass, declared reachability ----
    Reach declared;
    std::vector<std::vector<Declared>> declaredNames;   // per pass: the textures it declared
    if (c.graph)
    {
        const Snap& g = *c.graph;
        const size_t passes = g.passes.size();
        declaredNames.resize(passes);
        int32_t ignored = -1;
        for (size_t i = 0; i < passes; ++i)
        {
            const Snap::Pass& p = g.passes[i];
            if (!ignorePass.empty() && p.name == ignorePass) ignored = static_cast<int32_t>(i);
            if (!p.Ran()) continue;
            if (p.cl < lists)        c.passOfList[p.cl]        = static_cast<int32_t>(i);
            if (p.auxTailCl < lists) c.passOfList[p.auxTailCl] = static_cast<int32_t>(i);
            if (p.accesses.empty()) ++c.coverage.opaquePasses;
            for (const Snap::PassAccess& a : p.accesses)
            {
                if (a.kind == Snap::Access::Manual) ++c.coverage.manualBarriers;
                // An import (e.g. the HDR scene colour) is known to the journal by its
                // device name, not by the graph's.
                if (a.texture < g.textures.size() && static_cast<int32_t>(i) != ignored)
                {
                    const Snap::Texture& t = g.textures[a.texture];
                    const std::string* name = t.deviceName.empty() ? &t.name : &t.deviceName;
                    const uint8_t planes = a.plane == 0 ? 1 : a.plane == 1 ? 2 : 3;
                    const uint8_t writes = PureRead(a.kind) ? 0 : planes;
                    auto it = std::find_if(declaredNames[i].begin(), declaredNames[i].end(),
                                           [name](const Declared& x) { return *x.name == *name; });
                    if (it == declaredNames[i].end()) declaredNames[i].push_back({ name, writes, t.planar });
                    else                              it->writes = static_cast<uint8_t>(it->writes | writes);
                }
            }
        }
        c.coverage.declaredEdges = static_cast<uint32_t>(g.edges.size());

        // Declared edges always point forward in execution order (from < to),
        // so closing them from the last pass back is one pass over the list.
        std::vector<std::vector<uint32_t>> out(passes);
        for (const Snap::Dependency& e : g.edges)
        {
            if (e.from >= passes || e.to >= passes) continue;
            if (static_cast<int32_t>(e.from) == ignored || static_cast<int32_t>(e.to) == ignored) continue;
            out[e.from].push_back(e.to);
        }
        declared.Init(passes);
        for (size_t i = passes; i-- > 0;)
        {
            std::sort(out[i].begin(), out[i].end());   // a successor is always > i: already closed
            for (uint32_t j : out[i]) if (j > i) declared.Add(i, j);
        }
    }

    // ---- device side: which lists the consumer waits on (declared) ----
    // ancestors(B) = every list B's waitFor reaches. Submission order is a
    // topological order of waitFor (EndFrame's Kahn sort), so walking it
    // closes each list after all of its dependencies.
    Reach waits;
    waits.Init(lists);
    for (uint32_t slot : d.submitOrder)
    {
        if (slot >= lists) continue;
        for (uint32_t dep : d.commandLists[slot].waitFor)
            if (dep < lists && dep != slot) waits.Add(slot, dep);
    }

    // ---- walk the journal in the order the GPU meets it ----
    std::unordered_map<uint32_t, Track> tracks;   // by resource index
    struct Key { uint32_t from, to, resource; };
    std::vector<Key> seen;
    // @p sub: the subresource handed over (Device::kNone = every one of them).
    auto handover = [&](uint32_t from, uint32_t to, uint32_t resource, uint32_t state, uint32_t barrier,
                        uint32_t sub)
    {
        if (from == to || from >= lists || to >= lists) return;
        for (const Key& k : seen)   // a few hundred at most, built rarely
            if (k.from == from && k.to == to && k.resource == resource) return;
        seen.push_back({ from, to, resource });

        FrameCapture::ObservedEdge e;
        e.from     = from;
        e.to       = to;
        e.fromPass = c.passOfList[from];
        e.toPass   = c.passOfList[to];
        e.resource = resource;
        e.state    = state;
        e.barrier  = barrier;
        if (e.fromPass >= 0 && e.toPass >= 0)
        {
            if (e.fromPass == e.toPass)
                e.kind = FrameCapture::EdgeClass::IntraPass;
            else if (declared.Has(static_cast<size_t>(e.fromPass), static_cast<size_t>(e.toPass)))
                e.kind = FrameCapture::EdgeClass::Covered;
            if (resource < d.resources.size() && !d.resources[resource].name.empty())
            {
                const std::string& name = d.resources[resource].name;
                auto declared = [&](int32_t pass) -> const Declared*
                {
                    if (static_cast<size_t>(pass) >= declaredNames.size()) return nullptr;
                    for (const Declared& x : declaredNames[static_cast<size_t>(pass)])
                        if (*x.name == name) return &x;
                    return nullptr;
                };
                const Declared* a = declared(e.fromPass);
                const Declared* b = declared(e.toPass);
                e.resourceDeclared = a && b;
                // READ-READ: neither pass writes what was handed over - the plane
                // the transition moved (subresource 0 / 1 of a depth-stencil
                // texture), or the whole resource.
                if (e.kind == FrameCapture::EdgeClass::Hidden && a && b)
                {
                    const uint8_t planes = a->planar && sub < 2 ? static_cast<uint8_t>(1u << sub) : uint8_t(3);
                    if (!(a->writes & planes) && !(b->writes & planes))
                        e.kind = FrameCapture::EdgeClass::ReadRead;
                }
            }
        }
        e.ordered    = waits.Has(to, from);
        e.crossQueue = d.commandLists[from].queue != d.commandLists[to].queue;
        c.observed.push_back(e);
    };
    // @p strict: a disagreeing "before" is a mismatch. Not for the whole-
    // resource state once subresources moved on their own - how many of them
    // it still describes is not known here.
    // @p sub: the subresource @p last describes (Device::kNone = the whole resource).
    auto check = [&](const Last& last, const Device::Barrier& b, uint32_t index, bool strict, uint32_t sub)
    {
        if (!last.valid) return;   // first touch this frame: left by an earlier frame
        if (last.state != b.before)
        {
            if (strict && b.before != kStateCommon)
                c.mismatches.push_back({ index, last.state, last.list });
            return;
        }
        handover(last.list, b.cl, b.resource, b.before, index, sub);
    };

    for (uint32_t i = 0; i < static_cast<uint32_t>(d.journal.size()); ++i)
    {
        const Device::Barrier& b = d.journal[i];
        if (b.kind != Device::Kind::Transition || b.resource == Device::kNone) continue;
        if (b.flags & kBeginOnly) continue;
        Track& t = tracks[b.resource];
        if (b.subresource == Device::kNone)
        {
            // Every subresource must be in `before`; a newer per-subresource
            // state is each its own handover.
            if (t.subs.empty())
                check(t.all, b, i, true, Device::kNone);
            else
            {
                for (const auto& [sub, last] : t.subs) check(last, b, i, true, sub);
                check(t.all, b, i, false, Device::kNone);
                t.subs.clear();
            }
            t.all = { true, b.after, b.cl };
        }
        else
        {
            const auto it = t.subs.find(b.subresource);
            if (it != t.subs.end()) check(it->second, b, i, true, b.subresource);
            else                    check(t.all, b, i, true, b.subresource);
            t.subs[b.subresource] = { true, b.after, b.cl };
        }
    }

    // ---- counts ----
    FrameCapture::Coverage& n = c.coverage;
    n.commandLists   = static_cast<uint32_t>(lists);
    n.batches        = static_cast<uint32_t>(d.batches.size());
    n.barriers       = static_cast<uint32_t>(d.journal.size());
    n.nativeBarriers = d.NativeCount();
    n.resources      = static_cast<uint32_t>(d.resources.size());
    n.observedEdges  = static_cast<uint32_t>(c.observed.size());
    n.stateMismatches = static_cast<uint32_t>(c.mismatches.size());
    for (const FrameCapture::ObservedEdge& e : c.observed)
    {
        switch (e.kind)
        {
        case FrameCapture::EdgeClass::Covered:   ++n.coveredEdges;   break;
        case FrameCapture::EdgeClass::IntraPass: ++n.intraPassEdges; break;
        case FrameCapture::EdgeClass::Hidden:    ++n.hiddenEdges;    break;
        case FrameCapture::EdgeClass::ReadRead:  ++n.readReadEdges;  break;
        }
        n.crossQueueEdges += e.crossQueue ? 1u : 0u;
        n.unorderedEdges  += e.ordered ? 0u : 1u;
        n.undeclaredResourceEdges += e.resourceDeclared ? 0u : 1u;
    }
    return cap;
}

FrameCaptureBoard& FrameCaptures()
{
    static FrameCaptureBoard s_board;
    return s_board;
}
}
