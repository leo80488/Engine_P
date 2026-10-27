#include "ECS/MaterialReflectionSync.h"

#include "System/Log.h"

#include <cstring>
#include <cmath>
#include <string_view>

// Reflection-sync tracing, off by default: Sync() can run for every custom-shader material
// (Renderer::ResolveCustomPSID / VSID), so dumping the full reflection per call would spam
// DX12Log.txt and cost frame-build time. Define MATERIAL_REFLECTION_SYNC_VERBOSE=1 to trace binding /
// param resolution. The one-shot "+ added / ~ migrated" lines stay unconditional: they fire only
// when a param / texture is first resolved.
#ifndef MATERIAL_REFLECTION_SYNC_VERBOSE
#define MATERIAL_REFLECTION_SYNC_VERBOSE 0
#endif
#if MATERIAL_REFLECTION_SYNC_VERBOSE
    #define MRS_TRACE(...) LOG_INFO(__VA_ARGS__)
#else
    #define MRS_TRACE(...) ((void)0)
#endif

namespace MaterialReflectionSync
{
namespace
{
    // CBuffers bound by the engine's GBuffer pass: they must NEVER surface as editable per-material
    // params. Names match the HLSL `cbuffer` declarations in shaders/GBuffer.ps.hlsl + shared headers.
    constexpr const char* kReservedCBuffers[] = {
        "PerViewCB",         // b1 space0: the engine view CB (the real reflected
                             // name; custom_vertex.hlsli + GBuffer.vs.hlsl declare it).
                             // Must be filtered so its members (gTimeParams/cameraPos)
                             // don't leak into custom params and it can't be mistaken
                             // for the user's b8 cbuffer.
        "PerView",           // alternate name, filtered too
        "PerObject",
        "MaterialBuffer",
        "MeshDescriptors",
        "InstanceBuffer",
        "PushConstants",     // auto-filled per-draw (meshDescIdx / materialIndex / ...)
        "LightCB",           // b2 (GBuffer custom hooks) / b1 (deferred): sun/camera/
                             // shadow/IBL state; custom NPR hooks pull it in via
                             // light_cb.hlsli. Must never surface as editable params.
        "$Globals",          // unnamed cbuffer: usually our root constants
        "DynamicSamplerCB",  // b0 space9: the run-time sampler's heap index
                             // (sampler_heap.hlsli); every material shader
                             // reads it through g_LinearWrap.
    };

    // Texture bindings the engine binds globally in GBufferPass::BindGlobals. Surfacing them as "Custom
    // Textures" would mislead: the values come from the standard Material TextureMap slots
    // (MATERIAL_TEXTURE_SLOTS), not from the custom-shader texture list.
    constexpr const char* kReservedTextures[] = {
        "g_BaseColor",        // t3 space0, per-draw fallback
        "g_SurfaceMap",       // t4 space0, per-draw fallback
        "g_NormalMap",        // t5 space0, per-draw fallback
        "g_AllTextures",      // t0 space2, bindless array
        "g_LinearWrap",       // sampler: already filtered by type, listed for documentation
    };

    bool IsReservedCBufferName(std::string_view name)
    {
        for (const char* r : kReservedCBuffers)
            if (name == r) return true;
        return false;
    }

    bool IsReservedTextureName(std::string_view name)
    {
        for (const char* r : kReservedTextures)
            if (name == r) return true;
        return false;
    }

    bool ArrEq(const std::array<float, 4>& a, const std::array<float, 4>& b)
    {
        for (int i = 0; i < 4; ++i)
            if (std::fabs(a[i] - b[i]) > 1e-5f) return false;
        return true;
    }

    uint8_t ComponentCountOfType(ShaderReflect::VarType t)
    {
        using V = ShaderReflect::VarType;
        switch (t)
        {
            case V::Float2: case V::Int2: case V::UInt2: return 2;
            case V::Float3: case V::Int3: case V::UInt3: return 3;
            case V::Float:  case V::Int:  case V::UInt:
            case V::Bool:                                return 1;
            default:                                     return 4;
        }
    }

