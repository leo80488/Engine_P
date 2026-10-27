#ifndef LIGHT_CB_HLSLI
#define LIGHT_CB_HLSLI

// light_cb.hlsli - the single source of truth for the LightCB layout.
//
// The C++ mirror is `RendererDetail::LightCB` in include/Renderer/Renderer.h (size
// static_asserted against RHI::Slots::BindlessIdx::kLightCB_Size), and
// src/Renderer/LayoutSpecs.cpp checks every member's offset against this header's
// reflection at load ([LAYOUT]). Add or reorder fields in both in lockstep: the
// C++ side mirrors the HLSL packing rules (a member fits in one 16-byte row, a
// float3 cannot straddle, array elements pad to 16) with a scalar-stride layout.
//
// Passes bind LightCB at different registers - the deferred LightingPass at b1,
// the forward TransparentPass at b2 (its VS owns b1 for PerViewCB) - so a consumer
// `#define`s LIGHT_CB_REGISTER to the slot literal before including this header:
//
//     #define LIGHT_CB_REGISTER b1
//     #include "light_cb.hlsli"
//
// Two forms, one member list (light_cb_fields.hlsli):
//   * default: `cbuffer LightCB` at LIGHT_CB_REGISTER, members at global scope
//     (bare `lightDir`, `cameraPos`, ...) - every graphics consumer;
//   * `#define LIGHT_CB_AS_STRUCT` before the include: `struct LightCB`, for a
//     shader that opens the frame's LightCB from the descriptor heap,
//
//         ConstantBuffer<LightCB> lcb = ResourceDescriptorHeap[idx];
//
//     idx being IGraphicsDevice::GetBufferCBVHeapIndex of the frame's LightCB
//     buffer (VolumetricRaymarch.cs: FroxelParams.lightCbIdx). A ConstantBuffer
//     packs its struct by the cbuffer rules, so the offsets are the same.
// Never write a partial copy of this layout for such a shader: a block opened
// from the heap is not among the reflection's bindings, so LayoutCheck never
// sees it, and a copy would drift unchecked.

#ifdef LIGHT_CB_AS_STRUCT
struct LightCB
{
#include "light_cb_fields.hlsli"
};
#else
#ifndef LIGHT_CB_REGISTER
#error "Define LIGHT_CB_REGISTER (e.g. b1) - or LIGHT_CB_AS_STRUCT - before #include \"light_cb.hlsli\""
#endif

cbuffer LightCB : register(LIGHT_CB_REGISTER, space0)
{
#include "light_cb_fields.hlsli"
};
#endif

// Keep in lockstep with WaterRippleSystem::kMaxBodies (C++).
#define WATER_MAX_RIPPLE_BODIES 8u

#endif // LIGHT_CB_HLSLI
