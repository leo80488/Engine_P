#include "Harness/GameFlowDump.h"

// Self-guard as well as the vcxproj's ExcludedFromBuild, for the same reason
// ControlChannel.cpp carries one: this TU is gated on EnableHarness while its
// second consumer is gated on EnableControl, and a hand override that sets one
// property without the other should still compile.
#ifdef WITH_HARNESS

#include "Gameplay/GameFlowState.h"
#include "ECS/ISystem.h"
#include "ECS/SystemRegistry.h"
#include "Harness/RunLedger.h"
#include "Scripting/ScriptSystem.h"
#include "System/PathUtil.h"
#include "System/Log.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Harness
{
namespace
{
    using Flow = Gameplay::GameFlowStateMachine;

    // A local copy, deliberately, matching RunLedger.cpp and WorldStateDump.cpp:
    // each harness writer owns its escaper in an anonymous namespace. State names
    // come from Lua and can carry anything a Lua string can; an unescaped one makes
    // the whole object unparseable exactly when a caller most needs to read it.
    std::string JsonEscape(const std::string& in)
    {
        std::string out;
        out.reserve(in.size() + 8);
        for (char c : in)
        {
            switch (c)
            {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04X",
                                      static_cast<unsigned>(static_cast<unsigned char>(c)));
                        out += esc;
                    }
                    else out += c;
                    break;
            }
        }
        return out;
    }

    // The two layouts share one emitter so the file and the pipe reply cannot
    // drift into different field names (see the header). `pretty` controls
    // whitespace and NOTHING else.
    struct Writer
    {
        std::string j;
        bool        pretty = true;

        void Break(int indent)
        {
            if (!pretty) return;
            j += '\n';
            j.append(static_cast<std::size_t>(indent) * 2, ' ');
        }
        void Key(const char* name, int indent)
        {
            Break(indent);
            j += '"'; j += name; j += pretty ? "\": " : "\":";
        }
        void Str(const std::string& s) { j += '"'; j += JsonEscape(s); j += '"'; }
        void Bool(bool b)              { j += b ? "true" : "false"; }
        void Num(std::size_t n)        { j += std::to_string(n); }

        void StringArray(const std::vector<std::string>& items)
        {
            j += '[';
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                if (i) j += pretty ? ", " : ",";
                Str(items[i]);
            }
            j += ']';
        }
    };
}

