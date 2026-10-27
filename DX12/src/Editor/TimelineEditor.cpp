#include "Editor/TimelineEditor.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/AnimationResource.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Timeline;

// ==== Track helpers ====

void Track::SortKeyframes()
{
    std::sort(keyframes.begin(), keyframes.end(),
              [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

int Track::InsertKeyframe(float time, float value)
{
    Keyframe kf;
    kf.time  = time;
    kf.value = value;
    auto it = std::lower_bound(keyframes.begin(), keyframes.end(), time,
                               [](const Keyframe& k, float t) { return k.time < t; });
    int idx = static_cast<int>(it - keyframes.begin());
    keyframes.insert(it, kf);
    return idx;
}

void Track::RemoveKeyframe(int index)
{
    if (index >= 0 && index < static_cast<int>(keyframes.size()))
        keyframes.erase(keyframes.begin() + index);
}

// ==== Undo / redo command types ====

namespace
{
    struct MoveKeyframeCmd : Command
    {
        Track* track;
        int    index;
        float  oldTime, newTime;

        void Execute() override { track->keyframes[index].time = newTime; track->SortKeyframes(); }
        void Undo()    override { track->keyframes[index].time = oldTime; track->SortKeyframes(); }
    };

    struct DeleteKeyframeCmd : Command
    {
        Track*   track;
        int      index;
        Keyframe saved;

        void Execute() override { track->RemoveKeyframe(index); }
        void Undo()    override
        {
            auto it = track->keyframes.begin() + index;
            track->keyframes.insert(it, saved);
        }
    };

    struct InsertKeyframeCmd : Command
    {
        Track* track;
        int    index;
        Keyframe kf;

        void Execute() override
        {
            auto it = track->keyframes.begin() + index;
            track->keyframes.insert(it, kf);
        }
        void Undo() override { track->RemoveKeyframe(index); }
    };

    static ImVec4 TrackColorByIndex(int idx)
    {
        static const ImVec4 s_palette[] = {
            ImVec4(0.90f, 0.30f, 0.30f, 1.f),
            ImVec4(0.30f, 0.80f, 0.30f, 1.f),
            ImVec4(0.30f, 0.50f, 0.90f, 1.f),
            ImVec4(0.90f, 0.80f, 0.20f, 1.f),
            ImVec4(0.80f, 0.40f, 0.90f, 1.f),
            ImVec4(0.20f, 0.80f, 0.80f, 1.f),
            ImVec4(0.90f, 0.55f, 0.20f, 1.f),
            ImVec4(0.70f, 0.70f, 0.70f, 1.f),
        };
        return s_palette[idx % 8];
    }
}

// ==== Interpolation ====

float TimelineEditor::EvaluateTrack(const Track& track, float time)
{
    if (track.keyframes.empty()) return 0.f;
    if (track.keyframes.size() == 1) return track.keyframes[0].value;

    if (time <= track.keyframes.front().time) return track.keyframes.front().value;
    if (time >= track.keyframes.back().time)  return track.keyframes.back().value;

    int idx = 0;
    for (int i = 0; i < static_cast<int>(track.keyframes.size()) - 1; ++i)
    {
        if (time >= track.keyframes[i].time && time < track.keyframes[i + 1].time)
        { idx = i; break; }
    }

    const Keyframe& a = track.keyframes[idx];
    const Keyframe& b = track.keyframes[idx + 1];

    if (a.interp == InterpMode::Stepped) return a.value;
    const float dt = b.time - a.time;
    if (dt <= 0.f) return a.value;
    const float t = (time - a.time) / dt;
    if (a.interp == InterpMode::Linear)
        return a.value + (b.value - a.value) * t;

    const float t2 = t * t, t3 = t2 * t;
    const float h00 =  2.f*t3 - 3.f*t2 + 1.f;
    const float h10 =  t3 - 2.f*t2 + t;
    const float h01 = -2.f*t3 + 3.f*t2;
    const float h11 =  t3 - t2;
    return h00*a.value + h10*dt*a.tangentOut + h01*b.value + h11*dt*b.tangentIn;
}

// ==== TimelineEditor ====

TimelineEditor::TimelineEditor()  = default;
TimelineEditor::~TimelineEditor() = default;

void TimelineEditor::AddTrack(const std::string& name)
{
    Track t;
    t.name  = name;
    t.color = TrackColorByIndex(static_cast<int>(m_clip.tracks.size()));
    m_clip.tracks.push_back(std::move(t));

    // Put in its own group
    TrackGroup g;
    g.name = name;
    g.trackIndices.push_back(static_cast<int>(m_clip.tracks.size()) - 1);
    m_groups.push_back(std::move(g));
    RebuildDisplayRows();
}

// ==== Display rows: groups + tracks -> a flat row list ====

void TimelineEditor::RebuildDisplayRows()
{
    m_displayRows.clear();
    for (int gi = 0; gi < static_cast<int>(m_groups.size()); ++gi)
    {
        const TrackGroup& grp = m_groups[gi];
        // Skip groups with no tracks
        if (grp.trackIndices.empty()) continue;

        // Only show group header if group has more than 1 track
        bool showHeader = grp.trackIndices.size() > 1;

        if (showHeader)
        {
            DisplayRow hdr;
            hdr.type     = DisplayRow::GroupHeader;
            hdr.groupIdx = gi;
            m_displayRows.push_back(hdr);
        }

        if (!showHeader || grp.expanded)
        {
            for (int ti : grp.trackIndices)
            {
                DisplayRow row;
                row.type     = DisplayRow::TrackRow;
                row.groupIdx = gi;
                row.trackIdx = ti;
                m_displayRows.push_back(row);
            }
        }
    }
}

float TimelineEditor::DisplayRowY(int displayIdx) const
{
    float y = 0.f;
    for (int i = 0; i < displayIdx && i < static_cast<int>(m_displayRows.size()); ++i)
    {
        y += (m_displayRows[i].type == DisplayRow::GroupHeader)
             ? m_state.groupHeight : m_state.trackHeight;
    }
    return y - m_state.scrollY;
}

float TimelineEditor::TotalContentHeight() const
{
    float h = 0.f;
    for (const auto& row : m_displayRows)
        h += (row.type == DisplayRow::GroupHeader) ? m_state.groupHeight : m_state.trackHeight;
    return h;
}

// ==== Load from an .ianim AnimationResource ====

void TimelineEditor::LoadFromAnimationResource(const Resource::AnimationResource& res)
{
    m_clip.tracks.clear();
    m_groups.clear();
    m_clip.currentTime = 0.f;
    m_clip.isPlaying   = false;

    if (res.clips.empty()) return;

    const Resource::AnimClipData& clip = res.clips[0];
    m_clip.name     = clip.name;
    m_clip.duration = clip.duration;
    m_clip.fps      = clip.frameRate;

    int colorIdx = 0;
    for (const Resource::AnimClipChannel& ch : clip.channels)
    {
        const float frameTime = (clip.frameCount > 1)
            ? clip.duration / static_cast<float>(clip.frameCount - 1)
            : 0.f;

        TrackGroup grp;
        grp.name = ch.name;

        // Position X/Y/Z
        const char* posAxes[] = { ".posX", ".posY", ".posZ" };
        for (int axis = 0; axis < 3; ++axis)
        {
            if (ch.positions.empty()) continue;
            Track t;
            t.name  = std::string(ch.name) + posAxes[axis];
            t.color = TrackColorByIndex(colorIdx++);
            for (uint32_t fi = 0; fi < static_cast<uint32_t>(ch.positions.size()); ++fi)
            {
                float time = fi * frameTime;
                float val  = (axis == 0) ? ch.positions[fi].x
                           : (axis == 1) ? ch.positions[fi].y
                                         : ch.positions[fi].z;
                t.keyframes.push_back({ time, val, 0.f, 0.f, InterpMode::Linear, false });
            }
            int idx = static_cast<int>(m_clip.tracks.size());
            m_clip.tracks.push_back(std::move(t));
            grp.trackIndices.push_back(idx);
        }

        // Rotation X/Y/Z/W
        const char* rotAxes[] = { ".rotX", ".rotY", ".rotZ", ".rotW" };
        for (int axis = 0; axis < 4; ++axis)
        {
            if (ch.rotations.empty()) continue;
            Track t;
            t.name  = std::string(ch.name) + rotAxes[axis];
            t.color = TrackColorByIndex(colorIdx++);
            for (uint32_t fi = 0; fi < static_cast<uint32_t>(ch.rotations.size()); ++fi)
            {
                float time = fi * frameTime;
                float val;
                switch (axis)
                {
                    case 0:  val = ch.rotations[fi].x; break;
                    case 1:  val = ch.rotations[fi].y; break;
                    case 2:  val = ch.rotations[fi].z; break;
                    default: val = ch.rotations[fi].w; break;
                }
                t.keyframes.push_back({ time, val, 0.f, 0.f, InterpMode::Linear, false });
            }
            int idx = static_cast<int>(m_clip.tracks.size());
            m_clip.tracks.push_back(std::move(t));
            grp.trackIndices.push_back(idx);
        }

        // Start collapsed by default (bones typically have many channels)
        grp.expanded = false;
        if (!grp.trackIndices.empty())
            m_groups.push_back(std::move(grp));
    }

    // Morph tracks
    for (const Resource::MorphClipData& morphClip : res.morphClips)
    {
        const float frameTime = (morphClip.frameCount > 1)
            ? morphClip.duration / static_cast<float>(morphClip.frameCount - 1)
            : 0.f;

        TrackGroup morphGrp;
        morphGrp.name = "Morph";
        morphGrp.expanded = false;

        for (const Resource::MorphClipChannel& ch : morphClip.channels)
        {
            Track t;
            t.name  = std::string("morph:") + ch.name;
            t.color = TrackColorByIndex(colorIdx++);
            for (uint32_t fi = 0; fi < static_cast<uint32_t>(ch.weights.size()); ++fi)
            {
                float time = fi * frameTime;
                t.keyframes.push_back({ time, ch.weights[fi], 0.f, 0.f, InterpMode::Linear, false });
            }
            int idx = static_cast<int>(m_clip.tracks.size());
            m_clip.tracks.push_back(std::move(t));
            morphGrp.trackIndices.push_back(idx);
        }
        if (!morphGrp.trackIndices.empty())
            m_groups.push_back(std::move(morphGrp));
    }

    RebuildDisplayRows();

    m_state.scrollX = 0.f;
    m_state.scrollY = 0.f;
    m_history.clear();
    m_historyIndex = 0;
    FrameAll();
}

// ==== Undo / redo ====

void TimelineEditor::PushCommand(std::unique_ptr<Command> cmd)
{
    if (m_historyIndex < static_cast<int>(m_history.size()))
        m_history.resize(m_historyIndex);
    m_history.push_back(std::move(cmd));
    m_historyIndex = static_cast<int>(m_history.size());
}

void TimelineEditor::UndoCommand()
{
    if (m_historyIndex > 0)
    {
        --m_historyIndex;
        m_history[m_historyIndex]->Undo();
    }
}

void TimelineEditor::RedoCommand()
{
    if (m_historyIndex < static_cast<int>(m_history.size()))
    {
        m_history[m_historyIndex]->Execute();
        ++m_historyIndex;
    }
}

// ==== Scroll clamping ====

void TimelineEditor::ClampScroll(float visibleWidth, float visibleHeight)
{
    float maxContentPx = m_clip.duration * m_state.zoom + m_state.headerWidth;
    float maxScrollX   = maxContentPx - visibleWidth;
    if (maxScrollX < 0.f) maxScrollX = 0.f;
    if (m_state.scrollX < 0.f) m_state.scrollX = 0.f;
    if (m_state.scrollX > maxScrollX) m_state.scrollX = maxScrollX;

    float totalH    = TotalContentHeight();
    float maxScrollY = totalH - visibleHeight;
    if (maxScrollY < 0.f) maxScrollY = 0.f;
    if (m_state.scrollY < 0.f) m_state.scrollY = 0.f;
    if (m_state.scrollY > maxScrollY) m_state.scrollY = maxScrollY;
}

// ==== Selection helpers ====

void TimelineEditor::SelectAllKeyframes(bool select)
{
    for (auto& track : m_clip.tracks)
        for (auto& kf : track.keyframes)
            kf.selected = select;
}

void TimelineEditor::DeleteSelectedKeyframes()
{
    for (auto& track : m_clip.tracks)
        for (int i = static_cast<int>(track.keyframes.size()) - 1; i >= 0; --i)
            if (track.keyframes[i].selected)
                track.RemoveKeyframe(i);
}

void TimelineEditor::InsertKeyframeAtScrubber()
{
    for (auto& track : m_clip.tracks)
    {
        if (!track.visible) continue;
        float val = EvaluateTrack(track, m_clip.currentTime);
        track.InsertKeyframe(m_clip.currentTime, val);
    }
}

void TimelineEditor::FrameAll()
{
    float minT = 0.f, maxT = m_clip.duration;
    for (const auto& track : m_clip.tracks)
        for (const auto& kf : track.keyframes)
        {
            if (kf.time < minT) minT = kf.time;
            if (kf.time > maxT) maxT = kf.time;
        }
    float range = maxT - minT;
    if (range < 0.01f) range = 1.f;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    float contentW = avail.x - m_state.headerWidth - 20.f;
    if (contentW < 100.f) contentW = 100.f;

    m_state.zoom    = contentW / range;
    m_state.scrollX = minT * m_state.zoom;
    m_state.scrollY = 0.f;
}

// ==== Main Draw ====

void TimelineEditor::Draw(float deltaTime)
{
    // Poll pending async .ianim load
    if (m_pendingLoadHandle != 0 && m_animClipSys)
    {
        Resource::AnimHandle ah;
        ah.packed = m_pendingLoadHandle;
        if (m_animClipSys->IsReady(ah))
        {
            const Resource::AnimationResource* res = m_animClipSys->GetResource(ah);
            if (res) LoadFromAnimationResource(*res);
            m_pendingLoadHandle = 0;
            m_pendingLoadPath.clear();
        }
    }

    // Playback
    if (m_clip.isPlaying)
    {
        m_clip.currentTime += deltaTime;
        if (m_clip.currentTime >= m_clip.duration)
        {
            if (m_clip.isLooping)
                m_clip.currentTime = fmodf(m_clip.currentTime, m_clip.duration);
            else
            {
                m_clip.currentTime = m_clip.duration;
                m_clip.isPlaying   = false;
            }
        }
        if (m_onTimeChanged) m_onTimeChanged(m_clip.currentTime);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    m_windowPos = ImGui::GetCursorScreenPos();

    DrawToolbar();

    ImVec2 regionAvail = ImGui::GetContentRegionAvail();
    float totalWidth  = regionAvail.x;
    float totalHeight = regionAvail.y;

    if (totalWidth < 100.f || totalHeight < 50.f)
    {
        ImGui::PopStyleVar(2);
        return;
    }

    // Empty state
    if (m_clip.tracks.empty())
    {
        DrawDropTarget();
        ImGui::PopStyleVar(2);
        return;
    }

    float rulerY        = ImGui::GetCursorScreenPos().y;
    float contentStartY = rulerY + m_state.rulerHeight;
    float contentHeight = totalHeight - m_state.rulerHeight;
    float contentStartX = ImGui::GetCursorScreenPos().x;
    float contentWidth  = totalWidth;

    ClampScroll(contentWidth - m_state.headerWidth, contentHeight);

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(
        ImVec2(contentStartX, rulerY),
        ImVec2(contentStartX + totalWidth, rulerY + totalHeight),
        IM_COL32(30, 30, 30, 255));

    // InvisibleButton to own all mouse interaction in this area
    ImGui::SetCursorScreenPos(ImVec2(contentStartX, rulerY));
    ImGui::InvisibleButton("##timelineArea", ImVec2(totalWidth, totalHeight),
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight |
                           ImGuiButtonFlags_MouseButtonMiddle);
    bool areaHovered = ImGui::IsItemHovered();
    bool areaActive  = ImGui::IsItemActive();

    DrawRuler(contentStartX, totalWidth, rulerY);
    DrawTrackHeaders(contentStartY, contentHeight);
    DrawTrackContents(contentStartX, contentStartY, contentWidth, contentHeight);
    DrawScrubber(drawList, contentStartX, rulerY, totalHeight);
    HandleInput(contentStartX, contentStartY, contentWidth, contentHeight,
                rulerY, areaHovered, areaActive);

    // Scrollbar indicator on the right edge
    {
        float totalH = TotalContentHeight();
        if (totalH > contentHeight)
        {
            float barX  = contentStartX + contentWidth - 6.f;
            float ratio = contentHeight / totalH;
            float thumbH = contentHeight * ratio;
            if (thumbH < 20.f) thumbH = 20.f;
            float thumbY = contentStartY + (m_state.scrollY / (totalH - contentHeight))
                           * (contentHeight - thumbH);
            drawList->AddRectFilled(
                ImVec2(barX, thumbY),
                ImVec2(barX + 4.f, thumbY + thumbH),
                IM_COL32(200, 200, 200, 80), 2.f);
        }
    }

    DrawDropTarget();
    ImGui::PopStyleVar(2);
}

// ==== Drop target for .ianim files ====

void TimelineEditor::DrawDropTarget()
{
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    bool isDragging = payload && payload->IsDataType("IANIM_PATH");

    if (m_clip.tracks.empty())
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 pos   = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        ImU32 bg = isDragging ? IM_COL32(40, 100, 40, 160) : IM_COL32(45, 45, 48, 255);
        dl->AddRectFilled(pos, ImVec2(pos.x + avail.x, pos.y + avail.y), bg);

        const char* hint = isDragging
            ? "Release to load animation"
            : "Drag .ianim file here to load";
        ImVec2 textSize = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(pos.x + (avail.x - textSize.x) * 0.5f,
                           pos.y + (avail.y - textSize.y) * 0.5f),
                    isDragging ? IM_COL32(120, 255, 120, 255) : IM_COL32(150, 150, 150, 255),
                    hint);

        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##dropEmpty", avail);
    }

    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("IANIM_PATH"))
        {
            const char* path = static_cast<const char*>(p->Data);
            if (m_animClipSys)
            {
                Resource::AnimHandle ah = m_animClipSys->AcquireClip(path);
                if (m_animClipSys->IsReady(ah))
                {
                    const Resource::AnimationResource* res = m_animClipSys->GetResource(ah);
                    if (res) LoadFromAnimationResource(*res);
                }
                else
                {
                    m_pendingLoadHandle = ah.packed;
                    m_pendingLoadPath   = path;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

// ==== Toolbar ====

void TimelineEditor::DrawToolbar()
{
    float toolbarH = m_state.toolbarHeight;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p, ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y + toolbarH),
                      IM_COL32(45, 45, 48, 255));

    ImGui::SetCursorScreenPos(ImVec2(p.x + 4.f, p.y + 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 0));

    if (m_clip.isPlaying)
    { if (ImGui::SmallButton("||")) m_clip.isPlaying = false; }
    else
    { if (ImGui::SmallButton(">"))  m_clip.isPlaying = true;  }
    ImGui::SameLine();

    if (ImGui::SmallButton("[]"))
    {
        m_clip.isPlaying = false;
        m_clip.currentTime = 0.f;
        if (m_onTimeChanged) m_onTimeChanged(0.f);
    }
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button,
        m_clip.isLooping ? IM_COL32(80, 120, 200, 255) : IM_COL32(60, 60, 60, 255));
    if (ImGui::SmallButton("Loop")) m_clip.isLooping = !m_clip.isLooping;
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();

    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%.2f / %.2f s", m_clip.currentTime, m_clip.duration);
    ImGui::Text("%s", timeBuf); ImGui::SameLine();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
    ImGui::Text("FPS:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(50.f);
    ImGui::DragFloat("##fps", &m_clip.fps, 1.f, 1.f, 120.f, "%.0f"); ImGui::SameLine();
    ImGui::Text("Dur:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(60.f);
    ImGui::DragFloat("##dur", &m_clip.duration, 0.1f, 0.1f, 600.f, "%.1f"); ImGui::SameLine();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();

    // Expand all / Collapse all
    if (ImGui::SmallButton("E"))
    {
        for (auto& g : m_groups) g.expanded = true;
        RebuildDisplayRows();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Expand All");
    ImGui::SameLine();
    if (ImGui::SmallButton("C"))
    {
        for (auto& g : m_groups) g.expanded = false;
        RebuildDisplayRows();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Collapse All");
    ImGui::SameLine();

    if (!m_clip.name.empty())
    {
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.f, 1.f), "%s (%d bones)",
                           m_clip.name.c_str(), static_cast<int>(m_groups.size()));
    }

    if (m_pendingLoadHandle != 0)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.f, 1.f, 0.3f, 1.f), "  Loading...");
    }

    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + toolbarH));
}

