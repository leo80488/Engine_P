// EditorLayer_ComponentEditors.cpp - the Inspector's component editors:
// RegisterDefaultEditors (every built-in component editor and tag
// registration, in one function - their order matters, see EditorLayer.h),
// the script slots' exposed-variable widgets (DrawScriptVarWidgets,
// DrawScriptComponentSlots) and DrawMeshChannelInfo (the vertex-channel rows
// the mesh editors show).

#include "Editor/ColorWidgets.h"
#include "Editor/EditorLayer.h"
#include "Resource/AssetTypes.generated.h"
#include "Editor/AttackEditor.h"
#include "Editor/AnimGraphEditorWindow.h"
#include "Editor/AnimSetEditorWindow.h"
#include "ECS/IKRigComponent.h"
#include "Editor/EditorLayerShared.h"  // SaveFileDialog and the other helpers shared by the EditorLayer_* files
#include "Editor/ImGuiWidgets.h"
#include "Editor/MaterialPreviewWidget.h"
#include "Editor/CameraPreviewWindow.h"
#include "Editor/AssetPreviewWindow.h"
#include "Editor/MeshEditorWindow.h"       // the Mesh Editor
#include "Editor/SkeletonEditorWindow.h"   // the Skeleton Editor
#include "Editor/TextureViewerWindow.h"
#include "Editor/UIEditorWindow.h"   // Canvas / UI Rect: Edit in UI Editor
#include "Editor/MaterialPropertyPanel.h"
#include "Editor/MaterialEditorWindow.h"
#include "Editor/NodeEditorSmoke.h"       // the node editor's acceptance smoke run
#include "Graphics/FormatNames.h"
#include "Editor/SplitViewportPanel.h"
#include "Editor/ChainPhysicsPreviewWindow.h" // chain physics preview
#include "System/CommandLine.h"
#include "Harness/ControlChannelJobs.h"
#include "Editor/EntityRefPicker.h"
#include "ECS/PlayModeController.h"   // authoritative Edit/Play state machine
#include "Reflection/ComponentReflection.h" // Reflect::Descriptor<T> specializations for RegisterReflectedComponent
#include "Sequencer/SequenceEval.h"        // Seq::AcquireSequence - the inspector transport needs the asset's frame count
#include "ECS/GuidComponent.h"
#include "ECS/GuidRegistry.h"
#include "AI/AIComponents.h"
#include "AI/AISystem.h"   // m_aiSys->AcquireTree from the AIComponent inspector
#include "AI/BTAsset.h"
#include "AI/BTNode.h"
#include "ECS/Components.h"           // Transform, MaterialComponent
#include "ECS/HierarchyOps.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/PerObjectShadowComponent.h" // per-object (high-res) shadow opt-in tag
#include "ECS/HairShadowCasterComponent.h" // hair (bangs -> face) shadow opt-in tag
#include "ECS/FollowComponents.h"
#include "ECS/FollowSystem.h"
#include "Editor/EditorModule.h"              // the modules' inspector sections
#include "UI/UICanvas.h"
#include "UI/UIDocument.h"     // the UI Document section
#include "UI/WorldSpaceUI.h"
#include "ECS/TerrainComponent.h"     // TerrainComponent + TerrainLayer
#include "ECS/BillboardFXComponent.h" // BillboardFXComponent (animated sprite billboard)
#include "ECS/ParticleComponent.h"    // ParticleEmitterComponent
#include "ECS/TrailComponent.h"       // TrailComponent
#include "ECS/AnimFXGateComponent.h"  // AnimFXGateComponent - anim-gated weapon FX
#include "ECS/BeamComponent.h"        // BeamComponent (procedural-tube heavy beam)
#include "Graphics/TracerSystem.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/TagComponent.h"
#include "Scene/MeshSpawner.h"
#include "Scene/SceneDefaults.h"
#include "Resource/SceneSerializer.h"  // SaveScene / LoadScene
#include "Resource/AssetFS.h"
#include "Resource/PostProcessConfig.h" // Save/Load/Apply post-process config (.ippc)
#include "Resource/EngineSettings.h"
#include "Editor/InputBindingsWindow.h" // Window > Input Bindings (rebind + pad diagnostics)
#include "Platform/Window.h"
#include "ECS/HierarchyComponents.h"  // LocalTransform, GlobalTransform, Parent, Children, etc.
#include "ECS/AnimEntityResolve.h"    // a model root's skeleton carrier
#include "ECS/StaticMeshComponents.h" // StaticMesh owners and their derived sections
#include "Resource/MeshLibIdentity.h" // MeshKeyToHex - StaticMesh slot keys
#include "Scene/MeshAssetRegistry.h"
#include "ECS/PhysicsComponents.h"    // RigidBodyComponent, ColliderComponent
#include "Tools/CollisionMeshBaker.h" // Bake render meshes into combined .meshlib
#include "Resource/MeshLibrary.h"     // DrawMeshChannelInfo reads a library's mesh flags and entries
#include "Resource/AssetHeader.h"     // ComputeMeshLibVertexLayout + MESHLIB_FLAG_* (mesh channel inspector)
#include "System/TaskSystem.h"
#include "System/PathUtil.h"
#include "Editor/Undo/ComponentEditAction.h" // Add/RemoveComponentAction (pulls ComponentBlob.h)
#include "Editor/Undo/TransformEditAction.h"
#include "Scripting/LuaEntityBindings.h"
#include "Editor/Undo/ValueEditAction.h"
#include "Editor/Undo/StructuralAction.h"
#include "Editor/Undo/RenameAction.h"
#include "Physics/PhysicsSystem.h"
#include "Nav/NavComponents.h"        // NavAgentComponent (reflection-registered)
#include "ECS/AIIntentComponent.h"    // AIIntentComponent (reflection-registered)
#include "ECS/PerceptionComponent.h"  // PerceptionComponent (reflection-registered)
#include "Scene/SceneInstanceLoader.h"
#include "Resource/ModelImportWrite.h"   // non-destructive model-import write policy
#include "Resource/RetargetBaker.h"
#include "Editor/HumanoidMapping.h"             // the mapping rows, the .iskel save
#include "Resource/AnimationSerializer.h"
#include "Resource/AnimationLoader.h"
#include "Renderer/Renderer.h"
#include <chrono>
#include "Graphics/ShaderReflection.h"
#include "Graphics/ShaderLibrary.h"
#include "ECS/MaterialSchema.h"
#include "Graphics/GraphicsDX12.h"
#include "RenderGraph/RenderPass/SkyIBLPass.h"
#include "PostProcess/PostChain.h"
#include "PostProcess/ProfileSystem.h"
#include "PostProcess/PostProcessProfileSerializer.h"
#include "PostProcess/PostProcessRuntime.h"
#include "ECS/PostProcessVolumeComponent.h"
#include "RenderGraph/RenderPass/DDGIProbeDebugPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/PointShadowPass.h"
#include "System/Log.h"
#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/AssetManager.h"
#include "Resource/MaterialImporter.h"
#include "Resource/MaterialSerializer.h"
#include "Resource/MaterialOverrideApply.h"
#include "Resource/MaterialLiveOverlay.h"
#include "Resource/DecalMaterialSerializer.h"
#include "Resource/PrefabSerializer.h"
#include "Module/ModuleRegistry.h"           // the module that owns a core-registered component
#include "Resource/ComponentSerializers.h"
#include "Resource/ComponentWrite.h"
#include "Resource/TextureImporter.h"
#include "Resource/TextureImportRecord.h"
#include "Resource/AssetGuid.h"        // an import keeps (or mints) its .meta identity
#include "System/Stage.h"
#include "System/UiWindows.h"
#include "Resource/ShaderImporter.h"
#include "Resource/SceneImporter.h"
#include "Resource/PmxImporter.h"
#include "Resource/VrmImporter.h"
#include "Resource/AnimationImporter.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "Resource/AnimationSerializer.h"
#include "Resource/VmdImporter.h"
#include "Audio/AudioImporter.h"
#include "ECS/AnimationComponents.h"
#include "ECS/FacialAnimationSystem.h" // FacialAnimationSystem::TriggerBlink + the Facial:: helpers; brings FacialAnimationComponent.h and SkeletonAsset.h
#include "ECS/NotifyTypes.h"
#include "ECS/FollowComponents.h"
#include "ECS/SkyboxComponent.h"
#include "ECS/ReflectionProbeComponent.h"
#include "ECS/DDGIComponents.h"
#include "Physics/ChainPhysicsSystem.h"
#include "Scripting/ScriptComponent.h"
#include "Editor/DebugDrawSystem.h"
#include "Scripting/ScriptSystem.h"     // exposed-var schema + live push
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsDX12.h"
#include "Graphics/GPUProfiler.h"
#include "imgui/imgui_impl_win32.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/ImGuizmo/ImGuizmo.h"
#include "Editor/ChainRestPoseMath.h"      // ChainPreview UI gizmo: shared rest-pose delta math
#include <cassert>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <optional>
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>

using namespace DirectX;

namespace
{
    // Mesh vertex-channel inspector. Surfaces which interleaved streams a mesh
    // actually carries: pos / normal / uv0 are always present (base 32 B); the
    // optional tangent / uv1 / color streams are derived from the library's
    // MESHLIB_FLAG_* bits through the single-source-of-truth helper
    // ComputeMeshLibVertexLayout(), so what's shown here is exactly the layout
    // MeshManager builds the MeshDescriptor from. Imported meshes (MeshLibRef)
    // report their real per-mesh layout; engine primitives (MeshHandle, with no
    // backing library) get a short fixed-channel note instead.
    void DrawMeshChannelInfo(Resource::MeshLibrary* meshLib, World* w, Entity e)
    {
        ImGui::Separator();
        ImGui::TextDisabled("Vertex Channels");

        const MeshLibRef* ref = w ? w->GetComponent<MeshLibRef>(e) : nullptr;
        if (!ref || !ref->IsValid() || !meshLib)
        {
            // Primitive / non-library mesh: layout is fixed by the in-engine
            // generator (positions + normals + uv0; tangent basis synthesised
            // in the vertex shader from the normal).
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                               "  [x] Position  Normal  UV0");
            ImGui::TextDisabled("  (primitive / no mesh library — fixed layout)");
            return;
        }

        const uint32_t flags = meshLib->GetMeshLibFlags(ref->libHandle);
        const Resource::MeshLibVertexLayout L =
            Resource::ComputeMeshLibVertexLayout(flags);

        const ImVec4 onCol (0.45f, 0.85f, 0.45f, 1.0f);   // present - green
        const ImVec4 offCol(0.55f, 0.55f, 0.55f, 1.0f);   // absent  - gray

        auto row = [&](const char* name, const char* type,
                       bool present, uint32_t offset)
        {
            if (present)
                ImGui::TextColored(onCol,  "  [x] %-9s %-7s @ +%2u B", name, type, offset);
            else
                ImGui::TextColored(offCol, "  [ ] %-9s %-7s (absent)", name, type);
        };

        constexpr uint32_t kAbsent = 0xFFFFFFFFu;   // MeshLibVertexLayout sentinel
        row("Position", "float3", true,                      L.posOffset);
        row("Normal",   "float3", true,                      L.normalOffset);
        row("UV0",      "float2", true,                      L.uv0Offset);
        row("Tangent",  "float4", L.tangentOffset != kAbsent, L.tangentOffset);
        row("UV1",      "float2", L.uv1Offset     != kAbsent, L.uv1Offset);
        row("UV2",      "float2", L.uv2Offset     != kAbsent, L.uv2Offset);
        row("UV3",      "float2", L.uv3Offset     != kAbsent, L.uv3Offset);
        row("Color",    "RGBA8",  L.colorOffset   != kAbsent, L.colorOffset);

        ImGui::Spacing();
        ImGui::TextDisabled("stride %u B   |   meshId %u", L.stride, ref->meshId);
        if (const auto* entry = meshLib->GetEntry(ref->libHandle, ref->meshId))
            ImGui::TextDisabled("verts %u   |   indices %u",
                                entry->vertexCount, entry->indexCount);
    }
}

