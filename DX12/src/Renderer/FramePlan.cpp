#include "Renderer/FramePlan.h"

#include <cstdio>

namespace FramePlan
{
namespace
{
    bool IsPathTrace(Path p) { return p == Path::PathTrace || p == Path::PathTraceRealtime; }

    // Appends "name=value" (and a separating space) while it fits.
    void Append(char* out, size_t outSize, size_t& used, const char* name, uint32_t value)
    {
        if (used >= outSize) return;
        const int n = std::snprintf(out + used, outSize - used, "%s%s=%u", used ? " " : "", name, value);
        if (n > 0) used = (used + static_cast<size_t>(n) < outSize) ? used + static_cast<size_t>(n) : outSize - 1;
    }

    // Every field once, in declaration order: Format and DiffMask walk the same list.
    template <class Fn>
    void ForEachField(const Decisions& d, Fn&& fn)
    {
        fn("tier", static_cast<uint32_t>(d.tier));
        fn("tierPartial", d.tierPartial);
        fn("ptOwns", d.ptOwns);
        fn("csm", d.csm);
        fn("ssrChain", d.ssrChain);
        fn("ptReaders", d.ptReaders);
        fn("ddgi", d.ddgi);
        fn("tlas", d.tlas);
        fn("fogOwner", static_cast<uint32_t>(d.fogOwner));
        fn("rr", d.rr);
        fn("rrRuns", d.rrRuns);
        fn("fsr", d.fsr);
        fn("dlssWanted", d.dlssWanted);
        fn("rtr", d.rtr);
        fn("ssr", d.ssrTier);
        fn("front", d.front);
        fn("rtShadow", d.rtShadow);
        fn("rtao", d.rtao);
        fn("pt", d.pt);
        fn("di", d.di);
        fn("gi", d.gi);
        fn("ssgi", d.ssgi);
        fn("xegtao", d.xegtao);
        fn("fogRt", d.fogRt);
        fn("dlss", d.dlss);
        fn("fsrResolved", d.fsrResolved);
        fn("dlssResolved", d.dlssResolved);
        fn("taaStandIn", d.taaStandIn);
        fn("postW", d.postW);
        fn("postH", d.postH);
        fn("rrGuidesWanted", d.rrGuidesWanted);
        fn("rrGuides", d.rrGuides);
    }
}

const char* PathName(Path p)
{
    switch (p)
    {
    case Path::Raster:            return "Raster";
    case Path::ReSTIR_DI:         return "ReSTIR_DI";
    case Path::ReSTIR_GI:         return "ReSTIR_GI";
    case Path::ReSTIR_DI_GI:      return "ReSTIR_DI_GI";
    case Path::PathTrace:         return "PathTrace";
    case Path::PathTraceRealtime: return "PathTrace_Realtime";
    default:                      return "Invalid";
    }
}

const char* TierName(const Decisions& d)
{
    switch (d.tier)
    {
    case Tier::T0:  return "T0";
    case Tier::T1:  return "T1";
    case Tier::T2:  return d.tierPartial ? "T2*" : "T2";
    case Tier::T3:  return !d.tierPartial ? "T3" : d.path == Path::ReSTIR_DI ? "T3:DI" : "T3:GI";
    case Tier::T4:  return "T4";
    case Tier::Ref: return "Ref";
    default:        return "Invalid";
    }
}

Path EffectivePath(const Intent& in, const State& s)
{
    const bool restir = in.path == Path::ReSTIR_DI || in.path == Path::ReSTIR_GI || in.path == Path::ReSTIR_DI_GI;
    return s.budgetRaster && restir ? Path::Raster : in.path;
}

// The frame's decisions (Renderer/FramePlan.h: the inputs, the stages and the
// rules). The comments name the RenderAsync site each decision gates.
Decisions PlanFrame(const Intent& in, const State& s, const Facts& f)
{
    Decisions d;
    const Path path = EffectivePath(in, s);
    d.path = path;
    const bool traced = IsPathTrace(path);
    const bool pathDI = path == Path::ReSTIR_DI || path == Path::ReSTIR_DI_GI;
    const bool pathGI = path == Path::ReSTIR_GI || path == Path::ReSTIR_DI_GI;
    // ReSTIR DI / GI also run in the lighting-bucket views; the path tracer
    // composites the whole frame, so it is Lit-only.
    const bool restirView = in.lit || in.lightingDebug;

    // ---- A: before the acceleration structure ----
    // The path tracer owns the frame: a path-tracing path, a Lit view and a
    // usable core (its camera rays need perspective). What it replaces is off
    // below while it holds; a path-tracing path without it falls back to the
    // raster features as the switches have them.
    d.ptOwns = traced && in.lit && s.ptPass && !s.ptInert && s.ptOutput && (!s.ptCameraPrimary || !s.ortho);
    // What ownership excludes (State::legacyPtRule, harness only: the path itself).
    const bool ptExcludes = s.legacyPtRule ? traced : d.ptOwns;
    // The camera rays' fog: the reference path tracer integrates the medium when it owns
    // the frame (a veto frame then has no fog, like what else it replaces); else the
    // ray-traced froxels where pass.volumetricFog.rtLighting wants them - auto on the hybrid
    // and path-traced paths, on wherever the device traces (the TLAS is then built for them).
    const bool hybridOrTraced = traced || pathDI || pathGI || in.rtReflections || in.rtShadows ||
                                (in.ssao && in.rtao);
    const bool fogRtWant = s.fog && s.fogRtUsable && !in.wireframe && !s.ortho &&
                           (s.fogRtMode == 2 || (s.fogRtMode == 1 && hybridOrTraced));
    d.fogOwner = d.ptOwns && path == Path::PathTrace && s.ptMedia ? FogOwner::PathTracer
               : fogRtWant ? FogOwner::RtFroxel : FogOwner::Raster;
    // While the path tracer owns the frame, what still reads the raster systems
    // it does not replace (FramePlan.h, the rules; ReaderBit).
    const bool readersGate = d.ptOwns && !s.ignorePtReaders;
    if (d.ptOwns)
    {
        // Camera-primary rays: LightingPass draws every pixel through the path
        // tracer's composite, toon pixels included.
        const bool toon      = s.toonDraws && !s.ptCameraPrimary;
        const bool toonUnlit = toon && !s.ptToonLighting;
        d.ptReaders = (s.translucentDraws ? kReaderTranslucent : 0u) | (s.waterDraws ? kReaderWater : 0u) |
                      (toon ? kReaderToon : 0u) | (toonUnlit ? kReaderToonUnlit : 0u) |
                      (s.particles ? kReaderParticles : 0u) |
                      (s.fog && d.fogOwner == FogOwner::Raster ? kReaderFog : 0u) |
                      (s.faceOverlay ? kReaderFaceOverlay : 0u) | (s.previewWorldViews ? kReaderPreview : 0u) |
                      (s.probeBake ? kReaderProbeBake : 0u) | (s.ddgiDebug ? kReaderDdgiDebug : 0u);
    }
    const uint32_t r = d.ptReaders;
    const bool csmRead    = (r & (kReaderToon | kReaderWater | kReaderTranslucent | kReaderParticles | kReaderFog |
                                  kReaderFaceOverlay | kReaderPreview | kReaderProbeBake)) != 0;
    const bool ddgiRead   = (r & (kReaderToon | kReaderTranslucent | kReaderWater | kReaderFaceOverlay |
                                  kReaderDdgiDebug)) != 0;
    const bool ssrRead    = (r & (kReaderWater | kReaderToon | kReaderTranslucent)) != 0;
    const bool shadowRead = (r & (kReaderWater | kReaderTranslucent | kReaderParticles | kReaderToonUnlit)) != 0;
    d.csm      = !readersGate || csmRead;
    d.ssrChain = !readersGate || ssrRead;
    // DDGIVolumesActive(): the probe trace and relight this frame.
    d.ddgi = s.ddgiReady && s.ddgiEnabled && s.ddgiVolumes > 0 && (!readersGate || ddgiRead);
    // The lighting tier the path and the switches describe (FramePlan.h, Tier).
    // Raster splits by what the switches add: any hybrid RT feature is T2
    // (partial unless RT reflections, RT shadows, RTAO and DDGI are all on),
    // DDGI alone T1, neither T0.
    {
        const bool rtr = in.rtReflections, sh = in.rtShadows, ao = in.ssao && in.rtao;
        if (path == Path::PathTrace)              d.tier = Tier::Ref;
        else if (path == Path::PathTraceRealtime) d.tier = Tier::T4;
        else if (pathDI || pathGI)
        {
            d.tier = Tier::T3;
            d.tierPartial = path != Path::ReSTIR_DI_GI;
        }
        else if (rtr || sh || ao)
        {
            d.tier = Tier::T2;
            d.tierPartial = !(rtr && sh && ao && d.ddgi);
        }
        else d.tier = d.ddgi ? Tier::T1 : Tier::T0;
    }
    // Each traced feature below, at its stage-A terms (B adds only the TLAS
    // and the promote list, which this demand decides).
    const bool rtrWant = !ptExcludes && !in.wireframe && in.rtReflections && !s.ortho && s.refl && s.reflReady &&
                         s.reflHwTier && s.reflUnified;
    const bool shadowWant = !in.wireframe && in.rtShadows && !s.ortho && s.rtShadow && s.rtShadowReady &&
                            s.rtShadowOutput && s.sun && (!readersGate || shadowRead);
    const bool aoWant = !ptExcludes && !in.wireframe && in.ssao && in.rtao && s.rtao && s.rtaoReady && s.rtaoOutput;
    const bool diWant = s.diPass && (pathDI || (d.ptOwns && s.ptRestirDI)) && restirView && !s.diInert && s.diOutput;
    const bool giWant = s.giPass && pathGI && restirView && !s.giInert && s.giOutput;
    // The TLAS: exactly when one of them could trace this frame.
    d.tlas = d.ptOwns || rtrWant || shadowWant || aoWant || diWant || giWant || fogRtWant;
    // Ray reconstruction (IsRayReconstructionActive) needs a ray-traced path it
    // can denoise - not Raster, not the reference path tracer, not camera rays
    // forced by a flag - in a Lit perspective view; that SELECTION sizes the
    // render target. rrRuns adds DLSS's health: a failed evaluate hands the
    // ReSTIR shells back to NRD and the path tracer back to accumulation until
    // ServiceRetry re-arms it. A DLSS_RR request that cannot run falls back to
    // DLSS SR off the path tracer (IsRRFallbackToDLSS); DLSS SR and FSR do not
    // run on a path-tracing path. Gated on the resolve block existing, as the
    // render half's site is.
    if (s.post)
    {
        d.rr = in.aa == AA::DLSS_RR && s.slRR && path != Path::Raster && path != Path::PathTrace &&
               !(path == Path::PathTraceRealtime && s.rrForcedCameraRays) && in.lit && !s.ortho;
        d.rrRuns = d.rr && s.dlssHealthy;
        d.fsr = in.aa == AA::FSR && !traced;
        const bool rrFallback = in.aa == AA::DLSS_RR && !traced && !d.rr && s.slDLSS;
        d.dlssWanted = (in.aa == AA::DLSS && !traced) || d.rr || rrFallback;
    }
    if (f.stage == Stage::A) return d;

    // ---- B: after the acceleration structure ----
    // Phase 2b.7: the reflection pass's hardware tier (rtrActive) and its
    // screen-space tier (screenActive) - neither under the path tracer, in an
    // orthographic view or in wireframe.
    if (s.refl)
    {
        d.rtr = rtrWant && f.tlas && f.promote;
        d.ssrTier = !ptExcludes && in.ssr && f.pyramid && s.reflReady && s.reflUnified && !s.ortho && !in.wireframe;
        // The glass's front layer: only behind a live hardware tier, with the
        // forward reflection fold on and a translucent draw to stamp.
        if (s.front)
            d.front = d.rtr && s.transparentPass && in.transparentReflections && f.frontDraws && s.frontReady;
    }
    // Phase 2b.8: the RT sun-shadow mask. It has no path-tracing term: on that
    // path it runs for its readers (water, translucents, particles, toon pixels
    // the path tracer does not light; shadowRead).
    if (s.rtShadow)
        d.rtShadow = shadowWant && f.tlas && f.promote;
    // Phase 2b.9: RTAO (SSAO and RTAO both on).
    if (s.rtao)
        d.rtao = aoWant && f.tlas && f.promote;
    // The path tracer and the ReSTIR shells. A missing TLAS vetoes the path
    // tracer for the frame; what it replaces stays off (ptOwns). The PT core's
    // x1 can take its direct light from ReSTIR DI.
    if (s.ptPass || s.diPass || s.giPass)
    {
        const bool tables = f.tlas && f.tlasTables && f.promote;
        d.pt = d.ptOwns && tables;
        d.di = s.diPass && (pathDI || (d.pt && s.ptRestirDI)) && restirView && !s.diInert && s.diOutput && tables;
        d.gi = giWant && tables;
    }
    // Phase 2b.10: SSGI, the near-field half - not under the path tracer or
    // ReSTIR GI (State::legacyPtRule: only while the path tracer RUNS).
    const bool ptReplaces = s.legacyPtRule ? d.pt : d.ptOwns;
    if (s.ssgiPass)
        d.ssgi = !ptReplaces && !d.gi && s.ssgiEnabled && s.ssgiUsable && f.depth && f.normal;
    // XeGTAO: the AO source unless the path tracer or RTAO owns it.
    d.xegtao = !ptReplaces && s.xegtaoPass && in.ssao && !d.rtao && !s.ortho && f.depth;
    // The ray-traced froxel lighting: for the camera rays, or under the path tracer's own fog
    // for the surfaces drawn after it. Without the tables an RtFroxel frame lights its fog
    // the raster way.
    d.fogRt = fogRtWant && f.tlas && f.tlasTables && f.promote;
    if (f.stage == Stage::B) return d;

    // ---- C: the resolve (phase 5) ----
    // The game half evaluates this stage too, without B's facts, for the post
    // size and the reactive mask (Renderer::PlanGameResolve): it reads
    // fsrResolved / dlssResolved / postW / postH, none of which needs B. dlss
    // does (the path tracer's pt) and is the render half's only.
    if (s.post)
    {
        // RRInputsReady: off the path tracer the raster guides are made here;
        // on it, DLSS reads them only at -rr_raster_guides>=2, else the path
        // tracer's own.
        const bool readsRasterGuides = d.rr && (!traced || f.rrRasterGuides >= 2);
        const bool rrInputs = !traced || readsRasterGuides || (s.ptPass && f.ptRRReady);
        d.dlss = d.dlssWanted && (!d.rr || d.pt || !traced) && (!d.rr || rrInputs);
        d.fsrResolved = d.fsr && s.fsrContext;
        d.dlssResolved = d.dlssWanted && s.dlssContext && !(d.rr && f.rrRaw) && (!d.rr || rrInputs);
        // A Super Resolution upscaler whose context is down (it failed to build, or
        // DLSS went unhealthy) resolves through TAA, in a view TAA runs in, rather
        // than leaving the frame un-antialiased.
        const bool srWanted = d.fsr || (d.dlssWanted && !d.rr);
        d.taaStandIn = srWanted && !d.fsrResolved && !d.dlssResolved && in.lit && !s.ortho;
        const bool display = d.fsrResolved || d.dlssResolved;
        d.postW = display ? s.viewportW : s.renderW;
        d.postH = display ? s.viewportH : s.renderH;
        // Wanted, then the RR guide render feature's Prepare allocates them
        // (Renderer/Features/RRGuideFeature.cpp) and reports rrGuidesPrepared.
        d.rrGuidesWanted = s.rrGuidePass && d.rr && (!traced || f.rrRasterGuides > 0);
        d.rrGuides = d.rrGuidesWanted && f.rrGuidesPrepared;
    }
    return d;
}

uint64_t DiffMask(const Decisions& a, const Decisions& b)
{
    uint32_t values[64] = {};
    uint32_t count = 0;
    ForEachField(a, [&](const char*, uint32_t v) { if (count < 64) values[count++] = v; });
    uint64_t mask = 0;
    uint32_t i = 0;
    ForEachField(b, [&](const char*, uint32_t v) { if (i < count && values[i] != v) mask |= 1ull << i; ++i; });
    return mask;
}

void Format(const Decisions& d, char* out, size_t outSize)
{
    if (!outSize) return;
    out[0] = '\0';
    size_t used = 0;
    ForEachField(d, [&](const char* name, uint32_t v) { Append(out, outSize, used, name, v); });
}

void ForEachDecision(const Decisions& d, void (*fn)(void* ctx, const char* name, uint32_t value), void* ctx)
{
    ForEachField(d, [&](const char* name, uint32_t v) { fn(ctx, name, v); });
}
}