// ==== Track headers: the left column (group headers + track names) ====

void TimelineEditor::DrawTrackHeaders(float contentStartY, float contentHeight)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = m_windowPos.x;

    dl->AddRectFilled(
        ImVec2(x, contentStartY),
        ImVec2(x + m_state.headerWidth, contentStartY + contentHeight),
        IM_COL32(40, 40, 43, 255));
    dl->AddLine(
        ImVec2(x + m_state.headerWidth, contentStartY),
        ImVec2(x + m_state.headerWidth, contentStartY + contentHeight),
        IM_COL32(70, 70, 70, 255), 1.f);

    dl->PushClipRect(ImVec2(x, contentStartY),
                     ImVec2(x + m_state.headerWidth, contentStartY + contentHeight), true);

    for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
    {
        const DisplayRow& row = m_displayRows[di];
        float y  = contentStartY + DisplayRowY(di);
        float rh = (row.type == DisplayRow::GroupHeader) ? m_state.groupHeight : m_state.trackHeight;

        if (y + rh < contentStartY || y > contentStartY + contentHeight)
            continue;

        if (row.type == DisplayRow::GroupHeader)
        {
            // Group header row
            TrackGroup& grp = m_groups[row.groupIdx];

            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + m_state.headerWidth, y + rh),
                              IM_COL32(50, 50, 55, 255));

            // Expand/collapse triangle
            const char* arrow = grp.expanded ? "v" : ">";
            ImU32 arrowCol = IM_COL32(200, 200, 200, 255);
            dl->AddText(ImVec2(x + 6.f, y + (rh - ImGui::GetTextLineHeight()) * 0.5f),
                        arrowCol, arrow);

            // Group name + child count
            char label[128];
            snprintf(label, sizeof(label), "%s (%d)", grp.name.c_str(),
                     static_cast<int>(grp.trackIndices.size()));
            dl->AddText(ImVec2(x + 20.f, y + (rh - ImGui::GetTextLineHeight()) * 0.5f),
                        IM_COL32(220, 220, 220, 255), label);

            dl->AddLine(ImVec2(x, y + rh), ImVec2(x + m_state.headerWidth, y + rh),
                        IM_COL32(65, 65, 65, 255));
        }
        else
        {
            // Track row
            Track& track = m_clip.tracks[row.trackIdx];
            bool isChild = m_groups[row.groupIdx].trackIndices.size() > 1;
            float indent = isChild ? 20.f : 6.f;

            ImU32 rowBg = (di & 1) ? IM_COL32(38, 38, 42, 255) : IM_COL32(34, 34, 38, 255);
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + m_state.headerWidth, y + rh), rowBg);

            // Color indicator
            ImU32 colInd = ImGui::ColorConvertFloat4ToU32(track.color);
            dl->AddRectFilled(ImVec2(x, y + 2.f), ImVec2(x + 3.f, y + rh - 2.f), colInd);

            // Track name (truncated to fit)
            // Show only the suffix after the bone name for child tracks
            const char* displayName = track.name.c_str();
            if (isChild)
            {
                const char* dot = strrchr(displayName, '.');
                if (dot) displayName = dot; // e.g. ".posX"
            }

            ImVec4 textCol = track.visible ? ImVec4(0.85f, 0.85f, 0.85f, 1.f)
                                           : ImVec4(0.5f, 0.5f, 0.5f, 1.f);
            dl->AddText(ImVec2(x + indent, y + (rh - ImGui::GetTextLineHeight()) * 0.5f),
                        ImGui::ColorConvertFloat4ToU32(textCol), displayName);

            dl->AddLine(ImVec2(x, y + rh), ImVec2(x + m_state.headerWidth, y + rh),
                        IM_COL32(50, 50, 50, 255));
        }
    }

    dl->PopClipRect();
}

