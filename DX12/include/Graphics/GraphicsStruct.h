#pragma once

#include <cstdint>
#include <initializer_list>
#include <vector>
#include <limits>
#include <type_traits>

#include "../../shaders/shared/mesh_descriptor.hlsli"   // RHI::MeshDescriptor / StreamDescriptor
#include "Graphics/RootSignatureLayout.h"   // the typed root slots

// Forward declarations for CommandList recording methods
class IGraphicsDevice;
namespace RG { class RenderContext; }

namespace RHI
{
    // ==== Enums ====

    enum class ShaderStage : uint8_t
    {
        MS,     // Mesh Shader
        AS,     // Amplification Shader
        VS,     // Vertex Shader
        HS,     // Hull Shader
        DS,     // Domain Shader
        GS,     // Geometry Shader
        PS,     // Pixel Shader
        CS,     // Compute Shader
        LIB,    // Shader Library
        Count,
    };

    enum class ShaderFormat : uint8_t
    {
        NONE,
        HLSL5,  // DXBC
        HLSL6,  // DXIL
    };

    enum class ShaderModel : uint8_t
    {
        SM_5_0, SM_6_0, SM_6_1, SM_6_2,
        SM_6_3, SM_6_4, SM_6_5, SM_6_6, SM_6_7,
    };

    enum class PrimitiveTopology : uint8_t
    {
        UNDEFINED,
        TRIANGLELIST,
        TRIANGLESTRIP,
        POINTLIST,
        LINELIST,
        LINESTRIP,
        PATCHLIST,
    };

    enum class ComparisonFunc : uint8_t
    {
        NEVER, LESS, EQUAL, LESS_EQUAL,
        GREATER, NOT_EQUAL, GREATER_EQUAL, ALWAYS,
        // No comparison (D3D12's 0), for a static sampler that never compares: pipeline layouts that
        // reproduce a hand-filled root signature with that field zero keep the same blob.
        NONE,
    };

    enum class DepthWriteMask : uint8_t { ZERO, ALL };

    enum class StencilOp : uint8_t
    {
        KEEP, ZERO, REPLACE, INCR_SAT,
        DECR_SAT, INVERT, INCR, DECR,
    };

    enum class Blend : uint8_t
    {
        ZERO, ONE,
        SRC_COLOR, INV_SRC_COLOR,
        SRC_ALPHA, INV_SRC_ALPHA,
        DEST_ALPHA, INV_DEST_ALPHA,
        DEST_COLOR, INV_DEST_COLOR,
        SRC_ALPHA_SAT,
        BLEND_FACTOR, INV_BLEND_FACTOR,
        SRC1_COLOR, INV_SRC1_COLOR,
        SRC1_ALPHA, INV_SRC1_ALPHA,
    };

    enum class BlendOp : uint8_t { ADD, SUBTRACT, REV_SUBTRACT, MIN, MAX };
    enum class FillMode : uint8_t { WIREFRAME, SOLID };
    enum class CullMode : uint8_t { NONE, FRONT, BACK };

    enum class InputClassification : uint8_t
    {
        PER_VERTEX_DATA,
        PER_INSTANCE_DATA,
    };

    enum class Usage : uint8_t
    {
        DEFAULT,    // GPU read/write, CPU no access
        UPLOAD,     // CPU write, GPU read (staging)
        READBACK,   // CPU read, GPU write
    };

    enum class TextureAddressMode : uint8_t
    {
        WRAP, MIRROR, CLAMP, BORDER, MIRROR_ONCE,
    };

    enum class Filter : uint8_t
    {
        MIN_MAG_MIP_POINT,
        MIN_MAG_POINT_MIP_LINEAR,
        MIN_POINT_MAG_LINEAR_MIP_POINT,
        MIN_POINT_MAG_MIP_LINEAR,
        MIN_LINEAR_MAG_MIP_POINT,
        MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MIN_MAG_LINEAR_MIP_POINT,
        MIN_MAG_MIP_LINEAR,
        ANISOTROPIC,
        COMPARISON_MIN_MAG_MIP_POINT,
        COMPARISON_MIN_MAG_POINT_MIP_LINEAR,
        COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT,
        COMPARISON_MIN_POINT_MAG_MIP_LINEAR,
        COMPARISON_MIN_LINEAR_MAG_MIP_POINT,
        COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        COMPARISON_MIN_MAG_MIP_LINEAR,
        COMPARISON_ANISOTROPIC,
        MINIMUM_MIN_MAG_MIP_POINT,
        MINIMUM_MIN_MAG_POINT_MIP_LINEAR,
        MINIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
        MINIMUM_MIN_POINT_MAG_MIP_LINEAR,
        MINIMUM_MIN_LINEAR_MAG_MIP_POINT,
        MINIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MINIMUM_MIN_MAG_LINEAR_MIP_POINT,
        MINIMUM_MIN_MAG_MIP_LINEAR,
        MINIMUM_ANISOTROPIC,
        MAXIMUM_MIN_MAG_MIP_POINT,
        MAXIMUM_MIN_MAG_POINT_MIP_LINEAR,
        MAXIMUM_MIN_POINT_MAG_LINEAR_MIP_POINT,
        MAXIMUM_MIN_POINT_MAG_MIP_LINEAR,
        MAXIMUM_MIN_LINEAR_MAG_MIP_POINT,
        MAXIMUM_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        MAXIMUM_MIN_MAG_LINEAR_MIP_POINT,
        MAXIMUM_MIN_MAG_MIP_LINEAR,
        MAXIMUM_ANISOTROPIC,
    };

    enum class SamplerBorderColor : uint8_t
    {
        TRANSPARENT_BLACK, OPAQUE_BLACK, OPAQUE_WHITE,
    };

