#pragma once

// ReflectionProbeCapturePass: turns the Renderer's draw packets into a baked
// reflection cubemap, one probe at a time. Per probe:
//   (1) render the scene 6 times into a shared 128x128x6 temp TextureCube with
//       a simplified forward shader (sky + opaque, sun Lambert + SH ambient);
//   (2) GGX-prefilter it, 7 mips x 6 faces = 42 dispatches, into one cube of
//       the renderer's reflection-probe TextureCubeArray (slices
//       cubeIdx*6 .. cubeIdx*6+5).
// The pass is NOT a RenderGraph node: the Renderer drives it explicitly so it
// can spread the cost across frames (one probe per frame by default). The bake
// target slice and the scene state come in BakeContext.

#include "RenderGraph/PassLifecycle.h"
#include "Graphics/IGraphicsDevice.h"
#include "Graphics/GraphicsStruct.h"
#include "Graphics/ShaderLibrary.h"
#include "Graphics/RenderTypes.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <DirectXMath.h>

class ReflectionProbeCapturePass : public RG::PassLifecycle
{
public:
    static constexpr uint32_t kFaceSize = 128;
    static constexpr uint32_t kFaceMips = 7;  // log2(128) + 1
    // A face's lens: 90 degrees, square, reversed-Z between these planes.
    static constexpr float    kNearZ    = 0.1f;
    static constexpr float    kFarZ     = 1000.0f;

    struct BakeContext
    {
        RHI::Texture*           probeArray         = nullptr;  // destination cubemap-array
        DirectX::XMFLOAT3       probePos           { 0, 0, 0 }; // capture origin
        DirectX::XMFLOAT3       sunDir             { 0, -1, 0 };
        DirectX::XMFLOAT3       sunColor           { 1, 1, 1 };
        uint64_t                skySHSrv           = 0;        // t19 space0
        uint64_t                materialBufSrv     = 0;        // t2 space0
        uint64_t                bindlessTexTable   = 0;        // t0 space2
        uint64_t                bindlessBufTable   = 0;        // t0 space1 (PVF g_Buffers[])
        const RHI::GPUBuffer*   instanceBuffer     = nullptr;  // root SRV slot 8 (t0 space0)
        const RHI::GPUBuffer*   meshDescBuffer     = nullptr;  // root SRV slot 9 (t1 space0)
        // This frame's instance-remap heap index (0 = identity). The bake draws
        // reuse GBuffer.vs with draw-local instanceOffset root constants, so under
        // retained instances they need the same remap the main view uses.
        uint32_t                instanceRemapIdx   = 0;
        // What each face draws (Renderer::CullView with the face's own frustum):
        // this frame's opaque packets, then the shadow-only ones - entities the main
        // frustum rejected but a CSM cascade kept, so a probe also captures what is
        // behind the main camera. Iterated with the capture PSO in that order.
        DrawList                faceDraws[6];
        // Harness only (-probecull_legacy): a packet without a base colour texture
        // keeps the previous draw's t3, so its colour depends on the draw order.
        bool                    legacyTextureBind  = false;
        uint64_t                defaultWhiteSrv    = 0;        // fallback for t3 (BaseColor)
        uint64_t                defaultFlatNormSrv = 0;        // fallback for t5 (NormalMap)
        // Skybox draw: fills pixels the opaque pass left untouched. Borrows
        // SkyboxPass's 36-vertex cube VB (ByteAddressBuffer at t2 space0 in the sky
        // shader) and SkyIBLPass's active sky cubemap SRV.
        const RHI::GPUBuffer*   skyCubeVB          = nullptr;
        uint64_t                skyCubemapSrv      = 0;        // t6 space0

        // ---- Direct-sun shadowing for the bake (optional) ----
        // With shadowArraySrv != 0 the capture PS shadows the direct sun with the
        // main camera's CSM, so roofed interiors do not bake full sunlight. Cascades
        // are camera-relative: a probe outside their range bakes unshadowed. All
        // values are frame-global (the same for every probe baked this frame).
        uint64_t                shadowArraySrv     = 0;        // CSM Texture2DArray @ t9 space0 (root slot Gfx::ShadowSRV)
        DirectX::XMFLOAT4X4     cascadeVP[4]       {};         // transposed (matches LightCB.shadowMatrix)
        DirectX::XMFLOAT4       cascadeSplits      { 0,0,0,0 };
        DirectX::XMFLOAT3       camPos             { 0,0,0 };   // main camera (cascade selection)
        DirectX::XMFLOAT3       camFwd             { 0,0,1 };
        float                   shadowStrength     = 0.0f;     // 0 = no CSM this frame -> bake unshadowed
        float                   shadowTexel        = 1.0f / 4096.0f; // 1 / ShadowPass::GetShadowResolution() (PCF step)

