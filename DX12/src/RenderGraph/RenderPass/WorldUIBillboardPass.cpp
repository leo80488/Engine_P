#include "RenderGraph/RenderPass/WorldUIBillboardPass.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/RootSignatureLayout.h"
#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"
#include "UI/WorldSpaceUI.h"
#include "UI/UIDrawList.h"   // Vec2 (used by Font measure return)
#include "UI/Font.h"
#include "UI/FontLibrary.h"
#include "System/Log.h"

#include <algorithm>
#include <cstring>
#include <vector>

using namespace DirectX;

// Graphics root signature slots this pass uses:
//   b1 space0 - its own CB (un-jittered viewProj + clock), in the PerView slot
//   b2 space0 - the text effects table
//   t2 space0 - the vertex ByteAddressBuffer (the generic t2 slot, hence the local name)
//   t0 space2 - the bindless Texture2D[] table (font atlas pages, image SRVs)
// The two constants below are ABSOLUTE graphics root indices (for
// BindDescriptorTableGpuHandle), from RHI::Slots::Gfx (Graphics/RootSignatureLayout.h).
static constexpr auto kRootSlot_VertexSRV     = RHI::Slots::Gfx::SRV_t2;      // t2 space0
static constexpr auto kRootSlot_BindlessTex   = RHI::Slots::Gfx::BindlessTex;  // t0 space2 (engine-shared)
// ABSOLUTE root index for the 4-DWORD root-constant block (SetGraphicsRootConstant).
static constexpr auto kRootConstantsSlot      = RHI::Slots::Gfx::RootConstants;
// GROUP-RELATIVE indices - BindConstantBuffer adds the group base
// (CB -> +Gfx::CBVBase); an absolute root index binds NOTHING and logs nothing (see
// RootSignatureLayout.h). WorldUI.ps and WorldTextFX sample with the fixed
// SMP_LINEAR_CLAMP heap slot, so there is no sampler to bind.
static constexpr auto kCBRel_PerView          = RHI::Slots::GfxRel::CB_b1;      // -> b1 space0
static constexpr auto kCBRel_Effects          = RHI::Slots::GfxRel::CB_b2;      // -> b2 space0

static_assert(UI::WorldSpaceUIComponent::kViewCount == RHI::kMaxViews - 1,
              "WorldSpaceUIComponent keeps one fade / scale / cull per secondary view");

void WorldUIBillboardPass::EnsureViewRing(uint32_t viewSlot)
{
    if (!m_gfx || viewSlot == 0 || viewSlot >= RHI::kMaxViews || m_vbMapped[0][viewSlot]) return;
    RHI::GPUBufferDesc bd{};
    bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(WorldUIVertex);
    bd.usage      = RHI::Usage::UPLOAD;
    bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
    bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
    for (uint32_t i = 0; i < kFrameCount; ++i)
    {
        if (m_gfx->CreateBuffer(bd, m_vertexBuffer[i][viewSlot]))
            m_vbMapped[i][viewSlot] = m_gfx->MapBuffer(m_vertexBuffer[i][viewSlot]);
        else
            LOG_ERROR("WorldUIBillboardPass: view %u VB[%u] creation failed", viewSlot, i);
    }
}

void WorldUIBillboardPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    m_shaderLib.Init(gfx, "shaders/");
    m_shaderLib.Register(ShaderID::WorldUI_VS, RHI::ShaderStage::VS, "WorldUI.vs.hlsl");
    m_shaderLib.Register(ShaderID::WorldUI_PS, RHI::ShaderStage::PS, "WorldUI.ps.hlsl");
    m_psoCache.Init(gfx, m_shaderLib);

    // Vertex buffer - flat-expanded (no IA index buffer; the engine root signature
    // exposes none), rebuilt every frame; one per frame in flight so frame N+1's CPU
    // writes cannot race frame N's GPU reads.
    {
        RHI::GPUBufferDesc bd{};
        bd.size       = static_cast<uint64_t>(kMaxVertices) * sizeof(WorldUIVertex);
        bd.usage      = RHI::Usage::UPLOAD;
        bd.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        bd.miscFlags = RHI::ResourceMiscFlag::BUFFER_RAW;
        for (uint32_t i = 0; i < kFrameCount; ++i)
        {
            if (gfx.CreateBuffer(bd, m_vertexBuffer[i][0]))
                m_vbMapped[i][0] = gfx.MapBuffer(m_vertexBuffer[i][0]);
            else
                LOG_ERROR("WorldUIBillboardPass: VB[%u] creation failed", i);
        }
    }

    // The pass's own b1 CB (un-jittered viewProj + clock), bound in the PerView slot in
    // place of the engine's PerViewCB, as UIPass does.
    m_cb.Create(gfx, "WorldUIBillboard.CB");
    // Text effects table at b2 (outline/glow/custom-shader params).
    m_effectsCB.Create(gfx, "WorldUIBillboard.EffectsCB");

    if (m_psoCache.GetOrCreate(BuildPSODesc()))
        LOG_SUCCESS("WorldUIBillboardPass: initialized");
    else
        LOG_ERROR("WorldUIBillboardPass: PSO creation failed");
}

