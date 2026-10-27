// Face.* / Morph.* Lua bindings for FacialAnimationComponent (the API is in LuaFacialBindings.h).
// This layer only resolves entities (character ROOT or skeleton entity -> the SkeletonComponent
// CARRIER, the only entity BuildSkinJobs reads the component from), marshals arguments and runs
// LuaDiag guards so no failure is silent. The state machinery is the Facial:: namespace
// (ECS/FacialAnimationSystem.h): one component per call, no World access, no allocation.
//
// Phase ordering, which every script-visible semantic depends on: Face.* runs in GameplayLogic /
// GameplayPostLogic and FacialAnimationSystem::Update in Animation, so an instant write
// (blendDuration <= 0) reads back the SAME frame (Facial::SetChannel sets current = target at call
// time) and Face.GetResolved reports LAST frame's composed layer.

#include "ECS/LuaFacialBindings.h"

#include "ECS/ECS.h"
#include "ECS/FacialAnimationSystem.h"   // FacialAnimationComponent + Facial::*
#include "ECS/AnimationComponents.h"     // SkeletonComponent / SkeletonRef / kInvalidAnimHandle
#include "ECS/HierarchyComponents.h"     // Children (skeleton-carrier walk), Parent, SceneSourcePath
#include "ECS/AnimEntityResolve.h"       // Anim::ResolveAnimEntityOrSelf
#include "ECS/FacialPresets.h"           // the model's presets (MP records)
#include "Resource/ModelShapeKeys.h"
#include "Resource/SkeletonAsset.h"      // SkeletonRegistry / SkeletonAsset

#include "Scripting/LuaDiag.h"
#include "Scripting/LuaDoc.h"            // Morph.GetPoolWeights is described as it is bound
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (MUST follow sol)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

// ==== Argument marshalling ====

// A Lua number arrives as integer or float depending on how the literal was written, and
// sol::object::is<float>() is false for an integer in this configuration; testing the TYPE covers
// both spellings in one place.
float ReadNumber(const sol::object& o, float fallback)
{
    if (o.valid() && o.get_type() == sol::type::number)
        return static_cast<float>(o.as<double>());
    return fallback;
}

// blendDuration: seconds, default 0 = instant. A negative value is treated as instant rather than
// rejected: a script computing a blend from a lerp can pass a tiny negative, and "snap" is the
// harmless reading.
float ReadBlend(const sol::object& o)
{
    const float v = ReadNumber(o, 0.0f);
    return (v > 0.0f) ? v : 0.0f;
}

float ReadOpt(sol::table t, const char* key, float fallback)
{
    sol::object v = t[key];
    return ReadNumber(v, fallback);
}

// ==== Owner resolution ====
// The SkeletonComponent carrier (Facial::ResolveSkeletonCarrier, shared with FaceTracking.Attach in
// ECS/FacialPresets.h).
Entity ResolveFaceOwner(World& world, LuaEntity id)
{
    return Facial::ResolveSkeletonCarrier(world, LuaId::ResolveLive(world, id));
}

// The skeleton asset for a resolved owner, or null. The bounds test is MANDATORY, not defensive:
// SkeletonRegistry::Get is a bare m_skeletons[index] with no check, and assetIndex is
// kInvalidAnimHandle (~0u) until the rig finishes streaming.
const SkeletonAsset* OwnerSkeleton(World& world, SkeletonRegistry& skeletons, Entity owner)
{
    const auto* sk = world.GetComponent<SkeletonComponent>(owner);
    if (!sk || sk->assetIndex == kInvalidAnimHandle || sk->assetIndex >= skeletons.Count())
        return nullptr;
    return &skeletons.Get(sk->assetIndex);
}

