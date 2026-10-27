// OutputLogPanel - see include/Editor/OutputLogPanel.h.
#include "Editor/OutputLogPanel.h"
#include "Editor/FontAwesomeIcons.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace Editor
{
namespace
{
    constexpr std::size_t kSources = static_cast<std::size_t>(LogSource::Count);

    ImVec4 LevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Success: return ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        case LogLevel::Info:    return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        case LogLevel::Warning: return ImVec4(1.0f, 0.9f, 0.2f, 1.0f);
        case LogLevel::Error:   return ImVec4(1.0f, 0.3f, 0.2f, 1.0f);
        default:                return ImVec4(0.8f, 0.8f, 0.8f, 1.0f);
        }
    }

    // Sources are told apart by a short badge, not by colour: colour already
    // means severity in this panel.
    const char* SourceBadge(LogSource s)
    {
        switch (s)
        {
        case LogSource::Lua:     return "[lua] ";
        case LogSource::Console: return "[console] ";
        case LogSource::Control: return "[control] ";
        default:                 return "";
        }
    }

    const char* SourceLabel(std::size_t i)
    {
        switch (static_cast<LogSource>(i))
        {
        case LogSource::Engine:  return "Engine";
        case LogSource::Lua:     return "Lua";
        case LogSource::Console: return "Console";
        case LogSource::Control: return "Control";
        default:                 return "?";
        }
    }

    bool ContainsNoCase(std::string_view hay, const std::string& needle)
    {
        if (needle.empty()) return true;
        const auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
            [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
        return it != hay.end();
    }

    // A toggle drawn in its colour when ON and dimmed when OFF.
    void Chip(const char* text, const ImVec4& onColor, bool* on)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, *on ? onColor
                                                 : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::PushStyleColor(ImGuiCol_Button, *on ? ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive)
                                                   : ImVec4(0.f, 0.f, 0.f, 0.f));
        if (ImGui::SmallButton(text)) *on = !*on;
        ImGui::PopStyleColor(2);
        ImGui::SameLine(0.0f, 4.0f);
    }
}

bool OutputLogPanel::FilterKey::operator==(const FilterKey& o) const
{
    return std::equal(std::begin(level), std::end(level), std::begin(o.level)) &&
           std::equal(std::begin(source), std::end(source), std::begin(o.source)) &&
           text == o.text && tag == o.tag;
}

void OutputLogPanel::Pump()
{
    m_scratch.clear();
    Logger::Get().CopyRecordsSince(m_lastSeq, m_scratch);
    if (m_scratch.empty()) return;

    // The ring kept fewer lines than this panel had not read yet.
    const std::uint64_t firstNew = m_scratch.front().seq;
    if (firstNew > m_lastSeq + 1)
        AppendGapRow(firstNew - m_lastSeq - 1);

    for (LogRecord& r : m_scratch)
    {
        m_lastSeq = r.seq;
        Append(std::move(r));
    }
    TrimToCapacity();
}

void OutputLogPanel::Append(LogRecord&& r)
{
    if (r.tagLen) ++m_tagCounts[std::string(r.Tag())];
    const std::uint64_t abs = m_nextAbs++;
    m_mirror.push_back(std::move(r));
    // A dirty index is rebuilt whole before the next draw; appending to it now
    // would only be thrown away.
    if (!m_filterDirty && Passes(m_mirror.back()))
        m_filtered.push_back(abs);
}

void OutputLogPanel::AppendGapRow(std::uint64_t lines)
{
    ++m_gaps;
    m_gapLines += lines;
    LogRecord g;                     // seq 0 = synthetic: never a ring line
    g.level  = LogLevel::Warning;
    g.frame  = Logger::CurrentFrame();
    g.text   = "... " + std::to_string(lines) +
               " line(s) left the log ring before the Output Log read them - the log FILE has them";
    Append(std::move(g));
}

