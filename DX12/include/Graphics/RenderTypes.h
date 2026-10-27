#pragma once

// Renderer-layer types: FrameIndex, DrawFilter, DrawPacket, DrawList, RenderView,
// the shadow draw-group classification and RenderCamera. No DX12 types (ID3D12*,
// D3D12_*, DXGI_FORMAT) appear here.
//
// DrawPacket deliberately carries NO PSO pointer: the Renderer analyzes mesh +
// material to decide filter membership and permutation, and each RenderPass
// resolves the matching PSO from its own PSOCache at draw time.

#include "Graphics/GraphicsStruct.h"
#include <cmath>
#include <DirectXCollision.h>
#include "Graphics/PermutationKey.h"

#include <cstdint>
#include <array>
#include <span>
#include <DirectXMath.h>

using FrameIndex = uint64_t;

enum class DrawFilter : uint8_t
{
    Opaque      = 0,
    Shadow      = 1,
    Transparent = 2,
    Custom      = 3,
    // Water - consumed ONLY by WaterSurfacePass. Every other consumer names the
    // filters it wants explicitly (nothing enumerates DrawFilter), so tagging a draw
    // Water removes it from GBuffer, Transparent, all five shadow passes, FaceOverlay,
    // HairShadowMark, SceneVoxel and the indirect-args builder without touching any of
    // them.
    Water       = 4,
    // SECONDARY VIEW (the camera preview / in-game PiP). A candidate that failed the
    // MAIN camera frustum but is inside a live secondary camera's is DUPLICATED into
    // one of these two filters, keeping every derived field the main-view packet would
    // have had (permutation, stencil ref, material index, instance range). The original
    // candidate still becomes a DrawFilter::Shadow caster or is dropped, so the CSM and
    // the main view are untouched.
    //
    // A SEPARATE FILTER RATHER THAN A viewMask BIT because of the GPU compaction path:
    // the cull CS reads GPUInstanceData::viewMask, which is the ENTITY's mask, so
    // clearing ViewBit::MainCamera on the packet alone would still let the compacted
    // args draw it. As nothing enumerates DrawFilter, a new value removes these packets
    // from GBuffer, Transparent, all five shadow passes, FaceOverlay, HairShadowMark,
    // SceneVoxel, the outline passes, the picking pass and the indirect-args groups
    // without touching any of them. The two explicit exceptions are
    // ClassifyShadowDrawGroup (below, which walks ALL packets for the retained shadow
    // meta) and the triangle census.
    //
    // TWO VALUES, not one plus a bit: Renderer::CullWorldForPreview has to hand the
    // preview graph one contiguous Opaque run followed by one contiguous Transparent
    // run (DrawList is a span found by scanning for a filter run), and the source
    // filter is the only thing that says which is which.
    SecondaryView      = 5,   // copy of an Opaque candidate
    SecondaryViewBlend = 6,   // copy of a Transparent candidate
    // THE REFLECTION PROBE'S DUPLICATE: the same copy for the probe baking this frame -
    // an OPAQUE candidate leaving the main lists inside the probe's faces
    // (Renderer::NoteSecondaryViewCandidate), read only by the faces' CullView in
    // ProcessProbeBakeQueue. Its own filter, so a PiP never draws what only the probe
    // sees and the probe never draws what only a PiP sees: a candidate both see gets
    // both copies. The duplicate filters are the LARGEST values on purpose - they stay the
    // sort's tail, which SortAndEmit's indirect-args and cull-range trims rest on.
    ProbeView          = 7,   // copy of an Opaque candidate
    // A main view's secondary view's copy of an outline shell (a Custom candidate the main
    // frustum rejected): its CullView turns it back into Custom for the outline passes.
    SecondaryViewCustom = 8,
    Count,
};

// Is this filter one of the duplicates kept for another view (the PiP's two, the
// probe's one), which the MAIN view's passes never draw? Written once so the
// triangle census and every other consumer agree about it.
constexpr bool IsSecondaryViewFilter(DrawFilter f)
{
    return f == DrawFilter::SecondaryView || f == DrawFilter::SecondaryViewBlend
        || f == DrawFilter::ProbeView || f == DrawFilter::SecondaryViewCustom;
}

