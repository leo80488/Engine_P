#pragma once

// BeamSystem - GPU-driven procedural-tube beam (heavy-beam) generator.
//
// All beams share four PVF vertex buffers (pos / normal / tangent / uv) and one index
// buffer, each a single slab; a beam's vertex window is addressed via
// MeshDescriptor.byteOffset. One CS dispatch per beam rewrites its slice every frame,
// using a parallel-transport frame so curved beams do not twist.
// Typical scene: 1..10 active beams; the pool holds 32.
//
// Lifecycle:
//   uint32_t slot = system->Acquire();
//   system->SetControlPoints(slot, points, count);
//   system->SetParams(slot, { .globalRadiusScale = 1.0f, .wobbleAmplitude = 0.02f });
//   ... draw via DrawPacket{ meshDescriptorIndex = system->GetMeshDescSlot(slot), ... }
//   system->Release(slot);

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"

#include <vector>
#include <cstdint>
#include <DirectXMath.h>

class IGraphicsDevice;
class MeshDescriptorHeap;

// 32 bytes - must match shaders/BeamCommon.hlsli BeamControlPoint.
struct alignas(16) BeamControlPointGPU
{
    DirectX::XMFLOAT3 position;
    float             radius;
    DirectX::XMFLOAT4 colorTint;
};
static_assert(sizeof(BeamControlPointGPU) == 32, "BeamControlPointGPU layout drift");

// 256 bytes - one per-beam CS CBV slot. HLSL BeamGenParams reads the first 32 bytes;
// the rest pads to D3D12's 256-byte CBV alignment.
struct alignas(16) BeamGenParamsGPU
{
    uint32_t controlPointOffset;   // index into the shared control-point buffer
    uint32_t controlPointCount;    // typically 2..8
    uint32_t vertexBaseElement;    // start vertex index in the shared VBs
    uint32_t _pad0;
    float    globalRadiusScale;
    float    wobbleAmplitude;
    float    wobbleSpeed;
    float    time;
    uint8_t  _pad1[256 - 32];      // padding to a full 256B CBV slot
};
static_assert(sizeof(BeamGenParamsGPU) == 256, "BeamGenParamsGPU must be 256B");

class BeamSystem
{
public:
    // Tube topology - must match the shaders/BeamCommon.hlsli macros.
    static constexpr uint32_t kRadialSegs                 = 12;
    static constexpr uint32_t kAxialSegs                  = 16;
    static constexpr uint32_t kRingVertCount              = kRadialSegs + 1;       // +1 for UV seam
    static constexpr uint32_t kAxialRingCount             = kAxialSegs + 1;
    static constexpr uint32_t kVertsPerBeam               = kRingVertCount * kAxialRingCount;     // 13 x 17 = 221
    static constexpr uint32_t kIndicesPerBeam             = kAxialSegs * kRadialSegs * 6;          // 16 x 12 x 6 = 1152
    static constexpr uint32_t kMaxControlPointsPerBeam    = 8;

    // Pool size: 32 x 221 verts x 48 B (12+12+16+8) ~ 340 KB.
    static constexpr uint32_t kMaxBeams                   = 32;
    static constexpr uint64_t kBeamParamSlotStride        = 256;

    static constexpr uint32_t kInvalidBeamSlot            = 0xFFFFFFFFu;

    // ---- BeamSimPass-facing accessors ----
    struct ActiveBeam
    {
        uint32_t beamSlot;
        uint32_t cbOffset;   // bytes into m_paramsBuffer for this beam's CBV slot
    };

    // ---- Lifetime ----
    void Init(IGraphicsDevice& gfx, MeshDescriptorHeap& meshDescHeap);
    void Shutdown(IGraphicsDevice& gfx);

    // ---- Beam slot management ----
    // Returns a slot in [0, kMaxBeams), or kInvalidBeamSlot when the pool is exhausted.
    // The slot stays reserved until Release().
    uint32_t Acquire();
    void     Release(uint32_t beamSlot);

    // Replaces the beam's control points; at most kMaxControlPointsPerBeam of @p points
    // are kept, extras are dropped. With fewer than 2 the beam is inactive: BeginFrame
    // skips it and no dispatch rewrites its vertices.
    void SetControlPoints(uint32_t beamSlot,
                          const BeamControlPointGPU* points,
                          uint32_t                   count);

    // Replaces the beam's per-frame params (radius scale / wobble); the CS uses them from
    // its next dispatch. BeginFrame overwrites the offset, count, vertex-base and time fields.
    void SetParams(uint32_t beamSlot, const BeamGenParamsGPU& params);

