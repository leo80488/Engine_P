#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Resource/TextureSystem.h"
#include "Resource/AssetFS.h"
#include "Diagnostics/Profiler.h"
#include "Graphics/IGraphicsDevice.h"
#include "System/CommandLine.h"   // -tex_promote_budget_ms
#include "System/HotReloadReceipt.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/TaskSystem.h"     // the loader workers' promotions
#include <chrono>
#include <exception>
#include <dxgiformat.h>
#include <filesystem>
#include <system_error>
#include <unordered_set>

namespace Resource
{
    namespace
    {
        // Which write of the file this is: its size and mtime, 0 when it
        // cannot be read (a pak, a missing file) - then every notice reloads.
        std::uint64_t FileStamp(const std::string& path)
        {
            std::error_code ec;
            const std::string onDisk = AssetFS::Get().ResolveOnDisk(path);   // the file the load reads
            const auto p = Sys::U8Path(onDisk.empty() ? path : onDisk);
            const auto size = std::filesystem::file_size(p, ec);
            if (ec) return 0;
            const auto time = std::filesystem::last_write_time(p, ec);
            if (ec) return 0;
            return (static_cast<std::uint64_t>(time.time_since_epoch().count()) * 1099511628211ull) ^
                   static_cast<std::uint64_t>(size) ^ 1ull;
        }
    }

    // ==== Path hashing (FNV-1a 64-bit, the same algorithm as ResourceManager) ====

