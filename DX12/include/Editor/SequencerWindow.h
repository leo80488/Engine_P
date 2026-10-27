#pragma once

// SequencerWindow - the `.iseq` authoring window.
//
// Owns a Seq::Sequence loaded from disk, edits it and writes it back. It does
// not edit the scene: the only thing it touches in the world is a preview
// SequencePlayerComponent, driven through the same transport requests the
// inspector and Lua use, so one code path moves a playhead.
//
// Edits are live: the preview player reads its asset by path through
// Seq::AcquireSequence, so while a preview is up the window publishes its
// document into that cache (Seq::PublishSequenceEdit) and revokes the publish
// when the preview closes - an unsaved document must not outlive the window
// editing it.
//
// Undo is separate from the scene undo stack: EditorLayer::PushEdit calls
// MarkSceneDirty(), so an asset edit routed through it would flag "unsaved
// scene" on every key drag and interleave document and scene history. The
// window keeps its own ring of whole-document snapshots (BuildSequenceText,
// cheap), taken when a drag starts so a multi-step drag undoes as one step.

#include "Sequencer/SequenceAsset.h"
#include "Editor/TimelineCanvas.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class World;

namespace Editor
{

class SequencerWindow
{
public:
    // `selected` is the editor's current selection, used by "Bind to selection".
    void Draw(bool* open, World* world, uint32_t selected);

    // Load a .iseq and show the window. Called from the asset browser's
    // double-click handler.
    void Open(const std::string& path);

    // Create a new sequence at `path` and open it. Writes immediately, so the
    // asset exists on disk (and in the browser) the moment it is made.
    bool NewSequence(const std::string& path);

    // File dialogs live in EditorLayer (they are Win32 and editor-only), so the
    // window asks for a path through these instead of owning them. Both return
    // an empty string on cancel. Unset, the toolbar's New/Open/Save As do nothing;
    // the no-document screen's inline path field still creates a sequence.
    void SetFileDialogs(std::function<std::string()> pickSave,
                        std::function<std::string()> pickOpen)
    { m_pickSavePath = std::move(pickSave); m_pickOpenPath = std::move(pickOpen); }

    const std::string& Path() const { return m_path; }
    bool               Dirty() const { return m_dirty; }
    // Writes the document to Path(); false when there is no path or the write failed.
    bool               Save();
    // The document's own history (the window's Ctrl+Z / Ctrl+Y; the Edit menu reaches it too).
    void               Undo();
    void               Redo();

    // True when the window had focus during the last Draw(): the editor layer's
    // shortcut dispatcher yields Ctrl+Z/Y/S to a focused panel that owns its undo
    // ring and save (this one does both). Consumes the flag so a closed window
    // cannot leave a stale claim on the dispatcher.
    bool ConsumeFocusedLastDraw()
    {
        const bool f = m_wasFocused;
        m_wasFocused = false;
        return f;
    }

    // Tear down the preview player. Called on window close, before another
    // document opens and when Preview turns off - a preview entity that outlives
    // its window is a sequence nobody can stop.
    void ClosePreview(World* world);

    // True while Record is on and a preview is up; OnTransformCommitted turns a
    // gizmo commit into keys only then.
    bool  Recording() const { return m_recordMode && m_loaded && m_previewOn; }
    // Called by EditorLayer::CommitTransformEdit after a gizmo drag ends.
    void  OnTransformCommitted(World* world, uint32_t entity);

private:
    // ---- drag state ----
    enum class Drag { None, Playhead, SectionMove, SectionResizeL, SectionResizeR, Key };

    // ---- display rows ----
    struct Row
    {
        int track = -1, section = -1, channel = -1;   // channel >= 0 -> a curve row
    };

    // ---- live preview publishing ----
    // Sets both dirty flags. Every mutation of m_seq goes through this or
    // PushUndo: a site that sets m_dirty directly is an edit the preview never
    // sees.
    void MarkEdited() { m_dirty = true; m_previewDirty = true; }
    void PublishPreviewDoc();
    void RevokePreviewDoc();

