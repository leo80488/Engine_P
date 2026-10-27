// LuaConsoleWindow.cpp - the editor's Lua console backend: the Output Log's command line
// and the Tools > Lua Script Tools runner (cf. Unreal's Output Log command line).
//
// It runs a chunk against the LIVE Edit-mode world, so the mutation lands on the world the
// editor holds and Ctrl+S saves it like a hand edit. A script cannot do that itself:
// ScriptLogicSystem is SystemRunMask::Play, and Play -> Stop restores the snapshot. (The
// -control pipe also runs Lua in Edit mode outside Retail, but it is an opt-in launch flag
// for tools and agents.) There is no output pane of its own: the echo, the result and every
// line the chunk logs on the main thread are log records with source=console and the
// command's request id (Logger::ScopedContext), filterable in the Output Log.
//
// Two rules:
// 1. Never run a chunk inside Draw(). Editor draw code holds raw component pointers across
//    calls (the Inspector a MaterialComponent*, the Outliner walks Children), and a chunk
//    can reallocate a pool or fire destroy listeners mid-draw. Enter only QUEUES the text;
//    ProcessPendingLuaChunk drains it at the frame boundary, like the Outliner's
//    s_pendingReparent and ProcessPendingLoadScene. The expression-first probe compile
//    runs there too, so the Lua VM is never touched inside the ImGui pass.
// 2. It is NOT undoable: the undo stack is command-based (Editor/Undo/*) and a chunk
//    mutates the World without pushing an IEditAction. The command line says so.

#include "Editor/EditorLayer.h"
#include "Scripting/ScriptSystem.h"
#include "System/Check.h"
#include "System/CommandLine.h"
#include "System/Log.h"
#include "System/PathUtil.h"

#include "imgui/imgui.h"

#include <sol/sol.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace
{
    constexpr const char* kToolDir = "asset/scripts/editor";

    // Stringifies one Lua return value for the result line. Shallow on purpose: a table is
    // "<table>", not a recursive dump that could be megabytes and re-enter __index.
    std::string Describe(const sol::object& o)
    {
        switch (o.get_type())
        {
        case sol::type::nil:      return {};
        case sol::type::boolean:  return o.as<bool>() ? "true" : "false";
        case sol::type::string:   return o.as<std::string>();
        case sol::type::number:
        {
            const double d = o.as<double>();
            char b[64];
            if (d == std::floor(d) && std::abs(d) < 1e15) snprintf(b, sizeof(b), "%lld", (long long)d);
            else                                          snprintf(b, sizeof(b), "%g", d);
            return b;
        }
        case sol::type::table:    return "<table>";
        case sol::type::function: return "<function>";
        default:                  return "<userdata>";
        }
    }

    // One log line, however long or multi-line the command was.
    std::string OneLine(const std::string& s, std::size_t max)
    {
        std::string out;
        out.reserve((std::min)(s.size(), max) + 4);
        for (char c : s)
        {
            if (out.size() >= max) { out += "..."; break; }
            out.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
        }
        return out;
    }
}

// ==== Queue and submit ====
bool EditorLayer::QueueLuaChunk(std::string chunk, std::string label, std::uint32_t requestId,
                                bool tryExpression)
{
    if (chunk.empty()) return false;
    // One in flight: a second submission in the same frame is refused with a warning instead
    // of silently dropping the first.
    if (!m_pendingLuaChunk.empty())
    {
        Logger::ScopedContext ctx(LogSource::Console, requestId);
        LOG_WARNING("[CONSOLE] #%u ! busy - a command is already queued for this frame (%s)",
                    requestId, OneLine(label.empty() ? chunk : label, 120).c_str());
        return false;
    }
    m_pendingLuaChunk   = std::move(chunk);
    m_pendingLuaLabel   = std::move(label);
    m_pendingLuaRequest = requestId;
    m_pendingLuaExpr    = tryExpression;
    return true;
}

void EditorLayer::SubmitConsoleLine(const std::string& text, bool fromHarness)
{
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;
    const std::uint32_t id = Logger::NextRequestId();
    if (fromHarness)
        LOG_INFO("[CONSOLE] harness: submitted #%u at editor frame %llu: %s", id,
                 static_cast<unsigned long long>(m_editorFrame), OneLine(text, 160).c_str());
    // A typed line is tried as an EXPRESSION first, so `Engine.GetName(1)` prints its value;
    // the statement form is the fallback, so `for ... do` still works. Decided at the frame
    // boundary.
    QueueLuaChunk(text, std::string(), id, true);
}

