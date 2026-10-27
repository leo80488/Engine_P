#include "Harness/SettingsStoreScript.h"

#ifdef WITH_HARNESS

#include "Settings/SettingsStore.h"
#include "Settings/LegacyFlags.h"   // the legacy / legacycheck ops
#include "System/PathUtil.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <vector>

namespace Harness
{
namespace
{
    using json = nlohmann::json;

    Settings::Request ToRequest(const json& v)
    {
        Settings::Request q;
        if (v.is_boolean())     { q.type = Settings::Request::Type::Bool;   q.b = v.get<bool>(); }
        else if (v.is_number()) { q.type = Settings::Request::Type::Number; q.number = v.get<double>(); }
        else if (v.is_string()) { q.type = Settings::Request::Type::Text;   q.text = v.get<std::string>(); }
        else if (v.is_object() && v.value("nan", false))
        {
            q.type   = Settings::Request::Type::Number;
            q.number = std::numeric_limits<double>::quiet_NaN();
        }
        return q;
    }

    template <class S>
    json ToJson(const Settings::RowT<S>& r, const Settings::Value& v)
    {
        switch (r.kind)
        {
        case Settings::Kind::Bool:   return v.b;
        case Settings::Kind::Int:
        case Settings::Kind::Enum:
        case Settings::Kind::Choice: return v.i;
        case Settings::Kind::Float:  return v.f;
        case Settings::Kind::String: return v.s;
        }
        return nullptr;
    }

