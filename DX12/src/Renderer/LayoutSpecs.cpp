#include "Renderer/LayoutSpecs.h"

#include "Graphics/LayoutCheck.h"
#include "Graphics/DDGIVolumeManager.h"
#include "Graphics/GpuLight.h"
#include "Graphics/ParticleSystem.h"
#include "Graphics/ReflectionProbeTypes.h"
#include "Renderer/Renderer.h"
#include "Resource/SystemHandles.h"

// The C++ side of each mirrored block, member by member (Graphics/LayoutCheck.h).
// Every row is checked against DXC's reflection of each shader that declares the
// block, on load and on every hot reload: a row that pairs the wrong members is
// reported, not trusted.
//
// ADDING A BLOCK: draft its table with tools/gen_layout_spec.py (pairs the C++ and
// HLSL members in declaration order, `--alias` lends other declarations' names by
// position), paste it here, add a LAYOUT_SPEC row, run any scene and read the
// "[LAYOUT] <block> ... first checked" line. EDITING A STRUCT: change both sides;
// a member the table does not list is still position-checked, and listing it
// makes its NAME checked too.

namespace RendererDetail
{
namespace
{
    // Drafted by tools/gen_layout_spec.py --struct LightCB --hlsl shaders/light_cb.hlsli
    // --block LightCB. Mirror: shaders/light_cb_fields.hlsli, the one member list
    // light_cb.hlsli includes in its cbuffer (every graphics user - checked here)
    // and in its struct form (a ConstantBuffer<LightCB> opened from the heap, which
    // reflection does not show, so the shared text is what keeps it in step).
    const Layout::Var kLightCB[] = {
        LAYOUT_VAR("lightDir", LightCB, lightDir),
        LAYOUT_VAR("_p0", LightCB, pad0),
        LAYOUT_VAR("lightColor", LightCB, lightColor),
        LAYOUT_VAR("_p1", LightCB, pad1),
        LAYOUT_VAR("cameraPos", LightCB, cameraPos),
        LAYOUT_VAR("_p3", LightCB, pad3),
        LAYOUT_VAR("invViewProj", LightCB, invViewProj),
        LAYOUT_VAR("iblRadianceMips", LightCB, iblRadianceMips),
        LAYOUT_VAR("iblStrength", LightCB, iblStrength),
        LAYOUT_VAR("iblUseSH", LightCB, iblUseSH),
        LAYOUT_VAR("aerialMaxDistKm", LightCB, aerialMaxDistKm),
        LAYOUT_VAR("shadowMatrix", LightCB, shadowMatrix),
        LAYOUT_VAR("cascadeSplits", LightCB, cascadeSplits),
        LAYOUT_VAR("cascadeTexelWorldSize", LightCB, cascadeTexelWorldSize),
        LAYOUT_VAR("shadowBias", LightCB, shadowBias),
        LAYOUT_VAR("shadowStrength", LightCB, shadowStrength),
        LAYOUT_VAR("shadowMapTexelSize", LightCB, shadowMapTexelSize),
        LAYOUT_VAR("shadowBlendRange", LightCB, shadowBlendRange),
        LAYOUT_VAR("shadowFrameIndex", LightCB, shadowFrameIndex),
        LAYOUT_VAR("shadowNormalOffset", LightCB, shadowNormalOffset),
        LAYOUT_VAR("rtShadowEnabled", LightCB, rtShadowEnabled),
        LAYOUT_VAR("ssgiResultIdx", LightCB, ssgiResultIdx),
        LAYOUT_VAR("cameraForward", LightCB, cameraForward),
        LAYOUT_VAR("ssgiIntensity", LightCB, ssgiIntensity),
        LAYOUT_VAR("viewMatrix", LightCB, viewMatrix),
        LAYOUT_VAR("clusterNearZ", LightCB, clusterNearZ),
        LAYOUT_VAR("clusterFarZ", LightCB, clusterFarZ),
        LAYOUT_VAR("clusterLightCount", LightCB, clusterLightCount),
        LAYOUT_VAR("nprMinBrightness", LightCB, nprMinBrightness),
        LAYOUT_VAR("reflectionProbeCount", LightCB, reflectionProbeCount),
        LAYOUT_VAR("nprMaxBrightness", LightCB, nprMaxBrightness),
        LAYOUT_VAR("_reflectionPad", LightCB, reflectionPad),
        LAYOUT_VAR("ddgiVolumeCount", LightCB, ddgiVolumeCount),
        LAYOUT_VAR("ddgiEnabled", LightCB, ddgiEnabled),
        LAYOUT_VAR("ddgiDiffuseScale", LightCB, ddgiDiffuseScale),
        LAYOUT_VAR("_padSkyDiffuse", LightCB, _padSkyDiffuse),
        LAYOUT_VAR("ddgiAONearFieldStrength", LightCB, ddgiAONearFieldStrength),
        LAYOUT_VAR("viewMode", LightCB, viewMode),
        LAYOUT_VAR("exposureBufIdx", LightCB, exposureBufIdx),
        LAYOUT_VAR("ambientIntensity", LightCB, ambientIntensity),
        LAYOUT_VAR("perObjShadowMatrix", LightCB, perObjShadowMatrix),
        LAYOUT_VAR("perObjShadowParams", LightCB, perObjShadowParams),
        LAYOUT_VAR("perObjShadowTexelSize", LightCB, perObjShadowTexelSize),
        LAYOUT_VAR("ambientColor", LightCB, ambientColor),
        LAYOUT_VAR("hairShadowMatrix", LightCB, hairShadowMatrix),
        LAYOUT_VAR("hairShadowParams", LightCB, hairShadowParams),
        LAYOUT_VAR("hairShadowTexelSize", LightCB, hairShadowTexelSize),
        LAYOUT_VAR("hairShadowStrength", LightCB, hairShadowStrength),
        LAYOUT_VAR("rtShadowMaskIdx", LightCB, rtShadowMaskIdx),
        LAYOUT_VAR("rtReflectionIdx", LightCB, rtReflectionIdx),
        LAYOUT_VAR("pointShadowIdx", LightCB, pointShadowIdx),
        LAYOUT_VAR("perObjShadowIdx", LightCB, perObjShadowIdx),
        LAYOUT_VAR("hairShadowIdx", LightCB, hairShadowIdx),
        LAYOUT_VAR("ddgiVolumeBufIdx", LightCB, ddgiVolumeBufIdx),
        LAYOUT_VAR("reflProbeGridIdx", LightCB, reflProbeGridIdx),
        LAYOUT_VAR("reflProbeIndexIdx", LightCB, reflProbeIndexIdx),
        LAYOUT_VAR("ssrResultIdx", LightCB, ssrResultIdx),
        LAYOUT_VAR("reflProbeBufIdx", LightCB, reflProbeBufIdx),
        LAYOUT_VAR("gWaterGlobals", LightCB, waterGlobals),
        LAYOUT_VAR("gWaterRTSize", LightCB, waterRTSize),
        LAYOUT_VAR("gWaterBodies", LightCB, waterBodies),
        LAYOUT_VAR("gCloudShadowRight", LightCB, cloudShadowRight),
        LAYOUT_VAR("gCloudShadowUp", LightCB, cloudShadowUp),
        LAYOUT_VAR("gCloudShadowIdx", LightCB, cloudShadowIdx),
        LAYOUT_VAR("gNprPtAmbientIdx", LightCB, nprPtAmbientIdx),
        LAYOUT_VAR("gFaceFrameIdx", LightCB, faceFrameIdx),
        LAYOUT_VAR("gTransmissionApplied", LightCB, transmissionApplied),
        LAYOUT_VAR("gFwdReflNormalIdx", LightCB, fwdReflNormalIdx),
        LAYOUT_VAR("gFwdReflDepthIdx", LightCB, fwdReflDepthIdx),
        LAYOUT_VAR("gFwdReflStrength", LightCB, fwdReflStrength),
        LAYOUT_VAR("gFwdReflPrevHdrIdx", LightCB, fwdReflPrevHdrIdx),
        LAYOUT_VAR("gFwdReflPrevHdrMaxMip", LightCB, fwdReflPrevHdrMaxMip),
        LAYOUT_VAR("gFwdReflNearZ", LightCB, fwdReflNearZ),
        LAYOUT_VAR("gFwdReflFarZ", LightCB, fwdReflFarZ),
        LAYOUT_VAR("gFwdReflRoughMax", LightCB, fwdReflRoughMax),
        LAYOUT_VAR("gFwdReflRayMax", LightCB, fwdReflRayMax),
        LAYOUT_VAR("gFwdReflPrevJitterUV", LightCB, fwdReflPrevJitterUV),
        LAYOUT_VAR("gFwdReflFrontRtrIdx", LightCB, fwdReflFrontRtrIdx),
        LAYOUT_VAR("gFwdReflFrontDepthIdx", LightCB, fwdReflFrontDepthIdx),
        LAYOUT_VAR("gFwdAoIdx", LightCB, fwdAoIdx),
        LAYOUT_VAR("gRestirDiIdx", LightCB, restirDiIdx),
        LAYOUT_VAR("gPtResultIdx", LightCB, ptResultIdx),
        LAYOUT_VAR("gCsmCamPos", LightCB, csmCamPos),
        LAYOUT_VAR("gRenderPathMode", LightCB, renderPathMode),
        LAYOUT_VAR("gCsmCamFwd", LightCB, csmCamFwd),
        LAYOUT_VAR("gRestirGiIdx", LightCB, restirGiIdx),
    };

