#include "Graphics/GraphicsDX12.h"
#include "Graphics/MeshPsoStream.h"   // the stream every mesh PSO is built from
#include "Graphics/PsoManifest.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

// The pipeline manifest (Graphics/PsoManifest.h): recording what this device builds (the harness's
// -pso_variants) and replaying a recorded manifest into the PSO library before the title screen.

// ==== Recording ====

namespace
{
    using BlobList = std::vector<std::pair<uint64_t, std::vector<uint8_t>>>;

    uint64_t AddBytecode(BlobList& blobs, const D3D12_SHADER_BYTECODE& bc)
    {
        if (!bc.pShaderBytecode || bc.BytecodeLength == 0) return 0;
        const auto* b = static_cast<const uint8_t*>(bc.pShaderBytecode);
        const uint64_t h = Gfx::PsoManifest::HashBlob(b, bc.BytecodeLength);
        blobs.emplace_back(h, std::vector<uint8_t>(b, b + bc.BytecodeLength));
        return h;
    }

    // The PSO library names are FNV-1a over the UTF-16 name (GraphicsDX12_Resources.cpp HashPsoName).
    uint64_t HashName(const std::wstring& name)
    {
        uint64_t h = 14695981039346656037ull;
        for (wchar_t c : name)
        {
            h ^= static_cast<uint64_t>(c);
            h *= 1099511628211ull;
        }
        return h;
    }
}

void GraphicsDX12::NoteRootSignature(ID3D12RootSignature* rs, const void* blob, size_t size)
{
    if (!rs || !blob || size == 0) return;
    const auto* b = static_cast<const uint8_t*>(blob);
    std::lock_guard<std::mutex> lock(m_rootSigBlobMutex);
    RootSigBlob& slot = m_rootSigBlobs[rs];
    slot.hash = Gfx::PsoManifest::HashBlob(blob, size);
    slot.bytes.assign(b, b + size);
}

void GraphicsDX12::RecordPipeline(Gfx::PsoManifest::Kind kind, const wchar_t* name, ID3D12RootSignature* rs,
                                  const D3D12_SHADER_BYTECODE (&stages)[Gfx::PsoManifest::kStageCount],
                                  const Gfx::PsoManifest::State* state, uint32_t buildUs)
{
    if (!Gfx::PsoManifest::RecordingEnabled() || !name || !*name) return;
    if (Gfx::PsoManifest::IsRecorded(name)) return;

    Gfx::PsoManifest::Record rec;
    rec.kind        = kind;
    rec.name        = name;
    rec.estimatedUs = buildUs;
    BlobList blobs;
    {
        std::lock_guard<std::mutex> lock(m_rootSigBlobMutex);
        const auto it = m_rootSigBlobs.find(rs);
        if (it == m_rootSigBlobs.end())
        {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                LOG_WARNING("PSO manifest: a pipeline on a root signature the device did not register is not "
                            "recorded (%ls)", name);
            return;
        }
        rec.rootSignature = it->second.hash;
        blobs.emplace_back(it->second.hash, it->second.bytes);
    }
    for (uint32_t s = 0; s < Gfx::PsoManifest::kStageCount; ++s)
        rec.stages[s] = AddBytecode(blobs, stages[s]);
    if (state) rec.state = *state;
    Gfx::PsoManifest::AddRecord(rec, blobs);
}

// ==== Replay ====

// One StartPipelinePrecompile: the manifest, its root signatures built once, the workers and their
// tallies. Shared with the workers, which outlive nothing: FinishPipelinePrecompile joins them.
struct GraphicsDX12::PipelinePrecompile
{
    Gfx::PsoManifest::Manifest                                     manifest;
    std::unordered_map<uint64_t, ComPtr<ID3D12RootSignature>>      rootSigs;
    std::vector<uint32_t>                                          order;      // longest first
    std::vector<std::thread>                                       workers;
    std::atomic<uint32_t>                                          next{ 0 };
    std::atomic<uint32_t>                                          built{ 0 };
    std::atomic<uint32_t>                                          present{ 0 };
    std::atomic<uint32_t>                                          failed{ 0 };
    std::atomic<uint32_t>                                          running{ 0 };   // workers not done yet
    std::atomic<bool>                                              cancel{ false };
    std::chrono::steady_clock::time_point                          start;
    double                                                         elapsedMs = 0.0;   // set at the end
    uint64_t                                                       manifestHash = 0;
    std::string                                                    indexPath;
};

