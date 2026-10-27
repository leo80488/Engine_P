#pragma once

#include <vector>

// PoseRingBuffer    - per-frame UPLOAD ring of bone matrices for all skinned entities: the CPU
//                     writes float4x4[] through MapSlice(), SkinningCS reads them.
// SkinnedVertexRing - per-frame DEFAULT rings for SkinningCS output: positions and normals
//                     (float3) and tangents (float4), UAV-written by SkinningCS, SRV-read by
//                     the vertex shaders. SkinningPass places the barriers.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;

// ==== PoseRingBuffer ====
class PoseRingBuffer
{
public:
    // Bone-matrix slots per frame. An UPLOAD-heap, permanently mapped ring: all of it is resident
    // system RAM from startup, empty scene included - 1<<17 slots = 8 MiB per frame slot, 24 MiB in
    // all. A skeleton takes boneCount slots per frame (~260 characters at 500 bones). Overflow is
    // graceful (Alloc returns ~0u -> rest pose) and BeginFrame warns at 75% / 90%, naming this
    // constant.
    static constexpr uint32_t kMaxBonesPerFrame = 1 << 17;
    static constexpr uint32_t kFramesInFlight   = RHI::kFramesInFlight;

    // Call once after device creation.
    void Init(IGraphicsDevice& gfx);

    // Reset the per-frame write head; call at the start of every BeginFrame.
    // frameIndex wraps internally via % kFramesInFlight.
    void BeginFrame(uint64_t frameIndex);

    // Allocate boneCount consecutive matrix slots; returns the first slot index,
    // or ~0u when the ring is full (callers fall back to the rest pose).
    // Slots are valid until the next BeginFrame() for the same frame-in-flight slot.
    uint32_t Alloc(uint32_t boneCount);

    // A CPU-writable pointer to @p boneCount matrices from @p slot; do not write past
    // slot + boneCount. It points into the CPU shadow (see m_shadow), not the UPLOAD mapping: call
    // FlushToGPU() once after every writer has run, or the GPU sees last frame's bytes.
    DirectX::XMFLOAT4X4* MapSlice(uint32_t slot, uint32_t boneCount);

    // Push this frame's written range (slots [0, writeHead)) from the CPU
    // shadow into the UPLOAD mapping, as ONE sequential memcpy. Idempotent and
    // free when nothing was written. Must run after the last MapSlice writer
    // and before the render graph executes SkinningPass.
    void FlushToGPU();

    // GPU SRV handle of this frame's buffer (a ByteAddressBuffer for SkinningCS). HALF-KEYED:
    // m_frameSlot is rotated on the GAME half, so a render-half caller names its own slot
    // (SkinnedVertexRing's UAV handles say why; both rings share the rule).
    uint64_t UploadSRVHandle() const;              // game half: m_frameSlot
    uint64_t SRVHandleForSlot(uint32_t slot) const;// render half: GetFrameIndex()

    // Read-only access to this frame's bone matrices; @p byteOffset / 64 = first bone index.
    // Returns nullptr when the buffer is not mapped. Reads the CPU shadow, not the UPLOAD mapping
    // (see m_shadow).
    const DirectX::XMFLOAT4X4* ReadMapped(uint32_t byteOffset) const;
    uint32_t GetWriteHead() const { return m_writeHead; }

private:
    RHI::GPUBuffer m_buffers[kFramesInFlight];
    void*          m_mapped[kFramesInFlight]{};
    uint64_t       m_srvHandles[kFramesInFlight]{};

    // CPU shadow of the CURRENT frame's slots, in plain cached RAM. m_mapped[] points into an UPLOAD
    // heap mapped WRITE-COMBINED: sequential writes are fast, but reads bypass the cache and run at a
    // few hundred MB/s, and several per-frame systems read poses back (SocketSystem,
    // FaceShadingSystem, the skinned-AABB pass, the Lua skeleton bindings, the editor's animation
    // window). Reading the WC mapping cost ~9.5 ms/frame at 100 characters against ~0.1 ms from
    // cached RAM. So writers and readers use the shadow and FlushToGPU() copies the written range
    // across in one sequential pass, the pattern WC is good at; no engine code holds a WC pointer.
    // Costs one kMaxBonesPerFrame-sized array of RAM (8 MiB at 1<<17).
    std::vector<DirectX::XMFLOAT4X4> m_shadow;

