#pragma once

// AssetManager - thin facade over the texture and resource systems (it owns
// none of them): one-stop Init / Shutdown, texture calls delegated to TextureSystem
// (path dedup + async load), and raw-system accessors so the Renderer gets
// TextureSystem from one handle. Meshes live in MeshLibrary
// (Resource/MeshLibrary.h).
//
// Usage:
//   assetMgr.Init(texSys, rm, gfx);              // once at startup
//   TextureHandle th = assetMgr.AcquireTexture("stone.itex");
//   assetMgr.Tick();                              // once per frame
//   assetMgr.Shutdown();                          // before device shutdown

#include "Resource/TextureSystem.h"
#include "Resource/ResourceManager.h"
#include "Resource/SystemHandles.h"
#include "Graphics/GraphicsStruct.h"

#include <string>

class IGraphicsDevice;

namespace Resource
{
    class AssetManager
    {
    public:
        AssetManager()  = default;
        ~AssetManager() = default;

        AssetManager(const AssetManager&)            = delete;
        AssetManager& operator=(const AssetManager&) = delete;

        // Wire up the underlying systems. None are owned by AssetManager.
        void Init(TextureSystem&   texSys,
                  ResourceManager& rm,
                  IGraphicsDevice& gfx);

        // ---- Texture ----

        // Delegates to TextureSystem::Acquire (already path-deduplicates).
        TextureHandle       AcquireTexture(const std::string& path);
        void                ReleaseTexture(TextureHandle h);
        bool                IsTextureReady(TextureHandle h) const;
        const RHI::Texture* GetTexture(TextureHandle h)     const;

        // ---- Per-frame ----

        // Forward to TextureSystem::Tick (GPU promotion + deferred destroy).
        // Call once per frame, after BeginFrame.
        void Tick();

        // Releases AssetManager's own state (none: MeshLibrary and TextureSystem clean up
        // themselves). Call before TextureSystem shutdown.
        void Shutdown();

        // ---- Raw system access (for Renderer injection) ----
        TextureSystem*   GetTextureSystem()   const { return m_texSys;  }
        ResourceManager* GetResourceManager() const { return m_rm;      }
        IGraphicsDevice* GetGraphicsDevice()  const { return m_gfx;     }

    private:
        TextureSystem*   m_texSys  = nullptr;
        ResourceManager* m_rm      = nullptr;
        IGraphicsDevice* m_gfx     = nullptr;
    };
}
