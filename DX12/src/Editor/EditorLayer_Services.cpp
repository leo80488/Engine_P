// EditorLayer_Services.cpp - EditorLayer's adapters behind Editor::Services (Editor/EditorServices.h):
// the selection, the documents, the active document's undo, notifications (the toasts) and jobs
// (the control channel's job table).

#include "Editor/EditorLayer.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorNotify.h"
#include "Editor/EditorServices.h"
#include "Harness/ControlChannelJobs.h"

#include <nlohmann/json.hpp>

#include <memory>

namespace Editor
{
Services& EditorServices()
{
    static Services s_services;
    return s_services;
}
}

// The adapters read and change EditorLayer's state, so they are its friends' business: one struct
// holding all five, owned by m_serviceAdapters.
struct EditorServiceAdapters
{
    struct SelectionAdapter : Editor::ISelectionService
    {
        EditorLayer* l = nullptr;
        Entity Primary() const override { return l->m_selectedEntity; }
        std::vector<Entity> Selection() const override { return l->m_selectedEntities; }
        void Select(Entity e, bool add) override
        {
            l->SelectEntity(e, add ? EditorLayer::SelectMode::Add : EditorLayer::SelectMode::Replace);
        }
        std::uint64_t Epoch() const override { return l->m_selectionEpoch; }
    };

    struct DocumentAdapter : Editor::IDocumentService
    {
        EditorLayer* l = nullptr;
        std::string ActiveKind() const override { return l->m_documents.Active().kind; }
        std::string ActiveName() const override
        {
            const Editor::Document& d = l->m_documents.Active();
            return d.title ? d.title() : d.kind;
        }
        std::size_t UnsavedCount() override { return l->CollectUnsavedDocuments().size(); }
        void SaveActive() override { Editor::CommandRegistry::Get().Execute("doc.save"); }
    };

    struct UndoAdapter : Editor::IUndoService
    {
        EditorLayer* l = nullptr;
        static bool Enabled(const char* id)
        {
            const Editor::CommandRegistry& reg = Editor::CommandRegistry::Get();
            const Editor::Command* c = reg.Find(id);
            return c && reg.IsEnabled(*c);
        }
        bool CanUndo() const override { return Enabled("doc.undo"); }
        bool CanRedo() const override { return Enabled("doc.redo"); }
        void Undo() override { Editor::CommandRegistry::Get().Execute("doc.undo"); }
        void Redo() override { Editor::CommandRegistry::Get().Execute("doc.redo"); }
    };

    struct NotifyAdapter : Editor::INotifyService
    {
        void Notify(Editor::NoticeLevel level, std::string text, std::function<void()> action) override
        {
            Editor::Notices::Get().Push(level, std::move(text), std::move(action));
        }
    };

    struct JobAdapter : Editor::IJobService
    {
        std::vector<Editor::JobInfo> Jobs() const override
        {
            std::vector<Editor::JobInfo> out;
#if defined(WITH_HARNESS) && WITH_HARNESS
            const nlohmann::json reply = nlohmann::json::parse(Harness::Jobs::ServeVerb("list"), nullptr, false);
            if (!reply.is_object() || !reply.contains("jobs")) return out;
            for (const nlohmann::json& j : reply["jobs"])
            {
                Editor::JobInfo info;
                info.id = j.value("id", 0ull);
                info.kind = j.value("kind", std::string());
                info.detail = j.value("detail", std::string());
                info.state = j.value("state", std::string());
                if (j.contains("progress") && j["progress"].is_object())
                {
                    const double done = j["progress"].value("done", 0.0), total = j["progress"].value("total", 0.0);
                    if (total > 0.0) info.progress = static_cast<float>(done / total);
                }
                if (j.contains("error") && j["error"].is_object())
                    info.error = j["error"].value("code", std::string()) + ": " + j["error"].value("message", std::string());
                out.push_back(std::move(info));
            }
#endif
            return out;
        }
        // The control channel's jobs run to their end: none is cancellable from here.
        bool Cancel(std::uint64_t) override { return false; }
    };

    SelectionAdapter selection;
    DocumentAdapter  documents;
    UndoAdapter      undo;
    NotifyAdapter    notify;
    JobAdapter       jobs;
};

void EditorLayer::InstallServices()
{
    auto adapters = std::make_shared<EditorServiceAdapters>();
    adapters->selection.l = this;
    adapters->documents.l = this;
    adapters->undo.l = this;
    Editor::Services& s = Editor::EditorServices();
    s.selection = &adapters->selection;
    s.documents = &adapters->documents;
    s.undo = &adapters->undo;
    s.notify = &adapters->notify;
    s.jobs = &adapters->jobs;
    m_serviceAdapters = adapters;
}
