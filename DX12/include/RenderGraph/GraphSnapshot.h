#pragma once

// RG::GraphSnapshot - one EXECUTED frame of a RenderGraph as plain data.
//
// DescribePasses() (the -dump_catalog view) lists DECLARATIONS; this records what
// one Execute DID - each pass's activity-gate answer, the transitions Phase A
// emitted, the command list it recorded on, its record time - and derives the
// dependency edges between the passes that ran. FrameCapture joins it with the
// device's frame, RenderGraphDag validates its edges, the editor's Render Graph
// panel draws it.
//
// On request only: a graph builds one only when its SnapshotBoard has an
// unanswered Request(); otherwise the cost is a pointer test and one relaxed
// atomic load per frame, and one null test per barrier.
//
// Consumers: -rgdump=<file>[@<frame>] writes one as JSON and the control channel's
// `rg` verb answers from one, both through one builder (Harness/RenderGraphDump.h)
// so the file and the reply cannot disagree.
//
// Indices: passes[i].index is the declaration index, which IS the execution order
// (the sorted order; `registered` is the AddPass order; inactive passes are
// skipped). Texture references index `textures`, the graph's own texture ids for
// this compile. States are RHI::ResourceState values, formats RHI::Format values;
// the JSON names them.

#include "RenderGraph/Order.h"   // OrderRule
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace RG
{
    struct GraphSnapshot
    {
        // How a pass declared a texture in Setup().
        enum class Access : uint8_t
        {
            Read,           // ReadSRV
            RenderTarget,   // WriteRenderTarget
            Depth,          // WriteDepthStencil
            UAV,            // WriteUAV (read-write)
            Manual,         // ManualBarrier: self-barriered, direction unknown
            History,        // ReadHistory: last frame's content - a read with no RAW edge
            Scoped,         // ReadScoped: read inside the pass's own list, put back at its state
            // From RenderGraphBuilder::DepthStencil / CopySrc / CopyDst:
            DsvRead,        // a depth or stencil test through the DSV - a read
            CopySrc,        // a copy's source - a read
            CopyDst,        // a copy's destination - a write
        };
        // Why a pass did not record this frame.
        enum class Skip : uint8_t
        {
            None,           // it recorded
            Inactive,       // its own IsActive() said no
            Hidden,         // the view does not show what it draws (RenderPass::Shown)
            View,           // a secondary view, and the pass is not view-safe (PassLifecycle::kViewSafe)
        };
        // Dependency between two passes that RAN, derived from declarations
        // in execution order. Read-after-write is the producer -> consumer
        // edge; write-after-read and write-after-write are the ordering edges
        // a reorder would have to keep. UAV and Manual count as read AND
        // write (a Manual pass's direction is not declared, so it is assumed
        // both ways - the conservative reading).
        enum class Edge : uint8_t { RAW, WAR, WAW };

        struct PassAccess
        {
            uint32_t texture    = 0;
            Access   kind       = Access::Read;
            uint32_t entryState = 0;   // Manual: the declared entry state (0 = none); Scoped: its state
            uint8_t  load       = 0;   // RenderTarget only: RG::Load (0 Load, 1 Clear, 2 DontCare)
            // The plane of a depth-stencil texture (0 depth, 1 stencil); -1 the whole
            // texture. stencilMask: the stencil bits a stencil-plane access reads or writes.
            int8_t   plane       = -1;
            uint8_t  stencilMask = 0xFF;
        };
        struct Barrier
        {
            uint32_t texture = 0;
            uint32_t before  = 0;
            uint32_t after   = 0;
            int8_t   plane   = -1;   // one plane of a depth-stencil texture; -1 every plane
        };
        struct Pass
        {
            std::string             name;
            uint32_t                index       = 0;
            Skip                    skip        = Skip::None;
            const char*             colorTarget = "None";   // the builtin target (static string)
            const char*             stage       = "Unstaged";   // RG::StageName of its AddPass tag (static string)
            uint32_t                registered  = 0;      // its AddPass number, the first sort's input
            std::vector<OrderRule>  order;         // its After / Before constraints (RenderGraph/Order.h)
            uint32_t                showFlags   = 0;   // the RG::ShowFlag bits it needs (RequiredShowFlags)
            std::vector<PassAccess> accesses;
            std::vector<Barrier>    barriers;      // what Phase A emitted before it recorded
            std::vector<uint32_t>   uavBarriers;   // textures Phase A gave a UAV barrier
            uint32_t                externalWaits = 0;      // cross-queue waits landed here (after re-homing)
            uint32_t                cl          = ~0u;      // pool slot of its graph CL ("cmd<id>"), ~0u = did not record
            uint32_t                auxTailCl   = ~0u;      // the CL Execute returned when not `cl`
            float                   cpuMs       = 0.f;      // Phase B record time, its extra tasks included
            float                   gpuMs       = -1.f;     // GPU time by name in snapshot.gpuFrame; -1 = unknown
            std::vector<uint32_t>   undeclared;    // textures it used without declaring (session-cumulative)
            // The cull report: it ran and wrote, and nothing in the graph read what it
            // wrote this frame - every texture graph-owned (not an import, not created
            // isSRV for reads outside the graph) and no RAW edge out of it. A candidate
            // only: the graph culls nothing (-rgtest_unread is the reverse arm).
            bool                    unread      = false;

            bool Ran() const { return skip == Skip::None; }
        };
        struct Texture
        {
            std::string name;
            uint32_t    width = 0, height = 0;   // resolved: a 0 in the desc means the graph's size
            uint32_t    format = 0;              // RHI::Format value
            uint32_t    arraySize = 1;
            bool        isDepth = false, isUAV = false;
            bool        graphScope = false;      // CreateTexture (stable handle) vs a pass's DeclareTexture
            bool        onDemand = false;        // TextureResidency::OnDemand
            bool        imported = false;        // ImportTexture: the device's, bound per Execute
            // Imported only: RG::ImportEntry (0 stale, 1 fresh, 2 history) and
            // RG::ImportTracking (0 graph, 1 owner - the graph never transitions it; its
            // states here are UNDEFINED).
            uint8_t     importEntry    = 0;
            uint8_t     importTracking = 0;
            // An import's resource as the device names it (its debug name, e.g.
            // "GraphicsDX12.HdrEditorViewport"); "" = the graph's own name is the device's.
            // The frame capture matches the journal by it.
            std::string deviceName;
            bool        backed = false;          // has a physical resource this frame
            uint32_t    initialState = 0;        // the graph tracker before this Execute
            uint32_t    finalState   = 0;        // ... and after it
            // A depth-stencil format: the two states above are its depth plane's, these
            // its stencil plane's.
            bool        planar = false;
            uint32_t    stencilInitialState = 0;
            uint32_t    stencilFinalState   = 0;
            int32_t     firstUse = -1;           // first / last pass that RAN and declared it; -1 = none
            int32_t     lastUse  = -1;
        };
        struct Dependency
        {
            uint32_t from = 0, to = 0, texture = 0;
            Edge     kind = Edge::RAW;
            // Which planes of the texture carry it: bit 0 the depth plane (the whole of
            // any other texture), bit 1 the stencil plane.
            uint8_t  planes = 1;
        };
        enum class Check : uint8_t { StaleRead, InactiveProducer, ConflictInPass, OpaquePass, UavNeedsBarrier,
                                     ClearAfterLive, StencilOverlap };
        struct Finding
        {
            Check    check   = Check::StaleRead;
            uint32_t pass    = 0;
            uint32_t texture = ~0u;   // ~0u = none (OpaquePass)
            uint32_t other   = ~0u;   // UavNeedsBarrier: the pass that used it as UAV just before;
                                      // ClearAfterLive: the pass whose content the clear wipes;
                                      // StencilOverlap: the pass that writes the tested bit after it
        };

        uint64_t                frame    = 0;    // the owner's frame stamp (SetSnapshotFrame)
        uint64_t                sequence = 0;    // the board's publish counter
        int64_t                 gpuFrame = -1;   // Diag::Prof GPU frame gpuMs came from; -1 = none
        uint32_t                width = 0, height = 0;
        // The view the graph drew (RenderGraph::SetView): its name and RG::ShowFlags bits.
        std::string             viewName;
        uint32_t                viewShow = 0;
        // Where it looked (SceneView::cull): the volume's kind (RG::CullVolume::Kind),
        // the eye and the ViewBit it draws.
        uint8_t                 viewCullKind = 0;
        float                   viewEye[3]   = {};
        uint32_t                viewBit      = 0;
        std::vector<Pass>       passes;
        std::vector<Texture>    textures;
        std::vector<Dependency> edges;
        std::vector<Finding>    findings;   // this frame's, in pass order
        double                  deriveUs = 0.0;   // what deriving the DAG cost this frame

        // What a validator found in the DAG of the passes that ran
        // (RenderGraph/RenderGraphDag.h says what each check means).
        static const char* CheckName(Check c)
        {
            switch (c)
            {
            case Check::StaleRead:        return "StaleRead";
            case Check::InactiveProducer: return "InactiveProducer";
            case Check::ConflictInPass:   return "ConflictInPass";
            case Check::OpaquePass:      return "OpaquePass";
            case Check::UavNeedsBarrier: return "UavNeedsBarrier";
            case Check::ClearAfterLive:  return "ClearAfterLive";
            case Check::StencilOverlap:  return "StencilOverlap";
            }
            return "?";
        }

        // The names the -rgdump JSON, the `rg` verb and the editor's Render Graph
        // panel print - here, not in the harness's dump, so a Retail editor has them too.
        static const char* AccessName(Access a)
        {
            switch (a)
            {
            case Access::Read:         return "read";
            case Access::RenderTarget: return "renderTarget";
            case Access::Depth:        return "depth";
            case Access::UAV:          return "uav";
            case Access::Manual:       return "manual";
            case Access::History:      return "history";
            case Access::Scoped:       return "scoped";
            case Access::DsvRead:      return "dsvRead";
            case Access::CopySrc:      return "copySrc";
            case Access::CopyDst:      return "copyDst";
            }
            return "?";
        }
        static const char* SkipName(Skip s)
        {
            switch (s)
            {
            case Skip::None:     return "none";
            case Skip::Inactive: return "inactive";
            case Skip::Hidden:   return "hidden";
            case Skip::View:     return "view";
            }
            return "?";
        }
        static const char* EdgeName(Edge e)
        {
            switch (e)
            {
            case Edge::RAW: return "RAW";
            case Edge::WAR: return "WAR";
            case Edge::WAW: return "WAW";
            }
            return "?";
        }
        // A plane of a depth-stencil texture (PassAccess::plane; -1, the whole, has no
        // name); an edge's planes (Dependency::planes).
        static const char* PlaneName(int plane) { return plane == 1 ? "stencil" : "depth"; }
        static const char* PlanesName(uint8_t planes)
        {
            return planes == 3 ? "both" : (planes == 2 ? "stencil" : "depth");
        }
        // A render target's load op (PassAccess::load).
        static const char* LoadName(uint8_t load)
        {
            switch (load)
            {
            case 1:  return "clear";
            case 2:  return "dontCare";
            default: return "load";
            }
        }
        // An RHI::ResourceState value by its enumerator name; a combination (which the
        // graph does not produce) as hex.
        static std::string StateName(uint32_t s);

        uint32_t FindingCount(Check c) const
        {
            uint32_t n = 0;
            for (const Finding& f : findings) n += f.check == c ? 1u : 0u;
            return n;
        }
        uint32_t UavBarrierCount() const
        {
            uint32_t n = 0;
            for (const Pass& p : passes) n += static_cast<uint32_t>(p.uavBarriers.size());
            return n;
        }

        // Totals, the numbers [RG_SNAPSHOT] prints.
        uint32_t RanCount()       const;
        uint32_t BackedCount()    const;
        uint32_t BarrierCount()   const;
        uint32_t WaitCount()      const;
        uint32_t AuxTailCount()   const;
        uint32_t UndeclaredCount()const;
        uint32_t UnreadCount()    const;   // the cull candidates (Pass::unread)
    };

    // ==== Board<T> ====
    // The mailbox between whoever builds a snapshot on the render half and whoever
    // asked (-rgdump, the `rg` verb on the main thread, the editor's Render Graph
    // panel on the UI thread). Thread-safe; one board per producer. SnapshotBoard
    // carries a graph's snapshots; RG::FrameCaptures() the joined graph + device
    // frame (RenderGraph/FrameCapture.h).
    // PROTOCOL. A reader calls Request(), keeps the sequence it returns, and waits
    // until PublishedSequence() is past it; Latest() is then a snapshot PUBLISHED
    // after the request - normally of the next frame built (a build already under
    // way can answer it one frame early). Readers asking in the same frame share one
    // build. The producer calls ConsumeRequest() once per frame and, when it returns
    // true, MUST Publish() - for a graph, before Execute returns, the nothing-active
    // early-out included - or the reader waits forever. T carries a
    // `uint64_t sequence` the board stamps.
    template <class T>
    class Board
    {
    public:
        uint64_t Request()
        {
            const uint64_t seq = m_published.load(std::memory_order_acquire);
            m_wanted.store(true, std::memory_order_release);
            return seq;
        }
        // One relaxed load on the frames nobody asked (the common case).
        bool ConsumeRequest()
        {
            return m_wanted.load(std::memory_order_relaxed) &&
                   m_wanted.exchange(false, std::memory_order_acq_rel);
        }
        void Publish(std::shared_ptr<T> snapshot)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // The sequence is stamped under the lock, so Latest() never returns
            // a snapshot whose number is behind PublishedSequence().
            const uint64_t seq = m_published.load(std::memory_order_relaxed) + 1;
            if (snapshot) snapshot->sequence = seq;
            m_latest = std::move(snapshot);
            m_published.store(seq, std::memory_order_release);
        }
        std::shared_ptr<const T> Latest() const
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            return m_latest;
        }
        uint64_t PublishedSequence() const { return m_published.load(std::memory_order_acquire); }

    private:
        std::atomic<bool>        m_wanted{ false };
        std::atomic<uint64_t>    m_published{ 0 };
        mutable std::mutex       m_mutex;
        std::shared_ptr<const T> m_latest;
    };

    using SnapshotBoard = Board<GraphSnapshot>;

    // The Renderer's main graph publishes here (Renderer::Compile binds it); the
    // preview graphs publish nowhere.
    SnapshotBoard& MainGraphSnapshots();
}
