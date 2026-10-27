#include "ECS/SystemRegistry.h"
#include "ECS/ECS.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>

// Tick-order registration lives in App::RegisterTickSystems: the adapters need references to
// App-owned concrete systems (ScriptSystem, PhysicsSystem, ...) that a static factory cannot
// reach cleanly.

void SystemRegistry::Initialize(World& world)
{
    for (auto& phaseVec : m_phases)
        for (auto& sys : phaseVec)
            sys->OnRegister(world);
}

void SystemRegistry::Shutdown(World& world)
{
    // Reverse-iterate so OnUnregister mirrors OnRegister order. Systems
    // late in the list often subscribed to events fired by earlier ones;
    // tearing them down in reverse keeps that dependency direction.
    for (auto it = m_phases.rbegin(); it != m_phases.rend(); ++it)
        for (auto sit = it->rbegin(); sit != it->rend(); ++sit)
            (*sit)->OnUnregister(world);
}

std::span<const std::unique_ptr<ISystem>>
SystemRegistry::GetSystems(TickPhase phase) const
{
    return m_phases[static_cast<size_t>(phase)];
}

void SystemRegistry::MoveBefore(TickPhase phase, const ISystem& system, std::string_view beforeName)
{
    auto& systems = m_phases[static_cast<size_t>(phase)];
    const auto self = std::find_if(systems.begin(), systems.end(),
                                   [&](const std::unique_ptr<ISystem>& s) { return s.get() == &system; });
    const auto target = std::find_if(systems.begin(), systems.end(),
                                     [&](const std::unique_ptr<ISystem>& s) { return beforeName == s->GetName(); });
    ENGINE_CHECK(target != systems.end(), "SystemRegistry: '%s' is to run before '%.*s', which its phase lacks - it "
                 "stays at the phase's end", system.GetName(), static_cast<int>(beforeName.size()), beforeName.data());
    if (self == systems.end() || target == systems.end() || target > self)
        return;
    std::rotate(target, self, self + 1);
    ++m_generation;
}

bool SystemRegistry::AssignToSet(ISystem& system, std::string_view setName)
{
    if (setName.empty())
    {
        LOG_ERROR("SystemRegistry::AssignToSet(%s): set name cannot be empty "
                  "-- system stays UNGATED and will always run",
                  system.GetName());
        return false;
    }

    const SystemSetId id(setName);
    auto nameIt = m_systemSetNames.find(id);
    if (nameIt != m_systemSetNames.end() && nameIt->second != setName)
    {
        LOG_ERROR("SystemRegistry: SystemSet hash collision '%s' <-> '%.*s' "
                  "-- system '%s' stays UNGATED and will always run",
                  nameIt->second.c_str(), static_cast<int>(setName.size()),
                  setName.data(), system.GetName());
        return false;
    }
    if (nameIt == m_systemSetNames.end())
        m_systemSetNames.emplace(id, std::string(setName));

    auto& memberships = m_systemSets[&system];
    if (std::find(memberships.begin(), memberships.end(), id) == memberships.end())
    {
        memberships.push_back(id);
        ++m_generation;
    }
    return true;
}

bool SystemRegistry::IsAlwaysRun(const ISystem& system) const
{
#if WITH_HARNESS
    // -alwaysrun_legacy (harness builds only): no system is declared always-run, as before the
    // declaration existed. The unset_policy_always_run pack's reverse arm.
    static const bool s_legacy = Sys::CommandLine::Get().Has("-alwaysrun_legacy");
    if (s_legacy) return false;
#endif
    return m_alwaysRun.count(&system) != 0;
}

std::span<const SystemSetId>
SystemRegistry::GetSystemSets(const ISystem& system) const
{
    static const std::vector<SystemSetId> kEmpty;
    auto it = m_systemSets.find(&system);
    return it != m_systemSets.end()
        ? std::span<const SystemSetId>(it->second)
        : std::span<const SystemSetId>(kEmpty);
}

std::vector<const ISystem*>
SystemRegistry::GetSystemsInSet(SystemSetId set) const
{
    std::vector<const ISystem*> out;
    for (const auto& phase : m_phases)
    {
        for (const auto& system : phase)
        {
            const auto memberships = GetSystemSets(*system);
            if (std::find(memberships.begin(), memberships.end(), set)
                != memberships.end())
            {
                out.push_back(system.get());
            }
        }
    }
    return out;
}
