#include "Gameplay/GameFlowState.h"

#include "ECS/CommandBuffer.h"
#include "ECS/FrameContext.h"
#include "ECS/HierarchyComponents.h"
#include "Gameplay/GameState.h"     // BeginRun clears it
#include "Harness/RunLedger.h"      // a throwing hook is a RUN fault
#include "System/CommandLine.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <sstream>
#include <unordered_set>

namespace Gameplay
{
namespace
{
    const std::string kEmptyName;
}

const char* ToString(SwitchResult result) noexcept
{
    switch (result)
    {
    case SwitchResult::Accepted:          return "ok";
    case SwitchResult::Undefined:         return "undefined";
    case SwitchResult::InvalidDefinition: return "invalid";
    case SwitchResult::AlreadyCurrent:    return "already-current";
    case SwitchResult::PendingOccupied:   return "pending-occupied";
    case SwitchResult::ParentInactive:    return "parent-inactive";
    case SwitchResult::AbstractTarget:    return "abstract-target";
    case SwitchResult::NoReturnTarget:    return "no-return-target";
    case SwitchResult::RejectedByGuard:   return "rejected-by-guard";
    case SwitchResult::TerminalState:     return "terminal-state";
    }
    return "unknown";
}

void GameFlowStateMachine::AddDiagnostic(FlowDiagnostic::Severity severity,
                                         std::string state, std::string message)
{
    m_diagnostics.push_back(
        FlowDiagnostic{ severity, std::move(state), std::move(message) });
}

void GameFlowStateMachine::PushHistory(std::string from, std::string to,
                                       SwitchResult kind, float elapsedMs)
{
    // Fold a repeat of the newest record: a controller retrying the same rejected Switch every frame
    // would otherwise flush the ring in one second, evicting the entry that explains the bug.
    if (!m_history.empty())
    {
        const std::size_t newest =
            (m_historyNext + m_history.size() - 1) % m_history.size();
        FlowTransitionRecord& last = m_history[newest];
        if (last.kind == kind && kind != SwitchResult::Accepted
            && last.from == from && last.to == to)
        {
            last.frame = m_frameIndex;
            ++last.repeats;
            return;
        }
    }

    FlowTransitionRecord record;
    record.frame     = m_frameIndex;
    record.from      = std::move(from);
    record.to        = std::move(to);
    record.elapsedMs = elapsedMs;
    record.kind      = kind;

    if (m_history.size() < kHistoryCapacity)
    {
        m_history.push_back(std::move(record));
        m_historyNext = m_history.size() % kHistoryCapacity;
    }
    else
    {
        m_history[m_historyNext] = std::move(record);
        m_historyNext = (m_historyNext + 1) % kHistoryCapacity;
    }
}

SwitchResult GameFlowStateMachine::Reject(std::string_view target,
                                          SwitchResult reason)
{
    PushHistory(CurrentName(), std::string(target), reason, 0.f);
    return reason;
}

std::vector<GameFlowStateMachine::FlowTransitionRecord>
GameFlowStateMachine::History() const
{
    std::vector<FlowTransitionRecord> out;
    out.reserve(m_history.size());
    if (m_history.size() < kHistoryCapacity)
    {
        out = m_history;   // never wrapped: storage order IS chronological
        return out;
    }
    for (std::size_t i = 0; i < m_history.size(); ++i)
        out.push_back(m_history[(m_historyNext + i) % m_history.size()]);
    return out;
}

bool GameFlowStateMachine::DefineState(GameFlowStateDefinition definition)
{
    if (definition.name.empty())
    {
        LOG_ERROR("GameFlow.DefineState: state name cannot be empty");
        return false;
    }
    if (m_inTransition)
    {
        LOG_WARNING("GameFlow.DefineState('%s'): rejected during transition",
                    definition.name.c_str());
        return false;
    }

    const GameStateId id(definition.name);
    if (const Definition* prior = Find(id);
        prior && prior->name != definition.name)
    {
        LOG_ERROR("GameFlow: state hash collision '%s' <-> '%s' (0x%016llx)",
                  prior->name.c_str(), definition.name.c_str(),
                  static_cast<unsigned long long>(id.value));
        return false;
    }

    Definition next;
    next.id         = id;
    next.name       = std::move(definition.name);
    next.parentName = std::move(definition.parent);
    next.parent     = GameStateId(next.parentName);
    next.onEnter    = std::move(definition.onEnter);
    next.onExit     = std::move(definition.onExit);
    next.initialName = std::move(definition.initial);
    next.initial     = GameStateId(next.initialName);
    next.abstract    = definition.abstract;
    next.final       = definition.final;
    next.priority    = definition.priority;
    next.on          = std::move(definition.on);
    next.canEnter    = std::move(definition.canEnter);
    next.canExit     = std::move(definition.canExit);
    next.timeout     = definition.timeout;
    next.timeoutTarget = std::move(definition.timeoutTarget);
    next.rebuildOnSceneLoad = definition.rebuildOnSceneLoad;
    next.onUpdate    = std::move(definition.onUpdate);
    next.timeScale  = definition.timeScale;
    next.inputContext = std::move(definition.inputContext);
    next.cursor     = definition.cursor;
    next.pauseAudio = definition.pauseAudio;
    next.busDuck    = std::move(definition.busDuck);

    for (std::string& setName : definition.sets)
    {
        if (setName.empty()) continue;
        const SystemSetId set(setName);
        if (std::find(next.sets.begin(), next.sets.end(), set) != next.sets.end())
            continue;
        next.sets.push_back(set);
        next.setNames.push_back(std::move(setName));
    }
    for (std::string& setName : definition.suspends)
    {
        if (setName.empty()) continue;
        const SystemSetId set(setName);
        if (std::find(next.suspends.begin(), next.suspends.end(), set) != next.suspends.end())
            continue;
        next.suspends.push_back(set);
        next.suspendNames.push_back(std::move(setName));
    }

    const bool replacing = m_definitions.find(id) != m_definitions.end();
    m_definitions[id] = std::move(next);

    if (m_finalized)
    {
        // Post-finalize definition has no order contract: this state may name a
        // parent or an initial child that the very next line declares. Findings
        // are still RETAINED at Error severity; only the log level softens.
        m_validationLogAsWarning = true;
        m_definitionsValid = ValidateDefinitions(/*logErrors=*/true);
        m_validationLogAsWarning = false;
    }
    if (m_current.IsValid())
        RefreshActiveSets();

    LOG_INFO("GameFlow: %s state '%s' (parent='%s', sets=%zu, suspends=%zu)",
             replacing ? "redefined" : "defined", NameOf(id).c_str(),
             Find(id)->parentName.c_str(), Find(id)->sets.size(),
             Find(id)->suspends.size());
    return true;
}

bool GameFlowStateMachine::RegisterSystemSet(std::string_view name)
{
    if (name.empty()) return false;

    const SystemSetId id(name);
    auto it = m_knownSystemSets.find(id);
    if (it != m_knownSystemSets.end())
    {
        if (it->second != name)
        {
            LOG_ERROR("GameFlow: SystemSet hash collision '%s' <-> '%.*s' (0x%016llx)",
                      it->second.c_str(), static_cast<int>(name.size()), name.data(),
                      static_cast<unsigned long long>(id.value));
            return false;
        }
        return true;
    }

    m_knownSystemSets.emplace(id, std::string(name));
    // A pause with no state gates by the known sets: a late one joins the active ones.
    if (m_pauseApplied && !m_current.IsValid()) RefreshActiveSets();
    if (m_finalized)
    {
        m_validationLogAsWarning = true;   // see DefineState
        m_definitionsValid = ValidateDefinitions(/*logErrors=*/true);
        m_validationLogAsWarning = false;
    }
    return true;
}

bool GameFlowStateMachine::FinalizeDefinitions()
{
#if WITH_HARNESS
    // -gameflow_unset_policy=never | default:<Set> (harness builds only): what a system that declares no
    // set does under a flow state. No game picks a policy yet; a pack runs the engine under one to prove
    // the systems that must always run (SystemRegistry::AddAlwaysRun) still do.
    {
        const std::string policy = Sys::CommandLine::Get().Get("-gameflow_unset_policy");
        if (policy == "never")
            SetUnsetSystemPolicy(UnsetSystemPolicy::NeverRun, {});
        else if (policy.rfind("default:", 0) == 0)
            SetUnsetSystemPolicy(UnsetSystemPolicy::DefaultSet, std::string_view(policy).substr(8));
    }
#endif
    m_finalized = true;
    m_definitionsValid = ValidateDefinitions(/*logErrors=*/true);

    if (m_pending)
    {
        const Definition* target = Find(*m_pending);
        if (!target || !target->valid)
        {
            LOG_ERROR("GameFlow: clearing invalid pending startup state '%s'",
                      PendingName().c_str());
            m_pending.reset();
        }
    }
    if (m_initial)
    {
        const Definition* initial = Find(*m_initial);
        if (!initial || !initial->valid)
        {
            LOG_ERROR("GameFlow: clearing invalid remembered initial state '%s'; "
                      "the next accepted Switch becomes the replayed boot state",
                      NameOf(*m_initial).c_str());
            m_initial.reset();
        }
    }

    if (m_definitionsValid)
        LOG_INFO("GameFlow: validated %zu states against %zu SystemSets",
                 m_definitions.size(), m_knownSystemSets.size());
    return m_definitionsValid;
}

bool GameFlowStateMachine::ValidateDefinitions(bool logErrors)
{
    bool allValid = true;
    // Findings are RETAINED, not just logged: the editor panel, -dumpflow and the CI gate all read
    // Diagnostics(), and none of them sees a log line.
    m_diagnostics.clear();
    for (auto& [id, definition] : m_definitions)
        definition.valid = true;

    // One place that both retains the finding and logs it, so the two never disagree.
    // The LOG LEVEL varies: validation re-runs on every DefineState after finalize, where definition
    // order is no contract - a parent defined before its child, or an `initial` about to be declared,
    // is unresolved for one line. Logging those at ERROR would blow a -maxerrors budget for nothing.
    // The DIAGNOSTIC stays Error either way, so the panel and check_gameflow.py still fail on a
    // finding that remains once the graph settles.
    const bool asWarning = m_validationLogAsWarning;
    auto report = [&](const std::string& stateName, std::string message)
    {
        if (logErrors)
        {
            if (asWarning)
                LOG_WARNING("GameFlow state '%s': %s (may resolve once the rest "
                            "of the graph is defined)",
                            stateName.c_str(), message.c_str());
            else
                LOG_ERROR("GameFlow state '%s': %s",
                          stateName.c_str(), message.c_str());
        }
        AddDiagnostic(FlowDiagnostic::Severity::Error, stateName, std::move(message));
    };

    // Local references first: parents and set names must exist.
    for (auto& [id, definition] : m_definitions)
    {
        if (definition.parent.IsValid())
        {
            // Name compare mirrors the set-reference check: a hash-colliding
            // parent name must not silently reparent onto the colliding state.
            const Definition* parentDef = Find(definition.parent);
            if (!parentDef || parentDef->name != definition.parentName)
            {
                definition.valid = false;
                allValid = false;
                report(definition.name,
                       "unknown parent '" + definition.parentName + "'");
            }
        }

        // `initial` must name a real state AND be a child of the state that
        // declares it. A cousin would "work" for one transition and then leave
        // the path meaning something nobody wrote down.
        if (definition.initial.IsValid())
        {
            const Definition* child = Find(definition.initial);
            if (!child || child->name != definition.initialName)
            {
                definition.valid = false;
                allValid = false;
                report(definition.name,
                       "unknown initial child '" + definition.initialName + "'");
            }
            else if (child->parent != id)
            {
                definition.valid = false;
                allValid = false;
                report(definition.name, "initial '" + definition.initialName
                                        + "' is not one of its children");
            }
        }
        else if (definition.abstract)
        {
            definition.valid = false;
            allValid = false;
            report(definition.name,
                   "abstract states must name an 'initial' child");
        }

        auto validateSets = [&](const std::vector<SystemSetId>& ids,
                                const std::vector<std::string>& names,
                                const char* field)
        {
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const auto known = m_knownSystemSets.find(ids[i]);
                if (known == m_knownSystemSets.end() || known->second != names[i])
                {
                    definition.valid = false;
                    allValid = false;
                    report(definition.name, "unknown SystemSet '" + names[i]
                                            + "' in " + std::string(field));
                }
            }
        };
        validateSets(definition.sets, definition.setNames, "sets");
        validateSets(definition.suspends, definition.suspendNames, "suspends");
    }