    enum class Format : uint8_t
    {
        UNKNOWN,
        R32G32B32A32_FLOAT, R32G32B32A32_UINT, R32G32B32A32_SINT,
        R32G32B32_FLOAT,    R32G32B32_UINT,    R32G32B32_SINT,
        R16G16B16A16_FLOAT, R16G16B16A16_UNORM, R16G16B16A16_UINT,
        R16G16B16A16_SNORM, R16G16B16A16_SINT,
        R32G32_FLOAT, R32G32_UINT, R32G32_SINT,
        D32_FLOAT_S8X24_UINT,
        R10G10B10A2_UNORM, R10G10B10A2_UINT, R11G11B10_FLOAT,
        R8G8B8A8_UNORM, R8G8B8A8_UNORM_SRGB, R8G8B8A8_UINT,
        R8G8B8A8_SNORM, R8G8B8A8_SINT,
        B8G8R8A8_UNORM, B8G8R8A8_UNORM_SRGB,
        R16G16_FLOAT, R16G16_UNORM, R16G16_UINT, R16G16_SNORM, R16G16_SINT,
        D32_FLOAT, R32_FLOAT, R32_UINT, R32_SINT,
        D24_UNORM_S8_UINT, R9G9B9E5_SHAREDEXP,
        R8G8_UNORM, R8G8_UINT, R8G8_SNORM, R8G8_SINT,
        R16_FLOAT, D16_UNORM, R16_UNORM, R16_UINT, R16_SNORM, R16_SINT,
        R8_UNORM, R8_UINT, R8_SNORM, R8_SINT,
        BC1_UNORM, BC1_UNORM_SRGB,
        BC2_UNORM, BC2_UNORM_SRGB,
        BC3_UNORM, BC3_UNORM_SRGB,
        BC4_UNORM, BC4_SNORM,
        BC5_UNORM, BC5_SNORM,
        BC6H_UF16, BC6H_SF16,
        BC7_UNORM, BC7_UNORM_SRGB,
        NV12,
    };

    enum class ColorWrite : uint32_t
    {
        DISABLE      = 0,
        ENABLE_RED   = 1 << 0,
        ENABLE_GREEN = 1 << 1,
        ENABLE_BLUE  = 1 << 2,
        ENABLE_ALPHA = 1 << 3,
        ENABLE_ALL   = ~0u,
    };

    // ==== Flag enums (bitwise operators |, &, ^, ~, |=, &=, ^=) ====

    enum class BindFlag : uint32_t
    {
        NONE             = 0,
        VERTEX_BUFFER    = 1 << 0,
        INDEX_BUFFER     = 1 << 1,
        CONSTANT_BUFFER  = 1 << 2,
        SHADER_RESOURCE  = 1 << 3,
        RENDER_TARGET    = 1 << 4,
        DEPTH_STENCIL    = 1 << 5,
        UNORDERED_ACCESS = 1 << 6,
        SHADING_RATE     = 1 << 7,
    };

    enum class ResourceState : uint32_t
    {
        UNDEFINED                         = 0,
        SHADER_RESOURCE                   = 1 << 0,
        SHADER_RESOURCE_COMPUTE           = 1 << 1,
        UNORDERED_ACCESS                  = 1 << 2,
        COPY_SRC                          = 1 << 3,
        COPY_DST                          = 1 << 4,
        RENDERTARGET                      = 1 << 5,
        DEPTHSTENCIL                      = 1 << 6,
        DEPTHSTENCIL_READONLY             = 1 << 7,
        SHADING_RATE_SOURCE               = 1 << 8,
        VERTEX_BUFFER                     = 1 << 9,
        INDEX_BUFFER                      = 1 << 10,
        CONSTANT_BUFFER                   = 1 << 11,
        INDIRECT_ARGUMENT                 = 1 << 12,
        RAYTRACING_ACCELERATION_STRUCTURE = 1 << 13,
        PREDICATION                       = 1 << 14,
        VIDEO_DECODE_SRC                  = 1 << 15,
        VIDEO_DECODE_DST                  = 1 << 16,
        VIDEO_DECODE_DPB                  = 1 << 17,
        SWAPCHAIN                         = 1 << 18,
        DEPTH_READ_SRV                    = 1 << 19,  // depth read + pixel shader SRV simultaneously
    };

    enum class ResourceMiscFlag : uint32_t
    {
        NONE                          = 0,
        TEXTURECUBE                   = 1 << 0,
        INDIRECT_ARGS                 = 1 << 1,
        BUFFER_RAW                    = 1 << 2,
        BUFFER_STRUCTURED             = 1 << 3,
        RAY_TRACING                   = 1 << 4,
        PREDICATION                   = 1 << 5,
        TRANSIENT_ATTACHMENT          = 1 << 6,
        SPARSE                        = 1 << 7,
        ALIASING_BUFFER               = 1 << 8,
        ALIASING_TEXTURE_NON_RT_DS    = 1 << 9,
        ALIASING_TEXTURE_RT_DS        = 1 << 10,
        ALIASING                      = ALIASING_BUFFER | ALIASING_TEXTURE_NON_RT_DS | ALIASING_TEXTURE_RT_DS,
        TYPED_FORMAT_CASTING          = 1 << 11,
        TYPELESS_FORMAT_CASTING       = 1 << 12,
        VIDEO_DECODE                  = 1 << 13,
        VIDEO_DECODE_OUTPUT_ONLY      = 1 << 14,
        VIDEO_DECODE_DPB_ONLY         = 1 << 15,
        NO_DEFAULT_DESCRIPTORS        = 1 << 16,
        SHARED                        = 1 << 17,
        VIDEO_COMPATIBILITY_H264      = 1 << 18,
        VIDEO_COMPATIBILITY_H265      = 1 << 19,
        // A TYPED buffer view (HLSL Buffer<T> / RWBuffer<T>): the SRV and the UAV use
        // GPUBufferDesc::format, one element per texel of it, no structure stride. Opt-in: without it
        // a format reaches only the SRV, as for every other buffer.
        BUFFER_TYPED                  = 1 << 20,
        // The buffer can be cleared with IGraphicsDevice::ClearUavUint although its UAV is structured.
        // D3D12's ClearUnorderedAccessView* takes only a raw or typed view, so the device makes a raw
        // (R32_TYPELESS) one beside it, used only to clear.
        UAV_CLEARABLE                 = 1 << 21,
    };

    // A pipeline layout's root signature flags (PipelineLayoutDesc).
    enum class LayoutFlags : uint32_t
    {
        NONE                   = 0,
        INPUT_ASSEMBLER        = 1 << 0,   // a graphics layout read through an input layout
        HEAP_INDEXED_RESOURCES = 1 << 1,   // ResourceDescriptorHeap[] (SM 6.6)
        HEAP_INDEXED_SAMPLERS  = 1 << 2,   // SamplerDescriptorHeap[]
    };

