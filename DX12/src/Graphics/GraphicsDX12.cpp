#include "Graphics/GraphicsDX12.h"
#include "Graphics/GpuMemoryLedger.h"   // the profiler's GPU resource ledger
#include "Graphics/GraphicsDX12Internal.h"
#include "Diagnostics/Profiler.h"
#include "Diagnostics/ProfilerFrame.h"   // SetVideoMemoryQuery
#include "Graphics/DepthTrace.h"
#include "Graphics/DxcCompiler.h"
#include "Graphics/SamplerSlots.h"   // the fixed sampler heap slots
#include "Graphics/StreamlineBackend.h"
#include "System/Log.h"
#include "System/PathUtil.h"
#include "System/PlayerText.h"   // Sys::NotePlayerError
#include "System/Check.h"
#include "System/BootTimeline.h"
#include "System/JobDefaults.h"
#include "System/CommandLine.h"
#include "Resource/EngineSettings.h"   // PeekSettingsWantStreamline
#include "Resource/EngineDevSettings.h" // PeekEngineDevRayReconstructionLoad
#include "System/TaskGraph.h"
#include "System/OwnerTask.h"
#include "System/CrashReport.h"
#include "Harness/Determinism.h"
#include "Graphics/VideoMemory.h"
#include "Harness/RunLedger.h"   // device-lost verdict latch
#include "System/HostPolicy.h"   // Sys::IsEditorHost: Frame Generation is game-only
#include <cstring>
#include <stdexcept>
#include <queue>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include "d3dx12.h"
#include <d3d12sdklayers.h>
#include <renderdoc/renderdoc_app.h>   // -rdcapture (in-app API; MIT, vendored)
#ifdef TRACY_ENABLE
#include <psapi.h>              // GetProcessMemoryInfo (memory plots in EndFrame)
#endif

// ==== Hybrid graphics (Optimus / PowerXpress): run on the discrete GPU ====
// On an iGPU + dGPU laptop the driver gives an unknown exe the power-saving adapter;
// DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE in LoadPipeline only reorders DXGI's enumeration. The NVIDIA
// and AMD drivers look these two exported symbols up by name in the process at startup.
// They work only if the EXE exports them: EngineCore is a static lib, and the linker drops an
// unreferenced .obj together with its dllexport, silently. They live in this TU because every exe
// constructs a GraphicsDX12, so GraphicsDX12.obj is always linked. Check with
// `dumpbin /exports Game.exe` (both names must appear). If an exe ever links EngineCore without
// GraphicsDX12, move this block into that exe's main TU; do not copy it: a duplicate in one exe is a
// link error.
extern "C"
{
    __declspec(dllexport) DWORD NvOptimusEnablement                  = 1;  // NVIDIA Optimus
    __declspec(dllexport) int   AmdPowerXpressRequestHighPerformance = 1;  // AMD PowerXpress / switchable
}

using Microsoft::WRL::ComPtr;

// GPUProfiler's readback ring is read kFrameCount-1 frames late, matching the swap-chain pipelining.
// A ring depth other than kFrameCount would read a slot whose resolve is still in flight.
static_assert(GPUProfiler::kFrameCount == GraphicsDX12::kFrameCount,
              "GPUProfiler::kFrameCount must match GraphicsDX12::kFrameCount");

void ThrowIfFailedImpl(HRESULT hr, const char* expr, const char* file, int line)
{
    if (FAILED(hr))
    {
        Logger::Get().LogHResult(expr, hr, file, line);

        // Latch device-lost here, not only in Present: a TDR usually first surfaces at a fence wait, a
        // command-list Close or a resource creation. Without the latch the run reports a generic escaped C++
        // exception and looks like a code regression, when the GPU died (often because something else used
        // it).
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_HUNG ||
            hr == DXGI_ERROR_DEVICE_RESET)
        {
            Harness::RunLedger::Get().NoteDeviceLost(expr);
        }

        throw std::runtime_error("DX12 call failed");
    }
}


// ==== Default root signature builder ====
// No hand-bumped version constant: both builders FNV-1a their SERIALIZED root-signature blob into
// m_rootSigHash, and ComputePSOName mixes that into every PSO library entry name, so adding, removing,
// moving or re-scoping a root parameter renames the cached PSOs by itself.

namespace
{
    // Fold a serialized root-signature blob into a running FNV-1a 64 hash.
    // Same constants PSOCache.cpp / CreateShader use, so every content hash in
    // the engine speaks one dialect.
    void FoldRootSigBlob(uint64_t& h, ID3DBlob* blob)
    {
        if (!blob) return;
        constexpr uint64_t kPrime = 1099511628211ull;
        const auto*  bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
        const size_t count = blob->GetBufferSize();
        for (size_t i = 0; i < count; ++i) { h ^= bytes[i]; h *= kPrime; }
    }

    // ---- Barrier journal ----
    // The device journaling the current frame, for GfxDiag::NoteNativeBarriers:
    // a free function, because most native sites hold a raw D3D12 list and no
    // device. Null outside a journaled frame, which is what keeps every report
    // at one load.
    std::atomic<GraphicsDX12*> g_journalDevice{ nullptr };

    // Private data on every pooled D3D12 command list: its pool slot. Written
    // once, when BeginCommandList creates the list.
    constexpr GUID kPoolSlotGuid =
        { 0x6c2f1d4e, 0x93a7, 0x4b15, { 0x8e, 0x2d, 0x5a, 0x71, 0xc0, 0x3b, 0x9f, 0x46 } };

    // WKPDID_D3DDebugObjectNameW (d3dcommon.h), spelled out so this TU needs no
    // dxguid.lib: the key ID3D12Object::SetName stores the name under.
    constexpr GUID kDebugObjectNameW =
        { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };

    // The UTF-8 debug name of a D3D12 object, "" when it has none.
    std::string DebugNameOf(ID3D12Object* object)
    {
        if (!object) return {};
        UINT bytes = 0;
        if (FAILED(object->GetPrivateData(kDebugObjectNameW, &bytes, nullptr)) || bytes < sizeof(wchar_t))
            return {};
        std::wstring wide(bytes / sizeof(wchar_t), L'\0');
        if (FAILED(object->GetPrivateData(kDebugObjectNameW, &bytes, wide.data())))
            return {};
        while (!wide.empty() && wide.back() == L'\0') wide.pop_back();   // SetName stores the terminator
        if (wide.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            out.data(), n, nullptr, nullptr);
        return out;
    }

    // The heaps both SHARED signatures let a shader index directly - from the
    // constants ShaderBindingValidation checks the shaders against
    // (RootSignatureLayout.h), so the two can never disagree.
    D3D12_ROOT_SIGNATURE_FLAGS SharedHeapFlags()
    {
        UINT f = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        if (RHI::Slots::kSharedResourceHeapIndexed) f |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        if (RHI::Slots::kSharedSamplerHeapIndexed)  f |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
        return static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(f);
    }
}

ComPtr<ID3D12RootSignature> GraphicsDX12::CreateDefaultRootSignature()
{
    // Slot indices come from Graphics/RootSignatureLayout.h, the shared table
    // render passes read too. Do not reintroduce local literals here: a
    // local copy that drifts from the header is a silent wrong-resource bind.
    namespace S = RHI::Slots::Gfx;
    constexpr UINT kBindlessTexSlot  = S::BindlessTex.index;  // t0 space2: bindless texture array
    constexpr UINT kMaterialBufSlot  = S::MaterialBuf.index;  // t12 space0: MaterialBuffer (per-material params)
    constexpr UINT kEmissiveSRVSlot  = S::EmissiveSRV.index;  // t17 space0: GBuffer emissive SRV (deferred lighting)
    constexpr UINT kSSAOSRVSlot      = S::SSAOSRV.index;      // t18 space0: SSAO texture (XeGTAO, one frame of latency)
    constexpr UINT kSkySHSRVSlot      = S::SkySH.index;       // t19 space0: sky SH coefficient buffer (9 x float4)
    constexpr UINT kAerialPerspSlot   = S::AerialPersp.index; // t20 space0: aerial perspective 3D LUT (Texture3D<float4>)
    constexpr UINT kSpotShadowAtlasSlot = S::SpotShadowAtlas.index; // t21 space0: SpotShadowPass atlas (Texture2DArray<float>)
    constexpr UINT kSpotShadowVPSlot  = S::SpotShadowVP.index; // t22 space0: StructuredBuffer<float4x4> per-slice VPs
    constexpr UINT kCustomMatCBVSlot  = S::CustomMatCBV.index; // b8 space0: per-material CBV packed from shader reflection
    constexpr UINT kCustomMatTexSlot  = S::CustomMatTex.index; // t0..t(N-1) space3: per-material named texture table
    // Source of truth is GraphicsDX12::kCustomMatTextureSlots (header). Use
    // that here so root-sig + descriptor-table sizes can never drift.
    constexpr UINT kCustomMatTexCount = GraphicsDX12::kCustomMatTextureSlots;
    // t16 and t23..t45 are reserved registers, not parameters (RootSignatureLayout.h, namespace
    // BindlessIdx): the signature must stay within the D3D12 limit of 64 DWORDs, so these resources are
    // heap indices opened with ResourceDescriptorHeap[] at no root cost:
    //     t24 probe buffer     -> LightCB.reflProbeBufIdx
    //     t25 probe grid       -> LightCB.reflProbeGridIdx
    //     t26 probe index list -> LightCB.reflProbeIndexIdx
    //     t27 SSR result       -> LightCB.ssrResultIdx
    //     t28 DDGI volume buf  -> LightCB.ddgiVolumeBufIdx
    //     t29..t32 DDGI SH     -> VolumeGPUDesc.irradianceBaseIdx   + volumeSlot
    //     t33..t36 DDGI depth  -> VolumeGPUDesc.depthBaseIdx     + volumeSlot
    //     t37..t40 DDGI probes -> VolumeGPUDesc.probeDataBaseIdx + volumeSlot
    //     t41 point cube atlas -> LightCB.pointShadowIdx
    //     t42 per-object map   -> LightCB.perObjShadowIdx
    //     t43 hair map         -> LightCB.hairShadowIdx
    //     t44 RTR              -> LightCB.rtReflectionIdx
    //     t45 RT sun mask      -> LightCB.rtShadowMaskIdx
    // t16 (the global NPR ramp) and t23 (the probe cube array) have no carrier: nothing reads them; each
    // Toon material samples its own ramp and each probe cube is a per-cube bindless SRV
    // (probe.cubemapSrvIndex). t23 is only a register: ReflectionProbeManager still owns the
    // TextureCubeArray every cubemapSrvIndex is a view of, so do not delete that resource.
    constexpr UINT totalParams = S::ParamCount; // 33 - highest slot (DynamicSampler) + 1
    static_assert(totalParams == S::DynamicSampler.index + 1,
                  "RootSignatureLayout.h Gfx::ParamCount must cover every declared slot");
    D3D12_ROOT_PARAMETER params[totalParams] = {};

    // [0] Root constants: 4 uint32 at b0 space0
    //     [0] meshDescIdx  [1] instanceOffset  [2] materialIndex  [3] outlinePixelsBits (asfloat)
    params[kRootConstantsSlot].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstantsSlot].Constants.ShaderRegister = 0;
    params[kRootConstantsSlot].Constants.RegisterSpace  = 0;
    params[kRootConstantsSlot].Constants.Num32BitValues = 4;
    params[kRootConstantsSlot].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    // [1..7] Root CBVs: b1-b7 space0
    for (UINT i = 0; i < kCBVSlotCount; ++i)
    {
        params[kCBVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[kCBVSlotBase + i].Descriptor.ShaderRegister = i + 1; // b1..b7
        params[kCBVSlotBase + i].Descriptor.RegisterSpace  = 0;
        params[kCBVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [8] Root SRV: InstanceBuffer at t0 space0. Visibility ALL: the GBuffer / Transparent / Shadow
    // pixel stages read per-instance dissolve params through the VS-forwarded instanceSlot.
    params[kInstanceBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kInstanceBufSlot].Descriptor.ShaderRegister = 0; // t0
    params[kInstanceBufSlot].Descriptor.RegisterSpace  = 0;
    params[kInstanceBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [9] Root SRV: MeshDescriptors at t1 space0. Visibility ALL: the meshlet mesh shader runs the
    // GBuffer vertex function (gbuffer_vertex.hlsli) and the amplification shader reads the mesh's
    // sidecar. No other parameter binds t1 space0.
    params[kMeshDescSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kMeshDescSlot].Descriptor.ShaderRegister = 1; // t1
    params[kMeshDescSlot].Descriptor.RegisterSpace  = 0;
    params[kMeshDescSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [10..13] Descriptor tables: 1 SRV each at t2-t5 space0
    static D3D12_DESCRIPTOR_RANGE s_srvRanges[kSRVSlotCount] = {};
    for (UINT i = 0; i < kSRVSlotCount; ++i)
    {
        s_srvRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        s_srvRanges[i].NumDescriptors                    = 1;
        s_srvRanges[i].BaseShaderRegister                = i + 2; // t2..t5
        s_srvRanges[i].RegisterSpace                     = 0;
        s_srvRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &s_srvRanges[i];
        params[kSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    // [14] Descriptor table: kMaxBindlessBuffers SRVs at t0 space1 (bindless g_Buffers[])
    static D3D12_DESCRIPTOR_RANGE s_bindlessRange = {};
    s_bindlessRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_bindlessRange.NumDescriptors                    = kMaxBindlessBuffers;
    s_bindlessRange.BaseShaderRegister                = 0; // t0
    s_bindlessRange.RegisterSpace                     = 1; // space1
    s_bindlessRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessSlot].DescriptorTable.pDescriptorRanges   = &s_bindlessRange;
    // Visibility ALL: the meshlet AS / MS fetch vertices, meshlet records and the sidecar through
    // g_Buffers[] (t0 space1 is bound by no other parameter).
    params[kBindlessSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // No sampler tables: every shader takes its samplers from the sampler heap (see [32]).
    // ShaderBindingValidation names a graphics shader that declares a sampler register.

    // [15..17] Descriptor tables - 1 SRV each at t6-t8 space0 (IBL cubemaps + BRDF LUT)
    static D3D12_DESCRIPTOR_RANGE s_iblRanges[kIBLSRVSlotCount] = {};
    for (UINT i = 0; i < kIBLSRVSlotCount; ++i)
    {
        s_iblRanges[i].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        s_iblRanges[i].NumDescriptors                    = 1;
        s_iblRanges[i].BaseShaderRegister                = i + kSRVSlotCount + 2; // t6..t8
        s_iblRanges[i].RegisterSpace                     = 0;
        s_iblRanges[i].OffsetInDescriptorsFromTableStart = 0;

        params[kIBLSRVSlotBase + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kIBLSRVSlotBase + i].DescriptorTable.NumDescriptorRanges = 1;
        params[kIBLSRVSlotBase + i].DescriptorTable.pDescriptorRanges   = &s_iblRanges[i];
        params[kIBLSRVSlotBase + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [18] Descriptor table - 3 SRVs at t9-t11 space0. Only t9 is used: the CSM
    // is ONE Texture2DArray SRV holding every cascade; t10/t11 are unused.
    static D3D12_DESCRIPTOR_RANGE s_shadowRange = {};
    s_shadowRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_shadowRange.NumDescriptors                    = kShadowSRVCount;
    s_shadowRange.BaseShaderRegister                = kSRVSlotCount + 2 + kIBLSRVSlotCount; // t9
    s_shadowRange.RegisterSpace                     = 0;
    s_shadowRange.OffsetInDescriptorsFromTableStart = 0;

    params[kShadowSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kShadowSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kShadowSRVSlot].DescriptorTable.pDescriptorRanges   = &s_shadowRange;
    params[kShadowSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [19-21] Descriptor tables - 1 SRV each at t13-t15 space0 (clustered lighting)
    static D3D12_DESCRIPTOR_RANGE s_clusterRanges[kClusterSRVSlotCount] = {};
    for (UINT ci = 0; ci < kClusterSRVSlotCount; ++ci)
    {
        s_clusterRanges[ci].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        s_clusterRanges[ci].NumDescriptors                    = 1;
        s_clusterRanges[ci].BaseShaderRegister                = 13 + ci; // t13, t14, t15 (after shadow t9-t11)
        s_clusterRanges[ci].RegisterSpace                     = 0;
        s_clusterRanges[ci].OffsetInDescriptorsFromTableStart = 0;

        params[kClusterSRVSlotBase + ci].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[kClusterSRVSlotBase + ci].DescriptorTable.NumDescriptorRanges = 1;
        params[kClusterSRVSlotBase + ci].DescriptorTable.pDescriptorRanges   = &s_clusterRanges[ci];
        params[kClusterSRVSlotBase + ci].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // [23] Descriptor table - 1 SRV at t12 space0 (MaterialBuffer for per-material NPR params)
    static D3D12_DESCRIPTOR_RANGE s_materialBufRange = {};
    s_materialBufRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_materialBufRange.NumDescriptors                    = 1;
    s_materialBufRange.BaseShaderRegister                = 12; // t12
    s_materialBufRange.RegisterSpace                     = 0;
    s_materialBufRange.OffsetInDescriptorsFromTableStart = 0;

    params[kMaterialBufSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kMaterialBufSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kMaterialBufSlot].DescriptorTable.pDescriptorRanges   = &s_materialBufRange;
    params[kMaterialBufSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [22] Descriptor table - unbounded SRVs at t0 space2 (bindless texture array)
    static D3D12_DESCRIPTOR_RANGE s_bindlessTexRange = {};
    s_bindlessTexRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_bindlessTexRange.NumDescriptors                    = kMaxBindlessTextures;
    s_bindlessTexRange.BaseShaderRegister                = 0; // t0
    s_bindlessTexRange.RegisterSpace                     = 2; // space2
    s_bindlessTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kBindlessTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kBindlessTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kBindlessTexSlot].DescriptorTable.pDescriptorRanges   = &s_bindlessTexRange;
    params[kBindlessTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [24] Descriptor table - 1 SRV at t17 space0 (GBuffer emissive for deferred lighting)
    static D3D12_DESCRIPTOR_RANGE s_emissiveRange = {};
    s_emissiveRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_emissiveRange.NumDescriptors                    = 1;
    s_emissiveRange.BaseShaderRegister                = 17; // t17
    s_emissiveRange.RegisterSpace                     = 0;
    s_emissiveRange.OffsetInDescriptorsFromTableStart = 0;

    params[kEmissiveSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kEmissiveSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kEmissiveSRVSlot].DescriptorTable.pDescriptorRanges   = &s_emissiveRange;
    params[kEmissiveSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [25] Descriptor table - 1 SRV at t18 space0 (SSAO texture from XeGTAO)
    static D3D12_DESCRIPTOR_RANGE s_ssaoRange = {};
    s_ssaoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_ssaoRange.NumDescriptors                    = 1;
    s_ssaoRange.BaseShaderRegister                = 18; // t18
    s_ssaoRange.RegisterSpace                     = 0;
    s_ssaoRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSSAOSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSSAOSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSSAOSRVSlot].DescriptorTable.pDescriptorRanges   = &s_ssaoRange;
    params[kSSAOSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [26] Descriptor table - 1 SRV at t19 space0 (Sky SH coefficient buffer)
    static D3D12_DESCRIPTOR_RANGE s_skySHRange = {};
    s_skySHRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_skySHRange.NumDescriptors                    = 1;
    s_skySHRange.BaseShaderRegister                = 19; // t19
    s_skySHRange.RegisterSpace                     = 0;
    s_skySHRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSkySHSRVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSkySHSRVSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSkySHSRVSlot].DescriptorTable.pDescriptorRanges   = &s_skySHRange;
    params[kSkySHSRVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [27] Descriptor table - 1 SRV at t20 space0 (Aerial Perspective 3D LUT)
    static D3D12_DESCRIPTOR_RANGE s_apRange = {};
    s_apRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_apRange.NumDescriptors                    = 1;
    s_apRange.BaseShaderRegister                = 20; // t20
    s_apRange.RegisterSpace                     = 0;
    s_apRange.OffsetInDescriptorsFromTableStart = 0;

    params[kAerialPerspSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kAerialPerspSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kAerialPerspSlot].DescriptorTable.pDescriptorRanges   = &s_apRange;
    params[kAerialPerspSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_PIXEL;

    // [28] Descriptor table - 1 SRV at t21 space0 (SpotShadowPass atlas).
    // Texture2DArray<float> D32; sampled with the shadow comparison sampler
    // (a fixed sampler-heap slot) for PCF inside the Lighting / Volumetric-fog
    // shaders.
    static D3D12_DESCRIPTOR_RANGE s_spotShadowAtlasRange = {};
    s_spotShadowAtlasRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_spotShadowAtlasRange.NumDescriptors   = 1;
    s_spotShadowAtlasRange.BaseShaderRegister = 21; // t21
    s_spotShadowAtlasRange.RegisterSpace      = 0;
    s_spotShadowAtlasRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowAtlasSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowAtlasSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowAtlasSlot].DescriptorTable.pDescriptorRanges   = &s_spotShadowAtlasRange;
    params[kSpotShadowAtlasSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [29] Descriptor table - 1 SRV at t22 space0 (SpotShadowPass per-slice
    // viewProj buffer, StructuredBuffer<float4x4>). Indexed by GPULight::
    // shadowSliceIdx so the consumer shader can transform worldPos into the
    // light's shadow-map space and do a single SampleCmpLevelZero.
    static D3D12_DESCRIPTOR_RANGE s_spotShadowVPRange = {};
    s_spotShadowVPRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_spotShadowVPRange.NumDescriptors   = 1;
    s_spotShadowVPRange.BaseShaderRegister = 22; // t22
    s_spotShadowVPRange.RegisterSpace      = 0;
    s_spotShadowVPRange.OffsetInDescriptorsFromTableStart = 0;

    params[kSpotShadowVPSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kSpotShadowVPSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kSpotShadowVPSlot].DescriptorTable.pDescriptorRanges   = &s_spotShadowVPRange;
    params[kSpotShadowVPSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [30] Root CBV: the per-material custom cbuffer at b8 space0. Custom GBuffer pixel or vertex
    // shaders may declare `cbuffer Foo : register(b8, space0) {...}`; Renderer packs
    // MaterialComponent::customParams into an upload-heap ring each frame and binds the GPU VA here.
    // Standard shaders do not declare b8, so leaving it unbound is harmless. Visibility ALL so custom
    // vertex hooks (custom_vertex.hlsli) read their reflected tunables too.
    params[kCustomMatCBVSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[kCustomMatCBVSlot].Descriptor.ShaderRegister = 8; // b8
    params[kCustomMatCBVSlot].Descriptor.RegisterSpace  = 0;
    params[kCustomMatCBVSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [31] Descriptor table: up to kCustomMatTexCount SRVs at t0-t7 space3. Custom shaders declare
    // `Texture2D MyTex : register(tN, space3)` and bind it by name; Renderer copies each reflected
    // texture's SRV into a per-material ring slot each frame. Unused slots get a default white
    // descriptor so sampling never reads random heap memory.
    static D3D12_DESCRIPTOR_RANGE s_customMatTexRange = {};
    s_customMatTexRange.RangeType        = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    s_customMatTexRange.NumDescriptors   = kCustomMatTexCount;
    s_customMatTexRange.BaseShaderRegister = 0;  // t0
    s_customMatTexRange.RegisterSpace      = 3;  // space3
    s_customMatTexRange.OffsetInDescriptorsFromTableStart = 0;

    params[kCustomMatTexSlot].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kCustomMatTexSlot].DescriptorTable.NumDescriptorRanges = 1;
    params[kCustomMatTexSlot].DescriptorTable.pDescriptorRanges   = &s_customMatTexRange;
    // ALL so custom vertex shaders can vertex-texture-fetch (e.g. heightmap
    // displacement) from their reflected space3 textures, mirroring the PS.
    params[kCustomMatTexSlot].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    // [32] The one sampler a pass picks at run time: its heap index as a 1-DWORD root constant at b0
    // space9, read by sampler_heap.hlsli as SamplerDescriptorHeap[g_DynamicSamplerIdx]. Every other
    // sampler is a fixed heap slot the shader names as a constant (shared/sampler_slots.hlsli).
    // IGraphicsDevice::SetDynamicSampler writes it.
    params[S::DynamicSampler.index].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[S::DynamicSampler.index].Constants.ShaderRegister = 0;
    params[S::DynamicSampler.index].Constants.RegisterSpace  = 9;
    params[S::DynamicSampler.index].Constants.Num32BitValues = 1;
    params[S::DynamicSampler.index].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    // [32] is the last parameter. Rules the heap-index carriers of t16 / t23..t45 depend on:
    // - An absent resource gets a TYPE-CORRECT stand-in (LightingPass's 1x1 fallback textures and null
    //   structured-buffer SRVs, WaterPass's 1x1 black texture), never a descriptor of another type: a
    //   cube-array descriptor opened as a StructuredBuffer is UB that Resource Binding Tier 3 does not report.
    // - Renderer writes the LightCB indices every frame, unconditionally (LightCB and the probe buffer
    //   are rings). The SSR result is rebuilt by SSRSubsystem::OnResize, so its index is re-derived each
    //   frame before graph.Execute: never cache it.
    // - DDGI carries one BASE index per kMaxVolumes-wide static block (allocated once, never moved;
    //   DDGIVolumeManager writes the bases, and an empty slot holds type-correct null descriptors).
    //   Lighting.ps reads base + volumeSlot with NonUniformResourceIndex, which is required: the slot is a
    //   per-pixel loop variable.
    // - A ResourceDescriptorHeap[] read on a signature without HEAP_DIRECTLY_INDEXED compiles under dxc
    //   and fails at PSO creation.
    // - t23 space0 in HybridReflIntersectHW/SW.cs is RTReflectionPass's private signature, unrelated.

    // The budget RootSignatureLayout.h states (and the skills cite) is the cost
    // of what was just built: root constants 1 DWORD each, a root descriptor 2,
    // a table 1.
    UINT dwords = 0;
    for (const D3D12_ROOT_PARAMETER& p : params)
        dwords += p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS ? p.Constants.Num32BitValues
                : p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE ? 1u : 2u;
    ENGINE_CHECK(dwords == S::DwordCost,
                 "graphics root signature costs %u DWORD, RootSignatureLayout.h Gfx::DwordCost says %u",
                 dwords, S::DwordCost);

    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters     = totalParams;
    rootDesc.pParameters       = params;
    rootDesc.NumStaticSamplers = 0;
    // HEAP_DIRECTLY_INDEXED (SM6.6, Resource Binding Tier 3): shaders open probe cubemaps, shadow maps,
    // DDGI data and the other heap-index resources listed above with ResourceDescriptorHeap[], at no root
    // DWORD. The deferred lighting path depends on it for reflections, shadows and GI, so a device
    // without Tier 3 + SM6.6 is rejected at init.
    rootDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE            // no IA layout: programmable vertex fetch
                               | SharedHeapFlags();   // + the resource and sampler heaps

    ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                             &sigBlob, &errorBlob);
    if (FAILED(hr))
    {
        const char* msg = errorBlob ? static_cast<const char*>(errorBlob->GetBufferPointer()) : "";
        LOG_ERROR("CreateDefaultRootSignature failed: %s", msg);
        ThrowIfFailed(hr);
    }

    // These bytes ARE the layout: fold them in before the blob is dropped, so the disk PSO names track
    // the root signature.
    FoldRootSigBlob(m_rootSigHash, sigBlob.Get());

    ComPtr<ID3D12RootSignature> rs;
    ThrowIfFailed(m_device->CreateRootSignature(
        0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    NoteRootSignature(rs.Get(), sigBlob->GetBufferPointer(), sigBlob->GetBufferSize());
    return rs;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> GraphicsDX12::CreateComputeRootSignature()
{
    // Slot indices come from Graphics/RootSignatureLayout.h (RHI::Slots::Cmp);
    // the GraphicsDX12.h class banner narrates the same table.
    namespace C = RHI::Slots::Cmp;
    CD3DX12_ROOT_PARAMETER1 params[C::ParamCount];
    params[C::CB.index].InitAsConstantBufferView(0, 2, D3D12_ROOT_DESCRIPTOR_FLAG_NONE,
                                       D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 srvRanges[5];
    srvRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 2);
    srvRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 2);
    srvRanges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 2);
    srvRanges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 2);
    srvRanges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 2); // morph deltas
    CD3DX12_DESCRIPTOR_RANGE1 srvRange5;
    srvRange5.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5, 2);  // morph weights
    params[C::SRV_t0.index].InitAsDescriptorTable(1, &srvRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[C::SRV_t1.index].InitAsDescriptorTable(1, &srvRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[C::SRV_t2.index].InitAsDescriptorTable(1, &srvRanges[2], D3D12_SHADER_VISIBILITY_ALL);
    params[C::SRV_t3.index].InitAsDescriptorTable(1, &srvRanges[3], D3D12_SHADER_VISIBILITY_ALL);
    params[C::SRV_t4.index].InitAsDescriptorTable(1, &srvRanges[4], D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_DESCRIPTOR_RANGE1 uavRanges[2];
    uavRanges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 2);
    uavRanges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 2);
    params[C::UAV_u0.index].InitAsDescriptorTable(1, &uavRanges[0], D3D12_SHADER_VISIBILITY_ALL);
    params[C::UAV_u1.index].InitAsDescriptorTable(1, &uavRanges[1], D3D12_SHADER_VISIBILITY_ALL);
    params[C::SRV_t5.index].InitAsDescriptorTable(1, &srvRange5,    D3D12_SHADER_VISIBILITY_ALL);

    // [9], [10]: mirror of graphics [8], [9] for PVF under compute. Only
    // SceneVoxelize binds them; unused slots are legal.
    static CD3DX12_DESCRIPTOR_RANGE1 s_instanceRange;
    static CD3DX12_DESCRIPTOR_RANGE1 s_meshDescRange;
    s_instanceRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0); // t0 space0
    s_meshDescRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0); // t1 space0
    params[C::InstanceBuf.index].InitAsDescriptorTable(1, &s_instanceRange, D3D12_SHADER_VISIBILITY_ALL);
    params[C::MeshDesc.index].InitAsDescriptorTable(1, &s_meshDescRange, D3D12_SHADER_VISIBILITY_ALL);

    // [11]: bindless vertex/index buffer array, the graphics layout, so
    // pvf_fetch.hlsli works under compute.
    CD3DX12_DESCRIPTOR_RANGE1 bindlessRange;
    bindlessRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                       kMaxBindlessBuffers, 0, 1,
                       D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[C::BindlessBuffers.index].InitAsDescriptorTable(1, &bindlessRange, D3D12_SHADER_VISIBILITY_ALL);

    // [12]: SpotShadowPass atlas (Texture2DArray<float>) at t6 space2.
    // [13]: per-slice viewProj StructuredBuffer at t7 space2.
    // FroxelLightInject samples them so spot beams blocked by walls do not light
    // the fog behind them.
    static CD3DX12_DESCRIPTOR_RANGE1 s_spotShadowAtlasRangeC;
    static CD3DX12_DESCRIPTOR_RANGE1 s_spotShadowVPRangeC;
    s_spotShadowAtlasRangeC.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6, 2); // t6 space2
    s_spotShadowVPRangeC   .Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7, 2); // t7 space2
    params[C::SpotShadowAtlas.index].InitAsDescriptorTable(1, &s_spotShadowAtlasRangeC, D3D12_SHADER_VISIBILITY_ALL);
    params[C::SpotShadowVP.index].InitAsDescriptorTable(1, &s_spotShadowVPRangeC,    D3D12_SHADER_VISIBILITY_ALL);

    // [14..16]: u2 / u3 / u4 space2 (extra UAV slots for the XeGTAO prefilter).
    static CD3DX12_DESCRIPTOR_RANGE1 s_uavRangeU2;
    static CD3DX12_DESCRIPTOR_RANGE1 s_uavRangeU3;
    static CD3DX12_DESCRIPTOR_RANGE1 s_uavRangeU4;
    s_uavRangeU2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2, 2);  // u2 space2
    s_uavRangeU3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 3, 2);  // u3 space2
    s_uavRangeU4.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 4, 2);  // u4 space2
    params[C::UAV_u2.index].InitAsDescriptorTable(1, &s_uavRangeU2, D3D12_SHADER_VISIBILITY_ALL);
    params[C::UAV_u3.index].InitAsDescriptorTable(1, &s_uavRangeU3, D3D12_SHADER_VISIBILITY_ALL);
    params[C::UAV_u4.index].InitAsDescriptorTable(1, &s_uavRangeU4, D3D12_SHADER_VISIBILITY_ALL);

    // [17]: bindless Texture2D[] at t0 space3, bound to the same heap-region base as graphics
    // kBindlessTexSlot; only the register / space differ, so compute and graphics HLSL layouts are
    // independent. DESCRIPTORS_VOLATILE as on the graphics side: new texture allocations may change the
    // table between recordings.
    static CD3DX12_DESCRIPTOR_RANGE1 s_bindlessTex2DRange;
    s_bindlessTex2DRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                            kMaxBindlessTextures, 0, 3,
                            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
    params[C::BindlessTex.index].InitAsDescriptorTable(1, &s_bindlessTex2DRange, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[3]{};

    // s0 space2: linear clamp (Sample / SampleLevel).
    samplers[0].Init(0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f, 16, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z (ignored: non-comparison filter)
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[0].RegisterSpace = 2;

    // s1 space2: comparison sampler (PCF) for CSM sampling in the volumetric-fog light-inject shader.
    // Do not pair SampleCmpLevelZero with a non-comparison sampler: it silently returns garbage (no god
    // rays).
    samplers[1].Init(1,
        D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
        0.0f, 0, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // reversed Z: lit when receiver z >= SM z
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[1].RegisterSpace = 2;

    // s2 space2: linear WRAP for tileable noise (volumetric clouds: base shape, detail erosion, weather
    // map). frac() wrapping with the clamp sampler leaves a one-texel seam per tile.
    samplers[2].Init(2,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f, 16, D3D12_COMPARISON_FUNC_GREATER_EQUAL, // ignored: non-comparison filter
        D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
        0.0f, D3D12_FLOAT32_MAX,
        D3D12_SHADER_VISIBILITY_ALL);
    samplers[2].RegisterSpace = 2;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init_1_1(C::ParamCount, params, C::StaticSamplerCount, samplers,
                    SharedHeapFlags());

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rsDesc,
                     D3D_ROOT_SIGNATURE_VERSION_1_1,
                     &serialized, &error);
    if (FAILED(hr))
    {
        if (error) LOG_ERROR("CreateComputeRootSignature: %s",
                             static_cast<char*>(error->GetBufferPointer()));
        return nullptr;
    }

    // Fold the compute layout in too: compute PSOs share the same disk library.
    FoldRootSigBlob(m_rootSigHash, serialized.Get());

    ComPtr<ID3D12RootSignature> rs;
    hr = m_device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                       serialized->GetBufferSize(),
                                       IID_PPV_ARGS(&rs));
    if (FAILED(hr)) { LOG_ERROR("CreateComputeRootSignature: CreateRootSignature failed"); return nullptr; }
    NoteRootSignature(rs.Get(), serialized->GetBufferPointer(), serialized->GetBufferSize());
    return rs;
}

#ifdef TRACY_ENABLE
#include <tracy/TracyD3D12.hpp>
// Pimpl payload for GraphicsDX12's Tracy GPU state. One D3D12 context per queue;
// per-pass D3D12ZoneScope objects are split across BeginGPUTimestamp (emplace)
// and EndGPUTimestamp (reset), keyed by the timestamp profiler's region index.
struct GraphicsDX12::TracyGpuState
{
    tracy::D3D12QueueCtx*                ctx[3] = {};   // [GRAPHICS, COMPUTE, COPY]
    std::optional<tracy::D3D12ZoneScope> zones[GPUProfiler::kMaxRegions];
};
#endif

// ==== Construction / destruction ====

GraphicsDX12::GraphicsDX12()
{
    LOG_INFO("GraphicsDX12 constructor");
    // The main surface first, detached (no swap chain): it carries the present
    // settings and the frame limiter, and MainSurface() must be valid for the
    // device's whole life. The destructor destroys it explicitly.
    m_surface = std::make_unique<SurfaceDX12>(*this);
    // The device alone needs no window. CreateMainSurface puts the swap chain on a window afterwards;
    // App does it right after this.
    LoadPipeline();
    LoadAssets();
    // ImGui init lives in the editor (EditorImGuiBridge::Attach): the engine is UI-agnostic.
    m_gpuProfiler.Init(m_device.Get(),
                       m_queues[0].Get(),  // GRAPHICS
                       m_queues[1].Get(),  // COMPUTE
                       m_queues[2].Get()); // COPY
    // This frame's transient constants. A frame takes a few dozen 256-byte blocks plus one 4 KiB chunk
    // per recording thread; 512 KiB a slot is ample, and [CBALLOC] logs how much was really taken.
    {
        uint32_t perSlot = 512u * 1024u;
#ifdef WITH_HARNESS
        // -cballoc_kib=<n> (the transient_constants pack's reverse arm): a
        // slot that small runs out in the first frame, which must be loud.
        if (const int kib = Sys::CommandLine::Get().GetInt("-cballoc_kib", 0); kib > 0)
            perSlot = static_cast<uint32_t>(kib) * 1024u;
#endif
        m_transientConstants.Init(m_device.Get(), kFrameCount, perSlot, 4096u);
    }

    // Engine profiler: video memory for its per-frame memory sample. Called on
    // the profiler's COLLECTOR thread - IDXGIAdapter3::QueryVideoMemoryInfo is
    // free-threaded - so the main thread pays nothing for it. The adapter is a
    // process-lifetime static like the other VRAM readers in this file; the
    // collector is stopped before the device is torn down.
    {
        static Microsoft::WRL::ComPtr<IDXGIAdapter3> s_profAdapter;
        if (m_factory && m_device)
            m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&s_profAdapter));
        if (s_profAdapter)
        {
            Diag::Prof::SetVideoMemoryQuery([](Diag::Prof::VideoMemory& out) noexcept -> bool
            {
                DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonLocal{};
                if (!s_profAdapter || FAILED(s_profAdapter->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)))
                    return false;
                if (FAILED(s_profAdapter->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal)))
                    nonLocal = {};
                out.localUsage     = local.CurrentUsage;
                out.localBudget    = local.Budget;
                out.nonLocalUsage  = nonLocal.CurrentUsage;
                out.nonLocalBudget = nonLocal.Budget;
                return true;
            });
        }
    }

    // -gpuprofile: enable the timestamp profiler at run time. Set outside the TRACY_ENABLE block because
    // only Profile|x64 enables Tracy (TRACY_ENABLE changes struct layout, so Engine.Cpp.props keeps it uniform
    // across projects); without this flag a Release run reads zero GPU timings. The Tracy block below
    // forces it on, which is why this assignment comes first.
    m_gpuProfiler.enabled = Sys::CommandLine::Get().Has("-gpuprofile");

    // GPU debug markers: they name passes in RenderDoc / PIX captures and DRED reports. On with
    // -gpumarkers or DX12_MARKERS=1, or when a capture tool is already injected into the process;
    // DX12_MARKERS=0 forces them off. The [GPUMARKERS] line is the only way to tell "markers on" from
    // "markers never armed".
    {
        char envBuf[8]{};
        const DWORD envLen = ::GetEnvironmentVariableA("DX12_MARKERS", envBuf, sizeof(envBuf));
        const int  env  = (envLen == 0 || envLen >= sizeof(envBuf)) ? 0 : (envBuf[0] == '0' ? -1 : 1);
        const bool flag = Sys::CommandLine::Get().Has("-gpumarkers");
        const bool tool = ::GetModuleHandleW(L"renderdoc.dll") != nullptr ||
                          ::GetModuleHandleW(L"WinPixGpuCapturer.dll") != nullptr;
        // DRED on implies markers on: its breadcrumb contexts ARE these events,
        // and a hang report that cannot name the pass is what DRED is armed to avoid.
        const bool dred = IsDREDEnabled();
        m_debugMarkers = (env != 0) ? (env > 0) : (flag || tool || dred);
        if (m_debugMarkers)
            LOG_INFO("[GPUMARKERS] active (%s): graph passes and GPU profiler regions are named "
                     "events, command lists are named per use",
                     env > 0 ? "DX12_MARKERS=1" : (flag ? "-gpumarkers" : (tool ? "capture tool loaded" : "DRED")));
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -rdcapture=N: when RenderDoc launched this process (renderdoccmd capture
    // ... Game.exe ...), have it capture frame N with no keypress, so an
    // unattended run leaves a .rdc to inspect; EndFrame logs the path. Never
    // loads RenderDoc: without it the flag only reports that it did nothing.
    if (const int rdFrame = Sys::CommandLine::Get().GetInt("-rdcapture", 0); rdFrame > 0)
    {
        m_rdCaptureFrame = static_cast<uint64_t>(rdFrame);
        if (HMODULE rd = ::GetModuleHandleW(L"renderdoc.dll"))
            if (auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(::GetProcAddress(rd, "RENDERDOC_GetAPI")))
                if (getApi(eRENDERDOC_API_Version_1_1_2, &m_rdocApi) != 1) m_rdocApi = nullptr;
        if (m_rdocApi)
            LOG_INFO("[RDCAPTURE] armed: RenderDoc will capture frame %d", rdFrame);
        else
            LOG_WARNING("[RDCAPTURE] -rdcapture=%d ignored: this process was not started by "
                        "RenderDoc (renderdoccmd capture -w <exe> ...)", rdFrame);
    }
#endif

    // -slotkey=dxgi: kill switch for the upload-ring keying. Default (`frame`) keys every per-slot ring
    // off a monotonic frame counter; `dxgi` keys them off the swap-chain back-buffer index.
    // Slot aliasing has no loud failure mode (invisible to the debug layer, never crashes, shows as
    // intermittent skinned-mesh flicker), so this is the one-flag A/B for such a report, without a
    // build.
    m_slotKeyDxgi = (Sys::CommandLine::Get().Get("-slotkey", "frame") == "dxgi");
    m_slotLag = static_cast<uint64_t>(
        std::max(0, Sys::CommandLine::Get().GetInt("-slotlag", 0)));
    if (m_slotLag > 0)
        LOG_INFO("[SPLITCHECK] slotlag=%llu (render half reports itself this many "
                 "frames behind; renders from the wrong upload slot on purpose)",
                 static_cast<unsigned long long>(m_slotLag));
    {
        char*  b = nullptr; size_t n = 0;
        if (_dupenv_s(&b, &n, "DX12_SLOTAUDIT") == 0 && b)
        {
            m_slotAudit = (b[0] == '1');
            std::free(b);
        }
    }
    LOG_INFO("[SPLITCHECK] slotkey=%s", m_slotKeyDxgi ? "dxgi" : "frame");

#ifdef TRACY_ENABLE
    // Tracy GPU contexts for GRAPHICS and COMPUTE only, the queues every BeginGPUTimestamp targets. Do not
    // create one for COPY: its timestamp support is optional and CreateD3D12Context panics inside the
    // ctor without it (validity cannot be queried: GetId() is private).
    m_tracy = std::make_unique<TracyGpuState>();
    auto makeTracyCtx = [this](uint32_t q, const char* label)
    {
        auto* c = tracy::CreateD3D12Context(m_device.Get(), m_queues[q].Get());
        m_tracy->ctx[q] = c;                       // always non-null
        if (c) c->Name(label, static_cast<uint16_t>(std::strlen(label)));
    };
    makeTracyCtx(0, "Graphics");
    makeTracyCtx(1, "Compute");
    // Tracy GPU zones piggyback on the timestamp profiler's per-pass region
    // indices, so enable it (also drives the editor GPU HUD in Profile builds).
    m_gpuProfiler.enabled = true;
#endif

}

namespace
{
    // Cookie for the D3D12 info-queue message callback (0 = not registered). File scope so the destructor
    // can unregister it in every config: the debug layer is a runtime opt-in, so a Release run can have
    // it live.
    DWORD g_d3d12MsgCallbackCookie = 0;
}

GraphicsDX12::~GraphicsDX12()
{
    // Wait for the GPU before releasing D3D12 objects. Gated on m_fence: fence waits use a per-thread
    // event created on demand.
    if (m_device && m_fence)
    {
        try { WaitForPreviousFrame(); } catch (...) {}
    }
    VideoMemory::Shutdown();
    // How much of the frame's transient constants the session took.
    if (m_transientConstants.IsValid())
    {
        const TransientConstantAllocator::Stats s = m_transientConstants.GetStats();
        LOG_INFO("[CBALLOC] summary: frames=%llu peakBytes=%u lastBytes=%u capacity=%u chunk=%u "
                 "allocations=%llu overflows=%llu",
                 static_cast<unsigned long long>(s.frames), s.peakTaken, s.lastTaken, s.capacity, s.chunk,
                 static_cast<unsigned long long>(s.allocations), static_cast<unsigned long long>(s.overflows));
        m_transientConstants.Shutdown();
    }

#ifdef TRACY_ENABLE
    // Destroy Tracy GPU contexts while the device + queues are still alive and
    // the GPU is idle (WaitForPreviousFrame above). Only valid contexts were
    // stored, so DestroyD3D12Context is safe.
    if (m_tracy)
    {
        for (auto*& c : m_tracy->ctx)
            if (c) { tracy::DestroyD3D12Context(c); c = nullptr; }
        m_tracy.reset();
    }
#endif


    // Streamline down while the device and swap chain are alive: it owns a proxy over the swap chain and
    // (with Frame Generation) its own present queue, which must unwind before what they wrap.
    // Frame Generation must go first: slShutdown with DLSS-G still enabled faults inside
    // sl.common.dll. ReleaseMainSurface does that for the surface (FG off, flush, slShutdown, then the
    // swap chain) and is a no-op when App already released it; Streamline itself can still be up, hence
    // the flush and the idempotent Shutdown after it. Wrapped because this destructor may run during
    // exception unwinding, where a throw terminates.
    ReleaseMainSurface();
    try
    {
        if (m_device) FlushAndWait();
    }
    catch (...) {}
    SL::StreamlineBackend::Get().Shutdown();

    // The surface OBJECT goes here, at a fixed point and not wherever its member
    // declaration happens to put it: the device, its queues, its descriptor
    // heaps and every pool are still alive, the GPU is idle, and what it still
    // holds (the present composition, the display-encode PSO, the HUD-less copy,
    // the frame limiter's timer) is released against them.
    m_surface.reset();

    // Release all resource pools + command-list pool *before* live-object reporting.
    // Otherwise ReportLiveDeviceObjects() will list objects that are only still
    // alive until member-destructors run at the end of this destructor body.

    // HDR render targets and their descriptors, every slot (only slot 0 is created by the engine; empty
    // slots free nothing). Each live slot is a texture pool record; its views are freed here, before the
    // shutdown reports below count them.
    for (uint32_t slot = 0; slot < kMaxHdrTargets; ++slot)
    {
        if (TextureDX12* rec = HdrRecord(slot))
        {
            rec->rtv.Free();
            rec->srv.Free();
            rec->uav.Free();
            rec->resource.Reset();
        }
        m_hdrTargets[slot].tex.Reset();
    }

    // Explicitly destroy command-list pool entries constructed via placement-new.
    // BlockAllocator itself does not call T destructors automatically.
    for (CommandListDX12* p : m_commandLists)
    {
        if (p) p->~CommandListDX12();
    }
    m_commandLists.fill(nullptr);
    m_commandListHighWater = 0;
    m_commandListCount = 0;
    m_cmdAllocator.blocks.clear();
    m_cmdAllocator.freeList.clear();

    // Pool high-water marks, for sizing kMax*Pool (slots never shrink: the free lists recycle
    // buffer/texture ids, shader/PSO ids retire in place). "samplers N/2048" is the GPU static sampler
    // region, which the 75%/90% allocator pressure warnings (CPU staging only) cannot speak for. The
    // dedupe hits are CreateSampler calls answered without a heap slot; hits + N is the pre-dedupe count
    // (kSamplerDedupe = false makes hits 0).
    LOG_INFO("GraphicsDX12: pool high-water - buffers %zu/%zu, textures %zu/%zu, "
             "shaders %zu/%zu, PSOs %zu, samplers %zu/%u, sampler dedupe hits %u",
             m_bufferPool.size(), m_bufferPool.capacity(),
             m_texturePool.size(), m_texturePool.capacity(),
             m_shaderPool.size(), m_shaderPool.capacity(),
             m_psoPool.size(), m_samplerPool.size(), kMaxSamplers,
             m_samplerDedupeHits);
    // The same numbers as `name=value`, so a pack can BOUND them (expect.logLineFields); the prose line
    // above cannot be parsed. `shaders` is the one pool an idle GraphSet teardown RETIRES (DestroyShader
    // leaves the slot spent: a bare index handle has no generation), so it grows with open/close cycles,
    // not with the scene. `psoCap` is a SOFT ceiling (kSoftMaxPsoPool: nothing refuses at it), printed
    // so a pack can bound `psos` and catch a per-teardown PSO leak. `destroyRefused` / `freeListDupes`
    // count the lifetime guards' refusals (DestroyBuffer / DestroyTexture, DrainReleasesUpTo): a clean
    // run has none.
    LOG_INFO("[POOL] shaders=%zu shaderCap=%zu psos=%zu psoCap=%zu buffers=%zu "
             "textures=%zu samplers=%zu destroyRefused=%u freeListDupes=%u",
             m_shaderPool.size(), m_shaderPool.capacity(), m_psoPool.size(),
             kSoftMaxPsoPool,
             m_bufferPool.size(), m_texturePool.size(), m_samplerPool.size(),
             m_destroyRefusals.load(), m_freeListDupes.load());
    // Which allocations raised the RTV / DSV high-water: the bound on rtvUsed/dsvUsed says a rise
    // happened, this says what it was. The shader-visible allocator records no tags (see NoteTag).
    m_rtvAllocator.ReportTagHistogram("rtv");
    m_dsvAllocator.ReportTagHistogram("dsv");
    // The static slots still handed out (the pools below are cleared, not freed) and the second
    // Frees each allocator refused.
    m_cbvSrvUavAllocator.ReportResidual("cbvSrvUav");
    m_rtvAllocator.ReportResidual("rtv");
    m_dsvAllocator.ReportResidual("dsv");
    m_samplerAllocator.ReportResidual("sampler");
    LogBindlessKinds("shutdown");
    FinishBindlessValidation();     // the GPU is idle: WaitForPreviousFrame above
    m_bindlessLazyNulls.clear();
    // Release CPU-side pools that hold ComPtr resources/pso/root signatures.
    m_bufferPool.clear();
    m_texturePool.clear();
    m_shaderPool.clear();
    m_psoPool.clear();
    m_layoutPool.clear();
    m_samplerPool.clear();
    // Must die WITH the pool it indexes: an entry that outlived m_samplerPool
    // would hand a later CreateSampler an index into an empty vector.
    m_samplerDedupe.clear();
    m_bufferFreeList.clear();
    m_textureFreeList.clear();

    // Release descriptor heap allocators (heaps are inside DescriptorHeapAllocator).
    m_rtvAllocator.Reset();
    m_cbvSrvUavAllocator.Reset();
    m_dsvAllocator.Reset();
    m_samplerAllocator.Reset();

    // Release upload-only staging objects and synchronization primitives.
    for (auto& ctx : m_uploadCtx)
    {
        ctx.list.Reset();
        ctx.allocator.Reset();
        ctx.fence.Reset();
        ctx.copyList.Reset();
        ctx.copyAllocator.Reset();
        ctx.copyFence.Reset();
        ctx.stagingKeepAlive.clear();
        ctx.currentBufferHeap.Reset();
    }
    // The stream rings and any staging still held (the GPU is idle here).
    ShutdownStreamUploader();
    m_fence.Reset();
    for (auto& f : m_queueFences) f.Reset();

    // ---- What the PSO library did this run ----
    // One line, at the only point where every PSO the run needed has been built. It is the signal of PSO
    // NAMING DRIFT (rename what ComputePSOName hashes and every warm start misses every pipeline: seconds
    // of ISA compilation per launch), since the debug layer's per-miss LOADPIPELINE_NAMENOTFOUND is
    // downgraded to Info.
    // Gate the RELATION, not the counts (hits depend on the machine's cache file): a pack forces the cold
    // case (-pso_cache_dir into a per-run directory), where loadedBytes is 0 and misses == compiled;
    // drift is a WARM run (loadedBytes > 0) that still reads misses == compiled.
    // hits == diskHits + sessionDupes; packs read `hits`, so keep its name and meaning. StorePipeline
    // fills the library as the run compiles, so a repeated psoName is a hit with nothing loaded
    // (loadedBytes, not hits, says how warm the cache was). The resolved-name set is filled on the load
    // path too, so a name asked for again on a warm run is a dupe as well: a pack that does not force the
    // cold case can gate sessionDupes >= 1.
    // nameWaits: how often a thread waited for another one creating the same psoName (m_psoLibInflight).
    // computeHits / computeMisses / computeCompiled: the compute pipelines' share, kept out of the fields
    // before them.
    if (m_psoLibHits.load() || m_psoLibMisses.load() || m_psoLibCompiled.load() || m_psoLibBytes ||
        m_psoLibComputeHits.load() || m_psoLibComputeMisses.load())
        LOG_INFO("[PSO] library hits=%u diskHits=%u sessionDupes=%u misses=%u "
                 "compiled=%u loadedBytes=%llu nameWaits=%u meshHits=%u meshMisses=%u meshCompiled=%u "
                 "computeHits=%u computeMisses=%u computeCompiled=%u",
                 m_psoLibHits.load(), m_psoLibDiskHits, m_psoLibSessionDupes,
                 m_psoLibMisses.load(), m_psoLibCompiled.load(),
                 static_cast<unsigned long long>(m_psoLibBytes),
                 m_psoLibNameWaits.load(), m_psoLibMeshHits.load(), m_psoLibMeshMisses.load(),
                 m_psoLibMeshCompiled.load(), m_psoLibComputeHits.load(), m_psoLibComputeMisses.load(),
                 m_psoLibComputeCompiled.load());

    // Pipeline library bytes & ComPtr.
    m_psoLibraryData.clear();
    m_psoLibrary.Reset();

    // Enforce the DXGI release order here, so member destruction order does not matter:
    //   render targets -> swap chain (after SetFullscreenState) -> queues
    // The first two went with the surface (ReleaseMainSurface, above); the queues follow here. This
    // avoids DXGI's "process terminating" live-object warning.
    for (auto& q : m_queues) q.Reset();

#ifdef _DEBUG
    // Report live objects while the message callback is still registered so output reaches the log.
    // IGNORE_INTERNAL filters debug-layer bookkeeping objects, leaving application-level leaks.
    if (m_device)
    {
        ComPtr<ID3D12DebugDevice1> debugDevice1;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice1))) && debugDevice1)
        {
            debugDevice1->ReportLiveDeviceObjects(
                D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
        else
        {
            ComPtr<ID3D12DebugDevice> debugDevice;
            if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&debugDevice))) && debugDevice)
                debugDevice->ReportLiveDeviceObjects(
                    D3D12_RLDO_SUMMARY | D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
        }
    }

#endif

    // Unregister in every config: the debug layer is a runtime opt-in (DX12_DEBUGLAYER=1), so a Release
    // run can have this callback live. It formats into the Logger, and a message arriving during device
    // teardown after the Logger shut down would be a use-after-free.
    if (g_d3d12MsgCallbackCookie != 0 && m_device)
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
            infoQueue->UnregisterMessageCallback(g_d3d12MsgCallbackCookie);
        g_d3d12MsgCallbackCookie = 0;
    }
    // Member ComPtr destructors release the rest (device, factory, heaps, pools).
}


// ==== Pool helpers ====

// Returned by GetPoolEntry when the pool is empty and there is no slot 0 to fall back to. Never
// recorded into: its per-queue command lists stay null, so GetCommandList() yields nullptr and the
// null-guarded call sites bail out.
static CommandListDX12 s_invalidCommandList;

const CommandListDX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd) const
{
    // A handle must name a slot opened THIS frame: below the live count (reset
    // by every BeginFrame) and constructed (the array is the fixed capacity, so
    // an unconstructed slot is a null pointer, not an out-of-range index).
    // Anything else - the invalid handle, a handle kept from an earlier frame
    // (its list is closed) - falls back to slot 0, the primary CL, live for the
    // whole frame, so a bad handle degrades to misdirected recording instead of
    // memory corruption. Reported once per session for both overloads (the
    // non-const one forwards here).
    const uint32_t live = (std::min)(m_commandListCount.load(std::memory_order_relaxed),
                                     kMaxCommandLists);
    if (!ENGINE_CHECK_ONCE(cmd.internalId < live && m_commandLists[cmd.internalId] != nullptr,
            "GetPoolEntry: CommandList id %u is not open this frame (%u in use) - "
            "falling back to slot 0", cmd.internalId, live))
    {
        if (!m_commandLists[0]) return s_invalidCommandList;
        return *m_commandLists[0];
    }
    return *m_commandLists[cmd.internalId];
}

CommandListDX12& GraphicsDX12::GetPoolEntry(RHI::CommandList cmd)
{
    return const_cast<CommandListDX12&>(
        static_cast<const GraphicsDX12&>(*this).GetPoolEntry(cmd));
}

void GraphicsDX12::AddCommandListDependency(RHI::CommandList waiter,
                                             RHI::CommandList dependency)
{
    // Checked in every config: an out-of-range id would index past the pool, and a self-edge deadlocks
    // EndFrame's topological sort.
    const uint32_t count = static_cast<uint32_t>(m_commandListCount.load(std::memory_order_relaxed));
    if (!ENGINE_CHECK_ONCE(waiter.internalId < count,
            "AddCommandListDependency: waiter handle %u invalid (count %u)",
            waiter.internalId, count))
        return;
    if (!ENGINE_CHECK_ONCE(dependency.internalId < count,
            "AddCommandListDependency: dependency handle %u invalid (count %u)",
            dependency.internalId, count))
        return;
    if (!ENGINE_CHECK_ONCE(waiter.internalId != dependency.internalId,
            "AddCommandListDependency: self-dependency on command list %u",
            waiter.internalId))
        return;
    m_commandLists[waiter.internalId]->waitFor.push_back(dependency.internalId);
}

void GraphicsDX12::SetSubmitBarrier(RHI::CommandList cmd)
{
    if (cmd.internalId >= m_commandListCount) return;
    m_commandLists[cmd.internalId]->submitBarrier = true;
}

// ==== DX12-specific accessors ====

// The recording owner of the command lists this thread begins (IGraphicsDevice::SetRecordingOwner).
static thread_local uintptr_t t_recordingOwner = 0;

ID3D12GraphicsCommandList* GraphicsDX12::GetNativeCommandList(RHI::CommandList cmd) const
{
    const CommandListDX12& entry = GetPoolEntry(cmd);
    // A recorder holding the native list can record what the device does not count: this list's
    // counts may be short.
    if (entry.IsValid()) ++entry.counters.nativeTakes;
    return entry.GetCommandList();
}
ID3D12Device*              GraphicsDX12::GetDevice()            const { return m_device.Get(); }

ID3D12Device5* GraphicsDX12::GetDevice5() const
{
    if (!m_dxrSupported) return nullptr;
    // QI on demand: one COM ref bump, and DDGI calls this once at Compile() to cache the pointer.
    ID3D12Device5* dev5 = nullptr;
    if (FAILED(m_device.Get()->QueryInterface(IID_PPV_ARGS(&dev5))))
        return nullptr;
    dev5->Release(); // caller does not own ref; pointer lifetime tied to m_device
    return dev5;
}
DescriptorHeapAllocator&   GraphicsDX12::GetRtvAllocator()            { return m_rtvAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetCbvSrvUavAllocator()      { return m_cbvSrvUavAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetSamplerAllocator()        { return m_samplerAllocator; }
DescriptorHeapAllocator&   GraphicsDX12::GetDsvAllocator()             { return m_dsvAllocator; }

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureDsvCpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid() || tex.handleId >= static_cast<uint32_t>(m_texturePool.size()))
        return D3D12_CPU_DESCRIPTOR_HANDLE{};
    return m_texturePool[tex.handleId].dsv.GetCpuHandle();
}

ID3D12Resource* GraphicsDX12::GetBufferResource(const RHI::GPUBuffer& buffer) const
{
    if (!buffer.IsValid() || buffer.handleId >= m_bufferPool.size()) return nullptr;
    return m_bufferPool[buffer.handleId].resource.Get();
}

uint64_t GraphicsDX12::CreateDepthTextureSRVTable(const RHI::Texture* textures,
                                                   uint32_t            count)
{
    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(count);
    if (!alloc.IsValid())
    {
        LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: descriptor allocation failed");
        return 0;
    }

    const UINT descSize = m_cbvSrvUavAllocator.GetDescriptorSize();

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!textures[i].IsValid()) continue;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                    = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels       = 1;
        srvDesc.Texture2D.MostDetailedMip = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuSlot = alloc.GetCpuHandle();
        cpuSlot.ptr += static_cast<SIZE_T>(i) * descSize;

        ID3D12Resource* resource = GetTextureResource(textures[i]);
        if (resource)
            m_cbvSrvUavAllocator.WriteSrv(resource, &srvDesc, cpuSlot);
        else
            LOG_ERROR("GraphicsDX12::CreateDepthTextureSRVTable: no resource for slot %u", i);
    }

    alloc.CopyToGpu();
    const uint64_t gpuHandle = alloc.GetGpuHandle().ptr;
    m_descriptorTablePool[gpuHandle] = std::move(alloc);
    LOG_INFO("GraphicsDX12::CreateDepthTextureSRVTable: handle = %llu (%u entries)", gpuHandle, count);
    return gpuHandle;
}

void GraphicsDX12::FreeDescriptorTable(uint64_t gpuHandle)
{
    if (!gpuHandle) return;
    auto it = m_descriptorTablePool.find(gpuHandle);
    if (it != m_descriptorTablePool.end())
    {
        // Deferred: in-flight draws may still reference this table.
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        PendingReleaseBucket().descriptors.push_back(std::move(it->second));
        m_descriptorTablePool.erase(it);
    }
}

ID3D12Resource* GraphicsDX12::GetTextureResource(const RHI::Texture& texture) const
{
    if (!texture.IsValid() || texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return nullptr;
    return m_texturePool[texture.handleId].resource.Get();
}

// ==== Descriptor-table fill helpers (multi-volume DDGI) ====
void GraphicsDX12::CreateStructuredBufferSRVAtCpu(const RHI::GPUBuffer& buf,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetBufferResource(buf);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN; // structured
    const uint32_t stride           = buf.desc.stride > 0 ? buf.desc.stride : 4;
    srvd.Buffer.NumElements         = static_cast<UINT>(buf.desc.size / stride);
    srvd.Buffer.StructureByteStride = stride;
    m_cbvSrvUavAllocator.WriteSrv(res, &srvd, dst);
}

void GraphicsDX12::CreateTexture2DSRVAtCpu(const RHI::Texture& tex,
                                           D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    ID3D12Resource* res = GetTextureResource(tex);
    if (!res) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = ToDxgiFormat(tex.desc.format);
    srvd.Texture2D.MipLevels        = tex.desc.mipLevels > 0 ? tex.desc.mipLevels : 1;
    srvd.Texture2D.MostDetailedMip  = 0;
    m_cbvSrvUavAllocator.WriteSrv(res, &srvd, dst);
}

void GraphicsDX12::CreateNullStructuredBufferSRVAtCpu(uint32_t stride,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = DXGI_FORMAT_UNKNOWN;
    srvd.Buffer.NumElements         = 1;
    srvd.Buffer.StructureByteStride = stride > 0 ? stride : 4;
    m_cbvSrvUavAllocator.WriteSrv(nullptr, &srvd, dst);
}

void GraphicsDX12::CreateNullTexture2DSRVAtCpu(DXGI_FORMAT format,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.ViewDimension              = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Format                     = format;
    srvd.Texture2D.MipLevels        = 1;
    m_cbvSrvUavAllocator.WriteSrv(nullptr, &srvd, dst);
}

// Null descriptors. "Absent" is the null of the field's kind: it reads zeros and, unlike a
// 0 / ~0u / -1 sentinel, never aliases a live resource. The common kinds have a FIXED slot right
// after the bindless table, at the BINDLESS_NULL_* index shaders/shared/bindless_kind.hlsli names
// (kFixedBindlessNulls: one table for creating them and finding them); any other kind - a structured
// buffer, whose null carries its stride - gets one made on first use (BindlessNull).
namespace
{
    struct FixedBindlessNull { uint32_t slot; uint8_t kind; };
    constexpr FixedBindlessNull kFixedBindlessNulls[] = {
        { BINDLESS_NULL_TEX2D_FLOAT,        RHI::Bindless::Kind::Tex2DFloat },
        { BINDLESS_NULL_TEX2D_UINT,         RHI::Bindless::Kind::Tex2DUint },
        { BINDLESS_NULL_TEX2D_SINT,         RHI::Bindless::Kind::Tex2DSint },
        { BINDLESS_NULL_TEX2DARRAY_FLOAT,   RHI::Bindless::Kind::Tex2DArrayFloat },
        { BINDLESS_NULL_TEXCUBE_FLOAT,      RHI::Bindless::Kind::TexCubeFloat },
        { BINDLESS_NULL_TEXCUBEARRAY_FLOAT, RHI::Bindless::Kind::TexCubeArrayFloat },
        { BINDLESS_NULL_TEX3D_FLOAT,        RHI::Bindless::Kind::Tex3DFloat },
        { BINDLESS_NULL_RAWBUFFER,          RHI::Bindless::Kind::RawBuffer },
        { BINDLESS_NULL_CBV,                RHI::Bindless::Kind::Cbv },
        { BINDLESS_NULL_RWTEX2D_FLOAT,      RHI::Bindless::Kind::RWTex2DFloat },
        { BINDLESS_NULL_RWTEX2D_UINT,       RHI::Bindless::Kind::RWTex2DUint },
        { BINDLESS_NULL_RWRAWBUFFER,        RHI::Bindless::Kind::RWRawBuffer },
        { BINDLESS_NULL_STRUCTURED,         RHI::Bindless::Kind::StructuredBuffer },   // stride 4
    };
    static_assert(sizeof(kFixedBindlessNulls) / sizeof(kFixedBindlessNulls[0]) == BINDLESS_NULL_COUNT,
                  "one kFixedBindlessNulls row per BINDLESS_NULL_* slot");

    // The format a null view of a class is written with (float textures: RGBA8 UNORM for an SRV,
    // RGBA16F for a UAV).
    DXGI_FORMAT NullViewFormat(uint32_t cls, bool uav, bool buffer)
    {
        if (cls == BK_CLASS_UINT) return DXGI_FORMAT_R32_UINT;
        if (cls == BK_CLASS_SINT) return DXGI_FORMAT_R32_SINT;
        if (buffer)               return DXGI_FORMAT_R32_FLOAT;
        return uav ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

bool GraphicsDX12::WriteNullDescriptor(uint8_t kind, uint32_t stride,
                                       D3D12_CPU_DESCRIPTOR_HANDLE dst) const
{
    using namespace RHI::Bindless;
    const uint32_t shape = ShapeOf(kind);
    const uint32_t cls   = ClassOf(kind);
    const UINT     sbs   = stride ? stride : 4u;
    if (shape == BK_CBV)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC c{};   // BufferLocation 0: reads zeros
        m_cbvSrvUavAllocator.WriteCbv(&c, dst);
        return true;
    }
    if (IsUav(kind))
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = NullViewFormat(cls, true, false);
        switch (shape)
        {
        case BK_TEX1D:      d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D; break;
        case BK_TEX1DARRAY: d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
                            d.Texture1DArray.ArraySize = 1; break;
        case BK_TEX2D:      d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; break;
        case BK_TEX2DARRAY: d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                            d.Texture2DArray.ArraySize = 1; break;
        case BK_TEX3D:      d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                            d.Texture3D.WSize = 1; break;
        case BK_TYPEDBUFFER:
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Format = NullViewFormat(cls, true, true);
            d.Buffer.NumElements = 1;
            break;
        case BK_STRUCTUREDBUFFER:
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.Buffer.NumElements = 1;
            d.Buffer.StructureByteStride = sbs;
            break;
        case BK_RAWBUFFER:
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Format = DXGI_FORMAT_R32_TYPELESS;
            d.Buffer.NumElements = 1;
            d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            break;
        default:
            return false;
        }
        m_cbvSrvUavAllocator.WriteUav(nullptr, nullptr, &d, dst);
        return true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format                  = NullViewFormat(cls, false, false);
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    switch (shape)
    {
    case BK_TEX1D:        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                          d.Texture1D.MipLevels = 1; break;
    case BK_TEX1DARRAY:   d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
                          d.Texture1DArray.MipLevels = 1; d.Texture1DArray.ArraySize = 1; break;
    case BK_TEX2D:        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                          d.Texture2D.MipLevels = 1; break;
    case BK_TEX2DARRAY:   d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                          d.Texture2DArray.MipLevels = 1; d.Texture2DArray.ArraySize = 1; break;
    case BK_TEX2DMS:      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; break;
    case BK_TEX2DMSARRAY: d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
                          d.Texture2DMSArray.ArraySize = 1; break;
    case BK_TEX3D:        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                          d.Texture3D.MipLevels = 1; break;
    case BK_TEXCUBE:      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                          d.TextureCube.MipLevels = 1; break;
    case BK_TEXCUBEARRAY: d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                          d.TextureCubeArray.MipLevels = 1; d.TextureCubeArray.NumCubes = 1; break;
    case BK_TYPEDBUFFER:
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Format = NullViewFormat(cls, false, true);
        d.Buffer.NumElements = 1;
        break;
    case BK_STRUCTUREDBUFFER:
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.Buffer.NumElements = 1;
        d.Buffer.StructureByteStride = sbs;
        break;
    case BK_RAWBUFFER:
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.Buffer.NumElements = 1;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        break;
    default:
        return false;   // Free; an acceleration structure has no null here
    }
    m_cbvSrvUavAllocator.WriteSrv(nullptr, &d, dst);
    return true;
}

void GraphicsDX12::CreateBindlessNulls()
{
    m_bindlessNulls = m_cbvSrvUavAllocator.Allocate(BINDLESS_NULL_COUNT);
    const uint32_t base = m_bindlessNulls.IsValid()
        ? DescriptorHeapIndexFromGpuHandle(m_bindlessNulls.GetGpuHandle().ptr) : ~0u;
    if (!ENGINE_CHECK(base == BINDLESS_NULL_BASE,
                      "bindless null descriptors start at heap %u, bindless_kind.hlsli expects %u",
                      base, BINDLESS_NULL_BASE))
        return;

    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (const FixedBindlessNull& n : kFixedBindlessNulls)
    {
        if (!ENGINE_CHECK(n.slot >= BINDLESS_NULL_BASE && n.slot < BINDLESS_NULL_BASE + BINDLESS_NULL_COUNT,
                          "fixed bindless null slot %u outside %u..%u", n.slot, BINDLESS_NULL_BASE,
                          BINDLESS_NULL_BASE + BINDLESS_NULL_COUNT - 1))
            continue;
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_bindlessNulls.GetGpuCpuHandle();
        h.ptr += static_cast<SIZE_T>(n.slot - BINDLESS_NULL_BASE) * inc;
        WriteNullDescriptor(n.kind, 0, h);
        // The allocator derived the kind from what was written; it must be
        // the null of the row's kind. A cheap check of the whole recording
        // path (the view -> kind mapping, the handle -> slot lookup) at boot.
        const uint8_t recorded = m_cbvSrvUavAllocator.RecordAtGpu(n.slot).kind;
        ENGINE_CHECK(recorded == static_cast<uint8_t>(n.kind | BK_NULL),
                     "bindless null at heap %u is recorded as kind 0x%02X, expected 0x%02X",
                     n.slot, recorded, static_cast<unsigned>(n.kind | BK_NULL));
        // ... and the checked path accepts it as its kind, and an absent
        // field of that kind gets it.
        ENGINE_CHECK(MakeBindlessHandle(n.slot, n.kind, 0, "boot self-check") == n.slot &&
                     MakeBindlessHandle(~0u, n.kind, 0, nullptr) == n.slot,
                     "bindless null at heap %u is not what MakeBindlessHandle returns for "
                     "kind 0x%02X", n.slot, static_cast<unsigned>(n.kind));
    }
    LOG_INFO("[BINDLESS] table at heap 0..%u; %u null descriptors at %u..%u",
             BINDLESS_TABLE_SIZE - 1, BINDLESS_NULL_COUNT, BINDLESS_NULL_BASE,
             BINDLESS_NULL_BASE + BINDLESS_NULL_COUNT - 1);

    // The debug targets' fixed slots, right after the nulls. Until DebugTargetViewPass makes them, the
    // counters hold the null RWTexture2D<uint> and the cost table the structured null.
    m_bindlessDebugSlots = m_cbvSrvUavAllocator.Allocate(BINDLESS_DEBUG_COUNT);
    const uint32_t debugBase = m_bindlessDebugSlots.IsValid()
        ? DescriptorHeapIndexFromGpuHandle(m_bindlessDebugSlots.GetGpuHandle().ptr) : ~0u;
    if (!ENGINE_CHECK(debugBase == BINDLESS_DEBUG_BASE,
                      "the debug target counters start at heap %u, bindless_kind.hlsli expects %u",
                      debugBase, BINDLESS_DEBUG_BASE))
        return;
    m_bindlessDebugBase = debugBase;
    for (uint32_t s = BINDLESS_DEBUG_BASE; s < BINDLESS_DEBUG_BASE + BINDLESS_DEBUG_COUNT; ++s)
    {
        if (s == BINDLESS_DEBUG_COST_TABLE) SetFixedDebugBuffer(s, nullptr);
        else                                SetFixedDebugUav(s, nullptr);
    }
    LOG_INFO("[BINDLESS] %u debug target slots at %u..%u (the counters, the material cost table at %u)",
             BINDLESS_DEBUG_COUNT, BINDLESS_DEBUG_BASE, BINDLESS_DEBUG_BASE + BINDLESS_DEBUG_COUNT - 1,
             BINDLESS_DEBUG_COST_TABLE);
}

bool GraphicsDX12::SetFixedDebugUav(uint32_t slot, const RHI::Texture* texture)
{
    if (m_bindlessDebugBase != BINDLESS_DEBUG_BASE || slot < BINDLESS_DEBUG_BASE ||
        slot >= BINDLESS_DEBUG_BASE + BINDLESS_DEBUG_COUNT || slot == BINDLESS_DEBUG_COST_TABLE)
        return false;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessDebugSlots.GetGpuCpuHandle();
    dst.ptr += static_cast<SIZE_T>(slot - BINDLESS_DEBUG_BASE) * inc;
    if (!texture || !texture->IsValid())
        return WriteNullDescriptor(RHI::Bindless::Kind::RWTex2DUint, 0, dst);
    const TextureDX12& t = m_texturePool[texture->handleId];
    if (!t.uav.IsValid()) return false;
    // From the texture's CPU staging copy: the allocator carries the kind record.
    m_cbvSrvUavAllocator.CopyDescriptors(1, dst, t.uav.GetCpuHandle());
    return true;
}

bool GraphicsDX12::SetFixedDebugBuffer(uint32_t slot, const RHI::GPUBuffer* buffer)
{
    if (m_bindlessDebugBase != BINDLESS_DEBUG_BASE || slot != BINDLESS_DEBUG_COST_TABLE)
        return false;
    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = m_bindlessDebugSlots.GetGpuCpuHandle();
    dst.ptr += static_cast<SIZE_T>(slot - BINDLESS_DEBUG_BASE) * inc;
    if (!buffer || !buffer->IsValid())
        return WriteNullDescriptor(RHI::Bindless::Kind::StructuredBuffer, 4u, dst);
    const GPUBufferDX12& b = m_bufferPool[buffer->handleId];
    if (!b.srv.IsValid()) return false;
    m_cbvSrvUavAllocator.CopyDescriptors(1, dst, b.srv.GetCpuHandle());   // the kind record comes along
    return true;
}

uint32_t GraphicsDX12::BindlessNullIndex(uint8_t kind) const
{
    for (const FixedBindlessNull& n : kFixedBindlessNulls)
        if (RHI::Bindless::SameType(n.kind, kind))
            return n.slot;
    return ~0u;
}

uint32_t GraphicsDX12::BindlessNull(uint8_t kind, uint32_t stride)
{
    using namespace RHI::Bindless;
    const uint8_t type = static_cast<uint8_t>(kind & ~BK_NULL);
    // Only a structured buffer's null depends on more than the kind; the fixed
    // one (BINDLESS_NULL_STRUCTURED) has stride 4.
    if (ShapeOf(type) != BK_STRUCTUREDBUFFER) stride = 0;
    else if (stride == 0)                     stride = 4;
    const uint32_t fixed = BindlessNullIndex(type);
    if (fixed != ~0u && (stride == 0 || stride == 4))
        return fixed;
    const uint32_t key = (stride << 8) | type;
    std::lock_guard<std::mutex> lock(m_bindlessNullMutex);
    if (auto it = m_bindlessLazyNulls.find(key); it != m_bindlessLazyNulls.end())
        return it->second.GetGpuIndex();

    DescriptorAllocation a = m_cbvSrvUavAllocator.Allocate(1);
    if (!a.IsValid())
        return ~0u;
    if (!WriteNullDescriptor(type, stride, a.GetCpuHandle()))
    {
        a.Free();
        return ~0u;
    }
    a.CopyToGpu();
    const uint32_t index = a.GetGpuIndex();
    m_bindlessLazyNulls.emplace(key, a);
    char name[48];
    Describe(type, name, sizeof(name));
    LOG_INFO("[BINDLESS] made the null %s%s at heap %u", name,
             stride ? (" stride " + std::to_string(stride)).c_str() : "", index);
    return index;
}

void GraphicsDX12::ReportBindlessMismatch(uint32_t heapIndex, uint8_t heldKind,
                                          uint32_t heldStride, uint8_t wantKind,
                                          uint32_t wantStride, const char* what)
{
    m_bindlessMismatches.fetch_add(1, std::memory_order_relaxed);
    const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(what)) * 0x9E3779B97F4A7C15ull)
                       ^ (static_cast<uint64_t>(heapIndex) << 8) ^ wantKind;
    {
        std::lock_guard<std::mutex> lock(m_bindlessReportMutex);
        if (!m_bindlessReported.insert(key).second)
            return;
    }
    char held[48], want[48];
    RHI::Bindless::Describe(heldKind, held, sizeof(held));
    RHI::Bindless::Describe(wantKind, want, sizeof(want));
    ::EngineCheck::ReportFailure(__FILE__, __LINE__, "the slot holds the kind asked for",
        "[BINDLESS] %s: heap %u holds %s (stride %u), asked for %s (stride %u) - "
        "using the null of the kind asked for",
        what ? what : "(unnamed)", heapIndex, held, heldStride, want, wantStride);
}

uint32_t GraphicsDX12::MakeBindlessHandle(uint32_t heapIndex, uint8_t kind,
                                          uint32_t stride, const char* what)
{
    using namespace RHI::Bindless;
    const uint8_t type = static_cast<uint8_t>(kind & ~BK_NULL);
    if (heapIndex != ~0u)
    {
        const DescriptorHeapAllocator::SlotRecord rec = m_cbvSrvUavAllocator.RecordAtGpu(heapIndex);
        const bool kindOk   = ShapeOf(type) != BK_FREE && SameType(rec.kind, type);
        const bool strideOk = stride == 0 || ShapeOf(type) != BK_STRUCTUREDBUFFER
                           || rec.stride == stride;
        if (kindOk && strideOk)
            return MakeHandle(heapIndex, rec.gen);
        ReportBindlessMismatch(heapIndex, rec.kind, rec.stride, type, stride, what);
    }
    const uint32_t nullIndex = BindlessNull(type, stride);
    if (!ENGINE_CHECK_ONCE(nullIndex != ~0u,
            "[BINDLESS] %s: no null descriptor can be made for kind 0x%02X",
            what ? what : "(unnamed)", static_cast<unsigned>(type)))
        return ~0u;
    return MakeHandle(nullIndex, m_cbvSrvUavAllocator.RecordAtGpu(nullIndex).gen);
}

void GraphicsDX12::LogBindlessKinds(const char* when) const
{
    if (!m_cbvSrvUavAllocator.TracksKinds())
        return;
    UINT hist[256];
    m_cbvSrvUavAllocator.KindHistogram(hist);
    struct Row { UINT count; uint8_t kind; };
    std::vector<Row> rows;
    UINT live = 0;
    for (uint32_t k = 1; k < 256; ++k)
        if (hist[k]) { rows.push_back({ hist[k], static_cast<uint8_t>(k) }); live += hist[k]; }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
    std::string list;
    for (const Row& r : rows)
    {
        char name[48];
        RHI::Bindless::Describe(r.kind, name, sizeof(name));
        char item[80];
        std::snprintf(item, sizeof(item), "%s%s %u", list.empty() ? "" : ", ", name, r.count);
        if (list.size() + std::strlen(item) > 700) { list += ", ..."; break; }
        list += item;
    }
    // name=value first, so a pack can bound them (expect.logLineFields).
    LOG_INFO("[BINDLESS] kinds at %s: live=%u freed=%u mismatches=%u - %s",
             when, live, hist[BK_FREE], m_bindlessMismatches.load(std::memory_order_relaxed),
             list.c_str());
}

// The bindless debug lane (-bindless_validate). Every shader compiles with BINDLESS_VALIDATE=1
// (DxcCompiler), so each bindless.hlsli accessor looks the slot it opens up in the record - mirrored
// here, one uint per GPU slot - and appends a refused read to the log instead of performing it.
// Nothing is ever cleared: each EndFrame copies the log into its frame slot's readback slice and reads
// what the slot's previous frame copied there, so a refusal surfaces as a [BINDLESS] ERROR about
// kFrameCount frames late, and the shutdown line carries the total.
void GraphicsDX12::CreateBindlessValidation()
{
    if (!DxcCompiler::BindlessValidate())
        return;
    m_bindlessValidateSlots = m_cbvSrvUavAllocator.Allocate(2);
    const uint32_t base = m_bindlessValidateSlots.IsValid()
        ? DescriptorHeapIndexFromGpuHandle(m_bindlessValidateSlots.GetGpuHandle().ptr) : ~0u;
    if (!ENGINE_CHECK(base == BINDLESS_VALIDATE_TABLE,
                      "-bindless_validate: the lane's descriptors start at heap %u, "
                      "bindless_kind.hlsli expects %u - the lane stays off", base,
                      BINDLESS_VALIDATE_TABLE))
        return;

    const UINT entries = m_cbvSrvUavAllocator.GetCapacity();
    D3D12_HEAP_PROPERTIES upload{ D3D12_HEAP_TYPE_UPLOAD };
    const auto tableDesc = CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(entries) * 4u);
    ThrowIfFailed(m_device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &tableDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_bindlessTable)));
    GpuLedger::TrackCommitted(m_device.Get(), m_bindlessTable.Get(), Diag::Prof::GpuMemCat::Upload,
                              "Bindless.ValidateTable");
    m_bindlessTable->SetName(L"GraphicsDX12.BindlessValidateTable");

    // DEFAULT and committed, so it starts zeroed; COMMON, so every queue's
    // shaders and EndFrame's copy promote it implicitly - nothing ever
    // transitions it.
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT };
    const auto logDesc = CD3DX12_RESOURCE_DESC::Buffer(BINDLESS_VALIDATE_LOG_BYTES,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ThrowIfFailed(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &logDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_bindlessLog)));
    GpuLedger::TrackCommitted(m_device.Get(), m_bindlessLog.Get(), Diag::Prof::GpuMemCat::Buffer, "Bindless.ValidateLog");
    m_bindlessLog->SetName(L"GraphicsDX12.BindlessValidateLog");

    D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
    const auto rbDesc = CD3DX12_RESOURCE_DESC::Buffer(
        static_cast<UINT64>(kFrameCount) * BINDLESS_VALIDATE_LOG_BYTES);
    ThrowIfFailed(m_device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &rbDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_bindlessLogReadback)));
    GpuLedger::TrackCommitted(m_device.Get(), m_bindlessLogReadback.Get(), Diag::Prof::GpuMemCat::Readback,
                              "Bindless.ValidateLogReadback");
    m_bindlessLogReadback->SetName(L"GraphicsDX12.BindlessValidateReadback");

    const UINT inc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_bindlessValidateSlots.GetGpuCpuHandle();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    srv.Format                  = DXGI_FORMAT_R32_TYPELESS;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements      = entries;
    srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    m_cbvSrvUavAllocator.WriteSrv(m_bindlessTable.Get(), &srv, h);
    h.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension      = D3D12_UAV_DIMENSION_BUFFER;
    uav.Format             = DXGI_FORMAT_R32_TYPELESS;
    uav.Buffer.NumElements = BINDLESS_VALIDATE_LOG_BYTES / 4u;
    uav.Buffer.Flags       = D3D12_BUFFER_UAV_FLAG_RAW;
    m_cbvSrvUavAllocator.WriteUav(m_bindlessLog.Get(), nullptr, &uav, h);

    void* mapped = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    ThrowIfFailed(m_bindlessTable->Map(0, &noRead, &mapped));
    m_cbvSrvUavAllocator.AttachGpuMirror(static_cast<uint32_t*>(mapped), entries);
    m_bindlessValidate = true;
    LOG_INFO("[BINDLESS] validate on: the record mirrored at heap %u (%u slots, %u KB), "
             "refused reads logged at heap %u", BINDLESS_VALIDATE_TABLE, entries,
             entries * 4u / 1024u, BINDLESS_VALIDATE_LOG);
}

namespace
{
    // One log record { handle, kind asked for, the slot's kind | gen << 8, 0 }.
    std::string DescribeBindlessRecord(const uint32_t* rec)
    {
        using namespace RHI::Bindless;
        char asked[48], held[48], line[200];
        Describe(static_cast<uint8_t>(rec[1]), asked, sizeof(asked));
        Describe(static_cast<uint8_t>(rec[2] & 0xFFu), held, sizeof(held));
        std::snprintf(line, sizeof(line),
                      "heap %u (handle gen %u) read as %s; the slot holds %s, gen %u",
                      IndexOf(rec[0]), GenOf(rec[0]), asked, held,
                      (rec[2] >> 8) & BINDLESS_GEN_MASK);
        return line;
    }
}

void GraphicsDX12::ReadBindlessLog(const uint32_t* log, bool final)
{
    const uint32_t refused = log[0];
    if (refused > m_bindlessRefusedSeen)
    {
        if (m_bindlessRefusedSeen == 0)
            LOG_ERROR("[BINDLESS] GPU: a shader read a descriptor as what it is not - "
                      "first: %s. It read the null of the kind it asked for instead "
                      "(-bindless_validate; the shutdown line counts them all)",
                      DescribeBindlessRecord(log + 4).c_str());
        m_bindlessRefusedSeen = refused;
    }
    if (!final)
        return;

    // The distinct (handle, kind) pairs among the records the log kept.
    std::vector<const uint32_t*> distinct;
    const uint32_t kept = (std::min)(refused, BINDLESS_VALIDATE_RECORDS);
    for (uint32_t r = 0; r < kept; ++r)
    {
        const uint32_t* rec = log + 4 + r * 4;
        bool seen = false;
        for (const uint32_t* d : distinct)
            seen = seen || (d[0] == rec[0] && d[1] == rec[1]);
        if (!seen) distinct.push_back(rec);
    }
    // name=value, so a pack can bound it (expect.logLineFields).
    LOG_INFO("[BINDLESS] validate: gpuRefused=%u distinct=%zu (the first %u reads kept)",
             refused, distinct.size(), BINDLESS_VALIDATE_RECORDS);
    for (size_t i = 0; i < distinct.size() && i < 8; ++i)
        LOG_INFO("[BINDLESS]   %s", DescribeBindlessRecord(distinct[i]).c_str());
}

void GraphicsDX12::PollBindlessValidation(ID3D12GraphicsCommandList* cmd)
{
    if (!m_bindlessValidate)
        return;
    // This frame slot's previous frame has retired (BeginFrame waited for
    // it), so the copy it recorded has landed.
    const UINT64 off = static_cast<UINT64>(GetFrameIndex()) * BINDLESS_VALIDATE_LOG_BYTES;
    const D3D12_RANGE read{ static_cast<SIZE_T>(off), static_cast<SIZE_T>(off + BINDLESS_VALIDATE_LOG_BYTES) };
    void* p = nullptr;
    if (SUCCEEDED(m_bindlessLogReadback->Map(0, &read, &p)))
    {
        ReadBindlessLog(reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + off), false);
        const D3D12_RANGE noWrite{ 0, 0 };
        m_bindlessLogReadback->Unmap(0, &noWrite);
    }
    cmd->CopyBufferRegion(m_bindlessLogReadback.Get(), off, m_bindlessLog.Get(), 0,
                          BINDLESS_VALIDATE_LOG_BYTES);
}

void GraphicsDX12::FinishBindlessValidation()
{
    if (!m_bindlessValidate)
        return;
    // The GPU is idle, so every slice holds a landed copy; the newest one has
    // the largest count.
    const D3D12_RANGE all{ 0, static_cast<SIZE_T>(kFrameCount) * BINDLESS_VALIDATE_LOG_BYTES };
    void* p = nullptr;
    if (SUCCEEDED(m_bindlessLogReadback->Map(0, &all, &p)))
    {
        const uint32_t* newest = static_cast<const uint32_t*>(p);
        for (uint32_t slot = 1; slot < kFrameCount; ++slot)
        {
            const uint32_t* log = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(p) + static_cast<size_t>(slot) * BINDLESS_VALIDATE_LOG_BYTES);
            if (log[0] > newest[0]) newest = log;
        }
        ReadBindlessLog(newest, true);
        const D3D12_RANGE noWrite{ 0, 0 };
        m_bindlessLogReadback->Unmap(0, &noWrite);
    }
    // The mirror points into the table: detach before it goes.
    m_cbvSrvUavAllocator.AttachGpuMirror(nullptr, 0);
    m_bindlessTable->Unmap(0, nullptr);
    m_bindlessValidate = false;
}

// ==== Pipeline setup ====

// DRED master switch. The default follows the build config (Debug on, Release off), but it is a
// run-time value in both: see GraphicsDX12::SetDREDEnabled() in the header.
namespace
{
#if defined(_DEBUG)
    bool g_dredEnabled = true;
#else
    bool g_dredEnabled = false;
#endif

    // Escape hatch with no boot-order wiring: the DX12_DRED environment variable OVERRIDES the setter both
    // ways (=1 forces on, =0 forces off), because a player can be walked through it without a new build.
    // Unset: no opinion, the setter wins. Read once and cached.
    //   +1 force on, -1 force off, 0 not set.
    int DREDEnvOverride()
    {
        static const int s_env = []
        {
            char buf[8]{};
            const DWORD n = ::GetEnvironmentVariableA("DX12_DRED", buf, sizeof(buf));
            if (n == 0 || n >= sizeof(buf)) return 0;
            return (buf[0] == '0') ? -1 : 1;
        }();
        return s_env;
    }

    // SL_LOG=1: Streamline's own log, off by default (very verbose). It is the only channel that explains
    // why a DLSS / DLSS-G feature declines to run: the support queries and slDLSSGGetState report success
    // while nothing happens.
    bool IsStreamlineLogEnabled()
    {
        static const bool s_on = []
        {
            char buf[8]{};
            const DWORD n = ::GetEnvironmentVariableA("SL_LOG", buf, sizeof(buf));
            return n != 0 && n < sizeof(buf) && buf[0] != '0';
        }();
        return s_on;
    }

    // The D3D12 debug layer, the same opt-in shape as DRED: on by default in Debug, off in Release (it
    // costs real performance); DX12_DEBUGLAYER=1 / =0 forces it on / off. Without it a Release failure
    // is a bare `Close() -> E_INVALIDARG`; with it the run names the cause (e.g. invalid resource-state
    // flags for a compute command list).
    bool IsDebugLayerEnabled()
    {
#ifndef WITH_HARNESS
        return false;   // a shipped build ignores DX12_DEBUGLAYER: it is a development switch
#else
        static const bool s_on = []
        {
            char buf[8]{};
            const DWORD n = ::GetEnvironmentVariableA("DX12_DEBUGLAYER", buf, sizeof(buf));
            if (n != 0 && n < sizeof(buf)) return buf[0] != '0';
#ifdef _DEBUG
            return true;
#else
            return false;
#endif
        }();
        return s_on;
#endif
    }

    // Left by a device loss that DRED did not record (ReportDeviceRemoved), read by the next launch's
    // device creation: that launch arms DRED once, so if the loss comes back it names the GPU command.
    std::string DredNextLaunchMarker()
    {
        return Sys::WritableDataDir() + "/dred_next_launch";
    }
}

void GraphicsDX12::SetDREDEnabled(bool v) { g_dredEnabled = v; }
bool GraphicsDX12::IsDREDEnabled()
{
    const int env = DREDEnvOverride();
    if (env != 0) return env > 0;
    if (g_dredEnabled) return true;
    static const bool s_armedByLastLaunch = []
    {
        std::error_code ec;
        const std::filesystem::path marker = Sys::U8Path(DredNextLaunchMarker());
        if (!std::filesystem::exists(marker, ec)) return false;
        std::filesystem::remove(marker, ec);
        LOG_INFO("DRED armed for this launch: the last one lost its device without it");
        return true;
    }();
    return s_armedByLastLaunch;
}

void GraphicsDX12::LoadPipeline()
{
    m_deviceThread = std::this_thread::get_id();
    m_deviceFrameThread.store(m_deviceThread, std::memory_order_release);

    LOG_INFO("LoadPipeline begin");

    // MUST precede device creation.
    if (IsDebugLayerEnabled())
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            LOG_INFO("D3D12 debug layer enabled ??messages will be forwarded to Logger");
            // GPU-based validation, a further opt-in on top of the layer (DX12_GBV=1): the only check that sees a
            // descriptor read through the wrong view type under Resource Binding Tier 3 (e.g. a TEXTURECUBE SRV
            // sampled as Texture2D). Very slow; a diagnostic run, never a default.
            char gbv[8]{};
            const DWORD n = ::GetEnvironmentVariableA("DX12_GBV", gbv, sizeof(gbv));
            ComPtr<ID3D12Debug1> debug1;
            if (n != 0 && n < sizeof(gbv) && gbv[0] != '0' && SUCCEEDED(debugController.As(&debug1)))
            {
                debug1->SetEnableGPUBasedValidation(TRUE);
                LOG_INFO("D3D12 GPU-based validation enabled (DX12_GBV)");
            }
        }
    }

    // DRED: GPU auto-breadcrumbs + the page-fault VA, so a hung / removed device names the last GPU
    // command and the faulting resource. A breadcrumb write per command costs a little, so Release keeps
    // it compiled in but off by default (Debug on; SetDREDEnabled / DX12_DRED=1). It must be set BEFORE
    // the device is created.
    if (IsDREDEnabled())
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
        {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            // DRED 1.2: record the open marker (BeginEvent string) per breadcrumb,
            // so the dump names the pass, not just "DRAWINSTANCED in cmd17".
            ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings1;
            if (SUCCEEDED(dredSettings.As(&dredSettings1)))
                dredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            m_dredActive = true;
            LOG_INFO("D3D12 DRED auto-breadcrumbs + page-fault tracking enabled");
        }
        else
            LOG_WARNING("D3D12 DRED unavailable (GetDebugInterface failed) ??device-hang breadcrumbs won't be captured");
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    // Streamline must initialise BEFORE the first DXGI / D3D call: its manual hooking must be resident to
    // proxy the swap chain later. A failure is normal and non-fatal (no SDK, non-NVIDIA GPU, blocked
    // DLLs): DLSS and Frame Generation stay unavailable. -no_streamline opts out (offline rendering,
    // diagnosing NGX / telemetry shutdown stalls).
    // Streamline costs ~4 s of every boot (slInit ~1.9 s here, the NGX probes ~2.2 s once the device
    // exists) and most harness runs cannot use it, so an UNATTENDED run whose settings ask for no DLSS /
    // DLSS_RR / Reflex / Frame Generation, and whose command line names no -dlss* flag, skips it as
    // -no_streamline does; -streamline loads it anyway. The decision reads the same settings bytes the
    // real load will (Resource::PeekSettingsWantStreamline). Interactive runs and Retail never take this
    // branch.
    bool skipStreamline = Sys::CommandLine::Get().Has("-no_streamline");
#if WITH_HARNESS
    if (!skipStreamline && Sys::CommandLine::Get().Has("-unattended"))
    {
        bool dlssFlag = false;
        for (const std::string& a : Sys::CommandLine::Get().Args())
            if (_strnicmp(a.c_str(), "-dlss", 5) == 0 || _strnicmp(a.c_str(), "--dlss", 6) == 0)
            { dlssFlag = true; break; }
        std::string why;
        const bool forced = Sys::CommandLine::Get().Has("-streamline");
        const bool wanted = forced || dlssFlag || Resource::PeekSettingsWantStreamline(why);
        if (!wanted)
        {
            skipStreamline = true;
            LOG_INFO("[BOOT] Streamline skipped: this unattended run asks for no DLSS, Reflex or "
                     "Frame Generation (%s, no -dlss* flag); -streamline loads it anyway",
                     why.c_str());
        }
        else
            LOG_INFO("[BOOT] Streamline loaded: %s", forced ? "-streamline"
                     : dlssFlag ? "a -dlss* flag is on the command line" : why.c_str());
    }
#endif
    if (!skipStreamline)
    {
        SL::InitDesc slDesc{};
        slDesc.enableLogging = IsStreamlineLogEnabled();   // SL_LOG=1
        // Whether to load the DLSS-RR plugin (engine.json raytracing.rayReconstructionLoad). auto (default)
        // loads it in every interactive run and in an unattended run whose settings ask for DLSS-RR, so
        // harness runs keep their fast, RR-less boot unless a pack asks; -dlss_rr / -dlss_rr_probe force it.
        {
            const auto& slCmd = Sys::CommandLine::Get();
            const int rrLoad = Resource::PeekEngineDevRayReconstructionLoad();
            bool rrSettings = false;
            if (rrLoad == 0)
            {
                std::string ignored;
                rrSettings = Resource::PeekSettingsWantRayReconstruction(ignored);
            }
            slDesc.requestRayReconstruction = slCmd.Has("-dlss_rr_probe") || slCmd.Has("-dlss_rr") ||
                rrLoad == 1 || (rrLoad == 0 && (!slCmd.Has("-unattended") || rrSettings));
            LOG_INFO("[BOOT] DLSS-RR plugin %s (rayReconstructionLoad=%s%s)",
                slDesc.requestRayReconstruction ? "requested" : "not requested",
                rrLoad == 1 ? "always" : rrLoad == 2 ? "never" : "auto",
                rrSettings ? ", settings ask for DLSS-RR" : "");
        }
        // Identity for the NGX-backed features. With no applicationId issued by
        // NVIDIA, Streamline wants the engine triple instead; the projectId is
        // just a stable GUID for this engine, not something NVIDIA assigns.
        slDesc.projectId     = "b2e8f4a1-6d3c-4e57-9a1b-7c5d2e0f8a34";
        slDesc.engineVersion = "1.0";
        // Over-the-air plugin updates: worth it in the shipped game (NVIDIA can push a newer DLSS than the
        // one packaged), pure cost in the editor: ~1.2 s per launch (checkForOTA plus pluginManager
        // reconciling the %PROGRAMDATA%/NVIDIA/NGX copies with the ones shipped beside the exe). Off, the
        // editor also keeps an eDefault DLSS preset from changing after an OTA (DLSSPass pins preset M).
        // A runtime query, not #ifdef: in EngineCore WITH_EDITOR always takes the shipped-game branch.
        slDesc.allowOTA      = !Sys::IsEditorHost();
        SL::StreamlineBackend::Get().Init(slDesc);
    }

    ThrowIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)));

    // Route the FACTORY through Streamline. Manual hooking makes the host responsible for the mandatory
    // hook set (sl_hooks.h), and IDXGIFactory::CreateSwapChainForHwnd is on it: without this the swap
    // chain is not one Streamline knows, so DLSS-G never installs its Present path - yet it still loads,
    // accepts its options and reports eDLSSGStatusOk.
    {
        void* f = m_factory.Get();
        if (SL::StreamlineBackend::Get().UpgradeInterface(&f) && f != m_factory.Get())
            m_factory.Attach(static_cast<IDXGIFactory6*>(f));
    }

    // Adapter selection. Pass the ENUMERATED adapter to D3D12CreateDevice; do not simplify it to nullptr,
    // which lets DXGI pick a default adapter unrelated to the one logged (the device could land on the
    // iGPU while the log names the dGPU).
    // Boot marks: everything before "preDevice" is the engine's own preparation; from there to "device"
    // it is the driver creating the device.
    Sys::Boot::Mark("preDevice");
    // display.adapter: the player's card goes first; any other order stays the high-performance one.
    const std::string wantedAdapter = Resource::PeekSettingsAdapter();
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; DXGI_ERROR_NOT_FOUND != m_factory->EnumAdapterByGpuPreference(
                 i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)); ++i)
            adapters.push_back(std::move(a));
    }
    if (!wantedAdapter.empty())
    {
        auto named = std::find_if(adapters.begin(), adapters.end(), [&](const ComPtr<IDXGIAdapter1>& a)
        {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            return Sys::WideToUtf8(d.Description) == wantedAdapter;
        });
        if (named != adapters.end())
            std::rotate(adapters.begin(), named, named + 1);
        else
            LOG_WARNING("display.adapter '%s': no such graphics card here; using the high-performance one",
                        wantedAdapter.c_str());
    }
    ComPtr<IDXGIAdapter1> adapter;
    UINT hwAdaptersTried = 0;
    for (ComPtr<IDXGIAdapter1>& candidate : adapters)
    {
        adapter = candidate;
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // WARP is not a fallback we want
        ++hwAdaptersTried;
        LOG_INFO("Trying adapter: %ls", desc.Description);
        bool ok = false;
        for (auto fl : featureLevels)
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&m_device))))
            {
                m_featureLevel = fl;
                ok = true;
                break;
            }
        if (ok)
        {
            Sys::Boot::Mark("device");   // bootPhasesMs
            VideoMemory::Sample(m_factory.Get(), m_device.Get());   // the [MEM] lines carry it from here on
            // Record the adapter the device was ACTUALLY created on, so the report below and the PSO-cache
            // fingerprint describe reality.
            m_adapter     = adapter;
            m_adapterDesc = desc;
            m_adapterLuid = desc.AdapterLuid;
            // UMD version: the piece CreatePipelineLibrary silently keys on. Not all adapters report it; 0
            // means "unknown", not an error.
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
                m_driverVersion = static_cast<uint64_t>(umd.QuadPart);
            break;
        }
        // Failed adapter: m_device may hold a partially-assigned pointer from a
        // failed IID_PPV_ARGS call on some runtimes. Reset so the null test below
        // is meaningful.
        m_device.Reset();
    }

    // No hardware adapter reached FL 11.0: fail here with a reason, not with an access violation on a
    // null m_device in CreateCommandQueue.
    if (!ENGINE_CHECK(m_device != nullptr,
                      "No Direct3D 12 capable GPU found. This engine requires a graphics "
                      "card supporting D3D12 Feature Level 11.0 with an up-to-date driver. "
                      "(Tried %u hardware adapter(s); none created a device.)",
                      hwAdaptersTried))
    {
        Sys::NotePlayerError(Sys::PlayerError::MinimumSpec, hwAdaptersTried ? "no Direct3D 12 device" : "none found");
        throw std::runtime_error(
            "No Direct3D 12 capable GPU found (Feature Level 11.0 required). "
            "Update your graphics driver, or run on a machine with a D3D12-class GPU.");
    }

    // Route the DEVICE through Streamline too, BEFORE any command queue is created:
    // ID3D12Device::CreateCommandQueue is on manual hooking's mandatory list, because DLSS-G must know
    // which queue presents. FidelityFX / NRD / Tracy then receive the proxy rather than the raw device;
    // that is supported (SL proxies forward QueryInterface), and slGetNativeInterface returns the real
    // pointer if one ever needs it.
    {
        void* d = m_device.Get();
        if (SL::StreamlineBackend::Get().UpgradeInterface(&d) && d != m_device.Get())
            m_device.Attach(static_cast<ID3D12Device*>(d));
    }

    // Also runs the per-feature support queries, so DLSS availability is known
    // from here on.
    SL::StreamlineBackend::Get().SetD3DDevice(m_device.Get());
    if(Sys::CommandLine::Get().Has("-dlss_rr_probe"))
    {
        // Configuration-only control. It does not evaluate RR or alter the AA arm.
        auto& backend=SL::StreamlineBackend::Get();
        DirectX::XMFLOAT4X4 identity;
        DirectX::XMStoreFloat4x4(&identity,DirectX::XMMatrixIdentity());
        const bool supported=backend.IsFeatureSupported(SL::Feature::DLSS_RR);
        bool sr=false,d=false,e=false,f=false,off=false;
        if(supported)
        {
            sr=backend.SetDLSSOptions(SL::DLSSMode::DLAA,1920,1080,0.f,true,SL::DLSSPreset::M);
            d=backend.SetDLSSDOptions(SL::DLSSMode::DLAA,1920,1080,identity,identity,SL::DLSSRRModel::D);
            e=backend.SetDLSSDOptions(SL::DLSSMode::DLAA,1920,1080,identity,identity,SL::DLSSRRModel::E);
            f=backend.SetDLSSDOptions(SL::DLSSMode::DLAA,1920,1080,identity,identity,SL::DLSSRRModel::F);
            off=backend.SetDLSSDOptions(SL::DLSSMode::Off,1920,1080,identity,identity);
            off=backend.SetDLSSOptions(SL::DLSSMode::Off,1920,1080,0.f,true)&&off;
        }
        LOG_INFO("[DLSS_RR_PROBE] supported=%u sr=%u presetD=%u presetE=%u presetF=%u off=%u evaluated=0 status=%s",
            supported,sr,d,e,f,off,backend.GetFeatureStatusText(SL::Feature::DLSS_RR));
    }

    // Create one command queue per queue type (GRAPHICS / COMPUTE / COPY)
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[0])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[1])));
        qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queues[2])));
        // DRED and captures name the queue a hung list ran on; a list on none
        // of these three is a third-party one (Streamline / NGX / video).
        m_queues[0]->SetName(L"GraphicsDX12.Graphics");
        m_queues[1]->SetName(L"GraphicsDX12.Compute");
        m_queues[2]->SetName(L"GraphicsDX12.Copy");
    }

    // Forward D3D12 debug layer messages to the Logger, only when the layer is on. The cookie is the
    // file-scope g_d3d12MsgCallbackCookie, so the destructor can unregister it in every config.
    if (IsDebugLayerEnabled())
    {
        ComPtr<ID3D12InfoQueue1> infoQueue;
        if (SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            struct D3D12LogCallback
            {
                static void __stdcall Callback(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                    D3D12_MESSAGE_ID id, LPCSTR pDescription, void*)
                {
                    LogLevel level = LogLevel::Info;
                    switch (severity)
                    {
                    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
                    case D3D12_MESSAGE_SEVERITY_ERROR:   level = LogLevel::Error;   break;
                    case D3D12_MESSAGE_SEVERITY_WARNING: level = LogLevel::Warning; break;
                    default:                             level = LogLevel::Info;    break;
                    }
                    // ---- One message id is downgraded, and only one ----
                    // LOADPIPELINE_NAMENOTFOUND is a COLD PSO CACHE (a first run on a machine, after a driver update
                    // or a deleted cache file): GraphicsDX12_Resources.cpp asks the library first and compiles the PSO
                    // itself on the miss, by design. As a WARNING it made every pack's engineWarnings budget depend on
                    // whether pso_cache.bin existed (51 on one cold run against a budget of 4). Every other WARNING
                    // keeps its severity. The `[PSO] library ...` line at shutdown is the naming-drift signal instead
                    // (always present, one line; see the note there).
                    if (id == D3D12_MESSAGE_ID_LOADPIPELINE_NAMENOTFOUND)
                        level = LogLevel::Info;
                    Logger::Get().Log(level, "D3D12", 0, "%s", pDescription ? pDescription : "");
                }
            };
            if (SUCCEEDED(infoQueue->RegisterMessageCallback(
                    &D3D12LogCallback::Callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &g_d3d12MsgCallbackCookie)))
                LOG_INFO("D3D12 debug messages forwarded to Logger");
        }
    }

    // Check tearing (variable refresh rate) support.
    {
        BOOL allowTearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(m_factory.As(&factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                          &allowTearing, sizeof(allowTearing));
        m_tearingSupported = (allowTearing == TRUE);
        LOG_INFO("GraphicsDX12: tearing support = %s", m_tearingSupported ? "YES" : "NO");
    }

    // Mesh-shader capability (Options7). Terrain and the other mesh-shader passes check
    // SupportsMeshShader() and fall back when the runtime / GPU lacks it.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,
                                                    &opts7, sizeof(opts7))))
        {
            m_meshShaderSupported = (opts7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1);
        }
#if WITH_HARNESS
        // -force_no_mesh_shader: a no-mesh-shader machine on one that has them (like -force_no_inline_rt):
        // Terrain / Grass self-disable and the meshlet path reports reason=no_mesh_shader (meshlet_fallback
        // pack).
        if (Sys::CommandLine::Get().Has("-force_no_mesh_shader")) m_meshShaderSupported = false;
#endif
        LOG_INFO("GraphicsDX12: mesh shader support = %s",
                 m_meshShaderSupported ? "YES (Tier_1+)" : "NO");
    }

    // DXR raytracing capability (Options5). DDGI probe trace requires Tier_1_0.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,
                                                    &opts5, sizeof(opts5))))
        {
            m_dxrSupported = (opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0);
            m_inlineRtSupported = (opts5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1);
        }
#if WITH_HARNESS
        if (Sys::CommandLine::Get().Has("-force_no_inline_rt")) m_inlineRtSupported = false;
#endif
        LOG_INFO("GraphicsDX12: DXR support = %s",
                 m_dxrSupported ? "YES (Tier_1_0+)" : "NO");
    }

    // Native 16-bit shader ops (Options4): what -enable-16bit-types compiles
    // for. RTXDI's ReSTIR PT kernels need it.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 opts4{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4,
                                                    &opts4, sizeof(opts4))))
            m_native16BitSupported = opts4.Native16BitShaderOpsSupported != FALSE;
#if WITH_HARNESS
        if (Sys::CommandLine::Get().Has("-force_no_native16")) m_native16BitSupported = false;
#endif
        LOG_INFO("GraphicsDX12: native 16-bit shader ops = %s", m_native16BitSupported ? "YES" : "NO");
    }

    // 64-bit atomics on descriptor-heap resources (Options1 + Options11):
    // SHaRC's hash-grid insert in the RTXDI PT core.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS11 opts11{};
        const bool int64Ops = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1))) &&
                              opts1.Int64ShaderOps != FALSE;
        const bool heapAtomics = SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &opts11, sizeof(opts11))) &&
                                 opts11.AtomicInt64OnDescriptorHeapResourceSupported != FALSE;
        m_int64HeapAtomics = int64Ops && heapAtomics;
#if WITH_HARNESS
        if (Sys::CommandLine::Get().Has("-force_no_int64_atomics")) m_int64HeapAtomics = false;
#endif
        LOG_INFO("GraphicsDX12: 64-bit atomics on heap resources = %s (int64 ops %s, heap atomics %s)",
                 m_int64HeapAtomics ? "YES" : "NO", int64Ops ? "YES" : "NO", heapAtomics ? "YES" : "NO");
    }

    // SM6.6 bindless-heap capability (Resource Binding Tier 3): a HARD engine requirement, not a feature
    // toggle (see the throw below). Queried before the root signatures are built.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                                                    &opts, sizeof(opts))))
        {
            m_resourceBindingTier3 =
                (opts.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3);
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 };
        const bool sm66 =
            SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                                                    &sm, sizeof(sm)))
            && sm.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
        m_shaderModel66 = sm66;
        LOG_INFO("GraphicsDX12: resource binding tier 3 = %s, SM6.6 = %s",
                 m_resourceBindingTier3 ? "YES" : "NO", sm66 ? "YES" : "NO");
        // The hard requirement check is not here: it fires right after the boot GPU report below, so a
        // rejected machine still logs the whole paste-into-a-bug-report block first.
    }

    // ---- Boot GPU report ----
    // One contiguous block, one item per line, so a player can select it whole and paste it into a bug
    // report. Emitted here because the caps it lists are only known once the device exists. It
    // describes the adapter the device was actually created on.
    {
        const char* flName = "unknown";
        switch (m_featureLevel)
        {
        case D3D_FEATURE_LEVEL_12_2: flName = "12_2"; break;
        case D3D_FEATURE_LEVEL_12_1: flName = "12_1"; break;
        case D3D_FEATURE_LEVEL_12_0: flName = "12_0"; break;
        case D3D_FEATURE_LEVEL_11_1: flName = "11_1"; break;
        case D3D_FEATURE_LEVEL_11_0: flName = "11_0"; break;
        default: break;
        }
        // WDDM packs the UMD version as four 16-bit fields inside the QuadPart.
        const unsigned drvA = static_cast<unsigned>((m_driverVersion >> 48) & 0xFFFF);
        const unsigned drvB = static_cast<unsigned>((m_driverVersion >> 32) & 0xFFFF);
        const unsigned drvC = static_cast<unsigned>((m_driverVersion >> 16) & 0xFFFF);
        const unsigned drvD = static_cast<unsigned>((m_driverVersion      ) & 0xFFFF);
        const double toMB = 1.0 / (1024.0 * 1024.0);

        LOG_INFO("===== GPU report (paste this whole block into bug reports) =====");
        LOG_INFO("  Adapter        : %ls", m_adapterDesc.Description);
        LOG_INFO("  Vendor/Device  : VEN_%04X DEV_%04X SUBSYS_%08X REV_%02X",
                 m_adapterDesc.VendorId, m_adapterDesc.DeviceId,
                 m_adapterDesc.SubSysId, m_adapterDesc.Revision);
        LOG_INFO("  Adapter LUID   : %08X-%08X",
                 static_cast<unsigned>(m_adapterLuid.HighPart),
                 static_cast<unsigned>(m_adapterLuid.LowPart));
        LOG_INFO("  Driver (UMD)   : %u.%u.%u.%u%s", drvA, drvB, drvC, drvD,
                 m_driverVersion == 0 ? "  (not reported by adapter)" : "");
        LOG_INFO("  VRAM dedicated : %.0f MB",
                 static_cast<double>(m_adapterDesc.DedicatedVideoMemory) * toMB);
        LOG_INFO("  VRAM sys-side  : %.0f MB",
                 static_cast<double>(m_adapterDesc.DedicatedSystemMemory) * toMB);
        LOG_INFO("  Shared sys mem : %.0f MB",
                 static_cast<double>(m_adapterDesc.SharedSystemMemory) * toMB);
        LOG_INFO("  Feature level  : %s", flName);
        LOG_INFO("  Shader Model   : %s", m_shaderModel66 ? "6.6+" : "< 6.6 (UNSUPPORTED)");
        LOG_INFO("  Res binding    : %s", m_resourceBindingTier3 ? "Tier 3" : "< Tier 3 (UNSUPPORTED)");
        LOG_INFO("  DXR raytracing : %s", m_dxrSupported ? "YES (Tier_1_0+)" : "NO");
        LOG_INFO("  Mesh shaders   : %s", m_meshShaderSupported ? "YES (Tier_1+)" : "NO");
        LOG_INFO("  Tearing / VRR  : %s", m_tearingSupported ? "YES" : "NO");
        LOG_INFO("  DRED           : %s", m_dredActive ? "ON" : "off (set DX12_DRED=1 to arm)");
        LOG_INFO("===============================================================");
    }

    // ---- Minimum hardware gate: Resource Binding Tier 3 + Shader Model 6.6 ----
    // Fatal: the deferred path opens its point / per-object / hair shadow maps, RT reflections, RT
    // sun-shadow mask and DDGI data only through ResourceDescriptorHeap[]. Without heap-direct indexing
    // the shaders read undefined descriptors mid-shading (corrupted lighting or a device removal), which
    // is worse than not booting. ENGINE_CHECK compiles in every configuration, so the Release exe prints
    // the reason; the throw matches the other unrecoverable init failures (ThrowIfFailedImpl ->
    // std::runtime_error).
    if (!ENGINE_CHECK(m_resourceBindingTier3 && m_shaderModel66,
                      "This GPU does not meet the engine's minimum requirements. "
                      "Resource Binding Tier 3 = %s (Tier 3 required), Shader "
                      "Model = %s (6.6 required). The deferred lighting path "
                      "reads its point/per-object/hair shadow maps, its RT "
                      "reflections and its RT sun-shadow mask through "
                      "ResourceDescriptorHeap[], which needs both. In practice "
                      "that means NVIDIA Turing (GTX 16 / RTX 20 series) or "
                      "newer, AMD RDNA (RX 5000) or newer, or Intel Arc ??with "
                      "an up-to-date driver. See the GPU report above.",
                      m_resourceBindingTier3 ? "YES" : "NO",
                      m_shaderModel66 ? "6.6+" : "below 6.6"))
    {
        Sys::NotePlayerError(Sys::PlayerError::MinimumSpec, Sys::WideToUtf8(m_adapterDesc.Description));
        throw std::runtime_error(
            "GPU lacks Resource Binding Tier 3 / Shader Model 6.6, which the "
            "engine requires for bindless descriptor-heap indexing");
    }

    // The swap chain, its RTV heap and its buffers belong to the MAIN SURFACE: CreateMainSurface, after
    // the device exists.
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Managed descriptor heap allocators.
    // RTV 1024 / DSV 2048, CPU-only (gpuStatic = gpuDynamic = 0), so the cost is a few KB. Steady state:
    // DSV ~43 views (~79 during a world reload, while old shadow-atlas views wait in the deferred
    // bucket); RTV holds one lazy view per (reflection probe, face) = 64 x 6 on top of the per-texture
    // views. CreateTexture refuses on exhaustion, so undersizing is loud.
    m_rtvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1024, 0, 0);
    m_dsvAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,  2048, 0, 0);
    // CBV_SRV_UAV: 1,000,000 CPU staging, 1,000,000 GPU static, 0 GPU dynamic. The capacity obtained
    // is what GetCapacity() reports after CreateHeapWithFallback.
    // 1,000,000 is D3D12_MAX_SHADER_VISIBLE_DESCRIPTOR_HEAP_SIZE_TIER_1, the largest shader-visible heap
    // the API guarantees on any binding tier, so the ceiling is not re-tuned per scene. 65536 is not
    // enough: the two bindless ranges (MeshDescriptorHeap::kMaxBuffers + kMaxBindlessTextures, 16384
    // each) plus ~21000 per-resource views take 82% before a scene loads, and a world's buffers stay live
    // while the NEXT world loads (SkinnedMeshSubsystem's one-world release lag, DestroyBuffer's
    // kFrameCount deferral), so a heavy scene must fit twice.
    // Cost: 32 B a descriptor, ~32 MB for each of the two heaps, ~64 MB resident. It does not touch the
    // root signature or any bindless index (bounded by MaxBindlessBuffers / MaxBindlessTextures).
    // No dynamic region: nothing calls AllocateDynamic / ReclaimDynamic (every descriptor comes from
    // AllocateStatic or a bindless sub-range), and with 0 AllocateDynamic returns an invalid handle, so a
    // new caller fails at its own call site. Give it a real size if the ring is ever wired up.
    // Init() checks the HRESULT and halves until the driver accepts, logging what it settled on: at the
    // API maximum a refusal under memory pressure is possible.
    m_cbvSrvUavAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                              1000000, 1000000, 0);
    // Sampler: kMaxSamplers CPU staging, kMaxSamplers GPU static, 0 dynamic.
    // Move both together: AllocateStatic reserves a CPU staging slot before the GPU static one, so the
    // ceiling is the MINIMUM of the two. 2048 is D3D12's hard cap on a shader-visible sampler heap (~128
    // KB for both heaps at 32 B a descriptor). CreateSampler's dedupe (kSamplerDedupe,
    // GraphicsDX12_Resources.cpp) makes the shutdown high-water the count of DISTINCT descs; "hits + N"
    // on that line is the pre-dedupe call count.
    m_samplerAllocator.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                            kMaxSamplers, kMaxSamplers, 0);

    // Allocate bindless texture table (contiguous SRV block for all textures).
    m_bindlessTexTable = m_cbvSrvUavAllocator.Allocate(kMaxBindlessTextures);
    if (m_bindlessTexTable.IsValid())
    {
        // Fill with null Texture2D SRVs (RGBA8 UNORM) so unbound slots read zeros instead of faulting.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
        nullSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv.Texture2D.MipLevels     = 1;
        UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuH = m_bindlessTexTable.GetGpuCpuHandle();
        for (uint32_t i = 0; i < kMaxBindlessTextures; ++i)
        {
            m_cbvSrvUavAllocator.WriteSrv(nullptr, &nullSrv, cpuH);
            cpuH.ptr += descInc;
        }
        LOG_SUCCESS("GraphicsDX12: bindless texture table allocated (%u slots)", kMaxBindlessTextures);
    }
    // g_AllTextures[i] and ResourceDescriptorHeap[i] name the same descriptor only while the table starts
    // at heap index 0, which holds because it is the heap's first allocation. Every "texture handleId as
    // a heap index" shortcut (the water ripple field) leans on it.
    ENGINE_CHECK(m_bindlessTexTable.IsValid()
                 && DescriptorHeapIndexFromGpuHandle(m_bindlessTexTable.GetGpuHandle().ptr) == 0,
                 "bindless texture table must start at heap index 0 (it starts at %u)",
                 m_bindlessTexTable.IsValid()
                     ? DescriptorHeapIndexFromGpuHandle(m_bindlessTexTable.GetGpuHandle().ptr) : ~0u);
    CreateBindlessNulls();
    CreateBindlessValidation();   // only under -bindless_validate

    // Command allocators for BOTH upload contexts up front, not lazily: a lazy create would let the game
    // side's first CreateBuffer allocate from whichever thread got there first, the device call from an
    // unexpected thread the game/render split forbids.
    for (auto& ctx : m_uploadCtx)
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.allocator)));
    // The loader contexts too, for the same reason.
    for (auto& ctx : m_loaderCtx)
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ctx.allocator)));

    LOG_SUCCESS("LoadPipeline finished");
}

void GraphicsDX12::LoadAssets()
{
    // Upload command lists stay open so CreateBuffer / CreateTexture can record into them. One per
    // context, each with its OWN fence: a shared fence counter would let one side's flush return on the
    // other side's signal, before its own copy landed.
    // The copies into resources created with initial data go on a COPY list on the COPY queue;
    // -upload_directqueue keeps them on the graphics list (the A/B, and the reverse arm of
    // load_upload_copyqueue).
    m_uploadCopyQueue = !Sys::CommandLine::Get().Has("-upload_directqueue");
    // A loader worker's uploads take a loader context of their own; -upload_sync keeps the callers on
    // their main-thread path (the reverse arm). Never with the creation copies on the graphics list.
    m_loaderUploads = m_uploadCopyQueue && !Sys::CommandLine::Get().Has("-upload_sync");
    UploadContext* allCtx[] = { &m_uploadCtx[0], &m_uploadCtx[1], &m_loaderCtx[0], &m_loaderCtx[1] };
    static_assert(kLoaderContexts == 2, "allCtx lists the loader contexts");
    for (UploadContext* ctxPtr : allCtx)
    {
        UploadContext& ctx = *ctxPtr;
        ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            ctx.allocator.Get(), nullptr, IID_PPV_ARGS(&ctx.list)));
        const std::wstring uploadName = IsLoaderCtx(ctx)
            ? L"GraphicsDX12.Loader" + std::to_wstring(&ctx - m_loaderCtx)
            : L"GraphicsDX12.Upload" + std::to_wstring(&ctx - m_uploadCtx);
        ctx.list->SetName(uploadName.c_str());
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&ctx.fence)));
        ctx.fenceValue = 0;

        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&ctx.copyAllocator)));
        ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY,
            ctx.copyAllocator.Get(), nullptr, IID_PPV_ARGS(&ctx.copyList)));
        ctx.copyList->SetName((uploadName + L".Copy").c_str());
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&ctx.copyFence)));
        ctx.copyFenceValue = 0;
        ctx.copyDirty = ctx.directDirty = false;
    }
    // Created open; leave them open for immediate staging use.
    LOG_INFO("[UPLOAD] creation copies on the %s queue%s",
             m_uploadCopyQueue ? "COPY" : "graphics", m_uploadCopyQueue ? "" : " (-upload_directqueue)");
    LOG_INFO("[UPLOAD] loader uploads %s",
             m_loaderUploads ? "on (2 loader contexts: a worker's batch waits on that worker)"
                             : (m_uploadCopyQueue ? "off (-upload_sync)" : "off (-upload_directqueue)"));

    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue = 1;


    // Per-queue GPU-GPU cross-queue synchronization fences
    for (UINT q = 0; q < 3; ++q)
    {
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_queueFences[q])));
        m_queueFenceValues[q] = 0;
    }

    // Build the shared root signature once: every CreatePipelineState call reuses it.
    m_defaultRootSignature = CreateDefaultRootSignature();

    m_computeRootSignature = CreateComputeRootSignature();
    if (!m_computeRootSignature)
    { LOG_ERROR("GraphicsDX12: failed to create compute root signature"); }
    else
    { LOG_SUCCESS("GraphicsDX12: compute root signature ready"); }

    // ---- Command signature for ExecuteIndirect (root constants + DrawInstanced) ----
    {
        D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
        // Arg 0: 4 root constants at param 0
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant.RootParameterIndex = kRootConstantsSlot;
        args[0].Constant.DestOffsetIn32BitValues = 0;
        args[0].Constant.Num32BitValuesToSet = 4;
        // Arg 1: DrawInstanced
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;

        D3D12_COMMAND_SIGNATURE_DESC csDesc = {};
        csDesc.ByteStride = 32; // sizeof(IndirectDrawCommand) = 4*4 root const + 4*4 draw args = 32
        csDesc.NumArgumentDescs = 2;
        csDesc.pArgumentDescs = args;

        HRESULT hr = m_device->CreateCommandSignature(
            &csDesc, m_defaultRootSignature.Get(),
            IID_PPV_ARGS(&m_indirectCommandSignature));
        if (FAILED(hr))
            LOG_ERROR("GraphicsDX12: CreateCommandSignature failed (hr=0x%08X)", static_cast<unsigned>(hr));
        else
            LOG_SUCCESS("GraphicsDX12: indirect command signature ready (stride=%u)", csDesc.ByteStride);
    }
    // ---- ... and the same command with DispatchMesh: the cull CS rewrites a meshlet group's words
    // 4..6 to ThreadGroupCountX / Y / Z = cluster blocks, survivors, 1; word 7 pads the 28 bytes
    // DISPATCH_MESH reads to the shared 32-byte stride.
    if (m_meshShaderSupported)
    {
        D3D12_INDIRECT_ARGUMENT_DESC args[2] = {};
        args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        args[0].Constant.RootParameterIndex = kRootConstantsSlot;
        args[0].Constant.DestOffsetIn32BitValues = 0;
        args[0].Constant.Num32BitValuesToSet = 4;
        args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;

        D3D12_COMMAND_SIGNATURE_DESC csDesc = {};
        csDesc.ByteStride = 32;
        csDesc.NumArgumentDescs = 2;
        csDesc.pArgumentDescs = args;

        HRESULT hr = m_device->CreateCommandSignature(
            &csDesc, m_defaultRootSignature.Get(),
            IID_PPV_ARGS(&m_dispatchMeshCommandSignature));
        if (FAILED(hr))
            LOG_ERROR("GraphicsDX12: CreateCommandSignature (DISPATCH_MESH) failed (hr=0x%08X)",
                      static_cast<unsigned>(hr));
        else
            LOG_SUCCESS("GraphicsDX12: indirect dispatch-mesh signature ready (stride=%u)", csDesc.ByteStride);
    }
    // ---- ... and a bare compute dispatch (DispatchComputeIndirect): one DISPATCH argument, a 12-byte
    // stride. It changes no root arguments, so it takes no root signature and serves every layout.
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg = {};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

        D3D12_COMMAND_SIGNATURE_DESC csDesc = {};
        csDesc.ByteStride       = sizeof(D3D12_DISPATCH_ARGUMENTS);
        csDesc.NumArgumentDescs = 1;
        csDesc.pArgumentDescs   = &arg;

        HRESULT hr = m_device->CreateCommandSignature(
            &csDesc, nullptr, IID_PPV_ARGS(&m_dispatchCommandSignature));
        if (FAILED(hr))
            LOG_ERROR("GraphicsDX12: CreateCommandSignature (DISPATCH) failed (hr=0x%08X)",
                      static_cast<unsigned>(hr));
    }

    // Pre-reserve the resource pools so push_back never reallocates while another thread holds a
    // reference from an earlier Create* call. These reserves are HARD CAPS: the Create* push sites refuse
    // to grow past capacity() (see the pool comment in GraphicsDX12.h). m_psoPool is a
    // Sys::StableAppendPool (nothing a reader indexes ever moves) and needs no reserve.
    m_bufferPool.reserve(kMaxBufferPool);
    m_texturePool.reserve(kMaxTexturePool);
    m_shaderPool.reserve(kMaxShaderPool);
    // Same rule for the sampler pool: SetDynamicSampler (SamplerHeapIndex) indexes it and reads size()
    // from recording workers without m_resourceMutex. kMaxSamplers slots of DescriptorAllocation is ~96
    // KB, and the descriptor heaps behind it are sized for the same count.
    m_samplerPool.reserve(kMaxSamplers);
#if WITH_HARNESS
    // -texpool_pad=N: N empty records first, so every texture after them gets a pool id - its bindless
    // index - N higher, with no descriptor and no memory changing: the shape of one more texture-pool
    // record anywhere. A picture or a draw order that moves under it reads resource ids as data
    // (tests/unit/test_opaque_order.py). The records count in the [POOL] textures high-water.
    if (const int pad = Sys::CommandLine::Get().GetInt("-texpool_pad", 0); pad > 0)
    {
        for (int i = 0; i < pad; ++i) m_texturePool.emplace_back();
        LOG_INFO("[TEXPOOL_PAD] %d empty texture-pool record(s): every texture id +%d", pad, pad);
    }
    // -rhi_lifetime_probe_unguarded: the lifetime probe's reverse arm. DestroyBuffer / DestroyTexture
    // stop refusing an id outside the pool or destroyed already, DrainReleasesUpTo stops refusing an
    // id already on its free list, and CreateTexturePlaced creates a placement past its heap's limit
    // again: the behaviour the guards replaced.
    if (Sys::CommandLine::Get().Has("-rhi_lifetime_probe_unguarded"))
    {
        m_lifetimeGuardsOff = true;
        LOG_WARNING("[RHI_LIFETIME_PROBE] -rhi_lifetime_probe_unguarded: the Destroy, free-list and placement "
                    "guards are off");
    }
#endif

    // The fixed sampler slots, before any pass creates a sampler.
    CreateFixedSamplers();
}

// The fixed slots of shaders/shared/sampler_slots.hlsli, created FIRST so the append-only pool hands
// out exactly SMP_* - which is also the heap index the shaders read, checked here. Every field
// matches what the passes create for the same use, so their own CreateSampler calls dedupe onto these
// slots.
void GraphicsDX12::CreateFixedSamplers()
{
    const auto make = [](RHI::Filter filter, RHI::TextureAddressMode address)
    {
        RHI::SamplerDesc sd{};
        sd.filter    = filter;
        sd.addressU = address;
        sd.addressV = address;
        sd.addressW = address;
        return sd;
    };
    using F = RHI::Filter;
    using A = RHI::TextureAddressMode;
    RHI::SamplerDesc fixed[SMP_FIXED_COUNT] = {
        make(F::MIN_MAG_MIP_LINEAR, A::CLAMP),               // SMP_LINEAR_CLAMP
        make(F::MIN_MAG_MIP_LINEAR, A::WRAP),                // SMP_LINEAR_WRAP
        make(F::MIN_MAG_MIP_LINEAR, A::MIRROR),              // SMP_LINEAR_MIRROR
        make(F::MIN_MAG_MIP_POINT, A::CLAMP),                // SMP_POINT_CLAMP
        make(F::MIN_MAG_MIP_POINT, A::WRAP),                 // SMP_POINT_WRAP
        make(F::MIN_MAG_MIP_POINT, A::MIRROR),               // SMP_POINT_MIRROR
        make(F::MIN_MAG_LINEAR_MIP_POINT, A::CLAMP),         // SMP_LINEAR_MIP_POINT_CLAMP
        make(F::COMPARISON_MIN_MAG_MIP_LINEAR, A::BORDER),   // SMP_CMP_LINEAR_BORDER_WHITE_GE
        make(F::COMPARISON_MIN_MAG_LINEAR_MIP_POINT, A::CLAMP), // SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE
    };
    fixed[SMP_CMP_LINEAR_BORDER_WHITE_GE].borderColor    = RHI::SamplerBorderColor::OPAQUE_WHITE;
    fixed[SMP_CMP_LINEAR_BORDER_WHITE_GE].comparisonFunc = RHI::ComparisonFunc::GREATER_EQUAL;
    fixed[SMP_CMP_LINEAR_MIP_POINT_CLAMP_GE].comparisonFunc = RHI::ComparisonFunc::GREATER_EQUAL;

    for (uint32_t slot = 0; slot < SMP_FIXED_COUNT; ++slot)
    {
        int index = -1;
        const bool ok = CreateSampler(fixed[slot], index);
        ENGINE_CHECK(ok && index == static_cast<int>(slot) && SamplerHeapIndex(index) == slot,
                     "fixed sampler slot %u came out as pool index %d, heap index %u - the "
                     "sampler pool was not empty, or something else allocates from the "
                     "sampler heap (shared/sampler_slots.hlsli)",
                     slot, index, ok ? SamplerHeapIndex(index) : ~0u);
    }
}

// The heap index of sampler pool entry @p poolIndex: what SamplerDescriptorHeap[]
// takes. The same number for the fixed slots (checked above); computed from the
// descriptor for any other.
uint32_t GraphicsDX12::SamplerHeapIndex(int poolIndex) const
{
    if (poolIndex < 0 || static_cast<size_t>(poolIndex) >= m_samplerPool.size()) return ~0u;
    ID3D12DescriptorHeap* heap = m_samplerAllocator.GetGpuHeap();
    const uint32_t inc = m_samplerAllocator.GetDescriptorSize();
    if (!heap || inc == 0) return ~0u;
    const uint64_t base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const uint64_t at   = m_samplerPool[static_cast<size_t>(poolIndex)].GetGpuHandle().ptr;
    return at >= base ? static_cast<uint32_t>((at - base) / inc) : ~0u;
}

// Shared by Present and the fence paths. A TDR most often surfaces at a fence
// first: D3D12 reports a removed device by completing every fence at
// UINT64_MAX, and the slot-retirement wait then fails with nothing but
// "GPU fence device removed" unless this runs.
bool GraphicsDX12::ReportDeviceRemoved(const char* where)
{
    const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : S_OK;
    if (reason == S_OK) return false;
    if (m_deviceRemovedReported.exchange(true)) return true;
    const char* name = "unknown";
    switch ((uint32_t)reason)
    {
    case 0x887A0001: name = "DXGI_ERROR_INVALID_CALL";       break;
    case 0x887A0005: name = "DXGI_ERROR_DEVICE_REMOVED";     break;
    case 0x887A0006: name = "DXGI_ERROR_DEVICE_HUNG";        break;
    case 0x887A0007: name = "DXGI_ERROR_DEVICE_RESET";       break;
    case 0x887A0020: name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
    case 0x80070057: name = "E_INVALIDARG (likely shader / resource state)"; break;
    case 0x8007000E: name = "E_OUTOFMEMORY (GPU out of VRAM)"; break;
    }
    LOG_ERROR("Device removed (noticed by %s): reason=0x%08X (%s). Inspect prior log + DRED output below.",
              where ? where : "?", (unsigned)reason, name);

    // Second of the two required latch points (the other is
    // ThrowIfFailedImpl). This one carries the decoded reason string,
    // which is strictly better information when Present noticed first.
    Harness::RunLedger::Get().NoteDeviceLost(name);
    Sys::NotePlayerError(Sys::PlayerError::DeviceLost, name);   // main's error handler tells the player

    // DRED: dump the GPU op the device died on and any page-fault resource. Needs DRED armed BEFORE
    // device creation (LoadPipeline); compiled in every config, gated by the runtime opt-in.
    if (!m_dredActive)
    {
        LOG_ERROR("DRED was not armed for this device ??no GPU breadcrumbs / page-fault "
                  "info available. The next launch arms it (or relaunch with DX12_DRED=1) "
                  "to capture them if this reproduces.");
        // A file, not a setting: the player's settings stay theirs, and the next launch deletes it.
        const std::string marker = DredNextLaunchMarker();
        (void)Sys::WriteFileBytes(marker, name, std::strlen(name));
    }
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (m_dredActive && SUCCEEDED(m_device->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        // Translate the D3D12_AUTO_BREADCRUMB_OP enum to a readable name.
        auto dredOpName = [](int op) -> const char* {
            switch (op)
            {
            case 0:  return "SETMARKER";
            case 1:  return "BEGINEVENT";
            case 2:  return "ENDEVENT";
            case 3:  return "DRAWINSTANCED";
            case 4:  return "DRAWINDEXEDINSTANCED";
            case 5:  return "EXECUTEINDIRECT";
            case 6:  return "DISPATCH";
            case 7:  return "COPYBUFFERREGION";
            case 8:  return "COPYTEXTUREREGION";
            case 9:  return "COPYRESOURCE";
            case 12: return "CLEARRENDERTARGETVIEW";
            case 13: return "CLEARUNORDEREDACCESSVIEW";
            case 14: return "CLEARDEPTHSTENCILVIEW";
            case 11: return "RESOLVESUBRESOURCE";
            case 15: return "RESOURCEBARRIER";
            case 16: return "EXECUTEBUNDLE";
            case 17: return "PRESENT";
            case 18: return "RESOLVEQUERYDATA";
            case 26: return "WRITEBUFFERIMMEDIATE";
            case 31: return "BUILDRAYTRACINGAS";
            case 33: return "COPYRAYTRACINGAS";
            case 34: return "DISPATCHRAYS";
            case 36: return "EXECUTEMETACOMMAND";
            case 41: return "EXECUTEEXTENSIONCOMMAND";
            case 42: return "DISPATCHMESH";
            case 46: return "BEGIN_COMMAND_LIST";
            default: return "OTHER";
            }
        };
        // A command list mid-execution (0 < done < total) is the actual hang; done == 0 lists are only queued
        // behind it, so they are counted, not listed, and the culprit is not buried.
        // Names: the engine names lists and queues with SetName (WIDE), which DRED reports only in the *W
        // fields.
        auto nameOf = [](const char* a, const wchar_t* w, const char* fallback) -> std::string {
            if (a && *a) return a;
            if (w && *w) return Sys::WideToUtf8(w);
            return fallback;
        };
        // The ops around the hang point: "op-2 op-1 [op] op+1", so a draw is
        // seen next to the barrier / clear / dispatch that framed it.
        auto opWindow = [&](const D3D12_AUTO_BREADCRUMB_OP* hist, UINT done, UINT total) -> std::string {
            std::string s;
            if (!hist) return s;
            const UINT lo = done >= 3 ? done - 3 : 0, hi = (std::min)(total, done + 2);
            for (UINT i = lo; i < hi; ++i) {
                if (!s.empty()) s += ' ';
                if (i == done) s += '[';
                s += dredOpName(static_cast<int>(hist[i]));
                if (i == done) s += ']';
            }
            return s;
        };
        UINT active = 0, notStarted = 0;
        ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1{};
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT  bc{};
        if (SUCCEEDED(dred.As(&dred1)) && SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1)))
        {
            for (const D3D12_AUTO_BREADCRUMB_NODE1* n = bc1.pHeadAutoBreadcrumbNode; n; n = n->pNext)
            {
                const UINT total = n->BreadcrumbCount;
                const UINT done  = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (total == 0 || done >= total) continue;  // empty or finished cleanly
                if (done == 0) { ++notStarted; continue; }  // queued behind the hang
                ++active;
                const int op = n->pCommandHistory ? (int)n->pCommandHistory[done] : -1;
                // The innermost marker opened at or before the hung op: contexts
                // are recorded at BeginEvent/SetMarker breadcrumbs, in order.
                std::string pass = "<no marker>";
                for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c) {
                    const D3D12_DRED_BREADCRUMB_CONTEXT& ctx = n->pBreadcrumbContexts[c];
                    if (ctx.BreadcrumbIndex <= done && ctx.pContextString)
                        pass = Sys::WideToUtf8(ctx.pContextString);
                }
                LOG_ERROR("DRED: CL '%s' / queue '%s' HUNG at op %u/%u = %s (enum %d) in '%s'; ops: %s",
                          nameOf(n->pCommandListDebugNameA, n->pCommandListDebugNameW, "<unnamed CL>").c_str(),
                          nameOf(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, "<unnamed queue>").c_str(),
                          done, total, dredOpName(op), op, pass.c_str(),
                          opWindow(n->pCommandHistory, done, total).c_str());
            }
        }
        else if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
        {
            for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n; n = n->pNext)
            {
                const UINT total = n->BreadcrumbCount;
                const UINT done  = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (total == 0 || done >= total) continue;
                if (done == 0) { ++notStarted; continue; }
                ++active;
                const int op = n->pCommandHistory ? (int)n->pCommandHistory[done] : -1;
                LOG_ERROR("DRED: CL '%s' / queue '%s' HUNG at op %u/%u = %s (enum %d); ops: %s",
                          nameOf(n->pCommandListDebugNameA, n->pCommandListDebugNameW, "<unnamed CL>").c_str(),
                          nameOf(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, "<unnamed queue>").c_str(),
                          done, total, dredOpName(op), op,
                          opWindow(n->pCommandHistory, done, total).c_str());
            }
        }
        if (active == 0 && notStarted > 0)
            LOG_ERROR("DRED: no CL reported partial progress; %u submitted but never ran "
                      "(GPU hung on a first op or a prior submission).", notStarted);
        else if (notStarted > 0)
            LOG_ERROR("DRED: + %u more CL(s) queued but not started (blocked behind the hang).", notStarted);
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)))
        {
            if (pf.PageFaultVA != 0)
                LOG_ERROR("DRED page fault at GPU VA = 0x%llx", (unsigned long long)pf.PageFaultVA);
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode;
                 a != nullptr; a = a->pNext)
                LOG_ERROR("DRED  faulting VA hits EXISTING allocation '%s'",
                          a->ObjectNameA ? a->ObjectNameA : "<unnamed>");
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode;
                 a != nullptr; a = a->pNext)
                LOG_ERROR("DRED  faulting VA hits RECENTLY-FREED allocation '%s' (use-after-free!)",
                          a->ObjectNameA ? a->ObjectNameA : "<unnamed>");
        }
    }
    return true;
}

// Block until @p fence reaches @p value, on a PER-THREAD event. Two threads sharing one auto-reset
// event steal each other's wakeups: one returns from a wait its fence has NOT satisfied (and goes on
// to overwrite GPU-owned memory), the other sleeps on a signalled fence and never wakes.
// Keep the GetCompletedValue() pre-check: SetEventOnCompletion on an already-passed value signals the
// event at once, and the NEXT wait on this thread would return on that stale signal.
void GraphicsDX12::WaitFence(ID3D12Fence* fence, uint64_t value)
{
    if (!fence || value == 0)                    return;
    if (fence->GetCompletedValue() >= value)     return;

    // Leaked deliberately at thread exit: the alternative is a wrapper whose
    // destructor closes the handle during static teardown, which on a worker
    // that outlives the device is a use-after-free on a handle D3D12 may still
    // hold. One HANDLE per thread that ever waits on a fence is a rounding error.
    static thread_local HANDLE t_event = nullptr;
    if (!t_event)
    {
        t_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!t_event) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
    // Only reached when the fence is NOT yet complete (the pre-check above),
    // so this scope is a real block on the GPU, never a free poll.
    PROF_WAIT(WaitGpu, "GPU.FenceWait");
    ThrowIfFailed(fence->SetEventOnCompletion(value, t_event));
    WaitForSingleObject(t_event, INFINITE);
}

uint64_t GraphicsDX12::SignalGraphicsFence()
{
    // The increment and the Signal are ONE step. An atomic counter alone is not
    // enough: two threads can take N and N+1 atomically and then Signal out of
    // order, leaving the fence at N after N+1 - GetCompletedValue() moving
    // backwards, which every waiter here reads as "not done yet".
    std::lock_guard<std::mutex> lk(m_fenceSignalMutex);
    const uint64_t fv = m_fenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    ThrowIfFailed(m_queues[0]->Signal(m_fence.Get(), fv));
    return fv;
}

void GraphicsDX12::WaitForPreviousFrame()
{
    // Park the render thread first: everything below assumes it owns the
    // device alone. Free in serial mode, idempotent within a frame.
    DrainFrameIfNeeded(Diag::DrainCause::DeviceFlush);
    // Sync the GRAPHICS queue (the CPU-GPU frame pacing fence). SignalGraphicsFence pre-increments, as
    // EndFrame does, so the signalled value is strictly above any earlier one.
    const UINT64 fence = SignalGraphicsFence();
    WaitFence(m_fence.Get(), fence);

    // Sync COMPUTE and COPY queues via their cross-queue fences.
    for (UINT q = 1; q <= 2; ++q)
        WaitFence(m_queueFences[q].Get(), m_queueFenceValues[q]);

    // The GPU is fully idle: free every frame-stamped release, not only the retired ones BeginFrame
    // frees, so Resize / destructors can safely release resources.
    ProcessAllDeferredReleases();

    // GPU is fully idle, so every fence-keyed entry's target value must have
    // completed too. Drain them so callers (e.g. ~GraphicsDX12, Resize) see
    // a fully-empty release queue.
    ProcessFenceKeyedReleases();

    // Everything submitted so far is retired: raise the GetRetiredFrame floor. Without it, on a path
    // where frames have stopped (shutdown, Resize), GetRetiredFrame stays kFrameCount frames behind and
    // "has my last record retired" answers no forever.
    // The floor must stay strictly below the stamp of any release still taken against unsubmitted work,
    // so it is frame-aware:
    //   INSIDE a frame  -> m_renderFrame. A Destroy* later in this frame carries ReleaseStampFrame()
    //                      (m_renderFrame + 1) and may name a resource the open primary list still
    //                      references; the query-based immediate-free consumers
    //                      (PreviewAssetSource::DrainPreviewMeshLibHandbacks, LightingPass's quiesced
    //                      predicate) would free it in place.
    //   OUTSIDE a frame -> m_renderFrame + 1. EndFrame submitted that frame and the wait proved it
    //                      finished; recording again needs BeginFrame, which raises the next stamp to
    //                      m_renderFrame + 2. m_renderFrame here would make LightingPass::Shutdown's
    //                      quiesced check fail for a pass whose last record finished.
    // Monotonic: a plain store would LOWER the floor when an in-frame sync follows an out-of-frame one,
    // and GetRetiredFrame never goes backwards. A CAS loop, because fetch_max is C++26.
    const bool     syncInFrame = m_inFrame.load(std::memory_order_acquire);
    const uint64_t syncStamp   = ReleaseStampFrame();     // m_renderFrame + 1
    const uint64_t syncProven  = syncInFrame ? (syncStamp - 1) : syncStamp;
    uint64_t prevFloor = m_hardSyncedFrame.load(std::memory_order_relaxed);
    while (prevFloor < syncProven &&
           !m_hardSyncedFrame.compare_exchange_weak(prevFloor, syncProven,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed))
    {
    }
    const uint64_t syncFloor = m_hardSyncedFrame.load(std::memory_order_relaxed);
    // The self-test, where the invariant is established: inside a frame the floor stays STRICTLY below
    // the stamp a release taken now would carry, and it never exceeds it in either case. ENGINE_CHECK,
    // not assert: it logs and the sync proceeds.
    ENGINE_CHECK(syncInFrame ? (syncFloor < syncStamp) : (syncFloor <= syncStamp),
        "[GFX] hard sync floor %llu is wrong for stamp %llu (inFrame=%d): inside "
        "a frame the floor must be strictly below the stamp a release taken now "
        "would carry, or GetRetiredFrame() reports that release retired before "
        "its command list has been submitted and the immediate-free consumers "
        "(preview mesh-lib handbacks, LightingPass::Shutdown/Init) free in "
        "place.",
        static_cast<unsigned long long>(syncFloor),
        static_cast<unsigned long long>(syncStamp),
        syncInFrame ? 1 : 0);
    // For the packs: one line per hard sync with both numbers and the branch, so the ordering is read off
    // the log. `floor < stamp` on every inFrame=1 line is the assertion; scene_reload_heavy /
    // feature_sweep produce them, and preview_teardown exercises the immediate-free consumers.
    LOG_INFO("[GFX] hard sync floor=%llu stamp=%llu inFrame=%d",
             static_cast<unsigned long long>(syncFloor),
             static_cast<unsigned long long>(syncStamp),
             syncInFrame ? 1 : 0);

    // The back buffer DXGI hands out next (none without a swap chain).
    if (m_surface)
        m_surface->SyncBackBufferIndex();
}

GraphicsDX12::DeferredReleaseList& GraphicsDX12::PendingReleaseBucket()
{
    const uint64_t f = ReleaseStampFrame();
    if (m_pendingReleases.empty() || m_pendingReleases.back().frame != f)
        m_pendingReleases.push_back({ f, {} });

    // Depth tripwire. Stamps are monotonic and BeginFrame drains everything
    // older than kFrameCount, so in steady state the deque holds at most the
    // frames still in flight plus the one being stamped. A deque that keeps
    // growing means the drain predicate stopped matching the stamp - the two
    // are written in different places and nothing else would notice them
    // drifting apart until memory ran out.
    ENGINE_CHECK_ONCE(m_pendingReleases.size() <= kFrameCount + 2,
        "[SPLITCHECK][RELEASEKEY] pending-release queue is %zu deep (expected <= %u): "
        "stamps are being written faster than BeginFrame retires them, so the stamp "
        "and the drain predicate have drifted apart.",
        m_pendingReleases.size(), kFrameCount + 2);

    return m_pendingReleases.back().list;
}

void GraphicsDX12::DrainReleasesUpTo(uint64_t retiredFrame)
{
    // Everything freeable is moved out under the lock and destroyed after it.
    // ComPtr::Release can in principle re-enter this device, and freeing a
    // hundred textures inside the mutex would also stall every concurrent
    // Destroy* on the game side for no reason.
    std::vector<DeferredReleaseList> local;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        while (!m_pendingReleases.empty() && m_pendingReleases.front().frame <= retiredFrame)
        {
            local.push_back(std::move(m_pendingReleases.front().list));
            m_pendingReleases.pop_front();
        }
    }
    if (local.empty()) return;

    for (auto& l : local)
        for (auto& d : l.descriptors) d.Free();

    std::lock_guard<std::mutex> lk(m_resourceMutex);
    const bool tableValid = m_bindlessTexTable.IsValid();
    const UINT descInc = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
    nullSrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    nullSrv.Texture2D.MipLevels     = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE base = tableValid
        ? m_bindlessTexTable.GetGpuCpuHandle() : D3D12_CPU_DESCRIPTOR_HANDLE{};

    for (auto& l : local)
    {
        // An id already on its free list was released twice (two release paths named it): pushed again
        // it would go to two resources at once.
        for (uint32_t sl : l.bufferSlots)
        {
            if (!m_lifetimeGuardsOff &&
                !ENGINE_CHECK_ONCE(sl < m_bufferPool.size() && !m_bufferPool[sl].onFreeList,
                                   "DrainReleasesUpTo: buffer %u is outside the pool or on the buffer free "
                                   "list already - refused", sl))
            {
                m_freeListDupes.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (sl < m_bufferPool.size()) m_bufferPool[sl].onFreeList = true;
            m_bufferFreeList.push_back(sl);
        }

        // Null each freed texture's bindless-table slot before recycling it. CreateTexture copied the
        // texture's SRV into the bindless table at slot [handleId]; DestroyTexture frees only the source
        // DescriptorAllocation, which leaves that copy pointing at a released resource (and a stale SRV would
        // survive a reuse of the slot by an RTV/DSV/UAV-only texture). Safe here: an entry drains only once
        // its stamped frame has retired. Symmetric with the null-fill at Init; serialized with CreateTexture's
        // bindless write by m_resourceMutex (held here).
        for (uint32_t sl : l.textureSlots)
        {
            if (!m_lifetimeGuardsOff &&
                !ENGINE_CHECK_ONCE(sl < m_texturePool.size() && !m_texturePool[sl].onFreeList,
                                   "DrainReleasesUpTo: texture %u is outside the pool or on the texture free "
                                   "list already - refused", sl))
            {
                m_freeListDupes.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (sl < m_texturePool.size()) m_texturePool[sl].onFreeList = true;
            if (tableValid && sl < kMaxBindlessTextures)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst = base;
                dst.ptr += static_cast<SIZE_T>(sl) * descInc;
                m_cbvSrvUavAllocator.WriteSrv(nullptr, &nullSrv, dst);
                // The slot's next texture is a new generation,
                // so a handle made for the destroyed one reads as stale.
                m_cbvSrvUavAllocator.BumpGeneration(m_bindlessTexTable.GetGpuIndex() + sl);
            }
            m_textureFreeList.push_back(sl);
        }
    }
    // `local` destructs on return: heaps outlive their placed resources because
    // DeferredReleaseList declares `heaps` before `resources`.
}

void GraphicsDX12::WaitIdleAndReleaseDeferred()
{
    // Park the render thread first: everything below assumes it owns the
    // device alone. Free in serial mode, idempotent within a frame.
    DrainFrameIfNeeded(Diag::DrainCause::DeviceFlush);
    // TRIPWIRE ONLY: logs, then runs anyway. Do not refuse: the world-load path
    // (EditorLayer::ProcessPendingActions -> OnWorldClear) relies on this drain to avoid a delayed
    // bindless-SRV TDR.
    // It is frame-boundary-only because it hard-syncs all three queues, frees EVERY frame-stamped
    // release (including this frame's) and resyncs the surface's back-buffer index: after a pass
    // started recording it frees descriptors open lists still reference and points EndFrame's present /
    // barrier at a different back buffer than BeginFrame opened.
    // CanHardSyncNow() admits the two designed-in mid-frame callers (App.cpp's ProcessPendingActions
    // drain, Renderer::BeginFrame's graph recompile -> NrdDenoiser::Resize), where only the primary CL
    // exists; it catches pass Execute() bodies and Phase B workers.
    ENGINE_CHECK_ONCE(CanHardSyncNow(),
        "WaitIdleAndReleaseDeferred() called after pass recording started "
        "(cmdLists=%u, parallelPhaseB=%d) ??ProcessAllDeferredReleases will free "
        "descriptors and resources this frame's OPEN command lists already reference, "
        "and m_frameIndex is reassigned under them. Move the call to App.cpp's "
        "ProcessPendingActions drain point (right after BeginFrame, before any new "
        "GPU recording).",
        m_commandListCount.load(std::memory_order_relaxed),
        static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire)));

    WaitForPreviousFrame();
}

void GraphicsDX12::FlushAndWait()
{
    // Park the render thread first: everything below assumes it owns the
    // device alone. Free in serial mode, idempotent within a frame.
    DrainFrameIfNeeded(Diag::DrainCause::DeviceFlush);
    // THREAD tripwire (not a frame-boundary one: this destroys nothing and at worst stalls).
    // SignalGraphicsFence() holds m_fenceSignalMutex across the increment and the Signal, so the value is
    // safe; the SCOPE is wrong: flushing during parallel pass recording waits on submissions other
    // recording threads have not finished making.
    ENGINE_CHECK_ONCE(!m_parallelPassRecording.load(std::memory_order_acquire),
        "FlushAndWait() called during RenderGraph Phase B (parallel pass recording) - "
        "it waits on submissions other recording threads have not finished making. "
        "Move the flush to Phase A or to a frame boundary.");

    // Signal the graphics queue and CPU-wait until the GPU has processed all work submitted to it
    // (picking only uses the graphics queue). SignalGraphicsFence pre-increments, so the value cannot
    // collide with EndFrame's signal and turn the wait into a no-op.
    const UINT64 fence = SignalGraphicsFence();
    WaitFence(m_fence.Get(), fence);
}


// ==== HDR render target ====

void GraphicsDX12::CreateHdrTarget(uint32_t slot, uint32_t width, uint32_t height)
{
    if (!ENGINE_CHECK(slot < kMaxHdrTargets,
                      "CreateHdrTarget: slot %u out of range (kMaxHdrTargets=%u)",
                      slot, kMaxHdrTargets))
        return;
    if (width == 0 || height == 0) return;
    ReleaseHdrTarget(slot, /*releaseId=*/false);
    HdrTargetBinding& hdr = m_hdrTargets[slot];

    const DXGI_FORMAT format = ToDxgiFormat(kHdrSceneColorFormat);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC   td{};
    td.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width              = width;  td.Height = height;
    td.DepthOrArraySize   = 1;      td.MipLevels = 1;
    td.Format             = format;
    td.SampleDesc.Count   = 1;
    td.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags              = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                          | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS; // SSR composite writes
    D3D12_CLEAR_VALUE cv{ format, {0,0,0,0} };
    Microsoft::WRL::ComPtr<ID3D12Resource> res;
    ThrowIfFailed(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&res)));
    const char* const name = slot == 0 ? "GraphicsDX12.HdrEditorViewport" : "GraphicsDX12.HdrTarget";
    GpuLedger::TrackTexture(m_device.Get(), res.Get(), name);
    // Slot 0 keeps the name every debug-layer barrier message and every RenderDoc capture
    // refers to: do not rename it. Extra slots get a distinct one so a barrier error names the
    // target that actually failed.
    if (slot == 0)
        res->SetName(L"GraphicsDX12.HdrEditorViewport");
    else
        res->SetName((L"GraphicsDX12.HdrTarget[" + std::to_wstring(slot) + L"]").c_str());

    // The RTV/DSV fresh-slot histogram names its call sites.
    DescriptorAllocation rtv = m_rtvAllocator.Allocate(1, "GraphicsDX12.HdrTarget");
    D3D12_RENDER_TARGET_VIEW_DESC rtvd{ format, D3D12_RTV_DIMENSION_TEXTURE2D };
    m_device->CreateRenderTargetView(res.Get(), &rtvd, rtv.GetCpuHandle());

    DescriptorAllocation srv = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                  = format;
    srvd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvd.Texture2D.MipLevels     = 1;
    m_cbvSrvUavAllocator.WriteSrv(res.Get(), &srvd, srv.GetCpuHandle());
    srv.CopyToGpu();

    DescriptorAllocation uav = m_cbvSrvUavAllocator.Allocate(1);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format        = format;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    m_cbvSrvUavAllocator.WriteUav(res.Get(), nullptr, &uavd, uav.GetCpuHandle());
    uav.CopyToGpu();

    // The slot's pool record: the one it already has (a rebuild keeps its id) or a new one, exactly as
    // CreateTexture takes an id. Only the id is new; the record holds the resource and the views made
    // above.
    bool registered = true;
    {
        std::lock_guard<std::mutex> lock(m_resourceMutex);
        if (!hdr.tex.IsValid())
        {
            uint32_t id = ~0u;
            if (!m_textureFreeList.empty())
            {
                id = m_textureFreeList.back();
                m_textureFreeList.pop_back();
            }
            else if (ENGINE_CHECK_ONCE(m_texturePool.size() < m_texturePool.capacity(),
                         "CreateHdrTarget: texture pool full (%zu) - slot %u gets no target. "
                         "Raise GraphicsDX12::kMaxTexturePool.", m_texturePool.capacity(), slot))
            {
                id = static_cast<uint32_t>(m_texturePool.size());
                m_texturePool.emplace_back();
            }
            if (id != ~0u)
            {
                m_texturePool[id] = TextureDX12{};
                m_texturePool[id].generation = NextTextureGeneration();
                hdr.tex.handleId = id;
                hdr.tex.type      = RHI::GPUResource::Type::Texture;
            }
            registered = id != ~0u;
        }
        if (registered)
        {
            TextureDX12& rec = m_texturePool[hdr.tex.handleId];
            rec.resource = std::move(res);
            rec.rtv      = rtv;
            rec.srv      = srv;
            rec.uav      = uav;
            rec.state    = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }
    if (!registered)
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        auto& dr = PendingReleaseBucket();
        dr.descriptors.push_back(rtv);
        dr.descriptors.push_back(srv);
        dr.descriptors.push_back(uav);
        dr.resources.push_back(std::move(res));
        return;
    }
    RHI::TextureDesc& d = hdr.tex.desc;
    d            = RHI::TextureDesc{};
    d.type       = RHI::TextureDesc::Type::TEXTURE_2D;
    d.format     = kHdrSceneColorFormat;
    d.bindFlags = RHI::BindFlag::SHADER_RESOURCE | RHI::BindFlag::RENDER_TARGET |
                   RHI::BindFlag::UNORDERED_ACCESS;
    d.width      = width;
    d.height     = height;
    d.clear      = RHI::ClearValue::Color(0.f, 0.f, 0.f, 0.f);
    d.layout     = RHI::ResourceState::RENDERTARGET;
    // The resource's own D3D12 name (SetName above): the frame capture's journal knows it by this, and
    // the graph's import is matched to it.
    static const char* const kSlotNames[] = {
        "GraphicsDX12.HdrEditorViewport", "GraphicsDX12.HdrTarget[1]", "GraphicsDX12.HdrTarget[2]",
        "GraphicsDX12.HdrTarget[3]",      "GraphicsDX12.HdrTarget[4]", "GraphicsDX12.HdrTarget[5]",
        "GraphicsDX12.HdrView[1]",        "GraphicsDX12.HdrView[2]",   "GraphicsDX12.HdrView[3]" };
    static_assert(sizeof(kSlotNames) / sizeof(kSlotNames[0]) == kMaxHdrTargets,
                  "one D3D12 name per HDR slot");
    d.debugName = kSlotNames[slot];
    hdr.w = width; hdr.h = height;
    // hdr.clear is deliberately NOT reset: the clear colour is per-frame state pushed by
    // SetHdrClearColor and must survive a target rebuild.
}

void GraphicsDX12::DestroyHdrTarget(uint32_t slot)
{
    if (!ENGINE_CHECK(slot < kMaxHdrTargets,
                      "DestroyHdrTarget: slot %u out of range (kMaxHdrTargets=%u)",
                      slot, kMaxHdrTargets))
        return;
    ReleaseHdrTarget(slot, /*releaseId=*/true);
}

void GraphicsDX12::ReleaseHdrTarget(uint32_t slot, bool releaseId)
{
    HdrTargetBinding& hdr = m_hdrTargets[slot];
    if (!hdr.tex.IsValid()) return;
    TextureDX12& rec = m_texturePool[hdr.tex.handleId];
    // Defer: GPU may still be reading the resource / its descriptors
    // from frames still in flight. Freeing now would let the descriptor slots
    // be reused and the resource destroyed while GPU work is pending.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    auto& dr = PendingReleaseBucket();
    if (rec.rtv.IsValid()) { dr.descriptors.push_back(rec.rtv); rec.rtv = {}; }
    if (rec.srv.IsValid()) { dr.descriptors.push_back(rec.srv); rec.srv = {}; }
    if (rec.uav.IsValid()) { dr.descriptors.push_back(rec.uav); rec.uav = {}; }
    if (rec.resource)      { dr.resources.push_back(std::move(rec.resource)); }
    rec.state = D3D12_RESOURCE_STATE_COMMON;
    // The id goes back with the frame-stamped bucket, as DestroyTexture's does
    // (its bindless-table entry, never written, is nulled again on the way).
    if (releaseId)
    {
        dr.textureSlots.push_back(hdr.tex.handleId);
        hdr.tex.Reset();
    }
    // hdr.w/h are deliberately left alone: GetRenderWidth() reads them, and zeroing them would make it
    // answer with the WINDOW size while CreateHdrTarget is between destroy and rebuild. ResizeHdrTarget
    // carries the "no resource" term instead, so a destroyed slot still rebuilds.
}

TextureDX12* GraphicsDX12::HdrRecord(uint32_t slot)
{
    const RHI::Texture& t = m_hdrTargets[slot].tex;
    if (!t.IsValid()) return nullptr;
    TextureDX12& rec = m_texturePool[t.handleId];
    return rec.resource ? &rec : nullptr;
}

const TextureDX12* GraphicsDX12::HdrRecord(uint32_t slot) const
{
    const RHI::Texture& t = m_hdrTargets[slot].tex;
    if (!t.IsValid()) return nullptr;
    const TextureDX12& rec = m_texturePool[t.handleId];
    return rec.resource ? &rec : nullptr;
}

RHI::Texture GraphicsDX12::GetHdrTexture(uint32_t slot) const
{
    if (slot >= kMaxHdrTargets || !HdrRecord(slot)) return {};
    return m_hdrTargets[slot].tex;
}

ID3D12Resource* GraphicsDX12::GetHdrSceneResource() const
{
    const TextureDX12* rec = HdrRecord();
    return rec ? rec->resource.Get() : nullptr;
}

// Slot hand-over. Both ENGINE_CHECKs are load-bearing: see the contract on PushHdrTarget in the
// header. A refused Push leaves the depth untouched, so the matching Pop refuses too rather than
// restoring a slot nobody pushed.
bool GraphicsDX12::PushHdrTarget(uint32_t slot)
{
    // EVERY refusal below returns FALSE, and the caller must not touch the HDR
    // target after one. A refused push leaves m_hdrCurrent where it was - slot
    // 0, the main viewport - so the caller's next SetRenderTargetToHdr would
    // clear the live scene and leave slot 0's state tracker somewhere the graph
    // did not put it. That is a rendering corruption with no debug-layer
    // message, which is why this is a return value rather than a comment.
    if (!ENGINE_CHECK(!m_parallelPassRecording.load(std::memory_order_acquire),
                      "PushHdrTarget(%u) called inside a RenderGraph Phase B window. Handing "
                      "the HDR binding over while passes record in parallel swaps the state "
                      "tracker under barriers those passes have already recorded. Wrap the "
                      "graph's Execute() from the OUTSIDE.", slot))
        return false;
    if (!ENGINE_CHECK(slot < kMaxHdrTargets,
                      "PushHdrTarget: slot %u out of range (kMaxHdrTargets=%u)",
                      slot, kMaxHdrTargets))
        return false;
    const int depth = m_hdrScopeDepth.load(std::memory_order_relaxed);
    if (!ENGINE_CHECK(depth < static_cast<int>(kMaxHdrTargets),
                      "PushHdrTarget: scope depth %d would overflow the restore stack (%u)",
                      depth, kMaxHdrTargets))
        return false;
    m_hdrScopeStack[depth] = m_hdrCurrent;
    m_hdrCurrent           = slot;
    m_hdrScopeDepth.store(depth + 1, std::memory_order_release);
    return true;
}

void GraphicsDX12::PopHdrTarget()
{
    if (!ENGINE_CHECK(!m_parallelPassRecording.load(std::memory_order_acquire),
                      "PopHdrTarget called inside a RenderGraph Phase B window - same hazard "
                      "as PushHdrTarget. Wrap the graph's Execute() from the OUTSIDE."))
        return;
    const int depth = m_hdrScopeDepth.load(std::memory_order_relaxed);
    if (!ENGINE_CHECK(depth > 0,
                      "PopHdrTarget without a matching PushHdrTarget (depth=%d)", depth))
        return;
    m_hdrCurrent = m_hdrScopeStack[depth - 1];
    m_hdrScopeDepth.store(depth - 1, std::memory_order_release);
}

// ==== Frame lifecycle ====

void GraphicsDX12::WaitForNextFrameSlot()
{
    // FRAME-BOUNDARY GUARD: refuse, do not execute.
    // Mid-frame this would reassign m_frameIndex under the open command lists and leave
    // m_frameSlotPrepared latched past EndFrame. The NEXT frame then skips its slot fence wait (in
    // AcquireUploadSlot's dxgi path and in BeginFrame), so per-frame CPU upload writes land on buffers
    // the GPU is still reading: intermittent skinned-mesh flicker, never a crash.
    if (!ENGINE_CHECK_ONCE(!m_inFrame,
            "WaitForNextFrameSlot() called between BeginFrame and EndFrame ??refused. "
            "It has no meaning mid-frame and would latch m_frameSlotPrepared, making the "
            "NEXT frame skip its slot fence wait (silent per-frame upload tearing). "
            "Call it once per frame from the main loop before BeginFrame (App::Run's "
            "Animation phase), never from a pass or an ImGui handler."))
        return;

    // Idempotent within a frame: BeginFrame also calls this, and clears the flag once it has consumed
    // the prepared slot.
    if (m_frameSlotPrepared) return;

    // The back-buffer index is still sampled here for BeginFrame's present barrier and RTV pick; it
    // selects no upload ring (m_uploadSlotRender does).
    m_surface->SyncBackBufferIndex();

    // Retire the slot the RENDER side is about to write: in dxgi mode the back-buffer index, in frame
    // mode the slot BeginFrame(F) published from the monotonic counter.
    const uint32_t slot = m_slotKeyDxgi
                        ? m_surface->BackBufferIndex()
                        : m_uploadSlotRender.load(std::memory_order_relaxed);
    WaitSlotRetired(slot);

    m_frameSlotPrepared = true;
}

// A task that completes once every queue has passed the fences recorded the last time @p slot was
// used (empty when the slot is unused or already retired). WaitSlotRetired and the async
// AcquireUploadSlot share it, so there is ONE definition of "this slot is free". Sets
// *eventRegistered when a D3D12 event was armed.
// All THREE queues, not just graphics: the compute and copy snapshots keep async-compute work (DDGI)
// from being overwritten mid-flight.
Sys::TaskHandle GraphicsDX12::RequestSlotRetirement(uint32_t slot, bool* eventRegistered)
{
    if (eventRegistered) *eventRegistered = false;
    if (TaskSystem::OnWorkerThread() || slot >= kFrameCount)
        throw std::logic_error("Slot retirement snapshot requires a valid owner slot");
    const auto& s = m_slotFence[slot];
    if (s.framePlus1.load(std::memory_order_acquire) == 0) return {};
    const std::array<uint64_t, 3> targets{s.gfx, s.compute, s.copy};
    std::array<Microsoft::WRL::ComPtr<ID3D12Fence>, 3> fences{m_fence, m_queueFences[1], m_queueFences[2]};
    std::array<uint64_t, 3> alreadyCompleted{};
    for (size_t i = 0; i < 3; ++i) {
        if (!targets[i]) continue;
        if (!fences[i]) throw std::runtime_error("Missing GPU retirement fence");
        alreadyCompleted[i] = fences[i]->GetCompletedValue();
    }
    bool ready = false;
    try { ready = Sys::FenceCompletionQueue::Reached(alreadyCompleted, targets); }
    catch (...) { ReportDeviceRemoved("slot retirement"); throw; }
    // Avoid event allocation/callback round-trip for already-retired slots.
    // Verification can force the real D3D12 event bridge even on a fast GPU.
    if (ready && !Sys::CommandLine::Get().Has("-gpueventverify")) return {};
    Microsoft::WRL::ComPtr<ID3D12Device1> eventDevice;
    ThrowIfFailed(m_device.As(&eventDevice));
    auto retirement = m_gpuCompletions.Request([targets, fences, eventDevice](HANDLE event) {
        std::array<ID3D12Fence*, 3> active{};
        std::array<uint64_t, 3> values{};
        UINT count = 0;
        for (size_t i = 0; i < 3; ++i) {
            if (!targets[i]) continue;
            if (!fences[i]) throw std::runtime_error("Missing GPU retirement fence");
            active[count] = fences[i].Get(); values[count++] = targets[i];
        }
        if (count == 0) {
            if (!SetEvent(event)) throw std::runtime_error("GPU completion event signal failed");
        } else {
            ThrowIfFailed(eventDevice->SetEventOnMultipleFenceCompletion(active.data(), values.data(),
                count, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, event));
        }
    }, [targets, fences] {
        std::array<uint64_t, 3> completed{};
        for (size_t i = 0; i < 3; ++i) {
            if (!targets[i]) continue;
            if (!fences[i]) throw std::runtime_error("Missing GPU retirement fence");
            completed[i] = fences[i]->GetCompletedValue();
        }
        if (!Sys::FenceCompletionQueue::Reached(completed, targets))
            throw std::runtime_error("GPU event signalled before slot retirement");
    });
    if (eventRegistered) *eventRegistered = true;
    return retirement;
}

void GraphicsDX12::WaitSlotRetired(uint32_t slot)
{
    if (slot >= kFrameCount) return;
    const FrameSlotFence& s = m_slotFence[slot];

    // Acquire: pairs with EndFrame's release store of framePlus1, so seeing a
    // frame here guarantees the three values below belong to it. 0 = the slot
    // has never been used (the first kFrameCount frames after startup, and after
    // a full idle): nothing to wait for.
    if (s.framePlus1.load(std::memory_order_acquire) == 0) return;

    static const bool s_bridge = !Sys::CommandLine::Get().Has("-framepreparelegacy");
    if (s_bridge) {
        bool eventRegistered = false;
        auto retirement = RequestSlotRetirement(slot, &eventRegistered);
        {
            ZoneScopedN("TaskGraph.GpuEventAwait");
            PROF_WAIT(WaitGpu, "TaskGraph.GpuEventAwait");
            // The owner blocks here for synchronous callers; the GPU-event callback completes the task without a
            // Poll.
            try { retirement.Wait(); }
            catch (...) { ReportDeviceRemoved("slot retirement"); throw; }
        }
        PollGpuCompletions(); // reclaim completed registrations, not drive completion
        if (Sys::CommandLine::Get().Has("-materialcpuverify"))
            LOG_INFO("[GPU_COMPLETION] slot=%u gfx=%llu compute=%llu copy=%llu match=1 event=%u", slot,
                static_cast<unsigned long long>(s.gfx), static_cast<unsigned long long>(s.compute),
                static_cast<unsigned long long>(s.copy), unsigned(eventRegistered));
    } else {
        WaitFence(m_fence.Get(),         s.gfx);
        WaitFence(m_queueFences[1].Get(), s.compute);
        WaitFence(m_queueFences[2].Get(), s.copy);
    }
}

// DX12_SLOTAUDIT=1 reporter. One line per distinct CALLER, not per call site: GetFrameIndex() is one
// inline body shared by ~70 call sites, so a once-per-site report would name whichever ran first and
// hide the rest. Return-address keying gives the inventory the game-side re-key needs.
// Its own table, not Sys::ThreadCtx::FirstTimeFromCaller's: that one is shared by the ECS audit, and
// two inventories in 64 entries silently truncate whichever runs second.
void GraphicsDX12::AuditSlotCaller(const void* ret) const
{
    static constexpr int    kMax = 64;
    static const void*      s_seen[kMax]{};
    static std::atomic<int> s_count{ 0 };
    const int n = s_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < kMax; ++i)
        if (s_seen[i] == ret) return;
    if (n >= kMax) return;
    s_seen[n] = ret;
    s_count.store(n + 1, std::memory_order_relaxed);

    LOG_INFO("[SPLITAUDIT][SLOT] game-phase GetFrameIndex() (should be GetUploadSlot) "
             "gameSlot=%u renderSlot=%u  <- %s",
             m_uploadSlotGame.load(std::memory_order_relaxed),
             m_uploadSlotRender.load(std::memory_order_relaxed),
             Sys::CrashReport::SymbolizeAddress(ret).c_str());
}

// The mirror of AuditSlotCaller: GetUploadSlot() asked from inside Sys::ThreadCtx::Render. Its own
// de-duplication table, for the same reason.
// The COUNTER is bumped on every fire, before the de-duplication, because the animoverlap_* packs
// bound it ("conflicts=0"); the log line is for the human who fixes the call site.
void GraphicsDX12::AuditGameSlotCaller(const void* ret) const
{
    m_gameSlotFromRender.fetch_add(1, std::memory_order_relaxed);

    static constexpr int    kMax = 64;
    static const void*      s_seen[kMax]{};
    static std::atomic<int> s_count{ 0 };
    const int n = s_count.load(std::memory_order_relaxed);
    for (int i = 0; i < n && i < kMax; ++i)
        if (s_seen[i] == ret) return;
    if (n >= kMax) return;
    s_seen[n] = ret;
    s_count.store(n + 1, std::memory_order_relaxed);

    LOG_WARNING("[SPLITAUDIT][GAMESLOT] render-half GetUploadSlot() (should be "
                "GetFrameIndex) gameSlot=%u renderSlot=%u  <- %s",
                m_uploadSlotGame.load(std::memory_order_relaxed),
                m_uploadSlotRender.load(std::memory_order_relaxed),
                Sys::CrashReport::SymbolizeAddress(ret).c_str());
}

void GraphicsDX12::AcquireUploadSlot(uint64_t frame)
{
    AcquireUploadSlotImpl(frame, false);
}

Sys::OwnerTask GraphicsDX12::AcquireUploadSlotAsync(uint64_t frame)
{
    // What async acquisition needs is that nothing moves the slot it claims while it claims it;
    // m_inFrame is only a proxy for that. -animoverlap records frame F-1 on the render-owner lane while
    // the game half claims F's slot, so SetConcurrentUploadAcquire relaxes the proxy. With m_inFrame true
    // the following still hold:
    //   * F and F-1 are different slots (kFramesInFlight = 3); the SLOTFENCE check in
    //     AcquireUploadSlotImpl says so when it stops being true.
    //   * the TENANT re-check below, the real invariant: only EndFrame writes
    //     m_slotFence[x].framePlus1, and EndFrame(F-1) writes slot (F-1)%3, not the one awaited.
    //   * the window stamp: m_uploadSlotOpenFrame names the frame that opened it and EndFrame closes it
    //     by CAS, so opening F's window while EndFrame(F-1) closes F-1's is correct.
    //   * m_frameIndex / m_frameSlotPrepared / the per-slot allocators are untouched on the frame-keyed
    //     path; the dxgi path, which reads them, is still refused.
    //   * GetUploadSlot() readers on the render half WOULD break: -animoverlapverify arms the mirror
    //     audit, and [ANIM_OVERLAP] conflicts= bounds it at zero.
    // Refused by default: the relaxation is opt-in through SetConcurrentUploadAcquire.
    if (m_slotKeyDxgi || (m_inFrame && !m_concurrentUploadAcquire))
        throw std::logic_error("Async upload acquisition requires an idle frame-keyed device");
    const uint32_t slot = static_cast<uint32_t>(frame % kFrameCount);
    const uint64_t tenant = m_slotFence[slot].framePlus1.load(std::memory_order_acquire);
    bool eventRegistered = false;
    auto retired = RequestSlotRetirement(slot, &eventRegistered);
    // A removal seen by the GPU-event callback arrives here as its exception;
    // ReportDeviceRemoved is a no-op for any other failure.
    try { co_await retired; }
    catch (...) { ReportDeviceRemoved("upload slot retirement"); throw; }
    if (m_inFrame && !m_concurrentUploadAcquire)
        throw std::logic_error("A frame opened while an upload slot was being acquired");
    if (tenant != m_slotFence[slot].framePlus1.load(std::memory_order_acquire))
        throw std::logic_error("Upload slot tenant changed during retirement await");
    PollGpuCompletions();
    AcquireUploadSlotImpl(frame, true);
    if (Sys::CommandLine::Get().Has("-materialcpuverify")) {
        const auto& s = m_slotFence[slot];
        if (tenant)
            LOG_INFO("[GPU_COMPLETION] slot=%u gfx=%llu compute=%llu copy=%llu match=1 event=%u", slot,
                static_cast<unsigned long long>(s.gfx), static_cast<unsigned long long>(s.compute),
                static_cast<unsigned long long>(s.copy), unsigned(eventRegistered));
        LOG_INFO("[FRAME_SLOT_ACQUIRED] frame=%llu slot=%u async=1 match=1",
            static_cast<unsigned long long>(frame), slot);
    }
}

void GraphicsDX12::AcquireUploadSlotImpl(uint64_t frame, bool retirementComplete)
{
    // In dxgi mode the game side has no independent slot: take the back-buffer path, so the physical
    // slot sequence is the back-buffer index.
    if (m_slotKeyDxgi)
    {
        WaitForNextFrameSlot();
        const uint32_t backBuffer = m_surface->BackBufferIndex();
        m_uploadSlotGame.store(backBuffer, std::memory_order_relaxed);
        // Physical-slot selection still acquires this logical frame. Leaving
        // the stamp at zero breaks parity checks and completed-frame ownership.
        m_gameFrame = frame;
        m_gameSlotStamp.store((frame << 8) | uint64_t(backBuffer),
                              std::memory_order_release);
        m_uploadSlotOpenFrame.store(frame, std::memory_order_release);
        // WaitForNextFrameSlot retired this back buffer's previous frame.
        StreamUploaderOpenSlot(frame, backBuffer);
        return;
    }

    const uint32_t slot = static_cast<uint32_t>(frame % kFrameCount);

    // The tenant of this slot must be frame F-kFrameCount. Anything newer means the render side has not
    // caught up and the game thread is about to overwrite a slot still being recorded from.
    const uint64_t tenantPlus1 = m_slotFence[slot].framePlus1.load(std::memory_order_acquire);
    if (tenantPlus1 != 0)
    {
        const uint64_t tenant = tenantPlus1 - 1;
        ENGINE_CHECK_ONCE(tenant + kFrameCount <= frame,
            "[SPLITCHECK][SLOTFENCE] frame %llu wants slot %u but its tenant is frame "
            "%llu (expected <= %llu) — the render side is more than %u frames behind "
            "and this write would land on a slot still in use.",
            static_cast<unsigned long long>(frame), slot,
            static_cast<unsigned long long>(tenant),
            static_cast<unsigned long long>(frame) - kFrameCount, kFrameCount);
    }

    if (!retirementComplete) WaitSlotRetired(slot);
    m_uploadSlotGame.store(slot, std::memory_order_relaxed);
    m_gameFrame      = frame;
    // LAST, and with release: the render half reads this ONE value when it has
    // to compare the two together. See the member's declaration.
    m_gameSlotStamp.store((frame << 8) | uint64_t(slot), std::memory_order_release);
    // The slot's previous tenant has retired: the game side may write it until
    // the EndFrame of THIS frame submits it.
    m_uploadSlotOpenFrame.store(frame, std::memory_order_release);
    // Retired - by WaitSlotRetired above, or by the async path's await before it called this - so this
    // slot's stream ring may be reused, its staging released, and the frame opened.
    StreamUploaderOpenSlot(frame, slot);
}

RHI::CommandList GraphicsDX12::BeginFrame()
{
    ZoneScoped;
    PROF_SCOPE("GraphicsDX12.BeginFrame");
    if (m_deviceExecution.Enabled() && !m_deviceExecution.Owns())
        throw std::logic_error("BeginFrame requires its device execution token");

    // FRAME-BOUNDARY GUARD: refuse, do not execute.
    // Re-entering would reopen the primary command list (dropping everything recorded so far) and emit a
    // SECOND PRESENT->RENDER_TARGET barrier on a back buffer already in RENDER_TARGET, which the debug
    // layer rejects and the retail driver silently mis-tracks. Pool entry 0 is the primary and is still
    // open, so handing it back keeps the caller recording into the right list.
    if (!ENGINE_CHECK_ONCE(!m_inFrame,
            "BeginFrame() re-entered without a matching EndFrame ??refused, returning the "
            "already-open primary command list. There must be exactly one BeginFrame/EndFrame "
            "pair per iteration of App::Run."))
        return RHI::CommandList{ 0 };

    // Backstop for Resize / SetFullscreen requests latched mid-frame. Deliberately BEFORE
    // WaitForNextFrameSlot: servicing rebuilds the swap chain and reassigns m_frameIndex, so a slot
    // prepared earlier (AcquireUploadSlot's dxgi path calls WaitForNextFrameSlot in the Animation phase)
    // would no longer describe the buffer about to be rendered into. Clearing m_frameSlotPrepared makes
    // the wait below re-run against the new index, at no cost: ServicePendingSwapChainOps has just idled
    // the GPU.
    // PendingSwapChainOp is not a word, so HasPendingSwapChainOp reads it under the surface's lock and
    // releases it before ServicePendingSwapChainOps takes the same lock.
    const bool swapChainOpPending = m_surface->HasPendingSwapChainOp();
    if (swapChainOpPending)
    {
        if (std::this_thread::get_id() == m_deviceThread)
        {
            m_surface->ServicePendingSwapChainOps();
            m_frameSlotPrepared = false;
        }
        else
        {
            // On the render-owner lane: leave the op latched for the main thread's next loop top (App::Run
            // services it there, outside any frame). Do not apply it here: SetWindowPos / SetFullscreenState on
            // the main thread's window SEND messages and block until main pumps, and main can be waiting for
            // this lane without pumping - a deadlock. The frame renders at the old size; the op lands one frame
            // later.
            static std::atomic<bool> s_deferredLogged{ false };
            if (!s_deferredLogged.exchange(true, std::memory_order_relaxed))
                LOG_INFO("[RENDER_OWNER] a swap-chain op was pending at a lane BeginFrame; "
                         "left for the main thread's loop top");
        }
    }

    // Publish the RENDER side's upload slot before anything can read it.
    // Serial: the frame being rendered IS the frame the game side just acquired.
    // Threaded: the render half named it from its packet, and reading
    // m_gameFrame here would read whatever the game thread has moved on to.
    const uint64_t srcFrame = m_renderFrameReqSet.load(std::memory_order_acquire)
                            ? m_renderFrameReq.load(std::memory_order_acquire)
                            : m_gameFrame.load(std::memory_order_acquire);
    m_renderFrame.store((srcFrame >= m_slotLag) ? (srcFrame - m_slotLag) : srcFrame,
                        std::memory_order_relaxed);
    m_uploadSlotRender.store(
        m_slotKeyDxgi ? m_surface->BackBufferIndex()
                      : static_cast<uint32_t>(m_renderFrame.load(std::memory_order_relaxed) % kFrameCount),
        std::memory_order_release);

    if (m_deviceExecution.Enabled()) {
        if (!m_deviceExecution.Matches(m_renderFrame.load(std::memory_order_relaxed),
                                      m_uploadSlotRender.load(std::memory_order_relaxed)))
            throw std::logic_error("Device execution generation/upload slot mismatch");
        if (Sys::CommandLine::Get().Has("-materialcpuverify"))
            LOG_INFO("[DEVICE_EXECUTION] frame=%llu slot=%u match=1 upload=%u",
                static_cast<unsigned long long>(m_renderFrame.load(std::memory_order_relaxed)),
                m_uploadSlotRender.load(std::memory_order_relaxed), unsigned(&Upload() - m_uploadCtx));
    }

    // kFrameCount-deep pipelining: per-slot allocators and profiler readback key off the upload slot
    // (deferred releases off their frame stamp), so other slots stay untouched; WaitForPreviousFrame()
    // is the hard-sync path (destructor, resize, picking, capture helpers).
    // The game side already retired this slot in AcquireUploadSlot, so this call is normally a no-op; it
    // stays because BeginFrame must be correct on its own (the editor and the capture helpers reach it
    // without an App-side acquire).
    WaitForNextFrameSlot();
    m_frameSlotPrepared = false;
    // The slot's previous frame has retired: its transient constants are free.
    m_transientConstants.BeginFrame(m_uploadSlotRender.load(std::memory_order_relaxed));
    if (m_transientConstants.IsValid())
    {
        // [CBALLOC] once the scene is up (the 60th finished frame); the
        // destructor prints the session's summary.
        const TransientConstantAllocator::Stats s = m_transientConstants.GetStats();
        if (s.frames == 61)
            LOG_INFO("[CBALLOC] frame=60 lastBytes=%u peakBytes=%u capacity=%u allocations=%llu overflows=%llu",
                     s.lastTaken, s.peakTaken, s.capacity, static_cast<unsigned long long>(s.allocations),
                     static_cast<unsigned long long>(s.overflows));
    }

    // Slot parity: the game side claimed a slot in App's Animation phase (AcquireUploadSlot) and wrote
    // this frame's upload rings into it. If the render side reads another slot it reads another frame's
    // data, silently (no debug-layer message, no crash, intermittent skinned-mesh flicker).
    // Slot EQUALITY is not the invariant: it is a tautology on -slotkey=frame and false by design when a
    // render thread runs one frame behind. What must hold, checked separately because they fail
    // differently: the render side is on the game's frame or exactly one behind, never ahead (a logic
    // error); and the game does not lap the ring (data corruption: it overwrites the slot about to be
    // read).
    {
        const uint64_t gf = m_gameFrame.load(std::memory_order_acquire);
        // The lag the parity checks below actually see, kept for MaxObservedFrameLag() so "they never
        // fired" is a measurement. It reads 0 in both modes, by the park design: the game half is exclusive
        // and m_gameFrame advances only in AcquireUploadSlot during a game phase. So the parity checks are
        // tautologies there, and this value stops anyone claiming coverage they do not give.
        {
            const uint64_t rfv = m_renderFrame.load(std::memory_order_relaxed);
            const uint64_t d   = (gf >= rfv) ? (gf - rfv) : 0;
            if (d > m_maxObservedFrameLag) m_maxObservedFrameLag = d;
        }
        const uint64_t rf = m_renderFrame.load(std::memory_order_relaxed);
        ENGINE_CHECK_ONCE(rf <= gf && (gf - rf) <= 1,
            "[SPLITCHECK][SLOTPARITY] render is on frame %llu while the game is on "
            "%llu - the render half must be on the game's frame or exactly one "
            "behind, never ahead (game slot %u, render slot %u)",
            static_cast<unsigned long long>(rf), static_cast<unsigned long long>(gf),
            m_uploadSlotGame.load(std::memory_order_relaxed),
            m_uploadSlotRender.load(std::memory_order_relaxed));
        ENGINE_CHECK_ONCE(rf > gf || (gf - rf) < kFrameCount,
            "[SPLITCHECK][SLOTPARITY] the game half is %llu frames ahead of the "
            "render half with only %u upload slots - it has already overwritten "
            "the slot the render half is about to read",
            static_cast<unsigned long long>(gf - rf), kFrameCount);
        // Only meaningful while the halves are on the SAME frame; on different frames different slots are
        // the correct answer.
        // The pair comes from ONE atomic (m_gameSlotStamp): sampling m_gameFrame and m_uploadSlotGame
        // separately can read the new slot beside the old frame and report a violation the game half never
        // committed. gf above still comes from m_gameFrame because the two checks before this one are about
        // the frame alone and one-sided (rf <= gf).
        const uint64_t gameStamp = m_gameSlotStamp.load(std::memory_order_acquire);
        const uint64_t stampFrame = gameStamp >> 8;
        const uint32_t stampSlot  = static_cast<uint32_t>(gameStamp & 0xFFull);
        ENGINE_CHECK_ONCE(rf != stampFrame ||
                          stampSlot == m_uploadSlotRender.load(std::memory_order_relaxed),
            "[SPLITCHECK][SLOTPARITY] same frame %llu but game wrote upload slot %u "
            "and render is about to read slot %u",
            static_cast<unsigned long long>(stampFrame), stampSlot,
            m_uploadSlotRender.load(std::memory_order_relaxed));
    }

    // Free every release whose stamped frame has provably retired. The wait above proved only that this
    // slot's tenant, frame r - kFrameCount, is done; r-1 and r-2 may still be executing, so that - not
    // "the slot came round" - is the predicate.
    {
        const uint64_t r = m_renderFrame.load(std::memory_order_relaxed);
        if (r >= kFrameCount)
            DrainReleasesUpTo(r - kFrameCount);
    }
    // The profiler's descriptor gauge: shader-visible CBV/SRV/UAV slots in use
    // right now (not the allocator's high-water cursor).
    Diag::Prof::SetGauge(Diag::Prof::Gauge::DescriptorsLive, m_cbvSrvUavAllocator.GetStaticLive());
    Diag::Prof::SetGauge(Diag::Prof::Gauge::DescriptorsCapacity, m_cbvSrvUavAllocator.GetStaticCapacity());

    // Drain fence-keyed releases whose target fence value has completed: independent of the
    // frame-stamped drain, so resources tied to async-compute fences (DDGI) can outlive kFrameCount
    // without a WaitForPreviousFrame stall.
    ProcessFenceKeyedReleases();

#if defined(WITH_HARNESS) && WITH_HARNESS
    {
        // -rhi_lifetime_probe[=F]: the resource-lifetime guards exercised on purpose at render frame F
        // (GraphicsDX12_LifetimeProbe.cpp). Called here, after both drains: its second stage reads what
        // they put on the free lists.
        static const bool s_lifetimeProbe = Sys::CommandLine::Get().Has("-rhi_lifetime_probe");
        if (s_lifetimeProbe) RunLifetimeProbe();
    }
#endif

    // Fold this frame's want count (debug HUD) together with whatever a direct
    // owner (-gpuprofile, the editor's Profiler window) last wrote. MUST come
    // before the first test of `enabled` below, or a want raised this frame
    // takes effect one frame late.
    ReconcileGPUProfilerEnable();

    // Read back this slot's GPU timestamps, resolved kFrameCount frames ago.
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.BeginFrame(m_uploadSlotRender.load(std::memory_order_relaxed));

#ifdef TRACY_ENABLE
    // Advance each GPU context's frame: snapshots the frame's query range and
    // signals the context's payload fence on its queue (after prior submits).
    if (m_tracy)
        for (auto* c : m_tracy->ctx) if (c) c->NewFrame();
#endif

    // From here on a command list is open against the back buffer: the device
    // is "in a frame" and every frame-boundary-only entry point must divert.
    // Set AFTER the deferred-release drains above (those are the last legal
    // hard-sync work of the boundary) and BEFORE the primary CL exists.
    m_inFrame = true;

    // Reset the pool: this slot's entries are no longer in flight.
    m_commandListCount = 0;

    // Journal this whole frame when ArmFrameJournal asked before it
    // (Graphics/FrameSnapshot.h). Latched here, before the primary list
    // exists, so its PRESENT -> RENDER_TARGET barrier is journaled too. A
    // snapshot of an earlier frame that nobody took is dropped.
    {
        const bool journal = m_journalArmed.exchange(false, std::memory_order_acq_rel);
        m_journalOn.store(journal, std::memory_order_relaxed);
        g_journalDevice.store(journal ? this : nullptr, std::memory_order_release);
        m_frameSnapshot.reset();
    }

    // DX12_DEPTHTRACE: the roster is keyed by POOL SLOT, so it is only
    // meaningful between this reset and the next one. Cleared here, with the
    // pool, rather than at EndFrame - a frame that throws past EndFrame would
    // otherwise carry last frame's names onto this frame's slots.
    GfxTrace::BeginFrame(m_renderFrame.load(std::memory_order_relaxed));

#if defined(WITH_HARNESS) && WITH_HARNESS
    // -late_pso_at=N (fault injection): on render frame N compile a trivial compute shader with DXC and
    // build a PSO from it through CreatePipelineState, then release both - one shaderCompiles and one
    // psoCreates late in the run, the hitch frames.jsonl's totals exist to show, made on purpose so a
    // "nothing compiles after frame M" pack can go red.
    {
        static const int s_latePsoAt = Sys::CommandLine::Get().GetInt("-late_pso_at", -1);
        if (s_latePsoAt >= 0 &&
            m_renderFrame.load(std::memory_order_relaxed) == static_cast<uint64_t>(s_latePsoAt))
        {
            static const char kSrc[] = "[numthreads(1, 1, 1)] void main() {}";
            DxcCompiler::CompileOptions opts;
            opts.sourceName = "late_pso.cs.hlsl";
            opts.stage      = RHI::ShaderStage::CS;
            const DxcCompiler::CompileResult res = DxcCompiler::Compile(kSrc, sizeof(kSrc) - 1, opts);
            RHI::Shader        cs;
            RHI::PipelineState pso;
            bool built = false;
            if (res.ok && CreateShader(RHI::ShaderStage::CS, res.dxil.data(), res.dxil.size(), cs))
            {
                RHI::PipelineStateDesc d{};
                d.cs  = &cs;
                built = CreatePipelineState(d, pso);
                if (pso.IsValid()) DestroyPipelineState(pso);
                DestroyShader(cs);
            }
            LOG_INFO("[LATE_PSO] frame=%d: compiled %s, built a PSO %s (-late_pso_at)",
                     s_latePsoAt, res.ok ? "ok" : "FAILED", built ? "ok" : "FAILED");
        }
    }
#endif

    // Open the primary command list (pool entry 0) for the GRAPHICS queue.
    RHI::CommandList primary = BeginCommandList(RHI::QueueType::GRAPHICS);
    SetCommandListName(primary, "Primary");
    // primary.internalId == 0 guaranteed (first allocation after reset).

    auto* cmd = GetPoolEntry(primary).GetCommandList();
    // The full-window viewport, and the back buffer PRESENT -> RENDER_TARGET.
    m_surface->BeginFrameRecording(cmd);

    return primary;
}

void GraphicsDX12::EndFrame()
{
    ZoneScoped;
    PROF_SCOPE("GraphicsDX12.EndFrame");
    if (m_deviceExecution.Enabled() &&
        !m_deviceExecution.Matches(m_renderFrame.load(std::memory_order_relaxed),
                                  m_uploadSlotRender.load(std::memory_order_relaxed)))
        throw std::logic_error("EndFrame requires its device execution token");

    // FRAME-BOUNDARY GUARD: refuse, do not execute.
    // A second EndFrame would emit another RENDER_TARGET->PRESENT barrier on a
    // back buffer already in PRESENT and re-submit an empty (or already
    // submitted) pool.
    if (!ENGINE_CHECK_ONCE(m_inFrame,
            "EndFrame() called without a matching BeginFrame ??refused. Exactly one "
            "BeginFrame/EndFrame pair per iteration of App::Run."))
        return;

    // Clear m_inFrame on EVERY exit path, exceptions included. The tail of this function is
    // ThrowIfFailed(Present(...)), which throws on device removal; with a plain `m_inFrame = false;` at
    // the bottom the flag would stay latched and every guarded entry point (resize, fullscreen, capture)
    // would refuse for the rest of the session.
    struct FrameExitGuard
    {
        std::atomic<bool>& flag;
        // RELEASE: everything this frame recorded must be visible to whoever
        // observes !m_inFrame and concludes the frame boundary is open.
        ~FrameExitGuard() { flag.store(false, std::memory_order_release); }
    } frameExitGuard{ m_inFrame };

    // What the shader-visible heap holds once the first frame is built: the boot half of the [BINDLESS]
    // kind statistics.
    if (!m_bindlessKindsLogged)
    {
        m_bindlessKindsLogged = true;
        LogBindlessKinds("first frame");
    }

    // Resolve GPU timestamp queries before closing the primary CL.
    auto* primaryCmd = m_commandLists[0]->GetCommandList();
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.ResolveQueries(primaryCmd, GetFrameIndex(),
                                     m_renderFrame.load(std::memory_order_relaxed));

    // The bindless debug lane's log, read back.
    PollBindlessValidation(primaryCmd);

    // The back buffer RENDER_TARGET -> PRESENT on the primary CL (or, for a
    // pending -ui_screenshot, the copy in its place): the surface's.
    m_surface->EndFrameRecording(primaryCmd);

    const uint32_t n = m_commandListCount;
    const auto& closeCmd = Sys::CommandLine::Get();
    static const bool s_requestCloseJobs = closeCmd.Has("-closejobs") || Sys::JobSplittingDefaultsEnabled();
    const bool closeJobs = s_requestCloseJobs && n >= 16 &&
        TaskSystem::Get().GetWorkerCount() > 1 && !TaskSystem::OnWorkerThread();
    ZoneNamedN(closeJobsZone, "CloseJobs.Graph", closeJobs);
    ZoneNamedN(closeSerialZone, "CloseJobs.Legacy", !closeJobs);

    // Command-list pool high-water, logged only when a frame beats the previous peak, so DX12Log.txt
    // shows how large the pool grows without a line per frame; a sudden jump is the first sign a pass
    // started allocating lists per object. A member so the editor's pool watermark panel can read it.
    // Do not merge it with m_commandListHighWater (the largest slot ever ALLOCATED, which never falls):
    // this is the most lists used in a SINGLE frame.
    if (n > m_commandListPeak)
    {
        m_commandListPeak = n;
        LOG_INFO("GraphicsDX12: command-list pool high-water %u lists in one frame", n);
    }

    // ---- Kahn's topological sort: the GPU submission order ----
    // An entry dep in waitFor of entry i means pool entry dep executes before pool entry i. The scratch
    // containers are members reused across frames: this is the hot submit path.
    {
    ZoneScopedN("EndFrame.Topology");
    m_submitInDegree.assign(n, 0);
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t dep : m_commandLists[i]->waitFor)
            if (dep < n) ++m_submitInDegree[i];

    m_submitReady.clear();
    for (uint32_t i = 0; i < n; ++i)
        if (m_submitInDegree[i] == 0) m_submitReady.push_back(i);

    m_submitOrder.clear();
    size_t readyHead = 0;
    while (readyHead < m_submitReady.size())
    {
        const uint32_t cur = m_submitReady[readyHead++];
        m_submitOrder.push_back(cur);
        // Reduce in-degree of every CL that depends on cur.
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t dep : m_commandLists[j]->waitFor)
                if (dep == cur && --m_submitInDegree[j] == 0)
                    m_submitReady.push_back(j);
    }

    if (m_submitOrder.size() != n)
    {
        LOG_ERROR("EndFrame: dependency cycle in command lists ??appending remaining in pool order");
        for (uint32_t i = 0; i < n; ++i)
            if (std::find(m_submitOrder.begin(), m_submitOrder.end(), i) == m_submitOrder.end())
                m_submitOrder.push_back(i);
    }

    // DX12_DEPTHTRACE: the ANSWER line. Everything above decides what order the
    // GPU sees; a barrier's StateBefore was baked at RECORD time. Printing the
    // sort's output beside the per-command-list barrier lines is what turns
    // "'cmd28' disagreed with the GPU" into "cmd28 ran before cmd26".
    GfxTrace::SubmitOrder(m_submitOrder.data(),
                          static_cast<uint32_t>(m_submitOrder.size()));
    if (GfxTrace::Enabled())
        for (uint32_t idx : m_submitOrder)
            GfxTrace::Edges(idx, m_commandLists[idx]->waitFor.data(),
                            static_cast<uint32_t>(m_commandLists[idx]->waitFor.size()));

    // ---- Submit in topological order, BATCHING same-queue runs ----
    // Each run of same-queue CLs is one ExecuteCommandLists + one trailing fence Signal; one ECL + Signal
    // per CL costs a fixed kernel-submission overhead each (with ~26 CLs a frame it dominated EndFrame's
    // self time). Same-queue order is kept by array order inside a batch and by submission order across
    // batches. Cross-queue order uses the per-queue fences, but a batch signals ONCE, so a later
    // cross-queue dependent waits for the whole batch: conservative, never incorrect (it is
    // topologically after every member).
    // The open batch flushes when the next CL targets a different queue (one open batch at a time), or
    // has a cross-queue dependency: its queue->Wait() must be recorded BEFORE the ECL that contains it,
    // and flushing is what assigns the fence value the Wait() targets.
    // Do not split the batch after every CL that is the SOURCE of a cross-queue dependency without a
    // profile that shows a win: it measured worse (the extra ECL + Signal per split cost more than the
    // sync it removed). CommandListDX12::submitBarrier opts one CL in.

    }
    auto closeList = [&](uint32_t idx) {
        ZoneNamedN(closeWorkerZone, "CloseJobs.CommandList", closeJobs);
        ZoneNamedN(closeOwnerZone, "EndFrame.CloseCommandList", !closeJobs);
        auto& cl = *m_commandLists[idx];
        if (cl.markerDepth > 0)
        {
            m_markerUnclosed.fetch_add(static_cast<uint32_t>(cl.markerDepth), std::memory_order_relaxed);
            while (cl.markerDepth > 0) { --cl.markerDepth; cl.GetCommandList()->EndEvent(); }
        }
        ThrowIfFailed(cl.GetCommandList()->Close());
    };
    if (closeJobs)
    {
        ZoneScopedN("EndFrame.ClosePhase");
        // All recording, including Phase C's tail writes and the primary
        // PRESENT barrier above, has finished. Each list has one closer.
        // Jobs borrow only stable pool / order storage and drain before submission
        // or unwind.
        const uint32_t lanes = (std::min)(4u, (std::min)(TaskSystem::Get().GetWorkerCount(), n / 8u));
        auto closeLane = [&](uint32_t lane) {
            ZoneScopedN("CloseJobs.Work");
            for (uint32_t k = uint64_t(n) * lane / lanes; k < uint64_t(n) * (lane + 1) / lanes; ++k)
                closeList(m_submitOrder[k]);
        };
        Sys::CpuTaskScope scope(lanes - 1);
        for (uint32_t lane = 1; lane < lanes; ++lane)
            scope.Submit([&, lane] { closeLane(lane); });
        closeLane(0);
        scope.Join();
    }
#if defined(WITH_HARNESS) && WITH_HARNESS
    static const bool s_verifyCloseJobs = closeCmd.Has("-closejobsverify");
    if (s_verifyCloseJobs && m_renderFrame.load(std::memory_order_relaxed) % 120 == 0)
        LOG_INFO("[CLOSE_JOBS] lists=%u graph=%u", n, closeJobs ? 1u : 0u);
#endif
    // CPU instants of this frame's first and last submission, for the engine
    // profiler's "was the GPU waiting for the CPU" test (ProfilerGpu.h).
    int64_t submitFirstNs = 0, submitLastNs = 0;

    // The graphics queue waits for the producers (a decoder) of this frame's queued GPU copies, ahead of
    // the StreamUpload list that reads their frames.
    StreamUploaderBeforeSubmit();

    // On a journaled frame the submit loop also records what it
    // did - every ExecuteCommandLists batch and every cross-queue Wait - for
    // BuildFrameSnapshot below. One bool test per batch / wait otherwise.
    const bool journal = m_journalOn.load(std::memory_order_relaxed);
    uint32_t submitPos = 0;   // lists handed to a batch so far (= index in m_submitOrder)
    if (journal)
    {
        m_journalBatches.clear();
        m_journalBatchOf.assign(n, GfxDiag::FrameSnapshot::kNone);
        m_journalWaits.resize(n);
        for (uint32_t i = 0; i < n; ++i) m_journalWaits[i].clear();
    }

    auto flushBatch = [&](UINT qi)
    {
        if (m_submitBatch.empty()) return;
        {
        ZoneScopedN("EndFrame.ExecuteCommandLists");
        PROF_SCOPE("EndFrame.ExecuteCommandLists");
        submitLastNs = Diag::Prof::NowNs();
        if (!submitFirstNs) submitFirstNs = submitLastNs;
        m_queues[qi]->ExecuteCommandLists(
            static_cast<UINT>(m_submitBatch.size()), m_submitBatch.data());
        }
        const uint64_t sigVal = ++m_queueFenceValues[qi];
        {
        ZoneScopedN("EndFrame.BatchSignal");
        ThrowIfFailed(m_queues[qi]->Signal(m_queueFences[qi].Get(), sigVal));
        }
        for (uint32_t bIdx : m_submitBatchIds)
            m_commandLists[bIdx]->signalFenceValue = sigVal;
        if (journal)
        {
            GfxDiag::FrameSnapshot::Batch batch;
            batch.queue       = static_cast<uint8_t>(qi);
            batch.count       = static_cast<uint32_t>(m_submitBatchIds.size());
            batch.firstSubmit = submitPos - batch.count;
            batch.signalValue = sigVal;
            for (uint32_t bIdx : m_submitBatchIds)
                if (bIdx < n) m_journalBatchOf[bIdx] = static_cast<uint32_t>(m_journalBatches.size());
            m_journalBatches.push_back(batch);
        }
        m_submitBatch.clear();
        m_submitBatchIds.clear();
    };

    UINT openQueue = UINT_MAX;   // queue of the currently-open batch
    for (uint32_t idx : m_submitOrder)
    {
        auto& cl = *m_commandLists[idx];
        const UINT qi = static_cast<UINT>(cl.queue);

        // Drain any debug-marker regions still open on this CL so PIX/RenderDoc
        // begin/end counts stay balanced at the queue level (a multi-CL worker
        // pass can leave a marker open on its primary while the matching end ran
        // on a different list). No-op for the common balanced case.
        if (!closeJobs) closeList(idx);

        bool crossDep = false;
        for (uint32_t depIdx : cl.waitFor)
        {
            if (depIdx >= n) continue;
            if (m_commandLists[depIdx]->queue != cl.queue) { crossDep = true; break; }
        }

        if (openQueue != UINT_MAX && (qi != openQueue || crossDep))
            flushBatch(openQueue);

        if (crossDep)
        {
            // Insert GPU-side Wait() for every dependency on a different queue.
            // Every cross-queue dep is already flushed at this point (deps are
            // topologically earlier, and only one batch is ever open), so its
            // signalFenceValue is valid.
            for (uint32_t depIdx : cl.waitFor)
            {
                if (depIdx >= n) continue;
                const auto& dep = *m_commandLists[depIdx];
                if (dep.queue != cl.queue && dep.signalFenceValue != 0)
                {
                    const UINT depQi = static_cast<UINT>(dep.queue);
                    ZoneScopedN("EndFrame.GPUQueueDependency");
                    ThrowIfFailed(m_queues[qi]->Wait(
                        m_queueFences[depQi].Get(), dep.signalFenceValue));
                    if (journal)
                        m_journalWaits[idx].push_back({ depIdx, static_cast<uint8_t>(depQi),
                                                        dep.signalFenceValue });
                }
            }
        }

        m_submitBatch.push_back(cl.commandLists[qi].Get());
        m_submitBatchIds.push_back(idx);
        ++submitPos;
        openQueue = qi;

        // Opt-in batch barrier. Batch members share ONE fence value, so a CL another queue waits on hands
        // that queue the completion of everything batched with it; closing the batch here gives this CL a
        // private value. Explicit rather than automatic (see the batching note above and
        // CommandListDX12::submitBarrier): it costs one ExecuteCommandLists + Signal per barrier, so it is
        // worth paying only where a consumer is genuinely blocked.
        if (cl.submitBarrier)
        {
            flushBatch(qi);
            openQueue = UINT_MAX;
        }
    }
    if (openQueue != UINT_MAX)
        flushBatch(openQueue);
    if (m_gpuProfiler.enabled)
        m_gpuProfiler.NoteSubmit(GetFrameIndex(), submitFirstNs, submitLastNs);

    // The StreamUpload list's place in the order just submitted is checked, and the frame's stream
    // copies are marked submitted (MustDeliver lets go of its bytes).
    StreamUploaderEndFrame();

    // The frame as data, while every list's journal, waitFor and
    // fence value still describe THIS frame and every resource it barriered is
    // still alive (releases are deferred past the GPU). Then the journal stops.
    if (journal)
    {
        ZoneScopedN("EndFrame.FrameSnapshot");
        BuildFrameSnapshot(n);
        m_journalOn.store(false, std::memory_order_relaxed);
        g_journalDevice.store(nullptr, std::memory_order_release);
    }

    // [GPUMARKERS] report: the 3rd frame with markers on (early proof for short
    // runs) and the 60th (the scene is up by then) say how much got named
    // (lists named / lists used); the first frame that had to close a region
    // itself (Begin and End on different lists, or an early return between
    // them) warns once, since the capture tree nests that region wrong.
    if (m_debugMarkers)
    {
        const uint32_t events   = m_markerEvents.exchange(0, std::memory_order_relaxed);
        const uint32_t named    = m_markerNamed.exchange(0, std::memory_order_relaxed);
        const uint32_t unclosed = m_markerUnclosed.exchange(0, std::memory_order_relaxed);
        const auto frame = static_cast<unsigned long long>(m_renderFrame.load(std::memory_order_relaxed));
        ++m_markerFrames;
        if (m_markerFrames == 3 || m_markerFrames == 60)
            LOG_INFO("[GPUMARKERS] frame=%llu events=%u namedLists=%u lists=%u unclosed=%u",
                     frame, events, named, n, unclosed);
        if (unclosed > 0 && !m_markerUnclosedWarned)
        {
            m_markerUnclosedWarned = true;
            LOG_WARNING("[GPUMARKERS] frame=%llu: EndFrame closed %u marker region(s) left open "
                        "on their list; a capture shows them nested under the next regions",
                        frame, unclosed);
        }
    }

#if defined(WITH_HARNESS) && WITH_HARNESS
    if (m_rdocApi)   // -rdcapture=N (see the constructor)
    {
        auto* rd = static_cast<RENDERDOC_API_1_1_2*>(m_rdocApi);
        const uint64_t f = m_renderFrame.load(std::memory_order_relaxed);
        // RenderDoc starts at the Present after the trigger, so arming it on
        // frame N-1 captures frame N, Present to Present.
        if (f + 1 == m_rdCaptureFrame)
            rd->TriggerCapture();
        if (!m_rdCaptureLogged && f > m_rdCaptureFrame && rd->GetNumCaptures() > 0)
        {
            m_rdCaptureLogged = true;
            uint32_t len = 0;
            const uint32_t idx = rd->GetNumCaptures() - 1;
            if (rd->GetCapture(idx, nullptr, &len, nullptr) && len > 0)
            {
                // +1: the header does not say whether len counts the NUL it writes.
                std::string path(len + 1, '\0');
                rd->GetCapture(idx, path.data(), &len, nullptr);
                path.resize(std::strlen(path.c_str()));
                LOG_INFO("[RDCAPTURE] wrote frame %llu: %s",
                         static_cast<unsigned long long>(m_rdCaptureFrame), path.c_str());
            }
        }
    }
#endif

    // Present: the surface's (SurfaceDX12::Present - the PCL markers around
    // exactly that call, the back-buffer capture's PNG, Frame Generation's
    // health window).
    m_surface->Present();

    // Signal the fence for this frame: the next BeginFrame waits on it.
    // EndFrame does NOT wait; only BeginFrame does (one wait per frame).
    const uint64_t fv = [&] {
        ZoneScopedN("EndFrame.FinalSignal");
        return SignalGraphicsFence();
    }();

    // ---- VRAM residency watch (always on, throttled) ----
    VideoMemory::Sample(m_factory.Get(), m_device.Get());

#ifdef TRACY_ENABLE
    // Harvest GPU timestamps whose payload fence has completed (non-blocking;
    // does nothing when no Tracy server is connected under TRACY_ON_DEMAND).
    {
    ZoneScopedN("EndFrame.TracyCollect");
    if (m_tracy)
        for (auto* c : m_tracy->ctx) if (c) c->Collect();
    }

    // ---- Per-frame memory plots (Tracy Plots pane) ----
    // VRAM from the DXGI budget query (what the OS resident-tracks for this process: every GPU heap -
    // textures, buffers, upload rings, RT/DS) + process RAM. Complements the heap-event hooks in
    // TracyMemory.cpp, which only see C++ operator-new traffic.
    {
        ZoneScopedN("EndFrame.MemoryPlots");
        // One-time: memory formatting for the plots + the adapter3 lookup.
        // A function-local static is fine: the engine has a single device.
        static Microsoft::WRL::ComPtr<IDXGIAdapter3> s_adapter3 =
            [this]() {
                Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
                if (m_factory && m_device)
                    m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(),
                                                 IID_PPV_ARGS(&a3));
                TracyPlotConfig("VRAM used",        tracy::PlotFormatType::Memory, false, true, 0);
                TracyPlotConfig("VRAM budget",      tracy::PlotFormatType::Memory, false, true, 0);
                TracyPlotConfig("VRAM shared",      tracy::PlotFormatType::Memory, false, true, 0);
                TracyPlotConfig("RAM working set",  tracy::PlotFormatType::Memory, false, true, 0);
                TracyPlotConfig("RAM commit",       tracy::PlotFormatType::Memory, false, true, 0);
                return a3;
            }();

        DXGI_QUERY_VIDEO_MEMORY_INFO vmLocal{}, vmShared{};
        if (s_adapter3
            && SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(
                   0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vmLocal))
            && SUCCEEDED(s_adapter3->QueryVideoMemoryInfo(
                   0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vmShared)))
        {
            TracyPlot("VRAM used",   static_cast<int64_t>(vmLocal.CurrentUsage));
            TracyPlot("VRAM budget", static_cast<int64_t>(vmLocal.Budget));
            TracyPlot("VRAM shared", static_cast<int64_t>(vmShared.CurrentUsage));
        }

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
        {
            TracyPlot("RAM working set", static_cast<int64_t>(pmc.WorkingSetSize));
            TracyPlot("RAM commit",      static_cast<int64_t>(pmc.PrivateUsage));
        }
    }
#endif

    // The slot is submitted: the game side may no longer write it. Closed here
    // rather than at the next acquire so the window is bounded by SUBMISSION,
    // which is the event that actually puts the GPU on the slot.
    //
    // ONLY the frame that opened it may close it. In threaded mode EndFrame(N-1)
    // runs while the game half of N is inside the window it just opened; a
    // plain store would shut it under them, and both consumers would silently
    // take their deferred branch.
    {
        uint64_t mine = m_renderFrame.load(std::memory_order_relaxed);
        m_uploadSlotOpenFrame.compare_exchange_strong(
            mine, UINT64_MAX, std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    // Retire this frame's UPLOAD slot: the record every later wait reads. kFrameCount frames on,
    // AcquireUploadSlot (game side) and WaitForNextFrameSlot (render side) wait on these values before
    // anyone resets the slot's allocators; BeginFrame's frame-stamped drain (DrainReleasesUpTo) relies
    // on the same wait.
    // Values first, then framePlus1 with RELEASE ordering: a reader that observes the frame observes the
    // three fences that belong to it. framePlus1 because 0 is the "never used" sentinel and frame 0 is
    // real. Do not use ~0ull instead: every <= drain predicate accepts it, so the first frames would
    // free every pending resource with no fence proven.
    {
        const uint32_t uslot = m_slotKeyDxgi
                             ? m_surface->BackBufferIndex()
                             : static_cast<uint32_t>(m_renderFrame.load(std::memory_order_relaxed) % kFrameCount);
        FrameSlotFence& s = m_slotFence[uslot];
        s.gfx     = fv;
        s.compute = m_queueFenceValues[1];
        s.copy    = m_queueFenceValues[2];
        s.framePlus1.store(m_renderFrame.load(std::memory_order_relaxed) + 1,
                           std::memory_order_release);
    }

    // The CPU-side frame-rate cap is NOT applied here: App::Run calls LimitFrameRate() after it captures
    // CPU frame time and scheduler phase timings, which keeps the pacing sleep out of every profiler
    // (in-engine panels and the Tracy EndFrame zone) while it still sits after all GPU submission,
    // folded into the next frame's dt.
}

RHI::CommandList GraphicsDX12::BeginCommandList(RHI::QueueType queue)
{
    // ---- Allocate a pool slot (mutex-protected) ----
    m_commandListMutex.lock();
    const uint32_t id = m_commandListCount++;
    if (id >= kMaxCommandLists)
    {
        // Hard cap. Returning an invalid handle is survivable (recording calls
        // null-check the pool entry); silently indexing past the array is not.
        m_commandListCount = kMaxCommandLists;
        m_commandListMutex.unlock();
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            LOG_ERROR("BeginCommandList: command-list pool exhausted (%u slots). "
                      "Raise GraphicsDX12::kMaxCommandLists.", kMaxCommandLists);
        }
        return {};
    }
    if (id >= m_commandListHighWater)
    {
        m_commandLists[id] = m_cmdAllocator.Allocate();
        m_commandListHighWater = id + 1;
    }
    CommandListDX12* clPtr = m_commandLists[id];
    // Under the lock, so GetOwnerCounters (which scans the pool under it) never reads a half-reset
    // entry.
    clPtr->counters = {};
    clPtr->owner    = t_recordingOwner;
    m_commandListMutex.unlock();

    // ---- Reset per-frame state ----
    CommandListDX12& cl     = *clPtr;
    cl.id                    = id;
    cl.queue                 = queue;
    cl.bufferIndex          = GetFrameIndex();
    cl.activePso               = nullptr;
    cl.activeRootSigGraphics  = nullptr;
    cl.activeRootSigCompute   = nullptr;
    cl.prevTopology                  = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    cl.prevStencilRef          = 0;
    cl.dirtyPso                = false;
    ClearDepthView(cl);                 // no DSV, no pipeline yet
    cl.psoWritesDepth           = false;
    cl.psoWritesStencil         = false;
    cl.psoStencilWriteMask      = 0;
    cl.psoId                    = ~0u;
    cl.frameBarriers.clear();
    cl.discards.clear();
    cl.waitFor.clear();
    cl.signalFenceValue = 0;
    cl.submitBarrier     = false;
    cl.markerDepth        = 0;   // fresh CL: no open debug-marker regions
    cl.name               = nullptr;   // this use names itself (SetCommandListName)
    cl.journal.clear();                // the last journaled use's barriers

    const UINT qi = static_cast<UINT>(queue);
    const D3D12_COMMAND_LIST_TYPE listType = ToD3D12CommandListType(queue);

    // ---- Lazily create command allocators (once per slot x frame x queue) ----
    for (UINT f = 0; f < kFrameCount; ++f)
    {
        if (!cl.commandAllocators[f][qi])
            ThrowIfFailed(m_device->CreateCommandAllocator(
                listType, IID_PPV_ARGS(&cl.commandAllocators[f][qi])));
    }

    // ---- Lazily create the command list for this slot + queue ----
    if (!cl.commandLists[qi])
    {
        ThrowIfFailed(m_device->CreateCommandList(
            0, listType, cl.commandAllocators[GetFrameIndex()][qi].Get(),
            nullptr, IID_PPV_ARGS(&cl.commandLists[qi])));
        // Close immediately so Reset() works from the second frame onward.
        ThrowIfFailed(static_cast<ID3D12GraphicsCommandList*>(
            cl.commandLists[qi].Get())->Close());

        // Debug name visible in PIX / RenderDoc.
        std::wstring name = L"cmd" + std::to_wstring(id);
        cl.commandLists[qi]->SetName(name.c_str());

        // The pool slot travels with the D3D12 object, so a site
        // that holds only the raw list can report its own ResourceBarrier
        // calls (GfxDiag::NoteNativeBarriers). A slot owns its lists for life,
        // so this is written once, here.
        const uint32_t slot = id;
        cl.commandLists[qi]->SetPrivateData(kPoolSlotGuid, sizeof(slot), &slot);
    }

    // ---- Reset allocator and open the command list ----
    ThrowIfFailed(cl.commandAllocators[GetFrameIndex()][qi]->Reset());
    ThrowIfFailed(cl.GetCommandList()->Reset(
        cl.commandAllocators[GetFrameIndex()][qi].Get(), nullptr));

    // A pooled list keeps the name its previous user gave it (SetCommandListName);
    // put it back to the slot name so this use starts clean.
    if (cl.namedListMask & (1u << qi))
    {
        const std::wstring slotName = L"cmd" + std::to_wstring(id);
        cl.commandLists[qi]->SetName(slotName.c_str());
        cl.namedListMask &= ~(1u << qi);
    }

    // ---- Bind shader-visible descriptor heaps (DIRECT + COMPUTE, not COPY) ----
    if (queue != RHI::QueueType::COPY)
    {
        ID3D12DescriptorHeap* heaps[] = {
            m_cbvSrvUavAllocator.GetGpuHeap(),
            m_samplerAllocator.GetGpuHeap()
        };
        cl.GetCommandList()->SetDescriptorHeaps(_countof(heaps), heaps);
    }

    return RHI::CommandList{ id };
}

void GraphicsDX12::SetViewportSize(uint32_t width, uint32_t height)
{
    ResizeHdrTarget(0, width, height);
}

void GraphicsDX12::ResizeHdrTarget(uint32_t slot, uint32_t width, uint32_t height)
{
    if (!ENGINE_CHECK(slot < kMaxHdrTargets,
                      "ResizeHdrTarget: slot %u out of range (kMaxHdrTargets=%u)",
                      slot, kMaxHdrTargets))
        return;
    UINT w = width  ? width  : GetWidth();    // 0 = the back buffer's size
    UINT h = height ? height : GetHeight();
    if (w < 1) w = 1; if (h < 1) h = 1;

    // IDEMPOTENCE GUARD: do not delete. App::RenderPhase calls SetViewportSize every frame, so without
    // the compare every frame would rebuild the HDR target and its RTV/SRV/UAV (CreateHdrTarget) and
    // reset the slot's tracked state to RENDER_TARGET.
    // The HdrRecord(slot) term makes a DESTROYED slot rebuild at its old size; it is a no-op for slot 0,
    // whose target exists from LoadPipeline on.
    if (HdrRecord(slot) &&
        w == m_hdrTargets[slot].w && h == m_hdrTargets[slot].h) return;

    // TRIPWIRE ONLY: logs, then rebuilds anyway; refusing would make the editor viewport un-resizable,
    // for a hazard that is not a use-after-free. It catches a rebuild requested after passes started
    // recording: CreateHdrTarget forces the slot's tracked state back to RENDER_TARGET, so the next
    // barrier's before-state no longer matches what the recorded barriers left (the
    // WarnOnRacyHdrStateChange failure, by another road).
    ENGINE_CHECK_ONCE(CanHardSyncNow(),
        "SetViewportSize(%u,%u) called after pass recording started (cmdLists=%u, "
        "parallelPhaseB=%d) ??the HDR target is rebuilt and the tracked HDR state is reset to "
        "RENDER_TARGET, so barriers already recorded this frame will have a mismatched "
        "before-state. Change the viewport size from App::Run's per-frame call site "
        "(immediately after BeginFrame), not from an ImGui handler or a pass.",
        w, h,
        m_commandListCount.load(std::memory_order_relaxed),
        static_cast<int>(m_parallelPassRecording.load(std::memory_order_acquire)));

    CreateHdrTarget(slot, w, h);
}

void GraphicsDX12::SetRenderTargetToHdr(const float clearColor[4], RHI::CommandList cmd)
{
    const HdrTargetBinding& hdr = Hdr();
    TextureDX12* rec = HdrRecord();
    if (!rec || !rec->rtv.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (rec->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        WarnOnRacyHdrStateChange("SetRenderTargetToHdr");
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            rec->resource.Get(), rec->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        JournalNativeBarriers(cmd, &b, 1);
        rec->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0,0, static_cast<float>(hdr.w), static_cast<float>(hdr.h), 0,1 };
    D3D12_RECT     sr{ 0,0, static_cast<LONG>(hdr.w),  static_cast<LONG>(hdr.h) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rec->rtv.GetCpuHandle();
    cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    ClearDepthView(GetPoolEntry(cmd));
    // The clear colour is the caller's: the graph hands over its HDR import's value (BindImport:
    // {0,0,0,0} in the sky modes, matching the RT's optimized clear value, or the scene's flat background
    // in None/FlatColor). Null = the slot's stored colour (SetHdrClearColor), which only the
    // -rgimportoff arm still sets.
    cl->ClearRenderTargetView(rtv, clearColor ? clearColor : hdr.clear, 0, nullptr);
}

void GraphicsDX12::SetHdrClearColor(const float clearColor[4])
{
    if (!clearColor) return;
    for (int i = 0; i < 4; ++i) Hdr().clear[i] = clearColor[i];
}

void GraphicsDX12::SetRenderTargetToHdrWithDepth(const RHI::Texture* dsv, RHI::CommandList cmd,
                                                 const RHI::DepthView& view)
{
    const HdrTargetBinding& hdr = Hdr();
    TextureDX12* rec = HdrRecord();
    if (!rec || !rec->rtv.IsValid()) return;
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (rec->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        WarnOnRacyHdrStateChange("SetRenderTargetToHdrWithDepth");
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            rec->resource.Get(), rec->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        JournalNativeBarriers(cmd, &b, 1);
        rec->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(hdr.w), static_cast<float>(hdr.h), 0, 1 };
    D3D12_RECT     sr{ 0, 0, static_cast<LONG>(hdr.w),  static_cast<LONG>(hdr.h) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rec->rtv.GetCpuHandle();
    if (dsv && dsv->IsValid() && dsv->handleId < static_cast<uint32_t>(m_texturePool.size()))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = BindDepthView(entry, *dsv, view);
        cl->OMSetRenderTargets(1, &rtv, FALSE, &dsvHandle);
    }
    else
    {
        ClearDepthView(entry);
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }
    // No clear: the skybox draws on top of existing HDR content.
}

uint64_t GraphicsDX12::GetHdrSceneSrvGpuHandle() const
{
    const TextureDX12* rec = HdrRecord();
    return rec && rec->srv.IsValid() ? rec->srv.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetHdrSceneUavGpuHandle() const
{
    const TextureDX12* rec = HdrRecord();
    return rec && rec->uav.IsValid() ? rec->uav.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetHdrTargetSrvGpuHandle(uint32_t slot) const
{
    // Deliberately NOT ENGINE_CHECK'd on the slot: a 0 handle is a defined,
    // survivable answer everywhere it is consumed (SetComputeDescriptorTable
    // ignores it), and this is reachable from a per-frame path.
    if (slot >= kMaxHdrTargets) return 0ull;
    const TextureDX12* rec = HdrRecord(slot);
    return rec && rec->srv.IsValid() ? rec->srv.GetGpuHandle().ptr : 0ull;
}

void GraphicsDX12::CopyHdrSceneTo(const RHI::Texture& dst, RHI::CommandList cmd)
{
    const TextureDX12* rec = HdrRecord();
    if (!rec || !dst.IsValid()) return;
    if (dst.handleId >= m_texturePool.size()) return;
    ID3D12Resource* dstRes = m_texturePool[dst.handleId].resource.Get();
    if (!dstRes) return;
    GetPoolEntry(cmd).GetCommandList()->CopyResource(dstRes, rec->resource.Get());
}

// The current HDR scene target's size (IGraphicsDevice).
bool GraphicsDX12::GetHdrSceneSize(uint32_t& width, uint32_t& height) const
{
    const TextureDX12* rec = HdrRecord();
    if (!rec) return false;
    const D3D12_RESOURCE_DESC d = rec->resource->GetDesc();
    width  = static_cast<uint32_t>(d.Width);
    height = d.Height;
    return true;
}

void GraphicsDX12::CopyHdrSceneToScoped(const RHI::Texture& dst, RHI::CommandList cmd)
{
    const TextureDX12* rec = HdrRecord();
    if (!rec || !dst.IsValid()) return;
    if (dst.handleId >= m_texturePool.size()) return;
    ID3D12Resource* dstRes = m_texturePool[dst.handleId].resource.Get();
    if (!dstRes) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    // DELIBERATELY does not read or write the state tracker. Callers are render-graph passes whose
    // Execute() runs on a ParallelFor worker (RenderGraph Phase B), and the tracker is a plain shared
    // member: SetHdrTextureState's read-modify-write would be seen by every pass recording at the same
    // instant, each would emit a COPY_SOURCE -> RENDER_TARGET barrier, and the first one ordered would hit
    // the GPU while HDR is still RENDER_TARGET (a before-state mismatch).
    // The round trip as a closed run inside ONE command list keeps the GPU-visible state identical at
    // entry and exit, so the tracker stays truthful at RENDER_TARGET for the whole of Phase B.
    const D3D12_RESOURCE_STATES kEntry = D3D12_RESOURCE_STATE_RENDER_TARGET;

    auto toCopySrc = CD3DX12_RESOURCE_BARRIER::Transition(
        rec->resource.Get(), kEntry, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->ResourceBarrier(1, &toCopySrc);
    JournalNativeBarriers(cmd, &toCopySrc, 1);

    cl->CopyResource(dstRes, rec->resource.Get());

    auto back = CD3DX12_RESOURCE_BARRIER::Transition(
        rec->resource.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, kEntry);
    cl->ResourceBarrier(1, &back);
    JournalNativeBarriers(cmd, &back, 1);
}

uint64_t GraphicsDX12::GetTextureSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const TextureDX12& entry = m_texturePool[texture.handleId];
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::GetTextureSRVCpuHandleNative(const RHI::Texture& texture) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE nul{};
    if (!texture.IsValid()) return nul;
    if (texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return nul;
    const TextureDX12& entry = m_texturePool[texture.handleId];
    // Return the NON-shader-visible CPU descriptor (where the SRV was created,
    // see CreateTexture). A CopyDescriptorsSimple SOURCE must be CPU-readable;
    // a shader-visible heap's CPU handle (GetGpuCpuHandle) is CPU-WRITE-ONLY and
    // the debug layer rejects it as a copy source ("SrcDescriptorRangeStart
    // points to a descriptor heap type that is CPU write only"). This mirrors
    // the bindless-table copy (CreateTexture: dst=GetGpuCpuHandle, src=GetCpuHandle).
    return entry.srv.IsValid() ? entry.srv.GetCpuHandle() : nul;
}

uint64_t GraphicsDX12::GetTextureSRVCpuHandle(const RHI::Texture& texture) const
{
    return GetTextureSRVCpuHandleNative(texture).ptr;
}

void GraphicsDX12::CopyCbvSrvUavDescriptors(uint64_t dstCpuHandle,
                                            uint64_t srcCpuHandle,
                                            uint32_t count)
{
    if (dstCpuHandle == 0 || srcCpuHandle == 0 || count == 0) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dst{ static_cast<SIZE_T>(dstCpuHandle) };
    D3D12_CPU_DESCRIPTOR_HANDLE src{ static_cast<SIZE_T>(srcCpuHandle) };
    m_cbvSrvUavAllocator.CopyDescriptors(count, dst, src);   // carries the kinds
}

uint32_t GraphicsDX12::GetCbvSrvUavDescriptorIncrement() const
{
    return m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

uint64_t GraphicsDX12::GetTexturePreviewSrvGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const TextureDX12& entry = m_texturePool[texture.handleId];
    // Return UNORM alias if available (for SRGB textures), else fall back to normal SRV.
    if (entry.previewSrv.IsValid()) return entry.previewSrv.GetGpuHandle().ptr;
    return entry.srv.IsValid() ? entry.srv.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetTextureAllocationBytes(const RHI::Texture& texture) const
{
    ID3D12Resource* res = GetTextureResource(texture);
    if (!res || !m_device) return 0ull;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    const D3D12_RESOURCE_ALLOCATION_INFO info = m_device->GetResourceAllocationInfo(0, 1, &rd);
    return info.SizeInBytes == UINT64_MAX ? 0ull : info.SizeInBytes;
}

std::size_t GraphicsDX12::ShaderPoolUsed() const
{
    std::lock_guard<std::mutex> lk(m_resourceMutex);
    return m_shaderPool.size();
}

std::size_t GraphicsDX12::ShaderPoolCapacity() const
{
    std::lock_guard<std::mutex> lk(m_resourceMutex);
    return m_shaderPool.capacity();
}

uint64_t GraphicsDX12::GetTextureStencilSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const TextureDX12& entry = m_texturePool[texture.handleId];
    return entry.stencilSrv.IsValid() ? entry.stencilSrv.GetGpuHandle().ptr : 0ull;
}

uint64_t GraphicsDX12::GetTextureUVPlaneSRVGpuHandle(const RHI::Texture& texture) const
{
    if (!texture.IsValid()) return 0ull;
    if (texture.handleId >= static_cast<uint32_t>(m_texturePool.size())) return 0ull;
    const TextureDX12& entry = m_texturePool[texture.handleId];
    return entry.uvPlaneSrv.IsValid() ? entry.uvPlaneSrv.GetGpuHandle().ptr : 0ull;
}


// ==== Command recording: state ====

void GraphicsDX12::BindPipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    if (!pso.IsValid()) return;
    // A retired slot (DestroyPipelineState) is present but empty. Skip the
    // bind LOUDLY: the alternative is SetPipelineState(nullptr), which the
    // debug layer rejects and Release dereferences. A handle that reaches
    // here is a pass that kept a PSO across a hot reload without re-fetching.
    if (!ENGINE_CHECK_ONCE(pso.handleId < m_psoPool.size() && m_psoPool[pso.handleId]
                           && m_psoPool[pso.handleId]->pso,
                           "BindPipelineState: PSO handle %u is destroyed or out of range "
                           "(pool %zu) - stale handle held across a shader reload?",
                           pso.handleId, m_psoPool.size()))
        return;
    PipelineStateDX12& p = *m_psoPool[pso.handleId];
    auto& entry = GetPoolEntry(cmd);
    auto* cl    = entry.GetCommandList();
    cl->SetPipelineState(p.pso.Get());
    if (entry.IsValid()) ++entry.counters.psoBinds;
    if (p.isCompute)
    {
        cl->SetComputeRootSignature(p.rootSignature.Get());
        entry.activeRootSigCompute  = p.rootSignature.Get();
    }
    else
    {
        cl->SetGraphicsRootSignature(p.rootSignature.Get());
        entry.activeRootSigGraphics = p.rootSignature.Get();
    }
    entry.activePso = &pso;
    entry.psoWritesDepth   = p.writesDepth;     // the depth tripwire
    entry.psoWritesStencil = p.writesStencil;
    entry.psoStencilWriteMask = p.stencilWriteMask;   // the stencil bits
    entry.psoId            = pso.handleId;
}

void GraphicsDX12::SetPrimitiveTopology(RHI::PrimitiveTopology topology, RHI::CommandList cmd)
{
    auto d3dTopo = ToD3D12Topology(topology);
    GetPoolEntry(cmd).GetCommandList()->IASetPrimitiveTopology(d3dTopo);
    GetPoolEntry(cmd).prevTopology = d3dTopo;
}

void GraphicsDX12::SetViewport(const RHI::Viewport& viewport, RHI::CommandList cmd)
{
    D3D12_VIEWPORT d{ viewport.topLeftX, viewport.topLeftY, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth };
    GetPoolEntry(cmd).GetCommandList()->RSSetViewports(1, &d);
}

void GraphicsDX12::SetScissorRect(uint32_t left, uint32_t top,
                                  uint32_t right, uint32_t bottom,
                                  RHI::CommandList cmd)
{
    D3D12_RECT r{ static_cast<LONG>(left), static_cast<LONG>(top),
                  static_cast<LONG>(right), static_cast<LONG>(bottom) };
    GetPoolEntry(cmd).GetCommandList()->RSSetScissorRects(1, &r);
}

// ==== Command recording: resource binding ====

void GraphicsDX12::BindConstantBuffer(RHI::Slots::GfxCB slot,
                                      const RHI::GPUBuffer& buffer, RHI::CommandList cmd)
{
    if (!buffer.IsValid()) return;
    // GROUP-RELATIVE (RHI::Slots::GfxCB): an absolute index is another type and cannot reach here; the
    // range check stays for a GfxCB built by hand.
    const uint32_t slotIndex = slot.index;
    if (!ENGINE_CHECK_ONCE(slotIndex < kCBVSlotCount,
            "BindConstantBuffer: slot %u >= %u - nothing was bound", slotIndex,
            static_cast<uint32_t>(kCBVSlotCount)))
        return;
    if (buffer.handleId >= m_bufferPool.size())
    {
        LOG_ERROR("BindConstantBuffer: handleId %u out of range (pool size %zu)",
                  buffer.handleId, m_bufferPool.size());
        return;
    }
    GPUBufferDX12& p = m_bufferPool[buffer.handleId];
    if (!p.resource)
    {
        LOG_ERROR("BindConstantBuffer: buffer handleId %u has null resource", buffer.handleId);
        return;
    }
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slotIndex, p.resource->GetGPUVirtualAddress());
}

// Slot-offset variant of BindConstantBuffer: ring-buffer owners advance through 256-byte aligned
// slots within one CB instead of recreating one per frame (the reflection probe capture uploads one
// CB per face in a 6-slot ring).
void GraphicsDX12::BindConstantBufferAtOffset(RHI::Slots::GfxCB slot,
                                               const RHI::GPUBuffer& buffer,
                                               uint64_t byteOffset,
                                               RHI::CommandList cmd)
{
    const uint32_t slotIndex = slot.index;
    if (!buffer.IsValid() || buffer.handleId >= m_bufferPool.size()) return;
    // Range-checked: an out-of-range index added to kCBVSlotBase would bind a CBV onto some OTHER root
    // parameter.
    if (!ENGINE_CHECK_ONCE(slotIndex < kCBVSlotCount,
            "BindConstantBufferAtOffset: slot %u >= %u - nothing was bound", slotIndex,
            static_cast<uint32_t>(kCBVSlotCount)))
        return;
    GPUBufferDX12& p = m_bufferPool[buffer.handleId];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        kCBVSlotBase + slotIndex, p.resource->GetGPUVirtualAddress() + byteOffset);
}

// Bind a raw GPU VA to the per-material custom CBV slot (b8 space0). Separate from
// BindConstantBuffer because the upload-ring owner tracks VAs, not GPUBuffer handles. 0 is a no-op.
void GraphicsDX12::BindCustomMaterialCBV(uint64_t gpuAddress, RHI::CommandList cmd)
{
    if (gpuAddress == 0) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(
        RHI::Slots::Gfx::CustomMatCBV.index, gpuAddress);
}

// This frame's transient constants (Graphics/TransientConstants.h).
IGraphicsDevice::TransientConstants GraphicsDX12::AllocTransientConstants(uint32_t bytes)
{
    const TransientConstantAllocator::Alloc a = m_transientConstants.Allocate(bytes);
    return { a.cpu, a.gpu };
}

void GraphicsDX12::BindConstantBufferAddress(RHI::Slots::GfxCB slot, uint64_t gpuAddress, RHI::CommandList cmd)
{
    const uint32_t slotIndex = slot.index;
    if (gpuAddress == 0) return;
    // Group-relative, as BindConstantBuffer's.
    if (!ENGINE_CHECK_ONCE(slotIndex < kCBVSlotCount,
            "BindConstantBufferAddress: slot %u >= %u - nothing was bound", slotIndex,
            static_cast<uint32_t>(kCBVSlotCount)))
        return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(kCBVSlotBase + slotIndex, gpuAddress);
}

// Bind a shader-visible GPU handle to the per-material custom texture table (t0-t7 space3,
// kCustomMatTextureSlots descriptors). A zero handle is a no-op.
void GraphicsDX12::BindCustomMaterialTextureTable(uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(
        RHI::Slots::Gfx::CustomMatTex.index, h);
}

void GraphicsDX12::BindResource(RHI::Slots::GfxSRV slot,
                                const RHI::GPUResource& resource, RHI::CommandList cmd)
{
    const uint32_t slotIndex = slot.index;
    if (!resource.IsValid()) return;
    if (!ENGINE_CHECK_ONCE(slotIndex < kSRVSlotCount,
            "BindResource: slot %u >= %u - nothing was bound", slotIndex,
            static_cast<uint32_t>(kSRVSlotCount)))
        return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();

    if (resource.type == RHI::GPUResource::Type::Texture)
    {
        if (!ENGINE_CHECK_ONCE(resource.handleId < m_texturePool.size(),
                               "BindResource: texture handle %u out of range (pool %zu)",
                               resource.handleId, m_texturePool.size()))
            return;
        TextureDX12& tex = m_texturePool[resource.handleId];
        if (tex.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slotIndex, tex.srv.GetGpuHandle());
    }
    else if (resource.type == RHI::GPUResource::Type::Buffer)
    {
        if (!ENGINE_CHECK_ONCE(resource.handleId < m_bufferPool.size(),
                               "BindResource: buffer handle %u out of range (pool %zu)",
                               resource.handleId, m_bufferPool.size()))
            return;
        GPUBufferDX12& buf = m_bufferPool[resource.handleId];
        if (buf.srv.IsValid())
            cl->SetGraphicsRootDescriptorTable(kSRVSlotBase + slotIndex, buf.srv.GetGpuHandle());
    }
}

void GraphicsDX12::SetDynamicSampler(int samplerIndex, RHI::CommandList cmd)
{
    // A negative index is a failed CreateSampler being set. Silently keeping the previous draw's sampler
    // in the root constant would bind a sampler the pass did not ask for, so report it once.
    const uint32_t heapIndex = SamplerHeapIndex(samplerIndex);
    if (!ENGINE_CHECK_ONCE(heapIndex != ~0u,
                           "SetDynamicSampler: sampler index %d invalid (pool %zu) - "
                           "CreateSampler failed or the sampler was never created",
                           samplerIndex, m_samplerPool.size()))
        return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstant(
        RHI::Slots::Gfx::DynamicSampler.index, heapIndex, 0);
}


// ==== PVF root bindings ====

void GraphicsDX12::SetRootConstants(uint32_t v0, uint32_t v1, uint32_t v2,
                                    RHI::CommandList cmd)
{
    uint32_t values[3] = { v0, v1, v2 };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstants(
        kRootConstantsSlot, 3, values, 0);
}

void GraphicsDX12::SetRootConstants4(uint32_t v0, uint32_t v1, uint32_t v2, uint32_t v3,
                                     RHI::CommandList cmd)
{
    const uint32_t values[4] = { v0, v1, v2, v3 };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstants(
        kRootConstantsSlot, 4, values, 0);
}

void GraphicsDX12::SetRootBufferSRV(RHI::Slots::GfxParam rootSlot,
                                    const RHI::GPUBuffer& buf, RHI::CommandList cmd)
{
    if (!buf.IsValid() || buf.handleId >= m_bufferPool.size()) return;
    GPUBufferDX12& p = m_bufferPool[buf.handleId];
    if (!p.resource) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootShaderResourceView(
        rootSlot.index, p.resource->GetGPUVirtualAddress());
}

void GraphicsDX12::BindDescriptorTableGpuHandle(RHI::Slots::GfxParam rootSlot,
                                                uint64_t gpuHandle,
                                                RHI::CommandList cmd)
{
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(rootSlot.index, h);
}

// ==== Command recording: draws + barriers ====

void GraphicsDX12::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount,
                                 uint32_t startVertex,  uint32_t startInstance,
                                 RHI::CommandList cmd)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    CheckDepthWrites(entry);
    entry.GetCommandList()->DrawInstanced(
        vertexCount, instanceCount, startVertex, startInstance);
    if (entry.IsValid()) ++entry.counters.draws;
}

void GraphicsDX12::DrawIndexedInstanced(uint32_t indexCount,  uint32_t instanceCount,
                                        uint32_t startIndex,  int32_t  baseVertex,
                                        uint32_t startInstance, RHI::CommandList cmd)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    CheckDepthWrites(entry);
    entry.GetCommandList()->DrawIndexedInstanced(
        indexCount, instanceCount, startIndex, baseVertex, startInstance);
    if (entry.IsValid()) ++entry.counters.draws;
}

void GraphicsDX12::ExecuteIndirectDraw(const RHI::GPUBuffer& argBuffer,
                                        uint64_t argBufferOffset,
                                        uint32_t maxCommands,
                                        const RHI::GPUBuffer* countBuffer,
                                        uint64_t countBufferOffset,
                                        RHI::CommandList cmd)
{
    if (!m_indirectCommandSignature || !argBuffer.IsValid()) return;
    if (argBuffer.handleId >= m_bufferPool.size()) return;

    ID3D12Resource* argRes = m_bufferPool[argBuffer.handleId].resource.Get();
    if (!argRes) return;

    ID3D12Resource* countRes = nullptr;
    if (countBuffer && countBuffer->IsValid() && countBuffer->handleId < m_bufferPool.size())
        countRes = m_bufferPool[countBuffer->handleId].resource.Get();

    CommandListDX12& entry = GetPoolEntry(cmd);
    CheckDepthWrites(entry);
    entry.GetCommandList()->ExecuteIndirect(
        m_indirectCommandSignature.Get(),
        maxCommands,
        argRes,
        argBufferOffset,
        countRes,
        countRes ? countBufferOffset : 0);
    if (entry.IsValid()) ++entry.counters.drawsIndirect;
}

void GraphicsDX12::ExecuteIndirectDispatchMesh(const RHI::GPUBuffer& argBuffer,
                                               uint64_t argBufferOffset,
                                               uint32_t maxCommands,
                                               const RHI::GPUBuffer* countBuffer,
                                               uint64_t countBufferOffset,
                                               RHI::CommandList cmd)
{
    if (!m_dispatchMeshCommandSignature || !argBuffer.IsValid()) return;
    if (argBuffer.handleId >= m_bufferPool.size()) return;

    ID3D12Resource* argRes = m_bufferPool[argBuffer.handleId].resource.Get();
    if (!argRes) return;

    ID3D12Resource* countRes = nullptr;
    if (countBuffer && countBuffer->IsValid() && countBuffer->handleId < m_bufferPool.size())
        countRes = m_bufferPool[countBuffer->handleId].resource.Get();

    CommandListDX12& entry = GetPoolEntry(cmd);
    CheckDepthWrites(entry);
    entry.GetCommandList()->ExecuteIndirect(
        m_dispatchMeshCommandSignature.Get(),
        maxCommands,
        argRes,
        argBufferOffset,
        countRes,
        countRes ? countBufferOffset : 0);
    if (entry.IsValid()) ++entry.counters.dispatchesMesh;
}

void GraphicsDX12::CopyBuffer(const RHI::GPUBuffer& src, const RHI::GPUBuffer& dst,
                               uint64_t size, RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handleId >= m_bufferPool.size() || dst.handleId >= m_bufferPool.size()) return;
    ID3D12Resource* srcRes = m_bufferPool[src.handleId].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handleId].resource.Get();
    if (!srcRes || !dstRes || size == 0) return;
    GetPoolEntry(cmd).GetCommandList()->CopyBufferRegion(dstRes, 0, srcRes, 0, size);
}

void GraphicsDX12::CopyBufferRegion(const RHI::GPUBuffer& src, uint64_t srcOffset,
                                    const RHI::GPUBuffer& dst, uint64_t dstOffset,
                                    uint64_t size, RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handleId >= m_bufferPool.size() || dst.handleId >= m_bufferPool.size()) return;
    ID3D12Resource* srcRes = m_bufferPool[src.handleId].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handleId].resource.Get();
    if (!srcRes || !dstRes || size == 0) return;
    GetPoolEntry(cmd).GetCommandList()->CopyBufferRegion(dstRes, dstOffset,
                                                         srcRes, srcOffset, size);
}

// The whole resource, src to dst (the RHI's src-first order).
void GraphicsDX12::CopyTexture(const RHI::Texture& src, const RHI::Texture& dst, RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handleId >= m_texturePool.size() || dst.handleId >= m_texturePool.size()) return;
    ID3D12Resource* srcRes = m_texturePool[src.handleId].resource.Get();
    ID3D12Resource* dstRes = m_texturePool[dst.handleId].resource.Get();
    if (!srcRes || !dstRes) return;
    GetPoolEntry(cmd).GetCommandList()->CopyResource(dstRes, srcRes);
}

void GraphicsDX12::CopyTextureSubresource(const RHI::Texture& src,
                                           uint32_t srcMip, uint32_t srcArraySlice,
                                           const RHI::Texture& dst,
                                           uint32_t dstMip, uint32_t dstArraySlice,
                                           RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handleId >= m_texturePool.size() || dst.handleId >= m_texturePool.size()) return;

    const TextureDX12& srcEntry = m_texturePool[src.handleId];
    const TextureDX12& dstEntry = m_texturePool[dst.handleId];
    ID3D12Resource* srcRes = srcEntry.resource.Get();
    ID3D12Resource* dstRes = dstEntry.resource.Get();
    if (!srcRes || !dstRes) return;

    const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
    const D3D12_RESOURCE_DESC dstDesc = dstRes->GetDesc();

    // D3D12CalcSubresource(mip, arraySlice, planeSlice, mipLevels, arraySize)
    const UINT srcSub = D3D12CalcSubresource(
        srcMip, srcArraySlice, 0, srcDesc.MipLevels, srcDesc.DepthOrArraySize);
    const UINT dstSub = D3D12CalcSubresource(
        dstMip, dstArraySlice, 0, dstDesc.MipLevels, dstDesc.DepthOrArraySize);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcRes;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = srcSub;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource        = dstRes;
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = dstSub;

    GetPoolEntry(cmd).GetCommandList()->CopyTextureRegion(
        &dstLoc, 0, 0, 0, &srcLoc, /*pSrcBox*/ nullptr);
}

// ==== Compute dispatch ====

void GraphicsDX12::BindComputePipelineState(const RHI::PipelineState& pso, RHI::CommandList cmd)
{
    BindPipelineState(pso, cmd);
}

void GraphicsDX12::DispatchCompute(uint32_t threadGroupX, uint32_t threadGroupY, uint32_t threadGroupZ, RHI::CommandList cmd)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    entry.GetCommandList()->Dispatch(threadGroupX, threadGroupY, threadGroupZ);
    if (entry.IsValid()) ++entry.counters.dispatches;
}

// A graphics PSO's own pipeline layout, by absolute root parameter; the shared signature's binders
// take group-relative slots.
void GraphicsDX12::SetGraphicsRootCBVAddress(RHI::Slots::GfxParam rootSlot, uint64_t gpuAddress,
                                             RHI::CommandList cmd)
{
    if (gpuAddress == 0) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootConstantBufferView(rootSlot.index, gpuAddress);
}

void GraphicsDX12::SetGraphicsRootConstants(RHI::Slots::GfxParam rootSlot, uint32_t count, const void* data,
                                            uint32_t destOffset, RHI::CommandList cmd)
{
    if (count == 0 || !data) return;
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRoot32BitConstants(rootSlot.index, count, data, destOffset);
}

void GraphicsDX12::SetGraphicsDescriptorTable(RHI::Slots::GfxParam rootSlot, uint64_t gpuHandle,
                                              RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetGraphicsRootDescriptorTable(rootSlot.index, h);
}

// One dispatch, its group counts at @p byteOffset in @p args. Counted as a dispatch, and among the
// indirect ones.
void GraphicsDX12::DispatchComputeIndirect(const RHI::GPUBuffer& args, uint64_t byteOffset, RHI::CommandList cmd)
{
    if (!ENGINE_CHECK_ONCE(m_dispatchCommandSignature && args.IsValid() && args.handleId < m_bufferPool.size(),
                           "DispatchComputeIndirect: no dispatch signature, or an invalid argument buffer"))
        return;
    ID3D12Resource* argRes = m_bufferPool[args.handleId].resource.Get();
    if (!argRes) return;
    CommandListDX12& entry = GetPoolEntry(cmd);
    entry.GetCommandList()->ExecuteIndirect(m_dispatchCommandSignature.Get(), 1, argRes, byteOffset, nullptr, 0);
    if (entry.IsValid())
    {
        ++entry.counters.dispatches;
        ++entry.counters.dispatchesIndirect;
    }
}

void GraphicsDX12::DispatchMesh(uint32_t threadGroupX, uint32_t threadGroupY, uint32_t threadGroupZ, RHI::CommandList cmd)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    CheckDepthWrites(entry);
    entry.GetCommandList()->DispatchMesh(threadGroupX, threadGroupY, threadGroupZ);
    if (entry.IsValid()) ++entry.counters.dispatchesMesh;
}

void GraphicsDX12::SetComputeRootCBV(RHI::Slots::CmpParam rootSlot, const RHI::GPUBuffer& cb, uint32_t byteOffset, RHI::CommandList cmd)
{
    if (!cb.IsValid()) return;
    const GPUBufferDX12& p = m_bufferPool[cb.handleId];
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootConstantBufferView(
        rootSlot.index, p.resource->GetGPUVirtualAddress() + byteOffset);
}

void GraphicsDX12::SetComputeRootCBVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuAddress, RHI::CommandList cmd)
{
    if (gpuAddress == 0) return;
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootConstantBufferView(rootSlot.index, gpuAddress);
}

void GraphicsDX12::SetComputeDescriptorTable(RHI::Slots::CmpParam rootSlot, uint64_t gpuHandle, RHI::CommandList cmd)
{
    if (gpuHandle == 0) return;
    D3D12_GPU_DESCRIPTOR_HANDLE h{ gpuHandle };
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootDescriptorTable(rootSlot.index, h);
}

void GraphicsDX12::SetComputeRootConstants(RHI::Slots::CmpParam rootSlot, uint32_t count, const void* data,
                                           uint32_t destOffset, RHI::CommandList cmd)
{
    if (count == 0 || !data) return;
    GetPoolEntry(cmd).GetCommandList()->SetComputeRoot32BitConstants(rootSlot.index, count, data, destOffset);
}

void GraphicsDX12::SetComputeRootSRVAddress(RHI::Slots::CmpParam rootSlot, uint64_t gpuVA, RHI::CommandList cmd)
{
    GetPoolEntry(cmd).GetCommandList()->SetComputeRootShaderResourceView(rootSlot.index, gpuVA);
}

uint64_t GraphicsDX12::GetTextureUAVGpuHandle(const RHI::Texture& tex) const
{
    if (!tex.IsValid()) return 0;
    const TextureDX12& t = m_texturePool[tex.handleId];
    if (!t.uav.IsValid()) return 0;
    return t.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferUAVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBufferDX12& b = m_bufferPool[buf.handleId];
    if (!b.uav.IsValid()) return 0;
    return b.uav.GetGpuHandle().ptr;
}

uint64_t GraphicsDX12::GetBufferSRVGpuHandle(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid()) return 0;
    const GPUBufferDX12& b = m_bufferPool[buf.handleId];
    if (!b.srv.IsValid()) return 0;
    return b.srv.GetGpuHandle().ptr;
}

// (GpuHandle - heapStart) / descriptorSize: the value an SM6.6 shader feeds to
// ResourceDescriptorHeap[], the same arithmetic ReflectionProbeManager uses for its static cubemap
// block. Takes a GPU handle because every producer of a CB-carried heap-index resource (LightingPass's
// RT reflection / RT shadow-mask outputs) hands out a GPU handle rather than an RHI::Texture.
uint32_t GraphicsDX12::DescriptorHeapIndexFromGpuHandle(uint64_t gpuHandle) const
{
    if (gpuHandle == 0) return ~0u;
    ID3D12DescriptorHeap* heap = m_cbvSrvUavAllocator.GetGpuHeap();
    if (!heap) return ~0u;
    const uint64_t base = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const uint32_t inc  = m_cbvSrvUavAllocator.GetDescriptorSize();
    if (gpuHandle < base || inc == 0) return ~0u;
    return static_cast<uint32_t>((gpuHandle - base) / inc);
}

uint32_t GraphicsDX12::GetBufferSRVHeapIndex(const RHI::GPUBuffer& buf) const
{
    // Buffer SRVs live in the shader-visible heap's STATIC region (CreateBuffer
    // allocates via AllocateStatic), so the index is stable for the buffer's
    // life. Texture SRVs are NOT: recompute those every frame.
    return DescriptorHeapIndexFromGpuHandle(GetBufferSRVGpuHandle(buf));
}

uint32_t GraphicsDX12::GetBufferCBVHeapIndex(const RHI::GPUBuffer& buf) const
{
    // The whole-buffer CBV CreateBuffer gives every CONSTANT_BUFFER up to 64 KB:
    // the same static region as the SRV above, so stable for the buffer's life.
    if (!buf.IsValid() || buf.handleId >= m_bufferPool.size()) return ~0u;
    const GPUBufferDX12& b = m_bufferPool[buf.handleId];
    if (!b.cbv.IsValid()) return ~0u;
    return DescriptorHeapIndexFromGpuHandle(b.cbv.GetGpuHandle().ptr);
}

// Device address of a buffer's memory (as opposed to its descriptor), for the
// acceleration-structure builders and the indirect-argument setup.
uint64_t GraphicsDX12::GetBufferGPUAddress(const RHI::GPUBuffer& buf) const
{
    if (!buf.IsValid() || buf.handleId >= m_bufferPool.size()) return 0;
    const GPUBufferDX12& p = m_bufferPool[buf.handleId];
    if (!p.resource) return 0;
    return p.resource->GetGPUVirtualAddress();
}

// Named-region timestamp lookup, so budget watchdogs don't need the profiler
// object. Results are kFrameCount-1 frames late by design (see the GPUProfiler banner).
bool GraphicsDX12::GetGPUTimestampResult(const char* regionName,
                                         float& outGpuMs) const
{
    if (!regionName) return false;
    if (!m_gpuProfiler.enabled || !m_gpuProfiler.hasResults) return false;
    for (uint32_t i = 0; i < m_gpuProfiler.resultCount; ++i)
    {
        const auto& r = m_gpuProfiler.results[i];
        if (r.name && std::strcmp(r.name, regionName) == 0)
        {
            outGpuMs = r.gpuMs;
            return true;
        }
    }
    return false;
}

void GraphicsDX12::WarnOnRacyHdrStateChange(const char* site) const
{
    if (!m_parallelPassRecording.load(std::memory_order_acquire)) return;
    static std::atomic<bool> s_logged{ false };
    if (s_logged.exchange(true)) return;
    LOG_ERROR("%s changed the shared HDR state tracker during RenderGraph Phase B "
              "(parallel pass recording). Every pass recording concurrently reads the "
              "same tracker, so they emit barriers whose before-state will not match "
              "the state the GPU is actually in ??expect ResourceBarrier errors on "
              "'GraphicsDX12.HdrEditorViewport'. Use CopyHdrSceneToScoped, or move the "
              "transition into Phase A. (logged once per session)", site);
}

void GraphicsDX12::SetHdrTextureState(RHI::ResourceState newState, RHI::CommandList cmd)
{
    TextureDX12* rec = HdrRecord();
    if (!rec) return;
    const D3D12_RESOURCE_STATES target = ToD3D12ResourceState(newState);
    if (rec->state == target) return;
    WarnOnRacyHdrStateChange("SetHdrTextureState");
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        rec->resource.Get(), rec->state, target);
    GetPoolEntry(cmd).GetCommandList()->ResourceBarrier(1, &barrier);
    JournalNativeBarriers(cmd, &barrier, 1);
    rec->state = target;
}

void GraphicsDX12::RebindShaderVisibleHeaps(RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    ID3D12DescriptorHeap* heaps[] = {
        m_cbvSrvUavAllocator.GetGpuHeap(),
        m_samplerAllocator.GetGpuHeap()
    };
    cl->SetDescriptorHeaps(_countof(heaps), heaps);
}

void GraphicsDX12::RestoreDefaultComputeRootSignature(RHI::CommandList cmd)
{
    auto& entry = GetPoolEntry(cmd);
    entry.GetCommandList()->SetComputeRootSignature(m_computeRootSignature.Get());
    entry.activeRootSigCompute = m_computeRootSignature.Get();
    entry.activePso = nullptr;
}

uint32_t GraphicsDX12::TranslateBarrier(const RHI::GPUBarrier&  barrier,
                                        D3D12_RESOURCE_BARRIER* out)
{
    // Resolve a generic GPUResource* -> ID3D12Resource*.
    auto resolveResource = [this](const RHI::GPUResource* res) -> ID3D12Resource*
    {
        if (!res || !res->IsValid()) return nullptr;
        if (res->type == RHI::GPUResource::Type::Texture)
            return m_texturePool[res->handleId].resource.Get();
        if (res->type == RHI::GPUResource::Type::Buffer)
            return m_bufferPool[res->handleId].resource.Get();
        if (res->type == RHI::GPUResource::Type::AccelerationStructure)
            return LookupAccelerationStructure(*res);
        return nullptr;
    };

    switch (barrier.type)
    {
    case RHI::GPUBarrier::Type::MEMORY:
        out[0] = CD3DX12_RESOURCE_BARRIER::UAV(resolveResource(barrier.memory.resource));
        return 1;

    case RHI::GPUBarrier::Type::IMAGE:
    {
        // A null / invalid / out-of-range handle has no state to track, so
        // dropping the barrier is exact (PushBarriers counts only translated
        // entries). Without this the read below is a wild-address AV.
        const RHI::Texture* img = barrier.image.texture;
        if (!ENGINE_CHECK_ONCE(img && img->IsValid() && img->handleId < m_texturePool.size(),
                               "TranslateBarrier: IMAGE barrier on a null/invalid texture "
                               "handle (id %u, pool %zu) - dropped",
                               img ? img->handleId : ~0u, m_texturePool.size()))
            return 0;
        TextureDX12& t = m_texturePool[img->handleId];
        // A streaming upload target is moved by the uploader alone, with barriers the book never sees:
        // QueueTextureUpload reads its book state on the game half, which a transition here would race - and
        // leave stale.
        ENGINE_CHECK_ONCE(!t.streamTarget,
                          "TranslateBarrier: '%s' is a streaming upload target - only the uploader may move it "
                          "(sec.5.8); this book transition races QueueTextureUpload's state read",
                          DebugNameOf(t.resource.Get()).c_str());
        const D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.image.layoutBefore);
        const D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.image.layoutAfter);
        const UINT mips   = img->desc.mipLevels ? img->desc.mipLevels : 1;
        const UINT slices = img->desc.arraySize ? img->desc.arraySize : 1;

        // One (mip, slice). Nothing records such a barrier yet; the book has no
        // entry per subresource, so it is left as it was.
        if (barrier.image.mip >= 0 && barrier.image.slice >= 0)
        {
            ENGINE_CHECK_ONCE(false, "TranslateBarrier: a (mip %d, slice %d) barrier on '%s' - the state "
                              "book does not track subresources and stays as it was",
                              barrier.image.mip, barrier.image.slice, DebugNameOf(t.resource.Get()).c_str());
            const UINT plane = barrier.image.plane > 0 && t.planes == 2 ? 1u : 0u;
            out[0] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after,
                D3D12CalcSubresource(barrier.image.mip, barrier.image.slice, plane, mips, slices));
            return 1;
        }

        // A single-plane texture: the whole of it.
        if (t.planes < 2)
        {
            if (!ENGINE_CHECK_ONCE(barrier.image.plane <= 0,
                                   "TranslateBarrier: plane %d of the single-plane texture '%s' - dropped",
                                   barrier.image.plane, DebugNameOf(t.resource.Get()).c_str()))
                return 0;
            out[0] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after,
                                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            t.state = after;
            return 1;
        }

        // A depth-stencil texture: plane 0 depth and plane 1 stencil, a book each. A plane's subresources
        // are mips x slices from plane x mips x slices on; every one the engine makes has one mip and one
        // slice, so a plane is one subresource - more is reported and the barrier covers every plane
        // instead.
        D3D12_RESOURCE_STATES* book[2] = { &t.state, &t.stencilState };
        const bool onePlaneOneSub = mips * slices == 1;
        if (barrier.image.plane >= 0)
        {
            const int p = barrier.image.plane;
            if (!ENGINE_CHECK_ONCE(p <= 1, "TranslateBarrier: plane %d of '%s' - dropped",
                                   p, DebugNameOf(t.resource.Get()).c_str()))
                return 0;
            if (ENGINE_CHECK_ONCE(onePlaneOneSub, "TranslateBarrier: a plane barrier on '%s' (%u mips x %u slices) - "
                                  "every plane is moved instead", DebugNameOf(t.resource.Get()).c_str(), mips, slices))
            {
                out[0] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after, static_cast<UINT>(p));
                *book[p] = after;
                return 1;
            }
        }
        // Every plane. The planes agree: one barrier, from the caller's state.
        if (t.state == t.stencilState || !onePlaneOneSub)
        {
            out[0] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), before, after,
                                                          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
            t.state = t.stencilState = after;
            return 1;
        }
        // They differ (a plane barrier moved one of them): one barrier per plane,
        // each from its own book - the caller's tracker holds one state for the
        // texture. A plane already in @p after has none (a transition must change
        // the state).
        ENGINE_CHECK_ONCE(before == t.state || before == t.stencilState,
                          "TranslateBarrier: a barrier on every plane of '%s' from 0x%X - the planes are in 0x%X "
                          "(depth) and 0x%X (stencil); each is moved from its own",
                          DebugNameOf(t.resource.Get()).c_str(), static_cast<unsigned>(before),
                          static_cast<unsigned>(t.state), static_cast<unsigned>(t.stencilState));
        uint32_t n = 0;
        for (UINT p = 0; p < 2; ++p)
        {
            if (*book[p] != after)
                out[n++] = CD3DX12_RESOURCE_BARRIER::Transition(t.resource.Get(), *book[p], after, p);
            *book[p] = after;
        }
        return n;
    }
    case RHI::GPUBarrier::Type::BUFFER:
    {
        const RHI::GPUBuffer* b = barrier.buffer.buffer;
        if (!ENGINE_CHECK_ONCE(b && b->IsValid() && b->handleId < m_bufferPool.size(),
                               "TranslateBarrier: BUFFER barrier on a null/invalid buffer "
                               "handle (id %u, pool %zu) - dropped",
                               b ? b->handleId : ~0u, m_bufferPool.size()))
            return 0;
        GPUBufferDX12& buf = m_bufferPool[b->handleId];
        D3D12_RESOURCE_STATES before = ToD3D12ResourceState(barrier.buffer.stateBefore);
        D3D12_RESOURCE_STATES after  = ToD3D12ResourceState(barrier.buffer.stateAfter);
        out[0] = CD3DX12_RESOURCE_BARRIER::Transition(buf.resource.Get(), before, after);
        buf.state = after;
        return 1;
    }
    case RHI::GPUBarrier::Type::ALIASING:
        out[0] = CD3DX12_RESOURCE_BARRIER::Aliasing(
            resolveResource(barrier.aliasing.resourceBefore),
            resolveResource(barrier.aliasing.resourceAfter));
        return 1;
    }
    return 0;
}

void GraphicsDX12::PushBarrier(const RHI::GPUBarrier& barrier, RHI::CommandList cmd)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;

    D3D12_RESOURCE_BARRIER b[kMaxBarriersPerTranslate]{};
    const uint32_t n = TranslateBarrier(barrier, b);
    if (!n) return;
    cl->ResourceBarrier(n, b);
    if (m_journalOn.load(std::memory_order_relaxed))
        JournalBarriers(entry, b, n, false);
    if (entry.IsValid()) entry.counters.barriers += n;
    TraceImageBarrier(barrier, b, n, cmd);
}

// The barrier as PushBarrier issues it (see IGraphicsDevice) - the list, the journal, the counters -
// with the texture's book put back (both planes' books of a depth-stencil texture). Only the pass
// recording this list touches that texture in Phase B, so the put-back races nothing; no texture is
// created then, so the pool holds still.
void GraphicsDX12::PushBarrierScoped(const RHI::GPUBarrier& barrier, RHI::CommandList cmd)
{
    const RHI::Texture* img = barrier.type == RHI::GPUBarrier::Type::IMAGE ? barrier.image.texture : nullptr;
    const bool tracked = img && img->IsValid() && img->handleId < m_texturePool.size();
    const D3D12_RESOURCE_STATES kept        = tracked ? m_texturePool[img->handleId].state        : D3D12_RESOURCE_STATE_COMMON;
    const D3D12_RESOURCE_STATES keptStencil = tracked ? m_texturePool[img->handleId].stencilState : D3D12_RESOURCE_STATE_COMMON;
    PushBarrier(barrier, cmd);
    if (tracked)
    {
        m_texturePool[img->handleId].state        = kept;
        m_texturePool[img->handleId].stencilState = keptStencil;
    }
}

void GraphicsDX12::PushBarriers(const RHI::GPUBarrier* barriers,
                                uint32_t               count,
                                RHI::CommandList       cmd)
{
    if (!barriers || count == 0) return;
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;

    // Per-call local storage on purpose: pass recording runs on TaskSystem workers, so a shared scratch
    // buffer would race. kInline covers every batch the engine emits; the largest are the shadow-family
    // cull's three 1 + 2*kShadowCullViewsTotal = 31 element batches (Renderer::RecordShadowFamilyCull),
    // hence 32 barriers' worth, not 16 (which would heap-allocate three times a frame on the main
    // thread). A barrier may translate to kMaxBarriersPerTranslate D3D12 barriers (a depth-stencil
    // texture's planes). Anything larger falls back to the heap rather than being silently split into
    // several ResourceBarrier calls.
    constexpr uint32_t kInline = 32 * kMaxBarriersPerTranslate;
    D3D12_RESOURCE_BARRIER              inlineArr[kInline];
    std::vector<D3D12_RESOURCE_BARRIER> heapArr;
    D3D12_RESOURCE_BARRIER*             arr = inlineArr;
    if (count * kMaxBarriersPerTranslate > kInline)
    {
        heapArr.resize(count * kMaxBarriersPerTranslate);
        arr = heapArr.data();
    }

    // State book-keeping is applied in array order inside TranslateBarrier, so
    // the result is identical to calling PushBarrier @p count times - only the
    // ResourceBarrier call is coalesced.
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t got = TranslateBarrier(barriers[i], arr + n);
        TraceImageBarrier(barriers[i], arr + n, got, cmd);
        n += got;
    }

    if (n) cl->ResourceBarrier(n, arr);
    if (n && m_journalOn.load(std::memory_order_relaxed))   // what the GPU got
        JournalBarriers(entry, arr, n, false);
    if (entry.IsValid()) entry.counters.barriers += n;
}

// DX12_DEPTHTRACE hook. Both PushBarrier paths funnel here, so a transition on GBuffer_Depth is
// logged with the command-list id that RECORDED it - the one fact a debug-layer "Before state ...
// does not match" line about 'cmdNN' does not give. One line per translated D3D12 barrier (a
// depth-stencil texture's barrier may be two); the name is the D3D12 object's, since
// TextureDesc::debugName is borrowed for the CreateTexture call alone.
void GraphicsDX12::TraceImageBarrier(const RHI::GPUBarrier& barrier, const D3D12_RESOURCE_BARRIER* out,
                                     uint32_t n, RHI::CommandList cmd)
{
    if (!GfxTrace::Enabled()) return;
    if (barrier.type != RHI::GPUBarrier::Type::IMAGE || !out || n == 0) return;
    const RHI::Texture* tex = barrier.image.texture;
    if (!tex || !tex->IsValid() || tex->handleId >= m_texturePool.size()) return;
    const TextureDX12& t = m_texturePool[tex->handleId];
    const std::string name = DebugNameOf(t.resource.Get());
    for (uint32_t i = 0; i < n; ++i)
    {
        if (out[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
        const UINT sub = out[i].Transition.Subresource;
        const int plane = (t.planes == 2 && sub != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) ? static_cast<int>(sub) : -1;
        GfxTrace::ImageBarrier(cmd.internalId, name.c_str(),
                               barrier.image.layoutBefore, barrier.image.layoutAfter,
                               static_cast<uint32_t>(out[i].Transition.StateBefore),
                               static_cast<uint32_t>(out[i].Transition.StateAfter), plane);
    }
}

// The DSV a bind uses, and the draw-time depth tripwire. BindDepthView picks the writable DSV or the
// read-only variant RHI::DepthView names and records the view on the list; a variant the texture
// lacks (an array, or a stencil flag on a depth-only format) is reported once and the writable DSV
// bound instead. CheckDepthWrites runs before every draw: a pipeline that writes depth or stencil
// through a DSV whose pass declared that plane read-only is reported, as is one writing stencil bits
// outside the pass's declared mask - the depth / stencil half of GPU-based validation, which this
// machine cannot run.
D3D12_CPU_DESCRIPTOR_HANDLE GraphicsDX12::BindDepthView(CommandListDX12& entry, const RHI::Texture& dsv,
                                                        const RHI::DepthView& view)
{
    const TextureDX12& t = m_texturePool[dsv.handleId];
    D3D12_CPU_DESCRIPTOR_HANDLE h = t.dsv.GetCpuHandle();
    const int ro = (view.readOnlyDepth ? 1 : 0) | (view.readOnlyStencil ? 2 : 0);
    if (ro)
    {
        const DescriptorAllocation& a = t.dsvReadOnly[ro - 1];
        if (ENGINE_CHECK_ONCE(a.IsValid(), "BindDepthView: '%s' has no read-only (%s) DSV - its writable DSV is bound",
                              DebugNameOf(t.resource.Get()).c_str(),
                              ro == 1 ? "depth" : ro == 2 ? "stencil" : "depth and stencil"))
            h = a.GetCpuHandle();
    }
    entry.depthView  = view;
    entry.depthTexId = dsv.handleId;
    entry.depthGuard = !view.mayWriteDepth || !view.mayWriteStencil || view.stencilWriteMask != 0xFF;
    return h;
}

void GraphicsDX12::ClearDepthView(CommandListDX12& entry)
{
    entry.depthView  = {};
    entry.depthTexId = ~0u;
    entry.depthGuard = false;
}

void GraphicsDX12::CheckDepthWrites(const CommandListDX12& entry) const
{
    if (!entry.depthGuard) return;
    const bool depth   = entry.psoWritesDepth   && !entry.depthView.mayWriteDepth;
    const bool stencil = entry.psoWritesStencil && !entry.depthView.mayWriteStencil;
    ENGINE_CHECK_ONCE(!depth && !stencil,
                      "Depth tripwire: a draw on '%s' writes %s of '%s' with pipeline %u - its pass declared it "
                      "read-only", entry.name ? entry.name : "<unnamed list>",
                      depth ? (stencil ? "depth and stencil" : "depth") : "stencil",
                      entry.depthTexId < m_texturePool.size()
                          ? DebugNameOf(m_texturePool[entry.depthTexId].resource.Get()).c_str() : "?",
                      entry.psoId);
    // The stencil bits: a pipeline writing bits its pass's declaration does not name
    // (RenderGraph/StencilBits.h).
    const uint32_t extra = entry.psoWritesStencil && entry.depthView.mayWriteStencil
                         ? static_cast<uint32_t>(entry.psoStencilWriteMask & ~entry.depthView.stencilWriteMask) : 0u;
    ENGINE_CHECK_ONCE(extra == 0,
                      "Depth tripwire: a draw on '%s' writes stencil bits 0x%02X of '%s' with pipeline %u - its pass "
                      "declared 0x%02X", entry.name ? entry.name : "<unnamed list>", extra,
                      entry.depthTexId < m_texturePool.size()
                          ? DebugNameOf(m_texturePool[entry.depthTexId].resource.Get()).c_str() : "?",
                      entry.psoId, static_cast<uint32_t>(entry.depthView.stencilWriteMask));
}

// The barrier journal and the frame snapshot (Graphics/FrameSnapshot.h). Every barrier a journaled
// frame records lands in its list's CommandListDX12::journal: PushBarrier / PushBarriers after the
// translation (the D3D12 barrier the GPU got), this device's own native transitions through
// JournalNativeBarriers, and every other site's through GfxDiag::NoteNativeBarriers. EndFrame folds
// them into one FrameSnapshot.
void GraphicsDX12::JournalBarriers(CommandListDX12& entry, const D3D12_RESOURCE_BARRIER* barriers,
                                   uint32_t count, bool native)
{
    if (!barriers) return;
    for (uint32_t i = 0; i < count; ++i)
    {
        const D3D12_RESOURCE_BARRIER& b = barriers[i];
        CommandListDX12::JournalRecord r;
        r.type   = static_cast<uint8_t>(b.Type);
        r.flags  = static_cast<uint8_t>(b.Flags);
        r.native = native;
        switch (b.Type)
        {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            r.resource    = b.Transition.pResource;
            r.before      = static_cast<uint32_t>(b.Transition.StateBefore);
            r.after       = static_cast<uint32_t>(b.Transition.StateAfter);
            r.subresource = b.Transition.Subresource;   // ALL_SUBRESOURCES is ~0u, the snapshot's kNone
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
            r.resource      = b.Aliasing.pResourceBefore;
            r.resourceAfter = b.Aliasing.pResourceAfter;
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            r.resource = b.UAV.pResource;
            break;
        default:
            break;
        }
        entry.journal.push_back(r);
    }
}

void GraphicsDX12::JournalNativeList(ID3D12GraphicsCommandList* list,
                                     const D3D12_RESOURCE_BARRIER* barriers, uint32_t count)
{
    if (!list || !m_journalOn.load(std::memory_order_relaxed)) return;
    uint32_t slot = ~0u;
    UINT     size = sizeof(slot);
    // Not one of the pool's lists (an upload context, the video decoder's
    // own): those are not in the frame's command-list graph, so not journaled.
    if (FAILED(list->GetPrivateData(kPoolSlotGuid, &size, &slot)) || size != sizeof(slot)) return;
    if (slot >= m_commandListCount.load(std::memory_order_relaxed)) return;
    CommandListDX12* entry = m_commandLists[slot];
    if (!entry || entry->id != slot) return;
    JournalBarriers(*entry, barriers, count, true);
}

void GfxDiag::NoteNativeBarriers(ID3D12GraphicsCommandList* list,
                                 const D3D12_RESOURCE_BARRIER* barriers, uint32_t count)
{
    GraphicsDX12* device = g_journalDevice.load(std::memory_order_acquire);
    if (!device || !list || !barriers || count == 0) return;
    device->JournalNativeList(list, barriers, count);
}

std::shared_ptr<const GfxDiag::FrameSnapshot> GraphicsDX12::TakeFrameSnapshot()
{
    return std::exchange(m_frameSnapshot, nullptr);
}

void GraphicsDX12::BuildFrameSnapshot(uint32_t listCount)
{
    using Snap = GfxDiag::FrameSnapshot;
    auto snap = std::make_shared<Snap>();
    Snap& s = *snap;
    s.frame       = m_renderFrame.load(std::memory_order_relaxed);
    s.submitOrder = m_submitOrder;
    s.batches     = m_journalBatches;

    s.commandLists.resize(listCount);
    for (uint32_t i = 0; i < listCount; ++i)
    {
        const CommandListDX12* e = m_commandLists[i];
        Snap::CommandList& c = s.commandLists[i];
        c.slot = i;
        if (!e) continue;
        c.name  = e->name ? e->name : "";
        c.queue = static_cast<uint8_t>(e->queue);
        for (uint32_t dep : e->waitFor)
            if (dep < listCount) c.waitFor.push_back(dep);
        c.batch = i < m_journalBatchOf.size() ? m_journalBatchOf[i] : Snap::kNone;
        if (i < m_journalWaits.size()) c.waits = m_journalWaits[i];
    }
    for (uint32_t k = 0; k < static_cast<uint32_t>(s.submitOrder.size()); ++k)
        if (s.submitOrder[k] < listCount) s.commandLists[s.submitOrder[k]].submitIndex = k;

    // Resources by identity; the name is read once per resource, here: every
    // resource this frame barriered is alive until the GPU is done with it.
    std::unordered_map<ID3D12Resource*, uint32_t> index;
    auto resourceOf = [&](ID3D12Resource* r) -> uint32_t
    {
        if (!r) return Snap::kNone;
        const auto [it, added] = index.try_emplace(r, static_cast<uint32_t>(s.resources.size()));
        if (added)
        {
            Snap::Resource x;
            x.address = reinterpret_cast<uint64_t>(r);
            x.name    = DebugNameOf(r);
            x.buffer  = r->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
            s.resources.push_back(std::move(x));
        }
        return it->second;
    };

    // The journal in the order the GPU meets it: lists in submission order,
    // each list's barriers in recording order.
    for (uint32_t slot : s.submitOrder)
    {
        if (slot >= listCount || !m_commandLists[slot]) continue;
        CommandListDX12& e = *m_commandLists[slot];
        Snap::CommandList& c = s.commandLists[slot];
        c.firstBarrier = static_cast<uint32_t>(s.journal.size());
        for (const CommandListDX12::JournalRecord& r : e.journal)
        {
            Snap::Barrier b;
            b.cl            = slot;
            b.kind          = static_cast<Snap::Kind>(r.type);
            b.path          = r.native ? Snap::Path::Native : Snap::Path::Device;
            b.flags         = r.flags;
            b.resource      = resourceOf(r.resource);
            b.resourceAfter = resourceOf(r.resourceAfter);
            b.before        = r.before;
            b.after         = r.after;
            b.subresource   = r.subresource;
            s.journal.push_back(b);
        }
        c.barrierCount = static_cast<uint32_t>(s.journal.size()) - c.firstBarrier;
        e.journal.clear();
    }
    m_frameSnapshot = std::move(snap);
}

// Device-side resource-state tracker readback (RenderGraph depth tripwire). TextureDX12::state is
// what TranslateBarrier last wrote for this resource; the graph keeps its OWN tracker, and the two
// silently diverging is what this answers. Returns false only when the backend genuinely disagrees:
// an unknown / never-barriered handle reports agreement, so the tripwire cannot fire on a texture it
// knows nothing about.
bool GraphicsDX12::DeviceStateAgrees(const RHI::Texture& tex,
                                     RHI::ResourceState  expected,
                                     uint32_t*           deviceStateOut,
                                     uint32_t*           expectedStateOut,
                                     int                 plane) const
{
    const D3D12_RESOURCE_STATES want = ToD3D12ResourceState(expected);
    if (expectedStateOut) *expectedStateOut = static_cast<uint32_t>(want);
    if (!tex.IsValid() || tex.handleId >= m_texturePool.size()) return true;
    const TextureDX12& t = m_texturePool[tex.handleId];
    // One plane of a depth-stencil texture, or (plane -1) every plane, each of which must agree.
    auto agrees = [want](D3D12_RESOURCE_STATES have) {
        return have == D3D12_RESOURCE_STATE_COMMON || have == want;   // COMMON: never barriered, no opinion
    };
    const D3D12_RESOURCE_STATES have = (plane == 1 && t.planes == 2) ? t.stencilState : t.state;
    if (deviceStateOut) *deviceStateOut = static_cast<uint32_t>(have);
    if (!agrees(have)) return false;
    if (plane < 0 && t.planes == 2 && !agrees(t.stencilState))
    {
        if (deviceStateOut) *deviceStateOut = static_cast<uint32_t>(t.stencilState);
        return false;
    }
    return true;
}

// The same book, as the single RHI state whose translation is exactly what it holds
// (ToD3D12ResourceState's inverse over the single states). An imported graph texture starts each
// Execute here. Plane 0 / 1 of a depth-stencil texture; -1 names a state only while both planes hold
// it.
RHI::ResourceState GraphicsDX12::QueryTextureState(const RHI::Texture& tex, int plane) const
{
    if (!tex.IsValid() || tex.handleId >= m_texturePool.size()) return RHI::ResourceState::UNDEFINED;
    const TextureDX12& t = m_texturePool[tex.handleId];
    if (plane < 0 && t.planes == 2 && t.state != t.stencilState) return RHI::ResourceState::UNDEFINED;
    const D3D12_RESOURCE_STATES have = (plane == 1 && t.planes == 2) ? t.stencilState : t.state;
    if (have == D3D12_RESOURCE_STATE_COMMON) return RHI::ResourceState::UNDEFINED;
    for (uint32_t bit = 0; bit < 32; ++bit)
    {
        const auto s = static_cast<RHI::ResourceState>(1u << bit);
        if (ToD3D12ResourceState(s) == have) return s;
    }
    return RHI::ResourceState::UNDEFINED;
}

// The D3D12 object's name, read the way the barrier journal reads it (DebugNameOf), so the graph's
// snapshot and the journal name an imported texture with one string.
std::string GraphicsDX12::QueryTextureName(const RHI::Texture& tex) const
{
    if (!tex.IsValid() || tex.handleId >= m_texturePool.size()) return {};
    return DebugNameOf(m_texturePool[tex.handleId].resource.Get());
}

// ==== IGraphicsDevice: render targets, heap binding, buffer mapping, resource destruction ====

void GraphicsDX12::SetRenderTargets(uint32_t numRTs,
                                     const RHI::Texture* const* rtvs,
                                     const RHI::Texture*        dsv,
                                     RHI::CommandList           cmd,
                                     const RHI::DepthView&      view)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;

    // Clamp what reaches D3D12 to the 8 handles the loop fills.
    if (!ENGINE_CHECK_ONCE(numRTs <= 8, "SetRenderTargets: %u render targets requested, max 8", numRTs))
        numRTs = 8;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    for (uint32_t i = 0; i < numRTs; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        const uint32_t id = rtvs[i]->handleId;
        // A live texture with no RTV would bind a {ptr=0} descriptor, which
        // D3D12 takes without a word outside the debug layer and draws nowhere.
        if (!ENGINE_CHECK_ONCE(id < m_texturePool.size() && m_texturePool[id].rtv.IsValid(),
                               "SetRenderTargets: RT slot %u texture %u has no RTV (created "
                               "without BindFlag::RENDER_TARGET?) - binding null", i, id))
        { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[id].rtv.GetCpuHandle();
    }

    if (dsv && dsv->IsValid())
    {
        const uint32_t did = dsv->handleId;
        if (!ENGINE_CHECK_ONCE(did < m_texturePool.size() && m_texturePool[did].dsv.IsValid(),
                               "SetRenderTargets: depth texture %u has no DSV - binding none", did))
        {
            ClearDepthView(entry);
            cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, nullptr);
            return;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = BindDepthView(entry, *dsv, view);
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        ClearDepthView(entry);
        cl->OMSetRenderTargets(numRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::SetRenderTargetsAndHdr(uint32_t numRTs,
                                           const RHI::Texture* const* rtvs,
                                           const RHI::Texture*        dsv,
                                           RHI::CommandList           cmd,
                                           const RHI::DepthView&      view)
{
    CommandListDX12& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;
    TextureDX12* rec = HdrRecord();
    if (!rec || !rec->rtv.IsValid()) return;

    // Transition HDR to RENDER_TARGET if needed (mirrors SetRenderTargetToHdr). Normally a no-op: Phase A
    // already forced RT on this pass's own CL. It fires only if some pass left the tracker off
    // RENDER_TARGET, which from a parallel Phase B body is exactly the race the guard reports.
    if (rec->state != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        WarnOnRacyHdrStateChange("SetRenderTargetsAndHdr");
        auto b = CD3DX12_RESOURCE_BARRIER::Transition(
            rec->resource.Get(), rec->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        cl->ResourceBarrier(1, &b);
        JournalNativeBarriers(cmd, &b, 1);
        rec->state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Build the RTV array: caller-supplied RTs first, HDR appended last, within D3D12's 8-RT limit
    // (numRTs + 1 <= 8).
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[8];
    const uint32_t capped = (numRTs > 7) ? 7u : numRTs;
    for (uint32_t i = 0; i < capped; ++i)
    {
        if (!rtvs[i] || !rtvs[i]->IsValid()) { rtvHandles[i] = {}; continue; }
        rtvHandles[i] = m_texturePool[rtvs[i]->handleId].rtv.GetCpuHandle();
    }
    rtvHandles[capped] = rec->rtv.GetCpuHandle();
    const uint32_t totalRTs = capped + 1;

    if (dsv && dsv->IsValid())
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = BindDepthView(entry, *dsv, view);
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, &dsvHandle);
    }
    else
    {
        ClearDepthView(entry);
        cl->OMSetRenderTargets(totalRTs, rtvHandles, FALSE, nullptr);
    }
}

void GraphicsDX12::ClearRenderTarget(const RHI::Texture& texture,
                                      const float         color[4],
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearRenderTargetView(
        m_texturePool[texture.handleId].rtv.GetCpuHandle(), color, 0, nullptr);
}

void GraphicsDX12::ClearDepthStencil(const RHI::Texture& texture,
                                      float               depth,
                                      uint8_t             stencil,
                                      RHI::CommandList    cmd)
{
    if (!texture.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    auto& tex = m_texturePool[texture.handleId];

    // Also clear the STENCIL plane when the format carries one. The deferred
    // pipeline stencil-gates LightingPass on per-pixel shading-model ids that
    // GBuffer writes via REPLACE; if stencil is never cleared, pixels NOT
    // rasterized this frame keep stale ids and get re-shaded next frame. That's
    // invisible when geometry+sky cover every pixel, but Wireframe view (sparse
    // edge coverage + suppressed sky) makes the stale ids accumulate as a
    // "never cleared" smear. Gate on a stencil-bearing format so depth-only
    // targets (shadow maps, D32) don't trip the debug layer.
    D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
    if (tex.resource)
    {
        const DXGI_FORMAT f = tex.resource->GetDesc().Format;
        if (f == DXGI_FORMAT_D24_UNORM_S8_UINT  || f == DXGI_FORMAT_R24G8_TYPELESS ||
            f == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || f == DXGI_FORMAT_R32G8X24_TYPELESS)
            flags |= D3D12_CLEAR_FLAG_STENCIL;
    }
    cl->ClearDepthStencilView(
        tex.dsv.GetCpuHandle(),
        flags, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetDepthStencilSlice(const RHI::Texture& depthTex,
                                         uint32_t            arraySlice,
                                         RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handleId];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = tex.sliceDsvs[arraySlice].GetCpuHandle();
    ClearDepthView(GetPoolEntry(cmd));   // writable, nothing checked
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
}

// Bind one (arraySlice, mip) subresource of a render target as the sole RT. Backs
// IGraphicsDevice::SetRenderTargetSubresource, so the reflection-probe capture pass needs no CPU
// descriptor handle or OMSetRenderTargets of its own.
void GraphicsDX12::SetRenderTargetSubresource(const RHI::Texture& rt,
                                              uint32_t            arraySlice,
                                              uint32_t            mip,
                                              const RHI::Texture* ds,
                                              RHI::CommandList    cmd)
{
    if (!rt.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    // GetTextureCubeFaceRTVCpuHandle keys on (cube, face, mip) and lazily
    // allocates, which is exactly a flat (arraySlice, mip) lookup once the
    // slice is split into cube/face. Non-cube arrays land on cube 0.
    const uint64_t rtvPtr = GetTextureCubeFaceRTVCpuHandle(
        rt, arraySlice / 6u, arraySlice % 6u, mip);
    if (rtvPtr == 0) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{ static_cast<SIZE_T>(rtvPtr) };

    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    bool hasDsv = false;
    if (ds && ds->IsValid())
    {
        auto& dsEntry = m_texturePool[ds->handleId];
        if (dsEntry.dsv.IsValid()) { dsv = dsEntry.dsv.GetCpuHandle(); hasDsv = true; }
    }
    ClearDepthView(GetPoolEntry(cmd));   // writable, nothing checked
    cl->OMSetRenderTargets(1, &rtv, FALSE, hasDsv ? &dsv : nullptr);
}

// Clears the subresource SetRenderTargetSubresource binds, through the same lazily made (cube, face,
// mip) RTV.
void GraphicsDX12::ClearRenderTargetSubresource(const RHI::Texture& rt,
                                                uint32_t            arraySlice,
                                                uint32_t            mip,
                                                const float         color[4],
                                                RHI::CommandList    cmd)
{
    if (!rt.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    const uint64_t rtvPtr = GetTextureCubeFaceRTVCpuHandle(
        rt, arraySlice / 6u, arraySlice % 6u, mip);
    if (rtvPtr == 0) return;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv{ static_cast<SIZE_T>(rtvPtr) };
    cl->ClearRenderTargetView(rtv, color, 0, nullptr);
}

void GraphicsDX12::SetDepthStencilTarget(const RHI::Texture& depthTex,
                                          RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handleId];
    if (!tex.dsv.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = tex.dsv.GetCpuHandle();
    ClearDepthView(GetPoolEntry(cmd));   // writable, nothing checked
    cl->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
}

void GraphicsDX12::ClearDepthStencilSlice(const RHI::Texture& depthTex,
                                           uint32_t            arraySlice,
                                           float               depth,
                                           uint8_t             stencil,
                                           RHI::CommandList    cmd)
{
    if (!depthTex.IsValid()) return;
    auto& tex = m_texturePool[depthTex.handleId];
    if (arraySlice >= tex.sliceDsvs.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->ClearDepthStencilView(tex.sliceDsvs[arraySlice].GetCpuHandle(),
                              D3D12_CLEAR_FLAG_DEPTH, depth, stencil, 0, nullptr);
}

void GraphicsDX12::SetStencilRef(uint32_t ref, RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->OMSetStencilRef(ref);
}

void GraphicsDX12::SetGraphicsRootConstant(RHI::Slots::GfxParam rootSlot,
                                            uint32_t value,
                                            uint32_t offsetInWords,
                                            RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    cl->SetGraphicsRoot32BitConstant(rootSlot.index, value, offsetInWords);
}

uint64_t GraphicsDX12::GetTextureMipUAVGpuHandle(const RHI::Texture& tex,
                                                  uint32_t mip) const
{
    if (!tex.IsValid()) return 0;
    const auto& entry = m_texturePool[tex.handleId];
    if (mip >= entry.mipUavs.size()) return 0;
    if (!entry.mipUavs[mip].IsValid()) return 0;
    return entry.mipUavs[mip].GetGpuHandle().ptr;
}

// ==== Lazy per-cube descriptor views (reflection probe capture / prefilter) ====
// These mutate the texture entry on the first request to allocate a CPU descriptor in the right
// heap; later calls are constant-time.
uint64_t GraphicsDX12::GetTextureCubeFaceRTVCpuHandle(const RHI::Texture& tex,
                                                       uint32_t cubeIdx,
                                                       uint32_t face,
                                                       uint32_t mip)
{
    if (!tex.IsValid() || face >= 6) return 0;
    auto& entry = m_texturePool[tex.handleId];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 16) | (face << 8) | mip;
    auto it = entry.cubeFaceRtvs.find(key);
    if (it != entry.cubeFaceRtvs.end())
        return it->second.GetCpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t arraySlice = cubeIdx * 6u + face;
    if (arraySlice >= rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_rtvAllocator.Allocate(1,
                                                         "CubeFaceRTV");
    if (!alloc.IsValid()) return 0;

    D3D12_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format = rd.Format;
    rtvd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvd.Texture2DArray.MipSlice        = mip;
    rtvd.Texture2DArray.FirstArraySlice = arraySlice;
    rtvd.Texture2DArray.ArraySize       = 1;
    rtvd.Texture2DArray.PlaneSlice      = 0;
    m_device->CreateRenderTargetView(entry.resource.Get(), &rtvd, alloc.GetCpuHandle());

    auto [emplaceIt, _ok] = entry.cubeFaceRtvs.emplace(key, alloc);
    return emplaceIt->second.GetCpuHandle().ptr;
}

uint64_t GraphicsDX12::GetTextureCubeMipUAVGpuHandle(const RHI::Texture& tex,
                                                      uint32_t cubeIdx,
                                                      uint32_t mip)
{
    if (!tex.IsValid()) return 0;
    auto& entry = m_texturePool[tex.handleId];
    if (!entry.resource) return 0;

    const uint32_t key = (cubeIdx << 8) | mip;
    auto it = entry.cubeMipUavs.find(key);
    if (it != entry.cubeMipUavs.end())
        return it->second.GetGpuHandle().ptr;

    const D3D12_RESOURCE_DESC rd = entry.resource->GetDesc();
    const uint32_t firstSlice = cubeIdx * 6u;
    if (firstSlice + 6u > rd.DepthOrArraySize) return 0;

    DescriptorAllocation alloc = m_cbvSrvUavAllocator.Allocate(1);
    if (!alloc.IsValid()) return 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavd{};
    uavd.Format = rd.Format;
    uavd.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    uavd.Texture2DArray.MipSlice        = mip;
    uavd.Texture2DArray.FirstArraySlice = firstSlice;
    uavd.Texture2DArray.ArraySize       = 6;
    uavd.Texture2DArray.PlaneSlice      = 0;
    m_cbvSrvUavAllocator.WriteUav(entry.resource.Get(), nullptr, &uavd, alloc.GetCpuHandle());
    alloc.CopyToGpu();

    auto [emplaceIt, _ok] = entry.cubeMipUavs.emplace(key, alloc);
    return emplaceIt->second.GetGpuHandle().ptr;
}


uint32_t GraphicsDX12::BeginGPUTimestampNoTracy(RHI::CommandList cmd, const char* name)
{
    if (!m_gpuProfiler.enabled) return ~0u;
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return ~0u;
    // CL pool entry already tracks which queue it was opened on; just forward
    // it so the profiler can split totals + compute the critical path.
    const uint32_t region = m_gpuProfiler.BeginTimestamp(cl, name, static_cast<uint8_t>(entry.queue));
    // What the list has counted so far; the end annotates the difference. Region indices are unique per
    // frame, so no lock.
    if (region < GPUProfiler::kMaxRegions)
    {
        m_regionBegin[region]     = entry.counters;
        m_regionBeginList[region] = entry.id;
    }
    return region;
}

void GraphicsDX12::EndGPUTimestampNoTracy(RHI::CommandList cmd, uint32_t regionIndex)
{
    if (regionIndex == ~0u) return;
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;
    m_gpuProfiler.EndTimestamp(cl, regionIndex);
    // What the region's list recorded in between (a region that ended on another list than it began on
    // is left uncounted).
    if (regionIndex < GPUProfiler::kMaxRegions && entry.IsValid() && m_regionBeginList[regionIndex] == entry.id)
        m_gpuProfiler.AnnotateRegion(regionIndex, entry.counters.Since(m_regionBegin[regionIndex]));
}

void GraphicsDX12::SetRecordingOwner(uintptr_t owner)
{
    t_recordingOwner = owner;
}

Diag::Prof::GpuCounters GraphicsDX12::GetOwnerCounters(uintptr_t owner)
{
    Diag::Prof::GpuCounters sum;
    if (owner == 0) return sum;
    std::lock_guard<std::mutex> lk(m_commandListMutex);   // BeginCommandList sets owners under it
    const uint32_t n = (std::min)(m_commandListCount.load(std::memory_order_relaxed), kMaxCommandLists);
    for (uint32_t i = 0; i < n; ++i)
        if (const CommandListDX12* e = m_commandLists[i]; e && e->owner == owner)
            sum += e->counters;
    sum.valid = true;
    return sum;
}

void GraphicsDX12::AnnotateGPUTimestamp(uint32_t regionIndex, const Diag::Prof::GpuCounters& counters)
{
    m_gpuProfiler.AnnotateRegion(regionIndex, counters);
}

void GraphicsDX12::BeginTracyGPUZone(RHI::CommandList cmd, const char* name,
                                     uint32_t regionIndex)
{
#ifdef TRACY_ENABLE
    if (regionIndex == ~0u || regionIndex >= GPUProfiler::kMaxRegions) return;
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;
    // Routed to the context for this CL's queue and keyed by the profiler region, which is unique per
    // pass, so concurrent callers never touch the same zones[] slot. Tracy's own state is thread-safe
    // here (atomic query ids + serial queue), but the begin and the matching end must run on the SAME
    // thread while `cmd` is still open.
    const uint8_t q = static_cast<uint8_t>(entry.queue);
    if (m_tracy && q < 3 && m_tracy->ctx[q])
    {
        const char* zn = (name && *name) ? name : "GPU";
        m_tracy->zones[regionIndex].emplace(
            m_tracy->ctx[q],
            static_cast<uint32_t>(__LINE__), __FILE__, sizeof(__FILE__) - 1,
            "GPUPass", sizeof("GPUPass") - 1,
            zn, std::strlen(zn),
            cl, true);
    }
#else
    (void)cmd; (void)name; (void)regionIndex;
#endif
}

void GraphicsDX12::EndTracyGPUZone(uint32_t regionIndex)
{
#ifdef TRACY_ENABLE
    // The destructor records the end-query + ResolveQueryData into the command
    // list captured at begin time, so this must run while that CL is open.
    if (m_tracy && regionIndex != ~0u && regionIndex < GPUProfiler::kMaxRegions)
        m_tracy->zones[regionIndex].reset();
#else
    (void)regionIndex;
#endif
}

// Combined form for the serial call sites (Renderer, subsystem passes): the Tracy zone opens and
// closes on one thread with correct nesting, so coupling it to the profiler region is fine there.
// RenderGraph must NOT use this: see IGraphicsDevice::BeginGPUTimestampNoTracy.
uint32_t GraphicsDX12::BeginGPUTimestamp(RHI::CommandList cmd, const char* name)
{
    const uint32_t region = BeginGPUTimestampNoTracy(cmd, name);
    BeginTracyGPUZone(cmd, name, region);
    // The serial call sites are exactly the passes OUTSIDE the graph (post
    // chain, SSR, HiZ, sims, overlays) plus sub-scopes inside graph passes, so
    // opening the capture marker here names all of them.
    // Gated inside BeginEventMarker; independent of the profiler.
    BeginEventMarker(cmd, name);
    return region;
}

void GraphicsDX12::EndGPUTimestamp(RHI::CommandList cmd, uint32_t regionIndex)
{
    // BEFORE the disabled-profiler early-out: the marker was opened whether or
    // not the profiler handed out a region, so it must be closed the same way.
    EndEventMarker(cmd);
    if (regionIndex == ~0u) return;
    // Tracy zone first: it records into the still-open CL, before the
    // profiler's own end-timestamp.
    EndTracyGPUZone(regionIndex);
    EndGPUTimestampNoTracy(cmd, regionIndex);
}

// Debug markers in the PIX UNICODE event format: Metadata 0 (WINPIX_EVENT_UNICODE_VERSION in
// WinPixEventRuntime's pix3_win.h; 1 is ANSI, 2 a PIX3 binary blob) and a NUL-terminated UTF-16
// string. It needs no WinPixEventRuntime dependency and RenderDoc / PIX both decode it, so every pass
// shows up named in a capture. Do not pass 2: tools then parse the string as a PIX3 blob, names come
// out as garbage, and the debug layer counts no Begin for any End.
void GraphicsDX12::BeginEventMarker(RHI::CommandList cmd, const char* name)
{
    if (!m_debugMarkers) return;          // opt-in (see SetDebugMarkersEnabled)
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;
    const char* n = (name && *name) ? name : "<pass>";
    wchar_t wbuf[128];
    int wn = MultiByteToWideChar(CP_UTF8, 0, n, -1, wbuf, _countof(wbuf));
    if (wn <= 0) { wbuf[0] = L'\0'; wn = 1; }           // null-terminated fallback
    wbuf[_countof(wbuf) - 1] = L'\0';                    // guarantee termination
    cl->BeginEvent(0 /*WINPIX_EVENT_UNICODE_VERSION*/, wbuf,
                   static_cast<UINT>(wn) * sizeof(wchar_t));
    ++entry.markerDepth;
    m_markerEvents.fetch_add(1, std::memory_order_relaxed);   // [GPUMARKERS] report
}

void GraphicsDX12::EndEventMarker(RHI::CommandList cmd)
{
    if (!m_debugMarkers) return;          // opt-in (see SetDebugMarkersEnabled)
    auto& entry = GetPoolEntry(cmd);
    auto* cl = entry.GetCommandList();
    if (!cl) return;
    if (entry.markerDepth <= 0) return;   // suppress an unmatched End (depth 0)
    --entry.markerDepth;
    cl->EndEvent();
}

// "cmd<id>:<name>" on the list's current queue object, so DRED's breadcrumb report, debug-layer
// messages and captures say what was recording instead of a bare pool slot. Only while markers or
// DRED are on: a SetName per list per frame is not free.
void GraphicsDX12::SetCommandListName(RHI::CommandList cmd, const char* name)
{
    if (cmd.internalId >= m_commandListCount.load(std::memory_order_relaxed)) return;
    auto& entry = GetPoolEntry(cmd);
    // The CPU copy first, for every list whether or not markers are on: the
    // frame snapshot names lists by it. A pointer store.
    entry.name = name;
    if ((!m_debugMarkers && !m_dredActive) || !name || !*name) return;
    const UINT qi = static_cast<UINT>(entry.queue);
    auto& list = entry.commandLists[qi];
    if (!list) return;
    wchar_t wbuf[160];
    const int prefix = swprintf_s(wbuf, _countof(wbuf), L"cmd%u:", cmd.internalId);
    if (prefix < 0) return;
    const int wn = MultiByteToWideChar(CP_UTF8, 0, name, -1, wbuf + prefix,
                                       static_cast<int>(_countof(wbuf)) - prefix);
    if (wn <= 0) wbuf[prefix - 1] = L'\0'; // name did not fit or convert: keep "cmd<id>"
    wbuf[_countof(wbuf) - 1] = L'\0';
    list->SetName(wbuf);
    entry.namedListMask |= 1u << qi;
    m_markerNamed.fetch_add(1, std::memory_order_relaxed);    // [GPUMARKERS] report
}

void GraphicsDX12::BindDescriptorHeaps(RHI::CommandList cmd)
{
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;
    ID3D12DescriptorHeap* heaps[] =
    {
        m_cbvSrvUavAllocator.GetHeap(),
        m_samplerAllocator.GetHeap()
    };
    cl->SetDescriptorHeaps(2, heaps);
}

void* GraphicsDX12::MapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return nullptr;
    void* ptr = nullptr;

    // The range tells the runtime what the CPU intends to READ. {0,0} means
    // "nothing", which is right for an UPLOAD buffer and WRONG for a READBACK
    // one: on a discrete GPU it happens to work, but it is a promise the
    // caller then breaks, and the runtime is entitled to skip making the
    // GPU writes visible. Ask for the whole buffer when it is READBACK.
    const bool readback = buffer.desc.usage == RHI::Usage::READBACK;
    D3D12_RANGE rr{ 0, readback ? static_cast<SIZE_T>(buffer.desc.size) : 0 };
    m_bufferPool[buffer.handleId].resource->Map(0, &rr, &ptr);
    return ptr;
}

void GraphicsDX12::UnmapBuffer(const RHI::GPUBuffer& buffer)
{
    if (!buffer.IsValid()) return;
    m_bufferPool[buffer.handleId].resource->Unmap(0, nullptr);
}

void GraphicsDX12::CopyTexturePixelToBuffer(const RHI::Texture& src,
                                             uint32_t srcX, uint32_t srcY,
                                             RHI::GPUBuffer& dst,
                                             RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    TextureDX12&  srcEntry = m_texturePool[src.handleId];
    GPUBufferDX12& dstEntry = m_bufferPool[dst.handleId];

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcEntry.resource.Get();
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource                          = dstEntry.resource.Get();
    dstLoc.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset             = 0;
    dstLoc.PlacedFootprint.Footprint.Format   = ToDxgiFormat(src.desc.format);
    dstLoc.PlacedFootprint.Footprint.Width    = 1;
    dstLoc.PlacedFootprint.Footprint.Height   = 1;
    dstLoc.PlacedFootprint.Footprint.Depth    = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    const D3D12_BOX srcBox = { srcX, srcY, 0u, srcX + 1u, srcY + 1u, 1u };
    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &srcBox);
}

bool GraphicsDX12::GetTexture2DReadbackFootprint(const RHI::Texture& tex,
                                                 uint32_t& outRowPitch,
                                                 uint32_t& outRows,
                                                 uint64_t& outTotalBytes) const
{
    outRowPitch = outRows = 0;
    outTotalBytes = 0;
    ID3D12Resource* res = GetTextureResource(tex);
    if (!res) return false;
    const D3D12_RESOURCE_DESC desc = res->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return false;

    // A planar format (NV12 / P010) reads back both planes, the second at the
    // next 512-byte placement after the first (the video probe); the pitch and
    // rows returned are the first plane's.
    const UINT planes = (desc.Format == DXGI_FORMAT_NV12 || desc.Format == DXGI_FORMAT_P010) ? 2u : 1u;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[2]{};
    UINT   numRows[2]      = {};
    UINT64 rowSizeBytes[2] = {};
    UINT64 totalBytes      = 0;
    m_device->GetCopyableFootprints(&desc, 0, planes, 0,
                                    fp, numRows, rowSizeBytes, &totalBytes);
    outRowPitch   = fp[0].Footprint.RowPitch;
    outRows       = numRows[0];
    outTotalBytes = totalBytes;
    return totalBytes != 0;
}

void GraphicsDX12::CopyTexture2DToBuffer(const RHI::Texture& src,
                                         const RHI::GPUBuffer& dst,
                                         RHI::CommandList cmd)
{
    if (!src.IsValid() || !dst.IsValid()) return;
    if (src.handleId >= m_texturePool.size() ||
        dst.handleId >= m_bufferPool.size()) return;
    auto* cl = GetPoolEntry(cmd).GetCommandList();
    if (!cl) return;

    ID3D12Resource* srcRes = m_texturePool[src.handleId].resource.Get();
    ID3D12Resource* dstRes = m_bufferPool[dst.handleId].resource.Get();
    if (!srcRes || !dstRes) return;

    // The footprint is asked of the DEVICE rather than computed from width and
    // a bytes-per-pixel table: the row pitch is 256-aligned and the total size
    // is what the READBACK buffer had to be created with, and re-deriving
    // either by hand is how a copy silently writes past the end of a buffer the
    // debug layer then reports as a completely different resource's corruption.
    const D3D12_RESOURCE_DESC srcDesc = srcRes->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    m_device->GetCopyableFootprints(&srcDesc, 0, 1, 0, &fp, nullptr, nullptr, nullptr);

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource        = srcRes;
    srcLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource       = dstRes;
    dstLoc.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint = fp;

    cl->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, /*pSrcBox*/ nullptr);
}


void GraphicsDX12::DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource)
{
    if (!resource) return;
    // Same deferral as Destroy{Buffer,Texture}: the resource joins the bucket stamped
    // ReleaseStampFrame(), so it outlives every list recorded this frame, and BeginFrame's
    // DrainReleasesUpTo frees it once that frame retires. No pool slot to recycle: this is a
    // non-pool resource.
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    PendingReleaseBucket().resources.push_back(std::move(resource));
}

void GraphicsDX12::DeferReleaseResource(Microsoft::WRL::ComPtr<ID3D12Resource> resource,
                                        uint32_t queueIndex,
                                        uint64_t fenceValue)
{
    if (!resource) return;
    if (queueIndex >= 3)
    {
        // Bad queue index: fall back to the frame-stamped defer so the resource is still freed.
        DeferReleaseResource(std::move(resource));
        return;
    }
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    m_fenceKeyedReleases.push_back({ queueIndex, fenceValue, std::move(resource) });
}

// See the declaration for why a static descriptor block needs this. The bucket is the SAME one
// DestroyHdrTarget / DestroyTexture use, so the slots are freed by
// DrainReleasesUpTo(m_renderFrame - kFrameCount), the device's own retired-frame predicate, rather
// than by a caller's count of quiet frames. ProcessAllDeferredReleases (WaitForPreviousFrame,
// ~GraphicsDX12) drains with UINT64_MAX, so nothing is stranded at shutdown.
void GraphicsDX12::DeferReleaseDescriptors(DescriptorAllocation alloc)
{
    if (!alloc.IsValid()) return;
    std::lock_guard<std::mutex> lock(m_deferredMutex);
    PendingReleaseBucket().descriptors.push_back(alloc);
}

// A pass's block of null structured-buffer SRVs (LightingPass's probe grid / index / probe buffer).
// One AllocateStatic, written straight into the shader-visible heap through its CPU handle (as
// DDGIVolumeManager::Init does), so no CopyToGpu step. The allocation stays in m_descriptorBlocks;
// the pass holds the RHI handle.
bool GraphicsDX12::CreateNullStructuredSrvBlock(const uint32_t* strides, uint32_t count,
                                                RHI::DescriptorBlock& out)
{
    out = {};
    if (!strides || count == 0) return false;
    DescriptorAllocation block = m_cbvSrvUavAllocator.AllocateStatic(count);
    if (!block.IsValid()) return false;
    const UINT descSize = m_cbvSrvUavAllocator.GetDescriptorSize();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = block.GetGpuCpuHandle();
    for (uint32_t i = 0; i < count; ++i, cpu.ptr += descSize)
        CreateNullStructuredBufferSRVAtCpu(strides[i], cpu);
    RegisterDescriptorBlock(block, count, out);
    return true;
}

void GraphicsDX12::RegisterDescriptorBlock(const DescriptorAllocation& block, uint32_t count,
                                           RHI::DescriptorBlock& out)
{
    std::lock_guard<std::mutex> lock(m_descriptorBlockMutex);
    uint32_t id;
    if (!m_freeDescriptorBlockIds.empty())
    {
        id = m_freeDescriptorBlockIds.back();
        m_freeDescriptorBlockIds.pop_back();
        m_descriptorBlocks[id] = block;
    }
    else
    {
        id = static_cast<uint32_t>(m_descriptorBlocks.size());
        m_descriptorBlocks.push_back(block);
    }
    out.handleId = id;
    out.type      = RHI::GPUResource::Type::DescriptorBlock;
    out.gpuBase   = block.GetGpuHandle().ptr;
    out.cpuBase   = block.GetGpuCpuHandle().ptr;
    out.increment = m_cbvSrvUavAllocator.GetDescriptorSize();
    out.count     = count;
}

// The same static-region block, left for the caller to write (PreviewRenderer's space3 table, the
// reflection probes' cube views).
bool GraphicsDX12::AllocateDescriptorBlock(uint32_t count, RHI::DescriptorBlock& out)
{
    out = {};
    if (count == 0) return false;
    DescriptorAllocation block = m_cbvSrvUavAllocator.AllocateStatic(count);
    if (!block.IsValid()) return false;
    RegisterDescriptorBlock(block, count, out);
    return true;
}

// A TextureCubeArray SRV in the texture's own format (a colour texture's resource format IS its view
// format; only depth resources are made typeless), cubes from face firstCube * 6, mips from 0.
bool GraphicsDX12::WriteTextureCubeArraySrv(const RHI::DescriptorBlock& block, uint32_t index,
                                            const RHI::Texture& texture, uint32_t firstCube,
                                            uint32_t numCubes, uint32_t mipLevels)
{
    if (!block.IsValid() || index >= block.count || block.cpuBase == 0 || numCubes == 0 || mipLevels == 0)
        return false;
    ID3D12Resource* res = GetTextureResource(texture);
    if (!res) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                               = res->GetDesc().Format;
    srv.ViewDimension                        = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    srv.Shader4ComponentMapping              = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCubeArray.MostDetailedMip     = 0;
    srv.TextureCubeArray.MipLevels           = mipLevels;
    srv.TextureCubeArray.First2DArrayFace    = firstCube * 6;
    srv.TextureCubeArray.NumCubes            = numCubes;
    srv.TextureCubeArray.ResourceMinLODClamp = 0.0f;
    m_cbvSrvUavAllocator.WriteSrv(res, &srv,
        D3D12_CPU_DESCRIPTOR_HANDLE{ static_cast<SIZE_T>(block.Cpu(index)) });   // the kind is recorded
    return true;
}

// QUIESCED (the caller proved the frames that could name the block retired):
// Free now, so the slots are back on the allocator's free list before a
// rebuild's AllocateStatic asks for the same size and the rebuild takes the
// same block (preview_teardown gates the static high-water drift at 0).
// Otherwise the frame-stamped bucket. The allocation leaves the pool BY VALUE
// and its slot is emptied, so the bucket - or nobody, after Free - is its only
// owner: DescriptorAllocation is memberwise-copyable with no destructor, and a
// copy left behind would still name the block.
void GraphicsDX12::ReleaseDescriptorBlock(RHI::DescriptorBlock& block, bool quiesced)
{
    if (!block.IsValid()) return;
    DescriptorAllocation alloc;
    {
        std::lock_guard<std::mutex> lock(m_descriptorBlockMutex);
        if (block.handleId < m_descriptorBlocks.size())
        {
            alloc = m_descriptorBlocks[block.handleId];
            m_descriptorBlocks[block.handleId] = DescriptorAllocation{};
            m_freeDescriptorBlockIds.push_back(block.handleId);
        }
    }
    block = {};
    if (!alloc.IsValid()) return;
    if (quiesced) alloc.Free();
    else          DeferReleaseDescriptors(alloc);
}

uint64_t GraphicsDX12::GetLastSignaledFenceValue(uint32_t queueIndex) const
{
    if (queueIndex >= 3) return 0;
    return m_queueFenceValues[queueIndex];
}

void GraphicsDX12::ProcessFenceKeyedReleases()
{
    std::deque<FenceKeyedRelease> drained;
    {
        std::lock_guard<std::mutex> lock(m_deferredMutex);
        // Walk the queue; move entries whose fence has completed into `drained`
        // so the ComPtr destruction happens OUTSIDE the lock (Release() could
        // re-enter graphics-device calls).
        for (auto it = m_fenceKeyedReleases.begin(); it != m_fenceKeyedReleases.end(); )
        {
            // The fence the value was signalled on (GetLastSignaledFenceValue), for every queue the graphics
            // one included: m_fence paces frames and counts them, not the graphics queue's batches.
            const uint32_t qi = it->queueIndex;
            ID3D12Fence* fence = m_queueFences[qi].Get();
            if (!fence)
            {
                // Fence not initialized: keep the entry (dropped later).
                ++it;
                continue;
            }
            if (fence->GetCompletedValue() >= it->fenceValue)
            {
                drained.push_back(std::move(*it));
                it = m_fenceKeyedReleases.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    // `drained` destructs here; ComPtrs release ID3D12Resources outside the lock.
}

// The adapter the boot's GPU report names, for the first boot's quality pick and Lua
// Settings.caps().
std::vector<RHI::AdapterInfo> GraphicsDX12::ListAdapters() const
{
    std::vector<RHI::AdapterInfo> out;
    if (!m_factory) return out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                           IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        RHI::AdapterInfo a;
        a.vendorId               = desc.VendorId;
        a.deviceId               = desc.DeviceId;
        a.dedicatedVideoMemoryMB = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024ull * 1024ull);
        a.sharedSystemMemoryMB   = static_cast<uint64_t>(desc.SharedSystemMemory) / (1024ull * 1024ull);
        a.description            = Sys::WideToUtf8(desc.Description);
        out.push_back(std::move(a));
    }
    return out;
}

RHI::AdapterInfo GraphicsDX12::GetAdapterInfo() const
{
    RHI::AdapterInfo a;
    a.vendorId               = m_adapterDesc.VendorId;
    a.deviceId               = m_adapterDesc.DeviceId;
    a.dedicatedVideoMemoryMB = static_cast<uint64_t>(m_adapterDesc.DedicatedVideoMemory) / (1024ull * 1024ull);
    a.sharedSystemMemoryMB   = static_cast<uint64_t>(m_adapterDesc.SharedSystemMemory) / (1024ull * 1024ull);
    a.description            = Sys::WideToUtf8(m_adapterDesc.Description);
    return a;
}

// Editor stats: dedicated (LOCAL segment) VRAM usage vs the OS budget. The same DXGI query as
// EndFrame's over-budget watch; the adapter3 lookup is resolved once and cached (single-device
// engine).
bool GraphicsDX12::QueryVramUsage(uint64_t& usedBytes, uint64_t& budgetBytes)
{
    static Microsoft::WRL::ComPtr<IDXGIAdapter3> s_adapter3 =
        [this]() {
            Microsoft::WRL::ComPtr<IDXGIAdapter3> a3;
            if (m_factory && m_device)
                m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(),
                                             IID_PPV_ARGS(&a3));
            return a3;
        }();

    DXGI_QUERY_VIDEO_MEMORY_INFO vm{};
    if (!s_adapter3 || FAILED(s_adapter3->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm)))
        return false;
    usedBytes   = vm.CurrentUsage;
    budgetBytes = vm.Budget;
    return true;
}