    // Parent graph must be acyclic. Mark every node participating in a found
    // cycle invalid so no alternate entry path can sneak into it.
    for (auto& [startId, start] : m_definitions)
    {
        std::unordered_set<GameStateId> seen;
        GameStateId cursor = startId;
        while (cursor.IsValid())
        {
            if (!seen.insert(cursor).second)
            {
                allValid = false;
                for (GameStateId cycleMember : seen)
                    if (Definition* d = Find(cycleMember)) d->valid = false;
                report(start.name,
                       "parent cycle detected at '" + NameOf(cursor) + "'");
                break;
            }
            const Definition* d = Find(cursor);
            if (!d) break;
            cursor = d->parent;
        }
    }

    // Invalid ancestry invalidates descendants as well.
    bool changed = true;
    while (changed)
    {
        changed = false;
        for (auto& [id, definition] : m_definitions)
        {
            if (!definition.valid || !definition.parent.IsValid()) continue;
            const Definition* parent = Find(definition.parent);
            if (parent && !parent->valid)
            {
                definition.valid = false;
                allValid = false;
                changed = true;
                report(definition.name,
                       "parent '" + parent->name + "' is invalid");
            }
        }
    }

    // A known set that no state activates or suspends is usually a typo in a system's
    // `set = "..."` declaration: such systems never run once a flow state is active. Warning only:
    // sets are legal before flow adoption, so this only runs once both registries have loaded.
    if (m_finalized && !m_definitions.empty())
    {
        std::unordered_set<SystemSetId> referenced;
        for (const auto& [id, definition] : m_definitions)
        {
            referenced.insert(definition.sets.begin(), definition.sets.end());
            referenced.insert(definition.suspends.begin(),
                              definition.suspends.end());
        }
        for (const auto& [setId, setName] : m_knownSystemSets)
        {
            if (referenced.find(setId) != referenced.end()) continue;
            if (logErrors)
                LOG_WARNING("GameFlow: SystemSet '%s' is not referenced by any "
                            "state's sets/suspends", setName.c_str());
            AddDiagnostic(FlowDiagnostic::Severity::Warning, {},
                          "SystemSet '" + setName + "' is not referenced by any "
                          "state's sets/suspends");
        }
    }

    // Systems that declared no set at all. Under the default AlwaysRun policy
    // these keep running forever and no state can gate them, which is correct
    // for the engine layer and a silent bug for anything else -- so it is worth
    // one line the panel and the CI gate can both see.
    if (m_finalized && !m_unsetSystems.empty()
        && m_unsetPolicy == UnsetSystemPolicy::AlwaysRun)
    {
        std::string message = std::to_string(m_unsetSystems.size())
            + " registered system(s) declare no SystemSet and therefore run in "
              "every state (policy=AlwaysRun): ";
        for (size_t i = 0; i < m_unsetSystems.size(); ++i)
            message += (i ? ", " : "") + m_unsetSystems[i];
        AddDiagnostic(FlowDiagnostic::Severity::Warning, {}, std::move(message));
    }

    return allValid;
}