namespace
{
    // The index StartPipelinePrecompile reads to skip a library already prepared: the manifest, the
    // adapter and driver it was prepared on, and the library's serialized size then (a library that
    // loads smaller lost what was stored; another adapter or driver loads an empty one).
    std::string IndexLine(uint64_t manifestHash, const DXGI_ADAPTER_DESC1& adapter, uint64_t driver, uint64_t bytes)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "manifest=%016llx adapter=%04x:%04x:%08x:%02x driver=%llx libraryBytes=%llu",
                      static_cast<unsigned long long>(manifestHash), adapter.VendorId, adapter.DeviceId,
                      adapter.SubSysId, adapter.Revision, static_cast<unsigned long long>(driver),
                      static_cast<unsigned long long>(bytes));
        return buf;
    }

    bool ParseIndexLine(const std::string& line, uint64_t& manifestHash, std::string& identity, uint64_t& bytes)
    {
        const size_t at = line.find(" libraryBytes=");
        if (line.rfind("manifest=", 0) != 0 || at == std::string::npos) return false;
        manifestHash = std::strtoull(line.c_str() + 9, nullptr, 16);
        identity     = line.substr(26, at - 26);   // "adapter=... driver=..."
        bytes        = std::strtoull(line.c_str() + at + 14, nullptr, 10);
        return true;
    }
}

bool GraphicsDX12::StartPipelinePrecompile(const std::vector<uint8_t>& manifest, const std::string& indexPath,
                                           uint32_t threads)
{
    m_precompileStatus = {};
    if (m_precompile) { m_precompileStatus.skipped = "a precompile is running"; return false; }
    if (!m_psoLibrary) { m_precompileStatus.skipped = "no PSO library"; return false; }

    auto job = std::make_shared<PipelinePrecompile>();
    std::string error;
    if (!Gfx::PsoManifest::Parse(manifest.data(), manifest.size(), job->manifest, error))
    {
        m_precompileStatus.skipped = "the manifest is malformed: " + error;
        LOG_ERROR("Pipeline precompile: %s", m_precompileStatus.skipped.c_str());
        return false;
    }
    job->manifestHash = Gfx::PsoManifest::ContentHash(job->manifest);
    job->indexPath    = indexPath;
    m_precompileStatus.total = static_cast<uint32_t>(job->manifest.records.size());

    // Prepared before, on this adapter and driver, into a library that still holds it: nothing to do.
    {
        std::string line;
        std::ifstream f = Sys::OpenIfstream(indexPath, std::ios::in);
        uint64_t hash = 0, bytes = 0;
        std::string identity;
        if (f && std::getline(f, line) && ParseIndexLine(line, hash, identity, bytes) &&
            hash == job->manifestHash && m_psoLibBytes >= bytes && bytes > 0 &&
            IndexLine(hash, m_adapterDesc, m_driverVersion, bytes).find(identity) != std::string::npos)
        {
            m_precompileStatus.skipped = "already prepared";
            m_precompileStatus.done    = true;
            m_precompileStatus.present = m_precompileStatus.total;
            m_precompileStatus.finished = m_precompileStatus.total;
            return false;
        }
    }

    // Root signatures, once each, here: a worker only reads the map.
    for (const Gfx::PsoManifest::Record& rec : job->manifest.records)
    {
        if (job->rootSigs.count(rec.rootSignature)) continue;
        ComPtr<ID3D12RootSignature>& rs = job->rootSigs[rec.rootSignature];
        const auto it = job->manifest.blobs.find(rec.rootSignature);
        if (it == job->manifest.blobs.end() ||
            FAILED(m_device->CreateRootSignature(0, it->second.data(), it->second.size(), IID_PPV_ARGS(&rs))))
        {
            rs = nullptr;
            LOG_WARNING("Pipeline precompile: root signature %016llx is missing or invalid; its pipelines are "
                        "skipped", static_cast<unsigned long long>(rec.rootSignature));
        }
    }

    // Longest first: the dearest pipeline must not start last and run alone at the end.
    job->order.resize(job->manifest.records.size());
    for (uint32_t i = 0; i < job->order.size(); ++i) job->order[i] = i;
    std::stable_sort(job->order.begin(), job->order.end(), [&](uint32_t a, uint32_t b)
    {
        return job->manifest.records[a].estimatedUs > job->manifest.records[b].estimatedUs;
    });

    threads = (std::max)(1u, (std::min)(threads, static_cast<uint32_t>(job->order.size())));
    job->start = std::chrono::steady_clock::now();
    job->running.store(threads);
    m_precompile = job;
    m_precompileStatus.started = true;
    for (uint32_t t = 0; t < threads; ++t)
    {
        job->workers.emplace_back([this, job, t]
        {
            const std::wstring threadName = L"PSO precompile " + std::to_wstring(t);
            ::SetThreadDescription(::GetCurrentThread(), threadName.c_str());
            for (;;)
            {
                if (job->cancel.load(std::memory_order_relaxed)) break;
                const uint32_t i = job->next.fetch_add(1);
                if (i >= job->order.size()) break;
                PrecompileOne(*job, job->manifest.records[job->order[i]]);
            }
            job->running.fetch_sub(1);
        });
    }
    LOG_INFO("Pipeline precompile: %u pipelines on %u threads (%zu root signatures, %zu blobs)",
             m_precompileStatus.total, threads, job->rootSigs.size(), job->manifest.blobs.size());
    return true;
}

