#include "Graphics/MaterialSurfaceCache.h"
#include "Graphics/IGraphicsDevice.h"
#include "Diagnostics/Profiler.h"
#include "System/Log.h"

#include <algorithm>

// ==== Configuration ====

void MaterialSurfaceCache::SetEnabled(bool v)
{
    if (v == m_enabled) return;   // called every frame from the settings apply
    m_enabled = v;
    // Turning the feature OFF must revert every instance's proxy on the very
    // next gather, and turning it ON must stop the retained blob from replaying
    // the alias-path proxy forever. Both are "the publishable set changed", so
    // both ride the same epoch DDGISceneAS watches.
    ++m_publishEpoch;
    if (!m_enabled)
        ResetPages();   // drop the schedule; the atlas textures stay allocated
}

void MaterialSurfaceCache::ResetPages()
{
    for (Page& p : m_pages) p = Page{};
    m_keyToPage.clear();
    m_pagesInUse   = 0;
    m_pagesBaked   = 0;
    m_pendingBakes = 0;
}

// ==== Frame lifecycle ====

void MaterialSurfaceCache::BeginFrame()
{
#ifdef TRACY_ENABLE
    // Publish LAST frame's churn (this runs before the frame's own acquires) and
    // the epoch it produced. The epoch plot is the direct read-out of the
    // DDGISceneAS retained-gather gate's scStable term: flat = the gate can arm,
    // a per-frame staircase = a full static gather every frame.
    TracyPlot("SC.publishEpoch",   static_cast<int64_t>(m_publishEpoch));
    TracyPlot("SC.pagesAcquired",  static_cast<int64_t>(m_statAcquired));
    TracyPlot("SC.pagesEvicted",   static_cast<int64_t>(m_statEvicted));
    TracyPlot("SC.pagesPromoted",  static_cast<int64_t>(m_statPromoted));
#endif
    m_statAcquired = m_statEvicted = m_statPromoted = 0;

    ++m_frame;
    m_mappedThisFrame.clear();   // every stamp in it names the previous frame now
    m_atlasFullThisFrame = false;
    m_budgetLeft = m_enabled ? m_pagesPerFrame : 0u;
    if (!m_enabled) return;

    // Promote pages whose bake has aged past the publish lag. This is the one place `usable` can flip
    // TRUE, so it is also the one place the epoch moves for a delayed publication (MarkBaked changes
    // nothing the proxy may act on yet - see kPublishLagFrames).
    for (Page& p : m_pages)
    {
        if (!p.inUse || !p.baked || p.usable) continue;
        if (m_frame - p.bakedFrame < kPublishLagFrames) continue;
        p.usable = true;
        ++m_publishEpoch;
        ++m_statPromoted;
    }
}

void MaterialSurfaceCache::OnWorldClear()
{
    ResetPages();
    // Entity ids are recycled across worlds, so a stale stamp on a recycled id
    // could hand the new entity the old world's page for one frame. The frame
    // counter keeps advancing, so clearing the vector is the cheap correct move
    // (it is one entry per entity id, not per page).
    m_entityPage.clear();
    m_mappedThisFrame.clear();
    ++m_publishEpoch;
}

void MaterialSurfaceCache::Shutdown(IGraphicsDevice& gfx)
{
    if (m_albedoAtlas.IsValid())  gfx.DestroyTexture(m_albedoAtlas);
    if (m_surfaceAtlas.IsValid()) gfx.DestroyTexture(m_surfaceAtlas);
    m_albedoAtlas  = {};
    m_surfaceAtlas = {};
    ResetPages();
    m_entityPage.clear();
    m_mappedThisFrame.clear();
}

// ==== Atlas allocation ====
// On first need only: a scene with no custom-shader material must pay no VRAM.

