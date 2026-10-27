#include "Graphics/ShaderDiskCache.h"
#include "Graphics/LayoutCheck.h"
#include "Graphics/ShaderReflection.h"
#include "Graphics/ShaderRecipes.h"

#include "Resource/AssetHeader.h"
#include "Resource/AssetFS.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Include-list memo behind MaxIncludeTime (see ForgetIncludeLists in the header): a file's
    // list is kept with the write time it was read at and re-read when that changes. The walk
    // still reads every file's write time, so the staleness answer equals an uncached walk's.
    struct IncludeList
    {
        uint64_t                 writeTime = 0;
        std::vector<std::string> includes;
    };
    std::mutex                                   g_includeListMutex;
    std::unordered_map<std::string, IncludeList> g_includeLists;   // "shaderDir|path"

    std::vector<std::string> ExtractIncludePaths(const std::string& hlslPath,
                                                 const std::string& shaderDir);

    // The include list of @p hlslPath as of @p writeTime (0 = unknown: a
    // packed build reads no source, and nothing unknown is remembered).
    std::vector<std::string> IncludeListAt(const std::string& hlslPath,
                                           const std::string& shaderDir,
                                           uint64_t           writeTime)
    {
        if (writeTime == 0) return ExtractIncludePaths(hlslPath, shaderDir);
        const std::string key = shaderDir + '|' + hlslPath;
        {
            std::lock_guard<std::mutex> lock(g_includeListMutex);
            const auto it = g_includeLists.find(key);
            if (it != g_includeLists.end() && it->second.writeTime == writeTime)
                return it->second.includes;
        }
        std::vector<std::string> includes = ExtractIncludePaths(hlslPath, shaderDir);
        std::lock_guard<std::mutex> lock(g_includeListMutex);
        g_includeLists[key] = IncludeList{ writeTime, includes };
        return includes;
    }

    // Recursive worker behind ShaderDiskCache::MaxIncludeTime. Kept private so
    // callers cannot forget to hand it a fresh `visited` set.
    uint64_t MaxIncludeTimeRec(const std::string& hlslPath,
                               const std::string& shaderDir,
                               std::unordered_set<std::string>& visited);

    // Scans @p hlslPath for `#include "foo.hlsli"` / `#include <Rtxdi/foo.hlsli>` and returns
    // the resolved full paths. Line-based: tolerates whitespace, skips a line whose `//` comes
    // before the `#`. Angle includes (the vendored RTXDI's style) resolve as DXC resolves them,
    // against shaderDir only; skipping them would keep a stale blob after an RTXDI header
    // changes. Includes behind a define are scanned unconditionally: at worst an extra
    // recompile, never a missed one. A name nothing resolves (<stdint.h> in a C++ branch) is
    // skipped.
    std::vector<std::string> ExtractIncludePaths(const std::string& hlslPath,
                                                 const std::string& shaderDir)
    {
        std::vector<std::string> out;
        std::ifstream f(Sys::U8Path(hlslPath));
        if (!f) return out;

        const std::filesystem::path sourceDir = Sys::U8Path(hlslPath).parent_path();

        std::string line;
        while (std::getline(f, line))
        {
            // Strip the trailing `\r` of CRLF files.
            if (!line.empty() && line.back() == '\r') line.pop_back();

            // Ignore lines where `//` comes before `#include` (line comment).
            const std::size_t commentPos = line.find("//");
            const std::size_t hashPos    = line.find('#');
            if (hashPos == std::string::npos) continue;
            if (commentPos != std::string::npos && commentPos < hashPos) continue;

            // Match `#[ws]include[ws]"NAME"` or `<NAME>` starting at hashPos.
            std::size_t i = hashPos + 1;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (line.compare(i, 7, "include") != 0) continue;
            i += 7;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
            if (i >= line.size() || (line[i] != '"' && line[i] != '<')) continue;
            const bool angle = line[i] == '<';
            const char close = angle ? '>' : '"';
            ++i;
            const std::size_t start = i;
            while (i < line.size() && line[i] != close) ++i;
            if (i >= line.size()) continue;
            const std::string name = line.substr(start, i - start);
            if (name.empty()) continue;

            // Resolve: a quoted include first relative to the including file's
            // directory (matches D3D_COMPILE_STANDARD_FILE_INCLUDE), then
            // shaderDir (for root-level shared headers); an angle include
            // against shaderDir only.
            std::error_code ec;
            std::filesystem::path p = angle ? Sys::U8Path(shaderDir) / Sys::U8Path(name)
                                            : sourceDir / Sys::U8Path(name);
            if (!angle && !std::filesystem::exists(p, ec))
                p = Sys::U8Path(shaderDir) / Sys::U8Path(name);
            if (!std::filesystem::exists(p, ec)) continue;

            out.push_back(Sys::PathToUtf8(p));
        }
        return out;
    }

    uint64_t MaxIncludeTimeRec(const std::string& hlslPath,
                               const std::string& shaderDir,
                               std::unordered_set<std::string>& visited)
    {
        if (!visited.insert(hlslPath).second) return 0;

        uint64_t maxTime = ShaderDiskCache::FileWriteTime(hlslPath);
        for (const auto& inc : IncludeListAt(hlslPath, shaderDir, maxTime))
        {
            const uint64_t t = MaxIncludeTimeRec(inc, shaderDir, visited);
            if (t > maxTime) maxTime = t;
        }
        return maxTime;
    }
}

