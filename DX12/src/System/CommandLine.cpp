#include "System/CommandLine.h"
#include "System/Check.h"
#include "System/PathUtil.h"
#include "System/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

#pragma comment(lib, "shell32.lib")

namespace Sys
{
namespace
{
// Flags that were removed but may still sit on a saved command line. One row per flag, kept
// for one version, then deleted: the hint names what to use instead.
struct RetiredFlag
{
    const char* key;
    const char* hint;
};

constexpr const char* kOnlyPath = "the A/B or fallback arm it selected is gone; the default path is the only one";
constexpr const char* kNoOracle = "the oracle it enabled is gone; the default path is unchanged";

constexpr RetiredFlag kRetiredFlags[] = {
    { "lifecycle",            "the swap chain always rebuilds at the frame boundary (latched)" },
    { "jobsystem",            "the job system always uses the sharded queues" },
    { "animsamplelegacy",     kOnlyPath },
    { "frameslotlegacy",      kOnlyPath },
    { "batchplanjobs",        kOnlyPath },
    { "materialcpuab",        kOnlyPath },
    { "materialcpuserial",    kOnlyPath },
    { "materialearlyab",      kOnlyPath },
    { "materialtailab",       kOnlyPath },
    { "materialtailoff",      kOnlyPath },
    { "planoverlapab",        kOnlyPath },
    { "planoverlapoff",       kOnlyPath },
    { "planprefixab",         kOnlyPath },
    { "planwideab",           kOnlyPath },
    { "slotmemoab",           kOnlyPath },
    { "slotmemooff",          kOnlyPath },
    { "slotmemoverify",       kNoOracle },
    { "sortmergeserial",      kOnlyPath },
    { "sortverify",           kNoOracle },
    { "classifycacheab",      kOnlyPath },
    { "classifycacheoff",     kOnlyPath },
    { "laneassembleoff",      "it was a no-op spelling of the default; -laneassemble is the opt-in" },
    { "matsnapdedupverify",   kNoOracle },
    { "emitgraphab",          kOnlyPath },
    { "emitgraphoff",         kOnlyPath },
    { "instancepublishab",    kOnlyPath },
    { "instancepublishjobs",  kOnlyPath },
    { "instancepublishoff",   kOnlyPath },
    { "stageorderlegacy",     kOnlyPath },
    { "stageorderverify",     kNoOracle },
    { "stageshadowlegacy",    kOnlyPath },
    { "stageshadowverify",    kNoOracle },
    { "boneaabbprepab",       kOnlyPath },
    { "boneaabbprepfresh",    kOnlyPath },
    { "closejobsab",          kOnlyPath },
    { "closejobsoff",         kOnlyPath },
    { "spotreduceab",         kOnlyPath },
    { "spotreduceserial",     kOnlyPath },
    { "skinrecordab",         kOnlyPath },
    { "skinextentab",         kOnlyPath },
    { "skinextentoff",        kOnlyPath },
    { "gathermemoryab",       kOnlyPath },
    { "gathermemoryoff",      kOnlyPath },
    { "gathermemoryverify",   kNoOracle },
    { "chainpipelineab",      kOnlyPath },
    { "chainprepab",          kOnlyPath },
    { "ddgi_legacy_schedule", kOnlyPath },
    { "ddgi_schedule_log",    "the [DDGISCHEDULE] log line is gone" },
    { "ddgimirrorstats",      "-ddgimirrorverify logs the same [DDGIMIRROR] tally" },
    { "asrecordab",           kOnlyPath },
    { "asrecordadaptab",      kOnlyPath },
    { "asrecordadaptive",     "the scene-AS records always use the adaptive chunk plan" },
    { "asrecordfixed",        kOnlyPath },
    { "asrecordjobs",         "the job-split preset (-jobdefaultsoff turns it off) decides whether the records use jobs" },
    { "asrecordserial",       kOnlyPath },
    { "asrecordstream",       kOnlyPath },
    { "asresolvejobs",        "the job-split preset decides whether the BLAS addresses resolve in jobs" },
    { "asresolveoff",         kOnlyPath },
    { "asstreamab",           kOnlyPath },
    { "asstreamcoarse",       kOnlyPath },
    { "asgrainab",            kOnlyPath },
    { "asvaab",               kOnlyPath },
    { "asvamemo",             kOnlyPath },
    { "asvaverify",           kNoOracle },
    { "di_nrd_fp16_pool",     "DI's NRD illumination pools are always FP32" },
    { "di_nrd_fp32_pool",     "DI's NRD illumination pools are always FP32" },
    { "gi_nrd_fp16_pool",     "GI's NRD illumination pools are always FP32" },
    { "gi_nrd_fp32_pool",     "GI's NRD illumination pools are always FP32" },
};
}  // namespace

// The single instance. A function-local static inside a MEMBER function: the
// constructor is private, so a free helper in an anonymous namespace could not
// construct it. Initialize() runs from wWinMain before anything else, but Get()
// can be reached from another TU's static initializer, and a function-local
// static is ordered correctly where a namespace-scope object is not.
CommandLine& CommandLine::Mutable()
{
    static CommandLine s_cl;
    return s_cl;
}

const CommandLine& CommandLine::Get()
{
    return Mutable();
}

std::string CommandLine::NormalizeKey(const std::string& raw)
{
    std::size_t i = 0;
    while (i < raw.size() && raw[i] == '-') ++i;   // "-x" and "--x" are the same key
    std::string out = raw.substr(i);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void CommandLine::Initialize()
{
    CommandLine& self = Mutable();
    if (self.m_initialized) return;               // idempotent by contract
    self.m_initialized = true;

    int      argc = 0;
    LPWSTR*  argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv)
    {
        // Nothing we can do and nowhere to say it yet; record for replay.
        self.m_diagnostics.push_back(
            "CommandLine: CommandLineToArgvW failed; no arguments are visible "
            "to this process.");
        return;
    }

    // argv[0] is the exe path - kept out of Args() so callers can iterate the
    // user's actual arguments without special-casing the first one.
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = Sys::WideToUtf8(argv[i]);
        self.m_args.push_back(arg);

        if (arg.empty() || arg[0] != '-')
        {
            // A positional token. The engine takes none; recording it as a
            // diagnostic beats ignoring it, because the usual cause is a quoting
            // mistake ("-scene=My Scene" without quotes) that would otherwise
            // look like "the flag did nothing".
            self.m_diagnostics.push_back(
                "CommandLine: ignoring positional argument '" + arg +
                "' (the engine takes no positional arguments; if this was meant "
                "to be part of a value, quote the whole switch).");
            continue;
        }

        Entry e;
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos)
        {
            e.key = NormalizeKey(arg);
        }
        else
        {
            e.key      = NormalizeKey(arg.substr(0, eq));
            e.value    = arg.substr(eq + 1);       // keeps any further '='
            e.hasValue = true;
        }