// ==== Ruler ====

void TimelineEditor::DrawRuler(float rulerStartX, float rulerWidth, float rulerY)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rulerH = m_state.rulerHeight;

    dl->AddRectFilled(ImVec2(rulerStartX, rulerY),
                      ImVec2(rulerStartX + rulerWidth, rulerY + rulerH),
                      IM_COL32(50, 50, 55, 255));
    dl->AddLine(ImVec2(rulerStartX, rulerY + rulerH),
                ImVec2(rulerStartX + rulerWidth, rulerY + rulerH),
                IM_COL32(80, 80, 80, 255));

    float majorStep, minorStep;
    if (m_state.zoom < 20.f)       { majorStep = 5.0f;  minorStep = 1.0f; }
    else if (m_state.zoom <= 200.f){ majorStep = 1.0f;  minorStep = 0.25f; }
    else                           { majorStep = 0.1f;  minorStep = 0.01f; }

    float timeStart = m_state.PixelToTime(m_state.headerWidth);
    float timeEnd   = m_state.PixelToTime(rulerWidth);
    float firstTick = floorf(timeStart / minorStep) * minorStep;

    dl->PushClipRect(ImVec2(rulerStartX + m_state.headerWidth, rulerY),
                     ImVec2(rulerStartX + rulerWidth, rulerY + rulerH), true);

    for (float t = firstTick; t <= timeEnd; t += minorStep)
    {
        if (t < 0.f) continue;
        float px = rulerStartX + m_state.TimeToPixel(t);

        bool isMajor = fabsf(fmodf(t + majorStep * 0.001f, majorStep)) < majorStep * 0.01f;
        float lineH = isMajor ? rulerH * 0.7f : rulerH * 0.35f;
        ImU32 lineCol = isMajor ? IM_COL32(180, 180, 180, 255) : IM_COL32(100, 100, 100, 255);
        dl->AddLine(ImVec2(px, rulerY + rulerH - lineH), ImVec2(px, rulerY + rulerH), lineCol);

        if (isMajor)
        {
            char label[16];
            if (majorStep >= 1.f) snprintf(label, sizeof(label), "%.0fs", t);
            else                  snprintf(label, sizeof(label), "%.1fs", t);
            dl->AddText(ImVec2(px + 2.f, rulerY + 2.f), IM_COL32(200, 200, 200, 255), label);
        }
    }
    dl->PopClipRect();
}