GameStateId GameFlowStateMachine::ResolveEntryLeaf(GameStateId target) const
{
    // Walk `initial` down to a leaf. The visited guard is not paranoia: an
    // initial chain that loops is a definition error validation quarantines,
    // but this can be called on a graph that has not been finalized yet.
    std::unordered_set<GameStateId> visited;
    GameStateId cursor = target;
    while (cursor.IsValid() && visited.insert(cursor).second)
    {
        const Definition* definition = Find(cursor);
        if (!definition || !definition->initial.IsValid()) break;
        const Definition* child = Find(definition->initial);
        if (!child || child->name != definition->initialName) break;
        cursor = definition->initial;
    }
    return cursor.IsValid() ? cursor : target;
}

std::string GameFlowStateMachine::EntryLeafName(std::string_view stateName) const
{
    if (!IsDefined(stateName)) return {};
    return NameOf(ResolveEntryLeaf(GameStateId(stateName)));
}

SwitchResult GameFlowStateMachine::AcceptPending(GameStateId leaf,
                                                 std::string_view requested,
                                                 bool reentry, bool overlay,
                                                 PropertyBag params,
                                                 std::string_view trigger)
{
    const Definition* target = Find(leaf);
    const int priority = target ? target->priority : 0;

    if (m_pending)
    {
        // Priority arbitration: ties keep first-write-wins. Both sides are logged: silently losing a
        // transition is the failure this slot exists to make visible.
        if (priority <= m_pendingPriority)
        {
            LOG_WARNING("GameFlow.Switch('%.*s'): rejected -- '%s' already won this "
                        "frame (priority %d vs %d)",
                        static_cast<int>(requested.size()), requested.data(),
                        PendingName().c_str(), priority, m_pendingPriority);
            return Reject(requested, SwitchResult::PendingOccupied);
        }
        LOG_WARNING("GameFlow.Switch('%.*s'): PREEMPTS pending '%s' (priority %d > %d)",
                    static_cast<int>(requested.size()), requested.data(),
                    PendingName().c_str(), priority, m_pendingPriority);
        PushHistory(CurrentName(), PendingName(), SwitchResult::PendingOccupied, 0.f);
    }

    m_pending         = leaf;
    m_pendingPriority = priority;
    m_pendingReentry  = reentry;
    m_pendingOverlay  = overlay;
    m_pendingParams   = std::move(params);
    m_pendingTrigger.assign(trigger);

    if (!m_initial && !m_current.IsValid() && !m_inTransition)
        m_initial = leaf;

    if (m_inTransition)
        LOG_INFO("GameFlow.Switch('%s'): queued during transition for next frame",
                 NameOf(leaf).c_str());
    // Accepted requests are NOT pushed to history here: the transition they
    // produce is recorded at resolve time, with its real cost and real frame.
    return SwitchResult::Accepted;
}

bool GameFlowStateMachine::GuardAllows(const Definition& definition, bool entering,
                                       const GameFlowTransition& edge) const
{
    const GameFlowGuard& guard = entering ? definition.canEnter : definition.canExit;
    if (!guard) return true;
    try
    {
        return guard(edge);
    }
    catch (const std::exception& e)
    {
        // A guard that threw has not answered, and the safe reading of "no
        // answer" is no: letting the transition through would run OnEnter for a
        // state whose own precondition just crashed.
        LOG_ERROR("GameFlow: can_%s('%s') threw (%s); treating it as a refusal",
                  entering ? "enter" : "exit", definition.name.c_str(), e.what());
        return false;
    }
    catch (...)
    {
        LOG_ERROR("GameFlow: can_%s('%s') threw; treating it as a refusal",
                  entering ? "enter" : "exit", definition.name.c_str());
        return false;
    }
}

SwitchResult GameFlowStateMachine::TryRequestSwitch(std::string_view stateName,
                                                    PropertyBag params)
{
    const GameStateId targetId(stateName);
    const Definition* target = Find(targetId);
    if (stateName.empty() || !target || target->name != stateName)
    {
        LOG_WARNING("GameFlow.Switch('%.*s'): rejected -- target is not defined",
                    static_cast<int>(stateName.size()), stateName.data());
        return Reject(stateName, SwitchResult::Undefined);
    }
    if (m_finalized && !target->valid)
    {
        LOG_WARNING("GameFlow.Switch('%s'): rejected -- state definition is invalid",
                    target->name.c_str());
        return Reject(stateName, SwitchResult::InvalidDefinition);
    }
    // A terminal state is terminal even for a request already in flight when it
    // was entered. The fault diversion is the one exception: landing in the
    // fault state has to work from anywhere, including from inside a Quit.
    if (!m_inFaultDivert)
    {
        if (const Definition* current = Find(m_current); current && current->final)
        {
            LOG_WARNING("GameFlow.Switch('%s'): rejected -- '%s' is a terminal state",
                        target->name.c_str(), current->name.c_str());
            return Reject(stateName, SwitchResult::TerminalState);
        }
    }

    // Naming a grouping state means "whatever that state currently means",
    // which is its initial chain's leaf. Without this, every caller has to know
    // the leaf names of a subtree it should not care about.
    const GameStateId leafId = ResolveEntryLeaf(targetId);
    const Definition* leaf   = Find(leafId);
    if (!leaf)
    {
        LOG_WARNING("GameFlow.Switch('%s'): rejected -- initial chain leads nowhere",
                    target->name.c_str());
        return Reject(stateName, SwitchResult::Undefined);
    }
    if (leaf->abstract)
    {
        LOG_WARNING("GameFlow.Switch('%s'): rejected -- '%s' is abstract; name one "
                    "of its children", target->name.c_str(), leaf->name.c_str());
        return Reject(stateName, SwitchResult::AbstractTarget);
    }
    if (m_finalized && !leaf->valid)
    {
        LOG_WARNING("GameFlow.Switch('%s'): rejected -- resolved leaf '%s' is invalid",
                    target->name.c_str(), leaf->name.c_str());
        return Reject(stateName, SwitchResult::InvalidDefinition);
    }
    if (m_current == leafId)
    {
        LOG_WARNING("GameFlow.Switch('%s'): rejected -- already current "
                    "(use Reenter to restart it)", leaf->name.c_str());
        return Reject(stateName, SwitchResult::AlreadyCurrent);
    }

    // Guards are asked HERE, at request time, not inside the transition, so Switch reports a refusal
    // synchronously to the caller that can act on it. A guard therefore runs with
    // m_inTransition == false and must be pure (see GameFlowGuard).
    GameFlowTransition edge;
    edge.from = m_current;  edge.fromName = CurrentName();
    edge.to   = leafId;     edge.toName   = leaf->name;

    if (m_current.IsValid())
    {
        if (const Definition* current = Find(m_current);
            current && !GuardAllows(*current, /*entering=*/false, edge))
        {
            LOG_WARNING("GameFlow.Switch('%s'): refused by can_exit on '%s'",
                        leaf->name.c_str(), current->name.c_str());
            return Reject(stateName, SwitchResult::RejectedByGuard);
        }
    }
    if (!GuardAllows(*leaf, /*entering=*/true, edge))
    {
        LOG_WARNING("GameFlow.Switch('%s'): refused by its own can_enter",
                    leaf->name.c_str());
        return Reject(stateName, SwitchResult::RejectedByGuard);
    }

    return AcceptPending(leafId, stateName, /*reentry=*/false, /*overlay=*/false,
                         std::move(params), {});
}

SwitchResult GameFlowStateMachine::TryRequestReenter()
{
    if (!m_current.IsValid())
    {
        LOG_WARNING("GameFlow.Reenter(): rejected -- no current state");
        return Reject({}, SwitchResult::Undefined);
    }
    const Definition* current = Find(m_current);
    if (!current || (m_finalized && !current->valid))
    {
        LOG_WARNING("GameFlow.Reenter(): rejected -- current state is invalid");
        return Reject(CurrentName(), SwitchResult::InvalidDefinition);
    }
    // Re-entry deliberately skips the guards. can_exit exists to veto LEAVING,
    // and a re-entry does not leave -- vetoing a respawn with the same predicate
    // that guards a level transition would be a surprise, not a feature.
    return AcceptPending(m_current, current->name, /*reentry=*/true,
                         /*overlay=*/false, m_currentParams, {});
}

