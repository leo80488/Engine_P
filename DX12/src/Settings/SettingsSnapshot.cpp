#include "Settings/SettingsSnapshot.h"

#include "Settings/SettingsStore.h"

#include <algorithm>

namespace Settings
{
bool FillSnapshot(const SettingsStore& store, Snapshot& out)
{
    const auto rows = store.Rows();
    if (out.revision == store.Revision() && out.values.size() == rows.size())
        return false;
    out.values.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const Row&   r = rows[i];
        const Value& v = store.Effective(r);
        SnapValue    s{};
        switch (r.kind)
        {
        case Kind::Bool:   s.i = v.b ? 1 : 0; break;
        case Kind::Int:
        case Kind::Enum:
        case Kind::Choice: s.i = v.i; break;
        case Kind::Float:  s.f = v.f; break;
        case Kind::String:
        {
            const auto it = std::find(r.labels.begin(), r.labels.end(), v.s);
            s.i = it == r.labels.end() ? -1 : static_cast<int32_t>(it - r.labels.begin());
            break;
        }
        }
        out.values[i] = s;
    }
    out.revision = store.Revision();
    ++out.fills;
    return true;
}
}
