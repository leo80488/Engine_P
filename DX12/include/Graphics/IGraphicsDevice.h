#pragma once

// IGraphicsDevice - the platform-agnostic graphics device interface. Every method takes RHI
// descriptor structs and resource handles (GraphicsStruct.h), so callers never see backend types.
//
// Multi-threaded recording model:
//   BeginFrame()                          - CPU sync, opens the primary command list (CommandList{0}).
//   BeginCommandList(queue)               - opens another list from the pool; safe from worker
//                                           threads concurrently.
//   AddCommandListDependency(waiter, dep) - waiter executes after dep on the GPU: same queue by
//                                           topological submission order, cross queue by a fence
//                                           Wait/Signal that EndFrame inserts.
//   Every draw / bind / barrier / state call takes a CommandList; threads may record different
//   lists in parallel.
//   EndFrame()                            - sorts the open lists by their declared dependencies,
//                                           inserts the cross-queue fences, closes and submits them
//                                           all, then presents.

#include "Graphics/BindlessKind.h"   // RHI::BindlessHandle<K>
#include "Graphics/GraphicsStruct.h"
#include "Graphics/GpuTimingView.h"   // GetGPUTimings
#include "Diagnostics/GpuCounters.h"   // Diag::Prof::GpuCounters
#include "Graphics/HDROutput.h"
#include "Graphics/ISurface.h"   // MainSurface(): presentation
#include "Graphics/StreamUploadTypes.h"   // QueueTextureUpload
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace GfxDiag { struct FrameSnapshot; }   // Graphics/FrameSnapshot.h
class TransientAliasArena;                      // Graphics/TransientAliasArena.h

namespace RHI
{
    // Optional backend features. Passes QUERY these rather than downcast: an RTTI probe against a
    // concrete backend type silently reports "false" on any other backend. All-false defaults make a
    // backend that ignores this struct a baseline device.
    struct DeviceCapabilities
    {
        bool meshShader                = false;  // DispatchMesh / amplification shaders
        bool raytracing                = false;  // DXR-equivalent inline/pipeline RT
        bool inlineRaytracing          = false;  // DXR Tier 1.1 RayQuery
        bool unboundedDescriptorArrays = false;  // Resource Binding Tier 3
        // D3D12_OPTIONS4::Native16BitShaderOpsSupported: min16 / half as real 16-bit types
        // (-enable-16bit-types). RTXDI's ReSTIR PT needs it; without it that core stays inert.
        bool native16BitShaderOps      = false;
        // D3D12_OPTIONS1::Int64ShaderOps and OPTIONS11::AtomicInt64OnDescriptorHeapResourceSupported:
        // 64-bit atomics on a bindless buffer. SHaRC's hash-grid insert needs them; without them the RTXDI
        // PT core runs without the radiance cache.
        bool int64HeapAtomics          = false;

        // No hardwareVideoDecode flag: its one would-be caller (Mp4FrameSource) hands FFmpeg a native
        // device pointer, so a bool cannot remove its downcast, and FFmpeg falls back to software decode
        // by itself when the device has no video support.
    };

    // The adapter a device runs on: the first boot's quality pick reads its video memory, Lua
    // Settings.caps() reports it. Zeros / empty from a backend that does not know.
    struct AdapterInfo
    {
        uint32_t    vendorId               = 0;   // PCI: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
        uint32_t    deviceId               = 0;
        uint64_t    dedicatedVideoMemoryMB = 0;
        uint64_t    sharedSystemMemoryMB   = 0;
        std::string description;                  // UTF-8
    };

    // Where the boot precompile stands (IGraphicsDevice::StartPipelinePrecompile).
    struct PipelinePrecompileStatus
    {
        bool        started  = false;   // workers were started (false: see skipped)
        bool        done     = false;   // every record finished, or a cancel skipped the rest
        uint32_t    total    = 0;       // records in the manifest
        uint32_t    finished = 0;       // built + present + failed
        uint32_t    built    = 0;       // compiled by the driver and stored in the library
        uint32_t    present  = 0;       // the library had it already
        uint32_t    failed   = 0;       // a missing blob, an unknown root signature, the driver refused
        double      elapsedMs = 0.0;
        std::string skipped;            // why nothing was started; "" when it was
    };

    // PresentSettings and DisplayHDRInfo (presentation) are in Graphics/ISurface.h.
}

class IDeviceFrameControl;   // Graphics/IDeviceFrameControl.h

class IGraphicsDevice
{
public:
    // Room in this frame's constants (AllocTransientConstants): write cpu, bind gpu.
    struct TransientConstants { void* cpu = nullptr; uint64_t gpu = 0; };

    // One texture plane read back by ReadTextureRaw / ReadHdrSceneRaw, rows packed tight.
    struct RawTexture
    {
        std::vector<std::uint8_t> bytes;        // height rows of width * bytesPerTexel
        std::uint32_t width = 0, height = 0, bytesPerTexel = 0;
        std::uint32_t resourceFormat = 0;       // the resource's DXGI_FORMAT
        std::uint32_t planeFormat    = 0;       // the copied plane's DXGI_FORMAT (the footprint's)
        std::uint32_t plane          = 0;
        std::uint32_t arraySlice     = 0;       // the array slice read (0 for a plain 2D texture)
        std::string   error;
    };

    virtual ~IGraphicsDevice() = default;
    // Owner-side mutation barrier for CPU readers of published frame resources.
    // `cause` is a Diag::DrainCause (Diagnostics/OwnerAccounting.h), carried
    // as its underlying type so this interface stays free of that header.
    virtual void DrainCpuFrame(uint8_t /*cause*/ = 0) {}
    // The frame executor's contract (FramePipeline only; Graphics/IDeviceFrameControl.h); null for a
    // device without frame coordination.
    virtual IDeviceFrameControl* FrameControl() { return nullptr; }

    // ---- Device capabilities / configuration ----
    // Feature query; see RHI::DeviceCapabilities.
    virtual RHI::DeviceCapabilities GetCapabilities() const { return {}; }
    // The adapter; see RHI::AdapterInfo.
    virtual RHI::AdapterInfo GetAdapterInfo() const { return {}; }
    // Every hardware adapter, the high-performance one first (the settings menu's graphics card list).
    virtual std::vector<RHI::AdapterInfo> ListAdapters() const { return {}; }

    // Frames the backend keeps in flight (default 3). Every per-frame ring (constant buffers, pose /
    // skinning uploads, descriptor staging) sizes itself from this, NOT from a local constant: a ring
    // smaller than the real in-flight depth races the GPU reading the same slot for an earlier frame -
    // silent corruption, not a crash. A compile-time array bound keeps its constant but static_asserts
    // it against the backend's kFrameCount.
    virtual uint32_t GetFrameCount() const { return 3; }

    // Required alignment (bytes) of a constant-buffer view offset, for ring allocators that pack many
    // per-draw CBs into one buffer.
    virtual uint32_t GetConstantBufferAlignment() const { return 256; }

    // ---- Presentation: the main surface (Graphics/ISurface.h) ----
    // The swap chain on the window and everything about presenting to it: present / pacing settings,
    // HDR output and the display's capability, the frame limiter, resize and fullscreen, recording onto
    // the back buffer, back-buffer capture. One per device; App creates and releases it around the
    // device's life.
    virtual ISurface& MainSurface() = 0;

    // ---- Frame lifecycle ----
    // CPU-waits until the GPU (graphics, compute and copy queues) has finished with the upload slot
    // the render side is about to write: the slot BeginFrame published from the monotonic frame counter,
    // or the back-buffer index under -slotkey=dxgi. BeginFrame calls it, as does AcquireUploadSlot under
    // -slotkey=dxgi; any other caller is the main loop before BeginFrame, never a pass or an ImGui
    // handler. A per-frame upload write (mesh-descriptor, pose, skinned-vertex, instance / material
    // rings) made before this wait races the GPU still reading that slot (torn descriptor reads,
    // skinned-mesh flicker). Refused between BeginFrame and EndFrame. Idempotent within a frame: the
    // wait runs once until BeginFrame consumes it.
    virtual void WaitForNextFrameSlot() {}

    // Publishes the render frame's upload slot (GetFrameIndex), waits for that slot's previous tenant to
    // retire (WaitForNextFrameSlot) and opens the primary command list (id 0), on which it pushes the
    // swap-chain PRESENT -> RENDER_TARGET barrier. Returns that list.
    virtual RHI::CommandList BeginFrame() = 0;

    // Closes and executes all open command lists, presents and signals the fence. Does NOT pace the
    // frame: App::Run calls ISurface::LimitFrameRate() after its frame-timing capture.
    virtual void EndFrame() = 0;

    // True between BeginFrame() and EndFrame(), while command lists are open against the back buffer.
    // Code that runs from both an ImGui handler and the main loop (Window, EditorLayer, EngineSettings)
    // asks this to choose "do it now" vs "queue it for the frame boundary" without downcasting to
    // GraphicsDX12. Default false: a headless / null device is always safe to act on. Nothing that
    // destroys a back buffer or hard-syncs the queues may run while this is true (the FRAME-BOUNDARY
    // CONTRACT at the top of GraphicsDX12.h).
    virtual bool IsRecordingFrame() const noexcept { return false; }

    // The frame-rate limiter (LimitFrameRate, GetLastFramePacingWaitMs) paces against the display: it
    // is on ISurface.

    // Signals the graphics queue fence and CPU-waits until all submitted GPU work completes. Stalls the
    // CPU: editor operations only.
    virtual void FlushAndWait() = 0;

    // Shader hot reload, after Renderer::ReloadShaders with the GPU flushed: rebuilds the pipelines
    // the DEVICE holds (the present composite). Default: none.
    virtual void ReloadDeviceShaders() {}

    // Hard sync: waits on EVERY queue (graphics, compute, copy) and drains every pending deferred
    // release, so pending Destroy* calls free their GPU memory. A full pipeline bubble, heavier than
    // FlushAndWait: only for rare editor transitions where freeing the old resources before allocating
    // new ones matters (world reload).
    virtual void WaitIdleAndReleaseDeferred() = 0;

    // ---- Command list management ----
    // Opens another command list from the pool (thread-safe); pass the returned handle to every
    // recording call. Reuses pooled allocators and creates the DX12 objects lazily on first use.
    virtual RHI::CommandList BeginCommandList(
        RHI::QueueType queue = RHI::QueueType::GRAPHICS) = 0;

    // Declares a GPU execution dependency: @p waiter runs after @p dependency. Same queue: topological
    // submission order (no cost); cross queue: EndFrame inserts a fence Wait/Signal at the boundary.
    // Call after both lists are open and before EndFrame().
    virtual void AddCommandListDependency(RHI::CommandList waiter,
                                          RHI::CommandList dependency) = 0;

    // ---- Scene targets (the swap chain itself is ISurface's) ----
    // Sets the scene viewport size (the ImGui panel size); 0,0 = the window size.
    virtual void SetViewportSize(uint32_t width, uint32_t height) = 0;

    // Binds the HDR offscreen render target and clears it to @p clearColor (the render graph passes
    // its HDR import's clear value); null = the slot's stored colour (SetHdrClearColor). Records into
    // @p cmd, after BeginFrame / BeginCommandList.
    virtual void SetRenderTargetToHdr(const float clearColor[4],
                                      RHI::CommandList cmd) = 0;

    // Stores the clear colour SetRenderTargetToHdr(nullptr) uses for the current slot (default
    // {0,0,0,0}). Only the -rgimportoff path, which has no graph import, sets it; otherwise the clear
    // value travels with the graph's HDR import (BindImport).
    virtual void SetHdrClearColor(const float clearColor[4]) {}

