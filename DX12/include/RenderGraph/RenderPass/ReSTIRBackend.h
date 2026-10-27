#pragma once
// ReSTIR backend selection.
//
// Every ReSTIR stage keeps ONE graph pass "shell" (RTDI / RTGI / PT.Trace)
// that owns the downstream contract - result textures, NRD, the Lighting fold
// indices, log fields, profiler region names - and swaps only the resampling
// "core" in the middle. These names are the vocabulary engine.json, the CLI,
// Lua and the editor use for that choice:
//   DI : engine | rtxdi | hybrid (RTXDI candidates + engine visibility-aware reuse)
//   GI : engine | rtxdi
//   PT : auto | area | rtxdi  (Reference is the ground truth, never a backend)
// "auto" is PT's default request and splits by render path: RTXDI's ReSTIR PT on
// Path Trace (Real-time), the engine's own path tracer ("area") on Path Trace
// (Reference) - and on the real-time path when a command-line pin asks for it:
// an explicit bounce count, -pt_reference, Area reuse, a thin lens or
// camera-primary rays (PathTracePass::ResolveRequest).
// A request the build or the device cannot honour falls back LOUDLY to the
// engine core: the [RESTIR_BACKEND] log line and Settings.caps().restir carry
// requested / effective / reason, and packs assert the effective backend.

#include "Graphics/RootSignatureLayout.h"
#include <cstdint>
#include <string>
#include <string_view>

class IGraphicsDevice;
namespace RHI { struct PipelineLayout; }

