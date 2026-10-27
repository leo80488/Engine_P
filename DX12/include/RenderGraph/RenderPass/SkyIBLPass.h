#pragma once

// SkyIBLPass - the sky's image-based lighting, recomputed when the sky changes.
// The source is the procedural Hillaire atmosphere (transmittance / multi-scatter /
// sky-view LUTs -> a 256^2 sky cube) or a custom sky material's cube. It is
// projected into L2 spherical harmonics (LightingPass's diffuse IBL) and
// GGX-prefiltered into a 128^2 x 6, 7-mip specular cube (one face a frame); the
// pass also writes the 32^3 aerial-perspective LUT.
//
// Resource boundary:
//   - Owns the SH buffer (9 x float4, written by SkySHProjection.cs.hlsl), the
//     specular cube (imported as RG::PassTex::kSkySpecularCube) and its async
//     reader's copy, the atmosphere cube and the LUTs.
//   - Does NOT own the static environment cubemap. With neither the atmosphere nor
//     a material sky the pass does nothing and the lighting path uses the static
//     irradiance + radiance cubemaps.

#include "Graphics/FrameConstants.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/ViewState.h"   // ViewData (a secondary view's aerial LUT)
#include "RenderGraph/PassTextureNames.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/FrameCB.h"

#include <array>
#include <cmath>
#include <DirectXMath.h>
#include <functional>
struct MaterialComponent;

class SkyIBLPass : public RG::RenderPass
{
public:
    enum class SkyboxSource : uint8_t { Atmosphere = 0, Static = 1 };

    // Called on the GAME side (Renderer::SyncSkyboxIBL, inside BuildRenderScene),
    // before BeginFrame and before parallel graph recording, so the CB is written
    // through the frame's upload slot (FrameCB::Upload) - Execute reads it back
    // as CurrentBuffer() once the render half starts the same frame.
    // b0/space2: SkyTimeSize then reflected material float parameters (<= 1024 B).
    // t0..t3/space2: reflected custom textures; u0: six-face RGBA16F output.
    void PrepareMaterialSky(const MaterialComponent* material, float time,
                            const std::function<uint64_t(const std::string&)>& texture);
    bool IsMaterialSkyEnabled() const { return m_materialSkyEnabled; }
    SkyIBLPass() = default;

    const char* GetName() const override { return "SkyIBLPass"; }
    // The specular cube is this pass's import (RG::PassTex::kSkySpecularCube;
    // 128^2 x 6, mips): created in Init and prefiltered here (Owner tracking) - one
    // face a frame, while the sky changes. Lighting, the forward passes and the RT
    // passes declare reading it.
    void DeclareImports(RG::ImportDeclarer& d) override
    {
        m_specH = d.Import(RG::PassTex::kSkySpecularCube,
                           { RHI::Format::R16G16B16A16_FLOAT, kSpecularSize, kSpecularSize, false, true, false, 6 },
                           { &m_specTex, RG::ImportEntry::Stale, RG::ImportTracking::Owner });
    }
    void Setup  (RG::RenderGraphBuilder& b)     override
    {
        // Compute-only pass - no render target. The sky sources are set externally;
        // the specular cube it prefilters is its import.
        b.SetColorTarget(RG::BuiltinTexture::None);
        b.WriteUAV(m_specH);
    }
    void Init   (IGraphicsDevice& gfx)          override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // Hot reload: rebuilds the PSOs AND invalidates everything they baked -
    // the static LUTs are baked once and the cube / SH only when the sun
    // moves, so without that the new shaders would not run until then.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    // Mirrors Execute's first three early-outs exactly (PSO / SH buffer validity,
    // then "no atmosphere and no material sky - do NOTHING"). The dirty check below
    // them stays inside Execute: it decides which sub-dispatches run, not whether the
    // pass records, and the dispatches themselves update its inputs
    // (m_lastBakedSunDir).
    bool IsActive() const override
    {
        return !RG::PassActivityGatesEnabled()
            || (m_pso.IsValid() && m_shBuffer.IsValid()
                && (m_atmosphereEnabled || m_materialSkyEnabled));
    }

