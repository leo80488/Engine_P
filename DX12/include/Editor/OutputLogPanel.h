#pragma once
// OutputLogPanel - the model and list view behind the editor's Output Log.
//
// The panel keeps its OWN mirror of the Logger's record ring, filled incrementally
// (Logger::CopyRecordsSince: only what is newer than the last seq it saw), and a
// filtered index the list draws through ImGuiListClipper - so a visible panel costs the
// few lines logged this frame plus the rows on screen, not a deep copy of the buffer
// under the lock every LOG_ call takes.
//
// The level toggles and auto-scroll live in EditorLayer (persisted in editor_prefs.ini,
// shown in Editor Preferences), so DrawBody takes them by reference; the source / tag
// filters are session state and live here.
//
// The mirror can LAG the ring but never silently lose a line: when the ring dropped
// lines the panel never pulled, a synthetic row (seq 0) says how many; the log FILE
// still has them.

#include "System/Log.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct ImGuiInputTextCallbackData;

namespace Editor
{
    class OutputLogPanel
    {
    public:
        static constexpr std::size_t kDefaultCapacity = 20000;

        // The persisted filters EditorLayer owns.
        struct LevelFilters
        {
            bool* showSuccess;
            bool* showInfo;
            bool* showWarning;
            bool* showError;
            bool* autoScroll;
            char* text;          // substring filter buffer
            std::size_t textSize;
        };

        // The command line: what the Enter key runs. EditorLayer fills it every frame;
        // DrawBody without it draws no command line.
        struct ConsoleHooks
        {
            bool available = false;   // there is a Lua VM and a world to run in
            bool playing   = false;   // Play mode: whatever it creates is discarded on Stop
            std::function<void(const std::string&)> submit;   // queues it for the frame boundary
        };

        // ---- harness (-logpanel_verify) ----
        struct VerifyResult
        {
            std::size_t   mirror = 0, ring = 0, overlap = 0, matched = 0;
            std::size_t   missing = 0, mismatches = 0, dups = 0, filtered = 0, tags = 0;
            std::size_t   bySource[static_cast<std::size_t>(LogSource::Count)] = {};
            std::uint64_t gaps = 0, gapLines = 0;
        };

        // Pull every record newer than the last one seen. Call once per frame
        // while the panel is open - drawn or not - so the mirror has no gaps.
        void Pump();

        // Toolbar + list (+ command line). Must run between ImGui::Begin and End.
        void DrawBody(const LevelFilters& filters, bool fontAwesome,
                      const ConsoleHooks* console = nullptr);

        // Bring the panel to the front and put the caret in the command line
        // (Tools > Lua Script Tools, the Command Palette).
        void FocusCommandLine() { m_focusInput = true; m_focusWindow = true; }
        // EditorLayer asks this before Begin: true once per FocusCommandLine.
        bool TakeWindowFocusRequest() { const bool f = m_focusWindow; m_focusWindow = false; return f; }

        // "Clear": the mirror, the filtered index, and the tab badge
        // (Logger::ResetBadgeTallies). The ring and the log FILE are untouched.
        void Clear();

        // Pump, then compare the mirror against a fresh copy of the Logger's
        // ring over the seqs both hold: every ring line must be in the mirror
        // exactly once, with the same text, level, source and frame.
        VerifyResult Verify();

        // Reverse arm of the check above (-logpanel_verify_break): Verify drops
        // the newest mirrored line before comparing - the lost-line bug the
        // mirror exists to exclude - and must then report missing >= 1.
        void SetBreakVerifyForReverseArm(bool on) { m_breakVerify = on; }

    private:
        struct FilterKey
        {
            bool        level[4]  = { true, true, true, true };   // by LogLevel
            bool        source[static_cast<std::size_t>(LogSource::Count)] = { true, true, true, true };
            std::string text;
            std::string tag;
            bool operator==(const FilterKey& o) const;
        };

        void DrawCommandLine(const ConsoleHooks& console);
        static int HistoryCallback(ImGuiInputTextCallbackData* data);
        void Append(LogRecord&& r);
        void AppendGapRow(std::uint64_t lines);
        void TrimToCapacity();
        bool Passes(const LogRecord& r) const;
        void RebuildFilter();

        std::deque<LogRecord>      m_mirror;
        std::uint64_t              m_firstAbs = 0;   // absolute index of m_mirror.front()
        std::uint64_t              m_nextAbs  = 0;   // absolute index of the next append
        std::vector<std::uint64_t> m_filtered;       // absolute indices, ascending
        std::uint64_t              m_lastSeq  = 0;   // newest ring seq pulled
        std::uint64_t              m_gaps = 0, m_gapLines = 0;
        std::vector<LogRecord>     m_scratch;

        FilterKey                  m_active;         // what m_filtered was built for
        bool                       m_filterDirty = true;
        bool                       m_showSource[static_cast<std::size_t>(LogSource::Count)] = { true, true, true, true };
        std::string                m_tag;            // "" = every tag
        std::unordered_map<std::string, std::uint32_t> m_tagCounts;   // seen this session

        std::size_t                m_capacity    = kDefaultCapacity;
        bool                       m_breakVerify = false;

        // Command line state. The history is the session's (never persisted:
        // editor_prefs.ini is read-only under the harness, and a command is
        // not a preference).
        char                       m_cmd[4096]   = {};
        bool                       m_multiline   = false;
        bool                       m_focusInput  = false;
        bool                       m_focusWindow = false;
        bool                       m_scrollToEnd = false;
        std::vector<std::string>   m_history;
        int                        m_historyPos  = -1;   // -1 = editing a new line
    };
}