        if (e.key.empty())
        {
            self.m_diagnostics.push_back(
                "CommandLine: ignoring malformed argument '" + arg + "'.");
            continue;
        }

        // Last occurrence wins, which is what a shell user expects when they
        // append an override to a saved command line.
        auto it = std::find_if(self.m_entries.begin(), self.m_entries.end(),
                               [&](const Entry& x) { return x.key == e.key; });
        if (it != self.m_entries.end()) *it = e;
        else                            self.m_entries.push_back(e);
    }

    ::LocalFree(argv);

    // The entries are final from here on, so one read mark per slot can live
    // in a fixed array (value-initialised: nothing read yet).
    self.m_read = std::make_unique<std::atomic<bool>[]>(self.m_entries.size());
}

void CommandLine::ReplayDiagnostics()
{
    const CommandLine& self = Mutable();
    for (const std::string& d : self.m_diagnostics)
        LOG_WARNING("%s", d.c_str());

    // A stale command line gets told once, loudly: a pack that passes a removed flag would
    // otherwise run the default and look as if the flag were honoured.
    // -retired_flags_off (harness) skips the report: the reverse arm of the pack that pins it.
    bool report = true;
#if WITH_HARNESS
    report = !self.Has("-retired_flags_off");
#endif
    for (const RetiredFlag& r : kRetiredFlags)
        if (report) ENGINE_CHECK(!self.Has(r.key), "-%s was removed: %s", r.key, r.hint);
}

const CommandLine::Entry* CommandLine::Find(const char* key) const
{
    const Entry* e = Lookup(key);
    if (e && m_read) m_read[static_cast<std::size_t>(e - m_entries.data())].store(true, std::memory_order_relaxed);
    return e;
}

