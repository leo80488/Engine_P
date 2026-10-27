#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "System/Check.h"

// RHI::* -> D3D12 enum / format / topology translation helpers: static methods on GraphicsDX12,
// kept in their own TU because they are pure conversion tables with no class state.

// ==== DX12 enum / format conversion helpers ====

DXGI_FORMAT GraphicsDX12::ToDxgiFormat(RHI::Format f)
{
    switch (f)
    {
    case RHI::Format::UNKNOWN:               return DXGI_FORMAT_UNKNOWN;
    case RHI::Format::R32G32B32A32_FLOAT:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RHI::Format::R32G32B32A32_UINT:     return DXGI_FORMAT_R32G32B32A32_UINT;
    case RHI::Format::R32G32B32A32_SINT:     return DXGI_FORMAT_R32G32B32A32_SINT;
    case RHI::Format::R32G32B32_FLOAT:       return DXGI_FORMAT_R32G32B32_FLOAT;
    case RHI::Format::R32G32B32_UINT:        return DXGI_FORMAT_R32G32B32_UINT;
    case RHI::Format::R32G32B32_SINT:        return DXGI_FORMAT_R32G32B32_SINT;
    case RHI::Format::R16G16B16A16_FLOAT:    return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RHI::Format::R16G16B16A16_UNORM:    return DXGI_FORMAT_R16G16B16A16_UNORM;
    case RHI::Format::R16G16B16A16_UINT:     return DXGI_FORMAT_R16G16B16A16_UINT;
    case RHI::Format::R16G16B16A16_SNORM:    return DXGI_FORMAT_R16G16B16A16_SNORM;
    case RHI::Format::R16G16B16A16_SINT:     return DXGI_FORMAT_R16G16B16A16_SINT;
    case RHI::Format::R32G32_FLOAT:          return DXGI_FORMAT_R32G32_FLOAT;
    case RHI::Format::R32G32_UINT:           return DXGI_FORMAT_R32G32_UINT;
    case RHI::Format::R32G32_SINT:           return DXGI_FORMAT_R32G32_SINT;
    case RHI::Format::D32_FLOAT_S8X24_UINT:  return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case RHI::Format::R10G10B10A2_UNORM:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case RHI::Format::R10G10B10A2_UINT:      return DXGI_FORMAT_R10G10B10A2_UINT;
    case RHI::Format::R11G11B10_FLOAT:       return DXGI_FORMAT_R11G11B10_FLOAT;
    case RHI::Format::R8G8B8A8_UNORM:        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RHI::Format::R8G8B8A8_UNORM_SRGB:  return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case RHI::Format::R8G8B8A8_UINT:         return DXGI_FORMAT_R8G8B8A8_UINT;
    case RHI::Format::R8G8B8A8_SNORM:        return DXGI_FORMAT_R8G8B8A8_SNORM;
    case RHI::Format::R8G8B8A8_SINT:         return DXGI_FORMAT_R8G8B8A8_SINT;
    case RHI::Format::B8G8R8A8_UNORM:        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case RHI::Format::B8G8R8A8_UNORM_SRGB:  return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case RHI::Format::R16G16_FLOAT:          return DXGI_FORMAT_R16G16_FLOAT;
    case RHI::Format::R16G16_UNORM:          return DXGI_FORMAT_R16G16_UNORM;
    case RHI::Format::R16G16_UINT:           return DXGI_FORMAT_R16G16_UINT;
    case RHI::Format::R16G16_SNORM:          return DXGI_FORMAT_R16G16_SNORM;
    case RHI::Format::R16G16_SINT:           return DXGI_FORMAT_R16G16_SINT;
    case RHI::Format::D32_FLOAT:             return DXGI_FORMAT_D32_FLOAT;
    case RHI::Format::R32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
    case RHI::Format::R32_UINT:              return DXGI_FORMAT_R32_UINT;
    case RHI::Format::R32_SINT:              return DXGI_FORMAT_R32_SINT;
    case RHI::Format::D24_UNORM_S8_UINT:     return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RHI::Format::R9G9B9E5_SHAREDEXP:    return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    case RHI::Format::R8G8_UNORM:            return DXGI_FORMAT_R8G8_UNORM;
    case RHI::Format::R8G8_UINT:             return DXGI_FORMAT_R8G8_UINT;
    case RHI::Format::R8G8_SNORM:            return DXGI_FORMAT_R8G8_SNORM;
    case RHI::Format::R8G8_SINT:             return DXGI_FORMAT_R8G8_SINT;
    case RHI::Format::R16_FLOAT:             return DXGI_FORMAT_R16_FLOAT;
    case RHI::Format::D16_UNORM:             return DXGI_FORMAT_D16_UNORM;
    case RHI::Format::R16_UNORM:             return DXGI_FORMAT_R16_UNORM;
    case RHI::Format::R16_UINT:              return DXGI_FORMAT_R16_UINT;
    case RHI::Format::R16_SNORM:             return DXGI_FORMAT_R16_SNORM;
    case RHI::Format::R16_SINT:              return DXGI_FORMAT_R16_SINT;
    case RHI::Format::R8_UNORM:              return DXGI_FORMAT_R8_UNORM;
    case RHI::Format::R8_UINT:               return DXGI_FORMAT_R8_UINT;
    case RHI::Format::R8_SNORM:              return DXGI_FORMAT_R8_SNORM;
    case RHI::Format::R8_SINT:               return DXGI_FORMAT_R8_SINT;
    case RHI::Format::BC1_UNORM:             return DXGI_FORMAT_BC1_UNORM;
    case RHI::Format::BC1_UNORM_SRGB:        return DXGI_FORMAT_BC1_UNORM_SRGB;
    case RHI::Format::BC2_UNORM:             return DXGI_FORMAT_BC2_UNORM;
    case RHI::Format::BC2_UNORM_SRGB:        return DXGI_FORMAT_BC2_UNORM_SRGB;
    case RHI::Format::BC3_UNORM:             return DXGI_FORMAT_BC3_UNORM;
    case RHI::Format::BC3_UNORM_SRGB:        return DXGI_FORMAT_BC3_UNORM_SRGB;
    case RHI::Format::BC4_UNORM:             return DXGI_FORMAT_BC4_UNORM;
    case RHI::Format::BC4_SNORM:             return DXGI_FORMAT_BC4_SNORM;
    case RHI::Format::BC5_UNORM:             return DXGI_FORMAT_BC5_UNORM;
    case RHI::Format::BC5_SNORM:             return DXGI_FORMAT_BC5_SNORM;
    case RHI::Format::BC6H_UF16:             return DXGI_FORMAT_BC6H_UF16;
    case RHI::Format::BC6H_SF16:             return DXGI_FORMAT_BC6H_SF16;
    case RHI::Format::BC7_UNORM:             return DXGI_FORMAT_BC7_UNORM;
    case RHI::Format::BC7_UNORM_SRGB:        return DXGI_FORMAT_BC7_UNORM_SRGB;
    case RHI::Format::NV12:                  return DXGI_FORMAT_NV12;
    default:                                 return DXGI_FORMAT_UNKNOWN;
    }
}

