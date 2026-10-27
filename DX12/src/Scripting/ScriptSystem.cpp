#include "Scripting/ScriptSystem.h"
#include "Scripting/ScriptVM.h"      // -luavmcheck
#include "Scripting/ScriptOwner.h"   // the owner backend, cleared with the system
#include "System/TaskSystem.h"        // -luavmselftest's worker entry
#include <atomic>
#include <thread>
#include "System/Check.h"
#include "System/CommandLine.h"
#include "Diagnostics/ProfilerMemory.h"   // the Lua-heap gauge
#include "Diagnostics/Profiler.h"
#include "Input/InputProfiles.h"
#include "Harness/RunLedger.h"   // script faults are run-level failures
#include "Harness/Determinism.h"  // -seed pins math.random
#include "Module/ModuleRegistry.h"   // the folder scan skips a disabled script module's scripts
#include "Scripting/ScriptManifest.h"
#include "Scripting/ScriptComponent.h"
#include "Scripting/LuaMathTypes.h"
#include "Scripting/LuaDiag.h"     // WarnOnce / ResetOnce
#include "ECS/Components.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/HierarchyComponents.h"
#include "ECS/HierarchyOps.h"          // Hierarchy::DestroySubtree
#include "ECS/AnimationComponents.h"
#include "Gameplay/GameplayTime.h"        // TimeDilationComponent, the notify hit-stop sink
#include "Gameplay/Damage.h"              // the damage pre-hook, cleared with the system
#include "ECS/MotionDirectorComponent.h"  // Engine.GetMotionDirector
#include "ECS/LocomotionComponents.h"     // Engine.HasComponent("Locomotion"/...)
#include "ECS/CharacterAnimDriver.h"      // Engine.HasComponent("AnimDriver")
#include "ECS/CharacterIntentComponent.h" // Engine.HasComponent("Brain")
#include "ECS/CharacterControllerComponent.h"
#include "ECS/TODSystems.h"               // Engine.GetTimeOfDay / IsNight (TODUtil::Find*)
#include "ECS/TODComponents.h"            // TODConfigComponent / TODOutputComponent fields
#include "Resource/MaterialSerializer.h"  // Engine.SetMaterialAsset / ClearMaterialCache
#include "Resource/ComponentSerializers.h" // Engine.AddComponent / HasComponent fallback
#include "UI/UICanvas.h"                   // Engine.SetUIText (Canvas UIText)
#include "UI/UICanvasSystem.h"             // clear the sticky keyboard claim on Stop
#include "UI/WorldSpaceUI.h"               // Engine.SetWorldUITextFont
#include "ECS/CommandAPI.h"
#include "ECS/TagComponent.h"
#include "ECS/GuidRegistry.h"           // Engine.FindByGuid / MakePersistent
#include "ECS/GuidComponent.h"          // PersistentTag + RuntimeSpawnedTag (Engine.MakePersistent)
#include "ECS/Guid.h"                   // Guid::Parse
#include "ECS/NotifyTypes.h"
#include "ECS/DissolveComponent.h"      // Engine.Dissolve / DissolveIn / SetDissolveAmount
#include "Gameplay/GameState.h"         // Engine.SetFlag / AddItem / ...
#include "Gameplay/GameFlowState.h"     // StateChangedEvent -> Lua bus
#include "Input/InputSystem.h"
#include "Input/InputTypes.h"          // GameAction / PadBtn / ActionState (Input.* Lua surface)
#include "System/EventBus.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/HostPolicy.h"   // HotReloadEnabled: authoring host or -hotreload only
#include "System/HotReloadReceipt.h"   // [HOTRELOAD] receipts
#include "Resource/AssetFS.h"
#include "Resource/EngineSettings.h"   // Input.SaveBindings()

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include "ScriptSystem_Internal.h"   // private to src/Scripting, not on the include path
using namespace ScriptInternal;   // RunScriptAsset / GetInstance*

#include <array>
#include <cctype>
#include <chrono>       // math.randomseed at Initialize
#include <cmath>
#include <cstring>
#include <filesystem>
#include <tuple>
#include <algorithm>
#include <mutex>
#include <unordered_set>

using namespace DirectX;
namespace fs = std::filesystem;

namespace
{

    // Device-identity edge memory. Input::LastDevice() is a per-frame poll; scripts
    // want an edge ("the player just picked up the pad: swap every glyph"), so Update
    // publishes InputDeviceChanged only when the device differs from the last one
    // published. File scope, not a member: there is one Input singleton and one
    // ScriptSystem, and ScriptSystem.h stays free of Input (and sol).
    // Re-armed (reset to None) only in ClearAll() / ClearInstances(), the two
    // functions that wipe m_luaBus->subs, so a world reload republishes the current
    // device once to a HUD that re-subscribes in OnInit.
    InputDeviceKind s_lastReportedDevice = InputDeviceKind::None;

    // Day/night phase last announced on the Lua bus as "TimeOfDayPhaseChanged":
    // -1 = unarmed or no enabled TOD authority in the scene, 0 = day, 1 = night.
    // File scope for the same reason as s_lastReportedDevice (ScriptSystem.h stays
    // free of the TOD headers); re-armed in the same two places.
    int s_lastTodPhase = -1;
}


// Per-frame dispatch cache (validation scheme: see ScriptSystem.h). Call sites
// must COPY the sol handles into locals before invoking OnUpdate: the callback can
// destroy its own entity (erasing rows) or reload scripts (bumping the gen)
// mid-call, and a registry-ref copy survives that where a reference into this
// struct does not.
struct ScriptSystem::DispatchCache
{
    struct SysEntry
    {
        uint32_t                gen = 0;   // m_luaDispatchGen at fill
        sol::table              inst;      // __systems[name]
        sol::protected_function upd;       // inst["OnUpdate"]
    };

    struct SlotEntry
    {
        uint32_t                gen       = 0;
        uint32_t                pathHash  = 0;   // FNV32 of the slot's script path
        uint32_t                entityGen = 0;   // World generation at fill
        sol::table              inst;            // __logic_instances[e][slot+1]
        sol::protected_function upd;
    };

    std::vector<SysEntry> systems;         // parallel to m_systems

    std::unordered_map<uint64_t, SlotEntry> slots;  // (entity << 16) | slot

    void Clear() { systems.clear(); slots.clear(); }
};

// Initializer order follows declaration order in the header (m_animEvents is
// declared with the deferred-destroy state, ahead of the other pImpls).
ScriptSystem::ScriptSystem()
    : m_animEvents(std::make_unique<AnimEventQueue>())
    , m_timers    (std::make_unique<TimerQueue>())
    , m_tasks     (std::make_unique<TaskQueue>())
    , m_tweens    (std::make_unique<TweenQueue>())
    , m_luaBus    (std::make_unique<LuaBus>())
{}

ScriptSystem::~ScriptSystem()
{
    // The hit-stop sink, the owner backend and the damage pre-hook capture `this`.
    GameplayTime::SetTimedScaleSink(nullptr);
    Scripting::SetOwnerBackend(nullptr, nullptr);
    Gameplay::Damage::SetPreHook(nullptr);

    // Drop the World listener first: it captures `this`, so a DestroyEntity on the
    // still-alive World after ScriptSystem is gone would crash.
    UnbindWorld();

    // Best-effort scene exit on app quit (OnSceneExit, the scene-scoped Systems' OnShutdown),
    // then OnShutdown on each engine System before tearing down state.
    ExitEnteredScene();
    for (auto& sys : m_systems) TeardownSystem(sys);
    m_systems.clear();

    // Unsubscribe the bridge lambdas before the Lua state goes away: they capture
    // `this` and m_lua.
    for (auto& unsub : m_cppBridgeUnsubscribers) unsub();
    m_cppBridgeUnsubscribers.clear();

    // Drop every container that may hold sol::function / sol::object refs before
    // deleting m_lua: members destruct in reverse declaration order, so their
    // destructors would otherwise run after `delete m_lua` and call luaL_unref on a
    // freed lua_State.
    ClearAll();
    m_dispatchCache.reset();    // holds sol refs: same before-delete rule

    delete m_lua;
}

void ScriptSystem::ClearAll()
{
    m_states.clear();
    if (m_dispatchCache) m_dispatchCache->Clear();
    ++m_luaDispatchGen;
    m_logicTemplates.clear();
    m_services.clear();
    m_uiScripts.clear();
    // Scene script: drop the active instance + flags. OnSceneExit is NOT fired
    // here (ClearAll has drop-without-callback semantics, like services); the
    // dtor runs ExitEnteredScene() first for app quit, and scene-to-scene
    // transitions exit via ExitScene / ProcessSceneScriptSwap.
    m_sceneScriptPending   = false;
    m_sceneScriptEntered   = false;
    m_sceneScriptHasUpdate = false;
    m_sceneScriptHasExit   = false;
    m_sceneScriptPath.clear();
    m_sceneScriptName.clear();
    m_pendingSceneScriptPath.clear();
    m_pendingSceneScriptName.clear();
    m_sceneSystems.clear();
    m_pendingSceneSystems.clear();
    m_sceneScriptOwner = 0;
    m_playSessionOpen  = false;
    if (m_lua) (*m_lua)["__scene_script"] = sol::lua_nil;
    // m_systems is not cleared here: the destructor runs TeardownSystem (OnShutdown)
    // first, then clears. A live caller that wants shutdown semantics must call
    // TeardownSystem itself.
    if (m_luaBus) { m_luaBus->subs.clear(); m_luaBus->pending.clear(); }
    CancelTasks(0, /*all=*/true);
    if (m_timers) m_timers->entries.clear();
    if (m_tweens) m_tweens->entries.clear();
    m_pendingDestroys.clear();
    if (m_animEvents) m_animEvents->pending.clear();
    LuaDiag::ResetOnce();   // each session gets one warning per site again
    // Every owned resource went wholesale above; System records stay (their entries do).
    ForgetOwnersOfKind(OwnerKind::Logic);
    ForgetOwnersOfKind(OwnerKind::SceneScript);
    ForgetOwnersOfKind(OwnerKind::World);
    m_worldOwner = 0;
    m_contextOwners.clear();
    for (const OwnerReleaseHook& hook : m_ownerReleaseHooks) hook(0, true);
    m_timeRequests.clear();
    m_legacyTimeScale = 1.f;
    RefreshTimeScale();
    // The next tick puts every slowed entity back to 1 (the world outlives a reload).
    m_entityTimeRequests.clear();
    m_entityTimeDirty = true;
    // The subs wipe above took every "InputDeviceChanged" listener with it: re-arm
    // the device edge so the next subscriber is told the current device once instead
    // of waiting for a switch that, for a pad-only player, never comes.
    s_lastReportedDevice = InputDeviceKind::None;
    // Same deal for the day/night edge: the new scene may already be at night,
    // and a lamp that only listens for the transition would never hear one.
    s_lastTodPhase = -1;
    // Mapping contexts are pushed from Lua and nothing tracks who pushed them: a
    // scene unloaded mid-menu ("Menu" with blockLower) would leave every gameplay
    // action UNBOUND with no script left to pop it. Deactivate them here with the
    // other Lua-owned input state; definitions survive (ClearContexts keeps them), so
    // a reload that re-pushes works without re-running InputContexts.lua.
    Input::Get().ClearContexts();
    InputProfiles::Registry::Get().Deactivate();
    // m_lua is kept: the bindings stay valid.
}

void ScriptSystem::ClearInstances()
{
    // Drop Logic instances without OnDestroy: the editor's snapshot restore discards
    // the whole play world, so this is teardown, not gameplay death. Their Lua
    // instance tables are overwritten by the next EnsureLogicInstance or collected.
    m_states.clear();
    if (m_dispatchCache) m_dispatchCache->Clear();
    ++m_luaDispatchGen;
    // Transient play-session runtime queues.
    if (m_luaBus) { m_luaBus->subs.clear(); m_luaBus->pending.clear(); }
    CancelTasks(0, /*all=*/true);
    if (m_timers) m_timers->entries.clear();
    if (m_tweens) m_tweens->entries.clear();
    m_pendingDestroys.clear();   // queued against the discarded play world
    if (m_animEvents) m_animEvents->pending.clear();   // same reason
    LuaDiag::ResetOnce();        // ...and re-arm the warn-once sites
    // The owned resources went wholesale above: forget the per-instance owners. System owners
    // stay, because their entries do.
    ForgetOwnersOfKind(OwnerKind::Logic);
    ForgetOwnersOfKind(OwnerKind::SceneScript);
    ForgetOwnersOfKind(OwnerKind::World);
    m_worldOwner = 0;
    m_contextOwners.clear();
    for (const OwnerReleaseHook& hook : m_ownerReleaseHooks) hook(0, true);
    // A Lua hit-stop or pause may have left a non-1 time scale; reset for the editor.
    m_timeRequests.clear();
    m_legacyTimeScale = 1.f;
    RefreshTimeScale();
    m_entityTimeRequests.clear();
    m_entityTimeDirty = true;
    // The subs wipe also removed every System's Engine.Subscribe handlers: re-arm
    // OnInit so the next Play re-subscribes. Re-derive hasUpdate too: Update() clears
    // it when OnUpdate throws, and Stop/Play is the reflex that must revive the
    // system. System tables persist (engine lifetime).
    for (auto& sys : m_systems)
    {
        sys.initCalled = false;
        // A scene System's methods live on its prototype; __systems holds only an activation.
        const char* table = sys.sceneScoped ? "__system_protos" : "__systems";
        if (sys.loaded)
            sys.hasUpdate = (*m_lua)[table][sys.name].valid()
                         && (*m_lua)[table][sys.name]["OnUpdate"].valid();
    }
    // Clear loadFailed too (parked slots went with m_states above; templates are
    // engine lifetime). Nothing re-runs the file: a template whose first load failed
    // stays without a prototype.
    for (auto& [tpath, tmpl] : m_logicTemplates)
        tmpl.loadFailed = false;
    // Re-arm the input device edge: the wiped subs included every
    // "InputDeviceChanged" listener, so seed the memory with a value no real
    // device ever has and let the next Update republish once the re-subscribed
    // HUD is actually listening (see the gate in Update()).
    s_lastReportedDevice = InputDeviceKind::None;
    s_lastTodPhase       = -1;   // same reasoning, for the day/night edge
    // Re-arm the scene script the same way: TickSceneScript clears
    // m_sceneScriptHasUpdate when OnSceneUpdate throws, and otherwise only
    // ProcessSceneScriptSwap (a real scene load) re-arms it. Re-derived from the live
    // table, so a hot-reloaded script that gained or lost a handler is picked up.
    if (m_lua)
    {
        // sol::optional, not sol::table: converting a proxy to sol::table is a checked
        // conversion, and __scene_script is nil for any scene without a scene script (and
        // after FireSceneExitIfEntered while m_sceneScriptHas* stay set). Converting nil
        // raises a Lua error that sol's panic handler turns into a C++ throw across the
        // Lua/C boundary, ending in std::terminate. sol::optional type-tests without
        // raising.
        sol::optional<sol::table> sceneInst = (*m_lua)["__scene_script"];
        if (sceneInst)
        {
            m_sceneScriptHasUpdate = (*sceneInst)["OnSceneUpdate"].valid();
            m_sceneScriptHasExit   = (*sceneInst)["OnSceneExit"].valid();
        }
    }
    // Same as ClearAll: the UI script that pushed a context is discarded with the
    // play world, so nothing would pop it and the next Play would start with the pad
    // silenced. Definitions are kept (engine lifetime).
    Input::Get().ClearContexts();
    InputProfiles::Registry::Get().Deactivate();
    // Same for the Canvas keyboard claim: a sticky process global that only Lua
    // clears (a key-rebind cell arms it, its Close releases it), and that Close never
    // runs on Stop, so the editor would swallow every keystroke for the rest of the
    // session.
    UI::UICanvasSystem::SetKeyboardCaptureRequest(false);
    // Templates, services and UI scripts (engine lifetime) and the scene-script
    // binding (scene lifetime) are kept.
}

