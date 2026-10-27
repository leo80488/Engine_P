#include "Resource/MaterialSerializer.h"
#include "Resource/LoadCounters.h"   // a runtime spawn's census counts its parses
#include "Diagnostics/Profiler.h"
#include "Resource/MaterialLiveOverlay.h"
#include "Resource/AssetHeader.h"
#include "Resource/AssetMigration.h"
#include "Resource/AssetFS.h"
#include "Resource/AssetRefs.h"   // guid-augmented references: survive a rename
#include "Resource/AuthoringSchema.h"
#include "ECS/Components.h"
#include "ECS/MaterialSchema.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Resource
{
const std::vector<Authoring::Field>& Authoring::MaterialFields()
{
    static const auto s_fields = [] {
        std::vector<Field> out;
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
        {
            using K = MaterialSchema::Kind;
            const int arity = f.kind == K::Color4 ? 4 : f.kind == K::Color3 ? 3 : 1;
            const bool normalized = std::string_view(f.name).find("roughness") == 0 ||
                                    std::string_view(f.name).find("metalness") == 0;
            out.push_back({f.name, arity > 1 ? "vector" : "number", arity,
                arity > 1 ? "srgb" : "unitless", normalized || arity > 1 ? 0 : -FloatMax,
                normalized ? 1 : FloatMax, false, false,
                "PBR schema; UI slider bounds are hints, not blanket legal ranges"});
        }
        return out;
    }();
    return s_fields;
}

namespace
{
    namespace fs = std::filesystem;

    struct MaterialDependencyStamp
    {
        std::string        path;
        fs::file_time_type writeTime{};
        std::uintmax_t     fileSize = 0;
        bool               inPak    = false;
        bool               exists   = false;
    };

    struct MaterialCacheEntry
    {
        MaterialComponent                     material;
        std::vector<MaterialDependencyStamp> dependencies;
        // What THIS file authored (not what the resolved material holds). Cached with the material
        // and invalidated by the same stamps; without it a cache hit would carry no override set and
        // a re-save would drop every override the file declared.
        MaterialLoadInfo                      info;
    };

    // Entries are stored by shared_ptr<const ...> so a reader can grab an
    // immutable snapshot under the lock (a cheap refcount bump) and then run the
    // filesystem currency check + the heavy MaterialComponent copy OFF the lock.
    std::mutex                                                                g_materialCacheMutex;
    std::unordered_map<std::string, std::shared_ptr<const MaterialCacheEntry>> g_materialCache;
    MaterialCacheStats                                                        g_materialCacheStats;

    // See MaterialContentGeneration(). Its own counter, not g_materialCacheStats.invalidations:
    // that statistic moves only when an entry was dropped, while a save of a material nobody has
    // cached is still a content change to whoever holds a copy.
    std::atomic<std::uint64_t>                                                g_materialContentGen{ 1 };

    // Reverse dependency index: file -> the materials whose resolution read it, parents at any
    // depth (a chain resolve records its own stamp before recursing).
    // Kept SEPARATE from the cache: which materials a file feeds is a property of the files, and
    // it is needed right after a save, exactly when cache entries are dropped. Derived from the
    // cache, a save's invalidation would destroy the answer and instances of an edited master
    // would not update.
    // Only ever added to, so it goes stale only in the safe direction: a material that stops
    // depending on a file costs one redundant reload, a dependency is never missed. Cleared only
    // by ClearMaterialCache.
    std::unordered_map<std::string, std::unordered_set<std::string>> g_materialDependents;

    // The live overlay (Resource/MaterialLiveOverlay.h): an editor's unsaved version of a file,
    // keyed by the canonical path. g_liveCount is the resolver's lock-free "is there any" -
    // always zero in Game.exe.
    struct LiveEntry
    {
        MaterialComponent material;
        MaterialLoadInfo  info;
    };
    std::mutex                                                        g_liveMutex;
    std::unordered_map<std::string, std::shared_ptr<const LiveEntry>> g_live;
    std::atomic<std::size_t>                                          g_liveCount{ 0 };
    thread_local int                                                  t_liveBypass = 0;   // LiveMaterialBypass depth

    std::string CanonicalMaterialPath(const std::string& path)
    {
        std::string canonical = Sys::RelativizeAssetPath(path);
        for (char& c : canonical)
            if (c == '\\') c = '/';
        while (canonical.size() >= 2 && canonical[0] == '.' && canonical[1] == '/')
            canonical.erase(0, 2);
        return canonical;
    }

    MaterialDependencyStamp CaptureDependencyStamp(const std::string& path)
    {
        MaterialDependencyStamp stamp;
        stamp.path  = path;
        stamp.inPak = ::Resource::AssetFS::Get().HasInPak(path);
        if (stamp.inPak)
        {
            // A mounted package is immutable for its lifetime. AssetFS::Mount
            // happens before material loads; tools that remount at runtime must
            // call ClearMaterialCache afterwards.
            stamp.exists = true;
            return stamp;
        }

        // The file the load reads: through the search roots (a worktree's
        // shared content root included), else the path as given.
        std::error_code ec;
        const std::string onDisk = ::Resource::AssetFS::Get().ResolveOnDisk(path);
        const fs::path diskPath = Sys::U8Path(onDisk.empty() ? path : onDisk);
        stamp.writeTime = fs::last_write_time(diskPath, ec);
        if (ec) return stamp;

        stamp.fileSize = fs::file_size(diskPath, ec);
        if (ec) return stamp;

        stamp.exists = true;
        return stamp;
    }

    bool SameDependencyStamp(const MaterialDependencyStamp& a,
                             const MaterialDependencyStamp& b)
    {
        if (a.path != b.path || a.inPak != b.inPak || a.exists != b.exists)
            return false;
        if (!a.exists || a.inPak)
            return true;
        return a.writeTime == b.writeTime && a.fileSize == b.fileSize;
    }

    bool IsCacheEntryCurrent(const MaterialCacheEntry& entry)
    {
        for (const auto& dependency : entry.dependencies)
            if (!SameDependencyStamp(dependency,
                                     CaptureDependencyStamp(dependency.path)))
                return false;
        return true;
    }

    bool TryGetCachedMaterial(const std::string& path, MaterialCacheEntry& out)
    {
        std::shared_ptr<const MaterialCacheEntry> entry;
        {
            std::lock_guard lock(g_materialCacheMutex);
            const auto it = g_materialCache.find(path);
            if (it == g_materialCache.end())
            {
                ++g_materialCacheStats.misses;
                return false;
            }
            entry = it->second;   // cheap refcount bump; no deep copy under lock
        }

        // Currency check (filesystem stat) + the caller's deep copy both run on
        // the immutable snapshot, off the lock.
        if (IsCacheEntryCurrent(*entry))
        {
            out = *entry;
            std::lock_guard lock(g_materialCacheMutex);
            ++g_materialCacheStats.hits;
            return true;
        }

        // Stale: drop it, but only if the map STILL holds the snapshot just checked. A concurrent
        // StoreCachedMaterial may have replaced it with a fresh entry in the gap; erasing blindly
        // would discard that valid entry and force a needless reparse.
        std::lock_guard lock(g_materialCacheMutex);
        const auto it = g_materialCache.find(path);
        if (it != g_materialCache.end() && it->second == entry)
        {
            g_materialCache.erase(it);
            ++g_materialCacheStats.invalidations;
        }
        ++g_materialCacheStats.misses;
        return false;
    }

    void StoreCachedMaterial(const std::string& path,
                             const MaterialComponent& material,
                             const std::vector<MaterialDependencyStamp>& dependencies,
                             const MaterialLoadInfo& info)
    {
        // Build the entry outside the lock; only the map insert is serialized.
        auto entry = std::make_shared<MaterialCacheEntry>();
        entry->material     = material;
        entry->dependencies = dependencies;
        entry->info         = info;
        std::lock_guard lock(g_materialCacheMutex);
        for (const auto& dep : dependencies)
            g_materialDependents[dep.path].insert(path);
        g_materialCache.insert_or_assign(path, std::move(entry));
    }

    // The reverse index alone, for a resolution that is NOT cached (a
    // LiveMaterialBypass read while the live overlay holds something): it still
    // read those files, and a later save of one of them must find it - the
    // Material Editor's tabs that follow a saved parent are found this way.
    void RecordDependents(const std::string& path, const std::vector<MaterialDependencyStamp>& dependencies)
    {
        std::lock_guard lock(g_materialCacheMutex);
        for (const auto& dep : dependencies)
            g_materialDependents[dep.path].insert(path);
    }

    // Drop the cache entries that read @p file, leaving every unrelated entry
    // alone. Returns how many were dropped. The reverse index is deliberately
    // NOT touched: it is what the caller needs immediately afterwards to find
    // the entities to reload.
    std::size_t InvalidateDependentsLocked(const std::string& file)
    {
        auto it = g_materialDependents.find(file);
        if (it == g_materialDependents.end()) return 0;
        std::size_t dropped = 0;
        for (const std::string& mat : it->second)
            dropped += g_materialCache.erase(mat);
        return dropped;
    }
}

// ==== Save ====
// Build the key=value text payload of a material.
//   base == nullptr -> full dump (every key), for a standalone .imat.
//   base != nullptr -> INSTANCE delta against the resolved parent @p base, after a
//                      `parent = <parentPath>` line.
// One writer for both, so the two key sets cannot drift.
// Which keys an instance writes is decided by @p ovr:
//   ovr != nullptr  EXPLICIT mode (imatfmt = 2): a key is written because the author marked
//                   it, not because its value differs (UMaterialInstanceConstant semantics;
//                   PostProcessProfileSerializer's overrideState works the same way).
//   ovr == nullptr  VALUE-DIFF mode: the keys whose value differs from @p base, in a stable
//                   byte format so files on disk need no migration. A value pinned equal to
//                   the parent's leaves no trace and follows the next edit of the master.
// @p unsetKeys (explicit mode) removes a `cparam_` / `ctex_` / `sswitch_` the instance
// INHERITS: the custom loops walk the child's own map, so "parent has it, child does not"
// emits nothing and the parent's value merges back in.
static std::string BuildTextImpl(const MaterialComponent& mat,
                                 const MaterialComponent* base,
                                 const std::string* parentPath,
                                 const std::unordered_set<std::string>* ovr = nullptr,
                                 const std::vector<std::string>* unsetKeys = nullptr)
{
    std::ostringstream s;
    char buf[128];
    const bool full     = (base == nullptr);
    const bool explicitSet = (!full && ovr != nullptr);
    // want() takes the "does it differ" answer BY VALUE, so every diff
    // expression below is evaluated before the call -- including in full-dump
    // mode, where `base` is null. Aliasing the baseline to `mat` itself there
    // makes every comparison trivially false and every dereference valid; the
    // answer is discarded anyway, because want() returns true on `full` first.
    const MaterialComponent& baseRef = base ? *base : mat;
    auto fdiff = [](float a, float b) { return std::fabsf(a - b) > 1e-5f; };

    // The one predicate every emit below goes through.
    auto want = [&](const char* key, bool valueDiffers) -> bool
    {
        if (full)        return true;                    // standalone: everything
        if (explicitSet) return ovr->count(key) != 0;     // instance: the author's set
        return valueDiffers;                              // value diff: stable bytes
    };

    // An empty path is an override only in explicit mode, where it means "clear what the parent
    // bound". Spelled `<none>` because ApplyMaterialText drops empty values before key dispatch
    // (that guard protects the master .imat files and must stay); angle brackets are illegal in
    // Windows filenames, so the token cannot collide with a real path, and `%` is already taken
    // by percent-encoding.
    auto pathOrNone = [&](const std::string& p) -> std::string
    { return (p.empty() && explicitSet) ? std::string("<none>") : p; };

    s << (full ? "# DX12 Engine Material\n" : "# DX12 Engine Material Instance\n");
    if (parentPath && !parentPath->empty())
        s << "parent = " << *parentPath << "\n";
    // Format marker: its PRESENCE tells the loader (CollectAuthoredKeys) that the keys are an
    // authored set rather than a value diff.
    if (explicitSet)
        s << "imatfmt = 2\n";
    // Material text generation. 3 = the lighting axis is customShadingModel alone;
    // `shaderType` below is a tombstone for older builds, not routed on. A file's header version
    // says the same from version 2 on; this line is what headerless text (a scene's inline
    // material, an instance delta) and a version 1 file carry, and it stops ApplyMaterialText
    // re-running the shaderType -> model fold over text whose model was edited afterwards.
    // Unconditional (not gated on want): a format marker is a property of the FILE, not a diff.
    s << "matfmt = 3\n";

    // ---- Meta / mode (not MaterialSchema rows: not editable params) ----
    // shaderType is a tombstone kept so older builds still read these files. ApplyMaterialText
    // re-derives it from customShadingModel on every load (MaterialComponent::SyncLegacyShaderType),
    // so the value echoed here always agrees with the axis that renders.
    if (want("shaderType", mat.shaderType != baseRef.shaderType))
        s << "shaderType = "     << static_cast<uint32_t>(mat.shaderType)     << "\n";
    if (want("blendMode", mat.userBlendMode != baseRef.userBlendMode))
        s << "blendMode = "      << static_cast<uint32_t>(mat.userBlendMode)  << "\n";
    if (want("shadowCullMode", mat.shadowCullMode != baseRef.shadowCullMode))
        s << "shadowCullMode = " << static_cast<uint32_t>(mat.shadowCullMode) << "\n";
    // Material domain (0 Opaque / 1 Transparent / 2 Decal / 3 PostProcess) picks the PIPELINE
    // that draws the material, so an instance that inherits it degrades to Opaque the moment its
    // parent fails to resolve (a translucent water plane drawn as an opaque slab by GBufferPass).
    // Value-diff mode therefore always writes it; explicit mode treats it as an ordinary key that
    // instance creation seeds into the set, so an author can unpin it.
    if (full || !explicitSet || ovr->count("domain"))
        s << "domain = "         << static_cast<uint32_t>(mat.domain)         << "\n";
    if (want("decalWriteMask", mat.decalWriteMask != baseRef.decalWriteMask))
        s << "decalWriteMask = " << mat.decalWriteMask << "\n";
    if (want("decalAngleFade", fdiff(mat.decalAngleFadeStart, baseRef.decalAngleFadeStart)))
    {
        snprintf(buf, sizeof(buf), "%.6f", mat.decalAngleFadeStart);
        s << "decalAngleFade = " << buf << "\n";
    }
    if (want("decalSortLayer", mat.decalSortLayer != baseRef.decalSortLayer))
        s << "decalSortLayer = " << mat.decalSortLayer << "\n";
    if (want("castShadow", mat.IsCastingShadow() != baseRef.IsCastingShadow()))
        s << "castShadow = "    << (mat.IsCastingShadow()  ? "1" : "0") << "\n";
    if (want("receiveShadow", mat.IsReceiveShadow() != baseRef.IsReceiveShadow()))
        s << "receiveShadow = " << (mat.IsReceiveShadow()  ? "1" : "0") << "\n";
    if (want("doubleSided", mat.IsDoubleSided() != baseRef.IsDoubleSided()))
        s << "doubleSided = "   << (mat.IsDoubleSided()    ? "1" : "0") << "\n";
    if (want("outline", mat.IsOutlineEnabled() != baseRef.IsOutlineEnabled()))
        s << "outline = "       << (mat.IsOutlineEnabled() ? "1" : "0") << "\n";
    // The Inspector's "Screen Space Outline" (edge detection on top of the hull). Written in
    // every full dump, 0 included: the Material Editor reads a key's parent value and validates a
    // scripted Set against the full dump, where a missing key is "not a key". Absent on read = off.
    if (want("outlineScreenSpace", mat.IsOutlineScreenSpaceEnabled() != baseRef.IsOutlineScreenSpaceEnabled()))
        s << "outlineScreenSpace = " << (mat.IsOutlineScreenSpaceEnabled() ? "1" : "0") << "\n";
    {
        const bool a = (mat.flags & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
        const bool b = base && (baseRef.flags & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
        if (want("excludeFromSsao", a != b)) s << "excludeFromSsao = " << (a ? "1" : "0") << "\n";
    }
    {
        const bool a = (mat.flags & MaterialComponent::HAIR_MASK) != 0;
        const bool b = base && (baseRef.flags & MaterialComponent::HAIR_MASK) != 0;
        if (want("hairMask", a != b)) s << "hairMask = " << (a ? "1" : "0") << "\n";
    }
    {
        // The stylized cast-shadow edge. The float it reads (nprSceneShadowSmooth) is a schema row,
        // but the flag that makes the shader read it is not, so without this key a save reverts the
        // material to the raw PCF penumbra. An ABSENT key means OFF (the reader has no else): files
        // carry a non-zero nprSceneShadowSmooth and keep the plain look until an artist ticks the box.
        const bool a = (mat.flags & MaterialComponent::NPR_TOON_SHADOW_EDGE) != 0;
        const bool b = base && (baseRef.flags & MaterialComponent::NPR_TOON_SHADOW_EDGE) != 0;
        if (want("nprToonShadowEdge", a != b)) s << "nprToonShadowEdge = " << (a ? "1" : "0") << "\n";
    }
    {
        const bool a = (mat.flags & MaterialComponent::SOURCE_OUTLINE_EXEMPT) != 0;
        const bool b = base && (baseRef.flags & MaterialComponent::SOURCE_OUTLINE_EXEMPT) != 0;
        if (want("sourceOutlineExempt", a != b)) s << "sourceOutlineExempt = " << (a ? "1" : "0") << "\n";
    }
    if (want("useVertexColor", mat.IsUsingVertexColors() != baseRef.IsUsingVertexColors()))
        s << "useVertexColor = " << (mat.IsUsingVertexColors() ? "1" : "0") << "\n";
    // On/off for the RELATIVE NPR brightness ceiling; its value is the schema row
    // nprBrightnessClamp, so 0 need not double as "disabled". Absent key = off.
    if (want("nprClampEnabled", mat.IsNprBrightnessClampEnabled() != baseRef.IsNprBrightnessClampEnabled()))
        s << "nprClampEnabled = " << (mat.IsNprBrightnessClampEnabled() ? "1" : "0") << "\n";

    // outlinePixels lives outside the param schema (tied to the OUTLINE flag UI).
    if (want("outlinePixels", fdiff(mat.outlinePixels, baseRef.outlinePixels)))
    {
        snprintf(buf, sizeof(buf), "%.6f", mat.outlinePixels);
        s << "outlinePixels = " << buf << "\n";
    }

    // ---- Schema-driven parameter dump / diff ----
    // Walks kMaterialPBRSchema: each row writes one `name = value` line when it differs from base
    // (always, in a full dump). FloatComponent rows are skipped: their parent Color3/4 row covers
    // the storage. A new editable field is one row in MaterialSchema.h.
    for (const auto& f : MaterialSchema::kMaterialPBRSchema)
    {
        if (f.kind == MaterialSchema::Kind::FloatComponent) continue;
        const float* cur = MaterialSchema::FieldPtr(mat, f);
        // baseRef, never `base`: want() reads bas[] eagerly, so it must be a real pointer even in a
        // full dump (see the alias above).
        const float* bas = MaterialSchema::FieldPtr(baseRef, f);
        switch (f.kind)
        {
        case MaterialSchema::Kind::Float:
            if (want(f.name, fdiff(cur[0], bas[0])))
            {
                snprintf(buf, sizeof(buf), "%.6f", cur[0]);
                s << f.name << " = " << buf << "\n";
            }
            break;
        case MaterialSchema::Kind::Color3:
        case MaterialSchema::Kind::Color4:
            // Always 4 floats: a Color3's .w may carry data (emissiveColor.w = strength).
            if (want(f.name, fdiff(cur[0], bas[0]) || fdiff(cur[1], bas[1]) ||
                             fdiff(cur[2], bas[2]) || fdiff(cur[3], bas[3])))
            {
                snprintf(buf, sizeof(buf), "%.6f %.6f %.6f %.6f",
                         cur[0], cur[1], cur[2], cur[3]);
                s << f.name << " = " << buf << "\n";
            }
            break;
        default: break;
        }
    }

    // ---- UV tiling / offset ----
    if (want("texTiling", fdiff(mat.texMulAdd.x, baseRef.texMulAdd.x) ||
                          fdiff(mat.texMulAdd.y, baseRef.texMulAdd.y)))
    {
        snprintf(buf, sizeof(buf), "%.6f %.6f", mat.texMulAdd.x, mat.texMulAdd.y);
        s << "texTiling = " << buf << "\n";
    }
    if (want("texOffset", fdiff(mat.texMulAdd.z, baseRef.texMulAdd.z) ||
                          fdiff(mat.texMulAdd.w, baseRef.texMulAdd.w)))
    {
        snprintf(buf, sizeof(buf), "%.6f %.6f", mat.texMulAdd.z, mat.texMulAdd.w);
        s << "texOffset = " << buf << "\n";
    }

    // ---- Texture slots ----
    // pathOrNone writes a cleared slot as `<none>`: an empty value is dropped by the reader before
    // dispatch, so the parent's texture would come back on the next load.
#define WRITE_TEX_(e, label, defpath) \
    if (want("tex_" #e, mat.textures[MaterialComponent::e].name != baseRef.textures[MaterialComponent::e].name)) \
        s << "tex_" #e " = " << pathOrNone(mat.textures[MaterialComponent::e].name) << "\n";
    MATERIAL_TEXTURE_SLOTS(WRITE_TEX_)
#undef WRITE_TEX_

    // ---- Custom shader + reflection-driven param bag ----
    // Persists the custom-shader path so such a material round-trips as a standalone .imat or as
    // instance deltas. Keys are sorted so the text is deterministic. Param / texture names are
    // HLSL identifiers (no spaces); values are "f f f f" / "<uvset> <path>".
    if (want("useCustomShader", mat.useCustomShader != baseRef.useCustomShader))
        s << "useCustomShader = " << (mat.useCustomShader ? "1" : "0") << "\n";
    if (want("rtUseStandardMaterial", mat.rtUseStandardMaterial != baseRef.rtUseStandardMaterial))
        s << "rtUseStandardMaterial = " << (mat.rtUseStandardMaterial ? "1" : "0") << "\n";
    // In explicit mode an empty path is a real override (an instance clearing an inherited custom
    // shader) and goes out as <none>. Value-diff mode skips empty paths so its bytes stay stable;
    // there an inherited path cannot be cleared.
    if (want("customShaderPath", mat.customShaderPath != baseRef.customShaderPath)
        && (explicitSet || !mat.customShaderPath.empty()))
        s << "customShaderPath = " << pathOrNone(mat.customShaderPath) << "\n";
    if (want("customVertexShaderPath", mat.customVertexShaderPath != baseRef.customVertexShaderPath)
        && (explicitSet || !mat.customVertexShaderPath.empty()))
        s << "customVertexShaderPath = " << pathOrNone(mat.customVertexShaderPath) << "\n";
    if (want("materialGraph", mat.materialGraph != baseRef.materialGraph)
        && (explicitSet || !mat.materialGraph.empty()))
        s << "materialGraph = " << pathOrNone(mat.materialGraph) << "\n";
    if (want("customShadingModel", mat.customShadingModel != baseRef.customShadingModel))
        s << "customShadingModel = " << static_cast<uint32_t>(mat.customShadingModel) << "\n";

    // Custom-param FORMAT marker: its presence tells the loader "diff-against-default format, do
    // not run sentinel migration". Keyed on customParamsLegacyFormat, not on customParamMeta: a
    // freshly loaded new-format material that has not rendered yet has empty meta but must keep
    // the marker, or a save-before-render would make the next load flag it old-format and migrate
    // (clobbering a deliberate sentinel-valued override). A material still on the old sentinel
    // format writes no marker until its first render Sync migrates it and clears the flag.
    const bool newFormat = mat.useCustomShader && !mat.customParamsLegacyFormat;
    if (newFormat)
        s << "cparamFmt = 2\n";
    // Diff-against-default needs the reflected defaults, which exist only once the shader has
    // been reflected (meta populated). Until then dump full values; the marker above still keeps
    // the reload from setting customParamsLegacyFormat.
    const bool diffAgainstDefault = newFormat && !mat.customParamMeta.empty();

    {
        auto arrEq = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
            for (int i = 0; i < 4; ++i) if (std::fabsf(a[i] - b[i]) > 1e-5f) return false;
            return true;
        };
        std::vector<const std::string*> keys;
        keys.reserve(mat.customParams.size());
        for (const auto& kv : mat.customParams) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        for (const std::string* name : keys)
        {
            const auto& v = mat.customParams.at(*name);
            if (explicitSet)
            {
                // Explicit mode: only the author's set. The value-diff branch below compares against
                // baseRef.customParams, but a resolved parent's bag holds only the keys literally in ITS
                // text, while this bag holds every reflected //@ parameter (MaterialReflectionSync::Sync);
                // that lookup misses almost every key and pins ~20 parameters the artist never touched.
                if (!ovr->count("cparam_" + *name)) continue;
            }
            else if (!full)
            {
                auto it = baseRef.customParams.find(*name);
                if (it != baseRef.customParams.end() && it->second == v) continue;
            }
            else if (diffAgainstDefault)
            {
                // Only OVERRIDES are persisted: skip params equal to the reflected
                // default. Unset params reappear with the real default at load time
                // via MaterialReflectionSync. (Params with no known meta still write.)
                auto mit = mat.customParamMeta.find(*name);
                if (mit != mat.customParamMeta.end() && arrEq(v, mit->second.defaultValue))
                    continue;
            }
            snprintf(buf, sizeof(buf), "%.6f %.6f %.6f %.6f", v[0], v[1], v[2], v[3]);
            s << "cparam_" << *name << " = " << buf << "\n";
        }
    }
    {
        std::vector<const std::string*> keys;
        keys.reserve(mat.customTextures.size());
        for (const auto& kv : mat.customTextures) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        for (const std::string* name : keys)
        {
            const auto& tex = mat.customTextures.at(*name);
            if (explicitSet)
            {
                // Explicit mode: only the author's set (see the cparam_ loop above).
                if (!ovr->count("ctex_" + *name)) continue;
            }
            else if (!full)
            {
                auto it = baseRef.customTextures.find(*name);
                if (it != baseRef.customTextures.end() &&
                    it->second.name == tex.name && it->second.uvset == tex.uvset) continue;
            }
            // An assigned-but-empty slot carries no information in a full dump: the loader re-seeds it
            // from reflection, and "<uvset> " would trim to a lone token. An instance delta still writes
            // it: there empty is an override that CLEARS the parent's texture (<none> in explicit mode).
            if (full && tex.name.empty()) continue;
            s << "ctex_" << *name << " = " << tex.uvset << " "
              << pathOrNone(tex.name) << "\n";
        }
    }
    {
        // The graph's Static Switch Parameter values (which shader combination customShaderPath
        // names); editor-only, the runtime ignores them.
        std::vector<const std::string*> keys;
        keys.reserve(mat.staticSwitches.size());
        for (const auto& kv : mat.staticSwitches) keys.push_back(&kv.first);
        std::sort(keys.begin(), keys.end(),
                  [](const std::string* a, const std::string* b) { return *a < *b; });
        for (const std::string* name : keys)
        {
            const bool v = mat.staticSwitches.at(*name);
            if (explicitSet)
            {
                if (!ovr->count("sswitch_" + *name)) continue;
            }
            else if (!full)
            {
                auto it = baseRef.staticSwitches.find(*name);
                if (it != baseRef.staticSwitches.end() && it->second == v) continue;
            }
            s << "sswitch_" << *name << " = " << (v ? 1 : 0) << "\n";
        }
    }

    // ---- Explicit removals ----
    // `unset = <key>` drops an inherited cparam_ / ctex_ / sswitch_ entry: the loops above walk
    // the CHILD's map, so a key only the parent has emits nothing and merges back in on load.
    // Written last so it wins over anything above.
    if (explicitSet && unsetKeys)
        for (const std::string& k : *unsetKeys)
            if (!k.empty()) s << "unset = " << k << "\n";

    return s.str();
}

static std::string BuildText(const MaterialComponent& mat)
{
    // Stamp every texture / shader reference with its target's guid, so the .imat file, the
    // in-memory blob and a scene's MaterialInline blob all carry identities. SaveMaterialInstance
    // applies the same wrap to its own delta text (which also stamps `parent`).
    return AssetRefs::AugmentImatText(BuildTextImpl(mat, nullptr, nullptr));
}

std::string BuildMaterialText(const MaterialComponent& mat)
{
    return BuildText(mat);
}

// ==== Version migration ====
// A material file's header version is its format generation (kAssetFormats). Version 1 predates
// the header carrying it: the generation was the text's own `matfmt` line, so text with no header
// (an inline scene material, an instance delta) is read as version 1 and says its generation in
// its text. kMaterialMigrations[i] lifts version i + 1 to i + 2 on the decoded material.
namespace
{
    struct MaterialDecode
    {
        MaterialComponent& mat;
        bool               textSaidFolded;   // the text carried matfmt >= 3
    };

    // 1 -> 2. customShadingModel is THE lighting axis. A text before matfmt 3 carries the routing
    // decision on shaderType (a parallel axis that picked the deferred lighting variant), so it is
    // folded onto the model here, at load. ApplyMaterialText is the single applier for .imat,
    // .imatinst deltas, pak blobs AND the percent-encoded MaterialInline blobs embedded in scenes
    // (which no grep or asset tool sees), so every corpus migrates at once.
    // shaderType wins because it is what drove the stencil on those files: the fold is
    // pixel-preserving, the other direction would change materials authored Unlit on shaderType
    // and left Standard on the model axis. PBR and the retired parallax slot fall through and keep
    // the authored model (Standard / Subsurface / Anisotropic / TwoSidedFoliage / Water).
    // Skipped once the text says matfmt >= 3, so a deliberate model edit is not clobbered by a
    // stale shaderType line.
    void FoldShaderTypeIntoModel(MaterialDecode& decoded)
    {
        if (decoded.textSaidFolded)
            return;
        switch (decoded.mat.shaderType)
        {
        case MaterialComponent::SHADERTYPE_NPR_RAMP:
        case MaterialComponent::SHADERTYPE_NPR_COLOR:
            decoded.mat.customShadingModel = ShadingModel::Toon;      break;
        case MaterialComponent::SHADERTYPE_UNLIT:
            decoded.mat.customShadingModel = ShadingModel::Unlit;     break;
        case MaterialComponent::SHADERTYPE_RESERVED_3:
            decoded.mat.customShadingModel = ShadingModel::ClearCoat; break;
        default:
            // SHADERTYPE_PBR and the retired parallax slot: the model axis is
            // already authoritative for these.
            break;
        }
    }

    constexpr MigrateStep<MaterialDecode> kMaterialMigrations[] = {
        FoldShaderTypeIntoModel,   // 1 -> 2
    };
}

// Defined further down (kept next to the load path it belongs to). @p fileVersion is the header
// version of the file @p text came from; MinimumVersion(MAGIC_MATERIAL) for text with no header.
static void ApplyMaterialText(const char* text, size_t textLen,
                              MaterialComponent& mat, bool clearBag, uint16_t fileVersion);

void ApplyMaterialTextInPlace(MaterialComponent& mat, const std::string& text)
{
    // Normally a no-op: a MaterialInline blob reaching here was already
    // resolved and stripped by ResolveSceneText. Kept so this entry point is
    // safe for any caller that hands over raw authored text.
    const std::string resolved = AssetRefs::ResolveImatText(text, "<inline material>");
    ApplyMaterialText(resolved.c_str(), resolved.size(), mat, /*clearBag=*/true, MinimumVersion(MAGIC_MATERIAL));
    mat.SetDirty();
}

void ApplyMaterialTextDelta(MaterialComponent& mat, const std::string& text)
{
    ApplyMaterialText(text.c_str(), text.size(), mat, /*clearBag=*/false, MinimumVersion(MAGIC_MATERIAL));
    mat.SetDirty();
}

bool ValidateMaterialEdit(const MaterialComponent& current,
    const std::vector<std::pair<std::string, std::string>>& values, std::string& error)
{
    auto split = [](const std::string& text) {
        std::map<std::string, std::string> result;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) { const auto a=s.find_first_not_of(" \t\r");
                return a == std::string::npos ? std::string() : s.substr(a, s.find_last_not_of(" \t\r")-a+1); };
            result[trim(line.substr(0, eq))] = trim(line.substr(eq+1));
        }
        return result;
    };
    std::string delta;
    Authoring::Values checked;
    for (const auto& [key, value] : values)
    {
        auto fail = [&](const std::string& why) { error = "Material." + key + ": " + why; return false; };
        if (key.empty() || key.find_first_of("= \t\r\n") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
            return fail("invalid key or multiline value");
        if (const auto* field = Authoring::FindField("Material", key))
        {
            std::istringstream input(value);
            std::string token, canonical;
            while (input >> token) { if (!canonical.empty()) canonical += '_'; canonical += token; }
            checked.emplace_back(key, canonical);
        }
        else if (key.rfind("cparam_", 0) == 0)
        {
            std::istringstream input(value); double v; int n=0;
            while (input >> v) { if (!std::isfinite(v) || std::fabs(v) > Authoring::FloatMax) return fail("finite float32 values required"); ++n; }
            if (!input.eof() || n < 1 || n > 4) return fail("requires 1..4 numbers");
        }
        else if (key.rfind("tex_", 0) == 0 || key.rfind("ctex_", 0) == 0)
        {
            std::string path = value;
            if (key.rfind("ctex_", 0) == 0)
            {
                const auto space = value.find(' ');
                if (space == std::string::npos) return fail("requires UV-set index and texture path");
                if (value.substr(0, space) != "0" && value.substr(0, space) != "1") return fail("UV-set index must be 0 or 1");
                path = value.substr(space+1);
            }
            if (!path.empty() && path != "<none>" && !AssetFS::Get().ExistsExact(path)) return fail("texture does not exist");
        }
        delta += key + " = " + value + "\n";
    }
    if (!Authoring::Validate("Material", checked, error)) return false;
    MaterialComponent probe = current;
    ApplyMaterialTextDelta(probe, delta);
    // Validation must not call BuildMaterialText: its GUID augmentation may
    // author missing .meta sidecars in Editor, even on a rejected request.
    const auto written = split(BuildTextImpl(probe, nullptr, nullptr));
    for (const auto& [key, value] : values)
        if (!written.count(key)) { error = "Material." + key + ": unknown field"; return false; }
    return true;
}

bool SaveMaterial(const MaterialComponent& mat, const std::string& path,
                  bool allowFlattenInstance)
{
    // Refuse to overwrite an INSTANCE with a standalone dump unless the caller asked to flatten:
    // BuildText writes every key and no `parent =` line, so the file would keep its .imatinst
    // name and keep rendering but never follow its master again, with no warning and no undo.
    if (!allowFlattenInstance)
    {
        const std::string existingParent = GetMaterialParent(path);
        if (!existingParent.empty())
        {
            LOG_ERROR("MaterialSerializer: refusing to flatten instance '%s' (parent '%s') "
                      "into a standalone material — save it as an instance instead, or "
                      "break the link deliberately (allowFlattenInstance).",
                      path.c_str(), existingParent.c_str());
            return false;
        }
    }

    const std::string text        = BuildText(mat);
    const uint32_t    textLength  = static_cast<uint32_t>(text.size());
    const uint32_t    payloadSize = textLength + 1; // +1 null terminator

    MaterialMetadata meta{};
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MATERIAL;
    hdr.version      = CurrentVersion(MAGIC_MATERIAL);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Material);
    hdr.metadataSize = sizeof(MaterialMetadata);
    hdr.dataSize     = payloadSize;

    auto f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("MaterialSerializer: cannot open '%s' for writing", path.c_str());
        return false;
    }

    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLength);
    const char nul = '\0';
    f.write(&nul, 1);

    f.flush();
    if (!f.good())
    {
        LOG_ERROR("MaterialSerializer: write error for '%s'", path.c_str());
        return false;
    }

    f.close();
    // A standalone save affects only the cached materials that read this file (itself and its
    // instances). Clearing the whole cache would reparse every material and destroy the reverse
    // index the hot-reload path needs right after the save.
    InvalidateMaterialsDependingOn(path);
    LOG_INFO("MaterialSerializer: saved '%s' (%u bytes text)", path.c_str(), textLength);
    return true;
}

