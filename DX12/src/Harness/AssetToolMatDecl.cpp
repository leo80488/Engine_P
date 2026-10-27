#include "Harness/AssetToolMatDecl.h"

#ifdef WITH_HARNESS

#include "Graphics/DxcCompiler.h"
#include "Graphics/MaterialShaderDecl.h"
#include "Graphics/PermutationKey.h"
#include "Graphics/ShaderLibrary.h"      // DynamicPermMask
#include "Graphics/ShaderReflection.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Harness
{
namespace
{
    using Json = nlohmann::json;
    namespace fs = std::filesystem;

    constexpr int kDone = 0, kRefused = 1, kUsage = 2;

    struct Job
    {
        std::string                 source;
        std::string                 key;         // the manifest key, or "<source>|perm0|<stage>"
        bool                        fromManifest = true;
        DxcCompiler::CompileOptions opts;
        // Filled by the worker.
        bool                        compiled = false;
        std::string                 error;
        std::vector<std::string>    mismatches;
    };

    struct Source
    {
        MaterialShaderDecl::Decl decl;
        std::string              text;           // the root's text
        std::string              includeDir;
    };

    bool ParseStage(const std::string& s, RHI::ShaderStage& out)
    {
        static const struct { const char* name; RHI::ShaderStage stage; } kStages[] = {
            { "MS", RHI::ShaderStage::MS }, { "AS", RHI::ShaderStage::AS }, { "VS", RHI::ShaderStage::VS },
            { "HS", RHI::ShaderStage::HS }, { "DS", RHI::ShaderStage::DS }, { "GS", RHI::ShaderStage::GS },
            { "PS", RHI::ShaderStage::PS }, { "CS", RHI::ShaderStage::CS }, { "LIB", RHI::ShaderStage::LIB },
        };
        for (const auto& st : kStages)
            if (s == st.name) { out = st.stage; return true; }
        return false;
    }

    const char* StageName(RHI::ShaderStage s)
    {
        switch (s)
        {
        case RHI::ShaderStage::VS: return "VS";
        case RHI::ShaderStage::PS: return "PS";
        case RHI::ShaderStage::CS: return "CS";
        case RHI::ShaderStage::MS: return "MS";
        case RHI::ShaderStage::AS: return "AS";
        default:                   return "other";
        }
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        return s;
    }

    std::string Slashes(std::string s)
    {
        std::replace(s.begin(), s.end(), '\\', '/');
        return s;
    }

    bool EndsWith(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        return s.size() >= n && Lower(s.substr(s.size() - n)) == suffix;
    }

    Json Bindings(const MaterialShaderDecl::Decl& d)
    {
        Json a = Json::array();
        for (const ShaderReflect::ResourceBinding& b : d.iface.bindings)
        {
            std::string file;
            uint32_t line = 0;
            for (const MaterialShaderDecl::Site& s : d.sites)
                if (s.name == b.name) { file = s.file; line = s.line; break; }
            a.push_back(Json{ { "name", b.name }, { "type", ShaderReflect::ToString(b.type) },
                              { "dim", ShaderReflect::ToString(b.dim) }, { "bindPoint", b.bindPoint },
                              { "space", b.space }, { "count", b.bindCount }, { "file", file }, { "line", line } });
        }
        return a;
    }

    Json CBuffers(const MaterialShaderDecl::Decl& d)
    {
        Json a = Json::array();
        for (const ShaderReflect::CBufferLayout& c : d.iface.cbuffers)
        {
            Json vars = Json::array();
            for (const ShaderReflect::CBufferVar& v : c.vars)
            {
                Json j{ { "name", v.name }, { "type", ShaderReflect::ToString(v.type) }, { "offset", v.offset },
                        { "size", v.size }, { "elements", v.elements } };
                if (v.hasDefault) j["default"] = v.defaultValue;
                if (!v.label.empty()) j["label"] = v.label;
                if (!v.engineRole.empty()) j["engine"] = v.engineRole;
                vars.push_back(std::move(j));
            }
            a.push_back(Json{ { "name", c.name }, { "bindPoint", c.bindPoint }, { "space", c.space },
                              { "size", c.sizeBytes }, { "vars", std::move(vars) } });
        }
        return a;
    }
}

int RunMatDeclCheck(Json& out)
{
    auto& cl = Sys::CommandLine::Get();
    const std::string manifestPath = cl.Get("-manifest", "tests/baselines/shader_variants.json");
    const std::string extra        = cl.Get("-asset");
    auto usage = [&out](const std::string& message)
    {
        out = Json{ { "ok", false }, { "error", "usage" }, { "message", message } };
        return kUsage;
    };

    std::vector<uint8_t> text;
    if (!Sys::ReadFileBytes(manifestPath, text) || text.empty())
        return usage("matdecl [-manifest=<variants.json>] [-asset=<file|dir>[;...]]: cannot read -manifest='" +
                     manifestPath + "'");
    Json doc = Json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("variants") || !doc["variants"].is_array())
        return usage("'" + manifestPath + "' is not a variant list ({\"variants\": [...]})");

    // ---- The variants whose interface a material's source declares ----
    std::vector<Job> jobs;
    std::map<std::string, Source> sources;   // by source path as the manifest spells it
    std::set<std::string> manifestSources;   // lower-case
    for (const Json& v : doc["variants"])
    {
        const std::string source     = Slashes(v.value("source", std::string()));
        const std::string includeDir = Slashes(v.value("includeDir", std::string("shaders/")));
        RHI::ShaderStage stage{};
        if (source.empty() || !ParseStage(v.value("stage", std::string()), stage)) continue;
        // The engine's: under its include root, or under shaders/ (a pass that
        // compiles its own shaders records another include root).
        if (!includeDir.empty() && Lower(source).rfind(Lower(includeDir), 0) == 0) continue;
        if (Lower(source).rfind("shaders/", 0) == 0) continue;
        if (v.value("failed", false)) continue;
        Job job;
        job.source                = source;
        job.key                   = v.value("key", std::string());
        job.opts.sourceName       = source;
        job.opts.entry            = v.value("entry", std::string("main"));
        job.opts.stage            = stage;
        job.opts.includeDir       = includeDir;
        job.opts.debug            = false;
        job.opts.hlsl2021         = v.value("hlsl2021", false);
        job.opts.enable16BitTypes = v.value("enable16BitTypes", false);
        for (const Json& d : v.value("defines", Json::array()))
            if (d.is_array() && d.size() == 2)
                job.opts.defines.push_back({ d[0].get<std::string>(), d[1].get<std::string>() });
        manifestSources.insert(Lower(source));
        sources[source].includeDir = includeDir;
        jobs.push_back(std::move(job));
    }

    // ---- -asset: sources the manifest does not list, at permutation 0 ----
    std::vector<std::string> extraFiles;
    for (size_t b = 0; b <= extra.size() && !extra.empty();)
    {
        size_t e = extra.find(';', b);
        if (e == std::string::npos) e = extra.size();
        const std::string item = Slashes(extra.substr(b, e - b));
        b = e + 1;
        if (item.empty()) continue;
        std::error_code ec;
        if (fs::is_directory(Sys::U8Path(item), ec))
        {
            for (fs::recursive_directory_iterator it(Sys::U8Path(item), fs::directory_options::skip_permission_denied, ec), end;
                 !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec)) continue;
                const std::string p = Slashes(Sys::PathToUtf8(it->path()));
                if (EndsWith(p, ".hlsl")) extraFiles.push_back(p);
            }
        }
        else extraFiles.push_back(item);
        if (e == extra.size()) break;
    }
    std::sort(extraFiles.begin(), extraFiles.end());
    extraFiles.erase(std::unique(extraFiles.begin(), extraFiles.end()), extraFiles.end());
    for (const std::string& f : extraFiles)
    {
        if (manifestSources.count(Lower(f))) continue;
        const MaterialShaderDecl::Decl d = MaterialShaderDecl::Parse(f, "shaders/");
        std::vector<RHI::ShaderStage> stages;
        if (EndsWith(f, ".vs.hlsl"))      stages = { RHI::ShaderStage::VS };
        else if (EndsWith(f, ".cs.hlsl")) stages = { RHI::ShaderStage::CS };
        else if (EndsWith(f, ".ps.hlsl")) stages = { RHI::ShaderStage::PS };
        else
        {
            stages = { RHI::ShaderStage::PS };
            if (d.hasVertexOffset) stages.push_back(RHI::ShaderStage::VS);
        }
        sources[f].includeDir = "shaders/";
        for (RHI::ShaderStage st : stages)
        {
            Job job;
            job.source          = f;
            job.key             = f + "|perm0|" + StageName(st);
            job.fromManifest    = false;
            job.opts.sourceName = f;
            job.opts.entry      = "main";
            job.opts.stage      = st;
            job.opts.includeDir = "shaders/";
            job.opts.debug      = false;
            // ShaderLibrary::MakeDefines(0) + LoadOrCompile's stage selector.
            for (const PermutationDefine& pd : kPermutationDefines) job.opts.defines.push_back({ pd.name, "0" });
            if (st == RHI::ShaderStage::VS) job.opts.defines.push_back({ "MATERIAL_STAGE_VS", "1" });
            if (st == RHI::ShaderStage::PS) job.opts.defines.push_back({ "MATERIAL_STAGE_PS", "1" });
            jobs.push_back(std::move(job));
        }
    }

    // ---- Parse every source once ----
    for (auto& [path, s] : sources)
    {
        s.decl = MaterialShaderDecl::Parse(path, s.includeDir);
        std::vector<uint8_t> bytes;
        if (Resource::AssetFS::Get().ReadFile(path, bytes))
            s.text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // ---- Compile, reflect, compare - in parallel ----
    // DxcCompiler, ShaderReflect and the annotation stamp are thread-safe; the decls are read-only.
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    unsigned nJobs = static_cast<unsigned>(std::clamp(
        cl.GetInt("-cook_jobs", static_cast<int>(std::min(hw > 1 ? hw - 1 : 1u, 8u))), 1, 64));
    nJobs = std::min<unsigned>(nJobs, static_cast<unsigned>(std::max<size_t>(jobs.size(), 1)));
    std::atomic<size_t> next{ 0 };
    auto worker = [&]()
    {
        for (size_t i = next.fetch_add(1); i < jobs.size(); i = next.fetch_add(1))
        {
            Job& job = jobs[i];
            const Source& src = sources.at(job.source);
            if (src.text.empty()) { job.error = "cannot read '" + job.source + "'"; continue; }
            const DxcCompiler::CompileResult cr = DxcCompiler::Compile(src.text.data(), src.text.size(), job.opts);
            if (!cr.ok)
            {
                job.error = cr.errorMsg.empty() ? std::string("DXC compile failed (no diagnostics)") : cr.errorMsg;
                continue;
            }
            ShaderReflect::Reflection refl;
            if (!ShaderReflect::Reflect(cr.dxil.data(), cr.dxil.size(), refl))
            {
                job.error = "no reflection";
                continue;
            }
            job.compiled = true;
            // What ShaderLibrary::GetShaderInternal stamps: the root source's annotations.
            MaterialShaderDecl::StampSourceAnnotations(src.text, refl, job.source);
            job.mismatches = MaterialShaderDecl::CrossCheck(src.decl, refl);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nJobs; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // ---- The report ----
    uint32_t compiled = 0, compileFailed = 0, extraCompileFailed = 0, mismatches = 0, mismatchedVariants = 0;
    uint32_t inexactSources = 0;
    Json capabilityDiffs = Json::array();
    Json rows = Json::array();
    for (auto& [path, s] : sources)
    {
        const MaterialShaderDecl::Decl& d = s.decl;
        if (!d.Exact()) ++inexactSources;
        const bool txtSurface = s.text.find("MATERIAL_HAS_SURFACE") != std::string::npos;
        const bool txtOffset  = s.text.find("MATERIAL_HAS_VERTEX_OFFSET") != std::string::npos;
        const bool txtModify  = s.text.find("MATERIAL_HAS_VERTEX_MODIFY") != std::string::npos;
        auto diff = [&](const char* which, bool txt, bool decl) {
            if (txt != decl)
                capabilityDiffs.push_back(Json{ { "source", path }, { "capability", which },
                                                { "textSearch", txt }, { "declared", decl } });
        };
        diff("MATERIAL_HAS_SURFACE", txtSurface, d.hasSurface);
        diff("MATERIAL_HAS_VERTEX_OFFSET", txtOffset, d.hasVertexOffset);
        diff("MATERIAL_HAS_VERTEX_MODIFY", txtModify, d.hasVertexModify);

        Json variants = Json::array();
        for (const Job& job : jobs)
        {
            if (job.source != path) continue;
            if (job.compiled) ++compiled;
            else if (job.fromManifest) ++compileFailed;
            else ++extraCompileFailed;
            mismatches += static_cast<uint32_t>(job.mismatches.size());
            if (!job.mismatches.empty()) ++mismatchedVariants;
            Json defs = Json::array();
            for (const DxcCompiler::Define& df : job.opts.defines)
                if (df.value != "0") defs.push_back(df.name);
            Json row{ { "key", job.key }, { "stage", StageName(job.opts.stage) }, { "manifest", job.fromManifest },
                      { "defines", std::move(defs) }, { "compiled", job.compiled }, { "mismatches", job.mismatches } };
            if (!job.error.empty()) row["error"] = job.error.substr(0, 2000);
            variants.push_back(std::move(row));
        }
        rows.push_back(Json{
            { "source", path }, { "read", d.read }, { "exact", d.Exact() }, { "files", d.files },
            { "inexact", d.inexact }, { "notes", d.notes },
            { "capabilities", { { "surface", d.hasSurface }, { "vertexOffset", d.hasVertexOffset },
                                { "vertexModify", d.hasVertexModify },
                                { "domain", d.domain == MaterialShaderDecl::Domain::Water ? "water" : "surface" } } },
            { "textSearch", { { "surface", txtSurface }, { "vertexOffset", txtOffset }, { "vertexModify", txtModify } } },
            { "permBits", d.permBits }, { "engineRolesExplicit", d.iface.engineRolesExplicit },
            { "engineIncludes", d.engineIncludes }, { "unresolvedIncludes", d.unresolvedIncludes },
            { "warnings", d.warnings },
            { "dynamicMask", ShaderLibrary::DynamicPermMask(d) },
            { "bindings", Bindings(d) }, { "cbuffers", CBuffers(d) }, { "variants", std::move(variants) } });
    }
    const bool ok = compileFailed == 0 && mismatches == 0;
    out = Json{ { "ok", ok }, { "manifest", manifestPath }, { "sources", std::move(rows) },
                { "summary", { { "sources", sources.size() }, { "variants", jobs.size() }, { "compiled", compiled },
                               { "compileFailed", compileFailed }, { "extraCompileFailed", extraCompileFailed },
                               { "mismatches", mismatches }, { "mismatchedVariants", mismatchedVariants },
                               { "inexactSources", inexactSources }, { "capabilityDiffs", capabilityDiffs } } },
                { "jobs", nJobs }, { "ms", ms } };
    LOG_INFO("[MATDECL] sources=%zu variants=%zu compiled=%u compileFailed=%u extraCompileFailed=%u mismatches=%u "
             "(in %u variant(s)) inexact=%u capabilityDiffs=%zu ms=%.0f",
             sources.size(), jobs.size(), compiled, compileFailed, extraCompileFailed, mismatches, mismatchedVariants,
             inexactSources, capabilityDiffs.size(), ms);
    return ok ? kDone : kRefused;
}
} // namespace Harness

#endif // WITH_HARNESS
