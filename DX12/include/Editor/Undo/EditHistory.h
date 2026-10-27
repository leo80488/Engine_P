#pragma once

// EditHistory - the editor's undo/redo stack.
//
// Header-only (like Reflect.h / ECS.h) so it needs no vcxproj registration and
// can be a by-value member of EditorLayer. Holds two stacks of IEditActions:
// the undo stack (past edits, newest on top) and the redo stack (edits that
// were undone, ready to replay). Any new Push clears redo: the classic linear
// history model.
//
// Save point: every committed entry carries a serial, and the document state
// is named by the serial on top of the undo stack (StateSerial). MarkSaved
// remembers it; AtSavePoint compares, so undoing back to the saved state reads
// clean while a different branch of the same length does not.

#include "Editor/Undo/EditAction.h"
#include "System/Log.h"   // Logger::CurrentSource: whose edit this is

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Editor::Undo
{
// An entry made while the control channel serves a request: an AI agent
// driving this editor (Tools > AI Control Channel). ByAgent() says so - the
// Edit menu shows "Undo AI: Edit Collider", Editor.State() answers
// undoByAgent - while Label() stays the edit's own name, which scripts and
// tests compare. It merges only with another agent entry
// (EditHistory::PushMergingTop), never into the person's.
class AgentEditAction : public IEditAction
{
public:
    explicit AgentEditAction(std::unique_ptr<IEditAction> inner) : m_inner(std::move(inner)) {}

    void Apply(EditContext& c) override               { m_inner->Apply(c); }
    void Revert(EditContext& c) override              { m_inner->Revert(c); }
    void OnDiscard(EditContext& c) override           { m_inner->OnDiscard(c); }
    bool TryMerge(const IEditAction& incoming) override { return m_inner->TryMerge(incoming); }
    size_t HeapBytes() const override                 { return m_inner->HeapBytes(); }
    const char* Label() const override                { return m_inner->Label(); }
    bool ByAgent() const override                     { return true; }

private:
    std::unique_ptr<IEditAction> m_inner;
};

// EditMacro - groups several actions into ONE undo entry so a single user
// gesture that touches many entities (multi-root spawn, multi-select op) is a
// single Ctrl+Z. Applied in order, reverted in reverse.
class EditMacro : public IEditAction
{
public:
    explicit EditMacro(std::string label) : m_label(std::move(label)) {}

    void Add(std::unique_ptr<IEditAction> a) { if (a) m_actions.push_back(std::move(a)); }
    bool Empty() const { return m_actions.empty(); }
    size_t Count() const { return m_actions.size(); }
    // Hand one action back out: lets EditHistory::EndMacro unwrap a
    // single-action macro instead of burying it in a pointless wrapper.
    std::unique_ptr<IEditAction> Release(size_t i)
    { return (i < m_actions.size()) ? std::move(m_actions[i]) : nullptr; }

    void Apply(EditContext& c) override
    {
        for (auto& a : m_actions) a->Apply(c);
    }
    void Revert(EditContext& c) override
    {
        for (auto it = m_actions.rbegin(); it != m_actions.rend(); ++it) (*it)->Revert(c);
    }
    void OnDiscard(EditContext& c) override
    {
        for (auto& a : m_actions) a->OnDiscard(c);
    }
    const char* Label() const override { return m_label.c_str(); }

private:
    std::vector<std::unique_ptr<IEditAction>> m_actions;
    std::string                               m_label;
};

class EditHistory
{
public:
    // The context Apply/Revert receive. Rebound by EditorLayer whenever the
    // World changes (SetWorld) so actions always target the live world.
    void         SetContext(const EditContext& ctx) { m_ctx = ctx; }
    EditContext& Context()                          { return m_ctx; }

    // Record an already-performed edit. The call site did the mutation (e.g.
    // the gizmo already moved the entity); we only capture how to reverse it.
    // Clears the redo stack and trims the oldest entries past the budget.
    //
    // Entries are NOT auto-merged here. Coalescing a CONTINUOUS gesture (a whole
    // gizmo drag / slider scrub) into one action is the call site's job (it
    // captures one before/after per gesture, see EditorLayer's transform
    // bracketing), so by the time we get here each Push is already one complete,
    // independent gesture. Auto-merging by target would wrongly collapse several
    // separate drags on the same entity into a single undo step. For the rare
    // case where a caller explicitly wants to extend the previous entry, use
    // PushMergingTop().
    void Push(std::unique_ptr<IEditAction> action)
    {
        if (!action || !m_recording) return;
        // Inside an open macro every Push collects instead of committing, so an
        // operation built out of several existing single-entity helpers still
        // costs exactly one Ctrl+Z. See BeginMacro.
        if (m_macro) { m_macro->Add(std::move(action)); return; }
        DiscardAll(m_redo);   // redo branch is now unreachable
        m_redo.clear();
        m_redoSerials.clear();
        // Tagged here, at commit, never inside an open macro: a scripted
        // Editor.Batch from the control channel becomes ONE agent entry.
        m_undo.push_back(TagOrigin(std::move(action)));
        m_undoSerials.push_back(m_nextSerial++);
        Trim();
    }

    // ---- Grouping ----
    //
    // Bracket a batch operation that is implemented by calling existing
    // per-entity helpers (duplicate each of N entities, delete each of N
    // subtrees). Without this each helper would push its own entry and undoing
    // one user gesture would take N presses.
    //
    // Nesting is COUNTED: an inner Begin/End pair (DeleteEntities inside a
    // scripted Editor.Batch) joins the open macro, and only the outermost
    // EndMacro commits it, so the whole batch stays one entry. Callers must pair
    // Begin with End (every one does); EndMacro stays safe to call unmatched.
    void BeginMacro(std::string label)
    {
        if (!m_recording) return;
        if (m_macro) { ++m_macroDepth; return; }  // nested: joins the open one
        m_macro = std::make_unique<EditMacro>(std::move(label));
        m_macroDepth = 1;
    }

    void EndMacro()
    {
        if (!m_macro) return;
        if (m_macroDepth > 1) { --m_macroDepth; return; }
        m_macroDepth = 0;
        std::unique_ptr<EditMacro> macro = std::move(m_macro);
        m_macro.reset();
        if (macro->Empty()) return;               // nothing happened; no entry
        if (macro->Count() == 1)
        {
            // One action does not need the wrapper; unwrapping keeps TryMerge and the
            // entry's own label working.
            Push(macro->Release(0));
            return;
        }
        Push(std::move(macro));
    }

    bool MacroOpen() const { return m_macro != nullptr; }

    // Undo everything the open macro collected - newest first - and drop it,
    // whatever the nesting depth: the batch leaves nothing behind (no entry, no
    // redo loss). What a failed Editor.Batch calls. Returns how many actions
    // were reverted.
    size_t AbortMacro()
    {
        if (!m_macro) return 0;
        std::unique_ptr<EditMacro> macro = std::move(m_macro);
        m_macroDepth = 0;
        const size_t n = macro->Count();
        macro->Revert(m_ctx);
        macro->OnDiscard(m_ctx);   // a reverted create frees its parked ids
        return n;
    }

    // Like Push, but first offers the incoming action to the top entry's
    // TryMerge. Returns true if it merged into the existing entry (no new entry
    // added). Callers use this only when they KNOW the edit continues the
    // previous one (e.g. a coalescing token still open).
    bool PushMergingTop(std::unique_ptr<IEditAction> action)
    {
        if (!action || !m_recording) return false;
        // Merge only within one origin: an agent continuing its own drag still
        // coalesces, but its edit never hides inside the person's entry below,
        // nor theirs inside an agent's.
        if (!m_undo.empty() && m_undo.back()->ByAgent() == FromAgent() &&
            m_undo.back()->TryMerge(*action))
        {
            DiscardAll(m_redo);
            m_redo.clear();
            m_redoSerials.clear();
            // The top entry now ends in a different state: it is a new state.
            m_undoSerials.back() = m_nextSerial++;
            return true;
        }
        DiscardAll(m_redo);
        m_redo.clear();
        m_redoSerials.clear();
        m_undo.push_back(TagOrigin(std::move(action)));
        m_undoSerials.push_back(m_nextSerial++);
        Trim();
        return false;
    }

    // Perform AND record an edit in one step (Apply then Push). Use for actions
    // that own their mutation (structural ops).
    void Execute(std::unique_ptr<IEditAction> action)
    {
        if (!action) return;
        action->Apply(m_ctx);
        Push(std::move(action));
    }

    bool        CanUndo()   const { return !m_undo.empty(); }
    bool        CanRedo()   const { return !m_redo.empty(); }
    const char* UndoLabel() const { return m_undo.empty() ? "" : m_undo.back()->Label(); }
    const char* RedoLabel() const { return m_redo.empty() ? "" : m_redo.back()->Label(); }
    // The next undo / redo is an AI agent's edit (AgentEditAction).
    bool        UndoByAgent() const { return !m_undo.empty() && m_undo.back()->ByAgent(); }
    bool        RedoByAgent() const { return !m_redo.empty() && m_redo.back()->ByAgent(); }

    void Undo()
    {
        if (m_undo.empty()) return;
        std::unique_ptr<IEditAction> a = std::move(m_undo.back());
        m_undo.pop_back();
        a->Revert(m_ctx);
        m_redo.push_back(std::move(a));
        m_redoSerials.push_back(m_undoSerials.back());
        m_undoSerials.pop_back();
    }

    void Redo()
    {
        if (m_redo.empty()) return;
        std::unique_ptr<IEditAction> a = std::move(m_redo.back());
        m_redo.pop_back();
        a->Apply(m_ctx);
        m_undo.push_back(std::move(a));
        m_undoSerials.push_back(m_redoSerials.back());
        m_redoSerials.pop_back();
    }

    // Wipe both stacks: call when entity IDs become invalid under the current
    // actions (scene load / world replacement).
    // An open macro is dropped too: its entity ids belong to the world being
    // replaced, so committing it after the swap would target the wrong scene.
    // The state left behind is a new identity. It is still the saved one when the
    // history was at its save point (a world rebound to the same content); otherwise
    // nothing matches it until the caller marks it saved (a load).
    void Clear()
    {
        const bool atSavePoint = AtSavePoint();
        m_macro.reset();
        m_macroDepth = 0;
        DiscardAll(m_undo); DiscardAll(m_redo); m_undo.clear(); m_redo.clear();
        m_undoSerials.clear();
        m_redoSerials.clear();
        m_baseSerial = m_nextSerial++;
        if (atSavePoint) m_savedSerial = m_baseSerial;
    }

    // ---- Save point ----
    // The identity of the current document state: the serial of the newest undo
    // entry, or of the state under the whole stack (0 at birth; a trimmed or
    // cleared history names its new base).
    uint64_t StateSerial() const { return m_undoSerials.empty() ? m_baseSerial : m_undoSerials.back(); }
    // The current state is what is on disk now (after a successful save or a load).
    void     MarkSaved()          { m_savedSerial = StateSerial(); m_savedValid = true; }
    // A change the history cannot reverse happened (an import, an edit made
    // outside the undo stack): no undo or redo reaches the saved state again.
    void     MarkUnrecoverable()  { m_savedValid = false; }
    bool     AtSavePoint() const  { return m_savedValid && StateSerial() == m_savedSerial; }

    // Recording is gated off during Play so runtime mutations don't pollute the
    // authoring history (EditorLayer toggles this on the Edit/Play edge).
    void SetRecording(bool on) { m_recording = on; }
    bool IsRecording() const   { return m_recording; }

    size_t UndoCount() const { return m_undo.size(); }
    size_t RedoCount() const { return m_redo.size(); }

private:
    static constexpr size_t kMaxEntries = 256;

    // Edits made while the control channel serves a request (its log context
    // is LogSource::Control) are an agent's - see AgentEditAction.
    static bool FromAgent() { return Logger::CurrentSource() == LogSource::Control; }
    static std::unique_ptr<IEditAction> TagOrigin(std::unique_ptr<IEditAction> action)
    {
        if (action && FromAgent())
            return std::make_unique<AgentEditAction>(std::move(action));
        return action;
    }

    // Cap the number of retained undo entries; drop from the bottom (oldest).
    // Discarded actions get OnDiscard so they can release held resources (e.g.
    // a delete action un-parking its reserved entity ids).
    void Trim()
    {
        while (m_undo.size() > kMaxEntries)
        {
            if (m_undo.front()) m_undo.front()->OnDiscard(m_ctx);
            m_undo.erase(m_undo.begin());
            // The dropped edit stays applied: the state under the stack is now its state.
            m_baseSerial = m_undoSerials.front();
            m_undoSerials.erase(m_undoSerials.begin());
        }
    }

    void DiscardAll(std::vector<std::unique_ptr<IEditAction>>& stack)
    {
        for (auto& a : stack)
            if (a) a->OnDiscard(m_ctx);
    }

    EditContext                               m_ctx;
    bool                                      m_recording = true;
    // Non-null while a batch operation is being collected (BeginMacro).
    std::unique_ptr<EditMacro>                m_macro;
    int                                       m_macroDepth = 0;   // Begin/End nesting
    std::vector<std::unique_ptr<IEditAction>> m_undo;
    std::vector<std::unique_ptr<IEditAction>> m_redo;

    // ---- Save point ----
    // Parallel to m_undo / m_redo: the state each entry ends in.
    std::vector<uint64_t> m_undoSerials;
    std::vector<uint64_t> m_redoSerials;
    uint64_t              m_nextSerial  = 1;
    uint64_t              m_baseSerial  = 0;
    uint64_t              m_savedSerial = 0;
    bool                  m_savedValid  = true;   // a new history is an unsaved-but-empty document: clean
};

} // namespace Editor::Undo