    // Drafted by tools/gen_layout_spec.py --struct PerViewCB --hlsl
    // shaders/material_bindings.hlsli --block PerViewCB, names lent by
    // gbuffer_vertex.hlsli, DDGIProbeDebug.ps.hlsl, Water.vs.hlsl,
    // BillboardFX.vs.hlsl and Skybox.vs.hlsl. 19 shader files declare a
    // `cbuffer PerViewCB` - most a prefix of this struct, under five naming
    // schemes; a member none of the names below covers is checked by position.
    // WorldTextFX.hlsli / WorldUI.vs.hlsl declare ANOTHER struct under the same
    // name and register (view-projection + the UI clock, g_wuTimeSec at 64):
    // `foreign` names it so those blocks are skipped rather than reported.
    const Layout::Var kPerViewCB[] = {
        LAYOUT_VAR("viewProj|g_ViewProj|g_viewProj", PerViewCB, viewProj),
        LAYOUT_VAR("prevViewProj|g_PrevViewProj|g_prevViewProj", PerViewCB, prevViewProj),
        LAYOUT_VAR("curViewProjNoJitter|g_CurViewProjNoJitter|g_curViewProjNoJitter", PerViewCB, curViewProjNoJitter),
        LAYOUT_VAR("gTimeParams|g_TimeParams", PerViewCB, timeParams),
        LAYOUT_VAR("gCameraPosWS|g_CameraPosWS", PerViewCB, cameraPosWS),
        LAYOUT_VAR("gDissolveNoiseIdx", PerViewCB, dissolveNoiseIdx),
        LAYOUT_VAR("gInstanceRemapIdx", PerViewCB, instanceRemapIdx),
        LAYOUT_VAR("gFrameIndex", PerViewCB, frameIndex),
        LAYOUT_VAR("_gDissolvePad", PerViewCB, _dissolvePad),
    };

