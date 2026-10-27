#pragma once

// Harness/SettingsStoreScript.h - -settings_storetest=<ops.jsonl>: the settings store's
// layers, policies, validation and history, driven by a script against a store of its
// own - never the process's - so tests/unit/test_settings_store.py can state each
// operation and its expected answer side by side.
//
// One JSON object per line (blank lines and lines starting with "//" skipped):
//   {"op":"write", "path":P, "layer":L, "value":V, "writer":W}
//        V: a boolean, number, string, or {"nan":true}; coerced by the row's rules
//        (Settings::Coerce) before it is written
//   {"op":"clear", "path":P, "layer":L, "writer":W}
//   {"op":"where", "path":P}      value, owner (OwnerValue), source, layers, history
//   {"op":"counts"}
//   {"op":"policy", "path":P, "policy":"layered"|"lastWrite"|"capped"}
//        only before the store is built (the first write / clear / where / counts):
//        the store's rows are a copy of the player rows, and this gives one of them
//        another policy, so every policy's rules stay testable
//   {"op":"legacy", "path":P, "spec":S, "style":"pass"|"developer", "valueOnly":B}
//        a copy of engine.json row P with S as its legacy flag, read from this
//        process's command line (Settings::ReadLegacy)
//   {"op":"legacycheck", "path":P, "spec":S}   S's well-formedness
// L is a layer name ("player", "scene", "commandLine", ...). Each operation writes one
// result line: {"op":..., "ok":..., ...}; an unknown path, layer or policy is a result
// (ok false), an unknown op, a line that does not parse or a late policy op is a script
// error.

#include <string>

namespace Harness
{
    // False, with @p error, when @p opsPath cannot be read, @p outPath cannot be
    // written, or a line is not an operation.
    bool RunSettingsStoreScript(const std::string& opsPath, const std::string& outPath,
                                std::string& error);
}
