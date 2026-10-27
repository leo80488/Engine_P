#include "Settings/LegacyFlags.h"

#include "System/CommandLine.h"

#include <string_view>

namespace Settings
{
std::vector<LegacySpelling> SplitLegacy(const char* spec)
{
    std::vector<LegacySpelling> out;
    if (!spec) return out;
    std::string_view s(spec);
    for (;;)
    {
        const size_t bar = s.find('|');
        const std::string_view one = s.substr(0, bar);
        LegacySpelling sp;
        const size_t eq = one.find('=');
        sp.flag = std::string(one.substr(0, eq));
        if (eq != std::string_view::npos)
        {
            sp.constant = true;
            sp.text     = std::string(one.substr(eq + 1));
        }
        out.push_back(std::move(sp));
        if (bar == std::string_view::npos) break;
        s.remove_prefix(bar + 1);
    }
    return out;
}

const char* LegacyResultName(LegacyResult r)
{
    switch (r)
    {
    case LegacyResult::Absent:  return "absent";
    case LegacyResult::Taken:   return "taken";
    case LegacyResult::Ignored: return "ignored";
    case LegacyResult::Refused: return "refused";
    }
    return "absent";
}

namespace
{
    // "-x" in the pass style: the reader the pass used, exactly, without the mark.
    template <class S>
    LegacyResult ReadPassStyle(const RowT<S>& r, const char* flag, Value& out)
    {
        const Sys::CommandLine& cl = Sys::CommandLine::Get();
        const bool valueOnly = (r.spec.flags & kSettingLegacyValueOnly) != 0;
        out = ValueOf(r, S{});
        switch (r.kind)
        {
        case Kind::Bool:
            out.b = true;   // Has(): given is on, whatever its value
            return LegacyResult::Taken;
        case Kind::Int:
        case Kind::Enum:
        case Kind::Choice:
            if (valueOnly) return cl.PeekIntValue(flag, out.i) ? LegacyResult::Taken : LegacyResult::Ignored;
            out.i = cl.PeekInt(flag, out.i);
            return LegacyResult::Taken;
        case Kind::Float:
            if (valueOnly) return cl.PeekFloatValue(flag, out.f) ? LegacyResult::Taken : LegacyResult::Ignored;
            out.f = cl.PeekFloat(flag, out.f);
            return LegacyResult::Taken;
        case Kind::String:
            out.s = cl.PeekGet(flag, out.s);
            return LegacyResult::Taken;
        }
        return LegacyResult::Ignored;
    }

    // "-x" in the developer style: -set:'s reading of a value. A bare Bool is true. A bare
    // Int / Float, or a number (Int, Float, Bool) whose value is malformed or NaN, is Ignored
    // with `why` the warning (the row keeps its own). A name (String, Enum, Choice) with no
    // value, or any value the row refuses otherwise, is Refused, as -set: refuses it.
    template <class S>
    LegacyResult ReadDeveloperStyle(const RowT<S>& r, const char* flag, Value& out, std::string* why)
    {
        const bool number = r.kind == Kind::Int || r.kind == Kind::Float || r.kind == Kind::Bool;
        const std::string text = Sys::CommandLine::Get().PeekGet(flag);
        if (text.empty())
        {
            if (r.kind == Kind::Bool)
            {
                out.b = true;   // a bare switch
                return LegacyResult::Taken;
            }
            if (!number)
            {
                if (why) *why = std::string("'") + flag + "' needs a value (" + r.path + ")";
                return LegacyResult::Refused;
            }
            if (why) *why = std::string("'") + flag + "' has no value: " + r.path + " keeps its own";
            return LegacyResult::Ignored;
        }
        const Refusal refusal = Coerce(r, RequestFromText(r, text), out);
        if (refusal == Refusal::None) return LegacyResult::Taken;
        if (number && (refusal == Refusal::WrongType || refusal == Refusal::NaN))
        {
            if (why) *why = std::string("'") + flag + "=" + text + "' is not a value " + r.path + " takes (" +
                            RefusalName(refusal) + "): it keeps its own";
            return LegacyResult::Ignored;
        }
        if (why) *why = std::string("'") + flag + "=" + text + "': " + r.path + " refuses the value (" + RefusalName(refusal) + ")";
        return LegacyResult::Refused;
    }
}

template <class S>
LegacyResult ReadLegacy(const RowT<S>& r, LegacyStyle style, Value& out, std::string& writer, std::string* why)
{
    const Sys::CommandLine& cl = Sys::CommandLine::Get();
    const std::vector<LegacySpelling> spellings = SplitLegacy(r.spec.legacyFlag);
    // The developer style marks every spelling given as read (the registry consumes them
    // all at boot); the pass style leaves the marks to the row's getter.
    if (style == LegacyStyle::Developer)
        for (const LegacySpelling& sp : spellings)
            if (cl.Peek(sp.flag.c_str())) cl.MarkRead(sp.flag.c_str());
    for (const LegacySpelling& sp : spellings)
    {
        const char* flag = sp.flag.c_str();
        if (!cl.Peek(flag)) continue;
        // The first spelling given decides.
        writer = sp.flag;
        if (sp.constant)
        {
            const Refusal refusal = Coerce(r, RequestFromText(r, sp.text), out);
            if (refusal == Refusal::None) return LegacyResult::Taken;
            if (why) *why = "'" + sp.flag + "=" + sp.text + "': " + r.path + " refuses the value (" + RefusalName(refusal) + ")";
            return LegacyResult::Refused;
        }
        return style == LegacyStyle::Pass ? ReadPassStyle(r, flag, out) : ReadDeveloperStyle(r, flag, out, why);
    }
    return LegacyResult::Absent;
}

template <class S>
std::string LegacySpecError(const RowT<S>& r)
{
    if (r.spec.legacyRead && r.spec.legacyFlag) return "both .legacyFlag and .legacyRead";
    if (!r.spec.legacyFlag) return {};
    for (const LegacySpelling& sp : SplitLegacy(r.spec.legacyFlag))
    {
        if (sp.flag.size() < 2 || sp.flag[0] != '-')
            return "'" + sp.flag + "' is not a flag spelled with its '-'";
        if (!sp.constant) continue;
        Value v;
        const Refusal refusal = Coerce(r, RequestFromText(r, sp.text), v);
        if (refusal != Refusal::None)
            return "'" + sp.flag + "=" + sp.text + "': the row refuses the value (" + RefusalName(refusal) + ")";
    }
    return {};
}

#define LEGACY_FLAGS_INSTANTIATE(S)                                                                       \
    template LegacyResult ReadLegacy(const RowT<S>&, LegacyStyle, Value&, std::string&, std::string*);   \
    template std::string  LegacySpecError(const RowT<S>&);
LEGACY_FLAGS_INSTANTIATE(Resource::EngineSettings)
LEGACY_FLAGS_INSTANTIATE(Resource::EngineDevSettings)
LEGACY_FLAGS_INSTANTIATE(Resource::PassSettings)
#undef LEGACY_FLAGS_INSTANTIATE
}
