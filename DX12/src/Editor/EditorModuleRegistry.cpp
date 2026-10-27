#include "Editor/EditorModule.h"

#include "Editor/EditorLayer.h"
#include "Module/ModuleRegistry.h"

namespace Editor
{
ModuleRegistry& ModuleRegistry::Get()
{
    static ModuleRegistry s_registry;
    return s_registry;
}

void ModuleRegistry::Link(std::string_view name, EditorModuleFactory factory)
{
    Entry e;
    e.name    = std::string(name);
    e.factory = factory;
    m_entries.push_back(std::move(e));
}

IEditorModule* ModuleRegistry::Instance(Entry& e)
{
    if (!e.instance && e.factory) e.instance = e.factory();
    return e.instance.get();
}

void ModuleRegistry::RegisterComponentEditors(const ModuleHost& host)
{
    for (Entry& e : m_entries)
        if (IEditorModule* m = Instance(e))
        {
            host.layer->SetRegisteringModule(e.name);
            m->RegisterComponentEditors(host);
            host.layer->SetRegisteringModule({});
        }
}

void ModuleRegistry::Shutdown()
{
    for (Entry& e : m_entries) e.instance.reset();
}

void ModuleRegistry::AddPanels(std::vector<ModulePanel>& out)
{
    for (Entry& e : m_entries)
        if (Module::Registry::Get().IsEnabled(e.name))
            if (IEditorModule* m = Instance(e)) m->AddPanels(out);
}

void ModuleRegistry::AddMenuItems(std::vector<ModuleMenuItem>& out)
{
    for (Entry& e : m_entries)
        if (Module::Registry::Get().IsEnabled(e.name))
            if (IEditorModule* m = Instance(e)) m->AddMenuItems(out);
}

void ModuleRegistry::DrawPanels(const ModuleHost& host)
{
    for (Entry& e : m_entries)
        if (Module::Registry::Get().IsEnabled(e.name))
            if (IEditorModule* m = Instance(e)) m->DrawPanels(host);
}
}