// One resolution step for every entry point. On failure it warns once per SITE (two sites per
// function, so "no skeleton under this entity" and "the rig has not streamed yet" cannot silence
// each other) and returns false; the caller returns its own sentinel and NEVER reaches EnsureComp,
// so the component is never created on an entity that cannot use it.
bool ResolveOwner(World& world, SkeletonRegistry& skeletons, LuaEntity id,
                  const char* fn, const void* siteOwner, const void* siteAsset,
                  Entity* outOwner, const SkeletonAsset** outSkel)
{
    const Entity owner = ResolveFaceOwner(world, id);
    if (owner == NullEntity)
    {
        LuaDiag::WarnOnce(siteOwner,
            "%s: entity %u is dead or has no SkeletonComponent under it - the facial "
            "layer is read only off the skeleton entity, so nothing was created.",
            fn, static_cast<unsigned>(id.raw()));
        return false;
    }
    const SkeletonAsset* skel = OwnerSkeleton(world, skeletons, owner);
    if (!skel)
    {
        LuaDiag::WarnOnce(siteAsset,
            "%s: entity %u has a SkeletonComponent but no loaded skeleton asset yet "
            "(morph names cannot be resolved). Call it after the rig has streamed.",
            fn, static_cast<unsigned>(owner));
        return false;
    }
    if (outOwner) *outOwner = owner;
    if (outSkel)  *outSkel  = skel;
    return true;
}

// EnsurePool -> GetComponent -> AddComponent -> RE-FETCH. Both halves matter:
// GetPool returns null until the first Add, and AddComponent can realloc the
// dense vector, so the pre-Add pointer is not reusable.
FacialAnimationComponent* EnsureComp(World& world, Entity e)
{
    world.EnsurePool<FacialAnimationComponent>();
    auto* fa = world.GetComponent<FacialAnimationComponent>(e);
    if (!fa)
    {
        world.AddComponent<FacialAnimationComponent>(e, FacialAnimationComponent{});
        fa = world.GetComponent<FacialAnimationComponent>(e);
    }
    return fa;
}

// ==== Morph-name diagnostics ====
// Content-keyed (WarnOnceNamed), not pointer-keyed: the keys are the MODEL's morph names, which are
// dynamic, and a single site would silence every typo after the first.
//
// A name missing from a rig whose name table hit the cap gets a DISTINCT message, because "past the
// cap" and "typo" are debugged differently: SkeletonAsset::MAX_MORPH_TARGETS and MorphComponent::MAX
// are both kMaxMorphTargets (shaders/morph_limits.hlsli), so it means the rig has more blend shapes
// than the engine keeps, and the importer logged which ones it dropped.
bool CheckMorphName(const SkeletonAsset& skel, const std::string& name)
{
    if (Facial::FindMorphSlot(skel, name.c_str()) >= 0) return true;

    if (Facial::IsNameTableTruncated(skel))
        LuaDiag::WarnOnceNamed("Face/truncated-names:" + name,
            "Face: morph '%s' did not resolve, and this skeleton's name table is "
            "TRUNCATED at %u entries - morphs past that index cannot be addressed by "
            "name. This is not a typo.",
            name.c_str(), SkeletonAsset::MAX_MORPH_TARGETS);
    else
        LuaDiag::WarnOnceNamed("Face/unknown-morph:" + name,
            "Face: morph '%s' is not in this skeleton (%u targets). The channel is "
            "kept and retried if the rig changes.",
            name.c_str(), skel.morphTargetCount);
    return false;
}

// ==== Presets ====
// The MP records of the model the owner was spawned from: its SceneSourcePath, on the owner (a
// model's root carries both) or the nearest ancestor that has one. Every Mesh's records merge into
// ONE table by preset name (a VRM expression moving face and teeth is a record on each Mesh and one
// preset here); a target named by two Meshes' records keeps the larger weight, since the runtime
// drives names, not Meshes. Orphan records (their Mesh is gone from the library) are left out.
// Shared with face tracking's "preset:<name>" targets (ECS/FacialPresets.h).
using Facial::PresetTable;
using Facial::OwnerModelPath;
using Facial::PresetsOfModel;

