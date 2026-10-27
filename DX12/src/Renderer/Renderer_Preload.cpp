// A prefab preload's renderer half: what a cold spawn's first frames would
// register, load and build for one material (its custom shaders, the pipelines
// every pass draws it with, the material surface cache's resources), done before
// the spawn on the thread allowed to do each: the registration on main, shader
// loads and pipeline builds on a TaskSystem worker, the atlas and bake resources
// on main while the render lane is idle. The preload itself: Resource/PrefabSpawnQueue.h.

#include "Renderer/Renderer.h"
#include "Renderer/MaterialPermutation.h"     // MaterialDrawPermutation
#include "ECS/MaterialSchema.h"               // RendererSceneShared.h's schema walk
#include "Renderer/RendererSceneShared.h"     // ComputeMatBits
#include "RenderGraph/RenderPass/GBufferPass.h"
#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "RenderGraph/RenderPass/ShadowPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/SurfaceCacheBakePass.h"
#include "RenderGraph/RenderPass/TranslucentFrontLayerPass.h"
#include "RenderGraph/RenderPass/TransparentPass.h"
#include "Diagnostics/Profiler.h"
#include "System/Check.h"
#include "System/TaskSystem.h"
#include "System/ThreadContext.h"

Renderer::PreloadMaterialWarm Renderer::PreparePreloadMaterial(const MaterialComponent& m, bool skinned)
{
    // The registration half of ResolveCustomPSID / ResolveCustomVSID: the same
    // map entry, the same fan-out, on the one registering thread (main).
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render &&
                            !TaskSystem::OnWorkerThread(),
                            "[SPLITCHECK][SHADERLIB] PreparePreloadMaterial ran off the main thread");
    PreloadMaterialWarm w;
    if (m.useCustomShader && m_gbufferPass)
    {
        RuntimeMaterialShaderState& state = m_runtimeMaterialShaders[BuildMaterialShaderKey(m)];
        if (!m.customShaderPath.empty() && state.customPSID < 0)
        {
            PROF_SCOPE("Mat.RegisterCustomPS");
            const uint32_t id = RegisterMaterialShader(m.customShaderPath.c_str(), RHI::ShaderStage::PS);
            state.customPSID = (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);
        }
        if (state.customVSID < 0)
        {
            PROF_SCOPE("Mat.RegisterCustomVS");
            const std::string vsPath = CustomVertexShaderPath(m);
            if (vsPath.empty())
                state.customVSID = 0;   // pixel-only, as ResolveCustomVSID resolves it
            else
            {
                const uint32_t id = RegisterMaterialShader(vsPath.c_str(), RHI::ShaderStage::VS);
                state.customVSID = (id == ShaderLibrary::kInvalidDynShaderID) ? -1 : static_cast<int>(id);
            }
        }
        // ResolveCustomPSID's early-out: no PS id without a PS path.
        if (!m.customShaderPath.empty() && state.customPSID > 0)
            w.customPSID = static_cast<uint32_t>(state.customPSID);
        if (state.customVSID > 0) w.customVSID = static_cast<uint32_t>(state.customVSID);
    }
    w.reflectPerm = MaterialDrawPermutation(m);

    // The routing the gather gives a candidate of this material (ApplyBlendMode:
    // filter, permutation, shadow cull, cast shadow), as the packet the emit
    // makes of it (Renderer_EmitBatches: depthPrepassEligible).
    DrawCandidate c{};
    c.mc         = &m;
    c.castShadow = 1;
    ApplyBlendMode(c, DirectX::XMFLOAT3{ 0.f, 0.f, 1.f });
    DrawPacket& dp = w.packet;
    dp.permutation          = c.perm;
    dp.filter               = c.filter;
    dp.castShadow           = c.castShadow;
    dp.shadowCullMode       = c.shadowCullMode;
    dp.stencilRef           = c.stencilRef;
    dp.mirrored             = 0;
    dp.viewMask             = ViewBit::All;
    dp.renderInMainPass     = 1;
    dp.customPSID           = w.customPSID;
    dp.customVSID           = w.customVSID;
    // A masked custom material's shadow runs its own hook (the emit's rule in
    // Renderer_EmitBatches), so the shadow passes warm the SHADOW_HOOK PSO their
    // per-draw loops will ask for.
    dp.shadowHookPS         = (w.customPSID != 0 && c.perm.Has(PermutationKey::ALPHA_TEST) &&
                               ShadowHookEnabled()) ? 1u : 0u;
    dp.prevPosElementBase   = skinned ? 0u : 0xFFFFFFFFu;   // a skinned draw carries one
    dp.depthPrepassEligible = (!c.perm.Has(PermutationKey::ALPHA_TEST) && w.customPSID == 0) ? 1u : 0u;

    w.outline = m.IsOutlineEnabled() && c.filter == DrawFilter::Opaque;
    // SurfaceAtlasEligible's test, on the material's bits.
    const uint16_t bits = ComputeMatBits(&m);
    w.surfaceCache = m_surfaceCache.IsEnabled() && (bits & MB_CUSTOM_SHADER) && w.customPSID != 0 &&
                     !(bits & MB_ALPHA_TEST) && !(bits & MB_FRAME_PARAMS) && !(bits & MB_AUTHORED_RT) &&
                     c.filter == DrawFilter::Opaque && (bits & MB_IDENTITY_UV);
    return w;
}

