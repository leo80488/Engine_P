// Damage.cpp: the damage pipeline (Gameplay/Damage.h) - the order a hit goes through, the events it
// ends in and the record that says why a hit changed nothing.

#include "Gameplay/Damage.h"

#include "ECS/NotifyTypes.h"
#include "Gameplay/EventDelivery.h"
#include "Gameplay/GameplayTagsComponent.h"
#include "System/CommandLine.h"
#include "System/EventBus.h"
#include "System/Log.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace Gameplay::Damage
{
namespace
{
    constexpr size_t kRingSize  = 64;
    constexpr size_t kMaxQueued = 256;
    constexpr size_t kReasonCount = static_cast<size_t>(Reason::NoAmount) + 1;

    PreHook             g_preHook;
    bool                g_inHook = false;
    std::vector<Spec>   g_queue;
    std::deque<Record>  g_ring;
    uint64_t            g_seq = 0;
    uint64_t            g_counts[kReasonCount] = {};
    uint64_t            g_diedCount = 0;

    struct HookScope
    {
        HookScope()  { g_inHook = true; }
        ~HookScope() { g_inHook = false; }
    };

    // The same swing applied to this target already: an instigator's hit group is one hit however
    // many volumes of it overlap.
    bool Remembered(const AttributeSetComponent& set, const Spec& spec, const World& world)
    {
        const uint32_t generation = spec.instigator != NullEntity ? world.GetGeneration(spec.instigator) : 0u;
        return std::any_of(set.recentHits.begin(), set.recentHits.end(), [&](const AttributeSetComponent::RecentHit& r)
        {
            return r.instigator == spec.instigator && r.generation == generation && r.hitGroup == spec.hitGroup;
        });
    }

    void Remember(AttributeSetComponent& set, const Spec& spec, const World& world)
    {
        const uint32_t generation = spec.instigator != NullEntity ? world.GetGeneration(spec.instigator) : 0u;
        set.recentHits.push_back({ spec.instigator, generation, spec.hitGroup });
        if (set.recentHits.size() > AttributeSetComponent::kMaxRecentHits)
            set.recentHits.erase(set.recentHits.begin());
    }

    // The outcome: the record, the log line and the events. @p died: Health reached zero with this hit.
    Result Finish(const Spec& spec, Result result, Reason reason, bool died)
    {
        result.reason = reason;
        result.died   = died;
        ++g_counts[static_cast<size_t>(reason)];

        g_ring.push_back({ ++g_seq, spec.target, spec.instigator, spec.type, result.requested, result.applied,
                           result.overkill, reason, result.detail, spec.hitGroup });
        if (g_ring.size() > kRingSize) g_ring.pop_front();

        if (reason != Reason::Unmanaged)
            LOG_INFO("[DAMAGE] target=%u instigator=%u type=%s requested=%.2f applied=%.2f overkill=%.2f "
                     "health=%.2f reason=%s hitGroup=%u%s%s",
                     static_cast<unsigned>(spec.target), static_cast<unsigned>(spec.instigator), spec.type.c_str(),
                     result.requested, result.applied, result.overkill, result.health, ReasonName(reason),
                     spec.hitGroup, result.detail.empty() ? "" : " detail=", result.detail.c_str());

        DamagedEvent event;
        event.target     = spec.target;
        event.instigator = spec.instigator;
        event.source     = spec.source;
        event.part       = spec.part;
        event.type       = spec.type;
        event.requested  = result.requested;
        event.applied    = result.applied;
        event.overkill   = result.overkill;
        event.health     = result.health;
        event.reason     = (Reverse("hitstop") && !Landed(reason)) ? Reason::Applied : reason;
        event.detail     = result.detail;
        event.hitGroup   = spec.hitGroup;
        event.fromHit    = spec.fromHit;
        event.point      = spec.point;
        event.normal     = spec.normal;
        EventBus::Get().Publish(std::move(event));

        // Died is the single end of a life: once, however many hits arrive after it. The "died" reverse
        // arm announces it for every hit that leaves Health at zero.
        if (died || (Reverse("died") && result.health <= 0.f && reason != Reason::NoTarget && reason != Reason::Unmanaged))
        {
            ++g_diedCount;
            DiedEvent death;
            death.target     = spec.target;
            death.instigator = spec.instigator;
            death.source     = spec.source;
            death.type       = spec.type;
            death.overkill   = result.overkill;
            death.hitGroup   = spec.hitGroup;
            EventBus::Get().Publish(std::move(death));
        }
        return result;
    }

    // The queued hits as specs, applied.
    bool RunQueue(World& world)
    {
        if (g_queue.empty()) return false;
        std::vector<Spec> specs;
        specs.swap(g_queue);
        for (Spec& spec : specs) Apply(world, std::move(spec));
        return true;
    }

    void QueueHit(const HitboxHitEvent& hit)
    {
        Spec spec;
        spec.target     = hit.victim;
        spec.instigator = hit.attacker;
        spec.source     = hit.source;
        spec.part       = hit.victimPart;
        spec.amount     = hit.damage;
        spec.hitGroup   = hit.hitGroup;
        spec.fromHit    = true;
        spec.immune     = hit.result == HitResult::Immune;
        spec.slot       = hit.slot;
        spec.point      = hit.point;
        spec.normal     = hit.normal;
        if (g_queue.size() >= kMaxQueued) g_queue.erase(g_queue.begin());
        g_queue.push_back(std::move(spec));
    }
}

// ==== Names and switches ====
const char* ReasonName(Reason reason)
{
    switch (reason)
    {
    case Reason::Applied:      return "applied";
    case Reason::Unmanaged:    return "unmanaged";
    case Reason::Immune:       return "immune";
    case Reason::Blocked:      return "blocked";
    case Reason::Vetoed:       return "vetoed";
    case Reason::Dead:         return "dead";
    case Reason::SameHitGroup: return "same_hit_group";
    case Reason::NoTarget:     return "no_target";
    case Reason::Resisted:     return "resisted";
    case Reason::NoAmount:     return "no_amount";
    }
    return "unknown";
}

bool Reverse(const char* mode)
{
#if WITH_HARNESS
    // -damage_reverse=<mode>[,<mode>...] restores a behaviour the damage_pipeline pack holds: resist (no
    // resistance), group (no hit-group memory), died (Died on every hit that leaves Health at zero), hitstop
    // (Damaged consumers see every refused hit as applied).
    static const std::string s_modes = "," + Sys::CommandLine::Get().Get("-damage_reverse") + ",";
    return s_modes.find(std::string(",") + mode + ",") != std::string::npos;
#else
    (void)mode;
    return false;
#endif
}

void SetPreHook(PreHook hook) { g_preHook = std::move(hook); }
bool InPreHook() { return g_inHook; }

// ==== The pipeline ====
Result Apply(World& world, Spec spec)
{
    Result result;
    result.requested = spec.amount;

    if (g_inHook)
    {
        // A hook that applies damage would run the hooks again on its own hit.
        LOG_WARNING("[DAMAGE] Apply from inside a pre-hook is refused (target=%u)", static_cast<unsigned>(spec.target));
        result.detail = "reentrant";
        return Finish(spec, result, Reason::Vetoed, false);
    }
    if (!world.IsAlive(spec.target)) return Finish(spec, result, Reason::NoTarget, false);

    AttributeSetComponent* set = world.GetComponent<AttributeSetComponent>(spec.target);
    const AttributeSetComponent::Entry* health = set ? set->Find(Attr::kHealth) : nullptr;
    if (health) result.health = health->current;

    if (health && health->current <= 0.f && !Reverse("died")) return Finish(spec, result, Reason::Dead, false);

    bool immune = spec.immune;
    if (!immune)
        if (const GameplayTagsComponent* tags = world.GetComponent<GameplayTagsComponent>(spec.target))
            immune = tags->Matches(Std::IFrame()) || tags->Matches(Std::Invincible());
    if (immune) return Finish(spec, result, Reason::Immune, false);

    if (set && spec.hitGroup != 0 && !Reverse("group") && Remembered(*set, spec, world))
        return Finish(spec, result, Reason::SameHitGroup, false);

    if (g_preHook)
    {
        Verdict verdict;
        {
            HookScope scope;
            g_preHook(world, spec, verdict);
        }
        result.requested = spec.amount;
        if (verdict.veto)
        {
            result.detail = verdict.detail;
            return Finish(spec, result, verdict.reason, false);
        }
        if (!world.IsAlive(spec.target)) return Finish(spec, result, Reason::NoTarget, false);
        // The hook may have added or removed components: the pointers are read again.
        set    = world.GetComponent<AttributeSetComponent>(spec.target);
        health = set ? set->Find(Attr::kHealth) : nullptr;
        result.health = health ? health->current : 0.f;
    }

    if (!(spec.amount > 0.f) || !std::isfinite(spec.amount)) return Finish(spec, result, Reason::NoAmount, false);
    if (!health) return Finish(spec, result, Reason::Unmanaged, false);

    const float resist = set->Get(Attr::ResistName(spec.type), 0.f);
    const float factor = Reverse("resist") ? 1.f : std::max(0.f, 1.f - resist);
    const float amount = spec.amount * factor;
    if (!(amount > 0.f)) return Finish(spec, result, Reason::Resisted, false);

    const float before  = health->current;
    const float applied = std::min(amount, before);
    result.applied  = applied;
    result.overkill = amount - applied;
    if (!(applied > 0.f)) return Finish(spec, result, Reason::Dead, false);

    const Attr::WriteResult write = Attr::Write(world, spec.target, Attr::kHealth, before - applied,
                                                Attr::Target::BaseAndCurrent, /*announceDeath*/ false);
    result.health = write.current;
    if (spec.hitGroup != 0)
        if (AttributeSetComponent* after = world.GetComponent<AttributeSetComponent>(spec.target))
            Remember(*after, spec, world);
    return Finish(spec, result, Reason::Applied, write.died);
}

// ==== Hits ====
void Install()
{
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;
    EventBus::Get().Subscribe<HitboxHitEvent>([](const HitboxHitEvent& hit) { QueueHit(hit); });
    EventDelivery::AddStage(RunQueue);
}

// ==== Diagnostics ====
std::vector<Record> Recent(size_t count)
{
    std::vector<Record> out;
    for (auto it = g_ring.rbegin(); it != g_ring.rend() && out.size() < count; ++it) out.push_back(*it);
    return out;
}

bool Last(Entity target, Record& out)
{
    for (auto it = g_ring.rbegin(); it != g_ring.rend(); ++it)
        if (it->target == target) { out = *it; return true; }
    return false;
}

uint64_t Count(Reason reason) { return g_counts[static_cast<size_t>(reason)]; }
uint64_t DiedCount() { return g_diedCount; }

void ResetDiagnostics()
{
    g_ring.clear();
    g_seq = 0;
    g_diedCount = 0;
    std::fill(std::begin(g_counts), std::end(g_counts), uint64_t{ 0 });
}
}