// Per-entity values live in the slot's `vars` override map and serialize with the scene.
void EditorLayer::DrawScriptVarWidgets(ScriptComponent& sc, std::size_t slot, World* /*world*/, Entity e)
{
    if (!m_scriptSys || slot >= sc.scripts.size()) return;
    ScriptInstance& si = sc.scripts[slot];
    auto& vars = si.vars;
    if (si.scriptPath.empty()) return;

    const std::vector<ScriptVarDesc>& schema = m_scriptSys->GetExposedSchema(si.scriptPath);

    ImGui::Separator();
    if (schema.empty())
    {
        ImGui::TextDisabled("No exposed variables");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Declare a `T.exposed = { ... }` table in the script to surface tunable values here.");
        return;
    }
    ImGui::TextDisabled("Exposed Variables");

    // An asset extension's drag-drop payload, as the Content Browser publishes it (AssetTypes.json).
    auto payloadForExt = [](const std::string& ext) -> const char* {
        const Resource::AssetTypes::Row* row = Resource::AssetTypes::FindByExt(ext);
        return row ? row->payload : nullptr;
    };

    const bool playing = (GetPlayState() != ViewportPlayState::Stopped);
    const float btnW   = ImGui::GetFrameHeight();
    bool anyChanged    = false;

    for (const ScriptVarDesc& d : schema)
    {
        ImGui::PushID(d.name.c_str());

        const bool        overridden = (vars.find(d.name) != vars.end());
        const ScriptVarValue cur     = ResolveScriptVar(d, vars);
        const char* label = d.label.empty() ? d.name.c_str() : d.label.c_str();

        ScriptVarValue next    = cur;
        bool           changed = false;

        // Reserve room for the trailing reset button and the widget's own label.
        // ImGui draws the label to the right of the frame, so a full-width item
        // pushes the label off the panel edge and clips it - subtract the label's
        // measured width (text before "##") so it always fits.
        if (d.type != ScriptVarType::Bool)
        {
            const ImGuiStyle& st = ImGui::GetStyle();
            const float labelW = ImGui::CalcTextSize(label, nullptr, true).x;
            float w = ImGui::GetContentRegionAvail().x - btnW
                    - st.ItemSpacing.x - st.ItemInnerSpacing.x - labelW;
            if (w < 80.f) w = 80.f;   // keep the field usable on very long labels
            ImGui::SetNextItemWidth(w);
        }

        switch (d.type)
        {
        case ScriptVarType::Float:
        {
            float v = cur.data.f;
            if (d.hasMin && d.hasMax)
                changed = EditorUI::DragFloat(label, &v, d.minVal, d.maxVal);
            else
                changed = ImGui::DragFloat(label, &v, d.speed > 0.f ? d.speed : 0.01f,
                                           d.hasMin ? d.minVal : 0.f, d.hasMax ? d.maxVal : 0.f);
            if (changed) next = ScriptVarValue::MakeFloat(v);
        } break;
        case ScriptVarType::Int:
        {
            int v = cur.data.i;
            if (d.hasMin && d.hasMax)
                changed = EditorUI::DragInt(label, &v, (int)d.minVal, (int)d.maxVal);
            else
                changed = ImGui::DragInt(label, &v, d.speed > 0.f ? d.speed : 1.f,
                                         d.hasMin ? (int)d.minVal : 0, d.hasMax ? (int)d.maxVal : 0);
            if (changed) next = ScriptVarValue::MakeInt(v);
        } break;
        case ScriptVarType::Bool:
        {
            bool v = cur.data.b;
            changed = ImGui::Checkbox(label, &v);
            if (changed) next = ScriptVarValue::MakeBool(v);
        } break;
        case ScriptVarType::Float3:
        {
            float v[3] = { cur.data.v3[0], cur.data.v3[1], cur.data.v3[2] };
            changed = ImGui::DragFloat3(label, v, d.speed > 0.f ? d.speed : 0.01f,
                                        d.hasMin ? d.minVal : 0.f, d.hasMax ? d.maxVal : 0.f);
            if (changed) next = ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
        } break;
        case ScriptVarType::Color:
        {
            float v[3] = { cur.data.v3[0], cur.data.v3[1], cur.data.v3[2] };
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float;
            if (d.hdr) flags |= ImGuiColorEditFlags_HDR;
            changed = EditorUI::ColorEdit3(label, v, flags);
            if (changed) next = ScriptVarValue::MakeColor(v[0], v[1], v[2]);
        } break;
        case ScriptVarType::String:
        {
            // `ext = 'tag'` in the exposed schema turns the free-text field
            // into a TagRegistry dropdown (pick, do not type - no typo drift).
            if (!d.options.empty())
            {
                if (ImGui::BeginCombo(label, cur.str.c_str()))
                {
                    for (const auto& option : d.options)
                    {
                        if (ImGui::Selectable(option.c_str(), cur.str == option))
                        { next = ScriptVarValue::MakeString(option); changed = true; }
                        if (cur.str == option) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }
            else if (d.assetExt == "tag")
            {
                const char* preview = cur.str.empty() ? "(none)" : cur.str.c_str();
                if (ImGui::BeginCombo(label, preview))
                {
                    if (ImGui::Selectable("(none)", cur.str.empty()))
                    { next = ScriptVarValue::MakeString(""); changed = true; }
                    for (const std::string& t : TagRegistry::Get().Tags())
                        if (ImGui::Selectable(t.c_str(), cur.str == t))
                        { next = ScriptVarValue::MakeString(t); changed = true; }
                    if (!cur.str.empty() && !TagRegistry::Get().Contains(cur.str))
                    {
                        ImGui::Separator();
                        ImGui::TextDisabled("current (unregistered): %s", cur.str.c_str());
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                char buf[512];
                strncpy_s(buf, cur.str.c_str(), _TRUNCATE);
                if (ImGui::InputText(label, buf, sizeof(buf)))
                {
                    next = ScriptVarValue::MakeString(buf);
                    changed = true;
                }
            }
        } break;
        case ScriptVarType::Asset:
        {
            char buf[512];
            strncpy_s(buf, cur.str.c_str(), _TRUNCATE);
            if (ImGui::InputText(label, buf, sizeof(buf)))
            {
                next = ScriptVarValue::MakeAsset(buf);
                changed = true;
            }
            if (const char* payload = payloadForExt(d.assetExt))
            {
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(payload))
                    {
                        const char* dropped = static_cast<const char*>(p->Data);
                        if (dropped && *dropped) { next = ScriptVarValue::MakeAsset(dropped); changed = true; }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        } break;
        case ScriptVarType::Entity:
        {
            // Persisted by GUID (cur.str) so the ref survives save/load and
            // prefab instantiation; data.entity is a live-session fallback.
            // Resolve to a live entity just to show a readable name here.
            Entity curEnt = NullEntity;
            if (!cur.str.empty())
                curEnt = ECS::GuidRegistry::Get().Find(ECS::Guid::FromHex(cur.str));
            if (curEnt == NullEntity && cur.data.entity != 0)
                curEnt = (Entity)cur.data.entity;

            char buf[256];
            if (curEnt != NullEntity && m_world && m_world->IsAlive(curEnt))
                snprintf(buf, sizeof(buf), "%s (#%u)",
                         m_world->GetName(curEnt).c_str(), (uint32_t)curEnt);
            else
                strncpy_s(buf, "<none — drop an entity>", _TRUNCATE);

            ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_ReadOnly);
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ENTITY"))
                {
                    const Entity de = *static_cast<const Entity*>(p->Data);
                    // Stamp a GUID on the target (idempotent) and store it as the
                    // persistent identity, mirroring AttachmentRef::BindAndStamp.
                    std::string guidHex;
                    if (m_world) guidHex = ECS::EnsureGuidOn(*m_world, de).ToHex();
                    next = ScriptVarValue::MakeEntity((uint32_t)de, std::move(guidHex));
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
        } break;
        }

        if (!d.tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", d.tooltip.c_str());

        // Trailing reset-to-default button (enabled only when this entity has
        // an override stored for the variable).
        ImGui::SameLine();
        ImGui::BeginDisabled(!overridden);
        const bool resetClicked = ImGui::Button("R", ImVec2(btnW, 0));
        ImGui::EndDisabled();
        if (overridden && ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset '%s' to default", d.name.c_str());

        ImGui::PopID();

        if (changed)            // widget edit -> store / refresh the per-entity override
        {
            vars[d.name] = next;
            anyChanged = true;
        }
        else if (resetClicked)  // drop the override -> falls back to the schema default
        {
            vars.erase(d.name);
            anyChanged = true;
        }
    }

    if (anyChanged && playing)
        m_scriptSys->ApplyExposedVars(e, slot, vars);
}

// ScriptSystem's reconcile loop tears down / spawns slots next frame (it diffs scripts.size()).
void EditorLayer::DrawScriptComponentSlots(ScriptComponent& sc, World* world, Entity e)
{
    int removeIdx = -1;

    for (std::size_t i = 0; i < sc.scripts.size(); ++i)
    {
        ImGui::PushID(static_cast<int>(i));
        ScriptInstance& si = sc.scripts[i];

        // Header shows the index + file stem so reordering stays legible.
        std::string stem = si.scriptPath;
        if (auto p = stem.find_last_of("/\\"); p != std::string::npos)
            stem = stem.substr(p + 1);

        // Runtime health, read before the header so a faulted slot is visible
        // even while collapsed.
        bool slotFaulted = false;
        const std::string* slotErr = m_scriptSys
            ? m_scriptSys->GetLogicSlotError(e, i, &slotFaulted) : nullptr;

        char hdr[192];
        snprintf(hdr, sizeof(hdr), "%zu: %s%s%s###scriptslot%zu",
                 i, stem.empty() ? "(no script)" : stem.c_str(),
                 si.enabled ? "" : " (disabled)",
                 slotErr ? " (!)" : "", i);

        // Distinct per-slot header tint so multiple scripts are easy to tell
        // apart at a glance. Hue is spread by the golden ratio (0.618) for good
        // separation between adjacent slots; disabled slots are desaturated/dim.
        float hue = 0.08f + 0.618f * static_cast<float>(i);
        hue -= static_cast<float>(static_cast<int>(hue));           // fractional part -> [0,1)
        const float sat = si.enabled ? 0.50f : 0.12f;
        const float val = si.enabled ? 0.42f : 0.26f;
        ImVec4 cBase, cHover, cActive;
        ImGui::ColorConvertHSVtoRGB(hue, sat,         val,         cBase.x,   cBase.y,   cBase.z);   cBase.w   = 1.0f;
        ImGui::ColorConvertHSVtoRGB(hue, sat,         val + 0.13f, cHover.x,  cHover.y,  cHover.z);  cHover.w  = 1.0f;
        ImGui::ColorConvertHSVtoRGB(hue, sat + 0.10f, val + 0.22f, cActive.x, cActive.y, cActive.z); cActive.w = 1.0f;
        ImGui::PushStyleColor(ImGuiCol_Header,        cBase);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, cHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  cActive);
        const bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);   // tint only the header bar, not the body

        if (open)
        {
            ImGui::Checkbox("Enabled", &si.enabled);

            // .lua path: free text + ILUA_PATH drag-drop from the Resource panel.
            char buf[512];
            strncpy_s(buf, si.scriptPath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Script Path", buf, sizeof(buf)))
                si.scriptPath = buf;
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ILUA_PATH"))
                {
                    const char* dropped = static_cast<const char*>(p->Data);
                    if (dropped && *dropped) si.scriptPath = dropped;
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TextDisabled("Drop a .lua file from the Resource Panel");

            // This slot's editor-exposed variables.
            DrawScriptVarWidgets(sc, i, world, e);

            if (slotErr)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s %s",
                                   slotFaulted ? "FAULTED (OnUpdate threw; parked until Stop or hot reload):"
                                               : "Last error:",
                                   slotErr->c_str());
                ImGui::PopStyleColor();
            }

            if (ImGui::Button("Remove Script"))
                removeIdx = static_cast<int>(i);
        }
        ImGui::PopID();
    }

    if (removeIdx >= 0)
        sc.scripts.erase(sc.scripts.begin() + removeIdx);

    ImGui::Separator();
    if (ImGui::Button("+ Add Script"))
        sc.scripts.emplace_back();
}

// ==== Built-in component editors and tags ====
std::string EditorLayer::ModuleOwningComponent(std::type_index type)
{
    const auto& all = GetComponentRegistry().All();
    const auto it = all.find(type);
    if (it == all.end()) return {};
    const std::string* owner = Module::Registry::Get().OwnerOfComponentTag(it->second.tag);
    return owner ? *owner : std::string{};
}

void EditorLayer::RegisterDefaultEditors()
{
    InstallReflectionHooks();
    Renderer* renderer = m_renderer; // captured by lambdas below

    // Project-wide tag list (asset/tags.txt) backing every tag dropdown -
    // the Tag component picker and ext='tag' script string vars. A missing
    // file just means an empty registry until the first "New..." tag.
    TagRegistry::Get().Load();

    // ---- Material (Add-menu-only entry) ----
    // MaterialComponent deliberately has no Reflect descriptor - its UI is the
    // hand-written built-in "Material" section in RenderInspectorPanel. This
    // draw-less entry exists so VFX entities (trail/particle) can add a
    // material from the editor exactly like a mesh entity (mesh-like split:
    // component = shape, material = look). The generic section loop skips
    // draw-less entries, so the built-in section stays the single UI owner.
    {
        ComponentEditorEntry e;
        e.label    = "Material";
        e.type     = std::type_index(typeid(MaterialComponent));
        e.category = "Rendering";
        e.priority = 4;
        e.fetch    = [](World& w, Entity ent) -> void*
            { return w.GetComponent<MaterialComponent>(ent); };
        e.add      = [](World& w, Entity ent)
            { w.AddComponent<MaterialComponent>(ent, MaterialComponent{}); };
        e.remove   = [](World& w, Entity ent)
            { w.RemoveComponent<MaterialComponent>(ent); };
        m_componentEditors.emplace(e.type, std::move(e));
    }

    // Pure descriptor - FOV via angle conversion, near/far planes.
    RegisterReflectedComponent<CameraComponent>("Camera", /*priority*/ 5);

    // FPS controller state - yaw/pitch sliders + mouse/move tuning. The
    // camera pose itself is edited through the Local Transform component.
    // followTarget is an AttachmentRef - picker rendered here in postDraw
    // (REFLECT_BEGIN intentionally omits it; see ComponentReflection.h).
    RegisterReflectedComponent<CameraControllerComponent>(
        "Camera Controller", /*priority*/ 6,
        [](CameraControllerComponent& c, World* w, Entity e)
    {
        if (!w) return;
        if (c.mode != CameraControllerComponent::Mode::Free)
            Editor::DrawAttachmentRef("Follow Target", *w, e, c.followTarget);
    });

    // ---- Camera stack (docs/design/camera_stack_system.md) ----
    RegisterReflectedComponent<VirtualCameraComponent>("Virtual Camera (VCam)", /*priority*/ 7);
    RegisterReflectedComponent<CameraPoseComponent>   ("Camera Pose",           /*priority*/ 8);
    RegisterReflectedComponent<VCamPriorityComponent> ("VCam Priority",         /*priority*/ 9);
    RegisterReflectedComponent<VCamBlendComponent>    ("VCam Blend",            /*priority*/ 10);
    RegisterReflectedComponent<FollowCameraComponent>(
        "Follow Camera", /*priority*/ 11,
        [](FollowCameraComponent& fc, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Target", *w, e, fc.target);
    });
    RegisterReflectedComponent<AimCameraComponent>(
        "Aim Camera", /*priority*/ 12,
        [](AimCameraComponent& ac, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Target", *w, e, ac.target);
    });
    RegisterReflectedComponent<CameraShakeComponent>  ("Camera Shake",          /*priority*/ 13);
    RegisterReflectedComponent<LiveCameraComponent>   ("Live Camera (read-only)", /*priority*/ 14);

    // ---- Identity (docs/design/entity_persistence_architecture.md) ----
    // GuidComponent is intentionally not registered in the Inspector: it has
    // no user-editable fields (regenerating breaks every existing ref) and is
    // surfaced via the [guid:xxxxxxxx] badge in the Inspector header instead.
    // BindAndStamp auto-adds it whenever an AttachmentRef is dropped onto an
    // entity, so users never need to add it manually either.
    RegisterComponentTag<PersistentTag>("Persistent (save-game)");

    // LocalTransform - descriptor with a custom Euler-degree widget for rotation.
    RegisterReflectedComponent<LocalTransform>("Local Transform", /*priority*/ 0);

    // RigidBody - descriptor for motion/mass/damping; postDraw shows "needs Collider" + BodyID.
    RegisterReflectedComponent<RigidBodyComponent>("Rigid Body",
        /*priority*/ 20,
        [](RigidBodyComponent& rb, World* world, Entity e)
        {
            const bool created     = rb.bodyId != kInvalidPhysicsBodyId;
            const bool hasCollider = world && world->HasComponent<ColliderComponent>(e);
            ImGui::Separator();
            if (!created)
            {
                if (!hasCollider)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                        "Needs a Collider component to activate physics.");
                    if (ImGui::Button("Add Default Box Collider"))
                        world->AddComponent<ColliderComponent>(e, ColliderComponent{});
                }
                else
                {
                    ImGui::TextDisabled("Body will be created on next physics tick.");
                }
            }
            else
            {
                ImGui::TextDisabled("BodyID: 0x%08X (live)", rb.bodyId);
                ImGui::TextDisabled("Remove + re-add component to apply creation-time changes.");
            }
        });

    // Collider - shape-conditional visibility for halfExtents/radius/halfHeight.
    // postDraw: edit it in the viewport; fit a primitive to the mesh's bounds
    // (the inspector's dispatch records the undo step for both field writes).
    RegisterReflectedComponent<ColliderComponent>("Collider", /*priority*/ 21,
        [this](ColliderComponent& col, World* world, Entity e)
        {
            if (!world) return;
            ImGui::Separator();
            bool edit = m_editCollider && m_selectedEntity == e;
            if (ImGui::Checkbox("Edit in viewport", &edit))
            {
                if (edit && m_selectedEntity != e) m_selectedEntity = e;
                SetEditCollider(edit);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The viewport gizmo (W / E / R) moves, turns and sizes this collider\n"
                                  "instead of the entity (the toolbar's Collider toggle).");
            const bool primitive = col.shape == ColliderComponent::Shape::Box ||
                                   col.shape == ColliderComponent::Shape::Sphere ||
                                   col.shape == ColliderComponent::Shape::Capsule;
            if (primitive)
            {
                ImGui::SameLine();
                if (ImGui::Button("Fit to Bounds"))
                {
                    std::string why;
                    if (!FitColliderToBounds(e, col, why)) LOG_WARNING("Collider: Fit to Bounds - %s", why.c_str());
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Offset, rotation and size from the entity's mesh bounds\n"
                                      "(its own mesh, or its Static Mesh's sections).");
            }
        });

    // Tag - gameplay identity tags (Unity Tag analogue). The whole editor is
    // one custom widget (list + remove buttons + add row); serialized via the
    // "Tag" component serializer so editor-authored tags survive save/load.
    RegisterReflectedComponent<TagComponent>("Tag", /*priority*/ 1);
    // AI strategic / tactical / sensing components share priority 22. Order
    // matters for the inspector list: AIIntent (strategic, top) -> NavAgent
    // (tactical, mid) -> Perception (sensing).
    RegisterReflectedComponent<AIIntentComponent> ("AI Intent",   /*priority*/ 22);
    RegisterReflectedComponent<NavAgentComponent> ("NavAgent",    /*priority*/ 22);
    RegisterReflectedComponent<PerceptionComponent>("Perception", /*priority*/ 22);
    // Passive counterpart: an entity that only needs to be SEEN carries this
    // and pays nothing per frame. Listed right after Perception so the two
    // read as a pair in the Add Component menu.
    RegisterReflectedComponent<PerceptibleComponent>("Perceptible", /*priority*/ 22);
    RegisterReflectedComponent<AIBehaviorComponent>("AI Behavior",23,
        [](AIBehaviorComponent& c,World* world,Entity self){
            if(!world)return;
            if(c.targetMode==2)Editor::DrawAttachmentRef("Fixed target",*world,self,c.fixedTarget);
            ImGui::TextWrapped("Automatic targets use Perception visibility and different factions. Targets need Perceptible enabled.");
            ImGui::SeparatorText("Patrol route");
            ImGui::TextWrapped("Pick scene markers in travel order. Empty route stays at home. Move markers with the scene gizmo.");
            int remove=-1,up=-1,down=-1;
            for(int i=0;i<int(c.patrolPoints.size());++i){
                ImGui::PushID(i);
                char label[32];snprintf(label,sizeof(label),"Point %d",i+1);
                Editor::DrawAttachmentRef(label,*world,self,c.patrolPoints[i]);
                if(ImGui::SmallButton("Remove"))remove=i;
                if(i>0){ImGui::SameLine();if(ImGui::SmallButton("Up"))up=i;}
                if(i+1<int(c.patrolPoints.size())){ImGui::SameLine();if(ImGui::SmallButton("Down"))down=i;}
                ImGui::PopID();
            }
            if(remove>=0)c.patrolPoints.erase(c.patrolPoints.begin()+remove);
            else if(up>=0)std::swap(c.patrolPoints[up],c.patrolPoints[up-1]);
            else if(down>=0)std::swap(c.patrolPoints[down],c.patrolPoints[down+1]);
            if(c.patrolPoints.size()<256&&ImGui::Button("Add patrol point"))c.patrolPoints.emplace_back();
            if(ImGui::Button("Reset AI working memory"))AI::ResetBehavior(*world,self);
            ImGui::TextWrapped("Play: inspect Blackboard for TargetEntity, TargetVisible, AIState, PatrolIndex and PatrolGoal. Save AI Behavior in Edit mode to keep configuration.");
        });

    // CharacterControllerComponent - KCC tuning. postDraw shows live ground
    // state + velocity so authors can verify the capsule sits on the floor while
    // tweaking radius/halfHeight from the Inspector.
    RegisterReflectedComponent<CharacterControllerComponent>("Character Controller",
        /*priority*/ 23,
        [](CharacterControllerComponent& cc, World* /*world*/, Entity /*e*/)
        {
            ImGui::Separator();
            ImGui::TextDisabled("Runtime state");
            ImGui::Text("Grounded: %s%s", cc.isGrounded ? "yes" : "no",
                        (cc.isGrounded != cc.wasGrounded) ? " (changed)" : "");
            const float horizSq = cc.velocity.x * cc.velocity.x
                                + cc.velocity.z * cc.velocity.z;
            ImGui::Text("Velocity: %.2f, %.2f, %.2f  (|h|=%.2f)",
                        cc.velocity.x, cc.velocity.y, cc.velocity.z,
                        sqrtf(horizSq));
            ImGui::Text("Time In Air: %.2fs", cc.timeInAir);
            if (cc.groundEntity != NullEntity)
                ImGui::Text("Ground Entity: %u  (normal %.2f, %.2f, %.2f)",
                            cc.groundEntity, cc.groundNormal.x, cc.groundNormal.y, cc.groundNormal.z);
            else
                ImGui::TextDisabled("Airborne — no ground body");
        });

    // PlayerComponent - gameplay-side tuning.
    RegisterReflectedComponent<PlayerComponent>(
        "Player", /*priority*/ 24,
        [](PlayerComponent& pc, World* w, Entity e)
    {
        if (w) Editor::DrawAttachmentRef("Camera Entity (empty = main)",
                                         *w, e, pc.cameraEntity);
    });

    // MeshHandle - primitive picker driven by the descriptor enum. postDraw appends
    // the vertex-channel readout (primitives report their fixed layout; an entity
    // that also carries a MeshLibRef shows that library's real channels).
    RegisterReflectedComponent<MeshHandle>("Mesh", /*priority*/ 100,
        [this](MeshHandle&, World* w, Entity e)
        {
            DrawMeshChannelInfo(m_renderer ? m_renderer->GetMeshLibrary() : nullptr, w, e);
        });

    // PerObjectShadowComponent - opt-in tag for a high-res per-object directional
    // shadow (composited over CSM). Add to a character's root entity.
    RegisterReflectedComponent<PerObjectShadowComponent>("Per-Object Shadow");

    // HairShadowCasterComponent - opt-in tag for the hair (bangs -> face) shadow
    // (screen mark and/or fitted map). Add to the bangs entity (or the parent of
    // the bangs meshes); NPR face pixels (Face.ps SDF marker) receive it in
    // Lighting.ps.
    RegisterReflectedComponent<HairShadowCasterComponent>("Hair Shadow Caster");

    // FaceOverlayComponent - opt-in tag for brows/lashes drawn through the hair
    // (FaceOverlayPass). Add to the eyebrow / eyelash entities; requires the
    // hair material's GBuffer PS to write extra.b = 1 (asset/Shaders/Hair.ps.hlsl).
    RegisterReflectedComponent<FaceOverlayComponent>("Face Overlay (Through Hair)");

    // DissolveComponent - per-entity noise dissolve. Add to any mesh entity (a
    // character root dissolves the whole subtree). Drag Amount for a live
    // preview; runtime animation via Lua Engine.Dissolve / Engine.DissolveIn.
    RegisterReflectedComponent<DissolveComponent>("Dissolve");

    // VisibilityComponent - flag-bits widget (Visible / Cast Shadow / Render In
    // Main Pass) + viewMask + read-only inheritedHidden. Pinned above Local
    // Transform (priority -10) and starts collapsed because it is rarely edited
    // per-entity once authored.
    RegisterReflectedComponent<VisibilityComponent>("Visibility", /*priority*/ -10);
    SetComponentDefaultCollapsed<VisibilityComponent>(true);

    // Entity-as-widget Canvas UI (UI/UICanvas.h) - UIRect layout + hierarchy.
    RegisterReflectedComponent<UI::UICanvas>("Canvas (Root)", /*priority*/ 90,
        [](UI::UICanvas& canvas, World*, Entity e)
        {
            ImGui::Separator();
            // Queued; the UI Editor opens it in ProcessPendingActions.
            if (ImGui::Button("Edit in UI Editor")) Editor::UIEditorWindow::Get().RequestOpen(e);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open this canvas in the UI Editor: a designer view at its own resolution,\n"
                                  "zoom and pan, independent of the game viewport.");
            ImGui::TextDisabled("Runtime scale: %.4f", canvas.computedScale);
            if (canvas.scaleMode == UI::CanvasScaleMode::ConstantPixelSize)
            {
                ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f),
                    "Constant Pixel Size: Reference Resolution and Match are inactive.");
            }
            else
            {
                ImGui::TextDisabled("Reference Resolution and Match drive the Canvas scale.");
            }
        });
    RegisterReflectedComponent<UI::UIRect>("UI Rect", /*priority*/ 91,
        [](UI::UIRect& rect, World* world, Entity e)
        {
            ImGui::Separator();
            // Opens the canvas above this node, with this node selected.
            if (ImGui::Button("Edit Canvas in UI Editor")) Editor::UIEditorWindow::Get().RequestOpen(e);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open the canvas this node belongs to in the UI Editor, with this node selected.");
            ImGui::TextUnformatted("Anchor Presets");
            ImGui::TextDisabled("Presets preserve the current resolved screen rect.");

            auto resolveParent = [world](Entity node) -> Entity
            {
                if (!world) return NullEntity;
                const UI::UIParent* up = world->GetComponent<UI::UIParent>(node);
                if (!up) return NullEntity;
                Entity parent = up->parent;
                if ((parent == NullEntity || !world->IsAlive(parent)) && up->parentGuid.IsValid())
                    parent = ECS::GuidRegistry::Get().Find(up->parentGuid);
                return (parent != NullEntity && world->IsAlive(parent)) ? parent : NullEntity;
            };

            const UI::UIComputedRect* runtimeRect = world
                ? world->GetComponent<UI::UIComputedRect>(e) : nullptr;
            const Entity parent = resolveParent(e);
            const UI::UIComputedRect* parentRuntimeRect =
                (world && parent != NullEntity)
                ? world->GetComponent<UI::UIComputedRect>(parent) : nullptr;

            float canvasScale = 1.f;
            bool  foundCanvas = false;
            if (world)
            {
                Entity cursor = e;
                for (int guard = 0; cursor != NullEntity && guard < 256; ++guard)
                {
                    if (const UI::UICanvas* canvas = world->GetComponent<UI::UICanvas>(cursor))
                    {
                        canvasScale = canvas->computedScale;
                        foundCanvas = true;
                        break;
                    }
                    const Entity next = resolveParent(cursor);
                    if (next == cursor) break;
                    cursor = next;
                }
            }
            if (!std::isfinite(canvasScale) || canvasScale <= 1e-6f)
                canvasScale = 1.f;

            if (runtimeRect && runtimeRect->valid)
            {
                const float width  = runtimeRect->rect.mx.x - runtimeRect->rect.mn.x;
                const float height = runtimeRect->rect.mx.y - runtimeRect->rect.mn.y;
                ImGui::TextDisabled("Runtime rect: (%.1f, %.1f)  %.1f x %.1f%s",
                    runtimeRect->rect.mn.x, runtimeRect->rect.mn.y, width, height,
                    runtimeRect->culled ? "  [culled]" : "");
            }
            else
            {
                ImGui::TextDisabled("Runtime rect: unavailable (not reached by an active Canvas)");
            }
            ImGui::TextDisabled(foundCanvas ? "Canvas scale: %.4f" : "Canvas scale: %.4f (fallback)",
                                canvasScale);

            const bool invertedAnchors = rect.anchorMin.x > rect.anchorMax.x
                                      || rect.anchorMin.y > rect.anchorMax.y;
            if (invertedAnchors)
            {
                ImGui::TextColored(ImVec4(1.f, 0.42f, 0.25f, 1.f),
                    "Warning: Anchor Min exceeds Anchor Max; the anchor span is inverted.");
            }

            bool negativeResolvedRect = false;
            if (runtimeRect && runtimeRect->valid)
            {
                negativeResolvedRect = runtimeRect->rect.mx.x < runtimeRect->rect.mn.x
                                    || runtimeRect->rect.mx.y < runtimeRect->rect.mn.y;
            }
            if (!negativeResolvedRect && parentRuntimeRect && parentRuntimeRect->valid)
            {
                const float parentW = parentRuntimeRect->rect.mx.x - parentRuntimeRect->rect.mn.x;
                const float parentH = parentRuntimeRect->rect.mx.y - parentRuntimeRect->rect.mn.y;
                const float resolvedW = (rect.anchorMax.x - rect.anchorMin.x) * parentW
                                      + rect.size.x * canvasScale;
                const float resolvedH = (rect.anchorMax.y - rect.anchorMin.y) * parentH
                                      + rect.size.y * canvasScale;
                negativeResolvedRect = resolvedW < 0.f || resolvedH < 0.f;
            }
            if (negativeResolvedRect)
            {
                ImGui::TextColored(ImVec4(1.f, 0.42f, 0.25f, 1.f),
                    "Warning: the resolved UI rect has negative width or height.");
            }

            const bool canPreserve = runtimeRect && runtimeRect->valid
                                  && parentRuntimeRect && parentRuntimeRect->valid
                                  && !negativeResolvedRect;
            auto applyPreset = [&](UI::Vec2 newAnchorMin, UI::Vec2 newAnchorMax)
            {
                if (!canPreserve) return;

                const UI::Rect visual = runtimeRect->rect;
                const UI::Rect parentRect = parentRuntimeRect->rect;
                const UI::Vec2 parentSize{
                    parentRect.mx.x - parentRect.mn.x,
                    parentRect.mx.y - parentRect.mn.y
                };
                const UI::Vec2 visualSize{
                    visual.mx.x - visual.mn.x,
                    visual.mx.y - visual.mn.y
                };
                const UI::Vec2 anchorLo{
                    parentRect.mn.x + newAnchorMin.x * parentSize.x,
                    parentRect.mn.y + newAnchorMin.y * parentSize.y
                };
                const UI::Vec2 anchorHi{
                    parentRect.mn.x + newAnchorMax.x * parentSize.x,
                    parentRect.mn.y + newAnchorMax.y * parentSize.y
                };
                const UI::Vec2 span{ anchorHi.x - anchorLo.x, anchorHi.y - anchorLo.y };
                const UI::Vec2 pivot{
                    std::clamp(rect.pivot.x, 0.f, 1.f),
                    std::clamp(rect.pivot.y, 0.f, 1.f)
                };
                const UI::Vec2 visualPivot{
                    visual.mn.x + pivot.x * visualSize.x,
                    visual.mn.y + pivot.y * visualSize.y
                };
                const UI::Vec2 anchorReference{
                    anchorLo.x + pivot.x * span.x,
                    anchorLo.y + pivot.y * span.y
                };
                const float invScale = 1.f / canvasScale;

                rect.anchorMin = newAnchorMin;
                rect.anchorMax = newAnchorMax;
                rect.size = {
                    (visualSize.x - span.x) * invScale,
                    (visualSize.y - span.y) * invScale
                };
                rect.offset = {
                    (visualPivot.x - anchorReference.x) * invScale,
                    (visualPivot.y - anchorReference.y) * invScale
                };
            };

            auto presetButton = [&](const char* label, const char* tooltip,
                                    UI::Vec2 anchorMin, UI::Vec2 anchorMax)
            {
                if (ImGui::SmallButton(label)) applyPreset(anchorMin, anchorMax);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            };

            ImGui::PushID("UIRectAnchorPresets");
            if (!canPreserve) ImGui::BeginDisabled();

            ImGui::TextDisabled("Point");
            presetButton("TL##Point", "Point anchor: top-left",     { 0.f, 0.f }, { 0.f, 0.f }); ImGui::SameLine();
            presetButton("T##Point",  "Point anchor: top-center",   { .5f, 0.f }, { .5f, 0.f }); ImGui::SameLine();
            presetButton("TR##Point", "Point anchor: top-right",    { 1.f, 0.f }, { 1.f, 0.f });
            presetButton("L##Point",  "Point anchor: middle-left",  { 0.f, .5f }, { 0.f, .5f }); ImGui::SameLine();
            presetButton("C##Point",  "Point anchor: center",       { .5f, .5f }, { .5f, .5f }); ImGui::SameLine();
            presetButton("R##Point",  "Point anchor: middle-right", { 1.f, .5f }, { 1.f, .5f });
            presetButton("BL##Point", "Point anchor: bottom-left",  { 0.f, 1.f }, { 0.f, 1.f }); ImGui::SameLine();
            presetButton("B##Point",  "Point anchor: bottom-center",{ .5f, 1.f }, { .5f, 1.f }); ImGui::SameLine();
            presetButton("BR##Point", "Point anchor: bottom-right", { 1.f, 1.f }, { 1.f, 1.f });

            ImGui::TextDisabled("Stretch");
            presetButton("H Top##Stretch",    "Horizontal stretch at top",    { 0.f, 0.f },  { 1.f, 0.f }); ImGui::SameLine();
            presetButton("H Mid##Stretch",    "Horizontal stretch at center", { 0.f, .5f }, { 1.f, .5f }); ImGui::SameLine();
            presetButton("H Bottom##Stretch", "Horizontal stretch at bottom", { 0.f, 1.f },  { 1.f, 1.f });
            presetButton("V Left##Stretch",   "Vertical stretch at left",      { 0.f, 0.f },  { 0.f, 1.f }); ImGui::SameLine();
            presetButton("V Mid##Stretch",    "Vertical stretch at center",    { .5f, 0.f }, { .5f, 1.f }); ImGui::SameLine();
            presetButton("V Right##Stretch",  "Vertical stretch at right",     { 1.f, 0.f },  { 1.f, 1.f });
            presetButton("Full##Stretch",     "Stretch to the full parent rect",{ 0.f, 0.f }, { 1.f, 1.f });

            if (!canPreserve) ImGui::EndDisabled();
            ImGui::PopID();
            if (!canPreserve)
                ImGui::TextDisabled("Presets need valid child and parent runtime rects.");
        });
    RegisterReflectedComponent<UI::UIClipRect>("UI Clip Rect", /*priority*/ 92);
    RegisterReflectedComponent<UI::UIInteractable>("UI Interactable", /*priority*/ 93);
    RegisterReflectedComponent<UI::UIText>  ("Canvas Text",   /*priority*/ 94);
    RegisterReflectedComponent<UI::UIImage>("Canvas Image", /*priority*/ 95,
        [](UI::UIImage& img, World* world, Entity e)
        {
            // A Sprite Animation rewrites UV 0 / UV 1 every frame (in Edit mode too) -
            // an edit of them does not stick.
            if (world && world->HasComponent<UI::UISpriteAnimComponent>(e))
                ImGui::TextColored(ImVec4(1.f, 0.72f, 0.25f, 1.f),
                                   "UV 0 / UV 1 are driven by the Sprite Animation.");
            if (img.srvGpuHandle != 0)
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(img.srvGpuHandle)),
                             ImVec2(64.f, 64.f));
            else if (!img.texturePath.empty())
                ImGui::TextDisabled("(loading \"%s\" ...)", img.texturePath.c_str());
            else
                ImGui::TextDisabled("(no texture — solid colour fill)");
        });
    // Sprite-sheet animation - drives the sibling Canvas Image's uv each frame.
    RegisterReflectedComponent<UI::UISpriteAnimComponent>("Sprite Animation", /*priority*/ 96,
        [](UI::UISpriteAnimComponent& a, World*, Entity)
        {
            const int total = a.frameCount > 0 ? a.frameCount
                                               : std::max(1, a.columns) * std::max(1, a.rows);
            ImGui::TextDisabled("Frame %d / %d", a.frame + 1, total);
            if (ImGui::SmallButton("Restart")) { a.elapsed = 0.f; a.frame = 0; }
            ImGui::SameLine();
            if (ImGui::SmallButton(a.playing ? "Pause" : "Play")) a.playing = !a.playing;
        });
    // A .iui the scene references. The instance is runtime - read-only here,
    // edited in its own UI Editor tab - and follows the file (a save, the
    // hot-reload watcher) and the path.
    RegisterReflectedComponent<UI::UIDocument>("UI Document", /*priority*/ 97,
        [](UI::UIDocument& d, World* world, Entity e)
        {
            if (!world) return;
            if (!d.error.empty())
                ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f), "%s", d.error.c_str());
            else if (d.root != NullEntity && world->IsAlive(d.root))
                ImGui::TextDisabled("Instance: %zu node(s), read-only - edit the asset itself.",
                                    UI::DocumentNodeCount(*world, e));
            else if (!d.path.empty())
                ImGui::TextDisabled("(the instance is made on the next frame)");
            ImGui::BeginDisabled(d.path.empty());
            if (ImGui::SmallButton("Edit in UI Editor"))
                Editor::UIEditorWindow::Get().RequestOpenAsset(d.path);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Open the .iui in a UI Editor tab of its own: its Save updates every scene "
                                  "that references it.");
        });
    // Trivially copyable and carrying runtime state (the computed scale; hover /
    // press / focus; the playhead): the raw-byte fingerprint would take a click in
    // the Inspector for an edit - a bogus undo entry and a dirty scene. The
    // serializer blob holds only what is authored.
    UseSerializerFingerprint<UI::UICanvas>();
    UseSerializerFingerprint<UI::UIInteractable>();
    UseSerializerFingerprint<UI::UISpriteAnimComponent>();
    // (The instance's root, asset and error are runtime - only the path is authored.)
    UseSerializerFingerprint<UI::UIDocument>();

    // World-space UI - billboarded 3D quads via WorldSpaceUISystem + WorldUIBillboardPass; pair marker + content component.
    RegisterReflectedComponent<UI::WorldSpaceUIComponent>("World-Space UI",     /*priority*/ 101);
    RegisterReflectedComponent<UI::WorldUIBarComponent>  ("World UI Bar",       /*priority*/ 102);
    RegisterReflectedComponent<UI::WorldUITextComponent> ("World UI Text",      /*priority*/ 103);
    RegisterReflectedComponent<UI::WorldUIImageComponent>("World UI Image",     /*priority*/ 104);
    RegisterReflectedComponent<UI::DamageNumberComponent>("Damage Number",      /*priority*/ 105);

    // World Environment - per-scene sky-mode singleton (None / Flat Color /
    // Static Skybox / Procedural Atmosphere). Presence marks the scene as
    // authored under the optional-sky model.
    RegisterReflectedComponent<WorldEnvironmentComponent>("World Environment", /*priority*/ 99);

    // Skybox / IBL - the descriptor handles paths/knobs; runtime GPU handles in postDraw.
    RegisterReflectedComponent<SkyboxComponent>("Skybox / IBL",
        /*priority*/ 100,
        [](SkyboxComponent& sc, World*, Entity)
        {
            ImGui::TextDisabled("(Atmosphere knobs live on AtmosphereComponent.)");
            ImGui::TextDisabled("Irradiance GPU: %llu", sc.irradianceGpuHandle);
            ImGui::TextDisabled("Radiance GPU:   %llu", sc.radianceGpuHandle);
            ImGui::TextDisabled("Skybox GPU:     %llu", sc.skyboxGpuHandle);
        });

    // Atmosphere - all procedural sky knobs.
    RegisterReflectedComponent<AtmosphereComponent>("Atmosphere",     /*priority*/ 101);
    // Time-of-Day - singleton: TODConfig drives the cycle, TODOutput shows computed state.
    RegisterReflectedComponent<TODConfigComponent>  ("Time-of-Day",   /*priority*/ 102);
    RegisterReflectedComponent<TODOutputComponent>  ("TOD Output",    /*priority*/ 103);
    // Per-light TOD switch - street lamps, neon, windows. Works in Edit mode.
    RegisterReflectedComponent<TODLightComponent>   ("TOD Light",     /*priority*/ 105);
    // Sun / Moon tags - attach to a directional LightData entity to put it under TOD control.
    RegisterReflectedComponent<SunLightTag>         ("Sun Light Tag", /*priority*/ 104);
    RegisterReflectedComponent<MoonLightTag>        ("Moon Light Tag",/*priority*/ 105);
    // Volumetric clouds - author tunables on the Sky entity; TOD drives sun direction/color.
    RegisterReflectedComponent<CloudComponent>      ("Volumetric Clouds", /*priority*/ 106);
    // Exponential height fog - analytic UE-style fog on the Sky entity.
    RegisterReflectedComponent<HeightFogComponent>  ("Height Fog",        /*priority*/ 106);
    // Volumetric fog - the scene's global froxel medium. Its presence makes the
    // scene own its fog (settings.json / .ippc fog values are then ignored).
    RegisterReflectedComponent<VolumetricFogComponent>("Volumetric Fog",   /*priority*/ 106,
        [this](VolumetricFogComponent&, World*, Entity) { DrawVolumetricFogStatus(); });
    // Local fog volume - bounded medium; bounds follow the Transform.
    RegisterReflectedComponent<LocalFogVolumeComponent>("Local Fog Volume", /*priority*/ 106);
    // Procedural grass field (GoT-style GPU blades) - anchors to the scene's TerrainComponent heightmap.
    RegisterReflectedComponent<GrassComponent>      ("Grass Field",       /*priority*/ 107);
    // Water surface - Fresnel sky reflection + flow normals; worldCenter.y is the water level.
    RegisterReflectedComponent<WaterComponent>      ("Water Surface",     /*priority*/ 108);
    RegisterReflectedComponent<WaterInteractionComponent>("Water Ripples",  /*priority*/ 109);

    // Terrain - heightmap + up to kMaxTerrainLayersUI PBR layers; path edits trigger the TextureSystem
    // rebind in BuildScene_SyncTerrain. Numeric fields feed the per-frame TerrainParams, so changes
    // show next frame (no reimport).
    RegisterReflectedComponent<TerrainComponent>("Terrain",
        /*priority*/ 100,
        [tx = m_textureSys, gfx = m_gfx](TerrainComponent& tcRef, World*, Entity)
        {
            auto* tc = &tcRef;

            // Per-instance text input buffers, keyed by component address: avoid InputText truncation and a
            // per-frame strncpy. One entry per layer; slots per layer = albedo/normal/arm/disp.
            struct PathBufs
            {
                std::array<char, 512>                                heightmap;
                std::array<char, 512>                                splatmap;
                std::vector<std::array<std::array<char, 512>, 4>>    layers; // [layer][slot]
            };
            static std::unordered_map<void*, PathBufs> s_bufs;
            auto& bufs = s_bufs[tc];
            // GPU cap on rendered layers - mirrors Renderer::kMaxTerrainLayers.
            constexpr size_t kMaxTerrainLayersUI = 8;

            auto syncBuf = [](std::array<char, 512>& buf, const std::string& src)
            {
                strncpy_s(buf.data(), buf.size(), src.c_str(), _TRUNCATE);
            };

            // Resolve TextureHandle to SRV gpu handle for ImGui::Image; returns 0 when unbound (placeholder drawn).
            auto resolveSrv = [tx, gfx](Resource::TextureHandle h) -> uint64_t
            {
                if (!tx || !gfx) return 0;
                if (h == Resource::kInvalidTextureHandle) return 0;
                if (!tx->IsReady(h)) return 0;
                const RHI::Texture* tex = tx->GetTexture(h);
                if (!tex || !tex->IsValid()) return 0;
                return gfx->GetTexturePreviewSrvGpuHandle(*tex);   // sRGB: the UNORM alias
            };

            // Thumbnail + path-input row (mirrors decal-material inspector for consistent ITEX drop behaviour).
            auto pathRow = [&resolveSrv](const char* label,
                                         std::array<char, 512>& buf,
                                         std::string& targetPath,
                                         Resource::TextureHandle texHandle,
                                         float previewSize) -> bool
            {
                ImGui::PushID(label);
                bool changed = false;

                const uint64_t gpu = resolveSrv(texHandle);
                if (gpu != 0)
                {
                    ImGui::Image(ImTextureRef(static_cast<ImTextureID>(gpu)),
                                 ImVec2(previewSize, previewSize));
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.18f, 0.18f, 0.18f, 1.f));
                    ImGui::Button("##prev", ImVec2(previewSize, previewSize));
                    ImGui::PopStyleColor();
                }

                // Drag-drop accepts ITEX onto the thumbnail too.
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    {
                        targetPath = static_cast<const char*>(p->Data);
                        strncpy_s(buf.data(), buf.size(), targetPath.c_str(), _TRUNCATE);
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }

                ImGui::SameLine();
                ImGui::BeginGroup();
                ImGui::TextUnformatted(label);

                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("##path", buf.data(), buf.size()))
                {
                    targetPath = buf.data();
                    changed = true;
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    {
                        targetPath = static_cast<const char*>(p->Data);
                        strncpy_s(buf.data(), buf.size(), targetPath.c_str(), _TRUNCATE);
                        changed = true;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndGroup();

                ImGui::PopID();
                return changed;
            };

            // ---- Heightmap + splatmap ----
            syncBuf(bufs.heightmap, tc->heightmapPath);
            pathRow("Heightmap", bufs.heightmap, tc->heightmapPath,
                    tc->heightmapHandle, 48.0f);

            syncBuf(bufs.splatmap, tc->splatmapPath);
            pathRow("Splatmap (optional)", bufs.splatmap, tc->splatmapPath,
                    tc->splatmapHandle, 48.0f);

            // ---- Tile placement ----
            ImGui::Separator();
            ImGui::TextUnformatted("Placement");
            ImGui::DragFloat3("World Center",
                reinterpret_cast<float*>(&tc->worldCenter), 1.0f);
            ImGui::DragFloat("World Size", &tc->worldSize, 8.0f, 16.0f, 65536.0f, "%.0f m");
            ImGui::DragFloat("Height Scale", &tc->heightScale, 1.0f, 1.0f, 65536.0f, "%.0f m");
            ImGui::TextDisabled("Relief floor->peak. Valley floor stays pinned at");
            ImGui::TextDisabled("World Center Y while you scale (data-range re-anchored).");

            // tilesPerSide as an int slider clamped to a sane range; the
            // mesh-shader dispatch count is N^2, so the upper bound matters.
            int tps = static_cast<int>(tc->tilesPerSide);
            if (EditorUI::DragInt("Tiles Per Side", &tps, 1, 256,
                                 "%d  (~%.1f m / quad)"))
                tc->tilesPerSide = static_cast<uint32_t>(tps);
            ImGui::TextDisabled("≈ %.2f m per quad", tc->worldSize / float(std::max(tps, 1) * 11));

            // ---- Height-correlated blend section ----
            ImGui::Separator();
            ImGui::Checkbox("Height Blend (disp)", &tc->heightBlendEnabled);
            ImGui::TextDisabled("Bias layer transitions by each layer's Disp map");
            ImGui::TextDisabled("so they follow the micro-relief. Off = plain lerp.");
            if (tc->heightBlendEnabled)
            {
                EditorUI::DragFloat("Blend Strength", &tc->heightBlendStrength,
                    0.0f, 1.0f, "%.3f");
                EditorUI::DragFloat("Blend Range", &tc->heightBlendRange,
                    0.001f, 0.5f, "%.3f");
                ImGui::TextDisabled("Strength: how hard disp biases. Range: smaller = sharper interlock.");
            }

            // ---- Auto-blend section ----
            ImGui::Separator();
            ImGui::TextUnformatted("Auto-Blend (no splatmap)");
            ImGui::TextDisabled("Each layer is gated by a world-meter height range");
            ImGui::TextDisabled("AND a degree slope range — both must hold.");

            // ---- Layers ----
            // baseY/topY only label the tile's Y range for the Height Range drag; nothing clamps to it.
            const float baseY = tc->worldCenter.y;
            const float topY  = tc->worldCenter.y + tc->heightScale;

            // Keep the per-instance text buffers tracking the layer count.
            if (bufs.layers.size() != tc->layers.size())
                bufs.layers.resize(tc->layers.size());

            // Add / remove layers (data-driven count; capped at the GPU max).
            if (tc->layers.size() < kMaxTerrainLayersUI)
            {
                if (ImGui::Button("+ Add Layer"))
                {
                    tc->layers.emplace_back();
                    bufs.layers.emplace_back();
                }
            }
            else
            {
                ImGui::TextDisabled("Max %zu layers (GPU cap)", kMaxTerrainLayersUI);
            }
            ImGui::TextDisabled("Layers: %zu  (splatmap uses the first 4 channels)", tc->layers.size());

            int removeIdx = -1;
            for (size_t i = 0; i < tc->layers.size(); ++i)
            {
                auto& l = tc->layers[i];
                ImGui::PushID(static_cast<int>(i));
                char hdr[32];
                snprintf(hdr, sizeof(hdr), "Layer %zu", i);
                if (ImGui::CollapsingHeader(hdr,
                        i == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0))
                {
                    constexpr float kLayerPreview = 32.0f;
                    syncBuf(bufs.layers[i][0], l.albedoPath);
                    pathRow("Albedo",               bufs.layers[i][0],
                            l.albedoPath, l.albedoHandle, kLayerPreview);
                    syncBuf(bufs.layers[i][1], l.normalPath);
                    pathRow("Normal",               bufs.layers[i][1],
                            l.normalPath, l.normalHandle, kLayerPreview);
                    syncBuf(bufs.layers[i][2], l.armPath);
                    pathRow("ARM (AO/Rough/Metal)", bufs.layers[i][2],
                            l.armPath,    l.armHandle,    kLayerPreview);
                    syncBuf(bufs.layers[i][3], l.dispPath);
                    pathRow("Disp (height-blend)",  bufs.layers[i][3],
                            l.dispPath,   l.dispHandle,   kLayerPreview);

                    // Tiling density (rep/m): 0.5 ~ 2 m cycle, 0.05 ~ 20 m cycle.
                    EditorUI::DragFloat("Tiling", &l.tilingScale,
                        0.001f, 2.0f, "%.3f rep/m");
                    if (l.tilingScale > 1e-4f)
                        ImGui::TextDisabled("≈ 1 cycle / %.2f m", 1.0f / l.tilingScale);

                    ImGui::Spacing();
                    ImGui::TextDisabled("Tile Y range: %.0f m … %.0f m", baseY, topY);

                    // Height range (world m); DragFloatRange2 enforces min <= max.
                    ImGui::DragFloatRange2("Height Range",
                        &l.minHeight, &l.maxHeight,
                        1.0f, -1e6f, 1e6f,
                        "min %.0f m", "max %.0f m");
                    EditorUI::DragFloat("Height Fade", &l.fadeHeight,
                        0.1f, 200.0f, "%.1f m");

                    // Slope range (degrees, 0=flat, 90=cliff).
                    ImGui::DragFloatRange2("Slope Range",
                        &l.minSlopeDeg, &l.maxSlopeDeg,
                        0.5f, 0.0f, 90.0f,
                        "min %.1f\xc2\xb0", "max %.1f\xc2\xb0");
                    EditorUI::DragFloat("Slope Fade", &l.fadeSlopeDeg,
                        0.1f, 45.0f, "%.1f\xc2\xb0");

                    ImGui::TextDisabled("Bindless: albedo=%d nor=%d arm=%d disp=%d",
                        l.albedoBindlessIdx, l.normalBindlessIdx, l.armBindlessIdx,
                        l.dispBindlessIdx);

                    // Keep at least one layer; defer the erase past the loop.
                    if (tc->layers.size() > 1 && ImGui::SmallButton("Remove Layer"))
                        removeIdx = static_cast<int>(i);
                }
                ImGui::PopID();
            }
            if (removeIdx >= 0 && static_cast<size_t>(removeIdx) < tc->layers.size())
            {
                tc->layers.erase(tc->layers.begin() + removeIdx);
                if (static_cast<size_t>(removeIdx) < bufs.layers.size())
                    bufs.layers.erase(bufs.layers.begin() + removeIdx);
            }
        });

    // LightData descriptor handles type/radius/cone/color (aim is the transform's +Z, not a field);
    // postDraw shows the live derived direction + atlas-slot hint.
    RegisterReflectedComponent<LightData>("Light",
        /*priority*/ 100,
        [renderer](LightData& ld, World* w, Entity e)
        {
            // Live derived aim - resolved the same way the renderer does, so
            // this doubles as a check that the transform says what you expect.
            if (ld.type == LightType::Directional || ld.type == LightType::Spot)
            {
                const GlobalTransform* gt = w ? w->GetComponent<GlobalTransform>(e) : nullptr;
                if (gt)
                {
                    const DirectX::XMFLOAT3 d = LightUtil::DirectionFromWorldF(gt->matrix);
                    ImGui::TextDisabled("dir  %.3f  %.3f  %.3f  (entity +Z)", d.x, d.y, d.z);
                }
                else
                {
                    ImGui::TextDisabled("dir  -- (no transform on this entity)");
                }
            }
            // Budget line: the dropped count answers "why doesn't this lamp cast a
            // shadow?" - slices go to the first N qualifying lights in renderer order,
            // and every light past N keeps lighting the scene while casting nothing.
            // Shown on the light itself, where people look.
            if (ld.enabled && ld.type == LightType::Spot && ld.castsShadow)
            {
                ImGui::TextDisabled("(uses 1 of %u spot atlas slices)",
                                    SpotShadowPass::kMaxCasters);
                if (renderer && renderer->GetSpotShadowCastersDropped() > 0)
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f),
                        "%u shadow-casting spot light(s) got NO slice this frame "
                        "(occlusion-only fallback)",
                        renderer->GetSpotShadowCastersDropped());
            }
            if (ld.enabled && ld.type == LightType::Point && ld.castsShadow)
            {
                ImGui::TextDisabled("(uses 1 of %u cube shadow casters)",
                                    PointShadowPass::kMaxCasters);
                if (renderer && renderer->GetPointShadowCastersDropped() > 0)
                    ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f),
                        "%u shadow-casting point light(s) got NO cube this frame "
                        "(occlusion-only fallback)",
                        renderer->GetPointShadowCastersDropped());
            }
        });

    // SkeletonComponent - read-only meta + bone hierarchy via a custom postDraw.
    RegisterReflectedComponent<SkeletonComponent>("Skeleton",
        /*priority*/ 20,
        [renderer](SkeletonComponent& scRef, World* w, Entity e)
        {
            auto* sc = &scRef;
            if (sc->assetIndex == kInvalidSkeletonIndex)
            {
                ImGui::TextDisabled("(no skeleton asset assigned)");
                return;
            }
            ImGui::TextDisabled("Asset index : %u", sc->assetIndex);
            ImGui::TextDisabled("Bone count  : %u", sc->boneCount);
            // The model this skeleton was spawned from (the SceneSourcePath of the
            // .iscn root: this entity or an ancestor).
            if (w && w->IsAlive(e))
            {
                std::string model;
                Entity cur = e;
                for (int hops = 0; cur != NullEntity && w->IsAlive(cur) && hops < 16 && model.empty(); ++hops)
                {
                    if (const SceneSourcePath* src = w->GetComponent<SceneSourcePath>(cur)) model = src->path;
                    const Parent* p = w->GetComponent<Parent>(cur);
                    cur = p ? p->entity : NullEntity;
                }
                if (!model.empty() && ImGui::Button("Open in Skeleton Editor"))
                    Editor::SkeletonEditorWindow::Get().RequestOpen(Sys::RelativizeAssetPath(model));
                if (!model.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", model.c_str());
            }

            if (!renderer) return;
            const SkeletonRegistry& reg = renderer->GetSkeletonRegistry();
            if (sc->assetIndex >= reg.Count()) return;
            const SkeletonAsset& skel = reg.Get(sc->assetIndex);

            if (ImGui::TreeNode("Bone Hierarchy"))
            {
                std::vector<std::vector<uint32_t>> children(skel.boneCount);
                std::vector<uint32_t> roots;
                for (uint32_t i = 0; i < skel.boneCount; ++i)
                {
                    if (skel.parentIndex[i] < 0) roots.push_back(i);
                    else children[static_cast<uint32_t>(skel.parentIndex[i])].push_back(i);
                }

                std::function<void(uint32_t)> drawBone = [&](uint32_t b) {
                    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth;
                    if (children[b].empty()) f |= ImGuiTreeNodeFlags_Leaf;
                    const char* nm = skel.boneNames[b][0] ? skel.boneNames[b] : "(unnamed)";
                    ImGui::PushID(static_cast<int>(b));
                    if (ImGui::TreeNodeEx(nm, f))
                    {
                        for (uint32_t ch : children[b]) drawBone(ch);
                        ImGui::TreePop();
                    }
                    ImGui::PopID();
                };
                for (uint32_t r : roots) drawBone(r);
                ImGui::TreePop();
            }
        });

    RegisterComponentEditor<IKRigComponent>("IK Rig",
        [this](void* ptr,World* world,Entity entity){DrawIKRigInspector(ptr,world,entity);},31);

    // Authored animation references. Runtime clip indices and blend clocks stay
    // in AnimationComponent; this component is the scene/prefab-facing library.
    // Compact row: summary + high-frequency fields only. The full clip-list
    // authoring UI is the "Animation Set Editor" window
    // (AnimationSetEditorWindow.cpp), opened by the Edit button below.
    RegisterComponentEditor<AnimationSetComponent>("Animation Set",
        [this](void* ptr, World* world, Entity entity)
        {
            auto& set = *static_cast<AnimationSetComponent*>(ptr);

            ImGui::Text("%d clip(s)", static_cast<int>(set.clips.size()));
            ImGui::Checkbox("Play Default On Load", &set.playDefaultOnLoad);
            if (set.clips.empty())
            {
                ImGui::TextDisabled("No clips. Open the editor (or drop a .ianim on the entity).");
            }
            else if (ImGui::BeginCombo("Default Clip",
                                      set.defaultClip.empty() ? "(none)" : set.defaultClip.c_str()))
            {
                if (ImGui::Selectable("(none)", set.defaultClip.empty()))
                    set.defaultClip.clear();
                for (const auto& ref : set.clips)
                {
                    if (ref.name.empty()) continue;
                    const bool selected = (set.defaultClip == ref.name);
                    if (ImGui::Selectable(ref.name.c_str(), selected))
                        set.defaultClip = ref.name;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Edit Animation Set..."))
            {
                m_showAnimSetEditor = true;
                if (world) m_animSetEditTarget = world->MakeHandle(entity);
                m_animSetSelClip = set.clips.empty() ? -1 : 0;
            }
        },
        /*priority*/ 29,
        /*requires_*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // Runtime Animator state. It is intentionally transient; authored paths live
    // in AnimationSetComponent and Lua drives it by clip name. Compact row: the
    // raw ClipLibrary-index debug combos are in the Animation Set editor
    // window's "Debug (raw clip indices)" section.
    RegisterReflectedComponent<AnimationComponent>("Animator (Runtime)",
        /*priority*/ 30,
        [this](AnimationComponent& anim, World* world, Entity entity)
        {
            if (ImGui::Button(anim.paused ? "Resume Animator" : "Pause Animator"))
                anim.paused = !anim.paused;
            ImGui::SameLine();
            ImGui::BeginDisabled(!world || !m_animClipSys);
            if (ImGui::Button("Stop"))
                m_animClipSys->StopAnimator(*world, entity);
            ImGui::EndDisabled();

            if (world)
            {
                if (const auto* transition = world->GetComponent<AnimatorTransitionComponent>(entity))
                {
                    const char* current = transition->currentClipName.empty()
                                        ? "(unnamed)" : transition->currentClipName.c_str();
                    ImGui::TextDisabled("Current: %s", current);
                    if (transition->active)
                        ImGui::TextDisabled("Blending to %s: %.0f%%",
                            transition->targetClipName.c_str(), anim.blendWeight * 100.f);
                }
            }
            ImGui::TextDisabled("Raw clip debug lives in the Animation Set editor.");
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // CameraMotionComponent - plays a camera .ianim (e.g. an MMD camera VMD) on
    // a dedicated VCam. Reflected: speed/translation scale/looping/playOnLoad.
    // postDraw owns the .ianim path field + transport buttons (flags only; the
    // system drives the VCam push/pop). Drives the camera when the sim is
    // advancing (PIE / Game).
    RegisterReflectedComponent<CameraMotionComponent>("Camera Motion",
        /*priority*/ 31,
        [](CameraMotionComponent& cm, World* /*world*/, Entity /*entity*/)
        {
            char buf[260];
            strncpy_s(buf, sizeof(buf), cm.motionPath.c_str(), _TRUNCATE);
            if (ImGui::InputText("Motion (.ianim)", buf, sizeof(buf)))
            {
                cm.motionPath      = buf;
                cm.handleRequested = false;   // re-acquire on path change
            }
            // Drag a .ianim from the Asset panel onto the field above.
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                {
                    cm.motionPath      = static_cast<const char*>(payload->Data);
                    cm.handleRequested = false;   // re-acquire the dropped clip
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::TextDisabled("(or drag a .ianim from the Asset panel onto the field)");
            ImGui::TextDisabled("Translation Scale converts clip units to world units (e.g. 0.01 for cm -> m).");
            ImGui::InputInt("VCam Priority", &cm.priority);

            ImGui::Separator();
            if (cm.playing)
            {
                if (ImGui::Button(cm.paused ? "Resume" : "Pause"))
                    cm.paused = !cm.paused;
                ImGui::SameLine();
                if (ImGui::Button("Stop"))
                    cm.stopRequested = true;
            }
            else
            {
                if (ImGui::Button("Play"))
                {
                    cm.playing   = true;
                    cm.started   = true;
                    cm.paused    = false;
                    cm.time      = 0.f;
                    cm.prevTime  = -1.f;
                }
            }
            ImGui::SameLine();
            ImGui::Text("t = %.2f s", cm.time);

            if (cm.motionPath.empty())
                ImGui::TextDisabled("Import a camera VMD, then point this at the .ianim.");
            else
                ImGui::TextDisabled("Plays when the sim advances (enter Play / Game).");
        });

    // MotionDirectorComponent - master clock that syncs a camera-motion file
    // with a separate skinned-motion file. Reflected: speed/duration/loop/
    // playOnLoad. postDraw owns transport + the per-file track list.
    RegisterReflectedComponent<MotionDirectorComponent>("Motion Director",
        /*priority*/ 32,
        [](MotionDirectorComponent& d, World* world, Entity entity)
        {
            ImGui::Separator();
            if (d.playing)
            {
                if (ImGui::Button(d.paused ? "Resume" : "Pause"))
                    d.paused = !d.paused;
                ImGui::SameLine();
                if (ImGui::Button("Stop")) d.stopRequested = true;
            }
            else if (ImGui::Button("Play"))
            {
                d.playing = true; d.started = true; d.paused = false; d.masterTime = 0.f;
            }
            ImGui::SameLine();
            ImGui::Text("t = %.2f s", d.masterTime);

            ImGui::Separator();
            ImGui::TextDisabled("Tracks (%d) — one per file/entity to sync",
                                static_cast<int>(d.tracks.size()));

            int removeIdx = -1;
            for (int i = 0; i < static_cast<int>(d.tracks.size()); ++i)
            {
                MotionDirectorTrack& tr = d.tracks[i];
                ImGui::PushID(i);
                const char* kinds[] = { "Skinned", "Camera", "Audio" };
                int ti = static_cast<int>(tr.type);
                if (ti < 0 || ti > 2) ti = 0;
                if (ImGui::Combo("Type", &ti, kinds, 3))
                    tr.type = static_cast<uint8_t>(ti);
                if (world) Editor::DrawAttachmentRef("Target", *world, entity, tr.target);
                ImGui::DragFloat("Time Offset", &tr.timeOffset, 0.01f, 0.f, 600.f, "%.2f s");
                // speedMul is ignored for Audio cues (audio can't time-scale here).
                if (tr.type != MotionDirectorTrack::Audio)
                    ImGui::DragFloat("Speed x", &tr.speedMul, 0.01f, -4.f, 4.f, "%.2f");
                if (ImGui::Button("Remove Track")) removeIdx = i;
                ImGui::Separator();
                ImGui::PopID();
            }
            if (removeIdx >= 0)
                d.tracks.erase(d.tracks.begin() + removeIdx);
            if (ImGui::Button("Add Track"))
                d.tracks.emplace_back();
        });

    // SequencePlayerComponent - cinematic sequence playback. Reflected fields
    // (asset path + playback options) come from the reflection pass; postDraw
    // owns the transport and the cast.
    //
    // The transport writes request flags only. SequencerTickSystem consumes
    // them, which keeps one code path for the inspector, Lua and a parent
    // sequence, and keeps the playhead advancing in exactly one place.
    RegisterReflectedComponent<SequencePlayerComponent>("Sequence Player",
        /*priority*/ 34,
        [](SequencePlayerComponent& p, World* world, Entity entity)
        {
            ImGui::Separator();

            // Frame count comes from the asset, not the component: the scrubber
            // has to span the authored range, and the asset is the only thing
            // that knows it. Acquire is cached, so this is a map lookup.
            std::shared_ptr<const Seq::Sequence> seq =
                p.sequencePath.empty() ? nullptr : Seq::AcquireSequence(p.sequencePath);
            const int   lastFrame = seq ? std::max(0, seq->durationFrames) : 0;
            const float fps       = (seq && seq->frameRate > 0.f) ? seq->frameRate : 30.f;

            if (p.playing)
            {
                if (ImGui::Button(p.paused ? "Resume" : "Pause")) p.paused = !p.paused;
                ImGui::SameLine();
                if (ImGui::Button("Stop")) p.stopRequested = true;
            }
            else if (ImGui::Button("Play"))
            {
                p.playRequested   = true;
                p.seekRequested   = true;
                p.seekTargetFrame = (p.rate < 0.f) ? lastFrame : 0;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!p.playing ||
                                 p.skipPolicy == SequencePlayerComponent::SkipPolicy::None);
            if (ImGui::Button("Skip")) p.skipRequested = true;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fires the same request the player's skip button does. "
                                  "Jump-to-end is a plain seek to the last frame, so every "
                                  "actor lands in its authored final pose.");
            ImGui::SameLine();
            if (ImGui::Button("<|")) { p.seekRequested = true; p.seekTargetFrame = p.frame - 1; }
            ImGui::SameLine();
            if (ImGui::Button("|>")) { p.seekRequested = true; p.seekTargetFrame = p.frame + 1; }

            // Scrubbing is a SEEK, not a playhead poke: it must go through the
            // discontinuity contract (notify cursors re-armed, temporal history
            // reset) or dragging backwards replays every notify in every clip
            // the sequence touches.
            int scrub = p.frame;
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::SliderInt("##seqScrub", &scrub, 0, lastFrame > 0 ? lastFrame : 1,
                                 "frame %d"))
            {
                p.seekRequested   = true;
                p.seekTargetFrame = scrub;
            }

            if (seq)
            {
                ImGui::TextDisabled("%d frames @ %.4g fps  (%.2f s)   frame %d  t = %.2f s",
                                    lastFrame, fps, lastFrame / fps, p.frame, p.frame / fps);
                ImGui::TextDisabled("%zu bindings, %zu tracks",
                                    seq->bindings.size(), seq->tracks.size());
            }
            else if (!p.sequencePath.empty())
            {
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f),
                                   "Sequence failed to load — check the path.");
            }
            else
            {
                ImGui::TextDisabled("Drop a .iseq onto the Sequence field above.");
            }

            // ---- Cast ----
            // Roles come from the asset; the actors come from here. That split is
            // what makes one .iseq reusable across levels and prefab instances.
            ImGui::Separator();
            ImGui::TextDisabled("Cast — one actor per role declared by the sequence");

            if (seq)
            {
                for (const Seq::Binding& b : seq->bindings)
                {
                    if (b.IsSpawnable()) continue;   // the sequence creates these itself

                    auto it = std::find_if(p.roleBindings.begin(), p.roleBindings.end(),
                                           [&](const SequenceRoleBinding& rb)
                                           { return rb.role == b.role; });
                    if (it == p.roleBindings.end())
                    {
                        p.roleBindings.push_back(SequenceRoleBinding{ b.role, {} });
                        it = p.roleBindings.end() - 1;
                    }
                    ImGui::PushID(b.role.c_str());
                    if (world) Editor::DrawAttachmentRef(b.role.c_str(), *world, entity, it->target);
                    if (!b.childPath.empty())
                        ImGui::TextDisabled("    -> %s", b.childPath.c_str());
                    ImGui::PopID();
                }
            }

            // Entries whose role no longer exists in the asset are kept, not
            // dropped: repointing the component at a different .iseq and back
            // must not silently lose a cast the user wired up.
            bool anyStale = false;
            for (const SequenceRoleBinding& rb : p.roleBindings)
            {
                const bool live = seq && seq->FindBindingByRole(rb.role) >= 0;
                if (!live) { anyStale = true; break; }
            }
            if (anyStale && ImGui::TreeNode("Unused cast entries"))
            {
                int removeIdx = -1;
                for (int i = 0; i < static_cast<int>(p.roleBindings.size()); ++i)
                {
                    const SequenceRoleBinding& rb = p.roleBindings[i];
                    if (seq && seq->FindBindingByRole(rb.role) >= 0) continue;
                    ImGui::PushID(i);
                    ImGui::TextDisabled("%s", rb.role.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Remove")) removeIdx = i;
                    ImGui::PopID();
                }
                if (removeIdx >= 0) p.roleBindings.erase(p.roleBindings.begin() + removeIdx);
                ImGui::TreePop();
            }
        });

    // AudioSourceComponent - 2D/3D sound emitter. Reflected fields (clip path +
    // drag-drop, bus, volume/pitch, loop, 3D, attenuation) are drawn by the
    // reflection pass; postDraw owns the live Play/Pause/Stop transport (flags
    // only - AudioSystem starts/stops the actual XAudio2 voice).
    RegisterReflectedComponent<Audio::AudioSourceComponent>("Audio Source",
        /*priority*/ 33,
        [](Audio::AudioSourceComponent& a, World* /*world*/, Entity /*entity*/)
        {
            ImGui::Separator();
            const bool sounding = (a.state == Audio::PlayState::Playing ||
                                   a.state == Audio::PlayState::Paused);
            if (sounding)
            {
                if (ImGui::Button(a.paused ? "Resume" : "Pause"))
                    a.paused = !a.paused;
                ImGui::SameLine();
                if (ImGui::Button("Stop"))
                    a.stopRequested = true;
            }
            else if (ImGui::Button("Play"))
            {
                a.playRequested = true;
                a.paused        = false;
            }

            ImGui::SameLine();
            const char* st = "Stopped";
            switch (a.state)
            {
                case Audio::PlayState::Playing:  st = "Playing";  break;
                case Audio::PlayState::Paused:   st = "Paused";   break;
                case Audio::PlayState::Stopping: st = "Stopping"; break;
                default: break;
            }
            ImGui::Text("State: %s", st);

            if (a.clipPath.empty())
                ImGui::TextDisabled("Assign a .aclip (drag from Asset panel), then Play "
                                    "or enable Play On Enable.");
        });

    // AudioListenerComponent - listener pose source (usually the camera). All
    // fields (masterVolume / active) are reflected; no postDraw needed.
    RegisterReflectedComponent<Audio::AudioListenerComponent>("Audio Listener",
        /*priority*/ 34);

    // MorphComponent - descriptor for paused/looping; clip combo + weight inspector in postDraw.
    // Manual mode (no clip bound) lets the user drag mesh-target weights directly to
    // author multi-morph blends without needing an animation.
    RegisterReflectedComponent<MorphComponent>("Morph",
        /*priority*/ 35,
        [this, renderer](MorphComponent& morph, World* world, Entity entity)
        {
            if (!renderer || !world) return;
            const MorphClipLibrary& morphLib  = renderer->GetMorphClipLibrary();
            const uint32_t          clipCount = morphLib.Count();

            // Resolve the skeleton asset for this entity (MorphComponent lives on
            // the skeleton root, which also carries SkeletonComponent).
            const SkeletonAsset* skelAsset = nullptr;
            {
                auto* sc = world->GetComponent<SkeletonComponent>(entity);
                if (sc && sc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skelAsset = &renderer->GetSkeletonRegistry().Get(sc->assetIndex);
            }

            auto clipLabel = [&](uint32_t idx) -> std::string {
                if (idx == kInvalidClipIndex) return "(none - manual)";
                if (idx >= clipCount)         return "(invalid)";
                const MorphClipAsset& c = morphLib.Get(idx);
                char buf[160];
                snprintf(buf, sizeof(buf), "%s  |  %.2f s  |  %u channels##mc%u",
                         c.name[0] ? c.name : ("MorphClip #" + std::to_string(idx)).c_str(),
                         c.duration, c.morphCount, idx);
                return buf;
            };
            ImGui::TextDisabled("Morph Clip");
            {
                const std::string cur = clipLabel(morph.primaryMorphClip);
                if (ImGui::BeginCombo("##morph_clip", cur.c_str()))
                {
                    // Only the clips that would move THIS rig: a clip binds by channel
                    // NAME (GetMorphRemap), so one naming none of the skeleton's morphs
                    // plays nothing here. The library is every clip bound this session.
                    std::unordered_set<std::string_view> rigNames;
                    if (skelAsset)
                        for (uint32_t m = 0; m < std::min<uint32_t>(skelAsset->morphTargetCount,
                                                                    SkeletonAsset::MAX_MORPH_TARGETS); ++m)
                            if (skelAsset->morphTargetNames[m][0]) rigNames.insert(skelAsset->morphTargetNames[m]);
                    uint32_t hidden = 0;
                    if (ImGui::Selectable("(none - manual)",
                                          morph.primaryMorphClip == kInvalidClipIndex))
                    {
                        morph.primaryMorphClip = kInvalidClipIndex;
                        // Re-key weights to mesh-target indexing for manual mode;
                        // clip-indexed values left over from a previous clip would
                        // land on the wrong morph targets, so reset them.
                        if (skelAsset)
                        {
                            morph.count = std::min<uint32_t>(skelAsset->morphTargetCount,
                                                              MorphComponent::MAX);
                            for (uint32_t i = 0; i < morph.count; ++i)
                                morph.weights[i] = 0.f;
                        }
                    }
                    for (uint32_t i = 0; i < clipCount; ++i)
                    {
                        const bool      sel = (morph.primaryMorphClip == i);
                        const MorphClipAsset& c = morphLib.Get(i);
                        uint32_t matched = 0;
                        for (uint32_t ch = 0; ch < c.morphCount; ++ch)
                            matched += rigNames.count(std::string_view(c.morphNames[ch])) ? 1u : 0u;
                        if (matched == 0 && !sel) { ++hidden; continue; }
                        if (ImGui::Selectable(clipLabel(i).c_str(), sel))
                        { morph.primaryMorphClip = i; morph.time = 0.f; }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%u of its %u channels name a morph of this skeleton", matched,
                                              c.morphCount);
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    if (hidden > 0)
                        ImGui::TextDisabled("(%u clip(s) for other rigs hidden)", hidden);
                    ImGui::EndCombo();
                }
            }

            const bool hasClip = (morph.primaryMorphClip != kInvalidClipIndex
                               && morph.primaryMorphClip < clipCount);
            const bool hasAnim = world->HasComponent<AnimationComponent>(entity);

            if (hasClip && !hasAnim)
            {
                const float dur = morphLib.Get(morph.primaryMorphClip).duration;
                EditorUI::DragFloat("Time##mt", &morph.time, 0.f,
                                   dur > 0.f ? dur : 1.f, "%.3f s");
            }
            else if (hasClip && hasAnim)
            {
                ImGui::TextDisabled("Time synced to AnimationComponent");
            }

            ImGui::Separator();

            // ---- Manual mode: drag any mesh morph target ----
            if (!hasClip)
            {
                if (!skelAsset || skelAsset->morphTargetCount == 0)
                {
                    ImGui::TextDisabled("(skeleton has no morph targets)");
                    return;
                }
                const uint32_t n = std::min<uint32_t>(skelAsset->morphTargetCount,
                                                      MorphComponent::MAX);
                // Ensure count is sized so the skinning subsystem picks weights up.
                if (morph.count < n) morph.count = n;

                // With a Facial Animation component the sliders are its Authored list -
                // saved with the scene / prefab, folded on top of every other weight
                // (BuildSkinJobs) - one entry per morph name, a slider at 0 removing its
                // entry. Without one they edit MorphComponent::weights[], runtime only (no
                // serializer, rewritten by a bound clip, zeroed on a rebind).
                FacialAnimationComponent* fa = world->GetComponent<FacialAnimationComponent>(entity);
                const std::type_index     faType(typeid(FacialAnimationComponent));
                auto faBlob = [&]() {
                    return Editor::Undo::SerializeComponentBlob(GetComponentRegistry(), *world, entity, faType);
                };
                // One undo step per gesture: the blob before it, and the one after.
                auto commit = [&](const std::string& before)
                {
                    const std::string after = faBlob();
                    if (UndoEnabled() && !before.empty() && after != before)
                        PushEdit(std::make_unique<Editor::Undo::ComponentEditAction>(
                            world->MakeHandle(entity), faType, before, after, "Facial Animation"));
                };

                ImGui::TextDisabled(fa ? "Mesh Morph Targets (%u) - saved as Facial Animation > Authored:"
                                       : "Mesh Morph Targets (%u) - drag to blend:", n);
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset All##morph_reset"))
                {
                    if (fa)
                    {
                        const std::string before = faBlob();
                        fa->authored.clear();
                        commit(before);
                    }
                    else
                        for (uint32_t i = 0; i < n; ++i) morph.weights[i] = 0.f;
                }
                if (!fa)
                    ImGui::TextDisabled("(runtime only - NOT saved. Add a Facial Animation component\n"
                                        " and these sliders become its saved Authored list.)");

                static ImGuiTextFilter s_morphFilter;
                s_morphFilter.Draw("Filter##morph_filter", 180.f);

                const bool        dragging   = fa && m_morphAuthoredEntity == entity && !m_morphAuthoredBefore.empty();
                const std::string frameStart = (fa && !dragging) ? faBlob() : std::string();
                for (uint32_t i = 0; i < n; ++i)
                {
                    const char* raw = skelAsset->morphTargetNames[i];
                    const char* nm  = raw[0] ? raw : "(unnamed)";
                    if (!s_morphFilter.PassFilter(nm)) continue;

                    char lbl[96];
                    snprintf(lbl, sizeof(lbl), "%s##mm%u", nm, i);
                    if (!fa)
                    {
                        EditorUI::DragFloat(lbl, &morph.weights[i], 0.f, 1.f);
                        continue;
                    }
                    // Keyed by NAME: an unnamed target cannot be saved, and a repeated
                    // name is its FIRST slot's (the fold resolves names that way).
                    const int16_t first = raw[0] ? Facial::FindMorphSlot(*skelAsset, raw) : -1;
                    if (!raw[0] || first != static_cast<int16_t>(i))
                    {
                        ImGui::BeginDisabled();
                        float z = 0.f;
                        EditorUI::DragFloat(lbl, &z, 0.f, 1.f);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip(raw[0] ? "Same name as slot %d - that slider saves it."
                                                     : "An unnamed morph cannot be saved (the list is by name).",
                                              static_cast<int>(first));
                        continue;
                    }
                    float w = std::max(0.f, Facial::GetAuthored(*fa, raw));
                    if (EditorUI::DragFloat(lbl, &w, 0.f, 1.f)) Facial::SetAuthored(*fa, raw, w);
                    if (ImGui::IsItemActivated() && !dragging)
                    {
                        m_morphAuthoredBefore = frameStart;
                        m_morphAuthoredEntity = entity;
                    }
                    if (ImGui::IsItemDeactivated() && m_morphAuthoredEntity == entity)
                    {
                        commit(m_morphAuthoredBefore);
                        m_morphAuthoredBefore.clear();
                        m_morphAuthoredEntity = NullEntity;
                    }
                }
                return;
            }

            // ---- Clip mode: show sampled weights (editable only when paused) ----
            if (morph.count == 0)
            {
                ImGui::TextDisabled("(weights not yet sampled - play to see values)");
                return;
            }
            ImGui::TextDisabled("Morph Weights (%u channels):", morph.count);
            const MorphClipAsset& asset  = morphLib.Get(morph.primaryMorphClip);
            const bool            canEdit = morph.paused && !hasAnim;
            for (uint32_t i = 0; i < morph.count; ++i)
            {
                const char* nm = (i < asset.morphCount && asset.morphNames[i][0])
                                 ? asset.morphNames[i] : "(unnamed)";
                if (canEdit)
                {
                    char lbl[96];
                    snprintf(lbl, sizeof(lbl), "[%u] %s##cw%u", i, nm, i);
                    EditorUI::DragFloat(lbl, &morph.weights[i], 0.f, 1.f);
                }
                else
                {
                    char ov[96];
                    snprintf(ov, sizeof(ov), "[%u] %s  %.3f", i, nm, morph.weights[i]);
                    ImGui::ProgressBar(morph.weights[i], ImVec2(-1.f, 0.f), ov);
                }
            }
            if (!canEdit)
                ImGui::TextDisabled("(pause + no AnimComp to edit, or switch clip to (none) for manual)");
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // FaceShadingComponent - NPR SDF face-shadow head-axis feed (pure data: head
    // bone name + axis pickers + read-only basis). The per-frame feed itself runs
    // in Renderer::TickAnimationChain (FaceShading::Update).
    RegisterReflectedComponent<FaceShadingComponent>("Face Shading", /*priority*/ 41);

    // HeadTrackComponent - head look-at. The reflection pass draws the tunables
    // (limits/weight/axes); this postDraw adds a live bone-name dropdown so the
    // author can pick the head bone from the actual skeleton instead of typing it.
    RegisterReflectedComponent<HeadTrackComponent>("Head Track",
        /*priority*/ 42,
        [renderer, this](HeadTrackComponent& comp, World* world, Entity entity)
        {
            if (!renderer || !world) return;

            // Look target: empty = active camera. Pick any entity (or drag-drop one
            // from the Hierarchy onto this row) to make the head follow it instead.
            Editor::DrawAttachmentRef("Look Target (empty = camera)", *world, entity, comp.targetRef);

            const SkeletonComponent* skc = world->GetComponent<SkeletonComponent>(entity);
            const SkeletonAsset* skel = nullptr;
            if (skc && skc->assetIndex != kInvalidSkeletonIndex
                && skc->assetIndex < renderer->GetSkeletonRegistry().Count())
                skel = &renderer->GetSkeletonRegistry().Get(skc->assetIndex);
            if (!skel) { ImGui::TextDisabled("(entity has no skeleton asset)"); return; }

            // Resolve the current name for display + a not-found warning.
            uint32_t curIdx = ~0u;
            for (uint32_t b = 0; b < skel->boneCount; ++b)
                if (std::strcmp(skel->boneNames[b], comp.headBoneName.c_str()) == 0) { curIdx = b; break; }

            const char* curLabel = (curIdx != ~0u) ? skel->boneNames[curIdx] : "(pick head bone)";
            if (ImGui::BeginCombo("Head Bone", curLabel))
            {
                for (uint32_t bi = 0; bi < skel->boneCount; ++bi)
                {
                    const bool sel = (curIdx == bi);
                    char row[96];
                    snprintf(row, sizeof(row), "[%u] %s", bi, skel->boneNames[bi]);
                    if (ImGui::Selectable(row, sel))
                    {
                        comp.headBoneName  = skel->boneNames[bi];
                        comp.cachedBoneIdx = ~0u;   // force re-resolve next tick
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            if (curIdx == ~0u)
                ImGui::TextDisabled("  bone \"%s\" not found in skeleton", comp.headBoneName.c_str());
        });

    // FacialAnimationComponent - morph-target (blend shape) control: auto-blink,
    // expressions, vowel visemes, and the designer's persistent Authored Channels.
    // Unrelated to FaceShadingComponent (SDF face shadows).
    //
    // The reflection pass draws scalars only. Every morph name on this component is
    // a char[64]: Reflect::FieldKind has no kind for char[] (REFLECT_STRING is
    // std::string only), and REFLECT_ARRAY is out because its '+' button memsets the
    // next slot. So all six name fields and the authored list live here, where the
    // skeleton's morph-name table turns typing a Japanese morph name into a dropdown.
    //
    // requires_ is HasComponent<SkeletonComponent> because the component is a silent
    // no-op anywhere else: BuildSkinJobs reads it only off the entity a section's
    // SkeletonRef resolves to. The gate covers the Add Component menu as well as the
    // inspector section, so it can never be added to an entity that cannot use it.
    RegisterReflectedComponent<FacialAnimationComponent>("Facial Animation",
        /*priority*/ 43,
        [renderer](FacialAnimationComponent& comp, World* world, Entity entity)
        {
            using FA = FacialAnimationComponent;
            if (!renderer || !world) return;

            // ---- Persistence banner ----
            // SaveScene writes prefab instances and .iscn placements as pure
            // references and drops every descendant row (SceneSerializer's marking
            // passes and its PrefabInstanceMember sweep). The skeleton entity of every
            // shipped character is such a descendant, so settings authored here are
            // silently lost on Save Scene. SavePrefab runs the full component registry
            // on every node, so Save Prefab keeps them.
            {
                bool underPrefab = false;
                bool underScene  = false;
                if (const PrefabInstanceMember* pim =
                        world->GetComponent<PrefabInstanceMember>(entity))
                    if (pim->root != NullEntity && pim->root != entity) underPrefab = true;

                Entity a = entity;
                for (int guard = 0; guard < 256 && !underPrefab; ++guard)
                {
                    const Parent* p = world->GetComponent<Parent>(a);
                    a = p ? p->entity : NullEntity;
                    if (a == NullEntity || !world->IsAlive(a)) break;
                    const PrefabSourcePath* pf = world->GetComponent<PrefabSourcePath>(a);
                    if (pf && !pf->path.empty()) { underPrefab = true; break; }
                    const SceneSourcePath* sp = world->GetComponent<SceneSourcePath>(a);
                    if (sp && !sp->path.empty()) { underScene = true; break; }
                }

                if (underPrefab || underScene)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.3f, 1.f));
                    ImGui::TextWrapped("%s",
                        underPrefab
                        ? "This entity is inside a PREFAB INSTANCE. Save Scene will NOT keep "
                          "these settings - it stores the instance as a bare reference and "
                          "skips every descendant row. Author them here, then Save Prefab "
                          "(.ipfb) / Apply to Prefab."
                        : "This entity is a descendant of a .iscn placement. Save Scene skips "
                          "descendant rows, so these settings will NOT survive it. Re-save the "
                          "prefab or the .iscn that owns this character instead.");
                    ImGui::PopStyleColor();
                    ImGui::Separator();
                }
            }

            // ---- Skeleton gate ----
            // SkeletonRegistry::Get is a bare m_skeletons[index] with no bounds check,
            // so both tests are mandatory.
            const SkeletonComponent* skc = world->GetComponent<SkeletonComponent>(entity);
            const SkeletonAsset*     skel = nullptr;
            if (skc && skc->assetIndex != kInvalidSkeletonIndex
                && skc->assetIndex < renderer->GetSkeletonRegistry().Count())
                skel = &renderer->GetSkeletonRegistry().Get(skc->assetIndex);
            if (!skel)
            {
                ImGui::TextDisabled("(entity has no skeleton asset - names cannot be resolved yet)");
                return;
            }
            const uint32_t morphCount = std::min<uint32_t>(skel->morphTargetCount,
                                                           SkeletonAsset::MAX_MORPH_TARGETS);
            if (morphCount == 0)
            {
                ImGui::TextDisabled("(this skeleton has no morph targets)");
                return;
            }
            if (Facial::IsNameTableTruncated(*skel))
                ImGui::TextDisabled("Name table is at the %u cap - morphs past it exist on the GPU\n"
                                    "but cannot be addressed by name.",
                                    SkeletonAsset::MAX_MORPH_TARGETS);

            // Morph-name dropdown over the skeleton's actual target list. Returns true
            // when the name changed; the caller then drops every cached name->slot
            // resolution (blinkMorph self-heals through blinkResolvedName, the viseme
            // and authored names do not).
            auto morphCombo = [&](const char* label, char* dst) -> bool
            {
                bool changed = false;
                const int16_t cur = Facial::FindMorphSlot(*skel, dst);
                char preview[128];
                if (cur >= 0)          snprintf(preview, sizeof(preview), "[%d] %s", (int)cur, dst);
                else if (dst[0])       snprintf(preview, sizeof(preview), "%s  (not in this skeleton)", dst);
                else                   snprintf(preview, sizeof(preview), "(none)");

                if (ImGui::BeginCombo(label, preview))
                {
                    if (ImGui::Selectable("(none)##fa_none", dst[0] == '\0'))
                    { dst[0] = '\0'; changed = true; }
                    for (uint32_t m = 0; m < morphCount; ++m)
                    {
                        const char* nm = skel->morphTargetNames[m][0]
                                       ? skel->morphTargetNames[m] : "(unnamed)";
                        char row[160];
                        snprintf(row, sizeof(row), "[%u] %s##fa_o%u", m, nm, m);
                        const bool sel = (cur == static_cast<int16_t>(m));
                        if (ImGui::Selectable(row, sel))
                        {
                            Facial::CopyName(dst, skel->morphTargetNames[m]);
                            changed = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                return changed;
            };

            bool namesChanged = false;

            ImGui::SeparatorText("Morph Names");
            if (morphCombo("Blink Morph##fa_blink", comp.blinkMorph)) namesChanged = true;
            static const char kVowelKeys[] = "aiueo";
            for (int v = 0; v < FA::kVowelCount; ++v)
            {
                char lbl[48];
                snprintf(lbl, sizeof(lbl), "Vowel %c##fa_v%d", kVowelKeys[v], v);
                if (morphCombo(lbl, comp.visemeMorph[v])) namesChanged = true;
            }

            // ---- Blink Now ----
            // TriggerBlink alone renders as nothing in Edit: the state machine only
            // advances while dt > 0, and Edit ticks the animation chain with dt == 0.
            // ForceBlinkWeight stamps the machine's output so the closed eye shows; the
            // next dt > 0 tick overwrites it from the machine, so Play still looks
            // normal. The reopen button exists for the same reason - at dt == 0 nothing
            // can open it.
            ImGui::Separator();
            if (ImGui::Button("Blink Now##fa_blink_now"))
            {
                FacialAnimationSystem::TriggerBlink(comp, 0.f, 0.f, 0.f, 0.f); // <=0 = authored values
                Facial::ForceBlinkWeight(comp, comp.blinkMaxWeight);
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Eyes##fa_blink_open"))
                Facial::ForceBlinkWeight(comp, 0.f);
            ImGui::SameLine();
            if (comp.blinkSlot >= 0)
                ImGui::TextDisabled("slot %d | w %.3f", (int)comp.blinkSlot, comp.blinkWeightCur);
            else
                ImGui::TextDisabled("blink morph unresolved");

            // ---- Authored Channels ----
            // Persistent designer base, applied first (Override): a sparse list keyed
            // by morph name (not channels, so it does not count against the live ones;
            // the Morph panel's sliders write it too). A new entry resolves its slot on
            // the next tick by itself.
            ImGui::SeparatorText("Authored Channels");
            ImGui::TextDisabled("Persistent expression saved with the component (%zu morph(s)).",
                                comp.authored.size());

            int removeIdx = -1;
            for (std::size_t r = 0; r < comp.authored.size(); ++r)
            {
                FA::AuthoredEntry& a = comp.authored[r];
                ImGui::PushID(static_cast<int>(r));
                const bool missing = (Facial::FindMorphSlot(*skel, a.name.c_str()) < 0);
                if (missing) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.3f, 1.f));
                ImGui::TextUnformatted(a.name.empty() ? "(empty)" : a.name.c_str());
                if (missing)
                {
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(not in this skeleton)");
                }
                ImGui::SetNextItemWidth(160.f);
                float w = a.weight;
                if (EditorUI::DragFloat("##fa_aw", &w, 0.f, 1.f)) a.weight = std::clamp(w, 0.f, 1.f);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("At 0 the entry is not saved (X removes it now)");
                ImGui::SameLine();
                if (ImGui::SmallButton("X##fa_arm")) removeIdx = static_cast<int>(r);
                ImGui::PopID();
            }
            if (removeIdx >= 0) comp.authored.erase(comp.authored.begin() + removeIdx);

            if (comp.authored.size() < static_cast<std::size_t>(FA::kMaxAuthored))
            {
                if (ImGui::BeginCombo("Add##fa_add", "(pick a morph to add)"))
                {
                    for (uint32_t m = 0; m < morphCount; ++m)
                    {
                        const char* nm = skel->morphTargetNames[m];
                        if (!nm[0] || Facial::GetAuthored(comp, nm) >= 0.f) continue;
                        char row[160];
                        snprintf(row, sizeof(row), "[%u] %s##fa_a%u", m, nm, m);
                        if (ImGui::Selectable(row)) Facial::SetAuthored(comp, nm, 1.0f);
                    }
                    ImGui::EndCombo();
                }
            }
            else
            {
                ImGui::TextDisabled("(authored list full - %d morphs)", FA::kMaxAuthored);
            }

            // One call covers every name edit above. It only drops cached slots, so it
            // is cheap and safe to run on any change.
            if (namesChanged) Facial::InvalidateResolution(comp);

            // ---- Live channels (read-only) ----
            // Runtime state. In Edit only Authored rows are normally live; Expression /
            // Viseme / Manual appear once a script has driven them in Play.
            ImGui::SeparatorText("Live Channels (read-only)");
            static const char* kGroupName[] =
                { "Authored", "Expression", "Viseme", "Manual", "Blink" };

            // A slot an Override channel owns wins over the MorphComponent manual weight
            // for the same slot, because BuildSkinJobs folds resolvedOut after the
            // clip/manual fill. Surface the conflict.
            const MorphComponent* morphComp = world->GetComponent<MorphComponent>(entity);
            const bool morphManual = morphComp && morphComp->primaryMorphClip == kInvalidClipIndex;

            int live = 0;
            for (int k = 0; k < FA::kMaxChannels; ++k)
            {
                const FA::Channel& c = comp.channels[k];
                if (c.state == 0) continue;
                ++live;
                const int  g          = (c.group < 5u) ? static_cast<int>(c.group) : 4;
                const bool unresolved = (c.slot < 0);
                if (unresolved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.3f, 1.f));
                ImGui::Text("%-10s %-24s %.3f -> %.3f  slot %d%s",
                            kGroupName[g],
                            comp.channelName[k][0] ? comp.channelName[k] : "(unnamed)",
                            c.current, c.target, (int)c.slot,
                            (c.state == 2) ? "  (releasing)" : "");
                if (unresolved) ImGui::PopStyleColor();

                if (!unresolved && morphManual
                    && static_cast<uint32_t>(c.slot) < morphComp->count
                    && morphComp->weights[c.slot] > 0.f
                    && c.group != static_cast<uint8_t>(FacialGroup::Blink))
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(overrides Morph manual weight %.3f)",
                                        morphComp->weights[c.slot]);
                }
            }
            if (live == 0) ImGui::TextDisabled("(no live channels)");

            static const char* kPhase[] = { "idle", "closing", "hold", "opening" };
            const int phase = (comp.blinkPhase < 4u) ? static_cast<int>(comp.blinkPhase) : 0;
            ImGui::TextDisabled("Blink machine: %s  t=%.3f  idle=%.2f/%.2f s",
                                kPhase[phase], comp.blinkT, comp.blinkIdleT, comp.blinkNextAt);

            // resolvedOut is what BuildSkinJobs will actually fold onto the morph pool
            // this frame - the one read that proves name -> slot resolution end to end.
            if (comp.resolvedCount > 0)
            {
                std::string line;
                char        cell[64];
                const uint32_t shown = std::min<uint32_t>(comp.resolvedCount, 12);
                for (uint32_t r = 0; r < shown; ++r)
                {
                    snprintf(cell, sizeof(cell), "%s%u=%.2f", r ? ", " : "",
                             static_cast<unsigned>(comp.resolvedOut[r].slot),
                             comp.resolvedOut[r].weight);
                    line += cell;
                }
                if (shown < comp.resolvedCount) line += ", ...";
                ImGui::TextDisabled("Resolved (%u): %s", comp.resolvedCount, line.c_str());
            }
            else
            {
                ImGui::TextDisabled("Resolved (0): nothing reaches the morph pool this frame.");
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // ChainPhysicsComponent - compact row. The full KawaiiPhysics-style chain
    // authoring UI (global sliders, per-group root pick / excludes / overrides,
    // auto-detect) is the Chain Physics Preview panel, opened on this
    // character by the Edit button below.
    RegisterComponentEditor<ChainPhysicsComponent>("Chain Physics",
        [this](void* ptr, World* world, Entity entity)
        {
            auto& comp = *static_cast<ChainPhysicsComponent*>(ptr);

            ImGui::Checkbox("Enabled", &comp.enabled);

            int chains = 0, springs = 0;
            std::vector<int> pieces;
            for (const ChainGroupDef& g : comp.groups)
            {
                switch (g.type)
                {
                case ChainGroupType::Chain:  ++chains;  break;
                case ChainGroupType::Spring: ++springs; break;
                case ChainGroupType::Cloth:
                {
                    bool seen = false;
                    for (int rg : pieces) if (rg == g.ringGroup) { seen = true; break; }
                    if (!seen) pieces.push_back(g.ringGroup);
                    break;
                }
                default: break;
                }
            }
            // The Inspector column is narrow - wrap.
            ImGui::TextWrapped("%d chain(s) | %d cloth piece(s) | %d spring(s)   (%d/%d groups)",
                               chains, static_cast<int>(pieces.size()), springs,
                               static_cast<int>(comp.groups.size()),
                               ChainPhysicsComponent::MAX_GROUPS);
            if (comp.groups.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
                ImGui::TextWrapped("No authored chains — legacy bone-name auto-detection.");
                ImGui::PopStyleColor();
            }

            if (const auto* cc = world ? world->GetComponent<CapsuleColliderComponent>(entity) : nullptr)
                ImGui::TextDisabled("%d capsule collider(s)", cc->count);

            if (ImGui::TreeNode("Main viewport tools"))
            {
                ImGui::Checkbox("Show chain overlay in viewport", &m_chainOverlayEnabled);

                // The main viewport's rest-pose gizmo (the only author of a prefab
                // instance's per-instance CPR rest pose) is armed here, for the
                // selection, so it does not depend on another window staying open.
                {
                    const bool stopped = GetPlayState() == ViewportPlayState::Stopped;
                    bool pm = m_chainMainPose.poseMode;
                    ImGui::BeginDisabled(!stopped);
                    if (ImGui::Checkbox("Pose rest bones in main viewport", &pm))
                    {
                        m_chainMainPose.poseMode = pm;
                        if (pm) m_chainOverlayEnabled = true;   // picker and overlay share one gate
                        else    m_chainMainPose.poseBone = ~0u;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(stopped
                            ? "Click a chain bone of this character in the viewport, then drag\n"
                              "its rotation ring: writes THIS entity's authored rest pose (one\n"
                              "undo entry per drag). Not available in F11 fullscreen."
                            : "Edit mode only.");
                    if (m_chainMainPose.poseMode && stopped)
                    {
                        ImGui::Indent();
                        const int ng = static_cast<int>(comp.groups.size());
                        if (m_chainMainPose.focusGroup >= ng) m_chainMainPose.focusGroup = -1;
                        const std::string fprev = m_chainMainPose.focusGroup >= 0
                            ? (comp.groups[m_chainMainPose.focusGroup].name[0]
                                   ? comp.groups[m_chainMainPose.focusGroup].name : "(chain)")
                            : "All groups";
                        if (ImGui::BeginCombo("Focus group##cpmain", fprev.c_str()))
                        {
                            if (ImGui::Selectable("All groups", m_chainMainPose.focusGroup < 0))
                                m_chainMainPose.focusGroup = -1;
                            for (int gi = 0; gi < ng; ++gi)
                            {
                                char lbl[112];
                                snprintf(lbl, sizeof(lbl), "%s  |  %s##cpmf%d",
                                         comp.groups[gi].name[0] ? comp.groups[gi].name : "(chain)",
                                         comp.groups[gi].rootBone[0] ? comp.groups[gi].rootBone : "(no root)",
                                         gi);
                                if (ImGui::Selectable(lbl, m_chainMainPose.focusGroup == gi))
                                    m_chainMainPose.focusGroup = gi;
                            }
                            ImGui::EndCombo();
                        }
                        const SkeletonComponent* skc = world ? world->GetComponent<SkeletonComponent>(entity) : nullptr;
                        const SkeletonAsset* skel =
                            (skc && m_renderer && skc->assetIndex != kInvalidSkeletonIndex &&
                             skc->assetIndex < m_renderer->GetSkeletonRegistry().Count())
                                ? &m_renderer->GetSkeletonRegistry().Get(skc->assetIndex) : nullptr;
                        if (skel && m_chainMainPose.owner == entity &&
                            m_chainMainPose.poseBone < skel->boneCount)
                            ImGui::Text("Selected bone: %s", skel->boneNames[m_chainMainPose.poseBone]);
                        else
                            ImGui::TextDisabled("Selected bone: (click one in the viewport)");
                        ImGui::Unindent();
                    }
                }

            ImGui::TreePop();
            }

            // The chain editor is the Chain Physics Preview panel. Edit mode:
            // preview this character there (or just focus it when it already is
            // the subject); Play: edit it live there.
            if (ImGui::Button("Edit Chains / Colliders..."))
            {
                if (world)
                    Editor::ChainPhysicsPreviewWindow::Get().RequestEditEntity(
                        world->MakeHandle(entity), GetPlayState() != ViewportPlayState::Stopped);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open the Chain Physics Preview on this character: tune on a\n"
                                  "staged copy, then Apply. In Play it edits this character LIVE\n"
                                  "(transient - reverted on Stop; 'Keep these values' stages them).");
        },
        /*priority*/ 40,
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // Undo/redo of ChainPhysicsComponent must re-init the running simulation:
    // the generic captureValue restores the component data, but the sim keeps
    // the pre-undo chain topology until something calls Invalidate. Same
    // pattern as the MaterialComponent custom capture (SetDirty-on-restore).
    {
        auto& chainEntry =
            m_componentEditors[std::type_index(typeid(ChainPhysicsComponent))];
        chainEntry.captureValue =
            [renderer](World& w, Entity e) -> Editor::Undo::RestoreFn
        {
            const ChainPhysicsComponent* c = w.GetComponent<ChainPhysicsComponent>(e);
            if (!c) return {};
            auto saved = std::make_shared<ChainPhysicsComponent>(*c);
            return [saved, renderer](World& w2, Entity e2)
            {
                if (ChainPhysicsComponent* c2 = w2.GetComponent<ChainPhysicsComponent>(e2))
                {
                    *c2 = *saved;
                    if (renderer)
                        if (ChainPhysicsSystem* cps = renderer->GetChainPhysicsSystem())
                            cps->Invalidate(e2);
                }
            };
        };
    }

    // PrefabSourcePath - prefab-instance link, read-only display + detach hint.
    // While present, SaveScene stores this subtree as a pure PrefabRef line
    // (edits to the .ipfb propagate to every scene). Removing the component
    // (the x on the header) detaches the instance -> saves as plain data again.
    RegisterReflectedComponent<PrefabSourcePath>("Prefab Ref",
        /*priority*/ 71,
        [](PrefabSourcePath& psp, World*, Entity)
        {
            ImGui::TextDisabled("Source : %s", psp.path.c_str());
            ImGui::TextDisabled("Scene saves this subtree as a prefab REFERENCE.");
            ImGui::TextDisabled("Remove this component to detach (save as plain data).");
        });

    // SkeletonRef - read-only display only. Empty descriptor + postDraw.
    RegisterReflectedComponent<SkeletonRef>("Skeleton Ref",
        /*priority*/ 70,
        [](SkeletonRef& ref, World* world, Entity)
        {
            if (ref.entity == NullEntity)
            {
                ImGui::TextDisabled("(not linked)");
                return;
            }
            ImGui::TextDisabled("Root entity : %u", ref.entity);
            if (world)
            {
                const std::string& nm = world->GetName(ref.entity);
                ImGui::TextDisabled("Root name   : %s", nm.c_str());
                const bool hasAnim  = world->HasComponent<AnimationComponent>(ref.entity);
                const bool hasSkel  = world->HasComponent<SkeletonComponent>(ref.entity);
                const bool hasMorph = world->HasComponent<MorphComponent>(ref.entity);
                ImGui::TextDisabled("Root has    :%s%s%s",
                    hasSkel  ? " Skeleton"   : "",
                    hasAnim  ? " Animation"  : "",
                    hasMorph ? " Morph"      : "");
            }
            ImGui::TextDisabled("(select root entity to edit animation)");
        });

    // CapsuleColliderComponent - bone combos need Renderer::GetSkeletonRegistry; custom postDraw.
    RegisterReflectedComponent<CapsuleColliderComponent>("Capsule Colliders",
        /*priority*/ 45,
        [renderer](CapsuleColliderComponent& ccRef, World* world, Entity entity)
        {
            auto* cc = &ccRef;

            // Show bone name list from skeleton if available.
            const SkeletonAsset* skel = nullptr;
            if (renderer && world)
            {
                const SkeletonComponent* sc = world->GetComponent<SkeletonComponent>(entity);
                if (sc && sc->assetIndex != kInvalidSkeletonIndex
                    && sc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skel = &renderer->GetSkeletonRegistry().Get(sc->assetIndex);
            }

            auto boneName = [&](uint32_t idx) -> const char* {
                if (skel && idx < skel->boneCount && skel->boneNames[idx][0])
                    return skel->boneNames[idx];
                return "(?)";
            };

            // Case-insensitive substring (self-contained - mirrors the Chain
            // Physics root picker; no <cctype>/<algorithm> dependency).
            auto icontains = [](const char* hay, const char* needle) -> bool {
                if (!needle || !needle[0]) return true;
                for (const char* h = hay; *h; ++h) {
                    const char* a = h; const char* b = needle;
                    while (*a && *b) {
                        char ca = *a, cb = *b;
                        if (ca >= 'A' && ca <= 'Z') ca += 32;
                        if (cb >= 'A' && cb <= 'Z') cb += 32;
                        if (ca != cb) break;
                        ++a; ++b;
                    }
                    if (!*b) return true;
                }
                return false;
            };

            // children[] / roots[] for the bone hierarchy tree (built once per draw).
            std::vector<std::vector<uint32_t>> children;
            std::vector<uint32_t> roots;
            if (skel) {
                children.resize(skel->boneCount);
                for (uint32_t bi = 0; bi < skel->boneCount; ++bi) {
                    if (skel->parentIndex[bi] < 0) roots.push_back(bi);
                    else children[static_cast<uint32_t>(skel->parentIndex[bi])].push_back(bi);
                }
            }

            // Bone picker - same UX as the Chain Physics root picker: the current
            // bone is shown inline, "Pick..." opens a popup with a name filter that
            // switches between a flat match list (filter set) and the full
            // hierarchy tree (filter empty). Writes the chosen bone index into
            // boneIdx. Capsule endpoints are consumed fresh every frame by the
            // sim, so no Invalidate() is needed on change.
            auto bonePicker = [&](const char* label, uint32_t& boneIdx)
            {
                ImGui::PushID(label);
                ImGui::Text("%s: %s", label, boneName(boneIdx));
                ImGui::SameLine();
                if (ImGui::Button("Pick...")) ImGui::OpenPopup("pick_bone");
                if (ImGui::BeginPopup("pick_bone"))
                {
                    static char s_filter[64] = "";
                    ImGui::InputText("Filter", s_filter, sizeof(s_filter));
                    ImGui::BeginChild("bonetree", ImVec2(300, 320), true);
                    if (!skel || skel->boneCount == 0) {
                        ImGui::TextDisabled("(entity has no skeleton asset)");
                    } else {
                        auto pick = [&](uint32_t b) {
                            boneIdx = b;
                            ImGui::CloseCurrentPopup();
                        };
                        if (s_filter[0]) {
                            for (uint32_t b = 0; b < skel->boneCount; ++b) {
                                if (!skel->boneNames[b][0] || !icontains(skel->boneNames[b], s_filter)) continue;
                                char lbl[96]; snprintf(lbl, sizeof(lbl), "[%u] %s", b, skel->boneNames[b]);
                                if (ImGui::Selectable(lbl)) pick(b);
                            }
                        } else {
                            // Bone NAME is the tree-node label so a single click on
                            // the row selects it (IsItemClicked); the arrow /
                            // double-click toggles children. (Matches the Chain
                            // Physics picker to avoid overlapping-hitbox mis-picks.)
                            std::function<void(uint32_t)> drawTree = [&](uint32_t b) {
                                const char* nm = skel->boneNames[b][0] ? skel->boneNames[b] : "(unnamed)";
                                const bool leaf = children[b].empty();
                                ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth
                                                     | ImGuiTreeNodeFlags_OpenOnArrow
                                                     | ImGuiTreeNodeFlags_OpenOnDoubleClick;
                                if (leaf) f |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                                ImGui::PushID(static_cast<int>(b));
                                const bool open = ImGui::TreeNodeEx(nm, f);
                                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                                    pick(b);
                                if (open && !leaf) {
                                    for (uint32_t c : children[b]) drawTree(c);
                                    ImGui::TreePop();
                                }
                                ImGui::PopID();
                            };
                            for (uint32_t r : roots) drawTree(r);
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            };

            for (int i = 0; i < cc->count; ++i)
            {
                ImGui::PushID(i);
                auto& cap = cc->capsules[i];

                char label[32]; snprintf(label, sizeof(label), "Capsule %d", i);
                if (ImGui::CollapsingHeader(label, 0))
                {
                    ImGui::Checkbox("Enabled", &cap.enabled);
                    bonePicker("Bone A", cap.boneA);
                    ImGui::DragFloat3("Offset A", &cap.offsetA.x, 0.005f, -2.f, 2.f, "%.3f");
                    bonePicker("Bone B", cap.boneB);
                    ImGui::DragFloat3("Offset B", &cap.offsetB.x, 0.005f, -2.f, 2.f, "%.3f");
                    ImGui::DragFloat("Radius", &cap.radius, 0.005f, 0.01f, 1.0f, "%.3f");

                    if (ImGui::Button("Remove"))
                    {
                        for (int j = i; j < cc->count - 1; ++j)
                            cc->capsules[j] = cc->capsules[j + 1];
                        cc->count--;
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }

            if (cc->count < CapsuleColliderComponent::MAX_CAPSULES)
            {
                if (ImGui::Button("+ Add Capsule"))
                {
                    cc->capsules[cc->count] = {};
                    cc->count++;
                }
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<ChainPhysicsComponent>(e);
        });

    // ScriptComponent - a dynamic list of attached Lua scripts. The reflected
    // descriptor is empty (a static field list cannot express a vector); the
    // whole inspector is hand-drawn here: per-slot enabled + path (ILUA_PATH
    // drag-drop) + exposed-variable UI, with add/remove controls.
    RegisterReflectedComponent<ScriptComponent>("Script", /*priority*/ 50,
        [this](ScriptComponent& sc, World* w, Entity e) {
            DrawScriptComponentSlots(sc, w, e);
        });

    // AnimGraphComponent - opens its graph / animation set in their editors and
    // drives the live Bool / Float graph parameters once the instance has compiled.
    RegisterReflectedComponent<AnimGraphComponent>("Animation Graph", 32,
        [this](AnimGraphComponent& graph, World* world, Entity e) {
            if(ImGui::Button("Edit Graph"))Editor::AnimGraphEditor::Open(graph.graphPath);
            ImGui::SameLine();
            ImGui::BeginDisabled(graph.animSetPath.empty());
            if (ImGui::Button("Edit Animation Set")) Editor::AnimSetEditor::Open(graph.animSetPath);
            ImGui::EndDisabled();
            if (!world || !m_renderer || !m_renderer->GetAnimGraphSystem()) return;
            const auto* inst = m_renderer->GetAnimGraphSystem()->GetInstance(e);
            auto* params = world->GetComponent<AnimGraphParamsComponent>(e);
            if (!inst || !params) { ImGui::TextDisabled("Loading / compiling graph..."); return; }
            for (size_t p = 0; p < inst->definition->params.size(); ++p) {
                const auto& decl = inst->definition->params[p]; auto& v = params->values[p];
                if (decl.type == Anim::Graph::ParamType::Bool) { bool value = v.Bool(); if (ImGui::Checkbox(decl.name.c_str(), &value)) v = Anim::Graph::ParamValue::FromInt(value); }
                else if (decl.type == Anim::Graph::ParamType::Float) ImGui::DragFloat(decl.name.c_str(), &v.vector[0], .01f);
            }
        }, [](World& w, Entity e) { return w.HasComponent<SkeletonComponent>(e); });
    UseSerializerFingerprint<AnimGraphComponent>();
    // LocomotionComponent - blend-space samples need list editing + .ianim drops.
    RegisterReflectedComponent<LocomotionComponent>("Locomotion",
        /*priority*/ 33,
        [](LocomotionComponent& lc, World*, Entity)
        {
            static const char* kModeNames[] = { "1D (speed)", "2D (8-way: angle + speed)" };
            int mode = lc.mode;
            if (ImGui::Combo("Mode", &mode, kModeNames, 2))
                lc.mode = static_cast<uint8_t>(mode);
            const bool is2D = lc.mode == static_cast<uint8_t>(LocomotionMode::Blend2D);
            if (is2D)
                EditorUI::DragFloat("Direction (deg)", &lc.dirDeg, -180.f, 180.f,
                                   "%.0f  (0=F  90=R  ±180=B  -90=L)");
            ImGui::TextDisabled("smoothed: v=%.2f dir=%.0f  phase=%.2f  blends=%d",
                                lc.smoothedSpeed, lc.smoothedDir, lc.phase, lc.outCount);
            // locoWeight is the cross-fade against the Animator: 1 = the blend
            // space owns the body, 0 = an Animator one-shot (action) does.
            ImGui::TextDisabled("vs Animator: locoWeight=%.2f %s", lc.locoWeight,
                                lc.locoWeight > 0.999f ? "(blend space owns the pose)"
                              : lc.locoWeight < 0.001f ? "(Animator owns the pose)"
                                                       : "(cross-fading)");
            ImGui::SeparatorText(is2D ? "Samples (speed ring + angle spoke)"
                                      : "Samples (sorted by speed)");
            int removeAt = -1;
            for (int i = 0; i < lc.sampleCount &&
                            i < static_cast<int>(LocomotionComponent::MAX_SAMPLES); ++i)
            {
                ImGui::PushID(i);
                LocomotionSample& s = lc.samples[i];
                const char* label = s.animPath.empty()
                    ? "(drop .ianim here)"
                    : s.animPath.c_str();
                const float tailW = is2D ? 200.f : 130.f;
                ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x - tailW, 0.f));
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                        if (p->Data)
                        {
                            s.animPath.assign(static_cast<const char*>(p->Data));
                            s.clip = ~0u; s.pendingPacked = 0; // re-resolve
                        }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(60.f);
                ImGui::DragFloat("##spd", &s.speed, 0.05f, 0.f, 50.f, "v%.2f");
                if (is2D)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(64.f);
                    ImGui::DragFloat("##ang", &s.angleDeg, 1.f, -180.f, 180.f, "%.0f\xC2\xB0");
                }
                ImGui::SameLine();
                ImGui::TextDisabled(s.animPath.empty() ? " " : (s.clip != ~0u ? "OK" : ".."));
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeAt = i;
                ImGui::PopID();
            }
            if (removeAt >= 0)
            {
                for (int i = removeAt; i + 1 < lc.sampleCount; ++i)
                    lc.samples[i] = lc.samples[i + 1];
                lc.samples[--lc.sampleCount] = LocomotionSample{};
            }
            if (lc.sampleCount < static_cast<int>(LocomotionComponent::MAX_SAMPLES) &&
                ImGui::Button("+ Add Sample"))
                lc.samples[lc.sampleCount++] = LocomotionSample{};

            // 2D authoring shortcut: the 9-slot layout the resolver expects -
            // one idle at speed 0 (a 1-member ring covers every angle) plus 8
            // direction spokes on a single gait ring. Typing those angles into
            // the 64px drag one at a time is the slowest part of setting a
            // blend space up; paths are still dropped by hand afterwards.
            if (is2D)
            {
                ImGui::SameLine();
                if (ImGui::Button("+ 8-way preset"))
                {
                    static const float kSpokes[8] =
                        { 0.f, 45.f, 90.f, 135.f, 180.f, -135.f, -90.f, -45.f };
                    // Ring speed defaults to the character's walk speed; 1.1 is
                    // only a starting point - match it to PlayerComponent.walkSpeed
                    // or the feet will slide.
                    const float ring = 1.1f;
                    lc.mode        = static_cast<uint8_t>(LocomotionMode::Blend2D);
                    lc.sampleCount = 0;
                    lc.samples[lc.sampleCount++] = LocomotionSample{};   // idle @ v0
                    for (float a : kSpokes)
                    {
                        LocomotionSample s{};
                        s.speed    = ring;
                        s.angleDeg = a;
                        lc.samples[lc.sampleCount++] = s;
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Replace the sample list with idle @ v0 + 8 "
                                      "spokes @ v1.1\n(0=F 45=FR 90=R 135=BR 180=B "
                                      "-135=BL -90=L -45=FL).\nDrop a .ianim on each "
                                      "row, then match the ring speed to walk speed.");
            }
        });

    // LocomotionTransitionComponent - start/stop/pivot/turn one-shots that the
    // blend space itself cannot express. Same drop-a-.ianim authoring as the
    // Locomotion samples, plus a kind and a direction spoke.
    RegisterReflectedComponent<LocomotionTransitionComponent>("Loco Transitions",
        /*priority*/ 32,
        [](LocomotionTransitionComponent& tc, World*, Entity)
        {
            static const char* kKindNames[] = { "Start", "Stop", "Pivot", "Turn In Place",
                                                "Jump Start", "Jump Loop", "Jump End" };
            ImGui::TextDisabled("active=%d  weight=%.2f  %s  cooldown=%.2f",
                                tc.active, tc.weight,
                                tc.releasing ? "releasing" : "holding",
                                tc.cooldownTimer);
            ImGui::SeparatorText("Transitions (kind + direction spoke)");

            int removeAt = -1;
            for (int i = 0; i < tc.count &&
                            i < LocomotionTransitionComponent::MAX_TRANSITIONS; ++i)
            {
                ImGui::PushID(i);
                LocoTransition& T = tc.transitions[i];
                const char* label = T.animPath.empty()
                    ? "(drop .ianim here)" : T.animPath.c_str();
                ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x - 285.f, 0.f));
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                        if (p->Data)
                        {
                            T.animPath.assign(static_cast<const char*>(p->Data));
                            T.clip = ~0u; T.pendingPacked = 0;   // re-resolve
                        }
                    ImGui::EndDragDropTarget();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(110.f);
                int kind = T.kind;
                if (ImGui::Combo("##kind", &kind, kKindNames, 7))
                    T.kind = static_cast<uint8_t>(kind);
                ImGui::SameLine();
                ImGui::Checkbox("##loop", &T.looping);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Loop forever instead of auto-releasing.\n"
                                      "Required on Jump Loop — it is a STATE held\n"
                                      "for the whole airborne time, not a one-shot.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(64.f);
                ImGui::DragFloat("##ang", &T.angleDeg, 1.f, -180.f, 180.f, "%.0f\xC2\xB0");
                ImGui::SameLine();
                ImGui::TextDisabled(T.animPath.empty() ? " " : (T.clip != ~0u ? "OK" : ".."));
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) removeAt = i;
                ImGui::PopID();
            }
            if (removeAt >= 0)
            {
                for (int i = removeAt; i + 1 < tc.count; ++i)
                    tc.transitions[i] = tc.transitions[i + 1];
                tc.transitions[--tc.count] = LocoTransition{};
            }
            if (tc.count < LocomotionTransitionComponent::MAX_TRANSITIONS &&
                ImGui::Button("+ Add Transition"))
                tc.transitions[tc.count++] = LocoTransition{};

            ImGui::SameLine();
            if (ImGui::Button("+ 8-way Start/Stop preset"))
            {
                static const float kSpokes[8] =
                    { 0.f, 45.f, 90.f, 135.f, 180.f, -135.f, -90.f, -45.f };
                tc.count = 0;
                for (int k = 0; k < 2; ++k)          // Start, then Stop
                    for (float a : kSpokes)
                    {
                        if (tc.count >= LocomotionTransitionComponent::MAX_TRANSITIONS) break;
                        LocoTransition T{};
                        T.kind     = static_cast<uint8_t>(k == 0 ? LocoTransitionKind::Start
                                                                 : LocoTransitionKind::Stop);
                        T.angleDeg = a;
                        tc.transitions[tc.count++] = T;
                    }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Replace the list with 8 Start + 8 Stop slots at\n"
                                  "0/45/90/135/180/-135/-90/-45. Drop the matching\n"
                                  "_Start / _Stop .ianim on each row.\n"
                                  "Pivot and Turn In Place rows are added by hand.");
        });

    // AnimLayerComponent - masked overlay layers (aim/attack over locomotion).
    RegisterReflectedComponent<AnimLayerComponent>("Anim Layers",
        /*priority*/ 32,
        [](AnimLayerComponent& lc, World* world, Entity entity)
        {
            static const char* kMaskNames[] = { "Full Body", "Upper Body", "Lower Body", "Custom Root" };
            int removeAt = -1;
            for (int i = 0; i < lc.layerCount && i < AnimLayerComponent::MAX_LAYERS; ++i)
            {
                ImGui::PushID(i);
                AnimLayer& L = lc.layers[i];
                char hdr[96];
                snprintf(hdr, sizeof(hdr), "Layer %d: %s###layer%d", i,
                         L.animPath.empty() ? "(empty)" : L.animPath.c_str(), i);
                if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    const char* label = L.animPath.empty()
                        ? "(drop .ianim here)"
                        : L.animPath.c_str();
                    ImGui::Button(label, ImVec2(-1.f, 0.f));
                    if (ImGui::BeginDragDropTarget())
                    {
                        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
                            if (p->Data)
                            {
                                L.animPath.assign(static_cast<const char*>(p->Data));
                                L.clip = ~0u; L.pendingPacked = 0; L.time = 0.f;
                            }
                        ImGui::EndDragDropTarget();
                    }
                    EditorUI::DragFloat("Weight", &L.weight, 0.f, 1.f);
                    ImGui::DragFloat("Speed", &L.speed, 0.01f, 0.f, 5.f);
                    ImGui::Checkbox("Looping", &L.looping); ImGui::SameLine();
                    ImGui::Checkbox("Playing", &L.playing); ImGui::SameLine();
                    ImGui::TextDisabled("t=%.2f %s", L.time, L.clip != ~0u ? "OK" : "..");
                    int mm = L.maskMode;
                    if (ImGui::Combo("Mask", &mm, kMaskNames, 4))
                        L.maskMode = static_cast<uint8_t>(mm);
                    if (L.maskMode == static_cast<uint8_t>(BoneMaskMode::CustomRoot))
                    {
                        char rootBuf[64];
                        snprintf(rootBuf, sizeof(rootBuf), "%s", L.customRootBone.c_str());
                        if (ImGui::InputText("Root Bone", rootBuf, sizeof(rootBuf)))
                            L.customRootBone = rootBuf;
                    }
                    // The bones this mask drives, on the model's skeleton (the Skeleton
                    // Editor's mask preview - the same BuildBoneMask).
                    if (L.maskMode != static_cast<uint8_t>(BoneMaskMode::FullBody))
                    {
                        const std::string model = world ? ModelPathOfEntity(*world, entity) : std::string();
                        ImGui::BeginDisabled(model.empty());
                        if (ImGui::SmallButton("Preview Mask"))
                            Editor::SkeletonEditorWindow::Get().RequestMaskPreview(model, L.maskMode, L.customRootBone);
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                            ImGui::SetTooltip(model.empty() ? "The entity was not spawned from a model (.iscn)."
                                                            : "Show the bones this mask drives in the Skeleton Editor.");
                    }
                    if (ImGui::SmallButton("Remove Layer")) removeAt = i;
                }
                ImGui::PopID();
            }
            if (removeAt >= 0)
            {
                for (int i = removeAt; i + 1 < lc.layerCount; ++i)
                    lc.layers[i] = lc.layers[i + 1];
                lc.layers[--lc.layerCount] = AnimLayer{};
            }
            if (lc.layerCount < AnimLayerComponent::MAX_LAYERS &&
                ImGui::Button("+ Add Layer"))
                lc.layers[lc.layerCount++] = AnimLayer{};
        });

    // SocketComponent - bone combos need a live skeleton lookup; full custom postDraw.
    RegisterReflectedComponent<SocketComponent>("Sockets",
        /*priority*/ 48,
        [renderer](SocketComponent& scRef, World* world, Entity entity)
        {
            auto* sc = &scRef;

            const SkeletonAsset* skel = nullptr;
            if (renderer && world)
            {
                const SkeletonComponent* skc = world->GetComponent<SkeletonComponent>(entity);
                if (skc && skc->assetIndex != kInvalidSkeletonIndex
                    && skc->assetIndex < renderer->GetSkeletonRegistry().Count())
                    skel = &renderer->GetSkeletonRegistry().Get(skc->assetIndex);
            }

            if (!skel)
            {
                ImGui::TextDisabled("(entity has no SkeletonComponent — add one first)");
                return;
            }

            auto boneNameAt = [&](uint32_t idx) -> const char* {
                if (idx < skel->boneCount && skel->boneNames[idx][0])
                    return skel->boneNames[idx];
                return "(?)";
            };

            for (uint32_t i = 0; i < sc->count; ++i)
            {
                ImGui::PushID(static_cast<int>(i));
                auto& s = sc->sockets[i];

                // A socket derived from the model's SO records belongs to the model -
                // shown, not edited here (the Skeleton Editor edits the model; an own
                // socket of the same name wins).
                if (s.flags & SocketComponent::kDerived)
                {
                    ImGui::BulletText("%s", s.name);
                    ImGui::SameLine();
                    ImGui::TextDisabled("[model]  bone %s%s", s.boneName[0] ? s.boneName : boneNameAt(s.boneIndex),
                                        (s.flags & SocketComponent::kBoneMissing) ? " (missing)" : "");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("From the model's socket records - edit it in the Skeleton Editor.\n"
                                          "Add an own socket of this name here to override it on this entity.");
                    ImGui::PopID();
                    continue;
                }

                // ### (triple hash): ImGui ignores the visible label for the ID, so typing a name does not reset the expanded state.
                char hdr[96];
                snprintf(hdr, sizeof(hdr), "%u: %s###sock%u",
                         i, s.name[0] ? s.name : "(unnamed)", i);

                if (ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen))
                {
                    // Renaming changes the hash every follower resolves against,
                    // so it invalidates their cached indices just like a remove.
                    if (ImGui::InputText("Name", s.name, sizeof(s.name)))
                        ++sc->generation;

                    // The bone by name: what is saved, and what a re-import that
                    // reorders the skeleton resolves again.
                    const char* curBone = (s.flags & SocketComponent::kBoneMissing) ? s.boneName
                                          : boneNameAt(s.boneIndex);
                    if (ImGui::BeginCombo("Bone", curBone))
                    {
                        for (uint32_t bi = 0; bi < skel->boneCount; ++bi)
                        {
                            const bool sel = (s.boneIndex == bi);
                            char row[96];
                            snprintf(row, sizeof(row), "[%u] %s", bi, boneNameAt(bi));
                            if (ImGui::Selectable(row, sel))
                            {
                                s.boneIndex = bi;
                                const size_t n = strnlen(skel->boneNames[bi], sizeof(s.boneName) - 1);
                                std::memcpy(s.boneName, skel->boneNames[bi], n);
                                s.boneName[n] = '\0';
                                s.flags &= static_cast<uint8_t>(~SocketComponent::kBoneMissing);
                            }
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (s.flags & SocketComponent::kBoneMissing)
                        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Bone '%s' is not in this skeleton.", s.boneName);

                    DirectX::XMFLOAT3 offPos{
                        s.localOffset._41, s.localOffset._42, s.localOffset._43 };
                    bool offChanged = false;
                    offChanged |= ImGui::DragFloat3("Offset", &offPos.x, 0.01f, -10.f, 10.f, "%.3f");
                    offChanged |= ImGui::DragFloat3("Rotation (deg)", &s.rotationEulerDeg.x,
                                                    0.5f, -360.f, 360.f, "%.2f");
                    if (offChanged)
                    {
                        using namespace DirectX;
                        const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                            XMConvertToRadians(s.rotationEulerDeg.x),
                            XMConvertToRadians(s.rotationEulerDeg.y),
                            XMConvertToRadians(s.rotationEulerDeg.z));
                        XMMATRIX M = XMMatrixRotationQuaternion(q);
                        M.r[3] = XMVectorSet(offPos.x, offPos.y, offPos.z, 1.0f);
                        XMStoreFloat4x4(&s.localOffset, M);
                    }

                    if (ImGui::Button("Remove"))
                    {
                        for (uint32_t j = i; j + 1 < sc->count; ++j)
                            sc->sockets[j] = sc->sockets[j + 1];
                        sc->count--;
                        ++sc->generation;   // every later socket just shifted down
                        ImGui::PopID();
                        break;
                    }
                }
                ImGui::PopID();
            }

            if (sc->count < SocketComponent::MAX)
            {
                // An own socket: after the entity's others, ahead of the model's.
                if (ImGui::Button("+ Add Socket"))
                    sc->Add(nullptr, 0);
            }
            else
                ImGui::TextDisabled("(max %u sockets)", SocketComponent::MAX);
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.HasComponent<SkeletonComponent>(e);
        });

    // FollowEntity - entity-handle picker; needs World context, custom postDraw.
    RegisterReflectedComponent<FollowEntityComponent>("Follow Entity",
        /*priority*/ 49,
        [](FollowEntityComponent& feRef, World* world, Entity self)
        {
            if (!world) return;
            auto* fe = &feRef;

            // Drag an entity row from the Hierarchy onto the field; BindAndStamp
            // adds a GuidComponent to the target if it doesn't have one.
            Editor::DrawAttachmentRef("Target", *world, self, fe->target);

            const Entity t = fe->target.Resolve(*world);
            if (t == self && t != NullEntity)
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Target is self.");
            else if (t != NullEntity && !world->HasComponent<GlobalTransform>(t))
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Target has no GlobalTransform.");

            ImGui::TextDisabled("Offset lives in this entity's Transform,");
            ImGui::TextDisabled("interpreted in the target's space.");
        });

    // FollowSocket - entity picker + socket combo, both World-context heavy.
    RegisterReflectedComponent<FollowSocketComponent>("Follow Socket",
        /*priority*/ 50,
        [](FollowSocketComponent& fsRef, World* world, Entity self)
        {
            if (!world) return;
            auto* fs = &fsRef;

            // Drag an entity row from the Hierarchy onto the field; BindAndStamp
            // adds a GuidComponent to the target if it doesn't have one.
            Editor::DrawAttachmentRef("Target", *world, self, fs->target);

            const Entity target = fs->target.Resolve(*world);
            const SocketComponent* sc = (target != NullEntity)
                ? world->GetComponent<SocketComponent>(target) : nullptr;

            if (target == self && target != NullEntity)
            {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Target is self.");
            }
            else if (!sc || sc->count == 0)
            {
                ImGui::TextDisabled("(target has no sockets — add SocketComponent entries first)");
            }
            else
            {
                auto socketLabel = [sc](uint32_t i) -> const char* {
                    if (i >= sc->count) return "(out of range)";
                    return sc->sockets[i].name[0] ? sc->sockets[i].name : "(unnamed)";
                };

                // Show the socket the name hash actually resolves to, not the
                // possibly-stale cached index.
                const uint32_t cur = (fs->socketNameHash != 0)
                    ? sc->FindByName(fs->socketNameHash) : fs->socketIndex;

                if (cur == SocketComponent::kNotFound)
                    ImGui::TextColored(ImVec4(1.f, 0.5f, 0.2f, 1.f),
                        "Socket no longer exists on target — pick another.");

                const char* curName = (cur == SocketComponent::kNotFound)
                    ? "(missing)" : socketLabel(cur);
                if (ImGui::BeginCombo("Socket", curName))
                {
                    for (uint32_t i = 0; i < sc->count; ++i)
                    {
                        const bool sel = (cur == i);
                        char row[96];
                        snprintf(row, sizeof(row), "%u: %s", i, socketLabel(i));
                        if (ImGui::Selectable(row, sel))
                        {
                            // Name hash is the authority; index/gen are the cache.
                            fs->socketNameHash  = SocketComponent::HashName(sc->sockets[i].name);
                            fs->socketIndex     = i;
                            fs->cachedSocketGen = sc->generation;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::TextDisabled("Offset lives in this entity's Transform,");
            ImGui::TextDisabled("interpreted in the socket's space.");

            // Both writers would fight over GlobalTransform; FollowSystem wins,
            // so the Parent link silently does nothing except corrupt any
            // grandchildren Propagate composes before FollowSystem overwrites.
            if (world && world->GetComponent<Parent>(self))
            {
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.2f, 1.f),
                    "Warning: also has a Parent — remove it.");
            }
        });

    // ---- Tag-only registrations ----
    RegisterComponentTag<GlobalTransform>      ("Global Transform");
    RegisterComponentTag<Parent>               ("Parent");
    RegisterComponentTag<Children>             ("Children");
    RegisterComponentTag<SceneNodeTag>         ("Scene Node");
    RegisterComponentTag<RenderLayer>          ("Render Layer");
    RegisterComponentTag<WorldAabb>            ("World AABB");
    // Mesh Section (runtime) - MeshLibRef, one material section of a loaded
    // .meshlib. Labelled "(runtime)" because nothing serializes it: the entity
    // gets it from the .iscn that spawned it (the serializer tag "MeshRef" is a
    // different component, MeshSourcePath, and stores only a path). A
    // display-only editor section so it can surface the section's vertex
    // channels. requires_ (entity already has the component) keeps it out of
    // the Add Component menu - an empty MeshLibRef has no library to bind.
    RegisterComponentEditor<MeshLibRef>("Mesh Section (runtime)",
        [this](void* /*data*/, World* w, Entity e)
        {
            DrawMeshChannelInfo(m_renderer ? m_renderer->GetMeshLibrary() : nullptr, w, e);
        },
        /*priority*/ 30,
        /*requires_*/ [](World& w, Entity e){ return w.HasComponent<MeshLibRef>(e); });
    // Static Mesh: one mesh of a model placed as an object. MeshInstanceSystem
    // derives the section children from these fields whenever they change;
    // nothing here touches the sections.
    RegisterComponentEditor<StaticMeshComponent>("Static Mesh",
        [this](void* data, World* w, Entity e)
        {
            if (data) DrawStaticMeshInspector(*static_cast<StaticMeshComponent*>(data), w, e);
        },
        /*priority*/ 25);
    RegisterComponentTag<MeshSectionOf>        ("Static Mesh Section (derived)");
    RegisterComponentTag<MeshSourcePath>       ("Mesh Source Path");
    RegisterComponentTag<MaterialComponent>    ("Material");
    RegisterComponentTag<MeshSkinnedComponent>    ("Skinned Mesh");
    RegisterComponentTag<SkinningOutputComponent> ("Skinning Output");

    // Volumetric Light - opt-in marker for Spot/Point lights to contribute fog froxels; needs LightData.
    RegisterReflectedComponent<VolumetricLightComponent>("Volumetric Light",
        /*priority*/ 60,
        /*postDraw*/ nullptr,
        [](World& w, Entity e) -> bool
        {
            return w.GetComponent<LightData>(e) != nullptr;
        });

    // Reflection Probe - descriptor for extents/realtime/interval; postDraw adds the Bake button (needs Renderer access).
    RegisterReflectedComponent<ReflectionProbeComponent>("Reflection Probe",
        /*priority*/ 55,
        [this](ReflectionProbeComponent& probe, World* w, Entity e)
        {
            ImGui::Separator();
            // Master switch, mirrored here from Debug > Scene Gizmos > Reflection
            // Probe Viz - same bool, and this is where people actually look for
            // it. Effective visibility is master AND the per-probe "Show Viz
            // Sphere" above; OnUIRender pushes the product onto the entity's
            // VisibilityComponent every frame.
            ImGui::Checkbox("Show ALL Probe Viz Spheres", &m_showProbeVizSpheres);
            if (!m_showProbeVizSpheres)
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "Master off - every probe ball is hidden.");
            else if (!probe.showVizSphere)
                ImGui::TextDisabled("This probe's ball is hidden (Show Viz Sphere is off).");
            // The cyan influence volume / orange proxy / white capture point are
            // debug WIREFRAMES, not this mesh: they come from DebugWirePass and
            // survive hiding the ball on purpose (you tune influence bounds with
            // the ball out of the way). Say where their switch is - "I hid the
            // probe and a box is still there" is otherwise a dead end.
            ImGui::TextDisabled("Influence/proxy wireframes: Debug > Wireframes >");
            ImGui::TextDisabled("Show Reflection Probes (separate toggle).");
            ImGui::Separator();
            // Proxy volume binding - a shareable ProxyVolumeComponent entity.
            // Null -> the upload synthesises a proxy from the influence volume.
            if (w) Editor::DrawAttachmentRef("Proxy Volume", *w, e, probe.proxyVolume);
            {
                int prio = static_cast<int>(probe.priority);
                if (ImGui::SliderInt("Priority", &prio, 0, 255))
                    probe.priority = static_cast<uint8_t>(prio);
                // Face resolution is a power-of-two choice, not a free scalar: the
                // prefilter mip chain is log2(size)+1 deep and the Karis sample table
                // is indexed per mip, so an arbitrary value has no valid chain. An
                // off-list value from an older scene is shown verbatim until the user
                // picks a listed one.
                static constexpr uint16_t kFaceResOptions[] = { 64, 128, 256, 512, 1024 };
                char faceResLabel[16];
                snprintf(faceResLabel, sizeof(faceResLabel), "%u",
                         static_cast<uint32_t>(probe.faceResolution));
                if (ImGui::BeginCombo("Face Resolution", faceResLabel))
                {
                    for (uint16_t opt : kFaceResOptions)
                    {
                        char row[16];
                        snprintf(row, sizeof(row), "%u", static_cast<uint32_t>(opt));
                        const bool sel = (probe.faceResolution == opt);
                        if (ImGui::Selectable(row, sel))
                            probe.faceResolution = opt;
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::TextDisabled("Not honoured yet - every probe bakes at %ux%u.",
                                    ReflectionProbeCapturePass::kFaceSize,
                                    ReflectionProbeCapturePass::kFaceSize);
            }
            ImGui::Separator();
            if (probe.cubemapSlice == ReflectionProbeComponent::kInvalidSlice)
                ImGui::Text("Cubemap Slice: <unassigned>");
            else
                ImGui::Text("Cubemap Slice: %u", probe.cubemapSlice);

            const bool baked = probe.IsBaked();
            ImGui::Text("Status: %s", baked ? "Baked" : "Not Baked");

            const bool canBake = m_renderer
                              && probe.cubemapSlice != ReflectionProbeComponent::kInvalidSlice;
            if (!canBake) ImGui::BeginDisabled();
            if (ImGui::Button("Bake This Probe"))
            {
                probe.RequestRebake();
                m_renderer->BakeProbe(probe.cubemapSlice);
            }
            if (!canBake) ImGui::EndDisabled();
            if (probe.NeedsRebake())
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "(pending)");
            }
            if (probe.realtime)
            {
                ImGui::TextDisabled("Auto-rebake every N frames (60 = 1s @ 60fps).");
                ImGui::TextDisabled("Each tick costs one full bake (~6 face draws + prefilter).");
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool
        {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // Proxy Volume - parallax proxy geometry (shape/offset/extents), shareable
    // across probes via the Reflection Probe's Proxy Volume picker. Available on
    // any entity; add via the component menu, place with the transform gizmo.
    RegisterReflectedComponent<ProxyVolumeComponent>("Proxy Volume", /*priority*/ 54);

    // DDGI Volume - DDGI probe ray-trace volume; needs DXR. postDraw shows readiness + a probe-density warning.
    RegisterReflectedComponent<DDGIVolumeComponent>("DDGI Volume",
        /*priority*/ 56,
        [this](DDGIVolumeComponent& v, World*, Entity)
        {
            ImGui::Separator();
            if (m_renderer)
            {
                ImGui::TextDisabled(m_renderer->IsDDGIReady()
                    ? "DXR ready — volume active."
                    : "DXR unavailable — Sky IBL fallback only.");
            }
            const uint32_t totalProbes = v.probeCountsX * v.probeCountsY * v.probeCountsZ;
            ImGui::TextDisabled("Total probes: %u", totalProbes);
            if (totalProbes > 4096)
                ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1),
                                   "(>4096 probes - atlas memory ~%u KB)",
                                   (totalProbes * 8 * 8 * 4) / 1024);
        },
        /*requires*/ [](World& w, Entity e) -> bool
        {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // IndirectLightingSettings - scene-singleton DDGI/IBL fallback knobs (first matching entity wins).
    RegisterReflectedComponent<IndirectLightingSettingsComponent>(
        "Indirect Lighting Settings", /*priority*/ 57);

    // Decal - clustered compute decal; references a shared DecalMaterialAsset (managed via Tools > Decal Materials).
    RegisterReflectedComponent<DecalComponent>("Decal",
        /*priority*/ 58,
        [this](DecalComponent& dcRef, World* w, Entity ent)
        {
            auto* dc = &dcRef;
            if (!m_renderer)
            {
                ImGui::TextDisabled("(Renderer not wired — cannot resolve material library.)");
                return;
            }

            // Unified-material path: an entity Material with Domain = Decal
            // drives this decal (and wins over the library asset picked below).
            if (w)
            {
                if (const MaterialComponent* entMc = w->GetComponent<MaterialComponent>(ent);
                    entMc && entMc->IsDecalDomain())
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.f),
                        "Driven by entity Material (Domain = Decal)");
                    ImGui::TextDisabled(
                        "Edit look in the Material section below; the legacy\n"
                        "library picker here is ignored while that material exists.");
                    ImGui::Separator();
                }
                else
                {
                    ImGui::TextDisabled(
                        "Tip: Add Component > Rendering > Material, set its\n"
                        "Domain to Decal, to author this decal as a .imat.");
                }
            }

            auto& lib = m_renderer->GetDecalMaterialLibrary();
            const auto& assets = lib.GetAll();

            // ---- Material picker: current asset name + combo to swap/clear ----
            const char* currentName = dc->material
                ? dc->material->name.c_str()
                : "(none)";

            if (ImGui::BeginCombo("Material", currentName))
            {
                // "None" clears the assignment - the entity stops contributing a decal.
                if (ImGui::Selectable("(none)", !dc->material))
                    dc->material.reset();

                ImGui::Separator();

                // List every asset. Sort for stable ordering.
                std::vector<const std::string*> names;
                names.reserve(assets.size());
                for (const auto& [name, _] : assets) names.push_back(&name);
                std::sort(names.begin(), names.end(),
                          [](const std::string* a, const std::string* b){ return *a < *b; });

                for (const std::string* namePtr : names)
                {
                    const bool selected =
                        dc->material && dc->material->name == *namePtr;
                    if (ImGui::Selectable(namePtr->c_str(), selected))
                        dc->material = lib.Find(*namePtr);
                }
                if (assets.empty())
                    ImGui::TextDisabled("(library empty — open Tools → Decal Materials)");
                ImGui::EndCombo();
            }

            // Shortcut button: open the Decal Materials window for editing
            // the selected asset.
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit..."))
            {
                m_showDecalMaterials = true;
                if (dc->material) m_selectedDecalMatName = dc->material->name;
            }

            if (!dc->material)
            {
                ImGui::TextDisabled("No material assigned — decal is inactive.");
                return;
            }

            ImGui::Separator();

            // ---- Per-instance state ----
            EditorUI::DragFloat("Fade Alpha", &dc->fadeAlpha, 0.0f, 1.0f,
                               "%.2f  (multiplies asset opacity)");

            // Lifetime: -1 = static, >= 0 = dynamic (drag for remaining seconds + fade + destroy-on-expire).
            bool isDynamic = (dc->lifetime >= 0.0f);
            if (ImGui::Checkbox("Dynamic", &isDynamic))
                dc->lifetime = isDynamic ? 5.0f : -1.0f;
            if (isDynamic)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.f);
                ImGui::DragFloat("##lifetime", &dc->lifetime, 0.1f, 0.0f, 600.0f,
                                 "Lifetime: %.1f s remaining");
                ImGui::DragFloat("Fade-out", &dc->fadeOutDuration, 0.05f, 0.0f, 60.0f,
                                 "%.2f s  (0 = instant pop)");
                ImGui::Checkbox("Destroy Entity on Expire", &dc->destroyEntityOnExpire);
                if (!dc->destroyEntityOnExpire)
                    ImGui::TextDisabled("Only the DecalComponent is removed; entity stays alive.");
            }
            else
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(static — never expires)");
            }

            // Tint override: w>0 means use override; UI splits into toggle + color picker.
            ImGui::Separator();
            bool overrideOn = (dc->tintOverride.w > 0.0f);
            if (ImGui::Checkbox("Override Tint", &overrideOn))
            {
                if (overrideOn)
                {
                    // Seed with asset tint so the editor doesn't show pure black.
                    dc->tintOverride = dc->material->baseColorTint;
                    if (dc->tintOverride.w <= 0.0f) dc->tintOverride.w = 1.0f;
                }
                else
                {
                    dc->tintOverride.w = 0.0f;
                }
            }
            if (overrideOn)
            {
                EditorUI::ColorEdit4("Override Color", &dc->tintOverride.x,
                                  ImGuiColorEditFlags_AlphaBar);
            }

            // ---- Bound-texture status (read-only) ----
            ImGui::Separator();
            const auto& a = *dc->material;
            const bool fullyResolved = a.IsFullyResolved();
            ImGui::TextColored(
                fullyResolved ? ImVec4(0.4f, 0.9f, 0.4f, 1.f)
                              : ImVec4(0.9f, 0.8f, 0.3f, 1.f),
                fullyResolved ? "Material resolved."
                              : "Material textures loading...");
            // Per-slot status: "-"=unused, "loading"=path set but not GPU-resident, "bound"=ready.
            for (uint32_t s = 0; s < Resource::DecalMaterialAsset::SLOT_COUNT; ++s)
            {
                const auto slot = static_cast<Resource::DecalMaterialAsset::TextureSlot>(s);
                const char* st = a.texBindless[s] >= 0    ? "bound"
                               : a.texPaths[s].empty()   ? "-"
                                                          : "loading";
                ImGui::TextDisabled("  %-13s: %s",
                    Resource::DecalMaterialAsset::SlotLabel(slot), st);
            }
        },
        /*requires*/ [](World& w, Entity e) -> bool {
            return w.GetComponent<GlobalTransform>(e) != nullptr;
        });

    // Post-Process Volume - bounds come from the Transform; the look is a shared
    // PostProcessProfile resource edited inline via the override widgets.
    RegisterReflectedComponent<ECS::PostProcessVolumeComponent>("Post-Process Volume",
        /*priority*/ 55,
        [](ECS::PostProcessVolumeComponent& vc, World*, Entity)
        {
            static constexpr const char* kProfFilter =
                "Post-Process Profile (*.ppprofile)\0*.ppprofile\0All Files\0*.*\0\0";

            auto& ps = PostProcess::ProfileSystem::Get();

            ImGui::Separator();
            ImGui::TextDisabled("Profile");

            if (vc.profilePath.empty())
                ImGui::TextDisabled("(runtime profile — not saved as an asset)");
            else
                ImGui::TextWrapped("Asset: %s", vc.profilePath.c_str());

            PostProcess::PostProcessProfile* prof = ps.Get(vc.profile);

            if (!prof)
            {
                if (ImGui::Button("New Profile"))
                {
                    vc.profile = ps.CreateRuntime();
                    vc.profilePath.clear();
                    prof = ps.Get(vc.profile);
                }
                ImGui::SameLine();
            }
            if (ImGui::Button("Load .ppprofile..."))
            {
                std::string path = OpenFileDialog(kProfFilter, nullptr);
                if (!path.empty())
                {
                    // profilePath is serialized into the .iscene; store it relative.
                    PostProcess::ProfileHandle h = ps.Acquire(path);
                    if (h.IsValid())
                    {
                        vc.profile     = h;
                        vc.profilePath = Sys::RelativizeAssetPath(path);
                        prof           = ps.Get(h);
                    }
                }
            }
            if (prof)
            {
                ImGui::SameLine();
                if (ImGui::Button("Save As..."))
                {
                    std::string path = SaveFileDialog(kProfFilter, "ppprofile", nullptr);
                    if (!path.empty() && ps.Save(vc.profile, path))
                        vc.profilePath = Sys::RelativizeAssetPath(path);
                }

                DrawProfileInspector(*prof);
            }
            else
            {
                ImGui::TextDisabled("No profile — create or load one to edit the look.");
            }
        });

    // ParticleRenderComponent - shared appearance. postDraw: texture badge/slot,
    // orientation combo, TAA/lit toggles, custom-shader slots.
    RegisterReflectedComponent<ParticleRenderComponent>("Particle Render",
        /*priority*/ 60,
        [](ParticleRenderComponent& rc, World* w, Entity e)
        {
            // Mesh-like split: ParticleRender = SHAPE-side appearance,
            // Material = LOOK. A material on this entity supplies blend
            // (unless Opaque = unset) + the sprite texture (Base Color when
            // not the engine default); its custom shader additionally takes
            // over PS/VS/lit + b8 params. Mesh-bearing entities keep their
            // material for the mesh.
            if (w)
            {
                auto* pm  = w->GetPool<MaterialComponent>();
                auto* pmr = w->GetPool<MeshLibRef>();
                MaterialComponent* mc = pm ? pm->Get(e) : nullptr;
                if (mc && pmr && pmr->Get(e)) mc = nullptr;   // mesh's material
                if (mc && mc->useCustomShader && !mc->customShaderPath.empty())
                {
                    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f),
                        "Look driven by the Material component.");
                    ImGui::TextDisabled(
                        "Shader/blend/lit/params come from the Material inspector;\n"
                        "this component's custom-shader/blend fields are ignored.");
                }
                else if (mc)
                {
                    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f),
                        "Material supplies blend + texture.");
                    ImGui::TextDisabled(
                        "Blend: material (Opaque = use this component's).  Texture:\n"
                        "material Base Color when set (non-default).");
                }
            }

            auto hlslDrop = [](const char* label, std::string& path) {
                char buf[256] = {};
                const size_t copy = (std::min)(path.size(), sizeof(buf) - 1);
                std::memcpy(buf, path.data(), copy);
                buf[copy] = '\0';
                if (ImGui::InputText(label, buf, sizeof(buf)))
                    path = buf;
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IHLSL_PATH"))
                        path = static_cast<const char*>(p->Data);
                    ImGui::EndDragDropTarget();
                }
            };

            // ---- Texture: thumbnail + path field (drop an .itex from the Asset Browser) ----
            if (rc.textureGpuHandle != 0)
            {
                ImGui::Image(ImTextureRef(static_cast<ImTextureID>(rc.textureGpuHandle)),
                             ImVec2(48, 48));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            {
                char tbuf[256] = {};
                const size_t copy = (std::min)(rc.texturePath.size(), sizeof(tbuf) - 1);
                std::memcpy(tbuf, rc.texturePath.data(), copy);
                tbuf[copy] = '\0';
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::InputText("Texture", tbuf, sizeof(tbuf)))
                {
                    rc.texturePath        = tbuf;
                    rc.textureBindlessIdx = -1;   // force re-resolve in CollectEmitters
                    rc.textureGpuHandle   = 0;
                }
                if (ImGui::BeginDragDropTarget())
                {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ITEX_PATH"))
                    {
                        rc.texturePath        = static_cast<const char*>(p->Data);
                        rc.textureBindlessIdx = -1;
                        rc.textureGpuHandle   = 0;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::TextDisabled("idx=%d  (drag a texture here)", rc.textureBindlessIdx);
            }
            ImGui::EndGroup();

            // ---- Orientation: billboard (faces camera) vs fixed-plane quad ----
            {
                const char* kOrientNames[] = {
                    "Billboard (faces camera)",
                    "Horizontal (flat, +Y up)",
                    "Vertical (upright, +Z)",
                    "Velocity Stretched (sparks)"
                };
                int oi = static_cast<int>(rc.orientation);
                if (ImGui::Combo("Orientation", &oi, kOrientNames, IM_ARRAYSIZE(kOrientNames)))
                    rc.orientation = static_cast<ParticleOrientation>(oi);
            }

            ImGui::Checkbox("Exclude from TAA (sharp VFX)", &rc.excludeFromTAA);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ON: stamp a stencil bit so TAA keeps these particles sharp "
                                  "(no ghost/smear). Turn OFF for slow soft smoke that benefits "
                                  "from temporal anti-aliasing.");
            ImGui::Checkbox("Lit (forward sun + ambient)", &rc.lit);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Diffuse-lit by the sun + sky ambient using a spherical "
                                  "billboard normal. For soft/dust/smoke — treats the color as "
                                  "albedo. Leave OFF for emissive VFX (fire, sparks, glow).");

            // Second opt-in, gated on Lit - the unlit path is an emissive
            // composite with no direct term to shadow.
            if (!rc.lit) ImGui::BeginDisabled();
            ImGui::Checkbox("Receive Shadow", &rc.receiveShadow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Sample the sun's cascade + per-object shadow on the lit path,\n"
                    "so these particles go dark under a canopy.\n\n"
                    "NOT implied by Lit, and OFF by default, because the right\n"
                    "answer differs per emitter: smoke, dust and steam want it;\n"
                    "fire, sparks and magic look wrong when a shadow edge sweeps\n"
                    "across them.\n\n"
                    "Costs one extra PSO for this emitter. Emitters that leave it\n"
                    "off pay nothing - their shader never declares the shadow map.");
            if (!rc.lit) ImGui::EndDisabled();

            // ---- Custom shader: drop a .hlsl from the Asset Browser ----
            ImGui::Separator();
            ImGui::Checkbox("Use Custom Shader", &rc.useCustomShader);
            if (rc.useCustomShader)
            {
                hlslDrop("Vertex Shader (.vs.hlsl)",  rc.customVertexPath);
                hlslDrop("Surface Shader (.ps.hlsl)", rc.customSurfacePath);
                ImGui::TextDisabled("Drag a .hlsl from the Asset Browser, or type a path.");
            }
        });

    // ParticleEmitterComponent - standard emit behaviour. postDraw: mesh source
    // name + Fire Burst. (Appearance lives on a sibling Particle Render.)
    RegisterReflectedComponent<ParticleEmitterComponent>("Particle Emitter",
        /*priority*/ 61,
        [](ParticleEmitterComponent& pe, World* w, Entity e)
        {
            if (w && !w->HasComponent<ParticleRenderComponent>(e))
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "No Particle Render component — using default look.");
            if (pe.shape == ParticleShape::Mesh
                && w && pe.meshSourceEntity != NullEntity)
            {
                const std::string& name = w->GetName(pe.meshSourceEntity);
                ImGui::TextDisabled("Mesh Source: %s", name.c_str());
            }
            if (pe.burstCount > 0)
            {
                if (ImGui::Button("Fire Burst"))
                    pe.burstTrigger = true;
                ImGui::SameLine();
                ImGui::TextDisabled("emits %d now", pe.burstCount);
            }
        });

    // ParticleFlockComponent - Boids behaviour.
    RegisterReflectedComponent<ParticleFlockComponent>("Particle Flock",
        /*priority*/ 61,
        [](ParticleFlockComponent&, World* w, Entity e)
        {
            if (w && !w->HasComponent<ParticleRenderComponent>(e))
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "No Particle Render component — using default look.");
        });

    // ParticleAmbientComponent - camera-follow wrap volume. postDraw: derived
    // population + refill button.
    RegisterReflectedComponent<ParticleAmbientComponent>("Particle Ambient",
        /*priority*/ 61,
        [](ParticleAmbientComponent& ac, World* w, Entity e)
        {
            if (w && !w->HasComponent<ParticleRenderComponent>(e))
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "No Particle Render component — using default look.");
            const float    vol  = 8.0f * ac.ambientExtents.x * ac.ambientExtents.y * ac.ambientExtents.z;
            const uint32_t want = (ac.ambientDensity > 0.0f)
                ? static_cast<uint32_t>(std::ceil(ac.ambientDensity * vol))
                : static_cast<uint32_t>((std::max)(ac.ambientCount, 1));
            const uint32_t got  = (std::min)(want, 131072u);   // per-bucket slice cap
            if (got < want)
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                    "Ambient population: %u  (CLAMPED from %u)", got, want);
            else
                ImGui::TextDisabled("Ambient population: %u", got);
            if (ImGui::Button("Refill Volume"))
                ac.ambientSpawned = false;   // force a uniform re-fill next tick
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Redistribute all particles uniformly in the current box.");
        });

    // Trail - shape-only descriptor (points/width/sampling/uv/gradient).
    // All shading lives on the entity's MaterialComponent: texture (Base
    // Color slot), blend mode, custom shader + its params/Tex_ textures -
    // edited in the "Material" section below this one. postDraw only shows
    // status + an Add Material shortcut. TrailSystem moves the component's
    // look fields (migration storage) into a Material on first sight.
    RegisterReflectedComponent<TrailComponent>("Trail",
        /*priority*/ 62,
        [this](TrailComponent& tc, World* w, Entity e)
        {
            ImGui::TextDisabled("Slot: %u  (auto-assigned on first sample)",
                                tc.trailSlot);
            if (!w) return;

            auto* pm  = w->GetPool<MaterialComponent>();
            auto* pmr = w->GetPool<MeshLibRef>();
            MaterialComponent* mc = pm ? pm->Get(e) : nullptr;
            const bool meshOwned = (mc && pmr && pmr->Get(e));

            if (meshOwned)
            {
                ImGui::TextDisabled("Shading: entity's Material belongs to its MESH —\n"
                                    "trail renders the default gradient look.");
            }
            else if (mc && mc->useCustomShader && !mc->customShaderPath.empty())
            {
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f),
                    "Shading: Material (custom shader).");
                ImGui::TextDisabled("Edit texture/blend/shader/params in the Material section.");
            }
            else if (mc)
            {
                ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f),
                    "Shading: Material (default trail shader).");
                ImGui::TextDisabled("Blend + mask texture (Base Color) come from the Material\n"
                                    "section; gradient above multiplies in as vertex color.");
            }
            else
            {
                ImGui::TextDisabled("Shading: none — gradient-only default look.");
                if (ImGui::SmallButton("Add Material"))
                {
                    const Entity ent = e;
                    w->AddComponent<MaterialComponent>(ent, MaterialComponent{});
                    if (UndoEnabled() && w->IsAlive(ent))
                    {
                        SyncedEditContext();
                        auto addFn    = [](World& w2, Entity e2)
                            { w2.AddComponent<MaterialComponent>(e2, MaterialComponent{}); };
                        auto removeFn = [](World& w2, Entity e2)
                            { w2.RemoveComponent<MaterialComponent>(e2); };
                        PushEdit(std::make_unique<Editor::Undo::AddComponentAction>(
                            w->MakeHandle(ent), addFn, removeFn, "Material"));
                    }
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Texture / blend / custom shader are Material properties now.");
            }
        });

    // Anim FX Gate - hands this entity's whole effect group (Trail / Particle
    // Emitter, itself + children + FollowEntity followers) to FXGate notify
    // intervals on the attack clip. postDraw reports what the gate can
    // actually see, because every failure mode here is silent: no driver
    // reachable, or a driver with no open gate, both just mean "nothing
    // happens" and look identical to a broken effect.
    RegisterReflectedComponent<AnimFXGateComponent>("Anim FX Gate",
        /*priority*/ 62,
        [](AnimFXGateComponent& g, World* w, Entity e)
        {
            ImGui::TextDisabled("Currently: %s", g.hasApplied
                ? (g.lastApplied ? "ARMED" : "off") : "(not evaluated yet)");
            if (!w) return;

            const bool hasFX = w->HasComponent<TrailComponent>(e) ||
                               w->HasComponent<ParticleEmitterComponent>(e);
            if (!hasFX && !g.includeFollowers)
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.2f, 1.f),
                    "No Trail / Particle Emitter here and followers are off:\n"
                    "this gate drives nothing.");

            // Same walk AnimFXGateSystem does, so the readout cannot disagree
            // with the runtime.
            Entity driver = NullEntity;
            {
                Entity cur = e;
                for (int hop = 0; hop < 16 && cur != NullEntity; ++hop)
                {
                    if (w->HasComponent<AnimationComponent>(cur)) { driver = cur; break; }
                    Entity next = NullEntity;
                    if (auto* fs = w->GetComponent<FollowSocketComponent>(cur))
                        next = fs->target.Resolve(*w);
                    if (next == NullEntity)
                        if (auto* fe = w->GetComponent<FollowEntityComponent>(cur))
                            next = fe->target.Resolve(*w);
                    if (next == NullEntity)
                        if (const Parent* p = w->GetComponent<Parent>(cur))
                            next = p->entity;
                    if (next == cur) break;
                    cur = next;
                }
            }
            if (driver == NullEntity)
                ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f),
                    "No animated driver up the attach chain: gate stays off.\n"
                    "Attach this to a character socket, or tick Force On.");
            else
                ImGui::TextDisabled("Driver: entity %u", driver);
        });

    // BillboardFX - animated sprite-sheet billboard; postDraw shows the bound
    // texture + the live flipbook frame.
    RegisterReflectedComponent<BillboardFXComponent>("Billboard FX (Sprite)",
        /*priority*/ 62,
        [](BillboardFXComponent& b, World*, Entity)
        {
            const int cols  = (b.columns > 0) ? b.columns : 1;
            const int rows  = (b.rows    > 0) ? b.rows    : 1;
            const int total = (b.frameCount > 0) ? b.frameCount : (cols * rows);
            ImGui::TextDisabled("Texture: %s  idx=%d",
                b.texturePath.empty() ? "(no texture)" : "bound",
                b.textureBindlessIdx);
            ImGui::TextDisabled("Frame: %d / %d", b.frame, total);
        });

    // Timeline - AnimNotify tracks. Uses the raw RegisterComponentEditor
    // path (no Reflect::Descriptor) because the nested vector<Track> with
    // PropertyBag values does not map onto REFLECT_FLOAT / REFLECT_STRING
    // etc. The full track/notify UI lives in the Timeline Editor panel; this
    // row gives a one-line summary + a button to open it.
    RegisterComponentEditor<TimelineComponent>("Timeline",
        [this](void* ptr, World*, Entity) {
            auto& tl = *static_cast<TimelineComponent*>(ptr);
            size_t numNotifies = 0, numStates = 0;
            for (const auto& tr : tl.tracks) {
                numNotifies += tr.notifies.size();
                numStates   += tr.states.size();
            }
            ImGui::Text("%zu tracks, %zu notifies, %zu states",
                        tl.tracks.size(), numNotifies, numStates);
            ImGui::Text("Duration: %.2fs", tl.clipDuration);
            ImGui::TextDisabled("Last observed time: %.3fs", tl.lastObservedTime);
            if (ImGui::Button("Open Timeline Editor"))
                m_showTimeline = true;
        },
        /*priority*/ 63);

    // BeamComponent - descriptor for globals + control points; postDraw seeds a default + shader hint.
    RegisterReflectedComponent<BeamComponent>("Beam (Procedural Tube)",
        /*priority*/ 63,
        [](BeamComponent& bc, World*, Entity)
        {
            if (bc.controlPoints.size() < 2)
            {
                BeamControlPoint p0, p1;
                p0.position = { 0.0f, 0.0f, 0.0f };
                p1.position = { 5.0f, 0.0f, 0.0f };
                p0.radius   = p1.radius   = 0.3f;
                p0.colorTint = p1.colorTint = { 1.0f, 0.5f, 0.2f, 4.0f };
                bc.controlPoints = { p0, p1 };
            }
            ImGui::Separator();
            ImGui::TextDisabled("Slot: %s  (auto-acquired on first frame)",
                                bc.beamSlot == 0xFFFFFFFFu
                                    ? "(unassigned)"
                                    : std::to_string(bc.beamSlot).c_str());
            ImGui::TextWrapped(
                "Hint: control points are interpreted as world-space when the "
                "entity transform is identity. Add a MaterialComponent with "
                "useCustomShader=true and shader path "
                "'shaders/BeamTube_InnerCore.ps.hlsl' (Opaque) for the inner "
                "core, or 'BeamTube_OuterGlow.ps.hlsl' (Additive) for the "
                "outer glow shell.");
        });

    // AIComponent - descriptor for enabled/tickInterval; postDraw walks the BTInstance trace via BTAsset lookup.
    RegisterReflectedComponent<AIComponent>("AI (Behavior Tree)",
        /*priority*/ 65,
        [aiSys = &m_aiSys](AIComponent& ai, World*, Entity)
        {
            // BT slot - drag a .bt.lua file from the Resource panel onto
            // this button to attach. Shows the current path (or a hint when
            // empty); right-click clears. Loading goes through
            // AISystem::AcquireTree so the tree is cached by path.
            const std::string& path = ai.treePath;
            const char* label = path.empty()
                ? "Drop .bt.lua here..."
                : path.c_str();

            ImGui::PushID("##aiTreeSlot");
            // Use a wide button so the drop zone is obvious.
            const ImVec2 slotSize(-FLT_MIN, 0);
            ImGui::Button(label, slotSize);

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("ILUA_PATH"))
                {
                    const char* dropped = static_cast<const char*>(p->Data);
                    if (*aiSys && dropped && *dropped)
                    {
                        if (auto tree = (*aiSys)->AcquireTree(dropped))
                        {
                            // SetTree, not raw assignment: re-dropping a
                            // different tree must not keep the old instance's
                            // NodeId-keyed state (see AIComponents.h).
                            ai.SetTree(tree, dropped);
                            LOG_INFO("AI: attached '%s' to entity", dropped);
                        }
                        else
                        {
                            LOG_ERROR("AI: AcquireTree failed for '%s' "
                                      "(not a valid .bt.lua?)", dropped);
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }

            // Right-click -> detach.
            if (ImGui::BeginPopupContextItem("##aiTreeCtx"))
            {
                if (ImGui::MenuItem("Detach", nullptr, false, !path.empty()))
                {
                    ai.SetTree(nullptr);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Status line - what is currently attached + a Reload button so
            // edits to the .bt.lua hot-reload without a re-drag.
            if (!path.empty())
            {
                ImGui::SameLine(0, 4);
                ImGui::BeginDisabled(!(*aiSys));
                if (ImGui::SmallButton("Reload"))
                {
                    // ReloadTree forces a re-parse and updates the path-cache,
                    // so subsequent AcquireTree calls (other entities, scene
                    // load) get the fresh tree. Existing aliased shared_ptrs
                    // keep their stale copy until they re-acquire.
                    if (auto tree = (*aiSys)->ReloadTree(path))
                    {
                        ai.tree = tree;
                        if (!ai.instance)
                            ai.instance = std::make_unique<AI::BTInstance>();
                        LOG_INFO("AI: reloaded '%s'", path.c_str());
                    }
                }
                ImGui::EndDisabled();
            }

            if (!ai.tree || !ai.instance || ai.instance->trace.empty()) return;
            ImGui::Separator();
            ImGui::TextDisabled("Last Tick Trace (%zu nodes)",
                                ai.instance->trace.size());

            // Sort by NodeId before display. Record() fires on Tick return, so
            // the raw trace is post-order - children appear before their
            // parent, which hides which Selector branch took the tick (the
            // most common question). AssignIds walks pre-order, so id order ==
            // the order written in the .bt.lua.
            static std::vector<AI::BTInstance::TraceEntry> s_sorted;
            s_sorted.assign(ai.instance->trace.begin(), ai.instance->trace.end());
            std::sort(s_sorted.begin(), s_sorted.end(),
                      [](const auto& a, const auto& b) { return a.id < b.id; });

            for (const auto& te : s_sorted)
            {
                const AI::BTNode* n = ai.tree->FindNode(te.id);
                // DebugLabel() names the leaf ("Action:PrepareAttack"), not just its type.
                const std::string lbl = n ? n->DebugLabel() : std::string("?");
                ImVec4 col(0.7f, 0.7f, 0.7f, 1.f);
                const char* st = "?";
                switch (te.status)
                {
                case AI::NodeStatus::Success: col = {0.4f, 0.9f, 0.4f, 1.f}; st = "Success"; break;
                case AI::NodeStatus::Failure: col = {0.9f, 0.4f, 0.4f, 1.f}; st = "Failure"; break;
                case AI::NodeStatus::Running: col = {1.0f, 0.85f, 0.3f, 1.f}; st = "Running"; break;
                default: break;
                }
                const float indent = n ? n->depth * 12.f : 0.f;
                if (indent > 0.f) ImGui::Indent(indent);
                ImGui::TextColored(col, "[%u] %s - %s", te.id, lbl.c_str(), st);
                if (indent > 0.f) ImGui::Unindent(indent);
            }
        });

    // Blackboard - variant table not expressible as a descriptor; postDraw renders live key/type/value rows.
    RegisterReflectedComponent<BlackboardComponent>("Blackboard",
        /*priority*/ 66,
        [](BlackboardComponent& bbRef, World* world, Entity)
        {
            ImGui::TextWrapped("Runtime working memory. Configure AI Behavior in Edit mode; these values are not saved.");
            auto* bb = &bbRef;
            if (bb->values.empty())
            {
                ImGui::TextDisabled("(empty)");
                return;
            }
            if (ImGui::BeginTable("##bb", 3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Key");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Value");
                ImGui::TableHeadersRow();
                for (auto& kv : bb->values)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kv.first.c_str());
                    ImGui::TableNextColumn();
                    std::visit([](const auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if      constexpr (std::is_same_v<T, bool>)        ImGui::TextUnformatted("bool");
                        else if constexpr (std::is_same_v<T, int>)         ImGui::TextUnformatted("int");
                        else if constexpr (std::is_same_v<T, float>)       ImGui::TextUnformatted("float");
                        else if constexpr (std::is_same_v<T, std::string>) ImGui::TextUnformatted("string");
                        else if constexpr (std::is_same_v<T, Entity>)      ImGui::TextUnformatted("Entity");
                        else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
                            ImGui::TextUnformatted("vec3[]");
                        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                            ImGui::TextUnformatted("string[]");
                        else                                               ImGui::TextUnformatted("vec3");
                    }, kv.second);
                    ImGui::TableNextColumn();
                    ImGui::PushID(kv.first.c_str());
                    std::visit([world](auto& v) {
                        using T = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<T, bool>)
                            ImGui::Checkbox("##v", &v);
                        else if constexpr (std::is_same_v<T, int>)
                            ImGui::DragInt("##v", &v);
                        else if constexpr (std::is_same_v<T, float>)
                            ImGui::DragFloat("##v", &v, 0.01f);
                        else if constexpr (std::is_same_v<T, std::string>)
                            ImGui::TextUnformatted(v.c_str());
                        else if constexpr (std::is_same_v<T, Entity>)
                        {
                            ImGui::Text("%s (#%u)",world && world->IsAlive(v)?world->GetName(v).c_str():"<missing>",v);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<DirectX::XMFLOAT3>>)
                        {
                            // Per-element drag would be enormous: show a count summary
                            // (arrays are typically authored in the BT file, not the inspector).
                            // TODO: a "View" disclosure that lists the points.
                            char buf[48];
                            snprintf(buf, sizeof(buf), "[%zu pts]", v.size());
                            ImGui::TextUnformatted(buf);
                        }
                        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
                        {
                            char buf[64];
                            snprintf(buf, sizeof(buf), "[%zu strs]", v.size());
                            ImGui::TextUnformatted(buf);
                        }
                        else
                        {
                            ImGui::DragFloat3("##v", &v.x, 0.01f);
                        }
                    }, kv.second);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        });

    // ---- Category assignments ----
    // The Add Component popup groups submenus by category (order: kCategoryOrder);
    // unset ones default to "Misc". Every addable component should appear here
    // exactly once so none silently falls into "Misc". Tags registered via
    // RegisterComponentTag have no Add entry and are intentionally absent.

    // -- Transform --
    SetComponentCategory<LocalTransform>                  ("Transform");

    // -- Rendering -- (renderable surfaces / visibility)
    SetComponentCategory<StaticMeshComponent>             ("Rendering");
    SetComponentCategory<MeshHandle>                      ("Rendering");
    SetComponentCategory<VisibilityComponent>             ("Rendering");
    SetComponentCategory<PerObjectShadowComponent>        ("Rendering");
    SetComponentCategory<HairShadowCasterComponent>       ("Rendering");
    SetComponentCategory<FaceOverlayComponent>            ("Rendering");
    SetComponentCategory<DissolveComponent>               ("Rendering");

    // -- Lighting -- (light sources + global illumination)
    SetComponentCategory<LightData>                       ("Lighting");
    // Sits on a light entity next to LightData, so it belongs in Lighting
    // rather than Environment - without an explicit category it defaults to
    // "Misc", which is pinned last in the Add Component popup.
    SetComponentCategory<TODLightComponent>               ("Lighting");
    SetComponentCategory<VolumetricLightComponent>        ("Lighting");
    SetComponentCategory<ReflectionProbeComponent>        ("Lighting");
    SetComponentCategory<DDGIVolumeComponent>             ("Lighting");
    SetComponentCategory<IndirectLightingSettingsComponent>("Lighting");

    // -- Environment -- (sky / atmosphere / time-of-day / terrain)
    SetComponentCategory<WorldEnvironmentComponent>       ("Environment");
    SetComponentCategory<SkyboxComponent>                 ("Environment");
    SetComponentCategory<AtmosphereComponent>             ("Environment");
    SetComponentCategory<TODConfigComponent>              ("Environment");
    SetComponentCategory<TODOutputComponent>              ("Environment");
    SetComponentCategory<SunLightTag>                     ("Environment");
    SetComponentCategory<MoonLightTag>                    ("Environment");
    SetComponentCategory<CloudComponent>                  ("Environment");
    SetComponentCategory<HeightFogComponent>              ("Environment");
    SetComponentCategory<VolumetricFogComponent>          ("Environment");
    SetComponentCategory<LocalFogVolumeComponent>         ("Environment");
    SetComponentCategory<TerrainComponent>                ("Environment");
    SetComponentCategory<GrassComponent>                  ("Environment");
    SetComponentCategory<WaterComponent>                  ("Environment");
    SetComponentCategory<WaterInteractionComponent>       ("Environment");

    // -- Camera -- (lens + controllers + VCam stack)
    SetComponentCategory<CameraComponent>                 ("Camera");
    SetComponentCategory<CameraControllerComponent>       ("Camera");
    SetComponentCategory<VirtualCameraComponent>          ("Camera");
    SetComponentCategory<CameraPoseComponent>             ("Camera");
    SetComponentCategory<VCamPriorityComponent>           ("Camera");
    SetComponentCategory<VCamBlendComponent>              ("Camera");
    SetComponentCategory<FollowCameraComponent>           ("Camera");
    SetComponentCategory<AimCameraComponent>              ("Camera");
    SetComponentCategory<CameraShakeComponent>            ("Camera");
    SetComponentCategory<LiveCameraComponent>             ("Camera");

    // -- Animation -- (skeletons / clips / morphs / sockets / IK / timeline)
    SetComponentCategory<SkeletonComponent>               ("Animation");
    SetComponentCategory<SkeletonRef>                     ("Animation");
    SetComponentCategory<AnimationSetComponent>           ("Animation");
    SetComponentCategory<AnimationComponent>              ("Animation");
    SetComponentCategory<MorphComponent>                  ("Animation");
    SetComponentCategory<SocketComponent>                 ("Animation");
    SetComponentCategory<IKRigComponent>("Animation");
    SetComponentCategory<HeadTrackComponent>              ("Animation");
    SetComponentCategory<FacialAnimationComponent>        ("Animation");
    // FacialAnimationComponent is trivially copyable AND ~3.8 KB of it is per-frame
    // runtime state (channels, channelName, the blink machine, resolvedOut), so the
    // default byte fingerprint would make the "Blink Now" preview button record an
    // undo entry and mark the scene dirty. The "FacialAnimation" serializer writes
    // exactly the authored fields the inspector can edit, so it is the right key.
    UseSerializerFingerprint<FacialAnimationComponent>();
    SetComponentCategory<TimelineComponent>               ("Animation");
    SetComponentCategory<LocomotionComponent>             ("Animation");
    SetComponentCategory<LocomotionTransitionComponent>   ("Animation");
    SetComponentCategory<AnimLayerComponent>              ("Animation");
    SetComponentCategory<CharacterAnimDriverComponent>    ("Animation");

    // -- Physics -- (rigid bodies / colliders / chains)
    SetComponentCategory<RigidBodyComponent>              ("Physics");
    SetComponentCategory<ColliderComponent>               ("Physics");
    SetComponentCategory<CapsuleColliderComponent>        ("Physics");
    SetComponentCategory<ChainPhysicsComponent>           ("Physics");

    // -- Gameplay -- (character motor + player state + identity tags)
    SetComponentCategory<CharacterControllerComponent>    ("Gameplay");
    SetComponentCategory<PlayerComponent>                 ("Gameplay");
    SetComponentCategory<TagComponent>                    ("Gameplay");

    // -- VFX -- (decals / particles / trails / beams)
    SetComponentCategory<DecalComponent>                  ("VFX");
    SetComponentCategory<ParticleRenderComponent>         ("VFX");
    SetComponentCategory<ParticleEmitterComponent>        ("VFX");
    SetComponentCategory<ParticleFlockComponent>          ("VFX");
    SetComponentCategory<ParticleAmbientComponent>        ("VFX");
    SetComponentCategory<TrailComponent>                  ("VFX");
    SetComponentCategory<BeamComponent>                   ("VFX");
    SetComponentCategory<AnimFXGateComponent>             ("VFX");

    // -- Post-Process --
    SetComponentCategory<ECS::PostProcessVolumeComponent> ("Post-Process");

    // -- Audio -- (sound emitters + listener)
    SetComponentCategory<Audio::AudioSourceComponent>     ("Audio");
    SetComponentCategory<Audio::AudioListenerComponent>   ("Audio");

    // -- UI -- (Canvas screen-space + world-space billboards)
    SetComponentCategory<UI::UICanvas>                    ("UI");
    SetComponentCategory<UI::UIRect>                      ("UI");
    SetComponentCategory<UI::UIClipRect>                  ("UI");
    SetComponentCategory<UI::UIInteractable>              ("UI");
    SetComponentCategory<UI::UIText>                      ("UI");
    SetComponentCategory<UI::UIImage>                     ("UI");
    SetComponentCategory<UI::UISpriteAnimComponent>       ("UI");
    SetComponentCategory<UI::UIDocument>                  ("UI");
    SetComponentCategory<UI::WorldSpaceUIComponent>       ("UI");
    SetComponentCategory<UI::WorldUIBarComponent>         ("UI");
    SetComponentCategory<UI::WorldUITextComponent>        ("UI");
    SetComponentCategory<UI::WorldUIImageComponent>       ("UI");
    SetComponentCategory<UI::DamageNumberComponent>       ("UI");

    // -- AI / Script -- (strategic/tactical/sensing AI + behavior tree + Lua)
    SetComponentCategory<AIIntentComponent>               ("AI / Script");
    SetComponentCategory<NavAgentComponent>               ("AI / Script");
    SetComponentCategory<PerceptionComponent>             ("AI / Script");
    SetComponentCategory<PerceptibleComponent>            ("AI / Script");
    SetComponentCategory<AIComponent>                     ("AI / Script");
    SetComponentCategory<AIBehaviorComponent>             ("AI / Script");
    SetComponentCategory<BlackboardComponent>             ("AI / Script");
    SetComponentCategory<ScriptComponent>                 ("AI / Script");

    // -- Attachment -- (follow entity / socket)
    SetComponentCategory<FollowEntityComponent>           ("Attachment");
    SetComponentCategory<FollowSocketComponent>           ("Attachment");

    // The linked modules' sections, categories and fingerprints (Editor/EditorModule.h).
    Editor::ModuleRegistry::Get().RegisterComponentEditors(ModuleHostFor());
}