std::vector<uint8_t> BuildMaterialBlob(const MaterialComponent& mat)
{
    const std::string text = BuildText(mat);
    const uint32_t textLength = static_cast<uint32_t>(text.size());
    const uint32_t payloadSize = textLength + 1;

    MaterialMetadata meta{};
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MATERIAL;
    hdr.version      = CurrentVersion(MAGIC_MATERIAL);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Material);
    hdr.metadataSize = sizeof(MaterialMetadata);
    hdr.dataSize     = payloadSize;

    std::vector<uint8_t> blob(sizeof(AssetHeader) + sizeof(MaterialMetadata) + payloadSize);
    uint8_t* dst = blob.data();
    std::memcpy(dst, &hdr,  sizeof(hdr));  dst += sizeof(hdr);
    std::memcpy(dst, &meta, sizeof(meta)); dst += sizeof(meta);
    std::memcpy(dst, text.data(), textLength);
    dst[textLength] = '\0';

    return blob;
}

// ==== Load ====
// Maximum parent-chain depth: guards against cyclic instance references.
static constexpr int kMaxInstanceDepth = 16;

static std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Apply the key=value payload of an .imat / .imatinst onto @p mat.
//   clearBag == true  -> full load: the custom param / texture bag is wiped first, so the
//                        file is authoritative (a standalone material).
//   clearBag == false -> instance-delta MERGE: the bag is kept and only the keys in @p text
//                        overwrite or add.
// The `parent` key is skipped here; ResolveMaterialChainCached consumes it.
// @p fileVersion: the header version of the file the text came from; a version below current runs
// the migration chain on the decoded material (kMaterialMigrations).
static void ApplyMaterialText(const char* text, size_t textLen,
                              MaterialComponent& mat, bool clearBag, uint16_t fileVersion)
{
    // Texture-slot key -> slot lookup table.
    struct TexEntry { const char* key; MaterialComponent::TEXTURESLOT slot; };
    static const TexEntry kTexSlots[] = {
#define TEX_ENTRY_(e, label, defpath) { "tex_" #e, MaterialComponent::e },
        MATERIAL_TEXTURE_SLOTS(TEX_ENTRY_)
#undef TEX_ENTRY_
    };
    constexpr int kTexCount = static_cast<int>(sizeof(kTexSlots) / sizeof(kTexSlots[0]));

    auto findSchemaField = [](const std::string& key) -> const MaterialSchema::FieldDesc*
    {
        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
            if (key == f.name) return &f;
        return nullptr;
    };

    if (clearBag)
    {
        // Full load replaces the flexible bag. Reflection sync re-adds any
        // missing keys (with defaults) after load, so clearing here makes load a
        // true "disk is authoritative" replace rather than a merge.
        mat.customParams.clear();
        mat.customTextures.clear();
    }

    std::istringstream ss(std::string(text, textLen));
    std::string line;
    bool sawCparam = false;   // any cparam_ line present in this file
    bool sawFmt    = false;   // cparamFmt marker present (diff-against-default format)
    bool sawMatFmt3 = false;  // matfmt >= 3: the version 1 step skips the shaderType fold
    // Malformed-value census. In a MERGE a swallowed parse error looks exactly like "never
    // overridden", so a silent catch would turn a corrupt file into a material that merely looks
    // inherited; count them and log once.
    int  malformedValues = 0;
    std::string firstMalformedKey;

    while (std::getline(ss, line))
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;

        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = Trim(t.substr(0, eq));
        const std::string val = Trim(t.substr(eq + 1));
        if (key.empty() || val.empty()) continue;
        if (key == "parent") continue; // consumed by ResolveMaterialChainCached

        // `<none>` = an explicitly CLEARED path. The empty-value guard above must stay: five of six
        // texture slots construct with a real default path, so an empty value in a master must keep
        // falling back. The token is additive: no master on disk contains one.
        const bool  valIsNone = (val == "<none>");
        const std::string sval = valIsNone ? std::string{} : val;

        // `imatfmt = 2` marks the EXPLICIT-override format. Consumed by
        // CollectAuthoredKeys, not here; skip so it never reaches dispatch.
        if (key == "imatfmt") continue;

        // `unset = cparam_X` / `ctex_X` / `sswitch_X` removes an INHERITED custom key. Only
        // meaningful while merging (clearBag == false, the bag holds the parent's entries); harmless
        // in a full load.
        if (key == "unset")
        {
            if (val.rfind("cparam_", 0) == 0) mat.customParams.erase(val.substr(7));
            else if (val.rfind("ctex_", 0) == 0) mat.customTextures.erase(val.substr(5));
            else if (val.rfind("sswitch_", 0) == 0) mat.staticSwitches.erase(val.substr(8));
            continue;
        }

        try
        {
            // ---- Schema-driven parameter dispatch ----
            if (const auto* f = findSchemaField(key))
            {
                float* base = MaterialSchema::FieldPtr(mat, *f);
                switch (f->kind)
                {
                case MaterialSchema::Kind::Float:
                    base[0] = std::stof(val);
                    break;
                case MaterialSchema::Kind::Color3:
                case MaterialSchema::Kind::Color4:
                {
                    float r = 0, g = 0, b = 0, a = 0;
                    if (sscanf_s(val.c_str(), "%f %f %f %f", &r, &g, &b, &a) == 4)
                    {
                        base[0] = r; base[1] = g; base[2] = b; base[3] = a;
                    }
                    break;
                }
                case MaterialSchema::Kind::FloatComponent:
                    // UI alias written by its parent Color3/4 row; ignored on load.
                    break;
                }
                continue;
            }

            // ---- Meta / mode (not in schema) ----
            if (key == "shaderType")
            {
                // Range-checked like `domain`: the value feeds `1u << shaderType` in the Inspector's schema
                // filter (EditorLayer), so an out-of-range value below 32 matches no row's visibleMask and
                // silently hides every material parameter, and >= 32 is undefined behaviour.
                const int raw = std::stoi(val);
                if (raw >= 0 && raw < static_cast<int>(MaterialComponent::SHADERTYPE_COUNT))
                    mat.shaderType = static_cast<MaterialComponent::SHADERTYPE>(raw);
                else
                    LOG_WARNING("LoadMaterial: shaderType = %d out of range, keeping %d",
                                raw, static_cast<int>(mat.shaderType));
            }
            else if (key == "blendMode")
            {
                // Range-checked: the Inspector filters rows on `1u << userBlendMode` (BLD_BIT).
                const int raw = std::stoi(val);
                if (raw >= 0 && raw < static_cast<int>(BlendMode::Count))
                    mat.userBlendMode = static_cast<BlendMode>(raw);
                else
                    LOG_WARNING("LoadMaterial: blendMode = %d out of range, keeping %d",
                                raw, static_cast<int>(mat.userBlendMode));
            }
            else if (key == "shadowCullMode")mat.shadowCullMode = static_cast<ShadowCullMode>(std::stoi(val));
            else if (key == "domain")
            {
                const int raw = std::stoi(val);
                if (raw >= 0 && Resource::IsValidMaterialDomain(static_cast<uint32_t>(raw)))
                    mat.domain = static_cast<Resource::MaterialDomain>(raw);
                else
                {
                    LOG_WARNING("LoadMaterial: domain = %d out of range, using Opaque", raw);
                    mat.domain = Resource::MaterialDomain::Opaque;
                }
            }
            else if (key == "decalWriteMask")mat.decalWriteMask = static_cast<uint32_t>(std::stoul(val));
            else if (key == "decalAngleFade")mat.decalAngleFadeStart = std::stof(val);
            else if (key == "decalSortLayer")mat.decalSortLayer = std::stoi(val);
            else if (key == "castShadow")    mat.SetCastShadow(val == "1");
            else if (key == "receiveShadow") mat.SetReceiveShadow(val == "1");
            else if (key == "doubleSided")
            {
                if (val == "1") mat.flags |= MaterialComponent::DOUBLE_SIDED;
                else            mat.flags &= ~MaterialComponent::DOUBLE_SIDED;
            }
            else if (key == "outline")
            {
                if (val == "1") mat.flags |= MaterialComponent::OUTLINE;
                else            mat.flags &= ~MaterialComponent::OUTLINE;
            }
            else if (key == "outlineScreenSpace")
            {
                if (val == "1") mat.flags |=  MaterialComponent::OUTLINE_SCREENSPACE;
                else            mat.flags &= ~MaterialComponent::OUTLINE_SCREENSPACE;
            }
            else if (key == "excludeFromSsao")
            {
                if (val == "1") mat.flags |=  MaterialComponent::EXCLUDE_FROM_SSAO;
                else            mat.flags &= ~MaterialComponent::EXCLUDE_FROM_SSAO;
            }
            else if (key == "hairMask")
            {
                if (val == "1") mat.flags |=  MaterialComponent::HAIR_MASK;
                else            mat.flags &= ~MaterialComponent::HAIR_MASK;
            }
            else if (key == "sourceOutlineExempt")
            {
                if (val == "1") mat.flags |= MaterialComponent::SOURCE_OUTLINE_EXEMPT;
                else            mat.flags &= ~MaterialComponent::SOURCE_OUTLINE_EXEMPT;
            }
            else if (key == "nprToonShadowEdge")
            {
                if (val == "1") mat.flags |=  MaterialComponent::NPR_TOON_SHADOW_EDGE;
                else            mat.flags &= ~MaterialComponent::NPR_TOON_SHADOW_EDGE;
            }
            else if (key == "nprClampEnabled")
            {
                if (val == "1") mat.flags |=  MaterialComponent::NPR_BRIGHTNESS_CLAMP;
                else            mat.flags &= ~MaterialComponent::NPR_BRIGHTNESS_CLAMP;
            }
            else if (key == "useVertexColor")
            {
                if (val == "1") mat.flags |=  MaterialComponent::USE_VERTEXCOLORS;
                else            mat.flags &= ~MaterialComponent::USE_VERTEXCOLORS;
            }
            else if (key == "outlinePixels") mat.outlinePixels = std::stof(val);
            // ---- Custom shader + reflection-driven param bag ----
            else if (key == "useCustomShader") mat.useCustomShader = (val == "1");
            else if (key == "rtUseStandardMaterial") mat.rtUseStandardMaterial = (val == "1");
            else if (key == "customShaderPath")
            {
                std::string normalized = sval;   // <none> => cleared
                for (char& c : normalized) if (c == '\\') c = '/';
                mat.customShaderPath = std::move(normalized);
            }
            else if (key == "customVertexShaderPath")
            {
                std::string normalized = sval;   // <none> => cleared
                for (char& c : normalized) if (c == '\\') c = '/';
                mat.customVertexShaderPath = std::move(normalized);
            }
            else if (key == "materialGraph")
            {
                std::string normalized = sval;   // <none> => cleared
                for (char& c : normalized) if (c == '\\') c = '/';
                mat.materialGraph = std::move(normalized);
            }
            else if (key.rfind("sswitch_", 0) == 0 && key.size() > 8)
                mat.staticSwitches[key.substr(8)] = sval == "1" || sval == "true" || sval == "on";
            else if (key == "customShadingModel")
            {
                // Range-checked like `domain`: the value routes the deferred BRDF and the GBuffer's RT4
                // packing, and an out-of-range model indexes neither, leaving the pixel to each switch's
                // fallthrough. Realistic sources: a file from a newer build, a hand edit.
                const int raw = std::stoi(val);
                if (raw >= 0 && raw < static_cast<int>(ShadingModel::Count))
                    mat.customShadingModel = static_cast<ShadingModel>(raw);
                else
                    LOG_WARNING("Material: customShadingModel = %d is out of range, "
                                "keeping %s", raw,
                                ShadingModelName(mat.customShadingModel));
            }
            else if (key == "cparamFmt")
                sawFmt = true;  // value unused: presence marks the new format
            else if (key == "matfmt")
            {
                // Text generation; 3 = shaderType folded into customShadingModel. Read by the
                // version 1 step only: a header at version 2 or later is the generation itself.
                try { sawMatFmt3 = (std::stoi(val) >= 3); } catch (...) {}
            }
            else if (key.rfind("cparam_", 0) == 0)
            {
                sawCparam = true;
                const std::string name = key.substr(7);
                std::array<float, 4> v{};
                sscanf_s(val.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]);
                if (!name.empty()) mat.customParams[name] = v;
            }
            else if (key.rfind("ctex_", 0) == 0)
            {
                const std::string name = key.substr(5);
                // value = "<uvset> <path>"; path may contain spaces.
                uint32_t    uvset = 0;
                std::string path;
                const size_t sp = val.find(' ');
                if (sp != std::string::npos)
                {
                    uvset = static_cast<uint32_t>(std::stoul(val.substr(0, sp)));
                    path  = Trim(val.substr(sp + 1));
                    // "<uvset> <none>" = keep the uv set, clear the texture.
                    if (path == "<none>") path.clear();
                }
                else if (!val.empty() &&
                         val.find_first_not_of("0123456789") == std::string::npos)
                {
                    // A LONE NUMBER is the uvset with an EMPTY path, not a file called "0": an unset slot
                    // written as "<uvset> " trims to just the number. Read as a path, the renderer would
                    // Acquire("0"), a load that never resolves, so TextureSystem::IsReady never latches,
                    // m_matTexUnsettled never reaches 0 and the retained-instance fast path stays off for the
                    // whole scene.
                    uvset = static_cast<uint32_t>(std::stoul(val));
                    path.clear();
                }
                else
                {
                    path = sval; // missing uvset token; <none> => cleared
                }
                for (char& c : path) if (c == '\\') c = '/';
                if (!name.empty())
                {
                    MaterialComponent::TextureMap t;
                    t.name  = std::move(path);
                    t.uvset = uvset;
                    mat.customTextures[name] = std::move(t);
                }
            }
            // ---- UV tiling / offset ----
            else if (key == "texTiling")
            {
                float u = 1, v = 1;
                if (sscanf_s(val.c_str(), "%f %f", &u, &v) == 2) { mat.texMulAdd.x = u; mat.texMulAdd.y = v; }
            }
            else if (key == "texOffset")
            {
                float u = 0, v = 0;
                if (sscanf_s(val.c_str(), "%f %f", &u, &v) == 2) { mat.texMulAdd.z = u; mat.texMulAdd.w = v; }
            }
            else
            {
                // ---- Texture slots ----
                for (int i = 0; i < kTexCount; ++i)
                {
                    if (key == kTexSlots[i].key)
                    {
                        // Normalize separators so TextureSystem hashes match a .imat saved with backslashes. sval,
                        // so an instance's `tex_X = <none>` clears the slot its parent bound.
                        std::string normalized = sval;
                        for (char& c : normalized) if (c == '\\') c = '/';
                        mat.textures[kTexSlots[i].slot].name = std::move(normalized);
                        break;
                    }
                }
            }
        }
        catch (...)
        {
            // Value did not parse (stof/stoi threw). Skip it, but count it.
            if (malformedValues++ == 0) firstMalformedKey = key;
        }
    }

    if (malformedValues != 0)
        LOG_WARNING("MaterialSerializer: %d malformed value(s) skipped while applying "
                    "material text (first key '%s'); those parameters keep their %s",
                    malformedValues, firstMalformedKey.c_str(),
                    clearBag ? "constructed defaults" : "inherited values");

    // Flag old-format custom-param files (cparam_ data but no cparamFmt marker) via
    // customParamsLegacyFormat, so reflection Sync migrates their (1,1,1,1)-style sentinels to
    // real defaults. A full (clearBag) load sets the flag outright; a delta merge can only RAISE
    // it (an old-format parent still needs migration).
    const bool legacyThisFile = (sawCparam && !sawFmt);
    if (clearBag)            mat.customParamsLegacyFormat = legacyThisFile;
    else if (legacyThisFile) mat.customParamsLegacyFormat = true;

    // ---- Migration chain: lift an older file to the current version ----
    MaterialDecode decoded{ mat, sawMatFmt3 };
    MigrateAsset<MAGIC_MATERIAL>(kMaterialMigrations, fileVersion, decoded);
    // Re-derive the tombstone from the now-authoritative model, unconditionally (current files
    // included): the two axes then cannot disagree in memory, which the renderer asserts. This
    // also normalises the retired parallax value to plain PBR.
    mat.SyncLegacyShaderType();
}

