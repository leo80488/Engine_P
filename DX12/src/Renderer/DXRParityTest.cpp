#include "Renderer/DXRParityTest.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PipelineLayoutBuilder.h"
#include "Graphics/RenderTypes.h"            // RenderView
#include "Graphics/RootSignatureLayout.h"    // RHI::Slots::CmpParam
#include "Graphics/ShaderDiskCache.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// See the header. The layout (shaders/DXRParity.hlsli): t0 the TLAS and t1 the
// rays as root SRVs, u0 the hits through a table, b0 four constants - one layout
// for the compute pipeline and as the DXR pipeline's global root signature.

namespace
{
// kSlot*: the class's kRays (the ray count) would shadow a plain kRays here.
constexpr RHI::Slots::CmpParam kSlotTLAS   { 0 };   // t0, root SRV (the TLAS address)
constexpr RHI::Slots::CmpParam kSlotRays   { 1 };   // t1, root SRV
constexpr RHI::Slots::CmpParam kSlotHits   { 2 };   // u0, table
constexpr RHI::Slots::CmpParam kSlotParams { 3 };   // b0, 4 constants

// DXRParity.hlsli's ParityRay / ParityHit.
struct ParityRay
{
    float origin[3];
    float tMin;
    float direction[3];
    float tMax;
};
struct ParityHit
{
    float    t;
    uint32_t instanceID;
    uint32_t primitiveIndex;
    uint32_t hit;
};
static_assert(sizeof(ParityRay) == 32 && sizeof(ParityHit) == 16, "DXRParity.hlsli's structs");

constexpr const char* kMiss[] = { "ParityMiss" };
const RHI::HitGroup   kHitGroup{ "ParityHitGroup", "ParityClosestHit", nullptr };

// T of the two closest hits: the same ray against the same TLAS, so they are
// expected to be bit-identical; the tolerance only absorbs a traversal that
// reports the same triangle through another numerical path.
constexpr float kRelativeT = 1.0e-4f;
} // namespace

bool DXRParityTest::Wanted()
{
    static const bool s_wanted = Sys::CommandLine::Get().Has("-dxr_parity");
    return s_wanted;
}

bool DXRParityTest::Prepare(IGraphicsDevice& gfx)
{
    const RHI::DeviceCapabilities caps = gfx.GetCapabilities();
    if (!caps.raytracing || !caps.inlineRaytracing)
    {
        LOG_WARNING("[DXR] parity skipped: the device has no ray tracing (raytracing=%d inline=%d)",
                    caps.raytracing ? 1 : 0, caps.inlineRaytracing ? 1 : 0);
        return false;
    }

    RHI::PipelineLayoutBuilder b("DXRParity");
    b.RootSRV(kSlotTLAS, 0).RootSRV(kSlotRays, 1).Table(kSlotHits, { RHI::UavRange(0) })
     .Constants(kSlotParams, 4, 0);
    if (!gfx.CreatePipelineLayout(b.Desc(), m_layout)) return false;

    // The inline-RayQuery kernel.
    {
        const auto cs = ShaderDiskCache::LoadOrCompile("shaders/DXRParity.cs.hlsl", "main", RHI::ShaderStage::CS,
                                                       {}, "DXRParityTest");
        RHI::Shader shader;
        if (cs.empty() || !gfx.CreateShader(RHI::ShaderStage::CS, cs.data(), cs.size(), shader)) return false;
        RHI::PipelineStateDesc pd{};
        pd.cs     = &shader;
        pd.layout = &m_layout;
        const bool ok = gfx.CreatePipelineState(pd, m_queryPSO);
        gfx.DestroyShader(shader);
        if (!ok) return false;
    }
    // The DXR library: ray generation, miss, one triangle hit group.
    {
        const auto lib = ShaderDiskCache::LoadOrCompile("shaders/DXRParity.lib.hlsl", "", RHI::ShaderStage::LIB,
                                                        {}, "DXRParityTest");
        RHI::Shader shader;
        if (lib.empty() || !gfx.CreateShader(RHI::ShaderStage::LIB, lib.data(), lib.size(), shader)) return false;
        RHI::RaytracingPipelineDesc rd;
        rd.library           = &shader;
        rd.rayGen            = "ParityRayGen";
        rd.miss              = kMiss;
        rd.missCount         = 1;
        rd.hitGroups         = &kHitGroup;
        rd.hitGroupCount     = 1;
        rd.maxPayloadBytes   = sizeof(ParityHit);
        rd.maxAttributeBytes = 8;   // the triangle barycentrics
        rd.maxRecursion      = 1;
        rd.layout            = &m_layout;
        rd.name              = "DXRParity";
        const bool ok = gfx.CreateRaytracingPipeline(rd, m_dxr);
        gfx.DestroyShader(shader);
        if (!ok) return false;
    }

    RHI::GPUBufferDesc rays;
    rays.size       = uint64_t(kRays) * sizeof(ParityRay);
    rays.stride     = sizeof(ParityRay);
    rays.usage      = RHI::Usage::UPLOAD;          // read through a root SRV: no view
    rays.debugName = "DXRParity.Rays";
    if (!gfx.CreateBuffer(rays, m_rays)) return false;

    RHI::GPUBufferDesc hits;
    hits.size       = uint64_t(kRays) * sizeof(ParityHit);
    hits.stride     = sizeof(ParityHit);
    hits.usage      = RHI::Usage::DEFAULT;
    hits.bindFlags = RHI::BindFlag::UNORDERED_ACCESS;
    hits.debugName = "DXRParity.Hits";
    RHI::GPUBufferDesc readback;
    readback.size       = hits.size;
    readback.usage      = RHI::Usage::READBACK;
    readback.debugName = "DXRParity.Readback";
    for (int i = 0; i < 2; ++i)
        if (!gfx.CreateBuffer(hits, m_hits[i]) || !gfx.CreateBuffer(readback, m_readback[i])) return false;
    return true;
}

