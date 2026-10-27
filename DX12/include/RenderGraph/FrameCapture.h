#pragma once

// RG::FrameCapture - the main graph's snapshot of one frame joined with the device's
// snapshot of the SAME frame, and the edges the barrier journal implies.
//
// WHY. RG::GraphSnapshot knows what the graph DECLARED; the device's
// GfxDiag::FrameSnapshot (Graphics/FrameSnapshot.h) knows what the frame DID - every
// command list, their submission order, every barrier anyone recorded. Joined, they
// answer: which handoffs between command lists does the graph not know about?
//
// OBSERVED EDGES. The journal is in the order the GPU meets it. A resource
// transitioned INTO a state by list A and later OUT of it by list B says B picked up
// what A left: an observed edge A -> B. Only handoffs that change a state are visible
// (a reader that finds the resource already in its state leaves no trace), so the
// observed edges are a lower bound on the frame's real dependencies.
//
// CLASSES. COVERED: both lists belong to main-graph passes and the graph's declared
// edges (GraphSnapshot::edges) lead from the producer pass to the consumer pass.
// INTRA-PASS: both lists belong to the same pass (its own list and its aux tail).
// READ-READ: both passes declared the resource and neither writes what was handed over
// (the moved plane of a depth-stencil texture, or the whole of any other) - two
// readers in different read states (a copy's source, then a depth test); the graph
// derives the transition between them in any order, so there is no dependency to
// keep ([RG_COVERAGE] readReadEdges=). HIDDEN otherwise: a dependency the graph would
// not keep if it reordered; [RG_COVERAGE] hiddenEdges= is the coverage metric.
// TODO(RenderGraph_AutoTopology_NodeView_Plan M7): move the out-of-graph work (SSR,
// Hi-Z, the pre-compute / compute / restore lists, UI, buffers) into the graph to
// bring hiddenEdges to 0.
// COVERED is about ORDER: the declarations already keep the producer first, whichever
// resource carried them there. RESOURCE DECLARED is stricter: both passes declared the
// very resource handed over (undeclaredResourceEdges counts the rest). ORDERED: the
// device itself orders the pair - the consumer waits on the producer through declared
// AddCommandListDependency edges; an unordered observed edge holds only because
// EndFrame's Kahn sort happened to put the producer first, or, across queues, not at
// all.
// STATE MISMATCHES: transitions whose "before" is not the state the resource was last
// left in, in submission order - the debug layer's "Before state ... does not match",
// found without the debug layer. A "before" of COMMON never counts (buffers and
// simultaneous-access textures decay to COMMON between ExecuteCommandLists calls).
//
// ON REQUEST ONLY. Readers ask RG::FrameCaptures(); the render half arms the device's
// journal for the next frame (IGraphicsDevice::ArmFrameJournal), the main graph
// snapshots that frame, and after EndFrame the Renderer joins the two here and
// publishes (Renderer::PublishFrameCapture).

#include "RenderGraph/GraphSnapshot.h"
#include "Graphics/FrameSnapshot.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace RG
{
    struct FrameCapture
    {
        enum class EdgeClass : uint8_t { Covered, IntraPass, Hidden, ReadRead };

        struct ObservedEdge
        {
            uint32_t  from = 0, to = 0;             // pool slots of the producer / consumer lists
            int32_t   fromPass = -1, toPass = -1;   // main-graph pass indices, -1 = not a pass's list
            uint32_t  resource = 0;                 // index into device->resources
            uint32_t  state    = 0;                 // the D3D12 state handed over
            uint32_t  barrier  = 0;                 // journal index of the consumer's transition
            EdgeClass kind       = EdgeClass::Hidden;
            bool      resourceDeclared = false;     // both passes declared this resource
            bool      ordered    = false;           // the consumer waits on the producer (declared)
            bool      crossQueue = false;
        };
        struct Mismatch
        {
            uint32_t barrier  = 0;   // journal index of the transition
            uint32_t expected = 0;   // the state the resource was last left in
            uint32_t lastList = 0;   // the list that left it there
        };
        // The numbers [RG_COVERAGE] prints.
        struct Coverage
        {
            uint32_t commandLists = 0, batches = 0, barriers = 0, nativeBarriers = 0, resources = 0;
            uint32_t observedEdges = 0, declaredEdges = 0, coveredEdges = 0, hiddenEdges = 0;
            uint32_t readReadEdges = 0;
            uint32_t intraPassEdges = 0, crossQueueEdges = 0, unorderedEdges = 0, stateMismatches = 0;
            uint32_t undeclaredResourceEdges = 0;   // observed edges whose resource a pass did not declare
            uint32_t opaquePasses = 0, manualBarriers = 0;
        };

        uint64_t frame    = 0;   // the device's render frame
        uint64_t sequence = 0;   // the board's publish counter
        std::shared_ptr<const GraphSnapshot>          graph;    // null: the main graph did not execute
        std::shared_ptr<const GfxDiag::FrameSnapshot> device;   // never null once published
        std::vector<int32_t>      passOfList;   // pool slot -> main-graph pass index, -1 = none
        std::vector<ObservedEdge> observed;
        std::vector<Mismatch>     mismatches;
        Coverage                  coverage;
        std::string               ignoredPass;  // -rgtest_undeclare: its declared edges were left out

        static const char* ClassName(EdgeClass c)   // the names the dump and the panel print
        {
            switch (c)
            {
            case EdgeClass::Covered:   return "covered";
            case EdgeClass::IntraPass: return "intraPass";
            case EdgeClass::Hidden:    return "hidden";
            case EdgeClass::ReadRead:  return "readRead";
            }
            return "?";
        }
    };

    // Join and derive. Pure and serial; a bad index is skipped, never trusted.
    // @p ignorePass (the -rgtest_undeclare reverse arm) leaves every declared
    // edge touching that pass out of the reachability, as if it had declared
    // nothing - its handoffs then read as hidden.
    std::shared_ptr<FrameCapture> BuildFrameCapture(std::shared_ptr<const GraphSnapshot> graph,
                                                    std::shared_ptr<const GfxDiag::FrameSnapshot> device,
                                                    const std::string& ignorePass = {});

    using FrameCaptureBoard = Board<FrameCapture>;

    // The board readers ask (-rgdump, the `rg` verb); the Renderer publishes.
    FrameCaptureBoard& FrameCaptures();
}
