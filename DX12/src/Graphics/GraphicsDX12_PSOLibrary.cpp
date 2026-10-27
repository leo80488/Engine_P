#include "Graphics/GraphicsDX12.h"
#include "Graphics/GraphicsDX12Internal.h"
#include "System/Log.h"
#include "System/PathUtil.h"   // Sys::OpenIfstream, Sys::WriteFileBytesAtomic

#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

// The pipeline library (ID3D12PipelineLibrary): its disk cache, pso_cache.bin, with the adapter
// fingerprint in front, and the name every PSO is stored under in it.

// ==== PSO library: disk-based ISA cache ====

// Adapter fingerprint prepended to pso_cache.bin. The library blob is driver ISA:
// CreatePipelineLibrary rejects another GPU's or driver's blob (ADAPTER_NOT_FOUND /
// DRIVER_VERSION_MISMATCH) and the library starts empty, but when the cache path is not writable the
// stale blob stays and every launch pays the full DXIL -> ISA rebuild while the log looks like a
// cache attempt. Checking the fingerprint ourselves logs the miss and what changed (GPU model, driver).
// The adapter LUID is recorded for that log but not compared: Windows assigns it anew at every boot,
// and whether this machine's blob still loads is CreatePipelineLibrary's call.
// The header is fixed at a 16-byte multiple on purpose: D3D gets a pointer INTO the std::vector
// buffer (16-byte aligned on x64), so the payload after it stays 16-byte aligned without a memmove.
namespace
{
    constexpr uint32_t kPSOCacheMagic   = 0x4F535049u;  // 'IPSO' little-endian
    constexpr uint32_t kPSOCacheVersion = 1u;

    struct PSOCacheHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t vendorId;
        uint32_t deviceId;
        uint32_t subSysId;
        uint32_t revision;
        uint32_t luidLow;
        int32_t  luidHigh;
        uint64_t driverVersion;   // 0 = adapter did not report one
        uint64_t payloadSize;     // bytes of library blob following this header
    };
    static_assert(sizeof(PSOCacheHeader) == 48,
                  "PSOCacheHeader must stay 48 bytes (a multiple of 16) so the "
                  "payload pointer handed to CreatePipelineLibrary stays aligned");
}

