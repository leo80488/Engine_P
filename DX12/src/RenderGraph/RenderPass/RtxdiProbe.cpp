#include "RenderGraph/RenderPass/RtxdiProbe.h"
#include "RenderGraph/RenderPass/ReSTIRBackend.h"
#include "RenderGraph/RenderPass/RtxdiBridge.h"
#include "Graphics/ComputePipelineBatch.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/ShaderDiskCache.h"
#include "Graphics/ShaderReflection.h"
#include "System/Log.h"
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#if ENGINE_WITH_RTXDI
#include <Rtxdi/ImportanceSamplingContext.h>
#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/DI/ReSTIRDI.h>
#include <Rtxdi/GI/ReSTIRGI.h>
#include <Rtxdi/PT/ReSTIRPT.h>
#include <Rtxdi/LightSampling/RISBufferSegmentAllocator.h>
#endif

namespace
{
#if ENGINE_WITH_RTXDI
// The probe TUs' cbuffers, member for member (RtxdiProbe.cs.hlsl,
// RtxdiProbePT.cs.hlsl). The RTXDI types are the library's shared C++/HLSL
// headers, so a layout disagreement here is one the RTXDI cores would have.
struct ProbeConstants
{
    RTXDI::BridgeConstants bridge;
    RTXDI_RuntimeParameters runtime;
    RTXDI_LightBufferParameters lightBuffer;
    RTXDI_RISBufferSegmentParameters localRIS;
    RTXDI_RISBufferSegmentParameters environmentRIS;
    RTXDI_ReservoirBufferParameters reservoirs;
    RTXDI_DIInitialSamplingParameters diInitial;
    RTXDI_DITemporalResamplingParameters diTemporal;
    RTXDI_DISpatialResamplingParameters diSpatial;
    RTXDI_GITemporalResamplingParameters giTemporal;
    RTXDI_GISpatialResamplingParameters giSpatial;
    uint32_t diReservoirUav, giReservoirUav, risUav, neighborOffsetsSrv;
    uint32_t frameIndex, pad0, pad1, pad2;
};
struct ProbePTConstants
{
    RTXDI::BridgeConstants bridge;
    RTXDI_RuntimeParameters runtime;
    RTXDI_PTParameters restirPT;        // g_Const.restirPT
    uint32_t ptReservoirUav, neighborOffsetsSrv, neighborSelectionUav, sampleIdUav;
    uint32_t duplicationUav, smoothedDuplicationUav, prevSmoothedDuplicationSrv, frameIndex;
};

struct Expect { const char* name; uint32_t offset, size; };
#define RTXDI_PROBE_VAR(hlsl, type, member) { hlsl, uint32_t(offsetof(type, member)), uint32_t(sizeof(type::member)) }
const Expect kDI[] = {
    RTXDI_PROBE_VAR("g_RabBridge", ProbeConstants, bridge),
    RTXDI_PROBE_VAR("g_Runtime", ProbeConstants, runtime),
    RTXDI_PROBE_VAR("g_LightBuffer", ProbeConstants, lightBuffer),
    RTXDI_PROBE_VAR("g_LocalRIS", ProbeConstants, localRIS),
    RTXDI_PROBE_VAR("g_EnvironmentRIS", ProbeConstants, environmentRIS),
    RTXDI_PROBE_VAR("g_Reservoirs", ProbeConstants, reservoirs),
    RTXDI_PROBE_VAR("g_DIInitial", ProbeConstants, diInitial),
    RTXDI_PROBE_VAR("g_DITemporal", ProbeConstants, diTemporal),
    RTXDI_PROBE_VAR("g_DISpatial", ProbeConstants, diSpatial),
    RTXDI_PROBE_VAR("g_GITemporal", ProbeConstants, giTemporal),
    RTXDI_PROBE_VAR("g_GISpatial", ProbeConstants, giSpatial),
    RTXDI_PROBE_VAR("g_DIReservoirUav", ProbeConstants, diReservoirUav),
    RTXDI_PROBE_VAR("g_FrameIndex", ProbeConstants, frameIndex),
};
const Expect kPT[] = {
    RTXDI_PROBE_VAR("g_RabBridge", ProbePTConstants, bridge),
    RTXDI_PROBE_VAR("g_Runtime", ProbePTConstants, runtime),
    RTXDI_PROBE_VAR("g_Const", ProbePTConstants, restirPT),
    RTXDI_PROBE_VAR("g_PTReservoirUav", ProbePTConstants, ptReservoirUav),
    RTXDI_PROBE_VAR("g_FrameIndex", ProbePTConstants, frameIndex),
};
#undef RTXDI_PROBE_VAR

// Every expected cbuffer member found at the C++ offset with the C++ size,
// and the cbuffer no larger than the C++ struct (rounded to a register).
// A member the reflection does not list is a mismatch too.
bool CheckLayout(const std::vector<uint8_t>& dxil, const char* tu, const Expect* vars, size_t count, size_t cppSize)
{
    ShaderReflect::Reflection r;
    if (!ShaderReflect::Reflect(dxil.data(), dxil.size(), r))
    { LOG_ERROR("[RTXDI] layout %s: no reflection", tu); return false; }
    const ShaderReflect::CBufferLayout* params = nullptr;
    for (const auto& cb : r.cbuffers) if (cb.name == "Params") params = &cb;
    if (!params) { LOG_ERROR("[RTXDI] layout %s: cbuffer Params not reflected", tu); return false; }
    bool ok = true;
    for (size_t i = 0; i < count; ++i)
    {
        const ShaderReflect::CBufferVar* v = nullptr;
        for (const auto& var : params->vars) if (var.name == vars[i].name) v = &var;
        if (!v) { LOG_ERROR("[RTXDI] layout %s.%s: not in the reflected cbuffer", tu, vars[i].name); ok = false; continue; }
        if (v->offset != vars[i].offset || v->size != vars[i].size)
        {
            LOG_ERROR("[RTXDI] layout %s.%s: hlsl offset=%u size=%u, c++ offset=%u size=%u",
                      tu, vars[i].name, v->offset, v->size, vars[i].offset, vars[i].size);
            ok = false;
        }
    }
    const size_t cppRounded = (cppSize + 15) & ~size_t(15);
    if (params->sizeBytes != cppRounded)
    {
        LOG_ERROR("[RTXDI] layout %s: cbuffer %u bytes, c++ %zu", tu, params->sizeBytes, cppRounded);
        ok = false;
    }
    return ok;
}
#endif
} // namespace

