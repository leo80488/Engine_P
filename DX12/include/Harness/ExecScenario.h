#pragma once
// -exec=<file.lua>: load a scenario driver as a Lua System (App::BootScripting).
// The driver MUST `return <table>`; its callbacks get the instance as arg 1, so
// declare them with colon syntax (function M:OnUpdate(dt)). It must NOT declare
// `set = "..."`: a system in a systemSet the GameFlow has not entered is skipped
// silently. Refused without -artifacts= (quits with PreconditionFail), skipped
// when bootScriptsRefused. WITH_HARNESS only: compiled out of Retail.

class ScriptSystem;

namespace Harness
{
    void StartExecScenario(ScriptSystem& scriptSystem, bool bootScriptsRefused);
}