void GraphicsDX12::PrecompileOne(PipelinePrecompile& job, const Gfx::PsoManifest::Record& rec)
{
    using namespace Gfx::PsoManifest;
    auto fail = [&](const char* why)
    {
        if (job.failed.fetch_add(1) < 8)
            LOG_WARNING("Pipeline precompile: %ls not built (%s)", rec.name.c_str(), why);
    };
    const auto rsIt = job.rootSigs.find(rec.rootSignature);
    if (rsIt == job.rootSigs.end() || !rsIt->second) { fail("its root signature"); return; }
    ID3D12RootSignature* rs = rsIt->second.Get();

    bool missing = false;
    auto bc = [&](StageSlot s) -> D3D12_SHADER_BYTECODE
    {
        if (!rec.stages[s]) return { nullptr, 0 };
        const auto it = job.manifest.blobs.find(rec.stages[s]);
        if (it == job.manifest.blobs.end()) { missing = true; return { nullptr, 0 }; }
        return { it->second.data(), it->second.size() };
    };

    const uint64_t nameHash = HashName(rec.name);
    AcquirePsoLibName(nameHash);
    struct Release { GraphicsDX12* d; uint64_t n; ~Release() { d->ReleasePsoLibName(n); } } release{ this, nameHash };

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = E_FAIL;
    switch (rec.kind)
    {
    case Kind::Graphics:
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
        std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
        ApplyState(rec.state, d, elements);
        d.pRootSignature = rs;
        d.VS = bc(kVS); d.PS = bc(kPS); d.GS = bc(kGS); d.HS = bc(kHS); d.DS = bc(kDS);
        if (missing) { fail("a shader blob is missing"); return; }
        if (SUCCEEDED(m_psoLibrary->LoadGraphicsPipeline(rec.name.c_str(), &d, IID_PPV_ARGS(&pso))))
        {
            ++job.present;
            return;
        }
        hr = m_device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
        break;
    }
    case Kind::Mesh:
    {
        ComPtr<ID3D12Device2>          device2;
        ComPtr<ID3D12PipelineLibrary1> lib1;
        if (FAILED(m_device.As(&device2)) || FAILED(m_psoLibrary.As(&lib1))) { fail("no mesh-shader device"); return; }
        MeshPsoStream stream{};
        stream.rootSig  = rs;
        stream.as       = bc(kAS);
        stream.ms       = bc(kMS);
        stream.ps       = bc(kPS);
        stream.blend    = CD3DX12_BLEND_DESC(rec.state.blend);
        stream.raster   = CD3DX12_RASTERIZER_DESC(rec.state.raster);
        stream.depth    = CD3DX12_DEPTH_STENCIL_DESC(rec.state.depthStencil);
        stream.dsFormat = static_cast<DXGI_FORMAT>(rec.state.dsvFormat);
        D3D12_RT_FORMAT_ARRAY rtfa{};
        rtfa.NumRenderTargets = rec.state.numRenderTargets;
        for (UINT i = 0; i < rec.state.numRenderTargets && i < 8u; ++i)
            rtfa.RTFormats[i] = static_cast<DXGI_FORMAT>(rec.state.rtvFormats[i]);
        stream.rtFormats  = rtfa;
        stream.sample     = DXGI_SAMPLE_DESC{ rec.state.sampleCount, rec.state.sampleQuality };
        stream.sampleMask = rec.state.sampleMask;
        stream.topology   = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(rec.state.topologyType);
        if (missing) { fail("a shader blob is missing"); return; }
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{ sizeof(stream), &stream };
        if (SUCCEEDED(lib1->LoadPipeline(rec.name.c_str(), &streamDesc, IID_PPV_ARGS(&pso))))
        {
            ++job.present;
            return;
        }
        hr = device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pso));
        break;
    }
    case Kind::Compute:
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rs;
        d.CS = bc(kCS);
        if (missing) { fail("a shader blob is missing"); return; }
        if (SUCCEEDED(m_psoLibrary->LoadComputePipeline(rec.name.c_str(), &d, IID_PPV_ARGS(&pso))))
        {
            ++job.present;
            return;
        }
        hr = m_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso));
        break;
    }
    }
    if (FAILED(hr)) { fail("the driver refused it"); return; }
    NotePsoLibStore(nameHash, m_psoLibrary->StorePipeline(rec.name.c_str(), pso.Get()));
    ++job.built;
}

