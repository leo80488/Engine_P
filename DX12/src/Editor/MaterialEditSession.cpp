// MaterialEditSession.cpp - see the header.
#include "Editor/MaterialEditSession.h"

#include "Resource/AssetGuid.h"
#include "Resource/MaterialLiveOverlay.h"   // LiveMaterialBypass
#include "Resource/MaterialSerializer.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace Editor
{
namespace
{
    // -mateditor_autooverride_off, the reverse arm: edits do NOT join the override set.
    bool AutoOverrideOff()
    {
        static const bool s_off = Sys::CommandLine::Get().Has("-mateditor_autooverride_off");
        return s_off;
    }

    std::string Canon(const std::string& path)
    {
        std::string p = Sys::RelativizeAssetPath(path);
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }

    // key -> value of every "key = value" line of a BuildMaterialText dump ($g_
    // guid annotations skipped).
    std::unordered_map<std::string, std::string> DumpMap(const std::string& text)
    {
        std::unordered_map<std::string, std::string> out;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == '$' || line[0] == '#') continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            out[k] = v;
        }
        return out;
    }

    // "key = value" of @p key in @p text (a BuildMaterialText dump); "" = absent.
    std::string LineOf(const std::string& text, const std::string& key)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            if (k == key) return line + "\n";
        }
        return {};
    }
}

const char* MaterialKindName(MaterialKind k)
{
    switch (k)
    {
    case MaterialKind::Standard:   return "standard";
    case MaterialKind::CustomHLSL: return "custom";
    case MaterialKind::Graph:      return "graph";
    case MaterialKind::Instance:   return "instance";
    case MaterialKind::Entity:     return "entity";
    case MaterialKind::Function:   return "function";
    }
    return "standard";
}

MaterialEditSession::MaterialEditSession() : m_mat(std::make_unique<MaterialComponent>()) {}
MaterialEditSession::~MaterialEditSession() = default;

bool MaterialEditSession::Load(const std::string& path, std::string& why)
{
    auto mat = std::make_unique<MaterialComponent>();
    Resource::MaterialLoadInfo info;
    // The FILE: while this tab is dirty, the live overlay serves its staged version for the
    // path, and a Revert must not read that back.
    Resource::LiveMaterialBypass fromDisk;
    if (!Resource::LoadMaterial(path, *mat, &info))
    {
        why = "'" + path + "' did not load as a material (see the log)";
        return false;
    }
    m_mat  = std::move(mat);
    m_info = std::move(info);
    m_mat->SetAssetDirty(false);
    m_dirty = false;
    ++m_revision;
    return true;
}

bool MaterialEditSession::Open(const std::string& pathIn, std::string& why)
{
    const std::string path = Canon(pathIn);
    if (path.empty()) { why = "a path"; return false; }
    if (!Load(path, why)) return false;
    m_path = path;
    m_undo.clear();
    m_redo.clear();
    m_gesture = false;
    Rebase();
    NoteDiskStamp();
    return true;
}

// ==== Entity tabs ====
void MaterialEditSession::OpenEntity(const std::string& label, const std::string& source, const MaterialComponent& mat,
                                     const std::vector<std::string>& overrideKeys)
{
    m_entity = true;
    m_path   = "entity:" + label;
    m_mat    = std::make_unique<MaterialComponent>(mat);
    m_mat->SetAssetDirty(false);
    m_info = {};
    m_info.parentPath = Canon(source);   // "" = an inline material: nothing to override against
    m_info.overrideKeys.insert(overrideKeys.begin(), overrideKeys.end());
    m_info.explicitFormat = true;
    m_dirty   = false;
    m_gesture = false;
    m_edges   = {};
    m_undo.clear();
    m_redo.clear();
    ++m_revision;
    Rebase();
}

void MaterialEditSession::SyncFromEntity(const std::string& source, const MaterialComponent& mat,
                                         const std::vector<std::string>& overrideKeys)
{
    if (!m_entity || m_gesture || m_edges.Any()) return;
    *m_mat = mat;
    m_mat->SetAssetDirty(false);
    m_info.parentPath = Canon(source);
    m_info.overrideKeys.clear();
    m_info.overrideKeys.insert(overrideKeys.begin(), overrideKeys.end());
}

