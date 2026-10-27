#pragma once

// Media - the C++ side of shaders/core/participating_media.hlsli: the scene's fog as one
// participating medium for the path tracers and the ray-traced froxel lighting. Three kinds of
// source: the global medium (VolumetricFogComponent or the scene's legacy fog), the height fog
// (HeightFogComponent) and the bounded local volumes (LocalFogVolumeComponent).
//
// The Renderer fills a Media::Set on the render half - the global medium from VolumetricFogPass,
// the height fog from the render features that contribute a medium, the local volumes from the
// fog gather - and SetBuffer::Upload writes it with its local volumes into this frame's slot of
// one StructuredBuffer<float4> (shaders/medium_set.hlsli reads it): the set in the first six
// float4, each local volume in the next six.

#include "Graphics/FrameConstants.h"
#include "Graphics/GraphicsStruct.h"

#include <DirectXMath.h>
#include <cstdint>
#include <span>

class IGraphicsDevice;

namespace Media
{
    // Mirror of MediumGlobal: sigma_t(y) = density x exp(-falloff x max(y - baseHeight, 0)),
    // within `reach` of the set's camera (0: everywhere).
    struct alignas(16) Global
    {
        float albedo[3]   = { 1.f, 1.f, 1.f };
        float density     = 0.f;    // 1/m; 0 = none
        float emission[3] = { 0.f, 0.f, 0.f };   // premultiplied by its intensity
        float falloff     = 0.f;    // 1/m above baseHeight
        float baseHeight  = 0.f;    // world Y (m)
        float anisotropy  = 0.f;    // HG g
        float reach       = 0.f;    // m from Set::camera; 0 = no end
        float _pad        = 0.f;
    };
    static_assert(sizeof(Global) == 48, "Media::Global must match MediumGlobal");

    // Mirror of MediumHeight: sigma_t(y) = density x exp(-falloff x (y - height)).
    struct alignas(16) Height
    {
        float albedo[3]  = { 1.f, 1.f, 1.f };
        float density    = 0.f;     // 1/m at y == height; 0 = none
        float falloff    = 0.f;     // 1/m
        float height     = 0.f;     // world Y (m)
        float anisotropy = 0.f;     // HG g
        float _pad       = 0.f;
    };
    static_assert(sizeof(Height) == 32, "Media::Height must match MediumHeight");

    // Mirror of MediumSet. `camera` is where the global medium's reach is measured from: zero
    // without a reach, so the set does not follow the camera.
    struct alignas(16) Set
    {
        Global   global;
        Height   height;
        uint32_t localCount = 0;
        float    camera[3]  = {};
    };
    static_assert(sizeof(Set) == 96, "Media::Set must match MediumSet");

    // Mirror of MediumLocal (and of VolumetricFogPass::LocalFogVolumeGPU, field for field).
    struct alignas(16) Local
    {
        float    worldToLocal[12] = {};   // three rows: local.i = dot(float4(p, 1), row_i)
        float    albedo[3]        = { 1.f, 1.f, 1.f };
        float    density          = 0.f;
        float    emission[3]      = { 0.f, 0.f, 0.f };
        float    edgeFade         = 0.f;
        uint32_t shape            = 0;
        float    heightFalloff    = 0.f;
        float    halfHeight       = 0.f;
        float    _pad             = 0.f;
    };
    static_assert(sizeof(Local) == 96, "Media::Local must match MediumLocal");

    // The local volumes a set carries: the nearest ones to the camera, from any distance.
    inline constexpr uint32_t kMaxLocals = 32;

    // The froxel fog's lighting volume as the last frame left it (VolumetricFogPass::LastLitVolume),
    // for the path tracer's continuation segments (pass.ptRtxdi.mediaContinuation froxel): its SRV
    // heap index (0: none), the view-projection it was lit for (FroxelParams' viewProj, row vectors)
    // and that view's camera, its radial range.
    struct FroxelVolume
    {
        uint32_t            srv = 0;
        DirectX::XMFLOAT4X4 viewProj{};
        DirectX::XMFLOAT3   camera{};
        float               froxelNear = 0.f;
        float               froxelFar  = 0.f;
    };

    // True when the set holds any medium: a density, or a local volume.
    bool HasMedium(const Set& set);

    // The set's buffer: one UPLOAD buffer per frame in flight, written by the render half.
    class SetBuffer
    {
    public:
        // Creates and maps the ring; false when a buffer could not be made.
        bool Init(IGraphicsDevice& gfx);
        void Release(IGraphicsDevice& gfx);

        // Writes @p set and up to kMaxLocals of @p locals (set.localCount is set to the count
        // written) into @p slot's buffer. Returns the buffer's SRV heap index, or 0 when the set
        // holds no medium or the ring is missing.
        uint32_t Upload(IGraphicsDevice& gfx, uint32_t slot, Set set, std::span<const Local> locals);

        // A hash of what the last Upload wrote (0 for no medium): a change restarts the
        // path tracer's accumulation.
        uint64_t Fingerprint() const { return m_fingerprint; }

    private:
        RHI::GPUBuffer m_buffer[RHI::kFramesInFlight];
        void*          m_mapped[RHI::kFramesInFlight] = {};
        uint64_t       m_fingerprint = 0;
    };
}
