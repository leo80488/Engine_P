#include "Graphics/ShaderBindingValidation.h"

#include <algorithm>
#include <cstdio>

#include <d3d12shader.h>   // D3D_SHADER_REQUIRES_*

#include "Graphics/RootSignatureLayout.h"

namespace RHI::Validation
{
namespace
{
    // Register class, derived from the reflected resource type. This is the
    // axis the root signature is organised by: a `t` register can never be
    // satisfied by a `u` root parameter no matter how the numbers line up.
    enum class RegClass : uint8_t { SRV, UAV, CBV, Sampler };

    // A contiguous register range that some root parameter provides.
    struct Range
    {
        RegClass cls;
        uint32_t space;
        uint32_t first;
        uint32_t count;      // 0 == unbounded
        bool Covers(RegClass c, uint32_t sp, uint32_t reg, uint32_t n) const
        {
            if (c != cls || sp != space) return false;
            if (reg < first)             return false;
            if (count == 0)              return true;         // unbounded array
            if (n == 0)                  return false;        // unbounded need vs bounded provide
            return (reg + n) <= (first + count);
        }
    };

    RegClass ClassOf(ShaderReflect::ResourceType t)
    {
        using RT = ShaderReflect::ResourceType;
        switch (t)
        {
        case RT::Texture:
        case RT::StructuredBuffer:
        case RT::ByteAddressBuffer:   return RegClass::SRV;
        case RT::RWTexture:
        case RT::RWStructuredBuffer:  return RegClass::UAV;
        case RT::CBuffer:             return RegClass::CBV;
        case RT::Sampler:             return RegClass::Sampler;
        default:                      return RegClass::SRV;
        }
    }

    const char* ClassLetter(RegClass c)
    {
        switch (c)
        {
        case RegClass::SRV:     return "t";
        case RegClass::UAV:     return "u";
        case RegClass::CBV:     return "b";
        default:                return "s";
        }
    }

    // ==== Graphics ("default") root signature ====
    // Mirrors GraphicsDX12::CreateDefaultRootSignature. Counts come from RHI::Slots::Gfx so a
    // resize cannot desync this table; the register ranges are restated because only the root
    // signature knows them and they are what a shader declares. No UAV entries: the graphics
    // signature has none.
    const std::vector<Range>& GraphicsRanges()
    {
        namespace S = Slots::Gfx;
        static const std::vector<Range> kRanges = {
            { RegClass::CBV, 0,  0, 1 },                    // [0]  b0 root constants
            { RegClass::CBV, 0,  1, S::CBVCount },          // [1..7] b1-b7
            { RegClass::SRV, 0,  0, 1 },                    // [8]  t0 instance buffer
            { RegClass::SRV, 0,  1, 1 },                    // [9]  t1 mesh descriptors
            { RegClass::SRV, 0,  2, S::SRVCount },          // [10..13] t2-t5
            { RegClass::SRV, 1,  0, 0 },                    // [14] t0 space1 bindless buffers
            // No sampler range: the graphics signature has no sampler tables (samplers come from
            // SamplerDescriptorHeap[], sampler_heap.hlsli), so a graphics shader that declares a
            // sampler register is named here instead of failing at PSO creation.
            { RegClass::SRV, 0,  6, S::IBLSRVCount },       // [15..17] t6-t8
            { RegClass::SRV, 0,  9, S::ShadowSRVCount },    // [18] t9-t11
            { RegClass::SRV, 0, 12, 1 },                    // [23] t12 material buffer
            { RegClass::SRV, 0, 13, S::ClusterSRVCount },   // [19..21] t13-t15
            { RegClass::SRV, 2,  0, 0 },                    // [22] t0 space2 bindless textures
            { RegClass::SRV, 0, 17, 1 },                    // [24] t17 emissive
            { RegClass::SRV, 0, 18, 1 },                    // [25] t18 SSAO
            { RegClass::SRV, 0, 19, 1 },                    // [26] t19 sky SH
            { RegClass::SRV, 0, 20, 1 },                    // [27] t20 aerial perspective
            { RegClass::SRV, 0, 21, 1 },                    // [28] t21 spot shadow atlas
            { RegClass::SRV, 0, 22, 1 },                    // [29] t22 spot shadow VPs
            { RegClass::CBV, 0,  8, 1 },                    // [30] b8 custom material CBV
            { RegClass::SRV, 3,  0, 0 },                    // [31] t0-t7 space3 custom material textures
            { RegClass::CBV, 9,  0, 1 },                    // [32] b0 space9 dynamic sampler index - last parameter
            // No range for t16 and t23..t45 on purpose: those registers are reserved (most became heap
            // indices in LightCB / DDGI::VolumeGPUDesc; RootSignatureLayout.h lists them), so a
            // graphics shader that declares one fails validation at startup instead of binding nothing.
            // Do not restore an entry to silence such a failure: fix the shader.
            // The HybridRefl* and RTReflectionTrace compute shaders declare t8/t12/t16 (DDGI),
            // t23/t24 (sky cubes), t25 (gReflectionProbes) and t27 (gDepthHier) under
            // RTReflectionPass's private root signature, not this one.
        };
        return kRanges;
    }

