#include "Scripting/ScriptSystem.h"
#include "Scripting/LuaDoc.h"   // bindings described as they are bound
#include "Scripting/ScriptComponent.h"
#include "Scripting/LuaMathTypes.h"
#include "Scripting/LuaDiag.h"     // WarnOnce / ResetOnce
#include "Harness/RunLedger.h"   // Test.* records into the run verdict
#include "Harness/ExitCodes.h"
#include "ECS/Components.h"
#include "ECS/LightUtil.h"   // light direction derived from transform (+Z)
#include "ECS/HierarchyComponents.h"
#include "ECS/HierarchyOps.h"          // Hierarchy::DestroySubtree
#include "ECS/AnimationComponents.h"
#include "ECS/MotionDirectorComponent.h"  // Engine.GetMotionDirector
#include "ECS/LocomotionComponents.h"     // Engine.HasComponent("Locomotion"/...)
#include "ECS/CharacterAnimDriver.h"      // Engine.HasComponent("AnimDriver")
#include "ECS/CharacterIntentComponent.h" // Engine.HasComponent("Brain")
#include "ECS/CharacterControllerComponent.h"
#include "ECS/TODSystems.h"               // Engine.GetTimeOfDay / IsNight (TODUtil::Find*)
#include "ECS/TODComponents.h"            // TODConfigComponent / TODOutputComponent fields
#include "Resource/MaterialSerializer.h"  // Engine.SetMaterialAsset / ClearMaterialCache
#include "Resource/ComponentSerializers.h" // Engine.AddComponent / HasComponent fallback
#include "Resource/ComponentWrite.h"       // Engine.AddComponent: ParseLine
#include "Resource/MaterialOverrideApply.h" // Engine.ReloadMaterialsAffectedBy
#include "UI/UICanvas.h"                   // Engine.SetUIText (Canvas UIText)
#include "UI/UICanvasSystem.h"             // Engine.SetUIKeyboardCapture
#include "UI/WorldSpaceUI.h"               // Engine.SetWorldUITextFont
#include "UI/FontLibrary.h"                // Engine.GetFontAtlasStats
#include "ECS/CommandAPI.h"
#include "ECS/TagComponent.h"
#include "ECS/GuidRegistry.h"           // Engine.FindByGuid / MakePersistent
#include "ECS/GuidComponent.h"          // PersistentTag + RuntimeSpawnedTag (Engine.MakePersistent)
#include "ECS/Guid.h"                   // Guid::Parse
#include "ECS/FollowEvents.h"
#include "ECS/EquipmentEvents.h"
#include "ECS/NotifyTypes.h"            // CustomNotifyEvent / PropertyBag
#include "ECS/DissolveComponent.h"      // Engine.Dissolve / DissolveIn / SetDissolveAmount
#include "Physics/PhysicsEvents.h"
#include "Gameplay/GameState.h"         // Engine.SetFlag / AddItem / ...
#include "Gameplay/GameFlowState.h"     // StateChangedEvent -> Lua bus
#include "Input/InputSystem.h"
#include "Input/InputTypes.h"          // GameAction / PadBtn / ActionState (Input.* Lua surface)
#include "System/EventBus.h"
#include "System/Log.h"
#include "System/CommandLine.h"  // -lua_print_legacy (the print->log reverse arm)
#include "System/PathUtil.h"
#include "System/HostPolicy.h"   // Sys::IsEditorHost / HotReloadEnabled
#include "System/TaskSystem.h"   // Test.ThrowFromTask
#include "Platform/MainWindow.h" // Test.Window / Test.WindowState (the window probe)
#include "Resource/AssetFS.h"
#include "Resource/EngineSettings.h"   // Input.SaveBindings()

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"   // LuaEntity + LuaId::* (needs sol first)
#include "Scripting/LuaGc.h"         // value usertypes without a finalizer
#include "Scripting/LuaEntityWrite.h" // Engine.AddComponent on an existing component
#include "Scripting/LuaEntityBindings.h" // LuaEntityDedicatedTags (serializers that stamp GUIDs)

#include <array>
#include <cctype>
#include <chrono>       // std::chrono::milliseconds (Test.Stall)
#include <cmath>
#include <cstring>
#include <filesystem>
#include <tuple>
#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

using namespace DirectX;
void RegisterInputProfiles(sol::state& lua);
namespace Scripting
{
    // The window probe's Test.* functions (LuaTestWindowBindings.cpp, WITH_HARNESS only).
    void RegisterLuaTestWindow(sol::table& test, int (*resolveKey)(sol::this_state, const sol::object&));
}
namespace fs = std::filesystem;


#include "ScriptSystem_Internal.h"   // private to src/Scripting - not on the include path
using namespace ScriptInternal;   // RunScriptAsset / GetInstance*

// ==== ScriptSystem::RegisterBindings: the whole Lua-facing C++ surface ====
// Its own translation unit for build time (heavy sol2 usertype instantiation). The
// anonymous-namespace helpers are used only here; ShouldWarnInputOnce owns warn-once
// state that must not be duplicated.

namespace
{

    // parent -> children over the UIParent graph, rebuilt per call.
    // UIParent stores only child -> parent, so a downward walk inverts it first. Children
    // are ordered like PrefabSerializer::BuildUiChildMap (siblingIndex, then entity id),
    // the order UICanvasSystem lays out and draws in, so a name lookup returns the node
    // the artist sees first. O(N) over the UI pool; deliberately not cached: any spawn,
    // destroy or reparent invalidates it, and a stale map binds values into the wrong
    // instance.
    std::unordered_map<Entity, std::vector<Entity>> BuildUIChildLists(World& world)
    {
        std::unordered_map<Entity, std::vector<Entity>> kids;
        auto* pp = world.GetPool<UI::UIParent>();
        if (!pp) return kids;

        const auto& ents = pp->Entities();
        const auto& data = pp->Data();
        for (size_t i = 0; i < data.size() && i < ents.size(); ++i)
        {
            if (!world.IsAlive(ents[i])) continue;

            // Resolve the GUID edge instead of trusting UIParent::parent: the CvParent
            // deserializer stores only parentGuid and UICanvasSystem fills the entity on its next
            // tick, so inside a freshly spawned prefab every edge is still NullEntity - and
            // binding values into a just-spawned template (Engine.SpawnUIPrefab) is a same-frame,
            // pre-tick operation.
            Entity parent = data[i].parent;
            if ((parent == NullEntity || !world.IsAlive(parent))
                && data[i].parentGuid.IsValid())
            {
                parent = ECS::GuidRegistry::Get().Find(data[i].parentGuid);
            }
            if (parent == NullEntity || !world.IsAlive(parent)) continue;
            kids[parent].push_back(ents[i]);
        }

        auto* rp = world.GetPool<UI::UIRect>();
        for (auto& kv : kids)
        {
            std::sort(kv.second.begin(), kv.second.end(),
                [rp](Entity a, Entity b)
                {
                    const auto* ra = rp ? rp->Get(a) : nullptr;
                    const auto* rb = rp ? rp->Get(b) : nullptr;
                    const int sa = ra ? ra->siblingIndex : 0;
                    const int sb = rb ? rb->siblingIndex : 0;
                    return sa != sb ? sa < sb : a < b;
                });
        }
        return kids;
    }

    // ---- Component proxy userdata ----
    // Engine.GetLocalTransform / GetLight / ... hand Lua a proxy holding (entity,
    // generation), never a T*: pool Add can realloc and Erase swaps with the back, so a
    // cached raw pointer (OnSpawn -> OnUpdate, or across a same-frame spawn) would
    // read/write freed memory that pcall cannot catch. Every property access re-fetches
    // through ScriptSystem::LuaWorld(); a stale or dead entity reads type defaults and
    // drops writes (the getter's nil return at fetch time is the loud signal).
    template <typename T>
    struct LuaComponentRef
    {
        ScriptSystem* sys = nullptr;
        Entity        e   = NullEntity;
        uint32_t      gen = 0;

        T* Fetch() const
        {
            World* w = sys ? sys->LuaWorld() : nullptr;
            if (!w || !w->IsAlive(e)) return nullptr;
            if (gen != 0 && (w->GetGeneration(e) & LuaId::kGenMask) != gen)
                return nullptr;
            return w->GetComponent<T>(e);
        }
    };

    using LuaTransformRef      = LuaComponentRef<LocalTransform>;
    using LuaLightRef          = LuaComponentRef<LightData>;
    using LuaCameraRef         = LuaComponentRef<CameraComponent>;
    using LuaCameraCtrlRef     = LuaComponentRef<CameraControllerComponent>;
    using LuaAnimationRef      = LuaComponentRef<AnimationComponent>;
    using LuaMotionDirectorRef = LuaComponentRef<MotionDirectorComponent>;
    using LuaMaterialRef       = LuaComponentRef<MaterialComponent>;

    // Property plumbing: read returns the field (or `def` when the component
    // is gone), write lands only while the component is reachable. T must
    // match the member's declared type; enums/vec fields use bespoke lambdas.
    template <typename Ref, typename T, typename MP>
    auto RefGet(MP field, T def)
    {
        return [field, def](Ref& r) -> T
        {
            if (auto* c = r.Fetch()) return c->*field;
            return def;
        };
    }
    template <typename Ref, typename T, typename MP>
    auto RefSet(MP field)
    {
        return [field](Ref& r, T v)
        {
            if (auto* c = r.Fetch()) c->*field = v;
        };
    }

    // ---- MaterialComponent property plumbing ----
    // Material setters do not use RefSet and never write unconditionally:
    // - A raw field write skips SetDirty(): GPU_LINEAR_CACHE_DIRTY never rises and the
    //   renderer uploads the stale _linearEmissiveColor cache, while HashMatParams (which
    //   hashes the raw field) still mints a new GPU material slot. So every setter calls
    //   the real Set* mutator - not SetEditDirty*, which also raises ASSET_DIRTY and marks
    //   the shared .imat unsaved. Engine.SetMaterialBlendMode is the deliberate exception:
    //   an authoring edit that mirrors the inspector combo (Play mode discards it at Stop).
    // - SetDirty() puts the entity on Renderer's m_sceneMatDirty, and a non-empty list
    //   disables the retained candidate cache and the DDGI/RT retained scene gather for
    //   the whole world that frame. So every setter no-ops when the value is unchanged;
    //   scripts should still quantize an animated value (asset/scripts/logic/StreetLamp.lua)
    //   so it settles instead of jittering in the last mantissa bits.
    template <typename SetFn>
    auto MatFloatProp(float MaterialComponent::* field, SetFn setter)
    {
        return sol::property(
            [field](LuaMaterialRef& r) -> float
            {
                if (auto* m = r.Fetch()) return m->*field;
                return 0.f;
            },
            [field, setter](LuaMaterialRef& r, float v)
            {
                auto* m = r.Fetch();
                if (!m || m->*field == v) return;
                (m->*setter)(v);
            });
    }

    // ---- Texture-slot helpers (Engine.Set/GetMaterialTexture) ----
    // Engine slot name -> index, generated from the SAME X-macro that declares the slots,
    // so the accepted spellings cannot drift from the enum. Case-insensitive; the names
    // are the enum ones (BASECOLORMAP, NORMALMAP, SURFACEMAP, EMISSIVEMAP, RAMPMAP,
    // OPACITYMAP). -1 = not an engine slot.
    int EngineTexSlotFromName(std::string name)
    {
        for (char& c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
#define X(ENUM, LABEL, DEFPATH) if (name == #ENUM) return MaterialComponent::ENUM;
        MATERIAL_TEXTURE_SLOTS(X)
#undef X
        return -1;
    }

    std::string NormalizeAssetPath(std::string p)
    {
        for (char& c : p) if (c == '\\') c = '/';
        return p;
    }

    // The renderer hashes texture paths into the material runtime key through a
    // separator- AND case-normalising hash, so "Asset/T.itex" and "asset/T.itex"
    // are the SAME binding to it. A no-op guard must agree, or an
    // equivalent-but-differently-spelled path marks the material dirty (a
    // scene-wide raster + DDGI re-gather that frame) and then gets early-outed
    // by SyncMaterialTextures because the key is unchanged: pure cost, zero
    // effect.
    std::string PathCompareKey(const std::string& p)
    {
        std::string k = NormalizeAssetPath(p);
        for (char& c : k)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return k;
    }

    // ---- Input action argument resolution ----
    // Input.Held / Pressed / Released / GlyphToken / GetBinding / SetBinding accept
    // Action.Jump (an int from the `Action` table) or the string "Jump". A misspelled
    // name makes the query return false forever, so it warns - once per distinct key,
    // because these are polled every frame.
    // Threading: Lua runs on the main thread only. ScriptTimerSystem, ScriptLogicSystem
    // and AIBTTickSystem keep the default ISystem::DeclareAccess (Opaque), so the
    // Scheduler runs each alone, on the calling thread (the App::Run main loop), and
    // AI::AISystem has no ParallelFor. The mutex stays anyway: one DeclareAccess override
    // would make it necessary, and only the failure path takes it.
    // ShouldWarnInputOnce returns true the first time `key` is seen; `key` is not always
    // the offending value (every out-of-range int shares one key). The set is capped at
    // kMaxInputWarnKeys because the keys are script data, and a script that builds names
    // at runtime would grow it forever.
    constexpr std::size_t kMaxInputWarnKeys = 64;

    bool ShouldWarnInputOnce(const std::string& key)
    {
        static std::mutex                      s_warnMutex;
        static std::unordered_set<std::string> s_warned;
        static bool                            s_capReported = false;

        std::lock_guard<std::mutex> lock(s_warnMutex);
        if (s_warned.count(key)) return false;              // already reported
        if (s_warned.size() >= kMaxInputWarnKeys)
        {
            // Logged from inside the lock on purpose: it happens at most once
            // for the whole process, and LOG_* only enqueues (async logger).
            if (!s_capReported)
            {
                s_capReported = true;
                LOG_WARNING("Lua: %zu distinct input-argument warnings reported — "
                            "further ones are suppressed. A script is very likely "
                            "building action names at runtime.",
                            kMaxInputWarnKeys);
            }
            return false;
        }
        s_warned.insert(key);
        return true;
    }

    void WarnUnknownActionOnce(const std::string& name)
    {
        if (!ShouldWarnInputOnce(name)) return;
        LOG_WARNING("Lua: unknown input action \"%s\" — the query returns false. "
                    "Valid names come from ToString(GameAction); the `Action` "
                    "table lists them all (Action.Jump, Action.Sprint, ...).",
                    name.c_str());
    }

    const char* LuaTypeLabel(sol::type t) noexcept
    {
        switch (t)
        {
        case sol::type::none:     return "no argument";
        case sol::type::lua_nil:  return "nil";
        case sol::type::boolean:  return "a boolean";
        case sol::type::table:    return "a table";
        case sol::type::function: return "a function";
        case sol::type::userdata: return "userdata";
        default:                  return "an unexpected value";
        }
    }

    // Returns false (and warns once) when the argument names nothing.
    bool ResolveGameAction(const sol::object& arg, GameAction& out)
    {
        if (arg.get_type() == sol::type::number)
        {
            // Action.* values are the raw enumerator ints. Out-of-range means a
            // script built the number itself; treat it like a bad name.
            const int v = arg.as<int>();
            if (v < 0 || v >= kGameActionCount)
            {
                // One key for every out-of-range integer: a script passing a runtime int (an
                // entity id, a loop counter) would otherwise grow the warn set without bound. Only
                // the first offender is named.
                if (ShouldWarnInputOnce("<action-int-range>"))
                    LOG_WARNING("Lua: input action id %d is out of range [0,%d) — "
                                "the query returns false. Pass Action.Jump (or "
                                "\"Jump\"), not a hand-built number. Further "
                                "out-of-range ids are not reported.",
                                v, kGameActionCount);
                return false;
            }
            out = static_cast<GameAction>(v);
            return true;
        }
        if (arg.get_type() == sol::type::string)
        {
            const std::string name = arg.as<std::string>();
            if (ParseGameAction(name.c_str(), out)) return true;
            WarnUnknownActionOnce(name);
            return false;
        }

        // nil / bool / table / function. Most likely a typo on the Action table itself:
        // `Action.Jmup` is nil, so Input.Pressed(Action.Jmup) would silently return false
        // forever. Fixed key so a per-frame poll cannot spam. The Action table's __index
        // metamethod (RegisterBindings) also names the bad key at the lookup, which is the
        // more useful message.
        if (ShouldWarnInputOnce("<action-bad-type>"))
            LOG_WARNING("Lua: Input.* received %s instead of an action — the query "
                        "returns false. A misspelled Action.<Name> is nil; pass "
                        "Action.Jump or the string \"Jump\".",
                        LuaTypeLabel(arg.get_type()));
        return false;
    }

    // ---- Lua-number -> engine range checks ----
    // A plain static_cast<uint16_t> of a Lua number wraps (vk1 = 70000 installs vk 4464,
    // a key nobody can press). Reject instead, warn once on a FIXED key (a rebind screen
    // can drive these every frame) and let the caller abandon the whole write.
    constexpr int kMaxVirtualKey = 255;      // Input::kNumKeys - 1
    constexpr int kMaxPadMask    = 0xFFFF;   // PadBtn::Mask is a uint16 bitset

    bool ResolveVk(int v, std::uint16_t& out)
    {
        if (v < 0 || v > kMaxVirtualKey)
        {
            if (ShouldWarnInputOnce("<vk-range>"))
                LOG_WARNING("Lua: virtual key %d is out of range [0,%d] — the "
                            "binding write was rejected. Use the Keys table "
                            "(Keys.Space, Keys.A), not a raw number.",
                            v, kMaxVirtualKey);
            return false;
        }
        out = static_cast<std::uint16_t>(v);
        return true;
    }

    bool ResolvePadMask(int v, std::uint16_t& out)
    {
        if (v < 0 || v > kMaxPadMask)
        {
            if (ShouldWarnInputOnce("<pad-mask-range>"))
                LOG_WARNING("Lua: pad button mask %d is out of range [0,%d] — "
                            "rejected. Use the Pad table (Pad.A, Pad.LB | Pad.RB).",
                            v, kMaxPadMask);
            return false;
        }
        out = static_cast<std::uint16_t>(v);
        return true;
    }

    // Fill an Axis2DBinding (the WASD cross + stick composite) from a Lua table.
    // Absent keys become 0; they do NOT inherit the base table. A context composite is
    // all-or-nothing (InputContext::definesMove / definesLook are single bools), so a
    // context that declares `move` owns the whole cross. Returns false on any
    // out-of-range field; the caller then rejects the whole context.
    bool ResolveAxis2D(const sol::table& t, Axis2DBinding& out)
    {
        Axis2DBinding b {};
        if (!ResolveVk(t.get_or("vkPosX", 0), b.vkPosX)) return false;
        if (!ResolveVk(t.get_or("vkNegX", 0), b.vkNegX)) return false;
        if (!ResolveVk(t.get_or("vkPosY", 0), b.vkPosY)) return false;
        if (!ResolveVk(t.get_or("vkNegY", 0), b.vkNegY)) return false;

        // stick is an INDEX (0 = left, 1 = right), not a mask. A static_cast of
        // anything else wraps into a stick that does not exist, and the composite
        // then reads a garbage axis pair with no error anywhere.
        const int stick = t.get_or("stick", 0);
        if (stick != 0 && stick != 1)
        {
            if (ShouldWarnInputOnce("<context-stick>"))
                LOG_WARNING("Lua: composite `stick` must be 0 (left) or 1 (right), "
                            "got %d — the context was rejected.", stick);
            return false;
        }
        b.stick = static_cast<std::uint8_t>(stick);
        out = b;
        return true;
    }

// The .lua file:line of the code that called the current C++ binding, for Log.*.
// Uses the core lua_getstack / lua_getinfo API (the `debug` library is not opened).
// Level 1 = the Lua function that called us.
std::string LuaCallSite(lua_State* L)
{
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar) && ar.currentline > 0)
    {
        const char* src = ar.short_src[0] ? ar.short_src : "?";
        return std::string(src) + ":" + std::to_string(ar.currentline);
    }
    return "?";
}

// A script's own line: `Lua [<file>:<line>]: <msg>`, attributed to
// LogSource::Lua - unless a console command or a control request is what is
// running this script, in which case the line stays with THAT request (its
// ScopedContext source), so "what did my command log" includes its prints.
void LuaLog(LogLevel level, lua_State* L, const std::string& msg)
{
    const LogSource ctx = Logger::CurrentSource();
    Logger::Get().LogFrom(ctx == LogSource::Engine ? LogSource::Lua : ctx, level, __FILE__, __LINE__,
                          "Lua [%s]: %s", LuaCallSite(L).c_str(), msg.c_str());
}

// print(...) -> the log, formatted like the reference print (tostring of each
// argument, honouring __tostring / __name, TAB-separated): the base print writes to
// stdout, which a Windows-subsystem process does not have. The text is built in a
// luaL_Buffer, not a std::string: luaL_tolstring can raise a Lua error (a throwing
// __tostring), and Lua is built as C here, so the longjmp would skip a live C++
// object's destructor.
int LuaPrintToLog(lua_State* L)
{
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i)
    {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, nullptr);   // pushes tostring(arg i)
        luaL_addvalue(&b);               // pops it into the buffer
    }
    luaL_pushresult(&b);
    size_t len = 0;
    const char* s = lua_tolstring(L, -1, &len);
    // Nothing below can raise a Lua error.
    LuaLog(LogLevel::Info, L, std::string(s ? s : "", len));
    return 0;
}
}