// Drops every Logic instance (and tweens and queued destroys, which target
// old-world ids) when the world epoch moved; see m_lastWorldEpoch. No OnDestroy:
// the new scene already owns those ids, but what each instance owned is released.
// System tables, their subscriptions and timers and the scene-script binding are kept
// (ExitScene / ProcessSceneScriptSwap handle the scene's own).
void ScriptSystem::DropInstancesForWorldSwap(World& world)
{
    const uint32_t epoch = world.Epoch();
    if (epoch == m_lastWorldEpoch) return;
    m_lastWorldEpoch = epoch;

    if (!m_states.empty())
        LOG_INFO("ScriptSystem: world epoch %u -> dropping %zu stale Logic instance set(s) "
                 "left by the previous world", epoch, m_states.size());
    DropLogicInstances();
    if (m_tweens) m_tweens->entries.clear();
    m_pendingDestroys.clear();
}

void ScriptSystem::DropLogicInstances()
{
    // What ended slots handed over goes with their world.
    ReleaseOwner(m_worldOwner);
    m_worldOwner = 0;
    if (m_states.empty()) return;
    for (const auto& [e, st] : m_states)
        for (const LogicSlot& slot : st.slots)
            ReleaseOwner(slot.owner);
    m_states.clear();
    if (m_dispatchCache) m_dispatchCache->Clear();
    ++m_luaDispatchGen;
}

// ==== Scene script ====
void ScriptSystem::SetActiveSceneScript(const std::string& path,
                                        const std::string& sceneName,
                                        const std::vector<std::string>& sceneSystems)
{
    // Queued: ProcessSceneScriptSwap runs the swap on the next Update.
    m_pendingSceneScriptPath = path;
    m_pendingSceneScriptName = sceneName;
    m_pendingSceneSystems    = sceneSystems;
    m_sceneScriptPending     = true;
}

void ScriptSystem::RebindScene(const std::string& path, const std::vector<std::string>& sceneSystems)
{
    if (m_playSessionOpen)
    {
        m_playScenePath    = path;
        m_playSceneSystems = sceneSystems;
        return;
    }
    const std::string name = m_sceneScriptPending ? m_pendingSceneScriptName : m_sceneScriptName;
    SetActiveSceneScript(path, name, sceneSystems);
}

void ScriptSystem::ExitScene(bool worldSwap)
{
    ExitEnteredScene();
    if (worldSwap)
    {
        // The same drop DropInstancesForWorldSwap makes on the next Update, made now so a Logic
        // timer cannot fire into the new world from the ScriptTimerSystem tick that precedes it.
        DropLogicInstances();
        if (m_tweens) m_tweens->entries.clear();
        m_pendingDestroys.clear();
        if (m_animEvents) m_animEvents->pending.clear();
    }
}

void ScriptSystem::BeginPlaySession()
{
    // Play always starts from a queued binding: the editor's scene load queues one, and
    // EndPlaySession queues this one again.
    m_playScenePath    = m_sceneScriptPending ? m_pendingSceneScriptPath : m_sceneScriptPath;
    m_playSceneName    = m_sceneScriptPending ? m_pendingSceneScriptName : m_sceneScriptName;
    m_playSceneSystems = m_sceneScriptPending ? m_pendingSceneSystems    : m_sceneSystems;
    m_playSessionOpen  = true;
}

void ScriptSystem::EndPlaySession()
{
    ExitEnteredScene();
    if (!m_playSessionOpen) return;
    m_playSessionOpen = false;
    SetActiveSceneScript(m_playScenePath, m_playSceneName, m_playSceneSystems);
}

void ScriptSystem::ExitEnteredScene()
{
    FireSceneExitIfEntered();
    DeactivateSceneSystems();
    m_sceneScriptPath.clear();
    m_sceneScriptName.clear();
    m_sceneScriptHasUpdate = false;
    m_sceneScriptHasExit   = false;
}

void ScriptSystem::FireSceneExitIfEntered()
{
    if (!m_sceneScriptEntered || !m_lua)
    {
        m_sceneScriptEntered = false;
        ReleaseOwner(m_sceneScriptOwner);
        m_sceneScriptOwner = 0;
        return;
    }

    if (m_sceneScriptHasExit)
    {
        // OnSceneExit routinely touches world bindings (despawn UI, save state).
        // Those guard on m_world, which Update sets only for its own duration;
        // when exiting from the dtor / a teardown path m_world is null, so
        // borrow the persistently-bound world like TickTimers does.
        WorldBorrow borrow(*this);

        // Same checked-conversion trap as ClearInstances above: m_sceneScriptHasExit
        // is a cached flag, and nothing keeps it in step with the Lua global, so
        // "has exit but the global is nil" is reachable. Ask without raising.
        sol::optional<sol::table> instOpt = (*m_lua)["__scene_script"];
        if (instOpt)
        {
            sol::table inst = *instOpt;
            sol::protected_function exit = inst["OnSceneExit"];
            if (exit.valid())
            {
                OwnerScope as(*this, m_sceneScriptOwner);
                auto res = exit(inst);
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua OnSceneExit error [%s]: %s",
                              m_sceneScriptName.c_str(), err.what());
                }
            }
        }
        // m_world restored by ~WorldBorrow.
    }

    m_sceneScriptEntered = false;
    (*m_lua)["__scene_script"] = sol::lua_nil;
    // What OnSceneExit did not unsubscribe or cancel goes now.
    ReleaseOwner(m_sceneScriptOwner);
    m_sceneScriptOwner = 0;
}

void ScriptSystem::ProcessSceneScriptSwap()
{
    if (!m_sceneScriptPending) return;
    m_sceneScriptPending = false;

    // A scene whose replacement was only queued (no ExitScene first) exits here, late: the world
    // it sees is already the new one.
    ExitEnteredScene();

    m_sceneScriptPath      = m_pendingSceneScriptPath;
    m_sceneScriptName      = m_pendingSceneScriptName;
    m_sceneScriptEntered   = false;
    m_sceneScriptHasUpdate = false;
    m_sceneScriptHasExit   = false;
    (*m_lua)["__scene_script"] = sol::lua_nil;

    // Before OnSceneEnter, so the scene script can configure its Systems.
    ActivateSceneSystems(m_pendingSceneSystems);

    if (m_sceneScriptPath.empty()) return;   // the new scene has no scene script

    // The file's top level and every hook run as the scene script's owner, so what it leaves
    // behind is released at its exit.
    m_sceneScriptOwner = NewOwner(OwnerKind::SceneScript, m_sceneScriptPath,
                                  "scene:" + m_sceneScriptPath);
    OwnerScope as(*this, m_sceneScriptOwner);

    // A scene script is re-run from source on every scene entry, so the returned
    // table is the single instance (no template or metatable, unlike per-entity Logic).
    auto result = RunScriptAsset(*m_lua, m_sceneScriptPath);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: scene script load error [%s]: %s",
                  m_sceneScriptPath.c_str(), err.what());
        m_sceneScriptPath.clear();
        ReleaseOwner(m_sceneScriptOwner);
        m_sceneScriptOwner = 0;
        return;
    }
    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: scene script [%s] did not return a table",
                  m_sceneScriptPath.c_str());
        m_sceneScriptPath.clear();
        ReleaseOwner(m_sceneScriptOwner);
        m_sceneScriptOwner = 0;
        return;
    }

    sol::table inst = ret.as<sol::table>();
    (*m_lua)["__scene_script"] = inst;
    m_sceneScriptHasUpdate = inst["OnSceneUpdate"].valid();
    m_sceneScriptHasExit   = inst["OnSceneExit"].valid();

    sol::protected_function enter = inst["OnSceneEnter"];
    if (enter.valid())
    {
        auto res = enter(inst, m_sceneScriptName);
        if (!res.valid())
        {
            sol::error err = res;
            LOG_ERROR("Lua OnSceneEnter error [%s]: %s",
                      m_sceneScriptName.c_str(), err.what());
        }
    }
    m_sceneScriptEntered = true;
    LOG_INFO("ScriptSystem: scene script '%s' entered (scene='%s', OnSceneUpdate=%d OnSceneExit=%d)",
             m_sceneScriptPath.c_str(), m_sceneScriptName.c_str(),
             m_sceneScriptHasUpdate, m_sceneScriptHasExit);
}

void ScriptSystem::TickSceneScript(float dt)
{
    if (!m_sceneScriptEntered || !m_sceneScriptHasUpdate) return;
    // Same checked-conversion trap as ClearInstances; this one runs per frame,
    // so a stale m_sceneScriptHasUpdate over a nil global would take the process
    // down mid-play rather than on Stop.
    sol::optional<sol::table> instOpt = (*m_lua)["__scene_script"];
    if (!instOpt) return;
    sol::table inst = *instOpt;
    sol::protected_function upd = inst["OnSceneUpdate"];
    if (!upd.valid()) { m_sceneScriptHasUpdate = false; return; }
    OwnerScope as(*this, m_sceneScriptOwner);
    auto res = upd(inst, dt);
    if (!res.valid())
    {
        sol::error err = res;
        LOG_ERROR("Lua OnSceneUpdate error [%s]: %s", m_sceneScriptName.c_str(), err.what());
        m_sceneScriptHasUpdate = false; // stop calling broken update
    }
}

// Drains destroys queued inside deferral windows (physics contact dispatch, BT
// ticks); called by TickTimers before any timer fires and by Update before the
// sweep. The generation captured at queue time guards against killing an entity
// that recycled the id.
void ScriptSystem::FlushPendingDestroys()
{
    if (m_pendingDestroys.empty() || !m_world) return;
    // Swap out first: DestroyEntity fires listeners that may run Lua, which
    // may queue MORE destroys (picked up at the next flush point).
    std::vector<PendingDestroy> pending;
    std::swap(pending, m_pendingDestroys);
    for (const PendingDestroy& p : pending)
    {
        if (!m_world->IsAlive(p.e)) continue;
        if ((m_world->GetGeneration(p.e) & LuaId::kGenMask) != p.gen) continue;
        // Subtree, matching the immediate Engine.DestroyEntity path: freeing only the
        // root would orphan the children. Resolved here, not at queue time, so a reparent
        // inside the deferral window is respected.
        Hierarchy::DestroySubtree(*m_world, p.e);
    }
}

// Game-dt tweens; called from Update() with m_world valid.
void ScriptSystem::TickTweens(float dt)
{
    if (!m_tweens || m_tweens->entries.empty()) return;

    // Index-based loop with snapshot-then-write: an onDone callback may push
    // new tweens (replace/chain) which can reallocate `entries`, so we never
    // hold a reference across the callback. New entries appended during this
    // pass are skipped until next frame (count is snapshotted).
    const std::size_t count = m_tweens->entries.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        bool                finished = false;
        Entity              e{};
        TweenQueue::Channel ch{};
        XMFLOAT4            from{}, to{};
        float               k = 0.f;
        {
            auto& tw = m_tweens->entries[i];
            if (!tw.active) continue;
            tw.elapsed += dt;
            float t = tw.duration > 0.f ? tw.elapsed / tw.duration : 1.f;
            if (t >= 1.f) { t = 1.f; finished = true; }
            e = tw.entity; ch = tw.channel; from = tw.from; to = tw.to;
            k = TweenQueue::Apply(tw.easing, t);
        }

        LocalTransform* lt = m_world ? m_world->GetComponent<LocalTransform>(e) : nullptr;
        if (lt)
        {
            switch (ch)
            {
                case TweenQueue::Channel::Translation:
                    lt->translation = { from.x + (to.x - from.x) * k,
                                        from.y + (to.y - from.y) * k,
                                        from.z + (to.z - from.z) * k };
                    break;
                case TweenQueue::Channel::Scale:
                    lt->scale = { from.x + (to.x - from.x) * k,
                                  from.y + (to.y - from.y) * k,
                                  from.z + (to.z - from.z) * k };
                    break;
                case TweenQueue::Channel::Rotation:
                {
                    const XMVECTOR qr = XMQuaternionSlerp(
                        XMLoadFloat4(&from), XMLoadFloat4(&to), k);
                    XMStoreFloat4(&lt->rotation, qr);
                    break;
                }
            }
        }

        if (finished)
        {
            // Deactivate and copy the callback out before invoking: it may push tweens
            // (reallocating entries) or reuse this now-inactive slot.
            sol::protected_function fn;
            OwnerId                 owner = 0;
            {
                auto& tw  = m_tweens->entries[i];
                tw.active = false;
                fn        = tw.onDone;
                owner     = tw.owner;
            }
            if (fn.valid())
            {
                OwnerScope as(*this, owner);
                auto res = fn();
                if (!res.valid())
                {
                    sol::error err = res;
                    LOG_ERROR("Lua tween onDone error: %s", err.what());
                }
            }
        }
    }

    // Compact when more than half the entries are dead, as TickTimers does.
    std::size_t dead = 0;
    for (const auto& e : m_tweens->entries) if (!e.active) ++dead;
    if (dead * 2 > m_tweens->entries.size() && !m_tweens->entries.empty())
    {
        std::vector<TweenQueue::Entry> live;
        live.reserve(m_tweens->entries.size() - dead);
        for (auto& e : m_tweens->entries) if (e.active) live.push_back(std::move(e));
        m_tweens->entries = std::move(live);
    }
}

// ==== Lua VM helpers ====
namespace {

// -luavmcheck: a LUA_MASKCALL hook that tests the VM's thread contract
// (Scripting/ScriptVM.h) on every entry into a Lua function, from any thread.
void LuaVMCheckHook(lua_State*, lua_Debug*)
{
    Sys::ScriptVM::Checks().fetch_add(1, std::memory_order_relaxed);
    if (Sys::ScriptVM::HeldByThisThread()) return;
    Sys::ScriptVM::Violations().fetch_add(1, std::memory_order_relaxed);
    ENGINE_CHECK_ONCE(false,
        "[LUAVM] Lua entered from a thread that is neither the VM's owner nor "
        "holds a kScriptVM grant - the single sol::state is not thread-safe");
}

// Message handler of every protected call on the VM (installed in Initialize):
// appends a stack traceback to the error message.
int LuaTracebackHandler(lua_State* L)
{
    const char* msg = lua_tostring(L, 1);
    if (!msg)
    {
        // Non-string error object (error({code=1})). Use its __tostring if it
        // has one, otherwise describe the type so it is not just "unknown".
        if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING)
            return 1;
        lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
        msg = lua_tostring(L, -1);
    }
    luaL_traceback(L, L, msg, 1);   // pushes "msg\nstack traceback:\n\t..."
    return 1;
}