void OutputLogPanel::TrimToCapacity()
{
    if (m_mirror.size() <= m_capacity) return;
    // Trim in chunks: one erase per ~1k lines rather than one per line.
    const std::size_t drop = (std::min)(m_mirror.size(), m_mirror.size() - m_capacity + 1024);
    m_mirror.erase(m_mirror.begin(), m_mirror.begin() + static_cast<std::ptrdiff_t>(drop));
    m_firstAbs += drop;
    const auto keep = std::lower_bound(m_filtered.begin(), m_filtered.end(), m_firstAbs);
    m_filtered.erase(m_filtered.begin(), keep);
}

bool OutputLogPanel::Passes(const LogRecord& r) const
{
    const std::size_t lv = static_cast<std::size_t>(r.level);
    if (lv < 4 && !m_active.level[lv]) return false;
    const std::size_t src = static_cast<std::size_t>(r.source);
    if (src < kSources && !m_active.source[src]) return false;
    if (!m_active.tag.empty() && r.Tag() != m_active.tag) return false;
    return ContainsNoCase(r.text, m_active.text);
}

void OutputLogPanel::RebuildFilter()
{
    m_filtered.clear();
    m_filtered.reserve(m_mirror.size());
    for (std::size_t i = 0; i < m_mirror.size(); ++i)
        if (Passes(m_mirror[i]))
            m_filtered.push_back(m_firstAbs + i);
    m_filterDirty = false;
}

void OutputLogPanel::Clear()
{
    m_mirror.clear();
    m_filtered.clear();
    m_firstAbs = m_nextAbs;
    m_tagCounts.clear();
    Logger::Get().ResetBadgeTallies();   // the ring and the log file are untouched
}

