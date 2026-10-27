// Programmable Vertex Fetching: format-dispatching fetch helpers over the bindless
// ByteAddressBuffer array, for any vertex shader that reads its streams itself.
// g_Buffers[] is register(t0, space1), 16384 slots; it must match the bindless root
// signature in GraphicsDX12.

#ifndef PVF_FETCH_HLSLI
#define PVF_FETCH_HLSLI

// ==== Vertex format constants (must match the RHI::VertexFormat enum) ====
#define VF_FLOAT3       0
#define VF_FLOAT2       1
#define VF_HALF2        2
#define VF_R10G10B10A2  3
#define VF_R8G8B8A8     4
#define VF_FLOAT4       5
#define INVALID_BUFFER  0xFFFFFFFFu

// ==== Bindless buffer array ====
ByteAddressBuffer g_Buffers[16384] : register(t0, space1);

// ==== StreamDesc / MeshDescriptor: one definition for C++ and every shader ====
#include "shared/mesh_descriptor.hlsli"

// ==== Low-level fetch helpers ====

float3 FetchFloat3(uint idx, uint off)
{
    return asfloat(g_Buffers[idx].Load3(off));
}

float2 FetchFloat2(uint idx, uint off)
{
    return asfloat(g_Buffers[idx].Load2(off));
}

float4 FetchFloat4(uint idx, uint off)
{
    return asfloat(g_Buffers[idx].Load4(off));
}

float2 FetchHalf2(uint idx, uint off)
{
    uint p = g_Buffers[idx].Load(off);
    return float2(f16tof32(p & 0xFFFF), f16tof32(p >> 16));
}

float3 FetchR10G10B10(uint idx, uint off)
{
    uint p = g_Buffers[idx].Load(off);
    return normalize(float3(
        ((p >>  0) & 0x3FF) / 511.5f - 1.f,
        ((p >> 10) & 0x3FF) / 511.5f - 1.f,
        ((p >> 20) & 0x3FF) / 511.5f - 1.f));
}

float4 FetchR8G8B8A8(uint idx, uint off)
{
    uint p = g_Buffers[idx].Load(off);
    return float4((p & 0xFF), ((p >> 8) & 0xFF),
                  ((p >> 16) & 0xFF), ((p >> 24) & 0xFF)) / 255.f;
}

// ==== Format-dispatching helpers ====

float3 FetchAsFloat3(uint idx, uint off, uint fmt)
{
    // Unbound or unpatched stream (e.g. a skinned position descriptor still on its
    // placeholder this frame) -> degenerate vertex, never an out-of-bounds
    // g_Buffers[INVALID_BUFFER] read.
    if (idx == INVALID_BUFFER)  return (float3)0;
    if (fmt == VF_FLOAT3)      return FetchFloat3(idx, off);
    if (fmt == VF_R10G10B10A2) return FetchR10G10B10(idx, off);
    return (float3)0;
}

float2 FetchAsFloat2(uint idx, uint off, uint fmt)
{
    if (idx == INVALID_BUFFER) return (float2)0;
    if (fmt == VF_FLOAT2) return FetchFloat2(idx, off);
    if (fmt == VF_HALF2)  return FetchHalf2(idx, off);
    return (float2)0;
}

float4 FetchAsFloat4(uint idx, uint off, uint fmt)
{
    if (fmt == VF_FLOAT4) return FetchFloat4(idx, off);
    return (float4)0;
}

// Per-vertex color: packed R8G8B8A8 (imported .meshlib), float4, or float3
// (procedural primitives, alpha 1). Opaque white when the mesh has no color stream,
// so an unconditional baseColor multiply is a no-op on uncolored meshes.
float4 FetchAsColor(uint idx, uint off, uint fmt)
{
    if (idx == INVALID_BUFFER)  return float4(1, 1, 1, 1);
    if (fmt == VF_R8G8B8A8)     return FetchR8G8B8A8(idx, off);
    if (fmt == VF_FLOAT4)       return FetchFloat4(idx, off);
    if (fmt == VF_FLOAT3)       return float4(FetchFloat3(idx, off), 1.0);
    return float4(1, 1, 1, 1);
}

// ==== Convenience macros (md = MeshDescriptor, vid = resolved vertex index) ====

#define FETCH_POS(md, vid) \
    FetchAsFloat3((md).position.bufferIndex, \
                  (md).position.byteOffset + (vid) * (md).position.byteStride, \
                  (md).position.format)

#define FETCH_NORMAL(md, vid) \
    FetchAsFloat3((md).normal.bufferIndex, \
                  (md).normal.byteOffset + (vid) * (md).normal.byteStride, \
                  (md).normal.format)

#define FETCH_TANGENT(md, vid) \
    FetchAsFloat3((md).tangent.bufferIndex, \
                  (md).tangent.byteOffset + (vid) * (md).tangent.byteStride, \
                  (md).tangent.format)

#define FETCH_UV0(md, vid) \
    FetchAsFloat2((md).uv0.bufferIndex, \
                  (md).uv0.byteOffset + (vid) * (md).uv0.byteStride, \
                  (md).uv0.format)

// Second UV set (lightmap / detail / blend). Returns (0,0) when absent.
#define FETCH_UV1(md, vid) \
    FetchAsFloat2((md).uv1.bufferIndex, \
                  (md).uv1.byteOffset + (vid) * (md).uv1.byteStride, \
                  (md).uv1.format)
#define FETCH_UV2(md, vid) \
    FetchAsFloat2((md).uv2.bufferIndex, \
                  (md).uv2.byteOffset + (vid) * (md).uv2.byteStride, \
                  (md).uv2.format)
#define FETCH_UV3(md, vid) \
    FetchAsFloat2((md).uv3.bufferIndex, \
                  (md).uv3.byteOffset + (vid) * (md).uv3.byteStride, \
                  (md).uv3.format)

// Per-vertex color from the color stream (not uv1), as float4 RGBA; opaque white when
// the mesh has no color stream.
#define FETCH_COLOR(md, vid) \
    FetchAsColor((md).color.bufferIndex, \
                 (md).color.byteOffset + (vid) * (md).color.byteStride, \
                 (md).color.format)

// Tangent fetched as float4 (xyz = tangent direction, w = bitangent handedness sign).
// Returns (1,0,0,1) as a fallback when no tangent buffer is bound.
#define FETCH_TANGENT4(md, vid) \
    (((md).tangent.bufferIndex != INVALID_BUFFER) ? \
        FetchAsFloat4((md).tangent.bufferIndex, \
                      (md).tangent.byteOffset + (vid) * (md).tangent.byteStride, \
                      (md).tangent.format) : \
        float4(1, 0, 0, 1))

// ==== Index fetch (no SetIndexBuffer in CPU recording code) ====

uint FetchIndex(MeshDescriptor md, uint rawID)
{
    if (md.indexBufferIndex == INVALID_BUFFER) return rawID;
    uint byteOff = md.indexByteOffset + rawID * (md.indexFormat == 0u ? 2u : 4u);
    if (md.indexFormat == 0u)
    {
        // uint16: load the 4-byte word containing the 2-byte index and extract.
        uint word = g_Buffers[md.indexBufferIndex].Load(byteOff & ~3u);
        return (word >> ((byteOff & 2u) * 8u)) & 0xFFFFu;
    }
    return g_Buffers[md.indexBufferIndex].Load(byteOff);
}

#endif // PVF_FETCH_HLSLI
