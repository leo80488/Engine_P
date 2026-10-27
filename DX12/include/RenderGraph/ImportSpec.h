#pragma once

// RG::ImportSpec - how a render graph takes a texture it does not own: the HDR
// scene colour (RenderGraph::ImportTexture + BindImport) and the pass textures
// that cross pass boundaries (RenderPass::DeclareImports). Its own header so the
// snapshot dump names the kinds without the whole graph.

#include "Graphics/GraphicsStruct.h"

#include <cstdint>

namespace RG
{
    // entry - what the DAG takes the content to be when the graph begins:
    //   Stale   - a pass of this graph writes it first (the HDR scene colour);
    //             a ReadSRV before that write is a StaleRead.
    //   Fresh   - written this frame, outside and before the graph (the CSM on
    //             worker 0's shadowCL): a read with no writer before it is not
    //             stale.
    //   History - a previous frame's content (the SSR pyramid and result,
    //             written after the graph): its readers declare ReadHistory.
    enum class ImportEntry : uint8_t { Stale, Fresh, History };

    // tracking - who transitions it:
    //   Graph - the graph, from the device's book at each Execute (the HDR).
    //   Owner - its owner: the graph only records the declarations (the DAG, the
    //           snapshot) - no transition, no UAV barrier, no tripwire, and the
    //           device's book is never read for it (the CSM changes it on worker 0
    //           while the graph's serial window runs).
    enum class ImportTracking : uint8_t { Graph, Owner };

    struct ImportSpec
    {
        // The texture the graph takes at each Execute's start (serial): the
        // owner's member, so a recreation reaches the next Execute with no
        // bind. It must outlive the graph's use of it. nullptr = BindImport
        // hands one over every frame (the HDR scene colour).
        const RHI::Texture* source   = nullptr;
        ImportEntry         entry    = ImportEntry::Stale;
        ImportTracking      tracking = ImportTracking::Graph;
    };

    inline const char* ImportEntryName(ImportEntry e)
    {
        switch (e)
        {
        case ImportEntry::Stale:   return "stale";
        case ImportEntry::Fresh:   return "fresh";
        case ImportEntry::History: return "history";
        }
        return "?";
    }

    inline const char* ImportTrackingName(ImportTracking t)
    {
        return t == ImportTracking::Owner ? "owner" : "graph";
    }
}
