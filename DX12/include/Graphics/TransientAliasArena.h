#pragma once

// TransientAliasArena - ONE shared placement heap for frame-transient textures whose GPU
// lifetime windows are provably disjoint, so they alias the same bytes instead of each owning a
// committed allocation.
//
// Residents (each group packs its own textures from offset 0, so the groups overlap - that is the point):
//   A) RTReflectionPass  rawColor/hitDist/specIn/specOut/normalRough/viewZ/mv
//   B) XeGTAOPass        linearDepth(5-mip)/aoRaw/edges/aoFinal
//   C) VolumetricFogPass density/lighting/scattering (3D froxels). The scattering volume is also
//      read after VolFog's apply by every surface drawn later in the graph (fog_apply.hlsli:
//      Transparent, VideoQuad, Particle, Trail, Tracer, BillboardFX); C's window ends at the last.
//   D) BloomPass         m_bloom[0..4]
//   E) RTReflectionPass(Variant::TranslucentFront) - the same 7 transients as A (same formats,
//      render res, offsets and end offset, so kTailGuard covers it). Registered right after A on
//      the graphics queue: A's window closes before E's opens, and E re-acquires with its own
//      aliasing barriers.
//   F) RTDIPass / RTGIPass and their ".Shadow" instances - the ReSTIR DI (8) and GI (6) per-frame
//      intermediates (ReSTIRTransientTextures.h), each packed from 0 with 64 KiB of slack after its
//      last NRD guide and reserved at the full-resolution worst case. Serialized graphics-queue
//      graph passes like A and E; no cross-frame reads.
//   Never move A, E or F to the compute queue or make one async: the aliasing becomes a race with
//   no debug-layer message. NPRStabilize's output is group D's late joiner, packed above Bloom
//   (see GroupDBase()).
//
// Why the groups may share bytes:
//   - A, B, C are direct-queue graph passes serialized by the RenderGraph prevCL chain
//     (RTR -> XeGTAO -> ... -> VolFog); a queue executes command lists in submission order. C's
//     later readers (forward / VFX fog) are graph passes on the same queue, and no other group's
//     user is registered between VolFog and BillboardFX: A / E / B / F run before Lighting, and D
//     starts only after the graph.
//   - D records into computeCL (compute queue), which waits on preComputeCL's graphics fence
//     (covers every graph-pass CL); restoreCL (graphics) waits on computeCL before anything later
//     touches the frame. The next frame's direct-queue writes order after restoreCL/primaryCL.
//   - Cross-frame handoff needs no extra sync: the next frame's first user emits an aliasing
//     barrier and fully overwrites before any read.
//
// Rules for joining:
//   1. Zero cross-frame reads. History / ping-pong / denoiser-feedback textures stay committed.
//      "Written every frame" is not enough - check decline paths (RTR m_denoised deliberately
//      serves last frame's pixels).
//   2. The acquiring pass records GPUBarrier::Aliasing(nullptr, tex) for each of its transients at
//      the start of its window every frame it runs, then fully overwrites before reading. Never
//      rely on "the pass between us": inactive passes record nothing.
//   3. Placed creation happens only on the frame's device-owner thread (the render lane under
//      -renderowner), in a serial phase (OnCompile, PrepareResources, frame-boundary service),
//      never during Phase-B worker recording; worker-side rebuild fallbacks create committed
//      textures. The rule is serialisation against the frame's recording, not an OS thread: with
//      -animoverlap main runs the Animation phases while the lane is inside a frame, so main must
//      create no placed texture then (the Animation phases allocate only committed resources and
//      descriptors from an internally locked free list).
//   4. Non-RT/DS SRV|UAV textures only (ALLOW_ONLY_NON_RT_DS_TEXTURES + CREATE_NOT_ZEROED).
//      Anything the asCL/ddgiCL compute segment touches must never join: that segment overlaps
//      direct-queue work.
//   5. Keep engine state tracking normal across aliasing barriers: the engine mirror and the D3D12
//      debug layer track state through them (per resource, not per byte). Do not reset states on
//      activation.
//
// Heap lifetime: grow-only. Every placed texture holds its own ComPtr ref on the heap
// (TextureDX12::placedHeap) and DestroyTexture defers that ref with the resource, so replacing
// m_heap never dangles an in-flight frame and needs no retirement ring (the GPUBuffer heap-batch
// model). On growth the epoch bumps; groups compare their stored epoch at their own service
// points and rebuild onto the current heap when stale (until then they sit on the old heap -
// correct, merely unshared).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wrl.h>
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "Graphics/VideoMemory.h"        // the render-target heaps' residency priority
#include <d3d12.h>
#include <cstdint>
#include <functional>
#include "System/Log.h"

