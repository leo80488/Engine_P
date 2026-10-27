// EditorLayer_WorldSettings.cpp - the WORLD SETTINGS window (UE's name for it):
// what travels WITH the open scene rather than with the project or the user.
//
//   * Scene script (.lua)    - serialized on the .iscene W line.
//   * Scene Systems          - the scene-scoped Lua Systems it runs (W line systems=).
//   * Render Config (.ippc)  - per-scene AO / Outline / Sky IBL values (and the
//                              volumetric-fog values for scenes without a
//                              Volumetric Fog component), plus which base look
//                              .ppprofile the scene loads.
//   * Ambient Occlusion (XeGTAO) and Outline tuning - the live values a Save of
//     the Render Config captures.
//
// Same split-TU pattern as the other EditorLayer_*.cpp files: member function
// definitions only, no state.

#include "Editor/EditorLayer.h"
#include "Editor/EditorLayerShared.h"                  // Open/SaveFileDialog
#include "Editor/EditorTheme.h"
#include "Editor/ImGuiWidgets.h"                       // EditorUI::DragFloat / DragInt
#include "Renderer/Renderer.h"
#include "RenderGraph/RenderPass/OutlinePass.h"
#include "RenderGraph/RenderPass/OutlineGeometryPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "Resource/PostProcessConfig.h"                // .ippc Capture/Save/Load/Apply
#include "Graphics/IGraphicsDevice.h"
#include "Scripting/ScriptSystem.h"                    // the scene-scoped Systems to offer
#include "System/PathUtil.h"                           // RelativizeAssetPath

#include "imgui/imgui.h"

#include <algorithm>
#include <string>
#include <vector>

