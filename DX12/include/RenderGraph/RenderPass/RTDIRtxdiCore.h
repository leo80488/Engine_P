#pragma once
// The RTXDI ReSTIR DI core: RTXDI-Library's light sampling (power RIS / ReGIR
// over the engine's light tables) and its temporal / spatial resampling (its
// bias-correction modes), through the engine's bridge (RtxdiBridge.hlsli in DI
// mode) inside the RTDI shell (RtxdiDI.cs.hlsl includes RTDI.cs.hlsl). Receivers,
// eligibility, integrand and target, visibility rays and NRD packing are the
// engine core's: the two cores differ only in how they sample and reuse.
//
// One class for every composition with an RTXDI half:
//   rtxdi        = RTXDI candidates x RTXDI reuse
//   hybrid       = RTXDI candidates x the engine's visibility-aware reuse
//   engine+rtxdi = the engine's candidates x RTXDI reuse
// An engine half runs RTDIEngineCore's own entries; the two reservoir formats
// convert exactly (RtxdiToEngine / EngineToRtxdi: the light point travels as the
// engine seed == RTXDI's packed uv).
//
// Built only with the vendored SDK (ENGINE_WITH_RTXDI): without it the factory
// returns nullptr and the shell keeps the engine core (RtxdiInertReason).

#include "RenderGraph/RenderPass/RTDICore.h"
#include <memory>

namespace RTDI
{
// nullptr when this build has no RTXDI SDK or @p composition has no RTXDI half.
std::unique_ptr<Core> MakeRtxdiCore(uint32_t shellFlags, const ReSTIR::DIComposition& composition);
} // namespace RTDI