bool GraphicsDX12::InitPSOLibrary(const char* cacheFilePath)
{
    // Try to get ID3D12Device1 (required for CreatePipelineLibrary)
    ComPtr<ID3D12Device1> device1;
    if (FAILED(m_device->QueryInterface(IID_PPV_ARGS(&device1))))
    {
        LOG_WARNING("InitPSOLibrary: ID3D12Device1 not available ??PSO library disabled");
        return false;
    }

    // Read the existing cache from disk (optional: a missing file is fine).
    const void* pData    = nullptr;
    SIZE_T      dataSize = 0;

    if (cacheFilePath && cacheFilePath[0] != '\0')
    {
        // Every failure below has the same recovery - leave m_psoLibraryData
        // empty and let the caller rebuild - so this is an early-return lambda.
        auto loadCache = [&]
        {
            std::ifstream f = Sys::OpenIfstream(cacheFilePath, std::ios::binary | std::ios::ate);
            if (!f.is_open())
                return;                     // no cache yet: normal on first launch

            // tellg() returns -1 on a stream error; casting that to size_t asks
            // for a ~16 EB allocation, so bail before resize().
            const auto sz = f.tellg();
            if (sz <= 0)
            {
                LOG_INFO("InitPSOLibrary: '%s' is empty or unreadable ??rebuilding PSO cache",
                         cacheFilePath);
                return;
            }

            f.seekg(0, std::ios::beg);
            m_psoLibraryData.resize(static_cast<size_t>(sz));
            f.read(reinterpret_cast<char*>(m_psoLibraryData.data()), sz);
            if (!f.good())
            {
                LOG_WARNING("InitPSOLibrary: short read on '%s' ??rebuilding PSO cache",
                            cacheFilePath);
                m_psoLibraryData.clear();
                return;
            }
            if (m_psoLibraryData.size() < sizeof(PSOCacheHeader))
            {
                // Also catches a headerless blob when it is tiny; a large headerless blob
                // fails the magic check below.
                LOG_INFO("InitPSOLibrary: '%s' is too small to carry a fingerprint "
                         "(%zu bytes) ??rebuilding PSO cache",
                         cacheFilePath, m_psoLibraryData.size());
                m_psoLibraryData.clear();
                return;
            }

            PSOCacheHeader hdr{};
            std::memcpy(&hdr, m_psoLibraryData.data(), sizeof(hdr));

            const char* mismatch = nullptr;
            if      (hdr.magic   != kPSOCacheMagic)          mismatch = "not a PSO cache file (or pre-fingerprint format)";
            else if (hdr.version != kPSOCacheVersion)        mismatch = "cache format version changed";
            else if (hdr.vendorId != m_adapterDesc.VendorId ||
                     hdr.deviceId != m_adapterDesc.DeviceId ||
                     hdr.subSysId != m_adapterDesc.SubSysId ||
                     hdr.revision != m_adapterDesc.Revision) mismatch = "different GPU model";
            else if (hdr.driverVersion != m_driverVersion)   mismatch = "driver version changed";
            else if (hdr.payloadSize != m_psoLibraryData.size() - sizeof(PSOCacheHeader))
                                                            mismatch = "truncated / trailing bytes";

            if (mismatch)
            {
                // Deliberately LOG_INFO, not a warning: after a GPU swap or a
                // driver update this is the correct outcome. It exists so "the
                // first launch after updating drivers is slow" is explainable
                // instead of mysterious.
                LOG_INFO("InitPSOLibrary: PSO cache fingerprint miss (%s) ??rebuilding from DXIL. "
                         "cached VEN_%04X/DEV_%04X luid %08X-%08X drv %llu | "
                         "current VEN_%04X/DEV_%04X luid %08X-%08X drv %llu",
                         mismatch,
                         hdr.vendorId, hdr.deviceId,
                         static_cast<unsigned>(hdr.luidHigh), hdr.luidLow,
                         static_cast<unsigned long long>(hdr.driverVersion),
                         m_adapterDesc.VendorId, m_adapterDesc.DeviceId,
                         static_cast<unsigned>(m_adapterLuid.HighPart),
                         static_cast<unsigned>(m_adapterLuid.LowPart),
                         static_cast<unsigned long long>(m_driverVersion));
                m_psoLibraryData.clear();
                return;
            }

            // Hand D3D a pointer INTO the retained buffer (past the header): the
            // blob must stay alive and unmodified for the pipeline library's whole
            // lifetime, which m_psoLibraryData guarantees. No memmove:
            // sizeof(PSOCacheHeader) is a multiple of 16, so the payload keeps the
            // vector allocation's alignment.
            dataSize = static_cast<SIZE_T>(hdr.payloadSize);
            pData    = dataSize ? (m_psoLibraryData.data() + sizeof(PSOCacheHeader))
                                : nullptr;  // header-only file = empty library
        };
        loadCache();
    }

    HRESULT hr = device1->CreatePipelineLibrary(pData, dataSize, IID_PPV_ARGS(&m_psoLibrary));
    if (FAILED(hr))
    {
        // D3D12_ERROR_DRIVER_VERSION_MISMATCH or D3D12_ERROR_ADAPTER_NOT_FOUND
        // means the cached data is stale: start fresh. Those two are the expected
        // outcome of a driver update or another adapter, like a fingerprint miss.
        if (hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH || hr == D3D12_ERROR_ADAPTER_NOT_FOUND)
            LOG_INFO("InitPSOLibrary: CreatePipelineLibrary refused the cache (%s) - starting empty library",
                     hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? "driver version mismatch" : "adapter not found");
        else
            LOG_WARNING("InitPSOLibrary: CreatePipelineLibrary failed (0x%08X), starting empty library",
                        static_cast<unsigned>(hr));
        m_psoLibraryData.clear();
        hr = device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&m_psoLibrary));
        if (FAILED(hr))
        {
            LOG_WARNING("InitPSOLibrary: could not create empty library (0x%08X) ??PSO library disabled",
                        static_cast<unsigned>(hr));
            m_psoLibrary = nullptr;
            return false;
        }
    }

    // loadedBytes in the shutdown [PSO] summary. dataSize is what was really handed to
    // CreatePipelineLibrary - zero on a fingerprint miss or a short read, which "the file existed"
    // cannot tell apart.
    m_psoLibBytes = static_cast<uint64_t>(dataSize);
    LOG_INFO("InitPSOLibrary: PSO library ready (%zu bytes from disk)", dataSize);
    return true;
}

void GraphicsDX12::SavePSOLibrary(const char* cacheFilePath)
{
    WritePSOLibrary(cacheFilePath, /*quietInUse*/ false);
}

void GraphicsDX12::CheckpointPSOLibrary(const char* cacheFilePath)
{
    if (m_psoLibStoresSinceSave.load(std::memory_order_relaxed) == 0)
        return;
    WritePSOLibrary(cacheFilePath, /*quietInUse*/ true);
}

