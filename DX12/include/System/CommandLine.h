#pragma once
// System/CommandLine.h - process command-line access for engine code.
//
// Lives in EngineCore, which is compiled once for Editor.exe and Game.exe and so
// cannot see WITH_EDITOR. It parses GetCommandLineW() itself, so no argv has to be
// threaded down from wWinMain: any TU can ask, and the answer is the same everywhere.
//
// Lifetime: Initialize() runs once from main.cpp BEFORE Logger::Initialize (the
// log's destination depends on `-artifacts=`), so parsing must not log. Every
// accessor is const and safe from any thread after that.
//
// Syntax:
//   -flag                 a switch; Has("-flag") is true
//   -key=value            a value; Get("-key") returns "value"
//   --flag / --key=value  identical (a leading "--" is normalised to "-")
// Keys match case-insensitively ("-Frames=10" == "-frames=10"): they are typed by
// hand and by scripts on a case-insensitive OS. Values keep their case (paths).
// A value keeps everything after the FIRST '=', so `-settings=C:/a=b/x.json` works.
// CommandLineToArgvW handles quoting: `-scene="My Scene"` arrives as one argv entry.
//
// Unread flags: there is no table of known flags, so a misspelled one would be
// ignored and the run would pass having tested nothing. Every lookup (Has / Get* /
// GetAll) marks its key read, and RunLedger::Finalize publishes the keys given but
// never read as run.json "unreadFlags". The signal is "not read in THIS run": a
// flag only some code path reads is reported when the run did not take that path.
// Iterating Args() reads nothing; read a repeatable flag with GetAll.
// Read an opt-in that forces a default-on feature before the default:
// `cl.Has("-x") || kDefault`, not `kDefault || cl.Has("-x")` - the latter never
// asks once the default is on, so every pack naming the flag reports it unread.

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace Sys
{
    class CommandLine
    {
    public:
        // Parse GetCommandLineW(). Idempotent: a second call is ignored, so a
        // host that calls it defensively cannot clobber the first parse.
        //
        // MUST NOT LOG. It runs before Logger::Initialize (the logger's own path
        // comes from `-artifacts=`), and the async logger drops - or worse,
        // resolves a path from - anything enqueued before its file is open.
        // Parse problems are recorded and replayed by ReplayDiagnostics() once
        // the logger is up.
        static void Initialize();

        // Emit any warning collected during Initialize (unknown-looking tokens,
        // a malformed value), and an ERROR for each removed flag the command line still
        // passes (CommandLine.cpp's retired table). Call once, immediately AFTER
        // Logger::Initialize.
        static void ReplayDiagnostics();

        static const CommandLine& Get();

        // True if the switch is present in any form (`-x` or `-x=...`).
        bool Has(const char* key) const;

        // Value for `-key=value`. Returns @p fallback when the key is absent, or
        // when it was given with no '=' (a bare `-key` has no value).
        std::string Get(const char* key, const std::string& fallback = {}) const;

        // Numeric readers. A malformed value returns @p fallback and is reported
        // by ReplayDiagnostics: a typo'd `-frames=abc` must not silently become
        // "run forever".
        int    GetInt  (const char* key, int    fallback) const;
        float  GetFloat(const char* key, float  fallback) const;

        // Every value of a REPEATABLE `-key=value`, in argv order. The parsed
        // table keeps only the LAST occurrence of a key, so a flag that may be
        // given several times is read here. Occurrences without '=' are not
        // values and are skipped. Counts as reading the key.
        std::vector<std::string> GetAll(const char* key) const;

        // Keys given on the command line that no Has / Get* / GetAll has asked
        // about yet, in argv order, each with one leading '-'.
        std::vector<std::string> UnreadKeys() const;

        // Has / Get / GetInt / GetFloat WITHOUT marking the key read, plus the
        // mark on its own (MarkRead). The settings registry reads a pass
        // setting's flag at boot exactly as the pass would - the same parse,
        // fallback and warning - and marks it read only when the pass first asks
        // for the setting, so "unreadFlags" still means "nothing this run asked".
        bool        Peek     (const char* key) const;
        std::string PeekGet  (const char* key, const std::string& fallback = {}) const;
        int         PeekInt  (const char* key, int   fallback) const;
        float       PeekFloat(const char* key, float fallback) const;
        void        MarkRead (const char* key) const;
        // PeekInt / PeekFloat that tell a value from a fallback (for
        // kSettingLegacyValueOnly rows): true, and @p out set, only for a
        // well-formed `-key=<number>`; a malformed one logs the readers'
        // warning. Not marked read.
        bool        PeekIntValue  (const char* key, int&   out) const;
        bool        PeekFloatValue(const char* key, float& out) const;

        // The raw argv (minus argv[0]), for diagnostics and for echoing the
        // invocation into run.json so a failed run carries its own repro.
        // Iterating it does not mark any flag as read (see the file header).
        const std::vector<std::string>& Args() const { return m_args; }

        // The full command line as one UTF-8 string, for the log banner.
        std::string ToString() const;

    private:
        struct Entry
        {
            std::string key;        // normalised
            std::string value;      // empty for a bare switch
            bool        hasValue = false;
        };

        CommandLine() = default;

        // The single instance, mutable. A private static MEMBER (rather than a
        // free function in the .cpp) because the constructor is private: only a
        // member can construct the function-local static.
        static CommandLine& Mutable();

        // Normalised lookup key: leading '-'/'--' stripped, lower-cased.
        static std::string NormalizeKey(const std::string& raw);

        // Finds the entry for @p key and marks it read.
        const Entry* Find(const char* key) const;
        // The same lookup without the mark (the Peek family, MarkRead). Compares the caller's
        // spelling against the keys normalised at parse time; allocates nothing.
        const Entry* Lookup(const char* key) const;
        // What Get / GetInt / GetFloat make of an entry (nullptr: absent) -
        // shared by the marking readers and their Peek twins. TryInt / TryFloat:
        // false for an absent key, a bare switch or a malformed number (the last
        // logs the warning).
        static std::string ValueOr(const Entry* e, const std::string& fallback);
        static int         IntOr  (const Entry* e, int   fallback);
        static float       FloatOr(const Entry* e, float fallback);
        static bool        TryInt  (const Entry* e, int&   out);
        static bool        TryFloat(const Entry* e, float& out);

        std::vector<Entry>       m_entries;
        std::vector<std::string> m_args;          // verbatim, minus argv[0]
        std::vector<std::string> m_diagnostics;   // replayed once the log is up
        // One flag per m_entries slot, allocated once parsing is done (the
        // entries never move after that). Set from any thread by Find.
        std::unique_ptr<std::atomic<bool>[]> m_read;
        bool                     m_initialized = false;
    };
}
