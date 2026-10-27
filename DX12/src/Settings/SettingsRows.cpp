#include "Settings/SettingsRows.h"

#include "Renderer/Renderer.h"        // RenderPath / AAMode tokens and bounds; the rows' setters
#include "Graphics/HDROutput.h"       // HDROutputMode <-> token
#include "Graphics/IGraphicsDevice.h" // MainSurface(): the Present phase
#include "Platform/Window.h"          // the Window phase
#include "PostProcess/ProfileSystem.h"   // the PostProcess phase
#include "UI/FontLibrary.h"           // ui.defaultFont's capture
#include "Settings/SettingBridge.h"   // faceTracking.camera: the FaceTracking module's live half
#include "ECS/CameraStackSystem.h"   // accessibility.cameraShake
#include "System/Check.h"
#include "System/CommandLine.h"       // -rt_capture_effective

#include <algorithm>
#include <cctype>
#include <iterator>

namespace Settings
{
namespace
{
    // Renderer::AAMode <-> readable token. Unknown tokens fall back to TAA, so
    // a settings.json written by a build that knew a mode this one does not
    // (or a hand-edited typo) degrades instead of refusing to load.
    const char* AAModeToToken(int m)
    {
        switch (m)
        {
        case 0:  return "None";
        case 1:  return "FXAA";
        case 3:  return "FXAA_TAA";
        case 4:  return "FSR";
        case 5:  return "DLSS";
        case 6:  return "DLSS_RR";
        default: return "TAA";
        }
    }
    int AAModeFromToken(const std::string& s)
    {
        if (s == "None")                        return 0;
        if (s == "FXAA")                        return 1;
        if (s == "FXAA_TAA" || s == "FXAA+TAA") return 3;
        if (s == "FSR")                         return 4;
        if (s == "DLSS")                        return 5;
        if (s == "DLSS_RR")                     return 6;
        return 2;   // "TAA" / unknown
    }

    // Out of range degrades to SDR, the mode that is correct on every display.
    const char* HDRModeToToken(int m)
    {
        return HDROutputModeToString(static_cast<HDROutputMode>((m >= 0 && m <= 2) ? m : 0));
    }
    int HDRModeFromToken(const std::string& s)
    {
        return static_cast<int>(HDROutputModeFromString(s.c_str()));
    }

    const char* RenderPathToToken(int p)
    {
        return Renderer::RenderPathName(static_cast<Renderer::RenderPath>(p));
    }
    int RenderPathFromToken(const std::string& s)
    {
        Renderer::RenderPath path = Renderer::RenderPath::Raster;
        Renderer::ParseRenderPath(s, path);
        return static_cast<int>(path);
    }

    // A quality level <-> its name (Settings/Scalability.h): the level rows' labels, which
    // Table() holds them to. Any case reads; an unknown name is High, every group's default.
    constexpr const char* kLevelNames[] = { "Low", "Medium", "High", "Ultra", "Cinematic" };
    constexpr int kLevelHigh = 2;
    const char* LevelToToken(int l)
    {
        return (l >= 0 && l < static_cast<int>(std::size(kLevelNames))) ? kLevelNames[l] : kLevelNames[kLevelHigh];
    }
    int LevelFromToken(const std::string& s)
    {
        for (int l = 0; l < static_cast<int>(std::size(kLevelNames)); ++l)
        {
            const std::string_view name = kLevelNames[l];
            bool same = name.size() == s.size();
            for (size_t i = 0; same && i < s.size(); ++i)
                same = std::tolower(static_cast<unsigned char>(s[i])) == std::tolower(static_cast<unsigned char>(name[i]));
            if (same) return l;
        }
        return kLevelHigh;
    }

    // An RT switch's value for a Save: the player's request. -rt_capture_effective stores what the
    // device kept running instead (the reverse arm of the request capture).
    bool CaptureRTSwitch(bool requested, bool effective)
    {
#if WITH_HARNESS
        static const bool s_effective = Sys::CommandLine::Get().Has("-rt_capture_effective");
        if (s_effective) return effective;
#endif
        (void)effective;
        return requested;
    }

    using ES = Resource::EngineSettings;

    const std::vector<Row>& Table()
    {
        static const std::vector<Row> s_rows = []
        {
            std::vector<Row> v;
#define SETTING(K, P, M, ...) v.push_back(detail::Make##K(P, &ES::M, RowSpec{ __VA_ARGS__ }));
#include "Resource/EngineSettingsSchema.inl"
#undef SETTING
            for (const Row& r : v)
            {
                detail::CheckHooks(r.path, r.spec.phase, r.spec.apply != nullptr, r.spec.capture != nullptr);
                // The enum label lists are presentation, but Range sanitizes
                // with them: a label missing from antialiasing.mode would clamp
                // a valid DLSS_RR back to TAA on every load.
                if (std::string_view(r.path) == "antialiasing.mode")
                    ENGINE_CHECK(r.labels.size() == Renderer::kMaxAAMode + 1,
                                 "antialiasing.mode has %zu labels, Renderer::AAMode has %u modes",
                                 r.labels.size(), Renderer::kMaxAAMode + 1);
                if (std::string_view(r.path) == "features.renderPath")
                    ENGINE_CHECK(r.labels.size() == static_cast<size_t>(Renderer::kMaxRenderPath) + 1,
                                 "features.renderPath has %zu labels, Renderer::RenderPath has %d paths",
                                 r.labels.size(), static_cast<int>(Renderer::kMaxRenderPath) + 1);
                // A level row's labels are the level names its codec writes.
                if (r.spec.codec == &kLevelCodec)
                {
                    bool same = r.labels.size() == std::size(kLevelNames);
                    for (size_t l = 0; same && l < r.labels.size(); ++l)
                        same = r.labels[l] == kLevelNames[l];
                    ENGINE_CHECK(same, "%s: a quality level row's labels must be Low|Medium|High|Ultra|Cinematic", r.path);
                }
            }
            return v;
        }();
        return s_rows;
    }
}

namespace detail
{
    std::vector<std::string> SplitPipe(const char* s)
    {
        std::vector<std::string> out;
        if (!s) return out;
        const char* b = s;
        for (const char* p = s;; ++p)
        {
            if (*p == '|' || *p == '\0')
            {
                out.emplace_back(b, static_cast<size_t>(p - b));
                if (*p == '\0') break;
                b = p + 1;
            }
        }
        return out;
    }

