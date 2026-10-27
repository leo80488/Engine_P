// LuaSequenceBindings - the `Sequence.*` Lua table.
// Every entry point writes the SAME transport request flags the inspector and a parent
// sequence use. There is no second path into the playhead: one evaluator, one discontinuity
// contract, one restore path, for a scrubber and a scene script alike.

#include "Sequencer/LuaSequenceBindings.h"

#include "ECS/ECS.h"
#include "Gameplay/GameState.h"
#include "Sequencer/SequenceAsset.h"     // Seq::HashTarget
#include "Sequencer/SequenceComponents.h"
#include "Sequencer/SequenceEval.h"
#include "Sequencer/SequenceTargets.h"   // Seq::FindSeqTarget / AllSeqTargets
#include "System/Log.h"

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)

#include <cstring>
#include <string>
#include <tuple>
#include <unordered_set>

namespace
{
    SequencePlayerComponent* GetPlayer(World& w, LuaEntity v)
    {
        const Entity e = LuaId::ResolveLive(w, v);
        if (auto* p = e != NullEntity ? w.GetComponent<SequencePlayerComponent>(e) : nullptr)
            return p;
        LOG_WARNING("Lua: entity %u has no live SequencePlayerComponent",
                    v.raw());
        return nullptr;
    }

    // An unknown target name is a programming error and Set/GetTarget may run every frame, so warn
    // once per distinct spelling (as the Input action-name resolver in ScriptSystem_Bindings.cpp does).
    const Seq::SeqTarget* ResolveTarget(const std::string& name)
    {
        if (const Seq::SeqTarget* t = Seq::FindSeqTarget(Seq::HashTarget(name.c_str())))
            return t;
        static std::unordered_set<std::string> s_warned;
        if (s_warned.insert(name).second)
        {
            LOG_WARNING("Sequence target '%s' does not exist "
                        "(see Sequence.Targets() for the registered names)",
                        name.c_str());
        }
        return nullptr;
    }
}

