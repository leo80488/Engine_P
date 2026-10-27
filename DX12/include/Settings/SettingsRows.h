#pragma once

// Settings/SettingsRows.h - the setting rows: one per setting, written once in an
// X-macro .inl and expanded into a table every consumer reads.
//
// Three tables, one per document:
//   player     include/Resource/EngineSettingsSchema.inl - settings.json, members of
//              Resource::EngineSettings (Row)
//   developer  include/Settings/Rows/Developer.inl - engine.json, members of
//              Resource::EngineDevSettings (DevRow)
//   pass       include/Settings/Rows/Passes.inl - not saved, members of
//              Resource::PassSettings (PassRow): the render passes' tuning
//              parameters, read through Settings::Pass::<member>()
// A row holds a pointer-to-member of its document's struct, so a renamed field is a
// build error, not a silently dead row - which is why a row is a template on that struct
// (RowT<S>) rather than a type-erased offset. Each document's load / save, apply /
// capture and the Lua Settings bridge walk its table; none keeps a list of its own.
//
// What a row carries:
//   kind    Bool / Int / Float / Enum (the value is the label index) / Choice (the
//           value is one of `values`) / String
//   path    "section.key": the file's group and key, the Lua path and the catalog
//           name are the same word
//   title,  what a UI shows: the editor's Project Settings control and its tooltip,
//   tip     Lua Settings.meta, gen_catalog --setting. Without a title a UI uses the key
//   lo, hi  Int / Float: the range a UI offers (and Coerce clamps a write to). AtLeast
//           also reads lo
//   labels  Enum: '|'-joined display labels. String: the legal set - every writer that
//           Coerces (Settings.set, -set:, the pipe) takes only those; the load enforces
//           it with OneOf (engine.json's name rows keep their own name rules instead)
//   values  Choice: '|'-joined legal integers
//   codec   Enum only, when the JSON holds a token rather than the index
//           (antialiasing.mode, display.hdrMode, features.renderPath), so inserting a
//           mode cannot re-point every file on disk
//   san     what the load does to the value it read. Not the UI range:
//           features.rtReflectionsRayTMax loads unclamped (its setter clamps),
//           display.windowWidth/Height follow a pair rule instead
//   flags   kSettingRestart (applies at the next launch), kSettingNeedsDXR,
//           kSettingLegacyValueOnly
//   phase   when ApplyTo / CaptureFrom reach the row (below)
//   apply   push the row's value to its owner (a Renderer setter, the present
//           settings, an audio event...); capture reads it back. Captureless lambdas
//           in the row itself.
//   policy  how the settings store picks the row's effective value from its layers
//           (Policy below; Settings/SettingsStore.h)
//   legacyFlag  the row's old command-line spellings: a pass row's own flag, an
//           engine.json row's old flag (the syntax and how each document reads them:
//           Settings/LegacyFlags.h)
//   legacyRead  engine.json rows whose old flags need logic (a cross-row default, a
//           value-dependent flag): reads them instead of legacyFlag
//
// The macro, as the includer defines it:
//   SETTING(Kind, "path", member, .field = value, ...)
// The tail is a C++20 designated initializer of RowSpecT<S>, so its fields must appear
// in RowSpecT's declaration order.

#include "Resource/EngineSettings.h"      // complete types: rows point at their members
#include "Resource/EngineDevSettings.h"
#include "Settings/PassSettings.h"
#include "Graphics/ISurface.h"            // RHI::PresentSettings: the Present phase's accumulator

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Renderer;
class IGraphicsDevice;
namespace Plat { class Window; }

namespace Settings
{
    struct Value;   // Settings/SettingsStore.h

    // What reading a row's old spellings found (Settings/LegacyFlags.h).
    enum class LegacyResult : uint8_t { Absent, Taken, Ignored, Refused };

