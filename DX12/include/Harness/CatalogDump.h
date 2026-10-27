#pragma once
// Harness/CatalogDump.h - the engine half of the generated catalog.
//
// tools/gen_catalog.py reads the source; this writes what only a running engine knows, because it
// is assembled at runtime and differs per host and configuration:
//
//   manifest.json    the exe that wrote it (path, configuration, SHA256, size), host, frame,
//                    scene, command line - a dump is only true for the binary named here
//   lua.json         every global the live Lua state holds: tables and their members
//                    (functions, constants, nested tables), usertypes
//   components.json  every ComponentSerializerRegistry tag: its C++ type, whether its serializer
//                    mutates (Entity.* reads those with a dedicated reader), how many entities of
//                    the loaded scene have it and the keys they serialize, and - in Editor.exe,
//                    the only host with reflection data - the reflected fields (kind, widget,
//                    range, enum options, label, info text)
//   systems.json     every tick phase in order, each system in execution order with its sets and
//                    its DeclareAccess reads / writes / exclusives; the Lua scripts' set
//                    assignments
//   passes.json      the main render graph's passes in execution order with the textures each
//                    reads / writes and whether the last frame ran it; every graph texture
//
// Written once by App at the control channel's serial point (-dump_catalog=<dir>); an -unattended
// run exits 0 afterwards. Read-only: nothing here changes the world, the Lua state or the graph.

#include <cstdint>
#include <string>

class World;
class ScriptSystem;
class SystemRegistry;
namespace RG { class RenderGraph; }

namespace Harness
{
    struct CatalogSources
    {
        World&                 world;
        ScriptSystem&          scripts;
        const SystemRegistry&  systems;
        const RG::RenderGraph& graph;
        std::uint64_t          frame = 0;
    };

    // Writes the five files into @p utf8Dir (created). False with @p error set
    // on the first failure. Never throws: it runs inside the frame loop.
    bool WriteCatalogDump(const CatalogSources& src, const std::string& utf8Dir, std::string& error);
}
