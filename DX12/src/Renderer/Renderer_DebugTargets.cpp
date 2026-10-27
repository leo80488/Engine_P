// Renderer_DebugTargets.cpp - the debug target view (Renderer/DebugTargets.h): the registry
// of what the main view can show - and the pipe can capture as data - the asked-for view,
// and its paint on the frame's restore list.
#include "Renderer/Renderer.h"
#include "Renderer/DebugTargets.h"
#include "Graphics/FormatNames.h"

#include "RenderGraph/RenderPass/ClusterPass.h"
#include "RenderGraph/RenderPass/DebugTargetViewPass.h"
#include "RenderGraph/RenderPass/DecalPass.h"
#include "RenderGraph/RenderPass/PathTracePass.h"
#include "RenderGraph/RenderPass/RRGuidePass.h"
#include "RenderGraph/RenderPass/RTAOPass.h"
#include "RenderGraph/RenderPass/RTShadowPass.h"
#include "RenderGraph/RenderPass/SpotShadowPass.h"
#include "RenderGraph/RenderPass/ToneMapPass.h"
#include "RenderGraph/RenderPass/UpscalerReactiveMaskPass.h"
#include "RenderGraph/RenderPass/XeGTAOPass.h"
#include "System/CommandLine.h"
#include "System/Log.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace
{
    using DebugTargets::Mode;

    // Where a registry entry's texels come from (Renderer::DebugTargetSlot::source).
    enum class Source : std::uint8_t { Graph, DeviceHdr, XeGTAO, RTAO, RTShadow, Csm, RRGuide, RRColor, SpotShadow,
                                       Feature,         // a render feature's own row (slice = its index)
                                       UpscalerReactive,
                                       Stencil,         // plane 1 of GBuffer_Depth
                                       ShadingModel,    // this and the rest: made by DebugTargetViewPass on request
                                       LightComplexity,
                                       Overdraw,
                                       Density,
                                       MaterialCost,
                                       ShadingCost };

    struct Curated
    {
        const char* name;
        const char* label;
        const char* group;
        const char* semantic;
        const char* meaning;
        const char* channels;
        Mode        mode;
        int         channel;
        Source      source;
        const char* graphName;   // Source::Graph
        std::uint8_t slice = 0;  // Source::Csm: the cascade (array slice); Source::RRGuide: the guide
        // The mapping a view that does not set it gets.
        float         defMin = 0.f, defMax = 1.f, defScale = 100.f;
        std::uint32_t defMask = 0xFFFFFFFFu;
    };

    // The curated registry, in the Buffer menu's order. A target a pass owns
    // outside the graph is one row here plus its pass's GetDebugOutput. The
    // GBuffer rows say what Harness/AttachmentDecode.cpp's manifests say.
    const Curated kCurated[] = {
        { "albedo", "Base Color", "GBuffer", "color",
          "baseColor, linear, no emissive (GBuffer0)",
          "r,g,b", Mode::Raw, -1, Source::Graph, "GBuffer0_Albedo" },
        { "normal", "World Normal", "GBuffer", "normal",
          "world-space normal packed to [0,1]; .a an exact 16-bit integer / 65535: bits 0..14 matIdx, bit 15 = "
          "excluded from XeGTAO (GBuffer1)",
          "x,y,z,a", Mode::Raw, -1, Source::Graph, "GBuffer1_Normal" },
        { "surface", "Surface: roughness | metalness | ao | reflectance", "GBuffer", "surface",
          "material surface terms after the material's min/max remap (GBuffer2)",
          "roughness,metalness,ao,reflectance", Mode::Channel, 0, Source::Graph, "GBuffer2_Surface" },
        { "depth", "Depth", "GBuffer", "depth",
          "reverse-Z device depth: 1 at the near plane, 0 at the far plane - exactly 0 where nothing was drawn "
          "(the sky)",
          "depth", Mode::Depth, -1, Source::Graph, "GBuffer_Depth" },
        // The same buffer's stencil byte: the view reads it through the depth buffer's stencil
        // SRV, a capture as one uint8.
        { "stencil", "Stencil: shading route | TAA exclude | outline rim", "GBuffer", "data",
          "the main depth buffer's stencil byte: bits 0..5 the shading route the GBuffer stamps (1 PBR, 2 NPR - "
          "Toon, 3 Unlit; Graphics/ShadingStencil.h), 0x40 a particle or trail kept out of TAA's history "
          "(excludeFromTAA), 0x80 the outline rim (OutlinePass, while it tags for TAA); 0 where nothing "
          "stamped it (the sky). Bits paints the route as a colour and the two flags as stripes",
          "stencil", Mode::Bits, 0, Source::Stencil, nullptr, 0, 0.f, 255.f, 100.f, 0xFFu },
        // GBuffer1.a's matIdx as a category, and its material's shading model (made on request
        // from the material table).
        { "materialId", "Material ID (matIdx)", "GBuffer", "data",
          "GBuffer1.a's matIdx - bits 0..14 of its 16-bit integer (UNORM16 * 65535; bit 15 = excluded from "
          "XeGTAO): one colour per material slot (Render.explain names an entity's)",
          "x,y,z,matIdx", Mode::Categorical, 3, Source::Graph, "GBuffer1_Normal", 0, 0.f, 32767.f, 65535.f, 0x7FFFu },
        { "shadingModel", "Shading Model", "GBuffer", "data",
          "the pixel's material's shading model (Graphics/ShadingModel.h): 0 Standard, 1 Unlit, 2 ClearCoat, "
          "3 Subsurface, 4 Anisotropic, 5 Water, 6 TwoSidedFoliage, 7 Toon; 255 where nothing was drawn (black), "
          "254 a matIdx past the table - made from GBuffer1.a and the material table on the frames it is shown "
          "or captured",
          "model", Mode::Categorical, 0, Source::ShadingModel, nullptr, 0, 0.f, 7.f, 1.f, 0xFFu },
        { "velocity", "Velocity", "GBuffer", "velocity",
          "NDC motion vector of the texel (GBuffer3)",
          "x,y", Mode::Velocity, -1, Source::Graph, "GBuffer3_Velocity" },
        { "extra", "Shading-Model Scratch", "GBuffer", "data",
          "shading-model scratch - SSS thickness, NPR terms, per model (GBuffer4; the graph calls it "
          "GBuffer4_Emissive, emissive itself seeds the HDR scene color)",
          "r,g,b,a", Mode::Raw, -1, Source::Graph, "GBuffer4_Emissive" },
        { "hdr", "HDR Scene Color", "Scene", "radiance",
          "linear scene radiance: lit scene + sky + transparent + in-place effects, before AA / upscale, exposure "
          "and tonemap (the device's HDR scene target)",
          "r,g,b,a", Mode::Hdr, -1, Source::DeviceHdr, nullptr },
        // The fragments the material pixel shaders ran for, counted by their DEBUG_SHADE
        // permutation on the frames it is shown or captured.
        { "overdraw", "Overdraw: fragments per pixel", "Scene", "data",
          "how many fragments the material pixel shaders ran for at the pixel this frame - the GBuffer (vertex and "
          "meshlet paths), forward translucent surfaces, particles and custom-material surfaces, each counted as "
          "its pixel shader starts (a fragment clip() or discard drops later still counts, and the debug "
          "permutation has no early depth test, so one the depth test rejects counts too); 0 where none ran. Not "
          "counted: trails, terrain, grass, decals, the depth prepass and shadow maps - made on the frames "
          "it is shown or captured, when those pixel shaders take the DEBUG_SHADE permutation",
          "fragments", Mode::Heat, 0, Source::Overdraw, nullptr, 0, 0.f, 8.f, 100.f, 0xFFFFFFFFu },
        // The front-most material fragment's base colour texel density, from the same DEBUG_SHADE
        // permutation.
        { "mipDensity", "Mip / Texel Density", "Scene", "data",
          "the base colour texture's density at the front-most material fragment: lod = log2 texels per pixel, "
          "the isotropic max(|ddx(uv * size)|, |ddy(uv * size)|) - 0 one texel per pixel, 1 two (the sampler drops "
          "a level), -1 half (magnified) - in 1/16 of a level; textured 1 where that fragment has a base colour "
          "texture (lod -1000 and textured 0 where it has none or nothing was drawn). Heat over [-4, 4]: green "
          "one texel per pixel, blue magnified, red minified. The GBuffer and forward material pixel shaders write "
          "it (not particles, trails, terrain or grass: behind one of those, the material surface it hides "
          "can win) - made on the frames it is shown or captured, with the DEBUG_SHADE permutation",
          "lod,textured", Mode::Heat, 0, Source::Density, nullptr, 0, -4.f, 4.f, 100.f, 0xFFFFFFFFu },
        // The pixel shader instructions behind the visible opaque surface: the material cost
        // table at GBuffer1.a's matIdx.
        { "materialCost", "Material Cost: pixel shader instructions", "Scene", "data",
          "the static instruction count (DXC reflection's InstructionCount: every branch of the permutation once - "
          "the shader's size, not what a pixel executes) of the pixel shader that drew the visible surface's "
          "material into the GBuffer: GBuffer.ps in that material's permutation or its custom pixel shader (water: "
          "WaterSurface.ps or its custom one); the lighting pass every such pixel also runs is not in it. 0 where "
          "nothing was drawn, and on terrain and grass (they write no material slot). Render.materialCosts() lists "
          "the table - made from GBuffer1.a and the frame's table on the frames it is shown or captured",
          "instructions", Mode::Heat, 0, Source::MaterialCost, nullptr, 0, 0.f, 1000.f, 100.f, 0xFFFFFFFFu },
        // Every fragment's pixel shader size summed per pixel, from the same DEBUG_SHADE
        // permutation and the same table.
        { "shadingCost", "Shading Cost: pixel shader instructions per pixel", "Scene", "data",
          "per pixel, the sum over every fragment the material pixel shaders ran for - what overdraw counts: the "
          "GBuffer, water and forward translucent surfaces; particles add nothing, they are not in the table - of "
          "its shader's static instruction count from the material cost table (materialCost, Render.materialCosts()): "
          "the shader work the pixel's draws are made of, as sizes, not executed instructions; the deferred lighting "
          "pass is not in it. Made on the frames it is shown or captured, with the DEBUG_SHADE permutation",
          "instructions", Mode::Heat, 0, Source::ShadingCost, nullptr, 0, 0.f, 5000.f, 100.f, 0xFFFFFFFFu },
        { "ssao", "Screen-Space AO (XeGTAO)", "Lighting", "mask",
          "screen-space ambient occlusion, 1 = unoccluded: the AO Lighting reads while ray-traced AO is off "
          "(XeGTAOPass)",
          "ao", Mode::Channel, 0, Source::XeGTAO, nullptr },
        { "rtao", "Ray-Traced AO", "Lighting", "mask",
          "ray-traced ambient occlusion, NRD-denoised, 1 = unoccluded: the AO Lighting reads while it is on "
          "(RTAOPass)",
          "ao", Mode::Channel, 0, Source::RTAO, nullptr },
        { "rtshadow", "Ray-Traced Sun Shadow", "Lighting", "mask",
          "ray-traced sun shadow mask, NRD-denoised, 1 = lit: rgb the colour of the sun's light (Transmissive "
          "glass tints it), lit its largest channel, the one min()'d into the cascaded shadow (RTShadowPass)",
          "r,g,b,lit", Mode::Channel, 3, Source::RTShadow, nullptr },
        // The pixel's cluster's lists, made on request from the main depth and ClusterPass /
        // DecalPass's grids.
        { "lightComplexity", "Light Complexity: lights | decals | probes", "Lighting", "data",
          "the lists of the pixel's cluster (48x27x24, cluster_common.hlsli) as Lighting and DecalApply pick it: r the "
          "local lights (the light grid's count; every light where the list overflowed), g the decals (16 at most: "
          "the cull drops the rest), b the reflection probes (8 at most), a 1 where the light list overflowed; 0 "
          "where nothing was drawn and in a channel whose grid the frame did not build (no local light, decal or "
          "probe) - made from the main depth and the grids on the frames it is shown or captured",
          "lights,decals,probes,overflow", Mode::Heat, 0, Source::LightComplexity, nullptr, 0, 0.f, 32.f, 100.f,
          0xFFFFFFFFu },
        // The sun's cascades (the VS and meshlet paths must write the same shadow map). Slices of
        // ShadowPass's cascade array - capture-only: a texture array cannot be painted.
        { "csm0", "Cascaded Shadow Map 0", "Shadows", "data",
          "the sun's cascade 0: light-space orthographic depth, reverse-Z (1 = nearest the light, 0 = cleared) - "
          "slice 0 of ShadowPass's cascade array",
          "depth", Mode::Depth, -1, Source::Csm, nullptr, 0 },
        { "csm1", "Cascaded Shadow Map 1", "Shadows", "data",
          "the sun's cascade 1: light-space orthographic depth, reverse-Z (1 = nearest the light, 0 = cleared) - "
          "slice 1 of ShadowPass's cascade array",
          "depth", Mode::Depth, -1, Source::Csm, nullptr, 1 },
        { "csm2", "Cascaded Shadow Map 2", "Shadows", "data",
          "the sun's cascade 2: light-space orthographic depth, reverse-Z (1 = nearest the light, 0 = cleared) - "
          "slice 2 of ShadowPass's cascade array",
          "depth", Mode::Depth, -1, Source::Csm, nullptr, 2 },
        // One spot light's shadow map: "spotShadow[N]" is logical slice N - the light's
        // shadowSliceIdx, assigned in light order - in whichever tier atlas (512 / 1024 / 2048)
        // holds it. Capture-only.
        { "spotShadow", "Spot Shadow Map", "Shadows", "data",
          "one spot light's shadow map: perspective depth, reverse-Z with the far plane at the light's radius "
          "(1 = nearest the light, 0 = cleared or beyond the radius) - spotShadow[N] is logical slice N",
          "depth", Mode::Depth, -1, Source::SpotShadow, nullptr, 0 },
        // The DLSS-RR guides the path tracer writes while RR runs (either ReSTIR backend).
        // PathTracePass owns them.
        { "rrAlbedo", "RR Guide: Diffuse Albedo", "Path Tracing", "color",
          "DLSS-RR's diffuse albedo guide: baseColor x (1 - metalness), linear; 0 on the sky and unlit pixels "
          "(PathTracePass, while ray reconstruction runs)",
          "r,g,b,a", Mode::Raw, -1, Source::RRGuide, nullptr, 0 },
        { "rrSpecular", "RR Guide: Specular Albedo", "Path Tracing", "color",
          "DLSS-RR's specular albedo guide: the split-sum F0 x A + B (EnvBRDFApprox), F0 = lerp(0.16 "
          "reflectance^2, baseColor, metalness); 0 under -pt_diffuse",
          "r,g,b,a", Mode::Raw, -1, Source::RRGuide, nullptr, 1 },
        { "rrNormal", "RR Guide: Normal + Roughness", "Path Tracing", "data",
          "DLSS-RR's normal guide: the signed world shading normal in xyz, perceptual roughness (>= 0.045) in a; "
          "(0, 0, 1, 1) on the sky and unlit pixels",
          "x,y,z,roughness", Mode::Signed, -1, Source::RRGuide, nullptr, 2 },
        { "rrHitDistance", "RR Guide: Specular Hit Distance", "Path Tracing", "data",
          "DLSS-RR's specular hit distance in world units, energy-weighted over the primary's specular light "
          "(a miss or the sun counts 10000)",
          "distance", Mode::Channel, 0, Source::RRGuide, nullptr, 3 },
        // The same four guides written from the GBuffer for a raster frame (rrRaster*) are the RR
        // guide render feature's rows, registered by it and listed right here
        // (Renderer/Features/RRGuideFeature.cpp).
        // DLSS-RR's colour before transparency: the HDR scene before any transparency, in RR's
        // input space (RRColorSnapshot, while RR runs).
        { "rrColorBeforeTransparency", "RR Colour Before Transparency", "Ray Reconstruction", "radiance",
          "the HDR scene before any transparency (after the fog and the face overlay), NPRStabilize's correction "
          "added - DLSS-RR's ColorBeforeTransparency (RRColorSnapshot)",
          "r,g,b,a", Mode::Hdr, -1, Source::RRColor, nullptr, 0 },
        // The reactive mask FSR 2 / DLSS SR read (UpscalerReactiveMaskPass, while one of them
        // runs).
        { "upscalerReactive", "Upscaler Reactive Mask", "Upscaler", "mask",
          "the reactive mask FSR 2 (reactive) and DLSS SR (BiasCurrentColor) read: TAA's own history floors on "
          "the stencil bits it weakens - particles that opted out of TAA (0x40) at particleVfxMinAlpha (0.92), "
          "the outline rim (0x80) at outlineMinAlpha (0 by default) - and 0 elsewhere; 1 = current frame only "
          "(UpscalerReactiveMaskPass)",
          "reactive", Mode::Channel, 0, Source::UpscalerReactive, nullptr, 0 },
    };

    using RHI::FormatName;   // Graphics/FormatNames.h

    bool SameName(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    // The producer a source is made by, or -1.
    int ProducerOf(Source s)
    {
        switch (s)
        {
        case Source::ShadingModel:    return static_cast<int>(DebugTargetViewPass::Produced::ShadingModel);
        case Source::LightComplexity: return static_cast<int>(DebugTargetViewPass::Produced::LightComplexity);
        case Source::Overdraw:        return static_cast<int>(DebugTargetViewPass::Produced::Overdraw);
        case Source::Density:         return static_cast<int>(DebugTargetViewPass::Produced::Density);
        case Source::MaterialCost:    return static_cast<int>(DebugTargetViewPass::Produced::MaterialCost);
        case Source::ShadingCost:     return static_cast<int>(DebugTargetViewPass::Produced::ShadingCost);
        default:                      return -1;
        }
    }

    // Can a Texture2D<float4> view read it? Float, unorm, snorm and depth:
    // yes. An integer format reads garbage through a float view; block-
    // compressed and video formats are never render targets.
    bool FloatReadable(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::UNKNOWN: case F::NV12:
        case F::R32G32B32A32_UINT: case F::R32G32B32A32_SINT: case F::R32G32B32_UINT: case F::R32G32B32_SINT:
        case F::R16G16B16A16_UINT: case F::R16G16B16A16_SINT: case F::R32G32_UINT: case F::R32G32_SINT:
        case F::R10G10B10A2_UINT: case F::R8G8B8A8_UINT: case F::R8G8B8A8_SINT:
        case F::R16G16_UINT: case F::R16G16_SINT: case F::R32_UINT: case F::R32_SINT:
        case F::R8G8_UINT: case F::R8G8_SINT: case F::R16_UINT: case F::R16_SINT: case F::R8_UINT: case F::R8_SINT:
            return false;
        default:
            return !(f >= F::BC1_UNORM && f <= F::BC7_UNORM_SRGB);
        }
    }

    // Can the view read it as integers? One or two channels of UINT: a Texture2D<uint2>
    // through a Tex2DUint handle. Wider ones are capture-only.
    bool UintReadable(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::R8_UINT: case F::R16_UINT: case F::R32_UINT:
        case F::R8G8_UINT: case F::R16G16_UINT: case F::R32G32_UINT:
            return true;
        default:
            return false;
        }
    }

    int ChannelCount(RHI::Format f)
    {
        using F = RHI::Format;
        switch (f)
        {
        case F::R32G32B32A32_FLOAT: case F::R32G32B32A32_UINT: case F::R32G32B32A32_SINT:
        case F::R16G16B16A16_FLOAT: case F::R16G16B16A16_UNORM: case F::R16G16B16A16_UINT:
        case F::R16G16B16A16_SNORM: case F::R16G16B16A16_SINT:
        case F::R10G10B10A2_UNORM: case F::R10G10B10A2_UINT:
        case F::R8G8B8A8_UNORM: case F::R8G8B8A8_UNORM_SRGB: case F::R8G8B8A8_UINT:
        case F::R8G8B8A8_SNORM: case F::R8G8B8A8_SINT:
        case F::B8G8R8A8_UNORM: case F::B8G8R8A8_UNORM_SRGB:
            return 4;
        case F::R32G32B32_FLOAT: case F::R32G32B32_UINT: case F::R32G32B32_SINT:
        case F::R11G11B10_FLOAT: case F::R9G9B9E5_SHAREDEXP:
            return 3;
        case F::R32G32_FLOAT: case F::R32G32_UINT: case F::R32G32_SINT:
        case F::R16G16_FLOAT: case F::R16G16_UNORM: case F::R16G16_UINT: case F::R16G16_SNORM: case F::R16G16_SINT:
        case F::R8G8_UNORM: case F::R8G8_UINT: case F::R8G8_SNORM: case F::R8G8_SINT:
            return 2;
        default:
            return 1;
        }
    }

    int CountChannels(const std::string& list)
    {
        return list.empty() ? 0 : 1 + static_cast<int>(std::count(list.begin(), list.end(), ','));
    }

    // A graph texture the table does not curate: what it is, by its name and
    // format - the capture's decode notes and the view's default follow.
    std::string GraphSemantic(const std::string& name, RHI::Format f, bool isDepth)
    {
        if (isDepth) return "depth";
        if (name.find("Velocity") != std::string::npos) return "velocity";
        if (name.find("Normal") != std::string::npos)   return "normal";
        if (name.find("Surface") != std::string::npos)  return "surface";
        if (f == RHI::Format::R16G16B16A16_FLOAT || f == RHI::Format::R32G32B32A32_FLOAT ||
            f == RHI::Format::R11G11B10_FLOAT)
            return "radiance";
        return "data";
    }

    Mode DefaultMode(const std::string& semantic)
    {
        if (semantic == "depth")    return Mode::Depth;
        if (semantic == "velocity") return Mode::Velocity;
        if (semantic == "radiance") return Mode::Hdr;
        return Mode::Raw;
    }

    std::string ChannelNames(RHI::Format f, const std::string& semantic)
    {
        if (semantic == "depth") return "depth";
        switch (ChannelCount(f))
        {
        case 4:  return semantic == "normal" ? "x,y,z,a" : "r,g,b,a";
        case 3:  return "r,g,b";
        case 2:  return semantic == "velocity" ? "x,y" : "r,g";
        default: return "r";
        }
    }

    // Why a backed texture cannot be painted (empty = it can).
    std::string NotViewable(const RHI::TextureDesc& d, std::uint64_t srv)
    {
        if (d.type != RHI::TextureDesc::Type::TEXTURE_2D) return "not a 2D texture";
        if (d.arraySize > 1)
            return "a texture array (" + std::to_string(d.arraySize) + " slices) - capture it as data: name[N] picks slice N (default 0)";
        if (d.sampleCount > 1) return "multisampled";
        if (!FloatReadable(d.format) && !UintReadable(d.format))
            return std::string("an integer / compressed format (") + FormatName(d.format) +
                   ") - capture it as data";
        if (srv == 0) return "it has no shader-resource view";
        return {};
    }

    // What the paint reads @p tex through, in @p state: its float view, or - a uint format, or
    // @p plane 1 of a depth buffer - a Tex2DUint handle (the stencil view's byte is its .y).
    // False with @p why, and @p s empty, when the view cannot read it. The main view's paint
    // and the pass capture's picture both make theirs here.
    bool ViewSource(IGraphicsDevice& gfx, const RHI::Texture& tex, RHI::ResourceState state, std::uint32_t plane,
                    DebugTargetViewPass::Source& s, std::string& why)
    {
        s         = DebugTargetViewPass::Source{};
        s.texture = &tex;
        s.state   = state;
        s.srv     = gfx.GetTextureSRVGpuHandle(tex);
        s.width   = tex.desc.width;
        s.height  = tex.desc.height;
        const std::string nv = NotViewable(tex.desc, s.srv);
        if (!nv.empty())
        {
            why = nv;
            s   = DebugTargetViewPass::Source{};
            return false;
        }
        if (plane == 1 || UintReadable(tex.desc.format))
        {
            const std::uint64_t uintSrv = plane == 1 ? gfx.GetTextureStencilSRVGpuHandle(tex) : s.srv;
            if (uintSrv == 0)
            {
                why = plane == 1 ? "it has no stencil view" : "it has no shader-resource view";
                s   = DebugTargetViewPass::Source{};
                return false;
            }
            s.uintMode   = plane == 1 ? 2u : 1u;
            s.uintHandle = gfx.BindlessOf<RHI::Bindless::Kind::Tex2DUint>(uintSrv, "DebugTargetView.uintSource").Bits();
        }
        return true;
    }
}