    // When ApplyTo / CaptureFrom reach a row. The player phases run in this order and the
    // rows of a phase in .inl order. Load-bearing: Path comes before Renderer because
    // SetAAMode -> UpdateAAEnabled reads the render path (TAA would stay on across a switch
    // to the path tracer otherwise). Developer rows are all Renderer (or None / Applied),
    // in .inl order.
    enum class Phase : uint8_t
    {
        None,          // not applied, not captured (ui.language: the script layer owns it)
        Present,       // fills ApplyCtx::ps; ONE SetPresentSettings at the end of the phase
        Path,          // the render path
        Renderer,      // Renderer setters
        Audio,         // one BusVolumeChangedEvent per bus, in row order
        PostProcess,   // the engine-default profile and its authored mirror
        Window,        // fullscreen request; the size is requested ONCE at the end
        Boot,          // applied at boot by its owner; only captured (ui.defaultFont)
        Applied,       // applied, never captured: the FILE owns the value, and a
                       // read-modify-write Save keeps what was loaded
                       // (culling.chainPhysicsLod - see Developer.inl)
    };

    // The player rows' hook contexts.
    struct ApplyCtx
    {
        Renderer&            r;
        IGraphicsDevice&     gfx;
        Plat::Window*        window = nullptr;
        RHI::PresentSettings ps{};                // the Present rows fill it
        int                  windowW = 0, windowH = 0;   // the Window rows fill them
    };
    struct CaptureCtx
    {
        Renderer&            r;
        IGraphicsDevice&     gfx;
        const Plat::Window*  window = nullptr;
        RHI::PresentSettings ps{};                // read once when the Present phase starts
    };
    // The developer rows' contexts (they only reach the Renderer). fromCommandLine: the
    // value being applied is the row's command-line one (an old flag spelling or -set:);
    // ApplyDevRows / ApplyDevRow set it per row, so a hook can keep a rule that holds for a
    // command-line value only (Renderer::DevPin).
    struct DevApplyCtx   { Renderer& r; bool fromCommandLine = false; };
    struct DevCaptureCtx { const Renderer& r; };
    // The pass rows have no hooks: the passes read them (PassSettings.h).
    struct PassApplyCtx   {};
    struct PassCaptureCtx {};

    // The struct a row's member belongs to -> the contexts its hooks take.
    template <class S> struct Domain;
    template <> struct Domain<Resource::EngineSettings>    { using Apply = ApplyCtx;    using Capture = CaptureCtx; };
    template <> struct Domain<Resource::EngineDevSettings> { using Apply = DevApplyCtx; using Capture = DevCaptureCtx; };
    template <> struct Domain<Resource::PassSettings>      { using Apply = PassApplyCtx; using Capture = PassCaptureCtx; };

    enum class Kind : uint8_t { Bool, Int, Float, Enum, Choice, String };

    enum RowFlags : uint32_t
    {
        kSettingNone     = 0,
        // Takes effect at the next process start (there is no restart
        // mechanism): a UI must say so, because the value being accepted is
        // not the value taking effect.
        kSettingRestart  = 1u << 0,
        kSettingNeedsDXR = 1u << 1,
        // A pass row whose fallback the pass computes itself: "-x" given bare or with a
        // malformed number is no value - the row stays unset, Given() false - instead of the
        // row's default (Settings/LegacyFlags.h).
        kSettingLegacyValueOnly = 1u << 2,
    };

    // How the settings store picks a row's effective value.
    //   Layered    the highest layer holding a value wins (Runtime > CommandLine > Scene >
    //              Player > Project > Scalability > Default). Every new row is Layered.
    //   LastWrite  the most recent write wins, whatever its layer: the policy of a family
    //              whose writers each call its setter in turn.
    //   Capped     (Bool rows only) the scene states an intent, every other layer an
    //              allowance: effective = (the highest non-Scene layer holding a value) &&
    //              (the Scene layer, or true when it holds nothing). The player's switch is
    //              a real cap - "off" turns the feature off - and the scene decides whether
    //              its world uses it at all; Source is the Scene layer exactly when the
    //              scene is what turned it off. The renderer applies the same rule
    //              (features.ddgi / ssgi / ssao / volumetricFog).
    enum class Policy : uint8_t { Layered, LastWrite, Capped };

