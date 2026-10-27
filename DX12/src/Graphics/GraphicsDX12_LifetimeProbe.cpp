// GraphicsDX12_LifetimeProbe.cpp - -rhi_lifetime_probe[=F]: the device's resource-lifetime guards
// exercised on purpose, once, from render frame F on (default 30). Harness only: compiled out of
// Retail. Called from BeginFrame right after the release drains.
//
// Stage 1, at frame F:
//   dup    a buffer and a texture, each destroyed through two copies of its handle: the second
//          Destroy must be refused.
//   oob    a buffer and a texture handle naming an id past the end of its pool: both refused.
//   slot   a buffer and a texture destroyed once, then their id queued for release a second time,
//          as a second release path naming the same id would: the drain must put it on its free
//          list once.
//   desc   a CBV_SRV_UAV descriptor freed through two copies of its allocation: the second Free
//          must be refused (descRefused) and the slot be on the allocator's free list once
//          (descCover).
//   fence  a committed buffer handed to DeferReleaseResource against the graphics queue (0), at
//          the value two batches past the last one that queue signalled. It must stay held until
//          that queue's fence reaches the value (fenceEarly=0) and go within kFenceWindow frames
//          (fenceReleased=1). q0 / frameFence are the queue's fence and the frame-pacing fence,
//          completed values when it was deferred: the two counters a queue-0 release must not mix.
// Stage 2, at the first frame whose drain retired stage 1's releases: each of the four ids must be
// on its free list once, and the next two buffers (textures) created must get two ids. Then:
//   placed a 256x256 texture placed at offset 0 of a 1 MiB heap tagged, as a TransientAliasArena
//          heap is, with a usable size of 64 KiB: inside the heap, across its guard band.
//          CreateTexturePlaced must refuse it (placedRefused).
//
// The line, once stage 2 ran and the fence part ended: [RHI_LIFETIME_PROBE] at= retiredAt= made=
// dupRefused= oobRefused= slotDupes= maxFree= bufShared= texShared= descRefused= descCover=
// placedRefused= fenceReleased= fenceEarly= fenceFrames= q0= frameFence= unguarded=
// Reverse arm: -rhi_lifetime_probe_unguarded turns the Destroy refusals, the drain's dedupe and the
// placement bounds refusal off, the old behaviour: each second Destroy and second queueing puts its
// id on the free list twice, two new resources share it, and the stray placement is created. oob
// is skipped there (oobRefused=-1): an unchecked Destroy indexes the pool past its end. desc and
// fence have no switch: the allocator's reverse arm is tests/unit/descriptor_slot_region_test.py's
// mutants, and the fence's is a build without the fix.

#include "Graphics/GraphicsDX12.h"

#if defined(WITH_HARNESS) && WITH_HARNESS

#include "Graphics/GraphicsDX12Internal.h"
#include "Graphics/TransientAliasArena.h"   // kUsableSizeTag: the placed part's heap
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>
#include <array>

namespace
{
    // Frames the fence part waits for its release before calling the buffer held for good.
    constexpr uint64_t kFenceWindow = 30;

    struct LifetimeProbeState
    {
        int                     stage     = 0;   // 0 waiting for F, 1 stage 1 ran, 2 stage 2 ran
        bool                    logged    = false;
        uint64_t                retiredAt = 0;   // the first frame whose drain retires stage 1's releases
        bool                    made      = false;
        std::array<uint32_t, 2> bufferIds{ ~0u, ~0u };    // dup, slot
        std::array<uint32_t, 2> textureIds{ ~0u, ~0u };
        uint32_t                dupRefused    = 0;
        int                     oobRefused    = -1;
        uint32_t                dupesBefore   = 0;
        int                     descCover     = -1;   // free-list blocks holding the twice-freed slot
        int                     descRefused   = -1;   // the second Free, refused
        int                     placedRefused = -1;

        // ---- fence ----
        const ID3D12Resource*   fenceRes      = nullptr;   // compared, never dereferenced
        uint64_t                fenceValue    = 0;
        uint64_t                fenceFrom     = 0;          // the frame it was deferred
        uint64_t                q0AtDefer     = 0;
        uint64_t                frameAtDefer  = 0;
        int                     fenceReleased = -1;         // -1 nothing deferred, 0 held, 1 released
        int                     fenceEarly    = -1;
        long long               fenceFrames   = -1;
        bool                    fenceDone     = true;

        // ---- stage 2 ----
        uint64_t                stage2At  = 0;
        uint32_t                slotDupes = 0;
        uint32_t                maxFree   = 0;
        bool                    bufShared = false;
        bool                    texShared = false;
    };

    RHI::GPUBufferDesc ProbeBufferDesc()
    {
        RHI::GPUBufferDesc d{};
        d.size      = 256;
        d.stride    = 16;
        d.usage     = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.debugName = "LifetimeProbe.Buffer";
        return d;
    }

