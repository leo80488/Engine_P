#include "RenderGraph/RenderPass/ReflectionProbeCapturePass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "ECS/HierarchyComponents.h"   // ViewBit (per-view filter)
#include "System/Log.h"

#include <cstring>
#include <algorithm>

namespace
{
    // Root-parameter indices come from Graphics/RootSignatureLayout.h - the
    // local names below are aliases that keep this file's vocabulary, never new
    // numbers. This file drives BOTH root signatures: the prefilter dispatch uses
    // the compute sig (C::), the cube-face draws the graphics sig (S:: absolute /
    // SRel:: group-relative).
    namespace S    = RHI::Slots::Gfx;
    namespace SRel = RHI::Slots::GfxRel;
    namespace C    = RHI::Slots::Cmp;

    // Mirror of SkyIBLPass's compute root sig slots: the same shared compute
    // signature.
    constexpr auto kCSCBSlot = C::CB;      // b0 space2
    constexpr auto kCSSrv0   = C::SRV_t0;  // t0 space2
    constexpr auto kCSUav0   = C::UAV_u0;  // u0 space2

    // Master graphics root sig slot conventions (see RHI::Slots::Gfx).
    constexpr auto kCBVSlotBase         = S::CBVBase; // b1..b7
    constexpr auto kInstanceBufSlot     = S::InstanceBuf; // root SRV  t0 space0
    constexpr auto kMeshDescSlot        = S::MeshDesc;    // root SRV  t1 space0
    constexpr auto kSRVSlotBase         = S::SRVBase; // desc tables t2..t5 space0
    constexpr auto kBindlessBufSlot     = S::BindlessBuffers; // t0 space1 - PVF g_Buffers[]
    constexpr auto kBindlessTexSlot     = S::BindlessTex;     // t0 space2
    constexpr auto kSkyCubeSRVSlot      = S::SRV_t6; // t6 space0 - sky cube for the capture sky PS
    constexpr auto kSkySHSRVSlot        = S::SkySH;  // t19 space0
    // GROUP-RELATIVE: this one goes through BindConstantBufferAtOffset, which
    // adds the CBV group base itself.
    constexpr auto kShadowCBSlot        = SRel::CB_b2;      // b2 space0 - ProbeShadowCB (slot 1 -> b2)
    constexpr auto kShadowSRVSlot       = S::ShadowSRV;     // t9 space0 - CSM Texture2DArray
    // No sampler slots: ProbeCapture.ps samples the material base colour with
    // the fixed SMP_LINEAR_WRAP heap slot and the CSM with
    // SMP_CMP_LINEAR_BORDER_WHITE_GE (reversed-Z, as LightingPass does, so the
    // bake matches deferred shadows). Every draw here uses this pass's own PSOs,
    // never a material's shader, so no run-time sampler either.

    // Karis sample counts per output mip (matches SkyIBLPass).
    constexpr uint32_t kPrefilterSampleTable[7] = { 1, 128, 128, 64, 32, 32, 32 };
    constexpr uint32_t kCBSlotStride = 256;       // D3D12 CBV alignment

    // Byte 228 carries the instance-remap heap index because GBuffer.vs reads
    // gInstanceRemapIdx at that fixed offset from EVERY b1 layout; cameraPos
    // sits at 240 so a float can never alias the remap field (a garbage heap
    // index would fault the VS's bindless remap fetch). Mirrors ProbeCaptureCB
    // in ProbeCapture.ps.hlsl.
    struct alignas(16) CaptureCB
    {
        float    viewProj[16];
        float    prevViewProj[16];          // = viewProj (no TAA)
        float    curViewProjNoJitter[16];   // = viewProj
        float    sunDir[3];     float pad0;    // 192
        float    sunColor[3];   float pad1;    // 208
        uint32_t pad2;                         // 224 (dissolve idx slot; unused)
        uint32_t instanceRemapIdx;             // 228 (0 = identity)
        uint32_t pad4[2];                      // 232
        float    cameraPos[3];  float pad3;    // 240
    };
    static_assert(sizeof(CaptureCB) <= kCBSlotStride, "CaptureCB exceeds 256-byte slot");
    static_assert(offsetof(CaptureCB, instanceRemapIdx) == 228,
                  "instanceRemapIdx must sit at byte 228 — fixed PerViewCB remap offset");