// Filters that are BACK-TO-FRONT painter-sorted and so need a per-frame
// camera-relative depth refresh.
//
// Use it at every one of these sites together: the 128-bit sort-key prefix, the
// fallback comparator and all three refreshDepth call sites. If the comparator and
// the key builder disagree about a class, std::sort(par_unseq) is handed a
// non-strict-weak ordering: undefined behaviour, not merely wrong order.
//
// SecondaryViewBlend is one of them: otherwise the PiP's blended duplicates take
// the PSO-KEY branch of the sort (perm / mirrored / stencil / texBaseColor), every
// instance of one material collapses into a handful of packets, and
// CullWorldForPreview's re-sort against the secondary camera, which can only order
// WHOLE PACKETS by the depth of their first instance, means nothing. Their depth is
// the MAIN camera's planar depth, which is all the batching granularity needs; the
// secondary camera's own order comes from that re-sort.
//
// The main view never sees the secondary filters: they exist only while a WorldCull
// view is live (NoteSecondaryViewCandidate returns on !m_secondaryCullActive, and
// the gather's wantSecondary is false otherwise). ProbeView, the probe's opaque
// copy, is not depth-sorted.
constexpr bool IsDepthSortedFilter(DrawFilter f)
{
    return f == DrawFilter::Transparent || f == DrawFilter::Water
        || f == DrawFilter::SecondaryViewBlend;
}

struct FrustumPlane
{
    DirectX::XMFLOAT3 normal;
    float             distance;
};
using FrustumPlanes = std::array<FrustumPlane, 6>;

// Extracts the 6 world-space frustum planes of a world -> clip matrix (viewProj)
// with the Gribb-Hartmann method. The planes are normalized and their normals point
// INWARD, so FrustumTestAABB(FrustumPlanes, ...) rejects with one dot product per
// plane. Works for standard and reverse-Z (the clip volume 0 <= z <= w is the same;
// only the near/far mapping differs).
//
// Row-vector convention: worldPt * VP = clipPt, row-major M[row][col], so clip
// component i is column i of VP: clip_i = sum over r of M[r][i] * worldPt[r].
// Inside the frustum: -w <= x <= w, -w <= y <= w, 0 <= z <= w, which gives the
// inward-normal planes
//   Left:   (col[3]+col[0]) . p >= 0
//   Right:  (col[3]-col[0]) . p >= 0
//   Bottom: (col[3]+col[1]) . p >= 0
//   Top:    (col[3]-col[1]) . p >= 0
//   Near:   col[2]          . p >= 0
//   Far:    (col[3]-col[2]) . p >= 0
inline FrustumPlanes ExtractFrustumPlanes(const DirectX::XMFLOAT4X4& viewProj)
{
    const auto& m = viewProj.m;
    auto makePlane = [](float a, float b, float c, float d) -> FrustumPlane
    {
        const float invLen = 1.0f / std::sqrt(a*a + b*b + c*c);
        FrustumPlane p;
        p.normal   = { a * invLen, b * invLen, c * invLen };
        p.distance = d * invLen;
        return p;
    };
    FrustumPlanes out;
    out[0] = makePlane(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]); // Left
    out[1] = makePlane(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]); // Right
    out[2] = makePlane(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]); // Bottom
    out[3] = makePlane(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]); // Top
    out[4] = makePlane(m[0][2],           m[1][2],           m[2][2],           m[3][2]);           // Near
    out[5] = makePlane(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]); // Far
    return out;
}

// Test an AABB against the BoundingFrustum. Uses DirectXCollision for correctness.
inline bool FrustumTestAABB(const DirectX::BoundingFrustum& frustum,
                             const DirectX::XMFLOAT3& aabbMin,
                             const DirectX::XMFLOAT3& aabbMax)
{
    DirectX::BoundingBox box;
    box.Center  = { (aabbMin.x + aabbMax.x) * 0.5f,
                    (aabbMin.y + aabbMax.y) * 0.5f,
                    (aabbMin.z + aabbMax.z) * 0.5f };
    box.Extents = { (aabbMax.x - aabbMin.x) * 0.5f,
                    (aabbMax.y - aabbMin.y) * 0.5f,
                    (aabbMax.z - aabbMin.z) * 0.5f };
    return frustum.Intersects(box);
}