    json Where(const Settings::SettingsStore& st, const Settings::Row& r)
    {
        json o;
        o["op"]             = "where";
        o["ok"]             = true;
        o["path"]           = r.path;
        o["policy"]         = Settings::PolicyName(r.spec.policy);
        o["value"]          = ToJson(r, st.Effective(r));
        o["owner"]          = ToJson(r, st.OwnerValue(r));   // a Capped row's allowance
        o["source"]         = Settings::LayerName(st.Source(r));
        o["revision"]       = st.RowRevision(r);
        o["globalRevision"] = st.Revision();
        json layers = json::object();
        for (size_t l = 0; l < Settings::kLayerCount; ++l)
            if (const auto& v = st.At(r, static_cast<Settings::Layer>(l)))
                layers[Settings::LayerName(static_cast<Settings::Layer>(l))] = ToJson(r, *v);
        o["layers"] = std::move(layers);
        json history = json::array();
        for (const Settings::WriteRecord& w : st.History(r))
        {
            json e;
            e["layer"]  = Settings::LayerName(w.layer);
            e["writer"] = w.writer;
            if (w.cleared) e["cleared"] = true;
            else           e["value"]   = ToJson(r, w.value);
            history.push_back(std::move(e));
        }
        o["history"] = std::move(history);
        return o;
    }
}

bool RunSettingsStoreScript(const std::string& opsPath, const std::string& outPath, std::string& error)
{
    std::string text;
    if (!Sys::ReadFileText(opsPath, text))
    {
        error = "cannot read the script";
        return false;
    }

    // The store's own rows: a copy of the player rows, so a script can give a row another
    // policy ({"op":"policy"}) and every policy's rules stay testable whichever rows use
    // it. The store is built at the first write / clear / where / counts.
    std::vector<Settings::Row> rows(Settings::PlayerRows().begin(), Settings::PlayerRows().end());
    std::optional<Settings::SettingsStore> store;
    std::string results;
    std::istringstream lines(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(lines, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line.compare(first, 2, "//") == 0) continue;

        json in;
        try { in = json::parse(line); }
        catch (const json::exception& e)
        {
            error = "line " + std::to_string(lineNo) + " does not parse: " + e.what();
            return false;
        }
        const std::string op = in.value("op", std::string());
        json out;
        out["op"] = op;

        if (op == "policy")
        {
            if (store)
            {
                error = "line " + std::to_string(lineNo) + ": a policy op after the store's first op";
                return false;
            }
            const std::string path = in.value("path", std::string()), name = in.value("policy", std::string());
            Settings::Row* row = nullptr;
            for (Settings::Row& r : rows)
                if (path == r.path) row = &r;
            bool known = false;
            for (Settings::Policy p : { Settings::Policy::Layered, Settings::Policy::LastWrite, Settings::Policy::Capped })
                if (name == Settings::PolicyName(p) && row) { row->spec.policy = p; known = true; }
            out["ok"] = known;
            if (!row) out["error"] = "unknown path";
            else if (!known) out["error"] = "unknown policy";
            results += out.dump() + "\n";
            continue;
        }
        // A row's old spellings (Settings/LegacyFlags.h) against this process's command line:
        // a copy of an engine.json row (its kind, range and names) given `spec` as its
        // .legacyFlag, read in `style` (pass / developer); legacycheck: the spec's
        // well-formedness.
        if (op == "legacy" || op == "legacycheck")
        {
            const Settings::DevRow* src = Settings::FindDevRow(in.value("path", std::string()));
            if (!src)
            {
                out["ok"]    = false;
                out["error"] = "unknown path";
                results += out.dump() + "\n";
                continue;
            }
            const std::string spec = in.value("spec", std::string());
            Settings::DevRow row = *src;
            row.spec.legacyFlag = spec.c_str();
            row.spec.legacyRead = nullptr;
            if (in.value("valueOnly", false)) row.spec.flags |= Settings::kSettingLegacyValueOnly;
            out["ok"] = true;
            if (op == "legacycheck")
            {
                out["error"] = Settings::LegacySpecError(row);
                results += out.dump() + "\n";
                continue;
            }
            const Settings::LegacyStyle style = in.value("style", std::string()) == "developer"
                ? Settings::LegacyStyle::Developer : Settings::LegacyStyle::Pass;
            Settings::Value v;
            std::string writer, why;
            const Settings::LegacyResult got = Settings::ReadLegacy(row, style, v, writer, &why);
            out["result"] = Settings::LegacyResultName(got);
            out["writer"] = writer;
            out["why"]    = why;
            if (got == Settings::LegacyResult::Taken) out["value"] = ToJson(row, v);
            results += out.dump() + "\n";
            continue;
        }
        if (!store) store.emplace(std::span<const Settings::Row>(rows));
        Settings::SettingsStore& st = *store;

        if (op == "counts")
        {
            out["ok"]   = true;
            out["rows"] = st.Rows().size();
            for (size_t l = 1; l < Settings::kLayerCount; ++l)
                out[Settings::LayerName(static_cast<Settings::Layer>(l))] = st.CountIn(static_cast<Settings::Layer>(l));
            out["lastWrite"] = st.CountPolicy(Settings::Policy::LastWrite);
            out["capped"]    = st.CountPolicy(Settings::Policy::Capped);
            results += out.dump() + "\n";
            continue;
        }
        if (op != "write" && op != "clear" && op != "where")
        {
            error = "line " + std::to_string(lineNo) + ": unknown op '" + op + "'";
            return false;
        }

        const std::string path = in.value("path", std::string());
        const Settings::Row* row = st.Find(path);
        if (!row)
        {
            out["ok"]    = false;
            out["error"] = "unknown path";
            results += out.dump() + "\n";
            continue;
        }
        if (op == "where")
        {
            results += Where(st, *row).dump() + "\n";
            continue;
        }

        Settings::Layer layer = Settings::Layer::Count;
        if (!Settings::ParseLayer(in.value("layer", std::string()), layer))
        {
            out["ok"]    = false;
            out["error"] = "unknown layer";
            results += out.dump() + "\n";
            continue;
        }
        const std::string writer = in.value("writer", std::string("script"));
        if (op == "clear")
        {
            out["ok"] = st.Clear(*row, layer, writer);
            results += out.dump() + "\n";
            continue;
        }
        Settings::Value v;
        const Settings::Refusal refusal = Settings::Coerce(*row, ToRequest(in.contains("value") ? in["value"] : json()), v);
        out["refusal"] = Settings::RefusalName(refusal);
        out["ok"]      = refusal == Settings::Refusal::None && st.Write(*row, layer, v, writer);
        results += out.dump() + "\n";
    }

    Sys::CreateParentDirs(outPath);
    auto f = Sys::OpenOfstream(outPath, std::ios::binary);
    if (f) f.write(results.data(), static_cast<std::streamsize>(results.size()));
    if (!f)
    {
        error = "cannot write '" + outPath + "'";
        return false;
    }
    return true;
}
}

#endif  // WITH_HARNESS