class TransientAliasArena
{
public:
    // What the heap holds: the frame-transient SRV / UAV textures (every group above), or render targets
    // and depth (the render graph's secondary view sets, RenderGraph::PlaceBoundSet). Resource heap tier 1
    // keeps the two apart.
    enum class Kind : uint8_t { Textures, RenderTargets };

    TransientAliasArena() = default;
    // @p ledgerName is the heap's name in the GPU ledger (Profiler.MemoryTop); it outlives the arena (a literal).
    // @p shared: the heap is every secondary view's, so its bytes are counted to none of them.
    TransientAliasArena(Kind kind, const char* ledgerName, bool shared = false)
        : m_kind(kind), m_name(ledgerName), m_shared(shared) {}
    Kind GetKind() const { return m_kind; }

    // The arena a secondary view's pass transients are placed in (null: committed). Views record one after
    // another, so one heap serves all of them; the groups' disjointness argument above holds inside a view's
    // segment as it does in the main frame.
    void SetViewTwin(TransientAliasArena* twin) { m_viewTwin = twin; }
    TransientAliasArena* ViewTwin() const { return m_viewTwin; }

    // Master gate for the whole transient-aliasing tier. Flip to false to make
    // every group fall back to committed textures (they all keep that path).
    static constexpr bool kEnabled = true;

    static constexpr uint64_t kGrowGranularity = 4ull << 20; // 4 MiB steps

    // Tail guard: unused slack after the residents. A bug mitigation, not a tuning knob - do not
    // remove it.
    // NRD (REBLUR_SPECULAR) is third-party and dispatches its own compute shaders over the raw
    // ID3D12Resource*s we hand it, and it reads past the end of its input textures. On a committed
    // resource that read lands in the allocation's own tail padding; on a placed resource packed
    // against the heap end it is unmapped memory and the GPU page-faults (DXGI_ERROR_DEVICE_HUNG on
    // the editor's startup resize). The over-read is under one 64 KiB placement-alignment unit (no
    // hang at 64 KiB of slack); 1 MiB is 64 KiB with margin and ~1% of the heap. Heap growth,
    // CREATE_NOT_ZEROED garbage, placements past the end and GPU idles were each ruled out.
    // RTR.specOut has the highest end offset, so one tail guard suffices; if a non-NRD resource ends
    // last, NRD's over-read lands in another group's mapped bytes, harmless for a read. The
    // alternative is taking the NRD textures out of the arena at their full VRAM cost.
    // Rule: anything handed to a third-party library that dispatches its own shaders needs slack
    // after it, because its addressing cannot be audited here.
    static constexpr uint64_t kTailGuard = 1ull << 20;

    // kTailGuard is reserved space: Ensure() sizes the heap as usable + kTailGuard and compares
    // requests against usable only - comparing against the guard-inclusive size would serve a grown
    // resident from the old heap with the slack gone, the same hang by another route. Keep m_usable
    // and m_capacity separate, and keep the CreateTexturePlaced range check.
    // The heap is tagged with its usable size through this private-data GUID so
    // GraphicsDX12::CreateTexturePlaced can verify on the CPU, at placement time, that no resident
    // reaches into the guard (the GPU over-read itself has no runtime detector).
    static inline const GUID kUsableSizeTag =
        { 0x7b3c1a52, 0x9e4d, 0x4c0b, { 0xa1, 0x2f, 0x55, 0x1d, 0x8e, 0x07, 0xc3, 0x91 } };

