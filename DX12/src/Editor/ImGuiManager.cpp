#include "Editor/ImGuiManager.h"
#include "Graphics/GraphicsDX12.h"   // kPresentCompositionFormat: ImGui's RTV format
#include "Graphics/UploadStats.h"    // the backend's texture-upload waits
#include "System/Log.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include "System/CommandLine.h"
#include "System/TaskSystem.h"

namespace
{
    // The DX12 backend uploads a texture it creates or updates by itself:
    // ExecuteCommandLists on the engine's graphics queue, Signal, then
    // WaitForSingleObject(INFINITE) (the tail of ImGui_ImplDX12_UpdateTexture) - the
    // same FIFO stall as GraphicsDX12::FlushUploadAndWait, which never sees it.
    // CommitImGuiTextures commits every texture change before RenderDrawData, so
    // UploadStats counts each wait; the backend's own catch-up loop at the top of
    // RenderDrawData then finds every status OK.
    //
    // Same order, same guard as that loop, so nothing changes but the count: the
    // backend skips the textures of a minimized frame, and so does this. A WantDestroy
    // passes through uncounted (it releases, it does not wait), and so does an upload
    // StreamTextureUpload took (nothing waited).
    bool s_streamedThisCall = false;   // the owner thread's; set by StreamTextureUpload

    void CommitImGuiTextures(const ImDrawData* data)
    {
        if (!data || !data->Textures) return;
        if (data->DisplaySize.x <= 0.0f || data->DisplaySize.y <= 0.0f) return;
        for (ImTextureData* tex : *data->Textures)
        {
            if (tex->Status == ImTextureStatus_OK) continue;
            const bool waits = tex->Status == ImTextureStatus_WantCreate ||
                               tex->Status == ImTextureStatus_WantUpdates;
            const auto t0 = std::chrono::steady_clock::now();
            s_streamedThisCall = false;
            ImGui_ImplDX12_UpdateTexture(tex);
            if (waits && !s_streamedThisCall)
                UploadStats::NoteImGuiWait(std::chrono::steady_clock::now() - t0);
        }
    }

