#pragma once

// NrdDenoiser - native D3D12 backend for NVIDIA NRD (Real-time Denoisers).
// Wraps ONE nrd::Instance running ONE Kind, set before the first Resize (RelaxDiffuseSpecularSplit is
// two NRD denoisers in that instance). The per-frame descriptor/CB ring and the prev-camera cache
// advance once per Denoise(), so one wrapper must NOT be shared across denoisers or called twice a
// frame.
// NRD owns no GPU objects: it hands over pipeline descriptions (embedded DXIL), a texture-pool spec
// and a per-frame dispatch list (CB bytes + bindings + grid size). This class realises them with
// fully self-contained D3D12 resources, so it never fights the engine's descriptor management:
//   pool textures    raw committed resources (not RHI::Texture)
//   descriptor heap  own shader-visible CBV_SRV_UAV heap, ring-buffered per frame (the caller
//                    re-binds the engine heaps after Denoise())
//   root signatures  one per NRD pipeline (root CBV + SRV table + UAV table + 2 static samplers,
//                    spaces from InstanceDesc)
//   constant buffer  one persistent-mapped upload ring
// Pass-owned textures cross the boundary as UserTexture{texture, state}; Denoise() transitions
// them as each dispatch needs and RESTORES the entry state, so the pass's RHI state tracking stays
// truthful. Same recording contract as the NRD SDK's Integration/NRDIntegration.hpp, without NRI.

#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

#include "NRD.h"          // external/NRD/Include (added to include dirs)
#include "NRDDescs.h"
#include "NRDSettings.h"

#include "Graphics/GraphicsStruct.h"   // RHI::Texture, RHI::ResourceState

class GraphicsDX12;
class IGraphicsDevice;
namespace RHI { struct CommandList; }

class NrdDenoiser
{
public:
    // Which NRD denoiser an instance runs.
    // Append only: the [NRD_POOL] / [NRD_CADENCE] lines log the numeric value and tools match it
    // (restir_gi_energy.py reads kind 8), so reordering silently repoints them at another denoiser.
    enum class Kind : uint32_t
    {
        ReblurSpecular,
        SigmaShadow,
        ReblurDiffuseOcclusion,
        // SIGMA_SHADOW plus a translucency channel: adds IN_TRANSLUCENCY and
        // widens OUT_SHADOW_TRANSLUCENCY to RGBA8. Used by RTShadowPass so
        // alpha-blend occluders produce a partial shadow.
        SigmaShadowTranslucency,
        // REBLUR_DIFFUSE: full diffuse RADIANCE + hit distance (not the occlusion-only variant), for
        // SSGIPass's hybrid diffuse-GI signal. Unlike REBLUR_DIFFUSE_OCCLUSION it has a temporal-
        // stabilization pass that declares IN_MV as an OUTPUT, so the motion-vector texture MUST be created
        // with UNORDERED_ACCESS; RTAOPass's SRV-only MV texture fails at bind time with no useful message.
        ReblurDiffuse,
        ReblurDiffuseSpecular,
        Reference,
        // RELAX_DIFFUSE_SPECULAR: the A-trous denoiser NVIDIA designed for
        // RTXDI (ReSTIR DI) signals. Unlike REBLUR it does not size its blur
        // from the hit distance (only the pre-pass and specular tracking read
        // it), so a punctual light's hard shadow survives denoising. Inputs are
        // RELAX_FrontEnd_PackRadianceAndHitDist (world-space hit distance).
        RelaxDiffuseSpecular,
        // RELAX_DIFFUSE: the same A-trous filter for the ReSTIR GI diffuse signal. A ReSTIR reservoir's
        // selected connection is usually a long one (the light it found), so REBLUR's hit-distance-sized
        // kernel would blur contact shadows and corner darkening away.
        RelaxDiffuse,
        // RELAX_DIFFUSE (identifier 0) + RELAX_SPECULAR (identifier 1) in ONE
        // instance, same inputs/outputs as RelaxDiffuseSpecular. RELAX has a
        // single anti-firefly switch for both lobes; splitting lets ReSTIR DI
        // run it on the specular lobe only (Params::diSpecularAntiFirefly) -
        // the lamp-highlight flicker is specular, while anti-firefly on the
        // diffuse lobe clipped ~3% of the occluded multi-light fixture.
        RelaxDiffuseSpecularSplit,
        // REBLUR_SPECULAR with the library's own settings, which NRD tunes for
        // noisy path-traced signals under camera motion (ReblurSpecular keeps
        // RTReflectionPass's short anti-ghost histories for its comparatively
        // clean mirror rays). PathTracePass's glass reflection under DLSS-RR:
        // one full path per pane pixel per frame. Same I/O as ReblurSpecular.
        ReblurSpecularPathTraced,
    };

