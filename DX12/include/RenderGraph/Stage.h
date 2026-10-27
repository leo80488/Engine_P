#pragma once

// RenderGraph/Stage.h - where in the frame a pass belongs. One enum for the main
// graph's stages and for the work the Renderer records around the graph.
//
// The stage is the first sort key. RenderGraph::AddPass(stage, ...) tags a pass;
// Compile's first sort (RenderGraph/Order.h) puts a graph's tagged passes in stage
// order - inside a stage by their After / Before constraints, then by registration -
// and that is the order the graph runs in. The stage check confirms that no tagged
// pass goes back to an earlier stage and logs the census as [RG_STAGES]; the graph
// snapshot (-rgdump, the `rg` verb) and the catalog's passes.json carry each tag.
//
// The in-graph stages are the main graph's: Renderer::Compile tags all its passes.
// The others name the work around the graph (the prepass and the post tail
// RenderAsync records by hand) where a render feature has an insertion point.
// Unstaged is what the AddPass overloads without a stage give: the preview graphs
// and the harness's test passes, which the sort leaves in place and the order check
// skips.
//
// Rules the graph checks on staged passes: the stage order; only the GBuffer stage
// clears the HDR scene colour (BuiltinTexture::HdrSceneColor); an external wait
// (SetExternalWait - the compute queue's DDGI / TLAS promote) lands no earlier than
// LightingInputs. The stencil bits' registry is StencilBits.h.
// TODO(RenderGraph_AutoTopology_NodeView_Plan M6): the DAG does not see the alias
// arena groups' lifetimes yet; only the pass order keeps them disjoint
// (TransientAliasArena.h).
//
// Not FramePlan::Stage, RG::PrepareStage, RG::ServiceStage, ReSTIR::Stage or
// PostProcess::Stage: those order other things.

#include <cstdint>

namespace RG
{
enum class Stage : uint8_t
{
    Prepass,                // before the graph: skinning, the simulations, cluster, cull

    // The main graph, in this order:
    GBuffer,                // the GBuffer and what draws beside it (terrain, grass, outline / hair marks)
    ShadowAndEnvironment,   // sky IBL; spot, point, per-object and hair shadow maps
    GBufferModify,          // decals onto the GBuffer
    LightingInputs,         // what lighting reads: SSGI, RT shadow / AO / reflections, XeGTAO, ReSTIR, path tracing
    Lighting,               // the deferred lighting
    SkyAndWater,            // sky, water, the GBuffer restore after water
    Atmosphere,             // clouds, height fog, video, volumetric fog, the NPR and RR snapshots
    Transparent,            // forward translucents, particles, trails, tracers, billboards
    PostOpaque,             // the outline composite
    Editor,                 // picking

    // After the graph (the post tail):
    PreResolve,             // before the AA / upscaler: RR guides, velocity fill
    Resolve,                // TAA / FSR 2 / DLSS, FXAA
    PostResolveHDR,         // the HDR post stack before the tonemap
    Tonemap,
    PostTonemap,
    Overlay,                // debug wire, world UI, icons, UI

    Count,
    Unstaged = 0xFF,
};

inline const char* StageName(Stage s)
{
    switch (s)
    {
    case Stage::Prepass:              return "Prepass";
    case Stage::GBuffer:              return "GBuffer";
    case Stage::ShadowAndEnvironment: return "ShadowAndEnvironment";
    case Stage::GBufferModify:        return "GBufferModify";
    case Stage::LightingInputs:       return "LightingInputs";
    case Stage::Lighting:             return "Lighting";
    case Stage::SkyAndWater:          return "SkyAndWater";
    case Stage::Atmosphere:           return "Atmosphere";
    case Stage::Transparent:          return "Transparent";
    case Stage::PostOpaque:           return "PostOpaque";
    case Stage::Editor:               return "Editor";
    case Stage::PreResolve:           return "PreResolve";
    case Stage::Resolve:              return "Resolve";
    case Stage::PostResolveHDR:       return "PostResolveHDR";
    case Stage::Tonemap:              return "Tonemap";
    case Stage::PostTonemap:          return "PostTonemap";
    case Stage::Overlay:              return "Overlay";
    case Stage::Unstaged:             return "Unstaged";
    default:                          return "?";
    }
}
}
