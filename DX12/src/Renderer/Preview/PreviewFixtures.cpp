// PreviewFixtures.cpp - see the header for why these live in code.

#include "Renderer/Preview/PreviewFixtures.h"

#include "Graphics/ShadingStencil.h"

#include <cstring>

namespace
{
    // Every fixture starts here. A default-constructed MaterialComponent already carries the
    // engine texture defaults and neutral PBR values; this adds the two shadow flags every
    // fixture shares (see the header) and a mid-grey albedo, so fixtures differ by shading model
    // rather than by colour.
    MaterialComponent Base()
    {
        MaterialComponent m{};
        // CAST_SHADOW is on by default; clear it. DISABLE_RECEIVE_SHADOW is set directly, not
        // through SetReceiveShadow(false): that setter also calls SetDirty(), and dirtiness is the
        // installing caller's decision, not the builder's.
        m.flags &= ~MaterialComponent::CAST_SHADOW;
        m.flags |= MaterialComponent::DISABLE_RECEIVE_SHADOW;
        // A 0.5 sRGB grey, not white: a white ball saturates under the studio key light and every
        // fixture then looks the same at the top end (preview_material's _imageWhy records a white
        // ball scoring higher on mean and stddev than the real material).
        m.baseColor    = { 0.5f, 0.5f, 0.5f, 1.0f };
        // Pin roughness and metalness instead of inheriting them from the
        // default ARM texture: `roughness = lerp(roughnessMin, roughnessMax,
        // surface.g)` (material_surface.hlsli), so min == max is the only way
        // to state a value that does not depend on what Default_ARM.itex
        // happens to contain.
        m.roughnessMin = m.roughnessMax = 0.5f;
        m.metalnessMin = m.metalnessMax = 0.0f;
        m.reflectance  = 0.5f;
        m.customShadingModel = ShadingModel::Standard;
        m.SyncLegacyShaderType();
        return m;
    }

    struct Entry
    {
        const char* name;
        MaterialComponent (*build)();
    };

    // ---- The roster ----
    // One fixture per claim the preview makes. Each comment says what it exercises and what a
    // reader should SEE: a fixture whose picture nobody can predict is not evidence.

    // Opaque PBR, the control: dielectric, mid roughness, stencil kPBR.
    MaterialComponent Standard() { return Base(); }

    // Metal. Needs a non-black IBL: at metalness 1 the diffuse term is gone and everything the
    // sphere shows is the prefiltered radiance cube plus the key light's GGX lobe, so it is the
    // first fixture to fail if the studio HDRI falls back to the 1x1 black texture.
    MaterialComponent Metal()
    {
        MaterialComponent m = Base();
        m.metalnessMin = m.metalnessMax = 1.0f;
        m.roughnessMin = m.roughnessMax = 0.2f;
        m.baseColor    = { 0.95f, 0.93f, 0.88f, 1.0f };   // pale gold-ish steel
        return m;
    }

    // Masked, with a real opacity source. The clip source is baseColor.a x BASECOLORMAP.a x
    // OPACITYMAP.r, and under the engine defaults each is 1.0 (Default_BaseColor is opaque, an
    // empty OPACITYMAP reads as 1.0), so without a texture a synthesised Masked fixture keeps or
    // discards every texel and looks identical to `standard`.
    // kMaskRampTex is engine-shipped (the toon RAMPMAP family, so a re-import cannot quietly
    // remove it) and uncompressed R8G8B8A8_UNORM_SRGB 256x16 whose red channel is a linear
    // 0 -> 255 ramp along U: a threshold pattern that is checkable offline, which a BC7 texture is
    // not until something decodes it.
    // kMaskTiling repeats the ramp around the sphere: untiled, the camera sees half of one wrap
    // and whether that half is kept or discarded depends on where the camera sits; with three
    // repeats every orbit sees at least one full transition. V is not tiled: the ramp's 16 rows
    // differ and V = 1 maps them pole to pole once. The other slots are engine defaults (2x2 / 1x1
    // constants), so the tiling changes only the mask.
    // alphaRef 0.35 against the sRGB-decoded ramp cuts at ~U 0.63, keeping the bright third of each
    // repeat; `dithered` is the fixture whose clip needs no texture.
    // Not a whole-frame A/B subject: a cutout exposes the backdrop, which -preview_parity's two
    // sides do not share (the preview rig draws its own, the main pipeline draws the scene). Over
    // the pixels the clip keeps the parity is exact (maxDelta 0); use `unlit` when a whole-frame
    // zero floor is wanted.
    constexpr const char* kMaskRampTex = "engine/textures/RampMap_Linear_RGBAHalf.itex";
    constexpr float       kMaskTiling  = 3.0f;

