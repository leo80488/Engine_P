#pragma once
#include "System/ExecutionDomain.h"
#include "Graphics/IDeviceFrameControl.h"
#include "Graphics/ViewSlot.h"          // RHI::kMaxViews (the secondary views' HDR slots)
#include "System/EventTaskQueue.h"
namespace Sys { class OwnerTask; }

// GraphicsDX12: the Direct3D 12 implementation of IGraphicsDevice.
//
// Root signatures: RHI::Slots::Gfx / RHI::Slots::Cmp (Graphics/RootSignatureLayout.h) are the
// authoritative parameter lists, registers and DWORD costs; never spell a root slot as a literal.
// Graphics draws use PVF (programmable vertex fetch from the instance buffer, the mesh descriptors
// and bindless g_Buffers[]; no input assembler), and samplers come from the sampler heap (fixed
// slots plus the DynamicSampler root constant). Registers t16 and t23..t45 are retired - never
// reuse them: the resources still read travel as shader-visible heap indices (LightCB,
// DDGI::VolumeGPUDesc) opened through ResourceDescriptorHeap[], with NonUniformResourceIndex where
// the index varies per pixel. That makes Resource Binding Tier 3 + SM 6.6 a hard requirement:
// device init throws without them.
//
// Multi-threaded recording: BeginFrame() returns CommandList 0 (the primary); BeginCommandList(queue)
// takes a slot of m_commandLists[1..kMaxCommandLists-1]. Each CommandListDX12 lazily creates its own
// allocators (per frame x per queue) and lists. EndFrame() submits the frame's lists in dependency
// order, consecutive same-queue lists as one ExecuteCommandLists.
//
// ==== FRAME-BOUNDARY CONTRACT ====
// Read this before adding a device call to an ImGui handler, an editor button or a RenderPass.
// A D3D12 command list holds NO reference on the resources it names. A back buffer or render target
// destroyed after an OPEN list recorded a barrier or OMSetRenderTargets against it really dies, and
// the driver's residency list keeps pointing at it: an access violation inside the vendor DLL
// (nvwgf2umx) during whatever draws next in the SAME frame - usually ImGui, so it looks like an
// ImGui bug. A settings-panel button reaching SetFullscreen between BeginImGuiFrame and
// EndImGuiFrame is the classic route.
//
// "In a frame" == between BeginFrame() and EndFrame(); IsRecordingFrame() asks. Two predicates
// encode the two levels of safety the entry points need:
//   CanRebuildSwapChain()  true only OUTSIDE a frame. Required by anything that destroys back
//                          buffers or the swap chain.
//   CanHardSyncNow()       also true INSIDE a frame while no pass has started recording (primary
//                          list only, no parallel Phase B): admits the designed-in mid-frame
//                          sites, SetViewportSize and the EditorLayer::ProcessPendingActions drain.
//
// Frame-boundary-only entry points, and what each does when called too late:
//   Resize / SetFullscreen ....... the surface's (SurfaceDX12.h): LATCH into m_pendingSwapChainOp,
//                                  serviced by ServicePendingSwapChainOps(). Not refused: a dropped
//                                  window resize leaves the swap chain at the wrong size for good.
//   WaitForNextFrameSlot ......... REFUSED. Mid-frame it would latch m_frameSlotPrepared, so the
//                                  NEXT frame skips its slot fence wait (silent upload tearing).
//   BeginFrame / EndFrame ........ REFUSED on re-entry / on an unmatched End.
//   WaitIdleAndReleaseDeferred ... TRIPWIRE ONLY, still executes: world loading depends on this
//                                  drain to dodge a delayed bindless-SRV TDR. It drains ALL
//                                  deferred releases, so after pass recording began it frees
//                                  descriptors the open lists still reference.
//   SetViewportSize .............. TRIPWIRE ONLY, still executes (refusing would make the viewport
//                                  un-resizable); its size-compare early-out is a SAFETY guard.
//
// Every entry point that REBUILDS a resource has an idempotence guard ("value unchanged -> return").
// Those early-outs are correctness, not micro-optimisation: a rebuild requested with the state we
// are already in is fatal mid-frame. Do not delete one.
//
// A change wanted from inside a frame goes through a queue drained at a boundary:
// Window::RequestFullscreen + Window::ServicePendingFullscreen (top of App::Run) for fullscreen,
// EditorLayer::ProcessPendingActions for world-level teardown.
//
// DX12-specific accessors (GetDevice, GetNativeCommandList, ...) are non-virtual: a RenderPass that
// needs them does static_cast<GraphicsDX12&>(gfx).

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <stdexcept>
#include <atomic>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <condition_variable>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <wrl.h>
#include "d3d12.h"
#include "dxgi1_6.h"
#include "d3dcompiler.h"

#include "Graphics/IGraphicsDevice.h"
#include "Graphics/FrameConstants.h"
#include "Graphics/FrameSnapshot.h"         // the frame snapshot + barrier journal
#include "Graphics/RootSignatureLayout.h"
#include "Graphics/BindlessKind.h"            // bindless kinds, handles, the null slots
#include "Graphics/DescriptorHeapAllocator.h"
#include "Graphics/GPUProfiler.h"
#include "Graphics/TransientConstants.h"
#include "Graphics/PsoManifest.h"            // the pipelines recorded and replayed at boot
#include "Graphics/StreamUploadQueue.h"    // the stream uploader's rules
#include "Graphics/SurfaceDX12.h"          // the main surface (Surface() / MainSurface())
#include "Diagnostics/OwnerAccounting.h"   // park-hook fire counter
// SL::FrameGenState only - pulls in no Streamline SDK headers.
#include "Graphics/StreamlineBackend.h"
#include "System/Log.h"
#include "System/StableAppendPool.h"   // m_psoPool: lock-free readers, append under m_resourceMutex
#include "System/ThreadContext.h"   // -animoverlap: which HALF is asking for the game slot
#include "System/allocator.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#ifdef _DEBUG
#pragma comment(lib, "DirectXTex_Debug.lib")
#else
#pragma comment(lib, "DirectXTex_Release.lib")
#endif

// ==== Forward declarations ====
struct TextureDX12;   // a texture pool record (GraphicsDX12Internal.h)

// ==== ThrowIfFailed (GraphicsDX12 and RenderPass implementations) ====
void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line);

