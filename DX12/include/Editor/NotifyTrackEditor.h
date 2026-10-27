#pragma once

// NotifyTrackEditor - ImGui multi-track editor for a TimelineComponent: named tracks,
// each with point Notifies and NotifyStates (interval events). Not TimelineEditor.h,
// which edits bone keyframe curves.
//
// Use from EditorLayer (or any ImGui host), per frame:
//     m_notifyEditor.SetTarget(world->GetComponent<TimelineComponent>(e));
//     m_notifyEditor.SetEditingEntity(e, world);
//     m_notifyEditor.RenderInWindow("Timeline Editor", &m_showTimeline);
//
// Tracks with a header (name, category, mute), a zoom/scroll ruler, point notifies as
// triangles and states as resizable rectangles; click-select, drag-move (Shift = 60 Hz
// frame snap), a right-click menu (Add Notify / Add NotifyState / Duplicate / Delete),
// and a property panel per category through NotifyEditorRegistry. Preview writes the
// scrubber into AnimationComponent::primaryTime on the editing entity, so notifies fire
// through the real TimelineSystem mailboxes ("editor sees == game sees"). Undo / redo
// (Ctrl+Z / Ctrl+Y) replays INotifyCommands, the last kMaxHistory kept; a drag pushes one
// command on release.

#include "ECS/ECS.h"
#include "ECS/NotifyTypes.h"

#include <functional>
#include <memory>
#include <vector>

namespace Editor
{

// One undoable edit of a TimelineComponent: Apply does it, Revert undoes it.
struct INotifyCommand
{
    virtual ~INotifyCommand() = default;
    virtual void Apply  (TimelineComponent& tl) = 0;
    virtual void Revert (TimelineComponent& tl) = 0;
    virtual const char* Name() const = 0;
};

class NotifyTrackEditor
{
public:
    // Target: the TimelineComponent we edit. Pass nullptr to show the
    // "select an entity" empty state.
    void SetTarget(TimelineComponent* tl);

    // Editing context: Preview mode writes into this entity's AnimationComponent.
    // Optional - without it Preview is an editor-only scrubber that fires no real notifies.
    void SetEditingEntity(Entity e, World* world) { m_editingEntity = e; m_world = world; }

    TimelineComponent* GetTarget() const { return m_target; }

    // The tracks' own history (the panel's Ctrl+Z / Ctrl+Y; the Edit menu reaches it too).
    void Undo();
    void Redo();

    // Called after every edit, undo and redo of the target: the host marks what owns the target
    // (the scene, for an entity's Timeline) unsaved. Empty = nothing to tell.
    void SetOnEdited(std::function<void()> onEdited) { m_onEdited = std::move(onEdited); }

    // Current editor preview/scrubber time in clip-local seconds. Lets a host
    // panel align other views (e.g. a bone-curve overlay) to the same cursor.
    float GetPreviewTime() const { return m_previewTime; }

    // Convenience wrapper that issues ImGui::Begin/End around Render().
    void RenderInWindow(const char* title, bool* pOpen);

    // Body-only render (caller already inside an ImGui::Begin). Useful if
    // you want to dock this into a sub-region of another window.
    void Render();

    // True when the host window had focus during the last Render(): the editor layer's
    // top-of-frame shortcut dispatcher yields Ctrl+Z/Y to a focused panel that owns its own
    // undo history (one frame behind). CONSUMES the flag, so a panel that stops rendering
    // cannot leave a stale claim on the dispatcher.
    bool ConsumeFocusedLastRender()
    {
        const bool f = m_wasFocused;
        m_wasFocused = false;
        return f;
    }

private:
    // ---- helpers ----
    enum class SelKind : uint8_t { None, Notify, NotifyState };
    struct Selection {
        SelKind  kind = SelKind::None;
        uint32_t id   = 0u;
        bool operator==(const Selection& o) const { return kind == o.kind && id == o.id; }
    };

    enum class DragMode : uint8_t { None, Move, ResizeStart, ResizeEnd, BoxSelect };

    // ---- clipboard ----
    struct ClipboardItem {
        SelKind     kind;
        Notify      n;
        NotifyState s;
        float       relTime;        // for Notify: time - boxLeft
        float       relTime2;       // for NotifyState: endTime - boxLeft
        int         trackOffset;    // delta from the topmost copied track
    };

    static constexpr size_t kMaxHistory = 128;

