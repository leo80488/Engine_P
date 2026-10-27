#include "RenderGraph/RenderPass/UIPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "System/Log.h"
#include <algorithm>
#include <cstring>

// Engine default root signature slots (as DebugWirePass uses them), from
// RHI::Slots (Graphics/RootSignatureLayout.h):
//   BindConstantBuffer takes a group-relative index (GfxRel::*)
//   BindDescriptorTableGpuHandle / SetGraphicsRootConstant take an absolute root
//   index (Gfx::*)
static constexpr auto kRootSlot_VertexSRV  = RHI::Slots::Gfx::SRV_t2; // t2 space0
static constexpr auto kRootSlot_TextureSRV = RHI::Slots::Gfx::SRV_t3; // t3 space0

void UIPass::EnsureViewRing(uint32_t viewSlot)
{
    if (!m_gfx || viewSlot == 0 || viewSlot >= RHI::kMaxViews || m_vbMapped[0][viewSlot]) return;
    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(kMaxIndices) * sizeof(UI::UIVertex);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_gfx->CreateBuffer(bd, m_vertexBuffer[i][viewSlot]))
            m_vbMapped[i][viewSlot] = m_gfx->MapBuffer(m_vertexBuffer[i][viewSlot]);
        else
            LOG_ERROR("%s: view %u vertex buffer[%u] creation failed", m_logName, viewSlot, i);
    }
}

void UIPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::UI_VS, RHI::ShaderStage::VS, "UI.vs.hlsl");
    m_shaderLib.Register(ShaderID::UI_PS, RHI::ShaderStage::PS, "UI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Vertex buffer, sized for kMaxIndices flat-expanded triangle verts: UIDrawList
    // stores verts + indices, and UIPass expands them to a flat triangle list at upload
    // because the PVF root signature has no IA index buffer. One per frame in flight so
    // CPU writes don't race in-flight GPU reads.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxIndices) * sizeof(UI::UIVertex);
        bd.stride     = 0;
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_vertexBuffer[i][0]))
                m_vbMapped[i][0] = gfx.MapBuffer(m_vertexBuffer[i][0]);
            else
                LOG_ERROR("UIPass: vertex buffer[%u] creation failed", i);
        }
    }

    // UI CB (canvas size + UI clock at b1 space0).
    m_cb.Create(gfx, "UI.CB");

    // SDF text effects table (b2 space0).
    m_effectsCB.Create(gfx, "UI.EffectsCB");

    // 1x1 white default texture, bound when a draw cmd has no texture so the PS path
    // stays unified (sample x tint = tint when sampling white).
    if (!CreateWhiteTexture())
        LOG_ERROR("UIPass: white texture creation failed");

    // UV samplers: clamp/wrap/mirror x linear/point. The order must match
    // UI::UISamplerId(wrap, point): [0..2] linear, [3..5] point.
    {
        const RHI::TextureAddressMode kAddr[3] = {
            RHI::TextureAddressMode::CLAMP,
            RHI::TextureAddressMode::WRAP,
            RHI::TextureAddressMode::MIRROR,
        };
        const RHI::Filter kFilter[2] = {
            RHI::Filter::MIN_MAG_MIP_LINEAR,
            RHI::Filter::MIN_MAG_MIP_POINT,
        };
        for (uint32_t f = 0; f < 2; ++f)
            for (uint32_t a = 0; a < 3; ++a)
            {
                RHI::SamplerDesc sd{};
                sd.filter    = kFilter[f];
                sd.addressU = kAddr[a];
                sd.addressV = kAddr[a];
                sd.addressW = kAddr[a];
                if (!gfx.CreateSampler(sd, m_samplers[f * 3 + a]))
                    LOG_ERROR("UIPass: sampler %u creation failed", f * 3 + a);
            }
    }

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("%s: initialized (max %u verts, %u indices, white tex SRV=%llu)",
                    m_logName, kMaxVertices, kMaxIndices,
                    static_cast<unsigned long long>(m_whiteTexSrv));
    else
        LOG_ERROR("UIPass: PSO creation failed");
}

bool UIPass::CreateWhiteTexture()
{
    RHI::TextureDesc td{};
    td.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    td.width      = 1;
    td.height     = 1;
    td.depth      = 1;
    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    static const uint8_t kWhitePx[4] = { 255, 255, 255, 255 };
    RHI::SubresourceData sub{};
    sub.dataPtr    = kWhitePx;
    sub.rowPitch   = 4;
    sub.slicePitch = 4;

    if (!m_gfx->CreateTexture(td, m_whiteTex, &sub)) return false;
    m_whiteTexSrv = m_gfx->GetTextureSRVGpuHandle(m_whiteTex);
    return m_whiteTexSrv != 0;
}