// Returns the `parent = ...` value (forward-slashed) or "" if the blob is a
// standalone material rather than a .imatinst instance.
static std::string FindParentKey(const char* text, size_t textLen)
{
    std::istringstream ss(std::string(text, textLen));
    std::string line;
    while (std::getline(ss, line))
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(t.substr(0, eq)) != "parent") continue;
        std::string p = Trim(t.substr(eq + 1));
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }
    return {};
}

// Collect the keys a material file AUTHORS, straight from its text: the authority for "what
// does this instance override", which the resolved MaterialComponent cannot answer after the
// parent merge.
// Excluded:
//   parent / $g_*       references, not parameters
//   imatfmt             the format marker itself (sets @p explicitFormatOut)
//   cparamFmt, matfmt   format markers too: properties of the file
//   unset               collected into @p unsetOut
// A value-diff file (a `parent` key, no `imatfmt`) gets every remaining key as an override.
// Deliberately literal: that writer emits only differing keys anyway, so the sets agree except
// for `domain`, which it writes unconditionally; such an instance keeps pinning its domain.
static void CollectAuthoredKeys(const char* text, size_t textLen,
                                std::unordered_set<std::string>& keysOut,
                                std::vector<std::string>&        unsetOut,
                                bool&                            explicitFormatOut)
{
    std::istringstream ss(std::string(text, textLen));
    std::string line;
    while (std::getline(ss, line))
    {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(t.substr(0, eq));
        if (key.empty()) continue;
        if (key == "imatfmt") { explicitFormatOut = true; continue; }
        if (key == "unset")
        {
            const std::string v = Trim(t.substr(eq + 1));
            if (!v.empty()) unsetOut.push_back(v);
            continue;
        }
        // Format markers are properties of the FILE, not overrides: an instance whose only lines are
        // markers authors nothing (the material_instance scenario asserts a fresh instance authors no
        // keys).
        if (key == "parent" || key == "cparamFmt" || key == "matfmt") continue;
        if (key.rfind("$g_", 0) == 0) continue;
        keysOut.insert(key);
    }
}

