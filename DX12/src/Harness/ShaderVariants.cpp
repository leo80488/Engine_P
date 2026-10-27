#include "Harness/ShaderVariants.h"

#ifdef WITH_HARNESS

#include "Graphics/DxcCompiler.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderRecipes.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace Harness
{
namespace
{
    const char* StageName(RHI::ShaderStage s)
    {
        switch (s)
        {
        case RHI::ShaderStage::MS:  return "MS";
        case RHI::ShaderStage::AS:  return "AS";
        case RHI::ShaderStage::VS:  return "VS";
        case RHI::ShaderStage::HS:  return "HS";
        case RHI::ShaderStage::DS:  return "DS";
        case RHI::ShaderStage::GS:  return "GS";
        case RHI::ShaderStage::PS:  return "PS";
        case RHI::ShaderStage::CS:  return "CS";
        case RHI::ShaderStage::LIB: return "LIB";
        default:                    return "?";
        }
    }

    std::string Slashes(std::string s)
    {
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    }

    // Which DXC recorded these (the cook and the gate compile with the one
    // their own process loads): the module's path and size.
    nlohmann::json CompilerIdentity()
    {
        nlohmann::json j = nlohmann::json::object();
        wchar_t buf[MAX_PATH] = {};
        if (HMODULE m = ::GetModuleHandleW(L"dxcompiler.dll"))
        {
            const DWORD n = ::GetModuleFileNameW(m, buf, MAX_PATH);
            if (n > 0 && n < MAX_PATH)
            {
                const std::filesystem::path p(buf);
                j["path"] = Sys::PathToUtf8(p);
                std::error_code ec;
                const auto size = std::filesystem::file_size(p, ec);
                if (!ec) j["size"] = static_cast<std::uint64_t>(size);
            }
        }
        return j;
    }
}

void WriteShaderVariantsIfRequested()
{
    auto& cl = Sys::CommandLine::Get();
    if (!cl.Has("-shader_variants")) return;
    std::string file = cl.Get("-shader_variants");
    if (file.empty())
    {
        if (!Sys::HasArtifactDir())
        {
            LOG_WARNING("-shader_variants needs -artifacts=<dir> or a file name; nothing written");
            return;
        }
        file = Sys::ArtifactDir() + "/shader_variants.json";
    }

    const std::vector<ShaderRecipes::Recorded> recorded = ShaderRecipes::Snapshot();
    nlohmann::json variants = nlohmann::json::array();
    nlohmann::json skipped  = nlohmann::json::array();
    uint32_t failed = 0;
    for (const ShaderRecipes::Recorded& r : recorded)
    {
        if (!r.ok) ++failed;
        const char* why = nullptr;
        const std::string key = ShaderDiskCache::ReleaseCachePath(r.cachePath, &why);
        if (key.empty())
        {
            skipped.push_back({ { "recorded", Slashes(r.cachePath) }, { "why", why ? why : "?" } });
            continue;
        }
        nlohmann::json defines = nlohmann::json::array();
        for (const DxcCompiler::Define& d : r.opts.defines) defines.push_back({ d.name, d.value });
        nlohmann::json v = {
            { "key", Slashes(key) },
            { "source", Slashes(r.opts.sourceName) },
            { "entry", r.opts.entry },
            { "stage", StageName(r.opts.stage) },
            { "defines", std::move(defines) },
            { "includeDir", Slashes(r.opts.includeDir) },
            { "hlsl2021", r.opts.hlsl2021 },
            { "enable16BitTypes", r.opts.enable16BitTypes },
            { "ok", r.ok },
            { "tags", r.tags },
        };
        if (key != r.cachePath) v["recorded"] = Slashes(r.cachePath);   // a Debug build's name
        variants.push_back(std::move(v));
    }

    nlohmann::json doc = {
        { "schema", 1 },
#ifdef _DEBUG
        { "config", "Debug" },
#else
        { "config", "Release" },
#endif
        { "compiler", CompilerIdentity() },
        { "variants", std::move(variants) },
        { "skipped", std::move(skipped) },
    };
    const std::string text = doc.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!Sys::WriteFileBytes(file, text.data(), text.size()))
    {
        LOG_ERROR("-shader_variants: cannot write '%s'", file.c_str());
        return;
    }
    LOG_INFO("[SHADER_VARIANTS] recipes=%zu release=%zu skipped=%zu failed=%u file=%s", recorded.size(),
             doc["variants"].size(), doc["skipped"].size(), failed, file.c_str());
}
} // namespace Harness

#endif // WITH_HARNESS
