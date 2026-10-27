// ScriptSystem_Damage.cpp - the Lua side of the damage pipeline (Gameplay/Damage.h): Damage.Apply, the
// pre-hook functions Damage.OnBefore registers (the policy a game puts between a hit and Health) and the
// queries that explain what happened to a hit. The hooks are subscriptions on the Lua bus under the name
// "DamageHook", so they belong to the script that made them and go with it.

#include "Scripting/ScriptSystem.h"
#include "Scripting/LuaDoc.h"         // bindings described as they are bound
#include <sol/sol.hpp>
#include "Scripting/LuaEntityId.h"    // LuaEntity + LuaId::* (needs sol first)
#include "Resource/AssetFS.h"         // ScriptSystem_Internal.h
#include "ScriptSystem_Internal.h"    // private to src/Scripting, not on the include path
#include "Gameplay/Damage.h"
#include "System/Log.h"

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    using Gameplay::TagTable;
    using namespace Gameplay::Damage;

    constexpr const char* kHookChannel = "DamageHook";

    sol::table RecordTable(sol::state_view lua, const World* world, const Record& record)
    {
        sol::table t = lua.create_table();
        t["seq"]        = record.seq;
        t["target"]     = LuaId::OutMaybe(world, record.target);
        t["instigator"] = LuaId::OutMaybe(world, record.instigator);
        t["type"]       = record.type;
        t["requested"]  = record.requested;
        t["applied"]    = record.applied;
        t["overkill"]   = record.overkill;
        t["reason"]     = ReasonName(record.reason);
        t["detail"]     = record.detail;
        t["hitGroup"]   = record.hitGroup;
        return t;
    }

    sol::table SpecTable(sol::state_view lua, const World* world, const Spec& spec)
    {
        sol::table t = lua.create_table();
        t["target"]     = LuaId::OutMaybe(world, spec.target);
        t["instigator"] = LuaId::OutMaybe(world, spec.instigator);
        t["source"]     = LuaId::OutMaybe(world, spec.source);
        t["part"]       = LuaId::OutMaybe(world, spec.part);
        t["type"]       = spec.type;
        t["amount"]     = spec.amount;
        t["hitGroup"]   = spec.hitGroup;
        t["fromHit"]    = spec.fromHit;
        t["slot"]       = spec.slot;
        t["x"] = spec.point.x;   t["y"] = spec.point.y;   t["z"] = spec.point.z;
        t["nx"] = spec.normal.x; t["ny"] = spec.normal.y; t["nz"] = spec.normal.z;
        sol::table tags = lua.create_table();
        int i = 1;
        for (const Gameplay::Tag tag : spec.tags) tags[i++] = TagTable::Get().Name(tag);
        t["tags"] = tags;
        return t;
    }

    // A hook's return value: nil / true leaves the spec alone, false vetoes it, a table may carry
    // veto (true, or "blocked" for a block / parry), detail, amount, type.
    void ApplyHookReturn(Spec& spec, Verdict& verdict, const sol::object& value)
    {
        if (value.get_type() == sol::type::boolean)
        {
            if (!value.as<bool>()) verdict.veto = true;
            return;
        }
        if (value.get_type() != sol::type::table) return;
        const sol::table t = value.as<sol::table>();
        if (const sol::optional<float> amount = t["amount"]) spec.amount = *amount;
        if (const sol::optional<std::string> type = t["type"]) spec.type = *type;
        if (const sol::optional<std::string> detail = t["detail"]) verdict.detail = *detail;
        const sol::object veto = t["veto"];
        if (veto.get_type() == sol::type::boolean && veto.as<bool>()) verdict.veto = true;
        if (veto.get_type() == sol::type::string)
        {
            verdict.veto = true;
            const std::string why = veto.as<std::string>();
            if (why == "blocked") verdict.reason = Reason::Blocked;
            else if (verdict.detail.empty()) verdict.detail = why;
        }
    }
}

