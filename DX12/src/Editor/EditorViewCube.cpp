#include "Editor/EditorViewCube.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cmath>

namespace
{
    // In OrthoViews order: the face a view looks at carries that view's name.
    const char* const kFaceLabel[6] = { "Top", "Bottom", "Front", "Back", "Left", "Right" };
    // Pixels the mouse may move between press and release and still be a click.
    constexpr float kClickSlop = 3.f;

    struct Face
    {
        int    view = -1;
        ImVec2 corners[4];
        ImVec2 centre;
        float  depth = 0.f;    // view-space z of the face centre: larger = farther
        float  facing = 0.f;   // dot(normal, forward): negative = towards the camera
    };

    bool InConvexQuad(const ImVec2 (&q)[4], const ImVec2& p)
    {
        float sign = 0.f;
        for (int i = 0; i < 4; ++i)
        {
            const ImVec2& a = q[i];
            const ImVec2& b = q[(i + 1) % 4];
            const float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
            if (cross == 0.f) continue;
            if (sign == 0.f) sign = cross;
            else if ((cross > 0.f) != (sign > 0.f)) return false;
        }
        return true;
    }

    ImU32 Colour(ImGuiCol idx, float alpha)
    {
        ImVec4 c = ImGui::GetStyleColorVec4(idx);
        c.w *= alpha;
        return ImGui::GetColorU32(c);
    }
}

Editor::ViewCube::Input Editor::ViewCube::Draw(ImDrawList* dl, float x, float y, float size, const Basis& basis,
                                               int activeView)
{
    // One cube per editor: the press survives frames here.
    static bool   s_pressed = false;
    static bool   s_dragged = false;
    static ImVec2 s_pressAt{};

    Input out;
    if (!dl || size <= 0.f) return out;
    const float  half   = size * 0.5f;
    const ImVec2 centre(x + half, y + half);
    const float  edge   = half * 0.52f;   // the cube's half edge in pixels: a corner reaches 0.9 x half

    Face faces[6];
    int  visible = 0;
    Face drawn[6];
    for (int v = 0; v < 6; ++v)
    {
        const DirectX::XMFLOAT3 n = FaceNormal(v);
        // Two axes in the face's plane.
        const DirectX::XMFLOAT3 t1 = std::fabs(n.y) > 0.5f ? DirectX::XMFLOAT3{ 1.f, 0.f, 0.f }
                                                           : DirectX::XMFLOAT3{ 0.f, 1.f, 0.f };
        const DirectX::XMFLOAT3 t2 = { n.y * t1.z - n.z * t1.y, n.z * t1.x - n.x * t1.z, n.x * t1.y - n.y * t1.x };
        const float s1[4] = { -1.f, 1.f, 1.f, -1.f };
        const float s2[4] = { -1.f, -1.f, 1.f, 1.f };
        Face& f = faces[v];
        f.view = v;
        for (int k = 0; k < 4; ++k)
        {
            const DirectX::XMFLOAT3 p = { n.x + s1[k] * t1.x + s2[k] * t2.x, n.y + s1[k] * t1.y + s2[k] * t2.y,
                                          n.z + s1[k] * t1.z + s2[k] * t2.z };
            const DirectX::XMFLOAT3 vp = ToView(basis, p);
            f.corners[k] = ImVec2(centre.x + vp.x * edge, centre.y - vp.y * edge);
        }
        const DirectX::XMFLOAT3 vc = ToView(basis, n);
        f.centre = ImVec2(centre.x + vc.x * edge, centre.y - vc.y * edge);
        f.depth  = vc.z;
        f.facing = n.x * basis.forward.x + n.y * basis.forward.y + n.z * basis.forward.z;
        if (f.facing < -0.01f) drawn[visible++] = f;
    }
    // Far faces first, so the nearest one is drawn on top.
    std::sort(drawn, drawn + visible, [](const Face& a, const Face& b) { return a.depth > b.depth; });

    const ImGuiIO& io    = ImGui::GetIO();
    const ImVec2   mouse = io.MousePos;
    // AllowWhenBlockedByActiveItem: the viewport's own button takes the active id on the press, in
    // the same frame and before this runs, and would hide the very click that starts on the cube.
    out.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && mouse.x >= x &&
                  mouse.x < x + size && mouse.y >= y && mouse.y < y + size;
    int hoveredView = -1;
    if (out.hovered)
        for (int i = visible - 1; i >= 0; --i)   // the nearest face wins
            if (InConvexQuad(drawn[i].corners, mouse)) { hoveredView = drawn[i].view; break; }

    // A press that starts on the cube is the cube's until release: a click selects a face, a drag turns.
    if (out.hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        s_pressed = true;
        s_dragged = false;
        s_pressAt = mouse;
    }
    if (s_pressed)
    {
        out.hovered = true;
        const float dx = mouse.x - s_pressAt.x, dy = mouse.y - s_pressAt.y;
        if (!s_dragged && dx * dx + dy * dy > kClickSlop * kClickSlop) s_dragged = true;
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (s_dragged)
            {
                out.dragging = true;
                out.dragX    = io.MouseDelta.x;
                out.dragY    = io.MouseDelta.y;
            }
        }
        else
        {
            if (!s_dragged) out.clickedView = hoveredView;
            s_pressed = false;
        }
    }

    dl->AddCircleFilled(centre, half * 0.95f, Colour(ImGuiCol_WindowBg, 0.35f), 32);
    for (int i = 0; i < visible; ++i)
    {
        const Face& f = drawn[i];
        const ImGuiCol fill = f.view == activeView ? ImGuiCol_ButtonActive
                            : f.view == hoveredView ? ImGuiCol_ButtonHovered : ImGuiCol_Button;
        dl->AddConvexPolyFilled(f.corners, 4, Colour(fill, 0.9f));
        dl->AddPolyline(f.corners, 4, Colour(ImGuiCol_Border, 1.f), ImDrawFlags_Closed, 1.f);
        // A label on a face seen nearly edge-on is unreadable: only the ones turned towards the camera.
        if (f.facing < -0.35f)
        {
            const char*  label = kFaceLabel[f.view];
            const ImVec2 ts    = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(f.centre.x - ts.x * 0.5f, f.centre.y - ts.y * 0.5f), Colour(ImGuiCol_Text, 1.f), label);
        }
    }
    if (out.hovered && hoveredView >= 0 && !out.dragging)
        ImGui::SetTooltip("%s view (click)  -  drag to turn the camera", kFaceLabel[hoveredView]);
    return out;
}
