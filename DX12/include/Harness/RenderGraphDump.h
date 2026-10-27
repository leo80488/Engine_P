#pragma once
// Harness/RenderGraphDump.h - a frame capture as JSON.
// RG::FrameCapture (RenderGraph/FrameCapture.h) holds the main graph's snapshot of one
// frame (passes, textures, declared edges, Phase A's barriers) plus the device's snapshot
// of the same frame (`device`: every command list with its name, queue, declared waits,
// submission index and batch; the batches; the barrier journal), the observed edges the
// journal implies with their class, the state mismatches, and the coverage numbers
// [RG_COVERAGE] prints.
// One builder, two consumers: -rgdump=<file>[@<frame>] writes the whole capture and the
// control channel's `rg` verb answers views of it, both from this code, so a field cannot
// be named one way in the file and another on the pipe.
// Names, not numbers: passes, textures, command lists and resources are written by name
// (indices beside them), RHI states by their RHI::ResourceState names, the journal's
// D3D12 states by their flag names AND value - the reader asks "who writes
// GBuffer_Depth", not an index table.
// A reply over kReplyBudget (under the pipe's 48 KB, ControlQuery::Limits) is written to
// <resultDir>/rg_<frame>_<view>.json instead and the reply names the file, as `run` does.

#include <string>

namespace RG { struct FrameCapture; }

namespace Harness
{
    /// The whole capture: the -rgdump file and `rg full`. Never throws.
    std::string BuildRenderGraphJson(const RG::FrameCapture& capture, bool pretty = true);

    /// Write that object to @p utf8Path (parent directories created). False on
    /// any I/O failure; never throws.
    bool WriteRenderGraphDump(const RG::FrameCapture& capture, const std::string& utf8Path);

    /// Answer one `rg` command and return the COMPLETE JSON reply. @p payload is
    /// everything after the verb: "" / "summary", "pass <name>",
    /// "resource <name>", "edges", "frame" (the device side: command lists,
    /// batches, observed edges, coverage), "journal [<resource>]" (the barrier
    /// journal, optionally one resource's), "full". @p resultDir receives the
    /// replies too large for the pipe.
    std::string ServeRgVerb(const RG::FrameCapture& capture, const std::string& payload,
                            const std::string& resultDir);
}