PSODesc UIPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::UI_VS;
    desc.psID        = ShaderID::UI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;

    // No depth - UI draws on top of everything.
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    // Standard alpha-over blend.
    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.destBlend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    rt.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    // The composition buffer's format: RGBA8 in SDR, FP16 in an HDR output mode.
    // Pushed by the Renderer; see m_targetFormat in the header.
    desc.rtvFormats[0] = m_targetFormat;
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;

    return desc;
}

bool UIPass::Execute(RHI::CommandList cl,
                     const RHI::Texture* target,
                     RHI::ResourceState entryState,
                     uint32_t canvasW, uint32_t canvasH,
                     float timeSec,
                     const UI::UIDrawList& list)
{
    if (!enabled || !m_gfx || !target || !target->IsValid()) return false;
    if (list.IsEmpty()) return false;
    if (canvasW == 0 || canvasH == 0) return false;

    auto& gfx = *m_gfx;
    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return false;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    const uint32_t viewSlot  = RHI::CurrentRecordingView().slot;
    if (viewSlot >= RHI::kMaxViews || !m_vbMapped[frameSlot][viewSlot]) return false;

    // ---- Upload: flat-expand UIDrawList -> triangle list VB ----
    // The engine root sig has no IA index buffer slot (PVF), so walk the index list and
    // copy the referenced vertices in order. Each command becomes a contiguous
    // [vertOffset, vertCount) range drawn with DrawInstanced.
    const auto& srcVerts = list.Vertices();
    const auto& srcIdx   = list.Indices();
    const auto& srcCmds  = list.Commands();

    if (srcIdx.size() > kMaxIndices)
    {
        LOG_WARNING("%s: drawlist index count %zu exceeds kMaxIndices %u — truncating",
                    m_logName, srcIdx.size(), kMaxIndices);
    }
    const size_t totalVerts = std::min<size_t>(srcIdx.size(), kMaxIndices);
    UI::UIVertex* dst = static_cast<UI::UIVertex*>(m_vbMapped[frameSlot][viewSlot]);

    struct ExpandedCmd
    {
        uint32_t          vertexOffset;
        uint32_t          vertexCount;
        // Where this command's vertices came from in the CPU drawlist, so the diagnostic
        // reads cached RAM instead of the write-combined m_vbMapped.
        uint32_t          srcIndexOffset;
        UI::Rect          clipRect;
        uint64_t          texSrv;
        uint32_t          materialID;
        uint32_t          effectIndex;
        uint32_t          samplerId;
        uint32_t          customShaderSlot;
    };
    std::vector<ExpandedCmd> expanded;
    expanded.reserve(srcCmds.size());

    uint32_t writeOff = 0;
    for (const auto& cmd : srcCmds)
    {
        if (cmd.indexCount == 0) continue;
        const uint32_t startIdx = cmd.indexOffset;
        const uint32_t endIdx   = startIdx + cmd.indexCount;
        const uint32_t expandStart = writeOff;
        for (uint32_t i = startIdx; i < endIdx; ++i)
        {
            if (writeOff >= kMaxIndices) break;
            const uint16_t vi = srcIdx[i];
            if (vi < srcVerts.size())
                dst[writeOff] = srcVerts[vi];
            ++writeOff;
        }
        const uint32_t expandCount = writeOff - expandStart;
        if (expandCount == 0) continue;
        expanded.push_back({
            expandStart, expandCount, startIdx, cmd.clipRect,
            cmd.texture.srvGpuHandle ? cmd.texture.srvGpuHandle : m_whiteTexSrv,
            cmd.materialID, cmd.effectIndex, cmd.samplerId, cmd.customShaderSlot
        });
        if (writeOff >= kMaxIndices) break;
    }
    if (expanded.empty()) return false;
    (void)totalVerts;

    // ---- Update CB (canvas size + UI clock) ----
    if (auto* slot = m_cb.Current(gfx))
    {
        UIPass::UICB cb{ static_cast<float>(canvasW), static_cast<float>(canvasH),
                         timeSec, 0 };
        *slot = cb;
    }

    // ---- Upload SDF text effects table (b2 space0) ----
    if (auto* fxTable = m_effectsCB.Current(gfx))
    {
        const auto& effects = list.Effects();
        const size_t n = std::min<size_t>(effects.size(), kMaxEffectSlots);
        for (size_t i = 0; i < n; ++i)        fxTable->fx[i] = effects[i];
        for (size_t i = n; i < kMaxEffectSlots; ++i) fxTable->fx[i] = UI::GpuTextEffect{};
    }

    // ---- Transition target -> RT ----
    // Use the caller's tracked state (usually SHADER_RESOURCE after
    // ToneMapPass::TransitionForDisplay, or UNORDERED_ACCESS / SHADER_RESOURCE_COMPUTE
    // when a path skipped that step); do not hard-code the entry state, the resize path
    // can leave the texture in UAV.
    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target,
            entryState,
            RHI::ResourceState::RENDERTARGET), cl);
    }

    // ---- Bind RTV (no clear: preserve the tonemapped scene) ----
    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.topLeftX = 0;
    vp.topLeftY = 0;
    vp.width      = static_cast<float>(canvasW);
    vp.height     = static_cast<float>(canvasH);
    vp.minDepth  = 0.0f;
    vp.maxDepth  = 1.0f;
    gfx.SetViewport(vp, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // UICB at b1 space0 (group-relative slot 0).
    gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b1, m_cb.CurrentBuffer(gfx), cl);
    // Effects table at b2 space0 (group-relative slot 1).
    gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b2, m_effectsCB.CurrentBuffer(gfx), cl);
    // Samplers are set per draw command from m_samplers[ecmd.samplerId].

    // VB ByteAddressBuffer at t2 space0.
    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer[frameSlot][viewSlot]);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    // ---- Draw, switching texture + scissor per command ----
    // Diagnostic: log once per change in command count, not every frame. Per instance
    // (m_lastCmdCount): one counter shared by two UIPass instances with different
    // counts would flip every call and log every command of every frame.
    // The main view's only: a secondary view's count would flip it too.
    const bool uiPassLog = viewSlot == 0 && expanded.size() != m_lastCmdCount;
    if (uiPassLog) {
        m_lastCmdCount = expanded.size();
        LOG_INFO("%s.Execute: %zu draws (canvas %ux%u, whiteSrv=%llu)",
                  m_logName, expanded.size(), canvasW, canvasH,
                  static_cast<unsigned long long>(m_whiteTexSrv));
    }

    uint64_t lastTex = 0;
    // b0 root constants carry (materialID, effectIndex) for the PS. UIPass otherwise
    // never writes b0, so it holds stale constants from an earlier pass: set it before
    // the first draw and on every change. UINT32_MAX forces the first write.
    uint32_t lastMat = UINT32_MAX, lastEff = UINT32_MAX, lastSampler = UINT32_MAX;
    uint32_t lastShaderSlot = 0; // 0 = the default PSO bound above
    int cmdIdx = 0;
    for (const auto& ecmd : expanded)
    {
        // ---- Custom text pixel shader (per-command PSO) ----
        // Slot -> path (UI::TextShaderPaths) -> dynamic PS on this pass's library
        // (RegisterDynamic dedups; the id is DynamicShaderRegistry's) -> PSO permutation. A
        // broken shader falls back to the built-in PSO (PSOCache negative-caches the
        // failure). BindPipelineState re-sets the root signature, which clears every root
        // argument: re-bind b1/b2/t2 here and reset the trackers so texture / sampler / b0
        // re-bind below.
        if (ecmd.customShaderSlot != lastShaderSlot)
        {
            const RHI::PipelineState* usePso = pso;
            if (ecmd.customShaderSlot != 0)
            {
                const auto& paths = UI::TextShaderPaths();
                const uint32_t pathIdx = ecmd.customShaderSlot - 1u;
                if (pathIdx < paths.size())
                {
                    // Slot -> dynId cache: the slot registry is append-only (an index's path never
                    // changes), so pay RegisterDynamic's string key + mutex once per slot. Per
                    // instance: the id belongs to this pass's library.
                    if (m_slotDynIds.size() < paths.size())
                        m_slotDynIds.resize(paths.size(), ShaderLibrary::kInvalidDynShaderID);
                    uint32_t& cached = m_slotDynIds[pathIdx];
                    if (cached == ShaderLibrary::kInvalidDynShaderID)
                        cached = m_shaderLib.RegisterDynamic(
                            paths[pathIdx].c_str(), RHI::ShaderStage::PS, "main");
                    const uint32_t dynId = cached;
                    if (dynId != ShaderLibrary::kInvalidDynShaderID)
                    {
                        PSODesc customDesc = BuildPSODesc();
                        customDesc.psID = static_cast<ShaderID>(dynId);
                        if (const RHI::PipelineState* p2 = m_psoCache.GetOrCreate(customDesc);
                            p2 && p2->IsValid())
                            usePso = p2;
                    }
                }
            }
            gfx.BindPipelineState(*usePso, cl);
            gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b1, m_cb.CurrentBuffer(gfx), cl);        // b1
            gfx.BindConstantBuffer(RHI::Slots::GfxRel::CB_b2, m_effectsCB.CurrentBuffer(gfx), cl); // b2
            if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);
            lastTex = 0;
            lastMat = lastEff = lastSampler = UINT32_MAX;
            lastShaderSlot = ecmd.customShaderSlot;
        }

        const uint32_t l = static_cast<uint32_t>(std::max(0.f, ecmd.clipRect.mn.x));
        const uint32_t t = static_cast<uint32_t>(std::max(0.f, ecmd.clipRect.mn.y));
        const uint32_t r = static_cast<uint32_t>(std::min<float>(static_cast<float>(canvasW),
                                                                  ecmd.clipRect.mx.x));
        const uint32_t b = static_cast<uint32_t>(std::min<float>(static_cast<float>(canvasH),
                                                                  ecmd.clipRect.mx.y));
        if (r <= l || b <= t) {
            if (uiPassLog)
                LOG_WARNING("%s.cmd[%d]: SKIP clip empty (clip=(%.0f,%.0f,%.0f,%.0f) "
                             "→ scissor=(%u,%u,%u,%u))", m_logName, cmdIdx,
                             ecmd.clipRect.mn.x, ecmd.clipRect.mn.y,
                             ecmd.clipRect.mx.x, ecmd.clipRect.mx.y, l, t, r, b);
            ++cmdIdx;
            continue; // fully clipped
        }
        gfx.SetScissorRect(l, t, r, b, cl);

        if (ecmd.texSrv != lastTex)
        {
            gfx.BindDescriptorTableGpuHandle(kRootSlot_TextureSRV, ecmd.texSrv, cl);
            lastTex = ecmd.texSrv;
        }
        // Per-command UV sampler (clamp/wrap/mirror x linear/point), set as the pass's
        // run-time sampler g_UISmp.
        if (ecmd.samplerId != lastSampler)
        {
            const uint32_t sid = (ecmd.samplerId < UI::kUISamplerCount) ? ecmd.samplerId : 0u;
            if (m_samplers[sid] >= 0)
                gfx.SetDynamicSampler(m_samplers[sid], cl);   // this draw's g_UISmp
            lastSampler = ecmd.samplerId;
        }
        // b0 root constants: [0]=materialID (0 plain / 1 SDF text), [1]=effectIndex,
        // [2]=vertexOffset (the VS adds it; we draw from StartVertexLocation 0).
        if (ecmd.materialID != lastMat)
        {
            gfx.SetGraphicsRootConstant(RHI::Slots::Gfx::RootConstants, ecmd.materialID,
                                        /*offsetWords*/0, cl);
            lastMat = ecmd.materialID;
        }
        if (ecmd.effectIndex != lastEff)
        {
            gfx.SetGraphicsRootConstant(RHI::Slots::Gfx::RootConstants, ecmd.effectIndex,
                                        /*offsetWords*/1, cl);
            lastEff = ecmd.effectIndex;
        }
        // Always set the per-draw vertex offset (it changes every command).
        gfx.SetGraphicsRootConstant(RHI::Slots::Gfx::RootConstants, ecmd.vertexOffset,
                                    /*offsetWords*/2, cl);
        // StartVertexLocation = 0: the VS applies g_vertexOffset itself, so we
        // never depend on SV_VertexID including StartVertexLocation.
        gfx.DrawInstanced(ecmd.vertexCount, 1, 0, 0, cl);
        if (uiPassLog) {
            // Log the first 4 vertex positions of this cmd. Read the source drawlist, not
            // m_vbMapped: that is a write-combined UPLOAD mapping where reads bypass the cache
            // (~80x slower), and this gate fires on any frame the UI command count changes,
            // shipping builds included.
            const uint32_t showCount = std::min<uint32_t>(4u, ecmd.vertexCount);
            auto srcVert = [&](uint32_t k) -> UI::UIVertex {
                const uint32_t i = ecmd.srcIndexOffset + k;
                if (i >= srcIdx.size()) return {};
                const uint16_t vi = srcIdx[i];
                return (vi < srcVerts.size()) ? srcVerts[vi] : UI::UIVertex{};
            };
            char vertBuf[256] = {};
            int off = 0;
            for (uint32_t k = 0; k < showCount; ++k) {
                const int n = std::snprintf(vertBuf + off, sizeof(vertBuf) - off,
                                             " v%u=(%.0f,%.0f)", k,
                                             srcVert(k).pos[0], srcVert(k).pos[1]);
                if (n <= 0 || off + n >= (int)sizeof(vertBuf)) break;
                off += n;
            }
            LOG_INFO("%s.cmd[%d]: DRAW vOff=%u vCount=%u tex=%llu scissor=(%u,%u,%u,%u)%s",
                      m_logName, cmdIdx, ecmd.vertexOffset, ecmd.vertexCount,
                      static_cast<unsigned long long>(ecmd.texSrv),
                      l, t, r, b, vertBuf);
        }
        ++cmdIdx;
    }

    // ---- Transition target back to SR for the composite / ImGui ----
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
    return true;
}

void UIPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
}
