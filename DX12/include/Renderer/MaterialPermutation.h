#pragma once

#include "Graphics/PermutationKey.h"

#include <cstdint>

struct MaterialComponent;
class ShaderLibrary;
namespace ShaderReflect { struct Reflection; }

// The permutation a material's own draw is compiled with, and the interface a
// custom material is synced, packed and inspected from.

// Applies to @p perm the bits the material decides - DOUBLE_SIDED, the blend
// route (ALPHA_BLEND / ADDITIVE / PREMULTIPLIED / MULTIPLY_BLEND), ALPHA_TEST
// and the water redirect (WATER_SURFACE, ALPHA_TEST off). Renderer::ApplyBlendMode
// calls it on a draw candidate: a bit the caller set before (a secondary draw's
// ALPHA_TEST) stays set unless a branch here writes it false (Opaque, water).
void ApplyMaterialPermutation(const MaterialComponent& mc, PermutationKey& perm);

// The same, from an empty key: the variant the material's custom shader draws with.
PermutationKey MaterialDrawPermutation(const MaterialComponent& mc);

// The interface of a custom material's shader. Its SOURCE DECLARATIONS when
// they parse exactly (ShaderLibrary::GetDynamicInterface): nothing compiles,
// and every member and texture the shader declares is in it - read by any
// variant or not. Otherwise permutation 0 merged with the variant the material
// draws with (ShaderLibrary::GetDynamicReflectionUnion): a texture or b8 member
// only that variant reads - an ALPHA_TEST mask, a forward-only texture, a water
// parameter - gets its Inspector row, its seeded default and its binding, which
// permutation 0 alone would drop. Every reader of a custom material's interface
// goes through here (or MaterialShaderInterface) so they agree.
const ShaderReflect::Reflection* MaterialShaderReflection(ShaderLibrary& lib, uint32_t dynId,
                                                          const MaterialComponent& mc);

// The same for a shader id without its material: the declarations, else
// permutation 0's reflection.
const ShaderReflect::Reflection* MaterialShaderInterface(ShaderLibrary& lib, uint32_t dynId);