    // Drafted by tools/gen_layout_spec.py --struct MaterialGPUData --hlsl
    // shaders/material.hlsli --block MaterialGPUData. The element of every
    // register-bound StructuredBuffer<MaterialGPUData> (g_Materials t2,
    // gMaterials t12, ...); C++ is #pragma pack(1), so the element stride must
    // be exactly sizeof (976).
    using Resource::MaterialGPUData;
    const Layout::Var kMaterialGPUData[] = {
        LAYOUT_VAR("roughnessMin", MaterialGPUData, roughnessMin),
        LAYOUT_VAR("roughnessMax", MaterialGPUData, roughnessMax),
        LAYOUT_VAR("metalnessMin", MaterialGPUData, metalnessMin),
        LAYOUT_VAR("metalnessMax", MaterialGPUData, metalnessMax),
        LAYOUT_VAR("reflectance", MaterialGPUData, reflectance),
        LAYOUT_VAR("normalStrength", MaterialGPUData, normalStrength),
        LAYOUT_VAR("saturation", MaterialGPUData, saturation),
        LAYOUT_VAR("alphaRef", MaterialGPUData, alphaRef),
        LAYOUT_VAR("baseColor", MaterialGPUData, baseColor),
        LAYOUT_VAR("specularColor", MaterialGPUData, specularColor),
        LAYOUT_VAR("emissiveColor", MaterialGPUData, emissiveColor),
        LAYOUT_VAR("shaderType", MaterialGPUData, shaderType),
        LAYOUT_VAR("nprMinBrightness", MaterialGPUData, nprMinBrightness),
        LAYOUT_VAR("nprShadowThreshold", MaterialGPUData, nprShadowThreshold),
        LAYOUT_VAR("nprShadowSmooth", MaterialGPUData, nprShadowSmooth),
        LAYOUT_VAR("nprRimPower", MaterialGPUData, nprRimPower),
        LAYOUT_VAR("nprRimStrength", MaterialGPUData, nprRimStrength),
        LAYOUT_VAR("nprRampBlend", MaterialGPUData, nprRampBlend),
        LAYOUT_VAR("nprBrightnessClamp", MaterialGPUData, nprBrightnessClamp),
        LAYOUT_VAR("nprDiffuseRampColor", MaterialGPUData, nprDiffuseRampColor),
        LAYOUT_VAR("nprMaxBrightness", MaterialGPUData, nprMaxBrightness),
        LAYOUT_VAR("nprShadowRampColor", MaterialGPUData, nprShadowRampColor),
        LAYOUT_VAR("_padNprShadow", MaterialGPUData, _padNprShadow),
        LAYOUT_VAR("nprMidWeight", MaterialGPUData, nprMidWeight),
        LAYOUT_VAR("nprVeinWeight", MaterialGPUData, nprVeinWeight),
        LAYOUT_VAR("nprSSSWeight", MaterialGPUData, nprSSSWeight),
        LAYOUT_VAR("_padNprSss", MaterialGPUData, _padNprSss),
        LAYOUT_VAR("textureHandleIds", MaterialGPUData, textureHandleIds),
        LAYOUT_VAR("paramCount", MaterialGPUData, paramCount),
        LAYOUT_VAR("textureCount", MaterialGPUData, textureCount),
        LAYOUT_VAR("materialFlags", MaterialGPUData, materialFlags),
        LAYOUT_VAR("exposureCompensation", MaterialGPUData, exposureCompensation),
        LAYOUT_VAR("customParams", MaterialGPUData, customParams),
        LAYOUT_VAR("customTextureIds", MaterialGPUData, customTextureIds),
        LAYOUT_VAR("customParamCount", MaterialGPUData, customParamCount),
        LAYOUT_VAR("customTextureCount", MaterialGPUData, customTextureCount),
        LAYOUT_VAR("customShadingModel", MaterialGPUData, customShadingModel),
        LAYOUT_VAR("toonRampTex", MaterialGPUData, toonRampTex),
        LAYOUT_VAR("nprSpecThreshold", MaterialGPUData, nprSpecThreshold),
        LAYOUT_VAR("nprSpecSmooth", MaterialGPUData, nprSpecSmooth),
        LAYOUT_VAR("nprSpecStrength", MaterialGPUData, nprSpecStrength),
        LAYOUT_VAR("nprSpecRampBlend", MaterialGPUData, nprSpecRampBlend),
        LAYOUT_VAR("nprShadowStrength", MaterialGPUData, nprShadowStrength),
        LAYOUT_VAR("nprSceneShadowSmooth", MaterialGPUData, nprSceneShadowSmooth),
        LAYOUT_VAR("nprMainLightLerp", MaterialGPUData, nprMainLightLerp),
        LAYOUT_VAR("nprDirectOcclusion", MaterialGPUData, nprDirectOcclusion),
        LAYOUT_VAR("nprRimWidth", MaterialGPUData, nprRimWidth),
        LAYOUT_VAR("_nprPad0", MaterialGPUData, _nprPad0),
        LAYOUT_VAR("_nprPad1", MaterialGPUData, _nprPad1),
        LAYOUT_VAR("_nprPad2", MaterialGPUData, _nprPad2),
        LAYOUT_VAR("uvTilingOffset", MaterialGPUData, uvTilingOffset),
        LAYOUT_VAR("nprDirectLightScale", MaterialGPUData, nprDirectLightScale),
        LAYOUT_VAR("nprAdditionalLightScale", MaterialGPUData, nprAdditionalLightScale),
        LAYOUT_VAR("nprAmbientLightScale", MaterialGPUData, nprAmbientLightScale),
        LAYOUT_VAR("nprSpecularLightScale", MaterialGPUData, nprSpecularLightScale),
        LAYOUT_VAR("nprSceneLightColorWeight", MaterialGPUData, nprSceneLightColorWeight),
        LAYOUT_VAR("nprSceneLightIntensityWeight", MaterialGPUData, nprSceneLightIntensityWeight),
        LAYOUT_VAR("nprReferenceLightLuminance", MaterialGPUData, nprReferenceLightLuminance),
        LAYOUT_VAR("nprExposureBiasEV", MaterialGPUData, nprExposureBiasEV),
        LAYOUT_VAR("nprShoulderStart", MaterialGPUData, nprShoulderStart),
        LAYOUT_VAR("nprShoulderRange", MaterialGPUData, nprShoulderRange),
        LAYOUT_VAR("nprShoulderStrength", MaterialGPUData, nprShoulderStrength),
        LAYOUT_VAR("_nprEnergyPad0", MaterialGPUData, _nprEnergyPad0),
        LAYOUT_VAR("anisotropy", MaterialGPUData, anisotropy),
        LAYOUT_VAR("anisotropyRotation", MaterialGPUData, anisotropyRotation),
        LAYOUT_VAR("subsurfaceWrap", MaterialGPUData, subsurfaceWrap),
        LAYOUT_VAR("subsurfaceStrength", MaterialGPUData, subsurfaceStrength),
        LAYOUT_VAR("subsurfaceColor", MaterialGPUData, subsurfaceColor),
        LAYOUT_VAR("subsurfaceOpacity", MaterialGPUData, subsurfaceOpacity),
        LAYOUT_VAR("clearCoat", MaterialGPUData, clearCoat),
        LAYOUT_VAR("clearCoatRoughness", MaterialGPUData, clearCoatRoughness),
        LAYOUT_VAR("_clearCoatPad0", MaterialGPUData, _clearCoatPad0),
        LAYOUT_VAR("_clearCoatPad1", MaterialGPUData, _clearCoatPad1),
        LAYOUT_VAR("faceShadow", MaterialGPUData, faceShadow),
        LAYOUT_VAR("transmission", MaterialGPUData, transmission),
        LAYOUT_VAR("ior", MaterialGPUData, ior),
        LAYOUT_VAR("thickness", MaterialGPUData, thickness),
        LAYOUT_VAR("attenuationDistance", MaterialGPUData, attenuationDistance),
        LAYOUT_VAR("attenuationColor", MaterialGPUData, attenuationColor),
        LAYOUT_VAR("_transmissionPad0", MaterialGPUData, _transmissionPad0),
    };

