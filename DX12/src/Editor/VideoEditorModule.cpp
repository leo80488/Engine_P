// VideoEditorModule: the Video module's editor half - the Video inspector section.

#include "Editor/EditorModule.h"

#include "ECS/VideoComponent.h"
#include "ECS/VideoHelpers.h"         // Video::Play/Pause/Stop/Restart/Seek
#include "Editor/EditorLayer.h"
#include "Editor/ImGuiWidgets.h"      // EditorUI::DragFloat
#include "Editor/VideoReflection.h"

#include "imgui/imgui.h"

class VideoEditorModule final : public Editor::IEditorModule
{
public:
    // The descriptor handles every field; postDraw appends the play / pause / stop / restart
    // buttons, the seek slider and read-only debug stats (clock, decoded PTS, frames).
    void RegisterComponentEditors(const Editor::ModuleHost& host) override
    {
        host.layer->RegisterReflectedComponent<VideoComponent>("Video",
            /*priority*/ 50,
            [](VideoComponent& vc, World*, Entity)
            {
                ImGui::Separator();
                ImGui::Text("Transport");
                if (ImGui::Button("Play"))    Video::Play(vc);     ImGui::SameLine();
                if (ImGui::Button("Pause"))   Video::Pause(vc);    ImGui::SameLine();
                if (ImGui::Button("Stop"))    Video::Stop(vc);     ImGui::SameLine();
                if (ImGui::Button("Restart")) Video::Restart(vc);

                // The frame source's duration when known, else 300 s, so the slider still has a
                // useful range for live / unknown-length streams.
                const double duration = vc.decodedFrameSource ? vc.decodedFrameSource->GetDurationSeconds() : -1.0;
                float maxT = (duration > 0.0) ? (float)duration : 300.0f;
                float clock = (float)vc.clockSeconds;
                if (EditorUI::DragFloat("Seek (s)", &clock, 0.f, maxT, "%.3f s"))
                    Video::Seek(vc, (double)clock);

                ImGui::Separator();
                // The decode path follows from whether a source is set.
                const char* pathLabel = vc.decodedFrameSource ? "Decoded source (FFmpeg / Mp4FrameSource)"
                                                              : "(no source - author-driven dpb)";
                ImGui::TextDisabled("Decode path     : %s", pathLabel);
                ImGui::TextDisabled("Clock           : %.3f s", vc.clockSeconds);
                ImGui::TextDisabled("Last decoded PTS: %.3f s", vc.lastDecodedPts);
                ImGui::TextDisabled("Frames decoded  : %llu",
                                    (unsigned long long)vc.framesDecoded);
                ImGui::TextDisabled("Current DPB slot: %u / %zu",
                                    vc.currentDpbSlot, vc.dpb.size());
            });
        host.layer->SetComponentCategory<VideoComponent>("Rendering");
    }
};

EDITOR_MODULE(VideoEditorModule)
