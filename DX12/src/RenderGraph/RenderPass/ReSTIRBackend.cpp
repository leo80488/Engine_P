#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PipelineLayoutBuilder.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace ReSTIR
{
namespace
{
bool EqualNoCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
} // namespace

const char* StageName(Stage stage)
{
    switch (stage)
    {
    case Stage::DI: return "DI";
    case Stage::GI: return "GI";
    case Stage::PT: return "PT";
    default:        return "?";
    }
}

const char* BackendName(Backend backend)
{
    switch (backend)
    {
    case Backend::Engine: return "engine";
    case Backend::Rtxdi:  return "rtxdi";
    case Backend::Hybrid: return "hybrid";
    case Backend::Area:   return "area";
    case Backend::Auto:   return "auto";
    default:              return "?";
    }
}

const char* CompareName(Compare compare)
{
    switch (compare)
    {
    case Compare::Off: return "off";
    case Compare::DI:  return "di";
    case Compare::GI:  return "gi";
    default:           return "?";
    }
}

bool IsLegal(Stage stage, Backend backend)
{
    switch (stage)
    {
    case Stage::DI: return backend == Backend::Engine || backend == Backend::Rtxdi || backend == Backend::Hybrid;
    case Stage::GI: return backend == Backend::Engine || backend == Backend::Rtxdi;
    case Stage::PT: return backend == Backend::Area || backend == Backend::Rtxdi || backend == Backend::Auto;
    default:        return false;
    }
}

Backend DefaultBackend(Stage stage)
{
    return stage == Stage::PT ? Backend::Area : Backend::Engine;
}

Backend DefaultRequest(Stage stage)
{
    return stage == Stage::PT ? Backend::Auto : Backend::Engine;
}

const char* RetirementDate(Stage stage, Backend backend)
{
    // The RTXDI DI / GI cores and DI's hybrid lost their A/B to the engine cores.
    // TODO(ReSTIR_Dual_Backend_Plan S8): selectable to 2026-11-15 (weekly A/B), CLI-only to 2026-12-13, then deleted.
    if ((stage == Stage::DI && (backend == Backend::Rtxdi || backend == Backend::Hybrid)) ||
        (stage == Stage::GI && backend == Backend::Rtxdi))
        return "2026-11-15";
    return nullptr;
}

int DateNumber(const char* iso)
{
    int year = 0, month = 0, day = 0;
    if (!iso || sscanf_s(iso, "%d-%d-%d", &year, &month, &day) != 3) return 0;
    return year * 10000 + month * 100 + day;
}

std::string OverdueRetirements(int today)
{
    std::string overdue;
    for (uint8_t s = 0; s < static_cast<uint8_t>(Stage::Count); ++s)
        for (uint8_t i = 0; i < static_cast<uint8_t>(Backend::Count); ++i)
        {
            const auto stage = static_cast<Stage>(s);
            const auto backend = static_cast<Backend>(i);
            if (!IsLegal(stage, backend)) continue;
            const char* date = RetirementDate(stage, backend);
            if (!date || DateNumber(date) == 0 || today <= DateNumber(date)) continue;
            overdue += std::string(overdue.empty() ? "" : ", ") + StageName(stage) + " " + BackendName(backend) +
                       " (" + date + ")";
        }
    return overdue;
}

bool ParseBackend(Stage stage, std::string_view token, Backend& out)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(Backend::Count); ++i)
    {
        const auto b = static_cast<Backend>(i);
        if (!IsLegal(stage, b)) continue;
        if (EqualNoCase(token, BackendName(b)) || (token.size() == 1 && token[0] == char('0' + i)))
        {
            out = b;
            return true;
        }
    }
    return false;
}

bool ParseCompare(std::string_view token, Compare& out)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(Compare::Count); ++i)
        if (EqualNoCase(token, CompareName(static_cast<Compare>(i))) ||
            (token.size() == 1 && token[0] == char('0' + i)))
        {
            out = static_cast<Compare>(i);
            return true;
        }
    return false;
}

const char* DenoiserName(Denoiser d)
{
    switch (d)
    {
    case Denoiser::Auto:   return "auto";
    case Denoiser::Relax:  return "relax";
    case Denoiser::Reblur: return "reblur";
    case Denoiser::None:   return "none";
    default:               return "?";
    }
}

