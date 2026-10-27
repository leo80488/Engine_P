// GPU indirect-command culling, one thread per IndirectDrawCommand (not per
// instance). BuildRenderScene already coarse-culled entities through the BVH and
// batched survivors into instanced commands; this pass does what the CPU cannot do
// cheaply: an exact per-instance frustum test (the BVH test is node-granular) and
// Hi-Z occlusion against the previous frame's depth pyramid.
//
// Three modes, selected by compactMode (each an independent CPU fallback):
//
// compactMode == 0: a command is kept if any of its instances passes; culled
// commands get instanceCount zeroed in place in the u0 arg buffer (degenerate-draw
// convention). Group argOffset/cmdCount stay valid; no count buffers.
//
// compactMode == 1 (per-instance cull + compaction; requires retained instances,
// i.e. the stable-slot remap): the pristine args (t3) are read-only, so a camera
// cut can never sticky-cull retained commands. Per instance the CS applies the
// main-pass predicates (instFlags / viewMask in GPUInstanceData) plus frustum and
// Hi-Z, compacts the surviving stable slots into the upper half of the working
// remap (u1) at kMaxInstances + instanceOffset (disjoint ranges per command, no
// allocator atomics), and emits the command with the survivor count into the
// working arg buffer (u0) at a group-compacted slot claimed by InterlockedAdd on
// the per-group count buffer (u2). The lower half of u1 is a CPU-copied pristine
// mapping and is never written here: full-count consumers sharing the main
// per-view CB (picking, transparent, outline) still see every instance.
// Skinned and WPO instances skip the AABB tests (bind-pose bounds do not cover the
// animated / displaced pose) via INSTANCE_FLAG_NO_GEOM_CULL on the instance record;
// every other predicate still applies. This is per instance: cmd.prevPosInfo is a
// TAA input (base element of the previous-frame skinned positions), legitimately
// 0xFFFFFFFF for a skinned draw with no previous deformation yet, so it never
// means "this draw is skinned".
//
// compactMode == 2: the per-shadow-view variant, see CompactShadowCommand.
//
// Root signature (the shared compute root sig, space2; it is frozen - slots are
// reused, no kRootSigVersion bump, the graphics sig is at the 64-DWORD cap):
//   [0] CBV  b0 space2 - CullingCB
//   [1] SRV  t0 space2 - GPUInstanceData[]      (batch instance ranges)
//   [2] SRV  t1 space2 - MeshAABB[]             (per-mesh local AABB)
//   [3] SRV  t2 space2 - Hi-Z pyramid (R32F, min-depth mips), read only when
//                        hiZValid != 0; Load()-based, no sampler
//   [4] UAV  u0 space2 - IndirectDrawCommand[]  (mode 0: in-place read/modify;
//                        modes 1/2: write-only working arg buffer)
//   [7] SRV  t3 space2 - pristine args (read-only, UPLOAD-heap SRV), reusing the
//                        skinning morph-delta slot (disjoint command lists)
//   [8] SRV  t4 space2 - per-command group meta (groupBase<<16|groupIdx,
//                        0xFFFFFFFF = in no indirect group)
//   [5] UAV  u1 space2 - working remap (2 x kMaxInstances u32; upper half
//                        written here, lower half CPU-pristine)
//  [14] UAV  u2 space2 - per-group survivor-command counts (atomics)
//   instance remap (uint per draw-local index): opened from the heap by
//                        CullingCB.remapIdx, read only when remapEnabled != 0

#include "gpu_instance.hlsli"
#include "hiz_cull.hlsli"
#include "bindless.hlsli"   // typed bindless handles

struct MeshAABB
{
    float3 aabbMin;
    float3 aabbMax;
};

// Must byte-match include/Graphics/IndirectDrawCommand.h (32 B).
struct IndirectDrawCommand
{
    uint meshDescIdx;
    uint instanceOffset;
    uint materialIdx;
    uint prevPosInfo;
    uint vertexCountPerInstance;
    uint instanceCount;
    uint startVertexLocation;
    uint startInstanceLocation;
};

