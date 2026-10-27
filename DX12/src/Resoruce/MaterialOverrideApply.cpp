#include "Resource/MaterialOverrideApply.h"

#include "ECS/Components.h"
#include "ECS/HierarchyComponents.h"
#include "ECS/MaterialSchema.h"
#include "Resource/AssetRefs.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialDomain.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Resource
{
    // ---- ParseMaterialOverrideKV ----
    // The grammar is the writer's (ComponentSerializers):
    //     <PercentEncode(key)>=<typeChar>:<value>
    //     f:<float>  v:<x>_<y>_<z>_<w>  i:<int>  b:<0|1>  t:<PercentEncode(path)>
    void ParseMaterialOverrideKV(const std::unordered_map<std::string, std::string>& kv,
                                 MaterialOverride& out)
    {
        for (const auto& [rawKey, rawVal] : kv)
        {
            const auto colon = rawVal.find(':');
            if (colon == std::string::npos || rawVal.empty()) continue;
            const char        typeChar = rawVal[0];
            const std::string valStr   = rawVal.substr(colon + 1);
            const std::string key      = AssetRefs::PercentDecode(rawKey);
            try
            {
                switch (typeChar)
                {
                case 'f':
                    out.Set(key, MaterialOverride::Value::MakeFloat(std::stof(valStr)));
                    break;
                case 'v':
                {
                    float x = 0.f, y = 0.f, z = 0.f, w = 0.f;
                    // Keep sscanf_s: the files on disk are written for exactly this parse.
                    sscanf_s(valStr.c_str(), "%f_%f_%f_%f", &x, &y, &z, &w);
                    out.Set(key, MaterialOverride::Value::MakeVec4(x, y, z, w));
                    break;
                }
                case 'i':
                    out.Set(key, MaterialOverride::Value::MakeInt(std::stoi(valStr)));
                    break;
                case 'b':
                    out.Set(key, MaterialOverride::Value::MakeBool(valStr != "0"));
                    break;
                case 't':
                    out.Set(key, MaterialOverride::Value::MakeTex(
                                     AssetRefs::PercentDecode(valStr)));
                    break;
                default:
                    break;
                }
            }
            catch (...) {}
        }
    }


    // ---- BuildMaterialOverride ----
    // Diff a LIVE MaterialComponent against the material FILE it came from into an
    // override bag (contract at the declaration). The save path needs it: SavePrefab /
    // SaveScene auto-create a .imat only for an entity with NO MaterialSourcePath, and a
    // part spawned from an F-line with `mat=` always has one, so without the diff only
    // "MaterialRef: path=" is written and unsaved edits vanish with no log.
    bool BuildMaterialOverride(const MaterialComponent& mat,
                               const std::string&       sourcePath,
                               MaterialOverride&        out)
    {
        out.props.clear();
        if (sourcePath.empty()) return false;

        MaterialComponent baseMat{};
        if (!LoadMaterial(sourcePath, baseMat))
        {
            LOG_ERROR("BuildMaterialOverride: base material '%s' failed to load — refusing "
                      "to build an override (it would be a default snapshot, not a diff).",
                      sourcePath.c_str());
            return false;
        }

        out.props.clear();
        MaterialOverride& ovr = out;

        auto fne = [](float a, float b) { return std::fabsf(a - b) > 1e-5f; };

        // Schema-driven diff: every editable param row that differs from the on-disk
        // baseline, so a field added to kMaterialPBRSchema is covered automatically.
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
        {
            // FloatComponent is a UI alias of its parent color (e.g. emissiveStrength ==
            // emissiveColor.w); the parent row covers it.
            if (f.kind == MaterialSchema::Kind::FloatComponent) continue;

            const float* cur  = MaterialSchema::FieldPtr(mat,     f);
            const float* base = MaterialSchema::FieldPtr(baseMat, f);
            switch (f.kind)
            {
            case MaterialSchema::Kind::Float:
                if (fne(cur[0], base[0]))
                    ovr.Set(f.name, MaterialOverride::Value::MakeFloat(cur[0]));
                break;
            case MaterialSchema::Kind::Color3:
            case MaterialSchema::Kind::Color4:
                if (fne(cur[0], base[0]) || fne(cur[1], base[1]) ||
                    fne(cur[2], base[2]) || fne(cur[3], base[3]))
                    ovr.Set(f.name, MaterialOverride::Value::MakeVec4(
                        cur[0], cur[1], cur[2], cur[3]));
                break;
            default: break;
            }
        }

        // outlinePixels is a meta field (not in the param schema): diff it explicitly.
        if (fne(mat.outlinePixels, baseMat.outlinePixels))
            ovr.Set("outlinePixels", MaterialOverride::Value::MakeFloat(mat.outlinePixels));

        // blendMode / shaderType are meta fields too; ApplyMaterialOverride reads both
        // keys (e.g. one instance flipped to Masked).
        if (mat.userBlendMode != baseMat.userBlendMode)
            ovr.Set("blendMode", MaterialOverride::Value::MakeInt(
                static_cast<int>(mat.userBlendMode)));
        if (mat.shaderType != baseMat.shaderType)
            ovr.Set("shaderType", MaterialOverride::Value::MakeInt(
                static_cast<int>(mat.shaderType)));
        // customShadingModel, the .imat key: one instance can be flipped to e.g.
        // Subsurface.
        if (mat.customShadingModel != baseMat.customShadingModel)
            ovr.Set("customShadingModel", MaterialOverride::Value::MakeInt(
                static_cast<int>(mat.customShadingModel)));
        // Boolean flags. Keep this list in step with MAT_OVR_FLAGS_ in
        // ApplyMaterialOverride.
    #define DIFF_FLAG_OVR_(key, accessor)                                          \
        if ((mat.accessor) != (baseMat.accessor))                  \
            ovr.Set(key, MaterialOverride::Value::MakeBool(mat.accessor));
        DIFF_FLAG_OVR_("castShadow",        IsCastingShadow())
        DIFF_FLAG_OVR_("receiveShadow",     IsReceiveShadow())
        DIFF_FLAG_OVR_("doubleSided",       IsDoubleSided())
        DIFF_FLAG_OVR_("outline",           IsOutlineEnabled())
        DIFF_FLAG_OVR_("outlineScreenSpace", IsOutlineScreenSpaceEnabled())
        DIFF_FLAG_OVR_("useVertexColor",    IsUsingVertexColors())
        DIFF_FLAG_OVR_("hairMask",          IsHairMaskEnabled())
        DIFF_FLAG_OVR_("nprToonShadowEdge", IsNprToonShadowEdgeEnabled())
        DIFF_FLAG_OVR_("sourceOutlineExempt", IsSourceOutlineExempt())
        DIFF_FLAG_OVR_("nprClampEnabled",   IsNprBrightnessClampEnabled())
    #undef DIFF_FLAG_OVR_
        {
            const bool a = (mat.flags     & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
            const bool b = (baseMat.flags & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
            if (a != b) ovr.Set("excludeFromSsao", MaterialOverride::Value::MakeBool(a));
        }

        // Domain + decal-authored params (Decal domain, see MaterialDomain.h).
        if (mat.domain != baseMat.domain)
            ovr.Set("domain", MaterialOverride::Value::MakeInt(
                static_cast<int>(mat.domain)));
        if (mat.decalWriteMask != baseMat.decalWriteMask)
            ovr.Set("decalWriteMask", MaterialOverride::Value::MakeInt(
                static_cast<int>(mat.decalWriteMask)));
        if (fne(mat.decalAngleFadeStart, baseMat.decalAngleFadeStart))
            ovr.Set("decalAngleFade", MaterialOverride::Value::MakeFloat(
                mat.decalAngleFadeStart));
        if (mat.decalSortLayer != baseMat.decalSortLayer)
            ovr.Set("decalSortLayer", MaterialOverride::Value::MakeInt(
                mat.decalSortLayer));

        // UV transform: texMulAdd is a float4 with a hand-written widget, not a
        // kMaterialPBRSchema row, so the schema loop skips it. Keys match the .imat
        // vocabulary (texTiling / texOffset); ApplyMaterialOverride reads only .xy of each.
        if (fne(mat.texMulAdd.x, baseMat.texMulAdd.x) ||
            fne(mat.texMulAdd.y, baseMat.texMulAdd.y))
            ovr.Set("texTiling", MaterialOverride::Value::MakeVec4(
                mat.texMulAdd.x, mat.texMulAdd.y, 0.0f, 0.0f));
        if (fne(mat.texMulAdd.z, baseMat.texMulAdd.z) ||
            fne(mat.texMulAdd.w, baseMat.texMulAdd.w))
            ovr.Set("texOffset", MaterialOverride::Value::MakeVec4(
                mat.texMulAdd.z, mat.texMulAdd.w, 0.0f, 0.0f));

        // Texture diffs
        for (int ti = 0; ti < MaterialComponent::TEXTURESLOT_COUNT; ++ti)
        {
            if (mat.textures[ti].name != baseMat.textures[ti].name)
            {
                // X-macro over the slot enum: no hardcoded count or stale slot names.
                static const char* kSlotNames[] = {
    #define SLOT_KEY_(e, label, path) "tex_" #e,
                    MATERIAL_TEXTURE_SLOTS(SLOT_KEY_)
    #undef SLOT_KEY_
                };
                if (ti < MaterialComponent::TEXTURESLOT_COUNT)
                    ovr.Set(kSlotNames[ti], MaterialOverride::Value::MakeTex(mat.textures[ti].name));
            }
        }


        return true;
    }

    void ApplyMaterialOverride(MaterialComponent& mat, const MaterialOverride& ovr)
    {
        using V = MaterialOverride::Value;
        for (const auto& [key, val] : ovr.props)
        {
            // Param keys (Float / Color): schema-driven, so EVERY kMaterialPBRSchema row
            // round-trips with BuildMaterialOverride's diff.
            const MaterialSchema::FieldDesc* f = nullptr;
            for (const auto& fd : MaterialSchema::kMaterialPBRSchema)
                if (key == fd.name) { f = &fd; break; }
            if (f)
            {
                float* dst = MaterialSchema::FieldPtr(mat, *f);
                if (val.type == V::Float && f->kind == MaterialSchema::Kind::Float)
                    dst[0] = val.data.f;
                else if (val.type == V::Vec4 &&
                         (f->kind == MaterialSchema::Kind::Color3 ||
                          f->kind == MaterialSchema::Kind::Color4))
                {
                    dst[0] = val.data.v4[0]; dst[1] = val.data.v4[1];
                    dst[2] = val.data.v4[2]; dst[3] = val.data.v4[3];
                }
                continue;
            }

            // Meta / mode / texture keys (not in the param schema).
            if (val.type == V::Float)
            {
                if      (key == "outlinePixels")  mat.outlinePixels = val.data.f;
                else if (key == "decalAngleFade") mat.decalAngleFadeStart = val.data.f;
            }
            else if (val.type == V::Int)
            {
                if      (key == "blendMode")
                {
                    if (val.data.i >= 0 && val.data.i < static_cast<int>(BlendMode::Count))
                        mat.userBlendMode = static_cast<BlendMode>(val.data.i);
                }
                else if (key == "shaderType")
                {
                    // Range-checked like its two neighbours below, as on the .imat path: an
                    // out-of-range value hides every Inspector row (`1u << shaderType`) with no
                    // message.
                    if (val.data.i >= 0 &&
                        val.data.i < static_cast<int>(MaterialComponent::SHADERTYPE_COUNT))
                        mat.shaderType = static_cast<MaterialComponent::SHADERTYPE>(val.data.i);
                    else
                        LOG_WARNING("MaterialOverride: shaderType = %d out of range, ignored",
                                    val.data.i);
                }
                else if (key == "customShadingModel")
                {
                    // Range-checked like `domain` below: the value is an int
                    // from a file, and an out-of-range ShadingModel would index
                    // the lighting pass's model routing with garbage.
                    if (val.data.i >= 0 &&
                        val.data.i < static_cast<int>(ShadingModel::Count))
                        mat.customShadingModel = static_cast<ShadingModel>(val.data.i);
                    else
                        LOG_WARNING("MaterialOverride: customShadingModel = %d out of range, ignored",
                                    val.data.i);
                }
                else if (key == "domain")
                {
                    if (val.data.i >= 0 &&
                        Resource::IsValidMaterialDomain(static_cast<uint32_t>(val.data.i)))
                        mat.domain = static_cast<Resource::MaterialDomain>(val.data.i);
                    else
                    {
                        LOG_WARNING("MaterialOverride: domain = %d out of range, using Opaque",
                                    val.data.i);
                        mat.domain = Resource::MaterialDomain::Opaque;
                    }
                }
                else if (key == "decalWriteMask") mat.decalWriteMask = static_cast<uint32_t>(val.data.i);
                else if (key == "decalSortLayer") mat.decalSortLayer = val.data.i;
            }
            else if (val.type == V::Bool)
            {
                // Flag overrides. X-macro over (key, flag, invert): receiveShadow is stored as a
                // DISABLE_ bit, so its override is the negation. Keep in step with DIFF_FLAG_OVR_
                // in BuildMaterialOverride.
#define MAT_OVR_FLAGS_(X)                                                    \
                X("castShadow",        MaterialComponent::CAST_SHADOW,            false) \
                X("receiveShadow",     MaterialComponent::DISABLE_RECEIVE_SHADOW, true)  \
                X("doubleSided",       MaterialComponent::DOUBLE_SIDED,           false) \
                X("outline",           MaterialComponent::OUTLINE,                false) \
                X("outlineScreenSpace", MaterialComponent::OUTLINE_SCREENSPACE,   false) \
                X("excludeFromSsao",   MaterialComponent::EXCLUDE_FROM_SSAO,      false) \
                X("hairMask",          MaterialComponent::HAIR_MASK,              false) \
                X("nprToonShadowEdge", MaterialComponent::NPR_TOON_SHADOW_EDGE,   false) \
                X("sourceOutlineExempt", MaterialComponent::SOURCE_OUTLINE_EXEMPT, false) \
                X("useVertexColor",    MaterialComponent::USE_VERTEXCOLORS,       false) \
                X("nprClampEnabled",   MaterialComponent::NPR_BRIGHTNESS_CLAMP,   false)

#define MAT_OVR_APPLY_FLAG_(k, flag, invert)                                 \
                if (key == (k))                                              \
                {                                                            \
                    const bool set = (invert) ? !val.data.b : val.data.b;     \
                    if (set) mat.flags |=  (flag);                          \
                    else     mat.flags &= ~(flag);                          \
                }
                MAT_OVR_FLAGS_(MAT_OVR_APPLY_FLAG_)
#undef MAT_OVR_APPLY_FLAG_
#undef MAT_OVR_FLAGS_
            }
            else if (val.type == V::Vec4)
            {
                // UV transform: texMulAdd is a float4 with a hand-written inspector widget, NOT a
                // kMaterialPBRSchema row, so it needs these explicit keys. Names match the .imat
                // keys (texTiling / texOffset), the same vocabulary at all three material levels;
                // only .xy of each is meaningful.
                if      (key == "texTiling")
                { mat.texMulAdd.x = val.data.v4[0]; mat.texMulAdd.y = val.data.v4[1]; }
                else if (key == "texOffset")
                { mat.texMulAdd.z = val.data.v4[0]; mat.texMulAdd.w = val.data.v4[1]; }
            }
            else if (val.type == V::TexRef)
            {
                // An empty path clears the slot (the editor's diff writes an empty TexRef when an
                // entity unbinds a texture), and so does `<none>`, so the three material levels
                // spell an explicit clear the same way.
                const std::string tex = (val.texPath == "<none>") ? std::string{} : val.texPath;
                // X-macro over every slot (incl. RAMPMAP / OPACITYMAP), matching
                // BuildMaterialOverride's X-macro diff.
    #define APPLY_TEX_OVR_(e, label, defpath) \
                if (key == "tex_" #e) { mat.textures[MaterialComponent::e].name = tex; }
                MATERIAL_TEXTURE_SLOTS(APPLY_TEX_OVR_)
    #undef APPLY_TEX_OVR_
            }
        }
    }

int ReloadMaterialsAffectedBy(World& world, const std::string& changedPathIn, bool live)
{
    if (changedPathIn.empty()) return 0;

    auto canon = [](const std::string& p)
    {
        std::string c = Sys::RelativizeAssetPath(p);
        for (char& ch : c) if (ch == '\\') ch = '/';
        return c;
    };
    auto lower = [](std::string s)
    {
        for (char& ch : s) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        return s;
    };

    // Everything below compares the SPELLING a material was loaded with: the reverse
    // index, the cache, MaterialSourcePath. The asset watcher reports LOWERCASED paths
    // (FileWatcher::Normalize), which on Windows name the same file, so an edit of
    // engine/textures/Default_mat.imat arrives as engine/textures/default_mat.imat and
    // would re-resolve nothing. Adopt the spelling in use: the index's, else an
    // entity's that differs only in case.
    std::string changedPath = MaterialIndexSpelling(changedPathIn);
    if (auto* sp = world.GetPool<MaterialSourcePath>())
    {
        const std::string low = lower(changedPath);
        for (Entity e : sp->Entities())
        {
            const MaterialSourcePath* p = world.GetComponent<MaterialSourcePath>(e);
            if (!p || p->path.empty()) continue;
            const std::string c = canon(p->path);
            if (c == changedPath) break;                       // already the spelling in use
            if (c.size() == low.size() && lower(c) == low) { changedPath = c; break; }
        }
    }
    const std::string needle = canon(changedPath);

    // The reverse dependency index is the primary answer and it is exact: it
    // records every file each resolved material read, parents at any depth
    // included, and unlike the cache it SURVIVES the invalidation a save
    // performs. It is also the only thing that can see a non-material dependency
    // such as a .itex, which no parent walk could find.
    const std::vector<std::string> known = MaterialsDependingOn(changedPath);
    std::unordered_set<std::string> want(known.begin(), known.end());
    want.insert(needle);

    // Disk walk as the FALLBACK, for a material an entity references that never
    // resolved (so it has no index entry): a first touch after a rename, or a parent
    // that was missing and has reappeared. Memoized per source path below, and only
    // reached for paths the index did not answer.
    auto chainReaches = [&](const std::string& start)
    {
        std::string cur = start;
        for (int depth = 0; depth <= 16; ++depth)   // same bound as the resolver
        {
            if (cur == needle) return true;
            const std::string parent = GetMaterialParent(cur);
            if (parent.empty()) return false;
            cur = canon(parent);
        }
        return false;
    };

    // Scoped cache drop AND scoped entity reload. Dropping the whole cache would force
    // a reparse of every material in the project; reloading an unaffected entity costs
    // a SetDirty(), the expensive one (see the header). The reverse index survives
    // this, which keeps `want` valid.
    InvalidateMaterialsDependingOn(changedPath);

    auto* srcPool = world.GetPool<MaterialSourcePath>();
    auto* matPool = world.GetPool<MaterialComponent>();
    if (!srcPool || !matPool) return 0;

    int reloaded = 0;
    // Snapshot the entity list: ApplyMaterialLoadInfo can add or remove a
    // MaterialInstanceInfo, and a structural change invalidates pool iterators.
    const std::vector<Entity> entities(srcPool->Entities().begin(),
                                       srcPool->Entities().end());
    // Per-path verdict, so a scene with 200 entities sharing 10 materials walks
    // 10 chains rather than 200.
    std::unordered_map<std::string, bool> verdict;

    for (Entity e : entities)
    {
        if (!world.IsAlive(e)) continue;
        const MaterialSourcePath* sp = world.GetComponent<MaterialSourcePath>(e);
        if (!sp || sp->path.empty()) continue;

        const std::string canonical = canon(sp->path);
        auto v = verdict.find(canonical);
        if (v == verdict.end())
            v = verdict.emplace(canonical,
                                want.count(canonical) != 0 || chainReaches(canonical)).first;
        if (!v->second) continue;

        MaterialComponent* mc = world.GetComponent<MaterialComponent>(e);
        if (!mc) continue;
        if (live && mc->IsAssetDirty()) continue;   // its own unsaved edits win over a live push

        MaterialLoadInfo info;
        MaterialComponent fresh{};
        if (!LoadMaterial(sp->path, fresh, &info)) continue;

        *mc = std::move(fresh);
        ApplyMaterialLoadInfo(world, e, info);

        // The per-entity bag sits ON TOP of the file and must be replayed, or a
        // reload silently reverts every per-entity override in the scene.
        if (const MaterialOverride* ovr = world.GetComponent<MaterialOverride>(e))
            ApplyMaterialOverride(*mc, *ovr);

        mc->SetDirty();
        ++reloaded;
    }

    if (reloaded && !live)
        LOG_INFO("MaterialOverrideApply: '%s' changed - re-materialized %d entit%s "
                 "(%zu distinct material file(s) examined)",
                 changedPath.c_str(), reloaded, reloaded == 1 ? "y" : "ies",
                 verdict.size());
    return reloaded;
}

} // namespace Resource
