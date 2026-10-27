#pragma once

// EditorServices - the narrow interfaces a panel or an editor module asks the editor through, instead
// of EditorLayer& or a singleton: the selection, the documents, the undo history, notifications and
// jobs. EditorLayer implements them (EditorLayer_Services.cpp) and installs them at startup; a host
// with no editor leaves them null. UI thread.

#include "ECS/ECS.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Editor
{
    class ISelectionService
    {
    public:
        virtual ~ISelectionService() = default;
        // The entity the Inspector shows (the last one selected); NullEntity for none.
        virtual Entity              Primary() const = 0;
        virtual std::vector<Entity> Selection() const = 0;
        virtual void                Select(Entity e, bool add) = 0;
        // Bumped whenever the selection changes: a panel compares it instead of the list.
        virtual std::uint64_t       Epoch() const = 0;
    };

    class IDocumentService
    {
    public:
        virtual ~IDocumentService() = default;
        // The active document's kind ("scene", "material", ...) and name.
        virtual std::string ActiveKind() const = 0;
        virtual std::string ActiveName() const = 0;
        // How many documents (every panel's files and the scene) have unsaved edits.
        virtual std::size_t UnsavedCount() = 0;
        virtual void        SaveActive() = 0;
    };

    class IUndoService
    {
    public:
        virtual ~IUndoService() = default;
        // The active document's history.
        virtual bool CanUndo() const = 0;
        virtual bool CanRedo() const = 0;
        virtual void Undo() = 0;
        virtual void Redo() = 0;
    };

    enum class NoticeLevel : std::uint8_t { Info, Success, Warning, Error };

    class INotifyService
    {
    public:
        virtual ~INotifyService() = default;
        // A toast (and an Output Log line); @p action, when set, is what clicking it does (locate the
        // result, open the log).
        virtual void Notify(NoticeLevel level, std::string text, std::function<void()> action = {}) = 0;
    };

    struct JobInfo
    {
        std::uint64_t id = 0;
        std::string   kind;     // "import", "ui_tests", "bake", ...
        std::string   detail;
        std::string   state;    // "queued", "running", "succeeded", "failed", "cancelled"
        float         progress = -1.f;   // 0..1, < 0 = unknown
        std::string   error;
        bool          cancellable = false;
    };

    class IJobService
    {
    public:
        virtual ~IJobService() = default;
        // Every job the editor tracks (the control channel's jobs and the editor's own work).
        virtual std::vector<JobInfo> Jobs() const = 0;
        virtual bool                 Cancel(std::uint64_t id) = 0;
    };

    struct Services
    {
        ISelectionService* selection = nullptr;
        IDocumentService*  documents = nullptr;
        IUndoService*      undo      = nullptr;
        INotifyService*    notify    = nullptr;
        IJobService*       jobs      = nullptr;
    };

    // The editor's services (all null until EditorLayer installs them).
    Services& EditorServices();
}
