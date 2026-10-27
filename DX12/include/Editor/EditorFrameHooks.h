#pragma once
// The editor's part of the frame (FramePipeline::EditorHooks), over EditorLayer: the
// editor UI build (BuildUI) and the editor steps of the frame, forwarded to EditorLayer.
// EngineCore's FramePipeline calls these, so it needs no #ifdef WITH_EDITOR.

#include "Renderer/FramePipeline.h"

class EditorLayer;
class Scheduler;

namespace Editor
{
    class EditorFrameHooks final : public FramePipeline::EditorHooks
    {
    public:
        // @p viewportFullscreen is App's F11 state, read at every UI build.
        EditorFrameHooks(EditorLayer& editorLayer, Scheduler& scheduler,
                         const bool& viewportFullscreen)
            : m_editorLayer(editorLayer), m_scheduler(scheduler),
              m_viewportFullscreen(viewportFullscreen) {}

        void DeferSnapshotCompletion() override;
        void BuildUI(IGraphicsDevice& backend, Renderer& renderer, World* world,
                     uint64_t sceneTexture, const RenderView* view) override;
        bool PrepareSnapshot(uint64_t frame) override;
        void CompleteDeferredSnapshot(bool frameLost) override;
        void DiscardSnapshot() override;
        void EndUIFrame(RHI::CommandList cl) override;
        Sys::OwnerTask EndUIFrameAsync(RHI::CommandList cl, uint64_t frame,
                                       uint64_t sceneTexture) override;

    private:
        EditorLayer& m_editorLayer;
        Scheduler&   m_scheduler;
        const bool&  m_viewportFullscreen;
    };
}