    // Binds the HDR offscreen render target WITHOUT clearing it, optionally with depth buffer @p dsv
    // (DEPTH_STENCIL bind flag): for passes such as SkyboxPass that draw over existing HDR content.
    // @p view picks the DSV - the writable one or a read-only variant - and states what the pass
    // declared it writes (RHI::DepthView); the default binds the writable DSV and checks nothing.
    virtual void SetRenderTargetToHdrWithDepth(const RHI::Texture*   dsv,
                                               RHI::CommandList      cmd,
                                               const RHI::DepthView& view = {}) = 0;

    // The HDR scene target's format, which the two calls above bind: what a PSO that draws into it
    // declares (RG::DrawFullscreen).
    virtual RHI::Format GetHdrSceneColorFormat() const { return RHI::Format::UNKNOWN; }

    // ---- Queries ----
    virtual uint32_t GetWidth()  const = 0;
    virtual uint32_t GetHeight() const = 0;

    // The render viewport size (the HDR offscreen target's): smaller than the swap chain while the
    // editor viewport panel is active; GetWidth / GetHeight until SetViewportSize is called.
    virtual uint32_t GetRenderWidth()  const { return GetWidth();  }
    virtual uint32_t GetRenderHeight() const { return GetHeight(); }

    // The upload-ring slot of the frame being RENDERED (render frame % kFrameCount; the back-buffer
    // index only under -slotkey=dxgi), stable for the whole frame after BeginFrame returns. FrameCB and
    // every other per-frame upload wrapper on the render side picks its slot with it, so CPU writes do
    // not race the GPU reading the same buffer for an earlier in-flight frame. The depth is
    // RHI::kFramesInFlight.
    virtual uint32_t GetFrameIndex()   const { return 0; }

    // The upload-ring slot the GAME side claimed for the frame it is simulating; GetFrameIndex() names
    // the slot the RENDER side is reading. Equal while the loop is serial, one slot apart once the
    // render half runs a frame behind, so the choice matters: a ring written through the wrong one
    // aliases another frame's data, which the D3D12 debug layer cannot see and which shows only as
    // intermittent skinned-mesh flicker.
    // Rule of thumb: called before BeginFrame -> GetUploadSlot(); from a RenderPass or anywhere inside
    // Renderer::Render -> GetFrameIndex().
    virtual uint32_t GetUploadSlot()   const { return GetFrameIndex(); }

    // True when the upload slot is derived from the monotonic frame counter (GetUploadSlot() ==
    // frame % kFramesInFlight by construction). False under the -slotkey=dxgi kill switch, where the
    // slot is the swap-chain back-buffer index: still a valid 3-deep rotation, but with an arbitrary
    // phase against the frame counter that a ResizeBuffers re-bases. Callers that assert the
    // frame-derived identity must ask this first, or the kill switch is useless for bisection.
    virtual bool IsSlotKeyedByFrame() const { return true; }

    // True while the GAME side may write the upload slot GetUploadSlot() names: from AcquireUploadSlot,
    // which proved the slot's previous tenant retired, to EndFrame, which submits it. Outside that
    // window the slot is still being read by a frame in flight or not yet claimed, and a CPU write into
    // it corrupts a correct frame.
    // Frame numbers or slot indices cannot answer this: in frame F+1's gameplay phases, before the
    // acquire, GetUploadSlot() and every ring's current slot are BOTH still frame F's - stale together,
    // so they agree and a comparison says "safe" exactly when it is not.
    virtual bool IsUploadSlotWritable() const { return true; }

    // The frame a resource or descriptor slot handed back RIGHT NOW must outlive, and the newest frame
    // whose GPU work has provably finished: anything stamped with GetReleaseStampFrame() may be freed
    // once GetRetiredFrame() reaches that stamp.
    // Exposed because subsystems that hand back their own CPU-side slot tables (a .meshlib's
    // mesh-descriptor and bindless VB/IB slots) need the same answer, and a private slot ring cannot
    // give it: "the slot came round again" means "the GPU is done" only while ONE thread owns both
    // halves of the frame. The stamp is one frame beyond the render half's (conservative under any
    // interleaving); the retired value is what BeginFrame's fence wait proved.
    // The defaults fail safe: a never-reachable stamp makes a device that cannot track frames HOLD what
    // it is given (a few slots leaked until the next world clear) instead of letting a
    // "stamp <= retired" predicate free a slot a command list still reads.
    virtual uint64_t GetReleaseStampFrame() const { return UINT64_MAX; }
    virtual uint64_t GetRetiredFrame()      const { return 0; }

    // GetSwapChainGeneration (the swap-chain rebuild counter) is on ISurface.

    // Game-thread upload-slot acquisition for frame @p frame: waits until the slot that frame will
    // write (frame % kFramesInFlight; the back-buffer index under -slotkey=dxgi) has been retired by the
    // GPU, then publishes it as the game-side slot and opens its write window (IsUploadSlotWritable).
    // Keyed on the GAME frame, whereas WaitForNextFrameSlot() retires the RENDER side's slot, so it
    // stays correct once the game thread runs ahead of the render thread's Present.
    virtual void AcquireUploadSlot(uint64_t /*frame*/) {}

    // GPU handle of the HDR SRV as uint64_t (ImTextureID / D3D12_GPU_DESCRIPTOR_HANDLE::ptr).
    virtual uint64_t GetHdrSceneSrvGpuHandle() const = 0;

    // GPU handle of the HDR UAV; 0 when the backend allocated none (HDR not UAV-bindable). The SSR
    // composite writes reflections into the HDR scene through it after lighting + skybox.
    virtual uint64_t GetHdrSceneUavGpuHandle() const { return 0; }

