#pragma once

// Shared-template material for the clustered decal system, Unreal-style channels (a
// grayscale map reads .r):
//   BaseColor    - albedo -> GBuffer.Albedo.
//   Normal       - tangent-space normal map, blended into GBuffer.Normal by Reoriented
//                  Normal Mapping (RNM).
//   Opacity      - mask multiplying the decal alpha; without it BaseColor's alpha is the mask.
//   Roughness    -> GBuffer.Surface.r.
//   Specular     - dielectric reflectance -> GBuffer.Surface.a.
//   AO           -> GBuffer.Surface.b.
//   Bump         - small-scale height; its derivative perturbs the normal, RNM-combined
//                  with Normal.
//   Cavity       - darkens BaseColor: 1 -> no change, 0 -> darkened by cavityStrength.
//   Displacement - parallax UV shift before all other samples; displacementScale sets
//                  its magnitude (0 = off).
// Each slot has a scalar that multiplies its texture, or is the raw constant when the slot
// is empty (no roughness texture + roughness = 0.2 -> a flat 0.2 wherever the decal lands).
// Ownership: DecalMaterialLibrary and every DecalComponent hold shared_ptr<DecalMaterialAsset>.
// Textures are acquired per asset, so components sharing one asset share one set of refs.

#include "Resource/SystemHandles.h"
#include "Resource/MaterialDomain.h"

#include <DirectXMath.h>
#include <cstdint>
#include <string>

class IGraphicsDevice;

namespace Resource
{
    class TextureSystem;
    class ResourceManager;

    class DecalMaterialAsset
    {
    public:
        // ---- Texture slots (Unreal-aligned order) ----
        enum TextureSlot : uint32_t
        {
            SLOT_BASECOLOR    = 0,
            SLOT_NORMAL       = 1,
            SLOT_OPACITY      = 2,
            SLOT_ROUGHNESS    = 3,
            SLOT_SPECULAR     = 4,
            SLOT_AO           = 5,
            SLOT_BUMP         = 6,
            SLOT_CAVITY       = 7,
            SLOT_DISPLACEMENT = 8,
            SLOT_COUNT        = 9,
        };

        // ---- Write-channel flags (the GBuffer channels this decal overwrites) ----
        // Only BaseColor writes by default; any other channel writes only when its WRITE_* flag
        // is set, so an unassigned slot's scalar default (roughness 0.5) never clobbers the surface.
        enum FLAGS : uint32_t
        {
            WRITE_BASECOLOR = 1 << 0,
            WRITE_NORMAL    = 1 << 1,
            WRITE_ROUGHNESS = 1 << 2,
            WRITE_SPECULAR  = 1 << 3,
            WRITE_AO        = 1 << 4,
            DEFAULT         = WRITE_BASECOLOR,
            // Every PBR channel, for a material authored with the full 9-slot set (e.g. one
            // created in code).
            ALL_PBR         = WRITE_BASECOLOR | WRITE_NORMAL | WRITE_ROUGHNESS
                            | WRITE_SPECULAR  | WRITE_AO,
        };

        enum class NormalBlendMode : uint32_t
        {
            RNM  = 0,   // Reoriented Normal Mapping (default; recommended)
            Lerp = 1,   // Plain linear blend (flat on sloped surfaces)
        };

        // ---- Material domain ----
        MaterialDomain GetDomain() const { return MaterialDomain::Decal; }

        // ---- Lifetime and texture slots ----
        DecalMaterialAsset()
        {
            for (uint32_t i = 0; i < SLOT_COUNT; ++i)
            {
                texHandles[i]  = kInvalidTextureHandle;
                texBindless[i] = -1;
            }
        }
        ~DecalMaterialAsset() = default;
        DecalMaterialAsset(const DecalMaterialAsset&)            = delete;
        DecalMaterialAsset& operator=(const DecalMaterialAsset&) = delete;

        void Resolve(TextureSystem&   texSys,
                     ResourceManager& resMgr,
                     IGraphicsDevice& gfx);
        bool TryPromote(TextureSystem& texSys) const;
        void Release(TextureSystem& texSys, IGraphicsDevice& gfx);
        bool IsFullyResolved() const;
        void SetTexturePath(TextureSlot       slot,
                            const std::string& newPath,
                            TextureSystem&    texSys,
                            IGraphicsDevice&  gfx);

        // Human-readable slot label (for editor UI + serializer keys).
        static const char* SlotLabel(TextureSlot slot);
        // Canonical JSON key (lowercase, stable across UI label changes).
        static const char* SlotKey(TextureSlot slot);

        // ---- Identity (serialization-facing) ----
        std::string name;

        // ---- Editor parameters ----
        std::string texPaths[SLOT_COUNT];

        // Multiplies the sampled BaseColor RGB. .w multiplies the decal alpha, with the
        // opacity scalar and the opacity texture.
        DirectX::XMFLOAT4 baseColorTint = { 1.f, 1.f, 1.f, 1.f };

        // Scalars: each multiplies the matching texture when present, or is the raw
        // constant when the texture is absent.
        float opacity          = 1.0f;   // overall alpha gain
        float roughness        = 0.5f;   // flat-value / multiplier
        float specular         = 0.5f;   // dielectric F0 factor
        float ao               = 1.0f;
        float normalStrength   = 1.0f;   // scales normal map XY before reconstruction
        // Bump slope multiplier per UV unit: 0 = flat, 0.1 = subtle, 1 = physical 1:1
        // height -> normal, 2+ = exaggerated.
        float bumpStrength     = 0.1f;
        float cavityStrength   = 1.0f;   // 0 = ignore cavity, 1 = full darkening
        float displacementScale = 0.0f;  // parallax UV offset magnitude; 0 = off

        uint32_t        flags           = DEFAULT;
        NormalBlendMode normalBlendMode = NormalBlendMode::RNM;
        float           angleFadeStart  = 0.3f;
        int32_t         sortLayer       = 0;

        // ---- Runtime caches (filled by Resolve + TryPromote) ----
        mutable TextureHandle texHandles[SLOT_COUNT];
        mutable int32_t       texBindless[SLOT_COUNT];
    };
}