// ==== DebugTargets:: free functions ====
namespace DebugTargets
{
namespace
{
    std::mutex               g_namesMutex;
    std::vector<std::string> g_names, g_aliases;
}

const char* ModeName(Mode m)
{
    switch (m)
    {
    case Mode::Auto:     return "auto";
    case Mode::Raw:      return "raw";
    case Mode::Channel:  return "channel";
    case Mode::Hdr:      return "hdr";
    case Mode::Depth:    return "depth";
    case Mode::Velocity: return "velocity";
    case Mode::Signed:   return "signed";
    case Mode::Heat:        return "heat";
    case Mode::Categorical: return "categorical";
    case Mode::Bits:        return "bits";
    case Mode::Ev:          return "ev";
    default:             return "?";
    }
}

bool ParseMode(const std::string& s, Mode& out)
{
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(Mode::Count); ++i)
        if (SameName(s, ModeName(static_cast<Mode>(i))))
        {
            out = static_cast<Mode>(i);
            return true;
        }
    return false;
}

void PublishNames(std::vector<std::string> names, std::vector<std::string> aliases)
{
    std::lock_guard<std::mutex> lk(g_namesMutex);
    g_names   = std::move(names);
    g_aliases = std::move(aliases);
}

std::vector<std::string> PublishedNames()
{
    std::lock_guard<std::mutex> lk(g_namesMutex);
    return g_names;
}