#define ThrowIfFailed(expr) \
    ThrowIfFailedImpl((expr), #expr, __FILE__, __LINE__)


// ==== CommandListDX12 ====
// One recording session's state. Instances live in GraphicsDX12::m_commandLists; callers hold the
// opaque RHI::CommandList handle (an index into that pool).
struct CommandListDX12
{
    using NativeCommandList = ID3D12GraphicsCommandList6;

    static constexpr uint32_t kBufferCount = RHI::kFramesInFlight;  // == GraphicsDX12::kFrameCount
    static constexpr uint32_t kQueueCount = 3;   // == RHI::QueueType::COUNT

    // ---- Barrier journal (Graphics/FrameSnapshot.h) ----
    struct JournalRecord
    {
        ID3D12Resource* resource      = nullptr;   // Aliasing: ResourceBefore
        ID3D12Resource* resourceAfter = nullptr;   // Aliasing only
        uint32_t        before        = 0;         // Transition: D3D12_RESOURCE_STATES
        uint32_t        after         = 0;
        uint32_t        subresource   = ~0u;       // ~0u = all subresources
        uint8_t         type          = 0;         // D3D12_RESOURCE_BARRIER_TYPE
        uint8_t         flags         = 0;         // D3D12_RESOURCE_BARRIER_FLAGS
        bool            native        = false;     // reported by NoteNativeBarriers
    };

    // ---- Resources to discard at the end of a render pass ----
    struct Discard
    {
        ID3D12Resource*      resource = nullptr;
        D3D12_DISCARD_REGION region   = {};
    };

    // ---- MSAA resolve targets ----
    static constexpr int kMaxRTs = D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT;

    // ---- Allocators, lists and pool identity ----
    // One allocator per [bufferIndex][queue type], created lazily.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> commandAllocators[kBufferCount][kQueueCount];
    // One command list per queue type, created lazily and reused across frames.
    Microsoft::WRL::ComPtr<ID3D12CommandList>      commandLists[kQueueCount];

    uint32_t        bufferIndex = 0;
    RHI::QueueType queue        = RHI::QueueType::GRAPHICS;
    uint32_t        id           = ~0u;

    // ---- Semaphore ordering ----
    // Pool IDs of the command lists this one must run after. Filled by AddCommandListDependency(),
    // consumed and cleared by EndFrame().
    std::vector<uint32_t> waitFor;
    // Fence value signalled on this CL's queue after its same-queue submit BATCH executes (batch
    // members share one value). Cross-queue dependents Wait() on it before their own submission.
    // Written by EndFrame(); not valid before.
    uint64_t signalFenceValue = 0;
    // When true, EndFrame closes the submit batch right AFTER this CL, so its fence value excludes
    // everything queued behind it. Set by SetSubmitBarrier(), cleared by BeginCommandList(). An
    // explicit opt-in costs one extra ExecuteCommandLists + Signal at the one place that unblocks the
    // compute queue; splitting after EVERY cross-queue dependency source costs ~6-7 extra pairs on a
    // ~8-submit frame and still leaves the producer late.
    bool     submitBarrier = false;

    // ---- Pipeline / draw-state tracking ----
    D3D_PRIMITIVE_TOPOLOGY     prevTopology                 = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    const RHI::PipelineState*  activePso              = nullptr;
    const ID3D12RootSignature* activeRootSigGraphics = nullptr;
    const ID3D12RootSignature* activeRootSigCompute  = nullptr;
    uint32_t                   prevStencilRef         = 0;
    bool                       dirtyPso               = false;
    // The depth tripwire's halves: what the bound DSV's pass declared it writes (RHI::DepthView, set
    // by every DSV bind) and what the bound pipeline writes (BindPipelineState). depthGuard is set only
    // while the bound view forbids a write or narrows the stencil bits, so every other draw pays one
    // branch.
    RHI::DepthView             depthView               = {};
    uint32_t                   depthTexId              = ~0u;
    bool                       depthGuard              = false;
    bool                       psoWritesDepth          = false;
    bool                       psoWritesStencil        = false;
    uint8_t                    psoStencilWriteMask     = 0;
    uint32_t                   psoId                   = ~0u;

    // ---- Debug-marker (PIX/RenderDoc) nesting depth ----
    // BeginEventMarker increments, EndEventMarker decrements (and drops an unmatched End at depth 0).
    // EndFrame closes leftover open markers before Close() so per-list - and so per-queue - begin/end
    // counts stay balanced, even across multi-list worker passes (GBuffer/Terrain).
    int                        markerDepth             = 0;

    // ---- Per-use command-list names ----
    // Bit q set = commandLists[q] carries a name SetCommandListName gave it for an earlier use.
    // BeginCommandList puts such a list back to "cmd<id>" before the new user names it, so a pooled
    // list never reports a previous owner.
    uint32_t                   namedListMask           = 0;
    // The name SetCommandListName gave this use, kept on the CPU whether or not
    // markers are on (one pointer store) so a frame snapshot can name every
    // list. A string literal or a pass's GetName(): it must outlive the frame.
    // nullptr = never named this use.
    const char*                name                    = nullptr;

    // ---- What this use recorded ----
    // Counted by the device's entry points on the recording thread (one recorder at a time, no
    // atomics); mutable so GetNativeCommandList (const) can count a native take. Reset, with the owner
    // (the recording thread's SetRecordingOwner), inside BeginCommandList's lock - GetOwnerCounters
    // scans the pool under it.
    mutable Diag::Prof::GpuCounters counters;
    uintptr_t                  owner                   = 0;

    // ---- Barrier journal (Graphics/FrameSnapshot.h) ----
    // Appended only on a journaled frame (GraphicsDX12::m_journalOn), by the thread recording this
    // list - one recorder at a time, so no lock. EndFrame folds it into the frame's
    // GfxDiag::FrameSnapshot; BeginCommandList empties it. The resource pointers are only identities:
    // EndFrame reads the debug names before anything this frame recorded can be released.
    std::vector<JournalRecord> journal;

    // ---- Pending resource barriers ----
    // UNUSED. Do not build a deferred PushBarrier on it: passes such as RTReflectionPass cache the raw
    // list and interleave native->Dispatch() with gfx.PushBarrier(), so a deferred barrier would land
    // after the dispatch it must precede. Batch explicitly with PushBarriers() instead.
    std::vector<D3D12_RESOURCE_BARRIER> frameBarriers;

    // ---- Render-pass begin / end barriers ----
    std::vector<D3D12_RESOURCE_BARRIER> renderPassBarriersBegin;
    std::vector<D3D12_RESOURCE_BARRIER> renderPassBarriersEnd;

    // ---- Resources to discard at the end of a render pass ----
    std::vector<Discard> discards;

    // ---- MSAA resolve targets ----
    ID3D12Resource* resolveSrc[kMaxRTs]     = {};
    ID3D12Resource* resolveDst[kMaxRTs]     = {};
    DXGI_FORMAT     resolveFormats[kMaxRTs] = {};
    std::vector<D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS>
        resolveSubresources[kMaxRTs];

    // ---- Helpers ----
    bool IsValid() const noexcept { return id != ~0u; }

    NativeCommandList* GetCommandList() const noexcept
    {
        return static_cast<NativeCommandList*>(
            commandLists[static_cast<UINT>(queue)].Get());
    }
};


// The device: frames, command lists, fences, resources, the HDR scene target. Presentation is its
// main surface's (Graphics/SurfaceDX12.h): callers holding an IGraphicsDevice reach it as an
// ISurface through MainSurface(), DX12 code through Surface().
class GraphicsDX12 : public IGraphicsDevice, public IDeviceFrameControl
{
public:
    // Swap-chain / frame-pipelining depth. Aliased from the shared engine
    // constant so the ~30 per-frame rings that size their member arrays
    // from it cannot drift away from the backend.
    static constexpr UINT kFrameCount  = RHI::kFramesInFlight;
    // Sampler descriptor ceiling: the D3D12 hard cap on a SHADER-VISIBLE sampler heap, not a tuning
    // knob. The pool is APPEND-ONLY (there is no DestroySampler), so this is a monotonic budget; with
    // kSamplerDedupe the ~50 CreateSampler calls at boot collapse to ~13 distinct descs.
    // Overflow does NOT degrade to an unbound sampler: CreateSampler returns index -1 and
    // SetDynamicSampler then leaves the previous draw's sampler in the root constant (a
    // non-comparison sampler can land in a SampleCmp slot).
    // m_samplerAllocator.Init takes this value as both its CPU staging and its GPU static count:
    // AllocateStatic reserves a staging slot before a GPU slot, so the smaller of the two is the
    // real ceiling.
    static constexpr UINT kMaxSamplers = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;  // 2048

    // Sampler dedupe kill switch: false restores one heap slot and one pool index per CreateSampler
    // call, in call order; the hard cap and the reserve stay either way.
    // Dedupe is safe only because the pool is APPEND-ONLY, nothing mutates a sampler after creation,
    // and SetDynamicSampler (through SamplerHeapIndex) is the only consumer of the index - no caller
    // uses it as an identity. A COMPARISON sampler must never merge into a non-comparison one, so the
    // key is the FULL desc (filter and comparisonFunc included), compared field by field; the hash
    // only picks the bucket. Never memcmp RHI::SamplerDesc: it has padding.
    static constexpr bool kSamplerDedupe = true;

    using DescriptorHeapStats = RHI::DescriptorHeapStats;

    // Every HDR scene target's format: CreateHdrTarget's resource and views, and
    // the answer to GetHdrSceneColorFormat.
    static constexpr RHI::Format kHdrSceneColorFormat = RHI::Format::R16G16B16A16_FLOAT;

    // ---- HDR scene target slots ----
    // One HdrTargetBinding per slot. PushHdrTarget makes one CURRENT for the duration of a view's graph,
    // so every pass bind site and every GetRenderWidth() / GetRenderHeight() reader resolves through
    // Hdr(), whichever binding is current. Slot 0 is the main viewport.
    // ONE STATE BOOK: the slot's texture is a record in m_texturePool (resource, RTV / SRV / UAV for the
    // SSR composite, state) and the binding keeps only its handle, so the HDR helpers, the graph's
    // barriers (TranslateBarrier) and DeviceStateAgrees read and write the same state. The record is made
    // over the views CreateHdrTarget allocates anyway, keeps its id across a rebuild (a resize) and
    // gives it back in DestroyHdrTarget. Its bindless-table entry is never written: nothing samples the
    // HDR target as a material texture. HdrRecord(slot) is the record (null when empty).
    struct HdrTargetBinding
    {
        RHI::Texture                           tex;   // m_texturePool[tex.handleId]
        uint32_t                               w{ 0 };
        uint32_t                               h{ 0 };
        // Clear colour (SetHdrClearColor, per frame). The default {0,0,0,0} matches the RT's optimized
        // clear value; a non-black WorldEnvironment FlatColor triggers the benign D3D12
        // CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE perf warning.
        float                                  clear[4]{ 0.f, 0.f, 0.f, 0.f };
    };
    // SLOT BUDGET, every slot spoken for:
    //   slot 0      the main viewport, for the life of the process;
    //   slots 1..4  live preview panels (Preview::kMaxPreviews);
    //   slot 5      the asset browser's THUMBNAIL lane, which owns a target for the whole session;
    //   slots 6..8  the renderer's secondary views (RHI::kMaxViews - 1), kFirstViewHdrSlot on.
    // Too few slots shows as a tile that never renders (ClaimHdrSlot answers 0) behind one
    // ENGINE_CHECK_ONCE. An unclaimed slot costs sizeof(HdrTargetBinding) of member storage and no
    // device memory: nothing is created until CreateHdrTarget.
    static constexpr uint32_t kMaxHdrTargets    = 9;
    static constexpr uint32_t kFirstViewHdrSlot = 6;
    static_assert(kFirstViewHdrSlot + RHI::kMaxViews - 1 <= kMaxHdrTargets,
                  "every secondary view has an HDR slot of its own");

    // ---- Descriptor-table and pool sizes ----
    // Slots in the custom-material texture table; the root signature's range is built from it. Must
    // equal Resource::kMaxCustomTextures so reflection-driven binding never overruns the table.
    static constexpr uint32_t kCustomMatTextureSlots = 8;

    // Bindless texture table size, public so passes with their own root signatures (DDGIPass, ...)
    // size their bindless ranges to match. PBR materials carry 5-7 textures, so 200 unique materials
    // are ~1400 textures; with IBL probes, atlases, LUTs and UI sprites a mid-size game reaches ~6-10k
    // resident, and 16k leaves headroom without aggressive streaming eviction. Must match the sized
    // g_AllTextures[16384] declarations of the ray-tracing shaders (DDGIRayTrace, RTAO, PathTrace, ...);
    // the others use unbounded [].
    static constexpr uint32_t kMaxBindlessTextures = 16384;
    static_assert(kMaxBindlessTextures == RHI::Slots::MaxBindlessTextures,
                  "kMaxBindlessTextures must match RHI::Slots::MaxBindlessTextures "
                  "(RootSignatureLayout.h) — the RT/DDGI private root signatures "
                  "size their g_AllTextures[] range from the shared value");

    // Hard pool caps (see the m_bufferPool comment). The texture cap equals
    // the bindless table: a texture past it would already have no bindless
    // SRV slot, so refusing it loses nothing. The reserve is virtual until
    // touched (~11 MB committed for all three at full size). Shutdown logs
    // the high-water marks for re-sizing.
    static constexpr uint32_t kMaxTexturePool = kMaxBindlessTextures;   // 16384
    static constexpr uint32_t kMaxBufferPool  = 16384;
    static constexpr uint32_t kMaxShaderPool  = 4096;   // append-only; hot reload retires slots

    // ---- Construction and the main surface ----
    ISurface&          MainSurface() override { return *m_surface; }
    SurfaceDX12&       Surface() noexcept       { return *m_surface; }
    const SurfaceDX12& Surface() const noexcept { return *m_surface; }

    // The device alone - adapter, queues, heaps, Streamline - with no window. The swap chain is the
    // MAIN SURFACE, put on a window by CreateMainSurface and taken off it by ReleaseMainSurface; App
    // owns both steps (its MainSurface member), between the device and every GPU-resource owner.
    GraphicsDX12();
    // Device + main surface in one go.
    GraphicsDX12(HWND hWnd, UINT width, UINT height) : GraphicsDX12() { CreateMainSurface(hWnd, width, height); }
    ~GraphicsDX12() override;

    // The swap chain on @p hWnd at a @p width x @p height client, its back
    // buffers, the HDR scene target sized to it and its display. Once.
    // (SurfaceDX12::Create / Release; kept here because App owns the order.)
    void CreateMainSurface(HWND hWnd, UINT width, UINT height) { m_surface->Create(hWnd, width, height); }
    // Frame Generation off, the GPU idle, Streamline shut down (its proxy lives
    // on this swap chain), then the swap chain - while its window is alive.
    // Idempotent; the destructor covers a device whose surface was never
    // released.
    void ReleaseMainSurface() noexcept { if (m_surface) m_surface->Release(); }
    bool HasMainSurface() const noexcept { return m_surface && m_surface->HasSwapChain(); }

    // ---- IGraphicsDevice: frame lifecycle ----
    void             WaitForNextFrameSlot() override;
    RHI::CommandList BeginFrame() override;
    void             EndFrame()   override;
    void             FlushAndWait() override;
    // Rebuilds the present composite's shaders (the main surface's).
    void             ReloadDeviceShaders() override { if (m_surface) (void)m_surface->ReloadPresentShaders(); }
    // Out of line so its frame-boundary tripwire reports once per session, not once per translation
    // unit: ENGINE_CHECK_ONCE's flag is per expansion.
    void             WaitIdleAndReleaseDeferred() override;
    bool             CaptureTextureToPNG(const RHI::Texture& tex,
                                         RHI::ResourceState  currentState,
                                         const char*         path) override;
    // The pipe's capture {"attachment"}: one plane read back as stored.
    bool             ReadTextureRaw(const RHI::Texture& tex, RHI::ResourceState currentState,
                                    std::uint32_t plane, RawTexture& out,
                                    std::uint32_t arraySlice = 0) override;
    bool             ReadHdrSceneRaw(RawTexture& out) override;
    bool             ReadBufferRaw(const RHI::GPUBuffer& buffer, RHI::ResourceState currentState,
                                   std::uint64_t offset, std::uint64_t size,
                                   std::vector<std::uint8_t>& out, std::string& error) override;

    // ---- IGraphicsDevice: command list management ----
    RHI::CommandList BeginCommandList(
        RHI::QueueType queue = RHI::QueueType::GRAPHICS) override;

    // Declare that @p waiter must execute after @p dependency on the GPU.
    // Same-queue: enforced by submission order.
    // Cross-queue: a GPU fence Wait/Signal is inserted in EndFrame.
    void AddCommandListDependency(RHI::CommandList waiter,
                                  RHI::CommandList dependency) override;

    // ---- Frame-boundary state (the FRAME-BOUNDARY CONTRACT at the top of this file) ----
    // True between BeginFrame() and EndFrame(). Callers that can run from both
    // an ImGui handler and the main loop can branch on this to decide whether
    // to act now or enqueue. Overrides the IGraphicsDevice hook of the same
    // signature so callers holding only the interface can ask too.
    bool IsRecordingFrame() const noexcept override
    { return m_inFrame.load(std::memory_order_acquire); }

    // Command-list pool telemetry for the editor's pool watermark panel; main thread, read-only.
    // GetCommandListPeak is the most lists used in ONE frame; GetCommandListAllocated is how many pool
    // slots were ever constructed (monotonic). They are not interchangeable.
    uint32_t GetCommandListCurrent()   const noexcept
        { return m_commandListCount.load(std::memory_order_relaxed); }
    uint32_t GetCommandListPeak()      const noexcept override { return m_commandListPeak; }
    uint32_t GetCommandListAllocated() const noexcept { return m_commandListHighWater; }
    static constexpr uint32_t GetCommandListCapacity() { return kMaxCommandLists; }

    // Descriptor-heap telemetry (RHI::DescriptorHeapStats), one per managed heap. Pure observation:
    // nothing allocates, and nobody pays unless a caller asks (Lua Debug.Stats(), the pool panel).
    // Read DescriptorHeapAllocator.h before asserting on these: 'used' is a MONOTONIC high-water
    // cursor (proves boundedness, never recycling); 'freeBlocks' is the recycling signal and the
    // linear-scan cost of the uncoalesced free list; 'capacity' is the EFFECTIVE ceiling
    // min(cpu staging, gpu static) as the driver GRANTED it - CreateHeapWithFallback may have halved
    // the request.
    DescriptorHeapStats GetCbvSrvUavHeapStats() const override
    { return MakeHeapStats(m_cbvSrvUavAllocator); }
    DescriptorHeapStats GetRtvHeapStats() const override
    { return MakeHeapStats(m_rtvAllocator); }
    DescriptorHeapStats GetDsvHeapStats() const override
    { return MakeHeapStats(m_dsvAllocator); }
    DescriptorHeapStats GetSamplerHeapStats() const override
    { return MakeHeapStats(m_samplerAllocator); }
    void GetCbvSrvUavStaticUsage(uint32_t& cpuUsed, uint32_t& cpuCap,
                                 uint32_t& gpuUsed, uint32_t& gpuCap) const override
    { m_cbvSrvUavAllocator.GetStaticUsage(cpuUsed, cpuCap, gpuUsed, gpuCap); }

    // Level 1: may we destroy/rebuild back buffers right now? Only outside a frame: an open command
    // list references the back buffer.
    bool CanRebuildSwapChain() const noexcept
    { return !m_inFrame.load(std::memory_order_acquire); }

    // Level 2: may we stall every queue and drain deferred releases right now?
    // Legal outside a frame, and also inside one as long as nothing has started
    // recording yet (primary CL only, no parallel Phase B). That admits the two
    // designed-in mid-frame sites (App.cpp's SetViewportSize, and the
    // ProcessPendingActions drain) while still catching a pass Execute(), an
    // ImGui handler, or a worker thread.
    bool CanHardSyncNow() const noexcept
    {
        return !m_inFrame
            || (m_commandListCount.load(std::memory_order_relaxed) <= 1
                && !m_parallelPassRecording.load(std::memory_order_acquire));
    }

    // Increment m_fenceValue and Signal the graphics queue as one step. Returns
    // the value signalled. Does NOT wait - callers that need to wait do so after
    // this returns, so the lock is never held across a GPU wait.
    uint64_t SignalGraphicsFence();

    // The IDeviceFrameControl half (FramePipeline's). Hard-sync operations from outside an executing
    // device segment drain the bounded frame transaction; inside a segment the token already owns it.
    IDeviceFrameControl* FrameControl() override { return this; }
    void SetFrameDrainHook(std::function<void()> hook) override { m_parkHook = std::move(hook); }
    void DrainCpuFrame(uint8_t cause = 0) override
    { DrainFrameIfNeeded(static_cast<Diag::DrainCause>(cause)); }
    void DrainFrameIfNeeded(Diag::DrainCause cause = Diag::DrainCause::Other)
    {
        if (m_parkHook && !m_deviceExecution.Owns())
        {
            // The hook runs FramePipeline::DrainFrameWork on main; it charges the drain to this.
            Diag::OwnerAccounting::Get().pendingHookCause.store(
                static_cast<uint8_t>(cause), std::memory_order_relaxed);
            // Counted, not just executed: every fire collapses that frame's game/render overlap to zero, and
            // resource paths on any thread can reach it, so the counter is atomic.
            Diag::OwnerAccounting::Get().parkHookFires.fetch_add(1, std::memory_order_relaxed);
            m_parkHook();
        }
    }

    void SetDeviceFrameThread(std::thread::id id) noexcept override
    { m_deviceFrameThread.store(id, std::memory_order_release); }
    void EnableLogicalFrameCoordinator() noexcept override { m_logicalFrameCoordinator = true; }
    bool OwnsDeviceFrame() const noexcept
    { return m_deviceExecution.Enabled() ? m_deviceExecution.Owns() :
        std::this_thread::get_id() == m_deviceFrameThread.load(std::memory_order_acquire); }
    Sys::ExecutionDomain::Token IssueDeviceExecution(uint64_t frame) override
    {
        if (std::this_thread::get_id() != m_deviceFrameThread.load(std::memory_order_acquire))
            throw std::logic_error("Only the frame coordinator may issue device execution");
        // The token names the RENDER slot, not the game's: under -animoverlap the game half claims F+1
        // while the lane is between IssueDeviceExecution(F) and BeginFrame(F), so GetUploadSlot() would
        // stamp the token one slot ahead and BeginFrame's m_deviceExecution.Matches would fail. Derived
        // exactly as BeginFrame derives m_uploadSlotRender (the same -slotlag subtraction and modulo), so
        // the two cannot drift. Under the dxgi kill switch the slot is the back-buffer index, not a function
        // of the frame counter, so it is read from GetUploadSlot().
        const uint32_t slot = m_slotKeyDxgi
            ? GetUploadSlot()
            : static_cast<uint32_t>(
                  ((frame >= m_slotLag) ? (frame - m_slotLag) : frame) % kFrameCount);
        return m_deviceExecution.IssueFrame(frame, slot);
    }
    void RetireDeviceExecution(const Sys::ExecutionDomain::Token& token) override { m_deviceExecution.Retire(token); }

    void SetRenderFrame(uint64_t frame) noexcept override
    {
        m_renderFrameReq.store(frame, std::memory_order_release);
        m_renderFrameReqSet.store(true, std::memory_order_release);
    }

    uint64_t MaxObservedFrameLag() const noexcept override { return m_maxObservedFrameLag; }

    // ---- IGraphicsDevice: the viewport and the HDR scene target ----
    void SetViewportSize(uint32_t width, uint32_t height) override;
    void SetRenderTargetToHdr(const float clearColor[4],
                              RHI::CommandList cmd) override;
    void SetHdrClearColor(const float clearColor[4])   override;
    void SetRenderTargetToHdrWithDepth(const RHI::Texture*   dsv,
                                       RHI::CommandList      cmd,
                                       const RHI::DepthView& view = {}) override;

    RHI::Format GetHdrSceneColorFormat() const override { return kHdrSceneColorFormat; }

    // ---- HDR scene target slots ----
    // Builds (or rebuilds) slot @p slot's target, first destroying what the slot held. The release is
    // deferred, which is why a mid-frame call is safe. A rebuild keeps the slot's pool record (and so its
    // handle, GetHdrTexture).
    void CreateHdrTarget (uint32_t slot, uint32_t width, uint32_t height) override;
    // Releases the slot's resource and views (deferred) and gives its pool record back, like
    // DestroyTexture.
    void DestroyHdrTarget(uint32_t slot) override;
    // The slot's texture as a pool handle: what the graph imports, barriers and checks with
    // DeviceStateAgrees. Invalid when the slot has no target. Never DestroyTexture it: the slot owns it.
    RHI::Texture GetHdrTexture(uint32_t slot) const override;
    // Idempotent resize, plus the "not after pass recording started" tripwire.
    // SetViewportSize(w, h) IS ResizeHdrTarget(0, w, h).
    void ResizeHdrTarget (uint32_t slot, uint32_t width, uint32_t height);

    // Makes @p slot the current HDR binding until the matching PopHdrTarget. Every HDR bind / accessor
    // on the device then resolves to that slot with no further code change - the point AND the hazard:
    // anything touching the HDR target inside the scope silently re-routes, with no compile error and no
    // debug-layer message.
    // Two hard rules, both checked:
    // - NEVER inside a RenderGraph Phase B window: handing over the state tracker while passes record in
    //   parallel swaps it under barriers already recorded. RenderGraph goes parallel at passCount >= 4
    //   whatever the render mode, so a preview graph's own Phase B counts too: Push/Pop wrap the preview
    //   Execute() from the OUTSIDE.
    // - The external HDR consumers must run at depth 0; each carries
    //   ENGINE_CHECK_ONCE(HdrScopeDepth() == 0, ...).
    // Returns false when the push did not take, and the caller MUST honour that: a refused push leaves
    // slot 0 (the main viewport) current, so carrying on would clear the live scene mid-frame and hand
    // slot 0's state tracker back in whatever state it was left.
    [[nodiscard]] bool PushHdrTarget(uint32_t slot) override;
    void PopHdrTarget() override;
    // 0 outside every push scope.
    int  HdrScopeDepth() const noexcept override
    { return m_hdrScopeDepth.load(std::memory_order_relaxed); }

    // ---- Back-buffer and render sizes ----
    // The back buffer's size: the main surface's client (0 x 0 without one).
    uint32_t GetWidth()  const override { return m_surface ? m_surface->Width()  : 0u; }
    uint32_t GetHeight() const override { return m_surface ? m_surface->Height() : 0u; }
    uint32_t GetRenderWidth()  const override { return Hdr().w ? Hdr().w : GetWidth();  }
    uint32_t GetRenderHeight() const override { return Hdr().h ? Hdr().h : GetHeight(); }
    // ---- Upload-ring slots (the game/render split) ----
    // The per-frame UPLOAD-RING slot of the frame being rendered: renderFrame % kFrameCount (see
    // m_uploadSlotRender), not the back-buffer index. Every FrameCB / staging ring / per-slot allocator
    // on the render side wants this one.
    uint32_t GetFrameIndex()   const override
    {
        // Audit (DX12_SLOTAUDIT=1): code running before BeginFrame is the GAME side and must call
        // GetUploadSlot(). This value is only published when the render side starts the frame, so a
        // game-phase caller writes its ring one slot behind where the frame reads it - silent aliasing
        // (intermittent skinned-mesh flicker) the debug layer cannot see. Plain member loads, no
        // function-local static: that would put a thread-safe-init guard on this hot accessor.
        if (m_slotAudit && !m_inFrame)
            AuditSlotCaller(_ReturnAddress());
        return m_uploadSlotRender.load(std::memory_order_relaxed);
    }

    // The per-frame UPLOAD-RING slot the GAME side claimed for the frame it is simulating
    // (AcquireUploadSlot). Every ring written before BeginFrame - the mesh-descriptor triple buffer,
    // skinning staging, VFX vertex rings - wants this one. Equal to GetFrameIndex() while the loop is
    // serial, one slot ahead once the render half runs a frame behind: the point of triple-buffering
    // those rings.
    uint32_t GetUploadSlot()   const override
    {
        // Audit (-animoverlapverify), the mirror of GetFrameIndex()'s: this value belongs to the GAME half,
        // and the render half must ask GetFrameIndex(). With -animoverlap the Animation barrier moves past
        // the acquire, so a render-half caller reads F % kFrameCount while the lane is still on F-1: one
        // slot ahead, aliasing another frame's ring invisibly. Only the accessor itself can show that no
        // render-half caller asked.
        if (m_gameSlotAudit && Sys::ThreadCtx::Current() == Sys::ThreadCtx::Render)
            AuditGameSlotCaller(_ReturnAddress());
        return m_uploadSlotGame.load(std::memory_order_relaxed);
    }
    bool     IsSlotKeyedByFrame() const override { return !m_slotKeyDxgi; }
    bool     IsUploadSlotWritable() const override
    { return m_uploadSlotOpenFrame.load(std::memory_order_acquire) != UINT64_MAX; }

    // The public face of the deferred-release clock (see IGraphicsDevice).
    // ReleaseStampFrame() itself stays private: it is the stamp THIS class
    // writes into m_pendingReleases, and a subsystem outside it has no business
    // reaching for that name. The retired value is EXACTLY the predicate
    // BeginFrame drains with, expressed once so the two cannot drift.
    uint64_t GetReleaseStampFrame() const override { return ReleaseStampFrame(); }
    uint64_t GetRetiredFrame() const override
    {
        const uint64_t r = m_renderFrame.load(std::memory_order_relaxed);
        const uint64_t byCount = (r >= kFrameCount) ? (r - kFrameCount) : 0ull;
        // ...or the hard-sync floor. `r - kFrameCount` is a proxy for "the GPU has finished it": right
        // while frames keep coming, wrong forever once they stop (shutdown, Resize). After
        // WaitForPreviousFrame everything submitted is retired, as ProcessAllDeferredReleases (UINT64_MAX)
        // already assumes. The floor is FRAME-AWARE: a sync inside a frame raises it only to m_renderFrame,
        // because a release taken later in that frame carries m_renderFrame + 1 and may name a resource the
        // open list still references, which the query-based immediate-free consumers (PreviewAssetSource's
        // mesh-lib handbacks, LightingPass's quiesced predicate) would free in place. Monotonic;
        // WaitForPreviousFrame holds the full argument and the ENGINE_CHECK that enforces it.
        const uint64_t floorF = m_hardSyncedFrame.load(std::memory_order_relaxed);
        return (byCount > floorF) ? byCount : floorF;
    }

    // Audit-mode reporter for GetFrameIndex() called outside a frame. Out of
    // line and never inlined into the accessor's hot path.
    void AuditSlotCaller(const void* ret) const;

    // The same reporter for the other direction: GetUploadSlot() asked from a
    // thread inside Sys::ThreadCtx::Render. Counted as well as logged, because
    // a pack has to bound "zero of them happened" and the log de-duplicates by
    // caller.
    void AuditGameSlotCaller(const void* ret) const;

    // -animoverlapverify: arm the reporter above. Separate from m_slotAudit
    // (DX12_SLOTAUDIT), which audits the opposite direction.
    void SetGameSlotAudit(bool on) override { m_gameSlotAudit = on; }

    // -animoverlap: permit AcquireUploadSlotAsync while a frame is open on the render-owner lane. The
    // block comment on that function makes the invariant-by-invariant argument; a run that has not
    // opted in keeps the hard refusal.
    void SetConcurrentUploadAcquire(bool on) override { m_concurrentUploadAcquire = on; }
    uint32_t GameSlotReadsFromRender() const override
    { return m_gameSlotFromRender.load(std::memory_order_relaxed); }

    // Game-thread slot acquisition, once per frame before the first ring write (App::Run, ahead of the
    // Animation phase): waits the retirement fences of the slot frame @p frame is about to write,
    // without touching swap-chain state.
    void AcquireUploadSlot(uint64_t frame) override;
    Sys::OwnerTask AcquireUploadSlotAsync(uint64_t frame) override;
    // Call while the slot snapshot is stable, at the same owner phase as
    // WaitSlotRetired. Consumers may co_await; GPU events publish completion without owner polling.
    Sys::TaskHandle RequestSlotRetirement(uint32_t slot, bool* eventRegistered = nullptr);
    void PollGpuCompletions() { m_gpuCompletions.Collect(); }
    void CloseGpuCompletions() override { m_gpuCompletions.Close(); }
    uint32_t GetFrameCount()   const override { return kFrameCount; }
    // ---- Capabilities, descriptors and HDR scene access ----
    uint32_t GetConstantBufferAlignment() const override
    { return D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT; }

    RHI::DeviceCapabilities GetCapabilities() const override
    {
        RHI::DeviceCapabilities c;
        c.meshShader                = m_meshShaderSupported;
        c.raytracing                = m_dxrSupported;
        c.inlineRaytracing          = m_inlineRtSupported;
        c.unboundedDescriptorArrays = m_resourceBindingTier3;
        c.native16BitShaderOps      = m_native16BitSupported;
        c.int64HeapAtomics          = m_int64HeapAtomics;
        return c;
    }
    // The DXGI adapter description.
    RHI::AdapterInfo GetAdapterInfo() const override;
    std::vector<RHI::AdapterInfo> ListAdapters() const override;

    uint64_t GetHdrSceneSrvGpuHandle() const override;
    uint64_t GetHdrSceneUavGpuHandle() const override;
    // SRV of a NAMED slot rather than the current one. The two accessors above answer for whatever
    // PushHdrTarget last made current: right for pass bind sites, wrong for a consumer that has already
    // POPPED (the preview layer's second ToneMapPass reads its slot after the scope closes). 0 when the
    // slot is empty.
    uint64_t GetHdrTargetSrvGpuHandle(uint32_t slot) const override;
    // The raw HDR scene resource, for FidelityFXBackend / FSR2Pass (the FFX API takes
    // ID3D12Resource*, not SRV handles). Null if not created.
    ID3D12Resource* GetHdrSceneResource() const;

    // Re-binds the engine's shader-visible descriptor heaps (CBV/SRV/UAV + Sampler) on @p cmd, as
    // BeginCommandList does. Call after any third-party DLL (FFX, ...) that may have swapped heaps via
    // SetDescriptorHeaps.
    void RebindShaderVisibleHeaps(RHI::CommandList cmd);
    // After a native compute pass or a pipeline with its own layout, restore the
    // engine slot layout (IGraphicsDevice). Root values are invalidated; the
    // following pass must bind the arguments it consumes.
    void RestoreDefaultComputeRootSignature(RHI::CommandList cmd) override;
    bool GetHdrSceneSize(uint32_t& width, uint32_t& height) const override;
    void     CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd) override;
    void     CopyHdrSceneToScoped(const RHI::Texture& dst, RHI::CommandList cmd) override;
    uint64_t GetTextureSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t GetTextureSRVCpuHandle(const RHI::Texture& texture) const override;
    // DX12-native counterpart, for staging descriptors into a ring (e.g. the custom-material texture
    // table) through the raw D3D12 handle. A zero handle when the texture is unloaded.
    D3D12_CPU_DESCRIPTOR_HANDLE GetTextureSRVCpuHandleNative(const RHI::Texture& texture) const;
    uint64_t GetBindlessTextureTableGpuHandle() const override { return m_bindlessTexTable.GetGpuHandle().ptr; }
    void     CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                      uint64_t srcCpuHandle,
                                      uint32_t count) override;
    uint32_t GetCbvSrvUavDescriptorIncrement() const override;
    // Returns a UNORM alias SRV for SRGB textures (raw sRGB data, no linearization).
    // For editor texture previews rendered to a UNORM RTV.
    uint64_t GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const override;
    uint64_t GetTextureAllocationBytes(const RHI::Texture& texture) const override;
    // ---- Shader pool usage ----
    // The shader pool's slots in use / its cap: a ratchet (slots are retired, never recycled). The
    // Material Editor's live graph compile reports what an edit costs against it.
    std::size_t ShaderPoolUsed() const;
    std::size_t ShaderPoolCapacity() const;
    // ---- Capabilities, descriptors and HDR scene access ----
    // Stencil-plane SRV for stencil-bearing depth textures (D32_FLOAT_S8X24 /
    // D24_UNORM_S8). Created lazily in CreateTexture alongside the depth-plane
    // SRV. Returns 0 if the texture is not stencil-bearing. Sampled as
    // Texture2D<uint2>; read .y for stencil.
    uint64_t GetTextureStencilSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t GetTextureUVPlaneSRVGpuHandle(const RHI::Texture& texture) const override;
    uint64_t CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                        uint32_t            count) override;
    void     FreeDescriptorTable(uint64_t gpuHandle) override;

    // ---- IGraphicsDevice: resource creation ----
    bool CreateBuffer(
        const RHI::GPUBufferDesc& desc,
        RHI::GPUBuffer&           outBuffer,
        const void*               initialData = nullptr) override;

    void BeginBufferUploadBatch() override;
    void EndBufferUploadBatch()   override;

    // Within this scope, DEFAULT-heap CreateBuffer() suballocates as a placed
    // resource from a shared heap instead of one CreateCommittedResource (= one
    // implicit heap allocation) per buffer. Used around bulk skinned-mesh buffer
    // creation (a character = ~210 small buffers) to collapse the per-buffer heap
    // allocations to a couple of CreateHeap calls. Nestable counter, like the
    // upload batch. UPLOAD/READBACK buffers always stay committed.
    void BeginBufferHeapBatch(uint64_t heapBytesHint = 0) override;
    void EndBufferHeapBatch()   override;

    // A loader worker's upload scope (IGraphicsDevice; the contexts are m_loaderCtx below).
    bool BeginLoaderUploads() override;
    void EndLoaderUploads()   override;
    bool LoaderUploadsEnabled() const override { return m_loaderUploads; }

    bool CreateTexture(
        const RHI::TextureDesc&     desc,
        RHI::Texture&               outTexture,
        const RHI::SubresourceData* initialData = nullptr) override;
    bool CreateTextureInArena(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                              uint64_t offset, RHI::Texture& outTexture) override;
    void DiscardTexture(const RHI::Texture& texture, RHI::CommandList cmd) override;

    // DX12-only: creates a TRANSIENT texture with CreatePlacedResource at (heap, heapOffset) instead of
    // its own committed heap, so lifetime-disjoint textures can alias the same heap bytes. 2D and 3D,
    // optional mip chain (per-mip UAVs as on the committed path), SHADER_RESOURCE (+ optional
    // UNORDERED_ACCESS) only: NO initialData / RT / DS / cube / array / MSAA / depth / NV12.
    // Otherwise identical to a committed texture (same SRV/UAV descriptors, same bindless slot). The
    // texture holds a ref on the heap (TextureDX12::placedHeap) that DestroyTexture defers with the
    // resource, so callers need no retirement ring and may drop every other heap reference at any time.
    bool CreateTexturePlaced(
        const RHI::TextureDesc&     desc,
        ID3D12Heap*                 heap,
        UINT64                      heapOffset,
        RHI::Texture&               outTexture);
    // CreateTexture's body: committed, or placed at @p placeOffset in @p placeHeap (CreateTextureInArena).
    bool CreateTextureImpl(const RHI::TextureDesc& desc, RHI::Texture& outTexture,
                           const RHI::SubresourceData* initialData, ID3D12Heap* placeHeap, uint64_t placeOffset);

    // Size/alignment a CreateTexturePlaced call would need for `desc`, from
    // ID3D12Device::GetResourceAllocationInfo on the exact same resource desc.
    // Returns false for shapes the placed path does not support.
    bool GetTextureAllocationInfo(
        const RHI::TextureDesc&     desc,
        uint64_t&                   outSize,
        uint64_t&                   outAlign) override;
    bool GetTextureFootprint(const RHI::TextureDesc& desc, uint64_t& outSize, uint64_t& outAlign) override;
    // The arena through IGraphicsDevice: its heap grown by this device, a texture placed in its
    // current heap (the native overload above).
    bool EnsureAliasArena(TransientAliasArena& arena, uint64_t bytes) override;
    bool CreateTexturePlaced(const RHI::TextureDesc& desc, const TransientAliasArena& arena,
                             uint64_t offset, RHI::Texture& outTexture) override;
    // UAV clears (IGraphicsDevice).
    void ClearUavUint(const RHI::GPUBuffer& buffer, const uint32_t values[4], RHI::CommandList cmd) override;
    void ClearUavUint(const RHI::Texture& texture, const uint32_t values[4], RHI::CommandList cmd) override;
    void ClearUavFloat(const RHI::Texture& texture, const float values[4], RHI::CommandList cmd) override;

    bool UpdateTexture(
        RHI::Texture&               texture,
        const RHI::SubresourceData* planes,
        uint32_t                    subresourceCount) override;

    // Streaming texture upload (GraphicsDX12_StreamUpload.cpp; the rules are
    // Graphics/StreamUploadQueue.h).
    StreamUpload::Result QueueTextureUpload(const RHI::Texture& dst, const StreamUpload::Desc& d) override;
    StreamUpload::Stats  GetStreamUploadStats() const override;
    void                 RecordPendingStreamUploads() override;
    // QueueTextureUpload for a texture the pool does not own (the ImGui backend's), in @p state
    // whenever no copy is moving it: the same window, ring, order and rules; the copy holds its own
    // reference to @p dst until its slot retires. Nothing else may transition @p dst.
    StreamUpload::Result QueueNativeTextureUpload(ID3D12Resource* dst, D3D12_RESOURCE_STATES state,
                                                  const StreamUpload::Desc& d);
    // Would QueueTextureUpload / QueueNativeTextureUpload accept a call from this thread and scope
    // right now. A caller with a synchronous fallback (the ImGui backend) asks first, so an expected
    // refusal - the -renderowneroff Editor's UI build inside a device-execution scope - is not logged
    // as a misuse.
    bool StreamUploadAcceptsCallsHere() const { return StreamUploadCallRefused() == nullptr; }
    // A GPU-to-GPU copy, queued like QueueTextureUpload, of all of @p src - a resource the pool does
    // not own, FFmpeg's D3D12VA frame - into @p dst (same format, size and subresources). @p src must
    // be in COMMON, as a finished decode surface is: the copy reads it through implicit promotion with
    // no barrier of ours, so this queue never transitions a resource the decoder's queue may use as a
    // reference. The graphics queue waits for @p srcFence >= @p srcFenceValue (null: none) before the
    // frame's first list; @p keepAlive (an AVFrame reference) is held until the copy's slot retires.
    // No queue flush or CPU wait, unlike CopyD3D12ResourceToTexture.
    StreamUpload::Result QueueNativeTextureCopy(const RHI::Texture& dst, ID3D12Resource* src,
                                                ID3D12Fence* srcFence, uint64_t srcFenceValue,
                                                std::shared_ptr<void> keepAlive, StreamUpload::Policy policy);

    // Synchronous GPU-to-GPU copy of an external ID3D12Resource (e.g. FFmpeg's D3D12VA NV12 output)
    // into a managed RHI::Texture: the graphics queue waits @p waitFence >= @p waitFenceValue before
    // the copy list, then the CPU waits for the copy, so @p dst can be sampled by any later list.
    // Both must share format and dimensions (CopyResource maps all subresources; NativeCopyFits).
    // @p src is read in COMMON through implicit promotion. A null @p waitFence skips the GPU wait (only
    // when the producer has provably retired). False when a handle is invalid or the copy does not
    // fit. DX12-only.
    bool CopyD3D12ResourceToTexture(RHI::Texture&    dst,
                                     ID3D12Resource* src,
                                     ID3D12Fence*    waitFence,
                                     UINT64          waitFenceValue);

    bool CreateShader(
        RHI::ShaderStage stage,
        const void*      bytecode,
        size_t           bytecodeSize,
        RHI::Shader&     outShader) override;

    bool CreateSampler(
        const RHI::SamplerDesc& desc,
        int&                    outDescriptorIndex) override;

    bool CreatePipelineState(
        const RHI::PipelineStateDesc& desc,
        RHI::PipelineState&           outPSO) override;
    // Compute pipelines created concurrently (IGraphicsDevice).
    bool CreatePipelineStates(const RHI::PipelineStateDesc* descs, uint32_t count,
                              RHI::PipelineState* outs) override;
    // A compute pipeline on a module's OWN root signature (NRD's embedded kernels) through the PSO
    // library, named by the serialized root signature and the bytecode; a create counts in psoCreates
    // like any other. False: the driver's create failed (the caller logs it).
    bool CreateComputePipelineCached(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                                     const void* rootSigBlob, size_t rootSigBlobSize,
                                     Microsoft::WRL::ComPtr<ID3D12PipelineState>& out);
    // A graphics pipeline built from a native desc on its own root signature (the surface's present
    // PSO) through the PSO library, named by the serialized root signature, the VS / PS bytecode and
    // @p stateKey - which must cover every other state the caller varies. Counted like any graphics
    // PSO. False: the driver's create failed (the caller logs it).
    bool CreateGraphicsPipelineCached(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                      const void* rootSigBlob, size_t rootSigBlobSize, uint64_t stateKey,
                                      Microsoft::WRL::ComPtr<ID3D12PipelineState>& out);

    // Pipeline layouts (IGraphicsDevice), and the field-for-field translation CreatePipelineLayout
    // serializes through (@p dwords: the root cost).
    bool CreatePipelineLayout(
        const RHI::PipelineLayoutDesc& desc,
        RHI::PipelineLayout&           outLayout) override;
    void DestroyPipelineLayout(RHI::PipelineLayout& layout) override;
    bool SerializePipelineLayout(const RHI::PipelineLayoutDesc& desc, std::vector<uint8_t>& blob,
                                 uint32_t& dwords, std::string& error) const;

    // Payload release for the APPEND-ONLY shader / PSO pools: frees the DXIL blob / defers the driver
    // PSO and leaves the slot retired-but-present, so a stale handle fails loudly in BindPipelineState
    // instead of aliasing a recycled one (see the resource-pool comment at m_bufferPool). On success
    // CreatePipelineState calls DestroyPipelineState on the handle it overwrites, so a rebuild is just
    // CreatePipelineState(d, m_pso) again.
    void DestroyShader(RHI::Shader& shader) override;
    void DestroyPipelineState(RHI::PipelineState& pso) override;

    // ---- IGraphicsDevice: command recording, state ----
    void BindPipelineState(const RHI::PipelineState& pso,
                           RHI::CommandList cmd) override;
    void SetPrimitiveTopology(RHI::PrimitiveTopology topology,
                              RHI::CommandList cmd) override;
    void SetViewport(const RHI::Viewport& viewport,
                     RHI::CommandList cmd) override;
    void SetScissorRect(uint32_t left, uint32_t top,
                        uint32_t right, uint32_t bottom,
                        RHI::CommandList cmd) override;

    // ---- IGraphicsDevice: command recording, resource binding ----
    void BindConstantBuffer(RHI::Slots::GfxCB slot,
                            const RHI::GPUBuffer& buffer,
                            RHI::CommandList cmd) override;

    // Slot-offset variant for ring-buffer style CB uploads (e.g. probe capture
    // pass uploading 6 face CBs into one 1.5KB ring).
    void BindConstantBufferAtOffset(RHI::Slots::GfxCB slot,
                                    const RHI::GPUBuffer& buffer,
                                    uint64_t byteOffset,
                                    RHI::CommandList cmd) override;

    // Custom-material CBV bind (root slot Gfx::CustomMatCBV, register b8 space0). Called per draw
    // with a GPU VA from MaterialCBVRing; GBufferPass is the main caller. Standard shaders that do not
    // declare b8 are unaffected when it is not called.
    void BindCustomMaterialCBV(uint64_t gpuAddress, RHI::CommandList cmd) override;

    // This frame's constants (Graphics/TransientConstants.h).
    TransientConstants AllocTransientConstants(uint32_t bytes) override;
    void BindConstantBufferAddress(RHI::Slots::GfxCB slot, uint64_t gpuAddress, RHI::CommandList cmd) override;

    // Custom-material texture table bind (root slot Gfx::CustomMatTex, t0-t7 space3). @p gpuHandle is
    // the base of a contiguous range from MaterialSRVRing::Allocate, filled with the shader's
    // reflected textures; unset slots in the range default to white.
    void BindCustomMaterialTextureTable(uint64_t gpuHandle, RHI::CommandList cmd) override;

    void BindResource(RHI::Slots::GfxSRV slot,
                      const RHI::GPUResource& resource,
                      RHI::CommandList cmd) override;
    void SetDynamicSampler(int samplerIndex, RHI::CommandList cmd) override;
    // The sampler heap index of pool entry @p poolIndex (what SamplerDescriptorHeap[] takes); ~0u when
    // there is none. For the fixed SMP_* slots, created first, it equals @p poolIndex.
    uint32_t SamplerHeapIndex(int poolIndex) const;

    // ---- IGraphicsDevice: PVF bindings ----
    void SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                          RHI::CommandList cmd) override;
    // One SetGraphicsRoot32BitConstants, not the base class's 3 + 1 (the driver corner case
    // GBufferPass.cpp records).
    void SetRootConstants4(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3,
                           RHI::CommandList cmd) override;
    void SetRootBufferSRV(RHI::Slots::GfxParam rootSlot,
                          const RHI::GPUBuffer& buf,
                          RHI::CommandList cmd) override;
    void BindDescriptorTableGpuHandle(RHI::Slots::GfxParam rootSlot, uint64_t gpuHandle,
                                      RHI::CommandList cmd) override;

    // ---- IGraphicsDevice: draws + barriers (no VB/IB: PVF replaces the input assembler) ----
    void DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                       uint32_t startVertex,  uint32_t startInstance,
                       RHI::CommandList cmd) override;
    void DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                              uint32_t startIndex,  int32_t  baseVertex,
                              uint32_t startInstance,
                              RHI::CommandList cmd) override;
    void ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                              uint64_t argBufferOffset,
                              uint32_t maxCommands,
                              const RHI::GPUBuffer* countBuffer,
                              uint64_t countBufferOffset,
                              RHI::CommandList cmd) override;
    void ExecuteIndirectDispatchMesh(const RHI::GPUBuffer& argBuffer,
                                     uint64_t argBufferOffset,
                                     uint32_t maxCommands,
                                     const RHI::GPUBuffer* countBuffer,
                                     uint64_t countBufferOffset,
                                     RHI::CommandList cmd) override;
    void CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                    uint64_t size, RHI::CommandList cmd) override;
    void CopyBufferRegion(const RHI::GPUBuffer& src, uint64_t srcOffset,
                          const RHI::GPUBuffer& dst, uint64_t dstOffset,
                          uint64_t size, RHI::CommandList cmd) override;
    void CopyTextureSubresource(const RHI::Texture& src,
                                uint32_t srcMip, uint32_t srcArraySlice,
                                const RHI::Texture& dst,
                                uint32_t dstMip, uint32_t dstArraySlice,
                                RHI::CommandList cmd) override;
    void CopyTexture(const RHI::Texture& src, const RHI::Texture& dst, RHI::CommandList cmd) override;

    // ---- IGraphicsDevice: compute dispatch ----
    void BindComputePipelineState(const RHI::PipelineState& pso,
                                  RHI::CommandList cmd) override;
    void DispatchCompute(uint32_t threadGroupX, uint32_t threadGroupY, uint32_t threadGroupZ,
                         RHI::CommandList cmd) override;
    void DispatchComputeIndirect(const RHI::GPUBuffer& args, uint64_t byteOffset,
                                 RHI::CommandList cmd) override;
    // A graphics PSO's own pipeline layout, by absolute root parameter.
    void SetGraphicsRootCBVAddress(RHI::Slots::GfxParam rootSlot, uint64_t gpuAddress,
                                   RHI::CommandList cmd) override;
    void SetGraphicsRootConstants(RHI::Slots::GfxParam rootSlot, uint32_t count, const void* data,
                                  uint32_t destOffset, RHI::CommandList cmd) override;
    void SetGraphicsDescriptorTable(RHI::Slots::GfxParam rootSlot, uint64_t gpuHandle,
                                    RHI::CommandList cmd) override;
    void DispatchMesh(uint32_t threadGroupX, uint32_t threadGroupY, uint32_t threadGroupZ,
                      RHI::CommandList cmd) override;
    void SetComputeRootCBV(RHI::Slots::CmpParam rootSlot,
                           const RHI::GPUBuffer& cb,
                           uint32_t byteOffset,
                           RHI::CommandList cmd) override;
    using IGraphicsDevice::SetComputeRootCBV;
    void SetComputeRootCBVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuAddress, RHI::CommandList cmd) override;
    void SetComputeDescriptorTable(RHI::Slots::CmpParam rootSlot,
                                   uint64_t gpuHandle,
                                   RHI::CommandList cmd) override;
    void SetComputeRootConstants(RHI::Slots::CmpParam rootSlot, uint32_t count,
                                 const void* data, uint32_t destOffset,
                                 RHI::CommandList cmd) override;
    void SetComputeRootSRVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuVA,
                                  RHI::CommandList cmd) override;
    uint64_t GetTextureUAVGpuHandle(const RHI::Texture& tex) const override;
    uint64_t GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const override;
    uint64_t GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const override;
    // Shader-visible-heap index of a buffer's SRV, for SM 6.6 ResourceDescriptorHeap[idx] (the shared
    // signatures set CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED; Resource Binding Tier 3 is required). ~0u
    // when the buffer has no SRV.
    uint32_t GetBufferSRVHeapIndex(const RHI::GPUBuffer& buf) const override;
    uint32_t GetBufferCBVHeapIndex(const RHI::GPUBuffer& buf) const override;
    uint32_t DescriptorHeapIndexFromGpuHandle(uint64_t gpuHandle) const override;
    // The heap index of the FIXED null descriptor for @p kind (shaders/shared/bindless_kind.hlsli):
    // BINDLESS_NULL_*, or ~0u for a kind with no fixed slot (BindlessNull makes those on first use).
    uint32_t BindlessNullIndex(uint8_t kind) const;
    uint32_t MakeBindlessHandle(uint32_t heapIndex, uint8_t kind, uint32_t stride,
                                const char* what) override;
    uint32_t BindlessNull(uint8_t kind, uint32_t stride = 0) override;
    // A pass's block of null structured-buffer SRVs; the allocation stays in m_descriptorBlocks.
    bool     CreateNullStructuredSrvBlock(const uint32_t* strides, uint32_t count,
                                          RHI::DescriptorBlock& out) override;
    void     ReleaseDescriptorBlock(RHI::DescriptorBlock& block, bool quiesced) override;
    // An uninitialised block, and a TextureCubeArray view written into one of its descriptors (the
    // reflection probes' per-cube SRVs).
    bool     AllocateDescriptorBlock(uint32_t count, RHI::DescriptorBlock& out) override;
    bool     WriteTextureCubeArraySrv(const RHI::DescriptorBlock& block, uint32_t index,
                                      const RHI::Texture& texture, uint32_t firstCube,
                                      uint32_t numCubes, uint32_t mipLevels) override;
    // Ray-tracing acceleration structures (GraphicsDX12_AccelerationStructure.cpp).
    bool     GetBLASPrebuildInfo(const RHI::ASTriangles* geoms, uint32_t count, RHI::ASBuildFlags flags,
                                 RHI::ASPrebuildInfo& out) const override;
    bool     GetTLASPrebuildInfo(uint32_t maxInstances, RHI::ASBuildFlags flags,
                                 RHI::ASPrebuildInfo& out) const override;
    bool     CreateAccelerationStructure(uint64_t sizeBytes, RHI::ASMemory memory,
                                         RHI::AccelerationStructure& out) override;
    void     DestroyAccelerationStructure(RHI::AccelerationStructure& as) override;
    void     DestroyAccelerationStructure(RHI::AccelerationStructure& as, RHI::QueueType queue) override;
    void     BuildBLAS(RHI::CommandList cmd, const RHI::ASTriangles* geoms, uint32_t count,
                       RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                       const RHI::AccelerationStructure& scratch, uint64_t scratchOffset) override;
    void     BuildTLAS(RHI::CommandList cmd, const RHI::GPUBuffer& instances, uint32_t instanceCount,
                       RHI::ASBuildFlags flags, const RHI::AccelerationStructure& dst,
                       const RHI::AccelerationStructure& scratch) override;
    bool     QueryCompactedSize(RHI::CommandList cmd, const RHI::AccelerationStructure& as) override;
    uint64_t ReadCompactedSize(const RHI::AccelerationStructure& as) override;
    bool     CompactAccelerationStructure(RHI::CommandList cmd, const RHI::AccelerationStructure& src,
                                          uint64_t compactedSize, RHI::AccelerationStructure& out) override;
    // The DXR pipeline (GraphicsDX12_RaytracingPipeline.cpp).
    bool     CreateRaytracingPipeline(const RHI::RaytracingPipelineDesc& desc,
                                      RHI::RaytracingPipeline& out) override;
    void     DestroyRaytracingPipeline(RHI::RaytracingPipeline& pipeline) override;
    void     BindRaytracingPipeline(const RHI::RaytracingPipeline& pipeline, RHI::CommandList cmd) override;
    void     DispatchRays(const RHI::RaytracingPipeline& pipeline, uint32_t width, uint32_t height,
                          uint32_t depth, RHI::CommandList cmd) override;
    bool     SetFixedDebugUav(uint32_t slot, const RHI::Texture* texture) override;
    bool     SetFixedDebugBuffer(uint32_t slot, const RHI::GPUBuffer* buffer) override;
    uint64_t GetBufferGPUAddress(const RHI::GPUBuffer& buf) const override;
    bool     GetGPUTimestampResult(const char* regionName,
                                   float& outGpuMs) const override;
    void     SetRenderTargetSubresource(const RHI::Texture& rt,
                                        uint32_t            arraySlice,
                                        uint32_t            mip,
                                        const RHI::Texture* ds,
                                        RHI::CommandList    cmd) override;
    void     ClearRenderTargetSubresource(const RHI::Texture& rt,
                                          uint32_t            arraySlice,
                                          uint32_t            mip,
                                          const float         color[4],
                                          RHI::CommandList    cmd) override;
    bool     IsResourceBindingTier3() const { return m_resourceBindingTier3; }
    void SetHdrTextureState(RHI::ResourceState newState,
                            RHI::CommandList cmd) override;

    void SetParallelPassRecording(bool active) override
    { m_parallelPassRecording.store(active, std::memory_order_release); }

    // Logs once if a caller CHANGES the shared HDR state tracker while RenderGraph Phase B records
    // passes in parallel: the change is visible to every other worker mid-flight, so they emit barriers
    // whose before-state does not match what the GPU will see. Use CopyHdrSceneToScoped (or transition
    // in Phase A) instead.
    void WarnOnRacyHdrStateChange(const char* site) const;

    void PushBarrier(const RHI::GPUBarrier& barrier,
                     RHI::CommandList cmd) override;

    void PushBarriers(const RHI::GPUBarrier* barriers,
                      uint32_t               count,
                      RHI::CommandList       cmd) override;

    void PushBarrierScoped(const RHI::GPUBarrier& barrier, RHI::CommandList cmd) override;

    bool DeviceStateAgrees(const RHI::Texture& tex,
                           RHI::ResourceState  expected,
                           uint32_t*           deviceStateOut   = nullptr,
                           uint32_t*           expectedStateOut = nullptr,
                           int                 plane            = -1) const override;
    RHI::ResourceState QueryTextureState(const RHI::Texture& tex, int plane = -1) const override;
    std::string        QueryTextureName(const RHI::Texture& tex) const override;

    // DX12_DEPTHTRACE only (Graphics/DepthTrace.h). Both PushBarrier forms route IMAGE barriers here;
    // returns at once when the trace is off. One line per D3D12 barrier the translation produced
    // (@p out, @p n), so the trace and the journal stay one to one.
    void TraceImageBarrier(const RHI::GPUBarrier& barrier, const D3D12_RESOURCE_BARRIER* out,
                           uint32_t n, RHI::CommandList cmd);

    // ---- Frame snapshot + barrier journal (Graphics/FrameSnapshot.h) ----
    // ArmFrameJournal before BeginFrame journals that whole frame; EndFrame builds its FrameSnapshot
    // after the submit loop.
    void ArmFrameJournal() override { m_journalArmed.store(true, std::memory_order_release); }
    bool FrameJournalActive() const override { return m_journalOn.load(std::memory_order_relaxed); }
    std::shared_ptr<const GfxDiag::FrameSnapshot> TakeFrameSnapshot() override;

    // Barriers this device's own code issued straight on a pool list (the HDR
    // target, the surface's back buffer and composition target): the same
    // journal entry PushBarrier makes, marked native. One relaxed load when the
    // frame is not journaled.
    void JournalNativeBarriers(RHI::CommandList cmd, const D3D12_RESOURCE_BARRIER* barriers,
                               uint32_t count)
    {
        CommandListDX12& entry = GetPoolEntry(cmd);
        if (entry.IsValid()) entry.counters.barriers += count;
        if (m_journalOn.load(std::memory_order_relaxed))
            JournalBarriers(entry, barriers, count, true);
    }
    // GfxDiag::NoteNativeBarriers lands here: finds the pool slot of a raw
    // D3D12 list (private data set when the list was created), then journals.
    void JournalNativeList(ID3D12GraphicsCommandList* list, const D3D12_RESOURCE_BARRIER* barriers,
                           uint32_t count);

    void SetRenderTargets(uint32_t numRTs,
                          const RHI::Texture* const* rtvs,
                          const RHI::Texture*        dsv,
                          RHI::CommandList           cmd,
                          const RHI::DepthView&      view = {}) override;

    void SetRenderTargetsAndHdr(uint32_t numRTs,
                                const RHI::Texture* const* rtvs,
                                const RHI::Texture*        dsv,
                                RHI::CommandList           cmd,
                                const RHI::DepthView&      view = {}) override;

    void ClearRenderTarget(const RHI::Texture& texture,
                           const float         color[4],
                           RHI::CommandList    cmd) override;

    void ClearDepthStencil(const RHI::Texture& texture,
                           float               depth,
                           uint8_t             stencil,
                           RHI::CommandList    cmd) override;

    void SetDepthStencilSlice(const RHI::Texture& depthTex,
                              uint32_t            arraySlice,
                              RHI::CommandList    cmd) override;

    void SetDepthStencilTarget(const RHI::Texture& depthTex,
                               RHI::CommandList    cmd) override;

    void ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                uint32_t            arraySlice,
                                float               depth,
                                uint8_t             stencil,
                                RHI::CommandList    cmd) override;

    void SetStencilRef(uint32_t ref, RHI::CommandList cmd) override;

    void SetGraphicsRootConstant(RHI::Slots::GfxParam rootSlot,
                                 uint32_t value,
                                 uint32_t offsetInWords,
                                 RHI::CommandList cmd) override;

    uint64_t GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                       uint32_t mip) const override;
    uint64_t GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                            uint32_t cubeIdx, uint32_t face,
                                            uint32_t mip) override;
    uint64_t GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                           uint32_t cubeIdx, uint32_t mip) override;

    uint32_t BeginGPUTimestamp(RHI::CommandList cmd, const char* name) override;
    void     EndGPUTimestamp  (RHI::CommandList cmd, uint32_t regionIndex) override;
    void     SetSubmitBarrier(RHI::CommandList cmd) override;
    uint32_t BeginGPUTimestampNoTracy(RHI::CommandList cmd, const char* name) override;
    void     EndGPUTimestampNoTracy  (RHI::CommandList cmd, uint32_t regionIndex) override;
    void     BeginTracyGPUZone(RHI::CommandList cmd, const char* name,
                               uint32_t regionIndex) override;
    void     EndTracyGPUZone  (uint32_t regionIndex) override;
    void     SetRecordingOwner(uintptr_t owner) override;
    Diag::Prof::GpuCounters GetOwnerCounters(uintptr_t owner) override;
    void     AnnotateGPUTimestamp(uint32_t regionIndex, const Diag::Prof::GpuCounters& counters) override;
    void     BeginEventMarker (RHI::CommandList cmd, const char* name) override;
    void     EndEventMarker   (RHI::CommandList cmd) override;
    void     SetCommandListName(RHI::CommandList cmd, const char* name) override;
    bool     IsGPUProfilerEnabled() const override { return m_gpuProfiler.enabled; }

    void BindDescriptorHeaps(RHI::CommandList cmd) override;

    void* MapBuffer(const RHI::GPUBuffer& buffer) override;
    void  UnmapBuffer(const RHI::GPUBuffer& buffer) override;

    void CopyTexturePixelToBuffer(const RHI::Texture& src,
                                  uint32_t srcX, uint32_t srcY,
                                  RHI::GPUBuffer& dst,
                                  RHI::CommandList cmd) override;

    // WHOLE mip 0 of a 2D texture into a READBACK buffer (material-preview thumbnails, the object-ID
    // image). The row pitch is NOT width*bpp: D3D12 aligns a placed footprint's rows to
    // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256), so walking the mapped bytes as a tight image reads the
    // wrong pixels whenever the natural pitch is not a multiple of 256. Call
    // GetTexture2DReadbackFootprint first, size the READBACK buffer to `outTotalBytes`, walk it with
    // `outRowPitch`, then copy. @p src must already be in COPY_SRC: no barriers are issued, like
    // CopyTextureSubresource.
    bool GetTexture2DReadbackFootprint(const RHI::Texture& tex,
                                       uint32_t& outRowPitch,
                                       uint32_t& outRows,
                                       uint64_t& outTotalBytes) const override;
    void CopyTexture2DToBuffer(const RHI::Texture& src,
                               const RHI::GPUBuffer& dst,
                               RHI::CommandList cmd) override;
    void CopyTexture2DToBufferInPlace(const RHI::Texture& src,   // GraphicsDX12_StreamUpload.cpp
                                      const RHI::GPUBuffer& dst,
                                      RHI::CommandList cmd) override;

    void DestroyBuffer(RHI::GPUBuffer& buffer) override;
    void DestroyTexture(RHI::Texture& texture) override;
    bool ReplaceTexture(RHI::Texture& texture, const RHI::TextureDesc& desc,
                        const RHI::SubresourceData* initialData) override;

    // Defer-release a raw ID3D12Resource that is not pool-managed (e.g. a released acceleration
    // structure's memory): it joins the frame-stamped release bucket, outlives every list recorded this
    // frame and is freed once that frame retires. Use it whenever a resource may still be referenced by
    // an in-flight or not-yet-submitted list, instead of letting its ComPtr destruct inline.
    void DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource);

    // Defer-release tied to one queue's fence value, for work that outlives the frame-stamped window
    // (async-compute BLAS builds still reading the resource frames later). Released at the first
    // BeginFrame after m_queueFences[queueIndex] completes @p fenceValue. @p queueIndex: 0 graphics,
    // 1 compute, 2 copy (RHI::QueueType). Pass GetLastSignaledFenceValue() taken right AFTER
    // submitting the work that uses the resource.
    void DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource,
                              uint32_t queueIndex,
                              uint64_t fenceValue);

    // ---- Static descriptor blocks, released like resources ----
    // DescriptorAllocation::Free() is IMMEDIATE: the next AllocateStatic can take the block, so freeing
    // one from a pass's Shutdown is safe only if no in-flight list still names it. This queues it into
    // the frame-stamped bucket DestroyHdrTarget uses; DrainReleasesUpTo frees it once the frames that
    // could reference it retire.
    // Takes the allocation BY VALUE and the caller must drop its own copy (assign {} / reset()):
    // DescriptorAllocation is memberwise copyable, so a moved-from source still names the block and a
    // second Free() would double-release.
    void DeferReleaseDescriptors(DescriptorAllocation alloc);

    // Last value signaled on a queue's fence. Caller can stash this right
    // after submitting work that references a resource, then pass it to
    // DeferReleaseResource(resource, queue, fenceValue) to time the
    // release precisely. queueIndex: 0/1/2 = graphics/compute/copy.
    uint64_t GetLastSignaledFenceValue(uint32_t queueIndex) const;

    // ---- Cubemap-array slice <-> .itex file I/O (reflection-probe bake persistence) ----
    // The engine's native .itex container ([AssetHeader][TextureMetadata][DDS bytes]), as
    // TextureImporter / TextureLoader use it, so probe cubemaps are ordinary engine assets. Both work on
    // ONE cube (6 consecutive array slices, all mips) of a TextureCubeArray: @p cubeIndex picks it,
    // slices [cubeIndex*6 .. cubeIndex*6+5]. @p currentState is the state of the WHOLE array on entry
    // (SHADER_RESOURCE outside an in-flight bake). Synchronous (FlushAndWait + a one-shot list + fence,
    // like CaptureTextureToPNG): call at frame boundaries or tool events, never inside an open pass.
    bool SaveTextureCubeToITEX(const RHI::Texture& cubeArrayTex,
                               uint32_t            cubeIndex,
                               RHI::ResourceState  currentState,
                               const char*         path) override;
    bool LoadITEXIntoTextureCube(RHI::Texture&      cubeArrayTex,
                                 uint32_t           cubeIndex,
                                 RHI::ResourceState currentState,
                                 const char*        path) override;

    bool InitPSOLibrary(const char* cacheFilePath) override;
    void SavePSOLibrary(const char* cacheFilePath) override;
    void CheckpointPSOLibrary(const char* cacheFilePath) override;

    // The boot precompile (GraphicsDX12_Precompile.cpp).
    bool StartPipelinePrecompile(const std::vector<uint8_t>& manifest, const std::string& indexPath,
                                 uint32_t threads) override;
    RHI::PipelinePrecompileStatus GetPipelinePrecompileStatus() const override;
    void FinishPipelinePrecompile(bool cancel, const std::string& cacheFilePath) override;

    // ---- DRED (Device Removed Extended Data), runtime opt-in ----
    // The only forensics that survive a TDR in a player's build: the command list and op index the GPU
    // died on, and whether the page-fault VA hit a live or a RECENTLY FREED allocation (use-after-free).
    // It costs a GPU breadcrumb write per command, so it defaults ON in Debug and OFF in Release.
    // MUST be set BEFORE the device is created (before GraphicsDX12 is constructed), hence static. No
    // settings key forwards to it yet; the DX12_DRED env var (=1 on, =0 off) overrides this setter in
    // both directions, the switch a player can be talked through.
    static void SetDREDEnabled(bool v);
    static bool IsDREDEnabled();
    // True only if DRED was actually armed on THIS device (the setting was on
    // AND D3D12GetDebugInterface handed us the settings object). The dump path
    // uses it to explain an empty breadcrumb list instead of staying silent.
    bool IsDREDActive() const { return m_dredActive; }
    // Logs GetDeviceRemovedReason + the DRED breadcrumbs / page fault and
    // latches RunLedger's device-lost, once per device. Present calls it on a
    // removed-class HRESULT; the fence paths call it when a completed value
    // reads UINT64_MAX, which is where a TDR usually surfaces first. Returns
    // false (and logs nothing) while the device is still healthy.
    bool ReportDeviceRemoved(const char* where);

    // ---- Selected adapter identity ----
    // Filled in LoadPipeline from the adapter the device was ACTUALLY created on; members because the
    // PSO-cache fingerprint and crash reports need them long after init.
    const DXGI_ADAPTER_DESC1& GetAdapterDesc()    const { return m_adapterDesc; }
    LUID                      GetAdapterLuid()    const { return m_adapterLuid; }
    // UMD (user-mode driver) version from CheckInterfaceSupport, 0 when the
    // adapter refuses to report it. Packed as the raw LARGE_INTEGER QuadPart.
    uint64_t                  GetDriverVersion()  const { return m_driverVersion; }
    D3D_FEATURE_LEVEL         GetFeatureLevel()   const { return m_featureLevel; }

    // ---- DX12-specific accessors (RenderPass code casts IGraphicsDevice& to GraphicsDX12&) ----
    ID3D12GraphicsCommandList* GetNativeCommandList(RHI::CommandList cmd) const;
    ID3D12Device*              GetDevice() const;
    // Exposed so the editor's ImGui bridge can bind backend-owned handles
    // without touching the engine's internal state directly.
    ID3D12CommandQueue*        GetGraphicsQueue()   const { return m_queues[0].Get(); }
    DescriptorHeapAllocator&   GetRtvAllocator();
    DescriptorHeapAllocator&   GetCbvSrvUavAllocator();
    // The shader-visible sampler heap, for a list that binds its own heaps: the shared signatures
    // index it directly, so bind it with the CBV/SRV/UAV heap wherever a shared signature may follow.
    DescriptorHeapAllocator&   GetSamplerAllocator();
    DescriptorHeapAllocator&   GetDsvAllocator();
    // True if the device reports D3D12_MESH_SHADER_TIER_1 or higher. Queried
    // once during init from D3D12_FEATURE_DATA_D3D12_OPTIONS7. Terrain (and
    // any future MS pass) must skip Init when this is false.
    bool                       SupportsMeshShader() const { return m_meshShaderSupported; }
    // DXR capability (D3D12_FEATURE_DATA_D3D12_OPTIONS5). Tier 1.0 has DispatchRays / AS / SBT;
    // Tier 1.1 adds inline ray tracing and more ray flags. DDGI's probe trace needs Tier 1.0.
    bool                       SupportsDXR() const { return m_dxrSupported; }
    bool                       SupportsInlineRT() const { return m_inlineRtSupported; }
    // QI'd from m_device on demand; nullptr when SupportsDXR() is false. For AS builds and
    // state-object creation.
    struct ID3D12Device5*      GetDevice5() const;
    D3D12_CPU_DESCRIPTOR_HANDLE GetTextureDsvCpuHandle(const RHI::Texture& tex) const;
    // The D3D12 resource behind a GPUBuffer (MeshDescriptorHeap, DDGIVolumeManager).
    ID3D12Resource*            GetBufferResource(const RHI::GPUBuffer& buffer) const;
    // The D3D12 resource behind a Texture.
    ID3D12Resource*            GetTextureResource(const RHI::Texture& texture) const;

    // FNV-1a over the SERIALIZED root-signature blobs (graphics + compute), folded in as each is built
    // at init: the canonical fingerprint of the binding layout, which changes by itself when a root
    // parameter is added, removed, moved or widened. ComputePSOName mixes it into the
    // ID3D12PipelineLibrary entry name, so a layout change purges the disk PSO cache by construction.
    // Not on IGraphicsDevice on purpose: only DX12-side code consumes it.
    uint64_t                   GetRootSignatureHash() const { return m_rootSigHash; }

    // ---- Descriptor-table fill helpers (multi-volume DDGI) ----
    // A caller (DDGIVolumeManager) owns a DescriptorAllocation of N contiguous slots and rewrites
    // single SRVs at chosen offsets without reallocating it. All four write through a CPU handle into
    // the shader-visible heap (GetGpuCpuHandle() + slot * descSize). The null variants keep unallocated
    // DDGI slots valid: the blocks are read through heap indices, not bound as root tables, so nothing
    // validates them and a type-correct null is all that stands between a stale volume slot and
    // undefined behaviour (Lighting.ps's per-slot `(flags & 1u)` gate still prevents the read).
    void CreateStructuredBufferSRVAtCpu(const RHI::GPUBuffer& buf,
                                        D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateTexture2DSRVAtCpu(const RHI::Texture& tex,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateNullStructuredBufferSRVAtCpu(uint32_t stride,
                                            D3D12_CPU_DESCRIPTOR_HANDLE dst) const;
    void CreateNullTexture2DSRVAtCpu(DXGI_FORMAT format,
                                     D3D12_CPU_DESCRIPTOR_HANDLE dst) const;

    // ---- DX12 translation helpers ----
    // Public: the backend's SDK wrappers (NrdDenoiser) take RHI textures and states and translate them
    // with the device's own rules.
    static DXGI_FORMAT             ToDxgiFormat(RHI::Format f);
    static D3D12_RESOURCE_STATES   ToD3D12ResourceState(RHI::ResourceState s);

    // ---- Upload-stall instrumentation (load profiling) ----
    // CPU time blocked inside FlushUploadAndWait (the per-batch GPU fence round trip of an asset
    // upload) plus a flush count, reset around a world load (EditorLayer::ProcessPendingActions) to
    // measure how much of the load stall is GPU-upload bound. Summed across BOTH upload contexts: "how
    // much CPU time did uploads cost this load" is a whole-process question. The never-reset
    // whole-process totals (frames.jsonl, Lua's Profiler.Upload) are Graphics/UploadStats.h.
    void     ResetUploadStats()          override
    {
        for (auto& c : m_uploadCtx)
        {
            c.stallNs.store(0, std::memory_order_relaxed);
            c.flushCount.store(0, std::memory_order_relaxed);
        }
    }
    double   GetUploadStallMs()    const override
    {
        return static_cast<double>(m_uploadCtx[0].stallNs.load(std::memory_order_relaxed) +
                                   m_uploadCtx[1].stallNs.load(std::memory_order_relaxed)) / 1.0e6;
    }
    uint32_t GetUploadFlushCount() const override
    {
        return m_uploadCtx[0].flushCount.load(std::memory_order_relaxed) +
               m_uploadCtx[1].flushCount.load(std::memory_order_relaxed);
    }
    // Per-context lifetime flush count. 0 == that context never carried a byte,
    // which is a fact worth being able to state rather than assume.
    uint64_t UploadFlushesLifetime(int ctx) const override
    { return m_uploadCtx[(ctx == 1) ? 1 : 0].flushesLifetime.load(std::memory_order_relaxed); }

    // Editor stats: dedicated (LOCAL segment) VRAM usage vs the OS budget, in bytes. False when the
    // adapter query is unavailable. The same DXGI query EndFrame's over-budget watchdog runs: cheap per
    // frame.
    bool QueryVramUsage(uint64_t& usedBytes, uint64_t& budgetBytes) override;

    // ---- GPU profiler (timestamp queries) ----
    GPUProfiler& GetGPUProfiler() { return m_gpuProfiler; }

    bool GetGPUFrameTiming(float& outEffectiveMs, float& outOverlapMs,
                           uint32_t& outAgeFrames) const override
    {
        if (!m_gpuProfiler.enabled || !m_gpuProfiler.hasResults) return false;
        outEffectiveMs = m_gpuProfiler.effectiveFrameMs;
        outOverlapMs   = m_gpuProfiler.overlapMs;
        outAgeFrames   = m_gpuProfiler.resultsAgeFrames;
        return true;
    }
    // The profiler's results as an RHI view.
    bool GetGPUTimings(RHI::GpuTimingView& out) const override
    {
        out.results          = m_gpuProfiler.results;
        out.resultCount      = m_gpuProfiler.resultCount;
        out.resultsAgeFrames = m_gpuProfiler.resultsAgeFrames;
        out.resultsSequence  = m_gpuProfiler.resultsSequence;
        out.enabled          = m_gpuProfiler.enabled;
        out.hasResults       = m_gpuProfiler.hasResults;
        out.calibrationValid = m_gpuProfiler.calibrationValid;
        return true;
    }

    // ---- GPU-profiler want count ----
    // GPUProfiler::enabled has several owners: -gpuprofile at boot, the editor's Profiler window (true
    // when opened, HARD-WRITES false when closed) and the debug HUD from level Unit up. A want is a
    // non-owning subscription: the profiler runs while at least one is held, so one owner switching off
    // cannot silently kill another's GPU readout. ReconcileGPUProfilerEnable composes wants with the
    // owners that still write the flag directly.
    void AddGPUProfilerWant() override
    { m_gpuProfilerWants.fetch_add(1, std::memory_order_relaxed); }
    void ReleaseGPUProfilerWant() override
    {
        // CAS rather than a test-then-decrement: two holders releasing at once
        // could both see 1 and both decrement, wrapping the count to ~0u and
        // pinning the profiler on for the rest of the process.
        uint32_t cur = m_gpuProfilerWants.load(std::memory_order_relaxed);
        while (cur > 0 && !m_gpuProfilerWants.compare_exchange_weak(
                   cur, cur - 1, std::memory_order_relaxed))
        {
        }
    }

    // ---- Debug-marker (PIX/RenderDoc) opt-in ----
    // OFF by default (a named event per pass and per profiler region, plus a command-list rename per
    // use, cost CPU time every frame). Turned on at init by -gpumarkers, DX12_MARKERS=1 (=0 forces off)
    // or an injected capture tool (renderdoc.dll / WinPixGpuCapturer.dll): the init block next to
    // -gpuprofile in GraphicsDX12.cpp. Every Begin is closed on the SAME command list (graph passes in
    // Phase A/C, regions in Begin/EndGPUTimestamp, chunks by their owner), and EndEventMarker ignores
    // an End with no open Begin on that list.
    void SetDebugMarkersEnabled(bool e) { m_debugMarkers = e; }
    bool IsDebugMarkersEnabled() const  { return m_debugMarkers; }

private:
    struct FrameSlotFence
    {
        std::atomic<uint64_t> framePlus1 { 0 };
        uint64_t              gfx        { 0 };
        uint64_t              compute    { 0 };
        uint64_t              copy       { 0 };
    };

    // Deferred release: Destroy{Buffer,Texture} / DestroyHdrTarget / FreeDescriptorTable queue GPU
    // resources and descriptors here instead of freeing them at once. PUSHED through
    // PendingReleaseBucket(), DRAINED by DrainReleasesUpTo() once provably retired. Keyed by frame
    // stamp, not by slot (m_pendingReleases says why).
    struct DeferredReleaseList
    {
        // Declared FIRST so that ~DeferredReleaseList destructs `resources` (placed buffers) BEFORE
        // `heaps`: D3D12 requires a heap to outlive every placed resource in it. Do not move `heaps` below
        // `resources`.
        std::vector<Microsoft::WRL::ComPtr<ID3D12Heap>>     heaps;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> resources;
        // Retired ID3D12PipelineState / ID3D12RootSignature objects (DestroyPipelineState). No heap
        // dependency; kept below `resources` so the destruction rule above is undisturbed.
        std::vector<Microsoft::WRL::ComPtr<ID3D12DeviceChild>> pipelineObjects;
        std::vector<DescriptorAllocation>                   descriptors;
        std::vector<uint32_t>                               bufferSlots;
        std::vector<uint32_t>                               textureSlots;
    };
    struct PendingRelease
    {
        uint64_t            frame;
        DeferredReleaseList list;
    };

    struct FenceKeyedRelease
    {
        uint32_t                                            queueIndex;
        uint64_t                                            fenceValue;
        Microsoft::WRL::ComPtr<ID3D12Resource>              resource;
    };

    // ---- Upload contexts: one per side of the game/render split ----
    struct UploadContext
    {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   list;
        // PRIVATE fence, never m_fenceValue (m_uploadCtx says why).
        Microsoft::WRL::ComPtr<ID3D12Fence>                 fence;
        uint64_t                                            fenceValue = 0;
        // The creation copies' COPY-type list and its own fence (m_uploadCtx says why).
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>      copyAllocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>   copyList;
        Microsoft::WRL::ComPtr<ID3D12Fence>                 copyFence;
        uint64_t                                            copyFenceValue = 0;
        // Which list holds work since the last flush. A flush executes only
        // those, and returns at once when neither does - so EVERY recording
        // into `list` or `copyList` must set its flag, or it never executes.
        bool                                                copyDirty   = false;
        bool                                                directDirty = false;

        // Batch-upload scope depth. When > 0, CreateBuffer(DEFAULT, initialData) records copies onto
        // `list` without a per-buffer flush and EndBufferUploadBatch flushes once at scope exit; staging
        // buffers created meanwhile wait in stagingKeepAlive until the GPU copy completes. The auto-flush
        // thresholds (kBatchFlushBytes / kBatchFlushCount) bound a batch of thousands of tiny buffers: at
        // either one the batch flushes, waits and clears staging, then stays open - ~1 wait per 64 MB / 512
        // buffers instead of ~1 per buffer.
        uint32_t                                            batchDepth = 0;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> stagingKeepAlive;
        uint64_t                                            stagingBytes = 0;

        // Buffer-heap suballocation scope (BeginBufferHeapBatch). While depth
        // > 0, DEFAULT CreateBuffer places into currentBufferHeap at heapOffset,
        // growing a fresh heap when full. Each placed buffer holds its own
        // ComPtr ref to its heap, so heaps outlive their buffers with no free
        // list.
        Microsoft::WRL::ComPtr<ID3D12Heap>                  currentBufferHeap;
        uint64_t                                            heapOffset = 0;
        uint64_t                                            heapSize   = 0;
        uint32_t                                            heapDepth  = 0;
        uint64_t                                            heapHint   = 0;   // the outermost scope's

        // Profiling counters behind the upload-stall accessors. ATOMIC: the
        // lane's own thread adds to them inside FlushUploadAndWait while readers
        // on other threads (the editor's load profile, the preview builds, the
        // shutdown [FRAME_EXECUTOR] line) load them. Nanoseconds rather than a
        // double so the add is a plain fetch_add.
        std::atomic<uint64_t>                               stallNs{ 0 };
        std::atomic<uint32_t>                               flushCount{ 0 };
        // NEVER reset, unlike flushCount (ResetUploadStats zeroes that around a world load and a preview
        // build): it answers "did this context carry anything at all", which the windowed counter cannot.
        std::atomic<uint64_t>                               flushesLifetime{ 0 };
    };

    static constexpr uint32_t kLoaderContexts = 2;

    // The loader context this thread's BeginLoaderUploads scope holds (Upload() answers it first), and
    // that scope's nesting depth.
    static thread_local UploadContext*                   t_loaderCtx;
    static thread_local uint32_t                         t_loaderDepth;

    static constexpr uint64_t kBatchFlushBytes  = 64ull * 1024 * 1024;  // 64 MB
    static constexpr uint32_t kBatchFlushCount  = 512;

    static constexpr uint64_t kBufferHeapBytes = 16ull * 1024 * 1024;  // 16 MB default heap

    // ---- Streaming texture upload (GraphicsDX12_StreamUpload.cpp) ----
    struct StreamCopy                       // StreamUpload::Item's payload
    {
        // The destination itself and its state, taken at QUEUE time on the game
        // half that owns the texture. The recorder never reads the pool record:
        // DestroyTexture can move it out and CreateTexture reuse the id while
        // the render lane records (DestroyTexture parks the lane only on the
        // device frame thread). This reference keeps a destroyed texture's
        // resource alive until the item's slot retires; nothing else
        // transitions a stream target, so `before` is still its state then.
        Microsoft::WRL::ComPtr<ID3D12Resource> dstResource;
        D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
        uint32_t     dstX = 0, dstY = 0;    // the rect's origin (0, 0 for a whole subresource)
        D3D12_SUBRESOURCE_FOOTPRINT footprint{};   // the copy's layout; the offset is the item's
        Microsoft::WRL::ComPtr<ID3D12Resource> staging;   // Fallback: a one-off buffer
        // A GPU-to-GPU copy (Item::gpuSource): all of srcResource into the destination. The source is in
        // COMMON and the copy reads it through implicit promotion - no barrier on it here. The graphics
        // queue waits for srcFence >= srcFenceValue (its producer, a decoder) before the frame's first list.
        // keepAlive owns whatever owns the source (an AVFrame reference) until the item's slot retires.
        Microsoft::WRL::ComPtr<ID3D12Resource> srcResource;
        Microsoft::WRL::ComPtr<ID3D12Fence>    srcFence;
        uint64_t     srcFenceValue = 0;
        std::shared_ptr<void> keepAlive;
    };
    struct StreamUploader
    {
        mutable std::mutex                          mutex;
        StreamUpload::Queue<StreamCopy>             queue;
        Microsoft::WRL::ComPtr<ID3D12Resource>      ring[kFrameCount];
        uint8_t*                                    mapped[kFrameCount] = {};
        uint64_t                                    ringBytes = 0;
        bool                                        initialized = false;
        bool                                        initFailed = false;
        uint64_t                                    stale = 0, recordedLists = 0, recordedCopies = 0;
        // Copies that overlapped an earlier one into the same subresource in
        // their list, so a transition pair was put between them (the recorder).
        uint64_t                                    serialized = 0;
        // The frame's list (its pool index) and the frame it was recorded for,
        // for EndFrame's order check; UINT32_MAX when it recorded none. Render
        // half only.
        uint32_t                                    listIndex = UINT32_MAX;
        uint64_t                                    listFrame = 0;
        // The slot the render half took and its frame: EndFrame of THAT frame
        // reports it submitted. Still set at the next take = the frame was lost.
        uint32_t                                    takenSlot = UINT32_MAX;
        uint64_t                                    takenFrame = 0;
        bool                                        recordIntoPrimary = false;   // -streamupload_probe_primary
        bool                                        orderBreak = false;          // -streamupload_probe_orderbreak
        // RecordPendingStreamUploads .. EndFrame: the device's frame thread is
        // recording a frame. Written and (for the refusal) read on that thread.
        bool                                        recording = false;
        // The recorded list's GPU copies' producer fences, for
        // StreamUploaderBeforeSubmit. Render half only.
        std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Fence>, uint64_t>> waits;
        // The render half's take finding the lock held by a game-half call copying its bytes into the
        // ring: how often, and for how long. Written under the mutex once it is taken.
        uint64_t                                    takeWaits = 0, takeWaitNs = 0, takeWaitMaxNs = 0;
    };

    // Where an item's bytes come from: `rows` rows of `rowBytes`, `pitch` apart - the caller's rows, or
    // the item's own copy (StreamRowsOf).
    struct StreamRows
    {
        const uint8_t* data = nullptr;
        uint64_t       pitch = 0;
        uint32_t       rows = 0;
        uint64_t       rowBytes = 0;
    };

    // ---- Command-list pool ----
    // m_commandLists' capacity (BeginCommandList refuses past it); the comment there says why the
    // storage is a fixed array that never reallocates.
    static constexpr uint32_t kMaxCommandLists = 256;

    // ---- Soft ceiling for m_psoPool ----
    // NOT a hard cap, deliberately: the stable pool makes growth SAFE (its own capacity is 262,144), so
    // refusing at this number would turn a survivable condition into a missing pipeline. It is the
    // denominator of `psos=` on the [POOL] line, the number a pack's `max` is set against, and what the
    // once-only warning watches (m_shaderPoolHeadroomLogged plays that role for the capped shader pool).
    // 4096 mirrors kMaxShaderPool: a graphics PSO needs at least a VS and usually a PS, so the PSO count
    // cannot outrun the shader count by much.
    static constexpr size_t kSoftMaxPsoPool = 4096;

    // ---- Sampler dedupe (kSamplerDedupe) ----
    using SamplerKey = std::array<uint32_t, 10>;
    struct SamplerKeyHash
    {
        size_t operator()(const SamplerKey& k) const noexcept
        {
            uint64_t h = 14695981039346656037ull;              // FNV-1a 64
            for (uint32_t v : k) { h ^= v; h *= 1099511628211ull; }
            return static_cast<size_t>(h);
        }
    };

    // ---- Barrier translation and depth views ----
    // TranslateBarrier's output bound: a depth-stencil barrier whose planes' states differ becomes one
    // per plane.
    static constexpr uint32_t kMaxBarriersPerTranslate = 2;

    // ---- Bindless table, descriptor blocks, AS and RT-pipeline pools ----
    static_assert(kMaxBindlessTextures == BINDLESS_TABLE_SIZE,
                  "shaders/shared/bindless_kind.hlsli places the null descriptors right "
                  "after the bindless table: BINDLESS_TABLE_SIZE must be its size");

    struct AccelStructDX12
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12Resource> compactSizeUav;       // DEFAULT UAV, 8 B: the postbuild emit's dest
        Microsoft::WRL::ComPtr<ID3D12Resource> compactSizeReadback;  // READBACK, 8 B
    };

    struct RaytracingPipelineDX12
    {
        Microsoft::WRL::ComPtr<ID3D12StateObject>   stateObject;
        Microsoft::WRL::ComPtr<ID3D12Resource>      shaderTable;     // UPLOAD
        Microsoft::WRL::ComPtr<ID3D12RootSignature> globalRootSig;   // the layout's
        uint64_t rayGenVA      = 0;
        uint64_t missVA        = 0;
        uint64_t hitGroupVA    = 0;
        uint32_t recordStride  = 0;
        uint32_t missCount     = 0;
        uint32_t hitGroupCount = 0;
    };

    // ---- Bring-up, GPU waits, deferred release and helpers ----
    friend class SurfaceDX12;   // the surface drives frames through the device's internals

    // Device bring-up: debug layer, DRED, adapter, device, queues, descriptor heaps, bindless table and
    // nulls (LoadPipeline); upload contexts, fences, root and command signatures, pools, fixed samplers
    // (LoadAssets). The swap chain is the main surface's.
    void LoadPipeline();
    void LoadAssets();
    // The fixed sampler slots (shared/sampler_slots.hlsli), first into the pool.
    void CreateFixedSamplers();

    // A full GPU drain (every queue), then every deferred release.
    void WaitForPreviousFrame();

    // Upload helper: synchronously copies CPU data into a DEFAULT buffer/texture
    // via an UPLOAD staging buffer; flushes and waits for the GPU.
    void FlushUploadAndWait();

    // Block until every queue has passed the fences recorded the last time
    // @p slot was used. One definition of "this slot is free", shared by the
    // render side (WaitForNextFrameSlot) and the game side (AcquireUploadSlot).
    void WaitSlotRetired(uint32_t slot);
    void AcquireUploadSlotImpl(uint64_t frame, bool retirementComplete);

    // Block until @p fence reaches @p value, on a PER-THREAD auto-reset event.
    // Every fence wait in this class goes through here; see the definition for
    // why a shared event is not merely slower but wrong once two threads wait.
    static void WaitFence(ID3D12Fence* fence, uint64_t value);

    // Free every pending release stamped with a frame <= @p retiredFrame, i.e.
    // every one whose newest possible GPU referencer has provably completed.
    // UINT64_MAX drains the lot and is only legal after a full idle.
    void DrainReleasesUpTo(uint64_t retiredFrame);
    // Shortcut for the hard-sync paths (WaitForPreviousFrame / dtor / Resize).
    void ProcessAllDeferredReleases() { DrainReleasesUpTo(UINT64_MAX); }
    // The frame a release pushed RIGHT NOW must outlive. See m_pendingReleases.
    uint64_t ReleaseStampFrame() const
    { return m_renderFrame.load(std::memory_order_relaxed) + 1; }

    // Drains the m_fenceKeyedReleases entries whose queue fence reached their target value. Called
    // from BeginFrame after the frame-stamped drain.
    void ProcessFenceKeyedReleases();

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -rhi_lifetime_probe (GraphicsDX12_LifetimeProbe.cpp): the resource-lifetime guards exercised on
    // purpose, from BeginFrame right after the release drains.
    void RunLifetimeProbe();
#endif

    // Resolve the pool entry for a CommandList handle
    CommandListDX12& GetPoolEntry(RHI::CommandList cmd);
    const CommandListDX12& GetPoolEntry(RHI::CommandList cmd) const;

    static D3D12_FILTER            ToD3D12Filter(RHI::Filter f);
    static D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(RHI::TextureAddressMode m);
    static D3D12_COMPARISON_FUNC   ToD3D12ComparisonFunc(RHI::ComparisonFunc f);
    static D3D12_BLEND             ToD3D12Blend(RHI::Blend b);
    static D3D12_BLEND_OP          ToD3D12BlendOp(RHI::BlendOp op);
    static D3D12_FILL_MODE         ToD3D12FillMode(RHI::FillMode m);
    static D3D12_CULL_MODE         ToD3D12CullMode(RHI::CullMode m);
    static D3D12_RESOURCE_FLAGS    ToD3D12ResourceFlags(RHI::BindFlag flags);
    static D3D12_HEAP_TYPE         ToD3D12HeapType(RHI::Usage usage);
    static D3D12_PRIMITIVE_TOPOLOGY ToD3D12Topology(RHI::PrimitiveTopology t);
    static D3D12_COMMAND_LIST_TYPE  ToD3D12CommandListType(RHI::QueueType q);

    // Builds the shared graphics root signature (RHI::Slots::Gfx).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateDefaultRootSignature();

    // Builds the shared compute root signature (RHI::Slots::Cmp).
    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateComputeRootSignature();

    // ReadTextureRaw / ReadHdrSceneRaw's shared body: @p res in @p fromState.
    bool ReadResourceRaw(ID3D12Resource* res, D3D12_RESOURCE_STATES fromState, std::uint32_t plane,
                         RawTexture& out, const char* who, std::uint32_t arraySlice = 0);

    // The bucket a release pushed RIGHT NOW belongs in: the m_pendingReleases entry stamped
    // ReleaseStampFrame(), appended when the deque is empty or its newest entry carries an older stamp.
    // CALLER MUST HOLD m_deferredMutex.
    DeferredReleaseList& PendingReleaseBucket();

    // ---- Barrier journal / frame snapshot ----
    void JournalBarriers(CommandListDX12& entry, const D3D12_RESOURCE_BARRIER* barriers,
                         uint32_t count, bool native);
    void BuildFrameSnapshot(uint32_t listCount);

    // ---- Upload contexts: one per side of the game/render split ----
    bool IsLoaderCtx(const UploadContext& c) const { return &c >= m_loaderCtx && &c < m_loaderCtx + kLoaderContexts; }

    // Pick the caller's context. A loader scope on this thread (t_loaderCtx) comes first. A caller
    // inside an entered device-execution token uses the token's lane: the render half's
    // (IssueDeviceExecution) is [1], wherever it runs. Every other caller - the game half, loaders,
    // workers - gets [0] once FramePipeline::Configure has enabled the logical frame coordinator
    // (every mode does), and [1] before that, during device bring-up. Chosen by execution scope rather
    // than by an explicit argument because the ~40 staging call sites are spread across resource
    // loaders that have no idea which side they are on.
    UploadContext&       Upload()
    {
        if (t_loaderCtx) return *t_loaderCtx;
        return m_uploadCtx[m_deviceExecution.UploadLane(m_logicalFrameCoordinator ? 0u : 1u)];
    }
    const UploadContext& Upload() const
    {
        if (t_loaderCtx) return *t_loaderCtx;
        return m_uploadCtx[m_deviceExecution.UploadLane(m_logicalFrameCoordinator ? 0u : 1u)];
    }

    void                                                 FlushBatchStagingIfNeeded();

    // ---- Streaming texture upload (GraphicsDX12_StreamUpload.cpp) ----
    uint32_t NextTextureGeneration() { return m_textureGeneration.fetch_add(1, std::memory_order_relaxed) + 1; }
    // The request's shared half: @p res in @p state, merged by (@p keyTexture,
    // @p keyGeneration); @p streamTarget (a pool texture's flag) is set once it queues.
    StreamUpload::Result QueueStreamCopies(const Microsoft::WRL::ComPtr<ID3D12Resource>& res,
                                           D3D12_RESOURCE_STATES state, uint32_t keyTexture,
                                           uint32_t keyGeneration, const StreamUpload::Desc& d,
                                           bool* streamTarget);
    // CopyResource's rule for @p src onto @p dst: the same dimension, size, subresources and format.
    // FFmpeg's D3D12VA frames fit their texture exactly: they are the picture's size (no 1088 padding
    // at 1920x1080) and textures of their own (its texture-array mode is opt-in, and its decoder writes
    // subresource 0 regardless).
    static bool NativeCopyFits(const D3D12_RESOURCE_DESC& dst, const D3D12_RESOURCE_DESC& src);
    // EndFrame, before the first submission: the queue waits the frame's GPU
    // copies' producers need (RecordPendingStreamUploads collected them).
    void StreamUploaderBeforeSubmit();
    // Why a request from this thread / scope is refused, or nullptr.
    const char* StreamUploadCallRefused() const;
    StreamUpload::Result StreamUploaderReject(const char* why, uint32_t id);
    // Lazily, on the first QueueTextureUpload: the rings. False when they cannot be made.
    bool InitStreamUploaderLocked();
    void ShutdownStreamUploader();
    // A slot acquired for frame @p frame, its previous GPU use retired: the
    // ring resets and the frame opens (carried-over requests land). Every
    // acquire path calls it at the point retirement is known.
    void StreamUploaderOpenSlot(uint64_t frame, uint32_t slot);
    // EndFrame: the order check, then the taken slot's frame is submitted.
    void StreamUploaderEndFrame();
    // DestroyTexture of a stream target: its requests no Take handed out yet go.
    void StreamUploaderForget(uint32_t texture, uint32_t generation);

    // @p it's own copy as StreamRows: already in the copy's layout, whole rows of RowPitch.
    static StreamRows StreamRowsOf(const StreamUpload::Item<StreamCopy>& it);
    // Writes @p src to @p it's place, in the copy's layout: the slot's ring at
    // @p offset, or (@p needsStaging) a new one-off staging buffer.
    bool StreamUploaderPlaceBytesLocked(StreamUpload::Item<StreamCopy>& it, uint32_t slot, uint64_t offset,
                                        bool needsStaging, const StreamRows& src);
    void StreamUploaderPublishLocked() const;

    // ---- Descriptor heap allocators ----
    // Shared body of the four GetXxxHeapStats() accessors above.
    static DescriptorHeapStats MakeHeapStats(const DescriptorHeapAllocator& a)
    {
        DescriptorHeapStats s;
        s.used       = static_cast<uint32_t>(a.GetStaticUsed());
        s.capacity   = static_cast<uint32_t>(a.GetStaticCapacity());
        s.freeBlocks = static_cast<uint32_t>(a.GetStaticFreeBlocks());
        s.exhausted  = a.GetExhaustedCount();
        return s;
    }

    // ---- HDR scene target slots ----
    HdrTargetBinding&       Hdr()       { return m_hdrTargets[m_hdrCurrent]; }
    const HdrTargetBinding& Hdr() const { return m_hdrTargets[m_hdrCurrent]; }
    // A slot's pool record (resource, views, state); null while the slot has no target. The
    // no-argument form is the CURRENT slot's.
    TextureDX12*       HdrRecord(uint32_t slot);
    const TextureDX12* HdrRecord(uint32_t slot) const;
    TextureDX12*       HdrRecord()       { return HdrRecord(m_hdrCurrent); }
    const TextureDX12* HdrRecord() const { return HdrRecord(m_hdrCurrent); }
    // Release a slot's resource and views (deferred); with releaseId the pool
    // record goes back too (DestroyHdrTarget), without it the slot keeps the
    // record for the rebuild (CreateHdrTarget).
    void ReleaseHdrTarget(uint32_t slot, bool releaseId);

    // ---- Sampler dedupe (kSamplerDedupe) ----
    // Canonical key: EVERY field of RHI::SamplerDesc that reaches D3D12_SAMPLER_DESC, one per slot,
    // floats by exact bit pattern, built field by field (never memcmp the struct: two identical descs'
    // padding bytes need not be equal).
    //   [0] filter          [1] addressU        [2] addressV   [3] addressW
    //   [4] maxAnisotropy   [5] comparisonFunc  [6] borderColor
    //   [7] mipLodBias bits [8] minLod bits     [9] maxLod bits
    // std::array's operator== compares all ten exactly on every bucket probe, so a comparison sampler
    // can never merge into a non-comparison one however the hash behaves.
    static SamplerKey MakeSamplerKey(const RHI::SamplerDesc& desc) noexcept;

    // ---- Barrier translation and depth views ----
    // Translates one RHI barrier into up to kMaxBarriersPerTranslate D3D12 barriers in @p out and
    // applies the CPU-side state book-keeping (IMAGE -> TextureDX12::state, BUFFER ->
    // GPUBufferDX12::state). A depth-stencil barrier while the planes' states differ becomes one per
    // plane, each from that plane's book. Shared by PushBarrier and PushBarriers so the two cannot
    // drift. Returns how many it wrote; 0 (with @p out untouched) when the barrier cannot be translated.
    uint32_t TranslateBarrier(const RHI::GPUBarrier&  barrier,
                              D3D12_RESOURCE_BARRIER* out);
    // The DSV a bind uses - the writable one or the read-only variant RHI::DepthView names - and the
    // list's depth-tripwire record; the record's reset (a bind without a DSV, or a writable one nothing
    // checks); and the check every draw makes.
    D3D12_CPU_DESCRIPTOR_HANDLE BindDepthView(CommandListDX12& entry, const RHI::Texture& dsv,
                                              const RHI::DepthView& view);
    static void ClearDepthView(CommandListDX12& entry);
    void CheckDepthWrites(const CommandListDX12& entry) const;

    // ---- Bindless table, descriptor blocks, AS and RT-pipeline pools ----
    void CreateBindlessNulls();

    // Writes @p kind's null view (with @p stride for a structured buffer) at
    // @p dst through the allocator. False for a kind with no null view.
    bool WriteNullDescriptor(uint8_t kind, uint32_t stride, D3D12_CPU_DESCRIPTOR_HANDLE dst) const;

    // Puts @p block in the pool and fills @p out.
    void RegisterDescriptorBlock(const DescriptorAllocation& block, uint32_t count, RHI::DescriptorBlock& out);

    // The resource behind an AccelerationStructure handle (TranslateBarrier's UAV barriers).
    ID3D12Resource* LookupAccelerationStructure(const RHI::GPUResource& as) const;
    // Empties @p as's pool entry into @p out (resource, query UAV, query readback),
    // frees the id and resets @p as.
    void TakeAccelerationStructure(RHI::AccelerationStructure& as,
                                   Microsoft::WRL::ComPtr<ID3D12Resource> (&out)[3]);

    void ReportBindlessMismatch(uint32_t heapIndex, uint8_t heldKind, uint32_t heldStride,
                                uint8_t wantKind, uint32_t wantStride, const char* what);
    // One [BINDLESS] line: how many shader-visible descriptors hold each kind.
    // At the first EndFrame (boot) and at shutdown.
    void LogBindlessKinds(const char* when) const;

    // ---- The bindless debug lane (-bindless_validate) ----
    void CreateBindlessValidation();
    void PollBindlessValidation(ID3D12GraphicsCommandList* cmd);   // EndFrame
    void FinishBindlessValidation();                               // shutdown, GPU idle
    void ReadBindlessLog(const uint32_t* log, bool final);

    // ---- GPU profiler (timestamp queries) ----
    // Fold the want count together with any direct write into the flag the
    // profiler actually reads. Called once per frame at the top of BeginFrame,
    // before anything tests `enabled`.
    void ReconcileGPUProfilerEnable()
    {
        if (m_gpuProfiler.enabled != m_gpuProfilerPublished)
            m_gpuProfilerDirectWant = m_gpuProfiler.enabled;   // someone else wrote it
        const bool want = m_gpuProfilerDirectWant
                       || (m_gpuProfilerWants.load(std::memory_order_relaxed) > 0);
        m_gpuProfiler.enabled   = want;
        m_gpuProfilerPublished  = want;
    }

    // ---- Core DX12 objects ----
    // Core DX12 objects. Declaration order determines reverse-destruction order. Required DXGI
    // release sequence: renderTargets -> swapChain -> queues -> device -> factory (the first two are the
    // surface's, released by SurfaceDX12::Release before the destructor reaches any of these).
    Microsoft::WRL::ComPtr<IDXGIFactory6>             m_factory;
    // The adapter the device was created ON. Held for the process lifetime so
    // late consumers (PSO-cache fingerprint, VRAM budget queries, crash report)
    // never have to re-enumerate and risk picking a different one.
    Microsoft::WRL::ComPtr<IDXGIAdapter1>             m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    // One command queue per queue type: [GRAPHICS=0, COMPUTE=1, COPY=2]
    // Outlive the swap chain (the surface's, released before the destructor
    // resets these).
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_queues[3];
    // The main surface: the swap chain and everything about presenting through
    // it (Graphics/SurfaceDX12.h). Constructed by the constructor, DESTROYED
    // EXPLICITLY by the destructor - right after Streamline shuts down, while
    // the queues, the descriptor heaps and every pool are still alive - so its
    // place in this member list decides nothing.
    std::unique_ptr<SurfaceDX12>                      m_surface;
    // CPU-GPU sync fence (graphics queue only, frame pacing)
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    // Monotonic graphics-queue signal counter. Atomic because EndFrame and the hard-sync helpers
    // (FlushAndWait, WaitForPreviousFrame) both pre-increment it, and a torn or lost increment hands
    // out an already-signalled value, turning the wait built on it into a silent no-op.
    std::atomic<uint64_t>                             m_fenceValue { 0 };
    // Held across the INCREMENT AND THE SIGNAL together, which the atomic alone
    // does not give. Two threads can each take a fresh value atomically and
    // then Signal in the opposite order: the queue ends up setting the fence to
    // N after N+1, GetCompletedValue() goes BACKWARDS, and a waiter that had
    // already been released by N+1 is the least of it - WaitSlotRetired then
    // decides a slot is still in flight forever. Every signal of m_fence goes
    // through SignalGraphicsFence().
    std::mutex                                        m_fenceSignalMutex;

    // ---- Upload-ring slots (the game/render split) ----
    // Derived from the MONOTONIC App frame counter, not the swap chain: the DXGI index only advances at
    // Present, and the game half reaches frame F+1 before the render half has presented F. Rings do not
    // care WHICH slot they use, only that every user of a frame agrees and the slot's previous tenant
    // (frame F-3) has retired.
    // ATOMIC, not thread_local: VideoQuadPass::Execute and GBufferPass's chunk workers read the render
    // slot on TaskSystem workers, where a thread_local would read 0 and silently alias slot 0.
    std::atomic<uint32_t>                             m_uploadSlotRender { 0 };
    // ATOMIC: under -animoverlap the Animation barrier sits past AcquireUploadSlot, so this STORE lands
    // inside the window the lane reads in. Relaxed on both sides: nothing is published through it - the
    // slot's contents are ordered by m_uploadSlotOpenFrame and m_slotFence (release/acquire).
    std::atomic<uint32_t>                             m_uploadSlotGame   { 0 };
    // WHICH frame's upload window is open (UINT64_MAX = closed): opened by AcquireUploadSlot, closed
    // by EndFrame. A bool would be timing-dependent once threaded: AcquireUploadSlot(N) runs on the game
    // side while EndFrame(N-1) may still be closing on the render side, and a late close would shut N's
    // window (a mesh registered that frame renders one frame late, a freed one ghosts for one). Stamped,
    // EndFrame closes only the window it opened. IGraphicsDevice::IsUploadSlotWritable explains why a
    // slot/frame comparison cannot answer this.
    std::atomic<uint64_t>                             m_uploadSlotOpenFrame { UINT64_MAX };
    // ATOMIC: written by AcquireUploadSlot on the game thread, read by
    // BeginFrame and the SLOTPARITY block on the render thread.
    std::atomic<uint64_t>                             m_gameFrame        { 0 };
    // The same two numbers as ONE value, (gameFrame << 8) | gameSlot, stored last with release.
    // m_uploadSlotGame and m_gameFrame are one logical fact the render half reads from another thread;
    // sampled separately, a reader can see the NEW slot beside the OLD frame (the slot is stored first)
    // and conclude frame F was written to slot F+1's ring. Reading one atomic removes the tear.
    std::atomic<uint64_t>                             m_gameSlotStamp    { 0 };
    // The frame the RENDER half is working on, set from its packet BEFORE BeginFrame (SetRenderFrame).
    // In serial mode nothing sets it and BeginFrame falls back to the game frame, the same number.
    std::atomic<uint64_t>                             m_renderFrameReq   { 0 };
    std::atomic<bool>                                 m_renderFrameReqSet{ false };
    // Largest game-minus-render frame lag BeginFrame has ever observed. Read at
    // shutdown. Its whole job is to make an unexercised oracle visible.
    uint64_t                                          m_maxObservedFrameLag { 0 };
    // The frame BeginFrame..EndFrame is building: m_renderFrameReq when the render half named one,
    // else the game frame, minus -slotlag. Atomic because the GAME side reads it on every Destroy* to
    // stamp the release queue, and that read must not tear against the render side's store.
    std::atomic<uint64_t>                             m_renderFrame      { 0 };
    // The highest frame stamp the GPU has provably finished because of a HARD SYNC, rather than
    // because kFrameCount more frames went by. Raised by WaitForPreviousFrame (which fences every queue,
    // then drains the release bucket) and read by GetRetiredFrame. Holds m_renderFrame inside a frame
    // and m_renderFrame + 1 outside one: inside, it must stay strictly below the stamp a release taken
    // now would carry, or a release queued after the sync (same frame, work not yet submitted) reads
    // as retired. Raised through a CAS loop, so it never decreases.
    std::atomic<uint64_t>                             m_hardSyncedFrame  { 0 };

    // Per-slot retirement record, published by EndFrame(F) at [F % kFrameCount] and consumed by
    // AcquireUploadSlot on the game side.
    // framePlus1 is the SENTINEL and the publication flag in one: 0 = never used, otherwise F+1 so frame
    // 0 differs from unused. Written LAST with release, read FIRST with acquire, so a reader that sees
    // a frame also sees its three fence values.
    // Do NOT use ~0ull as the sentinel: the drain predicate is a <= comparison that ~0ull passes, which
    // would free every pending resource in the first three frames with no fence proven.
    FrameSlotFence                                    m_slotFence[kFrameCount];

    // Kill switch (-slotkey=dxgi): the upload slot IS the back-buffer index instead of
    // renderFrame % kFrameCount. Resolved once at device init and never flipped, so slot aliasing
    // (skinned-mesh flicker) can be bisected to "the key" without a rebuild. Serial only:
    // -rendermode=graph, -animoverlap and AcquireUploadSlotAsync refuse it, because the back-buffer index
    // is not a function of the frame counter.
    bool                                              m_slotKeyDxgi { false };
    // -slotlag=N: the render half reports itself N frames behind the game half. Fault injection for
    // BeginFrame's [SPLITCHECK][SLOTPARITY] checks, which cannot fire while both halves are on the same
    // frame. It DOES make the render side read another frame's upload rings: a lagged run renders wrongly
    // on purpose (a diagnostic, not a mode).
    uint64_t                                          m_slotLag { 0 };

    // DX12_SLOTAUDIT=1: report every GetFrameIndex() caller that runs OUTSIDE a frame, i.e. every
    // game-phase site that should ask GetUploadSlot(). A plain member, not a function-local static: it
    // is tested on the hottest accessor in the engine, where a static would add MSVC's thread-safe-init
    // guard.
    bool                                              m_slotAudit { false };

    // -animoverlapverify: the mirror of m_slotAudit. Plain member for the same
    // reason - GetUploadSlot() is on the hot path of every per-frame ring.
    bool                                              m_gameSlotAudit { false };
    // -animoverlap only. Read on main inside AcquireUploadSlotAsync, written
    // once at init before the lane exists.
    bool                                              m_concurrentUploadAcquire { false };
    // Every fire, not just the first per caller: the count is what a pack can
    // bound, the log line is what a human reads.
    mutable std::atomic<uint32_t>                     m_gameSlotFromRender { 0 };

    // Set true by WaitForNextFrameSlot, cleared by BeginFrame right after its
    // own WaitForNextFrameSlot call (NOT by EndFrame). BeginFrame uses
    // it to skip a duplicate slot-fence wait when the caller already prepared
    // the slot early (so Animation phase can safely write per-frame upload
    // buffers before BeginFrame). Idempotent within a single frame.
    bool                                              m_frameSlotPrepared { false };
    // Set by BeginFrame, cleared by EndFrame via a scope guard: THE frame-boundary predicate (the
    // FRAME-BOUNDARY CONTRACT at the top of this file). The scope guard is not style: EndFrame ends in
    // ThrowIfFailed(Present()), and one device removal there would otherwise leave this latched true,
    // so every guarded entry point would refuse for the rest of the session.
    std::atomic<bool>                                 m_inFrame { false };
    bool                                              m_tearingSupported { false };

    bool                                              m_meshShaderSupported { false };
    bool                                              m_dxrSupported        { false };
    bool                                              m_inlineRtSupported   { false };
    bool                                              m_native16BitSupported{ false };
    bool                                              m_int64HeapAtomics{ false };   // GetCapabilities().int64HeapAtomics
    // Resource Binding Tier 3 (SM 6.6 ResourceDescriptorHeap). A hard requirement, checked at init
    // with m_shaderModel66: the shared root signatures set HEAP_DIRECTLY_INDEXED, which needs this tier
    // at serialization time. Also GetCapabilities().unboundedDescriptorArrays.
    bool                                              m_resourceBindingTier3{ false };
    // HighestShaderModel >= 6.6. Cached only so the boot GPU report can print
    // it next to the other caps (the hard requirement is checked at init).
    bool                                              m_shaderModel66       { false };
    // DRED actually armed on this device (IsDREDActive()).
    bool                                              m_dredActive          { false };
    // ReportDeviceRemoved ran: the threadpool fence callback and the owner can
    // both notice the same removal.
    std::atomic<bool>                                 m_deviceRemovedReported { false };

    // ---- Selected adapter identity (see GetAdapterDesc) ----
    // Zero-initialised: if adapter enumeration ever fails before the throw, the PSO-cache fingerprint
    // made from these is all-zero and never matches, which is the safe direction (rebuild, do not load
    // a foreign blob).
    DXGI_ADAPTER_DESC1                                m_adapterDesc         {};
    LUID                                              m_adapterLuid         {};
    uint64_t                                          m_driverVersion       { 0 };
    D3D_FEATURE_LEVEL                                 m_featureLevel        { D3D_FEATURE_LEVEL_11_0 };

    // ---- Deferred release ----
    // FRAME-STAMPED, not slot-keyed. A slot ring answers "is the GPU done with it" only while one
    // thread owns both halves of the frame: once the game thread runs ahead, a resource it destroys in
    // frame k can still be referenced by render frames k-1 and k-2, and a ring keyed on either side's
    // slot frees it after k-3 - a silent use-after-free. Each batch carries the frame its newest possible
    // referencer belongs to (ReleaseStampFrame(), one beyond the render side's frame, conservative under
    // any interleaving), and BeginFrame frees only what is provably retired. Stamps are monotonic, so
    // the deque stays sorted and the drain is a front pop.
    std::deque<PendingRelease>                        m_pendingReleases;

    // Fence-keyed deferred releases: each entry holds a queue index + fence value the GPU must reach
    // before the resource is safe to free. Drained from BeginFrame after the frame-stamped queue,
    // removing entries whose value <= m_queueFences[queueIndex]->GetCompletedValue(). For DDGI's
    // async-compute BLAS pipeline, so resources can outlive kFrameCount without a WaitForPreviousFrame
    // stall.
    std::deque<FenceKeyedRelease>                     m_fenceKeyedReleases;

    std::mutex                                        m_deferredMutex;

    // Destroy* calls refused (an id outside the pool, or a buffer / texture destroyed already) and
    // released ids DrainReleasesUpTo found on a free list already. Each refusal logs once per site;
    // these count every one, for the [POOL] shutdown line.
    std::atomic<uint32_t>                             m_destroyRefusals{ 0 };
    std::atomic<uint32_t>                             m_freeListDupes{ 0 };
    // -rhi_lifetime_probe_unguarded (harness): both refusals off, and CreateTexturePlaced's bounds
    // refusal - the lifetime probe's reverse arm.
    bool                                              m_lifetimeGuardsOff = false;

    // ---- EndFrame submit scratch ----
    // Reused across frames so the topological submit sort allocates nothing in steady state.
    std::vector<int>                m_submitInDegree;
    std::vector<uint32_t>           m_submitReady;    // vector-as-queue (head index kept locally)
    std::vector<uint32_t>           m_submitOrder;
    std::vector<ID3D12CommandList*> m_submitBatch;    // consecutive same-queue CLs for one ExecuteCommandLists
    std::vector<uint32_t>           m_submitBatchIds; // pool ids parallel to m_submitBatch

    // ---- Barrier journal / frame snapshot ----
    // m_journalArmed: a request for the NEXT BeginFrame (ArmFrameJournal, any thread). m_journalOn:
    // latched by BeginFrame, read on every barrier by whichever thread records, cleared by EndFrame once
    // the snapshot is built. The scratch below is EndFrame's, filled by its submit loop only while
    // m_journalOn; m_frameSnapshot waits for TakeFrameSnapshot.
    std::atomic<bool>                              m_journalArmed{ false };
    std::atomic<bool>                              m_journalOn{ false };
    std::vector<GfxDiag::FrameSnapshot::Batch>     m_journalBatches;
    std::vector<uint32_t>                          m_journalBatchOf;   // per pool slot
    std::vector<std::vector<GfxDiag::FrameSnapshot::Wait>> m_journalWaits;   // per pool slot
    std::shared_ptr<const GfxDiag::FrameSnapshot>  m_frameSnapshot;

    uint32_t                                          m_rtvDescriptorSize{ 0 };
    // Per-queue GPU-GPU cross-queue synchronization fences
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_queueFences[3];
    // Last value signalled on each queue. Read by the release drain and WaitSlotRetired, written by
    // every submit, so it crosses threads the moment submission does.
    std::atomic<uint64_t>                             m_queueFenceValues[3]{};

    // ---- Upload contexts: one per side of the game/render split ----
    // Staging copies (CreateBuffer / CreateTexture with initialData) need an allocator, list and fence
    // per side: two threads recording into one list interleave into nonsense, and a shared fence counter
    // hands out already-signalled values, so FlushUploadAndWait returns before the copy lands. The
    // contexts, picked by caller, still submit to the same queues, which is safe because every flush
    // CPU-waits its OWN fences before returning.
    // Each context has two lists. A copy into a resource CreateBuffer / CreateTexture has just made
    // (nothing else can reference it) goes on `copyList`, executed on the COPY queue, so the flush waits
    // for that copy alone instead of for every frame queued on the graphics queue. Legal without a
    // barrier: the resource is created in COMMON, the copy queue promotes it to COPY_DEST and it decays
    // to COMMON when that ExecuteCommandLists completes; buffers promote on any later use, and a texture
    // goes this way only when its intended state is promotable (shader resource or copy). The ledger
    // then records that intended state: a barrier on a resource in COMMON may name COMMON or any
    // promotable state as its BeforeState. Everything touching an existing resource (UpdateTexture,
    // CopyD3D12ResourceToTexture) stays on `list`; -upload_directqueue puts the creation copies back on
    // `list`.
    // The buffer-heap sub-allocation cursor is per context too: shared, one side's placement would
    // advance the other's offset, and one side's EndBufferHeapBatch would Reset a heap the other is still
    // placing into.
    UploadContext                                        m_uploadCtx[2];

    // The LOADER contexts. A TaskSystem worker's BeginLoaderUploads takes a free one for its scope
    // (waiting while both are taken), so a loader never records into a frame lane's lists or moves its
    // batch / heap state; the scope's End flushes and waits on that worker. Same lists and fences as
    // m_uploadCtx, created with them.
    UploadContext                                        m_loaderCtx[kLoaderContexts];
    bool                                                 m_loaderCtxBusy[kLoaderContexts] = {};
    std::mutex                                           m_loaderMutex;
    std::condition_variable                              m_loaderCv;
    // Off under -upload_sync (the load_upload_async pack's reverse arm) and
    // -upload_directqueue (the creation copies' graphics-list path). Read once in
    // LoadAssets.
    bool                                                 m_loaderUploads = false;

    // Set by EnableLogicalFrameCoordinator (FramePipeline::Configure, every mode): Upload() then gives
    // a caller in neither a loader scope nor a device-execution token m_uploadCtx[0] instead of [1].
    bool m_logicalFrameCoordinator = false; // configured before any frame/job starts

    // Creation copies on the COPY queue (false under -upload_directqueue). Read once in LoadAssets,
    // before the first upload.
    bool                                                 m_uploadCopyQueue = true;

    // ---- Streaming texture upload (GraphicsDX12_StreamUpload.cpp) ----
    // The queue's rules (window, ring, merging, lost frames) are Graphics/StreamUploadQueue.h; this is
    // the GPU half: one persistently mapped committed UPLOAD buffer per upload slot, the copies recorded
    // into a dedicated "StreamUpload" list at the start of RenderAsync, and the textures' identity
    // checked at record time.
    StreamUploader m_stream;
    // The record's creation stamp (TextureDX12::generation); never 0.
    std::atomic<uint32_t> m_textureGeneration{ 0 };

    // ---- Descriptor heap allocators ----
    DescriptorHeapAllocator m_rtvAllocator;
    DescriptorHeapAllocator m_cbvSrvUavAllocator;
    DescriptorHeapAllocator m_dsvAllocator;
    DescriptorHeapAllocator m_samplerAllocator;

    // ---- HDR scene target slots ----
    // HDR offscreen render targets, one binding per slot (kMaxHdrTargets); slot 0 is the main
    // viewport.
    std::array<HdrTargetBinding, kMaxHdrTargets> m_hdrTargets{};
    uint32_t                               m_hdrCurrent{ 0 };
    // Push/Pop bookkeeping. The depth is atomic because the consumer guards read it wherever their
    // site runs (Renderer's AA/upscaler block runs on a render worker), while Push/Pop write it only
    // from the serial thread that owns device frames, outside any Phase B window.
    std::atomic<int>                       m_hdrScopeDepth{ 0 };
    uint32_t                               m_hdrScopeStack[kMaxHdrTargets]{};

    // Set only while RenderGraph Phase B records passes on ParallelFor workers. The HDR state tracker
    // (the slot record's TextureDX12::state) is a plain shared member: a state CHANGE recorded in that
    // window is a data race, seen as a mismatched-before-state ResourceBarrier on
    // 'GraphicsDX12.HdrEditorViewport'. Guard only (WarnOnRacyHdrStateChange).
    std::atomic<bool>                      m_parallelPassRecording{ false };

    // ---- Device thread and execution ----
    // Thread that constructed the device. CreateTexturePlaced checks against it: placed resources
    // live in the shared TransientAliasArena heap, whose disjointness argument assumes creation in a
    // serial phase (TransientAliasArena.h rule 3), and this tripwire makes a violation show in the log.
    std::thread::id                        m_deviceThread;
    // Native owner is the issuance/bootstrap guard. Active device access is
    // authorized by the logical token, re-entered for each owner resume.
    std::atomic<std::thread::id>           m_deviceFrameThread;
    Sys::ExecutionDomain                  m_deviceExecution;
    Sys::EventTaskQueue                   m_gpuCompletions;
    std::function<void()>                  m_parkHook;

    // ---- Command-list pool ----
    // Command-list pool: CommandListDX12 objects come from m_cmdAllocator (stable addresses);
    // m_commandListCount resets to 0 each frame and entries are reused, not freed.
    // m_commandLists is a FIXED-CAPACITY array on purpose: BeginCommandList fills slots under
    // m_commandListMutex while render workers index it from GetPoolEntry with NO lock, so the storage
    // must never reallocate. Do not swap in a std::vector (reallocates past its reserve) or a std::deque
    // (MSVC's operator[] reads the block map that push_back's _Growmap reallocates). Overflow is a hard,
    // logged error; 256 is ~4x the measured peak (~60 lists in a fully loaded frame).
    Allocator::BlockAllocator<CommandListDX12, 64>        m_cmdAllocator;
    std::array<CommandListDX12*, kMaxCommandLists>        m_commandLists{};
    // Highest slot ever constructed (never decreases). Distinct from
    // m_commandListCount, which resets to 0 every frame.
    uint32_t                                               m_commandListHighWater{ 0 };
    std::atomic<uint32_t>                                  m_commandListCount{ 0 };
    // Most lists USED in a single frame (EndFrame updates it). Distinct from m_commandListHighWater,
    // the largest slot ever ALLOCATED, which never comes back down after one heavy frame grows the pool;
    // do not substitute one for the other. Read by the editor's pool watermark panel.
    uint32_t                                               m_commandListPeak{ 0 };
    std::mutex                                  m_commandListMutex;

    // ---- Resource pools ----
    // Resource pools: index == GPUResource::handleId. Reserved to kMax*Pool in LoadAssets and NEVER
    // grown past it: recording workers index m_texturePool / m_bufferPool / m_shaderPool without
    // m_resourceMutex (TranslateBarrier, BindResource, SetRenderTargets, CreatePipelineState's bytecode
    // fetch), so a reallocating push_back would dangle their references. The Create* push sites refuse
    // at the cap (ENGINE_CHECK_ONCE + false). m_resourceMutex guards concurrent Create* / Destroy*.
    // m_psoPool is a Sys::StableAppendPool (System/StableAppendPool.h): its chunk-pointer table is a
    // fixed array, so a lock-free BindPipelineState reading any index below size() never meets freed
    // storage, which neither a vector (reallocation) nor a std::deque (push_back reallocates its block
    // map) guarantees. unique_ptr of the forward-declared struct is fine: the pool is cleared and
    // destroyed in GraphicsDX12.cpp, where the type is complete.
    // m_shaderPool and m_psoPool are APPEND-ONLY: DestroyShader / DestroyPipelineState free the payload
    // (the DXIL, the driver PSO) and leave the cell. RHI::Shader / RHI::PipelineState have no generation
    // counter and are copied by value into pass members, so a recycled index would let a stale copy bind
    // a DIFFERENT live pipeline with no debug-layer message. A retired cell costs 8-16 bytes.
    std::vector<struct GPUBufferDX12>                   m_bufferPool;
    std::vector<struct TextureDX12>                     m_texturePool;
    std::vector<struct ShaderDX12>                      m_shaderPool;
    // Said ONCE, at 90% of kMaxShaderPool. This is the only pool whose count is
    // a one-way ratchet (DestroyShader retires slots, it does not recycle
    // them), so it is the only one where "we are close" is actionable before
    // the hard refusal in CreateShader. Guarded by m_resourceMutex like the
    // pool it watches.
    bool                                                 m_shaderPoolHeadroomLogged = false;
    // The pipeline layouts' root signatures. Retired, never recycled, like the PSO pool; read under
    // m_resourceMutex (LookupPipelineLayout).
    std::vector<struct PipelineLayoutDX12>              m_layoutPool;
    Sys::StableAppendPool<std::unique_ptr<struct PipelineStateDX12>> m_psoPool;

    // ---- Soft ceiling for m_psoPool ----
    // The once-only warning at 90% of kSoftMaxPsoPool has been said (PublishPipelineState). Guarded by
    // m_resourceMutex.
    bool                                                 m_psoPoolHeadroomLogged = false;
    // ---- Resource pools ----
    // Sampler pool: each entry is a live DescriptorAllocation; index == descriptor index. Reserved to
    // kMaxSamplers and a HARD cap for the same reason as the buffer/texture pools: SetDynamicSampler
    // (SamplerHeapIndex) indexes it and reads size() on recording workers WITHOUT m_resourceMutex. The
    // reserve removes only the REALLOCATION hazard; that unsynchronised size() read is safe because
    // every CreateSampler runs from Pass::Init, serially in RenderGraph::Compile, before any Execute
    // records. A graph compiled off the device-frame thread would break that invariant.
    std::vector<DescriptorAllocation>      m_samplerPool;

    // ---- Sampler dedupe (kSamplerDedupe) ----
    // key -> existing m_samplerPool index. Lookup and insert are TWO separate
    // m_resourceMutex critical sections (the descriptor allocation between them
    // runs unlocked), so two identical descs racing through CreateSampler is
    // benign, not impossible: try_emplace keeps the first index canonical and
    // the loser's descriptor stays valid but unreachable from the map. In
    // practice unreachable anyway - every CreateSampler runs from Pass::Init,
    // serially inside RenderGraph::Compile. Only grows (append-only pool, so an
    // entry can never go stale).
    std::unordered_map<SamplerKey, int, SamplerKeyHash> m_samplerDedupe;
    // CreateSampler calls answered from m_samplerDedupe (descriptors NOT
    // allocated). Reported on the shutdown pool high-water line.
    uint32_t                               m_samplerDedupeHits{ 0 };

    // ---- Resource pools ----
    // Persistent descriptor tables (e.g. shadow cascade SRV tables), keyed by GPU handle.
    std::unordered_map<uint64_t, DescriptorAllocation> m_descriptorTablePool;
    // Free-lists: slots from Destroy* calls that can be reused by the next Create*.
    std::vector<uint32_t>                  m_bufferFreeList;
    std::vector<uint32_t>                  m_textureFreeList;
    mutable std::mutex                     m_resourceMutex;   // mutable: ComputePSOName is const

    // ---- Root and command signatures ----
    // The shared graphics root signature (RHI::Slots::Gfx), created once in LoadAssets().
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_defaultRootSignature;

    // The shared compute root signature (RHI::Slots::Cmp), created once in LoadAssets().
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_computeRootSignature;

    // Command signature for ExecuteIndirect (root constants + DrawInstanced).
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_indirectCommandSignature;
    // The same layout with DISPATCH_MESH (meshlet groups); null without mesh-shader support.
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchMeshCommandSignature;
    // A bare compute dispatch (DispatchComputeIndirect).
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_dispatchCommandSignature;

    // ---- Bindless table, descriptor blocks, AS and RT-pipeline pools ----
    // Bindless texture table: a contiguous shader-visible block of kMaxBindlessTextures SRVs, bound at
    // RHI::Slots::Gfx::BindlessTex (t0 space2). Slot == m_texturePool's handleId, so m_textureFreeList
    // handles recycling and CreateTexture's CopyDescriptorsSimple overwrites a reused slot; no separate
    // allocator or high-water counter.
    DescriptorAllocation m_bindlessTexTable;

    // The null descriptors, one per kind, allocated right after the table so their heap indices are
    // BINDLESS_NULL_*.
    DescriptorAllocation m_bindlessNulls;

    // The debug targets' fixed slots, right after the nulls: BINDLESS_DEBUG_*. m_bindlessDebugBase is
    // BINDLESS_DEBUG_BASE once they exist (~0 = not made; SetFixedDebugUav / SetFixedDebugBuffer
    // refuse).
    DescriptorAllocation m_bindlessDebugSlots;
    uint32_t             m_bindlessDebugBase = ~0u;

    // Nulls of the kinds without a fixed slot, made on first use. Key:
    // stride << 8 | kind. Guarded by m_bindlessNullMutex.
    std::mutex m_bindlessNullMutex;
    std::unordered_map<uint32_t, DescriptorAllocation> m_bindlessLazyNulls;
    // The allocations behind RHI::DescriptorBlock handles (CreateNullStructuredSrvBlock,
    // AllocateDescriptorBlock), by handleId; a released id is reused. Guarded by m_descriptorBlockMutex.
    std::mutex                        m_descriptorBlockMutex;
    std::vector<DescriptorAllocation> m_descriptorBlocks;
    std::vector<uint32_t>             m_freeDescriptorBlockIds;

    // The memory behind RHI::AccelerationStructure handles, by handleId (a released id is reused),
    // with a compacted-size query's two transient buffers while one is pending. Every access holds
    // m_asMutex.
    mutable std::mutex            m_asMutex;
    std::vector<AccelStructDX12> m_asPool;
    std::vector<uint32_t>         m_asFreeIds;

    // The state objects behind RHI::RaytracingPipeline handles, by handleId (a released id is reused),
    // each with its shader table and the addresses DispatchRays hands D3D12. Guarded by
    // m_rtPipelineMutex.
    std::mutex                           m_rtPipelineMutex;
    std::vector<RaytracingPipelineDX12> m_rtPipelines;
    std::vector<uint32_t>                m_rtPipelineFreeIds;
    // MakeBindlessHandle's refusals: counted, and each (what, slot, kind)
    // reported once.
    std::atomic<uint32_t> m_bindlessMismatches { 0 };
    std::mutex m_bindlessReportMutex;
    std::unordered_set<uint64_t> m_bindlessReported;

    bool m_bindlessKindsLogged = false;

    // ---- The bindless debug lane (-bindless_validate) ----
    // Shaders compiled with BINDLESS_VALIDATE=1 check every bindless.hlsli read against the record,
    // mirrored into m_bindlessTable (BINDLESS_VALIDATE_TABLE), and append a refused read to
    // m_bindlessLog (BINDLESS_VALIDATE_LOG). The log is never cleared: EndFrame copies it into the frame
    // slot's slice of m_bindlessLogReadback and reads the slice the slot's last frame left there.
    // CreateBindlessValidation runs right after CreateBindlessNulls, the only place that puts the two
    // descriptors at their fixed indices.
    bool m_bindlessValidate = false;
    DescriptorAllocation m_bindlessValidateSlots;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bindlessTable;          // UPLOAD, mapped
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bindlessLog;            // DEFAULT, UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_bindlessLogReadback;    // READBACK, kFrameCount slices
    uint32_t m_bindlessRefusedSeen = 0;

    // ---- GPU profiler (timestamp queries) ----
    // Outstanding want count (see AddGPUProfilerWant).
    std::atomic<uint32_t> m_gpuProfilerWants { 0 };
    // What ReconcileGPUProfilerEnable last published, and the last value it attributed to a direct
    // writer. Together they let a direct write to GPUProfiler::enabled (the editor's, or any tool's)
    // be detected as a DIVERGENCE from what was published and treated as that owner's want rather than
    // clobbered. Without the pair, OR-ing a want into the flag would make it sticky: the previous
    // frame's own `true` would read as someone else's request and the profiler could never be released.
    bool     m_gpuProfilerPublished  = false;
    bool     m_gpuProfilerDirectWant = false;

    GPUProfiler  m_gpuProfiler;
    // A region's list's counts at its begin (and that list), so its end can annotate what the list
    // recorded in between.
    Diag::Prof::GpuCounters m_regionBegin[GPUProfiler::kMaxRegions];
    uint32_t                m_regionBeginList[GPUProfiler::kMaxRegions] {};
    // ---- Debug markers, frame constants and RenderDoc capture ----
    bool         m_debugMarkers = false;
    // This frame's constants, reset per frame slot in BeginFrame (Graphics/TransientConstants.h);
    // [CBALLOC] reports its use.
    TransientConstantAllocator m_transientConstants;
    // [GPUMARKERS] report, counted only while markers are on: events opened and
    // command lists renamed this frame, and regions EndFrame had to close itself
    // because their End never came on the same list (the capture tree then nests
    // them wrong). Recording threads add; EndFrame reads and resets.
    std::atomic<uint32_t> m_markerEvents  { 0 };
    std::atomic<uint32_t> m_markerNamed   { 0 };
    std::atomic<uint32_t> m_markerUnclosed{ 0 };
    uint32_t              m_markerFrames         = 0;
    bool                  m_markerUnclosedWarned = false;
    // -rdcapture=N: RenderDoc's in-app API (RENDERDOC_API_1_1_2*) when RenderDoc
    // launched this process, else null; the frame to capture; whether the
    // written capture has been logged. Harness builds only (GraphicsDX12.cpp).
    void*                 m_rdocApi         = nullptr;
    uint64_t              m_rdCaptureFrame  = 0;
    bool                  m_rdCaptureLogged = false;
#ifdef TRACY_ENABLE

    struct TracyGpuState;

    // Tracy GPU profiling state (pimpl: the full type lives in GraphicsDX12.cpp so this widely included
    // header stays free of TracyD3D12.hpp). Created in the ctor, destroyed in the dtor while the device
    // and queues are still alive.
    std::unique_ptr<TracyGpuState> m_tracy;
#endif

    void AcquirePsoLibName(uint64_t psoNameHash);
    void ReleasePsoLibName(uint64_t psoNameHash);

    // ---- The pipeline manifest (GraphicsDX12_Precompile.cpp) ----
    // A boot precompile in flight: its manifest, root signatures, workers and tallies.
    struct PipelinePrecompile;
    struct RootSigBlob
    {
        uint64_t             hash = 0;
        std::vector<uint8_t> bytes;
    };
    // The serialized blob @p rs was created from, so a recorded pipeline can name its root signature
    // and a replay rebuild it. Every root signature the device creates registers here; a new one at a
    // reused address replaces the entry.
    void NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size);
    // Records a pipeline this device built or loaded under @p name when the harness's -pso_variants
    // asked (Graphics/PsoManifest.h): its root signature (registered by NoteRootSignature), the
    // @p stages' DXIL (StageSlot order) and @p state (graphics and mesh). @p buildUs: the driver's
    // create time, 0 for a library hit. A no-op when not recording.
    void RecordPipeline(Gfx::PsoManifest::Kind kind, const wchar_t* name, ID3D12RootSignature* rs,
                        const D3D12_SHADER_BYTECODE (&stages)[Gfx::PsoManifest::kStageCount],
                        const Gfx::PsoManifest::State* state, uint32_t buildUs);
    // One record of @p job, on a precompile worker: library hit, or create + store.
    void PrecompileOne(PipelinePrecompile& job, const Gfx::PsoManifest::Record& rec);
    // A Load(Graphics)Pipeline hit: m_psoLibHits, and a disk hit or a session dupe by whether this
    // session stored or loaded the name before.
    void NotePsoLibHit(uint64_t psoNameHash);
    // A StorePipeline's outcome: the name joins m_psoLibStoredNames; a failure
    // warns once per session (E_INVALIDARG = a duplicate name).
    void NotePsoLibStore(uint64_t psoNameHash, HRESULT hrStore);
    // Serializes the library to @p cacheFilePath (SavePSOLibrary's body). @p quietInUse: a file
    // another process holds is an INFO, not a WARNING (a checkpoint; the shutdown save retries).
    void WritePSOLibrary(const char* cacheFilePath, bool quietInUse);
    // Loads psoName from the library, else creates + stores it, under the name lease; counts
    // computeHits / computeMisses / computeCompiled and psoCreates (a create only). @p profile = false
    // on a thread that must not enter a profiler scope. False: the driver's create failed (@p hrOut
    // says how).
    bool CreateComputePSOViaLibrary(const wchar_t* psoName, uint64_t psoNameHash,
                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
                                    Microsoft::WRL::ComPtr<ID3D12PipelineState>& out,
                                    bool profile, HRESULT& hrOut);

    // Derives a stable wchar_t name for a PipelineState from its creation desc. Non-static: it reads
    // bytecode hashes from m_shaderPool and m_rootSigHash. Content-addressed, so names are stable across
    // runs (the disk PSO library reuses entries instead of growing every launch) yet change the moment
    // the DXIL or the binding layout does. desc.cacheKey, when set, is folded in as a seed only.
    void ComputePSOName(const RHI::PipelineStateDesc& desc,
                        wchar_t* outName, size_t maxChars) const;

    // Copies {data,size,hash} out of m_shaderPool under m_resourceMutex; false
    // (plus a one-time ENGINE_CHECK) for an out-of-range or DESTROYED handle.
    bool LookupShaderBytecode(const RHI::Shader* s, D3D12_SHADER_BYTECODE& out,
                              uint64_t* outHash, const char* stage);
    // The root signature of a pipeline layout handle, under m_resourceMutex;
    // false (plus a one-time ENGINE_CHECK) for an out-of-range or destroyed one.
    bool LookupPipelineLayout(const RHI::PipelineLayout& layout,
                              Microsoft::WRL::ComPtr<ID3D12RootSignature>& out);
    // Appends a built PSO to m_psoPool, then retires the handle outPSO held on entry (if any): the
    // overwrite-in-place contract of CreatePipelineState.
    bool PublishPipelineState(struct PipelineStateDX12&& internal,
                              RHI::PipelineState& outPSO, const char* kind);

    // Accumulated FNV-1a over every serialized root-signature blob this device
    // builds. Seeded with the FNV-1a 64 offset basis; CreateDefaultRootSignature
    // and CreateComputeRootSignature each fold their blob in. See
    // GetRootSignatureHash().
    uint64_t m_rootSigHash{ 14695981039346656037ull };

    // PSO library (optional: null until InitPSOLibrary is called, or on unsupported hardware)
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> m_psoLibrary;
    std::vector<uint8_t>                          m_psoLibraryData; // serialised bytes, kept alive

    // ---- The PSO library's own numbers, said once at shutdown ----
    // `[PSO] library hits=N misses=M compiled=K loadedBytes=B` is the pack-readable signal of PSO NAMING
    // DRIFT (a ComputePSOName change makes every warm start miss), since LOADPIPELINE_NAMENOTFOUND is
    // logged at Info so engineWarnings budgets do not depend on pso_cache.bin existing.
    // Scope: the GRAPHICS path and the mesh-shader stream path (ID3D12PipelineLibrary1::LoadPipeline /
    // StorePipeline under the same name lease). Compute PSOs use the library too but are counted on their
    // own (m_psoLibCompute*), so `misses == compiled` stays an equality over these paths.
    // HITS ARE NOT ONLY DISK HITS: StorePipeline fills the library as the run compiles, so a repeat
    // request for a stored psoName is a hit with nothing loaded from disk (a cold run can report
    // hits > 0). loadedBytes is the field that says whether the run was warm.
    // Atomic: threads creating DIFFERENT pipelines are here at once (the lease serializes one name only).
    std::atomic<uint32_t> m_psoLibHits{ 0 };     // Load(Graphics)Pipeline succeeded (disk OR in-session)
    std::atomic<uint32_t> m_psoLibMisses{ 0 };   // library present, Load(Graphics)Pipeline failed
    std::atomic<uint32_t> m_psoLibCompiled{ 0 }; // Create(Graphics)PipelineState after such a miss
    // The mesh (stream) PSOs' share of the three above: LoadPipeline hits / misses and the
    // CreatePipelineState after a miss. On the `[PSO] library` line as meshHits / meshMisses /
    // meshCompiled.
    std::atomic<uint32_t> m_psoLibMeshHits{ 0 };
    std::atomic<uint32_t> m_psoLibMeshMisses{ 0 };
    std::atomic<uint32_t> m_psoLibMeshCompiled{ 0 };
    // The compute pipelines' own three (CreatePipelineState's compute branch, the
    // CreatePipelineStates batch, CreateComputePipelineCached). On the `[PSO] library` line as
    // computeHits / computeMisses / computeCompiled.
    std::atomic<uint32_t> m_psoLibComputeHits{ 0 };
    std::atomic<uint32_t> m_psoLibComputeMisses{ 0 };
    std::atomic<uint32_t> m_psoLibComputeCompiled{ 0 };
    uint64_t m_psoLibBytes    = 0;   // bytes InitPSOLibrary actually took from disk
    // StorePipeline successes since the library was last written: CheckpointPSOLibrary writes only
    // when this is non-zero.
    std::atomic<uint32_t> m_psoLibStoresSinceSave{ 0 };

    // ---- The two halves of `hits` ----
    // Known at the call site (did this run itself store or load that psoName before?), for one hash-set
    // probe per graphics PSO:
    //   diskHits      the library had it and THIS RUN did not put it there: it came from pso_cache.bin.
    //                 EXACTLY 0 on a cold run (-pso_cache_dir at a per-run directory), so a non-zero
    //                 value there means the flag stopped working and the pack measures a warm cache.
    //   sessionDupes  an earlier CreatePipelineState in this run already asked for it: a DUPLICATE PSO
    //                 desc, a property of the pass roster rather than the cache. Non-zero on warm and
    //                 cold runs alike, so a warm pack can gate on it.
    uint32_t m_psoLibDiskHits    = 0;
    uint32_t m_psoLibSessionDupes = 0;
    // psoNames THIS RUN has already resolved once, by storing one it compiled or by loading one off
    // the disk image. A hit on one of these is a session dupe; the FIRST hit on a name is the disk hit.
    // Insert on the load path too: inserting only on compile makes sessionDupes 0 on a warm run (nothing
    // is compiled) and counts every repeat as a disk hit. Hashed rather than kept as strings:
    // ComputePSOName is content-addressed, so its 64-bit FNV collides no more readily than the name.
    std::unordered_set<uint64_t> m_psoLibStoredNames;
    // Its own mutex, NOT m_resourceMutex: CreatePipelineState takes that one
    // inside PublishPipelineState and this set is touched on either side of
    // that call, so sharing the lock would either nest or widen a critical
    // section that lock-free readers depend on being short.
    std::mutex                   m_psoLibNamesMutex;

    // ---- One thread at a time per psoName inside the library ----
    // ID3D12PipelineLibrary is thread-safe except that threads loading the SAME pipeline must
    // synchronize themselves (ID3D12Device1::CreatePipelineLibrary, "Thread Safety"): the library hands
    // every load of a name the object it first loaded, and two first loads racing corrupt that record. A
    // standalone 4-thread test gets distinct objects for one name, failed loads of present names and an
    // access violation within a few dozen rounds; serialized, 25,600 loads run clean. Render-graph
    // workers create pipelines lazily on a scene's first frame, so two can miss their PSOCache and Load
    // one name at once, and the loser's pipeline is freed under it (an AV in D3D12Core).
    // So CreatePipelineState LEASES the psoName for its whole load -> compile-on-miss -> store sequence:
    // m_psoLibInflight holds the name hashes a thread is inside the library for, and a second asker
    // waits on m_psoLibInflightCv; different names never wait. The waiter then LOADS what the first
    // stored instead of compiling again. Only leaf locks (m_psoLibNamesMutex, the logger's) are taken
    // under a lease - PublishPipelineState runs after release - so it cannot join a lock cycle.
    std::mutex                   m_psoLibInflightMutex;
    std::condition_variable      m_psoLibInflightCv;
    std::unordered_set<uint64_t> m_psoLibInflight;
    std::atomic<uint32_t>        m_psoLibNameWaits{ 0 };  // leases that had to wait; on the [PSO] line

    // ---- The pipeline manifest ----
    std::mutex                                               m_rootSigBlobMutex;   // guards m_rootSigBlobs
    std::unordered_map<ID3D12RootSignature*, RootSigBlob>    m_rootSigBlobs;
    // Main thread only: Start, the status and Finish are called from it; the workers hold their own
    // reference to the job.
    std::shared_ptr<PipelinePrecompile>                      m_precompile;
    RHI::PipelinePrecompileStatus                            m_precompileStatus;

#ifdef _DEBUG
    // D3D12 debug layer message callback cookie (0 = not registered)
    DWORD m_d3d12MessageCallbackCookie{ 0 };
#endif
};