namespace ReSTIR
{
enum class Stage : uint8_t { DI = 0, GI = 1, PT = 2, Count };
enum class Backend : uint8_t { Engine = 0, Rtxdi = 1, Hybrid = 2, Area = 3, Auto = 4, Count };
// Shadow comparison: a second shell instance runs the OTHER core in the same
// frame and writes a private result that is never composited.
enum class Compare : uint8_t { Off = 0, DI = 1, GI = 2, Count };

const char* StageName(Stage stage);          // "DI" / "GI" / "PT"
const char* BackendName(Backend backend);    // "engine" / "rtxdi" / "hybrid" / "area" / "auto"
const char* CompareName(Compare compare);    // "off" / "di" / "gi"
bool IsLegal(Stage stage, Backend backend);
// The engine-owned core of a stage, always available - every fallback's
// target: DI engine, GI engine, PT area.
Backend DefaultBackend(Stage stage);
// The request a stage starts with (engine.json's default): DI engine, GI
// engine, PT auto.
Backend DefaultRequest(Stage stage);
// The date until which a core that lost its stage's A/B stays selectable in
// the editor and engine.json (after it only the CLI and the nightly reference
// packs select it, then it is deleted), or nullptr. A static string.
const char* RetirementDate(Stage stage, Backend backend);
// An ISO date "YYYY-MM-DD" as the number YYYYMMDD; 0 when it is not a date.
int DateNumber(const char* iso);
// The cores RetirementDate still names whose date is before @p today (a DateNumber), as
// "DI rtxdi (<date>), ..."; empty when none. The editor menu offers exactly these.
std::string OverdueRetirements(int today);
// Case-insensitive; also accepts the backend's index ("0", "1", ...).
bool ParseBackend(Stage stage, std::string_view token, Backend& out);
bool ParseCompare(std::string_view token, Compare& out);
// "engine|rtxdi|hybrid" etc., for error messages.
std::string LegalBackends(Stage stage);

// The DI / GI shell's denoiser (engine.json restirDiDenoiser / restirGiDenoiser,
// the editor's "Denoiser" combos). Under DLSS Ray Reconstruction it is the
// denoiser BEFORE RR. Auto: DI RELAX (no NRD under RR, full resolution), GI
// RELAX (kept under RR).
enum class Denoiser : uint8_t { Auto = 0, Relax = 1, Reblur = 2, None = 3, Count };
const char* DenoiserName(Denoiser d);   // "auto" / "relax" / "reblur" / "none"
// Case-insensitive; also accepts the index ("0".."3").
bool ParseDenoiser(std::string_view token, Denoiser& out);

// DI only: a DI core is {candidates} x {reuse}, and the named backends are
// corners of that product - engine = engine x engine, rtxdi = rtxdi x rtxdi,
// hybrid = rtxdi candidates x engine visibility-aware reuse. The dev flags
// -di_backend_initial= / -di_backend_reuse= pick either half on its own;
// engine candidates x rtxdi reuse is the corner with no name.
struct DIComposition
{
    Backend initial = Backend::Engine;
    Backend reuse = Backend::Engine;
    bool operator==(const DIComposition&) const = default;
};
DIComposition Decompose(Backend named);
// "engine" / "rtxdi" / "hybrid" / "engine+rtxdi" (a static string).
const char* CompositionName(const DIComposition& c);
// One half of a composition: engine | rtxdi (case-insensitive).
bool ParseHalf(std::string_view token, Backend& out);

// Why a stage's RTXDI core cannot run here, or nullptr when it can, most
// fundamental first: the SDK is not in this build (ENGINE_WITH_RTXDI=0;
// tools/vendor_rtxdi.py unpacks it), the device lacks what the core compiles
// for (PT: native 16-bit shader ops), the core itself is not in this build.
// A static string.
const char* RtxdiInertReason(Stage stage, bool native16BitShaderOps);
// The vendored RTXDI-Library commit, short form (the first 7 characters of
// tools/vendor_rtxdi.py's COMMIT), or "none" without the SDK.
const char* RtxdiSdkVersion();

// What one shell reports about its core choice ([RESTIR_BACKEND], caps()).
struct BackendStatus
{
    Backend requested = Backend::Engine;
    Backend effective = Backend::Engine;
    // The names the log and caps() print: BackendName of the two above, or a
    // DI composition's (CompositionName). Static strings.
    const char* requestedName = "engine";
    const char* effectiveName = "engine";
    // Why effective differs from requested ("selected" when it does not).
    // A static string: no ownership.
    const char* reason = "selected";
    double pipelineMs = 0.0;          // the effective core's last pipeline build
    uint64_t residentBytes = 0;       // the effective core's GPU working set
    std::string ignored;              // stage flags on the command line this core does not read
};

// The shells' compute pipeline layout (CreateShellLayout below): the RTDI and
// RTGI shells, every core they run, the PT RTXDI core and the RTXDI probe bind
// it; shaders/RtxdiBridge.hlsli declares the same registers.
namespace ShellSlots
{
inline constexpr RHI::Slots::CmpParam CB       { 0 };  // b0 root CBV: the shell's constants
inline constexpr RHI::Slots::CmpParam TLAS     { 1 };  // t0 root SRV
inline constexpr RHI::Slots::CmpParam Instances{ 2 };  // t1 root SRV
inline constexpr RHI::Slots::CmpParam Alpha    { 3 };  // t2 root SRV: the alpha records
inline constexpr RHI::Slots::CmpParam Buffers  { 4 };  // table t0 space1: the bindless buffers
inline constexpr RHI::Slots::CmpParam Textures { 5 };  // table t0 space2: the bindless textures
inline constexpr RHI::Slots::CmpParam Result   { 6 };  // table u0
inline constexpr RHI::Slots::CmpParam CoreCB   { 7 };  // b1 root CBV: a core's own constants
inline constexpr uint32_t ParamCount = 8;
} // namespace ShellSlots

// The shell layout: ShellSlots' root CBVs b0 / b1 and root SRVs t0..t2, the
// 16384-entry bindless tables (t0 space1 buffers, t0 space2 textures), the u0
// Result table, the linear-wrap static sampler s1 (no comparison) and heap
// direct indexing. Each holder creates its own (@p name: the [PIPELINE_LAYOUT]
// line's) and builds every pipeline it binds on it.
bool CreateShellLayout(IGraphicsDevice& gfx, RHI::PipelineLayout& out, const char* name);
} // namespace ReSTIR