bool IsPublishedName(const std::string& name)
{
    std::lock_guard<std::mutex> lk(g_namesMutex);
    for (const std::vector<std::string>* list : { &g_names, &g_aliases })
        for (const std::string& n : *list)
            if (SameName(n, name)) return true;
    return false;
}
}

// ==== The registry ====
void Renderer::BuildDebugTargetRegistry()
{
    std::vector<DebugTargets::Info> infos;
    std::vector<DebugTargetSlot>    slots;
    auto known = [&infos](const std::string& name)
    {
        for (const DebugTargets::Info& i : infos)
            if (SameName(i.name, name) || SameName(i.graphName, name)) return true;
        return false;
    };

    // A render feature's own rows (RenderFeature.h, RegisterDebugTargets), each
    // right after the curated row it names - or after the curated rows when it
    // names none this registry has.
    const std::vector<FeatureDebugTarget>& featureRows = m_featureDebugTargets.Targets();
    std::vector<std::uint8_t> placed(featureRows.size(), 0);
    auto addFeatureRows = [&](const char* after)
    {
        for (size_t k = 0; k < featureRows.size(); ++k)
        {
            const FeatureDebugTarget& t = featureRows[k];
            if (placed[k] || (after && (!t.after || !SameName(t.after, after)))) continue;
            placed[k] = 1;
            DebugTargetSlot s;
            s.source = static_cast<std::uint8_t>(Source::Feature);
            s.slice  = static_cast<std::uint8_t>(k);
            infos.push_back(t.info);
            slots.push_back(s);
        }
    };
    const char* prevCurated = nullptr;
    for (const Curated& c : kCurated)
    {
        if (prevCurated) addFeatureRows(prevCurated);
        prevCurated = c.name;
        DebugTargetSlot s;
        s.source = static_cast<std::uint8_t>(c.source);
        s.slice  = c.slice;
        if (c.source == Source::Graph || c.source == Source::Stencil)
        {
            s.handle = m_graph.FindTexture(c.source == Source::Stencil ? "GBuffer_Depth" : c.graphName);
            if (!s.handle.IsValid()) continue;   // this graph does not declare it
        }
        DebugTargets::Info i;
        i.name           = c.name;
        i.label          = c.label;
        i.group          = c.group;
        i.semantic       = c.semantic;
        i.meaning        = c.meaning;
        i.channels       = c.channels;
        i.graphName      = c.graphName ? c.graphName : "";
        i.defaultMode    = c.mode;
        i.defaultChannel = c.channel;
        i.defaultMin     = c.defMin;
        i.defaultMax     = c.defMax;
        i.defaultScale   = c.defScale;
        i.defaultMask    = c.defMask;
        infos.push_back(std::move(i));
        slots.push_back(s);
    }
    if (prevCurated) addFeatureRows(prevCurated);
    addFeatureRows(nullptr);   // the rest

    // Every other graph texture, by its graph name. Its meaning names the
    // passes that declare a write to it (declared, not whether they run).
    const std::vector<RG::RenderGraph::PassInfo> passes = m_graph.DescribePasses();
    for (const RG::RenderGraph::TextureInfo& t : m_graph.DescribeTextures())
    {
        // An import is the device's texture, already a curated target (the HDR scene colour) -
        // not another graph row.
        if (t.name.empty() || t.imported || known(t.name)) continue;
        std::string writers;
        for (const RG::RenderGraph::PassInfo& p : passes)
        {
            auto in = [&t](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), t.name) != v.end(); };
            if (in(p.writes) || in(p.depth) || in(p.readWrite) || in(p.manual))
                writers += (writers.empty() ? "" : ", ") + p.name;
        }
        const RHI::Format f = static_cast<RHI::Format>(t.format);
        DebugTargets::Info i;
        i.name        = t.name;
        i.label       = t.name;
        i.group       = "Graph";
        i.graphName   = t.name;
        i.semantic    = GraphSemantic(t.name, f, t.isDepth);
        i.defaultMode = DefaultMode(i.semantic);
        i.channels    = ChannelNames(f, i.semantic);
        i.format      = FormatName(f);
        i.meaning     = "render-graph texture " + t.name +
                        (writers.empty() ? std::string(" (no pass declares a write to it)") : " - written by " + writers);
        if (t.arraySize > 1)
        {
            i.viewable = false;
            i.viewWhy  = "a texture array (" + std::to_string(t.arraySize) + " slices) - capture it as data: name[N] picks slice N (default 0)";
        }
        else if (!FloatReadable(f) && !UintReadable(f))
        {
            i.viewable = false;
            i.viewWhy  = std::string("an integer / compressed format (") + FormatName(f) + ") - capture it as data";
        }
        DebugTargetSlot s;
        s.source = static_cast<std::uint8_t>(Source::Graph);
        s.handle = m_graph.FindTexture(t.name);
        infos.push_back(std::move(i));
        slots.push_back(s);
    }

    std::vector<std::string> names, aliases;
    names.reserve(infos.size());
    for (const DebugTargets::Info& i : infos)
    {
        names.push_back(i.name);
        if (!i.graphName.empty() && i.graphName != i.name) aliases.push_back(i.graphName);
    }
    const size_t count = infos.size();
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        m_debugTargetInfos = std::move(infos);
        m_debugTargetSlots = std::move(slots);
    }
    DebugTargets::PublishNames(std::move(names), std::move(aliases));
    LOG_INFO("Renderer: %zu debug targets (Render.targets(), the viewport's Buffer menu, -debugtarget)", count);

    // -debugtarget=<name>: start with one. Read here, once the registry that
    // validates it exists; an unknown name is an error the run log names.
    const std::string asked = Sys::CommandLine::Get().Get("-debugtarget");
    if (!asked.empty())
    {
        DebugTargets::View v;
        v.name = asked;
        std::string error;
        if (SetDebugTargetView(v, error))
            LOG_INFO("Renderer: -debugtarget: the main view shows '%s'", GetDebugTargetView().name.c_str());
        else
            LOG_ERROR("Renderer: -debugtarget=%s refused: %s", asked.c_str(), error.c_str());
    }
}

