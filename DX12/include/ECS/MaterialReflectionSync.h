#pragma once

// MaterialReflectionSync - bridges ShaderReflect::Reflection (the shader's declared resource bindings
// + cbuffer variables) to MaterialComponent's per-name customTextures / customParams stores.
//
// The renderer calls Sync after a material's custom shader resolves, gated per stage by the stamp
// below: missing keys get defaults, present keys are left alone. Stale entries (absent from the
// shader) are kept silently so a transient compile failure does not drop the user's tweaks.
//
//   Texture binding -> customTextures: a ShaderReflect::ResourceType::Texture at space3.
//   Custom param -> customParams: a var of the b8 space0 cbuffer that is NOT one of the
//   engine-reserved buffers (PerView, MaterialBuffer, MeshDescriptors, ...), which are bound
//   globally by the GBuffer pass and must not show up as user knobs.
//
// The reserved name lists live in the .cpp; add new engine cbuffers there.

#include "ECS/Components.h"
#include "Graphics/ShaderReflection.h"

namespace MaterialReflectionSync
{
    // Adds defaults for any reflected texture binding / cbuffer var not yet
    // in the material's maps. Returns the number of new entries added so the
    // caller can decide whether to flag the material dirty.
    std::size_t Sync(MaterialComponent& m, const ShaderReflect::Reflection& r);

    // True when the cbuffer is bound globally by the engine (PerView, etc.)
    // and therefore should NOT surface as a per-material parameter.
    bool IsReservedCBuffer(const char* name);

    // True when the texture binding is populated by the engine (per-draw
    // fallback slots, bindless array). Custom-shader authors reusing these
    // names get the engine-bound values transparently; the inspector must
    // NOT offer them as user-assignable slots.
    bool IsReservedTexture(const char* name);

    // ---- Per-frame amortization ----
    // Sync() is idempotent but walks every reflected var through string maps; run per material per
    // FRAME it would be the largest constant string cost in scene build. MaterialComponent::reflSyncStamp
    // gates it to once per (shader paths + variant, ShaderLibrary reload epoch, stage): a shader path
    // edit re-syncs because the hash half changes, a hot reload because the epoch half changes, and PS/VS
    // stamp independently so a still-compiling stage keeps retrying without blocking the other.

    inline uint32_t ShaderPathHash32(const MaterialComponent& m)
    {
        uint32_t h = 2166136261u;
        for (const char c : m.customShaderPath)       { h ^= uint8_t(c); h *= 16777619u; }
        h ^= uint8_t('|'); h *= 16777619u;
        for (const char c : m.customVertexShaderPath) { h ^= uint8_t(c); h *= 16777619u; }
        // ...and what picks the variant the reflection merges in (Renderer's MaterialShaderReflection): a
        // blend-mode, alpha-test, double-sided or water change re-syncs too.
        const uint32_t variant = static_cast<uint32_t>(m.userBlendMode)
                               | (m.IsAlphaTestEnabled() ? 0x100u : 0u)
                               | ((m.flags & MaterialComponent::DOUBLE_SIDED) ? 0x200u : 0u)
                               | (m.IsWaterShadingModel() ? 0x400u : 0u);
        for (int i = 0; i < 4; ++i) { h ^= uint8_t(variant >> (8 * i)); h *= 16777619u; }
        return h;
    }

    inline uint64_t StampBase(const MaterialComponent& m, uint32_t reloadEpoch)
    {
        return (uint64_t(ShaderPathHash32(m)) << 32)
             | (uint64_t(reloadEpoch & 0x3FFFFFFFu) << 2);
    }

    // True when this stage already synced against the CURRENT paths + epoch.
    inline bool StageSynced(const MaterialComponent& m, uint32_t reloadEpoch, bool vsStage)
    {
        const uint64_t base = StampBase(m, reloadEpoch);
        if ((m.reflSyncStamp & ~3ull) != base) return false;
        return (m.reflSyncStamp & (vsStage ? 2ull : 1ull)) != 0;
    }

    // Record a completed Sync for one stage; a paths/epoch change resets the
    // other stage's bit implicitly (fresh base).
    inline void MarkStageSynced(MaterialComponent& m, uint32_t reloadEpoch, bool vsStage)
    {
        const uint64_t base = StampBase(m, reloadEpoch);
        if ((m.reflSyncStamp & ~3ull) != base) m.reflSyncStamp = base;
        m.reflSyncStamp |= (vsStage ? 2ull : 1ull);
    }
}
