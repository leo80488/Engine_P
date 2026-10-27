#pragma once
// RenderGraph/SceneView.h - what one view of a graph shows.
// A graph's owner (the Renderer for the main graph, the preview for each of its
// GraphSets) hands its graph one SceneView with RenderGraph::SetView once it
// knows what the view shows (main view: when the view mode and the path
// tracer's camera primary are known; preview: right before each Execute). The
// graph then sets every pass's Shown() from the ShowFlag it declares
// (RenderPass::RequiredShowFlags) at once - the forward fog reads a fog pass's
// Shown() before the graph runs - and its activity gate skips a pass that is not
// shown: no command list, no barrier, no Execute. Passes read the view in
// Execute through RenderContext::GetView().
// The flags are "draw it or not" per kind of content; HOW a view draws (the
// wireframe PSO, a debug view mode) is not a flag. The set is only what some view
// hides: the main view's environment (kShowEnvironment) and, in wireframe, its
// water; the preview's sky, studio shadow and forward pass. A new flag is one
// enumerator, kShowFlagCount raised, its name in ShowFlagName, and the pass that
// needs it declaring it.

#include <cstdint>
#include <string>
#include <string_view>

class ViewState;   // RenderGraph/ViewState.h

namespace RG
{
    enum class ShowFlag : uint32_t
    {
        Sky           = 1u << 0,   // SkyboxPass: the sky / HDRI backdrop
        Clouds        = 1u << 1,   // CloudPass
        VolumetricFog = 1u << 2,   // VolumetricFogPass
        HeightFog     = 1u << 3,   // HeightFogPass (a render feature)
        Water         = 1u << 4,   // WaterPass (the planar water; mesh water is a material)
        Shadows       = 1u << 5,   // the preview's studio shadow (the main view's shadow passes declare none)
        Transparent   = 1u << 6,   // TransparentPass (the preview's forward pass)
        // The editor's overlays: the grid, the debug wires and icons, the selection outline. A Scene
        // view shows them, a game view does not (the main view hides them while it is the Game View).
        EditorOverlays = 1u << 7,
        // The screen-space Canvas HUD (UIPass): a game view lays it out at its own size.
        Hud            = 1u << 8,
    };
    inline constexpr uint32_t kShowFlagCount = 9;
    inline constexpr uint32_t kShowAll       = (1u << kShowFlagCount) - 1u;
    // What the main view hides in wireframe, a debug view mode, under an ortho
    // camera and the path tracer's camera primary: the sky, the clouds, both fogs.
    inline constexpr uint32_t kShowEnvironment =
        static_cast<uint32_t>(ShowFlag::Sky) | static_cast<uint32_t>(ShowFlag::Clouds) |
        static_cast<uint32_t>(ShowFlag::VolumetricFog) | static_cast<uint32_t>(ShowFlag::HeightFog);

    inline const char* ShowFlagName(ShowFlag f)
    {
        switch (f)
        {
        case ShowFlag::Sky:           return "sky";
        case ShowFlag::Clouds:        return "clouds";
        case ShowFlag::VolumetricFog: return "volumetricFog";
        case ShowFlag::HeightFog:     return "heightFog";
        case ShowFlag::Water:         return "water";
        case ShowFlag::Shadows:       return "shadows";
        case ShowFlag::Transparent:   return "transparent";
        case ShowFlag::EditorOverlays: return "editorOverlays";
        case ShowFlag::Hud:            return "hud";
        }
        return "?";
    }
    // The flag a name names (ShowFlagName's words, case-sensitive); false for none.
    inline bool ShowFlagFromName(std::string_view name, ShowFlag& out)
    {
        for (uint32_t b = 0; b < kShowFlagCount; ++b)
            if (name == ShowFlagName(static_cast<ShowFlag>(1u << b)))
            {
                out = static_cast<ShowFlag>(1u << b);
                return true;
            }
        return false;
    }
    // @p bits as their names in enumerator order, joined by @p sep ("" for none).
    inline std::string ShowFlagList(uint32_t bits, const char* sep = ",")
    {
        std::string out;
        for (uint32_t b = 0; b < kShowFlagCount; ++b)
            if (bits & (1u << b))
            {
                if (!out.empty()) out += sep;
                out += ShowFlagName(static_cast<ShowFlag>(1u << b));
            }
        return out;
    }

    // A view's flags; every one shown by default.
    struct ShowFlags
    {
        uint32_t bits = kShowAll;

        bool Has(ShowFlag f) const { return (bits & static_cast<uint32_t>(f)) != 0; }
        bool HasAll(uint32_t required) const { return (bits & required) == required; }
        void Show(ShowFlag f, bool on = true)
        {
            bits = on ? (bits | static_cast<uint32_t>(f)) : (bits & ~static_cast<uint32_t>(f));
        }
        void Hide(ShowFlag f) { Show(f, false); }
        void HideAll(uint32_t flags) { bits &= ~flags; }
        bool operator==(const ShowFlags& o) const { return bits == o.bits; }
        bool operator!=(const ShowFlags& o) const { return bits != o.bits; }
    };

    // ---- Where a view looks, for culling ----
    // The volume Renderer::CullView asks the scene BVH with. Planes: six inward
    // planes (a, b, c, d: a*x + b*y + c*z + d >= 0 inside - FrustumPlanes'
    // layout, ExtractFrustumPlanes); Box: an axis-aligned box; None: the view
    // culls nothing (every packet is drawn and the GPU clips).
    struct CullVolume
    {
        enum class Kind : uint8_t { None, Planes, Box };
        Kind  kind = Kind::None;
        float planes[6][4] = {};
        float boxMin[3] = {}, boxMax[3] = {};
    };
    inline const char* CullVolumeKindName(CullVolume::Kind k)
    {
        switch (k)
        {
        case CullVolume::Kind::None:   return "none";
        case CullVolume::Kind::Planes: return "planes";
        case CullVolume::Kind::Box:    return "box";
        }
        return "?";
    }
    struct ViewCull
    {
        CullVolume volume;
        float      eye[3]     = {};                  // where it looks from
        float      forward[3] = { 0.f, 0.f, 1.f };   // its planar view depth (the transparent sort)
        uint32_t   viewBit    = 0;                   // the ViewBit (VisibilityComponent) it draws; 0 = no test
    };

    struct SceneView
    {
        const char* name = "main";      // the main view, or a preview's lane ("preview", "thumbnail", "world"), "probe"
        ShowFlags   show;
        uint8_t     viewMode = 0;       // Renderer::ViewMode (the preview's ViewDesc::viewMode); the graph does not read it
        bool        orthographic = false;
        uint32_t    width = 0, height = 0;   // the render size
        ::ViewState* state = nullptr;   // the view's cross-frame state; only the main view sets it
        ViewCull    cull;               // where it looks; the graph does not read it
    };
}
