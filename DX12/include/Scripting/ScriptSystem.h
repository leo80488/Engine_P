#pragma once

// ScriptSystem - owns the Lua VM and the script categories of
// docs/design/Script_Architecture_Reference.md:
//   Logic   - entity-bound: one template per .lua file, one instance per
//             ScriptComponent slot. The file returns a table; the engine calls
//             inst:OnSpawn(entity), inst:OnUpdate(dt), inst:OnDestroy().
//   System  - global singleton returning a table with OnInit / OnUpdate / OnShutdown.
//   Service - global singleton, a stateless table of functions (UI scripts share
//             the shape).
//   Scene   - one script bound to the active scene (OnSceneEnter / OnSceneUpdate /
//             OnSceneExit).
// Config (Lua-as-data) is Engine.LoadConfig(path): no engine-side storage.
// All sol::table state lives in Lua tables (__logic_templates, __logic_instances,
// __systems, __services, __ui_scripts, __scene_script), so this header is sol-free.

#include "ECS/ECS.h"
#include "ECS/HierarchyComponents.h"    // LocalTransform (LogicState::baseTransform)
#include "Scripting/ScriptExposedVar.h" // ScriptVarValue / ScriptVarDesc
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>

// Forward declare sol types to avoid pulling the heavy header here.
namespace sol { class state; }
struct PropertyBag; // ECS/NotifyTypes.h: AnimNotify payload
namespace Scripting { struct ScriptManifest; } // Scripting/ScriptManifest.h
namespace Gameplay::Damage { struct Spec; struct Verdict; } // Gameplay/Damage.h

class ScriptSystem
{
public:
    // RAII borrow of the bound world for code that runs Lua outside Update()
    // (contact bridges, TickTimers, scene exit): sets m_world to m_boundWorld when it
    // is null and restores it on exit. Without it every Engine.* call in that Lua is a
    // guarded no-op. Scope it around the Lua call, not the whole handler.
    // Kept separate from DeferDestroys on purpose: TickTimers borrows but must not
    // defer (it is the flush point, and a timer that destroys its own entity has to
    // take effect), while the contact bridges need both.
    struct WorldBorrow
    {
        explicit WorldBorrow(ScriptSystem& s) : m_s(s), m_saved(s.m_world)
        {
            if (!m_s.m_world) m_s.m_world = m_s.m_boundWorld;
        }
        ~WorldBorrow() { m_s.m_world = m_saved; }
        WorldBorrow(const WorldBorrow&)            = delete;
        WorldBorrow& operator=(const WorldBorrow&) = delete;

    private:
        ScriptSystem& m_s;
        World*        m_saved;
    };

    // Queue DestroyEntity instead of running it synchronously, for spans where
    // a synchronous destroy is a use-after-free in the caller's stack (the contact
    // bridges, BT ticks). Drains at the next script phase.
    struct DeferDestroys
    {
        explicit DeferDestroys(ScriptSystem& s) : m_s(s) { m_s.BeginDeferredDestroyWindow(); }
        ~DeferDestroys() { m_s.EndDeferredDestroyWindow(); }
        DeferDestroys(const DeferDestroys&)            = delete;
        DeferDestroys& operator=(const DeferDestroys&) = delete;

    private:
        ScriptSystem& m_s;
    };

    // ---- Script ownership ----
    // Who created a Lua-owned resource (a bus subscription, an AfterDelay timer, a time-scale
    // request, an input-context push, a tween's onDone): the System, scene script or Logic slot
    // whose code was running. When the owner ends (a System's teardown, the scene's exit, a Logic
    // slot's destroy) everything it owns is released with it. 0 = no owner (BT action libraries,
    // pipe chunks, Service and UI code called from no owner): released only by ClearInstances /
    // ClearAll.
    using OwnerId = std::uint64_t;

    // The owner whose Lua is running (0 outside any script).
    OwnerId CurrentOwner() const { return m_currentOwner; }
    // Code that keeps something per owner outside ScriptSystem (the game flow's pauses) hears when
    // an owner ends. @p everything: a Lua reload or the editor's Stop released all script-owned
    // resources at once.
    using OwnerReleaseHook = std::function<void(OwnerId owner, bool everything)>;
    void AddOwnerReleaseHook(OwnerReleaseHook hook) { m_ownerReleaseHooks.push_back(std::move(hook)); }

    // ---- Read-only registry snapshot (editor Script Registry panel) ----
    struct ScriptRegistrySnapshot
    {
        struct SystemRow
        {
            std::string name;        // file stem = registration id
            std::string path;
            std::string systemSet;   // empty = not gated
            std::string lastError;
            int32_t     order       = 0;
            bool        loaded      = false;
            bool        initCalled  = false;
            bool        hasUpdate   = false;
            bool        hasShutdown = false;
            bool        setActive   = true;  // GameFlow gate result right now
            bool        sceneScoped = false; // scope = "scene": runs only while a scene lists it
            bool        active      = true;  // engine scope, or listed by the entered scene
        };
        struct OwnerRow
        {
            std::string label;
            int subs = 0, timers = 0, timeRequests = 0, contexts = 0, tasks = 0;
        };
        struct NamedRow
        {
            std::string name;
            std::string path;
            std::string lastError;
            bool        loaded = false;
        };
        struct TemplateRow
        {
            std::string path;        // Logic templates are path-keyed
            std::string lastError;
            bool hasSpawn = false, hasUpdate = false, hasDestroy = false;
            bool loadFailed = false; // entry may still hold a last-good prototype
            std::string systemSet;   // empty = not gated
            bool setActive  = true;  // GameFlow gate result right now
            int  exposedCount  = 0;
            int  instanceCount = 0;  // slots referencing this template
            int  liveCount     = 0;  // of those, OnSpawn already fired
            int  faultedCount  = 0;  // of those, parked by a thrown OnUpdate
        };
        struct SceneScriptRow
        {
            std::string path, sceneName, pendingPath, pendingSceneName;
            std::vector<std::string> systems, pendingSystems;   // scene-scoped Systems
            bool pending = false, entered = false;
            bool hasUpdate = false, hasExit = false;
        };

