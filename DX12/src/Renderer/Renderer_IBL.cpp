// Renderer_IBL.cpp - sky / IBL: SkyboxComponent + Atmosphere + TOD + clouds -> LightCB +
// passes, plus the sun transmittance, the video extraction and the volumetric fog gather.
// All members of class Renderer (Renderer/Renderer.h).

#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/WaterSurfacePass.h"

// engine graphics / backend
#include "System/Log.h"
#include "System/Check.h"
#include "Settings/SettingsStore.h"   // the scene's fog intent (RecordSceneFogIntent)
#include "Graphics/SunTransmittance.h"
#include "Resource/MaterialSerializer.h"

// render passes
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "RenderGraph/RenderPass/SkyboxPass.h"
#include "RenderGraph/RenderPass/CloudPass.h"
#include "RenderGraph/RenderPass/LightingPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "RenderGraph/RenderPass/ParticlePasses.h"
#include "RenderGraph/RenderPass/FaceOverlayPass.h"
#include "RenderGraph/RenderPass/WaterPass.h"

// ECS components + systems
#include "ECS/SkyboxComponent.h"
#include "ECS/AtmosphereComponent.h"
#include "ECS/WorldEnvironment.h"
#include "ECS/CloudComponent.h"
#include "ECS/HeightFogComponent.h"
#include "ECS/VolumetricFogComponent.h"
#include "ECS/LocalFogVolumeComponent.h"
#include "ECS/HierarchyComponents.h"            // GlobalTransform
#include "RenderGraph/RenderPass/VolumetricFogPass.h"
#include "ECS/TODComponents.h"
#include "ECS/TODSystems.h"

// STL / DirectXMath
#include <DirectXPackedVector.h>   // XMConvertFloatToHalf (the uniform sky cube)
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace DirectX;

// CPU-side cbuffer mirrors live in Renderer.h (RendererDetail namespace) so the
// triple-buffered FrameCB<T> members can be instantiated in the class layout.
// Layouts there MUST stay in sync with the matching HLSL.
using PerViewCB       = RendererDetail::PerViewCB;
using LightCB         = RendererDetail::LightCB;

namespace
{
    // A TOD body's authoring switch: the first Sun/MoonLightTag light's
    // `enabled`. TOD can also drive a sky without light entities, so having
    // none counts as on. LightCB and the clouds honour the same answer.
    template <typename Tag>
    bool TodBodyEnabled(World& world)
    {
        if (auto* pool = world.GetPool<Tag>())
            for (Entity e : pool->Entities())
                if (world.IsAlive(e))
                    if (const auto* light = world.GetComponent<LightData>(e))
                        return light->enabled;
        return true;
    }
}

// FlatColor's uniform sky (see m_uniformSkyCube): a 1x1 RGBA16F cube shaped like the
// shared black fallback, so it binds wherever that does. One mip: iblRadianceMips 1
// samples mip 0 at every roughness, which is exact for a uniform sky.
uint64_t Renderer::EnsureUniformSkyCube(const DirectX::XMFLOAT3& radiance)
{
    if (m_uniformSkyCubeSrv && radiance.x == m_uniformSkyRadiance.x
        && radiance.y == m_uniformSkyRadiance.y && radiance.z == m_uniformSkyRadiance.z)
        return m_uniformSkyCubeSrv;

    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    td.width      = 1;
    td.height     = 1;
    td.arraySize = 6;
    td.mipLevels = 1;
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
    using DirectX::PackedVector::XMConvertFloatToHalf;
    const uint16_t texel[4] = { XMConvertFloatToHalf(std::min(radiance.x, 65000.f)),
                                XMConvertFloatToHalf(std::min(radiance.y, 65000.f)),
                                XMConvertFloatToHalf(std::min(radiance.z, 65000.f)),
                                XMConvertFloatToHalf(1.0f) };
    RHI::SubresourceData faces[6];
    for (auto& f : faces) f = RHI::SubresourceData{ texel, sizeof(texel), sizeof(texel) };
    RHI::Texture cube;
    if (!m_gfx.CreateTexture(td, cube, faces))
    {
        ENGINE_CHECK_ONCE(false, "[SKYLIGHT] FlatColor uniform sky cube creation failed - the ambient falls back to the fill");
        return 0;
    }
    if (m_uniformSkyCube.IsValid()) m_gfx.DestroyTexture(m_uniformSkyCube);
    m_uniformSkyCube     = cube;
    m_uniformSkyCubeSrv  = m_gfx.GetTextureSRVGpuHandle(m_uniformSkyCube);
    m_uniformSkyRadiance = radiance;
    LOG_INFO("[SKYLIGHT] FlatColor: uniform sky light radiance (%.4f, %.4f, %.4f)",
             radiance.x, radiance.y, radiance.z);
    return m_uniformSkyCubeSrv;
}

