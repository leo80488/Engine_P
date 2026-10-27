// Window > Animation > Face Tracking (see include/Editor/FaceTrackingWindow.h).

#include "Editor/FaceTrackingWindow.h"

#include "ECS/FaceTrackingSystem.h"      // FaceTrackingSystem::BakeTakeFile
#include "FaceTracking/FacePoseMath.h"   // FacePose::Calibration (the channel bars)
#include "FaceTracking/FaceTrackerMath.h" // kBlendshapeNames (the face state's scores)
#include "FaceTracking/FaceTrackingService.h"
#include "FaceTracking/ImageCapture.h"   // CameraDevice
#include "Graphics/IGraphicsDevice.h"
#include "Settings/SettingsStore.h"      // the camera permission's player layer

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW - ms-settings:privacy-webcam

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Editor
{
namespace
{
    constexpr const char* kFaults[] = {"none", "denied", "busy", "lost"};

    // MediaPipe's face mesh connections (mediapipe/python/solutions/face_mesh_connections.py): the landmark
    // index pairs of the outlines the preview draws. "Left" and "right" are the subject's.
    using Edge = std::pair<int, int>;
    constexpr Edge kFaceOval[] = {{10, 338}, {338, 297}, {297, 332}, {332, 284}, {284, 251}, {251, 389}, {389, 356}, {356, 454},
                                  {454, 323}, {323, 361}, {361, 288}, {288, 397}, {397, 365}, {365, 379}, {379, 378}, {378, 400},
                                  {400, 377}, {377, 152}, {152, 148}, {148, 176}, {176, 149}, {149, 150}, {150, 136}, {136, 172},
                                  {172, 58},  {58, 132},  {132, 93},  {93, 234},  {234, 127}, {127, 162}, {162, 21},  {21, 54},
                                  {54, 103},  {103, 67},  {67, 109},  {109, 10}};
    constexpr Edge kLips[] = {{61, 146},  {146, 91},  {91, 181},  {181, 84},  {84, 17},   {17, 314},  {314, 405}, {405, 321},
                              {321, 375}, {375, 291}, {61, 185},  {185, 40},  {40, 39},   {39, 37},   {37, 0},    {0, 267},
                              {267, 269}, {269, 270}, {270, 409}, {409, 291}, {78, 95},   {95, 88},   {88, 178},  {178, 87},
                              {87, 14},   {14, 317},  {317, 402}, {402, 318}, {318, 324}, {324, 308}, {78, 191},  {191, 80},
                              {80, 81},   {81, 82},   {82, 13},   {13, 312},  {312, 311}, {311, 310}, {310, 415}, {415, 308}};
    constexpr Edge kLeftEye[] = {{263, 249}, {249, 390}, {390, 373}, {373, 374}, {374, 380}, {380, 381}, {381, 382}, {382, 362},
                                 {263, 466}, {466, 388}, {388, 387}, {387, 386}, {386, 385}, {385, 384}, {384, 398}, {398, 362}};
    constexpr Edge kRightEye[] = {{33, 7},    {7, 163},   {163, 144}, {144, 145}, {145, 153}, {153, 154}, {154, 155}, {155, 133},
                                  {33, 246},  {246, 161}, {161, 160}, {160, 159}, {159, 158}, {158, 157}, {157, 173}, {173, 133}};
    constexpr Edge kLeftBrow[] = {{276, 283}, {283, 282}, {282, 295}, {295, 285}, {300, 293}, {293, 334}, {334, 296}, {296, 336}};
    constexpr Edge kRightBrow[] = {{46, 53}, {53, 52}, {52, 65}, {65, 55}, {70, 63}, {63, 105}, {105, 66}, {66, 107}};
    constexpr Edge kLeftIris[] = {{474, 475}, {475, 476}, {476, 477}, {477, 474}};
    constexpr Edge kRightIris[] = {{469, 470}, {470, 471}, {471, 472}, {472, 469}};

    struct Outline
    {
        const Edge* edges;
        size_t count;
        ImU32 colour;
    };

    // The face state's scores: the expressions that drive a character most visibly (the raw tracker
    // scores; the window's channel bars show every channel raw and calibrated).
    constexpr const char* kStateScores[] = {"eyeBlinkLeft",  "eyeBlinkRight",  "eyeWideLeft", "eyeWideRight",
                                            "browInnerUp",   "browDownLeft",   "browDownRight", "jawOpen",
                                            "mouthSmileLeft", "mouthSmileRight", "mouthFunnel", "mouthPucker"};

    bool IsImageSource(const std::string& kind) { return kind == "camera" || kind == "fakecamera" || kind == "video"; }

    ImVec4 StateColour(const std::string& state)
    {
        if (state == "running") return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
        if (state == "opening" || state == "ended" || state == "stopped") return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
        if (state == "busy" || state == "lost") return ImVec4(0.95f, 0.70f, 0.30f, 1.0f);
        return ImVec4(0.95f, 0.40f, 0.40f, 1.0f); // denied, failed
    }

    template <size_t N> constexpr Outline MakeOutline(const Edge (&e)[N], ImU32 colour) { return {e, N, colour}; }

    int BlendshapeIndex(const char* name)
    {
        for (size_t k = 0; k < FaceTracking::Tracker::kBlendshapeNames.size(); ++k)
            if (std::strcmp(FaceTracking::Tracker::kBlendshapeNames[k], name) == 0) return static_cast<int>(k);
        return -1;
    }
}

FaceTrackingWindow& FaceTrackingWindow::Get()
{
    static FaceTrackingWindow s_instance;
    return s_instance;
}

void FaceTrackingWindow::Draw(const Context& ctx)
{
    const bool held = ctx.held && ctx.held("Face Tracking");
    if (held && !m_heldBefore)
    {
        m_open = true;
        ImGui::SetNextWindowFocus();
    }
    m_heldBefore = held;
    if (!m_open) return;
    ImGui::SetNextWindowPos(ImVec2(40.0f, 60.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(520.0f, 880.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Face Tracking", &m_open))
    {
        ImGui::End(); // collapsed: no preview is asked for, so none is uploaded
        return;
    }
    if (!FaceTracking::Service::Get().Enabled())
        ImGui::TextWrapped("Face tracking is off (-facetrack_off).");
    else
    {
        DrawSources();
        ImGui::Separator();
        DrawOpen();
        ImGui::Separator();
        DrawSelected(ctx);
        ImGui::Separator();
        DrawBake(ctx);
    }
    ImGui::End();
}

void FaceTrackingWindow::DrawSources()
{
    FaceTracking::Service& svc = FaceTracking::Service::Get();
    const std::vector<FaceTracking::SourceStatus> sources = svc.Sources();
    bool selectedOpen = false;
    for (const FaceTracking::SourceStatus& s : sources) selectedOpen = selectedOpen || s.name == m_selected;
    if (!selectedOpen) m_selected = sources.empty() ? std::string() : sources.front().name;
    if (sources.empty())
    {
        ImGui::TextDisabled("No face source is open.");
        return;
    }
    std::string close;
    if (ImGui::BeginTable("##faceSources", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableHeadersRow();
        for (const FaceTracking::SourceStatus& s : sources)
        {
            ImGui::PushID(s.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(s.name.c_str(), s.name == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                m_selected = s.name;
            if (ImGui::IsItemHovered() && !s.detail.empty()) ImGui::SetTooltip("%s", s.detail.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.kind.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextColored(StateColour(s.state), "%s", s.state.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%llu", static_cast<unsigned long long>(s.received));
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("Close")) close = s.name;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!close.empty()) svc.Close(close);
}

void FaceTrackingWindow::DrawOpen()
{
    if (!ImGui::CollapsingHeader("Open a source", ImGuiTreeNodeFlags_DefaultOpen)) return;
    FaceTracking::Service& svc = FaceTracking::Service::Get();
    ImGui::InputText("Name", m_newName, sizeof(m_newName));

    // The player's camera permission (faceTracking.camera), with what it means.
    bool allowed = svc.CameraAllowed();
    if (ImGui::Checkbox("Allow face tracking to use the camera", &allowed))
    {
        auto& store = Settings::SettingsStore::Get();
        if (const auto* row = store.Find("faceTracking.camera"))
        {
            Settings::Value v;
            v.b = allowed;
            store.Write(*row, Settings::Layer::Player, v, "Face Tracking window");
        }
        svc.SetCameraAllowed(allowed);
        m_listed = false;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("%s", "Face tracking may use a camera only while this is on. The picture stays on this computer and is never "
                              "saved or sent anywhere: only the face's movements drive the character.");
    ImGui::PopTextWrapPos();
    ImGui::TextDisabled("Saved with the player's settings.");
    if (!allowed)
    {
        m_cameras.clear();
        m_listError.clear();
    }

    // Listing opens every camera (their lights blink): once when the window first shows, then
    // on Refresh.
    auto list = [&](bool refresh) {
        std::vector<FaceTracking::CameraDevice> devices;
        m_listError.clear();
        m_cameras.clear();
        if (!svc.ListCameras(devices, refresh, m_listError)) return;
        for (const FaceTracking::CameraDevice& d : devices) m_cameras.push_back({d.id, d.name.empty() ? d.id : d.name});
        if (m_camera >= static_cast<int>(m_cameras.size())) m_camera = 0;
    };
    if (!m_listed && allowed)
    {
        m_listed = true;
        list(false);
    }
    const char* preview = m_cameras.empty() ? "(no camera)" : m_cameras[static_cast<size_t>(m_camera)].name.c_str();
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::BeginCombo("##camera", preview))
    {
        for (int i = 0; i < static_cast<int>(m_cameras.size()); ++i)
        {
            ImGui::PushID(i);
            if (ImGui::Selectable(m_cameras[static_cast<size_t>(i)].name.c_str(), i == m_camera)) m_camera = i;
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!allowed);
    if (ImGui::Button("Refresh")) list(true);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_cameras.empty());
    if (ImGui::Button("Open camera"))
    {
        m_lastError.clear();
        if (svc.OpenCamera(m_cameras[static_cast<size_t>(m_camera)].id, 0, 0, 0.0f, m_newName, m_lastError)) m_selected = m_newName;
    }
    ImGui::EndDisabled();
    if (!m_listError.empty()) ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "%s", m_listError.c_str());

    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##video", "a video file (.mp4)", m_videoPath, sizeof(m_videoPath));
    ImGui::SameLine();
    if (ImGui::Button("Open video"))
    {
        m_lastError.clear();
        if (svc.OpenVideo(m_videoPath, m_newName, m_lastError)) m_selected = m_newName;
    }
#if WITH_HARNESS
    ImGui::SetNextItemWidth(120.0f);
    ImGui::Combo("##fault", &m_fault, kFaults, static_cast<int>(sizeof(kFaults) / sizeof(kFaults[0])));
    ImGui::SameLine();
    if (ImGui::Button("Open fake camera"))
    {
        m_lastError.clear();
        const std::string fault = m_fault == 0 ? std::string() : std::string(kFaults[m_fault]);
        if (svc.OpenFakeCamera(fault, m_newName, m_lastError)) m_selected = m_newName;
    }
#endif
    if (!m_lastError.empty()) ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "%s", m_lastError.c_str());
}

void FaceTrackingWindow::DrawSelected(const Context& ctx)
{
    if (m_selected.empty()) return;
    FaceTracking::Service& svc = FaceTracking::Service::Get();
    FaceTracking::SourceStatus st;
    if (!svc.Status(m_selected, st)) return;
    ImGui::Text("%s (%s):", st.name.c_str(), st.kind.c_str());
    ImGui::SameLine();
    ImGui::TextColored(StateColour(st.state), "%s", st.state.c_str());
    if (!st.lastError.empty()) ImGui::TextWrapped("%s", st.lastError.c_str());
    if (st.state == "denied" && ImGui::Button("Open the camera privacy settings"))
        ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-webcam", nullptr, nullptr, SW_SHOWNORMAL);
    // The picture first (what the tracker sees and makes of it), then recording and calibrating it.
    if (IsImageSource(st.kind)) DrawPreview(ctx);

    // ---- Record: what the source delivers, before any character's calibration or filter ----
    if (st.recording)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "Recording: %llu frames, %.2f s",
                           static_cast<unsigned long long>(st.recordedFrames), st.recordedSeconds);
        ImGui::SameLine();
        if (ImGui::Button("Stop recording"))
        {
            FaceTracking::RecordingResult r;
            std::string err;
            char buf[160];
            if (svc.StopRecording(m_selected, r, err))
            {
                std::snprintf(buf, sizeof(buf), "%llu frames, %.2f s%s", static_cast<unsigned long long>(r.frames), r.seconds,
                              r.truncated ? " (reached its limit)" : "");
                m_recordNote = r.path + ": " + buf;
            }
            else
                m_recordNote = "Not written: " + err;
        }
        // A take holds one frame an instant: a source whose time stands still records one frame.
        if (!st.live && st.recordedFrames > 0 && st.recordedSeconds == 0.0)
            ImGui::TextWrapped("This source's time is standing still (a synthetic or replay source runs on the game's "
                               "time, which stops in Edit mode and while paused): the take keeps only its newest frame "
                               "until it runs.");
    }
    else
    {
        ImGui::SetNextItemWidth(300.0f);
        ImGui::InputText("##takePath", m_takePath, sizeof(m_takePath));
        ImGui::SameLine();
        if (ImGui::Button("Record"))
        {
            std::string err;
            m_recordNote = svc.StartRecording(m_selected, m_takePath, 600.0, err) ? std::string() : "Not recording: " + err;
        }
    }
    if (!m_recordNote.empty()) ImGui::TextWrapped("%s", m_recordNote.c_str());
    DrawCalibration(st);
}

void FaceTrackingWindow::DrawPreview(const Context& ctx)
{
    FaceTracking::Service& svc = FaceTracking::Service::Get();
    // Asked every frame this is drawn; Service::Pump uploads (never UI code).
    svc.RequestPreview(m_selected);
    const FaceTracking::PreviewStats p = svc.GetPreviewStats();
    ImGui::Checkbox("Outlines", &m_showOutlines);
    ImGui::SameLine();
    ImGui::Checkbox("Points", &m_showPoints);
    ImGui::SameLine();
    ImGui::Checkbox("Crop", &m_showCrop);
    ImGui::SameLine();
    ImGui::Checkbox("Head", &m_showHead);
    ImGui::SameLine();
    ImGui::Checkbox("Face state", &m_showFaceState);
    RHI::Texture tex;
    const uint64_t srv = ctx.gfx && p.shown == m_selected && svc.PreviewTexture(tex) ? ctx.gfx->GetTexturePreviewSrvGpuHandle(tex) : 0;
    if (srv != 0)
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(srv)),
                     ImVec2(static_cast<float>(FaceTracking::kPreviewWidth), static_cast<float>(FaceTracking::kPreviewHeight)));
    else
        ImGui::Dummy(ImVec2(static_cast<float>(FaceTracking::kPreviewWidth), static_cast<float>(FaceTracking::kPreviewHeight)));
    const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
    FaceTracking::PreviewOverlay ov;
    const bool overlay = srv != 0 && svc.GetPreviewOverlay(ov) && ov.source == m_selected;
    if (overlay) DrawOverlay(ov, r0, r1);
    ImGui::TextDisabled("%llu images shown in %llu uploads (%llu dropped, %llu rejected)", static_cast<unsigned long long>(p.images),
                        static_cast<unsigned long long>(p.uploads), static_cast<unsigned long long>(p.dropped),
                        static_cast<unsigned long long>(p.rejected));
    if (srv != 0 && !overlay)
        ImGui::TextDisabled("No tracker output for this image (the tracker is loading or not in this build: see the source's detail).");
    if (overlay && m_showFaceState) DrawFaceState(ov);
}

void FaceTrackingWindow::DrawOverlay(const FaceTracking::PreviewOverlay& ov, const ImVec2& r0, const ImVec2& r1) const
{
    const FaceTracking::FaceOverlay& f = ov.face;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sx = (r1.x - r0.x) / static_cast<float>(FaceTracking::kPreviewWidth);
    const float sy = (r1.y - r0.y) / static_cast<float>(FaceTracking::kPreviewHeight);
    auto at = [&](float x, float y) { return ImVec2(r0.x + x * sx, r0.y + y * sy); };
    auto point = [&](int i) { return at(f.points[static_cast<size_t>(i) * 2], f.points[static_cast<size_t>(i) * 2 + 1]); };
    dl->PushClipRect(r0, r1, true);
    if (!f.found)
    {
        dl->AddText(ImVec2(r0.x + 6.0f, r0.y + 4.0f), IM_COL32(255, 110, 110, 255), "no face");
        dl->PopClipRect();
        return;
    }
    if (m_showCrop)
    {
        ImVec2 c[4];
        for (int k = 0; k < 4; ++k) c[k] = at(f.roi[k][0], f.roi[k][1]);
        dl->AddPolyline(c, 4, IM_COL32(255, 160, 40, 190), ImDrawFlags_Closed, 1.0f);
    }
    if (m_showOutlines)
    {
        const Outline outlines[] = {
            MakeOutline(kFaceOval, IM_COL32(230, 230, 230, 170)), MakeOutline(kLeftBrow, IM_COL32(255, 200, 60, 230)),
            MakeOutline(kRightBrow, IM_COL32(255, 200, 60, 230)), MakeOutline(kLeftEye, IM_COL32(80, 220, 255, 235)),
            MakeOutline(kRightEye, IM_COL32(80, 220, 255, 235)),  MakeOutline(kLips, IM_COL32(255, 90, 140, 235)),
            MakeOutline(kLeftIris, IM_COL32(120, 255, 120, 255)), MakeOutline(kRightIris, IM_COL32(120, 255, 120, 255)),
        };
        for (const Outline& o : outlines)
            for (size_t e = 0; e < o.count; ++e) dl->AddLine(point(o.edges[e].first), point(o.edges[e].second), o.colour, 1.5f);
    }
    if (m_showPoints)
        for (int i = 0; i < FaceTracking::kOverlayLandmarks; ++i) dl->AddCircleFilled(point(i), 1.1f, IM_COL32(120, 255, 120, 150), 4);
    if (m_showHead)
    {
        // The nose tip (landmark 1) and where the face points, as the camera sees it: yaw > 0 turns to the
        // subject's right, the image's left; pitch > 0 looks up.
        const ImVec2 nose = point(1);
        const ImVec2 c0 = at(f.roi[0][0], f.roi[0][1]), c1 = at(f.roi[1][0], f.roi[1][1]);
        const float side = std::sqrt((c1.x - c0.x) * (c1.x - c0.x) + (c1.y - c0.y) * (c1.y - c0.y));
        const float toRad = 3.14159265f / 180.0f;
        const ImVec2 tip(nose.x - std::sin(f.yawDeg * toRad) * side * 0.6f, nose.y - std::sin(f.pitchDeg * toRad) * side * 0.6f);
        dl->AddLine(nose, tip, IM_COL32(255, 80, 80, 255), 2.0f);
        dl->AddCircleFilled(nose, 3.0f, IM_COL32(255, 80, 80, 255));
        char text[96];
        std::snprintf(text, sizeof(text), "yaw %+.0f  pitch %+.0f  roll %+.0f", f.yawDeg, f.pitchDeg, f.rollDeg);
        dl->AddText(ImVec2(r0.x + 6.0f, r0.y + 4.0f), IM_COL32(255, 255, 255, 230), text);
    }
    dl->PopClipRect();
}

void FaceTrackingWindow::DrawFaceState(const FaceTracking::PreviewOverlay& ov) const
{
    const FaceTracking::FaceOverlay& f = ov.face;
    if (!f.found)
    {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "No face in this image (presence %.2f)", f.presence);
        return;
    }
    ImGui::Text("Face %s, presence %.2f  |  yaw %+.1f, pitch %+.1f, roll %+.1f deg", f.detected ? "detected" : "tracked", f.presence,
                f.yawDeg, f.pitchDeg, f.rollDeg);
    ImGui::TextDisabled("detector %.1f ms, landmarks %.1f ms, blendshapes %.1f ms, whole image %.1f ms  (%ux%u)", f.ms[0], f.ms[1],
                        f.ms[2], f.ms[3], f.width, f.height);
    if (!ImGui::BeginTable("##faceState", 4, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("name2", ImGuiTableColumnFlags_WidthFixed, 110.0f);
    ImGui::TableSetupColumn("bar2", ImGuiTableColumnFlags_WidthStretch);
    static const auto s_indices = [] {
        std::vector<int> v;
        for (const char* name : kStateScores) v.push_back(BlendshapeIndex(name));
        return v;
    }();
    for (size_t k = 0; k < s_indices.size(); ++k)
    {
        if (k % 2 == 0) ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kStateScores[k]);
        ImGui::TableNextColumn();
        const float v = s_indices[k] >= 0 ? f.blendshapes[static_cast<size_t>(s_indices[k])] : 0.0f;
        char label[16];
        std::snprintf(label, sizeof(label), "%.2f", v);
        ImGui::ProgressBar(std::clamp(v, 0.0f, 1.0f), ImVec2(-1.0f, 0.0f), label);
    }
    ImGui::EndTable();
}

// The performer's neutral face (and range) for the selected source, after a 3 s
// countdown to take it; the newest frame's channels raw and calibrated. Offered once
// the source has delivered a frame (a camera or a video gives faces only with a
// tracker); the countdown and the capture can be cancelled, and a capture the source
// does not finish gives up by itself.
void FaceTrackingWindow::DrawCalibration(const FaceTracking::SourceStatus& st)
{
    if (!ImGui::CollapsingHeader("Calibrate", ImGuiTreeNodeFlags_DefaultOpen)) return;
    FaceTracking::Service& svc = FaceTracking::Service::Get();
    const FaceTracking::CalibrationStatus cs = svc.GetCalibrationStatus(m_selected);
    const double now = ImGui::GetTime();
    if (m_countdownSource == m_selected && m_countdownEnd > 0.0)
    {
        if (now >= m_countdownEnd)
        {
            std::string err;
            m_calNote = svc.StartCalibration(m_selected, m_countdownRange, 1.0, err) ? std::string() : err;
            m_countdownEnd = 0.0;
        }
        else
        {
            ImGui::Text("Hold a %s face: %.1f s", m_countdownRange ? "wide-open (pull every face you can)" : "relaxed, looking ahead",
                        m_countdownEnd - now);
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) m_countdownEnd = 0.0;
        }
    }
    else if (cs.capturing)
    {
        ImGui::ProgressBar(static_cast<float>(cs.progress), ImVec2(240.0f, 0.0f), cs.mode.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) svc.CancelCalibration(m_selected);
    }
    else
    {
        auto start = [&](bool range) {
            m_countdownSource = m_selected;
            m_countdownRange = range;
            m_countdownEnd = now + 3.0;
            m_calNote.clear();
        };
        const bool delivering = st.received > 0;
        if (!delivering) ImGui::BeginDisabled();
        ImGui::BeginGroup();
        if (ImGui::Button("Neutral")) start(false);
        ImGui::SameLine();
        if (ImGui::Button("Range")) start(true);
        ImGui::EndGroup();
        if (!delivering) ImGui::EndDisabled();
        if (!delivering && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Nothing has come from this source yet: there is no face to calibrate from.");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) svc.ResetCalibration(m_selected);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", cs.hasNeutral && cs.hasRange ? "neutral + range" : cs.hasNeutral ? "neutral" : cs.hasRange ? "range" : "uncalibrated");
    if (!cs.capturing && !cs.error.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "The last capture stopped: %s", cs.error.c_str());
    if (!m_calNote.empty()) ImGui::TextWrapped("%s", m_calNote.c_str());

    FaceTracking::FaceFrame f;
    if (!svc.Newest(m_selected, f) || !ImGui::TreeNode("Channels (raw | calibrated)")) return;
    FaceTracking::FacePose::Calibration cal;
    uint64_t generation = 0;
    const bool have = svc.GetCalibration(m_selected, cal, generation);
    for (int ch = 0; ch < FaceTracking::kArkitChannelCount; ++ch)
    {
        if (!f.Has(ch)) continue;
        const float raw = f.values[ch];
        const float calibrated = have ? FaceTracking::FacePose::CalibrateChannel(raw, cal.neutral[ch], cal.maximum[ch]) : raw;
        const std::string name(FaceTracking::ArkitChannelName(ch));
        ImGui::ProgressBar(std::clamp(raw, 0.0f, 1.0f), ImVec2(110.0f, 0.0f), "");
        ImGui::SameLine();
        ImGui::ProgressBar(std::clamp(calibrated, 0.0f, 1.0f), ImVec2(110.0f, 0.0f), "");
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
    }
    ImGui::TreePop();
}

// Bake a take through the selected character's mapping into an .ianim.
void FaceTrackingWindow::DrawBake(const Context& ctx)
{
    if (!ImGui::CollapsingHeader("Bake a take")) return;
    ImGui::InputText("Take", m_bakeTake, sizeof(m_bakeTake));
    ImGui::InputText("Output .ianim", m_bakeOut, sizeof(m_bakeOut));
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputFloat("Keys a second", &m_bakeFps, 0.0f, 0.0f, "%.0f");
    m_bakeFps = m_bakeFps < 1.0f ? 1.0f : (m_bakeFps > 240.0f ? 240.0f : m_bakeFps);
    ImGui::SameLine();
    ImGui::Checkbox("Mirror", &m_bakeMirror);
    const bool can = ctx.world && ctx.skeletons && ctx.selected != NullEntity;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Bake for the selected character"))
    {
        FaceTrackingSystem::BakeOptions o;
        o.fps = m_bakeFps;
        o.mirror = m_bakeMirror;
        FaceTrackingSystem::BakeReport report;
        std::string err;
        if (FaceTrackingSystem::BakeTakeFile(*ctx.world, *ctx.skeletons, ctx.selected, m_bakeTake, m_bakeOut, o, report, err))
        {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%u keys (%.2f s): %zu morphs, %zu bones from '%s'. Play the bone clip on an AnimLayer "
                          "masked CustomRoot = that bone, the morph clip with the Animator.",
                          report.keys, report.duration, report.morphs, report.bones, report.root.c_str());
            m_bakeNote = buf;
            for (const std::string& w : report.warnings) m_bakeNote += "\n" + w;
        }
        else
            m_bakeNote = "Bake failed: " + err;
    }
    ImGui::EndDisabled();
    if (!can) ImGui::TextDisabled("Select the character to bake for.");
    if (!m_bakeNote.empty()) ImGui::TextWrapped("%s", m_bakeNote.c_str());
}

} // namespace Editor
