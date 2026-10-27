#include "Graphics/GraphicsDX12.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "System/Log.h"

#include "d3dx12_core.h"
#include "d3dx12_state_object.h"

#include <cstring>
#include <deque>
#include <string>

// The DXR ray-tracing pipeline behind IGraphicsDevice: one DXIL library with a
// raygen shader, any number of miss shaders and triangle hit groups. Every ray the
// engine traces is an inline RayQuery in a compute pass; the one caller of this
// path is the -dxr_parity self-test (Renderer/DXRParityTest.cpp), which checks it
// against RayQuery on the scene TLAS.

using Microsoft::WRL::ComPtr;

namespace
{

constexpr uint32_t kShaderIdentSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;          // 32
constexpr uint32_t kTableAlign      = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;   // 64

constexpr uint64_t AlignTo(uint64_t value, uint64_t align)
{ return (value + align - 1) & ~(align - 1); }

std::wstring Widen(const char* s)
{
    if (!s || !*s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

} // anonymous namespace

bool GraphicsDX12::CreateRaytracingPipeline(const RHI::RaytracingPipelineDesc& desc, RHI::RaytracingPipeline& out)
{
    out = {};
    const char* name = desc.name ? desc.name : "(unnamed)";
    if (!m_dxrSupported) return false;
    if (!desc.library || !desc.rayGen || !desc.layout || desc.missCount == 0 || desc.hitGroupCount == 0)
    {
        LOG_ERROR("CreateRaytracingPipeline %s: needs a library, a ray-generation shader, a layout, a miss "
                  "shader and a hit group", name);
        return false;
    }
    D3D12_SHADER_BYTECODE library{};
    if (!LookupShaderBytecode(desc.library, library, nullptr, "LIB")) return false;
    ComPtr<ID3D12RootSignature> globalRootSig;
    if (!LookupPipelineLayout(*desc.layout, globalRootSig)) return false;
    ID3D12Device5* dev5 = GetDevice5();
    if (!dev5) return false;

    // The subobjects keep pointers to these names until CreateStateObject: a
    // deque never moves what it holds.
    std::deque<std::wstring> names;
    auto keep = [&names](const char* s) -> const wchar_t* { names.push_back(Widen(s)); return names.back().c_str(); };

    CD3DX12_STATE_OBJECT_DESC pipelineDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };

    // ---- DXIL library: the exports the pipeline names ----
    auto* lib = pipelineDesc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    lib->SetDXILLibrary(&library);
    const wchar_t* rayGen = keep(desc.rayGen);
    lib->DefineExport(rayGen);
    std::vector<const wchar_t*> miss(desc.missCount);
    for (uint32_t i = 0; i < desc.missCount; ++i)
        lib->DefineExport(miss[i] = keep(desc.miss[i]));

    // ---- Triangle hit groups ----
    std::vector<const wchar_t*> hitGroups(desc.hitGroupCount);
    for (uint32_t i = 0; i < desc.hitGroupCount; ++i)
    {
        const RHI::HitGroup& g = desc.hitGroups[i];
        auto* hg = pipelineDesc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
        if (g.closestHit)
        {
            const wchar_t* ch = keep(g.closestHit);
            lib->DefineExport(ch);
            hg->SetClosestHitShaderImport(ch);
        }
        if (g.anyHit)
        {
            const wchar_t* ah = keep(g.anyHit);
            lib->DefineExport(ah);
            hg->SetAnyHitShaderImport(ah);
        }
        hg->SetHitGroupExport(hitGroups[i] = keep(g.name));
        hg->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
    }

    // ---- Shader / pipeline config, the global root signature ----
    auto* sc = pipelineDesc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    sc->Config(desc.maxPayloadBytes, desc.maxAttributeBytes);
    auto* pc = pipelineDesc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    pc->Config(desc.maxRecursion);
    auto* rs = pipelineDesc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    rs->SetRootSignature(globalRootSig.Get());

    RaytracingPipelineDX12 entry;
    HRESULT hr = dev5->CreateStateObject(pipelineDesc, IID_PPV_ARGS(&entry.stateObject));
    if (FAILED(hr))
    {
        LOG_ERROR("CreateRaytracingPipeline %s: CreateStateObject failed (hr=0x%08X)", name, (unsigned)hr);
        return false;
    }
    ComPtr<ID3D12StateObjectProperties> properties;
    if (FAILED(entry.stateObject.As(&properties)))
    {
        LOG_ERROR("CreateRaytracingPipeline %s: ID3D12StateObjectProperties QI failed", name);
        return false;
    }

    // ---- Shader table ----
    // The ray-generation record, the miss records, the hit-group records. Each record
    // is one identifier padded to the 64-byte table alignment, so every sub-table
    // starts aligned.
    const uint32_t stride  = static_cast<uint32_t>(AlignTo(kShaderIdentSize, kTableAlign));
    const uint32_t records = 1 + desc.missCount + desc.hitGroupCount;
    const uint64_t bytes   = uint64_t(stride) * records;
    {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = bytes;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags            = D3D12_RESOURCE_FLAG_NONE;
        hr = m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&entry.shaderTable));
        if (FAILED(hr))
        {
            LOG_ERROR("CreateRaytracingPipeline %s: shader table allocation failed (hr=0x%08X)", name, (unsigned)hr);
            return false;
        }
        GpuLedger::TrackCommitted(m_device.Get(), entry.shaderTable.Get(), Diag::Prof::GpuMemCat::Upload,
                                  "RT.ShaderTable");
    }
    void* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    if (FAILED(entry.shaderTable->Map(0, &noRead, &mapped)) || !mapped)
    {
        LOG_ERROR("CreateRaytracingPipeline %s: shader table map failed", name);
        return false;
    }
    bool identifiers = true;
    auto writeRecord = [&](uint32_t record, const wchar_t* exportName)
    {
        const void* id = properties->GetShaderIdentifier(exportName);
        if (!id) { identifiers = false; return; }
        std::memcpy(static_cast<uint8_t*>(mapped) + uint64_t(record) * stride, id, kShaderIdentSize);
    };
    writeRecord(0, rayGen);
    for (uint32_t i = 0; i < desc.missCount; ++i)     writeRecord(1 + i, miss[i]);
    for (uint32_t i = 0; i < desc.hitGroupCount; ++i) writeRecord(1 + desc.missCount + i, hitGroups[i]);
    entry.shaderTable->Unmap(0, nullptr);
    if (!identifiers)
    {
        LOG_ERROR("CreateRaytracingPipeline %s: a shader identifier is missing (an export the library lacks)", name);
        return false;
    }

    // The table lives in the UPLOAD heap: small and read once per dispatch; a DEFAULT
    // copy would only add barriers.
    const uint64_t base = entry.shaderTable->GetGPUVirtualAddress();
    entry.globalRootSig = globalRootSig;
    entry.recordStride  = stride;
    entry.rayGenVA      = base;
    entry.missVA        = base + uint64_t(stride);
    entry.missCount     = desc.missCount;
    entry.hitGroupVA    = base + uint64_t(stride) * (1 + desc.missCount);
    entry.hitGroupCount = desc.hitGroupCount;

    {
        std::lock_guard<std::mutex> lock(m_rtPipelineMutex);
        uint32_t id;
        if (!m_rtPipelineFreeIds.empty())
        {
            id = m_rtPipelineFreeIds.back();
            m_rtPipelineFreeIds.pop_back();
            m_rtPipelines[id] = std::move(entry);
        }
        else
        {
            id = static_cast<uint32_t>(m_rtPipelines.size());
            m_rtPipelines.push_back(std::move(entry));
        }
        out.handleId = id;
        out.type      = RHI::GPUResource::Type::RaytracingPipeline;
    }
    LOG_INFO("[RT_PIPELINE] %s miss=%u hitGroups=%u records=%u stride=%u bytes=%llu", name, desc.missCount,
             desc.hitGroupCount, records, stride, (unsigned long long)bytes);
    return true;
}

