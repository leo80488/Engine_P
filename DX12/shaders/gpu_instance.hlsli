#ifndef GPU_INSTANCE_HLSLI
#define GPU_INSTANCE_HLSLI

// gpu_instance.hlsli - GPU instance data layout.
// Mirrors C++ GPUInstanceData in include/Graphics/GPUInstanceData.h.

// Per-instance visibility bits. Written by the retained-instance emit; read only
// by the InstanceCull.cs compaction mode - raster VSes never touch them. Values
// mirror C++ ViewBit (HierarchyComponents.h) / kInstanceFlagMainPass
// (GPUInstanceData.h).
#define VIEWBIT_MAIN_CAMERA     (1u << 0)
#define INSTANCE_FLAG_MAIN_PASS (1u << 0)
// Player (per-object shadow caster) subtree: the shadow-view compaction skips
// these instances when the CSM excludes per-object casters this frame.
#define INSTANCE_FLAG_PEROBJ_SHADOW (1u << 1)
// Custom vertex shader (WPO): displaced geometry is not bounded by the static
// AABB, so frustum/Hi-Z tests are skipped (same class as skinned). Mirrors
// kInstanceFlagNoGeomCull in GPUInstanceData.h.
#define INSTANCE_FLAG_NO_GEOM_CULL  (1u << 2)
// Frustum-cull, but do not occlusion-cull. For deforming (skinned) geometry: its
// per-frame AABB is published and correct, so the frustum test is sound, but the
// Hi-Z pyramid and the AABB buffer are each up to a frame out of step with the
// pose, and a deforming silhouette tested against stale depth pops in and out at
// a doorway edge.
#define INSTANCE_FLAG_NO_OCCLUSION_CULL (1u << 3)

struct GPUInstanceData
{
    float4x4 world;          // 64 bytes - transposed current world matrix
    uint     meshDescIdx;    //  4 bytes
    uint     materialIdx;    //  4 bytes
    uint     viewMask;       //  4 bytes - ViewBit mask (0 on the per-frame path)
    uint     instFlags;      //  4 bytes - INSTANCE_FLAG_* bits
    float4x4 prevWorld;      // 64 bytes - transposed prev-frame world (TAA velocity)
    // Dissolve (all-zero = inactive; the PS branches on dissolveParams.x > 0).
    float4   dissolveParams;    // 16 bytes - x=amount y=edgeWidth z=noiseScale w=edgeIntensity
    float4   dissolveEdgeColor; // 16 bytes - rgb edge glow, w unused
};                            // (total: 176 bytes)

// Mixed spot-shadow cache partition. Skinned instances carry NO_OCCLUSION;
// WPO carries NO_GEOM. Rigid transforms/material edits invalidate static depth
// through the CPU dirty tracker, so they remain eligible for the static cache.
bool IsDynamicShadowInstance(GPUInstanceData inst)
{
    return (inst.instFlags & (INSTANCE_FLAG_NO_GEOM_CULL | INSTANCE_FLAG_NO_OCCLUSION_CULL)) != 0 ||
           inst.dissolveParams.x > 0.0f;
}

// Handedness correction for mirrored instances (Unity's
// unity_WorldTransformParams.w / GetOddNegativeScale(), Unreal's per-primitive
// determinant sign). Returns -1 when the world matrix has an odd number of
// negative scale axes (negative determinant), else +1.
// N and T stay correct under a mirror, but B is derived as cross(N, T), and
// cross(Ma, Mb) = det(M) * M^-T * cross(a, b): for a mirror the true bitangent
// carries an extra sign(det) that a plain cross() drops, so normal-mapped mirrored
// surfaces are lit from the wrong side (bumps read as dents).
// Apply it exactly once, in the vertex stage, folded into the tangent .w term: the
// pixel stage uses the interpolated bitangent verbatim (custom_surface.hlsli
// MS_MakeParams). det is invariant under transpose, so the transposed storage of
// GPUInstanceData::world needs no adjustment.
float InstanceHandednessSign(float4x4 world)
{
    return (determinant((float3x3)world) < 0.0f) ? -1.0f : 1.0f;
}

// Maps a draw-local index (instanceOffset + SV_InstanceID) to its stable slot:
// with retained instances the records live at stable per-entity slots in one
// DEFAULT-heap InstanceBuffer, and a per-frame StructuredBuffer<uint> holds the
// map. Its heap index (remapHeapIdx) rides in the per-view CB (b1), and 0 ==
// identity, so per-frame compaction and the retained-off path need no permutation
// and no root-signature change. ResourceDescriptorHeap needs SM6.6, Resource
// Binding Tier 3 (the engine requires DXR-class hardware) and the root signature's
// CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED flag. Compiled out of compute targets
// (__SHADER_TARGET_STAGE 5) so a cs_* include never carries the heap-indexing DXIL
// feature flag; InstanceCull.cs opens the remap itself.
#if !defined(__SHADER_TARGET_STAGE) || (__SHADER_TARGET_STAGE != 5)
uint ResolveInstanceSlot(uint remapHeapIdx, uint linearIdx)
{
    [branch]
    if (remapHeapIdx == 0)
        return linearIdx;
    StructuredBuffer<uint> remapBuf = ResourceDescriptorHeap[remapHeapIdx];
    return remapBuf[linearIdx];
}
#endif

#endif // GPU_INSTANCE_HLSLI