namespace
{
    // Every RHI state bit maps to D3D12 bits and an OR of RHI states to the OR of theirs, so a
    // combined read state (a depth plane's DEPTHSTENCIL_READONLY | SHADER_RESOURCE) is legal.
    // UNDEFINED and SWAPCHAIN carry no bit: COMMON, which PRESENT equals.
    constexpr uint32_t D3D(D3D12_RESOURCE_STATES s) { return static_cast<uint32_t>(s); }
    constexpr uint32_t D3D12StateBits(uint32_t rhi)
    {
        using RS = RHI::ResourceState;
        struct Entry { RS rhi; uint32_t d3d; };
        constexpr Entry kMap[] = {
            { RS::SHADER_RESOURCE,                   D3D(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                                     D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) },
            { RS::SHADER_RESOURCE_COMPUTE,           D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) },
            { RS::UNORDERED_ACCESS,                  D3D(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) },
            { RS::COPY_SRC,                          D3D(D3D12_RESOURCE_STATE_COPY_SOURCE) },
            { RS::COPY_DST,                          D3D(D3D12_RESOURCE_STATE_COPY_DEST) },
            { RS::RENDERTARGET,                      D3D(D3D12_RESOURCE_STATE_RENDER_TARGET) },
            { RS::DEPTHSTENCIL,                      D3D(D3D12_RESOURCE_STATE_DEPTH_WRITE) },
            { RS::DEPTHSTENCIL_READONLY,             D3D(D3D12_RESOURCE_STATE_DEPTH_READ) },
            { RS::SHADING_RATE_SOURCE,               D3D(D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE) },
            { RS::VERTEX_BUFFER,                     D3D(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) },
            { RS::INDEX_BUFFER,                      D3D(D3D12_RESOURCE_STATE_INDEX_BUFFER) },
            { RS::CONSTANT_BUFFER,                   D3D(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) },
            { RS::INDIRECT_ARGUMENT,                 D3D(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) },
            { RS::RAYTRACING_ACCELERATION_STRUCTURE, D3D(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE) },
            { RS::PREDICATION,                       D3D(D3D12_RESOURCE_STATE_PREDICATION) },
            { RS::VIDEO_DECODE_SRC,                  D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_READ) },
            { RS::VIDEO_DECODE_DST,                  D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) },
            { RS::VIDEO_DECODE_DPB,                  D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_READ) |
                                                     D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE) },
            { RS::DEPTH_READ_SRV,                    D3D(D3D12_RESOURCE_STATE_DEPTH_READ) |
                                                     D3D(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                                     D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) },
        };
        uint32_t d = 0;
        for (const Entry& e : kMap)
            if (rhi & static_cast<uint32_t>(e.rhi)) d |= e.d3d;
        return d;
    }
    constexpr uint32_t S(RHI::ResourceState s) { return D3D12StateBits(static_cast<uint32_t>(s)); }
    constexpr uint32_t Or(RHI::ResourceState a, RHI::ResourceState b)
    { return D3D12StateBits(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); }

    // Each single state translates to its exact D3D12 state(s)...
    using RSA = RHI::ResourceState;
    static_assert(S(RSA::UNDEFINED)               == D3D(D3D12_RESOURCE_STATE_COMMON));
    static_assert(S(RSA::SWAPCHAIN)               == D3D(D3D12_RESOURCE_STATE_COMMON));
    static_assert(S(RSA::SHADER_RESOURCE)         == (D3D(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                                     D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
    static_assert(S(RSA::SHADER_RESOURCE_COMPUTE) == D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    static_assert(S(RSA::UNORDERED_ACCESS)        == D3D(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    static_assert(S(RSA::COPY_SRC)                == D3D(D3D12_RESOURCE_STATE_COPY_SOURCE));
    static_assert(S(RSA::COPY_DST)                == D3D(D3D12_RESOURCE_STATE_COPY_DEST));
    static_assert(S(RSA::RENDERTARGET)            == D3D(D3D12_RESOURCE_STATE_RENDER_TARGET));
    static_assert(S(RSA::DEPTHSTENCIL)            == D3D(D3D12_RESOURCE_STATE_DEPTH_WRITE));
    static_assert(S(RSA::DEPTHSTENCIL_READONLY)   == D3D(D3D12_RESOURCE_STATE_DEPTH_READ));
    static_assert(S(RSA::DEPTH_READ_SRV)          == (D3D(D3D12_RESOURCE_STATE_DEPTH_READ) |
                                                     D3D(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
                                                     D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
    static_assert(S(RSA::VERTEX_BUFFER)           == D3D(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    static_assert(S(RSA::CONSTANT_BUFFER)         == D3D(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    static_assert(S(RSA::INDEX_BUFFER)            == D3D(D3D12_RESOURCE_STATE_INDEX_BUFFER));
    static_assert(S(RSA::INDIRECT_ARGUMENT)       == D3D(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT));
    static_assert(S(RSA::RAYTRACING_ACCELERATION_STRUCTURE) == D3D(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE));
    static_assert(S(RSA::PREDICATION)             == D3D(D3D12_RESOURCE_STATE_PREDICATION));
    static_assert(S(RSA::VIDEO_DECODE_SRC)        == D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_READ));
    static_assert(S(RSA::VIDEO_DECODE_DST)        == D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE));
    static_assert(S(RSA::VIDEO_DECODE_DPB)        == (D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_READ) |
                                                     D3D(D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE)));
    // ...and the combinations a depth plane takes compose.
    static_assert(Or(RSA::DEPTHSTENCIL_READONLY, RSA::SHADER_RESOURCE) == S(RSA::DEPTH_READ_SRV));
    static_assert(Or(RSA::DEPTHSTENCIL_READONLY, RSA::SHADER_RESOURCE_COMPUTE) ==
                  (D3D(D3D12_RESOURCE_STATE_DEPTH_READ) | D3D(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)));
}

D3D12_RESOURCE_STATES GraphicsDX12::ToD3D12ResourceState(RHI::ResourceState s)
{
    return static_cast<D3D12_RESOURCE_STATES>(D3D12StateBits(static_cast<uint32_t>(s)));
}

D3D12_FILTER GraphicsDX12::ToD3D12Filter(RHI::Filter f)
{
    switch (f)
    {
    case RHI::Filter::MIN_MAG_MIP_POINT:              return D3D12_FILTER_MIN_MAG_MIP_POINT;
    case RHI::Filter::MIN_MAG_POINT_MIP_LINEAR:       return D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MIN_POINT_MAG_LINEAR_MIP_POINT: return D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MIN_POINT_MAG_MIP_LINEAR:       return D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    case RHI::Filter::MIN_LINEAR_MAG_MIP_POINT:       return D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    case RHI::Filter::MIN_LINEAR_MAG_POINT_MIP_LINEAR:return D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MIN_MAG_LINEAR_MIP_POINT:       return D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MIN_MAG_MIP_LINEAR:             return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::ANISOTROPIC:                    return D3D12_FILTER_ANISOTROPIC;
    case RHI::Filter::COMPARISON_MIN_MAG_MIP_POINT:   return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
    case RHI::Filter::COMPARISON_MIN_MAG_MIP_LINEAR:  return D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::COMPARISON_ANISOTROPIC:         return D3D12_FILTER_COMPARISON_ANISOTROPIC;
    // A SamplerComparisonState fed a non-comparison D3D12 sampler makes SampleCmp* undefined, with
    // no debug-layer or GBV message (it reads as unshadowed): every comparison filter must map to
    // its D3D12 comparison filter, never fall to the default.
    case RHI::Filter::COMPARISON_MIN_MAG_POINT_MIP_LINEAR:        return D3D12_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT:  return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::COMPARISON_MIN_POINT_MAG_MIP_LINEAR:        return D3D12_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR;
    case RHI::Filter::COMPARISON_MIN_LINEAR_MAG_MIP_POINT:        return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT;
    case RHI::Filter::COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR: return D3D12_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::COMPARISON_MIN_MAG_LINEAR_MIP_POINT:        return D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    // MINIMUM / MAXIMUM reduction filters (Tier 2+ hardware). Unused, but mapped because the enum
    // declares them: a declared value that falls to the default is a silent bug.
    case RHI::Filter::MINIMUM_MIN_MAG_MIP_POINT:               return D3D12_FILTER_MINIMUM_MIN_MAG_MIP_POINT;
    case RHI::Filter::MINIMUM_MIN_MAG_POINT_MIP_LINEAR:        return D3D12_FILTER_MINIMUM_MIN_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:  return D3D12_FILTER_MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MINIMUM_MIN_POINT_MAG_MIP_LINEAR:        return D3D12_FILTER_MINIMUM_MIN_POINT_MAG_MIP_LINEAR;
    case RHI::Filter::MINIMUM_MIN_LINEAR_MAG_MIP_POINT:        return D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_MIP_POINT;
    case RHI::Filter::MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR: return D3D12_FILTER_MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MINIMUM_MIN_MAG_LINEAR_MIP_POINT:        return D3D12_FILTER_MINIMUM_MIN_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MINIMUM_MIN_MAG_MIP_LINEAR:              return D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::MINIMUM_ANISOTROPIC:                     return D3D12_FILTER_MINIMUM_ANISOTROPIC;
    case RHI::Filter::MAXIMUM_MIN_MAG_MIP_POINT:               return D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_POINT;
    case RHI::Filter::MAXIMUM_MIN_MAG_POINT_MIP_LINEAR:        return D3D12_FILTER_MAXIMUM_MIN_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT:  return D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MAXIMUM_MIN_POINT_MAG_MIP_LINEAR:        return D3D12_FILTER_MAXIMUM_MIN_POINT_MAG_MIP_LINEAR;
    case RHI::Filter::MAXIMUM_MIN_LINEAR_MAG_MIP_POINT:        return D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_MIP_POINT;
    case RHI::Filter::MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR: return D3D12_FILTER_MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case RHI::Filter::MAXIMUM_MIN_MAG_LINEAR_MIP_POINT:        return D3D12_FILTER_MAXIMUM_MIN_MAG_LINEAR_MIP_POINT;
    case RHI::Filter::MAXIMUM_MIN_MAG_MIP_LINEAR:              return D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR;
    case RHI::Filter::MAXIMUM_ANISOTROPIC:                     return D3D12_FILTER_MAXIMUM_ANISOTROPIC;
    default:
        // An unmapped enum is a bug in this table; do not downgrade silently: a comparison filter that
        // becomes a plain one kills shadows without an error.
        (void)ENGINE_CHECK_ONCE(false, "ToD3D12Filter: unmapped RHI::Filter %d — "
                                       "add it to the table (falling back to linear)",
                                static_cast<int>(f));
        return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

D3D12_TEXTURE_ADDRESS_MODE GraphicsDX12::ToD3D12AddressMode(RHI::TextureAddressMode m)
{
    switch (m)
    {
    case RHI::TextureAddressMode::WRAP:        return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case RHI::TextureAddressMode::MIRROR:      return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case RHI::TextureAddressMode::CLAMP:       return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case RHI::TextureAddressMode::BORDER:      return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case RHI::TextureAddressMode::MIRROR_ONCE: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default:                                   return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

D3D12_COMPARISON_FUNC GraphicsDX12::ToD3D12ComparisonFunc(RHI::ComparisonFunc f)
{
    switch (f)
    {
    case RHI::ComparisonFunc::NEVER:         return D3D12_COMPARISON_FUNC_NEVER;
    case RHI::ComparisonFunc::LESS:          return D3D12_COMPARISON_FUNC_LESS;
    case RHI::ComparisonFunc::EQUAL:         return D3D12_COMPARISON_FUNC_EQUAL;
    case RHI::ComparisonFunc::LESS_EQUAL:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    case RHI::ComparisonFunc::GREATER:       return D3D12_COMPARISON_FUNC_GREATER;
    case RHI::ComparisonFunc::NOT_EQUAL:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
    case RHI::ComparisonFunc::GREATER_EQUAL: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    case RHI::ComparisonFunc::ALWAYS:        return D3D12_COMPARISON_FUNC_ALWAYS;
    case RHI::ComparisonFunc::NONE:          return D3D12_COMPARISON_FUNC_NONE;
    default:                                 return D3D12_COMPARISON_FUNC_NEVER;
    }
}

D3D12_BLEND GraphicsDX12::ToD3D12Blend(RHI::Blend b)
{
    switch (b)
    {
    case RHI::Blend::ZERO:             return D3D12_BLEND_ZERO;
    case RHI::Blend::ONE:              return D3D12_BLEND_ONE;
    case RHI::Blend::SRC_COLOR:        return D3D12_BLEND_SRC_COLOR;
    case RHI::Blend::INV_SRC_COLOR:    return D3D12_BLEND_INV_SRC_COLOR;
    case RHI::Blend::SRC_ALPHA:        return D3D12_BLEND_SRC_ALPHA;
    case RHI::Blend::INV_SRC_ALPHA:    return D3D12_BLEND_INV_SRC_ALPHA;
    case RHI::Blend::DEST_ALPHA:       return D3D12_BLEND_DEST_ALPHA;
    case RHI::Blend::INV_DEST_ALPHA:   return D3D12_BLEND_INV_DEST_ALPHA;
    case RHI::Blend::DEST_COLOR:       return D3D12_BLEND_DEST_COLOR;
    case RHI::Blend::INV_DEST_COLOR:   return D3D12_BLEND_INV_DEST_COLOR;
    case RHI::Blend::SRC_ALPHA_SAT:    return D3D12_BLEND_SRC_ALPHA_SAT;
    case RHI::Blend::BLEND_FACTOR:     return D3D12_BLEND_BLEND_FACTOR;
    case RHI::Blend::INV_BLEND_FACTOR: return D3D12_BLEND_INV_BLEND_FACTOR;
    case RHI::Blend::SRC1_COLOR:       return D3D12_BLEND_SRC1_COLOR;
    case RHI::Blend::INV_SRC1_COLOR:   return D3D12_BLEND_INV_SRC1_COLOR;
    case RHI::Blend::SRC1_ALPHA:       return D3D12_BLEND_SRC1_ALPHA;
    case RHI::Blend::INV_SRC1_ALPHA:   return D3D12_BLEND_INV_SRC1_ALPHA;
    default:                           return D3D12_BLEND_ZERO;
    }
}

D3D12_BLEND_OP GraphicsDX12::ToD3D12BlendOp(RHI::BlendOp op)
{
    switch (op)
    {
    case RHI::BlendOp::ADD:          return D3D12_BLEND_OP_ADD;
    case RHI::BlendOp::SUBTRACT:     return D3D12_BLEND_OP_SUBTRACT;
    case RHI::BlendOp::REV_SUBTRACT: return D3D12_BLEND_OP_REV_SUBTRACT;
    case RHI::BlendOp::MIN:          return D3D12_BLEND_OP_MIN;
    case RHI::BlendOp::MAX:          return D3D12_BLEND_OP_MAX;
    default:                         return D3D12_BLEND_OP_ADD;
    }
}

D3D12_FILL_MODE GraphicsDX12::ToD3D12FillMode(RHI::FillMode m)
{
    return (m == RHI::FillMode::WIREFRAME) ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
}

D3D12_CULL_MODE GraphicsDX12::ToD3D12CullMode(RHI::CullMode m)
{
    switch (m)
    {
    case RHI::CullMode::NONE:  return D3D12_CULL_MODE_NONE;
    case RHI::CullMode::FRONT: return D3D12_CULL_MODE_FRONT;
    case RHI::CullMode::BACK:  return D3D12_CULL_MODE_BACK;
    default:                   return D3D12_CULL_MODE_NONE;
    }
}

D3D12_RESOURCE_FLAGS GraphicsDX12::ToD3D12ResourceFlags(RHI::BindFlag flags)
{
    D3D12_RESOURCE_FLAGS out = D3D12_RESOURCE_FLAG_NONE;
    auto f = static_cast<uint32_t>(flags);
    if (f & static_cast<uint32_t>(RHI::BindFlag::RENDER_TARGET))    out |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (f & static_cast<uint32_t>(RHI::BindFlag::DEPTH_STENCIL))    out |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (f & static_cast<uint32_t>(RHI::BindFlag::UNORDERED_ACCESS)) out |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return out;
}

D3D12_HEAP_TYPE GraphicsDX12::ToD3D12HeapType(RHI::Usage usage)
{
    switch (usage)
    {
    case RHI::Usage::UPLOAD:   return D3D12_HEAP_TYPE_UPLOAD;
    case RHI::Usage::READBACK: return D3D12_HEAP_TYPE_READBACK;
    default:                   return D3D12_HEAP_TYPE_DEFAULT;
    }
}

D3D12_PRIMITIVE_TOPOLOGY GraphicsDX12::ToD3D12Topology(RHI::PrimitiveTopology t)
{
    switch (t)
    {
    case RHI::PrimitiveTopology::POINTLIST:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RHI::PrimitiveTopology::LINELIST:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RHI::PrimitiveTopology::LINESTRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RHI::PrimitiveTopology::TRIANGLELIST:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case RHI::PrimitiveTopology::TRIANGLESTRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    default:
        // PATCHLIST needs the control-point count (D3D_PRIMITIVE_TOPOLOGY_n_
        // CONTROL_POINT_PATCHLIST), which this per-draw call does not carry,
        // and UNDEFINED is not a draw topology. Neither has a caller (no HS/DS
        // shader exists); do not let one rasterise as triangles silently.
        (void)ENGINE_CHECK_ONCE(false, "ToD3D12Topology: unmapped RHI::PrimitiveTopology %d "
                                       "(PATCHLIST/UNDEFINED) — falling back to TRIANGLELIST",
                                static_cast<int>(t));
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

// ==== DX12 translation helper - queue type ====

D3D12_COMMAND_LIST_TYPE GraphicsDX12::ToD3D12CommandListType(RHI::QueueType q)
{
    switch (q)
    {
    case RHI::QueueType::COMPUTE: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case RHI::QueueType::COPY:    return D3D12_COMMAND_LIST_TYPE_COPY;
    default:                       return D3D12_COMMAND_LIST_TYPE_DIRECT;
    }
}