bool MaterialSurfaceCache::EnsureAtlas(IGraphicsDevice& gfx)
{
    if (AtlasReady())  return true;
    if (m_atlasFailed) return false;
    // Created the first time an NPR material appears - mid-game, on the render half (48 MB, a
    // 2.8-24.7 ms hitch).
    PROF_SCOPE("SurfaceCache.CreateAtlas");

    // GraphicsDX12::CreateTexture assigns handleId AND copies the SRV into the bindless table at that
    // slot, so handleId IS the g_AllTextures[] index the RT shaders sample (textures need no Register*
    // call), and an engine-owned texture's slot survives OnWorldClear.
    RHI::TextureDesc td{};
    td.width      = kAtlasSize;
    td.height     = kAtlasSize;
    td.mipLevels = 1;
    // SHADER_RESOURCE (sampled by the RT proxy) + UNORDERED_ACCESS (written by SurfaceCacheResolve.cs).
    // Deliberately NOT a render target: the bake rasterises into a page-sized scratch MRT set and the
    // resolve CS repacks from there, because GBuffer RT2's channel order is (rough, metal, ao,
    // reflectance) while the proxy consumes (AO, rough, metal).
    td.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::UNORDERED_ACCESS;
    td.usage      = RHI::Usage::DEFAULT;
    td.layout     = RHI::ResourceState::SHADER_RESOURCE;

    // LINEAR, not sRGB: engine BASECOLORMAPs are BC7_UNORM_SRGB and the RT shaders lean on the hardware
    // sRGB decode, but the GBuffer albedo output is already linear. RGBA16F keeps HDR albedo and gives .a
    // to opacity, which R11G11B10 (the real GBuffer RT0) has no room for.
    td.format     = RHI::Format::R16G16B16A16_FLOAT;
    td.debugName = "MaterialSurfaceCache.AlbedoAtlas";
    if (!gfx.CreateTexture(td, m_albedoAtlas))
    {
        LOG_ERROR("MaterialSurfaceCache: albedo atlas creation failed (%ux%u) — "
                  "surface cache disabled for this session", kAtlasSize, kAtlasSize);
        m_atlasFailed = true;
        return false;
    }

    td.format     = RHI::Format::R8G8B8A8_UNORM;
    td.debugName = "MaterialSurfaceCache.SurfaceAtlas";
    if (!gfx.CreateTexture(td, m_surfaceAtlas))
    {
        LOG_ERROR("MaterialSurfaceCache: surface atlas creation failed (%ux%u) — "
                  "surface cache disabled for this session", kAtlasSize, kAtlasSize);
        gfx.DestroyTexture(m_albedoAtlas);
        m_albedoAtlas = {};
        m_atlasFailed = true;
        return false;
    }

    m_atlasState = RHI::ResourceState::SHADER_RESOURCE;
    LOG_INFO("MaterialSurfaceCache: atlases ready (%ux%u, %u pages of %u^2; "
             "albedo bindless=%u surface bindless=%u)",
             kAtlasSize, kAtlasSize, kPageCount, kPageSize,
             m_albedoAtlas.handleId, m_surfaceAtlas.handleId);
    return true;
}

// ==== Producer side ====

void MaterialSurfaceCache::GetPageOrigin(uint32_t pageIdx, uint32_t& x, uint32_t& y) const
{
    x = (pageIdx % kPagesPerRow) * kPageSize;
    y = (pageIdx / kPagesPerRow) * kPageSize;
}

