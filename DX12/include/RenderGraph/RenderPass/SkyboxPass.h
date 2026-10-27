#pragma once

// SkyboxPass - draws the environment cubemap as the sky after the lighting pass,
// with the analytic sun disk, the moon and a baked starfield.
//
// The VS drops translation (w = 0 position) and forces clip z = 0, the far plane
// under reversed Z; with a GREATER_EQUAL test and no depth write the sky draws only
// where no geometry did. Cull NONE draws the inside of the cube.
//
// Renderer supplies the environment cubemap each frame via SetEnvMap().

#include "RenderGraph/RenderGraph.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <cmath>
#include <DirectXMath.h>

// HLSL-layout shadow of the per-frame SkyCB (b2 space0), in the header so
// FrameCB<SkyCBData> can be a member of SkyboxPass.
struct alignas(16) SkyCBData
{
    float    sunDir[3];    float sunDiskSize;       // cos(half-angle of sun core)
    float    sunColor[3];  float sunDiskIntensity;
    float    moonDir[3];   float moonDiskCos;       // cos(half-angle of moon disk)
    float    moonColor[3]; float moonVisible;       // 1 = draw moon, 0 = skip
    float    starIntensity;                          // [0,1] day/night blend
    float    starTime;                               // seconds, twinkle phase
    float    starDensity;                            // grid resolution
    float    starBrightness;                         // overall multiplier
};

namespace RG { class RenderContext; }

class SkyboxPass : public RG::RenderPass
{
public:
    // The base name every instance's GetName() starts with, spelled in one place
    // (like RTReflectionPass::kOpaqueName).
    static constexpr const char* kBaseName = "SkyboxPass";

    // ---- The sky state, as a copyable POD ----
    struct SkyState
    {
        bool                valid = false;
        DirectX::XMFLOAT3   sunDir  { 0.f, 1.f, 0.f };
        DirectX::XMFLOAT3   sunColor{ 0.f, 0.f, 0.f };
        float               sunDiskCos       = 0.99998f;
        float               sunDiskIntensity = 0.f;
        uint64_t            moonTex          = 0;
        bool                moonVisible      = false;
        DirectX::XMFLOAT3   moonDir  { 0.f, 1.f, 0.f };
        DirectX::XMFLOAT3   moonColor{ 0.f, 0.f, 0.f };
        float               moonDiskCos      = 0.99998f;
        float               starIntensity    = 0.f;
        float               starTime         = 0.f;
        float               starDensity      = 256.f;
        float               starBrightness   = 0.7f;
        // The OWNER's baked star cube, for a borrower to bind. 0 = none yet.
        uint64_t            starsCubeSrv     = 0;
    };

    explicit SkyboxPass(RG::RGTextureHandle depth);

    // Instance name. RenderGraph keys the external-wait map, the PIX marker and the
    // GPU timestamp region on GetName(), and GetGPUTimestampResult is a
    // FIRST-MATCH-WINS linear name scan, so two instances sharing a name hand every
    // consumer the union of both. With no suffix it is kBaseName.
    const char* GetName() const override { return m_name.c_str(); }
    // Call BEFORE the pass is handed to a RenderGraph - see GBufferPass.h.
    void SetNameSuffix(const char* suffix);