cbuffer CullingCB : register(b0, space2)
{
    float4   frustumPlanes[6]; // xyz = inward normal, w = distance
    float4x4 viewProj;         // no-jitter, for Hi-Z screen projection
    uint     commandCount;
    uint     hiZValid;         // 0 = skip the occlusion test entirely
    uint     hiZWidth;         // Hi-Z mip 0 dimensions
    uint     hiZHeight;
    uint     hiZMipCount;
    // Shadow-view fields (zero in modes 0/1; mirror CullingPass.h):
    uint     shadowViewFlags;  // bit0 = exclude per-object; bit1 = dynamic-only spot cache update;
                               // bit2 = meshlet twin buckets as DISPATCH_MESH (CSM)
    uint     requiredViewBits; // this view's ViewBit (per-cascade author intent)
    uint     countBase;        // element base of this view's slice of u2
    // Stable-slot remap: 0 = g_Instances is draw-local-ordered (per-frame
    // compaction); 1 = records live at stable slots, resolve draw-local indices
    // through InstanceRemap(). Mirror CullingPass.h.
    uint     remapEnabled;
    // 1 = per-instance cull + compaction, 2 = per shadow view (see the file header).
    // Nonzero implies remapEnabled == 1: the stable-slot indirection is what lets a
    // draw skip individual instances. Mirror CullingPass.h.
    uint     compactMode;
    // Upper-half base of the working remap == kMaxInstances (mirrors Renderer.h;
    // also the arg/remap buffer element capacity); 0 in mode 2.
    uint     compactRemapBase;
    // Diagnostics: 1 = accumulate the post-cull visible-triangle counters into u3.
    // Gated because u3 is bound only when the Renderer both created the buffer and
    // emitted its state transition, and writing an unbound UAV is silent under
    // Resource Binding Tier 3. Same contract as hiZValid / remapEnabled. Mirror
    // CullingPass.h.
    uint     visStatsEnabled;
    // Heap index of the instance remap (StructuredBuffer<uint>, one slot per
    // draw-local index), read only when remapEnabled != 0. Mirror CullingPass.h.
    uint     remapIdx;
    uint3    _padRemap;
};

StructuredBuffer<GPUInstanceData>       g_Instances : register(t0, space2);
StructuredBuffer<MeshAABB>              g_MeshAABBs : register(t1, space2);
Texture2D<float>                        g_HiZ       : register(t2, space2);
RWStructuredBuffer<IndirectDrawCommand> g_Commands  : register(u0, space2);
// ==== Compaction resources, bound only when compactMode != 0 ====
StructuredBuffer<IndirectDrawCommand>   g_PristineCmds  : register(t3, space2);
StructuredBuffer<uint>                  g_CmdGroupMeta  : register(t4, space2);
RWStructuredBuffer<uint>                g_WorkingRemap  : register(u1, space2);
RWStructuredBuffer<uint>                g_GroupCounts   : register(u2, space2);
// ==== Diagnostics counters, bound only when visStatsEnabled != 0 ====
// [0] visible triangles  [1] surviving instances  [2] surviving commands
// [3] unused (16 B keeps the buffer a single allocation).
// Zeroed by the CPU each frame before this dispatch, read back a few frames later.
// Written only from CompactCommand (see the note there).
RWStructuredBuffer<uint>                g_VisStats      : register(u3, space2);
// The stable-slot instance remap, opened from the heap; call only on a path where
// remapEnabled != 0.
uint InstanceRemap(uint drawLocalIndex)
{
    StructuredBuffer<uint> remap = BindlessSBuf<uint>(remapIdx);
    return remap[drawLocalIndex];
}


// Frustum test: world AABB vs the 6 inward-pointing planes (positive vertex).
bool FrustumTestWorldAABB(float3 aabbMin, float3 aabbMax)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        float3 n = frustumPlanes[i].xyz;
        float  d = frustumPlanes[i].w;
        float3 pv;
        pv.x = (n.x >= 0) ? aabbMax.x : aabbMin.x;
        pv.y = (n.y >= 0) ? aabbMax.y : aabbMin.y;
        pv.z = (n.z >= 0) ? aabbMax.z : aabbMin.z;
        if (dot(n, pv) + d < 0)
            return false;
    }
    return true;
}

// Transforms a local AABB by the world matrix into a world-space AABB. The matrix
// convention follows the raster VS (GBuffer.vs.hlsl): world[3].xyz is the
// translation and world[0..2].xyz are the basis rows, used directly (row-vector
// convention as uploaded). Do not transpose here: identity rotations would still
// pass, but every rotated/scaled instance would get a wrong world AABB and be
// culled while on screen.
void TransformAABB(float4x4 world, float3 localMin, float3 localMax,
                   out float3 worldMin, out float3 worldMax)
{
    float3 t = world[3].xyz;
    worldMin = t;
    worldMax = t;
    [unroll]
    for (int i = 0; i < 3; i++)
    {
        float3 axis = world[i].xyz;
        float3 a = axis * localMin[i];
        float3 b = axis * localMax[i];
        worldMin += min(a, b);
        worldMax += max(a, b);
    }
}

