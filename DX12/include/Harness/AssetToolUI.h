#pragma once
// Harness/AssetToolUI.h - the .iui verbs of the offline asset tool: Game.exe / Editor.exe
// -assettool=ui_<verb>, no window and no device, through the codec the engine spawns with
// (UI/UIAsset.h) and the layout kernel the game screen lays out with.
//
//   ui_inspect  -asset=<x.iui>                         the tree: id, name, depth, every block
//   ui_validate -asset=<x.iui> [-assettool_refs=0]     the codec's issues (the Cv* authoring
//                                                      contract included) and every referenced file;
//                                                      exit 1 on any
//   ui_fmt      -asset=<x.iui> [-ui_check]             rewrite in canonical form; -ui_check: exit 1
//                                                      when it is not (nothing written)
//   ui_patch    -asset=<x.iui> -ops=<ops.json> [-out=<file>] [-dry_run]
//               {"ops": [{"op": "set", "node": id, "block": "rect", "values": {"size": [640, 34]}},
//                        {"op": "unset", "node": id, "block": "text", "fields": ["color"]},
//                        {"op": "remove_block", "node": id, "block": "clip"},
//                        {"op": "add", "parent": id, "index": n?, "node": {...a node...}},
//                        {"op": "remove", "node": id},
//                        {"op": "move", "node": id, "parent": id, "index": n?},
//                        {"op": "rename", "node": id, "name": "..."},
//                        {"op": "order", "node": id, "order": n | null}]}
//               every op or none: the result must parse clean, then it is written canonical
//   ui_layout   -asset=<x.iui> [-ui_res=1280x720,1920x1080,3840x2160]
//               every node's rect per resolution and the problems (zero size, off screen,
//               outside its parent)
//   ui_convert  -asset=<x.ipfb> [-out=<x.iui>]         a UI prefab (root CvRect or CvCanvas) as a .iui
//
// The result JSON always has "ok"; exit 0 done, 1 refused, 2 usage / I/O.
#include <nlohmann/json_fwd.hpp>

#include <string>

namespace Harness
{
    // @p command is "ui_<verb>"; fills @p out, returns the exit code.
    int RunUIAssetTool(const std::string& command, nlohmann::json& out);
}
