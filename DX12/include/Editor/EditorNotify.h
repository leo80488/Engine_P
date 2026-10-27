#pragma once

// EditorNotify - the editor's toasts: short notices stacked at the main viewport's bottom right that
// fade after a few seconds, each also written to the log. Clicking one runs its action (locate the
// result, open the Output Log). Editor::EditorServices().notify is the way in; the Jobs panel posts one
// when a job finishes. Under -unattended nothing is drawn (packs photograph the UI) but the notices
// are kept. UI thread.

#include "Editor/EditorServices.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace Editor
{
    struct Notice
    {
        std::uint64_t         id = 0;
        NoticeLevel           level = NoticeLevel::Info;
        std::string           text;
        std::function<void()> action;
        double                postedAt = 0.0;   // ImGui time, seconds
        bool                  dismissed = false;
    };

    class Notices
    {
    public:
        static Notices& Get();

        void Push(NoticeLevel level, std::string text, std::function<void()> action = {});
        // The toasts, once a frame after the panels: the newest at the bottom, each for kShowSeconds.
        void Draw();
        // The last kKept notices, oldest first (the Jobs panel's history, tests).
        const std::deque<Notice>& Recent() const { return m_recent; }
        void Clear() { m_recent.clear(); }

        static constexpr double      kShowSeconds = 4.0;
        static constexpr std::size_t kKept = 32;

    private:
        std::deque<Notice> m_recent;
        std::uint64_t      m_nextId = 1;
    };
}
