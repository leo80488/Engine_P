#pragma once

// BillboardFXPass - draws BillboardFXComponent entities as animated camera-facing sprite-sheet
// quads into the HDR scene colour (pre-tonemap), depth-tested read-only so scene geometry occludes
// them and Bloom picks up the emissive / additive glow.
// Self-contained like WorldUIBillboardPass: the CPU expands one quad per effect, with the current
// flipbook frame's atlas sub-rect, into a per-frame UPLOAD vertex ring. HDR + depth binding as in
// TransparentPass (SetRenderTargetToHdrWithDepth, reversed-Z test, no depth write).
// BuildFrame(world, dt, ...) runs on the Renderer each frame before the graph executes (it needs
// World + dt to advance flipbooks and resolve textures); Execute(cl) binds the ring and draws.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's expansion)
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FogApply.h"
#include "Resource/TextureSystem.h"   // Resource::TextureHandle / kInvalidTextureHandle
#include "ECS/BillboardFXComponent.h"

#include <DirectXMath.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

class World;
namespace Resource { class ResourceManager; }

class BillboardFXPass : public RG::RenderPass
{
public:
    // 40-byte vertex x 6 per quad; 24576 vertices = 4096 quads per frame.
    static constexpr uint32_t kMaxVertices = 24576;

    explicit BillboardFXPass(RG::RGTextureHandle depth);
    // The opaque depth snapshot (SSRTraceDepth) the soft fade reads through FogApplyCB's heap index,
    // declared so the graph knows. Set before the graph compiles.
    void SetOpaqueDepthSnapshot(RG::RGTextureHandle h) { m_opaqueDepth = h; }

    const char* GetName() const override { return "BillboardFXPass"; }
    void Setup  (RG::RenderGraphBuilder& b)       override;
    void Init   (IGraphicsDevice& gfx)            override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's first early-out (`!enabled || draws.empty()`, the bound view's). The
    // per-frame-slot VB mapping test below it needs GetFrameIndex, which is
    // device state rather than settled pass state, so it stays in Execute.
    bool IsActive() const override
    { return !RG::PassActivityGatesEnabled() || (enabled && !m_x->draws.empty()); }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Texture-path -> bindless-index resolution (optional; null = untextured).
    void SetResourceSystems(Resource::TextureSystem* texSys,
                            Resource::ResourceManager* resMgr)
    { m_texSys = texSys; m_resMgr = resMgr; }

    // CPU prep: advance flipbooks, resolve textures, expand camera-facing quads into this frame's
    // vertex ring and record draw ranges. Called by the Renderer before graph execute. viewProj is not
    // passed: the VS reads it from the shared PerView CB (b1) like GBuffer.vs; viewMatrix gives the
    // camera right / up axes, cameraPos the back-to-front sort.
    void BuildFrame(World& world, float dt,
                    const DirectX::XMFLOAT4X4& viewMatrix,
                    const DirectX::XMFLOAT3&   cameraPos);
    // The same frame's sprites expanded for secondary view @p view's camera, into its own ring (game half,
    // after BuildFrame). Its expansion exists once the view was bound (Renderer::ServiceViews).
    void ExpandView(ViewState& view, const DirectX::XMFLOAT4X4& viewMatrix, const DirectX::XMFLOAT3& cameraPos);
    // Execute draws the bound view's expansion.
    void BindView(ViewState& view) override;

    // Releases the entity's cached texture handle, so a recycled Entity ID never inherits a stale
    // bindless slot and refcounts stay balanced (as in ParticleSystem). The Renderer calls it from the
    // destroy-listener fan-out, and OnWorldClear on reload (World::Clear fires no per-entity destroy
    // listeners). TextureSystem::Release defers GPU destruction, so mid-frame calls are safe.
    void OnEntityDestroyed(uint32_t entity);
    void OnWorldClear(const RG::WorldClearArgs&) override;
    // Events this pass handles (Renderer::PassSet); it records any view (its expansion is the view's).
    uint32_t HandledEvents() const override { return kWorldClear | kView | kViewSafe; }

private:
    // 40-byte vertex: pos(12) + uv(8) + HDR colour float4(16) + texIdx(4).
    struct Vertex
    {
        float    pos[3];
        float    uv[2];
        float    color[4];
        uint32_t texIdx;
    };
    static_assert(sizeof(Vertex) == 40, "BillboardFX Vertex must be 40 bytes");

    static constexpr uint32_t kInvalidTexIdx = 0xFFFFFFFFu;

    // One DrawInstanced range = a run of consecutive sorted quads that share a
    // (blend, depthTest) PSO.  Painter order is preserved across the run.
    struct DrawRange
    {
        uint32_t         startVertex;
        uint32_t         count;
        BillboardFXBlend blend;
        bool             depthTest;
    };

    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;     // == GraphicsDX12::kFrameCount

    // One gathered sprite (BuildFrame): what every view's expansion places.
    struct Rec
    {
        DirectX::XMFLOAT3 pos;
        float             half;
        float             u0, v0, u1, v1;
        float             color[4];
        uint32_t          texIdx;
        BillboardFXBlend  blend;
        bool              depthTest;
        BillboardFXFace   face;
    };
    // One view's expansion: its per-frame UPLOAD vertex ring and this frame's draw ranges.
    struct Expansion
    {
        RHI::GPUBuffer         vertexBuffer[kFrameCount];
        void*                  vbMapped[kFrameCount] = {};
        std::vector<DrawRange> draws;
    };
    struct ViewExpansion : ViewData
    {
        Expansion expansion;
        void Release(IGraphicsDevice& gfx) override;
    };
    void CreateRing(Expansion& x);
    // m_recs sorted back to front for the camera, expanded into @p x's ring at the upload slot.
    void Expand(Expansion& x, const DirectX::XMFLOAT4X4& viewMatrix, const DirectX::XMFLOAT3& cameraPos);

    struct TexCache { std::string path; Resource::TextureHandle handle = Resource::kInvalidTextureHandle; };

    PSODesc BuildPSODesc(BillboardFXBlend blend, bool depthTest) const;

    static void EmitQuad(std::vector<Vertex>& out,
                         const DirectX::XMFLOAT3& center,
                         const DirectX::XMFLOAT3& right,
                         const DirectX::XMFLOAT3& up,
                         float halfW, float halfH,
                         float u0, float v0, float u1, float v1,
                         const float color[4], uint32_t texIdx);

    // Resolves texturePath -> bindless index like ParticleSystem: cached per entity, the old handle
    // released on a path change, the bindless index set once the async load is ready.
    void ResolveTexture(uint32_t entity, BillboardFXComponent& b);

public:
    bool enabled = true;

private:
    IGraphicsDevice*    m_gfx = nullptr;
    RG::RGTextureHandle m_depth;
    RG::RGTextureHandle m_opaqueDepth;   // soft fade (SetOpaqueDepthSnapshot)
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;

    // The main view's expansion; m_x is the bound view's (BindView).
    Expansion  m_main;
    Expansion* m_x = &m_main;
    std::vector<Rec> m_recs;   // this frame's sprites (BuildFrame)

    // Texture resolution dependencies (optional; null = no texture support).
    Resource::TextureSystem*   m_texSys = nullptr;
    Resource::ResourceManager* m_resMgr = nullptr;

    std::unordered_map<uint32_t, TexCache> m_texCache;
};