    // ---- undo (whole-document text snapshots; see the file comment) ----
    void PushUndo();          // snapshot the CURRENT document before mutating it

    // ---- drawing and input ----
    void DrawToolbar(World* world);
    void DrawBindingPanel(World* world, uint32_t selected);
    void DrawTimeline(World* world);
    void DrawInspector();
    void DrawTrackHeader(int trackIdx, float rowY);
    void DrawSections(int trackIdx, float rowY);
    // Split in two so the header column and the lane content draw under
    // different clip rects: one clip for both renders the header column blank.
    void DrawChannelHeader(int trackIdx, int sectionIdx, int channelIdx, float rowY);
    void DrawChannelKeys  (int trackIdx, int sectionIdx, int channelIdx, float rowY);
    void HandleTimelineInput(World* world);

    // ---- preview and record mode ----
    void  SyncPreview(World* world);
    // Write (or replace) keys on every Transform channel of `entity`'s track at
    // the playhead. Returns false when the entity is not bound to a Transform
    // track in this document.
    bool  KeyTransformAt(World* world, uint32_t entity, int32_t frame);

    // ---- save and work range ----
    int32_t WorkEnd() const;

    // ---- display rows ----
    void RebuildRows();

    // ---- window state ----
    bool m_wasFocused = false;   // see ConsumeFocusedLastDraw()

    std::function<std::string()> m_pickSavePath;
    std::function<std::string()> m_pickOpenPath;

    // The no-document screen's inline path field (always shown; the only way to
    // create a sequence in a host without file dialogs).
    char m_newPathBuf[260] = "asset/Sequence/NewSequence.iseq";

    // Width of the right-hand inspector column.
    float m_inspectorWidth = 340.f;

    // ---- document ----
    Seq::Sequence m_seq;
    std::string   m_path;
    bool          m_dirty  = false;
    bool          m_loaded = false;

    // ---- live preview publishing ----
    // Every edit sets `m_previewDirty`; while the preview is up the window pushes
    // `m_seq` into the sequence cache. `m_publishedPath` records that an unsaved
    // document sits in that cache, so it can be revoked on close: an in-progress
    // edit must not become what a level's own SequencePlayerComponent plays after
    // the window closes.
    bool        m_previewDirty  = false;
    std::string m_publishedPath;

    // ---- selection ----
    int m_selTrack   = -1;
    int m_selSection = -1;
    int m_selChannel = -1;
    int m_selKey     = -1;

    // ---- view ----
    TimelineCanvas   m_canvas;
    std::vector<int> m_expandedTracks;   // track indices whose channels are shown

    // ---- transport / work range ----
    int32_t  m_frame      = 0;
    int32_t  m_workStart  = 0;
    int32_t  m_workEnd    = 0;      // 0 -> use the full duration
    bool     m_snapToFrame = true;
    bool     m_previewOn   = false;
    // Transport. While playing, the PLAYER owns the clock and the window
    // mirrors it; while stopped the window owns it and seeks. See SyncPreview.
    bool     m_playing     = false;
    float    m_playRate    = 1.f;
    bool     m_cameraLock  = false;   // park the editor free-cam so the shot camera owns the viewport
    bool     m_recordMode  = false;   // gizmo edits become keys at the playhead
    uint32_t m_previewEntity = 0;   // NullEntity when absent

    // ---- drag state ----
    Drag    m_drag      = Drag::None;
    int     m_dragTrack = -1, m_dragSection = -1, m_dragChannel = -1, m_dragKey = -1;
    int32_t m_dragGrabFrame = 0;    // frame under the cursor when the drag began
    int32_t m_dragOrigStart = 0, m_dragOrigEnd = 0;

    // ---- undo (whole-document text snapshots; see the file comment) ----
    std::vector<std::string> m_undo;
    std::vector<std::string> m_redo;
    bool                     m_pendingSnapshot = false;

    // ---- display rows ----
    // Total row count with expansions applied, and the (track, section, channel)
    // a display row maps to. Rebuilt every frame: cheap, and no stale index
    // survives adding or removing a track.
    std::vector<Row> m_rows;
};

} // namespace Editor
