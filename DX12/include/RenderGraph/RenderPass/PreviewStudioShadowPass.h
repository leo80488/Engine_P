#pragma once

// PreviewStudioShadowPass - the material preview's own key-light shadow map: depth only,
// 512^2, one sampled slice, and deliberately not a ShadowPass instance. ShadowPass's cascades
// are fitted to the main camera's frustum; the preview camera is a fixed studio orbit around a
// unit subject two metres away, so those cascades miss it by orders of magnitude, and a second
// ShadowSystem would cost a second cascade solver, a 256 MB 4096^2 x 4 array and a per-frame
// Update for one sphere. This pass costs 2 MB.
//
// Borrows the main GBufferPass's ShaderLibrary and creates none: the static shadow ids are
// registered into it (Init), a material's dynamic ids resolve through DynamicShaderRegistry.
// An opaque caster draws GBuffer_VS with no PS (PSODesc::kNoPixelShader), an alpha-tested one
// Shadow_VS + Shadow_PS, a masked custom material its own SHADOW_HOOK PS with its custom VS or
// ShadowHook_VS (ShadowCasterShaders.h). No pixel stage writes colour, so there is no
// SV_Target signature to get wrong, and each VS / PS pair travels together (BuildPSODesc). Only
// a hooked caster uses a custom vertex shader; every other caster casts its rest shape (see
// Execute).
//
// Owns one Texture2DArray (D32_FLOAT, kSize x kSize x kSlices) that is not a graph texture, so
// this pass barriers it itself (as RTReflectionPass does its t44 output). Everything else
// arrives per frame through setters from PreviewRenderer, which owns the studio rig.

#include "RenderGraph/RenderGraph.h"
#include "Graphics/PSOCache.h"
#include "Graphics/GraphicsStruct.h"

#include <string>

class ShaderLibrary;

class PreviewStudioShadowPass : public RG::RenderPass
{
public:
    static constexpr const char* kBaseName = "PreviewStudioShadowPass";
    // 512^2: the subject occupies a 1 m box inside a 3.2 m fit, so ~160 texels per metre - about
    // what a 4096^2 cascade gives at 25 m, and more than a 128^2 thumbnail can resolve.
    static constexpr uint32_t kSize = 512;
    // Two slices, not one and not four.
    //   - Not one: IGraphicsDevice::CreateTexture emits a TEXTURE2DARRAY SRV only when
    //     arraySize > 1, and shadow.hlsli declares `Texture2DArray<float> gShadowCascades`. A
    //     Texture2D SRV bound to that declaration is undefined behaviour Resource Binding Tier 3
    //     does not report (as TransparentPass's 1x1 CSM fallback notes).
    //   - Not four: only slice 0 is selected. PreviewRenderer pins LightCB.cascadeSplits so
    //     ComputeCascadeShadowTerm resolves to cascade 0 for every viewZ, and shadowBlendRange to 0
    //     so the cascade+1 cross-fade never samples. Slice 1 only makes the SRV an array; it is
    //     cleared every frame so it is never uninitialised memory.
    static constexpr uint32_t kSlices = 2;

    PreviewStudioShadowPass() = default;

    const char* GetName() const override { return m_name.c_str(); }
    // Call BEFORE the pass is handed to a RenderGraph - GBufferPass.h lists the three things
    // RenderGraph keys on GetName().
    void SetNameSuffix(const char* suffix);

    void Setup(RG::RenderGraphBuilder& b) override;
    void Init (IGraphicsDevice& gfx)      override;
    // m_lib is always borrowed (there is no owned library member), so this releases the PSO
    // cache and the depth array and nothing else.
    void Shutdown(IGraphicsDevice& gfx)   override;
    RHI::CommandList Execute(RHI::CommandList cl) override;
    // The preview view shows its studio shadow only when the request asks for it and this
    // frame's light VP was published (PreviewRenderer::ApplyPreviewView); the world lane never does
    // (RenderGraph/SceneView.h).
    uint32_t RequiredShowFlags() const override { return static_cast<uint32_t>(RG::ShowFlag::Shadows); }
    // Borrower: clears only its own PSOCache. GetReloadableShaderLibrary stays the base's nullptr,
    // so the graph's default reload never reaches the borrowed library through this class - a
    // second ClearCaches would double-bump the reload epoch MaterialReflectionSync stamps against.
    void ReloadShaders(IGraphicsDevice& gfx) override;