    RHI::TextureDesc ProbeTextureDesc()
    {
        RHI::TextureDesc d{};
        d.width     = 4;
        d.height    = 4;
        d.format    = RHI::Format::R8G8B8A8_UNORM;
        d.usage     = RHI::Usage::DEFAULT;
        d.bindFlags = RHI::BindFlag::SHADER_RESOURCE;
        d.layout    = RHI::ResourceState::SHADER_RESOURCE;
        d.debugName = "LifetimeProbe.Texture";
        return d;
    }

    uint32_t CountOf(const std::vector<uint32_t>& list, uint32_t id)
    {
        return static_cast<uint32_t>(std::count(list.begin(), list.end(), id));
    }
}

void GraphicsDX12::RunLifetimeProbe()
{
    static LifetimeProbeState s_state;
    static const int s_at = (std::max)(0, Sys::CommandLine::Get().GetInt("-rhi_lifetime_probe", 30));
    // The reverse arm, read here as well as at the device's init: an unguarded run skips oob.
    static const bool s_unguarded = Sys::CommandLine::Get().Has("-rhi_lifetime_probe_unguarded");
    LifetimeProbeState& s = s_state;
    const uint64_t frame = m_renderFrame.load(std::memory_order_relaxed);
    if (s.logged) return;

    if (s.stage == 0 && frame >= static_cast<uint64_t>(s_at))
    {
        s.stage = 1;
        const RHI::GPUBufferDesc bd = ProbeBufferDesc();
        const RHI::TextureDesc   td = ProbeTextureDesc();
        RHI::GPUBuffer bufDup, bufSlot;
        RHI::Texture   texDup, texSlot;
        s.made = CreateBuffer(bd, bufDup) && CreateBuffer(bd, bufSlot) &&
                 CreateTexture(td, texDup) && CreateTexture(td, texSlot);
        if (!ENGINE_CHECK(s.made, "[RHI_LIFETIME_PROBE] could not create its buffers and textures"))
        {
            s.logged = true;
            LOG_INFO("[RHI_LIFETIME_PROBE] at=%llu made=0", static_cast<unsigned long long>(frame));
            return;
        }
        s.bufferIds  = { bufDup.handleId, bufSlot.handleId };
        s.textureIds = { texDup.handleId, texSlot.handleId };
        // Every release below lands in the bucket stamped now.
        s.retiredAt = ReleaseStampFrame() + kFrameCount;

        // dup: two copies of each handle, both destroyed.
        const uint32_t refusedBefore = m_destroyRefusals.load();
        RHI::GPUBuffer bufCopy = bufDup;
        RHI::Texture   texCopy = texDup;
        DestroyBuffer(bufDup);
        DestroyBuffer(bufCopy);
        DestroyTexture(texDup);
        DestroyTexture(texCopy);
        s.dupRefused = m_destroyRefusals.load() - refusedBefore;

        // oob: ids no Create handed out.
        if (!s_unguarded)
        {
            const uint32_t oobBefore = m_destroyRefusals.load();
            RHI::GPUBuffer strayBuf;
            strayBuf.type     = RHI::GPUResource::Type::Buffer;
            strayBuf.handleId = static_cast<uint32_t>(m_bufferPool.size()) + 7;
            RHI::Texture strayTex;
            strayTex.type     = RHI::GPUResource::Type::Texture;
            strayTex.handleId = static_cast<uint32_t>(m_texturePool.size()) + 7;
            DestroyBuffer(strayBuf);
            DestroyTexture(strayTex);
            s.oobRefused = static_cast<int>(m_destroyRefusals.load() - oobBefore);
        }

        // slot: destroyed once, the id queued a second time.
        const uint32_t slotBuf = bufSlot.handleId;
        const uint32_t slotTex = texSlot.handleId;
        DestroyBuffer(bufSlot);
        DestroyTexture(texSlot);
        {
            std::lock_guard<std::mutex> lock(m_deferredMutex);
            DeferredReleaseList& dr = PendingReleaseBucket();
            dr.bufferSlots.push_back(slotBuf);
            dr.textureSlots.push_back(slotTex);
        }
        s.dupesBefore = m_freeListDupes.load();

        // desc: a CBV_SRV_UAV slot freed through two copies of its allocation. Free is immediate.
        DescriptorAllocation desc = m_cbvSrvUavAllocator.Allocate(1);
        if (desc.IsValid())
        {
            const DescriptorAllocation slot  = desc;   // names the slot after both frees
            DescriptorAllocation       other = desc;   // the second owner
            const unsigned long long descBefore = m_cbvSrvUavAllocator.GetDoubleFreeCount();
            desc.Free();
            other.Free();
            s.descRefused = static_cast<int>(m_cbvSrvUavAllocator.GetDoubleFreeCount() - descBefore);
            s.descCover   = static_cast<int>(m_cbvSrvUavAllocator.FreeBlocksCovering(slot));
        }

        // fence: two batches past the graphics queue's last signal, so it must stay held a while.
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 256;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> res;
        if (SUCCEEDED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                        D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&res))))
        {
            res->SetName(L"LifetimeProbe.FenceKeyed");
            s.fenceRes      = res.Get();
            s.fenceValue    = GetLastSignaledFenceValue(0) + 2;
            s.fenceFrom     = frame;
            s.q0AtDefer     = m_queueFences[0]->GetCompletedValue();
            s.frameAtDefer  = m_fence->GetCompletedValue();
            s.fenceReleased = 0;
            s.fenceDone     = false;
            DeferReleaseResource(std::move(res), 0, s.fenceValue);
        }
        return;
    }
    if (s.stage == 0) return;

    // fence: released by this frame's drain, or still held?
    if (!s.fenceDone)
    {
        bool held = false;
        {
            std::lock_guard<std::mutex> lock(m_deferredMutex);
            for (const FenceKeyedRelease& r : m_fenceKeyedReleases)
                if (r.resource.Get() == s.fenceRes) { held = true; break; }
        }
        if (!held)
        {
            // The drain read the queue's fence before this; it only grows, so a value below the
            // target now was below it then too.
            s.fenceReleased = 1;
            s.fenceEarly    = m_queueFences[0]->GetCompletedValue() < s.fenceValue ? 1 : 0;
            s.fenceFrames   = static_cast<long long>(frame - s.fenceFrom);
            s.fenceDone     = true;
        }
        else if (frame >= s.fenceFrom + kFenceWindow)
        {
            s.fenceDone = true;
        }
    }

    if (s.stage == 1 && frame >= s.retiredAt)
    {
        s.stage    = 2;
        s.stage2At = frame;
        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);
            for (uint32_t id : s.bufferIds)  s.maxFree = (std::max)(s.maxFree, CountOf(m_bufferFreeList, id));
            for (uint32_t id : s.textureIds) s.maxFree = (std::max)(s.maxFree, CountOf(m_textureFreeList, id));
        }
        s.slotDupes = m_freeListDupes.load() - s.dupesBefore;

        // The next two of each must get two ids.
        const RHI::GPUBufferDesc bd = ProbeBufferDesc();
        const RHI::TextureDesc   td = ProbeTextureDesc();
        RHI::GPUBuffer b1, b2;
        RHI::Texture   t1, t2;
        CreateBuffer(bd, b1);
        CreateBuffer(bd, b2);
        CreateTexture(td, t1);
        CreateTexture(td, t2);
        s.bufShared = b1.IsValid() && b1.handleId == b2.handleId;
        s.texShared = t1.IsValid() && t1.handleId == t2.handleId;
        // A shared id is destroyed once: both handles name the same record.
        DestroyBuffer(b1);
        if (!s.bufShared) DestroyBuffer(b2);
        DestroyTexture(t1);
        if (!s.texShared) DestroyTexture(t2);

        // placed: inside the heap, across the guard band its usable-size tag sets. After the shared-id
        // check: a placement the reverse arm creates would put its own id first on the free list.
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes     = 1ull << 20;
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment       = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        hd.Flags           = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        if (SUCCEEDED(m_device->CreateHeap(&hd, IID_PPV_ARGS(&heap))))
        {
            const uint64_t usable = 64ull << 10;
            heap->SetPrivateData(TransientAliasArena::kUsableSizeTag, sizeof(usable), &usable);
            RHI::TextureDesc pd = ProbeTextureDesc();
            pd.width     = 256;
            pd.height    = 256;
            pd.debugName = "LifetimeProbe.Placed";
            RHI::Texture placed;
            const bool created = CreateTexturePlaced(pd, heap.Get(), 0, placed);
            s.placedRefused = created ? 0 : 1;
            if (created) DestroyTexture(placed);
        }
    }

    if (s.stage != 2 || !s.fenceDone)
        return;
    s.logged = true;
    LOG_INFO("[RHI_LIFETIME_PROBE] at=%llu retiredAt=%llu made=%d dupRefused=%u oobRefused=%d slotDupes=%u "
             "maxFree=%u bufShared=%d texShared=%d descRefused=%d descCover=%d placedRefused=%d "
             "fenceReleased=%d fenceEarly=%d fenceFrames=%lld q0=%llu frameFence=%llu unguarded=%d",
             static_cast<unsigned long long>(s.stage2At), static_cast<unsigned long long>(s.retiredAt),
             s.made ? 1 : 0, s.dupRefused, s.oobRefused, s.slotDupes, s.maxFree, s.bufShared ? 1 : 0,
             s.texShared ? 1 : 0, s.descRefused, s.descCover, s.placedRefused, s.fenceReleased, s.fenceEarly,
             s.fenceFrames, static_cast<unsigned long long>(s.q0AtDefer),
             static_cast<unsigned long long>(s.frameAtDefer), s_unguarded ? 1 : 0);
}

#endif
