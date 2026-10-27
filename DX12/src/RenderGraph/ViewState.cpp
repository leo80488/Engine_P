#include "RenderGraph/ViewState.h"

#include "Graphics/IGraphicsDevice.h"
#include "System/Check.h"
#include "System/Log.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace HistoryReset
{
void Names(uint32_t bits, char* out, size_t size)
{
    if (!out || size == 0) return;
    out[0] = '\0';
    static const struct { uint32_t bit; const char* name; } kNames[] = {
        { HardCut, "HardCut" }, { CameraCut, "CameraCut" }, { Resize, "Resize" }, { WorldClear, "WorldClear" },
        { PathChange, "PathChange" },
    };
    size_t used = 0;
    for (const auto& n : kNames)
    {
        if (!(bits & n.bit)) continue;
        const int w = std::snprintf(out + used, size - used, "%s%s", used ? "," : "", n.name);
        if (w < 0 || static_cast<size_t>(w) >= size - used) break;
        used += static_cast<size_t>(w);
    }
    if (used == 0)
        std::snprintf(out, size, "none");
}
}

ViewState::~ViewState()
{
    // The Renderer releases the GPU resources at shutdown (ReleaseHistories);
    // one left here would leak its textures past the device.
    ENGINE_CHECK(m_histories.empty() && m_data.empty(), "ViewState '%s': %zu histories and %zu view data "
                 "still held at destruction (ReleaseHistories was not called)", m_name, m_histories.size(),
                 m_data.size());
}

void ViewState::Add(const void* owner, std::type_index type, std::unique_ptr<ViewHistory> history)
{
    char reasons[64];
    HistoryReset::Names(history->ResetsOn(), reasons, sizeof(reasons));
    LOG_INFO("[VIEW_STATE] view=%s history=%s resetsOn=%s histories=%zu", m_name, history->Name(), reasons,
             m_histories.size() + 1);
    m_histories.push_back(Entry{ owner, type, std::move(history) });
}

void ViewState::Invalidate(uint32_t reasons, bool quiet)
{
    Apply(reasons, quiet, -1);
}

void ViewState::Apply(uint32_t reasons, bool quiet, int64_t frame)
{
    std::string names;
    for (Entry& e : m_histories)
    {
        const uint32_t mine = reasons & e.history->ResetsOn();
        if (!mine) continue;
        e.history->Invalidate(mine);
        if (quiet) continue;
        if (!names.empty()) names += ',';
        names += e.history->Name();
    }
    // The view's own state: the Hi-Z pyramid shows the old world's depth -
    // the next cull must not test the new world's draws against it.
    if (reasons & HistoryReset::WorldClear)
        hiz.framesValid = 0;
    if (quiet) return;
    char why[64];
    HistoryReset::Names(reasons, why, sizeof(why));
    if (frame >= 0)
        LOG_INFO("[VIEW_STATE] invalidate view=%s reasons=%s frame=%lld histories=%s", m_name, why,
                 static_cast<long long>(frame), names.empty() ? "-" : names.c_str());
    else
        LOG_INFO("[VIEW_STATE] invalidate view=%s reasons=%s histories=%s", m_name, why,
                 names.empty() ? "-" : names.c_str());
}

void ViewState::BeginFrame(uint64_t frame, uint32_t reasons, uint32_t loud)
{
    m_frameBreaks = reasons;
    bool cutContinues = false;
    if (reasons & HistoryReset::CameraCut)
    {
        // A camera that keeps cutting (a script calling HardCutTo every frame)
        // logs once per run, not once per frame.
        cutContinues = m_cutRun > 0 && frame == m_cutRunLast + 1;
        if (!cutContinues) EndCutRun();
        ++m_cutRun;
        m_cutRunLast = frame;
    }
    else
        EndCutRun();
    if (!reasons)
        return;
    // Quiet: nothing but quiet reasons (the Sequencer's scrubbing) and, at
    // most, a cut that continues a run.
    const bool cutLoud = (reasons & HistoryReset::CameraCut) && !cutContinues;
    const bool quiet = !cutLoud && (loud & ~static_cast<uint32_t>(HistoryReset::CameraCut)) == 0;
    Apply(reasons, quiet, static_cast<int64_t>(frame));
}

void ViewState::EndCutRun()
{
    if (m_cutRun > 1)
        LOG_INFO("[VIEW_STATE] cut run view=%s frames=%u last=%llu", m_name, m_cutRun,
                 static_cast<unsigned long long>(m_cutRunLast));
    m_cutRun = 0;
}

void ViewState::ReleaseHistories(IGraphicsDevice& gfx)
{
    for (Entry& e : m_histories)
        e.history->Release(gfx);
    m_histories.clear();
    for (DataEntry& e : m_data)
        e.data->Release(gfx);
    m_data.clear();
}