// The collector mode for the one VM, generational by default
// (docs/perf/lua_gc_policy.md): script garbage is short-lived temporaries over a
// small (~1 MB) live set. With the value usertypes' missing __gc (Scripting/LuaGc.h;
// without it neither mode bounds an idle heap) that gives ~0.12 ms minor collections
// instead of a ~1.1 ms incremental cycle inside one frame, and an idle heap within
// 1.2x its live size (incremental: 2x).
// -luagc=incremental | generational picks the mode for a run; any other value (a
// bare -luagc included) fails the check and runs the default. The 0 arguments keep
// each mode's defaults: generational a minor collection every 20 % of growth and a
// major one past 100 % over the last major; incremental pause 200 %, step
// multiplier 100, step 2^13 bytes.
void ApplyLuaGcMode(lua_State* L)
{
    const auto& cl = Sys::CommandLine::Get();
    constexpr const char* kDefault = "generational";
    const std::string mode = cl.Has("-luagc") ? cl.Get("-luagc", "") : std::string(kDefault);
    const bool known = mode == "incremental" || mode == "generational";
    ENGINE_CHECK(known, "-luagc=%s is not a Lua GC mode (generational | incremental); running %s",
                 mode.c_str(), kDefault);
    const std::string use = known ? mode : std::string(kDefault);
    if (use == "generational") lua_gc(L, LUA_GCGEN, 0, 0);
    else                       lua_gc(L, LUA_GCINC, 0, 0, 0);
    LOG_INFO("[LUAGC] mode=%s", use.c_str());
}

} // namespace

void ScriptSystem::Initialize()
{
    // An animation notify's hit-stop (CameraEffectSystem) joins the Lua requests: timed, under the
    // world owner, so replacing the world releases it with the rest.
    GameplayTime::SetTimedScaleSink([this](float scale, float realSeconds)
    {
        m_timeRequests.push_back(TimeRequest{ ++m_nextTimeRequestId, WorldOwner(), scale, realSeconds });
        RefreshTimeScale();
    });

    m_lua = new sol::state();
    // The VM belongs to the thread that created it (main); any other thread needs a
    // Sys::ScriptVM::Grant. -luavmcheck enforces it.
    Sys::ScriptVM::SetOwner(std::this_thread::get_id());
    if (Sys::CommandLine::Get().Has("-luavmcheck"))
    {
        lua_sethook(m_lua->lua_state(), &LuaVMCheckHook, LUA_MASKCALL, 0);
        LOG_INFO("[LUAVM] check armed: every Lua call tests the owner / kScriptVM-grant contract");
        // -luavmselftest: the check's POSITIVE CONTROL. Enter the VM exactly
        // once from a TaskSystem worker - Push, not ParallelFor, whose caller
        // may run the chunk itself - while main waits and touches nothing, so
        // there is no real concurrency on the VM. The hook must count ONE.
        if (Sys::CommandLine::Get().Has("-luavmselftest"))
        {
            lua_State* L = m_lua->lua_state();
            std::atomic<int> done{0};
            TaskSystem::Get().Push([L, &done] {
                luaL_dostring(L, "local x = 1 + 1");
                done.store(1, std::memory_order_release);
            }, TaskSystem::TaskPriority::High);
            while (done.load(std::memory_order_acquire) == 0) std::this_thread::yield();
            LOG_INFO("[LUAVM] selftest: one worker entry, violations=%llu",
                     static_cast<unsigned long long>(Sys::ScriptVM::Violations().load()));
        }
    }
    // coroutine is opened for sequenced gameplay (cutscene beats, "walk, wait,
    // talk", multi-step abilities): Engine.AfterDelay only chains callbacks and the
    // Sequencer is asset-driven. Safe with Update()'s re-entrancy rules: a coroutine
    // resumed from a callback runs on the same lua_State and returns before the
    // callback does. A yield inside Lua that a binding runs synchronously (OnDestroy
    // fired by Engine.DestroyEntity) would cross a C-call boundary: Lua raises an
    // error there.
    // Not opened: `os` (non-deterministic time invites frame-rate bugs), `package` /
    // `require` (bypasses AssetFS; see Engine.Import), `debug` (the traceback handler
    // needs only lauxlib).
    m_lua->open_libraries(sol::lib::base, sol::lib::math, sol::lib::string,
                          sol::lib::table, sol::lib::io, sol::lib::coroutine);
    ApplyLuaGcMode(m_lua->lua_state());

    // Install LuaTracebackHandler as the message handler of every protected call
    // before any binding registers, so every protected_function built later picks it
    // up with no call-site changes. Without it a failed pcall yields only the
    // innermost "chunk:line: message" with the stack already unwound, so an error
    // inside a shared Service says nothing about which script or entity called it.
    // luaL_traceback is lauxlib, so it works without sol::lib::debug.
    {
        lua_State* L = m_lua->lua_state();
        lua_pushcfunction(L, &LuaTracebackHandler);
        sol::reference handlerRef(L, -1);   // refs the value at the top
        lua_pop(L, 1);                      // ...which does not pop it
        // sol2 3.x keeps this in detail:: and reads it back per protected call
        // from the global named sol::detail::default_handler_name().
        sol::detail::set_default_handler(L, handlerRef);
    }

    // Seed math.random from the steady clock so launches differ (services/Damage.lua
    // rolls damage with it), except under -seed (Harness/Determinism.h), which
    // promises exactly this stream: AI decisions draw from it (enemy_actions.lua), and
    // a clock seed would make a -fixeddt -seed run diverge and an input replay drift
    // (tests/unit/test_ai_replay.py).
    {
        const auto& det = Harness::Determinism::Get();
        const lua_Integer seedValue = det.HasSeed()
            ? static_cast<lua_Integer>(det.Seed())
            : static_cast<lua_Integer>(std::chrono::steady_clock::now().time_since_epoch().count());
        sol::protected_function seed = (*m_lua)["math"]["randomseed"];
        if (seed.valid()) seed(seedValue);
        if (det.HasSeed())
            LOG_INFO("ScriptSystem: math.random seeded from -seed=%llu",
                     static_cast<unsigned long long>(det.Seed()));
    }

    RegisterBindings();
    InstallMethodWrapper();
    RegisterCppEventBridges();
    RegisterDamageBindings();
    LOG_SUCCESS("ScriptSystem: Lua %s initialized", LUA_RELEASE);
}

// LoadSystem wraps each function field of a System table so a call from another script
// (Engine.GetSystem("HitStop"):Trigger()) runs as the System: what the method subscribes, schedules
// or requests is the System's to release, not the caller's. A System that is not active charges
// the caller instead. The call runs under xpcall so an error restores the caller's owner before it
// is raised again (with the traceback of where it happened); a method that yields keeps the System
// as owner until it returns. The C hooks reach the Lua code as upvalues only: they are not API.
void ScriptSystem::InstallMethodWrapper()
{
    sol::state& lua = *m_lua;
    sol::table hooks = lua.create_table();
    hooks.set_function("enter", [this](const std::string& systemName) -> OwnerId
    {
        const OwnerId prev = m_currentOwner;
        if (const OwnerId owner = SystemOwner(systemName); owner != 0) m_currentOwner = owner;
        return prev;
    });
    hooks.set_function("leave", [this](OwnerId prev) { m_currentOwner = prev; });
    hooks["traceback"] = &LuaTracebackHandler;

    auto factory = lua.safe_script(R"lua(
return function(enter, leave, traceback)
    local function finish(prev, ok, ...)
        leave(prev)
        if not ok then error((...), 0) end
        return ...
    end
    return function(t, name)
        for k, v in pairs(t) do
            if type(v) == "function" then
                t[k] = function(...)
                    local prev = enter(name)
                    return finish(prev, xpcall(v, traceback, ...))
                end
            end
        end
    end
end
)lua", sol::script_pass_on_error, "@ScriptSystem.owner_wrap");
    if (!factory.valid())
    {
        sol::error err = factory;
        LOG_ERROR("ScriptSystem: method-ownership wrapper failed to load: %s", err.what());
        return;
    }
    sol::protected_function make = factory;
    auto wrap = make(hooks["enter"], hooks["leave"], hooks["traceback"]);
    if (!wrap.valid())
    {
        sol::error err = wrap;
        LOG_ERROR("ScriptSystem: method-ownership wrapper failed to build: %s", err.what());
        return;
    }
    lua["__ScriptWrapMethods"] = wrap.get<sol::object>();
}

// ==== Exposed variables ====
// File-local helpers that read a script's `exposed` table into ScriptVarDesc; the
// sol-typed code stays out of the header.
namespace
{
    // Read a 3-float vector from a Lua value shaped as an array {x,y,z}, a
    // keyed {x=,y=,z=}, or a color {r=,g=,b=}. Returns false if not vec-shaped.
    bool ReadExposedVec3(const sol::object& o, float out[3])
    {
        if (o.get_type() != sol::type::table) return false;
        sol::table t = o.as<sol::table>();
        sol::optional<float> a1 = t[1], a2 = t[2], a3 = t[3];
        if (a1 && a2 && a3) { out[0]=*a1; out[1]=*a2; out[2]=*a3; return true; }
        sol::optional<float> x = t["x"], y = t["y"], z = t["z"];
        if (x && y && z) { out[0]=*x; out[1]=*y; out[2]=*z; return true; }
        sol::optional<float> r = t["r"], g = t["g"], b = t["b"];
        if (r && g && b) { out[0]=*r; out[1]=*g; out[2]=*b; return true; }
        return false;
    }

    // Build a default ScriptVarValue of @p type from a Lua `default` object (nil -> a
    // type-appropriate zero / empty value).
    ScriptVarValue ReadExposedDefault(ScriptVarType type, const sol::object& def)
    {
        switch (type)
        {
        case ScriptVarType::Float:
            return ScriptVarValue::MakeFloat(def.is<double>() ? (float)def.as<double>() : 0.f);
        case ScriptVarType::Int:
            return ScriptVarValue::MakeInt(def.is<double>() ? (int32_t)def.as<double>() : 0);
        case ScriptVarType::Bool:
            return ScriptVarValue::MakeBool(def.is<bool>() ? def.as<bool>() : false);
        case ScriptVarType::Float3:
        case ScriptVarType::Color:
        {
            float v[3] = { 0.f, 0.f, 0.f };
            ReadExposedVec3(def, v);
            return type == ScriptVarType::Color
                ? ScriptVarValue::MakeColor (v[0], v[1], v[2])
                : ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
        }
        case ScriptVarType::Entity:
            return ScriptVarValue::MakeEntity(def.is<double>() ? (uint32_t)def.as<double>() : 0u);
        case ScriptVarType::String:
            return ScriptVarValue::MakeString(def.is<std::string>() ? def.as<std::string>() : std::string());
        case ScriptVarType::Asset:
            return ScriptVarValue::MakeAsset(def.is<std::string>() ? def.as<std::string>() : std::string());
        }
        return ScriptVarValue::MakeFloat(0.f);
    }
}

// Loads a .lua file as a Logic prototype: the file must `return T`, a table with
// any subset of OnSpawn / OnUpdate / OnDestroy. Instances inherit from it through
// metatable __index.
bool ScriptSystem::LoadLogicTemplate(const std::string& path)
{
    auto& tmpl = m_logicTemplates[path];
    tmpl.path = path;

    // Do not clear hasSpawn / hasUpdate / hasDestroy or replace
    // __logic_templates[path] before the load succeeds: those flags gate OnUpdate
    // (Update) and OnDestroy (FireSlotDestroy), so a typo mid-session would freeze
    // every live instance and skip their OnDestroy cleanup. On failure the last-good
    // template keeps running and only lastError is recorded.
    auto result = RunScriptAsset(*m_lua, path);
    if (!result.valid())
    {
        sol::error err = result;
        tmpl.lastError  = err.what();
        tmpl.loadFailed = true;
        LOG_ERROR("ScriptSystem: logic load error [%s]: %s (keeping last-good template)",
                  path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        tmpl.lastError  = "Logic script must `return T` where T is a table.";
        tmpl.loadFailed = true;
        LOG_ERROR("ScriptSystem: logic [%s] did not return a table (keeping last-good template)",
                  path.c_str());
        return false;
    }

    sol::table t = ret.as<sol::table>();
    (*m_lua)["__logic_templates"][path] = t;

    tmpl.lastError.clear();
    tmpl.loadFailed = false;
    tmpl.hasSpawn   = t["OnSpawn"].valid();
    tmpl.hasUpdate  = t["OnUpdate"].valid();
    tmpl.hasDestroy = t["OnDestroy"].valid();

    // Optional GameFlow SystemSet membership, same `set = "..."` spelling a
    // global System table uses. Most of this project's gameplay logic lives in
    // Logic scripts, so without this a Paused state could gate every C++ system
    // and still leave the game's actual behaviour running.
    tmpl.systemSet = t.get_or("set", std::string{});
    if (!tmpl.systemSet.empty() && m_registerSystemSet)
        m_registerSystemSet(tmpl.systemSet);

    // Parse the editor-exposed variable schema (`T.exposed`), if any.
    ParseExposedSchema(path, tmpl.exposed);

    try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded logic template [%s] (OnSpawn=%d OnUpdate=%d OnDestroy=%d set='%s')",
             path.c_str(), tmpl.hasSpawn, tmpl.hasUpdate, tmpl.hasDestroy,
             tmpl.systemSet.c_str());
    return true;
}

// Returns false (the slot stays uninitialised and is retried next frame) only when
// the template has no prototype; an OnSpawn error is logged and still returns true.
bool ScriptSystem::EnsureLogicInstance(Entity e, std::size_t slot, const std::string& path,
                                       const std::unordered_map<std::string, ScriptVarValue>* overrides)
{
    auto tmplIt = m_logicTemplates.find(path);
    if (tmplIt == m_logicTemplates.end())
    {
        LOG_WARNING("ScriptSystem: no template entry for logic [%s] (entity %u slot %zu)",
                    path.c_str(), e, slot);
        return false;
    }

    sol::table tmpl = (*m_lua)["__logic_templates"][path];
    if (!tmpl.valid())
    {
        // Reached for a file whose first load failed once ClearInstances has cleared
        // loadFailed: the entry exists (operator[]) but no prototype was installed. Warn
        // rather than silently `continue`.
        LOG_WARNING("ScriptSystem: logic [%s] has no loaded prototype (entity %u slot %zu)%s",
                    path.c_str(), e, slot,
                    tmplIt->second.lastError.empty() ? "" : " — see the load error above");
        return false;
    }

    // A fresh instance is about to replace whatever lived at this (e, slot):
    // drop the dispatch-cache row NOW. A remove-then-re-add of the SAME script
    // at the SAME slot passes every self-validation check (same path hash,
    // same entity generation), so without this the cached registry ref keeps
    // the ZOMBIE table alive and OnUpdate runs against divorced state.
    if (m_dispatchCache)
        m_dispatchCache->slots.erase((uint64_t(e) << 16) | uint64_t(slot & 0xFFFFu));

    sol::table inst = m_lua->create_table();
    sol::table mt   = m_lua->create_table();
    mt["__index"]   = tmpl;
    inst[sol::metatable_key] = mt;

    // Convenience: every instance has self.entity and self.slot pre-set so a
    // script can tell which entity it is bound to and which slot it occupies.
    // self.entity carries the generation (LuaEntityId.h) so a cached copy of
    // it can never silently alias a recycled slot.
    inst["entity"] = LuaId::OutMaybe(m_world, e);
    inst["slot"]   = static_cast<uint32_t>(slot);

    sol::table arr = GetInstanceArray(*m_lua, static_cast<uint32_t>(e), /*create*/ true);
    arr[slot + 1] = inst;

    // Inject editor-exposed variables (schema defaults + per-slot overrides)
    // onto the instance table BEFORE OnSpawn so the script reads them as
    // self.<name> on its very first tick.
    InjectExposedVars(e, slot, path, overrides);

    // One owner per instance, recorded before OnSpawn runs (it may subscribe, or destroy e).
    OwnerId owner = 0;
    if (auto sIt = m_states.find(e); sIt != m_states.end() && slot < sIt->second.slots.size())
    {
        LogicSlot& ls = sIt->second.slots[slot];
        // A previous instance at this slot that was never torn down.
        ReleaseOwner(ls.owner, /*handOverDeferred=*/true);
        ls.owner = owner = NewOwner(OwnerKind::Logic, path,
                                    "logic:" + path + "#" + std::to_string(e) + "." + std::to_string(slot));
    }

    if (tmplIt->second.hasSpawn)
    {
        sol::protected_function fn = tmpl["OnSpawn"];
        OwnerScope as(*this, owner);
        auto res = fn(inst, LuaId::OutMaybe(m_world, e));
        if (!res.valid()) {
            sol::error err = res;
            LOG_ERROR("Lua OnSpawn error [%s] entity %u slot %zu: %s",
                      path.c_str(), e, slot, err.what());
        }
    }
    return true;
}

