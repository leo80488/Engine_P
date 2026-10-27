#include "UI/LuaUIEngineBindings.h"

#include "ECS/ECS.h"
#include "UI/UICanvas.h"              // UI::AttachUIParent
#include "UI/UICanvasSystem.h"
#include "Renderer/Renderer.h"
#include "Resource/AssetManager.h"
#include "Resource/AnimationClipSystem.h"
#include "Resource/PrefabSerializer.h" // Resource::LoadPrefab
#include "Resource/PrefabSpawn.h"      // Resource::SpawnPrefab (Engine.SpawnUIPrefab is a runtime spawn)
#include "System/Log.h"
#include "System/PathUtil.h"           // Sys::RelativizeAssetPath (Engine.SpawnUI)
#include "UI/UIAsset.h"                // Engine.SpawnUI / CaptureUI / FindUINode
#include "UI/UIDocument.h"             // Engine.UIDocumentRoot

#include <sol/sol.hpp>

#include "Scripting/LuaEntityId.h"     // LuaEntity + LuaId::* (needs sol first)
#include "Scripting/LuaDoc.h"

#include <cstdio>
#include <string>
#include <tuple>

void UI::RegisterLuaUIEngineBindings(sol::state& state, World& world, UICanvasSystem& uiCanvasSystem,
                                     const UIInputState& uiInput,
                                     Resource::AssetManager& assetMgr, Renderer& renderer,
                                     Resource::AnimationClipSystem& animClipSys)
{
    sol::state* lua = &state;
    sol::table engineTbl = state["Engine"];

    // Engine.SpawnUIPrefab(path, parentEntity) -> rootEntity
    //
    // The Canvas twin of SpawnPrefab, a separate function rather than a flag
    // because the two differ in both halves:
    //   * UI nodes have NO transform, so there is no position to set: the
    //     rect comes from the authored UIRect and the parent's rect.
    //   * The hierarchy edge is UIParent, not Children, and it must be
    //     RE-POINTED at the caller's parent. The baked parentGuid is remapped
    //     per instance, but it still refers to whatever the template was
    //     authored UNDER - the wrong container for every row instance after
    //     the first.
    // The workflow: author one row in the editor, instantiate it per data
    // item, then bind values into the named children.
    engineTbl.set_function("SpawnUIPrefab",
        [&world, &assetMgr, &renderer, &animClipSys](const std::string& path, LuaEntity parent) -> LuaEntity
        {
            Entity root = Resource::SpawnPrefab(
                path, world, assetMgr, &renderer, &animClipSys, nullptr, "Engine.SpawnUIPrefab");
            if (root == NullEntity) return {};

            const Entity p = parent.raw() == NullEntity
                           ? NullEntity : LuaId::ResolveLive(world, parent);
            if (parent.raw() != NullEntity && p == NullEntity)
            {
                LOG_WARNING("Engine.SpawnUIPrefab('%s'): parent is not alive — "
                            "the instance keeps the parent it was authored under",
                            path.c_str());
            }
            else if (p != NullEntity)
            {
                // AttachUIParent, never a raw UIParent write: it stamps the
                // parent's GUID, without which the edge does not survive a
                // save/load of the scene this instance now lives in.
                UI::AttachUIParent(world, root, p);
            }
            return LuaId::Out(world, root);
        });

    // Engine.SpawnUI(path, parent [, {seed = "<32 hex>"}]) -> rootEntity
    //
    // A .iui UI asset (UI/UIAsset.h) spawned through the scene loader's own Cv*
    // deserializers - no transform, no prefab instance, every node's GUID derived
    // from (the asset, its id, a fresh seed), so two stamps never tangle. A rect
    // root goes LAST under `parent`; a canvas asset is a root of its own. Each node
    // keeps its file id: Engine.FindUINode. A file that does not load is an ERROR
    // in the log - a missing template must not pass quietly - and the result is
    // the empty id, as SpawnUIPrefab's.
    LuaDoc::Fn(engineTbl, "Engine", "SpawnUI",
        [&world, &assetMgr](const std::string& path, sol::object parentObj, sol::optional<sol::table> opts) -> LuaEntity
        {
            Entity parent = NullEntity;
            if (parentObj.valid() && parentObj.get_type() != sol::type::lua_nil)
            {
                if (!parentObj.is<LuaEntity>())
                {
                    LOG_ERROR("Engine.SpawnUI('%s'): the parent is not an entity id", path.c_str());
                    return {};
                }
                parent = LuaId::ResolveLive(world, parentObj.as<LuaEntity>());
                if (parent == NullEntity)
                {
                    LOG_ERROR("Engine.SpawnUI('%s'): the parent is not alive", path.c_str());
                    return {};
                }
            }
            std::string why;
            const std::shared_ptr<const UI::UIAsset> asset = UI::LoadUIAsset(path, &why);
            if (!asset)
            {
                LOG_ERROR("Engine.SpawnUI('%s'): %s", path.c_str(), why.c_str());
                return {};
            }
            UI::UISpawnOptions o;
            o.parent   = parent;
            o.assetKey = Sys::RelativizeAssetPath(path);
            o.assets   = &assetMgr;
            if (opts)
                if (const sol::optional<std::string> seed = opts->get<sol::optional<std::string>>("seed"))
                    o.seed = ECS::Guid::FromHex(*seed);
            const Entity root = UI::SpawnUIAsset(world, *asset, o, &why);
            if (root == NullEntity)
            {
                LOG_ERROR("Engine.SpawnUI('%s'): %s", path.c_str(), why.c_str());
                return {};
            }
            return LuaId::Out(world, root);
        },
        { .doc = "Spawn a .iui UI asset: a rect root goes last under parent (a canvas or a UI node; nil = unattached), "
                 "a canvas asset is a root of its own. Node GUIDs derive from (asset, node id, seed); nodes keep their "
                 "file id for Engine.FindUINode. A file that does not load logs an ERROR and returns the empty id.",
          .params = { { "path", "a .iui path" },
                      { "parent", "a canvas or UI node id; nil = unattached", nullptr, true },
                      { "opts", "{seed = 32 hex digits}: reproducible node GUIDs", nullptr, true } },
          .returns = "EntityId", .effect = LuaDoc::Effect::World, .context = LuaDoc::Context::Any });

    // Engine.CaptureUI(root) -> string | nil, why
    //
    // The canonical .iui text of a UI subtree in the world (a canvas or any UI node and
    // everything under it), each node's Cv* components through their serializers - what
    // a .iui of it would hold. Ids: a SpawnUI node keeps its own, others are made from the
    // name. With Engine.SpawnUI it round-trips: capture, spawn, capture = the same text.
    LuaDoc::Fn(engineTbl, "Engine", "CaptureUI",
        [&world](sol::this_state ts, LuaEntity root) -> std::tuple<sol::object, sol::object>
        {
            const Entity r = LuaId::ResolveLive(world, root);
            UI::UIAsset asset;
            std::string why = "the entity is not alive";
            if (r == NullEntity || !UI::CaptureUIAsset(world, r, asset, &why))
                return { sol::make_object(ts, sol::lua_nil), sol::make_object(ts, why) };
            // A .iui (v1) holds no references: a UI Document's instance is written inline.
            if (const std::size_t docs = UI::DocumentNodesUnder(world, r))
                LOG_WARNING("Engine.CaptureUI: %zu node(s) of UI Documents under '%s' written inline (a .iui holds no "
                            "reference)", docs, world.GetName(r).c_str());
            return { sol::make_object(ts, UI::WriteUIAsset(asset)), sol::make_object(ts, sol::lua_nil) };
        },
        { .doc = "The canonical .iui text of a UI subtree (a canvas or any UI node), every node's Cv* components "
                 "through their serializers; nil and why when root is no UI node (or past a .iui's limits: 64 levels, "
                 "4096 nodes). Round-trips with Engine.SpawnUI. UI Document instances in it are written inline.",
          .params = { { "root", "a canvas or UI node id" } },
          .returns = "string|nil, string?", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Any });

    // Engine.FindUINode(root, id) -> entity | nil
    //
    // The node of a SpawnUI instance whose .iui id is `id` - an id survives a rename of
    // the node, which a name lookup does not.
    LuaDoc::Fn(engineTbl, "Engine", "FindUINode",
        [&world](sol::this_state ts, LuaEntity root, const std::string& id) -> sol::object
        {
            const Entity r = LuaId::ResolveLive(world, root);
            const Entity e = r == NullEntity ? NullEntity : UI::FindUINode(world, r, id);
            if (e == NullEntity) return sol::make_object(ts, sol::lua_nil);
            return sol::make_object(ts, LuaId::Out(world, e));
        },
        { .doc = "The node of a Engine.SpawnUI instance under root whose .iui id is id (the root itself included); "
                 "nil when there is none.",
          .params = { { "root", "an instance's root" }, { "id", "a node id of its .iui" } },
          .returns = "EntityId|nil", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Any });

    // Engine.UIDocumentRoot(host) -> entity | nil, why
    //
    // The root of the instance of @p host's UI Document (CvDoc: the .iui the scene
    // references) - Engine.FindUINode finds its nodes by id. Made by the frame's UI
    // update (before scripts), so a script sees it on the frame the host appears;
    // nil and why when there is none (no CvDoc, no path, a file that does not load
    // or does not fit its host).
    LuaDoc::Fn(engineTbl, "Engine", "UIDocumentRoot",
        [&world](sol::this_state ts, LuaEntity host) -> std::tuple<sol::object, sol::object>
        {
            const Entity h = LuaId::ResolveLive(world, host);
            const UI::UIDocument* d = h != NullEntity ? world.GetComponent<UI::UIDocument>(h) : nullptr;
            if (!d) return { sol::make_object(ts, sol::lua_nil), sol::make_object(ts, std::string("no UI Document")) };
            if (d->root == NullEntity || !world.IsAlive(d->root))
                return { sol::make_object(ts, sol::lua_nil),
                         sol::make_object(ts, d->error.empty() ? std::string("no instance yet") : d->error) };
            return { sol::make_object(ts, LuaId::Out(world, d->root)), sol::make_object(ts, sol::lua_nil) };
        },
        { .doc = "The root of a UI Document host's instance (the .iui its CvDoc references, spawned at run time); "
                 "nil and why when there is none.",
          .params = { { "host", "an entity with a CvDoc" } },
          .returns = "EntityId|nil, string?", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Any });

    // Engine.ResolveUILayout() -> bool
    //
    // Lay out every Canvas subtree now instead of at the engine's next resolve.
    // Layout output (UIComputedRect) is produced before the script phase and
    // again after it, so a script that builds widgets and immediately asks about
    // them (Engine.SetUIFocus refuses a node with no valid rect,
    // Engine.GetUIComputedRect returns nil) gets nothing back on that tick; call
    // this once after building a screen. Cheap to repeat: it is the same
    // fingerprint-gated resolve the frame runs, so a redundant call is a hash
    // compare. It does NOT run interaction, so it cannot consume a click.
    engineTbl.set_function("ResolveUILayout",
        [&uiCanvasSystem, &world]() -> bool { return uiCanvasSystem.ResolveLayout(world); });

    // Engine.UIPaintStats() -> { verts, indices, cmds, effects, paints, hash }
    //
    // The draw-list totals of the last Canvas paint (the drawables only, before
    // the debug wireframe): what a pack pins to prove a change to the layout or
    // paint code produced the same geometry (ui_canvas_stress). `hash` is the
    // list's UIDrawList::ContentHash as 16 hex digits (a double cannot hold 64
    // bits) once Engine.UIPaintHash(true) asked for it, else "".
    LuaDoc::Fn(engineTbl, "Engine", "UIPaintStats",
        [&uiCanvasSystem, lua]() -> sol::table
        {
            const auto& s = uiCanvasSystem.LastPaintStats();
            sol::table t = lua->create_table();
            t["verts"]   = s.verts;
            t["indices"] = s.indices;
            t["cmds"]    = s.cmds;
            t["effects"] = s.effects;
            t["paints"]  = static_cast<double>(s.paints);
            char hex[20] = "";
            if (uiCanvasSystem.PaintHashEnabled() && s.hash != 0)
                std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(s.hash));
            t["hash"] = std::string(hex);
            return t;
        },
        { .doc = "Draw-list totals of the last Canvas UI paint (drawables only, before the debug wireframe); "
                 "hash = its content hash in hex once Engine.UIPaintHash(true) turned hashing on, else \"\".",
          .returns = "table", .effect = LuaDoc::Effect::Read, .context = LuaDoc::Context::Any });

    // Engine.UIPaintHash(on) -> bool (the previous setting)
    //
    // Hash every Canvas paint from now on (UIPaintStats().hash): a walk over every
    // vertex, so off by default. The UI Editor's parity pack compares it with a
    // designer view's design-pixel hash (Editor.UIEditor().tabs[i].designHash).
    LuaDoc::Fn(engineTbl, "Engine", "UIPaintHash",
        [&uiCanvasSystem](bool on) -> bool
        {
            const bool was = uiCanvasSystem.PaintHashEnabled();
            uiCanvasSystem.SetPaintHash(on);
            return was;
        },
        { .doc = "Turn the content hash of every Canvas UI paint on or off (Engine.UIPaintStats().hash); "
                 "returns the previous setting.",
          .params = { { "on", "true: hash every paint from now on; false: stop" } },
          .returns = "boolean", .effect = LuaDoc::Effect::Settings, .context = LuaDoc::Context::Any });

    // Engine.GetUIPointer() -> { x, y, left, right, down, up, wheel }
    //
    // The pointer in CANVAS space: the exact snapshot UICanvasSystem hit-tested
    // with this frame. Use it, never Input.GetMousePos, for UI maths (slider
    // drags, drag-and-drop): GetMousePos is GetCursorPos +
    // ScreenToClient(GetForegroundWindow()), a DIFFERENT space whenever the
    // editor viewport is not at the window origin. `down`/`up` are the same
    // one-frame edges the interaction pass uses.
    // World-free (reads App's own per-frame snapshot), so unlike most Engine.*
    // functions it is safe to call at any time.
    engineTbl.set_function("GetUIPointer",
        [&uiInput, lua]() -> sol::table {
            return lua->create_table_with(
                "x",     uiInput.mousePos.x,
                "y",     uiInput.mousePos.y,
                "left",  uiInput.mouseLeft,
                "right", uiInput.mouseRight,
                "down",  uiInput.mouseDown,
                "up",    uiInput.mouseUp,
                // Wheel notches this frame, positive = away from the user.
                // Read from the ungated physical snapshot, so it is available
                // exactly when the UI owns the pointer: when a scroll view needs
                // it and the gameplay wheel path drops it.
                "wheel", uiInput.wheelDelta.y);
        });
}