void EditorLayer::RenderWorldSettingsWindow()
{
    ImGui::SetNextWindowSize(ImVec2(560.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World Settings", &m_showWorldSettings))
    {
        ImGui::End();
        return;
    }
    if (!m_renderer)
    {
        ImGui::TextDisabled("(renderer not available)");
        ImGui::End();
        return;
    }

    // ---- Scene script (.lua), serialized into the .iscene W line ----
    // SceneManager binds it on every ActivateScene: OnSceneEnter / OnSceneUpdate /
    // OnSceneExit, fresh instance per entry.
    // An edit here is queued for the next Play (ScriptSystem::RebindScene), so it does not wait
    // for the scene to be saved and reopened.
    bool bindingChanged = false;
    EditorTheme::Heading("Scene Script");
    {
        static constexpr const char* kSceneLuaFilter =
            "Lua Scene Script (*.lua)\0*.lua\0All Files\0*.*\0\0";
        if (ImGui::Button("Bind Script..."))
        {
            std::string path = OpenFileDialog(kSceneLuaFilter,
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                m_sceneScriptPath = Sys::RelativizeAssetPath(path);
                bindingChanged    = true;
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_sceneScriptPath.empty());
        if (ImGui::Button("Clear##sceneScript"))
        {
            m_sceneScriptPath.clear();
            bindingChanged = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (m_sceneScriptPath.empty())
            ImGui::TextDisabled("(none — game.json manifest 'script' applies, if registered)");
        else
            ImGui::TextWrapped("%s", m_sceneScriptPath.c_str());
    }

    // ---- Scene Systems: the scene-scoped Lua Systems this scene runs (W-line systems=) ----
    // Each gets a fresh instance and OnInit when the scene is entered and OnShutdown when it is
    // left; a System with `scope = "scene"` runs nowhere else. Takes effect on the next Play.
    EditorTheme::Heading("Scene Systems");
    {
        std::vector<std::string> available;
        if (m_scriptSys)
        {
            ScriptSystem::ScriptRegistrySnapshot snap;
            m_scriptSys->FillRegistrySnapshot(snap);
            for (const auto& row : snap.systems)
                if (row.sceneScoped && row.loaded
                    && std::find(m_sceneSystems.begin(), m_sceneSystems.end(), row.name) == m_sceneSystems.end())
                    available.push_back(row.name);
        }

        int removeAt = -1;
        for (int i = 0; i < static_cast<int>(m_sceneSystems.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::SmallButton("Remove")) removeAt = i;
            ImGui::SameLine();
            ImGui::TextUnformatted(m_sceneSystems[i].c_str());
            ImGui::PopID();
        }
        if (removeAt >= 0)
        {
            m_sceneSystems.erase(m_sceneSystems.begin() + removeAt);
            MarkSceneDirty();   // not on the undo stack: an undo back to the save point must not clear it
            bindingChanged = true;
        }
        if (m_sceneSystems.empty())
            ImGui::TextDisabled("(none — game.json manifest 'systems' applies, if registered)");

        ImGui::BeginDisabled(available.empty());
        if (ImGui::BeginCombo("##addSceneSystem", "Add System..."))
        {
            for (const std::string& name : available)
                if (ImGui::Selectable(name.c_str()))
                {
                    m_sceneSystems.push_back(name);
                    MarkSceneDirty();
                    bindingChanged = true;
                }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (available.empty() && m_scriptSys)
            ImGui::TextDisabled("(no other System declares scope = \"scene\")");
    }
    if (bindingChanged && m_scriptSys)
        m_scriptSys->RebindScene(m_sceneScriptPath, m_sceneSystems);

    // ---- Render Config (.ippc) ----
    EditorTheme::Heading("Render Config (.ippc)");
    {
        static constexpr const char* kPPCFilter =
            "Post-Process Config (*.ippc)\0*.ippc\0All Files\0*.*\0\0";
        if (ImGui::Button("Save Config..."))
        {
            std::string path = SaveFileDialog(kPPCFilter, "ippc",
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                Resource::PostProcessConfig cfg;
                cfg.CaptureFrom(*m_renderer);
                cfg.engineProfilePath = m_engineProfilePath;  // bind the look asset
                // Write to where the dialog pointed, but bind the relative form:
                // this string is serialized into the .iscene.
                if (Resource::SavePostProcessConfig(cfg, path))
                    m_postProcessConfigPath = Sys::RelativizeAssetPath(path);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Load Config..."))
        {
            std::string path = OpenFileDialog(kPPCFilter,
                m_assetDir.empty() ? nullptr : m_assetDir.c_str());
            if (!path.empty())
            {
                Resource::PostProcessConfig cfg;
                if (Resource::LoadPostProcessConfig(path, cfg))
                {
                    cfg.ApplyTo(*m_renderer, Sys::RelativizeAssetPath(path));   // also loads the .ppprofile look
                    m_postProcessConfigPath = Sys::RelativizeAssetPath(path);
                    m_engineProfilePath     = cfg.engineProfilePath;   // already relativized on load
                }
            }
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_postProcessConfigPath.empty());
        if (ImGui::Button("Clear##ippc"))
            m_postProcessConfigPath.clear();
        ImGui::EndDisabled();

        if (m_postProcessConfigPath.empty())
            ImGui::TextDisabled("(none bound — the scene saves without one)");
        else
            ImGui::TextWrapped("Bound: %s", m_postProcessConfigPath.c_str());
        ImGui::TextDisabled("Holds this scene's AO / Outline / Sky IBL values, its base look\n"
                            ".ppprofile, and volumetric fog for scenes without a\n"
                            "Volumetric Fog component. Loaded with the scene.");
    }

    ImGui::Spacing();
    // ---- XeGTAO (screen-space AO) ----
    if (XeGTAOPass* gtao = m_renderer ? m_renderer->GetXeGTAOPass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Screen-space AO (XeGTAO)"))
        {
            // The SCENE's AO switch (its .ippc's) - the player's features.ssao can still
            // turn AO off.
            bool aoOn = m_renderer->GetSSAOSceneIntent();
            if (ImGui::Checkbox("Enabled##XeGTAO", &aoOn))
                m_renderer->SetSSAOSceneIntent(aoOn, "World Settings");
            if (aoOn && !m_renderer->GetSSAOAuthored())
                ImGui::TextDisabled("Off for now: the player's Ambient Occlusion setting is off.");

            ImGui::TextDisabled("Quality");
            // Reference presets: Low=1x2  Medium=2x2  High=3x3  Ultra=9x3
            int sliceCount    = int(gtao->sliceCount);
            int stepsPerSlice = int(gtao->stepsPerSlice);
            if (EditorUI::DragInt("Slice Count",      &sliceCount,    1, 9))
                gtao->sliceCount    = uint32_t(sliceCount);
            if (EditorUI::DragInt("Steps per Slice",  &stepsPerSlice, 1, 6))
                gtao->stepsPerSlice = uint32_t(stepsPerSlice);

            ImGui::TextDisabled("Effect shape");
            EditorUI::DragFloat("Radius",                  &gtao->effectRadius,             0.05f, 5.0f,  "%.3f");
            EditorUI::DragFloat("Radius Multiplier",       &gtao->radiusMultiplier,         0.5f,  3.0f,  "%.3f");
            EditorUI::DragFloat("Falloff Range",           &gtao->effectFalloffRange,       0.0f,  1.0f,  "%.3f");
            EditorUI::DragFloat("Sample Distribution Pow", &gtao->sampleDistributionPower,  1.0f,  3.0f,  "%.2f");
            EditorUI::DragFloat("Thin Occluder Comp",      &gtao->thinOccluderCompensation, 0.0f,  1.0f,  "%.2f");
            EditorUI::DragFloat("Final Value Power",       &gtao->finalValuePower,          0.5f,  4.0f,  "%.2f");
            EditorUI::DragFloat("Depth MIP Offset",        &gtao->depthMIPSamplingOffset,   0.0f,  5.0f,  "%.2f");

            ImGui::TextDisabled("Temporal + denoise");
            EditorUI::DragFloat("Denoise Blur β",          &gtao->denoiseBlurBeta,          0.0f,  5.0f,  "%.2f");
            EditorUI::DragFloat("Temporal History α",      &gtao->temporalHistoryAlpha,     0.0f,  0.98f, "%.3f");
            ImGui::SameLine();
            ImGui::TextDisabled("(0 = off)");
            EditorUI::DragFloat("Temporal Reject |Δ|",     &gtao->temporalRejectionDiff,    0.1f,  1.0f,  "%.2f");

            if (ImGui::Button("Reset to XeGTAO Defaults"))
            {
                gtao->effectRadius             = 0.5f;
                gtao->radiusMultiplier         = 1.457f;
                gtao->effectFalloffRange       = 0.615f;
                gtao->sampleDistributionPower  = 2.0f;
                gtao->thinOccluderCompensation = 0.0f;
                gtao->finalValuePower          = 2.2f;
                gtao->depthMIPSamplingOffset   = 3.30f;
                gtao->denoiseBlurBeta          = 1.2f;
                gtao->sliceCount               = 3;
                gtao->stepsPerSlice            = 3;
                gtao->temporalHistoryAlpha     = 0.9f;
                gtao->temporalRejectionDiff    = 0.5f;
            }
        }
    }

    // ---- Outline ----
    // The fade widgets bind to OutlinePass even though OutlineGeometryPass normally
    // draws the rim: Renderer copies these onto the geometry pass every frame
    // (Renderer::UploadFrameData, the geometry-phase outline block). Binding them to the
    // geometry pass would give those fields two writers and the slider would snap back
    // every frame - the trap the diagSkipHull / hullSuppressed split avoids. Save/Load
    // Config round-trips both values (.ippc "Outline ... fadeStart= fadeEnd=").
    if (OutlinePass* op = m_renderer ? m_renderer->GetOutlinePass() : nullptr)
    {
        if (ImGui::CollapsingHeader("Outline", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // fadeEnd <= fadeStart makes the shaders' max(fadeEnd - fadeStart, 0.01) divisor
            // degenerate: everything past fadeStart snaps to distFade 0 and the rim vanishes
            // in one step. Push the other edge along instead of letting the two cross.
            constexpr float kFadeGap = 0.01f;

            if (EditorUI::DragFloat("Fade start (m)", &op->outlineFadeStart,
                                    0.0f, 500.0f, "%.1f m"))
                op->outlineFadeEnd = std::max(op->outlineFadeEnd,
                                              op->outlineFadeStart + kFadeGap);
            ImGui::SetItemTooltip(
                "Distance at which the outline starts fading. Inside this radius the\n"
                "rim is drawn at its full authored width.");

            if (EditorUI::DragFloat("Fade end (m)", &op->outlineFadeEnd,
                                    kFadeGap, 1000.0f, "%.1f m"))
                op->outlineFadeStart = std::clamp(op->outlineFadeStart, 0.0f,
                                                  op->outlineFadeEnd - kFadeGap);
            ImGui::SetItemTooltip(
                "Distance at which the outline is fully gone. Past this the hull\n"
                "vertices are collapsed behind the near plane and the GPU clips the\n"
                "triangle, so there is never a rim beyond it.");

            // In geometry mode the fade shrinks the extrusion WIDTH, not alpha (the
            // GBuffer has no blending), and the extrusion is authored in DISPLAY
            // pixels while rasterization happens at render res. Under an upscaler the
            // fade band can therefore sit entirely below one render pixel, which reads
            // as "the outline never fades, it just pops off". Show where that line is
            // crossed so the band can be authored around it.
            const OutlineGeometryPass* ogp =
                m_renderer ? m_renderer->GetOutlineGeometryPass() : nullptr;
            if (ogp && ogp->enabled && m_gfx)
            {
                const float renderW  = static_cast<float>(m_gfx->GetRenderWidth());
                const float displayW = ogp->displayWidth
                                     ? static_cast<float>(ogp->displayWidth) : renderW;
                const float ratio    = (renderW > 0.0f) ? (displayW / renderW) : 1.0f;
                const float wStart   = (ratio > 0.0f) ? (op->outlinePixels / ratio) : 0.0f;
                if (wStart <= 1.0f)
                    ImGui::TextDisabled("Sub-pixel at every distance (%.2f render px at full width)",
                                        wStart);
                else
                {
                    const float t = 1.0f - 1.0f / wStart;   // distFade where width == 1 px
                    ImGui::TextDisabled("Below 1 render px from %.1f m (%.2f px at full width)",
                                        op->outlineFadeStart
                                            + t * (op->outlineFadeEnd - op->outlineFadeStart),
                                        wStart);
                }
                ImGui::SetItemTooltip(
                    "Geometry mode fades the rim's WIDTH, not its alpha -- the GBuffer\n"
                    "has no blending. Below one render pixel the rim stops getting\n"
                    "dimmer and starts getting PATCHY instead, and only the temporal\n"
                    "resolve (TAA / DLSS / FSR2) averages that back into a fade.\n"
                    "Uses the global width; a material with its own outlinePixels\n"
                    "crosses this line somewhere else.");
            }

            if (OutlineGeometryPass* geom =
                    m_renderer ? m_renderer->GetOutlineGeometryPass() : nullptr)
            {
                ImGui::Separator();
                ImGui::Checkbox("Draw as geometry (writes depth + velocity)",
                                &geom->enabled);
                ImGui::SetItemTooltip(
                    "ON  = the inverted hull is drawn during the geometry phase into\n"
                    "      the 6-MRT GBuffer, so the rim writes DEPTH and VELOCITY\n"
                    "      like any other surface and TAA needs no outline special-\n"
                    "      casing at all (the stencil tag is dropped automatically).\n"
                    "      Distance fade shrinks the rim WIDTH instead of its alpha.\n"
                    "OFF = the old colour-only composite hull in OutlinePass, which\n"
                    "      fades true ALPHA but needs the stencil tag + TAA special\n"
                    "      cases to avoid trailing.\n\n"
                    "Mutually exclusive -- both draw the same hull.");
            }
        }
    }

    ImGui::End();
}
