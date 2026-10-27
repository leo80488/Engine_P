#ifndef BINDLESS_HLSLI
#define BINDLESS_HLSLI

// bindless.hlsli - typed reads of a bindless handle. A handle is what the C++ side put
// into a constant buffer through the device (IGraphicsDevice::Srv<K>, BindlessOf<K>,
// Cbv ...): gen << 20 | index, never a bare heap index. Open it only through these,
// never ResourceDescriptorHeap[handle]:
//
//   BindlessTex2D<T>(h)          Texture2D<T>
//   BindlessTex2DArray<T>(h)     Texture2DArray<T>
//   BindlessTexCube<T>(h)        TextureCube<T>
//   BindlessTexCubeArray<T>(h)   TextureCubeArray<T>
//   BindlessTex3D<T>(h)          Texture3D<T>
//   BindlessRWTex2D<T>(h)        RWTexture2D<T>
//   BindlessSBuf<T>(h)           StructuredBuffer<T>
//   BindlessRWSBuf<T>(h)         RWStructuredBuffer<T>
//   BindlessRawBuf(h)            ByteAddressBuffer
//   BindlessRWRawBuf(h)          RWByteAddressBuffer
//   BindlessCBuf<T>(h)           ConstantBuffer<T>
//
// Normally an accessor is ResourceDescriptorHeap[h & BINDLESS_INDEX_MASK], one AND more
// than a raw read. In the debug lane (-bindless_validate compiles every shader with
// BINDLESS_VALIDATE=1) it first looks the slot up in the C++ record mirrored at
// BINDLESS_VALIDATE_TABLE. A slot of another kind, or of another generation than the
// handle names (the handle outlived its resource), is appended to BINDLESS_VALIDATE_LOG
// (read back and reported as a [BINDLESS] ERROR), and the accessor opens the fixed null
// of the requested kind when it has one: the lane reports a wrong read, it does not
// perform it.
//
// The component class comes from T: uint and int (1-4 components) are UINT and SINT,
// anything else (float, half ...) FLOAT; 16-bit integer T is not classified. A handle is
// a constant-buffer value, so wave-uniform: no NonUniformResourceIndex.
//
// Pure ASCII: the offline SDK dxc reads the system code page.

#include "shared/bindless_kind.hlsli"

#ifndef BINDLESS_VALIDATE
#define BINDLESS_VALIDATE 0
#endif

template<typename T> uint BindlessClassOf() { return BK_CLASS_FLOAT; }
template<> uint BindlessClassOf<uint>()  { return BK_CLASS_UINT; }
template<> uint BindlessClassOf<uint2>() { return BK_CLASS_UINT; }
template<> uint BindlessClassOf<uint3>() { return BK_CLASS_UINT; }
template<> uint BindlessClassOf<uint4>() { return BK_CLASS_UINT; }
template<> uint BindlessClassOf<int>()   { return BK_CLASS_SINT; }
template<> uint BindlessClassOf<int2>()  { return BK_CLASS_SINT; }
template<> uint BindlessClassOf<int3>()  { return BK_CLASS_SINT; }
template<> uint BindlessClassOf<int4>()  { return BK_CLASS_SINT; }

#if BINDLESS_VALIDATE

// The fixed null of @p kind (shared/bindless_kind.hlsli), or 0xFFFFFFFF for a
// kind without one.
uint BindlessFixedNull(uint kind)
{
    const uint shape = kind & BK_SHAPE_MASK;
    const uint cls   = kind & BK_CLASS_MASK;
    if ((kind & BK_UAV) != 0u)
    {
        if (shape == BK_TEX2D && cls == BK_CLASS_FLOAT) return BINDLESS_NULL_RWTEX2D_FLOAT;
        if (shape == BK_TEX2D && cls == BK_CLASS_UINT)  return BINDLESS_NULL_RWTEX2D_UINT;
        if (shape == BK_RAWBUFFER)                      return BINDLESS_NULL_RWRAWBUFFER;
        return 0xFFFFFFFFu;
    }
    if (shape == BK_TEX2D)
        return cls == BK_CLASS_UINT ? BINDLESS_NULL_TEX2D_UINT
             : (cls == BK_CLASS_SINT ? BINDLESS_NULL_TEX2D_SINT : BINDLESS_NULL_TEX2D_FLOAT);
    if (cls == BK_CLASS_FLOAT && shape == BK_TEX2DARRAY)   return BINDLESS_NULL_TEX2DARRAY_FLOAT;
    if (cls == BK_CLASS_FLOAT && shape == BK_TEXCUBE)      return BINDLESS_NULL_TEXCUBE_FLOAT;
    if (cls == BK_CLASS_FLOAT && shape == BK_TEXCUBEARRAY) return BINDLESS_NULL_TEXCUBEARRAY_FLOAT;
    if (cls == BK_CLASS_FLOAT && shape == BK_TEX3D)        return BINDLESS_NULL_TEX3D_FLOAT;
    if (shape == BK_RAWBUFFER)        return BINDLESS_NULL_RAWBUFFER;
    if (shape == BK_STRUCTUREDBUFFER) return BINDLESS_NULL_STRUCTURED;
    if (shape == BK_CBV)              return BINDLESS_NULL_CBV;
    return 0xFFFFFFFFu;
}