size_t Renderer::FindDebugTargetLocked(const std::string& name) const
{
    for (size_t i = 0; i < m_debugTargetInfos.size(); ++i)
        if (SameName(m_debugTargetInfos[i].name, name)) return i;
    // A graph name a curated entry covers names that entry ("GBuffer1_Normal").
    for (size_t i = 0; i < m_debugTargetInfos.size(); ++i)
        if (!m_debugTargetInfos[i].graphName.empty() && SameName(m_debugTargetInfos[i].graphName, name)) return i;
    return SIZE_MAX;
}

// @p slot's texels as the render lane left them. Fills texture / state /
// plane / deviceHdr only. `texture` is set for a backed graph texture even
// when the last frame did not write it (so its size and format are known);
// the return value is whether the data is THIS frame's. Render lane parked,
// or the render half itself: the graph's tracker and the passes' states are
// the render lane's.
bool Renderer::ResolveDebugTargetSlot(const DebugTargetSlot& slot, AttachmentSource& out, std::string& why) const
{
    out.texture   = nullptr;
    out.state     = RHI::ResourceState::UNDEFINED;
    out.plane     = 0;   // a depth-stencil's plane 0 is its depth
    out.arraySlice = slot.slice;
    out.deviceHdr = false;
    switch (static_cast<Source>(slot.source))
    {
    case Source::Graph:
    case Source::Stencil:   // the depth buffer's plane 1
    {
        if (static_cast<Source>(slot.source) == Source::Stencil) out.plane = 1;
        const RHI::Texture* tex = m_graph.InspectPhysicalTexture(slot.handle);
        if (!tex || !tex->IsValid())
        {
            why = "not backed in this run (an on-demand target its producer has not asked for, or a creation "
                  "that failed)";
            return false;
        }
        out.texture = tex;
        out.state   = m_graph.GetTextureState(slot.handle);
        if (!m_graph.WrittenByLastExecute(slot.handle))
        {
            why = "no pass wrote it in the last frame (the passes that write it did not run)";
            return false;
        }
        return true;
    }
    case Source::DeviceHdr:
        if (m_gfx.GetHdrSceneSrvGpuHandle() == 0)
        {
            why = "the main view has no HDR scene target";
            return false;
        }
        out.deviceHdr = true;
        return true;
    case Source::XeGTAO:
        out.texture = m_xegtaoPass ? m_xegtaoPass->GetDebugOutput(out.state) : nullptr;
        if (!out.texture)
        {
            why = "XeGTAO did not run in the last frame (SSAO off, ray-traced AO in its place, an orthographic "
                  "camera, or the path tracer)";
            return false;
        }
        return true;
    case Source::RTAO:
        out.texture = m_rtaoPass ? m_rtaoPass->GetDebugOutput(out.state) : nullptr;
        if (!out.texture)
        {
            why = m_rtaoPass ? "ray-traced AO did not run in the last frame (it needs SSAO and RTAO on, a scene "
                               "TLAS and NRD)"
                             : "no ray-traced AO pass in this run (no inline ray tracing)";
            return false;
        }
        return true;
    case Source::RTShadow:
        out.texture = m_rtShadowPass ? m_rtShadowPass->GetDebugOutput(out.state) : nullptr;
        if (!out.texture)
        {
            why = m_rtShadowPass ? "ray-traced shadows did not run in the last frame (they need RT shadows on, a "
                                   "sun, a scene TLAS and NRD)"
                                 : "no ray-traced shadow pass in this run (no inline ray tracing)";
            return false;
        }
        return true;
    case Source::RRGuide:
        out.arraySlice = 0;   // slice names the guide
        out.texture = m_pathTracePass && m_pathTracePass->IsActive()
                    ? m_pathTracePass->GetRRGuide(slot.slice, out.state) : nullptr;
        if (!out.texture)
        {
            why = "ray reconstruction did not run on the path tracer in the last frame (AA mode DLSS-RR, the "
                  "PathTrace render path, a GBuffer primary)";
            return false;
        }
        return true;
    case Source::RRColor:
        out.arraySlice = 0;
        out.texture = m_rrColorSnapshot ? m_rrColorSnapshot->GetTexture(out.state) : nullptr;
        if (!out.texture)
        {
            why = "the RR colour before transparency was not written in the last frame (ray reconstruction not "
                  "running)";
            return false;
        }
        return true;
    case Source::UpscalerReactive:
        out.arraySlice = 0;
        out.texture = m_reactiveMaskPass ? m_reactiveMaskPass->GetDebugOutput(out.state) : nullptr;
        if (!out.texture)
        {
            why = "the upscaler reactive mask was not written in the last frame (it exists while FSR 2 or DLSS "
                  "Super Resolution runs; never for DLSS-RR, TAA or native)";
            return false;
        }
        return true;
    case Source::Feature:
    {
        out.arraySlice = 0;   // slice names the feature's row
        const std::vector<FeatureDebugTarget>& rows = m_featureDebugTargets.Targets();
        const FeatureDebugTarget* t = slot.slice < rows.size() ? &rows[slot.slice] : nullptr;
        out.texture = t && t->texture ? t->texture(out.state) : nullptr;
        if (!out.texture)
        {
            why = t ? t->missing : "no such render feature row";
            return false;
        }
        return true;
    }
    case Source::ShadingModel:
    case Source::LightComplexity:
    case Source::Overdraw:
    case Source::Density:
    case Source::MaterialCost:
    case Source::ShadingCost:
    {
        // Made on request: there only when the frame just run made it (~0 = made by no frame
        // yet). The texture is reported either way, for its size and format.
        const auto kind = static_cast<DebugTargetViewPass::Produced>(ProducerOf(static_cast<Source>(slot.source)));
        std::uint64_t made = ~0ull;
        out.arraySlice = 0;
        out.texture = m_debugTargetPass ? m_debugTargetPass->GetProduced(kind, out.state, made) : nullptr;
        if (!out.texture || made == ~0ull || made != m_debugTargetFrame.load(std::memory_order_acquire))
        {
            why = (m_debugTargetPass && m_debugTargetPass->IsReady() && !m_debugTargetPass->ProducerReady(kind))
                      ? "its producer (DebugTargetProduce.cs.hlsl) did not build - the log says why; a shader "
                        "reload retries"
                      : "made on request - on the frames a view shows it (Render.target) or a capture names it";
            return false;
        }
        return true;
    }
    case Source::Csm:
        out.texture = m_shadowPass ? m_shadowPass->GetDebugOutput(out.state) : nullptr;
        if (!out.texture)
        {
            why = "the cascaded shadow map has not been drawn yet";
            return false;
        }
        return true;
    case Source::SpotShadow:
    {
        std::uint32_t physical = 0;
        out.texture = m_spotShadowPass ? m_spotShadowPass->GetDebugSlice(slot.slice, physical, out.state) : nullptr;
        if (!out.texture)
        {
            why = "no spot shadow map in logical slice " + std::to_string(slot.slice) +
                  " (no shadow-casting spot light there, or not drawn yet)";
            return false;
        }
        out.arraySlice = physical;
        return true;
    }
    }
    why = "unknown source";
    return false;
}