    // Catches the one hard failure of the staging design: a MapSlice writer that runs AFTER
    // FlushToGPU writes only the shadow, and the GPU silently renders last frame's poses. Alloc()
    // reports an Alloc after a flush with no BeginFrame in between.
    bool m_flushedThisFrame = false;

    uint32_t m_writeHead = 0;  // current bone slot cursor (resets each BeginFrame)
    uint32_t m_frameSlot = 0;  // current index into [kFramesInFlight]

    // Capacity watch (BeginFrame): session peak plus one-shot 75% / 90% notices, so the cap is
    // tuned from evidence.
    uint32_t m_peakBones = 0;
    bool     m_warned75  = false;
    bool     m_warned90  = false;
};

// ==== SkinnedVertexRing ====
class SkinnedVertexRing
{
public:
    // Per-frame budget in SKINNED VERTICES: one slot per vertex in each ring, 12 (pos) + 12 (nrm) +
    // 16 (tan) = 40 B per frame slot. This is the default; Init takes the run's (m_vertexBudget), which
    // SkinnedMeshSubsystem resolves from -skinvertexbudget, engine.json's skinning.vertexBudget or the
    // adapter, kMinSkinnedVertsPerFrame to kMaxSkinVertexBudget.
    // DEFAULT-heap VRAM, resident from startup EVEN ON AN EMPTY SCENE - a real content budget, not a
    // free upper bound: 1<<23 verts = 320 MiB per frame slot, 960 MiB across kFramesInFlight,
    // ~118 characters of the project's Rossi (70,573 verts, 15 sections, 498 bones). That holds the
    // 100-character target with ~18% headroom, on purpose: past the cap meshes silently freeze in
    // the REST POSE with only a log line, the least diagnosable failure here.
    // The cheapest way to buy VRAM back is packing: normals and tangents in R10G10B10A2 (4 B each)
    // take 40 B per vertex to 20 B. Positions cannot be packed: they are the DXR BLAS vertex stream
    // (R32G32B32_FLOAT). The raster path reads packed normals (pvf_fetch.hlsli FetchAsFloat3;
    // tangents need the matching case), but the RT paths hard-code a 12 B normal stride
    // (`Load3((inst.nrmStart + i) * 12u)` in DDGIRayTrace.cs.hlsl, HybridReflIntersectHW.cs.hlsl and
    // rt_hit_fetch.hlsli), so it is a change with a ray-tracing blast radius.
    // Do not answer scaling with another doubling: cost is linear in characters x verts per
    // character, and the lever is a skinned LOD for distant characters, which does not exist
    // (animation culling in Renderer::TickAnimationChain is visible / not visible). Build it before
    // raising this again.
    // Pre-sized, never grown on demand:
    //   * posBindlessIdx[] / nrmBindlessIdx[] / tanBindlessIdx[] are registered once per frame slot
    //     and read through g_Buffers[]; a realloc invalidates them.
    //   * The GPU is still reading the older slots, so a move needs a full flush, not a fence.
    //   * DDGISceneAS builds per-frame skinned BLAS directly over ring slices; a BLAS over a moved or
    //     unwritten region hangs DXR traversal.
    //   * The overflow frame renders wrong either way.
    // If it must grow, do it at a frame boundary behind a full GPU idle (level-load semantics),
    // never mid-frame.
    static constexpr uint32_t kMaxSkinnedVertsPerFrame = 1 << 23;
    // The smallest budget: 1<<20 verts = 40 MiB per frame slot, 120 MiB across kFramesInFlight,
    // ~14 Rossi-sized characters. The adapter-sized budget (engine.json skinning.vertexBudget 0, the
    // default) is this under 4 GB of video memory, which a first boot reads as the Low tier.
    static constexpr uint32_t kMinSkinnedVertsPerFrame = 1 << 20;
    // The largest budget Init accepts: 1<<25 verts = 3840 MiB across kFramesInFlight.
    static constexpr uint32_t kMaxSkinVertexBudget = 1 << 25;