// ==== Track contents: grid lines + keyframes in the right area ====

void TimelineEditor::DrawTrackContents(float contentStartX, float contentStartY,
                                        float contentWidth, float contentHeight)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float trackAreaX = contentStartX + m_state.headerWidth;
    float trackAreaW = contentWidth - m_state.headerWidth;

    dl->PushClipRect(ImVec2(trackAreaX, contentStartY),
                     ImVec2(trackAreaX + trackAreaW, contentStartY + contentHeight), true);

    // Row backgrounds
    for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
    {
        const DisplayRow& row = m_displayRows[di];
        float y  = contentStartY + DisplayRowY(di);
        float rh = (row.type == DisplayRow::GroupHeader) ? m_state.groupHeight : m_state.trackHeight;

        if (y + rh < contentStartY || y > contentStartY + contentHeight) continue;

        if (row.type == DisplayRow::GroupHeader)
        {
            dl->AddRectFilled(ImVec2(trackAreaX, y), ImVec2(trackAreaX + trackAreaW, y + rh),
                              IM_COL32(45, 45, 50, 255));
            dl->AddLine(ImVec2(trackAreaX, y + rh), ImVec2(trackAreaX + trackAreaW, y + rh),
                        IM_COL32(60, 60, 60, 255));
        }
        else
        {
            ImU32 rowBg = (di & 1) ? IM_COL32(36, 36, 40, 255) : IM_COL32(32, 32, 36, 255);
            dl->AddRectFilled(ImVec2(trackAreaX, y), ImVec2(trackAreaX + trackAreaW, y + rh), rowBg);
            dl->AddLine(ImVec2(trackAreaX, y + rh), ImVec2(trackAreaX + trackAreaW, y + rh),
                        IM_COL32(50, 50, 50, 255));
        }
    }

    // Vertical grid lines
    float majorStep = (m_state.zoom < 20.f) ? 5.f : (m_state.zoom <= 200.f) ? 1.f : 0.1f;
    float timeStart = m_state.PixelToTime(0.f);
    float timeEnd   = m_state.PixelToTime(trackAreaW);
    float firstTick = floorf(timeStart / majorStep) * majorStep;

    for (float t = firstTick; t <= timeEnd; t += majorStep)
    {
        float px = contentStartX + m_state.TimeToPixel(t);
        if (px < trackAreaX || px > trackAreaX + trackAreaW) continue;
        dl->AddLine(ImVec2(px, contentStartY), ImVec2(px, contentStartY + contentHeight),
                    IM_COL32(50, 50, 55, 200));
    }

    // Duration end marker
    float endPx = contentStartX + m_state.TimeToPixel(m_clip.duration);
    if (endPx >= trackAreaX && endPx <= trackAreaX + trackAreaW)
        dl->AddLine(ImVec2(endPx, contentStartY), ImVec2(endPx, contentStartY + contentHeight),
                    IM_COL32(120, 50, 50, 200), 2.f);

    // Draw keyframes for each visible track row
    for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
    {
        if (m_displayRows[di].type == DisplayRow::TrackRow)
            DrawKeyframesForRow(dl, di, contentStartX, contentStartY, contentWidth);
    }

    // Box selection rectangle
    if (m_editMode == EditMode::BoxSelect)
    {
        ImVec2 bMin(fminf(m_boxSelectStart.x, m_boxSelectEnd.x),
                    fminf(m_boxSelectStart.y, m_boxSelectEnd.y));
        ImVec2 bMax(fmaxf(m_boxSelectStart.x, m_boxSelectEnd.x),
                    fmaxf(m_boxSelectStart.y, m_boxSelectEnd.y));
        dl->AddRectFilled(bMin, bMax, IM_COL32(100, 150, 255, 40));
        dl->AddRect(bMin, bMax, IM_COL32(100, 150, 255, 180));
    }

    dl->PopClipRect();
}

