#pragma once

// PerObjectShadowPass: one high-resolution directional shadow map fitted tightly to the
// player character, composited over the CSM (directional light only; spot / point are
// untouched). Like SpotShadowPass, but one Texture2D target, one caster CB and per-draw
// rendering (the player is a handful of packets, so no ExecuteIndirect).
//
// Each frame the Renderer finds the player (PlayerComponent) and merges its world AABB,
// builds a reversed-Z ortho light VP fitted to it along the sun, writes the transposed VP to
// LightCB.perObjShadowMatrix (+ enabled = 1) and hands the raw VP here (SetShadowMatrix /
// SetEnabled), and tags the player's DrawPackets perObjectShadowCaster = 1. This pass clears
// its map, renders only the eligible tagged packets depth-only (DrawPacket::castShadow and the
// shadow viewMask are authoritative) and leaves the map in DEPTH_READ_SRV for Lighting.ps
// (shadow.hlsli SamplePerObjectShadow, gated on LightCB.perObjShadowParams.x).
//
// Shadow_VS / Shadow_PS are the CSM / spot depth-only pair, so a skinned player casts its
// animated silhouette (PVF FETCH_POS reads the per-frame skinned stream like the GBuffer VS).
// Reversed-Z contract (as ShadowSystem / SpotShadowPass): clear 0.0f, depthFunc
// GREATER_EQUAL, ortho near = 0 / far = large, negative slope-scaled bias, VP transposed on
// upload, gShadowSampler (s2).
// Root slots (the shared graphics root signature): b1 space0 ShadowPerViewCB (the fitted
// light VP), t0 space0 InstanceBuffer, t1 space0 MeshDescriptors, t0 space1 bindless
// g_Buffers[]. Lighting reads the map through LightCB.perObjShadowIdx (hairShadowIdx for the
// Hair variant).

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/PassTextureNames.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class PerObjectShadowPass : public RG::RenderPass
{
public:
    // High-res map for the player + its cast shadow. 4096^2 D32 = 64 MB: the extra linear
    // resolution gives headroom for a larger fitted footprint while keeping the texel density
    // high (texelWorld = 2*fitR / kShadowMapSize).
    static constexpr uint32_t kShadowMapSize = 4096;
    // Map size for the hair (bangs -> face) variant. Its frustum is fitted to a head-sized bangs
    // AABB (fitR well under a metre), so 1024^2 D32 (4 MB) still gives sub-millimetre texels on
    // the face.
    static constexpr uint32_t kHairShadowMapSize = 1024;

    // Which caster set this instance renders:
    //   Player - packets tagged DrawPacket::perObjectShadowCaster (the whole character subtree),
    //            read through LightCB.perObjShadow*.
    //   Hair   - packets tagged DrawPacket::hairShadowCaster (the bangs subtree only), read
    //            through LightCB.hairShadow* by NPR face pixels only (bangs shadow on the face,
    //            Lighting.ps).
    enum class Variant : uint8_t { Player, Hair };

    explicit PerObjectShadowPass(Variant variant = Variant::Player);
    ~PerObjectShadowPass();

    const char* GetName() const override
    { return m_variant == Variant::Hair ? "HairShadowPass" : "PerObjectShadowPass"; }
    // The map is this instance's import: RG::PassTex::kShadowPerObject, or kShadowHair for the
    // Hair variant. Created in Init; the graph transitions it (Graph tracking).
    void DeclareImports(RG::ImportDeclarer& d) override
    {
        m_mapH = d.Import(m_variant == Variant::Hair ? RG::PassTex::kShadowHair : RG::PassTex::kShadowPerObject,
                          { RHI::Format::D32_FLOAT, m_mapSize, m_mapSize, true, false, false, 1 },
                          { &m_map, RG::ImportEntry::Stale, RG::ImportTracking::Graph });
    }
    void Setup (RG::RenderGraphBuilder& b) override { b.WriteDepthStencil(m_mapH); }
    void Init  (IGraphicsDevice& gfx)    override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Mirrors Execute's two early-outs, including the one-shot
    // DEPTHSTENCIL -> SRV transition the first disabled frame still owes
    // Lighting.ps (m_firstExecution). Both instances (Player and Hair) use it.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_map.IsValid() && (m_enabled || m_firstExecution));
    }
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }

    // Renderer calls these each frame before the graph executes. Enabled = a player exists this
    // frame AND the feature is on; when false the pass renders nothing and the map keeps its
    // (unsampled) prior content.
    void SetEnabled(bool e)                             { m_enabled = e; }
    void SetShadowMatrix(const DirectX::XMFLOAT4X4& vp) { m_pendingVP = vp; }
    // Scene time + camera for custom-VS (WPO) player materials + dissolve noise
    // index; written into the caster CB at byte offsets 192/208/224 (matches
    // custom_vertex.hlsli CUSTOM_VS_SHADOW + Shadow.ps ShadowPerViewCB).
    void SetCustomVsTime(float t, float dt, const DirectX::XMFLOAT3& camPos,
                         uint32_t dissolveNoiseIdx = ~0u,
                         uint32_t instanceRemapIdx = 0)
    { m_cvTime = t; m_cvDeltaTime = dt; m_cvCameraPos = camPos;
      m_dissolveNoiseIdx = dissolveNoiseIdx;
      m_instanceRemapIdx = instanceRemapIdx; }

    // Read by LightingPass (LightCB.perObjShadowIdx for Player, hairShadowIdx for Hair). 0 until
    // Init succeeds.
    uint64_t GetShadowSrvHandle() const;
    uint32_t GetMapSize() const { return m_mapSize; }

    // A DrawPacket's customVSID / customPSID resolves in this library as in every other (one
    // DynamicShaderRegistry); the pass compiles them in CUSTOM_VS_SHADOW / SHADOW_HOOK mode.
    ShaderLibrary& GetShaderLibrary() { return m_shaderLib; }

