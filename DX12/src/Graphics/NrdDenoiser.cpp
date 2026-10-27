#include "Graphics/NrdDenoiser.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "Graphics/GraphicsDX12.h"
#include "System/Log.h"
#include "Harness/Determinism.h"

#include <algorithm>   // (std::min)/(std::max): parenthesised, the Windows macros are live
#include <chrono>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
// nrd::Format -> DXGI. Only the formats the pools actually allocate are guaranteed mapped;
// anything else returns UNKNOWN and fails loudly at pool creation.
DXGI_FORMAT ToDxgi(nrd::Format f)
{
    switch (f)
    {
        case nrd::Format::R8_UNORM:              return DXGI_FORMAT_R8_UNORM;
        case nrd::Format::R8_UINT:               return DXGI_FORMAT_R8_UINT;
        case nrd::Format::RG8_UNORM:             return DXGI_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_UINT:              return DXGI_FORMAT_R8G8_UINT;
        case nrd::Format::RGBA8_UNORM:           return DXGI_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM:           return DXGI_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::R16_UNORM:             return DXGI_FORMAT_R16_UNORM;
        case nrd::Format::R16_UINT:              return DXGI_FORMAT_R16_UINT;
        case nrd::Format::R16_SFLOAT:            return DXGI_FORMAT_R16_FLOAT;
        case nrd::Format::RG16_UNORM:            return DXGI_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM:            return DXGI_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_SFLOAT:           return DXGI_FORMAT_R16G16_FLOAT;
        case nrd::Format::RGBA16_UNORM:          return DXGI_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM:          return DXGI_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_SFLOAT:         return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case nrd::Format::R32_UINT:              return DXGI_FORMAT_R32_UINT;
        case nrd::Format::R32_SFLOAT:            return DXGI_FORMAT_R32_FLOAT;
        case nrd::Format::RG32_UINT:             return DXGI_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SFLOAT:           return DXGI_FORMAT_R32G32_FLOAT;
        case nrd::Format::RGBA32_UINT:           return DXGI_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SFLOAT:         return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM:  return DXGI_FORMAT_R10G10B10A2_UNORM;
        case nrd::Format::R10_G10_B10_A2_UINT:   return DXGI_FORMAT_R10G10B10A2_UINT;
        case nrd::Format::R11_G11_B10_UFLOAT:    return DXGI_FORMAT_R11G11B10_FLOAT;
        case nrd::Format::R9_G9_B9_E5_UFLOAT:    return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default:                                 return DXGI_FORMAT_UNKNOWN;
    }
}

// One denoiser per instance, always registered at identifier 0 - except
// RelaxDiffuseSpecularSplit, whose RELAX_SPECULAR half is identifier 1.
constexpr nrd::Identifier kDenoiserId = 0;
constexpr nrd::Identifier kSpecularDenoiserId = 1;

nrd::Denoiser ToNrdDenoiser(NrdDenoiser::Kind k)
{
    switch (k)
    {
        case NrdDenoiser::Kind::SigmaShadow:           return nrd::Denoiser::SIGMA_SHADOW;
        case NrdDenoiser::Kind::SigmaShadowTranslucency: return nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY;
        case NrdDenoiser::Kind::ReblurDiffuseOcclusion: return nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION;
        case NrdDenoiser::Kind::ReblurDiffuse:          return nrd::Denoiser::REBLUR_DIFFUSE;
        case NrdDenoiser::Kind::ReblurDiffuseSpecular:  return nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        case NrdDenoiser::Kind::Reference:             return nrd::Denoiser::REFERENCE;
        case NrdDenoiser::Kind::RelaxDiffuseSpecular:  return nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
        case NrdDenoiser::Kind::RelaxDiffuse:          return nrd::Denoiser::RELAX_DIFFUSE;
        // Identifier 0 of the split pair; RELAX_SPECULAR is added at creation.
        case NrdDenoiser::Kind::RelaxDiffuseSpecularSplit: return nrd::Denoiser::RELAX_DIFFUSE;
        case NrdDenoiser::Kind::ReblurSpecularPathTraced:
        case NrdDenoiser::Kind::ReblurSpecular:
        default:                                        return nrd::Denoiser::REBLUR_SPECULAR;
    }
}

const char* KindName(NrdDenoiser::Kind k)
{
    switch (k)
    {
        case NrdDenoiser::Kind::SigmaShadow:            return "SIGMA_SHADOW";
        case NrdDenoiser::Kind::SigmaShadowTranslucency: return "SIGMA_SHADOW_TRANSLUCENCY";
        case NrdDenoiser::Kind::ReblurDiffuseOcclusion: return "REBLUR_DIFFUSE_OCCLUSION";
        case NrdDenoiser::Kind::ReblurDiffuse:          return "REBLUR_DIFFUSE";
        case NrdDenoiser::Kind::ReblurDiffuseSpecular:  return "REBLUR_DIFFUSE_SPECULAR";
        case NrdDenoiser::Kind::Reference:             return "REFERENCE";
        case NrdDenoiser::Kind::RelaxDiffuseSpecular:  return "RELAX_DIFFUSE_SPECULAR";
        case NrdDenoiser::Kind::RelaxDiffuse:          return "RELAX_DIFFUSE";
        case NrdDenoiser::Kind::RelaxDiffuseSpecularSplit: return "RELAX_DIFFUSE+RELAX_SPECULAR";
        case NrdDenoiser::Kind::ReblurSpecularPathTraced: return "REBLUR_SPECULAR(path-traced)";
        default:                                        return "REBLUR_SPECULAR";
    }
}

uint32_t DivRoundUp(uint32_t a, uint32_t b) { return (a + b - 1u) / b; }
} // namespace

NrdDenoiser::~NrdDenoiser()
{
    Shutdown();
}

void NrdDenoiser::Shutdown()
{
    DestroyGpuObjects();
    if (m_instance)
    {
        nrd::DestroyInstance(*m_instance);
        m_instance = nullptr;
    }
    m_ready = false;
    m_width = m_height = 0;
    m_havePrev = false;
}

void NrdDenoiser::DestroyGpuObjects()
{
    // GPU idle is guaranteed by Resize (WaitIdleAndReleaseDeferred before the
    // teardown) and by engine shutdown for the destructor path.
    m_rootSigs.clear();
    m_psos.clear();
    m_permanent.clear();
    m_transient.clear();
    m_descHeap.Reset();
    if (m_cbUpload && m_cbMapped)
        m_cbUpload->Unmap(0, nullptr);
    m_cbMapped = nullptr;
    m_cbUpload.Reset();
}

