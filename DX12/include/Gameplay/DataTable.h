#pragma once

// Data tables: a *.dtable file is JSON,
//   { "schemaVersion": 1,
//     "columns": { "hp": { "type": "number", "min": 1, "max": 100000, "default": 100 },
//                  "kind": { "type": "enum", "values": ["grunt", "boss"], "required": true },
//                  "tags": { "type": "tags" }, "icon": { "type": "asset", "ext": ".itex" }, ... },
//     "rows": { "wolf": { "hp": 40, "kind": "grunt", "tags": ["Team.Enemy"] }, ... } }
// and Gameplay::Data::Tables loads it once per path: the schema is parsed, every row is checked against
// it (Gameplay/DataSchema.h) and kept with its defaults filled in. A file that breaks either is
// REFUSED - the reasons are logged, kept in Refusals() and, in a harness run, written to run.json's
// dataTables block - and serves nothing; a reload that is refused keeps the previous good table.
// A *.dtable is an asset type (src/Resoruce/AssetTypes.json): a script or an asset that names one gets
// it packaged, and the HotReload watcher reloads an edited one.
// Main thread.

#include "Gameplay/DataSchema.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Gameplay::Data
{
    struct Table
    {
        std::string                path;
        Schema                     schema;
        std::map<std::string, Json> rows;      // by id, defaults filled in
        uint32_t                   version = 0; // 1 for the first load, +1 per accepted reload
    };

    // Parses and checks @p text. Null, with every problem in @p errors, when the schema or any row is
    // illegal. Registers nothing.
    std::shared_ptr<Table> ParseTable(const std::string& text, const std::string& path,
                                      std::vector<std::string>& errors);

    struct Refusal
    {
        std::string path;
        std::string error;   // the problems, joined by "; "
    };

    class Tables
    {
    public:
        static Tables& Get();

        // Loads @p path through AssetFS (once: a loaded table is returned as it is). False, with the
        // reason in @p error, when the file is missing or refused.
        bool Load(const std::string& path, std::string& error);
        // Reads @p path again. A refused reload keeps the table served so far.
        bool Reload(const std::string& path, std::string& error);

        // The loaded table, or null.
        std::shared_ptr<const Table> Find(const std::string& path) const;
        // The path a loaded table was loaded under that names the same file as @p path, ignoring case
        // and the slash direction (a file watcher reports lower case); empty when none is loaded.
        std::string LoadedPathLike(const std::string& path) const;

        std::vector<Refusal> Refusals() const;
        size_t   LoadedCount() const;
        uint32_t ReloadCount() const;

    private:
        bool Read(const std::string& path, bool reload, std::string& error);
        void NoteToLedger() const;   // callers hold m_lock

        mutable std::mutex                              m_lock;
        std::map<std::string, std::shared_ptr<Table>>   m_tables;
        std::map<std::string, std::string>              m_refused;   // path -> error
        uint32_t                                        m_reloads = 0;
    };

    // True under -datatable_novalidate (harness builds only): a table is accepted as it is read, the
    // schema and the rows unchecked. The data_table pack's reverse arm.
    bool ValidationOff();
}
