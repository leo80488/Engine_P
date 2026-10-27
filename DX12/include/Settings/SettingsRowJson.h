#pragma once

// Settings/SettingsRowJson.h - a row's value in its document's JSON: the typed read,
// the value in the file's own form, and the load's sanitize. settings.json's loader
// (EngineSettings.cpp) and engine.json's (EngineDevSettings.cpp) both walk their rows
// through these; only the two TUs that parse a document include it.

#include "Settings/SettingsStore.h"   // Value, ValueOf
#include "System/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace Settings
{
    // One key of an object into @p out: true when the key is there with a usable value;
    // absent or the wrong type leaves @p out alone and answers false, the latter with a
    // warning in @p who's name. Do not use nlohmann's value() here: it throws on a type
    // mismatch ("vsync": "yes") and would abort the whole document. For a well-typed value
    // this is exactly value(): both go through get<T>().
    template <class T>
    bool TryValue(const nlohmann::json& obj, const char* key, T& out, const char* who)
    {
        const auto it = obj.find(key);
        if (it == obj.end()) return false;
        try
        {
            out = it->template get<T>();
            return true;
        }
        catch (const nlohmann::json::exception&)
        {
            LOG_WARNING("%s: '%s' holds a %s, not the expected type - keeping %s",
                        who, key, it->type_name(), "the previous value");
            return false;
        }
    }

    // A row's value in the file's own form (an Enum with a codec as its token).
    template <class S>
    nlohmann::json ValueJson(const RowT<S>& r, const Value& v)
    {
        switch (r.kind)
        {
        case Kind::Bool:   return v.b;
        case Kind::Int:
        case Kind::Choice: return v.i;
        case Kind::Enum:   return r.spec.codec ? nlohmann::json(r.spec.codec->toToken(v.i)) : nlohmann::json(v.i);
        case Kind::Float:  return v.f;
        case Kind::String: return v.s;
        }
        return nullptr;
    }

    template <class S>
    nlohmann::json RowJson(const RowT<S>& r, const S& s)
    {
        return ValueJson(r, ValueOf(r, s));
    }

    // One row from its (present) section; true when the file supplied it.
    // Absent or mistyped: the current value, false.
    template <class S>
    bool ReadRow(const RowT<S>& r, const nlohmann::json& sec, S& s, const char* who)
    {
        const char* key = r.key.c_str();
        switch (r.kind)
        {
        case Kind::Bool:   return TryValue(sec, key, s.*r.mb, who);
        case Kind::Int:
        case Kind::Choice: return TryValue(sec, key, s.*r.mi, who);
        case Kind::Enum:
            if (const TokenCodec* c = r.spec.codec)
            {
                // An absent key parses the codec's absent token, or round-trips
                // the current value through its own token.
                std::string token;
                const bool got = TryValue(sec, key, token, who);
                if (!got)
                    token = c->absentToken ? std::string(c->absentToken) : std::string(c->toToken(s.*r.mi));
                s.*r.mi = c->fromToken(token);
                return got;
            }
            return TryValue(sec, key, s.*r.mi, who);
        case Kind::Float:  return TryValue(sec, key, s.*r.mf, who);
        case Kind::String: return TryValue(sec, key, s.*r.ms, who);
        }
        return false;
    }

    // The row's load sanitize. Floats: NaN cannot come from JSON, but
    // std::clamp passes it straight through, so Clamp replaces it with the
    // fallback first; ClampFinite does the same for an infinity.
    template <class S>
    void SanitizeRow(const RowT<S>& r, S& s)
    {
        const RowSpecT<S>& sp = r.spec;
        switch (sp.san)
        {
        case San::None:
            break;
        case San::Clamp:
            if (r.kind == Kind::Float)
            {
                float& v = s.*r.mf;
                if (!(v == v)) v = static_cast<float>(sp.fallback);
                v = std::clamp(v, sp.lo, sp.hi);
            }
            else
            {
                int& v = s.*r.mi;
                v = std::clamp(v, static_cast<int>(sp.lo), static_cast<int>(sp.hi));
            }
            break;
        case San::ClampFinite:
        {
            float& v = s.*r.mf;
            v = std::isfinite(v) ? std::clamp(v, sp.lo, sp.hi) : static_cast<float>(sp.fallback);
            break;
        }
        case San::Range:
        {
            int& v = s.*r.mi;
            const int lo = r.kind == Kind::Enum ? 0 : static_cast<int>(sp.lo);
            const int hi = r.kind == Kind::Enum ? static_cast<int>(r.labels.size()) - 1 : static_cast<int>(sp.hi);
            if (v < lo || v > hi) v = static_cast<int>(sp.fallback);
            break;
        }
        case San::OneOf:
            if (r.kind == Kind::String)
            {
                std::string& v = s.*r.ms;
                if (std::find(r.labels.begin(), r.labels.end(), v) == r.labels.end())
                    v = sp.fallbackStr ? sp.fallbackStr : "";
            }
            else
            {
                int& v = s.*r.mi;
                if (std::find(r.values.begin(), r.values.end(), v) == r.values.end())
                    v = static_cast<int>(sp.fallback);
            }
            break;
        case San::AtLeast:
        {
            int& v = s.*r.mi;
            if (v < static_cast<int>(sp.lo)) v = static_cast<int>(sp.fallback);
            break;
        }
        }
    }
}
