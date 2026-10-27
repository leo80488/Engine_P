#include "AI/AISystem.h"
#include "Diagnostics/Profiler.h"
#include "AI/AIComponents.h"
#include "AI/AIBehavior.h"
#include "AI/BTAsset.h"
#include "AI/BTNode.h"
#include "AI/BTNodes.h"
#include "ECS/ECS.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/HostPolicy.h"   // HotReloadEnabled: authoring host or -hotreload only
#include "System/HotReloadReceipt.h"   // [HOTRELOAD] receipts
#include "Resource/AssetFS.h"

#include <sol/sol.hpp>

#include "Scripting/LuaDiag.h"   // ResetOnce on tree hot-reload
#include "AI/LuaBBConvert.h"   // shared BBValue <-> Lua conversion (needs sol)

#include <algorithm>   // std::min for the typo-suggestion edit distance
#include <unordered_set>   // abort-reset: visited-node set
#include <vector>
#include <fstream>
#include <sstream>

namespace AI
{
    AISystem::AISystem() = default;
    AISystem::~AISystem() = default;

    void AISystem::Init(sol::state* lua)
    {
        m_lua = lua;
        m_actions.BindLua(lua);
    }

    // ==== Lua table -> C++ node tree ====
    // Schema:
    //   {
    //     type     = "Sequence" | "StatefulSequence" (alias "MemSequence") |
    //                "Selector" | "Parallel" |
    //                "Inverter" | "Repeater" | "Cooldown" |
    //                "BlackboardCondition" |
    //                "Action" | "Condition",
    //     name     = "optional designer label",
    //     children = { <subtable>, <subtable>, ... },
    //     -- type-specific fields:
    //     func     = "RegistryName"               -- Action / Condition
    //     params   = { key = value, ... }         -- Action / Condition
    //     count    = N                            -- Repeater
    //     seconds  = X                            -- Cooldown
    //     succeed_on = N                          -- Parallel
    //     fail_on    = N                          -- Parallel
    //     key      = "BBKey"                      -- BlackboardCondition
    //     op       = "is_set" | "equals" | "not_equals"
    //     expected = <BBValue>                    -- BlackboardCondition
    //   }
    // A new node type is a new case in ParseNode.

    // LuaToBBValue comes from AI/LuaBBConvert.h, shared with ctx:SetBB in LuaBTBindings.cpp. Do not
    // fork a local copy: two parsers drift (a fork turned string lists into zero vectors).

    static std::unordered_map<std::string, BBValue>
    ParseParams(const sol::table& t)
    {
        std::unordered_map<std::string, BBValue> out;
        sol::object paramsObj = t["params"];
        if (!paramsObj.is<sol::table>()) return out;
        sol::table params = paramsObj.as<sol::table>();
        for (auto& kv : params)
        {
            if (!kv.first.is<std::string>()) continue;
            out[kv.first.as<std::string>()] = LuaToBBValue(kv.second);
        }
        return out;
    }

    static std::unique_ptr<BTNode> ParseNode(const sol::table& t);