void OutputLogPanel::DrawBody(const LevelFilters& f, bool icons, const ConsoleHooks* console)
{
    // ---- Toolbar: level | source | tag | text filter | auto-scroll / copy / clear ----
    auto levelChip = [&](const char* icon, const char* label, LogLevel level, bool* on)
    {
        char text[64];
        std::snprintf(text, sizeof(text), "%s%s%s##lvl", icons ? icon : "", icons ? " " : "", label);
        Chip(text, LevelColor(level), on);
    };
    levelChip(FontAwesome::kError,   "Error",   LogLevel::Error,   f.showError);
    levelChip(FontAwesome::kWarning, "Warning", LogLevel::Warning, f.showWarning);
    levelChip(FontAwesome::kInfo,    "Info",    LogLevel::Info,    f.showInfo);
    levelChip(FontAwesome::kCheck,   "Success", LogLevel::Success, f.showSuccess);

    ImGui::TextDisabled("|");
    ImGui::SameLine(0.0f, 4.0f);
    for (std::size_t i = 0; i < kSources; ++i)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%s##src", SourceLabel(i));
        Chip(text, ImGui::GetStyleColorVec4(ImGuiCol_Text), &m_showSource[i]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", i == 0 ? "Lines nobody claimed (most engine code)"
                                  : i == 1 ? "Log.* and print() from Lua scripts"
                                  : i == 2 ? "The command line below: echo, result, and what it logged"
                                           : "-control pipe requests (an AI or a script)");
    }

    // Tag filter: "[TAG]" or "Name" - whatever the codebase writes first.
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##logTag", m_tag.empty() ? "All tags" : m_tag.c_str()))
    {
        if (ImGui::Selectable("All tags", m_tag.empty())) m_tag.clear();
        std::vector<std::pair<std::string, std::uint32_t>> tags(m_tagCounts.begin(), m_tagCounts.end());
        std::sort(tags.begin(), tags.end(),
                  [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second
                                                                                 : a.first < b.first; });
        if (tags.size() > 200) tags.resize(200);
        for (const auto& t : tags)
        {
            char label[128];
            std::snprintf(label, sizeof(label), "%s  (%u)", t.first.c_str(), t.second);
            if (ImGui::Selectable(label, m_tag == t.first)) m_tag = t.first;
        }
        ImGui::EndCombo();
    }

    const ImGuiStyle& st = ImGui::GetStyle();
    const float actionsW = ImGui::CalcTextSize("Auto-scroll").x + ImGui::GetFrameHeight()
                         + ImGui::CalcTextSize("Copy").x + ImGui::CalcTextSize("Clear").x
                         + st.FramePadding.x * 4.0f + st.ItemSpacing.x * 3.0f + st.ItemInnerSpacing.x;
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth((std::max)(80.0f, ImGui::GetContentRegionAvail().x - actionsW));
    ImGui::InputTextWithHint("##logFilter", "Filter (substring, case-insensitive)",
                             f.text, f.textSize);
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", f.autoScroll);
    ImGui::SameLine();
    const bool copyRequested = ImGui::SmallButton("Copy");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copy the visible (filtered) lines to the clipboard,\n"
                          "in the log file's own format");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        Clear();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clear the panel and the tab's error/warning badge.\n"
                          "The log FILE (log/DX12Log.txt) is untouched.");

    ImGui::Separator();

    // ---- Filter index: rebuilt only when a filter actually changed ----
    FilterKey key;
    key.level[static_cast<std::size_t>(LogLevel::Success)] = *f.showSuccess;
    key.level[static_cast<std::size_t>(LogLevel::Info)]    = *f.showInfo;
    key.level[static_cast<std::size_t>(LogLevel::Warning)] = *f.showWarning;
    key.level[static_cast<std::size_t>(LogLevel::Error)]   = *f.showError;
    for (std::size_t i = 0; i < kSources; ++i) key.source[i] = m_showSource[i];
    key.text = f.text;
    key.tag  = m_tag;
    if (m_filterDirty || !(key == m_active))
    {
        m_active = std::move(key);
        RebuildFilter();
    }

    std::string clip;   // filled only on a Copy press
    if (copyRequested)
        for (const std::uint64_t abs : m_filtered)
        {
            clip += m_mirror[static_cast<std::size_t>(abs - m_firstAbs)].text;
            clip += '\n';
        }

    // The list takes everything above the command line.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float cmdH = !console ? 0.0f
                     : m_multiline ? ImGui::GetTextLineHeight() * 5.0f + style.FramePadding.y * 2.0f
                                     + style.ItemSpacing.y * 2.0f
                                   : ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
    if (ImGui::BeginChild("LogContent", ImVec2(0, -cmdH), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(m_filtered.size()));
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const LogRecord& r = m_mirror[static_cast<std::size_t>(m_filtered[row] - m_firstAbs)];

                const char* glyph = "!";
                if (icons)
                    switch (r.level)
                    {
                    case LogLevel::Error:   glyph = FontAwesome::kError;   break;
                    case LogLevel::Warning: glyph = FontAwesome::kWarning; break;
                    case LogLevel::Success: glyph = FontAwesome::kCheck;   break;
                    default:                glyph = FontAwesome::kInfo;    break;
                    }

                // Compact row: HH:MM:SS  f<frame>  [source] message. The file's
                // "[YYYY-MM-DD HH:MM:SS]" prefix is fixed-width, so the clock is
                // at a fixed offset; a synthetic row (seq 0) has no prefix.
                char head[48];
                const std::string_view time =
                    (r.seq != 0 && r.text.size() > 20 && r.text[0] == '[')
                        ? std::string_view(r.text).substr(12, 8) : std::string_view("--:--:--");
                std::snprintf(head, sizeof(head), "%.*s f%-6llu",
                              static_cast<int>(time.size()), time.data(),
                              static_cast<unsigned long long>(r.frame));

                const ImVec4 col = LevelColor(r.level);
                ImGui::BeginGroup();   // the whole row answers IsItemHovered below
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(glyph);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextDisabled("%s", head);
                ImGui::SameLine(0.0f, 6.0f);
                std::string_view msg = r.seq ? r.Message() : std::string_view(r.text);
                // ONE line per row - the clipper assumes a fixed row height. A
                // multi-line message (a shader compiler's diagnostics) shows its
                // first line here and the whole text in the tooltip.
                const std::size_t nl = msg.find('\n');
                const bool more = nl != std::string_view::npos;
                if (more) msg = msg.substr(0, nl);
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                const char* badge = SourceBadge(r.source);
                if (*badge)
                {
                    ImGui::TextUnformatted(badge);
                    ImGui::SameLine(0.0f, 0.0f);
                }
                ImGui::TextUnformatted(msg.data(), msg.data() + msg.size());
                if (more)
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled("[...]");
                }
                ImGui::PopStyleColor();
                ImGui::EndGroup();

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\nseq %llu  frame %llu  thread %u  source %s%s%s\n"
                                      "double-click: copy this line",
                                      r.text.c_str(),
                                      static_cast<unsigned long long>(r.seq),
                                      static_cast<unsigned long long>(r.frame),
                                      r.thread, LogSourceName(r.source),
                                      r.requestId ? "  request #" : "",
                                      r.requestId ? std::to_string(r.requestId).c_str() : "");
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        ImGui::SetClipboardText(r.text.c_str());
                }
            }
        }

        // Auto-scroll only when pinned to bottom (1px epsilon vs subpixel drift);
        // a submitted command always scrolls, so its echo and result are seen.
        if (m_scrollToEnd ||
            (*f.autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f))
            ImGui::SetScrollHereY(1.0f);
        m_scrollToEnd = false;
    }
    ImGui::EndChild();
    if (copyRequested)
        ImGui::SetClipboardText(clip.c_str());

    if (console)
        DrawCommandLine(*console);
}

