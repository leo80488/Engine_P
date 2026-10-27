#pragma once

// CloudPass - Nubis/Frostbite-style volumetric clouds.
// Once, on the first enabled frame: bake the base shape (128^3 RGBA8, R = Perlin-Worley,
// GBA = Worley FBM octaves), the detail erosion noise (32^3 RGBA8) and the weather map
// (512^2 RGBA8: coverage / fill / type).
// Per frame: CloudRaymarch (compute, quarter res, RGBA16F: rgb = premultiplied scatter,
// a = transmittance; spherical-shell march, adaptive steps, IGN jitter, multi-scatter
// octaves, dual-lobe HG), CloudTemporal accumulates it, CloudComposite blends it into HDR
// (src ONE, dst SRC_ALPHA), CloudShadowBake bakes the sun-direction shadow map.
// Renderer pushes the camera, the sun (TODOutputComponent, or the directional light when TOD
// is off) and the CloudComponent fields (SetParams) every frame.

#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <vector>
#include <DirectXMath.h>

struct CloudComponent;

class CloudPass : public RG::RenderPass
{
public:
    // HLSL CB layout: keep in sync with CloudCB in CloudCommon.hlsli. Each row is exactly 16
    // bytes: HLSL pads before a float3 when the row has < 12 bytes left, so every row that
    // precedes a float3 packs 4 scalars.
    struct alignas(16) CloudConstants
    {
        float invViewProj[16];                                    // rows 0..3

        float cameraPos[3];      float nearZ;                     // row 4
        float farZ;              float bottomAltitude;
        float topAltitude;       float coverage;                  // row 5
        float density;           float baseNoiseScale;
        float detailNoiseScale;  float detailStrength;            // row 6
        float weatherScale;      float cloudTypeBias;
        float anvilBias;         float extinction;                // row 7

        float sunDir[3];         float ambientStrength;           // row 8
        float sunColor[3];       float phaseFwdG;                 // row 9
        float ambientTint[3];    float phaseBackG;                // row 10
        float cloudColor[3];     float phaseBlend;                // row 11
        float windOffset[3];     float silverIntensity;           // row 12

        float silverSpread;      float frameIndex;
        float maxSteps;          float maxTraceDist;              // row 13

        float halfResW;          float halfResH;
        float fullResW;          float fullResH;                  // row 14

        // Temporal reconstruction: the PREVIOUS frame's jittered view-projection, transposed like
        // invViewProj. SetCamera derives it by inverting invViewProj: with
        // invViewProj = transpose(inverse(VP)), its inverse is transpose(VP), the form the shader's
        // row-vector mul wants, so there is no second matrix convention to keep in sync.
        float prevViewProj[16];                                   // rows 15..18

        float resetHistory;      float temporalBlend;
        // ambientLightLum: luminance of the light that lights the sky (SetSun's
        // skyColor) - the skylight term's scale, 0 at night.
        float temporalOn;        float ambientLightLum;           // row 19

        // Cloud-shadow basis. csRight / csUp are orthonormal and BOTH perpendicular to sunDir: that
        // makes the projection altitude-invariant, so one 2D map shadows every receiver under the
        // deck. cloudShadowExtent is the HALF-size in metres.
        float csRight[3];            float cloudShadowExtent;    // row 20
        float csUp[3];               float cloudShadowDim;       // row 21
        float cloudShadowCenter[3];  float cloudShadowStrength;  // row 22
    };

    explicit CloudPass(RG::RGTextureHandle depth);
    ~CloudPass();