void RegisterLuaSequenceBindings(sol::state& lua, World& world)
{
    sol::table seq = lua.create_table();

    // Sequence.Play(entity [, castTable])
    // The optional cast { RoleName = entity, ... } overrides the component's authored bindings for
    // the roles it names, so one .iseq (a "door opens" cutscene) runs on whichever door the player
    // touched.
    seq.set_function("Play", [&world](LuaEntity e, sol::optional<sol::table> cast)
    {
        auto* p = GetPlayer(world, e);
        if (!p) return false;

        if (cast)
        {
            for (const auto& kv : *cast)
            {
                if (!kv.first.is<std::string>()) continue;
                const std::string role = kv.first.as<std::string>();
                const Entity target = LuaId::ResolveLive(world, kv.second.as<LuaEntity>());
                if (target == NullEntity)
                {
                    LOG_WARNING("Sequence.Play: role '%s' maps to a dead entity", role.c_str());
                    continue;
                }
                auto it = std::find_if(p->roleBindings.begin(), p->roleBindings.end(),
                                       [&](const SequenceRoleBinding& rb) { return rb.role == role; });
                if (it == p->roleBindings.end())
                {
                    p->roleBindings.push_back(SequenceRoleBinding{ role, {} });
                    it = p->roleBindings.end() - 1;
                }
                it->target.BindAndStamp(world, target);
            }
        }

        p->playRequested   = true;
        p->seekRequested   = true;
        p->seekTargetFrame = 0;
        return true;
    });

    seq.set_function("Stop", [&world](LuaEntity e)
    { if (auto* p = GetPlayer(world, e)) { p->stopRequested = true; return true; } return false; });

    seq.set_function("Pause", [&world](LuaEntity e)
    { if (auto* p = GetPlayer(world, e)) { p->paused = true; return true; } return false; });

    seq.set_function("Resume", [&world](LuaEntity e)
    { if (auto* p = GetPlayer(world, e)) { p->paused = false; return true; } return false; });

    // Skip is a REQUEST, not an action: the component's skipPolicy decides what
    // it means, so an unskippable cutscene stays unskippable no matter which
    // script calls this.
    seq.set_function("Skip", [&world](LuaEntity e)
    { if (auto* p = GetPlayer(world, e)) { p->skipRequested = true; return true; } return false; });

    // Seek takes FRAMES, the unit the asset and the editor use; seconds would round differently
    // at 24 vs 30 fps.
    seq.set_function("Seek", [&world](LuaEntity e, int frame)
    {
        if (auto* p = GetPlayer(world, e))
        { p->seekRequested = true; p->seekTargetFrame = frame; return true; }
        return false;
    });

    seq.set_function("SeekSeconds", [&world](LuaEntity e, float sec)
    {
        auto* p = GetPlayer(world, e);
        if (!p) return false;
        auto asset = Seq::AcquireSequence(p->sequencePath);
        const float fps = (asset && asset->frameRate > 0.f) ? asset->frameRate : 30.f;
        p->seekRequested   = true;
        p->seekTargetFrame = static_cast<int32_t>(sec * fps + 0.5f);
        return true;
    });

    seq.set_function("SetRate", [&world](LuaEntity e, float r)
    { if (auto* p = GetPlayer(world, e)) { p->rate = r; return true; } return false; });

    seq.set_function("IsPlaying", [&world](LuaEntity e)
    { auto* p = GetPlayer(world, e); return p && p->playing; });

    seq.set_function("GetFrame", [&world](LuaEntity e)
    { auto* p = GetPlayer(world, e); return p ? p->frame : 0; });

    seq.set_function("GetTime", [&world](LuaEntity e) -> float
    {
        auto* p = GetPlayer(world, e);
        if (!p) return 0.f;
        auto asset = Seq::AcquireSequence(p->sequencePath);
        const float fps = (asset && asset->frameRate > 0.f) ? asset->frameRate : 30.f;
        return static_cast<float>(p->frame) / fps;
    });

    seq.set_function("GetDuration", [&world](LuaEntity e) -> float
    {
        auto* p = GetPlayer(world, e);
        if (!p) return 0.f;
        auto asset = Seq::AcquireSequence(p->sequencePath);
        if (!asset || asset->frameRate <= 0.f) return 0.f;
        return static_cast<float>(asset->durationFrames) / asset->frameRate;
    });

    seq.set_function("SetSequence", [&world](LuaEntity e, const std::string& path)
    { if (auto* p = GetPlayer(world, e)) { p->sequencePath = path; return true; } return false; });

    // "Has this player seen it?" - reads the save-persisted GameState key the runtime writes on a
    // natural end, so the answer survives a save/load.
    seq.set_function("WasWatched", [](const std::string& path)
    { return Gameplay::GameState::Get().GetBool("seq.watched." + path, false); });

    // ---- SeqTarget registry, addressable by name ----
    // The calls above are transport: they replay curves baked into an .iseq. SetTarget / GetTarget /
    // Targets expose the Property track's address space, so a script can drive the same fields with
    // computed values. They resolve through the editor's own table (Seq::FindSeqTarget); do not add a
    // parallel one: one set of names, one place to add a row. Each target encodes its own
    // invalidation: light / lens / fog rows write raw fields (re-gathered every frame), material.* rows
    // call MaterialComponent's Set* mutators so the GPU material cache is marked dirty.
    // lens.* addresses VirtualCameraComponent (the camera-stack lens), not CameraComponent: the
    // `Camera` usertype's .fov is a different field.

    // Sequence.SetTarget(entity, name, a [,b,c,d]) -> bool
    // Partial writes are well-defined: the current value is read first and only the supplied
    // components are overwritten (SetTarget(e, "light.color", 1.0) sets red only); values past the
    // target's count (Sequence.Targets() lists it) are ignored. Returns false when the name is unknown
    // or the entity lacks the component, so a mis-bound call goes inert like a mis-bound Property
    // track. An unchanged value skips the write: for material.* rows a write means SetDirty(), a
    // scene-wide retained-cache re-gather that frame, so a script animating one should also quantize
    // the value.
    seq.set_function("SetTarget",
        [&world](LuaEntity e, const std::string& name, float a,
                 sol::optional<float> b, sol::optional<float> c,
                 sol::optional<float> d) -> bool
        {
            const Seq::SeqTarget* t = ResolveTarget(name);
            if (!t || !t->Set || !t->Get) return false;
            const Entity ent = LuaId::ResolveLive(world, e);
            if (ent == NullEntity) return false;

            // Seed from the current value so an unsupplied component keeps it
            // (and so the no-op compare below has something to compare against).
            float v[4] = { 0.f, 0.f, 0.f, 0.f };
            if (!t->Get(world, ent, v)) return false;   // component absent - inert

            float next[4] = { v[0], v[1], v[2], v[3] };
            const int n = static_cast<int>(t->count);
            if (n > 0) next[0] = a;
            if (n > 1 && b) next[1] = *b;
            if (n > 2 && c) next[2] = *c;
            if (n > 3 && d) next[3] = *d;

            if (std::memcmp(next, v, sizeof(float) * static_cast<size_t>(n)) == 0)
                return true;   // unchanged - do not pay the invalidation
            return t->Set(world, ent, next);
        });

    // -> ok, a, b, c, d   (components past the target's count read 0)
    seq.set_function("GetTarget",
        [&world](LuaEntity e, const std::string& name)
            -> std::tuple<bool, float, float, float, float>
        {
            constexpr std::tuple<bool, float, float, float, float> kNone
                { false, 0.f, 0.f, 0.f, 0.f };
            const Seq::SeqTarget* t = ResolveTarget(name);
            if (!t || !t->Get) return kNone;
            const Entity ent = LuaId::ResolveLive(world, e);
            if (ent == NullEntity) return kNone;
            float v[4] = { 0.f, 0.f, 0.f, 0.f };
            if (!t->Get(world, ent, v)) return kNone;
            return { true, v[0], v[1], v[2], v[3] };
        });

    // -> { { name = "light.intensity", count = 1, tooltip = "..." }, ... }
    // Enumerated at CALL time, so targets registered after boot are included.
    seq.set_function("Targets",
        [&lua]() -> sol::table
        {
            sol::table out = lua.create_table();
            int i = 0;
            for (const Seq::SeqTarget& t : Seq::AllSeqTargets())
            {
                sol::table row = lua.create_table();
                row["name"]    = t.name ? t.name : "";
                row["count"]   = static_cast<int>(t.count);
                row["tooltip"] = t.tooltip ? t.tooltip : "";
                out[++i] = row;
            }
            return out;
        });

    lua["Sequence"] = seq;
}
