// Renderer/Features/RRGuideFeature.cpp - DLSS-RR's raster guides as a render
// feature.
//
// Ray reconstruction on a raster frame - a ReSTIR path, or beside the path tracer
// with -rr_raster_guides (=1 a shadow for the equality gate, =2 DLSS reads them
// instead of the path tracer's) - needs the four guides the path tracer writes for
// itself: diffuse albedo, specular albedo, normal + roughness, specular hit
// distance. RRGuidePass writes them from the GBuffer.
//   Prepare(PreResolve) (post phase, main thread, after the frame plan's first
//     stage-C pass): allocates them (~55 MiB at 1080p) only while the plan wants
//     them and reports whether that succeeded - stage C is planned again with
//     rrGuides = rrGuidesWanted && prepared. Last frame's guides are stale from
//     here on; a frame that will not write them frees them.
//   Record(PreResolve) (pre-compute list, after every GBuffer writer and before
//     the upscaler; Order 100, before the velocity fill): moves the GBuffer reads
//     to compute-readable via the graph tracker; the specular hit distance takes
//     ReSTIR DI's light distance (while RR denoises DI), the traced reflection's
//     kept hit distance and the sky cube. The written guides are
//     FrameResources::rrRasterGuides, which DLSS reads.
//   RegisterDebugTargets: the four rrRaster* rows, after the path tracer's.
//
// The Renderer keeps the frame plan's facts and decisions (one source - DLSS
// reads them too), whether DLSS reads the raster guides (DLSSReadsRasterRRGuides,
// for its RRInputsReady), and the frame resources the guides read.

#include "Renderer/RenderFeature.h"

#include "Graphics/IGraphicsDevice.h"
#include "RenderGraph/RenderPass/RRGuidePass.h"
#include "Renderer/PassSet.h"
#include "System/CommandLine.h"

namespace
{
class RRGuideFeature final : public IRenderFeature
{
public:
    const char* Name() const override { return "RRGuide"; }
    uint32_t    Stages() const override { return StageBit(RG::Stage::PreResolve); }
    // Before the velocity fill (Order 200).
    int         Order() const override { return 100; }

    void CreatePasses(FeatureCompileContext& ctx) override
    {
        // Owns nothing until ray reconstruction wants the guides; the pipeline
        // builds on first use.
        m_pass = std::make_unique<RRGuidePass>();
        ctx.passes.Register("RRGuide", m_pass);
    }

    void Prepare(RG::Stage, FeatureFrameContext& ctx) override
    {
        if (!m_pass)
            return;
        const bool prepared = ctx.plan.rrGuidesWanted &&
                              m_pass->Prepare(ctx.gfx, ctx.view.renderWidth, ctx.view.renderHeight);
        ctx.facts.rrGuidesPrepared = prepared;
        m_pass->MarkStale();
        if (!prepared)   // the plan's rrGuides: wanted and prepared
            m_pass->Release(ctx.gfx);
    }

