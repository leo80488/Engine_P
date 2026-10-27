#pragma once
// System/HotReloadReceipt.h - one record per hot reload, from "a file changed"
// to "the new version is in effect from frame N" or "it is not, and why".
//
// Every reload:
//   * logs ONE line tagged [HOTRELOAD] when it finishes (the pipe's `log` verb:
//     `log tag=[HOTRELOAD]`):
//        [HOTRELOAD] shader #12 ok: <message>
//        [HOTRELOAD] shader #13 compile_failed: <message>\n<first diagnostic>
//     INFO when it is in effect or not_in_use, WARNING for restart_required,
//     ERROR otherwise;
//   * is a job (Harness/ControlChannelJobs.h) of kind "hotreload.<kind>":
//     queued when the change is seen, running while it compiles, then
//     succeeded (output: files, counts, effectiveFrame, ...) or failed
//     (error: {code, message, log}, plus the same output). Without the control
//     channel (Retail) the id is a local sequence and only the line remains.
//
// effectiveFrame is the index of the first frame drawn (or simulated, for Lua)
// with the new version: the `frame` of frames.jsonl rows and of log records,
// and what Engine.Frame() returns during that frame. A capture (the pipe's
// `capture` verb) with afterFrame >= effectiveFrame shows it; 0 = nothing
// took effect.
//
// Codes (only "" is a success): "" (in effect), compile_failed (nothing
// changed: the old version stays), restart_required (took effect except where
// the receipt lists), errors_after_reload (it reloaded, and errors were logged
// while it took effect), not_in_use (nothing loaded uses the file: nothing to
// reload), lua_error, reload_error, load_failed (a texture's new file did not
// load: the previous contents stay), superseded (a newer write of the same
// file was reloaded instead).
//
// Kinds: shader, lua (scripts and behaviour trees), material (.imat /
// .imatinst), texture (.itex: re-read and swapped in place - same handle and
// bindless slot - by Resource::TextureSystem::RequestReload; output has width /
// height / bindless). A texture receipt is closed by the frame that swapped it in.

#include <cstdint>
#include <string>
#include <vector>

namespace HotReload
{
    struct Receipt
    {
        std::string              code;               // "" = the new version is in effect
        std::string              message;            // one line
        std::uint64_t            effectiveFrame = 0;
        std::uint32_t            ok     = 0;         // units that took the new version (shader recipes, scripts, entities)
        std::uint32_t            failed = 0;         // units that did not
        std::vector<std::string> errors;             // diagnostics; Close bounds them
        std::string              detailJson;         // a JSON object's text: extra output fields, or ""
    };

    // A change was seen: the receipt's id (its job id when the control channel
    // is compiled in). @p files are named the way a user would (root-relative).
    std::uint64_t Open(const char* kind, const std::vector<std::string>& files);
    // More files joined an open receipt - a shader batch collects every change
    // its debounce window sees - so output.files names each file it reloaded,
    // not only those of the first notification.
    void AddFiles(std::uint64_t id, const std::vector<std::string>& files);
    // The reload itself begins (the job goes from queued to running).
    void Start(std::uint64_t id);
    // Terminal: the [HOTRELOAD] line and the job's final state.
    void Close(std::uint64_t id, const Receipt& receipt);
}