    // ==== Parse-time validation of leaf func names ====
    // func = "Name" is looked up in a global table at tick time, so a typo would survive load and
    // return Failure forever (in a Selector: a branch that never runs). The Actions / Conditions tables
    // are loaded before any tree (boot scripts, App::BootScripting), so names are checked once, at load.
    static int EditDistance(const std::string& a, const std::string& b)
    {
        std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
        for (size_t j = 0; j <= b.size(); ++j) prev[j] = static_cast<int>(j);
        for (size_t i = 1; i <= a.size(); ++i)
        {
            cur[0] = static_cast<int>(i);
            for (size_t j = 1; j <= b.size(); ++j)
                cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1,
                                    prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1) });
            prev = cur;
        }
        return prev[b.size()];
    }

    // Returns true when name resolves to a function in the global table tableName. On a miss logs
    // the closest registered name as well, since these are almost always typos.
    static bool ValidateLeafFunc(const sol::table& treeTable,
                                 const char* tableName,
                                 const std::string& name,
                                 const std::string& nodeLabel)
    {
        sol::state_view lua(treeTable.lua_state());
        if (name.empty())
        {
            LOG_ERROR("BT parse: %s node '%s' has no `func` field",
                      tableName, nodeLabel.c_str());
            return false;
        }
        sol::object tblObj = lua[tableName];
        if (!tblObj.is<sol::table>())
        {
            LOG_ERROR("BT parse: global `%s` table does not exist — are the BT "
                      "script libraries loaded? (App::InitLuaBindings)", tableName);
            return false;
        }
        sol::table tbl = tblObj.as<sol::table>();
        if (tbl[name].valid() && tbl[name].get_type() == sol::type::function)
            return true;

        std::string best; int bestD = 1 << 30;
        for (auto& kv : tbl)
        {
            if (!kv.first.is<std::string>()) continue;
            const std::string k = kv.first.as<std::string>();
            const int d = EditDistance(name, k);
            if (d < bestD) { bestD = d; best = k; }
        }
        if (!best.empty() && bestD <= 4)
            LOG_ERROR("BT parse: %s.%s not registered (node '%s') — did you mean "
                      "'%s'?", tableName, name.c_str(), nodeLabel.c_str(), best.c_str());
        else
            LOG_ERROR("BT parse: %s.%s not registered (node '%s')",
                      tableName, name.c_str(), nodeLabel.c_str());
        return false;
    }

    // Composites need children, decorators exactly one, leaves none. Without this an empty Sequence
    // succeeds and an Inverter silently ignores a second child.
    static bool ValidateArity(const BTNode& node, const std::string& type,
                              const std::string& label)
    {
        const size_t n = node.children.size();
        auto fail = [&](const char* want) {
            LOG_ERROR("BT parse: %s node '%s' has %zu children, expected %s",
                      type.c_str(), label.c_str(), n, want);
            return false;
        };
        if (type == "Inverter" || type == "Repeater" || type == "Cooldown")
            return n == 1 ? true : fail("exactly 1");
        if (type == "Sequence" || type == "Selector" || type == "Parallel"
         || type == "StatefulSequence" || type == "MemSequence")
            return n >= 1 ? true : fail("at least 1");
        if (type == "Action" || type == "Condition" || type == "BlackboardCondition")
            return n == 0 ? true : fail("0 (leaves cannot have children)");
        return true;
    }

    // Returns false if any child failed to parse, which rejects the whole tree: loading the rest would
    // run a plausible tree with a branch missing and one line in the log.
    static bool ParseChildren(const sol::table& t, BTNode& parent)
    {
        sol::object kids = t["children"];
        if (!kids.is<sol::table>()) return true;
        sol::table children = kids.as<sol::table>();
        for (size_t i = 1; ; ++i)
        {
            sol::object slot = children[i];
            if (!slot.is<sol::table>()) break;
            auto child = ParseNode(slot.as<sol::table>());
            if (!child)
            {
                LOG_ERROR("BT parse: child #%zu of '%s' failed to parse — "
                          "rejecting the whole tree", i,
                          parent.name.empty() ? "<unnamed>" : parent.name.c_str());
                return false;
            }
            parent.children.push_back(std::move(child));
        }
        return true;
    }

    static std::unique_ptr<BTNode> ParseNode(const sol::table& t)
    {
        const std::string type = t.get_or<std::string>("type", "");
        const std::string name = t.get_or<std::string>("name", "");

        std::unique_ptr<BTNode> node;

        if      (type == "Sequence")            node = std::make_unique<Sequence>();
        else if (type == "StatefulSequence"
              || type == "MemSequence")         node = std::make_unique<StatefulSequence>();
        else if (type == "Selector")            node = std::make_unique<Selector>();
        else if (type == "Parallel")
        {
            auto p = std::make_unique<Parallel>();
            p->succeedOn = t.get_or("succeed_on", 0u);
            p->failOn    = t.get_or("fail_on",    1u);
            node = std::move(p);
        }
        else if (type == "Inverter")            node = std::make_unique<Inverter>();
        else if (type == "Repeater")
        {
            auto r = std::make_unique<Repeater>();
            r->count = t.get_or("count", 1u);
            node = std::move(r);
        }
        else if (type == "Cooldown")
        {
            auto c = std::make_unique<Cooldown>();
            c->seconds = t.get_or("seconds", 1.0f);
            node = std::move(c);
        }
        else if (type == "BlackboardCondition")
        {
            auto b = std::make_unique<BlackboardCondition>();
            b->key = t.get_or<std::string>("key", "");
            const std::string op = t.get_or<std::string>("op", "is_set");
            if      (op == "is_set")     b->op = BBCmpOp::IsSet;
            else if (op == "equals")     b->op = BBCmpOp::Equals;
            else if (op == "not_equals") b->op = BBCmpOp::NotEquals;
            sol::object exp = t["expected"];
            if (!exp.is<sol::nil_t>()) b->expected = LuaToBBValue(exp);
            node = std::move(b);
        }
        else if (type == "Action")
        {
            auto a = std::make_unique<ActionLeaf>();
            a->actionName = t.get_or<std::string>("func", "");
            if (!ValidateLeafFunc(t, "Actions", a->actionName,
                                  name.empty() ? "<unnamed>" : name))
                return nullptr;
            a->params     = ParseParams(t);
            node = std::move(a);
        }
        else if (type == "Condition")
        {
            auto c = std::make_unique<ConditionLeaf>();
            c->conditionName = t.get_or<std::string>("func", "");
            if (!ValidateLeafFunc(t, "Conditions", c->conditionName,
                                  name.empty() ? "<unnamed>" : name))
                return nullptr;
            c->params        = ParseParams(t);
            node = std::move(c);
        }
        else
        {
            LOG_ERROR("BT parse: unknown node type '%s' (node '%s'). Valid types: "
                      "Sequence, Selector, StatefulSequence, MemSequence, Parallel, "
                      "Inverter, Repeater, Cooldown, Action, Condition, "
                      "BlackboardCondition",
                      type.c_str(), name.empty() ? "<unnamed>" : name.c_str());
            return nullptr;
        }

        node->name = name;
        if (!ParseChildren(t, *node)) return nullptr;
        if (!ValidateArity(*node, type, name.empty() ? "<unnamed>" : name))
            return nullptr;
        return node;
    }

    std::shared_ptr<BTAsset> AISystem::ParseFromFile(const std::string& path)
    {
        // Register the mtime before anything fallible: a tree that fails to parse must still be watched by
        // CheckHotReload, so saving the fixed file recovers it.
        try {
            m_fileTimestamps[path] = std::filesystem::last_write_time(Sys::U8Path(path));
        } catch (...) {}

        if (!m_lua)
        {
            LOG_ERROR("BT load: no Lua state bound (Init not called?)");
            return nullptr;
        }

        // Read through AssetFS (pak first, then loose disk) so packed builds resolve the tree from game.ipak.
        std::string source;
        if (!Resource::AssetFS::Get().ReadFileText(path, source))
        {
            LOG_ERROR("BT load: cannot open '%s'", path.c_str());
            return nullptr;
        }

        // Chunkname "@" + path makes Lua format errors as "asset/x.bt.lua:42: ...", the same as
        // RunScriptAsset (a bare path gives [string "asset/x.bt.lua"]:42:).
        sol::protected_function_result result = m_lua->safe_script(source,
            sol::script_pass_on_error,
            "@" + path);
        if (!result.valid())
        {
            sol::error err = result;
            LOG_ERROR("BT load: '%s' lua error: %s", path.c_str(), err.what());
            return nullptr;
        }
        if (result.return_count() == 0 || !result.get<sol::object>().is<sol::table>())
        {
            LOG_ERROR("BT load: '%s' did not return a table", path.c_str());
            return nullptr;
        }

        sol::table root = result.get<sol::table>();
        auto rootNode   = ParseNode(root);
        if (!rootNode) return nullptr;

        auto asset = std::make_shared<BTAsset>();
        asset->root       = std::move(rootNode);
        asset->sourcePath = path;
        asset->AssignIds();

        try {
            m_fileTimestamps[path] = std::filesystem::last_write_time(Sys::U8Path(path));
        } catch (...) {}

        return asset;
    }

    std::shared_ptr<BTAsset> AISystem::AcquireTree(const std::string& path)
    {
        // The cache stores failures too (a null shared_ptr). AdvanceAndMeasure retries AcquireTree every
        // frame for each agent with a null tree, before the interval gate; without a negative entry one
        // broken tree re-reads, recompiles and logs per agent per frame, a hitch that also flushes the
        // original error out of the editor's 2000-line log ring. CheckHotReload replaces the entry when
        // the file changes, so saving a fix recovers.
        auto it = m_loaded.find(path);
        if (it != m_loaded.end()) return it->second;   // may legitimately be null

        auto asset = ParseFromFile(path);
        m_loaded[path] = asset;                        // cache success AND failure
        return asset;
    }

    std::shared_ptr<BTAsset> AISystem::ReloadTree(const std::string& path)
    {
        auto fresh = ParseFromFile(path);
        if (!fresh) return nullptr;
        m_loaded[path] = fresh;
        return fresh;
    }

    void AISystem::Update(World& world, float dt)
    {
        // Zoned because AI cost is spiky rather than steady (see AI.Agents and AI.BTAgent): an
        // unattributed spike reads as a stutter with no cause.
        ZoneScopedN("AI.Update");

        m_elapsed += dt;

        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;

        const auto& ents = pool->Entities();
        m_ticksThisFrame  = 0;
        m_agentsThisFrame = static_cast<uint32_t>(ents.size());
#ifdef TRACY_ENABLE
        TracyPlot("AI.Agents", static_cast<int64_t>(ents.size()));
#endif
        // ---- Per-frame tick budget (anti-convoy, part 3) ----
        // The phase offset (part 1) is not sufficient because agents only tick on frame boundaries: the
        // offsets collapse into floor(interval / dt) buckets. At 60 Hz with a 0.1 s interval that is 6 and
        // the spread survives; at 30 Hz it is 3, the frame carrying a bucket runs longer, the longer dt
        // pushes the neighbouring buckets over their threshold, and they merge into one convoy that pays the
        // whole population every third frame.
        // The budget breaks that loop: each frame at most the expected arrival count,
        // sum(dt / tickInterval), ticks, most overdue first. Agents left out keep their accumulation and win
        // priority next frame: a self-levelling round robin with no starvation, and the per-frame cost is
        // bounded by the arrival rate.
        m_due.clear();
        m_due.reserve(ents.size());
        float expected = 0.f;
        for (size_t i = 0; i < ents.size(); ++i)
        {
            const float over = AdvanceAndMeasure(world, ents[i], dt, expected);
            if (over >= 0.f) m_due.push_back({ over, ents[i] });
        }

        if (!m_due.empty())
        {
            // ceil, and never zero: with one agent on a long interval the
            // expected arrival rate rounds to 0 and it would never tick.
            size_t budget = static_cast<size_t>(expected) + 1u;
            if (budget > m_due.size()) budget = m_due.size();

            if (budget < m_due.size())
            {
                // Most overdue first. partial_sort, not sort: only the first
                // `budget` need to be ordered, and this runs every frame.
                std::partial_sort(
                    m_due.begin(), m_due.begin() + budget, m_due.end(),
                    [](const DueAgent& a, const DueAgent& b)
                    { return a.overdue > b.overdue; });
            }
            for (size_t i = 0; i < budget; ++i)
                TickEntity(world, m_due[i].entity, dt);
        }
#ifdef TRACY_ENABLE
        // Agents that ran their tree this frame, vs the population (AI.Agents). If this plot steps with
        // AI.Agents instead of staying flat, agents tick on the same frame and the cost arrives as a convoy.
        TracyPlot("AI.BTTicks", static_cast<int64_t>(m_ticksThisFrame));
#endif
    }

    float AISystem::AdvanceAndMeasure(World& world, Entity e, float dt,
                                      float& expected)
    {
        AIComponent* ai = world.GetComponent<AIComponent>(e);
        if (!ai || !ai->enabled) return -1.f;

        // Deserialized AIComponents carry treePath but no tree / instance (the path is the persisted
        // handle): resolve on the first tick so a world load needs no re-drop in the Inspector.
        if (!ai->tree && !ai->treePath.empty())
        {
            ai->tree = AcquireTree(ai->treePath);
            if (ai->tree && !ai->instance)
                ai->instance = std::make_unique<BTInstance>();
        }
        if (!ai->tree || !ai->tree->root) return -1.f;

        // ---- Phase offset (anti-convoy, part 1) ----
        // Agents spawned on the same frame share timeSinceLastTick = 0 and tickInterval, so they would cross
        // the gate together forever (with 500 agents the frame carrying all of them was the p99, ~3x the
        // median). Hashed from the entity id, not random: a fixed-seed benchmark must replay identically.
        if (!ai->phaseSeeded)
        {
            ai->phaseSeeded = true;
            uint32_t h = static_cast<uint32_t>(e) * 2654435761u;
            h ^= h >> 15;
            ai->timeSinceLastTick =
                ai->tickInterval * (static_cast<float>(h & 0xFFFFu) / 65535.f);
        }

        // Arrival rate for the budget. A zero/negative interval means "every
        // frame", which contributes a whole agent per frame.
        expected += (ai->tickInterval > 1e-6f) ? (dt / ai->tickInterval) : 1.f;

        ai->timeSinceLastTick += dt;

        // wakeRequested (AIComponent) bypasses the interval once and sorts ahead of every due agent: a wake
        // is an edge event, and queueing it behind routine polls is the latency the flag removes. Cleared in
        // TickEntity, only when the agent actually runs.
        if (ai->wakeRequested) return 1e9f;
        if (ai->timeSinceLastTick < ai->tickInterval) return -1.f;
        return ai->timeSinceLastTick - ai->tickInterval;
    }

    void AISystem::TickEntity(World& world, Entity e, float dt)
    {
        AIComponent* ai = world.GetComponent<AIComponent>(e);
        // Re-fetched, not passed through: AdvanceAndMeasure ran over the whole pool before any tick, and a
        // BT leaf can add a component and reallocate the pool. The gates below already ran there.
        if (!ai || !ai->tree || !ai->tree->root) return;
        ai->wakeRequested = false;

        // Zoned per agent, not once per phase: at scale the question is how many land on the same frame.
        ZoneScopedN("AI.BTAgent");
        PROF_SCOPE_D("AI.BTAgent");   // -profile=detail: one scope per agent tick
        ++m_ticksThisFrame;

        const float tickDt    = ai->timeSinceLastTick;
        // ---- Interval carry (anti-convoy, part 2) ----
        // Subtract the interval instead of resetting to 0: a reset drops the overshoot, re-quantises the
        // agent onto the frame boundary and undoes the phase offset within a few ticks.
        // The debt is clamped to one interval: AILODSystem can drop tickInterval from 5 s to 0.1 s, which
        // would otherwise owe 50 back-to-back ticks on the frame the agent comes into view.
        ai->timeSinceLastTick -= ai->tickInterval;
        if (ai->timeSinceLastTick > ai->tickInterval || ai->timeSinceLastTick < 0.f)
            ai->timeSinceLastTick = 0.f;

        if (!ai->instance) ai->instance = std::make_unique<BTInstance>();
        // Scratch keys (leading '_') are leaf-private working state: a reset must drop them, or a re-entered
        // subtree resumes mid-phase. Keys without the prefix (TargetEntity, Health) are a gameplay contract
        // and survive.
        if (ai->instance->resetRequested)
        {
            if (auto* bb = world.GetComponent<BlackboardComponent>(e))
            {
                for (auto it = bb->values.begin(); it != bb->values.end(); )
                    it = (!it->first.empty() && it->first[0] == '_')
                       ? bb->values.erase(it) : std::next(it);
            }
        }
        if (ai->instance->resetRequested)
        {
            ai->instance->cooldowns.clear();
            ai->instance->repeats.clear();
            ai->instance->sequenceCursors.clear();
            ai->instance->runningNode    = kInvalidNodeId;
            ai->instance->resetRequested = false;
        }
        ai->instance->trace.clear();

        // This AddComponent can reallocate the pool, which is why BTContext must not cache the blackboard
        // pointer for the tree walk.
        if (!world.GetComponent<BlackboardComponent>(e))
            world.AddComponent<BlackboardComponent>(e, BlackboardComponent{});

        { PROF_SCOPE_D("AI.UpdateBehavior");
        UpdateBehavior(world,e,m_elapsed); }

        BTContext ctx;
        ctx.entity     = e;
        ctx.world      = &world;
        // No ctx.blackboard: BTContext::Blackboard() re-fetches on each access, so a component add mid-tick
        // (Engine.SpawnPrefab from a Lua leaf) cannot leave the walk on a dangling pool pointer.
        ctx.instance   = ai->instance.get();
        ctx.actions    = &m_actions;
        ctx.deltaTime  = tickDt;
        ctx.elapsed    = m_elapsed;

        { PROF_SCOPE_D("AI.BTTree");
        ai->tree->root->Tick(ctx); }

        // ---- Abort semantics: interrupted means reset ----
        // The tree ticks from the root every tick, so the top-level Selector preempts lower-priority
        // branches without observer-abort declarations. The abandoned branch is never told (Selector::Tick
        // just returns; nothing calls BTNode::OnExit), so a StatefulSequence would keep its cursor and
        // resume mid-sequence the next time its branch is chosen.
        // A node absent from this tick's trace was not visited: drop its resume state. Every node type
        // Records on every return path, so "not in trace" is sound. This must stay here in TickEntity, not
        // in Update: on frames the agent does not tick, trace still holds the previous tick.
        // cooldowns is not pruned: it is a deadline, not a resume point, and a missing entry reads as 0
        // (operator[]), so pruning would clear the cooldown of exactly the preempted branches.
        if (!ai->instance->trace.empty()
            || !ai->instance->sequenceCursors.empty()
            || !ai->instance->repeats.empty())
        {
            // Zoned separately: the root Records on every return path, so trace is never empty and the set
            // build plus both prune passes are per-agent-per-tick work, not an exceptional path.
            ZoneScopedN("AI.BTAbortPrune");
            PROF_SCOPE_D("AI.BTAbortPrune");
            std::unordered_set<NodeId> visited;
            visited.reserve(ai->instance->trace.size());
            for (const auto& te : ai->instance->trace) visited.insert(te.id);

            auto pruneUnvisited = [&visited](auto& m)
            {
                for (auto it = m.begin(); it != m.end(); )
                    it = visited.count(it->first) ? std::next(it) : m.erase(it);
            };
            pruneUnvisited(ai->instance->sequenceCursors);
            pruneUnvisited(ai->instance->repeats);   // also drops the 0 a finished Repeater leaves
        }
        // Do not wire up SubTree before namespacing NodeIds: SubTree::Tick enters the sub-asset's root with
        // the same instance and BTAsset::AssignIds numbers every asset from 1, so the ids would collide in
        // these maps and in trace. ParseNode has no "SubTree" case, so no .bt.lua builds one yet.
    }

    void AISystem::CheckHotReload(World& world)
    {
        // Hot reload only where Sys::HotReloadEnabled (the editor host, or Game.exe with -hotreload): a
        // shipped game reads trees from game.ipak and nobody edits them. A runtime check, not
        // #ifdef WITH_EDITOR: EngineCore is compiled once for every exe and never sees that define.
        if (!Sys::HotReloadEnabled()) { (void)world; return; }

        // Amortised: re-stat a slice per frame instead of every tracked file. A save is noticed within a
        // few frames, and the syscall cost does not scale with the number of loaded trees.
        constexpr std::size_t kFilesPerFrame = 4;
        if (m_fileTimestamps.empty()) return;
        if (m_hotReloadCursor >= m_fileTimestamps.size()) m_hotReloadCursor = 0;

        auto      it = m_fileTimestamps.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(m_hotReloadCursor));
        std::size_t examined = 0;

        // Stat first, reload after (the ScriptSystem::CheckHotReload shape):
        // nothing a reload does may run under the sweep's live iterator.
        std::vector<std::string> changed;
        for (; it != m_fileTimestamps.end() && examined < kFilesPerFrame; ++examined)
        {
            const std::string& path    = it->first;
            auto&              oldTime = it->second;
            ++it;
            ++m_hotReloadCursor;
            try {
                auto newTime = std::filesystem::last_write_time(Sys::U8Path(path));
                if (newTime == oldTime) continue;
                oldTime = newTime;
                changed.push_back(path);
            } catch (...) {}
        }

        for (const std::string& path : changed)
        {
            // One receipt per tree (System/HotReloadReceipt.h).
            const std::uint64_t receipt = HotReload::Open("lua", { path });
            HotReload::Start(receipt);
            HotReload::Receipt rc;
            rc.detailJson = "{\"category\":\"behavior_tree\"}";
            try {
                std::vector<CapturedLogLine> captured;
                std::shared_ptr<BTAsset> fresh;
                {
                    Logger::ScopedContext capture(Logger::CurrentSource(), Logger::CurrentRequestId(),
                                                  &captured, 32);
                    fresh = ParseFromFile(path);
                }
                // Write the result either way. On failure a good tree's entry becomes null on purpose: the file is
                // broken, and the negative entry stops a per-frame reparse storm. Live agents keep the shared_ptr
                // they hold, so a broken save only blocks new acquisitions.
                m_loaded[path] = fresh;
                if (!fresh)
                {
                    LOG_ERROR("AISystem: hot-reload '%s' FAILED — keeping running "
                              "agents on the previous tree; fix and save again",
                              path.c_str());
                    for (const CapturedLogLine& l : captured)
                        if (l.level == LogLevel::Error) rc.errors.push_back(l.message);
                    rc.code    = "lua_error";
                    rc.failed  = 1;
                    rc.message = "behaviour tree " + path + " did not parse - running agents keep the "
                                 "previous tree, new ones cannot acquire it until it is fixed";
                    HotReload::Close(receipt, rc);
                    continue;
                }
                OnTreeReloaded(world, path);
                // Re-arm warn-once so a fixed-and-saved tree reports fresh.
                LuaDiag::ResetOnce();
                LOG_INFO("AISystem: hot-reload '%s'", path.c_str());
                // AISystem::Update runs right after this, in this same frame.
                rc.ok             = 1;
                rc.effectiveFrame = Logger::CurrentFrame();
                rc.message        = "behaviour tree " + path + " in effect from frame " +
                                    std::to_string(rc.effectiveFrame);
            } catch (const std::exception& e) {
                rc.code    = "reload_error";
                rc.failed  = 1;
                rc.message = "behaviour tree " + path + " reload threw";
                rc.errors.push_back(e.what());
            } catch (...) {
                rc.code    = "reload_error";
                rc.failed  = 1;
                rc.message = "behaviour tree " + path + " reload threw";
            }
            HotReload::Close(receipt, rc);   // no-op when the failure path closed it
        }
    }

    void AISystem::OnTreeReloaded(World& world, const std::string& path)
    {
        auto it = m_loaded.find(path);
        if (it == m_loaded.end()) return;

        std::shared_ptr<BTAsset> newTree = it->second;
        auto* pool = world.GetPool<AIComponent>();
        if (!pool) return;
        const auto& ents = pool->Entities();
        for (size_t i = 0; i < ents.size(); ++i)
        {
            AIComponent& ai = pool->Data()[i];
            if (ai.treePath == path)
            {
                // Full reset: a reloaded tree re-runs AssignIds, so every
                // NodeId in the live instance may now mean a different node.
                ai.SetTree(newTree, path);
            }
        }
    }
}
