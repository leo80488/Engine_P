#pragma once

// GPUInstanceData - per-instance data uploaded to a GPU StructuredBuffer.
// Must be byte-for-byte identical to GPUInstanceData in shaders/gpu_instance.hlsli.

#include <DirectXMath.h>
#include <cstdint>

// instFlags bit 0: the entity opted into the main pass (renderInMainPass). The GPU
// compaction CS applies it with viewMask, so the pristine indirect args stay
// view-independent and the shadow views reuse them.
inline constexpr uint32_t kInstanceFlagMainPass = 1u << 0;
// instFlags bit 1: the instance belongs to the player (per-object shadow caster)
// subtree; batch-uniform (perObjectShadowCaster is a batch-key field). The
// shadow-view cull CS skips these instances when the per-frame "exclude per-object
// casters from CSM" decision is on. That decision lands in UploadFrameData, after
// the retained shadow meta was filled, so it rides the dispatch CB and this bit,
// not the CPU meta.
inline constexpr uint32_t kInstanceFlagPerObjShadow = 1u << 1;
// Exempt from GPU frustum / Hi-Z culling: custom vertex shaders (WPO) displace
// vertices outside the static AABB. Wind-swaying foliage renders nearer than its
// own AABB corner, so the Hi-Z test would cull it off its own depth and it would
// pop in and out frame to frame.
inline constexpr uint32_t kInstanceFlagNoGeomCull   = 1u << 2;
// Frustum-cull, but skip the Hi-Z occlusion test. Set for skinned instances: they
// publish real animated bounds per frame (SkinnedMeshSubsystem -> SetMeshAABB), so
// the frustum test is sound and culls them for the shadow views too. But the Hi-Z
// pyramid is a frame old, the mesh-AABB buffer is single (not per-frame), and
// prevWorld tracks the entity transform rather than the pose, so occlusion-testing
// a deforming silhouette makes it pop in and out frame to frame.
inline constexpr uint32_t kInstanceFlagNoOcclusionCull = 1u << 3;

struct GPUInstanceData
{
    DirectX::XMFLOAT4X4 world;          // 64 bytes - transposed world matrix
    uint32_t            meshDescIdx;    //  4 bytes
    uint32_t            materialIdx;    //  4 bytes
    // viewMask / instFlags: the retained-instance path writes both; the per-frame path
    // writes viewMask 0 and only the cull-exemption bits. Read by the GPU culls
    // (InstanceCull.cs, SceneMeshlet.as) and Shadow.vs's dynamic-caster split
    // (IsDynamicShadowInstance); the other raster VSes ignore both fields.
    uint32_t            viewMask;       //  4 bytes - ViewBit mask (HierarchyComponents.h)
    uint32_t            instFlags;      //  4 bytes - kInstanceFlag* bits
    DirectX::XMFLOAT4X4 prevWorld;      // 64 bytes - previous-frame world (TAA velocity)
    // Dissolve (all-zero = inactive; the PS branches on dissolveParams.x > 0).
    DirectX::XMFLOAT4   dissolveParams;    // 16 bytes - x=amount y=edgeWidth z=noiseScale w=edgeIntensity
    DirectX::XMFLOAT4   dissolveEdgeColor; // 16 bytes - rgb edge glow; w = outline width in pixels (OutlineGeometryPass's VS)
};                                       // total: 176 bytes

static_assert(sizeof(GPUInstanceData) == 176, "GPUInstanceData must be 176 bytes");