void Renderer::RefreshDebugTargetAvailability() const
{
    std::vector<DebugTargetSlot> slots;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        slots = m_debugTargetSlots;
    }
    struct Live
    {
        bool          available = false, known = false;   // known: backed, so size / format / viewability are known
        std::string   why, viewWhy, format;
        std::uint32_t width = 0, height = 0;
    };
    std::vector<Live> live(slots.size());
    for (size_t k = 0; k < slots.size(); ++k)
    {
        Live& l = live[k];
        AttachmentSource src;
        l.available = ResolveDebugTargetSlot(slots[k], src, l.why);
        if (l.available) l.why.clear();
        if (src.deviceHdr)
        {
            l.known  = true;
            l.width  = m_gfx.GetRenderWidth();
            l.height = m_gfx.GetRenderHeight();
            l.format = "R16G16B16A16_FLOAT";
        }
        else if (src.texture)
        {
            const RHI::TextureDesc& d = src.texture->desc;
            l.known   = true;
            l.width   = d.width;
            l.height  = d.height;
            l.format  = FormatName(d.format);
            l.viewWhy = NotViewable(d, m_gfx.GetTextureSRVGpuHandle(*src.texture));
        }
    }
    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    if (m_debugTargetInfos.size() != live.size()) return;   // rebuilt meanwhile
    for (size_t k = 0; k < live.size(); ++k)
    {
        DebugTargets::Info& i = m_debugTargetInfos[k];
        Live&               l = live[k];
        i.available = l.available;
        i.why       = std::move(l.why);
        if (l.known)   // otherwise keep what was last known (or Build's reasons)
        {
            i.width    = l.width;
            i.height   = l.height;
            i.format   = std::move(l.format);
            i.viewable = l.viewWhy.empty();
            i.viewWhy  = std::move(l.viewWhy);
        }
    }
}