    void Record(RG::Stage, FeatureRecordContext& ctx) override
    {
        FrameResources& res = ctx.resources;
        if (m_pass && ctx.plan.rrGuides && ctx.graph.GetPhysicalTexture(ctx.textures.depth))
        {
            auto computeRead = [&ctx](RG::RGTextureHandle h) -> uint32_t {
                const RHI::Texture* t = h.IsValid() ? ctx.graph.GetPhysicalTexture(h) : nullptr;
                if (!t || !t->IsValid()) return 0u;
                const RHI::ResourceState s = ctx.graph.GetTextureState(h);
                if (s != RHI::ResourceState::SHADER_RESOURCE && s != RHI::ResourceState::SHADER_RESOURCE_COMPUTE)
                {
                    ctx.gfx.PushBarrier(RHI::GPUBarrier::Image(t, s, RHI::ResourceState::SHADER_RESOURCE_COMPUTE), ctx.cl);
                    ctx.graph.SetTextureState(h, RHI::ResourceState::SHADER_RESOURCE_COMPUTE);
                }
                const uint64_t srv = ctx.gfx.GetTextureSRVGpuHandle(*t);
                const uint32_t index = srv ? ctx.gfx.DescriptorHeapIndexFromGpuHandle(srv) : ~0u;
                return index == ~0u ? 0u : index;
            };
            RRGuidePass::Inputs in{};
            in.viewProj = ctx.view.view.viewProjMatrix;   // jittered, as the GBuffer was drawn
            in.camera = ctx.view.camera.position;
            in.width = ctx.view.renderWidth; in.height = ctx.view.renderHeight;
            in.sunDirection = res.sunDirection;
            in.sunColor = res.sunColor;
            in.depthIdx = computeRead(ctx.textures.depth);
            in.albedoIdx = computeRead(ctx.textures.albedo);
            in.normalIdx = computeRead(ctx.textures.normal);
            in.surfaceIdx = computeRead(ctx.textures.surface);
            in.extraIdx = computeRead(ctx.textures.emissive);
            in.materialsIdx = res.materialsIdx;
            // Beside the path tracer (the -rr_raster_guides gate) its own GGX
            // and -pt_diffuse; on a raster path the lighting's shading models.
            if (res.pathTrace)
                in.flags = Sys::CommandLine::Get().Has("-pt_diffuse") ? RRGuidePass::kDiffuseOnly : 0u;
            else
                in.flags = RRGuidePass::kRasterModels;
            // The specular hit distance's sources: ReSTIR DI's light distance (its
            // Resolve writes it while RR denoises DI), the traced reflection with its
            // kept hit distance, the sky cube.
            if (res.diDistance)
            {
                in.diResultIdx = computeRead(ctx.textures.diResult);
                if (in.diResultIdx) in.flags |= RRGuidePass::kDiDistance;
            }
            in.reflectionIdx = res.rtReflectionIdx;
            in.reflectionDistanceIdx = res.rtReflectionDistanceIdx;
            in.radianceIdx = res.skyRadianceIdx;
            in.radianceMips = res.iblRadianceMips;
            in.iblStrength = res.iblStrength;
            const uint32_t r = ctx.gfx.BeginGPUTimestamp(ctx.cl, "RRGuides");
            m_pass->Execute(ctx.cl, ctx.gfx, in);
            ctx.gfx.EndGPUTimestamp(ctx.cl, r);
        }
        // What DLSS reads: this frame's guides, or none.
        res.rrRasterGuidesWritten = m_pass && m_pass->Wrote();
        for (uint32_t i = 0; i < 4; ++i)
            res.rrRasterGuides[i] = m_pass ? m_pass->GetGuide(i) : nullptr;
    }

    void RegisterDebugTargets(DebugTargetRegistry& registry) override
    {
        // The same four guides as the path tracer's rrAlbedo ... rrHitDistance,
        // which they must equal beside it (tools/verify_rr_guides.py --raster).
        static const char* const kMissing =
            "the raster RR guides were not written in the last frame (ray reconstruction on a raster path, or "
            "-rr_raster_guides beside the path tracer's GBuffer primary)";
        auto add = [&](uint32_t index, const char* name, const char* label, const char* semantic, const char* meaning,
                       const char* channels, DebugTargets::Mode mode, int channel)
        {
            FeatureDebugTarget t;
            t.info.name = name;
            t.info.label = label;
            t.info.group = "Ray Reconstruction";
            t.info.semantic = semantic;
            t.info.meaning = meaning;
            t.info.channels = channels;
            t.info.defaultMode = mode;
            t.info.defaultChannel = channel;
            t.after = "rrHitDistance";
            t.texture = [this, index](RHI::ResourceState& state) -> const RHI::Texture* {
                return m_pass ? m_pass->GetGuide(index, state) : nullptr;
            };
            t.missing = kMissing;
            registry.Add(std::move(t));
        };
        add(0, "rrRasterAlbedo", "RR Raster Guide: Diffuse Albedo", "color",
            "the raster DLSS-RR diffuse albedo guide: baseColor x (1 - metalness), linear, in [0,1]; 0 on the sky "
            "and unlit pixels; a clear coat's film attenuates it (RRGuidePass)",
            "r,g,b,a", DebugTargets::Mode::Raw, -1);
        add(1, "rrRasterSpecular", "RR Raster Guide: Specular Albedo", "color",
            "the raster DLSS-RR specular albedo guide: the split-sum F0 x A + B (EnvBRDFApprox); 0 on a toon pixel "
            "authored with reflectance 0; a clear coat adds its film (RRGuidePass)",
            "r,g,b,a", DebugTargets::Mode::Raw, -1);
        add(2, "rrRasterNormal", "RR Raster Guide: Normal + Roughness", "data",
            "the raster DLSS-RR normal guide: the signed world shading normal in xyz, perceptual roughness "
            "(>= 0.045) in a; (0, 0, 1, 1) on the sky and unlit pixels (RRGuidePass)",
            "x,y,z,roughness", DebugTargets::Mode::Signed, -1);
        add(3, "rrRasterHitDistance", "RR Raster Guide: Specular Hit Distance", "data",
            "the raster DLSS-RR specular hit distance in world units, energy-weighted over the specular light the "
            "pass sees (the DI light distance where the DI shell writes it, the sun at 10000) (RRGuidePass)",
            "distance", DebugTargets::Mode::Channel, 0);
    }

private:
    std::unique_ptr<RRGuidePass> m_pass;
};
}

REGISTER_RENDER_FEATURE(RRGuideFeature)