bool ParseDenoiser(std::string_view token, Denoiser& out)
{
    for (uint8_t i = 0; i < static_cast<uint8_t>(Denoiser::Count); ++i)
        if (EqualNoCase(token, DenoiserName(static_cast<Denoiser>(i))) ||
            (token.size() == 1 && token[0] == char('0' + i)))
        {
            out = static_cast<Denoiser>(i);
            return true;
        }
    return false;
}

DIComposition Decompose(Backend named)
{
    switch (named)
    {
    case Backend::Rtxdi:  return {Backend::Rtxdi, Backend::Rtxdi};
    case Backend::Hybrid: return {Backend::Rtxdi, Backend::Engine};
    default:              return {Backend::Engine, Backend::Engine};
    }
}

const char* CompositionName(const DIComposition& c)
{
    const bool rtxdiInitial = c.initial == Backend::Rtxdi, rtxdiReuse = c.reuse == Backend::Rtxdi;
    if (rtxdiInitial && rtxdiReuse) return "rtxdi";
    if (rtxdiInitial) return "hybrid";
    if (rtxdiReuse) return "engine+rtxdi";
    return "engine";
}

bool ParseHalf(std::string_view token, Backend& out)
{
    if (EqualNoCase(token, "engine")) { out = Backend::Engine; return true; }
    if (EqualNoCase(token, "rtxdi")) { out = Backend::Rtxdi; return true; }
    return false;
}

std::string LegalBackends(Stage stage)
{
    std::string s;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Backend::Count); ++i)
    {
        const auto b = static_cast<Backend>(i);
        if (!IsLegal(stage, b)) continue;
        if (!s.empty()) s += '|';
        s += BackendName(b);
    }
    return s;
}

const char* RtxdiInertReason(Stage stage, bool native16BitShaderOps)
{
#if ENGINE_WITH_RTXDI
    // RTXDI's ReSTIR PT kernels are HLSL 2021 with 16-bit types.
    if (stage == Stage::PT && !native16BitShaderOps) return "no native 16-bit shader ops";
    // Whether each stage's RTXDI core is in this build, indexed by Stage.
    constexpr bool kCoreBuilt[static_cast<size_t>(Stage::Count)] = {true, true, true};   // DI, GI, PT
    const auto i = static_cast<size_t>(stage);
    if (i >= static_cast<size_t>(Stage::Count) || !kCoreBuilt[i]) return "rtxdi core not in this build";
    return nullptr;
#else
    (void)stage; (void)native16BitShaderOps;
    return "rtxdi sdk not in this build";
#endif
}

const char* RtxdiSdkVersion()
{
#if ENGINE_WITH_RTXDI
    return "f12037f";   // first 7 of tools/vendor_rtxdi.py's COMMIT: change both together
#else
    return "none";
#endif
}

bool CreateShellLayout(IGraphicsDevice& gfx, RHI::PipelineLayout& out, const char* name)
{
    namespace S = ShellSlots;
    RHI::PipelineLayoutBuilder b(name);
    b.RootCBV(S::CB, 0)
     .RootSRV(S::TLAS, 0).RootSRV(S::Instances, 1).RootSRV(S::Alpha, 2)
     .Table(S::Buffers,  { RHI::SrvRange(0, 16384, 1) })   // t0 space1: the bindless buffers
     .Table(S::Textures, { RHI::SrvRange(0, 16384, 2) })   // t0 space2: the bindless textures
     .Table(S::Result,   { RHI::UavRange(0) })
     // b1: a core's own constants (the RTXDI cores'); the engine cores'
     // shaders never declare it.
     .RootCBV(S::CoreCB, 1);
    RHI::SamplerDesc linearWrap{};   // MIN_MAG_MIP_LINEAR, WRAP, MaxLOD FLT_MAX
    linearWrap.maxAnisotropy  = 1;
    linearWrap.comparisonFunc = RHI::ComparisonFunc::NONE;   // D3D12's 0: no comparison
    b.Sampler(linearWrap, 1);
    b.Flags(RHI::LayoutFlags::HEAP_INDEXED_RESOURCES);
    return gfx.CreatePipelineLayout(b.Desc(), out);
}
} // namespace ReSTIR