    // The static environment cubemap's SRV GPU handle (0 = none). Stored only:
    // Execute projects and prefilters the atmosphere cube or a material sky.
    void SetSourceCubemap(uint64_t gpuHandle) { m_srcCubemap = gpuHandle; }

    // GPU SRV handle of the SH coefficient buffer (9 x float4); LightingPass binds
    // it at its shader slot.
    uint64_t GetSHSrvHandle() const { return m_shSrvHandle; }

    // True once an SH projection has produced valid coefficients.
    bool IsSHValid() const { return m_shValid; }

    // GPU SRV handle of the prefiltered specular cubemap (7 mips), valid from Init
    // on; IsSpecularValid() says whether it holds a prefiltered sky yet.
    uint64_t GetSpecularSrvHandle() const { return m_specSrvHandle; }

    // True once the specular cubemap has been prefiltered at least once.
    bool IsSpecularValid() const { return m_specValid; }

    // Mip count of the prefiltered specular cubemap (7 for 128x128).
    uint32_t GetSpecularMipCount() const { return kSpecularMips; }

    // Per-face resolution of mip 0 of the prefiltered specular cubemap.
    static constexpr uint32_t GetSpecularSize() { return kSpecularSize; }

    // The specular texture itself, for external copies (e.g. broadcasting into the
    // reflection-probe cubemap array). The caller must respect its tracked state
    // (GetSpecularState / SetSpecularState) and issue matching barriers around any
    // copy or transition.
    const RHI::Texture* GetSpecularTexture() const { return &m_specTex; }
    RHI::ResourceState  GetSpecularState()   const { return m_specState; }
    void                SetSpecularState(RHI::ResourceState s) { m_specState = s; }

    // The cube an ASYNC-COMPUTE reader samples. DDGI traces on the compute queue
    // while this pass rewrites the specular cube on the graphics queue the same frame,
    // with no edge between them, so DDGI samples a COPY: filled in this pass's own
    // list the frame the cube first becomes valid (nobody has been handed it before),
    // afterwards only by RefreshAsyncCopy on a list ordered after the reader. DDGI's
    // sky is therefore a frame late - inside its hysteresis. Returns the copy when
    // @p live is this pass's specular cube and the copy holds it; anything else (a
    // static or uniform cube, -ddgiskysamecube, a copy that could not be created)
    // comes back unchanged.
    uint64_t AsyncReaderSrv(uint64_t live) const
    {
        return (m_specCopyValid && live != 0 && live == m_specSrvHandle) ? m_specCopySrv : live;
    }
    // Copies every face the prefilter wrote since the last copy into the async
    // reader's cube. Record it on a GRAPHICS list that runs after the reader (the
    // Renderer's promoteCL, which waits on DDGI).
    void RefreshAsyncCopy(RHI::CommandList cl);
    // Whether an async reader (DDGI's list) records this frame; the Renderer sets it
    // before the graph runs. Without one nothing can be sampling the copy, so Execute
    // refreshes it itself - a reader that starts again does not get a sky from
    // whenever it last ran.
    void SetAsyncReaderActive(bool on) { m_asyncReaderActive = on; }

    // Enables the procedural atmospheric sky: the atmosphere cube then drives SH
    // projection + specular prefilter. Disabled (and no material sky), Execute does no
    // per-frame compute and the lighting path falls back to the static irradiance +
    // radiance cubemaps. Re-enabling schedules a full 6-face prefilter so the cube is
    // valid immediately.
    void SetAtmosphereEnabled(bool on)
    {
        if (on && !m_atmosphereEnabled)
            m_prefilterNeedsFullBake = true;
        m_atmosphereEnabled = on;
    }
    bool IsAtmosphereEnabled() const   { return m_atmosphereEnabled; }

    // Skybox backdrop source: Atmosphere = the procedural sky cube, Static = the
    // caller's static .itex handle (ResolveSkyboxSrvHandle's fallback). Default
    // Atmosphere; a valid material sky overrides both.
    void SetSkyboxSource(SkyboxSource s) { m_skyboxSource = s; }
    SkyboxSource GetSkyboxSource() const { return m_skyboxSource; }