std::vector<DebugTargets::Info> Renderer::ListDebugTargets(bool refresh) const
{
    if (refresh)
        RefreshDebugTargetAvailability();
    else
        m_debugTargetRefreshWanted.store(true, std::memory_order_release);   // the render half's next frame
    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    return m_debugTargetInfos;
}

bool Renderer::SetDebugTargetView(const DebugTargets::View& v, std::string& error)
{
    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    if (v.name.empty())
    {
        m_debugTargetView = DebugTargets::View{};
        m_debugTargetWanted.store(false, std::memory_order_release);
        return true;
    }
    const size_t idx = FindDebugTargetLocked(v.name);
    if (idx == SIZE_MAX)
    {
        error = "no debug target '" + v.name + "' - Render.targets() lists them";
        return false;
    }
    const DebugTargets::Info& info = m_debugTargetInfos[idx];
    if (!info.viewable)
    {
        error = "'" + info.name + "' cannot be painted: " + info.viewWhy;
        return false;
    }
    if (v.mode == DebugTargets::Mode::Count)
    {
        error = "no such mode";
        return false;
    }
    const int channels = CountChannels(info.channels);
    if (v.channel < -1 || v.channel > 3 || v.channel >= channels)
    {
        error = "'" + info.name + "' has " + std::to_string(channels) + " channel(s) (" + info.channels +
                "): channel " + std::to_string(v.channel) + " is not one";
        return false;
    }
    if (!std::isfinite(v.rangeMin) || !std::isfinite(v.rangeMax) || v.rangeMin == v.rangeMax)
    {
        error = "min and max must be finite and different";
        return false;
    }
    if (!std::isfinite(v.exposure) || v.exposure <= 0.f || !std::isfinite(v.scale) || v.scale <= 0.f)
    {
        error = "exposure and scale must be positive";
        return false;
    }
    m_debugTargetView      = v;
    m_debugTargetView.name = info.name;   // the registry's spelling
    m_debugTargetWanted.store(true, std::memory_order_release);
    return true;
}

DebugTargets::View Renderer::GetDebugTargetView() const
{
    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    return m_debugTargetView;
}

DebugTargets::Shown Renderer::GetDebugTargetShown() const
{
    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    return m_debugTargetShown;
}

void Renderer::EnsureDebugTargetPipeline()
{
    if (m_debugTargetPass) m_debugTargetPass->EnsurePipeline();
}

bool Renderer::GetAttachmentSource(const std::string& name, AttachmentSource& out, std::string& why) const
{
    out = AttachmentSource{};
    // "Name[N]" captures array slice N of a texture array (RestirDIResult[1]
    // is ReSTIR DI's specular). The capture refuses a slice past the end.
    std::string base = name;
    int slice = -1;
    if (const size_t open = name.rfind('['); open != std::string::npos && !name.empty() && name.back() == ']')
    {
        const std::string digits = name.substr(open + 1, name.size() - open - 2);
        if (!digits.empty() && digits.size() <= 3 &&
            digits.find_first_not_of("0123456789") == std::string::npos)
        {
            slice = std::stoi(digits);
            base = name.substr(0, open);
        }
    }
    DebugTargetSlot slot;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        const size_t idx = FindDebugTargetLocked(base);
        if (idx == SIZE_MAX)
        {
            why = "no render target '" + name + "' - Render.targets() lists them";
            return false;
        }
        slot     = m_debugTargetSlots[idx];
        out.info = m_debugTargetInfos[idx];
    }
    if (slice > 255)
    {
        why = "array slice " + std::to_string(slice) + " is out of range (0-255)";
        return false;
    }
    if (slice >= 0) slot.slice = static_cast<std::uint8_t>(slice);
    if (ResolveDebugTargetSlot(slot, out, why)) return true;
    why = "the main view's '" + out.info.name + "' is not there: " + why;
    return false;
}

// What a capture {"attachment", "afterPass"} copies. A registry name names its graph
// texture (a graph row's; the stencil row is the depth buffer's plane 1 and hdr the HDR
// import, as the registry builds them); any other name must be a main-graph texture's own
// (an import's included), its entry made from the format. Serial window (the graph's
// names).
bool Renderer::ResolvePassCaptureTarget(const std::string& name, std::string& graphName, std::uint32_t& plane,
                                        DebugTargets::Info& info, std::string& why) const
{
    graphName.clear();
    plane = 0;
    bool known = false;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        const size_t idx = FindDebugTargetLocked(name);
        if (idx != SIZE_MAX)
        {
            info  = m_debugTargetInfos[idx];
            known = true;
            switch (static_cast<Source>(m_debugTargetSlots[idx].source))
            {
            case Source::Graph:     graphName = info.graphName; break;
            case Source::Stencil:   graphName = "GBuffer_Depth"; plane = 1; break;
            case Source::DeviceHdr: graphName = RG::kHdrSceneTexture; break;
            default:                break;
            }
        }
    }
    if (graphName.empty())
    {
        if (!m_graph.FindTexture(name).IsValid())
        {
            why = known ? "'" + name + "' is not a texture of the main render graph (a pass's own: after a pass "
                                       "captures graph textures - RenderGraph panel / rg verb name them)"
                        : "no render target or graph texture '" + name + "'";
            return false;
        }
        graphName = name;
    }
    if (!known)
    {
        const RG::RGTextureHandle h = m_graph.FindTexture(graphName);
        const RHI::Texture* tex = m_graph.InspectPhysicalTexture(h);
        const RHI::Format f = tex ? tex->desc.format : RHI::Format::UNKNOWN;
        info           = DebugTargets::Info{};
        info.name      = graphName;
        info.label     = graphName;
        info.group     = "Graph";
        info.graphName = graphName;
        info.semantic  = GraphSemantic(graphName, f, tex && RHI::HasFlag(tex->desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL));
        info.defaultMode = DefaultMode(info.semantic);
        info.channels  = ChannelNames(f, info.semantic);
        info.format    = FormatName(f);
        info.meaning   = "render-graph texture " + graphName;
    }
    return true;
}

