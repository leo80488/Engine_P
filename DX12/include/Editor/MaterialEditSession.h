#pragma once

// MaterialEditSession - one material FILE open for editing: its staged MaterialComponent
// and MaterialLoadInfo (the parent, the override / unset keys), its dirty state, a 64-step
// undo ring, and the file operations (Save, Save As, Save as Instance, Revert). No ImGui:
// the Material Editor window draws it (with Editor::MaterialPropertyPanel) and Lua edits it.
//
// EVERY EDIT GOES THROUGH MarkEdited(key). In an INSTANCE the key joins the override set
// (UE's rule): an .imatinst writes only the keys in that set, so an unmarked edit would be
// dropped by Save. The panel reaches it through MaterialPanelContext::onEdit and Set calls
// it per key, so a pack that edits by script proves the controls' path.
// -mateditor_autooverride_off is the reverse arm.
//
// UNDO. A gesture (a drag, a click, a scripted Set) is one step: the state before its
// first edit goes onto the ring and the redo side is cleared. The window brackets the panel
// with BeginFrame / EndFrame; an edit is detected by the material's own ASSET_DIRTY bit
// (every panel edit calls SetEditDirty), which the session owns since the staged material
// belongs to no entity. A copy is taken only at gesture boundaries, never per frame.
//
// Editor-only TU: compiled into EditorCore.