    Layout::Spec MakePerView()
    {
        Layout::Spec s = LAYOUT_SPEC("PerViewCB", CBuffer, PerViewCB, kPerViewCB);
        s.foreign = "g_wuTimeSec";
        return s;
    }

    // Drafted by tools/gen_layout_spec.py --cpp include/Graphics/FogApply.h --struct
    // Constants --hlsl shaders/fog_apply.hlsli --block FogApplyCB.
    // Mirror: shaders/fog_apply.hlsli (one declaration, every user).
    const Layout::Var kFogApplyCB[] = {
        LAYOUT_VAR("gFogInvViewProj", FogApply::Constants, invViewProj),
        LAYOUT_VAR("gFogCameraPos", FogApply::Constants, cameraPos),
        LAYOUT_VAR("gFogFlags", FogApply::Constants, flags),
        LAYOUT_VAR("gFogDirToSun", FogApply::Constants, dirToSun),
        LAYOUT_VAR("gFogNearZ", FogApply::Constants, nearZ),
        LAYOUT_VAR("gFogSunColor", FogApply::Constants, sunColor),
        LAYOUT_VAR("gFogFarZ", FogApply::Constants, farZ),
        LAYOUT_VAR("gFogInvRenderSize", FogApply::Constants, invRenderSize),
        LAYOUT_VAR("gFogFroxelIdx", FogApply::Constants, froxelIdx),
        LAYOUT_VAR("gFogOpaqueDepthIdx", FogApply::Constants, opaqueDepthIdx),
        LAYOUT_VAR("gFogHfDensity", FogApply::Constants, hfDensity),
        LAYOUT_VAR("gFogHfFalloff", FogApply::Constants, hfFalloff),
        LAYOUT_VAR("gFogHfHeight", FogApply::Constants, hfHeight),
        LAYOUT_VAR("gFogHfStart", FogApply::Constants, hfStart),
        LAYOUT_VAR("gFogHfColor", FogApply::Constants, hfColor),
        LAYOUT_VAR("gFogHfMaxOpacity", FogApply::Constants, hfMaxOpacity),
        LAYOUT_VAR("gFogHfSunInscatter", FogApply::Constants, hfSunInscatter),
        LAYOUT_VAR("gFogHfAnisotropy", FogApply::Constants, hfAnisotropy),
        LAYOUT_VAR("gFogFroxelNear", FogApply::Constants, froxelNear),
        LAYOUT_VAR("gFogFroxelFar", FogApply::Constants, froxelFar),
        LAYOUT_VAR("gFogFroxelDepth", FogApply::Constants, froxelDepth),
        LAYOUT_VAR("gFogSoftFadeRange", FogApply::Constants, softFadeRange),
        LAYOUT_VAR("_gFogPad", FogApply::Constants, _pad),
    };