SwitchResult GameFlowStateMachine::TryRequestSwitchOverlay(std::string_view stateName)
{
    // Remember where we are BEFORE the request is judged, and only commit it if
    // the request is actually accepted -- a refused overlay must not rewrite the
    // slot, or Back() would return somewhere nobody ever went.
    const std::optional<GameStateId> saved = m_current.IsValid()
        ? std::optional<GameStateId>(m_current) : std::nullopt;

    const SwitchResult result = TryRequestSwitch(stateName);
    if (result != SwitchResult::Accepted) return result;

    m_pendingOverlay = true;
    m_returnTo       = saved;
    return result;
}

SwitchResult GameFlowStateMachine::TryRequestReturn()
{
    if (!m_returnTo)
    {
        LOG_WARNING("GameFlow.Back(): rejected -- nothing in the return slot");
        return Reject({}, SwitchResult::NoReturnTarget);
    }
    const GameStateId target = *m_returnTo;
    const Definition* definition = Find(target);
    if (!definition || (m_finalized && !definition->valid))
    {
        LOG_WARNING("GameFlow.Back(): rejected -- '%s' is no longer valid",
                    NameOf(target).c_str());
        m_returnTo.reset();
        return Reject(NameOf(target), SwitchResult::InvalidDefinition);
    }
    // A plain Switch, so resolving it CLEARS the slot: returning twice in a row
    // is not a thing, which is exactly the ambiguity a real stack would add.
    return TryRequestSwitch(definition->name);
}

const std::string& GameFlowStateMachine::ReturnToName() const
{
    return m_returnTo ? NameOf(*m_returnTo) : kEmptyName;
}

bool GameFlowStateMachine::CanEnterSubState(const Definition& target) const
{
    // Deliberately permissive: a target whose parent chain is NOT on the current path is legal.
    // ResolvePending's LCA walk enters every intermediate ancestor on the way down, so
    // MainMenu -> InGame.Combat is one transition - not two Switches across two frames, with the frame
    // between parked on a parent that was never meant as a destination.
    // The only structural refusal is an abstract target, checked by the caller after the initial
    // chain is resolved.
    (void)target;
    return true;
}

bool GameFlowStateMachine::Dispatch(std::string_view trigger)
{
    if (trigger.empty() || !m_current.IsValid()) return false;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return false;

    // Leaf -> root: the NEAREST handler wins (see GameFlowEdge).
    for (size_t i = path.size(); i > 0; --i)
    {
        const Definition* definition = Find(path[i - 1]);
        if (!definition) continue;
        for (const GameFlowEdge& edge : definition->on)
        {
            if (edge.trigger != trigger) continue;

            const GameStateId targetId(edge.target);
            const Definition* target = Find(targetId);
            if (!target || target->name != edge.target)
            {
                LOG_WARNING("GameFlow.Dispatch('%.*s'): '%s' names an undefined "
                            "target '%s'", static_cast<int>(trigger.size()),
                            trigger.data(), definition->name.c_str(),
                            edge.target.c_str());
                return false;
            }
            const SwitchResult result = TryRequestSwitch(target->name);
            if (result == SwitchResult::Accepted) m_pendingTrigger.assign(trigger);
            // A handler that EXISTS has consumed the trigger even if the switch
            // was refused: bubbling further would hand the same input to an
            // ancestor that a nearer state already claimed.
            return result == SwitchResult::Accepted;
        }
    }
    return false;
}

void GameFlowStateMachine::RebuildAfterWorldReload(World& world)
{
    if (!m_current.IsValid()) return;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return;

    // The edge describes a transition that did not happen: from and to are both
    // the current state. Hooks that branch on it therefore see "I am being set
    // up again", not "I came from somewhere".
    GameFlowTransition edge;
    edge.from = m_current;  edge.fromName = CurrentName();
    edge.to   = m_current;  edge.toName   = CurrentName();

    for (GameStateId stateId : path)
    {
        const Definition* definition = Find(stateId);
        if (!definition || !definition->rebuildOnSceneLoad) continue;
        LOG_INFO("GameFlow: rebuilding '%s' after a world reload",
                 definition->name.c_str());
        InvokeHook(*definition, /*entering=*/true, world, edge, m_currentParams);
    }
}

void GameFlowStateMachine::TickCurrent(World& world, float dt)
{
    if (!m_current.IsValid() || m_inTransition) return;

    m_timeInState += dt;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return;

    // ROOT -> LEAF: an ancestor keeps ticking while a sub-state is current.
    for (GameStateId stateId : path)
    {
        const Definition* definition = Find(stateId);
        if (!definition || !definition->onUpdate) continue;
        try
        {
            definition->onUpdate(world, dt, m_timeInState);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("GameFlow: on_update('%s') threw: %s",
                      definition->name.c_str(), e.what());
        }
        catch (...)
        {
            LOG_ERROR("GameFlow: on_update('%s') threw an unknown exception",
                      definition->name.c_str());
        }
    }

    // Timeout is the LEAF's only: an ancestor's clock would keep re-arming as
    // sub-states came and went, which is not a timer anyone means to write.
    if (m_timeoutFired) return;
    const Definition* leaf = Find(m_current);
    if (!leaf || leaf->timeout <= 0.f || leaf->timeoutTarget.empty()) return;
    if (m_timeInState < leaf->timeout) return;

    // Latched: one attempt per visit. Without this a refused switch would be
    // retried every frame for as long as the state stayed current.
    m_timeoutFired = true;
    LOG_INFO("GameFlow: '%s' timed out after %.3fs -> '%s'",
             leaf->name.c_str(), leaf->timeout, leaf->timeoutTarget.c_str());
    TryRequestSwitch(leaf->timeoutTarget);
}