    // An acceleration-structure build's flags (D3D12's values). A prebuild query must carry the
    // flags its build will.
    enum class ASBuildFlags : uint32_t
    {
        NONE              = 0,
        ALLOW_UPDATE      = 1 << 0,   // a later build may refit it (PERFORM_UPDATE)
        ALLOW_COMPACTION  = 1 << 1,   // QueryCompactedSize + CompactAccelerationStructure
        PREFER_FAST_TRACE = 1 << 2,
        PREFER_FAST_BUILD = 1 << 3,
        PERFORM_UPDATE    = 1 << 5,   // refit in place: the source is the destination
    };

#define RHI_DEFINE_ENUM_BITOPS(E)                                                                          \
    inline constexpr E  operator| (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b)); } \
    inline constexpr E  operator& (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b)); } \
    inline constexpr E  operator^ (E a, E b)  noexcept { return static_cast<E>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b)); } \
    inline constexpr E  operator~ (E a)       noexcept { return static_cast<E>(~static_cast<uint32_t>(a)); }                           \
    inline constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }                                                          \
    inline constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }                                                          \
    inline constexpr E& operator^=(E& a, E b) noexcept { return a = a ^ b; }

    RHI_DEFINE_ENUM_BITOPS(BindFlag)
    RHI_DEFINE_ENUM_BITOPS(ResourceState)
    RHI_DEFINE_ENUM_BITOPS(ResourceMiscFlag)
    RHI_DEFINE_ENUM_BITOPS(LayoutFlags)
    RHI_DEFINE_ENUM_BITOPS(ASBuildFlags)

