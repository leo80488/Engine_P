#include "Editor/EditorCommands.h"

#include "imgui/imgui.h"

#include <algorithm>

namespace Editor
{
    CommandRegistry& CommandRegistry::Get()
    {
        static CommandRegistry s_instance;
        return s_instance;
    }

    void CommandRegistry::Register(Command c)
    {
        for (Command& existing : m_commands)
            if (existing.id == c.id)
            {
                existing = std::move(c);
                return;
            }
        m_commands.push_back(std::move(c));
    }

    const Command* CommandRegistry::Find(std::string_view id) const
    {
        for (const Command& c : m_commands)
            if (c.id == id) return &c;
        return nullptr;
    }

    std::string CommandRegistry::LabelOf(const Command& c) const
    {
        if (c.dynamicLabel)
        {
            std::string label = c.dynamicLabel();
            if (!label.empty()) return label;
        }
        return c.label;
    }

    bool CommandRegistry::IsEnabled(const Command& c, std::string* why) const
    {
        if (!c.run) { if (why) *why = "not available in this build"; return false; }
        if (!c.enabled) return true;
        std::string reason;
        const bool ok = c.enabled(reason);
        if (!ok && why) *why = reason;
        return ok;
    }

    bool CommandRegistry::Execute(std::string_view id, std::string* why)
    {
        const Command* c = Find(id);
        if (!c) { if (why) *why = "no such command"; return false; }
        if (!IsEnabled(*c, why)) return false;
        // A copy: the command may register or replace commands while it runs.
        const std::function<void()> run = c->run;
        run();
        return true;
    }

    Chord CommandRegistry::ChordOf(const Command& c) const
    {
        const auto it = m_userChords.find(c.id);
        return it != m_userChords.end() ? it->second : c.defaultChord;
    }

    Chord CommandRegistry::ChordOf(std::string_view id) const
    {
        const Command* c = Find(id);
        return c ? ChordOf(*c) : Chord{};
    }

    void CommandRegistry::SetUserChord(const std::string& id, const Chord& chord)
    {
        m_userChords[id] = chord;
    }

    void CommandRegistry::ResetChord(const std::string& id)
    {
        m_userChords.erase(id);
    }

    std::vector<std::pair<std::string, std::string>> CommandRegistry::Conflicts() const
    {
        auto live = [](const Command& a, const Command& b) {
            if (a.scope == CommandScope::Panel || b.scope == CommandScope::Panel)
                return a.scope == b.scope && a.panel == b.panel;
            return true;   // Global and Document chords are all heard by the one dispatcher
        };
        std::vector<std::pair<std::string, std::string>> out;
        for (std::size_t i = 0; i < m_commands.size(); ++i)
        {
            const Chord ci = ChordOf(m_commands[i]);
            if (ci.Empty()) continue;
            for (std::size_t j = i + 1; j < m_commands.size(); ++j)
                if (ChordOf(m_commands[j]) == ci && live(m_commands[i], m_commands[j]))
                    out.emplace_back(m_commands[i].id, m_commands[j].id);
        }
        return out;
    }

    bool ChordPressed(const Chord& c)
    {
        if (c.Empty()) return false;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl != c.ctrl || io.KeyShift != c.shift || io.KeyAlt != c.alt) return false;
        return ImGui::IsKeyPressed(c.key, false);
    }
}