    // One user-side texture NRD reads or writes, in RHI terms: its format is the texture's own.
    // `state` is read AND written: Denoise() transitions the resource as its dispatches require and
    // puts it back into the entry state before returning.
    struct UserTexture
    {
        const RHI::Texture* texture = nullptr;
        RHI::ResourceState  state   = RHI::ResourceState::SHADER_RESOURCE_COMPUTE;
    };

    struct Params
    {
        // Engine XMFLOAT4X4 memory (row-major, row-vector v*M) IS the column-major column-vector layout
        // NRD asks for: memcpy'd verbatim. Both matrices must be NON-jittered.
        float    viewToClip[16];    // proj (no jitter)
        float    worldToView[16];   // view
        float    jitter[2];         // current sub-pixel jitter, [-0.5;0.5] px
        uint32_t frameIndex   = 0;
        bool     resetHistory = false;
        float    denoisingRange = 1000.0f;  // ~farZ; viewZ beyond = sky
        uint32_t width  = 0;
        uint32_t height = 0;
        // SIGMA_SHADOW only: world-space direction TO the sun (normalized). Ignored by REBLUR kinds.
        float    lightDirection[3] = { 0.0f, 0.0f, 0.0f };
        // REBLUR_DIFFUSE_OCCLUSION only: the AO ray max distance (world m). Becomes
        // hitDistanceParameters.A (with B=0, C=1) so a MISS (hitDist == this) normalizes to 1 = fully
        // unoccluded. MUST equal the pack shader's normalization A, or the AO comes out systematically
        // dark. Ignored by the other kinds.
        float    aoDistanceScale = 1.0f;
        // REBLUR_DIFFUSE only: hitDistanceParameters.A, with B=0 and C=1 so a ray reaching its maximum
        // length normalizes to exactly 1. MUST equal SSGIConstants::hitDistParamA (what SSGINRDPack feeds
        // to REBLUR_FrontEnd_GetNormHitDist): if they drift, REBLUR denormalizes with another constant and
        // the blur radius and temporal reprojection go wrong globally and silently, as on the specular and
        // AO paths. Ignored by the other kinds.
        float    diffHitDistScaleA = 4.0f;
        // DI uses one range for both lobes and the pack shader: {A,0,1}.
        float    diHitDistScaleA = 150.0f;
        // DI-only: convert full-render-resolution pixel radii to trace pixels.
        float    diBlurRadiusScale = 1.0f;
        bool     diAntiFirefly = true;
        // RelaxDiffuseSpecularSplit only: the specular denoiser's anti-firefly
        // (diAntiFirefly then drives the diffuse one).
        bool     diSpecularAntiFirefly = true;
        bool     diTemporalStabilization = true;
        bool     diPrepass = true;
        bool     diHistoryCounts = false;
        // RELAX_DIFFUSE (ReSTIR GI) only: pre-pass radius in NRD pixels, anti-firefly.
        float    giPrepassRadius = 0.0f;
        bool     giAntiFirefly = false;
        // RELAX_DIFFUSE (ReSTIR GI) A/B knobs; the defaults are NRD's own.
        uint32_t giVarianceHistory = 3;      // spatialVarianceEstimationHistoryThreshold
        float    giAntilagReset = 0.5f;      // antilagSettings.resetAmount
        float    giAntilagSpatialSigma = 4.5f; // antilagSettings.spatialSigmaScale
        uint32_t giAtrous = 5;               // atrousIterationNum
        float    giHistorySeconds = 0.5f;    // diffuseMaxAccumulatedFrameNum, in seconds
        // Floor in frames: at the ~8-10 fps a full-quality ReSTIR GI frame runs
        // at, 0.5 s is only 8 frames of history.
        uint32_t giMinHistoryFrames = 16;
        bool     historyConfidenceAvailable = false;
        // REBLUR_SPECULAR only: hitDistanceParameters.A. MUST equal the value the pack shader normalizes
        // with (HybridReflNRDPack's kHitDistParams.x, from HybridReflConstants::hitDistParamA), or REBLUR
        // denormalizes with another constant and its blur radius and virtual-motion specular reprojection
        // go wrong globally, with no error. Scales with RTReflectionPass::rayTMax
        // (A = rayTMax * kHitDistAScale). The default 3.0f is the nrd::ReblurSettings library default.
        float    specHitDistScaleA = 3.0f;
    };

