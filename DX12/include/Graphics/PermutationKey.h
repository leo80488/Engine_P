#pragma once
#include <cstdint>

// PermutationKey - bitmask of the #define features of a shader variant; each bit is
// one compile-time define passed to DXC. Derived from MaterialData in the Renderer
// layer; never exposed to ECS.
//
// PERMUTATION_KEY_BITS is the single source of every bit: it generates the
// PermutationKey::X constants below and kPermutationDefines, which
// ShaderLibrary::MakeDefines walks to build the -D list, so a new bit is one row:
//
//   X(Constant, bit, "DEFINE_NAME")
//
// Per-bit notes:
//   ALPHA_TEST          clip(alpha - alphaRef) in deferred GBuffer
//   DOUBLE_SIDED        back faces rendered; custom_surface flips the normal
//   ALPHA_BLEND         forward transparent (SRC_ALPHA / INV_SRC_ALPHA)
//   ADDITIVE_BLEND      forward transparent additive (ONE / ONE)
//   PREMULTIPLIED_BLEND forward transparent premultiplied (ONE / INV_SRC_ALPHA)
//   MULTIPLY_BLEND      forward transparent multiply (DST_COLOR / INV_SRC_ALPHA)
//   TRANSMISSIVE_BLEND  forward transmissive: the surface layer over the refracted background
//   TRANSMISSION_PREPASS the transmission prepass of a Transmissive surface: three data targets
//                       (TransmissionPrepass), never a colour
//                       (the five blend bits encode ONE enum; see kBlendModes)
//   BILLBOARD           camera-facing quad (VS ignores world rotation)
//   UNLIT               skip lighting, output albedo directly
//   NPR_STENCIL         lighting pass: stencil-gated NPR variant. Its DEFINE is
//                       NPR_PASS - the name Lighting.ps.hlsl tests.
//   CUSTOM_VS_SHADOW    custom_vertex.hlsli: compile the depth-only shadow variant
//   PARTICLE_LIT        particle_surface.hlsli: forward-lit (sun + IBL) instead
//                       of emissive
//   WATER_SURFACE       custom_surface.hlsli: compile the WATER main - a 4-MRT
//                       forward variant (GBuffer normal/surface/velocity stamp +
//                       alpha-blended HDR colour) drawn by WaterSurfacePass.
//                       Implies MS_IS_FORWARD. See ShadingModel::Water.
//   PARTICLE_SHADOW     particle_surface.hlsli: the LIT particle path also
//                       receives the sun shadow (CSM + per-object). A SECOND
//                       opt-in on top of PARTICLE_LIT, not implied by it: smoke,
//                       dust and steam should go dark under a canopy; fire,
//                       sparks and magic are emissive-ish and look wrong when a
//                       shadow crosses them. One PSO variant per lit bucket that
//                       asks for it, nothing for emitters that do not.
//   TRANSLUCENT_PREPASS custom_surface.hlsli: compile the translucent FRONT-LAYER
//                       prepass main - a 3-MRT, depth-writing, colour-less
//                       forward variant (normal / surface / velocity stamp of the
//                       nearest translucent surface) drawn by
//                       TranslucentFrontLayerPass so RTReflectionPass's second
//                       instance can trace reflections FROM the glass. Implies
//                       MS_IS_FORWARD, like water.
//   DEBUG_SHADE         debug_shade.hlsli: the material pixel shaders
//                       (custom_surface.hlsli's GBuffer and forward mains,
//                       particle_surface.hlsli) also count their invocation into
//                       the debug targets' counters (overdraw). Set on the PSOs
//                       only for the frames that show or capture such a target
//                       (GBufferPass / TransparentPass / ParticleRenderPass::
//                       SetDebugShade); every other frame's PSOs and DXIL stay
//                       unchanged.
//   SHADOW_HOOK         a masked custom material's SHADOW variant -
//                       custom_vertex.hlsli outputs the full
//                       MaterialSurfaceVaryings in the shadow view, and
//                       custom_surface.hlsli compiles a depth-only main that runs
//                       the material's hook and the GBuffer's coverage rule. Set
//                       only by the shadow passes, always with CUSTOM_VS_SHADOW
//                       and ALPHA_TEST.
//
// Never reuse bits 0, 1, 4 and 12 (kRetiredBits: HAS_NORMALMAP, HAS_EMISSIVE,
// SKINNED, OCCLUSION_CULL, which no shader tested), so a .ishdr name (the perm bits
// are in it) keeps its meaning.
#define PERMUTATION_KEY_BITS(X)                           \
    X(ALPHA_TEST,          2,  "ALPHA_TEST")              \
    X(DOUBLE_SIDED,        3,  "DOUBLE_SIDED")            \
    X(ALPHA_BLEND,         5,  "ALPHA_BLEND")             \
    X(ADDITIVE_BLEND,      6,  "ADDITIVE_BLEND")          \
    X(PREMULTIPLIED_BLEND, 7,  "PREMULTIPLIED_BLEND")     \
    X(MULTIPLY_BLEND,      8,  "MULTIPLY_BLEND")          \
    X(BILLBOARD,           9,  "BILLBOARD")               \
    X(UNLIT,               10, "UNLIT")                   \
    X(NPR_STENCIL,         11, "NPR_PASS")                \
    X(CUSTOM_VS_SHADOW,    13, "CUSTOM_VS_SHADOW")        \
    X(PARTICLE_LIT,        14, "PARTICLE_LIT")            \
    X(WATER_SURFACE,       15, "WATER_SURFACE")           \
    X(PARTICLE_SHADOW,     16, "PARTICLE_SHADOW")         \
    X(TRANSLUCENT_PREPASS, 17, "TRANSLUCENT_PREPASS")     \
    X(DEBUG_SHADE,         18, "DEBUG_SHADE")             \
    X(SHADOW_HOOK,         19, "SHADOW_HOOK")             \
    X(TRANSMISSIVE_BLEND,  20, "TRANSMISSIVE_BLEND")      \
    X(TRANSMISSION_PREPASS, 21, "TRANSMISSION_PREPASS")