    // Mesh descriptor slot for DrawPacket.meshDescriptorIndex; stable for the slot's
    // whole life.
    uint32_t GetMeshDescSlot(uint32_t beamSlot) const
    { return beamSlot < kMaxBeams ? m_perBeam[beamSlot].meshDescSlot : 0xFFFFFFFFu; }

    // Index count of one beam, for DrawIndexedInstanced.
    static constexpr uint32_t GetIndexCount() { return kIndicesPerBeam; }

    // ---- BeamSimPass-facing accessors ----
    const std::vector<ActiveBeam>& GetActiveBeams() const { return m_activeBeams; }

    // The params / control-point UPLOAD buffers have one copy per frame in flight; these
    // return the copy for gfx.GetFrameIndex(). BeamSimPass calls them once per Execute.
    const RHI::GPUBuffer& GetParamsBuffer()    const;
    uint64_t              GetControlPointsSrv()const;

    const RHI::GPUBuffer& GetPosBuffer()       const { return m_posBuffer; }
    const RHI::GPUBuffer& GetNormalBuffer()    const { return m_normalBuffer; }
    const RHI::GPUBuffer& GetTangentBuffer()   const { return m_tangentBuffer; }
    const RHI::GPUBuffer& GetUvBuffer()        const { return m_uvBuffer; }

    uint64_t GetPosUav()     const { return m_posUav; }
    uint64_t GetNormalUav()  const { return m_normalUav; }
    uint64_t GetTangentUav() const { return m_tangentUav; }
    uint64_t GetUvUav()      const { return m_uvUav; }

    // ---- Per-frame upload ----
    // Rebuilds m_activeBeams and uploads every active beam's CPU control points and
    // params; call from Renderer::BeginFrame, consumed by BeamSimPass::Execute.
    // @p globalTimeSec is written into each beam's params.time (wobble noise).
    void BeginFrame(float globalTimeSec);

private:
    static constexpr uint32_t kFrameCount = RHI::kFramesInFlight;

    struct PerBeam
    {
        bool                                       acquired = false;
        bool                                       hasControlPoints = false;
        uint32_t                                   meshDescSlot = 0xFFFFFFFFu;
        std::vector<BeamControlPointGPU>           controlPoints;       // CPU mirror
        BeamGenParamsGPU                           params{};            // CPU mirror
    };

    IGraphicsDevice*    m_gfx     = nullptr;
    MeshDescriptorHeap* m_meshHeap = nullptr;

    // Shared PVF VBs (DEFAULT heap, SRV + UAV, ByteAddressBuffer).
    RHI::GPUBuffer m_posBuffer;       // 12 B/vert
    RHI::GPUBuffer m_normalBuffer;    // 12 B/vert
    RHI::GPUBuffer m_tangentBuffer;   // 16 B/vert
    RHI::GPUBuffer m_uvBuffer;        //  8 B/vert
    uint64_t       m_posUav     = 0, m_normalUav = 0, m_tangentUav = 0, m_uvUav = 0;
    uint32_t       m_posBindless = 0, m_normalBindless = 0,
                   m_tangentBindless = 0, m_uvBindless = 0;

    // Shared IB (DEFAULT heap, ByteAddressBuffer SRV, no UAV): filled once at Init with
    // the canonical tube index pattern, read by every beam draw.
    RHI::GPUBuffer m_indexBuffer;
    uint32_t       m_indexBindless = 0;

    // Control-point buffer shared by all beams (kMaxControlPointsPerBeam entries per slot;
    // UPLOAD heap, StructuredBuffer SRV), one per frame in flight, written every frame by
    // BeginFrame.
    RHI::GPUBuffer m_controlPointsBuffer[kFrameCount];
    uint64_t       m_controlPointsSrv[kFrameCount]    = {};
    void*          m_controlPointsMapped[kFrameCount] = {};

    // Per-beam BeamGenParams CBV (UPLOAD heap, root CBV per dispatch at offset
    // beamSlot * kBeamParamSlotStride), one per frame in flight.
    RHI::GPUBuffer m_paramsBuffer[kFrameCount];
    void*          m_paramsMapped[kFrameCount] = {};

    PerBeam               m_perBeam[kMaxBeams];
    std::vector<uint32_t> m_freeList;            // beam slot indices

    // Built each BeginFrame; consumed by BeamSimPass.
    std::vector<ActiveBeam> m_activeBeams;
};
