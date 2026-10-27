#pragma once
// Harness/AssetToolUpgrade.h - Game.exe / Editor.exe -assettool=upgrade: asset files re-saved at
// their format's current version (Resource::kAssetFormats), through the engine's own codec.
//
//   upgrade -asset=<file|dir> [-apply]
//
// Dry run unless -apply. Every file with a version header under -asset (one file, or a directory
// walked without entering a junction) gets one row:
//   current      already at the current version of its format
//   would_upgrade / upgraded   older than current, with an upgrader (-apply writes it, atomically,
//                reads it back and requires the current version)
//   newer        written by a newer build than this one: left alone, a refusal
//   too_old      below the oldest version this engine still reads: a refusal
//   no_upgrader  older than current but the format has no upgrader here: a refusal
//   refused      the upgrader declined (why says so), or the file is reached through a junction
//   unreadable   not an asset header this engine knows
// Each container keeps every header field but the version (flags, reserved, resource type, the
// metadata's other words); a format whose migration changes the data is written through its
// writer and read back against the original (Resource::UpgradeMaterialBlob).
//
// Result JSON: ok, apply, files[{path, format, magic, from, to, state, why}], summary{count, current,
// upgraded, wouldUpgrade, refused, newer, tooOld, noUpgrader, unreadable, written}. Exit 0 when no
// row is a refusal; 1 otherwise; 2 usage / I/O.
#include <nlohmann/json_fwd.hpp>

namespace Harness
{
    // Fills @p out, returns the exit code.
    int RunUpgrade(nlohmann::json& out);
}