// Overload for the plane set ExtractFrustumPlanes returns: tests, per plane, the
// AABB corner furthest along the inward normal.
inline bool FrustumTestAABB(const FrustumPlanes& frustum,
                             const DirectX::XMFLOAT3& aabbMin,
                             const DirectX::XMFLOAT3& aabbMax)
{
    for (const auto& p : frustum)
    {
        float px = (p.normal.x >= 0) ? aabbMax.x : aabbMin.x;
        float py = (p.normal.y >= 0) ? aabbMax.y : aabbMin.y;
        float pz = (p.normal.z >= 0) ? aabbMax.z : aabbMin.z;
        if (p.normal.x * px + p.normal.y * py + p.normal.z * pz + p.distance < 0)
            return false;
    }
    return true;
}

// Test a world-space SPHERE against the six planes. The secondary-view light cull
// is the caller: a punctual light is a position plus a radius, and outside the
// exact frustum it cannot reach a rasterised pixel.
//
// ExtractFrustumPlanes normalises every plane and its normals point INWARD, so
// `dot(n, c) + d` is the SIGNED DISTANCE from the plane and `< -r` is "the whole
// sphere is on the outside". Both facts are load-bearing: an unnormalised plane
// makes the comparison against -r meaningless, and outward normals would invert
// the test into "keep nothing". Works for the orthographic case for the same
// reason the AABB overload does - the planes come from a clip matrix, not from
// a slope-based BoundingFrustum.
inline bool FrustumTestSphere(const FrustumPlanes& frustum,
                              const DirectX::XMFLOAT3& center, float radius)
{
    for (const auto& p : frustum)
    {
        if (p.normal.x * center.x + p.normal.y * center.y +
            p.normal.z * center.z + p.distance < -radius)
            return false;
    }
    return true;
}

struct RenderView
{
    DirectX::XMFLOAT4X4 viewMatrix;
    DirectX::XMFLOAT4X4 projMatrix;         // jittered: 3D scene rendering
    DirectX::XMFLOAT4X4 viewProjMatrix;     // jittered: 3D scene rendering
    DirectX::XMFLOAT4X4 projMatrixNoJitter;     // un-jittered: ImGuizmo / overlay
    DirectX::XMFLOAT4X4 viewProjMatrixNoJitter; // un-jittered: ImGuizmo / overlay
    DirectX::XMFLOAT3   cameraPosition;
    DirectX::XMFLOAT3   cameraForward;   // unit forward vector (into scene)
    float               nearZ    = 0.1f;
    float               farZ     = 1000.f;
    FrustumPlanes              frustum{};
    DirectX::BoundingFrustum   boundingFrustum{}; // DirectXMath frustum for AABB tests
};