        bool                     bootedFromManifest = false;
        std::vector<std::string> globals;        // load order
        std::vector<NamedRow>    services;       // name-sorted
        std::vector<SystemRow>   systems;        // tick order
        std::vector<NamedRow>    uiScripts;      // name-sorted
        std::vector<std::string> disabled;       // "[kind] path" manifest rows
        std::vector<TemplateRow> logicTemplates; // path-sorted
        SceneScriptRow           sceneScript;
        std::vector<OwnerRow>    owners;         // label-sorted; only owners holding something
    };

    // ---- System scripts (singleton, ordered, OnInit/OnUpdate/OnShutdown) ----
    using SystemSetActivePredicate = std::function<bool(std::string_view)>;
    using SystemSetRegistrar       = std::function<void(std::string_view)>;

    // ---- Lifetime, update and Lua access ----
    ScriptSystem();
    ~ScriptSystem();

    void Initialize();
    void Update(World& world, float dt);
    void CheckHotReload(World& world);
    void ClearAll();

    // Lighter reset than ClearAll, for the editor's Stop: drops per-entity Logic
    // instances and the transient queues (AfterDelay timers, tweens, Lua event bus,
    // pending destroys and anim events), resets the time scale and input contexts,
    // and re-arms System OnInit / OnUpdate and the scene script's handlers. Keeps the
    // boot-loaded templates, systems, services, UI scripts and the scene-script
    // binding, so the next Play starts from fresh instances without a re-scan.
    void ClearInstances();

    // Borrowed access to the live sol::state. Used by the BT runtime / UI
    // bindings to share a single Lua VM.
    // Null until Initialize() runs; callers must guard.
    sol::state* GetLua() const { return m_lua; }

    // The world visible to Lua bindings RIGHT NOW (set during Update, and
    // borrowed for timers / event bridges / BT ticks / GameFlow hooks).
    // Component proxy userdata re-fetches through this on every property
    // access so a proxy cached across frames can never dangle.
    World* LuaWorld() const { return m_world; }

    // ---- Deferred-destroy window ----
    // While the depth is non-zero, Engine.DestroyEntity queues instead of destroying;
    // the queue drains at the next script phase (TickTimers / Update). Opened around the
    // contact bridges (the EventDelivery drain after the animation chain: a synchronous
    // destroy would erase the script state OnEnter is walking, and drop an entity this
    // frame's skin jobs name) and BT ticks (a destroy mid-tick relocates pool storage the
    // AI iteration still points at).
    void BeginDeferredDestroyWindow() { ++m_deferDestroyDepth; }
    void EndDeferredDestroyWindow()
    {
        if (m_deferDestroyDepth > 0) --m_deferDestroyDepth;
    }

    // ---- Global scripts (BT action libraries) ----
    // Runs the file on the shared VM (top-level code defines Actions.* /
    // Conditions.* entries) and registers it for hot reload: on an mtime change
    // CheckHotReload re-runs the file, redefining its handlers in place.
    bool AddGlobalScript(const std::string& path);

    // Binds @p world as m_world for Lua that runs outside Update() (GameFlow OnEnter /
    // OnExit hooks from the Input-phase resolver, BT ticks), where every Engine.* entity
    // API would otherwise be a guarded no-op. Returns the previous value; the caller
    // restores it afterwards.
    World* ExchangeBoundWorld(World* world)
    {
        World* previous = m_world;
        m_world = world;
        return previous;
    }

    // ---- Game time scale ----
    // The effective scale (GetTimeScale) is the legacy factor times the strongest slow-down among
    // the Sequencer's TimeDilation stack and every live Lua request (Time.RequestScale), so a
    // pause menu, a hit-stop and a slow-motion shot cannot overwrite each other.
    // SetTimeScale is Engine.SetTimeScale's legacy factor; negative clamps to 0.
    void  SetTimeScale(float s);
    float GetLegacyTimeScale() const        { return m_legacyTimeScale; }
    // The Sequencer's resolved TimeDilation stack (Seq::TimeScale's sink).
    void  SetSequencerTimeScale(float s);
    float GetTimeScale() const              { return m_timeScale; }

    // The real-time services: publishes Time.realDt / Time.realElapsed, expires timed time
    // requests, ticks the real-domain Engine.AfterDelay / Every timers and Engine.Run tasks and
    // flushes queued destroys. Pass REAL (unscaled) dt so hit-stop timers expire even while the
    // game clock stands still; ScriptTimerSystem runs it outside every SystemSet.
    void TickTimers(float realDt);

