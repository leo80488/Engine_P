#pragma once

// The kind of a shader-visible descriptor, the bindless handle layout
// (gen << 20 | index) and the fixed null descriptors, defined once for C++ and HLSL
// in shaders/shared/bindless_kind.hlsli. Portable: no D3D types; the
// view-description -> kind mapping lives in the DX12 backend.

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../../shaders/shared/bindless_kind.hlsli"

namespace RHI::Bindless
{
    constexpr uint32_t MakeHandle(uint32_t index, uint32_t gen)
    {
        return ((gen & BINDLESS_GEN_MASK) << BINDLESS_GEN_SHIFT) | (index & BINDLESS_INDEX_MASK);
    }
    constexpr uint32_t IndexOf(uint32_t handle) { return handle & BINDLESS_INDEX_MASK; }
    constexpr uint32_t GenOf(uint32_t handle)   { return (handle >> BINDLESS_GEN_SHIFT) & BINDLESS_GEN_MASK; }

    // shape: BK_TEX2D ...; cls: BK_CLASS_FLOAT ...
    constexpr uint8_t MakeKind(uint32_t shape, uint32_t cls, bool uav = false, bool isNull = false)
    {
        return static_cast<uint8_t>((shape & BK_SHAPE_MASK) | (cls & BK_CLASS_MASK)
                                    | (uav ? BK_UAV : 0u) | (isNull ? BK_NULL : 0u));
    }
    constexpr uint32_t ShapeOf(uint8_t kind) { return kind & BK_SHAPE_MASK; }
    constexpr uint32_t ClassOf(uint8_t kind) { return kind & BK_CLASS_MASK; }
    constexpr bool     IsUav(uint8_t kind)   { return (kind & BK_UAV) != 0; }
    constexpr bool     IsNull(uint8_t kind)  { return (kind & BK_NULL) != 0; }
    // Two kinds name the same view type when they agree on everything but null.
    constexpr bool     SameType(uint8_t a, uint8_t b) { return ((a ^ b) & ~BK_NULL & 0xFFu) == 0; }

    // The kinds that typed bindless handle fields name.
    namespace Kind
    {
        inline constexpr uint8_t Tex2DFloat         = MakeKind(BK_TEX2D,            BK_CLASS_FLOAT);
        inline constexpr uint8_t Tex2DUint          = MakeKind(BK_TEX2D,            BK_CLASS_UINT);
        inline constexpr uint8_t Tex2DSint          = MakeKind(BK_TEX2D,            BK_CLASS_SINT);
        inline constexpr uint8_t Tex2DArrayFloat    = MakeKind(BK_TEX2DARRAY,       BK_CLASS_FLOAT);
        inline constexpr uint8_t Tex3DFloat         = MakeKind(BK_TEX3D,            BK_CLASS_FLOAT);
        inline constexpr uint8_t TexCubeFloat       = MakeKind(BK_TEXCUBE,          BK_CLASS_FLOAT);
        inline constexpr uint8_t TexCubeArrayFloat  = MakeKind(BK_TEXCUBEARRAY,     BK_CLASS_FLOAT);
        inline constexpr uint8_t StructuredBuffer   = MakeKind(BK_STRUCTUREDBUFFER, BK_CLASS_NONE);
        inline constexpr uint8_t RawBuffer          = MakeKind(BK_RAWBUFFER,        BK_CLASS_NONE);
        inline constexpr uint8_t Cbv                = MakeKind(BK_CBV,              BK_CLASS_NONE);
        inline constexpr uint8_t RWTex2DFloat       = MakeKind(BK_TEX2D,            BK_CLASS_FLOAT, true);
        inline constexpr uint8_t RWTex2DUint        = MakeKind(BK_TEX2D,            BK_CLASS_UINT,  true);
        inline constexpr uint8_t RWStructuredBuffer = MakeKind(BK_STRUCTUREDBUFFER, BK_CLASS_NONE,  true);
        inline constexpr uint8_t RWRawBuffer        = MakeKind(BK_RAWBUFFER,        BK_CLASS_NONE,  true);
    }

    inline const char* ShapeName(uint32_t shape)
    {
        switch (shape)
        {
        case BK_FREE:             return "free";
        case BK_TEX1D:            return "Tex1D";
        case BK_TEX2D:            return "Tex2D";
        case BK_TEX2DARRAY:       return "Tex2DArray";
        case BK_TEX2DMS:          return "Tex2DMS";
        case BK_TEX3D:            return "Tex3D";
        case BK_TEXCUBE:          return "TexCube";
        case BK_TEXCUBEARRAY:     return "TexCubeArray";
        case BK_TYPEDBUFFER:      return "TypedBuffer";
        case BK_STRUCTUREDBUFFER: return "StructuredBuffer";
        case BK_RAWBUFFER:        return "RawBuffer";
        case BK_CBV:              return "CBV";
        case BK_ACCELSTRUCT:      return "AccelStruct";
        case BK_TEX1DARRAY:       return "Tex1DArray";
        case BK_TEX2DMSARRAY:     return "Tex2DMSArray";
        default:                  return "?";
        }
    }

    // "Tex2D float", "RW StructuredBuffer", "null TexCubeArray float", "free".
    inline void Describe(uint8_t kind, char* out, size_t n)
    {
        const uint32_t shape = ShapeOf(kind);
        if (shape == BK_FREE && !IsNull(kind)) { std::snprintf(out, n, "free"); return; }
        const char* cls = "";
        switch (ClassOf(kind))
        {
        case BK_CLASS_FLOAT: cls = " float"; break;
        case BK_CLASS_UINT:  cls = " uint";  break;
        case BK_CLASS_SINT:  cls = " sint";  break;
        default:             break;
        }
        std::snprintf(out, n, "%s%s%s%s", IsNull(kind) ? "null " : "",
                      IsUav(kind) ? "RW " : "", ShapeName(shape), cls);
    }
}

class IGraphicsDevice;

namespace RHI
{
    // A shader-visible descriptor the device has checked holds a K: the value
    // (gen << 20 | index) a constant buffer field carries. Only the device makes one
    // (IGraphicsDevice::Srv / Uav / Cbv / BindlessOf) - there is no constructor from an
    // integer and no default - and when its input names nothing, or something that is
    // not a K, it makes the null of K: never 0, ~0u or -1.
    template<uint8_t K>
    class BindlessHandle
    {
    public:
        static constexpr uint8_t kKind = K;

        constexpr uint32_t Bits()  const { return m_bits; }
        constexpr uint32_t Index() const { return Bindless::IndexOf(m_bits); }
        constexpr uint32_t Gen()   const { return Bindless::GenOf(m_bits); }

    private:
        explicit constexpr BindlessHandle(uint32_t bits) : m_bits(bits) {}

        friend class ::IGraphicsDevice;

        uint32_t m_bits;
    };
}