    // The backend's texture uploads - its atlas at boot, every glyph ImGui 1.92 adds
    // later - go through the engine's streaming upload (recorded first in this frame's
    // render half, ahead of the primary list that draws ImGui; no CPU wait) instead of
    // ExecuteCommandLists + Signal + WaitForSingleObject. Declined (the backend then
    // uploads and waits) before the device is known, under -imgui_syncupload, or when
    // the uploader refuses the call: the -renderowneroff UI build holds a
    // device-execution scope. Declining one update after streaming another is safe for
    // ImGui's own textures: it writes only regions never written before, so the two
    // cannot overlap.
    bool StreamTextureUpload(ImGui_ImplDX12_InitInfo* info, ID3D12Resource* texture, int x, int y, int w, int h,
                             const void* pixels, int pitch)
    {
        const ImGuiManager* mgr = static_cast<const ImGuiManager*>(info->UserData);
        GraphicsDX12* gfx = mgr ? mgr->GetStreamUploader() : nullptr;
        if (!gfx || !texture || !pixels || w <= 0 || h <= 0 || pitch <= 0) return false;
        // Asked first: under -renderowneroff the UI build is refused every time,
        // which is expected - the backend's own upload is the fallback - and
        // must not log the uploader's misuse warning.
        if (!gfx->StreamUploadAcceptsCallsHere()) return false;
        RHI::SubresourceData sub{};
        sub.dataPtr    = pixels;
        sub.rowPitch   = static_cast<uint32_t>(pitch);
        sub.slicePitch = static_cast<uint32_t>(pitch) * static_cast<uint32_t>(h);
        StreamUpload::Desc d{};
        d.planes = &sub;
        d.rect   = { static_cast<uint32_t>(x), static_cast<uint32_t>(y),
                     static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
        d.policy = StreamUpload::Policy::MustDeliver;   // new glyphs must land, never merged or dropped
        const StreamUpload::Result r =
            gfx->QueueNativeTextureUpload(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, d);
        const bool taken = r == StreamUpload::Result::Queued || r == StreamUpload::Result::Fallback ||
                           r == StreamUpload::Result::CarriedOver;
        s_streamedThisCall = taken;
        if (taken) UploadStats::imguiStreamed.fetch_add(1, std::memory_order_relaxed);
        return taken;
    }

    void SrvAllocFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
    {
        ImGuiManager* mgr = static_cast<ImGuiManager*>(info->UserData);
        DescriptorAllocation a = mgr->GetSrvAllocator()->Allocate(1);
        a.CopyToGpu();
        // The backend writes this SRV itself, past the allocator, so say what it
        // writes: always a Texture2D of an 8-bit UNORM format (imgui_impl_dx12's font /
        // texture uploads).
        mgr->GetSrvAllocator()->NoteKind(a.GetGpuCpuHandle(), RHI::Bindless::Kind::Tex2DFloat);
        *outCpuHandle = a.GetGpuCpuHandle();   // GPU-heap CPU handle for ImGui direct writes
        *outGpuHandle = a.GetGpuHandle();
        mgr->AddSrvAllocation(std::move(a));
    }

    void SrvFreeFn(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE /*gpu_handle*/)
    {
        ImGuiManager* mgr = static_cast<ImGuiManager*>(info->UserData);
        mgr->FreeSrvAllocation(cpuHandle);
    }
}

void ImGuiManager::Initialize(
    HWND hWnd,
    ID3D12Device* device,
    ID3D12CommandQueue* commandQueue,
    DescriptorHeapAllocator& srvAllocator,
    UINT frameCount)
{
    if (m_initialized)
        return;

    m_srvAllocator = &srvAllocator;

    if (!ImGui_ImplWin32_Init(hWnd))
    {
        LOG_ERROR("ImGui Win32 init failed");
        return;
    }

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = commandQueue;
    info.NumFramesInFlight = frameCount;
    // MUST match SurfaceDX12::kPresentCompositionFormat, NOT the swap chain.
    //
    // ImGui bakes RTVFormat into bd->RTVFormat at init and into its PSO and has no
    // setter: changing it needs a full Shutdown/Init, which frees every backend SRV
    // descriptor, including those the current frame's ImGui::Image / AddImage calls
    // already captured. So the editor never renders ImGui to the back buffer: it
    // renders to a composition buffer whose format is a compile-time constant, and
    // GraphicsDX12 resolves that to the (possibly HDR, possibly 10-bit) back buffer
    // through the display encode. That makes HDR switchable at runtime.
    //
    // A mismatch between these two formats is a PSO/RTV mismatch - a
    // device-removal-class bug every editor frame, not a colour shift.
    info.RTVFormat = SurfaceDX12::kPresentCompositionFormat;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.UserData = this;
    info.SrvDescriptorHeap = srvAllocator.GetGpuHeap();
    info.SrvDescriptorAllocFn = SrvAllocFn;
    info.SrvDescriptorFreeFn = SrvFreeFn;
    // -imgui_syncupload: the backend's own upload-and-wait (the A/B arm, and the
    // flood pack's reverse arm).
    if (m_streamGfx && !Sys::CommandLine::Get().Has("-imgui_syncupload"))
        info.TextureUploadFn = StreamTextureUpload;

    if (!ImGui_ImplDX12_Init(&info))
    {
        ImGui_ImplWin32_Shutdown();
        LOG_ERROR("ImGui DX12 init failed");
        return;
    }

    m_initialized = true;
}

struct ImGuiManager::DrawSnapshot
{
    ImDrawData data;
    ImGuiViewport viewport;

    std::vector<std::unique_ptr<ImDrawList>> lists;
    std::vector<ImDrawList*> sources; // restoration only; never read by recording job
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
    uint64_t frame = 0;

