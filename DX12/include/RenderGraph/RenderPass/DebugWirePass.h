#pragma once

// DebugWirePass - wireframe debug visualization: AABBs, frustum, colliders, navmesh
// and light-range gizmos. Owned by the Renderer, not a RenderGraph pass.
//
// Drawn after tonemap into the display-res final output, the overlay slot of
// WorldUIBillboardPass / DebugIconPass / UIPass. Do not move it before
// post-processing: debug lines write no motion vectors, so TAA/FSR2 would reproject
// them by whatever geometry sits behind them and they smear with camera movement;
// exposure would also scale their colour and bloom blur them.
//
// The target is display-res while the scene depth is render-res, and D3D12 cannot
// pair mismatched RTV/DSV sizes, so depth testing is done manually in
// DebugWireDepth.ps.hlsl against a depth SRV.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/FrameCB.h"
#include <DirectXMath.h>
#include <vector>

class IGraphicsDevice;

class DebugWirePass : public RG::PassLifecycle
{
public:
    // Alpha multiplier applied to occluded pixels of depth-aware lines.
    static constexpr float kXRayAlpha = 0.22f;

    void Init(IGraphicsDevice& gfx);
    // Hot-reload entry (Renderer::ReloadShaders calls it): clears the shader library
    // and the PSO cache; the next Execute rebuilds.
    void ReloadShaders();
    void ReloadShaders(IGraphicsDevice&) override { ReloadShaders(); }
    uint32_t HandledEvents() const override { return kReloadShaders; }

    // Add an AABB wireframe (12 edges = 24 vertices).
    void AddAABB(const DirectX::XMFLOAT3& mn, const DirectX::XMFLOAT3& mx, uint32_t color);

    // Add an oriented box: the local box [-halfExtents,+halfExtents] transformed by
    // `worldMatrix` (row-major, DX-style; last row = translation). The decal debug
    // overlay draws a decal's projection volume with it (the unit cube [-0.5,0.5]^3 in
    // decal-local space, see decal_common.hlsli). 12 edges.
    void AddOBB(const DirectX::XMFLOAT4X4& worldMatrix,
                const DirectX::XMFLOAT3& halfExtents, uint32_t color);

    // Add a frustum wireframe (12 edges = 24 vertices) from 8 corner points.
    void AddFrustum(const DirectX::XMFLOAT3 corners[8], uint32_t color);

    // Add a capsule wireframe (two end circles + connecting lines).
    void AddCapsule(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b,
                    float radius, uint32_t color, int segments = 12);

    // Add a sphere wireframe: three great circles in the XY, YZ and XZ planes.
    // `segments` controls per-circle smoothness; 16 gives a recognisable sphere
    // silhouette from any viewing angle.
    void AddSphere(const DirectX::XMFLOAT3& center, float radius,
                   uint32_t color, int segments = 16);

    // Add a 3-axis cross marker centred on @p pos with arms of length @p size. The DDGI
    // probe debug view draws one per probe, so the user sees where the volume's grid
    // lands in the scene.
    void AddCross(const DirectX::XMFLOAT3& pos, float size, uint32_t color);

    // Raw line: public so external subsystems (NavMesh wireframe, custom gizmos) can
    // push edges without the AABB / Frustum helpers. Always on top: no depth test.
    void AddLine(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, uint32_t color);

    // Depth-aware line: full alpha where nothing occludes it, faded to kXRayAlpha where
    // scene geometry is nearer. Occlusion is the strongest cue that a gizmo sits IN the
    // world; without it a wireframe seems to swim with the camera. Fading rather than
    // discarding keeps a light buried behind a wall findable. Used by the light-range
    // gizmos.
    // Emit each edge ONCE: the fade is a pixel-shader decision. These lines live in
    // their own vertex buffer, drawn second with their own PSO; without a depth SRV in
    // Execute they draw with the plain overlay PSO.
    void AddLineDepthTested(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b,
                            uint32_t color);

    // False -> Execute() is a guaranteed no-op. `enabled` alone is not enough:
    // EditorLayer force-enables the pass just so Clear() caches the ring slot, so an
    // enabled pass with zero lines is routine. Check this before assuming Execute leaves
    // the target in SHADER_RESOURCE.
    bool HasLines() const { return (m_vertexCount + m_depthVertexCount) > 0; }

    // Clear all lines (call at the start of the frame). Also caches this frame's mapped
    // vertex pointer so AddLine's hot path avoids a per-line virtual call (defined in
    // the .cpp: needs the full IGraphicsDevice).
    void Clear();

    // Uploads the lines and draws them into @p target (the composition buffer: the
    // tonemap final output), like DebugIconPass::Execute: @p entryState -> RENDERTARGET,
    // draw, -> SHADER_RESOURCE.
    // @p viewProjNoJitter: un-jittered VP (this pass runs after TAA; jitter would only
    // wobble the lines). @p canvasW / @p canvasH: target size (display res).
    // @p sceneDepthSrv: render-res scene depth SRV for the manual occlusion test; 0
    // disables it (depth-aware lines then draw as plain overlays). @p depthW / @p depthH:
    // that depth texture's size (render res).
    void Execute(RHI::CommandList            cl,
                 const DirectX::XMFLOAT4X4&  viewProjNoJitter,
                 const RHI::Texture*         target,
                 RHI::ResourceState          entryState,
                 uint32_t                    canvasW,
                 uint32_t                    canvasH,
                 uint64_t                    sceneDepthSrv,
                 uint32_t                    depthW,
                 uint32_t                    depthH);

