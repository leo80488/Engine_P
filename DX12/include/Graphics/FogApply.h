#pragma once

// FogApply - the C++ side of shaders/fog_apply.hlsli: the fog the opaque scene got
// from its fullscreen composites (HeightFogApply, VolumetricApply), for every
// surface drawn after them (forward translucency, particles, trails, tracers,
// BillboardFX, world-space video), plus the opaque depth their soft fade reads.
//
// The Renderer writes the constants once per frame (Renderer::WriteFogApplyCB, the
// RG slot FogApply); a pass whose shader includes fog_apply.hlsli binds them with
// FogApply::Bind after every SetPipelineState. The froxel volume's sampler is the
// fixed SMP_LINEAR_CLAMP heap slot, named by the shader: nothing to bind.

#include "Graphics/GraphicsStruct.h"
#include "Graphics/RootSignatureLayout.h"

#include <cstdint>

namespace FogApply
{
    inline constexpr uint32_t kFlagHeight = 1u;   // FOG_APPLY_HEIGHT: HeightFogApply ran this frame
    inline constexpr uint32_t kFlagFroxel = 2u;   // FOG_APPLY_FROXEL: VolumetricApply ran this frame

    // Mirror of cbuffer FogApplyCB (b4 space0) in fog_apply.hlsli; the layout
    // specs (Renderer/LayoutSpecs.cpp) check it against DXC's reflection.
    struct alignas(16) Constants
    {
        float    invViewProj[16];          //   0  LightCB.invViewProj (jittered, transposed)
        float    cameraPos[3];             //  64
        uint32_t flags;                    //  76  kFlag*; 0 = no fog
        float    dirToSun[3];              //  80  -LightCB.lightDir
        float    nearZ;                    //  92
        float    sunColor[3];              //  96
        float    farZ;                     // 108
        float    invRenderSize[2];         // 112
        uint32_t froxelIdx;                // 120  the scattering volume's heap index
        uint32_t opaqueDepthIdx;           // 124  SSRTraceDepth's heap index; 0 = none
        float    hfDensity;                // 128  HeightFogPass's constants, clamped as
        float    hfFalloff;                //      its Execute clamps them
        float    hfHeight;
        float    hfStart;
        float    hfColor[3];               // 144
        float    hfMaxOpacity;
        float    hfSunInscatter;           // 160
        float    hfAnisotropy;
        float    froxelNear;
        float    froxelFar;
        uint32_t froxelDepth;              // 176
        float    softFadeRange;            //      meters; 0 = no soft fade
        float    _pad[2];
    };
    static_assert(sizeof(Constants) == 192, "FogApply::Constants must match fog_apply.hlsli's FogApplyCB");

    inline constexpr auto kCBRel      = RHI::Slots::GfxRel::CB_b4;       // b4 space0

    // For a draw whose shader includes fog_apply.hlsli: this frame's constants (the RG
    // slot) at b4. SetPipelineState resets root arguments, so bind after each one.
    // The froxel sampler is SMP_LINEAR_CLAMP in fog_apply.hlsli: trilinear and clamped
    // on every axis like VolumetricFogPass's own, since a wrap would blend the far
    // slice into the near one.
    inline void Bind(RHI::CommandList cl)
    {
        cl.BindCB(kCBRel, RG::RGSlot::FogApply);
    }
}