void Renderer::SyncSkyboxIBL(World& world)
{
    uint64_t irradianceHandle = 0;
    uint64_t radianceHandle   = 0;
    uint64_t skyboxHandle     = 0;
    uint32_t radianceMips     = 5;
    float    iblStrength      = 1.0f;

    // Resolve the environment singleton (READ-ONLY). WorldEnvironmentComponent is the
    // per-scene sky-mode selector. This function never creates sky/atmosphere/ToD entities:
    // Scene::SpawnDefaultEnvironment / MigrateEnvironment seed them once at load. When the
    // component is absent (a raw World that skipped load, e.g. a tool preview) the mode falls
    // back to ProceduralAtmosphere, still without mutating the World mid-render-build.
    // AtmosphereComponent / SkyboxComponent / CloudComponent / HeightFogComponent are
    // read-only parameter carriers; `mode` decides which the backdrop + IBL path consumes. TOD
    // is an opt-in controller (no auto-spawn) driving any SunLightTag / MoonLightTag
    // directional light.
    using EnvMode = WorldEnvironmentComponent::Mode;
    EnvMode           envMode  = EnvMode::ProceduralAtmosphere;
    DirectX::XMFLOAT3 envFlatColor { 0.f, 0.f, 0.f };
    DirectX::XMFLOAT3 envAmbient   { 0.f, 0.f, 0.f };
    float             envAmbientIntensity = 1.0f;
    // The sky light's intensity: negative = not authored -> inherit
    // AtmosphereComponent.iblStrength below.
    float             envIblStrength        = -1.0f;
    if (auto* envPool = world.GetPool<WorldEnvironmentComponent>();
        envPool && !envPool->Data().empty())
    {
        const WorldEnvironmentComponent& e = envPool->Data()[0];
        envMode             = e.mode;
        envFlatColor        = e.flatColor;
        envAmbient          = e.ambientColor;
        envAmbientIntensity = e.ambientIntensity;
        envIblStrength        = e.iblStrength;
    }
    const bool atmosphereMode = (envMode == EnvMode::ProceduralAtmosphere);
    const bool skyMode        = (envMode == EnvMode::ProceduralAtmosphere ||
                                 envMode == EnvMode::StaticSkybox);

    AtmosphereComponent* atmosComp = nullptr;
    if (auto* p = world.GetPool<AtmosphereComponent>(); p && !p->Data().empty())
        atmosComp = &p->Data()[0];

    // TOD pipeline, only when a Time-of-Day entity exists (opt-in, no auto-spawn: a scene
    // without one keeps its authored directional lights). The systems are no-ops when
    // TODConfig.enabled is false. Order: tick -> evaluate -> sync (sun, moon, atmosphere). The
    // sync systems write the LightData of the tagged sun/moon entities, which the light
    // gather reads as normal directional lights; this frame's gather already ran, so the
    // active body is also written into LightCB further down (the todActive branch).
    if (auto* todPool = world.GetPool<TODConfigComponent>();
        todPool && !todPool->Entities().empty())
    {
        TODTickSystem::Update          (world, FrameViewWrite().worldDeltaTime);
        TODEvaluationSystem::Update    (world);
        TODSunSyncSystem::Update       (world);
        TODMoonSyncSystem::Update      (world);
        TODAtmosphereSyncSystem::Update(world);
        // Local lights (street lamps, neon, windows) + their bulb emissive, last in the block so
        // it reads a fully evaluated TODOutput. Run here rather than as a scheduled system because
        // this block has no EngineMode gate: the lamps respond in EDIT mode while the author drags
        // the time slider (a Lua script cannot: ScriptLogicSystem is SystemRunMask::Play).
        // SyncSkyboxIBL runs after BuildScene_SyncMaterials and the light gather, so both writes
        // land on the next frame - invisible against a 1.5 s fade, and the same phase lag TOD has
        // for every other consumer.
        TODLightSyncSystem::Update     (world, FrameViewWrite().worldDeltaTime);
    }

    // Push the environment mode -> SkyIBLPass. The procedural atmosphere runs ONLY in
    // ProceduralAtmosphere mode; StaticSkybox / FlatColor / None turn it off (StaticSkybox then
    // drives IBL from the loaded cubemaps, FlatColor from its uniform sky cube; None has no sky
    // - see iblRadianceMips below). AtmosphereComponent supplies the numeric knobs when
    // present, else defaults.
    if (m_skyIBLPass)
    {
        // The temporal prefilter's face = frame % 6 (SkyIBLPass::SetFrameNumber).
        m_skyIBLPass->SetFrameNumber(m_materialFrameEpoch);
        m_skyIBLPass->SetAtmosphereEnabled(atmosphereMode);
        m_skyIBLPass->SetSkyboxSource(atmosphereMode
            ? SkyIBLPass::SkyboxSource::Atmosphere
            : SkyIBLPass::SkyboxSource::Static);
        // Intensity resolve: WorldEnvironment.iblStrength wins when authored (>= 0); otherwise
        // inherit AtmosphereComponent.iblStrength (0.1 without one), so scenes that author it
        // only on the atmosphere keep their look.
        m_skyIBLPass->SetIBLStrength(
            envIblStrength >= 0.0f ? envIblStrength
                                   : (atmosComp ? atmosComp->iblStrength : 0.1f));
        m_skyIBLPass->SetAerialCompositeEnabled(
            atmosComp ? atmosComp->aerialCompositeEnabled : false);
    }

    // Singleton pool probe, like WorldEnvironment/Atmosphere/TODConfig above. Do not walk the
    // World's entities calling GetComponent: each call is a GetPool (type_index hash + map
    // find), and a scene without a SkyboxComponent pool would scan every entity every frame.
    // Data()[0] is the first DENSE entry, not the first entity in m_entities order; with the
    // one-skybox-per-scene invariant they are the same, and with more than one neither order
    // is stable (DestroyEntity swap-removes).
    bool hasMaterialSky = false;
    if (auto* skyPool = world.GetPool<SkyboxComponent>();
        skyPool && !skyPool->Data().empty())
    {
        SkyboxComponent* sc = &skyPool->Data()[0];
        if (skyMode && !sc->materialPath.empty() && m_skyIBLPass && m_texSys && m_resMgr)
        {
            if (m_skyMaterialPath != sc->materialPath || (++m_skyMaterialPoll % 60) == 0)
            {
                if (m_skyMaterialPath != sc->materialPath)
                {
                    for (auto& [path, entry] : m_skyMaterialTextures)
                        if (entry.handle != Resource::kInvalidTextureHandle)
                            m_texSys->Release(entry.handle, m_gfx);
                    m_skyMaterialTextures.clear();
                }
                m_skyMaterialPath = sc->materialPath;
                m_skyMaterialConfig = {};
                Resource::LoadMaterial(m_skyMaterialPath, m_skyMaterialConfig);
                auto layer = [&](const std::string& path, const char* prefix) {
                    if (path.empty()) return;
                    MaterialComponent source;
                    if (Resource::LoadMaterial(path, source))
                        for (const auto& [name, value] : source.customParams)
                            m_skyMaterialConfig.customParams[std::string(prefix) + name] = value;
                };
                layer(sc->baseMaterialPath, "Sky_");
                layer(sc->cloudMaterialPath, "Cloud_");
            }
            m_skyIBLPass->PrepareMaterialSky(&m_skyMaterialConfig, FrameViewWrite().globalTimeSec,
                [&](const std::string& path) -> uint64_t {
                    auto& entry = m_skyMaterialTextures[path];
                    if (entry.handle == Resource::kInvalidTextureHandle)
                        entry.handle = m_texSys->Acquire(path, *m_resMgr, m_gfx);
                    if (entry.handle != Resource::kInvalidTextureHandle && m_texSys->IsReady(entry.handle))
                        if (const auto* tex = m_texSys->GetTexture(entry.handle))
                            return m_gfx.GetTextureSRVGpuHandle(*tex);
                    return 0;
                });
            hasMaterialSky = true;
        }

        if (m_texSys && m_resMgr)
        {
            auto syncTex = [&](SkyboxTexEntry& entry, const std::string& path, uint64_t& outHandle)
            {
                if (path.empty()) return;
                if (entry.path != path)
                {
                    if (entry.handle != Resource::kInvalidTextureHandle)
                        m_texSys->Release(entry.handle, m_gfx);
                    entry.path   = path;
                    entry.handle = m_texSys->Acquire(path, *m_resMgr, m_gfx);
                    outHandle    = 0;
                }
                if (entry.handle != Resource::kInvalidTextureHandle && m_texSys->IsReady(entry.handle))
                    if (const RHI::Texture* tex = m_texSys->GetTexture(entry.handle))
                        outHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
            };

            const uint64_t prevIrr = m_skyboxTexCache[0].lastLoggedHandle;
            const uint64_t prevRad = m_skyboxTexCache[1].lastLoggedHandle;
            syncTex(m_skyboxTexCache[0], sc->irradiancePath, irradianceHandle);
            syncTex(m_skyboxTexCache[1], sc->radiancePath,   radianceHandle);
            syncTex(m_skyboxTexCache[2], sc->skyboxPath,     skyboxHandle);

            if (irradianceHandle && irradianceHandle != prevIrr)
            {
                m_skyboxTexCache[0].lastLoggedHandle = irradianceHandle;
                LOG_INFO("SyncSkyboxIBL: irradiance handle ready (gpu=0x%llX)", irradianceHandle);
            }
            if (radianceHandle && radianceHandle != prevRad)
            {
                m_skyboxTexCache[1].lastLoggedHandle = radianceHandle;
                LOG_INFO("SyncSkyboxIBL: radiance handle ready (gpu=0x%llX)", radianceHandle);
            }

            sc->irradianceGpuHandle = irradianceHandle;
            sc->radianceGpuHandle   = radianceHandle;
            sc->skyboxGpuHandle     = skyboxHandle;
        }

        radianceMips = sc->radianceMipLevels;
        // SkyboxComponent.iblStrength is ignored: the master gate is
        // WorldEnvironmentComponent.iblStrength (Environment panel), inheriting
        // AtmosphereComponent.iblStrength when unauthored.
    }

    if (!hasMaterialSky && m_skyIBLPass) m_skyIBLPass->PrepareMaterialSky(nullptr, 0, {});
    // Global IBL strength lives on SkyIBLPass.
    if (m_skyIBLPass)
        iblStrength = m_skyIBLPass->GetIBLStrength();

    // BRDF LUT: fixed asset, loaded once.
    static constexpr const char* kBRDFLUTPath = "engine/ibl/BRDF_LUT.itex";
    uint64_t brdfLutHandle = 0;
    if (m_texSys && m_resMgr)
    {
        if (m_brdfLutEntry.path.empty())
        {
            m_brdfLutEntry.path   = kBRDFLUTPath;
            m_brdfLutEntry.handle = m_texSys->Acquire(kBRDFLUTPath, *m_resMgr, m_gfx);
        }
        if (m_brdfLutEntry.handle != Resource::kInvalidTextureHandle
            && m_texSys->IsReady(m_brdfLutEntry.handle))
        {
            if (const RHI::Texture* tex = m_texSys->GetTexture(m_brdfLutEntry.handle))
                brdfLutHandle = m_gfx.GetTextureSRVGpuHandle(*tex);
        }
    }

    // SkyIBL: atmosphere drives SH/specular/backdrop by default. Disabled fallback priority: skybox > radiance > irradiance.
    uint64_t shSourceHandle = skyboxHandle ? skyboxHandle
                            : radianceHandle ? radianceHandle
                            : irradianceHandle;
    if (m_skyIBLPass)
    {
        m_skyIBLPass->SetSourceCubemap(shSourceHandle);

        // Do not push the AP camera (SetCameraForAerial) here: UploadFrameData writes this frame's
        // LightCB slot later, so a read here gets a kFrameCount-stale slot (zeros on the first
        // frames). BeginFrame pushes it after UploadFrameData.

        // TOD: read the computed sun/moon state from TODOutputComponent (written by
        // TODEvaluationSystem earlier this frame). With TOD off, the light gather already put the
        // directional light into LightCB; it is read back below for the atmosphere shaders.
        const auto* todCfg = TODUtil::FindConfig(world);
        const auto* todOut = TODUtil::FindOutput(world);
        const bool  todActive = todCfg && todOut && todCfg->enabled;

        // Extract one bool for the post-process side: the lens-flare horizon gate reads
        // m_todMoonActive instead of calling TODUtil::FindOutput from the post-process render
        // worker (a live-World read off the main thread). Gated on todActive: a disabled TOD
        // leaves its last output in place (TODEvaluationSystem stops writing it), and a stale
        // night would hide the authored sun's flare.
        m_todMoonActive = todActive && todOut->isMoonActive;

        if (todActive)
        {
            // TOD overrides the gathered sun below. Honour the tagged light's
            // authoring switch here too, otherwise a disabled sun reappears.
            const bool sunEnabled  = TodBodyEnabled<SunLightTag>(world);
            const bool moonEnabled = TodBodyEnabled<MoonLightTag>(world);
            const XMFLOAT3 activeColor = (todOut->isMoonActive ? moonEnabled : sunEnabled)
                ? todOut->activeColor : XMFLOAT3{0, 0, 0};
            const XMFLOAT3 sunColor = sunEnabled ? todOut->sunColor : XMFLOAT3{0, 0, 0};
            // Active body (sun-by-day / moon-by-night) drives LightCB so
            // CSM + lighting + fog stay in sync with the sky. The sun/moon
            // LightData entities also got their colors set by the sync
            // systems, but the gather loop ordering is non-deterministic
            // between two directional lights; this explicit write makes the
            // active body authoritative regardless of iteration order.
            if (m_lightCB.Upload(m_gfx))
            {
                auto* lb = m_lightCB.Upload(m_gfx);
                lb->lightDir[0] = -todOut->activeDirection.x;
                lb->lightDir[1] = -todOut->activeDirection.y;
                lb->lightDir[2] = -todOut->activeDirection.z;
                // Through SetLightCBSun, so the TOD body picks up atmosphere transmittance exactly like an
                // authored directional: a setting sun reddens on its own instead of needing the TOD colour
                // curve to fake it.
                SetLightCBSun(world, activeColor);
            }
            // Push to SkyIBLPass: the active body for lighting consumers, the geometric sun for
            // atmosphere scattering. Both take the authored top-of-atmosphere colour; the sky
            // integrates its own extinction.
            m_skyIBLPass->SetSunDir(todOut->activeDirection, activeColor);
            m_skyIBLPass->SetAtmosphereSun(todOut->sunDirection, sunColor);
        }
        else if (m_lightCB.Upload(m_gfx))
        {
            // TOD off -> the gathered directional light (in LightCB) drives the sun. Read it back and
            // push it to SkyIBLPass for the atmosphere shaders; directional intensity 0 propagates as
            // zero sun (no sun = no atmosphere scatter, the scene darkens).
            // Colour comes from m_sunColorTOA, NOT lb->lightColor: the gather already folded
            // transmittance into the latter, and feeding a pre-extinguished sun to the sky-view LUT
            // would apply the extinction twice.
            auto* lb = m_lightCB.Upload(m_gfx);
            DirectX::XMFLOAT3 sunDir{ -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
            m_skyIBLPass->SetSunDir(sunDir, m_sunColorTOA);
            m_skyIBLPass->SetAtmosphereSun(sunDir, m_sunColorTOA);
        }
    }

    // Volumetric clouds: push CloudComponent + TOD sun state.
    if (m_cloudPass)
    {
        // First CloudComponent in the world (typically on the Sky entity; clouds are opt-in,
        // never auto-spawned).
        CloudComponent* cloudComp = nullptr;
        if (auto* p = world.GetPool<CloudComponent>(); p && !p->Data().empty())
            cloudComp = &p->Data()[0];

        if (cloudComp)
        {
            // The light the deck sees, resolved with the SAME rules as the scene's own directional
            // light (LightCB): the TOD body gated by its light's `enabled` (sun by day, moon by
            // night), or the authored directional light - and that top-of-atmosphere colour filtered
            // by the air above the layer (ResolveSunTransmittance at the layer's mid altitude, (0,0,0)
            // once the planet hides the sun from there). The march integrates only the cloud's own
            // density, never the atmosphere: without this filter a sun below the horizon would light
            // the deck at full strength from under the planet while scene and sky are dark, and auto
            // exposure metering the dark scene would blow the clouds out at night.
            // skyRGB drives the skylight term (CloudCB.ambientLightLum): the light that lights the SKY
            // - the geometric sun, which TOD keeps at zero below the horizon - not the moon, so the
            // deck's ambient follows the sky dome every surface takes its sky ambient from.
            DirectX::XMFLOAT3 sunDirToSun { 0.f, 1.f, 0.f };
            DirectX::XMFLOAT3 sunTOA      { 1.f, 1.f, 1.f };
            DirectX::XMFLOAT3 skyDirToSun { 0.f, 1.f, 0.f };
            DirectX::XMFLOAT3 skyTOA      { 1.f, 1.f, 1.f };
            const auto* todCfg = TODUtil::FindConfig(world);
            const auto* todOut = TODUtil::FindOutput(world);
            if (todCfg && todOut && todCfg->enabled)
            {
                const bool sunOn  = TodBodyEnabled<SunLightTag>(world);
                const bool bodyOn = todOut->isMoonActive ? TodBodyEnabled<MoonLightTag>(world) : sunOn;
                sunDirToSun = todOut->activeDirection;   // already towards-body
                sunTOA      = bodyOn ? todOut->activeColor : XMFLOAT3{ 0.f, 0.f, 0.f };
                skyDirToSun = todOut->sunDirection;
                skyTOA      = sunOn ? todOut->sunColor : XMFLOAT3{ 0.f, 0.f, 0.f };
            }
            else if (m_lightCB.Upload(m_gfx))
            {
                auto* lb = m_lightCB.Upload(m_gfx);
                sunDirToSun = { -lb->lightDir[0], -lb->lightDir[1], -lb->lightDir[2] };
                sunTOA      = m_sunColorTOA;
                skyDirToSun = sunDirToSun;
                skyTOA      = sunTOA;
            }
            const float layerKm = 0.0005f * (cloudComp->bottomAltitude + cloudComp->topAltitude);
            auto atLayer = [&](const XMFLOAT3& dirToSun, const XMFLOAT3& toa)
            {
                const float travels[3] = { -dirToSun.x, -dirToSun.y, -dirToSun.z };
                const XMFLOAT3 T = ResolveSunTransmittance(world, travels, layerKm);
                return XMFLOAT3{ toa.x * T.x, toa.y * T.y, toa.z * T.z };
            };
            const XMFLOAT3 sunRGB = atLayer(sunDirToSun, sunTOA);
            const XMFLOAT3 skyRGB = atLayer(skyDirToSun, skyTOA);

            // Clouds only composite over an actual sky (procedural/static);
            // in None/FlatColor there is no backdrop to layer them onto.
            m_cloudPass->SetEnabled(cloudComp->enabled && skyMode);
            m_cloudPass->SetParams (*cloudComp, FrameViewWrite().worldDeltaTime);
            m_cloudPass->SetSun    (sunDirToSun, sunRGB, skyRGB);

            // The cloud camera is pushed in BeginFrame after UploadFrameData, like the aerial LUT's:
            // a LightCB read here is kFrameCount frames stale.
        }
        else
        {
            m_cloudPass->SetEnabled(false);
        }
    }

    // Volumetric fog: global component + local volumes.
    GatherVolumetricFog(world);

    const bool atmosphereOn = m_skyIBLPass &&
        (m_skyIBLPass->IsAtmosphereEnabled() || m_skyIBLPass->IsMaterialSkyEnabled());
    // Static mode skips every compute pass: gate on atmosphereOn so stale LUTs do not leak.
    const bool skyUseSH     = atmosphereOn
                           && m_skyIBLPass && m_skyIBLPass->IsSHValid();

    // iblStrength is the sky light's master scale (Lighting.ps.hlsl "Knob
    // ownership"): sky diffuse, sky specular, and the sky every traced consumer
    // sees on a miss - DDGI probes, reflections, the path tracer. Not
    // reflection probes (gated at bake time) and not DDGI's own bounce. 0 is a
    // valid user choice ("no sky light"); don't override it because the
    // atmosphere is on.

    // Effective mip count may come from SkyIBLPass's pre-filtered cube vs static radiance.
    uint32_t cbRadianceMips = radianceMips;
    if (atmosphereOn && m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
        cbRadianceMips = m_skyIBLPass->GetSpecularMipCount();

    // FlatColor = a uniform sky light: its ambient colour x intensity is the radiance of a
    // uniform sky, and that sky goes where every other sky goes - the irradiance AND radiance
    // cube of every pass, DDGI's / RT reflections' / the path tracer's miss - at iblStrength 1
    // (the intensity is in the colour). So the sky-light chain weights it (kDibl, the
    // split-sum, the hemisphere partition) and GI occludes and bounces it, instead of a
    // constant added on top of everything. None keeps ambientColor as a separate non-physical
    // fill (Lighting.ps weights it like a uniform environment but on top of the partition,
    // unseen by GI).
    const DirectX::XMFLOAT3 flatSkyL{ envAmbient.x * envAmbientIntensity,
                                      envAmbient.y * envAmbientIntensity,
                                      envAmbient.z * envAmbientIntensity };
    const bool uniformSky = (envMode == EnvMode::FlatColor)
                         && (flatSkyL.x > 0.f || flatSkyL.y > 0.f || flatSkyL.z > 0.f);
    const uint64_t uniformSkySrv = uniformSky ? EnsureUniformSkyCube(flatSkyL) : 0;
    const bool     fillAmbient   = !skyMode && !uniformSkySrv;   // None (or a failed cube): the fill

    if (m_lightCB.Upload(m_gfx))
    {
        auto* lb = m_lightCB.Upload(m_gfx);
        // None: force the shader IBL block OFF via the radianceMips gate so no
        // stale sky SH/specular is sampled (and no unbound-cube read). FlatColor:
        // one mip of the uniform cube.
        lb->iblRadianceMips = skyMode ? cbRadianceMips : (uniformSkySrv ? 1u : 0u);
        lb->iblStrength     = uniformSkySrv ? 1.0f : iblStrength;
        lb->iblUseSH        = (skyMode && skyUseSH) ? 1u : 0u;
        // Flat constant ambient: the None mode's non-physical fill, in repurposed spare LightCB
        // pads (see light_cb.hlsli). Zero in the sky modes (the sky IBL carries the ambient) and in
        // FlatColor (the uniform sky does).
        lb->ambientColor[0]  = fillAmbient ? envAmbient.x : 0.f;
        lb->ambientColor[1]  = fillAmbient ? envAmbient.y : 0.f;
        lb->ambientColor[2]  = fillAmbient ? envAmbient.z : 0.f;
        lb->ambientIntensity = fillAmbient ? envAmbientIntensity : 0.f;
        // AP composite gated on atmosphere + valid + opt-in; the first frame's ap.a = 0 would
        // multiply the scene -> black.
        const bool apReady = m_skyIBLPass
                          && m_skyIBLPass->IsAtmosphereEnabled()
                          && m_skyIBLPass->IsAerialValid()
                          && m_skyIBLPass->IsAerialCompositeEnabled();
        lb->aerialMaxDistKm = apReady ? m_skyIBLPass->GetAerialMaxDistanceKm() : 0.0f;
    }

    // HDR scene-color clear: the flat backdrop in None/FlatColor (the sky pass draws nothing
    // there), black in the sky modes (the sky pass paints the background). Consumed by
    // GraphicsDX12::SetRenderTargetToHdr. Alpha stays 0, as in the default HDR clear
    // ({0,0,0,0}).
    FrameViewWrite().hdrClearColor[0] = skyMode ? 0.f : envFlatColor.x;
    FrameViewWrite().hdrClearColor[1] = skyMode ? 0.f : envFlatColor.y;
    FrameViewWrite().hdrClearColor[2] = skyMode ? 0.f : envFlatColor.z;
    FrameViewWrite().hdrClearColor[3] = 0.f;

    // Atmosphere -> prefer the live SkyIBLPass specular; static -> the loaded .itex (the
    // SkyIBLPass cube would be stale).
    uint64_t effectiveRadiance = radianceHandle;
    uint32_t effectiveRadianceMips = radianceMips;
    if (atmosphereOn && m_skyIBLPass && m_skyIBLPass->IsSpecularValid())
    {
        const uint64_t spec = m_skyIBLPass->GetSpecularSrvHandle();
        if (spec)
        {
            effectiveRadiance     = spec;
            effectiveRadianceMips = m_skyIBLPass->GetSpecularMipCount();
        }
    }

    // SSR composite cache (same handle as LightingPass split-sum BRDF LUT).
    m_brdfLutSrv             = brdfLutHandle;
    // DDGI misses sample the RADIANCE cube (sun disk preserved); irradiance would smear the
    // sun -> L1 SH ~ 0. None has no sky -> hand DDGI nothing (0 -> the pass binds its fallback
    // cube); FlatColor hands its uniform sky (RT reflections and the path tracer read this
    // handle too).
    m_skyRadianceSrvForDDGI  = skyMode ? effectiveRadiance : uniformSkySrv;

    // Forward handles to passes. Substitute the shared black cube when a cube handle is 0 so
    // the deferred/forward shaders never sample an UNBOUND t6/t7 descriptor (the first frames
    // of a procedural scene, before SH is valid); reads resolve to defined black.
    // None/FlatColor hand the black cube too, even when a SkyboxComponent's cubes are loaded:
    // Lighting.ps's indirect block also runs for DDGI / probes / SSGI / the traced tier, and
    // inside it the sky terms sample t6 / t7 with no mode gate (iblRadianceMips = 0 only picks
    // mip 0), so a FlatColor scene with a leftover Skybox entity would take that cube's
    // diffuse and a mirror-sharp reflection of it at every roughness; forward weights the
    // same samples by envW. FlatColor's uniform sky is both cubes at once.
    const uint64_t noSkyCube = uniformSkySrv ? uniformSkySrv : m_fallbackSkyCubeSrv;
    const uint64_t iblIrr = (skyMode && irradianceHandle)  ? irradianceHandle  : noSkyCube;
    const uint64_t iblRad = (skyMode && effectiveRadiance) ? effectiveRadiance : noSkyCube;
    // RTR analytic hits mirror Lighting.ps's exact skyDiff branch (iblUseSH ? SH : irradiance
    // cube): hand them the SAME cube. Real or black fallback, so the trace can sample
    // unconditionally like Lighting.
    m_skyIrradianceSrvForRTR = iblIrr;
    if (m_lightingPass)
    {
        m_lightingPass->SetIBL(iblIrr, iblRad, effectiveRadianceMips, iblStrength);
        m_lightingPass->SetBRDFLUT(brdfLutHandle);
        m_lightingPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0,
                                 skyUseSH);
        // AP: 3D LUT + max distance. Handle 0 -> the 1x1x1 fallback binds (inscatter=0, T=1,
        // no-op).
        if (m_skyIBLPass)
            m_lightingPass->SetAerialPerspective(
                m_skyIBLPass->GetAerialPerspectiveSrvHandle(),
                m_skyIBLPass->GetAerialMaxDistanceKm());
    }
    if (m_transparentPass)
    {
        // Mirror LightingPass: procedural specular cube when atmosphere on (forward agrees w/ deferred).
        // Diffuse stays on irradiance cube; iblUseSH=1 routes forward through EvalSH2.
        m_transparentPass->SetIBL(iblIrr, iblRad,
                                  effectiveRadianceMips, iblStrength);
        m_transparentPass->SetBRDFLUT(brdfLutHandle);
        m_transparentPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    if (m_waterSurfacePass)
    {
        // Water reflects through gRadiance + the reflection-probe path like any
        // other forward material (so an indoor pool reflects its probe, not the
        // outdoor sky) rather than through a dedicated sharp sky cube.
        m_waterSurfacePass->SetIBLHandles(iblIrr, iblRad, brdfLutHandle);
        m_waterSurfacePass->SetSkySHHandle(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    if (m_particleRenderPass)
    {
        // LIT particle buckets (diffuse: sun + ambient irradiance/SH). Same IBL
        // sources as the forward mesh path.
        m_particleRenderPass->SetIBL(iblIrr);
        m_particleRenderPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    if (m_faceOverlayPass)
    {
        // The brow/lash overlay's ambient is the face's (DDGI + sky, isotropic): the same sky
        // sources as the meshes.
        m_faceOverlayPass->SetIBL(iblIrr);
        m_faceOverlayPass->SetSkySH(m_skyIBLPass ? m_skyIBLPass->GetSHSrvHandle() : 0);
    }
    // Snapshot for the preview's "use scene lighting" toggle, captured next to the handles
    // the passes are given so the preview and the main viewport cannot disagree about the
    // scene's sky; read by SyncPreviewEnvironment later in the same game half.
    // The sky-mode gate is part of the snapshot: None/FlatColor have no scene sky cube, and
    // publishing the black fallback here would hand the preview a defined black instead of
    // "no scene sky" - a dead-black metal sphere instead of the studio rig fallback.
    m_sceneIblIrradiance   = skyMode ? irradianceHandle : 0;
    m_sceneIblRadiance     = skyMode ? effectiveRadiance : 0;
    m_sceneIblRadianceMips = skyMode ? effectiveRadianceMips : 0;
    m_sceneSkySH           = (skyMode && m_skyIBLPass) ? m_skyIBLPass->GetSHSrvHandle() : 0;
    // Cleared here and filled in below, inside `if (m_skyboxPass)`: without the
    // clear a session that later loses its SkyboxPass would keep handing the
    // preview a handle nothing refreshes.
    m_sceneSkyboxCube      = 0;
    m_sceneSkyUseSH        = skyMode && skyUseSH;
    m_sceneIblStrength     = iblStrength;

    if (m_skyboxPass)
    {
        const uint64_t staticFallback = skyboxHandle ? skyboxHandle
                                      : radianceHandle ? radianceHandle
                                      : irradianceHandle;
        // None/FlatColor draw NO backdrop cube regardless of a stray
        // SkyboxComponent; the flat background comes from the HDR clear instead.
        const uint64_t resolved = !skyMode ? 0
            : (m_skyIBLPass ? m_skyIBLPass->ResolveSkyboxSrvHandle(staticFallback)
                            : staticFallback);
        m_skyboxPass->SetEnvMap(resolved);
        // The same cube SkyboxPass#2 draws behind a preview whose backdrop is Background::Skybox
        // with Feature::kSceneLighting on. Taken from the resolve, so the preview's backdrop is
        // the viewport's by construction.
        m_sceneSkyboxCube = resolved;

        // Water reflects exactly the sky cube the SkyboxPass draws. In None/
        // FlatColor (resolved==0), or before the sky cube is ready, fall back to
        // the shared black cube so water renders a dark reflection instead of
        // vanishing entirely (WaterPass early-outs on a null sky cube).
        if (m_waterPass)
            m_waterPass->SetSkyCube(resolved ? resolved : m_fallbackSkyCubeSrv);

        // Analytic sun disk in PS for pixel-sharp result; uses whichever sun drives the atmosphere.
        if (m_skyIBLPass)
        {
            const DirectX::XMFLOAT3& sd = m_skyIBLPass->GetSunDir();
            const DirectX::XMFLOAT3& sc = m_skyIBLPass->GetSunColor();
            // Real solar half-angle ~ 0.27 deg (0.00465 rad); 0.005 is a touch wider. The analytic
            // sun disk is drawn only in procedural atmosphere: a static skybox bakes its own sun, and
            // None/FlatColor draw no backdrop at all.
            m_skyboxPass->SetSun(sd, sc, /*half-angle rad*/ 0.005f,
                                 /*intensity*/ atmosphereMode ? 15.0f : 0.0f);

            if (m_moonHandle == Resource::kInvalidTextureHandle && m_texSys && m_resMgr)
                m_moonHandle = m_texSys->Acquire(
                    "engine/textures/moon.itex", *m_resMgr, m_gfx);
            if (m_moonSRV == 0 && m_texSys
                && m_moonHandle != Resource::kInvalidTextureHandle
                && m_texSys->IsReady(m_moonHandle))
            {
                if (const RHI::Texture* tex = m_texSys->GetTexture(m_moonHandle))
                    m_moonSRV = m_gfx.GetTextureSRVGpuHandle(*tex);
            }

            // Moon disk + stars come from TODOutputComponent (singleton). With TOD off the moon is
            // hidden and night alpha is zero (an author can drape a static night sky via
            // SkyboxComponent). "Off" includes a TODConfig with enabled = false: its output is NOT
            // cleared (TODEvaluationSystem returns before writing), so reading it would keep the last
            // evaluated night sky on screen.
            const auto* todCfg = TODUtil::FindConfig(world);
            const auto* todOut = (todCfg && todCfg->enabled) ? TODUtil::FindOutput(world)
                                                             : nullptr;
            const bool  moonVisible = todOut && todOut->moonDiskVisible;
            const DirectX::XMFLOAT3 moonDir   = todOut ? todOut->moonDirection
                                                       : DirectX::XMFLOAT3{ 0, -1, 0 };
            const DirectX::XMFLOAT3 moonColor = todOut ? todOut->moonColor
                                                       : DirectX::XMFLOAT3{ 0, 0, 0 };
            m_skyboxPass->SetMoon(m_moonSRV, moonVisible, moonDir, moonColor,
                                  /*half-angle rad*/ 0.026f);

            // Stars drive off MOON altitude (above horizon = night).
            const float moonY = moonDir.y;
            auto smoothstepF = [](float e0, float e1, float x) {
                float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
                return t * t * (3.0f - 2.0f * t);
            };
            float nightAlpha = smoothstepF(-0.20f, 0.10f, moonY);

            // Star twinkle: wraps every ~10 min for sin-phase float precision.
            static float s_starTime = 0.0f;
            s_starTime = std::fmod(s_starTime + FrameViewWrite().worldDeltaTime, 600.0f);

            const float starDensity    = atmosComp ? atmosComp->starDensity    : 256.0f;
            const float starBrightness = atmosComp ? atmosComp->starBrightness : 0.7f;
            m_skyboxPass->SetStars(nightAlpha, s_starTime, starDensity, starBrightness);
        }
    }
}

// Identity whenever the feature does not apply (not ProceduralAtmosphere: a static skybox
// bakes its sun, None/FlatColor have no air; or switched off on the atmosphere), so callers
// multiply unconditionally.
DirectX::XMFLOAT3 Renderer::ResolveSunTransmittance(
    World& world, const float lightDirTravels[3], float altitudeKm) const
{
    const XMFLOAT3 kIdentity{ 1.0f, 1.0f, 1.0f };

    // Same env-mode resolve as SyncSkyboxIBL: an absent singleton means a raw
    // World that skipped load, which falls back to ProceduralAtmosphere.
    using EnvMode = WorldEnvironmentComponent::Mode;
    EnvMode envMode = EnvMode::ProceduralAtmosphere;
    if (auto* envPool = world.GetPool<WorldEnvironmentComponent>();
        envPool && !envPool->Data().empty())
        envMode = envPool->Data()[0].mode;
    if (envMode != EnvMode::ProceduralAtmosphere) return kIdentity;

    if (!m_skyIBLPass || !m_skyIBLPass->IsAtmosphereEnabled()) return kIdentity;

    const AtmosphereComponent* atmos = nullptr;
    if (auto* p = world.GetPool<AtmosphereComponent>(); p && !p->Data().empty())
        atmos = &p->Data()[0];

    const bool  on       = atmos ? atmos->sunTransmittanceEnabled  : true;
    const float strength = atmos ? atmos->sunTransmittanceStrength : 1.0f;
    if (!on || strength <= 0.0f) return kIdentity;

    // lightDir is the direction light TRAVELS (sun -> ground); the transmittance integral is
    // parameterised on the direction TOWARDS the sun.
    const XMFLOAT3 dirToSun{ -lightDirTravels[0], -lightDirTravels[1], -lightDirTravels[2] };
    const XMFLOAT3 T = Atmo::SunTransmittance(dirToSun, altitudeKm);
    const float    s = std::clamp(strength, 0.0f, 1.0f);
    return { 1.0f + s * (T.x - 1.0f),
             1.0f + s * (T.y - 1.0f),
             1.0f + s * (T.z - 1.0f) };
}

void Renderer::SetLightCBSun(World& world, const DirectX::XMFLOAT3& colorTOA)
{
    m_sunColorTOA = colorTOA;

    auto* lb = m_lightCB.Upload(m_gfx);
    if (!lb) return;

    // lightDir must already hold this sun's direction: every caller writes it first, and the
    // transmittance depends on it.
    const XMFLOAT3 T = ResolveSunTransmittance(world, lb->lightDir);
    lb->lightColor[0] = colorTOA.x * T.x;
    lb->lightColor[1] = colorTOA.y * T.y;
    lb->lightColor[2] = colorTOA.z * T.z;
}




// Game half. Render_BindFrameResources pushes the published VolFogSceneWrite() copy into
// VolumetricFogPass; ServiceEnableGatedResources reads m_volFogDemandBits (the froxel
// resource gate).
void Renderer::GatherVolumetricFog(World& world)
{
    using namespace DirectX;
    VolFogSceneState& out = VolFogSceneWrite();
    out.fog           = VolumetricFogPass::SceneFog{};
    out.locals.clear();
    out.emissiveCount = 0;
    out.mediumLocals.clear();

    // Global medium (singleton by convention, first pool entry wins).
    if (auto* p = world.GetPool<VolumetricFogComponent>(); p && !p->Data().empty())
    {
        const VolumetricFogComponent& c = p->Data()[0];
        VolumetricFogPass::SceneFog& f = out.fog;
        auto sat = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
        f.owned            = true;
        f.enabled          = c.enabled;
        f.godRays          = c.godRays;
        f.temporal         = c.temporal;
        f.density          = (std::max)(c.density, 0.0f);
        f.anisotropy       = std::clamp(c.anisotropy, -0.95f, 0.95f);
        f.baseHeight       = c.baseHeight;
        f.heightFalloff    = (std::max)(c.heightFalloff, 0.0f);
        f.ambientIntensity = (std::max)(c.ambientIntensity, 0.0f);
        // The froxel slices are log-distributed between near and far, so both
        // must be positive and far must clear near (log(far/near) divides).
        f.startDistance    = std::clamp(c.startDistance, 0.05f, 50.0f);
        f.viewDistance     = (std::max)(c.viewDistance, f.startDistance + 1.0f);
        f.temporalBlend    = std::clamp(c.temporalBlend, 0.01f, 1.0f);
        f.albedo           = { sat(c.albedo.x), sat(c.albedo.y), sat(c.albedo.z) };
        f.ambientColor     = c.ambientColor;
        const float ei     = (std::max)(c.emissiveIntensity, 0.0f);
        f.emissive         = { c.emissive.x * ei, c.emissive.y * ei, c.emissive.z * ei };
    }

    // Local volumes: cull against the froxel far plane (a volume past it cannot touch a
    // single voxel), keep the NEAREST kMaxLocalFogVolumes, then move the emissive ones to the
    // front so FroxelLightInject walks only that prefix. The participating medium keeps the
    // nearest Media::kMaxLocals at any distance.
    uint32_t total = 0;
    if (auto* pool = world.GetPool<LocalFogVolumeComponent>();
        pool && !pool->Entities().empty())
    {
        struct Candidate
        {
            float                                dist;
            bool                                 emissive;
            bool                                 inRange;   // within the froxel far plane
            VolumetricFogPass::LocalFogVolumeGPU gpu;
        };
        std::vector<Candidate> cands;
        cands.reserve(pool->Entities().size());

        const XMFLOAT3 cam      = FrameViewWrite().view.cameraPosition;
        const float    viewDist = out.fog.owned
                                ? out.fog.viewDistance
                                : (m_volFogPass ? m_volFogPass->GetRangeFar() : 80.0f);

        const auto& ents = pool->Entities();
        const auto& data = pool->Data();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const LocalFogVolumeComponent& v = data[i];
            if (!v.enabled || !(v.density > 0.0f)) continue;
            const GlobalTransform* gt = world.GetComponent<GlobalTransform>(ents[i]);
            if (!gt) continue;
            ++total;

            const XMMATRIX M = XMLoadFloat4x4(&gt->matrix);
            XMVECTOR det;
            const XMMATRIX inv = XMMatrixInverse(&det, M);
            if (!(std::fabs(XMVectorGetX(det)) > 1e-12f)) continue;   // zero-scale axis
            XMVECTOR sV, rQ, tV;
            if (!XMMatrixDecompose(&sV, &rQ, &tV, M)) continue;
            XMFLOAT3 sc, ctr;
            XMStoreFloat3(&sc, XMVectorAbs(sV));
            XMStoreFloat3(&ctr, tV);

            const float radius = (v.shape == LocalFogShape::Sphere)
                ? (std::max)({ sc.x, sc.y, sc.z })
                : std::sqrt(sc.x * sc.x + sc.y * sc.y + sc.z * sc.z);
            const float dx = ctr.x - cam.x, dy = ctr.y - cam.y, dz = ctr.z - cam.z;
            const float dist = (std::max)(std::sqrt(dx * dx + dy * dy + dz * dz) - radius, 0.0f);

            Candidate c{};
            c.dist = dist;
            c.inRange = dist <= viewDist;
            // Row-vector world matrix: local = world * inv, so each local axis is a COLUMN of inv,
            // i.e. a row of its transpose.
            XMFLOAT4X4 t;
            XMStoreFloat4x4(&t, XMMatrixTranspose(inv));
            const float rows[12] = { t._11, t._12, t._13, t._14,
                                     t._21, t._22, t._23, t._24,
                                     t._31, t._32, t._33, t._34 };
            std::memcpy(c.gpu.worldToLocal, rows, sizeof(rows));
            c.gpu.albedo[0] = std::clamp(v.albedo.x, 0.0f, 1.0f);
            c.gpu.albedo[1] = std::clamp(v.albedo.y, 0.0f, 1.0f);
            c.gpu.albedo[2] = std::clamp(v.albedo.z, 0.0f, 1.0f);
            c.gpu.density   = v.density;
            const float ei  = (std::max)(v.emissiveIntensity, 0.0f);
            c.gpu.emissive[0] = v.emissive.x * ei;
            c.gpu.emissive[1] = v.emissive.y * ei;
            c.gpu.emissive[2] = v.emissive.z * ei;
            c.gpu.edgeFade      = std::clamp(v.edgeFade, 0.0f, 1.0f);
            c.gpu.shape         = static_cast<uint32_t>(v.shape);
            c.gpu.heightFalloff = (std::max)(v.heightFalloff, 0.0f);
            c.gpu.halfHeight    = sc.y;
            c.emissive = (c.gpu.emissive[0] + c.gpu.emissive[1] + c.gpu.emissive[2]) > 0.0f;
            cands.push_back(c);
        }

        {
            std::vector<Candidate> medium = cands;
            if (medium.size() > Media::kMaxLocals)
            {
                std::nth_element(medium.begin(), medium.begin() + Media::kMaxLocals, medium.end(),
                                 [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
                medium.resize(Media::kMaxLocals);
            }
            out.mediumLocals.reserve(medium.size());
            for (const Candidate& c : medium)
                out.mediumLocals.push_back(c.gpu);
        }
        cands.erase(std::remove_if(cands.begin(), cands.end(), [](const Candidate& c) { return !c.inRange; }),
                    cands.end());

        const size_t cap = VolumetricFogPass::kMaxLocalFogVolumes;
        if (cands.size() > cap)
        {
            std::nth_element(cands.begin(), cands.begin() + cap, cands.end(),
                             [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
            cands.resize(cap);
        }
        std::stable_partition(cands.begin(), cands.end(),
                              [](const Candidate& c) { return c.emissive; });
        out.locals.reserve(cands.size());
        for (const Candidate& c : cands)
        {
            out.locals.push_back(c.gpu);
            if (c.emissive) ++out.emissiveCount;
        }
    }

    uint8_t bits = 0;
    if (out.fog.owned)       bits |= kFogDemandOwned;
    if (out.fog.enabled)     bits |= kFogDemandEnabled;
    if (out.fog.godRays)     bits |= kFogDemandGodRays;
    if (!out.locals.empty()) bits |= kFogDemandLocal;
    m_volFogDemandBits.store(bits, std::memory_order_relaxed);
    m_localFogTotal.store(total, std::memory_order_relaxed);
    m_localFogKept.store(static_cast<uint32_t>(out.locals.size()), std::memory_order_relaxed);

    // The scene's fog intent for the settings store: its Volumetric Fog component when it has
    // one, else its .ippc's switch, or any enabled local volume (in view or not: the intent
    // must not follow the camera). VolumetricFogPass::WantsFog applies the same rule with the
    // player's allowance on top.
    const bool configOn = out.fog.owned ? out.fog.enabled : m_fogSceneConfig;
    const bool intent   = configOn || total > 0;
    const std::string_view writer = out.fog.owned ? std::string_view("VolumetricFog")
                                  : (!configOn && total > 0) ? std::string_view("LocalFogVolume")
                                  : std::string_view(m_fogConfigWriter);
    RecordSceneFogIntent(intent, writer);
}

void Renderer::RecordSceneFogIntent(bool intent, std::string_view writer)
{
    m_sceneFogIntent.store(intent, std::memory_order_relaxed);
    const uint8_t now = intent ? 1u : 0u;
    if (now == m_fogIntentRecorded && writer == m_fogIntentWriterRecorded) return;
    m_fogIntentRecorded       = now;
    m_fogIntentWriterRecorded = std::string(writer);
    Settings::SettingsStore& st = Settings::SettingsStore::Get();
    if (const Settings::Row* r = st.Find("features.volumetricFog"))
        st.Write(*r, Settings::Layer::Scene, Settings::Value{ .b = intent }, writer);
}
