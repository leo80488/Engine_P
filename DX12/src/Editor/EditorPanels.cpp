#include "Editor/EditorPanels.h"

#include "imgui/imgui.h"

#include <algorithm>
#include <utility>

namespace Editor
{
namespace
{
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // A layout's lines in one preference value: '\n' -> "\\n", '\\' -> "\\\\".
    std::string Escape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size() + text.size() / 16);
        for (char c : text)
        {
            if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c != '\r') out += c;
        }
        return out;
    }

    std::string Unescape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '\\' && i + 1 < text.size())
            {
                out += text[i + 1] == 'n' ? '\n' : text[i + 1];
                ++i;
            }
            else out += text[i];
        }
        return out;
    }

    std::vector<std::string> SplitCommas(const std::string& text)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= text.size())
        {
            const std::size_t comma = text.find(',', start);
            std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!part.empty()) out.push_back(std::move(part));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return out;
    }
}

PanelRegistry& PanelRegistry::Get()
{
    static PanelRegistry s_registry;
    return s_registry;
}

std::size_t PanelRegistry::IndexOf(const std::string& id) const
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].id == id) return i;
    return kNone;
}

void PanelRegistry::Register(PanelDesc desc)
{
    const std::size_t i = IndexOf(desc.id);
    if (i != kNone)
    {
        m_panels[i] = std::move(desc);
        return;
    }
    m_ownOpen.push_back(desc.defaultOpen ? 1 : 0);
    m_panels.push_back(std::move(desc));
}

const PanelDesc* PanelRegistry::Find(const std::string& id) const
{
    const std::size_t i = IndexOf(id);
    return i == kNone ? nullptr : &m_panels[i];
}

PanelDesc* PanelRegistry::Find(const std::string& id)
{
    const std::size_t i = IndexOf(id);
    return i == kNone ? nullptr : &m_panels[i];
}

bool PanelRegistry::IsOpen(const PanelDesc& p) const
{
    if (p.isOpen) return p.isOpen();
    const std::size_t i = IndexOf(p.id);
    return i != kNone && m_ownOpen[i] != 0;
}

bool PanelRegistry::IsOpen(const std::string& id) const
{
    const PanelDesc* p = Find(id);
    return p && IsOpen(*p);
}

void PanelRegistry::SetOpen(const std::string& id, bool open)
{
    const std::size_t i = IndexOf(id);
    if (i == kNone) return;
    PanelDesc& p = m_panels[i];
    const bool was = IsOpen(p);
    if (p.setOpen) p.setOpen(open);
    else m_ownOpen[i] = open ? 1 : 0;
    if (was == open) return;
    if (open && p.onOpened) p.onOpened();
    if (!open && p.onClosed) p.onClosed();
}

void PanelRegistry::DrawOpen(const std::function<bool(const char*)>& held)
{
    // By index: a draw may register a panel (a lazily created tool).
    for (std::size_t i = 0; i < m_panels.size(); ++i)
    {
        PanelDesc& p = m_panels[i];
        bool hold = false;
        if (held)
            for (const std::string& name : p.holdNames) hold = hold || held(name.c_str());
        if (hold)
        {
            SetOpen(p.id, true);
            if (p.focusOnHold) ImGui::SetNextWindowFocus();
        }
        if (p.draw && IsOpen(p)) p.draw();
    }
}

void PanelRegistry::FrameBoundary()
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].onFrameBoundary) m_panels[i].onFrameBoundary();
}

void PanelRegistry::WorldChanged()
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].onWorldChanged) m_panels[i].onWorldChanged();
}

void PanelRegistry::PlayModeChanged()
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].onPlayModeChanged) m_panels[i].onPlayModeChanged();
}

void PanelRegistry::SelectionChanged()
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].onSelectionChanged) m_panels[i].onSelectionChanged();
}

void PanelRegistry::Shutdown()
{
    for (std::size_t i = 0; i < m_panels.size(); ++i)
        if (m_panels[i].onShutdown) m_panels[i].onShutdown();
}

void PanelRegistry::ApplyOpenStates(const std::map<std::string, std::string>& prefs)
{
    for (const PanelDesc& p : m_panels)
    {
        if (!p.persistOpen) continue;
        const auto it = prefs.find("panel." + p.id);
        if (it != prefs.end()) SetOpen(p.id, it->second == "1");
    }
}

void PanelRegistry::WriteOpenStates(std::map<std::string, std::string>& prefs) const
{
    for (const PanelDesc& p : m_panels)
    {
        if (!p.persistOpen) continue;
        const bool open = IsOpen(p);
        if (open != p.defaultOpen) prefs["panel." + p.id] = open ? "1" : "0";
    }
}

void PanelRegistry::SaveWorkspace(const std::string& name, std::string layout)
{
    Workspace w;
    w.name = name;
    w.layout = std::move(layout);
    for (const PanelDesc& p : m_panels)
        if (IsOpen(p)) w.open.push_back(p.id);
    for (Workspace& existing : m_workspaces)
        if (existing.name == name)
        {
            existing = std::move(w);
            return;
        }
    m_workspaces.push_back(std::move(w));
}

bool PanelRegistry::OpenWorkspace(const std::string& name, std::string& layout)
{
    const auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                                 [&](const Workspace& w) { return w.name == name; });
    if (it == m_workspaces.end()) return false;
    for (const PanelDesc& p : m_panels)
    {
        const bool open = std::find(it->open.begin(), it->open.end(), p.id) != it->open.end();
        SetOpen(p.id, open);
    }
    layout = it->layout;
    return true;
}

void PanelRegistry::DeleteWorkspace(const std::string& name)
{
    m_workspaces.erase(std::remove_if(m_workspaces.begin(), m_workspaces.end(),
                                      [&](const Workspace& w) { return w.name == name; }),
                       m_workspaces.end());
}

void PanelRegistry::ReadWorkspaces(const std::map<std::string, std::string>& prefs)
{
    m_workspaces.clear();
    for (const auto& [key, value] : prefs)
    {
        if (key.rfind("workspace.", 0) != 0) continue;
        const std::size_t dot = key.rfind('.');
        if (dot <= 10) continue;
        const std::string name = key.substr(10, dot - 10);
        const std::string field = key.substr(dot + 1);
        auto it = std::find_if(m_workspaces.begin(), m_workspaces.end(),
                               [&](const Workspace& w) { return w.name == name; });
        if (it == m_workspaces.end())
        {
            m_workspaces.push_back({ name, {}, {} });
            it = m_workspaces.end() - 1;
        }
        if (field == "open") it->open = SplitCommas(value);
        else if (field == "layout") it->layout = Unescape(value);
    }
}

void PanelRegistry::WriteWorkspaces(std::map<std::string, std::string>& prefs) const
{
    for (const Workspace& w : m_workspaces)
    {
        std::string open;
        for (const std::string& id : w.open) open += (open.empty() ? "" : ",") + id;
        prefs["workspace." + w.name + ".open"] = open;
        prefs["workspace." + w.name + ".layout"] = Escape(w.layout);
    }
}

void PanelRegistry::Clear()
{
    m_panels.clear();
    m_ownOpen.clear();
    m_workspaces.clear();
}
}
