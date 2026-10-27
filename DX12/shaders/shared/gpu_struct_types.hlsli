// shared/gpu_struct_types.hlsli - what a GPU struct shared by C++ and HLSL needs.
//
// A header under shaders/shared/ defines a struct once for both languages: the
// shaders include it as "shared/<name>.hlsli", and one C++ header per struct
// includes it by relative path (the depth is fixed: "../../shaders/shared/...").
// tools/source_gates.py fails a second definition of a struct it lists.
//
// The C++ side (__cplusplus defined): the HLSL vector types map to uint32_t and
// DirectXMath's XMUINT* / XMFLOAT* inside namespace GpuShared, the types the C++
// fill code writes. Every member is 4-byte aligned with no implicit padding, which
// is the StructuredBuffer packing too; a constant buffer adds the 16-byte row rule,
// checked per member by HLSL_ROW_SAFE (and HLSL_ROW_ARRAY for arrays, whose
// elements each take a full row - so arrays are float4 / uint4 only). Each header
// asserts its size and every member's offset, the numbers the HLSL side gets.
//
// Keep this file pure ASCII: the offline SDK dxc reads it in the system code
// page (build-and-dxc-gotchas).

#ifndef SHARED_GPU_STRUCT_TYPES_HLSLI
#define SHARED_GPU_STRUCT_TYPES_HLSLI

#ifdef __cplusplus
#include <bit>
#include <cstddef>
#include <cstdint>
#include <DirectXMath.h>

namespace GpuShared
{
    using uint     = uint32_t;
    using uint2    = DirectX::XMUINT2;
    using uint3    = DirectX::XMUINT3;
    using uint4    = DirectX::XMUINT4;
    using int2     = DirectX::XMINT2;
    using int4     = DirectX::XMINT4;
    using float2   = DirectX::XMFLOAT2;
    using float3   = DirectX::XMFLOAT3;
    using float4   = DirectX::XMFLOAT4;
    using float4x4 = DirectX::XMFLOAT4X4;
}

#define GPU_SHARED_BEGIN namespace GpuShared {
#define GPU_SHARED_END   }
// The C++ struct's own alignment where the C++ side relied on it (HLSL has none).
#define GPU_SHARED_ALIGN16 alignas(16)
// A member's C++ default (HLSL members have none): `uint x GPU_SHARED_INIT(0u);`
// makes `T t;` / `T t{};` start from that value on the C++ side.
#define GPU_SHARED_INIT(v) = v

// The checks end in their own semicolon: write them without one, so the HLSL
// side (where they expand to nothing) has no stray ';' at file scope.
#define GPU_SHARED_SIZE(T, n) \
    static_assert(sizeof(T) == (n), #T " must be " #n " bytes: the HLSL side is");
#define GPU_SHARED_OFFSET(T, m, off) \
    static_assert(offsetof(T, m) == (off), #T "::" #m " must sit at byte " #off);
// A constant-buffer member must not straddle a 16-byte row: HLSL would start it
// on the next row and every member after it would move.
#define HLSL_ROW_SAFE(T, m)                                                        \
    static_assert(offsetof(T, m) / 16 ==                                           \
                  (offsetof(T, m) + sizeof(static_cast<T*>(nullptr)->m) - 1) / 16, \
                  #T "::" #m " straddles a 16-byte constant-buffer row");
// A constant-buffer array: every element is a full row in HLSL, so it starts
// on a row and its elements are 16 bytes.
#define HLSL_ROW_ARRAY(T, m)                                                       \
    static_assert(offsetof(T, m) % 16 == 0 &&                                      \
                  sizeof(static_cast<T*>(nullptr)->m[0]) == 16,                    \
                  #T "::" #m " must be a row-aligned array of 16-byte elements");
#else
#define GPU_SHARED_BEGIN
#define GPU_SHARED_END
#define GPU_SHARED_ALIGN16
#define GPU_SHARED_INIT(v)
#define GPU_SHARED_SIZE(T, n)
#define GPU_SHARED_OFFSET(T, m, off)
#define HLSL_ROW_SAFE(T, m)
#define HLSL_ROW_ARRAY(T, m)
#endif

#endif // SHARED_GPU_STRUCT_TYPES_HLSLI
