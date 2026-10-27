#pragma once

// RG::Dag - the dependency DAG of one Execute's ACTIVE passes, derived from
// their Setup() declarations in execution order, and the validators that read it.
// It is derived before Phase A on every frame, so a pass that is active only now
// and then - and reads what an inactive one did not write - is caught on the frame
// it happens. The DAG is a model: nothing is reordered; declaration order is
// execution order.
//
// EDGES, per texture, in execution order: a read depends on the last writer
// (RAW); a write on every reader since that writer (WAR) and on that writer
// (WAW). RenderTarget / Depth / UAV / Manual write; Read / History / Scoped /
// UAV / Manual read - UAV and Manual are both (a Manual pass declares no
// direction). History (ReadHistory) reads LAST frame's content: no RAW edge, only
// the WAR that keeps it before this frame's writer. Scoped (ReadScoped) is a read
// inside the pass's own list at a state it may also bind the texture at, so it
// never conflicts with that write. A Load write (PassAccess::load; every depth
// write, every WriteRenderTarget) keeps what was there: a WAW chain (GBuffer ->
// Terrain -> Grass on depth) is a chain of uses, not of overwrites.
//
// THE CHECKS (GraphSnapshot::Check; each (check, pass, texture) is logged once
// per Compile as `[RG_DAG] finding`, an ERROR under -rgdagverify for StaleRead,
// ConflictInPass, ClearAfterLive and StencilOverlap):
//   StaleRead        a ReadSRV of a texture no active pass wrote earlier this
//                    frame, whose producer is declared LATER or nowhere: the
//                    reader gets the previous frame's content (reading it on
//                    purpose is ReadHistory). A Fresh import (ImportSpec.h; written
//                    outside before the graph, e.g. the CSM) read with no writer
//                    before is no finding.
//   InactiveProducer the same, but the producer is declared EARLIER and did not
//                    run this frame: a conditional read (LightingPass's of the
//                    ReSTIR / path-tracer results when those paths are off) -
//                    information, not a bug by itself.
//   ConflictInPass   one pass declares the same texture as a read AND a write
//                    (an SRV beside an RTV, a DSV or a UAV): contradictory
//                    states. ManualBarrier is how a pass binds one texture two
//                    ways and transitions it itself.
//   OpaquePass       a pass that ran and declared nothing (coverage).
//   UavNeedsBarrier  the pass before this one used the texture as a UAV too, so
//                    no transition separates their accesses, and EndFrame batches
//                    same-queue lists into one ExecuteCommandLists, inside which
//                    nothing orders them. The graph emits a UAV barrier for
//                    exactly these (not under -rguavbarrieroff).
//   ClearAfterLive   a Load::Clear render target on a texture an active pass
//                    (`other`) already wrote this frame: the clear wipes that
//                    content (e.g. an all-black picture). Any stage.
//   StencilOverlap   a pass tested a stencil bit (through the DSV or a
//                    stencil-plane SRV) that an active pass (`other`) writes
//                    after it this frame, so the order decides what it saw.
//                    Moving a writer of LightingPass's bits behind it, or
//                    widening its mask over TaaExclude / OutlineRim, triggers it
//                    (StencilBits.h, -rgtest_stencilwide=<pass>).
// PLANES: a depth-stencil texture is its depth plane plus each of its stencil
// plane's eight bits as a resource of its own (kDagSlots); a stencil access
// touches the bits of its mask (PassAccess::stencilMask), a reader depends only
// on the writers of its bits, and a whole-texture access (plane -1) touches every
// one. An edge between two passes on one texture is one edge, whichever planes
// carry it (Dependency::planes). DsvRead (a depth / stencil test) and CopySrc are
// reads, CopyDst a write. Which pass may write or test which bits is the stencil
// registry's check at Compile (StencilOwner, StencilBits.h).
// TODO(RenderGraph_AutoTopology_NodeView_Plan M7): UnusedWrite (a write nobody
// reads before the next write or the frame's end) - needs exit states / exports.

#include "RenderGraph/GraphSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RG
{
    // The resources one texture is to the derivation: a depth-stencil texture's
    // depth plane (slot 0) and its stencil plane's bits (slots 1-8); any other
    // texture uses slot 0 alone.
    inline constexpr uint32_t kDagSlots = 9;

    struct Dag
    {
        std::vector<GraphSnapshot::Dependency> edges;
        std::vector<GraphSnapshot::Finding>    findings;
        uint32_t raw = 0, war = 0, waw = 0;

        void Clear()
        {
            edges.clear();
            findings.clear();
            raw = war = waw = 0;
        }
        uint32_t Count(GraphSnapshot::Check c) const
        {
            uint32_t n = 0;
            for (const GraphSnapshot::Finding& f : findings) n += f.check == c ? 1u : 0u;
            return n;
        }
    };

    // Per-texture state the derivation walks with; kept by the caller and
    // reused frame to frame, so a steady frame allocates nothing.
    struct DagScratch
    {
        struct Res
        {
            uint32_t res = 0, tex = 0;
            uint8_t  bit = 1;   // Dependency::planes' bit: 1 the depth plane (or the whole texture), 2 the stencil plane
            GraphSnapshot::Access kind = GraphSnapshot::Access::Read;
            uint8_t  load = 0;
        };

        std::vector<int32_t>               writer;    // the last pass that wrote it this frame, -1 none
        std::vector<std::vector<uint32_t>> readers;   // the passes that read it since that write
        std::vector<int32_t>               uavPass;   // the last access was a UAV by this pass, else -1
        std::vector<int32_t>               firstWriter;   // the first pass DECLARING a write, active or not
        std::vector<std::vector<uint32_t>> testers;   // the passes that tested a stencil bit since its write

        // One pass's accesses on resources - a texture's depth plane or one bit of
        // its stencil plane (res = kDagSlots x texture + slot) - rebuilt per pass.
        std::vector<Res>                   acc;
    };

    // Derive the DAG of the passes that ran. @p accesses[i] is pass i's
    // declarations in Phase A's transition order (render targets, depth, UAVs,
    // reads, manual handles); @p ran[i] != 0 means it recorded this frame. A
    // texture index at or past @p textureCount is skipped, never trusted.
    // @p freshAtEntry (optional): per texture, nonzero = an import written outside
    // before the graph (ImportEntry::Fresh). @p planar (optional): per texture,
    // nonzero = a depth-stencil format, two planes.
    void DeriveDag(const std::vector<std::vector<GraphSnapshot::PassAccess>>& accesses,
                   const std::vector<uint8_t>& ran, size_t textureCount,
                   Dag& out, DagScratch& scratch,
                   const std::vector<uint8_t>* freshAtEntry = nullptr,
                   const std::vector<uint8_t>* planar = nullptr);
}