private:
    static constexpr uint32_t kCasterCBStride = RHI::kConstantBufferAlignment; // D3D12 CBV alignment
    struct CasterCBSlot { uint8_t bytes[kCasterCBStride]; };

    // depthOnly drops the PS entirely (PSODesc::kNoPixelShader): legal only when no packet in
    // the group can discard (see ShadowDrawNeedsPS). hookPSID != 0: a masked custom material's
    // own PS in its SHADOW_HOOK variant, fed by its custom VS or ShadowHook_VS
    // (ShadowCasterShaders.h).
    PSODesc BuildPSODesc(PermutationKey perm, uint32_t customVSID = 0,
                         bool depthOnly = false, uint32_t hookPSID = 0) const;
    void    UploadCasterCB();

    ShaderLibrary m_shaderLib;
    PSOCache      m_psoCache;

    float             m_cvTime      = 0.0f;
    float             m_cvDeltaTime = 0.0f;
    DirectX::XMFLOAT3 m_cvCameraPos = { 0.0f, 0.0f, 0.0f };
    // Dissolve noise bindless index (caster CB offset 224; ~0u = none).
    uint32_t          m_dissolveNoiseIdx = ~0u;
    // Instance-remap heap index (caster CB offset 228; 0 = identity).
    uint32_t          m_instanceRemapIdx = 0;

    RHI::Texture m_map;  // Texture2D<D32_FLOAT>, single slice
    RG::RGTextureHandle m_mapH;   // its import

    // One 256B caster CB (transposed VP @0, WPO time/cam @192). Triple-buffered
    // so frame N+1's CPU upload can't race frame N's GPU read.
    FrameCB<CasterCBSlot> m_casterCB;

    DirectX::XMFLOAT4X4 m_pendingVP{};
    bool                m_enabled = false;

    Variant  m_variant = Variant::Player;
    uint32_t m_mapSize = kShadowMapSize;   // set from m_variant in the ctor

    IGraphicsDevice* m_gfxPtr = nullptr;

    // Linear-wrap sampler for alpha-test shadow PS (matches GBuffer.ps's g_LinearWrap).
    int  m_alphaSamplerIdx = -1;

    // The first execution transitions from the create-time DEPTHSTENCIL state; later ones from
    // DEPTH_READ_SRV.
    bool m_firstExecution = true;
};
