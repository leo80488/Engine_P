#include "Graphics/DxcCompiler.h"
#include "Graphics/PipelineStats.h"
#include "Diagnostics/Profiler.h"

#include "System/CommandLine.h"   // -hlsl2018 (UsesHlsl2021)
#include "System/Log.h"
#include "System/PathUtil.h"
#include "Resource/AssetFS.h"   // AssetFS-backed #include resolution for packed builds

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>

#include <dxcapi.h>
#include <d3d12shader.h>

#include <filesystem>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace
{
    // Lazy-init globals. IDxcCompiler3 / IDxcUtils are documented thread-safe and not
    // free to create, so the process shares one pair.
    std::once_flag       g_initFlag;
    ComPtr<IDxcUtils>    g_utils;
    ComPtr<IDxcCompiler3> g_compiler;
    bool                 g_initOk = false;

    void EnsureInit()
    {
        std::call_once(g_initFlag, []
        {
            HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils));
            if (FAILED(hr))
            {
                LOG_ERROR("DxcCompiler: DxcCreateInstance(DxcUtils) failed (hr=0x%08X)", hr);
                return;
            }
            hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_compiler));
            if (FAILED(hr))
            {
                LOG_ERROR("DxcCompiler: DxcCreateInstance(DxcCompiler) failed (hr=0x%08X)", hr);
                return;
            }
            g_initOk = true;
        });
    }

    // UTF-8 -> UTF-16: DXC takes wide-character argument arrays.
    std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                             static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                            static_cast<int>(s.size()), w.data(), wlen);
        return w;
    }

    // UTF-16 -> UTF-8 (DXC hands include paths back as wide strings).
    std::string Narrow(const wchar_t* w)
    {
        if (!w || !*w) return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string s(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
        return s;
    }

    std::string NormalizeKey(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        while (s.rfind("./", 0) == 0) s.erase(0, 2);
        return s;
    }

    // IDxcIncludeHandler that resolves #include "X.hlsli" through AssetFS (game.ipak
    // first, loose-disk fallback). The stock CreateDefaultIncludeHandler reads the real
    // filesystem only, so in a packed build every runtime compile that includes a
    // shared header (all DDGI compute shaders, for one) would fail. dirA / dirB hold
    // the source-file parent and the explicit -I dir so a bare basename still resolves.
    class AssetFSIncludeHandler : public IDxcIncludeHandler
    {
    public:
        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename,
                                             IDxcBlob** ppIncludeSource) override
        {
            if (ppIncludeSource) *ppIncludeSource = nullptr;
            if (!pFilename || !g_utils) return E_FAIL;

            const std::string norm = NormalizeKey(Narrow(pFilename));
            std::string base = norm;
            if (auto s = norm.find_last_of('/'); s != std::string::npos)
                base = norm.substr(s + 1);
            if (base == "bindless.hlsli")
                needs2021 = true;
            // The include as written: DXC joins it onto the including file's directory or an
            // -I root. Do not fall back to the bare file name for an include that names a
            // subdirectory ("Rtxdi/PT/Reservoir.hlsli"): RTXDI ships several names two or
            // three times (Reservoir, TemporalResampling, ... under DI / GI / PT / Utils), and
            // a basename hit would silently compile the wrong one. Refusing lets DXC try its
            // next search directory. A bare name, also behind "../" ("../TextFX.hlsli" from
            // asset/Shaders/text_fx/, which only shaders/ holds), keeps the basename fallback.
            std::string rel = norm;
            for (const std::string* dir : { &dirA, &dirB })
                if (!dir->empty() && rel.size() > dir->size() + 1 &&
                    rel.compare(0, dir->size(), *dir) == 0 && rel[dir->size()] == '/')
                { rel = rel.substr(dir->size() + 1); break; }
            while (rel.rfind("../", 0) == 0) rel.erase(0, 3);
            const bool bare = rel.find('/') == std::string::npos;

            std::string cands[5];
            int n = 0;
            cands[n++] = norm;                                   // as DXC joined it
            // An absolute path DXC built from a relative root: the same file
            // from its last "shaders/" on, which AssetFS and the pak know.
            if (auto s = norm.rfind("shaders/"); s != std::string::npos && s > 0)
                cands[n++] = norm.substr(s);
            if (bare)
            {
                if (!dirA.empty()) cands[n++] = dirA + "/" + base;   // source-relative
                if (!dirB.empty()) cands[n++] = dirB + "/" + base;   // -I relative
                cands[n++] = base;                                   // last resort
            }

            std::vector<std::uint8_t> bytes;
            for (int i = 0; i < n; ++i)
            {
                if (!Resource::AssetFS::Get().ReadFile(cands[i], bytes) || bytes.empty())
                    continue;
                ComPtr<IDxcBlobEncoding> blob;
                if (FAILED(g_utils->CreateBlob(bytes.data(),
                                               static_cast<UINT32>(bytes.size()),
                                               DXC_CP_UTF8, &blob)))
                    return E_FAIL;
                *ppIncludeSource = blob.Detach();
                return S_OK;
            }
            return E_FAIL;   // not found -> DXC emits the include error
        }

        // Stack-scoped per Compile() call; no real refcounting needed.
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
        {
            if (ppv && (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)))
            { *ppv = this; return S_OK; }
            if (ppv) *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef()  override { return 1; }
        ULONG STDMETHODCALLTYPE Release() override { return 1; }

        std::string dirA;   // source-file parent (normalised, no trailing slash)
        std::string dirB;   // explicit includeDir (normalised, no trailing slash)
        // DXC asked for a header written in HLSL 2021 (bindless.hlsli's templates), so
        // the TU cannot compile as 2018.
        bool needs2021 = false;
    };
} // namespace