    // ---- Boot scan ----
    // Loads root/services, root/systems and root/ui/*.lua (direct children,
    // alphabetical; from the pak index when one is mounted). Logic templates are not
    // scanned: they load on first sight in a ScriptComponent (entity scripts can live
    // anywhere). Systems are then sorted by (order, name): a System table may declare
    // an integer `order`, unset means 0, so order-less systems stay alphabetical.
    void ScanScriptDirectory(const std::string& root);

    // Manifest-driven boot, the alternative to ScanScriptDirectory: entries arrive
    // validated and (stage, order, stem)-sorted from LoadScriptManifest and go to the
    // same per-kind loaders, so registration ids stay filename stems. App picks
    // exactly one of the two per boot.
    void LoadBootScripts(const Scripting::ScriptManifest& manifest);

    // Logs one table: what booted, as which kind, in what order (systems in tick
    // order). Called by FinalizeBootRegistry at the end of either boot path; safe to
    // call again from debug code.
    void DumpBootRegistry() const;

    // ---- Read-only registry snapshot (editor Script Registry panel) ----
    // Plain copied-out data: the internal containers stay private because m_systems
    // is index-parallel to the dispatch cache and rewritten in place by hot reload,
    // so handed-out references would dangle. Cheap enough to refill every frame the
    // panel is open.
    void FillRegistrySnapshot(ScriptRegistrySnapshot& out) const;

    // ---- System scripts (singleton, ordered, OnInit/OnUpdate/OnShutdown) ----
    // Loading a script whose stem is already registered tears the old system down
    // (OnShutdown) and replaces it in its tick slot. The teardown comes first, so a
    // load that fails leaves that system unloaded.
    bool LoadSystem(const std::string& path);
    bool RemoveSystem(const std::string& name);

    // Optional GameFlow SystemSet membership for global Lua System scripts.
    // A returned System table may declare `set = "GameplaySimulation"`, or a
    // bootstrap script may assign it by name through GameFlow.AssignSystemSet.
    void SetSystemSetActivePredicate(SystemSetActivePredicate predicate)
    {
        m_isSystemSetActive = std::move(predicate);
    }
    void SetSystemSetRegistrar(SystemSetRegistrar registrar);
    bool AssignSystemToSet(const std::string& systemName,
                           const std::string& setName);
    bool ValidateSystemSetAssignments() const;
    std::vector<std::pair<std::string, std::string>> GetSystemSetAssignments() const;

    // ---- Service scripts (singleton, stateless, no callbacks) ----
    bool LoadService(const std::string& path);

    // ---- UI scripts (singleton, Service-shaped; scripts/ui/) ----
    // Loaded by the boot scan or the manifest. Callers fetch one with
    // Engine.GetUIScript(name) and drive its :Open() / :Close().
    bool LoadUIScript(const std::string& path);

    // ---- Scene script and scene-scoped Systems (bound to the active scene) ----
    // One scene script at a time: a Lua table with OnSceneEnter(self, sceneName) /
    // OnSceneUpdate(self, dt) / OnSceneExit(self). @p sceneSystems names the scene-scoped
    // Systems (`scope = "scene"`) the scene runs; they get a fresh instance and OnInit on entry,
    // before OnSceneEnter, and OnShutdown on exit. SceneManager calls this on every scene load
    // (an empty @p path = the new scene has no script). The enter is deferred to the next
    // Update() so it runs with m_world valid and under the editor's play/pause gating (it fires
    // on the first played frame).
    void SetActiveSceneScript(const std::string& path, const std::string& sceneName,
                              const std::vector<std::string>& sceneSystems = {});
    const std::string& ActiveSceneScript() const { return m_sceneScriptPath; }
    const std::vector<std::string>& ActiveSceneSystems() const { return m_sceneSystems; }

    // Ends the entered scene now (when one is entered): OnSceneExit, then OnShutdown of its
    // scene-scoped Systems in reverse tick order, then everything they own is released. Call it
    // while the outgoing world is still alive (SceneManager before LoadScene). @p worldSwap also
    // drops every Logic instance, without OnDestroy, and what each owns, because the world is
    // about to be cleared.
    void ExitScene(bool worldSwap);

    // The editor's World Settings changed the scene's script or Systems: replace the queued
    // binding (Edit mode), or during Play the one Stop queues for the next Play, keeping the scene
    // name. The running scene is not touched.
    void RebindScene(const std::string& path, const std::vector<std::string>& sceneSystems);

    // Editor Play / Stop. BeginPlaySession remembers the scene binding Play starts from;
    // EndPlaySession exits the entered scene (Logic instances are ClearInstances' job) and queues
    // that binding again, so the next Play enters the editor's scene with fresh instances, also
    // after a Scene.Load during Play.
    void BeginPlaySession();
    void EndPlaySession();

    // ---- Animation event dispatch (AnimationSystem / gameplay) ----
    // Fires OnAnimEvent(self, name, payload) synchronously on each initialized Logic
    // slot of @p e. Lua equivalent: Engine.PublishAnimEvent(entityId, name, payload).
    // The PropertyBag overload converts the bag to a Lua table (int/float/string
    // values; vectors become {x,y,z[,w]} tables); a null bag gives a nil payload.
    void DispatchAnimEvent(Entity e, const std::string& name);
    void DispatchAnimEvent(Entity e, const std::string& name,
                           const PropertyBag* params);

