#pragma once

// Top-level game-flow state (Boot/Menu/Loading/InGame/Paused), not character AI.
// States are data definitions; the scheduler owns gating and one Input-phase
// resolver owns transition ordering.

#include "ECS/CommandBuffer.h"   // m_transitionCommands is held by value
#include "ECS/NotifyTypes.h"     // PropertyBag - transition params
#include "ECS/ECS.h"
#include "ECS/ISystem.h"
#include "ECS/SystemSet.h"
#include "System/StringHash.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Gameplay
{

using GameStateId = StringHash;

struct StateScoped
{
    GameStateId owner;
};

struct StateChangedEvent
{
    GameStateId from;
    GameStateId to;
    std::string fromName;
    std::string toName;
    // The states the LCA walk left and entered, so a subscriber need not recompute a hierarchy it
    // does not own.
    std::vector<std::string> exited;    // leaf -> LCA, in the order they exited
    std::vector<std::string> entered;   // LCA -> leaf, in the order they entered
    std::string trigger;                // set only when Dispatch() caused it
};

// Published BEFORE any OnExit runs: a subscriber that must act while the outgoing state is still
// alive (fade out its music, snapshot its scoreboard) does it here; StateChangedEvent is too late.
struct StateWillChangeEvent
{
    GameStateId from;
    GameStateId to;
    std::string fromName;
    std::string toName;
};

// Published when the game starts or stops being paused (GameFlow.Pause, the menu key), at the
// start of the frame the change takes effect.
struct PauseChangedEvent
{
    bool        paused = false;
    std::size_t requests = 0;   // live pause requests after the change
    std::string reasons;        // "menu key, settings"
};

// True under -pause_legacy (harness builds only): the menu key pauses the host again.
bool PauseLegacyHostMode();

// Why a request was (not) accepted. RequestSwitch is the bool shell; TryRequestSwitch returns the
// reason. Every non-Accepted value is also pushed into the transition history: a dropped request
// is the hardest half to debug from a log.
enum class SwitchResult : std::uint8_t
{
    Accepted,
    Undefined,          // no such state, or a hash collision under another name
    InvalidDefinition,  // quarantined by validation (bad parent / unknown set)
    AlreadyCurrent,     // already there; use RequestReenter to restart it
    PendingOccupied,    // first-write-wins: an earlier request owns this frame
    ParentInactive,     // CanEnterSubState refused it (it accepts every target: unused)
    AbstractTarget,     // a grouping state; name one of its children instead
    NoReturnTarget,     // Return() with nothing in the one-deep return slot
    RejectedByGuard,    // a can_exit / can_enter predicate said no
    TerminalState,      // the current state is `final`; nothing leaves it
};

const char* ToString(SwitchResult result) noexcept;

// What the scheduler does with a system that declared NO set at all. AlwaysRun is the default,
// but forgetting to declare a set is SILENT under it: the system keeps running and no state can
// ever gate it. DefaultSet turns "always on" from an omission into a declaration, routing every
// unset system through one named set the game layer must explicitly activate.
enum class UnsetSystemPolicy : std::uint8_t
{
    AlwaysRun,   // no membership == no gate (default)
    NeverRun,    // no membership == never runs once a flow is active
    DefaultSet,  // no membership == membership of one named set
};

// Cursor policy a state declares. Inherit means "whatever my ancestors said",
// so a sub-state that only cares about time does not have to restate it.
enum class CursorMode : std::uint8_t { Inherit, Captured, Free };

// The EDGE, not just the endpoints, handed to every hook and guard: "leaving InGame for MainMenu
// must fade out, leaving it for Paused must not" is otherwise inexpressible without the state
// knowing every state it can reach.
struct GameFlowTransition
{
    GameStateId from, to;
    std::string fromName, toName;
    std::string trigger;            // set only when Dispatch() caused it
    bool        isReentry = false;
};

// Hooks take the edge and the transition params. Guards take the edge alone and run OUTSIDE the
// transition, at request time, so they must be pure (Switch returns false synchronously); a guard
// that throws counts as a refusal.
// A hook returns FALSE when it failed. Catching C++ exceptions is not enough: the Lua hooks run
// through sol::protected_function, which swallows a Lua error() and reports an invalid result - a
// broken on_enter would otherwise look clean and never reach the fault state.
using GameFlowHook  = std::function<bool(World&, const GameFlowTransition&,
                                         const PropertyBag&)>;
using GameFlowGuard = std::function<bool(const GameFlowTransition&)>;
using GameFlowUpdate = std::function<void(World&, float dt, float timeInState)>;

// One row of the declarative trigger table. Dispatch("pause") walks the current path leaf -> root
// and the NEAREST handler wins (statechart bubbling), so "Escape" can mean different things in
// different states without an `if (state == X)` in the input code.
struct GameFlowEdge
{
    std::string trigger;
    std::string target;
};

struct GameFlowStateDefinition
{
    std::string name;
    std::string parent;                 // empty for a top-level state
    std::vector<std::string> sets;
    std::vector<std::string> suspends;  // applied after inherited parent sets

    // ---- Hierarchy ----
    // A state with children is usually a grouping concept ("InGame") whose meaning is one of its
    // leaves. `initial` names the child to descend into when this state is the switch target;
    // `abstract` refuses to be a target without one, so a typo cannot park the flow on a
    // half-configured parent.
    std::string initial;
    bool        abstract = false;

    // A TERMINAL state (Quit, ReturnToDesktop). Once entered, every further
    // switch is refused, so a request left in flight by a system that is being
    // torn down cannot drag the flow back out mid-shutdown.
    bool        final = false;

    // Same-frame arbitration: strictly higher wins the pending slot from a request that already
    // holds it; ties keep first-write-wins.
    int priority = 0;

    // ---- Declarative resources ----
    // What a state OWNS for its lifetime, declared rather than done in onEnter and undone in onExit.
    // Effective*() says why these are resolved rather than paired.
    std::optional<float> timeScale;     // 0 = freeze; unset = inherit
    std::string inputContext;           // pushed while this state is on the path
    CursorMode  cursor = CursorMode::Inherit;
    bool        pauseAudio = false;
    std::vector<std::pair<std::string, float>> busDuck;  // bus name -> volume

    // ---- Behaviour ----
    std::vector<GameFlowEdge> on;       // trigger -> target, bubbled up the path
    GameFlowGuard canEnter;             // asked at REQUEST time, must be pure
    GameFlowGuard canExit;
    float       timeout = 0.f;          // >0: auto-switch after this many seconds
    std::string timeoutTarget;

    // Re-run on_enter after a scene load wiped the world: World::Clear destroys this state's scoped
    // entities without on_exit or re-entry, so a HUD spawned by on_enter would simply vanish.
    // Rebuilding puts the entities in the NEW world with fresh ids, which every reference to them
    // expects.
    bool rebuildOnSceneLoad = false;

    GameFlowHook   onEnter;
    GameFlowHook   onExit;
    GameFlowUpdate onUpdate;            // ticked root -> leaf every frame
};

class GameFlowStateMachine
{
public:
    // ---- Tooling-facing value types ----
    // Pulled by the editor panel, the harness dump and the control channel. Plain data (no ImGui, no
    // sol, no engine headers), so those paths share one shape.

    struct FlowDiagnostic
    {
        enum class Severity : std::uint8_t { Warning, Error };
        Severity    severity = Severity::Error;
        std::string state;     // empty when the finding is not state-specific
        std::string message;
    };

    struct FlowTransitionRecord
    {
        std::uint64_t frame     = 0;
        std::string   from;
        std::string   to;
        float         elapsedMs = 0.f;   // resolve cost; 0 for a rejection
        SwitchResult  kind      = SwitchResult::Accepted;
        std::uint32_t repeats   = 0;     // consecutive identical rejections folded
    };

    struct FlowStateRow
    {
        std::string name;
        std::string parent;
        std::vector<std::string> sets;
        std::vector<std::string> suspends;
        bool valid         = true;
        bool onCurrentPath = false;
        bool isCurrent     = false;
        int  depth         = 0;
    };

    struct FlowSetRow
    {
        std::string name;
        bool active     = false;
        bool referenced = false;   // named by some state's sets/suspends
    };

    struct FlowSnapshot
    {
        std::string current;
        std::string pending;
        bool inTransition     = false;
        // True while no flow has started: every gate answers "active", so a
        // consumer that prints IsSystemSetActive verbatim would be lying.
        bool passThrough      = true;
        bool definitionsValid = true;
        std::vector<std::string> currentPath;   // root -> leaf
        std::vector<FlowStateRow> states;       // sorted by name
        std::vector<FlowSetRow>   sets;         // sorted by name
    };

    static constexpr std::size_t kHistoryCapacity = 64;

    // Definitions may be replaced (Lua hot reload). Hash collisions with a
    // different source name are rejected rather than silently aliasing states.
    bool DefineState(GameFlowStateDefinition definition);

    // System sets are declared by C++ scheduler registration and Lua System
    // scripts. Definitions are finalized after both registries have loaded.
    bool RegisterSystemSet(std::string_view name);
    bool FinalizeDefinitions();

    // First-write-wins, unless the new target's `priority` is strictly higher. During
    // OnExit/OnEnter a request may occupy the now-empty pending slot, but the current transition
    // always completes first and the new request is not resolved until the next frame.
    SwitchResult TryRequestSwitch(std::string_view stateName,
                                  PropertyBag params = {});
    bool RequestSwitch(std::string_view stateName, PropertyBag params = {})
    {
        return TryRequestSwitch(stateName, std::move(params))
            == SwitchResult::Accepted;
    }

    // Declarative trigger dispatch: walks the current path leaf -> root and takes the nearest state
    // that handles `trigger`. Returns SYNCHRONOUSLY, like Switch, so the caller learns whether
    // anything took it - not a fire-and-forget request channel.
    bool Dispatch(std::string_view trigger);

    // Params the current state was entered with. Empty for a state entered
    // without any, and cleared when the state is left, so nothing can read a
    // previous visit's payload.
    const PropertyBag& CurrentParams() const noexcept { return m_currentParams; }

    // Seconds the current LEAF has been current, advanced by TickCurrent with
    // the caller's dt (so a state that freezes time also freezes its own age).
    float TimeInState() const noexcept { return m_timeInState; }

    // Runs each state's onUpdate along the current path, ROOT -> LEAF, and
    // services `timeout`. Root-to-leaf is what makes "Paused is layered over
    // InGame, and InGame is still ticking" expressible.
    void TickCurrent(World& world, float dt);

    // Called by the owner after a scene load has replaced the world. Re-runs
    // on_enter, root -> leaf, for every state on the current path that declared
    // rebuild_on_scene_load. Does NOT re-run on_exit or move the state: nothing
    // was left, the floor was replaced.
    void RebuildAfterWorldReload(World& world);

    // ---- Fault handling ----
    // Where the flow lands when an on_enter fails (throws or returns false). Without one, a typo in a
    // hook leaves the world HALF-ENTERED (the sets live, the hook's entities missing, no state
    // reachable to fix it), and an unattended run renders that for N frames and exits 0.
    void SetFaultState(std::string_view stateName);
    const std::string& FaultStateName() const noexcept { return m_faultStateName; }

    // ---- Shutdown ----
    // Runs OnExit leaf -> root and clears the runtime, so "save on quit" is expressible: otherwise
    // no state learns it was the last one.
    void ShutdownRuntime(World& world, CommandBuffer& commands);

    // ---- Run boundary ----
    // New Game / level select / back to title are the SAME action: drop the return slot and the
    // params, optionally wipe GameState, and switch.
    bool BeginRun(std::string_view stateName, bool clearGameState);

    // ---- Hot reload ----
    // Removes a definition. If it is on the current path, the flow retreats to the nearest surviving
    // ancestor instead of pointing at a missing state. Deliberately not a staged graph-diff
    // transaction: that needs a second definition table and a merge, for a failure (a half-reloaded
    // graph for one frame) not seen in practice.
    bool UndefineState(std::string_view stateName);

    // ---- Save / restore ----
    // The path root -> leaf as plain data; the save writer stores it with CurrentParams without
    // knowing the machine.
    std::vector<std::string> SerializeRuntimePath() const { return CurrentPathNames(); }

    // Queues a restore for the next world that finishes loading. NOT applied here: the states being
    // restored spawn entities into a world that does not exist yet at load time. ApplyQueuedRestore
    // goes through the ordinary request path (never by writing m_current), so a state renamed away
    // since the save is refused loudly instead of poisoning the machine.
    void QueueRestore(std::string leafName, PropertyBag params);
    bool HasQueuedRestore() const noexcept { return m_queuedRestore.has_value(); }
    bool ApplyQueuedRestore();

    // Restarts the current LEAF: exits it, then re-enters it, every ancestor untouched. Retry and
    // respawn need this without a ghost node in the graph: the obvious alternative, Scene.Reload(),
    // goes through World::Clear, which wipes the state's StateScoped entities without OnExit, without
    // re-entering, and without a line in the log.
    // Plain Switch(Current()) stays REJECTED: re-entry has to be asked for.
    SwitchResult TryRequestReenter();
    bool RequestReenter() { return TryRequestReenter() == SwitchResult::Accepted; }

    // ---- One-deep return slot ----
    // NOT a general state stack, and it must not grow into one: it serves "go back where I came from"
    // (Options opened from MainMenu and from Paused - a sub-state cannot express that, it has one
    // parent). The slot holds exactly ONE state, only an overlay switch fills it, and any ordinary
    // Switch CLEARS it, so normal play cannot accumulate a hidden spine of screens.
    SwitchResult TryRequestSwitchOverlay(std::string_view stateName);
    bool RequestSwitchOverlay(std::string_view stateName)
    {
        return TryRequestSwitchOverlay(stateName) == SwitchResult::Accepted;
    }
    SwitchResult TryRequestReturn();
    bool RequestReturn() { return TryRequestReturn() == SwitchResult::Accepted; }
    std::optional<GameStateId> ReturnTo() const noexcept { return m_returnTo; }
    const std::string& ReturnToName() const;

    // Descend `target`'s `initial` chain to the leaf that will actually be
    // entered. Returns `target` unchanged when it names no initial child.
    GameStateId ResolveEntryLeaf(GameStateId target) const;
    // Name-in, name-out convenience for tooling and Lua. Empty when the name
    // is not a defined state.
    std::string EntryLeafName(std::string_view stateName) const;

    // Called once at the fixed Input-phase transition point.
    bool ResolvePending(World& world, CommandBuffer& commands);

    // Stamped by GameFlowResolveSystem so history records carry a frame number. A request made
    // mid-frame is stamped with the running frame; the transition it produces with the next one -
    // the one-frame deferral every transition has.
    void SetFrameIndex(std::uint64_t frame) noexcept { m_frameIndex = frame; }

    // Runtime entity ownership helper. SceneTransientTag is added with
    // StateScoped so a play-spawned presentation entity cannot leak into a
    // saved scene.
    bool ScopeEntity(World& world, Entity entity,
                     std::optional<GameStateId> owner = std::nullopt) const;

    // Editor Play->Stop keeps definitions but resets the runtime leaf state.
    // queueInitial ARMS a replay of the first accepted boot request; the slot
    // itself is only filled on the next frame that actually resolves, so Edit
    // mode never sits on an occupied pending slot.
    void ResetRuntime(bool queueInitial);

    bool HasCurrentState() const noexcept { return m_current.IsValid(); }
    bool InTransition() const noexcept { return m_inTransition; }
    GameStateId CurrentId() const noexcept { return m_current; }
    std::optional<GameStateId> PendingId() const noexcept { return m_pending; }
    const std::string& CurrentName() const;
    const std::string& PendingName() const;

    // ---- Hierarchy queries ----
    // Current() names only the LEAF, so `Current() == "InGame"` goes false the moment a sub-state is
    // entered: ask these instead.
    bool IsInState(GameStateId stateOrAncestor) const;
    bool IsInState(std::string_view stateName) const;
    std::vector<std::string> CurrentPathNames() const;   // root -> leaf
    int  CurrentDepth() const;                           // 0 when no flow started
    GameStateId ParentOf(GameStateId state) const;
    bool IsDefined(std::string_view stateName) const;

    // ---- Resolved declarative resources ----
    // Recomputed from the whole current path whenever it changes, NOT applied and unapplied in matched
    // pairs: the owner reads these every frame and makes the world match. That is idempotent, so an
    // aborted transition, a hot reload or a Play->Stop cannot strand a state's cursor grab or input
    // context (the failure mode of a push/pop design).
    // Scalars are LEAF-WINS along root -> leaf (the nearest ancestor that declared a value);
    // pauseAudio is an OR; input contexts accumulate in root -> leaf order.
    float      EffectiveTimeScale() const noexcept { return m_effectiveTimeScale; }
    CursorMode EffectiveCursorMode() const noexcept { return m_effectiveCursor; }
    bool       EffectiveAudioPaused() const noexcept { return m_effectiveAudioPaused; }
    const std::vector<std::string>& EffectiveInputContexts() const noexcept
    {
        return m_effectiveInputContexts;
    }
    const std::vector<std::pair<std::string, float>>& EffectiveBusDucks() const noexcept
    {
        return m_effectiveBusDucks;
    }

    // ---- Pause layer ----
    // A pause is not a state: it lies over whichever state is current (or none), leaves it alone and
    // stacks - the settings screen opens over the menu-key pause, and the game runs again when the
    // last request is released. While paused (applied at the start of a frame, like a switch) the
    // resolved time scale is multiplied by the template's (0), the template's SystemSets are
    // suspended, and its audio / input context / cursor wishes join the resolved resources. By
    // default the template suspends the simulation, the hit boxes and the cinematics and keeps the
    // animation chain, scripts, UI and audio running.
    struct PauseTemplate
    {
        std::vector<std::string> suspends = { "GameplaySimulation", "GameplayFX", "Cinematics" };
        float       timeScale  = 0.f;
        bool        pauseAudio = false;
        std::string inputContext;
        CursorMode  cursor     = CursorMode::Inherit;
        // The menu key (Game.exe: Escape / pad Start) toggles a pause when no state handles the
        // 'pause' trigger.
        bool        menuKey    = true;
    };
    using PauseId = std::uint64_t;
    // The owner of the menu key's own request; script owners are ScriptSystem owner ids.
    static constexpr std::uint64_t kHostPauseOwner = ~std::uint64_t{ 0 };

    PauseId     RequestPause(std::string reason, std::uint64_t owner = 0);
    bool        ReleasePause(PauseId id);
    std::size_t ReleasePausesOwnedBy(std::uint64_t owner);
    // Everything a script asked for (not the menu key's): a Lua reload or the editor's Stop.
    std::size_t ReleaseScriptPauses();
    std::vector<std::string> PauseReasons() const;
    bool        IsPaused() const noexcept { return m_pauseApplied; }
    void        ConfigurePause(PauseTemplate pauseTemplate);
    const PauseTemplate& GetPauseTemplate() const noexcept;
    // Game.exe's menu key (MenuToggle). Takes the 'pause' trigger through the state graph first,
    // else toggles the menu-key pause. False only under -pause_legacy: the caller then pauses the host.
    bool        OnMenuKey();
    // True when SystemSet gates mean something: a state is current, or a pause is applied.
    bool        GatingActive() const noexcept { return m_current.IsValid() || m_pauseApplied; }

    // When no flow has been started, scoped gates are pass-through, so a project without GameFlow
    // states runs every system.
    bool IsSystemSetActive(SystemSetId set) const;
    bool IsSystemSetActive(std::string_view setName) const;
    std::unordered_set<SystemSetId> ActiveSystemSetSnapshot() const;
    std::vector<std::string> ActiveSystemSetNames() const;

    const std::unordered_map<SystemSetId, std::string>& KnownSystemSets() const
    {
        return m_knownSystemSets;
    }

    void SetTraceTransitions(bool enabled) noexcept { m_traceTransitions = enabled; }
    bool TraceTransitions() const noexcept { return m_traceTransitions; }

    // ---- Unset-system policy ----
    void SetUnsetSystemPolicy(UnsetSystemPolicy policy,
                              std::string_view defaultSet = {});
    UnsetSystemPolicy GetUnsetSystemPolicy() const noexcept { return m_unsetPolicy; }
    // Resolved answer for "may a system with no declared set run right now?".
    // Read once per phase by the Scheduler, alongside the active-set snapshot.
    bool UnsetSystemRuns() const;

    // The machine deliberately knows nothing about ISystem, so the owner (which
    // holds the SystemRegistry) hands it the list of registered systems that
    // declared no set. Surfaced as one Warning diagnostic, because forgetting a
    // set fails silently: the system simply keeps running and no state can ever
    // gate it.
    void SetUnsetSystemReport(std::vector<std::string> systemNames);
    const std::vector<std::string>& UnsetSystemReport() const noexcept
    {
        return m_unsetSystems;
    }

    // ---- Introspection ----
    void FillSnapshot(FlowSnapshot& out) const;
    const std::vector<FlowDiagnostic>& Diagnostics() const noexcept
    {
        return m_diagnostics;
    }
    // Oldest -> newest. Capacity kHistoryCapacity; consecutive identical
    // rejections fold into one record's `repeats` so a per-frame retry loop
    // cannot flush the interesting entries out of the ring.
    std::vector<FlowTransitionRecord> History() const;

    // The state / pending / active-set half of the debug dump; the binding / tooling layer appends
    // scheduler and Lua-system membership.
    std::string DebugDump() const;

private:
    struct Definition
    {
        GameStateId id;
        std::string name;
        GameStateId parent;
        std::string parentName;
        std::vector<SystemSetId> sets;
        std::vector<std::string> setNames;
        std::vector<SystemSetId> suspends;
        std::vector<std::string> suspendNames;
        GameStateId initial;
        std::string initialName;
        bool        abstract = false;
        bool        final = false;
        int         priority = 0;
        std::optional<float> timeScale;
        std::string inputContext;
        CursorMode  cursor = CursorMode::Inherit;
        bool        pauseAudio = false;
        std::vector<std::pair<std::string, float>> busDuck;
        std::vector<GameFlowEdge> on;
        GameFlowGuard  canEnter;
        GameFlowGuard  canExit;
        float          timeout = 0.f;
        std::string    timeoutTarget;
        bool           rebuildOnSceneLoad = false;
        GameFlowHook   onEnter;
        GameFlowHook   onExit;
        GameFlowUpdate onUpdate;
        bool valid = true;
    };

    bool ValidateDefinitions(bool logErrors);
    SwitchResult AcceptPending(GameStateId leaf, std::string_view requested,
                               bool reentry, bool overlay,
                               PropertyBag params, std::string_view trigger);
    bool BuildStatePath(GameStateId leaf, std::vector<GameStateId>& out) const;
    bool CanEnterSubState(const Definition& target) const;
    void RefreshActiveSets();
    void RefreshResolvedResources();
    // Brings the applied pause in line with the requests (start of ResolvePending).
    void ApplyPause();
    bool IsPauseSuspended(SystemSetId set) const;
    void QueueScopedCleanup(World& world, CommandBuffer& commands,
                            GameStateId owner) const;
    // Returns false when the hook threw or returned false. A failed OnEnter is not survivable:
    // the sets are already live and the entities the hook was spawning are not, so the caller
    // diverts to the fault state instead of continuing.
    bool InvokeHook(const Definition& definition, bool entering, World& world,
                    const GameFlowTransition& edge, const PropertyBag& params) const;
    bool GuardAllows(const Definition& definition, bool entering,
                     const GameFlowTransition& edge) const;
    const Definition* Find(GameStateId id) const;
    Definition* Find(GameStateId id);
    const std::string& NameOf(GameStateId id) const;

    void AddDiagnostic(FlowDiagnostic::Severity severity,
                       std::string state, std::string message);
    void PushHistory(std::string from, std::string to,
                     SwitchResult kind, float elapsedMs);
    SwitchResult Reject(std::string_view target, SwitchResult reason);

    std::unordered_map<GameStateId, Definition> m_definitions;
    std::unordered_map<SystemSetId, std::string> m_knownSystemSets;
    std::unordered_set<SystemSetId> m_activeSets;

    struct PauseRequest
    {
        PauseId       id;
        std::string   reason;
        std::uint64_t owner;
    };
    std::vector<PauseRequest> m_pauseRequests;
    PauseTemplate             m_pauseTemplate;
    PauseId                   m_nextPauseId = 0;
    bool                      m_pauseApplied = false;
    bool                      m_pauseDirty   = false;   // the template changed under a pause

    float      m_effectiveTimeScale   = 1.f;
    CursorMode m_effectiveCursor      = CursorMode::Inherit;
    bool       m_effectiveAudioPaused = false;
    std::vector<std::string> m_effectiveInputContexts;
    std::vector<std::pair<std::string, float>> m_effectiveBusDucks;

    PropertyBag m_currentParams;
    PropertyBag m_pendingParams;
    std::string m_pendingTrigger;
    float       m_timeInState = 0.f;
    bool        m_timeoutFired = false;

    std::string m_faultStateName;
    bool        m_inFaultDivert = false;   // guards a fault inside the fault path
    std::optional<std::pair<std::string, PropertyBag>> m_queuedRestore;
    mutable bool m_warnedPassThroughWithStates = false;

    GameStateId m_current;
    std::optional<GameStateId> m_pending;
    std::optional<GameStateId> m_initial;
    bool m_inTransition = false;
    bool m_finalized = false;
    bool m_definitionsValid = true;
    // Softens ValidateDefinitions from ERROR to WARNING while re-validating
    // after a post-finalize DefineState, where an unresolved reference is
    // usually just definition order. Diagnostics keep Error severity.
    bool m_validationLogAsWarning = false;
    bool m_traceTransitions = false;
    bool m_replayInitial = false;

    // Pending-slot bookkeeping: what kind of request owns it, and at what
    // priority, so a later request can decide whether it may preempt.
    int  m_pendingPriority = 0;
    bool m_pendingReentry  = false;
    bool m_pendingOverlay  = false;
    std::optional<GameStateId> m_returnTo;   // exactly one; see TryRequestSwitchOverlay

    // Scoped cleanup for the states being EXITED. Private, not the phase
    // buffer, because it is flushed mid-transition -- after every OnExit and
    // before m_current moves -- so the incoming state's OnEnter cannot find the
    // outgoing state's entities still alive. Flushing the shared phase buffer
    // at that point would also replay whatever every other Input-phase system
    // had queued, at a moment none of them chose.
    CommandBuffer m_transitionCommands;

    UnsetSystemPolicy m_unsetPolicy = UnsetSystemPolicy::AlwaysRun;
    SystemSetId       m_unsetDefaultSet;
    std::string       m_unsetDefaultSetName;
    std::vector<std::string> m_unsetSystems;

    std::uint64_t m_frameIndex = 0;
    std::vector<FlowDiagnostic> m_diagnostics;
    std::vector<FlowTransitionRecord> m_history;   // circular, m_historyNext = head
    std::size_t m_historyNext = 0;
};

// Per-state on_update + timeout servicing. Separate from the resolver because
// it belongs in a different phase: the resolver is the frame's fixed transition
// point (Input), while state logic is gameplay logic and must run after the
// scripts that feed it.
class GameFlowTickSystem final : public SystemInPhase<TickPhase::GameplayLogic>
{
public:
    explicit GameFlowTickSystem(GameFlowStateMachine& flow) : m_flow(flow) {}

    const char* GetName() const override { return "GameFlowTickSystem"; }
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    void Update(World& world, const FrameContext& ctx) override;

private:
    GameFlowStateMachine& m_flow;
};

class GameFlowResolveSystem final : public SystemInPhase<TickPhase::Input>
{
public:
    explicit GameFlowResolveSystem(GameFlowStateMachine& flow) : m_flow(flow) {}

    const char* GetName() const override { return "GameFlowResolveSystem"; }
    SystemRunMask RunMask() const override { return SystemRunMask::Play; }
    void Update(World& world, const FrameContext& ctx) override;

private:
    GameFlowStateMachine& m_flow;
};

} // namespace Gameplay