    // Position-ring slots: ONE per skinned vertex, like normal and tangent. Last frame's positions
    // (TAA motion vectors) are read from the previous ring slot through
    // MeshDescriptor::prevPosition, not copied into this frame.
    // ---- What kFramesInFlight depth buys ----
    //   1. A FRAME-LATE READ, distance 1: frame F's vertex shaders read slot S(F-1); impossible at
    //      depth 1.
    //   2. A same-slot write-after-read, distance kFramesInFlight: frame F's skinning writes S(F),
    //      last read by frame F-kFramesInFlight; GraphicsDX12::WaitSlotRetired fences that tenant.
    // ---- What orders (1) ----
    // The bytes frame F reads from S(F-1) are next overwritten by frame F+2's Skin.cs, and no fence
    // or barrier orders that pair: SkinningPass's GPUBarrier::Memory is a plain UAV barrier (the
    // buffers are created COMMON and never transition), and the slot fence only proves frame F-1
    // complete when F+2 starts recording, while F and F+1 may still execute. GRAPHICS-QUEUE
    // SUBMISSION ORDER orders it: skinning and every prev-position consumer record onto
    // QueueType::GRAPHICS lists, frame F's in an earlier ExecuteCommandLists call than F+2's, and
    // separate calls on one queue do not overlap.
    // Three things break it silently (stale prev positions -> wrong TAA velocities -> ghosting on
    // skinned meshes only, invisible to the debug layer):
    //   * moving a prev-position consumer to the async COMPUTE queue without a cross-queue fence
    //     (the post chain already picks its queue at runtime, by DLSS);
    //   * merging two frames' work into one ExecuteCommandLists;
    //   * dropping kFramesInFlight to 2, which collapses the distance to 1.
    static constexpr uint32_t kMaxPosSlotsPerFrame = kMaxSkinnedVertsPerFrame;

    static constexpr uint32_t kFramesInFlight   = RHI::kFramesInFlight;

    // Call once with the run's budget and where it came from (the init log names @p source);
    // SkinnedMeshSubsystem::Init then registers every slot's buffers in the bindless table
    // (posBindlessIdx[] / nrmBindlessIdx[] / tanBindlessIdx[]). The budget is resolved by the caller
    // so this file stays free of the settings layer: the standalone unit tests link the pose ring
    // from EngineCore.lib, and the developer settings drag the renderer in behind them.
    void Init(IGraphicsDevice& gfx, uint32_t vertexBudget, const char* source);

    // Reset per-frame allocation heads; call at the start of every BeginFrame.
    void BeginFrame(uint64_t frameIndex);

    // Allocate vertexCount position slots; returns the BYTE OFFSET into posBuffer,
    // or ~0u when the ring is full (callers fall back to the rest pose).
    uint32_t AllocPosition(uint32_t vertexCount);

    // Allocate vertexCount normal slots; returns the BYTE OFFSET into nrmBuffer,
    // or ~0u when the ring is full.
    uint32_t AllocNormal(uint32_t vertexCount);

    // Allocate vertexCount TANGENT slots (float4 = 16 B/vertex); BYTE OFFSET,
    // or ~0u when the ring is full.
    uint32_t AllocTangent(uint32_t vertexCount);