void ScriptSystem::RegisterBindings()
{
    auto& lua = *m_lua;
    RegisterInputProfiles(lua);

    // ---- Vec3 ----
    lua.new_usertype<LuaVec3>("Vec3",
        sol::constructors<LuaVec3(), LuaVec3(float,float,float)>(),
        "x", &LuaVec3::x,
        "y", &LuaVec3::y,
        "z", &LuaVec3::z,
        "Length",     &LuaVec3::Length,
        "LengthSq",   &LuaVec3::LengthSq,
        "Normalized", &LuaVec3::Normalized,
        "Dot",        &LuaVec3::Dot,
        "Cross",      &LuaVec3::Cross,
        "Distance",   &LuaVec3::Distance,
        sol::meta_function::addition,       &LuaVec3::operator+,
        sol::meta_function::subtraction,
            static_cast<LuaVec3 (LuaVec3::*)(const LuaVec3&) const>(&LuaVec3::operator-),
        sol::meta_function::unary_minus,
            static_cast<LuaVec3 (LuaVec3::*)() const>(&LuaVec3::operator-),
        sol::meta_function::multiplication, sol::overload(
            &LuaVec3::operator*,
            [](float s, const LuaVec3& v) { return v * s; }),
        sol::meta_function::division,       &LuaVec3::operator/,
        sol::meta_function::equal_to,       &LuaVec3::operator==,
        sol::meta_function::to_string, [](const LuaVec3& v) {
            char buf[64]; snprintf(buf, sizeof(buf), "Vec3(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
            return std::string(buf);
        }
    );
    // Statics reachable as Vec3.Lerp(a, b, t).
    {
        sol::table vt = lua["Vec3"];
        vt.set_function("Lerp", &LuaVec3::Lerp);
    }

    // ---- Quat ----
    // Conventions match the engine: FromEuler is XMQuaternionRotationRollPitchYaw (pitch,
    // yaw, roll in DEGREES, as Engine.RotateTo); q1 * q2 composes, q * v rotates a Vec3.
    lua.new_usertype<LuaQuat>("Quat",
        sol::constructors<LuaQuat(), LuaQuat(float,float,float,float)>(),
        "x", &LuaQuat::x, "y", &LuaQuat::y, "z", &LuaQuat::z, "w", &LuaQuat::w,
        "Normalized", &LuaQuat::Normalized,
        "Conjugate",  &LuaQuat::Conjugate,
        "Dot",        &LuaQuat::Dot,
        "Rotate",     &LuaQuat::Rotate,
        sol::meta_function::multiplication, sol::overload(
            &LuaQuat::operator*,
            [](const LuaQuat& q, const LuaVec3& v) { return q.Rotate(v); }),
        sol::meta_function::to_string, [](const LuaQuat& q) {
            char buf[80]; snprintf(buf, sizeof(buf), "Quat(%.3f, %.3f, %.3f, %.3f)", q.x, q.y, q.z, q.w);
            return std::string(buf);
        }
    );
    // Statics: Quat.FromEuler(pitchDeg, yawDeg, rollDeg), Quat.AngleAxis(deg,
    // axisVec3), Quat.Slerp(a, b, t), Quat.Identity().
    {
        sol::table qt = lua["Quat"];
        qt.set_function("FromEuler", &LuaQuat::FromEuler);
        qt.set_function("AngleAxis", &LuaQuat::AngleAxis);
        qt.set_function("Slerp",     &LuaQuat::Slerp);
        qt.set_function("Identity",  []() { return LuaQuat{}; });
    }

    // ---- LocalTransform (proxy, see LuaComponentRef) ----
    lua.new_usertype<LuaTransformRef>("LocalTransform",
        sol::no_constructor,
        "translation", sol::property(
            [](LuaTransformRef& r) -> LuaVec3 {
                if (auto* lt = r.Fetch()) return {lt->translation.x, lt->translation.y, lt->translation.z};
                return {};
            },
            [](LuaTransformRef& r, LuaVec3 v) {
                if (auto* lt = r.Fetch()) lt->translation = {v.x, v.y, v.z};
            }),
        "scale", sol::property(
            [](LuaTransformRef& r) -> LuaVec3 {
                if (auto* lt = r.Fetch()) return {lt->scale.x, lt->scale.y, lt->scale.z};
                return {1.f, 1.f, 1.f};
            },
            [](LuaTransformRef& r, LuaVec3 v) {
                if (auto* lt = r.Fetch()) lt->scale = {v.x, v.y, v.z};
            }),
        "rotation", sol::property(
            [](LuaTransformRef& r) -> LuaQuat {
                if (auto* lt = r.Fetch()) return {lt->rotation.x, lt->rotation.y, lt->rotation.z, lt->rotation.w};
                return {};
            },
            [](LuaTransformRef& r, LuaQuat q) {
                if (auto* lt = r.Fetch()) lt->rotation = {q.x, q.y, q.z, q.w};
            })
    );

    // ---- LightData (proxy) ----
    lua.new_usertype<LuaLightRef>("Light",
        sol::no_constructor,
        "radius",      sol::property(RefGet<LuaLightRef>(&LightData::radius,      0.f),
                                     RefSet<LuaLightRef, float>(&LightData::radius)),
        "intensity",   sol::property(RefGet<LuaLightRef>(&LightData::intensity,   0.f),
                                     RefSet<LuaLightRef, float>(&LightData::intensity)),
        "spotAngle",   sol::property(RefGet<LuaLightRef>(&LightData::spotAngle,   0.f),
                                     RefSet<LuaLightRef, float>(&LightData::spotAngle)),
        "enabled", sol::property(RefGet<LuaLightRef>(&LightData::enabled, true),
                                 RefSet<LuaLightRef, bool>(&LightData::enabled)),
        "castsShadow", sol::property(RefGet<LuaLightRef>(&LightData::castsShadow, false),
                                     RefSet<LuaLightRef, bool>(&LightData::castsShadow)),
        "sourceRadius", sol::property(RefGet<LuaLightRef>(&LightData::sourceRadius, 0.f),
                                      RefSet<LuaLightRef, float>(&LightData::sourceRadius)),
        // Read-only: the renderer's first-sight stamp (LightData::gatherSeq).
        // Lights are gathered in ascending order - the sun is the directional
        // with the LARGEST, shadow slices go to the smallest. 0 = not gathered yet.
        "gatherSeq", sol::readonly_property(
            [](LuaLightRef& r) -> uint32_t {
                if (auto* l = r.Fetch()) return l->gatherSeq;
                return 0u;
            }),
        "type", sol::property(
            [](LuaLightRef& r) -> int  {
                if (auto* l = r.Fetch()) return static_cast<int>(l->type);
                return 0;
            },
            [](LuaLightRef& r, int v)  {
                if (auto* l = r.Fetch()) l->type = static_cast<LightType>(v);
            }),
        "color", sol::property(
            [](LuaLightRef& r) -> LuaVec3 {
                if (auto* l = r.Fetch()) return {l->color.x, l->color.y, l->color.z};
                return {};
            },
            [](LuaLightRef& r, LuaVec3 v) {
                if (auto* l = r.Fetch()) l->color = {v.x, v.y, v.z};
            }),
        // `direction` is derived from the entity transform (forward = +Z), not stored on
        // LightData (LightUtil.h). Reading it returns the entity's world forward; writing it
        // rotates the entity's LocalTransform to aim that way, so a parented light composes
        // normally and the next propagate refreshes GlobalTransform.
        "direction", sol::property(
            [](LuaLightRef& r) -> LuaVec3 {
                World* w = r.sys ? r.sys->LuaWorld() : nullptr;
                if (!w || !w->IsAlive(r.e)) return {};
                if (const auto* gt = w->GetComponent<GlobalTransform>(r.e))
                {
                    const DirectX::XMFLOAT3 d = LightUtil::DirectionFromWorldF(gt->matrix);
                    return { d.x, d.y, d.z };
                }
                return {};
            },
            [](LuaLightRef& r, LuaVec3 v) {
                World* w = r.sys ? r.sys->LuaWorld() : nullptr;
                if (!w || !w->IsAlive(r.e)) return;
                if (auto* lt = w->GetComponent<LocalTransform>(r.e))
                    lt->rotation = LightUtil::RotationAiming({ v.x, v.y, v.z });
            })
    );

    lua["LightType"] = lua.create_table_with(
        "Directional", 0, "Point", 1, "Spot", 2);

    // ---- MaterialComponent (proxy) ----
    // From Engine.GetMaterial(id). Writes change THIS entity only: SceneInstanceLoader
    // gives every mesh entity its own MaterialComponent copy.
    // The .imat is on the MESH entity, not a prefab root: GetMaterial(prefabRoot) is nil;
    // walk the subtree (Engine.FindMaterialDescendant) or name the child
    // (Engine.FindChildByName).
    // emissiveColor / baseColor are sRGB, pow(c, 2.2)'d by the renderer on upload; the
    // scalar channels (emissiveStrength, opacity, roughness, ...) pass through.
    // Emissive is not a light: it is written straight to the HDR target and lights
    // nothing (bar indirect DDGI / reflections); pair it with a LightData child.
    // Reads are plain field reads; every write goes through the real Set* mutator and
    // no-ops when unchanged (see MatFloatProp).
    lua.new_usertype<LuaMaterialRef>("Material",
        sol::no_constructor,
        "roughnessMin",      MatFloatProp(&MaterialComponent::roughnessMin,      &MaterialComponent::SetRoughnessMin),
        "roughnessMax",      MatFloatProp(&MaterialComponent::roughnessMax,      &MaterialComponent::SetRoughnessMax),
        "metalnessMin",      MatFloatProp(&MaterialComponent::metalnessMin,      &MaterialComponent::SetMetalnessMin),
        "metalnessMax",      MatFloatProp(&MaterialComponent::metalnessMax,      &MaterialComponent::SetMetalnessMax),
        "saturation",        MatFloatProp(&MaterialComponent::saturation,        &MaterialComponent::SetSaturation),
        "reflectance",       MatFloatProp(&MaterialComponent::reflectance,       &MaterialComponent::SetReflectance),
        "normalMapStrength", MatFloatProp(&MaterialComponent::normalMapStrength, &MaterialComponent::SetNormalMapStrength),
        "alphaRef",          MatFloatProp(&MaterialComponent::alphaRef,          &MaterialComponent::SetAlphaRef),
        // BlendMode::Transmissive's fields (glTF transmission / volume / ior); inert on other modes.
        "transmission",        MatFloatProp(&MaterialComponent::transmission,        &MaterialComponent::SetTransmission),
        "ior",                 MatFloatProp(&MaterialComponent::ior,                 &MaterialComponent::SetIor),
        "thickness",           MatFloatProp(&MaterialComponent::thickness,           &MaterialComponent::SetThickness),
        "attenuationDistance", MatFloatProp(&MaterialComponent::attenuationDistance, &MaterialComponent::SetAttenuationDistance),

        // emissiveColor.w: the HDR strength multiplier, i.e. "turn the lamp on". Linear
        // radiance: ~2..16 is the useful band; below ~1 vanishes under ACES once
        // auto-exposure lifts a dark scene. The stock shader multiplies it by the EMISSIVEMAP
        // slot, whose engine default is a 1x1 BLACK texture, so on a material with that
        // default this does nothing: clear the slot in the .imat (empty path = unbound = no
        // multiply) or bind a real map.
        "emissiveStrength", sol::property(
            [](LuaMaterialRef& r) -> float {
                if (auto* m = r.Fetch()) return m->GetEmissiveStrength();
                return 0.f;
            },
            [](LuaMaterialRef& r, float v) {
                auto* m = r.Fetch();
                if (!m || m->emissiveColor.w == v) return;
                m->SetEmissiveStrength(v);
            }),

        // baseColor.w: meaningful only on a translucent / masked blend mode.
        "opacity", sol::property(
            [](LuaMaterialRef& r) -> float {
                if (auto* m = r.Fetch()) return m->GetOpacity();
                return 0.f;
            },
            [](LuaMaterialRef& r, float v) {
                auto* m = r.Fetch();
                if (!m || m->baseColor.w == v) return;
                m->SetOpacity(v);
            }),

        // The two colours are rgb-only (Vec3). Their .w channels are the
        // separate emissiveStrength / opacity properties above and are
        // PRESERVED across a colour write, so the two never fight.
        "emissiveColor", sol::property(
            [](LuaMaterialRef& r) -> LuaVec3 {
                if (auto* m = r.Fetch())
                    return { m->emissiveColor.x, m->emissiveColor.y, m->emissiveColor.z };
                return {};
            },
            [](LuaMaterialRef& r, LuaVec3 v) {
                auto* m = r.Fetch();
                if (!m) return;
                if (m->emissiveColor.x == v.x && m->emissiveColor.y == v.y
                    && m->emissiveColor.z == v.z) return;
                m->SetEmissiveColor({ v.x, v.y, v.z, m->emissiveColor.w });
            }),
        "baseColor", sol::property(
            [](LuaMaterialRef& r) -> LuaVec3 {
                if (auto* m = r.Fetch())
                    return { m->baseColor.x, m->baseColor.y, m->baseColor.z };
                return {};
            },
            [](LuaMaterialRef& r, LuaVec3 v) {
                auto* m = r.Fetch();
                if (!m) return;
                if (m->baseColor.x == v.x && m->baseColor.y == v.y
                    && m->baseColor.z == v.z) return;
                m->SetBaseColor({ v.x, v.y, v.z, m->baseColor.w });
            }),
        // sRGB, like the Inspector's swatch.
        "attenuationColor", sol::property(
            [](LuaMaterialRef& r) -> LuaVec3 {
                if (auto* m = r.Fetch())
                    return { m->attenuationColor.x, m->attenuationColor.y, m->attenuationColor.z };
                return {};
            },
            [](LuaMaterialRef& r, LuaVec3 v) {
                auto* m = r.Fetch();
                if (!m) return;
                if (m->attenuationColor.x == v.x && m->attenuationColor.y == v.y
                    && m->attenuationColor.z == v.z) return;
                m->SetAttenuationColor({ v.x, v.y, v.z, 1.0f });
            }),

        "castsShadow", sol::property(
            [](LuaMaterialRef& r) -> bool {
                if (auto* m = r.Fetch()) return m->IsCastingShadow();
                return false;
            },
            [](LuaMaterialRef& r, bool v) {
                auto* m = r.Fetch();
                if (!m || m->IsCastingShadow() == v) return;
                m->SetCastShadow(v);
            })
    );

    // ---- CameraComponent (lens / view-projection; proxy) ----
    // The camera pose is on LocalTransform: move it via GetLocalTransform(id).translation
    // / .rotation, not through this usertype.
    lua.new_usertype<LuaCameraRef>("Camera",
        sol::no_constructor,
        "fov",   sol::property(RefGet<LuaCameraRef>(&CameraComponent::fov,   60.f),
                               RefSet<LuaCameraRef, float>(&CameraComponent::fov)),
        "nearZ", sol::property(RefGet<LuaCameraRef>(&CameraComponent::nearZ, 0.1f),
                               RefSet<LuaCameraRef, float>(&CameraComponent::nearZ)),
        "farZ",  sol::property(RefGet<LuaCameraRef>(&CameraComponent::farZ,  1000.f),
                               RefSet<LuaCameraRef, float>(&CameraComponent::farZ))
    );

    // ---- CameraControllerComponent (FPS controller state; proxy) ----
    lua.new_usertype<LuaCameraCtrlRef>("CameraController",
        sol::no_constructor,
        "yaw",              sol::property(RefGet<LuaCameraCtrlRef>(&CameraControllerComponent::yaw,   0.f),
                                          RefSet<LuaCameraCtrlRef, float>(&CameraControllerComponent::yaw)),
        "pitch",            sol::property(RefGet<LuaCameraCtrlRef>(&CameraControllerComponent::pitch, 0.f),
                                          RefSet<LuaCameraCtrlRef, float>(&CameraControllerComponent::pitch)),
        "mouseSensitivity", sol::property(RefGet<LuaCameraCtrlRef>(&CameraControllerComponent::mouseSensitivity, 0.f),
                                          RefSet<LuaCameraCtrlRef, float>(&CameraControllerComponent::mouseSensitivity)),
        "moveSpeed",        sol::property(RefGet<LuaCameraCtrlRef>(&CameraControllerComponent::moveSpeed, 0.f),
                                          RefSet<LuaCameraCtrlRef, float>(&CameraControllerComponent::moveSpeed))
    );

    // ---- AnimationComponent (proxy) ----
    lua.new_usertype<LuaAnimationRef>("Animation",
        sol::no_constructor,
        "primaryClip",   sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::primaryClip,   kInvalidAnimHandle),
                                       RefSet<LuaAnimationRef, uint32_t>(&AnimationComponent::primaryClip)),
        "secondaryClip", sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::secondaryClip, kInvalidAnimHandle),
                                       RefSet<LuaAnimationRef, uint32_t>(&AnimationComponent::secondaryClip)),
        "primaryTime",   sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::primaryTime,   0.f),
                                       RefSet<LuaAnimationRef, float>(&AnimationComponent::primaryTime)),
        "secondaryTime", sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::secondaryTime, 0.f),
                                       RefSet<LuaAnimationRef, float>(&AnimationComponent::secondaryTime)),
        "blendWeight",   sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::blendWeight,   0.f),
                                       RefSet<LuaAnimationRef, float>(&AnimationComponent::blendWeight)),
        "speed",         sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::speed,         1.f),
                                       RefSet<LuaAnimationRef, float>(&AnimationComponent::speed)),
        "looping",       sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::looping,       true),
                                       RefSet<LuaAnimationRef, bool>(&AnimationComponent::looping)),
        "paused",        sol::property(RefGet<LuaAnimationRef>(&AnimationComponent::paused,        false),
                                       RefSet<LuaAnimationRef, bool>(&AnimationComponent::paused))
    );

    // ---- MotionDirectorComponent (master-clock performance transport) ----
    // Raw transport fields, plus two read-only conveniences so a Lua HUD never
    // has to know how the total is sourced:
    //   .total     = authored `duration`, or the track span auto-resolved by
    //                MotionDirectorTickSystem when duration == 0 (0 until ready).
    //   .remaining = seconds left, loop-aware; 0 while .total is still unknown.
    lua.new_usertype<LuaMotionDirectorRef>("MotionDirector",
        sol::no_constructor,
        "masterSpeed",      sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::masterSpeed, 1.f),
                                          RefSet<LuaMotionDirectorRef, float>(&MotionDirectorComponent::masterSpeed)),
        "duration",         sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::duration, 0.f),
                                          RefSet<LuaMotionDirectorRef, float>(&MotionDirectorComponent::duration)),
        "resolvedDuration", sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::resolvedDuration, 0.f),
                                          RefSet<LuaMotionDirectorRef, float>(&MotionDirectorComponent::resolvedDuration)),
        "looping",          sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::looping, false),
                                          RefSet<LuaMotionDirectorRef, bool>(&MotionDirectorComponent::looping)),
        "playing",          sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::playing, false),
                                          RefSet<LuaMotionDirectorRef, bool>(&MotionDirectorComponent::playing)),
        "paused",           sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::paused, false),
                                          RefSet<LuaMotionDirectorRef, bool>(&MotionDirectorComponent::paused)),
        "masterTime",       sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::masterTime, 0.f),
                                          RefSet<LuaMotionDirectorRef, float>(&MotionDirectorComponent::masterTime)),
        "stopRequested",    sol::property(RefGet<LuaMotionDirectorRef>(&MotionDirectorComponent::stopRequested, false),
                                          RefSet<LuaMotionDirectorRef, bool>(&MotionDirectorComponent::stopRequested)),
        "total", sol::readonly_property([](LuaMotionDirectorRef& r) -> float {
            const auto* d = r.Fetch();
            if (!d) return 0.f;
            return d->duration > 1e-6f ? d->duration : d->resolvedDuration;
        }),
        "remaining", sol::readonly_property([](LuaMotionDirectorRef& r) -> float {
            const auto* d = r.Fetch();
            if (!d) return 0.f;
            const float total = d->duration > 1e-6f ? d->duration : d->resolvedDuration;
            if (total <= 1e-6f) return 0.f;
            const float rem = d->looping ? (total - std::fmod(d->masterTime, total))
                                         : (total - d->masterTime);
            return rem < 0.f ? 0.f : rem;
        })
    );

    // The per-frame script garbage is made of these: no __gc, so a dead one is
    // freed by the first collection that finds it (Scripting/LuaGc.h).
    {
        lua_State* L = lua.lua_state();
        LuaGc::DropTrivialFinalizer<LuaVec3>(L);
        LuaGc::DropTrivialFinalizer<LuaQuat>(L);
        LuaGc::DropTrivialFinalizer<LuaTransformRef>(L);
        LuaGc::DropTrivialFinalizer<LuaLightRef>(L);
        LuaGc::DropTrivialFinalizer<LuaMaterialRef>(L);
        LuaGc::DropTrivialFinalizer<LuaCameraRef>(L);
        LuaGc::DropTrivialFinalizer<LuaCameraCtrlRef>(L);
        LuaGc::DropTrivialFinalizer<LuaAnimationRef>(L);
        LuaGc::DropTrivialFinalizer<LuaMotionDirectorRef>(L);
    }

    // ---- Input ----
    // Reads the shared Input snapshot, so Lua sees the same edge state as native systems
    // within a frame. IsMouseDown reads the mouse buttons as VK_*BUTTON keys of that
    // snapshot. WasKeyPressed / WasKeyReleased give single-frame transitions without
    // manual edge tracking.
    auto input = lua.create_table();
    input.set_function("IsKeyDown", [](int key) -> bool {
        return Input::Get().IsKeyDown(key);
    });
    input.set_function("WasKeyPressed", [](int key) -> bool {
        return Input::Get().WasKeyPressed(key);
    });
    input.set_function("WasKeyReleased", [](int key) -> bool {
        return Input::Get().WasKeyReleased(key);
    });
    input.set_function("IsMouseDown", [](int button) -> bool {
        int vk = (button == 1) ? VK_RBUTTON : (button == 2) ? VK_MBUTTON : VK_LBUTTON;
        return Input::Get().IsKeyDown(vk);
    });
    input.set_function("GetMousePos", []() -> std::tuple<float,float> {
        POINT p; GetCursorPos(&p);
        // Our window's client space, not the foreground window's (another application's
        // while the game is in the background).
        if (HWND hwnd = Plat::MainWindowHandle())
            ScreenToClient(hwnd, &p);
        return { static_cast<float>(p.x), static_cast<float>(p.y) };
    });

    // ---- Device-agnostic actions (the usercmd) ----
    // Everything below reads Input::Cmd(): the single ActionState latched once per frame
    // on the main thread, a const POD for the whole frame, so every consumer (C++ brain,
    // camera tick, Lua) sees the same pressed / released bits and nothing can
    // double-consume or miss an edge.
    // The five raw functions above are device-aware: shipped scripts pass raw VK integers
    // (Afterimage.lua 0x4C, Shooter.lua 0x46, the scene scripts 0x0D) and bypass the
    // `Keys` table, so raw ints must keep working.
    input.set_function("Held", [](sol::object action) -> bool {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return false;
        return Input::Get().Cmd().Held(a);
    });
    input.set_function("Pressed", [](sol::object action) -> bool {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return false;
        return Input::Get().Cmd().Pressed(a);
    });
    input.set_function("Released", [](sol::object action) -> bool {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return false;
        return Input::Get().Cmd().Released(a);
    });

    // x, y, mag: already post-deadzone, post-curve and clamped. Do NOT re-normalize
    // (x, y): moveMag carries the analog speed the player asked for.
    input.set_function("Move", []() -> std::tuple<float,float,float> {
        const ActionState& c = Input::Get().Cmd();
        return { c.moveX, c.moveY, c.moveMag };
    });

    // mouseDX, mouseDY, stickX, stickY, active. Two carriers, different units: the mouse
    // pair is a DISPLACEMENT in pixels (never multiply by dt), the stick pair a RATE in
    // [-1, 1] (always multiply by dt); mixing them makes look framerate-dependent.
    // The 5th value is ActionState::lookActive (RMB held || right stick past the
    // deadzone), the engine's rotation gate, so a Lua camera agrees with the native camera
    // tick on the frames a gesture starts and ends. It is last on purpose:
    // `local dx, dy, sx, sy = Input.Look()` keeps working.
    input.set_function("Look", []() -> std::tuple<float,float,float,float,bool> {
        const ActionState& c = Input::Get().Cmd();
        return { c.lookMouseDX, c.lookMouseDY, c.lookStickX, c.lookStickY,
                 c.lookActive };
    });
    // Same bit on its own, for scripts that only need the gate.
    input.set_function("LookActive", []() -> bool {
        return Input::Get().Cmd().lookActive;
    });

    // 1 = LT, 2 = RT (1-based to match Lua's indexing everywhere else).
    input.set_function("Trigger", [](int i) -> float {
        if (i != 1 && i != 2) return 0.f;
        return Input::Get().Cmd().trigger[i - 1];
    });
    input.set_function("IsAnalogMove", []() -> bool {
        return Input::Get().Cmd().analogMove;
    });

    // ---- Device identity / prompt glyphs ----
    // Cosmetic only. Gameplay must never branch on the device.
    input.set_function("LastDevice", []() -> std::string {
        return ToString(Input::Get().LastDevice());
    });

    // "pad/a" | "kbm/space" | nil. Copy to std::string IMMEDIATELY: the header
    // only promises the returned pointer stays valid until the next call on
    // this thread, and handing a Lua string that pointer would be a dangle.
    input.set_function("GlyphToken", [](sol::object action) -> sol::optional<std::string> {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return sol::nullopt;
        const char* token = Input::Get().GlyphToken(a);
        if (!token || !*token) return sol::nullopt;      // unbound
        return std::string(token);
    });

    // No argument = "any pad". With an argument = that exact slot.
    input.set_function("IsGamepadConnected", [](sol::optional<int> slot) -> bool {
        const GamepadDevice& pad = Input::Get().Pad();
        return slot ? pad.Connected(*slot) : pad.AnyConnected();
    });

    // The frame-latched slot, not the live device slot, so this agrees with
    // everything else Lua read this frame (a pad suppressed by ImGui/Canvas
    // reports -1 here even though the hardware is still plugged in).
    input.set_function("ActivePadSlot", []() -> int {
        return Input::Get().Cmd().padSlot;
    });

    // QUEUED and applied by the next Poll() on the main thread; QueueRumble is safe from
    // any thread. Omitted slot (or < 0) means "the active pad".
    input.set_function("SetRumble", [](float lo, float hi, sol::optional<int> slot) {
        Input::Get().QueueRumble(slot ? *slot : -1, lo, hi);
    });

    // ---- Raw pad escape hatch ----
    // For UI / debug that needs a physical button (Input.PadDown(Pad.A)) rather than an
    // action; unrebindable by definition, so gameplay uses Held / Pressed.
    // Ungated on purpose, unlike every other Input.* query: these read GamepadDevice
    // directly, not Input::Cmd(), so they ignore UpdateInputs::suppressGamepad
    // (ImGui / Canvas owning the pad) and UpdateInputs::simAdvancing (editor paused): a
    // held button reads true here while Held / Pressed report nothing. Right for a debug
    // overlay or a Lua pause menu; wrong for gameplay.
    input.set_function("PadDown", [](int mask) -> bool {
        std::uint16_t m = 0;
        if (!ResolvePadMask(mask, m)) return false;       // would wrap to a bogus bit
        const GamepadDevice& pad = Input::Get().Pad();
        const int slot = pad.ActiveSlot();
        if (slot < 0) return false;                       // State() masks &3; -1 would alias slot 3
        return pad.State(slot).Down(m);
    });
    input.set_function("PadPressed", [](int mask) -> bool {
        std::uint16_t m = 0;
        if (!ResolvePadMask(mask, m)) return false;
        const GamepadDevice& pad = Input::Get().Pad();
        const int slot = pad.ActiveSlot();
        if (slot < 0) return false;
        return pad.State(slot).Pressed(m);
    });

    // ---- Rebinding ----
    // Read-modify-write through SetBindings(), which Sanitize()s on entry, so a script
    // cannot install an out-of-range tuning value. Sanitize() never UNBINDS anything, so a
    // bad write cannot lock the player out of moving.
    input.set_function("GetBinding", [this](sol::object action) -> sol::optional<sol::table> {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return sol::nullopt;
        const ActionBinding& ab = Input::Get().Bindings().action[static_cast<int>(a)];
        sol::table t = m_lua->create_table();
        t["vk1"] = static_cast<int>(ab.vk[0]);
        t["vk2"] = static_cast<int>(ab.vk[1]);
        t["pad"] = static_cast<int>(ab.pad);
        return t;
    });

    // Absent keys KEEP their current value: Input.SetBinding(Action.Jump, { pad = Pad.A })
    // must not wipe the keyboard binding. Every field is range-checked before anything is
    // installed, and one bad field rejects the whole call: Sanitize() never touches
    // bindings, so a wrapped vk would reach settings.json, and a half-applied rebind is
    // harder to debug than a refusal.
    input.set_function("SetBinding", [](sol::object action, sol::table t) -> bool {
        GameAction a = GameAction::Count;   // sentinel: never read unless resolved
        if (!ResolveGameAction(action, a)) return false;
        InputBindings b  = Input::Get().Bindings();
        ActionBinding& ab = b.action[static_cast<int>(a)];
        std::uint16_t vk0 = 0, vk1 = 0, padMask = 0;
        if (!ResolveVk(t.get_or("vk1", static_cast<int>(ab.vk[0])), vk0))     return false;
        if (!ResolveVk(t.get_or("vk2", static_cast<int>(ab.vk[1])), vk1))     return false;
        if (!ResolvePadMask(t.get_or("pad", static_cast<int>(ab.pad)), padMask)) return false;
        ab.vk[0] = vk0;
        ab.vk[1] = vk1;
        ab.pad   = padMask;
        Input::Get().SetBindings(b);
        return true;
    });

    input.set_function("GetTuning", [this]() -> sol::table {
        const InputBindings& b = Input::Get().Bindings();
        sol::table t = m_lua->create_table();
        t["deadzone"]         = b.stickDeadzone;
        t["lookRate"]         = b.lookRateDegPerSec;
        t["curve"]            = b.stickCurveExp;
        t["mouseScale"]       = b.mouseLookScale;
        t["triggerThreshold"] = b.triggerThreshold;
        t["padInvertY"]       = b.padInvertLookY;
        t["mouseInvertY"]     = b.mouseInvertLookY;
        t["gamepadEnabled"]   = b.gamepadEnabled;
        return t;
    });
    // Absent keys keep their current value, so a script can nudge one knob without
    // restating the others. The current values go through a prvalue cast on purpose:
    // get_or deduces its type from the default argument, and a raw member lvalue deduces
    // `float&` / `bool&`.
    input.set_function("SetTuning", [](sol::table t) {
        InputBindings b = Input::Get().Bindings();
        b.stickDeadzone     = t.get_or("deadzone",         float(b.stickDeadzone));
        b.lookRateDegPerSec = t.get_or("lookRate",         float(b.lookRateDegPerSec));
        b.stickCurveExp     = t.get_or("curve",            float(b.stickCurveExp));
        b.mouseLookScale    = t.get_or("mouseScale",       float(b.mouseLookScale));
        b.triggerThreshold  = t.get_or("triggerThreshold", float(b.triggerThreshold));
        b.padInvertLookY    = t.get_or("padInvertY",       bool(b.padInvertLookY));
        b.mouseInvertLookY  = t.get_or("mouseInvertY",     bool(b.mouseInvertLookY));
        b.gamepadEnabled    = t.get_or("gamepadEnabled",   bool(b.gamepadEnabled));
        Input::Get().SetBindings(b);   // clamps every scalar back into range
    });

    // "Reset controls": bindings only. InputBindings holds the binding table AND the
    // player's tuning scalars (deadzone, curve, look rate, invert flags, gamepadEnabled),
    // so SetBindings(Defaults()) would throw away a stick calibration. Copy the live
    // struct and overwrite only the binding half; ResetTuning is the separate tuning
    // reset (two buttons in every options screen).
    input.set_function("ResetBindings", []() {
        const InputBindings def = InputBindings::Defaults();
        InputBindings b = Input::Get().Bindings();   // keeps every tuning scalar
        for (int i = 0; i < kGameActionCount; ++i)
            b.action[i] = def.action[i];
        b.move = def.move;
        b.look = def.look;
        Input::Get().SetBindings(b);
    });

    // The other half: every player-authored scalar back to the shipped value,
    // bindings untouched. Listed field by field rather than assigning Defaults()
    // wholesale so that adding a tuning field to InputBindings without adding it
    // here is a visible omission, not a silent binding wipe.
    input.set_function("ResetTuning", []() {
        const InputBindings def = InputBindings::Defaults();
        InputBindings b = Input::Get().Bindings();   // keeps every binding
        b.lookRateDegPerSec = def.lookRateDegPerSec;
        b.mouseLookScale    = def.mouseLookScale;
        b.stickDeadzone     = def.stickDeadzone;
        b.stickCurveExp     = def.stickCurveExp;
        b.triggerThreshold  = def.triggerThreshold;
        b.padInvertLookY    = def.padInvertLookY;
        b.mouseInvertLookY  = def.mouseInvertLookY;
        b.gamepadEnabled    = def.gamepadEnabled;
        Input::Get().SetBindings(b);
    });

    // Persist the live bindings. Deliberately a READ-MODIFY-WRITE of the
    // existing settings.json rather than EngineSettings::CaptureFrom: a Lua
    // options screen has no Renderer, and more importantly an in-game "apply
    // controls" button must not silently rewrite the player's graphics settings
    // from whatever the engine happens to be running at that moment.
    input.set_function("SaveBindings", []() -> bool {
        // The file's rows stay as they are (the rows argument is left alone).
        return Resource::EditEngineSettingsFile([](Resource::EngineSettings& s, std::vector<uint8_t>&) {
            s.CaptureInputFrom(Input::Get().Bindings());
        });
    });

    // ---- Movement / look composite ----
    // Input.SetBinding reaches only InputBindings::action[]; the WASD composite and the
    // stick selection live in `move` / `look`, read and written here (CaptureInputFrom
    // persists them). axis is "move" or "look", matching the settings.json override prefix.
    input.set_function("GetAxis", [this](const std::string& axis) -> sol::object {
        const InputBindings& b = Input::Get().Bindings();
        const Axis2DBinding* a = (axis == "look") ? &b.look
                               : (axis == "move") ? &b.move : nullptr;
        if (!a) return sol::lua_nil;
        return sol::make_object(*m_lua, m_lua->create_table_with(
            "posX", static_cast<int>(a->vkPosX), "negX", static_cast<int>(a->vkNegX),
            "posY", static_cast<int>(a->vkPosY), "negY", static_cast<int>(a->vkNegY),
            "stick", static_cast<int>(a->stick)));
    });

    // Absent keys keep their current value, same as SetBinding / SetTuning.
    // All-or-nothing on range violations for the same reason: a half-applied
    // movement rebind is a character that walks in three directions.
    input.set_function("SetAxis", [](const std::string& axis, sol::table t) -> bool {
        InputBindings b = Input::Get().Bindings();
        Axis2DBinding* a = (axis == "look") ? &b.look
                         : (axis == "move") ? &b.move : nullptr;
        if (!a) return false;

        auto readVk = [&t](const char* k, std::uint16_t cur, bool& ok) -> std::uint16_t {
            if (!t[k].valid()) return cur;
            const int v = t[k].get<int>();
            if (v < 0 || v > 255) { ok = false; return cur; }
            return static_cast<std::uint16_t>(v);
        };
        bool ok = true;
        const std::uint16_t px = readVk("posX", a->vkPosX, ok);
        const std::uint16_t nx = readVk("negX", a->vkNegX, ok);
        const std::uint16_t py = readVk("posY", a->vkPosY, ok);
        const std::uint16_t ny = readVk("negY", a->vkNegY, ok);
        std::uint8_t stick = a->stick;
        if (t["stick"].valid())
        {
            const int s = t["stick"].get<int>();
            if (s != 0 && s != 1) ok = false;
            else stick = static_cast<std::uint8_t>(s);
        }
        if (!ok) return false;

        a->vkPosX = px; a->vkNegX = nx; a->vkPosY = py; a->vkNegY = ny;
        a->stick  = stick;
        Input::Get().SetBindings(b);   // clamps the tuning scalars back into range
        return true;
    });

    // ---- Rebind capture ("press any key") ----
    // The engine owns capture because a Lua key scan cannot do what it does: while a
    // capture is pending BuildCommand zeroes ALL action output, so the capturing press
    // cannot also fire gameplay; mouse buttons come from a raw pre-quarantine snapshot,
    // so a click is bindable whoever owns the pointer; and every control held when
    // BeginCapture ran is ignored until released, so the click or Enter that opened the
    // cell cannot bind itself. Escape is never capturable: it is the cancel key.
    // There is NO engine-side timeout: while a capture is pending every action and the
    // move axes read zero, so a screen that forgets to cancel looks like "input stopped
    // working". The caller owns the deadline (the editor panel and the Lua widget kit arm
    // 5 s). Always CancelCapture on close.
    input.set_function("BeginCapture", []() { Input::Get().BeginCapture(); });
    input.set_function("CancelCapture", []() { Input::Get().CancelCapture(); });
    input.set_function("CapturePending", []() -> bool {
        return Input::Get().CapturePending();
    });

    // Input.TakeCapture() -> nil                          (nothing captured yet)
    //                     -> { isPad=, vk=, pad=, path= }  (captured; ends capture)
    // `path` is the canonical FormatBindingPath spelling ("kbm/space", "pad/a"): the
    // string settings.json stores and Glyphs.lua is keyed by.
    input.set_function("TakeCapture", [this]() -> sol::object {
        bool isPad = false;
        std::uint16_t vk = 0, pad = 0;
        if (!Input::Get().TakeCapture(isPad, vk, pad)) return sol::lua_nil;
        char buf[64] = {};
        FormatBindingPath(isPad, isPad ? pad : vk, buf, static_cast<int>(sizeof(buf)));
        return sol::make_object(*m_lua, m_lua->create_table_with(
            "isPad", isPad,
            "vk",    static_cast<int>(vk),
            "pad",   static_cast<int>(pad),
            "path",  std::string(buf)));
    });

    // Input.BindingPath(isPad, code) -> "kbm/space" | "pad/a" | nil
    // The per-slot companion to Input.GlyphToken (which resolves one token for the active
    // device): a rebind table renders Key 1 / Key 2 / Gamepad from Input.GetBinding's raw
    // integers. nil for an unbound slot (code 0), so a UI prints its own placeholder.
    // These are the FROZEN settings.json spellings, not display names: map them through a
    // table (asset/scripts/config/Glyphs.lua) for display, and never rename them at the
    // source, or settings.json on disk stops round-tripping.
    input.set_function("BindingPath", [](bool isPad, int code) -> sol::optional<std::string> {
        if (code <= 0) return sol::nullopt;
        char buf[64] = {};
        if (!FormatBindingPath(isPad, static_cast<std::uint16_t>(code),
                               buf, static_cast<int>(sizeof(buf))))
            return sol::nullopt;
        return std::string(buf);
    });

    // ---- Mapping contexts ----
    // The "B cancels in a menu but dodges in combat" mechanism. A context is a
    // priority-stacked LAYER of bindings: define it once (asset/scripts/config/
    // InputContexts.lua at boot, or any script), push it when the owning mode opens, pop
    // it when it closes. Resolution per action, highest priority first:
    //   the context DEFINES the action -> its binding wins, stop
    //   else if the context blockLower -> the action is UNBOUND, stop
    //   else                           -> keep walking; nothing = base bindings
    // Key present vs absent is the distinction (InputContext::definedMask):
    //   actions = { Dodge = {} }  -- Dodge DEFINED with no control: unbound while active
    //   actions = { }             -- Dodge not this context's business: falls through
    // Collapse the two and blockLower is the only tool left, forcing a selective menu to
    // silence ALL gameplay to mute one button.
    input.set_function("DefineContext", [](sol::table t) -> bool
    {
        InputContext c {};

        // ---- name: the identity Push/Pop/IsContextActive take ----
        sol::object nameObj = t["name"];
        if (nameObj.get_type() != sol::type::string)
        {
            if (ShouldWarnInputOnce("<context-noname>"))
                LOG_WARNING("Lua: Input.DefineContext needs a string `name` — the "
                            "context was not registered. It is the handle "
                            "Input.PushContext(\"Menu\") looks up.");
            return false;
        }
        const std::string name = nameObj.as<std::string>();
        if (name.empty() || name.size() >= static_cast<std::size_t>(InputContext::kNameCap))
        {
            // Truncating instead would register a context under a name no script
            // spells, so every later Push/Pop would silently miss.
            if (ShouldWarnInputOnce("<context-namelen>"))
                LOG_WARNING("Lua: input context name \"%s\" must be 1..%d characters "
                            "— the context was not registered.",
                            name.c_str(), InputContext::kNameCap - 1);
            return false;
        }
        std::memcpy(c.name, name.c_str(), name.size());   // c.name is zero-filled

        c.priority   = t.get_or("priority", 0);
        c.blockLower = t.get_or("blockLower", false);

        // ---- actions: SPARSE. Keys may be names ("Dodge") or Action.* ints ----
        sol::object actionsObj = t["actions"];
        if (actionsObj.get_type() == sol::type::table)
        {
            for (const auto& kv : actionsObj.as<sol::table>())
            {
                GameAction a = GameAction::Count;   // sentinel: never read unless resolved
                // A typo'd row is DROPPED and warned (ResolveGameAction reports
                // it once) but the rest of the context still installs: losing a
                // whole menu layer to one misspelling would be a worse trade.
                if (!ResolveGameAction(kv.first, a)) continue;

                if (kv.second.get_type() != sol::type::table)
                {
                    if (ShouldWarnInputOnce("<context-action-value>"))
                        LOG_WARNING("Lua: input context action rows must be tables — "
                                    "{ pad = Pad.B } to bind, {} to explicitly unbind. "
                                    "Got %s; the row was skipped.",
                                    LuaTypeLabel(kv.second.get_type()));
                    continue;
                }

                // Absent field = 0 = unbound IN THIS CONTEXT. Deliberately not
                // "inherit the base binding": a context row states the control
                // it wants in full, and inheriting would make `{ pad = Pad.B }`
                // silently keep whatever key the base table had.
                sol::table    bt = kv.second.as<sol::table>();
                ActionBinding ab {};
                // An out-of-range NUMBER is a different failure from a typo'd
                // name: static_cast would wrap it into a plausible-looking
                // control. Reject the whole context so nothing half-parsed goes
                // live, same all-or-nothing rule as Input.SetBinding.
                if (!ResolveVk(bt.get_or("vk1", 0), ab.vk[0]))     return false;
                if (!ResolveVk(bt.get_or("vk2", 0), ab.vk[1]))     return false;
                if (!ResolvePadMask(bt.get_or("pad", 0), ab.pad))  return false;

                c.Define(a, ab);   // sets definedMask even for an EMPTY binding
            }
        }
        else if (actionsObj.valid() && actionsObj.get_type() != sol::type::lua_nil)
        {
            if (ShouldWarnInputOnce("<context-actions-type>"))
                LOG_WARNING("Lua: Input.DefineContext `actions` must be a table, got "
                            "%s — the context was not registered.",
                            LuaTypeLabel(actionsObj.get_type()));
            return false;
        }

        // ---- composites: optional, all-or-nothing ----
        sol::object moveObj = t["move"];
        if (moveObj.get_type() == sol::type::table)
        {
            if (!ResolveAxis2D(moveObj.as<sol::table>(), c.move)) return false;
            c.definesMove = true;
        }
        sol::object lookObj = t["look"];
        if (lookObj.get_type() == sol::type::table)
        {
            if (!ResolveAxis2D(lookObj.as<sol::table>(), c.look)) return false;
            c.definesLook = true;
        }

        if (!Input::Get().DefineContext(c))
        {
            // Only failure left is a full definition table (Input::kMaxContexts).
            if (ShouldWarnInputOnce("<context-table-full>"))
                LOG_WARNING("Lua: input context \"%s\" was NOT registered — the "
                            "context table is full (max %d). Every later "
                            "PushContext on it will fail.",
                            name.c_str(), Input::kMaxContexts);
            return false;
        }
        return true;
    });

    // Push / Pop are idempotent-safe: pushing an active context re-seats it rather than
    // stacking a duplicate; popping an inactive one is a no-op false. Push warns once per
    // NAME: a false is nearly always a name never defined (InputContexts.lua failed to
    // load, or a typo), and a menu whose push did nothing looks like "the pad is broken".
    // A push is popped again when the script that made it ends (its scene, System or entity), so
    // a scene unloaded mid-menu cannot leave the gameplay actions unbound.
    input.set_function("PushContext", [this](const std::string& name) -> bool
    {
        if (Input::Get().PushContext(name.c_str()))
        {
            if (m_currentOwner != 0) m_contextOwners[name] = m_currentOwner;
            else                     m_contextOwners.erase(name);
            return true;
        }
        if (ShouldWarnInputOnce("PushContext:" + name))
            LOG_WARNING("Lua: Input.PushContext(\"%s\") failed — no context by that "
                        "name is defined, or the active stack is full (max %d). "
                        "Bindings are unchanged.",
                        name.c_str(), Input::kMaxContexts);
        return false;
    });
    input.set_function("PopContext", [this](const std::string& name) -> bool
    {
        m_contextOwners.erase(name);
        return Input::Get().PopContext(name.c_str());   // false = wasn't active
    });
    // Deactivate everything, keeping the DEFINITIONS. The panic button for a UI
    // script that lost track of its own pushes; also run on world teardown from
    // C++ (see ClearAll / ClearInstances).
    input.set_function("ClearContexts", [this]()
    {
        m_contextOwners.clear();
        Input::Get().ClearContexts();
    });
    input.set_function("IsContextActive", [](const std::string& name) -> bool
    {
        return Input::Get().IsContextActive(name.c_str());
    });

    // Array of { name, priority }, HIGHEST priority first: index 1 owns the pad now. For
    // a debug overlay, or a UI script checking that nothing modal is up before pushing.
    input.set_function("ActiveContexts", [this]() -> sol::table
    {
        sol::table  arr = m_lua->create_table();
        const Input& in = Input::Get();
        const int    n  = in.ActiveContextCount();
        for (int i = 0; i < n; ++i)
        {
            const char* nm = in.ActiveContextName(i);
            sol::table  e  = m_lua->create_table();
            e["name"]     = (nm && *nm) ? nm : "";
            e["priority"] = in.ActiveContextPriority(i);
            arr[i + 1] = e;                            // 1-based, Lua array part
        }
        return arr;
    });

    lua["Input"] = input;

    lua["Mouse"] = lua.create_table_with("Left", 0, "Right", 1, "Middle", 2);

    auto keys = lua.create_table();
    keys["W"] = 0x57; keys["A"] = 0x41; keys["S"] = 0x53; keys["D"] = 0x44;
    keys["Q"] = 0x51; keys["E"] = 0x45;
    keys["Up"] = VK_UP; keys["Down"] = VK_DOWN;
    keys["Left"] = VK_LEFT; keys["Right"] = VK_RIGHT;
    keys["Space"] = VK_SPACE; keys["Shift"] = VK_SHIFT;
    keys["Ctrl"] = VK_CONTROL;
    lua["Keys"] = keys;

    // The full Windows VK map authored in Lua (asset/scripts/config/Keys.lua) MERGES onto
    // the built-in subset above (Keys.Enter, Keys.Escape, Keys.F1, Keys.A..Z,
    // Keys.Num0..9, ...). Read pak-first via AssetFS, so the editor and a packed build
    // agree; a missing file leaves the built-in subset. Editing keys needs no C++ rebuild.
    {
        auto keysResult = RunScriptAsset(lua, "asset/scripts/config/Keys.lua");
        if (!keysResult.valid())
        {
            sol::error err = keysResult;
            LOG_WARNING("ScriptSystem: Keys.lua not loaded (%s) — built-in key subset only",
                        err.what());
        }
    }

    // ---- Pad: physical gamepad button names (values = PadBtn::Mask) ----
    // Our bit vocabulary, not XInput's: a GameInput / DualSense / Raw-HID backend maps
    // onto the same bits, so a `Pad.A` in a script or settings.json survives a backend
    // swap.
    auto pad = lua.create_table();
    pad["A"]      = static_cast<int>(PadBtn::A);
    pad["B"]      = static_cast<int>(PadBtn::B);
    pad["X"]      = static_cast<int>(PadBtn::X);
    pad["Y"]      = static_cast<int>(PadBtn::Y);
    pad["LB"]     = static_cast<int>(PadBtn::LB);
    pad["RB"]     = static_cast<int>(PadBtn::RB);
    pad["Back"]   = static_cast<int>(PadBtn::Back);
    pad["Start"]  = static_cast<int>(PadBtn::Start);
    pad["LS"]     = static_cast<int>(PadBtn::LS);
    pad["RS"]     = static_cast<int>(PadBtn::RS);
    pad["DUp"]    = static_cast<int>(PadBtn::DUp);
    pad["DDown"]  = static_cast<int>(PadBtn::DDown);
    pad["DLeft"]  = static_cast<int>(PadBtn::DLeft);
    pad["DRight"] = static_cast<int>(PadBtn::DRight);
    pad["LT"]     = static_cast<int>(PadBtn::LT);   // synthesized from the analog trigger
    pad["RT"]     = static_cast<int>(PadBtn::RT);
    lua["Pad"] = pad;

    // ---- Action: the device-agnostic action ids (values = the enum int) ----
    // Generated from ToString(GameAction), so a new InputTypes.h enumerator reaches Lua
    // with no edit here. Not authored in a .lua: the C++ enum is authoritative, and a
    // stale Action.lua would hand scripts ids that mean something else.
    auto actions = lua.create_table();
    for (int i = 0; i < kGameActionCount; ++i)
    {
        // Guard the null: sol would happily construct a Lua string from a null
        // pointer and take the process with it if a ToString row is ever missed.
        if (const char* name = ToString(static_cast<GameAction>(i)); name && *name)
            actions[name] = i;
    }

    // Typo guard. `Action.Jmup` is an ordinary table miss: nil reaches Input.Pressed()
    // and the query returns false forever. __index fires only on a miss (every real
    // action is a raw key), so it costs nothing on the happy path and names the bad key
    // at the lookup. Warn, do NOT error(): an error inside OnUpdate kills that script for
    // the session. The lookup still yields nil, so `if Action.Foo` probes behave like a
    // plain table.
    {
        sol::table actionMeta = lua.create_table();
        actionMeta.set_function("__index",
            [](sol::table, sol::object key) -> sol::optional<int>
            {
                if (key.get_type() == sol::type::string)
                {
                    const std::string k = key.as<std::string>();
                    if (ShouldWarnInputOnce("Action." + k))
                        LOG_WARNING("Lua: Action.%s does not exist — the lookup is "
                                    "nil and any Input query on it returns false. "
                                    "Valid names come from ToString(GameAction) "
                                    "(Action.Jump, Action.Sprint, ...).",
                                    k.c_str());
                }
                else if (ShouldWarnInputOnce("<action-nonstring-key>"))
                {
                    LOG_WARNING("Lua: the Action table was indexed with %s — it is "
                                "keyed by name (Action.Jump).",
                                LuaTypeLabel(key.get_type()));
                }
                return sol::nullopt;   // nil, exactly as a plain table would
            });
        actions[sol::metatable_key] = actionMeta;
    }

    lua["Action"] = actions;

    // Optional Pad name ALIASES authored in Lua (Pad.Cross / Pad.L1 / Pad.Menu for a
    // PlayStation- or Nintendo-facing project), merged onto the built-in table like
    // Keys.lua; a missing file leaves the engine set. The engine owns the primaries: a
    // new BUTTON is a PadBtn::Mask edit, not a Lua one.
    {
        auto padResult = RunScriptAsset(lua, "asset/scripts/config/Pad.lua");
        if (!padResult.valid())
        {
            sol::error err = padResult;
            LOG_WARNING("ScriptSystem: Pad.lua not loaded (%s) — built-in pad button set only",
                        err.what());
        }
    }

    // ---- Optional mapping contexts authored in Lua ----
    // Runs LAST of the three config scripts: it calls Input.DefineContext and names
    // Keys.* / Pad.* / Action.*, so those tables must exist first. Non-fatal like Keys.lua
    // and Pad.lua: without the file the base InputBindings keep working. Defining a
    // context does nothing until a script calls Input.PushContext(name).
    {
        auto ctxResult = RunScriptAsset(lua, "asset/scripts/config/InputContexts.lua");
        if (!ctxResult.valid())
        {
            sol::error err = ctxResult;
            LOG_WARNING("ScriptSystem: InputContexts.lua not loaded (%s) — no mapping "
                        "contexts defined; Input.PushContext will fail until a script "
                        "defines one", err.what());
        }
    }

    // ---- Log ----
    // Each takes the raw lua_State so the line is attributed to the CALLING .lua
    // file:line, not to this file.
    auto log = lua.create_table();
    LuaDoc::Fn(log, "Log", "Info", [](sol::this_state ts, const std::string& msg) {
        LuaLog(LogLevel::Info, ts.lua_state(), msg);
    },
        { .doc = "Log one INFO line attributed to the calling script.", .params = { { .name = "msg" } } });
    LuaDoc::Fn(log, "Log", "Warning", [](sol::this_state ts, const std::string& msg) {
        LuaLog(LogLevel::Warning, ts.lua_state(), msg);
    },
        { .doc = "Log one WARNING line attributed to the calling script (counted by -maxwarnings style checks).", .params = { { .name = "msg" } } });
    log.set_function("Error", [](sol::this_state ts, const std::string& msg) {
        LuaLog(LogLevel::Error, ts.lua_state(), msg);
    });
    lua["Log"] = log;

    // print() -> the log (see LuaPrintToLog). -lua_print_legacy keeps the base
    // library's stdout print: the reverse arm of the lua_print_log pack.