// DrawPacket - plain descriptor of one draw call. Produced by
// Renderer::BuildRenderScene (mesh + material analysis); consumed by
// RenderPass::Execute, where the pass selects and batch-switches PSOs from its own
// PSOCache by the permutation. No VB/IB pointers: vertex data is fetched from
// ByteAddressBuffer SRVs.
struct DrawPacket
{
    uint32_t       meshDescriptorIndex = 0;  // slot in global MeshDescriptor buffer
    uint32_t       instanceOffset      = 0;  // first instance in InstanceBuffer
    uint32_t       instanceCount       = 1;
    uint32_t       vertexOrIndexCount  = 0;  // passed to DrawInstanced
    uint32_t       materialIndex       = 0;
    PermutationKey permutation{};            // active shader features for this material
    DrawFilter     filter              = DrawFilter::Opaque;
    uint64_t       sortKey             = 0;
    uint64_t       texBaseColor        = 0;  // GPU SRV handle for base-color texture (0 = default white)
    uint64_t       texSurfaceMap       = 0;  // GPU SRV handle for surface map (R=roughness, G=metalness)
    uint64_t       texNormalMap        = 0;  // GPU SRV handle for normal map (0 = default flat normal)
    float          outlinePixels       = 2.0f; // per-material outline thickness (only used for DrawFilter::Custom)
    bool           screenSpaceOutline  = true; // enable screen-space edge detection for this draw
    // Editor selection outline: written into ObjectID with depth-test OFF +
    // bit-31 tag, painted yellow-green by OutlinePass's screen-space sub-pass.
    // Skips the inverted-hull sub-pass entirely.
    bool           isPickingOutline    = false;
    uint8_t        stencilRef          = 1;    // stencil value: 1=PBR, 2=NPR (written in GBuffer pass)
    // 1 when this batch's instances have a negative-determinant (mirrored) world
    // matrix. Passes that CULL feed it into rs.frontCounterClockwise so BACK cull keeps
    // culling the back face; flipping the cull MODE instead would leave SV_IsFrontFace
    // inverted and break the DOUBLE_SIDED normal flip in custom_surface.hlsli.
    // Batch key - see DrawCandidate::mirrored.
    uint8_t        mirrored            = 0;
    // Cast-side rasterizer cull override for ShadowPass. Raw storage of
    // ShadowCullMode enum (0=Default back-cull, 1=Front, 2=None); the enum
    // definition lives in ECS/Components.h to avoid an include from the
    // Renderer-layer RenderTypes.h back up into ECS.
    uint8_t        shadowCullMode      = 0;
    // When false, ShadowPass skips this draw entirely: the material opted out of
    // casting shadows via MaterialComponent::CAST_SHADOW. The packet still lives in
    // the Opaque / Transparent draw lists for GBuffer / Lighting.
    uint8_t        castShadow          = 1;
    // Author intent: the views this packet takes part in (ViewBit in
    // HierarchyComponents.h). Each pass ANDs its own bit against viewMask before
    // submitting. The default ~0u keeps emitters that do not read VisibilityComponent
    // (lights, billboards, beams) visible everywhere.
    uint32_t       viewMask            = 0xFFFFFFFFu;
    // 0 = entity opted out of the main / GBuffer pass (e.g. shadow-only proxies).
    uint8_t        renderInMainPass    = 1;
    // 1 when this draw belongs to the player character (root or a descendant).
    // PerObjectShadowPass renders ONLY these packets into the high-res player
    // shadow map; every other pass ignores the flag.
    uint8_t        perObjectShadowCaster = 0;
    // 1 when this draw belongs to a HairShadowCasterComponent subtree (the bangs).
    // The Hair variant of PerObjectShadowPass renders ONLY these into the small
    // bangs -> face shadow map, and HairShadowMarkPass re-draws them as the
    // screen-space fringe-shadow proxy; every other pass ignores the flag (the bangs
    // keep casting normally into CSM / the player map).
    uint8_t        hairShadowCaster      = 0;
    // 1 when this draw belongs to a FaceOverlayComponent subtree (eyebrows /
    // eyelashes). FaceOverlayPass re-draws ONLY these over the lit scene where hair
    // covers them (brows and eyes showing through the hair); every other pass ignores
    // the flag.
    uint8_t        faceOverlay           = 0;
    // Per-draw FaceOverlayComponent settings packed into one root constant:
    // low 16 bits = alpha UNORM; high 16 bits = max distance in [0.005, 0.5] m.
    uint32_t       faceOverlayParams     = 0;
    uint32_t       prevPosElementBase  = 0xFFFFFFFFu; // TAA: prev skinned pos base element (0xFFFFFFFF = none)
    // Depth-prepass eligibility: 1 only when NOTHING in this draw can discard a
    // fragment, so a depth-only pass may pre-populate its depth safely. Cleared by
    // ALPHA_TEST (clip lives in the PS), by any custom PS (may discard), and by an
    // active per-instance DISSOLVE (also a PS clip). Do not write depth for a fragment
    // the GBuffer will later clip: early-Z rejects the geometry behind it and the
    // clipped pixel never writes colour, leaving a black hole.
    uint8_t        depthPrepassEligible = 0;
    // 1 when ANY instance in this batch is mid-dissolve. Dissolve is per-instance and
    // deliberately NOT part of the batch key, so a dissolving batch and an intact one
    // can share the same (meshDescriptorIndex, materialIndex). Passes that key off that
    // pair - the material surface cache bake - must tell them apart, because
    // MS_ApplyDissolve runs unconditionally in the PS and would burn dissolve holes
    // into a cache page that outlives the effect. Distinct from depthPrepassEligible,
    // which folds this together with ALPHA_TEST and custom-PS and so cannot recover the
    // term.
    uint8_t        batchHasDissolve    = 0;
    // This caster's shadow is drawn by its material's OWN pixel shader (the
    // SHADOW_HOOK variant): a masked (ALPHA_TEST) material with a custom PS, whose
    // coverage only that PS knows. The shadow passes draw it in their per-draw loops
    // (ShadowDrawsPerDraw below, RenderGraph/RenderPass/ShadowCasterShaders.h). Set at
    // emit; 0 under -shadow_hook_off (the caster then uses Shadow.ps).
    uint8_t        shadowHookPS        = 0;
    // ceil(the entry's clusters over ALL LOD levels / 32) - the amplification
    // dispatch's X - when the packet is GEOMETRICALLY eligible for the meshlet path (a
    // library mesh whose MLET is resident, not skinned, no custom VS); 0 = never
    // meshlets. Decided at gather (DrawCandidate::meshletBlocks); whether the path is
    // taken this frame is the render half's latch (GBufferPass::MeshletEligible). Fits
    // uint16 by construction: a dispatch dimension is capped at 65535.
    uint16_t       meshletBlocks       = 0;

