#include "Gameplay/DataSchema.h"

#include "Gameplay/GameplayTags.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace Gameplay::Data
{
namespace
{
    constexpr size_t kMaxColumns   = 64;
    constexpr size_t kMaxNameLength = 48;
    constexpr size_t kMaxStringLength = 4096;

    bool IsIdentifier(const std::string& name)
    {
        if (name.empty() || name.size() > kMaxNameLength) return false;
        for (size_t i = 0; i < name.size(); ++i)
        {
            const char c = name[i];
            const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
            if (!(letter || (i > 0 && c >= '0' && c <= '9'))) return false;
        }
        return true;
    }

    std::string Num(double v)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.10g", v);
        return buf;
    }

    // Why @p v is not a legal value of @p col, or empty when it is. A tag list is interned on the way.
    std::string ValueProblem(const Column& col, const Json& v)
    {
        switch (col.type)
        {
        case ColumnType::Number:
        case ColumnType::Int:
        {
            if (!v.is_number()) return "is not a number";
            const double d = v.get<double>();
            if (!std::isfinite(d)) return "is not a finite number";
            if (col.type == ColumnType::Int && d != std::floor(d)) return Num(d) + " is not a whole number";
            if (d < col.min) return Num(d) + " is below min " + Num(col.min);
            if (d > col.max) return Num(d) + " is above max " + Num(col.max);
            return {};
        }
        case ColumnType::Bool:
            return v.is_boolean() ? std::string{} : "is not true or false";
        case ColumnType::String:
            if (!v.is_string()) return "is not a string";
            if (v.get_ref<const std::string&>().size() > col.maxLength)
                return "is longer than maxLength " + std::to_string(col.maxLength);
            return {};
        case ColumnType::Enum:
            if (!v.is_string()) return "is not a string";
            if (std::find(col.values.begin(), col.values.end(), v.get_ref<const std::string&>()) == col.values.end())
                return "'" + v.get<std::string>() + "' is not one of the values";
            return {};
        case ColumnType::Tags:
        {
            if (!v.is_array()) return "is not a list of tag names";
            for (const Json& item : v)
            {
                if (!item.is_string()) return "holds a tag that is not a string";
                std::string why;
                if (!TagTable::Get().Intern(item.get_ref<const std::string&>(), &why).IsValid()) return why;
            }
            return {};
        }
        case ColumnType::Asset:
        {
            if (!v.is_string()) return "is not an asset path";
            const std::string& path = v.get_ref<const std::string&>();
            if (path.empty() || col.ext.empty()) return {};
            if (path.size() < col.ext.size()) return "'" + path + "' is not a " + col.ext + " path";
            std::string tail = path.substr(path.size() - col.ext.size());
            std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return tail == col.ext ? std::string{} : "'" + path + "' is not a " + col.ext + " path";
        }
        }
        return "has an unknown type";
    }

    // Keys a column may carry, by type.
    bool KeyApplies(ColumnType type, const std::string& key)
    {
        if (key == "type" || key == "required" || key == "default") return true;
        switch (type)
        {
        case ColumnType::Number:
        case ColumnType::Int:    return key == "min" || key == "max";
        case ColumnType::String: return key == "maxLength";
        case ColumnType::Enum:   return key == "values";
        case ColumnType::Asset:  return key == "ext";
        default:                 return false;
        }
    }

    bool ParseType(const std::string& text, ColumnType& out)
    {
        static const std::pair<const char*, ColumnType> kTypes[] = {
            { "number", ColumnType::Number }, { "int", ColumnType::Int },   { "bool", ColumnType::Bool },
            { "string", ColumnType::String }, { "enum", ColumnType::Enum }, { "tags", ColumnType::Tags },
            { "asset", ColumnType::Asset },
        };
        for (const auto& [name, type] : kTypes)
            if (text == name) { out = type; return true; }
        return false;
    }
}

