// shared/mesh_descriptor.hlsli - one mesh's vertex streams as the GPU reads them: the
// element of the global StructuredBuffer<MeshDescriptor>. C++ reads it as
// RHI::MeshDescriptor / RHI::StreamDescriptor (include/Graphics/GraphicsStruct.h), the
// shaders through pvf_fetch.hlsli (programmable vertex fetching) and ParticleEmit.cs
// (mesh-surface emission). The buffer's stride is the ABI every reader shares: do not keep a
// hand-written copy of this struct, a short one reads every element past 0 from the wrong
// offset.
// KEEP THIS FILE PURE ASCII (gpu_struct_types.hlsli).

#ifndef SHARED_MESH_DESCRIPTOR_HLSLI
#define SHARED_MESH_DESCRIPTOR_HLSLI

#include "gpu_struct_types.hlsli"

GPU_SHARED_BEGIN

// One vertex attribute stream, 16 bytes. bufferIndex 0xFFFFFFFF
// (RHI::kInvalidBufferIndex, INVALID_BUFFER in pvf_fetch.hlsli) = the stream is
// absent; format is a VF_* constant / RHI::VertexFormat (0xFFFFFFFF = Invalid).
struct StreamDesc
{
    uint bufferIndex GPU_SHARED_INIT(0xFFFFFFFFu);   // bindless index into g_Buffers[]
    uint byteOffset  GPU_SHARED_INIT(0u);            // byte offset to vertex 0
    uint byteStride  GPU_SHARED_INIT(0u);            // bytes between consecutive vertices
    uint format      GPU_SHARED_INIT(0xFFFFFFFFu);   // VF_* / RHI::VertexFormat
};

GPU_SHARED_SIZE(StreamDesc, 16)

// 160 bytes: nine streams and four index words.
struct MeshDescriptor
{
    StreamDesc position;        // always present
    StreamDesc normal;          // bufferIndex 0xFFFFFFFF if absent
    StreamDesc tangent;         // bufferIndex 0xFFFFFFFF if absent
    StreamDesc uv0;             // bufferIndex 0xFFFFFFFF if absent
    StreamDesc uv1;             // the second UV set; absent if 0xFFFFFFFF
    StreamDesc color;           // per-vertex colour; absent if 0xFFFFFFFF
    uint indexBufferIndex GPU_SHARED_INIT(0xFFFFFFFFu);   // 0xFFFFFFFF if non-indexed
    uint indexByteOffset  GPU_SHARED_INIT(0u);
    uint indexFormat      GPU_SHARED_INIT(0u);            // 0 = uint16, 1 = uint32
    uint vertexCount      GPU_SHARED_INIT(0u);

    // The previous frame's skinned positions, for TAA motion vectors - the only stream that
    // points into another frame's buffer. The skinned vertex ring is kFramesInFlight deep and
    // each slot is written only by its own frame, so last frame's output is intact in the
    // previous slot: `position` names this frame's slot and this one last frame's, at the base
    // offset where last frame's dispatch landed. Patched per frame beside `position` in
    // SkinnedMeshSubsystem::BuildSkinJobs.
    // bufferIndex == 0xFFFFFFFF is the only "no previous position" signal the vertex shaders
    // test: absent for every static mesh (never patched) and for a skinned entity the preceding
    // build did not skin at the same vertex count. Do not gate on the prevPosInfo root constant
    // instead: OutlinePass (picking flag / outline width) and FaceOverlayPass (overlay params)
    // alias that DWORD while still running GBuffer_VS.
    StreamDesc prevPosition;
    StreamDesc uv2;             // optional; appended to keep the earlier offsets
    StreamDesc uv3;
};

GPU_SHARED_SIZE(MeshDescriptor, 160)
GPU_SHARED_OFFSET(MeshDescriptor, color, 80)
GPU_SHARED_OFFSET(MeshDescriptor, indexBufferIndex, 96)
GPU_SHARED_OFFSET(MeshDescriptor, vertexCount, 108)
GPU_SHARED_OFFSET(MeshDescriptor, prevPosition, 112)
GPU_SHARED_OFFSET(MeshDescriptor, uv2, 128)
GPU_SHARED_OFFSET(MeshDescriptor, uv3, 144)

GPU_SHARED_END

#endif // SHARED_MESH_DESCRIPTOR_HLSLI