// ==== The producers ====
// The targets no pass writes, made by the view pass on the frames that show or capture
// them.
//
// The producers this frame runs - the shown target's and the ones a capture asked for.
// The service point; the render half reads the mask from the FrameView.
std::uint32_t Renderer::DebugTargetProduceMask()
{
    std::uint32_t mask = m_debugProduceRequested.exchange(0, std::memory_order_acq_rel);
    if (m_debugTargetWanted.load(std::memory_order_acquire))
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        const size_t idx = m_debugTargetView.name.empty() ? SIZE_MAX : FindDebugTargetLocked(m_debugTargetView.name);
        if (idx != SIZE_MAX)
            if (const int p = ProducerOf(static_cast<Source>(m_debugTargetSlots[idx].source)); p >= 0)
                mask |= 1u << p;
    }
    // The DEBUG_SHADE permutation writes every counter and each producer that reads one zeroes
    // it, so they run together.
    if ((mask & DebugTargetViewPass::CountedMask()) != 0) mask |= DebugTargetViewPass::CountedMask();
    return mask;
}

bool Renderer::DebugTargetNeedsProduce(const std::string& name)
{
    int p = -1;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        const size_t idx = FindDebugTargetLocked(name);
        if (idx != SIZE_MAX) p = ProducerOf(static_cast<Source>(m_debugTargetSlots[idx].source));
    }
    if (p < 0 || !m_debugTargetPass) return false;
    RHI::ResourceState state = RHI::ResourceState::UNDEFINED;
    std::uint64_t made = ~0ull;
    if (m_debugTargetPass->GetProduced(static_cast<DebugTargetViewPass::Produced>(p), state, made) &&
        made != ~0ull && made == m_debugTargetFrame.load(std::memory_order_acquire))
        return false;
    m_debugProduceRequested.fetch_or(1u << p, std::memory_order_acq_rel);
    return true;
}

void Renderer::RunDebugProducers(RHI::CommandList cl, std::uint32_t mask)
{
    if (!m_debugTargetPass) return;
    DebugTargetViewPass::ProduceInputs in;
    in.normal = m_graph.InspectPhysicalTexture(m_normalHandle);
    in.depth  = m_graph.InspectPhysicalTexture(m_depthHandle);
    if (in.normal) in.normalState = m_graph.GetTextureState(m_normalHandle);
    if (in.depth)  in.depthState  = m_graph.GetTextureState(m_depthHandle);
    if (in.depth && RG::RenderGraph::PlanesEnabled() && RHI::HasStencilPlane(in.depth->desc.format))
        in.depthPlane = 0;   // the graph tracks the planes apart: move the depth plane alone
    in.materialSrv   = m_gfx.GetBufferSRVGpuHandle(MaterialSRVBuffer(m_gfx.GetFrameIndex()));
    in.materialCount = GetMaterialSlotCapacity();
    const FrameView& fv = FrameViewRead();
    if ((mask & DebugTargetViewPass::Bit(DebugTargetViewPass::Produced::LightComplexity)) != 0)
    {
        // The grids this frame's ClusterPass / DecalPass wrote (Phase 2a.5, this render half), as
        // the bindless handles the shader reads.
        auto grid = [&](std::uint64_t srv, const char* what)
        {
            return m_gfx.BindlessOf<RHI::Bindless::Kind::StructuredBuffer>(srv, what, 8u).Bits();   // {offset, count}
        };
        in.nearZ = fv.camera.nearZ;
        in.farZ = fv.camera.farZ;
        in.orthographic = fv.camera.orthographic;
        if (m_clusterPass && m_clusterPass->LightGridWritten())
        {
            in.lightGrid = grid(m_clusterPass->GetLightGridSRVHandle(), "DebugTargetView.lightGrid");
            in.gridFlags |= DebugTargetViewPass::kGridLights;
        }
        if (m_clusterPass && m_clusterPass->ProbeGridWritten())
        {
            in.probeGrid = grid(m_clusterPass->GetProbeGridSRVHandle(), "DebugTargetView.probeGrid");
            in.gridFlags |= DebugTargetViewPass::kGridProbes;
        }
        if (const std::uint64_t srv = m_decalPass ? m_decalPass->GetDecalGridSRVHandle() : 0; srv != 0)
        {
            in.decalGrid = grid(srv, "DebugTargetView.decalGrid");
            in.gridFlags |= DebugTargetViewPass::kGridDecals;
        }
    }
    const std::uint64_t frame = static_cast<std::uint64_t>(fv.currentFrame);
    for (std::uint32_t k = 0; k < static_cast<std::uint32_t>(DebugTargetViewPass::Produced::Count); ++k)
        if ((mask & (1u << k)) != 0)
            m_debugTargetPass->Produce(cl, static_cast<DebugTargetViewPass::Produced>(k), in, frame);
}

// Per material slot, the instructions of the pixel shader its main-view packets draw with
// (the most, for a slot two shaders draw), copied to the fixed-slot table on a list of its
// own opened at the material copy - before every list that reads it. Only on the frames a
// producer that reads it runs; GBuffer1.a holds slots below the capacity, so the table
// covers them all.
void Renderer::RecordDebugCostTable(std::uint32_t frameSlot)
{
    // The coverage a materialCost frame counted, read back now that its frame
    // slot comes round again (before this frame's producers reuse it).
    if (m_debugTargetPass)
    {
        std::vector<std::uint32_t> pixels;
        std::uint64_t frame = 0;
        if (m_debugTargetPass->TakeCoverage(frameSlot, pixels, frame))
        {
            std::lock_guard<std::mutex> lk(m_debugCoverageMutex);
            m_debugCoverage.swap(pixels);
            m_debugCoverageFrame = frame;
        }
    }
    const FrameView& fv = FrameViewRead();
    if (!m_debugTargetPass || (fv.debugProduce & DebugTargetViewPass::CostTableMask()) == 0 ||
        !m_debugTargetPass->CostTableReady())
        return;
    std::vector<MaterialCostRow> rows;
    CollectMaterialCosts(rows, /*details=*/false);
    m_debugCostTable.assign(m_maxMaterials, 0u);
    for (const MaterialCostRow& row : rows)
        if (row.slot < m_debugCostTable.size())
            m_debugCostTable[row.slot] = (std::max)(m_debugCostTable[row.slot], row.instructions);
    RHI::CommandList cl = m_gfx.BeginCommandList(RHI::QueueType::GRAPHICS);
    m_gfx.SetCommandListName(cl, "DebugTargetCostTable");
    cl.gfx = &m_gfx;
    m_debugTargetPass->UploadCostTable(cl, frameSlot, m_debugCostTable.data(),
                                       static_cast<std::uint32_t>(m_debugCostTable.size()),
                                       static_cast<std::uint64_t>(fv.currentFrame));
}

