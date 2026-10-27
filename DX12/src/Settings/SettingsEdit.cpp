#include "Settings/SettingsEdit.h"

#include "Settings/Scalability.h"       // a player's choice: the quality levels' rule
#include "Graphics/IGraphicsDevice.h"   // the surface's present settings

#include <vector>

namespace Settings
{
bool CapturePlayerRow(const Row& r, Resource::EngineSettings& s, CaptureCtx& c)
{
    if (!r.spec.capture)
        return false;
    if (r.spec.phase == Phase::Present)
        c.ps = c.gfx.MainSurface().GetPresentSettings();
    r.spec.capture(s, c);
    return true;
}

bool CaptureDevRow(const DevRow& r, Resource::EngineDevSettings& s, DevCaptureCtx& c)
{
    if (!r.spec.capture)
        return false;
    r.spec.capture(s, c);
    return true;
}

bool EditPlayerRow(const Row& r, const Value& v, std::string_view writer, ApplyCtx& c)
{
    // The owners hold the store's values: note them, record the choice (a
    // level lets go of its group's overrides - Settings/Scalability.h), then
    // push this row and every other one whose value the write moved.
    SettingsStore& st = SettingsStore::Get();
    const auto rows = PlayerRows();
    std::vector<Value> before;
    before.reserve(rows.size());
    for (const Row& q : rows) before.push_back(st.OwnerValue(q));
    Scalability::RecordPlayerChoice(r, v, writer);
    // Each player hook reads only its own field (the Present / Window rows
    // seed the rest of their phase from what is live - ApplyPlayerRow).
    Resource::EngineSettings s;
    Resource::ProjectStoreValues(s);
    bool applied = false;
    for (size_t i = 0; i < rows.size(); ++i)
    {
        if (&rows[i] == &r)                                        applied = ApplyPlayerRow(r, s, c);
        else if (!SameValue(rows[i], before[i], st.OwnerValue(rows[i]))) ApplyPlayerRow(rows[i], s, c);
    }
    return applied;
}

bool EditDevRow(const DevRow& r, const Value& v, std::string_view writer, DevApplyCtx& c)
{
    DevSettingsStore::Get().Write(r, Layer::Runtime, v, writer);
    Resource::EngineDevSettings s;
    PutValue(r, v, s);
    return ApplyDevRow(r, s, c);
}

bool EditPassRow(const PassRow& r, const Value& v, std::string_view writer)
{
    return WritePassSetting(r, Layer::Runtime, &v, writer);
}
}