    // Drafted by tools/gen_layout_spec.py --struct ParticleGPU --hlsl
    // shaders/Particle.hlsli --block Particle. The element of the GPU particle
    // pool (the emit / update / boid kernels' RWStructuredBuffer<Particle> u0,
    // particle_vertex.hlsli's StructuredBuffer<Particle> t4). The stride check is
    // the point: the pool's views use sizeof(ParticleGPU) = 128, and a 124-byte
    // HLSL struct makes buckets 3-7 draw nothing.
    const Layout::Var kParticleGPU[] = {
        LAYOUT_VAR("position", ParticleGPU, position),
        LAYOUT_VAR("lifetime", ParticleGPU, lifetime),
        LAYOUT_VAR("velocity", ParticleGPU, velocity),
        LAYOUT_VAR("maxLifetime", ParticleGPU, maxLifetime),
        LAYOUT_VAR("startColor", ParticleGPU, startColor),
        LAYOUT_VAR("endColor", ParticleGPU, endColor),
        LAYOUT_VAR("startSize", ParticleGPU, startSize),
        LAYOUT_VAR("textureBindlessIdx", ParticleGPU, textureBindlessIdx),
        LAYOUT_VAR("visualMode", ParticleGPU, visualMode),
        LAYOUT_VAR("materialIndex", ParticleGPU, materialIndex),
        LAYOUT_VAR("phaseSeed", ParticleGPU, phaseSeed),
        LAYOUT_VAR("orientation", ParticleGPU, orientation),
        LAYOUT_VAR("intensity", ParticleGPU, intensity),
        LAYOUT_VAR("stretch", ParticleGPU, stretch),
        LAYOUT_VAR("startScale", ParticleGPU, startScale),
        LAYOUT_VAR("drag", ParticleGPU, drag),
        LAYOUT_VAR("emissiveFade", ParticleGPU, emissiveFade),
        LAYOUT_VAR("flags", ParticleGPU, flags),
        LAYOUT_VAR("fade", ParticleGPU, fade),
        LAYOUT_VAR("_pad5", ParticleGPU, _pad5),
    };

