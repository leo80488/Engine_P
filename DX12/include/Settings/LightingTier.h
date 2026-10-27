#pragma once

// Settings/LightingTier.h - the lighting tier as a preset of player rows.
//
// Choosing a tier writes the rows below as if the player had changed each of them; the
// tier the frame runs is still derived from the rows (FramePlan::PlanFrame,
// Decisions::tier), and a row changed afterwards makes it "custom" (tierPartial), not
// another tier. The tier is not a setting and is never saved: settings.json keeps the
// rows. Rows a tier does not name (the AA mode, SSR, the ReSTIR / PT engine.json knobs)
// keep their values. The Project Settings page (Ray Tracing) and Lua
// (Settings.setLightingTier) both read this one table.
//
// features.ddgi is the player's allowance (see Policy::Capped): a scene whose
// IndirectLightingSettings leaves DDGI off (or that has no DDGI volume) reads T0 after T1
// is chosen - the tier is what runs, derived from the plan. The same holds for a device
// without inline ray tracing, where every tier above T0 reads T0 (LightingTierNeedsRT).

#include "Renderer/FramePlan.h"

#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>

namespace Settings
{
    // One row a tier writes: a Bool row's 0 / 1, or features.renderPath's
    // label index (Renderer::RenderPath, mirrored by FramePlan::Path).
    struct TierWrite
    {
        const char* path;
        int         value;
    };

    namespace TierDetail
    {
        constexpr int Path(FramePlan::Path p) { return static_cast<int>(p); }
        // T0..T2 render Raster; T3 is ReSTIR DI + GI with RT reflections forced on and the T2
        // AO; T4 / Ref only choose their path tracer: what it replaces is off while it owns the
        // frame (FramePlan::Decisions::ptOwns), so the raster switches stay as they are for
        // when the player comes back. They write no AA mode either: Ray Reconstruction runs
        // only under the DLSS mode with its RR box, which the player chooses separately.
        inline constexpr TierWrite kT0[] = {
            { "features.renderPath", Path(FramePlan::Path::Raster) },
            { "features.rtReflections", 0 }, { "features.rtShadows", 0 }, { "features.rtao", 0 },
            { "features.ssao", 1 }, { "features.ddgi", 0 },
        };
        inline constexpr TierWrite kT1[] = {
            { "features.renderPath", Path(FramePlan::Path::Raster) },
            { "features.rtReflections", 0 }, { "features.rtShadows", 0 }, { "features.rtao", 0 },
            { "features.ssao", 1 }, { "features.ddgi", 1 },
        };
        inline constexpr TierWrite kT2[] = {
            { "features.renderPath", Path(FramePlan::Path::Raster) },
            { "features.rtReflections", 1 }, { "features.rtShadows", 1 }, { "features.rtao", 1 },
            { "features.ssao", 1 }, { "features.ddgi", 1 },
        };
        inline constexpr TierWrite kT3[] = {
            { "features.renderPath", Path(FramePlan::Path::ReSTIR_DI_GI) },
            { "features.rtReflections", 1 }, { "features.rtShadows", 1 }, { "features.rtao", 1 },
            { "features.ssao", 1 }, { "features.ddgi", 1 },
        };
        inline constexpr TierWrite kT4[]  = { { "features.renderPath", Path(FramePlan::Path::PathTraceRealtime) } };
        inline constexpr TierWrite kRef[] = { { "features.renderPath", Path(FramePlan::Path::PathTrace) } };
    }

    inline constexpr FramePlan::Tier kLightingTiers[] = {
        FramePlan::Tier::T0, FramePlan::Tier::T1, FramePlan::Tier::T2,
        FramePlan::Tier::T3, FramePlan::Tier::T4, FramePlan::Tier::Ref,
    };

    // The rows choosing @p t writes, in this order.
    inline std::span<const TierWrite> LightingTierWrites(FramePlan::Tier t)
    {
        switch (t)
        {
        case FramePlan::Tier::T0:  return TierDetail::kT0;
        case FramePlan::Tier::T1:  return TierDetail::kT1;
        case FramePlan::Tier::T2:  return TierDetail::kT2;
        case FramePlan::Tier::T3:  return TierDetail::kT3;
        case FramePlan::Tier::T4:  return TierDetail::kT4;
        case FramePlan::Tier::Ref: return TierDetail::kRef;
        default:                   return {};
        }
    }

    // "T0".."T4", "Ref" - the names FramePlan::TierName prints without a
    // partial mark.
    inline const char* LightingTierKey(FramePlan::Tier t)
    {
        switch (t)
        {
        case FramePlan::Tier::T0:  return "T0";
        case FramePlan::Tier::T1:  return "T1";
        case FramePlan::Tier::T2:  return "T2";
        case FramePlan::Tier::T3:  return "T3";
        case FramePlan::Tier::T4:  return "T4";
        case FramePlan::Tier::Ref: return "Ref";
        default:                   return "?";
        }
    }

    // One line for a menu: what the tier lights the frame with.
    inline const char* LightingTierSummary(FramePlan::Tier t)
    {
        switch (t)
        {
        case FramePlan::Tier::T0:  return "Raster: shadow maps, sky light, SSR, XeGTAO";
        case FramePlan::Tier::T1:  return "Raster + DDGI";
        case FramePlan::Tier::T2:  return "Hybrid RT: DDGI, RT reflections, RT sun shadows, RTAO";
        case FramePlan::Tier::T3:  return "ReSTIR DI + GI, RT reflections";
        case FramePlan::Tier::T4:  return "Path tracer (real-time, accumulating)";
        case FramePlan::Tier::Ref: return "Path tracer (reference, accumulating)";
        default:                   return "";
        }
    }

    // Whether @p t needs inline ray tracing (every tier above T0). T1's DDGI pass fails to initialize
    // without it, so the plan derives T0 there; a menu greys the tier out and gives the reason below.
    inline bool LightingTierNeedsRT(FramePlan::Tier t)
    {
        return t != FramePlan::Tier::T0;
    }

    inline constexpr const char* kLightingTierNeedsRTReason =
        "Needs inline ray tracing (DXR 1.1), which this GPU does not report.";

    // Why a menu must not offer @p t on a device with or without inline ray tracing (the reason to
    // show), or nullptr when it may. The editor's tier combo and Settings.caps().lightingTiers both
    // ask it (SetFlags.cpp).
    const char* LightingTierUnavailableReason(FramePlan::Tier t, bool inlineRaytracing);

    // "T0".."T4" / "Ref", case-insensitive. False for anything else.
    inline bool ParseLightingTier(std::string_view s, FramePlan::Tier& out)
    {
        for (FramePlan::Tier t : kLightingTiers)
        {
            const std::string_view key = LightingTierKey(t);
            if (key.size() != s.size()) continue;
            bool same = true;
            for (size_t i = 0; i < s.size() && same; ++i)
                same = std::tolower(static_cast<unsigned char>(s[i])) == std::tolower(static_cast<unsigned char>(key[i]));
            if (same) { out = t; return true; }
        }
        return false;
    }
}