    // The cubemap SRV SkyboxPass draws: a valid material sky's cube, else the
    // atmosphere cube (SkyboxSource::Atmosphere with the atmosphere on), else
    // @p staticFallback.
    uint64_t ResolveSkyboxSrvHandle(uint64_t staticFallback) const;

    // Active light direction / colour - the body driving direct lighting (sun by
    // day, moon by night). LightCB convention: `dir` points TOWARDS the body. Pushed
    // each frame by Renderer from the SunLightTag entity (or via TODOutput when TOD
    // is active).
    void SetSunDir(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color)
    {
        m_sunDir = dir;
        m_sunColor = color;
    }

    // The frame being built (Renderer's m_materialFrameEpoch), pushed each frame by
    // Renderer::SyncSkyboxIBL. The temporal prefilter picks its face as frame % 6, so
    // which face holds which frame's sky depends on the frame number alone - not on
    // when the bootstrap ran (it waits for the sky material's textures to load).
    void SetFrameNumber(uint64_t frame) { m_frameNumber = frame; }
    const DirectX::XMFLOAT3& GetSunDir()   const { return m_sunDir; }
    const DirectX::XMFLOAT3& GetSunColor() const { return m_sunColor; }

    // Geometric sun direction / colour - the true sun position even below the
    // horizon, with colour zero there. The atmosphere / aerial-perspective shaders use
    // it so Rayleigh scattering goes dark at night instead of drawing a faint blue
    // dome from the moon's direction.
    void SetAtmosphereSun(const DirectX::XMFLOAT3& dir, const DirectX::XMFLOAT3& color)
    {
        m_atmosphereSunDir   = dir;
        m_atmosphereSunColor = color;
    }

    // Per-frame RELAY of the unified sky-IBL master gate, written every frame by
    // Renderer::SyncSkyboxIBL from WorldEnvironmentComponent.iblStrength (falling back
    // to the deprecated AtmosphereComponent value when unauthored); read back into
    // LightCB.iblStrength and by the probe bake. Gates SKY-DERIVED indirect ONLY - DDGI
    // is decoupled (ddgiDiffuseScale; the knob-ownership contract is in
    // Lighting.ps.hlsl). This pass's own shaders never read it.
    void SetIBLStrength(float s)   { m_iblStrength = s; }
    float GetIBLStrength() const   { return m_iblStrength; }

    // SRV of the procedural sky cubemap (SkyboxPass's backdrop), created in Init;
    // 0 only if its creation failed.
    uint64_t GetAtmosphereSrvHandle() const { return m_atmosphereSrvHandle; }

    // SRV of the 3D aerial-perspective LUT (32x32x32), created in Init. The lighting
    // pass fades distant opaque geometry into atmospheric fog with it; IsAerialValid()
    // says whether it has been written.
    uint64_t GetAerialPerspectiveSrvHandle() const { return m_mainAerial.srv; }
    // The bound view's (a secondary view's segment); the getters around it are the main view's (game half).
    uint64_t GetViewAerialSrvHandle() const        { return m_a->srv; }
    float    GetAerialMaxDistanceKm() const        { return kAerialMaxKm; }

    // True after the first aerial-perspective dispatch. Before it the LUT holds
    // zeros, so the lighting pass must NOT composite it (ap.a = 0 would black out
    // opaque pixels).
    bool IsAerialValid() const { return m_mainAerial.valid; }

    // Enables the aerial-perspective composite in the lighting pass. Default OFF:
    // atmosphere, SH, specular and skybox still work; only the distance-fog composite
    // (and its per-frame LUT dispatch) is skipped. Useful when a scene's units do not
    // match the shader's km assumption.
    void SetAerialCompositeEnabled(bool on) { m_aerialCompositeEnabled = on; }
    bool IsAerialCompositeEnabled() const   { return m_aerialCompositeEnabled; }

