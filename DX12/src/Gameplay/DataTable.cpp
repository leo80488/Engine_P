#include "Gameplay/DataTable.h"

#include "Harness/RunLedger.h"
#include "Resource/AssetFS.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>
#include <cctype>

namespace Gameplay::Data
{
namespace
{
    constexpr size_t kMaxRows = 100000;
    constexpr size_t kMaxReported = 12;   // problems kept per refusal

    std::string Joined(const std::vector<std::string>& problems)
    {
        std::string out;
        for (size_t i = 0; i < problems.size() && i < kMaxReported; ++i)
            out += (i ? "; " : "") + problems[i];
        if (problems.size() > kMaxReported)
            out += "; ... and " + std::to_string(problems.size() - kMaxReported) + " more";
        return out;
    }
}

bool ValidationOff()
{
#if WITH_HARNESS
    static const bool s_off = Sys::CommandLine::Get().Has("-datatable_novalidate");
    return s_off;
#else
    return false;
#endif
}

std::shared_ptr<Table> ParseTable(const std::string& text, const std::string& path, std::vector<std::string>& errors)
{
    const Json doc = Json::parse(text, nullptr, /*allow_exceptions*/ false, /*ignore_comments*/ true);
    if (doc.is_discarded() || !doc.is_object())
    {
        errors.push_back("the file is not a JSON object");
        return nullptr;
    }
    auto table = std::make_shared<Table>();
    table->path = path;
    const Json& rows = doc.contains("rows") ? doc["rows"] : Json();

    if (ValidationOff())
    {
        if (!rows.is_object()) { errors.push_back("rows: must be an object"); return nullptr; }
        for (auto it = rows.begin(); it != rows.end(); ++it) table->rows[it.key()] = it.value();
        return table;
    }

    const size_t before = errors.size();
    if (doc.value("schemaVersion", 0) != 1) errors.push_back("schemaVersion: must be 1");
    if (!doc.contains("columns"))
        errors.push_back("columns: missing");
    else
        ParseSchema(doc["columns"], table->schema, errors);
    if (!rows.is_object())
        errors.push_back("rows: must be an object");
    else if (rows.size() > kMaxRows)
        errors.push_back("rows: " + std::to_string(rows.size()) + " rows, at most " + std::to_string(kMaxRows));
    if (errors.size() != before) return nullptr;   // a row cannot be judged by a schema that is not legal

    for (auto it = rows.begin(); it != rows.end(); ++it)
    {
        Json checked;
        if (ValidateRow(table->schema, it.key(), it.value(), checked, errors))
            table->rows[it.key()] = std::move(checked);
    }
    return errors.size() == before ? table : nullptr;
}

Tables& Tables::Get()
{
    static Tables s_instance;
    return s_instance;
}

bool Tables::Read(const std::string& path, bool reload, std::string& error)
{
    std::lock_guard<std::mutex> lock(m_lock);
    if (!reload)
    {
        // A table is read once; a refused one is not read again until Reload says so.
        if (m_tables.count(path)) return true;
        auto refused = m_refused.find(path);
        if (refused != m_refused.end())
        {
            error = refused->second;
            return false;
        }
    }

    std::string text;
    std::vector<std::string> problems;
    std::shared_ptr<Table> table;
    if (!Resource::AssetFS::Get().ReadFileText(path, text))
        problems.push_back("the file could not be read");
    else
        table = ParseTable(text, path, problems);

    if (!table)
    {
        error = Joined(problems);
        m_refused[path] = error;
        LOG_WARNING("[DATATABLE] '%s' refused%s: %s", path.c_str(),
                    m_tables.count(path) ? " (the previous table stays)" : "", error.c_str());
        NoteToLedger();
        return false;
    }
    auto it = m_tables.find(path);
    table->version = it == m_tables.end() ? 1u : it->second->version + 1u;
    if (reload) ++m_reloads;
    m_tables[path] = table;
    m_refused.erase(path);
    LOG_INFO("[DATATABLE] '%s' loaded: %zu column(s), %zu row(s), version %u", path.c_str(),
             table->schema.columns.size(), table->rows.size(), table->version);
    NoteToLedger();
    return true;
}

bool Tables::Load(const std::string& path, std::string& error)
{
    return Read(path, /*reload*/ false, error);
}

bool Tables::Reload(const std::string& path, std::string& error)
{
    return Read(path, /*reload*/ true, error);
}

std::shared_ptr<const Table> Tables::Find(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(m_lock);
    auto it = m_tables.find(path);
    return it == m_tables.end() ? nullptr : it->second;
}

std::string Tables::LoadedPathLike(const std::string& path) const
{
    const auto fold = [](std::string s)
    {
        for (char& c : s) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string wanted = fold(path);
    std::lock_guard<std::mutex> lock(m_lock);
    for (const auto& [loaded, table] : m_tables)
        if (fold(loaded) == wanted) return loaded;
    return {};
}

std::vector<Refusal> Tables::Refusals() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    std::vector<Refusal> out;
    for (const auto& [path, error] : m_refused) out.push_back({ path, error });
    return out;
}

size_t Tables::LoadedCount() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_tables.size();
}

uint32_t Tables::ReloadCount() const
{
    std::lock_guard<std::mutex> lock(m_lock);
    return m_reloads;
}

// Tells the run ledger what the tables did, as the "dataTables" block of the run's verdict file; the
// harness only (the block's names are literals a shipped build does not carry).
void Tables::NoteToLedger() const
{
#if WITH_HARNESS
    Json refused = Json::array();
    Json paths = Json::array();
    for (const auto& [path, error] : m_refused)
    {
        refused.push_back({ { "path", path }, { "error", error } });
        paths.push_back(path);
    }
    Json block;
    block["loaded"]       = m_tables.size();
    block["reloads"]      = m_reloads;
    block["refusedCount"] = m_refused.size();
    block["refusedPaths"] = std::move(paths);
    block["refused"]      = std::move(refused);
    Harness::RunLedger::Get().NoteSection("dataTables", block.dump());
#endif
}
}
