// MaterialPropertyPanel.cpp - see the header. File-local helpers:
// RepublishMaterialProvenance, DrawShadingModelSupport, InspectorEngineTex /
// InspectorCustomTex.
#include "Editor/MaterialPropertyPanel.h"

#include "Editor/ColorWidgets.h"         // EditorUI::ColorEdit4
#include "Editor/EditorLayerShared.h"    // SaveFileDialog / OpenFileDialog
#include "Editor/ImGuiWidgets.h"         // EditorUI::DragFloat
#include "Editor/MaterialWidgets.h"      // DrawTextureSlotWidget / DrawCustomParamWidget
#include "ECS/HierarchyComponents.h"     // MaterialSourcePath, MaterialInstanceInfo, ApplyMaterialLoadInfo
#include "ECS/MaterialReflectionSync.h"
#include "ECS/MaterialSchema.h"
#include "ECS/TrailComponent.h"         // a trail look has no runtime PS id
#include "Graphics/MaterialRuntime.h"    // MaterialRuntimeTextureView
#include "Graphics/MaterialShaderDecl.h" // what a custom shader declares
#include "Graphics/ShaderLibrary.h"
#include "Graphics/ShaderReflection.h"
#include "Graphics/ShadingModel.h"
#include "Graphics/ShadingModelTable.h"   // the Inspector's row family per model
#include "Renderer/Renderer.h"
#include "Resource/MaterialOverrideApply.h"   // BuildMaterialOverride
#include "Resource/MaterialSerializer.h"
#include "System/CommandLine.h"     // -mateditor_autooverride_off
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Editor
{
// Re-reads a material file just written (or just pointed at) and pushes its
// provenance onto the entity. Every inspector button that changes which file
// backs an entity must call this, or MaterialInstanceInfo keeps describing the
// previous file: the "> instance of:" row, the override ticks and Break Link
// read the wrong parent, and a later re-save has no authored key set and falls
// back to guessing one (see ApplyMaterialLoadInfo).
// A re-read rather than the in-memory state: SaveMaterial /
// SaveMaterialInstance decide the authored key set, and the file is the only
// record of that decision. One parse per button press, usually served from the
// resolved-material cache.
static void RepublishMaterialProvenance(World& world, Entity e, const std::string& refPath)
{
    if (e == NullEntity || refPath.empty()) return;
    MaterialComponent        reread{};
    Resource::MaterialLoadInfo info;
    if (Resource::LoadMaterial(refPath, reread, &info))
        ApplyMaterialLoadInfo(world, e, info);
}

// Under the Shading Model menu: which render paths draw the model exactly,
// approximate it, or do not draw it at all - the shading-model table's path
// columns. Hovering lists every path with what it draws instead
// (ShadingModels::kPathNotes).
static void DrawShadingModelSupport(ShadingModel model)
{
    using namespace ShadingModels;
    const Desc& d = Of(model);
    std::string approx, none;
    for (size_t p = 0; p < static_cast<size_t>(Path::Count); ++p)
    {
        std::string* list = d.paths[p] == SMT_PATH_APPROX ? &approx
                          : d.paths[p] == SMT_PATH_NONE   ? &none : nullptr;
        if (!list) continue;
        if (!list->empty()) *list += ", ";
        *list += kPathNames[p];
    }
    ImGui::BeginGroup();
    if (approx.empty() && none.empty())
        ImGui::TextDisabled("Every render path draws this model exactly.");
    // The hover hint goes on whichever line is last.
    if (!approx.empty()) ImGui::TextDisabled(none.empty() ? "Approximated by: %s (hover)" : "Approximated by: %s",
                                             approx.c_str());
    if (!none.empty())   ImGui::TextDisabled("Not drawn by: %s (hover)", none.c_str());
    ImGui::EndGroup();
    if (!ImGui::IsItemHovered()) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
    ImGui::TextUnformatted("How each render path draws this shading model:");
    for (size_t p = 0; p < static_cast<size_t>(Path::Count); ++p)
    {
        const char* how  = d.paths[p] == SMT_PATH_FULL ? "exact"
                         : d.paths[p] == SMT_PATH_APPROX ? "approximated" : "not drawn";
        const char* note = PathNoteFor(model, static_cast<Path>(p));
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::TextWrapped("%s: %s%s%s", kPathNames[p], how, note ? " - " : "", note ? note : "");
    }
    ImGui::TextDisabled("Forward is the translucent route; RT hits are what ray-traced reflections "
                        "and GI bounces see.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// ==== Texture-slot thumbnails ====
// Entity mode reads the entity's runtime textures and is preferred: it carries
// the per-entity override layer (RuntimeMaterialEntityBinding::engineOverrides)
// that the by-key resource cannot know. Asset mode has no entity, so it
// resolves by the material's content key; without that every .imat slot would
// draw the grey "loading" box.
// @p assetKey is computed once per frame by the caller: these run inside the
// per-slot loops, and BuildPreviewMaterialRuntimeKey hashes the shader paths,
// every engine texture path and the sorted custom bindings. 0 means no by-key
// lookup (entity mode, or no renderer).
static MaterialRuntimeTextureView InspectorEngineTex(
    Renderer* renderer, Entity selEnt, uint64_t assetKey, uint32_t slot)
{
    if (!renderer) return {};
    if (selEnt != NullEntity) return renderer->GetMaterialTextureRuntime(selEnt, slot);
    const MaterialRuntimeTextureView view =
        renderer->GetPreviewEngineTextureRuntime(assetKey, slot);
    // Logged once per material-runtime epoch, and load-bearing: preview packs
    // assert on these lines as the observed proof that asset-mode thumbnails
    // resolve real textures by key. The condition is the one DrawTextureSlotWidget
    // uses to choose between an image and the grey "loading" box.
    // The epoch enables the second line, which shows by-key resolution still works
    // after a world clear wiped the runtime-material table. It is a distinct
    // message because a pack's logMustContain is a substring test and cannot count
    // occurrences.
    static uint32_t s_loggedEpoch = ~0u;
    const uint32_t epoch = renderer->MaterialRuntimeEpoch();
    if (s_loggedEpoch != epoch && (view.previewGpuHandle || view.gpuHandle))
    {
        const bool first = (s_loggedEpoch == ~0u);
        s_loggedEpoch = epoch;
        if (first)
            LOG_INFO("[PREVIEW] asset-mode texture slot %u resolved BY KEY "
                     "(bindless %d) - the entity-keyed wall is down", slot,
                     view.bindlessIndex);
        else
            LOG_INFO("[PREVIEW] asset-mode texture slot %u resolved BY KEY again "
                     "after a world clear (bindless %d, matRuntimeEpoch %u)",
                     slot, view.bindlessIndex, epoch);
    }
    return view;
}

static MaterialRuntimeTextureView InspectorCustomTex(
    Renderer* renderer, Entity selEnt, uint64_t assetKey, std::string_view name)
{
    if (!renderer) return {};
    if (selEnt != NullEntity) return renderer->GetMaterialCustomTextureRuntime(selEnt, name);
    return renderer->GetPreviewCustomTextureRuntime(assetKey, name);
}

void MaterialPropertyPanel::Draw(MaterialComponent& mat, const MaterialPanelContext& ctx)
{
    const bool assetMode = ctx.assetMode;
    // Texture lifetime managed by Renderer::SyncMaterialTextures (no editor-side acquire/release).

    // ---- Material source reference + dirty status ----
    static const char* kImatFilter     = "Material (*.imat)\0*.imat\0All Files\0*.*\0\0";
    static const char* kImatInstFilter = "Material Instance (*.imatinst)\0*.imatinst\0All Files\0*.*\0\0";
    static const char* kImatLoadFilter = "Material (*.imat;*.imatinst)\0*.imat;*.imatinst\0All Files\0*.*\0\0";

    const Entity selEnt = ctx.entity;
    MaterialSourcePath* matSrcPath = (selEnt != NullEntity && ctx.world)
        ? ctx.world->GetComponent<MaterialSourcePath>(selEnt) : nullptr;

    // One by-key material hash for the whole frame (see InspectorEngineTex): it
    // hashes the shader paths, every engine texture path and the sorted custom
    // bindings, and is identical for every slot. 0 in entity mode, where the
    // entity-keyed path is preferred.
    const uint64_t assetMatKey = (ctx.renderer && selEnt == NullEntity)
        ? ctx.renderer->BuildPreviewMaterialRuntimeKey(mat) : 0ull;

    // ---- Per-parameter override marks (instances only) ----
    // Resolved once for the four sections below: the schema rows, the engine
    // texture slots, the custom //@ params and the custom texture slots. nullptr
    // means "not an instance", and every section draws without ticks.
    // Ticking pins the value as it is now - that is what an override means, so it
    // cannot be a value diff. Unticking writes the parent's value back into the
    // live material so the viewport shows the inherited look at once.
    std::unordered_set<std::string>* ovrSet    = nullptr;
    std::vector<std::string>*        ovrUnset  = nullptr;
    std::string                      ovrParent;
    if (assetMode)
    {
        if (!ctx.assetInfo->parentPath.empty())
        {
            ovrSet    = &ctx.assetInfo->overrideKeys;
            ovrUnset  = &ctx.assetInfo->unsetKeys;
            ovrParent = ctx.assetInfo->parentPath;
        }
    }
    else if (selEnt != NullEntity && ctx.world)
    {
        if (auto* mi = ctx.world->GetComponent<MaterialInstanceInfo>(selEnt);
            mi && !mi->parentPath.empty())
        {
            ovrSet    = &mi->overrides;
            ovrUnset  = &mi->unsetKeys;
            ovrParent = mi->parentPath;
        }
    }

    // Resolved parent snapshot, cached by path. Loading it per frame would mean an
    // AssetFS read plus a full chain resolve every frame the Inspector is open.
    if (ovrSet && (m_parentSnapPath != ovrParent || m_parentSnapGen != Resource::MaterialContentGeneration()))
    {
        m_parentSnapPath = ovrParent;
        m_parentSnapGen  = Resource::MaterialContentGeneration();
        m_parentSnap     = MaterialComponent{};
        Resource::LoadMaterial(ovrParent, m_parentSnap);
    }

    // One override checkbox. Returns true when the mark changed, so callers can
    // mark dirty; `restore` runs on untick and copies the inherited value back.
    auto overrideMark = [&](const char* key, const std::function<void()>& restore) -> bool
    {
        if (!ovrSet) return false;
        bool tick = ovrSet->count(key) != 0;
        ImGui::PushID(key);
        const bool changed = ImGui::Checkbox("##ov", &tick);
        if (changed)
        {
            if (tick) ovrSet->insert(key);
            else { ovrSet->erase(key); if (restore) restore(); }
            mat.SetEditDirty();
            if (assetMode) *ctx.assetDirty = true;
        }
        ImGui::PopID();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(tick
                ? "Overridden by this instance. Untick to follow the parent again."
                : "Inherited from the parent, and follows it when the parent changes.");
        ImGui::SameLine();
        return changed;
    };
    auto isOverridden = [&](const char* key) { return !ovrSet || ovrSet->count(key) != 0; };

    // ---- An edit is an override ----
    // An instance file writes only the keys in its override set (MaterialSerializer
    // want() in explicit mode), so in asset mode an edit of a control with no
    // override tick - Blend Mode, Shadow Cull, Domain, the decal fields, every
    // flag, the outline, UV tiling / offset, the custom-shader block, "Common",
    // the water slots - adds its key (UE's rule: changing a value in an instance
    // overrides it). In entity mode the edit belongs to the entity (its
    // MaterialOverride on scene save), not to the file.
    // -mateditor_autooverride_off turns this off, so a pack can show that such an
    // edit is otherwise dropped on save.
    static const bool s_autoOverrideOff = Sys::CommandLine::Get().Has("-mateditor_autooverride_off");
    auto editKey = [&](const char* key)
    {
        if (ctx.onEdit) ctx.onEdit(key);   // the Material Editor's session decides
        else if (assetMode && ovrSet && !s_autoOverrideOff) ovrSet->insert(key);
        mat.SetEditDirty();
        if (assetMode) *ctx.assetDirty = true;
    };

    // Picking a model in either Shading Model menu. Some models are inert without
    // another setting, so the selection also sets it - the table's `onSelect`
    // column (Graphics/ShadingModelTable.h); each stays editable:
    //   double-sided  Foliage's lit far side, Water's view from below;
    //   translucent   an Opaque water surface is just a lid;
    //   no shadow     Water has no shadow-pass PSO: a solid slab under the lake.
    // Every field it changes goes through editKey, so an instance records the
    // override and the asset is marked dirty.
    auto selectShadingModel = [&](ShadingModel model)
    {
        mat.customShadingModel = model;
        // Keep the shaderType tombstone in step, so a resaved .imat cannot carry the
        // two axes in disagreement and an older reader picks the same lighting route.
        mat.SyncLegacyShaderType();
        const uint32_t also = ShadingModels::Of(model).onSelect;
        if ((also & SMT_SELECT_TRANSLUCENT) &&
            (mat.userBlendMode == BlendMode::Opaque || mat.userBlendMode == BlendMode::Masked ||
             mat.userBlendMode == BlendMode::Dithered))
        {
            mat.userBlendMode = BlendMode::Alpha;
            editKey("blendMode");
        }
        if (also & SMT_SELECT_DOUBLE_SIDED) { mat.SetDoubleSided(true);  editKey("doubleSided"); }
        if (also & SMT_SELECT_NO_SHADOW)    { mat.SetCastShadow(false);  editKey("castShadow"); }
        editKey("customShadingModel");
    };

    // Everything from here to the Separator is entity-oriented: the source path,
    // the Save / Save as New / Override / Instance buttons and the per-entity
    // override count. Asset mode has no entity, MaterialSourcePath or
    // MaterialOverride, and RenderMaterialAssetInspector draws its own header.
    if (!assetMode)
    {
        if (matSrcPath && !matSrcPath->path.empty())
        {
            ImGui::TextDisabled("Source: %s", matSrcPath->path.c_str());
            if (mat.IsAssetDirty())
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "(unsaved)");
            }
            // If the source is itself an instance, show what it inherits from.
            // Read from MaterialInstanceInfo rather than re-parsing the file every
            // frame the Inspector is open.
            const MaterialInstanceInfo* instInfo = (selEnt != NullEntity && ctx.world)
                ? ctx.world->GetComponent<MaterialInstanceInfo>(selEnt) : nullptr;
            if (instInfo && !instInfo->parentPath.empty())
            {
                if (!instInfo->parentResolved)
                    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f),
                                       "  ! parent unresolved: %s — values below are defaults",
                                       instInfo->parentPath.c_str());
                else
                    ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f),
                                       "  > instance of: %s  (%zu overridden)",
                                       instInfo->parentPath.c_str(), instInfo->overrides.size());

                // The deliberate flatten that a plain SaveMaterial refuses: named, one click,
                // and its tooltip says what it costs.
                ImGui::SameLine();
                if (ImGui::SmallButton("Break Link"))
                {
                    if (Resource::SaveMaterial(mat, matSrcPath->path,
                                               /*allowFlattenInstance=*/true))
                    {
                        ctx.world->RemoveComponent<MaterialInstanceInfo>(selEnt);
                        mat.SetAssetDirty(false);
                        LOG_SUCCESS("EditorLayer: '%s' flattened into a standalone "
                                    "material — it no longer follows a parent",
                                    matSrcPath->path.c_str());
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Rewrite this file as a STANDALONE material holding its current\n"
                        "resolved values. It stops following '%s' permanently.\n"
                        "There is no undo for this.", instInfo->parentPath.c_str());
            }
        }
        else
            ImGui::TextDisabled("Source: (none — inline material)");

        // ---- Save (writes back to the source file, or prompts for a new one) ----
        // Routes by what the target already is. Saving an instance through
        // SaveMaterial would write a standalone dump with no `parent` line and sever
        // the link, so SaveMaterial refuses it and an instance target goes through
        // SaveMaterialInstance. Deliberate flattening is the separate Break Link.
        if (ImGui::Button("Save"))
        {
            std::string savePath;
            if (matSrcPath && !matSrcPath->path.empty())
                savePath = matSrcPath->path; // save back to source
            else
                savePath = SaveFileDialog(kImatFilter, "imat", "asset/");

            if (!savePath.empty())
            {
                const std::string tgtParent = Resource::GetMaterialParent(savePath);
                const bool saved = tgtParent.empty()
                    ? Resource::SaveMaterial(mat, savePath)
                    : Resource::SaveMaterialInstance(savePath, tgtParent, mat);
                if (saved)
                {
                    mat.SetAssetDirty(false);
                    // Update source path if saving to a new file. MaterialSourcePath is
                    // serialized as MaterialRef, so it must be project-root-relative;
                    // the dialog hands back an absolute path.
                    const std::string refPath = Sys::RelativizeAssetPath(savePath);
                    if (selEnt != NullEntity && ctx.world)
                    {
                        if (matSrcPath) matSrcPath->path = refPath;
                        else ctx.world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ refPath });
                        ctx.world->RemoveComponent<MaterialOverride>(selEnt); // deltas now live in the file
                        // Re-publish provenance: Save may have gone through SaveMaterialInstance
                        // (the target had a `parent`), which changes the authored key set, and a Save
                        // to a different file changes the parent outright.
                        RepublishMaterialProvenance(*ctx.world, selEnt, refPath);
                    }
                }
                else
                    LOG_ERROR("EditorLayer: save failed for '%s' — see the log above; "
                              "the material is still unsaved.", savePath.c_str());
            }
        }

        // ---- Save as New (always prompts for a fresh path) ----
        // Writes a new .imat and re-points MaterialSourcePath: a variant that leaves
        // the original untouched.
        ImGui::SameLine();
        if (ImGui::Button("Save as New"))
        {
            const std::string savePath = SaveFileDialog(kImatFilter, "imat", "asset/");
            if (!savePath.empty())
            {
                // Not routed like Save: "as New" is a standalone variant by definition. The
                // dialog can still land on an existing .imatinst, which SaveMaterial refuses
                // to flatten - report that rather than re-point the entity at a file that was
                // not written.
                if (Resource::SaveMaterial(mat, savePath))
                {
                    mat.SetAssetDirty(false);
                    const std::string refPath = Sys::RelativizeAssetPath(savePath);
                    if (selEnt != NullEntity && ctx.world)
                    {
                        if (matSrcPath) matSrcPath->path = refPath;
                        else ctx.world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ refPath });
                        ctx.world->RemoveComponent<MaterialOverride>(selEnt);
                        // "as New" always writes a STANDALONE material, so the entity's
                        // source is now a master and any MaterialInstanceInfo left over
                        // from the previous source is a lie. RepublishMaterialProvenance
                        // removes it (ApplyMaterialLoadInfo drops the component when
                        // parentPath is empty).
                        RepublishMaterialProvenance(*ctx.world, selEnt, refPath);
                    }
                }
                else
                    LOG_ERROR("EditorLayer: 'Save as New' failed for '%s' — entity still "
                              "points at its previous material.", savePath.c_str());
            }
        }

        // ---- Save as Override (per-entity diff; the .imat is untouched) ----
        ImGui::SameLine();
        if (ImGui::Button("Save as Override"))
        {
            if (selEnt != NullEntity && ctx.world && matSrcPath && !matSrcPath->path.empty())
            {
                // The diff lives in Resource::BuildMaterialOverride so the scene save path
                // uses the same code (see MaterialOverrideApply.h). It logs and returns false
                // when the base cannot be loaded; do not read that as "no differences".
                MaterialOverride ovr;
                if (Resource::BuildMaterialOverride(mat, matSrcPath->path, ovr))
                {
                    if (!ovr.IsEmpty())
                    {
                        ctx.world->AddComponent<MaterialOverride>(selEnt, std::move(ovr));
                        mat.SetAssetDirty(false);
                        LOG_INFO("EditorLayer: saved %zu material override(s) on entity %u",
                                 ctx.world->GetComponent<MaterialOverride>(selEnt)->props.size(), selEnt);
                    }
                    else
                    {
                        // No differences: drop any override the entity still carries. A stale one
                        // would pin the old value against future master edits.
                        if (ctx.world->GetComponent<MaterialOverride>(selEnt))
                            ctx.world->RemoveComponent<MaterialOverride>(selEnt);
                        mat.SetAssetDirty(false);
                        LOG_INFO("EditorLayer: no differences from base — no override needed");
                    }
                }
            }
            else
                LOG_WARNING("EditorLayer: no MaterialSourcePath — save as .imat first");
        }

        // ---- Save as Instance (.imatinst inheriting from the current source) ----
        // Creates a shareable child material that overrides only what this entity
        // changed, then re-points the entity at it. The asset browser's "Create
        // Material Instance" makes a fresh, empty instance; this button turns an
        // entity's tweaks into an asset.
        // When the current source is itself an instance, the new one parents to that
        // source's parent, so a second press does not build a grandchild.
        ImGui::SameLine();
        if (ImGui::Button("Save as Instance"))
        {
            if (selEnt != NullEntity && ctx.world && matSrcPath && !matSrcPath->path.empty())
            {
                const std::string instPath = SaveFileDialog(kImatInstFilter, "imatinst", "asset/");
                if (!instPath.empty())
                {
                    const std::string curParent = Resource::GetMaterialParent(matSrcPath->path);
                    const std::string useParent = curParent.empty() ? matSrcPath->path : curParent;

                    // Explicit set from what the source already declared, so a
                    // re-save keeps the author's marks instead of re-deriving them
                    // from whichever values happen to differ today.
                    const MaterialInstanceInfo* mi =
                        ctx.world->GetComponent<MaterialInstanceInfo>(selEnt);
                    const std::unordered_set<std::string>* keys = mi ? &mi->overrides : nullptr;
                    const std::vector<std::string>*        uns  = mi ? &mi->unsetKeys : nullptr;

                    if (Resource::SaveMaterialInstance(instPath, useParent, mat, keys, uns))
                    {
                        const std::string instRef = Sys::RelativizeAssetPath(instPath);
                        matSrcPath->path = instRef;                        // entity -> instance asset
                        ctx.world->RemoveComponent<MaterialOverride>(selEnt); // deltas now live in the instance
                        // Re-read provenance from what was just written, or the
                        // entity keeps claiming its old parent and override set.
                        MaterialComponent reread{};
                        Resource::MaterialLoadInfo info;
                        if (Resource::LoadMaterial(instRef, reread, &info))
                            ApplyMaterialLoadInfo(*ctx.world, selEnt, info);
                        mat.SetAssetDirty(false);
                        LOG_INFO("EditorLayer: saved material instance '%s' (parent '%s')",
                                 instPath.c_str(), useParent.c_str());
                    }
                }
            }
            else
                LOG_WARNING("EditorLayer: Save as Instance needs a source material — save a base .imat first");
        }

        // ---- Load .imat (replaces runtime data + updates ref) ----
        ImGui::SameLine();
        if (ImGui::Button("Load .imat"))
        {
            const std::string path = OpenFileDialog(kImatLoadFilter, "asset/");
            if (!path.empty())
            {
                // Load through the project-relative form, not the dialog's absolute
                // path: that is the string that ends up in MaterialSourcePath, and
                // loading through a different spelling defeats the resolved-material
                // cache and can pick a different AssetFS entry in a packed build.
                const std::string refPath = Sys::RelativizeAssetPath(path);
                Resource::MaterialLoadInfo info;
                if (Resource::LoadMaterial(refPath, mat, &info))
                {
                    mat.SetAssetDirty(false);
                    if (selEnt != NullEntity && ctx.world)
                    {
                        if (matSrcPath) matSrcPath->path = refPath;
                        else ctx.world->AddComponent<MaterialSourcePath>(selEnt, MaterialSourcePath{ refPath });
                        ctx.world->RemoveComponent<MaterialOverride>(selEnt);
                        // The filter accepts .imatinst, so this button can hand the
                        // entity an INSTANCE. Without publishing the load info the
                        // "instance of:" row, the override ticks and Break Link all
                        // keep describing the PREVIOUS material's parent.
                        ApplyMaterialLoadInfo(*ctx.world, selEnt, info);
                    }
                }
                else
                    LOG_ERROR("EditorLayer: 'Load .imat' failed for '%s' — entity unchanged",
                              refPath.c_str());
            }
        }

        if (selEnt != NullEntity && ctx.world)
        {
            const MaterialOverride* ovr = ctx.world->GetComponent<MaterialOverride>(selEnt);
            if (ovr && !ovr->IsEmpty())
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.f, 1.f), "Override: %zu prop(s)", ovr->props.size());
        }
    }

    ImGui::Separator();

    if (!mat.useCustomShader)
    {
        // ---- Shading Model (engine materials) ----
        // The deferred lighting model and the only lighting axis (nothing routes on
        // MaterialComponent::shaderType; see Graphics/ShadingStencil.h): how the
        // deferred pass interprets the GBuffer this material writes, Unreal's Shading
        // Model. Every entry works with the engine's own pixel shader.
        // Water is absent on purpose: it needs the custom-shader path plus a
        // translucent blend, so the custom block's combo offers it. Which models this
        // menu offers, their order and names come from the shading-model table
        // (`menus`, kMenuOrder, `display`).
        {
            ShadingModel models[ShadingModels::kRowCount];
            const char*  labels[ShadingModels::kRowCount];
            const int    modelCount = ShadingModels::MenuEntries(SMT_MENU_ENGINE, models, labels);

            // A model this combo does not list (Water) is shown read-only rather than as
            // "Standard", which would misreport the material and invite a click that
            // silently retypes it.
            int  modelIdx = 0;
            bool listed   = false;
            for (int m = 0; m < modelCount; ++m)
                if (mat.customShadingModel == models[m]) { modelIdx = m; listed = true; }

            // Instance-override checkbox. Without it the control would be editable on an
            // .imatinst and the edit dropped on save: for an instance MaterialSerializer's
            // want() writes only keys in the override set.
            overrideMark("customShadingModel", [&]{
                mat.customShadingModel = m_parentSnap.customShadingModel;
            });
            const bool modelOverridden = !ovrSet || ovrSet->count("customShadingModel") != 0;
            if (!modelOverridden) ImGui::BeginDisabled();

            if (!listed)
            {
                ImGui::TextDisabled("Shading Model: %s",
                                    ShadingModelName(mat.customShadingModel));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Water is the only model this combo does not offer: it also\n"
                        "needs a translucent blend and the custom-shader path.\n"
                        "Tick Use Custom Shader to reach it.");
            }
            else if (ImGui::Combo("Shading Model", &modelIdx, labels, modelCount))
                selectShadingModel(models[modelIdx]);

            if (!modelOverridden) ImGui::EndDisabled();
            DrawShadingModelSupport(mat.customShadingModel);

        }
    }

    // ---- Custom material shader ----
    // Replaces the engine GBuffer pixel shader; the Shading Model still decides
    // how the deferred pass lights it.
    if (ImGui::Checkbox("Use Custom Shader", &mat.useCustomShader))
    {
        editKey("useCustomShader");
    }
    if (mat.useCustomShader)
    {
        // How the custom-shader id readouts below are resolved. Entity mode: the
        // selection. Asset mode: never the selection - this panel edits a
        // MaterialComponent that belongs to no entity, and the selected entity's
        // reflection would filter this material's //@ rows out (the hasRefl /
        // typeByName gate below) and aim "Prune Unused Params" at the wrong shader.
        // It asks for this material's ids through the read-only
        // Renderer::LookupMaterialCustom*ID pair, keyed by (useCustomShader,
        // customShaderPath, customVertexShaderPath), never by an entity.
        // Both lambdas keep the -1 / 0 / >0 contract the readouts below switch on, and
        // neither registers a shader: registering from an ImGui callback would race a
        // compiling worker and bypass the nine-library parity fan-out (see
        // LookupMaterialCustomPSID).
        auto lookupPSID = [&]() -> int {
            if (!ctx.renderer) return -1;
            if (assetMode)   return ctx.renderer->LookupMaterialCustomPSID(mat);
            return (selEnt != NullEntity)
                ? ctx.renderer->GetMaterialRuntimeCustomPSID(selEnt) : -1;
        };
        auto lookupVSID = [&]() -> int {
            if (!ctx.renderer) return -1;
            if (assetMode)   return ctx.renderer->LookupMaterialCustomVSID(mat);
            return (selEnt != NullEntity)
                ? ctx.renderer->GetMaterialRuntimeCustomVSID(selEnt) : -1;
        };

        // ---- One unified material shader file ----
        // One source compiled for both stages: always as the pixel (surface) shader,
        // and also as a vertex shader when it declares World-Position-Offset (#define
        // MATERIAL_HAS_VERTEX_OFFSET). Authored against shaders/custom_material.hlsli:
        // define EvaluateMaterial (pixel) and/or MaterialGetWorldPositionOffset
        // (vertex). Pixel-only materials share the engine's generic vertex shader (no
        // per-material VS is compiled).
        char pathBuf[256] = {};
        const size_t copy = (std::min)(mat.customShaderPath.size(), sizeof(pathBuf) - 1);
        std::memcpy(pathBuf, mat.customShaderPath.data(), copy);
        pathBuf[copy] = '\0';
        if (ImGui::InputText("Material Shader", pathBuf, sizeof(pathBuf)))
        {
            mat.customShaderPath     = pathBuf;
            editKey("customShaderPath");
        }
        // Drag-drop accepts IHLSL_PATH from Resource panel `.hlsl` rows.
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IHLSL_PATH"))
            {
                mat.customShaderPath     = static_cast<const char*>(p->Data);
                // Drop old schema; new shader's reflection repopulates next tick.
                mat.customTextures.clear();
                mat.customParams.clear();
                editKey("customShaderPath");
            }
            ImGui::EndDragDropTarget();
        }

        // ---- Separate vertex-shader file (two-file materials) ----
        // Shown only for materials that already have one; new materials use the single
        // Material Shader above. Clearing it switches the material to the unified
        // file's WPO hook.
        if (!mat.customVertexShaderPath.empty())
        {
            char vsBuf[256] = {};
            const size_t vcopy = (std::min)(mat.customVertexShaderPath.size(), sizeof(vsBuf) - 1);
            std::memcpy(vsBuf, mat.customVertexShaderPath.data(), vcopy);
            vsBuf[vcopy] = '\0';
            if (ImGui::InputText("Vertex Shader (legacy)", vsBuf, sizeof(vsBuf)))
            {
                mat.customVertexShaderPath = vsBuf;
                editKey("customVertexShaderPath");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear##legacyvs"))
            {
                mat.customVertexShaderPath.clear();
                editKey("customVertexShaderPath");
            }
            const int runtimeVSID = lookupVSID();
            if (ctx.renderer && runtimeVSID > 0)
            {
                const std::string verr = ctx.renderer->GetCustomShaderError(runtimeVSID);
                if (!verr.empty())
                {
                    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "  VS Compile Error:");
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.55f, 1.f));
                    ImGui::TextWrapped("%s", verr.c_str());
                    ImGui::PopStyleColor();
                }
            }
        }

        // ---- "Shading Model": the single lighting control ----
        // It writes customShadingModel and nothing else; the lighting route (the
        // GBuffer stencil) is derived from it by ShadingStencil::RefFor.
        // Every model is offered here, Water (Unreal's Single Layer Water: a
        // translucent Surface material redirected to WaterSurfacePass) included; what
        // picking one also sets is selectShadingModel's (the table's `onSelect`). A
        // value outside the menu shows as the first entry.
        {
            ShadingModel models[ShadingModels::kRowCount];
            const char*  labels[ShadingModels::kRowCount];
            const int    modelCount = ShadingModels::MenuEntries(SMT_MENU_CUSTOM, models, labels);
            int litIdx = 0;
            for (int m = 0; m < modelCount; ++m)
                if (mat.customShadingModel == models[m]) litIdx = m;
            if (ImGui::Combo("Shading Model", &litIdx, labels, modelCount))
                selectShadingModel(models[litIdx]);
            DrawShadingModelSupport(mat.customShadingModel);
        }

        const int runtimePSID = lookupPSID();
        const int runtimeVSID = lookupVSID();
        // A trail's PS lives in TrailRenderPass's own pass-local ShaderLibrary and is
        // resolved by TrailSystem::CollectTrails, so its material never enters
        // Renderer::m_runtimeMaterialShaders (only ResolveCustomPSID/VSID write it,
        // from the mesh / beam / afterimage draw gathers). For a trail entity
        // runtimePSID is always -1 and the warning below would be a permanent false
        // alarm; suppress it and point at the real diagnostic, the Trail component's
        // "Slot:" readout.
        const bool trailLook = ctx.world && selEnt != NullEntity &&
                               ctx.world->HasComponent<TrailComponent>(selEnt);
        if (runtimePSID < 0 && assetMode)
            // Asset mode has no entity, so nothing registers or compiles this shader on
            // the next frame and the entity-mode wording would be a permanent false alarm.
            // -1 here means no material has resolved this shader key in this process (the
            // id map survives world reloads; OnWorldClear does not clear it), so the rows
            // below come straight from the file.
            ImGui::TextDisabled(
                "  (no live reflection - no material has resolved this shader yet;\n"
                "   params/textures below are shown exactly as the file stores them)");
        else if (runtimePSID < 0 && !trailLook)
            ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f),
                "  (shader not yet resolved — will compile on next frame)");
        else if (trailLook && mat.customParams.empty() && mat.customTextures.empty())
            // Neutral, not an alarm: a trail shader may legitimately declare no
            // b8 params. Says where the rows come from instead of diagnosing.
            ImGui::TextDisabled(
                "  (trail look — params/textures are reflected by TrailSystem;\n"
                "   none declared by this shader, or the path does not resolve)");

        // Surface DXC compile error (cached by ShaderLibrary; cleared on .hlsl edit via hot-reload).
        if (ctx.renderer && runtimePSID > 0)
        {
            const std::string err = ctx.renderer->GetCustomShaderError(runtimePSID);
            if (!err.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f),
                                   "  Compile Error:");
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.55f, 0.55f, 1.f));
                ImGui::TextWrapped("%s", err.c_str());
                ImGui::PopStyleColor();
            }
        }

        // What the shader's source declares - its domain (#define
        // MATERIAL_DOMAIN_WATER; recorded, not enforced) - and where the parameters
        // below come from: the declarations, or the compiled shader for a form the
        // parser cannot lay out. A domain that disagrees with the Shading Model (which
        // routes the draw) is pointed out.
        if (ctx.renderer && runtimePSID > 0)
            if (ShaderLibrary* declLib = ctx.renderer->GetMaterialShaderLibrary())
                if (const auto decl = declLib->GetDynamicDecl(static_cast<uint32_t>(runtimePSID)); decl && decl->read)
                {
                    const bool waterDomain = decl->domain == MaterialShaderDecl::Domain::Water;
                    if (decl->Exact())
                        ImGui::TextDisabled("  declares the %s domain; parameters read from the source",
                                            waterDomain ? "water" : "surface");
                    else
                        ImGui::TextDisabled("  declares the %s domain; parameters from the compiled shader\n"
                                            "  (the source has a form the parser cannot lay out: %s)",
                                            waterDomain ? "water" : "surface", decl->inexact.front().c_str());
                    if (waterDomain != mat.IsWaterShadingModel())
                        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.2f, 1.f), "%s",
                            waterDomain ? "  the shader declares MATERIAL_DOMAIN_WATER; the Shading Model is not Water"
                                        : "  the Shading Model is Water; the shader declares no MATERIAL_DOMAIN_WATER");
                }

        // ---- Reflection-driven custom textures + params ----
        // A unified material is compiled for both stages and DXC strips each stage's
        // reflection to the vars that stage uses: surface params live in the pixel
        // reflection, World-Position-Offset params in the vertex one. Gather both so
        // every b8 tunable gets a typed widget. Passing the material reflects the
        // variant it draws with, so the rows match what the renderer synced and binds.
        const ShaderReflect::Reflection* reflPS = (ctx.renderer && runtimePSID > 0)
            ? ctx.renderer->GetCustomShaderReflection(runtimePSID, &mat) : nullptr;
        const ShaderReflect::Reflection* reflVS = (ctx.renderer && runtimeVSID > 0)
            ? ctx.renderer->GetCustomShaderReflection(runtimeVSID, &mat) : nullptr;
        const bool hasRefl = (reflPS != nullptr) || (reflVS != nullptr);

        // The rows are the shader's declarations - every parameter it declares,
        // whether a variant reads it or not. Parameters no compiled variant reads are
        // marked "(not read)"; until a variant of every stage the material uses has
        // compiled, nothing is marked.
        ShaderLibrary::DynamicReads readsPS, readsVS;
        if (ctx.renderer)
        {
            if (runtimePSID > 0) readsPS = ctx.renderer->GetCustomShaderReads(runtimePSID);
            if (runtimeVSID > 0) readsVS = ctx.renderer->GetCustomShaderReads(runtimeVSID);
        }
        const bool readsKnown = (runtimePSID > 0 || runtimeVSID > 0) &&
                                (runtimePSID <= 0 || readsPS.known) && (runtimeVSID <= 0 || readsVS.known);
        auto paramNotRead = [&](const std::string& n) {
            return readsKnown && !readsPS.vars.count(n) && !readsVS.vars.count(n);
        };

        // An entity material gets its //@ bag filled by the renderer:
        // ResolveCustomPSID/VSID run MaterialReflectionSync::Sync whenever they resolve
        // it, so every declared parameter has a row even when the file stores none. The
        // asset buffer is off-World and nothing syncs it, and an .imatinst stores
        // overrides only, so run the same sync here.
        // Stamped like the renderer's (shader paths + ShaderLibrary reload epoch, per
        // stage) so it runs once, not per frame: an unstamped re-sync would re-add a
        // parameter the instant the "x" button below drops it. Editor-side and
        // material-local - it registers nothing.
        if (assetMode && hasRefl)
        {
            ShaderLibrary* matLib = ctx.renderer ? ctx.renderer->GetMaterialShaderLibrary() : nullptr;
            if (matLib)
            {
                const uint32_t reflEpoch = matLib->GetReloadEpoch();
                if (reflPS && !MaterialReflectionSync::StageSynced(mat, reflEpoch, /*vsStage*/ false))
                {
                    MaterialReflectionSync::Sync(mat, *reflPS);
                    MaterialReflectionSync::MarkStageSynced(mat, reflEpoch, /*vsStage*/ false);
                }
                if (reflVS && !MaterialReflectionSync::StageSynced(mat, reflEpoch, /*vsStage*/ true))
                {
                    MaterialReflectionSync::Sync(mat, *reflVS);
                    MaterialReflectionSync::MarkStageSynced(mat, reflEpoch, /*vsStage*/ true);
                }
            }
        }

        if (!mat.customTextures.empty())
        {
            ImGui::Separator();
            ImGui::Text("Custom Textures");
            // Declaration-order + @group sections (Tex_ b8 slots carry meta;
            // space3 textures without meta keep map order at the end).
            std::vector<std::pair<const std::string*, MaterialComponent::TextureMap*>> texRows;
            texRows.reserve(mat.customTextures.size());
            for (auto& [name, tex] : mat.customTextures)
                texRows.emplace_back(&name, &tex);
            auto texMetaOf = [&](const std::string& n) -> const MaterialComponent::CustomParamMeta* {
                auto it = mat.customParamMeta.find(n);
                return it != mat.customParamMeta.end() ? &it->second : nullptr;
            };
            std::sort(texRows.begin(), texRows.end(), [&](const auto& a, const auto& b) {
                const auto* ma = texMetaOf(*a.first);
                const auto* mb = texMetaOf(*b.first);
                const uint16_t sa = ma ? ma->sortOrder : 0xFFFF;
                const uint16_t sb = mb ? mb->sortOrder : 0xFFFF;
                if (sa != sb) return sa < sb;
                return *a.first < *b.first;
            });
            static const std::string kNoGroup;
            const std::string* lastTexGroup = &kNoGroup;
            for (auto& [namePtr, texPtr] : texRows)
            {
                const auto* pm2 = texMetaOf(*namePtr);
                const std::string& g = pm2 ? pm2->group : kNoGroup;
                if (!g.empty() && g != *lastTexGroup)
                    ImGui::SeparatorText(g.c_str());
                lastTexGroup = &g;
                const MaterialRuntimeTextureView runtime =
                    InspectorCustomTex(ctx.renderer, selEnt, assetMatKey, *namePtr);
                const std::string ctexKey = "ctex_" + *namePtr;
                overrideMark(ctexKey.c_str(), [&]{
                    auto pit = m_parentSnap.customTextures.find(*namePtr);
                    *texPtr = (pit != m_parentSnap.customTextures.end())
                                ? pit->second : MaterialComponent::TextureMap{};
                });
                const bool tOverridden = isOverridden(ctexKey.c_str());
                if (!tOverridden) ImGui::BeginDisabled();
                if (DrawTextureSlotWidget(namePtr->c_str(), *texPtr, runtime))
                {
                    mat.SetEditDirty();
                    if (assetMode) *ctx.assetDirty = true;
                }
                if (!tOverridden) ImGui::EndDisabled();
            }
        }

        if (!mat.customParams.empty())
        {
            ImGui::Separator();
            ImGui::Text("Custom Parameters");

            // name -> reflected VarType for typed widgets; absent falls back to
            // DragFloat4. Built only from the custom-param CBV at b8 space0, the slot the
            // runtime packer uses: engine CBs (PerViewCB@b1, LightCB@b2, ...) pulled in by
            // shared headers are excluded by register, so their members never appear as
            // params. Doubles as a whitelist below.
            std::unordered_map<std::string, ShaderReflect::VarType> typeByName;
            for (const ShaderReflect::Reflection* r : { reflPS, reflVS })
            {
                if (!r) continue;
                for (const auto& cb : r->cbuffers)
                {
                    if (cb.bindPoint != 8u || cb.space != 0u) continue;
                    if (MaterialReflectionSync::IsReservedCBuffer(cb.name.c_str())) continue;
                    for (const auto& v : cb.vars)
                        if (v.used)   // unused-in-both-stages vars get no widget
                            typeByName.emplace(v.name, v.type);
                }
            }

            // Two-phase: collect visible rows, sort by declaration order (meta sortOrder
            // = cbuffer byte offset), then draw with @group section headers - an
            // unordered_map walk would scatter the groups.
            struct ParamRow
            {
                const std::string*                        name;
                std::array<float, 4>*                     val;
                const MaterialComponent::CustomParamMeta* meta;
                ShaderReflect::VarType                    type;
                int                                       numComponents;
            };
            std::vector<ParamRow> rows;
            rows.reserve(mat.customParams.size());

            for (auto& [name, val] : mat.customParams)
            {
                // Only show params in the current shader's b8 cbuffer (either stage): hides
                // stale entries a previous shader left in the bag (Sync never prunes). With no
                // reflection ready yet, show the bag as-is.
                if (hasRefl && typeByName.find(name) == typeByName.end()) continue;
                if (!name.empty() && name[0] == '_') continue;   // authoring pads
                // "Tex_" entries in the float bag are dead data (texture slots live in
                // customTextures; the packers override the bag value with the resolved index),
                // so hide them.
                if (name.rfind("Tex_", 0) == 0) continue;

                const MaterialComponent::CustomParamMeta* pmeta = nullptr;
                if (auto mit = mat.customParamMeta.find(name); mit != mat.customParamMeta.end())
                    pmeta = &mit->second;

                using V = ShaderReflect::VarType;
                auto it = typeByName.find(name);
                V   type          = (it != typeByName.end()) ? it->second : V::Unknown;
                int numComponents = 4;
                switch (type)
                {
                    case V::Float2: case V::Int2: case V::UInt2: numComponents = 2; break;
                    case V::Float3: case V::Int3: case V::UInt3: numComponents = 3; break;
                    case V::Float:  case V::Int:  case V::UInt:
                    case V::Bool:                                numComponents = 1; break;
                    case V::Unknown:
                        // No live reflection in the renderer libraries (the shader may belong to a
                        // pass-local library, e.g. trail/particle): size the widget from the synced
                        // meta. Recover `isBool` too, or a reflected `bool` degrades to a 0..1 slider
                        // whose in-between values mean nothing to the shader.
                        if (pmeta && pmeta->isBool)
                        {
                            numComponents = 1;
                            type = V::Bool;
                            break;
                        }
                        if (pmeta && pmeta->numComponents >= 1 && pmeta->numComponents <= 4)
                            numComponents = pmeta->numComponents;
                        type = V::Float4;
                        break;
                    default: numComponents = 4; break;
                }
                rows.push_back({ &name, &val, pmeta, type, numComponents });
            }

            std::sort(rows.begin(), rows.end(), [](const ParamRow& a, const ParamRow& b) {
                const uint16_t sa = a.meta ? a.meta->sortOrder : 0xFFFF;
                const uint16_t sb = b.meta ? b.meta->sortOrder : 0xFFFF;
                if (sa != sb) return sa < sb;
                return *a.name < *b.name;
            });

            static const std::string kNoParamGroup;
            const std::string* lastGroup = &kNoParamGroup;
            for (const ParamRow& row : rows)
            {
                const std::string& g = row.meta ? row.meta->group : kNoParamGroup;
                if (!g.empty() && g != *lastGroup)
                    ImGui::SeparatorText(g.c_str());
                lastGroup = &g;

                // Override mark. Restoring the inherited value prefers the
                // PARENT's authored value and falls back to the shader's
                // reflected default, which is the correct answer when the parent
                // does not mention the parameter either.
                const std::string ovrKey = "cparam_" + *row.name;
                overrideMark(ovrKey.c_str(), [&]{
                    auto pit = m_parentSnap.customParams.find(*row.name);
                    if (pit != m_parentSnap.customParams.end())      *row.val = pit->second;
                    else if (row.meta)                               *row.val = row.meta->defaultValue;
                });
                const bool pOverridden = isOverridden(ovrKey.c_str());
                if (!pOverridden) ImGui::BeginDisabled();

                CustomParamDrawDesc d;
                d.name          = row.name->c_str();
                d.numComponents = row.numComponents;
                d.type          = row.type;
                if (row.meta)
                {
                    d.defaultValue = row.meta->defaultValue.data();
                    d.minValue     = row.meta->minValue.data();
                    d.maxValue     = row.meta->maxValue.data();
                    d.hasRange     = row.meta->hasRange;
                    d.label        = row.meta->label.c_str();
                }
                if (DrawCustomParamWidget(d, row.val->data()))
                {
                    mat.SetEditDirty();
                    if (assetMode) *ctx.assetDirty = true;
                }

                if (!pOverridden) ImGui::EndDisabled();

                if (paramNotRead(*row.name))
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not read)");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("No compiled variant of this shader reads it. It is declared, so it is\n"
                                          "listed, saved and packed - changing it changes nothing on screen.");
                }

                // "Drop inherited" is the only way to remove a parameter the parent declares:
                // both emit loops in BuildTextImpl walk the child's map, so a key the child
                // lacks emits nothing and merges back in on load. `unset = <key>` is its only
                // spelling.
                if (ovrUnset && !pOverridden &&
                    m_parentSnap.customParams.count(*row.name))
                {
                    ImGui::SameLine();
                    ImGui::PushID(ovrKey.c_str());
                    if (ImGui::SmallButton("x"))
                    {
                        if (std::find(ovrUnset->begin(), ovrUnset->end(), ovrKey) == ovrUnset->end())
                            ovrUnset->push_back(ovrKey);
                        mat.customParams.erase(*row.name);
                        mat.SetEditDirty();
                        if (assetMode) *ctx.assetDirty = true;
                        ImGui::PopID();
                        break;   // `rows` points into the map we just erased from
                    }
                    ImGui::PopID();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Drop this inherited parameter entirely\n"
                                          "(writes `unset = %s`).", ovrKey.c_str());
                }
            }

            // Prune only when every stage the material declares has a live reflection:
            // with (say) the PS compile failed and only the VS reflected, the keep-set
            // would miss every pixel-stage param and pruning would erase the user's PS
            // tunables.
            const bool expectPS = mat.useCustomShader && !mat.customShaderPath.empty();
            const bool expectVS = !mat.customVertexShaderPath.empty();
            const bool canPrune = (!expectPS || reflPS) && (!expectVS || reflVS) && hasRefl;
            if (canPrune)
            {
                // Reflection is live -> safe to drop entries the current shader no longer
                // declares (Sync never prunes, so stale keys would serialize forever).
                ImGui::Spacing();
                if (ImGui::SmallButton("Prune Unused Params"))
                {
                    std::unordered_set<std::string> texNames;   // reflected space3 textures
                    for (const ShaderReflect::Reflection* r : { reflPS, reflVS })
                    {
                        if (!r) continue;
                        for (const auto& b : r->bindings)
                            if (b.type == ShaderReflect::ResourceType::Texture &&
                                b.space == 3u &&
                                !MaterialReflectionSync::IsReservedTexture(b.name.c_str()))
                                texNames.insert(b.name);
                    }
                    // Float bag: drop keys the shader no longer declares and every "Tex_" entry
                    // (texture slots live in customTextures; a Tex_ key in the bag is dead data).
                    std::erase_if(mat.customParams, [&](const auto& kv)
                        { return typeByName.find(kv.first) == typeByName.end() ||
                                 kv.first.rfind("Tex_", 0) == 0; });
                    std::erase_if(mat.customParamMeta, [&](const auto& kv)
                        { return typeByName.find(kv.first) == typeByName.end(); });
                    // Textures: keep reflected space3 names AND "Tex_" b8 slots.
                    std::erase_if(mat.customTextures, [&](const auto& kv)
                        { return texNames.find(kv.first) == texNames.end() &&
                                 typeByName.find(kv.first) == typeByName.end(); });
                    mat.SetEditDirty();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove saved params/textures the current shader no longer declares.");
            }
        }

        // ---- Engine params the generic custom shaders honor ----
        // The generic part shaders (Face/Hair/Eye/Base/Stockings/Trans) read these
        // MaterialGPUData fields, and the schema-driven "Material Parameters" block
        // below does not show all of them for every shading model (normalMapStrength's
        // row is SM_ANY_PBR|SM_ANY_NPR, so Unlit would have no widget). Draw them here.
        // Do not repeat "Exposure Protection" here: its schema row is SM_ANY, so the
        // block below draws it for every model including Unlit, with the schema's
        // 0..1 range and the .imatinst override tick this hand-rolled path lacks.
        ImGui::Separator();
        ImGui::TextDisabled("Common");
        if (EditorUI::ColorEdit4 ("Base Color##cs",      &mat.baseColor.x))                       editKey("baseColor");
        if (EditorUI::DragFloat("Normal Strength##cs", &mat.normalMapStrength,   0.0f, 4.0f))   editKey("normalMapStrength");
        if (EditorUI::DragFloat("Alpha Ref##cs",       &mat.alphaRef,            0.0f, 1.0f))   editKey("alphaRef");
    }

    // ---- Blend mode ----
    static const char* kBlendNames[] = { "Opaque", "Alpha", "Additive", "Premultiplied","Multiply","Masked","Dithered",
                                         "Transmissive" };
    static_assert(IM_ARRAYSIZE(kBlendNames) == static_cast<int>(BlendMode::Count),
                  "kBlendNames must list every BlendMode in enum order");
    int blendIdx = static_cast<int>(mat.userBlendMode);
    if (ImGui::Combo("Blend Mode", &blendIdx, kBlendNames, IM_ARRAYSIZE(kBlendNames)))
    {
        mat.userBlendMode = static_cast<BlendMode>(blendIdx);
        // Deliberately no write to alphaRef. The Masked default clip value lives
        // where the value is published (MaterialComponent::MaskClipValue, used by
        // WriteMaterialSlot / DDGISceneAS), so the authored field survives any round
        // trip. Seeding alphaRef on entering Masked would leave an Opaque material that
        // merely visited Masked alpha-tested (main view, shadow maps, probe bakes, RT
        // scene), and Ctrl+S would write that residue into the scene as a
        // MaterialOverride.
        editKey("blendMode");
    }
    if (mat.userBlendMode == BlendMode::Dithered && ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Dithered (stochastic transparency): deferred draw with a per-pixel,\n"
            "per-frame screen-door clip on Base Color alpha x texture alpha x\n"
            "Opacity Mask. TAA / DLSS / FSR2 resolve it into transparency and the\n"
            "surface receives ray-traced reflections, SSR, DDGI, RTAO and shadows\n"
            "like any opaque pixel (the forward Alpha route cannot). Needs a\n"
            "temporal AA - under FXAA it is a visible pattern. Shadows dither\n"
            "statically; probe bakes treat it as opaque.");
    if (mat.userBlendMode == BlendMode::Transmissive && ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Transmissive (glass, liquids, gems): forward draw whose background is\n"
            "refracted and tinted by the Transmission rows; Base Color alpha stays\n"
            "coverage. Thickness 0 is a thin wall (a window pane), above 0 a solid\n"
            "that bends what is behind it. The reflection's F0 follows IOR.");
    if (mat.userBlendMode == BlendMode::Masked)
    {
        const bool unsetRef = mat.alphaRef > kAlphaRefNoClipThreshold;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Masked (Unreal-style): deferred opaque draw with clip().\n"
                "Clip source = Base Color alpha x Opacity Mask (.r, optional slot).\n"
                "\"Alpha Ref\" is the Mask Clip Value.%s",
                unsetRef ? "\nAlpha Ref is unset (1.0), so Masked clips at the default 0.5.\n"
                           "Lower Alpha Ref to author a different clip value; it is never\n"
                           "rewritten for you, so leaving Masked restores the exact look."
                         : "");
        if (unsetRef)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(clips at %.2f)", kMaskClipDefault);
        }
    }

    // ---- Material domain (Surface / Decal) ----
    // Decal-domain materials drive a clustered projected decal on entities that
    // also carry a DecalComponent; they are excluded from mesh/trail/particle look
    // resolution (domain gates) and serialized as `domain` in the .imat.
    {
        // Index <-> value table, so a domain the combo cannot represent (e.g.
        // Transparent) shows as Surface and is not rewritten unless the user picks an
        // entry. Keep the values and names in lockstep; a new domain is one row here.
        // Water is not here on purpose: it is a shading model on a translucent Surface
        // material (Unreal's Single Layer Water), not a domain.
        static constexpr Resource::MaterialDomain kDomainValues[] = {
            Resource::MaterialDomain::Opaque,
            Resource::MaterialDomain::Decal,
        };
        static const char* kDomainNames[] = { "Surface", "Decal" };
        static_assert(IM_ARRAYSIZE(kDomainValues) == IM_ARRAYSIZE(kDomainNames),
            "kDomainValues and kDomainNames must stay in lockstep");

        int domainIdx = 0;   // fall back to Surface for a value not in the table
        for (int i = 0; i < IM_ARRAYSIZE(kDomainValues); ++i)
            if (mat.domain == kDomainValues[i]) { domainIdx = i; break; }

        if (ImGui::Combo("Domain", &domainIdx, kDomainNames, IM_ARRAYSIZE(kDomainNames)))
        {
            mat.domain = kDomainValues[domainIdx];
            editKey("domain");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Surface: normal GBuffer/Transparent mesh material.\n"
                "Decal:   projected clustered decal — pair with a DecalComponent.\n"
                "Slots: Base Color / Normal / Surface(ARM: AO=R Rough=G) /\n"
                "Opacity Mask(.r); Reflectance = flat specular write.\n"
                "\n"
                "Water is not a domain — it is a Shading Model on a translucent\n"
                "Surface material (see Use Custom Shader -> Shading Model).");

        if (mat.IsWaterShadingModel())
        {
            ImGui::Separator();
            ImGui::TextDisabled("Water (Single Layer)");
            ImGui::TextWrapped(
                "Drawn by WaterSurfacePass after the sky, before clouds/fog — a "
                "translucent Surface material redirected out of the ordinary "
                "translucency pass so it can write depth and stamp the GBuffer "
                "for screen-space reflections. Does not cast shadows and is "
                "excluded from ray tracing. The look is entirely your shader's; "
                "Blend Mode still selects the HDR blend.");
            if (!mat.useCustomShader || mat.customShaderPath.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f),
                    "No Material Shader assigned — using the engine default look.");
        }

        if (mat.IsDecalDomain())
        {
            ImGui::Separator();
            ImGui::TextDisabled("Decal");
            bool mask = false;
            // Bit values mirror DecalMaterialAsset::FLAGS / DECAL_WRITE_*.
            mask |= ImGui::CheckboxFlags("Write Base Color", &mat.decalWriteMask, 1u << 0);
            mask |= ImGui::CheckboxFlags("Write Normal",     &mat.decalWriteMask, 1u << 1);
            mask |= ImGui::CheckboxFlags("Write Roughness",  &mat.decalWriteMask, 1u << 2);
            mask |= ImGui::CheckboxFlags("Write Specular",   &mat.decalWriteMask, 1u << 3);
            mask |= ImGui::CheckboxFlags("Write AO",         &mat.decalWriteMask, 1u << 4);
            if (mask) editKey("decalWriteMask");
            if (EditorUI::DragFloat("Angle Fade Start", &mat.decalAngleFadeStart, 0.0f, 1.0f))
                editKey("decalAngleFade");
            int sortLayer = mat.decalSortLayer;
            if (ImGui::DragInt("Sort Layer", &sortLayer, 1.0f, -128, 127))
            {
                mat.decalSortLayer = sortLayer;
                editKey("decalSortLayer");
            }
        }
    }

    // ---- Shadow cull mode ----
    // Default = back-cull, Front = closed mesh, None = single-sided; alpha-tested
    // materials always cull None.
    static const char* kShadowCullNames[] = { "Default (Back)", "Front (Closed-Mesh)", "None (Single-Sided)" };
    int shadowCullIdx = static_cast<int>(mat.shadowCullMode);
    if (ImGui::Combo("Shadow Cull", &shadowCullIdx, kShadowCullNames, IM_ARRAYSIZE(kShadowCullNames)))
    {
        mat.shadowCullMode = static_cast<ShadowCullMode>(shadowCullIdx);
        editKey("shadowCullMode");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Default: back-face cull, relies on depth bias to fight self-shadow acne.\n"
            "Front:   front-face cull, eliminates self-shadow acne on closed meshes\n"
            "         (character bodies, organic solids). Shadows on other surfaces\n"
            "         gain a tiny gap (peter-pan). Wrong for single-sided geometry.\n"
            "None:    no cull — use for hair cards, eyelash planes, cloth, leaves.");

    // ---- Flags ----
    {
        bool castShadow    = mat.IsCastingShadow();
        bool receiveShadow = mat.IsReceiveShadow();
        bool doubleSided   = mat.IsDoubleSided();
        bool outline       = mat.IsOutlineEnabled();
        // SetCastShadow / SetReceiveShadow mark only the renderer dirty; an edit also
        // has to mark the asset (the "(unsaved)" tag, the MaterialOverride a scene
        // save writes) - editKey does.
        if (ImGui::Checkbox("Cast Shadow",    &castShadow))    { mat.SetCastShadow(castShadow); editKey("castShadow"); }
        ImGui::SameLine();
        if (ImGui::Checkbox("Receive Shadow", &receiveShadow)) { mat.SetReceiveShadow(receiveShadow); editKey("receiveShadow"); }
        if (ImGui::Checkbox("Double Sided",   &doubleSided))
        { editKey("doubleSided"); if (doubleSided) mat.flags |= MaterialComponent::DOUBLE_SIDED; else mat.flags &= ~MaterialComponent::DOUBLE_SIDED; }
        ImGui::SameLine();
        if (ImGui::Checkbox("Outline",        &outline))
        { editKey("outline"); if (outline) mat.flags |= MaterialComponent::OUTLINE; else mat.flags &= ~MaterialComponent::OUTLINE; }
        if (outline)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::DragFloat("##OutlinePixels", &mat.outlinePixels, 0.1f, 0.5f, 20.0f, "%.1f px"))
                editKey("outlinePixels");
            ImGui::SameLine();
            ImGui::TextDisabled("Thickness");

            bool ssOutline = mat.IsOutlineScreenSpaceEnabled();
            if (ImGui::Checkbox("Screen Space Outline", &ssOutline))
            { editKey("outlineScreenSpace"); mat.SetOutlineScreenSpaceEnabled(ssOutline); }
        }

        // Per-vertex color: multiply baseColor by the mesh's vertex-color stream
        // (imported meshes that carry vertex colors). Opt-in so primitives /
        // meshes whose color is incidental aren't tinted.
        bool useVtxColor = mat.IsUsingVertexColors();
        if (ImGui::Checkbox("Vertex Color", &useVtxColor))
        {
            editKey("useVertexColor");
            if (useVtxColor) mat.flags |= MaterialComponent::USE_VERTEXCOLORS;
            else             mat.flags &= ~MaterialComponent::USE_VERTEXCOLORS;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Multiply base color by the mesh's per-vertex color stream\n"
                              "(FBX/glTF/PMX meshes that carry vertex colors).");

        // Skip XeGTAO for characters / skin (avoids temporal ghosts on moving meshes);
        // the flag short-circuits AO to 1.0.
        bool excludeSSAO = (mat.flags & MaterialComponent::EXCLUDE_FROM_SSAO) != 0;
        if (ImGui::Checkbox("Exclude from SSAO", &excludeSSAO))
        {
            editKey("excludeFromSsao");
            if (excludeSSAO) mat.flags |=  MaterialComponent::EXCLUDE_FROM_SSAO;
            else             mat.flags &= ~MaterialComponent::EXCLUDE_FROM_SSAO;
        }

        // Hair mask: the GBuffer stamps RT4 extra.b = 1 so FaceOverlay-tagged
        // brows/lashes can redraw through this material's pixels. Only for materials
        // without a custom PS; custom hair shaders write the mask themselves
        // (asset/Shaders/Hair.ps.hlsl).
        bool hairMask = (mat.flags & MaterialComponent::HAIR_MASK) != 0;
        if (ImGui::Checkbox("Hair Mask (Brow Overlay)", &hairMask))
        {
            editKey("hairMask");
            if (hairMask) mat.flags |=  MaterialComponent::HAIR_MASK;
            else          mat.flags &= ~MaterialComponent::HAIR_MASK;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mark this material's pixels as HAIR in the GBuffer (RT4 extra.b)\n"
                              "so entities with a Face Overlay component (brows/lashes) show\n"
                              "through it. Needed on hair materials that do NOT use a custom\n"
                              "shader; custom hair shaders write the mask themselves.");
    }

    // ---- Material Parameters: the kMaterialPBRSchema rows ----
    // Add a field by extending the schema. Shown for custom-shader materials too:
    // a custom PS replaces only the GBuffer stage, and the deferred Lighting.ps
    // still shades the pixel with this material's shading-model params (for a
    // custom Toon material, the whole NPR block). Serialization and upload walk
    // the schema unconditionally.
    {
        ImGui::Separator();
        ImGui::Text("Material Parameters");
        if (mat.useCustomShader)
            ImGui::TextDisabled("Deferred shading-model params (Lighting.ps) — separate\n"
                                "from the custom shader's own Custom Parameters above.");
        if (mat.customShadingModel == ShadingModel::TwoSidedFoliage &&
            mat.useCustomShader && !mat.customParams.empty())
            ImGui::TextDisabled("Transmission tint / wrap / strength come from this shader's\n"
                                "Custom Parameters (slots 0-2), not from the rows below.");

        // Row visibility uses the schema's SM_* masks, derived from the shading
        // model's lighting route - the table's `route` column, the value
        // ShadingStencil::RefFor returns (shaderType is a tombstone nothing authors).
        // Unlit sees the SM_UNLIT rows, Toon the SM_ANY_NPR rows (the union of both
        // NPR masks; rows no shader reads are SM_HIDDEN), every lit model the
        // SM_ANY_PBR rows.
        uint32_t shaderMask;
        switch (ShadingModels::Of(mat.customShadingModel).route)
        {
        case SMT_ROUTE_UNLIT: shaderMask = MaterialSchema::SM_UNLIT;   break;
        case SMT_ROUTE_NPR:   shaderMask = MaterialSchema::SM_ANY_NPR; break;
        default:              shaderMask = MaterialSchema::SM_ANY_PBR; break;
        }
        const uint32_t modelMaskBit =
            1u << static_cast<uint32_t>(mat.customShadingModel);

        // TwoSidedFoliage reads its tint/wrap/strength from one of two places, and
        // the material packer (WriteMaterialSlot) decides by whether the material has
        // reflected b8 params (`customParamCount > 0`): a custom-shader foliage
        // material uses its own foliage parameters (named `@engine`, or the
        // customParams[0..2] convention two shipping Tokyo materials author), and only
        // a material without them reads the named subsurface fields. In the first case
        // the named rows would be three sliders nothing reads. `customParams` is the
        // CPU mirror of what WriteMaterialSlot counts.
        const bool foliageOnLegacyParams =
            mat.customShadingModel == ShadingModel::TwoSidedFoliage &&
            mat.useCustomShader && !mat.customParams.empty();
        const char* lastGroup = nullptr;

        // The checkbox shape matches the post-process volume panel: a checkbox
        // gating a disabled value widget. ovrSet and the parent snapshot are resolved
        // once at the top of this function.
        if (ovrSet)
        {
            ImGui::TextDisabled("Tick to override; unticked follows the parent.");
            ImGui::Spacing();
        }

        for (const auto& f : MaterialSchema::kMaterialPBRSchema)
        {
            if (!(f.visibleMask & shaderMask)) continue;
            // Second visibility axis: the shading model itself. The route mask cannot
            // tell Subsurface or Anisotropic from Standard (all take the PBR route), so
            // without this every anisotropy/subsurface row would appear on every PBR
            // material. Serialization does not filter on it, so switching models back and
            // forth keeps the values of the model left.
            if (!(f.modelMask & modelMaskBit)) continue;
            // Third axis: the blend mode (the transmission rows read nothing elsewhere).
            if (!(f.blendMask & MaterialSchema::BLD_BIT(mat.userBlendMode))) continue;
            // See foliageOnLegacyParams above: these are the three rows shared with
            // Subsurface, and on such a material nothing reads them. The `!= MDL_ANY` half
            // is load-bearing: MDL_ANY is ~0u and contains MDL_FOLIAGE, so testing the bit
            // alone would hide every ordinary row (base colour, roughness, ...).
            if (foliageOnLegacyParams &&
                f.modelMask != MaterialSchema::MDL_ANY &&
                (f.modelMask & MaterialSchema::MDL_FOLIAGE))
                continue;

            // Restore copies the whole float4 for every non-Float Kind:
            // FloatComponent rows alias their parent color's storage, so a
            // partial copy would leave the alias inconsistent.
            overrideMark(f.name, [&]{
                float*       dst = MaterialSchema::FieldPtr(mat, f);
                const float* src = MaterialSchema::FieldPtr(m_parentSnap, f);
                const int n = (f.kind == MaterialSchema::Kind::Float) ? 1 : 4;
                for (int i = 0; i < n; ++i) dst[i] = src[i];
            });
            const bool overridden = isOverridden(f.name);

            // Emit group label header on first sighting (mirrors hand-written subheadings).
            if (f.group && (!lastGroup || std::strcmp(f.group, lastGroup) != 0))
            {
                if (lastGroup) ImGui::Spacing();
                ImGui::TextDisabled("%s", f.group);
                lastGroup = f.group;
            }

            float* base    = MaterialSchema::FieldPtr(mat, f);
            bool   changed = false;

            // The relative NPR brightness ceiling is flag-gated, not 0-gated, so its
            // enable checkbox is drawn before its slider; the generic Kind::Float path has
            // no on/off companion.
            // A count, not a bool: a flag-gated row can push a disable and an inherited
            // row pushes another; one bool would leak the second push and grey out the
            // rest of the panel.
            int  pushedDisabled = 0;
            if (std::strcmp(f.name, "nprBrightnessClamp") == 0)
            {
                bool clampOn = mat.IsNprBrightnessClampEnabled();
                if (ImGui::Checkbox("Clamp Brightness to Albedo", &clampOn))
                {
                    mat.SetNprBrightnessClampEnabled(clampOn);
                    editKey("nprClampEnabled");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Caps output luminance at (albedo luminance x the value below).\n"
                        "Keeps texture contrast but flattens the response to light: at\n"
                        "1.0 the material can never be brighter than its own base color,\n"
                        "so it will not brighten under a stronger sun, and rim/specular\n"
                        "vanish on dark textures. OFF is the default; turn it on only for\n"
                        "a deliberately flat, paint-like look.");
                if (!clampOn) { ImGui::BeginDisabled(); ++pushedDisabled; }
            }
            // Same shape for the stylized cast-shadow edge: flag-gated, not 0-gated. It
            // cannot be value-gated: existing NPR .imat files already store a non-zero
            // nprSceneShadowSmooth, so "enable when != 0" would harden the cast-shadow
            // terminator on hundreds of materials.
            if (std::strcmp(f.name, "nprSceneShadowSmooth") == 0)
            {
                bool edgeOn = mat.IsNprToonShadowEdgeEnabled();
                if (ImGui::Checkbox("Stylized Cast-Shadow Edge", &edgeOn))
                {
                    mat.SetNprToonShadowEdgeEnabled(edgeOn);
                    editKey("nprToonShadowEdge");
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Sharpens the CAST shadow's edge into a toon terminator instead\n"
                        "of the raw PCF penumbra. The width below is the half-width of\n"
                        "the transition; 0 = off even with this ticked.\n\n"
                        "Shapes the EDGE only - a fully occluded pixel still receives\n"
                        "exactly zero sunlight at every setting. That is the difference\n"
                        "from the pre-2048cf7 sigmoid, which left up to 15%% of the sun\n"
                        "in full shadow and is what made characters glow in cast shadows.");
                if (!edgeOn) { ImGui::BeginDisabled(); ++pushedDisabled; }
            }

            // An inherited row is read-only: editing it would change a value the
            // file does not record, so the edit would vanish on the next load.
            if (!overridden) { ImGui::BeginDisabled(); ++pushedDisabled; }

            switch (f.kind)
            {
            case MaterialSchema::Kind::Float:
                changed = EditorUI::DragFloat(f.label, base, f.min, f.max);
                break;
            case MaterialSchema::Kind::Color3:
                changed = EditorUI::ColorEdit4(f.label, base, ImGuiColorEditFlags_NoAlpha);
                break;
            case MaterialSchema::Kind::Color4:
                changed = EditorUI::ColorEdit4(f.label, base);
                break;
            case MaterialSchema::Kind::FloatComponent:
                changed = EditorUI::DragFloat(f.label, base + f.component, f.min, f.max);
                break;
            }

            while (pushedDisabled-- > 0) ImGui::EndDisabled();

            if (changed)
            {
                mat.SetEditDirty();
                if (assetMode) *ctx.assetDirty = true;
            }
        }
    }

    // ---- Texture slots ----
    // Engine shaders only; a custom shader uses the reflected Custom Textures.
    if (!mat.useCustomShader)
    {
    ImGui::Separator();
    ImGui::Text("Texture Slots");
    if (mat.IsWaterShadingModel())
    {
        ImGui::TextDisabled("(Water domain — engine default look)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "These feed WaterSurface.ps.hlsl, the built-in water look.\n"
                "All four are optional: unassigned slots fall back to calm water,\n"
                "the computed shore->deep gradient, and no foam.\n"
                "Assign a Material Shader instead to author your own slots.");
    }

    static const char* kSlotNames[] = {
#define SLOT_LABEL_(e, label, path) label,
        MATERIAL_TEXTURE_SLOTS(SLOT_LABEL_)
#undef SLOT_LABEL_
    };

    constexpr float kPreviewSize = 64.f;

    // Water repurposes the engine slots: the generic PBR names mean nothing on a
    // water surface (no albedo texture, no ARM, no emissive). The slot indices are
    // unchanged, because everything downstream (serialization, overrides, the RT
    // alpha path) addresses them by index; only the label and the visibility
    // filter differ. WaterSurface.ps.hlsl reads exactly these indices - keep the
    // two in sync.
    const bool isWaterDomainTex = mat.IsWaterShadingModel();
    static const char* kWaterSlotNames[] = {
        nullptr,                        // 0 BASECOLORMAP - hidden (the Base Color swatch is the deep colour)
        "Flow Normal A (swell)",        // 1 NORMALMAP
        "Flow Normal B (detail)",       // 2 SURFACEMAP
        nullptr,                        // 3 EMISSIVEMAP - hidden
        "Depth Ramp (shore->deep)",     // 4 RAMPMAP
        "Foam Mask",                    // 5 OPACITYMAP
    };
    static_assert(IM_ARRAYSIZE(kWaterSlotNames) == MaterialComponent::TEXTURESLOT_COUNT,
        "kWaterSlotNames must cover every MATERIAL_TEXTURE_SLOTS entry");

    for (int s = 0; s < MaterialComponent::TEXTURESLOT_COUNT; ++s)
    {
        if (isWaterDomainTex)
        {
            if (!kWaterSlotNames[s]) continue;      // not meaningful for water
            const MaterialRuntimeTextureView rtw =
                InspectorEngineTex(ctx.renderer, selEnt, assetMatKey, static_cast<uint32_t>(s));
            static const char* kWaterSlotKeys[] = {
#define WATER_SLOT_KEY_(e, label, path) "tex_" #e,
                MATERIAL_TEXTURE_SLOTS(WATER_SLOT_KEY_)
#undef WATER_SLOT_KEY_
            };
            if (DrawTextureSlotWidget(kWaterSlotNames[s], mat.textures[s], rtw))
                editKey(kWaterSlotKeys[s]);
            ImGui::Spacing();
            continue;
        }

        // Filter texture slots by SHADING MODEL (shaderType is a tombstone).
        const bool isToonTex  = (mat.customShadingModel == ShadingModel::Toon);
        const bool isUnlitTex = (mat.customShadingModel == ShadingModel::Unlit);
        if (isToonTex)
        {
            // Toon: Base Color, Normal Map, Surface Map. Not the engine Ramp Texture
            // slot, which the toon model does not read: it samples its ramp from one of
            // the material's custom textures (toonRampTex: the one named
            // `@engine toonRamp`, else t3) and uses the two ramp colours without one.
            // Water reads RAMPMAP as its depth ramp, through its own slot list above.
            if (s != MaterialComponent::BASECOLORMAP &&
                s != MaterialComponent::NORMALMAP &&
                s != MaterialComponent::SURFACEMAP)
                continue;
        }
        else if (isUnlitTex)
        {
            // Unlit: only Base Color + Emissive textures are meaningful.
            if (s != MaterialComponent::BASECOLORMAP &&
                s != MaterialComponent::EMISSIVEMAP)
                continue;
        }
        else
        {
            // PBR: skip the Ramp Texture slot.
            if (s == MaterialComponent::RAMPMAP)
                continue;
        }

        const MaterialRuntimeTextureView runtime =
            InspectorEngineTex(ctx.renderer, selEnt, assetMatKey, static_cast<uint32_t>(s));
        // Key must match the .imat vocabulary (tex_BASECOLORMAP, ...), which is
        // what BuildTextImpl and MaterialOverride both use.
        static const char* kSlotOvrKeys[] = {
#define SLOT_OVR_KEY_(e, label, path) "tex_" #e,
            MATERIAL_TEXTURE_SLOTS(SLOT_OVR_KEY_)
#undef SLOT_OVR_KEY_
        };
        const char* slotKey = kSlotOvrKeys[s];
        overrideMark(slotKey, [&]{ mat.textures[s] = m_parentSnap.textures[s]; });
        const bool slotOverridden = isOverridden(slotKey);
        if (!slotOverridden) ImGui::BeginDisabled();
        if (DrawTextureSlotWidget(kSlotNames[s], mat.textures[s], runtime))
        {
            mat.SetEditDirty();
            if (assetMode) *ctx.assetDirty = true;
        }
        if (!slotOverridden) ImGui::EndDisabled();

        if (s < MaterialComponent::TEXTURESLOT_COUNT - 1)
            ImGui::Spacing();
    }
    } // if (!mat.useCustomShader)

    // ---- UV tiling / offset ----
    ImGui::Separator();
    ImGui::Text("UV Transform");
    if (ImGui::DragFloat2("Tiling",  &mat.texMulAdd.x, 0.01f)) editKey("texTiling");
    if (ImGui::DragFloat2("Offset",  &mat.texMulAdd.z, 0.01f)) editKey("texOffset");
}

} // namespace Editor
