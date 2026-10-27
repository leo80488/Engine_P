#pragma once

// PSOCache - in-memory pipeline state object deduplication.
// - Caches RHI::PipelineState objects keyed by PSODesc::Hash().
// - On a miss: fetches the RHI::Shader handles from ShaderLibrary and calls
//   IGraphicsDevice::CreatePipelineState, which handles the ID3D12PipelineLibrary disk cache
//   (InitPSOLibrary / SavePSOLibrary).
// - Thread-safe: GetOrCreate is guarded by an internal mutex, and concurrent misses on one key
//   build it once.
// Call Init() once at startup. GetOrCreate() warms up at load time and is safe at runtime for
// unanticipated permutations: a miss builds synchronously on the calling thread, unless misses are
// deferred (SetDeferMisses) and the frame is recording - then it builds in the background and the
// draw that asked is skipped until it is ready.
// No DX12 types appear in this header.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/PermutationKey.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <array>
#include <atomic>
#include <memory>

class IGraphicsDevice;
class ShaderLibrary;
enum class ShaderID : uint32_t;

// Vertex input layout id, part of the PSO key. Every pass uses None (vertices are fetched from
// bindless buffers, PVF) and CreateNew never binds an input layout.
enum class InputLayoutType : uint8_t
{
    None        = 0,  // no IA layout: SV_VertexID or bindless vertex fetch
    StaticMesh  = 1,  // POSITION(f3) NORMAL(f3) COLOR(f3)
};

// PSODesc - plain-data descriptor used as the PSOCache key; every field is value-comparable and
// hashable.
struct PSODesc
{
    static constexpr ShaderID kNoPixelShader = static_cast<ShaderID>(~0u);

    static constexpr ShaderID kNoShader = static_cast<ShaderID>(~0u);

    ShaderID         vsID       = static_cast<ShaderID>(~0u);

    // Sentinel psID meaning "no pixel shader" -> depth-only PSO. It is the default, so an unset psID is
    // a depth-only pipeline.
    ShaderID         psID       = kNoPixelShader;

    // Mesh-shader pipeline: a valid msID makes this an AS/MS PSO (vsID is then ignored), asID
    // optional. The sentinel is the same "none" value, and the two ids are mixed into the hash only
    // when set, so a VS desc's hash (and its disk PSO name) does not depend on them.
    ShaderID         msID       = kNoShader;
    ShaderID         asID       = kNoShader;
    PermutationKey   perm       = {};

    InputLayoutType  inputLayout = InputLayoutType::None;

    // Render state (copied by value for hashing)
    RHI::RasterizerState   rs  = {};
    RHI::DepthStencilState dss = {};
    RHI::BlendState        bs  = {};

    RHI::PrimitiveTopology     topology   = RHI::PrimitiveTopology::TRIANGLELIST;

    std::array<RHI::Format, 8> rtvFormats = {};
    uint32_t                   rtvCount   = 0;
    RHI::Format                dsvFormat  = RHI::Format::UNKNOWN;
    uint32_t                   sampleCount = 1;

    // Stable FNV-1a hash of every field that shapes the pipeline: perm masked to the bits the stages
    // read, rtvFormats up to rtvCount, the mesh-stage ids only when set.
    uint64_t Hash() const noexcept;
};

class PSOCache
{
public:
    PSOCache();
    // Waits for a background build of this cache's (SetDeferMisses) that is running.
    ~PSOCache();

    void Init(IGraphicsDevice& gfx, ShaderLibrary& shaderLib);
    void Shutdown();

    // ---- Misses while a frame records ----
    // On (a shipped game whose pipelines were prepared before its boot scene, Graphics/PsoPrepare.h):
    // a pipeline the PSO library lacks, asked for on the render half of a frame, is built by a
    // low-priority background task and GetOrCreate answers nullptr - the caller skips that draw - until
    // it is ready, instead of the frame waiting on the driver. Each such pipeline is logged once
    // ("[PSO] runtime miss"). Off: every miss builds on the asking thread. A request outside the render
    // half (a pass's Init, a warm-up) always builds where it is.
    static void     SetDeferMisses(bool on);
    static bool     DeferMisses();
    static uint32_t DeferredMisses();   // pipelines this process built in the background
    // After a nullptr from GetOrCreate(@p desc): true when that was a deferred miss - a background task
    // builds it, or has just finished - and false when the pipeline failed. Do not latch a path off or
    // block a shader for a deferred one.
    bool IsPending(const PSODesc& desc);

    // Returns a pointer into the internal cache table (stable until Clear() / Shutdown()).
    // Thread-safe; creates the PSO on the first call for a unique desc. Returns nullptr if shader lookup
    // or PSO creation fails.
    const RHI::PipelineState* GetOrCreate(const PSODesc& desc);

    // Drop every cached PSO. The next GetOrCreate rebuilds through the backend (through
    // ID3D12PipelineLibrary when cached on disk). Hot reload calls this after
    // ShaderLibrary::ClearCaches so the next GetOrCreate compiles fresh.
    // Pointers returned by earlier GetOrCreate calls DANGLE: callers must re-fetch, typically by
    // re-running the owning pass's Init.
    void Clear();

    // Build an InputLayout for a given InputLayoutType (shared helper).
    static RHI::InputLayout BuildInputLayout(InputLayoutType type);

private:
    // Destroys every cached PSO through the backend, then empties m_cache; without it a hot reload
    // orphans every ID3D12PipelineState for the process lifetime. The release is deferred inside the
    // backend, so a command list recorded this frame still finishes on the old pipeline. Caller holds
    // m_mutex.
    void ReleaseAllLocked();

    // A background build's link to its cache: the task builds only while cache is set, under mutex, so
    // the destructor (which clears it) waits for a build that is running.
    struct Life
    {
        std::mutex mutex;
        PSOCache*  cache = nullptr;
    };

    // Creates a new PSO; called on cache miss. @p libraryOnly: load it from the PSO library or fail
    // quietly, never a driver build.
    // Returns an invalid RHI::PipelineState on error.
    RHI::PipelineState CreateNew(const PSODesc& desc, bool libraryOnly = false);
    // A deferred miss's build, on a background task: the PSO joins the cache (or the failed set).
    void FinishDeferred(uint64_t key, const PSODesc& desc);

    IGraphicsDevice* m_gfx       = nullptr;
    ShaderLibrary*   m_shaderLib = nullptr;

    std::unordered_map<uint64_t, RHI::PipelineState> m_cache;
    // Negative cache: PSODesc::Hash() values whose CreateNew failed, so a material with a broken custom
    // PS does not re-run CreateNew and re-log an ERROR on every draw. Cleared by Clear() (hot reload),
    // so a fixed shader is retried.
    std::unordered_set<uint64_t> m_failedPSOs;
    std::mutex m_mutex;
    // Keys a thread is building right now. A second thread that misses one waits for it instead of
    // building a twin (a record lane and a preload's worker warm-up can ask for the same PSO).
    std::unordered_set<uint64_t> m_inFlight;
    std::condition_variable      m_inFlightCv;
    // Keys a background task is building (SetDeferMisses): GetOrCreate answers nullptr for them.
    std::unordered_set<uint64_t> m_deferred;
    std::shared_ptr<Life>        m_life;
};
