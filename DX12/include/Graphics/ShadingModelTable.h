#pragma once

// ShadingModelTable - the C++ view of shaders/shading_model_table.hlsli: one row per
// ShadingModel holding the decisions other code makes ABOUT a model (its lighting route, ReSTIR
// DI / GI, the RT proxy's material slot, the graph pins it reads, the editor menus). The shaders
// read the same rows through the masks at the bottom of that file; the comment at its top says
// why one file serves both languages and what does NOT belong in it.

#include "Graphics/ShadingModel.h"

#include <cstddef>
#include <cstdint>

#include "../../shaders/shading_model_table.hlsli"

namespace ShadingModels
{
    // The table's path columns, in order.
    enum class Path : uint8_t { Deferred, Forward, RtHit, PathTrace, Ddgi, Count };

    struct Desc
    {
        ShadingModel model;
        const char*  name;       // the enumerator ("TwoSidedFoliage"): the authoring meta value
        const char*  token;      // "twosidedfoliage": MaterialGraph::ParseShadingModel
        const char*  display;    // "Two Sided Foliage": ShadingModelName - never respell (see the .hlsli)
        uint8_t      route;      // SMT_ROUTE_* - the GBuffer stencil code
        uint8_t      di;         // SMT_DI_*
        bool         gi;         // ReSTIR GI receives on it
        bool         rtMatSlot;  // the RT / DDGI proxy carries its raster material slot
        uint32_t     pins;       // SMT_PIN_* groups the material graph reads
        uint32_t     menus;      // SMT_MENU_*
        uint32_t     onSelect;   // SMT_SELECT_*
        uint8_t      paths[static_cast<size_t>(Path::Count)];   // SMT_PATH_*, declared

        constexpr bool    Reads(uint32_t pinGroup) const { return (pins & pinGroup) != 0; }
        constexpr uint8_t Support(Path p) const { return paths[static_cast<size_t>(p)]; }
    };

    inline constexpr Desc kRows[] = {
#define SMT_CPP_ROW(name, value, token, display, route, di, gi, slot, pins, menus, sel, d0, d1, d2, d3, d4) \
        { ShadingModel::name, #name, token, display, route, di, (gi) != 0, (slot) != 0,                       \
          static_cast<uint32_t>(pins), static_cast<uint32_t>(menus), static_cast<uint32_t>(sel),             \
          { d0, d1, d2, d3, d4 } },
        SHADING_MODEL_TABLE(SMT_CPP_ROW)
#undef SMT_CPP_ROW
    };
    inline constexpr size_t kRowCount = sizeof(kRows) / sizeof(kRows[0]);

    // What a value outside the enum reads. Both loaders refuse one, but a cast can still make it:
    // the PBR route and nothing else.
    inline constexpr Desc kUnknown = {
        ShadingModel::Count, "Unknown", "", "Unknown", SMT_ROUTE_PBR, SMT_DI_NEVER, false, false, 0u, 0u, 0u, {} };

    constexpr const Desc& Of(ShadingModel m)
    {
        const size_t i = static_cast<size_t>(m);
        return i < kRowCount ? kRows[i] : kUnknown;
    }