    void CheckHooks(const char* path, Phase ph, bool applied, bool captured)
    {
        const bool ok = ph == Phase::None    ? (!applied && !captured)
                      : ph == Phase::Boot    ? (!applied && captured)
                      : ph == Phase::Applied ? (applied && !captured)
                                             : (applied && captured);
        ENGINE_CHECK(ok, "%s: phase %d needs %s", path, static_cast<int>(ph),
                     ph == Phase::None    ? "no hooks"
                   : ph == Phase::Boot    ? "a capture and no apply"
                   : ph == Phase::Applied ? "an apply and no capture"
                                          : "both an apply and a capture");
    }
}

const TokenCodec kAAModeCodec     { &AAModeToToken,     &AAModeFromToken,     nullptr  };
const TokenCodec kHDRModeCodec    { &HDRModeToToken,    &HDRModeFromToken,    nullptr  };
const TokenCodec kRenderPathCodec { &RenderPathToToken, &RenderPathFromToken, "Raster" };
const TokenCodec kLevelCodec      { &LevelToToken,      &LevelFromToken,      nullptr  };

// The phases in apply order (see Phase); the rows of a phase in .inl order.
static constexpr Phase kApplyOrder[] = { Phase::Present, Phase::Path, Phase::Renderer,
                                         Phase::Audio, Phase::PostProcess, Phase::Window };

std::span<const Row> PlayerRows()
{
    const std::vector<Row>& t = Table();
    return { t.data(), t.size() };
}

const Row* FindPlayerRow(std::string_view path)
{
    for (const Row& r : Table())
        if (path == r.path) return &r;
    return nullptr;
}

void ApplyPlayerRows(const Resource::EngineSettings& s, ApplyCtx& c)
{
    for (const Phase ph : kApplyOrder)
    {
        if (ph == Phase::Present) c.ps = RHI::PresentSettings{};   // every field is a row
        for (const Row& r : Table())
            if (r.spec.phase == ph) r.spec.apply(s, c);
        // The grouped owners take their values once, at the end of the phase.
        // SetPresentSettings LATCHES an HDR mode change to the next frame
        // boundary (ApplyTo runs mid-frame, out of a panel's button handler).
        // The size is a REQUEST serviced at the loop top, after the fullscreen
        // request the Window rows made: a same-Apply mode change is decided
        // before the resize is evaluated. 0 = leave the window alone.
        if (ph == Phase::Present)
            c.gfx.MainSurface().SetPresentSettings(c.ps);
        if (ph == Phase::Window && c.window && c.windowW > 0 && c.windowH > 0)
            c.window->RequestSize(c.windowW, c.windowH);
    }
}

bool ApplyPlayerRow(const Row& r, const Resource::EngineSettings& s, ApplyCtx& c)
{
    if (!r.spec.apply)
        return false;
    switch (r.spec.phase)
    {
    case Phase::Present:
        // The live present settings with this row's field changed: every other
        // present row keeps what is running (not what a struct holds).
        c.ps = c.gfx.MainSurface().GetPresentSettings();
        r.spec.apply(s, c);
        c.gfx.MainSurface().SetPresentSettings(c.ps);
        return true;
    case Phase::Window:
    {
        // A size row sets one axis; the other is the window's requested one.
        // The fullscreen row requests on its own and touches neither.
        const int w = c.window ? c.window->RequestedClientWidth()  : 0;
        const int h = c.window ? c.window->RequestedClientHeight() : 0;
        c.windowW = w;
        c.windowH = h;
        r.spec.apply(s, c);
        if (c.window && (c.windowW != w || c.windowH != h) && c.windowW > 0 && c.windowH > 0)
            c.window->RequestSize(c.windowW, c.windowH);
        return true;
    }
    default:
        r.spec.apply(s, c);
        return true;
    }
}

void CapturePlayerRows(Resource::EngineSettings& s, CaptureCtx& c)
{
    // Reading only, so the order does not matter; the present settings are
    // fetched once for the Present rows.
    c.ps = c.gfx.MainSurface().GetPresentSettings();
    for (const Row& r : Table())
        if (r.spec.capture) r.spec.capture(s, c);
}
}