// OnDestroy may destroy entities (mutating m_states): work from copies and detach
// the slot before the call.
void ScriptSystem::FireSlotDestroy(Entity e, std::size_t slot, const LogicSlot& ls)
{
    const std::string path       = ls.path;        // copies: OnDestroy may erase `ls`
    const bool        initCalled = ls.initCalled;
    const OwnerId     owner      = ls.owner;

    sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), slot);

    // Detach from the registry first so re-entrant teardown is a no-op. `inst`
    // keeps the table alive across the OnDestroy call.
    sol::table arr = GetInstanceArray(*m_lua, static_cast<uint32_t>(e), /*create*/ false);
    if (arr.valid()) arr[slot + 1] = sol::lua_nil;

    auto tmplIt = m_logicTemplates.find(path);
    if (inst.valid() && tmplIt != m_logicTemplates.end() && tmplIt->second.hasDestroy && initCalled)
    {
        sol::protected_function fn = inst["OnDestroy"];
        if (fn.valid())
        {
            OwnerScope as(*this, owner);
            auto res = fn(inst);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua OnDestroy error [%s] entity %u slot %zu: %s",
                          path.c_str(), e, slot, err.what());
            }
        }
    }
    // What OnDestroy did not unsubscribe goes with the instance; what it left scheduled runs on
    // in this world.
    ReleaseOwner(owner, /*handOverDeferred=*/true);
}

// Tears down one slot, keeping the entity's state (Update's reconcile on shrink /
// path change). The caller must re-find m_states[e] afterwards: OnDestroy may have
// erased it.
void ScriptSystem::DestroyLogicInstance(Entity e, std::size_t slot)
{
    // Same dispatch-cache eviction as EnsureLogicInstance.
    if (m_dispatchCache)
        m_dispatchCache->slots.erase((uint64_t(e) << 16) | uint64_t(slot & 0xFFFFu));
    auto sIt = m_states.find(e);
    if (sIt == m_states.end() || slot >= sIt->second.slots.size()) return;
    FireSlotDestroy(e, slot, sIt->second.slots[slot]);
}

// Destroys every slot of an entity and drops its instance array (entity
// destruction, sweep). Erases the m_states entry first, working from a moved-out
// copy of the slots, so a re-entrant DestroyEntity from an OnDestroy finds nothing
// to tear down twice.
void ScriptSystem::TeardownEntity(Entity e)
{
    auto it = m_states.find(e);
    if (it == m_states.end()) return;
    const std::vector<LogicSlot> slots = std::move(it->second.slots);
    m_states.erase(it);
    for (std::size_t i = 0; i < slots.size(); ++i)
        FireSlotDestroy(e, i, slots[i]);
    (*m_lua)["__logic_instances"][static_cast<uint32_t>(e)] = sol::lua_nil;
}

// Accepts the explicit descriptor form ({type='float', default=, min=, ...}) and
// bare-literal shorthands (a number / bool / string / 3-array infers its type).
// Output is sorted by name for a stable inspector layout.
void ScriptSystem::ParseExposedSchema(const std::string& path, std::vector<ScriptVarDesc>& out)
{
    out.clear();
    if (!m_lua) return;

    sol::table tmpl = (*m_lua)["__logic_templates"][path];
    if (!tmpl.valid()) return;

    sol::object exposedObj = tmpl["exposed"];
    if (exposedObj.get_type() != sol::type::table) return;
    sol::table exposed = exposedObj.as<sol::table>();

    for (auto& kv : exposed)
    {
        if (kv.first.get_type() != sol::type::string) continue;  // skip array part
        const std::string name = kv.first.as<std::string>();
        const sol::object  val  = kv.second;

        ScriptVarDesc d;
        d.name = name;

        switch (val.get_type())
        {
        case sol::type::number:
            d.type   = ScriptVarType::Float;
            d.defVal = ScriptVarValue::MakeFloat((float)val.as<double>());
            break;
        case sol::type::boolean:
            d.type   = ScriptVarType::Bool;
            d.defVal = ScriptVarValue::MakeBool(val.as<bool>());
            break;
        case sol::type::string:
            d.type   = ScriptVarType::String;
            d.defVal = ScriptVarValue::MakeString(val.as<std::string>());
            break;
        case sol::type::table:
        {
            sol::table desc = val.as<sol::table>();
            sol::optional<std::string> typeStr = desc["type"];
            if (typeStr && ScriptVarTypeFromString(*typeStr, d.type))
            {
                d.defVal = ReadExposedDefault(d.type, desc["default"]);
                if (*typeStr == "enum")
                {
                    sol::optional<sol::table> options = desc["options"];
                    if (options)
                        for (size_t i = 1; i <= options->size(); ++i)
                        {
                            sol::object option = (*options)[i];
                            if (!option.is<std::string>()) continue;
                            const std::string value = option.as<std::string>();
                            if (std::find(d.options.begin(), d.options.end(), value) == d.options.end())
                                d.options.push_back(value);
                        }
                    if (d.options.empty())
                    {
                        LOG_WARNING("Script enum '%s' requires non-empty string options", d.name.c_str());
                        continue;
                    }
                    if (std::find(d.options.begin(), d.options.end(), d.defVal.str) == d.options.end())
                        d.defVal = ScriptVarValue::MakeString(d.options.front());
                }
                if (sol::optional<float>       mn  = desc["min"])     { d.minVal = *mn;  d.hasMin = true; }
                if (sol::optional<float>       mx  = desc["max"])     { d.maxVal = *mx;  d.hasMax = true; }
                if (sol::optional<float>       sp  = desc["step"])      d.speed   = *sp;
                if (sol::optional<float>       sp2 = desc["speed"])     d.speed   = *sp2;
                if (sol::optional<std::string> tip = desc["tooltip"])   d.tooltip = *tip;
                if (sol::optional<std::string> lbl = desc["label"])     d.label   = *lbl;
                if (sol::optional<std::string> ext = desc["ext"])       d.assetExt= *ext;
                if (sol::optional<bool>        hdr = desc["hdr"])       d.hdr     = *hdr;
            }
            else
            {
                // Bare 3-number array shorthand -> Float3.
                float v[3] = { 0.f, 0.f, 0.f };
                if (ReadExposedVec3(val, v))
                {
                    d.type   = ScriptVarType::Float3;
                    d.defVal = ScriptVarValue::MakeFloat3(v[0], v[1], v[2]);
                }
                else
                {
                    continue;  // unrecognised table: skip
                }
            }
        } break;
        default:
            continue;  // function / userdata / nil: not an exposed variable
        }

        out.push_back(std::move(d));
    }

    std::sort(out.begin(), out.end(),
        [](const ScriptVarDesc& a, const ScriptVarDesc& b) { return a.name < b.name; });
}

const std::vector<ScriptVarDesc>& ScriptSystem::GetExposedSchema(const std::string& path)
{
    static const std::vector<ScriptVarDesc> kEmpty;
    if (path.empty() || !m_lua) return kEmpty;

    auto it = m_logicTemplates.find(path);
    if (it == m_logicTemplates.end())
    {
        if (!LoadLogicTemplate(path)) return kEmpty;
        it = m_logicTemplates.find(path);
        if (it == m_logicTemplates.end()) return kEmpty;
    }
    return it->second.exposed;
}

// Float3 / Color land as Vec3 userdata (self.v.x/.y/.z); strings and assets as
// plain strings.
void ScriptSystem::InjectExposedVars(Entity e, std::size_t slot, const std::string& path,
                                     const std::unordered_map<std::string, ScriptVarValue>* overrides)
{
    auto it = m_logicTemplates.find(path);
    if (it == m_logicTemplates.end()) return;
    const std::vector<ScriptVarDesc>& schema = it->second.exposed;
    if (schema.empty()) return;

    sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), slot);
    if (!inst.valid()) return;

    static const std::unordered_map<std::string, ScriptVarValue> kEmpty;
    const std::unordered_map<std::string, ScriptVarValue>& ovr = overrides ? *overrides : kEmpty;

    for (const ScriptVarDesc& d : schema)
    {
        const ScriptVarValue v = ResolveScriptVar(d, ovr);
        switch (v.type)
        {
        case ScriptVarType::Float:  inst[d.name] = (double)v.data.f;            break;
        case ScriptVarType::Int:    inst[d.name] = (int64_t)v.data.i;           break;
        case ScriptVarType::Bool:   inst[d.name] = v.data.b;                    break;
        case ScriptVarType::Float3:
        case ScriptVarType::Color:  inst[d.name] = LuaVec3{ v.data.v3[0], v.data.v3[1], v.data.v3[2] }; break;
        case ScriptVarType::String:
        case ScriptVarType::Asset:  inst[d.name] = v.str;                       break;
        case ScriptVarType::Entity:
        {
            // Persistent form is the target GUID (v.str). Resolve it to the
            // current entity id so the script gets a live handle even after the
            // ids were reassigned by a scene reload / prefab instantiation. 0
            // when the GUID has no loaded owner yet (script should guard).
            Entity eid = static_cast<Entity>(v.data.entity);
            if (!v.str.empty())
            {
                const Entity r = ECS::GuidRegistry::Get().Find(ECS::Guid::FromHex(v.str));
                eid = (r != NullEntity) ? r : NullEntity;
            }
            inst[d.name] = LuaId::OutMaybe(m_world ? m_world : m_boundWorld, eid);
        } break;
        }
    }
}

void ScriptSystem::ApplyExposedVars(Entity e, std::size_t slot,
                                    const std::unordered_map<std::string, ScriptVarValue>& overrides)
{
    if (!m_lua) return;
    auto sIt = m_states.find(e);
    if (sIt == m_states.end() || slot >= sIt->second.slots.size()) return;
    const std::string& path = sIt->second.slots[slot].path;
    if (path.empty()) return;
    InjectExposedVars(e, slot, path, &overrides);
}

bool ScriptSystem::LoadSystem(const std::string& path)
{
    const std::string name = Sys::PathToUtf8(Sys::U8Path(path).stem());

    // Tear down existing entry with the same name (hot-reload path). A scene-scoped System that
    // was running comes back running.
    bool wasActiveSceneSystem = false;
    for (auto& sys : m_systems)
    {
        if (sys.name != name) continue;
        wasActiveSceneSystem = sys.loaded && sys.sceneScoped && sys.active;
        TeardownSystem(sys);
    }

    // The file's top level runs as this load's owner, so teardown / reload drops exactly what it
    // created.
    const OwnerId loadOwner = NewOwner(OwnerKind::System, name, "system:" + name);
    sol::protected_function_result result = [&]
    {
        OwnerScope as(*this, loadOwner);
        return RunScriptAsset(*m_lua, path);
    }();
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: system load error [%s]: %s", path.c_str(), err.what());
        ReleaseOwner(loadOwner);
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: system [%s] did not return a table", path.c_str());
        ReleaseOwner(loadOwner);
        return false;
    }

    sol::table t = ret.as<sol::table>();

    // Find or insert. Insertion preserves order; replacement keeps slot.
    SystemEntry* slot = nullptr;
    for (auto& sys : m_systems)
        if (sys.name == name) { slot = &sys; break; }
    if (!slot)
    {
        m_systems.push_back({});
        slot = &m_systems.back();
    }

    slot->name        = name;
    slot->path        = path;
    slot->loaded      = true;
    slot->initCalled  = false;          // OnInit fires on next Update
    slot->hasUpdate   = t["OnUpdate"].valid();
    slot->hasShutdown = t["OnShutdown"].valid();
    // Optional integer `order`, re-read on every (re)load; only boot finalization
    // sorts by it, so an order edit takes effect on the next boot.
    slot->order = 0;
    bool tableDeclaredOrder = false;
    if (sol::object orderField = t["order"]; orderField.valid())
    {
        if (orderField.is<int32_t>())
        {
            slot->order = orderField.as<int32_t>();
            tableDeclaredOrder = true;
        }
        else
            LOG_WARNING("ScriptSystem: system [%s] `order` must be an integer — using 0",
                        name.c_str());
    }
    slot->systemSet   = t.get_or("set", std::string{});
    // Optional `scope`: "engine" (the default, every scene) or "scene" (only while the entered
    // scene lists it).
    slot->sceneScoped = false;
    bool tableDeclaredScope = false;
    if (sol::object scopeField = t["scope"]; scopeField.valid())
    {
        const std::string scope = scopeField.is<std::string>() ? scopeField.as<std::string>()
                                                               : std::string{};
        if (scope == "scene" || scope == "engine")
        {
            slot->sceneScoped  = scope == "scene";
            tableDeclaredScope = true;
        }
        else
            LOG_WARNING("ScriptSystem: system [%s] `scope` must be \"engine\" or \"scene\" — using engine",
                        name.c_str());
    }

    // Manifest set/order/scope apply only when the loading path matches the manifest entry:
    // a stem-colliding load from elsewhere (an -exec driver named like a manifest
    // system) must not inherit a `set` and be silently gated off. Applied here so boot
    // and hot reload both re-apply it (a reload rewrites every SystemEntry field).
    if (auto metaIt = m_manifestSystemMeta.find(name); metaIt != m_manifestSystemMeta.end())
    {
        const ManifestSystemMeta& meta = metaIt->second;
        if (meta.path == path)
        {
            // Manifest order overrides only when declared there: an entry without the field
            // must not reset a table `order` to 0.
            if (meta.hasOrder)
            {
                if (tableDeclaredOrder && slot->order != meta.order)
                    LuaDiag::WarnOnceNamed("manifest-order:" + name,
                            "ScriptSystem: system [%s] table `order`=%d differs from "
                            "manifest order=%d — manifest wins",
                            name.c_str(), slot->order, meta.order);
                slot->order = meta.order;
            }
            if (!meta.set.empty())
            {
                if (!slot->systemSet.empty() && slot->systemSet != meta.set)
                    LuaDiag::WarnOnceNamed("manifest-set:" + name,
                            "ScriptSystem: system [%s] table `set`='%s' differs from "
                            "manifest set='%s' — manifest wins",
                            name.c_str(), slot->systemSet.c_str(), meta.set.c_str());
                slot->systemSet = meta.set;
            }
            if (meta.hasScope)
            {
                if (tableDeclaredScope && slot->sceneScoped != meta.sceneScoped)
                    LuaDiag::WarnOnceNamed("manifest-scope:" + name,
                            "ScriptSystem: system [%s] table `scope` differs from manifest "
                            "scope='%s' — manifest wins",
                            name.c_str(), meta.sceneScoped ? "scene" : "engine");
                slot->sceneScoped = meta.sceneScoped;
            }
        }
        else
        {
            LOG_ERROR("ScriptSystem: system [%s] loaded from '%s' collides with manifest "
                      "entry '%s' — manifest set/order NOT applied. The stem is the "
                      "registration id; the earlier system was torn down and replaced.",
                      name.c_str(), path.c_str(), meta.path.c_str());
        }
    }

    // GameFlow.AssignSystemSet still wins over everything (bootstrap override).
    if (auto overrideIt = m_systemSetOverrides.find(name);
        overrideIt != m_systemSetOverrides.end())
    {
        slot->systemSet = overrideIt->second;
    }
    slot->lastError.clear();

    if (!slot->systemSet.empty() && m_registerSystemSet)
        m_registerSystemSet(slot->systemSet);

    // A System's methods run as the System when another script calls them
    // (Engine.GetSystem("HitStop"):Trigger()), so what they create is the System's to release.
    {
        sol::protected_function wrap = (*m_lua)["__ScriptWrapMethods"];
        if (wrap.valid())
        {
            auto wr = wrap(t, name);
            if (!wr.valid())
            {
                sol::error err = wr;
                LOG_ERROR("ScriptSystem: system [%s] method wrap failed: %s", name.c_str(), err.what());
            }
        }
    }

    const bool sceneScoped = slot->sceneScoped;
    if (!sceneScoped)
    {
        (*m_lua)["__systems"][name] = t;
        slot->owner  = loadOwner;
        slot->active = true;
    }
    else
    {
        // The file table is the prototype; each scene entry gets a fresh instance over it.
        (*m_lua)["__system_protos"][name] = t;
        (*m_lua)["__systems"][name]       = sol::lua_nil;
        slot->owner  = 0;
        slot->active = false;
        // State belongs to the activation: what the top level created would outlive every scene.
        const std::size_t subs   = m_luaBus ? m_luaBus->RemoveOwned(loadOwner) : 0;
        const std::size_t timers = m_timers ? m_timers->CancelOwned(loadOwner) : 0;
        if (subs + timers > 0)
            LOG_WARNING("ScriptSystem: scene-scoped system [%s] subscribed / scheduled at its file's "
                        "top level (%zu subscription(s), %zu timer(s)) — dropped; do it in OnInit",
                        name.c_str(), subs, timers);
        ReleaseOwner(loadOwner);
    }

    try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded system [%s] from %s (OnInit=%d OnUpdate=%d OnShutdown=%d scope=%s)",
             name.c_str(), path.c_str(),
             t["OnInit"].valid(), slot->hasUpdate, slot->hasShutdown,
             sceneScoped ? "scene" : "engine");

    // A hot reload of a running scene System, or a first load of one the entered scene already
    // lists: run it now (OnInit fires on the next Update, in tick order).
    if (sceneScoped
        && (wasActiveSceneSystem
            || std::find(m_sceneSystems.begin(), m_sceneSystems.end(), name) != m_sceneSystems.end()))
    {
        for (SystemEntry& sys : m_systems)
            if (sys.name == name) { ActivateSceneSystem(sys); break; }
        if (std::find(m_sceneSystems.begin(), m_sceneSystems.end(), name) == m_sceneSystems.end())
            m_sceneSystems.push_back(name);
    }
    return true;
}

