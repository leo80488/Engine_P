#pragma once
// Harness/AssetToolShaderCook.h - Game.exe / Editor.exe -assettool=shadercook: compile a shader
// variant manifest offline with the runtime's compile path.
//
//   shadercook -manifest=<json> -out=<dir> [-cook_jobs=N]
//
// <json> is a variant list - tests/baselines/shader_variants.json (tools/shader_variants.py), one
// run's shader_variants.json (-shader_variants) or a subset a packager wrote. Every variant is
// compiled the way ShaderLibrary::LoadOrCompile compiles it - its source read through AssetFS,
// DxcCompiler::Compile (the dxcompiler.dll this process loaded, the same arguments, -O3: debug
// off) and ShaderDiskCache::BuildContainer - and written to <dir>/<the key below
// shaders/shader_cache_dxil/>, so <dir> drops in as a package's shaders/shader_cache_dxil/. No
// window, no device.
//
// Result JSON: ok, manifest, out, variants, compiled, failed[{key, source, entry, stage, message}],
// missingSource[], writeFailed[{key, message}], refused[{key, why}] (a variant without a
// shader_cache_dxil key, a source or a stage), warnings, compiler{path, sha256, bytes}, jobs, ms.
// Exit 0 every variant compiled and written, 1 otherwise (a failed compile or write, a missing
// source, a refused variant), 2 usage / I/O.
#include <nlohmann/json_fwd.hpp>

namespace Harness
{
    // Fills @p out, returns the exit code.
    int RunShaderCook(nlohmann::json& out);
}