void RtxdiProbe::Release(IGraphicsDevice& gfx)
{
    for (auto& p : m_pso) gfx.DestroyPipelineState(p);
    gfx.DestroyPipelineLayout(m_layout);
    m_allOk = false;
}

void RtxdiProbe::ReloadShaders(IGraphicsDevice& gfx)
{
    gfx.WaitIdleAndReleaseDeferred();
    Release(gfx);
    Build(gfx, m_width, m_height);
}

void RtxdiProbe::Build(IGraphicsDevice& gfx, uint32_t renderWidth, uint32_t renderHeight)
{
    m_width = renderWidth; m_height = renderHeight;
    ++m_builds;
    const auto start = std::chrono::steady_clock::now();
    auto elapsedMs = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
#if !ENGINE_WITH_RTXDI
    m_allOk = false;
    LOG_INFO("[RTXDI] probe build=%u sdk=%s di=skipped:no_sdk gi=skipped:no_sdk pt=skipped:no_sdk layout=skipped ms=%.1f",
             m_builds, ReSTIR::RtxdiSdkVersion(), elapsedMs());
#else
    // The library's C++ half, once: the same contexts the RTXDI cores own.
    rtxdi::ImportanceSamplingContext_StaticParameters sp;
    sp.renderWidth = renderWidth; sp.renderHeight = renderHeight;
    rtxdi::ImportanceSamplingContext isc(sp);
    isc.GetReSTIRDIContext().SetFrameIndex(1);
    const RTXDI_ReservoirBufferParameters diRes = isc.GetReSTIRDIContext().GetReservoirBufferParameters();
    const RTXDI_ReservoirBufferParameters ptRes = isc.GetReSTIRPTContext().GetReservoirBufferParameters();
    std::vector<uint8_t> offsets(size_t(isc.GetNeighborOffsetCount()) * 2);
    rtxdi::FillNeighborOffsetBuffer(offsets.data(), isc.GetNeighborOffsetCount());
    uint32_t pdfW = 0, pdfH = 0, pdfMips = 0;
    rtxdi::ComputePdfTextureSize(4096, pdfW, pdfH, pdfMips);
    LOG_INFO("[RTXDI] context sdk=%s render=%ux%u diArrayPitch=%u ptArrayPitch=%u neighborOffsets=%u risElements=%u pdf4096=%ux%u/%u",
             ReSTIR::RtxdiSdkVersion(), renderWidth, renderHeight, diRes.reservoirArrayPitch, ptRes.reservoirArrayPitch,
             isc.GetNeighborOffsetCount(), isc.GetRISBufferSegmentAllocator().GetTotalSizeInElements(), pdfW, pdfH, pdfMips);

    if (!m_layout.IsValid() && !ReSTIR::CreateShellLayout(gfx, m_layout, "RtxdiProbe"))
        LOG_ERROR("[RTXDI] probe: pipeline layout failed");
    struct Tu { const char* file; const char* entry; ShaderDiskCache::CompileFlags flags; bool pt; };
    static constexpr Tu kTus[] = {
        {"shaders/RtxdiProbe.cs.hlsl", "ProbeDI", {false, false}, false},
        {"shaders/RtxdiProbe.cs.hlsl", "ProbeGI", {false, false}, false},
        {"shaders/RtxdiProbePT.cs.hlsl", "ProbePT", {true, true}, true},
    };
    const bool native16 = gfx.GetCapabilities().native16BitShaderOps;
    std::string verdict[3];
    bool layoutOk = true, allOk = m_layout.IsValid();
    for (size_t i = 0; i < 3; ++i)
    {
        if (kTus[i].pt && !native16) { verdict[i] = "skipped:no_native16"; continue; }
        const auto dxil = m_layout.IsValid() ? ShaderDiskCache::LoadOrCompile(kTus[i].file, kTus[i].entry, RHI::ShaderStage::CS, {},
                                                                             "RtxdiProbe", kTus[i].flags)
                                             : std::vector<uint8_t>{};
        if (dxil.empty() || !ComputePipelineBatch::Create(gfx, m_layout, &dxil, 1, &m_pso[i]))
        {
            LOG_ERROR("[RTXDI] probe: pipeline %s failed", kTus[i].entry);
            verdict[i] = "failed"; allOk = false;
            continue;
        }
        verdict[i] = "ok";
        if (i == 0) layoutOk &= CheckLayout(dxil, "RtxdiProbe", kDI, std::size(kDI), sizeof(ProbeConstants));
        if (i == 2) layoutOk &= CheckLayout(dxil, "RtxdiProbePT", kPT, std::size(kPT), sizeof(ProbePTConstants));
    }
    m_allOk = allOk && layoutOk;
    LOG_INFO("[RTXDI] probe build=%u sdk=%s di=%s gi=%s pt=%s layout=%s ms=%.1f",
             m_builds, ReSTIR::RtxdiSdkVersion(), verdict[0].c_str(), verdict[1].c_str(), verdict[2].c_str(),
             layoutOk ? "ok" : "mismatch", elapsedMs());
#endif
}