    void Setup  (RG::RenderGraphBuilder& b)   override;
    void Init   (IGraphicsDevice& gfx)        override;
    // Releases what this instance owns: its library, cube VB, SkyCB ring, stars
    // bake PSO / CB and (if created) the stars cubemap.
    void Shutdown(IGraphicsDevice& gfx)        override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // A view that hides the sky (RenderGraph/SceneView.h) - the main view's
    // wireframe, debug view modes, ortho camera and the path tracer's camera primary;
    // the preview's wireframe and debug views - skips the pass, and the background
    // stays the cleared colour.
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Sky); }
    // Hot-reload. Clearing these two is not enough on its own: the stars bake
    // PSO is a member and the cube it bakes is baked once, so ReloadShaders
    // also rebuilds that PSO and re-bakes.
    ShaderLibrary*         GetReloadableShaderLibrary() override { return &m_shaderLib; }
    std::vector<PSOCache*> GetReloadablePSOCaches()     override { return { &m_psoCache }; }
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // The cube drawn as the sky, set each frame: by Renderer, SkyIBLPass::
    // ResolveSkyboxSrvHandle's choice (material sky, atmosphere cube or the static
    // skybox); by a preview, its backdrop. 0 skips the skybox draw.
    void SetEnvMap(uint64_t gpuHandle) { m_envMapGpuHandle = gpuHandle; }

    // The 36-vertex cube ByteAddressBuffer, borrowed by external passes (e.g.
    // reflection probe capture) to reuse the geometry without re-uploading; invalid
    // before Init().
    const RHI::GPUBuffer& GetCubeVB() const { return m_cubeVB; }

    // Per-frame sun parameters for the analytic sun disk: @p dir unit vector
    // towards the sun; @p color linear radiance (time-of-day attenuation included);
    // @p diskHalfAngleRad the visible disk's angular radius (~0.0087 rad = 0.5 deg,
    // widened slightly for a cleaner silhouette); @p diskIntensity its brightness.
    void SetSun(const DirectX::XMFLOAT3& dir,
                const DirectX::XMFLOAT3& color,
                float diskHalfAngleRad,
                float diskIntensity)
    {
        m_sunDir           = dir;
        m_sunColor         = color;
        m_sunDiskCos       = std::cos(diskHalfAngleRad);
        m_sunDiskIntensity = diskIntensity;
    }

    // Per-frame moon state. The disk is drawn whenever @p visible is true and
    // @p texHandle is non-zero; its direction and colour are tracked apart from
    // m_sunDir, so the moon keeps drawing as it sinks below the horizon (and before it
    // takes over as the directional light). @p dir points TOWARDS the moon, unit.
    void SetMoon(uint64_t texHandle,
                 bool visible,
                 const DirectX::XMFLOAT3& dir,
                 const DirectX::XMFLOAT3& color,
                 float diskHalfAngleRad)
    {
        m_moonTexHandle = texHandle;
        m_moonVisible   = visible;
        m_moonDir       = dir;
        m_moonColor     = color;
        m_moonDiskCos   = std::cos(diskHalfAngleRad);
    }

    // Per-frame starfield state. @p intensity is the day / night blend (0 = day,
    // invisible; 1 = full night); 0 skips the star branch entirely. @p time
    // accumulates game seconds and drives per-star twinkle. @p density and
    // @p brightness are tuning knobs.
    void SetStars(float intensity, float time,
                  float density = 256.0f, float brightness = 0.7f)
    {
        m_starIntensity  = intensity;
        m_starTime       = time;
        m_starDensity    = density;
        m_starBrightness = brightness;
    }

    // The current-frame slot of the triple-buffered SkyCB, for Renderer to register
    // with the RenderGraph each frame. Rebind every frame: the buffer rotates.
    const RHI::GPUBuffer& GetSkyCB(IGraphicsDevice& gfx) const { return m_skyCB.CurrentBuffer(gfx); }

    // What the sun disk, the moon and the starfield are THIS FRAME, in the stored
    // (cos-folded) form the CB upload uses, so a second instance can be handed the
    // first one's answer without re-deriving it from half-angles and without touching
    // a GPU resource.
    //
    // THE SNAPSHOT DIRECTION IS THE POINT. The preview must never read the main
    // pass's per-frame SkyCB RING: that is another graph's rotating per-frame state,
    // which must not be cached across the seam (the rule that keeps ClusterPass's SRVs
    // out of the preview). These are the CPU inputs the ring is written FROM - the
    // same shape as Renderer::PreviewMainShadowSnapshot, which borrows the CSM the
    // same way.
    SkyState GetSkyState(IGraphicsDevice& gfx) const;
    // Adopts a snapshot wholesale. The cos fields go in RAW: re-deriving them through
    // SetSun / SetMoon would take an acos and a cos of an exact value for nothing.
    // `starsCubeSrv` becomes the BORROWED cube (m_borrowedStarsSrv): a borrowing
    // instance never allocates a 48 MiB cubemap of its own.
    void ApplySkyState(const SkyState& s);

    // Enable-gated star cubemap residency. Call ONLY at the frame boundary (OnService,
    // from Renderer::ServiceEnableGatedResources, before any recording): creating /
    // destroying a texture mints and frees descriptors, which is illegal once the
    // graph's worker recorders are live. Driven by the last SetStars() intensity: a sky
    // that never reaches night never allocates the cube.
    void EnsureStarsResources();
    // The service point: the star cube, driven by the day / night blend SetStars
    // pushes (a sky that never reaches night never allocates it).
    void OnService(RG::ServiceContext&) override { EnsureStarsResources(); }
    // Per-world release (Renderer::OnWorldClear, real scene changes only).
    void ReleaseStarsResources();
    // The pass events Renderer::PassSet delivers; kViewSafe: its CB is the view's ring, the rest the scene's.
    uint32_t HandledEvents() const override { return kShutdown | kWorldClear | kViewSafe; }
    // The star cubemap is a per-world lazy resident: released with the world
    // that needed it, not on a feature-idle timer.
    void OnWorldClear(const RG::WorldClearArgs& a) override { if (!a.worldSurvives) ReleaseStarsResources(); }

