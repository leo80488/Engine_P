#pragma once

// FaceTrackingWindow - Window > Animation > Face Tracking: the face sources
// FaceTracking::Service holds, each with its state and what to do about it (a camera
// refused by the privacy setting gets the button that opens that setting), opening a
// camera / a video / (harness builds) the fake camera, the preview of the selected
// source's images, recording the selected source to a take, calibrating it, and
// baking a take into an .ianim for the selected character.
//
// THE PREVIEW. While the window is drawn and a source is selected it calls
// Service::RequestPreview every frame; the service uploads from its Pump (the game
// half), never from here: under -renderowneroff UI code holds the device's execution
// scope and the upload would be refused. Collapsed, closed or with nothing selected:
// nothing is asked, so nothing is uploaded. Over the picture it draws what the
// tracker made of that same image (Service::GetPreviewOverlay): MediaPipe's outlines
// of the face, the eyes, the brows, the lips and the irises, the 478 points, the
// landmark crop and the head's direction, each one a toggle; under it, the face's
// state - found or not, tracked or detected, the head's angles, the stage times and
// the main expressions' raw scores.
//
// THE HARNESS. -ui_open_menu=Face Tracking@<frame> opens it (Context::held is
// EditorLayer::HarnessMenuHeldOpen); with nothing selected it shows the first source.
//
// Editor-only TU: compiled into EditorCore, never into EngineCore.

#include "ECS/ECS.h"

#include <functional>
#include <string>
#include <vector>

class IGraphicsDevice;
class SkeletonRegistry;
struct ImVec2;
namespace FaceTracking { struct SourceStatus; struct PreviewOverlay; }

namespace Editor
{
    class FaceTrackingWindow
    {
    public:
        struct Context
        {
            IGraphicsDevice*                 gfx = nullptr;
            std::function<bool(const char*)> held;          // the harness's -ui_open_menu hold
            World*                           world = nullptr;
            SkeletonRegistry*                skeletons = nullptr;
            Entity                           selected = NullEntity; // the character a bake is for
        };

        static FaceTrackingWindow& Get();

        bool IsOpen() const { return m_open; }
        void SetOpen(bool open) { m_open = open; }
        // Every frame, open or not (a harness hold opens it).
        void Draw(const Context& ctx);

    private:
        struct CameraEntry { std::string id, name; };

        void DrawSources();
        void DrawOpen();
        void DrawSelected(const Context& ctx);
        void DrawPreview(const Context& ctx);
        void DrawCalibration(const FaceTracking::SourceStatus& st);
        void DrawBake(const Context& ctx);
        // Over the preview image (r0..r1 on screen): what the tracker made of it - the landmarks, MediaPipe's
        // outlines, the landmark crop, the head's direction; then, under it, the face's state.
        void DrawOverlay(const FaceTracking::PreviewOverlay& ov, const ImVec2& r0, const ImVec2& r1) const;
        void DrawFaceState(const FaceTracking::PreviewOverlay& ov) const;

        bool m_open = false;
        bool m_heldBefore = false;
        std::string m_selected;               // the previewed source
        int m_camera = 0;                     // index into the listed cameras
        bool m_listed = false;
        std::string m_listError;

        std::vector<CameraEntry> m_cameras;
        char m_newName[64] = "camera";
        char m_videoPath[260] = "";
        int m_fault = 0;                      // the fake camera's: none / denied / busy / lost
        std::string m_lastError;              // the last open's
        // The preview's overlay layers
        bool m_showOutlines = true;
        bool m_showPoints = false;
        bool m_showCrop = true;
        bool m_showHead = true;
        bool m_showFaceState = true;
        // Recording a take and baking one into an .ianim
        char m_takePath[260] = "asset/FaceTakes/take.iface";
        std::string m_recordNote;
        char m_bakeTake[260] = "asset/FaceTakes/take.iface";
        char m_bakeOut[260] = "asset/FaceTakes/take.ianim";
        float m_bakeFps = 30.0f;
        bool m_bakeMirror = false;
        std::string m_bakeNote;
        // The calibration's countdown (ImGui time), for one source
        std::string m_countdownSource;
        double m_countdownEnd = 0.0;
        bool m_countdownRange = false;
        std::string m_calNote;
    };
}