    // What the load does to a value after reading it.
    enum class San : uint8_t
    {
        None,         // as loaded
        Clamp,        // Float: NaN -> fallback, then clamp to [lo, hi]. Int: clamp to [lo, hi]
        Range,        // Int: outside [lo, hi] -> fallback. Enum: outside [0, labels-1]
        OneOf,        // Choice: not one of `values` -> fallback. String: not in `labels` -> fallbackStr
        AtLeast,      // Int / Choice: below lo -> fallback
        ClampFinite,  // Float: NaN or infinite -> fallback, then clamp to [lo, hi]
    };

    // An Enum written as a token. fromToken maps an unknown token to that
    // enum's own fallback (TAA, SDR, Raster) - a file from a build that knew a
    // mode this one does not degrades instead of failing.
    struct TokenCodec
    {
        const char* (*toToken)(int value);
        int         (*fromToken)(const std::string& token);
        // The token Load parses when the key is absent but its section is
        // there; nullptr keeps the current value.
        const char*  absentToken;
    };

    template <class S>
    struct RowSpecT
    {
        const char*       title       = nullptr;   // a UI's label; nullptr: the key
        const char*       tip         = nullptr;   // a UI's tooltip; nullptr: none
        float             lo          = 0.f;
        float             hi          = 0.f;
        const char*       labels      = nullptr;
        const char*       values      = nullptr;
        const TokenCodec* codec       = nullptr;
        San               san         = San::None;
        double            fallback    = 0.0;
        const char*       fallbackStr = nullptr;
        uint32_t          flags       = kSettingNone;
        Phase             phase       = Phase::None;
        void (*apply)(const S&, typename Domain<S>::Apply&)  = nullptr;
        void (*capture)(S&, typename Domain<S>::Capture&)    = nullptr;
        Policy            policy      = Policy::Layered;
        const char*       legacyFlag  = nullptr;   // the old spellings: Settings/LegacyFlags.h
        // Reads the row's old flags itself: Taken sets `out` and `writer`
        // (the flag credited), Refused sets `why` (the boot refuses the run).
        LegacyResult (*legacyRead)(Value& out, std::string& writer, std::string* why) = nullptr;
    };

    template <class S>
    struct RowT
    {
        using Struct = S;
        const char* path = nullptr;
        Kind        kind = Kind::Bool;
        // Exactly one is set, selected by kind (Int / Enum / Choice share mi).
        bool        S::* mb = nullptr;
        int         S::* mi = nullptr;
        float       S::* mf = nullptr;
        std::string S::* ms = nullptr;
        RowSpecT<S> spec;
        // Split once from path / labels / values.
        std::string              section, key;
        std::vector<std::string> labels;
        std::vector<int>         values;
    };

    using RowSpec    = RowSpecT<Resource::EngineSettings>;
    using Row        = RowT<Resource::EngineSettings>;
    using DevRowSpec = RowSpecT<Resource::EngineDevSettings>;
    using DevRow     = RowT<Resource::EngineDevSettings>;
    using PassRowSpec = RowSpecT<Resource::PassSettings>;
    using PassRow     = RowT<Resource::PassSettings>;
    using ApplyFn    = void (*)(const Resource::EngineSettings&, ApplyCtx&);
    using CaptureFn  = void (*)(Resource::EngineSettings&, CaptureCtx&);

    // The player rows, in declaration order.
    std::span<const Row> PlayerRows();
    const Row* FindPlayerRow(std::string_view path);
    // The developer rows (engine.json), in declaration order.
    std::span<const DevRow> DevRows();
    const DevRow* FindDevRow(std::string_view path);
    // The pass rows, in declaration order.
    std::span<const PassRow> PassRows();
    const PassRow* FindPassRow(std::string_view path);