const CommandLine::Entry* CommandLine::Lookup(const char* key) const
{
    if (!key) return nullptr;
    // The stored keys were normalised once at parse time, so a query compares the caller's
    // spelling against them in place: no std::string per lookup (this runs from per-frame code).
    std::string_view wanted(key);
    while (!wanted.empty() && wanted.front() == '-') wanted.remove_prefix(1);
    for (const Entry& e : m_entries)
    {
        if (e.key.size() != wanted.size()) continue;
        std::size_t i = 0;
        while (i < wanted.size() &&
               e.key[i] == static_cast<char>(std::tolower(static_cast<unsigned char>(wanted[i]))))
            ++i;
        if (i == wanted.size()) return &e;
    }
    return nullptr;
}

void CommandLine::MarkRead(const char* key) const
{
    (void)Find(key);
}

bool        CommandLine::Peek     (const char* key) const                                { return Lookup(key) != nullptr; }
std::string CommandLine::PeekGet  (const char* key, const std::string& fallback) const   { return ValueOr(Lookup(key), fallback); }
int         CommandLine::PeekInt  (const char* key, int fallback) const                  { return IntOr(Lookup(key), fallback); }
float       CommandLine::PeekFloat(const char* key, float fallback) const                { return FloatOr(Lookup(key), fallback); }
bool        CommandLine::PeekIntValue  (const char* key, int& out) const                 { return TryInt(Lookup(key), out); }
bool        CommandLine::PeekFloatValue(const char* key, float& out) const               { return TryFloat(Lookup(key), out); }

std::vector<std::string> CommandLine::GetAll(const char* key) const
{
    std::vector<std::string> out;
    if (!Find(key)) return out;   // marks the key read; absent = nothing to collect
    const std::string k = NormalizeKey(key);
    for (const std::string& arg : m_args)
    {
        if (arg.empty() || arg[0] != '-') continue;
        const std::size_t eq = arg.find('=');
        if (eq != std::string::npos && NormalizeKey(arg.substr(0, eq)) == k)
            out.push_back(arg.substr(eq + 1));
    }
    return out;
}

std::vector<std::string> CommandLine::UnreadKeys() const
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < m_entries.size(); ++i)
        if (!m_read || !m_read[i].load(std::memory_order_relaxed))
            out.push_back("-" + m_entries[i].key);
    return out;
}

bool CommandLine::Has(const char* key) const
{
    return Find(key) != nullptr;
}

std::string CommandLine::Get(const char* key, const std::string& fallback) const
{
    return ValueOr(Find(key), fallback);
}

int CommandLine::GetInt(const char* key, int fallback) const
{
    return IntOr(Find(key), fallback);
}

float CommandLine::GetFloat(const char* key, float fallback) const
{
    return FloatOr(Find(key), fallback);
}

std::string CommandLine::ValueOr(const Entry* e, const std::string& fallback)
{
    return (e && e->hasValue) ? e->value : fallback;
}

int CommandLine::IntOr(const Entry* e, int fallback)
{
    int v = 0;
    return TryInt(e, v) ? v : fallback;
}

float CommandLine::FloatOr(const Entry* e, float fallback)
{
    float v = 0.f;
    return TryFloat(e, v) ? v : fallback;
}

bool CommandLine::TryInt(const Entry* e, int& out)
{
    if (!e || !e->hasValue) return false;

    // strtol, not atoi: atoi cannot tell "0" from "abc", and a silently
    // zeroed -frames would mean "quit before the first frame" - a failure that
    // looks exactly like a working harness producing an empty artifact set.
    char*         end = nullptr;
    const long    v   = std::strtol(e->value.c_str(), &end, 10);
    if (end == e->value.c_str() || (end && *end != '\0'))
    {
        LOG_WARNING("CommandLine: '-%s=%s' is not an integer; using the default instead.",
                    e->key.c_str(), e->value.c_str());
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

bool CommandLine::TryFloat(const Entry* e, float& out)
{
    if (!e || !e->hasValue) return false;

    char*        end = nullptr;
    const double v   = std::strtod(e->value.c_str(), &end);
    if (end == e->value.c_str() || (end && *end != '\0'))
    {
        LOG_WARNING("CommandLine: '-%s=%s' is not a number; using the default instead.",
                    e->key.c_str(), e->value.c_str());
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

std::string CommandLine::ToString() const
{
    std::string out;
    for (std::size_t i = 0; i < m_args.size(); ++i)
    {
        if (i) out += ' ';
        out += m_args[i];
    }
    return out;
}

}  // namespace Sys