    // Compile-time guard, because no runtime test catches this hang: the scenario harness
    // (-unattended -artifacts -maxseconds) masks it, and so do the D3D12 debug layer, DRED and
    // -settings=, so a scenario pack for it could never fire. 64 KiB is the smallest slack measured
    // hang-free; anything below it is a device hang, not a memory saving.
    static_assert(kTailGuard >= (64ull << 10),
                  "kTailGuard must stay >= 64 KiB. NRD reads past the end of the "
                  "textures handed to it; with no slack after the arena's last "
                  "resident that read lands on unmapped memory and hangs the GPU. "
                  "See the analysis above before changing this.");


    static uint64_t AlignUp(uint64_t v, uint64_t a)
    {
        return (v + a - 1) & ~(a - 1);
    }

    // Idles the GPU before Ensure() replaces an existing heap. The Renderer sets it once; without it,
    // growth proceeds unguarded.
    void SetIdleCallback(std::function<void()> fn) { m_idle = std::move(fn); }

    // Returns a heap whose usable capacity is >= @p bytes, growing it (and bumping the epoch) if
    // needed. Serial phases on the device-owner thread only (rule 3): placed-resource creation during
    // Phase-B worker recording is the descriptor race. Returns nullptr when disabled or on CreateHeap
    // failure (callers fall back to committed textures).
    ID3D12Heap* Ensure(ID3D12Device* device, uint64_t bytes)
    {
        if (!kEnabled || !device || bytes == 0) return nullptr;
        // Against the USABLE size, never m_capacity (which includes the guard).
        if (m_heap && bytes <= m_usable) return m_heap.Get();

        // Growth: replacing the heap bumps the epoch and every group re-places its textures onto the
        // new heap at its own service point. Groups pack from 0 and overlap, so a re-place creates a
        // resource over bytes another group's textures occupy; rule 2 covers steady-state aliasing, not
        // creating a resource over bytes the GPU is still reading, and an unidled startup-resize growth
        // hung the GPU. Growth only happens on a resolution change, so the idle is rare and cheap; the
        // first allocation needs none.
        // This idle does not order a later group's re-place against the frames after this one; it
        // suffices because every group services the epoch change before the next submission. A group
        // that re-places outside that window needs its own idle.
        if (m_heap && m_idle)
        {
            LOG_INFO("TransientAliasArena: growing %llu -> %llu MiB usable (+%llu MiB "
                     "tail guard) — idling the GPU before the heap is replaced "
                     "(groups re-place onto it)",
                     (unsigned long long)(m_usable >> 20),
                     (unsigned long long)(AlignUp(bytes, kGrowGranularity) >> 20),
                     (unsigned long long)(kTailGuard >> 20));
            m_idle();
        }

        const uint64_t usable = AlignUp(bytes, kGrowGranularity);
        const uint64_t cap    = usable + kTailGuard;
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes                 = cap;
        hd.Properties.Type             = D3D12_HEAP_TYPE_DEFAULT;
        hd.Alignment                   = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        // Single-category heap (works on resource-heap-tier-1 hardware). NOT_ZEROED is safe: rule 2
        // guarantees full overwrite before read, and rule 4 keeps depth-stencil planes out of a Textures
        // heap; a RenderTargets heap's residents are discarded after every aliasing barrier (D3D12's first
        // operation on an aliased render target or depth texture).
        hd.Flags = (m_kind == Kind::RenderTargets ? D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES
                                                  : D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES)
                 | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED;

        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(&heap))))
        {
            LOG_ERROR("TransientAliasArena: CreateHeap(%llu MiB) failed — "
                      "residents fall back to committed textures",
                      (unsigned long long)(cap >> 20));
            return nullptr;
        }
        // Old heap: just drop our ref. Placed textures created against it keep
        // it alive through their own deferred release (TextureDX12::placedHeap).
        // Tag the heap with its usable size (see kUsableSizeTag).
        heap->SetPrivateData(kUsableSizeTag, sizeof(usable), &usable);
        if (m_kind == Kind::RenderTargets) VideoMemory::PrioritizeHigh(device, heap.Get());
        const uint32_t attributed = m_shared ? GpuLedger::SetViewAttribution(0) : 0u;
        GpuLedger::TrackHeap(heap.Get(), Diag::Prof::GpuMemCat::TransientHeap, m_name);
        if (m_shared) GpuLedger::SetViewAttribution(attributed);
        m_heap     = std::move(heap);
        m_usable   = usable;
        m_capacity = cap;
        ++m_epoch;
        LOG_INFO("TransientAliasArena: heap %llu MiB = %llu usable + %llu tail guard (epoch %u)",
                 (unsigned long long)(cap >> 20), (unsigned long long)(usable >> 20),
                 (unsigned long long)(kTailGuard >> 20), m_epoch);
        return m_heap.Get();
    }

    // Drops the heap. The textures placed in it keep it alive through their own references until D3D12 destroys
    // them; the next Ensure makes a new heap (and bumps the epoch).
    void Release()
    {
        m_heap.Reset();
        m_usable = m_capacity = 0;
        m_groupDBase = 0;
    }

    // Groups stamp the epoch they built against; a mismatch at their next service point means
    // "rebuild onto the current heap".
    uint32_t Epoch()    const { return m_epoch; }
    // The current heap (null before the first Ensure), for IGraphicsDevice::CreateTexturePlaced.
    ID3D12Heap* Heap()  const { return m_heap.Get(); }
    // Bytes residents may occupy (heap size minus kTailGuard) - the number every caller wants.
    // HeapSize() is the D3D12 heap size.
    uint64_t Capacity() const { return m_usable; }
    uint64_t HeapSize() const { return m_capacity; }

    // ---- Group D sub-layout ----
    // Group D's members all live inside one computeCL and are read by the same ToneMap dispatch, so
    // they must be disjoint from each other while still aliasing the other groups. Bloom owns offsets
    // [0, base) and publishes base here at the end of its rebuild; the late joiner (NPRStabilize)
    // packs upward from it.
    // There is exactly one late joiner; a second cannot share the base. With AA off the post chain
    // reads NPRStabilize's output through ToneMap (resolvedSrv = aaInputSrv = NPR's output), so a later
    // computeCL writer needs its own bytes. That takes a full group-D relayout, every member re-placed
    // together, because a member that re-places alone (FSR toggling moves one resolution and not the
    // other) grows into its neighbour; LensFlare stays committed for this reason.
    // Joiners re-place when the base moves, not only on an epoch change: Bloom is sized from the
    // post-process resolution and NPRStabilize from the render resolution, so toggling FSR moves
    // Bloom's footprint without touching NPR's - without the base check NPR keeps its old offset and
    // lands inside the grown bloom chain.
    void     SetGroupDBase(uint64_t bytes) { m_groupDBase = bytes; }
    uint64_t GroupDBase() const            { return m_groupDBase; }

private:
    Microsoft::WRL::ComPtr<ID3D12Heap> m_heap;
    uint64_t m_capacity   = 0;   // D3D12 heap size = m_usable + kTailGuard
    uint64_t m_usable     = 0;   // what Ensure() compares against - NOT m_capacity
    uint64_t m_groupDBase = 0;
    uint32_t m_epoch      = 0;
    Kind     m_kind       = Kind::Textures;
    const char* m_name    = "TransientAliasArena.Heap";
    bool     m_shared     = false;
    TransientAliasArena* m_viewTwin = nullptr;
    std::function<void()> m_idle;   // see SetIdleCallback / Ensure
};
