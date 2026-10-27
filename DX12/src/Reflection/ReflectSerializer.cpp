#include "Reflection/ReflectSerializer.h"

#include "Reflection/Property.h"
#include "Resource/ComponentKV.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <sstream>

namespace Reflect
{
namespace
{
    void Number(std::ostringstream& out, float v, NumberStyle style)
    {
        if (style == NumberStyle::Stream)
        {
            out << v;
            return;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.4f", v);
        out << buf;
    }

    // A field's keys: one, or one per component of a split key.
    std::vector<std::string> KeysOf(const FieldDescriptor& f)
    {
        std::vector<std::string> out;
        std::stringstream in(KeyOf(f));
        std::string part;
        while (std::getline(in, part, ',')) out.push_back(part);
        return out;
    }

    // A single-step path to a top-level field (and one component of it, or -1).
    PropertyPath FieldPath(const TypeDescriptor& desc, const FieldDescriptor& f, int component)
    {
        PropertyPath p;
        p.type = &desc;
        p.steps.push_back({ &f, component, -1 });
        p.text = f.name;
        return p;
    }

    bool Saved(const FieldDescriptor& f)
    {
        return !IsLayout(f.kind) && !Has(f, FieldFlag::Runtime) && f.kind != FieldKind::Custom &&
               f.kind != FieldKind::Vector && f.kind != FieldKind::Struct;
    }

    // @p kv's value for @p key, or for one of the field's legacy keys.
    const std::string* Lookup(const KVMap& kv, const std::string& key, const FieldDescriptor& f)
    {
        auto it = kv.find(key);
        if (it != kv.end()) return &it->second;
        if (!f.legacyKeys) return nullptr;
        std::stringstream in(f.legacyKeys);
        std::string legacy;
        while (std::getline(in, legacy, '|'))
            if ((it = kv.find(legacy)) != kv.end()) return &it->second;
        return nullptr;
    }
}

std::string SerializeLine(const char* tag, const TypeDescriptor& desc, const void* obj, NumberStyle numbers)
{
    std::ostringstream out;
    out << "  " << tag << ':';
    for (const FieldDescriptor& f : desc.fields)
    {
        if (!Saved(f)) continue;
        const std::vector<std::string> keys = KeysOf(f);
        const PropertyValue v = Get(obj, FieldPath(desc, f, -1));
        if (const FloatVec* fv = std::get_if<FloatVec>(&v))
        {
            if (keys.size() > 1)
            {
                for (int i = 0; i < fv->n && i < static_cast<int>(keys.size()); ++i)
                {
                    out << ' ' << keys[static_cast<std::size_t>(i)] << '=';
                    Number(out, fv->v[i], numbers);
                }
                continue;
            }
            out << ' ' << keys[0] << '=';
            for (int i = 0; i < fv->n; ++i)
            {
                if (i) out << '_';
                Number(out, fv->v[i], numbers);
            }
            continue;
        }
        out << ' ' << keys[0] << '=';
        if (const double* d = std::get_if<double>(&v)) Number(out, static_cast<float>(*d), numbers);
        else if (const int64_t* i = std::get_if<int64_t>(&v)) out << *i;
        else if (const bool* b = std::get_if<bool>(&v)) out << (*b ? 1 : 0);
        else if (const std::string* s = std::get_if<std::string>(&v)) out << ComponentKV::PercentEncode(*s);
    }
    out << '\n';
    return out.str();
}

bool DeserializeKV(const TypeDescriptor& desc, void* obj, const KVMap& kv, std::string& error)
{
    bool ok = true;
    for (const FieldDescriptor& f : desc.fields)
    {
        if (!Saved(f)) continue;
        const std::vector<std::string> keys = KeysOf(f);
        for (std::size_t part = 0; part < keys.size(); ++part)
        {
            const std::string* text = Lookup(kv, keys[part], f);
            if (!text) continue;
            const PropertyPath path = FieldPath(desc, f, keys.size() > 1 ? static_cast<int>(part) : -1);
            const std::string value = (f.kind == FieldKind::String || f.kind == FieldKind::AssetRef)
                                          ? ComponentKV::PercentDecode(*text) : *text;
            PropertyValue v;
            if (!FromText(path, value, v) || !WriteProperty(obj, path, v, WriteOrigin::Undo).ok)
            {
                // A malformed value keeps the field's default, as the hand-written readers do.
                if (ok) error = keys[part] + "=" + *text;
                ok = false;
            }
        }
    }
    return ok;
}

bool WriteKeys(const TypeDescriptor& desc, void* obj, const std::vector<std::pair<std::string, std::string>>& pairs,
               WriteOrigin origin, std::string& error)
{
    if (!desc.construct || !desc.destroy || !desc.copy)
    {
        error = std::string(desc.name) + " cannot be copied, so a write to it cannot be all or nothing";
        return false;
    }
    // The pairs go onto a copy, which replaces the object only when every one was accepted.
    void* const scratch = ::operator new(desc.size, std::align_val_t(desc.align));
    desc.construct(scratch);
    struct Guard
    {
        const TypeDescriptor& desc;
        void*                 p;
        ~Guard()
        {
            desc.destroy(p);
            ::operator delete(p, std::align_val_t(desc.align));
        }
    } guard{ desc, scratch };
    void* tmp = scratch;
    desc.copy(tmp, obj);

    for (const auto& [key, text] : pairs)
    {
        // The field the key names: a split key's part, or the key (or C++ name) of a whole field.
        const FieldDescriptor* field = nullptr;
        int component = -1;
        for (const FieldDescriptor& f : desc.fields)
        {
            if (IsLayout(f.kind)) continue;
            const std::vector<std::string> keys = KeysOf(f);
            for (std::size_t part = 0; part < keys.size() && !field; ++part)
                if (keys[part] == key)
                {
                    field = &f;
                    component = keys.size() > 1 ? static_cast<int>(part) : -1;
                }
            if (!field && key == f.name) field = &f;
            if (field) break;
        }
        if (!field)
        {
            error = "'" + key + "' names no field of " + desc.name;
            return false;
        }
        const PropertyPath path = FieldPath(desc, *field, component);
        const std::string value = (field->kind == FieldKind::String || field->kind == FieldKind::AssetRef)
                                      ? ComponentKV::PercentDecode(text) : text;
        PropertyValue v;
        if (!FromText(path, value, v))
        {
            error = "'" + key + "': '" + text + "' is not a value of that field";
            return false;
        }
        const WriteResult r = WriteProperty(tmp, path, v, origin);
        if (!r.ok)
        {
            error = r.error;
            return false;
        }
    }
    desc.copy(obj, tmp);
    return true;
}
}