int OutputLogPanel::HistoryCallback(ImGuiInputTextCallbackData* data)
{
    auto* self = static_cast<OutputLogPanel*>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || self->m_history.empty())
        return 0;
    const int n = static_cast<int>(self->m_history.size());
    int pos = self->m_historyPos;
    if (data->EventKey == ImGuiKey_UpArrow)
        pos = pos < 0 ? n - 1 : (std::max)(0, pos - 1);
    else if (data->EventKey == ImGuiKey_DownArrow)
        pos = (pos < 0 || pos + 1 >= n) ? -1 : pos + 1;
    self->m_historyPos = pos;
    data->DeleteChars(0, data->BufTextLen);
    if (pos >= 0)
        data->InsertChars(0, self->m_history[static_cast<std::size_t>(pos)].c_str());
    return 0;
}

void OutputLogPanel::DrawCommandLine(const ConsoleHooks& c)
{
    // Where it runs, on the line's own face: Lua, in the Edit world, no undo -
    // or PLAY in red, because whatever it creates is discarded on Stop.
    if (c.playing)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f), "PLAY");
    else
        ImGui::TextDisabled("Lua");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs Lua against the %s world at the next frame boundary.\n"
                          "Tried as an expression first (its value is printed), else as a statement.\n"
                          "NOT undoable: a chunk mutates the World directly. Save before anything destructive.\n"
                          "Its echo, its result and every line it logs are tagged source=console\n"
                          "(the Console chip above filters to exactly that).\n"
                          "Up/Down: history.  The toggle on the right switches to multi-line (Ctrl+Enter runs).",
                          c.playing ? "PLAYING (discarded on Stop)" : "Edit-mode");
    ImGui::SameLine();

    ImGui::BeginDisabled(!c.available);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float runW    = ImGui::CalcTextSize("Run").x + st.FramePadding.x * 2.0f;
    const float toggleW = ImGui::CalcTextSize("+ lines").x + st.FramePadding.x * 2.0f;
    const float inputW  = (std::max)(80.0f, ImGui::GetContentRegionAvail().x - runW - toggleW
                                            - st.ItemSpacing.x * 2.0f);
    if (m_focusInput)
    {
        ImGui::SetKeyboardFocusHere();   // applies to the next widget
        m_focusInput = false;
    }

    bool submit = false;
    if (!m_multiline)
    {
        ImGui::SetNextItemWidth(inputW);
        submit = ImGui::InputTextWithHint(
            "##cmd", c.available ? "Lua command - Enter runs it at the next frame boundary"
                                 : "(no Lua VM / world to run in)",
            m_cmd, sizeof(m_cmd),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
            &OutputLogPanel::HistoryCallback, this);
    }
    else
    {
        ImGui::InputTextMultiline("##cmdml", m_cmd, sizeof(m_cmd),
                                  ImVec2(inputW, ImGui::GetTextLineHeight() * 5.0f));
        submit = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl &&
                 ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(m_multiline ? "1 line" : "+ lines"))
        m_multiline = !m_multiline;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(m_multiline ? "Back to a single line (Enter runs)"
                                      : "Multi-line editing (Ctrl+Enter runs)");
    ImGui::SameLine();
    if (ImGui::Button("Run")) submit = true;
    ImGui::EndDisabled();

    if (!submit) return;
    std::string text = m_cmd;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ' || text.back() == '\t'))
        text.pop_back();
    if (text.empty() || !c.submit) return;

    c.submit(text);
    if (m_history.empty() || m_history.back() != text) m_history.push_back(text);
    if (m_history.size() > 200) m_history.erase(m_history.begin());
    m_historyPos  = -1;
    m_cmd[0]      = '\0';
    m_focusInput  = true;    // keep typing
    m_scrollToEnd = true;
}