void GraphicsDX12::WritePSOLibrary(const char* cacheFilePath, bool quietInUse)
{
    if (!m_psoLibrary || !cacheFilePath || cacheFilePath[0] == '\0')
        return;
    // Taken before Serialize: a pipeline another thread stores meanwhile may miss this file, so it
    // must keep the next checkpoint due.
    const uint32_t storesSeen = m_psoLibStoresSinceSave.load(std::memory_order_relaxed);

    const SIZE_T serializedSize = m_psoLibrary->GetSerializedSize();
    if (serializedSize == 0)
        return;

    std::vector<uint8_t> buf(serializedSize);
    HRESULT hr = m_psoLibrary->Serialize(buf.data(), serializedSize);
    if (FAILED(hr))
    {
        LOG_ERROR("SavePSOLibrary: Serialize failed (0x%08X)", static_cast<unsigned>(hr));
        return;
    }

    // Fingerprint header first, then the driver blob (see PSOCacheHeader).
    PSOCacheHeader hdr{};
    hdr.magic         = kPSOCacheMagic;
    hdr.version       = kPSOCacheVersion;
    hdr.vendorId      = m_adapterDesc.VendorId;
    hdr.deviceId      = m_adapterDesc.DeviceId;
    hdr.subSysId      = m_adapterDesc.SubSysId;
    hdr.revision      = m_adapterDesc.Revision;
    hdr.luidLow       = static_cast<uint32_t>(m_adapterLuid.LowPart);
    hdr.luidHigh      = m_adapterLuid.HighPart;
    hdr.driverVersion = m_driverVersion;
    hdr.payloadSize   = static_cast<uint64_t>(buf.size());

    // ONE buffer, written through a temporary file and a rename. The default path is shared
    // (%LOCALAPPDATA%\Game\pso_cache.bin by every Game.exe of every worktree, DX12/pso_cache.bin by the
    // editors), and parallel runs read it at boot while others save it at exit: a truncate-and-write
    // would let InitPSOLibrary see half a file.
    std::vector<uint8_t> file(sizeof(hdr) + buf.size());
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    std::memcpy(file.data() + sizeof(hdr), buf.data(), buf.size());
    bool inUse = false;
    if (Sys::WriteFileBytesAtomic(cacheFilePath, file.data(), file.size(), &inUse))
    {
        m_psoLibStoresSinceSave.fetch_sub(storesSeen, std::memory_order_relaxed);
        LOG_INFO("SavePSOLibrary: saved %zu bytes (+%zu byte adapter fingerprint) to '%s'",
                 serializedSize, sizeof(PSOCacheHeader), cacheFilePath);
    }
    else if (inUse && quietInUse)
        LOG_INFO("SavePSOLibrary: checkpoint skipped - '%s' is in use by another process; "
                 "the next checkpoint or the shutdown save writes it", cacheFilePath);
    else if (inUse)
        // Another process held the file open the whole time (it was loading
        // it): the previous cache stays, intact - a cold PSO or two next time.
        LOG_WARNING("SavePSOLibrary: '%s' stayed in use by another process - kept the "
                    "previous cache, this session's new pipelines are not saved", cacheFilePath);
    else
        // Loud on purpose: a non-writable path means every launch rebuilds every
        // PSO from DXIL and nothing else in the log says so.
        LOG_ERROR("SavePSOLibrary: cannot write '%s' - the PSO cache will NOT persist "
                  "and the next launch rebuilds these pipelines", cacheFilePath);
}

// ==== PSO library name derivation ====


