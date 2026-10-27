#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <deque>
#include <string>
#include <string_view>

namespace Editor {
// Keep the authored JSON: shorthand paths and additional metadata round-trip unchanged.
class AnimSetDocument {
    using Json = nlohmann::json;

public:
    AnimSetDocument() { New(); }
    void New() {
        m_definition = {{"schema", 1}, {"slots", Json::object()}};
        m_saved = m_definition; m_undo.clear(); m_redo.clear(); EndGesture();
    }
    static bool Validate(const Json& definition, std::string& error) {
        try {
            if (definition.value("schema", 0) != 1 || !definition.at("slots").is_object())
                throw std::runtime_error("Expected Animation Set schema 1 and a slots object.");
            for (auto it = definition["slots"].begin(); it != definition["slots"].end(); ++it) {
                const auto& slot = it.value();
                const std::string asset = slot.is_string() ? slot.get<std::string>() : slot.at("path").get<std::string>();
                if (it.key().empty() || asset.empty())
                    throw std::runtime_error("Each slot needs a unique name and an animation asset path.");
                if (!slot.is_string()) {
                    slot.value("sourceClip", std::string()); slot.value("loop", true);
                    if (!std::isfinite(slot.value("rate", 1.f)))
                        throw std::runtime_error("Playback rate must be finite.");
                }
            }
            error.clear(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    bool Load(std::string_view text, std::string& error) {
        try {
            auto definition = Json::parse(text);
            if (!Validate(definition, error)) return false;
            m_definition = std::move(definition); m_saved = m_definition;
            m_undo.clear(); m_redo.clear(); EndGesture(); return true;
        } catch (const std::exception& e) { error = e.what(); return false; }
    }
    const Json& Definition() const { return m_definition; }
    std::string Text() const { return m_definition.dump(2) + "\n"; }
    bool Dirty() const { return m_definition != m_saved; }
    void Saved() { m_saved = m_definition; }
    void BeginGesture() { m_gesture = true; m_gestureCommitted = false; }
    void EndGesture() { m_gesture = false; m_gestureCommitted = false; }
    void Commit(Json next) {
        if (next == m_definition) return;
        if (!m_gesture || !m_gestureCommitted) {
            m_undo.push_back(m_definition); if (m_undo.size() > 64) m_undo.pop_front();
        }
        if (m_gesture) m_gestureCommitted = true;
        m_definition = std::move(next); m_redo.clear();
    }
    void Set(const std::string& name, Json slot) {
        auto next = m_definition; next["slots"][name] = std::move(slot); Commit(std::move(next));
    }
    bool Rename(const std::string& name, const std::string& replacement, std::string& error) {
        if (name == replacement) return true;
        const auto& slots = m_definition["slots"];
        if (replacement.empty() || slots.contains(replacement) || !slots.contains(name)) {
            error = "Slot names must be non-empty and unique."; return false;
        }
        auto next = m_definition; next["slots"][replacement] = next["slots"][name];
        next["slots"].erase(name); Commit(std::move(next)); error.clear(); return true;
    }
    void Remove(const std::string& name) { auto next = m_definition; next["slots"].erase(name); Commit(std::move(next)); }
    bool Undo() {
        EndGesture(); if (m_undo.empty()) return false;
        m_redo.push_back(m_definition); m_definition = std::move(m_undo.back()); m_undo.pop_back(); return true;
    }
    bool Redo() {
        EndGesture(); if (m_redo.empty()) return false;
        m_undo.push_back(m_definition); m_definition = std::move(m_redo.back()); m_redo.pop_back(); return true;
    }

private:
    Json m_definition, m_saved;
    std::deque<Json> m_undo, m_redo;
    bool m_gesture = false, m_gestureCommitted = false;

public:
    std::string path;
};
}
