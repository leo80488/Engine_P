#include "Editor/EditorFrameHooks.h"

#include "Editor/EditorLayer.h"
#include "ECS/Scheduler.h"
#include "Graphics/GraphicsDX12.h"
#include "Renderer/Renderer.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/TaskSystem.h"
#include "System/ThreadContext.h"

#include "imgui/imgui.h"

static_assert(FramePipeline::EditorHooks::kSceneTexturePlaceholder ==
              EditorLayer::FrameSceneTexturePlaceholder,
              "the pipeline's placeholder scene texture must be the editor's");

void Editor::EditorFrameHooks::DeferSnapshotCompletion()
{
    m_editorLayer.SetDeferImGuiSnapshotCompletion(true);
}

void Editor::EditorFrameHooks::BuildUI(IGraphicsDevice& backend, Renderer& renderer, World* world,
                                       uint64_t sceneTexture, const RenderView* view)
{
    ZoneScopedN("EditorImGui");
    // Build UI on the native owner. World reads end before the draw-data
    // snapshot recording job; mutations remain deferred to the drain phase.
    Sys::ThreadCtx::Exempt _x("EditorImGui owner build before snapshot publication");
    m_editorLayer.SetSceneTextureId(sceneTexture);
    m_editorLayer.SetViewportFullscreen(m_viewportFullscreen);
    // Frame Generation is only coherent when the scene fills the window.
    // F11 makes that true in the editor; docked mode does not.
    static_cast<GraphicsDX12&>(backend).Surface().SetEditorViewportFullscreen(m_viewportFullscreen);
    m_editorLayer.SetWorld(world);
    m_editorLayer.SetRenderView(view ? *view : renderer.GetView());
    m_editorLayer.BeginImGuiFrame();
    m_editorLayer.OnUIRender();
#if defined(WITH_HARNESS) && WITH_HARNESS
    if (Sys::CommandLine::Get().Has("-frameuicallbackverify")) {
        ImGui::GetForegroundDrawList()->AddCallback([](const ImDrawList*, const ImDrawCmd*) {
            ENGINE_CHECK(!TaskSystem::OnWorkerThread(), "ImGui callback must stay on owner");
            LOG_INFO("[FRAME_UI_CALLBACK] owner=1");
        }, nullptr);
    }
#endif

    // Phase Debug: last-frame ms per system + per phase. Editor-side so
    // EngineCore stays ImGui-free. Toggled from Debug > Phase Debug.
    if (m_editorLayer.GetShowPhaseDebug())
    {
        bool open = true;
        if (ImGui::Begin("Phase Debug", &open))
        {
            const auto debugInfo = m_scheduler.GetDebugInfo();
            float totalMs = 0.f;
            for (const auto& p : debugInfo) totalMs += p.lastFrameMs;
            ImGui::Text("Scheduler total: %.3f ms", totalMs);
            // Pacing runs outside every phase (App::Run end-of-frame), so it is
            // NOT in the numbers above; shown here so a capped frame's idle time
            // is visible instead of mistaken for work.
            ImGui::TextDisabled("Frame pacing wait: %.3f ms (excluded from phases)",
                                backend.MainSurface().GetLastFramePacingWaitMs());
            // In parallel batches per-system times overlap, so systems in
            // one phase may sum to MORE than the phase wall time.
            ImGui::Separator();
            for (const auto& p : debugInfo)
            {
                if (p.systems.empty()) continue;
                const auto flags = ImGuiTreeNodeFlags_DefaultOpen
                                 | ImGuiTreeNodeFlags_SpanAvailWidth;
                if (ImGui::TreeNodeEx(p.name, flags,
                                      "%-22s  %6.3f ms",
                                      p.name, p.lastFrameMs))
                {
                    for (const auto& s : p.systems)
                        ImGui::Text("    %-30s  %6.3f ms",
                                    s.name, s.lastFrameMs);
                    ImGui::TreePop();
                }
            }
        }
        ImGui::End();
        // Window X button -> flip the menu toggle off so state stays synced.
        if (!open) m_editorLayer.SetShowPhaseDebug(false);
    }
}

bool Editor::EditorFrameHooks::PrepareSnapshot(uint64_t frame)
{
    return m_editorLayer.PrepareImGuiFrameSnapshot(frame);
}

void Editor::EditorFrameHooks::CompleteDeferredSnapshot(bool frameLost)
{
    m_editorLayer.CompleteDeferredImGuiSnapshot(frameLost);
}

void Editor::EditorFrameHooks::DiscardSnapshot()
{
    m_editorLayer.DiscardImGuiFrameSnapshot();
}

void Editor::EditorFrameHooks::EndUIFrame(RHI::CommandList cl)
{
    m_editorLayer.EndImGuiFrame(cl);
}

Sys::OwnerTask Editor::EditorFrameHooks::EndUIFrameAsync(RHI::CommandList cl, uint64_t frame,
                                                         uint64_t sceneTexture)
{
    return m_editorLayer.EndImGuiFrameAsync(cl, frame, sceneTexture);
}