// ==== Keyframes: diamond shapes per display row ====

void TimelineEditor::DrawKeyframesForRow(ImDrawList* drawList, int displayIdx,
                                          float contentStartX, float contentStartY,
                                          float contentWidth)
{
    const DisplayRow& row = m_displayRows[displayIdx];
    if (row.type != DisplayRow::TrackRow) return;

    Track& track = m_clip.tracks[row.trackIdx];
    if (!track.visible) return;

    float y = contentStartY + DisplayRowY(displayIdx) + m_state.trackHeight * 0.5f;
    float halfSize = 4.f;
    float trackAreaX = contentStartX + m_state.headerWidth;

    ImU32 colNormal   = IM_COL32(200, 180, 60, 255);
    ImU32 colHover    = IM_COL32(255, 255, 100, 255);
    ImU32 colSelected = IM_COL32(255, 200, 0, 255);

    ImVec2 mousePos = ImGui::GetMousePos();

    for (auto& kf : track.keyframes)
    {
        float px = contentStartX + m_state.TimeToPixel(kf.time);
        if (px < trackAreaX - halfSize || px > contentStartX + contentWidth + halfSize)
            continue;

        bool hovered = (fabsf(mousePos.x - px) < halfSize + 2.f &&
                        fabsf(mousePos.y - y)  < halfSize + 2.f);
        ImU32 col = kf.selected ? colSelected : (hovered ? colHover : colNormal);

        ImVec2 pts[4] = {
            ImVec2(px,            y - halfSize),
            ImVec2(px + halfSize, y),
            ImVec2(px,            y + halfSize),
            ImVec2(px - halfSize, y)
        };
        drawList->AddConvexPolyFilled(pts, 4, col);
        drawList->AddPolyline(pts, 4, IM_COL32(255, 255, 255, 100), ImDrawFlags_Closed, 1.f);
    }
}

