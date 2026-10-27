#include "Graphics/ShaderReflection.h"
#include "Graphics/DxcCompiler.h"

#include "System/Log.h"

#include <d3d12shader.h>     // ID3D12ShaderReflection + descriptors
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ShaderReflect
{
namespace
{
    // ==== D3D enums -> ShaderReflect enums ====
    ResourceType MapResourceType(D3D_SHADER_INPUT_TYPE t)
    {
        switch (t)
        {
            case D3D_SIT_TEXTURE:               return ResourceType::Texture;
            case D3D_SIT_SAMPLER:               return ResourceType::Sampler;
            case D3D_SIT_CBUFFER:               return ResourceType::CBuffer;
            case D3D_SIT_TBUFFER:               return ResourceType::CBuffer;       // tbuffer ~= cbuffer for layout
            case D3D_SIT_STRUCTURED:            return ResourceType::StructuredBuffer;
            case D3D_SIT_UAV_RWSTRUCTURED:      return ResourceType::RWStructuredBuffer;
            case D3D_SIT_UAV_RWTYPED:           return ResourceType::RWTexture;
            case D3D_SIT_BYTEADDRESS:           return ResourceType::ByteAddressBuffer;
            case D3D_SIT_UAV_RWBYTEADDRESS:     return ResourceType::ByteAddressBuffer;
            default:                            return ResourceType::Other;
        }
    }

    TextureDim MapDim(D3D_SRV_DIMENSION d)
    {
        switch (d)
        {
            case D3D_SRV_DIMENSION_TEXTURE1D:        return TextureDim::Tex1D;
            case D3D_SRV_DIMENSION_TEXTURE1DARRAY:   return TextureDim::Tex1DArray;
            case D3D_SRV_DIMENSION_TEXTURE2D:        return TextureDim::Tex2D;
            case D3D_SRV_DIMENSION_TEXTURE2DARRAY:   return TextureDim::Tex2DArray;
            case D3D_SRV_DIMENSION_TEXTURE2DMS:      return TextureDim::Tex2DMS;
            case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: return TextureDim::Tex2DMSArray;
            case D3D_SRV_DIMENSION_TEXTURE3D:        return TextureDim::Tex3D;
            case D3D_SRV_DIMENSION_TEXTURECUBE:      return TextureDim::TexCube;
            case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: return TextureDim::TexCubeArray;
            case D3D_SRV_DIMENSION_BUFFER:           return TextureDim::Buffer;
            default:                                 return TextureDim::Unknown;
        }
    }

    // Resolve a CBuffer variable's type from its D3D_SHADER_TYPE_DESC. Returns
    // VarType::Struct for nested structures and VarType::Unknown for anything
    // outside the small set the editor knows how to render.
    VarType MapVarType(const D3D12_SHADER_TYPE_DESC& td)
    {
        if (td.Class == D3D_SVC_STRUCT) return VarType::Struct;

        // Matrices: only float4x4 has an inspector widget.
        if (td.Class == D3D_SVC_MATRIX_COLUMNS || td.Class == D3D_SVC_MATRIX_ROWS)
        {
            if (td.Type == D3D_SVT_FLOAT && td.Rows == 4 && td.Columns == 4)
                return VarType::Float4x4;
            return VarType::Unknown;
        }

        // Scalar or vector: one switch on (component type, columns).
        const UINT cols = (td.Class == D3D_SVC_SCALAR) ? 1 : td.Columns;
        if (td.Type == D3D_SVT_FLOAT)
        {
            switch (cols) { case 1: return VarType::Float;  case 2: return VarType::Float2;
                            case 3: return VarType::Float3; case 4: return VarType::Float4; }
        }
        else if (td.Type == D3D_SVT_INT)
        {
            switch (cols) { case 1: return VarType::Int;    case 2: return VarType::Int2;
                            case 3: return VarType::Int3;   case 4: return VarType::Int4; }
        }
        else if (td.Type == D3D_SVT_UINT)
        {
            switch (cols) { case 1: return VarType::UInt;   case 2: return VarType::UInt2;
                            case 3: return VarType::UInt3;  case 4: return VarType::UInt4; }
        }
        else if (td.Type == D3D_SVT_BOOL)
        {
            return VarType::Bool;
        }
        return VarType::Unknown;
    }
} // namespace

bool Reflect(const void* dxbc, std::size_t dxbcSize, Reflection& out)
{
    out = {};

    if (!dxbc || dxbcSize == 0)
    {
        LOG_WARNING("ShaderReflect::Reflect: empty bytecode");
        return false;
    }

    // DxcCompiler::CreateReflection (IDxcUtils::CreateReflection) finds the reflection part inside
    // the DXIL container. Attach() takes over its reference without an AddRef.
    ComPtr<ID3D12ShaderReflection> refl;
    refl.Attach(DxcCompiler::CreateReflection(dxbc, dxbcSize));
    if (!refl)
    {
        LOG_ERROR("ShaderReflect::Reflect: DxcCompiler::CreateReflection failed");
        return false;
    }

    D3D12_SHADER_DESC desc{};
    if (FAILED(refl->GetDesc(&desc)))
    {
        LOG_ERROR("ShaderReflect::Reflect: GetDesc failed");
        return false;
    }
    out.requiresFlags = refl->GetRequiresFlags();
    out.instructionCount = desc.InstructionCount;

    // ---- Resource bindings (one entry per name in the resource table) ----
    out.bindings.reserve(desc.BoundResources);
    for (UINT i = 0; i < desc.BoundResources; ++i)
    {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bd))) continue;

        ResourceBinding b;
        b.name      = bd.Name ? bd.Name : "";
        b.type      = MapResourceType(bd.Type);
        b.dim       = MapDim(bd.Dimension);
        b.bindPoint = bd.BindPoint;
        b.bindCount = bd.BindCount;     // 0 means unbounded (`Texture2D t[]`)
        b.space     = bd.Space;
        out.bindings.push_back(std::move(b));
    }

    // ---- Constant buffer layouts ----
    out.cbuffers.reserve(desc.ConstantBuffers);
    for (UINT i = 0; i < desc.ConstantBuffers; ++i)
    {
        ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        if (!cb) continue;

        D3D12_SHADER_BUFFER_DESC bd{};
        if (FAILED(cb->GetDesc(&bd))) continue;

        // A StructuredBuffer<T> reflects as a RESOURCE_BIND_INFO block named after the buffer, holding
        // one "$Element" of type T; when T is a struct its top-level members (name + offset) are what
        // Layout::Check compares against the C++ mirror.
        if (bd.Type == D3D_CT_RESOURCE_BIND_INFO && bd.Variables == 1)
        {
            ID3D12ShaderReflectionVariable* el = cb->GetVariableByIndex(0);
            ID3D12ShaderReflectionType*     et = el ? el->GetType() : nullptr;
            D3D12_SHADER_TYPE_DESC etd{};
            if (et && SUCCEEDED(et->GetDesc(&etd)) && etd.Class == D3D_SVC_STRUCT && etd.Members > 0)
            {
                StructElement se;
                se.resource = bd.Name ? bd.Name : "";
                se.type     = etd.Name ? etd.Name : "";
                se.stride   = bd.Size;
                se.members.reserve(etd.Members);
                for (UINT m = 0; m < etd.Members; ++m)
                {
                    ID3D12ShaderReflectionType* mt = et->GetMemberTypeByIndex(m);
                    D3D12_SHADER_TYPE_DESC mtd{};
                    if (!mt || FAILED(mt->GetDesc(&mtd))) continue;
                    const LPCSTR mn = et->GetMemberTypeName(m);
                    se.members.push_back({ mn ? mn : "", mtd.Offset });
                }
                out.elements.push_back(std::move(se));
            }
            continue;
        }

        // Skip texture buffers, interface pointers etc.: only CBV-bound `cbuffer` blocks get a layout.
        if (bd.Type != D3D_CT_CBUFFER) continue;

        CBufferLayout layout;
        layout.name      = bd.Name ? bd.Name : "";
        layout.sizeBytes = bd.Size;
        layout.vars.reserve(bd.Variables);

        for (UINT v = 0; v < bd.Variables; ++v)
        {
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            if (!var) continue;

            D3D12_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd))) continue;

            ID3D12ShaderReflectionType* t = var->GetType();
            D3D12_SHADER_TYPE_DESC td{};
            if (!t || FAILED(t->GetDesc(&td))) continue;

            CBufferVar cv;
            cv.name     = vd.Name ? vd.Name : "";
            cv.offset   = vd.StartOffset;
            cv.size     = vd.Size;
            cv.elements = td.Elements;
            cv.type     = MapVarType(td);
            cv.used     = (vd.uFlags & D3D_SVF_USED) != 0;
            if (cv.used && cv.name == "gTimeParams") out.readsFrameTime = true;
            layout.vars.push_back(std::move(cv));
        }

        // If the compiler populated no usage flags (every var reads "unused"), keep them all:
        // filtering everything would silently blank the shader's whole param UI.
        {
            bool anyUsed = layout.vars.empty();
            for (const CBufferVar& cv : layout.vars) anyUsed |= cv.used;
            if (!anyUsed)
                for (CBufferVar& cv : layout.vars) cv.used = true;
        }

        // The constant-buffer iterator carries no register slot / space: take them from the binding
        // entry of the same name.
        for (const ResourceBinding& b : out.bindings)
        {
            if (b.type == ResourceType::CBuffer && b.name == layout.name)
            {
                layout.bindPoint = b.bindPoint;
                layout.space     = b.space;
                break;
            }
        }

        out.cbuffers.push_back(std::move(layout));
    }

    return true;
}