    Layout::Spec MakeParticle()
    {
        Layout::Spec s = LAYOUT_SPEC("Particle", StructElement, ParticleGPU, kParticleGPU);
        return s;
    }

    // DDGI's per-probe state (Graphics/DDGIVolumeManager.h DDGI::ProbeData vs
    // shaders/DDGICommon.hlsli DDGIProbeData): the relocate kernel's
    // RWStructuredBuffer u0 and every sampler's StructuredBuffer. The stride is
    // the point, as for the particles: the probe-data views use sizeof(ProbeData)
    // = 48, the sky visibility's first moment included.
    const Layout::Var kDDGIProbeData[] = {
        LAYOUT_VAR("offset", DDGI::ProbeData, offset),
        LAYOUT_VAR("state", DDGI::ProbeData, state),
        LAYOUT_VAR("skyVisibility", DDGI::ProbeData, skyVisibility),
        LAYOUT_VAR("pushSmooth", DDGI::ProbeData, pushSmooth),
        LAYOUT_VAR("skyVisDir", DDGI::ProbeData, skyVisDir),
        LAYOUT_VAR("_padSkyDir", DDGI::ProbeData, _padSkyDir),
    };

    // One reflection probe: a single definition for C++ and HLSL,
    // shaders/shared/reflection_probe_gpu.hlsli, so this checks the packing rule
    // rather than a copy - against every shader that reads the probe buffer
    // (Lighting.ps, custom_surface, ClusterCullProbes.cs, the hybrid reflection
    // intersects).
    using GpuProbe = GpuShared::ReflectionProbe;
    const Layout::Var kReflectionProbe[] = {
        LAYOUT_VAR("worldToProxyR0", GpuProbe, worldToProxyR0),
        LAYOUT_VAR("worldToProxyR1", GpuProbe, worldToProxyR1),
        LAYOUT_VAR("worldToProxyR2", GpuProbe, worldToProxyR2),
        LAYOUT_VAR("worldToInflR0", GpuProbe, worldToInflR0),
        LAYOUT_VAR("worldToInflR1", GpuProbe, worldToInflR1),
        LAYOUT_VAR("worldToInflR2", GpuProbe, worldToInflR2),
        LAYOUT_VAR("proxyExtents", GpuProbe, proxyExtents),
        LAYOUT_VAR("proxyShape", GpuProbe, proxyShape),
        LAYOUT_VAR("capturePosWS", GpuProbe, capturePosWS),
        LAYOUT_VAR("mipCount", GpuProbe, mipCount),
        LAYOUT_VAR("inflExtents", GpuProbe, inflExtents),
        LAYOUT_VAR("inflShape", GpuProbe, inflShape),
        LAYOUT_VAR("fadePos", GpuProbe, fadePos),
        LAYOUT_VAR("intensity", GpuProbe, intensity),
        LAYOUT_VAR("fadeNeg", GpuProbe, fadeNeg),
        LAYOUT_VAR("priority", GpuProbe, priority),
        LAYOUT_VAR("cubemapSrvIndex", GpuProbe, cubemapSrvIndex),
        LAYOUT_VAR("flags", GpuProbe, flags),
        LAYOUT_VAR("_pad", GpuProbe, _pad),
    };

