#include "Renderer/PassSet.h"

#include "RenderGraph/ViewState.h"
#include "System/Log.h"

#include <string>

void PassSet::SetPrepareOrder(RG::PrepareStage stage, std::initializer_list<Named> passes)
{
    std::vector<Named>& list = m_prepare[static_cast<size_t>(stage)];
    list.clear();
    for (const Named& n : passes)
        if (n.pass) list.push_back(n);
}

void PassSet::LogPrepareOrder() const
{
    for (size_t i = 0; i < m_prepare.size(); ++i)
    {
        std::string names;
        for (const Named& n : m_prepare[i])
        {
            if (!names.empty()) names += ' ';
            names += n.name ? n.name : "?";
        }
        LOG_INFO("[PASSES] prepare %s %zu: %s", RG::PrepareStageName(static_cast<RG::PrepareStage>(i)),
                 m_prepare[i].size(), names.empty() ? "-" : names.c_str());
    }
}

void PassSet::SetServiceOrder(RG::ServiceStage stage, std::initializer_list<Named> passes)
{
    std::vector<Named>& list = m_service[static_cast<size_t>(stage)];
    list.clear();
    for (const Named& n : passes)
        if (n.pass) list.push_back(n);
}

void PassSet::LogServiceOrder() const
{
    for (size_t i = 0; i < m_service.size(); ++i)
    {
        std::string names;
        for (const Named& n : m_service[i])
        {
            if (!names.empty()) names += ' ';
            names += n.name ? n.name : "?";
        }
        LOG_INFO("[PASSES] service %s %zu: %s", RG::ServiceStageName(static_cast<RG::ServiceStage>(i)),
                 m_service[i].size(), names.empty() ? "-" : names.c_str());
    }
}

void PassSet::AddView(const char* name, std::function<RG::PassLifecycle*()> pass)
{
    ENGINE_CHECK(m_view != nullptr, "PassSet: '%s' keeps a history in the view but registered before SetView",
                 name ? name : "?");
    if (m_view)
        if (RG::PassLifecycle* p = pass())
            p->BindView(*m_view);
    m_views.push_back(ViewPass{ name, std::move(pass) });
}

void PassSet::BindView(ViewState& view)
{
    if (m_bound == &view) return;
    m_bound = &view;
    for (const ViewPass& v : m_views)
        if (RG::PassLifecycle* p = v.pass())
            p->BindView(view);
}

void PassSet::LogViews() const
{
    std::string names;
    size_t bound = 0;
    for (const ViewPass& v : m_views)
    {
        if (!v.pass()) continue;
        if (!names.empty()) names += ' ';
        names += v.name ? v.name : "?";
        ++bound;
    }
    LOG_INFO("[VIEW_STATE] view=%s bound=%zu: %s histories=%zu", m_view ? m_view->Name() : "-", bound,
             names.empty() ? "-" : names.c_str(), m_view ? m_view->HistoryCount() : size_t(0));
}