    // UAV handles for SkinningPass, the only writer, by slot. HALF-KEYED, for both rings:
    // m_frameSlot is rotated from GetUploadSlot() on the GAME half (Renderer's Animation phase) and
    // is right for a game-half caller baking data for its own frame. SkinningPass runs in the render
    // graph on the render half's slot (gfx.GetFrameIndex()); the two match only while the halves run
    // serially, and mixing them would write one slot's buffers and place the UAV barrier on
    // another's - a bug no validation layer sees. So the UAV forms take a slot and have no
    // no-argument version.
    uint64_t PosUAVHandleForSlot(uint32_t slot) const;
    // SRV of a slot's position buffer, for the render half; UploadPosSRVHandle() below is the game
    // half's slot.
    uint64_t PosSRVHandleForSlot(uint32_t slot) const;
    uint64_t NrmUAVHandleForSlot(uint32_t slot) const;
    uint64_t TanUAVHandleForSlot(uint32_t slot) const;

    // Game-half SRV handles of this frame's buffers, valid after SkinningPass's UAV barrier
    // (AfterimageSystem's capture copy reads through them; the vertex shaders use the bindless
    // indices below).
    uint64_t UploadPosSRVHandle() const;
    uint64_t UploadNrmSRVHandle() const;
    uint64_t UploadTanSRVHandle() const;

    // This frame's buffers on the game half (Upload*: DDGISceneAS::Gather builds BLAS over them),
    // and any slot's on the render half (*ForSlot: SkinningPass's barriers, bindless registration).
    const RHI::GPUBuffer& UploadPosBuffer() const;
    const RHI::GPUBuffer& UploadNrmBuffer() const;
    const RHI::GPUBuffer& UploadTanBuffer() const;
    const RHI::GPUBuffer& PosBufferForSlot(uint32_t slot) const;
    const RHI::GPUBuffer& NrmBufferForSlot(uint32_t slot) const;
    const RHI::GPUBuffer& TanBufferForSlot(uint32_t slot) const;

    // Upload* = the GAME half's slot, right for a game-half caller baking data for its own frame.
    // Every accessor names its half; the render half uses the *ForSlot forms and has no
    // no-argument fallback.
    uint32_t UploadPosBindlessIdx()     const { return posBindlessIdx[m_frameSlot]; }
    uint32_t UploadNrmBindlessIdx()     const { return nrmBindlessIdx[m_frameSlot]; }
    uint32_t UploadTanBindlessIdx()     const { return tanBindlessIdx[m_frameSlot]; }

    // The active (game-half) frame slot, 0..kFramesInFlight-1.
    uint32_t GetFrameSlot() const { return m_frameSlot; }

    // Bindless indices in g_Buffers[] per frame slot, registered once by SkinnedMeshSubsystem::Init
    // (persistent: they survive world reloads).
    uint32_t posBindlessIdx[kFramesInFlight];
    uint32_t nrmBindlessIdx[kFramesInFlight];
    uint32_t tanBindlessIdx[kFramesInFlight];

private:
    RHI::GPUBuffer m_posBuffers[kFramesInFlight];
    RHI::GPUBuffer m_nrmBuffers[kFramesInFlight];
    RHI::GPUBuffer m_tanBuffers[kFramesInFlight];

    uint64_t m_posUAVHandles[kFramesInFlight]{};
    uint64_t m_nrmUAVHandles[kFramesInFlight]{};
    uint64_t m_tanUAVHandles[kFramesInFlight]{};
    uint64_t m_tanSRVHandles[kFramesInFlight]{};
    uint64_t m_posSRVHandles[kFramesInFlight]{};
    uint64_t m_nrmSRVHandles[kFramesInFlight]{};

    // The vertex budget, fixed at startup by Init (-skinvertexbudget, engine.json's
    // skinning.vertexBudget or the adapter, resolved by SkinnedMeshSubsystem); every byte limit and
    // the telemetry use it.
    uint32_t m_vertexBudget = kMaxSkinnedVertsPerFrame;
    uint32_t m_posByteHead     = 0;
    uint32_t m_nrmByteHead     = 0;
    uint32_t m_tanByteHead     = 0;
    uint32_t m_frameSlot       = 0;

    // Capacity watch (BeginFrame): session peak + one-shot 75%/90% notices.
    uint32_t m_peakPosVerts = 0;
    bool     m_warned75     = false;
    bool     m_warned90     = false;
};