// The heap index to open for handle @p h read as @p kind: its own when the
// record agrees on the kind (null or not) and the generation, otherwise - after
// logging the read - the kind's fixed null.
uint BindlessResolve(uint h, uint kind)
{
    const uint index = h & BINDLESS_INDEX_MASK;
    ByteAddressBuffer table = ResourceDescriptorHeap[BINDLESS_VALIDATE_TABLE];
    const uint slot     = table.Load(index * 4u);   // past the heap: 0, i.e. Free
    const bool sameKind = ((slot ^ kind) & (0xFFu & ~BK_NULL)) == 0u;
    const bool sameGen  = ((slot >> 8) & BINDLESS_GEN_MASK) == ((h >> BINDLESS_GEN_SHIFT) & BINDLESS_GEN_MASK);
    if (sameKind && sameGen)
        return index;

    RWByteAddressBuffer errLog = ResourceDescriptorHeap[BINDLESS_VALIDATE_LOG];
    uint n;
    errLog.InterlockedAdd(0u, 1u, n);
    if (n < BINDLESS_VALIDATE_RECORDS)
        errLog.Store4(16u + n * 16u, uint4(h, kind, slot, 0u));
    const uint fallback = BindlessFixedNull(kind);
    return fallback != 0xFFFFFFFFu ? fallback : index;
}

#else

uint BindlessResolve(uint h, uint kind) { return h & BINDLESS_INDEX_MASK; }

#endif // BINDLESS_VALIDATE

// Whether a handle names nothing: 0 (a field nobody wrote) or one of the
// fixed nulls. The gate in front of a read whose null would be a wrong
// VALUE (a depth of 0, an AO of 0). Not for a structured buffer, whose null
// is made per stride rather than fixed, and not for a handle into the
// bindless texture table (heap 0 .. BINDLESS_TABLE_SIZE-1), where index 0
// is texture handle_id 0.
bool BindlessNone(uint h)
{
    const uint i = h & BINDLESS_INDEX_MASK;
    return i == 0u || (i >= BINDLESS_NULL_BASE && i < BINDLESS_NULL_BASE + BINDLESS_NULL_COUNT);
}

template<typename T> Texture2D<T> BindlessTex2D(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEX2D | BindlessClassOf<T>())]; }

template<typename T> Texture2DArray<T> BindlessTex2DArray(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEX2DARRAY | BindlessClassOf<T>())]; }

template<typename T> TextureCube<T> BindlessTexCube(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEXCUBE | BindlessClassOf<T>())]; }

template<typename T> TextureCubeArray<T> BindlessTexCubeArray(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEXCUBEARRAY | BindlessClassOf<T>())]; }

template<typename T> Texture3D<T> BindlessTex3D(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEX3D | BindlessClassOf<T>())]; }

template<typename T> RWTexture2D<T> BindlessRWTex2D(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_TEX2D | BK_UAV | BindlessClassOf<T>())]; }

template<typename T> StructuredBuffer<T> BindlessSBuf(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_STRUCTUREDBUFFER | BK_CLASS_NONE)]; }

template<typename T> RWStructuredBuffer<T> BindlessRWSBuf(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_STRUCTUREDBUFFER | BK_UAV | BK_CLASS_NONE)]; }

ByteAddressBuffer BindlessRawBuf(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_RAWBUFFER | BK_CLASS_NONE)]; }

RWByteAddressBuffer BindlessRWRawBuf(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_RAWBUFFER | BK_UAV | BK_CLASS_NONE)]; }

template<typename T> ConstantBuffer<T> BindlessCBuf(uint h)
{ return ResourceDescriptorHeap[BindlessResolve(h, BK_CBV | BK_CLASS_NONE)]; }

#endif // BINDLESS_HLSLI