void Renderer::WarmPreloadMaterial(const PreloadMaterialWarm& w)
{
    PROF_SCOPE("Preload.WarmMaterial");
    // A hot reload's cache clears wait for this material (ShaderLibrary.h).
    ShaderCacheGate::Shared gate;
    if (m_gbufferPass)
    {
        // The first resolve's shader miss (MaterialShaderReflection's union).
        ShaderLibrary& lib = m_gbufferPass->GetShaderLibrary();
        if (w.customPSID) (void)lib.GetDynamicReflectionUnion(w.customPSID, { PermutationKey{}, w.reflectPerm });
        if (w.customVSID) (void)lib.GetDynamicReflectionUnion(w.customVSID, { PermutationKey{}, w.reflectPerm });
        m_gbufferPass->WarmPipelines(w.packet);
    }
    if (m_transparentPass) m_transparentPass->WarmPipelines(w.packet);
    if (m_frontLayerPass)  m_frontLayerPass->WarmPipelines(w.packet);
    if (m_shadowPass)      m_shadowPass->WarmPipelines(w.packet);
    if (m_spotShadowPass)  m_spotShadowPass->WarmPipelines(w.packet);
    if (m_pointShadowPass) m_pointShadowPass->WarmPipelines(w.packet);
    if (w.outline && m_outlineGeometryPass)        m_outlineGeometryPass->WarmPipelines();
    if (w.surfaceCache && m_surfaceCacheBakePass)  m_surfaceCacheBakePass->WarmPipelines(w.packet);
}

void Renderer::WarmPreloadSurfaceCache()
{
    PROF_SCOPE("Preload.WarmSurfaceCache");
    ShaderCacheGate::Shared gate;
    if (m_surfaceCacheBakePass) m_surfaceCacheBakePass->WarmShaders();
}

bool Renderer::PrewarmSurfaceCache()
{
    (void)ENGINE_CHECK_ONCE(Sys::ThreadCtx::Current() != Sys::ThreadCtx::Render &&
                            !TaskSystem::OnWorkerThread(),
                            "[SPLITCHECK] PrewarmSurfaceCache ran off the main thread");
    if (!m_surfaceCache.IsEnabled()) return false;
    PROF_SCOPE("Preload.SurfaceCache");
    const bool atlas = m_surfaceCache.PrepareAtlas(m_gfx);
    const bool bake  = m_surfaceCacheBakePass && m_surfaceCacheBakePass->Prewarm(m_gfx);
    return atlas && bake;
}
