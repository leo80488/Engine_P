#pragma once

// Wraps DXC's IDxcUtils::CreateReflection so the engine can ask "what does this compiled
// shader want bound?" without including <d3d12shader.h> or <dxcapi.h>. The output structs are
// copyable and hold no COM types: safe to cache, serialize or pass across module boundaries.
// ShaderLibrary keeps a Reflection with each compiled shader; material UI and binding
// generation read the resource bindings, cbuffer layouts and struct elements from it.
// Not reflected (add when needed): input / output signatures, compute thread-group sizes,
// function-link information.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ShaderReflect
{

enum class ResourceType : uint8_t
{
    Texture,            // SRV: Texture1D/2D/3D/Cube
    Sampler,
    CBuffer,
    StructuredBuffer,   // SRV: StructuredBuffer<T>
    RWTexture,          // UAV
    RWStructuredBuffer, // UAV
    ByteAddressBuffer,
    Other
};

enum class TextureDim : uint8_t
{
    Unknown,
    Tex1D, Tex1DArray,
    Tex2D, Tex2DArray, Tex2DMS, Tex2DMSArray,
    Tex3D,
    TexCube, TexCubeArray,
    Buffer
};

enum class VarType : uint8_t
{
    Unknown,
    Bool,
    Float, Float2, Float3, Float4,
    Float4x4,
    Int,   Int2,   Int3,   Int4,
    UInt,  UInt2,  UInt3,  UInt4,
    Struct
};

// What an UNASSIGNED custom-material texture slot samples, from a trailing annotation on its
// declaration in the material's own .hlsl:
//   Texture2D _Mask : register(t2, space3);   //@default black @linear
// white (the default) | black | grey | normal: the packer pre-fills the slot with the matching
// renderer-lifetime 1x1 texture. The colour space matters only for grey: `@srgb` is sRGB
// mid-grey (0.5 encoded, ~0.214 as sampled), `@linear` or none is 0.5 as sampled. A slot's
// real texture is decoded by its own format either way.
enum class TextureDefault : uint8_t { White, Black, Grey, Normal };
enum class TextureColorSpace : uint8_t { Unspecified, Linear, SRGB };

// The `@engine <role>` names: what the engine reads out of a custom shader's declarations BY
// MEANING; the material packer copies each into MaterialGPUData's named field. A shader that
// names none keeps the positional convention (the toon ramp at t3, foliage in params 0..2, the
// face shadow in param 2); one that names any - or says `//@engine explicit` on a line of its
// own - gets exactly what it names and nothing positional (Reflection::engineRolesExplicit).
inline constexpr const char* kEngineRoleToonRamp        = "toonRamp";         // a texture
inline constexpr const char* kEngineRoleFoliageTint     = "foliageTint";      // float3
inline constexpr const char* kEngineRoleFoliageWrap     = "foliageWrap";      // scalar
inline constexpr const char* kEngineRoleFoliageStrength = "foliageStrength";  // scalar
inline constexpr const char* kEngineRoleFaceShadow      = "faceShadow";       // (offset, sharpness, strength, uvSet)

struct ResourceBinding
{
    std::string  name;
    ResourceType type       = ResourceType::Other;
    TextureDim   dim        = TextureDim::Unknown;
    uint32_t     bindPoint  = 0;     // register slot (e.g. t0 -> 0)
    uint32_t     bindCount  = 1;     // 0 = unbounded array (Texture2D arr[])
    uint32_t     space      = 0;     // register space
    // Parsed from the HLSL source like the cbuffer annotations below.
    TextureDefault    texDefault = TextureDefault::White;
    TextureColorSpace colorSpace = TextureColorSpace::Unspecified;
    // `@engine <role>`: a texture the ENGINE reads by meaning ("toonRamp") rather than by
    // register. Empty = no role.
    std::string       engineRole;
};

struct CBufferVar
{
    std::string name;
    VarType     type     = VarType::Unknown;
    uint32_t    offset   = 0;        // bytes from start of cbuffer
    uint32_t    size     = 0;        // bytes (single element for arrays)
    uint32_t    elements = 0;        // 0 = scalar/vector, N = array length
    // D3D_SVF_USED from reflection: false = declared but never read by this
    // stage. Param SYNC + inspectors skip unused vars (no widget, no seeded
    // bag entry); the CBV packers still iterate every var (offset-addressed,
    // and the g_Materials mirror's slot index = declaration order must not
    // shift with usage).
    bool        used     = true;

    // ---- Authoring annotations parsed from the HLSL source ----
    // DXC reflection cannot carry cbuffer defaults (HLSL has no cbuffer member initializers), so
    // the engine scans the .hlsl source for a trailing comment on each member:
    //   float4 FaceShadow;  //@default 0.5 2.0 1.0 1.0 @min 0 0 0 0 @max 2 8 1 1 @label "Offset|Sharp|Strength|UVSet"
    // These drive the material's seeded default and the inspector UI; hasDefault == false -> the
    // seed falls back to DefaultForType.
    std::array<float, 4> defaultValue = { 0, 0, 0, 0 };
    bool                 hasDefault   = false;
    std::array<float, 4> minValue     = { 0, 0, 0, 0 };
    std::array<float, 4> maxValue     = { 1, 1, 1, 1 };
    bool                 hasRange     = false;
    std::string          label;       // optional inspector label ('|'-separated per component)
    // Optional inspector SECTION from a trailing `@group "Color"` annotation: the inspector lists
    // params in declaration order and emits a separator header whenever the group changes.
    std::string          group;
    // `@engine <role>`: a parameter the ENGINE reads by meaning rather than by its slot -
    // foliageTint / foliageWrap / foliageStrength / faceShadow. The material packer copies it into
    // MaterialGPUData's named field. Empty = no role.
    std::string          engineRole;
};

struct CBufferLayout
{
    std::string             name;
    uint32_t                bindPoint = 0;
    uint32_t                space     = 0;
    uint32_t                sizeBytes = 0;
    std::vector<CBufferVar> vars;
};

// The element of a register-bound StructuredBuffer<T> whose T is a struct (a
// ResourceDescriptorHeap[] buffer is not in the reflection at all). Members are the struct's
// top level, by offset; Layout::Check compares them against the C++ mirror.
struct StructMember
{
    std::string name;
    uint32_t    offset = 0;   // bytes from the element's start
};

struct StructElement
{
    std::string               resource;    // the buffer's name (g_Materials)
    std::string               type;        // the element struct's HLSL name (MaterialGPUData)
    uint32_t                  stride = 0;  // bytes per element
    std::vector<StructMember> members;
};

struct Reflection
{
    std::vector<ResourceBinding> bindings;   // textures + samplers + cbuffers + buffers
    std::vector<CBufferLayout>   cbuffers;   // detailed layouts for each CBV
    std::vector<StructElement>   elements;   // struct-typed StructuredBuffer elements
    // ID3D12ShaderReflection::GetRequiresFlags: D3D_SHADER_REQUIRES_* bits the DXIL declares,
    // e.g. RESOURCE_DESCRIPTOR_HEAP_INDEXING, which the root signature must allow (checked by
    // ShaderBindingValidation).
    uint64_t                     requiresFlags = 0;
    // D3D12_SHADER_DESC::InstructionCount: every branch of this permutation counted once - the
    // shader's static size, not what one invocation executes. Renderer::CollectMaterialCosts
    // compares pixel shaders by it.
    uint32_t                     instructionCount = 0;
    // The source names its engine roles explicitly: it has an `@engine` annotation (on a
    // declaration, even one DXC stripped, or the bare line `//@engine explicit`), so the material
    // packer reads no role from a positional slot. Parsed from the source like the annotations.
    bool                         engineRolesExplicit = false;
    // The shader reads the frame's time (gTimeParams, PerViewCB / ShadowPerViewCB): from a DXIL,
    // the compiler's own usage flag - not the all-unused fallback, which would claim it for a
    // cbuffer the shader keeps but never reads; on a material's declared interface
    // (MaterialShaderDecl), its own code reading `.time` or gTimeParams. Such a material animates
    // without moving its geometry or changing its record (Renderer::WriteMaterialSlot sets
    // kMatGpuFlagReadsTime).
    bool                         readsFrameTime = false;
};

// Reflects the DXIL container @p dxbc (@p dxbcSize bytes) into @p out. Returns false and logs
// when DXC rejects it (corrupt blob, no reflection part). Safe from any thread: IDxcUtils is
// thread-safe.
bool Reflect(const void* dxbc, std::size_t dxbcSize, Reflection& out);

// Human-readable names for logging / inspector display.
const char* ToString(ResourceType t);
const char* ToString(VarType t);
const char* ToString(TextureDim d);

} // namespace ShaderReflect
