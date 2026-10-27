#pragma once
// The RTXDI ReSTIR GI core: RTXDI-Library's GI temporal and spatial resampling
// (RTXDI_GITemporalResampling / RTXDI_GISpatialResampling, its bias-correction
// modes, its boiling filter) through the engine's bridge (RtxdiBridge.hlsli in GI
// mode) inside the RTGI shell (RtxdiGI.cs.hlsl includes RTGI.cs.hlsl).
// Trace, the coverage partition, NRD and Resolve stay the shell's; the
// receivers, the target and integrand, the visibility rays, the history
// re-trace / re-shade and the Shade pass are the engine core's, so the two
// cores differ only in how they resample:
//   * the history validation runs as a pre-pass over the history buffer
//     (RTXDI loads the previous reservoir inside its temporal function);
//   * Trace's 40-byte reservoir converts into RTXDI's packed one (W = 1/pdf),
//     a genuine zero keeping its population (a point far in front, W 0);
//   * the final reservoir converts back for the engine's Shade.
// Built only with the vendored SDK (ENGINE_WITH_RTXDI): without it the factory
// returns nullptr and the shell keeps the engine core (RtxdiInertReason).

#include "RenderGraph/RenderPass/RTGICore.h"
#include <memory>

namespace RTGI
{
// nullptr when this build has no RTXDI SDK.
std::unique_ptr<Core> MakeRtxdiCore();
} // namespace RTGI
