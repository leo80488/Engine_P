#pragma once
// Harness/AssetTool.h - the offline asset tool: Game.exe / Editor.exe -assettool=<command>,
// answered before any window, device or world exists, then the process exits.
//
// One binary is the codec authority: the tool reads and writes .iscene files through
// Resource::SceneDocument, whose lines are parsed by the loader's own parsers - not a second
// implementation in Python. A mode of the engine's exe needs no extra project, link hooks or DLLs.
//
//   -assettool=inspect   -asset=<scene>                         nodes, components, W settings
//   -assettool=validate  -asset=<scene> [-assettool_refs=0]      findings; exit 1 on an error
//   -assettool=patch     -asset=<in> -ops=<ops.json> -out=<out> [-dry_run] [-expect_hash=<sha256>]
//   -assettool=roundtrip -asset=<scene or directory>             load+write == the file, per scene
//   -assettool=catalog   [-assettool_roots=asset;engine]         every asset: type, guid, header
//                        metadata, its references resolved (ok / guid_repair / missing + same-name
//                        candidates); duplicate guids, orphan .meta, unstamped
//   -assettool=meshlib_identity -asset=<.meshlib or directory> [-apply [-force]] [-iscn=<model.iscn>]
//                        the identity layer of each .meshlib: its Meshes/Sections and keys, or - for
//                        a file without one - the identity the loader synthesizes (grouped by the
//                        same-stem .iscn). -apply backfills it: writes exactly those keys into the
//                        file (atomic), reads them back, checks the geometry bytes are untouched.
//                        Refuses a damaged tail without -force and any file reached through a
//                        junction (another checkout's). One file lists every mesh and section; a
//                        directory lists counts.
//   -assettool=bake_primitives [-out=<dir>]                     the engine's Primitives model
//                        (Scene/PrimitiveAssets): the five Create > 3D Object shapes as
//                        engine/meshes/Primitives.meshlib + .iscn, deterministic; stamps a .meta
//                        for a file without one
//   -assettool=iscn_keys -asset=<.iscn or directory> [-apply]    the keys in the model files: F
//                        `sec=`, N `mkey=`, ME records, stale mesh= hints refreshed
//                        (SceneInstanceLoader::StampModelKeys). Dry run unless -apply; idempotent;
//                        never writes through a junction
//   -assettool=scene_keys -asset=<.iscene/.ipfb or directory> [-apply]  the keys in scenes and
//                        prefabs: `srcSec=` on MC/FS/MO/MR/MRI/MOI override lines, `meshSec=` on a
//                        collider's mesh (MC, `Collider:`), stale hints refreshed
//                        (Resource::StampSceneKeys). Same dry-run / junction rules
//   -assettool_out=<file.json>                                   where the JSON result goes
// Also: material_patch (-asset=<.imat> -ops=<ops.json holding {"set": {key: value}}>: the authored
// PBR fields only, same -out / -dry_run / -expect_hash rules), compat, skeleton_report, meshlets,
// collision_migrate, mc_echoes and matgraph (each at its section in AssetTool.cpp), ui_*
// (AssetToolUI.h), shadercook (AssetToolShaderCook.h), matdecl (AssetToolMatDecl.h) and upgrade
// (AssetToolUpgrade.h: files re-saved at their format's current version; the catalog lists each
// asset's format, version and status).
//
// patch ops (applied in order; any refusal writes nothing):
//   {"op":"set_transform", "node":<idx|name>, "position":[x,y,z], "rotation":[x,y,z,w], "scale":[x,y,z]}
//   {"op":"add_prefab_instance", "prefab":"<.ipfb>", "name":..., "position":..., "rotation":..., "scale":..., "parent":<idx|name>}
//   {"op":"set_component", "node":..., "tag":"LightData", "set":{"intensity":2, "color":[1,0.9,0.8]}}
//   {"op":"rename", "node":..., "name":...}
//   {"op":"remove_node", "node":...}
// then: validate (errors refuse), write <out> via a temp file + rename, read it back and check
// every op's intent. -expect_hash refuses when <in> changed.
//
// Exit codes: 0 done, 1 refused (validation errors, a bad op, a hash mismatch, a roundtrip
// difference), 2 usage / I/O. tools/asset_tool.py and the SDK (engine_tools.assets) are the
// callers.

namespace Harness
{
    // The process exit code, or -1 when -assettool is not on the command line.
    int RunAssetToolIfRequested();
}