namespace DxcCompiler
{

const char* ProfileForStage(RHI::ShaderStage stage)
{
    // SM 6.6: wave intrinsics and DescriptorHeap[] dynamic indexing, which the
    // bindless renderer relies on; 6.7 buys little here.
    switch (stage)
    {
    case RHI::ShaderStage::VS: return "vs_6_6";
    case RHI::ShaderStage::PS: return "ps_6_6";
    case RHI::ShaderStage::CS: return "cs_6_6";
    case RHI::ShaderStage::GS: return "gs_6_6";
    case RHI::ShaderStage::HS: return "hs_6_6";
    case RHI::ShaderStage::DS: return "ds_6_6";
    case RHI::ShaderStage::MS: return "ms_6_6";
    case RHI::ShaderStage::AS: return "as_6_6";
    // DXR libraries: lib_6_3 is the minimum, lib_6_5 adds raytracing features
    // (e.g. inline RT).
    case RHI::ShaderStage::LIB: return "lib_6_5";
    default:                   return "ps_6_6";
    }
}

const wchar_t* ProfileForStageW(RHI::ShaderStage stage)
{
    switch (stage)
    {
    case RHI::ShaderStage::VS: return L"vs_6_6";
    case RHI::ShaderStage::PS: return L"ps_6_6";
    case RHI::ShaderStage::CS: return L"cs_6_6";
    case RHI::ShaderStage::GS: return L"gs_6_6";
    case RHI::ShaderStage::HS: return L"hs_6_6";
    case RHI::ShaderStage::DS: return L"ds_6_6";
    case RHI::ShaderStage::MS: return L"ms_6_6";
    case RHI::ShaderStage::AS: return L"as_6_6";
    case RHI::ShaderStage::LIB: return L"lib_6_5";
    default:                   return L"ps_6_6";
    }
}

bool UsesHlsl2021(bool required)
{
    // -hlsl2018 is the A/B and fallback switch back to HLSL 2018; read once.
    static const bool s_legacy = Sys::CommandLine::Get().Has("-hlsl2018");
    return required || !s_legacy;
}

bool BindlessValidate()
{
    static const bool s_on = Sys::CommandLine::Get().Has("-bindless_validate");
    return s_on;
}

CompileResult Compile(const void* src, std::size_t srcSize, const CompileOptions& opts)
{
    CompileResult out;

    // Every compile is a cost a cached run does not pay: count it (frames.jsonl
    // shaderCompiles) and give it a profiler scope.
    PROF_SCOPE("DXC.Compile");
    PipelineStats::shaderCompiles.fetch_add(1, std::memory_order_relaxed);

#if !defined(WITH_RUNTIME_SHADER_COMPILE)
    // A configuration that compiles nothing (Retail, Engine.Cpp.props): the package's cooked shaders are
    // the only ones, and the sources it ships are interfaces with no function body. Still counted
    // above, so a harness run sees the miss.
    (void)src;
    (void)srcSize;
    (void)opts;
    out.errorMsg = "runtime shader compilation is off in this configuration: the package's cooked "
                   "shaders do not hold this variant";
    return out;
#else
    EnsureInit();
    if (!g_initOk || !g_compiler || !g_utils)
    {
        out.errorMsg = "DXC not initialised";
        return out;
    }
    if (!src || srcSize == 0)
    {
        out.errorMsg = "empty source";
        return out;
    }

    // Build the wstring storage first, then derive the pointer array: the vector of
    // wstrings reallocates as it grows, which invalidates every c_str() taken earlier.
    std::vector<std::wstring> argStorage;
    argStorage.reserve(32);

    // Source name (must come first to be the "main file path")
    if (!opts.sourceName.empty())
        argStorage.emplace_back(Widen(opts.sourceName));

    // Entry + profile. A DXIL library (lib_6_x) has no entry point - it exports every
    // [shader(...)] function - so an empty entry adds no -E.
    if (opts.stage != RHI::ShaderStage::LIB || !opts.entry.empty())
    {
        argStorage.emplace_back(L"-E");
        argStorage.emplace_back(Widen(opts.entry));
    }
    argStorage.emplace_back(L"-T");
    argStorage.emplace_back(ProfileForStageW(opts.stage));

    // HLSL 2021: short-circuiting ?:, templates, strict overload resolution;
    // -hlsl2018 takes the TUs that do not require it back to 2018 (UsesHlsl2021).
    argStorage.emplace_back(L"-HV");
    const std::size_t hvArg = argStorage.size();   // the -HV value, rewritten by the bindless.hlsli retry
    argStorage.emplace_back(UsesHlsl2021(opts.hlsl2021) ? L"2021" : L"2018");
    if (opts.enable16BitTypes)
        argStorage.emplace_back(L"-enable-16bit-types");

    // Add the source file's parent directory as an -I path: covers
    // shaders/foo.ps.hlsl including shaders/bar.hlsli.
    if (!opts.sourceName.empty())
    {
        std::filesystem::path p = Sys::U8Path(opts.sourceName);
        std::filesystem::path parent = p.parent_path();
        if (!parent.empty())
        {
            argStorage.emplace_back(L"-I");
            argStorage.emplace_back(parent.wstring());
        }
    }

    if (!opts.includeDir.empty())
    {
        argStorage.emplace_back(L"-I");
        argStorage.emplace_back(Widen(opts.includeDir));
    }

    // Defines: DXC accepts -D NAME=VALUE (one argument, '=' inline).
    for (const Define& d : opts.defines)
    {
        std::string s = d.name;
        if (!d.value.empty())
        {
            s.push_back('=');
            s.append(d.value);
        }
        argStorage.emplace_back(L"-D");
        argStorage.emplace_back(Widen(s));
    }
    // The bindless debug lane reaches every compile, the engine's and the materials'
    // alike.
    if (BindlessValidate())
    {
        argStorage.emplace_back(L"-D");
        argStorage.emplace_back(L"BINDLESS_VALIDATE=1");
    }

    if (opts.debug)
    {
        argStorage.emplace_back(L"-Zi");
        argStorage.emplace_back(L"-Od");
    }
    else
    {
        argStorage.emplace_back(L"-O3");
    }

    std::vector<const wchar_t*> args;
    args.reserve(argStorage.size());
    for (const std::wstring& s : argStorage)
        args.push_back(s.c_str());

    DxcBuffer srcBuf{};
    srcBuf.Ptr      = src;
    srcBuf.Size     = srcSize;
    srcBuf.Encoding = DXC_CP_UTF8;

    // Includes resolve through AssetFS so runtime compiles work in packed builds
    // (see AssetFSIncludeHandler).
    AssetFSIncludeHandler includeHandler;
    if (!opts.sourceName.empty())
        includeHandler.dirA = NormalizeKey(
            Sys::PathToUtf8(Sys::U8Path(opts.sourceName).parent_path()));
    if (!opts.includeDir.empty())
        includeHandler.dirB = NormalizeKey(opts.includeDir);

    ComPtr<IDxcResult> result;
    HRESULT hr = g_compiler->Compile(&srcBuf,
                             args.data(), static_cast<UINT32>(args.size()),
                             &includeHandler,
                             IID_PPV_ARGS(&result));

    // Under -hlsl2018 a TU that includes bindless.hlsli (HLSL 2021 templates) cannot
    // compile as 2018: when that compile fails, retry it as 2021, like a TU that
    // requires 2021 (CompileOptions::hlsl2021). Every other TU keeps the switch's 2018;
    // the include handler says which is which.
    if (!UsesHlsl2021(opts.hlsl2021) && includeHandler.needs2021)
    {
        HRESULT status = E_FAIL;
        if (SUCCEEDED(hr) && result)
            result->GetStatus(&status);
        if (FAILED(status))
        {
            argStorage[hvArg] = L"2021";
            args.clear();
            for (const std::wstring& s : argStorage)
                args.push_back(s.c_str());
            result.Reset();
            hr = g_compiler->Compile(&srcBuf,
                             args.data(), static_cast<UINT32>(args.size()),
                             &includeHandler,
                             IID_PPV_ARGS(&result));
        }
    }

    // Error / warning text: present even on success when there are warnings.
    if (result)
    {
        ComPtr<IDxcBlobUtf8> errBlob;
        if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errBlob), nullptr))
            && errBlob && errBlob->GetStringLength() > 0)
        {
            out.errorMsg.assign(errBlob->GetStringPointer(),
                                errBlob->GetStringLength());
        }
    }

    if (FAILED(hr) || !result)
    {
        if (out.errorMsg.empty())
            out.errorMsg = "IDxcCompiler3::Compile call failed";
        return out;
    }

    HRESULT compileHr = S_OK;
    result->GetStatus(&compileHr);
    if (FAILED(compileHr))
    {
        // out.errorMsg already holds DXC's diagnostics.
        return out;
    }

    ComPtr<IDxcBlob> objBlob;
    if (FAILED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&objBlob), nullptr))
        || !objBlob)
    {
        if (out.errorMsg.empty())
            out.errorMsg = "DXC compile produced no DXC_OUT_OBJECT";
        return out;
    }

    const uint8_t* p = static_cast<const uint8_t*>(objBlob->GetBufferPointer());
    out.dxil.assign(p, p + objBlob->GetBufferSize());
    out.ok = true;
    return out;
#endif
}

ID3D12ShaderReflection* CreateReflection(const void* container, std::size_t containerSize)
{
    EnsureInit();
    if (!g_initOk || !g_utils) return nullptr;
    if (!container || containerSize == 0) return nullptr;

    DxcBuffer buf{};
    buf.Ptr      = container;
    buf.Size     = containerSize;
    buf.Encoding = 0;   // binary container, no encoding hint

    ID3D12ShaderReflection* refl = nullptr;
    HRESULT hr = g_utils->CreateReflection(&buf, IID_PPV_ARGS(&refl));
    if (FAILED(hr))
    {
        LOG_ERROR("DxcCompiler::CreateReflection failed (hr=0x%08X)", hr);
        return nullptr;
    }
    return refl;
}

} // namespace DxcCompiler