uint32_t MaterialSurfaceCache::AcquirePage(IGraphicsDevice& gfx,
                                           uint32_t meshDescIdx,
                                           uint32_t materialIndex,
                                           uint64_t contentHash)
{
    // Tripwire counter (see GetAcquireCallCount): under -surfacepagesearly the emit nest asserts it did
    // not move across the nest. Unconditional, not WITH_HARNESS-only: one increment per gated batch, and
    // the invariant it guards is what keeps the nest side-effect free with respect to this class.
    ++m_dbgAcquireCalls;
    if (!m_enabled) return kInvalidPage;

    const uint64_t key = (static_cast<uint64_t>(meshDescIdx) << 32) | materialIndex;

    if (auto it = m_keyToPage.find(key); it != m_keyToPage.end())
    {
        Page& hit = m_pages[it->second];
        hit.lastSeenFrame = m_frame;
        if (hit.contentHash != contentHash)
        {
            // A recycled heap slot put DIFFERENT content behind the same key.
            // Un-publish first, re-bake second: serving the previous tenant's
            // texels for the three frames the re-bake takes to retire would be a
            // visible wrong-material reflection, and it is exactly the failure a
            // reader would never trace back to slot recycling.
            hit.contentHash = contentHash;
            if (!hit.needsBake)
            {
                hit.needsBake = true;
                ++m_pendingBakes;
            }
            if (hit.usable)
            {
                hit.usable = false;
                ++m_publishEpoch;
            }
        }
        return it->second;
    }

    if (!EnsureAtlas(gfx)) return kInvalidPage;

    // Once the atlas is full for this frame, it stays full for this frame: the scan rejects any page
    // with lastSeenFrame == m_frame, and within a frame lastSeenFrame is only ever RAISED to m_frame, so
    // after one scan finds no victim every later one would too. Without the latch a crowd scene rescans
    // all 256 pages for every gated batch past the 256th (100k+ futile iterations a frame).
    // The atlas does saturate in crowd scenes, leaving most character sections on the alias proxy. Do not
    // "fix" that by rotating pages: every eviction bumps m_publishEpoch, which vetoes DDGISceneAS's
    // retained-gather patch (~1.77 ms a frame). Saturation is load-bearing: it keeps the epoch still.
    if (m_atlasFullThisFrame) return kInvalidPage;

    // Free slot first, then LRU. The victim must NOT have been seen this frame: every entity mapped this
    // frame already stamped its page as seen, so a page is never pulled from under a mapping made earlier
    // in the same batch loop.
    uint32_t victim  = kInvalidPage;
    uint64_t oldest  = UINT64_MAX;
    for (uint32_t i = 0; i < kPageCount; ++i)
    {
        if (!m_pages[i].inUse) { victim = i; oldest = 0; break; }
        if (m_pages[i].lastSeenFrame == m_frame) continue;   // live this frame
        if (m_pages[i].lastSeenFrame < oldest)
        {
            oldest = m_pages[i].lastSeenFrame;
            victim = i;
        }
    }
    if (victim == kInvalidPage)
    {
        // Every page belongs to a batch drawn this frame: this batch keeps the alias-path proxy and wins a
        // page once some other batch stops being drawn.
        m_atlasFullThisFrame = true;   // no later scan this frame can succeed
        static bool s_warned = false;
        if (!s_warned)
        {
            LOG_WARNING("MaterialSurfaceCache: all %u pages live in one frame — "
                        "extra custom-shader sections fall back to the alias proxy",
                        kPageCount);
            s_warned = true;
        }
        return kInvalidPage;
    }

    Page& p = m_pages[victim];
    if (p.inUse)
    {
        m_keyToPage.erase(p.key);
        if (p.baked) --m_pagesBaked;
        if (p.needsBake && m_pendingBakes) --m_pendingBakes;
        // An evicted page's texels are about to be overwritten by a different
        // material, so any instance still pointing at it must stop trusting it
        // THIS frame, not after the retained gather next happens to invalidate.
        ++m_publishEpoch;
        ++m_statEvicted;
    }
    else
    {
        ++m_pagesInUse;
    }
    ++m_statAcquired;

    p = Page{};
    p.key           = key;
    p.contentHash   = contentHash;
    p.lastSeenFrame = m_frame;
    p.inUse         = true;
    p.needsBake     = true;
    ++m_pendingBakes;
    m_keyToPage[key] = victim;
    return victim;
}

