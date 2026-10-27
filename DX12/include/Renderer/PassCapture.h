#pragma once
// Renderer/PassCapture.h - a main-graph texture's content right after one pass.
//
// The graph copies the texture on a command list of its own, right after the
// pass, in the order the GPU runs it (RenderGraph::ArmTextureCopy): the copy is
// a texture with the same desc, owned by the Renderer. Three askers:
//   * the editor's Render Graph panel - its texture Inspector's preview: a
//     picture of the copy (RGBA8, the debug target shader, its own constants)
//     for ImGui::Image;
//   * the pipe's capture {"attachment", "afterPass"} (SDK eng.capture(...,
//     after_pass=)): the harness reads the copy back between frames;
//   * -rgcapture=<texture>[@<pass>] (harness): every frame, [RG_CAPTURE].
//
// Three contexts, as TextureViewService: Renderer::RequestPassCapture /
// ReleasePassCapture only latch (any thread); the next frame boundary
// (ServiceEnableGatedResources) resolves the names, makes the copy and the
// picture and arms the graph; the render half's restore list paints the picture
// after the graph and publishes the Status. One request at a time: a new one
// replaces the last. Nothing asked: nothing recorded, and the copy and picture
// are released a few hundred frames later.
//
// Never copied (Status::why): a texture that is not backed, an owner-tracked
// import (its owner holds its state), a pass that did not run. A frame that
// captures the render graph (the panel's frame capture) is skipped - its
// barrier journal would carry the copy - and the next one copies.

#include "Renderer/DebugTargets.h"

#include <cstdint>
#include <string>

namespace PassCapture
{
    struct Desc
    {
        std::string   texture;               // a main-graph texture's name ("GBuffer_Depth", "HdrScene")
        std::string   afterPass;             // a pass's instance name; empty = after the last pass that runs
        bool          everyFrame = false;    // armed every frame until another request or Release (-rgcapture)
        bool          picture    = false;    // paint the copy (the Render Graph panel's preview)
        int           plane      = 0;        // the plane a depth-stencil copy's picture shows (1 = stencil)
        DebugTargets::View view;             // the picture's mapping; mode Auto = the texture's own (name unused)
        std::uint64_t id         = 0;        // the asker's, echoed in Status
    };

    struct Status
    {
        std::uint64_t seq     = 0;           // bumped with every result (0 = none yet)
        std::uint64_t id      = 0;           // the request's
        std::uint64_t frame   = 0;           // the frame the copy was made in
        bool          done    = false;       // this result made a copy
        bool          pending = false;       // asked, no copy yet (the frame boundary, a skipped frame)
        std::string   why;                   // why not done
        std::string   texture;
        std::string   afterPass;             // the pass the copy followed (the last pass's name for "")
        std::uint32_t width = 0, height = 0, arraySize = 1;
        std::uint32_t format = 0;            // RHI::Format value
        bool          depth = false;         // a depth-stencil format: two planes
        // The picture (Desc::picture): its SRV for ImGui::Image (0 = none - why in
        // pictureWhy) and the mapping painted, Auto resolved.
        std::uint64_t pictureSrv = 0;
        std::uint32_t pictureW = 0, pictureH = 0;
        DebugTargets::View shown;
        std::string   pictureWhy;
    };
}