    // float-bag default by reflected scalar/vector type. Identity-ish: 0 for
    // most things, 1 for float4 alpha so a pure-write of an unset color slot
    // doesn't render fully transparent by accident.
    std::array<float, 4> DefaultForType(ShaderReflect::VarType t)
    {
        using V = ShaderReflect::VarType;
        switch (t)
        {
            case V::Float4:   return { 1.f, 1.f, 1.f, 1.f };
            case V::Float3:   return { 1.f, 1.f, 1.f, 0.f };
            case V::Float2:   return { 0.f, 0.f, 0.f, 0.f };
            case V::Float:    return { 0.f, 0.f, 0.f, 0.f };
            case V::Bool:     return { 0.f, 0.f, 0.f, 0.f };
            case V::Int: case V::Int2: case V::Int3: case V::Int4:
            case V::UInt: case V::UInt2: case V::UInt3: case V::UInt4:
                              return { 0.f, 0.f, 0.f, 0.f };
            default:          return { 0.f, 0.f, 0.f, 0.f };
        }
    }
} // namespace

std::size_t Sync(MaterialComponent& m, const ShaderReflect::Reflection& r)
{
    std::size_t added = 0;

    MRS_TRACE("MaterialReflectionSync::Sync: reflection has %zu bindings, %zu cbuffers",
              r.bindings.size(), r.cbuffers.size());

    for (const ShaderReflect::ResourceBinding& b : r.bindings)
    {
        MRS_TRACE("  binding [%s] '%s' bind=%u space=%u count=%u %s",
                  ShaderReflect::ToString(b.type), b.name.c_str(),
                  b.bindPoint, b.space, b.bindCount,
                  (b.type == ShaderReflect::ResourceType::Texture
                   && IsReservedTextureName(b.name)) ? "(RESERVED, skipped)" : "");

        if (b.type != ShaderReflect::ResourceType::Texture) continue;
        if (b.name.empty())              continue;
        if (IsReservedTextureName(b.name)) continue;   // engine-bound, not user-owned
        // Custom textures live ONLY in the per-material table at space3 (root 36).
        // Filter by register so engine textures pulled in by a shared header
        // (e.g. IBL cubes at space0) can never leak in, regardless of name.
        if (b.space != 3u)               continue;

        if (m.customTextures.find(b.name) == m.customTextures.end())
        {
            MaterialComponent::TextureMap t;     // empty path, runtime handles -1/0
            m.customTextures.emplace(b.name, std::move(t));
            ++added;
            LOG_INFO("    + customTextures['%s'] added", b.name.c_str());
        }
    }

    for (const ShaderReflect::CBufferLayout& cb : r.cbuffers)
    {
        const bool reserved = IsReservedCBufferName(cb.name);
        // Only the per-material custom CBV slot (b8 space0, root 35) holds user params. Engine CBs at any
        // other register (PerViewCB@b1, LightCB@b2, ...) must never leak into customParams even when a shared
        // header declares them: filter by register, not just by name, as the CBV packer does.
        const bool wrongSlot = (cb.bindPoint != 8u || cb.space != 0u);
        MRS_TRACE("  cbuffer '%s' bind=%u space=%u size=%uB vars=%zu %s",
                  cb.name.c_str(), cb.bindPoint, cb.space, cb.sizeBytes, cb.vars.size(),
                  (reserved || wrongSlot) ? "(RESERVED, skipped)" : "");
        if (reserved || wrongSlot) continue;

        for (const ShaderReflect::CBufferVar& v : cb.vars)
        {
            if (v.name.empty()) continue;
            if (!v.used) continue;                                        // declared but never read by this stage: no widget/seed
            if (v.name[0] == '_') continue;                               // authoring pads (_pad0 ...): never editable/persisted
            if (v.type == ShaderReflect::VarType::Struct) continue;       // no nested-struct UI
            if (v.type == ShaderReflect::VarType::Float4x4) continue;     // matrices not supported
            if (v.elements > 0) continue;                                 // arrays not supported

            // Real default = the shader's //@default annotation, else the type-based sentinel.
            const std::array<float, 4> sentinel = DefaultForType(v.type);
            const std::array<float, 4> defVal   = v.hasDefault ? v.defaultValue : sentinel;

            // Refresh the reflected metadata every Sync (defaults/ranges can change
            // when the shader is edited + hot-reloaded). Runtime-only; not serialized.
            MaterialComponent::CustomParamMeta& meta = m.customParamMeta[v.name];
            meta.defaultValue  = defVal;
            meta.minValue      = v.minValue;
            meta.maxValue      = v.maxValue;
            meta.hasRange      = v.hasRange;
            meta.label         = v.label;
            meta.numComponents = ComponentCountOfType(v.type);
            meta.isTextureSlot = (v.name.rfind("Tex_", 0) == 0);
            meta.isBool        = (v.type == ShaderReflect::VarType::Bool);
            meta.group         = v.group;
            // Declaration-order sort key: the cbuffer byte offset (identical
            // in PS and VS reflections of a shared b8 layout).
            meta.sortOrder     = static_cast<uint16_t>(
                v.offset < 0xFFFEu ? v.offset : 0xFFFEu);

            // "Tex_*" b8 params are texture SLOTS (VFX convention): the packed
            // value is a bindless index the owning system resolves from a path.
            // Seed the PATH store instead of the float bag so the inspector
            // shows a drag-drop field and no bogus tunable is serialized.
            if (meta.isTextureSlot)
            {
                if (m.customTextures.find(v.name) == m.customTextures.end())
                {
                    m.customTextures.emplace(v.name, MaterialComponent::TextureMap{});
                    ++added;
                    LOG_INFO("    + customTextures['%s'] added (Tex_ b8 slot)", v.name.c_str());
                }
                continue;
            }

            auto it = m.customParams.find(v.name);
            if (it == m.customParams.end())
            {
                m.customParams.emplace(v.name, defVal);   // seed with the REAL default
                ++added;
                LOG_INFO("    + customParams['%s'] added (type=%s, %s default)",
                         v.name.c_str(), ShaderReflect::ToString(v.type),
                         v.hasDefault ? "annotated" : "type-based");
            }
            else if (m.customParamsLegacyFormat && v.hasDefault && ArrEq(it->second, sentinel) &&
                     !ArrEq(defVal, sentinel))
            {
                // Old-format .imat (customParamsLegacyFormat): a stored value still at the type-based sentinel
                // meant "untouched" (the FaceUntouched convention); adopt the real annotated default so the look
                // survives once the shader drops its ternaries. Values the user changed (!= sentinel) are kept.
                // A default equal to the sentinel has nothing to migrate: matching it would log the no-op on
                // every Sync while the legacy flag is set.
                it->second = defVal;
                LOG_INFO("    ~ customParams['%s'] migrated sentinel→default", v.name.c_str());
            }
        }
    }

    // NOTE: the customParamsLegacyFormat flag is intentionally NOT cleared here.
    // Sync runs once per stage (PS then VS) and the user cbuffer may exist in only
    // ONE stage's reflection (DXC strips params the other stage doesn't read), so
    // clearing here would let PS-sync drop the flag before VS-sync migrates a
    // vertex-only param. The caller (Renderer::ResolveCustomVSID) clears it once,
    // after BOTH stages have synced.

    MRS_TRACE("MaterialReflectionSync::Sync: total entries added = %zu, "
              "customTextures.size=%zu, customParams.size=%zu",
              added, m.customTextures.size(), m.customParams.size());

    // A reflection add changes the material's authored layout (new customTextures change the runtime
    // key; new customParams change the packed CBV). This runs during draw resolution, AFTER
    // BuildScene_SyncMaterials cleared the frame's dirty flag, so mark dirty here or the dirty-gated
    // material sync would skip the change and render a stale binding. Sync adds only entries the maps
    // lack (find-guarded), so this fires only when the shader brings a new name, not every call.
    if (added > 0) m.SetDirty();

    return added;
}

bool IsReservedCBuffer(const char* name)
{
    return name && IsReservedCBufferName(name);
}

bool IsReservedTexture(const char* name)
{
    return name && IsReservedTextureName(name);
}

} // namespace MaterialReflectionSync