    // Frame-global shadow + ambient controls bound at b2. cascadeVP matches
    // LightCB.shadowMatrix (transposed) so the capture PS reuses the same
    // row-vector mul + cascade-select math the deferred pass uses.
    struct alignas(16) ProbeShadowCB
    {
        float    cascadeVP[4][16];                   // 256 - transposed cascade view-projs
        float    cascadeSplits[4];                   // 16
        float    camPos[3];     float shadowStrength;// 16
        float    camFwd[3];     float iblStrength;   // 16
        float    ambientScale;  float shadowTexel;   // 16 - shadowTexel = 1 / CSM resolution (PCF step)
        float    pad[2];
    }; // 320 bytes
    constexpr uint32_t kShadowCBSize = 512;          // 256-aligned, holds the 320-byte struct
    static_assert(sizeof(ProbeShadowCB) <= kShadowCBSize, "ProbeShadowCB too large");

    struct alignas(16) PrefilterCB
    {
        uint32_t faceSize;
        uint32_t sampleCount;
        float    roughness;
        uint32_t sourceMipCount;
        uint32_t faceIndex;
        uint32_t useCoverage;
        uint32_t pad1;
        uint32_t pad2;
    };
    static_assert(sizeof(PrefilterCB) == 32, "PrefilterCB must match HLSL");

    // A face's view-proj, transposed for the CB: shaders use the row-vector
    // convention, mul(float4(pos,1), viewProj), and GBuffer.vs multiplies
    // from the left.
    void BuildFaceViewProj(const DirectX::XMFLOAT3& probePos,
                           uint32_t face,
                           DirectX::XMFLOAT4X4& outVP)
    {
        using namespace DirectX;
        const XMFLOAT4X4 vp = ReflectionProbeCapturePass::FaceViewProj(probePos, face);
        XMStoreFloat4x4(&outVP, XMMatrixTranspose(XMLoadFloat4x4(&vp)));
    }
} // namespace

// The six faces in DX face order: 0:+X  1:-X  2:+Y  3:-Y  4:+Z  5:-Z.
DirectX::XMFLOAT3 ReflectionProbeCapturePass::FaceForward(uint32_t face)
{
    switch (face)
    {
    case 0:  return {  1.f,  0.f,  0.f };
    case 1:  return { -1.f,  0.f,  0.f };
    case 2:  return {  0.f,  1.f,  0.f };
    case 3:  return {  0.f, -1.f,  0.f };
    case 4:  return {  0.f,  0.f,  1.f };
    default: return {  0.f,  0.f, -1.f };
    }
}

// Reverse-Z perspective (near=1, far=0) to match the engine's depth convention.
DirectX::XMFLOAT4X4 ReflectionProbeCapturePass::FaceViewProj(const DirectX::XMFLOAT3& probePos, uint32_t face)
{
    using namespace DirectX;
    XMVECTOR eye = XMLoadFloat3(&probePos);

    XMVECTOR fwd, up;
    switch (face)
    {
    case 0: fwd = XMVectorSet( 1, 0, 0, 0); up = XMVectorSet(0, 1, 0, 0); break; // +X
    case 1: fwd = XMVectorSet(-1, 0, 0, 0); up = XMVectorSet(0, 1, 0, 0); break; // -X
    case 2: fwd = XMVectorSet( 0, 1, 0, 0); up = XMVectorSet(0, 0,-1, 0); break; // +Y
    case 3: fwd = XMVectorSet( 0,-1, 0, 0); up = XMVectorSet(0, 0, 1, 0); break; // -Y
    case 4: fwd = XMVectorSet( 0, 0, 1, 0); up = XMVectorSet(0, 1, 0, 0); break; // +Z
    default: fwd = XMVectorSet(0, 0,-1, 0); up = XMVectorSet(0, 1, 0, 0); break; // -Z
    }

    XMMATRIX view = XMMatrixLookToLH(eye, fwd, up);
    // 90 deg FOV per face; reverse-Z by swapping near / far.
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, kFarZ, kNearZ);
    XMFLOAT4X4 vp;
    XMStoreFloat4x4(&vp, view * proj);
    return vp;
}