    // ==== Compute root signature ====
    // Mirrors GraphicsDX12::CreateComputeRootSignature, including its static samplers: they are
    // not root parameters, but most compute shaders declare s0..s2 space2.
    const std::vector<Range>& ComputeRanges()
    {
        static const std::vector<Range> kRanges = {
            { RegClass::CBV, 2, 0, 1 },      // [0]  b0 space2
            { RegClass::SRV, 2, 0, 1 },      // [1]  t0 space2
            { RegClass::SRV, 2, 1, 1 },      // [2]  t1 space2
            { RegClass::SRV, 2, 2, 1 },      // [3]  t2 space2
            { RegClass::UAV, 2, 0, 1 },      // [4]  u0 space2
            { RegClass::UAV, 2, 1, 1 },      // [5]  u1 space2
            { RegClass::SRV, 2, 5, 1 },      // [6]  t5 space2
            { RegClass::SRV, 2, 3, 1 },      // [7]  t3 space2
            { RegClass::SRV, 2, 4, 1 },      // [8]  t4 space2
            { RegClass::SRV, 0, 0, 1 },      // [9]  t0 space0 instance buffer
            { RegClass::SRV, 0, 1, 1 },      // [10] t1 space0 mesh descriptors
            { RegClass::SRV, 1, 0, 0 },      // [11] t0 space1 bindless buffers
            { RegClass::SRV, 2, 6, 1 },      // [12] t6 space2
            { RegClass::SRV, 2, 7, 1 },      // [13] t7 space2
            { RegClass::UAV, 2, 2, 1 },      // [14] u2 space2
            { RegClass::UAV, 2, 3, 1 },      // [15] u3 space2
            { RegClass::UAV, 2, 4, 1 },      // [16] u4 space2
            { RegClass::SRV, 3, 0, 0 },      // [17] t0 space3 bindless textures
            // Static samplers s0..s2 space2: not root parameters, but shaders legitimately declare
            // them (leaving them out is the biggest source of false positives).
            { RegClass::Sampler, 2, 0, Slots::Cmp::StaticSamplerCount },
        };
        return kRanges;
    }