std::string BuildGameFlowJson(const Flow& flow,
                              const SystemRegistry& systems,
                              const ScriptSystem& scripts,
                              const GameFlowJsonOptions& options)
{
    Flow::FlowSnapshot snap;
    flow.FillSnapshot(snap);

    const std::vector<Flow::FlowTransitionRecord> history = flow.History();
    const std::vector<Flow::FlowDiagnostic>&      findings = flow.Diagnostics();
    const std::vector<std::pair<std::string, std::string>> luaAssignments =
        scripts.GetSystemSetAssignments();

    // Keep the NEWEST records when capped: a truncated tail would throw away
    // the transition that ended the run, which is the one being asked about.
    std::size_t historyFirst = 0;
    if (options.historyLimit != 0 && history.size() > options.historyLimit)
        historyFirst = history.size() - options.historyLimit;

    Writer w;
    w.pretty = options.pretty;
    w.j.reserve(16 * 1024);
    w.j += '{';

    w.Key("schema", 1);            w.j += '1'; w.j += ',';
    w.Key("current", 1);           w.Str(snap.current);          w.j += ',';
    w.Key("pending", 1);           w.Str(snap.pending);          w.j += ',';
    w.Key("inTransition", 1);      w.Bool(snap.inTransition);    w.j += ',';
    // passThrough == "no flow has started, so every set gate answers active".
    // Emitted next to the set table on purpose: a reader that printed `active`
    // verbatim without this flag would be reporting a gate that is not gating.
    w.Key("passThrough", 1);       w.Bool(snap.passThrough);     w.j += ',';
    w.Key("definitionsValid", 1);  w.Bool(snap.definitionsValid); w.j += ',';
    w.Key("path", 1);              w.StringArray(snap.currentPath); w.j += ',';

    // ---- states ----
    w.Key("states", 1);
    w.j += '[';
    for (std::size_t i = 0; i < snap.states.size(); ++i)
    {
        const Flow::FlowStateRow& row = snap.states[i];
        if (i) w.j += ',';
        w.Break(2);
        w.j += '{';
        w.Key("name", 3);          w.Str(row.name);        w.j += ',';
        w.Key("parent", 3);        w.Str(row.parent);      w.j += ',';
        w.Key("sets", 3);          w.StringArray(row.sets);     w.j += ',';
        w.Key("suspends", 3);      w.StringArray(row.suspends); w.j += ',';
        w.Key("valid", 3);         w.Bool(row.valid);         w.j += ',';
        w.Key("onCurrentPath", 3); w.Bool(row.onCurrentPath); w.j += ',';
        w.Key("isCurrent", 3);     w.Bool(row.isCurrent);     w.j += ',';
        w.Key("depth", 3);         w.j += std::to_string(row.depth);
        w.Break(2);
        w.j += '}';
    }
    w.Break(1);
    w.j += "],";

    // ---- sets, with their membership resolved ----
    // The membership is the half DebugDump() carries and FillSnapshot does not, and
    // it answers "the state is right, so why did nothing run" - a set with no
    // members is a typo between the C++ registration and the Lua state definition,
    // and it fails silently by design (SystemSet.h).
    w.Key("sets", 1);
    w.j += '[';
    for (std::size_t i = 0; i < snap.sets.size(); ++i)
    {
        const Flow::FlowSetRow& row = snap.sets[i];
        if (i) w.j += ',';
        w.Break(2);
        w.j += '{';
        w.Key("name", 3);       w.Str(row.name);       w.j += ',';
        w.Key("active", 3);     w.Bool(row.active);    w.j += ',';
        w.Key("referenced", 3); w.Bool(row.referenced); w.j += ',';

        // The id IS the FNV hash of the name (StringHash), so this reproduces exactly
        // the id AssignToSet stored - no reverse map needed, and no chance of the two
        // disagreeing.
        const SystemSetId setId{ std::string_view(row.name) };

        w.Key("members", 3);
        w.j += '{';
        w.Key("cpp", 4);
        w.j += '[';
        {
            // Tick order, not alphabetical: "which of these runs first" is a
            // question the reader has next, and the order is already stable
            // (phase order, then registration order within the phase).
            const std::vector<const ISystem*> cppSystems =
                systems.GetSystemsInSet(setId);
            for (std::size_t s = 0; s < cppSystems.size(); ++s)
            {
                if (s) w.j += w.pretty ? ", " : ",";
                const char* name = cppSystems[s]->GetName();
                w.Str(name ? name : "?");
            }
        }
        w.j += "],";
        w.Key("lua", 4);
        w.j += '[';
        {
            bool first = true;
            for (const auto& [scriptName, scriptSet] : luaAssignments)
            {
                if (scriptSet != row.name) continue;
                if (!first) w.j += w.pretty ? ", " : ",";
                first = false;
                w.Str(scriptName);
            }
        }
        w.j += ']';
        w.Break(3);
        w.j += '}';
        w.Break(2);
        w.j += '}';
    }
    w.Break(1);
    w.j += "],";

    // ---- diagnostics ----
    w.Key("diagnostics", 1);
    w.j += '[';
    for (std::size_t i = 0; i < findings.size(); ++i)
    {
        if (i) w.j += ',';
        w.Break(2);
        w.j += '{';
        w.Key("severity", 3);
        w.Str(findings[i].severity == Flow::FlowDiagnostic::Severity::Error
                  ? "error" : "warning");
        w.j += ',';
        w.Key("state", 3);   w.Str(findings[i].state);   w.j += ',';
        w.Key("message", 3); w.Str(findings[i].message);
        w.Break(2);
        w.j += '}';
    }
    w.Break(1);
    w.j += "],";

    // ---- history ----
    // Oldest -> newest, REJECTIONS INCLUDED. A dropped request leaves no other
    // trace: PendingOccupied means an earlier request that frame won, and without
    // this record the second one never happened as far as any observer can tell.
    w.Key("historyCapacity", 1);
    w.Num(Flow::kHistoryCapacity);
    w.j += ',';
    // True when the ring is full, i.e. older records have already been
    // overwritten and any count taken from `history` is a LOWER BOUND.
    w.Key("historySaturated", 1);
    w.Bool(history.size() >= Flow::kHistoryCapacity);
    w.j += ',';
    w.Key("historyTruncated", 1);
    w.Bool(historyFirst != 0);
    w.j += ',';
    w.Key("history", 1);
    w.j += '[';
    for (std::size_t i = historyFirst; i < history.size(); ++i)
    {
        const Flow::FlowTransitionRecord& rec = history[i];
        if (i != historyFirst) w.j += ',';
        w.Break(2);
        w.j += '{';
        w.Key("frame", 3);
        w.j += std::to_string(static_cast<unsigned long long>(rec.frame));
        w.j += ',';
        w.Key("from", 3); w.Str(rec.from); w.j += ',';
        w.Key("to", 3);   w.Str(rec.to);   w.j += ',';
        w.Key("ms", 3);
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f",
                          static_cast<double>(rec.elapsedMs));
            w.j += buf;
        }
        w.j += ',';
        // The SwitchResult spelling the Lua surface and ToString() use ("ok",
        // "pending-occupied", ...), not the enumerator name - one vocabulary for the
        // whole tooling surface.
        w.Key("kind", 3); w.Str(Gameplay::ToString(rec.kind)); w.j += ',';
        w.Key("repeats", 3);
        w.j += std::to_string(static_cast<unsigned long>(rec.repeats));
        w.Break(2);
        w.j += '}';
    }
    w.Break(1);
    w.j += ']';

    w.Break(0);
    w.j += '}';
    if (w.pretty) w.j += '\n';
    return std::move(w.j);
}