    // Copies the full HDR scene target into @p dst (identical format and dimensions). The caller owns
    // barriers: HDR in COPY_SOURCE, @p dst in COPY_DEST. The SSR composite snapshots HDR with it so its
    // compute pass can read the copy while writing the same UAV.
    virtual void CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd) {}

    // Self-contained HDR -> @p dst copy: RENDER_TARGET -> COPY_SOURCE, the copy and COPY_SOURCE ->
    // RENDER_TARGET as ONE run inside @p cmd, WITHOUT touching the device's shared HDR state tracker.
    // Render-graph passes must use this form. Their Execute() bodies run in parallel on workers
    // (RenderGraph Phase B), so SetHdrTextureState(COPY_SRC) -> CopyHdrSceneTo -> SetHdrTextureState(RT)
    // is a data race: the other passes recording concurrently read the same tracker, see COPY_SOURCE
    // mid-window and each emit a spurious COPY_SOURCE -> RENDER_TARGET barrier into their own list,
    // which the GPU executes while the resource is still RENDER_TARGET.
    // Precondition: HDR is RENDER_TARGET on entry to @p cmd, which the graph guarantees for any pass
    // declaring an HdrSceneColor* color target (Phase A binds and transitions it serially on that
    // pass's own list).
    virtual void CopyHdrSceneToScoped(const RHI::Texture& dst, RHI::CommandList cmd) {}
    // The HDR scene target's size (false: none yet): what a CopyHdrSceneToScoped destination must
    // match, with GetHdrSceneColorFormat.
    virtual bool GetHdrSceneSize(uint32_t& width, uint32_t& height) const { return false; }

    // ---- HDR scene target slots ----
    // Slot 0 is the main viewport; the preview layer claims the others for its panels and the asset
    // browser's thumbnails. GraphicsDX12::PushHdrTarget carries the scope rules in full.
    // Builds (or rebuilds) slot @p slot's target; the old one is released deferred.
    virtual void CreateHdrTarget(uint32_t slot, uint32_t width, uint32_t height) {}
    virtual void DestroyHdrTarget(uint32_t slot) {}
    // Makes @p slot the current HDR binding until the matching PopHdrTarget: every HDR bind and
    // accessor then resolves to it. Never inside a RenderGraph Phase B window. FALSE WHEN THE PUSH DID
    // NOT TAKE: the binding is still slot 0's, and the caller must not render as if it were @p slot's.
    [[nodiscard]] virtual bool PushHdrTarget(uint32_t slot) { return false; }
    virtual void PopHdrTarget() {}
    // 0 outside every push scope. The consumers that must run at depth 0 check it with
    // ENGINE_CHECK_ONCE.
    virtual int HdrScopeDepth() const { return 0; }
    // The SRV of a NAMED slot rather than the current one (0 when the slot is empty): what a consumer
    // reads after it popped.
    virtual uint64_t GetHdrTargetSrvGpuHandle(uint32_t slot) const { return 0; }
    // Slot @p slot's HDR scene target as a texture handle (invalid when the slot is empty). One state
    // book: the handle's state is what the HDR helpers transition and what the graph's barriers and
    // DeviceStateAgrees see. The slot owns it - never DestroyTexture it.
    virtual RHI::Texture GetHdrTexture(uint32_t slot) const { return {}; }

    // GPU handle of a texture's default SRV as uint64_t (0 when the texture is invalid or has no SRV);
    // usable as the ImTextureID of an ImGui::Image() thumbnail.
    virtual uint64_t GetTextureSRVGpuHandle(const RHI::Texture& texture) const { return 0; }

    // GPU handle of a texture's UNORM alias SRV (raw sRGB bytes, no linearisation), for editor
    // previews that render to a UNORM RTV. The default SRV when no alias exists.
    virtual uint64_t GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const
    { return GetTextureSRVGpuHandle(texture); }

    // Bytes the device set aside for the texture's resource (ID3D12Device::GetResourceAllocationInfo
    // of its desc): its VRAM cost, alignment included; 0 for an invalid texture. Diagnostics only (the
    // Texture Viewer, Loaded Textures, Lua Asset.get(..).texture.gpuBytes).
    virtual uint64_t GetTextureAllocationBytes(const RHI::Texture& texture) const { return 0; }

    // GPU handle of the NV12 UV-plane SRV (R8G8_UNORM, PlaneSlice=1); GetTextureSRVGpuHandle returns
    // the Y plane (R8_UNORM, PlaneSlice=0) of the same texture. 0 when the texture is not NV12 / P010
    // or was not created with the SHADER_RESOURCE bind flag.
    virtual uint64_t GetTextureUVPlaneSRVGpuHandle(const RHI::Texture& texture) const { return 0; }

    // GPU handle of a depth/stencil texture's stencil-plane SRV (X32_TYPELESS_G8X24_UINT view of
    // D32_FLOAT_S8X24, or X24_TYPELESS_G8 view of D24_UNORM_S8); 0 when the format has no stencil or
    // there is no SRV. Sample as Texture2D<uint2> and read .y for the stencil byte.
    virtual uint64_t GetTextureStencilSRVGpuHandle(const RHI::Texture& texture) const { return 0; }

    // ---- Descriptor table management ----
    // Allocates @p count consecutive shader-visible SRV descriptors, one per depth texture in
    // @p textures, each viewed as DXGI_FORMAT_R32_FLOAT (D32 read as float), and returns the GPU
    // handle of the first (for BindDescriptorTableGpuHandle); 0 on failure. Valid until
    // FreeDescriptorTable.
    virtual uint64_t CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                                uint32_t            count) = 0;

    // Releases a descriptor table allocated by CreateDepthTextureSRVTable.
    virtual void FreeDescriptorTable(uint64_t gpuHandle) = 0;

    // ---- Resource creation ----
    virtual bool CreateBuffer(
        const RHI::GPUBufferDesc& desc,
        RHI::GPUBuffer&           outBuffer,
        const void*               initialData = nullptr) = 0;

    // Begin/end a buffer-upload batch scope: inside it, CreateBuffer calls that would
    // FlushUploadAndWait() per buffer defer the fence wait to EndBufferUploadBatch(). The per-buffer
    // GPU-fence round trip is the biggest cost of loading thousands of meshes.
    // Nestable: GraphicsDX12 flattens nested scopes into a depth counter and only the outermost End
    // flushes (plus a mid-batch flush whenever 64 MB or 512 staging buffers pile up). CreateTexture
    // and UpdateTexture honour the scope too. A scope belongs to the caller's upload context, picked by
    // execution lane (GraphicsDX12::Upload), so end it on the lane that began it.
    virtual void BeginBufferUploadBatch() {}
    virtual void EndBufferUploadBatch()   {}

    // Begin/end a buffer-heap suballocation scope: inside it, a DEFAULT-heap CreateBuffer() carves a
    // placed resource out of a shared heap instead of one committed resource per buffer, so a
    // character's many small per-part skinned-mesh buffers (~210) take a couple of heap allocations.
    // Nestable counter; UPLOAD / READBACK stay committed; the default is a no-op (committed).
    // @p heapBytesHint: for the OUTERMOST scope, its heaps are that big instead of the 16 MB default (a
    // buffer larger than the hint still gets a heap of its own size) - for a small set of buffers built
    // alone, such as a preload's skinned part, which would strand most of a 16 MB heap. Ignored by a
    // nested scope.
    virtual void BeginBufferHeapBatch(uint64_t heapBytesHint = 0) { (void)heapBytesHint; }
    virtual void EndBufferHeapBatch()   {}

    // Begin/end a LOADER upload scope on THIS thread - a TaskSystem worker preparing resources nothing
    // uses yet. Inside it, CreateBuffer / CreateTexture with initial data record into a loader context
    // of their own (own lists, fences, batch and heap state - never a frame lane's) as ONE upload
    // batch; End executes the copies, waits for them on the worker and frees their staging. What the
    // scope created is complete when End returns, and its owner publishes it to the frame's threads
    // afterwards, so the main thread never waits for these copies. Begin answers false when the loader
    // path is off (a device without it, or a run without it): the caller keeps its own path and does
    // not call End. Nestable on one thread (the outermost End submits); a Begin waits while every
    // loader context is taken by another thread's scope.
    virtual bool BeginLoaderUploads() { return false; }
    virtual void EndLoaderUploads()   {}
    // Whether BeginLoaderUploads would take a scope (the loader path is on).
    virtual bool LoaderUploadsEnabled() const { return false; }

    virtual bool CreateTexture(
        const RHI::TextureDesc&      desc,
        RHI::Texture&                outTexture,
        const RHI::SubresourceData*  initialData = nullptr) = 0;

    // Uploads @p planes into an EXISTING DEFAULT-heap texture through an UPLOAD staging buffer +
    // CopyTextureRegion. @p subresourceCount is `texture.desc.mipLevels * desc.arraySize` for regular
    // formats, or 2 for NV12 (plane 0 = Y, plane 1 = UV).
    // Outside an upload batch it is synchronous - flushes the upload list on return - so the copy is
    // visible to the next BeginFrame (per-frame video / streaming updates). Inside a
    // BeginBufferUploadBatch or BeginLoaderUploads scope it only records the copy, which lands when
    // the batch flushes (its outermost End or a mid-batch flush); a caller that needs the copy at once
    // opens no batch. The caller owns no barriers: the implementation transitions in -> COPY_DEST ->
    // back. False on a size mismatch or a staging failure.
    virtual bool UpdateTexture(
        RHI::Texture&                texture,
        const RHI::SubresourceData*  planes,
        uint32_t                     subresourceCount) { return false; }

    // ---- Streaming texture upload ----
    // Queues a copy of CPU data into @p dst WITHOUT waiting for the GPU. The data is copied into this
    // upload slot's persistently mapped ring during the call; the render half records every queued
    // copy into one "StreamUpload" command list at the start of RenderAsync - a list with no
    // dependencies, so it executes before every reader of the frame - and transitions each texture
    // in -> COPY_DEST -> back.
    // Call from the game half. The copy lands in the OPEN frame (the one the last AcquireUploadSlot
    // opened) while the render half has not taken it: F from AcquireUploadSlot(F) on, still F-1 in F's
    // earlier phases if the render half has not reached it (frame overlap). Once the render half took
    // the open frame, the copy carries over into the next one (Result says which). Copies run in
    // request order. Rejected on the render half's thread and inside a device-execution scope.
    // @p d.policy says what merges and what survives overflow (Graphics/StreamUploadTypes.h).
    // Rules: create the texture once, initial state SHADER_RESOURCE; never read it on the async compute
    // queue; never mix this with UpdateTexture / CopyD3D12ResourceToTexture on the same texture (the
    // first QueueTextureUpload marks it a stream target and the synchronous paths then fail an
    // ENGINE_CHECK: their direct submission would run before the frame's queued copies).
    // The default (a device without an uploader) does a synchronous UpdateTexture for a
    // whole-subresource request (Fallback) and refuses a rect (Rejected).
    virtual StreamUpload::Result QueueTextureUpload(const RHI::Texture& dst, const StreamUpload::Desc& d)
    {
        if (d.rect.width != 0 || !d.planes || d.planeCount == 0 || d.mip != 0 || d.arraySlice != 0)
            return StreamUpload::Result::Rejected;
        RHI::Texture t = dst;
        return UpdateTexture(t, d.planes, d.planeCount) ? StreamUpload::Result::Fallback
                                                        : StreamUpload::Result::Rejected;
    }
    // Running totals since boot; `active` is false without an uploader.
    virtual StreamUpload::Stats GetStreamUploadStats() const { return {}; }
    // The render half, once per frame at the start of RenderAsync (before any other command list it
    // begins): records the frame's queued copies.
    virtual void RecordPendingStreamUploads() {}

    // Gives an existing SHADER-RESOURCE-ONLY texture new contents - a new resource built from @p desc /
    // @p initialData, another size or format allowed - KEEPING its handleId: the bindless slot
    // [handleId] and every SRV descriptor a consumer cached (GetTextureSRVGpuHandle, the preview alias)
    // are rewritten IN PLACE, so nothing that holds the texture re-resolves it (a reimport). The old
    // resource goes to the frame-stamped release queue.
    // The caller must have IDLED the GPU (WaitIdleAndReleaseDeferred, at a frame boundary): in-flight
    // frames read the descriptors being rewritten. Refuses (false, nothing changed) a texture with RTV /
    // UAV / DSV views, a placed or planar one, or a @p desc that is not SRV-only.
    virtual bool ReplaceTexture(
        RHI::Texture&                texture,
        const RHI::TextureDesc&      desc,
        const RHI::SubresourceData*  initialData) { return false; }

    // Textures placed in the render graph's transient alias arena (Graphics/TransientAliasArena.h,
    // opaque here). GetTextureAllocationInfo: the size and alignment a placed texture of @p desc needs
    // (false: a shape the placed path does not take). EnsureAliasArena: makes the arena hold at least
    // @p bytes, growing it (and bumping its epoch) if needed - false when it is disabled or the heap
    // could not be made, and the caller keeps committed textures. Main thread / serial phases only.
    // CreateTexturePlaced: a texture at @p offset in the arena's current heap.
    virtual bool GetTextureAllocationInfo(const RHI::TextureDesc& desc, uint64_t& outSize, uint64_t& outAlign) { return false; }
    // The size and alignment of a placed texture of any 2D / 3D / array shape, render targets and depth
    // included (the render graph's aliasing census and heaps); false when the device cannot say.
    virtual bool GetTextureFootprint(const RHI::TextureDesc& desc, uint64_t& outSize, uint64_t& outAlign) { return false; }
    virtual bool EnsureAliasArena(TransientAliasArena& arena, uint64_t bytes) { return false; }
    virtual bool CreateTexturePlaced(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                                     uint64_t offset, RHI::Texture& outTexture) { return false; }
    // Any shape CreateTexture takes - render targets and depth included, no initial data - at @p offset in
    // the arena's current heap, an arena of the matching kind (TransientAliasArena::Kind::RenderTargets
    // for render targets and depth): the render graph's secondary view sets. Serial phases only.
    virtual bool CreateTextureInArena(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                                      uint64_t offset, RHI::Texture& outTexture) { return false; }
    // Marks @p texture's contents undefined (DiscardResource): the first operation on a render target or
    // depth texture an aliasing barrier made active. It must be in its render-target / depth-write state.
    virtual void DiscardTexture(const RHI::Texture& texture, RHI::CommandList cmd) {}

    virtual bool CreateShader(
        RHI::ShaderStage stage,
        const void*      bytecode,
        size_t           bytecodeSize,
        RHI::Shader&     outShader) = 0;

    virtual bool CreateSampler(
        const RHI::SamplerDesc& desc,
        int&                    outDescriptorIndex) = 0;

    virtual bool CreatePipelineState(
        const RHI::PipelineStateDesc& desc,
        RHI::PipelineState&           outPSO) = 0;

    // Several COMPUTE pipelines at once, outs[i] from descs[i]. Creating a pipeline from a large kernel
    // is the driver compiling it (~270 ms for a RayQuery kernel, every run), so the backend creates
    // them concurrently: the wall time is the slowest one, not the sum. A failed entry leaves its out
    // as it was, as CreatePipelineState does; true when every one succeeded. Most callers want
    // Graphics/ComputePipelineBatch.h (DXIL in, all or nothing). The default creates them one by one.
    virtual bool CreatePipelineStates(const RHI::PipelineStateDesc* descs, uint32_t count,
                                      RHI::PipelineState* outs)
    {
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i) ok = CreatePipelineState(descs[i], outs[i]) && ok;
        return ok;
    }

    // A pipeline layout: a root signature described in RHI terms (Graphics/PipelineLayoutBuilder.h
    // assembles one). A PSO built with PipelineStateDesc::layout binds with it and BindPipelineState
    // sets it, so a pass never sets a root signature itself. Translated field for field, so the blob
    // equals what the same hand-filled D3D12 description serializes to. Logs one [PIPELINE_LAYOUT] line
    // per layout: parameters, DWORD cost, samplers, serialized bytes and the blob's FNV-1a. On success
    // the handle passed in is retired (like CreatePipelineState), so a rebuild needs no destroy; false
    // (the log says why) for an incomplete list, a table without ranges, more than 64 DWORDs or a
    // refused serialization.
    virtual bool CreatePipelineLayout(
        const RHI::PipelineLayoutDesc& desc,
        RHI::PipelineLayout&           outLayout) = 0;

    // ---- Command recording - state (all require a valid CommandList) ----
    virtual void BindPipelineState(const RHI::PipelineState& pso,
                                   RHI::CommandList cmd) = 0;

    virtual void SetPrimitiveTopology(RHI::PrimitiveTopology topology,
                                      RHI::CommandList cmd) = 0;

    virtual void SetViewport(const RHI::Viewport& viewport,
                             RHI::CommandList cmd) = 0;

    virtual void SetScissorRect(uint32_t left, uint32_t top,
                                uint32_t right, uint32_t bottom,
                                RHI::CommandList cmd) = 0;

    // ---- Command recording - resource binding (slot -> HLSL register b/t/s index) ----
    // Every slot here is typed (RHI::Slots, Graphics/RootSignatureLayout.h) and comes first. GfxCB /
    // GfxSRV are group-relative (the backend adds the base and range-checks), GfxParam is the absolute
    // root parameter; passing one for another, or a bare number, does not compile.
    virtual void BindConstantBuffer(RHI::Slots::GfxCB slot,
                                    const RHI::GPUBuffer& buffer,
                                    RHI::CommandList cmd) = 0;

    // Offset variant of BindConstantBuffer, for rings that pack many per-draw constant blocks into one
    // allocation (probe capture's six face CBs, water / video per-instance CBs). @p byteOffset is a
    // multiple of GetConstantBufferAlignment(); @p slot is GROUP-RELATIVE, as in BindConstantBuffer
    // (RHI::Slots::GfxRel in RootSignatureLayout.h).
    virtual void BindConstantBufferAtOffset(RHI::Slots::GfxCB slot,
                                            const RHI::GPUBuffer& buffer,
                                            uint64_t byteOffset,
                                            RHI::CommandList cmd) {}

    // Binds a raw GPU address to the per-material custom constant-buffer slot: an address, not a
    // buffer handle, because the upload ring that owns these tracks addresses. Zero is a no-op.
    virtual void BindCustomMaterialCBV(uint64_t gpuAddress, RHI::CommandList cmd) {}

    // ---- This frame's constants ----
    // Room for @p bytes of constants in this frame's upload memory (Graphics/TransientConstants.h):
    // write cpu, bind gpu with BindConstantBufferAddress / SetComputeRootCBVAddress. Any recording
    // thread; live until this frame's GPU work retires. Empty when the frame's constants ran out
    // (reported there): skip the draw. The typed form is AllocConstants<T>(gfx) /
    // cl.AllocConstants<T>() below.
    virtual TransientConstants AllocTransientConstants(uint32_t bytes) { (void)bytes; return {}; }
    // A graphics root CBV from a GPU address; @p slot is group-relative, as in BindConstantBuffer.
    // Zero is a no-op.
    virtual void BindConstantBufferAddress(RHI::Slots::GfxCB slot, uint64_t gpuAddress, RHI::CommandList cmd) {}

    // ---- Command recording - resource binding (slot -> HLSL register b/t/s index) ----
    // Binds the base of a contiguous descriptor range holding a material's reflected textures. Zero is
    // a no-op.
    virtual void BindCustomMaterialTextureTable(uint64_t gpuHandle,
                                                RHI::CommandList cmd) {}

    virtual void BindResource(RHI::Slots::GfxSRV slot,
                              const RHI::GPUResource& resource,
                              RHI::CommandList cmd) = 0;

    // The ONE sampler a graphics pass picks at run time: the material sampler, Terrain's, Water's or a
    // UI draw's - @p samplerIndex as CreateSampler returned it, or a fixed SMP_* slot
    // (Graphics/SamplerSlots.h). Writes its heap index into the shared signature's DynamicSampler root
    // constant, which sampler_heap.hlsli's SmpDynamic() reads; per draw when the choice is per draw.
    // Every other sampler is a fixed slot the shader names, and the shared signatures have no sampler
    // tables, so there is nothing else to bind.
    virtual void SetDynamicSampler(int samplerIndex, RHI::CommandList cmd) {}

    // ---- Command recording - PVF (Programmable Vertex Fetching) bindings ----
    // Pushes 3 root constants (meshDescIdx, instanceOffset, materialIndex) at root param 0.
    virtual void SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                                  RHI::CommandList cmd) = 0;

    // 4-value form. The PVF root-constant block is 4 DWORDs wide; word 3 carries a per-draw payload
    // (prevPosElementBase for skinned velocity, outlinePixelsBits for the outline pass). The portable
    // way to write all four; the default forwards the first three and writes word 3 separately, so a
    // backend need only implement the primitives.
    virtual void SetRootConstants4(uint32_t v0, uint32_t v1, uint32_t v2,
                                   uint32_t v3, RHI::CommandList cmd)
    {
        SetRootConstants(v0, v1, v2, cmd);
        SetGraphicsRootConstant(RHI::Slots::Gfx::RootConstants, v3, 3, cmd);
    }

    // Binds a GPU buffer as a root SRV at absolute root parameter @p rootSlot (not an API slot). Works
    // for DEFAULT and UPLOAD heap StructuredBuffers / ByteAddressBuffers.
    virtual void SetRootBufferSRV(RHI::Slots::GfxParam rootSlot,
                                  const RHI::GPUBuffer& buf,
                                  RHI::CommandList cmd) = 0;

    // Binds a descriptor table by raw GPU handle (e.g. the bindless g_Buffers[] table) at absolute root
    // parameter @p rootSlot.
    virtual void BindDescriptorTableGpuHandle(RHI::Slots::GfxParam rootSlot,
                                              uint64_t gpuHandle,
                                              RHI::CommandList cmd) = 0;

    // ---- Command recording - draws ----
    virtual void DrawInstanced(uint32_t vertexCount,
                               uint32_t instanceCount,
                               uint32_t startVertex,
                               uint32_t startInstance,
                               RHI::CommandList cmd) = 0;

    // GPU-driven draw: executes the IndirectDrawCommand entries in @p argBuffer. @p countBuffer
    // (optional; null = maxCommands) holds a uint32 GPU-side draw count.
    virtual void ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                                     uint64_t argBufferOffset,
                                     uint32_t maxCommands,
                                     const RHI::GPUBuffer* countBuffer,
                                     uint64_t countBufferOffset,
                                     RHI::CommandList cmd) = 0;

    // The same 32-byte commands executed with a DISPATCH_MESH signature: 4 root constants, then words
    // 4..6 = ThreadGroupCountX / Y / Z (the cull CS writes blocks, survivors, 1 for a meshlet group;
    // word 7 is never read). No-op on a device without mesh shaders - the meshlet path forms no group
    // there.
    virtual void ExecuteIndirectDispatchMesh(const RHI::GPUBuffer& argBuffer,
                                             uint64_t argBufferOffset,
                                             uint32_t maxCommands,
                                             const RHI::GPUBuffer* countBuffer,
                                             uint64_t countBufferOffset,
                                             RHI::CommandList cmd) {}

    // Copies @p size bytes from @p src to @p dst.
    virtual void CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                            uint64_t size, RHI::CommandList cmd) = 0;

    // Clears a buffer's or a texture's UAV - the view it was created with (a texture's covers mip 0) -
    // to @p values (ClearUnorderedAccessViewUint / Float; a raw buffer takes values[0]). D3D12 clears
    // only raw or typed views, so a STRUCTURED buffer needs ResourceMiscFlag::UAV_CLEARABLE, which adds
    // a raw view for this (without it the clear is refused). The resource must be in UNORDERED_ACCESS;
    // a buffer in COMMON is promoted. A later UAV access needs a UAV barrier (GPUBarrier::Memory), as
    // after any UAV write.
    virtual void ClearUavUint(const RHI::GPUBuffer& buffer, const uint32_t values[4], RHI::CommandList cmd) {}
    virtual void ClearUavUint(const RHI::Texture& texture, const uint32_t values[4], RHI::CommandList cmd) {}
    virtual void ClearUavFloat(const RHI::Texture& texture, const float values[4], RHI::CommandList cmd) {}

    // Offset variant (retained-instance dirty ranges): copies @p size bytes from @p src + @p srcOffset
    // into @p dst + @p dstOffset. The caller owns barriers: dst in COPY_DEST (or COMMON, promoted).
    virtual void CopyBufferRegion(const RHI::GPUBuffer& src, uint64_t srcOffset,
                                  const RHI::GPUBuffer& dst, uint64_t dstOffset,
                                  uint64_t size, RHI::CommandList cmd) = 0;

    // Copies one (mip, arraySlice) subresource from @p src to @p dst; both share the format and
    // per-mip dimensions at that level. The caller owns barriers: src in COPY_SOURCE, dst in COPY_DEST.
    // The reflection probe broadcast fans a TextureCube into one cube of a TextureCubeArray with it.
    virtual void CopyTextureSubresource(const RHI::Texture& src,
                                        uint32_t srcMip, uint32_t srcArraySlice,
                                        const RHI::Texture& dst,
                                        uint32_t dstMip, uint32_t dstArraySlice,
                                        RHI::CommandList cmd) {}

    // Copies all of @p src into @p dst (D3D12 CopyResource): same format, size, mip and array count.
    // The caller owns barriers: src in COPY_SRC, dst in COPY_DST.
    virtual void CopyTexture(const RHI::Texture& src, const RHI::Texture& dst, RHI::CommandList cmd) {}

    virtual void DrawIndexedInstanced(uint32_t indexCount,
                                      uint32_t instanceCount,
                                      uint32_t startIndex,
                                      int32_t  baseVertex,
                                      uint32_t startInstance,
                                      RHI::CommandList cmd) = 0;

    // ---- Ray-tracing acceleration structures ----
    // Every ray is an inline RayQuery against the scene TLAS (DDGISceneAS), bound as a root SRV by its
    // address (RHI::AccelerationStructure::address). Builds record onto any list and emit NO barrier:
    // push GPUBarrier::Memory(&as) on what a build wrote (or Memory() for all UAV work) before anything
    // reads it or reuses its scratch region. Without GetCapabilities().raytracing every call is a no-op
    // returning false / 0.

    // What a BLAS build over @p geoms with @p flags needs (the build must pass the same flags). False
    // without ray tracing or geometry.
    virtual bool GetBLASPrebuildInfo(const RHI::ASTriangles* geoms, uint32_t count, RHI::ASBuildFlags flags,
                                     RHI::ASPrebuildInfo& out) const { out = {}; return false; }
    // What a TLAS build of up to @p maxInstances instances with @p flags needs.
    virtual bool GetTLASPrebuildInfo(uint32_t maxInstances, RHI::ASBuildFlags flags,
                                     RHI::ASPrebuildInfo& out) const { out = {}; return false; }
    // @p sizeBytes of acceleration-structure memory: a RESULT a BLAS / TLAS is built into, or a
    // SCRATCH a build works in. Not zero-filled.
    virtual bool CreateAccelerationStructure(uint64_t sizeBytes, RHI::ASMemory memory,
                                             RHI::AccelerationStructure& out) { return false; }
    // Releases @p as (with any compacted-size query it still holds) once the frames that could read it
    // retire (the frame-stamped deferred release), and resets it. The second form instead waits for the
    // work submitted so far on @p queue: for memory only that queue used (the async-compute AS builds'
    // scratch), which the frame window does not cover.
    virtual void DestroyAccelerationStructure(RHI::AccelerationStructure& as) {}
    virtual void DestroyAccelerationStructure(RHI::AccelerationStructure& as, RHI::QueueType queue) {}
    // Records a BLAS build of @p geoms into @p dst, working in @p scratch from @p scratchOffset (a
    // multiple of RHI::kASAlignment). PERFORM_UPDATE in @p flags refits @p dst in place; its build must
    // have carried ALLOW_UPDATE.
    virtual void BuildBLAS(RHI::CommandList cmd, const RHI::ASTriangles* geoms, uint32_t count,
                           RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                           const RHI::AccelerationStructure& scratch, uint64_t scratchOffset) {}
    // Records a TLAS build into @p dst from the first @p instanceCount RHI::ASInstance records of
    // @p instances (PERFORM_UPDATE: refit in place).
    virtual void BuildTLAS(RHI::CommandList cmd, const RHI::GPUBuffer& instances, uint32_t instanceCount,
                           RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                           const RHI::AccelerationStructure& scratch) {}
    // Records a query of @p as's compacted size (it was built with ALLOW_COMPACTION and a barrier on it
    // was pushed); ReadCompactedSize answers once this list has executed. False when the query's
    // memory could not be made.
    virtual bool QueryCompactedSize(RHI::CommandList cmd, const RHI::AccelerationStructure& as) { return false; }
    // The size the query recorded (0: none). Read on the CPU, so only after the list executed (the
    // caller's frame delay). Releases the query's memory.
    virtual uint64_t ReadCompactedSize(const RHI::AccelerationStructure& as) { return 0; }
    // Makes a right-sized RESULT in @p out and records a compacting copy of @p src into it.
    virtual bool CompactAccelerationStructure(RHI::CommandList cmd, const RHI::AccelerationStructure& src,
                                              uint64_t compactedSize, RHI::AccelerationStructure& out) { return false; }

    // ---- DXR ray-tracing pipeline ----
    // Kept and proved, not used: every ray the engine traces is an inline RayQuery. The one caller is
    // the -dxr_parity self-test (DXRParityTest).

    // A ray-tracing state object from @p desc with its shader table: the ray-generation record, then
    // the miss records, then the hit-group records, no local root arguments. False without ray tracing,
    // or when the state object does not link (an export the library lacks).
    virtual bool CreateRaytracingPipeline(const RHI::RaytracingPipelineDesc& desc,
                                          RHI::RaytracingPipeline& out) { return false; }
    // Releases it with its shader table (deferred); resets @p pipeline.
    virtual void DestroyRaytracingPipeline(RHI::RaytracingPipeline& pipeline) {}
    // Binds the state object and its layout as the compute root signature. Set the layout's arguments
    // with the SetCompute* binders AFTER this: binding a root signature drops them.
    virtual void BindRaytracingPipeline(const RHI::RaytracingPipeline& pipeline, RHI::CommandList cmd) {}
    // Launches @p width x @p height x @p depth ray-generation invocations with the bound pipeline's
    // shader table.
    virtual void DispatchRays(const RHI::RaytracingPipeline& pipeline, uint32_t width, uint32_t height,
                              uint32_t depth, RHI::CommandList cmd) {}

    // ---- Command recording - compute dispatch ----
    // Binds a compute pipeline state and its compute root signature.
    virtual void BindComputePipelineState(const RHI::PipelineState& pso,
                                          RHI::CommandList cmd) {}

    // Dispatches compute thread groups.
    virtual void DispatchCompute(uint32_t threadGroupX,
                                 uint32_t threadGroupY,
                                 uint32_t threadGroupZ,
                                 RHI::CommandList cmd) {}

    // One compute dispatch whose thread-group counts the GPU wrote: three uint32 (x, y, z:
    // D3D12_DISPATCH_ARGUMENTS) at @p byteOffset in @p args, which must be in INDIRECT_ARGUMENT. The
    // device owns the one dispatch command signature; it sets no root arguments, so it serves the
    // shared layout and every pipeline layout alike.
    virtual void DispatchComputeIndirect(const RHI::GPUBuffer& args, uint64_t byteOffset,
                                         RHI::CommandList cmd) {}

    // Dispatches mesh-shader thread groups (DX12 Ultimate, SM 6.5+): with an amplification shader
    // bound the AS launches with these counts, with only an MS bound the MS launches directly. No-op on
    // backends without mesh shaders.
    virtual void DispatchMesh(uint32_t threadGroupX,
                              uint32_t threadGroupY,
                              uint32_t threadGroupZ,
                              RHI::CommandList cmd) {}

    // Binds a GPU buffer as a compute root inline CBV at root parameter @p rootSlot.
    virtual void SetComputeRootCBV(RHI::Slots::CmpParam rootSlot,
                                   const RHI::GPUBuffer& cb,
                                   uint32_t byteOffset,
                                   RHI::CommandList cmd) {}
    virtual void SetComputeRootCBV(RHI::Slots::CmpParam rootSlot,
                                   const RHI::GPUBuffer& cb,
                                   RHI::CommandList cmd)
    { SetComputeRootCBV(rootSlot, cb, 0, cmd); }
    // A compute root inline CBV at root parameter @p rootSlot from a GPU address (AllocConstants'
    // gpu). Zero is a no-op.
    virtual void SetComputeRootCBVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuAddress, RHI::CommandList cmd) {}

    // Binds a descriptor table to the compute root signature.
    virtual void SetComputeDescriptorTable(RHI::Slots::CmpParam rootSlot,
                                           uint64_t gpuHandle,
                                           RHI::CommandList cmd) {}

    // Writes @p count 32-bit values from @p data into the root constants at @p rootSlot, from value
    // @p destOffset on. Only a pass's private compute root signature has root constants.
    virtual void SetComputeRootConstants(RHI::Slots::CmpParam rootSlot, uint32_t count,
                                         const void* data, uint32_t destOffset,
                                         RHI::CommandList cmd) {}

    // Binds the shared compute root signature again after a pipeline with its own layout, for code
    // that sets the shared layout's arguments before it binds a pipeline (AutoExposure detaches an SRV
    // that way). Root values are invalidated. The Renderer ends with it the private-layout work it
    // records into lists other code continues (RRGuides, RRColorAlign, DLSS after its evaluate, FSR 2
    // after its dispatch).
    virtual void RestoreDefaultComputeRootSignature(RHI::CommandList cmd) {}

    // A compute root SRV by GPU virtual address (a TLAS, a buffer of records) on a private compute root
    // signature. Zero binds a null root SRV, as D3D12 does: the shader must not read it.
    virtual void SetComputeRootSRVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuVA,
                                          RHI::CommandList cmd) {}

    // GPU descriptor handle of a texture's UAV (space2); 0 when the texture was not created with the
    // UNORDERED_ACCESS bind flag.
    virtual uint64_t GetTextureUAVGpuHandle(const RHI::Texture& tex) const { return 0; }

    // GPU descriptor handle of a buffer's UAV; 0 when the buffer was not created with the
    // UNORDERED_ACCESS bind flag.
    virtual uint64_t GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const { return 0; }

    // GPU descriptor handle of a buffer's SRV; 0 when the buffer was not created with the
    // SHADER_RESOURCE bind flag.
    virtual uint64_t GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const { return 0; }

    // Index of a buffer's SRV in the shader-visible descriptor heap, for shaders that index the heap
    // directly instead of binding a table; ~0u when the buffer has no SRV.
    virtual uint32_t GetBufferSRVHeapIndex(const RHI::GPUBuffer& buf) const { return ~0u; }

    // Index of a constant buffer's whole-buffer CBV in the shader-visible descriptor heap (CreateBuffer
    // makes one for every CONSTANT_BUFFER up to 64 KB), for a shader that opens it as
    // `ConstantBuffer<T> cb = ResourceDescriptorHeap[idx]` - e.g. the frame's LightCB from a compute
    // shader whose signature has no root CBV for it. Stable for the buffer's life, like the SRV index
    // above; ~0u when the buffer has no CBV.
    virtual uint32_t GetBufferCBVHeapIndex(const RHI::GPUBuffer& buf) const { return ~0u; }

    // Converts ANY shader-visible GPU descriptor handle into its index in the shader-visible
    // CBV/SRV/UAV heap - the value SM 6.6 shaders feed to ResourceDescriptorHeap[]; ~0u for a null /
    // foreign handle. Every descriptor table costs 1 of the 64 root DWORDs (RootSignatureLayout.h), so
    // a single-SRV resource is passed as a uint index in a constant buffer + ResourceDescriptorHeap[] in
    // HLSL (0 DWORDs) - the rule for a new resource - and its producer already hands out a GPU handle:
    // this is the conversion.
    // Recompute it every frame from the CURRENT handle, never cache it at Init: Resize recreates
    // textures and the free list recycles handle ids, so a cached index can silently point at someone
    // else's descriptor.
    virtual uint32_t DescriptorHeapIndexFromGpuHandle(uint64_t gpuHandle) const { return ~0u; }

    // ---- Typed bindless handles ----
    // The checked way to put a descriptor into a constant buffer field: a BindlessHandle<K> carries
    // gen << 20 | index, and the device makes one only after the heap's record says the slot holds a K.

    // The handle of shader-visible heap slot @p heapIndex when the heap records it as holding a @p kind
    // (a null one counts) and, for a structured buffer with @p stride != 0, that stride. Otherwise the
    // handle of @p kind's null descriptor: silently when @p heapIndex is ~0u (nothing named - the field
    // is absent), with an ERROR naming @p what when the slot holds something else (once per what, slot
    // and kind; all counted in the shutdown [BINDLESS] line). Never 0, ~0u or -1 for a kind that has a
    // null. Checked in Release too: it is one table read.
    virtual uint32_t MakeBindlessHandle(uint32_t heapIndex, uint8_t kind, uint32_t stride,
                                        const char* what) { return ~0u; }

    // Heap index of @p kind's null descriptor: a fixed BINDLESS_NULL_* slot, or, for a kind without one
    // (a structured buffer, per stride), one made on first use. ~0u for a kind no null is made for.
    virtual uint32_t BindlessNull(uint8_t kind, uint32_t stride = 0) { return ~0u; }

    // @p count consecutive null StructuredBuffer SRVs in one block of the shader-visible heap's static
    // region, descriptor i of stride @p strides[i]: type-correct nulls a pass keeps for bindless fields
    // behind a false gate (LightingPass's probe grid / index / probe buffer). Descriptor i's GPU handle
    // is @p out.Gpu(i). False when the static region has no room. Hand it back with
    // ReleaseDescriptorBlock.
    virtual bool CreateNullStructuredSrvBlock(const uint32_t* strides, uint32_t count,
                                              RHI::DescriptorBlock& out) { return false; }
    // Gives a block back - at once when @p quiesced (no frame in flight can name it: the caller's
    // GetRetiredFrame() test), otherwise through the device's frame-stamped deferred bucket - and
    // resets @p block.
    virtual void ReleaseDescriptorBlock(RHI::DescriptorBlock& block, bool quiesced) {}
    // @p count consecutive descriptors of the static region, NOT initialised: the caller first writes
    // every one a shader can read (CopyCbvSrvUavDescriptors to @p out.Cpu(i), WriteTextureCubeArraySrv).
    // False when the region has no room. Hand it back with ReleaseDescriptorBlock.
    virtual bool AllocateDescriptorBlock(uint32_t count, RHI::DescriptorBlock& out) { return false; }
    // Writes descriptor @p index of @p block as a TextureCubeArray SRV of @p texture in its own format:
    // @p numCubes cubes from cube @p firstCube (array face firstCube * 6), mips 0 .. @p mipLevels - 1.
    // False when the block, the index or the texture is not valid.
    virtual bool WriteTextureCubeArraySrv(const RHI::DescriptorBlock& block, uint32_t index,
                                          const RHI::Texture& texture, uint32_t firstCube,
                                          uint32_t numCubes, uint32_t mipLevels) { return false; }

    // Points fixed heap slot @p slot (BINDLESS_DEBUG_*, shared/bindless_kind.hlsli) at @p texture's UAV
    // (an R32_UINT texture made with UNORDERED_ACCESS), or back at that kind's null for null. Main
    // thread, at a frame boundary (a frame in flight that writes through the slot sees whichever
    // descriptor it reads). False for a slot outside the range or a texture without a UAV.
    virtual bool SetFixedDebugUav(uint32_t slot, const RHI::Texture* texture) { return false; }
    // Points BINDLESS_DEBUG_COST_TABLE at @p buffer's SRV (a StructuredBuffer<uint>, stride 4), or back
    // at the structured null for null. Same thread and frame-boundary rule as SetFixedDebugUav. False
    // for any other slot or a buffer without an SRV.
    virtual bool SetFixedDebugBuffer(uint32_t slot, const RHI::GPUBuffer* buffer) { return false; }

    template<uint8_t K>
    RHI::BindlessHandle<K> BindlessOf(uint64_t gpuDescriptorHandle, const char* what,
                                      uint32_t stride = 0)
    {
        const uint32_t index = gpuDescriptorHandle
            ? DescriptorHeapIndexFromGpuHandle(gpuDescriptorHandle) : ~0u;
        return RHI::BindlessHandle<K>(MakeBindlessHandle(index, K, stride, what));
    }
    template<uint8_t K>
    RHI::BindlessHandle<K> Srv(const RHI::Texture& texture, const char* what)
    { return BindlessOf<K>(GetTextureSRVGpuHandle(texture), what); }
    template<uint8_t K>
    RHI::BindlessHandle<K> Uav(const RHI::Texture& texture, const char* what)
    { return BindlessOf<K>(GetTextureUAVGpuHandle(texture), what); }
    template<typename T>
    RHI::BindlessHandle<RHI::Bindless::Kind::StructuredBuffer>
    SrvStructured(const RHI::GPUBuffer& buffer, const char* what)
    {
        return BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(
            GetBufferSRVGpuHandle(buffer), what, static_cast<uint32_t>(sizeof(T)));
    }
    template<typename T>
    RHI::BindlessHandle<RHI::Bindless::Kind::RWStructuredBuffer>
    UavStructured(const RHI::GPUBuffer& buffer, const char* what)
    {
        return BindlessOf<RHI::Bindless::Kind::RWStructuredBuffer>(
            GetBufferUAVGpuHandle(buffer), what, static_cast<uint32_t>(sizeof(T)));
    }
    RHI::BindlessHandle<RHI::Bindless::Kind::Cbv>
    Cbv(const RHI::GPUBuffer& buffer, const char* what)
    {
        return RHI::BindlessHandle<RHI::Bindless::Kind::Cbv>(
            MakeBindlessHandle(GetBufferCBVHeapIndex(buffer), RHI::Bindless::Kind::Cbv, 0, what));
    }
    // The null of K itself, for a field that is absent on purpose.
    template<uint8_t K>
    RHI::BindlessHandle<K> NullOf(uint32_t stride = 0)
    { return RHI::BindlessHandle<K>(MakeBindlessHandle(~0u, K, stride, nullptr)); }

    // GPU address of a buffer's memory, for acceleration-structure and indirect-argument code that
    // addresses memory rather than descriptors; 0 for an invalid buffer.
    virtual uint64_t GetBufferGPUAddress(const RHI::GPUBuffer& buf) const { return 0; }

    // GPU descriptor handle of the backend's bindless texture table, for passes that bind
    // g_AllTextures[] as a descriptor table; 0 when the backend allocates none.
    // In the graphics root signature it is root param Gfx::BindlessTex = t0 space2 (Lighting.ps.hlsl,
    // material_bindings.hlsli); in GraphicsDX12's compute root signature root param 17 = t0 space3
    // (DecalApply.cs.hlsl). The spaces belong to the ROOT SIGNATURE, not to this handle: the RT / DDGI
    // compute shaders build their own root signatures and declare g_AllTextures[] at t0 space2.
    virtual uint64_t GetBindlessTextureTableGpuHandle() const { return 0; }

    // CPU descriptor handle of a texture's SRV, for descriptor-table staging such as the
    // custom-material SRV rings; 0 when the texture has no SRV or the backend exposes no CPU handles.
    virtual uint64_t GetTextureSRVCpuHandle(const RHI::Texture& texture) const { return 0; }

    // Copies @p count CBV/SRV/UAV descriptors from @p srcCpuHandle to @p dstCpuHandle, for frame
    // descriptor-table builders. The caller advances the destination handle by the descriptor
    // increment between separate copies.
    virtual void CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                          uint64_t srcCpuHandle,
                                          uint32_t count) {}

    // The CBV/SRV/UAV heap's descriptor increment (bytes), for code that composes a descriptor table
    // slot by slot; 0 when the backend has no CPU descriptor heap.
    virtual uint32_t GetCbvSrvUavDescriptorIncrement() const { return 0; }

    // Transitions the HDR offscreen render target to @p newState: records the barrier into @p cmd and
    // updates the device's state tracking.
    virtual void SetHdrTextureState(RHI::ResourceState newState,
                                    RHI::CommandList cmd) {}

    // Brackets the window in which the render graph records passes in parallel on worker threads.
    // Backends use it to detect races on shared resource-state trackers; it changes no rendering.
    virtual void SetParallelPassRecording(bool active) {}

    // ---- Command recording - barriers ----
    virtual void PushBarrier(const RHI::GPUBarrier& barrier,
                             RHI::CommandList cmd) = 0;

    // Batched PushBarrier: issues @p count barriers as ONE backend barrier call. IMMEDIATE like
    // PushBarrier (nothing is deferred past the caller's next command), and the per-barrier state
    // book-keeping is applied in array order, so the result equals @p count PushBarrier calls, only
    // cheaper.
    virtual void PushBarriers(const RHI::GPUBarrier* barriers,
                              uint32_t               count,
                              RHI::CommandList       cmd) = 0;

    // PushBarrier that leaves the device's state book where it was: one leg of a round trip a pass makes
    // inside its own list on a texture the RENDER GRAPH tracks (SpotShadowPass's static-cache copy,
    // DEPTHSTENCIL -> COPY_DEST -> DEPTHSTENCIL). The graph's Phase A writes the book for every pass
    // before Phase B records any, so a barrier recorded in Phase B would overwrite what later passes
    // left there; the round trip ends where it began, so the book must not hear of it. The barrier
    // journal still does. Default: PushBarrier (a backend without a book).
    virtual void PushBarrierScoped(const RHI::GPUBarrier& barrier, RHI::CommandList cmd)
    {
        PushBarrier(barrier, cmd);
    }

    // Does the BACKEND's own resource-state tracker agree that @p tex is in @p expected? The RenderGraph
    // keeps a second, independent tracker (VirtualTexture::currentState) and drives its barriers off
    // it; when the two diverge, a depth buffer gets a StateBefore the GPU rejects much later, naming
    // only a command-list number. The graph's depth tripwire (EmitBarriersBeforePass) asks this once per
    // depth handle per pass, so the FIRST divergence is loud and names the pass.
    // @p deviceStateOut / @p expectedStateOut optionally receive the backend's raw state bits and its
    // translation of @p expected: opaque to the caller, but printable in the hex the D3D12 debug layer
    // uses.
    // True (agreement) for an unknown handle or a resource the backend never barriered: "no opinion"
    // must never read as "divergence".
    // @p plane: one plane of a depth-stencil texture (0 depth, 1 stencil); -1 = every plane, each of
    // which must agree.
    virtual bool DeviceStateAgrees(const RHI::Texture& /*tex*/,
                                   RHI::ResourceState  /*expected*/,
                                   uint32_t*           /*deviceStateOut*/   = nullptr,
                                   uint32_t*           /*expectedStateOut*/ = nullptr,
                                   int                 /*plane*/            = -1) const
    { return true; }

    // The backend's own book for @p tex as the ONE RHI state that names it: where an imported texture's
    // graph tracker starts each Execute (RenderGraph::ImportTexture). UNDEFINED for an unknown handle, a
    // never-barriered texture, or a backend state no single RHI state names. @p plane: one plane of a
    // depth-stencil texture (0 depth, 1 stencil); -1 names a state only while every plane holds it
    // (UNDEFINED while they differ).
    virtual RHI::ResourceState QueryTextureState(const RHI::Texture& /*tex*/, int /*plane*/ = -1) const
    { return RHI::ResourceState::UNDEFINED; }

    // The backend object's own debug name for @p tex (UTF-8) - the name the barrier journal reads; ""
    // for an unknown handle or an unnamed resource. A reader after creation asks here, never the
    // texture's desc: TextureDesc::debugName is borrowed for the CreateTexture call alone, so a creator
    // that built it in a temporary (RTReflectionPass's "RTR.denoised") leaves that pointer dangling.
    virtual std::string QueryTextureName(const RHI::Texture& /*tex*/) const
    { return {}; }

    // ---- Command recording - render targets ----
    // Binds several render targets and an optional depth-stencil; @p view as in
    // SetRenderTargetToHdrWithDepth.
    virtual void SetRenderTargets(uint32_t numRTs,
                                  const RHI::Texture* const* rtvs,
                                  const RHI::Texture*        dsv,
                                  RHI::CommandList           cmd,
                                  const RHI::DepthView&      view = {}) = 0;

    // Binds several render targets PLUS the device-internal HdrSceneColor as the LAST RT (RT[numRTs]),
    // so GBufferPass / TerrainPass write per-material emissive straight into scene colour (emissive
    // bypasses the BRDF; lighting adds on top with additive blend). Transitions HDR to RENDER_TARGET if
    // needed.
    virtual void SetRenderTargetsAndHdr(uint32_t numRTs,
                                        const RHI::Texture* const* rtvs,
                                        const RHI::Texture*        dsv,
                                        RHI::CommandList           cmd,
                                        const RHI::DepthView&      view = {}) = 0;

    virtual void ClearRenderTarget(const RHI::Texture& texture,
                                   const float         color[4],
                                   RHI::CommandList    cmd) = 0;

    virtual void ClearDepthStencil(const RHI::Texture& texture,
                                   float               depth,
                                   uint8_t             stencil,
                                   RHI::CommandList    cmd) = 0;

    // ---- Command recording - array-slice depth targets (CSM cascades, etc.) ----
    // Binds one array slice of a DEPTH_STENCIL Texture2DArray (arraySize > 1) as the only DSV and
    // clears the RT list (depth-only pass). The texture must already be in DEPTHSTENCIL and have the
    // DEPTH_STENCIL bind flag.
    virtual void SetDepthStencilSlice(const RHI::Texture& depthTex,
                                      uint32_t            arraySlice,
                                      RHI::CommandList    cmd) {}

    // Binds a single (non-array) DEPTH_STENCIL Texture2D as the only DSV and clears the RT list
    // (depth-only pass), e.g. PerObjectShadowPass's high-res player shadow map. The texture must
    // already be in DEPTHSTENCIL and have the DEPTH_STENCIL bind flag.
    virtual void SetDepthStencilTarget(const RHI::Texture& depthTex,
                                       RHI::CommandList    cmd) {}

    // Clears one array slice of a DEPTH_STENCIL Texture2DArray; the texture must be in DEPTHSTENCIL.
    virtual void ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                        uint32_t            arraySlice,
                                        float               depth,
                                        uint8_t             stencil,
                                        RHI::CommandList    cmd) {}

    // Binds ONE (arraySlice, mip) subresource of a render-target texture as the sole RT, with an
    // optional depth-stencil @p ds (null for a colour-only pass): how cube-face capture renders into one
    // face of a TextureCubeArray at a time.
    virtual void SetRenderTargetSubresource(const RHI::Texture& rt,
                                            uint32_t            arraySlice,
                                            uint32_t            mip,
                                            const RHI::Texture* ds,
                                            RHI::CommandList    cmd) {}

    // Clears ONE (arraySlice, mip) subresource of a render target to @p color - the one
    // SetRenderTargetSubresource binds (ClearRenderTarget clears through the whole-texture view). It
    // must be in RENDER_TARGET. The probe capture clears each cube face this way.
    virtual void ClearRenderTargetSubresource(const RHI::Texture& rt,
                                              uint32_t            arraySlice,
                                              uint32_t            mip,
                                              const float         color[4],
                                              RHI::CommandList    cmd) {}

    // ---- Command recording - root constants / stencil ref ----
    // Sets the stencil reference value for the following draws.
    virtual void SetStencilRef(uint32_t ref, RHI::CommandList cmd) {}

    // Writes one 32-bit value into the graphics root constants at @p rootSlot, word @p offsetInWords:
    // per-draw scalars such as OutlinePass's outlinePixels.
    virtual void SetGraphicsRootConstant(RHI::Slots::GfxParam rootSlot,
                                         uint32_t value,
                                         uint32_t offsetInWords,
                                         RHI::CommandList cmd) {}

    // Binders for a graphics PSO with its OWN pipeline layout, by absolute root parameter (the shared
    // signature's binders above take group-relative slots): a root CBV from a GPU address (zero is a
    // no-op), @p count root constants from @p data at value @p destOffset, and a descriptor table from
    // a GPU descriptor handle (zero is a no-op). Bind the PSO first: BindPipelineState sets its layout,
    // which invalidates every root argument when the root signature changes.
    virtual void SetGraphicsRootCBVAddress(RHI::Slots::GfxParam rootSlot, uint64_t gpuAddress,
                                           RHI::CommandList cmd) {}
    virtual void SetGraphicsRootConstants(RHI::Slots::GfxParam rootSlot, uint32_t count, const void* data,
                                          uint32_t destOffset, RHI::CommandList cmd) {}
    virtual void SetGraphicsDescriptorTable(RHI::Slots::GfxParam rootSlot, uint64_t gpuHandle,
                                            RHI::CommandList cmd) {}

    // ---- Per-subresource descriptor queries ----
    // GPU descriptor handle of the UAV for mip @p mip of a multi-mip texture (for a Texture2DArray /
    // TextureCube it covers every array slice at that mip); 0 when the mip is out of range or the
    // texture has no UAV bind flag.
    virtual uint64_t GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                               uint32_t mip) const { return 0; }

    // CPU descriptor handle of an RTV targeting exactly one (cubeIdx, face, mip) subresource of a
    // TextureCubeArray, allocated on first request and cached on the texture entry - the reflection
    // probe capture renders one cube face at a time through it. 0 when the subresource is out of range
    // or the RTV bind flag is missing.
    virtual uint64_t GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                                    uint32_t cubeIdx,
                                                    uint32_t face,
                                                    uint32_t mip) { return 0; }

    // GPU descriptor handle of a Texture2DArray UAV viewing the 6 slices [cubeIdx*6 .. cubeIdx*6+5] at
    // one mip: one cube of a TextureCubeArray as a 6-layer array UAV, which the prefilter compute shader
    // writes one probe's GGX-filtered mip through.
    virtual uint64_t GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                                   uint32_t cubeIdx,
                                                   uint32_t mip) { return 0; }

    // ---- GPU profiler hooks ----
    // Thin wrappers so passes record timestamp regions without casting to the concrete device. No-ops
    // by default (headless / unprofiled backends); GraphicsDX12 forwards to its GPUProfiler.

    // Returns an opaque region index, or ~0u when profiling is disabled.
    virtual uint32_t BeginGPUTimestamp(RHI::CommandList /*cmd*/,
                                       const char* /*name*/) { return ~0u; }
    virtual void     EndGPUTimestamp  (RHI::CommandList /*cmd*/,
                                       uint32_t /*regionIndex*/) {}
    virtual bool     IsGPUProfilerEnabled() const { return false; }

    // Split variants for callers that open the timestamp region on one thread and record the pass on
    // another. BeginGPUTimestamp couples the profiler region to a Tracy GPU zone, and Tracy fixes zone
    // PARENTAGE at begin time on the emitting thread: RenderGraph opens its regions serially in Phase A
    // and records in parallel in Phase B, so combined begins would nest every pass under the one opened
    // before it (garbage per-pass durations). RenderGraph therefore opens the profiler region with
    // *NoTracy (its indices stay allocation-ordered for a stable editor HUD) and brackets the Tracy zone
    // around the actual recording with the two calls below. Serial, correctly nested call sites keep
    // the combined BeginGPUTimestamp / EndGPUTimestamp.
    virtual uint32_t BeginGPUTimestampNoTracy(RHI::CommandList /*cmd*/,
                                              const char* /*name*/) { return ~0u; }
    virtual void     EndGPUTimestampNoTracy  (RHI::CommandList /*cmd*/,
                                              uint32_t /*regionIndex*/) {}
    // Opens / closes a Tracy GPU zone on `cmd` for an already-allocated profiler region index. Safe from
    // worker threads: Tracy's D3D12 query ids come from an atomic and its zone events go through the
    // serial queue (with an explicit thread handle). Both calls for a region MUST happen on the same
    // thread, around that pass's recording, while `cmd` is still open.
    virtual void     BeginTracyGPUZone(RHI::CommandList /*cmd*/,
                                       const char* /*name*/,
                                       uint32_t /*regionIndex*/) {}
    virtual void     EndTracyGPUZone  (uint32_t /*regionIndex*/) {}

    // Draw / dispatch / PSO / barrier counts of what a pass recorded. The command lists THIS THREAD
    // begins from now on belong to @p owner (0 = nobody); RenderGraph sets it to the pass around its
    // setup and its recording, so the chunk lists a pass opens count as its own.
    virtual void SetRecordingOwner(uintptr_t /*owner*/) {}
    // The counts of every command list begun this frame with @p owner.
    virtual Diag::Prof::GpuCounters GetOwnerCounters(uintptr_t /*owner*/) { return {}; }
    // Sets a profiler region's counts (a region's end sets what its own command list recorded since
    // the begin).
    virtual void AnnotateGPUTimestamp(uint32_t /*regionIndex*/, const Diag::Prof::GpuCounters& /*counters*/) {}

    // Closes the submit batch right AFTER `cmd`, giving it a fence value that excludes everything
    // queued behind it. Submitted lists coalesce into per-queue batches sharing ONE fence value, so a
    // cross-queue consumer waiting on any member waits for the whole batch - the compute queue would
    // idle until the whole graphics prologue (skinning, shadows, culling, G-Buffer) retired. Marking the
    // producer lets its consumer start as soon as the producer itself is done.
    // Costs one extra ExecuteCommandLists + Signal: use it only where a consumer on another queue is
    // demonstrably blocked, never as a default.
    virtual void     SetSubmitBarrier(RHI::CommandList /*cmd*/) {}

    // Last resolved GPU time (ms) of the named timestamp region. False, @p outGpuMs untouched, when
    // profiling is off or the region has no result yet. Lets budget watchdogs read their own cost
    // without the concrete profiler object.
    virtual bool GetGPUTimestampResult(const char* /*regionName*/,
                                       float& /*outGpuMs*/) const { return false; }

    // Whole-frame GPU cost, for the harness per-frame record.
    // False when the timestamp profiler is off (OFF by default in every configuration; -gpuprofile
    // enables it) or nothing has resolved yet. A caller MUST then emit null, not 0.0: a zero GPU time is
    // indistinguishable from "never measured", and the per-frame record is worth having only because
    // absent data reads as absent.
    // @param outEffectiveMs  GPU busy time: the UNION of all queue intervals, not their sum (the sum
    //                        double-counts nesting and async-compute overlap).
    // @param outOverlapMs    How much of that overlapped across queues.
    // @param outAgeFrames    How many frames ago these numbers were MEASURED. The profiler reads back
    //                        the slot written on its previous visit, so this tracks the in-flight depth
    //                        (typically 2). Do not hard-code it; attributing the numbers to the CURRENT
    //                        frame is a plausible-looking lie.
    virtual bool GetGPUFrameTiming(float& /*outEffectiveMs*/, float& /*outOverlapMs*/,
                                   uint32_t& /*outAgeFrames*/) const { return false; }

    // The last resolved frame of timestamp regions (RHI::GpuTimingView). Fills @p out and returns true
    // whenever the device has a timestamp profiler, enabled or not (the view says which); false
    // without one. The view points into the profiler's own arrays and is valid until its next
    // BeginFrame: read it where the device frame runs.
    virtual bool GetGPUTimings(RHI::GpuTimingView& out) const { out = {}; return false; }

    // ---- Device telemetry ----
    // Pure observation: nothing here allocates.
    // Local video memory in use and the OS budget for this process; false when the adapter query is
    // unavailable.
    virtual bool QueryVramUsage(uint64_t& /*usedBytes*/, uint64_t& /*budgetBytes*/) { return false; }
    // One struct per managed descriptor heap (RHI::DescriptorHeapStats).
    virtual RHI::DescriptorHeapStats GetCbvSrvUavHeapStats() const { return {}; }
    virtual RHI::DescriptorHeapStats GetRtvHeapStats()       const { return {}; }
    virtual RHI::DescriptorHeapStats GetDsvHeapStats()       const { return {}; }
    virtual RHI::DescriptorHeapStats GetSamplerHeapStats()   const { return {}; }
    // The CBV/SRV/UAV heap's static region: the CPU-staging and shader-visible cursors against their
    // capacities.
    virtual void GetCbvSrvUavStaticUsage(uint32_t& cpuUsed, uint32_t& cpuCap,
                                         uint32_t& gpuUsed, uint32_t& gpuCap) const
    { cpuUsed = cpuCap = gpuUsed = gpuCap = 0; }
    // The most command lists one frame used.
    virtual uint32_t GetCommandListPeak() const { return 0; }
    // The largest game-to-render frame lag the slot-parity checks saw (0: nothing to catch).
    virtual uint64_t MaxObservedFrameLag() const { return 0; }

    // Holds a non-owning subscription to the GPU timestamp profiler: it runs while at least one want is
    // held, on top of whatever -gpuprofile or an editor panel asked for. Paired: every Add gets exactly
    // one Release. Costs two timestamps per pass plus one ResolveQueryData per frame while held, so
    // hold one only for something the user turned on.
    virtual void AddGPUProfilerWant()     {}
    virtual void ReleaseGPUProfilerWant() {}

    // Upload-path instrumentation: a resettable window over the device's
    // upload flushes, for the editor's world-load profile and the preview
    // builds. The never-reset process totals (frames.jsonl, Profiler.Upload)
    // are Graphics/UploadStats.h.
    virtual void     ResetUploadStats()          {}
    virtual double   GetUploadStallMs()    const { return 0.0; }
    virtual uint32_t GetUploadFlushCount() const { return 0u; }

    // Debug event markers for GPU-capture tools (RenderDoc / PIX / Nsight). Independent of the GPU
    // profiler but gated: a backend emits them only while its markers are on (GraphicsDX12:
    // -gpumarkers, DX12_MARKERS=1, or a capture tool loaded at device init). BeginGPUTimestamp also
    // opens one with the region's name, so standalone passes are named too. No-op by default.
    virtual void     BeginEventMarker(RHI::CommandList /*cmd*/, const char* /*name*/) {}
    virtual void     EndEventMarker  (RHI::CommandList /*cmd*/) {}

    // Names a command list for this use ("cmd<id>:<name>") so DRED reports, debug-layer messages and
    // captures say what was recording instead of a pool slot. Lists are pooled, so the name resets to
    // "cmd<id>" on every begin. Works only while markers or DRED are on; no-op by default. The frame
    // snapshot keeps the name too, so it must outlive the frame (a string literal or a pass's
    // GetName(), never a temporary).
    virtual void     SetCommandListName(RHI::CommandList /*cmd*/, const char* /*name*/) {}

    // ---- The device frame as data (Graphics/FrameSnapshot.h) ----
    // ArmFrameJournal() BEFORE BeginFrame: that whole frame records a barrier journal, and EndFrame
    // builds a GfxDiag::FrameSnapshot of it - the command lists, their declared waits, the submission
    // order and batches, every barrier. TakeFrameSnapshot() hands it over once (null when the last frame
    // was not journaled). FrameJournalActive() is true from that BeginFrame until its EndFrame. All
    // no-ops by default.
    virtual void ArmFrameJournal() {}
    virtual bool FrameJournalActive() const { return false; }
    virtual std::shared_ptr<const GfxDiag::FrameSnapshot> TakeFrameSnapshot() { return nullptr; }

    // ---- Command recording - descriptor heap management ----
    // Binds the shader-visible CBV/SRV/UAV and sampler heaps. Call before any BindResource /
    // SetDynamicSampler.
    virtual void BindDescriptorHeaps(RHI::CommandList cmd) = 0;

    // ---- Buffer mapping (UPLOAD-heap constant / staging buffers) ----
    // Maps an UPLOAD buffer and returns a CPU pointer, valid until UnmapBuffer.
    virtual void* MapBuffer(const RHI::GPUBuffer& buffer) = 0;

    // Unmaps an UPLOAD buffer mapped by MapBuffer.
    virtual void  UnmapBuffer(const RHI::GPUBuffer& buffer) = 0;

    // ---- Readback and resource destruction ----
    // Copies the 1x1 pixel at (@p srcX, @p srcY) of a 2D texture's mip 0 to the start of READBACK buffer
    // @p dst. The texture must be in COPY_SRC.
    virtual void CopyTexturePixelToBuffer(const RHI::Texture& src,
                                          uint32_t srcX, uint32_t srcY,
                                          RHI::GPUBuffer& dst,
                                          RHI::CommandList cmd) = 0;

    // The WHOLE mip 0 of a 2D texture into a READBACK buffer (the object-ID image, the material-preview
    // thumbnails). Ask GetTexture2DReadbackFootprint first: the copy's rows sit @p outRowPitch apart -
    // the backend's copy alignment (D3D12: 256 bytes), not width x bytes per pixel - and the buffer must
    // hold @p outTotalBytes; false for a texture that is not 2D. CopyTexture2DToBuffer issues no
    // barrier: @p src must be in COPY_SRC.
    virtual bool GetTexture2DReadbackFootprint(const RHI::Texture& tex, uint32_t& outRowPitch,
                                               uint32_t& outRows, uint64_t& outTotalBytes) const
    { outRowPitch = outRows = 0; outTotalBytes = 0; return false; }
    virtual void CopyTexture2DToBuffer(const RHI::Texture& src, const RHI::GPUBuffer& dst,
                                       RHI::CommandList cmd) {}
    // CopyTexture2DToBuffer for a STREAM TARGET (QueueTextureUpload): moves the texture to COPY_SRC and
    // back with barriers the device's state book never sees, because QueueTextureUpload snapshots that
    // book on the game half while this records on the render half - nothing transitions a stream
    // target through the book. The stream / video upload probes read with it.
    virtual void CopyTexture2DToBufferInPlace(const RHI::Texture& src, const RHI::GPUBuffer& dst,
                                              RHI::CommandList cmd) {}

    // Frees all GPU resources of a buffer and invalidates the handle.
    virtual void DestroyBuffer(RHI::GPUBuffer& buffer) = 0;

    // Frees all GPU resources of a texture and invalidates the handle.
    virtual void DestroyTexture(RHI::Texture& texture) = 0;

    // Releases a shader's bytecode and invalidates the handle. The backend slot is retired, never
    // recycled (bare index handles carry no generation). ShaderLibrary::ClearCaches calls it on hot
    // reload.
    virtual void DestroyShader(RHI::Shader& shader) = 0;

    // Releases a pipeline state (deferred until the GPU is done with it) and invalidates the handle.
    // CreatePipelineState calls it on the handle it overwrites, on success, so a
    // `CreatePipelineState(desc, m_pso)` rebuild needs no explicit destroy; PSOCache::Clear / Shutdown
    // call it too.
    virtual void DestroyPipelineState(RHI::PipelineState& pso) = 0;

    // Releases a pipeline layout (deferred like DestroyPipelineState; a PSO built with it holds its own
    // reference) and invalidates the handle.
    virtual void DestroyPipelineLayout(RHI::PipelineLayout& layout) = 0;

    // ---- PSO library (ID3D12PipelineLibrary): disk-based ISA cache ----
    // InitPSOLibrary at startup, SavePSOLibrary at clean shutdown, CheckpointPSOLibrary between; all are
    // no-ops when the backend has no PipelineLibrary (e.g. feature level < 12_1).

    // Loads the PSO library cache from @p cacheFilePath. A missing file, corrupt data or a driver update
    // starts an empty library that recompiles ISA on first use.
    virtual bool InitPSOLibrary(const char* cacheFilePath) = 0;

    // Serializes all compiled PSOs to @p cacheFilePath for a fast warm-up on the next launch.
    virtual void SavePSOLibrary(const char* cacheFilePath) = 0;

    // SavePSOLibrary at a mid-run point (a finished scene load), only when pipelines were stored since
    // the last save, so a crash or a killed process keeps what this session built. Serializing costs
    // the whole library: call it where a frame may stall. A file another process holds is skipped
    // quietly; the next checkpoint or the shutdown save writes it.
    virtual void CheckpointPSOLibrary(const char* cacheFilePath) = 0;

    // ---- Boot precompile (Graphics/PsoManifest.h) ----
    // Builds every pipeline a recorded manifest names that the PSO library lacks, on @p threads worker
    // threads, and stores each in the library, so the game's first use of it is a library hit. Nothing
    // starts when the library is missing, the manifest is malformed, or @p indexPath shows this
    // library was prepared from the same manifest on this adapter and driver (the status says which).
    // The workers never touch the frame: call it any time after InitPSOLibrary.
    virtual bool StartPipelinePrecompile(const std::vector<uint8_t>& manifest, const std::string& indexPath,
                                         uint32_t threads) = 0;
    virtual RHI::PipelinePrecompileStatus GetPipelinePrecompileStatus() const = 0;
    // Joins the workers (@p cancel: the records not started are skipped), saves the library to
    // @p cacheFilePath and, when every record is in it, writes the index StartPipelinePrecompile
    // reads. A no-op when nothing was started.
    virtual void FinishPipelinePrecompile(bool cancel, const std::string& cacheFilePath) = 0;

    // ---- Synchronous GPU readback and .itex I/O ----
    // Back-buffer captures (RequestBackBufferCapture / TakeBackBufferShot, the pipe's capture
    // {"source": "app_ui"}) are on ISurface.

    // Captures an R8G8B8A8 texture to a PNG through a READBACK heap (the editor previews' and the
    // harness's captures). Synchronous (FlushAndWait inside), so the file matches the moment of the call.
    // Frame-boundary rule: only before the frame's pass recording starts (between frames, or a
    // mid-frame drain with just the primary list open); refused (false) after, since its one-shot list
    // would run before the frame's recorded lists. @p currentState is the texture's state, so the
    // backend can go through COPY_SOURCE and back without disturbing the renderer's state tracker.
    // False for a missing handle, an unsupported format or an I/O failure.
    virtual bool CaptureTextureToPNG(const RHI::Texture& tex,
                                     RHI::ResourceState  currentState,
                                     const char*         path) = 0;

    // The pipe's `capture {"attachment"}` (Harness/AttachmentDecode.h): ONE plane of mip 0 / array
    // slice @p arraySlice of a 2D texture read back exactly as stored - no conversion, no clamp - with
    // its rows packed tight. Plane 0 of a depth-stencil texture is its depth, plane 1 its stencil.
    // Synchronous, with CaptureTextureToPNG's frame-boundary rule and state round trip (@p currentState
    // on entry, restored after). False with @p out.error saying why.
    virtual bool ReadTextureRaw(const RHI::Texture& /*tex*/, RHI::ResourceState /*currentState*/,
                                std::uint32_t /*plane*/, RawTexture& out, std::uint32_t /*arraySlice*/ = 0)
    {
        out.error = "this device cannot read textures back";
        return false;
    }
    // Reads the MAIN view's HDR scene target (slot 0) as ReadTextureRaw reads a texture, from the
    // state the device tracks for it. False with @p out.error saying why.
    virtual bool ReadHdrSceneRaw(RawTexture& out)
    {
        out.error = "this device cannot read its HDR scene target back";
        return false;
    }
    // Reads @p size bytes of a GPU buffer at @p offset back into @p out, as ReadTextureRaw reads a
    // texture: a picture's metadata (the exposure the tonemap used). Synchronous, with the same
    // frame-boundary rule and @p currentState round trip. False with @p error saying why.
    virtual bool ReadBufferRaw(const RHI::GPUBuffer& /*buffer*/, RHI::ResourceState /*currentState*/,
                               std::uint64_t /*offset*/, std::uint64_t /*size*/,
                               std::vector<std::uint8_t>& /*out*/, std::string& error)
    {
        error = "this device cannot read buffers back";
        return false;
    }

    // Saves one cube of a TextureCubeArray to an .itex file / loads one back, so the reflection-probe
    // bake cache survives a world save. @p currentState is the texture's state on entry; the backend
    // goes through COPY_SOURCE / COPY_DEST and restores it.
    virtual bool SaveTextureCubeToITEX(const RHI::Texture& cubeArrayTex,
                                       uint32_t            cubeIndex,
                                       RHI::ResourceState  currentState,
                                       const char*         path) { return false; }
    virtual bool LoadITEXIntoTextureCube(RHI::Texture&      cubeArrayTex,
                                         uint32_t           cubeIndex,
                                         RHI::ResourceState currentState,
                                         const char*        path) { return false; }
};