PSODesc WorldUIBillboardPass::BuildPSODesc() const
{
    PSODesc desc;
    desc.vsID        = ShaderID::WorldUI_VS;
    desc.psID        = ShaderID::WorldUI_PS;
    desc.inputLayout = InputLayoutType::None;
    desc.topology    = RHI::PrimitiveTopology::TRIANGLELIST;

    desc.rs.cullMode         = RHI::CullMode::NONE;
    desc.rs.depthClipEnable = false;

    // No depth test: every entity draws as DepthMode::Always. DepthMode::Test is not
    // honoured here (it needs the scene depth as a separate PSO variant).
    desc.dss.depthEnable     = false;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ZERO;

    auto& rt = desc.bs.renderTarget[0];
    rt.blendEnable             = true;
    rt.srcBlend                = RHI::Blend::SRC_ALPHA;
    rt.destBlend               = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOp                 = RHI::BlendOp::ADD;
    rt.srcBlendAlpha          = RHI::Blend::ONE;
    rt.destBlendAlpha         = RHI::Blend::INV_SRC_ALPHA;
    rt.blendOpAlpha           = RHI::BlendOp::ADD;
    rt.renderTargetWriteMask = RHI::ColorWrite::ENABLE_ALL;

    desc.rtvFormats[0] = m_targetFormat;   // the composition buffer's live format (see m_targetFormat)
    desc.rtvCount      = 1;
    desc.dsvFormat     = RHI::Format::UNKNOWN;
    return desc;
}

// Pivot-anchored billboard quad: 6 vertices (2 triangles) in CCW order; the PSO culls
// nothing, so the facing does not matter.
void WorldUIBillboardPass::EmitQuad(std::vector<WorldUIVertex>& out,
                                    const XMFLOAT3& worldCenter,
                                    const XMFLOAT3& cameraRightWS,
                                    const XMFLOAT3& cameraUpWS,
                                    float halfW, float halfH,
                                    float u0, float v0, float u1, float v1,
                                    uint32_t col, uint32_t texIdx)
{
    const XMVECTOR center = XMLoadFloat3(&worldCenter);
    const XMVECTOR right  = XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW);
    const XMVECTOR up     = XMVectorScale(XMLoadFloat3(&cameraUpWS),    halfH);

    XMFLOAT3 p[4];
    XMStoreFloat3(&p[0], XMVectorSubtract(XMVectorAdd(center, up), right));      // top-L
    XMStoreFloat3(&p[1], XMVectorAdd(XMVectorAdd(center, up), right));            // top-R
    XMStoreFloat3(&p[2], XMVectorAdd(XMVectorSubtract(center, up), right));       // bot-R
    XMStoreFloat3(&p[3], XMVectorSubtract(XMVectorSubtract(center, up), right));  // bot-L

    auto pushV = [&](const XMFLOAT3& xyz, float u, float v) {
        WorldUIVertex vv{};
        vv.pos[0] = xyz.x; vv.pos[1] = xyz.y; vv.pos[2] = xyz.z;
        vv.uv[0]  = u;     vv.uv[1]  = v;
        vv.col    = col;
        vv.texIdx = texIdx;
        out.push_back(vv);
    };

    pushV(p[0], u0, v0);
    pushV(p[1], u1, v0);
    pushV(p[2], u1, v1);
    pushV(p[0], u0, v0);
    pushV(p[2], u1, v1);
    pushV(p[3], u0, v1);
}

// Pack RGBA float [0..1] x alpha into uint32 0xAABBGGRR.
static uint32_t PackColor(const XMFLOAT4& c, float alpha)
{
    auto cv = [](float f) {
        return static_cast<uint8_t>(std::clamp(f, 0.f, 1.f) * 255.f);
    };
    return cv(c.x) | (uint32_t(cv(c.y)) << 8)
                   | (uint32_t(cv(c.z)) << 16)
                   | (uint32_t(cv(c.w * alpha)) << 24);
}

// World-space half size (@p halfW / @p halfH) of one billboard from the WorldSpaceUI
// component's settings.
static void WorldUIQuadHalfSize(const UI::WorldSpaceUIComponent& ws,
                                  float& halfW, float& halfH)
{
    halfW = ws.baseSize.x * 0.5f * ws.computedScale;
    halfH = ws.baseSize.y * 0.5f * ws.computedScale;
}

// Anchor the billboard quad: shift @p worldPos so the WorldSpaceUI `pivot` lands on
// the entity's world position.
//
// The vertical pivot uses WORLD UP (0,1,0), NOT camera up: otherwise the quad drifts
// as the camera pitches - a HP bar with pivot (0.5, 1.0) bottom-anchored to a head
// would slide away from the entity whenever the camera tilts.
//
// The horizontal pivot uses CAMERA RIGHT, so pivot.x = 0 means "the quad's left edge"
// on screen, whichever world direction is left.
static XMFLOAT3 ApplyPivot(const XMFLOAT3& worldPos,
                           const XMFLOAT3& cameraRightWS,
                           const XMFLOAT3& /* cameraUpWS - unused, world up is used (see above) */,
                           float halfW, float halfH,
                           const XMFLOAT2& pivot)
{
    XMVECTOR p     = XMLoadFloat3(&worldPos);
    XMVECTOR right = XMLoadFloat3(&cameraRightWS);
    const XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    p = XMVectorAdd(p, XMVectorScale(right,    (1.0f - 2.0f * pivot.x) * halfW));
    p = XMVectorAdd(p, XMVectorScale(worldUp,  (2.0f * pivot.y - 1.0f) * halfH));
    XMFLOAT3 out;
    XMStoreFloat3(&out, p);
    return out;
}

