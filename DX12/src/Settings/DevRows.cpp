#include "Settings/SettingsRows.h"
#include "Settings/LegacyFlags.h"                  // a row's old spellings, checked when the table is built
#include "Settings/SettingsStore.h"                // DevApplyCtx::fromCommandLine: the row's source

#include "Renderer/Renderer.h"                      // the rows' setters and getters
#include "RenderGraph/RenderPass/ReSTIRBackend.h"   // the backend / denoiser / compare names
#include "RenderGraph/RenderPass/PTRtxdiCore.h"     // PTRtxdi::DirectLight names
#include "System/Check.h"
#include "System/CommandLine.h"                    // the rows' legacyRead hooks

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The developer rows (engine.json): Settings/Rows/Developer.inl expanded into a table.
namespace Settings
{
namespace
{
    using DS = Resource::EngineDevSettings;

    void CheckNames(const std::vector<DevRow>& v);

    const std::vector<DevRow>& Table()
    {
        static const std::vector<DevRow> s_rows = []
        {
            std::vector<DevRow> v;
#define SETTING(K, P, M, ...) v.push_back(detail::Make##K(P, &DS::M, DevRowSpec{ __VA_ARGS__ }));
#include "Settings/Rows/Developer.inl"
#undef SETTING
            for (const DevRow& r : v)
            {
                detail::CheckHooks(r.path, r.spec.phase, r.spec.apply != nullptr, r.spec.capture != nullptr);
                // Developer rows only reach the Renderer: Renderer, None, Applied.
                ENGINE_CHECK(r.spec.phase == Phase::Renderer || r.spec.phase == Phase::None ||
                             r.spec.phase == Phase::Applied,
                             "%s: a developer row is Renderer, None or Applied (phase %d)", r.path,
                             static_cast<int>(r.spec.phase));
                // engine.json's sections are not settings.json's: one path, one
                // row, whichever document it lives in.
                ENGINE_CHECK(FindPlayerRow(r.path) == nullptr, "%s is both a player and a developer row", r.path);
                // The row's old spellings (Settings/LegacyFlags.h): well-formed, and constants the
                // row takes.
                const std::string legacy = LegacySpecError(r);
                ENGINE_CHECK(legacy.empty(), "%s: .legacyFlag: %s", r.path, legacy.c_str());
            }
            CheckNames(v);
            return v;
        }();
        return s_rows;
    }

    // A String row's labels are its legal names (-set:, the pipe and Settings.setRuntime
    // take only those), so they must be the names its hook parses, which live in C++
    // tables: a backend that retires (ReSTIR::IsLegal) fails here instead of drifting.
    void CheckNames(const std::vector<DevRow>& v)
    {
        const auto same = [&v](const char* path, std::vector<std::string> legal)
        {
            for (const DevRow& r : v)
                if (std::string_view(r.path) == path)
                {
                    std::vector<std::string> have = r.labels;
                    std::sort(have.begin(), have.end());
                    std::sort(legal.begin(), legal.end());
                    ENGINE_CHECK(have == legal, "%s: its labels are not the names its parse takes", path);
                    return;
                }
            ENGINE_CHECK(false, "%s: no such developer row", path);
        };
        const auto backends = [](ReSTIR::Stage st)
        {
            std::vector<std::string> n;
            for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Backend::Count); ++i)
                if (ReSTIR::IsLegal(st, static_cast<ReSTIR::Backend>(i)))
                    n.push_back(ReSTIR::BackendName(static_cast<ReSTIR::Backend>(i)));
            return n;
        };
        std::vector<std::string> denoisers, compares, directLights;
        for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Denoiser::Count); ++i)
            denoisers.push_back(ReSTIR::DenoiserName(static_cast<ReSTIR::Denoiser>(i)));
        for (uint8_t i = 0; i < static_cast<uint8_t>(ReSTIR::Compare::Count); ++i)
            compares.push_back(ReSTIR::CompareName(static_cast<ReSTIR::Compare>(i)));
        for (uint8_t i = 0; i < static_cast<uint8_t>(PTRtxdi::DirectLight::Count); ++i)
            directLights.push_back(PTRtxdi::DirectLightName(static_cast<PTRtxdi::DirectLight>(i)));
        same("raytracing.restirBackendDI", backends(ReSTIR::Stage::DI));
        same("raytracing.restirBackendGI", backends(ReSTIR::Stage::GI));
        same("raytracing.restirBackendPT", backends(ReSTIR::Stage::PT));
        same("raytracing.restirDiDenoiser", denoisers);
        same("raytracing.restirGiDenoiser", denoisers);
        same("raytracing.restirCompare", compares);
        same("raytracing.ptDirectLight", directLights);
    }
}

std::span<const DevRow> DevRows()
{
    const std::vector<DevRow>& t = Table();
    return { t.data(), t.size() };
}

const DevRow* FindDevRow(std::string_view path)
{
    for (const DevRow& r : Table())
        if (path == r.path) return &r;
    return nullptr;
}

namespace
{
    // True when the value a hook is handed is the row's command-line one (an old flag
    // spelling or -set:): the store's source is CommandLine and its effective value is the
    // one being applied.
    bool FromCommandLine(const DevRow& r, const Resource::EngineDevSettings& s)
    {
        const DevSettingsStore& st = DevSettingsStore::Get();
        return st.Source(r) == Layer::CommandLine && SameValue(r, st.Effective(r), ValueOf(r, s));
    }
}

void ApplyDevRows(const Resource::EngineDevSettings& s, DevApplyCtx& c)
{
    // Rows apply in Developer.inl order.
    for (const DevRow& r : Table())
        if (r.spec.apply)
        {
            c.fromCommandLine = FromCommandLine(r, s);
            r.spec.apply(s, c);
        }
    c.fromCommandLine = false;
}

void CaptureDevRows(Resource::EngineDevSettings& s, DevCaptureCtx& c)
{
    for (const DevRow& r : Table())
        if (r.spec.capture) r.spec.capture(s, c);
}

bool ApplyDevRow(const DevRow& r, const Resource::EngineDevSettings& s, DevApplyCtx& c)
{
    if (!r.spec.apply) return false;
    c.fromCommandLine = FromCommandLine(r, s);
    r.spec.apply(s, c);
    c.fromCommandLine = false;
    return true;
}
}