    const char* GetName() const override { return "CloudPass"; }
    // The cloud shadow map (RG::PassTex::kCloudShadow) is this pass's import, made lazily here
    // and transitioned by the graph. Readers before this pass (Lighting, Water, WaterSurface)
    // get last frame's map, readers after it (VolumetricFog, Transparent) this frame's.
    // TODO(RenderGraph_AutoTopology_NodeView_Plan M7): declare the cloud colour's reader (the
    // lens flare in the post chain); Execute still decides which texture it gets (the raymarch
    // target or an accumulation slot).
    void DeclareImports(RG::ImportDeclarer& d) override;
    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // The per-frame flag resets and the history drop live in PrepareFrame, which the graph runs
    // whether or not the pass records (RenderPass::PrepareFrame); this is Execute's own early-out.
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_enabled && m_paramsEnabled && Shown() && m_gfx != nullptr);
    }
    // A view that hides the clouds (wireframe, debug view modes, ortho camera, the path
    // tracer's primary) skips the pass; PrepareFrame drops its history.
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Clouds); }
    void PrepareFrame() override;

    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }
    // Clearing the library and the cache is not enough: the six compute PSOs are members built
    // once and the noise volumes are baked once. Rebuilds the PSOs and re-bakes.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Each view owns its accumulation (History), created empty in BindView, and its raymarch targets
    // and camera (Frame); RebuildCloudTexture makes its textures. It records any view; the cloud
    // shadow map (sun-relative, the world's) is baked by the main view only.
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

    // ---- Renderer-pushed per-frame state ----
    void SetEnabled(bool on) { m_enabled = on; }
    bool IsEnabled() const   { return m_enabled; }

    // Per-frame camera. invViewProj uses the LightCB convention (row vector:
    // mul(ndc4, invViewProj), then divide by w).
    // The main view's (game half).
    void SetCamera(const DirectX::XMFLOAT4X4& invViewProj,
                   const DirectX::XMFLOAT3&   cameraPos,
                   float nearZ, float farZ);
    // Secondary view @p view's (its segment, render half).
    void SetViewCamera(ViewState& view, const DirectX::XMFLOAT4X4& invViewProj,
                       const DirectX::XMFLOAT3& cameraPos, float nearZ, float farZ);

    // dir points TOWARDS the sun (unit). color is the linear-RGB light reaching the cloud layer
    // (sun or moon, after the atmosphere above the layer; zero in the planet's shadow). skyColor
    // lights the sky dome at the layer; its luminance scales the clouds' skylight term.
    void SetSun(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color,
                const DirectX::XMFLOAT3& skyColor);

    // Copies every CloudComponent author field; dt (s) advances the wind offset.
    void SetParams(const CloudComponent& c, float dt);

    // SRV of the cloud colour (a = view-ray transmittance) LensFlarePass uses for sun occlusion.
    // 0 unless the raymarch ran THIS frame, so disabled / wireframe / failed-PSO frames never
    // serve stale overcast.
    uint64_t GetCloudSrvHandle() const;

    // SRV of the sun-direction cloud shadow map, or 0 when it was not baked this frame (clouds
    // off, wireframe, strength 0). Renderer turns it into LightCB.cloudShadowIdx, where 0 means
    // full sun, so a stale map never leaks into a frame without clouds.
    uint64_t GetCloudShadowSrvHandle() const;

    // The two pre-folded basis rows LightCB carries for cloud_shadow.hlsli:
    // xyz = axis / (2 x extent), w = 0.5 - dot(centre, xyz). Only meaningful when
    // GetCloudShadowSrvHandle() is non-zero; read both together.
    void GetCloudShadowRows(float outRight[4], float outUp[4]) const;