// ==== Scene-scoped Systems ====
void ScriptSystem::ActivateSceneSystem(SystemEntry& sys)
{
    sol::table proto = (*m_lua)["__system_protos"][sys.name];
    if (!proto.valid()) return;
    sol::table inst = m_lua->create_table();
    sol::table mt   = m_lua->create_table();
    mt["__index"]   = proto;
    inst[sol::metatable_key] = mt;
    (*m_lua)["__systems"][sys.name] = inst;
    sys.owner      = NewOwner(OwnerKind::System, sys.name, "system:" + sys.name);
    sys.active     = true;
    sys.initCalled = false;
    // A fresh instance: an OnUpdate parked by an error in an earlier entry runs again.
    sys.hasUpdate  = proto["OnUpdate"].valid();
    ++m_luaDispatchGen;   // dispatch-cache rows hold the previous instance
}

void ScriptSystem::ActivateSceneSystems(const std::vector<std::string>& names)
{
    m_sceneSystems.clear();
    for (const std::string& want : names)
    {
        SystemEntry* sys = nullptr;
        for (SystemEntry& s : m_systems)
            if (s.name == want) { sys = &s; break; }
        if (!sys || !sys->loaded)
        {
            // A System the project switched off (manifest `enabled: false`, a disabled module)
            // is not this scene's fault.
            const bool disabled = std::any_of(m_manifestDisabled.begin(), m_manifestDisabled.end(),
                [&](const std::string& row)
                {
                    const std::string tail = "/" + want + ".lua";
                    return row.size() >= tail.size()
                        && row.compare(row.size() - tail.size(), tail.size(), tail) == 0;
                });
            if (disabled)
            {
                LOG_INFO("[SCRIPTSCOPE] scene '%s' lists System '%s', which this project disables — skipped",
                         m_sceneScriptName.c_str(), want.c_str());
                continue;
            }
            const std::string what = "scene '" + m_sceneScriptName + "' lists System '" + want
                                   + "', which is not loaded";
            LOG_ERROR("[SCRIPTSCOPE] %s", what.c_str());
            Harness::RunLedger::Get().NoteScriptFault("SceneSystems", what.c_str());
            continue;
        }
        if (!sys->sceneScoped)
        {
            LOG_WARNING("[SCRIPTSCOPE] scene '%s' lists System '%s', which is engine-scoped and "
                        "already runs in every scene — give it `scope = \"scene\"` to make the list mean something",
                        m_sceneScriptName.c_str(), want.c_str());
            continue;
        }
        if (sys->active) continue;   // listed twice
        ActivateSceneSystem(*sys);
    }

    // OnInit in tick order, before the scene script's OnSceneEnter. By name: an OnInit may load
    // or remove Systems and reallocate m_systems.
    std::vector<std::string> order;
    for (const SystemEntry& s : m_systems)
        if (s.sceneScoped && s.active) order.push_back(s.name);
    for (const std::string& n : order)
        for (SystemEntry& s : m_systems)
            if (s.name == n)
            {
                if (s.active && !s.initCalled) InitSystem(s);
                break;
            }
    m_sceneSystems = order;
    if (!order.empty())
    {
        std::string list;
        for (const std::string& n : order) list += (list.empty() ? "" : ", ") + n;
        LOG_INFO("[SCRIPTSCOPE] scene '%s' systems: %s", m_sceneScriptName.c_str(), list.c_str());
    }
}

void ScriptSystem::DeactivateSceneSystems()
{
    // Reverse tick order: a later System may still use an earlier one in its OnShutdown.
    std::vector<std::string> order;
    for (const SystemEntry& s : m_systems)
        if (s.sceneScoped && s.active) order.push_back(s.name);
    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const std::string& n = *it;
        OwnerId owner = 0;
        bool    shutdown = false;
        for (SystemEntry& s : m_systems)
            if (s.name == n) { owner = s.owner; shutdown = s.loaded && s.hasShutdown && s.initCalled; break; }
        if (shutdown)
        {
            sol::optional<sol::table> inst = (*m_lua)["__systems"][n];
            if (inst)
            {
                sol::protected_function fn = (*inst)["OnShutdown"];
                if (fn.valid())
                {
                    WorldBorrow borrow(*this);
                    OwnerScope  as(*this, owner);
                    auto res = fn(*inst);
                    if (!res.valid())
                    {
                        sol::error err = res;
                        LOG_ERROR("Lua OnShutdown error [%s]: %s", n.c_str(), err.what());
                    }
                }
            }
        }
        // Re-found: OnShutdown may have reloaded or removed Systems.
        for (SystemEntry& s : m_systems)
            if (s.name == n)
            {
                ReleaseOwner(s.owner);
                s.owner      = 0;
                s.active     = false;
                s.initCalled = false;
                break;
            }
        (*m_lua)["__systems"][n] = sol::lua_nil;
    }
    if (!order.empty())
    {
        ++m_luaDispatchGen;
        LOG_INFO("[SCRIPTSCOPE] scene '%s' systems shut down: %zu", m_sceneScriptName.c_str(), order.size());
    }
    m_sceneSystems.clear();
}

void ScriptSystem::InitSystem(SystemEntry& sys)
{
    // Marked first: OnInit may reallocate m_systems, so `sys` is not touched after the call.
    sys.initCalled = true;
    sol::optional<sol::table> inst = (*m_lua)["__systems"][sys.name];
    if (!inst) return;
    sol::protected_function init = (*inst)["OnInit"];
    if (!init.valid()) return;
    const std::string name = sys.name;
    WorldBorrow borrow(*this);
    OwnerScope  as(*this, sys.owner);
    auto res = init(*inst);
    if (!res.valid())
    {
        sol::error err = res;
        LOG_ERROR("Lua OnInit error [%s]: %s", name.c_str(), err.what());
        Harness::RunLedger::Get().NoteScriptFault(name.c_str(), err.what());
    }
}

ScriptSystem::OwnerId ScriptSystem::SystemOwner(const std::string& name) const
{
    for (const SystemEntry& s : m_systems)
        if (s.name == name) return s.loaded && s.active ? s.owner : 0;
    return 0;
}

// ==== Script ownership ====
ScriptSystem::OwnerId ScriptSystem::NewOwner(OwnerKind kind, const std::string& name, std::string label)
{
    const OwnerId id = ++m_nextOwnerId;
    m_owners[id] = OwnerRecord{ kind, name, std::move(label) };
    return id;
}

ScriptSystem::OwnerId ScriptSystem::WorldOwner()
{
    if (m_worldOwner == 0) m_worldOwner = NewOwner(OwnerKind::World, std::string{}, "world");
    return m_worldOwner;
}

void ScriptSystem::ReleaseOwner(OwnerId owner, bool handOverDeferred)
{
    if (owner == 0) return;
    const std::size_t subs = m_luaBus ? m_luaBus->RemoveOwned(owner) : 0;
    std::size_t timers = CancelTasks(owner, false), tweens = 0, handed = 0;
    const OwnerId heir = handOverDeferred ? WorldOwner() : 0;
    if (handOverDeferred)
    {
        handed += m_timers ? m_timers->Reassign(owner, heir) : 0;
        handed += m_tweens ? m_tweens->Reassign(owner, heir) : 0;
    }
    else
    {
        timers += m_timers ? m_timers->CancelOwned(owner) : 0;
        tweens = m_tweens ? m_tweens->DropOwnedCallbacks(owner) : 0;
    }

    std::size_t times = 0;
    for (auto it = m_timeRequests.begin(); it != m_timeRequests.end();)
    {
        if (it->owner != owner) { ++it; continue; }
        if (handOverDeferred && it->remaining >= 0.f) { it->owner = heir; ++handed; ++it; continue; }
        it = m_timeRequests.erase(it);
        ++times;
    }
    if (times) RefreshTimeScale();
    std::size_t entityTimes = 0;
    for (auto it = m_entityTimeRequests.begin(); it != m_entityTimeRequests.end();)
    {
        if (it->owner != owner) { ++it; continue; }
        if (handOverDeferred && it->remaining >= 0.f) { it->owner = heir; ++handed; ++it; continue; }
        it = m_entityTimeRequests.erase(it);
        ++entityTimes;
    }
    if (entityTimes) RefreshEntityTimeScales();
    times += entityTimes;

    // Pop only what is still active and still this owner's (a pop by anyone clears the entry).
    std::size_t contexts = 0;
    for (auto it = m_contextOwners.begin(); it != m_contextOwners.end();)
    {
        if (it->second == owner)
        {
            if (Input::Get().PopContext(it->first.c_str())) ++contexts;
            it = m_contextOwners.erase(it);
        }
        else ++it;
    }

    for (const OwnerReleaseHook& hook : m_ownerReleaseHooks) hook(owner, false);

    auto rec = m_owners.find(owner);
    if (subs + timers + tweens + times + contexts + handed > 0)
        LOG_INFO("[SCRIPTSCOPE] release %s: subs=%zu timers=%zu time=%zu contexts=%zu tweenCallbacks=%zu handedToWorld=%zu",
                 rec != m_owners.end() ? rec->second.label.c_str() : "(forgotten owner)",
                 subs, timers, times, contexts, tweens, handed);
    if (rec != m_owners.end()) m_owners.erase(rec);
}

bool ScriptSystem::OwnerSuspended(OwnerId owner) const
{
    if (owner == 0 || !m_isSystemSetActive) return false;
    auto it = m_owners.find(owner);
    if (it == m_owners.end()) return false;
    const OwnerRecord& rec = it->second;
    const std::string* set = nullptr;
    if (rec.kind == OwnerKind::System)
    {
        for (const SystemEntry& s : m_systems)
            if (s.name == rec.name) { set = &s.systemSet; break; }
    }
    else if (rec.kind == OwnerKind::Logic)
    {
        if (auto t = m_logicTemplates.find(rec.name); t != m_logicTemplates.end())
            set = &t->second.systemSet;
    }
    return set && !set->empty() && !m_isSystemSetActive(*set);
}

void ScriptSystem::ForgetOwnersOfKind(OwnerKind kind)
{
    for (auto it = m_owners.begin(); it != m_owners.end();)
    {
        if (it->second.kind == kind) it = m_owners.erase(it);
        else ++it;
    }
}

void ScriptSystem::SetSystemSetRegistrar(SystemSetRegistrar registrar)
{
    m_registerSystemSet = std::move(registrar);
    if (!m_registerSystemSet) return;
    for (const SystemEntry& system : m_systems)
        if (!system.systemSet.empty()) m_registerSystemSet(system.systemSet);
}

bool ScriptSystem::AssignSystemToSet(const std::string& systemName,
                                     const std::string& setName)
{
    if (systemName.empty() || setName.empty())
    {
        LOG_WARNING("ScriptSystem::AssignSystemToSet: system/set name cannot be empty");
        return false;
    }

    m_systemSetOverrides[systemName] = setName;
    for (SystemEntry& system : m_systems)
    {
        if (system.name != systemName) continue;
        system.systemSet = setName;
        break;
    }
    if (m_registerSystemSet) m_registerSystemSet(setName);
    return true; // may target a System loaded later in the boot scan
}

bool ScriptSystem::ValidateSystemSetAssignments() const
{
    bool valid = true;
    for (const auto& [systemName, setName] : m_systemSetOverrides)
    {
        const bool found = std::any_of(m_systems.begin(), m_systems.end(),
            [&](const SystemEntry& system) { return system.name == systemName; });
        if (!found)
        {
            valid = false;
            LOG_ERROR("GameFlow.AssignSystemSet('%s','%s'): Lua System was not loaded",
                      systemName.c_str(), setName.c_str());
        }
    }
    return valid;
}

