#pragma once

// DxcCompiler: thin wrapper around IDxcCompiler3 + IDxcUtils.
//
// The one place that compiles HLSL -> DXIL container and builds an
// ID3D12ShaderReflection from a container blob. ShaderLibrary, ShaderImporter and
// the inline-shader call sites all go through here, so the engine speaks one DXC
// dialect (HLSL 2021 by default, SM 6.6) through one compile path.

#include "Graphics/GraphicsStruct.h"

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12ShaderReflection;

namespace DxcCompiler
{
    struct Define
    {
        std::string name;
        std::string value;   // "1" / "0"; empty means -D name (no value)
    };

    struct CompileOptions
    {
        std::string         sourceName;          // diagnostic display + virtual path for relative includes
        std::string         entry = "main";
        RHI::ShaderStage    stage = RHI::ShaderStage::PS;
        std::string         includeDir;          // extra -I directory; empty to skip
        std::vector<Define> defines;
        bool                debug = false;       // -Zi -Od when true; -O3 otherwise
        // The engine compiles HLSL 2021; UsesHlsl2021 decides the version per compile.
        // hlsl2021 marks a TU that REQUIRES it (those that include RTXDI's ReSTIR PT:
        // templates, `half` storage), so -hlsl2018 cannot take it back to 2018. The DXIL
        // is a tracked baseline (tests/baselines/dxil_baseline.json).
        bool                hlsl2021 = false;            // the TU needs -HV 2021
        bool                enable16BitTypes = false;    // -enable-16bit-types (needs native 16-bit ops)
    };

    // Whether a compile is HLSL 2021: every one, unless the process runs with
    // -hlsl2018 (the A/B and fallback switch) and the TU does not require 2021
    // (@p required, CompileOptions::hlsl2021). Every shader cache folds the answer
    // into its key (ShaderDiskCache::CachePath).
    bool UsesHlsl2021(bool required);

    // Whether this process runs the bindless debug lane (-bindless_validate): every
    // compile then gets -D BINDLESS_VALIDATE=1 (shaders/bindless.hlsli), and every
    // shader cache folds it into its key (ShaderDiskCache::CachePath), so a validate
    // run never loads an ordinary blob or the other way round.
    bool BindlessValidate();

    struct CompileResult
    {
        bool                 ok = false;
        std::vector<uint8_t> dxil;       // full DXIL container (matches IDxcResult::GetOutput(DXC_OUT_OBJECT))
        std::string          errorMsg;   // diagnostic text; set on warnings AND failures
    };

    // Compile HLSL source to a DXIL container. Thread-safe.
    CompileResult Compile(const void* src, std::size_t srcSize, const CompileOptions& opts);

    // Builds an ID3D12ShaderReflection over a DXIL container (the blob in
    // CompileResult::dxil). Returns nullptr if reflection cannot be built; the caller
    // owns the returned COM pointer and must Release() it.
    ID3D12ShaderReflection* CreateReflection(const void* container, std::size_t containerSize);

    // SM 6.6 profile string for a stage ("vs_6_6", "ps_6_6", ...; a DXR library is
    // "lib_6_5").
    const char*    ProfileForStage(RHI::ShaderStage stage);
    const wchar_t* ProfileForStageW(RHI::ShaderStage stage);
}