// Hi-Z occlusion: HiZOcclusionTest lives in hiz_cull.hlsli (shared with
// SceneMeshlet.as's per-cluster test); this pass hands it the CullingCB's captured
// viewProj (uploaded transposed) and the t2 pyramid.

// Per-instance cull + compaction (compactMode == 1). Reads the pristine command,
// keeps the instances that pass every predicate, writes their stable slots to the
// working remap's upper half, and emits the command (survivor count, offset
// re-pointed at the upper half) into a group-compacted working-arg slot. Commands
// outside every indirect group (transparent / Custom / shadow-only packets) are
// ignored: their pristine lower-half remap ranges stay authoritative for the
// CPU-path passes that draw them.
void CompactCommand(uint idx)
{
    uint meta = g_CmdGroupMeta[idx];
    if (meta == 0xFFFFFFFFu) return;         // not consumed by ExecuteIndirect

    IndirectDrawCommand cmd = g_PristineCmds[idx];
    if (cmd.instanceCount == 0) return;      // defensive: emit never writes these

    const uint groupIdx  = meta & 0xFFFFu;
    // First command slot of the group: bits 16..30 (bit 31 = meshlet group, read
    // where the command is written).
    const uint groupBase = (meta >> 16) & 0x7FFFu;

    // Culling never looks at cmd.prevPosInfo: it is a TAA input, 0xFFFFFFFF whenever
    // no previous deformation exists (first frame, paused, ring allocation skipped),
    // not a "this draw is skinned" bit. Skinned and WPO draws are exempted per instance
    // by INSTANCE_FLAG_NO_GEOM_CULL below.
    MeshAABB meshAabb = g_MeshAABBs[cmd.meshDescIdx];

    uint kept = 0;
    for (uint i = 0; i < cmd.instanceCount; ++i)
    {
        // compactMode implies remapEnabled: retained mode is a hard CPU-side
        // precondition (per-instance compaction needs the slot indirection).
        const uint slotIdx = InstanceRemap(cmd.instanceOffset + i);
        GPUInstanceData inst = g_Instances[slotIdx];

        if ((inst.instFlags & INSTANCE_FLAG_MAIN_PASS) == 0) continue;
        if ((inst.viewMask  & VIEWBIT_MAIN_CAMERA)     == 0) continue;

        // Skinned and WPO (custom VS) instances leave the static AABB: the geometric
        // tests would cull them off their own displaced pose.
        if ((inst.instFlags & INSTANCE_FLAG_NO_GEOM_CULL) == 0)
        {
            float3 worldMin, worldMax;
            TransformAABB(inst.world, meshAabb.aabbMin, meshAabb.aabbMax,
                          worldMin, worldMax);

            if (!FrustumTestWorldAABB(worldMin, worldMax))
                continue;
            if (hiZValid != 0 &&
                (inst.instFlags & INSTANCE_FLAG_NO_OCCLUSION_CULL) == 0)
            {
                // The pyramid and its captured VP are one frame old: a rigid instance that moved
                // this frame is occlusion-tested where it was as well as where it is (union with
                // the prev-frame AABB; prevWorld is in the record for TAA velocity, and settled
                // objects have prev == curr, so the union is free for the static majority).
                float3 prevMin, prevMax;
                TransformAABB(inst.prevWorld, meshAabb.aabbMin, meshAabb.aabbMax,
                              prevMin, prevMax);
                if (!HiZOcclusionTest(min(worldMin, prevMin),
                                      max(worldMax, prevMax),
                                      viewProj, g_HiZ, hiZWidth, hiZHeight, hiZMipCount))
                    continue;
            }
        }

        // Upper-half range [base+offset, base+offset+kept): disjoint across
        // commands by the draw-local contiguity contract, so no atomics.
        g_WorkingRemap[compactRemapBase + cmd.instanceOffset + kept] = slotIdx;
        ++kept;
    }

    if (kept == 0) return;

    // Visible-triangle counters. The only place they are written: not in
    // CompactShadowCommand or a shared helper, because the shadow variant runs once per
    // CSM cascade, spot slice and point cube (up to 15 dispatches a frame), which would
    // inflate the count about tenfold and make it swing with the sun angle.
    // vertexCountPerInstance is the index count of a triangle list (programmable vertex
    // fetch hands DrawInstanced the index count), hence /3. The product is bounded by
    // the CPU's 64M-vertex TDR guard on vertexOrIndexCount.
    if (visStatsEnabled != 0)
    {
        InterlockedAdd(g_VisStats[0], (cmd.vertexCountPerInstance / 3u) * kept);
        InterlockedAdd(g_VisStats[1], kept);
        InterlockedAdd(g_VisStats[2], 1u);
    }

    cmd.instanceOffset = compactRemapBase + cmd.instanceOffset;
    cmd.instanceCount  = kept;
    // A main-view meshlet group (meta bit 31) executes with the DISPATCH_MESH
    // signature: X = the cluster blocks the pristine keeps in StartInstanceLocation,
    // Y = the survivors (instanceCount, just written), Z = 1; word 7 is never read. The
    // vis-stats above counted the pristine index count first. The pristine itself stays
    // a legal DRAW for the shadow views and the fallbacks.
    if ((meta & 0x80000000u) != 0)
    {
        cmd.vertexCountPerInstance = cmd.startInstanceLocation;
        cmd.startVertexLocation    = 1u;
        cmd.startInstanceLocation  = 0u;
    }
    uint outSlot;
    InterlockedAdd(g_GroupCounts[groupIdx], 1u, outSlot);
    g_Commands[groupBase + outSlot] = cmd;
}

