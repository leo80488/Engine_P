#pragma once
// Renderer/AssetQuery.h - the Lua `Asset` table: what the running engine holds of an asset.
//
// The asset catalog (tools/asset_tool.py) answers from the disk: what exists, what references
// what, which references are broken. This answers from the process, which the catalog cannot:
// is the asset loaded, is it ready or why did it fail, and which entities does the engine
// resolve it for - "the preview showed it" is not "the scene's renderer consumed it".
//
//   Asset.get(path) -> {path, type, exists, guid?, format = {kind, magic, version, current,
//                       minimum, status, reason?}?, resource = {state, reason?}?,
//                       texture = {state, reason?, width, height, refs}?,
//                       material = {cached}?, users = {ids}, usersTotal}
//       format: the file's own header (Resource::kAssetFormats), read without loading it:
//       status is "current", "upgradable" (older, read through the format's migration chain),
//       "newer" (written by a newer build: refused, reason says the version it would need) or
//       "older_than_minimum" (refused).
//       resource: the ResourceManager's entry, when it holds one - "ready" | "loading" |
//       "failed" (with the reason); it loads what the TextureSystem asks for and its like.
//       Materials, prefabs and scenes have loaders of their own: for them no entry is not "not
//       loaded". A texture's entry goes once the texture is on the GPU (only a slot that keeps
//       its CPU image, TextureSystem::AcquireWithCpuImage, keeps it): see texture for those.
//       texture (.itex): the TextureSystem's view - GPU-resident or not.
//       material (.imat / .imatinst): whether the material cache holds it.
//       users, by type: .itex - entities whose runtime material samples it (the renderer's
//       resolved view); .imat / .imatinst - entities whose MaterialSourcePath is it or whose
//       material chain reads it; .iscn / .ipfb / .meshlib - SceneSourcePath / PrefabSourcePath /
//       MeshSourcePath; .ianim - an AnimationSet entry. At most 256 ids; usersTotal counts all.
//   Asset.loaded{type = "itex", state = "failed", limit = N}
//       -> {rows = {{path, type, state, reason?}}, count, truncated}:
//       the ResourceManager's slots in use, sorted by path (an uploaded texture is not one).
//   Asset.meshes(model) -> {model, namesSkeleton, error?, meshes = {{key, name, flags, hidden,
//                           skinned, library, synthesized, collision?, sections = {{entry,
//                           key, slot, material}}}}}
//       the Meshes a model (.iscn) can place as a StaticMesh, keyed as a StaticMesh keys them.
//       It loads the model's libraries as world libraries - what placing it would do.
//       synthesized: the library has no stored identity; its keys are the positional ones a
//       backfill would write.
//   Also Asset.textures{...} (the VRAM audit), Asset.textureInfo(path) (a .itex file's header),
//   Asset.meshLibraries() and Asset.previewComposition(path): see AssetQuery.cpp.
//
// Both exes; read-only; main thread (a control-channel run, a scenario driver) - the renderer
// half drains the frame's render work first, like Render.explain.

#include <functional>

struct lua_State;
class  World;
class  Renderer;
namespace Resource { class AssetManager; }

namespace AssetQuery
{
    void RegisterLua(lua_State* L, World& world, Renderer& renderer, Resource::AssetManager& assets,
                     std::function<void()> drain);
}