bool GameFlowStateMachine::ResolvePending(World& world, CommandBuffer& commands)
{
    // The phase buffer is not where scoped cleanup goes (m_transitionCommands lands it before
    // OnEnter). The parameter stays: the resolver system's null-check on it catches a FrameContext
    // wired without a CommandBuffer, and a hook that wants phase-boundary deferral should use this
    // one rather than the private buffer.
    (void)commands;

    // A pause requested or released since last frame takes effect now, with this frame's
    // transitions: scripts and the scheduler see one consistent frame.
    ApplyPause();

    // Editor Play->Stop ARMS a boot replay; it materialises HERE, on the first frame that resolves,
    // not at Stop: an m_pending written at Stop would stay occupied through the editing session and
    // make the next Play's first Switch lose first-write-wins to a stale request. A script that
    // switches before this point simply wins, which is correct.
    if (m_replayInitial && !m_pending && !m_current.IsValid() && !m_inTransition)
    {
        m_replayInitial = false;
        if (m_initial) m_pending = m_initial;
    }

    if (!m_pending || m_inTransition) return false;

    const GameStateId targetId = *m_pending;
    const bool isReentry = m_pendingReentry;
    const bool isOverlay = m_pendingOverlay;
    PropertyBag params   = std::move(m_pendingParams);
    const std::string trigger = std::move(m_pendingTrigger);
    m_pending.reset(); // callbacks may now queue the next frame's request
    m_pendingPriority = 0;
    m_pendingReentry  = false;
    m_pendingOverlay  = false;
    m_pendingParams   = {};
    m_pendingTrigger.clear();

    const std::string requestedName = NameOf(targetId).empty()
                                    ? std::string("<unknown>") : NameOf(targetId);

    const Definition* target = Find(targetId);
    if (!target || (m_finalized && !target->valid))
    {
        LOG_ERROR("GameFlow: pending transition target is no longer valid");
        PushHistory(CurrentName(), requestedName,
                    SwitchResult::InvalidDefinition, 0.f);
        // Never replay a dead boot state: Editor Play->Stop->Play re-queues
        // m_initial, so a stale value would log this error once per session.
        if (m_initial && *m_initial == targetId)
            m_initial.reset();
        return false;
    }
    if (!CanEnterSubState(*target))
    {
        LOG_WARNING("GameFlow: discarded pending sub-state '%s' because parent '%s' is no longer active",
                    target->name.c_str(), target->parentName.c_str());
        PushHistory(CurrentName(), target->name,
                    SwitchResult::ParentInactive, 0.f);
        return false;
    }

    std::vector<GameStateId> oldPath;
    std::vector<GameStateId> newPath;
    if (m_current.IsValid() && !BuildStatePath(m_current, oldPath))
    {
        LOG_ERROR("GameFlow: current state path is invalid; transition aborted");
        PushHistory(CurrentName(), target->name,
                    SwitchResult::InvalidDefinition, 0.f);
        return false;
    }
    if (!BuildStatePath(targetId, newPath))
    {
        LOG_ERROR("GameFlow: target state path is invalid; transition aborted");
        PushHistory(CurrentName(), target->name,
                    SwitchResult::InvalidDefinition, 0.f);
        return false;
    }

    const GameStateId fromId = m_current;
    const std::string fromName = NameOf(fromId);
    const std::string toName = target->name;
    const auto transitionStart = std::chrono::steady_clock::now();

    GameFlowTransition edge;
    edge.from = fromId;   edge.fromName = fromName;
    edge.to   = targetId; edge.toName   = toName;
    edge.trigger   = trigger;
    edge.isReentry = isReentry;

    // Announced BEFORE any OnExit, while the outgoing state is still alive (see StateWillChangeEvent).
    EventBus::Get().Publish(StateWillChangeEvent{ fromId, targetId,
                                                  fromName, toName });

    m_inTransition = true;
    struct TransitionGuard
    {
        bool& flag;
        ~TransitionGuard() { flag = false; }
    } guard{ m_inTransition };

    size_t common = 0;
    while (common < oldPath.size() && common < newPath.size()
           && oldPath[common] == newPath[common])
    {
        ++common;
    }

    // Re-entry: the paths are identical, so the LCA is the leaf itself and the
    // loops below would do nothing. Back the common prefix off by one so
    // exactly the leaf exits and re-enters, and every ancestor -- with its
    // sets, its scoped entities and its declared resources -- is untouched.
    if (isReentry && common > 0 && common == oldPath.size()
        && oldPath.size() == newPath.size())
    {
        --common;
    }

    // Exit leaf -> LCA. Parent-owned entities survive a transition into a
    // child sub-state (InGame -> Paused), which is the key overlay contract.
    std::vector<std::string> exitedNames;
    std::vector<std::string> enteredNames;
    for (size_t i = oldPath.size(); i > common; --i)
    {
        const Definition* exiting = Find(oldPath[i - 1]);
        if (!exiting) continue;
        // The OUTGOING state's own params, not the incoming ones: on_exit is the
        // last chance to read what this visit was started with.
        InvokeHook(*exiting, /*entering=*/false, world, edge, m_currentParams);
        exitedNames.push_back(exiting->name);
        QueueScopedCleanup(world, m_transitionCommands, exiting->id);
    }

    // Flush the exit's destroys HERE, before m_current moves and before any OnEnter runs. On the
    // shared phase buffer they would land only at the phase boundary, so the incoming OnEnter would
    // still see the outgoing state's entities (Engine.FindByTag("HUDRoot") returning the doomed HUD,
    // anything parented to it dying with it). A private buffer, flushed where no user code is on the
    // stack, closes that window without touching what the Input phase's own buffer does with what
    // the hooks queue.
    m_transitionCommands.Flush(world);

    m_current       = targetId;
    m_currentParams = std::move(params);
    m_timeInState   = 0.f;
    m_timeoutFired  = false;
    RefreshActiveSets();

    // The return slot survives exactly one hop. An ordinary Switch clears it;
    // only an overlay switch (which set it in TryRequestSwitchOverlay) keeps it.
    // Without this the slot would silently accumulate meaning across unrelated
    // transitions and Back() would become a guess.
    if (!isOverlay && !isReentry) m_returnTo.reset();

    // Enter LCA child -> target leaf.
    bool enterFailed = false;
    for (size_t i = common; i < newPath.size(); ++i)
    {
        const Definition* entering = Find(newPath[i]);
        if (!entering) continue;
        if (!InvokeHook(*entering, /*entering=*/true, world, edge, m_currentParams))
        {
            // Stop descending. Continuing would run the children's OnEnter on
            // top of a parent that failed to set itself up, which is a worse
            // world than the one we are already in.
            enterFailed = true;
            break;
        }
        enteredNames.push_back(entering->name);
    }

    EventBus::Get().Publish(StateChangedEvent{
        fromId, targetId, fromName, toName,
        std::move(exitedNames), std::move(enteredNames), trigger
    });

    // An OnExit hook can request the transition's own target while `current`
    // still names the old leaf. It is not a meaningful next-frame transition.
    if (m_pending && *m_pending == m_current)
    {
        LOG_WARNING("GameFlow: discarded redundant callback request for current state '%s'",
                    CurrentName().c_str());
        PushHistory(fromName, CurrentName(), SwitchResult::AlreadyCurrent, 0.f);
        m_pending.reset();
    }

    const float elapsedMs = std::chrono::duration<float, std::milli>(
        std::chrono::steady_clock::now() - transitionStart).count();
    PushHistory(fromName, toName, SwitchResult::Accepted, elapsedMs);

    // An OnEnter failed (threw or returned false). m_current has ALREADY moved and the sets are
    // already live, so the world is half-entered: gated systems are running for a state whose
    // entities were never spawned, and there is no reason to believe any request issued from here
    // would land. Divert to the fault state if the project named one - it is the only place known
    // to be enterable. Guarded against re-entry so a fault INSIDE the fault state's own OnEnter
    // stops at a loud log rather than a loop.
    if (enterFailed && !m_faultStateName.empty() && !m_inFaultDivert)
    {
        if (IsDefined(m_faultStateName) && !IsInState(m_faultStateName))
        {
            LOG_ERROR("GameFlow: OnEnter failed entering '%s'; diverting to the "
                      "fault state '%s'", toName.c_str(), m_faultStateName.c_str());
            m_inFaultDivert = true;
            TryRequestSwitch(m_faultStateName);
            m_inFaultDivert = false;
        }
    }
    else if (enterFailed)
    {
        LOG_ERROR("GameFlow: OnEnter failed entering '%s' and no fault state is "
                  "set -- the world is half-entered. Call GameFlow.SetFaultState "
                  "to give this a landing point.", toName.c_str());
    }

    if (m_traceTransitions)
        LOG_INFO("GameFlow transition '%s' -> '%s' completed in %.3f ms",
                 fromName.c_str(), toName.c_str(), elapsedMs);
    return true;
}

bool GameFlowStateMachine::BuildStatePath(GameStateId leaf,
                                          std::vector<GameStateId>& out) const
{
    out.clear();
    std::unordered_set<GameStateId> visited;
    GameStateId cursor = leaf;
    while (cursor.IsValid())
    {
        if (!visited.insert(cursor).second) return false;
        const Definition* d = Find(cursor);
        if (!d || (m_finalized && !d->valid)) return false;
        out.push_back(cursor);
        cursor = d->parent;
    }
    std::reverse(out.begin(), out.end());
    return true;
}

void GameFlowStateMachine::RefreshActiveSets()
{
    m_activeSets.clear();

    std::vector<GameStateId> path;
    if (m_current.IsValid() && BuildStatePath(m_current, path))
    {
        for (GameStateId stateId : path)
        {
            const Definition* definition = Find(stateId);
            if (!definition) continue;
            for (SystemSetId suspended : definition->suspends)
                m_activeSets.erase(suspended);
            for (SystemSetId enabled : definition->sets)
                m_activeSets.insert(enabled);
        }
    }
    else if (!m_current.IsValid() && m_pauseApplied)
    {
        // No state: every set is on, less what the pause suspends.
        for (const auto& [id, name] : m_knownSystemSets)
        {
            (void)name;
            m_activeSets.insert(id);
        }
    }

    if (m_pauseApplied)
        for (const std::string& name : m_pauseTemplate.suspends)
            m_activeSets.erase(SystemSetId(name));

    RefreshResolvedResources();
}