// Resolve a material file (.imat or .imatinst) into @p mat. For an instance the parent chain
// is resolved first (recursively), then this file's deltas are MERGED on top; a standalone
// material applies as a full (bag-clearing) load. Returns false only when @p path itself
// cannot be read or has an invalid header; a broken or too-deep parent chain sets @p complete
// to false.
static bool ResolveMaterialChainCached(
    const std::string& path,
    MaterialComponent& mat,
    int depth,
    std::vector<MaterialDependencyStamp>& dependencies,
    bool& complete,
    bool* rootCacheHit = nullptr,
    MaterialLoadInfo* info = nullptr)   // filled for the LEAF only (depth == 0)
{
    const std::string canonicalPath = CanonicalMaterialPath(path);

    // An editor's live version of this file (Resource/MaterialLiveOverlay.h) wins over the file
    // and the cache. Never cached itself; recorded as a dependency, so an instance resolved on
    // top of it is dropped with it.
    if (t_liveBypass == 0 && g_liveCount.load(std::memory_order_acquire) != 0)
    {
        std::shared_ptr<const LiveEntry> live;
        {
            std::lock_guard lock(g_liveMutex);
            if (auto it = g_live.find(canonicalPath); it != g_live.end()) live = it->second;
        }
        if (live)
        {
            mat = live->material;
            dependencies.push_back(CaptureDependencyStamp(canonicalPath));
            if (rootCacheHit) *rootCacheHit = true;   // a memory read, not a parse: no "loaded" line
            if (info && depth == 0) *info = live->info;
            return true;
        }
    }

    MaterialCacheEntry cached;
    if (TryGetCachedMaterial(canonicalPath, cached))
    {
        mat = cached.material;
        dependencies.insert(dependencies.end(),
                            cached.dependencies.begin(), cached.dependencies.end());
        if (rootCacheHit) *rootCacheHit = true;
        // The authored set rides along, or a cache hit would look like a
        // material that overrides nothing and re-saving it would erase the file.
        if (info && depth == 0) *info = cached.info;
        return true;
    }

    const size_t dependencyStart = dependencies.size();
    dependencies.push_back(CaptureDependencyStamp(canonicalPath));

    std::vector<uint8_t> data;
    ++LoadCounters::ThisThread().materialParses;   // a cache miss: this file is read and parsed
    if (!::Resource::AssetFS::Get().ReadFile(canonicalPath, data))
    {
        complete = false;
        LOG_ERROR("MaterialSerializer: cannot open '%s'", canonicalPath.c_str());
        return false;
    }

    std::string headerWhy;
    if (!ValidateHeader(data.data(), data.size(), MAGIC_MATERIAL, headerWhy))
    {
        complete = false;
        LOG_ERROR("MaterialSerializer: invalid material header in '%s': %s", canonicalPath.c_str(),
                  headerWhy.c_str());
        return false;
    }
    const uint16_t fileVersion = GetHeader(data.data())->version;

    // Resolve ONCE, then feed the repaired text to both the parent lookup and the field
    // application below: they must agree on what this material says.
    std::size_t       refsRepaired = 0;
    const std::string body = AssetRefs::ResolveImatText(
        std::string(GetPayloadText(data.data(), data.size())), canonicalPath, &refsRepaired);
    if (refsRepaired)
        LOG_WARNING("MaterialSerializer: '%s' — %zu reference(s) repaired by guid. "
                    "Re-save the material to make the repair permanent.",
                    canonicalPath.c_str(), refsRepaired);
    const char*  text    = body.c_str();
    const size_t textLen = body.size();

    if (info && depth == 0)
    {
        info->overrideKeys.clear();
        info->unsetKeys.clear();
        info->explicitFormat = false;
        CollectAuthoredKeys(text, textLen, info->overrideKeys,
                            info->unsetKeys, info->explicitFormat);
    }

    const std::string parent = FindParentKey(text, textLen);
    if (info && depth == 0) info->parentPath = parent;

    // An .imatinst with NO parent key is a flattened instance: a standalone dump severed its
    // link. It still loads and renders, so nothing but this warning would notice.
    if (parent.empty() && canonicalPath.size() >= 9 &&
        canonicalPath.compare(canonicalPath.size() - 9, 9, ".imatinst") == 0)
        LOG_WARNING("MaterialSerializer: '%s' is named as an instance but has no "
                    "parent key — it was flattened into a standalone material and "
                    "no longer follows anything.", canonicalPath.c_str());
    if (!parent.empty() && depth < kMaxInstanceDepth)
    {
        // Resolve parent (and its ancestors) into mat first, then merge our deltas.
        if (!ResolveMaterialChainCached(parent, mat, depth + 1,
                                        dependencies, complete))
        {
            complete = false;
            LOG_WARNING("MaterialSerializer: instance '%s' parent '%s' failed to "
                        "resolve; applying deltas onto default state",
                        canonicalPath.c_str(), parent.c_str());
        }
        ApplyMaterialText(text, textLen, mat, /*clearBag=*/false, fileVersion);
    }
    else
    {
        if (!parent.empty())
        {
            complete = false;
            LOG_WARNING("MaterialSerializer: instance parent chain too deep at '%s' "
                        "(cycle?) — applying as standalone", canonicalPath.c_str());
        }
        ApplyMaterialText(text, textLen, mat, /*clearBag=*/true, fileVersion);
    }

    // Only complete chains are immutable asset snapshots. A missing/cyclic
    // parent deliberately falls back to defaults for this call but is retried
    // next time instead of poisoning the cache. Nor is a LiveMaterialBypass read
    // cached while the overlay holds anything: it may have resolved a parent the
    // overlay would have served, and every other reader must still see that one.
    if (complete)
    {
        std::vector<MaterialDependencyStamp> ownDependencies(
            dependencies.begin() + static_cast<std::ptrdiff_t>(dependencyStart),
            dependencies.end());
        if (t_liveBypass != 0 && g_liveCount.load(std::memory_order_acquire) != 0)
        {
            RecordDependents(canonicalPath, ownDependencies);
            return true;
        }
        // Re-derive the authored set for the entry even when the caller did not
        // ask for it: the next reader of this cache line may well ask, and it has
        // no text left to derive it from.
        MaterialLoadInfo entryInfo;
        entryInfo.parentPath = parent;
        CollectAuthoredKeys(text, textLen, entryInfo.overrideKeys,
                            entryInfo.unsetKeys, entryInfo.explicitFormat);
        StoreCachedMaterial(canonicalPath, mat, ownDependencies, entryInfo);
    }
    return true;
}