    // One mesh's vertex streams: shaders/shared/mesh_descriptor.hlsli, the
    // element of the global mesh-descriptor StructuredBuffer every PVF vertex
    // shader and ParticleEmit.cs read. Each stream is a 16-byte StreamDesc.
    using GpuMesh = GpuShared::MeshDescriptor;
    const Layout::Var kMeshDescriptor[] = {
        LAYOUT_VAR("position", GpuMesh, position),
        LAYOUT_VAR("normal", GpuMesh, normal),
        LAYOUT_VAR("tangent", GpuMesh, tangent),
        LAYOUT_VAR("uv0", GpuMesh, uv0),
        LAYOUT_VAR("uv1", GpuMesh, uv1),
        LAYOUT_VAR("color", GpuMesh, color),
        LAYOUT_VAR("indexBufferIndex", GpuMesh, indexBufferIndex),
        LAYOUT_VAR("indexByteOffset", GpuMesh, indexByteOffset),
        LAYOUT_VAR("indexFormat", GpuMesh, indexFormat),
        LAYOUT_VAR("vertexCount", GpuMesh, vertexCount),
        LAYOUT_VAR("prevPosition", GpuMesh, prevPosition),
        LAYOUT_VAR("uv2", GpuMesh, uv2),
        LAYOUT_VAR("uv3", GpuMesh, uv3),
    };