const char* ToString(ResourceType t)
{
    switch (t)
    {
        case ResourceType::Texture:             return "Texture";
        case ResourceType::Sampler:             return "Sampler";
        case ResourceType::CBuffer:             return "CBuffer";
        case ResourceType::StructuredBuffer:    return "StructuredBuffer";
        case ResourceType::RWTexture:           return "RWTexture";
        case ResourceType::RWStructuredBuffer:  return "RWStructuredBuffer";
        case ResourceType::ByteAddressBuffer:   return "ByteAddressBuffer";
        default:                                return "Other";
    }
}

const char* ToString(VarType t)
{
    switch (t)
    {
        case VarType::Bool:      return "bool";
        case VarType::Float:     return "float";
        case VarType::Float2:    return "float2";
        case VarType::Float3:    return "float3";
        case VarType::Float4:    return "float4";
        case VarType::Float4x4:  return "float4x4";
        case VarType::Int:       return "int";
        case VarType::Int2:      return "int2";
        case VarType::Int3:      return "int3";
        case VarType::Int4:      return "int4";
        case VarType::UInt:      return "uint";
        case VarType::UInt2:     return "uint2";
        case VarType::UInt3:     return "uint3";
        case VarType::UInt4:     return "uint4";
        case VarType::Struct:    return "struct";
        default:                 return "unknown";
    }
}

const char* ToString(TextureDim d)
{
    switch (d)
    {
        case TextureDim::Tex1D:         return "Tex1D";
        case TextureDim::Tex1DArray:    return "Tex1DArray";
        case TextureDim::Tex2D:         return "Tex2D";
        case TextureDim::Tex2DArray:    return "Tex2DArray";
        case TextureDim::Tex2DMS:       return "Tex2DMS";
        case TextureDim::Tex2DMSArray:  return "Tex2DMSArray";
        case TextureDim::Tex3D:         return "Tex3D";
        case TextureDim::TexCube:       return "TexCube";
        case TextureDim::TexCubeArray:  return "TexCubeArray";
        case TextureDim::Buffer:        return "Buffer";
        default:                        return "unknown";
    }
}

} // namespace ShaderReflect