    // ---- Editor-exposed script variables ----
    // Returns the parsed schema (type, default, UI metadata) of a Logic script's
    // `exposed` table, loading the template on first request so the inspector can show
    // variables before the script ticks. Empty for a script without an `exposed`
    // table, or when the load fails.
    const std::vector<ScriptVarDesc>& GetExposedSchema(const std::string& path);

    // Pushes one slot's exposed-var values onto its live Lua instance (the editor,
    // for inspector edits during Play). No-op while the slot has no live instance
    // (edit mode): the values persist on the ScriptComponent and are injected at
    // spawn. @p overrides is the slot's var map; absent keys use schema defaults.
    void ApplyExposedVars(Entity e, std::size_t slot,
                          const std::unordered_map<std::string, ScriptVarValue>& overrides);

    // Editor introspection: the last runtime error of entity @p e's Logic slot
    // @p slot, or nullptr when healthy or unknown. @p faulted (optional) reports
    // whether the slot is parked by a thrown OnUpdate.
    const std::string* GetLogicSlotError(Entity e, std::size_t slot,
                                         bool* faulted = nullptr) const
    {
        auto it = m_states.find(e);
        if (it == m_states.end() || slot >= it->second.slots.size()) return nullptr;
        const auto& s = it->second.slots[slot];
        if (faulted) *faulted = s.faulted;
        return s.lastError.empty() ? nullptr : &s.lastError;
    }

    // ---- World binding ----
    // Wires a listener onto `world` so OnDestroy fires synchronously with
    // World::DestroyEntity instead of on next Update's poll. Without this, a
    // CreateEntity that recycles the freed ID in the same frame as the destroy
    // would leave the old entity's Lua state bound to the new entity until
    // SweepDestroyed catches up.
    void BindWorld(World& world);
    void UnbindWorld();

private:
    // Per-slot Logic metadata, one per script attached to an entity. The instance
    // table itself is (*m_lua)["__logic_instances"][entity][slot+1].
    struct LogicSlot
    {
        bool        initCalled = false;     // OnSpawn fired for this slot
        // Set when this slot's OnUpdate threw. Runtime-only on purpose:
        // ScriptComponent::scripts[i].enabled is serialized, so parking through it would
        // let a Save World during Play bake the fault into the scene. Cleared by
        // ClearInstances (editor Stop) and by a successful hot reload.
        bool        faulted    = false;
        std::string path;                   // which template this slot's instance came from
        std::string lastError;
        OwnerId     owner      = 0;         // this instance's resources; 0 until it is created
    };

    // Per-entity Logic state. The baseline transform is shared across all of
    // the entity's scripts; each attached script has its own LogicSlot.
    struct LogicState
    {
        // Snapshot of the entity's LocalTransform taken the first time any of its
        // scripts runs. Scripts that oscillate around a rest pose (bob, orbit, breathe)
        // should read Engine.GetBasePosition() and write absolute values, not integrate.
        // Shared by every slot; kept across hot reloads.
        LocalTransform baseTransform;
        bool           baseCaptured = false;

        // One slot per ScriptComponent::scripts entry, index-aligned with it.
        std::vector<LogicSlot> slots;
    };

    // Per-path template metadata. Prototype table lives in
    // (*m_lua)["__logic_templates"][path].
    struct LogicTemplate
    {
        std::string path;
        bool        hasSpawn   = false;
        bool        hasUpdate  = false;
        bool        hasDestroy = false;
        // True when the last load attempt failed. The entry exists anyway (and may
        // still hold a last-good prototype), so presence in the map does not mean loaded:
        // Update skips a path with no prototype instead of re-running it every frame. A
        // successful reload clears it. Only a path that loaded once is hot-reload-tracked
        // (m_fileTimestamps), so a file whose first load failed is not re-run this session.
        bool        loadFailed = false;
        std::string lastError;

        // Optional GameFlow SystemSet from `T.set`; empty = never gated. Gates OnUpdate
        // only: a gated-off slot still spawns and tears down, so suspending the set
        // freezes the script instead of leaving it uninitialised. (A global System's gate
        // skips the whole entry: it has no per-entity wiring to lose.)
        std::string systemSet;

        // Parsed `exposed` table, the editor-visible variable schema; empty when the
        // script declares none. Sorted by name so the inspector layout is stable across
        // loads.
        std::vector<ScriptVarDesc> exposed;
    };

    // System / Service / UI entries; the tables live in Lua under
    // __systems[name] / __services[name] / __ui_scripts[name]. A scene-scoped System's file
    // table is its prototype (__system_protos[name]); __systems[name] holds the instance of the
    // current activation and is nil while no entered scene lists it.
    struct SystemEntry
    {
        std::string name;       // file stem
        std::string path;
        bool        loaded      = false;
        bool        initCalled  = false;
        bool        hasUpdate   = false;
        bool        hasShutdown = false;
        int32_t     order       = 0;    // optional `order` (table or manifest); ties sort by name
        std::string systemSet;  // empty = not gated by GameFlow
        std::string lastError;
        bool        sceneScoped = false;  // `scope = "scene"` (table or manifest)
        bool        active      = true;   // engine scope, or listed by the entered scene
        OwnerId     owner       = 0;      // the current load (engine) or activation (scene)
    };