// ==== Scrubber ====

void TimelineEditor::DrawScrubber(ImDrawList* drawList, float contentStartX,
                                   float rulerY, float totalHeight)
{
    float px = contentStartX + m_state.TimeToPixel(m_clip.currentTime);
    float minX = contentStartX + m_state.headerWidth;
    if (px < minX) return;

    drawList->AddLine(ImVec2(px, rulerY), ImVec2(px, rulerY + totalHeight),
                      IM_COL32(220, 50, 50, 230), 2.f);

    float s = 6.f;
    ImVec2 tri[3] = {
        ImVec2(px - s, rulerY),
        ImVec2(px + s, rulerY),
        ImVec2(px,     rulerY + s * 1.5f)
    };
    drawList->AddTriangleFilled(tri[0], tri[1], tri[2], IM_COL32(220, 50, 50, 255));
}

// ==== Input handling ====

void TimelineEditor::HandleInput(float contentStartX, float contentStartY,
                                  float contentWidth, float contentHeight, float rulerY,
                                  bool areaHovered, bool areaActive)
{
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    float trackAreaX = contentStartX + m_state.headerWidth;
    float visibleW = contentWidth - m_state.headerWidth;

    bool inTrackArea = (mouse.x >= trackAreaX &&
                        mouse.x <= contentStartX + contentWidth &&
                        mouse.y >= contentStartY &&
                        mouse.y <= contentStartY + contentHeight);
    bool inRuler = (mouse.x >= trackAreaX &&
                    mouse.x <= contentStartX + contentWidth &&
                    mouse.y >= rulerY &&
                    mouse.y < contentStartY);
    bool inHeader = (mouse.x >= contentStartX &&
                     mouse.x < trackAreaX &&
                     mouse.y >= contentStartY &&
                     mouse.y <= contentStartY + contentHeight);

    // ---- Group expand/collapse on header click ----
    if (inHeader && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
        {
            const DisplayRow& row = m_displayRows[di];
            if (row.type != DisplayRow::GroupHeader) continue;

            float y  = contentStartY + DisplayRowY(di);
            float rh = m_state.groupHeight;
            if (mouse.y >= y && mouse.y < y + rh)
            {
                m_groups[row.groupIdx].expanded = !m_groups[row.groupIdx].expanded;
                RebuildDisplayRows();
                break;
            }
        }
    }

    // ---- Keyboard shortcuts ----
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
            m_clip.isPlaying = !m_clip.isPlaying;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            DeleteSelectedKeyframes();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false))
            SelectAllKeyframes(true);
        if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            UndoCommand();
        if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
            (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
            RedoCommand();
        if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)
            InsertKeyframeAtScrubber();
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
            FrameAll();
    }

    // ---- Scroll wheel ----
    if (areaHovered && m_editMode == EditMode::None)
    {
        float wheel = io.MouseWheel;
        if (fabsf(wheel) > 0.01f)
        {
            if (io.KeyShift)
            {
                m_state.scrollY -= wheel * 40.f;
            }
            else if (io.KeyAlt)
            {
                m_state.scrollX -= wheel * 40.f;
            }
            else
            {
                // Default scroll = vertical (most natural for long track lists)
                if (inTrackArea || inHeader)
                {
                    m_state.scrollY -= wheel * 40.f;
                }
                else
                {
                    // On ruler = zoom
                    float timeAtCursor = m_state.PixelToTime(mouse.x - contentStartX);
                    float zoomFactor = (wheel > 0.f) ? 1.15f : (1.f / 1.15f);
                    m_state.zoom *= zoomFactor;
                    m_state.zoom = (m_state.zoom < 5.f) ? 5.f : (m_state.zoom > 5000.f) ? 5000.f : m_state.zoom;
                    float newPx = m_state.headerWidth + timeAtCursor * m_state.zoom;
                    m_state.scrollX += newPx - (mouse.x - contentStartX);
                }
            }
            ClampScroll(visibleW, contentHeight);
        }

        // Horizontal scroll with mouse wheel horizontal (some mice have this)
        float wheelH = io.MouseWheelH;
        if (fabsf(wheelH) > 0.01f)
        {
            m_state.scrollX -= wheelH * 40.f;
            ClampScroll(visibleW, contentHeight);
        }
    }

    // ---- Edit mode state machine ----
    switch (m_editMode)
    {
    case EditMode::None:
    {
        if (!areaHovered && !areaActive) break;

        // Ruler click = scrubber
        if (inRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            m_editMode = EditMode::DragScrubber;
            float t = m_state.PixelToTime(mouse.x - contentStartX);
            m_clip.currentTime = (t < 0.f) ? 0.f : (t > m_clip.duration) ? m_clip.duration : t;
            if (m_onTimeChanged) m_onTimeChanged(m_clip.currentTime);
            break;
        }

        // Scrubber handle hit
        {
            float scrubPx = contentStartX + m_state.TimeToPixel(m_clip.currentTime);
            if (fabsf(mouse.x - scrubPx) < 8.f && mouse.y >= rulerY && mouse.y <= rulerY + 16.f &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_editMode = EditMode::DragScrubber;
                break;
            }
        }

        // Middle-click or Ctrl+Left = pan
        if ((inTrackArea || inRuler || inHeader) &&
            (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
             (io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left))))
        {
            m_editMode = EditMode::PanCanvas;
            break;
        }

        // Left-click in track area: keyframe select or box select
        if (inTrackArea && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            bool hitKf = false;
            for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
            {
                const DisplayRow& row = m_displayRows[di];
                if (row.type != DisplayRow::TrackRow) continue;

                Track& track = m_clip.tracks[row.trackIdx];
                if (!track.visible) continue;
                float ky = contentStartY + DisplayRowY(di) + m_state.trackHeight * 0.5f;

                for (int ki = 0; ki < static_cast<int>(track.keyframes.size()); ++ki)
                {
                    Keyframe& kf = track.keyframes[ki];
                    float kx = contentStartX + m_state.TimeToPixel(kf.time);
                    if (fabsf(mouse.x - kx) < 6.f && fabsf(mouse.y - ky) < 6.f)
                    {
                        if (!io.KeyShift) SelectAllKeyframes(false);
                        kf.selected = true;
                        if (!track.locked)
                        {
                            m_editMode     = EditMode::DragKeyframe;
                            m_dragTrackIdx = row.trackIdx;
                            m_dragKeyIdx   = ki;
                            m_dragStartTime = kf.time;
                        }
                        hitKf = true;
                        break;
                    }
                }
                if (hitKf) break;
            }

            if (!hitKf)
            {
                if (!io.KeyShift) SelectAllKeyframes(false);
                m_editMode = EditMode::BoxSelect;
                m_boxSelectStart = m_boxSelectEnd = mouse;
            }
        }

        // Right-click context menu
        if (inTrackArea && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
            {
                const DisplayRow& row = m_displayRows[di];
                if (row.type != DisplayRow::TrackRow) continue;
                Track& track = m_clip.tracks[row.trackIdx];
                if (!track.visible) continue;
                float ky = contentStartY + DisplayRowY(di) + m_state.trackHeight * 0.5f;
                for (auto& kf : track.keyframes)
                {
                    float kx = contentStartX + m_state.TimeToPixel(kf.time);
                    if (fabsf(mouse.x - kx) < 6.f && fabsf(mouse.y - ky) < 6.f)
                    {
                        kf.selected = true;
                        ImGui::OpenPopup("##KfCtxMenu");
                        break;
                    }
                }
            }
        }
        break;
    }

    case EditMode::DragScrubber:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float t = m_state.PixelToTime(mouse.x - contentStartX);
            m_clip.currentTime = (t < 0.f) ? 0.f : (t > m_clip.duration) ? m_clip.duration : t;
            if (m_onTimeChanged) m_onTimeChanged(m_clip.currentTime);
        }
        else m_editMode = EditMode::None;
        break;

    case EditMode::DragKeyframe:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            float dt = io.MouseDelta.x / m_state.zoom;
            for (auto& track : m_clip.tracks)
            {
                if (track.locked) continue;
                for (auto& kf : track.keyframes)
                    if (kf.selected) { kf.time += dt; if (kf.time < 0.f) kf.time = 0.f; }
            }
        }
        else
        {
            if (m_dragTrackIdx >= 0 && m_dragTrackIdx < static_cast<int>(m_clip.tracks.size()) &&
                m_dragKeyIdx >= 0 && m_dragKeyIdx < static_cast<int>(m_clip.tracks[m_dragTrackIdx].keyframes.size()))
            {
                auto cmd = std::make_unique<MoveKeyframeCmd>();
                cmd->track   = &m_clip.tracks[m_dragTrackIdx];
                cmd->index   = m_dragKeyIdx;
                cmd->oldTime = m_dragStartTime;
                cmd->newTime = m_clip.tracks[m_dragTrackIdx].keyframes[m_dragKeyIdx].time;
                if (fabsf(cmd->oldTime - cmd->newTime) > 0.0001f)
                    PushCommand(std::move(cmd));
            }
            for (auto& track : m_clip.tracks) track.SortKeyframes();
            m_editMode     = EditMode::None;
            m_dragTrackIdx = -1;
            m_dragKeyIdx   = -1;
        }
        break;

    case EditMode::BoxSelect:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            m_boxSelectEnd = mouse;
            ImVec2 bMin(fminf(m_boxSelectStart.x, m_boxSelectEnd.x),
                        fminf(m_boxSelectStart.y, m_boxSelectEnd.y));
            ImVec2 bMax(fmaxf(m_boxSelectStart.x, m_boxSelectEnd.x),
                        fmaxf(m_boxSelectStart.y, m_boxSelectEnd.y));

            for (int di = 0; di < static_cast<int>(m_displayRows.size()); ++di)
            {
                const DisplayRow& row = m_displayRows[di];
                if (row.type != DisplayRow::TrackRow) continue;
                Track& track = m_clip.tracks[row.trackIdx];
                float ky = contentStartY + DisplayRowY(di) + m_state.trackHeight * 0.5f;
                for (auto& kf : track.keyframes)
                {
                    float px = contentStartX + m_state.TimeToPixel(kf.time);
                    kf.selected = (px >= bMin.x && px <= bMax.x &&
                                   ky >= bMin.y && ky <= bMax.y);
                }
            }
        }
        else m_editMode = EditMode::None;
        break;

    case EditMode::PanCanvas:
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
            (io.KeyCtrl && ImGui::IsMouseDown(ImGuiMouseButton_Left)))
        {
            m_state.scrollX -= io.MouseDelta.x;
            m_state.scrollY -= io.MouseDelta.y;
            ClampScroll(visibleW, contentHeight);
        }
        else m_editMode = EditMode::None;
        break;
    }

    // Context menu
    if (ImGui::BeginPopup("##KfCtxMenu"))
    {
        if (ImGui::MenuItem("Delete Selected"))
            DeleteSelectedKeyframes();
        ImGui::Separator();
        if (ImGui::MenuItem("Linear"))
            for (auto& tr : m_clip.tracks) for (auto& kf : tr.keyframes)
                if (kf.selected) kf.interp = InterpMode::Linear;
        if (ImGui::MenuItem("Bezier"))
            for (auto& tr : m_clip.tracks) for (auto& kf : tr.keyframes)
                if (kf.selected) kf.interp = InterpMode::Bezier;
        if (ImGui::MenuItem("Stepped"))
            for (auto& tr : m_clip.tracks) for (auto& kf : tr.keyframes)
                if (kf.selected) kf.interp = InterpMode::Stepped;
        ImGui::EndPopup();
    }
}