    uint64_t TextureSystem::HashPath(const std::string& path)
    {
        // Normalize while hashing so the same asset referenced with different separators ('/' vs '\') or
        // letter case dedups to one slot: authored .imat paths mix both (e.g. "engine/textures\\RampMap..."),
        // which would otherwise load a duplicate GPU texture. Only the dedup key changes; the file is opened
        // from the original path.
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : path)
        {
            if (c == '\\')                 c = '/';      // unify separators
            else if (c >= 'A' && c <= 'Z') c = c + 32;   // case-insensitive (Windows FS)
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    // ==== DXGI_FORMAT -> RHI::Format ====

    RHI::Format TextureSystem::DxgiToRhiFormat(uint32_t dxgiFmt)
    {
        switch (static_cast<DXGI_FORMAT>(dxgiFmt))
        {
        // Typeless formats from DDS are mapped to a sampleable typed variant.
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return RHI::Format::R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return RHI::Format::R16G16B16A16_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return RHI::Format::R10G10B10A2_UNORM;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return RHI::Format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return RHI::Format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_R16G16_TYPELESS:       return RHI::Format::R16G16_UNORM;
        case DXGI_FORMAT_R32_TYPELESS:          return RHI::Format::R32_FLOAT;
        case DXGI_FORMAT_R16_TYPELESS:          return RHI::Format::R16_UNORM;
        case DXGI_FORMAT_R8_TYPELESS:           return RHI::Format::R8_UNORM;
        case DXGI_FORMAT_BC1_TYPELESS:          return RHI::Format::BC1_UNORM;
        case DXGI_FORMAT_BC2_TYPELESS:          return RHI::Format::BC2_UNORM;
        case DXGI_FORMAT_BC3_TYPELESS:          return RHI::Format::BC3_UNORM;
        case DXGI_FORMAT_BC4_TYPELESS:          return RHI::Format::BC4_UNORM;
        case DXGI_FORMAT_BC5_TYPELESS:          return RHI::Format::BC5_UNORM;
        case DXGI_FORMAT_BC6H_TYPELESS:         return RHI::Format::BC6H_UF16;
        case DXGI_FORMAT_BC7_TYPELESS:          return RHI::Format::BC7_UNORM;

        case DXGI_FORMAT_R32G32B32A32_FLOAT:   return RHI::Format::R32G32B32A32_FLOAT;
        case DXGI_FORMAT_R32G32B32A32_UINT:    return RHI::Format::R32G32B32A32_UINT;
        case DXGI_FORMAT_R32G32B32A32_SINT:    return RHI::Format::R32G32B32A32_SINT;
        case DXGI_FORMAT_R32G32B32_FLOAT:      return RHI::Format::R32G32B32_FLOAT;
        case DXGI_FORMAT_R32G32B32_UINT:       return RHI::Format::R32G32B32_UINT;
        case DXGI_FORMAT_R32G32B32_SINT:       return RHI::Format::R32G32B32_SINT;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   return RHI::Format::R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R16G16B16A16_UNORM:   return RHI::Format::R16G16B16A16_UNORM;
        case DXGI_FORMAT_R16G16B16A16_UINT:    return RHI::Format::R16G16B16A16_UINT;
        case DXGI_FORMAT_R16G16B16A16_SNORM:   return RHI::Format::R16G16B16A16_SNORM;
        case DXGI_FORMAT_R16G16B16A16_SINT:    return RHI::Format::R16G16B16A16_SINT;
        case DXGI_FORMAT_R32G32_FLOAT:         return RHI::Format::R32G32_FLOAT;
        case DXGI_FORMAT_R32G32_UINT:          return RHI::Format::R32G32_UINT;
        case DXGI_FORMAT_R32G32_SINT:          return RHI::Format::R32G32_SINT;
        case DXGI_FORMAT_R10G10B10A2_UNORM:    return RHI::Format::R10G10B10A2_UNORM;
        case DXGI_FORMAT_R10G10B10A2_UINT:     return RHI::Format::R10G10B10A2_UINT;
        case DXGI_FORMAT_R11G11B10_FLOAT:      return RHI::Format::R11G11B10_FLOAT;
        case DXGI_FORMAT_R8G8B8A8_UNORM:       return RHI::Format::R8G8B8A8_UNORM;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return RHI::Format::R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_R8G8B8A8_UINT:        return RHI::Format::R8G8B8A8_UINT;
        case DXGI_FORMAT_R8G8B8A8_SNORM:       return RHI::Format::R8G8B8A8_SNORM;
        case DXGI_FORMAT_R8G8B8A8_SINT:        return RHI::Format::R8G8B8A8_SINT;
        case DXGI_FORMAT_B8G8R8A8_UNORM:       return RHI::Format::B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:  return RHI::Format::B8G8R8A8_UNORM_SRGB;
        case DXGI_FORMAT_R16G16_FLOAT:         return RHI::Format::R16G16_FLOAT;
        case DXGI_FORMAT_R16G16_UNORM:         return RHI::Format::R16G16_UNORM;
        case DXGI_FORMAT_R16G16_UINT:          return RHI::Format::R16G16_UINT;
        case DXGI_FORMAT_R16G16_SNORM:         return RHI::Format::R16G16_SNORM;
        case DXGI_FORMAT_R16G16_SINT:          return RHI::Format::R16G16_SINT;
        case DXGI_FORMAT_D32_FLOAT:            return RHI::Format::D32_FLOAT;
        case DXGI_FORMAT_R32_FLOAT:            return RHI::Format::R32_FLOAT;
        case DXGI_FORMAT_R32_UINT:             return RHI::Format::R32_UINT;
        case DXGI_FORMAT_R32_SINT:             return RHI::Format::R32_SINT;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:    return RHI::Format::D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R8G8_UNORM:           return RHI::Format::R8G8_UNORM;
        case DXGI_FORMAT_R8G8_UINT:            return RHI::Format::R8G8_UINT;
        case DXGI_FORMAT_R8G8_SNORM:           return RHI::Format::R8G8_SNORM;
        case DXGI_FORMAT_R8G8_SINT:            return RHI::Format::R8G8_SINT;
        case DXGI_FORMAT_R16_FLOAT:            return RHI::Format::R16_FLOAT;
        case DXGI_FORMAT_D16_UNORM:            return RHI::Format::D16_UNORM;
        case DXGI_FORMAT_R16_UNORM:            return RHI::Format::R16_UNORM;
        case DXGI_FORMAT_R16_UINT:             return RHI::Format::R16_UINT;
        case DXGI_FORMAT_R16_SNORM:            return RHI::Format::R16_SNORM;
        case DXGI_FORMAT_R16_SINT:             return RHI::Format::R16_SINT;
        case DXGI_FORMAT_R8_UNORM:             return RHI::Format::R8_UNORM;
        case DXGI_FORMAT_R8_UINT:              return RHI::Format::R8_UINT;
        case DXGI_FORMAT_R8_SNORM:             return RHI::Format::R8_SNORM;
        case DXGI_FORMAT_R8_SINT:              return RHI::Format::R8_SINT;
        case DXGI_FORMAT_BC1_UNORM:            return RHI::Format::BC1_UNORM;
        case DXGI_FORMAT_BC1_UNORM_SRGB:       return RHI::Format::BC1_UNORM_SRGB;
        case DXGI_FORMAT_BC2_UNORM:            return RHI::Format::BC2_UNORM;
        case DXGI_FORMAT_BC2_UNORM_SRGB:       return RHI::Format::BC2_UNORM_SRGB;
        case DXGI_FORMAT_BC3_UNORM:            return RHI::Format::BC3_UNORM;
        case DXGI_FORMAT_BC3_UNORM_SRGB:       return RHI::Format::BC3_UNORM_SRGB;
        case DXGI_FORMAT_BC4_UNORM:            return RHI::Format::BC4_UNORM;
        case DXGI_FORMAT_BC4_SNORM:            return RHI::Format::BC4_SNORM;
        case DXGI_FORMAT_BC5_UNORM:            return RHI::Format::BC5_UNORM;
        case DXGI_FORMAT_BC5_SNORM:            return RHI::Format::BC5_SNORM;
        case DXGI_FORMAT_BC6H_UF16:            return RHI::Format::BC6H_UF16;
        case DXGI_FORMAT_BC6H_SF16:            return RHI::Format::BC6H_SF16;
        case DXGI_FORMAT_BC7_UNORM:            return RHI::Format::BC7_UNORM;
        case DXGI_FORMAT_BC7_UNORM_SRGB:       return RHI::Format::BC7_UNORM_SRGB;
        case DXGI_FORMAT_NV12:                 return RHI::Format::NV12;
        default:                               return RHI::Format::UNKNOWN;
        }
    }

    // ==== GPU upload (UploadToGPU: the caller holds m_mutex) ====

    bool TextureSystem::DescribeUpload(const TextureResource& res, RHI::TextureDesc& desc,
                                       std::vector<RHI::SubresourceData>& subData, std::string& error)
    {
        const DirectX::TexMetadata& meta = res.GetMetadata();
        const DirectX::ScratchImage& img = res.GetImage();

        desc = {};
        desc.width      = static_cast<uint32_t>(meta.width);
        desc.height     = static_cast<uint32_t>(meta.height);
        desc.depth      = static_cast<uint32_t>(meta.depth);
        desc.arraySize = static_cast<uint32_t>(meta.arraySize);
        desc.mipLevels = static_cast<uint32_t>(meta.mipLevels);
        desc.format     = DxgiToRhiFormat(static_cast<uint32_t>(meta.format));
        desc.usage      = RHI::Usage::DEFAULT;
        desc.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        desc.layout     = RHI::ResourceState::SHADER_RESOURCE;

        switch (meta.dimension)
        {
        case DirectX::TEX_DIMENSION_TEXTURE1D: desc.type = RHI::TextureDesc::Type::TEXTURE_1D; break;
        case DirectX::TEX_DIMENSION_TEXTURE3D: desc.type = RHI::TextureDesc::Type::TEXTURE_3D; break;
        default:                               desc.type = RHI::TextureDesc::Type::TEXTURE_2D; break;
        }

        if (meta.IsCubemap())
            desc.miscFlags |= RHI::ResourceMiscFlag::TEXTURECUBE;

        LOG_TRACE("TextureSystem::UploadToGPU: %ux%u fmt=%u mips=%zu arraySize=%zu isCubemap=%d",
                 meta.width, meta.height, static_cast<unsigned>(meta.format),
                 meta.mipLevels, meta.arraySize, meta.IsCubemap() ? 1 : 0);

        if (desc.format == RHI::Format::UNKNOWN)
        {
            LOG_ERROR("TextureSystem: unsupported DXGI format %u (%ux%u, mips=%zu)",
                      static_cast<unsigned>(meta.format), meta.width, meta.height, meta.mipLevels);
            error = "unsupported DXGI format " + std::to_string(static_cast<unsigned>(meta.format));
            return false;
        }

        // One SubresourceData per image (mip x array slice).
        const size_t subCount = img.GetImageCount();
        subData.assign(subCount, RHI::SubresourceData{});
        const DirectX::Image* images = img.GetImages();
        for (size_t i = 0; i < subCount; ++i)
        {
            subData[i].dataPtr    = images[i].pixels;
            subData[i].rowPitch   = static_cast<uint32_t>(images[i].rowPitch);
            subData[i].slicePitch = static_cast<uint32_t>(images[i].slicePitch);
        }
        return true;
    }

    void TextureSystem::UploadToGPU(const TextureResource& res, TextureEntry& entry, IGraphicsDevice& gfx,
                                    const std::string& name)
    {
        PROF_SCOPE("Tex.UploadToGPU");
        const DirectX::TexMetadata& meta = res.GetMetadata();
        RHI::TextureDesc desc;
        std::vector<RHI::SubresourceData> subData;
        if (!DescribeUpload(res, desc, subData, entry.uploadError))
            return;
        desc.debugName = name.c_str();

        if (gfx.CreateTexture(desc, entry.texture, subData.data()))
        {
            entry.gpuReady = true;
            entry.gpuBytes = gfx.GetTextureAllocationBytes(entry.texture);
            entry.revision = m_contentRevision.fetch_add(1,std::memory_order_relaxed) + 1;
        }
        else
        {
            LOG_ERROR("TextureSystem: CreateTexture failed (format=%u, %ux%u mips=%zu)",
                      static_cast<unsigned>(meta.format), meta.width, meta.height, meta.mipLevels);
            entry.uploadError = "CreateTexture failed (format " + std::to_string(static_cast<unsigned>(meta.format)) +
                                ", " + std::to_string(meta.width) + "x" + std::to_string(meta.height) + ")";
        }
    }

    void TextureSystem::DropCpuImage(TextureEntry& entry, ResourceManager& rm)
    {
        if (entry.keepCpuImage || !entry.rmHandle.IsValid()) return;
        // At once, not through m_pendingRmUnload: a reload requested before the next Tick must find no
        // entry to hand the old contents back from.
        rm.Unload(entry.rmHandle);
        entry.rmHandle = Handle{};
    }

    // ==== Slot pool helpers (the caller holds m_mutex) ====

    uint32_t TextureSystem::AllocSlot()
    {
        if (!m_freeList.empty())
        {
            uint32_t idx = m_freeList.back();
            m_freeList.pop_back();
            return idx;
        }
        m_slots.emplace_back();
        return static_cast<uint32_t>(m_slots.size() - 1);
    }

    void TextureSystem::FreeSlot(uint32_t index)
    {
        m_contentRevision.fetch_add(1,std::memory_order_relaxed);
        TextureEntry& e = m_slots[index];
        m_pathHashToSlot.erase(e.pathHash);
        // The IO cache owns both the file bytes and the decoded CPU image.
        // Dropping only these handles leaves every departed scene's textures
        // resident. Tick has the ResourceManager needed to retire them safely.
        if (e.rmHandle.IsValid()) m_pendingRmUnload.push_back(e.rmHandle);
        if (e.reloadRm.IsValid()) m_pendingRmUnload.push_back(e.reloadRm);
        // Bump the generation; it never wraps to 0 (0 = null sentinel).
        e.generation = (e.generation % Handle::MAX_GEN) + 1;
        e.texture    = {};
        e.refCount   = 0;
        e.rmHandle   = Handle{};
        e.gpuReady   = false;
        e.pathHash   = 0;
        e.gpuBytes   = 0;
        e.revision   = 0;
        e.uploadError.clear();
        if (e.reloadReceipt != 0)
        {
            HotReload::Receipt rc;
            rc.code    = "not_in_use";
            rc.message = "the texture was released before its reload finished";
            HotReload::Close(e.reloadReceipt, rc);
        }
        e.reloadRm      = Handle{};
        e.reloadReceipt = 0;
        e.fileStamp     = 0;
        e.budgeted      = false;
        e.promoting     = false;   // an in-flight promotion of the old occupant is destroyed at publish
        e.path.clear();
        e.keepCpuImage  = false;
        m_freeList.push_back(index);
    }

    // ==== Budgeted promotions on a loader worker ====

    void TextureSystem::StartAsyncPromotion(TextureEntry& entry, uint32_t idx, ResourceManager& rm,
                                            IGraphicsDevice& gfx)
    {
        auto p = std::make_shared<AsyncPromotion>();
        p->slot       = idx;
        p->generation = entry.generation;
        p->rmHandle   = entry.rmHandle;
        p->name       = rm.GetPath(entry.rmHandle);
        entry.promoting = true;
        m_asyncPromotions.push_back(p);
        {
            std::lock_guard<std::mutex> lk(m_asyncMutex);
            ++m_asyncJobs;
        }
        // The resource manager's image stays loaded while this runs (Tick keeps
        // the handle out of its unload pass until the promotion is published).
        TaskSystem::Get().Push([this, p, rmPtr = &rm, gfxPtr = &gfx]()
        {
            PROF_SCOPE("Tex.PromoteOnLoader");
            IGraphicsDevice& g = *gfxPtr;
            try
            {
                LoaderUploadScope scope(g);
                if (scope.Active())
                {
                    if (const auto* res = rmPtr->Get<TextureResource>(p->rmHandle))
                    {
                        RHI::TextureDesc desc;
                        std::vector<RHI::SubresourceData> subData;
                        if (DescribeUpload(*res, desc, subData, p->error))
                        {
                            desc.debugName = p->name.c_str();
                            if (g.CreateTexture(desc, p->texture, subData.data()))
                                p->gpuBytes = g.GetTextureAllocationBytes(p->texture);
                            else
                                p->error = "CreateTexture failed on the loader (" + std::to_string(desc.width) + "x" +
                                           std::to_string(desc.height) + ")";
                        }
                    }
                    else
                        p->error = "the resource is not a TextureResource";
                }
                else
                    p->error = "the loader uploads are off";
            }
            catch (const std::exception& e) { p->error = e.what(); }
            catch (...)                     { p->error = "an unknown exception"; }
            p->done.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(m_asyncMutex);
                --m_asyncJobs;
            }
            m_asyncCv.notify_all();
        });
    }