    // EngineSettings::ApplyTo / CaptureFrom: every row's hook, phase by phase.
    void ApplyPlayerRows(const Resource::EngineSettings& s, ApplyCtx& c);
    void CapturePlayerRows(Resource::EngineSettings& s, CaptureCtx& c);
    // EngineDevSettings::ApplyTo / CaptureFrom: every row's hook in .inl order.
    void ApplyDevRows(const Resource::EngineDevSettings& s, DevApplyCtx& c);
    void CaptureDevRows(Resource::EngineDevSettings& s, DevCaptureCtx& c);

    // ONE row's hook, the rest of its phase left as it is live: a Present row
    // changes its field of the surface's current present settings, a window
    // size row its axis of the window's requested size. False for a row with
    // no apply (Phase::None / Boot - nothing to push to). The caller drains
    // the CPU frame first, as ApplyTo does.
    bool ApplyPlayerRow(const Row& r, const Resource::EngineSettings& s, ApplyCtx& c);
    // ONE developer row's hook (each reads only its own field). False for a row with no
    // apply (rayReconstructionLoad, gameFlow, debug, upload.streamRingMiB,
    // skinning.vertexBudget: their owners read the file).
    bool ApplyDevRow(const DevRow& r, const Resource::EngineDevSettings& s, DevApplyCtx& c);

    // The token codecs (also what the settings peeks read). kLevelCodec: a quality level
    // row's (Settings/Scalability.h) - "Low" .. "Cinematic", any case; an unknown name
    // reads High.
    extern const TokenCodec kAAModeCodec;
    extern const TokenCodec kHDRModeCodec;
    extern const TokenCodec kRenderPathCodec;
    extern const TokenCodec kLevelCodec;

    // ---- Building a table (the TUs that expand an .inl) ----
    namespace detail
    {
        std::vector<std::string> SplitPipe(const char* s);

        template <class S>
        RowT<S> Finish(RowT<S> r)
        {
            const std::string p = r.path;
            const size_t dot = p.find('.');
            r.section = p.substr(0, dot);
            r.key     = dot == std::string::npos ? std::string() : p.substr(dot + 1);
            r.labels  = SplitPipe(r.spec.labels);
            for (const std::string& v : SplitPipe(r.spec.values)) r.values.push_back(std::stoi(v));
            return r;
        }
        // One maker per kind: the member pointer's type has to match the kind,
        // or the row does not compile.
        template <class S> RowT<S> MakeBool(const char* p, bool S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::Bool;   r.mb = m; r.spec = s; return Finish(r); }
        template <class S> RowT<S> MakeInt(const char* p, int S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::Int;    r.mi = m; r.spec = s; return Finish(r); }
        template <class S> RowT<S> MakeEnum(const char* p, int S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::Enum;   r.mi = m; r.spec = s; return Finish(r); }
        template <class S> RowT<S> MakeChoice(const char* p, int S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::Choice; r.mi = m; r.spec = s; return Finish(r); }
        template <class S> RowT<S> MakeFloat(const char* p, float S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::Float;  r.mf = m; r.spec = s; return Finish(r); }
        template <class S> RowT<S> MakeString(const char* p, std::string S::* m, RowSpecT<S> s)
        { RowT<S> r; r.path = p; r.kind = Kind::String; r.ms = m; r.spec = s; return Finish(r); }

        // The checks every table gets when it is built: a row's phase against
        // its hooks (None: no hooks; Boot: a capture and no apply; Applied: an
        // apply and no capture; the rest both - a row applied and never
        // captured makes every Save write back what was loaded instead of what
        // is live, or a value loads and never reaches its owner).
        void CheckHooks(const char* path, Phase ph, bool applied, bool captured);
    }
}