#include "ECS/Components.h"                 // MaterialComponent
#include "Resource/MaterialSerializer.h"    // Resource::MaterialLoadInfo

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Editor
{
    // What a session edits. Graph is a standalone material whose shader its graph generates;
    // Entity is one entity's own material (see OpenEntity); Function is a Material Function's
    // graph (see OpenFunction).
    enum class MaterialKind : std::uint8_t { Standard, CustomHLSL, Graph, Instance, Entity, Function };
    const char* MaterialKindName(MaterialKind k);   // "standard" | "custom" | "graph" | "instance" | "entity" | "function"

    class MaterialEditSession
    {
    public:
        static constexpr std::size_t kUndoDepth = 64;

        struct EntityEdges
        {
            bool began = false, edited = false, ended = false;
            bool Any() const { return began || edited || ended; }
        };

        MaterialEditSession();
        ~MaterialEditSession();

        // Loads @p path (an .imat or .imatinst; relativized). False with @p why when it does not
        // load; the session is then unchanged.
        bool Open(const std::string& path, std::string& why);

        const std::string&          Path() const { return m_path; }
        MaterialKind                Kind() const;
        MaterialComponent&          Mat()        { return *m_mat; }
        const MaterialComponent&    Mat() const  { return *m_mat; }
        Resource::MaterialLoadInfo& Info()       { return m_info; }
        const Resource::MaterialLoadInfo& Info() const { return m_info; }
        bool                        IsInstance() const { return !m_info.parentPath.empty(); }
        bool                        Dirty() const { return m_dirty; }

        // ---- Editing ----
        // One edit of @p key (the .imat vocabulary: "blendMode", "roughnessMin", "tex_NORMALMAP",
        // "cparam_Tint", ...): an instance overrides it; the material is marked edited. The panel's
        // onEdit and Set both call it.
        void MarkEdited(const std::string& key);
        // Ticks / unticks an instance's override of @p key (false when not an instance).
        // Unticking puts the parent's value back. One undo step.
        bool SetOverride(const std::string& key, bool on, std::string& why);
        // Applies "key = value" lines with the loader's own parser
        // (Resource::ApplyMaterialTextDelta), each key through MarkEdited. One undo step. False,
        // with nothing applied, on an empty or malformed list.
        bool Set(const std::vector<std::pair<std::string, std::string>>& kv, std::string& why);
        // Drops every override: the parent's values, an empty set. One undo step.
        bool ResetOverrides(std::string& why);
        // @p key's value as the file format spells it (BuildMaterialText's line); "" when the key
        // is not in the format.
        std::string ValueOf(const std::string& key) const;

        // The panel bracket the window draws around the panel: BeginFrame before it, EndFrame
        // after it with ImGui::IsAnyItemActive().
        void BeginFrame();
        void EndFrame(bool anyItemActive);
        // A control is being dragged / typed into (an edit gesture is open).
        bool InGesture() const { return m_gesture; }
        // Changes whenever the staged material changes (an edit, undo / redo, a revert or a
        // reload); the window's live push compares it.
        std::uint64_t Revision() const { return m_revision; }

        // ---- Entity tab ----
        // The staged material mirrors ONE entity's MaterialComponent, not a file: its parent is
        // the entity's material asset (@p source; "" = an inline material), its override set the
        // entity's MaterialOverride keys, so the panel ticks what this entity overrides and
        // unticking puts the asset's value back. The window writes every edit into the entity at
        // once (MaterialOverride re-derived against the file), so the tab is never dirty and has
        // no file to save; each gesture is ONE step of the SCENE's undo history, bracketed by the
        // edges below, never of this session's ring.
        void OpenEntity(const std::string& label, const std::string& source, const MaterialComponent& mat,
                        const std::vector<std::string>& overrideKeys);
        bool IsEntity() const { return m_entity; }
        // The entity changed under the tab (the scene's undo, a reload, a script): mirror it.
        // Ignored while a gesture is open.
        void SyncFromEntity(const std::string& source, const MaterialComponent& mat,
                            const std::vector<std::string>& overrideKeys);

        // What the window owes the entity since the last call: a gesture BEGAN (capture the state
        // before it), the material was EDITED (write it), the gesture ENDED (one undo step). A
        // scripted Set / override / reset is a whole gesture at once.
        EntityEdges TakeEntityEdges();

        // ---- Function tab ----
        // A Material Function (.imatfunc at @p path) has no material: the staged one is a
        // transient unlit material the preview draws the function's first output with. Never
        // dirty; Save / Revert belong to the window (the graph) and do nothing here; Save As and
        // Save as Instance are refused.
        void OpenFunction(const std::string& path);
        bool IsFunction() const { return m_function; }

        // ---- Undo ----
        bool        Undo();
        bool        Redo();
        std::size_t UndoDepth() const { return m_undo.size(); }
        std::size_t RedoDepth() const { return m_redo.size(); }

        // ---- Files ----
        // Writes to Path(): SaveMaterial, or SaveMaterialInstance with the explicit set.
        bool Save(std::string& why);
        // Saves the same kind at @p path; the session moves there (EnsureGuid gives the path a
        // guid).
        bool SaveAs(const std::string& path, std::string& why);
        // Writes a new .imatinst at @p path whose parent is THIS file as it is on disk; its
        // overrides are the keys whose values the staged material changes (both full dumps
        // compared line by line - one writer, one normalization). This session is untouched.
        // @p keys (optional) receives those keys, sorted.
        bool SaveAsInstance(const std::string& path, std::string& why,
                            std::vector<std::string>* keys = nullptr);
        // Re-reads the file (one undo step).
        bool Revert(std::string& why);
        // The file changed on disk since it was read or saved here.
        bool ChangedOnDisk() const;

    private:
        struct State
        {
            std::unique_ptr<MaterialComponent> mat;
            Resource::MaterialLoadInfo         info;
            bool                               dirty = false;
        };

        State Capture() const;
        void  Restore(State& s);
        void  PushUndo(State s);
        void  Rebase();                       // the gesture base = the current state
        void  NoteDiskStamp();
        bool  Load(const std::string& path, std::string& why);   // into the live state

        std::string                        m_path;
        std::unique_ptr<MaterialComponent> m_mat;
        Resource::MaterialLoadInfo         m_info;
        bool                               m_dirty = false;
        std::deque<State>                  m_undo, m_redo;
        State                              m_base;               // the state before the next gesture
        bool                               m_gesture = false;    // an edit is in progress
        std::uint64_t                      m_revision = 0;       // see Revision()
        bool                               m_entity = false;     // an entity tab
        bool                               m_function = false;   // a Material Function tab
        EntityEdges                        m_edges;              // owed to the entity
        std::uint64_t                      m_diskSize = 0;
        std::int64_t                       m_diskTime = 0;
    };
}