    // Format of that target: push ToneMapPass::GetFinalOutputFormat() before Execute
    // (m_targetFormat says why a literal will not do).
    void SetTargetFormat(RHI::Format f) { m_targetFormat = f; }

private:
    struct LineVertex
    {
        float    x, y, z;
        uint32_t color; // RGBA8 packed
    };
    static_assert(sizeof(LineVertex) == 16);

    // Mirror of DebugWireCB in shaders/debug_wire_common.hlsli. viewProj is
    // stored TRANSPOSED (HLSL cbuffers pack column-major).
    struct WireCB
    {
        DirectX::XMFLOAT4X4 viewProj;
        float               targetSize[4]; // xy = target px, zw = depth px
        float               params[4];     // x = x-ray alpha, y = depth bias
    };

    // 2 M vertices = 1 M line segments: sized for a full-scene collision wireframe (a
    // Bistro-scale ~1500 entities x ~300 tris x 3 edges passes 1 M edges) plus navmesh
    // and the usual AABB/frustum overlays. 32 MiB per ring slot. AddLine drops lines
    // past it silently.
    static constexpr uint32_t kMaxVertices = 2097152;

    // Depth-aware lines get their OWN buffer rather than a tail region of the overlay
    // one, so both draws start at vertex 0 and nothing depends on SV_VertexID including
    // DrawInstanced's StartVertexLocation - when it does not, the second draw reads the
    // FRONT of the buffer (zero-filled upload memory -> degenerate alpha-0 lines -> the
    // gizmos vanish). 256 K verts = 128 K segments ~ 700 spot lights at ~170 segments
    // each; 4 MiB per ring slot.
    static constexpr uint32_t kMaxDepthVertices = 262144;

    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    // Allocates the two UPLOAD rings on the first debug-wire frame, not at Init:
    // together 96 MiB + 12 MiB = 108 MiB of persistently mapped upload heap (non-pageable
    // system RAM) that a shipping Game.exe which never draws a gizmo must not pay (as
    // ParticleSystem::EnsurePoolResources does for its 128 MiB pool). Called from
    // Clear(), the single main-thread entry of a debug-wire build, already gated on
    // `enabled`; AddLine's null guard covers a failed allocation as it covers overflow.
    void EnsureRings();

    // @p depthAware false -> plain overlay PS; true -> the occlusion-fade PS. Two PSO
    // cache entries (psID is part of the hash).
    PSODesc BuildPSODesc(bool depthAware) const;

public:
    bool showCapsules = false;

    bool enabled              = false;
    bool showAABBs            = false;
    bool showFrustum          = false;
    bool showReflectionProbes = false;
    bool showDDGIVolumes      = false;  // when true, AABB + per-probe crosses
    bool showDecals           = false;  // decal projection-volume OBBs + facing arrow
    bool  showCollision         = false;  // Mesh ColliderComponent wireframe
    bool  showPhysics           = false;  // Jolt bodies by sleep state + KCC capsules (PhysicsSystem::DrawDebug)
    // <= 0 = unlimited range (every Mesh collider in the world). DebugDrawSystem mirrors
    // its own value here (that one is what the collider and nav-soup emitters use, and the
    // Debug menu's slider - shown while the Collision or Nav Soup category is on - edits
    // it). Default: no cull (full-scene visibility); set a radius when frame time matters.
    float collisionMaxDistance  = 0.0f;

private:
    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    bool m_ringsCreated = false;

    // UPLOAD and written every frame -> one ring slot per frame in flight.
    RHI::GPUBuffer   m_vertexBuffer[kFrameCount];
    void*            m_vertexMapped[kFrameCount] = {};
    uint32_t         m_vertexCount      = 0;

    RHI::GPUBuffer   m_depthVertexBuffer[kFrameCount];
    void*            m_depthVertexMapped[kFrameCount] = {};
    uint32_t         m_depthVertexCount = 0;

    // Current frame's mapped write target, refreshed by Clear(). Lets AddLine (called
    // once per wireframe edge: millions on a full-scene collision overlay) skip the
    // per-call virtual GetFrameIndex() + null checks.
    LineVertex*      m_curVerts      = nullptr;
    LineVertex*      m_curDepthVerts = nullptr;

    FrameCB<WireCB>  m_cb;   // un-jittered viewProj + sizes (b1 space0)

    // RTVFormats[0] for the composition buffer this pass blends into. That buffer
    // follows the HDR output mode (RGBA8_UNORM in SDR, FP16 in HDR10 / scRGB:
    // HDRCompositionFormat, Graphics/HDROutput.h) and the RTV silently inherits the
    // texture's real format, so a hardcoded one is a debug-layer error and undefined
    // output with no compile error. The Renderer pushes the live format via
    // SetTargetFormat before Execute. PSODesc::Hash mixes rtvFormats, so the SDR and HDR
    // variants coexist in the PSO cache instead of evicting each other on a mode change.
    RHI::Format      m_targetFormat = RHI::Format::R8G8B8A8_UNORM;
};