bool WriteGameFlowDump(const Flow& flow,
                       const SystemRegistry& systems,
                       const ScriptSystem& scripts,
                       const std::string& utf8Path)
{
    const std::string j = BuildGameFlowJson(flow, systems, scripts, {});

    // WriteFileBytes creates the parent directory and returns false rather than
    // throwing - the same whole-file write WorldStateDump uses, for the same
    // never-throw reason.
    if (!Sys::WriteFileBytes(utf8Path, j.data(), j.size()))
    {
        LOG_ERROR("GameFlowDump: could not write '%s'", utf8Path.c_str());
        return false;
    }

    LOG_INFO("GameFlowDump: state '%s' -> %s",
             flow.CurrentName().empty() ? "<none>" : flow.CurrentName().c_str(),
             utf8Path.c_str());
    return true;
}

namespace
{
    // THE REPLY BUDGET. ControlChannel.cpp sizes the pipe's out-buffer at
    // kOutBuffer = 64 KB, and ControlQuery::Limits::maxTotalLen caps a query reply
    // at 48 KB. A write larger than the out-buffer does not fail, it BLOCKS until
    // the client drains - and WriteResponse gives up after 5 s - so exceeding it
    // turns a good answer into a dead connection. Same 48 KB here, for the same
    // reason.
    constexpr std::size_t kPipeReplyBudget = 48 * 1024;

    // 64 records x two state names is a few KB: fine for a file, needless weight
    // on every `flow` poll. The last 16 cover the transitions an interactive caller
    // asks about; `flow history` lifts the cap when they want the whole ring.
    constexpr std::size_t kPipeHistoryLimit = 16;
}