void DXRParityTest::Record(IGraphicsDevice& gfx, RHI::CommandList cmd, uint64_t tlasAddress, const RenderView& view)
{
    // The rays: a kWidth x kHeight grid of pixel centres through the camera's
    // un-jittered view-projection, from the near plane (reverse-Z: NDC z = 1)
    // towards the NDC z = 0.5 point - written once, read by both kernels.
    using namespace DirectX;
    const XMMATRIX inv = XMMatrixInverse(nullptr, XMLoadFloat4x4(&view.viewProjMatrixNoJitter));
    auto* rays = static_cast<ParityRay*>(gfx.MapBuffer(m_rays));
    if (!rays) return;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const float ndcX = (static_cast<float>(x) + 0.5f) / kWidth * 2.0f - 1.0f;
            const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / kHeight * 2.0f;
            const XMVECTOR nearP = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inv);
            const XMVECTOR midP  = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.5f, 1.0f), inv);
            XMFLOAT3 o, d;
            XMStoreFloat3(&o, nearP);
            XMStoreFloat3(&d, XMVector3Normalize(XMVectorSubtract(midP, nearP)));
            ParityRay& r = rays[y * kWidth + x];
            r.origin[0] = o.x;    r.origin[1] = o.y;    r.origin[2] = o.z;
            r.direction[0] = d.x; r.direction[1] = d.y; r.direction[2] = d.z;
            r.tMin = 0.0f;
            r.tMax = 1.0e5f;
        }
    gfx.UnmapBuffer(m_rays);

    const uint32_t params[4] = { kWidth, kHeight, kRays, 0 };
    auto bindArgs = [&](const RHI::GPUBuffer& hits)
    {
        gfx.SetComputeRootSRVAddress(kSlotTLAS, tlasAddress, cmd);
        gfx.SetComputeRootSRVAddress(kSlotRays, gfx.GetBufferGPUAddress(m_rays), cmd);
        gfx.SetComputeDescriptorTable(kSlotHits, gfx.GetBufferUAVGpuHandle(hits), cmd);
        gfx.SetComputeRootConstants(kSlotParams, 4, params, 0, cmd);
    };

    gfx.BindDescriptorHeaps(cmd);
    const RHI::GPUBarrier toUav[2] = {
        RHI::GPUBarrier::Buffer(&m_hits[0], RHI::ResourceState::UNDEFINED, RHI::ResourceState::UNORDERED_ACCESS),
        RHI::GPUBarrier::Buffer(&m_hits[1], RHI::ResourceState::UNDEFINED, RHI::ResourceState::UNORDERED_ACCESS) };
    gfx.PushBarriers(toUav, 2, cmd);

    // Inline RayQuery: one thread a ray.
    gfx.BindComputePipelineState(m_queryPSO, cmd);
    bindArgs(m_hits[1]);
    gfx.DispatchCompute((kRays + 63) / 64, 1, 1, cmd);

    // DXR: one launch a ray. Binding the state object sets its global root
    // signature, so the arguments are set again after it.
    gfx.BindRaytracingPipeline(m_dxr, cmd);
    bindArgs(m_hits[0]);
    gfx.DispatchRays(m_dxr, kWidth, kHeight, 1, cmd);

    const RHI::GPUBarrier toCopy[2] = {
        RHI::GPUBarrier::Buffer(&m_hits[0], RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::COPY_SRC),
        RHI::GPUBarrier::Buffer(&m_hits[1], RHI::ResourceState::UNORDERED_ACCESS, RHI::ResourceState::COPY_SRC) };
    gfx.PushBarriers(toCopy, 2, cmd);
    for (int i = 0; i < 2; ++i)
        gfx.CopyBuffer(m_hits[i], m_readback[i], uint64_t(kRays) * sizeof(ParityHit), cmd);

    // Other work continues on this list: put the shared compute layout back
    // (the AutoExposure contract - RestoreDefaultComputeRootSignature).
    gfx.RestoreDefaultComputeRootSignature(cmd);
}

