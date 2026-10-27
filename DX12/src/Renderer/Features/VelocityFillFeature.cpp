// Renderer/Features/VelocityFillFeature.cpp - the sky motion-vector fill as a
// render feature (Renderer/RenderFeature.h).
//
// The velocity buffer is complete for geometry and EMPTY for sky: SkyboxPass and
// the cloud composite bind colour only, so depth==0 pixels keep the GBuffer clear
// (zero motion). TAA hides that with its own synthesis; FSR 2 and DLSS read the
// buffer raw and would ghost the sky under camera rotation. VelocityFillPass repairs
// it once, in the shared buffer, at PreResolve: after every geometry writer, before
// every temporal consumer, and before the upscale branch calls
// TAAJitterState::AdvanceToNextFrame - so FrameResources' previous unjittered
// view-projection is still the previous frame's. Not gated on the AA mode: a correct
// velocity buffer is not an upscaler-specific concern. Ortho is skipped: the fill
// unprojects through the far plane, and the temporal passes skip ortho frames anyway.
//
// The Renderer keeps the velocity graph texture (declared isUAV for this fill) and
// the transition to compute-readable after it, which every temporal consumer needs
// whether the fill ran or not.

#include "Renderer/RenderFeature.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/VelocityFillPass.h"
#include "Renderer/PassSet.h"

#include <DirectXMath.h>

using namespace DirectX;

namespace
{
class VelocityFillFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "VelocityFill"; }
    uint32_t    Stages() const override { return StageBit(RG::Stage::PreResolve); }
    // After the RR guides feature (order 100) at the same stage.
    int         Order() const override { return 200; }

    void CreatePasses(FeatureCompileContext& ctx) override
    {
        m_pass = std::make_unique<VelocityFillPass>();
        m_pass->Init(ctx.gfx);
        ctx.passes.Register("VelocityFill", m_pass);
    }

    void Record(RG::Stage, FeatureRecordContext& ctx) override
    {
        const FrameResources& res = ctx.resources;
        const RHI::Texture* velocity = ctx.graph.GetPhysicalTexture(ctx.textures.velocity);
        if (!m_pass || !m_pass->IsEnabled() || !velocity || ctx.view.camera.orthographic || res.depthSrv == 0)
            return;
        const uint64_t velocityUav = ctx.gfx.GetTextureUAVGpuHandle(*velocity);
        if (!velocityUav)
            return;
        const RHI::ResourceState state = ctx.graph.GetTextureState(ctx.textures.velocity);
        if (state != RHI::ResourceState::UNORDERED_ACCESS)
        {
            ctx.gfx.PushBarrier(RHI::GPUBarrier::Image(velocity, state, RHI::ResourceState::UNORDERED_ACCESS), ctx.cl);
            ctx.graph.SetTextureState(ctx.textures.velocity, RHI::ResourceState::UNORDERED_ACCESS);
        }

        // Transposed for row-vector HLSL, same as the matrices fed to TAAPass
        // for its equivalent reconstruction.
        XMFLOAT4X4 prevVP, invVPNoJitter;
        XMStoreFloat4x4(&prevVP, XMMatrixTranspose(XMLoadFloat4x4(&res.prevViewProjNoJitter)));
        XMStoreFloat4x4(&invVPNoJitter,
                        XMMatrixTranspose(XMMatrixInverse(nullptr, XMLoadFloat4x4(&res.viewProjNoJitter))));
        m_pass->SetFrameData(invVPNoJitter, prevVP);
        m_pass->SetViewportSize(ctx.view.renderWidth, ctx.view.renderHeight);
        const uint32_t r = ctx.gfx.BeginGPUTimestamp(ctx.cl, "VelocityFill");
        m_pass->Execute(ctx.cl, res.depthSrv, velocityUav);
        ctx.gfx.EndGPUTimestamp(ctx.cl, r);
    }

private:
    std::unique_ptr<VelocityFillPass> m_pass;
};
}

REGISTER_RENDER_FEATURE(VelocityFillFeature)