#undef RHI_DEFINE_ENUM_BITOPS

    // True when flags and test share any bit.
    inline constexpr bool HasFlag(BindFlag     flags, BindFlag     test) noexcept { return (flags & test) != BindFlag::NONE; }
    inline constexpr bool HasFlag(ResourceState flags, ResourceState test) noexcept { return (flags & test) != ResourceState::UNDEFINED; }
    inline constexpr bool HasFlag(ResourceMiscFlag flags, ResourceMiscFlag test) noexcept { return (flags & test) != ResourceMiscFlag::NONE; }
    inline constexpr bool HasFlag(LayoutFlags flags, LayoutFlags test) noexcept { return (flags & test) != LayoutFlags::NONE; }
    inline constexpr bool HasFlag(ASBuildFlags flags, ASBuildFlags test) noexcept { return (flags & test) != ASBuildFlags::NONE; }

    // ==== Descriptor structs ====

    struct Viewport
    {
        float topLeftX = 0;
        float topLeftY = 0;
        float width      = 0;
        float height     = 0;
        float minDepth  = 0;
        float maxDepth  = 1;
    };

    struct InputLayout
    {
        static constexpr uint32_t kAppendAlignedElement = ~0u;
        struct Element
        {
            // Not owned: must point to a string that outlives the pipeline build (e.g. the literal
            // "POSITION"); nothing is heap-allocated at pipeline-build time.
            const char*         semanticName     = nullptr;
            uint32_t            semanticIndex    = 0;
            Format              format            = Format::UNKNOWN;
            uint32_t            inputSlot        = 0;
            uint32_t            alignedByteOffset = kAppendAlignedElement;
            InputClassification inputSlotClass  = InputClassification::PER_VERTEX_DATA;
        };
        std::vector<Element> elements;
    };

    // ==== ClearValue ====
    // Static factories keep the color and depth/stencil union members apart.
    union ClearValue
    {
        float color[4];
        struct ClearDepthStencil { float depth; uint32_t stencil; } depthStencil;

        static ClearValue Color(float r, float g, float b, float a = 1.0f) noexcept
        {
            ClearValue v{};
            v.color[0] = r; v.color[1] = g; v.color[2] = b; v.color[3] = a;
            return v;
        }

        // Reversed-Z convention: near plane is z=1, far plane is z=0. Clearing
        // to 0.0 means "start every pixel at the far plane"; subsequent draws
        // that are closer pass the GREATER_EQUAL depth test and overwrite it.
        static ClearValue DepthStencil(float depth = 0.0f, uint32_t stencil = 0) noexcept
        {
            ClearValue v{};
            v.depthStencil.depth   = depth;
            v.depthStencil.stencil = stencil;
            return v;
        }
    };

    struct TextureDesc
    {
        enum class Type : uint8_t { TEXTURE_1D, TEXTURE_2D, TEXTURE_3D } type = Type::TEXTURE_2D;
        Format           format       = Format::UNKNOWN;
        Usage            usage        = Usage::DEFAULT;
        BindFlag         bindFlags   = BindFlag::NONE;
        uint32_t         width        = 1;
        uint32_t         height       = 1;
        uint32_t         depth        = 1;
        uint32_t         arraySize   = 1;
        uint32_t         mipLevels   = 1;
        uint32_t         sampleCount = 1;
        ClearValue       clear        = {};
        ResourceMiscFlag miscFlags   = ResourceMiscFlag::NONE;
        ResourceState    layout       = ResourceState::SHADER_RESOURCE;
        // UTF-8. CreateTexture passes it to ID3D12Resource::SetName, so validation messages and PIX /
        // RenderDoc captures name the resource. Needed only during the call (SetName copies it).
        const char*      debugName   = nullptr;
    };

    struct SamplerDesc
    {
        Filter             filter          = Filter::MIN_MAG_MIP_LINEAR;
        TextureAddressMode addressU       = TextureAddressMode::WRAP;
        TextureAddressMode addressV       = TextureAddressMode::WRAP;
        TextureAddressMode addressW       = TextureAddressMode::WRAP;
        float              mipLodBias    = 0;
        uint32_t           maxAnisotropy  = 0;
        ComparisonFunc     comparisonFunc = ComparisonFunc::NEVER;
        SamplerBorderColor borderColor    = SamplerBorderColor::TRANSPARENT_BLACK;
        float              minLod         = 0;
        float              maxLod         = (std::numeric_limits<float>::max)();
    };

    struct RasterizerState
    {
        FillMode fillMode                         = FillMode::SOLID;
        CullMode cullMode                         = CullMode::BACK;
        bool     frontCounterClockwise           = false;
        int32_t  depthBias                        = 0;
        float    depthBiasClamp                  = 0;
        float    slopeScaledDepthBias           = 0;
        bool     depthClipEnable                 = true;
        bool     multisampleEnable                = false;
        bool     antialiasedLineEnable           = false;
        bool     conservativeRasterizationEnable = false;
        // No ForcedSampleCount (target-independent rasterization): add it only together with its D3D12
        // translation and its constraint (DepthEnable == StencilEnable == FALSE).
    };
    inline static constexpr RasterizerState kDefaultRasterizerState;

    struct DepthStencilState
    {
        struct DepthStencilOp
        {
            StencilOp      stencilFailOp       = StencilOp::KEEP;
            StencilOp      stencilDepthFailOp = StencilOp::KEEP;
            StencilOp      stencilPassOp       = StencilOp::KEEP;
            ComparisonFunc stencilFunc          = ComparisonFunc::ALWAYS;
        };

        bool           depthEnable       = false;
        DepthWriteMask depthWriteMask   = DepthWriteMask::ZERO;
        ComparisonFunc depthFunc         = ComparisonFunc::LESS;
        bool           stencilEnable     = false;
        uint8_t        stencilReadMask  = 0xff;
        uint8_t        stencilWriteMask = 0xff;

        DepthStencilOp frontFace;
        DepthStencilOp backFace;
        // No depth-bounds test: D3D12_DEPTH_STENCIL_DESC, which the graphics PSO path and its
        // ID3D12PipelineLibrary warm start are built on, cannot carry it. Adding it needs DESC1 on both
        // PSO paths, ID3D12PipelineLibrary1, an OPTIONS2 feature gate and an OMSetDepthBounds RHI call.
    };
    inline static constexpr DepthStencilState kDefaultDepthStencilState;

    struct BlendState
    {
        struct RenderTargetBlendState
        {
            bool      blendEnable              = false;
            Blend     srcBlend                 = Blend::SRC_ALPHA;
            Blend     destBlend                = Blend::INV_SRC_ALPHA;
            BlendOp   blendOp                  = BlendOp::ADD;
            Blend     srcBlendAlpha           = Blend::ONE;
            Blend     destBlendAlpha          = Blend::ONE;
            BlendOp   blendOpAlpha            = BlendOp::ADD;
            ColorWrite renderTargetWriteMask = ColorWrite::ENABLE_ALL;
        };

        bool alphaToCoverageEnable = false;
        bool independentBlendEnable = false;

        RenderTargetBlendState renderTarget[8];
    };
    inline static constexpr BlendState kDefaultBlendState;

    struct GPUBufferDesc
    {
        uint64_t         size       = 0;
        uint32_t         stride     = 0;     // for structured buffers
        uint32_t         alignment  = 0;
        Usage            usage      = Usage::DEFAULT;
        Format           format     = Format::UNKNOWN;
        BindFlag         bindFlags = BindFlag::NONE;
        ResourceMiscFlag miscFlags = ResourceMiscFlag::NONE;
        // The D3D12 object name and the GPU memory report's label (Profiler.MemoryTop); both copy it, so
        // it need not outlive the call.
        const char*      debugName = nullptr;
    };

    // ==== PVF (programmable vertex fetching) types ====

    // Sentinel value: no buffer / stream absent
    static constexpr uint32_t kInvalidBufferIndex = 0xFFFFFFFFu;

    // Per-attribute format; the values must never change (stored in MeshDescriptor on the GPU).
    enum class VertexFormat : uint32_t
    {
        Float3      = 0,   // float3: positions, normals
        Float2      = 1,   // float2: UVs (full precision)
        Half2       = 2,   // float16 x2: UVs (packed)
        R10G10B10A2 = 3,   // packed normal / tangent
        R8G8B8A8    = 4,   // vertex color
        Float4      = 5,   // float4: tangent with bitangent sign
        Invalid     = 0xFFFFFFFFu,
    };

    // One vertex attribute stream (16 bytes) and one mesh descriptor slot in the global GPU
    // StructuredBuffer<MeshDescriptor> (160 bytes; prevPosition, the one stream into another
    // frame's buffer, is documented there): defined ONCE for C++ and the shaders in
    // shaders/shared/mesh_descriptor.hlsli, which asserts the layout. The C++ defaults it spells (an
    // absent stream, an Invalid format) are these constants.
    using StreamDescriptor = GpuShared::StreamDesc;
    using MeshDescriptor   = GpuShared::MeshDescriptor;
    static_assert(kInvalidBufferIndex == 0xFFFFFFFFu &&
                  static_cast<uint32_t>(VertexFormat::Invalid) == 0xFFFFFFFFu,
                  "shaders/shared/mesh_descriptor.hlsli spells these defaults as 0xFFFFFFFF");


    // ==== GPU resource handles ====
    // Lightweight value types (no heap allocation). handleId is an opaque index into the backend's
    // per-type pool, ~0u = invalid / not created; the backend fills it on creation. Derived types
    // carry a copy of the creation descriptor for convenience; the authoritative data lives in the
    // backend pool.

    struct GPUResource
    {
        // Discriminates which backend pool owns this handle.
        // Set by the backend on creation; lets generic GPUResource* parameters
        // (e.g. GPUBarrier::Memory) resolve to the correct pool without dynamic_cast.
        enum class Type : uint8_t { Unknown, Buffer, Texture, Shader, PipelineState, PipelineLayout,
                                    DescriptorBlock, AccelerationStructure, RaytracingPipeline };

        uint32_t handleId = ~0u;
        Type     type      = Type::Unknown;

        constexpr bool IsValid() const noexcept { return handleId != ~0u; }
        void Reset() noexcept { handleId = ~0u; type = Type::Unknown; }
    };

    struct GPUBuffer : GPUResource
    {
        GPUBufferDesc desc{};
    };

    struct Texture : GPUResource
    {
        TextureDesc desc{};
    };

    struct Shader : GPUResource
    {
        ShaderStage stage = ShaderStage::Count;
    };

    struct PipelineState : GPUResource
    {
        // All pipeline state lives in the backend pool (PipelineStateDX12 etc.)
    };

    // ==== Pipeline layout ====
    // The root signature a PSO binds with, in RHI terms: root parameter i is params[i], a table's
    // ranges are ranges[firstRange ..]. Graphics/PipelineLayoutBuilder.h assembles one by typed
    // slot. The device translates it field for field, so a layout serializes to exactly the blob of
    // the equivalent hand-filled D3D12 description.
    enum class ShaderVisibility : uint8_t { ALL, VS, HS, DS, GS, PS, AS, MS };

    struct DescriptorRange
    {
        enum class Type : uint8_t { SRV, UAV, CBV };

        static constexpr uint32_t kAppend = 0xFFFFFFFFu;

        Type     type         = Type::SRV;
        uint32_t baseRegister = 0;
        uint32_t space        = 0;
        uint32_t count        = 1;
        // Descriptors from the table's start; kAppend = right after the previous range.
        uint32_t tableOffset  = 0;
    };

    struct LayoutParam
    {
        enum class Kind : uint8_t { CONSTANTS, CBV, SRV, UAV, TABLE };
        Kind             kind           = Kind::CBV;
        uint32_t         reg            = 0;   // CONSTANTS / CBV / SRV / UAV
        uint32_t         space          = 0;
        uint32_t         num32BitValues = 0;   // CONSTANTS
        uint32_t         firstRange     = 0;   // TABLE
        uint32_t         rangeCount     = 0;
        ShaderVisibility visibility     = ShaderVisibility::ALL;
    };

    struct StaticSampler
    {
        SamplerDesc      desc;
        uint32_t         reg        = 0;
        uint32_t         space      = 0;
        ShaderVisibility visibility = ShaderVisibility::ALL;
    };

    struct PipelineLayoutDesc
    {
        const char*            name         = nullptr;   // the [PIPELINE_LAYOUT] line; the root signature's name
        // Null with paramCount > 0 = an incomplete list (PipelineLayoutBuilder:
        // a slot given twice or skipped); the device refuses it.
        const LayoutParam*     params       = nullptr;
        uint32_t               paramCount   = 0;
        const DescriptorRange* ranges       = nullptr;
        uint32_t               rangeCount   = 0;
        const StaticSampler*   samplers     = nullptr;
        uint32_t               samplerCount = 0;
        LayoutFlags            flags        = LayoutFlags::NONE;
    };

    struct PipelineLayout : GPUResource
    {
        // The root signature lives in the backend pool (PipelineLayoutDX12).
    };

    // ==== Descriptor block ====
    // Consecutive descriptors of the shader-visible heap's static region
    // (IGraphicsDevice::CreateNullStructuredSrvBlock, AllocateDescriptorBlock). The allocation stays
    // in the backend's pool; descriptor i's GPU handle is Gpu(i), its CPU handle Cpu(i) - the
    // shader-visible heap's CPU side, a copy destination only (CopyCbvSrvUavDescriptors), never a
    // source.
    struct DescriptorBlock : GPUResource
    {
        uint64_t gpuBase   = 0;
        uint64_t cpuBase   = 0;
        uint32_t increment = 0;   // bytes between consecutive descriptors
        uint32_t count     = 0;
        uint64_t Gpu(uint32_t i) const noexcept { return gpuBase + static_cast<uint64_t>(i) * increment; }
        uint64_t Cpu(uint32_t i) const noexcept { return cpuBase + static_cast<uint64_t>(i) * increment; }
    };

    // Descriptor-heap telemetry, one per managed heap. 'used' is a monotonic high-water cursor: it
    // proves boundedness, never recycling; 'freeBlocks' is the recycling signal; 'capacity' is the
    // effective ceiling the driver granted.
    struct DescriptorHeapStats
    {
        uint32_t           used       = 0;   // monotonic high-water cursor
        uint32_t           capacity   = 0;   // effective ceiling, as granted
        uint32_t           freeBlocks = 0;   // uncoalesced free-list blocks
        unsigned long long exhausted  = 0;   // failed AllocateStatic calls
    };

    // ==== Ray-tracing acceleration structures ====
    // Every ray is an inline RayQuery against the scene TLAS, which a pass binds as a root SRV by
    // its address. The backend pools the memory: a RESULT holds a built BLAS / TLAS, a SCRATCH is a
    // build's working memory - both DEFAULT and UAV-capable, never zero-filled (a build writes
    // before anything reads). A build reads geometry and instance records from ordinary buffers by
    // address.
    enum class ASMemory : uint8_t { RESULT, SCRATCH };

    struct AccelerationStructure : GPUResource
    {
        uint64_t sizeBytes = 0;
        uint64_t address   = 0;   // GPU address, fixed for the object's life (0 = none)
        ASMemory memory    = ASMemory::RESULT;
    };

    // One triangle geometry of a BLAS: positions are 3 floats, positionStride apart.
    struct ASTriangles
    {
        uint64_t positionAddress = 0;   // vertex 0's position
        uint32_t positionStride  = 0;   // bytes between vertices
        uint32_t vertexCount     = 0;
        uint64_t indexAddress    = 0;   // 0 = a non-indexed triangle list
        uint32_t indexCount      = 0;
        Format   indexFormat     = Format::R16_UINT;   // R16_UINT or R32_UINT
        bool     opaque          = true;               // no any-hit
    };

    // What a build needs, each a multiple of kASAlignment. updateScratchSize is 0
    // unless the query carried ALLOW_UPDATE - and 0 then means "not refittable".
    struct ASPrebuildInfo
    {
        uint64_t resultSize        = 0;
        uint64_t scratchSize       = 0;
        uint64_t updateScratchSize = 0;
    };

    inline constexpr uint64_t kASAlignment = 256;   // results, scratch sizes and offsets

    // One TLAS instance as the build reads it: 64 bytes, the record
    // D3D12_RAYTRACING_INSTANCE_DESC and VkAccelerationStructureInstanceKHR share.
    struct ASInstance
    {
        float    transform[3][4];        // row-major 3x4: world = transform * (p, 1)
        uint32_t instanceID    : 24;     // the shader's InstanceID()
        uint32_t instanceMask  : 8;
        uint32_t hitGroupIndex : 24;
        uint32_t flags         : 8;      // ASInstanceFlag
        uint64_t blasAddress;
    };
    static_assert(sizeof(ASInstance) == 64, "ASInstance is the APIs' 64-byte instance record");

    namespace ASInstanceFlag
    {
        inline constexpr uint32_t TRIANGLE_CULL_DISABLE = 0x1;
        inline constexpr uint32_t TRIANGLE_FRONT_CCW    = 0x2;
        inline constexpr uint32_t FORCE_OPAQUE          = 0x4;
        inline constexpr uint32_t FORCE_NON_OPAQUE      = 0x8;
    }

    // ==== DXR ray-tracing pipeline ====
    // Every ray the engine traces is an inline RayQuery in a compute pass; this state-object path is
    // kept working and proved by the -dxr_parity self-test (Renderer/DXRParityTest), not used. One
    // DXIL library, one ray-generation shader, miss shaders, triangle hit groups; the global root
    // signature is a PipelineLayout, and there are no local root arguments.
    struct HitGroup
    {
        const char* name       = nullptr;   // the hit group's export name
        const char* closestHit = nullptr;
        const char* anyHit     = nullptr;   // optional
    };

    struct RaytracingPipelineDesc
    {
        const Shader*         library           = nullptr;   // ShaderStage::LIB
        const char*           rayGen            = nullptr;
        const char* const*    miss              = nullptr;   // miss shader i = TraceRay's MissShaderIndex i
        uint32_t              missCount         = 0;
        const HitGroup*       hitGroups         = nullptr;   // record i = hit-group index i
        uint32_t              hitGroupCount     = 0;
        uint32_t              maxPayloadBytes   = 16;
        uint32_t              maxAttributeBytes = 8;         // the triangle barycentrics
        uint32_t              maxRecursion      = 1;
        const PipelineLayout* layout            = nullptr;   // the global root signature (required)
        const char*           name              = nullptr;   // the [RT_PIPELINE] line
    };

    struct RaytracingPipeline : GPUResource
    {
        // The state object and its shader table live in the backend pool.
    };

    // ==== PipelineStateDesc ====
    // References Shader handles and, through `layout`, the root signature: null = the shared compute
    // root signature for a compute PSO, the shared graphics one otherwise (RootSignatureLayout.h).
    struct PipelineStateDesc
    {
        const Shader*          vs  = nullptr;
        const Shader*          ps  = nullptr;
        const Shader*          hs  = nullptr;
        const Shader*          ds  = nullptr;
        const Shader*          gs  = nullptr;
        const Shader*          ms  = nullptr;   // mesh shader
        const Shader*          as  = nullptr;   // amplification shader
        const Shader*          cs  = nullptr;   // compute shader
        const BlendState*      bs  = nullptr;
        const RasterizerState* rs  = nullptr;
        const DepthStencilState* dss = nullptr;
        const InputLayout*     il  = nullptr;
        PrimitiveTopology      pt  = PrimitiveTopology::TRIANGLELIST;
        uint32_t               patchControlPoints = 3;
        uint32_t               sampleMask          = 0xFFFFFFFF;
        // Render target formats (up to 8 colour + 1 depth)
        Format                 rtvFormats[8] = { Format::R8G8B8A8_UNORM };
        uint32_t               rtvCount      = 1;
        Format                 dsvFormat     = Format::UNKNOWN;
        uint32_t               sampleCount   = 1;
        // Set by PSOCache::CreateNew from PSODesc::Hash() (ShaderID-based, deterministic across runs);
        // 0 = not set. ComputePSOName mixes it in only as a seed of the PSO disk-library name, beside the
        // stage bytecode, the layout and the render state.
        uint64_t               cacheKey      = 0;
        // The pipeline layout (root signature) this PSO binds with; null = the shared one. Last, so
        // positional initializers keep their meaning.
        const PipelineLayout*  layout         = nullptr;
        // Load it from the PSO library or fail: no driver build (PSOCache defers a miss while drawing).
        bool                   libraryOnly    = false;
    };

    // ==== GPU barriers ====
    struct GPUBarrier
    {
        struct Memory   { const GPUResource* resource; };
        struct Image
        {
            const Texture* texture;
            ResourceState  layoutBefore;
            ResourceState  layoutAfter;
            int            mip;
            int            slice;
            // One plane of a depth-stencil texture (0 depth, 1 stencil; every mip and slice of it), -1
            // every plane.
            int            plane;
        };
        struct Buffer
        {
            const GPUBuffer* buffer;
            ResourceState    stateBefore;
            ResourceState    stateAfter;
        };
        struct Aliasing
        {
            const GPUResource* resourceBefore;
            const GPUResource* resourceAfter;
        };

        enum class Type { MEMORY, IMAGE, BUFFER, ALIASING } type = Type::MEMORY;

        union { Memory memory; Image image; Buffer buffer; Aliasing aliasing; };

        GPUBarrier() {}

        static GPUBarrier Memory(const GPUResource* res = nullptr) noexcept
        {
            GPUBarrier b; b.type = Type::MEMORY; b.memory.resource = res; return b;
        }
        static GPUBarrier Image(const Texture* tex,
            ResourceState before, ResourceState after,
            int mip = -1, int slice = -1, int plane = -1) noexcept
        {
            GPUBarrier b; b.type = Type::IMAGE;
            b.image = { tex, before, after, mip, slice, plane }; return b;
        }
        // One plane of a depth-stencil texture.
        static GPUBarrier ImagePlane(const Texture* tex, int plane,
            ResourceState before, ResourceState after) noexcept
        {
            return Image(tex, before, after, -1, -1, plane);
        }
        static GPUBarrier Buffer(const GPUBuffer* buf,
            ResourceState before, ResourceState after) noexcept
        {
            GPUBarrier b; b.type = Type::BUFFER;
            b.buffer = { buf, before, after }; return b;
        }
        static GPUBarrier Aliasing(const GPUResource* before,
                                   const GPUResource* after) noexcept
        {
            GPUBarrier b; b.type = Type::ALIASING;
            b.aliasing = { before, after }; return b;
        }
    };

    // A depth-stencil format: two planes, depth (0) and stencil (1), each with its own resource
    // state.
    inline bool HasStencilPlane(Format f)
    {
        return f == Format::D32_FLOAT_S8X24_UINT || f == Format::D24_UNORM_S8_UINT;
    }

    // How a pass binds a depth texture's DSV (IGraphicsDevice::SetRenderTargetToHdrWithDepth /
    // SetRenderTargets / SetRenderTargetsAndHdr). readOnly* pick the DSV's D3D12_DSV_FLAG_READ_ONLY_*
    // variant: a plane in a DEPTH_READ state is bound read-only, one in DEPTH_WRITE may be bound
    // writable (that state permits the test too). mayWrite* is what the pass declared it writes,
    // stencilWriteMask the stencil bits it declared (RenderGraph/StencilBits.h; moot while
    // mayWriteStencil is false); on every draw the device compares them with the bound pipeline's
    // depth / stencil writes and reports a pipeline that writes a plane declared read-only, or
    // stencil bits outside the mask. The default is a writable DSV with nothing checked.
    struct DepthView
    {
        bool    readOnlyDepth    = false;
        bool    readOnlyStencil  = false;
        bool    mayWriteDepth    = true;
        bool    mayWriteStencil  = true;
        uint8_t stencilWriteMask = 0xFF;
    };

    struct SubresourceData
    {
        const void* dataPtr    = nullptr;
        uint32_t    rowPitch   = 0;
        uint32_t    slicePitch = 0;
    };

    // ==== Queue type and command list handle ====

    enum class QueueType : uint8_t
    {
        GRAPHICS = 0,   // D3D12_COMMAND_LIST_TYPE_DIRECT
        COMPUTE  = 1,   // D3D12_COMMAND_LIST_TYPE_COMPUTE
        COPY     = 2,   // D3D12_COMMAND_LIST_TYPE_COPY
        COUNT    = 3,
    };

} // namespace RHI