        // ---- Indirect / ambient bake controls ----
        float                   iblStrength        = 1.0f;     // live sky-IBL master at bake time
        float                   ambientScale       = 1.0f;     // IndirectLightingSettings.reflectionProbeBakeAmbient
    };

    // Face @p face's view-projection from @p eye (0:+X 1:-X 2:+Y 3:-Y 4:+Z
    // 5:-Z), row-vector convention, NOT transposed - what ExtractFrustumPlanes
    // reads (each face is a view culled on its own). The CB gets its transpose.
    static DirectX::XMFLOAT4X4 FaceViewProj(const DirectX::XMFLOAT3& eye, uint32_t face);
    static DirectX::XMFLOAT3   FaceForward(uint32_t face);

    bool Init(IGraphicsDevice& gfx);
    // Hot reload (Renderer::ReloadShaders, through ReflectionProbeManager):
    // rebuilds the PSOs, so the NEXT bake uses the new shaders. Probes already
    // baked keep their cubes until something re-queues them.
    void ReloadShaders(IGraphicsDevice& gfx) override;
    uint32_t HandledEvents() const override { return kReloadShaders; }   // events this pass handles (Renderer::PassSet)

    // Bakes cube @p cubeIdx of ctx.probeArray: records the six face draws and
    // the prefilter dispatches onto @p cl. Expects the probe array in
    // SHADER_RESOURCE and leaves it there (it is UNORDERED_ACCESS in between).
    void BakeProbe(RHI::CommandList cl, uint32_t cubeIdx, const BakeContext& ctx);

private:
    bool CreatePipelines(IGraphicsDevice& gfx);

    void DrawSceneFace(RHI::CommandList cl, uint32_t face, const BakeContext& ctx);

    IGraphicsDevice* m_gfx = nullptr;

    ShaderLibrary       m_shaderLib;
    RHI::PipelineState  m_capturePSO;     // forward graphics PSO (1 RTV)
    // Same PSO with frontCounterClockwise=true, for MIRRORED instances (a
    // negative-determinant world matrix reverses winding; without it they bake
    // inside-out). Selected per DrawPacket::mirrored.
    RHI::PipelineState  m_capturePSOMirrored;
    RHI::PipelineState  m_skyPSO;         // skybox draw: fills sky pixels after opaque
    RHI::PipelineState  m_prefilterPSO;   // GGX prefilter compute (same shape as SkyIBLPass's)

    // 128x128 D32 depth for the per-face forward draw, cleared per face.
    RHI::Texture       m_depthBuffer;
    RHI::ResourceState m_depthState = RHI::ResourceState::DEPTHSTENCIL;

    // 128x128x6 R16G16B16A16_FLOAT TextureCube: capture mip 0 destination,
    // prefilter source. Lives as long as the pass so the descriptors behind the
    // per-face RTVs are reused. GraphicsDX12::CreateTexture starts RT-bound
    // resources in RENDER_TARGET, so the tracked state starts there too.
    RHI::Texture       m_tempCube;
    RHI::ResourceState m_tempCubeState = RHI::ResourceState::RENDERTARGET;

    // CB ring: the capture uploads 6 entries per probe (one per face), in
    // 256-byte slots so root CBV addresses can offset into the buffer.
    RHI::GPUBuffer m_captureCB;
    void*          m_captureCBMapped = nullptr;

    // Prefilter CB ring: 42 entries per probe (7 mips x 6 faces).
    RHI::GPUBuffer m_prefilterCB;
    void*          m_prefilterCBMapped = nullptr;

    // Per-probe shadow / ambient CB (b2): frame-global data (CSM cascades,
    // camera, iblStrength, ambientScale), uploaded once per BakeProbe and bound
    // on each face. One buffer is safe: the data is identical for every probe
    // baked in a frame.
    RHI::GPUBuffer m_shadowCB;
    void*          m_shadowCBMapped = nullptr;
};