    MaterialComponent Masked()
    {
        MaterialComponent m = Base();
        m.userBlendMode = BlendMode::Masked;
        m.alphaRef      = 0.35f;
        m.textures[MaterialComponent::OPACITYMAP].name  = kMaskRampTex;
        m.textures[MaterialComponent::OPACITYMAP].uvset = 0;
        m.texMulAdd = { kMaskTiling, 1.0f, 0.0f, 0.0f };
        return m;
    }

    // Dithered: proves alpha-test clipping executes. The threshold is
    // TemporalDither(sv.xy, gFrameIndex), a 4x4 Bayer pattern, and the preview pins gFrameIndex to
    // 0 (PreviewRenderer::WritePerViewCB), so coverage 0.5 discards a deterministic half of the
    // pixels: runs are bit-identical and the picture is visibly a screen door.
    // Do not use it for the A/B parity pack: the main pipeline's gFrameIndex rotates, so its dither
    // phase depends on the capture frame while the preview's is frozen.
    MaterialComponent Dithered()
    {
        MaterialComponent m = Base();
        m.userBlendMode = BlendMode::Dithered;
        m.baseColor.w   = 0.5f;   // coverage; the dither compares against this
        return m;
    }

    // Toon / NPR. Selects the NPR_PASS permutation of Lighting.ps (stencil kNPR), a compile-time
    // variant: the most consequential fixture, because a wrong stencil renders a plausible picture
    // through the wrong binary and no image gate can see it. Defaults kept: nprShadowThreshold 0.5
    // / smooth 0.05 give the hard terminator that makes the toon variant obvious.
    MaterialComponent Toon()
    {
        MaterialComponent m = Base();
        m.customShadingModel = ShadingModel::Toon;
        m.SyncLegacyShaderType();
        return m;
    }

    // Anisotropic. A runtime branch inside the PBR variant fed by GBuffer RT4 (.r = the direction
    // as an angle in the tangent plane, .g = the signed anisotropy), so it tests the RT4
    // encode/decode pair, not a permutation. Negative, Unreal's sign convention: the streak runs
    // across the authored tangent, which on the sphere's longitudinal tangents is the brushed-metal
    // look. Metallic and smooth, or there is no lobe to stretch.
    MaterialComponent Aniso()
    {
        MaterialComponent m = Base();
        m.customShadingModel = ShadingModel::Anisotropic;
        m.metalnessMin = m.metalnessMax = 1.0f;
        m.roughnessMin = m.roughnessMax = 0.30f;
        m.anisotropy         = -0.8f;
        m.anisotropyRotation = 0.0f;
        m.SyncLegacyShaderType();
        return m;
    }

    // Subsurface. Wrap-softened diffuse + a per-light transmission lobe, lit from behind by the
    // rig's clustered back light (Preview::kBackLight). It asserts "finite, lit, right stencil",
    // not fidelity.
    MaterialComponent Sss()
    {
        MaterialComponent m = Base();
        m.customShadingModel = ShadingModel::Subsurface;
        m.subsurfaceColor    = { 0.80f, 0.35f, 0.25f, 1.0f };
        m.subsurfaceWrap     = 0.6f;
        m.subsurfaceStrength = 1.0f;
        m.subsurfaceOpacity  = 0.4f;
        m.roughnessMin = m.roughnessMax = 0.45f;
        m.SyncLegacyShaderType();
        return m;
    }

    // ClearCoat. A second split-sum lobe at the coat's own roughness on top of the base, with the
    // base attenuated by the coat's Fresnel. Strength 0 is bit-identical to Standard, so a fixture
    // that forgot `clearCoat` would be a silent no-op: hence 1.0 and a mirror-smooth coat over a
    // deliberately rough base, the configuration a single GGX lobe cannot fake.
    MaterialComponent ClearCoat()
    {
        MaterialComponent m = Base();
        m.customShadingModel   = ShadingModel::ClearCoat;
        m.clearCoat            = 1.0f;
        m.clearCoatRoughness   = 0.03f;
        m.roughnessMin = m.roughnessMax = 0.75f;
        m.baseColor            = { 0.35f, 0.05f, 0.06f, 1.0f };   // car-paint red
        m.SyncLegacyShaderType();
        return m;
    }