void EditorLayer::RunLuaScriptTool(const std::string& relPath)
{
    // Read the file here instead of emitting `dofile(path)`: dofile resolves against the OS
    // working directory and turns a missing file into a Lua error, while the engine reads
    // assets through AssetFS; loading the text ourselves keeps the failure legible.
    // The tool runs as a plain chunk, not a System table: a `return {}` System needs the Play
    // tick loop. An editor tool does its work at top level.
    const std::uint32_t id = Logger::NextRequestId();
    std::ifstream f(Sys::U8Path(relPath), std::ios::binary);
    if (!f)
    {
        Logger::ScopedContext ctx(LogSource::Console, id);
        LOG_ERROR("[CONSOLE] #%u ! cannot open %s", id, relPath.c_str());
        return;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    QueueLuaChunk(ss.str(), relPath, id, false);
}

// Drained from ProcessPendingActions, OUTSIDE the ImGui frame (rule 1).
void EditorLayer::ProcessPendingLuaChunk()
{
    if (m_pendingLuaChunk.empty()) return;

    std::string chunk, label;
    chunk.swap(m_pendingLuaChunk);
    label.swap(m_pendingLuaLabel);
    const std::uint32_t id      = m_pendingLuaRequest;
    const bool          tryExpr = m_pendingLuaExpr;
    m_pendingLuaRequest = 0;
    m_pendingLuaExpr    = false;

    // Everything this command logs on this thread from here on (the echo, its print()s and
    // Log.* lines, the engine lines its calls produce, the result) carries source=console and
    // its id. -console_context_off (the editor_console pack's reverse arm) runs without the
    // scope.
    std::vector<CapturedLogLine> captured;
    std::optional<Logger::ScopedContext> scope;
    if (!m_consoleContextOff)
        scope.emplace(LogSource::Console, id, &captured, 256);

    const std::string shown = label.empty() ? OneLine(chunk, 200) : ("run " + label);
    LOG_INFO("[CONSOLE] #%u > %s", id, shown.c_str());
    captured.clear();   // the echo is not the command's output

    if (!m_scriptSys || !m_scriptSys->GetLua() || !m_world)
    {
        LOG_ERROR("[CONSOLE] #%u ! %s", id,
                  !m_scriptSys ? "no ScriptSystem attached"
                               : !m_scriptSys->GetLua() ? "Lua VM not initialised" : "no world");
        return;
    }
    sol::state& lua = *m_scriptSys->GetLua();

    // The chunk NAME is what a Lua error and a print()'s call site report:
    // `console#7:1: ...` / `Lua [console#7:1]: ...` for a typed command, the tool's path for a
    // script tool.
    const std::string chunkName = label.empty() ? ("console#" + std::to_string(id)) : label;
    std::string code = chunk;
    if (tryExpr)
    {
        const std::string expr = "return (" + chunk + "\n)";
        sol::load_result probe = lua.load(expr, "=" + chunkName);
        if (probe.valid()) code = expr;
    }

    const std::size_t before = m_world->GetEntities().size();

    // WorldBorrow points ScriptSystem::m_world at the persistently bound world for the call.
    // Without it every world-touching Engine.* binding is a guarded no-op: the chunk runs,
    // reports success and changes nothing.
    ScriptSystem::WorldBorrow borrow(*m_scriptSys);
    sol::protected_function_result r = lua.safe_script(code, sol::script_pass_on_error, "=" + chunkName);

    const std::size_t lines = captured.size();
    if (!r.valid())
    {
        const sol::error err = r;
        LOG_ERROR("[CONSOLE] #%u ! %s lines=%zu (%s)", id, err.what(), lines, shown.c_str());
        return;
    }

    std::string result = "ok";
    if (r.return_count() > 0)
    {
        const std::string d = Describe(r.get<sol::object>(0));
        if (!d.empty()) result = OneLine(d, 400);
    }
    const std::size_t after = m_world->GetEntities().size();
    char entities[80] = "";
    if (after != before)
    {
        std::snprintf(entities, sizeof(entities), " entities=%zu->%zu", before, after);
        // Only a change in the entity count marks the scene dirty, so a read-only query does not
        // make Ctrl+S think there is something to write. A chunk that only edits component VALUES
        // is missed and has to be saved deliberately - the safer direction to be wrong in.
        MarkSceneDirty();
    }
    LOG_INFO("[CONSOLE] #%u = %s lines=%zu%s (%s)", id, result.c_str(), lines, entities, shown.c_str());
}

// -console_exec=<editor frame>:<lua>, repeatable: submitted through
// SubmitConsoleLine - the same call the Enter key makes - on that editor frame.
void EditorLayer::TickHarnessConsole()
{
    for (const auto& e : m_harnessConsoleExecs)
        if (e.first == m_editorFrame)
            SubmitConsoleLine(e.second, true);
}

// ==== Lua Script Tools menu ====
void EditorLayer::ScanLuaTools()
{
    m_luaToolsScanned = true;
    m_luaTools.clear();
    std::error_code ec;
    const auto dir = Sys::U8Path(kToolDir);
    if (std::filesystem::exists(dir, ec))
        for (const auto& de : std::filesystem::directory_iterator(dir, ec))
            if (de.is_regular_file(ec) && de.path().extension() == ".lua")
                m_luaTools.push_back(Sys::PathToUtf8(de.path().filename()));
    std::sort(m_luaTools.begin(), m_luaTools.end());
}

void EditorLayer::RenderLuaToolsMenu()
{
    if (!ImGui::BeginMenu("Lua Script Tools")) return;
    if (!m_luaToolsScanned) ScanLuaTools();

    ImGui::TextDisabled("%s/*.lua - runs in the Edit world, no undo", kToolDir);
    ImGui::Separator();
    if (m_luaTools.empty())
        ImGui::TextDisabled("(none - drop a .lua in %s)", kToolDir);
    for (const std::string& t : m_luaTools)
        if (ImGui::MenuItem(t.c_str()))
            RunLuaScriptTool(std::string(kToolDir) + "/" + t);
    ImGui::Separator();
    if (ImGui::MenuItem("Refresh list")) m_luaToolsScanned = false;
    if (ImGui::MenuItem("Open the Output Log's command line"))
    {
        m_showOutputLog = true;
        m_outputLog.FocusCommandLine();
    }
    ImGui::EndMenu();
}
