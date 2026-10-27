// PreviewStudioShadowPass.cpp - see the header for why this is not a
// ShadowPass instance and what it borrows.

#include "RenderGraph/RenderPass/PreviewStudioShadowPass.h"
#include "RenderGraph/RenderPass/ShadowCasterShaders.h"   // ShadowCaster hook-PS helpers

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/RenderTypes.h"
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/ShaderLibrary.h"
#include "RenderGraph/RenderContext.h"
#include "System/Check.h"
#include "System/Log.h"

namespace S  = RHI::Slots::Gfx;
namespace SR = RHI::Slots::GfxRel;

void PreviewStudioShadowPass::SetNameSuffix(const char* suffix)
{
    if (!ENGINE_CHECK(suffix && *suffix,
                      "PreviewStudioShadowPass::SetNameSuffix: empty suffix would "
                      "duplicate '%s'", kBaseName))
        return;
    m_name = kBaseName;
    m_name += '.';
    m_name += suffix;
}

void PreviewStudioShadowPass::Setup(RG::RenderGraphBuilder& /*b*/)
{
    // Declares nothing: the only texture this pass touches is its own, not a graph texture.
    // Do not declare the preview depth buffer either: this pass runs first in the preview graph,
    // before GBufferPass, which declares BuiltinTexture::HdrSceneColor (bind + CLEAR). Touching the
    // graph's depth here would move that clear's barrier and sometimes leave last frame's picture
    // as the background.
}

// m_lib is always borrowed, so there is no OwnsShaderLibrary() test here and there must never
// be one. DestroyTexture also releases the per-slice DSVs the device minted lazily (it moves
// entry.sliceDsvs into the deferred bucket).
void PreviewStudioShadowPass::Shutdown(IGraphicsDevice& gfx)
{
    m_psoCache.Shutdown();
    if (m_depthArray.IsValid()) gfx.DestroyTexture(m_depthArray);
    m_srv    = 0;
    m_state  = RHI::ResourceState::DEPTHSTENCIL;
    m_viewCB = nullptr;
    m_lib    = nullptr;   // borrowed; dropped, never destroyed
    m_gfx    = nullptr;
    // One-shot log latches: a rebuilt instance is a new tenancy and its first
    // Init / first draw are worth one line each again.
    m_initLogged = false;
    m_drawLogged = false;
}