void GameFlowStateMachine::RefreshResolvedResources()
{
    // Rebuilt WHOLE from the current path, never patched incrementally, so applying it again gives
    // the same world (see the resolved declarative resources in the header).
    m_effectiveTimeScale   = 1.f;
    m_effectiveCursor      = CursorMode::Inherit;
    m_effectiveAudioPaused = false;
    m_effectiveInputContexts.clear();
    m_effectiveBusDucks.clear();

    std::vector<GameStateId> path;
    if (m_current.IsValid() && !BuildStatePath(m_current, path)) path.clear();

    for (GameStateId stateId : path)
    {
        const Definition* definition = Find(stateId);
        if (!definition) continue;

        // Leaf-wins: walking root -> leaf and overwriting means the deepest
        // state that declared a value is the one that survives, so Paused can
        // freeze time without InGame having to know Paused exists.
        if (definition->timeScale) m_effectiveTimeScale = *definition->timeScale;
        if (definition->cursor != CursorMode::Inherit)
            m_effectiveCursor = definition->cursor;
        // OR, not leaf-wins: a state that wants silence must not be un-silenced
        // by a sub-state that simply did not mention audio.
        m_effectiveAudioPaused = m_effectiveAudioPaused || definition->pauseAudio;

        if (!definition->inputContext.empty())
        {
            if (std::find(m_effectiveInputContexts.begin(),
                          m_effectiveInputContexts.end(),
                          definition->inputContext)
                == m_effectiveInputContexts.end())
            {
                m_effectiveInputContexts.push_back(definition->inputContext);
            }
        }

        for (const auto& [bus, volume] : definition->busDuck)
        {
            auto it = std::find_if(m_effectiveBusDucks.begin(),
                                   m_effectiveBusDucks.end(),
                                   [&](const auto& e) { return e.first == bus; });
            if (it != m_effectiveBusDucks.end()) it->second = volume;
            else m_effectiveBusDucks.emplace_back(bus, volume);
        }
    }

    // The pause layer lies over whatever the path resolved to.
    if (m_pauseApplied)
    {
        m_effectiveTimeScale *= m_pauseTemplate.timeScale;
        m_effectiveAudioPaused = m_effectiveAudioPaused || m_pauseTemplate.pauseAudio;
        if (m_pauseTemplate.cursor != CursorMode::Inherit) m_effectiveCursor = m_pauseTemplate.cursor;
        if (!m_pauseTemplate.inputContext.empty()
            && std::find(m_effectiveInputContexts.begin(), m_effectiveInputContexts.end(),
                         m_pauseTemplate.inputContext) == m_effectiveInputContexts.end())
            m_effectiveInputContexts.push_back(m_pauseTemplate.inputContext);
    }
}

void GameFlowStateMachine::QueueScopedCleanup(World& world,
                                               CommandBuffer& commands,
                                               GameStateId owner) const
{
    const auto* pool = world.GetPool<StateScoped>();
    if (!pool) return;

    // Scoped roots and ALL their descendants die together: a scoped prefab root must not strand
    // children with dangling Parent references. Iterative DFS: deep skinned hierarchies overflow a
    // recursive walk.
    std::vector<Entity> stack;
    const auto& entities = pool->Entities();
    const auto& scoped   = pool->Data();
    for (size_t i = 0; i < scoped.size(); ++i)
    {
        if (scoped[i].owner == owner)
            stack.push_back(entities[i]);
    }

    std::unordered_set<Entity> queued;
    while (!stack.empty())
    {
        const Entity e = stack.back();
        stack.pop_back();
        if (!queued.insert(e).second) continue;
        commands.DestroyEntity(e);
        if (const Children* children = world.GetComponent<Children>(e))
        {
            for (Entity child : children->entities)
                if (world.IsAlive(child)) stack.push_back(child);
        }
    }
}

bool GameFlowStateMachine::InvokeHook(const Definition& definition,
                                      bool entering, World& world,
                                      const GameFlowTransition& edge,
                                      const PropertyBag& params) const
{
    const auto& hook = entering ? definition.onEnter : definition.onExit;
    if (!hook) return true;

    if (m_traceTransitions)
        LOG_INFO("GameFlow: On%s('%s')", entering ? "Enter" : "Exit",
                 definition.name.c_str());

    // A throwing hook is recorded as a RUN-LEVEL fault, not just a log line (as the Lua System path
    // does): otherwise an unattended run keeps rendering to -frames=N with a broken flow and exits 0,
    // having asserted nothing.
    try
    {
        // The hook's OWN verdict, not just "it did not throw" -- see the note on
        // GameFlowHook.
        if (hook(world, edge, params)) return true;
        // Reported the same way a thrown one is: a Lua error() arrives as a false return, not an
        // exception, and a typo in an on_enter - the failure that actually happens - would otherwise leave
        // an unattended run exiting 0 with a broken flow.
        Harness::RunLedger::Get().NoteScriptFault(
            ("GameFlow:" + definition.name).c_str(),
            entering ? "on_enter failed" : "on_exit failed");
        return false;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("GameFlow: On%s('%s') threw: %s",
                  entering ? "Enter" : "Exit", definition.name.c_str(), e.what());
        Harness::RunLedger::Get().NoteScriptFault(
            ("GameFlow:" + definition.name).c_str(), e.what());
    }
    catch (...)
    {
        LOG_ERROR("GameFlow: On%s('%s') threw an unknown exception",
                  entering ? "Enter" : "Exit", definition.name.c_str());
        Harness::RunLedger::Get().NoteScriptFault(
            ("GameFlow:" + definition.name).c_str(), "unknown exception");
    }
    return false;
}

void GameFlowStateMachine::SetFaultState(std::string_view stateName)
{
    m_faultStateName.assign(stateName);
    if (!stateName.empty() && !IsDefined(stateName))
    {
        LOG_WARNING("GameFlow.SetFaultState('%.*s'): that state is not defined "
                    "(yet) -- a fault before it exists will only be logged",
                    static_cast<int>(stateName.size()), stateName.data());
    }
}

void GameFlowStateMachine::ShutdownRuntime(World& world, CommandBuffer& commands)
{
    (void)commands;
    if (!m_current.IsValid()) return;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path))
    {
        m_current = {};
        return;
    }

    GameFlowTransition edge;
    edge.from = m_current; edge.fromName = CurrentName();
    // No target: nothing is being entered. A hook that branches on edge.to sees
    // an empty name, which is the honest description of "the process is ending".

    LOG_INFO("GameFlow: shutting down from '%s'", CurrentName().c_str());
    for (size_t i = path.size(); i > 0; --i)
    {
        const Definition* exiting = Find(path[i - 1]);
        if (!exiting) continue;
        InvokeHook(*exiting, /*entering=*/false, world, edge, m_currentParams);
        // Scoped entities are NOT queued for destruction here: the world is
        // about to be torn down wholesale, and queueing into a buffer nobody
        // will flush would just be a lie in the log.
    }

    m_current = {};
    m_pending.reset();
    m_currentParams = {};
    RefreshActiveSets();
}

bool GameFlowStateMachine::BeginRun(std::string_view stateName, bool clearGameState)
{
    // The return slot and the params belong to the run that is ending. Carrying
    // either into a New Game is how a fresh save inherits the pause menu's idea
    // of where Back should go.
    m_returnTo.reset();
    m_currentParams = {};
    m_queuedRestore.reset();
    if (clearGameState) GameState::Get().ClearAll();
    return TryRequestSwitch(stateName) == SwitchResult::Accepted;
}

