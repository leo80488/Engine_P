#include "Reflection/Property.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace Reflect
{
namespace
{
    using K = FieldKind;

    // ==== Kinds ====

    int FloatCount(FieldKind k)
    {
        switch (k)
        {
        case K::F32x2: return 2;
        case K::F32x3: return 3;
        case K::F32x4: case K::Quat: return 4;
        default: return 0;
        }
    }

    bool IsInteger(FieldKind k)
    {
        return k == K::I32 || k == K::U32 || k == K::U8 || k == K::U16 || k == K::I64 || k == K::EntityRef ||
               k == K::Enum || k == K::Flags;
    }

    // The storage range of an integer kind.
    void IntegerRange(const FieldDescriptor& f, double& lo, double& hi)
    {
        switch (f.kind)
        {
        case K::I32: lo = (std::numeric_limits<int32_t>::min)(); hi = (std::numeric_limits<int32_t>::max)(); return;
        case K::U32: case K::EntityRef: lo = 0; hi = (std::numeric_limits<uint32_t>::max)(); return;
        case K::U8: lo = 0; hi = 255; return;
        case K::U16: lo = 0; hi = 65535; return;
        case K::I64: lo = -9.2e18; hi = 9.2e18; return;
        case K::Enum: case K::Flags:
            lo = f.enumSize == 4 && f.kind == K::Enum ? (std::numeric_limits<int32_t>::min)() : 0;
            hi = f.enumSize == 1 ? 255 : f.enumSize == 2 ? 65535 : (std::numeric_limits<uint32_t>::max)();
            return;
        default: lo = hi = 0; return;
        }
    }

    // The keys of a field: one, or one per component of a split key.
    std::vector<std::string_view> KeyParts(const FieldDescriptor& f)
    {
        std::vector<std::string_view> out;
        std::string_view keys = KeyOf(f);
        while (true)
        {
            const std::size_t comma = keys.find(',');
            out.push_back(keys.substr(0, comma));
            if (comma == std::string_view::npos) break;
            keys.remove_prefix(comma + 1);
        }
        return out;
    }

    // The path text that names field @p f alone: its key, or its C++ name when the key is split.
    std::string FieldPathText(const FieldDescriptor& f)
    {
        return KeyParts(f).size() > 1 ? std::string(f.name) : std::string(KeyOf(f));
    }

    // The live element count of a vector / fixed array whose field sits at @p fieldPtr inside @p parent.
    std::size_t ElementCount(const FieldDescriptor& f, const void* parent, const void* fieldPtr)
    {
        if (f.vecSize) return f.vecSize(fieldPtr);
        const void* countPtr = static_cast<const unsigned char*>(parent) + f.countOffset;
        const long long count = f.countIsInt ? *static_cast<const int*>(countPtr) : *static_cast<const uint32_t*>(countPtr);
        if (count < 0) return 0;
        return static_cast<std::size_t>(count) < f.arrayCapacity ? static_cast<std::size_t>(count) : f.arrayCapacity;
    }

    int64_t ReadInteger(const FieldDescriptor& f, const void* p)
    {
        switch (f.kind)
        {
        case K::I32: return *static_cast<const int32_t*>(p);
        case K::U32: case K::EntityRef: return *static_cast<const uint32_t*>(p);
        case K::U8: return *static_cast<const uint8_t*>(p);
        case K::U16: return *static_cast<const uint16_t*>(p);
        case K::I64: return *static_cast<const int64_t*>(p);
        case K::Enum: case K::Flags:
            switch (f.enumSize)
            {
            case 1: return *static_cast<const uint8_t*>(p);
            case 2: return *static_cast<const uint16_t*>(p);
            case 4: return f.kind == K::Enum ? *static_cast<const int32_t*>(p) : *static_cast<const uint32_t*>(p);
            default: return 0;
            }
        default: return 0;
        }
    }

    void WriteInteger(const FieldDescriptor& f, void* p, int64_t v)
    {
        switch (f.kind)
        {
        case K::I32: *static_cast<int32_t*>(p) = static_cast<int32_t>(v); break;
        case K::U32: case K::EntityRef: *static_cast<uint32_t*>(p) = static_cast<uint32_t>(v); break;
        case K::U8: *static_cast<uint8_t*>(p) = static_cast<uint8_t>(v); break;
        case K::U16: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(v); break;
        case K::I64: *static_cast<int64_t*>(p) = v; break;
        case K::Enum: case K::Flags:
            switch (f.enumSize)
            {
            case 1: *static_cast<uint8_t*>(p) = static_cast<uint8_t>(v); break;
            case 2: *static_cast<uint16_t*>(p) = static_cast<uint16_t>(v); break;
            case 4: *static_cast<uint32_t*>(p) = static_cast<uint32_t>(v); break;
            default: break;
            }
            break;
        default: break;
        }
    }

    // A value as numbers (a bool is 0 / 1, a vector its components); false for a string or nothing.
    bool AsNumbers(const PropertyValue& v, double* out, int& n)
    {
        if (const bool* b = std::get_if<bool>(&v)) { out[0] = *b ? 1.0 : 0.0; n = 1; return true; }
        if (const int64_t* i = std::get_if<int64_t>(&v)) { out[0] = static_cast<double>(*i); n = 1; return true; }
        if (const double* d = std::get_if<double>(&v)) { out[0] = *d; n = 1; return true; }
        if (const FloatVec* fv = std::get_if<FloatVec>(&v))
        {
            n = fv->n;
            for (int i = 0; i < fv->n; ++i) out[i] = fv->v[i];
            return n > 0;
        }
        return false;
    }

    // ==== Hooks ====
    std::vector<std::pair<const TypeDescriptor*, PropertyChangedFn>>& Hooks()
    {
        static std::vector<std::pair<const TypeDescriptor*, PropertyChangedFn>> s_hooks;
        return s_hooks;
    }
}

// ==== Paths ====

bool ParsePath(const TypeDescriptor& type, std::string_view text, PropertyPath& out, std::string& error)
{
    out = {};
    out.type = &type;
    out.text = std::string(text);
    const TypeDescriptor* scope = &type;
    std::string_view rest = text;
    while (!rest.empty())
    {
        const std::size_t dot = rest.find('.');
        std::string_view token = rest.substr(0, dot);
        rest = dot == std::string_view::npos ? std::string_view() : rest.substr(dot + 1);
        if (dot != std::string_view::npos && rest.empty())
        {
            error = "the path ends in '.'";
            return false;
        }

        // A member reflected per element ("localPoints[1]") is named with its brackets.
        const FieldDescriptor* whole = nullptr;
        if (scope)
            for (const FieldDescriptor& f : scope->fields)
                if (!IsLayout(f.kind) && f.name && token == f.name && token.find('[') != std::string_view::npos)
                    whole = &f;
        if (whole)
        {
            out.steps.push_back({ whole, -1, -1 });
            scope = whole->kind == K::Struct ? whole->elementDesc : nullptr;
            continue;
        }

        int index = -1;
        const std::size_t bracket = token.find('[');
        if (bracket != std::string_view::npos)
        {
            const std::size_t close = token.find(']', bracket);
            if (close != token.size() - 1 || close == bracket + 1)
            {
                error = "a malformed index in '" + std::string(token) + "'";
                return false;
            }
            index = 0;
            for (char c : token.substr(bracket + 1, close - bracket - 1))
            {
                if (c < '0' || c > '9' || index > 100000000)
                {
                    error = "a malformed index in '" + std::string(token) + "'";
                    return false;
                }
                index = index * 10 + (c - '0');
            }
            token = token.substr(0, bracket);
        }
        if (token.empty())
        {
            error = "an empty step in '" + out.text + "'";
            return false;
        }

        // A component of the float vector the previous step named.
        if (!out.steps.empty() && index < 0)
        {
            PathStep& last = out.steps.back();
            const int floats = FloatCount(last.field->kind);
            if (floats > 0 && last.component < 0 && token.size() == 1)
            {
                const char* kAxes = "xyzw";
                const char* at = std::strchr(kAxes, token[0]);
                if (at && at - kAxes < floats)
                {
                    last.component = static_cast<int>(at - kAxes);
                    continue;
                }
            }
        }

        if (!scope)
        {
            error = "'" + std::string(token) + "' follows a field that has no fields";
            return false;
        }
        PathStep step;
        for (const FieldDescriptor& f : scope->fields)
        {
            if (IsLayout(f.kind)) continue;
            const std::vector<std::string_view> parts = KeyParts(f);
            if (parts.size() > 1)
            {
                for (std::size_t i = 0; i < parts.size(); ++i)
                    if (parts[i] == token) { step.field = &f; step.component = static_cast<int>(i); }
            }
            else if (parts[0] == token)
                step.field = &f;
            if (!step.field && f.name && token == f.name) step.field = &f;
            if (step.field) break;
        }
        if (!step.field)
        {
            error = "no field '" + std::string(token) + "' in " + std::string(scope->name ? scope->name : "?");
            return false;
        }
        if (index >= 0)
        {
            if (step.field->kind != K::Vector)
            {
                error = "'" + std::string(token) + "' is not a vector or an array";
                return false;
            }
            step.index = index;
        }
        out.steps.push_back(step);
        scope = (step.field->kind == K::Struct || (step.field->kind == K::Vector && index >= 0)) ? step.field->elementDesc
                                                                                                : nullptr;
    }
    if (out.steps.empty())
    {
        error = "an empty path";
        return false;
    }
    return true;
}

const void* Address(const void* obj, const PropertyPath& path)
{
    const unsigned char* parent = static_cast<const unsigned char*>(obj);
    const unsigned char* p = parent;
    for (const PathStep& step : path.steps)
    {
        p = parent + step.field->offset;
        if (step.index >= 0)
        {
            if (static_cast<std::size_t>(step.index) >= ElementCount(*step.field, parent, p)) return nullptr;
            p = static_cast<const unsigned char*>(step.field->vecData(const_cast<unsigned char*>(p),
                                                                     static_cast<std::size_t>(step.index)));
        }
        parent = p;
    }
    return p;
}

void* Address(void* obj, const PropertyPath& path)
{
    return const_cast<void*>(Address(static_cast<const void*>(obj), path));
}

// ==== Values ====

PropertyValue Get(const void* obj, const PropertyPath& path)
{
    const FieldDescriptor* f = path.Leaf();
    const void* p = f ? Address(obj, path) : nullptr;
    if (!p) return {};
    const PathStep& leaf = path.steps.back();
    if (leaf.index >= 0) return {};   // a whole element: a struct, no single value
    if (const int floats = FloatCount(f->kind))
    {
        const float* v = static_cast<const float*>(p);
        if (leaf.component >= 0) return static_cast<double>(v[leaf.component]);
        FloatVec out;
        out.n = floats;
        for (int i = 0; i < floats; ++i) out.v[i] = v[i];
        return out;
    }
    switch (f->kind)
    {
    case K::F32: return static_cast<double>(*static_cast<const float*>(p));
    case K::F64: return *static_cast<const double*>(p);
    case K::Bool: return *static_cast<const bool*>(p);
    case K::String: case K::AssetRef: return *static_cast<const std::string*>(p);
    default: break;
    }
    if (IsInteger(f->kind)) return ReadInteger(*f, p);
    return {};
}

bool Equal(const PropertyValue& a, const PropertyValue& b)
{
    if (a.index() != b.index()) return false;
    if (const bool* x = std::get_if<bool>(&a)) return *x == std::get<bool>(b);
    if (const int64_t* x = std::get_if<int64_t>(&a)) return *x == std::get<int64_t>(b);
    if (const double* x = std::get_if<double>(&a)) return *x == std::get<double>(b);
    if (const std::string* x = std::get_if<std::string>(&a)) return *x == std::get<std::string>(b);
    if (const FloatVec* x = std::get_if<FloatVec>(&a))
    {
        const FloatVec& y = std::get<FloatVec>(b);
        if (x->n != y.n) return false;
        for (int i = 0; i < x->n; ++i)
            if (x->v[i] != y.v[i]) return false;
        return true;
    }
    return true;   // both empty
}

std::string ToText(const PropertyValue& v)
{
    char buf[128];
    if (const bool* b = std::get_if<bool>(&v)) return *b ? "1" : "0";
    if (const int64_t* i = std::get_if<int64_t>(&v))
    {
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*i));
        return buf;
    }
    if (const double* d = std::get_if<double>(&v))
    {
        std::snprintf(buf, sizeof(buf), "%.9g", *d);
        return buf;
    }
    if (const FloatVec* fv = std::get_if<FloatVec>(&v))
    {
        std::string out;
        for (int i = 0; i < fv->n; ++i)
        {
            std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(fv->v[i]));
            if (i) out += '_';
            out += buf;
        }
        return out;
    }
    if (const std::string* s = std::get_if<std::string>(&v)) return *s;
    return {};
}