    // ---- cached layout ----
    static constexpr float kHeaderColumnWidth = 240.f;  // roomier two-row header
    static constexpr float kTrackHeight       = 48.f;   // fits name row + controls row
    static constexpr float kRulerHeight       = 24.f;

    // ---- pixel <-> time mapping ----
    float TimeToPixel(float t) const { return t * m_pixelsPerSecond - m_scrollX; }
    float PixelToTime(float px) const { return (px + m_scrollX) / m_pixelsPerSecond; }

    // ---- sub-draws ----
    void DrawToolbar();
    void DrawRuler(float canvasX, float canvasY, float width);
    void DrawTrackHeader(int trackIdx, float canvasX, float rowY, float headerWidth);
    void DrawTrackContent(int trackIdx, float canvasX, float rowY, float width);
    void DrawCurrentTimeCursor(float canvasX, float topY, float bottomY);
    void DrawPropertyPanel();
    void HandleContextMenu();

    // ---- preview integration ----
    // Pushes m_previewTime into AnimationComponent::primaryTime on the editing entity so the
    // runtime path (AnimationSystem -> TimelineSystem -> mailboxes -> consumer systems) sees
    // the time drawn here.
    void ApplyPreviewTimeToEntity();

    // ---- helpers ----
    Notify*      FindNotifyById     (uint32_t id, int* outTrackIdx = nullptr);
    NotifyState* FindNotifyStateById(uint32_t id, int* outTrackIdx = nullptr);

    // Selection is the "primary" pick (used by property panel). m_multiSel
    // holds additional picks for batch operations (delete, copy/paste).
    bool IsInMultiSel(const Selection& s) const;
    void AddToMultiSel(const Selection& s);
    void ClearMultiSel();
    void SelectExclusive(const Selection& s);

    void DeleteSelection();
    void DuplicateSelection();
    void CopySelectionToClipboard();
    void PasteClipboardAtTime(float t);

    // Widen the timeline's clipDuration to cover AnimationComponent::primaryTime (a stand-in
    // for the active clip's duration, see the definition). Requires SetEditingEntity; no-op
    // otherwise.
    void SyncDurationFromActiveClip();

    // Auto-scroll the canvas when a drag is near a horizontal edge.
    void TickAutoScrollOnDrag(float canvasX, float canvasWidth);

    // Clamp m_scrollX to [0, contentEnd] for the given lane view width so
    // zooming / panning can't push all the tracks off-screen.
    void ClampScroll(float viewWidth);

    // ---- undo / redo ----
    void PushCommand(std::unique_ptr<INotifyCommand> cmd, bool runApply = true);

    bool m_wasFocused = false;   // see ConsumeFocusedLastRender()

    // ---- target ----
    TimelineComponent* m_target        = nullptr;
    std::function<void()> m_onEdited;
    Entity             m_editingEntity = 0;            // NullEntity
    World*             m_world         = nullptr;

    // ---- view state ----
    float m_pixelsPerSecond = 200.f;
    float m_scrollX         = 0.f;
    float m_previewTime     = 0.f;
    bool  m_previewPlaying  = false;
    float m_previewSpeed    = 1.f;

    // ---- selection / drag ----
    Selection              m_selection;       // primary pick (drives property panel)
    std::vector<Selection> m_multiSel;        // additional picks; never contains m_selection

    DragMode m_dragMode        = DragMode::None;
    float    m_dragStartT0     = 0.f;        // initial time / startTime
    float    m_dragStartT1     = 0.f;        // initial endTime (only for state)
    float    m_dragStartMouseX = 0.f;

    // Box-select rectangle screen coords (only valid while m_dragMode == BoxSelect).
    float m_boxStartX = 0.f, m_boxStartY = 0.f;
    float m_boxEndX   = 0.f, m_boxEndY   = 0.f;

    // ---- clipboard ----
    // Stores deep copies so paste survives the source being deleted /
    // edited. Layout: per-item, the original time is stored relative to
    // the bounding-box left edge so paste re-anchors at the cursor.
    std::vector<ClipboardItem> m_clipboard;

    // ---- context menu state ----
    bool  m_openContextMenu  = false;
    int   m_pendingAddTrack  = -1;
    float m_pendingAddTime   = 0.f;

    // ---- undo / redo history ----
    std::vector<std::unique_ptr<INotifyCommand>> m_undoStack;
    std::vector<std::unique_ptr<INotifyCommand>> m_redoStack;
};

} // namespace Editor
