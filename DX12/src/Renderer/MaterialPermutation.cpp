#include "Renderer/MaterialPermutation.h"

#include "ECS/Components.h"
#include "Graphics/ShaderLibrary.h"

void ApplyMaterialPermutation(const MaterialComponent& mc, PermutationKey& perm)
{
    // No HAS_NORMALMAP / HAS_EMISSIVE bits: no shader tests either macro, so they
    // would only split the shader + PSO caches into variants with byte-identical
    // bytecode.
    perm.Set(PermutationKey::DOUBLE_SIDED, (mc.flags & MaterialComponent::DOUBLE_SIDED) != 0);

    switch (mc.userBlendMode)
    {
    case BlendMode::Alpha:
        perm.Set(PermutationKey::ALPHA_BLEND, true);
        // The forward draw clips texels below alphaRef. Shadow maps do not follow
        // it: forward-translucent packets never cast (ApplyBlendMode).
        if (mc.IsAlphaTestEnabled())
            perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    case BlendMode::Additive:
        perm.Set(PermutationKey::ADDITIVE_BLEND, true);
        break;
    case BlendMode::Premultiplied:
        perm.Set(PermutationKey::PREMULTIPLIED_BLEND, true);
        if (mc.IsAlphaTestEnabled())
            perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    case BlendMode::Multiply:
        perm.Set(PermutationKey::MULTIPLY_BLEND, true);
        break;
    case BlendMode::Transmissive:
        perm.Set(PermutationKey::TRANSMISSIVE_BLEND, true);
        if (mc.IsAlphaTestEnabled())
            perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    case BlendMode::Masked:
        // Unreal-style Masked: unconditional clip against alphaRef using
        // baseAlpha x OPACITYMAP.r (custom_surface ALPHA_TEST block). Shadow
        // passes pick the ALPHA_TEST bucket from the same bit.
        perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    case BlendMode::Dithered:
        // The same ALPHA_TEST permutation as Masked; the clip block selects the
        // temporal-dither threshold from MAT_FLAG_DITHERED_COVERAGE.
        perm.Set(PermutationKey::ALPHA_TEST, true);
        break;
    default: // BlendMode::Opaque
        // IsAlphaTestEnabled(): alphaRef strict enough to discard (< full-opaque threshold).
        perm.Set(PermutationKey::ALPHA_TEST, mc.IsAlphaTestEnabled());
        break;
    }

    // Water (Unreal "Single Layer Water") keeps whatever blend the author
    // picked and redirects the draw to WaterSurfacePass's variant. ALPHA_TEST
    // is meaningless for water and would compile the masked clip block into a
    // shader whose opacity is the shoreline fade.
    if (mc.IsWaterShadingModel())
    {
        perm.Set(PermutationKey::WATER_SURFACE, true);
        perm.Set(PermutationKey::ALPHA_TEST, false);
    }
}

PermutationKey MaterialDrawPermutation(const MaterialComponent& mc)
{
    PermutationKey perm;
    ApplyMaterialPermutation(mc, perm);
    return perm;
}

const ShaderReflect::Reflection* MaterialShaderReflection(ShaderLibrary& lib, uint32_t dynId,
                                                          const MaterialComponent& mc)
{
    if (const ShaderReflect::Reflection* iface = lib.GetDynamicInterface(dynId)) return iface;
    return lib.GetDynamicReflectionUnion(dynId, { PermutationKey{}, MaterialDrawPermutation(mc) });
}

const ShaderReflect::Reflection* MaterialShaderInterface(ShaderLibrary& lib, uint32_t dynId)
{
    if (const ShaderReflect::Reflection* iface = lib.GetDynamicInterface(dynId)) return iface;
    return lib.GetDynamicReflection(dynId);
}
