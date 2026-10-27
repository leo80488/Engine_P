#pragma once
// HotReload::WatchService - the shader and asset watchers, polled at the top of
// App::Run's loop: every changed shader goes through the pre-flighted reload and its
// receipt, every changed .itex is swapped in place, every changed .imat / .imatinst
// re-resolves the entities it feeds. What lies outside the engine core - parking
// App's render-owner lane, the editor's asset browser and thumbnail cache - arrives
// through Hooks, so this file never needs WITH_EDITOR.

#include "Graphics/ShaderHotReload.h"   // HotReload::ShaderReloader
#include "System/FileWatcher.h"         // HotReload::FileWatcher

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class World;
class Renderer;
class IGraphicsDevice;
struct GameModeContext;
namespace Resource { class TextureSystem; class ResourceManager; }

namespace HotReload
{
    // A module's asset type the asset watcher reloads (Module::IEngineModule::Initialize):
    // @p handler gets the watcher-relative path of every changed file whose extension is
    // @p ext (".ifacemap"), on main at the loop top. @p ext must outlive the process.
    using AssetHandler = void (*)(const std::string& path);
    void RegisterAssetHandler(const char* ext, AssetHandler handler);

    class WatchService
    {
    public:
        struct Hooks
        {
            // Before anything below touches renderer state the render-owner lane reads.
            std::function<void()> parkRenderLane;
            // Editor host: the thumbnail cache's dependency index (ROOT-relative paths).
            std::function<void(const std::vector<std::string>&)> thumbnailDependencyChanged;
            // Editor host: the asset browser repaints (and says so in the log).
            std::function<void(std::size_t)> externalAssetChange;
        };

        // ---- Watch prefixes ----
        // FileWatcher::PollChanges returns paths RELATIVE TO THE WATCHED DIRECTORY
        // and LOWERCASED. Everything downstream that consumes a file identity - in
        // particular PreviewThumbnailCache's dependency index - is keyed on
        // ROOT-RELATIVE paths, the form MaterialComponent stores and the loaders
        // take: "engine/textures/x.itex", "asset/Shaders/y.hlsl". These prefixes
        // put the root back; without them a texture re-import or a shader edit
        // never matches the index and its thumbnails stay stale. (The case
        // difference is fine: the index lowercases both sides.)
        static constexpr const char* kShaderWatchPrefix[2] = { "shaders/", "asset/Shaders/" };
        static constexpr const char* kAssetWatchPrefix [2] = { "asset/",   "engine/" };

        WatchService(World& world, Resource::TextureSystem& textures,
                     Resource::ResourceManager& resources, Hooks hooks)
            : m_world(world), m_textureSys(textures), m_resourceMgr(resources),
              m_hooks(std::move(hooks)) {}

        // Boot: open the watchers on the loose trees (a packed build has none).
        void Start();
        // Loop top, before BeginFrame, so no in-flight command list still references
        // the old PSOs: drain every watcher and reload what changed.
        void Poll(const GameModeContext& ctx, IGraphicsDevice& backend, Renderer& renderer);

    private:
        World&                     m_world;
        Resource::TextureSystem&   m_textureSys;
        Resource::ResourceManager& m_resourceMgr;
        Hooks                      m_hooks;

        // Every tree that can hold loose .hlsl/.hlsli; on a change the in-memory
        // shader + PSO caches are dropped so every reload-aware pass re-fetches.
        //
        //   shaders/        engine shaders + the shared .hlsli headers
        //   asset/Shaders/  user-authored custom material shaders: the tree a
        //                   material's customShaderPath loads from. ReloadShaders
        //                   re-arms the persistent material slots when a reloaded
        //                   custom PS/VS changes its b8 layout.
        //
        // A packed build has no loose shader directories (they live inside
        // game.ipak) and watches neither. FileWatcher filters to .hlsl/.hlsli and
        // skips shader_cache_dxil/, so a recursive watch on either tree is silent
        // when nobody is editing.
        HotReload::FileWatcher shaderWatchers[2];
        // The watched directories themselves: the hot-reload pre-flight matches
        // ABSOLUTE paths against the shader recipes (ShaderRecipes.h), and neither
        // prefix is one.
        std::string shaderWatchRoot[2];
        // Turns what the shader watchers report into a pre-flighted reload with a
        // receipt (Graphics/ShaderHotReload.h).
        HotReload::ShaderReloader shaderReloader;

        // Asset-tree watchers (asset/ and engine/): an external DCC re-export is
        // picked up without a manual Refresh. Cooked engine formats only (a source
        // .png/.fbx change matters once its importer runs, and the importer's output
        // is one of these); .hlsl stays with the shader watchers, .lua with
        // ScriptSystem's mtime polling.
        HotReload::FileWatcher assetWatchers[2];
    };
}