bool NrdDenoiser::Resize(IGraphicsDevice& device, uint32_t width, uint32_t height)
{
    // NRD records into native lists and owns D3D12 objects: backend-only code, its callers stay in
    // RHI terms.
    auto& gfx = static_cast<GraphicsDX12&>(device);
    if (width == 0 || height == 0) return false;
    if (m_ready && width == m_width && height == m_height) return true;

    // Frames recorded up to kFrameCount-1 frames ago may still reference the heap / PSOs / pool
    // textures about to be released, and the engine hard-flushes only on OS window resizes, not on
    // editor-viewport resizes (which reach here through the render-size graph recompile). Drain the GPU
    // first, per the engine's resize convention; resizes are rare enough for the stall.
    const bool hadGpuObjects = !m_psos.empty() || !m_permanent.empty() ||
                               !m_transient.empty() || m_descHeap;
    if (hadGpuObjects)
        gfx.WaitIdleAndReleaseDeferred();

    // Full teardown: pool sizes and (the first time) instance/pipelines change.
    const bool hadInstance = (m_instance != nullptr);
    DestroyGpuObjects();
    m_ready  = false;
    m_width  = width;
    m_height = height;
    m_havePrev = false;

    if (!m_instance)
    {
        nrd::DenoiserDesc denoisers[] = {
            { kDenoiserId, ToNrdDenoiser(m_kind) },
            { kSpecularDenoiserId, nrd::Denoiser::RELAX_SPECULAR },
        };
        nrd::InstanceCreationDesc icd{};
        icd.denoisers    = denoisers;
        icd.denoisersNum = (m_kind == Kind::RelaxDiffuseSpecularSplit) ? 2u : 1u;
        if (nrd::CreateInstance(icd, m_instance) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: nrd::CreateInstance failed");
            m_instance = nullptr;
            return false;
        }
        // Sanity: the library must be built with the encodings the pack shaders assume (their
        // NRD_NORMAL_ENCODING / NRD_ROUGHNESS_ENCODING defines, e.g. HybridReflNRDPack.cs.hlsl).
        const nrd::LibraryDesc* lib = nrd::GetLibraryDesc();
        if (lib &&
            (lib->normalEncoding    != nrd::NormalEncoding::R10_G10_B10_A2_UNORM ||
             lib->roughnessEncoding != nrd::RoughnessEncoding::LINEAR))
        {
            LOG_ERROR("NrdDenoiser: NRD.lib encodings (normal=%u rough=%u) do not "
                      "match the pack shader (normal=2 linear rough=1) — rebuild NRD",
                      (uint32_t)lib->normalEncoding, (uint32_t)lib->roughnessEncoding);
            // Destroy the instance so the NEXT Resize re-runs this check; leaving it alive would skip the
            // '!m_instance' block and set m_ready with mismatched encodings.
            nrd::DestroyInstance(*m_instance);
            m_instance = nullptr;
            return false;
        }
    }
    (void)hadInstance;

    if (!CreateInstanceAndPipelines(gfx)) return false;
    if (!CreatePools(gfx))                return false;

    m_ready = true;
    const auto poolBytes = [&](const std::vector<PoolTexture>& pool) {
        uint64_t bytes = 0;
        for (const auto& texture : pool)
        {
            const auto desc = texture.resource->GetDesc();
            bytes += gfx.GetDevice()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        }
        return bytes;
    };
    const uint64_t permanentBytes = poolBytes(m_permanent);
    const uint64_t transientBytes = poolBytes(m_transient);
    LOG_INFO("NrdDenoiser: %s ready at %ux%u (%zu pipelines, %zu+%zu pool textures) permanentBytes=%llu transientBytes=%llu poolBytes=%llu",
             KindName(m_kind), width, height, m_psos.size(), m_permanent.size(), m_transient.size(),
             static_cast<unsigned long long>(permanentBytes), static_cast<unsigned long long>(transientBytes),
             static_cast<unsigned long long>(permanentBytes + transientBytes));
    return true;
}