bool LoadMaterial(const std::string& path, MaterialComponent& mat,
                  MaterialLoadInfo* out)
{
    PROF_SCOPE("Mat.LoadMaterial");
    // Instance-aware: .imat loads directly; .imatinst resolves its parent chain
    // and merges deltas. Every existing caller (scene/prefab load, editor, VFX,
    // mesh spawner) gets instance support transparently through this one entry.
    MaterialComponent resolved{};
    std::vector<MaterialDependencyStamp> dependencies;
    bool complete = true;
    bool cacheHit = false;
    if (!ResolveMaterialChainCached(path, resolved, 0, dependencies,
                                    complete, &cacheHit, out))
        return false;

    // A broken parent chain still returns TRUE: the MaterialRef deserializer is
    // `if (LoadMaterial(...)) { ... }`, so false would leave the entity with NO
    // MaterialComponent ("renders nothing" instead of "renders white"). Callers surface
    // parentResolved instead.
    if (out) out->parentResolved = complete;

    mat = std::move(resolved);
    mat.SetDirty();
    // Cache hits are ordinary reads (including the sky's hot-reload polling),
    // not file loads. Avoid formatting/enqueuing a log on every cached read.
    if (!cacheHit)
        LOG_INFO("MaterialSerializer: loaded '%s' (parsed)",
                 CanonicalMaterialPath(path).c_str());
    return true;
}