    // ---- Script ownership ----
    // World: what a Logic slot leaves to run on after it ends (a cleanup timer for an entity it
    // spawned), kept until the world is replaced.
    enum class OwnerKind : std::uint8_t { System, SceneScript, Logic, World };
    struct OwnerRecord
    {
        OwnerKind   kind = OwnerKind::System;
        std::string name;    // System stem, scene script path or Logic template path
        std::string label;   // logs and the registry panel
    };
    // Makes @p owner the current one for its lifetime: wraps every engine -> Lua entry point so
    // what that Lua creates is attributed. Restores on unwind, so an error cannot leak it.
    struct OwnerScope
    {
        OwnerScope(ScriptSystem& s, OwnerId owner) : m_s(s), m_saved(s.m_currentOwner)
        {
            m_s.m_currentOwner = owner;
        }
        ~OwnerScope() { m_s.m_currentOwner = m_saved; }
        OwnerScope(const OwnerScope&)            = delete;
        OwnerScope& operator=(const OwnerScope&) = delete;

    private:
        ScriptSystem& m_s;
        OwnerId       m_saved;
    };

    // ---- Game time scale ----
    struct TimeRequest
    {
        std::uint64_t id;
        OwnerId       owner;
        float         scale;
        float         remaining;   // real seconds; negative = until released
    };
    // Time.RequestEntityScale: one entity's own slow-down, on top of the game clock.
    struct EntityTimeRequest
    {
        std::uint64_t id;
        OwnerId       owner;
        Entity        entity;      // as the script named it; its animated descendant is resolved on write
        std::uint32_t generation;
        float         scale;
        float         remaining;   // real seconds; negative = until released
    };
    struct ServiceEntry
    {
        std::string name;       // file stem
        std::string path;
        bool        loaded      = false;
        std::string lastError;
    };

    // ---- Per-frame dispatch cache ----
    struct DispatchCache;

    struct LuaBus;                          // pImpl: holds sol::* storage

    struct ManifestSystemMeta
    {
        std::string path;
        std::string set;
        int32_t     order    = 0;
        bool        hasOrder = false;  // only a declared manifest order overrides a table `order`
        bool        sceneScoped = false;
        bool        hasScope    = false;  // likewise for `scope`
    };

    // ---- Deferred destroys (see BeginDeferredDestroyWindow) ----
    struct PendingDestroy { Entity e; uint32_t gen; };

    // ---- Deferred anim events ----
    struct AnimEventQueue;                  // pImpl: the payload is a sol::object

    // ---- Timers, tasks and tweens ----
    struct TimerQueue;
    struct TaskQueue;

    struct TweenQueue;

    // ---- Internal helpers ----
    void RegisterBindings();
    // The Time.* functions (ScriptSystem_Time.cpp); RegisterBindings calls it after making Time.
    void RegisterTimeBindings();
    // Engine.AfterDelay / Every / CancelDelay / Run / CancelRun and the Wait.* helpers
    // (ScriptSystem_Timers.cpp).
    void RegisterTimerBindings();
    // Fires the due timers of one clock; real = TickTimers' (real dt), else TickGameTimers' (game dt).
    void FireTimers(bool realDomain, float dt);
    // Resumes the Engine.Run tasks whose wait on that clock is over.
    void TickTasks(bool realDomain, float dt);
    // Resumes task @p index with @p nargs values on its stack; handles its yield, end or error.
    void ResumeTask(std::size_t index, int nargs);
    // Ends the tasks of @p owner (all of them when @p all); returns how many.
    std::size_t CancelTasks(OwnerId owner, bool all);
    // Advances game-clock timers and tasks; Update calls it with the game dt.
    void TickGameTimers(float dt);
    // Defines __ScriptWrapMethods, the wrapper LoadSystem applies to a System table's methods.
    void InstallMethodWrapper();
    // Bridges C++ EventBus events to the Lua bus as table payloads (ScriptSystem_Bridges.cpp: one
    // row per event). The bus delivers at Gameplay/EventDelivery's two drain points; a publish
    // reaches Lua subscribers at the next Update's DispatchLuaEvents, so a payload entity may be
    // dead by then: check Engine.IsAlive. The contact bridges also call OnEnter / OnExit
    // synchronously, with the world borrowed and destroys deferred. Called once, from Initialize().
    void RegisterCppEventBridges();
    // The Damage.* functions (ScriptSystem_Damage.cpp), and the pre-hook they install into the damage
    // pipeline: it calls the Damage.OnBefore functions. Called once, from Initialize().
    void RegisterDamageBindings();
    void RunDamageHooks(World& world, Gameplay::Damage::Spec& spec, Gameplay::Damage::Verdict& verdict);
    // One bridge row; defined and used only in ScriptSystem_Bridges.cpp.
    template <typename E, typename Fill, typename After>
    void AddBridge(const char* luaName, bool scoped, Fill fill, After after);
    // Seeds the input-device edge memory (the edges Update() publishes itself) and registers its
    // reset beside the bridges' unsubscribers.
    void SeedInputDeviceEdge();