    // customPSID / customVSID: custom pixel / vertex shader overrides, 0 = the pass
    // default (GBuffer_PS / GBuffer_VS). Resolved upstream (Renderer::BuildRenderScene)
    // from the material's customShaderPath / customVertexShaderPath via
    // ShaderLibrary::RegisterDynamic; the pass plugs them into PSODesc.psID / vsID.
    // Independent: a material may override either stage, both or neither.
    uint32_t       customPSID          = 0;
    uint32_t       customVSID          = 0;
    // GPU VA of the packed custom-material CBV of this draw (root parameter
    // RHI::Slots::Gfx::CustomMatCBV, register b8 space0). 0 = the custom PS declares no
    // cbuffer or the material has no custom shader: the bind is skipped.
    uint64_t       customCbvVA         = 0;
    // GPU handle of the packed custom-material texture table (root parameter
    // RHI::Slots::Gfx::CustomMatTex, t0-t7 space3). 0 = no custom textures: the bind is
    // skipped.
    uint64_t       customTexTable      = 0;

    // Build a sort key from permutation bits (pass-bucket), material, and depth.
    static uint64_t MakeSortKey(uint16_t permBits, uint16_t materialBits, uint32_t depthBits)
    {
        return ((uint64_t)permBits      << 48)
             | ((uint64_t)materialBits  << 32)
             | ((uint64_t)depthBits);
    }
};

using DrawList = std::span<const DrawPacket>;

// Per-PSO group for ExecuteIndirect batching: a contiguous range of
// IndirectDrawCommands that share every key field below (permutation, stencil ref,
// mirrored, depth-prepass eligibility, meshlet, custom shaders and their bindings).
// The GBuffer pass switches PSO once per group before the indirect call.
struct IndirectGroup
{
    PermutationKey perm;
    uint8_t        stencilRef     = 1;
    uint8_t        mirrored       = 0;  // -> rs.frontCounterClockwise for the group
    // Part of the group KEY, so every command in a group agrees: the depth prepass
    // either covers the whole group or skips it. A dissolving packet therefore splits
    // the group - slightly more groups, but the prepass runs per GROUP through
    // ExecuteIndirect instead of per draw on the CPU.
    uint8_t        depthPrepassEligible = 0;
    // The group's commands are meshlet draws: the cull CS turns them into
    // DISPATCH_MESH(blocks, survivors, 1) (meta bit 31) and GBufferPass executes them
    // with the mesh PSO. Part of the key.
    uint8_t        meshlet        = 0;
    uint32_t       customPSID     = 0;  // 0 = default GBuffer_PS
    uint32_t       customVSID     = 0;  // 0 = default GBuffer_VS
    uint64_t       customCbvVA    = 0;  // 0 = no custom CBV bind
    uint64_t       customTexTable = 0;  // 0 = no custom texture table
    uint32_t       argOffset      = 0;  // byte offset into the indirect arg buffer
    uint32_t       cmdCount       = 0;  // number of IndirectDrawCommands in this group
};