std::vector<std::pair<std::string, std::string>>
ScriptSystem::GetSystemSetAssignments() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const SystemEntry& system : m_systems)
        if (!system.systemSet.empty()) out.emplace_back(system.name, system.systemSet);
    std::sort(out.begin(), out.end());
    return out;
}

bool ScriptSystem::RemoveSystem(const std::string& name)
{
    for (auto it = m_systems.begin(); it != m_systems.end(); ++it)
    {
        if (it->name != name) continue;
        TeardownSystem(*it);
        m_systems.erase(it);
        return true;
    }
    return false;
}

void ScriptSystem::TeardownSystem(SystemEntry& sys)
{
    if (sys.loaded && sys.active && sys.hasShutdown && sys.initCalled)
    {
        // sol::optional: an inactive scene System's slot is nil, and a checked conversion of nil
        // throws across the Lua boundary.
        sol::optional<sol::table> inst = (*m_lua)["__systems"][sys.name];
        if (inst)
        {
            sol::protected_function fn = (*inst)["OnShutdown"];
            if (fn.valid())
            {
                OwnerScope as(*this, sys.owner);
                auto res = fn(*inst);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnShutdown error [%s]: %s",
                              sys.name.c_str(), err.what());
                }
            }
        }
    }
    // Old handlers would fire against the nil'd table (and a reload would
    // stack a second live set): release everything this System owns.
    ReleaseOwner(sys.owner);
    sys.owner = 0;
    (*m_lua)["__systems"][sys.name] = sol::lua_nil;
    if (sys.sceneScoped)
    {
        (*m_lua)["__system_protos"][sys.name] = sol::lua_nil;
        sys.active = false;
        m_sceneSystems.erase(std::remove(m_sceneSystems.begin(), m_sceneSystems.end(), sys.name),
                             m_sceneSystems.end());
    }
    sys.loaded = false;
    sys.initCalled = false;
}

// Stores the returned table under __services[name] for Engine.GetService; no
// callbacks are fired.
bool ScriptSystem::LoadService(const std::string& path)
{
    const std::string name = Sys::PathToUtf8(Sys::U8Path(path).stem());

    auto result = RunScriptAsset(*m_lua, path);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: service load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: service [%s] did not return a table", path.c_str());
        return false;
    }

    sol::table serviceTable = ret.as<sol::table>();
    // A service table with lifecycle callbacks is almost always a System registered
    // as the wrong kind: services get no engine-driven lifecycle, so the callbacks
    // would never fire.
    if (serviceTable["OnInit"].valid() || serviceTable["OnUpdate"].valid()
        || serviceTable["OnShutdown"].valid())
        LOG_WARNING("ScriptSystem: service [%s] declares OnInit/OnUpdate/OnShutdown — "
                    "services get no lifecycle callbacks; did you mean kind 'system'?",
                    name.c_str());
    (*m_lua)["__services"][name] = serviceTable;

    auto& svc = m_services[name];
    svc.name = name;
    svc.path = path;
    svc.loaded = true;
    svc.lastError.clear();

    try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded service [%s] from %s", name.c_str(), path.c_str());
    return true;
}

bool ScriptSystem::LoadUIScript(const std::string& path)
{
    const std::string name = Sys::PathToUtf8(Sys::U8Path(path).stem());

    auto result = RunScriptAsset(*m_lua, path);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: ui-script load error [%s]: %s", path.c_str(), err.what());
        return false;
    }

    sol::object ret = result;
    if (ret.get_type() != sol::type::table)
    {
        LOG_ERROR("ScriptSystem: ui-script [%s] did not return a table", path.c_str());
        return false;
    }

    sol::table uiTable = ret.as<sol::table>();
    // Same trap as services: UI scripts are convention-driven (:Open/:Close),
    // the engine never fires lifecycle callbacks on them.
    if (uiTable["OnInit"].valid() || uiTable["OnUpdate"].valid()
        || uiTable["OnShutdown"].valid())
        LOG_WARNING("ScriptSystem: ui-script [%s] declares OnInit/OnUpdate/OnShutdown — "
                    "ui scripts get no lifecycle callbacks; did you mean kind 'system'?",
                    name.c_str());
    (*m_lua)["__ui_scripts"][name] = uiTable;

    auto& ui = m_uiScripts[name];
    ui.name = name;
    ui.path = path;
    ui.loaded = true;
    ui.lastError.clear();

    try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); } catch (...) {}

    LOG_INFO("ScriptSystem: loaded ui-script [%s] from %s", name.c_str(), path.c_str());
    return true;
}

// A mid-play reload is safe: per-agent state lives on the blackboard /
// BTInstance, not in these files.
bool ScriptSystem::AddGlobalScript(const std::string& path)
{
    if (!m_lua) return false;

    // Snapshot Actions / Conditions to report cross-file redefinitions: they are
    // flat global tables loaded in sequence, so a later file silently wins a name
    // collision for every tree, including trees that wanted the earlier definition.
    auto snapshotOwners = [this](const char* tableName,
                                 std::unordered_map<std::string, const void*>& out)
    {
        sol::object o = (*m_lua)[tableName];
        if (!o.is<sol::table>()) return;
        for (auto& kv : o.as<sol::table>())
            if (kv.first.is<std::string>())
                out[kv.first.as<std::string>()] = kv.second.pointer();
    };
    std::unordered_map<std::string, const void*> before[2];
    snapshotOwners("Actions",    before[0]);
    snapshotOwners("Conditions", before[1]);

    auto result = RunScriptAsset(*m_lua, path);
    if (!result.valid())
    {
        sol::error err = result;
        LOG_ERROR("ScriptSystem: global script load error [%s]: %s",
                  path.c_str(), err.what());
        return false;
    }

    if (std::find(m_globalScripts.begin(), m_globalScripts.end(), path)
        == m_globalScripts.end())
    {
        m_globalScripts.push_back(path);
    }
    try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); } catch (...) {}

    // Report entries this file replaced rather than added. Comparing the function
    // pointer, not presence, keeps a hot reload of the same file quiet: only a
    // cross-file override warns.
    const char* names[2] = { "Actions", "Conditions" };
    for (int i = 0; i < 2; ++i)
    {
        sol::object o = (*m_lua)[names[i]];
        if (!o.is<sol::table>()) continue;
        for (auto& kv : o.as<sol::table>())
        {
            if (!kv.first.is<std::string>()) continue;
            const std::string key = kv.first.as<std::string>();
            auto it = before[i].find(key);
            if (it != before[i].end() && it->second != kv.second.pointer())
            {
                LOG_WARNING("BT library [%s] REDEFINES %s.%s — the later "
                            "definition wins for every tree, including trees "
                            "that wanted the earlier one",
                            path.c_str(), names[i], key.c_str());
            }
        }
    }

    LOG_INFO("ScriptSystem: loaded global script [%s]", path.c_str());
    return true;
}

void ScriptSystem::DispatchAnimEvent(Entity e, const std::string& name)
{
    DispatchAnimEvent(e, name, nullptr);
}

void ScriptSystem::DispatchAnimEvent(Entity e, const std::string& name,
                                     const PropertyBag* params)
{
    auto sIt = m_states.find(e);
    if (sIt == m_states.end()) return;

    sol::object payload = sol::lua_nil;
    if (params && !params->values.empty())
    {
        sol::table t = m_lua->create_table();
        for (const auto& [key, value] : params->values)
        {
            std::visit([&](const auto& v)
            {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, DirectX::XMFLOAT3>)
                    t[key] = m_lua->create_table_with("x", v.x, "y", v.y, "z", v.z);
                else if constexpr (std::is_same_v<V, DirectX::XMFLOAT4>)
                    t[key] = m_lua->create_table_with("x", v.x, "y", v.y,
                                                      "z", v.z, "w", v.w);
                else
                    t[key] = v;
            }, value);
        }
        payload = t;
    }

    for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
    {
        if (!sIt->second.slots[i].initCalled) continue;
        sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), i);
        if (!inst.valid()) continue;
        sol::protected_function fn = inst["OnAnimEvent"];
        if (!fn.valid()) continue;
        OwnerScope as(*this, sIt->second.slots[i].owner);
        auto res = fn(inst, name, payload);
        if (!res.valid()) {
            sol::error err = res;
            LOG_ERROR("Lua OnAnimEvent error [%s] entity %u slot %zu name=%s: %s",
                      sIt->second.slots[i].path.c_str(), e, i, name.c_str(), err.what());
        }
    }
}

// Deferred path of the CustomNotifyEvent bridge: see AnimEventQueue in
// ScriptSystem.h.
void ScriptSystem::QueueAnimEvent(Entity e, const std::string& name,
                                  const PropertyBag* params)
{
    if (!m_lua) return;
    if (m_states.find(e) == m_states.end()) return;   // nobody would receive it

    // Build the payload NOW (pure Lua-table work, needs no world) so the queue
    // does not have to own a PropertyBag copy.
    sol::object payload = sol::lua_nil;
    if (params && !params->values.empty())
    {
        sol::table t = m_lua->create_table();
        for (const auto& [key, value] : params->values)
        {
            std::visit([&](const auto& v)
            {
                using V = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<V, DirectX::XMFLOAT3>)
                    t[key] = m_lua->create_table_with("x", v.x, "y", v.y, "z", v.z);
                else if constexpr (std::is_same_v<V, DirectX::XMFLOAT4>)
                    t[key] = m_lua->create_table_with("x", v.x, "y", v.y,
                                                      "z", v.z, "w", v.w);
                else
                    t[key] = v;
            }, value);
        }
        payload = t;
    }

    const World* w = m_world ? m_world : m_boundWorld;
    AnimEventQueue::Entry ent;
    ent.e       = e;
    ent.gen     = w ? (w->GetGeneration(e) & LuaId::kGenMask) : 0u;
    ent.name    = name;
    ent.payload = payload;
    m_animEvents->pending.push_back(std::move(ent));
}

void ScriptSystem::DispatchPendingAnimEvents()
{
    if (!m_animEvents || m_animEvents->pending.empty()) return;

    // Swap out so a handler that queues another anim event does not invalidate
    // the iteration (it lands on the next frame's drain, same as the LuaBus).
    std::vector<AnimEventQueue::Entry> batch;
    batch.swap(m_animEvents->pending);

    for (auto& ev : batch)
    {
        // Generation gate: the id may have been recycled during the one frame
        // this entry waited.
        if (m_world)
        {
            if (!m_world->IsAlive(ev.e)) continue;
            if (ev.gen != 0 && ev.gen != (m_world->GetGeneration(ev.e) & LuaId::kGenMask))
                continue;
        }

        auto sIt = m_states.find(ev.e);
        if (sIt == m_states.end()) continue;

        for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
        {
            if (!sIt->second.slots[i].initCalled) continue;
            if (sIt->second.slots[i].faulted)     continue;
            sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(ev.e), i);
            if (!inst.valid()) continue;
            const bool graphEvent=ev.payload.is<sol::table>() && ev.payload.as<sol::table>()["graphEvent"].valid();
            sol::protected_function fn = inst[graphEvent ? "OnAnimGraphEvent" : "OnAnimEvent"];
            if (!fn.valid()) continue;
            OwnerScope as(*this, sIt->second.slots[i].owner);
            auto res = fn(inst, ev.name, ev.payload);
            if (!res.valid()) {
                sol::error err = res;
                LOG_ERROR("Lua OnAnimEvent error [%s] entity %u slot %zu name=%s: %s",
                          sIt->second.slots[i].path.c_str(), ev.e, i,
                          ev.name.c_str(), err.what());
            }
            // A handler may have destroyed the entity or reallocated m_states.
            sIt = m_states.find(ev.e);
            if (sIt == m_states.end()) break;
            if (i >= sIt->second.slots.size()) break;
        }
    }
}

void ScriptSystem::ScanScriptDirectory(const std::string& root)
{
    enum class Kind { Service, System, UI };
    auto scan = [this](const fs::path& dir, Kind kind)
    {
        // Collect the .lua files directly under `dir`. A packed build has no loose
        // files, so a directory_iterator would find nothing and every auto-scanned script
        // would silently never register: use the pak index when one is mounted, else the
        // loose disk (editor / unpacked dev). Content is read pak-first either way
        // (RunScriptAsset -> AssetFS::ReadFileText).
        std::vector<std::string> files;

        auto& afs = ::Resource::AssetFS::Get();
        if (afs.IsMounted())
        {
            std::string prefix = Sys::WideToUtf8(dir.generic_wstring());          // forward slashes
            if (!prefix.empty() && prefix.back() != '/') prefix += '/';
            std::vector<std::string> keys;
            afs.EnumerateUnder(prefix, keys);
            for (const std::string& k : keys)
            {
                const std::string rest = k.substr(prefix.size());
                if (rest.find('/') != std::string::npos) continue;   // direct children only
                if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".lua") == 0)
                    files.push_back(k);
            }
        }
        else
        {
            std::error_code ec;
            if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return;
            for (auto& entry : fs::directory_iterator(dir, ec))
                if (entry.path().extension() == ".lua")
                    // generic form: forward slashes, matching pak keys and
                    // manifest paths, so registry paths / m_fileTimestamps
                    // keys / dump output are mode-independent.
                    files.push_back(Sys::WideToUtf8(entry.path().generic_wstring()));
        }

        // Sort alphabetically so load order is deterministic across runs.
        std::sort(files.begin(), files.end());

        for (const std::string& f : files)
        {
            // A script module the project leaves off (game.json "modules"), as the manifest boot
            // disables its entries; a package does not even carry the files.
            if (!Module::Registry::Get().IsScriptEnabled(f))
            {
                LOG_INFO("[MODULES] boot script %s: its module is off in this project", f.c_str());
                continue;
            }
            switch (kind)
            {
                case Kind::Service: LoadService (f); break;
                case Kind::System:  LoadSystem  (f); break;
                case Kind::UI:      LoadUIScript(f); break;
            }
        }
    };

    scan(Sys::U8Path(root) / "services", Kind::Service);
    scan(Sys::U8Path(root) / "systems",  Kind::System);
    scan(Sys::U8Path(root) / "ui",       Kind::UI);

    FinalizeBootRegistry();
}