void MaterialSurfaceCache::MapEntity(uint32_t entityId, uint32_t pageIdx)
{
    ++m_dbgMapEntityCalls;   // see AcquirePage's note
    if (!m_enabled || pageIdx >= kPageCount) return;
    if (entityId >= m_entityPage.size())
        m_entityPage.resize(std::max<size_t>(entityId + 1, m_entityPage.size() * 2 + 64));
    EntitySlot& slot = m_entityPage[entityId];
    if (slot.frame != m_frame) m_mappedThisFrame.push_back(entityId);   // once per frame
    slot.frame = m_frame;
    slot.page  = pageIdx;
}

// ==== Consumer side ====

bool MaterialSurfaceCache::LookupEntity(uint32_t entityId, PageBinding& out) const
{
    if (!m_enabled || !AtlasReady())          return false;
    if (entityId >= m_entityPage.size())      return false;

    const EntitySlot& slot = m_entityPage[entityId];
    if (slot.frame != m_frame)                return false;   // not drawn this frame
    if (slot.page >= kPageCount)              return false;

    const Page& p = m_pages[slot.page];
    // `usable`, not `baked`: allocation is CPU-side and happens BEFORE the GPU
    // fills the page, so a freshly allocated page holds either nothing or the
    // previous tenant's texels. Sampling it would be strictly worse than the
    // alias path it replaces.
    if (!p.inUse || !p.usable)                return false;

    uint32_t px = 0, py = 0;
    GetPageOrigin(slot.page, px, py);
    constexpr float kInvAtlas = 1.0f / static_cast<float>(kAtlasSize);
    // HALF-TEXEL INSET. Mapping uv [0,1] onto the page's full [px, px+kPageSize] range would put uv=0
    // EXACTLY on atlas coordinate px, which bilinear filtering blends 50/50 with column px-1 - a texel of
    // the PREVIOUS PAGE, another material (same at uv=1). Character unwraps often pack UV shells flush
    // against 0 or 1. Shrinking the range by one texel and centring it maps uv=0 / uv=1 to the centres
    // of the page's first / last texel, so every tap stays inside the page; the outermost half-texel of
    // authored UV is resampled rather than reproduced exactly.
    constexpr float kInsetScale = static_cast<float>(kPageSize - 1);
    out.rect[0] = kInsetScale * kInvAtlas;
    out.rect[1] = kInsetScale * kInvAtlas;
    out.rect[2] = (static_cast<float>(px) + 0.5f) * kInvAtlas;
    out.rect[3] = (static_cast<float>(py) + 0.5f) * kInvAtlas;
    out.albedoTexIdx  = static_cast<int32_t>(m_albedoAtlas.handleId);
    out.surfaceTexIdx = static_cast<int32_t>(m_surfaceAtlas.handleId);
    return true;
}

// ==== Bake side ====

uint32_t MaterialSurfaceCache::TakeBakeSlot(uint32_t meshDescIdx, uint32_t materialIndex)
{
    if (!m_enabled || m_budgetLeft == 0) return kInvalidPage;

    const uint64_t key = (static_cast<uint64_t>(meshDescIdx) << 32) | materialIndex;
    auto it = m_keyToPage.find(key);
    if (it == m_keyToPage.end()) return kInvalidPage;

    Page& p = m_pages[it->second];
    if (!p.inUse || !p.needsBake)      return kInvalidPage;
    if (p.claimedFrame == m_frame)     return kInvalidPage;   // already scheduled

    // needsBake is deliberately NOT cleared here - only MarkBaked clears it, so a page whose PSO failed
    // to build is retried next frame instead of being published as baked-but-empty.
    p.claimedFrame = m_frame;
    --m_budgetLeft;
    return it->second;
}

void MaterialSurfaceCache::MarkBaked(uint32_t pageIdx)
{
    if (pageIdx >= kPageCount) return;
    Page& p = m_pages[pageIdx];
    if (!p.inUse || !p.needsBake) return;

    p.needsBake  = false;
    p.bakedFrame = m_frame;
    if (m_pendingBakes) --m_pendingBakes;
    if (!p.baked)
    {
        p.baked = true;
        ++m_pagesBaked;
    }
    // Publication is deliberately NOT set here - see kPublishLagFrames and the promotion loop in
    // BeginFrame.
}