void DXRParityTest::Compare(IGraphicsDevice& gfx)
{
    const auto* dxr   = static_cast<const ParityHit*>(gfx.MapBuffer(m_readback[0]));
    const auto* query = static_cast<const ParityHit*>(gfx.MapBuffer(m_readback[1]));
    if (!dxr || !query)
    {
        if (dxr)   gfx.UnmapBuffer(m_readback[0]);
        if (query) gfx.UnmapBuffer(m_readback[1]);
        LOG_WARNING("[DXR] parity skipped: the readback did not map");
        return;
    }
    uint32_t hits = 0, misses = 0, hitMismatch = 0, idMismatch = 0, primMismatch = 0, tMismatch = 0;
    float    maxRelT = 0.0f;
    uint32_t reported = 0;
    for (uint32_t i = 0; i < kRays; ++i)
    {
        const ParityHit& d = dxr[i];
        const ParityHit& q = query[i];
        bool bad = false;
        if (d.hit != q.hit)                     { ++hitMismatch; bad = true; }
        else if (!d.hit)                        { ++misses; }
        else
        {
            ++hits;
            if (d.instanceID != q.instanceID)       { ++idMismatch;   bad = true; }
            if (d.primitiveIndex != q.primitiveIndex) { ++primMismatch; bad = true; }
            const float rel = std::fabs(d.t - q.t) / std::max(std::fabs(q.t), 1.0e-3f);
            maxRelT = std::max(maxRelT, rel);
            if (rel > kRelativeT)                   { ++tMismatch;    bad = true; }
        }
        if (bad && reported < 4)
        {
            ++reported;
            LOG_WARNING("[DXR] parity ray %u: dxr hit=%u t=%g instance=%u primitive=%u | rayquery hit=%u t=%g "
                        "instance=%u primitive=%u", i, d.hit, d.t, d.instanceID, d.primitiveIndex,
                        q.hit, q.t, q.instanceID, q.primitiveIndex);
        }
    }
    gfx.UnmapBuffer(m_readback[0]);
    gfx.UnmapBuffer(m_readback[1]);

    const bool ok = hits > 0 && hitMismatch == 0 && idMismatch == 0 && primMismatch == 0 && tMismatch == 0;
    LOG_INFO("[DXR] parity rays=%u hits=%u misses=%u hitMismatch=%u idMismatch=%u primMismatch=%u tMismatch=%u "
             "maxRelT=%.3g ok=%d", kRays, hits, misses, hitMismatch, idMismatch, primMismatch, tMismatch,
             static_cast<double>(maxRelT), ok ? 1 : 0);
}

void DXRParityTest::Tick(IGraphicsDevice& gfx, RHI::CommandList cmd, uint64_t tlasAddress, const RenderView& view)
{
    if (m_state == State::Done || !cmd.IsValid()) return;
    if (m_state == State::Waiting)
    {
        if (tlasAddress == 0) return;
        if (++m_framesWithTlas < kSettleFrames) return;
        if (!Prepare(gfx))
        {
            LOG_WARNING("[DXR] parity skipped: the pipelines or buffers could not be made");
            Shutdown(gfx);
            m_state = State::Done;
            return;
        }
        Record(gfx, cmd, tlasAddress, view);
        LOG_INFO("[DXR] parity recorded: %u x %u camera rays, DXR library and inline RayQuery", kWidth, kHeight);
        m_state             = State::Recorded;
        m_framesSinceRecord = 0;
        return;
    }
    if (++m_framesSinceRecord < kReadbackDelay) return;
    Compare(gfx);
    Shutdown(gfx);
    m_state = State::Done;
}

void DXRParityTest::Shutdown(IGraphicsDevice& gfx)
{
    if (m_dxr.IsValid())      gfx.DestroyRaytracingPipeline(m_dxr);
    if (m_queryPSO.IsValid()) gfx.DestroyPipelineState(m_queryPSO);
    if (m_layout.IsValid())   gfx.DestroyPipelineLayout(m_layout);
    if (m_rays.IsValid())     gfx.DestroyBuffer(m_rays);
    for (int i = 0; i < 2; ++i)
    {
        if (m_hits[i].IsValid())     gfx.DestroyBuffer(m_hits[i]);
        if (m_readback[i].IsValid()) gfx.DestroyBuffer(m_readback[i]);
    }
}