std::uint64_t MaterialContentGeneration()
{
    return g_materialContentGen.load(std::memory_order_relaxed);
}

void ClearMaterialCache()
{
    // BUMPED UNCONDITIONALLY, and before the early-out below: an empty cache
    // still means "every resolved copy anybody is holding is now unbacked".
    g_materialContentGen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_materialCacheMutex);
    if (!g_materialCache.empty())
        ++g_materialCacheStats.invalidations;
    g_materialCache.clear();
    // The index goes too: it is only ever added to, so a world reload would
    // otherwise accumulate every material the process has ever resolved.
    g_materialDependents.clear();
}

std::size_t InvalidateMaterialsDependingOn(const std::string& changedPath)
{
    const std::string needle = CanonicalMaterialPath(changedPath);
    if (needle.empty()) return 0;
    // The content signal, bumped whether or not anything was CACHED - a holder
    // of a resolved copy (the asset preview's parts) is stale either way.
    g_materialContentGen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(g_materialCacheMutex);
    const std::size_t dropped = InvalidateDependentsLocked(needle);
    // The file itself may have no index entry yet (never resolved), so drop it
    // explicitly as well.
    const std::size_t self = g_materialCache.erase(needle);
    if (dropped + self) ++g_materialCacheStats.invalidations;
    return dropped + self;
}