bool GameFlowStateMachine::UndefineState(std::string_view stateName)
{
    const GameStateId id(stateName);
    const Definition* definition = Find(id);
    if (!definition || definition->name != stateName) return false;

    const bool wasOnPath = IsInState(id);
    const GameStateId parent = definition->parent;
    m_definitions.erase(id);

    if (wasOnPath)
    {
        // Retreat to the nearest surviving ancestor rather than leaving
        // m_current pointing at the erased definition -- every
        // path walk from that point on would fail and the flow would look
        // "started" while answering nothing.
        GameStateId retreat = parent;
        while (retreat.IsValid() && !Find(retreat))
        {
            const Definition* d = Find(retreat);
            retreat = d ? d->parent : GameStateId{};
        }
        m_current = Find(retreat) ? retreat : GameStateId{};
        LOG_WARNING("GameFlow: '%.*s' was undefined while current; retreated to '%s'",
                    static_cast<int>(stateName.size()), stateName.data(),
                    m_current.IsValid() ? CurrentName().c_str() : "<none>");
        m_currentParams = {};
        m_timeInState   = 0.f;
        m_timeoutFired  = false;
        RefreshActiveSets();
    }

    if (m_pending && *m_pending == id) m_pending.reset();
    if (m_returnTo && *m_returnTo == id) m_returnTo.reset();
    if (m_initial && *m_initial == id) m_initial.reset();

    if (m_finalized)
    {
        m_validationLogAsWarning = true;
        m_definitionsValid = ValidateDefinitions(/*logErrors=*/true);
        m_validationLogAsWarning = false;
    }
    return true;
}

void GameFlowStateMachine::QueueRestore(std::string leafName, PropertyBag params)
{
    if (leafName.empty()) { m_queuedRestore.reset(); return; }
    m_queuedRestore.emplace(std::move(leafName), std::move(params));
}

bool GameFlowStateMachine::ApplyQueuedRestore()
{
    if (!m_queuedRestore) return false;

    const std::string leaf = m_queuedRestore->first;
    PropertyBag params     = m_queuedRestore->second;
    m_queuedRestore.reset();   // one shot, whatever happens

    if (!IsDefined(leaf))
    {
        // A save from a build whose graph has since been reshaped. Loudly
        // ignored rather than written into m_current, which would leave the
        // machine pointing at a state no path walk can resolve.
        LOG_WARNING("GameFlow: saved state '%s' no longer exists; leaving the "
                    "flow where it is", leaf.c_str());
        return false;
    }
    // Through the front door: the LCA walk enters every ancestor on the way down, so the LEAF alone
    // is enough and the saved path is not replayed one level at a time.
    const SwitchResult result = TryRequestSwitch(leaf, std::move(params));
    if (result != SwitchResult::Accepted)
    {
        LOG_WARNING("GameFlow: could not restore saved state '%s' (%s)",
                    leaf.c_str(), ToString(result));
        return false;
    }
    LOG_INFO("GameFlow: restoring saved state '%s'", leaf.c_str());
    return true;
}

bool GameFlowStateMachine::ScopeEntity(
    World& world, Entity entity, std::optional<GameStateId> owner) const
{
    if (!world.IsAlive(entity)) return false;
    const GameStateId resolvedOwner = owner.value_or(m_current);
    if (!resolvedOwner.IsValid() || !Find(resolvedOwner)) return false;

    world.AddComponent<StateScoped>(entity, StateScoped{ resolvedOwner });
    world.AddComponent<SceneTransientTag>(entity, SceneTransientTag{});
    return true;
}

void GameFlowStateMachine::ResetRuntime(bool queueInitial)
{
    m_current = {};
    m_pending.reset();
    m_inTransition = false;
    m_pendingPriority = 0;
    m_pendingReentry  = false;
    m_pendingOverlay  = false;
    m_returnTo.reset();
    m_queuedRestore.reset();
    m_inFaultDivert = false;
    m_currentParams = {};
    m_pendingParams = {};
    m_pendingTrigger.clear();
    m_timeInState  = 0.f;
    m_timeoutFired = false;
    m_activeSets.clear();
    m_pauseRequests.clear();
    m_pauseApplied = false;
    m_pauseDirty   = false;
    RefreshResolvedResources();   // back to 1x time, no context, no duck
    m_history.clear();
    m_historyNext = 0;
    // ARM the replay; ResolvePending fills the slot on the first frame that
    // actually runs. See the comment there for why the slot must stay empty
    // while the editor sits in Edit mode.
    m_replayInitial = queueInitial && m_initial.has_value();
}

void GameFlowStateMachine::SetUnsetSystemPolicy(UnsetSystemPolicy policy,
                                                std::string_view defaultSet)
{
    m_unsetPolicy = policy;
    m_unsetDefaultSetName.assign(defaultSet);
    m_unsetDefaultSet = SystemSetId(defaultSet);

    if (policy == UnsetSystemPolicy::DefaultSet)
    {
        if (defaultSet.empty())
        {
            LOG_ERROR("GameFlow: UnsetSystemPolicy::DefaultSet needs a set name; "
                      "falling back to AlwaysRun");
            m_unsetPolicy = UnsetSystemPolicy::AlwaysRun;
            return;
        }
        // Declare it here so a state may reference it without the name having to
        // come from some system's registration -- by definition nothing is IN it.
        RegisterSystemSet(defaultSet);
    }
    LOG_INFO("GameFlow: unset-system policy = %s%s%s",
             policy == UnsetSystemPolicy::AlwaysRun  ? "AlwaysRun"
           : policy == UnsetSystemPolicy::NeverRun   ? "NeverRun" : "DefaultSet",
             m_unsetDefaultSetName.empty() ? "" : " -> ",
             m_unsetDefaultSetName.c_str());
}

bool GameFlowStateMachine::UnsetSystemRuns() const
{
    // Pass-through: before a flow starts nothing is gated, so this cannot be a
    // way to switch systems off by accident during boot.
    if (!m_current.IsValid()) return true;

    switch (m_unsetPolicy)
    {
    case UnsetSystemPolicy::NeverRun:   return false;
    case UnsetSystemPolicy::DefaultSet:
        return m_activeSets.find(m_unsetDefaultSet) != m_activeSets.end();
    case UnsetSystemPolicy::AlwaysRun:
    default:                            return true;
    }
}

void GameFlowStateMachine::SetUnsetSystemReport(std::vector<std::string> systemNames)
{
    m_unsetSystems = std::move(systemNames);
    std::sort(m_unsetSystems.begin(), m_unsetSystems.end());
    // Re-run so the finding lands in Diagnostics() immediately -- the editor
    // panel and the CI gate both read that, and the owner calls this once, right
    // after FinalizeDefinitions.
    if (m_finalized) m_definitionsValid = ValidateDefinitions(/*logErrors=*/false);
}

bool GameFlowStateMachine::IsInState(GameStateId stateOrAncestor) const
{
    if (!stateOrAncestor.IsValid() || !m_current.IsValid()) return false;
    if (m_current == stateOrAncestor) return true;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return false;
    return std::find(path.begin(), path.end(), stateOrAncestor) != path.end();
}

bool GameFlowStateMachine::IsInState(std::string_view stateName) const
{
    if (stateName.empty()) return false;
    const GameStateId id(stateName);
    // Name compare, not a bare hash hit: a colliding name must not silently
    // report "yes, you are in that state".
    const Definition* definition = Find(id);
    if (!definition || definition->name != stateName) return false;
    return IsInState(id);
}

std::vector<std::string> GameFlowStateMachine::CurrentPathNames() const
{
    std::vector<std::string> out;
    if (!m_current.IsValid()) return out;

    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return out;
    out.reserve(path.size());
    for (GameStateId id : path) out.push_back(NameOf(id));
    return out;
}

int GameFlowStateMachine::CurrentDepth() const
{
    if (!m_current.IsValid()) return 0;
    std::vector<GameStateId> path;
    if (!BuildStatePath(m_current, path)) return 0;
    return static_cast<int>(path.size());
}

GameStateId GameFlowStateMachine::ParentOf(GameStateId state) const
{
    const Definition* definition = Find(state);
    return definition ? definition->parent : GameStateId{};
}