// Facial::SetChannel returns false ONLY when all 32 channels are live; it never evicts one, so the
// script keeps the face it already has.
bool ReportSetChannel(bool ok, const std::string& name)
{
    if (!ok)
        LuaDiag::WarnOnceNamed("Face/channels-full:" + name,
            "Face: no free facial channel for '%s' (all %d are live across "
            "Expression/Viseme/Manual/Blink - the Authored list does not count). Nothing "
            "was evicted; clear something with Face.ClearAll or Face.ClearExpression.",
            name.c_str(), FacialAnimationComponent::kMaxChannels);
    return ok;
}

} // namespace

void RegisterLuaFacialBindings(sol::state& lua, World& world,
                               SkeletonRegistry& skeletons,
                               const MorphClipLibrary* morphClips,
                               MorphPoolReader readPool)
{
    // Merge-safe get-or-create, like the Character and Animator tables: the registration order between
    // binding files is not a stable contract, and a bare create_named_table would silently clobber
    // another registrar.
    //
    // Do not write `sol::table t = lua["Face"];` on a possibly-nil value: that std::terminates.
    sol::object existing = lua["Face"];
    sol::table  face = existing.is<sol::table>() ? existing.as<sol::table>()
                                                 : lua.create_table();

    // Captures: `skeletons` is owned by the Renderer, a Run()-local, while the sol::state holding these
    // closures is an App member. Safe because no Lua runs after Run() returns; every Renderer-owned
    // binding (ChainLOD, Settings.*, Debug.Stats) has the same shape.

    // ---- Manual channel (group Manual, Override) ----
    face.set_function("SetWeight",
        [&world, &skeletons](LuaEntity entityId, const std::string& name,
                             float weight, sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.SetWeight/no-owner";
            static constexpr char kSiteAsset[] = "Face.SetWeight/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.SetWeight",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            // The channel is created even for a name this rig lacks, so it resolves by itself if the character
            // is later moved onto a rig that has it; the CALL still reports false, because from the script's
            // point of view nothing moved.
            const bool known = CheckMorphName(*skel, name);
            const bool set   = ReportSetChannel(
                Facial::SetChannel(*fa, FacialGroup::Manual, name.c_str(),
                                   weight, ReadBlend(blendObj)), name);
            return known && set;
        });

    face.set_function("GetWeight",
        [&world, &skeletons](sol::this_state ts, LuaEntity entityId,
                             const std::string& name) -> sol::object
        {
            sol::state_view L(ts);
            static constexpr char kSiteOwner[] = "Face.GetWeight/no-owner";
            static constexpr char kSiteAsset[] = "Face.GetWeight/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.GetWeight",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return sol::make_object(L, sol::lua_nil);

            const auto* fa = world.GetComponent<FacialAnimationComponent>(owner);
            float w = 0.0f;
            if (!fa || !Facial::GetChannelWeight(*fa, FacialGroup::Manual,
                                                 name.c_str(), &w))
                return sol::make_object(L, sol::lua_nil);
            return sol::make_object(L, w);
        });

    face.set_function("ClearWeight",
        [&world, &skeletons](LuaEntity entityId, const std::string& name,
                             sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.ClearWeight/no-owner";
            static constexpr char kSiteAsset[] = "Face.ClearWeight/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.ClearWeight",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return false;

            auto* fa = world.GetComponent<FacialAnimationComponent>(owner);
            if (!fa) return false;   // nothing to clear -> no component created
            return Facial::ClearChannel(*fa, FacialGroup::Manual, name.c_str(),
                                        ReadBlend(blendObj));
        });

    face.set_function("ClearAll",
        [&world, &skeletons](LuaEntity entityId, sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.ClearAll/no-owner";
            static constexpr char kSiteAsset[] = "Face.ClearAll/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.ClearAll",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return false;

            auto* fa = world.GetComponent<FacialAnimationComponent>(owner);
            if (!fa) return true;    // already clear
            // Manual + Expression + Viseme. Authored rows are the designer's
            // persistent base and auto-blink is a machine, not a channel.
            Facial::ClearScripted(*fa, ReadBlend(blendObj));
            return true;
        });

    // ---- Expression group (Override): set-wise replace ----
    // Face.SetExpression(e, { [name] = weight, ... } | "name", blendDuration?). A name that is one of
    // the model's PRESETS (its MP records: VRM expressions, PMX group morphs, the Mesh Editor's) expands
    // into its targets at weight x the preset's own; any other name is one morph. The string form is
    // { [name] = 1 }. Two contributions to one target ADD (VRM's rule for expressions sharing a target)
    // and the sum clamps to 0..1.
    face.set_function("SetExpression",
        [&world, &skeletons](LuaEntity entityId, sol::object exprObj,
                             sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.SetExpression/no-owner";
            static constexpr char kSiteAsset[] = "Face.SetExpression/no-asset";
            static constexpr char kSiteArg[]   = "Face.SetExpression/not-a-table";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.SetExpression",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;
            const bool byName = exprObj.get_type() == sol::type::string;
            if (!byName && !exprObj.is<sol::table>())
                return LuaDiag::Fail(kSiteArg, false,
                    "Face.SetExpression: argument 2 must be a preset or morph name, or a "
                    "table of { [name] = weight }.");

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            // The weights this call sets, by target, in first-seen order.
            const std::shared_ptr<const PresetTable> presets = PresetsOfModel(OwnerModelPath(world, owner));
            std::vector<std::pair<std::string, float>> targets;
            auto add = [&targets](const std::string& target, float w)
            {
                const auto it = std::find_if(targets.begin(), targets.end(),
                                             [&](const std::pair<std::string, float>& x) { return x.first == target; });
                if (it == targets.end()) targets.emplace_back(target, w);
                else                     it->second += w;
            };
            auto expand = [&](const std::string& name, float w)
            {
                if (const auto p = presets->find(name); p != presets->end())
                    for (const auto& [target, pw] : p->second) add(target, pw * w);
                else
                    add(name, w);
            };
            if (byName)
                expand(exprObj.as<std::string>(), 1.0f);
            else
                for (const auto& kv : exprObj.as<sol::table>())
                {
                    if (kv.first.get_type()  != sol::type::string) continue;
                    if (kv.second.get_type() != sol::type::number) continue;
                    expand(kv.first.as<std::string>(), static_cast<float>(kv.second.as<double>()));
                }

            const float blend = ReadBlend(blendObj);
            bool ok = true;

            // Bracketed: BeginExpression marks every live Expression channel
            // "releasing", each SetChannel revives the ones this call names,
            // EndExpression finishes off the rest. That is what makes "absent
            // from the table fades out" work with no second name list. Manual /
            // Viseme / Blink are never touched.
            Facial::BeginExpression(*fa);
            for (const auto& [name, sum] : targets)
            {
                const float w = std::clamp(sum, 0.0f, 1.0f);
                ok = CheckMorphName(*skel, name) && ok;
                ok = ReportSetChannel(
                        Facial::SetChannel(*fa, FacialGroup::Expression,
                                           name.c_str(), w, blend), name) && ok;
            }
            Facial::EndExpression(*fa, blend);
            return ok;
        });

    // Face.GetPresets(e) -> { [preset] = { [target] = weight } }: the table
    // SetExpression expands names through (empty, never nil, when the model has
    // none or the owner does not resolve).
    face.set_function("GetPresets",
        [&world, &skeletons](sol::this_state ts, LuaEntity entityId) -> sol::table
        {
            sol::state_view L(ts);
            sol::table result = L.create_table();
            static constexpr char kSiteOwner[] = "Face.GetPresets/no-owner";
            static constexpr char kSiteAsset[] = "Face.GetPresets/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.GetPresets",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return result;
            const std::shared_ptr<const PresetTable> presets = PresetsOfModel(OwnerModelPath(world, owner));
            for (const auto& [name, weights] : *presets)
            {
                sol::table w = L.create_table();
                for (const auto& [target, weight] : weights) w[target] = weight;
                result[name] = w;
            }
            return result;
        });

    face.set_function("ClearExpression",
        [&world, &skeletons](LuaEntity entityId, sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.ClearExpression/no-owner";
            static constexpr char kSiteAsset[] = "Face.ClearExpression/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.ClearExpression",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return false;

            auto* fa = world.GetComponent<FacialAnimationComponent>(owner);
            if (!fa) return true;
            Facial::ClearGroup(*fa, FacialGroup::Expression, ReadBlend(blendObj));
            return true;
        });

    // ---- Visemes (group Viseme, Override): one vowel up, the other four down ----
    face.set_function("SetVowel",
        [&world, &skeletons](LuaEntity entityId, const std::string& vowel,
                             float amount, sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.SetVowel/no-owner";
            static constexpr char kSiteAsset[] = "Face.SetVowel/no-asset";
            static constexpr char kSiteKey[]   = "Face.SetVowel/bad-key";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.SetVowel",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;

            const int idx = Facial::VowelIndex(vowel.c_str());
            if (idx < 0)
                return LuaDiag::Fail(kSiteKey, false,
                    "Face.SetVowel: '%s' is not a vowel key - expected exactly one of "
                    "\"a\" \"i\" \"u\" \"e\" \"o\".", vowel.c_str());

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            // Diagnose against the component's OWN viseme name for this vowel: the defaults are the MMD
            // hiragana vowels (a i u e o), but the inspector can retarget them.
            const bool known = CheckMorphName(*skel, fa->visemeMorph[idx]);
            const bool set   = ReportSetChannel(
                Facial::SetVowel(*fa, idx, amount, ReadBlend(blendObj)),
                fa->visemeMorph[idx]);
            return known && set;
        });

    // ---- Eye closure: sugar over the blink morph, group Manual (OVERRIDE) ----
    // Blink is the MAX layer above it, so a blink still closes fully over a partial squint AND
    // SetEyeClose(e, 0) really reopens the eyes.
    face.set_function("SetEyeClose",
        [&world, &skeletons](LuaEntity entityId, float amount,
                             sol::object blendObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.SetEyeClose/no-owner";
            static constexpr char kSiteAsset[] = "Face.SetEyeClose/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.SetEyeClose",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            const std::string name = fa->blinkMorph;
            const bool known = CheckMorphName(*skel, name);
            const bool set   = ReportSetChannel(
                Facial::SetChannel(*fa, FacialGroup::Manual, name.c_str(),
                                   amount, ReadBlend(blendObj)), name);
            return known && set;
        });

    // ---- Blink ----
    face.set_function("Blink",
        [&world, &skeletons](LuaEntity entityId, sol::object optsObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.Blink/no-owner";
            static constexpr char kSiteAsset[] = "Face.Blink/no-asset";
            Entity owner = NullEntity;
            if (!ResolveOwner(world, skeletons, entityId, "Face.Blink",
                              kSiteOwner, kSiteAsset, &owner, nullptr))
                return false;

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            // Same keys as SetAutoBlink's opts, all seconds, all optional. 0 (the
            // default here) means "take the component's authored value" inside
            // TriggerBlink, so an omitted key costs nothing.
            // Named with a suffix on purpose: bare `close` / `open` are POSIX
            // names declared by <io.h>, which the PCH drags in.
            float closeT = 0.f, holdT = 0.f, openT = 0.f, peak = 0.f;
            if (optsObj.is<sol::table>())
            {
                sol::table o = optsObj.as<sol::table>();
                closeT = ReadOpt(o, "closeTime", 0.f);
                holdT  = ReadOpt(o, "holdTime",  0.f);
                openT  = ReadOpt(o, "openTime",  0.f);
                peak   = ReadOpt(o, "maxWeight", 0.f);
            }
            // The state machine only advances at dt > 0. Lua is Play-only, so
            // that always holds here; the EDITOR path additionally needs
            // Facial::ForceBlinkWeight, which is why the two are separate.
            FacialAnimationSystem::TriggerBlink(*fa, closeT, holdT, openT, peak);
            return true;
        });

    face.set_function("SetAutoBlink",
        [&world, &skeletons](LuaEntity entityId, bool enabled,
                             sol::object optsObj) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.SetAutoBlink/no-owner";
            static constexpr char kSiteAsset[] = "Face.SetAutoBlink/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.SetAutoBlink",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;

            auto* fa = EnsureComp(world, owner);
            if (!fa) return false;

            fa->autoBlinkEnabled = enabled;

            if (optsObj.is<sol::table>())
            {
                sol::table o = optsObj.as<sol::table>();

                sol::object morph = o["morph"];
                if (morph.get_type() == sol::type::string)
                {
                    const std::string m = morph.as<std::string>();
                    CheckMorphName(*skel, m);
                    // No InvalidateResolution: the system re-resolves blinkMorph
                    // whenever it differs from blinkResolvedName, so the one-entry
                    // blink cache is self-healing. Invalidating here would also
                    // drop every CHANNEL's cached slot for nothing.
                    Facial::CopyName(fa->blinkMorph, m.c_str());
                }

                // Clamp ONLY to >= 0 (and maxInterval >= minInterval). Clamping up to the authored 2.5 s default
                // would make sub-second cadences impossible; at -fixeddt=1/60 the shipped 2.5-6.0 s interval is
                // 150-360 frames between blinks, unobservable in a bounded test window.
                auto nonNeg = [](float v) { return (v > 0.f) ? v : 0.f; };
                fa->blinkIntervalMinSec = nonNeg(ReadOpt(o, "minInterval", fa->blinkIntervalMinSec));
                fa->blinkIntervalMaxSec = nonNeg(ReadOpt(o, "maxInterval", fa->blinkIntervalMaxSec));
                fa->blinkCloseSec       = nonNeg(ReadOpt(o, "closeTime",   fa->blinkCloseSec));
                fa->blinkHoldSec        = nonNeg(ReadOpt(o, "holdTime",    fa->blinkHoldSec));
                fa->blinkOpenSec        = nonNeg(ReadOpt(o, "openTime",    fa->blinkOpenSec));
                const float mw = ReadOpt(o, "maxWeight", fa->blinkMaxWeight);
                fa->blinkMaxWeight = (mw < 0.f) ? 0.f : ((mw > 1.f) ? 1.f : mw);
            }

            if (fa->blinkIntervalMaxSec < fa->blinkIntervalMinSec)
                fa->blinkIntervalMaxSec = fa->blinkIntervalMinSec;

            // Re-arm the PENDING countdown into the new range: it was seeded from the OLD maximum (up to 6 s at
            // the shipped defaults), so otherwise the first blink after a shortened interval waits out the old
            // value and reads as "SetAutoBlink did nothing". Clamping rather than re-rolling keeps the call
            // idempotent for a script that sets the same options every frame, and does not restart blinkIdleT.
            if (fa->blinkNextAt > fa->blinkIntervalMaxSec)
                fa->blinkNextAt = fa->blinkIntervalMaxSec;
            if (fa->blinkNextAt < fa->blinkIntervalMinSec)
                fa->blinkNextAt = fa->blinkIntervalMinSec;

            return true;
        });

    // ---- Introspection ----
    face.set_function("GetMorphNames",
        [&world, &skeletons](sol::this_state ts, LuaEntity entityId) -> sol::table
        {
            sol::state_view L(ts);
            sol::table result = L.create_table();   // EMPTY table on failure, never nil

            static constexpr char kSiteOwner[] = "Face.GetMorphNames/no-owner";
            static constexpr char kSiteAsset[] = "Face.GetMorphNames/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.GetMorphNames",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return result;

            // morphTargetCount is already capped at MAX_MORPH_TARGETS by both producers, but the read is a
            // fixed-size array: clamp anyway.
            const uint32_t n = (skel->morphTargetCount < SkeletonAsset::MAX_MORPH_TARGETS)
                             ? skel->morphTargetCount : SkeletonAsset::MAX_MORPH_TARGETS;
            for (uint32_t i = 0; i < n; ++i)
                result[static_cast<int>(i + 1)] = std::string(skel->morphTargetNames[i]);
            return result;
        });

    face.set_function("HasMorph",
        [&world, &skeletons](LuaEntity entityId, const std::string& name) -> bool
        {
            static constexpr char kSiteOwner[] = "Face.HasMorph/no-owner";
            static constexpr char kSiteAsset[] = "Face.HasMorph/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.HasMorph",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return false;
            // Deliberately silent about a miss: HasMorph is the question, not a
            // failure. CheckMorphName's warn-once belongs to the setters.
            return Facial::FindMorphSlot(*skel, name.c_str()) >= 0;
        });

    // Face.GetResolved(e)        -> 1-based array of { name, slot, weight, alpha, floor }
    // Face.GetResolved(e, name)  -> number | nil
    //
    // THE verification accessor: it reads the layer's own output, so it proves name->slot resolution,
    // the easing and the group fold. It does NOT prove the GPU deformed anything:
    // AnimStats.Get().facialSlots is the only observable downstream of BuildSkinJobs. It is LAST
    // frame's value (see the phase note at the top of this file).
    face.set_function("GetResolved",
        [&world, &skeletons](sol::this_state ts, LuaEntity entityId,
                             sol::object nameObj) -> sol::object
        {
            sol::state_view L(ts);
            const bool byName = (nameObj.get_type() == sol::type::string);

            static constexpr char kSiteOwner[] = "Face.GetResolved/no-owner";
            static constexpr char kSiteAsset[] = "Face.GetResolved/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Face.GetResolved",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return byName ? sol::make_object(L, sol::lua_nil)
                              : sol::make_object(L, L.create_table());

            const auto* fa = world.GetComponent<FacialAnimationComponent>(owner);

            if (byName)
            {
                const std::string name = nameObj.as<std::string>();
                const std::int16_t slot = Facial::FindMorphSlot(*skel, name.c_str());
                // nil means "that name is not a morph slot on this rig"; a resolvable slot the facial layer is not
                // driving reads 0, which is its actual contribution.
                if (slot < 0) return sol::make_object(L, sol::lua_nil);
                float w = 0.0f;
                if (fa) Facial::GetResolvedWeight(*fa, static_cast<std::uint16_t>(slot), &w);
                return sol::make_object(L, w);
            }

            sol::table result = L.create_table();
            if (!fa) return sol::make_object(L, std::move(result));
            const uint32_t n = (std::min)(fa->resolvedCount, static_cast<uint32_t>(fa->resolvedOut.size()));
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint16_t slot = fa->resolvedOut[i].slot;
                sol::table row = L.create_table();
                row["slot"]   = static_cast<int>(slot);
                row["weight"] = fa->resolvedOut[i].weight;
                // Below 1 only for a slot face tracking reached first: the pool then blends
                // (1 - alpha) x clip + alpha x weight, and floor is where a blink landed on it.
                row["alpha"]  = fa->resolvedOut[i].alpha;
                row["floor"]  = fa->resolvedOut[i].floor;
                row["name"]   = (static_cast<uint32_t>(slot) < skel->morphTargetCount &&
                                 static_cast<uint32_t>(slot) < SkeletonAsset::MAX_MORPH_TARGETS)
                              ? std::string(skel->morphTargetNames[slot])
                              : std::string();
                result[static_cast<int>(i + 1)] = row;
            }
            return sol::make_object(L, std::move(result));
        });

    lua["Face"] = face;

    // `Morph`: the same Manual-layer calls under the name used for shape keys that are not a face (a
    // prop's lid, a body slider): SetWeight / GetWeight / ClearWeight / ClearAll / Names / Has /
    // GetResolved. They ARE Face's functions (one owner walk, one set of warnings, which name Face.*),
    // so the two tables cannot disagree.
    sol::object existingMorph = lua["Morph"];
    sol::table  morph = existingMorph.is<sol::table>() ? existingMorph.as<sol::table>()
                                                       : lua.create_table();
    morph.set("SetWeight",   face.get<sol::object>("SetWeight"));
    morph.set("GetWeight",   face.get<sol::object>("GetWeight"));
    morph.set("ClearWeight", face.get<sol::object>("ClearWeight"));
    morph.set("ClearAll",    face.get<sol::object>("ClearAll"));
    morph.set("Names",       face.get<sol::object>("GetMorphNames"));
    morph.set("Has",         face.get<sol::object>("HasMorph"));
    morph.set("GetResolved", face.get<sol::object>("GetResolved"));

    // Morph.GetClipWeights(e) -> { [name] = weight }: the layer UNDER the facial fold, the owner's
    // MorphComponent: a bound morph clip's sampled channels by the CLIP's channel names, else the manual
    // weights by the skeleton's morph names. Weights above 0.0001 only; empty, never nil. Read-only:
    // what the clip says before Face.* folds on top (Face.GetResolved is that fold).
    morph.set_function("GetClipWeights",
        [&world, &skeletons, morphClips](sol::this_state ts, LuaEntity entityId) -> sol::table
        {
            sol::state_view L(ts);
            sol::table result = L.create_table();
            static constexpr char kSiteOwner[] = "Morph.GetClipWeights/no-owner";
            static constexpr char kSiteAsset[] = "Morph.GetClipWeights/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!ResolveOwner(world, skeletons, entityId, "Morph.GetClipWeights",
                              kSiteOwner, kSiteAsset, &owner, &skel))
                return result;
            const auto* mc = world.GetComponent<MorphComponent>(owner);
            if (!mc) return result;
            const uint32_t n = (std::min)(mc->count, static_cast<uint32_t>(MorphComponent::MAX));
            if (mc->primaryMorphClip != kInvalidAnimHandle)
            {
                if (!morphClips || mc->primaryMorphClip >= morphClips->Count()) return result;
                const MorphClipAsset& clip = morphClips->Get(mc->primaryMorphClip);
                for (uint32_t ch = 0; ch < n && ch < clip.morphCount; ++ch)
                    if (mc->weights[ch] > 0.0001f)
                        result[std::string(clip.morphNames[ch])] = mc->weights[ch];
            }
            else
            {
                const uint32_t names = (std::min)(skel->morphTargetCount, SkeletonAsset::MAX_MORPH_TARGETS);
                for (uint32_t s = 0; s < names && s < static_cast<uint32_t>(MorphComponent::MAX); ++s)
                    if (mc->weights[s] > 0.0001f)
                        result[std::string(skel->morphTargetNames[s])] = mc->weights[s];
            }
            return result;
        });

    // Morph.GetPoolWeights(e) -> { [name] = weight }: the TOP of the stack, what BuildSkinJobs wrote into
    // the morph pool (see the doc text). The first call arms the copy, so it answers empty until a frame
    // has been built after it.
    LuaDoc::Fn(morph, "Morph", "GetPoolWeights",
        [&world, &skeletons, readPool](sol::this_state ts, LuaEntity entityId) -> sol::table
        {
            sol::state_view L(ts);
            sol::table result = L.create_table();
            static constexpr char kSiteOwner[] = "Morph.GetPoolWeights/no-owner";
            static constexpr char kSiteAsset[] = "Morph.GetPoolWeights/no-asset";
            Entity owner = NullEntity; const SkeletonAsset* skel = nullptr;
            if (!readPool || !ResolveOwner(world, skeletons, entityId, "Morph.GetPoolWeights",
                                           kSiteOwner, kSiteAsset, &owner, &skel))
                return result;
            std::vector<std::pair<std::uint16_t, float>> rows;
            if (!readPool(owner, rows)) return result;
            for (const auto& [slot, w] : rows)
                if (w > 0.0001f && slot < skel->morphTargetCount && slot < SkeletonAsset::MAX_MORPH_TARGETS)
                    result[std::string(skel->morphTargetNames[slot])] = w;
            return result;
        },
        { .doc = "What the skinning step last wrote into the morph pool for a character, by the skeleton's morph "
                 "names: the clip, the facial fold, face tracking's alpha and the blink all in it (a slot two mesh "
                 "parts share: the larger). Last frame's; the first call arms the copy, so it is empty until a frame "
                 "has been built after it. Weights above 0.0001 only; empty, never nil.",
          .params = { { "id", "the character (its root or its skeleton entity)" } },
          .returns = "table" });
    lua["Morph"] = morph;

    LOG_INFO("Face: registered Lua facial/morph bindings");
}