    // One (ResourceType -> texture) binding the caller supplies for its denoiser's I/O slots. Order
    // does not matter; NRD requests by type.
    struct Binding
    {
        nrd::ResourceType type;
        UserTexture       tex;
    };

    ~NrdDenoiser();

    // Set ONCE before the first Resize (the instance bakes it at creation); ignored afterwards.
    void SetKind(Kind k) { m_kind = k; }
    Kind GetKind() const { return m_kind; }
    // FP32 float pools for the DI / GI kinds; set before Resize. RTDIPass and
    // RTGIPass turn them on: the typed FP16 UAV store truncates, and RELAX's
    // temporal loop compounds that over its history. Other kinds keep the SDK formats.
    void SetDiFloat32Pool(bool enabled) { m_diFloat32Pool=enabled; }
    void SetGiFloat32Pool(bool enabled) { m_giFloat32Pool=enabled; }
    bool GiFloat32Pool() const { return m_giFloat32Pool; }

    // (Re)creates the nrd::Instance + all GPU objects for a render size.
    // Cheap no-op when the size is unchanged. Returns false on any failure
    // (caller falls back to the non-NRD denoiser chain).
    bool Resize(IGraphicsDevice& gfx, uint32_t width, uint32_t height);
    void Shutdown();

    bool IsReady() const { return m_instance != nullptr && m_ready; }

    // Records every NRD dispatch for this frame onto `cl`. Caller contract:
    //  * user textures already in the states given in each UserTexture
    //  * after return the caller MUST re-bind engine descriptor heaps + its
    //    own root signature (NRD replaced both on the command list).
    // @p bindings must cover every ResourceType this denoiser's dispatches
    // reference that isn't an NRD pool texture (max ~6).
    bool Denoise(IGraphicsDevice& gfx, RHI::CommandList cl, const Params& p,
                 const Binding* bindings, uint32_t bindingCount);

private:
    struct PoolTexture
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        DXGI_FORMAT           format = DXGI_FORMAT_UNKNOWN;
        D3D12_RESOURCE_STATES state  = D3D12_RESOURCE_STATE_COMMON;
    };

    bool CreateInstanceAndPipelines(GraphicsDX12& gfx);
    bool CreatePools(GraphicsDX12& gfx);
    void DestroyGpuObjects();

    bool m_diFloat32Pool=false;
    bool m_giFloat32Pool=false;

    nrd::Instance* m_instance = nullptr;
    Kind           m_kind     = Kind::ReblurSpecular;
    bool           m_ready    = false;
    uint32_t       m_width  = 0;
    uint32_t       m_height = 0;

    // Per-pipeline PSO + root signature (index == InstanceDesc pipeline index).
    std::vector<Microsoft::WRL::ComPtr<ID3D12RootSignature>> m_rootSigs;
    std::vector<Microsoft::WRL::ComPtr<ID3D12PipelineState>> m_psos;

    std::vector<PoolTexture> m_permanent;
    std::vector<PoolTexture> m_transient;

    // Shader-visible descriptor ring (kFrameCount sections).
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descHeap;
    uint32_t m_descPerFrame   = 0;
    uint32_t m_descSize       = 0;
    uint32_t m_frameSlot      = 0;   // rotates 0..kFrameCount-1 per Denoise()

    // Constant-buffer upload ring (persistent-mapped).
    Microsoft::WRL::ComPtr<ID3D12Resource> m_cbUpload;
    uint8_t* m_cbMapped       = nullptr;
    uint32_t m_cbSliceSize    = 0;   // 256-aligned constantBufferMaxDataSize
    uint32_t m_cbSlicesPerFrame = 0;

    // Cached previous-frame camera state (NRD wants prev matrices + jitter).
    float m_prevViewToClip[16]  = {};
    float m_prevWorldToView[16] = {};
    float m_prevJitter[2]       = {};
    bool  m_havePrev            = false;

    // Frame-rate tracking for time-normalized accumulation: REBLUR history lengths are in FRAMES, so a
    // fixed count is a trail 3x longer in wall-clock time at 60 fps than at 180 fps. Denoise() runs
    // once per frame, so the delta between calls is the frame time (EMA-smoothed, clamped against
    // pauses / breakpoints).
    long long m_lastDenoiseUs = 0;    // steady_clock micros of previous call
    float     m_smoothedFps   = 0.0f; // 0 = no sample yet
};