    // Scene-script plumbing. ProcessSceneScriptSwap runs the deferred enter (and the exit of a
    // scene nobody exited with ExitScene); TickSceneScript fires OnSceneUpdate. Both are called
    // from Update() while m_world is valid.
    void ProcessSceneScriptSwap();
    void TickSceneScript(float dt);
    // OnSceneExit and the scene-scoped Systems' OnShutdown, each followed by the release of what
    // it owns; the shared body of ExitScene, the deferred swap and teardown.
    void ExitEnteredScene();
    void FireSceneExitIfEntered();

    // ---- Scene-scoped Systems ----
    // Creates a fresh instance (metatable over the prototype) and an owner for each listed
    // scene-scoped System and runs OnInit in tick order. Unknown or engine-scoped names are logged
    // (and are a run-level script fault: a scene that names a missing System is broken content).
    void ActivateSceneSystems(const std::vector<std::string>& names);
    // One System's fresh instance and owner; OnInit is left to the caller (or the next Update).
    void ActivateSceneSystem(SystemEntry& sys);
    // OnShutdown in reverse tick order, release, and nil the instances.
    void DeactivateSceneSystems();
    // OnInit under the System's owner; marks initCalled either way.
    void InitSystem(SystemEntry& sys);

    // ---- Script ownership ----
    OwnerId NewOwner(OwnerKind kind, const std::string& name, std::string label);
    // Releases everything @p owner holds (bus subscriptions, AfterDelay timers, time-scale
    // requests, the input contexts it pushed and still has active, its tweens' onDone callbacks)
    // and forgets it. 0 is a no-op. Logs one [SCRIPTSCOPE] line when anything was released.
    // @p handOverDeferred: a Logic slot ending inside a world that goes on keeps its pending
    // AfterDelay timers, tween callbacks and timed time requests running under the world's owner
    // (a projectile that schedules its explosion's cleanup, then destroys itself); its
    // subscriptions, untimed requests and contexts still go.
    void    ReleaseOwner(OwnerId owner, bool handOverDeferred = false);
    OwnerId WorldOwner();   // created on first use; released when the world is replaced
    // True when @p owner belongs to a System or Logic template whose GameFlow set is suspended:
    // its bus handlers are skipped like its OnUpdate (the events are dropped, not queued).
    bool    OwnerSuspended(OwnerId owner) const;
    // The owner a System's methods run under when other scripts call them (the Lua method
    // wrapper): its current load (engine scope) or activation (scene scope); 0 when inactive.
    OwnerId SystemOwner(const std::string& name) const;
    void    ForgetOwnersOfKind(OwnerKind kind);   // drop records whose resources were wiped wholesale

    // ---- Game time scale ----
    // Recomputes m_timeScale from the legacy factor, the Sequencer value and the requests.
    void RefreshTimeScale();
    // Ages timed requests (global and per entity) with real dt and drops the expired ones.
    void TickTimeRequests(float realDt);
    // Rewrites every TimeDilationComponent::scale from m_entityTimeRequests (the slowest live
    // request, on the named entity and its animated descendant) and removes a component that is
    // back to normal. Needs the current or the bound world; without one it waits for the next call.
    void RefreshEntityTimeScales();
    // Time.SetRealTime: TimeDilationComponent::realTime on @p e and its animated descendant.
    void ApplyRealTime(World& world, Entity e, bool realTime);

    // Hot reload of one changed file (CheckHotReload): the reload itself, with
    // its [HOTRELOAD] receipt (System/HotReloadReceipt.h). ReloadByCategory
    // returns which kind of script @p path was ("untracked" = none: no rule
    // reloads it) and clears @p ok when the reload failed.
    void        ReloadChangedScript(const std::string& path);
    const char* ReloadByCategory(const std::string& path, bool& ok);

    // Logic template / instance plumbing.
    bool LoadLogicTemplate(const std::string& path);
    // creates the slot's instance via metatable, injects exposed vars, fires
    // OnSpawn. `overrides` (may be null) supplies that slot's exposed-var values.
    bool EnsureLogicInstance(Entity e, std::size_t slot, const std::string& path,
                             const std::unordered_map<std::string, ScriptVarValue>* overrides);
    void DestroyLogicInstance(Entity e, std::size_t slot);       // fires OnDestroy, drops the slot's instance
    // Fire OnDestroy + nil the Lua table for one slot, using copied metadata so
    // it is safe after m_states has been erased and re-entrancy-safe (nils
    // before firing). Shared by DestroyLogicInstance and TeardownEntity.
    void FireSlotDestroy(Entity e, std::size_t slot, const LogicSlot& ls);
    void TeardownEntity(Entity e);                               // tears down every slot, drops the entity's array

    // Contact-dispatch target: returns @p start if any of its initialized Logic slots
    // defines @p fnName (e.g. "OnEnter"), else the first ancestor up the Parent chain
    // that does (depth-capped), else NullEntity. Lets a prefab keep its sensor
    // collider on a child and its script on the root, like Unity's collider ->
    // rigidbody message routing.
    Entity FindContactHandlerUpwards(Entity start, const char* fnName);

    // Parse a template table's `exposed` field into a ScriptVarDesc vector.
    // sol-typed body lives in the .cpp. Called from LoadLogicTemplate.
    void ParseExposedSchema(const std::string& path, std::vector<ScriptVarDesc>& out);
    // Write the resolved exposed-var values (default or override) onto a live
    // slot instance table. Shared by EnsureLogicInstance and ApplyExposedVars.
    void InjectExposedVars(Entity e, std::size_t slot, const std::string& path,
                           const std::unordered_map<std::string, ScriptVarValue>* overrides);

