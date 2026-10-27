#pragma once
// Harness/GameFlowDump.h - the whole game-flow state machine as one JSON object.
//
// GameFlowStateMachine::DebugDump() writes prose for a human. An automated
// caller asks "did the run end in the state it should, and if not, which
// request was dropped and why" - and the REJECTIONS are what a log makes
// hardest to reconstruct (a request refused with PendingOccupied leaves no
// trace of the frame it was made on). This turns FillSnapshot, Diagnostics and
// History into the stable, diffable file shape run.json and worldstate.json
// use.
//
// ONE BUILDER, TWO CONSUMERS, on purpose: -dumpflow=<path> writes the file and
// the control channel's `flow` verb sends the same bytes down the pipe. Both go
// through BuildGameFlowJson: a dump and a live query that disagreed on field
// names would let an agent parse one and silently mis-read the other.
//
// THE HISTORY CAP: the ring holds kHistoryCapacity (64) records of two state
// names each - a few KB, fine for a file but needless weight on every `flow`
// poll, whose whole reply must fit the pipe's 48 KB budget (64 KB out-buffer).
// `flow` sends the last 16 (`flow history` all of them); the file takes all.
//
// DETERMINISM: everything emitted is already sorted by its producer -
// FillSnapshot sorts states and sets by name, GetSystemSetAssignments sorts,
// and GetSystemsInSet walks phases in tick order and registration order within
// a phase. Do not re-order anything here: two dumps of the same flow are
// byte-identical, so a diff means a real change. (SystemRegistry::m_systemSets
// is keyed by ISystem POINTER, but GetSystemsInSet iterates the phase vectors
// and only probes that map, so its output does not inherit address order.)
//
// GAMEFLOWPROBE: the three objects the dump needs live on App, and neither the
// PostRender capture system nor the pipe's main-thread callback can reach them
// through their arguments. Same answer as FrameProbe: a tiny singleton App
// binds once, not a wider FrameContext (which every system is passed). Defined
// INLINE and OUTSIDE every harness guard, like FrameProbe, so App's single
// Bind() line compiles in Retail without an #ifdef.

#include <cstddef>
#include <string>

class ScriptSystem;
class SystemRegistry;

namespace Gameplay { class GameFlowStateMachine; }

namespace Harness
{
    /// What App knows and the harness cannot reach on its own. Bound once, in
    /// App::RegisterTickSystems right after FinalizeDefinitions() - the first moment
    /// all three exist and the state machine is validated.
    ///
    /// Non-const on the flow deliberately: `flow switch <name>` needs
    /// TryRequestSwitch. The dump entry points below take const references so the
    /// read-only path cannot drift into the mutating one by accident.
    struct GameFlowProbe
    {
        Gameplay::GameFlowStateMachine* flow    = nullptr;
        const SystemRegistry*           systems = nullptr;
        const ScriptSystem*             scripts = nullptr;

        static GameFlowProbe& Get()
        {
            static GameFlowProbe s_probe;
            return s_probe;
        }

        void Bind(Gameplay::GameFlowStateMachine* flowIn,
                  const SystemRegistry* systemsIn,
                  const ScriptSystem* scriptsIn)
        {
            flow = flowIn; systems = systemsIn; scripts = scriptsIn;
        }

        bool Bound() const { return flow && systems && scripts; }
    };

    struct GameFlowJsonOptions
    {
        /// Keep only the last N history records. 0 = every record the ring
        /// holds. When it truncates it says so in the object, because a
        /// silently short history reads as "nothing happened".
        std::size_t historyLimit = 0;
        /// Newlines and indentation. Off for the pipe, where the bytes are
        /// parsed and never read; on for the file, which a human opens.
        bool        pretty       = true;
    };

    /// One complete JSON object. Never throws; every string is escaped.
    std::string BuildGameFlowJson(const Gameplay::GameFlowStateMachine& flow,
                                  const SystemRegistry& systems,
                                  const ScriptSystem& scripts,
                                  const GameFlowJsonOptions& options = {});

    /// Write that object to @p utf8Path. Returns false on any I/O failure and never
    /// throws - it is reachable from a PostRender system, and the Scheduler's serial
    /// path has no try/catch.
    bool WriteGameFlowDump(const Gameplay::GameFlowStateMachine& flow,
                           const SystemRegistry& systems,
                           const ScriptSystem& scripts,
                           const std::string& utf8Path);

    /// Answer one `flow` control-channel command; returns the COMPLETE JSON reply
    /// object, ready for the pipe. The body lives here, not in ControlChannel.cpp,
    /// so the reply and the -dumpflow file both come out of BuildGameFlowJson;
    /// ControlChannel keeps the dispatch and the probe check.
    ///
    /// @p payload is everything after the verb, its trailing newline already
    /// stripped. Recognised: "" / "state" / "history" / "switch <name>".
    ///
    /// @p flow is NON-CONST because `switch` calls TryRequestSwitch. That adds no
    /// reach - `query` evaluates arbitrary Lua and GameFlow.Switch is on that
    /// surface; it makes the common case typo-proof and reports the refusal REASON,
    /// which a bool cannot.
    std::string ServeFlowVerb(Gameplay::GameFlowStateMachine& flow,
                              const SystemRegistry& systems,
                              const ScriptSystem& scripts,
                              const std::string& payload);

    /// Fold the end-of-run flow facts into run.json. Lives here rather than in
    /// RunLedger so the ledger keeps knowing nothing about Gameplay:: types,
    /// and App keeps a one-line call site at the point it already records
    /// framesRun.
    void NoteGameFlowToLedger(const Gameplay::GameFlowStateMachine& flow);
}