// A failing entry degrades like the scan: it logs and the boot continues.
void ScriptSystem::LoadBootScripts(const Scripting::ScriptManifest& manifest)
{
    using Scripting::BootScriptKind;

    // Fill the per-system meta before any LoadSystem runs so the first load already
    // sees its manifest set/order. Disabled entries stay out: an inert stale entry is
    // a trap for stem-colliding loads. Cleared first, so a second call starts clean.
    m_manifestSystemMeta.clear();
    m_manifestDisabled.clear();
    for (const Scripting::BootScriptEntry& e : manifest.entries)
        if (e.kind == BootScriptKind::System && e.enabled)
            m_manifestSystemMeta[e.stem] =
                ManifestSystemMeta{ e.path, e.systemSet, e.order, e.hasOrder,
                                    e.sceneScoped, e.hasScope };

    std::size_t loaded = 0, failed = 0, disabled = 0;
    for (const Scripting::BootScriptEntry& e : manifest.entries)
    {
        if (!e.enabled)
        {
            ++disabled;
            m_manifestDisabled.push_back(
                std::string("[") + Scripting::BootScriptKindName(e.kind) + "] " + e.path);
            LOG_INFO("ScriptSystem: manifest entry disabled: [%s] %s",
                     Scripting::BootScriptKindName(e.kind), e.path.c_str());
            continue;
        }
        bool ok = false;
        switch (e.kind)
        {
            case BootScriptKind::Global:  ok = AddGlobalScript(e.path); break;
            case BootScriptKind::Service: ok = LoadService(e.path);     break;
            case BootScriptKind::System:  ok = LoadSystem(e.path);      break;
            case BootScriptKind::UI:      ok = LoadUIScript(e.path);    break;
        }
        ok ? ++loaded : ++failed;
    }
    m_bootedFromManifest = true;

    // Baseline for CheckHotReload's restart-to-apply notice. Loose file only: a
    // packed build has nothing to stat and nobody editing.
    try
    {
        m_manifestTimestamp = fs::last_write_time(Sys::U8Path(Scripting::kScriptManifestPath));
        m_manifestTimestampValid = true;
    }
    catch (...) {}

    LOG_INFO("ScriptSystem: manifest boot — %zu loaded, %zu failed, %zu disabled (of %zu entries)",
             loaded, failed, disabled, manifest.entries.size());

    FinalizeBootRegistry();
}

void ScriptSystem::FinalizeBootRegistry()
{
    // Systems declaring an integer `order` (table or manifest) sort around
    // the rest; the name tie-break keeps order-less scripts in their
    // alphabetical positions. Sorting once here (not per-load) settles tick
    // order before the first Update fires any OnInit.
    std::stable_sort(m_systems.begin(), m_systems.end(),
                     [](const SystemEntry& a, const SystemEntry& b)
                     {
                         return a.order != b.order ? a.order < b.order
                                                   : a.name  < b.name;
                     });
    ++m_luaDispatchGen;   // dispatch-cache rows are index-parallel to m_systems

    DumpBootRegistry();
}

// Services and UI scripts print sorted by name (their registries are unordered
// maps).
void ScriptSystem::DumpBootRegistry() const
{
    LOG_INFO("ScriptSystem: boot registry — %zu global / %zu service / %zu system / %zu ui%s",
             m_globalScripts.size(), m_services.size(), m_systems.size(), m_uiScripts.size(),
             m_bootedFromManifest ? " (manifest)" : " (folder scan)");

    for (const std::string& path : m_globalScripts)
        LOG_INFO("  global   %-24s %s",
                 Sys::PathToUtf8(Sys::U8Path(path).stem()).c_str(), path.c_str());

    auto sortedByName = [](const std::unordered_map<std::string, ServiceEntry>& map)
    {
        std::vector<const ServiceEntry*> out;
        out.reserve(map.size());
        for (const auto& [name, entry] : map) out.push_back(&entry);
        std::sort(out.begin(), out.end(),
                  [](const ServiceEntry* a, const ServiceEntry* b) { return a->name < b->name; });
        return out;
    };

    for (const ServiceEntry* svc : sortedByName(m_services))
        LOG_INFO("  service  %-24s %s%s", svc->name.c_str(), svc->path.c_str(),
                 svc->loaded ? "" : "  [LOAD FAILED]");

    for (const SystemEntry& sys : m_systems)
        LOG_INFO("  system   %-24s order=%-5d set=%-18s scope=%-6s OnUpdate=%d OnShutdown=%d  %s%s",
                 sys.name.c_str(), sys.order,
                 sys.systemSet.empty() ? "-" : sys.systemSet.c_str(),
                 sys.sceneScoped ? "scene" : "engine",
                 (int)sys.hasUpdate, (int)sys.hasShutdown, sys.path.c_str(),
                 sys.loaded ? "" : "  [LOAD FAILED]");

    for (const ServiceEntry* ui : sortedByName(m_uiScripts))
        LOG_INFO("  ui       %-24s %s%s", ui->name.c_str(), ui->path.c_str(),
                 ui->loaded ? "" : "  [LOAD FAILED]");

    // Disabled manifest entries never reach the registries above; without
    // these rows an accidental enabled:false diffs as a silently-absent entry.
    for (const std::string& d : m_manifestDisabled)
        LOG_INFO("  disabled %s", d.c_str());
}

void ScriptSystem::FillRegistrySnapshot(ScriptRegistrySnapshot& out) const
{
    out = {};
    out.bootedFromManifest = m_bootedFromManifest;
    out.globals            = m_globalScripts;
    out.disabled           = m_manifestDisabled;

    auto copyNamed = [](const std::unordered_map<std::string, ServiceEntry>& map,
                        std::vector<ScriptRegistrySnapshot::NamedRow>& rows)
    {
        rows.reserve(map.size());
        for (const auto& [name, e] : map)
            rows.push_back({ e.name, e.path, e.lastError, e.loaded });
        std::sort(rows.begin(), rows.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
    };
    copyNamed(m_services, out.services);
    copyNamed(m_uiScripts, out.uiScripts);

    out.systems.reserve(m_systems.size());
    for (const SystemEntry& s : m_systems)
    {
        ScriptRegistrySnapshot::SystemRow row;
        row.name        = s.name;
        row.path        = s.path;
        row.systemSet   = s.systemSet;
        row.lastError   = s.lastError;
        row.order       = s.order;
        row.loaded      = s.loaded;
        row.initCalled  = s.initCalled;
        row.hasUpdate   = s.hasUpdate;
        row.hasShutdown = s.hasShutdown;
        // Mirror Update's gate exactly (skip = !setActive): empty set or no
        // predicate means the system always ticks.
        row.setActive   = s.systemSet.empty() || !m_isSystemSetActive
                       || m_isSystemSetActive(s.systemSet);
        row.sceneScoped = s.sceneScoped;
        row.active      = s.active;
        out.systems.push_back(std::move(row));
    }

    // What each owner holds now; owner 0 is "(none)".
    {
        std::unordered_map<OwnerId, ScriptRegistrySnapshot::OwnerRow> rows;
        if (m_luaBus)
            for (const auto& s : m_luaBus->subs) if (s.active) ++rows[s.owner].subs;
        if (m_timers)
            for (const auto& t : m_timers->entries) if (t.active) ++rows[t.owner].timers;
        if (m_tasks)
            for (const auto& t : m_tasks->entries) if (t.active) ++rows[t.owner].tasks;
        for (const TimeRequest& r : m_timeRequests) ++rows[r.owner].timeRequests;
        for (const auto& [name, owner] : m_contextOwners)
            if (Input::Get().IsContextActive(name.c_str())) ++rows[owner].contexts;
        out.owners.reserve(rows.size());
        for (auto& [owner, row] : rows)
        {
            const auto rec = m_owners.find(owner);
            row.label = owner == 0 ? "(none)"
                      : rec != m_owners.end() ? rec->second.label
                      : "(released #" + std::to_string(owner) + ")";
            out.owners.push_back(std::move(row));
        }
        std::sort(out.owners.begin(), out.owners.end(),
                  [](const auto& a, const auto& b) { return a.label < b.label; });
    }

    // Instance tally per template path: total slots, OnSpawn-fired, parked.
    struct Tally { int total = 0, live = 0, faulted = 0; };
    std::unordered_map<std::string, Tally> tallies;
    for (const auto& [entity, state] : m_states)
        for (const LogicSlot& slot : state.slots)
        {
            if (slot.path.empty()) continue;
            Tally& t = tallies[slot.path];
            ++t.total;
            if (slot.initCalled) ++t.live;
            if (slot.faulted)    ++t.faulted;
        }

    out.logicTemplates.reserve(m_logicTemplates.size());
    for (const auto& [path, tmpl] : m_logicTemplates)
    {
        ScriptRegistrySnapshot::TemplateRow row;
        row.path         = tmpl.path;
        row.lastError    = tmpl.lastError;
        row.hasSpawn     = tmpl.hasSpawn;
        row.hasUpdate    = tmpl.hasUpdate;
        row.hasDestroy   = tmpl.hasDestroy;
        row.loadFailed   = tmpl.loadFailed;
        row.systemSet    = tmpl.systemSet;
        row.setActive    = tmpl.systemSet.empty() || !m_isSystemSetActive
                        || m_isSystemSetActive(tmpl.systemSet);
        row.exposedCount = static_cast<int>(tmpl.exposed.size());
        if (auto it = tallies.find(path); it != tallies.end())
        {
            row.instanceCount = it->second.total;
            row.liveCount     = it->second.live;
            row.faultedCount  = it->second.faulted;
        }
        out.logicTemplates.push_back(std::move(row));
    }
    std::sort(out.logicTemplates.begin(), out.logicTemplates.end(),
              [](const auto& a, const auto& b) { return a.path < b.path; });

    out.sceneScript.path             = m_sceneScriptPath;
    out.sceneScript.sceneName        = m_sceneScriptName;
    out.sceneScript.pendingPath      = m_pendingSceneScriptPath;
    out.sceneScript.pendingSceneName = m_pendingSceneScriptName;
    out.sceneScript.systems          = m_sceneSystems;
    out.sceneScript.pendingSystems   = m_pendingSceneSystems;
    out.sceneScript.pending          = m_sceneScriptPending;
    out.sceneScript.entered          = m_sceneScriptEntered;
    out.sceneScript.hasUpdate        = m_sceneScriptHasUpdate;
    out.sceneScript.hasExit          = m_sceneScriptHasExit;
}

// ==== Entity teardown (destroy listener, sweep) ====
void ScriptSystem::BindWorld(World& world)
{
    if (m_boundWorld == &world) return;
    UnbindWorld();
    m_boundWorld = &world;
    m_entityDestroyListenerId = world.AddEntityDestroyListener(
        [this, &world](Entity e) { OnEntityDestroyed(world, e); });
}

void ScriptSystem::UnbindWorld()
{
    if (m_boundWorld && m_entityDestroyListenerId != 0)
        m_boundWorld->RemoveEntityDestroyListener(m_entityDestroyListenerId);
    m_boundWorld              = nullptr;
    m_entityDestroyListenerId = 0;
}

void ScriptSystem::OnEntityDestroyed(World& /*world*/, Entity e)
{
    TeardownEntity(e);   // erase-first + re-entrancy-safe (see TeardownEntity)
}

void ScriptSystem::SweepDestroyed(World& world)
{
    // Collect dead entities first, then tear down: FireSlotDestroy runs OnDestroy
    // (Lua), which can mutate m_states and invalidate iterators. TeardownEntity
    // re-finds each entry, so an entry already cleaned by a prior callback is a
    // harmless no-op.
    std::vector<Entity> dead;
    for (auto& [e, st] : m_states)
        if (!world.IsAlive(e) || !world.HasComponent<ScriptComponent>(e))
            dead.push_back(e);
    for (Entity e : dead)
        TeardownEntity(e);
}

void ScriptSystem::DispatchLuaEvents()
{
    std::vector<LuaBus::Pending> processing;
    std::swap(processing, m_luaBus->pending);
    if (m_luaBus->dropped)
    {
        LOG_WARNING("Lua event bus: %zu event(s) dropped while no script update drained the queue",
                    m_luaBus->dropped);
        m_luaBus->dropped = 0;
    }

    // The set check costs two lookups per handler: skip it while nothing declares a set.
    bool anySet = false;
    for (const SystemEntry& s : m_systems)
        if (!s.systemSet.empty()) { anySet = true; break; }
    if (!anySet)
        for (const auto& [path, tmpl] : m_logicTemplates)
            if (!tmpl.systemSet.empty()) { anySet = true; break; }

    for (const LuaBus::Pending& ev : processing)
    {
        // Index + copy, not range-for: a handler may Engine.Subscribe, and the push_back
        // reallocation would dangle a `Sub&`. Handlers subscribed mid-dispatch first fire on the
        // NEXT dispatch.
        const std::size_t count = m_luaBus->subs.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!m_luaBus->subs[i].active || m_luaBus->subs[i].name != ev.name) continue;
            const OwnerId owner = m_luaBus->subs[i].owner;
            // A suspended set pauses the owner's handlers as well as its OnUpdate.
            if (anySet && OwnerSuspended(owner)) continue;
            sol::protected_function fn = m_luaBus->subs[i].fn;
            OwnerScope as(*this, owner);
            auto res = fn(ev.payload);
            if (!res.valid())
            {
                sol::error err = res;
                LOG_ERROR("Lua event handler '%s' error: %s", ev.name.c_str(), err.what());
            }
        }
    }
    m_luaBus->Compact();
}

// Depth-capped (kMaxBubbleDepth) so a cyclic Parent graph terminates. Callers
// (the contact bridges) have already borrowed the world.
Entity ScriptSystem::FindContactHandlerUpwards(Entity start, const char* fnName)
{
    constexpr int kMaxBubbleDepth = 8;

    World* w = m_world ? m_world : m_boundWorld;
    Entity e = start;
    for (int depth = 0; depth <= kMaxBubbleDepth && e != NullEntity; ++depth)
    {
        auto sIt = m_states.find(e);
        if (sIt != m_states.end())
        {
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, static_cast<uint32_t>(e), i);
                if (!inst.valid()) continue;
                sol::object fn = inst[fnName];
                if (fn.is<sol::protected_function>()) return e;
            }
        }
        if (!w) break;                                 // no world: cannot walk up
        const Parent* p = w->GetComponent<Parent>(e);
        e = p ? p->entity : NullEntity;
    }
    return NullEntity;
}

void ScriptSystem::SeedInputDeviceEdge()
{
    s_lastReportedDevice = InputDeviceKind::None;
    m_cppBridgeUnsubscribers.push_back([] { s_lastReportedDevice = InputDeviceKind::None; });
}

