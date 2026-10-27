#include "Editor/MaterialInstanceRepair.h"

#include "ECS/Components.h"
#include "ECS/MaterialReflectionSync.h"
#include "Graphics/ShaderLibrary.h"
#include "Renderer/Renderer.h"   // RegisterMaterialShader
#include "Renderer/MaterialPermutation.h"   // MaterialShaderReflection
#include "Resource/MaterialSerializer.h"
#include "System/Check.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace Editor
{
namespace
{
    namespace fs = std::filesystem;

    bool ArrEq(const std::array<float, 4>& a, const std::array<float, 4>& b)
    {
        for (int i = 0; i < 4; ++i)
            if (std::fabsf(a[i] - b[i]) > 1e-5f) return false;
        return true;
    }

    // Every key authored anywhere UP the chain from @p parentPath. A mark an ancestor also
    // declares is a deliberate re-override, never a candidate for dropping.
    std::unordered_set<std::string> AncestorAuthoredKeys(const std::string& parentPath)
    {
        std::unordered_set<std::string> out;
        std::string cur = parentPath;
        for (int depth = 0; depth < 16 && !cur.empty(); ++depth)
        {
            MaterialComponent            m{};
            Resource::MaterialLoadInfo   info;
            if (!Resource::LoadMaterial(cur, m, &info)) break;
            out.insert(info.overrideKeys.begin(), info.overrideKeys.end());
            if (info.parentPath == cur) break;   // paranoia; the saver rejects self-parents
            cur = info.parentPath;
        }
        return out;
    }

    // A shader path -> its dynamic id: looked up read-only first; a shader nothing has
    // registered yet gets its id from Renderer::RegisterMaterialShader (game half). One
    // registry serves every library, so the id is the same whichever library asks.
    uint32_t ResolveShaderId(ShaderLibrary& lib, Renderer* renderer,
                             const std::string& path, RHI::ShaderStage stage)
    {
        const uint32_t found = lib.FindDynamic(path.c_str(), stage);
        if (found != ShaderLibrary::kInvalidDynShaderID) return found;
        if (!renderer) return ShaderLibrary::kInvalidDynShaderID;
        return renderer->RegisterMaterialShader(path.c_str(), stage);
    }

    // The shader's declared defaults, from reflecting the compiled shader. Sync() runs onto a
    // THROWAWAY material so the defaults land in its meta without touching anything real: the
    // same seeding step that causes the accidental marks, used here to recognise them.
    bool CollectShaderDefaults(ShaderLibrary& lib, Renderer* renderer,
                               const MaterialComponent& mat,
                               MaterialComponent& probeOut)
    {
        bool any = false;
        struct Stage { const std::string* path; RHI::ShaderStage stage; };
        const Stage stages[] = {
            { &mat.customShaderPath,       RHI::ShaderStage::PS },
            { &mat.customVertexShaderPath, RHI::ShaderStage::VS },
        };
        for (const Stage& s : stages)
        {
            if (s.path->empty()) continue;
            const uint32_t id = ResolveShaderId(lib, renderer, *s.path, s.stage);
            if (id == ShaderLibrary::kInvalidDynShaderID) continue;
            // The renderer's own reflection for this material: what its Sync seeded is what this
            // probe must recognise.
            const ShaderReflect::Reflection* refl = MaterialShaderReflection(lib, id, mat);
            if (!refl) continue;
            MaterialReflectionSync::Sync(probeOut, *refl);
            any = true;
        }
        return any;
    }

    void BackupFile(const fs::path& p)
    {
        std::error_code ec;
        fs::copy_file(p, fs::path(p).concat(".bak"),
                      fs::copy_options::overwrite_existing, ec);
        if (ec)
            LOG_WARNING("MaterialInstanceRepair: could not back up '%s' (%s)",
                        Sys::PathToUtf8(p).c_str(), ec.message().c_str());
    }
}

MaterialInstanceRepairReport RepairMaterialInstances(
    ShaderLibrary& lib, const std::string& root, bool apply, Renderer* renderer)
{
    MaterialInstanceRepairReport report;

    std::error_code ec;
    if (!fs::is_directory(Sys::U8Path(root), ec))
    {
        LOG_WARNING("MaterialInstanceRepair: '%s' is not a directory", root.c_str());
        return report;
    }

    for (fs::recursive_directory_iterator it(Sys::U8Path(root), ec), end;
         it != end && !ec; it.increment(ec))
    {
        if (!it->is_regular_file(ec)) continue;
        if (Sys::PathToUtf8(it->path().extension()) != ".imatinst") continue;

        MaterialInstanceRepairReport::Item item;
        item.path = Sys::RelativizeAssetPath(Sys::PathToUtf8(it->path()));
        ++report.scanned;

        MaterialComponent          mat{};
        Resource::MaterialLoadInfo info;
        if (!Resource::LoadMaterial(item.path, mat, &info))
        {
            item.skipReason = "will not load";
        }
        else if (info.parentPath.empty())
        {
            // Flattened (the audit tool reports it too): nothing records its parent, so there is
            // nothing here to repair.
            item.skipReason = "flattened (no parent) — re-pointing it is authoring, not migration";
        }
        else if (!info.parentResolved)
        {
            // Its values are defaults right now, so "equals the shader default"
            // would be true of everything and mean nothing.
            item.skipReason = "parent chain does not resolve";
        }
        else if (info.explicitFormat)
        {
            item.skipReason = "already explicit (imatfmt = 2) — its marks are authored";
        }
        else if (!mat.useCustomShader || mat.customShaderPath.empty())
        {
            // Only the custom-param path was mis-diffed; scalar marks on a stock material were derived
            // correctly.
            item.skipReason = "no custom shader — its marks were derived correctly";
        }

        if (!item.skipReason.empty())
        {
            ++report.skipped;
            report.items.push_back(std::move(item));
            continue;
        }

        item.parentPath = info.parentPath;

        MaterialComponent probe{};
        if (!CollectShaderDefaults(lib, renderer, mat, probe))
        {
            item.skipReason = renderer
                ? ("shader '" + mat.customShaderPath + "' did not compile or reflect")
                // Read-only scan (no Renderer): registration goes only through
                // Renderer::RegisterMaterialShader, so an unregistered shader is left alone.
                : ("shader '" + mat.customShaderPath + "' is not registered, and this "
                   "scan has no Renderer to register it through the parity fan-out");
            ++report.skipped;
            report.items.push_back(std::move(item));
            continue;
        }

        const std::unordered_set<std::string> ancestorKeys =
            AncestorAuthoredKeys(info.parentPath);

        // A mark is droppable when all three hold: it names a custom-shader parameter, no
        // ancestor authored it, and its value is exactly the shader's declared default. If any
        // fails, the artist could have meant it, and it stays.
        std::unordered_set<std::string> keep = info.overrideKeys;
        for (const std::string& key : info.overrideKeys)
        {
            const bool isParam = key.rfind("cparam_", 0) == 0;
            const bool isTex   = key.rfind("ctex_", 0) == 0;
            if (!isParam && !isTex)          continue;
            if (ancestorKeys.count(key))     continue;

            const std::string name = key.substr(isParam ? 7 : 5);
            bool droppable = false;

            if (isParam)
            {
                const auto meta = probe.customParamMeta.find(name);
                const auto val  = mat.customParams.find(name);
                droppable = meta != probe.customParamMeta.end() &&
                            val  != mat.customParams.end() &&
                            ArrEq(val->second, meta->second.defaultValue);
            }
            else
            {
                // A reflected texture slot seeds as an EMPTY TextureMap, so an
                // empty one is the untouched seed. A bound path is a real choice.
                const auto val = mat.customTextures.find(name);
                droppable = probe.customTextures.count(name) &&
                            val != mat.customTextures.end() && val->second.name.empty();
            }

            if (droppable) { item.droppable.push_back(key); keep.erase(key); }
        }
        std::sort(item.droppable.begin(), item.droppable.end());

        if (!item.droppable.empty())
        {
            ++report.flagged;
            if (apply)
            {
                BackupFile(it->path());
                std::vector<std::string> unset(info.unsetKeys.begin(), info.unsetKeys.end());
                if (Resource::SaveMaterialInstance(item.path, info.parentPath, mat,
                                                   &keep, &unset))
                {
                    item.rewritten = true;
                    ++report.rewritten;
                }
                else
                    LOG_ERROR("MaterialInstanceRepair: rewrite failed for '%s'",
                              item.path.c_str());
            }
        }

        report.items.push_back(std::move(item));
    }

    LOG_INFO("MaterialInstanceRepair: scanned %d, flagged %d, skipped %d, rewritten %d (%s)",
             report.scanned, report.flagged, report.skipped, report.rewritten,
             apply ? "APPLIED" : "dry run");
    return report;
}

} // namespace Editor