    // Per-frame camera state the aerial-perspective LUT needs. Uses the same
    // `invViewProj` convention as LightCB (row-vector matrix).
    // The main view's (game half); SetViewCameraForAerial is a secondary view's (its segment).
    void SetCameraForAerial(const DirectX::XMFLOAT3& cameraPosWorld,
                            const DirectX::XMFLOAT3& cameraForwardWorld,
                            const float invViewProjRowMajor[16])
    {
        SetAerialCamera(m_mainAerial, cameraPosWorld, cameraForwardWorld, invViewProjRowMajor);
    }
    void SetViewCameraForAerial(ViewState& view, const DirectX::XMFLOAT3& cameraPosWorld,
                                const DirectX::XMFLOAT3& cameraForwardWorld, const float invViewProjRowMajor[16]);

    // It records any view: a secondary view runs only the aerial-perspective LUT of its camera (the
    // sky, its cube, SH and prefilter are the sun's, once a frame in the main view).
    uint32_t HandledEvents() const override { return kView | kViewSafe; }
    void BindView(ViewState& view) override;

private:
    struct MaterialSkyCB { uint8_t bytes[1024]{}; };

    // One view's aerial-perspective LUT and the camera it is baked for.
    struct Aerial
    {
        RHI::Texture       tex;
        RHI::ResourceState state = RHI::ResourceState::SHADER_RESOURCE;
        uint64_t           srv   = 0;
        bool               valid = false;
        DirectX::XMFLOAT3  cameraPosWorld     { 0, 0, 0 };
        DirectX::XMFLOAT3  cameraForwardWorld { 0, 0, 1 };
        float              invViewProj[16]    { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    };
    struct ViewAerial : ViewData
    {
        Aerial aerial;
        void Release(IGraphicsDevice& gfx) override;
    };
    static void SetAerialCamera(Aerial& a, const DirectX::XMFLOAT3& pos, const DirectX::XMFLOAT3& fwd,
                                const float invViewProj[16])
    {
        a.cameraPosWorld     = pos;
        a.cameraForwardWorld = fwd;
        for (int i = 0; i < 16; ++i) a.invViewProj[i] = invViewProj[i];
    }
    void CreateAerial(IGraphicsDevice& gfx, Aerial& a);

    static constexpr uint32_t kMaterialSkySize = 512;

    static constexpr uint32_t kSpecularSize       = 128;
    static constexpr uint32_t kSpecularMips       = 7; // log2(128) + 1
    static constexpr uint32_t kAtmosphereSize     = 256;

    // Hillaire 2020 LUT sizes.
    static constexpr uint32_t kTransmittanceW = 256;
    static constexpr uint32_t kTransmittanceH = 64;
    static constexpr uint32_t kMultiScatterW  = 32;
    static constexpr uint32_t kMultiScatterH  = 32;
    static constexpr uint32_t kSkyViewW       = 192;
    static constexpr uint32_t kSkyViewH       = 108;
    static constexpr uint32_t kAerialW        = 32;
    static constexpr uint32_t kAerialH        = 32;
    static constexpr uint32_t kAerialD        = 32;
    static constexpr float    kAerialMaxKm    = 32.0f; // far slice = 32 km

    // CB pool strides / slot counts, here so the pool structs below size at compile
    // time (SkyIBLPass.cpp names the LUT slots). kLutCBStride is 512 because
    // AerialConstants exceeds 256 bytes.
    static constexpr uint32_t kPrefilterCBStride = RHI::kConstantBufferAlignment;
    static constexpr uint32_t kPrefilterCBSlots  = kSpecularMips * 6;
    static constexpr uint32_t kLutCBStride       = 512;
    static constexpr uint32_t kLutCBSlots        = 5;

    // Multi-slot CB pools (the BloomPass.h pattern): one buffer of N x stride bytes,
    // each dispatch writing its own slot, so no CPU write overlaps a slot the GPU
    // reads within the frame; the FrameCB ring covers the frames in flight.
    struct alignas(256) PrefilterCBPool { uint8_t bytes[kPrefilterCBStride * kPrefilterCBSlots]; };
    struct alignas(256) AtmoLutCBPool   { uint8_t bytes[kLutCBStride       * kLutCBSlots];      };