bool GameFlowStateMachine::IsDefined(std::string_view stateName) const
{
    if (stateName.empty()) return false;
    const Definition* definition = Find(GameStateId(stateName));
    return definition && definition->name == stateName;
}

void GameFlowStateMachine::FillSnapshot(FlowSnapshot& out) const
{
    out.current          = CurrentName();
    out.pending          = PendingName();
    out.inTransition     = m_inTransition;
    out.passThrough      = !m_current.IsValid();
    out.definitionsValid = m_definitionsValid;
    out.currentPath      = CurrentPathNames();
    out.states.clear();
    out.sets.clear();

    const std::unordered_set<GameStateId> onPath = [&]
    {
        std::vector<GameStateId> path;
        if (m_current.IsValid()) BuildStatePath(m_current, path);
        return std::unordered_set<GameStateId>(path.begin(), path.end());
    }();

    out.states.reserve(m_definitions.size());
    for (const auto& [id, definition] : m_definitions)
    {
        FlowStateRow row;
        row.name          = definition.name;
        row.parent        = definition.parentName;
        row.sets          = definition.setNames;
        row.suspends      = definition.suspendNames;
        row.valid         = definition.valid;
        row.isCurrent     = (m_current == id);
        row.onCurrentPath = onPath.find(id) != onPath.end();

        // Depth is walked with its own visited guard: an invalid definition may
        // still be sitting in a parent cycle, and the panel must render it
        // rather than hang on it.
        std::unordered_set<GameStateId> seen;
        GameStateId cursor = definition.parent;
        while (cursor.IsValid() && seen.insert(cursor).second)
        {
            ++row.depth;
            const Definition* parent = Find(cursor);
            if (!parent) break;
            cursor = parent->parent;
        }
        out.states.push_back(std::move(row));
    }
    std::sort(out.states.begin(), out.states.end(),
              [](const FlowStateRow& a, const FlowStateRow& b)
              { return a.name < b.name; });

    std::unordered_set<SystemSetId> referenced;
    for (const auto& [id, definition] : m_definitions)
    {
        referenced.insert(definition.sets.begin(), definition.sets.end());
        referenced.insert(definition.suspends.begin(), definition.suspends.end());
    }

    out.sets.reserve(m_knownSystemSets.size());
    for (const auto& [setId, setName] : m_knownSystemSets)
    {
        FlowSetRow row;
        row.name = setName;
        // NOT IsSystemSetActive: that reports "active" for everything while the
        // flow has not started, which is the pass-through lie this snapshot
        // exists to expose. passThrough carries that fact separately.
        row.active     = m_activeSets.find(setId) != m_activeSets.end();
        row.referenced = referenced.find(setId) != referenced.end();
        out.sets.push_back(std::move(row));
    }
    std::sort(out.sets.begin(), out.sets.end(),
              [](const FlowSetRow& a, const FlowSetRow& b)
              { return a.name < b.name; });
}

bool GameFlowStateMachine::IsSystemSetActive(SystemSetId set) const
{
    if (!m_current.IsValid() && m_pauseApplied)
    {
        // A pause with no state: every set is on but what the pause suspends.
        return m_knownSystemSets.find(set) == m_knownSystemSets.end() || m_activeSets.find(set) != m_activeSets.end();
    }
    if (!m_current.IsValid())
    {
        // Pass-through is for projects that never adopted GameFlow. A project that DEFINED a graph and
        // still has no current state is a bug, and the hardest one here to diagnose: every gate answers
        // "active", so the game looks normal while no state logic runs and DebugDump() prints the
        // pass-through gating line. Say it once.
        if (m_finalized && !m_definitions.empty() && !m_warnedPassThroughWithStates)
        {
            m_warnedPassThroughWithStates = true;
            LOG_ERROR("GameFlow: %zu states are defined but none is current, so "
                      "every SystemSet gate is open. Nothing switched into a "
                      "state -- check that a boot script calls GameFlow.Switch.",
                      m_definitions.size());
        }
        return true;
    }
    return m_activeSets.find(set) != m_activeSets.end();
}

bool GameFlowStateMachine::IsSystemSetActive(std::string_view setName) const
{
    return IsSystemSetActive(SystemSetId(setName));
}

std::unordered_set<SystemSetId>
GameFlowStateMachine::ActiveSystemSetSnapshot() const
{
    return m_activeSets;
}

std::vector<std::string> GameFlowStateMachine::ActiveSystemSetNames() const
{
    std::vector<std::string> out;
    out.reserve(m_activeSets.size());
    for (SystemSetId id : m_activeSets)
    {
        auto it = m_knownSystemSets.find(id);
        if (it != m_knownSystemSets.end())
        {
            out.push_back(it->second);
        }
        else
        {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "0x%016llx",
                          static_cast<unsigned long long>(id.value));
            out.emplace_back(buf);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

const std::string& GameFlowStateMachine::CurrentName() const
{
    return NameOf(m_current);
}

const std::string& GameFlowStateMachine::PendingName() const
{
    return m_pending ? NameOf(*m_pending) : kEmptyName;
}

std::string GameFlowStateMachine::DebugDump() const
{
    std::ostringstream out;
    const std::vector<std::string> path = CurrentPathNames();
    out << "GameFlow\n"
        << "  current: " << (CurrentName().empty() ? "<none>" : CurrentName()) << '\n'
        << "  path:";
    if (path.empty()) out << " <none>";
    for (size_t i = 0; i < path.size(); ++i)
        out << (i ? " / " : " ") << path[i];
    out << '\n'
        << "  pending: " << (PendingName().empty() ? "<none>" : PendingName()) << '\n'
        << "  inTransition: " << (m_inTransition ? "true" : "false") << '\n'
        << "  gating: " << (m_current.IsValid() ? "active" : "legacy-pass-through") << '\n'
        << "  definitions: " << (m_definitionsValid ? "valid" : "INVALID")
        << " (" << m_definitions.size() << " states, "
        << m_diagnostics.size() << " diagnostics)\n"
        << "  timeScale: " << m_effectiveTimeScale
        << "  cursor: " << (m_effectiveCursor == CursorMode::Captured ? "captured"
                          : m_effectiveCursor == CursorMode::Free     ? "free"
                                                                      : "inherit")
        << "  audioPaused: " << (m_effectiveAudioPaused ? "true" : "false") << '\n'
        << "  input contexts:";
    if (m_effectiveInputContexts.empty()) out << " <none>";
    for (const std::string& ctx : m_effectiveInputContexts) out << ' ' << ctx;
    out << '\n'
        << "  active sets:";

    const std::vector<std::string> names = ActiveSystemSetNames();
    if (names.empty()) out << " <none>";
    for (const std::string& name : names) out << ' ' << name;
    out << '\n';
    return out.str();
}

const GameFlowStateMachine::Definition*
GameFlowStateMachine::Find(GameStateId id) const
{
    auto it = m_definitions.find(id);
    return it != m_definitions.end() ? &it->second : nullptr;
}

GameFlowStateMachine::Definition* GameFlowStateMachine::Find(GameStateId id)
{
    auto it = m_definitions.find(id);
    return it != m_definitions.end() ? &it->second : nullptr;
}

const std::string& GameFlowStateMachine::NameOf(GameStateId id) const
{
    if (const Definition* d = Find(id)) return d->name;
    return kEmptyName;
}

void GameFlowTickSystem::Update(World& world, const FrameContext& ctx)
{
    // scaledDeltaTime, not deltaTime: a state that declared time_scale = 0 must
    // freeze its own on_update and its own timeout too, or a pause menu's
    // countdown would keep running behind it.
    m_flow.TickCurrent(world, ctx.scaledDeltaTime);
}

void GameFlowResolveSystem::Update(World& world, const FrameContext& ctx)
{
    m_flow.SetFrameIndex(ctx.frameIndex);
    if (!ctx.commandBuffer)
    {
        LOG_ERROR("GameFlowResolveSystem: FrameContext has no CommandBuffer");
        return;
    }
    m_flow.ResolvePending(world, *ctx.commandBuffer);
}

} // namespace Gameplay
