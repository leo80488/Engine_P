#pragma once

// TimelineEditor - ImGui animation timeline editor: multi-track keyframe editing
// with collapsible bone groups, Bezier (cubic Hermite) interpolation, scrubber
// playback, box select, zoom / scroll and undo / redo. Accepts .ianim
// drag-and-drop from the asset browser.

#include "imgui/imgui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Resource { class AnimationClipSystem; class AnimationResource; }

namespace Timeline
{
    // Interpolation mode between two keyframes.
    enum class InterpMode : uint8_t
    {
        Linear,
        Bezier,   // Cubic Hermite with tangents
        Stepped   // Hold previous value until next keyframe
    };

    // One point on a track.
    struct Keyframe
    {
        float      time       = 0.f;
        float      value      = 0.f;
        float      tangentIn  = 0.f;
        float      tangentOut = 0.f;
        InterpMode interp     = InterpMode::Linear;
        bool       selected   = false;
    };

    // A named channel of keyframes.
    struct Track
    {
        std::string           name;
        std::vector<Keyframe> keyframes;   // sorted by time ascending
        ImVec4                color = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
        bool                  visible = true;
        bool                  solo    = false;
        bool                  locked  = false;

        void SortKeyframes();
        int  InsertKeyframe(float time, float value);
        void RemoveKeyframe(int index);
    };

    // A collapsible group header (e.g. one bone's channels).
    struct TrackGroup
    {
        std::string       name;         // group name (bone name)
        std::vector<int>  trackIndices; // indices into AnimationClip::tracks
        bool              expanded = true;
    };

    // One visible row in the timeline: a group header or a child track. Rebuilt by
    // RebuildDisplayRows when the groups or their expanded state change.
    struct DisplayRow
    {
        enum Type { GroupHeader, TrackRow };
        Type type       = TrackRow;
        int  groupIdx   = -1;   // index into m_groups
        int  trackIdx   = -1;   // index into m_clip.tracks (only for TrackRow)
    };

    // Tracks sharing one duration.
    struct AnimationClip
    {
        std::string        name       = "Untitled";
        float              duration   = 10.0f;
        float              fps        = 24.0f;
        std::vector<Track> tracks;
        float              currentTime = 0.0f;
        bool               isPlaying   = false;
        bool               isLooping   = true;
    };

    // View state: scroll, zoom, layout.
    struct TimelineState
    {
        float scrollX     = 0.0f;
        float scrollY     = 0.0f;
        float zoom        = 100.0f;   // pixels per second
        float trackHeight = 24.0f;
        float groupHeight = 26.0f;    // slightly taller for group headers
        float headerWidth = 200.0f;
        float rulerHeight = 28.0f;
        float toolbarHeight = 32.0f;

        float TimeToPixel(float time) const
        {
            return headerWidth + (time * zoom) - scrollX;
        }
        float PixelToTime(float px) const
        {
            return (px - headerWidth + scrollX) / zoom;
        }
    };

    // The pointer-interaction state machine.
    enum class EditMode
    {
        None,
        DragKeyframe,
        DragScrubber,
        BoxSelect,
        PanCanvas
    };

    // An undoable edit.
    struct Command
    {
        virtual ~Command() = default;
        virtual void Execute() = 0;
        virtual void Undo()    = 0;
    };

    // The editor window; call Draw() each frame.
    class TimelineEditor
    {
    public:
        TimelineEditor();
        ~TimelineEditor();

        void Draw(float deltaTime);

        AnimationClip&       GetClip()       { return m_clip; }
        const AnimationClip& GetClip() const { return m_clip; }

        static float EvaluateTrack(const Track& track, float time);

        void AddTrack(const std::string& name);
        void LoadFromAnimationResource(const Resource::AnimationResource& res);

        void SetAnimationClipSystem(Resource::AnimationClipSystem* acs) { m_animClipSys = acs; }
        void SetOnTimeChanged(std::function<void(float)> cb) { m_onTimeChanged = std::move(cb); }

    private:
        // Builds m_displayRows from m_groups and their expanded state.
        void RebuildDisplayRows();
        // Y pixel offset of display row @p displayIdx from the content top.
        float DisplayRowY(int displayIdx) const;
        // Total content height in pixels
        float TotalContentHeight() const;

        // ---- Drawing ----
        void DrawToolbar();
        void DrawTrackHeaders(float contentStartY, float contentHeight);
        void DrawRuler(float rulerStartX, float rulerWidth, float rulerY);
        void DrawTrackContents(float contentStartX, float contentStartY,
                               float contentWidth, float contentHeight);
        void DrawKeyframesForRow(ImDrawList* drawList, int displayIdx,
                                 float contentStartX, float contentStartY,
                                 float contentWidth);
        void DrawScrubber(ImDrawList* drawList, float contentStartX,
                          float rulerY, float totalHeight);
        void DrawDropTarget();
        void HandleInput(float contentStartX, float contentStartY,
                         float contentWidth, float contentHeight, float rulerY,
                         bool areaHovered, bool areaActive);

        void ClampScroll(float visibleWidth, float visibleHeight);

        // ---- Undo / redo ----
        void PushCommand(std::unique_ptr<Command> cmd);
        void UndoCommand();
        void RedoCommand();

        // ---- Helpers ----
        void SelectAllKeyframes(bool select);
        void DeleteSelectedKeyframes();
        void InsertKeyframeAtScrubber();
        void FrameAll();

        // ---- State ----
        AnimationClip              m_clip;
        std::vector<TrackGroup>    m_groups;
        std::vector<DisplayRow>    m_displayRows;
        TimelineState              m_state;
        EditMode                   m_editMode = EditMode::None;

        // Drag state
        int   m_dragTrackIdx    = -1;
        int   m_dragKeyIdx      = -1;
        float m_dragStartTime   = 0.f;

        // Box-select
        ImVec2 m_boxSelectStart = {};
        ImVec2 m_boxSelectEnd   = {};

        // Undo history; m_historyIndex counts the applied commands, entries past it are
        // the redo tail.
        std::vector<std::unique_ptr<Command>> m_history;
        int m_historyIndex = 0;

        // Called with the new current time when playback, the scrubber or Stop moves it.
        std::function<void(float)> m_onTimeChanged;

        // Cached window position
        ImVec2 m_windowPos = {};

        // Animation clip system for drag-and-drop .ianim loading
        Resource::AnimationClipSystem* m_animClipSys = nullptr;

        // Pending async load handle (packed uint32_t, 0 = none)
        uint32_t m_pendingLoadHandle = 0;
        std::string m_pendingLoadPath;
    };

} // namespace Timeline