void PreviewStudioShadowPass::Init(IGraphicsDevice& gfx)
{
    m_gfx = &gfx;

    // No ShaderLibrary of our own: the owner registered the GBuffer shaders and Inits before this
    // borrower. PSOCache stores a bare ShaderLibrary*, so this pass must not outlive the owner;
    // PreviewRenderer is owned by Renderer and destroyed before m_graph, which guarantees it.
    if (!ENGINE_CHECK(m_lib != nullptr,
                      "PreviewStudioShadowPass::Init with no shared ShaderLibrary - "
                      "SetSharedShaderLibrary must be called first; the pass will "
                      "not draw."))
        return;

    // ---- The one thing this pass writes into the borrowed library ----
    // ShaderLibrary::Register is a fixed-array store indexed by ShaderID: it mints no id (dynamic
    // ids live in DynamicShaderRegistry), touches no cache (ClearCaches leaves m_registry alone, so
    // a hot reload keeps this) and is idempotent across the preview GraphSets. The owner
    // (GBufferPass) registers GBuffer_VS + GBuffer_PS and never uses Shadow_PS, so nothing it
    // builds changes. The alpha-test shadow pair is this pass's requirement, so it is registered
    // here rather than in the owner, both stages: Shadow.ps's PSIn matches only Shadow.vs's VSOut
    // (see BuildPSODesc). Same source and permutation as ShadowPass compiles, so the shader cache
    // returns the same .ishdr blob and no new DXIL is produced (tools/check_dxil_identity.ps1).
    m_lib->Register(ShaderID::Shadow_VS, RHI::ShaderStage::VS, "Shadow.vs.hlsl");
    m_lib->Register(ShaderID::Shadow_PS, RHI::ShaderStage::PS, "Shadow.ps.hlsl");
    // The VS a masked custom material's hook shadow draws with when it has no custom VS,
    // registered for the same reasons as the pair above. The material's own PS is already in this
    // library: the GBuffer owner draws it.
    m_lib->Register(ShaderID::ShadowHook_VS, RHI::ShaderStage::VS, "ShadowHook.vs.hlsl");

    m_psoCache.Init(gfx, *m_lib);

    // The run-time sampler (SetDynamicSampler) for a hooked caster: linear wrap, what its material
    // shaders read as g_LinearWrap here; Shadow.ps's own alpha test names the fixed SMP_LINEAR_WRAP
    // slot. Byte-for-byte ShadowPass's descriptor (see its Init), so the hash dedupe returns the
    // existing sampler index instead of allocating a new slot.
    {
        RHI::SamplerDesc sd{};
        sd.filter    = RHI::Filter::MIN_MAG_MIP_LINEAR;
        sd.addressU = RHI::TextureAddressMode::WRAP;
        sd.addressV = RHI::TextureAddressMode::WRAP;
        sd.addressW = RHI::TextureAddressMode::WRAP;
        if (!gfx.CreateSampler(sd, m_alphaSamplerIdx))
            LOG_ERROR("PreviewStudioShadowPass: alpha-test wrap sampler creation "
                      "failed; a masked caster will sample with whatever s0 holds");
    }

    RHI::TextureDesc td;
    td.width      = kSize;
    td.height     = kSize;
    td.format     = RHI::Format::D32_FLOAT;
    td.bindFlags = RHI::BindFlag::DEPTH_STENCIL | RHI::BindFlag::SHADER_RESOURCE;
    td.usage      = RHI::Usage::DEFAULT;
    td.mipLevels = 1;
    td.arraySize = kSlices;
    td.clear      = RHI::ClearValue::DepthStencil(0.0f, 0);   // reversed Z
    td.debugName = "PreviewStudioShadow.Array";
    if (!gfx.CreateTexture(td, m_depthArray))
    {
        LOG_ERROR("PreviewStudioShadowPass: %ux%ux%u depth array creation failed; "
                  "the preview keeps LightingPass's fully-lit 1x1 fallback.",
                  kSize, kSize, kSlices);
        return;
    }
    m_srv   = gfx.GetTextureSRVGpuHandle(m_depthArray);
    m_state = RHI::ResourceState::DEPTHSTENCIL;

    // Warm the default PSO so the first shadowed frame does not compile one on
    // the render half.
    if (!m_psoCache.GetOrCreate(BuildPSODesc({}, false)))
        LOG_ERROR("PreviewStudioShadowPass: default PSO creation failed");

    if (!m_initLogged)
    {
        m_initLogged = true;
        LOG_INFO("[PREVIEW] studio shadow pass ready: %ux%u x%u D32 (borrows the "
                 "GBuffer VS, no PS; alpha-test casters get Shadow_VS+PS)",
                 kSize, kSize, kSlices);
    }
}

void PreviewStudioShadowPass::ReloadShaders(IGraphicsDevice& /*gfx*/)
{
    // ONLY our own cache. The library is the owner's and it has already been
    // cleared and re-warmed by the time this runs (PreviewRenderer::ReloadShaders
    // is called from Renderer::ReloadShaders AFTER the main graph's).
    m_psoCache.Clear();
    if (m_lib && !m_psoCache.GetOrCreate(BuildPSODesc({}, false)))
        LOG_ERROR("PreviewStudioShadowPass: PSO rebuild after shader reload failed");
}

uint64_t PreviewStudioShadowPass::GetShadowArrayGpuHandle() const
{
    return m_depthArray.IsValid() ? m_srv : 0ull;
}

