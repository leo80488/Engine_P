#pragma once

// EditorDocuments - the document the editor's Save / Undo / Redo commands act on. Each editor window
// keeps its own document model, history and save path; a Document is only how the shell reaches
// one: which window had keyboard focus, and what Save / Undo / Redo mean there. The scene is the
// fallback when no other document has focus.

#include <functional>
#include <string>
#include <vector>

namespace Editor
{
    struct Document
    {
        std::string kind;   // stable: "scene", "material", "ui", ... - tests and scripts name it
        std::function<std::string()> title;      // what the menus call it ("Material 'm4.imat'")
        // Last frame's keyboard focus was in this document's window (read before the shortcuts).
        std::function<bool()> hasFocus;
        // Each optional: absent = the document cannot do it.
        std::function<bool(std::string& why)> canSave;
        std::function<void()> save;              // queues or runs; the window reports the result
        std::function<void()> saveAs;
        std::function<bool()> canUndo;
        std::function<bool()> canRedo;
        std::function<std::string()> undoLabel;  // "Move" (the entry's own name), or ""
        std::function<std::string()> redoLabel;
        std::function<void()> undo;
        std::function<void()> redo;
        // Its own clipboard and duplicate; absent = the scene's (the selected entities).
        std::function<void()> copy;
        std::function<void()> paste;
        std::function<void()> duplicate;
        // A focused panel that reads some chords itself inside its Draw (the Sequencer's Ctrl+S,
        // the notify tracks' Ctrl+Z): true for a command id whose chord the shortcut dispatcher
        // must leave to it. The menus and the palette still reach the document through the
        // functions above.
        std::function<bool(const std::string& commandId)> handlesKey;
    };

    class DocumentManager
    {
    public:
        // Adapters in priority order (the first one with focus wins); the fallback answers when
        // none has focus.
        void Add(Document d);
        void SetFallback(Document d) { m_fallback = std::move(d); }
        bool Empty() const { return m_documents.empty() && m_fallback.kind.empty(); }

        // Once per frame, before the shortcuts. While a popup or a menu is open (@p popupOpen) the
        // previous answer stands: opening the Edit menu or the palette must not make Undo the
        // scene's.
        void UpdateActive(bool popupOpen);
        // The document the commands act on (the fallback when nothing else had focus).
        Document& Active();
        const Document* Find(const std::string& kind) const;
        const std::vector<Document>& All() const { return m_documents; }

        // Convenience over Active(), with the document's own name in the label.
        std::string SaveLabel();
        std::string UndoLabel();
        std::string RedoLabel();

    private:
        std::vector<Document> m_documents;
        Document              m_fallback;
        int                   m_active = -1;   // index into m_documents; -1 = the fallback
    };
}
