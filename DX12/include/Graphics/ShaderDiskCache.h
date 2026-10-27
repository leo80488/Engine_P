#pragma once

// Shader disk cache: the `.ishdr` cache shared by ShaderLibrary and the render passes that
// compile their own shaders (RTReflection, RTAO, RTShadow, SSGI, DDGI, ...) from arbitrary
// (path, entry, stage, defines) recipes rather than ShaderLibrary's ShaderID +
// PermutationKey registry. Without it those passes run ~25 DXC compiles per boot
// (HybridReflIntersectHW.cs.hlsl alone ~11 s).
// It is the one definition of the cache file-name scheme and of the staleness rule. Do not
// copy either: a second copy is how a cache looks fresh while handing back bytecode
// compiled against a different struct layout.
// Packed builds: sources are read through AssetFS (game.ipak first, then loose disk) but
// timestamps come from the real filesystem, so MaxIncludeTime returns 0 for a shader that
// only exists in the pak. Read 0 as "unknown" and keep the cached blob, never as "source
// older than the cache" (LoadOrCompile does this).

#include "Graphics/DxcCompiler.h"   // DxcCompiler::Define
#include "Graphics/RenderTypes.h"   // RHI::ShaderStage

#include <cstdint>
#include <string>
#include <vector>

namespace ShaderDiskCache
{
    // Both are relative to the process working directory. Keep them in sync with
    // ShaderLibrary::m_cacheDir and the shaderDir passed to ShaderLibrary::Init.
    inline constexpr const char* kCacheDir  = "shaders/shader_cache_dxil/";
    inline constexpr const char* kShaderDir = "shaders/";

    // Last-write time as FILETIME ticks (100 ns since 1601-01-01), 0 if the
    // file does not exist.
    uint64_t FileWriteTime(const std::string& path);

    // Max last-write time across @p hlslPath and everything it transitively `#include`s.
    // Cycle-safe. 0 means "could not tell" (a missing file, or a packed build whose source is
    // not on disk; see the file header).
    uint64_t MaxIncludeTime(const std::string& hlslPath, const std::string& shaderDir);

    // The `#include "..."` / `#include <...>` files @p hlslPath names directly, resolved as
    // MaxIncludeTime resolves them (quoted: the including file's directory, then @p shaderDir;
    // angle: @p shaderDir only, the vendored RTXDI's style). The hot-reload pre-flight walks the
    // closure with this, so "which shaders does this edit reach" and "is this .ishdr stale"
    // share one scanner (ShaderRecipes.h).
    std::vector<std::string> IncludesOf(const std::string& hlslPath, const std::string& shaderDir);

    // MaxIncludeTime remembers each file's include list for the session, keyed by the file's
    // write time (a boot walks the same trees ~200 times). Which file an include resolves to
    // depends on which files exist, so a shader file appearing or disappearing must forget
    // them: the shader hot reload calls this for every change it is told of.
    void ForgetIncludeLists();

    // Cache key for a define list, filling the 32-bit slot of the cache file name that
    // ShaderLibrary fills with PermutationKey::bits. The defines must be in the key: DDGIPass
    // compiles DDGIRelight.cs.hlsl and DDGIBorderUpdate.cs.hlsl twice each, differing only in
    // defines, and the variants would otherwise overwrite one .ishdr.
    // The seed is not the usual FNV basis: an empty define list must not produce 0,
    // ShaderLibrary's "no permutation" value, so a file reachable from both systems can never
    // collide on a key while compiled differently.
    uint32_t HashDefines(const std::vector<DxcCompiler::Define>& defines);

    // Maps an .hlsl source path + (@p permBits, @p entryPoint, @p stage) to its .ishdr path:
    // the single definition of the naming scheme (ShaderLibrary::CachePath forwards here).
    // @p debugFlags must equal the compile's DxcCompiler::CompileOptions::debug. It selects the
    // D* / O3* flag tag in the name, the only thing keeping `-Zi -Od` bytecode from a run that
    // wanted `-O3` (Debug and Release share one working directory, so one cache directory).
    // Do not derive it from `#ifdef _DEBUG` here: LoadOrCompile's callers compile -O3 in every
    // configuration.
    std::string CachePath(const std::string& cacheDir,
                          const std::string& shaderDir,
                          const std::string& hlslPath,
                          uint32_t           permBits,
                          const std::string& entryPoint,
                          RHI::ShaderStage   stage,
                          bool               debugFlags);

    // The name a shipping (Release, HLSL 2021) compile of the same recipe has: @p cachePath
    // with its flag tag swapped (D21 -> O3H21). Empty, with @p why set, for a variant no package
    // carries: a -bindless_validate compile (BV), a -hlsl2018 one (D / O3), or a name this
    // scheme did not write.
    std::string ReleaseCachePath(const std::string& cachePath, const char** why = nullptr);

    // Read + validate an .ishdr and hand back just the DXIL payload.
    bool LoadCached(const std::string& cachePath, std::vector<uint8_t>& outDxil);

    // Wrap DXIL in an .ishdr container and write it. Creates the directory.
    bool StoreCached(const std::string& cachePath,
                     const std::string& entryPoint,
                     RHI::ShaderStage   stage,
                     const std::vector<uint8_t>& dxil);

    // The .ishdr container bytes for a DXIL blob (AssetHeader + ShaderMetadata
    // + payload). The ONE builder: StoreCached, ShaderLibrary's cache write and
    // the hot-reload commit all use it.
    std::vector<uint8_t> BuildContainer(const std::string& entryPoint,
                                        RHI::ShaderStage   stage,
                                        const void* dxil, size_t dxilSize);

    // Writes @p bytes to @p path through a temporary sibling and a rename, so a reader (another
    // process sharing shader_cache_dxil/, or this one's render workers) sees the old file or
    // the new one, never a torn one. Creates the directory. Running packs in parallel depends
    // on this.
    bool WriteFileAtomic(const std::string& path, const std::vector<uint8_t>& bytes);

    // The whole path for callers that just want DXIL bytes: a cache hit when the .ishdr is at
    // least as new as the source and all its includes, otherwise a DXC compile that then fills
    // the cache. @p logTag prefixes the diagnostics so a failure names the pass that wanted
    // the shader. Returns an empty vector on failure; callers treat that as fatal.
    std::vector<uint8_t> LoadOrCompile(const std::string& hlslPath,
                                       const std::string& entryPoint,
                                       RHI::ShaderStage   stage,
                                       const std::vector<DxcCompiler::Define>& defines,
                                       const char*        logTag);

    // The language flags a TU may ask for. Both are in the cache key (LoadOrCompile adds them
    // to the define hash) and in the hot-reload recipe (ShaderRecipes keeps the CompileOptions),
    // so a reload compiles the TU exactly as its pass did.
    struct CompileFlags
    {
        bool hlsl2021 = false;
        bool enable16BitTypes = false;
    };
    std::vector<uint8_t> LoadOrCompile(const std::string& hlslPath,
                                       const std::string& entryPoint,
                                       RHI::ShaderStage   stage,
                                       const std::vector<DxcCompiler::Define>& defines,
                                       const char*        logTag,
                                       const CompileFlags& flags);
}