bool ReflectionProbeCapturePass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::GBuffer_VS,        RHI::ShaderStage::VS, "GBuffer.vs.hlsl");
    m_shaderLib.Register(ShaderID::ProbeCapture_PS,   RHI::ShaderStage::PS, "ProbeCapture.ps.hlsl");
    m_shaderLib.Register(ShaderID::Skybox_VS,         RHI::ShaderStage::VS, "Skybox.vs.hlsl");
    m_shaderLib.Register(ShaderID::ProbeCaptureSky_PS, RHI::ShaderStage::PS, "ProbeCaptureSky.ps.hlsl");
    m_shaderLib.Register(ShaderID::SpecularPrefilter_CS, RHI::ShaderStage::CS, "SpecularPrefilter.cs.hlsl");

    if (!CreatePipelines(gfx)) return false;

    // ---- Temp single-cube capture target ----
    {
        RHI::TextureDesc td{};
        td.width      = kFaceSize;
        td.height     = kFaceSize;
        td.arraySize = 6;
        td.mipLevels = 1;            // capture writes mip 0; prefilter sources only mip 0
        td.format     = RHI::Format::R16G16B16A16_FLOAT;
        td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::RENDER_TARGET;
        td.usage      = RHI::Usage::DEFAULT;
        td.miscFlags = RHI::ResourceMiscFlag::TEXTURECUBE;
        td.layout     = RHI::ResourceState::SHADER_RESOURCE;
        td.debugName = "ReflectionProbeCapture.TempCube";
        if (!gfx.CreateTexture(td, m_tempCube))
        {
            LOG_ERROR("ReflectionProbeCapturePass: temp cube creation failed");
            return false;
        }
    }

    // ---- Depth buffer (D32, 128x128, single 2D) ----
    {
        RHI::TextureDesc td{};
        td.width      = kFaceSize;
        td.height     = kFaceSize;
        td.arraySize = 1;
        td.mipLevels = 1;
        td.format     = RHI::Format::D32_FLOAT;
        td.bindFlags = RHI::BindFlag::DEPTH_STENCIL;
        td.usage      = RHI::Usage::DEFAULT;
        td.layout     = RHI::ResourceState::DEPTHSTENCIL;
        td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0); // reverse Z far = 0
        td.debugName = "ReflectionProbeCapture.Depth";
        if (!gfx.CreateTexture(td, m_depthBuffer))
        {
            LOG_ERROR("ReflectionProbeCapturePass: depth buffer creation failed");
            return false;
        }
    }

    // ---- CB rings (UPLOAD heap, persistently mapped) ----
    auto makeCB = [&](RHI::GPUBuffer& cb, void*& mapped, uint64_t size, const char* label)
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = size;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::CONSTANT_BUFFER;
        if (!gfx.CreateBuffer(bd, cb))
        {
            LOG_ERROR("ReflectionProbeCapturePass: %s CB creation failed", label);
            return false;
        }
        mapped = gfx.MapBuffer(cb);
        if (mapped) std::memset(mapped, 0, size);
        return true;
    };
    if (!makeCB(m_captureCB,   m_captureCBMapped,   kCBSlotStride * 6,            "capture"))   return false;
    if (!makeCB(m_prefilterCB, m_prefilterCBMapped, kCBSlotStride * kFaceMips * 6, "prefilter")) return false;
    if (!makeCB(m_shadowCB,    m_shadowCBMapped,    kShadowCBSize,                "shadow"))     return false;

    LOG_INFO("ReflectionProbeCapturePass: ready (%ux%u, %u mips, temp cube + depth allocated)",
             kFaceSize, kFaceSize, kFaceMips);
    return true;
}

