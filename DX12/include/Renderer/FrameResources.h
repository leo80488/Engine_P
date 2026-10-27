#pragma once

// Renderer/FrameResources.h - what the Renderer and the render features
// exchange.
//
// FrameTextures: the main graph's textures a feature's passes may declare -
// the Renderer's graph-scope handles, set once in Renderer::Compile and stable
// from then on.
//
// FrameResources: the per-frame values the Renderer fills before a feature
// records outside the graph, and the outputs a feature leaves for the Renderer
// and the other features. Typed fields, never a lookup by name: a new kind of
// frame resource is one field here, its single source. A field joins with the
// first feature that reads or writes it.
// TODO(RenderPipeline_Architecture_Review_Plan BD4): the Frame Resource Table
// that generalises this.

#include "Graphics/GraphicsStruct.h"   // RG::RGTextureHandle

#include <DirectXMath.h>
#include <array>
#include <cstdint>

struct FrameTextures
{
    RG::RGTextureHandle depth;      // the GBuffer's depth-stencil
    RG::RGTextureHandle albedo;     // GBuffer0
    RG::RGTextureHandle normal;     // GBuffer1
    RG::RGTextureHandle surface;    // GBuffer2
    RG::RGTextureHandle velocity;   // GBuffer3: motion vectors
    RG::RGTextureHandle emissive;
    RG::RGTextureHandle diResult;   // ReSTIR DI's result (on demand)
};

struct FrameResources
{
    // The GBuffer depth's SRV, compute-readable from PreResolve on; 0 when
    // there is none (RenderAsync's frame-top capture). VelocityFill.
    uint64_t depthSrv = 0;
    // TAA's unjittered view-projection, this frame's and the previous one
    // (TAAJitterState), filled before PreResolve records - before the upscale
    // branch advances them. VelocityFill reconstructs the sky's motion from them.
    DirectX::XMFLOAT4X4 viewProjNoJitter{};
    DirectX::XMFLOAT4X4 prevViewProjNoJitter{};

    // What the RR raster guides read (RRGuide), filled before PreResolve:
    bool     pathTrace = false;            // the frame is on the path tracer's path
    uint32_t materialsIdx = 0;             // this frame's material SRV, a heap index (0 = none)
    bool     lightCB = false;              // this frame's LightCB exists (the sun fields below hold it)
    DirectX::XMFLOAT3 sunDirection{};      // LightCB's: the direction the sun's light travels
    DirectX::XMFLOAT3 sunColor{};
    uint32_t iblRadianceMips = 1;          // LightCB's radiance cube mips (>= 1) and IBL strength
    float    iblStrength = 1.0f;
    uint32_t skyRadianceIdx = 0;           // the sky radiance cube, a heap index (0 = none)
    bool     diDistance = false;           // ReSTIR DI's result carries the DI light distance (RR denoises DI)
    uint32_t rtReflectionIdx = 0;          // the unified RT reflection's output, a heap index (0 = none)
    uint32_t rtReflectionDistanceIdx = 0;  // ... and the hit distance it keeps for RR

    // What the lens flare reads (the PostChain's LensFlare):
    bool     moonActive = false;           // the time of day's moon lights the scene (SyncSkyboxIBL)
    uint64_t cloudSrv = 0;                 // the clouds' transmittance, 0 when they did not render

    // What the PostChain's Resolve stage reads, filled before PreResolve records
    // (the jitter: before the chain advances it):
    DirectX::XMFLOAT2 jitter{};            // this frame's sub-pixel jitter in pixels (TAAJitterState); TAA, FSR 2
    float    nprMaxBrightness = 0.0f;      // LightCB's scene ceiling; NPRStabilize

    // RRGuide's output: the four raster guides when it wrote them this frame -
    // DLSS-RR reads them - else null.
    bool rrRasterGuidesWritten = false;
    std::array<const RHI::Texture*, 4> rrRasterGuides{};

    // What DLSS reads (the PostChain's DLSS), filled after PreResolve recorded -
    // RRGuide's output decides the guides:
    bool rr = false;                       // ray reconstruction this frame (Renderer::IsRayReconstructionActive)
    // RR's inputs are set: RR runs and a guide source exists (the path tracer,
    // or the raster guides DLSS reads). The guides - albedo, specular, normal,
    // hit distance - are the raster ones when DLSS reads them, else the path
    // tracer's (each null when not valid).
    bool rrSetGuides = false;
    std::array<const RHI::Texture*, 4> rrGuides{};
    const RHI::Texture* rrDiffuseDistance = nullptr;          // ReSTIR GI's connection distance (raster guides only)
    const RHI::Texture* rrColorBeforeTransparency = nullptr;  // RRColorSnapshot's when written (RRColorAlign first)
    void* slFrameToken = nullptr;          // Streamline's frame token (the Renderer's, main-thread state)
};
