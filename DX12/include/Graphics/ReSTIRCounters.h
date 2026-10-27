#pragma once
// ReSTIR counter ABI: the GPU counter slots behind the [RTDI] / [RTGI] log lines
// that scenario packs assert on.
//
// The SHELL fills the shell slots from its own entries (Classify, Prepare,
// Resolve, NRD bookkeeping) whatever the core is. The CORE fills the reuse
// slots; a core with no counters of its own (RTXDI) fills them from its
// wrapper kernels or leaves them 0 - never another meaning. Shader mirrors:
// the Count(<slot>) indices in RTDI.cs.hlsl / RTGI.cs.hlsl and their Clear
// loops (DI::Count / GI::Count below).

#include <cstdint>

namespace ReSTIR::Counters
{
namespace DI
{
enum Slot : uint32_t
{
    Eligible          = 0,   // shell: Classify (also the compaction count)
    Visibility        = 1,   // core : visibility rays of the selected samples
    Selected          = 2,   // core : pixels shaded from a sample
    NonFinite         = 3,   // shell+core: nonfinite shaded / resolved values
    Candidates        = 4,   // core : initial candidates streamed
    ReusedTemporal    = 5,   // core
    ReusedSpatial     = 6,   // core : spatial stage 0 neighbours accepted
    Occluded          = 7,   // core : selected samples found occluded
    MisNaN            = 8,   // core : invalid MIS weights
    MSum              = 9,   // core : represented M of the shaded reservoirs
    NrdHistoryMax     = 10,  // shell: -di_history_counts
    NrdHistorySum     = 11,  // shell
    NrdHistoryCount   = 12,  // shell
    ReusedSpatial2    = 13,  // core : spatial stage 1 neighbours accepted
    SpatialStage0     = 14,  // core : stage-0 dispatch marker
    SpatialStage1     = 15,  // core : stage-1 dispatch marker
    RawMirrorPixels   = 16,  // shell: Resolve's raw near-mirror escape
    BoundaryFirst     = 17,  // shell: 17..23 the -di_boundary_probe oracle
    BoundaryLast      = 23,
    ConfidenceRays    = 24,  // shell: Classify's visibility-confidence retest
    VisibilityChanges = 25,  // shell
    Boiled            = 26,  // core : boiling filter rejections
    SpatialVisibility = 27,  // core
    TemporalVisibility= 28,  // core
    ResolveFallback   = 29,  // shell: low-weight Resolve fallbacks
    LightVanished     = 30,  // core : history samples whose light is gone
    Count             = 31
};
constexpr bool IsShellSlot(uint32_t s)
{
    return s == Eligible || s == NonFinite || (s >= NrdHistoryMax && s <= NrdHistoryCount) ||
           s == RawMirrorPixels || (s >= BoundaryFirst && s <= BoundaryLast) ||
           s == ConfidenceRays || s == VisibilityChanges || s == ResolveFallback;
}
} // namespace DI

namespace GI
{
enum Slot : uint32_t
{
    Eligible        = 0,   // shell: Trace
    Rays            = 1,   // shell
    Resolved        = 2,   // shell
    Visibility      = 3,   // shell+core
    SkinnedHits     = 4,   // shell
    NonFinite       = 5,   // shell+core
    ReusedTemporal  = 6,   // core
    ReusedSpatial   = 7,   // core
    Validated       = 8,   // core : history re-traced and re-shaded
    Rejected        = 9,   // core
    SpatialVisibility = 10,// core
    LuminanceResets = 11,  // core
    ManyLightHits   = 12,  // shell: hit shading
    Boiled          = 13,  // core
    EmitterVisible  = 14,  // shell: emitter candidates
    EmitterSelected = 15,  // shell
    ReuseFirst      = 16,  // core : 16..24 the [RTGI_REUSE] rejection reasons
    ReuseLast       = 24,
    // shell: the SHaRC radiance cache ([RTGI_SHARC])
    SharcHits       = 25,  // hit shading answered by the cache
    SharcMisses     = 26,  // no entry above the maturity threshold
    SharcGated      = 27,  // the voxel too large, or the segment shorter than its diagonal
    SharcIneligible = 28,  // Unlit / skinned / untrusted hit: always analytic
    SharcUpdatePaths= 29,  // update vertices deposited
    SharcBadRadiance= 30,  // a non-finite or clamped deposit
    SharcModeFallback=31,  // a cached sample re-shaded analytically at validation
    Count           = 32   // the shader clears 32
};
} // namespace GI
} // namespace ReSTIR::Counters
