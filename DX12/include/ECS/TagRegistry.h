#pragma once

// TagRegistry: the project-wide list of known gameplay tags (Unity TagManager analogue). An
// authoring aid only: entities still store plain strings in TagComponent and Lua Engine.HasTag
// still compares strings; the registry backs the editor dropdowns so designers pick tags instead of
// typing them (no "Playr" vs "Player" drift).
//
// Persisted as a VCS-friendly text file (asset/tags.txt): one tag per line, UTF-8, '#' comments and
// blank lines ignored, kept sorted. One list per project, not per scene. The Game build never loads
// it (nothing at runtime needs it).
//
// Header-only on purpose (like EventBus): a function-local static singleton, no .cpp to register
// in the vcxproj. Editor main thread only.

#include "ECS/TagComponent.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

class TagRegistry
{
public:
    static constexpr const char* kDefaultPath = "asset/tags.txt";

    static TagRegistry& Get()
    {
        static TagRegistry s_instance;
        return s_instance;
    }

    // A missing file is not an error: the registry starts empty and the file is created on the first
    // Add(). Idempotent; a re-Load() replaces the list.
    void Load(const std::string& path = kDefaultPath)
    {
        m_path = path;
        m_tags.clear();
        auto f = Sys::OpenIfstream(path);
        if (!f) return;
        std::string line;
        while (std::getline(f, line))
        {
            // Trim CR (files may be CRLF) + surrounding whitespace.
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            std::size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            line.erase(0, b);
            if (line.empty() || line[0] == '#') continue;
            if (line.size() >= TagComponent::LEN) line.resize(TagComponent::LEN - 1);
            if (!Contains(line)) m_tags.push_back(line);
        }
        std::sort(m_tags.begin(), m_tags.end());
    }

    const std::vector<std::string>& Tags() const { return m_tags; }

    bool Contains(const std::string& tag) const
    {
        return std::find(m_tags.begin(), m_tags.end(), tag) != m_tags.end();
    }

    // Adds + persists immediately. Rejects empty / over-length (TagComponent
    // slots are 31 chars) / duplicates. Returns true when the tag is in the
    // registry after the call (including "was already there").
    bool Add(const std::string& tag)
    {
        if (tag.empty() || tag.size() >= TagComponent::LEN) return false;
        if (Contains(tag)) return true;
        m_tags.push_back(tag);
        std::sort(m_tags.begin(), m_tags.end());
        Save();
        return true;
    }

    // Removes from the registry only - tags already stamped on entities are untouched (the Tag editor
    // shows them as "unregistered").
    bool Remove(const std::string& tag)
    {
        auto it = std::find(m_tags.begin(), m_tags.end(), tag);
        if (it == m_tags.end()) return false;
        m_tags.erase(it);
        Save();
        return true;
    }

private:
    void Save() const
    {
        auto f = Sys::OpenOfstream(m_path);
        if (!f) return;
        f << "# DX12 Engine tag registry - one gameplay tag per line.\n"
             "# Edited by the editor (Tag component -> Manage); hand-editing is fine.\n";
        for (const std::string& t : m_tags)
            f << t << "\n";
    }

    std::string              m_path = kDefaultPath;
    std::vector<std::string> m_tags;
};