// Onto the live members: a failed rebuild keeps the old pipeline.
bool ReflectionProbeCapturePass::CreatePipelines(IGraphicsDevice& gfx)
{
    // ---- Capture graphics PSO ----
    {
        const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::GBuffer_VS);
        const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::ProbeCapture_PS);
        if (!vs || !ps)
        {
            LOG_ERROR("ReflectionProbeCapturePass: shader load failed");
            return false;
        }
        // PipelineStateDesc holds POINTERS to these state structs; static keeps
        // them alive past CreatePipelineState.
        static RHI::RasterizerState   s_rs{};
        static RHI::DepthStencilState s_dss{};
        static RHI::BlendState        s_bs{};
        s_rs.cullMode         = RHI::CullMode::BACK;
        s_rs.depthClipEnable = true;
        s_dss.depthEnable     = true;
        s_dss.depthWriteMask = RHI::DepthWriteMask::ALL;
        s_dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL; // reverse Z
        s_dss.stencilEnable   = false;
        s_bs.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

        RHI::PipelineStateDesc d{};
        d.vs = vs;
        d.ps = ps;
        d.rs  = &s_rs;
        d.dss = &s_dss;
        d.bs  = &s_bs;
        d.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtvCount      = 1;
        d.dsvFormat     = RHI::Format::D32_FLOAT;
        if (!gfx.CreatePipelineState(d, m_capturePSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: capture PSO creation failed");
            return false;
        }

        // Mirrored variant: identical except the front-face convention, so
        // BACK still culls the back face on negative-determinant instances.
        static RHI::RasterizerState s_rsMirrored{};
        s_rsMirrored = s_rs;
        s_rsMirrored.frontCounterClockwise = true;
        d.rs = &s_rsMirrored;
        if (!gfx.CreatePipelineState(d, m_capturePSOMirrored))
        {
            LOG_ERROR("ReflectionProbeCapturePass: mirrored capture PSO creation failed");
            return false;
        }
    }

    // ---- Skybox PSO ----
    // Skybox.vs forces clip Z = 0 (the reverse-Z far plane), so GREATER_EQUAL
    // with depth write OFF puts sky only on pixels the opaque pass left at the
    // cleared far plane. No backface cull: the cube is drawn from inside.
    {
        const RHI::Shader* vs = m_shaderLib.GetShader(ShaderID::Skybox_VS);
        const RHI::Shader* ps = m_shaderLib.GetShader(ShaderID::ProbeCaptureSky_PS);
        if (!vs || !ps)
        {
            LOG_ERROR("ReflectionProbeCapturePass: skybox shader load failed");
            return false;
        }
        static RHI::RasterizerState   s_skyRS{};
        static RHI::DepthStencilState s_skyDSS{};
        static RHI::BlendState        s_skyBS{};
        s_skyRS.cullMode         = RHI::CullMode::NONE;     // viewer is inside the cube
        s_skyRS.depthClipEnable = true;
        s_skyDSS.depthEnable     = true;
        s_skyDSS.depthWriteMask = RHI::DepthWriteMask::ZERO; // never overwrite opaque depth
        s_skyDSS.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;
        s_skyDSS.stencilEnable   = false;
        s_skyBS.renderTarget[0].renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

        RHI::PipelineStateDesc d{};
        d.vs  = vs;
        d.ps  = ps;
        d.rs  = &s_skyRS;
        d.dss = &s_skyDSS;
        d.bs  = &s_skyBS;
        d.rtvFormats[0] = RHI::Format::R16G16B16A16_FLOAT;
        d.rtvCount      = 1;
        d.dsvFormat     = RHI::Format::D32_FLOAT;
        if (!gfx.CreatePipelineState(d, m_skyPSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: sky PSO creation failed");
            return false;
        }
    }

    // ---- Prefilter compute PSO ----
    {
        const RHI::Shader* cs = m_shaderLib.GetShader(ShaderID::SpecularPrefilter_CS);
        if (!cs) { LOG_ERROR("ReflectionProbeCapturePass: prefilter CS missing"); return false; }
        RHI::PipelineStateDesc d{};
        d.cs = cs;
        if (!gfx.CreatePipelineState(d, m_prefilterPSO))
        {
            LOG_ERROR("ReflectionProbeCapturePass: prefilter PSO creation failed");
            return false;
        }
    }
    return true;
}