// Stable per-glyph [0,1) hash -> jitter phase (mirrors Font.cpp's helper).
static float WuGlyphHash01(int glyphIndex)
{
    uint32_t h = static_cast<uint32_t>(glyphIndex) * 374761393u + 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 16777216.0f;
}

// ---- BUILD half: runs on the GAME side ----
// Pure CPU: a read-only function of (World, camera basis, canvas size, time) that
// produces one vertex array plus the per-shader buckets. Nothing here touches a
// device slot or a mapped buffer - the memcpy into m_vbMapped stays in Execute, on
// the render slot. It must stay off the graph's recording workers (Phase B runs pass
// Execute bodies through ParallelFor): a six-pool World walk and font lookups.
//
// WHERE IT IS CALLED FROM IS LOAD-BEARING: it must run AFTER
// WorldSpaceUISystem::Tick, because it consumes that tick's per-frame output
// (computedAlpha, computedScale, isCulled, the integrated damage-number offset, the
// resolved custom-shader slot) and because Tick called EnsureGlyphs for the text it
// lays out. Any earlier, all of that is one frame stale AND every newly baked glyph
// drops for a frame - a pixel change, not code motion. That is why
// Renderer::ExtractWorldUI runs right after paintUI().
void WorldUIBillboardPass::BuildLane(World& world, uint32_t viewSlot,
                                     const DirectX::XMFLOAT4X4& viewProjMatrix,
                                     const DirectX::XMFLOAT4X4& viewMatrix,
                                     uint32_t canvasW, uint32_t canvasH,
                                     float timeSec)
{
    if (viewSlot >= RHI::kMaxViews) return;
    Lane& lane = m_lanes[viewSlot];
    // Carried on the lane, not re-read at draw time: the render half draws the frame the
    // game half extracted, so the matrix and the clock must be that frame's.
    lane.viewProj = viewProjMatrix;
    lane.timeSec  = timeSec;

    lane.verts.clear();
    lane.buckets.clear();
    lane.effects.clear();
    lane.bucketVertCount = 0;
    if (viewSlot == 0) m_lastVertexCount = 0;

    if (!enabled || !m_gfx) return;
    if (canvasW == 0 || canvasH == 0) return;
    EnsureViewRing(viewSlot);

    auto& gfx = *m_gfx;
    (void)gfx;

    // ---- Camera basis vectors (world space) ----
    // For a row-major DX view matrix the world-space camera axes are the COLUMNS of the
    // rotation block (derived in WorldSpaceUISystem.cpp).
    const float* vm = reinterpret_cast<const float*>(&viewMatrix);
    XMFLOAT3 cameraRightWS  { vm[0], vm[4], vm[8] };
    XMFLOAT3 cameraUpWS     { vm[1], vm[5], vm[9] };
    XMFLOAT3 cameraForwardWS{ vm[2], vm[6], vm[10] };

    // ---- Build the vertices from the ECS pools ----
    auto* wsPool = world.GetPool<UI::WorldSpaceUIComponent>();
    if (!wsPool || wsPool->Size() == 0) return;

    // The lane's vertex array, a MEMBER: the build runs on the game thread and the draw
    // on the render thread, and a thread_local would give each its own empty copy.
    auto& verts = lane.verts;
    verts.reserve(64);

    // Per-frame text effects table (slot 0 = no-op) - the same 96 B entries as UIPass, so
    // outline / glow / custom params share one convention. Built here, uploaded to b2 in
    // Execute.
    auto& effects = lane.effects;
    effects.emplace_back();
    auto addEffect = [&](const UI::GpuTextEffect& ge) -> uint32_t
    {
        for (size_t i = 1; i < effects.size(); ++i)
            if (std::memcmp(&effects[i], &ge, sizeof(ge)) == 0)
                return static_cast<uint32_t>(i);
        if (effects.size() >= kMaxEffectSlots) return 0u; // full -> plain SDF
        effects.push_back(ge);
        return static_cast<uint32_t>(effects.size() - 1);
    };

    // Custom-shader text quads draw in separate ranges (one PSO switch per shader).
    // Everything else - bars, images, plain / effect text, shadow copies - stays in the
    // single default draw.
    auto& customBuckets = lane.buckets;
    uint32_t& bucketVertCount = lane.bucketVertCount;
    auto bucketFor = [&](uint32_t slot) -> std::vector<WorldUIVertex>&
    {
        for (auto& b : customBuckets)
            if (b.first == slot) return b.second;
        customBuckets.emplace_back(slot, std::vector<WorldUIVertex>{});
        return customBuckets.back().second;
    };
    auto vertBudgetOK = [&](size_t add)
    { return verts.size() + bucketVertCount + add <= kMaxVertices; };

    // Single bindless batch - every quad writes its own bindless texIdx. Bar / border use
    // kInvalidTexIdx (the PS short-circuits); text glyphs their atlas PAGE's bindless
    // index (multi-font / multi-page, resolved per glyph below); images the
    // component-supplied bindless index. ONE bind of the bindless table, ONE
    // DrawInstanced. The high bit flags SDF font glyphs in the PS (the page's red channel
    // is a signed distance field); image quads leave it clear.
    //
    // Only bake-free font lookups here (FindGlyph / MeasureResident):
    // WorldSpaceUISystem::Tick ensured residency just before, on the game half. A glyph
    // Tick asked for this frame is RESERVED - its UVs, size and bearing are final - and
    // FontLibrary::FlushPendingUploads renders and streams its pixels before the graph
    // runs, so the quads laid out here are right.
    constexpr uint32_t kSdfTexFlag = 0x80000000u;

    const auto& wsEnts = wsPool->Entities();
    auto&       wsData = wsPool->Data();

    // Hoist every pool the loop probes: GetComponent<T> IS a GetPool<T> (AccessGuard
    // check + type_index hash + map find) - six per entity otherwise, with
    // DamageNumberComponent probed twice.
    auto* pXform  = world.GetPool<GlobalTransform>();
    auto* pDamage = world.GetPool<UI::DamageNumberComponent>();
    auto* pBar    = world.GetPool<UI::WorldUIBarComponent>();
    auto* pText   = world.GetPool<UI::WorldUITextComponent>();
    auto* pImage  = world.GetPool<UI::WorldUIImageComponent>();

    // A secondary view's lane: the component with that view's fade / scale / cull in its outputs.
    UI::WorldSpaceUIComponent viewWs;
    for (size_t i = 0; i < wsData.size(); ++i)
    {
        const UI::WorldSpaceUIComponent* wsp = &wsData[i];
        if (viewSlot != 0)
        {
            const uint32_t k = viewSlot - 1;
            viewWs = wsData[i];
            viewWs.isCulled      = (viewWs.viewCulled >> k) & 1u;
            viewWs.computedAlpha = viewWs.viewAlpha[k];
            viewWs.computedScale = viewWs.viewScale[k];
            wsp = &viewWs;
        }
        const UI::WorldSpaceUIComponent& ws = *wsp;
        if (ws.isCulled || ws.computedAlpha <= 0.001f) continue;

        const Entity e = wsEnts[i];
        const GlobalTransform* gt = pXform ? pXform->Get(e) : nullptr;
        if (!gt) continue;

        XMFLOAT3 worldPos{
            gt->matrix.m[3][0],
            gt->matrix.m[3][1],
            gt->matrix.m[3][2],
        };
        UI::DamageNumberComponent* dnComp = pDamage ? pDamage->Get(e) : nullptr;
        if (auto* dn = dnComp)
        {
            worldPos.x += dn->currentOffset.x;
            worldPos.y += dn->currentOffset.y;
            worldPos.z += dn->currentOffset.z;
        }
        // Camera-relative anchor offset:
        //   .x -> cameraRightWS   (positive = the camera's right)
        //   .y -> world up        (vertical stays world Y)
        //   .z -> cameraForwardWS (positive = away from the camera)
        // So "0.3 to the right" stays 0.3 to the camera's right even when the camera orbits
        // behind the entity.
        const auto& sso = ws.screenSpaceOffset;
        worldPos.x += cameraRightWS.x * sso.x + cameraForwardWS.x * sso.z;
        worldPos.y += cameraRightWS.y * sso.x + cameraForwardWS.y * sso.z + sso.y;
        worldPos.z += cameraRightWS.z * sso.x + cameraForwardWS.z * sso.z;

        float halfW, halfH;
        WorldUIQuadHalfSize(ws, halfW, halfH);
        const XMFLOAT3 quadCenter = ApplyPivot(worldPos, cameraRightWS, cameraUpWS,
                                                halfW, halfH, ws.pivot);

        // ---- Bar / border (texIdx = sentinel) ----
        if (auto* bar = pBar ? pBar->Get(e) : nullptr)
        {
            if (bar->visible && vertBudgetOK(36))
            {
                const uint32_t bg = PackColor(bar->backgroundColor, ws.computedAlpha);
                EmitQuad(verts, quadCenter, cameraRightWS, cameraUpWS,
                          halfW, halfH, 0,0,0,0, bg, kInvalidTexIdx);

                const float v = std::clamp(bar->value, 0.f, 1.f);
                if (v > 0.001f)
                {
                    const float fillHalfW = halfW * v;
                    XMVECTOR cv = XMLoadFloat3(&quadCenter);
                    cv = XMVectorSubtract(cv,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - fillHalfW));
                    XMFLOAT3 fillCenter; XMStoreFloat3(&fillCenter, cv);
                    const uint32_t fc = PackColor(bar->fillColor, ws.computedAlpha);
                    EmitQuad(verts, fillCenter, cameraRightWS, cameraUpWS,
                              fillHalfW, halfH, 0,0,0,0, fc, kInvalidTexIdx);
                }

                if (bar->borderThick > 0.f)
                {
                    const float t = std::min(bar->borderThick * 0.5f, std::min(halfW, halfH) * 0.5f);
                    const uint32_t bd = PackColor(bar->borderColor, ws.computedAlpha);

                    XMVECTOR center = XMLoadFloat3(&quadCenter);
                    XMVECTOR top = XMVectorAdd(center,
                        XMVectorScale(XMLoadFloat3(&cameraUpWS), halfH - t));
                    XMFLOAT3 c; XMStoreFloat3(&c, top);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, halfW, t, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR bot = XMVectorSubtract(center,
                        XMVectorScale(XMLoadFloat3(&cameraUpWS), halfH - t));
                    XMStoreFloat3(&c, bot);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, halfW, t, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR left = XMVectorSubtract(center,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - t));
                    XMStoreFloat3(&c, left);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, t, halfH, 0,0,0,0, bd, kInvalidTexIdx);

                    XMVECTOR right = XMVectorAdd(center,
                        XMVectorScale(XMLoadFloat3(&cameraRightWS), halfW - t));
                    XMStoreFloat3(&c, right);
                    EmitQuad(verts, c, cameraRightWS, cameraUpWS, t, halfH, 0,0,0,0, bd, kInvalidTexIdx);
                }
            }
        }

        // ---- Text (per-glyph page bindless + effects + custom shader) ----
        // Canvas UIText parity: free-combination outline / glow (effects table, b2) + CPU
        // shadow / jitter passes + an optional custom pixel shader (customShaderSlotRt,
        // resolved by WorldSpaceUISystem::Tick; its quads go to a per-shader bucket drawn
        // with its own PSO).
        {
            const std::string* str      = nullptr;
            const std::string* fontPath = nullptr;
            const XMFLOAT4*    col      = nullptr;
            float              tScale   = 1.f;
            bool fxO = false, fxG = false, fxS = false, fxJ = false;
            const XMFLOAT4* oc = nullptr; float ow = 2.f;
            const XMFLOAT4* gc = nullptr; float gw2 = 4.f;
            const XMFLOAT4* sc = nullptr; float sox = 2.f, soy = 2.f;
            float ja = 2.f, jf = 12.f;
            uint32_t customSlot = 0;
            const XMFLOAT4* cp0 = nullptr; const XMFLOAT4* cp1 = nullptr;

            if (auto* tc = pText ? pText->Get(e) : nullptr)
            {
                if (tc->visible && !tc->text.empty()) {
                    str      = &tc->text;
                    fontPath = &tc->fontPath;
                    col      = &tc->color;
                    tScale   = tc->scale;
                    fxO = tc->fxOutline; fxG = tc->fxGlow;
                    fxS = tc->fxShadow;  fxJ = tc->fxJitter;
                    oc = &tc->outlineColor; ow  = tc->outlineWidth;
                    gc = &tc->glowColor;    gw2 = tc->glowWidth;
                    sc = &tc->shadowColor;  sox = tc->shadowOffsetX; soy = tc->shadowOffsetY;
                    ja = tc->jitterAmplitude; jf = tc->jitterFrequency;
                    customSlot = tc->customShaderSlotRt;
                    cp0 = &tc->customParam0; cp1 = &tc->customParam1;
                }
            }
            else if (auto* dn = dnComp)   // already resolved above
            {
                if (!dn->text.empty()) {
                    str      = &dn->text;
                    fontPath = &dn->fontPath;
                    col      = &dn->color;
                    fxO = dn->fxOutline; fxG = dn->fxGlow;
                    fxS = dn->fxShadow;  fxJ = dn->fxJitter;
                    oc = &dn->outlineColor; ow  = dn->outlineWidth;
                    gc = &dn->glowColor;    gw2 = dn->glowWidth;
                    sc = &dn->shadowColor;  sox = dn->shadowOffsetX; soy = dn->shadowOffsetY;
                    ja = dn->jitterAmplitude; jf = dn->jitterFrequency;
                    customSlot = dn->customShaderSlotRt;
                    cp0 = &dn->customParam0; cp1 = &dn->customParam1;
                }
            }
            if (str && col)
            {
                // Bake-free resolve: Tick loaded the font this frame; a font
                // that never loaded falls back to the default.
                UI::Font* fontP = fontPath ? UI::FontLibrary::Get().Find(*fontPath) : nullptr;
                if (!fontP) fontP = &UI::FontLibrary::Get().Default();
                const UI::Font& font = *fontP;
                if (!font.IsReady()) { /* no font at all - skip text */ }
                else
                {
                const float basePx = font.Metrics().pixelSize;
                const float pixelLineH = std::max(1.0f, font.Metrics().lineHeight);
                const float worldPerPixel = (halfH * 2.0f) * tScale / pixelLineH;
                const UI::Vec2 sizePx = font.MeasureResident(str->c_str(), basePx);
                const float worldTextWidth = sizePx.x * worldPerPixel;
                // World units per Canvas "effect pixel" (kTextRefPx parity - shadow offsets /
                // jitter amplitudes keep their meaning).
                const float fxPxWorld = worldPerPixel * (basePx / UI::kTextRefPx);

                const XMVECTOR rightV = XMLoadFloat3(&cameraRightWS);
                const XMVECTOR upV    = XMLoadFloat3(&cameraUpWS);

                XMVECTOR penStart = XMLoadFloat3(&quadCenter);
                penStart = XMVectorSubtract(penStart,
                    XMVectorScale(rightV, worldTextWidth * 0.5f));
                const float asc = font.Metrics().ascender;
                penStart = XMVectorSubtract(penStart,
                    XMVectorScale(upV, (asc * worldPerPixel) - halfH));

                // Effects-table entry (outline/glow widths + custom params +
                // projected run rect). Same width compensation as
                // Font::DrawText: px * (basePx / kTextRefPx) / sdfPixelRange.
                uint32_t effIdx = 0;
                if (fxO || fxG || customSlot != 0)
                {
                    const float range = font.Metrics().sdfPixelRange > 0.f
                                      ? font.Metrics().sdfPixelRange : 8.f;
                    const float comp = basePx / UI::kTextRefPx;
                    UI::GpuTextEffect ge{};
                    auto toF4 = [](const XMFLOAT4& c, float out[4])
                    { out[0]=c.x; out[1]=c.y; out[2]=c.z; out[3]=c.w; };
                    if (fxO && oc)
                    {
                        toF4(*oc, ge.outlineColor);
                        ge.outlineWidthN = std::min(0.49f, std::max(0.f, ow * comp / range));
                    }
                    else { ge.outlineColor[3] = 0.f; }
                    if (fxG && gc)
                    {
                        toF4(*gc, ge.glowColor);
                        ge.glowWidthN = std::min(0.5f, std::max(0.f, gw2 * comp / range));
                    }
                    else { ge.glowColor[3] = 0.f; }
                    if (cp0) { toF4(*cp0, ge.customA); }
                    if (cp1) { toF4(*cp1, ge.customB); }
                    // Whole-run rect in SCREEN px for TextRunT gradients:
                    // billboards face the camera, so projecting the run's
                    // top-left / bottom-right corners gives a stable rect.
                    if (customSlot != 0)
                    {
                        const XMMATRIX vpM = XMLoadFloat4x4(&viewProjMatrix);
                        auto projectPx = [&](XMVECTOR wsPos, float& px, float& py) -> bool
                        {
                            const XMVECTOR clip =
                                XMVector4Transform(XMVectorSetW(wsPos, 1.f), vpM);
                            const float w = XMVectorGetW(clip);
                            if (w <= 1e-4f) return false;
                            px = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * canvasW;
                            py = (0.5f - XMVectorGetY(clip) / w * 0.5f) * canvasH;
                            return true;
                        };
                        const XMVECTOR a = XMVectorAdd(penStart,
                            XMVectorScale(upV, asc * worldPerPixel));   // left-top
                        const XMVECTOR b = XMVectorSubtract(
                            XMVectorAdd(a, XMVectorScale(rightV, worldTextWidth)),
                            XMVectorScale(upV, sizePx.y * worldPerPixel)); // right-bottom
                        float ax, ay, bx, by;
                        if (projectPx(a, ax, ay) && projectPx(b, bx, by))
                        {
                            ge.runRect[0] = std::min(ax, bx);
                            ge.runRect[1] = std::min(ay, by);
                            ge.runRect[2] = 1.f / std::max(std::abs(bx - ax), 1e-3f);
                            ge.runRect[3] = 1.f / std::max(std::abs(by - ay), 1e-3f);
                        }
                    }
                    effIdx = addEffect(ge);
                }

                // One glyph pass: offsets in world units along camera-right /
                // world-down, tint, effect slot, destination vertex stream.
                auto emitTextPass = [&](float offR, float offD, uint32_t tint,
                                        uint32_t eff, std::vector<WorldUIVertex>& dst)
                {
                    const bool toBucket = (&dst != &verts);
                    XMVECTOR penWS = penStart;
                    penWS = XMVectorAdd(penWS, XMVectorScale(rightV, offR));
                    penWS = XMVectorSubtract(penWS, XMVectorScale(upV, offD));
                    int glyphIndex = 0;
                    const char* p = str->c_str();
                    while (*p)
                    {
                        const uint32_t cp = UI::DecodeUtf8(p);
                        if (cp == '\n') continue;
                        const UI::Font::ResolvedGlyph rg = font.FindGlyph(cp);
                        if (!rg.glyph) continue;
                        const UI::FontGlyph& g = *rg.glyph;
                        // Fallback glyphs are in the OWNER's baked pixel units.
                        const float unit = basePx / rg.owner->Metrics().pixelSize;
                        const float wpp  = worldPerPixel * unit;

                        if (g.size.x > 0.f && g.size.y > 0.f)
                        {
                            const uint32_t pageIdx = rg.owner->PageBindlessIndex(g.page);
                            if (pageIdx == kInvalidTexIdx) { ++glyphIndex; continue; }
                            float jx = 0.f, jy = 0.f; // screen-style px: +y down
                            if (fxJ)
                            {
                                const float ph  = WuGlyphHash01(glyphIndex) * 6.2831853f;
                                const float amp = ja * fxPxWorld;
                                jx = amp * std::sin(timeSec * jf + ph);
                                jy = amp * std::cos(timeSec * jf * 1.13f + ph * 1.7f);
                            }
                            const float gw = g.size.x * wpp * 0.5f;
                            const float gh = g.size.y * wpp * 0.5f;
                            XMVECTOR center = penWS;
                            center = XMVectorAdd(center, XMVectorScale(rightV,
                                (g.bearing.x + g.size.x * 0.5f) * wpp + jx));
                            center = XMVectorAdd(center, XMVectorScale(upV,
                                (g.bearing.y - g.size.y * 0.5f) * wpp - jy));
                            XMFLOAT3 cc3; XMStoreFloat3(&cc3, center);
                            if (!vertBudgetOK(6)) break;
                            EmitQuad(dst, cc3, cameraRightWS, cameraUpWS, gw, gh,
                                     g.uv0.x, g.uv0.y, g.uv1.x, g.uv1.y, tint,
                                     pageIdx | (eff << kEffectShift) | kSdfTexFlag);
                            if (toBucket) bucketVertCount += 6;
                        }
                        penWS = XMVectorAdd(penWS, XMVectorScale(rightV, g.advance * wpp));
                        ++glyphIndex;
                    }
                };

                // Shadow copy first (built-in shader, effect 0) - it stays behind even when the main
                // pass uses a custom shader.
                if (fxS && sc)
                    emitTextPass(sox * fxPxWorld, soy * fxPxWorld,
                                 PackColor(*sc, ws.computedAlpha), 0u, verts);

                const uint32_t cc = PackColor(*col, ws.computedAlpha);
                emitTextPass(0.f, 0.f, cc, effIdx,
                             customSlot != 0 ? bucketFor(customSlot) : verts);
                }
            }
        }

        // ---- Image (texIdx = component-supplied bindless slot) ----
        if (auto* img = pImage ? pImage->Get(e) : nullptr)
        {
            if (img->visible && img->bindlessIndex != ~0u && vertBudgetOK(6))
            {
                const uint32_t cc = PackColor(img->tint, ws.computedAlpha);
                EmitQuad(verts, quadCenter, cameraRightWS, cameraUpWS,
                          halfW, halfH,
                          img->uv0.x, img->uv0.y, img->uv1.x, img->uv1.y,
                          cc, img->bindlessIndex);
            }
        }
    }

    // Published even when the build produced nothing (0, not the previous frame's value):
    // the only observable number of the vertex build (FrameStats::worldUIVerts).
    m_lastVertexCount = static_cast<uint32_t>(verts.size()) + bucketVertCount;

}