#ifdef WITH_HARNESS
    const bool legacyPrint = Sys::CommandLine::Get().Has("-lua_print_legacy");
#else
    const bool legacyPrint = false;
#endif
    if (!legacyPrint)
        lua_register(lua.lua_state(), "print", &LuaPrintToLog);

#ifdef WITH_HARNESS
    // ---- Test.*: scenario assertions (harness) ----
    // Compiled out of Retail (Engine.Cpp.props), on top of the runtime Sys::HasArtifactDir()
    // gate below: Test.Done() ends the process with a caller-chosen exit code. Registered
    // here, not in App.cpp, because LuaCallSite (this file's anonymous namespace) gives
    // the "driver.lua:42" attribution these exist for.
    // None of them raise a Lua error, on purpose: raising would unwind OnUpdate, be
    // counted again as a script fault by the traceback handler, and set hasUpdate = false,
    // which parks the driver so later assertions never run. Record and return.
    {
        auto test = lua.create_table();

        LuaDoc::Fn(test, "Test", "Expect",
            [](sol::this_state ts, bool cond, sol::optional<std::string> msg) -> bool
            {
                const std::string site = LuaCallSite(ts.lua_state());
                const std::string text = msg.value_or(std::string("expectation"));
                Harness::RunLedger::Get().NoteExpectation(cond, site.c_str(), text.c_str());
                if (!cond)
                    LOG_ERROR("Test.Expect FAILED [%s]: %s", site.c_str(), text.c_str());
                return cond;   // returned so a driver can branch without re-testing
            },
                { .doc = "Record an expectation: a false cond fails the run (exit 10; run.json names this line) and logs msg.", .params = { { .name = "cond" }, { .name = "msg", .doc = "what was expected" } }, .returnsDoc = "cond", .context = LuaDoc::Context::Harness });

        test.set_function("Fail",
            [](sol::this_state ts, sol::optional<std::string> msg)
            {
                const std::string site = LuaCallSite(ts.lua_state());
                const std::string text = msg.value_or(std::string("Test.Fail"));
                Harness::RunLedger::Get().NoteExpectation(false, site.c_str(), text.c_str());
                LOG_ERROR("Test.Fail [%s]: %s", site.c_str(), text.c_str());
            });

        test.set_function("Pass",
            [](sol::this_state ts, sol::optional<std::string> msg)
            {
                const std::string site = LuaCallSite(ts.lua_state());
                Harness::RunLedger::Get().NoteExpectation(
                    true, site.c_str(), msg.value_or(std::string("ok")).c_str());
            });

        // Test.Done([code]): the scenario ran to completion. Unlike Engine.Quit the default
        // code is Ok, and the verdict comes from the expectations recorded so far.
        LuaDoc::Fn(test, "Test", "Done",
            [](sol::this_state ts, sol::optional<int> code)
            {
                const std::string site = LuaCallSite(ts.lua_state());
                LOG_INFO("Test.Done [%s]: scenario finished (%u passed, %u failed)",
                         site.c_str(),
                         Harness::RunLedger::Get().PassCount(),
                         Harness::RunLedger::Get().FailCount());
                PostQuitMessage(code.value_or(static_cast<int>(Harness::ExitCode::Ok)));
            },
                { .doc = "The scenario ran to completion: quit with code (default 0). The verdict is decided by the expectations recorded so far.", .params = { { .name = "code" } }, .effect = LuaDoc::Effect::Process, .context = LuaDoc::Context::Harness });

        // Test.ThrowFromTask([msg]) - a queued task that throws: the main loop rethrows it
        // (TaskSystem::RethrowPendingTaskException), App::Run ends with it, and the process leaves
        // the way an escaped exception leaves (main's catch: exit 1) - without the orderly
        // shutdown, so whatever a feature left to static destruction runs then
        // (tests/scenarios/face_track_exit).
        LuaDoc::Fn(test, "Test", "ThrowFromTask",
            [](sol::this_state ts, sol::optional<std::string> msg)
            {
                const std::string text = msg.value_or(std::string("Test.ThrowFromTask"));
                LOG_INFO("[TEST] Test.ThrowFromTask [%s]: %s", LuaCallSite(ts.lua_state()).c_str(), text.c_str());
                TaskSystem::Get().Push([text] { throw std::runtime_error(text); });
            },
                { .doc = "Queue a task that throws msg: the main loop rethrows it and the run ends the way an escaped exception ends it (exit 1, no orderly shutdown). For tests of that exit.", .params = { { .name = "msg" } }, .effect = LuaDoc::Effect::Process, .context = LuaDoc::Context::Harness });

        // Read-back so a driver can gate on its own progress.
        test.set_function("Passed", [] { return Harness::RunLedger::Get().PassCount(); });
        test.set_function("Failed", [] { return Harness::RunLedger::Get().FailCount(); });

        // Test.Stall(ms) - block the calling thread for ms (at most 20000): a
        // slow frame on demand. Scripts and pipe `run` chunks run on the main
        // thread, so this is how a test makes the frame loop late - for what
        // waits on it, like the control channel's service deadline
        // (tests/unit/test_workflow_recovery.py).
        test.set_function("Stall", [](sol::this_state ts, int ms)
        {
            ms = std::clamp(ms, 0, 20000);
            LOG_INFO("[TEST] Test.Stall(%d) [%s]", ms, LuaCallSite(ts.lua_state()).c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        });

        // ---- The synthetic input device (Input::SynthHold) ----
        // Test.Hold(key [, frames = 1]) holds a key for the next `frames` input updates (-1:
        // until Test.Release) - through the bindings, contexts and edges exactly like a
        // physical press (a game-action test, not OS input). `key`: a VK number or a Keys name
        // ("W", "Space"). Returns the VK. An unknown key raises: a script bug.
        auto resolveKey = [](sol::this_state ts, const sol::object& key) -> int
        {
            if (key.is<double>()) return static_cast<int>(key.as<double>());
            if (key.is<std::string>())
            {
                sol::state_view L(ts);
                const sol::object k = L["Keys"][key.as<std::string>()];
                if (k.is<double>()) return static_cast<int>(k.as<double>());
            }
            return 0;
        };
        test.set_function("Hold", [resolveKey](sol::this_state ts, sol::object key, sol::optional<int> frames) -> int
        {
            const int vk = resolveKey(ts, key);
            if (!Input::Get().SynthHold(vk, frames.value_or(1)))
                throw sol::error("Test.Hold: unknown key (a VK 1..255 or a Keys name)");
            return vk;
        });
        test.set_function("Release", [resolveKey](sol::this_state ts, sol::object key) -> int
        {
            const int vk = resolveKey(ts, key);
            if (!Input::Get().SynthHold(vk, 0))
                throw sol::error("Test.Release: unknown key (a VK 1..255 or a Keys name)");
            return vk;
        });
        test.set_function("ReleaseAll", [] { Input::Get().SynthReleaseAll(); });
        // Test.Pad{ buttons = "A" | {"A", "LB"} | mask, lx, ly, rx, ry, lt, rt,
        //           frames = 1 } - the synthetic PAD (Input::SynthPad): COOKED
        // values, sticks [-1, 1] with +y up, triggers [0, 1] (clamped), for the
        // next `frames` input updates (-1: until Test.PadRelease). Names are the
        // binding vocabulary, case-insensitive (a b x y lb rb lt rt back start
        // ls rs dpadUp ...). An unknown name raises.
        test.set_function("Pad", [](sol::table t)
        {
            GamepadState s{};
            auto button = [](const sol::object& o) -> std::uint16_t
            {
                if (o.is<double>()) return static_cast<std::uint16_t>(o.as<double>());
                if (o.is<std::string>())
                {
                    const std::string n = o.as<std::string>();
                    const std::uint16_t m = ParsePadButton(n.c_str());
                    if (!m) throw sol::error("Test.Pad: unknown button '" + n + "'");
                    return m;
                }
                throw sol::error("Test.Pad: a button is a name or a PadBtn mask");
            };
            const sol::object b = t["buttons"];
            if (b.is<sol::table>())
            {
                for (const auto& kv : b.as<sol::table>())
                    s.buttons = static_cast<std::uint16_t>(s.buttons | button(kv.second));
            }
            else if (b.valid() && b != sol::lua_nil)
                s.buttons = button(b);
            auto axis = [&](const char* k, float lo) { return std::clamp(t.get_or(k, 0.f), lo, 1.f); };
            s.lx = axis("lx", -1.f); s.ly = axis("ly", -1.f);
            s.rx = axis("rx", -1.f); s.ry = axis("ry", -1.f);
            s.lt = axis("lt", 0.f);  s.rt = axis("rt", 0.f);
            Input::Get().SynthPad(s, t.get_or("frames", 1));
        });
        test.set_function("PadRelease", [] { Input::Get().SynthPad(GamepadState{}, 0); });
        // Test.Look{ dx, dy, frames = 1, held = true } - the synthetic MOUSE
        // LOOK (Input::SynthLook): dx, dy PIXELS (screen +y down) added to the
        // mouse's displacement at each of the next `frames` input updates (-1:
        // until Test.LookRelease), the look gesture (RMB) held unless held =
        // false. It turns a camera the way a drag does: Cmd().lookMouseDX x
        // CameraController.mouseSensitivity radians per update.
        test.set_function("Look", [](sol::table t)
        {
            Input::Get().SynthLook(t.get_or("dx", 0.f), t.get_or("dy", 0.f), t.get_or("frames", 1),
                                   t.get_or("held", true));
        });
        test.set_function("LookRelease", [] { Input::Get().SynthLook(0.f, 0.f, 0); });
        // {held, pad, look, updates, recording, replaying, replayEnd}
        test.set_function("InputStatus", [](sol::this_state ts) -> sol::table
        {
            const Input::SynthStatus s = Input::Get().Synth();
            sol::table t = sol::state_view(ts).create_table();
            t["held"]      = s.held;
            t["pad"]       = s.pad;
            t["look"]      = s.look;
            t["updates"]   = s.updates;
            t["recording"] = s.recording;
            t["replaying"] = s.replaying;
            t["replayEnd"] = s.replayEnd;
            return t;
        });

        // Test.Window / WindowState / InputFocus / TypedText (LuaTestWindowBindings.cpp).
        Scripting::RegisterLuaTestWindow(test, resolveKey);

        // Registered ONLY for a harness run (-artifacts=): Test.Done() calls PostQuitMessage
        // with a caller-chosen exit code, a script-reachable way to end the game, and without
        // the table a shipped script calling it gets a clean nil-index error.
        if (Sys::HasArtifactDir())
            lua["Test"] = test;
    }
#endif  // WITH_HARNESS

    // ---- Time (updated each frame in Update) ----
    lua["Time"] = lua.create_table_with("dt", 0.f, "elapsed", 0.f);

    // ---- Engine.* Command API ----
    auto engine = lua.create_table();

    // ---- Equipment / Follow Commands ----
    engine.set_function("EquipToSocket",
        [this](LuaEntity charId, LuaEntity itemId, const std::string& socketName) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.EquipToSocket called outside Update"); return false; }
            const Entity c = LuaId::ResolveLive(*m_world, charId);
            const Entity i = LuaId::ResolveLive(*m_world, itemId);
            if (c == NullEntity || i == NullEntity) return false;
            return Command::EquipToSocket(*m_world, c, i, socketName.c_str());
        });

    engine.set_function("UnequipItem",
        [this](LuaEntity itemId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.UnequipItem called outside Update"); return false; }
            const Entity i = LuaId::ResolveLive(*m_world, itemId);
            if (i == NullEntity) return false;
            return Command::UnequipItem(*m_world, i);
        });

    engine.set_function("AttachToEntity",
        [this](LuaEntity followerId, LuaEntity targetId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.AttachToEntity called outside Update"); return false; }
            const Entity f = LuaId::ResolveLive(*m_world, followerId);
            const Entity t = LuaId::ResolveLive(*m_world, targetId);
            if (f == NullEntity || t == NullEntity) return false;
            return Command::AttachToEntity(*m_world, f, t);
        });

    engine.set_function("DetachFromEntity",
        [this](LuaEntity followerId) -> bool
        {
            if (!m_world) { LOG_WARNING("Engine.DetachFromEntity called outside Update"); return false; }
            const Entity f = LuaId::ResolveLive(*m_world, followerId);
            if (f == NullEntity) return false;
            return Command::DetachFromEntity(*m_world, f);
        });

    // ---- Entity lifecycle ----
    engine.set_function("CreateEntity",
        [this]() -> LuaEntity
        {
            if (!m_world) { LOG_WARNING("Engine.CreateEntity called outside Update"); return {}; }
            return LuaId::Out(*m_world, m_world->CreateEntity());
        });

    // Engine.CreateEntityAt(x, y, z) -> LuaEntity
    // Engine.CreateEntity() gives an entity with no LocalTransform / GlobalTransform, so
    // everything positional silently does nothing (never drawn, GetWorldPosition returns
    // nil, a hitbox or follower sits at the origin). This one is positioned and usable;
    // it still has no mesh (add one with SpawnPrefab or a MeshRef).
    LuaDoc::Fn(engine, "Engine", "CreateEntityAt",
        [this](float x, float y, float z) -> LuaEntity
        {
            if (!m_world) { LOG_WARNING("Engine.CreateEntityAt called outside Update"); return {}; }
            const Entity e = m_world->CreateEntity();
            if (e == NullEntity) return {};
            LocalTransform lt;
            lt.translation = { x, y, z };
            m_world->AddComponent<LocalTransform>(e, lt);
            m_world->AddComponent<GlobalTransform>(e, {});
            return LuaId::Out(*m_world, e);
        },
            { .doc = "A new entity with LocalTransform and GlobalTransform at (x, y, z) - unlike Engine.CreateEntity, whose entity has no components and is never drawn.", .params = { { .name = "x" }, { .name = "y" }, { .name = "z" } }, .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });

    // Engine.GetWorldEpoch() -> integer
    // Bumped whenever the world's state is replaced wholesale: World::Clear and
    // World::AdoptState (the editor's Play / Stop restore). "Is this entity alive" is
    // not enough after a restore: AdoptState rewinds the id allocator (free list, alive
    // bits, generations, nextEntity), so the next session re-issues the same ids at the
    // same generations and a stale handle passes the generation check. Stamp the epoch
    // when you cache a handle; compare before you trust it.
    engine.set_function("GetWorldEpoch",
        [this]() -> uint32_t { return m_world ? m_world->Epoch() : 0u; });

    LuaDoc::Fn(engine, "Engine", "DestroyEntity",
        [this](LuaEntity id)
        {
            if (!m_world) { LOG_WARNING("Engine.DestroyEntity called outside Update"); return; }
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return;
            // Inside a deferral window (physics contact dispatch, BT tick) a synchronous destroy
            // is a use-after-free in the caller's stack: queue it for the next script phase.
            // Only the ROOT is queued; the subtree is re-derived at flush time, since a reparent
            // before the flush would make a set captured now wrong.
            if (m_deferDestroyDepth > 0)
            {
                const uint32_t gen = m_world->GetGeneration(e) & LuaId::kGenMask;
                for (const PendingDestroy& p : m_pendingDestroys)
                    if (p.e == e && p.gen == gen) return;   // already queued
                m_pendingDestroys.push_back({ e, gen });
                return;
            }
            // Destroy the SUBTREE: destroying only `e` would orphan every child - alive, drawn,
            // unreachable from script, with no destroy listener fired and its GPU slots leaked
            // until the next world reload.
            Hierarchy::DestroySubtree(*m_world, e);
        },
            { .doc = "Destroy one entity - flat: its children stay. During a contact dispatch the destroy is queued until the next update.", .params = { { .name = "id" } }, .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });

    LuaDoc::Fn(engine, "Engine", "IsAlive",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            return LuaId::IsLive(*m_world, id);
        },
            { .doc = "Whether the id names a live entity (its generation still matches).", .params = { { .name = "id" } } });

    engine.set_function("SetName",
        [this](LuaEntity id, const std::string& name)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return;
            m_world->SetName(e, name);
        });

    LuaDoc::Fn(engine, "Engine", "GetName",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return {};
            return m_world->GetName(LuaId::ResolveLive(*m_world, id));
        },
            { .doc = "The entity's name.", .params = { { .name = "id" } }, .context = LuaDoc::Context::Update });

    // Engine.AttachScript(id, path [, vars]) -> bool
    // `vars` is an optional table of exposed-variable overrides, the bag the Inspector
    // edits and the .iscene writes as `var.<name>=<t>:<v>`, so a batch tool can say which
    // thing each attached instance is about.
    // Each override is coerced to the type the SCRIPT declares in its `exposed` table,
    // not the Lua value's shape: a var bag is reconciled by name and a type-mismatched
    // entry is dropped for the schema default (ScriptSystem's InjectExposedVars), and Lua
    // has one number type, so guessing "integral means Int" would turn `lightOn = 7.0`
    // into an Int override of a Float var that is then discarded.
    // A name not in the schema is stored in the Lua value's shape: harmless, and a typo
    // stays visible in the Inspector.
    engine.set_function("AttachScript",
        [this](LuaEntity id, const std::string& path, sol::object varsObj) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;

            // Append a slot; never clobber scripts already attached. The next
            // ScriptSystem::Update picks it up (template loaded, OnSpawn fired) when the reconcile
            // loop sees the longer ScriptComponent::scripts vector.
            ScriptComponent* sc = m_world->GetComponent<ScriptComponent>(e);
            if (!sc)
            {
                m_world->AddComponent<ScriptComponent>(e, {});
                sc = m_world->GetComponent<ScriptComponent>(e);
            }
            if (!sc) return false;

            ScriptInstance si;
            si.scriptPath = path;
            si.enabled    = true;

            if (varsObj.is<sol::table>())
            {
                // Lazy-loads the template if needed; returns an empty vector for
                // a script with no `exposed` table.
                const std::vector<ScriptVarDesc>& schema = GetExposedSchema(path);
                auto declaredType = [&schema](const std::string& n,
                                              ScriptVarType& out) -> bool
                {
                    for (const ScriptVarDesc& d : schema)
                        if (d.name == n) { out = d.type; return true; }
                    return false;
                };

                for (const auto& [k, v] : varsObj.as<sol::table>())
                {
                    if (!k.is<std::string>()) continue;
                    const std::string name = k.as<std::string>();

                    // Read the Lua value once, in its own shape.
                    const bool  isBool = v.is<bool>();
                    const bool  isStr  = v.is<std::string>();
                    const bool  isNum  = !isBool && !isStr && v.is<double>();
                    double      num    = isNum ? v.as<double>() : 0.0;
                    std::string str    = isStr ? v.as<std::string>() : std::string{};
                    float       vx = 0.f, vy = 0.f, vz = 0.f;
                    bool        isVec = false;
                    if (v.is<LuaVec3>())
                    {
                        const LuaVec3 c = v.as<LuaVec3>();
                        vx = c.x; vy = c.y; vz = c.z; isVec = true;
                    }
                    else if (!isStr && v.is<sol::table>())
                    {
                        const sol::table t = v.as<sol::table>();
                        vx = t.get_or(1, 0.0f); vy = t.get_or(2, 0.0f); vz = t.get_or(3, 0.0f);
                        isVec = true;
                    }
                    if (isBool) num = v.as<bool>() ? 1.0 : 0.0;

                    ScriptVarType want;
                    if (!declaredType(name, want))
                    {
                        // Not in the schema: store it in the value's own shape.
                        if      (isBool) si.vars[name] = ScriptVarValue::MakeBool(v.as<bool>());
                        else if (isStr)  si.vars[name] = ScriptVarValue::MakeString(std::move(str));
                        else if (isNum)  si.vars[name] = ScriptVarValue::MakeFloat(static_cast<float>(num));
                        else if (isVec)  si.vars[name] = ScriptVarValue::MakeFloat3(vx, vy, vz);
                        continue;
                    }

                    switch (want)
                    {
                    case ScriptVarType::Float:
                        si.vars[name] = ScriptVarValue::MakeFloat(static_cast<float>(num));
                        break;
                    case ScriptVarType::Int:
                        si.vars[name] = ScriptVarValue::MakeInt(static_cast<int32_t>(num));
                        break;
                    case ScriptVarType::Bool:
                        si.vars[name] = ScriptVarValue::MakeBool(isStr ? !str.empty() : num != 0.0);
                        break;
                    case ScriptVarType::Float3:
                        si.vars[name] = ScriptVarValue::MakeFloat3(vx, vy, vz);
                        break;
                    case ScriptVarType::Color:
                        si.vars[name] = ScriptVarValue::MakeColor(vx, vy, vz);
                        break;
                    case ScriptVarType::String:
                        si.vars[name] = ScriptVarValue::MakeString(std::move(str));
                        break;
                    case ScriptVarType::Asset:
                        si.vars[name] = ScriptVarValue::MakeAsset(std::move(str));
                        break;
                    case ScriptVarType::Entity:
                    {
                        // An entity var persists by GUID; stamp one on the live
                        // target so the value survives a save/load round-trip
                        // the same way an Inspector drag does.
                        const Entity t = LuaId::ResolveLive(
                            *m_world, LuaEntity{ static_cast<Entity>(static_cast<uint64_t>(num) & 0xFFFFFFFFull),
                                                 static_cast<uint32_t>((static_cast<uint64_t>(num) >> 32) & LuaId::kGenMask) });
                        si.vars[name] = (t != NullEntity)
                            ? ScriptVarValue::MakeEntity(static_cast<uint32_t>(t),
                                                         ECS::EnsureGuidOn(*m_world, t).ToHex())
                            : ScriptVarValue::MakeEntity(0u);
                    } break;
                    }
                }
            }

            sc->scripts.push_back(std::move(si));
            return true;
        });

    // ---- Component access ----
    // Getters return a PROXY userdata (see LuaComponentRef), nil when the entity is
    // dead / stale or lacks the component at call time. Property access re-fetches every
    // time, so caching the returned value across frames is safe (it goes inert when the
    // entity dies).
    LuaDoc::Fn(engine, "Engine", "GetLocalTransform",
        [this](LuaEntity id) -> sol::optional<LuaTransformRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<LocalTransform>(e)) return sol::nullopt;
            return LuaTransformRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        },
            { .doc = "The entity's LocalTransform as a live reference (position, rotation, scale), or nil when it has none.", .params = { { .name = "id" } }, .returns = "LocalTransform?", .context = LuaDoc::Context::Update });

    LuaDoc::Fn(engine, "Engine", "GetLight",
        [this](LuaEntity id) -> sol::optional<LuaLightRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<LightData>(e)) return sol::nullopt;
            return LuaLightRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        },
            { .doc = "The entity's light as a live reference - its properties read and write LightData - or nil when it has none.", .params = { { .name = "id" } }, .returns = "Light?", .context = LuaDoc::Context::Update });

    // The .imat lives on the MESH entity: on a prefab root this returns nil. Find the
    // mesh child first (Engine.FindChildByName or Engine.FindMaterialDescendant).
    engine.set_function("GetMaterial",
        [this](LuaEntity id) -> sol::optional<LuaMaterialRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<MaterialComponent>(e)) return sol::nullopt;
            return LuaMaterialRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        });

    // ---- Blend mode ----
    // The integer is the BlendMode ordinal (Components.h): 0 Opaque, 1 Alpha, 2 Additive,
    // 3 Premultiplied, 4 Multiply, 5 Masked, 6 Dithered - the raw integer the .imat
    // `blendMode =` key stores.
    // A blend-mode change re-routes the draw (deferred GBuffer vs forward transparent, the
    // shadow bucket, the RT alpha record), so the setter goes through SetEditDirty()
    // exactly like the editor's Blend Mode combo, and a scenario exercises the inspector's
    // renderer path. Never write the field raw: the retained candidate cache replays the
    // old DrawFilter / permutation while it stays armed (PatchCandidateCache recomputes
    // matBits only; a dirty mark defeats the usePatch gate).
    // alphaRef is not touched: the Masked default clip value is resolved at publish
    // (MaterialComponent::MaskClipValue), never written into the material.
    engine.set_function("GetMaterialBlendMode",
        [this](LuaEntity id) -> sol::optional<int>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return sol::nullopt;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return sol::nullopt;
            return static_cast<int>(mc->userBlendMode);
        });
    // The clip threshold the GPU tests against: MaterialComponent::MaskClipValue(), the
    // one reconciliation point both publish sites use (WriteMaterialSlot for raster,
    // DDGISceneAS for the RT alpha record). Lets a scenario assert the contract: Masked
    // with an unset Alpha Ref clips at 0.5; every other mode publishes the authored value.
    engine.set_function("GetMaterialMaskClipValue",
        [this](LuaEntity id) -> sol::optional<float>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return sol::nullopt;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return sol::nullopt;
            return mc->MaskClipValue();
        });
    engine.set_function("SetMaterialBlendMode",
        [this](LuaEntity id, int mode) -> bool
        {
            if (!m_world) return false;
            if (mode < 0 || mode >= static_cast<int>(BlendMode::Count))
            {
                LOG_WARNING("Engine.SetMaterialBlendMode(%d): out of range (0..%d)",
                            mode, static_cast<int>(BlendMode::Count) - 1);
                return false;
            }
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;
            const BlendMode m = static_cast<BlendMode>(mode);
            if (mc->userBlendMode == m) return true;   // unchanged: no dirty, no re-gather
            mc->userBlendMode = m;
            mc->SetEditDirty();
            return true;
        });

    // ---- Custom-shader (b8 "//@") parameters ----
    // The float bag a custom material shader's `cbuffer Foo : register(b8, space0)`
    // members are packed from, keyed by the variable NAME in the HLSL. Always 4 wide (a
    // scalar is [0], a float2 [0..1], ...); pass as many components as the shader
    // declares, the rest default to 0. Meaningful only with Use Custom Shader + a shader
    // path; a name the shader does not declare does nothing (the packer walks DXC
    // reflection, not this map).
    // SetDirty() is REQUIRED even though HashMatParams folds every customParam into the
    // binding key: that fold runs only in a FULL candidate gather. A retained-cache patch
    // frame (PatchCandidateCache, `usePatch` in Renderer_Scene.cpp) keeps the baked
    // matHash, and an empty matDirty list is what arms it, so an undirtied write can stay
    // invisible indefinitely (AfterimageSystem dirties the same way). The cost is any
    // material write's (see MatFloatProp): quantize animated values; this no-ops when
    // unchanged.
    engine.set_function("SetMaterialParam",
        [this](LuaEntity id, const std::string& name, float x,
               sol::optional<float> y, sol::optional<float> z,
               sol::optional<float> w) -> bool
        {
            if (!m_world || name.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;

            // Custom-shader guard: b8 params exist only on a material with a custom shader.
            // Without it a typo'd name on an ordinary material would write the map and call
            // SetDirty(), dropping the retained candidate cache and the DDGI/RT retained gather
            // for the whole scene that frame (~2 ms) while changing nothing on screen.
            // SetMaterialTexture gates the same way.
            if (!mc->HasAnyCustomShader())
            {
                LOG_WARNING("Engine.SetMaterialParam('%s'): entity %u has no custom "
                            "shader — b8 params do not exist on this material",
                            name.c_str(), e);
                return false;
            }

            const std::array<float, 4> v{ x, y.value_or(0.f),
                                          z.value_or(0.f), w.value_or(0.f) };
            const auto it = mc->customParams.find(name);
            if (it != mc->customParams.end() && it->second == v) return true;
            mc->customParams[name] = v;
            mc->SetDirty();
            return true;
        });

    // -> ok, x, y, z, w   (ok = false when the entity/material/param is absent;
    // the Physics.Raycast multi-return idiom).
    engine.set_function("GetMaterialParam",
        [this](LuaEntity id, const std::string& name)
            -> std::tuple<bool, float, float, float, float>
        {
            constexpr std::tuple<bool, float, float, float, float> kNone
                { false, 0.f, 0.f, 0.f, 0.f };
            if (!m_world || name.empty()) return kNone;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return kNone;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return kNone;
            const auto it = mc->customParams.find(name);
            if (it == mc->customParams.end()) return kNone;
            return { true, it->second[0], it->second[1],
                           it->second[2], it->second[3] };
        });

    // ---- Whole-material swap ----
    // Replace the entity's material from an .imat / .imatinst. LoadMaterial resolves the
    // parent chain and marks the component dirty, so the GPU slot re-keys on its own.
    // FULL OVERWRITE: every runtime tweak is discarded and the flags (cast shadow, double
    // sided, blend mode) and custom-shader paths revert to the asset's. Right for a
    // two-state object (lamp_off.imat / lamp_on.imat); to animate one number use the
    // Material proxy. Textures resolve async, so fallbacks show for a frame or more.
    // MaterialSourcePath is not touched (a runtime appearance change, not an authoring
    // edit), so serialization keeps the authored asset; Engine.SetMaterialSource
    // repoints it.
    engine.set_function("SetMaterialAsset",
        [this](LuaEntity id, const std::string& path) -> bool
        {
            if (!m_world || path.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;
            if (!Resource::LoadMaterial(path, *mc))
            {
                LOG_WARNING("Engine.SetMaterialAsset: failed to load '%s'", path.c_str());
                return false;
            }
            return true;
        });

    // Drop the process-local authored-.imat snapshot cache. Needed only when .imat files
    // change on disk behind a running engine; the material serializers clear it after
    // their own writes.
    engine.set_function("ClearMaterialCache",
        [] { Resource::ClearMaterialCache(); });

    // ---- Material instances (.imatinst) ----
    // These make instance / override behaviour assertable from a headless scenario driver.

    // Engine.GetMaterialSource(entity) -> string
    // The .imat/.imatinst this entity's material was authored from, or "" for an
    // inline material. Project-root-relative, forward-slashed.
    engine.set_function("GetMaterialSource",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return {};
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return {};
            const MaterialSourcePath* sp = m_world->GetComponent<MaterialSourcePath>(e);
            return sp ? sp->path : std::string{};
        });

    // Engine.GetMaterialParent(path) -> string
    // The parent material of the file at `path`, or "" when it is a standalone
    // master (or unreadable). This is how a driver tells an instance from a master.
    engine.set_function("GetMaterialParent",
        [](const std::string& path) -> std::string
        { return path.empty() ? std::string{} : Resource::GetMaterialParent(path); });

    // Engine.SaveMaterialInstance(path, parentPath, entity) -> bool
    // Write the entity's CURRENT material as an instance of `parentPath`. False
    // on a self/cyclic parent, an unresolvable parent, or a write failure.
    engine.set_function("SaveMaterialInstance",
        [this](const std::string& path, const std::string& parentPath, LuaEntity id) -> bool
        {
            if (!m_world || path.empty() || parentPath.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;
            return Resource::SaveMaterialInstance(path, parentPath, *mc);
        });

    // Engine.GetMaterialCacheStats() -> { hits, misses, invalidations, entries }
    // Lets a scenario assert that a save invalidates only the materials that read the
    // saved file, not the whole cache.
    engine.set_function("GetMaterialCacheStats",
        [this]() -> sol::table
        {
            const Resource::MaterialCacheStats s = Resource::GetMaterialCacheStats();
            sol::table t = m_lua->create_table();
            t["hits"]          = static_cast<double>(s.hits);
            t["misses"]        = static_cast<double>(s.misses);
            t["invalidations"] = static_cast<double>(s.invalidations);
            t["entries"]       = static_cast<double>(s.entries);
            return t;
        });

    // Engine.SetMaterialSource(entity, path) -> bool
    // The AUTHORING counterpart of SetMaterialAsset: loads the material AND repoints
    // MaterialSourcePath, refreshing its provenance (MaterialInstanceInfo). Hot reload
    // finds affected entities by MaterialSourcePath, so an entity swapped with
    // SetMaterialAsset alone is invisible to it.
    engine.set_function("SetMaterialSource",
        [this](LuaEntity id, const std::string& path) -> bool
        {
            if (!m_world || path.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;
            Resource::MaterialLoadInfo info;
            if (!Resource::LoadMaterial(path, *mc, &info)) return false;
            if (auto* sp = m_world->GetComponent<MaterialSourcePath>(e)) sp->path = path;
            else m_world->AddComponent<MaterialSourcePath>(e, MaterialSourcePath{ path });
            ApplyMaterialLoadInfo(*m_world, e, info);
            mc->SetDirty();
            return true;
        });

    // Engine.ReloadMaterialsAffectedBy(path) -> int
    // Re-resolve every entity whose material depends on `path`, INCLUDING every
    // instance that inherits from it at any depth. Returns how many entities were
    // re-materialized. This is the same call the editor's asset watcher makes;
    // exposing it is what lets a headless scenario assert that a master edit
    // reaches live entities without a scene reload.
    engine.set_function("ReloadMaterialsAffectedBy",
        [this](const std::string& path) -> int
        {
            if (!m_world || path.empty()) return 0;
            return Resource::ReloadMaterialsAffectedBy(*m_world, path);
        });

    // Engine.GetMaterialInstanceParent(entity) -> string
    // The parent recorded on the entity's MaterialInstanceInfo, or "" when its
    // source is a master. Unlike GetMaterialParent(path) this reads the component,
    // so it also proves the provenance survived the load.
    engine.set_function("GetMaterialInstanceParent",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return {};
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return {};
            const MaterialInstanceInfo* mi = m_world->GetComponent<MaterialInstanceInfo>(e);
            return mi ? mi->parentPath : std::string{};
        });

    // Engine.GetMaterialOverrides(path) -> { "key", ... }
    // The keys the FILE at `path` authors. This cannot be derived from a loaded
    // MaterialComponent: after the parent merge an overridden value and an
    // inherited one are the same bytes. Sorted, so a test can compare it directly.
    engine.set_function("GetMaterialOverrides",
        [this](const std::string& path) -> sol::table
        {
            sol::table out = m_lua->create_table();
            if (path.empty()) return out;
            MaterialComponent scratch{};
            Resource::MaterialLoadInfo info;
            if (!Resource::LoadMaterial(path, scratch, &info)) return out;
            std::vector<std::string> keys(info.overrideKeys.begin(),
                                          info.overrideKeys.end());
            std::sort(keys.begin(), keys.end());
            for (size_t i = 0; i < keys.size(); ++i) out[i + 1] = keys[i];
            return out;
        });

    // Engine.SaveMaterialAsset(path, entity) -> bool
    // Write the entity's material as a STANDALONE .imat. Refused (false) when `path`
    // currently holds an instance: flattening one is never implicit.
    engine.set_function("SaveMaterialAsset",
        [this](const std::string& path, LuaEntity id) -> bool
        {
            if (!m_world || path.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            return mc && Resource::SaveMaterial(*mc, path);
        });

    // Engine.SaveMaterialInstanceEx(path, parentPath, entity, keys[, unset]) -> bool
    // The EXPLICIT form: `keys` is the array of parameter names this instance
    // overrides, and it is written as authored regardless of whether the value
    // currently differs from the parent. That is the whole difference from
    // SaveMaterialInstance above -- a value deliberately pinned to what the parent
    // happens to hold today survives the parent changing tomorrow.
    engine.set_function("SaveMaterialInstanceEx",
        [this](const std::string& path, const std::string& parentPath, LuaEntity id,
               sol::optional<sol::table> keys, sol::optional<sol::table> unset) -> bool
        {
            if (!m_world || path.empty() || parentPath.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;

            std::unordered_set<std::string> ovr;
            if (keys)
                for (auto& kv : *keys)
                {
                    const std::string k = kv.second.as<std::string>();
                    if (!k.empty()) ovr.insert(k);
                }
            std::vector<std::string> unsetKeys;
            if (unset)
                for (auto& kv : *unset)
                {
                    const std::string k = kv.second.as<std::string>();
                    if (!k.empty()) unsetKeys.push_back(k);
                }
            return Resource::SaveMaterialInstance(path, parentPath, *mc, &ovr, &unsetKeys);
        });

    // Engine.ReloadMaterial(entity) -> bool
    // Re-resolve the entity's material from its MaterialSourcePath, picking up any
    // edit made to the file OR to anything up its parent chain. Drops the snapshot
    // cache first, so this reads disk rather than the resolved copy taken at load.
    engine.set_function("ReloadMaterial",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            const MaterialSourcePath* sp = m_world->GetComponent<MaterialSourcePath>(e);
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!sp || sp->path.empty() || !mc) return false;
            Resource::ClearMaterialCache();
            return Resource::LoadMaterial(sp->path, *mc);
        });

    // ---- Texture slots ----
    // Engine.SetMaterialTexture(entity, slot, path) -> bool
    // `slot` is an integer index, an engine slot NAME ("BaseColorMap" / "EmissiveMap" /
    // ..., case-insensitive) or a custom shader's space3 texture binding name. An EMPTY
    // path unbinds the slot.
    // The renderer keys each material's runtime resource on a LAYOUT HASH of its texture
    // paths, so a new path is a new resource (new texture acquired, old one
    // refcount-released). But BuildScene_SyncMaterials skips any already-bound material
    // that is not dirty (Release has no self-heal), so SetDirty() is mandatory.
    // Async: for the 1-3 frames until the texture loads on a worker, the slot resolves to
    // bindless -1 and the surface samples the shader's fallback (white / flat normal);
    // SceneDiffView::matTexUnsettled keeps the DDGI retained gather from latching that -1.
    engine.set_function("SetMaterialTexture",
        [this](LuaEntity id, sol::object slot, const std::string& rawPath) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return false;

            const std::string path = NormalizeAssetPath(rawPath);

            // An empty path is legal ("unbind"); anything else MUST exist. A path that can never
            // load leaves its runtime material resource unresolved, pinning the renderer's
            // texture-unsettled counter above zero and disabling the retained candidate cache for
            // the whole scene for the rest of the session: a global perf regression with no
            // visible cause. Exists() checks the pak, the search roots (the shared content root
            // too) and the path as given: where the texture load will look.
            if (!path.empty() && !Resource::AssetFS::Get().Exists(path))
            {
                LOG_WARNING("Engine.SetMaterialTexture: asset '%s' does not exist",
                            path.c_str());
                return false;
            }

            std::string* target = nullptr;

            if (slot.get_type() == sol::type::number)
            {
                const int idx = static_cast<int>(slot.as<double>());
                if (idx < 0 || idx >= MaterialComponent::TEXTURESLOT_COUNT)
                {
                    LOG_WARNING("Engine.SetMaterialTexture: slot index %d out of range", idx);
                    return false;
                }
                target = &mc->textures[idx].name;
            }
            else if (slot.get_type() == sol::type::string)
            {
                const std::string name = slot.as<std::string>();
                const int idx = EngineTexSlotFromName(name);
                if (idx >= 0)
                {
                    target = &mc->textures[idx].name;
                }
                else
                {
                    // A custom space3 binding name - only on a material that HAS a custom shader.
                    // Otherwise a misspelled engine-slot name becomes a phantom custom binding, and since
                    // customTextures.size() is folded into the runtime key and the batch hash, it mints a
                    // whole material resource that binds nothing.
                    if (!mc->HasAnyCustomShader())
                    {
                        LOG_WARNING("Engine.SetMaterialTexture: unknown slot '%s' "
                                    "(and this material has no custom shader)",
                                    name.c_str());
                        return false;
                    }
                    // find() for the compare: operator[] would create an empty entry even on the no-op
                    // path, changing the map size (and the runtime key) with no dirty mark - the
                    // Release-only stale-binding case.
                    const auto it = mc->customTextures.find(name);
                    if (it != mc->customTextures.end())
                    {
                        target = &it->second.name;
                    }
                    else
                    {
                        // Absent is not an error: MaterialReflectionSync seeds this map only once the custom
                        // shader has compiled AND the entity has been gathered, so a legitimate slot is
                        // missing early on. Creating it is safe (Sync is find-guarded and will not clobber
                        // it); uvset stays 0, matching the editor's texture picker.
                        mc->customTextures[name].name = path;
                        mc->SetDirty();
                        return true;
                    }
                }
            }
            else
            {
                return false;   // slot must be a number or a string
            }

            if (!target) return false;
            if (PathCompareKey(*target) == PathCompareKey(path)) return true;

            *target = path;   // .name only: uvset is authored, not ours to touch
            mc->SetDirty();   // NOT SetEditDirty (see the Material usertype note)
            return true;
        });

    // -> the slot's current path, "" when unbound, nil when the entity/material
    // /slot does not exist.
    engine.set_function("GetMaterialTexture",
        [this](LuaEntity id, sol::object slot) -> sol::optional<std::string>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return sol::nullopt;
            const MaterialComponent* mc = m_world->GetComponent<MaterialComponent>(e);
            if (!mc) return sol::nullopt;

            if (slot.get_type() == sol::type::number)
            {
                const int idx = static_cast<int>(slot.as<double>());
                if (idx < 0 || idx >= MaterialComponent::TEXTURESLOT_COUNT) return sol::nullopt;
                return mc->textures[idx].name;
            }
            if (slot.get_type() == sol::type::string)
            {
                const std::string name = slot.as<std::string>();
                const int idx = EngineTexSlotFromName(name);
                if (idx >= 0) return mc->textures[idx].name;
                const auto it = mc->customTextures.find(name);
                if (it != mc->customTextures.end()) return it->second.name;
            }
            return sol::nullopt;
        });

    engine.set_function("GetCamera",
        [this](LuaEntity id) -> sol::optional<LuaCameraRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<CameraComponent>(e)) return sol::nullopt;
            return LuaCameraRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        });

    engine.set_function("GetCameraController",
        [this](LuaEntity id) -> sol::optional<LuaCameraCtrlRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<CameraControllerComponent>(e)) return sol::nullopt;
            return LuaCameraCtrlRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        });

    engine.set_function("GetAnimation",
        [this](LuaEntity id) -> sol::optional<LuaAnimationRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<AnimationComponent>(e)) return sol::nullopt;
            return LuaAnimationRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        });

    engine.set_function("GetMotionDirector",
        [this](LuaEntity id) -> sol::optional<LuaMotionDirectorRef>
        {
            if (!m_world) return sol::nullopt;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity || !m_world->GetComponent<MotionDirectorComponent>(e)) return sol::nullopt;
            return LuaMotionDirectorRef{ this, e, m_world->GetGeneration(e) & LuaId::kGenMask };
        });

    // ---- UI (Canvas-native Engine.* bindings) ----
    // Canvas visual fields are read every paint. Layout / input is resolved before
    // gameplay, so interaction state is available to scripts in the same frame.
    engine.set_function("SetUIText",
        [this](LuaEntity id, const std::string& s) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity ? m_world->GetComponent<UI::UIText>(e) : nullptr)
            {
                t->text = s;
                return true;
            }
            return false;
        });

    engine.set_function("GetUIText",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return std::string();
            if (const auto* t = m_world->GetComponent<UI::UIText>(LuaId::ResolveLive(*m_world, id)))
                return t->text;
            return std::string();
        });

    // Per-element font (FontLibrary path; "" = engine default). It loads and bakes on
    // demand at the next UI paint; no preload call.
    engine.set_function("SetUIFont",
        [this](LuaEntity id, const std::string& ttfPath) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity ? m_world->GetComponent<UI::UIText>(e) : nullptr)
            {
                t->fontPath = ttfPath;
                return true;
            }
            return false;
        });

    engine.set_function("GetUIFont",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return std::string();
            if (const auto* t = m_world->GetComponent<UI::UIText>(LuaId::ResolveLive(*m_world, id)))
                return t->fontPath;
            return std::string();
        });

    // A font's atlas stated as hashes, so two runs (or two bake paths) can be held to the
    // same atlas. Never loads a font. The hashes read every page (~1 MB each): ask once,
    // not per frame.
    LuaDoc::Fn(engine, "Engine", "GetFontAtlasStats",
        [](sol::this_state ts, sol::optional<std::string> ttfPath) -> sol::object
        {
            sol::state_view lv(ts);
            const std::string path = ttfPath.value_or(std::string());
            UI::FontLibrary& lib = UI::FontLibrary::Get();
            const UI::Font* font = path.empty() ? (lib.IsReady() ? &lib.Default() : nullptr) : lib.Find(path);
            if (!font) return sol::lua_nil;
            const UI::Font::AtlasHash h = font->HashAtlas();
            char layout[17], pixels[17];
            std::snprintf(layout, sizeof(layout), "%016llx", static_cast<unsigned long long>(h.layout));
            std::snprintf(pixels, sizeof(pixels), "%016llx", static_cast<unsigned long long>(h.pixels));
            sol::table t = lv.create_table();
            t["path"]       = font->TTFPath();
            t["glyphs"]     = static_cast<double>(font->Glyphs().size());
            t["pages"]      = static_cast<double>(font->PageCount());
            t["pending"]    = static_cast<double>(font->PendingCells());
            t["layoutHash"] = std::string(layout);
            t["pixelHash"]  = std::string(pixels);
            return t;
        },
            { .doc = "A loaded font's glyph atlas: {path, glyphs, pages, pending, layoutHash, pixelHash}, or nil "
                     "when that font was never loaded (this never loads one; no path = the default font). glyphs "
                     "is the resident glyph table (spaces and tofu included), pages the 1024x1024 atlas pages. "
                     "layoutHash (FNV-1a, 16 hex digits) covers the glyph table in codepoint order - UVs, size, "
                     "bearing, advance, page - and pixelHash the CPU copy of every page; the same text baked "
                     "the same way gives the same two. pending counts cells placed but not yet rendered: the "
                     "pixel hash is the final atlas only while it is 0. Reads every page (about 1 ms each).",
              .params = { { "ttfPath", "the font's path as loaded (e.g. \"asset/font/X.ttf\"); omitted = the default font",
                            "string", true } },
              .returns = "table?" });

    // Custom text pixel shader (path relative to shaders/, "" = built-in) +
    // its free-form params (index 0-3 = customParam0.xyzw, 4-7 = customParam1).
    engine.set_function("SetUITextCustomShader",
        [this](LuaEntity id, const std::string& hlslPath) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity ? m_world->GetComponent<UI::UIText>(e) : nullptr)
            {
                t->customShaderPath = hlslPath;
                return true;
            }
            return false;
        });

    engine.set_function("SetUITextCustomParam",
        [this](LuaEntity id, int index, float value) -> bool
        {
            if (!m_world || index < 0 || index > 7) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            auto* t = e != NullEntity ? m_world->GetComponent<UI::UIText>(e) : nullptr;
            if (!t) return false;
            float* p = (index < 4) ? &t->customParam0.x : &t->customParam1.x;
            p[index & 3] = value;
            return true;
        });

    // Free-combination effect toggles (any subset may be on at once).
    engine.set_function("SetUITextEffects",
        [this](LuaEntity id, bool outline, bool glow, bool shadow, bool jitter) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity ? m_world->GetComponent<UI::UIText>(e) : nullptr)
            {
                t->fxOutline = outline;
                t->fxGlow    = glow;
                t->fxShadow  = shadow;
                t->fxJitter  = jitter;
                return true;
            }
            return false;
        });

    engine.set_function("SetWorldUITextFont",
        [this](LuaEntity id, const std::string& ttfPath) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity
                          ? m_world->GetComponent<UI::WorldUITextComponent>(e) : nullptr)
            {
                t->fontPath = ttfPath;
                return true;
            }
            return false;
        });

    engine.set_function("SetWorldUITextEffects",
        [this](LuaEntity id, bool outline, bool glow, bool shadow, bool jitter) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity
                          ? m_world->GetComponent<UI::WorldUITextComponent>(e) : nullptr)
            {
                t->fxOutline = outline;
                t->fxGlow    = glow;
                t->fxShadow  = shadow;
                t->fxJitter  = jitter;
                return true;
            }
            return false;
        });

    engine.set_function("SetWorldUITextCustomShader",
        [this](LuaEntity id, const std::string& hlslPath) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* t = e != NullEntity
                          ? m_world->GetComponent<UI::WorldUITextComponent>(e) : nullptr)
            {
                t->customShaderPath = hlslPath;
                return true;
            }
            return false;
        });

    engine.set_function("SetUIImageTexture",
        [this](LuaEntity id, const std::string& path) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (auto* image = e != NullEntity ? m_world->GetComponent<UI::UIImage>(e) : nullptr)
            {
                image->texturePath = path;
                return true; // App's entity lease swaps the TextureHandle safely.
            }
            return false;
        });

    // Works on a NODE (UIRect::visible) or a whole CANVAS (UICanvas::visible): a canvas
    // root has no UIRect, and hiding a whole screen is what a menu needs most.
    engine.set_function("SetUIVisible",
        [this](LuaEntity id, bool visible) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            if (auto* rect = m_world->GetComponent<UI::UIRect>(e))
            {
                // UIRect visibility is inherited by the subtree and gates both
                // rendering and raycast without overwriting authored interactable flags.
                rect->visible = visible;
                return true;
            }
            // Canvas ROOT: no UIRect (its rect is the screen), so the equivalent
            // flag lives on UICanvas and gates the whole canvas the same way.
            if (auto* cv = m_world->GetComponent<UI::UICanvas>(e))
            {
                cv->visible = visible;
                return true;
            }
            return false;
        });

    // Engine.SetUIKeyboardCapture(on): claim (or release) the keyboard for the UI. While
    // on, Window drops WM_KEYDOWN / WM_CHAR before the Keyboard singleton sees them, so
    // keys pressed over a menu do not also reach gameplay. The Canvas has no focus owner
    // that asks for this, so a screen that reads keys (a rebind row, a text field) must.
    // STICKY: whoever turns it on turns it off. Clear it in the screen's close path, not
    // only on success, or gameplay keys stay silent. Static state: needs no bound world.
    engine.set_function("SetUIKeyboardCapture",
        [](bool on) { UI::UICanvasSystem::SetKeyboardCaptureRequest(on); });

    // ---- Focus / directional navigation (engine-owned) ----
    // UICanvasSystem owns one focused element and moves it geometrically from the nav
    // edges App derives each frame. A script reads focus for painting and gets activation
    // free: submit sets the same one-frame `clicked` edge a mouse release does, so
    // Engine.WasUIClicked covers pad and keyboard.
    engine.set_function("IsUIFocused",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            if (const auto* it = m_world->GetComponent<UI::UIInteractable>(
                                     LuaId::ResolveLive(*m_world, id)))
                return it->focused;
            return false;
        });

    // -> entityId, or 0 when nothing focusable is on screen.
    engine.set_function("GetUIFocus",
        [this]() -> LuaEntity
        {
            if (!m_world) return {};
            return LuaId::Out(*m_world, UI::UICanvasSystem::FocusedEntityGlobal());
        });

    // Force focus somewhere (opening a screen, after rebuilding a page). Refused unless
    // the target is live, enabled, focusable and on screen: focus on a hidden page means
    // the pad drives what nobody can see.
    engine.set_function("SetUIFocus",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            const Entity e = id.raw() == NullEntity
                           ? NullEntity : LuaId::ResolveLive(*m_world, id);
            return UI::UICanvasSystem::SetFocusGlobal(*m_world, e);
        });

    // -> { x, y, submit, cancel }: this frame's navigation edges. `x` is here because a
    // focused element with absorbNavX keeps the horizontal axis for its own value (a
    // slider, a cycler): focus does not move and the owning script reads it here. The
    // engine never acts on `cancel`: closing a screen is the screen's decision.
    engine.set_function("GetUINav",
        [this]() -> sol::table
        {
            return m_lua->create_table_with(
                "x",      UI::UICanvasSystem::NavXGlobal(),
                "y",      UI::UICanvasSystem::NavYGlobal(),
                "submit", UI::UICanvasSystem::SubmitGlobal(),
                "cancel", UI::UICanvasSystem::CancelGlobal());
        });

    engine.set_function("IsUIHovered",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            if (const auto* it = m_world->GetComponent<UI::UIInteractable>(LuaId::ResolveLive(*m_world, id)))
                return it->hovered;
            return false;
        });

    engine.set_function("IsUIPressed",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            if (const auto* it = m_world->GetComponent<UI::UIInteractable>(LuaId::ResolveLive(*m_world, id)))
                return it->pressed;
            return false;
        });

    engine.set_function("WasUIClicked",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            if (const auto* it = m_world->GetComponent<UI::UIInteractable>(LuaId::ResolveLive(*m_world, id)))
                return it->clicked;
            return false;
        });

    // ---- UI layout / colour / parenting (runtime-writable fields) ----
    // The Canvas UI is authored in the editor; these let a script drive an existing node's
    // size, position, colour and parent (a health bar that grows, a tooltip that follows).
    // Nodes are created with Engine.CreateEntity + Engine.AddComponent(tag, kv), which
    // dispatches into the component serializer registry and so covers every Cv* UI
    // component (CvCanvas / CvRect / CvClip / CvParent / CvImage / CvSpriteAnim / CvText /
    // CvInteract, see ComponentSerializers.cpp); edges still go through
    // Engine.AttachUIParent.

    // Engine.SetUIRect(id, { size={w,h}, offset={x,y}, anchorMin={x,y},
    //                        anchorMax={x,y}, pivot={x,y}, siblingIndex=n, visible=b })
    // Every key optional: only the ones present are written.
    engine.set_function("SetUIRect",
        [this](LuaEntity id, sol::table opts) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            auto* r = e != NullEntity ? m_world->GetComponent<UI::UIRect>(e) : nullptr;
            if (!r) return false;

            auto readVec2 = [](const sol::object& o, UI::Vec2& dst) -> bool
            {
                if (!o.is<sol::table>()) return false;
                sol::table t = o.as<sol::table>();
                // Accept {x=,y=} or the positional {w,h} / {x,y} array form.
                dst.x = t["x"].valid() ? t["x"].get<float>() : t.get_or(1, dst.x);
                dst.y = t["y"].valid() ? t["y"].get<float>() : t.get_or(2, dst.y);
                return true;
            };
            readVec2(opts["size"],      r->size);
            readVec2(opts["offset"],    r->offset);
            readVec2(opts["anchorMin"], r->anchorMin);
            readVec2(opts["anchorMax"], r->anchorMax);
            readVec2(opts["pivot"],     r->pivot);
            if (opts["siblingIndex"].valid())
                r->siblingIndex = opts["siblingIndex"].get<int>();
            if (opts["visible"].valid())
                r->visible = opts["visible"].get<bool>();
            return true;
        });

    // -> { sizeX, sizeY, offsetX, offsetY, pivotX, pivotY, siblingIndex, visible }, nil
    // when there is no UIRect
    engine.set_function("GetUIRect",
        [this](LuaEntity id) -> sol::object
        {
            if (!m_world) return sol::lua_nil;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* r = e != NullEntity ? m_world->GetComponent<UI::UIRect>(e) : nullptr;
            if (!r) return sol::lua_nil;
            return sol::make_object(*m_lua, m_lua->create_table_with(
                "sizeX", r->size.x,   "sizeY", r->size.y,
                "offsetX", r->offset.x, "offsetY", r->offset.y,
                "pivotX", r->pivot.x, "pivotY", r->pivot.y,
                "siblingIndex", r->siblingIndex,
                "visible", r->visible));
        });

    // Engine.GetUIComputedRect(id) -> { x, y, w, h, clipX, clipY, clipW, clipH,
    //                                    canvasScale, depth, sortKey, culled }
    // The RESOLVED screen rect UICanvasSystem produced this frame, in Engine.GetUIPointer's
    // space: UIRect is layout INPUT (anchors + sizeDelta, reference pixels), this is the
    // OUTPUT (real pixels, after the anchor solve and the canvas scaler), what a slider
    // track, a drag region or a custom hit test must read.
    // nil when the entity has no UIComputedRect or `valid == false` (not computed this
    // frame: a culled ancestor, or a node not yet through UpdateInput). Returning last
    // frame's rect would leave a drag one frame behind the pointer.
    engine.set_function("GetUIComputedRect",
        [this](LuaEntity id) -> sol::object
        {
            if (!m_world) return sol::lua_nil;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* cr = e != NullEntity
                           ? m_world->GetComponent<UI::UIComputedRect>(e) : nullptr;
            if (!cr || !cr->valid) return sol::lua_nil;
            return sol::make_object(*m_lua, m_lua->create_table_with(
                "x", cr->rect.mn.x, "y", cr->rect.mn.y,
                "w", cr->rect.Width(), "h", cr->rect.Height(),
                "clipX", cr->clipRect.mn.x, "clipY", cr->clipRect.mn.y,
                "clipW", cr->clipRect.Width(), "clipH", cr->clipRect.Height(),
                "canvasScale", cr->canvasScale,
                "depth", cr->depth,
                "sortKey", cr->sortKey,
                "culled", cr->culled));
        });

    // Engine.SetUIColor(id, r, g, b, a?): writes UIText.color and/or UIImage.color,
    // whichever the entity has (a node may have both).
    engine.set_function("SetUIColor",
        [this](LuaEntity id, float r, float g, float b, sol::optional<float> a) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            const DirectX::XMFLOAT4 c{ r, g, b, a.value_or(1.f) };
            bool any = false;
            if (auto* t = m_world->GetComponent<UI::UIText>(e))  { t->color = c; any = true; }
            if (auto* im = m_world->GetComponent<UI::UIImage>(e)) { im->color = c; any = true; }
            if (!any)
                LOG_WARNING("Engine.SetUIColor: entity %u has neither UIText nor UIImage", e);
            return any;
        });

    // Engine.AttachUIParent(child, parent): the ONLY sanctioned way to build a UI
    // hierarchy edge (UI::AttachUIParent stamps the parent GUID so the edge survives
    // save/load; writing UIParent directly does not). parent = 0 detaches.
    engine.set_function("AttachUIParent",
        [this](LuaEntity child, LuaEntity parent) -> bool
        {
            if (!m_world) return false;
            const Entity c = LuaId::ResolveLive(*m_world, child);
            if (c == NullEntity) return false;
            const Entity p = parent.raw() == NullEntity
                           ? NullEntity : LuaId::ResolveLive(*m_world, parent);
            if (parent.raw() != NullEntity && p == NullEntity)
            {
                LOG_WARNING("Engine.AttachUIParent: parent %u not alive (or recycled)",
                            parent.raw());
                return false;
            }
            UI::AttachUIParent(*m_world, c, p);
            return true;
        });

    // Engine.SetWorldUIBar(id, value, visible): value clamped to [0, 1]; false for a
    // non-finite value or no WorldUIBarComponent.
    engine.set_function("SetWorldUIBar",
        [this](LuaEntity id, float value, bool visible) -> bool {
            if (!m_world || !std::isfinite(value)) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            auto* bar = e != NullEntity ? m_world->GetComponent<UI::WorldUIBarComponent>(e) : nullptr;
            if (!bar) return false;
            bar->value = std::clamp(value, 0.f, 1.f);
            bar->visible = visible;
            return true;
        });

    // Engine.SetWorldUIText / GetWorldUIText: the world-space twins of SetUIText / GetUIText.
    engine.set_function("SetWorldUIText",
        [this](LuaEntity id, const std::string& s) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            auto* t = e != NullEntity
                    ? m_world->GetComponent<UI::WorldUITextComponent>(e) : nullptr;
            if (!t) return false;
            t->text = s;
            return true;
        });

    engine.set_function("GetWorldUIText",
        [this](LuaEntity id) -> std::string
        {
            if (!m_world) return {};
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* t = e != NullEntity
                          ? m_world->GetComponent<UI::WorldUITextComponent>(e) : nullptr;
            return t ? t->text : std::string{};
        });

    // Engine.FindFirstWithComponent(name) -> entity (0 when none); name is Animation,
    // Skeleton, MeshSkinned or Material. For test drivers: some entities (spawned through
    // a SceneRef, no PlayerComponent, no stable name) are reachable by no other Lua path,
    // e.g. the skinned actor tests/scenarios/anim_skin checks for a T-pose regression.
    // Pool order: stable for a given scene but not an authoring contract.
    engine.set_function("FindFirstWithComponent",
        [this](const std::string& name) -> LuaEntity
        {
            if (!m_world) return {};
            auto firstOf = [&](auto* pool) -> Entity {
                if (!pool || pool->Entities().empty()) return NullEntity;
                return pool->Entities().front();
            };
            Entity e = NullEntity;
            if      (name == "Animation")  e = firstOf(m_world->GetPool<AnimationComponent>());
            else if (name == "Skeleton")   e = firstOf(m_world->GetPool<SkeletonComponent>());
            else if (name == "MeshSkinned")e = firstOf(m_world->GetPool<MeshSkinnedComponent>());
            else if (name == "Material")   e = firstOf(m_world->GetPool<MaterialComponent>());
            else
            {
                // Naming a component that is not wired here is a script bug, and
                // silently returning 0 reads exactly like "the scene has none".
                LOG_WARNING("Engine.FindFirstWithComponent('%s'): unknown component name. "
                            "Known: Animation, Skeleton, MeshSkinned, Material.",
                            name.c_str());
                return {};
            }
            if (e == NullEntity) return {};
            return LuaId::Out(*m_world, e);
        });

    // Engine.FindAnimatedDescendant(id): depth-first from `id` (itself included), the first
    // entity with an AnimationComponent, 0 when none. For a character split as root
    // (collider / AI / script) + meshNode (skinned mesh + AnimationComponent): the BT runs
    // on root but clip transitions target meshNode. Call once in OnSpawn and cache.
    engine.set_function("FindAnimatedDescendant",
        [this](LuaEntity id) -> LuaEntity
        {
            if (!m_world) return {};
            const Entity start = LuaId::ResolveLive(*m_world, id);
            if (start == NullEntity) return {};

            // Visited-set guarded: Children is authored data (scene file, prefab, editor
            // reparent), so a cycle is a data bug, and an unguarded walk would hang the script
            // phase with no log.
            std::unordered_set<Entity> visited;
            std::vector<Entity> stack;
            stack.push_back(start);
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (!visited.insert(e).second) continue;
                if (m_world->GetComponent<AnimationComponent>(e))
                    return LuaId::Out(*m_world, e);
                if (const auto* kids = m_world->GetComponent<Children>(e))
                {
                    // Reverse-push to preserve a depth-first walk that
                    // visits earlier children first.
                    for (auto it = kids->entities.rbegin();
                         it != kids->entities.rend(); ++it)
                    {
                        stack.push_back(*it);
                    }
                }
            }
            return {};
        });

    // Engine.FindMaterialDescendant(id): depth-first from `id` (itself included), the
    // first entity with a MaterialComponent, 0 when none. The importer attaches the .imat
    // to the MESH entity, so a prefab root (where the Logic script lives) owns none:
    //   root  (script)
    //     +- Bulb  (mesh + MaterialComponent)
    // Call once in OnSpawn and cache. Ambiguous on a multi-material mesh (one child entity
    // per material, "first" in declaration order): name the child and use
    // Engine.FindChildByName.
    engine.set_function("FindMaterialDescendant",
        [this](LuaEntity id) -> LuaEntity
        {
            if (!m_world) return {};
            const Entity start = LuaId::ResolveLive(*m_world, id);
            if (start == NullEntity) return {};

            // Visited-set guarded so a malformed cyclic Children graph terminates instead of
            // hanging the script phase.
            std::vector<Entity>        stack;
            std::unordered_set<Entity> visited;
            stack.push_back(start);
            visited.insert(start);
            while (!stack.empty())
            {
                const Entity e = stack.back();
                stack.pop_back();
                if (m_world->GetComponent<MaterialComponent>(e))
                    return LuaId::Out(*m_world, e);
                if (const auto* kids = m_world->GetComponent<Children>(e))
                {
                    // Reverse-push so children POP in declaration order.
                    for (auto it = kids->entities.rbegin();
                         it != kids->entities.rend(); ++it)
                    {
                        if (visited.insert(*it).second) stack.push_back(*it);
                    }
                }
            }
            return {};
        });

    LuaDoc::Fn(engine, "Engine", "HasComponent",
        [this](LuaEntity id, const std::string& name) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if      (name == "LocalTransform")    return m_world->HasComponent<LocalTransform>(e);
            else if (name == "Light")             return m_world->HasComponent<LightData>(e);
            else if (name == "Material")          return m_world->HasComponent<MaterialComponent>(e);
            else if (name == "Camera")            return m_world->HasComponent<CameraComponent>(e);
            else if (name == "CameraController")  return m_world->HasComponent<CameraControllerComponent>(e);
            else if (name == "Animation")         return m_world->HasComponent<AnimationComponent>(e);
            else if (name == "MotionDirector")    return m_world->HasComponent<MotionDirectorComponent>(e);
            else if (name == "UICanvas")          return m_world->HasComponent<UI::UICanvas>(e);
            else if (name == "UIRect")            return m_world->HasComponent<UI::UIRect>(e);
            else if (name == "UIImage")           return m_world->HasComponent<UI::UIImage>(e);
            else if (name == "UIText")            return m_world->HasComponent<UI::UIText>(e);
            else if (name == "UIInteractable")    return m_world->HasComponent<UI::UIInteractable>(e);
            else if (name == "Script")            return m_world->HasComponent<ScriptComponent>(e);
            else if (name == "Tag")               return m_world->HasComponent<TagComponent>(e);
            else if (name == "Locomotion")        return m_world->HasComponent<LocomotionComponent>(e);
            else if (name == "LocoTransitions")   return m_world->HasComponent<LocomotionTransitionComponent>(e);
            else if (name == "AnimLayers")        return m_world->HasComponent<AnimLayerComponent>(e);
            else if (name == "AnimDriver")        return m_world->HasComponent<CharacterAnimDriverComponent>(e);
            else if (name == "Brain")             return m_world->HasComponent<BrainComponent>(e);
            else if (name == "CharacterController") return m_world->HasComponent<CharacterControllerComponent>(e);
            // Fall back to the serializer registry before giving up: it knows
            // every saveable component by tag, so this covers the long tail the
            // hand-written chain above does not, and keeps working as new
            // components are registered.
            if (const auto* ser = GetComponentRegistry().FindByTag(name))
                return ser->has && ser->has(*m_world, e);

            static constexpr char kSite[] = "Engine.HasComponent/unknown";
            return LuaDiag::Fail(kSite, false,
                "Engine.HasComponent: unknown component name '%s'", name.c_str());
        },
            { .doc = "Whether the entity has the component.", .params = { { .name = "id" }, { .name = "name", .doc = "the serializer tag" } }, .context = LuaDoc::Context::Update });

    // Engine.AddComponent(id, "Tag" [, { key = "value", ... }]) -> bool
    // Built on the serializer registry, so it covers every component that can be saved
    // and cannot drift from that list. The table uses the .iscene key/value form. Values:
    // numbers, booleans, text (a vector may be its text form "1_0_0"; text is
    // percent-encoded the way the serializers write it), a list of numbers or a Vec3 (the
    // vector form). No table = a default-constructed component. A key the serializer does
    // not write back warns once (a typo would otherwise do nothing).
    // A component the entity ALREADY has is not rebuilt from defaults: its deserializer
    // would reset every key not given and lose runtime state (a RigidBody's live Jolt body
    // leaked as a ghost collider, a playing AudioSource's voice orphaned and playOnEnable
    // starting another, a light re-stamped as the newest so a directional one becomes the
    // sun, a second Script slot). It is merged like Entity.set
    // (Scripting/LuaEntityWrite.h): only the keys given, runtime state carried across.
    // What Entity.set refuses (Script - use Engine.AttachScript, CharacterController, the
    // material blocks, an unwritten key, a wrongly shaped value) returns false with a
    // warning.
    LuaDoc::Fn(engine, "Engine", "AddComponent",
        [this](sol::this_state ts, LuaEntity id, const std::string& tag, sol::object optsObj) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;

            const auto* ser = GetComponentRegistry().FindByTag(tag);
            if (!ser || !ser->deserialize)
            {
                static constexpr char kSite[] = "Engine.AddComponent/unknown";
                return LuaDiag::Fail(kSite, false,
                    "Engine.AddComponent: no serializer registered for '%s' — "
                    "the tag must match the .iscene component tag exactly",
                    tag.c_str());
            }
            const bool haveValues = optsObj.is<sol::table>();

            if (ser->has && ser->has(*m_world, e))
            {
                if (!haveValues) return true;   // it is there; nothing to change
                try
                {
                    LuaEntityMergeWrite(sol::state_view(ts), *m_world, e, id, tag, optsObj.as<sol::table>());
                    return true;
                }
                catch (const sol::error& err)
                {
                    const std::string why = err.what();
                    LuaDiag::WarnOnceNamed("Engine.AddComponent/existing/" + tag + "/" + why,
                        "Engine.AddComponent(%s): the entity already has one, so it is changed "
                        "like Entity.set - refused, nothing changed: %s", tag.c_str(), why.c_str());
                    return false;
                }
            }

            KVMap kv;
            if (!Resource::Authoring::Fields(tag).empty())
            {
                try
                {
                    if (optsObj.valid() && optsObj != sol::lua_nil && !haveValues)
                        throw sol::error("Entity.create: bad_value: fields must be a table");
                    const sol::table fields = haveValues ? optsObj.as<sol::table>() : sol::state_view(ts).create_table();
                    for (const auto& [key, text] : LuaAuthoringCreateValues(tag, fields)) kv[key] = text;
                }
                catch (const sol::error& error)
                {
                    LuaDiag::WarnOnceNamed("Engine.AddComponent/create/" + tag + "/" + error.what(),
                        "Engine.AddComponent(%s): refused before creation: %s", tag.c_str(), error.what());
                    return false;
                }
            }
            else if (haveValues)
            {
                for (const auto& [k, v] : optsObj.as<sol::table>())
                {
                    if (!k.is<std::string>())
                    {
                        LuaDiag::WarnOnceNamed("Engine.AddComponent/key/" + tag,
                            "Engine.AddComponent(%s): the values table has a key that is not a "
                            "name (a list entry?) - ignored", tag.c_str());
                        continue;
                    }
                    const std::string key = k.as<std::string>();
                    std::string text, why;
                    if (!LuaValueToFieldText(v, text, why))
                    {
                        LuaDiag::WarnOnceNamed("Engine.AddComponent/value/" + tag + "/" + key,
                            "Engine.AddComponent(%s): %s wants %s - nothing was added",
                            tag.c_str(), key.c_str(), why.c_str());
                        return false;
                    }
                    kv[key] = std::move(text);
                }
            }
            // AssetManager is null: components whose deserializer resolves
            // assets (meshes, textures) will come up unresolved. Prefabs remain
            // the right tool for those; this is for plain data components.
            ser->deserialize(*m_world, e, kv, nullptr);

            // A key the serializer does not write back is a typo, an alias it reads once and does
            // not keep, or a key it writes only when non-default. Not checked for the tags whose
            // serializer stamps a GUID on the referenced entity (LuaEntityDedicatedTags; Entity.*
            // reads them with dedicated readers for the same reason): a check must not change the
            // world.
            const std::vector<std::string> stamping = LuaEntityDedicatedTags();   // sorted
            if (!kv.empty() && ser->serialize && (!ser->has || ser->has(*m_world, e)) &&
                !std::binary_search(stamping.begin(), stamping.end(), tag))
            {
                std::ostringstream ss;
                ser->serialize(*m_world, e, ss);
                std::istringstream lines(ss.str());
                std::string line, written;
                std::set<std::string> keys;
                while (std::getline(lines, line))
                    for (const auto& [wk, wv] : ComponentWrite::ParseLine(line)) keys.insert(wk);
                for (const std::string& wk : keys) { if (!written.empty()) written += ", "; written += wk; }
                for (const auto& [key, text] : kv)
                    if (!keys.count(key))
                        LuaDiag::WarnOnceNamed("Engine.AddComponent/unwritten/" + tag + "/" + key,
                            "Engine.AddComponent(%s): '%s' is not a key its serializer wrote back (%s) - "
                            "a typo, a legacy key read once and not kept, or a key it only writes when "
                            "it differs from the default", tag.c_str(), key.c_str(), written.c_str());
            }
            return true;
        },
            { .doc = "Add a component by its serializer tag (as .iscene writes it), fields from an optional {key = value} table; one the entity already has is merged like Entity.set.", .params = { { .name = "id" }, { .name = "tag" }, { .name = "fields", .type = "table", .optional = true } }, .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Update });

    // ---- Time of Day ----
    // TOD is OPT-IN: the pipeline runs only when some entity has a TODConfigComponent
    // (editor: Create > Environment > Time of Day) with enabled = true; SceneDefaults
    // creates it disabled. GetTimeOfDay / IsNight return NIL in both cases rather than a
    // plausible default, so a lamp script can tell "no day/night authority" from "daytime"
    // and fall back to explicit control; IsTimeOfDayEnabled() tells the two nil causes
    // apart. Never cache what these wrap: TODUtil::FindConfig returns &pool->Data()[0],
    // which any AddComponent can realloc.
    // Timebase: the Renderer advances TOD (SyncSkyboxIBL) on raw wall-clock dt, so it
    // runs while paused and ignores Engine.SetTimeScale, and in the Render phase, after
    // gameplay, so a script reads last frame's value. No sunrise / sunset event: poll and
    // latch the edge.
    engine.set_function("GetTimeOfDay",
        [this]() -> sol::optional<float>
        {
            if (!m_world) return sol::nullopt;
            const TODConfigComponent* cfg = TODUtil::FindConfig(*m_world);
            if (!cfg || !cfg->enabled) return sol::nullopt;
            return cfg->timeOfDay;   // 0 = midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset
        });

    engine.set_function("IsNight",
        [this]() -> sol::optional<bool>
        {
            if (!m_world) return sol::nullopt;
            const TODConfigComponent* cfg = TODUtil::FindConfig(*m_world);
            if (!cfg || !cfg->enabled) return sol::nullopt;
            const TODOutputComponent* out = TODUtil::FindOutput(*m_world);
            if (!out) return sol::nullopt;
            return out->isMoonActive;   // sun below horizon, moon drives lighting
        });

    engine.set_function("IsTimeOfDayEnabled",
        [this]() -> bool
        {
            if (!m_world) return false;
            const TODConfigComponent* cfg = TODUtil::FindConfig(*m_world);
            return cfg && cfg->enabled;
        });

    // Debug / cutscene forcing. Writes even when the config is disabled (so you
    // can seed a time before enabling); returns false only when the scene has
    // no TOD entity at all.
    engine.set_function("SetTimeOfDay",
        [this](float t) -> bool
        {
            if (!m_world) return false;
            TODConfigComponent* cfg = TODUtil::FindConfig(*m_world);
            if (!cfg) return false;
            cfg->timeOfDay = t - std::floor(t);   // wrap into [0,1)
            return true;
        });

    // ---- Tags ----
    engine.set_function("AddTag",
        [this](LuaEntity id, const std::string& tag) -> bool
        {
            if (!m_world || tag.empty()) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            TagComponent* tc = m_world->GetComponent<TagComponent>(e);
            if (!tc) { m_world->AddComponent<TagComponent>(e, {}); tc = m_world->GetComponent<TagComponent>(e); }
            return tc && tc->Add(tag.c_str());
        });

    engine.set_function("HasTag",
        [this](LuaEntity id, const std::string& tag) -> bool
        {
            if (!m_world) return false;
            const TagComponent* tc = m_world->GetComponent<TagComponent>(LuaId::ResolveLive(*m_world, id));
            return tc && tc->Has(tag.c_str());
        });

    engine.set_function("RemoveTag",
        [this](LuaEntity id, const std::string& tag) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            TagComponent* tc = m_world->GetComponent<TagComponent>(e);
            return tc && tc->Remove(tag.c_str());
        });

    // ---- Entity discovery ----
    // Gameplay wiring: a door / chest / pickup resolves "the player" / "its key" at
    // runtime instead of only reacting to an entity handed to it via OnEnter. Linear
    // scans, fine for the handful of named / tagged gameplay objects in a scene.
    LuaDoc::Fn(engine, "Engine", "FindByName",
        [this](const std::string& name) -> LuaEntity
        {
            if (!m_world) return {};
            for (Entity e : m_world->GetEntities())
                if (m_world->GetName(e) == name) return LuaId::Out(*m_world, e);
            return {};
        },
            { .doc = "The first entity whose name matches exactly (a linear scan); an invalid id when none does.", .params = { { .name = "name" } }, .context = LuaDoc::Context::Update });

    // Engine.FindAllByName(name [, contains]) -> { id, id, ... }
    // Every match, where FindByName stops at the first. `contains = true` switches to a
    // case-insensitive substring test, which an imported scene needs (Tokyo.iscn numbers
    // its lamps SM_StreetLight1 .. SM_StreetLight170). O(entities) per call: call it ONCE
    // (OnInit) and keep the table, never per frame.
    engine.set_function("FindAllByName",
        [this](const std::string& name, sol::optional<bool> contains) -> sol::table
        {
            sol::table out = m_lua->create_table();
            if (!m_world || name.empty()) return out;

            const bool sub = contains.value_or(false);
            std::string needle = name;
            if (sub)
                std::transform(needle.begin(), needle.end(), needle.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            int i = 0;
            for (Entity e : m_world->GetEntities())
            {
                const std::string& n = m_world->GetName(e);
                if (!sub)
                {
                    if (n == name) out[++i] = LuaId::Out(*m_world, e);
                    continue;
                }
                std::string low = n;
                std::transform(low.begin(), low.end(), low.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (low.find(needle) != std::string::npos) out[++i] = LuaId::Out(*m_world, e);
            }
            return out;
        });

    // Engine.SetParent(child, parent [, keepWorld]) -> bool
    // Transform-hierarchy reparenting: maintains Parent AND the parent's Children list,
    // which GetChildren / FindChildByName / the Outliner / TransformPropagateSystem read.
    // Not Engine.AttachToEntity (a FollowEntityComponent per-frame follower).
    // parent = 0 detaches to the scene root. keepWorld (default true, like the editor's
    // drag-drop) rewrites the child's LocalTransform as childWorld * inv(parentGT) so its
    // world pose does not move; pass false to write the local offset yourself. Refuses a
    // cycle (parenting under its own descendant), which would hang every subtree walk.
    engine.set_function("SetParent",
        [this](LuaEntity childId, LuaEntity parentId, sol::optional<bool> keepWorld) -> bool
        {
            using namespace DirectX;
            if (!m_world) { LOG_WARNING("Engine.SetParent called outside Update"); return false; }

            const Entity child = LuaId::ResolveLive(*m_world, childId);
            if (child == NullEntity) return false;

            // parent may legitimately be 0 / none: that is the detach request.
            Entity parent = NullEntity;
            if (parentId.raw() != 0)
            {
                parent = LuaId::ResolveLive(*m_world, parentId);
                if (parent == NullEntity) return false;
                if (parent == child) return false;

                // Cycle guard: walk UP from the prospective parent; if we meet
                // the child, this edge would close a loop.
                Entity walk  = parent;
                int    guard = 0;
                while (walk != NullEntity && ++guard < 4096)
                {
                    if (walk == child) return false;
                    const Parent* p = m_world->GetComponent<Parent>(walk);
                    walk = p ? p->entity : NullEntity;
                }
            }

            // 1. Preserve the world pose by rewriting LocalTransform.
            if (keepWorld.value_or(true))
            {
                LocalTransform* lt = m_world->GetComponent<LocalTransform>(child);
                if (lt)
                {
                    // COMPOSE the child's world matrix from its LocalTransform and its EXISTING parent;
                    // do NOT read the child's own GlobalTransform. An entity created this frame has an
                    // identity GlobalTransform (nothing has propagated since CreateEntityAt), so reading
                    // it computes I * inv(parentWorld) and parks the child at the world origin - in the
                    // common create, place, parent sequence, and visible only to a position probe.
                    XMMATRIX worldM = lt->ToMatrix();
                    if (const Parent* cp = m_world->GetComponent<Parent>(child))
                        if (cp->entity != NullEntity && m_world->IsAlive(cp->entity))
                            if (const GlobalTransform* opGT =
                                    m_world->GetComponent<GlobalTransform>(cp->entity))
                                worldM = XMMatrixMultiply(worldM, XMLoadFloat4x4(&opGT->matrix));

                    XMMATRIX newLocalM = worldM;   // root -> local == world
                    if (parent != NullEntity)
                    {
                        // The NEW parent's GlobalTransform is trusted as-is:
                        // re-parenting under an entity that was itself created
                        // this frame is the one case this cannot fix, and it
                        // needs a propagate pass, not more arithmetic here.
                        if (const GlobalTransform* pGT = m_world->GetComponent<GlobalTransform>(parent))
                        {
                            XMVECTOR det;
                            const XMMATRIX pInv = XMMatrixInverse(&det, XMLoadFloat4x4(&pGT->matrix));
                            // Degenerate parent (zero scale): keep the current LT, valid even if it snaps.
                            if (XMVectorGetX(det) != 0.0f) newLocalM = XMMatrixMultiply(worldM, pInv);
                        }
                    }
                    XMVECTOR t, r, s;
                    if (XMMatrixDecompose(&s, &r, &t, newLocalM))
                    {
                        XMStoreFloat3(&lt->translation, t);
                        XMStoreFloat4(&lt->rotation,    r);
                        XMStoreFloat3(&lt->scale,       s);
                        // Keep GlobalTransform consistent within THIS frame:
                        // the propagate pass may already have run, and the
                        // caller can legitimately read GetWorldPosition next.
                        if (GlobalTransform* gt = m_world->GetComponent<GlobalTransform>(child))
                            XMStoreFloat4x4(&gt->matrix, worldM);
                    }
                }
            }

            // 2. Unhook from the old parent's Children list.
            Parent*      oldP      = m_world->GetComponent<Parent>(child);
            const Entity oldParent = oldP ? oldP->entity : NullEntity;
            if (oldParent != NullEntity && m_world->IsAlive(oldParent))
            {
                if (Children* oldCh = m_world->GetComponent<Children>(oldParent))
                {
                    auto& v = oldCh->entities;
                    v.erase(std::remove(v.begin(), v.end(), child), v.end());
                }
            }

            // 3/4. Write the new edge and hook into the new Children list. The parent may have
            // no Children component yet (only .iscn nodes are guaranteed one): create it, or
            // GetChildren would never report this child.
            if (parent == NullEntity)
            {
                if (oldP) m_world->RemoveComponent<Parent>(child);
            }
            else
            {
                if (oldP) oldP->entity = parent;
                else      m_world->AddComponent<Parent>(child, Parent{ parent });

                Children* ch = m_world->GetComponent<Children>(parent);
                if (!ch)
                {
                    m_world->AddComponent<Children>(parent, Children{});
                    ch = m_world->GetComponent<Children>(parent);
                }
                if (ch && std::find(ch->entities.begin(), ch->entities.end(), child) == ch->entities.end())
                    ch->entities.push_back(child);
            }
            return true;
        });

    engine.set_function("FindByTag",
        [this](const std::string& tag) -> LuaEntity
        {
            if (!m_world) return {};
            Entity found = NullEntity;
            m_world->ForEach<TagComponent>([&](Entity e, TagComponent& tc)
            {
                if (found == NullEntity && tc.Has(tag.c_str())) found = e;
            });
            return LuaId::OutMaybe(m_world, found);
        });

    // Returns a Lua array {id, id, ...} of every entity carrying `tag`
    // (empty table if none). The InteractionSystem uses this to enumerate all
    // "Interactable" objects each frame.
    engine.set_function("FindAllByTag",
        [this](const std::string& tag) -> sol::table
        {
            sol::table out = m_lua->create_table();
            if (!m_world) return out;
            int i = 0;
            m_world->ForEach<TagComponent>([&](Entity e, TagComponent& tc)
            {
                if (tc.Has(tag.c_str())) out[++i] = LuaId::Out(*m_world, e);
            });
            return out;
        });

    // Resolve a stable cross-session Guid (36-char UUID string) to its live entity; 0
    // when unparseable or unregistered. Backed by the process-global GuidRegistry: only
    // entities with a GuidComponent are findable (the .iscn serializer stamps those on
    // saved cross-refs).
    engine.set_function("FindByGuid",
        [this](const std::string& guidStr) -> LuaEntity
        {
            const ECS::Guid g = ECS::Guid::Parse(guidStr);
            if (!g.IsValid()) return {};
            return LuaId::OutMaybe(m_world, ECS::GuidRegistry::Get().Find(g));
        });

    // ---- Save-game persistence marker ----
    // Marks a RUNTIME-created entity (e.g. Engine.SpawnPrefab loot) save-persistent:
    // PersistentTag + RuntimeSpawnedTag + a stamped Guid. The save's `spawned` section
    // re-instantiates it from its PrefabSourcePath on load; entities without one
    // (Physics.SpawnBall, bare CreateEntity) cannot be recreated and are warned at save
    // time. Scene-authored entities get PersistentTag in the editor instead.
    engine.set_function("MakePersistent",
        [this](LuaEntity id) -> bool
        {
            if (!m_world) return false;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return false;
            m_world->AddComponent<PersistentTag>(e, {});
            m_world->AddComponent<RuntimeSpawnedTag>(e, {});
            return ECS::EnsureGuidOn(*m_world, e).IsValid();
        });

    // ---- Hierarchy traversal ----
    // Prefab-friendly wiring: a trigger child reaches its root brain (GetParent) and a
    // root script resolves named parts of its own instance (FindChildByName) without
    // scene-unique names; FindByName breaks once a prefab is instantiated twice.
    engine.set_function("GetParent",
        [this](LuaEntity id) -> LuaEntity
        {
            if (!m_world) return {};
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return {};
            const Parent* p = m_world->GetComponent<Parent>(e);
            return LuaId::OutMaybe(m_world, p ? p->entity : NullEntity);
        });

    // Returns a Lua array {id, id, ...} of the entity's IMMEDIATE children
    // (empty table if none).
    engine.set_function("GetChildren",
        [this](LuaEntity id) -> sol::table
        {
            sol::table out = m_lua->create_table();
            if (!m_world) return out;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return out;
            int i = 0;
            if (const Children* kids = m_world->GetComponent<Children>(e))
                for (Entity c : kids->entities)
                    if (m_world->IsAlive(c)) out[++i] = LuaId::Out(*m_world, c);
            return out;
        });

    // The entity's world-space AABB, the box every culling consumer reads (SceneBVH
    // refit, the skinned CPU frustum cull, the GPU mesh-AABB publish), so a scenario can
    // assert bounds directly ("a rest-pose character's box spans the body"). nil when the
    // entity is dead or has no WorldAabb.
    engine.set_function("GetWorldAabb",
        [this](LuaEntity id) -> sol::object
        {
            if (!m_world || !m_lua) return sol::lua_nil;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return sol::lua_nil;
            const WorldAabb* wa = m_world->GetComponent<WorldAabb>(e);
            if (!wa) return sol::lua_nil;
            return sol::make_object(*m_lua, m_lua->create_table_with(
                "minX", wa->min.x, "minY", wa->min.y, "minZ", wa->min.z,
                "maxX", wa->max.x, "maxY", wa->max.y, "maxZ", wa->max.z));
        });

    // Depth-first search of the DESCENDANT subtree (root itself excluded) for
    // the first entity whose name matches. Visited-set guarded so a malformed
    // cyclic Children graph terminates instead of hanging the script phase.
    engine.set_function("FindChildByName",
        [this](LuaEntity rootId, const std::string& name) -> LuaEntity
        {
            if (!m_world || name.empty()) return {};
            const Entity root = LuaId::ResolveLive(*m_world, rootId);
            if (root == NullEntity) return {};

            std::vector<Entity>        stack;
            std::unordered_set<Entity> visited;
            stack.push_back(root);
            visited.insert(root);
            while (!stack.empty())
            {
                const Entity cur = stack.back();
                stack.pop_back();
                if (cur != root && m_world->GetName(cur) == name)
                    return LuaId::Out(*m_world, cur);
                // Children pushed in reverse so they POP in declaration order: "first match" is the
                // first sibling subtree, like the PrefabSerializer DFS.
                if (const Children* kids = m_world->GetComponent<Children>(cur))
                    for (auto rit = kids->entities.rbegin(); rit != kids->entities.rend(); ++rit)
                        if (m_world->IsAlive(*rit) && visited.insert(*rit).second)
                            stack.push_back(*rit);
            }
            return {};
        });

    // Engine.FindUIChildByName(root, name) -> entity (0 when not found)
    // The Canvas twin of FindChildByName; separate because the hierarchies are different
    // graphs: a Canvas node has no LocalTransform and is in no Children list (its edge is
    // UIParent), so FindChildByName finds nothing in a UI subtree - the lookup a spawned
    // row template needs ("the Label inside this instance"). BuildUIChildLists inverts
    // UIParent and the DFS runs in siblingIndex order, so "first match" agrees with the
    // template's authored order.
    engine.set_function("FindUIChildByName",
        [this](LuaEntity rootId, const std::string& name) -> LuaEntity
        {
            if (!m_world || name.empty()) return {};
            const Entity root = LuaId::ResolveLive(*m_world, rootId);
            if (root == NullEntity) return {};

            const auto kids = BuildUIChildLists(*m_world);

            std::vector<Entity>        stack{ root };
            std::unordered_set<Entity> visited{ root };
            while (!stack.empty())
            {
                const Entity cur = stack.back();
                stack.pop_back();
                if (cur != root && m_world->GetName(cur) == name)
                    return LuaId::Out(*m_world, cur);
                const auto it = kids.find(cur);
                if (it == kids.end()) continue;
                // Reverse-push so children POP in sibling order.
                for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit)
                    if (visited.insert(*rit).second) stack.push_back(*rit);
            }
            return {};
        });

    // Engine.DestroyUISubtree(root) -> number of entities destroyed
    // Engine.DestroyEntity also covers UIParent (via Hierarchy::DestroySubtree); this one
    // returns the count and is the counterpart to SpawnUIPrefab. World::DestroyEntity is
    // flat (exactly the entity given), so the walk here collects the subtree itself.
    // Honours DestroyEntity's deferral window: a destroy from inside physics contact
    // dispatch or a BT tick would be a use-after-free in the caller's stack.
    engine.set_function("DestroyUISubtree",
        [this](LuaEntity rootId) -> int
        {
            if (!m_world) { LOG_WARNING("Engine.DestroyUISubtree called outside Update"); return 0; }
            const Entity root = LuaId::ResolveLive(*m_world, rootId);
            if (root == NullEntity) return 0;

            const auto kids = BuildUIChildLists(*m_world);

            // Collect first, destroy after: mutating the world while walking the
            // map it was built from is the obvious way to get this wrong.
            std::vector<Entity>        order;
            std::vector<Entity>        stack{ root };
            std::unordered_set<Entity> visited{ root };
            while (!stack.empty())
            {
                const Entity cur = stack.back();
                stack.pop_back();
                order.push_back(cur);
                const auto it = kids.find(cur);
                if (it == kids.end()) continue;
                for (Entity c : it->second)
                    if (visited.insert(c).second) stack.push_back(c);
            }

            // Deepest last in `order`, so destroy in reverse: a parent is never
            // freed while a live child still points at it through UIParent.
            for (auto rit = order.rbegin(); rit != order.rend(); ++rit)
            {
                const Entity e = *rit;
                if (!m_world->IsAlive(e)) continue;
                if (m_deferDestroyDepth > 0)
                {
                    const uint32_t gen = m_world->GetGeneration(e) & LuaId::kGenMask;
                    bool queued = false;
                    for (const PendingDestroy& p : m_pendingDestroys)
                        if (p.e == e && p.gen == gen) { queued = true; break; }
                    if (!queued) m_pendingDestroys.push_back({ e, gen });
                }
                else
                {
                    m_world->DestroyEntity(e);
                }
            }
            return static_cast<int>(order.size());
        });

    // ---- Game time scale ----
    // The legacy factor: one value every caller shares, so a save / set / restore pair from two
    // scripts can overwrite each other. Time.RequestScale is the composing form.
    engine.set_function("SetTimeScale",
        [this](float s) { SetTimeScale(s); });
    // What Engine.SetTimeScale set (not the effective scale: Time.Scale), so a save / restore
    // pair around it restores only its own factor.
    engine.set_function("GetTimeScale",
        [this]() -> float { return GetLegacyTimeScale(); });

    // Time.RequestScale and one entity's time (ScriptSystem_Time.cpp).
    RegisterTimeBindings();


    // ---- Lua event bus ----
    engine.set_function("Subscribe",
        [this](const std::string& name, sol::protected_function fn) -> uint64_t
        {
            if (!fn.valid()) { LOG_WARNING("Engine.Subscribe: invalid callback"); return 0ull; }
            // Removed with the script that subscribed (its scene, System or entity).
            return m_luaBus->Subscribe(name, std::move(fn), m_currentOwner);
        });

    engine.set_function("Unsubscribe",
        [this](uint64_t id) -> bool
        {
            return m_luaBus->Unsubscribe(id);
        });

    engine.set_function("Publish",
        [this](const std::string& name, sol::object payload)
        {
            sol::table t;
            if (payload.is<sol::table>())
                t = payload.as<sol::table>();
            else
                t = m_lua->create_table();
            m_luaBus->Publish(name, std::move(t));
        });

    // ---- World-space transform reads ----
    // Decompose the GlobalTransform that TransformSystem computes each frame, so a script
    // on a parented entity (prefab child, socket attachment, UI-follow target) gets its
    // world pose without walking the Parent chain. GlobalTransform is written in the same
    // frame BEFORE the script phase, so this is current, unlike the Renderer-sourced
    // Engine.GetCameraPosition, which lags a frame.
    LuaDoc::Fn(engine, "Engine", "GetWorldPosition",
        [this](LuaEntity id) -> sol::object
        {
            if (!m_world) return sol::lua_nil;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* g = e != NullEntity
                          ? m_world->GetComponent<GlobalTransform>(e) : nullptr;
            if (!g) return sol::lua_nil;
            // Row-major XMFLOAT4X4: translation is row 3.
            return sol::make_object(*m_lua,
                LuaVec3{ g->matrix._41, g->matrix._42, g->matrix._43 });
        },
            { .doc = "The entity's world position from GlobalTransform, current this frame; nil when it has none.", .params = { { .name = "id" } }, .returns = "Vec3?", .context = LuaDoc::Context::Update });

    engine.set_function("GetWorldRotation",
        [this](LuaEntity id) -> sol::object
        {
            if (!m_world) return sol::lua_nil;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* g = e != NullEntity
                          ? m_world->GetComponent<GlobalTransform>(e) : nullptr;
            if (!g) return sol::lua_nil;
            DirectX::XMVECTOR s, r, t;
            if (!DirectX::XMMatrixDecompose(&s, &r, &t,
                                            DirectX::XMLoadFloat4x4(&g->matrix)))
                return sol::lua_nil;     // non-affine (mirrored/degenerate scale)
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q, r);
            return sol::make_object(*m_lua, LuaQuat{ q.x, q.y, q.z, q.w });
        });

    // One boundary crossing for the common scripted oriented-trigger query.
    // Matches GetWorldRotation():Conjugate() * (point - GetWorldPosition()),
    // divided by the clamped absolute LOCAL scale used by existing door scripts.
    // Return valid, inside, localZ; invalid/dead entities never dereference a proxy.
    engine.set_function("TestLocalTriggerBox",
        [this](LuaEntity id, const LuaVec3& point, const LuaVec3& center,
               const LuaVec3& half) -> std::tuple<bool, bool, double>
        {
            if (!m_world) return { false, false, 0.0 };
            const Entity e = LuaId::ResolveLive(*m_world, id);
            const auto* g = e != NullEntity ? m_world->GetComponent<GlobalTransform>(e) : nullptr;
            const auto* local = e != NullEntity ? m_world->GetComponent<LocalTransform>(e) : nullptr;
            if (!g || !local) return { false, false, 0.0 };
            DirectX::XMVECTOR scale, rotation, translation;
            if (!DirectX::XMMatrixDecompose(&scale, &rotation, &translation,
                                            DirectX::XMLoadFloat4x4(&g->matrix)))
                return { false, false, 0.0 };
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q, rotation);
            const LuaVec3 delta = point - LuaVec3{ g->matrix._41, g->matrix._42, g->matrix._43 };
            const LuaVec3 v = LuaQuat{ q.x, q.y, q.z, q.w }.Conjugate().Rotate(delta);
            const double x = v.x / (std::max)(std::abs(double(local->scale.x)), 0.001);
            const double y = v.y / (std::max)(std::abs(double(local->scale.y)), 0.001);
            const double z = v.z / (std::max)(std::abs(double(local->scale.z)), 0.001);
            return { true, std::abs(x-center.x) <= half.x &&
                           std::abs(y-center.y) <= half.y &&
                           std::abs(z-center.z) <= half.z, z };
        });

    // ---- Logic-instance baseline access (entity ID required) ----
    // The per-entity baseline transform captured the first time the script ran:
    //     local base = Engine.GetBasePosition(self.entity)
    engine.set_function("GetBasePosition",
        [this](LuaEntity id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(LuaId::ResolveLive(*m_world, id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& p = it->second.baseTransform.translation;
                t["x"] = p.x; t["y"] = p.y; t["z"] = p.z;
            } else {
                t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f;
            }
            return t;
        });
    engine.set_function("GetBaseRotation",
        [this](LuaEntity id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(LuaId::ResolveLive(*m_world, id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& r = it->second.baseTransform.rotation;
                t["x"] = r.x; t["y"] = r.y; t["z"] = r.z; t["w"] = r.w;
            } else {
                t["x"] = 0.f; t["y"] = 0.f; t["z"] = 0.f; t["w"] = 1.f;
            }
            return t;
        });
    engine.set_function("GetBaseScale",
        [this](LuaEntity id) -> sol::table
        {
            sol::table t = m_lua->create_table();
            const auto it = m_states.find(LuaId::ResolveLive(*m_world, id));
            if (it != m_states.end() && it->second.baseCaptured) {
                const auto& s = it->second.baseTransform.scale;
                t["x"] = s.x; t["y"] = s.y; t["z"] = s.z;
            } else {
                t["x"] = 1.f; t["y"] = 1.f; t["z"] = 1.f;
            }
            return t;
        });
    engine.set_function("ResetBase",
        [this](LuaEntity id)
        {
            if (!m_world) return;
            auto it = m_states.find(LuaId::ResolveLive(*m_world, id));
            if (it == m_states.end()) return;
            if (const LocalTransform* lt =
                m_world->GetComponent<LocalTransform>(LuaId::ResolveLive(*m_world, id)))
            {
                it->second.baseTransform = *lt;
                it->second.baseCaptured  = true;
            }
        });

    // ---- System / Service / UI / Config access ----
    // The registered system / service / ui table, or nil:
    //     local Damage = Engine.GetService("Damage")
    //     local quest  = Engine.GetSystem("QuestSystem")
    //     local hud    = Engine.GetUIScript("HUD")
    engine.set_function("GetService",
        [this](const std::string& name) -> sol::object
        {
            auto it = m_services.find(name);
            if (it == m_services.end() || !it->second.loaded)
                return sol::lua_nil;
            return (*m_lua)["__services"][name];
        });

    // nil for a scene-scoped System no entered scene lists: callers must expect it.
    engine.set_function("GetSystem",
        [this](const std::string& name) -> sol::object
        {
            for (const auto& sys : m_systems)
                if (sys.name == name && sys.loaded && sys.active)
                    return (*m_lua)["__systems"][name];
            return sol::lua_nil;
        });

    LuaDoc::Fn(engine, "Engine", "SceneSystems",
        [this]() -> sol::table
        {
            sol::table out = m_lua->create_table();
            int i = 0;
            for (const std::string& n : m_sceneSystems) out[++i] = n;
            return out;
        },
        { .doc = "Names of the scene-scoped Systems the entered scene runs, in tick order.", .returns = "string[]" });

    LuaDoc::Fn(engine, "Engine", "ScriptResources",
        [this]() -> sol::table
        {
            ScriptRegistrySnapshot snap;
            FillRegistrySnapshot(snap);
            sol::table out    = m_lua->create_table();
            sol::table owners = m_lua->create_table();
            int subs = 0, timers = 0, times = 0, contexts = 0, tasks = 0;
            for (const auto& row : snap.owners)
            {
                owners[row.label] = m_lua->create_table_with(
                    "subs", row.subs, "timers", row.timers,
                    "timeRequests", row.timeRequests, "contexts", row.contexts, "tasks", row.tasks);
                subs += row.subs; timers += row.timers; times += row.timeRequests; contexts += row.contexts;
                tasks += row.tasks;
            }
            out["subs"] = subs; out["timers"] = timers; out["timeRequests"] = times; out["contexts"] = contexts;
            out["tasks"] = tasks;
            out["owners"] = owners;
            return out;
        },
        { .doc = "What scripts hold right now: live bus subscriptions, pending AfterDelay / Every timers, Engine.Run tasks, time-scale requests and pushed input contexts, in total and per owner label (system:<name>, scene:<path>, logic:<path>#<entity>.<slot>, or (none)).",
          .returns = "table" });

    engine.set_function("GetUIScript",
        [this](const std::string& name) -> sol::object
        {
            auto it = m_uiScripts.find(name);
            if (it == m_uiScripts.end() || !it->second.loaded)
                return sol::lua_nil;
            return (*m_lua)["__ui_scripts"][name];
        });

    // Pure data loader: runs the file and returns its return value as-is.
    // Caller decides whether to cache.
    LuaDoc::Fn(engine, "Engine", "LoadConfig",
        [this](const std::string& path) -> sol::object
        {
            auto result = RunScriptAsset(*m_lua, path);
            if (!result.valid()) {
                sol::error err = result;
                LOG_ERROR("Engine.LoadConfig [%s]: %s", path.c_str(), err.what());
                return sol::lua_nil;
            }
            sol::object ret = result;
            return ret;
        },
            { .doc = "Run a Lua data file through AssetFS (packaged builds too) and return what it returns, uncached; nil and a logged error when it fails.", .params = { { .name = "path" } } });

    // Engine.Import(path) -> value: a memoized module loader.
    // The VM opens no `package` library, so there is no `require`, and Engine.LoadConfig
    // re-runs its file on every call. `package` / `require` stay off on purpose: `require`
    // resolves through package.path on the filesystem, bypassing AssetFS, so it would
    // work in the editor and fail in a packed build. Import goes through RunScriptAsset
    // (pak-first, disk fallback) like every script load.
    // The file runs at most once; its return value is cached in __modules[path]. A hot
    // reload of the file clears the entry, so the next Import re-runs it (callers holding
    // the old table keep it, as with Engine.GetService).
    (*m_lua)["__modules"] = m_lua->create_table();
    LuaDoc::Fn(engine, "Engine", "Import",
        [this](const std::string& path) -> sol::object
        {
            sol::table cache = (*m_lua)["__modules"];
            sol::object hit = cache[path];
            if (hit.valid() && hit.get_type() != sol::type::lua_nil) return hit;

            auto result = RunScriptAsset(*m_lua, path);
            if (!result.valid()) {
                sol::error err = result;
                LOG_ERROR("Engine.Import [%s]: %s", path.c_str(), err.what());
                return sol::lua_nil;
            }
            sol::object ret = result;
            if (!ret.valid() || ret.get_type() == sol::type::lua_nil)
            {
                LOG_WARNING("Engine.Import [%s]: module returned nil — a module "
                            "must `return` its table (result NOT cached)",
                            path.c_str());
                return sol::lua_nil;
            }
            cache[path] = ret;
            // Track for hot reload so editing the module invalidates the cache.
            try { m_fileTimestamps[path] = fs::last_write_time(Sys::U8Path(path)); }
            catch (...) {}
            return ret;
        },
            { .doc = "Load a Lua module through AssetFS once and return its table (cached; a hot reload clears it). require / package are off on purpose: they bypass AssetFS.", .params = { { .name = "path" } }, .returns = "table?" });

    // ---- Trigger / Animation manual fire ----
    // Lua-side equivalents of the C++ event bridges. PublishTrigger is for non-physics
    // overlap detection (custom AABB queries, ...); PublishAnimEvent fires OnAnimEvent on
    // a Logic instance from gameplay code.
    engine.set_function("PublishTrigger",
        [this](LuaEntity selfId, LuaEntity otherId)
        {
            const Entity selfE = LuaId::ResolveLive(*m_world, selfId);
            auto sIt = m_states.find(selfE);
            if (sIt == m_states.end()) return;
            // Fire OnEnter on every slot whose instance defines it. A manual trigger has no
            // point / normal: pass nil (same arg shape as the physics path) so scripts can tell.
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, selfE, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnEnter"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, otherId, sol::lua_nil, sol::lua_nil);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnEnter error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), selfE, i, err.what());
                }
            }
        });

    // Exit-edge twin of PublishTrigger: fires OnExit(self, other) on the target's Logic
    // slots, so forwarding a zone exit does not re-fire OnEnter. Generation-gated
    // (ResolveLive) so a stale cached id cannot fire OnExit on whatever recycled the slot.
    engine.set_function("PublishTriggerExit",
        [this](LuaEntity selfId, LuaEntity otherId)
        {
            if (!m_world) return;
            const Entity selfE = LuaId::ResolveLive(*m_world, selfId);
            if (selfE == NullEntity) return;
            auto sIt = m_states.find(selfE);
            if (sIt == m_states.end()) return;
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, selfE, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnExit"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, otherId);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnExit error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), selfE, i, err.what());
                }
            }
        });

    engine.set_function("PublishAnimEvent",
        [this](LuaEntity entityId, const std::string& name, sol::object payload)
        {
            const Entity selfE = LuaId::ResolveLive(*m_world, entityId);
            auto sIt = m_states.find(selfE);
            if (sIt == m_states.end()) return;
            // Forward payload as-is (table, number, string, nil: the script's contract; no empty
            // table is synthesized). Fire on every slot whose instance defines OnAnimEvent.
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, selfE, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnAnimEvent"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, name, payload);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnAnimEvent error [%s] entity %u slot %zu name=%s: %s",
                              sIt->second.slots[i].path.c_str(), selfE, i,
                              name.c_str(), err.what());
                }
            }
        });

    // PublishInteract(selfId, otherId): fire OnInteract(self, other) on the target's
    // Logic instance(s). The InteractionSystem calls it when the player presses the use
    // key while focused on a tagged "Interactable". Same dispatch shape as PublishTrigger.
    engine.set_function("PublishInteract",
        [this](LuaEntity selfId, LuaEntity otherId)
        {
            const Entity selfE = LuaId::ResolveLive(*m_world, selfId);
            auto sIt = m_states.find(selfE);
            if (sIt == m_states.end()) return;
            for (std::size_t i = 0; i < sIt->second.slots.size(); ++i)
            {
                if (!sIt->second.slots[i].initCalled) continue;
                sol::table inst = GetInstance(*m_lua, selfE, i);
                if (!inst.valid()) continue;
                sol::protected_function fn = inst["OnInteract"];
                if (!fn.valid()) continue;
                OwnerScope as(*this, sIt->second.slots[i].owner);
                auto res = fn(inst, otherId);
                if (!res.valid()) {
                    sol::error err = res;
                    LOG_ERROR("Lua OnInteract error [%s] entity %u slot %zu: %s",
                              sIt->second.slots[i].path.c_str(), selfE, i, err.what());
                }
            }
        });

    // ---- Dissolve (Engine.Dissolve / DissolveIn / SetDissolveAmount) ----
    // Lua expresses intent only; DissolveSystem lerps `amount` each frame in C++ and the
    // renderer packs it per instance (tagging a character ROOT dissolves the whole
    // subtree). duration = seconds for a full sweep; destroy = true despawns the entity
    // when the dissolve-out completes.
    auto ensureDissolve = [this](Entity e) -> DissolveComponent*
    {
        DissolveComponent* d = m_world->GetComponent<DissolveComponent>(e);
        if (!d)
        {
            m_world->AddComponent<DissolveComponent>(e, DissolveComponent{});
            d = m_world->GetComponent<DissolveComponent>(e);
        }
        return d;
    };
    engine.set_function("Dissolve",
        [this, ensureDissolve](LuaEntity id, float duration,
                               sol::optional<bool> destroy)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return;
            if (DissolveComponent* d = ensureDissolve(e))
            {
                d->target            = 1.0f;
                d->duration          = duration;
                d->destroyOnComplete = destroy.value_or(false);
            }
        });
    engine.set_function("DissolveIn",
        [this, ensureDissolve](LuaEntity id, float duration)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return;
            if (DissolveComponent* d = ensureDissolve(e))
            {
                // Fresh component: start fully dissolved so the entity
                // materializes in instead of popping.
                if (d->amount <= 0.0f && d->target <= 0.0f) d->amount = 1.0f;
                d->target            = 0.0f;
                d->duration          = duration;
                d->destroyOnComplete = false;
            }
        });
    engine.set_function("SetDissolveAmount",
        [this, ensureDissolve](LuaEntity id, float amount)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            if (e == NullEntity) return;
            if (DissolveComponent* d = ensureDissolve(e))
            {
                // Direct (script-driven) control: stop any running sweep.
                d->amount = d->target = std::clamp(amount, 0.0f, 1.0f);
            }
        });

    // ---- Transform tweens (Engine.MoveTo / RotateTo / ScaleTo) ----
    // A door swing / chest lid / bobbing pickup without per-frame easing bookkeeping in
    // Lua. `easing` is optional ("linear" / "easeIn" / "easeOut" / "easeInOut" /
    // "smoothstep", default smoothstep); `onDone` is an optional callback fired at the
    // target. Re-issuing on the same (entity, channel) replaces the active tween. Advanced
    // on game dt (pauses with the game).
    engine.set_function("MoveTo",
        [this](LuaEntity id, float x, float y, float z, float duration,
               sol::optional<std::string> easing,
               sol::optional<sol::protected_function> onDone)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            LocalTransform* lt = e != NullEntity
                ? m_world->GetComponent<LocalTransform>(e) : nullptr;
            if (!lt) return;
            TweenQueue::Entry en{};
            en.entity   = e;
            en.channel  = TweenQueue::Channel::Translation;
            {
                bool easeOk = false;
                const std::string easeName = easing.value_or("smoothstep");
                en.easing = TweenQueue::ParseEasing(easeName, &easeOk);
                if (!easeOk)
                    LOG_WARNING("Engine tween: unknown easing '%s' — using smoothstep. "
                                "Valid: linear, easeIn/in, easeOut/out, easeInOut/inout, "
                                "smoothstep", easeName.c_str());
            }
            en.from     = { lt->translation.x, lt->translation.y, lt->translation.z, 0.f };
            en.to       = { x, y, z, 0.f };
            en.elapsed  = 0.f;
            en.duration = duration > 0.f ? duration : 0.0001f;
            if (onDone && onDone->valid()) en.onDone = *onDone;
            en.active   = true;
            en.owner    = m_currentOwner;
            m_tweens->Add(std::move(en));
        });

    // RotateTo(id, pitchDeg, yawDeg, rollDeg, duration[, easing][, onDone]).
    // Euler degrees -> a quaternion target, slerped from the current rotation, so a door
    // hinge is RotateTo(door, 0, 90, 0, 0.8, "easeOut").
    engine.set_function("RotateTo",
        [this](LuaEntity id, float pitchDeg, float yawDeg, float rollDeg, float duration,
               sol::optional<std::string> easing,
               sol::optional<sol::protected_function> onDone)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            LocalTransform* lt = e != NullEntity
                ? m_world->GetComponent<LocalTransform>(e) : nullptr;
            if (!lt) return;
            const XMVECTOR q = XMQuaternionRotationRollPitchYaw(
                XMConvertToRadians(pitchDeg), XMConvertToRadians(yawDeg), XMConvertToRadians(rollDeg));
            XMFLOAT4 to; XMStoreFloat4(&to, q);
            TweenQueue::Entry en{};
            en.entity   = e;
            en.channel  = TweenQueue::Channel::Rotation;
            {
                bool easeOk = false;
                const std::string easeName = easing.value_or("smoothstep");
                en.easing = TweenQueue::ParseEasing(easeName, &easeOk);
                if (!easeOk)
                    LOG_WARNING("Engine tween: unknown easing '%s' — using smoothstep. "
                                "Valid: linear, easeIn/in, easeOut/out, easeInOut/inout, "
                                "smoothstep", easeName.c_str());
            }
            en.from     = lt->rotation;
            en.to       = to;
            en.elapsed  = 0.f;
            en.duration = duration > 0.f ? duration : 0.0001f;
            if (onDone && onDone->valid()) en.onDone = *onDone;
            en.active   = true;
            en.owner    = m_currentOwner;
            m_tweens->Add(std::move(en));
        });

    engine.set_function("ScaleTo",
        [this](LuaEntity id, float x, float y, float z, float duration,
               sol::optional<std::string> easing,
               sol::optional<sol::protected_function> onDone)
        {
            if (!m_world) return;
            const Entity e = LuaId::ResolveLive(*m_world, id);
            LocalTransform* lt = e != NullEntity
                ? m_world->GetComponent<LocalTransform>(e) : nullptr;
            if (!lt) return;
            TweenQueue::Entry en{};
            en.entity   = e;
            en.channel  = TweenQueue::Channel::Scale;
            {
                bool easeOk = false;
                const std::string easeName = easing.value_or("smoothstep");
                en.easing = TweenQueue::ParseEasing(easeName, &easeOk);
                if (!easeOk)
                    LOG_WARNING("Engine tween: unknown easing '%s' — using smoothstep. "
                                "Valid: linear, easeIn/in, easeOut/out, easeInOut/inout, "
                                "smoothstep", easeName.c_str());
            }
            en.from     = { lt->scale.x, lt->scale.y, lt->scale.z, 0.f };
            en.to       = { x, y, z, 0.f };
            en.elapsed  = 0.f;
            en.duration = duration > 0.f ? duration : 0.0001f;
            if (onDone && onDone->valid()) en.onDone = *onDone;
            en.active   = true;
            en.owner    = m_currentOwner;
            m_tweens->Add(std::move(en));
        });

    engine.set_function("CancelTweens",
        [this](LuaEntity id) -> int
        {
            return m_tweens->CancelEntity(LuaId::ResolveLive(*m_world, id));
        });

    // ---- Global game state (flags / counters / inventory) ----
    // Scene-independent store (Gameplay::GameState): survives scene reload, unlike a
    // System-script Lua table. For "has key", "vault looted", coin counts.
    // Engine.ClearState() wipes it (call at New Game). Needs no live world, so these are
    // safe to call from anywhere.
    engine.set_function("SetFlag",
        [](const std::string& k, bool v) { Gameplay::GameState::Get().SetBool(k, v); });
    engine.set_function("GetFlag",
        [](const std::string& k, sol::optional<bool> def) -> bool
        { return Gameplay::GameState::Get().GetBool(k, def.value_or(false)); });
    engine.set_function("SetNumber",
        [](const std::string& k, double v) { Gameplay::GameState::Get().SetNumber(k, v); });
    engine.set_function("GetNumber",
        [](const std::string& k, sol::optional<double> def) -> double
        { return Gameplay::GameState::Get().GetNumber(k, def.value_or(0.0)); });
    engine.set_function("SetString",
        [](const std::string& k, const std::string& v) { Gameplay::GameState::Get().SetString(k, v); });
    engine.set_function("GetString",
        [](const std::string& k, sol::optional<std::string> def) -> std::string
        { return Gameplay::GameState::Get().GetString(k, def.value_or(std::string())); });
    engine.set_function("AddItem",
        [](const std::string& item, sol::optional<int> delta) -> int
        { return Gameplay::GameState::Get().AddItem(item, delta.value_or(1)); });
    engine.set_function("GetItem",
        [](const std::string& item) -> int { return Gameplay::GameState::Get().GetItem(item); });
    engine.set_function("HasItem",
        [](const std::string& item, sol::optional<int> atLeast) -> bool
        { return Gameplay::GameState::Get().HasItem(item, atLeast.value_or(1)); });
    engine.set_function("ClearState",
        []() { Gameplay::GameState::Get().ClearAll(); });

    lua["Engine"] = engine;
    RegisterTimerBindings();   // Engine.AfterDelay / Every / Run and Wait.* (ScriptSystem_Timers.cpp)

    // ---- Registry-style storage tables (per script category) ----
    // Templates and instances for Logic scripts.
    lua["__logic_templates"] = lua.create_table();
    lua["__logic_instances"] = lua.create_table();
    // Singleton instance tables for systems / services / ui scripts.
    lua["__systems"]         = lua.create_table();
    lua["__services"]        = lua.create_table();
    lua["__ui_scripts"]      = lua.create_table();
    // Scene-scoped Systems' file tables; __systems holds their current activation.
    lua["__system_protos"]   = lua.create_table();
}
