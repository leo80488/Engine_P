#include "Graphics/PSOCache.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/DynamicShaderRegistry.h"   // ids as names in the errors
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/PipelineStats.h"   // psoCacheMisses
#include "Diagnostics/Profiler.h"   // PROF_SCOPE
#include "System/Log.h"
#include "System/TaskSystem.h"      // a deferred miss's background build
#include "System/ThreadContext.h"   // the render half of a frame

#include <chrono>
#include <cstring>

namespace
{
    std::atomic<bool>     s_deferMisses{ false };
    std::atomic<uint32_t> s_deferredMisses{ 0 };
}

// FNV-1a 64-bit; called once per draw packet per pass.
// No version constant: this is the IN-MEMORY cache key (and the seed of the disk PSO library name),
// and an in-memory table lives for one process run. Root-signature edits invalidate the disk
// library through GraphicsDX12::m_rootSigHash (FNV-1a over the serialized blob), which
// GraphicsDX12::ComputePSOName mixes into the entry name.
uint64_t PSODesc::Hash() const noexcept
{
    constexpr uint64_t kPrime = 1099511628211ull;

    uint64_t h = 14695981039346656037ull;

    // Per-byte FNV-1a: robust and obviously deterministic. No raw-struct bulk mixing on purpose:
    // padding bytes are the enemy.
    auto mixU8  = [&h, kPrime](uint8_t v)  { h ^= v; h *= kPrime; };
    auto mixU32 = [&](uint32_t v) {
        mixU8(static_cast<uint8_t>(v));
        mixU8(static_cast<uint8_t>(v >>  8));
        mixU8(static_cast<uint8_t>(v >> 16));
        mixU8(static_cast<uint8_t>(v >> 24));
    };
    auto mixI32  = [&](int32_t  v) { mixU32(static_cast<uint32_t>(v)); };
    auto mixF32  = [&](float    v) { uint32_t u; std::memcpy(&u, &v, 4); mixU32(u); };
    auto mixBool = [&](bool     v) { mixU8(v ? 1 : 0); };
    auto mixEnum = [&](auto     v) { mixU32(static_cast<uint32_t>(v)); };

    // ---- Top-level scalars ----
    mixEnum(vsID);
    mixEnum(psID);
    // Only the permutation bits that can change a stage's bytecode belong in the PSO identity (the rest
    // produce byte-identical DXIL under another key). With the union of the stage masks, two descs that
    // collide here agree on every masked key, so CreateNew fetches the same shaders for either. It is
    // finer than ShaderLibrary::KeyBits, which also canonicalises the blend bits: two spellings of one
    // blend are two PSOs over one VS/PS (a duplicate, never a collision), and the on-disk PSO names stay
    // stable. What else varies with `perm` (cull mode from DOUBLE_SIDED, blend state from the blend
    // bits) is hashed below via rs / bs.
    // kNoPixelShader is not a shader id: a depth-only PSO has no PS bytecode, hence mask 0.
    const bool meshPipeline = (msID != kNoShader);
    uint32_t permMask = meshPipeline ? ShaderLibrary::RelevantPermMask(static_cast<uint32_t>(msID))
                                     : ShaderLibrary::RelevantPermMask(static_cast<uint32_t>(vsID));
    if (psID != kNoPixelShader)
        permMask |= ShaderLibrary::RelevantPermMask(static_cast<uint32_t>(psID));
    if (meshPipeline && asID != kNoShader)
        permMask |= ShaderLibrary::RelevantPermMask(static_cast<uint32_t>(asID));
    // The mesh stages join the identity only when present, so a VS desc's hash (and the disk PSO name
    // seeded from it) does not depend on them.
    if (meshPipeline)
    {
        mixU8(0x4D);   // 'M': a mesh pipeline never collides with a VS one that happens to share ids
        mixEnum(msID);
        mixEnum(asID);
    }
    mixU32(perm.bits & permMask);
    mixU8(static_cast<uint8_t>(inputLayout));
    mixEnum(topology);
    mixEnum(dsvFormat);
    mixU32(rtvCount);
    mixU32(sampleCount);
    for (uint32_t i = 0; i < rtvCount && i < 8; ++i)
        mixEnum(rtvFormats[i]);

    // ---- RasterizerState ----
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

    // ---- DepthStencilState ----
    mixBool(dss.depthEnable);
    mixEnum(dss.depthWriteMask);
    mixEnum(dss.depthFunc);
    mixBool(dss.stencilEnable);
    mixU8  (dss.stencilReadMask);
    mixU8  (dss.stencilWriteMask);
    auto mixOp = [&](const RHI::DepthStencilState::DepthStencilOp& op) {
        mixEnum(op.stencilFailOp);
        mixEnum(op.stencilDepthFailOp);
        mixEnum(op.stencilPassOp);
        mixEnum(op.stencilFunc);
    };
    mixOp  (dss.frontFace);
    mixOp  (dss.backFace);

    // ---- BlendState (8 RT slots) ----
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

    return h;
}

