#pragma once
// What the main thread is doing right now when it does something long. A stalled frame
// counter cannot tell "loading a 20-second scene" from "hung", so long work declares
// itself, and whatever still runs while the main thread is busy (the control pipe's
// `status` verb, on the pipe thread) reads the declaration:
//
//     Sys::Stage::Scope stage("loading_scene", path);    // until end of scope
//     Sys::Stage::Progress(done, total, "assets");       // optional, any time
//
// A Scope nests (the previous stage returns when it ends) and is main-thread only;
// Current() may be called from any thread. Times are Sys::Boot::NowMs() (ms since process
// creation). Compiled in every configuration: a few words behind a mutex, declared from
// engine code (Resource::LoadScene, Resource::SaveScene).

#include <cstdint>
#include <string>

namespace Sys::Stage
{
    class Scope
    {
    public:
        Scope(const char* name, std::string detail = {});
        ~Scope();
        Scope(const Scope&)            = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        std::uint64_t m_id = 0;
    };

    // Progress of the innermost stage; @p what names the unit ("assets",
    // "characters"). A no-op when no stage is active.
    void Progress(std::uint64_t done, std::uint64_t total, const char* what = nullptr);

    struct Snapshot
    {
        bool          active = false;
        std::string   name, detail, what;
        double        sinceMs = 0.0;          // Boot::NowMs() when the stage began
        double        lastProgressMs = 0.0;   // when Progress() last ran (0 = never)
        std::uint64_t done = 0, total = 0;
        std::uint32_t depth = 0;              // nesting depth (1 = outermost)
    };
    // The innermost active stage. Any thread.
    Snapshot Current();
}
