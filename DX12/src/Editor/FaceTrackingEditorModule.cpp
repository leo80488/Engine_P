// FaceTrackingEditorModule: the FaceTracking module's editor half - the Face Tracking inspector
// section, the Face Tracking and Face Mapping windows and their Window-menu rows.

#include "Editor/EditorModule.h"

#include "ECS/AnimationComponents.h"        // SkeletonComponent, kInvalidAnimHandle
#include "ECS/FaceTrackingComponent.h"
#include "ECS/FacialAnimationComponent.h"
#include "ECS/FacialAnimationSystem.h"      // Facial::FindMorphSlot
#include "Editor/EditorLayer.h"
#include "Editor/FaceMappingWindow.h"
#include "Editor/FaceTrackingReflection.h"
#include "Editor/FaceTrackingWindow.h"
#include "FaceTracking/FaceBinding.h"       // the status lines read the binding
#include "FaceTracking/FaceTrackingService.h"
#include "Renderer/Renderer.h"

#include "imgui/imgui.h"

class FaceTrackingEditorModule final : public Editor::IEditorModule
{
public:
    // A character driven by a face source. The same carrier rule as Facial Animation, whose fold
    // the morphs go through; the status lines read the binding FaceTrackingSystem keeps on the
    // component (FaceTracking/FaceBinding.h). The Face Mapping window has the rest.
    void RegisterComponentEditors(const Editor::ModuleHost& host) override
    {
        Renderer* renderer = host.renderer;
        auto addWithUndo = host.addComponentWithUndo;
        host.layer->RegisterReflectedComponent<FaceTrackingComponent>("Face Tracking",
            /*priority*/ 44,
            [renderer, addWithUndo](FaceTrackingComponent& comp, World* world, Entity entity)
            {
                if (!world) return;
                if (!world->GetComponent<FacialAnimationComponent>(entity))
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "No Facial Animation here: only the bones are driven.");
                    if (ImGui::Button("Add Facial Animation"))
                    {
                        // Auto-blink only where the model has the blink morph, or every non-PMX
                        // model would log that it is inert.
                        bool blinkMorph = false;
                        if (renderer)
                            if (const auto* sk = world->GetComponent<SkeletonComponent>(entity);
                                sk && sk->assetIndex != kInvalidAnimHandle && sk->assetIndex < renderer->GetSkeletonRegistry().Count())
                                blinkMorph = Facial::FindMorphSlot(renderer->GetSkeletonRegistry().Get(sk->assetIndex),
                                                                   FacialAnimationComponent{}.blinkMorph) >= 0;
                        addWithUndo(entity, std::type_index(typeid(FacialAnimationComponent)),
                            [blinkMorph](World& w, Entity ent) {
                                FacialAnimationComponent fa;
                                fa.autoBlinkEnabled = blinkMorph;
                                w.AddComponent<FacialAnimationComponent>(ent, fa);
                            });
                    }
                }
                FaceTracking::SourceStatus st;
                if (!FaceTracking::Service::Get().Status(comp.source, st))
                    ImGui::TextDisabled("Source \"%s\" is not open.", comp.source.c_str());
                else
                    ImGui::TextDisabled("Source: %s (%s), %llu frames, face %s", st.name.c_str(), st.kind.c_str(),
                                        static_cast<unsigned long long>(st.received), st.faceFound ? "found" : "not found");
                if (ImGui::SmallButton("Open Face Mapping")) Editor::FaceMappingWindow::Get().SetOpen(true);
                const FaceTracking::BindingRuntime* rt = comp.rt.get();
                if (!rt)
                {
                    ImGui::TextDisabled("Not bound yet: the next animation frame compiles the mapping.");
                    return;
                }
                ImGui::TextDisabled("%s, authority %.2f: %u morphs, head %s, eyes %s, jaw %s",
                                    FaceTracking::TrackStateName(rt->authority.State()), rt->authorityValue, rt->trackedCount,
                                    rt->haveHead ? "on" : "off", rt->haveEyes ? "on" : "off", rt->haveJaw ? "on" : "off");
                ImGui::TextDisabled("Mapping: %s", rt->profilePath.c_str());
                if (rt->compileFailed)
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Refused: %s", rt->compileError.c_str());
                if (!rt->warnings.empty() && ImGui::TreeNode("FaceTrackingNotes", "Binding notes (%zu)", rt->warnings.size()))
                {
                    for (const std::string& w : rt->warnings) ImGui::BulletText("%s", w.c_str());
                    ImGui::TreePop();
                }
            },
            /*requires*/ [](World& w, Entity e) -> bool {
                return w.HasComponent<SkeletonComponent>(e);
            });
        host.layer->SetComponentCategory<FaceTrackingComponent>("Animation");
        // Its runtime half (rt: the filters, the authority, the cursor) changes every
        // frame: only the serialized fields may make an undo entry.
        host.layer->UseSerializerFingerprint<FaceTrackingComponent>();
    }

    void AddPanels(std::vector<Editor::ModulePanel>& out) override
    {
        out.push_back({ "Animation", "Face Tracking", "Face Tracking",
                        [] { return Editor::FaceTrackingWindow::Get().IsOpen(); },
                        [](bool v) { Editor::FaceTrackingWindow::Get().SetOpen(v); },
                        "The face sources - replays, network apps, cameras, videos - with their\n"
                        "state and what to do about it, opening a camera or a video, and the\n"
                        "selected camera's preview." });
        out.push_back({ "Animation", "Face Mapping", "Face Mapping",
                        [] { return Editor::FaceMappingWindow::Get().IsOpen(); },
                        [](bool v) { Editor::FaceMappingWindow::Get().SetOpen(v); },
                        "The selected character's face mapping: an automatic one from its morph\n"
                        "names with its coverage, the compiled rules' live values, and sliders\n"
                        "that drive a face without a camera." });
    }

    void DrawPanels(const Editor::ModuleHost& host) override
    {
        SkeletonRegistry* skeletons = host.renderer ? &host.renderer->GetSkeletonRegistry() : nullptr;
        // Every frame, open or not (a -ui_open_menu hold opens it); its preview is asked for only
        // while it is drawn.
        Editor::FaceTrackingWindow::Get().Draw({ host.gfx, host.menuHeld, host.world, skeletons, host.selected });
        // The selected character's mapping, coverage and the virtual tracker (pushes only while shown).
        Editor::FaceMappingWindow::Get().Draw({ host.menuHeld, host.world, skeletons, host.selected });
    }
};

EDITOR_MODULE(FaceTrackingEditorModule)