private:
    static constexpr uint32_t kBaseNoiseDim   = 128;
    static constexpr uint32_t kDetailNoiseDim = 32;
    static constexpr uint32_t kWeatherDim     = 512;
    // Max march distance (m) from the shell entry point (UE default mode).
    static constexpr float    kMaxTraceDist   = 30000.0f;

    static constexpr uint32_t kCloudShadowDim = 512;

    // Steady-state history weight of the temporal accumulator. 0.9 ~ a 10-frame window: long
    // enough to average the 8-frame jitter sequence several times, short enough that a
    // clipped / rejected history recovers in a few frames.
    static constexpr float    kTemporalBlend  = 0.90f;
    // Camera jump (metres/frame) past which the history is thrown away rather
    // than reprojected. Same guard VolumetricFogPass uses for teleports.
    static constexpr float    kTeleportDist   = 500.0f;

    struct History final : ViewHistory
    {
        // Temporally accumulated quarter-res result, ping-ponged. This, not the frame's cloudTex, is what the
        // composite and the lens flare read whenever temporal reconstruction is on.
        RHI::Texture       accum[2];
        RHI::ResourceState accumState[2] = { RHI::ResourceState::UNORDERED_ACCESS,
                                             RHI::ResourceState::UNORDERED_ACCESS };
        uint32_t           accumRead = 0, accumWrite = 1;
        bool               valid = false;
        // Monotonic frame counter; mod 8 drives the IGN jitter sequence (TAA runs after the cloud
        // composite and accumulates it).
        uint32_t            frameIndex = 0;
        // transpose(VP) and the camera position of the frame the history holds:
        // the reprojection and the teleport guard.
        DirectX::XMFLOAT4X4 prevViewProjT{};
        DirectX::XMFLOAT3   prevCameraPos{};

        const char* Name() const override { return "Cloud"; }
        // Resets on the content's cuts (hard cut, camera cut, world clear, as the view reports them)
        // and on a new render size (RebuildCloudTexture notices it). The teleport guard
        // (kTeleportDist), a shader reload, a singular camera, switching temporal on and a skipped
        // frame are the pass's own policy.
        uint32_t ResetsOn() const override { return HistoryReset::Cuts | HistoryReset::Resize; }
        void Invalidate(uint32_t reasons) override { if (reasons & HistoryReset::Cuts) valid = false; }
        void Release(IGraphicsDevice& gfx) override;
    };

    // SRV source for the composite / lens flare: the accumulated texture when temporal
    // reconstruction ran this frame, the raw raymarch otherwise.
    const RHI::Texture& ResolvedCloudTex() const;

    void RebuildCloudTexture(uint32_t fullW, uint32_t fullH);
    void CreateComputePipelines(IGraphicsDevice& gfx);
    void BakeNoiseOnce(RHI::CommandList cl);
    bool UploadCB();   // false: no CB this frame (the frame's constants ran out)
    void DispatchRaymarch(RHI::CommandList cl);
    bool DispatchTemporal(RHI::CommandList cl);
    void DispatchShadowBake(RHI::CommandList cl);

    // True when the cloud shadow should exist at all: clouds on, the feature on and strength > 0.
    // Drives both the per-frame bake and whether the map stays allocated.
    bool ShadowWanted() const;
    void EnsureCloudShadowTexture();
    void ReleaseCloudShadowTexture();
    void Composite(RHI::CommandList cl);

    bool m_enabled = false;

    RG::RGTextureHandle m_depth;

    IGraphicsDevice* m_gfx = nullptr;
    ShaderLibrary    m_shaderLib;
    PSOCache         m_psoCache;

    RHI::PipelineState m_noiseBakePSO;
    RHI::PipelineState m_detailBakePSO;
    RHI::PipelineState m_weatherBakePSO;
    RHI::PipelineState m_raymarchPSO;
    RHI::PipelineState m_temporalPSO;
    RHI::PipelineState m_shadowBakePSO;

    // Sun-direction cloud shadow map (R16_FLOAT). Baked every frame the clouds run; read through
    // LightCB's bindless index by the passes that declare RG::PassTex::kCloudShadow.
    RHI::Texture       m_cloudShadowTex;
    RG::RGTextureHandle m_cloudShadowH;   // its import
    // The pass's own book - used only without the import (-rgimportoff).
    RHI::ResourceState m_cloudShadowState = RHI::ResourceState::UNORDERED_ACCESS;
    bool               m_shadowBakedThisFrame = false;
    // Basis rows in the form cloud_shadow.hlsli consumes (GetCloudShadowRows).
    float              m_shadowRightRow[4] { 0, 0, 0, 0 };
    float              m_shadowUpRow[4]    { 0, 0, 0, 0 };

    // Baked-once lookup textures.
    RHI::Texture       m_baseNoiseTex;    // 128^3 RGBA8
    RHI::Texture       m_detailNoiseTex;  // 32^3  RGBA8
    RHI::Texture       m_weatherTex;      // 512^2 RGBA8
    RHI::ResourceState m_baseNoiseState   = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_detailNoiseState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::ResourceState m_weatherState     = RHI::ResourceState::UNORDERED_ACCESS;
    bool               m_noiseBaked = false;

    // One view's raymarch targets, their size latch, its per-frame flags and its camera.
    struct Frame
    {
        // Quarter-res raymarch output, RGBA16F.
        RHI::Texture       cloudTex;
        RHI::ResourceState cloudState = RHI::ResourceState::UNORDERED_ACCESS;
        // Quarter-res scene distance each texel marched against (R32F): the composite's
        // depth-aware upsample weights.
        RHI::Texture       cloudDistTex;
        RHI::ResourceState cloudDistState = RHI::ResourceState::UNORDERED_ACCESS;
        uint32_t           cloudW = 0, cloudH = 0;
        uint32_t           lastFullW = 0, lastFullH = 0;
        // True only when DispatchTemporal ran and wrote the history's accumWrite this frame;
        // decides what ResolvedCloudTex() hands out.
        bool               accumulatedThisFrame = false;
        // True only between a successful DispatchRaymarch and the next PrepareFrame: gates
        // GetCloudSrvHandle so consumers never read a stale frame.
        bool               renderedThisFrame = false;
        // The camera, captured by SetCamera / SetViewCamera.
        DirectX::XMFLOAT4X4 invViewProj{};
        // transpose(VP) for this frame (the previous one is the history's); see CloudConstants.
        DirectX::XMFLOAT4X4 viewProjT{};
        DirectX::XMFLOAT3   cameraPos{};
        float               nearZ = 0.f;
        float               farZ  = 0.f;
    };
    struct ViewFrame : ViewData
    {
        Frame frame;
        void Release(IGraphicsDevice& gfx) override;
    };
    // @p f's camera and its transpose(VP); a singular camera drops @p hist (when there is one).
    static void SetCameraOf(Frame& f, History* hist, const DirectX::XMFLOAT4X4& invViewProj,
                            const DirectX::XMFLOAT3& cameraPos, float nearZ, float farZ);

    // The view's cloud history: one per view, created empty by BindView. RebuildCloudTexture
    // makes the accumulation textures together with the view's raymarch target and march distance
    // (from the first enabled Execute).
    History* m_hist     = nullptr;   // the bound view's (BindView)
    History* m_mainHist = nullptr;   // the main view's, for SetCamera on the game half
    // The main view's frame; m_f is the bound view's.
    Frame    m_mainFrame;
    Frame*   m_f = &m_mainFrame;

    // This frame's CloudConstants block, written by UploadCB and bound by the dispatches and
    // the composite (b3 space0); 0 = none.
    uint64_t m_cbVA = 0;
    DirectX::XMFLOAT3   m_sunDir   { 0.f, 1.f, 0.f };
    DirectX::XMFLOAT3   m_sunColor { 1.f, 1.f, 1.f };
    float               m_skyLightLum = 1.f;   // Rec.601 luma of SetSun's skyColor

    // Cloud author state (defaults mirror CloudComponent).
    bool                m_paramsEnabled = false;
    float               m_bottomAlt     = 1500.f;
    float               m_topAlt        = 4000.f;
    float               m_coverage      = 0.5f;
    float               m_density       = 1.f;
    float               m_baseNoiseScale   = 0.00025f;
    float               m_detailNoiseScale = 0.002f;
    float               m_detailStrength   = 0.30f;
    float               m_weatherScale     = 0.00002f;
    float               m_cloudTypeBias    = 0.f;
    float               m_anvilBias        = 0.f;
    DirectX::XMFLOAT3   m_windDir       { 1.f, 0.f, 0.f };
    float               m_windSpeed     = 8.f;
    float               m_windOffset    = 0.f;  // accumulated wind distance, m
    float               m_phaseFwdG     = 0.6f;
    float               m_phaseBackG    = -0.2f;
    float               m_phaseBlend    = 0.3f;
    float               m_silverIntensity = 0.8f;
    float               m_silverSpread    = 0.25f;
    float               m_extinction    = 0.02f;
    float               m_ambient       = 0.06f;
    DirectX::XMFLOAT3   m_ambientTint   { 0.55f, 0.65f, 0.85f };
    DirectX::XMFLOAT3   m_cloudColor    { 1.f, 1.f, 1.f };
    float               m_maxSteps      = 128.f;
    bool                m_temporalUpsample = true;
    bool                m_cloudShadowEnabled  = true;
    float               m_cloudShadowStrength = 1.f;
    float               m_cloudShadowExtent   = 20000.f;
};
