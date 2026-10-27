#pragma once

// Name-keyed pool of DecalMaterialAsset instances. Many decals (thousands of blood
// splats) share one material: same name -> same asset. Texture handles are acquired
// once per asset, not per DecalComponent, and released exactly once with it. The
// editor lists every decal material through GetAll(), referenced or not.
// Threading: main thread only. The library holds strong ownership; callers may keep
// shared_ptr or weak_ptr copies.

#include "Resource/DecalMaterialAsset.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class IGraphicsDevice;

namespace Resource
{
    class TextureSystem;

    class DecalMaterialLibrary
    {
    public:
        DecalMaterialLibrary()  = default;
        ~DecalMaterialLibrary() = default;

        DecalMaterialLibrary(const DecalMaterialLibrary&)            = delete;
        DecalMaterialLibrary& operator=(const DecalMaterialLibrary&) = delete;

        // Returns the asset for @p name, creating an empty one if absent. Its texture paths
        // are unset: the caller fills them, then asset->Resolve() acquires them (the Renderer
        // resolves every referenced asset each frame).
        std::shared_ptr<DecalMaterialAsset> GetOrCreate(const std::string& name);

        // Returns the asset for @p name, or nullptr if absent. Non-mutating.
        std::shared_ptr<DecalMaterialAsset> Find(const std::string& name) const;

        // Drops the asset named @p name and releases its texture handles. shared_ptr copies
        // held by DecalComponents keep the asset alive until the last one drops.
        void Destroy(const std::string& name, TextureSystem& texSys, IGraphicsDevice& gfx);

        // Releases every asset's texture handles and clears the pool. Call before device
        // shutdown or on full scene teardown so refcounts balance against the TextureSystem.
        void Shutdown(TextureSystem& texSys, IGraphicsDevice& gfx);

        // Editor hook: every asset, for UI enumeration.
        const std::unordered_map<std::string, std::shared_ptr<DecalMaterialAsset>>&
            GetAll() const { return m_assets; }

    private:
        std::unordered_map<std::string, std::shared_ptr<DecalMaterialAsset>> m_assets;
    };
}