void ScriptSystem::RunDamageHooks(World& world, Spec& spec, Verdict& verdict)
{
    std::vector<std::uint64_t> ids;
    for (const LuaBus::Sub& s : m_luaBus->subs)
        if (s.active && s.name == kHookChannel) ids.push_back(s.id);
    if (ids.empty()) return;

    WorldBorrow borrow(*this);
    DeferDestroys defer(*this);
    for (const std::uint64_t id : ids)
    {
        sol::protected_function fn;
        OwnerId owner = 0;
        for (const LuaBus::Sub& s : m_luaBus->subs)
            if (s.id == id && s.active) { fn = s.fn; owner = s.owner; break; }
        if (!fn.valid()) continue;

        OwnerScope as(*this, owner);
        sol::protected_function_result result = fn(SpecTable(*m_lua, &world, spec));
        if (!result.valid())
        {
            sol::error err = result;
            LOG_ERROR("Damage hook error (target %u): %s", static_cast<unsigned>(spec.target), err.what());
            continue;
        }
        if (result.get_type() != sol::type::none) ApplyHookReturn(spec, verdict, result.get<sol::object>());
        if (verdict.veto) return;
    }
}

void ScriptSystem::RegisterDamageBindings()
{
    sol::table damage = m_lua->create_named_table("Damage");

    Gameplay::Damage::SetPreHook([this](World& world, Spec& spec, Verdict& verdict) { RunDamageHooks(world, spec, verdict); });

    LuaDoc::Fn(damage, "Damage", "Apply",
        [this](sol::table request) -> sol::table
        {
            if (!m_world) throw sol::error("Damage.Apply: no_world: there is no world to damage");
            Spec spec;
            const sol::optional<LuaEntity> target = request["target"];
            if (!target) throw sol::error("Damage.Apply: bad_spec: target is required");
            spec.target = target->raw();
            if (const sol::optional<LuaEntity> v = request["instigator"]) spec.instigator = v->raw();
            spec.source = spec.instigator;
            if (const sol::optional<LuaEntity> v = request["source"]) spec.source = v->raw();
            if (const sol::optional<std::string> v = request["type"]) spec.type = *v;
            const sol::optional<float> amount = request["amount"];
            if (!amount) throw sol::error("Damage.Apply: bad_spec: amount is required");
            spec.amount = *amount;
            if (const sol::optional<uint32_t> v = request["hitGroup"]) spec.hitGroup = *v;
            std::string error;
            if (!Gameplay::Attr::ValidName(spec.type, &error)) throw sol::error("Damage.Apply: bad_spec: type " + error);
            if (const sol::optional<sol::table> tags = request["tags"])
                for (const auto& entry : *tags)
                {
                    const Gameplay::Tag tag = TagTable::Get().Intern(entry.second.as<std::string>(), &error);
                    if (!tag.IsValid()) throw sol::error("Damage.Apply: bad_tag: " + error);
                    spec.tags.push_back(tag);
                }
            // The caller's generation-stamped ids were resolved by the check above only by their raw id:
            // a stale target is refused here.
            if (!LuaId::IsLive(*m_world, *target)) spec.target = NullEntity;

            const Result result = Gameplay::Damage::Apply(*m_world, std::move(spec));
            sol::table out = m_lua->create_table();
            out["reason"]    = ReasonName(result.reason);
            out["landed"]    = Landed(result.reason);
            out["requested"] = result.requested;
            out["applied"]   = result.applied;
            out["overkill"]  = result.overkill;
            out["health"]    = result.health;
            out["died"]      = result.died;
            out["detail"]    = result.detail;
            return out;
        },
        { .doc = "Damages an entity through the pipeline: the target must be alive and not dead or immune, the same "
                 "instigator's hitGroup (nonzero) applies once, the Damage.OnBefore hooks may veto or change it, "
                 "Resist_<type> takes its fraction off, and Health goes down by what is left (never below 0). Publishes "
                 "Damaged (always, with the reason) and Died (once). Returns {reason, landed, requested, applied, overkill, "
                 "health, died, detail}; reason is \"applied\", \"unmanaged\", \"immune\", \"blocked\", \"vetoed\", \"dead\", "
                 "\"same_hit_group\", \"no_target\", \"resisted\" or \"no_amount\". Refused from inside a hook.",
          .params = { { .name = "spec", .doc = "{target, amount, type = \"Physical\", instigator, source, hitGroup, tags = {names}}",
                        .type = "table" } },
          .returnsDoc = "the outcome",
          .effect = LuaDoc::Effect::World, .errors = { "no_world", "bad_spec", "bad_tag" } });

    LuaDoc::Fn(damage, "Damage", "OnBefore",
        [this](sol::protected_function fn) -> std::uint64_t
        {
            if (!fn.valid()) { LOG_WARNING("Damage.OnBefore: invalid callback"); return 0ull; }
            return m_luaBus->Subscribe(kHookChannel, std::move(fn), m_currentOwner);
        },
        { .doc = "Registers the policy between a hit and Health. fn(spec) runs for every damage that is not already "
                 "refused (target alive, not dead, not immune, hit group not yet applied) and may return nil to let it "
                 "through, false (or {veto = true}) to veto it, {veto = \"blocked\"} to veto it as a block or parry, or "
                 "{amount = n, type = \"Fire\"} to change it. spec has target, instigator, source, part, type, amount, "
                 "hitGroup, tags, fromHit and the contact point. Hooks run in registration order and the first veto "
                 "ends the chain. Released with the script that registered it.",
          .params = { { .name = "fn" } }, .returnsDoc = "handle for Damage.RemoveHook",
          .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(damage, "Damage", "RemoveHook",
        [this](std::uint64_t handle) -> bool { return m_luaBus->Unsubscribe(handle); },
        { .doc = "Removes a Damage.OnBefore hook; false when it was already gone.",
          .params = { { .name = "handle" } }, .effect = LuaDoc::Effect::World });

    LuaDoc::Fn(damage, "Damage", "Last",
        [this](sol::this_state ts, const LuaEntity& target) -> sol::object
        {
            Record record;
            if (!Gameplay::Damage::Last(target.raw(), record)) return sol::make_object(ts.L, sol::lua_nil);
            return sol::make_object(ts.L, RecordTable(sol::state_view(ts), m_world ? m_world : m_boundWorld, record));
        },
        { .doc = "The newest damage outcome for the target, applied or not: {seq, target, instigator, type, requested, "
                 "applied, overkill, reason, detail, hitGroup}; nil when none was recorded (the last 64 are kept). "
                 "The answer to \"why did that hit do nothing\".",
          .params = { { .name = "target" } }, .returnsDoc = "the record or nil" });

    LuaDoc::Fn(damage, "Damage", "Recent",
        [this](sol::this_state ts, sol::optional<int> count) -> sol::table
        {
            sol::state_view view(ts);
            sol::table out = view.create_table();
            const World* world = m_world ? m_world : m_boundWorld;
            int i = 1;
            for (const Record& r : Gameplay::Damage::Recent(static_cast<size_t>(std::max(1, count.value_or(16)))))
                out[i++] = RecordTable(view, world, r);
            return out;
        },
        { .doc = "The last damage outcomes, newest first (at most count, default 16): the records Damage.Last returns.",
          .params = { { .name = "count" } }, .returnsDoc = "a list of records" });

    LuaDoc::Fn(damage, "Damage", "Counts",
        [this](sol::this_state ts) -> sol::table
        {
            sol::state_view view(ts);
            sol::table out = view.create_table();
            for (int r = 0; r <= static_cast<int>(Reason::NoAmount); ++r)
                out[ReasonName(static_cast<Reason>(r))] = Gameplay::Damage::Count(static_cast<Reason>(r));
            out["died"] = Gameplay::Damage::DiedCount();
            return out;
        },
        { .doc = "How many outcomes of each reason the pipeline has produced this run, and how many deaths (died).",
          .returnsDoc = "{applied = n, blocked = n, ..., died = n}" });
}