struct PermutationKey
{
    uint32_t bits = 0;

#define PERMUTATION_KEY_DECLARE_CONSTANT(Constant, Bit, Define) \
    static constexpr uint32_t Constant = 1u << (Bit);

    PERMUTATION_KEY_BITS(PERMUTATION_KEY_DECLARE_CONSTANT)
#undef PERMUTATION_KEY_DECLARE_CONSTANT

    static constexpr uint32_t kBlendModes = ADDITIVE_BLEND | PREMULTIPLIED_BLEND | MULTIPLY_BLEND |
                                            TRANSMISSIVE_BLEND;

    void Set(uint32_t feature, bool val) noexcept
    { bits = val ? (bits | feature) : (bits & ~feature); }
    bool Has(uint32_t feature) const noexcept { return (bits & feature) != 0; }

    // The five blend bits encode ONE enum. ALPHA_BLEND is also the "forward" switch
    // every blend mode implies - TranslucentFrontLayer and the material preview set it
    // beside a specific mode - while at most ONE of the four specific modes may be
    // set: the PSO picks the blend by ADDITIVE > PREMULTIPLIED > MULTIPLY >
    // TRANSMISSIVE and custom_surface.hlsli its colour path by ADDITIVE > MULTIPLY >
    // PREMULTIPLIED > TRANSMISSIVE, so two at once would draw with a blend the shader
    // was not written for.
    bool LegalBlend() const noexcept
    {
        const uint32_t m = bits & kBlendModes;
        return (m & (m - 1u)) == 0;
    }
    // ALPHA_BLEND dropped where a specific mode already implies it. Applied by
    // ShaderLibrary::KeyBits only to a static shader that names the mode - one
    // that reads ALPHA_BLEND only as the forward switch (gen_perm_relevance.py
    // --verify-dxil checks the two spellings compile alike) - so both share one
    // cached shader.
    PermutationKey CanonicalBlend() const noexcept
    {
        PermutationKey k = *this;
        if (k.bits & kBlendModes) k.bits &= ~ALPHA_BLEND;
        return k;
    }

    bool operator==(const PermutationKey& o) const noexcept { return bits == o.bits; }
    bool operator!=(const PermutationKey& o) const noexcept { return bits != o.bits; }
};

// One row per bit, in PERMUTATION_KEY_BITS order: the bit and the define name
// it compiles as. ShaderLibrary::MakeDefines emits "-D <name>=0|1" for each.
struct PermutationDefine
{
    uint32_t    bit;
    const char* name;
};

inline constexpr PermutationDefine kPermutationDefines[] = {
#define PERMUTATION_KEY_DEFINE_ROW(Constant, Bit, Define) { PermutationKey::Constant, Define },
    PERMUTATION_KEY_BITS(PERMUTATION_KEY_DEFINE_ROW)
#undef PERMUTATION_KEY_DEFINE_ROW
};

namespace PermutationKeyDetail
{
    constexpr uint32_t kRetiredBits = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 12);

    constexpr bool RowsAreDistinctAndLive()
    {
        uint32_t seen = 0;
        for (const PermutationDefine& d : kPermutationDefines)
        {
            if (d.bit == 0 || (d.bit & (d.bit - 1)) != 0) return false;   // exactly one bit
            if (seen & d.bit) return false;                               // no duplicates
            if (d.bit & kRetiredBits) return false;                       // no retired bit
            seen |= d.bit;
        }
        return true;
    }
    static_assert(RowsAreDistinctAndLive(),
                  "PERMUTATION_KEY_BITS: every row needs its own single, non-retired bit");
}
