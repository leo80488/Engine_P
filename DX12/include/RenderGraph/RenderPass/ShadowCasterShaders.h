#pragma once
// ShadowCasterShaders.h - the shaders a masked custom material's shadow is
// drawn with: ONE rule for the CSM, spot, point and per-object / hair passes, so
// a caster cannot cast one silhouette into the sun's map and another into a
// spot's.
//
// A packet with DrawPacket::shadowHookPS (a masked material with a custom PS) is
// drawn in the passes' per-draw loops (ShadowDrawsPerDraw) by its OWN pixel
// shader in the SHADOW_HOOK variant - the hook decides coverage exactly as the
// GBuffer does (custom_surface.hlsli) - fed by a VS that outputs the full
// MaterialSurfaceVaryings in the shadow view: the material's custom VS, or
// ShadowHook_VS. A hook that does not compile in that variant falls back to the
// plain caster (Shadow.vs or its custom VS, with Shadow.ps), said once per
// shader - a caster never loses its shadow altogether.

#include "Graphics/PSOCache.h"
#include "Graphics/RenderTypes.h"     // DrawPacket
#include "Graphics/ShaderLibrary.h"
#include "System/Log.h"

#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace ShadowCaster
{
    // The hook PS a packet's shadow is drawn with, 0 = none (Shadow.ps or no PS).
    inline uint32_t HookPS(const DrawPacket& dp)
    {
        return dp.shadowHookPS ? dp.customPSID : 0u;
    }

    // PSODesc's shaders and permutation for the SHADOW_HOOK draw of @p hookPSID.
    inline void SelectHookShaders(PSODesc& desc, PermutationKey& perm, uint32_t customVSID, uint32_t hookPSID)
    {
        desc.vsID = customVSID != 0 ? static_cast<ShaderID>(customVSID) : ShaderID::ShadowHook_VS;
        desc.psID = static_cast<ShaderID>(hookPSID);
        perm.Set(PermutationKey::ALPHA_TEST, true);
        perm.Set(PermutationKey::CUSTOM_VS_SHADOW, true);
        perm.Set(PermutationKey::SHADOW_HOOK, true);
    }

    // The SHADOW_HOOK PSO of @p hookPSID did not build: say so once per shader
    // (every pass and frame after it reuses the PSOCache's negative entry and
    // this fallback silently).
    inline void NoteHookFallback(uint32_t hookPSID, const char* pass)
    {
        static std::mutex                   s_mutex;
        static std::unordered_set<uint32_t> s_warned;
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_warned.insert(hookPSID).second)
            LOG_WARNING("%s: the SHADOW_HOOK variant of custom shader %u did not build - its masked casters "
                        "keep Shadow.ps (the engine texture slots' alpha) instead of the hook's opacity "
                        "(render-arch MT3)", pass, hookPSID);
    }
}