    // TwoSidedFoliage. Lights the back with -N and needs no thickness channel. DOUBLE_SIDED is
    // mandatory: with back faces culled there is no far surface for the transmitted light to leave
    // through and the model is inert. Lit from behind by the same back light as Sss.
    // A plain material (no custom shader) reaches the transmission through the named
    // subsurfaceColor / Strength / Wrap fields: customParams do not exist without shader
    // reflection, and the material packer (WriteMaterialSlot) discriminates on customParamCount.
    MaterialComponent TwoSided()
    {
        MaterialComponent m = Base();
        m.customShadingModel = ShadingModel::TwoSidedFoliage;
        m.flags |= MaterialComponent::DOUBLE_SIDED;
        m.subsurfaceColor    = { 0.35f, 0.70f, 0.22f, 1.0f };   // leaf green
        m.subsurfaceStrength = 1.0f;
        m.subsurfaceWrap     = 0.5f;
        m.baseColor          = { 0.22f, 0.42f, 0.16f, 1.0f };
        m.roughnessMin = m.roughnessMax = 0.65f;
        m.SyncLegacyShaderType();
        return m;
    }

    // Unlit. Stencil kUnlit, a third Lighting.ps variant that early-outs to baseColor. Emissive is
    // authored so the fixture is not just "the base colour at 1.0 exposure": a flat-albedo fixture
    // could not tell if the early-out stopped inheriting the GBuffer emissive seed.
    MaterialComponent Unlit()
    {
        MaterialComponent m = Base();
        m.customShadingModel = ShadingModel::Unlit;
        m.baseColor     = { 0.20f, 0.45f, 0.75f, 1.0f };
        m.emissiveColor = { 0.20f, 0.45f, 0.75f, 0.6f };   // .w is the strength
        m.SyncLegacyShaderType();
        return m;
    }

    // ---- Shadow, glass, custom-VS, water and forward-twin fixtures ----

    // `standard_shadow` (StandardShadow, below) and `maskedshadow`: the studio shadow on.
    // StandardShadow is `standard` with the receive-shadow flag cleared and CAST_SHADOW set, so the
    // preview's key-light depth pass lands. Not an A/B fixture, and -preview_parity forces the
    // studio shadow off whatever the fixture: the preview's map is a 512^2 ortho slice around a
    // 3.2 m box with a pinned Bayer phase, the main pipeline's is four camera-fitted cascades with
    // a rotating one, so the evidence is a measured luma band against `standard` (darker by a
    // number the pack records), not a pixel comparison.
    // MaskedShadow is `masked` with the same two flag changes. `masked` alone cannot show what the
    // studio shadow does with a cut-out (it neither casts nor receives, so shadow on vs off is
    // bit-identical); here the opacity ramp cuts holes in the caster, the key light passes through
    // them and the receiver's shadow term follows, which makes the shadow pass's pixel stage
    // observable (an unclipped caster would throw a solid silhouette). Not an A/B fixture, for the
    // same reason as StandardShadow.
    MaterialComponent MaskedShadow()
    {
        MaterialComponent m = Masked();
        m.flags &= ~MaterialComponent::DISABLE_RECEIVE_SHADOW;
        m.flags |= MaterialComponent::CAST_SHADOW;
        return m;
    }

    MaterialComponent StandardShadow()
    {
        MaterialComponent m = Base();
        m.flags &= ~MaterialComponent::DISABLE_RECEIVE_SHADOW;
        m.flags |= MaterialComponent::CAST_SHADOW;
        return m;
    }

    // ---- maskedhook: the masked hook shadow ----
    // A masked custom material whose coverage comes only from its hook:
    // tests/fixtures/masked_hook_shadow/stripes_hook.hlsl computes stripes from the uv and writes
    // nothing else, and the engine slots keep Base()'s defaults (alpha 1). It proves the studio
    // shadow runs the material's PS: clipping on the default slots alone would throw a solid
    // silhouette while the GBuffer draws stripes. Otherwise MaskedShadow's shape: it casts and
    // receives, so on a non-convex subject the cut-out lands on lit geometry (preview_hook_shadow
    // drives a torus, preview_shadow_clip's rig).
    // A test shader named from here, as kCustomVsPath names a content shader: the fixture states a
    // path and registers nothing. Only -preview_fixture asks for it; a tree without the file
    // resolves no custom PS and the fixture is a plain masked material with nothing to clip.
    constexpr const char* kHookStripesPath = "tests/fixtures/masked_hook_shadow/stripes_hook.hlsl";