// IGraphicsDevice::BeginLoaderUploads / EndLoaderUploads as a scope: End on every exit, a throw
// included (a scope left open would keep its loader context taken and nest this thread's next scope
// into it). Active(): Begin took one.
class LoaderUploadScope
{
public:
    explicit LoaderUploadScope(IGraphicsDevice& gfx) : m_gfx(gfx), m_on(gfx.BeginLoaderUploads()) {}
    ~LoaderUploadScope()
    {
        if (!m_on) return;
        try { m_gfx.EndLoaderUploads(); } catch (...) {}   // the context is released either way
    }
    bool Active() const { return m_on; }
    LoaderUploadScope(const LoaderUploadScope&)            = delete;
    LoaderUploadScope& operator=(const LoaderUploadScope&) = delete;
private:
    IGraphicsDevice& m_gfx;
    bool             m_on;
};

// This frame's constants as a T: *c.cpu = {...}, then bind c.gpu. Empty on overflow
// (IGraphicsDevice::AllocTransientConstants).
template <class T>
RHI::Constants<T> AllocConstants(IGraphicsDevice& gfx)
{
    static_assert(std::is_trivially_copyable_v<T>, "constants are copied into upload memory");
    const IGraphicsDevice::TransientConstants a = gfx.AllocTransientConstants(sizeof(T));
    return { static_cast<T*>(a.cpu), a.gpu };
}

template <class T>
RHI::Constants<T> RHI::CommandList::AllocConstants()
{
    return ::AllocConstants<T>(GetDevice());
}