    // The library whose ids this pass's PSOCache resolves. Call before Init(), after the owner's
    // Init.
    void SetSharedShaderLibrary(ShaderLibrary& lib) { m_lib = &lib; }

    // The per-view constant buffer whose viewProj is the key light's VP. Owned by PreviewRenderer
    // (a RendererDetail::PerViewCB ring slot); this pass only binds it, and draws nothing while it
    // is null. Set every frame on the render half, before Execute.
    void SetViewCB(const RHI::GPUBuffer* cb) { m_viewCB = cb; }

    // -preview_shadow_alpha: false drops the pixel stage from every caster this pass draws,
    // whatever the packet's ALPHA_TEST bit says. The alphaTested=N log count is the PSO-level gate;
    // this lever lets one run render the same fixture with and without the clip and compare the
    // captures (an imagePair: same run, same adapter) to see whether the clip reaches any pixel.
    // Not harness-gated: only the setter is reachable from a flag, so there is one branch rather
    // than two spellings under an #ifdef. Default true. Set every frame on the render half,
    // beside SetViewCB.
    void SetAlphaTestEnabled(bool on) { m_alphaTestEnabled = on; }

    // SRV of the depth array, for LightingPass::SetShadowMap / TransparentPass::SetShadowMap.
    // 0 before Init or after a failed creation; both consumers substitute their own fully-lit 1x1
    // fallback for 0, so a failure degrades to "no shadow" rather than to an unbound table.
    uint64_t GetShadowArrayGpuHandle() const;

private:
    // @p mirrored flips frontCounterClockwise, as GBufferPass does for the same packet byte;
    // without it a negative-determinant placement casts its inside-out shell (the back faces
    // survive the depth test and the receiver gets the far side of the mesh).
    // @p alphaTest: a packet with ALPHA_TEST gets Shadow_PS with the ALPHA_TEST permutation instead
    // of kNoPixelShader, so a Masked / Dithered / opacity-mapped material casts its cut-out
    // silhouette. Opaque draws keep no PS (early-Z depth write intact).
    // @p hookPSID != 0: a masked custom material's own PS in its SHADOW_HOOK variant, fed by its
    // custom VS (@p customVSID) or ShadowHook_VS - the pair the game's shadow passes use
    // (ShadowCasterShaders.h), so the panel casts the coverage the scene will.
    PSODesc BuildPSODesc(PermutationKey perm, bool doubleSided,
                         bool mirrored = false, bool alphaTest = false,
                         uint32_t customVSID = 0, uint32_t hookPSID = 0) const;

    std::string  m_name = kBaseName;
    // Borrowed, never owned: this pass creates no ShaderLibrary at all, which works only because
    // it has no shader of its own.
    ShaderLibrary* m_lib = nullptr;
    PSOCache       m_psoCache;

    IGraphicsDevice* m_gfx = nullptr;
    RHI::Texture     m_depthArray;
    uint64_t         m_srv = 0;
    // m_depthArray's state between Executes. It is not a graph texture, so RenderGraph emits no
    // barrier for it. Starts at DEPTHSTENCIL (CreateTexture's initial state for a depth target)
    // and round-trips DEPTHSTENCIL -> DEPTH_READ_SRV every frame this pass runs.
    RHI::ResourceState m_state = RHI::ResourceState::DEPTHSTENCIL;

    const RHI::GPUBuffer* m_viewCB = nullptr;
    // The linear-WRAP run-time sampler (SetDynamicSampler) a hooked caster's material shaders read
    // as g_LinearWrap; Shadow.ps's own ALPHA_TEST branch names the fixed SMP_LINEAR_WRAP slot. Same
    // SamplerDesc as ShadowPass's, so GraphicsDX12::CreateSampler's hash dedupe returns the existing
    // index and this costs no new sampler slot (the corpus's `samplers 14/2048` ledger assertion
    // holds that). -1 until Init; a negative value skips the bind, the same degradation ShadowPass
    // takes.
    int  m_alphaSamplerIdx = -1;
    // The -preview_shadow_alpha lever (see SetAlphaTestEnabled).
    bool m_alphaTestEnabled = true;
    bool m_initLogged = false;
    // Set by the first Execute that drew a caster, not by the first Execute: an Opaque draw list
    // can legally be empty (a forward-domain material), and latching on that would emit a
    // "0 draw(s), 0 caster(s)" line no later frame can correct.
    bool m_drawLogged = false;
};