void GraphicsDX12::DestroyRaytracingPipeline(RHI::RaytracingPipeline& pipeline)
{
    if (!pipeline.IsValid()) return;
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12Resource>    shaderTable;
    {
        std::lock_guard<std::mutex> lock(m_rtPipelineMutex);
        if (pipeline.handleId < m_rtPipelines.size())
        {
            RaytracingPipelineDX12& e = m_rtPipelines[pipeline.handleId];
            stateObject = std::move(e.stateObject);
            shaderTable = std::move(e.shaderTable);
            e = RaytracingPipelineDX12{};
            m_rtPipelineFreeIds.push_back(pipeline.handleId);
        }
    }
    pipeline = {};
    // Both outlive every list recorded with them: the backbuffer slot's bucket.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& bucket = PendingReleaseBucket();
    if (stateObject) bucket.pipelineObjects.push_back(std::move(stateObject));
    if (shaderTable) bucket.resources.push_back(std::move(shaderTable));
}

void GraphicsDX12::BindRaytracingPipeline(const RHI::RaytracingPipeline& pipeline, RHI::CommandList cmd)
{
    ID3D12StateObject*   stateObject = nullptr;
    ID3D12RootSignature* rootSig     = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_rtPipelineMutex);
        if (!pipeline.IsValid() || pipeline.handleId >= m_rtPipelines.size()) return;
        const RaytracingPipelineDX12& e = m_rtPipelines[pipeline.handleId];
        stateObject = e.stateObject.Get();
        rootSig     = e.globalRootSig.Get();
    }
    if (!stateObject || !rootSig) return;
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    cl->SetPipelineState1(stateObject);
    cl->SetComputeRootSignature(rootSig);
    if (entry.IsValid()) ++entry.counters.psoBinds;   // per-list profiler counters
    entry.activeRootSigCompute = rootSig;
    entry.activePso             = nullptr;
}

void GraphicsDX12::DispatchRays(const RHI::RaytracingPipeline& pipeline, uint32_t width, uint32_t height,
                                uint32_t depth, RHI::CommandList cmd)
{
    if (width == 0 || height == 0 || depth == 0) return;
    D3D12_DISPATCH_RAYS_DESC desc{};
    {
        std::lock_guard<std::mutex> lock(m_rtPipelineMutex);
        if (!pipeline.IsValid() || pipeline.handleId >= m_rtPipelines.size()) return;
        const RaytracingPipelineDX12& e = m_rtPipelines[pipeline.handleId];
        if (!e.stateObject) return;
        desc.RayGenerationShaderRecord.StartAddress = e.rayGenVA;
        desc.RayGenerationShaderRecord.SizeInBytes  = e.recordStride;
        desc.MissShaderTable.StartAddress  = e.missVA;
        desc.MissShaderTable.SizeInBytes   = uint64_t(e.recordStride) * e.missCount;
        desc.MissShaderTable.StrideInBytes = e.recordStride;
        desc.HitGroupTable.StartAddress    = e.hitGroupVA;
        desc.HitGroupTable.SizeInBytes     = uint64_t(e.recordStride) * e.hitGroupCount;
        desc.HitGroupTable.StrideInBytes   = e.recordStride;
    }
    desc.Width  = width;
    desc.Height = height;
    desc.Depth  = depth;
    CommandListDX12& entry = GetPoolEntry(cmd);
    entry.GetCommandList()->DispatchRays(&desc);
    if (entry.IsValid()) ++entry.counters.dispatchesRays;   // per-list profiler counters
}
