#pragma once

// ActionRegistry - name -> handler dispatch for ActionLeaf / ConditionLeaf.
// Resolution order:
//   1. a native C++ handler registered with RegisterAction / RegisterCondition;
//   2. Lua fallback: Actions[name] / Conditions[name] in ScriptSystem's shared sol::state.
// Use native handlers for high-frequency work (movement, distance checks, perception queries) and
// Lua for designer-iterated logic (boss patterns, encounters). The Lua state is borrowed, so hot
// reload, AfterDelay, EventBus bridging and TimeScale behave the same inside Lua actions.

#include "AI/BTNode.h"      // NodeStatus, BTContext
#include "AI/AIComponents.h" // BBValue

#include <functional>
#include <string>
#include <unordered_map>

namespace sol { class state; }

namespace AI
{
    using ActionParams  = std::unordered_map<std::string, BBValue>;
    using NativeHandler = std::function<NodeStatus(BTContext&, const ActionParams&)>;

    class ActionRegistry
    {
    public:
        // Lua state for fallback dispatch: ScriptSystem's sol::state, borrowed (never deleted); nullptr
        // disables Lua dispatch.
        void BindLua(sol::state* lua) { m_lua = lua; }
        sol::state* GetLua() const    { return m_lua; }

        void RegisterAction   (std::string name, NativeHandler fn);
        void RegisterCondition(std::string name, NativeHandler fn);

        // Called by ActionLeaf / ConditionLeaf. An unknown name returns NodeStatus::Failure and logs once.
        NodeStatus DispatchAction   (const std::string& name,
                                     BTContext&         ctx,
                                     const ActionParams& params);
        NodeStatus DispatchCondition(const std::string& name,
                                     BTContext&         ctx,
                                     const ActionParams& params);

        // For the editor's available-actions list and for tests.
        bool HasNativeAction   (const std::string& name) const;
        bool HasNativeCondition(const std::string& name) const;

    private:
        NodeStatus CallLua(const char*         tableName,
                           const std::string&  name,
                           BTContext&          ctx,
                           const ActionParams& params,
                           std::unordered_map<std::string, bool>& warnSet);

        std::unordered_map<std::string, NativeHandler> m_actions;
        std::unordered_map<std::string, NativeHandler> m_conditions;
        sol::state*                                    m_lua = nullptr;

        // Warn-once sets: a misspelled name would otherwise log on every tick of every agent.
        std::unordered_map<std::string, bool> m_warnedActions;
        std::unordered_map<std::string, bool> m_warnedConditions;
    };
}