    // Shaders that build and bind their own root signature. Validating them against the shared
    // compute layout is meaningless: e.g. they declare g_AllTextures[] at t0 space2, where the
    // shared compute signature has bindless textures at t0 space3. Matched as a file-name suffix.
    bool UsesPrivateRootSignature(const std::string& shaderName)
    {
        // Exactly the shaders whose registers the shared signatures do not provide because their
        // owning pass builds its own layout (from a sweep of every register declaration in
        // shaders/ against the two shared tables).
        static const char* kPrivateRootSigShaders[] = {
            // DDGIPass: own root signature
            "DDGIRayTrace.cs.hlsl",
            "DDGIProbeRelocate.cs.hlsl",
            "DDGIBorderUpdate.cs.hlsl",
            "DDGIRelight.cs.hlsl",
            "DDGIRayAllocation.cs.hlsl",
            "DDGIPrepareRayCount.cs.hlsl",
            "DDGIFinalizeIndirect.cs.hlsl",
            // RTAOPass / RTShadowPass: own root signatures
            "RTAO.cs.hlsl",
            "RTAOResolve.cs.hlsl",
            "RTShadow.cs.hlsl",
            "RTShadowResolve.cs.hlsl",
            // RTReflectionPass: own root signature
            "HybridReflIntersectHW.cs.hlsl",
        };
        for (const char* p : kPrivateRootSigShaders)
        {
            const std::string s(p);
            if (shaderName.size() >= s.size() &&
                shaderName.compare(shaderName.size() - s.size(), s.size(), s) == 0)
                return true;
        }
        // Everything the RT reflection pass owns lives under its own layout too.
        return shaderName.find("HybridRefl") != std::string::npos ||
               shaderName.find("RTReflection") != std::string::npos;
    }
}

bool ValidateShaderBindings(const std::string&               shaderName,
                            RHI::ShaderStage                 stage,
                            const ShaderReflect::Reflection& reflection,
                            std::vector<std::string>&        outProblems)
{
    // Shader libraries (DXR) never run under either shared signature.
    if (stage == RHI::ShaderStage::LIB)          return true;
    if (UsesPrivateRootSignature(shaderName))    return true;

    const bool isCompute = (stage == RHI::ShaderStage::CS);
    bool ok = true;

    // What the DXIL requires of the signature: the SM 6.6 dynamic-resource flags the compiler
    // records, checked against what the shared signatures allow (RHI::Slots::kShared*HeapIndexed,
    // the constants GraphicsDX12's graphics and compute builders take their flags from). This
    // names the shader and the missing flag at load, before any PSO is built from it.
    // Runs before the no-bindings early-out on purpose: a heap-indexing shader often reflects no
    // binding.
    constexpr bool kSharedResourceHeapIndexed = RHI::Slots::kSharedResourceHeapIndexed;
    constexpr bool kSharedSamplerHeapIndexed  = RHI::Slots::kSharedSamplerHeapIndexed;
    auto requireFlag = [&](uint64_t bit, bool allowed, const char* heap, const char* flag)
    {
        if (!(reflection.requiresFlags & bit) || allowed) return;
        ok = false;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s indexes %s[], but the shared %s root signature lacks %s",
                      shaderName.c_str(), heap, isCompute ? "compute" : "graphics", flag);
        outProblems.emplace_back(buf);
    };
    requireFlag(D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING, kSharedResourceHeapIndexed,
                "ResourceDescriptorHeap", "CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED");
    requireFlag(D3D_SHADER_REQUIRES_SAMPLER_DESCRIPTOR_HEAP_INDEXING, kSharedSamplerHeapIndexed,
                "SamplerDescriptorHeap", "SAMPLER_HEAP_DIRECTLY_INDEXED");

    // No reflected bindings is a PASS, not a failure: that is what an SM6.6
    // shader indexing ResourceDescriptorHeap[] directly looks like.
    if (reflection.bindings.empty())             return ok;

    const std::vector<Range>& ranges = isCompute ? ComputeRanges() : GraphicsRanges();

    for (const ShaderReflect::ResourceBinding& b : reflection.bindings)
    {
        const RegClass cls = ClassOf(b.type);
        const bool covered = std::any_of(ranges.begin(), ranges.end(),
            [&](const Range& r) { return r.Covers(cls, b.space, b.bindPoint, b.bindCount); });
        if (covered) continue;

        ok = false;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%s declares '%s' at %s%u space%u (count %u) — the %s root "
                      "signature provides no such range",
                      shaderName.c_str(), b.name.c_str(), ClassLetter(cls),
                      b.bindPoint, b.space, b.bindCount,
                      isCompute ? "compute" : "graphics");
        outProblems.emplace_back(buf);
    }
    return ok;
}

} // namespace RHI::Validation