PSODesc PreviewStudioShadowPass::BuildPSODesc(PermutationKey perm,
                                              bool doubleSided,
                                              bool mirrored,
                                              bool alphaTest,
                                              uint32_t customVSID,
                                              uint32_t hookPSID) const
{
    PSODesc desc;
    // The VS is part of the pair, not a constant. GBuffer_VS and Shadow_VS compute the same
    // position here (PVF fetch, instance world matrix, b1's matrix at offset 0) but their output
    // signatures differ, and a VS output that does not match the PS input fails
    // CreateGraphicsPipelineState (debug layer: 'Semantic TEXCOORD is defined for mismatched
    // hardware registers'). So an alpha-tested caster takes Shadow.vs + Shadow.ps, and an opaque
    // one GBuffer_VS with no PS.
    desc.vsID = alphaTest ? ShaderID::Shadow_VS : ShaderID::GBuffer_VS;
    // No pixel shader for an opaque caster: the GBuffer PS writes six render targets and this
    // pass has none; ShadowPass's depth-only bucket does the same (dropping a may-discard PS
    // restores the early-Z depth write). An alpha-tested caster gets Shadow_PS with the ALPHA_TEST
    // bit, as in ShadowPass's GRP_ALPHATEST bucket, so a Masked / Dithered / opacity-mapped
    // material casts the silhouette the GBuffer clips. Only that branch pays.
    desc.psID        = alphaTest ? ShaderID::Shadow_PS : PSODesc::kNoPixelShader;
    // A masked custom material takes its own pair: its PS in the SHADOW_HOOK variant (the hook
    // decides the coverage, as in the GBuffer) and the VS whose output is that PS's input (its
    // custom VS or ShadowHook_VS). DOUBLE_SIDED is dropped from its key: the game's per-draw loops
    // key the hook variant on ALPHA_TEST alone (double-sidedness is the rasterizer's cull there, as
    // below), so this reuses their blob.
    if (hookPSID != 0)
    {
        perm.Set(PermutationKey::DOUBLE_SIDED, false);
        ShadowCaster::SelectHookShaders(desc, perm, customVSID, hookPSID);
    }
    desc.perm        = perm;
    desc.inputLayout = InputLayoutType::None;   // PVF

    // Back-face cull with the same conservative slope bias as ShadowPass's opaque bucket; the
    // receiver-side normal offset and receiver-plane depth bias in shadow.hlsli do most of the
    // work, this catches the residual rasterisation mismatch. Alpha test is a second cull axis, as
    // in ShadowPass: a cut-out exposes the far shell through its holes, so it rasterises with
    // CullMode::NONE. The bias pair follows the cull mode, not DOUBLE_SIDED: -30 / -1.0 are
    // authored for two-sided rasterisation.
    const bool twoSided             = doubleSided || alphaTest;
    desc.rs.cullMode               = twoSided ? RHI::CullMode::NONE
                                               : RHI::CullMode::BACK;
    // Mirrored is a winding flip, not a cull change. ShadowPass draws mirrored packets with
    // CullMode::NONE (world casters, no second PSO per cascade); here the caster is one preview
    // subject, PSOs are already keyed per (permutation, doubleSided), and the bias is authored for
    // back-face rasterisation, so do what GBufferPass does: declare the reversed winding and keep
    // culling the same faces.
    desc.rs.frontCounterClockwise = mirrored;
    desc.rs.depthClipEnable       = false;
    desc.rs.depthBias              = twoSided ? -30 : -50;
    desc.rs.slopeScaledDepthBias = twoSided ? -1.0f : -1.5f;
    desc.rs.depthBiasClamp        = 0.0f;

    desc.dss.depthEnable     = true;
    desc.dss.depthWriteMask = RHI::DepthWriteMask::ALL;
    desc.dss.depthFunc       = RHI::ComparisonFunc::GREATER_EQUAL;  // reversed Z

    desc.rtvCount  = 0;
    desc.dsvFormat = RHI::Format::D32_FLOAT;
    return desc;
}