// ==== RenderGraph virtual texture types ====
// Defined here so CommandList can use RGTextureHandle without a circular include through
// the render graph headers.
namespace RG
{
    struct RGTextureHandle
    {
        static constexpr uint32_t kInvalidId = ~0u;
        uint32_t id = kInvalidId;
        bool IsValid() const noexcept { return id != kInvalidId; }
        bool operator==(const RGTextureHandle& o) const noexcept { return id == o.id; }
        bool operator!=(const RGTextureHandle& o) const noexcept { return id != o.id; }
    };

    struct RGTextureDesc
    {
        RHI::Format    format    = RHI::Format::UNKNOWN;
        uint32_t       width     = 0;   // 0 = match render dimensions
        uint32_t       height    = 0;
        bool           isDepth   = false;
        // Set when any pass binds this texture as a UAV (e.g. the decal apply pass writes the GBuffer in
        // place): the physical resource gets BindFlag::UNORDERED_ACCESS and a UAV descriptor.
        bool           isUAV     = false;
        // No debugName here on purpose: RenderGraph::CreateTexture's name argument becomes
        // TextureDesc::debugName, which the backend passes to ID3D12Resource::SetName.
        // isSRV: set when this texture is read as an SRV from outside the graph (e.g. TAA / XeGTAO / SSR
        // read GBuffer velocity through GetTextureSRVGpuHandle, not a graph-declared read). Forces
        // BindFlag::SHADER_RESOURCE and an SRV descriptor although no in-graph pass declares the read.
        bool           isSRV     = false;
        uint32_t       arraySize = 1;
    };

