#pragma once

// Gameplay::Data: the one validator of authored gameplay data. A value is checked against a range
// (Number: what the Attack loader reads its fields with) and a table's columns declare theirs
// (ParseSchema), so a number out of range, a default outside its own column, an unknown key or an
// enum value that is not one of the values is refused the same way wherever the data came from.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Gameplay::Data
{
    using Json = nlohmann::json;

    // True when @p v is finite and lies in [lo, hi].
    bool InRange(double v, double lo, double hi);

    // @p j[@p key] as a float (else @p fallback), checked against [lo, hi]. Throws
    // std::runtime_error("Invalid <key>") for a value that is not a finite number in range.
    float Number(const Json& j, const char* key, float fallback, float lo, float hi);

    enum class ColumnType : uint8_t { Number, Int, Bool, String, Enum, Tags, Asset };

    // One column of a table; `min` / `max` bound a Number or Int, `maxLength` a String.
    struct Column
    {
        std::string              name;
        ColumnType               type = ColumnType::Number;
        bool                     required = false;
        bool                     hasDefault = false;
        Json                     defaultValue;
        double                   min = -1e30;
        double                   max = 1e30;
        size_t                   maxLength = 256;
        std::vector<std::string> values;   // Enum: the legal strings
        std::string              ext;      // Asset: the extension the path must end in (".imat"); empty = any
    };

    struct Schema
    {
        std::vector<Column> columns;   // sorted by name

        const Column* Find(const std::string& name) const;
    };

    const char* TypeName(ColumnType type);

    // Reads a "columns" object into @p out. False when any column is illegal; @p errors then holds
    // every problem found ("column 'hp': min 10 is above max 1"), not only the first.
    bool ParseSchema(const Json& columns, Schema& out, std::vector<std::string>& errors);

    // Checks the row @p id against @p schema and writes the row with every default filled in to
    // @p out. False when it breaks the schema; @p errors then gets one line per problem, each starting
    // "row '<id>':".
    bool ValidateRow(const Schema& schema, const std::string& id, const Json& row, Json& out,
                     std::vector<std::string>& errors);
}