// Per-shadow-view cull + compaction (compactMode == 2), one dispatch per shadow
// view; all three shadow families slice the same buffers:
//   * CSM cascade c - frustum = the cascade ortho VP, requiredViewBits =
//                     ShadowCascade0<<c, shadowViewFlags bit0 per frame.
//   * Spot slice s  - frustum = the spot's cone VP, requiredViewBits = ShadowAny
//                     (the gate SpotShadowPass applies CPU-side), no per-object
//                     exclusion.
//   * Point cube k  - one view for all six faces: they share one bounding sphere,
//                     so the CPU feeds the 6 axis-aligned planes of the sphere's
//                     bounding box, which with FrustumTestWorldAABB's
//                     positive-vertex selection is an exact conservative
//                     AABB-vs-AABB overlap test. requiredViewBits = ShadowAny.
// The plane test is the only geometric predicate, so any volume must be expressible
// as 6 inward half-spaces (hence the box around the point light's sphere).
// Differences from the main-view mode: (a) the t4 meta is the shadow group routing
// (ShadowDrawGroup buckets, CPU-classified from batch-static fields; a command in no
// bucket is 0xFFFFFFFF); (b) the main-pass predicates do not apply (shadow-only
// proxies with renderInMainPass=0 must still cast); (c) the frustum is the view's
// own and Hi-Z never runs (no shadow depth pyramid exists); (d) u1 is a per-view
// remap with no pristine lower half: compactRemapBase is 0 and every surviving
// command's range is written here, skinned ones included (they skip only the AABB
// test); (e) u2 counts are sliced per view via countBase.
// Per-instance predicates: requiredViewBits narrows the CPU meta's ShadowAny gate to
// this view's bit (per-cascade author intent), shadowViewFlags bit0 skips player
// instances while the per-object shadow map owns them, and bit1 keeps only dynamic
// instances for a spot cache update.
void CompactShadowCommand(uint idx)
{
    uint meta = g_CmdGroupMeta[idx];
    if (meta == 0xFFFFFFFFu) return;         // casts into no shadow bucket

    IndirectDrawCommand cmd = g_PristineCmds[idx];
    if (cmd.instanceCount == 0) return;      // defensive: emit never writes these

    const uint groupIdx  = meta & 0xFFFFu;
    // First command slot of the group: bits 16..30 (bit 31 = meshlet group, read
    // where the command is written).
    const uint groupBase = (meta >> 16) & 0x7FFFu;

    // See CompactCommand: skinned/WPO exemption is per instance via
    // INSTANCE_FLAG_NO_GEOM_CULL, never inferred from cmd.prevPosInfo.
    MeshAABB meshAabb = g_MeshAABBs[cmd.meshDescIdx];

    uint kept = 0;
    for (uint i = 0; i < cmd.instanceCount; ++i)
    {
        const uint slotIdx = InstanceRemap(cmd.instanceOffset + i);
        GPUInstanceData inst = g_Instances[slotIdx];

        if ((inst.viewMask & requiredViewBits) == 0) continue;
        if ((shadowViewFlags & 1u) != 0 &&
            (inst.instFlags & INSTANCE_FLAG_PEROBJ_SHADOW) != 0) continue;
        if ((shadowViewFlags & 2u) != 0 && !IsDynamicShadowInstance(inst)) continue;

        if ((inst.instFlags & INSTANCE_FLAG_NO_GEOM_CULL) == 0)
        {
            // The bind-pose AABB does not bound the animated pose (skinned), nor the static
            // AABB the WPO displacement (custom VS): both pass the geometric test.
            float3 worldMin, worldMax;
            TransformAABB(inst.world, meshAabb.aabbMin, meshAabb.aabbMax,
                          worldMin, worldMax);
            if (!FrustumTestWorldAABB(worldMin, worldMax))
                continue;
        }

        // Range [offset, offset+kept) in the per-view remap: disjoint across
        // commands by the draw-local contiguity contract, so no atomics.
        g_WorkingRemap[compactRemapBase + cmd.instanceOffset + kept] = slotIdx;
        ++kept;
    }

    if (kept == 0) return;

    cmd.instanceOffset = compactRemapBase + cmd.instanceOffset;
    cmd.instanceCount  = kept;
    // A CSM view (shadowViewFlags bit 2) writes a meshlet twin bucket's commands
    // (groupIdx >= 4, ShadowDrawGroup::MeshletOffset) as DISPATCH_MESH(blocks, kept, 1)
    // for ShadowMeshlet.as; the blocks ride the pristine StartInstanceLocation, as in
    // CompactCommand. Spot / point views keep DRAW and draw the twin like its base
    // bucket.
    if ((shadowViewFlags & 4u) != 0 && groupIdx >= 4u)
    {
        cmd.vertexCountPerInstance = cmd.startInstanceLocation;
        cmd.startVertexLocation    = 1u;
        cmd.startInstanceLocation  = 0u;
    }
    uint outSlot;
    InterlockedAdd(g_GroupCounts[countBase + groupIdx], 1u, outSlot);
    g_Commands[groupBase + outSlot] = cmd;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (idx >= commandCount) return;

    if (compactMode == 2)
    {
        CompactShadowCommand(idx);
        return;
    }
    if (compactMode != 0)
    {
        CompactCommand(idx);
        return;
    }

    IndirectDrawCommand cmd = g_Commands[idx];
    if (cmd.instanceCount == 0) return; // already degenerate (wantsMain filter)

    // See CompactCommand: skinned/WPO draws are exempted per instance via
    // INSTANCE_FLAG_NO_GEOM_CULL (checked in the loop below), never inferred from
    // cmd.prevPosInfo, a TAA input that is 0xFFFFFFFF for a skinned draw with no
    // previous-frame deformation.
    MeshAABB meshAabb = g_MeshAABBs[cmd.meshDescIdx];

    // Keep the command if any instance in the batch survives both tests. Batches
    // average ~10-20 instances and reach a few hundred at worst: one lane looping that
    // many AABB tests is cheap next to the draws it can eliminate.
    bool visible = false;
    for (uint i = 0; i < cmd.instanceCount && !visible; ++i)
    {
        // With retained instances the record lives at a stable slot; the command's
        // instanceOffset stays draw-local (matches the raster VSes).
        uint slotIdx = (remapEnabled != 0) ? InstanceRemap(cmd.instanceOffset + i)
                                           : (cmd.instanceOffset + i);
        GPUInstanceData inst = g_Instances[slotIdx];

        // Skinned/WPO exemption: the same rule as the compaction mode.
        if ((inst.instFlags & INSTANCE_FLAG_NO_GEOM_CULL) != 0)
        {
            visible = true;
            continue;
        }

        float3 worldMin, worldMax;
        TransformAABB(inst.world, meshAabb.aabbMin, meshAabb.aabbMax,
                      worldMin, worldMax);

        if (!FrustumTestWorldAABB(worldMin, worldMax))
            continue;

        if (hiZValid != 0 &&
            (inst.instFlags & INSTANCE_FLAG_NO_OCCLUSION_CULL) == 0)
        {
            // Union with the prev-frame AABB (see CompactCommand).
            float3 prevMin, prevMax;
            TransformAABB(inst.prevWorld, meshAabb.aabbMin, meshAabb.aabbMax,
                          prevMin, prevMax);
            if (!HiZOcclusionTest(min(worldMin, prevMin),
                                  max(worldMax, prevMax),
                                  viewProj, g_HiZ, hiZWidth, hiZHeight, hiZMipCount))
                continue;
        }

        visible = true;
    }

    if (!visible)
        g_Commands[idx].instanceCount = 0;
}