    MaterialComponent MaskedHook()
    {
        MaterialComponent m = Base();
        m.flags &= ~MaterialComponent::DISABLE_RECEIVE_SHADOW;
        m.flags |= MaterialComponent::CAST_SHADOW;
        m.userBlendMode    = BlendMode::Masked;
        m.alphaRef         = 0.5f;
        m.texMulAdd        = { 6.0f, 1.0f, 0.0f, 0.0f };   // six stripes round the major circle
        m.useCustomShader  = true;
        m.customShaderPath = kHookStripesPath;
        return m;
    }

    // Transparent / forward (`glass`): proves TransparentPass#2 exists. BlendMode::Alpha routes the
    // material to DrawFilter::Transparent, which GBufferPass#2 never reads, so a missing forward
    // instance (or a wrong library borrow) renders the background and fails the luma floor.
    // A dielectric, not a tint: baseColor.a 0.35 is the blend coverage and keeps the backdrop
    // visible; low roughness keeps the key light's highlight, without which a translucent sphere at
    // 0.35 is nearly the background and the luma band is useless as a tripwire.
    // Keeps Base()'s DISABLE_RECEIVE_SHADOW, so the forward BRDF's ComputeShadowFactor is
    // short-circuited on the material side and the fixture measures blending and forward
    // lighting, not shadows.
    MaterialComponent Glass()
    {
        MaterialComponent m = Base();
        m.userBlendMode = BlendMode::Alpha;
        m.baseColor     = { 0.72f, 0.82f, 0.90f, 0.35f };
        m.roughnessMin = m.roughnessMax = 0.08f;
        m.metalnessMin = m.metalnessMax = 0.0f;
        m.reflectance   = 0.5f;
        m.SyncLegacyShaderType();
        return m;
    }

    // ---- customvs / customvs_off: the custom-vertex-shader A/B pair ----
    // An .imat fixture (preview_material_customvs) shows the VS id resolved and the b8 slice
    // packed, but not that the world-position offset moved a pixel: its luma band cannot (the
    // VS-on and VS-off means are ~4.9% apart). The machine-independent proof renders both and
    // compares them in-process, and an imagePair needs both materials from one run - a pair of
    // fixtures, not a pair of .imat files.
    // The shader is asset/Shaders/wind_vertex.vs.hlsl: tracked, in the offline dxc sweep, a
    // 32-byte reflected b8 cbuffer and no space3 texture. The material lane pins timeParams and
    // gFrameIndex to 0, so sin(p.time * speed + (x + z) * scale) is a fixed spatial warp and the
    // pair is bit-stable.
    // The preview still never calls RegisterDynamic: Renderer::ResolvePreviewCustomVSID resolves
    // the id on the GAME half, through the one fan-out, as for any .imat naming a custom shader.
    constexpr const char* kCustomVsPath = "asset/Shaders/wind_vertex.vs.hlsl";

    // CAST_SHADOW is set on both halves, their one departure from Base(). The "studio shadow is
    // the REST shape" badge only means something in a panel that has a studio shadow, and
    // PreviewStudioShadowPass counts casters: with Base()'s cleared flag it reports 0 draws and the
    // pack could not tell a right badge from no shadow. It moves no pixel: both keep
    // DISABLE_RECEIVE_SHADOW, so nothing samples the map.
    MaterialComponent CustomVsBase()
    {
        MaterialComponent m = Base();
        m.flags |= MaterialComponent::CAST_SHADOW;
        return m;
    }

    MaterialComponent CustomVs()
    {
        MaterialComponent m = CustomVsBase();
        m.useCustomShader         = true;
        // customShaderPath deliberately empty (the separate-VS shape): ResolveCustomPSID returns 0
        // while ResolveCustomVSID does not, the case that looks safe. Do not gate on the pixel stage
        // alone: that leaves root 35 / root 36 unbound on a shader that reads them, a DEVICE_HUNG with
        // no page fault and no debug-layer message.
        m.customVertexShaderPath  = kCustomVsPath;
        return m;
    }

    // The control: CustomVsBase() with no shader named, so the only difference from `customvs` is
    // the branch under test. Its own name rather than `standard`: -preview_fixture writes
    // preview_<name>.png and the imagePair must name two files that differ in exactly one thing
    // (and `standard` does not cast, so it is not that pair).
    MaterialComponent CustomVsOff() { return CustomVsBase(); }

