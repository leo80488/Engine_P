#pragma once
// Harness/AssetToolMatDecl.h - Game.exe / Editor.exe -assettool=matdecl: every material shader's
// declarations checked against the DXIL of its compiled variants.
//
//   matdecl [-manifest=<variants.json>] [-asset=<file|dir>[;<file|dir>...]] [-cook_jobs=N]
//
// The manifest (default tests/baselines/shader_variants.json) lists the variants. Each one not
// marked failed whose source lies outside both its include root and shaders/ - a custom
// material's, a material graph's, a material sky's, a trail / UI / particle shader - is compiled
// the way ShaderLibrary::LoadOrCompile compiles it (the source through AssetFS,
// DxcCompiler::Compile, -O3), reflected, stamped with its root source's annotations as
// ShaderLibrary::GetShaderInternal stamps them, and compared with MaterialShaderDecl::Parse of the
// source (CrossCheck). -asset adds sources the manifest does not list (a directory: every .hlsl
// under it), each compiled at permutation 0 for the stage(s) its file name or its declarations
// name (.vs / .ps / .cs.hlsl; a unified .hlsl is PS, and VS when it defines
// MATERIAL_HAS_VERTEX_OFFSET).
//
// Result JSON: ok, manifest, sources[{source, read, exact, files[], inexact[], notes[],
// capabilities{surface, vertexOffset, vertexModify, domain}, textSearch{surface, vertexOffset,
// vertexModify} (a plain substring search of the root's text for each capability name: a name in
// a comment counts, an own include's define does not; each difference from capabilities is a
// summary.capabilityDiffs row), permBits, engineIncludes[], unresolvedIncludes[], warnings[],
// dynamicMask (ShaderLibrary::DynamicPermMask, the key mask the runtime caches the shader under),
// engineRolesExplicit, bindings[], cbuffers[], variants[{key, stage, manifest, defines[] (the names
// of those not 0), compiled, error, mismatches[]}]}], summary{sources, variants, compiled,
// compileFailed, extraCompileFailed, mismatches, mismatchedVariants, inexactSources,
// capabilityDiffs[]}, jobs, ms. No window, no device.
//
// Exit 0: every manifest variant compiled and no exact declaration disagrees with a DXIL;
// 1 otherwise; 2 usage / I/O.
#include <nlohmann/json_fwd.hpp>

namespace Harness
{
    // Fills @p out, returns the exit code.
    int RunMatDeclCheck(nlohmann::json& out);
}