OutputLogPanel::VerifyResult OutputLogPanel::Verify()
{
    Pump();

    // -logpanel_verify_break: lose the newest mirrored line - the observable
    // effect of a pump that dropped one - so the comparison below must report
    // missing >= 1. Done here rather than inside Pump so the lost line is
    // certainly still in the logger's ring when it is looked for.
    if (m_breakVerify)
        for (auto it = m_mirror.end(); it != m_mirror.begin();)
            if ((--it)->seq)
            {
                m_mirror.erase(it);
                m_filterDirty = true;   // absolute indices past it moved; shutdown only
                break;
            }

    VerifyResult v;
    std::vector<LogRecord> ring;
    std::uint64_t first = 0;
    const std::uint64_t last = Logger::Get().CopyRecordsSince(0, ring, static_cast<std::size_t>(-1), &first);

    v.ring     = ring.size();
    v.mirror   = m_mirror.size();
    v.filtered = m_filtered.size();
    v.tags     = m_tagCounts.size();
    v.gaps     = m_gaps;
    v.gapLines = m_gapLines;

    std::uint64_t mirrorFirst = 0;
    for (const LogRecord& r : m_mirror)
    {
        if (!r.seq) continue;
        if (!mirrorFirst) mirrorFirst = r.seq;
        const std::size_t s = static_cast<std::size_t>(r.source);
        if (s < kSources) ++v.bySource[s];
    }
    if (ring.empty() || !mirrorFirst) return v;

    // The seqs BOTH hold: the ring may already have dropped the mirror's
    // oldest lines, and lines logged after the Pump above are not the
    // mirror's to have yet.
    const std::uint64_t lo = (std::max)(first, mirrorFirst);
    const std::uint64_t hi = (std::min)(last, m_lastSeq);
    v.overlap = hi >= lo ? static_cast<std::size_t>(hi - lo + 1) : 0;

    std::uint64_t prev = 0;
    for (const LogRecord& r : m_mirror)
    {
        if (!r.seq) continue;
        if (r.seq <= prev) ++v.dups;
        prev = r.seq;
        if (r.seq < lo || r.seq > hi) continue;
        const LogRecord& g = ring[static_cast<std::size_t>(r.seq - first)];
        if (g.text == r.text && g.level == r.level && g.source == r.source && g.frame == r.frame)
            ++v.matched;
        else
            ++v.mismatches;
    }
    v.missing = v.overlap > v.matched + v.mismatches ? v.overlap - v.matched - v.mismatches : 0;
    return v;
}
}