void ReflectionProbeCapturePass::ReloadShaders(IGraphicsDevice& gfx)
{
    if (!m_gfx) return;   // Init never ran
    if (m_shaderLib.ClearCaches()) CreatePipelines(gfx);
}

void ReflectionProbeCapturePass::DrawSceneFace(RHI::CommandList cl,
                                                uint32_t face,
                                                const BakeContext& ctx)
{
    IGraphicsDevice& gfx = *m_gfx;

    // Pack and upload the per-face capture CB at this face's ring slot.
    if (m_captureCBMapped)
    {
        CaptureCB cb{};
        DirectX::XMFLOAT4X4 vp;
        BuildFaceViewProj(ctx.probePos, face, vp);
        std::memcpy(cb.viewProj,            &vp, sizeof(vp));
        std::memcpy(cb.prevViewProj,        &vp, sizeof(vp));
        std::memcpy(cb.curViewProjNoJitter, &vp, sizeof(vp));
        cb.sunDir[0]    = ctx.sunDir.x;   cb.sunDir[1]    = ctx.sunDir.y;   cb.sunDir[2]    = ctx.sunDir.z;
        cb.sunColor[0]  = ctx.sunColor.x; cb.sunColor[1]  = ctx.sunColor.y; cb.sunColor[2]  = ctx.sunColor.z;
        cb.instanceRemapIdx = ctx.instanceRemapIdx;   // GBuffer.vs reads it at byte 228
        cb.cameraPos[0] = ctx.probePos.x; cb.cameraPos[1] = ctx.probePos.y; cb.cameraPos[2] = ctx.probePos.z;
        std::memcpy(static_cast<uint8_t*>(m_captureCBMapped) + face * kCBSlotStride,
                    &cb, sizeof(cb));
    }

    // Bind the RTV (temp cube face) + DSV (shared depth). The subresource is a
    // flat array slice, arraySlice == cubeIdx * 6 + face; this pass always
    // captures into cube 0, so the slice IS the face index. The depth buffer has
    // arraySize=1, so its standard DSV covers it (SetRenderTargetSubresource
    // resolves it).
    m_gfx->SetRenderTargetSubresource(m_tempCube, /*arraySlice=*/face, /*mip=*/0,
                                      &m_depthBuffer, cl);

    // Clear colour (HDR black) + depth (reverse-Z far = 0). Alpha is geometry
    // coverage for the indirect-specular probe contract: empty / sky pixels stay
    // 0, ProbeCapture.ps writes 1 for captured geometry.
    // The colour clear goes through the cube-face RTV SetRenderTargetSubresource
    // cached on the texture entry, so it clears only this face (ClearRenderTarget
    // would clear the whole texture). The depth buffer is D32, so
    // ClearDepthStencil clears depth only.
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    gfx.ClearRenderTargetSubresource(m_tempCube, /*arraySlice=*/face, /*mip=*/0, clearColor, cl);
    gfx.ClearDepthStencil(m_depthBuffer, 0.0f, 0, cl);

    // Viewport + scissor sized to a single face.
    gfx.SetViewport(RHI::Viewport{ 0.0f, 0.0f, float(kFaceSize), float(kFaceSize), 0.0f, 1.0f }, cl);
    gfx.SetScissorRect(0, 0, kFaceSize, kFaceSize, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // Bind the capture PSO + descriptor heaps (the command list pool binds the
    // heaps at frame start; BindDescriptorHeaps is idempotent).
    gfx.BindPipelineState(m_capturePSO, cl);
    cl.BindDescriptorHeaps();

    // Per-face capture CB at b1 through the slot-offset helper, so the root CBV
    // points at this face's slot of the ring the 6 face draws share.
    gfx.BindConstantBufferAtOffset(SRel::CB_b1, m_captureCB,
                                   face * kCBSlotStride, cl);

    // PVF root SRVs (instance / mesh descriptor tables).
    if (ctx.instanceBuffer && ctx.instanceBuffer->IsValid())
        gfx.SetRootBufferSRV(kInstanceBufSlot, *ctx.instanceBuffer, cl);
    if (ctx.meshDescBuffer && ctx.meshDescBuffer->IsValid())
        gfx.SetRootBufferSRV(kMeshDescSlot, *ctx.meshDescBuffer, cl);

    // Material buffer at t2 space0 (root slot 10 = kSRVSlotBase + 0).
    if (ctx.materialBufSrv)
        gfx.BindDescriptorTableGpuHandle(RHI::Slots::Gfx::SRV_t2, ctx.materialBufSrv, cl);

    // Fallback textures t3 / t4 / t5 - the root sig demands all three. t3
    // (BaseColor) and t4 (Surface) -> 1x1 white; t5 (Normal) -> 1x1 flat normal
    // so unmapped pixels stay neutral instead of pointing to (1,1,1).
    if (ctx.defaultWhiteSrv)
    {
        gfx.BindDescriptorTableGpuHandle(RHI::Slots::Gfx::SRV_t3, ctx.defaultWhiteSrv, cl);
        gfx.BindDescriptorTableGpuHandle(RHI::Slots::Gfx::SRV_t4, ctx.defaultWhiteSrv, cl);
    }
    gfx.BindDescriptorTableGpuHandle(
        RHI::Slots::Gfx::SRV_t5,
        ctx.defaultFlatNormSrv ? ctx.defaultFlatNormSrv : ctx.defaultWhiteSrv,
        cl);

    // Bindless BUFFER pool - GBuffer.vs's PVF fetch reads vertex / index data
    // through g_Buffers[] at t0 space1. Do not skip it: the slot would point at
    // undefined heap memory and the GPU page-faults on the first vertex fetch ->
    // DEVICE_HUNG / TDR.
    if (ctx.bindlessBufTable)
        gfx.BindDescriptorTableGpuHandle(kBindlessBufSlot, ctx.bindlessBufTable, cl);

    // Bindless texture pool + sky SH SRV (used by the capture PS for SH ambient).
    if (ctx.bindlessTexTable)
        gfx.BindDescriptorTableGpuHandle(kBindlessTexSlot, ctx.bindlessTexTable, cl);
    if (ctx.skySHSrv)
        gfx.BindDescriptorTableGpuHandle(kSkySHSRVSlot, ctx.skySHSrv, cl);

    // Shadow / ambient CB at b2 + CSM shadow array (t9).
    // The capture PS shadows the direct sun and scales / AO-occludes the ambient.
    // With shadowArraySrv 0 the PS reads shadowStrength == 0 -> unshadowed bake.
    gfx.BindConstantBufferAtOffset(kShadowCBSlot, m_shadowCB, 0, cl);
    if (ctx.shadowArraySrv)
        gfx.BindDescriptorTableGpuHandle(kShadowSRVSlot, ctx.shadowArraySrv, cl);

    // This face's draws (Renderer::CullView with the face's own frustum): the
    // opaque packets, then the shadow-only ones (entities the main frustum cull
    // rejected but the CSM shadow cull kept), so probes capture the full room,
    // not just what the camera looks at. Packets are sorted with `mirrored` next
    // to the permutation, so the PSO switches at most a couple of times per face,
    // not per draw.
    bool psoIsMirrored = false;
    auto drawList = [&](const DrawList& list)
    {
        for (const DrawPacket& dp : list)
        {
            // Honour the author's per-view intent: VisibilityComponent's "Reflection
            // Probe" checkbox clears this bit. A probe bake is cached, so an excluded
            // entity drawn here would stay burned into the cubemap across reloads.
            if ((dp.viewMask & ViewBit::ReflectionProbe) == 0) continue;

            const bool wantMirrored = (dp.mirrored != 0);
            if (wantMirrored != psoIsMirrored)
            {
                gfx.BindPipelineState(wantMirrored ? m_capturePSOMirrored : m_capturePSO, cl);
                psoIsMirrored = wantMirrored;
            }
            // EVERY draw binds its t3: ProbeCapture.ps samples t3 whenever the material
            // has no bindless base colour, so a packet without a texture would otherwise
            // take the PREVIOUS draw's and its colour would depend on the draw order. No
            // texture -> white, as at the face's start. legacyTextureBind (harness
            // -probecull_legacy) keeps the order-dependent bind.
            if (ctx.legacyTextureBind)
            {
                if (dp.texBaseColor)
                    gfx.BindDescriptorTableGpuHandle(RHI::Slots::Gfx::SRV_t3, dp.texBaseColor, cl);
            }
            else if (dp.texBaseColor || ctx.defaultWhiteSrv)
                gfx.BindDescriptorTableGpuHandle(RHI::Slots::Gfx::SRV_t3,
                                                 dp.texBaseColor ? dp.texBaseColor : ctx.defaultWhiteSrv, cl);

            const uint32_t consts[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            gfx.SetRootConstants4(consts[0], consts[1], consts[2], consts[3], cl);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    };
    drawList(ctx.faceDraws[face < 6 ? face : 0]);
    // Transparent geometry is intentionally excluded from the coverage bake:
    // probe alpha is a binary geometry-coverage contract, not transmittance.

    // ---- Skybox draw: fills uncovered (Z = 0) pixels with sky ----
    // The sky PSO tests GREATER_EQUAL with depth write OFF, so it writes only
    // where the opaque pass left the cleared far plane, and leaves the depth
    // buffer unchanged for the rest of this face.
    if (ctx.skyCubeVB && ctx.skyCubeVB->IsValid() && ctx.skyCubemapSrv)
    {
        gfx.BindPipelineState(m_skyPSO, cl);

        // Skybox.vs reads PerViewCB at b1 (already bound to the face VP) and gCubeVB
        // at t2 space0: this overwrites root slot 10 (kSRVSlotBase + 0) for the draw.
        // The MaterialBuffer binding goes stale; the next face's DrawSceneFace
        // rebinds it.
        gfx.BindResource(SRel::SRV_t2, *ctx.skyCubeVB, cl);

        // ProbeCaptureSky.ps reads gSkyCube at t6 space0: root slot SRV_t6 of the
        // shared graphics sig (the IBLIrradiance slot).
        gfx.BindDescriptorTableGpuHandle(kSkyCubeSRVSlot, ctx.skyCubemapSrv, cl);

        cl.DrawInstanced(36, 1, 0, 0); // 12 tris x 3 verts = 36
    }
}

void ReflectionProbeCapturePass::BakeProbe(RHI::CommandList cl, uint32_t cubeIdx,
                                            const BakeContext& ctx)
{
    if (!ctx.probeArray || !ctx.probeArray->IsValid())
    {
        LOG_WARNING("ReflectionProbeCapturePass: probe array is invalid, skipping bake");
        return;
    }

    auto& dx12 = *m_gfx;

    // Upload the frame-global shadow / ambient CB once per bake (see m_shadowCB).
    if (m_shadowCBMapped)
    {
        ProbeShadowCB sb{};
        for (int c = 0; c < 4; ++c)
            std::memcpy(sb.cascadeVP[c], &ctx.cascadeVP[c], sizeof(DirectX::XMFLOAT4X4));
        sb.cascadeSplits[0] = ctx.cascadeSplits.x; sb.cascadeSplits[1] = ctx.cascadeSplits.y;
        sb.cascadeSplits[2] = ctx.cascadeSplits.z; sb.cascadeSplits[3] = ctx.cascadeSplits.w;
        sb.camPos[0] = ctx.camPos.x; sb.camPos[1] = ctx.camPos.y; sb.camPos[2] = ctx.camPos.z;
        sb.camFwd[0] = ctx.camFwd.x; sb.camFwd[1] = ctx.camFwd.y; sb.camFwd[2] = ctx.camFwd.z;
        sb.shadowStrength = (ctx.shadowArraySrv != 0) ? ctx.shadowStrength : 0.0f;
        sb.iblStrength    = ctx.iblStrength;
        sb.ambientScale   = ctx.ambientScale;
        sb.shadowTexel    = ctx.shadowTexel;
        std::memcpy(m_shadowCBMapped, &sb, sizeof(sb));
    }

    // ---- Phase 1: render 6 faces into the temp cube (mip 0) ----
    if (m_tempCubeState != RHI::ResourceState::RENDERTARGET)
    {
        dx12.PushBarrier(RHI::GPUBarrier::Image(
            &m_tempCube, m_tempCubeState, RHI::ResourceState::RENDERTARGET), cl);
        m_tempCubeState = RHI::ResourceState::RENDERTARGET;
    }
    for (uint32_t face = 0; face < 6; ++face)
        DrawSceneFace(cl, face, ctx);

    // ---- Phase 2: temp cube -> SRV, probe array -> UAV ----
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        &m_tempCube, RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    m_tempCubeState = RHI::ResourceState::SHADER_RESOURCE;

    // Transitions ALL subresources of the probe array (mip -1, slice -1), not
    // only this probe's 6 slices: the array has no per-cube state tracking, so
    // every cube goes through UAV. Per-subresource transitions would narrow it.
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        ctx.probeArray, RHI::ResourceState::SHADER_RESOURCE,
        RHI::ResourceState::UNORDERED_ACCESS), cl);

    // ---- Phase 3: prefilter dispatches into the probe's cube of the array ----
    {
        dx12.BindComputePipelineState(m_prefilterPSO, cl);
        const uint64_t srcSrv = m_gfx->GetTextureSRVGpuHandle(m_tempCube);
        dx12.SetComputeDescriptorTable(kCSSrv0, srcSrv, cl);

        for (uint32_t mip = 0; mip < kFaceMips; ++mip)
        {
            const uint32_t faceW = std::max(1u, kFaceSize >> mip);
            const float    roughness = (kFaceMips <= 1)
                                       ? 0.0f : float(mip) / float(kFaceMips - 1);

            // Bind the per-(cube, mip) UAV: a 6-slice view of the probe array.
            const uint64_t dstUav =
                m_gfx->GetTextureCubeMipUAVGpuHandle(*ctx.probeArray, cubeIdx, mip);
            dx12.SetComputeDescriptorTable(kCSUav0, dstUav, cl);

            // Per (face, mip) CB: the shader takes FaceIndex and covers one face per
            // dispatch, 7 x 6 = 42 dispatches per probe.
            for (uint32_t face = 0; face < 6; ++face)
            {
                PrefilterCB c{};
                c.faceSize       = faceW;
                c.sampleCount    = (mip < 7) ? kPrefilterSampleTable[mip] : 32u;
                c.roughness      = roughness;
                c.sourceMipCount = 1;
                c.faceIndex      = face;
                c.useCoverage    = 1;

                const uint32_t slot = face * kFaceMips + mip;
                uint8_t* dst = static_cast<uint8_t*>(m_prefilterCBMapped) + slot * kCBSlotStride;
                std::memcpy(dst, &c, sizeof(c));
                dx12.SetComputeRootCBV(kCSCBSlot, m_prefilterCB, slot * kCBSlotStride, cl);

                const uint32_t gx = (faceW + 7) / 8;
                const uint32_t gy = (faceW + 7) / 8;
                dx12.DispatchCompute(gx, gy, 1, cl);

                // UAV barrier between dispatches sharing the same destination
                // resource so the GPU serializes the writes.
                dx12.PushBarrier(RHI::GPUBarrier::Memory(nullptr), cl);
            }
        }
    }

    // ---- Phase 4: probe array -> SR (ready for the lighting pass) ----
    dx12.PushBarrier(RHI::GPUBarrier::Image(
        ctx.probeArray, RHI::ResourceState::UNORDERED_ACCESS,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}