    // Water: a water-domain material with a forward blend mode, and both halves are the point.
    // userBlendMode == Alpha would send any other material to TransparentPass#2, and
    // customShadingModel == Water is the branch in PreviewRenderer::WantsForward that keeps it on
    // the opaque route; with an opaque blend mode it would take that route for the ordinary reason
    // and the pack's "TransparentPass#2 never appears" assertion would prove nothing.
    // No water textures and no ripple field: the preview has no WaterSurfacePass to consume them,
    // which is what this fixture demonstrates. Its stencil is kPBR (ShadingStencil::RefFor answers
    // PBR for Water on purpose), so the capture line reads `stencil 1` like `standard`.
    MaterialComponent Water()
    {
        MaterialComponent m = Base();
        m.userBlendMode      = BlendMode::Alpha;
        m.customShadingModel = ShadingModel::Water;
        m.baseColor          = { 0.10f, 0.32f, 0.42f, 0.60f };
        m.roughnessMin = m.roughnessMax = 0.05f;
        m.metalnessMin = m.metalnessMax = 0.0f;
        m.reflectance        = 0.5f;
        m.SyncLegacyShaderType();
        return m;
    }

    // A fixture's forward twin: the same material with userBlendMode Alpha at alpha 1, so the
    // preview draws it in TransparentPass#2 through custom_surface's MS_ForwardLighting instead of
    // Lighting.ps - the same subject and studio, the other light path. preview_fwd_parity names
    // each pair and bounds the difference. Both paths evaluate the surface with
    // surface_eval.hlsli, so what is left is what forward does not do (for the toon twin, which
    // takes the toon model through Toon()'s two ramp colours: the screen-space depth rim on the
    // silhouette).
    MaterialComponent ForwardTwin(MaterialComponent m)
    {
        m.userBlendMode = BlendMode::Alpha;
        m.baseColor.w   = 1.0f;
        m.SyncLegacyShaderType();
        return m;
    }
    MaterialComponent StandardFwd()  { return ForwardTwin(Standard()); }
    MaterialComponent AnisoFwd()     { return ForwardTwin(Aniso()); }
    MaterialComponent SssFwd()       { return ForwardTwin(Sss()); }
    MaterialComponent ClearCoatFwd() { return ForwardTwin(ClearCoat()); }
    // Single-sided on purpose: a double-sided translucent draws its back faces with no depth
    // prepass, so at alpha 1 the far hemisphere's inside paints over the front (maxDelta 193 on
    // 49% of the pixels) - forward translucency's draw order, not the surface evaluation this pair
    // compares. The deferred side shows only front faces anyway.
    MaterialComponent TwoSidedFwd()
    {
        MaterialComponent m = ForwardTwin(TwoSided());
        m.flags &= ~MaterialComponent::DOUBLE_SIDED;
        return m;
    }
    MaterialComponent ToonFwd()      { return ForwardTwin(Toon()); }

    constexpr Entry kFixtures[] = {
        { "standard",  &Standard  },
        { "metal",     &Metal     },
        { "masked",    &Masked    },
        { "toon",      &Toon      },
        { "aniso",     &Aniso     },
        { "sss",       &Sss       },
        { "clearcoat", &ClearCoat },
        { "twosided",  &TwoSided  },
        { "unlit",     &Unlit     },
        { "dithered",  &Dithered  },
        // Rows from here on are appended, never inserted: packs name their fixtures in order and the
        // runner writes one PNG per name, so a reordering would silently rename every measured band.
        { "standard_shadow", &StandardShadow },
        { "glass",           &Glass          },
        { "customvs",        &CustomVs       },
        { "customvs_off",    &CustomVsOff    },
        { "maskedshadow",    &MaskedShadow   },
        { "water",           &Water          },
        { "standard_fwd",    &StandardFwd    },
        { "aniso_fwd",       &AnisoFwd       },
        { "sss_fwd",         &SssFwd         },
        { "clearcoat_fwd",   &ClearCoatFwd   },
        { "twosided_fwd",    &TwoSidedFwd    },
        { "toon_fwd",        &ToonFwd        },
        { "maskedhook",      &MaskedHook     },
    };
    constexpr std::size_t kFixtureCount = sizeof(kFixtures) / sizeof(kFixtures[0]);
}

namespace Preview
{

bool MakeFixtureMaterial(std::string_view name, MaterialComponent& out)
{
    for (const Entry& e : kFixtures)
    {
        if (name == e.name)
        {
            out = e.build();
            return true;
        }
    }
    return false;
}

std::size_t FixtureCount() { return kFixtureCount; }

const char* FixtureNameAt(std::size_t i)
{
    return (i < kFixtureCount) ? kFixtures[i].name : nullptr;
}

unsigned FixtureStencil(std::string_view name)
{
    MaterialComponent m;
    if (!MakeFixtureMaterial(name, m)) return 0u;
    return static_cast<unsigned>(ShadingStencil::RefFor(m.customShadingModel));
}

} // namespace Preview