    // ==== The table agrees with the enum, row by row ====
#define SMT_CPP_CHECK(name, value, ...)                                              \
    static_assert(static_cast<uint32_t>(ShadingModel::name) == (value),             \
                  "shading_model_table.hlsli: row " #name " is not at its enum value");
    SHADING_MODEL_TABLE(SMT_CPP_CHECK)
#undef SMT_CPP_CHECK
    static_assert(kRowCount == static_cast<size_t>(ShadingModel::Count),
                  "shading_model_table.hlsli: one row per ShadingModel");

    constexpr bool RowsInEnumOrder()
    {
        for (size_t i = 0; i < kRowCount; ++i)
            if (static_cast<size_t>(kRows[i].model) != i) return false;
        return true;
    }
    static_assert(RowsInEnumOrder(), "shading_model_table.hlsli: rows must be in enum-value order");

    // ==== What each path draws instead ====
    // The table's path columns say exact / approximated / not drawn; these notes say what
    // "approximated" means, cell by cell, and where in the code. The Material Editor shows them
    // under the Shading Model menu.
    inline constexpr const char* kPathNames[] = { "Deferred", "Forward", "RT hits", "Path tracer", "DDGI" };
    static_assert(sizeof(kPathNames) / sizeof(kPathNames[0]) == static_cast<size_t>(Path::Count),
                  "a name for every path column");

    struct PathNote { ShadingModel model; Path path; const char* note; };
    inline constexpr PathNote kPathNotes[] = {
        { ShadingModel::Unlit, Path::Forward,
          "without a custom shader it is lit like Standard: the engine's default hook leaves the forward "
          "Unlit branch black, so the material record says Standard there (Renderer_Materials.cpp)" },
        { ShadingModel::Unlit, Path::Ddgi,
          "relit as a Lambert surface: the probe trace has no Unlit branch (DDGIRayTrace.cs.hlsl)" },
        { ShadingModel::ClearCoat, Path::RtHit,
          "no coat lobe: a hit is shaded from rt_instance_data, which has no coat channels" },
        { ShadingModel::ClearCoat, Path::PathTrace,
          "no coat lobe: the path tracer's BSDF is Cook-Torrance only (bsdf_sample.hlsli)" },
        { ShadingModel::ClearCoat, Path::Ddgi, "Lambert, without the coat's attenuation of the base" },
        { ShadingModel::Subsurface, Path::RtHit,
          "no wrap or transmission: rt_instance_data carries no subsurface data" },
        { ShadingModel::Subsurface, Path::PathTrace, "no wrap or transmission: Cook-Torrance only" },
        { ShadingModel::Subsurface, Path::Ddgi, "Lambert, no wrap or transmission" },
        { ShadingModel::Anisotropic, Path::RtHit,
          "isotropic GGX: rt_instance_data carries no tangent frame" },
        { ShadingModel::Anisotropic, Path::PathTrace, "isotropic: Cook-Torrance only" },
        { ShadingModel::Water, Path::Deferred,
          "never deferred: WaterSurfacePass shades it on the forward route" },
        { ShadingModel::Water, Path::RtHit,
          "not in the ray-tracing scene (TLAS mask 0): its reflections come from the screen-space chain" },
        { ShadingModel::Water, Path::PathTrace, "not in the ray-tracing scene (TLAS mask 0)" },
        { ShadingModel::Water, Path::Ddgi, "not in the ray-tracing scene (TLAS mask 0)" },
        { ShadingModel::TwoSidedFoliage, Path::RtHit, "no transmission" },
        { ShadingModel::TwoSidedFoliage, Path::PathTrace, "no transmission: Cook-Torrance only" },
        { ShadingModel::TwoSidedFoliage, Path::Ddgi, "Lambert, no transmission" },
        { ShadingModel::Toon, Path::Forward,
          "the toon model - direct light through the ramp or, without one, the two ramp colours, "
          "and the toon ambient (custom_surface.hlsli MS_ForwardToon); no face SDF, direct-additive "
          "mask or screen-space depth rim" },
        { ShadingModel::Toon, Path::RtHit,
          "the toon ramp, knobs and outline ported to one hit (HybridReflIntersectHW); on a custom-shader "
          "material the surface-cache bake freezes light-dependent decals (hair shine, rim) at one light" },
        { ShadingModel::Toon, Path::PathTrace,
          "a primary keeps the deferred toon composite with traced ambient and sun visibility "
          "(NprAmbient); a bounce off it is Cook-Torrance" },
        { ShadingModel::Toon, Path::Ddgi, "Lambert: no ramp" },
    };

    // The note on (model, path), or nullptr.
    constexpr const char* PathNoteFor(ShadingModel m, Path p)
    {
        for (const PathNote& n : kPathNotes)
            if (n.model == m && n.path == p) return n.note;
        return nullptr;
    }

    // Every approximated cell says what it approximates; a note sits only on an
    // approximated or undrawn cell, once.
    constexpr bool PathNotesMatchTheTable()
    {
        for (size_t i = 0; i < kRowCount; ++i)
            for (size_t p = 0; p < static_cast<size_t>(Path::Count); ++p)
                if (kRows[i].paths[p] == SMT_PATH_APPROX && !PathNoteFor(kRows[i].model, static_cast<Path>(p)))
                    return false;
        for (size_t a = 0; a < sizeof(kPathNotes) / sizeof(kPathNotes[0]); ++a)
        {
            const PathNote& n = kPathNotes[a];
            if (Of(n.model).Support(n.path) == SMT_PATH_FULL) return false;
            for (size_t b = a + 1; b < sizeof(kPathNotes) / sizeof(kPathNotes[0]); ++b)
                if (kPathNotes[b].model == n.model && kPathNotes[b].path == n.path) return false;
        }
        return true;
    }
    static_assert(PathNotesMatchTheTable(),
                  "kPathNotes: every APPROX cell needs exactly one note, and a FULL cell none");

    // ==== The editor's Shading Model menus ====
    // Menu order is not the enum's (the enum is serialized, so it only grows at the end). Every
    // model is listed once; a menu offers the ones whose `menus` column names it.
    inline constexpr ShadingModel kMenuOrder[] = {
        ShadingModel::Standard,   ShadingModel::Unlit,      ShadingModel::Toon,
        ShadingModel::ClearCoat,  ShadingModel::Subsurface, ShadingModel::Anisotropic,
        ShadingModel::TwoSidedFoliage, ShadingModel::Water };

    constexpr bool MenuOrderListsEveryModelOnce()
    {
        if (sizeof(kMenuOrder) / sizeof(kMenuOrder[0]) != kRowCount) return false;
        for (size_t i = 0; i < kRowCount; ++i)
        {
            int seen = 0;
            for (const ShadingModel m : kMenuOrder)
                if (static_cast<size_t>(m) == i) ++seen;
            if (seen != 1) return false;
        }
        return true;
    }
    static_assert(MenuOrderListsEveryModelOnce(), "kMenuOrder must list every ShadingModel exactly once");

    // The models menu @p menu (SMT_MENU_*) offers, in menu order, with their
    // display names (an ImGui::Combo's items). Both arrays hold kRowCount.
    inline int MenuEntries(uint32_t menu, ShadingModel* models, const char** labels)
    {
        int n = 0;
        for (const ShadingModel m : kMenuOrder)
            if (Of(m).menus & menu)
            {
                models[n] = m;
                labels[n] = Of(m).display;
                ++n;
            }
        return n;
    }
}
