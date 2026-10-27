#pragma once
// Boot scripts: manifest mode when the well-known manifest exists, the folder scan
// otherwise; exactly one of the two runs per boot.

class ScriptSystem;

namespace Scripting
{
    // Returns true when the boot was refused (PostQuitMessage(PreconditionFail) posted, no
    // chunk ran): nothing after it may execute a script.
    bool RunBootScripts(ScriptSystem& scriptSystem);
}