    void SweepDestroyed(World& world);     // run OnDestroy for dead/detached entities

    // Advances transform tweens (Engine.MoveTo / RotateTo / ScaleTo) and fires their
    // onDone callbacks. Uses GAME dt, so tweens freeze while the game is paused,
    // unlike TickTimers (real dt). Called from Update(), where m_world is valid.
    void TickTweens(float dt);

    void OnEntityDestroyed(World& world, Entity e);  // listener body
    void DispatchLuaEvents();              // drain LuaBus::pending once

    // Calls the System's OnShutdown (when it has one, is active and OnInit ran), releases
    // everything it owns and nils __systems[name] (and a scene System's prototype). The entry
    // stays in m_systems, marked unloaded. Services have no teardown.
    void TeardownSystem(SystemEntry& sys);

    // Shared tail of ScanScriptDirectory / LoadBootScripts: (order, name)
    // stable-sort of m_systems, dispatch-cache invalidation, registry dump.
    void FinalizeBootRegistry();

    // Called by Update() before SweepDestroyed; see m_lastWorldEpoch.
    void     DropInstancesForWorldSwap(World& world);
    // Every Logic instance gone without OnDestroy, each slot's owner released.
    void     DropLogicInstances();

    // ---- Deferred destroys (see BeginDeferredDestroyWindow) ----
    void FlushPendingDestroys();

    // ---- Deferred anim events ----
    void QueueAnimEvent(Entity e, const std::string& name, const PropertyBag* params);
    void DispatchPendingAnimEvents();

    // ---- Per-frame dispatch cache ----
    // pImpl (defined in ScriptSystem.cpp) so this header stays sol-free. Caches each
    // System's and Logic slot's instance table + OnUpdate function, so Update does no
    // per-frame string-keyed Lua table walks (and their registry-ref garbage). Rows
    // self-validate: a path hash catches slot edits and shifts, the entity generation
    // catches id recycling, m_luaDispatchGen catches reloads, resets and the boot sort.
    std::unique_ptr<DispatchCache> m_dispatchCache;
    // Dispatch-cache generation: a row filled under an older value is refetched. Bumped
    // by every hot reload, by session resets (ClearAll, ClearInstances, a world swap that
    // drops Logic instances) and by the boot sort, which reorders m_systems (System rows
    // are index-parallel). RemoveSystem's erase shifts System rows without a bump.
    uint32_t                       m_luaDispatchGen = 1;

    // ---- Lua VM and script registries ----
    sol::state*                                      m_lua = nullptr;

    // Logic: per-entity instance metadata; templates keyed by file path.
    std::unordered_map<Entity, LogicState>           m_states;
    std::unordered_map<std::string, LogicTemplate>   m_logicTemplates;

    // Tick order is the vector order: FinalizeBootRegistry sorts it by
    // (order, name) at boot; a system first loaded later is appended.
    std::vector<SystemEntry>                         m_systems;
    std::unordered_map<std::string, std::string>     m_systemSetOverrides;

    // Manifest-provided per-system metadata, keyed by stem. LoadSystem applies it
    // only when the loading path matches `path`: a stem-colliding load from elsewhere
    // (an -exec driver) must not inherit a manifest set and be silently gated off. A
    // persistent map because a hot reload rewrites every SystemEntry field, so a
    // load-time-only argument would be lost on the first save. Filled once by
    // LoadBootScripts; empty in folder-scan mode.
    std::unordered_map<std::string, ManifestSystemMeta> m_manifestSystemMeta;
    bool                                             m_bootedFromManifest = false;
    // Disabled manifest entries ("[kind] path", filled by LoadBootScripts) for the
    // registry dump: they never enter the registries, and an accidentally disabled
    // system must not diff as merely absent.
    std::vector<std::string>                         m_manifestDisabled;

    // Restart-to-apply notice for edits to the manifest file itself (kept out of
    // m_fileTimestamps; see CheckHotReload). Baseline set by LoadBootScripts; one
    // warning per session.
    std::filesystem::file_time_type m_manifestTimestamp{};
    bool     m_manifestTimestampValid  = false;
    bool     m_manifestChangeNotified  = false;
    uint32_t m_manifestStatCounter     = 0;

    SystemSetActivePredicate                         m_isSystemSetActive;
    SystemSetRegistrar                               m_registerSystemSet;
    std::unordered_map<std::string, ServiceEntry>    m_services;

    // UI script registry: same shape as services, a separate map so tools can list
    // the category apart.
    std::unordered_map<std::string, ServiceEntry>    m_uiScripts;

    // ---- Scene script ----
    // Single active script; its instance table is the Lua global __scene_script.
    // m_sceneScriptPending queues a swap that ProcessSceneScriptSwap services on the
    // next Update.
    std::string m_sceneScriptPath;             // currently-entered script (empty = none)
    std::string m_sceneScriptName;             // scene name passed to OnSceneEnter
    std::string m_pendingSceneScriptPath;      // requested next script (empty = none)
    std::string m_pendingSceneScriptName;
    bool        m_sceneScriptPending   = false; // a SetActiveSceneScript request is queued
    bool        m_sceneScriptEntered   = false; // OnSceneEnter fired for m_sceneScriptPath
    bool        m_sceneScriptHasUpdate = false;
    bool        m_sceneScriptHasExit   = false;
    std::vector<std::string> m_sceneSystems;         // scene-scoped Systems of the entered scene
    std::vector<std::string> m_pendingSceneSystems;
    OwnerId     m_sceneScriptOwner     = 0;           // the entered scene script's resources
    // The binding the editor's Play started from (BeginPlaySession), queued again on Stop.
    std::string              m_playScenePath;
    std::string              m_playSceneName;
    std::vector<std::string> m_playSceneSystems;
    bool                     m_playSessionOpen = false;

