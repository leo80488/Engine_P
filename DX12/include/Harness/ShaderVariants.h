#pragma once
// Harness/ShaderVariants.h - -shader_variants[=<file>]: every shader variant this run
// loaded, compiled or failed to compile, written as JSON when the run ends. Each is the
// recipe exactly as DXC got it (source, entry, stage, defines, HLSL version, 16-bit
// types, include dir) under the name its SHIPPING compile has
// (ShaderDiskCache::ReleaseCachePath: a Debug build's recipe is recorded as the -O3 one a
// package carries; a -bindless_validate or -hlsl2018 variant is listed as skipped).
// tools/shader_variants.py harvests these files across the pack corpus into
// tests/baselines/shader_variants.json - the list the CI gate and the packaging cook
// compile.
// The file defaults to <artifacts>/shader_variants.json; a run without -artifacts must
// name one. Logs `[SHADER_VARIANTS] recipes= release= skipped= failed= file=`. Read from
// ShaderRecipes::Snapshot, which forgets nothing, so it runs after the teardown
// (RunLedger::Finalize).

namespace Harness
{
    void WriteShaderVariantsIfRequested();
}