void GraphicsDX12::ComputePSOName(const RHI::PipelineStateDesc& desc,
                                   wchar_t* outName, size_t maxChars) const
{
    // One path for every caller. The name is the ID3D12PipelineLibrary entry key, so it must cover
    // everything that changes the resulting ISA:
    //   - desc.cacheKey when set (PSOCache's ShaderID / permutation / render-state hash), only as a
    //     seed: it mixes ShaderIDs, not DXIL, so as the whole name an edited shader would keep its
    //     name, StorePipeline would fail as a duplicate and the disk library would keep serving the
    //     first ISA;
    //   - the compiled bytecode of every stage (m_shaderPool bytecodeHash);
    //   - the serialized root signature layout (GetRootSignatureHash());
    //   - the render-state scalar fields.
    // Mix field by field, never a raw struct: padding bytes are not zero-initialised in PSO descs
    // built on the stack, which makes names random across runs and pso_cache.bin grow without bound.
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = 14695981039346656037ull;

    auto mixU8  = [&h](uint8_t v)  { h ^= v; h *= kPrime; };
    auto mixU32 = [&](uint32_t v) {
        mixU8(static_cast<uint8_t>(v));
        mixU8(static_cast<uint8_t>(v >>  8));
        mixU8(static_cast<uint8_t>(v >> 16));
        mixU8(static_cast<uint8_t>(v >> 24));
    };
    auto mixU64 = [&](uint64_t v) {
        mixU32(static_cast<uint32_t>(v));
        mixU32(static_cast<uint32_t>(v >> 32));
    };
    auto mixI32  = [&](int32_t v) { mixU32(static_cast<uint32_t>(v)); };
    auto mixF32  = [&](float   v) { uint32_t u; std::memcpy(&u, &v, 4); mixU32(u); };
    auto mixBool = [&](bool    v) { mixU8(v ? 1 : 0); };
    auto mixEnum = [&](auto    v) { mixU32(static_cast<uint32_t>(v)); };

    // Seed with the caller-supplied key when there is one (0 = "not set").
    if (desc.cacheKey != 0)
        mixU64(desc.cacheKey);

    // The binding layout the ISA was compiled against: the PSO's own pipeline layout's blob hash,
    // else the shared signature's. Without it a root-signature edit leaves every PSO name unchanged
    // and the disk library hands a stale-layout PSO to LoadGraphicsPipeline.
    uint64_t layoutHash = 0;
    if (desc.layout && desc.layout->IsValid())
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        if (desc.layout->handleId < m_layoutPool.size())
            layoutHash = m_layoutPool[desc.layout->handleId].blobHash;
    }
    mixU64(layoutHash ? layoutHash : GetRootSignatureHash());

    auto mixShader = [&](const RHI::Shader* s)
    {
        // Under m_resourceMutex: CreateShader push_backs from other workers.
        uint64_t h = 0;
        if (s && s->IsValid())
        {
            std::lock_guard<std::mutex> lock(m_resourceMutex);
            if (s->handleId < m_shaderPool.size())
                h = m_shaderPool[s->handleId].bytecodeHash;
        }
        mixU64(h);
    };
    mixShader(desc.vs);
    mixShader(desc.ps);
    mixShader(desc.cs);
    mixShader(desc.gs);
    mixShader(desc.hs);
    mixShader(desc.ds);
    // Mesh-shader stages, the topology type and the sample mask are PSO inputs too.
    mixShader(desc.ms);
    mixShader(desc.as);
    mixEnum(desc.pt);
    mixU32(desc.sampleMask);

    if (desc.rs)
    {
        const auto& rs = *desc.rs;
        mixEnum(rs.fillMode);
        mixEnum(rs.cullMode);
        mixBool(rs.frontCounterClockwise);
        mixI32 (rs.depthBias);
        mixF32 (rs.depthBiasClamp);
        mixF32 (rs.slopeScaledDepthBias);
        mixBool(rs.depthClipEnable);
        mixBool(rs.multisampleEnable);
        mixBool(rs.antialiasedLineEnable);
        mixBool(rs.conservativeRasterizationEnable);
    }
    if (desc.dss)
    {
        const auto& ds = *desc.dss;
        mixBool(ds.depthEnable);
        mixEnum(ds.depthWriteMask);
        mixEnum(ds.depthFunc);
        mixBool(ds.stencilEnable);
        mixU8  (ds.stencilReadMask);
        mixU8  (ds.stencilWriteMask);
        auto mixOp = [&](const RHI::DepthStencilState::DepthStencilOp& op) {
            mixEnum(op.stencilFailOp);
            mixEnum(op.stencilDepthFailOp);
            mixEnum(op.stencilPassOp);
            mixEnum(op.stencilFunc);
        };
        mixOp  (ds.frontFace);
        mixOp  (ds.backFace);
    }
    if (desc.bs)
    {
        const auto& bs = *desc.bs;
        mixBool(bs.alphaToCoverageEnable);
        mixBool(bs.independentBlendEnable);
        for (uint32_t i = 0; i < 8; ++i)
        {
            const auto& rt = bs.renderTarget[i];
            mixBool(rt.blendEnable);
            mixEnum(rt.srcBlend);
            mixEnum(rt.destBlend);
            mixEnum(rt.blendOp);
            mixEnum(rt.srcBlendAlpha);
            mixEnum(rt.destBlendAlpha);
            mixEnum(rt.blendOpAlpha);
            mixEnum(rt.renderTargetWriteMask);
        }
    }

    mixU32 (desc.rtvCount);
    mixEnum(desc.dsvFormat);
    for (uint32_t i = 0; i < desc.rtvCount && i < 8; ++i)
        mixEnum(desc.rtvFormats[i]);
    mixU32 (desc.sampleCount);

    swprintf_s(outName, maxChars, L"PSO_%016llX", static_cast<unsigned long long>(h));
}
