#ifndef SHARED_BINDLESS_KIND_HLSLI
#define SHARED_BINDLESS_KIND_HLSLI

// bindless_kind.hlsli - what a shader-visible CBV/SRV/UAV descriptor IS, how a bindless
// handle is laid out, and where the null descriptors live. One table for C++ and HLSL:
// plain #defines, valid in both languages. C++ reaches it through
// include/Graphics/BindlessKind.h.
//
// Pure ASCII: the offline SDK dxc reads the system code page.

// ==== The handle: gen << 20 | index ====
// index: the descriptor's position in the shader-visible heap (1,000,000
// descriptors < 2^20). gen: the slot's generation, bumped whenever the slot is
// freed or its texture handle recycled, so a stale handle names its old
// generation. Shaders mask it off before ResourceDescriptorHeap[]; the debug
// lane (-bindless_validate) compares it.
#define BINDLESS_INDEX_BITS   20u
#define BINDLESS_INDEX_MASK   0x000FFFFFu
#define BINDLESS_GEN_SHIFT    20u
#define BINDLESS_GEN_MASK     0x00000FFFu

// ==== The kind byte ====
// bits 0-3 the shape, bit 4 UAV, bits 5-6 the component class, bit 7 null.
// Derived from the D3D12 view description where the descriptor is written,
// never typed in by hand. Deliberately NOT encoded: the channel count (reading
// float or float4 from one view is legal), a structured buffer's stride (the
// C++ side checks it where a handle is made), the exact format.
#define BK_SHAPE_MASK         0x0Fu
#define BK_FREE               0x00u
#define BK_TEX1D              0x01u
#define BK_TEX2D              0x02u
#define BK_TEX2DARRAY         0x03u
#define BK_TEX2DMS            0x04u
#define BK_TEX3D              0x05u
#define BK_TEXCUBE            0x06u
#define BK_TEXCUBEARRAY       0x07u
#define BK_TYPEDBUFFER        0x08u
#define BK_STRUCTUREDBUFFER   0x09u
#define BK_RAWBUFFER          0x0Au
#define BK_CBV                0x0Bu
#define BK_ACCELSTRUCT        0x0Cu
// CreateTexture can make the next two; no shader reads either through a handle.
#define BK_TEX1DARRAY         0x0Du
#define BK_TEX2DMSARRAY       0x0Eu
#define BK_UAV                0x10u
#define BK_CLASS_MASK         0x60u
#define BK_CLASS_FLOAT        0x00u   // FLOAT / UNORM / SNORM / depth
#define BK_CLASS_UINT         0x20u
#define BK_CLASS_SINT         0x40u
#define BK_CLASS_NONE         0x60u   // structured, raw, CBV, acceleration structure
#define BK_NULL               0x80u

// ==== The null descriptors ====
// One per kind, created right after the bindless texture table (heap 0 ..
// BINDLESS_TABLE_SIZE-1, asserted at creation), so their indices are constants
// both languages can name. "Absent" is the null of the field's kind: it reads
// zeros and never aliases a live resource. Structured buffers get theirs per
// stride, created on first use (not fixed). A fallback that carries a VALUE
// (1x1 white, flat normal, fully-lit shadow) is not "absent" and stays.
#define BINDLESS_TABLE_SIZE                16384u
#define BINDLESS_NULL_BASE                 BINDLESS_TABLE_SIZE
#define BINDLESS_NULL_TEX2D_FLOAT          (BINDLESS_NULL_BASE + 0u)
#define BINDLESS_NULL_TEX2D_UINT           (BINDLESS_NULL_BASE + 1u)
#define BINDLESS_NULL_TEX2D_SINT           (BINDLESS_NULL_BASE + 2u)
#define BINDLESS_NULL_TEX2DARRAY_FLOAT     (BINDLESS_NULL_BASE + 3u)
#define BINDLESS_NULL_TEXCUBE_FLOAT        (BINDLESS_NULL_BASE + 4u)
#define BINDLESS_NULL_TEXCUBEARRAY_FLOAT   (BINDLESS_NULL_BASE + 5u)
#define BINDLESS_NULL_TEX3D_FLOAT          (BINDLESS_NULL_BASE + 6u)
#define BINDLESS_NULL_RAWBUFFER            (BINDLESS_NULL_BASE + 7u)
#define BINDLESS_NULL_CBV                  (BINDLESS_NULL_BASE + 8u)
#define BINDLESS_NULL_RWTEX2D_FLOAT        (BINDLESS_NULL_BASE + 9u)
#define BINDLESS_NULL_RWTEX2D_UINT         (BINDLESS_NULL_BASE + 10u)
#define BINDLESS_NULL_RWRAWBUFFER          (BINDLESS_NULL_BASE + 11u)
// Stride 4. A structured field's own null is made for its stride (C++
// BindlessNull); this one is what the debug lane substitutes when it refuses
// a structured read, whose stride HLSL cannot name.
#define BINDLESS_NULL_STRUCTURED           (BINDLESS_NULL_BASE + 12u)
#define BINDLESS_NULL_COUNT                13u

// ==== The debug targets' fixed slots ====
// Fixed slots right after the nulls, allocated at boot in every run (ordinary allocations
// follow them): three R32_UINT RWTexture2D counters and the material cost table, each the
// null of its kind until the debug target view makes the resource (DebugTargetViewPass;
// IGraphicsDevice::SetFixedDebugUav / SetFixedDebugBuffer). Only the DEBUG_SHADE
// permutation (debug_shade.hlsli) and the producers that read them
// (DebugTargetProduce.cs) name them.
#define BINDLESS_DEBUG_BASE                (BINDLESS_NULL_BASE + BINDLESS_NULL_COUNT)
#define BINDLESS_DEBUG_OVERDRAW            (BINDLESS_DEBUG_BASE + 0u)   // fragments the material PS ran for
#define BINDLESS_DEBUG_DENSITY             (BINDLESS_DEBUG_BASE + 1u)   // texel density, packed with depth
#define BINDLESS_DEBUG_COST                (BINDLESS_DEBUG_BASE + 2u)   // shading cost per pixel
// StructuredBuffer<uint>, one per material slot: the instruction count of the pixel
// shader that draws it (Renderer::CollectMaterialCosts).
#define BINDLESS_DEBUG_COST_TABLE          (BINDLESS_DEBUG_BASE + 3u)
#define BINDLESS_DEBUG_COUNT               4u

// ==== The debug lane (-bindless_validate) ====
// Two more fixed slots after the debug targets, allocated ONLY in a validate run (every
// shader then compiles with BINDLESS_VALIDATE=1; otherwise nothing names them and
// ordinary allocations use those indices):
//   BINDLESS_VALIDATE_TABLE  ByteAddressBuffer, one uint per heap slot:
//                            kind | gen << 8 - the C++ record, mirrored;
//   BINDLESS_VALIDATE_LOG    RWByteAddressBuffer: uint [0] counts the refused reads
//                            (never reset); records of 4 uints from byte 16:
//                            { handle, kind asked for, the slot's kind | gen << 8, 0 },
//                            the first BINDLESS_VALIDATE_RECORDS of the run.
#define BINDLESS_VALIDATE_TABLE            (BINDLESS_DEBUG_BASE + BINDLESS_DEBUG_COUNT)
#define BINDLESS_VALIDATE_LOG              (BINDLESS_VALIDATE_TABLE + 1u)
#define BINDLESS_VALIDATE_RECORDS          63u
#define BINDLESS_VALIDATE_LOG_BYTES        (16u + BINDLESS_VALIDATE_RECORDS * 16u)

#endif // SHARED_BINDLESS_KIND_HLSLI