namespace ShaderDiskCache
{

uint64_t FileWriteTime(const std::string& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(Sys::Utf8ToWide(path).c_str(), GetFileExInfoStandard, &attr))
        return 0;
    ULARGE_INTEGER t;
    t.LowPart  = attr.ftLastWriteTime.dwLowDateTime;
    t.HighPart = attr.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

uint64_t MaxIncludeTime(const std::string& hlslPath, const std::string& shaderDir)
{
    std::unordered_set<std::string> visited;
    return MaxIncludeTimeRec(hlslPath, shaderDir, visited);
}

std::vector<std::string> IncludesOf(const std::string& hlslPath, const std::string& shaderDir)
{
    return ExtractIncludePaths(hlslPath, shaderDir);
}

void ForgetIncludeLists()
{
    std::lock_guard<std::mutex> lock(g_includeListMutex);
    g_includeLists.clear();
}

uint32_t HashDefines(const std::vector<DxcCompiler::Define>& defines)
{
    // Order-independent: each define is hashed on its own and the results are XOR-folded, so
    // reordering a call site's define list does not invalidate the cache. A separator byte
    // (0x1F, which no name or value holds) between name and value keeps {"AB",""} and
    // {"A","B"} apart.
    // Seed: see the header (an empty list must not produce 0).
    constexpr uint32_t kSeed = 0x5ADEC0DEu;
    uint32_t acc = kSeed;
    for (const auto& d : defines)
    {
        uint32_t h = 2166136261u;
        for (unsigned char b : d.name)  { h ^= b; h *= 16777619u; }
        h ^= 0x1Fu; h *= 16777619u;            // separator
        for (unsigned char b : d.value) { h ^= b; h *= 16777619u; }
        acc ^= h;
    }
    // Fold once more so an empty list is not literally the seed constant and a
    // pair of identical defines does not cancel back to it.
    acc ^= static_cast<uint32_t>(defines.size()) * 2654435761u;
    return acc ? acc : kSeed;
}

// Engine shaders land directly under cacheDir (shaderDir is stripped from the parent);
// project-relative custom shaders (asset/Shaders/...) keep their subdirectories; absolute
// drag-dropped sources flatten to the file name, which is why the full source path is
// hashed into the suffix.
std::string CachePath(const std::string& cacheDir,
                      const std::string& shaderDir,
                      const std::string& hlslPath,
                      uint32_t           permBits,
                      const std::string& entryPoint,
                      RHI::ShaderStage   stage,
                      bool               debugFlags)
{
    std::filesystem::path src = Sys::U8Path(hlslPath);
    const std::string stem = Sys::PathToUtf8(src.stem());        // "GBuffer.ps"

    // Compile-flags tag: Debug and Release share shaders/shader_cache_dxil/, but `-Zi -Od` and
    // `-O3` are different binaries, and without the tag one config would silently load the
    // other's DXIL. The HLSL version is in it for the same reason (2021 vs -hlsl2018, which
    // uses the plain D / O3 tags).
    const bool        hv2021    = DxcCompiler::UsesHlsl2021(false);
    std::string flagsTag = debugFlags ? (hv2021 ? "D21" : "D") : (hv2021 ? "O3H21" : "O3");
    // A -bindless_validate run compiles every shader with BINDLESS_VALIDATE=1: a different
    // binary, so a different file.
    if (DxcCompiler::BindlessValidate())
        flagsTag += "BV";

    // Hash of the full source path: absolute paths (Resource Panel drag-and-drop, ShaderLab)
    // flatten to the bare file name below, so two same-named custom shaders in different
    // folders would otherwise share one .ishdr and swap bytecode and reflection layouts.
    // Normalisation is lexical only (separators + case), never made absolute: engine shaders
    // hash machine-independent relative paths like "shaders/GBuffer.ps.hlsl", and the baked
    // cache ships in the game pak.
    uint32_t pathHash = 2166136261u;
    {
        std::string norm = Sys::PathToUtf8(src.lexically_normal());
        for (char& c : norm)
        {
            // ASCII-only folding on purpose: paths are UTF-8, and locale-aware
            // tolower would mangle continuation bytes non-deterministically.
            if (c == '\\')                c = '/';
            else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        for (unsigned char b : norm) { pathHash ^= b; pathHash *= 16777619u; }
    }

    // Stage is in the key (S%u): a unified material file compiles for both VS and
    // PS at the same (entry, perm); without the stage the two .ishdr files collide
    // and CreateGraphicsPipelineState gets a PS blob in the VS slot (E_INVALIDARG).
    char suffix[256];
    snprintf(suffix, sizeof(suffix), "_%s_S%u_P%08X_%s_H%08X.ishdr",
             entryPoint.c_str(), static_cast<unsigned>(stage), permBits,
             flagsTag.c_str(), pathHash);

    std::filesystem::path root = Sys::U8Path(cacheDir);
    std::filesystem::path parent;
    if (!src.is_absolute())
    {
        parent = src.parent_path();

        // Strip the shaderDir prefix: shaders/GBuffer.ps.hlsl -> <cacheDir>/GBuffer..., not
        // <cacheDir>/shaders/GBuffer...
        if (!shaderDir.empty())
        {
            std::string sdir = shaderDir;
            while (!sdir.empty() && (sdir.back() == '/' || sdir.back() == '\\'))
                sdir.pop_back();
            const std::string pStr = Sys::PathToUtf8(parent);
            if (!sdir.empty())
            {
                if (pStr == sdir)
                {
                    parent.clear();
                }
                else if (pStr.size() > sdir.size() &&
                         (pStr[sdir.size()] == '/' || pStr[sdir.size()] == '\\') &&
                         pStr.compare(0, sdir.size(), sdir) == 0)
                {
                    parent = pStr.substr(sdir.size() + 1);
                }
            }
        }
    }

    std::filesystem::path out = root / parent / (stem + suffix);
    return Sys::PathToUtf8(out);
}

std::string ReleaseCachePath(const std::string& cachePath, const char** why)
{
    auto refuse = [why](const char* reason) { if (why) *why = reason; return std::string(); };
    // CachePath's suffix: ..._P%08X_<flags>_H%08X.ishdr - the tag sits between
    // the perm field and the path hash, so it is found from the END (an entry
    // point or a stem may hold underscores of its own).
    constexpr const char kExt[] = ".ishdr";
    constexpr size_t     kTail  = 2 + 8 + sizeof(kExt) - 1;           // "_H" + 8 hex + ".ishdr"
    if (cachePath.size() < kTail + 2 || cachePath.compare(cachePath.size() - (sizeof(kExt) - 1), std::string::npos, kExt) != 0)
        return refuse("not an .ishdr name");
    const size_t hashAt = cachePath.size() - kTail;                   // the '_' before "H%08X"
    if (cachePath[hashAt] != '_' || cachePath[hashAt + 1] != 'H')
        return refuse("no path hash field");
    const size_t tagAt = cachePath.rfind('_', hashAt - 1);
    if (tagAt == std::string::npos || tagAt + 1 >= hashAt)
        return refuse("no flag tag");
    const std::string tag = cachePath.substr(tagAt + 1, hashAt - tagAt - 1);
    if (tag.find("BV") != std::string::npos) return refuse("bindless_validate");
    std::string release;
    if      (tag == "O3H21") release = tag;
    else if (tag == "D21")   release = "O3H21";
    else if (tag == "O3" || tag == "D") return refuse("hlsl2018");
    else return refuse("unknown flag tag");
    return cachePath.substr(0, tagAt + 1) + release + cachePath.substr(hashAt);
}

bool LoadCached(const std::string& cachePath, std::vector<uint8_t>& outDxil)
{
    // Through AssetFS, so shader_cache_dxil/ reads come from the pak when mounted (game build)
    // or from the disk (editor / dev).
    std::vector<uint8_t> blob;
    if (!Resource::AssetFS::Get().ReadFile(cachePath, blob) || blob.empty())
        return false;
    // A cache entry of another version is recompiled, not an error to report.
    if (Resource::CheckHeader(blob.data(), blob.size(), Resource::MAGIC_SHADER).status != Resource::HeaderStatus::Ok)
        return false;

    const uint8_t* dxil     = Resource::GetPayload(blob.data());
    const size_t   dxilSize = Resource::GetHeader(blob.data())->dataSize;
    if (!dxil || dxilSize == 0) return false;

    outDxil.assign(dxil, dxil + dxilSize);
    return true;
}

std::vector<uint8_t> BuildContainer(const std::string& entryPoint,
                                    RHI::ShaderStage   stage,
                                    const void* dxil, size_t dxilSize)
{
    Resource::ShaderMetadata meta{};
    meta.bytecodeSize = static_cast<uint32_t>(dxilSize);
    meta.stage        = static_cast<uint8_t>(stage);
    meta.shaderModel  = static_cast<uint8_t>(RHI::ShaderModel::SM_6_6);
    strncpy_s(meta.entryPoint, sizeof(meta.entryPoint),
              entryPoint.c_str(), entryPoint.size());

    Resource::AssetHeader hdr{};
    hdr.magic        = Resource::MAGIC_SHADER;
    hdr.version      = Resource::CurrentVersion(Resource::MAGIC_SHADER);
    hdr.resourceType = 0;
    hdr.metadataSize = sizeof(Resource::ShaderMetadata);
    hdr.dataSize     = static_cast<uint32_t>(dxilSize);
    hdr.flags        = 0;
    hdr.reserved     = 0;

    const size_t total =
        sizeof(Resource::AssetHeader) + sizeof(Resource::ShaderMetadata) + dxilSize;
    std::vector<uint8_t> blob(total);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr,  sizeof(Resource::AssetHeader));    dst += sizeof(Resource::AssetHeader);
    std::memcpy(dst, &meta, sizeof(Resource::ShaderMetadata)); dst += sizeof(Resource::ShaderMetadata);
    if (dxilSize) std::memcpy(dst, dxil, dxilSize);
    return blob;
}

bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& bytes)
{
    // Two render workers compiling one recipe, or two engines sharing the
    // cache directory (parallel packs, several worktrees), never see a torn
    // .ishdr. The one implementation lives in PathUtil.
    return Sys::WriteFileBytesAtomic(path, bytes.data(), bytes.size());
}