RHI::InputLayout PSOCache::BuildInputLayout(InputLayoutType type)
{
    RHI::InputLayout il;
    switch (type)
    {
    case InputLayoutType::StaticMesh:
        il.elements = {
            { "POSITION", 0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::kAppendAlignedElement,
              RHI::InputClassification::PER_VERTEX_DATA },
            { "NORMAL",   0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::kAppendAlignedElement,
              RHI::InputClassification::PER_VERTEX_DATA },
            { "COLOR",    0, RHI::Format::R32G32B32_FLOAT, 0,
              RHI::InputLayout::kAppendAlignedElement,
              RHI::InputClassification::PER_VERTEX_DATA },
        };
        break;
    case InputLayoutType::None:
    default:
        break;
    }
    return il;
}

void PSOCache::Init(IGraphicsDevice& gfx, ShaderLibrary& shaderLib)
{
    m_gfx       = &gfx;
    m_shaderLib = &shaderLib;
    m_cache.reserve(64);
}

void PSOCache::Shutdown()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    ReleaseAllLocked();     // before m_gfx is nulled, or the destroy is unreachable
    m_failedPSOs.clear();
    m_gfx       = nullptr;
    m_shaderLib = nullptr;
}

void PSOCache::Clear()
{
    // A worker warm-up building into this cache finishes its material first.
    ShaderCacheGate::Exclusive gate;
    std::lock_guard<std::mutex> lk(m_mutex);
    ReleaseAllLocked();
    m_failedPSOs.clear();   // hot-reload retries previously-broken PSOs
}

PSOCache::PSOCache()
    : m_life(std::make_shared<Life>())
{
    m_life->cache = this;
}

PSOCache::~PSOCache()
{
    std::lock_guard<std::mutex> lock(m_life->mutex);
    m_life->cache = nullptr;
}

void PSOCache::SetDeferMisses(bool on)
{
    s_deferMisses.store(on, std::memory_order_relaxed);
}

bool PSOCache::DeferMisses()
{
    return s_deferMisses.load(std::memory_order_relaxed);
}

uint32_t PSOCache::DeferredMisses()
{
    return s_deferredMisses.load(std::memory_order_relaxed);
}

bool PSOCache::IsPending(const PSODesc& desc)
{
    // A nullptr is a failure (the failed set) or a deferral; a deferral that finished since is in m_cache.
    const uint64_t key = desc.Hash();
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_failedPSOs.count(key) == 0;
}

void PSOCache::FinishDeferred(uint64_t key, const PSODesc& desc)
{
    const auto t0 = std::chrono::steady_clock::now();
    RHI::PipelineState pso = CreateNew(desc);
    static std::atomic<uint32_t> s_finished{ 0 };
    if (s_finished.fetch_add(1, std::memory_order_relaxed) < 32)
        LOG_INFO("[PSO] background build %s: vs=%s ms=%s ps=%s perm=0x%08X in %.1f ms", pso.IsValid() ? "done" : "FAILED",
                 DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.vsID)).c_str(),
                 DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.msID)).c_str(),
                 DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.psID)).c_str(), desc.perm.bits,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    std::lock_guard<std::mutex> lk(m_mutex);
    m_deferred.erase(key);
    if (!pso.IsValid())
    {
        m_failedPSOs.insert(key);
        return;
    }
    auto [it, inserted] = m_cache.emplace(key, std::move(pso));
    if (!inserted && m_gfx) m_gfx->DestroyPipelineState(pso);
}