    // Named graph-shared CB / buffer slots: passes bind through these, so the per-frame path is an
    // array index rather than a string hash; the string APIs are thin shims over RGSlotFromName
    // (RenderContext.h), the single string -> slot map.
    // Add new slots at the end and mirror the name in kRGSlotNames (RenderContext.cpp): the two
    // lists are index-paired.
    enum class RGSlot : uint8_t
    {
        // Constant buffers (RenderGraph::BindConstantBuffer -> RenderContext::GetCB). A CB that one pass
        // writes and only that pass reads is not a slot: it is a block of the frame's constants
        // (cl.AllocConstants<T>()).
        PerView,
        LightCB,
        TerrainParams,
        SkyCB,

        // Root-SRV buffers (RenderGraph::BindBuffer -> RenderContext::GetBuffer)
        InstanceBuffer,
        MeshDescriptors,
        MaterialBuffer,

        // The main view's MeshletCullCB (b4).
        MeshletCull,

        // The fog the opaque scene got, for everything drawn after the fog composites
        // (Graphics/FogApply.h, fog_apply.hlsli, b4).
        FogApply,

        Count   // also the "no such name" sentinel returned by RGSlotFromName
    };
} // namespace RG

// ==== CommandList: unified handle + recording interface ====
// IGraphicsDevice::BeginCommandList returns one with only internalId set (gfx / ctx null);
// RenderGraph::Execute attaches gfx and ctx before handing it to each pass. IGraphicsDevice's
// virtual methods take CommandList as an opaque handle.
namespace RHI
{
    // One draw's or dispatch's constants in this frame's upload memory
    // (Graphics/TransientConstants.h): write *cpu, bind gpu. Empty when the frame's constants ran out
    // (reported).
    template <class T>
    struct Constants
    {
        explicit operator bool() const { return cpu != nullptr; }