// ---- The live overlay (Resource/MaterialLiveOverlay.h) ----
void SetLiveMaterial(const std::string& path, const MaterialComponent& mat, const MaterialLoadInfo& info)
{
    const std::string canonical = CanonicalMaterialPath(path);
    if (canonical.empty()) return;
    auto entry = std::make_shared<LiveEntry>();   // the heavy copy happens off the lock
    entry->material = mat;
    entry->info     = info;
    // The editor's staged copy carries its edit marks. What the overlay serves is
    // "the file", as a load would read it: an entity given it has NO edits of its
    // own (ASSET_DIRTY would make the next live push skip it and a scene save
    // write it out as a per-entity override).
    entry->material.SetAssetDirty(false);
    {
        std::lock_guard lock(g_liveMutex);
        g_live[canonical] = std::move(entry);
        g_liveCount.store(g_live.size(), std::memory_order_release);
    }
    InvalidateMaterialsDependingOn(canonical);
}

bool ClearLiveMaterial(const std::string& path)
{
    const std::string canonical = CanonicalMaterialPath(path);
    bool had = false;
    {
        std::lock_guard lock(g_liveMutex);
        had = g_live.erase(canonical) != 0;
        g_liveCount.store(g_live.size(), std::memory_order_release);
    }
    if (had) InvalidateMaterialsDependingOn(canonical);
    return had;
}

bool GetLiveMaterial(const std::string& path, MaterialComponent& mat, MaterialLoadInfo* info)
{
    if (g_liveCount.load(std::memory_order_acquire) == 0) return false;
    std::shared_ptr<const LiveEntry> live;
    {
        std::lock_guard lock(g_liveMutex);
        if (auto it = g_live.find(CanonicalMaterialPath(path)); it != g_live.end()) live = it->second;
    }
    if (!live) return false;
    mat = live->material;
    if (info) *info = live->info;
    return true;
}

std::size_t LiveMaterialCount()
{
    return g_liveCount.load(std::memory_order_acquire);
}

LiveMaterialBypass::LiveMaterialBypass()  { ++t_liveBypass; }
LiveMaterialBypass::~LiveMaterialBypass() { --t_liveBypass; }