bool FromText(const PropertyPath& path, std::string_view text, PropertyValue& out)
{
    const FieldDescriptor* f = path.Leaf();
    if (!f || path.steps.back().index >= 0) return false;
    const std::string s(text);
    auto number = [](const std::string& t, double& v) {
        char* end = nullptr;
        v = std::strtod(t.c_str(), &end);
        return end != t.c_str() && *end == '\0';
    };
    const int floats = FloatCount(f->kind);
    if (floats > 0 && path.steps.back().component < 0)
    {
        FloatVec fv;
        std::size_t start = 0;
        while (fv.n < 4)
        {
            const std::size_t sep = s.find('_', start);
            double v = 0;
            if (!number(s.substr(start, sep == std::string::npos ? std::string::npos : sep - start), v)) return false;
            fv.v[fv.n++] = static_cast<float>(v);
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        if (fv.n != floats) return false;
        out = fv;
        return true;
    }
    switch (f->kind)
    {
    case K::Bool:
        if (s == "1" || s == "true") { out = true; return true; }
        if (s == "0" || s == "false") { out = false; return true; }
        return false;
    case K::String: case K::AssetRef:
        out = s;
        return true;
    default: break;
    }
    double v = 0;
    if (!number(s, v)) return false;
    if (IsInteger(f->kind))
    {
        if (v != std::floor(v)) return false;
        out = static_cast<int64_t>(v);
    }
    else
        out = v;
    return true;
}

bool Writable(const PropertyPath& path)
{
    const FieldDescriptor* f = path.Leaf();
    if (!f || path.steps.back().index >= 0) return false;
    return f->kind != K::Custom && f->kind != K::Vector && f->kind != K::Struct && !IsLayout(f->kind);
}

std::vector<std::string> Diff(const TypeDescriptor& type, const void* a, const void* b, bool intoElements)
{
    std::vector<std::string> out;
    for (const FieldDescriptor& f : type.fields)
    {
        if (IsLayout(f.kind)) continue;
        const auto* pa = static_cast<const unsigned char*>(a) + f.offset;
        const auto* pb = static_cast<const unsigned char*>(b) + f.offset;
        const std::string key = FieldPathText(f);
        if (f.kind == K::Vector)
        {
            const std::size_t na = ElementCount(f, a, pa), nb = ElementCount(f, b, pb);
            if (na != nb || !f.elementDesc)
            {
                out.push_back(key);
                continue;
            }
            bool differs = false;
            for (std::size_t i = 0; i < na; ++i)
            {
                const std::vector<std::string> inner = Diff(*f.elementDesc, f.vecData(const_cast<unsigned char*>(pa), i),
                                                            f.vecData(const_cast<unsigned char*>(pb), i), intoElements);
                if (inner.empty()) continue;
                differs = true;
                if (!intoElements) break;
                for (const std::string& sub : inner) out.push_back(key + "[" + std::to_string(i) + "]." + sub);
            }
            if (differs && !intoElements) out.push_back(key);
            continue;
        }
        if (f.kind == K::Struct)
        {
            if (!f.elementDesc) continue;
            for (const std::string& sub : Diff(*f.elementDesc, pa, pb, intoElements)) out.push_back(key + "." + sub);
            continue;
        }
        if (f.kind == K::Custom)
        {
            if (std::memcmp(pa, pb, f.size) != 0) out.push_back(key);
            continue;
        }
        PropertyPath path;
        path.type = &type;
        path.steps.push_back({ &f, -1, -1 });
        if (!Equal(Get(a, path), Get(b, path))) out.push_back(key);
    }
    return out;
}

// ==== Writing ====

WriteResult WriteProperty(void* obj, const PropertyPath& path, const PropertyValue& value, WriteOrigin origin)
{
    WriteResult r;
    const FieldDescriptor* f = path.Leaf();
    void* p = f ? Address(obj, path) : nullptr;
    if (!path.Valid() || !f) { r.error = "an unresolved path"; return r; }
    if (!p) { r.error = "'" + path.text + "': the element index is out of range"; return r; }
    const PathStep& leaf = path.steps.back();
    if (leaf.index >= 0) { r.error = "'" + path.text + "' names a whole element; write its fields"; return r; }
    if (Has(*f, FieldFlag::ReadOnly) && origin != WriteOrigin::Undo)
    {
        r.error = "'" + path.text + "' is read-only";
        return r;
    }
    if (origin == WriteOrigin::Lua && !Has(*f, FieldFlag::LuaWrite))
    {
        r.error = "'" + path.text + "' is not writable from Lua";
        return r;
    }
    r.before = Get(obj, path);

    const bool clampPolicy = origin == WriteOrigin::Inspector || Has(*f, FieldFlag::ClampHard);
    const bool enforce = origin != WriteOrigin::Undo;
    // Holds @p v inside [lo, hi]: clamps it under the clamp policy, else refuses.
    auto fit = [&](double& v, double lo, double hi, const char* what) {
        if (!enforce || (v >= lo && v <= hi)) return true;
        if (!clampPolicy)
        {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "'%s': %.9g is outside the %s [%.9g, %.9g]", path.text.c_str(), v, what, lo, hi);
            r.error = buf;
            return false;
        }
        v = v < lo ? lo : hi;
        r.clamped = true;
        return true;
    };

    const int floats = FloatCount(f->kind);
    if (f->kind == K::String || f->kind == K::AssetRef)
    {
        const std::string* s = std::get_if<std::string>(&value);
        if (!s) { r.error = "'" + path.text + "' takes a string"; return r; }
        *static_cast<std::string*>(p) = *s;
    }
    else if (f->kind == K::Bool)
    {
        double v[4];
        int n = 0;
        if (!AsNumbers(value, v, n) || n != 1) { r.error = "'" + path.text + "' takes a boolean"; return r; }
        *static_cast<bool*>(p) = v[0] != 0.0;
    }
    else if (floats > 0 || f->kind == K::F32 || f->kind == K::F64)
    {
        double v[4];
        int n = 0;
        const int want = (floats > 0 && leaf.component < 0) ? floats : 1;
        if (!AsNumbers(value, v, n) || n != want)
        {
            r.error = "'" + path.text + "' takes " + (want == 1 ? std::string("a number") : std::to_string(want) + " numbers");
            return r;
        }
        for (int i = 0; i < n; ++i)
        {
            if (!std::isfinite(v[i]) && enforce) { r.error = "'" + path.text + "': not a finite number"; return r; }
            if (f->hasHard && !fit(v[i], f->hardMin, f->hardMax, "hard range")) return r;
        }
        if (f->kind == K::F64) *static_cast<double*>(p) = v[0];
        else if (f->kind == K::F32) *static_cast<float*>(p) = static_cast<float>(v[0]);
        else if (leaf.component >= 0) static_cast<float*>(p)[leaf.component] = static_cast<float>(v[0]);
        else
            for (int i = 0; i < n; ++i) static_cast<float*>(p)[i] = static_cast<float>(v[i]);
    }
    else if (IsInteger(f->kind))
    {
        double v[4];
        int n = 0;
        if (!AsNumbers(value, v, n) || n != 1 || v[0] != std::floor(v[0]))
        {
            r.error = "'" + path.text + "' takes an integer";
            return r;
        }
        double lo, hi;
        IntegerRange(*f, lo, hi);
        if (!fit(v[0], lo, hi, "storage range")) return r;
        if (f->hasHard && !fit(v[0], f->hardMin, f->hardMax, "hard range")) return r;
        const int64_t iv = static_cast<int64_t>(v[0]);
        if (f->kind == K::Enum && enforce && f->enumOpts)
        {
            bool known = false;
            for (std::size_t i = 0; i < f->enumCount; ++i) known |= f->enumOpts[i].value == iv;
            if (!known) { r.error = "'" + path.text + "': " + std::to_string(iv) + " is not one of its values"; return r; }
        }
        WriteInteger(*f, p, iv);
    }
    else
    {
        r.error = "'" + path.text + "' has no value to write (a custom, vector or struct field)";
        return r;
    }

    r.written = Get(obj, path);
    r.ok = true;
    for (const auto& [type, fn] : Hooks())
        if (type == path.type) fn(obj, path);
    return r;
}

WriteResult ResetToDefault(void* obj, const PropertyPath& path, WriteOrigin origin)
{
    const void* defaults = path.type ? DefaultInstance(*path.type) : nullptr;
    if (!defaults)
    {
        WriteResult r;
        r.error = "the type has no default instance";
        return r;
    }
    return WriteProperty(obj, path, Get(defaults, path), origin);
}

void AddPropertyChangedHook(const TypeDescriptor& type, PropertyChangedFn fn)
{
    Hooks().emplace_back(&type, fn);
}
}