    struct alignas(16) SHConstants
    {
        uint32_t sampleCount;
        uint32_t _pad[3];
    };

    struct alignas(16) AtmosphereConstants
    {
        float    sunDir[3];    float    _pad0;
        float    sunColor[3];  uint32_t faceSize;
        float    cameraAltitudeKm;
        float    _pad1;
        float    _pad2;
        float    _pad3;
    };

    bool CreatePipelines(IGraphicsDevice& gfx);
    void DispatchMaterialSky(RHI::CommandList cl);

    void CreateSpecularCube(IGraphicsDevice& gfx);
    void CreateAtmosphereCube(IGraphicsDevice& gfx);
    void CreateAtmosphereLUTs(IGraphicsDevice& gfx);
    void BakeStaticLUTs(RHI::CommandList cl);   // Transmittance + MultiScatter, once
    void DispatchSkyViewLUT(RHI::CommandList cl);
    void DispatchAerialPerspective(RHI::CommandList cl);
    void DispatchAtmosphere(RHI::CommandList cl);
    void DispatchPrefilter (RHI::CommandList cl, uint64_t sourceSrv);

    void RecordAsyncCopy(RHI::CommandList cl);

    FrameCB<MaterialSkyCB> m_materialSkyCB;

    RHI::Texture m_materialSkyTex;
    RHI::ResourceState m_materialSkyState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t m_materialSkySrv = 0;
    RHI::PipelineState m_materialSkyPSO;
    std::string m_materialSkyShader;
    ShaderReflect::Reflection m_materialSkyReflection;
    std::array<uint64_t, 8> m_materialSkySRV{};
    bool m_materialSkyEnabled = false;
    bool m_materialSkyValid = false;

    IGraphicsDevice*   m_gfx = nullptr;
    ShaderLibrary      m_shaderLib;
    RHI::PipelineState m_pso;

    FrameCB<SHConstants> m_cb;          // UPLOAD - SHConstants

    RHI::GPUBuffer     m_shBuffer;     // UAV + SRV - 9 float4 coefficients
    uint64_t           m_shUavHandle = 0;
    uint64_t           m_shSrvHandle = 0;
    RHI::ResourceState m_shState     = RHI::ResourceState::UNORDERED_ACCESS;

    uint64_t           m_srcCubemap  = 0;
    bool               m_shValid     = false;

    // ---- Specular prefilter ----
    RHI::PipelineState m_prefilterPSO;
    // UPLOAD: kSpecularMips x 6 x 256 bytes, one slot per (face, mip) pair.
    FrameCB<PrefilterCBPool> m_prefilterCB;

    // One UAV-capable cubemap (128x128x6, 7 mips, R16G16B16A16_FLOAT). The RHI
    // creates the per-mip array UAVs + TextureCube SRV for the TEXTURECUBE misc flag
    // + UAV bind flag + mipLevels > 1.
    RHI::Texture       m_specTex;
    RG::RGTextureHandle m_specH;   // its import
    RHI::ResourceState m_specState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t           m_specSrvHandle = 0;

    // The async reader's copy (AsyncReaderSrv): the same cube without the UAV.
    // -ddgiskysamecube creates none.
    RHI::Texture       m_specCopyTex;
    RHI::ResourceState m_specCopyState = RHI::ResourceState::SHADER_RESOURCE;
    uint64_t           m_specCopySrv   = 0;
    uint8_t            m_specCopyDirty = 0;      // faces the prefilter wrote since the last copy (bit = face)
    bool               m_specCopyValid = false;  // holds a complete cube: a reader may be handed it
    bool               m_asyncReaderActive = false;

    uint64_t                               m_lastBakedSrc  = 0; // source handle that was last prefiltered
    bool                                   m_specValid     = false;
    // Temporal prefilter: after the first full bake (all 6 faces) the dispatch
    // rotates through ONE face per frame, ~1/6 of the all-faces cost. The face is
    // m_frameNumber % 6 (SetFrameNumber), never a count of dispatches.
    uint64_t                               m_frameNumber = 0;
    bool                                   m_prefilterNeedsFullBake = true;