    ~DrawSnapshot()
    {
        // RenderDrawData borrows the real viewport's backend data. This copy
        // never owns it; detach before ImGuiViewport's ownership assertion.
        viewport.RendererUserData = nullptr;
        viewport.PlatformUserData = nullptr;
    }
};

std::function<void()> ImGuiManager::PrepareRenderSnapshot(ID3D12GraphicsCommandList* commandList,
    DescriptorHeapAllocator& srvAllocator, uint64_t frame, uint64_t placeholderTexture, uint64_t sceneTexture)
{
    if (!m_initialized) return {};
    if (m_snapshotInFlight) throw std::logic_error("ImGui snapshot already in flight");
    m_snapshotNeedsOwnerCallbacks = false;
    ImGui::Render();
    ImDrawData* src = ImGui::GetDrawData();
    if (!src || !src->Valid) return {};
    if (placeholderTexture) {
        if (!sceneTexture) throw std::logic_error("Missing scene texture for owner callback recording");
        for (auto* list : src->CmdLists)
            for (auto& command : list->CmdBuffer)
                if (command.GetTexID() == static_cast<ImTextureID>(placeholderTexture))
                    command.TexRef = ImTextureRef(static_cast<ImTextureID>(sceneTexture));
    }
    // Match the backend's minimized/empty early exit before allocating or
    // publishing a job that cannot record any pixels.
    if (int(src->DisplaySize.x * src->FramebufferScale.x) <= 0 ||
        int(src->DisplaySize.y * src->FramebufferScale.y) <= 0 || src->CmdListsCount == 0) {
        if (Sys::CommandLine::Get().Has("-materialcpuverify"))
            LOG_INFO("[EDITOR_DRAW_SNAPSHOT] frame=%llu job=0 empty=1 match=1", frame);
        return {};
    }
    // Texture uploads/descriptors and arbitrary application callbacks retain
    // owner affinity. Only the documented reset-state sentinel is worker safe.
    for (const ImDrawList* list : src->CmdLists)
        for (const ImDrawCmd& cmd : list->CmdBuffer)
            if (cmd.UserCallback && cmd.UserCallback != ImDrawCallback_ResetRenderState) {
                m_snapshotNeedsOwnerCallbacks = true;
                // An early snapshot may not run arbitrary callbacks before
                // their command list exists. The caller must drain this frame
                // before allowing the next simulation to mutate its World.
                if (!commandList) return {};
                BindHeaps(commandList, srvAllocator.GetGpuHeap());
                CommitImGuiTextures(src);
                ImGui_ImplDX12_RenderDrawData(src, commandList);
                if (Sys::CommandLine::Get().Has("-materialcpuverify"))
                    LOG_INFO("[EDITOR_DRAW_SNAPSHOT] frame=%llu job=0 callback=1 match=1", frame);
                return {};
            }
    CommitImGuiTextures(src);

    if (!m_drawSnapshot) m_drawSnapshot = std::make_shared<DrawSnapshot>();
    auto snapshot = m_drawSnapshot;
    snapshot->commandList = commandList;
    snapshot->frame = frame;
    auto& data = snapshot->data;
    const size_t count = static_cast<size_t>(src->CmdListsCount);
    // All allocations happen before moving the first geometry buffer, so a
    // failed preparation leaves ImGui's original draw lists intact.
    while (snapshot->lists.size() < count) snapshot->lists.push_back(std::make_unique<ImDrawList>(nullptr));
    snapshot->sources.resize(count);
    data.CmdLists.resize(static_cast<int>(count));
    size_t commandBytes = 0;
    const bool verify = Sys::CommandLine::Get().Has("-materialcpuverify");
    auto hashBytes = [](uint64_t& hash, const void* bytes, size_t size) {
        const auto* p = static_cast<const unsigned char*>(bytes);
        for (size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    uint64_t expected = 1469598103934665603ull;
    for (size_t i = 0; i < count; ++i) {
        auto* source = src->CmdLists[static_cast<int>(i)];
        auto* target = snapshot->lists[i].get();
        snapshot->sources[i] = source;
        target->CmdBuffer.resize(source->CmdBuffer.Size);
        const size_t bytes = size_t(source->CmdBuffer.Size) * sizeof(ImDrawCmd);
        if (bytes) std::memcpy(target->CmdBuffer.Data, source->CmdBuffer.Data, bytes);
        commandBytes += bytes;
        for (auto& command : target->CmdBuffer) command.TexRef = ImTextureRef(command.GetTexID());
        target->Flags = source->Flags;
        data.CmdLists[static_cast<int>(i)] = target;
        if (verify) {
            hashBytes(expected, source->IdxBuffer.Data, size_t(source->IdxBuffer.Size) * sizeof(ImDrawIdx));
            hashBytes(expected, source->VtxBuffer.Data, size_t(source->VtxBuffer.Size) * sizeof(ImDrawVert));
        }
    }
    data.Valid = true;
    data.CmdListsCount = src->CmdListsCount;
    data.TotalIdxCount = src->TotalIdxCount; data.TotalVtxCount = src->TotalVtxCount;
    data.DisplayPos = src->DisplayPos; data.DisplaySize = src->DisplaySize;
    data.FramebufferScale = src->FramebufferScale;
    snapshot->viewport.RendererUserData = src->OwnerViewport->RendererUserData;
    data.OwnerViewport = &snapshot->viewport;
    data.Textures = nullptr; // texture changes were committed on the owner
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap = srvAllocator.GetGpuHeap();
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> samplerHeap = m_samplerHeap;   // see SetSamplerHeap
    std::function<void()> record = [snapshot, heap, samplerHeap, frame, expected, verify, hashBytes, commandBytes] {
        ZoneScopedN("EditorImGui.RecordSnapshot");
        if (!snapshot->commandList) throw std::logic_error("Unbound ImGui frame snapshot");
        if (verify) {
            uint64_t actual = 1469598103934665603ull;
            for (const ImDrawList* list : snapshot->data.CmdLists) {
                hashBytes(actual, list->IdxBuffer.Data, size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
                hashBytes(actual, list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
            }
            if (actual != expected) throw std::logic_error("ImGui geometry transfer mismatch");
            LOG_INFO("[EDITOR_DRAW_SNAPSHOT] frame=%llu job=1 callback=0 vertices=%d indices=%d geometryCopiedBytes=0 commandBytes=%zu width=%.0f height=%.0f match=1",
                frame, snapshot->data.TotalVtxCount, snapshot->data.TotalIdxCount, commandBytes, snapshot->data.DisplaySize.x, snapshot->data.DisplaySize.y);
        }
        ID3D12DescriptorHeap* heaps[] = {heap.Get(), samplerHeap.Get()};
        snapshot->commandList->SetDescriptorHeaps(samplerHeap ? 2u : 1u, heaps);
        ImGui_ImplDX12_RenderDrawData(&snapshot->data, snapshot->commandList.Get());
    };
    for (size_t i = 0; i < count; ++i) {
        snapshot->lists[i]->IdxBuffer.swap(snapshot->sources[i]->IdxBuffer);
        snapshot->lists[i]->VtxBuffer.swap(snapshot->sources[i]->VtxBuffer);
    }
    m_snapshotInFlight = true;
    return record;
}

void ImGuiManager::BindRenderSnapshot(ID3D12GraphicsCommandList* commandList, uint64_t frame,
    uint64_t placeholderTexture, uint64_t sceneTexture)
{
    if (!m_snapshotInFlight || !commandList || m_drawSnapshot->frame != frame || TaskSystem::OnWorkerThread())
        throw std::logic_error("Invalid ImGui snapshot binding or frame generation");
    if (placeholderTexture && !sceneTexture) throw std::logic_error("Missing frame scene texture");
    m_drawSnapshot->commandList = commandList;
    if (placeholderTexture)
        for (auto* list : m_drawSnapshot->data.CmdLists)
            for (auto& command : list->CmdBuffer)
                if (command.GetTexID() == static_cast<ImTextureID>(placeholderTexture))
                    command.TexRef = ImTextureRef(static_cast<ImTextureID>(sceneTexture));
}

void ImGuiManager::CompleteRenderSnapshot()
{
    if (!m_snapshotInFlight) return;
    if (TaskSystem::OnWorkerThread()) throw std::logic_error("ImGui snapshot restoration requires owner");
    for (size_t i = 0; i < m_drawSnapshot->sources.size(); ++i) {
        m_drawSnapshot->lists[i]->IdxBuffer.swap(m_drawSnapshot->sources[i]->IdxBuffer);
        m_drawSnapshot->lists[i]->VtxBuffer.swap(m_drawSnapshot->sources[i]->VtxBuffer);
    }
    m_drawSnapshot->sources.clear();
    m_drawSnapshot->commandList.Reset();
    m_snapshotInFlight = false;
}

void ImGuiManager::Shutdown()
{
    if (m_snapshotInFlight) throw std::logic_error("Drain ImGui recording before shutdown");
    m_drawSnapshot.reset();
    if (!m_initialized)
        return;

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyPlatformWindows();
    m_srvAllocations.clear();
    m_srvAllocator = nullptr;
    m_initialized = false;
}

void ImGuiManager::DestroyContext()
{
    ImGui::DestroyContext();
}

void ImGuiManager::BeginFrame()
{
    if (m_snapshotInFlight) throw std::logic_error("Drain ImGui recording before NewFrame");
    if (!m_initialized)
        return;
    ImGui_ImplWin32_NewFrame();
    // -hidden_ui (SetMinimizedDisplaySize): the Win32 backend just took the
    // client rect, which a minimized window reports as 0x0.
    ImGuiIO& io = ImGui::GetIO();
    if (m_minimizedW > 0.f && (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f))
    {
        io.DisplaySize             = ImVec2(m_minimizedW, m_minimizedH);
        io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    }
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
}

void ImGuiManager::Render(ID3D12GraphicsCommandList* commandList, DescriptorHeapAllocator& srvAllocator)
{
    if (!m_initialized)
        return;
    ImGui::Render();
    BindHeaps(commandList, srvAllocator.GetGpuHeap());
    CommitImGuiTextures(ImGui::GetDrawData());
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

void ImGuiManager::BindHeaps(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap) const
{
    // SetDescriptorHeaps REPLACES the list's bound heaps: binding the SRV heap
    // alone would unbind the engine's sampler heap for whatever follows on this
    // list (SetSamplerHeap).
    ID3D12DescriptorHeap* heaps[] = { srvHeap, m_samplerHeap };
    commandList->SetDescriptorHeaps(m_samplerHeap ? 2u : 1u, heaps);
}

void ImGuiManager::AddSrvAllocation(DescriptorAllocation a)
{
    m_srvAllocations.push_back({ a.GetGpuCpuHandle().ptr, std::move(a) });
}

void ImGuiManager::FreeSrvAllocation(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
    const std::size_t ptr = cpuHandle.ptr;
    auto it = std::find_if(m_srvAllocations.begin(), m_srvAllocations.end(),
        [ptr](const std::pair<std::size_t, DescriptorAllocation>& p) { return p.first == ptr; });
    if (it != m_srvAllocations.end())
    {
        it->second.Free();
        m_srvAllocations.erase(it);
    }
}
