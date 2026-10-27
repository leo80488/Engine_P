#pragma once

// Shader binding validation: turns root-signature drift into a startup log line instead of
// silent GPU garbage.
// Binding is a three-way agreement between the root signature GraphicsDX12 builds, the slot
// constants in RHI::Slots (RootSignatureLayout.h) and the HLSL `register` declarations, and
// nothing in the toolchain checks the HLSL side. A register the signature does not provide
// does not fail the draw: the shader reads whatever descriptor is there (garbage sampling,
// once DEVICE_HUNG). The DXIL's heap-indexing requirement flags are checked as well.
// DXC reflection already runs on every fetch (ShaderLibrary discovers material parameters
// with it), so this is a walk over existing data, no extra compilation.
// Deliberately not checked:
// - the reverse direction (is every root parameter consumed?): an unconsumed parameter is
//   legal, so such a check would flag correct engine code.
// - a shader with no reflected bindings passes: SM 6.6 ResourceDescriptorHeap[] indexing
//   reflects nothing, indistinguishable from binding nothing.
// - shaders that build their own root signature (the DDGI / RT passes) are exempt by name
//   (UsesPrivateRootSignature).
// It reports, it never aborts: a false positive must not stop the boot. Findings are logged
// as warnings.

#include <cstdint>
#include <string>
#include <vector>

#include "Graphics/GraphicsStruct.h"     // RHI::ShaderStage
#include "Graphics/ShaderReflection.h"

namespace RHI::Validation
{
    // Which shared root signature a shader is recorded under, from its stage: VS/PS/GS/HS/DS/
    // AS/MS use the graphics ("default") signature, CS the compute one.
    enum class RootSigKind : uint8_t { Graphics, Compute };

    // Validates one shader's reflected bindings (@p reflection, from ShaderReflect::Reflect)
    // against the shared signature @p stage selects. @p shaderName (the HLSL file name) names
    // the shader in messages and drives the private-root-signature exemption. Appends one
    // human-readable line per unbacked binding or missing heap flag to @p outProblems.
    // Returns true when every binding maps to a root parameter (or the shader is exempt or has
    // no bindings). Never throws or asserts; ShaderLibrary logs each distinct problem once per
    // process.
    bool ValidateShaderBindings(const std::string&                shaderName,
                                RHI::ShaderStage                  stage,
                                const ShaderReflect::Reflection&  reflection,
                                std::vector<std::string>&         outProblems);
}