// ==== The paint ====
// Phase 7, on the restore list, after the post chain and before the overlays. Nothing at
// all while no target is asked for.
void Renderer::ExecuteDebugTargetView(RHI::CommandList cl)
{
    // First the targets made on request, so this frame's paint, its availability and a
    // capture after it see them.
    {
        const FrameView& now = FrameViewRead();
        m_debugTargetFrame.store(static_cast<std::uint64_t>(now.currentFrame), std::memory_order_release);
        if (now.debugProduce != 0) RunDebugProducers(cl, now.debugProduce);
    }
    if (m_debugTargetRefreshWanted.exchange(false, std::memory_order_acq_rel))
        RefreshDebugTargetAvailability();
    if (!m_debugTargetWanted.load(std::memory_order_acquire))
    {
        if (m_debugTargetPainting)
        {
            std::lock_guard<std::mutex> lk(m_debugTargetMutex);
            m_debugTargetShown = DebugTargets::Shown{};
            m_debugTargetPainting = false;
        }
        return;
    }

    using DebugTargets::Mode;
    DebugTargets::View view;
    DebugTargetSlot    slot;
    Mode               defMode    = Mode::Raw;
    int                defChannel = -1;
    DebugTargets::Info defaults;   // the mapping a view that does not set it gets
    bool               found      = false;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        view = m_debugTargetView;
        const size_t idx = view.name.empty() ? SIZE_MAX : FindDebugTargetLocked(view.name);
        if (idx != SIZE_MAX)
        {
            slot       = m_debugTargetSlots[idx];
            defMode    = m_debugTargetInfos[idx].defaultMode;
            defChannel = m_debugTargetInfos[idx].defaultChannel;
            defaults.defaultMin   = m_debugTargetInfos[idx].defaultMin;
            defaults.defaultMax   = m_debugTargetInfos[idx].defaultMax;
            defaults.defaultScale = m_debugTargetInfos[idx].defaultScale;
            defaults.defaultMask  = m_debugTargetInfos[idx].defaultMask;
            found      = true;
        }
    }
    if (view.name.empty()) return;   // cleared since the flag was read: next frame clears Shown
    m_debugTargetPainting = true;

    // Auto takes the target's own mapping; a channel asked for with Auto
    // shows that channel alone.
    Mode mode    = view.mode;
    int  channel = view.channel;
    if (mode == Mode::Auto) mode = channel >= 0 ? Mode::Channel : defMode;
    if (channel < 0) channel = defChannel >= 0 ? defChannel : 0;
    // What the asker did not set, the target's own (for the targets without one these are
    // View's defaults).
    if (!view.minGiven)   view.rangeMin = defaults.defaultMin;
    if (!view.maxGiven)   view.rangeMax = defaults.defaultMax;
    if (!view.scaleGiven) view.scale    = defaults.defaultScale;
    if (!view.maskGiven)  view.mask     = defaults.defaultMask;

    const FrameView& fv = FrameViewRead();
    AttachmentSource src;
    std::string      why;
    bool             have = found && ResolveDebugTargetSlot(slot, src, why);
    if (!found) why = "no such target any more";
    DebugTargetViewPass::Source s;
    if (have && src.deviceHdr)
    {
        // The device tracks its HDR target's state itself: RENDERTARGET here
        // (Phase 7 put it there), back to it below.
        m_gfx.SetHdrTextureState(RHI::ResourceState::SHADER_RESOURCE, cl);
        s.srv    = m_gfx.GetHdrSceneSrvGpuHandle();
        s.width  = fv.renderWidth;
        s.height = fv.renderHeight;
    }
    // A uint target or a stencil plane reads through a Tex2DUint handle - the stencil view's
    // byte is its .y.
    else if (have && !ViewSource(m_gfx, *src.texture, src.state, src.plane, s, why))
        have = false;

    DebugTargetViewPass::Params p;
    p.mode         = mode;
    p.channel      = channel;
    p.rangeMin     = view.rangeMin;
    p.rangeMax     = view.rangeMax;
    p.exposure     = view.exposure;
    p.scale        = view.scale;
    p.mask         = view.mask;
    p.nearZ        = fv.camera.nearZ;
    p.farZ         = fv.camera.farZ;
    p.orthographic = fv.camera.orthographic;

    bool painted = false;
    if (m_debugTargetPass && m_toneMapPass)
    {
        const RHI::Texture* output = m_toneMapPass->GetFinalOutputTexture();
        if (output && output->IsValid())
        {
            const std::uint32_t r = m_gfx.BeginGPUTimestamp(cl, "DebugTargetView");
            painted = m_debugTargetPass->Execute(cl, s, *output, m_toneMapPass->GetFinalOutputState(),
                                                 fv.vpWidth, fv.vpHeight, p);
            m_gfx.EndGPUTimestamp(cl, r);
        }
    }
    if (have && src.deviceHdr)
        m_gfx.SetHdrTextureState(RHI::ResourceState::RENDERTARGET, cl);
    if (!painted)
        why = (m_debugTargetPass && m_debugTargetPass->IsReady())
                  ? "the view did not paint (no final output this frame)"
                  : "the view's pipeline is not built yet (it is at the next frame boundary)";

    std::lock_guard<std::mutex> lk(m_debugTargetMutex);
    m_debugTargetShown.view         = view;
    m_debugTargetShown.view.mode    = mode;
    m_debugTargetShown.view.channel = channel;
    m_debugTargetShown.active       = true;
    m_debugTargetShown.available    = have && painted;
    m_debugTargetShown.why          = (have && painted) ? std::string() : why;
    m_debugTargetShown.frame        = static_cast<std::uint64_t>(fv.currentFrame);
}

// ==== The pass capture's picture ====
// The copy (Renderer/PassCapture.h) painted into the picture with the same shader, through
// its own constants (Slot::Preview), so the main view's paint in the same frame keeps its
// own. The mapping: what the asker set; the rest the registry entry's (the texture's own
// row - a graph name a curated row covers names that row - or the stencil row for a depth
// copy's plane 1), else what the format says.
bool Renderer::PaintPassCapture(RHI::CommandList cl, const std::string& name, const RHI::Texture& copy,
                                RHI::ResourceState copyState, int plane, const DebugTargets::View& asked,
                                const RHI::Texture& picture, RHI::ResourceState pictureState,
                                DebugTargets::View& shown, std::string& why)
{
    if (!m_debugTargetPass || !m_debugTargetPass->IsReady())
    {
        why = "the debug target shader is not built (the next frame boundary builds it)";
        return false;
    }
    const RHI::Format   f        = copy.desc.format;
    const std::uint32_t usePlane = (plane == 1 && RHI::HasStencilPlane(f)) ? 1u : 0u;
    DebugTargets::Info defaults;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(m_debugTargetMutex);
        const size_t idx = FindDebugTargetLocked(usePlane == 1 ? std::string("stencil") : name);
        if (idx != SIZE_MAX)
        {
            defaults = m_debugTargetInfos[idx];
            found    = true;
        }
    }
    if (!found)
    {
        defaults.semantic    = GraphSemantic(name, f, RHI::HasFlag(copy.desc.bindFlags, RHI::BindFlag::DEPTH_STENCIL));
        defaults.defaultMode = DefaultMode(defaults.semantic);
    }

    DebugTargets::View view = asked;
    Mode mode    = view.mode;
    int  channel = view.channel;
    if (mode == Mode::Auto) mode = channel >= 0 ? Mode::Channel : defaults.defaultMode;
    if (channel < 0) channel = defaults.defaultChannel >= 0 ? defaults.defaultChannel : 0;
    if (!view.minGiven)   view.rangeMin = defaults.defaultMin;
    if (!view.maxGiven)   view.rangeMax = defaults.defaultMax;
    if (!view.scaleGiven) view.scale    = defaults.defaultScale;
    if (!view.maskGiven)  view.mask     = defaults.defaultMask;

    DebugTargetViewPass::Source s;
    if (!ViewSource(m_gfx, copy, copyState, usePlane, s, why)) return false;

    const FrameView& fv = FrameViewRead();
    DebugTargetViewPass::Params p;
    p.mode         = mode;
    p.channel      = channel;
    p.rangeMin     = view.rangeMin;
    p.rangeMax     = view.rangeMax;
    p.exposure     = view.exposure;
    p.scale        = view.scale;
    p.mask         = view.mask;
    p.nearZ        = fv.camera.nearZ;
    p.farZ         = fv.camera.farZ;
    p.orthographic = fv.camera.orthographic;
    if (!m_debugTargetPass->Execute(cl, s, picture, pictureState, picture.desc.width, picture.desc.height, p,
                                    DebugTargetViewPass::Slot::Preview))
    {
        why = "the paint did not run";
        return false;
    }
    shown         = view;
    shown.name    = name;
    shown.mode    = mode;
    shown.channel = channel;
    return true;
}