    std::unordered_map<std::string,
        std::filesystem::file_time_type>             m_fileTimestamps;
    // Paths registered via AddGlobalScript (BT action libraries): re-run
    // whole-file on hot reload, no category table to swap.
    std::vector<std::string>                         m_globalScripts;
    // Round-robin cursor for the amortised hot-reload sweep (hot-reload hosts only).
    std::size_t m_hotReloadCursor = 0;
    float m_elapsed = 0.f;
    // Unscaled companion to m_elapsed, published as Time.realElapsed beside
    // Time.realDt. Accumulated in TickTimers from the real frame dt, so a menu keeps
    // animating and a rebind timeout keeps counting while Engine.SetTimeScale(0)
    // freezes Time.dt / Time.elapsed.
    float m_realElapsed = 0.f;

    // World::Epoch() as of the last Update. A scene swap (World::Clear) or the
    // editor's Stop (World::AdoptState) replaces the world without firing destroy
    // listeners, and ids restart at 1, so SweepDestroyed would keep an old Logic
    // instance (with the old scene's exposed overrides) on a new entity that got the
    // same id and script path. DropInstancesForWorldSwap drops every Logic instance,
    // without OnDestroy, when the epoch moves.
    uint32_t m_lastWorldEpoch = 0;

    // ---- Deferred destroys (see BeginDeferredDestroyWindow) ----
    // The generation is captured at queue time so a drain never kills an entity that
    // recycled the id in between.
    std::vector<PendingDestroy> m_pendingDestroys;
    int                         m_deferDestroyDepth = 0;

    // ---- Deferred anim events ----
    // CustomNotifyEvent is delivered by EventBus::DispatchAll at an EventDelivery drain point,
    // between phases, where m_world is null and no deferred-destroy window is open:
    // OnAnimEvent fired there runs world-less, and Engine.SpawnPrefab (not m_world-gated) would
    // mutate the world outside any phase. So the bridge queues and Update dispatches, with the
    // same one frame of latency as the LuaBus. The generation is captured at queue time so a
    // drain never fires on an entity that recycled the id in between.
    std::unique_ptr<AnimEventQueue> m_animEvents;

    // ---- World access, timers, tweens and event buses ----
    // Set for the duration of Update() so Engine.* bindings registered once in
    // RegisterBindings reach the current World through a stable `this` capture. Null
    // outside Update unless borrowed (WorldBorrow, ExchangeBoundWorld); bindings guard
    // on it.
    World* m_world = nullptr;

    std::unique_ptr<TimerQueue> m_timers;   // pImpl: holds sol::* callbacks
    std::unique_ptr<TaskQueue>  m_tasks;    // pImpl: Engine.Run coroutines
    float                       m_timeScale = 1.f;   // effective; RefreshTimeScale writes it
    float                       m_legacyTimeScale    = 1.f;   // Engine.SetTimeScale
    float                       m_sequencerTimeScale = 1.f;   // Seq::TimeScale's sink
    std::vector<TimeRequest>    m_timeRequests;               // Time.RequestScale
    std::vector<EntityTimeRequest> m_entityTimeRequests;      // Time.RequestEntityScale
    bool                        m_entityTimeDirty    = false; // a refresh waits for a world
    std::uint64_t               m_nextTimeRequestId  = 0;     // shared by both request kinds

    std::unique_ptr<TweenQueue> m_tweens;   // pImpl: transform tweens + onDone callbacks

    std::unique_ptr<LuaBus> m_luaBus;       // string-keyed event queue

    // ---- Script ownership ----
    // The owner whose code is running (OwnerScope): Engine.Subscribe, Engine.AfterDelay, the
    // tweens, Time.RequestScale and Input.PushContext tag what they create with it.
    OwnerId                                  m_currentOwner = 0;
    OwnerId                                  m_nextOwnerId  = 0;
    std::unordered_map<OwnerId, OwnerRecord> m_owners;
    // Which owner pushed each active input context; a pop by anyone clears the entry.
    std::unordered_map<std::string, OwnerId> m_contextOwners;
    OwnerId                                  m_worldOwner = 0;   // WorldOwner(); 0 = none yet
    // Each entry unsubscribes one bridge from C++ EventBus. Bridges capture
    // `this` and the Lua state, so we must detach them before ScriptSystem
    // (and m_lua) get torn down.
    std::vector<std::function<void()>> m_cppBridgeUnsubscribers;
    std::vector<OwnerReleaseHook>      m_ownerReleaseHooks;

    // BindWorld subscription bookkeeping, so UnbindWorld removes exactly the
    // listener it added.
    World*   m_boundWorld                 = nullptr;
    uint32_t m_entityDestroyListenerId    = 0;
};