RHI::PipelinePrecompileStatus GraphicsDX12::GetPipelinePrecompileStatus() const
{
    RHI::PipelinePrecompileStatus s = m_precompileStatus;
    if (const std::shared_ptr<PipelinePrecompile> job = m_precompile)
    {
        s.built     = job->built.load();
        s.present   = job->present.load();
        s.failed    = job->failed.load();
        s.finished  = s.built + s.present + s.failed;
        s.done      = job->running.load() == 0;
        s.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job->start).count();
    }
    return s;
}

void GraphicsDX12::FinishPipelinePrecompile(bool cancel, const std::string& cacheFilePath)
{
    const std::shared_ptr<PipelinePrecompile> job = m_precompile;
    if (!job) return;
    if (cancel) job->cancel.store(true);
    for (std::thread& t : job->workers)
        if (t.joinable()) t.join();
    m_precompileStatus = GetPipelinePrecompileStatus();
    m_precompile.reset();
    const RHI::PipelinePrecompileStatus& s = m_precompileStatus;
    LOG_INFO("Pipeline precompile: %s - built %u, already in the library %u, failed %u of %u in %.0f ms",
             cancel ? "cancelled" : "done", s.built, s.present, s.failed, s.total, s.elapsedMs);

    if (!cacheFilePath.empty()) WritePSOLibrary(cacheFilePath.c_str(), /*quietInUse*/ false);
    // Only a complete one is remembered: a cancelled or partly failed preparation runs again next boot.
    if (cancel || s.failed != 0 || s.finished != s.total || job->indexPath.empty() || cacheFilePath.empty()) return;
    // The library's serialized size, what InitPSOLibrary reports as taken from disk next boot.
    const uint64_t bytes = static_cast<uint64_t>(m_psoLibrary->GetSerializedSize());
    const std::string line = IndexLine(job->manifestHash, m_adapterDesc, m_driverVersion, bytes) + "\n";
    if (!Sys::WriteFileBytes(job->indexPath, line.data(), line.size()))
        LOG_WARNING("Pipeline precompile: cannot write '%s'", job->indexPath.c_str());
}
