#pragma once

// ImGui lifecycle and DX12/Win32 backend management
// - Context is created externally before Initialize() (by Window)
// - Initialize sets up Win32 + DX12 backends; Shutdown tears down backends and platform windows
// - DestroyContext() must be called by the owner after DestroyWindow

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d12.h>
#include <vector>
#include <cstddef>
#include <functional>
#include <memory>

#include "Graphics/DescriptorHeapAllocator.h"

class GraphicsDX12;

class ImGuiManager
{
public:
    ImGuiManager() = default;
    ~ImGuiManager() = default;
    ImGuiManager(const ImGuiManager&) = delete;
    ImGuiManager& operator=(const ImGuiManager&) = delete;

    // Initialize: requires an existing ImGui context (e.g., Window already called CreateContext). Sets up Win32 + DX12 backends.
    void Initialize(
        HWND hWnd,
        ID3D12Device* device,
        ID3D12CommandQueue* commandQueue,
        DescriptorHeapAllocator& srvAllocator,
        UINT frameCount
    );

    // Shutdown backends and platform windows; does not destroy the context (call DestroyContext() after DestroyWindow).
    void Shutdown();

    // Destroy the ImGui context; must be called after DestroyWindow(hwnd).
    void DestroyContext();

    // Update stage: NewFrame (Win32 + DX12 + ImGui::NewFrame).
    void BeginFrame();

    // Harness -hidden_ui: the size ImGui lays out at while the window is minimized.
    // A minimized window's client rect is 0x0 (-hidden is SW_SHOWMINNOACTIVE), and
    // ImGui then lays out and draws NOTHING; with this set (the back buffer's size)
    // the editor draws its panels as a restored window would - what the pipe's app_ui
    // capture shows. 0 = off.
    void SetMinimizedDisplaySize(float width, float height) { m_minimizedW = width; m_minimizedH = height; }

    // Final render pass before Present: Render + ImGui_ImplDX12_RenderDrawData.
    void Render(ID3D12GraphicsCommandList* commandList, DescriptorHeapAllocator& srvAllocator);
    // Moves geometry into a frame snapshot; only command metadata is copied.
    // CompleteRenderSnapshot must run on the owner after the returned job ends,
    // before NewFrame, shutdown, or any other ImGui backend operation.
    std::function<void()> PrepareRenderSnapshot(ID3D12GraphicsCommandList* commandList,
        DescriptorHeapAllocator& srvAllocator, uint64_t frame,
        uint64_t placeholderTexture = 0, uint64_t sceneTexture = 0);
    void CompleteRenderSnapshot();
    // Bind the already-frozen draw data once this frame's output/CL exists.
    void BindRenderSnapshot(ID3D12GraphicsCommandList* commandList, uint64_t frame,
        uint64_t placeholderTexture = 0, uint64_t sceneTexture = 0);
    bool HasPreparedSnapshot() const { return m_snapshotInFlight; }
    bool SnapshotNeedsOwnerCallbacks() const { return m_snapshotNeedsOwnerCallbacks; }

    bool IsInitialized() const { return m_initialized; }

    // For ImGui DX12 backend callbacks (SrvDescriptorAllocFn / SrvDescriptorFreeFn).
    void AddSrvAllocation(DescriptorAllocation a);
    void FreeSrvAllocation(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    DescriptorHeapAllocator* GetSrvAllocator() const { return m_srvAllocator; }

    // The engine's shader-visible sampler heap, bound beside the SRV heap on every
    // list ImGui records into. The engine's shared root signatures index both heaps
    // directly, and such a signature needs its heaps set before it: binding the SRV
    // heap alone would leave a later shared-signature pass on the same list without
    // its sampler heap.
    void SetSamplerHeap(ID3D12DescriptorHeap* heap) { m_samplerHeap = heap; }

    // The device whose streaming upload takes the backend's texture updates (no CPU
    // wait). Set before Initialize; null (or -imgui_syncupload) leaves the backend
    // uploading and waiting.
    void SetStreamUploader(GraphicsDX12* gfx) { m_streamGfx = gfx; }
    GraphicsDX12* GetStreamUploader() const { return m_streamGfx; }

private:
    struct DrawSnapshot;

    // SetDescriptorHeaps with @p srvHeap and, when set, m_samplerHeap.
    void BindHeaps(ID3D12GraphicsCommandList* commandList, ID3D12DescriptorHeap* srvHeap) const;

    bool m_initialized{ false };
    float m_minimizedW = 0.f, m_minimizedH = 0.f;   // SetMinimizedDisplaySize

    std::shared_ptr<DrawSnapshot> m_drawSnapshot;
    bool m_snapshotInFlight = false;
    bool m_snapshotNeedsOwnerCallbacks = false;
    DescriptorHeapAllocator* m_srvAllocator{ nullptr };
    ID3D12DescriptorHeap* m_samplerHeap{ nullptr };   // SetSamplerHeap
    GraphicsDX12* m_streamGfx{ nullptr };             // SetStreamUploader
    std::vector<std::pair<std::size_t, DescriptorAllocation>> m_srvAllocations;
};