// The CSM 4-bucket shadow classification. ONE definition for BOTH layouts -
// ShadowPass's per-frame UPLOAD arg ring and the Renderer's retained shadow-group
// meta fill (SortAndEmit) - so a packet lands in the same bucket on either path.
// Every input is a batch-key field and material / author-static; the one per-FRAME
// rule, the per-object-caster CSM exclusion, stays out (the ring path applies it
// CPU-side in ShadowPass::classify, the retained path in the cull CS via
// kInstanceFlagPerObjShadow, since the decision lands after SortAndEmit).
namespace ShadowDrawGroup
{
    enum : int
    {
        OpaqueBack  = 0,   // ShadowCullMode::Default -> BACK cull (bias anti-acne)
        OpaqueFront = 1,   // ShadowCullMode::Front   -> closed-mesh characters
        OpaqueNone  = 2,   // ShadowCullMode::None    -> cloth / single-sided props
        AlphaTest   = 3,   // ALPHA_TEST perm (cull forced NONE)
        Count       = 4,
        Skip        = -1,  // packet casts into no CSM indirect bucket
        // The RETAINED layout (the shadow meta, the per-view working args, the count
        // grid) gives every bucket a meshlet twin at +MeshletOffset. Only the CSM cull
        // views write the twins' commands as DISPATCH_MESH (ShadowPass draws them through
        // ShadowMeshlet.as/.ms); the spot / point views keep DRAW and draw a twin exactly
        // like its base bucket. Classification stays 0..Count-1.
        MeshletOffset = 4,
        RetainedCount = 8,
    };
}

// D3D12 limits of one DispatchMesh: every dimension <= 65535 and X*Y*Z <= 2^22.
// X = cluster blocks, Y = instances.
inline bool MeshletDispatchWithinLimits(uint32_t blocks, uint32_t instances)
{
    return blocks != 0 && blocks <= 65535u && instances != 0 && instances <= 65535u &&
           uint64_t(blocks) * uint64_t(instances) <= (1ull << 22);
}

// A shadow caster the passes draw one by one in their per-draw loops instead of
// through an indirect bucket: its own vertex shader (a custom VS - WPO - needs its
// own VS PSO and b8 params) or its own pixel shader (a masked custom material's
// shadow runs its hook, DrawPacket::shadowHookPS).
inline bool ShadowDrawsPerDraw(const DrawPacket& dp)
{
    return dp.customVSID != 0 || dp.shadowHookPS != 0;
}

// A shadow caster the CSM meshlet path can draw - a pure function of the packet
// (library mesh with an MLET, not drawn per draw, not skinned, no billboard, no
// dissolve, within one DispatchMesh). Whether the path runs this frame is the
// Renderer's latch (SceneEmit::shadowMeshletFrameActive); the bucket is still
// ClassifyShadowDrawGroup's.
inline bool ShadowMeshletEligible(const DrawPacket& dp)
{
    return dp.meshletBlocks != 0 &&
           !ShadowDrawsPerDraw(dp) &&
           dp.batchHasDissolve == 0 &&
           !dp.permutation.Has(PermutationKey::BILLBOARD) &&
           dp.prevPosElementBase == 0xFFFFFFFFu &&     // skinned draws carry a prev-pos base
           MeshletDispatchWithinLimits(dp.meshletBlocks, dp.instanceCount);
}