    // Dirty tracking: while the sun neither moves nor changes brightness, every
    // per-frame dispatch (SkyView + atmosphere cube + SH + prefilter) would reproduce
    // the same output, so they are skipped.
    DirectX::XMFLOAT3 m_lastBakedSunDir        { 0.0f, 0.0f, 0.0f };
    float             m_lastBakedSunIntensity  = -1.0f;
    uint32_t          m_framesSinceCubeUpdate  = 0xFFFFFFFFu; // large = needs update

    // ---- Atmosphere ----
    bool                                   m_atmosphereEnabled = false;
    SkyboxSource                           m_skyboxSource      = SkyboxSource::Atmosphere;
    RHI::PipelineState                     m_atmospherePSO;
    FrameCB<AtmosphereConstants>           m_atmosphereCB;       // UPLOAD - AtmosphereConstants

    RHI::Texture       m_atmosphereTex;
    RHI::ResourceState m_atmosphereState = RHI::ResourceState::UNORDERED_ACCESS;
    uint64_t           m_atmosphereSrvHandle = 0;

    // "Active body" - the direction + colour the rest of the engine treats as the
    // directional light (SUN by day, MOON by night). Pushed by Renderer from the
    // SunLightTag entity (or TODOutput when TOD is active).
    DirectX::XMFLOAT3                      m_sunDir   { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3                      m_sunColor { 10.0f, 10.0f, 10.0f };
    // "Geometric sun" - the true sun direction even below the horizon, with colour
    // forced to zero there. Atmosphere / aerial perspective sample it so the sky stays
    // dark at night. Pushed via SetAtmosphereSun.
    DirectX::XMFLOAT3                      m_atmosphereSunDir   { 0, 1, 0 };
    DirectX::XMFLOAT3                      m_atmosphereSunColor { 0, 0, 0 };

    // ---- Hillaire LUTs ----
    RHI::PipelineState                     m_transmittancePSO;
    RHI::PipelineState                     m_multiScatterPSO;
    RHI::PipelineState                     m_skyViewPSO;

    RHI::Texture       m_transmittanceTex;
    RHI::ResourceState m_transmittanceState = RHI::ResourceState::UNORDERED_ACCESS;

    RHI::Texture       m_multiScatterTex;
    RHI::ResourceState m_multiScatterState = RHI::ResourceState::UNORDERED_ACCESS;

    RHI::Texture       m_skyViewTex;
    RHI::ResourceState m_skyViewState = RHI::ResourceState::UNORDERED_ACCESS;

    // Shared CB pool for the per-dispatch atmosphere LUT constants
    // (5 x 512B slots: Transmittance / MultiScatter / SkyView / [unused Atmo] / Aerial).
    FrameCB<AtmoLutCBPool> m_atmoLutCB;

    bool                                   m_staticLutsBaked = false;

    // Global IBL multiplier, default 0.1: full-strength IBL on a default sky
    // overwhelms the direct light; 0.1 gives a subtle ambient fill without flattening
    // the directional shading. A scene raises it through
    // WorldEnvironmentComponent.iblStrength (relayed each frame by SetIBLStrength).
    float m_iblStrength       = 0.1f;

    // ---- Aerial Perspective ----
    RHI::PipelineState m_aerialPSO;
    // The main view's LUT; m_a is the bound view's (BindView).
    Aerial             m_mainAerial;
    Aerial*            m_a = &m_mainAerial;
    // Starts in SHADER_RESOURCE so the lighting pass can sample before the first
    // dispatch (it reads the zeros the texture is created with).


    // Off by default - the AP composite depends on the engine's world-unit-to-km
    // scale and is easy to get wrong. Renderer::SyncSkyboxIBL relays
    // AtmosphereComponent.aerialCompositeEnabled each frame (the post-process config sets
    // it too), for scenes big enough for distance fog to matter.
    bool                                   m_aerialCompositeEnabled = false;


};