std::string ServeFlowVerb(Flow& flow,
                          const SystemRegistry& systems,
                          const ScriptSystem& scripts,
                          const std::string& payload)
{
    // Sub-verb parsing is LOCAL, because there is no shared grammar to reuse:
    // ExecuteOnMainThread splits the command on the FIRST whitespace only, so
    // `flow switch Menu` arrives as verb="flow", payload="switch Menu". The other
    // multi-word verbs (`frames N`, `quit 3`) parse their own payload the same way.
    const std::size_t sp = payload.find_first_of(" \t");
    const std::string action = payload.substr(0, sp);
    std::string arg = (sp == std::string::npos) ? std::string()
                                                : payload.substr(sp + 1);
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) arg.pop_back();
    while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t'))
        arg.erase(arg.begin());

    if (action == "switch")
    {
        if (arg.empty())
            return "{\"ok\":false,\"verb\":\"flow\",\"action\":\"switch\","
                   "\"error\":\"switch_needs_a_state_name\"}";

        // TryRequestSwitch, never RequestSwitch: the REASON is the whole value of
        // doing this over the pipe. A bool reports Undefined and PendingOccupied
        // identically, and those want opposite responses - fix the name, versus wait
        // one frame and retry.
        const Gameplay::SwitchResult result = flow.TryRequestSwitch(arg);
        const bool accepted = result == Gameplay::SwitchResult::Accepted;

        std::string out = "{\"ok\":";
        out += accepted ? "true" : "false";
        out += ",\"verb\":\"flow\",\"action\":\"switch\",\"state\":\"";
        out += JsonEscape(arg);
        out += "\",\"result\":\"";
        out += JsonEscape(Gameplay::ToString(result));
        out += "\",\"current\":\"";
        out += JsonEscape(flow.CurrentName());
        // ACCEPTED IS NOT APPLIED. The request occupies the pending slot and
        // GameFlowResolveSystem consumes it at the next Input phase, so a caller that
        // reads `current` straight back gets the OLD state and concludes nothing
        // happened. Say so in the reply: follow with `frames 1`.
        out += "\",\"note\":\"accepted means QUEUED; it resolves at the next "
               "Input phase — send 'frames 1' before reading the result\"}";
        return out;
    }

    if (!action.empty() && action != "state" && action != "history")
    {
        std::string out = "{\"ok\":false,\"verb\":\"flow\",\"error\":"
                          "\"unknown_flow_action\",\"action\":\"";
        out += JsonEscape(action);
        out += "\",\"actions\":[\"state\",\"history\",\"switch <name>\"]}";
        return out;
    }

    GameFlowJsonOptions options;
    options.pretty = false;                 // parsed, never read, and it is 64 KB
    options.historyLimit = (action == "history") ? 0 : kPipeHistoryLimit;

    std::string body = BuildGameFlowJson(flow, systems, scripts, options);
    if (body.size() > kPipeReplyBudget)
    {
        // A project with hundreds of states can outgrow the pipe no matter what
        // the history cap is. Refusing with the reason and the working
        // alternative beats a truncated object that parses as a smaller,
        // plausible, wrong answer.
        char out[320];
        std::snprintf(out, sizeof(out),
            "{\"ok\":false,\"verb\":\"flow\",\"error\":\"reply_too_large\","
            "\"bytes\":%zu,\"budget\":%zu,"
            "\"hint\":\"relaunch with -dumpflow=<path>, which has no size cap\"}",
            body.size(), kPipeReplyBudget);
        return out;
    }

    std::string reply = "{\"ok\":true,\"verb\":\"flow\",\"flow\":";
    reply += body;
    reply += '}';
    return reply;
}

void NoteGameFlowToLedger(const Flow& flow)
{
    Flow::FlowSnapshot snap;
    flow.FillSnapshot(snap);

    std::uint32_t invalid = 0;
    for (const Flow::FlowStateRow& row : snap.states)
        if (!row.valid) ++invalid;

    // COUNTED OVER THE RETAINED RING, so both are lower bounds on a run with more
    // than kHistoryCapacity requests (see RunLedger::NoteGameFlow). Acceptable:
    // -dumpflow writes the records themselves (with historySaturated), and run.json
    // asks "did anything get rejected at all", which a lower bound answers correctly
    // whenever the answer is yes.
    std::uint32_t transitions = 0, rejected = 0;
    for (const Flow::FlowTransitionRecord& rec : flow.History())
    {
        if (rec.kind == Gameplay::SwitchResult::Accepted) ++transitions;
        // repeats folds consecutive identical rejections into one record, so a
        // per-frame retry loop counts as the N attempts it actually was rather
        // than as one.
        else rejected += 1 + rec.repeats;
    }

    RunLedger::Get().NoteGameFlow(
        snap.current.empty() ? std::string("<none>") : snap.current,
        transitions, rejected, invalid);
}

}  // namespace Harness

#endif  // WITH_HARNESS