std::vector<std::string> MaterialsDependingOn(const std::string& changedPath)
{
    const std::string needle = CanonicalMaterialPath(changedPath);
    std::vector<std::string> out;
    if (needle.empty()) return out;

    // Straight off the reverse index, so the answer survives a save's cache invalidation (see
    // g_materialDependents).
    std::lock_guard lock(g_materialCacheMutex);
    const auto it = g_materialDependents.find(needle);
    if (it == g_materialDependents.end()) return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

std::string MaterialIndexSpelling(const std::string& path)
{
    const std::string needle = CanonicalMaterialPath(path);
    if (needle.empty()) return needle;
    auto lower = [](std::string s)
    {
        for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::lock_guard lock(g_materialCacheMutex);
    if (g_materialDependents.count(needle) || g_materialCache.count(needle)) return needle;
    const std::string low = lower(needle);
    for (const auto& kv : g_materialDependents)
        if (kv.first.size() == low.size() && lower(kv.first) == low) return kv.first;
    for (const auto& kv : g_materialCache)
        if (kv.first.size() == low.size() && lower(kv.first) == low) return kv.first;
    return needle;
}

bool IsMaterialCached(const std::string& path)
{
    const std::string spelled = MaterialIndexSpelling(path);
    std::lock_guard lock(g_materialCacheMutex);
    return g_materialCache.count(spelled) != 0;
}

MaterialCacheStats GetMaterialCacheStats()
{
    std::lock_guard lock(g_materialCacheMutex);
    MaterialCacheStats stats = g_materialCacheStats;
    stats.entries = g_materialCache.size();
    return stats;
}

// ==== Material instances (.imatinst) ====
bool SaveMaterialInstance(const std::string& path, const std::string& parentPath,
                          const MaterialComponent& mat,
                          const std::unordered_set<std::string>* overrideKeys,
                          const std::vector<std::string>*        unsetKeys)
{
    // ---- Cycle guard ----
    // An instance whose ancestor chain reaches back to itself makes ResolveMaterialChainCached
    // bottom out on kMaxInstanceDepth and fall back to a default material: a white surface plus
    // one WARNING from a save that reported success. Refuse it at authoring time.
    const std::string selfCanon = CanonicalMaterialPath(path);
    if (CanonicalMaterialPath(parentPath) == selfCanon)
    {
        LOG_ERROR("MaterialSerializer: '%s' cannot be its own parent", path.c_str());
        return false;
    }
    {
        std::string ancestor = GetMaterialParent(parentPath);
        for (int depth = 0; !ancestor.empty() && depth < kMaxInstanceDepth; ++depth)
        {
            if (CanonicalMaterialPath(ancestor) == selfCanon)
            {
                LOG_ERROR("MaterialSerializer: parent chain of '%s' cycles back through '%s'",
                          parentPath.c_str(), path.c_str());
                return false;
            }
            ancestor = GetMaterialParent(ancestor);
        }
    }

    // Resolve the EFFECTIVE parent (parent + all its ancestors): the baseline the delta is
    // written against. A chain that does not fully resolve is refused.
    MaterialComponent baseMat{};
    std::vector<MaterialDependencyStamp> dependencies;
    bool complete = true;
    if (!ResolveMaterialChainCached(parentPath, baseMat, 0,
                                    dependencies, complete) || !complete)
    {
        LOG_ERROR("MaterialSerializer: instance parent '%s' failed to resolve", parentPath.c_str());
        return false;
    }

    std::string parentNorm = parentPath;
    for (char& c : parentNorm) if (c == '\\') c = '/';

    // Stamp guids as SaveMaterial does: without them the instance's `parent` (and every
    // overridden texture) is a bare path, and the first rename of the master orphans it.
    // AugmentImatText emits the `$g_parent` token ResolveImatText uses to recover it on load.
    const std::string text        = AssetRefs::AugmentImatText(
                                        BuildTextImpl(mat, &baseMat, &parentNorm,
                                                      overrideKeys, unsetKeys));
    const uint32_t    textLength  = static_cast<uint32_t>(text.size());
    const uint32_t    payloadSize = textLength + 1;

    MaterialMetadata meta{};
    meta.textLength = textLength;

    AssetHeader hdr{};
    hdr.magic        = MAGIC_MATERIAL;
    hdr.version      = CurrentVersion(MAGIC_MATERIAL);
    hdr.resourceType = static_cast<uint16_t>(ResourceType::Material);
    hdr.metadataSize = sizeof(MaterialMetadata);
    hdr.dataSize     = payloadSize;

    auto f = Sys::OpenOfstream(path, std::ios::binary);
    if (!f)
    {
        LOG_ERROR("MaterialSerializer: cannot open '%s' for writing", path.c_str());
        return false;
    }
    f.write(reinterpret_cast<const char*>(&hdr),  sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&meta), sizeof(meta));
    f.write(text.c_str(), textLength);
    const char nul = '\0';
    f.write(&nul, 1);
    f.flush();
    if (!f.good())
    {
        LOG_ERROR("MaterialSerializer: write error for '%s'", path.c_str());
        return false;
    }

    f.close();
    InvalidateMaterialsDependingOn(path);
    LOG_INFO("MaterialSerializer: saved instance '%s' (parent '%s', %s, %u bytes text)",
             path.c_str(), parentNorm.c_str(),
             overrideKeys ? "explicit override set" : "legacy value diff", textLength);
    return true;
}

std::string GetMaterialParent(const std::string& path)
{
    std::vector<uint8_t> data;
    if (!::Resource::AssetFS::Get().ReadFile(path, data)) return {};
    if (!ValidateHeader(data.data(), data.size(), MAGIC_MATERIAL)) return {};
    // Resolve first: a `parent` whose .imat moved must still be findable, or
    // the whole instance chain collapses to defaults.
    const std::string body = AssetRefs::ResolveImatText(std::string(GetPayloadText(data.data(), data.size())), path);
    return FindParentKey(body.c_str(), body.size());
}

// ==== Upgrade ====
// Whether a material text carries `matfmt` >= 3 (what ApplyMaterialText reads for the version 1 step).
static bool TextSaysFolded(std::string_view text)
{
    std::istringstream ss{ std::string(text) };
    std::string line;
    while (std::getline(ss, line))
    {
        const std::string t = Trim(line);
        const size_t eq = t.find('=');
        if (t.empty() || t[0] == '#' || eq == std::string::npos || Trim(t.substr(0, eq)) != "matfmt")
            continue;
        try { return std::stoi(Trim(t.substr(eq + 1))) >= 3; } catch (...) { return false; }
    }
    return false;
}

bool UpgradeMaterialBlob(const std::string& path, const std::vector<uint8_t>& in,
                         std::vector<uint8_t>& out, std::string& why)
{
    if (!ValidateHeader(in.data(), in.size(), MAGIC_MATERIAL, why))
        return false;
    AssetHeader hdr{};
    std::memcpy(&hdr, in.data(), sizeof(hdr));
    out = in;

    const std::string body     = AssetRefs::ResolveImatText(std::string(GetPayloadText(in.data(), in.size())), path);
    const std::string parent   = FindParentKey(body.c_str(), body.size());
    const bool        folded   = TextSaysFolded(body);
    const uint16_t    current  = CurrentVersion(MAGIC_MATERIAL);
    std::string       reencoded;   // the text to write when the step changes the material

    if (!parent.empty() && !folded)
    {
        // An instance is a delta merged onto its resolved parent, and the version 1 step reads the merged
        // lighting axis: it is a no-op for this file when the merge decodes the same with and without the
        // fold. Only then may the delta keep its bytes; otherwise the editor's save is the way to lift it.
        MaterialComponent                    before;
        std::vector<MaterialDependencyStamp> dependencies;
        bool                                 complete = true;
        if (!ResolveMaterialChainCached(parent, before, 1, dependencies, complete) || !complete)
        {
            why = "its parent chain does not resolve, so the lighting axis it merges onto is unknown";
            return false;
        }
        MaterialComponent after = before;
        ApplyMaterialText(body.c_str(), body.size(), before, false, hdr.version);
        ApplyMaterialText(body.c_str(), body.size(), after, false, current);
        if (BuildMaterialText(before) != BuildMaterialText(after))
        {
            why = "folding its lighting axis changes the merged material: open it in the editor and save it";
            return false;
        }
    }
    else if (parent.empty())
    {
        // A standalone text that says matfmt >= 3 is already current but for its number: everything else
        // stays byte for byte, once the current reader is shown to decode it the same. A text that leaves
        // the lighting axis on shaderType is written through the writer after the fold, and read back.
        MaterialComponent before, after;
        ApplyMaterialText(body.c_str(), body.size(), before, true, hdr.version);
        if (folded)
            ApplyMaterialText(body.c_str(), body.size(), after, true, current);
        else
        {
            reencoded = BuildText(before);
            ApplyMaterialText(reencoded.c_str(), reencoded.size(), after, true, current);
        }
        if (BuildMaterialText(before) != BuildMaterialText(after))
        {
            why = "the current reader does not decode the upgraded material as the original";
            return false;
        }
    }

    if (!reencoded.empty())
    {
        const std::string& text = reencoded;
        const size_t metaEnd = sizeof(AssetHeader) + hdr.metadataSize;
        MaterialMetadata meta{};
        if (hdr.metadataSize < sizeof(MaterialMetadata) || metaEnd > in.size())
        {
            why = "the material metadata is shorter than MaterialMetadata";
            return false;
        }
        std::memcpy(&meta, in.data() + sizeof(AssetHeader), sizeof(meta));
        meta.textLength = static_cast<uint32_t>(text.size());
        hdr.dataSize    = static_cast<uint32_t>(text.size()) + 1;
        out.assign(in.begin(), in.begin() + static_cast<std::ptrdiff_t>(metaEnd));
        std::memcpy(out.data() + sizeof(AssetHeader), &meta, sizeof(meta));
        out.insert(out.end(), text.begin(), text.end());
        out.push_back(0);
    }
    hdr.version = CurrentVersion(MAGIC_MATERIAL);
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    return true;
}

} // namespace Resource