void ScriptSystem::Update(World& world, float dt)
{
    ZoneScopedN("ScriptSystem::Update");
    // The Lua heap, for the Tracy plot and the profiler's gauge. HERE rather
    // than beside the slot count below: Update returns early when no entity
    // has a ScriptComponent, and the VM (system / UI / scene scripts) exists
    // regardless. LUA_GCCOUNT is O(1); read only when someone is listening.
    if (m_lua && (Diag::Prof::GetLevel() != Diag::Prof::Level::Off || TracyIsConnected))
    {
        const int kib = lua_gc(m_lua->lua_state(), LUA_GCCOUNT, 0);
        TracyPlot("Lua.GCKiB", static_cast<int64_t>(kib));
        Diag::Prof::SetGauge(Diag::Prof::Gauge::LuaKB, kib);
    }
    m_elapsed += dt;
    m_world = &world;

    (*m_lua)["Time"]["dt"]      = dt;
    (*m_lua)["Time"]["elapsed"] = m_elapsed;

    // BEFORE the sweep: after a world swap the sweep sees recycled ids as alive and
    // would keep the previous world's instances (see m_lastWorldEpoch).
    DropInstancesForWorldSwap(world);
    FlushPendingDestroys();
    SweepDestroyed(world);

    // ---- Input device edges (memory re-armed in ClearAll / ClearInstances) ----
    // Published before DispatchLuaEvents so subscribers see a switch on the frame it
    // happened. Connect / disconnect use the backend's JustConnected /
    // JustDisconnected masks (bit N = slot N), computed once in Poll(), rather than a
    // second differ here that could drift from them.
    // This runs from ScriptLogicSystem (RunMask::Play), so a hotplug while the editor
    // is stopped raises no Lua event (the masks are one frame wide). Scripts that need
    // the current state on entry should call Input.IsGamepadConnected() in OnInit.
    {
        const InputDeviceKind device = Input::Get().LastDevice();
        if (device != s_lastReportedDevice)
        {
            // Publish only queues, and DispatchLuaEvents below runs before the systems'
            // OnInit pass. On the first tick after a world reload nobody has subscribed yet,
            // so the seeded republish would be lost with the edge already consumed. Gate on a
            // live listener instead of moving this after OnInit (which would delay every
            // normal switch by a frame): the republish then waits for the HUD to subscribe,
            // and no payload is built while nobody listens. The scan runs only on an edge.
            bool hasListener = false;
            for (const auto& s : m_luaBus->subs)
                if (s.active && s.name == "InputDeviceChanged") { hasListener = true; break; }

            if (hasListener)
            {
                s_lastReportedDevice = device;
                sol::table t = m_lua->create_table();
                t["device"] = ToString(device);
                m_luaBus->Publish("InputDeviceChanged", std::move(t));
            }
        }

        const GamepadDevice& pad        = Input::Get().Pad();
        const std::uint8_t connected    = pad.JustConnected();
        const std::uint8_t disconnected = pad.JustDisconnected();
        if (connected || disconnected)
        {
            for (int slot = 0; slot < GamepadDevice::kMaxPads; ++slot)
            {
                const std::uint8_t bit = static_cast<std::uint8_t>(1u << slot);
                if (connected & bit)
                {
                    sol::table t = m_lua->create_table();
                    t["slot"] = slot;
                    m_luaBus->Publish("GamepadConnected", std::move(t));
                }
                if (disconnected & bit)
                {
                    sol::table t = m_lua->create_table();
                    t["slot"] = slot;
                    m_luaBus->Publish("GamepadDisconnected", std::move(t));
                }
            }
        }
    }

    // ---- Time-of-day phase edge -> "TimeOfDayPhaseChanged" ----
    //   { isNight = bool, timeOfDay = 0..1 }   -- on an edge, not per frame
    // Polled and latched here, like the input-device edge: TODOutputComponent::dirty
    // is set every enabled frame (not an edge), and the TOD pipeline runs inside the
    // renderer (SyncSkyboxIBL), where an event would arrive a phase late. Lets a
    // street of lamps subscribe once instead of each polling IsNight(); polling stays
    // valid.
    {
        const TODConfigComponent* cfg = TODUtil::FindConfig(world);
        const TODOutputComponent* out =
            (cfg && cfg->enabled) ? TODUtil::FindOutput(world) : nullptr;
        // -1 = no TOD authority in this scene (no entity, or disabled).
        const int phase = out ? (out->isMoonActive ? 1 : 0) : -1;

        if (phase != s_lastTodPhase)
        {
            if (phase < 0)
            {
                // Losing TOD authority is not announced: just forget, so re-enabling
                // republishes the real phase.
                s_lastTodPhase = phase;
            }
            else
            {
                // Gate on a live listener and advance the latch only when publishing, as for the
                // device edge: on the reload tick the subscribers are wiped and OnInit has not
                // re-run, so an ungated publish would be lost.
                bool hasListener = false;
                for (const auto& s : m_luaBus->subs)
                    if (s.active && s.name == "TimeOfDayPhaseChanged") { hasListener = true; break; }

                if (hasListener)
                {
                    s_lastTodPhase = phase;
                    sol::table t = m_lua->create_table();
                    t["isNight"]   = (phase == 1);
                    t["timeOfDay"] = cfg->timeOfDay;
                    m_luaBus->Publish("TimeOfDayPhaseChanged", std::move(t));
                }
            }
        }
    }

    { ZoneScopedN("Lua.DispatchEvents"); DispatchLuaEvents(); }

    // Anim notifies queued by the render-phase CustomNotifyEvent bridge, fired here
    // with m_world valid (see AnimEventQueue in the header). Right after the LuaBus,
    // so an "AnimNotify" System listener and the entity's OnAnimEvent see the same
    // frame.
    DispatchPendingAnimEvents();

    // ---- Scene script: deferred enter/exit swap, then OnSceneUpdate ----
    // Runs with m_world valid, so a scene load requested last frame enters before
    // this frame's systems and Logic.
    ProcessSceneScriptSwap();
    TickSceneScript(dt);

    // Advance transform tweens (Engine.MoveTo/RotateTo/ScaleTo) on game dt.
    TickTweens(dt);
    TickGameTimers(dt);

    // ---- Systems: OnInit once, OnUpdate every frame ----
    if (!m_dispatchCache) m_dispatchCache = std::make_unique<DispatchCache>();
    m_dispatchCache->systems.resize(m_systems.size());
    for (std::size_t sysIdx = 0; sysIdx < m_systems.size(); ++sysIdx)
    {
        auto& sys = m_systems[sysIdx];
        if (!sys.loaded || !sys.active) continue;   // inactive: a scene System no entered scene lists
        if (!sys.systemSet.empty() && m_isSystemSetActive
            && !m_isSystemSetActive(sys.systemSet))
        {
            continue;
        }
        // Cached __systems[name] + OnUpdate (no per-frame string-keyed walk); local
        // copies for the same reason as the Logic slots below.
        auto& ce = m_dispatchCache->systems[sysIdx];
        if (ce.gen != m_luaDispatchGen || !ce.inst.valid())
        {
            ce.inst = (*m_lua)["__systems"][sys.name];
            ce.upd  = ce.inst.valid() ? sol::protected_function(ce.inst["OnUpdate"])
                                      : sol::protected_function{};
            ce.gen  = m_luaDispatchGen;
        }
        sol::table inst = ce.inst;
        if (!inst.valid()) continue;

        // OnInit / OnUpdate may load or remove Systems (an -exec driver), reallocating m_systems:
        // no reference is held across a call, and the entry is re-found by name after it.
        const std::string name = sys.name;
        if (!sys.initCalled)
            InitSystem(sys);
        if (sysIdx >= m_systems.size() || m_systems[sysIdx].name != name) break;
        if (!m_systems[sysIdx].hasUpdate || !m_systems[sysIdx].active) continue;

        sol::protected_function upd = ce.upd;
        if (!upd.valid()) { m_systems[sysIdx].hasUpdate = false; continue; }
        OwnerScope as(*this, m_systems[sysIdx].owner);
        auto res = upd(inst, dt);
        if (!res.valid())
        {
            sol::error err = res;
            LOG_ERROR("Lua OnUpdate error [%s]: %s", name.c_str(), err.what());

            // Record a run-level fault before parking the system: hasUpdate = false lasts
            // until the editor's Stop (ClearInstances) or a hot reload of the script, so a
            // Game.exe run would keep rendering to -frames=N with a dead system and exit 0. The
            // counter turns that into a failed run.
            Harness::RunLedger::Get().NoteScriptFault(name.c_str(), err.what());

            for (SystemEntry& s : m_systems)
                if (s.name == name) s.hasUpdate = false;   // stop calling broken update
        }
    }

    // ---- Logic: per-entity instances ----
    // Iterate the ScriptComponent pool, not world.GetEntities() (which walks every
    // entity and filters). Snapshot the entity list: OnSpawn may Engine.AttachScript
    // to other entities, pushing into the pool's dense vector mid-iteration; new
    // attachments are picked up next frame.
    auto* scPool = world.GetPool<ScriptComponent>();
    if (!scPool || scPool->Entities().empty()) { m_world = nullptr; return; }

    // thread_local so the snapshot's capacity survives across frames instead
    // of a fresh heap allocation per frame (main-thread system; the
    // thread_local is belt-and-braces, not a concurrency claim).
    static thread_local std::vector<Entity> t_entitiesSnapshot;
    t_entitiesSnapshot.assign(scPool->Entities().begin(), scPool->Entities().end());
    const std::vector<Entity>& entitiesSnapshot = t_entitiesSnapshot;

    // Every Lua callback below (OnSpawn / OnUpdate / teardown OnDestroy) can,
    // via Engine.DestroyEntity, synchronously erase this entity's m_states entry
    // (World fires destroy-listeners inline) or reallocate sc->scripts (a script
    // that AttachScripts to itself). So we hold NO long-lived references across a
    // callback: the component pointer and the m_states iterator are re-fetched
    // after every step, and scalar fields are copied out before use.
    for (Entity e : entitiesSnapshot)
    {
        if (!world.IsAlive(e)) continue;
        auto* sc = scPool->Get(e);
        if (!sc) continue;

        // Ensure per-entity state + capture baseline transform once (shared by
        // all scripts). No Lua runs here, so this short-lived ref is safe.
        {
            auto& st = m_states[e];
            if (!st.baseCaptured)
            {
                if (const LocalTransform* lt = world.GetComponent<LocalTransform>(e))
                    st.baseTransform = *lt;
                st.baseCaptured = true;
            }
        }

        const std::size_t n = sc->scripts.size();

        // Reconcile the slot count. Tearing down trailing slots fires OnDestroy, which
        // may destroy this entity: re-find after each and bail if gone.
        {
            auto it = m_states.find(e);
            if (it == m_states.end()) continue;
            if (it->second.slots.size() > n)
            {
                const std::size_t old = it->second.slots.size();
                for (std::size_t i = n; i < old; ++i)
                {
                    DestroyLogicInstance(e, i);
                    if (m_states.find(e) == m_states.end()) break;  // self-destroyed
                }
                it = m_states.find(e);
                if (it == m_states.end()) continue;
                it->second.slots.resize(n);
            }
            else if (it->second.slots.size() < n)
            {
                it->second.slots.resize(n);
            }
        }

        // Drive each slot independently.
        for (std::size_t i = 0; i < n; ++i)
        {
            sc = scPool->Get(e);
            auto it = m_states.find(e);
            if (!sc || it == m_states.end()) break;             // entity gone
            if (i >= sc->scripts.size() || i >= it->second.slots.size()) break;

            const std::string path    = sc->scripts[i].scriptPath;  // copy
            const bool        enabled = sc->scripts[i].enabled;

            // Path changed at this slot (first sight, edited path, or a Remove shifted
            // entries up): tear down the old instance and reset the metadata.
            if (it->second.slots[i].path != path)
            {
                if (!it->second.slots[i].path.empty())
                {
                    DestroyLogicInstance(e, i);            // fires OnDestroy (Lua)
                    it = m_states.find(e);
                    if (it == m_states.end()) break;
                    if (i >= it->second.slots.size()) break;
                }
                it->second.slots[i] = {};
                it->second.slots[i].path = path;
            }

            if (path.empty() || !enabled) continue;

            // A slot whose OnUpdate threw stays parked until Stop/Play or a hot reload
            // (runtime-only, never written to ScriptComponent).
            if (it->second.slots[i].faulted) continue;

            // Load the template if needed. Presence in m_logicTemplates is not "loaded":
            // LoadLogicTemplate inserts the entry before it can fail and keeps a last-good
            // prototype on failure. Skip a path that never loaded so it is not re-run (and
            // re-logged) every frame; it is not hot-reload-tracked either (see
            // LogicTemplate::loadFailed).
            {
                auto tIt = m_logicTemplates.find(path);
                if (tIt == m_logicTemplates.end())
                {
                    if (!LoadLogicTemplate(path)) continue;
                }
                else if (tIt->second.loadFailed && !(*m_lua)["__logic_templates"][path].valid())
                {
                    continue;   // never loaded successfully: not re-run this session
                }
            }

            // Ensure instance + fire OnSpawn once for this slot. The vars
            // pointer is consumed (InjectExposedVars) before OnSpawn runs, so it
            // is safe even though OnSpawn may later reallocate the vector. A
            // failed spawn leaves initCalled=false (retry next frame) and skips
            // OnUpdate this frame.
            if (!it->second.slots[i].initCalled)
            {
                const bool ok = EnsureLogicInstance(e, i, path, &sc->scripts[i].vars);
                it = m_states.find(e);
                if (it == m_states.end()) break;          // OnSpawn destroyed e
                if (i >= it->second.slots.size()) break;
                if (!ok) continue;
                it->second.slots[i].initCalled = true;
            }

            // Per-frame OnUpdate(self, dt), gated by the template's GameFlow SystemSet if it
            // declared one: the slot stays spawned and resumes where it left off, which is
            // what a Paused state wants.
            auto tmplIt = m_logicTemplates.find(path);
            if (tmplIt != m_logicTemplates.end()
                && !tmplIt->second.systemSet.empty() && m_isSystemSetActive
                && !m_isSystemSetActive(tmplIt->second.systemSet))
            {
                continue;
            }
            if (tmplIt != m_logicTemplates.end() && tmplIt->second.hasUpdate)
            {
                // Self-validating dispatch cache (FNV-1a path hash): the path hash catches slot
                // edits and Remove-shifts, the entity generation catches id recycling, the gen
                // catches hot reloads and session resets.
                uint32_t pathHash = 2166136261u;
                for (const char c : path) { pathHash ^= uint8_t(c); pathHash *= 16777619u; }
                const uint32_t entGen = world.GetGeneration(e);
                const uint64_t key    = (uint64_t(e) << 16) | uint64_t(i & 0xFFFFu);
                auto& ce = m_dispatchCache->slots[key];
                if (ce.gen != m_luaDispatchGen || ce.pathHash != pathHash
                    || ce.entityGen != entGen || !ce.inst.valid())
                {
                    ce.inst = GetInstance(*m_lua, static_cast<uint32_t>(e), i);
                    ce.upd  = ce.inst.valid()
                            ? sol::protected_function(ce.inst["OnUpdate"])
                            : sol::protected_function{};
                    ce.gen       = m_luaDispatchGen;
                    ce.pathHash  = pathHash;
                    ce.entityGen = entGen;
                }
                // Local copies before the call: OnUpdate may destroy this entity (erasing cache
                // rows) or reload scripts mid-call, and a registry-ref copy survives where `ce`
                // does not. Do not touch `ce` past this point.
                sol::table inst = ce.inst;
                if (!inst.valid()) continue;
                sol::protected_function fn = ce.upd;
                if (!fn.valid()) continue;
                OwnerScope as(*this, it->second.slots[i].owner);
                auto res = fn(inst, dt);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnUpdate error [%s] entity %u slot %zu: %s",
                              path.c_str(), e, i, err.what());
                    // Park only this slot (other scripts on the entity keep running), with a runtime
                    // flag rather than the serialized `enabled`, which a Save World during Play would
                    // bake into the scene. Re-fetch the state: OnUpdate may have reallocated the pool
                    // or destroyed the entity.
                    auto stIt = m_states.find(e);
                    if (stIt != m_states.end() && i < stIt->second.slots.size())
                    {
                        stIt->second.slots[i].faulted   = true;
                        stIt->second.slots[i].lastError = err.what();
                    }
                }
            }
        }
    }

    // Scale signal for Tracy: the Logic slot count.
    {
        std::size_t slots = 0;
        for (const auto& [e, st] : m_states) slots += st.slots.size();
        TracyPlot("Lua.LogicSlots", static_cast<int64_t>(slots));
        // Keep TracyPlot calls braced: with TRACY_ENABLE off it expands to nothing, and
        // an unbraced `if (...) TracyPlot(...);` becomes an empty statement (C4390). The
        // Lua heap is plotted at the top of Update: it is about the VM, which exists
        // whether or not any entity has a script.
    }

    m_world = nullptr;
}