RHI::CommandList PreviewStudioShadowPass::Execute(RHI::CommandList cl)
{
    if (!m_depthArray.IsValid() || !m_lib) return cl;
    // No CB means no light VP, and drawing with the PREVIOUS frame's matrix is
    // worse than not drawing: the map would be stale and the receiver would be
    // shadowed by a ghost.
    if (!m_viewCB || !m_viewCB->IsValid()) return cl;

    IGraphicsDevice& gfx = cl.GetDevice();

    // SELF-BARRIERED, both ways. This texture is not a graph texture, so
    // RenderGraph emits nothing for it and m_state is the only tracker there is.
    if (m_state != RHI::ResourceState::DEPTHSTENCIL)
    {
        cl.PushBarrier(RHI::GPUBarrier::Image(&m_depthArray, m_state,
                                              RHI::ResourceState::DEPTHSTENCIL));
        m_state = RHI::ResourceState::DEPTHSTENCIL;
    }

    // Every slice is cleared, including slice 1 that nothing samples (it exists only to make the
    // SRV a Texture2DArray, see kSlices): uninitialised DEFAULT-heap memory read through the array
    // declaration has no defined contents.
    for (uint32_t s = 0; s < kSlices; ++s)
        gfx.ClearDepthStencilSlice(m_depthArray, s, 0.0f, 0, cl);   // reversed Z

    const DrawList draws = cl.GetContext().GetDrawList(DrawFilter::Opaque);
    // Counted in the draw loop from the resolved PSODesc (psID != kNoPixelShader), not from the
    // packet list or its ALPHA_TEST bit: a BuildPSODesc that stopped honouring the bit must read
    // alphaTested=0, the regression the preview_fixtures gate catches.
    uint32_t casters = 0, clipped = 0, hooked = 0;
    // The clear alone is a correct frame: an empty draw list means "nothing
    // casts", and a cleared reversed-Z map reads as 0.0 everywhere, which
    // SampleCmpLevelZero(GREATER_EQUAL) resolves to fully lit. Transition and
    // leave rather than early-out, or the consumer samples a DEPTHSTENCIL
    // resource.
    if (!draws.empty())
    {
        gfx.SetDepthStencilSlice(m_depthArray, 0, cl);
        cl.SetViewport(kSize, kSize);
        cl.SetScissorRect(kSize, kSize);
        cl.SetPrimitiveTopology();

        const uint64_t bindlessHandle = cl.GetBindlessTableHandle();
        const RHI::PipelineState* activePSO = nullptr;

        for (const DrawPacket& dp : draws)
        {
            if (dp.castShadow == 0) continue;

            // The material's custom vertex shader is not used (except by a hook draw, below): ShadowPass
            // compiles a custom VS's CUSTOM_VS_SHADOW permutation, and the preview would hold PSOs for a
            // variant the main pipeline may never build, in a cache it shares nothing with. A material
            // whose world-position offset moves its silhouette therefore casts its rest shape here -
            // invisible for a unit primitive under a fixed key light, a known gap for a mesh override.
            PermutationKey perm{};
            const bool doubleSided = dp.permutation.Has(PermutationKey::DOUBLE_SIDED);
            perm.Set(PermutationKey::DOUBLE_SIDED, doubleSided);
            // The one material bit that reaches the pixel stage, taken off the packet the record half
            // already classified (Masked and Dithered always, Opaque when the material enables alpha
            // test), so the shadow's clip decision is the GBuffer's. -preview_shadow_alpha=0 turns it off
            // for every caster for one run (see SetAlphaTestEnabled); the default leaves every pack that
            // does not name the flag unchanged.
            const bool alphaTest = m_alphaTestEnabled &&
                                   dp.permutation.Has(PermutationKey::ALPHA_TEST);
            perm.Set(PermutationKey::ALPHA_TEST, alphaTest);

            // Mirrored is part of the PSO key, so the `pso != activePSO` rebind splits a mixed list into
            // two runs instead of drawing every part with the first packet's winding. The desc is kept
            // because the count below is taken off it, not off the packet bit.
            // A masked custom material draws with its own hook pair (hookPSID) only where the pixel stage
            // is on, so -preview_shadow_alpha=0 still strips every caster. Its custom VS is used here,
            // unlike the rest-shape rule above: the hook draw is the variant the game's shadow passes
            // build (CUSTOM_VS_SHADOW + SHADOW_HOOK), not a third one only the preview would hold.
            const uint32_t hookPS = alphaTest ? ShadowCaster::HookPS(dp) : 0u;
            PSODesc desc = BuildPSODesc(perm, doubleSided, dp.mirrored != 0,
                                        alphaTest, dp.customVSID, hookPS);
            const RHI::PipelineState* pso = m_psoCache.GetOrCreate(desc);
            if ((!pso || !pso->IsValid()) && hookPS != 0)
            {
                // The hook does not build as a shadow: fall back to Shadow_VS + Shadow_PS
                // (ShadowCasterShaders.h).
                ShadowCaster::NoteHookFallback(hookPS, GetName());
                desc = BuildPSODesc(perm, doubleSided, dp.mirrored != 0, alphaTest);
                pso  = m_psoCache.GetOrCreate(desc);
            }
            if (!pso || !pso->IsValid()) continue;

            // The resolved PSO's own answer, and the same bool the binds below are gated on, so "a pixel
            // stage was selected" and "its resources were bound" cannot disagree. hookPSO: the material's
            // own PS was selected.
            const bool clipPSO = (desc.psID != PSODesc::kNoPixelShader);
            const bool hookPSO = hookPS != 0 && desc.psID == static_cast<ShaderID>(hookPS);

            // COUNTED AFTER the PSO resolved, so a variant that failed to
            // build is not reported as a clipped caster.
            ++casters;
            if (clipPSO) ++clipped;
            if (hookPSO) ++hooked;

            if (pso != activePSO)
            {
                cl.SetPipelineState(*pso);
                activePSO = pso;
                cl.BindDescriptorHeaps();
                cl.BindRootBufferSRV(S::InstanceBuf, RG::RGSlot::InstanceBuffer);
                cl.BindRootBufferSRV(S::MeshDesc,    RG::RGSlot::MeshDescriptors);
                if (bindlessHandle)
                    cl.BindDescriptorTableHandle(S::BindlessBuffers, bindlessHandle);
                // b1 = the LIGHT's view-projection, NOT the graph's PerView slot.
                // That is the whole trick that lets this pass reuse GBuffer_VS:
                // the shader writes mul(worldPos, viewProj), so a different
                // viewProj is a different camera.
                gfx.BindConstantBuffer(SR::CB_b1, *m_viewCB, cl);

                // ---- What Shadow.ps additionally reads ----
                // Bound on the PSO edge, only for the alpha-test variant: the three things ShadowPass binds for
                // its GRP_ALPHATEST bucket:
                //   t0 space2  g_AllTextures[]  - the base-colour / mask lookup (Shadow.ps samples it through
                //                                 the fixed SMP_LINEAR_WRAP slot)
                //   dynamic    sampler          - the material sampler a hook PS reads as g_LinearWrap
                //   t2 space0  g_Materials[]    - alphaRef, the two texture handle ids, MaterialUV and the
                //                                 DITHERED_COVERAGE flag
                // The per-instance dissolve clip in Shadow.ps cannot fire here: WriteStudioShadowView writes
                // dissolveNoiseIdx = ~0u into this CB (byte 224, gDissolveNoiseIdx), and the preview's
                // private instance row carries dissolveParams.x = 0.
                if (clipPSO)
                {
                    if (cl.gfx)
                        if (const uint64_t texTable =
                                cl.gfx->GetBindlessTextureTableGpuHandle())
                            cl.BindDescriptorTableHandle(S::BindlessTex, texTable);
                    if (m_alphaSamplerIdx >= 0)
                        cl.SetDynamicSampler(m_alphaSamplerIdx);   // the material sampler a hook PS reads (SmpDynamic)
                    if (const RHI::GPUBuffer* matBuf =
                            cl.GetContext().GetBuffer(RG::RGSlot::MaterialBuffer))
                        if (matBuf->IsValid())
                            cl.BindBufferSRV(SR::SRV_t2, *matBuf);
                }
            }

            // What the material's own PS (and its custom VS) reads beyond Shadow.ps's three: its b8
            // parameters and space3 textures, the slices BuildOnePacket packed for this packet. Bound per
            // draw, as in the game's per-draw shadow loops.
            if (hookPSO && cl.gfx)
            {
                if (dp.customCbvVA)    cl.gfx->BindCustomMaterialCBV(dp.customCbvVA, cl);
                if (dp.customTexTable) cl.gfx->BindCustomMaterialTextureTable(dp.customTexTable, cl);
            }

            // All four root constants in ONE call: splitting them into 3 + 1 hits a driver corner case
            // (see GBufferPass's note).
            uint32_t constants[4] = {
                dp.meshDescriptorIndex,
                dp.instanceOffset,
                dp.materialIndex,
                dp.prevPosElementBase
            };
            gfx.SetRootConstants4(constants[0], constants[1], constants[2], constants[3], cl);
            cl.DrawInstanced(dp.vertexOrIndexCount, dp.instanceCount, 0, 0);
        }
    }

    // One line, and the only evidence a pack can gate on that the map has anything in it: a
    // shadow that never renders and one that lands exactly under the terminator look identical
    // (see the standard_shadow fixture's note).
    // Latched on a caster, not on reaching this line: the Opaque list is empty on a frame whose
    // material routes forward (a translucent preview draws through TransparentPass#2), an ordinary
    // first frame, and packs gate on the exact '1 draw(s), 1 caster(s)' text.
    // `alphaTested=` is the direct gate on the masked-shadow variant: under the shipped rig the key
    // light sits ~30 deg off the camera axis, so the self-shadow lands where N.L <= 0 and the
    // picture barely changes with the clip; the count is what says the clipped PSO was selected.
    if (!m_drawLogged && casters != 0)
    {
        m_drawLogged = true;
        // `hooked=` is the same PSO-level gate for the hook pair: casters that drew with their
        // material's own PS.
        LOG_INFO("[PREVIEW] studio shadow first render: %zu draw(s), %u "
                 "caster(s), alphaTested=%u hooked=%u", draws.size(), casters,
                 clipped, hooked);
    }

    cl.PushBarrier(RHI::GPUBarrier::Image(&m_depthArray,
                                          RHI::ResourceState::DEPTHSTENCIL,
                                          RHI::ResourceState::DEPTH_READ_SRV));
    m_state = RHI::ResourceState::DEPTH_READ_SRV;
    return cl;
}