bool StoreCached(const std::string& cachePath,
                 const std::string& entryPoint,
                 RHI::ShaderStage   stage,
                 const std::vector<uint8_t>& dxil)
{
    if (dxil.empty()) return false;
    return WriteFileAtomic(cachePath, BuildContainer(entryPoint, stage, dxil.data(), dxil.size()));
}

std::vector<uint8_t> LoadOrCompile(const std::string& hlslPath,
                                   const std::string& entryPoint,
                                   RHI::ShaderStage   stage,
                                   const std::vector<DxcCompiler::Define>& defines,
                                   const char*        logTag)
{
    return LoadOrCompile(hlslPath, entryPoint, stage, defines, logTag, CompileFlags{});
}

std::vector<uint8_t> LoadOrCompile(const std::string& hlslPath,
                                   const std::string& entryPoint,
                                   RHI::ShaderStage   stage,
                                   const std::vector<DxcCompiler::Define>& defines,
                                   const char*        logTag,
                                   const CompileFlags& flags)
{
    // -O3 in every build configuration, deliberately. Do not switch Debug to `-Zi -Od` here: it
    // would silently change what the GPU runs for these passes, and HybridReflIntersectHW.cs.hlsl
    // (70 KB of inline RayQuery with full material evaluation at the hit) must not reach the
    // compiler unoptimized. Caching is this function's job; changing codegen is not.
    constexpr bool kDebugFlags = false;

    // The language flags join the key through the define hash (a 2021 / 16-bit TU is another
    // binary). With neither flag set the key is the plain define hash, so existing .ishdr files
    // stay valid.
    std::vector<DxcCompiler::Define> keyed = defines;
    if (flags.hlsl2021) keyed.push_back({ "__ENGINE_HV", "2021" });
    if (flags.enable16BitTypes) keyed.push_back({ "__ENGINE_16BIT", "1" });
    const uint32_t    permBits  = HashDefines(keyed);
    const std::string cachePath =
        CachePath(kCacheDir, kShaderDir, hlslPath, permBits, entryPoint, stage,
                  kDebugFlags);

    // A cached blob is good unless the source (or a transitive #include) is strictly newer.
    // `hlslTime == 0` means unknown (a packed build whose .hlsl lives only in game.ipak): keep
    // the cache, never treat unknown as stale, or every packed launch recompiles from a source
    // it cannot read.
    const uint64_t hlslTime  = MaxIncludeTime(hlslPath, kShaderDir);
    const uint64_t cacheTime = FileWriteTime(cachePath);
    const bool     stale     = (hlslTime != 0 && hlslTime > cacheTime);

    DxcCompiler::CompileOptions opts;
    opts.sourceName = hlslPath;
    opts.entry      = entryPoint;
    opts.stage      = stage;
    opts.defines    = defines;
    opts.debug      = kDebugFlags;
    opts.hlsl2021   = flags.hlsl2021;
    opts.enable16BitTypes = flags.enable16BitTypes;

    // Every outcome is recorded, a failure included: a hot reload then knows
    // this pass holds the recipe, and that it did not compile before
    // (ShaderRecipes.h). The pass holds the bytes itself, so it is its tag,
    // not a library, that has to load it again.
    auto note = [&](bool ok)
    {
        ShaderRecipes::Load l;
        l.opts      = opts;
        l.cachePath = cachePath;
        l.scanDir   = kShaderDir;
        l.tag       = logTag;
        l.ok        = ok;
        ShaderRecipes::Note(l);
    };

    // Layout contract check, the same one ShaderLibrary runs: the private-root-signature
    // passes' shaders (RT, DDGI, RTXDI) include light_cb.hlsli / material.hlsli like everyone
    // else. One reflection per load (a load is not per frame).
    auto checkLayout = [&](const std::vector<uint8_t>& dxil)
    {
        ShaderReflect::Reflection r;
        if (ShaderReflect::Reflect(dxil.data(), dxil.size(), r))
            Layout::CheckRegistered((hlslPath + ":" + entryPoint).c_str(), r);
    };

    if (!stale)
    {
        std::vector<uint8_t> dxil;
        if (LoadCached(cachePath, dxil))
        {
            note(true);
            // Every load names its file, like ShaderLibrary's "loaded cached" line and the compile
            // path's: a run's log is then the record of which shaders it used (verify.py --hot reads it
            // to find a pack configuration that loads a shader the default scene does not).
            LOG_INFO("%s: loaded cached '%s' (%s, key=0x%08X)",
                     logTag, hlslPath.c_str(), entryPoint.c_str(), permBits);
            checkLayout(dxil);
            return dxil;
        }
    }

    std::vector<uint8_t> src;
    if (!Resource::AssetFS::Get().ReadFile(hlslPath, src) || src.empty())
    {
        LOG_ERROR("%s: cannot read shader %s", logTag, hlslPath.c_str());
        note(false);
        return {};
    }

    auto res = DxcCompiler::Compile(src.data(), src.size(), opts);
    if (!res.ok)
    {
        LOG_ERROR("%s: compile %s failed: %s", logTag, hlslPath.c_str(),
                  res.errorMsg.c_str());
        note(false);
        return {};
    }
    note(true);
    if (!res.errorMsg.empty())
        LOG_INFO("%s: %s warnings: %s", logTag, hlslPath.c_str(), res.errorMsg.c_str());

    if (StoreCached(cachePath, entryPoint, stage, res.dxil))
        LOG_INFO("%s: compiled + cached '%s' (%s, key=0x%08X, %zu bytes)",
                 logTag, hlslPath.c_str(), entryPoint.c_str(), permBits, res.dxil.size());
    else
        LOG_WARNING("%s: compiled '%s' but failed to write cache '%s'",
                    logTag, hlslPath.c_str(), cachePath.c_str());

    checkLayout(res.dxil);
    return res.dxil;
}

} // namespace ShaderDiskCache