        T*       cpu = nullptr;
        uint64_t gpu = 0;
    };

    struct CommandList
    {
        uint32_t           internalId = ~0u;
        IGraphicsDevice*   gfx         = nullptr;  // non-owning; set by RenderGraph
        RG::RenderContext* ctx         = nullptr;  // non-owning; set by RenderGraph

        constexpr bool IsValid() const noexcept { return internalId != ~0u; }
        bool operator==(const CommandList& o) const noexcept { return internalId == o.internalId; }

        // ---- Device / context accessors ----
        IGraphicsDevice&   GetDevice()  const;
        RG::RenderContext& GetContext() const;

        // ---- Viewport / scissor / topology ----
        void SetViewport(uint32_t width = 0, uint32_t height = 0);
        void SetScissorRect(uint32_t width = 0, uint32_t height = 0);
        void SetPrimitiveTopology(PrimitiveTopology topo = PrimitiveTopology::TRIANGLELIST);

        // ---- Pipeline state ----
        void SetPipelineState(const PipelineState& pso);

        // ---- Render targets - RG virtual-handle variants ----
        void SetRenderTarget(RG::RGTextureHandle rtv, RG::RGTextureHandle dsv = {});
        void SetRenderTargets(std::initializer_list<RG::RGTextureHandle> rtvs,
                              RG::RGTextureHandle dsv = {});
        // Like SetRenderTargets, but appends the device-internal HdrSceneColor as the last RT
        // (RT[rtvs.size()]): GBufferPass / TerrainPass write emissive straight into scene color.
        void SetRenderTargetsAndHdr(std::initializer_list<RG::RGTextureHandle> rtvs,
                                    RG::RGTextureHandle dsv = {});
        void ClearRenderTarget(RG::RGTextureHandle h, const float color[4]);
        void ClearDepthStencil(RG::RGTextureHandle h,
                               float depth = 0.0f, uint8_t stencil = 0); // reversed Z: far = 0