bool InRange(double v, double lo, double hi)
{
    return std::isfinite(v) && v >= lo && v <= hi;
}

float Number(const Json& j, const char* key, float fallback, float lo, float hi)
{
    const float v = j.value(key, fallback);
    if (!InRange(v, lo, hi)) throw std::runtime_error(std::string("Invalid ") + key);
    return v;
}

const char* TypeName(ColumnType type)
{
    switch (type)
    {
    case ColumnType::Number: return "number";
    case ColumnType::Int:    return "int";
    case ColumnType::Bool:   return "bool";
    case ColumnType::String: return "string";
    case ColumnType::Enum:   return "enum";
    case ColumnType::Tags:   return "tags";
    case ColumnType::Asset:  return "asset";
    }
    return "?";
}

const Column* Schema::Find(const std::string& name) const
{
    auto it = std::lower_bound(columns.begin(), columns.end(), name,
                               [](const Column& c, const std::string& key) { return c.name < key; });
    return it != columns.end() && it->name == name ? &*it : nullptr;
}

bool ParseSchema(const Json& columns, Schema& out, std::vector<std::string>& errors)
{
    const size_t before = errors.size();
    out.columns.clear();
    if (!columns.is_object() || columns.empty())
    {
        errors.push_back("columns: must be an object with at least one column");
        return false;
    }
    if (columns.size() > kMaxColumns)
        errors.push_back("columns: " + std::to_string(columns.size()) + " columns, at most " + std::to_string(kMaxColumns));

    for (auto it = columns.begin(); it != columns.end(); ++it)
    {
        const std::string where = "column '" + it.key() + "': ";
        const Json& spec = it.value();
        if (!IsIdentifier(it.key()))
        {
            errors.push_back(where + "the name must be letters, digits and '_', not starting with a digit, at most 48 characters");
            continue;
        }
        if (!spec.is_object() || !spec.contains("type") || !spec["type"].is_string())
        {
            errors.push_back(where + "must be an object with a string \"type\"");
            continue;
        }
        Column col;
        col.name = it.key();
        const std::string typeText = spec["type"].get<std::string>();
        if (!ParseType(typeText, col.type))
        {
            errors.push_back(where + "unknown type '" + typeText + "' (number, int, bool, string, enum, tags, asset)");
            continue;
        }

        bool legal = true;
        for (auto key = spec.begin(); key != spec.end(); ++key)
        {
            if (key.key().rfind('_', 0) == 0) continue;   // a comment
            const bool known = key.key() == "type" || key.key() == "required" || key.key() == "default" ||
                               key.key() == "min" || key.key() == "max" || key.key() == "maxLength" ||
                               key.key() == "values" || key.key() == "ext";
            if (!known)
            {
                errors.push_back(where + "unknown key '" + key.key() + "'");
                legal = false;
            }
            else if (!KeyApplies(col.type, key.key()))
            {
                errors.push_back(where + "key '" + key.key() + "' does not apply to a " + typeText + " column");
                legal = false;
            }
        }
        if (!legal) continue;

        if (spec.contains("required"))
        {
            if (!spec["required"].is_boolean()) { errors.push_back(where + "required must be true or false"); continue; }
            col.required = spec["required"].get<bool>();
        }

        bool numeric = col.type == ColumnType::Number || col.type == ColumnType::Int;
        if (numeric)
        {
            for (const char* key : { "min", "max" })
            {
                if (!spec.contains(key)) continue;
                if (!spec[key].is_number() || !std::isfinite(spec[key].get<double>()))
                {
                    errors.push_back(where + key + " must be a finite number");
                    legal = false;
                    continue;
                }
                const double v = spec[key].get<double>();
                if (col.type == ColumnType::Int && v != std::floor(v))
                {
                    errors.push_back(where + key + " " + Num(v) + " is not a whole number");
                    legal = false;
                }
                (key[1] == 'i' ? col.min : col.max) = v;
            }
            if (legal && col.min > col.max)
            {
                errors.push_back(where + "min " + Num(col.min) + " is above max " + Num(col.max));
                legal = false;
            }
        }
        if (col.type == ColumnType::String && spec.contains("maxLength"))
        {
            const Json& v = spec["maxLength"];
            if (!v.is_number_integer() || v.get<long long>() < 1 || v.get<long long>() > static_cast<long long>(kMaxStringLength))
            {
                errors.push_back(where + "maxLength must be a whole number from 1 to " + std::to_string(kMaxStringLength));
                legal = false;
            }
            else
                col.maxLength = v.get<size_t>();
        }
        if (col.type == ColumnType::Enum)
        {
            const Json& values = spec.contains("values") ? spec["values"] : Json();
            if (!values.is_array() || values.empty())
            {
                errors.push_back(where + "an enum column needs a non-empty list \"values\"");
                legal = false;
            }
            else
            {
                for (const Json& v : values)
                {
                    if (!v.is_string() || v.get_ref<const std::string&>().empty())
                    {
                        errors.push_back(where + "values must be non-empty strings");
                        legal = false;
                        break;
                    }
                    const std::string& s = v.get_ref<const std::string&>();
                    if (std::find(col.values.begin(), col.values.end(), s) != col.values.end())
                    {
                        errors.push_back(where + "value '" + s + "' appears twice");
                        legal = false;
                        break;
                    }
                    col.values.push_back(s);
                }
            }
        }
        if (col.type == ColumnType::Asset && spec.contains("ext"))
        {
            const Json& v = spec["ext"];
            if (!v.is_string() || v.get_ref<const std::string&>().size() < 2 || v.get_ref<const std::string&>().size() > 16 ||
                v.get_ref<const std::string&>()[0] != '.')
            {
                errors.push_back(where + "ext must be an extension such as \".imat\"");
                legal = false;
            }
            else
            {
                col.ext = v.get<std::string>();
                std::transform(col.ext.begin(), col.ext.end(), col.ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            }
        }
        if (!legal) continue;

        if (spec.contains("default"))
        {
            if (col.required)
            {
                errors.push_back(where + "a required column cannot also have a default");
                continue;
            }
            const std::string problem = ValueProblem(col, spec["default"]);
            if (!problem.empty())
            {
                errors.push_back(where + "default " + problem);
                continue;
            }
            col.hasDefault = true;
            col.defaultValue = spec["default"];
        }
        out.columns.push_back(std::move(col));
    }

    std::sort(out.columns.begin(), out.columns.end(), [](const Column& a, const Column& b) { return a.name < b.name; });
    return errors.size() == before;
}

bool ValidateRow(const Schema& schema, const std::string& id, const Json& row, Json& out,
                 std::vector<std::string>& errors)
{
    const size_t before = errors.size();
    const std::string where = "row '" + id + "': ";
    if (id.empty() || id.size() > 64)
    {
        errors.push_back(where + "the id must be 1 to 64 characters");
        return false;
    }
    if (!row.is_object())
    {
        errors.push_back(where + "must be an object");
        return false;
    }
    out = Json::object();
    for (auto it = row.begin(); it != row.end(); ++it)
    {
        if (it.key().rfind('_', 0) == 0) continue;   // a comment
        const Column* col = schema.Find(it.key());
        if (!col)
        {
            errors.push_back(where + "unknown column '" + it.key() + "'");
            continue;
        }
        const std::string problem = ValueProblem(*col, it.value());
        if (!problem.empty())
        {
            errors.push_back(where + it.key() + " " + problem);
            continue;
        }
        out[it.key()] = it.value();
    }
    for (const Column& col : schema.columns)
    {
        if (row.contains(col.name)) continue;
        if (col.hasDefault) out[col.name] = col.defaultValue;
        else if (col.required) errors.push_back(where + "missing required column '" + col.name + "'");
    }
    return errors.size() == before;
}
}