    void TextureSystem::PublishAsyncPromotions(IGraphicsDevice& gfx, ResourceManager& rm)
    {
        for (auto it = m_asyncPromotions.begin(); it != m_asyncPromotions.end();)
        {
            AsyncPromotion& p = **it;
            if (!p.done.load(std::memory_order_acquire)) { ++it; continue; }
            TextureEntry* e = p.slot < m_slots.size() ? &m_slots[p.slot] : nullptr;
            const bool live = e && e->generation == p.generation && e->refCount > 0 && e->promoting;
            if (live)
            {
                e->promoting = false;
                if (p.texture.IsValid())
                {
                    e->texture  = p.texture;
                    e->gpuReady = true;
                    e->gpuBytes = p.gpuBytes;
                    e->revision = m_contentRevision.fetch_add(1, std::memory_order_relaxed) + 1;
                    DropCpuImage(*e, rm);
                }
                else
                {
                    LOG_ERROR("TextureSystem: the loader's promotion of '%s' failed: %s", p.name.c_str(),
                              p.error.c_str());
                    e->uploadError = p.error.empty() ? std::string("CreateTexture failed on the loader") : p.error;
                }
            }
            else if (p.texture.IsValid())
                gfx.DestroyTexture(p.texture);   // released (or the slot reused) meanwhile
            it = m_asyncPromotions.erase(it);
        }
    }