// Returns the ShadowDrawGroup bucket of @p dp, or Skip. @p shadowAnyMask =
// ViewBit::ShadowAny, passed in because this header must not include ECS (the same
// rule as the raw shadowCullMode byte, whose values mirror ECS ShadowCullMode:
// 0 Default, 1 Front, 2 None).
inline int ClassifyShadowDrawGroup(const DrawPacket& dp, uint32_t shadowAnyMask)
{
    if (!dp.castShadow)                  return ShadowDrawGroup::Skip;
    // Custom vertex-shader casters cannot share the single Shadow_VS indirect PSO, nor
    // can a masked custom material share Shadow.ps (its hook decides the coverage):
    // they render in the passes' dedicated per-draw loops.
    if (ShadowDrawsPerDraw(dp))          return ShadowDrawGroup::Skip;
    // Custom (outline copies) never reach the shadow passes' Opaque / Shadow /
    // Transparent union; the retained meta fill walks ALL packets, so the guard lives
    // here.
    if (dp.filter == DrawFilter::Custom) return ShadowDrawGroup::Skip;
    // Same reason, and it is load-bearing rather than defensive: a secondary-view
    // duplicate is a SECOND packet for an entity whose ORIGINAL candidate is
    // already in the Shadow list, so admitting it here would draw that caster
    // into every cascade twice (double-shading its depth, doubling its cost) -
    // and this fill is the one place that walks every packet regardless of list.
    if (IsSecondaryViewFilter(dp.filter))  return ShadowDrawGroup::Skip;
    if ((dp.viewMask & shadowAnyMask) == 0) return ShadowDrawGroup::Skip;
    // Transparent packets contribute only when alpha-tested; additive sparks /
    // multiply decals would render solid silhouettes. Renderer::ApplyBlendMode
    // clears every forward-translucent packet's castShadow (the alpha-tested
    // ones included) unless -shadow_translucent_casters restores them.
    if (dp.filter == DrawFilter::Transparent &&
        !dp.permutation.Has(PermutationKey::ALPHA_TEST))
        return ShadowDrawGroup::Skip;
    if (dp.permutation.Has(PermutationKey::ALPHA_TEST))
        return ShadowDrawGroup::AlphaTest;
    // Mirrored casters have reversed winding, so BACK / FRONT cull would keep the
    // wrong face and the caster's shadow silhouette comes out of its far side.
    // OpaqueNone is the one winding-INDEPENDENT bucket, so route them there rather
    // than doubling all four buckets into flipped PSO variants. Trade-off: a mirrored
    // caster that explicitly asked for ShadowCullMode::Front loses that request (its
    // bias was tuned for back-face depth) - rare enough to accept.
    if (dp.mirrored) return ShadowDrawGroup::OpaqueNone;
    switch (dp.shadowCullMode)
    {
    case 1:  return ShadowDrawGroup::OpaqueFront;
    case 2:  return ShadowDrawGroup::OpaqueNone;
    default: return ShadowDrawGroup::OpaqueBack;
    }
}

// Does this packet need Shadow.ps at all? The indirect buckets never bind a
// material's custom PS (hook casters are drawn per draw), so the only things that
// can discard a shadow fragment there are the ALPHA_TEST clip and the per-instance
// dissolve clip - and depthPrepassEligible already encodes exactly "nothing here
// can discard" (Renderer_Scene.cpp: !ALPHA_TEST && no custom PS && no dissolve in
// the batch). When EVERY packet a shadow group draws answers false, that group's
// PSO can drop the pixel shader (PSODesc::kNoPixelShader) and the driver keeps
// early-Z depth WRITE enabled for it. All-or-nothing per group: one draw is one
// PSO, so a single dissolving packet keeps the whole group on Shadow_PS.
inline bool ShadowDrawNeedsPS(const DrawPacket& dp)
{
    return dp.depthPrepassEligible == 0;
}

// RenderCamera - camera state forwarded from the game layer to the Renderer, set
// per frame via Renderer::SetCamera() before BeginFrame / Render. `position` +
// `forward` come straight from the camera entity's GlobalTransform; the Renderer
// builds the view matrix with XMMatrixLookToLH (no Euler angles). The defaults
// look from (4,3,5) at the origin.
struct RenderCamera
{
    DirectX::XMFLOAT3 position = { 4.f, 3.f, 5.f };
    DirectX::XMFLOAT3 forward  = { -0.566f, -0.424f, -0.707f }; // normalized world-space view dir
    // World-space up. Lets cinematic / camera-motion cameras express ROLL (banked /
    // Dutch-angle shots, common in MMD camera VMDs). Defaults to world-up (no roll).
    DirectX::XMFLOAT3 up       = { 0.f, 1.f, 0.f };
    float             fov      = DirectX::XM_PI / 3.f;
    float             nearZ    = 0.1f;
    float             farZ     = 200.f;
    // False for one frame on a hard cut (Camera.HardCutTo, initial spawn, cinematic
    // shot boundary). Temporal passes (TAA, XeGTAO, VolumetricFog, SSR) AND this with
    // their internal heuristic, wired by the camera stack pipeline so every
    // temporal-history effect clears in lockstep. Defaults to true.
    bool              historyValid = true;
    // Orthographic projection (editor axis-locked Top / Front / Side views). When set,
    // `fov` is ignored and the view volume is a box of vertical half-extent
    // `orthoHalfHeight` (horizontal follows the viewport aspect) spanning nearZ..farZ
    // along `forward`. The Renderer disables TAA / FSR jitter, sky / cloud / fog
    // composites and screen-space AO for ortho frames: their perspective-only math
    // would produce artifacts.
    bool              orthographic    = false;
    float             orthoHalfHeight = 10.f;
};
