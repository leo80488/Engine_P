#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Resource/AssetManager.h"
#include "Graphics/IGraphicsDevice.h"

namespace Resource
{

    void AssetManager::Init(TextureSystem&   texSys,
                            ResourceManager& rm,
                            IGraphicsDevice& gfx)
    {
        m_texSys  = &texSys;
        m_rm      = &rm;
        m_gfx     = &gfx;
    }

    // ==== Texture passthroughs (TextureSystem owns the cache) ====

    TextureHandle AssetManager::AcquireTexture(const std::string& path)
    {
        if (!m_texSys || !m_rm || !m_gfx) return kInvalidTextureHandle;
        return m_texSys->Acquire(path, *m_rm, *m_gfx);
    }

    void AssetManager::ReleaseTexture(TextureHandle h)
    {
        if (m_texSys && m_gfx) m_texSys->Release(h, *m_gfx);
    }

    bool AssetManager::IsTextureReady(TextureHandle h) const
    {
        return m_texSys ? m_texSys->IsReady(h) : false;
    }

    const RHI::Texture* AssetManager::GetTexture(TextureHandle h) const
    {
        return m_texSys ? m_texSys->GetTexture(h) : nullptr;
    }

    // ==== Per-frame / lifecycle ====

    void AssetManager::Tick()
    {
        if (m_texSys && m_rm && m_gfx)
            m_texSys->Tick(*m_rm, *m_gfx);
    }

    void AssetManager::Shutdown()
    {
        // Nothing to release: MeshLibrary and TextureSystem own their cleanup.
    }

} // namespace Resource