    // One clustered light: shaders/shared/gpu_light.hlsli, the element of the
    // light buffers the cluster cull, the lighting, the fog and ReSTIR DI / PT
    // read. Named members here, so the last two words (power and source radius in
    // integer storage) are offset-checked, not only ranged.
    using GpuLightRec = GpuShared::GPULight;
    const Layout::Var kGPULight[] = {
        LAYOUT_VAR("position", GpuLightRec, position),
        LAYOUT_VAR("radius", GpuLightRec, radius),
        LAYOUT_VAR("color", GpuLightRec, color),
        LAYOUT_VAR("intensity", GpuLightRec, intensity),
        LAYOUT_VAR("direction", GpuLightRec, direction),
        LAYOUT_VAR("spotAngle", GpuLightRec, spotAngle),
        LAYOUT_VAR("type", GpuLightRec, type),
        LAYOUT_VAR("shadowSliceIdx", GpuLightRec, shadowSliceIdx),
        LAYOUT_VAR("powerBits", GpuLightRec, powerBits),
        LAYOUT_VAR("sourceRadiusBits", GpuLightRec, sourceRadiusBits),
    };

    const Layout::Spec kSpecs[] = {
        LAYOUT_SPEC("LightCB", CBuffer, LightCB, kLightCB),
        MakePerView(),
        LAYOUT_SPEC("MaterialGPUData", StructElement, MaterialGPUData, kMaterialGPUData),
        LAYOUT_SPEC("FogApplyCB", CBuffer, FogApply::Constants, kFogApplyCB),
        MakeParticle(),
        LAYOUT_SPEC("DDGIProbeData", StructElement, DDGI::ProbeData, kDDGIProbeData),
        LAYOUT_SPEC("ReflectionProbe", StructElement, GpuShared::ReflectionProbe, kReflectionProbe),
        LAYOUT_SPEC("MeshDescriptor", StructElement, GpuShared::MeshDescriptor, kMeshDescriptor),
        LAYOUT_SPEC("GPULight", StructElement, GpuShared::GPULight, kGPULight),
    };
} // namespace

void RegisterLayoutSpecs()
{
    Layout::Register(kSpecs, static_cast<uint32_t>(sizeof(kSpecs) / sizeof(kSpecs[0])));
}
} // namespace RendererDetail
