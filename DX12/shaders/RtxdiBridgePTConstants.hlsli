// RtxdiBridgePTConstants.hlsli - the engine path tracer's state an RTXDI PT
// TU embeds as g_PT. Mirror of PTRtxdi::EngineConstants (PTRtxdiCore.cpp),
// member for member; the core checks the offsets against this TU's reflection
// when it builds.
#ifndef RTXDI_BRIDGE_PT_CONSTANTS_HLSLI
#define RTXDI_BRIDGE_PT_CONSTANTS_HLSLI

// ptFlags
static const uint kRabPTDiffuse = 1u;               // -pt_diffuse: a Lambertian BSDF, cosine sampling (flags & 1)
static const uint kRabPTConstantEnvironment = 2u;   // -pt_environment: a constant sky (PathTrace flags & 2)
static const uint kRabPTNoSunNee = 4u;              // -pt_no_sun_nee: the sun only through BSDF hits (flags & 4)
static const uint kRabPTCounters = 8u;              // count into counterUav ([PT_RTXDI])
static const uint kRabPTAccumulate = 16u;           // the image is progressive this frame (a static scene)
static const uint kRabPTDecorrelation = 32u;        // RTXDI's DLSS-RR decorrelation (the preserved initial sample)
static const uint kRabPTDirectOnly = 64u;           // -pt_direct_only: only the primary's direct light (flags & 64)
static const uint kRabPTDiResult = 128u;            // the primary's local lights come from RestirDIResult
static const uint kRabPTReplayNoRc = 256u;          // -rtxdi_pt_replay_no_rc=1: the library's own no-reconnection replays (upstream PR #42 A/B)
// Optional reuse and reservoir features, each an engine kernel or hook:
static const uint kRabPTUnified = 512u;             // -rtxdi_pt_unified=1: x1's next-event light in the PT reservoir
static const uint kRabPTDupMap = 1024u;             // -rtxdi_pt_duplication_map=1: duplication-based history reduction (biased)
static const uint kRabPTDualMV = 2048u;             // -rtxdi_pt_dual_mv=1: the occluder's motion for a disoccluded pixel
static const uint kRabPTRgb = 4096u;                // -rtxdi_pt_rgb=1: shade every pairwise candidate in RGB
static const uint kRabPTCompaction = 8192u;         // -rtxdi_pt_compaction=1: random replays deferred to a compacted pass
static const uint kRabPTRRGuides = 16384u;          // Shade writes the DLSS-RR guides (rrAlbedoUav..rrDistanceUav)
static const uint kRabPTRRCorruptShift = 17u;       // (ptFlags >> 17) & 7 = 1 + the guide Shade fills with noise (-dlss_rr_corrupt_guide)
static const uint kRabPTGlass = 1u << 20;           // the TLAS holds a Transmissive instance: glass is a path vertex

// mediaFlags: what a continuation segment keeps through the fog (pass.ptRtxdi.mediaContinuation); a
// light segment keeps the whole transmittance. Read only by the kernels compiled with RAB_MEDIA.
static const uint kRabMediaAbsorption = 0u;         // exp(-tau_a): what absorption alone removes
static const uint kRabMediaNone = 1u;               // nothing: the segment passes untouched
static const uint kRabMediaFull = 2u;               // the whole transmittance
static const uint kRabMediaFroxel = 3u;             // inside the froxel fog's last lit volume the whole
                                                    // transmittance and the light it scattered in;
                                                    // outside it, absorption (RtxdiBridge.hlsli)

struct RAB_PTConstants
{
    float3 sunDirection; float sunCosRadius;    // the light travels along sunDirection; its cone's cos(radius)
    float3 sunColor; uint skyIdx;                // skyIdx: the sky radiance cube (bindless SRV), 0 = none
    float environmentScale; float constantEnvironment; uint ptFlags; uint pathMaterialsIdx;
    uint lightAliasIdx, activeLocalLightCount, resultUav, accumUav;
    uint diResultIdx, reservoirUav, neighborOffsetsSrv, counterUav;
    // mediumIdx: this frame's Media::Set (Graphics/MediumSet.h; bindless SRV), 0 = no fog.
    uint spp, frameIndex, mediumIdx, mediaFlags;
    float3 prevPrevCamera; float pad2;
    // The duplication map (sample ids, R32_UINT; the map, R16G16_FLOAT), the
    // pairwise spatial pass (scratch: 2 x uint4 per pixel and pairing; the
    // replay list; its counter + dispatch arguments; the RGB estimate) and
    // the per-frame pairings: partner_k(p) = ((p + t) ^ m) - t, t = .xy, m = .zw.
    uint sampleIdUav, dupMapUav, pairScratchUav, pairListUav;
    uint pairArgsUav, rgbUav, pairCount, pairCounterUav;
    // Local-light next-event candidates (power RIS; 1 = the plain power pick):
    // at the path's vertices, and at x1 under kRabPTUnified. Then the smoothed
    // stagnancy map, this frame's and the last (bindless UAVs; the last is 0
    // without a history), under kRabPTDecorrelation.
    uint neeCandidates, neeCandidatesX1, stagnancyUav, prevStagnancyUav;
    // PathTracePass's DLSS-RR guides, PathTrace.cs.hlsl's four - diffuse
    // albedo, specular albedo, normal + roughness (RGBA16F), the specular hit
    // distance (R32F); read only under kRabPTRRGuides.
    uint rrAlbedoUav, rrSpecularUav, rrNormalUav, rrDistanceUav;
    // SHaRC (RtxdiBridgeSharc.hlsli): the radiance cache's three buffers
    // (bindless UAVs: hash keys, accumulation, resolved), its capacity, grid
    // scale and fixed-point scale; the sparse update's tile edge and path
    // length; the resolve's previous camera and accumulation / stale windows.
    // Read only by the kernels compiled with RAB_SHARC.
    uint sharcHashUav, sharcAccumulationUav, sharcResolvedUav, sharcCapacity;
    float sharcSceneScale, sharcRadianceScale; uint sharcUpdateTile, sharcUpdateBounces;
    float3 sharcPrevCamera; uint sharcAccumulationFrames;
    uint sharcStaleFrames, sharcFrame, sharcFlags; float sharcMinRoughness;   // sharcFlags: kRabSharc*
    uint4 pairing[4];
    float4x4 prevViewProj;                       // the previous frame's, row vectors (transposed upload)
    // The froxel fog's last lit volume (kRabMediaFroxel; Graphics/MediumSet.h's FroxelVolume): the
    // view-projection it was lit for (as the fog's constants hold it), that camera, its SRV (0 = none)
    // and its radial range.
    float4x4 froxelViewProj;
    float3 froxelCamera; uint froxelHistoryIdx;
    float froxelNear, froxelFar; uint2 froxelPad;
};
#endif
