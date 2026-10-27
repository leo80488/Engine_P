#pragma once

// MaterialDomain - which pipeline executes a material. Domains share the texture
// and BindlessRegistry layers and the editor UI shell, but keep their own GPU
// layouts and shader / PSO paths (no union "MaterialGPU"). The editor routes
// inspector panels by it and asset saves tag serialized files with it.
//
//   Opaque      - per-material PSO, GBuffer output; MaterialComponent authored
//                 data + a renderer-owned runtime resource.
//   Transparent - forward TransparentPass. Nothing routes on this value (the
//                 editor offers only Surface and Decal): translucent materials
//                 reach that pass through MaterialComponent's BlendMode on the
//                 Opaque domain.
//   Decal       - compute-based clustered decals: no PSO, bindless. Driven by a
//                 MaterialComponent with this domain (.imat) or a shared
//                 DecalMaterialAsset (many DecalInstances -> one asset).
//   PostProcess - reserved: screen-space effects.

#include <cstdint>

namespace Resource
{
    enum class MaterialDomain : uint32_t
    {
        Opaque      = 0,
        Transparent = 1,
        Decal       = 2,
        PostProcess = 3,

        // Water is NOT a domain: it is a SHADING MODEL on an ordinary translucent Surface
        // material (MaterialComponent::IsWaterShadingModel(), ShadingModel::Water). As in
        // Unreal, a domain says which pipeline compiles the material, not how the surface
        // is shaded.

        Count            // keep last: bounds check only, never serialized
    };

    // Deserialization guard. Every .imat / .iscene / .ipfb reader casts a raw int
    // straight into the enum; an out-of-range value (a newer build's file, a corrupted
    // line) would be a domain nothing handles - an invisible mesh with no diagnostic.
    // Readers fall back to Opaque, which is always drawable.
    constexpr bool IsValidMaterialDomain(uint32_t raw)
    {
        return raw < static_cast<uint32_t>(MaterialDomain::Count);
    }
}