bool NrdDenoiser::CreateInstanceAndPipelines(GraphicsDX12& gfx)
{
    ID3D12Device* dev = gfx.GetDevice();
    const nrd::InstanceDesc* inst = nrd::GetInstanceDesc(*m_instance);
    if (!dev || !inst) return false;

    // ---- Static samplers (shared by every pipeline) ----
    D3D12_STATIC_SAMPLER_DESC samplers[(uint32_t)nrd::Sampler::MAX_NUM] = {};
    const uint32_t samplerCount = inst->samplersNum;
    for (uint32_t i = 0; i < samplerCount && i < (uint32_t)nrd::Sampler::MAX_NUM; ++i)
    {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[i];
        s.Filter = (inst->samplers[i] == nrd::Sampler::NEAREST_CLAMP)
                 ? D3D12_FILTER_MIN_MAG_MIP_POINT
                 : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD           = D3D12_FLOAT32_MAX;
        s.ShaderRegister   = inst->samplersBaseRegisterIndex + i;
        s.RegisterSpace    = inst->constantBufferAndSamplersSpaceIndex;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    // ---- One root signature + PSO per NRD pipeline ----
    m_rootSigs.resize(inst->pipelinesNum);
    m_psos.resize(inst->pipelinesNum);

    for (uint32_t p = 0; p < inst->pipelinesNum; ++p)
    {
        const nrd::PipelineDesc& pd = inst->pipelines[p];

        D3D12_ROOT_PARAMETER params[3] = {};
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        uint32_t paramCount = 0;

        // [0] root CBV: every NRD pipeline with constants expects it at the same register; declaring it
        // when unused is harmless.
        params[paramCount].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[paramCount].Descriptor.ShaderRegister = inst->constantBufferRegisterIndex;
        params[paramCount].Descriptor.RegisterSpace  = inst->constantBufferAndSamplersSpaceIndex;
        params[paramCount].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
        ++paramCount;

        // [1..2] one descriptor table per resource range (TEXTURE -> SRVs, STORAGE_TEXTURE -> UAVs),
        // registers starting at the shared base.
        for (uint32_t r = 0; r < pd.resourceRangesNum && r < 2; ++r)
        {
            const nrd::ResourceRangeDesc& rr = pd.resourceRanges[r];
            ranges[r].RangeType =
                (rr.descriptorType == nrd::DescriptorType::TEXTURE)
                ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[r].NumDescriptors     = rr.descriptorsNum;
            ranges[r].BaseShaderRegister = inst->resourcesBaseRegisterIndex;
            ranges[r].RegisterSpace      = inst->resourcesSpaceIndex;
            ranges[r].OffsetInDescriptorsFromTableStart = 0;

            params[paramCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[paramCount].DescriptorTable.NumDescriptorRanges = 1;
            params[paramCount].DescriptorTable.pDescriptorRanges   = &ranges[r];
            params[paramCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            ++paramCount;
        }

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = paramCount;
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = samplerCount;
        rsd.pStaticSamplers   = samplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            LOG_ERROR("NrdDenoiser: root sig serialize failed for pipeline %u (%s): %s",
                      p, pd.shaderIdentifier,
                      err ? (const char*)err->GetBufferPointer() : "");
            return false;
        }
        if (FAILED(dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                            IID_PPV_ARGS(&m_rootSigs[p]))))
        {
            LOG_ERROR("NrdDenoiser: CreateRootSignature failed for pipeline %u", p);
            return false;
        }

        if (!pd.computeShaderDXIL.bytecode || pd.computeShaderDXIL.size == 0)
        {
            LOG_ERROR("NrdDenoiser: pipeline %u has no embedded DXIL — NRD.lib must "
                      "be built with NRD_EMBEDS_DXIL_SHADERS=ON", p);
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd{};
        cd.pRootSignature     = m_rootSigs[p].Get();
        cd.CS.pShaderBytecode = pd.computeShaderDXIL.bytecode;
        cd.CS.BytecodeLength  = pd.computeShaderDXIL.size;
        // Through the device's PSO library (keyed by this root signature's blob and the kernel's DXIL), so
        // a warm start loads NRD's kernels instead of handing them to the driver again.
        if (!gfx.CreateComputePipelineCached(cd, sig->GetBufferPointer(), sig->GetBufferSize(), m_psos[p]))
        {
            LOG_ERROR("NrdDenoiser: PSO creation failed for pipeline %u (%s)",
                      p, pd.shaderIdentifier);
            return false;
        }
    }

    // ---- Descriptor ring ----
    const nrd::DescriptorPoolDesc& pool = inst->descriptorPoolDesc;
    m_descPerFrame = (pool.totalTexturesNum + pool.totalStorageTexturesNum) * 2u + 64u;
    m_descSize = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = m_descPerFrame * GraphicsDX12::kFrameCount;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_descHeap))))
    {
        LOG_ERROR("NrdDenoiser: descriptor heap creation failed (%u slots)",
                  hd.NumDescriptors);
        return false;
    }
    GpuLedger::TrackDescriptorHeap(dev, m_descHeap.Get(), "NRD.DescriptorHeap");

    // ---- Constant-buffer upload ring ----
    m_cbSliceSize      = (inst->constantBufferMaxDataSize + 255u) & ~255u;
    m_cbSlicesPerFrame = pool.setsMaxNum + 8u;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = uint64_t(m_cbSliceSize) * m_cbSlicesPerFrame * GraphicsDX12::kFrameCount;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                            IID_PPV_ARGS(&m_cbUpload))))
    {
        LOG_ERROR("NrdDenoiser: CB upload ring creation failed (%llu bytes)",
                  (unsigned long long)bd.Width);
        return false;
    }
    GpuLedger::TrackCommitted(dev, m_cbUpload.Get(), Diag::Prof::GpuMemCat::Upload, "NRD.ConstantsUpload");
    if (FAILED(m_cbUpload->Map(0, nullptr, reinterpret_cast<void**>(&m_cbMapped))))
    {
        LOG_ERROR("NrdDenoiser: CB upload map failed");
        return false;
    }
    return true;
}

bool NrdDenoiser::CreatePools(GraphicsDX12& gfx)
{
    ID3D12Device* dev = gfx.GetDevice();
    const nrd::InstanceDesc* inst = nrd::GetInstanceDesc(*m_instance);
    if (!dev || !inst) return false;

    uint64_t poolBytes=0;
    auto createPool = [&](const nrd::TextureDesc* descs, uint32_t count,
                          std::vector<PoolTexture>& out, const wchar_t* tag) -> bool
    {
        out.resize(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const nrd::TextureDesc& td = descs[i];
            DXGI_FORMAT fmt = ToDxgi(td.format);
            if((m_diFloat32Pool && (m_kind==Kind::ReblurDiffuseSpecular || m_kind==Kind::RelaxDiffuseSpecular || m_kind==Kind::RelaxDiffuseSpecularSplit)) ||
               (m_giFloat32Pool && (m_kind==Kind::ReblurDiffuse || m_kind==Kind::RelaxDiffuse)))
            {
                if(fmt==DXGI_FORMAT_R16_FLOAT) fmt=DXGI_FORMAT_R32_FLOAT;
                else if(fmt==DXGI_FORMAT_R16G16_FLOAT) fmt=DXGI_FORMAT_R32G32_FLOAT;
                else if(fmt==DXGI_FORMAT_R16G16B16A16_FLOAT) fmt=DXGI_FORMAT_R32G32B32A32_FLOAT;
            }
            if (fmt == DXGI_FORMAT_UNKNOWN)
            {
                LOG_ERROR("NrdDenoiser: unmapped nrd::Format %u (pool tex %u)",
                          (uint32_t)td.format, i);
                return false;
            }
            const uint32_t f = td.downsampleFactor ? td.downsampleFactor : 1u;

            D3D12_HEAP_PROPERTIES hp{};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = (std::max)(1u, DivRoundUp(m_width,  f));
            rd.Height           = (std::max)(1u, DivRoundUp(m_height, f));
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = fmt;
            rd.SampleDesc.Count = 1;
            rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            PoolTexture& t = out[i];
            t.format = fmt;
            t.state  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    t.state, nullptr,
                                                    IID_PPV_ARGS(&t.resource))))
            {
                LOG_ERROR("NrdDenoiser: pool texture %u creation failed", i);
                return false;
            }
            wchar_t name[64];
            swprintf_s(name, L"NRD.%s[%u]", tag, i);
            t.resource->SetName(name);
            poolBytes+=dev->GetResourceAllocationInfo(0,1,&rd).SizeInBytes;
            char nameA[64];
            snprintf(nameA, sizeof(nameA), "NRD.%ls[%u]", tag, i);
            GpuLedger::TrackTexture(dev, t.resource.Get(), nameA);
        }
        return true;
    };

    if (!createPool(inst->permanentPool, inst->permanentPoolSize, m_permanent, L"Perm"))
        return false;
    if (!createPool(inst->transientPool, inst->transientPoolSize, m_transient, L"Trans"))
        return false;
    LOG_INFO("[NRD_POOL] kind=%u width=%u height=%u bytes=%llu fp32=%u",static_cast<uint32_t>(m_kind),
        m_width,m_height,static_cast<unsigned long long>(poolBytes),
        ((m_diFloat32Pool && (m_kind==Kind::ReblurDiffuseSpecular || m_kind==Kind::RelaxDiffuseSpecular || m_kind==Kind::RelaxDiffuseSpecularSplit)) || (m_giFloat32Pool && (m_kind==Kind::ReblurDiffuse || m_kind==Kind::RelaxDiffuse)))?1u:0u);
    return true;
}