    bool TextureSystem::IsValidHandle(Handle h) const
    {
        if (!h.IsValid()) return false;
        const uint32_t idx = h.Index();
        if (idx >= m_slots.size()) return false;
        return m_slots[idx].generation == h.Generation();
    }

    // ==== Acquire ====

    TextureHandle TextureSystem::Acquire(const std::string& path,
                                         ResourceManager&   rm,
                                         IGraphicsDevice&   /*gfx*/)
    {
        return AcquireImpl(path, rm, /*budgeted*/ false, /*keepCpuImage*/ false);
    }

    TextureHandle TextureSystem::AcquireBudgeted(const std::string& path,
                                                 ResourceManager&   rm,
                                                 IGraphicsDevice&   /*gfx*/)
    {
        return AcquireImpl(path, rm, /*budgeted*/ true, /*keepCpuImage*/ false);
    }

    TextureHandle TextureSystem::AcquireWithCpuImage(const std::string& path,
                                                     ResourceManager&   rm,
                                                     IGraphicsDevice&   /*gfx*/)
    {
        return AcquireImpl(path, rm, /*budgeted*/ false, /*keepCpuImage*/ true);
    }

    uint32_t TextureSystem::PendingBudgetedPromotions() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_budgetedWaiting;
    }

    uint32_t TextureSystem::LastTickOversizedBudgetedPromotions() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_budgetedOversized;
    }

    double TextureSystem::LastTickBudgetedMs() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_budgetedMs;
    }

    TextureHandle TextureSystem::AcquireImpl(const std::string& path, ResourceManager& rm, bool budgeted,
                                             bool keepCpuImage)
    {
        if (path.empty())
            return Handle{};

        const uint64_t key = HashPath(path);
        std::lock_guard<std::mutex> lock(m_mutex);

        // Dedup: same path -> same slot, ref count + 1.
        auto it = m_pathHashToSlot.find(key);
        if (it != m_pathHashToSlot.end())
        {
            const uint32_t idx = it->second;
            TextureEntry& entry = m_slots[idx];
            entry.refCount++;
            // An ordinary acquire wants the texture now: no budget.
            if (!budgeted) entry.budgeted = false;
            if (keepCpuImage && !entry.keepCpuImage)
            {
                entry.keepCpuImage = true;
                // Released after its upload: load the file again for the image alone (Tick promotes only
                // slots that are not on the GPU yet, so this load uploads nothing).
                if (!entry.rmHandle.IsValid() && !entry.reloadRm.IsValid())
                    entry.rmHandle = rm.Load(entry.path, ResourceType::Texture);
            }
            return Handle::Make(idx, ResourceType::Texture, entry.generation);
        }

        uint32_t idx = AllocSlot();
        TextureEntry& entry = m_slots[idx];
        if (entry.generation == 0) entry.generation = 1;  // never issue gen=0

        entry.pathHash = key;
        entry.path     = path;
        entry.rmHandle = rm.Load(path, ResourceType::Texture);
        entry.refCount = 1;
        entry.budgeted = budgeted;
        entry.keepCpuImage = keepCpuImage;
        m_pathHashToSlot[key] = idx;

        // GPU promotion is deferred to Tick (once a frame on the main thread, outside the device frame).
        return Handle::Make(idx, ResourceType::Texture, entry.generation);
    }

    // ==== Release ====

    void TextureSystem::Release(TextureHandle handle, IGraphicsDevice& gfx)
    {
        if (!handle.IsValid())
            return;

        std::lock_guard<std::mutex> lock(m_mutex);

        if (!IsValidHandle(handle))
            return;

        const uint32_t idx = handle.Index();
        TextureEntry& entry = m_slots[idx];
        if (entry.refCount == 0)
            return;

        if (--entry.refCount == 0)
        {
            // Do not destroy immediately: the current frame may still reference the texture. Tick hands this
            // list to the device's frame-stamped release queue.
            if (entry.gpuReady)
                m_pendingDestroy.push_back(std::move(entry.texture));
            FreeSlot(idx);
        }
    }

    // ==== Reload ====
    // A reimport: the new file loads beside the live texture, then Tick swaps it into the same handle
    // and bindless slot.

    TextureSystem::ReloadRequest TextureSystem::RequestReload(const std::string& path, ResourceManager& rm,
                                                              std::uint64_t* outReceipt)
    {
        if (outReceipt) *outReceipt = 0;
        if (path.empty()) return ReloadRequest::NotLoaded;
        const uint64_t key   = HashPath(path);
        const uint64_t stamp = FileStamp(path);

        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_pathHashToSlot.find(key);
        if (it == m_pathHashToSlot.end())
        {
            const std::uint64_t id = HotReload::Open("texture", { path });
            HotReload::Receipt rc;
            rc.code    = "not_in_use";
            rc.message = "no loaded texture reads it: nothing to reload";
            HotReload::Close(id, rc);
            if (outReceipt) *outReceipt = id;
            return ReloadRequest::NotLoaded;
        }
        TextureEntry& e = m_slots[it->second];
        if (stamp != 0 && stamp == e.fileStamp)
            return ReloadRequest::Unchanged;
        e.fileStamp = stamp;

        // A newer write overtakes a reload still loading the older one.
        if (e.reloadReceipt != 0)
        {
            HotReload::Receipt rc;
            rc.code    = "superseded";
            rc.message = "a newer write of the file was reloaded instead";
            HotReload::Close(e.reloadReceipt, rc);
            e.reloadReceipt = 0;
        }
        if (e.reloadRm.IsValid())
        {
            rm.Unload(e.reloadRm);
            e.reloadRm = Handle{};
        }

        const std::uint64_t id = HotReload::Open("texture", { path });
        HotReload::Start(id);
        e.reloadReceipt = id;
        if (outReceipt) *outReceipt = id;

        // The ResourceManager dedups by path: drop its entry, or Load would
        // hand back the old contents instead of reading the file.
        if (e.rmHandle.IsValid())
            rm.Unload(e.rmHandle);
        if (e.gpuReady)
        {
            // The live texture keeps drawing until the new file is ready.
            e.rmHandle = Handle{};
            e.reloadRm = rm.Load(path, ResourceType::Texture);
        }
        else
        {
            // Never became ready (a failed load or upload): the ordinary
            // promotion takes the new file, and closes the receipt.
            e.uploadError.clear();
            e.rmHandle = rm.Load(path, ResourceType::Texture);
        }
        return ReloadRequest::Requested;
    }

    void TextureSystem::FinishReload(TextureEntry& e, ResourceManager& rm, IGraphicsDevice& gfx,
                                     std::uint64_t frame)
    {
        HotReload::Receipt rc;
        const Handle loaded = e.reloadRm;
        e.reloadRm = Handle{};
        if (rm.GetState(loaded) == ResourceState::Failed)
        {
            std::string why = rm.GetFailReason(loaded);
            rc.code    = "load_failed";
            rc.message = "the new file did not load (" + (why.empty() ? std::string("no reason given") : why) +
                         ") - the previous contents stay";
            rc.failed  = 1;
            rm.Unload(loaded);
        }
        else
        {
            const auto* res = rm.Get<TextureResource>(loaded);
            RHI::TextureDesc desc;
            std::vector<RHI::SubresourceData> subData;
            std::string error;
            const std::string name = rm.GetPath(loaded);
            if (!res)
                error = "the loaded resource is not a texture";
            else if (DescribeUpload(*res, desc, subData, error))
            {
                desc.debugName = name.c_str();   // as UploadToGPU names it
                if (!gfx.ReplaceTexture(e.texture, desc, subData.data()))
                    error = "the device refused the replacement (see the log)";
            }
            if (error.empty())
            {
                e.rmHandle = loaded;
                DropCpuImage(e, rm);
                e.gpuBytes = gfx.GetTextureAllocationBytes(e.texture);
                e.revision = m_contentRevision.fetch_add(1, std::memory_order_relaxed) + 1;
                rc.ok             = 1;
                rc.effectiveFrame = frame;
                rc.message        = "in effect from frame " + std::to_string(frame) + ": " +
                                    std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                                    ", the same handle and bindless slot";
                rc.detailJson     = "{\"width\":" + std::to_string(desc.width) + ",\"height\":" +
                                    std::to_string(desc.height) + ",\"bindless\":" +
                                    std::to_string(e.texture.handleId) + "}";
            }
            else
            {
                rc.code    = "reload_error";
                rc.message = error + " - the previous contents stay";
                rc.failed  = 1;
                rm.Unload(loaded);
            }
        }
        if (e.reloadReceipt != 0)
            HotReload::Close(e.reloadReceipt, rc);
        e.reloadReceipt = 0;
    }

    // ==== Queries ====

    bool TextureSystem::IsReady(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return false;
        return m_slots[handle.Index()].gpuReady;
    }

    const RHI::Texture* TextureSystem::GetTexture(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return nullptr;
        // No lock: main-thread only access (hot path).
        if (!IsValidHandle(handle))
            return nullptr;
        const TextureEntry& e = m_slots[handle.Index()];
        return e.gpuReady ? &e.texture : nullptr;
    }

    Handle TextureSystem::GetResourceManagerHandle(TextureHandle handle) const
    {
        if (!handle.IsValid())
            return Handle{};
        if (!IsValidHandle(handle))
            return Handle{};
        return m_slots[handle.Index()].rmHandle;
    }

    TextureSystem::Status TextureSystem::GetStatus(TextureHandle handle, const ResourceManager& rm) const
    {
        Status s;
        if (!handle.IsValid()) return s;
        Handle rmHandle;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!IsValidHandle(handle)) return s;
            const TextureEntry& e = m_slots[handle.Index()];
            if (e.gpuReady) { s.state = Status::State::Ready; return s; }
            if (!e.uploadError.empty())
            {
                s.state  = Status::State::Failed;
                s.reason = "GPU upload: " + e.uploadError;
                return s;
            }
            rmHandle = e.rmHandle;
        }
        // Outside m_mutex: the ResourceManager takes its own lock.
        switch (rm.GetState(rmHandle))
        {
        case ResourceState::Failed:
            s.state  = Status::State::Failed;
            s.reason = rm.GetFailReason(rmHandle);
            if (s.reason.empty()) s.reason = "the load failed";
            break;
        case ResourceState::Ready:     // loaded, the GPU upload is next frame's Tick
        case ResourceState::Loading:
        case ResourceState::NotLoaded:
            s.state = Status::State::Loading;
            break;
        }
        return s;
    }

    std::vector<TextureSystem::EntryView> TextureSystem::Snapshot(const ResourceManager& rm) const
    {
        // The handles under the lock; each one's description, status and path
        // after it (GetStatus / Describe take the lock themselves, and the
        // ResourceManager has its own).
        std::vector<std::pair<TextureHandle, std::string>> live;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            live.reserve(m_slots.size());
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_slots.size()); ++i)
            {
                const TextureEntry& e = m_slots[i];
                if (e.refCount == 0) continue;
                live.emplace_back(Handle::Make(i, ResourceType::Texture, e.generation), e.path);
            }
        }
        std::vector<EntryView> out;
        out.reserve(live.size());
        for (const auto& [h, path] : live)
        {
            EntryView v;
            v.handle = h;
            if (!Describe(h, v.info)) continue;   // released meanwhile
            v.status = GetStatus(h, rm);
            v.path   = path;
            out.push_back(std::move(v));
        }
        return out;
    }

    TextureHandle TextureSystem::Find(const std::string& path) const
    {
        if (path.empty()) return {};
        const uint64_t key = HashPath(path);
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = m_pathHashToSlot.find(key);
        if (it == m_pathHashToSlot.end()) return {};
        return Handle::Make(it->second, ResourceType::Texture, m_slots[it->second].generation);
    }

    bool TextureSystem::Describe(TextureHandle handle, Info& out) const
    {
        if (!handle.IsValid()) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!IsValidHandle(handle)) return false;
        const TextureEntry& e = m_slots[handle.Index()];
        out.refCount = e.refCount;
        out.gpuReady = e.gpuReady;
        if (e.gpuReady)
        {
            const RHI::TextureDesc& d = e.texture.desc;
            out.width     = d.width;
            out.height    = d.height;
            out.depth     = d.depth;
            out.mipLevels = d.mipLevels;
            out.arraySize = d.arraySize;
            out.isCube    = RHI::HasFlag(d.miscFlags, RHI::ResourceMiscFlag::TEXTURECUBE);
            out.format    = d.format;
            out.gpuBytes  = e.gpuBytes;
            out.revision  = e.revision;
        }
        return true;
    }

    // ==== Tick: flush deferred destroys, promote RM-ready entries to the GPU ====

    void TextureSystem::Tick(ResourceManager& rm, IGraphicsDevice& gfx, std::uint64_t frame)
    {
        PROF_SCOPE("Tex.Tick");
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_pendingRmUnload.empty())
        {
            // A release followed by a same-path Acquire before this Tick can
            // reuse the IO handle. Keep anything that the new slot still uses,
            // including the new file of an in-flight reload.
            std::unordered_set<uint32_t> live;
            for (const auto& entry : m_slots)
            {
                if (entry.refCount == 0) continue;
                if (entry.rmHandle.IsValid()) live.insert(entry.rmHandle.packed);
                if (entry.reloadRm.IsValid()) live.insert(entry.reloadRm.packed);
            }
            // A loader worker still reads its image.
            for (const auto& p : m_asyncPromotions) live.insert(p->rmHandle.packed);
            std::vector<Handle> loading;
            for (Handle handle : m_pendingRmUnload)
            {
                if (live.contains(handle.packed)) continue;
                // Workers commit into RM slots. Let an abandoned load finish
                // before recycling its slot, then evict it on a later Tick.
                if (rm.GetState(handle) == ResourceState::Loading)
                    loading.push_back(handle);
                else
                    rm.Unload(handle);
            }
            m_pendingRmUnload = std::move(loading);
        }

        // Reloads whose new file finished loading (RequestReload). The swap rewrites descriptors that
        // in-flight frames read, so the GPU is idled once for all of them, and first: the hard sync ends by
        // draining every stamped release bucket, so it must precede this frame's own destroys below (App's
        // AssetTick note; Renderer::ServiceEnableGatedResources, just before this call, follows the same rule).
        {
            std::vector<TextureEntry*> finished;
            bool anyReady = false;
            for (auto& entry : m_slots)
            {
                if (entry.refCount == 0 || !entry.reloadRm.IsValid()) continue;
                const ResourceState st = rm.GetState(entry.reloadRm);
                if (st != ResourceState::Ready && st != ResourceState::Failed) continue;
                anyReady |= (st == ResourceState::Ready);
                finished.push_back(&entry);
            }
            if (anyReady)
                gfx.WaitIdleAndReleaseDeferred();
            for (TextureEntry* entry : finished)
                FinishReload(*entry, rm, gfx, frame);
        }

        // Flush the deferred-destroy list. Safe outside the device frame because DestroyTexture only moves
        // the resource, its descriptors and its pool slot into the frame-stamped release queue, which frees
        // them once the stamped frame has retired.
        for (auto& tex : m_pendingDestroy)
            gfx.DestroyTexture(tex);
        m_pendingDestroy.clear();

        // Promote RM-ready entries to the GPU. Collect them first so the whole burst shares one upload
        // flush instead of one per texture (CreateTexture honours the batch scope; the copies run on the
        // copy queue and the flush waits for them alone). Open the batch only when there is work.
        // A budgeted entry (a preload's, AcquireBudgeted) waits in a list of its own and is promoted after
        // the burst, while the budget lasts.
        std::vector<TextureEntry*> toPromote;
        std::vector<TextureEntry*> budgeted;
        for (auto& entry : m_slots)
        {
            if (entry.refCount == 0 || entry.gpuReady || !entry.rmHandle.IsValid())
                continue;
            if (!entry.uploadError.empty())   // failed once: no retry (and no ERROR) every frame
                continue;
            if (entry.promoting)              // a loader worker has it
                continue;
            const ResourceState st = rm.GetState(entry.rmHandle);
            if (st == ResourceState::Failed && entry.reloadReceipt != 0)
            {
                // A reload of a texture that never became ready, failing again.
                HotReload::Receipt rc;
                rc.code    = "load_failed";
                rc.message = "the new file did not load (" + rm.GetFailReason(entry.rmHandle) + ")";
                rc.failed  = 1;
                HotReload::Close(entry.reloadReceipt, rc);
                entry.reloadReceipt = 0;
                continue;
            }
            if (st != ResourceState::Ready)
                continue;
            // A reload keeps going through the burst: a receipt waits for it.
            if (entry.budgeted && entry.reloadReceipt == 0) { budgeted.push_back(&entry); continue; }
            toPromote.push_back(&entry);
        }

        // The budget's share: whole textures while this Tick's budget lasts, the first one regardless (a
        // texture dearer than the budget still lands, one a frame), the rest on later Ticks. Each in its own
        // upload batch, so the clock sees what each one cost.
        m_budgetedWaiting   = 0;
        m_budgetedOversized = 0;
        m_budgetedMs        = 0.0;
        // With loader uploads on: publish the workers' finished ones first, then hand them the next ones (at
        // most kMaxAsyncPromotions in flight); this frame only publishes.
        const bool onLoader = gfx.LoaderUploadsEnabled();
        if (onLoader)
        {
            const auto t0 = std::chrono::steady_clock::now();
            PublishAsyncPromotions(gfx, rm);
            for (TextureEntry* entry : budgeted)
            {
                if (m_asyncPromotions.size() >= kMaxAsyncPromotions) { ++m_budgetedWaiting; continue; }
                StartAsyncPromotion(*entry, static_cast<uint32_t>(entry - m_slots.data()), rm, gfx);
            }
            m_budgetedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            budgeted.clear();
        }
        if (!budgeted.empty())
        {
            static const double s_budgetMs = [] {
                const double v = Sys::CommandLine::Get().GetFloat("-tex_promote_budget_ms", 2.0f);
                return v > 0.0 ? v : 2.0;
            }();
            PROF_SCOPE("Tex.PromoteBudgeted");
            const auto t0 = std::chrono::steady_clock::now();
            size_t done = 0;
            double lastMs = 0.0;   // the next one starts only if one like the last still fits
            for (; done < budgeted.size(); ++done)
            {
                if (done > 0 && std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - t0).count() + lastMs > s_budgetMs)
                    break;
                TextureEntry* entry = budgeted[done];
                const auto tp = std::chrono::steady_clock::now();
                gfx.BeginBufferUploadBatch();
                if (const auto* texRes = rm.Get<TextureResource>(entry->rmHandle))
                    UploadToGPU(*texRes, *entry, gfx, entry->path);
                else
                    LOG_WARNING("TextureSystem::Tick: resource is not a TextureResource");
                gfx.EndBufferUploadBatch();
                if (entry->gpuReady) DropCpuImage(*entry, rm);
                lastMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count();
                constexpr uint64_t kOversizedBytes = 8ull * 1024 * 1024;
                if (entry->gpuBytes > kOversizedBytes) ++m_budgetedOversized;
                // -prefab_preload_log (the preload's diagnostic): each one's cost.
                static const bool s_log = Sys::CommandLine::Get().Has("-prefab_preload_log");
                if (s_log)
                    LOG_INFO("[PRELOAD] texture frame=%llu ms=%.3f bytes=%llu path=%s",
                             static_cast<unsigned long long>(frame),
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp).count(),
                             static_cast<unsigned long long>(entry->gpuBytes), entry->path.c_str());
            }
            m_budgetedWaiting = static_cast<uint32_t>(budgeted.size() - done);
            m_budgetedMs      = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }

        if (!toPromote.empty())
        {
            // Every texture whose read finished since the last Tick, created with initial data in one batch.
            // A cold prefab spawn's next frame can be ~30 textures / ~96 MB / tens of ms here, which is why
            // preloads take the budgeted path.
            PROF_SCOPE("Tex.Promote");
            gfx.BeginBufferUploadBatch();
            for (TextureEntry* entry : toPromote)
            {
                const auto* texRes = rm.Get<TextureResource>(entry->rmHandle);
                if (texRes)
                    UploadToGPU(*texRes, *entry, gfx, entry->path);
                else
                    LOG_WARNING("TextureSystem::Tick: resource is not a TextureResource");
                if (entry->gpuReady) DropCpuImage(*entry, rm);
                if (entry->reloadReceipt != 0)
                {
                    // A reload that went through the ordinary promotion (the
                    // slot never had contents to keep drawing).
                    HotReload::Receipt rc;
                    if (entry->gpuReady)
                    {
                        rc.ok             = 1;
                        rc.effectiveFrame = frame;
                        rc.message        = "in effect from frame " + std::to_string(frame) +
                                            ": the texture had not loaded before, now it has";
                    }
                    else
                    {
                        rc.code    = "reload_error";
                        rc.message = entry->uploadError.empty() ? std::string("the upload failed")
                                                                : "GPU upload: " + entry->uploadError;
                        rc.failed  = 1;
                    }
                    HotReload::Close(entry->reloadReceipt, rc);
                    entry->reloadReceipt = 0;
                }
            }
            gfx.EndBufferUploadBatch();
        }
    }

    // ==== Shutdown ====

    void TextureSystem::Shutdown(IGraphicsDevice& gfx)
    {
        // The loader workers' promotions finish first (they use the device).
        {
            std::unique_lock<std::mutex> lk(m_asyncMutex);
            m_asyncCv.wait(lk, [this] { return m_asyncJobs == 0; });
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& p : m_asyncPromotions)
            if (p->texture.IsValid()) gfx.DestroyTexture(p->texture);
        m_asyncPromotions.clear();
        for (auto& tex : m_pendingDestroy)
            gfx.DestroyTexture(tex);
        m_pendingDestroy.clear();
        for (auto& entry : m_slots)
        {
            if (entry.refCount > 0 && entry.gpuReady)
                gfx.DestroyTexture(entry.texture);
        }
        m_slots.clear();
        m_freeList.clear();
        m_pathHashToSlot.clear();
        m_pendingRmUnload.clear(); // ResourceManager::Shutdown owns the remaining CPU cache.
    }

} // namespace Resource
