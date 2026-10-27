#include "Editor/EditorDocuments.h"

#include <utility>

namespace Editor
{
    void DocumentManager::Add(Document d)
    {
        m_documents.push_back(std::move(d));
    }

    void DocumentManager::UpdateActive(bool popupOpen)
    {
        if (popupOpen) return;
        m_active = -1;
        for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
            if (m_documents[i].hasFocus && m_documents[i].hasFocus())
            {
                m_active = i;
                return;
            }
    }

    Document& DocumentManager::Active()
    {
        if (m_active >= 0 && m_active < static_cast<int>(m_documents.size())) return m_documents[m_active];
        return m_fallback;
    }

    const Document* DocumentManager::Find(const std::string& kind) const
    {
        if (m_fallback.kind == kind) return &m_fallback;
        for (const Document& d : m_documents)
            if (d.kind == kind) return &d;
        return nullptr;
    }

    std::string DocumentManager::SaveLabel()
    {
        const Document& d = Active();
        return d.title ? "Save " + d.title() : std::string("Save");
    }

    std::string DocumentManager::UndoLabel()
    {
        const Document& d = Active();
        const std::string what = d.undoLabel ? d.undoLabel() : std::string();
        return what.empty() ? std::string("Undo") : "Undo " + what;
    }

    std::string DocumentManager::RedoLabel()
    {
        const Document& d = Active();
        const std::string what = d.redoLabel ? d.redoLabel() : std::string();
        return what.empty() ? std::string("Redo") : "Redo " + what;
    }
}
