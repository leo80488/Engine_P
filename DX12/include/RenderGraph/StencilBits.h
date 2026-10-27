#pragma once

// RG::StencilBits - the scene depth's stencil byte: which passes write and which
// test each of its bits. The values are ShadingStencil's (Graphics/ShadingStencil.h);
// this names their owners, so the byte's three users do not depend on pass order to
// stay apart:
//
//   ShadingCode 0x3F  the lighting route (1 PBR, 2 NPR, 3 Unlit): stamped by
//                     the GBuffer-stage writers, tested EQUAL by LightingPass's
//                     variants and by HairShadowMarkPass
//   TaaExclude  0x40  a particle / trail kept out of TAA's history
//                     (excludeFromTAA)
//   OutlineRim  0x80  the outline rim (OutlinePass's inverted hull, while it
//                     tags for TAA)
//
// GBufferPass clears the byte to 0 before its first draw (kClearedBy): every field
// starts the frame at 0, and that declaration writes all eight bits.
//
// Checks. Each Compile compares every DepthStencil declaration's stencil mask
// (RenderGraphBuilder::DepthStencil) - and a stencil-plane SRV, which reads the whole
// byte - with the fields that name its pass: a write outside its writer fields, or a
// test outside its reader fields, is StencilOwner ([RG_STENCIL] owner; an ERROR under
// -rgdagverify), and the census [RG_STENCIL] field= writers= readers= lists the
// graph's passes that declare each field. The frame's DAG keeps one resource per
// stencil bit, so a reader depends only on the writers of its own bits;
// StencilOverlap is a pass that tests a bit an active pass writes after it in the
// same frame (RenderGraph/RenderGraphDag.h). The device reports a draw whose pipeline
// writes bits outside its pass's declaration (the depth tripwire,
// RHI::DepthView::stencilWriteMask).
//
// A pass is named by its GetName() up to a '.': a preview graph's
// "GBufferPass.PreviewThumb" is GBufferPass. Readers outside the graph are not
// checked: TAA and the upscaler's reactive mask read TaaExclude and OutlineRim through
// the stencil-plane SRV after it, and the debug target `stencil` shows the byte.
// TODO(RenderGraph_AutoTopology_NodeView_Plan M7): bring those readers into the graph.
//
// A new bit: the byte is full - ShadingCode keeps six bits for codes that use two;
// narrow kCodeMask first. Then add a field naming the writers and the readers,
// declare its mask in their Setup, and give the writers' pipelines exactly that
// stencilWriteMask.

#include "Graphics/ShadingStencil.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace RG::StencilBits
{
    struct Field
    {
        uint8_t     mask;
        const char* name;
        const char* writers[4];   // pass names (GetName() up to a '.'); unused entries null
        const char* readers[2];
    };

    inline constexpr Field kFields[] = {
        { ShadingStencil::kCodeMask, "ShadingCode",
          { "GBufferPass", "OutlineGeometryPass", "TerrainPass", "GrassPass" },
          { "HairShadowMarkPass", "LightingPass" } },
        { ShadingStencil::kTaaExclude, "TaaExclude", { "ParticleRenderPass", "TrailRenderPass" }, {} },
        { ShadingStencil::kOutlineRim, "OutlineRim", { "OutlinePass" }, {} },
    };

    // The pass that starts the frame's byte at 0 (ClearDepthStencil before its
    // first draw): its declaration writes every bit.
    inline constexpr std::string_view kClearedBy = "GBufferPass";

    constexpr bool FieldsDisjoint()
    {
        uint32_t seen = 0;
        for (const Field& f : kFields)
        {
            if (seen & f.mask) return false;
            seen |= f.mask;
        }
        return true;
    }
    static_assert(FieldsDisjoint(), "a stencil bit belongs to one field");

    // A pass's name here: its GetName() up to a '.'.
    inline std::string_view BaseName(const char* name)
    {
        const std::string_view n = name ? std::string_view(name) : std::string_view();
        const size_t dot = n.find('.');
        return dot == std::string_view::npos ? n : n.substr(0, dot);
    }

    template <size_t N>
    constexpr bool Names(const char* const (&list)[N], std::string_view base)
    {
        for (const char* p : list)
            if (p && base == p) return true;
        return false;
    }

    // The bits @p base may write: its writer fields' - every bit for the pass
    // that clears the byte.
    inline uint8_t WriteMask(std::string_view base)
    {
        uint32_t m = base == kClearedBy ? 0xFFu : 0u;
        for (const Field& f : kFields)
            if (Names(f.writers, base)) m |= f.mask;
        return static_cast<uint8_t>(m);
    }

    // The bits @p base may test: its reader fields'.
    inline uint8_t ReadMask(std::string_view base)
    {
        uint32_t m = 0;
        for (const Field& f : kFields)
            if (Names(f.readers, base)) m |= f.mask;
        return static_cast<uint8_t>(m);
    }
}