void PSOCache::ReleaseAllLocked()
{
    if (m_gfx)
        for (auto& kv : m_cache)
            m_gfx->DestroyPipelineState(kv.second);
    m_cache.clear();
}

const RHI::PipelineState* PSOCache::GetOrCreate(const PSODesc& desc)
{
    const uint64_t key = desc.Hash();

    {
        std::unique_lock<std::mutex> lk(m_mutex);
        for (;;)
        {
            auto it = m_cache.find(key);
            if (it != m_cache.end())
                return &it->second;
            // Known-broken PSO: skip the rebuild so per-frame draws do not re-run CreateNew and re-log
            // "PS not found". Clear() (hot reload) drops this set so a fixed shader is rebuilt.
            if (m_failedPSOs.find(key) != m_failedPSOs.end())
                return nullptr;
            // Being built in the background: this draw goes without it.
            if (m_deferred.count(key)) return nullptr;
            // Another thread is building this PSO: wait and take its result.
            if (!m_inFlight.count(key)) break;
            PROF_SCOPE("PSOCache.WaitInFlight");
            m_inFlightCv.wait(lk, [this, key] { return !m_inFlight.count(key); });
        }
        m_inFlight.insert(key);
    }
    struct FlightRelease
    {
        PSOCache& cache; uint64_t key;
        ~FlightRelease()
        {
            { std::lock_guard<std::mutex> lk(cache.m_mutex); cache.m_inFlight.erase(key); }
            cache.m_inFlightCv.notify_all();
        }
    } flightRelease{ *this, key };

    PipelineStats::psoCacheMisses.fetch_add(1, std::memory_order_relaxed);
    const uint8_t ctx = Sys::ThreadCtx::Current();
    const bool defer = s_deferMisses.load(std::memory_order_relaxed) &&
                       (ctx == Sys::ThreadCtx::Render || ctx == Sys::ThreadCtx::RenderReadExempt);
    RHI::PipelineState pso = CreateNew(desc, /*libraryOnly*/ defer);
    if (!pso.IsValid() && defer)
    {
        // Not in the library: a background task builds it, and the draw that asked is skipped meanwhile.
        { std::lock_guard<std::mutex> lk(m_mutex); m_deferred.insert(key); }
        const uint32_t n = s_deferredMisses.fetch_add(1, std::memory_order_relaxed);
        if (n < 32)
            LOG_INFO("[PSO] runtime miss vs=%s ms=%s ps=%s perm=0x%08X: built in the background, its draws skipped "
                     "until it is ready", DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.vsID)).c_str(),
                     DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.msID)).c_str(),
                     DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.psID)).c_str(), desc.perm.bits);
        std::weak_ptr<Life> life = m_life;
        TaskSystem::Get().Push([life, key, desc]
        {
            const std::shared_ptr<Life> l = life.lock();
            if (!l) return;
            std::lock_guard<std::mutex> lock(l->mutex);
            if (l->cache) l->cache->FinishDeferred(key, desc);
        });
        return nullptr;
    }
    if (!pso.IsValid())
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_failedPSOs.insert(key);
        return nullptr;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    auto [it, inserted] = m_cache.emplace(key, std::move(pso));
    // A Clear() in between can bring the key back: the cached pipeline stands, and this one goes back
    // to the backend instead of leaking.
    if (!inserted && m_gfx) m_gfx->DestroyPipelineState(pso);
    return &it->second;
}