        // ---- Render targets - raw RHI::Texture* variants (e.g. PickingPass) ----
        void SetRenderTargets(uint32_t numRTs,
                              const Texture* const* rtvs,
                              const Texture*        dsv);
        void ClearRenderTarget(const Texture& tex, const float color[4]);
        void ClearDepthStencil(const Texture& tex,
                               float depth = 0.0f, uint8_t stencil = 0); // reversed Z: far = 0

        // ---- Descriptor heap / root bindings ----
        void BindDescriptorHeaps();
        // Enum variants are the per-frame path (array index, no hashing); the *ByName twins are shims
        // for names that have no RGSlot yet: do not add new pass code against them. Every slot is typed
        // (RHI::Slots, Graphics/RootSignatureLayout.h) - group-relative GfxCB / GfxSRV or absolute
        // GfxParam - so a wrong kind or a bare number does not compile.
        void BindCB(Slots::GfxCB slot, RG::RGSlot cb);
        void BindCBByName(Slots::GfxCB slot, const char* name);
        // This frame's constants for one draw: AllocConstants<T>() takes the room (defined in
        // Graphics/IGraphicsDevice.h), BindCBAddress binds it at the same group-relative slot BindCB
        // takes.
        template <class T> Constants<T> AllocConstants();
        void BindCBAddress(Slots::GfxCB slot, uint64_t gpuVA);
        void BindSRVByHandle(Slots::GfxSRV slot, RG::RGTextureHandle h);
        // The pass's run-time sampler (IGraphicsDevice::SetDynamicSampler). There is no BindSampler:
        // every other sampler is a fixed sampler-heap slot the shader names.
        void SetDynamicSampler(int samplerIndex);
        void SetPVFRootConstants(uint32_t meshDescIdx,
                                 uint32_t instanceOffset,
                                 uint32_t materialIndex);
        // Root SRV (SetRootBufferSRV), unlike BindBufferSRV, which binds a descriptor; the distinct name
        // keeps that difference visible.
        void BindRootBufferSRV(Slots::GfxParam rootSlot, RG::RGSlot buf);
        void BindBufferSRVByName(Slots::GfxParam rootSlot, const char* name);
        void BindBufferSRV(Slots::GfxSRV slot, const GPUBuffer& buf);
        void BindDescriptorTableHandle(Slots::GfxParam rootSlot, uint64_t gpuHandle);

        // ---- Draw calls ----
        void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount = 1,
                           uint32_t startVertex = 0, uint32_t startInstance = 0);
        void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                  uint32_t startIndex, int32_t baseVertex,
                                  uint32_t startInstance);
        void DrawFullscreenTriangle();

        // Mesh-shader dispatch, forwarded to IGraphicsDevice::DispatchMesh. With an amplification
        // shader bound, the AS receives the (x, y, z) group counts and launches the MS groups itself
        // (DispatchMesh inside the AS).
        void DispatchMesh(uint32_t threadGroupX,
                          uint32_t threadGroupY = 1,
                          uint32_t threadGroupZ = 1);

        // ---- Compute ----
        // Every slot is a Slots::CmpParam: the shared compute root signature's Slots::Cmp, or the
        // constants of a pass's private one. These forward to IGraphicsDevice like the graphics calls.
        void SetComputePipelineState(const PipelineState& pso);
        void BindComputeCB(Slots::CmpParam slot, const GPUBuffer& cb, uint32_t byteOffset = 0);
        // This frame's constants (AllocConstants<T>().gpu); zero binds nothing.
        void BindComputeCBAddress(Slots::CmpParam slot, uint64_t gpuVA);
        // A descriptor table; a zero handle binds nothing.
        void BindComputeTable(Slots::CmpParam slot, uint64_t gpuHandle);
        // Root constants and root SRVs exist only on private signatures.
        void SetComputeRootConstants(Slots::CmpParam slot, uint32_t count,
                                     const void* data, uint32_t destOffset = 0);
        template <class T> void SetComputeRootConstants(Slots::CmpParam slot, const T& values)
        {
            static_assert(std::is_trivially_copyable_v<T> && sizeof(T) % 4 == 0,
                          "root constants are whole 32-bit values");
            SetComputeRootConstants(slot, static_cast<uint32_t>(sizeof(T) / 4), &values, 0);
        }
        // Zero binds a null root SRV, as D3D12 does.
        void BindComputeRootSRV(Slots::CmpParam slot, uint64_t gpuVA);
        void Dispatch(uint32_t threadGroupX, uint32_t threadGroupY = 1, uint32_t threadGroupZ = 1);

        // ---- Barriers / copies ----
        void PushBarrier(const GPUBarrier& barrier);
        // One leg of a round trip that leaves the device's state book alone
        // (IGraphicsDevice::PushBarrierScoped).
        void PushBarrierScoped(const GPUBarrier& barrier);
        void CopyTexturePixelToBuffer(const Texture& src,
                                      uint32_t srcX, uint32_t srcY,
                                      GPUBuffer& dst);

        // ---- Dimension / bindless queries ----
        uint32_t GetWidth()              const;
        uint32_t GetHeight()             const;
        uint64_t GetBindlessTableHandle() const;
    };

} // namespace RHI