// ---- DRAW half: runs on the RENDER side ----
// Reads only the lane built above; the locals below are references onto the lane
// members.
void WorldUIBillboardPass::Execute(RHI::CommandList cl,
                                    const RHI::Texture* target,
                                    RHI::ResourceState entryState,
                                    uint32_t canvasW, uint32_t canvasH)
{
    if (!enabled || !m_gfx || !target || !target->IsValid()) return;
    if (canvasW == 0 || canvasH == 0) return;
    const uint32_t viewSlot = RHI::CurrentRecordingView().slot;
    if (viewSlot >= RHI::kMaxViews) return;
    const Lane& lane = m_lanes[viewSlot];
    if (lane.verts.empty() && lane.bucketVertCount == 0) return;

    auto& gfx = *m_gfx;
    const uint32_t frameSlot = gfx.GetFrameIndex();
    if (!m_vbMapped[frameSlot][viewSlot]) return;
    const PSODesc psoDesc = BuildPSODesc();
    const RHI::PipelineState* pso = m_psoCache.GetOrCreate(psoDesc);
    if (!pso || !pso->IsValid()) return;

    auto& verts         = lane.verts;
    auto& customBuckets = lane.buckets;
    auto& effects       = lane.effects;
    const uint32_t bucketVertCount = lane.bucketVertCount;

    // ---- Upload: default range first, then one range per custom shader ----
    struct DrawRange { uint32_t slot, start, count; };
    std::vector<DrawRange> customRanges;
    {
        uint8_t* dstBytes = static_cast<uint8_t*>(m_vbMapped[frameSlot][viewSlot]);
        uint32_t cursor = 0;
        auto append = [&](const std::vector<WorldUIVertex>& v) -> uint32_t
        {
            const uint32_t start = cursor;
            std::memcpy(dstBytes + static_cast<size_t>(cursor) * sizeof(WorldUIVertex),
                        v.data(), v.size() * sizeof(WorldUIVertex));
            cursor += static_cast<uint32_t>(v.size());
            return start;
        };
        if (!verts.empty()) append(verts);
        for (const auto& b : customBuckets)
            if (!b.second.empty())
                customRanges.push_back({ b.first, append(b.second),
                                         static_cast<uint32_t>(b.second.size()) });
    }

    // ---- Transition target -> RT ----
    if (entryState != RHI::ResourceState::RENDERTARGET)
    {
        gfx.PushBarrier(RHI::GPUBarrier::Image(
            target, entryState, RHI::ResourceState::RENDERTARGET), cl);
    }

    const RHI::Texture* rtArr[1] = { target };
    gfx.SetRenderTargets(1, rtArr, nullptr, cl);

    RHI::Viewport vp{};
    vp.topLeftX = 0; vp.topLeftY = 0;
    vp.width  = static_cast<float>(canvasW);
    vp.height = static_cast<float>(canvasH);
    vp.minDepth = 0.f; vp.maxDepth = 1.f;
    gfx.SetViewport(vp, cl);
    gfx.SetScissorRect(0, 0, canvasW, canvasH, cl);

    gfx.BindDescriptorHeaps(cl);
    gfx.BindPipelineState(*pso, cl);
    gfx.SetPrimitiveTopology(RHI::PrimitiveTopology::TRIANGLELIST, cl);

    // Upload the pass's own un-jittered viewProj into m_cb, so the UI does not dance with
    // the TAA jitter (the engine's PerViewCB holds the jittered one).
    if (auto* slot = m_cb.Current(gfx))
    {
        // The shader reads float4x4 viewProj at b1; engine CB matrices are stored transposed
        // (DirectX row-vector convention, HLSL column-major cbuffer), so transpose on upload.
        XMMATRIX m = XMLoadFloat4x4(&lane.viewProj);
        XMStoreFloat4x4(&slot->viewProj, XMMatrixTranspose(m));
        slot->timeSec = lane.timeSec;
    }
    // Text effects table -> b2. Zero-fill the unused entries.
    if (auto* fxTable = m_effectsCB.Current(gfx))
    {
        const size_t n = std::min<size_t>(effects.size(), kMaxEffectSlots);
        for (size_t i = 0; i < n; ++i)               fxTable->fx[i] = effects[i];
        for (size_t i = n; i < kMaxEffectSlots; ++i) fxTable->fx[i] = UI::GpuTextEffect{};
    }
    gfx.BindConstantBuffer(kCBRel_PerView, m_cb.CurrentBuffer(gfx), cl);        // b1 space0
    gfx.BindConstantBuffer(kCBRel_Effects, m_effectsCB.CurrentBuffer(gfx), cl); // b2 space0

    const uint64_t vbHandle = gfx.GetBufferSRVGpuHandle(m_vertexBuffer[frameSlot][viewSlot]);
    if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);

    // The engine's bindless texture table at t0 space2: the font atlas pages + every
    // WorldUIImage SRV in one bind; the per-vertex texIdx selects the descriptor.
    const uint64_t bindlessTbl = gfx.GetBindlessTextureTableGpuHandle();
    if (bindlessTbl)
        gfx.BindDescriptorTableGpuHandle(kRootSlot_BindlessTex, bindlessTbl, cl);

    // b0 word0 = the range's vertex offset (the VS adds it; b0 holds stale constants from
    // earlier passes, so ALWAYS write it before drawing).
    gfx.SetGraphicsRootConstant(kRootConstantsSlot, 0u, /*offsetWords*/0, cl);
    if (!verts.empty())
        gfx.DrawInstanced(static_cast<uint32_t>(verts.size()), 1, 0, 0, cl);

    // Custom-shader text ranges: one PSO switch each. A broken / missing shader falls back
    // to the built-in PSO (PSOCache negative-caches the failure). BindPipelineState
    // re-sets the root signature, which clears every root argument - re-bind them all per
    // range.
    const auto& shaderPaths = UI::TextShaderPaths();
    for (const DrawRange& dr : customRanges)
    {
        const RHI::PipelineState* usePso = pso;
        if (dr.slot >= 1u && dr.slot - 1u < shaderPaths.size())
        {
            // Slot -> dynId cache, per INSTANCE (m_slotDynIds): the registry is append-only, so a
            // cached id stays valid and the string key + mutex are paid once per slot, not per custom
            // text range per frame.
            if (m_slotDynIds.size() < shaderPaths.size())
                m_slotDynIds.resize(shaderPaths.size(), ShaderLibrary::kInvalidDynShaderID);
            uint32_t& cached = m_slotDynIds[dr.slot - 1u];
            if (cached == ShaderLibrary::kInvalidDynShaderID)
                cached = m_shaderLib.RegisterDynamic(
                    shaderPaths[dr.slot - 1u].c_str(), RHI::ShaderStage::PS, "main");
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
        gfx.BindPipelineState(*usePso, cl);
        gfx.BindConstantBuffer(kCBRel_PerView, m_cb.CurrentBuffer(gfx), cl);
        gfx.BindConstantBuffer(kCBRel_Effects, m_effectsCB.CurrentBuffer(gfx), cl);
        if (vbHandle) gfx.BindDescriptorTableGpuHandle(kRootSlot_VertexSRV, vbHandle, cl);
        if (bindlessTbl) gfx.BindDescriptorTableGpuHandle(kRootSlot_BindlessTex, bindlessTbl, cl);
        gfx.SetGraphicsRootConstant(kRootConstantsSlot, dr.start, /*offsetWords*/0, cl);
        gfx.DrawInstanced(dr.count, 1, 0, 0, cl);
    }

    // ---- Transition back to SR ----
    gfx.PushBarrier(RHI::GPUBarrier::Image(
        target,
        RHI::ResourceState::RENDERTARGET,
        RHI::ResourceState::SHADER_RESOURCE), cl);
}

void WorldUIBillboardPass::ReloadShaders()
{
    m_shaderLib.ClearCaches();
    m_psoCache.Clear();
}