MaterialEditSession::EntityEdges MaterialEditSession::TakeEntityEdges()
{
    const EntityEdges e = m_edges;
    m_edges = {};
    return e;
}

void MaterialEditSession::OpenFunction(const std::string& path)
{
    m_function = true;
    m_path     = Canon(path);
    m_mat      = std::make_unique<MaterialComponent>();
    m_mat->customShadingModel = ShadingModel::Unlit;   // its output is shown as emissive
    m_mat->SetAssetDirty(false);
    m_info    = {};
    m_dirty   = false;
    m_gesture = false;
    m_undo.clear();
    m_redo.clear();
    ++m_revision;
}

MaterialKind MaterialEditSession::Kind() const
{
    if (m_function) return MaterialKind::Function;
    if (m_entity) return MaterialKind::Entity;
    if (IsInstance()) return MaterialKind::Instance;
    if (!m_mat->materialGraph.empty()) return MaterialKind::Graph;   // its shader is the graph's
    if (m_mat->useCustomShader && !m_mat->customShaderPath.empty()) return MaterialKind::CustomHLSL;
    return MaterialKind::Standard;
}

// ==== Editing ====
void MaterialEditSession::MarkEdited(const std::string& key)
{
    if (IsInstance() && !key.empty() && !AutoOverrideOff()) m_info.overrideKeys.insert(key);
    m_mat->SetEditDirty();
    if (!m_entity) m_dirty = true;
    ++m_revision;
}

bool MaterialEditSession::SetOverride(const std::string& key, bool on, std::string& why)
{
    if (!IsInstance()) { why = "not an instance - a standalone material writes every key"; return false; }
    if (key.empty()) { why = "a key"; return false; }
    if ((m_info.overrideKeys.count(key) != 0) == on) return true;
    PushUndo(Capture());
    if (on)
        m_info.overrideKeys.insert(key);
    else
    {
        m_info.overrideKeys.erase(key);
        // Follow the parent again: its value, through the loader's parser.
        MaterialComponent parent{};
        if (Resource::LoadMaterial(m_info.parentPath, parent))
        {
            const std::string line = LineOf(Resource::BuildMaterialText(parent), key);
            if (!line.empty()) Resource::ApplyMaterialTextDelta(*m_mat, line);
        }
    }
    m_mat->SetEditDirty();
    if (m_entity) m_edges = { true, true, true };
    else          m_dirty = true;
    ++m_revision;
    Rebase();
    return true;
}

bool MaterialEditSession::Set(const std::vector<std::pair<std::string, std::string>>& kv, std::string& why)
{
    if (kv.empty()) { why = "nothing to set"; return false; }
    if (!Resource::ValidateMaterialEdit(*m_mat, kv, why)) return false;
    std::string text;
    for (const auto& [k, v] : kv)
    {
        if (k.empty() || k.find_first_of("= \t\r\n") != std::string::npos || v.find_first_of("\r\n") != std::string::npos)
        {
            why = "'" + k + "' is not a key (the .imat vocabulary: blendMode, roughnessMin, tex_NORMALMAP, "
                  "cparam_<Name>, ...)";
            return false;
        }
        text += k + " = " + v + "\n";
    }
    // Every key must be one the format knows: apply to a copy first, and the
    // key must then appear in its full dump (the parser skips unknown keys
    // silently - a typo would otherwise "succeed" and change nothing).
    MaterialComponent probe = *m_mat;
    Resource::ApplyMaterialTextDelta(probe, text);
    const std::string dump = Resource::BuildMaterialText(probe);
    for (const auto& [k, v] : kv)
        if (LineOf(dump, k).empty())
        {
            why = "'" + k + "' is not a key this material's format knows";
            return false;
        }
    PushUndo(Capture());
    Resource::ApplyMaterialTextDelta(*m_mat, text);
    for (const auto& [k, v] : kv) MarkEdited(k);
    if (m_entity) m_edges = { true, true, true };
    Rebase();
    return true;
}