private:
    // Star cube texels per face, deliberately 1024. StarsBake.cs.hlsl bakes each star as an
    // angular disk of coreCos 0.9999995..0.9999981 (~0.06..0.11 deg radius), constants
    // derived from this resolution (one texel ~0.088 deg, so a star is ~1.3-2.5 texels
    // across). At 256^2 a texel spans ~0.35 deg and a small star is under ONE texel: the
    // `cosD >= coreCos` test at texel centres would drop most stars or leave a single texel,
    // and the alpha-packed twinkle phase, which must stay flat across a star's multi-texel
    // footprint under bilinear sampling, stops working. Shrinking the cube is a star-size
    // retune (a visual change), not a memory-only edit.
    static constexpr uint32_t kStarsCubeSize = 1024;

    PSODesc BuildPSODesc() const;
    void CreateStarsBakePipeline(IGraphicsDevice& gfx);

    // See GetName() / SetNameSuffix.
    std::string         m_name = kBaseName;
    ShaderLibrary       m_shaderLib;
    PSOCache            m_psoCache;
    RG::RGTextureHandle m_depth;

    // Cube vertex buffer - 36 float3 positions (pre-expanded, no index buffer).
    RHI::GPUBuffer      m_cubeVB;

    // The sky cube to draw - set via SetEnvMap() each frame.
    uint64_t            m_envMapGpuHandle = 0;

    // Sun disk state (written each frame via SetSun, uploaded into m_skyCB).
    FrameCB<SkyCBData>  m_skyCB;
    DirectX::XMFLOAT3   m_sunDir           { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3   m_sunColor         { 0.0f, 0.0f, 0.0f };
    float               m_sunDiskCos       = 0.99998f;   // cos(0.5 deg)
    float               m_sunDiskIntensity = 40.0f;

    // Moon: the disk is drawn whenever the moon is within the horizon-fade band,
    // not gated on the "active body" lighting switch.
    uint64_t            m_moonTexHandle    = 0;
    bool                m_moonVisible      = false;
    DirectX::XMFLOAT3   m_moonDir          { 0, -1, 0 };
    DirectX::XMFLOAT3   m_moonColor        { 0.55f, 0.70f, 1.0f };
    float               m_moonDiskCos      = 0.9994f;    // cos(~2 deg), wider than the sun

    // Starfield state - Renderer pushes it per frame via SetStars().
    float               m_starIntensity    = 0.0f;
    float               m_starTime         = 0.0f;
    float               m_starDensity      = 256.0f;
    float               m_starBrightness   = 0.5f;

    // A BORROWED star cube, for an instance that must not own one. The cube is
    // 1024^2 x6 RGBA16F = 48 MiB, baked once and left in SHADER_RESOURCE for the life
    // of the world, so a preview instance binds the main pass's instead of allocating
    // and baking a second copy. 0 for the owning instance, which uses m_starsCubemap.
    // A borrower never runs EnsureStarsResources (its only caller is OnService, which
    // the main Renderer's PassSet runs and the preview does not), so its
    // m_starsCubemap stays invalid - that keeps the two paths apart.
    //
    // A DANGLING BORROW IS NOT REACHABLE. The owner drops the cube in
    // ReleaseStarsResources (Renderer::OnWorldClear) through
    // IGraphicsDevice::DestroyTexture - the frame-stamped DEFERRED bucket - so the
    // descriptor outlives every frame that could have named it. The borrower re-takes
    // the snapshot on EVERY record (PreviewRenderer::RecordWorldCullSetup), so it
    // reads 0 from the next frame on. One frame of a handle the owner stopped using is
    // the worst case, and that frame is inside the deferred window by construction.
    uint64_t            m_borrowedStarsSrv = 0;

    // Pre-baked star cubemap, sampled every frame by the skybox PS at t18 space0
    // (Lighting's SSAO slot: the skybox does not read SSAO, and the lighting pass
    // rebinds it next frame).
    //
    // RESIDENCY: 1024^2 x 6 x RGBA16F = 48 MiB, so it is not created in Init - most
    // scenes never render a night sky (m_starIntensity defaults to 0 and only the
    // ProceduralAtmosphere / moon path in Renderer_IBL drives it up).
    // EnsureStarsResources creates it on the first frame the intensity goes positive;
    // it is released with the WORLD, never on daylight (an in-game dawn would re-bake
    // it every game day, and a frame-counted idle timer depends on the display
    // refresh).
    IGraphicsDevice*    m_gfx = nullptr;      // stashed at Init for lazy create/release
    RHI::Texture        m_starsCubemap;
    RHI::ResourceState  m_starsCubemapState = RHI::ResourceState::UNORDERED_ACCESS;
    RHI::PipelineState  m_starsBakePSO;
    RHI::GPUBuffer      m_starsBakeCB;
    void*               m_starsBakeCBMapped = nullptr;
    bool                m_starsBaked        = false;
};