bool NrdDenoiser::Denoise(IGraphicsDevice& device, RHI::CommandList cl, const Params& p,
                          const Binding* bindings, uint32_t bindingCount)
{
    auto& gfx = static_cast<GraphicsDX12&>(device);   // as Resize
    if (!IsReady()) return false;
    if (!bindings || bindingCount == 0) return false;
    ID3D12GraphicsCommandList* native = gfx.GetNativeCommandList(cl);
    ID3D12Device*              dev    = gfx.GetDevice();
    if (!native || !dev) return false;

    // ---- Common settings ----
    nrd::CommonSettings cs{};
    std::memcpy(cs.viewToClipMatrix,      p.viewToClip,   sizeof(cs.viewToClipMatrix));
    std::memcpy(cs.worldToViewMatrix,     p.worldToView,  sizeof(cs.worldToViewMatrix));
    std::memcpy(cs.viewToClipMatrixPrev,  m_havePrev ? m_prevViewToClip  : p.viewToClip,
                sizeof(cs.viewToClipMatrixPrev));
    std::memcpy(cs.worldToViewMatrixPrev, m_havePrev ? m_prevWorldToView : p.worldToView,
                sizeof(cs.worldToViewMatrixPrev));
    cs.cameraJitter[0]     = p.jitter[0];
    cs.cameraJitter[1]     = p.jitter[1];
    cs.cameraJitterPrev[0] = m_havePrev ? m_prevJitter[0] : p.jitter[0];
    cs.cameraJitterPrev[1] = m_havePrev ? m_prevJitter[1] : p.jitter[1];
    cs.resourceSize[0] = cs.resourceSizePrev[0] = (uint16_t)p.width;
    cs.resourceSize[1] = cs.resourceSizePrev[1] = (uint16_t)p.height;
    cs.rectSize[0]     = cs.rectSizePrev[0]     = (uint16_t)p.width;
    cs.rectSize[1]     = cs.rectSizePrev[1]     = (uint16_t)p.height;
    cs.denoisingRange = p.denoisingRange;
    cs.frameIndex     = p.frameIndex;
    // The harness fixes simulation and shader time; NRD's internal clock and
    // our seconds-to-history conversion must use the same cadence as well.
    // Without this, faster test arms silently get longer temporal histories.
    const float fixedDt = Harness::Determinism::Get().FixedDt();
    if (fixedDt > 0.f) cs.timeDeltaBetweenFrames = fixedDt * 1000.f; // NRD: milliseconds
    // The pack shaders (e.g. HybridReflNRDPack.cs.hlsl) convert the engine's velocity buffer to
    // "pixelUvPrev = pixelUv + mv.xy" UV-space deltas, so no scaling.
    cs.motionVectorScale[0] = 1.0f;
    cs.motionVectorScale[1] = 1.0f;
    cs.motionVectorScale[2] = 0.0f;
    cs.isMotionVectorInWorldSpace = false;
    cs.isHistoryConfidenceAvailable = p.historyConfidenceAvailable;
    cs.accumulationMode = (p.resetHistory || !m_havePrev)
                        ? nrd::AccumulationMode::CLEAR_AND_RESTART
                        : nrd::AccumulationMode::CONTINUE;

    if (nrd::SetCommonSettings(*m_instance, cs) != nrd::Result::SUCCESS)
    {
        LOG_ERROR("NrdDenoiser: SetCommonSettings failed");
        return false;
    }

    // ---- Frame-rate estimate (EMA of the wall-clock delta between calls) ----
    // REBLUR history lengths are in FRAMES, while NRD's header says to accumulate in seconds: a frame
    // count tuned at 180 fps is a trail 3x longer in wall-clock time at 60 fps.
    {
        const long long nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (m_lastDenoiseUs > 0)
        {
            const float dtSec = float(nowUs - m_lastDenoiseUs) * 1e-6f;
            // Ignore pauses / breakpoints / resizes; accept 30..480 fps.
            if (dtSec > 1.0f / 480.0f && dtSec < 1.0f / 30.0f)
            {
                const float fps = 1.0f / dtSec;
                m_smoothedFps = (m_smoothedFps <= 0.0f)
                              ? fps
                              : m_smoothedFps + 0.1f * (fps - m_smoothedFps);
            }
        }
        m_lastDenoiseUs = nowUs;
    }
    const float fps = fixedDt > 0.f ? (std::min)(480.f, 1.f / fixedDt) : ((m_smoothedFps > 0.0f) ? m_smoothedFps : 60.0f);
    if (fixedDt > 0.f && (!m_havePrev || p.frameIndex % 120 == 0))
        LOG_INFO("[NRD_CADENCE] kind=%u frame=%u dtMs=%.6f historyFps=%.6f fixed=1",
            static_cast<unsigned>(m_kind), p.frameIndex, cs.timeDeltaBetweenFrames, fps);
    // seconds -> frames at the CURRENT frame rate, clamped so low fps never starves accumulation below
    // the noise floor and high fps never exceeds the tuned frame-count ceilings.
    const auto framesFor = [fps](float seconds, uint32_t lo, uint32_t hi)
    {
        const float f = seconds * fps + 0.5f;
        const uint32_t n = (f < 1.0f) ? 1u : uint32_t(f);
        return (n < lo) ? lo : (n > hi) ? hi : n;
    };

    // ---- Per-denoiser settings ----
    // BOTH SIGMA kinds must land here. NRD's SetDenoiserSettings copies `settingsSize` bytes blind and
    // SIGMA_SHADOW_TRANSLUCENCY declares sizeof(SigmaSettings), so a translucency instance falling
    // through to the REBLUR else-branch would SUCCEED while reading ReblurSettings bytes as
    // lightDirection / planeDistanceSensitivity / maxStabilizedFrameNum: a mis-oriented penumbra
    // search, not a visible wiring error.
    if (m_kind == Kind::SigmaShadow || m_kind == Kind::SigmaShadowTranslucency)
    {
        // SIGMA sun shadows. lightDirection is required for a directional
        // source (it drives the penumbra-direction search). Stabilization =
        // the built-in temporal pass (needs IN_MV + OUT_SHADOW_TRANSLUCENCY
        // history); SIGMA history caps at 7 frames.
        nrd::SigmaSettings ss{};
        ss.lightDirection[0] = p.lightDirection[0];
        ss.lightDirection[1] = p.lightDirection[1];
        ss.lightDirection[2] = p.lightDirection[2];
        ss.planeDistanceSensitivity = 0.02f;
        ss.maxStabilizedFrameNum    = framesFor(0.084f, 2u, 7u);  // SIGMA_DEFAULT_ACCUMULATION_TIME
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &ss) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (SIGMA) failed");
            return false;
        }
    }
    else if (m_kind == Kind::Reference)
    {
        nrd::ReferenceSettings rs{};
        rs.maxAccumulatedFrameNum = 120;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (REFERENCE) failed");
            return false;
        }
    }
    else if (m_kind == Kind::RelaxDiffuseSpecular || m_kind == Kind::RelaxDiffuseSpecularSplit)
    {
        // ReSTIR DI (RTDIPass). NRD's recommended denoiser for RTXDI signals.
        // Radii are in NRD resource pixels; the trace-stride scale applies.
        // The split kind gives each lobe its own denoiser with the SAME
        // settings, except anti-firefly: diAntiFirefly drives the diffuse one,
        // diSpecularAntiFirefly the specular one.
        nrd::RelaxSettings rs{};
        const float radiusScale=std::clamp(p.diBlurRadiusScale,0.25f,1.0f);
        rs.diffuseMaxAccumulatedFrameNum = rs.specularMaxAccumulatedFrameNum = framesFor(0.500f, 8u, 60u);
        rs.diffuseMaxFastAccumulatedFrameNum = rs.specularMaxFastAccumulatedFrameNum = framesFor(0.100f, 4u, 12u);
        rs.historyFixFrameNum = 3u;
        rs.diffusePrepassBlurRadius *= radiusScale;
        rs.specularPrepassBlurRadius *= radiusScale;
        if(!p.diPrepass) rs.diffusePrepassBlurRadius = rs.specularPrepassBlurRadius = 0.f;
        rs.enableAntiFirefly = p.diAntiFirefly;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (%s) failed", KindName(m_kind));
            return false;
        }
        if (m_kind == Kind::RelaxDiffuseSpecularSplit)
        {
            rs.enableAntiFirefly = p.diSpecularAntiFirefly;
            if (nrd::SetDenoiserSettings(*m_instance, kSpecularDenoiserId, &rs) != nrd::Result::SUCCESS)
            {
                LOG_ERROR("NrdDenoiser: SetDenoiserSettings (RELAX_SPECULAR) failed");
                return false;
            }
        }
    }
    else if (m_kind == Kind::RelaxDiffuse)
    {
        // ReSTIR GI (RTGIPass). Diffuse GI is low-frequency and its reservoir
        // input already carries tens of candidates, so it accumulates longer
        // than the DI signal; the pre-pass only takes the edge off the few
        // remaining sparkles and is kept narrow so contact detail survives.
        nrd::RelaxSettings rs{};
        rs.diffuseMaxAccumulatedFrameNum = (std::max)(framesFor(p.giHistorySeconds, 8u, 60u), (std::min)(p.giMinHistoryFrames, 60u));
        rs.diffuseMaxFastAccumulatedFrameNum = framesFor(0.100f, 4u, 12u);
        rs.historyFixFrameNum = 3u;
        rs.diffusePrepassBlurRadius = std::clamp(p.giPrepassRadius, 0.f, 60.f);
        rs.enableAntiFirefly = p.giAntiFirefly;
        rs.spatialVarianceEstimationHistoryThreshold = p.giVarianceHistory;
        rs.antilagSettings.resetAmount = p.giAntilagReset;
        rs.antilagSettings.spatialSigmaScale = p.giAntilagSpatialSigma;
        rs.atrousIterationNum = p.giAtrous;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (RELAX_DIFFUSE) failed");
            return false;
        }
    }
    else if (m_kind == Kind::ReblurDiffuseSpecular)
    {
        nrd::ReblurSettings rs{};
        rs.hitDistanceParameters.A = p.diHitDistScaleA;
        // NRD's internal resolutionScale is rect/resource, which is 1 for our
        // standalone half-resolution resources. Account for trace stride here.
        const float radiusScale=std::clamp(p.diBlurRadiusScale,0.25f,1.0f);
        rs.diffusePrepassBlurRadius*=radiusScale;
        rs.specularPrepassBlurRadius*=radiusScale;
        rs.minBlurRadius*=radiusScale;
        rs.maxBlurRadius*=radiusScale;
        if(!p.diPrepass) rs.diffusePrepassBlurRadius=rs.specularPrepassBlurRadius=0.f;
        rs.hitDistanceParameters.B = 0.f;
        rs.hitDistanceParameters.C = 1.f;
        // NRD's own REBLUR default accumulation time is 0.5 s (30 frames at 60 Hz); a shorter one starves
        // the DI signal of history. Responsiveness comes from anti-lag and the optional confidence input.
        rs.maxAccumulatedFrameNum = framesFor(0.500f, 8u, 60u);
        rs.maxFastAccumulatedFrameNum = framesFor(0.100f, 4u, 12u);
        rs.historyFixFrameNum = 3u;
        rs.maxStabilizedFrameNum = p.diTemporalStabilization?framesFor(0.250f, 4u, 30u):0u;
        rs.enableAntiFirefly = p.diAntiFirefly;
        rs.returnHistoryLengthInsteadOfOcclusion = p.diHistoryCounts;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (REBLUR_DIFFUSE_SPECULAR) failed");
            return false;
        }
    }
    else if (m_kind == Kind::ReblurDiffuse)
    {
        // REBLUR full diffuse radiance + hit distance (SSGIPass).
        // This branch must exist: the chain ends in a plain `else` with the REBLUR_SPECULAR tuning, and
        // nrd::SetDenoiserSettings takes a ReblurSettings for both, so a ReblurDiffuse instance falling
        // through would SUCCEED and denoise with mirror-reflection constants (hitDistanceParameters
        // {A,0.1,20}, a roughness rolloff meaningless for a diffuse lobe): globally wrong blur radii, no
        // error.
        // hitDistanceParameters {A = diffHitDistScaleA, B = 0, C = 1}, as on the AO path: the SSGI trace is
        // range-limited, so a max-length ray normalizes to exactly 1 (the specular default {3, 0.1, 20}
        // gives ~0.3, shrinking with distance).
        nrd::ReblurSettings rs{};
        rs.hitDistanceParameters.A = p.diffHitDistScaleA;
        rs.hitDistanceParameters.B = 0.0f;
        rs.hitDistanceParameters.C = 1.0f;

        // Diffuse GI is low-frequency and its near-field signal changes slowly, so it wants more
        // accumulation than the specular path; in SECONDS via framesFor() because NRD's history lengths
        // are in FRAMES.
        rs.maxAccumulatedFrameNum     = framesFor(0.400f, 8u, 40u);
        rs.maxFastAccumulatedFrameNum = framesFor(0.080f, 4u, 12u);
        // MUST stay strictly below maxFastAccumulatedFrameNum (NRD's range is the exclusive
        // [0; maxFast)); maxFast is floored at 4 above, so 3 is always legal.
        rs.historyFixFrameNum         = 3u;
        rs.maxStabilizedFrameNum      = framesFor(0.250f, 6u, 30u);
        // Responsive accumulation is a SPECULAR concept (it shortens history on near-mirror roughness);
        // the library default (threshold 0 = off) is right for a diffuse lobe. Do NOT copy the specular
        // branch's roughnessThreshold here: it would shorten history wherever GBuffer roughness happens to
        // be low.
        // ---- Spatial kernel width ----
        // Everything else stays at the library default (diffusePrepassBlurRadius 30, minBlurRadius 1,
        // maxBlurRadius 30) except lobeAngleFraction. Its 0.15 default suits a SPECULAR lobe, where a
        // neighbour with a slightly different normal reflects something else. A DIFFUSE lobe integrates the
        // hemisphere, so rejecting nearby normals only starves the spatial filter (at 1-2 rays per pixel,
        // the difference between denoised and noisy). 0.5 matches NRD's own RELAX diffuse default
        // (NRDSettings.h).
        rs.lobeAngleFraction = 0.5f;
        // Diffuse GI DOES have fireflies (a single ray landing on an emissive
        // or a sun-lit sliver), unlike the AO signal which has none.
        rs.enableAntiFirefly = true;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (REBLUR_DIFFUSE) failed");
            return false;
        }
    }
    else if (m_kind == Kind::ReblurDiffuseOcclusion)
    {
        // REBLUR AO. hitDistanceParameters MUST match the pack shader's kAOHitDistParams (NRD denormalizes
        // with them). The REBLUR_SPECULAR defaults {3,0.1,20} make a MISS (hitDist == aoRadius) normalize
        // to ~1/(3 + viewZ x 0.1) instead of 1, crushing AO and darkening with distance;
        // {A=aoDistanceScale, B=0, C=1} maps a miss to exactly 1 = unoccluded.
        // returnHistoryLengthInsteadOfOcclusion MUST stay false so OUT_DIFF_HITDIST carries occlusion, not
        // a counter.
        nrd::ReblurSettings rs{};
        rs.hitDistanceParameters.A = p.aoDistanceScale;
        rs.hitDistanceParameters.B = 0.0f;
        rs.hitDistanceParameters.C = 1.0f;
        rs.maxAccumulatedFrameNum     = framesFor(0.300f, 8u, 40u);
        rs.maxFastAccumulatedFrameNum = framesFor(0.060f, 4u, 12u);
        rs.historyFixFrameNum         = 3u;
        rs.maxStabilizedFrameNum      = framesFor(0.200f, 6u, 30u);
        // "1.1 is a safe value for occlusion denoising" (NRDSettings.h).
        rs.fastHistoryClampingSigmaScale = 1.1f;
        rs.enableAntiFirefly = false;            // AO has no fireflies
        rs.returnHistoryLengthInsteadOfOcclusion = false;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (REBLUR_OCCLUSION) failed");
            return false;
        }
    }
    else if (m_kind == Kind::ReblurSpecularPathTraced)
    {
        // A path-traced reflection (PathTracePass FrontReflection on every frame that does not
        // accumulate). hitDistanceParameters.A MUST equal the pack shader's (PathTraceFrontNRD.cs.hlsl
        // Pack). A head-on pane is a MIRROR (M_Glass roughness (1-N.V)^5) where REBLUR's blur radius is
        // ~1.6 px, so it can only average in time; with the library's 30-frame history and 6-frame
        // fast-history clamp a one-path-per-texel signal settles into a static mottled grain once the camera
        // stops. Hence the longest history REBLUR has and a looser fast-history clamp: a pane holds still far
        // more than a reflection on it moves.
        nrd::ReblurSettings rs{};
        rs.hitDistanceParameters.A = p.specHitDistScaleA;
        rs.enableAntiFirefly = true;
        rs.maxAccumulatedFrameNum = nrd::REBLUR_MAX_HISTORY_FRAME_NUM;
        rs.maxFastAccumulatedFrameNum = 12;
        rs.fastHistoryClampingSigmaScale = 2.5f;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings (REBLUR_SPECULAR path-traced) failed");
            return false;
        }
    }
    else
    {
        // Anti-ghost tuning. The library defaults target noisy path-traced signals under CAMERA motion;
        // REBLUR only sees SURFACE motion vectors + hitDist virtual motion, so a moving object inside a
        // reflection on a static mirror reads as flicker to suppress and long histories smear it into a
        // trail. This signal is comparatively clean (deterministic mirror rays, screen-reuse radiance
        // already lit / anti-aliased) and needs far less accumulation. The time targets reproduce the
        // 16/3/10-frame look tuned at 180 fps (~90/17/56 ms) at any rate.
        nrd::ReblurSettings rs{};
        // hitDistanceParameters.A MUST match the number HybridReflNRDPack normalized with
        // (HybridReflConstants::hitDistParamA); this side denormalizes. Both come from one expression in
        // RTReflectionPass::Execute. B and C stay at the library defaults; only A scales with the ray
        // budget, which keeps a miss (hitT == rayTMax) from saturating normHitDist to 1.0 and feeding a
        // clamped distance into the blur radius + virtual-motion reprojection.
        rs.hitDistanceParameters.A    = p.specHitDistScaleA;
        // Frame-rate starvation guard. The targets are in SECONDS so the trail stays constant in
        // wall-clock time, but noise convergence needs SAMPLE COUNT and the seconds were tuned at ~180 fps.
        // The floors keep low fps from starving accumulation (60 fps: 12/3/2/6 frames); at high fps the
        // seconds target still governs (180 fps: 16/3/2/10).
        // Trade-off: at low fps the wall-clock trail lengthens (12 frames = 200 ms), so ghosting risk rises
        // there. If a trail appears on a moving object reflected in a static rough surface, back off
        // maxStabilizedFrameNum first (6u -> 4u), then maxAccumulatedFrameNum, one knob at a time.
        rs.maxAccumulatedFrameNum     = framesFor(0.090f, 12u, 30u);
        rs.maxFastAccumulatedFrameNum = framesFor(0.017f,  3u,  8u);
        // Spatial fill for pixels whose history is too short to trust: strictly below the fast history it
        // seeds from, at least 1, capped at the library default of 3.
        rs.historyFixFrameNum         =
            (std::min)(3u, (std::max)(1u, rs.maxFastAccumulatedFrameNum - 1u));
        rs.maxStabilizedFrameNum = framesFor(0.056f, 6u, 12u);
        rs.responsiveAccumulationSettings.roughnessThreshold = 0.2f;
        rs.fastHistoryClampingSigmaScale     = 1.5f;
        rs.antilagSettings.luminanceSigmaScale = 1.5f;
        rs.enableAntiFirefly = true;
        if (nrd::SetDenoiserSettings(*m_instance, kDenoiserId, &rs) != nrd::Result::SUCCESS)
        {
            LOG_ERROR("NrdDenoiser: SetDenoiserSettings failed");
            return false;
        }
    }

    const nrd::DispatchDesc* dispatches = nullptr;
    uint32_t dispatchCount = 0;
    const nrd::Identifier ids[] = { kDenoiserId, kSpecularDenoiserId };
    const uint32_t idCount = (m_kind == Kind::RelaxDiffuseSpecularSplit) ? 2u : 1u;
    if (nrd::GetComputeDispatches(*m_instance, ids, idCount, dispatches, dispatchCount)
            != nrd::Result::SUCCESS || !dispatches)
    {
        LOG_ERROR("NrdDenoiser: GetComputeDispatches failed");
        return false;
    }

    // ---- Per-frame ring section ----
    m_frameSlot = (m_frameSlot + 1u) % GraphicsDX12::kFrameCount;
    const uint32_t descBase = m_frameSlot * m_descPerFrame;
    uint32_t descCursor = 0;
    uint32_t cbCursor   = 0;

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_descHeap->GetGPUDescriptorHandleForHeapStart();

    // User-texture shadow states (restored on exit), one entry per bound (ResourceType -> texture)
    // binding, matched by ResourceType. SIGMA's OUT_SHADOW_TRANSLUCENCY is bound once and serves both
    // its history read (SRV) and its write (UAV); the per-dispatch barrier loop flips its state.
    struct Tracked { nrd::ResourceType type; ID3D12Resource* res; DXGI_FORMAT fmt;
                     D3D12_RESOURCE_STATES cur; D3D12_RESOURCE_STATES entry; };
    std::vector<Tracked> user;
    user.reserve(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i)
    {
        // The resource and the format are the texture's own, the state the device's mapping of the RHI
        // one.
        const UserTexture& t = bindings[i].tex;
        const D3D12_RESOURCE_STATES st = GraphicsDX12::ToD3D12ResourceState(t.state);
        user.push_back({ bindings[i].type, t.texture ? gfx.GetTextureResource(*t.texture) : nullptr,
                         t.texture ? GraphicsDX12::ToDxgiFormat(t.texture->desc.format) : DXGI_FORMAT_UNKNOWN,
                         st, st });
    }
    auto userFor = [&](nrd::ResourceType t) -> Tracked* {
        for (Tracked& u : user) if (u.type == t) return &u;
        return nullptr;
    };

    ID3D12DescriptorHeap* heaps[] = { m_descHeap.Get() };
    native->SetDescriptorHeaps(1, heaps);

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(16);

    // NRD may flag constantBufferDataMatchesPreviousDispatch and hand back no data: reuse the previous
    // upload slice.
    D3D12_GPU_VIRTUAL_ADDRESS lastCbVA = 0;

    // Failure paths BREAK (never return) so the user-texture restore below always runs: the caller's
    // state tracking depends on it.
    bool ok = true;

    for (uint32_t d = 0; d < dispatchCount && ok; ++d)
    {
        const nrd::DispatchDesc& dd = dispatches[d];
        if (dd.pipelineIndex >= m_psos.size())
        {
            LOG_ERROR("NrdDenoiser: dispatch '%s' pipeline index %u out of range",
                      dd.name ? dd.name : "?", dd.pipelineIndex);
            ok = false;
            break;
        }
        const nrd::PipelineDesc& pd =
            nrd::GetInstanceDesc(*m_instance)->pipelines[dd.pipelineIndex];

        // ---- Transitions for every resource this dispatch touches ----
        barriers.clear();
        for (uint32_t r = 0; r < dd.resourcesNum; ++r)
        {
            const nrd::ResourceDesc& res = dd.resources[r];
            const D3D12_RESOURCE_STATES want =
                (res.descriptorType == nrd::DescriptorType::TEXTURE)
                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            ID3D12Resource* raw = nullptr;
            D3D12_RESOURCE_STATES* cur = nullptr;
            if (res.type == nrd::ResourceType::TRANSIENT_POOL)
            {
                if (res.indexInPool < m_transient.size())
                {
                    raw = m_transient[res.indexInPool].resource.Get();
                    cur = &m_transient[res.indexInPool].state;
                }
            }
            else if (res.type == nrd::ResourceType::PERMANENT_POOL)
            {
                if (res.indexInPool < m_permanent.size())
                {
                    raw = m_permanent[res.indexInPool].resource.Get();
                    cur = &m_permanent[res.indexInPool].state;
                }
            }
            else if (Tracked* t = userFor(res.type))
            {
                raw = t->res;
                cur = &t->cur;
            }
            if (!raw || !cur)
            {
                LOG_ERROR("NrdDenoiser: dispatch '%s' wants unbound/out-of-range resource type %u idx %u",
                          dd.name ? dd.name : "?", (uint32_t)res.type, res.indexInPool);
                ok = false;
                break;
            }

            D3D12_RESOURCE_BARRIER b{};
            if (*cur != want)
            {
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = raw;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                b.Transition.StateBefore = *cur;
                b.Transition.StateAfter  = want;
                barriers.push_back(b);
                *cur = want;
            }
            else if (want == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
            {
                // UAV -> UAV across dispatches: hazard barrier.
                b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                b.UAV.pResource = raw;
                barriers.push_back(b);
            }
        }
        if (!ok) break;
        if (!barriers.empty())
        {
            native->ResourceBarrier((UINT)barriers.size(), barriers.data());
            GfxDiag::NoteNativeBarriers(native, barriers.data(), (uint32_t)barriers.size());
        }

        // ---- Descriptors ----
        // SRV range first, then UAV range: NRD's resource list is concatenated in resourceRanges order.
        const uint32_t tableStart[2] = { descCursor,
            descCursor + (pd.resourceRangesNum > 0 ? pd.resourceRanges[0].descriptorsNum : 0) };
        uint32_t written = 0;
        for (uint32_t r = 0; r < dd.resourcesNum; ++r)
        {
            const nrd::ResourceDesc& res = dd.resources[r];
            ID3D12Resource* raw = nullptr;
            DXGI_FORMAT     fmt = DXGI_FORMAT_UNKNOWN;
            if (res.type == nrd::ResourceType::TRANSIENT_POOL)
            { raw = m_transient[res.indexInPool].resource.Get(); fmt = m_transient[res.indexInPool].format; }
            else if (res.type == nrd::ResourceType::PERMANENT_POOL)
            { raw = m_permanent[res.indexInPool].resource.Get(); fmt = m_permanent[res.indexInPool].format; }
            else if (Tracked* t = userFor(res.type))
            { raw = t->res; fmt = t->fmt; }

            if (descCursor >= m_descPerFrame)
            {
                LOG_ERROR("NrdDenoiser: descriptor ring overflow (%u per frame)",
                          m_descPerFrame);
                ok = false;
                break;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE h = cpuBase;
            h.ptr += size_t(descBase + descCursor) * m_descSize;

            if (res.descriptorType == nrd::DescriptorType::TEXTURE)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format                  = fmt;
                sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels     = 1;
                dev->CreateShaderResourceView(raw, &sd, h);
            }
            else
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format        = fmt;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                dev->CreateUnorderedAccessView(raw, nullptr, &ud, h);
            }
            ++descCursor;
            ++written;
        }
        (void)written;
        if (!ok) break;

        // ---- Constants ----
        D3D12_GPU_VIRTUAL_ADDRESS cbVA = lastCbVA;
        if (dd.constantBufferData && dd.constantBufferDataSize &&
            !dd.constantBufferDataMatchesPreviousDispatch)
        {
            if (cbCursor >= m_cbSlicesPerFrame)
            {
                LOG_ERROR("NrdDenoiser: CB ring overflow (%u slices per frame)",
                          m_cbSlicesPerFrame);
                ok = false;
                break;
            }
            const uint64_t offset =
                (uint64_t(m_frameSlot) * m_cbSlicesPerFrame + cbCursor) * m_cbSliceSize;
            std::memcpy(m_cbMapped + offset, dd.constantBufferData, dd.constantBufferDataSize);
            cbVA = m_cbUpload->GetGPUVirtualAddress() + offset;
            ++cbCursor;
        }
        lastCbVA = cbVA;

        // ---- Record ----
        native->SetComputeRootSignature(m_rootSigs[dd.pipelineIndex].Get());
        native->SetPipelineState(m_psos[dd.pipelineIndex].Get());
        if (cbVA)
            native->SetComputeRootConstantBufferView(0, cbVA);

        uint32_t param = 1;
        for (uint32_t r = 0; r < pd.resourceRangesNum && r < 2; ++r)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE g = gpuBase;
            g.ptr += uint64_t(descBase + tableStart[r]) * m_descSize;
            native->SetComputeRootDescriptorTable(param++, g);
        }

        native->Dispatch(dd.gridWidth, dd.gridHeight, 1);
    }

    // ---- Restore user textures to their entry states ----
    // Runs on EVERY path, success or mid-recording failure, so the caller's RHI-level state tracking
    // stays truthful.
    barriers.clear();
    for (Tracked& t : user)
    {
        if (t.cur != t.entry)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = t.res;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = t.cur;
            b.Transition.StateAfter  = t.entry;
            barriers.push_back(b);
        }
    }
    if (!barriers.empty())
    {
        native->ResourceBarrier((UINT)barriers.size(), barriers.data());
        GfxDiag::NoteNativeBarriers(native, barriers.data(), (uint32_t)barriers.size());
    }

    // ---- Cache camera state for next frame ----
    // On failure m_havePrev goes false: the forced CLEAR_AND_RESTART next frame flushes whatever
    // half-recorded history remains.
    if (ok)
    {
        std::memcpy(m_prevViewToClip,  p.viewToClip,  sizeof(m_prevViewToClip));
        std::memcpy(m_prevWorldToView, p.worldToView, sizeof(m_prevWorldToView));
        m_prevJitter[0] = p.jitter[0];
        m_prevJitter[1] = p.jitter[1];
        m_havePrev = true;
    }
    else
    {
        m_havePrev = false;
    }
    return ok;
}