std::string MaterialEditSession::ValueOf(const std::string& key) const
{
    const auto m = DumpMap(Resource::BuildMaterialText(*m_mat));
    const auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

bool MaterialEditSession::ResetOverrides(std::string& why)
{
    if (!IsInstance()) { why = "not an instance"; return false; }
    auto parent = std::make_unique<MaterialComponent>();
    if (!Resource::LoadMaterial(m_info.parentPath, *parent))
    {
        why = "the parent '" + m_info.parentPath + "' did not load";
        return false;
    }
    PushUndo(Capture());
    m_mat = std::move(parent);
    m_info.overrideKeys.clear();
    m_info.unsetKeys.clear();
    m_mat->SetEditDirty();
    if (m_entity) m_edges = { true, true, true };
    else          m_dirty = true;
    ++m_revision;
    Rebase();
    return true;
}

void MaterialEditSession::BeginFrame()
{
    m_mat->SetAssetDirty(false);
}

void MaterialEditSession::EndFrame(bool anyItemActive)
{
    if (m_mat->IsAssetDirty())
    {
        // The first edit of a gesture: the state before it is one undo step - of
        // this session's ring, or (an entity tab) of the scene's history.
        if (!m_gesture)
        {
            if (m_entity) m_edges.began = true;
            else          PushUndo(std::move(m_base));
            m_gesture = true;
        }
        if (m_entity) m_edges.edited = true;
        else          m_dirty = true;
        ++m_revision;
    }
    if (m_gesture && !anyItemActive)
    {
        m_gesture = false;
        if (m_entity) m_edges.ended = true;
        Rebase();
    }
}

// ==== Undo ====
MaterialEditSession::State MaterialEditSession::Capture() const
{
    State s;
    s.mat   = std::make_unique<MaterialComponent>(*m_mat);
    s.info  = m_info;
    s.dirty = m_dirty;
    return s;
}

void MaterialEditSession::Restore(State& s)
{
    m_mat   = std::move(s.mat);
    m_info  = std::move(s.info);
    m_dirty = s.dirty;
    // The renderer and the preview see a changed material.
    m_mat->SetDirty();
    ++m_revision;
}

void MaterialEditSession::PushUndo(State s)
{
    if (!s.mat || m_entity) return;   // an entity tab's undo is the scene's
    m_undo.push_back(std::move(s));
    while (m_undo.size() > kUndoDepth) m_undo.pop_front();
    m_redo.clear();
}

void MaterialEditSession::Rebase()
{
    m_base = Capture();
}

bool MaterialEditSession::Undo()
{
    if (m_entity) return false;   // the scene's history (the window routes Ctrl+Z there)
    if (m_gesture) { m_gesture = false; Rebase(); }
    if (m_undo.empty()) return false;
    m_redo.push_back(Capture());
    State s = std::move(m_undo.back());
    m_undo.pop_back();
    Restore(s);
    Rebase();
    return true;
}

bool MaterialEditSession::Redo()
{
    if (m_entity) return false;
    if (m_gesture) { m_gesture = false; Rebase(); }
    if (m_redo.empty()) return false;
    m_undo.push_back(Capture());
    State s = std::move(m_redo.back());
    m_redo.pop_back();
    Restore(s);
    Rebase();
    return true;
}

// ==== Files ====
void MaterialEditSession::NoteDiskStamp()
{
    std::error_code ec;
    const std::filesystem::path p = Sys::U8Path(m_path);
    m_diskSize = std::filesystem::file_size(p, ec);
    if (ec) m_diskSize = 0;
    const auto t = std::filesystem::last_write_time(p, ec);
    m_diskTime = ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

bool MaterialEditSession::ChangedOnDisk() const
{
    if (m_entity || m_function) return false;   // a function's file is its graph's (the window's)
    std::error_code ec;
    const std::filesystem::path p = Sys::U8Path(m_path);
    const std::uint64_t size = std::filesystem::file_size(p, ec);
    if (ec) return false;   // gone: nothing to reload from
    const auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return false;
    return size != m_diskSize || static_cast<std::int64_t>(t.time_since_epoch().count()) != m_diskTime;
}

bool MaterialEditSession::Save(std::string& why)
{
    if (m_function) return true;   // the window saves the function's graph
    if (m_entity)
    {
        why = "an entity tab has no file: Save as Instance (promote), Apply to Asset or Clear the entity's overrides";
        return false;
    }
    const bool ok = IsInstance()
        ? Resource::SaveMaterialInstance(m_path, m_info.parentPath, *m_mat, &m_info.overrideKeys, &m_info.unsetKeys)
        : Resource::SaveMaterial(*m_mat, m_path);
    if (!ok)
    {
        why = "the save of '" + m_path + "' failed (see the log)";
        return false;
    }
    if (IsInstance()) m_info.explicitFormat = true;   // written with the explicit set
    m_dirty = false;
    m_mat->SetAssetDirty(false);
    NoteDiskStamp();
    Rebase();
    return true;
}

bool MaterialEditSession::SaveAs(const std::string& pathIn, std::string& why)
{
    if (m_function) { why = "a Material Function tab: Save writes its graph"; return false; }
    if (m_entity)
    {
        why = "an entity tab has no file: Save as Instance (promote), Apply to Asset or Clear the entity's overrides";
        return false;
    }
    const std::string path = Canon(pathIn);
    if (path.empty()) { why = "a path"; return false; }
    const bool ok = IsInstance()
        ? Resource::SaveMaterialInstance(path, m_info.parentPath, *m_mat, &m_info.overrideKeys, &m_info.unsetKeys)
        : Resource::SaveMaterial(*m_mat, path);
    if (!ok)
    {
        why = "the save to '" + path + "' failed (see the log)";
        return false;
    }
    Resource::AssetGuidIndex::Get().EnsureGuid(path);
    m_path = path;
    if (IsInstance()) m_info.explicitFormat = true;
    m_dirty = false;
    m_mat->SetAssetDirty(false);
    NoteDiskStamp();
    Rebase();
    return true;
}

bool MaterialEditSession::SaveAsInstance(const std::string& pathIn, std::string& why,
                                         std::vector<std::string>* keysOut)
{
    if (m_function) { why = "a Material Function tab has no material to make an instance of"; return false; }
    if (m_entity)
    {
        why = "an entity tab has no file: Save as Instance (promote), Apply to Asset or Clear the entity's overrides";
        return false;
    }
    const std::string path = Canon(pathIn);
    if (path.empty()) { why = "a path"; return false; }
    if (path == m_path) { why = "an instance of a file cannot be the file itself"; return false; }
    // The keys the staged material changes against THIS file as it is on disk: both sides
    // through the same writer, compared key by key. The serializer's value diff is not used: it
    // writes `domain` unconditionally and compares resolved texture defaults, keys nobody
    // changed.
    MaterialComponent disk{};
    {
        Resource::LiveMaterialBypass fromDisk;   // the file, not this tab's live version
        if (!Resource::LoadMaterial(m_path, disk))
        {
            why = "'" + m_path + "' did not load from disk - save it first";
            return false;
        }
    }
    const auto before = DumpMap(Resource::BuildMaterialText(disk));
    const auto after  = DumpMap(Resource::BuildMaterialText(*m_mat));
    std::unordered_set<std::string> keys;
    for (const auto& [k, v] : after)
    {
        const auto it = before.find(k);
        if (it == before.end() || it->second != v) keys.insert(k);
    }
    for (const auto& [k, v] : before)
        if (!after.count(k) && (k.rfind("cparam_", 0) == 0 || k.rfind("ctex_", 0) == 0)) keys.insert(k);
    // Never the format markers.
    for (const char* marker : { "matfmt", "imatfmt", "cparamFmt", "parent" }) keys.erase(marker);
    if (!Resource::SaveMaterialInstance(path, m_path, *m_mat, &keys, nullptr))
    {
        why = "the instance could not be written to '" + path + "' (see the log)";
        return false;
    }
    Resource::AssetGuidIndex::Get().EnsureGuid(path);
    std::vector<std::string> sorted(keys.begin(), keys.end());
    std::sort(sorted.begin(), sorted.end());
    std::string list;
    for (const std::string& k : sorted) list += (list.empty() ? "" : ",") + k;
    LOG_INFO("[MATEDITOR] instance %s of %s: overrides=%zu keys=%s", path.c_str(), m_path.c_str(), keys.size(),
             list.empty() ? "-" : list.c_str());
    if (keysOut) *keysOut = std::move(sorted);
    return true;
}

bool MaterialEditSession::Revert(std::string& why)
{
    if (m_function) return true;   // the window reverts the function's graph
    if (m_entity)
    {
        why = "an entity tab has no file: Save as Instance (promote), Apply to Asset or Clear the entity's overrides";
        return false;
    }
    State before = Capture();
    if (!Load(m_path, why)) return false;
    PushUndo(std::move(before));
    m_gesture = false;
    Rebase();
    NoteDiskStamp();
    return true;
}
}