RHI::PipelineState PSOCache::CreateNew(const PSODesc& desc, bool libraryOnly)
{
    // A cache miss here is a first-use PSO build on whatever thread asked: shader fetch (maybe a
    // compile) plus the device's create.
    PROF_SCOPE("PSOCache.CreateNew");
    if (!m_gfx || !m_shaderLib)
    {
        LOG_ERROR("PSOCache: not initialized");
        return {};
    }

    // A mesh pipeline takes MS (+ optional AS) in place of the VS; the device builds it through the
    // pipeline-state STREAM path.
    const bool meshPipeline = (desc.msID != PSODesc::kNoShader);
    const RHI::Shader* vs = nullptr;
    const RHI::Shader* ms = nullptr;
    const RHI::Shader* as = nullptr;
    if (meshPipeline)
    {
        ms = m_shaderLib->GetShader(desc.msID, desc.perm);
        if (!ms || !ms->IsValid())
        { LOG_ERROR("PSOCache: MS not found (id=%s, perm=0x%08X)", DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.msID)).c_str(), desc.perm.bits); return {}; }
        if (desc.asID != PSODesc::kNoShader)
        {
            as = m_shaderLib->GetShader(desc.asID, desc.perm);
            if (!as || !as->IsValid())
            { LOG_ERROR("PSOCache: AS not found (id=%s, perm=0x%08X)", DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.asID)).c_str(), desc.perm.bits); return {}; }
        }
    }
    else
    {
        vs = m_shaderLib->GetShader(desc.vsID, desc.perm);
        if (!vs || !vs->IsValid())
        { LOG_ERROR("PSOCache: VS not found (id=%s, perm=0x%08X)", DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.vsID)).c_str(), desc.perm.bits); return {}; }
    }

    // psID == kNoPixelShader -> DEPTH-ONLY pipeline (no PS at all), used by the depth prepass: a real
    // null PS guarantees the fast early-Z path; an empty PS would still pay an invocation per fragment,
    // the cost the prepass exists to avoid.
    const bool depthOnly = (desc.psID == PSODesc::kNoPixelShader);
    const RHI::Shader* ps = depthOnly ? nullptr
                                      : m_shaderLib->GetShader(desc.psID, desc.perm);
    if (!depthOnly && (!ps || !ps->IsValid()))
    { LOG_ERROR("PSOCache: PS not found (id=%s, perm=0x%08X)", DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.psID)).c_str(), desc.perm.bits); return {}; }

    RHI::PipelineStateDesc psd;
    psd.vs         = vs;
    psd.ms         = ms;
    psd.as         = as;
    psd.ps         = ps;
    psd.rs         = &desc.rs;
    psd.dss        = &desc.dss;
    psd.bs         = &desc.bs;
    psd.il         = nullptr;  // PVF: input layout always null, no exceptions
    psd.rtvCount  = desc.rtvCount;
    psd.dsvFormat = desc.dsvFormat;
    psd.sampleCount = desc.sampleCount;
    psd.pt           = desc.topology;
    for (uint32_t i = 0; i < desc.rtvCount && i < 8; ++i)
        psd.rtvFormats[i] = desc.rtvFormats[i];

    // Stable ShaderID-based key. ComputePSOName folds it in as a SEED, then adds the stage bytecode
    // hashes + the root-signature hash, so an edited shader gets a new disk-library name instead of
    // colliding with the old one.
    psd.cacheKey    = desc.Hash();
    psd.libraryOnly = libraryOnly;

    RHI::PipelineState outPSO;
    if (!m_gfx->CreatePipelineState(psd, outPSO))
    {
        if (libraryOnly) return {};   // not in the library: the caller defers it
        LOG_ERROR("PSOCache: CreatePipelineState failed (vsID=%s, msID=%s, asID=%s, psID=%s, perm=0x%08X)",
                  DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.vsID)).c_str(),
                  DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.msID)).c_str(),
                  DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.asID)).c_str(),
                  DynamicShaderRegistry::Get().Label(static_cast<uint32_t>(desc.psID)).c_str(),
                  desc.perm.bits);
        return {};
    }
    return outPSO;
}
